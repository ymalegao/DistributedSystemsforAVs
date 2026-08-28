#include "v2vbft/app/ResDBIntersectionApp.h"
#include "v2vbft/app/ResDBUtil.h"
#include "v2vbft/sinr/ChannelMetrics.h"
#include "v2vbft/messages/BFTMessage_m.h"
#include "veins/modules/mac/ieee80211p/Mac1609_4.h"
#include "veins/base/phyLayer/PhyToMacControlInfo.h"
#include "veins/modules/phy/DeciderResult80211.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "v2vbft/app/ResdbV2VWire.h"
#include "v2vbft/app/ResDBDecisionGossip.h"

using namespace veins;
using namespace v2vbft;
using namespace v2vbft::resdb_app_util;

namespace {
int bftQuorumSize(int n, int f)
{
    if (n <= 0 || f < 0 || f > (n - 1) / 3) return -1;
    return std::max((n + f + 2) / 2, 1);
}

std::mutex g_completed_replica_epochs_mu;
std::set<std::pair<int, uint32_t>> g_completed_replica_epochs;

}  // namespace

Define_Module(v2vbft::ResDBIntersectionApp);

bool ResDBIntersectionApp::hasCompletedReplicaEpoch(int replicaId, uint32_t epoch)
{
    std::lock_guard<std::mutex> lk(g_completed_replica_epochs_mu);
    return g_completed_replica_epochs.count({replicaId, epoch}) > 0;
}

void ResDBIntersectionApp::markCompletedReplicaEpoch(int replicaId, uint32_t epoch)
{
    std::lock_guard<std::mutex> lk(g_completed_replica_epochs_mu);
    g_completed_replica_epochs.insert({replicaId, epoch});
}

int ResDBIntersectionApp::toleratedF() const
{
    // Explicit experiment override wins; otherwise derive from PBFT membership N
    // (num_replicas_ = vehicles + static intersection units) so f grows as units join.
    return ctx_.tolerated_faults_ >= 0 ? ctx_.tolerated_faults_ : (num_replicas_ - 1) / 3;
}

std::vector<int> ResDBIntersectionApp::staticUnitReplicaIds() const
{
    // Units are the top num_units_ replica IDs: [num_replicas_ - num_units_, num_replicas_).
    // This is robust when ctx_.total_vehicles_ (the cert/QUIET-fill count, e.g. 16 in the
    // rollback scenario) is less than the real provisioned vehicle count (18).
    std::vector<int> ids;
    for (int rid = num_replicas_ - num_units_; rid < num_replicas_; ++rid)
        if (rid >= 0) ids.push_back(rid);
    return ids;
}

bool ResDBIntersectionApp::isStaticUnitReplica(int replicaId) const
{
    return num_units_ > 0 &&
           replicaId >= num_replicas_ - num_units_ && replicaId < num_replicas_;
}

// ── Destructor ────────────────────────────────────────────────────────────────

ResDBIntersectionApp::~ResDBIntersectionApp()
{
    // Dynamic TraCI module deletion runs under the scenario manager's context.
    // cancelEvent() therefore returns a scheduled self-message to the manager,
    // not necessarily to this app.  Deleting that manager-owned object from
    // our destructor triggers OMNeT++'s object-stealing runtime error.  Retake
    // every timer after cancellation before deleting it; this is also safe for
    // ordinary end-of-simulation teardown where this app already owns it.
    auto deleteTimer = [this](cMessage*& msg) {
        if (!msg) return;
        if (msg->isScheduled()) cancelEvent(msg);
        if (msg->getOwner() != this) take(msg);
        delete msg;
        msg = nullptr;
    };
    deleteTimer(smoke_test_msg_);
    deleteTimer(transport_poll_msg_);
    deleteTimer(time_tick_msg_);
    deleteTimer(discovery_deadline_msg_);
    deleteTimer(discovery_settle_msg_);
    deleteTimer(cert_retry_timer_);
    deleteTimer(cert_gossip_timer_);
    deleteTimer(gossip_timer_);
    deleteTimer(discovery_tx_flush_timer_);
    deleteTimer(initial_announce_msg_);
    deleteTimer(stop_sign_timeout_msg_);
    deleteTimer(consensus_timeout_msg_);
    deleteTimer(resume_msg_);
    deleteTimer(cancel_cert_retry_timer_);
    deleteTimer(clear_cert_retry_timer_);
    deleteTimer(clear_cert_candidate_timer_);
    deleteTimer(clear_cert_relay_timer_);
    deleteTimer(wait_leader_send_timer_);
    deleteTimer(wait_follower_expiry_timer_);
    deleteTimer(cancel_drain_timer_);
    deleteTimer(consensus_retry_timer_);
    deleteTimer(consensus_relay_timer_);
    deleteTimer(cancel_vc_timer_);
    deleteTimer(cancel_gossip_timer_);
    deleteTimer(preceding_batch_poll_msg_);
    deleteTimer(crash_mac_grace_msg_);
    deleteTimer(broadcastArrivalAnnouncement_timer_);
    deleteTimer(channel_metrics_timer_);
    if (channel_metrics_) {
        cModule* nic = getParentModule() ? getParentModule()->getSubmodule("nic") : nullptr;
        cModule* mac = nic ? nic->getSubmodule("mac1609_4") : nullptr;
        if (mac && mac->isSubscribed(Mac1609_4::sigChannelBusy, channel_metrics_))
            mac->unsubscribe(Mac1609_4::sigChannelBusy, channel_metrics_);
        if (mac && mac->isSubscribed(Mac1609_4::sigCollision, channel_metrics_))
            mac->unsubscribe(Mac1609_4::sigCollision, channel_metrics_);
        delete channel_metrics_;
        channel_metrics_ = nullptr;
    }
    if (ctx_.resdb_server_handle_) {
        ResdbOmnetDestroyServer(ctx_.resdb_server_handle_);
        ctx_.resdb_server_handle_ = nullptr;
    }
    if (ctx_.ec_private_key_) {
        EVP_PKEY_free(ctx_.ec_private_key_);
        ctx_.ec_private_key_ = nullptr;
    }
    if (ambulance_private_key_) {
        EVP_PKEY_free(ambulance_private_key_);
        ambulance_private_key_ = nullptr;
    }
}

// ── initialize ────────────────────────────────────────────────────────────────

void ResDBIntersectionApp::initialize(int stage)
{
    DemoBaseApplLayer::initialize(stage);

    if (stage == 0) {
        stop_distance_         = par("stopDistance").doubleValue();
        //stop distance formula = stop distance * (total vehicles / 2)
        ctx_.total_vehicles_        = par("totalVehicles").intValue();
        stop_distance_ = stop_distance_ * (ctx_.total_vehicles_ / 2);

        // PBFT membership N (vehicles + static intersection units). -1 → = vehicles.
        is_intersection_unit_ = par("isIntersectionUnit").boolValue();
        num_replicas_         = par("totalReplicas").intValue();
        if (num_replicas_ < 0) num_replicas_ = ctx_.total_vehicles_;
        num_units_            = par("intersectionUnitCount").intValue();

        ctx_.replicaId_ = par("replicaId").intValue();
        const int ned_replica_id = ctx_.replicaId_;
        // Replica id must follow the SUMO vehicle this host
        // controls (mobility externalId), not OMNeT node index. Otherwise TraCI
        // uses veh<NED> while the module actually drives veh<externalId> — wrong
        // lane/position in proposals and crashes despite a correct OrderDecision.
        if (mobility) {
            try {
                const std::string sumoId = mobility->getExternalId();
                if (sumoId.size() > 3 && sumoId.compare(0, 3, "veh") == 0) {
                    const int from_sumo = std::stoi(sumoId.substr(3));
                    if (from_sumo >= 0 && from_sumo < ctx_.total_vehicles_) {
                        if (from_sumo != ned_replica_id) {
                            std::cout << "[IDENTITY BINDING] NED replicaId=" << ned_replica_id
                                      << " overridden by SUMO externalId=" << sumoId
                                      << " -> ctx_.replicaId_=" << from_sumo << "\n";
                        }
                        ctx_.replicaId_ = from_sumo;
                    }
                }
            } catch (const std::exception& e) {
                std::cerr << "[IDENTITY BINDING] keep NED replicaId=" << ctx_.replicaId_
                          << " (SUMO bind failed: " << e.what() << ")\n";
            } catch (...) {
                std::cerr << "[IDENTITY BINDING] keep NED replicaId=" << ctx_.replicaId_
                          << " (SUMO bind failed)\n";
            }
        }

        useRadioTransport_        = par("useRadioTransport").boolValue();
        transport_poll_interval_  = par("transportPollInterval").doubleValue();
        enable_sim_time_provider_ = par("enableSimTimeProvider").boolValue();
        time_tick_interval_       = par("timeTickInterval").doubleValue();
        broadcast_arrival_announcement_interval_ = par("broadcastArrivalAnnouncementIntervalSec").doubleValue();
        announce_max_interval_ = par("broadcastArrivalAnnouncementMaxIntervalSec").doubleValue();
        announce_backoff_ = broadcast_arrival_announcement_interval_;
        const double cert_timeout_base_sec = par("certCollectionTimeoutSec").doubleValue();
        const double cert_timeout_scale_sec = std::floor(static_cast<double>(ctx_.total_vehicles_) / 5.0);
        cert_collection_timeout_ = SimTime(cert_timeout_base_sec + cert_timeout_scale_sec);
        discovery_intent_settle_ = par("discoveryIntentSettleSec").doubleValue();

        const double view_change_timeout_sec = par("pbftVcTimeoutSec").doubleValue();
        const double view_change_timeout_scale_sec = std::floor(static_cast<double>(ctx_.total_vehicles_) / 5.0);
        pbft_vc_timeout_sec_ = (view_change_timeout_sec + view_change_timeout_scale_sec);

        debug_cert_protocol_    = par("debugCertProtocol").boolValue();
        debug_order_delivery_ = par("debugOrderDelivery").boolValue();
        enable_cert_retries_    = par("enableArrivalCertRetries").boolValue();
        cert_retry_interval_    = par("arrivalCertRetryIntervalSec").doubleValue();
        cert_retry_max_         = par("arrivalCertRetryMax").intValue();
        ctx_.tolerated_faults_       = par("toleratedFaults").intValue();
        configured_consensus_quorum_ = ctx_.tolerated_faults_ >= 0
            ? bftQuorumSize(num_replicas_, ctx_.tolerated_faults_)
            : -1;
        configured_cert_threshold_ = ctx_.tolerated_faults_ >= 0
            ? (ctx_.tolerated_faults_ + 1)
            : -1;
        if (ctx_.tolerated_faults_ >= 0) {
            std::cout << "[TOLERATED-F-APP] r" << ctx_.replicaId_
                      << " tolerated_f=" << ctx_.tolerated_faults_
                      << " static_n=" << num_replicas_
                      << " vehicles=" << ctx_.total_vehicles_
                      << " quorum=" << configured_consensus_quorum_
                      << " cert_threshold=" << configured_cert_threshold_;
            std::cout << "\n";
        }

        ctx_.gossip_enabled_          = par("enableDecisionGossip").boolValue();
        gossip_initial_interval_ = par("decisionGossipInitialIntervalSec").doubleValue();
        gossip_max_retries_      = par("decisionGossipMaxRetries").intValue();

        cruise_speed_mps_      = par("cruiseSpeedMps").doubleValue();
        is_ambulance_          = par("isAmbulance").boolValue();
        trigger_join_time_     = par("triggerJoinTimeSec").doubleValue();
        arrival_slot_sec_      = par("arrivalSlotSec").doubleValue();
        stop_sign_timeout_sec_ = par("stopSignTimeoutSec").doubleValue();
        consensus_timeout_sec_ = par("consensusTimeoutSec").doubleValue();
        preceding_batch_poll_period_sec_ = par("precedingBatchPollPeriodSec").doubleValue();
        clearance_timeout_sec_     = par("clearanceTimeoutSec").doubleValue();
        intended_direction_    = par("intendedDirection").stdstringValue();
        intended_lane_         = par("intendedLane").stdstringValue();
        is_byzantine_            = par("isByzantine").boolValue();
        byzantine_type_          = static_cast<ByzantineType>(par("byzantineType").intValue());
        byzantine_pbft_silent_   = par("byzantinePbftSilent").boolValue();
        enableAmbulanceCertGate_ = par("enableAmbulanceCertGate").boolValue();
        enable_arrival_position_gate_ = par("enableArrivalPositionGate").boolValue();
        for (const auto& token : splitStr(par("falseLaneColluderIds").stdstringValue(), ',')) {
            if (token.empty()) continue;
            try {
                const int id = std::stoi(token);
                if (id >= 0 && id < ctx_.total_vehicles_) false_lane_colluder_ids_.insert(id);
            } catch (...) {
                std::cout << "[FALSE-LANE-COLLUDER-SET] r" << ctx_.replicaId_
                          << " ignored_invalid_token=" << token << "\n";
            }
        }
        std::cout << "[FALSE-LANE-COLLUDER-SET] r" << ctx_.replicaId_
                  << " N=" << ctx_.total_vehicles_
                  << " F=" << false_lane_colluder_ids_.size()
                  << " ids=";
        bool firstColluder = true;
        for (int id : false_lane_colluder_ids_) {
            if (!firstColluder) std::cout << ",";
            std::cout << id;
            firstColluder = false;
        }
        std::cout << " position_gate=" << (enable_arrival_position_gate_ ? 1 : 0)
                  << "\n";
        ctx_.enableRollback_ = par("enableRollback").boolValue();
        crash_mac_grace_sec_ = par("crashMacGraceSec").doubleValue();
        crash_dwell_sec_ = par("crashDwellSec").doubleValue();
        crash_speed_eps_ = par("crashSpeedEps").doubleValue();
        clear_dwell_sec_ = par("clearDwellSec").doubleValue();
        clear_cert_candidate_slot_sec_ = par("clearCertCandidateSlotSec").doubleValue();
        wait_heartbeat_interval_sec_ = par("waitHeartbeatIntervalSec").doubleValue();
        wait_heartbeat_max_deferral_sec_ = par("waitHeartbeatMaxDeferralSec").doubleValue();
        wait_clock_skew_sec_ = par("waitClockSkewSec").doubleValue();
        cancel_cert_retry_interval_sec_ = par("cancelCertRetryIntervalSec").doubleValue();
        evidence_retry_base_sec_ = par("evidenceRetryBaseSec").doubleValue();
        evidence_retry_factor_ = par("evidenceRetryFactor").doubleValue();
        evidence_retry_cap_sec_ = par("evidenceRetryCapSec").doubleValue();
        cancel_gossip_retry_base_sec_ = par("cancelGossipRetryBaseSec").doubleValue();
        cancel_gossip_retry_cap_sec_ = par("cancelGossipRetryCapSec").doubleValue();
        cancel_cert_retry_max_ = par("cancelCertRetryMax").intValue();
        consensus_retry_interval_sec_ = par("consensusRetryIntervalSec").doubleValue();
        consensus_retry_max_ = par("consensusRetryMax").intValue();
        consensus_relay_carrier_cap_ =
            std::max(1, static_cast<int>(
                par("type11RelayCarrierCap").intValue()));
        consensus_relay_base_delay_sec_ =
            std::max(0.0, par("type11RelayBaseDelaySec").doubleValue());
        consensus_relay_slot_sec_ =
            std::max(0.0, par("type11RelaySlotSec").doubleValue());
        std::cout << "[TYPE11-CONFIG] r" << ctx_.replicaId_
                  << " carrier_cap=" << consensus_relay_carrier_cap_
                  << " carrier_target=" << consensusRelayCarrierThreshold()
                  << " base_delay=" << consensus_relay_base_delay_sec_
                  << " slot=" << consensus_relay_slot_sec_ << "\n";
        braking_decel_mps2_ = par("brakingDecelMps2").doubleValue();
        processing_latency_margin_ = par("processingLatencyMargin").doubleValue();
        rollback_vc_timeout_sec_ = par("rollbackVcTimeoutSec").doubleValue();
        inject_suppress_initial_cancel_leader_ =
            par("injectSuppressInitialCancelLeader").boolValue();
        enable_cancel_leader_failover_ =
            par("enableCancelLeaderFailover").boolValue();
        inject_fabricated_clearance_leader_ =
            par("injectFabricatedClearanceLeader").boolValue();
        enable_recovery_clear_evidence_gate_ =
            par("enableRecoveryClearEvidenceGate").boolValue();
        std::cout << "[CANCEL-LEADER-CONFIG] r" << ctx_.replicaId_
                  << " inject_suppress_initial="
                  << (inject_suppress_initial_cancel_leader_ ? 1 : 0)
                  << " failover=" << (enable_cancel_leader_failover_ ? 1 : 0)
                  << " timeout=" << rollback_vc_timeout_sec_ << "\n";
        std::cout << "[FABRICATED-CLEARANCE-CONFIG] r" << ctx_.replicaId_
                  << " inject="
                  << (inject_fabricated_clearance_leader_ ? 1 : 0)
                  << " evidence_gate="
                  << (enable_recovery_clear_evidence_gate_ ? 1 : 0)
                  << "\n";
        {
            std::string rollbackMode = par("rollbackFaultMode").stdstringValue();
            std::transform(rollbackMode.begin(), rollbackMode.end(),
                           rollbackMode.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            rollback_fault_mode_per_epoch_ = (rollbackMode != "anchored");
            std::cout << "[ROLLBACK-MODE] r" << ctx_.replicaId_
                      << " mode="
                      << (rollback_fault_mode_per_epoch_ ? "per_epoch" : "anchored")
                      << "\n";
        }

        const int ambulance_replica_id = par("ambulanceReplicaId").intValue();
        if (ambulance_replica_id >= 0) {
            is_ambulance_ = (ctx_.replicaId_ == ambulance_replica_id);

            std::cout << "[AMBULANCE BINDING] r" << ctx_.replicaId_
                      << " ambulanceReplicaId=" << ambulance_replica_id
                      << " -> isAmbulance=" << (is_ambulance_ ? 1 : 0) << "\n";
        }

        moduleIsAmbulance = is_ambulance_;

        if (is_ambulance_) {
            if (ambulance_private_key_) {
                EVP_PKEY_free(ambulance_private_key_);
                ambulance_private_key_ = nullptr;
            }
            my_ambulance_cert_bytes_.clear();
            uint8_t amb_pub[CRYPTO_PUBKEY_BYTES];
            ambulance_private_key_ = CryptoAuth::instance().generateKeyPair(amb_pub);
            VehicleCert cert = CryptoAuth::instance().issueCert(
                amb_pub, "ambulance", "Emergency_CA");
            my_ambulance_cert_bytes_.assign(
                reinterpret_cast<const uint8_t*>(&cert),
                reinterpret_cast<const uint8_t*>(&cert) + sizeof(VehicleCert));
            std::cout << "[AMBULANCE] r" << ctx_.replicaId_
                      << " auto-issued Emergency_CA cert ("
                      << my_ambulance_cert_bytes_.size() << " bytes)\n";
        }

        std::string crypto_dir = par("resdbCryptoDir").stdstringValue();
        config_file_      = par("resdbConfigFile").stdstringValue();
        private_key_file_ = par("resdbPrivateKeyFile").stdstringValue();
        cert_file_        = par("resdbCertFile").stdstringValue();
        log_dir_          = par("resdbLogDir").stdstringValue();
        if (!crypto_dir.empty() && config_file_.empty()) {
            int node_id = ctx_.replicaId_ + 1;
            config_file_      = crypto_dir + "/server.config";
            private_key_file_ = crypto_dir + "/node" + std::to_string(node_id) + ".key.pri";
            cert_file_        = crypto_dir + "/cert_"  + std::to_string(node_id) + ".cert";
            log_dir_          = crypto_dir + "/logs";
        }

        ctx_.ec_private_key_ = CryptoAuth::instance().generateKeyPair(ctx_.ec_pub_key_);
        // WitnessKeyRegistry is a process-global singleton; the orchestrator runs
        // each repetition as its own subprocess (experiment_orchestrator.py calls
        // subprocess.run per rep), so the registry always starts empty here — no
        // explicit reset needed (resetForNewRun() remains available for a future
        // in-process multi-run harness).
        if (!WitnessKeyRegistry::instance().registerKey(ctx_.replicaId_, ctx_.ec_pub_key_)) {
            std::cout << "[WITNESS-KEY-CONFLICT] r" << ctx_.replicaId_
                      << " registration rejected — stale registry from a prior run?\n";
        } else {
            std::cout << "[WITNESS-KEY-REG] r" << ctx_.replicaId_ << " registered\n";
        }

        if (useRadioTransport_) {
            transport_ = std::make_unique<VeinsTransport>(this);
            transport_poll_msg_ = new cMessage("resdbTransportPoll");
            scheduleAt(simTime() + transport_poll_interval_, transport_poll_msg_);
        } else {
            transport_ = std::make_unique<LoggingTransport>(ctx_.replicaId_);
        }

        if (config_file_.empty()) {
            ctx_.resdb_server_handle_ = ResdbOmnetCreateNullHandle();
        } else {
            ctx_.resdb_server_handle_ = ResdbOmnetCreateKvServer(
                &config_file_[0],
                private_key_file_.empty() ? nullptr : &private_key_file_[0],
                cert_file_.empty()        ? nullptr : &cert_file_[0],
                log_dir_.empty()          ? nullptr : &log_dir_[0]);
            if (!ctx_.resdb_server_handle_)
                ctx_.resdb_server_handle_ = ResdbOmnetCreateNullHandle();
        }

        if (ctx_.tolerated_faults_ >= 0) {
            int rc = ResdbOmnetSetToleratedFaults(
                ctx_.resdb_server_handle_, ctx_.tolerated_faults_);
            std::cout << "[TOLERATED-F-APP] r" << ctx_.replicaId_
                      << " bridge_set_rc=" << rc << "\n";
        }
        {
            int rc = ResdbOmnetSetRollbackFaultMode(
                ctx_.resdb_server_handle_, rollback_fault_mode_per_epoch_ ? 1 : 0);
            if (rc != 0) {
                std::cout << "[ROLLBACK-MODE] r" << ctx_.replicaId_
                          << " bridge_set_rc=" << rc << "\n";
            }
        }

        registerTransport();
        ResdbOmnetSetOrderCallback(ctx_.resdb_server_handle_,
                                   &ResDBIntersectionApp::onOrderDecided, this);
        ResdbOmnetSetClearEvidenceCallback(ctx_.resdb_server_handle_,
                                           &ResDBIntersectionApp::clearEvidenceCallback, this);
        ResdbOmnetSetCertSnapshotFn(ctx_.resdb_server_handle_,
                                    &ResDBIntersectionApp::certSnapshotCallback, this);
        ResdbOmnetSetVcTimeoutUs(ctx_.resdb_server_handle_,
                                 static_cast<int64_t>(pbft_vc_timeout_sec_ * 1e6));
        last_known_primary_ = ResdbOmnetGetPrimary(ctx_.resdb_server_handle_);
        std::cout << "[VC-INIT] r" << ctx_.replicaId_
                  << " vc_timeout_sec=" << pbft_vc_timeout_sec_
                  << " stop_sign_timeout_sec=" << stop_sign_timeout_sec_
                  << " consensus_timeout_sec=" << consensus_timeout_sec_
                  << " initial_primary=" << last_known_primary_ << "\n";

        if (enable_sim_time_provider_) {
            time_tick_msg_ = new cMessage("resdbTimeTick");
            scheduleAt(simTime() + time_tick_interval_, time_tick_msg_);
            ResdbOmnetUpdateSimTimeUs(ctx_.resdb_server_handle_, simTime().inUnit(SIMTIME_US));
        }

        ResdbOmnetRunServer(ctx_.resdb_server_handle_);

        // For PBFT omission faults: silence the communicator so this replica drops
        // outbound PRE_PREPARE/PREPARE/COMMIT traffic. SILENT_PRIMARY keeps its
        // legacy behavior; byzantinePbftSilent lets randomized followers be quiet too.
        if (is_byzantine_ &&
                (byzantine_type_ == BYZANTINE_SILENT_PRIMARY || byzantine_pbft_silent_)) {
            ResdbOmnetSetPbftSilent(ctx_.resdb_server_handle_, 1);
            std::cout << "[BYZANTINE] r" << ctx_.replicaId_
                      << " PBFT_SILENT: communicator silenced"
                      << " type=" << static_cast<int>(byzantine_type_)
                      << " explicit=" << (byzantine_pbft_silent_ ? 1 : 0)
                      << "\n";
        }

        discovery_deadline_msg_ = new cMessage("resdbDiscoveryDeadline");
        discovery_settle_msg_ = new cMessage("resdbDiscoverySettle");
        consensus_retry_timer_ = new cMessage("resdbConsensusRetry");
        consensus_relay_timer_ = new cMessage("resdbConsensusRelay");
    }

    if (stage == 1) {
        startDiscoveryRound("initial-approach");
        if (par("smokeTestBroadcast").boolValue()) {
            smoke_test_msg_ = new cMessage("resdbSmokeTest");
            scheduleAt(simTime() + 0.05, smoke_test_msg_);
        }
        // Static intersection units never announce their own arrival (they have no
        // arrival to announce). They still receive/echo/relay/vote/execute via the
        // transport-poll and onWSM paths armed in stage 0.
        if (!is_intersection_unit_) {
            // Staggered one-shot arrival announcement.
            initial_announce_msg_ = new cMessage("resdbInitialAnnounce");
            scheduleAt(simTime() + trigger_join_time_ + ctx_.replicaId_ * arrival_slot_sec_,
                       initial_announce_msg_);

            // // Periodic re-announce fallback (in case peers miss the initial broadcast).
            // state_announce_msg_ = new cMessage("resdbStateAnnounce");
            // scheduleAt(simTime() + state_announce_interval_, state_announce_msg_);

            broadcastArrivalAnnouncement_timer_ = new cMessage("resdbBroadcastArrivalAnnouncement");
            scheduleAt(simTime() + broadcast_arrival_announcement_interval_, broadcastArrivalAnnouncement_timer_);
        }

        // Metrics apply to every instantiated vehicle, including dynamic
        // recovery members.  ctx_.total_vehicles_ is the epoch-0 protocol boundary
        // in Scenario 15 (16), not the physical run size (18), so using it here
        // silently excluded late r16/r17 from channel and SINR collection.
        if (par("enableChannelMetricsCsv").boolValue()) {
            std::string dir = par("channelMetricsCsvDir").stdstringValue();
            if (dir.empty())
                dir = log_dir_;
            if (!dir.empty() && dir.back() != '/')
                dir += '/';
            if (dir.empty())
                dir = "./";
            std::string csvPath  = dir + "channel_V" + std::to_string(ctx_.replicaId_) + ".csv";
            std::string sinrPath = dir + "sinr_V" + std::to_string(ctx_.replicaId_) + ".csv";
            channel_metrics_ = new ChannelMetrics(ctx_.replicaId_, csvPath, sinrPath);
            cModule* nic = getParentModule()->getSubmodule("nic");
            cModule* mac = nic ? nic->getSubmodule("mac1609_4") : nullptr;
            if (mac) {
                mac->subscribe(Mac1609_4::sigChannelBusy, channel_metrics_);
                mac->subscribe(Mac1609_4::sigCollision, channel_metrics_);
            } else
                std::cerr << "[ChannelMetrics] r" << ctx_.replicaId_ << " no nic/mac1609_4 — utilization stays 0\n";
            channel_metrics_timer_ = new cMessage("channelMetricsTick");
            scheduleAt(simTime() + 0.1, channel_metrics_timer_);
        }
    }
}

// ── handleSelfMsg ─────────────────────────────────────────────────────────────

// ── Self-message dispatch ────────────────────────────────────────────────────
// One row per timer, in the order the previous if-chain tested them.
//
// Keyed on the MEMBER POINTER, not on a cached cMessage* value: several
// handlers delete their message and null their slot, so a table keyed on the
// pointer would keep a dangling key that a later allocation could reuse and
// silently match the wrong timer. Reading the member each time reproduces the
// old `msg == some_timer_` comparison exactly.
bool ResDBIntersectionApp::dispatchTimer(cMessage* msg)
{
    using Slot = cMessage* ResDBIntersectionApp::*;
    using Handler = void (ResDBIntersectionApp::*)(cMessage*);
    static const struct { Slot slot; Handler handler; } kTimerTable[] = {
        {&ResDBIntersectionApp::crash_mac_grace_msg_, &ResDBIntersectionApp::onCrashMacGrace},
        {&ResDBIntersectionApp::channel_metrics_timer_, &ResDBIntersectionApp::onChannelMetrics},
        {&ResDBIntersectionApp::smoke_test_msg_, &ResDBIntersectionApp::onSmokeTest},
        {&ResDBIntersectionApp::transport_poll_msg_, &ResDBIntersectionApp::onTransportPoll},
        {&ResDBIntersectionApp::time_tick_msg_, &ResDBIntersectionApp::onTimeTick},
        {&ResDBIntersectionApp::consensus_retry_timer_, &ResDBIntersectionApp::onConsensusRetry},
        {&ResDBIntersectionApp::consensus_relay_timer_, &ResDBIntersectionApp::onConsensusRelay},
        {&ResDBIntersectionApp::cancel_drain_timer_, &ResDBIntersectionApp::onCancelDrain},
        {&ResDBIntersectionApp::initial_announce_msg_, &ResDBIntersectionApp::onInitialAnnounce},
        {&ResDBIntersectionApp::broadcastArrivalAnnouncement_timer_, &ResDBIntersectionApp::onBroadcastArrivalAnnouncement},
        {&ResDBIntersectionApp::cert_retry_timer_, &ResDBIntersectionApp::onCertRetry},
        {&ResDBIntersectionApp::discovery_tx_flush_timer_, &ResDBIntersectionApp::onDiscoveryTxFlush},
        {&ResDBIntersectionApp::cert_gossip_timer_, &ResDBIntersectionApp::onCertGossip},
        {&ResDBIntersectionApp::gossip_timer_, &ResDBIntersectionApp::onGossip},
        {&ResDBIntersectionApp::cancel_gossip_timer_, &ResDBIntersectionApp::onCancelGossip},
        {&ResDBIntersectionApp::cancel_cert_retry_timer_, &ResDBIntersectionApp::onCancelCertRetry},
        {&ResDBIntersectionApp::clear_cert_retry_timer_, &ResDBIntersectionApp::onClearCertRetry},
        {&ResDBIntersectionApp::clear_cert_candidate_timer_, &ResDBIntersectionApp::onClearCertCandidate},
        {&ResDBIntersectionApp::clear_cert_relay_timer_, &ResDBIntersectionApp::onClearCertRelay},
        {&ResDBIntersectionApp::wait_leader_send_timer_, &ResDBIntersectionApp::onWaitLeaderSend},
        {&ResDBIntersectionApp::wait_follower_expiry_timer_, &ResDBIntersectionApp::onWaitFollowerExpiry},
        {&ResDBIntersectionApp::discovery_deadline_msg_, &ResDBIntersectionApp::onDiscoveryDeadline},
        {&ResDBIntersectionApp::discovery_settle_msg_, &ResDBIntersectionApp::onDiscoverySettle},
        {&ResDBIntersectionApp::vc_trigger_msg_, &ResDBIntersectionApp::onVcTrigger},
        {&ResDBIntersectionApp::cancel_vc_timer_, &ResDBIntersectionApp::onCancelVc},
        {&ResDBIntersectionApp::stop_sign_timeout_msg_, &ResDBIntersectionApp::onStopSignTimeout},
        {&ResDBIntersectionApp::consensus_timeout_msg_, &ResDBIntersectionApp::onConsensusTimeout},
        {&ResDBIntersectionApp::resume_msg_, &ResDBIntersectionApp::onResume},
        {&ResDBIntersectionApp::preceding_batch_poll_msg_, &ResDBIntersectionApp::onPrecedingBatchPoll},
    };

    for (const auto& binding : kTimerTable) {
        if (msg == this->*(binding.slot)) {
            (this->*(binding.handler))(msg);
            return true;
        }
    }
    return false;
}

void ResDBIntersectionApp::handleSelfMsg(cMessage* msg)
{
    if (dispatchTimer(msg)) return;
    DemoBaseApplLayer::handleSelfMsg(msg);
}

void ResDBIntersectionApp::onCrashMacGrace(cMessage* msg)
{
    crash_mac_grace_msg_ = nullptr;
    std::cout << "[CRASH-COMMS-DEAD] r" << ctx_.replicaId_
              << " t=" << simTime()
              << " grace_sec=" << crash_mac_grace_sec_ << "\n";
    delete msg;
    return;
}

void ResDBIntersectionApp::onChannelMetrics(cMessage* msg)
{
    if (channel_metrics_)
        channel_metrics_->tick(simTime());
    scheduleAt(simTime() + 0.1, channel_metrics_timer_);
    return;
}

void ResDBIntersectionApp::onSmokeTest(cMessage* msg)
{
    smoke_test_msg_ = nullptr;
    const uint8_t probe[] = {'R','E','S','D','B','T','S','T'};
    ResdbOmnetTestBroadcast(ctx_.resdb_server_handle_, probe, sizeof(probe));
    delete msg; return;
}

void ResDBIntersectionApp::onTransportPoll(cMessage* msg)
{
    drainOutboundQueue();
    processOrders();
    // Detect primary change after view-change.
    if (ctx_.resdb_server_handle_) {
        int current_primary = ResdbOmnetGetPrimary(ctx_.resdb_server_handle_);
        if (current_primary != last_known_primary_) {
            std::cout << "[VC-DEBUG] r" << ctx_.replicaId_
                      << " primary changed: " << last_known_primary_
                      << " -> " << current_primary
                      << " t=" << simTime()
                      << " phase=" << phaseToStr(ctx_.current_phase_)
                      << " propose_submitted=" << ctx_.propose_submitted_
                      << " order_applied=" << ctx_.order_applied_ << "\n";
            last_known_primary_ = current_primary;
            const int initial_order_primary = order_candidate_
                ? order_candidate_->initialPrimary : CertPrimary();
            if (order_vc_requested_ ||
                    (ctx_.discovery_.state == DiscoveryState::COMPLETE &&
                     current_primary != initial_order_primary)) {
                order_vc_authoritative_ = true;
            }
            if (current_primary == ctx_.replicaId_ && !is_intersection_unit_ &&
                !ctx_.order_applied_ &&
                ctx_.discovery_.state == DiscoveryState::COMPLETE &&
                ctx_.current_phase_ != ConsensusPhase::DEPARTED) {
                std::cout << "[VC-DEBUG] r" << ctx_.replicaId_
                          << " became primary, evaluating ORDER in phase="
                          << phaseToStr(ctx_.current_phase_) << " t=" << simTime() << "\n";
                evaluateOrderReadiness("primary-change");
            } else if (current_primary == ctx_.replicaId_) {
                std::cout << "[VC-DEBUG] r" << ctx_.replicaId_
                          << " became primary but skipped re-propose"
                          << " propose_submitted=" << ctx_.propose_submitted_
                          << " order_applied=" << ctx_.order_applied_
                          << " phase=" << phaseToStr(ctx_.current_phase_) << " t=" << simTime() << "\n";
            }
        }
    }
    scheduleAt(simTime() + transport_poll_interval_, transport_poll_msg_);
    return;
}

void ResDBIntersectionApp::onTimeTick(cMessage* msg)
{
    if (enable_sim_time_provider_)
        ResdbOmnetUpdateSimTimeUs(ctx_.resdb_server_handle_, simTime().inUnit(SIMTIME_US));
    scheduleAt(simTime() + time_tick_interval_, time_tick_msg_);
    return;
}

void ResDBIntersectionApp::onConsensusRetry(cMessage* msg)
{
    retryConsensusPackets();
    return;
}

void ResDBIntersectionApp::onConsensusRelay(cMessage* msg)
{
    flushDueConsensusRelays();
    return;
}

void ResDBIntersectionApp::onCancelDrain(cMessage* msg)
{
    cancel_drain_timer_ = nullptr;
    finishCancelDrain();
    delete msg; return;
}

void ResDBIntersectionApp::onInitialAnnounce(cMessage* msg)
{
    initial_announce_msg_ = nullptr;
    broadcastArrivalAnnouncement();
    delete msg; return;
}

void ResDBIntersectionApp::onBroadcastArrivalAnnouncement(cMessage* msg)
{
    // Periodic self-message: never delete after scheduleAt() — the same cMessage must stay owned
    // by the FES until cancelled (see transport_poll_msg_ / time_tick_msg_ above).
    // Keep re-announcing until cert is assembled: a car may have observed all peers yet still
    // lack f+1 echoes, so witnesses need continued re-announces to trigger re-echoes.
    if (ctx_.cancel_pending_ && !rollback_local_recallable_) {
        std::cout << "[DISCOVERY-VIEW] r" << ctx_.replicaId_
                  << " suppress periodic announce; local non-recallable"
                  << " cancelled_epoch=" << cancelled_epoch_
                  << " new_epoch=" << rollback_new_epoch_ << "\n";
        broadcastArrivalAnnouncement_timer_ = nullptr;
        delete msg;
        return;
    }
    // Late ambulance excluded from the committed order keeps re-broadcasting its
    // emergency arrival (bypassing ctx_.order_applied_) until admitted to a committed order,
    // so peers witness it and echo a CANCEL. Checked before the normal COLLECTING branch
    // because deactivateDiscovery("order-applied") leaves ctx_.discovery_.state == INACTIVE.
    if (is_ambulance_ && ctx_.enableRollback_ && ctx_.has_committed_order_ &&
            !ctx_.committed_order_vehicle_ids_.count(ctx_.replicaId_) &&
            !isEpochTombstoned(ctx_.last_committed_epoch_) &&
            ctx_.current_phase_ != ConsensusPhase::DEPARTED) {
        broadcastArrivalAnnouncement(/*forceEmergency=*/true);
        scheduleAt(simTime() + broadcast_arrival_announcement_interval_,
                   broadcastArrivalAnnouncement_timer_);
        std::cout << "[AMBULANCE-FORCE-ANN] r" << ctx_.replicaId_
                  << " excluded from committed epoch=" << ctx_.last_committed_epoch_
                  << "; re-broadcasting emergency arrival t=" << simTime() << "\n";
        return;
    }
    if (!cert_broadcast_ && ctx_.discovery_.state == DiscoveryState::COLLECTING) {
        broadcastArrivalAnnouncement();
        // Double after each send, capped. resetAnnounceBackoff() pulls this back
        // to the base interval whenever a previously unseen car is registered,
        // so a peer entering radio range is not left waiting out a long gap.
        announce_backoff_ = std::min(announce_backoff_ * 2, announce_max_interval_);
        scheduleAt(simTime() + announce_backoff_, broadcastArrivalAnnouncement_timer_);
        std::cout << "[ANN-SEND] Replica " << ctx_.replicaId_
                  << " rescheduled arrival-announcement timer backoff="
                  << announce_backoff_ << "\n";
    } else {
        std::cout << "[ANN-SEND-STOP] r" << ctx_.replicaId_
                  << " discovery_state=" << discoveryStateName()
                  << " cert_broadcast=" << (cert_broadcast_ ? 1 : 0)
                  << " order_applied=" << (ctx_.order_applied_ ? 1 : 0)
                  << " propose_submitted=" << (ctx_.propose_submitted_ ? 1 : 0)
                  << " t=" << simTime() << "\n";
        broadcastArrivalAnnouncement_timer_ = nullptr;
        delete msg;
    }
    return;
}

void ResDBIntersectionApp::onCertRetry(cMessage* msg)
{
    if (cert_pending_retries_.carId.empty() ||
            ctx_.discovery_.state == DiscoveryState::INACTIVE ||
            ctx_.discovery_.state == DiscoveryState::COMPLETE ||
            (ctx_.discovery_.state == DiscoveryState::DRAINING_CERTS &&
             ctx_.discovery_.localCertAired())) {
        std::cout << "[CERT-RETX-STOP] r" << ctx_.replicaId_
                  << " carId=" << cert_pending_retries_.carId
                  << " propose_submitted=" << (ctx_.propose_submitted_ ? 1 : 0)
                  << " order_applied=" << (ctx_.order_applied_ ? 1 : 0)
                  << " discovery_state=" << discoveryStateName()
                  << " local_cert_aired=" << (ctx_.discovery_.localCertAired() ? 1 : 0)
                  << " t=" << simTime() << "\n";
        stopCertBroadcastRetries();
        return;
    }
    cert_retry_count_++;
    sendBFTMessage(-1, serializeArrivalCert(cert_pending_retries_), kArrivalCertType, true);
    std::cout << "[CERT-RETX] Replica " << ctx_.replicaId_ << " ARRIVAL_CERT " << cert_pending_retries_.carId
              << " retry " << cert_retry_count_;
    if (cert_retry_max_ > 0)
        std::cout << "/" << cert_retry_max_;
    std::cout << "\n";
    if (cert_retry_max_ > 0 && cert_retry_count_ >= cert_retry_max_) {
        stopCertBroadcastRetries();
        return;
    }
    scheduleNextCertRetry();
    return;
}

void ResDBIntersectionApp::onDiscoveryTxFlush(cMessage* msg)
{
    flushDueDiscoveryTxs();
    return;
}

void ResDBIntersectionApp::onCertGossip(cMessage* msg)
{
    if (ctx_.discovery_.state != DiscoveryState::COLLECTING || CertPrimary() != ctx_.replicaId_ ||
            (cert_gossip_deadline_ >= SIMTIME_ZERO && simTime() >= cert_gossip_deadline_)) {
        cert_gossip_timer_ = nullptr;
        cert_gossip_deadline_ = -1;
        delete msg;
        return;
    }
    broadcastCollectedCerts("stop-zone-periodic");
    scheduleNextStopZoneCertGossip();
    return;
}

void ResDBIntersectionApp::onGossip(cMessage* msg)
{
    if (gossip_order_bytes_.empty()) return;
    if (isEpochTombstoned(gossip_epoch_) ||
            (cancel_consensus_pending_ && gossip_epoch_ == cancelled_epoch_)) {
        std::cout << "[GOSSIP-STOP] r" << ctx_.replicaId_
                  << " epoch=" << gossip_epoch_
                  << " reason=cancel-active t=" << simTime() << "\n";
        stopGossip();
        return;
    }
    if (decisionGossipPropagationConfirmed()) {
        std::cout << "[GOSSIP-STOP] r" << ctx_.replicaId_
                  << " epoch=" << gossip_epoch_
                  << " reason=propagation-confirmed t=" << simTime() << "\n";
        stopGossip();
        return;
    }
    auto inner  = resdb_gossip::serialize(gossip_epoch_, gossip_order_bytes_);
    auto signed_payload = resdbwire::packSignedPacket(
        ctx_.ec_private_key_, ctx_.ec_pub_key_, inner.data(), (uint32_t)inner.size());
    if (!signed_payload.empty()) {
        sendBFTMessage(-1, signed_payload, kDecisionGossipType);
        std::cout << "[GOSSIP-SEND] r" << ctx_.replicaId_ << " epoch=" << gossip_epoch_
                  << " retry=" << gossip_retry_count_ << " t=" << simTime() << "\n";
    }
    gossip_retry_count_++;
    scheduleNextGossip();
    return;
}

void ResDBIntersectionApp::onCancelGossip(cMessage* msg)
{
    if (cancel_gossip_bytes_.empty()) {
        cancel_gossip_timer_ = nullptr;
        delete msg;
        return;
    }
    if (cancelGossipPropagationConfirmed()) {
        std::cout << "[CANCEL-GOSSIP-STOP] r" << ctx_.replicaId_
                  << " cancelled_epoch=" << cancel_gossip_epoch_
                  << " reason=propagation-confirmed\n";
        cancel_gossip_timer_ = nullptr;
        cancel_gossip_bytes_.clear();
        delete msg;
        return;
    }
    auto inner = resdb_gossip::serialize(cancel_gossip_epoch_,
                                         cancel_gossip_bytes_);
    auto signed_payload = resdbwire::packSignedPacket(
        ctx_.ec_private_key_, ctx_.ec_pub_key_, inner.data(), (uint32_t)inner.size());
    if (!signed_payload.empty()) {
        sendBFTMessage(-1, signed_payload, kCancelCommitGossipType);
        std::cout << "[CANCEL-GOSSIP-SEND] r" << ctx_.replicaId_
                  << " cancelled_epoch=" << cancel_gossip_epoch_
                  << " retry=" << cancel_gossip_retry_count_ << "\n";
    }
    if (cancel_cert_retry_max_ > 0 &&
            cancel_gossip_retry_count_ + 1 >= cancel_cert_retry_max_) {
        cancel_gossip_retry_count_++;
        cancel_gossip_timer_ = nullptr;
        cancel_gossip_bytes_.clear();
        delete msg;
        return;
    }
    {
        const double delay = backoffDelaySec(cancel_gossip_retry_base_sec_,
                                             cancel_gossip_retry_cap_sec_,
                                             cancel_gossip_retry_count_);
        cancel_gossip_retry_count_++;
        scheduleAt(simTime() + delay, cancel_gossip_timer_);
    }
    return;
}

void ResDBIntersectionApp::onCancelCertRetry(cMessage* msg)
{
    if (cancel_cert_pending_retries_.echoes.empty() ||
            (!cancel_consensus_pending_ && !ctx_.cancel_pending_)) {
        std::cout << "[CANCEL-CERT-RETX-STOP] r" << ctx_.replicaId_
                  << " cancel_consensus_pending=" << (cancel_consensus_pending_ ? 1 : 0)
                  << " cancel_pending=" << (ctx_.cancel_pending_ ? 1 : 0)
                  << " discovery_state=" << discoveryStateName()
                  << " t=" << simTime() << "\n";
        stopCancelCertRetries();
        return;
    }
    {
        const std::string key = cancelReasonKey(cancel_cert_pending_retries_.cancelledEpoch,
                                                 cancel_cert_pending_retries_.reason,
                                                 cancel_cert_pending_retries_.reasonRef);
        if (cancelCertPropagationConfirmed(key)) {
            std::cout << "[CANCEL-CERT-STOP] r" << ctx_.replicaId_
                      << " key=" << key << " reason=propagation-confirmed\n";
            stopCancelCertRetries();
            return;
        }
    }
    cancel_cert_retry_count_++;
    sendBFTMessage(-1, serializeCancelCert(cancel_cert_pending_retries_), kCancelCertType);
    std::cout << "[CANCEL-CERT-RETX] r" << ctx_.replicaId_
              << " key=" << cancelReasonKey(cancel_cert_pending_retries_.cancelledEpoch,
                                             cancel_cert_pending_retries_.reason,
                                             cancel_cert_pending_retries_.reasonRef)
              << " retry=" << cancel_cert_retry_count_ << "\n";
    if (cancel_cert_retry_max_ > 0 &&
            cancel_cert_retry_count_ >= cancel_cert_retry_max_) {
        stopCancelCertRetries();
        return;
    }
    scheduleNextCancelCertRetry();
    return;
}

void ResDBIntersectionApp::onClearCertRetry(cMessage* msg)
{
    const BlockedIncident incident{clear_cert_pending_retries_.cancelledEpoch,
                                    clear_cert_pending_retries_.executingBatch};
    auto it = incidentRegistry_.find(incident);
    const bool alreadyCleared = it != incidentRegistry_.end() &&
        it->second.state == IncidentState::CLEARED;
    if (clear_cert_pending_retries_.echoes.empty() || alreadyCleared) {
        stopClearCertRetries();
        return;
    }
    clear_cert_retry_count_++;
    sendClearCertCarrier(clear_cert_pending_retries_, "CLEAR-CERT-RETX");
    std::cout << "[CLEAR-CERT-RETX] r" << ctx_.replicaId_
              << " epoch=" << clear_cert_pending_retries_.cancelledEpoch
              << " batch=" << clear_cert_pending_retries_.executingBatch
              << " retry=" << clear_cert_retry_count_ << "\n";
    if (cancel_cert_retry_max_ > 0 &&
            clear_cert_retry_count_ >= cancel_cert_retry_max_) {
        stopClearCertRetries();
        return;
    }
    scheduleNextClearCertRetry();
    return;
}

void ResDBIntersectionApp::onClearCertCandidate(cMessage* msg)
{
    if (clear_cert_candidate_.echoes.empty()) return;
    const ClearCert cert = clear_cert_candidate_;
    const BlockedIncident incident{
        cert.cancelledEpoch, cert.executingBatch};
    auto incidentIt = incidentRegistry_.find(incident);
    const bool alreadyCleared =
        incidentIt != incidentRegistry_.end() &&
        incidentIt->second.state == IncidentState::CLEARED;
    if (alreadyCleared || ctx_.order_applied_) {
        cancelClearCertCandidate(
            ctx_.order_applied_ ? "order-applied" : "incident-cleared");
        return;
    }
    broadcastClearCert(cert);
    return;
}

void ResDBIntersectionApp::onClearCertRelay(cMessage* msg)
{
    if (clear_cert_pending_relay_.echoes.empty()) return;
    const std::string key = clear_cert_pending_relay_key_;
    if (clearPropagationConfirmed(key) || ctx_.order_applied_ ||
            ctx_.propose_submitted_) {
        cancelClearCertRelay(
            ctx_.order_applied_ ? "order-applied" :
            ctx_.propose_submitted_ ? "order-proposed" :
            "propagation-confirmed");
        return;
    }
    const ClearCert cert = clear_cert_pending_relay_;
    clear_propagation_tracker_.observeAuthenticated(key, ctx_.replicaId_);
    std::cout << "[CLEAR-CARRIER] r" << ctx_.replicaId_
              << " key=" << key
              << " carrier=r" << ctx_.replicaId_
              << " count=" << clear_propagation_tracker_.count(key)
              << "/" << clearPropagationThreshold()
              << " source=local-relay\n";
    sendClearCertCarrier(cert, "CLEAR-RELAY");
    clear_cert_pending_relay_ = ClearCert{};
    clear_cert_pending_relay_key_.clear();
    return;
}

void ResDBIntersectionApp::onWaitLeaderSend(cMessage* msg)
{
    maybeSendWaitHeartbeat("periodic");
    return;
}

void ResDBIntersectionApp::onWaitFollowerExpiry(cMessage* msg)
{
    std::cout << "[WAIT-EXPIRED] r" << ctx_.replicaId_
              << " epoch=" << wait_follower_state_.cancelledEpoch
              << " batch=" << wait_follower_state_.executingBatch
              << " leader=r" << wait_follower_state_.leaderId
              << " t=" << simTime() << "\n";
    wait_follower_state_.active = false;
    order_vc_requested_ = true;
    ResdbOmnetForceViewChange(ctx_.resdb_server_handle_);
    std::cout << "[APP-VC] r" << ctx_.replicaId_
              << " WAIT expiry forced view change t=" << simTime() << "\n";
    return;
}

void ResDBIntersectionApp::onDiscoveryDeadline(cMessage* msg)
{
    maybeAdvanceDiscovery("hard-deadline", true);
    return;
}

void ResDBIntersectionApp::onDiscoverySettle(cMessage* msg)
{
    maybeAdvanceDiscovery("intent-stable", false);
    return;
}

void ResDBIntersectionApp::onVcTrigger(cMessage* msg)
{
    vc_trigger_msg_ = nullptr;
    processOrders();
    if (!ctx_.order_applied_) {
        if (cancel_consensus_pending_) {
            std::cout << "[ROLLBACK-VC-UNSUPPORTED] r" << ctx_.replicaId_
                      << " suppressed app forced view-change while CANCEL consensus active"
                      << " cancelled_epoch=" << cancelled_epoch_
                      << " new_epoch=" << rollback_new_epoch_
                      << " phase=" << phaseToStr(ctx_.current_phase_)
                      << " propose_submitted=" << ctx_.propose_submitted_
                      << " t=" << simTime() << "\n";
            delete msg; return;
        }
        if (ctx_.cancel_pending_ && hasBlockingIncidentForEpoch(cancelled_epoch_)) {
            std::cout << "[ORDER-VC-DEFER] r" << ctx_.replicaId_
                      << " reason=blocking-incident-with-wait"
                      << " cancelled_epoch=" << cancelled_epoch_
                      << " t=" << simTime() << "\n";
            maybeSendWaitHeartbeat("order-vc-defer");
            delete msg; return;
        }
        int primary = ResdbOmnetGetPrimary(ctx_.resdb_server_handle_);
        std::cout << "[VC-TRIGGER] r" << ctx_.replicaId_
                  << " forcing view change at " << simTime()
                  << " phase=" << phaseToStr(ctx_.current_phase_)
                  << " pbft_primary=" << primary
                  << " propose_submitted=" << ctx_.propose_submitted_;
        if (stop_time_ >= SIMTIME_ZERO)
            std::cout << " stop_to_vc_sec=" << (simTime() - stop_time_).dbl();
        std::cout << "\n";
        // TriggerViewChangeNow() in bridge: ChangeStatue(READY_VIEW_CHANGE) +
        // SendViewChangeMsg() directly.  All downstream VC timers (TYPE_VIEWCHANGE,
        // TYPE_NEWVIEW) use SleepForUs driven by SimTimeProvider → sim-time.
        order_vc_requested_ = true;
        ResdbOmnetForceViewChange(ctx_.resdb_server_handle_);
        std::cout << "[APP-VC] r" << ctx_.replicaId_
                  << " ResdbOmnetForceViewChange returned t=" << simTime() << "\n";
    }
    delete msg; return;
}

void ResDBIntersectionApp::onCancelVc(cMessage* msg)
{
    cancel_vc_timer_ = nullptr;
    const int oldProposer = chooseCancelProposer();
    const int oldIndex = cancel_rotation_index_;
    const std::vector<int> oldElectors = cancelElectorateCandidates();
    cancel_rotation_index_++;
    cancel_propose_submitted_ = false;
    ctx_.propose_submitted_ = false;
    const int newProposer = chooseCancelProposer();
    const std::vector<int> newElectors = cancelElectorateCandidates();
    std::cout << "[CANCEL-VC-STATE] r" << ctx_.replicaId_
              << " old_index=" << oldIndex
              << " new_index=" << cancel_rotation_index_
              << " old_proposer=r" << oldProposer
              << " new_proposer=r" << newProposer
              << " pending=" << (cancel_consensus_pending_ ? 1 : 0)
              << " cancel_pending=" << (ctx_.cancel_pending_ ? 1 : 0)
              << " submitted_reset=1"
              << " |E_old|=" << oldElectors.size()
              << " |E_new|=" << newElectors.size()
              << " cert_bytes=" << cancel_cert_bytes_.size()
              << " t=" << simTime() << "\n";
    std::cout << "[CANCEL-VC] r" << ctx_.replicaId_
              << " rotating cancel proposer index=" << cancel_rotation_index_
              << " cancelled_epoch=" << cancelled_epoch_
              << " t=" << simTime() << "\n";
    trySubmitCancelProposal("cancel-vc-timeout");
    delete msg; return;
}

void ResDBIntersectionApp::onStopSignTimeout(cMessage* msg)
{
    stop_sign_timeout_msg_ = nullptr;
    // Apply any consensus orders already queued before declaring timeout.
    // Otherwise FES ordering can deliver this self-message before the next
    // transport poll: processOrders() would see ctx_.order_applied_=1 and drop
    // the tail of pending_orders_ without ever logging Batch_Assignment.
    std::cout << "[TIMEOUT-PRE] r" << ctx_.replicaId_
              << " stop_sign flush processOrders order_applied=" << ctx_.order_applied_
              << " phase=" << phaseToStr(ctx_.current_phase_) << " t=" << simTime()
              << "\n";
    processOrders();
    std::cout << "[TIMEOUT-POST] r" << ctx_.replicaId_
              << " stop_sign after processOrders order_applied=" << ctx_.order_applied_
              << " t=" << simTime() << "\n";
    if (!ctx_.order_applied_) {
        if (ctx_.cancel_pending_ || cancel_consensus_pending_) {
            std::cout << "[HALT-LOCAL] r" << ctx_.replicaId_
                      << " stop_sign_timeout suppressed while cancel active epoch="
                      << cancelled_epoch_ << "\n";
            delete msg; return;
        }
        if (ctx_.enableRollback_) {
            std::cout << "[HALT-LOCAL] r" << ctx_.replicaId_
                      << " stop_sign_timeout fail-closed"
                      << " reason=no-committed-order"
                      << " epoch=" << ctx_.current_epoch_ << "\n";
            evaluateOrderReadiness("stop-sign-timeout");
            delete msg; return;
        }
        std::cout << "[METRICS " << ctx_.replicaId_ << "] StopSign_Timeout: 1\n";
        std::cout << "[VC-TIMEOUT] r" << ctx_.replicaId_
                  << " stop_sign_timeout_fired at=" << simTime()
                  << " phase=" << phaseToStr(ctx_.current_phase_)
                  << " propose_submitted=" << ctx_.propose_submitted_;
        if (stop_time_ >= SIMTIME_ZERO)
            std::cout << " stop_to_timeout_sec=" << (simTime() - stop_time_).dbl();
        if (propose_time_ >= SIMTIME_ZERO)
            std::cout << " propose_to_timeout_sec=" << (simTime() - propose_time_).dbl();
        std::cout << "\n";
        ctx_.order_applied_ = true;
        ctx_.current_phase_ = ConsensusPhase::EXECUTING;
        resumeVehicle(0);
    }
    delete msg; return;
}

void ResDBIntersectionApp::onConsensusTimeout(cMessage* msg)
{
    consensus_timeout_msg_ = nullptr;
    std::cout << "[TIMEOUT-PRE] r" << ctx_.replicaId_
              << " consensus flush processOrders order_applied=" << ctx_.order_applied_
              << " phase=" << phaseToStr(ctx_.current_phase_) << " t=" << simTime()
              << "\n";
    processOrders();
    std::cout << "[TIMEOUT-POST] r" << ctx_.replicaId_
              << " consensus after processOrders order_applied=" << ctx_.order_applied_
              << " t=" << simTime() << "\n";
    if (!ctx_.order_applied_) {
        if (ctx_.cancel_pending_ || cancel_consensus_pending_) {
            std::cout << "[HALT-LOCAL] r" << ctx_.replicaId_
                      << " consensus_timeout suppressed while cancel active epoch="
                      << cancelled_epoch_ << "\n";
            delete msg; return;
        }
        if (ctx_.enableRollback_) {
            std::cout << "[HALT-LOCAL] r" << ctx_.replicaId_
                      << " consensus_timeout fail-closed"
                      << " reason=no-committed-order"
                      << " epoch=" << ctx_.current_epoch_ << "\n";
            evaluateOrderReadiness("consensus-timeout");
            delete msg; return;
        }
        std::cout << "[METRICS " << ctx_.replicaId_ << "] Consensus_Timeout: 1\n";
        std::cout << "[VC-TIMEOUT] r" << ctx_.replicaId_
                  << " consensus_timeout_fired at=" << simTime()
                  << " phase=" << phaseToStr(ctx_.current_phase_)
                  << " propose_submitted=" << ctx_.propose_submitted_ << "\n";
        ctx_.order_applied_ = true;
        ctx_.current_phase_ = ConsensusPhase::EXECUTING;
        resumeVehicle(0);
    }
    delete msg; return;
}

void ResDBIntersectionApp::onResume(cMessage* msg)
{
    resume_msg_ = nullptr;
    if (ctx_.cancel_pending_ || cancel_consensus_pending_) {
        std::cout << "[HALT-LOCAL] r" << ctx_.replicaId_
                  << " pending resume suppressed while cancel active epoch="
                  << cancelled_epoch_ << "\n";
        delete msg; return;
    }
    resumeVehicle(pending_resume_position_);
    delete msg; return;
}

void ResDBIntersectionApp::onPrecedingBatchPoll(cMessage* msg)
{
    // Scenario 16 crash-dwell perception: scans every batch strictly before
    // my own (not just preceding_batch_cars_, which is only my_batch-1) so
    // every vehicle not in batch 0 is a potential BLOCKED witness. Gated
    // purely on ctx_.enableRollback_ — no separate scenario flag.
    if (ctx_.enableRollback_) {
        for (int b = 0; b < my_batch_index_; ++b) {
            if (b >= (int)committed_order_batches_.size()) continue;
            for (int rid : committed_order_batches_[b]) {
                if (rid == ctx_.replicaId_) continue;
                const std::string target = "veh" + std::to_string(rid);
                const bool qualified = !vehicleHasClearedIntersectionTraCI(target) &&
                    vehicleInConflictBoxTraCI(target) &&
                    vehicleSpeedTraCI(target) < crash_speed_eps_;
                if (!qualified) {
                    crash_dwell_since_.erase(target);
                    continue;
                }
                auto dwellIt = crash_dwell_since_.find(target);
                if (dwellIt == crash_dwell_since_.end()) {
                    crash_dwell_since_[target] = simTime();
                    continue;
                }
                const double dwell = (simTime() - dwellIt->second).dbl();
                if (dwell < crash_dwell_sec_) continue;
                if (crash_echoed_targets_.count(target)) continue;

                std::cout << "[CRASH-PERCEIVE] r" << ctx_.replicaId_
                          << " target=" << target
                          << " batch=" << b
                          << " dwell=" << dwell << "\n";
                maybeTriggerCrashRollback(formatBlockedBatchRef(ctx_.last_committed_epoch_, (uint32_t)b));
                crash_echoed_targets_.insert(target);
            }
        }

        // CLEAR empty-box dwell: same tick, same ctx_.enableRollback_ gate, no
        // separate timer. Checks whole-box occupancy (not per-target) since
        // the clearance predicate is "no vehicle occupies the box at all".
        for (const auto& kv : incidentRegistry_) {
            const BlockedIncident& incident = kv.first;
            if (kv.second.state != IncidentState::BLOCKING) continue;
            if (clear_echoed_incidents_.count(incident)) continue;
            if (anyVehicleInConflictBoxTraCI()) {
                clear_dwell_since_.erase(incident);
                continue;
            }
            auto dwellIt = clear_dwell_since_.find(incident);
            if (dwellIt == clear_dwell_since_.end()) {
                clear_dwell_since_[incident] = simTime();
                continue;
            }
            const double dwell = (simTime() - dwellIt->second).dbl();
            if (dwell < clear_dwell_sec_) continue;

            std::cout << "[CLEAR-PERCEIVE] r" << ctx_.replicaId_
                      << " epoch=" << incident.cancelledEpoch
                      << " batch=" << incident.executingBatch
                      << " dwell=" << dwell << "\n";
            sendClearEcho(incident.cancelledEpoch, incident.executingBatch);
            clear_echoed_incidents_.insert(incident);
        }
    }

    // Batch-gated resume: poll until ALL vehicles in the preceding batch
    // have physically cleared the intersection (or timeout fires).
    if (my_batch_index_ <= 0 || preceding_batch_cars_.empty()) {
        return;  // nothing to wait for (batch 0 goes immediately)
    }
    bool all_cleared = true;
    for (int rid : preceding_batch_cars_) {
        if (!vehicleHasClearedIntersectionTraCI("veh" + std::to_string(rid))) {
            all_cleared = false;
            break;
        }
    }
    bool timedOut = (clearance_started_ >= SIMTIME_ZERO) &&
                    ((simTime() - clearance_started_).dbl() >= clearance_timeout_sec_);

    if (all_cleared || timedOut) {
        if (ctx_.cancel_pending_ || cancel_consensus_pending_) {
            if (timedOut && !all_cleared) {
                std::cout << "[HALT-LOCAL] r" << ctx_.replicaId_
                          << " clearance timeout suppressed while cancel active epoch="
                          << cancelled_epoch_ << "\n";
            }
            scheduleAt(simTime() + preceding_batch_poll_period_sec_, preceding_batch_poll_msg_);
            return;
        }
        if (timedOut && !all_cleared) {
            std::cout << "[CLEARANCE r" << ctx_.replicaId_ << "] TIMEOUT waiting for batch "
                      << (my_batch_index_ - 1) << "; resuming anyway\n";
        } else {
            std::cout << "[CLEARANCE r" << ctx_.replicaId_ << "] batch "
                      << (my_batch_index_ - 1) << " cleared, resuming batch "
                      << my_batch_index_ << "\n";
        }
        std::cout << "[METRICS " << ctx_.replicaId_ << "] Resume_Time: " << simTime()
                  << " (batch=" << my_batch_index_ << ")\n";
        resumeVehicle(my_batch_index_);
        return;
    }

    scheduleAt(simTime() + preceding_batch_poll_period_sec_, preceding_batch_poll_msg_);
    return;
}


// ── handlePositionUpdate ──────────────────────────────────────────────────────

void ResDBIntersectionApp::handlePositionUpdate(cObject* obj)
{
    DemoBaseApplLayer::handlePositionUpdate(obj);

    // Static intersection units have no SUMO vehicle: skip lane discovery, stop-zone
    // entry, and crossing. This also prevents discoverLane() from dereferencing a
    // null mobility (RSU hosts a BaseMobility, not a TraCIMobility, so BaseMobility's
    // mobilityStateChangedSignal still delivers one position update here).
    if (is_intersection_unit_ || !mobility) return;

    if (moduleIsAmbulance && !ambulanceColorSet && mobility && mobility->getVehicleCommandInterface()) {
        std::cout << "[AMBULANCE COLOR] r" << ctx_.replicaId_ << " setting color to red\n";
        mobility->getVehicleCommandInterface()->setColor(TraCIColor(255, 0, 0, 255));
        ambulanceColorSet = true;
    }

    discoverLane();

    if (ctx_.order_applied_) return;

    double dist = getDistanceToIntersection();
    if (dist >= stop_distance_ || dist <= 0) return;

    if (!entered_stop_zone_) {
        entered_stop_zone_ = true;
        stop_time_ = simTime();
        ctx_.current_phase_ = ConsensusPhase::WAITING_FOR_CLEARANCE;
        std::cout << "[METRICS " << ctx_.replicaId_ << "] Stop_Time: " << stop_time_ << "\n";

        for (auto& [key, pr] : pending_relays_) {
            if (pr.relayCount >= 3) continue;
            if (ctx_.collected_certs_.count(pr.carId)) continue;

            sendArrivalAnnouncementGossipPayload(
                pr.carId, pr.epoch, pr.serializedAnnounce, "stop-zone");
            pr.relayCount++;
        }
        pending_relays_.clear();

        startStopZoneCertGossip("stop-zone");

        stopVehicle();
        
        // Safety fallback timers.
        stop_sign_timeout_msg_ = new cMessage("resdbStopSignTimeout");
        scheduleAt(simTime() + stop_sign_timeout_sec_, stop_sign_timeout_msg_);
        consensus_timeout_msg_ = new cMessage("resdbConsensusTimeout");
        scheduleAt(simTime() + consensus_timeout_sec_, consensus_timeout_msg_);
        std::cout << "[VC-DEBUG] r" << ctx_.replicaId_
                  << " stop-zone timers armed stop_sign_deadline="
                  << simTime() + stop_sign_timeout_sec_
                  << " consensus_deadline=" << simTime() + consensus_timeout_sec_ << "\n";

        armDiscoveryTimers("stop-zone-entry");
        maybeAdvanceDiscovery("stop-zone-entry");
    }
}

void ResDBIntersectionApp::recordIntersectionDeparture(simtime_t departedAt)
{
    if (departureTime >= SIMTIME_ZERO) return;

    departureTime = departedAt;
    cleared_time_ = departedAt;
    ctx_.is_departed_ = true;
    ctx_.current_phase_ = ConsensusPhase::DEPARTED;
    clearConsensusRetries("departed");
    deactivateDiscovery("departed");
    markCompletedReplicaEpoch(ctx_.replicaId_, ctx_.current_epoch_);
    if (ctx_.resdb_server_handle_) {
        ResdbOmnetMarkReplicaInactive(ctx_.resdb_server_handle_, ctx_.replicaId_, ctx_.current_epoch_ + 1);
    }
    {
        std::lock_guard<std::mutex> lk(outbound_mutex_);
        outbound_queue_.clear();
    }

    const double wait_sec = (stop_time_ >= SIMTIME_ZERO) ? (cleared_time_ - stop_time_).dbl() : -1.0;
    const double stop_dbl = (stop_time_ >= SIMTIME_ZERO) ? stop_time_.dbl() : -1.0;
    const char* role = is_ambulance_ ? "ambulance" : "normal";

    std::cout << "[DEPARTED] Replica " << ctx_.replicaId_ << " cleared intersection t=" << departedAt << "\n";
    std::cout << "[METRICS " << ctx_.replicaId_ << "] Total Latency (cleared-stop): " << wait_sec << std::endl;
    std::cout << "[CAR-METRICS] veh" << ctx_.replicaId_
              << " role=" << role
              << " epoch=" << ctx_.current_epoch_
              << " stop_time=" << stop_dbl
              << " depart_time=" << cleared_time_.dbl()
              << " wait_stop_to_departure_sec=" << wait_sec << "\n";
    if (is_ambulance_) {
        std::cout << "[AMBULANCE_METRICS] veh" << ctx_.replicaId_
                  << " sim_wait_stop_to_departure_sec=" << wait_sec
                  << " epoch=" << ctx_.current_epoch_ << "\n";
    }
}

// ── finish ────────────────────────────────────────────────────────────────────

void ResDBIntersectionApp::finish()
{
    // OMNeT++ freezes sim-time at endSimulation(), so any ResDB thread sleeping
    // in SleepUntilUs() would wait forever.  Advance sim-time to INT64_MAX first
    // so all SleepUntilUs waiters unblock before we call Stop()/join().
    std::cout << "[METRICS " << ctx_.replicaId_ << "] Messages_Sent: " << sentMessages_ << "\n";
    std::cout << "[METRICS " << ctx_.replicaId_ << "] Messages_Received: " << receivedMessages_ << "\n";
    std::cout << "[METRICS " << ctx_.replicaId_ << "] Bytes_Sent: " << sentPayloadBytes_ << "\n";
    std::cout << "[METRICS " << ctx_.replicaId_ << "] Megabytes_Sent: "
              << (static_cast<double>(sentPayloadBytes_) / (1024.0 * 1024.0)) << "\n";
    // One line per message type, so a layer's cost can be priced from a normal
    // run: the arrival-cert exchange and PBFT ordering (type 8) are separate
    // types, and totals alone cannot separate them.
    for (const auto& kv : sentMessagesByType_) {
        std::cout << "[METRICS " << ctx_.replicaId_ << "] Sent_By_Type: type="
                  << kv.first << " msgs=" << kv.second
                  << " bytes=" << sentBytesByType_[kv.first] << "\n";
    }
    std::cout << "[METRICS " << ctx_.replicaId_ << "] Quiet_Honest_Vehicles: "
              << quietHonestVehicles_ << "\n";
    std::cout << "[METRICS " << ctx_.replicaId_ << "] Quiet_Honest_Opportunities: "
              << quietHonestOpportunities_ << "\n";
    std::cout << "[METRICS " << ctx_.replicaId_ << "] Quiet_Honest_Rate: "
              << (quietHonestOpportunities_ > 0
                      ? (100.0 * static_cast<double>(quietHonestVehicles_) /
                         static_cast<double>(quietHonestOpportunities_))
                      : 0.0)
              << "\n";
    
    std::cerr << "[FINISH-PROBE] r" << ctx_.replicaId_
    << " handle=" << ctx_.resdb_server_handle_ << std::endl;

    if (ctx_.is_departed_ || departureTime >= SIMTIME_ZERO) {
        markCompletedReplicaEpoch(ctx_.replicaId_, ctx_.current_epoch_);
    }

    if (ctx_.resdb_server_handle_) {
        ResdbOmnetMarkReplicaInactive(ctx_.resdb_server_handle_, ctx_.replicaId_, ctx_.current_epoch_ + 1);
        ResdbOmnetUpdateSimTimeUs(ctx_.resdb_server_handle_,
                                  std::numeric_limits<int64_t>::max());
    }
    std::cerr << "[FINISH-PROBE] r" << ctx_.replicaId_ << " after UpdateSimTime" << std::endl;
    // Stop global monitor (stats thread); cv fix in stats.cpp ensures it wakes
    // immediately rather than sleeping for monitor_sleep_time_ wall-clock seconds.
    ResdbOmnetStopGlobalStats();
    std::cerr << "[FINISH-PROBE] r" << ctx_.replicaId_ << " after StopGlobalStats" << std::endl;

    // TraCI calls finish() from the scenario manager's timestep event before
    // dynamically deleting a vehicle module.  cancelEvent() therefore returns
    // a scheduled self-message to the manager's owning context.  Explicitly
    // retake each timer before deleting it; plain cancelAndDelete() here causes
    // OMNeT++'s object-stealing error.
    clearConsensusRetries("finish");
    auto deleteFinishedTimer = [this](cMessage*& timer) {
        if (!timer) return;
        if (timer->isScheduled()) cancelEvent(timer);
        if (timer->getOwner() != this) take(timer);
        delete timer;
        timer = nullptr;
    };
    deleteFinishedTimer(consensus_retry_timer_);
    deleteFinishedTimer(consensus_relay_timer_);

    deleteFinishedTimer(vc_trigger_msg_);
    if (ctx_.resdb_server_handle_) {
        std::cerr << "[FINISH-PROBE] r" << ctx_.replicaId_ << " calling StopServer" << std::endl;
        ResdbOmnetStopServer(ctx_.resdb_server_handle_);
        std::cerr << "[FINISH-PROBE] r" << ctx_.replicaId_ << " after StopServer" << std::endl;
        ResdbOmnetDestroyServer(ctx_.resdb_server_handle_);
        ctx_.resdb_server_handle_ = nullptr;
    }
    deleteFinishedTimer(channel_metrics_timer_);
    if (channel_metrics_) {
        cModule* nic = getParentModule() ? getParentModule()->getSubmodule("nic") : nullptr;
        cModule* mac = nic ? nic->getSubmodule("mac1609_4") : nullptr;
        if (mac && mac->isSubscribed(Mac1609_4::sigChannelBusy, channel_metrics_))
            mac->unsubscribe(Mac1609_4::sigChannelBusy, channel_metrics_);
        if (mac && mac->isSubscribed(Mac1609_4::sigCollision, channel_metrics_))
            mac->unsubscribe(Mac1609_4::sigCollision, channel_metrics_);
        delete channel_metrics_;
        channel_metrics_ = nullptr;
    }

    std::cerr << "[FINISH-PROBE] r" << ctx_.replicaId_ << " calling DemoBaseApplLayer::finish" << std::endl;
    DemoBaseApplLayer::finish();
    std::cerr << "[FINISH-PROBE] r" << ctx_.replicaId_ << " DONE" << std::endl;
}

// ── registerTransport ─────────────────────────────────────────────────────────

void ResDBIntersectionApp::onWSM(BaseFrame1609_4* wsm)
{
    receivedMessages_++;
    if (channel_metrics_) {
        if (auto* ci = dynamic_cast<PhyToMacControlInfo*>(wsm->getControlInfo())) {
            if (auto* res = dynamic_cast<DeciderResult80211*>(ci->getDeciderResult()))
                channel_metrics_->addSinrSample(res->getSnr());
        }
    }

    auto* bft = dynamic_cast<BFTMessage*>(wsm);
    if (!bft) return;
    if (bft->getFromReplicaId() == ctx_.replicaId_) return;   // no self-delivery
    if (ctx_.current_phase_ == ConsensusPhase::DEPARTED) return;  // Gap 9: zombie filter

    if (crashCommsDisabled_) {
        std::cout << "[CRASH-RX-DROP] r" << ctx_.replicaId_
                  << " from=" << bft->getFromReplicaId()
                  << " type=" << bft->getMessageType()
                  << " t=" << simTime() << "\n";
        return;
    }

    int msgType = bft->getMessageType();

    if (msgType == kArrivalAnnounceType) {
        handleArrivalAnnouncement(bft);
        return;
    }

    if (msgType == kArrivalEchoType) {
        if (bft->getToReplicaId() == ctx_.replicaId_ || bft->getToReplicaId() == -1)
            handleArrivalEcho(bft);
        return;
    }

    if (msgType == kArrivalCertType) {
        handleArrivalCert(bft);
        return;
    }

    // ── Type 9: Post-consensus order gossip ───────────────────────────────────
    if (msgType == kDecisionGossipType) {
        handleDecisionGossip(bft);
        return;
    }

    // ── Type 10: Arrival announce gossip ─────────────────────────────────────
    if (msgType == kArrivalAnnounceGossipType) {
        handleArrivalAnnouncementGossip(bft);
        return;
    }

    // ── Type 11: ResDB PBFT consensus relay ───────────────────────────────────
    if (msgType == kResdbConsensusRelayType) {
        handleResdbConsensusRelay(bft);
        bool has_pending_order = false;
        {
            std::lock_guard<std::mutex> lk(orders_mutex_);
            has_pending_order = !pending_orders_.empty();
        }
        if (vc_trigger_msg_ && vc_trigger_msg_->isScheduled()) {
            if (ctx_.order_applied_ || has_pending_order) {
                cancelEvent(vc_trigger_msg_);
                delete vc_trigger_msg_;
                vc_trigger_msg_ = nullptr;
                std::cout << "[VC-DEBUG] r" << ctx_.replicaId_
                          << " canceled follower vc_trigger on relayed delivered order at "
                          << simTime()
                          << " order_applied=" << ctx_.order_applied_
                          << " pending_order=" << has_pending_order << "\n";
            } else {
                cancelEvent(vc_trigger_msg_);
                scheduleAt(simTime() + pbft_vc_timeout_sec_, vc_trigger_msg_);
                std::cout << "[VC-DEBUG] r" << ctx_.replicaId_
                          << " rearmed follower vc_trigger on relayed PBFT traffic at "
                          << simTime() + pbft_vc_timeout_sec_
                          << " grace=" << pbft_vc_timeout_sec_ << "s\n";
            }
        }
        return;
    }

    if (msgType == kCancelEchoType) {
        handleCancelEcho(bft);
        return;
    }

    if (msgType == kCancelCertType) {
        handleCancelCert(bft);
        return;
    }

    if (msgType == kCancelCommitGossipType) {
        handleCancelCommitGossip(bft);
        return;
    }

    if (msgType == kClearEchoType) {
        handleClearEcho(bft);
        return;
    }

    if (msgType == kClearCertType) {
        handleClearCert(bft);
        return;
    }

    if (msgType == kWaitHeartbeatType) {
        handleWaitHeartbeat(bft);
        return;
    }

    // ── Type 8: ResDB PBFT consensus bytes ────────────────────────────────────
    if (msgType == kResdbConsensusMsgType) {
        handleResdbConsensusMessage(bft);

        bool has_pending_order = false;
        {
            std::lock_guard<std::mutex> lk(orders_mutex_);
            has_pending_order = !pending_orders_.empty();
        }
        if (vc_trigger_msg_ && vc_trigger_msg_->isScheduled()) {
            if (ctx_.order_applied_ || has_pending_order) {
                cancelEvent(vc_trigger_msg_);
                delete vc_trigger_msg_;
                vc_trigger_msg_ = nullptr;
                std::cout << "[VC-DEBUG] r" << ctx_.replicaId_
                          << " canceled follower vc_trigger on delivered order at "
                          << simTime()
                          << " order_applied=" << ctx_.order_applied_
                          << " pending_order=" << has_pending_order << "\n";
            } else {
                cancelEvent(vc_trigger_msg_);
                scheduleAt(simTime() + pbft_vc_timeout_sec_, vc_trigger_msg_);
                std::cout << "[VC-DEBUG] r" << ctx_.replicaId_
                          << " rearmed follower vc_trigger on PBFT traffic at "
                          << simTime() + pbft_vc_timeout_sec_
                          << " grace=" << pbft_vc_timeout_sec_ << "s\n";
            }
        }
    }
}

bool ResDBIntersectionApp::decisionGossipPropagationConfirmed() const
{
    if (gossip_order_bytes_.empty()) return false;
    // toleratedF(), not a vehicle-count f: decision gossip is relayed by every
    // replica including the static units, so the f+1 that confirms propagation
    // has to be f+1 of PBFT membership. Sizing it to vehicles alone confirms on
    // fewer distinct relayers than the threshold is meant to represent.
    const int f = toleratedF();
    return gossip_acc_.count(gossip_epoch_, gossip_order_bytes_) >= f + 1;
}

void ResDBIntersectionApp::triggerGossip(uint32_t epoch,
                                          const std::vector<uint8_t>& order_bytes)
{
    if (order_bytes.empty()) return;
    if (isEpochTombstoned(epoch) ||
            (cancel_consensus_pending_ && epoch == cancelled_epoch_)) {
        std::cout << "[GOSSIP-SKIP] r" << ctx_.replicaId_
                  << " epoch=" << epoch
                  << " reason=cancel-active t=" << simTime() << "\n";
        return;
    }
    if (gossip_timer_ && gossip_timer_->isScheduled())
        cancelEvent(gossip_timer_);

    gossip_epoch_       = epoch;
    gossip_order_bytes_ = order_bytes;
    gossip_retry_count_ = 0;

    // Broadcast first frame immediately (sign with own EC key).
    auto inner = resdb_gossip::serialize(epoch, order_bytes);
    auto signed_payload = resdbwire::packSignedPacket(
        ctx_.ec_private_key_, ctx_.ec_pub_key_, inner.data(), (uint32_t)inner.size());
    if (!signed_payload.empty()) {
        sendBFTMessage(-1, signed_payload, kDecisionGossipType);
        std::cout << "[GOSSIP-SEND] r" << ctx_.replicaId_ << " epoch=" << epoch
                  << " retry=0 t=" << simTime() << "\n";
    }
    gossip_retry_count_++;
    scheduleNextGossip();
}

void ResDBIntersectionApp::scheduleNextGossip()
{
    if (!ctx_.gossip_enabled_ || gossip_order_bytes_.empty()) return;
    if (gossip_max_retries_ > 0 && gossip_retry_count_ >= gossip_max_retries_) {
        stopGossip();
        return;
    }
    double interval = gossip_initial_interval_ * (1 << gossip_retry_count_);
    if (!gossip_timer_) gossip_timer_ = new cMessage("decisionGossip");
    scheduleAt(simTime() + interval, gossip_timer_);
}

void ResDBIntersectionApp::stopGossip()
{
    if (gossip_timer_) {
        if (gossip_timer_->isScheduled()) cancelEvent(gossip_timer_);
        delete gossip_timer_;
        gossip_timer_ = nullptr;
    }
    gossip_order_bytes_.clear();
}

void ResDBIntersectionApp::handleDecisionGossip(BFTMessage* bft)
{
    int plen = bft->getPayloadArraySize();
    if (plen <= 0) return;
    std::vector<uint8_t> buf((size_t)plen);
    for (int i = 0; i < plen; ++i) buf[i] = bft->getPayload(i);

    // Verify the sender's EC signature (same path as TYPE8).
    resdbwire::SignedPacketView view;
    if (!resdbwire::unpackSignedPacket(buf.data(), (uint32_t)buf.size(), &view)) return;
    if (view.resdbLen == 0) return;
    if (!CryptoAuth::instance().verifyBytes(view.pubKey, view.resdbBytes, view.resdbLen,
                                            view.sig, view.sigLen)) {
        std::cout << "[GOSSIP-RECV] r" << ctx_.replicaId_ << " dropped forged gossip from "
                  << bft->getFromReplicaId() << "\n";
        return;
    }

    uint32_t epoch;
    std::vector<uint8_t> order_bytes;
    if (!resdb_gossip::parse(view.resdbBytes, view.resdbLen, epoch, order_bytes)) return;

    if (isEpochTombstoned(epoch)) {
        std::cout << "[TYPE9-RECV] r" << ctx_.replicaId_
                  << " dropped tombstoned epoch=" << epoch << "\n";
        return;
    }

    // Epoch guard: drop stale gossip from a previous round.
    if (ctx_.has_committed_order_ && epoch <= ctx_.last_committed_epoch_) return;
    if (hasCompletedReplicaEpoch(ctx_.replicaId_, epoch)) {
        std::cout << "[TYPE9-RECV] r" << ctx_.replicaId_
                  << " epoch=" << epoch
                  << " already departed; skipping\n";
        return;
    }

    // Count this vote even if I've already applied my own order — same fix
    // as CANCEL-commit gossip: ctx_.order_applied_ becomes true for myself right
    // when I start my own retry loop, so if counting stopped here,
    // decisionGossipPropagationConfirmed() could never observe enough peers
    // to let that loop stop early.
    int f         = toleratedF();  // num_replicas_-based: counts intersection units
    int threshold = f + 1;
    bool reached  = gossip_acc_.add(bft->getFromReplicaId(), epoch, order_bytes, threshold);

    if (ctx_.order_applied_) {
        std::cout << "[TYPE9-RECV] r" << ctx_.replicaId_ << " from=" << bft->getFromReplicaId()
                  << " order_applied=true, skipping\n";
        return;
    }

    std::cout << "[GOSSIP-RECV] r" << ctx_.replicaId_ << " from=" << bft->getFromReplicaId()
              << " epoch=" << epoch
              << " count=" << gossip_acc_.count(epoch, order_bytes)
              << "/" << threshold << " t=" << simTime() << "\n";

    if (reached) applyGossipOrder(order_bytes, epoch);
}

bool ResDBIntersectionApp::applyGossipOrder(const std::vector<uint8_t>& order_bytes,
                                             uint32_t epoch)
{
    if (ctx_.order_applied_) return false;
    if (hasCompletedReplicaEpoch(ctx_.replicaId_, epoch)) {
        std::cout << "[GOSSIP-APPLY] r" << ctx_.replicaId_
                  << " refused completed epoch=" << epoch << "\n";
        return false;
    }
    if (isEpochTombstoned(epoch)) {
        std::cout << "[GOSSIP-APPLY] r" << ctx_.replicaId_
                  << " refused tombstoned epoch=" << epoch << "\n";
        return false;
    }
    std::cout << "[GOSSIP-APPLY] r" << ctx_.replicaId_
              << " epoch=" << epoch << " t=" << simTime() << "\n";
    // Push into the same queue onOrderDecided uses.  The 1 ms transport-poll
    // timer will call processOrders() and apply it on the next tick.
    {
        std::lock_guard<std::mutex> lk(orders_mutex_);
        pending_orders_.push_back(order_bytes);
    }
    ctx_.last_committed_epoch_ = epoch;
    ctx_.has_committed_order_ = true;
    committed_order_bytes_ = order_bytes;
    stopGossip();
    gossip_acc_.reset();
    cert_relay_tracker_.reset();
    announcement_relay_tracker_.reset();
    cancelPendingConsensusRelays("gossip-order-adopted");
    return true;
}

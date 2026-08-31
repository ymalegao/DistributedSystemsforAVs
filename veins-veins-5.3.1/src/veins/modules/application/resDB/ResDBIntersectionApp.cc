#include "veins/modules/application/resDB/ResDBIntersectionApp.h"
#include "veins/modules/application/resDB/ResDBUtil.h"
#include "veins/modules/application/resDB/sinr/ChannelMetrics.h"
#include "veins/modules/application/resDB/messages/BFTMessage_m.h"
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

#include "veins/modules/application/resDB/ResdbV2VWire.h"
#include "veins/modules/application/resDB/ResDBDecisionGossip.h"

using namespace veins;
using namespace veins::resdb_app_util;

namespace {
int bftQuorumSize(int n, int f)
{
    if (n <= 0 || f < 0 || f > (n - 1) / 3) return -1;
    return std::max((n + f + 2) / 2, 1);
}

std::mutex g_completed_replica_epochs_mu;
std::set<std::pair<int, uint32_t>> g_completed_replica_epochs;

}  // namespace

Define_Module(veins::ResDBIntersectionApp);

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
    deleteTimer(arrival_cert_finalize_timer_);
    deleteTimer(stopped_distance_finalize_timer_);
    deleteTimer(stopped_distance_attestation_retry_timer_);
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
    if (resdb_server_handle_) {
        ResdbOmnetDestroyServer(resdb_server_handle_);
        resdb_server_handle_ = nullptr;
    }
    if (ec_private_key_) {
        EVP_PKEY_free(ec_private_key_);
        ec_private_key_ = nullptr;
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
        total_vehicles_        = par("totalVehicles").intValue();
        stop_distance_ = stop_distance_ * (total_vehicles_ / 2);

        replicaId_ = par("replicaId").intValue();
        const int ned_replica_id = replicaId_;
        // Replica id must follow the SUMO vehicle this host
        // controls (mobility externalId), not OMNeT node index. Otherwise TraCI
        // uses veh<NED> while the module actually drives veh<externalId> — wrong
        // lane/position in proposals and crashes despite a correct OrderDecision.
        if (mobility) {
            try {
                const std::string sumoId = mobility->getExternalId();
                if (sumoId.size() > 3 && sumoId.compare(0, 3, "veh") == 0) {
                    const int from_sumo = std::stoi(sumoId.substr(3));
                    if (from_sumo >= 0 && from_sumo < total_vehicles_) {
                        if (from_sumo != ned_replica_id) {
                            std::cout << "[IDENTITY BINDING] NED replicaId=" << ned_replica_id
                                      << " overridden by SUMO externalId=" << sumoId
                                      << " -> replicaId_=" << from_sumo << "\n";
                        }
                        replicaId_ = from_sumo;
                    }
                }
            } catch (const std::exception& e) {
                std::cerr << "[IDENTITY BINDING] keep NED replicaId=" << replicaId_
                          << " (SUMO bind failed: " << e.what() << ")\n";
            } catch (...) {
                std::cerr << "[IDENTITY BINDING] keep NED replicaId=" << replicaId_
                          << " (SUMO bind failed)\n";
            }
        }

        useRadioTransport_        = par("useRadioTransport").boolValue();
        transport_poll_interval_  = par("transportPollInterval").doubleValue();
        enable_sim_time_provider_ = par("enableSimTimeProvider").boolValue();
        time_tick_interval_       = par("timeTickInterval").doubleValue();
        broadcast_arrival_announcement_interval_ = par("broadcastArrivalAnnouncementIntervalSec").doubleValue();
        const double cert_timeout_base_sec = par("certCollectionTimeoutSec").doubleValue();
        const double cert_timeout_scale_sec = std::floor(static_cast<double>(total_vehicles_) / 5.0);
        cert_collection_timeout_ = SimTime(cert_timeout_base_sec + cert_timeout_scale_sec);
        discovery_intent_settle_ = par("discoveryIntentSettleSec").doubleValue();

        const double view_change_timeout_sec = par("pbftVcTimeoutSec").doubleValue();
        const double view_change_timeout_scale_sec = std::floor(static_cast<double>(total_vehicles_) / 5.0);
        pbft_vc_timeout_sec_ = (view_change_timeout_sec + view_change_timeout_scale_sec);

        debug_cert_protocol_    = par("debugCertProtocol").boolValue();
        debug_order_delivery_ = par("debugOrderDelivery").boolValue();
        enable_cert_retries_    = par("enableArrivalCertRetries").boolValue();
        cert_retry_interval_    = par("arrivalCertRetryIntervalSec").doubleValue();
        cert_retry_max_         = par("arrivalCertRetryMax").intValue();
        tolerated_faults_       = par("toleratedFaults").intValue();
        configured_consensus_quorum_ = tolerated_faults_ >= 0
            ? bftQuorumSize(total_vehicles_, tolerated_faults_)
            : -1;
        configured_cert_threshold_ = tolerated_faults_ >= 0
            ? (tolerated_faults_ + 1)
            : -1;
        if (tolerated_faults_ >= 0) {
            std::cout << "[TOLERATED-F-APP] r" << replicaId_
                      << " tolerated_f=" << tolerated_faults_
                      << " static_n=" << total_vehicles_
                      << " quorum=" << configured_consensus_quorum_
                      << " cert_threshold=" << configured_cert_threshold_;
            std::cout << "\n";
        }

        gossip_enabled_          = par("enableDecisionGossip").boolValue();
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
        byzantine_cert_relay_silent_ = par("byzantineCertRelaySilent").boolValue();
        if (byzantine_cert_relay_silent_) {
            std::cout << "[CERT-RELAY-MODE] r" << replicaId_
                      << " withholding=1 isByzantine=" << (is_byzantine_ ? 1 : 0)
                      << "\n";
        }
        enableAmbulanceCertGate_ = par("enableAmbulanceCertGate").boolValue();
        direction_eligibility_collection_window_sec_ =
            par("directionEligibilityCollectionWindowSec").doubleValue();
        direction_eligibility_enabled_ =
            par("enableDirectionEligibility").boolValue();
        distance_stationary_speed_mps_ =
            par("distanceStationarySpeedMps").doubleValue();
        stopped_distance_attestation_retry_interval_sec_ =
            par("stoppedDistanceAttestationRetryIntervalSec").doubleValue();
        stopped_distance_attestation_retry_max_ =
            par("stoppedDistanceAttestationRetryMax").intValue();
        enable_phase2_cue_trace_ = par("enablePhase2CueTrace").boolValue();
        enable_phase2_controlled_cue_ = par("enablePhase2ControlledCue").boolValue();
        if (direction_eligibility_collection_window_sec_ < 0.0)
            throw cRuntimeError("directionEligibilityCollectionWindowSec must be non-negative");
        if (stopped_distance_attestation_retry_interval_sec_ <= 0.0 ||
                stopped_distance_attestation_retry_max_ < 0)
            throw cRuntimeError("stopped-distance attestation retry settings are invalid");
        for (const auto& token : splitStr(par("falseLaneColluderIds").stdstringValue(), ',')) {
            if (token.empty()) continue;
            try {
                const int id = std::stoi(token);
                if (id >= 0 && id < total_vehicles_) false_lane_colluder_ids_.insert(id);
            } catch (...) {
                std::cout << "[FALSE-LANE-COLLUDER-SET] r" << replicaId_
                          << " ignored_invalid_token=" << token << "\n";
            }
        }
        std::cout << "[FALSE-LANE-COLLUDER-SET] r" << replicaId_
                  << " N=" << total_vehicles_
                  << " F=" << false_lane_colluder_ids_.size()
                  << " ids=";
        bool firstColluder = true;
        for (int id : false_lane_colluder_ids_) {
            if (!firstColluder) std::cout << ",";
            std::cout << id;
            firstColluder = false;
        }
        std::cout << " perception_gate=1\n";
        const std::string phase2AttackKind = par("phase2AttackKind").stdstringValue();
        if (phase2AttackKind == "NONE") {
            phase2_attack_kind_ = Phase2AttackKind::NONE;
        } else if (phase2AttackKind == "WRONG_APPROACH") {
            phase2_attack_kind_ = Phase2AttackKind::WRONG_APPROACH;
        } else if (phase2AttackKind == "FALSE_PHYSICAL_LANE") {
            phase2_attack_kind_ = Phase2AttackKind::FALSE_PHYSICAL_LANE;
        } else if (phase2AttackKind == "FALSE_DIRECTION") {
            phase2_attack_kind_ = Phase2AttackKind::FALSE_DIRECTION;
        } else if (phase2AttackKind == "FALSE_DISTANCE") {
            phase2_attack_kind_ = Phase2AttackKind::FALSE_DISTANCE;
        } else {
            throw cRuntimeError("invalid phase2AttackKind '%s'", phase2AttackKind.c_str());
        }
        phase2_attack_target_replica_id_ = par("phase2AttackTargetReplicaId").intValue();
        phase2_actual_byzantine_count_ = par("phase2ActualByzantineCount").intValue();
        phase2_distance_claim_offset_m_ =
            par("phase2DistanceClaimOffsetM").doubleValue();
        phase2_lateral_claim_offset_m_ =
            par("phase2LateralClaimOffsetM").doubleValue();
        const std::string laneMode = par("laneObservationMode").stdstringValue();
        if (laneMode == "CATEGORICAL_CARDINAL") {
            lane_observation_mode_ = LaneObservationMode::CATEGORICAL_CARDINAL;
        } else if (laneMode == "ADJACENT_LATERAL") {
            lane_observation_mode_ = LaneObservationMode::ADJACENT_LATERAL;
        } else {
            throw cRuntimeError("invalid laneObservationMode '%s'", laneMode.c_str());
        }
        for (const auto& token : splitStr(par("phase2EvidenceColluderIds").stdstringValue(), ',')) {
            if (token.empty()) continue;
            try {
                const int id = std::stoi(token);
                if (id < 0 || id >= total_vehicles_ ||
                        id == phase2_attack_target_replica_id_)
                    throw cRuntimeError("invalid Phase 2 evidence colluder id %d", id);
                phase2_evidence_colluder_ids_.insert(id);
            } catch (const cRuntimeError&) {
                throw;
            } catch (...) {
                throw cRuntimeError("invalid phase2EvidenceColluderIds token '%s'", token.c_str());
            }
        }
        if (phase2_attack_kind_ == Phase2AttackKind::NONE) {
            if (phase2_actual_byzantine_count_ != 0 ||
                    !phase2_evidence_colluder_ids_.empty())
                throw cRuntimeError("Phase 2 NONE attack requires b=0 and no colluders");
            if (phase2_distance_claim_offset_m_ != 0.0)
                throw cRuntimeError("Phase 2 NONE attack requires zero distance offset");
            if (phase2_lateral_claim_offset_m_ != 0.0)
                throw cRuntimeError("Phase 2 NONE attack requires zero lateral offset");
        } else {
            if (phase2_attack_target_replica_id_ < 0 ||
                    phase2_attack_target_replica_id_ >= total_vehicles_)
                throw cRuntimeError("Phase 2 attack target is outside configured replicas");
            phase2_byzantine_replica_ids_.insert(phase2_attack_target_replica_id_);
            phase2_byzantine_replica_ids_.insert(
                phase2_evidence_colluder_ids_.begin(), phase2_evidence_colluder_ids_.end());
            if (phase2_actual_byzantine_count_ !=
                    static_cast<int>(phase2_byzantine_replica_ids_.size()))
                throw cRuntimeError("Phase 2 actual Byzantine count does not match target+colluders");
            if (phase2_attack_kind_ != Phase2AttackKind::FALSE_DISTANCE &&
                    phase2_distance_claim_offset_m_ != 0.0)
                throw cRuntimeError("distance claim offset requires FALSE_DISTANCE attack");
            if (phase2_attack_kind_ != Phase2AttackKind::FALSE_PHYSICAL_LANE &&
                    phase2_lateral_claim_offset_m_ != 0.0)
                throw cRuntimeError("lateral claim offset requires FALSE_PHYSICAL_LANE attack");
            if (phase2_attack_kind_ == Phase2AttackKind::FALSE_PHYSICAL_LANE &&
                    lane_observation_mode_ != LaneObservationMode::ADJACENT_LATERAL)
                throw cRuntimeError("FALSE_PHYSICAL_LANE requires ADJACENT_LATERAL mode");
        }
        std::cout << "[PHASE2-ATTACK-CONFIG] replica=" << replicaId_
                  << " kind=" << phase2AttackKindName()
                  << " target=" << phase2_attack_target_replica_id_
                  << " actualB=" << phase2_actual_byzantine_count_
                  << " colluders=";
        bool firstPhase2Colluder = true;
        for (int id : phase2_evidence_colluder_ids_) {
            if (!firstPhase2Colluder) std::cout << ",";
            std::cout << id;
            firstPhase2Colluder = false;
        }
        std::cout << " policyTiming=INIT_BEFORE_PERCEPTION\n";
        std::cout << "[DIRECTION-ABLATION-CONFIG] replica=" << replicaId_
                  << " eligibility="
                  << (direction_eligibility_enabled_ ? "ON" : "OFF") << "\n";
        if (phase2_attack_kind_ == Phase2AttackKind::FALSE_DISTANCE)
            std::cout << "[DIST-ATTACK-CONFIG] replica=" << replicaId_
                      << " target=" << phase2_attack_target_replica_id_
                      << " offsetM=" << phase2_distance_claim_offset_m_ << "\n";
        if (lane_observation_mode_ == LaneObservationMode::ADJACENT_LATERAL)
            std::cout << "[LATERAL-GATE-CONFIG] replica=" << replicaId_
                      << " sigma=" << par("lateralObservationSigmaM").doubleValue()
                      << " k=" << par("physicalGateK").doubleValue()
                      << " origin=" << par("adjacentLateralOriginX").doubleValue()
                      << "," << par("adjacentLateralOriginY").doubleValue()
                      << " normal=" << par("adjacentLateralNormalX").doubleValue()
                      << "," << par("adjacentLateralNormalY").doubleValue()
                      << " separation=" << par("adjacentLaneSeparationM").doubleValue()
                      << " attackOffset=" << phase2_lateral_claim_offset_m_ << "\n";
        enableRollback_ = par("enableRollback").boolValue();
        crash_mac_grace_sec_ = par("crashMacGraceSec").doubleValue();
        crash_dwell_sec_ = par("crashDwellSec").doubleValue();
        crash_speed_eps_ = par("crashSpeedEps").doubleValue();
        enable_noisy_crash_perception_ = par("enableNoisyCrashPerception").boolValue();
        enable_occupancy_perception_trace_ =
            par("enableOccupancyPerceptionTrace").boolValue();
        suppress_crash_blocked_echo_ = par("suppressCrashBlockedEcho").boolValue();
        inject_forged_crash_blocked_echo_ =
            par("injectForgedCrashBlockedEcho").boolValue();
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
        std::cout << "[TYPE11-CONFIG] r" << replicaId_
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
        fabricated_clearance_leader_replica_id_ =
            par("fabricatedClearanceLeaderReplicaId").intValue();
        enable_recovery_clear_evidence_gate_ =
            par("enableRecoveryClearEvidenceGate").boolValue();
        std::cout << "[CANCEL-LEADER-CONFIG] r" << replicaId_
                  << " inject_suppress_initial="
                  << (inject_suppress_initial_cancel_leader_ ? 1 : 0)
                  << " failover=" << (enable_cancel_leader_failover_ ? 1 : 0)
                  << " timeout=" << rollback_vc_timeout_sec_ << "\n";
        std::cout << "[FABRICATED-CLEARANCE-CONFIG] r" << replicaId_
                  << " inject="
                  << (inject_fabricated_clearance_leader_ ? 1 : 0)
                  << " attack_proposer=r" << fabricated_clearance_leader_replica_id_
                  << " is_attack_proposer="
                  << (replicaId_ == fabricated_clearance_leader_replica_id_ ? 1 : 0)
                  << " evidence_gate="
                  << (enable_recovery_clear_evidence_gate_ ? 1 : 0)
                  << "\n";
        std::cout << "[OCCUPANCY-CONFIG] r" << replicaId_
                  << " enabled=" << (enable_noisy_crash_perception_ ? 1 : 0)
                  << " sigma_lon=" << par("longitudinalObservationSigmaM").doubleValue()
                  << " blocked_dwell=" << crash_dwell_sec_
                  << " clear_dwell=" << clear_dwell_sec_
                  << " trace=" << (enable_occupancy_perception_trace_ ? 1 : 0)
                  << " suppress_blocked=" << (suppress_crash_blocked_echo_ ? 1 : 0)
                  << " forge_blocked=" << (inject_forged_crash_blocked_echo_ ? 1 : 0)
                  << "\n";
        {
            std::string rollbackMode = par("rollbackFaultMode").stdstringValue();
            std::transform(rollbackMode.begin(), rollbackMode.end(),
                           rollbackMode.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            rollback_fault_mode_per_epoch_ = (rollbackMode != "anchored");
            std::cout << "[ROLLBACK-MODE] r" << replicaId_
                      << " mode="
                      << (rollback_fault_mode_per_epoch_ ? "per_epoch" : "anchored")
                      << "\n";
        }

        const int ambulance_replica_id = par("ambulanceReplicaId").intValue();
        if (ambulance_replica_id >= 0) {
            is_ambulance_ = (replicaId_ == ambulance_replica_id);

            std::cout << "[AMBULANCE BINDING] r" << replicaId_
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
            std::cout << "[AMBULANCE] r" << replicaId_
                      << " auto-issued Emergency_CA cert ("
                      << my_ambulance_cert_bytes_.size() << " bytes)\n";
        }

        std::string crypto_dir = par("resdbCryptoDir").stdstringValue();
        config_file_      = par("resdbConfigFile").stdstringValue();
        private_key_file_ = par("resdbPrivateKeyFile").stdstringValue();
        cert_file_        = par("resdbCertFile").stdstringValue();
        log_dir_          = par("resdbLogDir").stdstringValue();
        if (!crypto_dir.empty() && config_file_.empty()) {
            int node_id = replicaId_ + 1;
            config_file_      = crypto_dir + "/server.config";
            private_key_file_ = crypto_dir + "/node" + std::to_string(node_id) + ".key.pri";
            cert_file_        = crypto_dir + "/cert_"  + std::to_string(node_id) + ".cert";
            log_dir_          = crypto_dir + "/logs";
        }

        ec_private_key_ = CryptoAuth::instance().generateKeyPair(ec_pub_key_);
        // WitnessKeyRegistry is a process-global singleton; the orchestrator runs
        // each repetition as its own subprocess (experiment_orchestrator.py calls
        // subprocess.run per rep), so the registry always starts empty here — no
        // explicit reset needed (resetForNewRun() remains available for a future
        // in-process multi-run harness).
        if (!WitnessKeyRegistry::instance().registerKey(replicaId_, ec_pub_key_)) {
            std::cout << "[WITNESS-KEY-CONFLICT] r" << replicaId_
                      << " registration rejected — stale registry from a prior run?\n";
        } else {
            std::cout << "[WITNESS-KEY-REG] r" << replicaId_ << " registered\n";
        }

        if (useRadioTransport_) {
            transport_ = std::make_unique<VeinsTransport>(this);
            transport_poll_msg_ = new cMessage("resdbTransportPoll");
            scheduleAt(simTime() + transport_poll_interval_, transport_poll_msg_);
        } else {
            transport_ = std::make_unique<LoggingTransport>(replicaId_);
        }

        if (config_file_.empty()) {
            resdb_server_handle_ = ResdbOmnetCreateNullHandle();
        } else {
            resdb_server_handle_ = ResdbOmnetCreateKvServer(
                &config_file_[0],
                private_key_file_.empty() ? nullptr : &private_key_file_[0],
                cert_file_.empty()        ? nullptr : &cert_file_[0],
                log_dir_.empty()          ? nullptr : &log_dir_[0]);
            if (!resdb_server_handle_)
                resdb_server_handle_ = ResdbOmnetCreateNullHandle();
        }

        if (tolerated_faults_ >= 0) {
            int rc = ResdbOmnetSetToleratedFaults(
                resdb_server_handle_, tolerated_faults_);
            std::cout << "[TOLERATED-F-APP] r" << replicaId_
                      << " bridge_set_rc=" << rc << "\n";
        }
        {
            int rc = ResdbOmnetSetRollbackFaultMode(
                resdb_server_handle_, rollback_fault_mode_per_epoch_ ? 1 : 0);
            if (rc != 0) {
                std::cout << "[ROLLBACK-MODE] r" << replicaId_
                          << " bridge_set_rc=" << rc << "\n";
            }
        }

        registerTransport();
        ResdbOmnetSetOrderCallback(resdb_server_handle_,
                                   &ResDBIntersectionApp::onOrderDecided, this);
        ResdbOmnetSetClearEvidenceCallback(resdb_server_handle_,
                                           &ResDBIntersectionApp::clearEvidenceCallback, this);
        ResdbOmnetSetRecoveryRejectCallback(resdb_server_handle_,
                                            &ResDBIntersectionApp::recoveryRejectCallback, this);
        ResdbOmnetSetCertSnapshotFn(resdb_server_handle_,
                                    &ResDBIntersectionApp::certSnapshotCallback, this);
        ResdbOmnetSetVcTimeoutUs(resdb_server_handle_,
                                 static_cast<int64_t>(pbft_vc_timeout_sec_ * 1e6));
        last_known_primary_ = ResdbOmnetGetPrimary(resdb_server_handle_);
        std::cout << "[VC-INIT] r" << replicaId_
                  << " vc_timeout_sec=" << pbft_vc_timeout_sec_
                  << " stop_sign_timeout_sec=" << stop_sign_timeout_sec_
                  << " consensus_timeout_sec=" << consensus_timeout_sec_
                  << " initial_primary=" << last_known_primary_ << "\n";

        if (enable_sim_time_provider_) {
            time_tick_msg_ = new cMessage("resdbTimeTick");
            scheduleAt(simTime() + time_tick_interval_, time_tick_msg_);
            ResdbOmnetUpdateSimTimeUs(resdb_server_handle_, simTime().inUnit(SIMTIME_US));
        }

        ResdbOmnetRunServer(resdb_server_handle_);

        // Full communicator omission is a separate fault from SILENT_PRIMARY.
        // A proposal-silent primary omits proposeAll() but must still participate
        // in view-change so consecutive faulty primaries can be replaced.  Only
        // the explicit byzantinePbftSilent mode drops all outbound PBFT traffic.
        if (is_byzantine_ && byzantine_pbft_silent_) {
            ResdbOmnetSetPbftSilent(resdb_server_handle_, 1);
            std::cout << "[BYZANTINE] r" << replicaId_
                      << " PBFT_SILENT: communicator silenced"
                      << " type=" << static_cast<int>(byzantine_type_)
                      << " explicit=" << (byzantine_pbft_silent_ ? 1 : 0)
                      << "\n";
        }

        discovery_deadline_msg_ = new cMessage("resdbDiscoveryDeadline");
        discovery_settle_msg_ = new cMessage("resdbDiscoverySettle");
        arrival_cert_finalize_timer_ = new cMessage("resdbArrivalCertFinalize");
        stopped_distance_finalize_timer_ = new cMessage("resdbStoppedDistanceFinalize");
        stopped_distance_attestation_retry_timer_ =
            new cMessage("resdbStoppedDistanceAttestationRetry");
        consensus_retry_timer_ = new cMessage("resdbConsensusRetry");
        consensus_relay_timer_ = new cMessage("resdbConsensusRelay");
    }

    if (stage == 1) {
        perception_ = std::make_unique<ResDBPerception>();
        perception_->configure(
            mobility,
            getRNG(par("perceptionRngIndex").intValue()),
            par("approachConfusionMatrix").stdstringValue(),
            par("approachSigmaM").doubleValue(),
            par("signalObservationError").doubleValue(),
            par("lateralObservationSigmaM").doubleValue(),
            par("longitudinalObservationSigmaM").doubleValue(),
            lane_observation_mode_ == LaneObservationMode::ADJACENT_LATERAL,
            par("adjacentLateralOriginX").doubleValue(),
            par("adjacentLateralOriginY").doubleValue(),
            par("adjacentLateralNormalX").doubleValue(),
            par("adjacentLateralNormalY").doubleValue(),
            par("adjacentLaneSeparationM").doubleValue());
        std::cout << "[PERCEPTION-CONFIG] r" << replicaId_
                  << " sigma=" << par("approachSigmaM").doubleValue()
                  << " signal_error=" << par("signalObservationError").doubleValue()
                  << " ego_lat_sigma=" << par("egoLateralSigmaM").doubleValue()
                  << " ego_lon_sigma=" << par("egoLongitudinalSigmaM").doubleValue()
                  << " witness_lat_sigma=" << par("lateralObservationSigmaM").doubleValue()
                  << " witness_lon_sigma=" << par("longitudinalObservationSigmaM").doubleValue()
                  << " gate_k=" << par("physicalGateK").doubleValue()
                  << " rng=" << par("perceptionRngIndex").intValue()
                  << " collection_window=" << direction_eligibility_collection_window_sec_
                  << "\n";
        startDiscoveryRound("initial-approach");
        if (par("smokeTestBroadcast").boolValue()) {
            smoke_test_msg_ = new cMessage("resdbSmokeTest");
            scheduleAt(simTime() + 0.05, smoke_test_msg_);
        }
        // Staggered one-shot arrival announcement.
        initial_announce_msg_ = new cMessage("resdbInitialAnnounce");
        scheduleAt(simTime() + trigger_join_time_ + replicaId_ * arrival_slot_sec_,
                   initial_announce_msg_);

        // // Periodic re-announce fallback (in case peers miss the initial broadcast).
        // state_announce_msg_ = new cMessage("resdbStateAnnounce");
        // scheduleAt(simTime() + state_announce_interval_, state_announce_msg_);

        broadcastArrivalAnnouncement_timer_ = new cMessage("resdbBroadcastArrivalAnnouncement");
        scheduleAt(simTime() + broadcast_arrival_announcement_interval_, broadcastArrivalAnnouncement_timer_);

        // Metrics apply to every instantiated vehicle, including dynamic
        // recovery members.  total_vehicles_ is the epoch-0 protocol boundary
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
            std::string csvPath  = dir + "channel_V" + std::to_string(replicaId_) + ".csv";
            std::string sinrPath = dir + "sinr_V" + std::to_string(replicaId_) + ".csv";
            channel_metrics_ = new ChannelMetrics(replicaId_, csvPath, sinrPath);
            cModule* nic = getParentModule()->getSubmodule("nic");
            cModule* mac = nic ? nic->getSubmodule("mac1609_4") : nullptr;
            if (mac) {
                mac->subscribe(Mac1609_4::sigChannelBusy, channel_metrics_);
                mac->subscribe(Mac1609_4::sigCollision, channel_metrics_);
            } else
                std::cerr << "[ChannelMetrics] r" << replicaId_ << " no nic/mac1609_4 — utilization stays 0\n";
            channel_metrics_timer_ = new cMessage("channelMetricsTick");
            scheduleAt(simTime() + 0.1, channel_metrics_timer_);
        }
    }
}

// ── handleSelfMsg ─────────────────────────────────────────────────────────────

void ResDBIntersectionApp::handleSelfMsg(cMessage* msg)
{
    if (msg == crash_mac_grace_msg_) {
        crash_mac_grace_msg_ = nullptr;
        std::cout << "[CRASH-COMMS-DEAD] r" << replicaId_
                  << " t=" << simTime()
                  << " grace_sec=" << crash_mac_grace_sec_ << "\n";
        delete msg;
        return;
    }

    if (msg == channel_metrics_timer_) {
        if (channel_metrics_)
            channel_metrics_->tick(simTime());
        scheduleAt(simTime() + 0.1, channel_metrics_timer_);
        return;
    }

    if (msg == smoke_test_msg_) {
        smoke_test_msg_ = nullptr;
        const uint8_t probe[] = {'R','E','S','D','B','T','S','T'};
        ResdbOmnetTestBroadcast(resdb_server_handle_, probe, sizeof(probe));
        delete msg; return;
    }

    if (msg == transport_poll_msg_) {
        drainOutboundQueue();
        processOrders();
        consumeRecoveryReject();
        // Detect primary change after view-change.
        if (resdb_server_handle_) {
            int current_primary = ResdbOmnetGetPrimary(resdb_server_handle_);
            if (current_primary != last_known_primary_) {
                std::cout << "[VC-DEBUG] r" << replicaId_
                          << " primary changed: " << last_known_primary_
                          << " -> " << current_primary
                          << " t=" << simTime()
                          << " phase=" << phaseToStr(current_phase_)
                          << " propose_submitted=" << propose_submitted_
                          << " order_applied=" << order_applied_ << "\n";
                last_known_primary_ = current_primary;
                const int initial_order_primary = order_candidate_
                    ? order_candidate_->initialPrimary : CertPrimary();
                if (order_vc_requested_ ||
                        (discovery_.state == DiscoveryState::COMPLETE &&
                         current_primary != initial_order_primary)) {
                    order_vc_authoritative_ = true;
                }
                // A completed view change consumes the previous one-shot
                // suspicion timer. Every honest follower must arm a fresh
                // timeout for the newly installed primary; otherwise a second
                // consecutive silent primary stalls until the independent
                // stop-sign fallback releases vehicles outside consensus.
                if (current_primary != replicaId_ && !order_applied_ &&
                        discovery_.state == DiscoveryState::COMPLETE &&
                        current_phase_ != ConsensusPhase::DEPARTED) {
                    armOrderSuspicionTimer("primary-change");
                }
                const bool primary_has_frozen_cert = order_candidate_ &&
                    std::binary_search(order_candidate_->proposerIds.begin(),
                                       order_candidate_->proposerIds.end(),
                                       current_primary);
                if (discovery_.state == DiscoveryState::COMPLETE &&
                        order_candidate_ && !primary_has_frozen_cert &&
                        !order_applied_ &&
                        current_phase_ != ConsensusPhase::DEPARTED) {
                    std::cout << "[CERT-PRIMARY-SKIP] r" << replicaId_
                              << " epoch=" << current_epoch_
                              << " uncertified_primary=r" << current_primary
                              << " eligible=";
                    for (size_t i = 0; i < order_candidate_->proposerIds.size(); ++i) {
                        if (i) std::cout << ",";
                        std::cout << "r" << order_candidate_->proposerIds[i];
                    }
                    std::cout << " action=force-next-pbft-view"
                              << " t=" << simTime() << "\n";
                    order_vc_requested_ = true;
                    const int vcRc = ResdbOmnetForceViewChange(resdb_server_handle_);
                    std::cout << "[APP-VC] r" << replicaId_
                              << " uncertified-primary-skip rc=" << vcRc
                              << " t=" << simTime() << "\n";
                    if (vcRc == 1) {
                        // The next transport poll reevaluates this same
                        // uncertified primary, so no additional timer or
                        // transmission source is needed here.
                        order_vc_requested_ = false;
                    }
                    scheduleAt(simTime() + transport_poll_interval_, transport_poll_msg_);
                    return;
                }
                if (current_primary == replicaId_ && !order_applied_ &&
                    discovery_.state == DiscoveryState::COMPLETE &&
                    current_phase_ != ConsensusPhase::DEPARTED) {
                    std::cout << "[VC-DEBUG] r" << replicaId_
                              << " became primary, evaluating ORDER in phase="
                              << phaseToStr(current_phase_) << " t=" << simTime() << "\n";
                    evaluateOrderReadiness("primary-change");
                } else if (current_primary == replicaId_) {
                    std::cout << "[VC-DEBUG] r" << replicaId_
                              << " became primary but skipped re-propose"
                              << " propose_submitted=" << propose_submitted_
                              << " order_applied=" << order_applied_
                              << " phase=" << phaseToStr(current_phase_) << " t=" << simTime() << "\n";
                }
            }
        }
        scheduleAt(simTime() + transport_poll_interval_, transport_poll_msg_);
        return;
    }

    if (msg == time_tick_msg_) {
        if (enable_sim_time_provider_)
            ResdbOmnetUpdateSimTimeUs(resdb_server_handle_, simTime().inUnit(SIMTIME_US));
        scheduleAt(simTime() + time_tick_interval_, time_tick_msg_);
        return;
    }

    if (msg == consensus_retry_timer_) {
        retryConsensusPackets();
        return;
    }

    if (msg == consensus_relay_timer_) {
        flushDueConsensusRelays();
        return;
    }

    if (msg == cancel_drain_timer_) {
        cancel_drain_timer_ = nullptr;
        finishCancelDrain();
        delete msg; return;
    }

    if (msg == initial_announce_msg_) {
        initial_announce_msg_ = nullptr;
        broadcastArrivalAnnouncement();
        delete msg; return;
    }

    if (msg == broadcastArrivalAnnouncement_timer_) {
        // Periodic self-message: never delete after scheduleAt() — the same cMessage must stay owned
        // by the FES until cancelled (see transport_poll_msg_ / time_tick_msg_ above).
        // Keep re-announcing until cert is assembled: a car may have observed all peers yet still
        // lack f+1 echoes, so witnesses need continued re-announces to trigger re-echoes.
        if (cancel_pending_ && !rollback_local_recallable_) {
            std::cout << "[DISCOVERY-VIEW] r" << replicaId_
                      << " suppress periodic announce; local non-recallable"
                      << " cancelled_epoch=" << cancelled_epoch_
                      << " new_epoch=" << rollback_new_epoch_ << "\n";
            broadcastArrivalAnnouncement_timer_ = nullptr;
            delete msg;
            return;
        }
        if (!cert_broadcast_ && discovery_.state == DiscoveryState::COLLECTING) {
            broadcastArrivalAnnouncement();
            scheduleAt(simTime() + broadcast_arrival_announcement_interval_, broadcastArrivalAnnouncement_timer_);
            std::cout << "[ANN-SEND] Replica " << replicaId_ << " rescheduled arrival-announcement timer\n";
        } else {
            std::cout << "[ANN-SEND-STOP] r" << replicaId_
                      << " discovery_state=" << discoveryStateName()
                      << " cert_broadcast=" << (cert_broadcast_ ? 1 : 0)
                      << " order_applied=" << (order_applied_ ? 1 : 0)
                      << " propose_submitted=" << (propose_submitted_ ? 1 : 0)
                      << " t=" << simTime() << "\n";
            broadcastArrivalAnnouncement_timer_ = nullptr;
            delete msg;
        }
        return;
    }

    if (msg == cert_retry_timer_) {
        if (cert_pending_retries_.carId.empty() ||
                discovery_.state == DiscoveryState::INACTIVE ||
                discovery_.state == DiscoveryState::COMPLETE ||
                (discovery_.state == DiscoveryState::DRAINING_CERTS &&
                 discovery_.localCertAired())) {
            std::cout << "[CERT-RETX-STOP] r" << replicaId_
                      << " carId=" << cert_pending_retries_.carId
                      << " propose_submitted=" << (propose_submitted_ ? 1 : 0)
                      << " order_applied=" << (order_applied_ ? 1 : 0)
                      << " discovery_state=" << discoveryStateName()
                      << " local_cert_aired=" << (discovery_.localCertAired() ? 1 : 0)
                      << " t=" << simTime() << "\n";
            stopCertBroadcastRetries();
            return;
        }
        cert_retry_count_++;
        sendBFTMessage(-1, serializeArrivalCert(cert_pending_retries_), kArrivalCertType, true);
        std::cout << "[CERT-RETX] Replica " << replicaId_ << " ARRIVAL_CERT " << cert_pending_retries_.carId
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

    if (msg == arrival_cert_finalize_timer_) {
        finalizeLocalArrivalCert("post-threshold-window");
        return;
    }

    if (msg == stopped_distance_finalize_timer_) {
        finalizeLocalStoppedDistanceCert("post-threshold-window");
        return;
    }

    if (msg == stopped_distance_attestation_retry_timer_) {
        retryStoppedDistanceAttestation();
        return;
    }

    if (msg == discovery_tx_flush_timer_) {
        flushDueDiscoveryTxs();
        return;
    }

    if (msg == cert_gossip_timer_) {
        if (discovery_.state != DiscoveryState::COLLECTING || CertPrimary() != replicaId_ ||
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

    if (msg == gossip_timer_) {
        if (gossip_order_bytes_.empty()) return;
        if (isEpochTombstoned(gossip_epoch_) ||
                (cancel_consensus_pending_ && gossip_epoch_ == cancelled_epoch_)) {
            std::cout << "[GOSSIP-STOP] r" << replicaId_
                      << " epoch=" << gossip_epoch_
                      << " reason=cancel-active t=" << simTime() << "\n";
            stopGossip();
            return;
        }
        if (decisionGossipPropagationConfirmed()) {
            std::cout << "[GOSSIP-STOP] r" << replicaId_
                      << " epoch=" << gossip_epoch_
                      << " reason=propagation-confirmed t=" << simTime() << "\n";
            stopGossip();
            return;
        }
        auto inner  = resdb_gossip::serialize(gossip_epoch_, gossip_order_bytes_);
        auto signed_payload = resdbwire::packSignedPacket(
            ec_private_key_, ec_pub_key_, inner.data(), (uint32_t)inner.size());
        if (!signed_payload.empty()) {
            sendBFTMessage(-1, signed_payload, kDecisionGossipType);
            std::cout << "[GOSSIP-SEND] r" << replicaId_ << " epoch=" << gossip_epoch_
                      << " retry=" << gossip_retry_count_ << " t=" << simTime() << "\n";
        }
        gossip_retry_count_++;
        scheduleNextGossip();
        return;
    }

    if (msg == cancel_gossip_timer_) {
        if (cancel_gossip_bytes_.empty()) {
            cancel_gossip_timer_ = nullptr;
            delete msg;
            return;
        }
        if (cancelGossipPropagationConfirmed()) {
            std::cout << "[CANCEL-GOSSIP-STOP] r" << replicaId_
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
            ec_private_key_, ec_pub_key_, inner.data(), (uint32_t)inner.size());
        if (!signed_payload.empty()) {
            sendBFTMessage(-1, signed_payload, kCancelCommitGossipType);
            std::cout << "[CANCEL-GOSSIP-SEND] r" << replicaId_
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

    if (msg == cancel_cert_retry_timer_) {
        if (cancel_cert_pending_retries_.echoes.empty() ||
                (!cancel_consensus_pending_ && !cancel_pending_)) {
            std::cout << "[CANCEL-CERT-RETX-STOP] r" << replicaId_
                      << " cancel_consensus_pending=" << (cancel_consensus_pending_ ? 1 : 0)
                      << " cancel_pending=" << (cancel_pending_ ? 1 : 0)
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
                std::cout << "[CANCEL-CERT-STOP] r" << replicaId_
                          << " key=" << key << " reason=propagation-confirmed\n";
                stopCancelCertRetries();
                return;
            }
        }
        cancel_cert_retry_count_++;
        sendBFTMessage(-1, serializeCancelCert(cancel_cert_pending_retries_), kCancelCertType);
        std::cout << "[CANCEL-CERT-RETX] r" << replicaId_
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

    if (msg == clear_cert_retry_timer_) {
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
        std::cout << "[CLEAR-CERT-RETX] r" << replicaId_
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

    if (msg == clear_cert_candidate_timer_) {
        if (clear_cert_candidate_.echoes.empty()) return;
        const ClearCert cert = clear_cert_candidate_;
        const BlockedIncident incident{
            cert.cancelledEpoch, cert.executingBatch};
        auto incidentIt = incidentRegistry_.find(incident);
        const bool alreadyCleared =
            incidentIt != incidentRegistry_.end() &&
            incidentIt->second.state == IncidentState::CLEARED;
        if (alreadyCleared || order_applied_) {
            cancelClearCertCandidate(
                order_applied_ ? "order-applied" : "incident-cleared");
            return;
        }
        broadcastClearCert(cert);
        return;
    }

    if (msg == clear_cert_relay_timer_) {
        if (clear_cert_pending_relay_.echoes.empty()) return;
        const std::string key = clear_cert_pending_relay_key_;
        if (clearPropagationConfirmed(key) || order_applied_ ||
                propose_submitted_) {
            cancelClearCertRelay(
                order_applied_ ? "order-applied" :
                propose_submitted_ ? "order-proposed" :
                "propagation-confirmed");
            return;
        }
        const ClearCert cert = clear_cert_pending_relay_;
        clear_propagation_tracker_.observeAuthenticated(key, replicaId_);
        std::cout << "[CLEAR-CARRIER] r" << replicaId_
                  << " key=" << key
                  << " carrier=r" << replicaId_
                  << " count=" << clear_propagation_tracker_.count(key)
                  << "/" << clearPropagationThreshold()
                  << " source=local-relay\n";
        sendClearCertCarrier(cert, "CLEAR-RELAY");
        clear_cert_pending_relay_ = ClearCert{};
        clear_cert_pending_relay_key_.clear();
        return;
    }

    if (msg == wait_leader_send_timer_) {
        maybeSendWaitHeartbeat("periodic");
        return;
    }

    if (msg == wait_follower_expiry_timer_) {
        std::cout << "[WAIT-EXPIRED] r" << replicaId_
                  << " epoch=" << wait_follower_state_.cancelledEpoch
                  << " batch=" << wait_follower_state_.executingBatch
                  << " leader=r" << wait_follower_state_.leaderId
                  << " t=" << simTime() << "\n";
        wait_follower_state_.active = false;
        order_vc_requested_ = true;
        const int vcRc = ResdbOmnetForceViewChange(resdb_server_handle_);
        std::cout << "[APP-VC] r" << replicaId_
                  << " WAIT expiry forced view change rc=" << vcRc
                  << " t=" << simTime() << "\n";
        if (vcRc == 1 && !order_applied_) {
            order_vc_requested_ = false;
            armOrderSuspicionTimer("wait-vc-proof-incomplete");
        }
        return;
    }

    if (msg == discovery_deadline_msg_) {
        maybeAdvanceDiscovery("hard-deadline", true);
        return;
    }

    if (msg == discovery_settle_msg_) {
        maybeAdvanceDiscovery("intent-stable", false);
        return;
    }

    if (msg == vc_trigger_msg_) {
        vc_trigger_msg_ = nullptr;
        processOrders();
        if (!order_applied_) {
            if (cancel_consensus_pending_) {
                std::cout << "[ROLLBACK-VC-UNSUPPORTED] r" << replicaId_
                          << " suppressed app forced view-change while CANCEL consensus active"
                          << " cancelled_epoch=" << cancelled_epoch_
                          << " new_epoch=" << rollback_new_epoch_
                          << " phase=" << phaseToStr(current_phase_)
                          << " propose_submitted=" << propose_submitted_
                          << " t=" << simTime() << "\n";
                delete msg; return;
            }
            if (cancel_pending_ && hasBlockingIncidentForEpoch(cancelled_epoch_)) {
                std::cout << "[ORDER-VC-DEFER] r" << replicaId_
                          << " reason=blocking-incident-with-wait"
                          << " cancelled_epoch=" << cancelled_epoch_
                          << " t=" << simTime() << "\n";
                maybeSendWaitHeartbeat("order-vc-defer");
                delete msg; return;
            }
            if (inject_fabricated_clearance_leader_ && cancel_pending_ &&
                    current_epoch_ == rollback_new_epoch_) {
                fabricated_clearance_attack_phase_complete_ = true;
            }
            int primary = ResdbOmnetGetPrimary(resdb_server_handle_);
            std::cout << "[VC-TRIGGER] r" << replicaId_
                      << " forcing view change at " << simTime()
                      << " phase=" << phaseToStr(current_phase_)
                      << " pbft_primary=" << primary
                      << " propose_submitted=" << propose_submitted_;
            if (stop_time_ >= SIMTIME_ZERO)
                std::cout << " stop_to_vc_sec=" << (simTime() - stop_time_).dbl();
            std::cout << "\n";
            // TriggerViewChangeNow() in bridge: ChangeStatue(READY_VIEW_CHANGE) +
            // SendViewChangeMsg() directly.  All downstream VC timers (TYPE_VIEWCHANGE,
            // TYPE_NEWVIEW) use SleepForUs driven by SimTimeProvider → sim-time.
            order_vc_requested_ = true;
            const int vcRc = ResdbOmnetForceViewChange(resdb_server_handle_);
            std::cout << "[APP-VC] r" << replicaId_
                      << " ResdbOmnetForceViewChange rc=" << vcRc
                      << " t=" << simTime() << "\n";
            if (vcRc == 1 && !order_applied_) {
                order_vc_requested_ = false;
                armOrderSuspicionTimer("vc-proof-incomplete");
            }
        }
        delete msg; return;
    }

    if (msg == cancel_vc_timer_) {
        cancel_vc_timer_ = nullptr;
        const int oldProposer = chooseCancelProposer();
        const int oldIndex = cancel_rotation_index_;
        const std::vector<int> oldElectors = cancelElectorateCandidates();
        cancel_rotation_index_++;
        cancel_propose_submitted_ = false;
        propose_submitted_ = false;
        const int newProposer = chooseCancelProposer();
        const std::vector<int> newElectors = cancelElectorateCandidates();
        std::cout << "[CANCEL-VC-STATE] r" << replicaId_
                  << " old_index=" << oldIndex
                  << " new_index=" << cancel_rotation_index_
                  << " old_proposer=r" << oldProposer
                  << " new_proposer=r" << newProposer
                  << " pending=" << (cancel_consensus_pending_ ? 1 : 0)
                  << " cancel_pending=" << (cancel_pending_ ? 1 : 0)
                  << " submitted_reset=1"
                  << " |E_old|=" << oldElectors.size()
                  << " |E_new|=" << newElectors.size()
                  << " cert_bytes=" << cancel_cert_bytes_.size()
                  << " t=" << simTime() << "\n";
        std::cout << "[CANCEL-VC] r" << replicaId_
                  << " rotating cancel proposer index=" << cancel_rotation_index_
                  << " cancelled_epoch=" << cancelled_epoch_
                  << " t=" << simTime() << "\n";
        trySubmitCancelProposal("cancel-vc-timeout");
        delete msg; return;
    }

    if (msg == stop_sign_timeout_msg_) {
        stop_sign_timeout_msg_ = nullptr;
        // Apply any consensus orders already queued before declaring timeout.
        // Otherwise FES ordering can deliver this self-message before the next
        // transport poll: processOrders() would see order_applied_=1 and drop
        // the tail of pending_orders_ without ever logging Batch_Assignment.
        std::cout << "[TIMEOUT-PRE] r" << replicaId_
                  << " stop_sign flush processOrders order_applied=" << order_applied_
                  << " phase=" << phaseToStr(current_phase_) << " t=" << simTime()
                  << "\n";
        processOrders();
        std::cout << "[TIMEOUT-POST] r" << replicaId_
                  << " stop_sign after processOrders order_applied=" << order_applied_
                  << " t=" << simTime() << "\n";
        if (!order_applied_) {
            if (cancel_pending_ || cancel_consensus_pending_) {
                std::cout << "[HALT-LOCAL] r" << replicaId_
                          << " stop_sign_timeout suppressed while cancel active epoch="
                          << cancelled_epoch_ << "\n";
                delete msg; return;
            }
            if (enableRollback_) {
                std::cout << "[HALT-LOCAL] r" << replicaId_
                          << " stop_sign_timeout fail-closed"
                          << " reason=no-committed-order"
                          << " epoch=" << current_epoch_ << "\n";
                evaluateOrderReadiness("stop-sign-timeout");
                delete msg; return;
            }
            std::cout << "[METRICS " << replicaId_ << "] StopSign_Timeout: 1\n";
            std::cout << "[VC-TIMEOUT] r" << replicaId_
                      << " stop_sign_timeout_fired at=" << simTime()
                      << " phase=" << phaseToStr(current_phase_)
                      << " propose_submitted=" << propose_submitted_;
            if (stop_time_ >= SIMTIME_ZERO)
                std::cout << " stop_to_timeout_sec=" << (simTime() - stop_time_).dbl();
            if (propose_time_ >= SIMTIME_ZERO)
                std::cout << " propose_to_timeout_sec=" << (simTime() - propose_time_).dbl();
            std::cout << "\n";
            order_applied_ = true;
            current_phase_ = ConsensusPhase::EXECUTING;
            resumeVehicle(0);
        }
        delete msg; return;
    }

    if (msg == consensus_timeout_msg_) {
        consensus_timeout_msg_ = nullptr;
        std::cout << "[TIMEOUT-PRE] r" << replicaId_
                  << " consensus flush processOrders order_applied=" << order_applied_
                  << " phase=" << phaseToStr(current_phase_) << " t=" << simTime()
                  << "\n";
        processOrders();
        std::cout << "[TIMEOUT-POST] r" << replicaId_
                  << " consensus after processOrders order_applied=" << order_applied_
                  << " t=" << simTime() << "\n";
        if (!order_applied_) {
            if (cancel_pending_ || cancel_consensus_pending_) {
                std::cout << "[HALT-LOCAL] r" << replicaId_
                          << " consensus_timeout suppressed while cancel active epoch="
                          << cancelled_epoch_ << "\n";
                delete msg; return;
            }
            if (enableRollback_) {
                std::cout << "[HALT-LOCAL] r" << replicaId_
                          << " consensus_timeout fail-closed"
                          << " reason=no-committed-order"
                          << " epoch=" << current_epoch_ << "\n";
                evaluateOrderReadiness("consensus-timeout");
                delete msg; return;
            }
            std::cout << "[METRICS " << replicaId_ << "] Consensus_Timeout: 1\n";
            std::cout << "[VC-TIMEOUT] r" << replicaId_
                      << " consensus_timeout_fired at=" << simTime()
                      << " phase=" << phaseToStr(current_phase_)
                      << " propose_submitted=" << propose_submitted_ << "\n";
            order_applied_ = true;
            current_phase_ = ConsensusPhase::EXECUTING;
            resumeVehicle(0);
        }
        delete msg; return;
    }

    if (msg == resume_msg_) {
        resume_msg_ = nullptr;
        if (cancel_pending_ || cancel_consensus_pending_) {
            std::cout << "[HALT-LOCAL] r" << replicaId_
                      << " pending resume suppressed while cancel active epoch="
                      << cancelled_epoch_ << "\n";
            delete msg; return;
        }
        resumeVehicle(pending_resume_position_);
        delete msg; return;
    }

    if (msg == preceding_batch_poll_msg_) {
        // Scenario 16 crash-dwell perception: scans every batch strictly before
        // my own (not just preceding_batch_cars_, which is only my_batch-1) so
        // every vehicle not in batch 0 is a potential BLOCKED witness. Gated
        // purely on enableRollback_ — no separate scenario flag.
        if (enableRollback_) {
            for (int b = 0; b < my_batch_index_; ++b) {
                if (b >= (int)committed_order_batches_.size()) continue;
                for (int rid : committed_order_batches_[b]) {
                    if (rid == replicaId_) continue;
                    const std::string target = "veh" + std::to_string(rid);
                    bool qualified = false;
                    if (enable_noisy_crash_perception_ && perception_) {
                        const auto sample =
                            perception_->observeConflictBoxOccupancy(target, simTime());
                        qualified = sample.valid && sample.observedOccupied;
                        if (sample.valid)
                            ++occupancy_confusion_[0][sample.trueOccupied ? 1 : 0]
                                                     [sample.observedOccupied ? 1 : 0];
                        else
                            ++occupancy_invalid_[0];
                        if (enable_occupancy_perception_trace_) {
                            std::cout << "[OCC-PERCEPTION] witness=" << replicaId_
                                      << " target=" << target
                                      << " decision=BLOCKED"
                                      << " valid=" << (sample.valid ? 1 : 0)
                                      << " trueOccupied=" << (sample.trueOccupied ? 1 : 0)
                                      << " observedOccupied=" << (sample.observedOccupied ? 1 : 0)
                                      << " trueMargin=" << sample.trueSignedMarginM
                                      << " observedMargin=" << sample.observedSignedMarginM
                                      << " t=" << simTime() << "\n";
                        }
                    } else {
                        qualified = !vehicleHasClearedIntersectionTraCI(target) &&
                            vehicleInConflictBoxTraCI(target) &&
                            vehicleSpeedTraCI(target) < crash_speed_eps_;
                    }
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

                    std::cout << "[CRASH-PERCEIVE] r" << replicaId_
                              << " target=" << target
                              << " batch=" << b
                              << " dwell=" << dwell
                              << " t=" << simTime() << "\n";
                    maybeTriggerCrashRollback(formatBlockedBatchRef(last_committed_epoch_, (uint32_t)b));
                    crash_echoed_targets_.insert(target);
                }
            }

            // CLEAR empty-box dwell: same tick, same enableRollback_ gate, no
            // separate timer. Checks whole-box occupancy (not per-target) since
            // the clearance predicate is "no vehicle occupies the box at all".
            for (const auto& kv : incidentRegistry_) {
                const BlockedIncident& incident = kv.first;
                if (kv.second.state != IncidentState::BLOCKING) continue;
                if (clear_echoed_incidents_.count(incident)) continue;
                bool observedAnyOccupied = false;
                bool observationValid = true;
                bool trueAnyOccupied = anyVehicleInConflictBoxTraCI();
                if (enable_noisy_crash_perception_ && perception_) {
                    try {
                        const auto sample =
                            perception_->observeAnyConflictBoxOccupancy(simTime());
                        if (sample.valid)
                            ++occupancy_confusion_[1][sample.trueOccupied ? 1 : 0]
                                                     [sample.observedOccupied ? 1 : 0];
                        else
                            ++occupancy_invalid_[1];
                        observationValid = sample.valid;
                        trueAnyOccupied = sample.valid ? sample.trueOccupied : true;
                        observedAnyOccupied = sample.valid ? sample.observedOccupied : true;
                        if (enable_occupancy_perception_trace_) {
                            std::cout << "[OCC-PERCEPTION] witness=" << replicaId_
                                      << " target=BOX_NEAREST"
                                      << " decision=CLEAR"
                                      << " valid=" << (sample.valid ? 1 : 0)
                                      << " trueOccupied=" << (sample.trueOccupied ? 1 : 0)
                                      << " observedOccupied=" << (sample.observedOccupied ? 1 : 0)
                                      << " trueMargin=" << sample.trueSignedMarginM
                                      << " observedMargin=" << sample.observedSignedMarginM
                                      << " t=" << simTime() << "\n";
                        }
                    } catch (...) {
                        observationValid = false;
                        observedAnyOccupied = true;
                    }
                } else {
                    observedAnyOccupied = trueAnyOccupied;
                }
                if (enable_occupancy_perception_trace_) {
                    std::cout << "[OCC-DECISION] witness=" << replicaId_
                              << " decision=CLEAR"
                              << " valid=" << (observationValid ? 1 : 0)
                              << " trueOccupied=" << (trueAnyOccupied ? 1 : 0)
                              << " observedOccupied=" << (observedAnyOccupied ? 1 : 0)
                              << " t=" << simTime() << "\n";
                }
                if (observedAnyOccupied) {
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

                std::cout << "[CLEAR-PERCEIVE] r" << replicaId_
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
            if (cancel_pending_ || cancel_consensus_pending_) {
                if (timedOut && !all_cleared) {
                    std::cout << "[HALT-LOCAL] r" << replicaId_
                              << " clearance timeout suppressed while cancel active epoch="
                              << cancelled_epoch_ << "\n";
                }
                scheduleAt(simTime() + preceding_batch_poll_period_sec_, preceding_batch_poll_msg_);
                return;
            }
            if (timedOut && !all_cleared) {
                std::cout << "[CLEARANCE r" << replicaId_ << "] TIMEOUT waiting for batch "
                          << (my_batch_index_ - 1) << "; resuming anyway\n";
            } else {
                std::cout << "[CLEARANCE r" << replicaId_ << "] batch "
                          << (my_batch_index_ - 1) << " cleared, resuming batch "
                          << my_batch_index_ << "\n";
            }
            std::cout << "[METRICS " << replicaId_ << "] Resume_Time: " << simTime()
                      << " (batch=" << my_batch_index_ << ")\n";
            resumeVehicle(my_batch_index_);
            return;
        }

        scheduleAt(simTime() + preceding_batch_poll_period_sec_, preceding_batch_poll_msg_);
        return;
    }

    DemoBaseApplLayer::handleSelfMsg(msg);
}

// ── handlePositionUpdate ──────────────────────────────────────────────────────

void ResDBIntersectionApp::handlePositionUpdate(cObject* obj)
{
    DemoBaseApplLayer::handlePositionUpdate(obj);

    if (moduleIsAmbulance && !ambulanceColorSet && mobility && mobility->getVehicleCommandInterface()) {
        std::cout << "[AMBULANCE COLOR] r" << replicaId_ << " setting color to red\n";
        mobility->getVehicleCommandInterface()->setColor(TraCIColor(255, 0, 0, 255));
        ambulanceColorSet = true;
    }

    discoverLane();
    applyPhase2ControlledCue();
    logPhase2CueTrace();

    if (entered_stop_zone_ && !stopped_distance_attestation_sent_ &&
            !propose_submitted_ && !order_applied_) {
        maybeBroadcastStoppedDistanceAttestation();
    }

    if (order_applied_) return;

    double dist = getDistanceToIntersection();
    if (dist >= stop_distance_ || dist <= 0) return;

    if (!entered_stop_zone_) {
        entered_stop_zone_ = true;
        stop_time_ = simTime();
        current_phase_ = ConsensusPhase::WAITING_FOR_CLEARANCE;
        std::cout << "[METRICS " << replicaId_ << "] Stop_Time: " << stop_time_ << "\n";

        for (auto& [key, pr] : pending_relays_) {
            if (pr.relayCount >= 3) continue;
            if (collected_certs_.count(pr.carId)) continue;

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
        std::cout << "[VC-DEBUG] r" << replicaId_
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
    is_departed_ = true;
    current_phase_ = ConsensusPhase::DEPARTED;
    clearConsensusRetries("departed");
    deactivateDiscovery("departed");
    markCompletedReplicaEpoch(replicaId_, current_epoch_);
    if (resdb_server_handle_) {
        ResdbOmnetMarkReplicaInactive(resdb_server_handle_, replicaId_, current_epoch_ + 1);
    }
    {
        std::lock_guard<std::mutex> lk(outbound_mutex_);
        outbound_queue_.clear();
    }

    const double wait_sec = (stop_time_ >= SIMTIME_ZERO) ? (cleared_time_ - stop_time_).dbl() : -1.0;
    const double stop_dbl = (stop_time_ >= SIMTIME_ZERO) ? stop_time_.dbl() : -1.0;
    const char* role = is_ambulance_ ? "ambulance" : "normal";

    std::cout << "[DEPARTED] Replica " << replicaId_ << " cleared intersection t=" << departedAt << "\n";
    std::cout << "[METRICS " << replicaId_ << "] Total Latency (cleared-stop): " << wait_sec << std::endl;
    std::cout << "[CAR-METRICS] veh" << replicaId_
              << " role=" << role
              << " epoch=" << current_epoch_
              << " stop_time=" << stop_dbl
              << " depart_time=" << cleared_time_.dbl()
              << " wait_stop_to_departure_sec=" << wait_sec << "\n";
    if (is_ambulance_) {
        std::cout << "[AMBULANCE_METRICS] veh" << replicaId_
                  << " sim_wait_stop_to_departure_sec=" << wait_sec
                  << " epoch=" << current_epoch_ << "\n";
    }
}

// ── finish ────────────────────────────────────────────────────────────────────

void ResDBIntersectionApp::finish()
{
    // OMNeT++ freezes sim-time at endSimulation(), so any ResDB thread sleeping
    // in SleepUntilUs() would wait forever.  Advance sim-time to INT64_MAX first
    // so all SleepUntilUs waiters unblock before we call Stop()/join().
    std::cout << "[METRICS " << replicaId_ << "] Messages_Sent: " << sentMessages_ << "\n";
    std::cout << "[METRICS " << replicaId_ << "] Messages_Received: " << receivedMessages_ << "\n";
    std::cout << "[METRICS " << replicaId_ << "] Bytes_Sent: " << sentPayloadBytes_ << "\n";
    std::cout << "[METRICS " << replicaId_ << "] Megabytes_Sent: "
              << (static_cast<double>(sentPayloadBytes_) / (1024.0 * 1024.0)) << "\n";
    std::cout << "[METRICS " << replicaId_ << "] Quiet_Honest_Vehicles: "
              << quietHonestVehicles_ << "\n";
    std::cout << "[METRICS " << replicaId_ << "] Quiet_Honest_Opportunities: "
              << quietHonestOpportunities_ << "\n";
    std::cout << "[METRICS " << replicaId_ << "] Quiet_Honest_Rate: "
              << (quietHonestOpportunities_ > 0
                      ? (100.0 * static_cast<double>(quietHonestVehicles_) /
                         static_cast<double>(quietHonestOpportunities_))
                      : 0.0)
              << "\n";
    for (int decision = 0; decision < 2; ++decision) {
        std::cout << "[OCC-METRICS] witness=" << replicaId_
                  << " decision=" << (decision == 0 ? "BLOCKED" : "CLEAR")
                  << " true0_obs0=" << occupancy_confusion_[decision][0][0]
                  << " true0_obs1=" << occupancy_confusion_[decision][0][1]
                  << " true1_obs0=" << occupancy_confusion_[decision][1][0]
                  << " true1_obs1=" << occupancy_confusion_[decision][1][1]
                  << " invalid=" << occupancy_invalid_[decision] << "\n";
    }
    if (perception_) {
        std::cout << "[PERCEPTION-RNG] replica=" << replicaId_
                  << " draws=" << perception_->randomDrawCount() << "\n";
    }
    
    std::cerr << "[FINISH-PROBE] r" << replicaId_
    << " handle=" << resdb_server_handle_ << std::endl;

    if (is_departed_ || departureTime >= SIMTIME_ZERO) {
        markCompletedReplicaEpoch(replicaId_, current_epoch_);
    }

    if (resdb_server_handle_) {
        ResdbOmnetMarkReplicaInactive(resdb_server_handle_, replicaId_, current_epoch_ + 1);
        ResdbOmnetUpdateSimTimeUs(resdb_server_handle_,
                                  std::numeric_limits<int64_t>::max());
    }
    std::cerr << "[FINISH-PROBE] r" << replicaId_ << " after UpdateSimTime" << std::endl;
    // Stop global monitor (stats thread); cv fix in stats.cpp ensures it wakes
    // immediately rather than sleeping for monitor_sleep_time_ wall-clock seconds.
    ResdbOmnetStopGlobalStats();
    std::cerr << "[FINISH-PROBE] r" << replicaId_ << " after StopGlobalStats" << std::endl;

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
    // A cancelled discovery timer may be owned by the TraCI scenario manager.
    // Retake it while finish() still has a valid manager/module context; waiting
    // for the C++ destructor can touch torn-down ownership state during network
    // deletion (notably when lane noise leaves a vehicle QUIET).
    deleteFinishedTimer(arrival_cert_finalize_timer_);
    deleteFinishedTimer(stopped_distance_finalize_timer_);
    deleteFinishedTimer(stopped_distance_attestation_retry_timer_);
    deleteFinishedTimer(discovery_tx_flush_timer_);
    deleteFinishedTimer(consensus_retry_timer_);
    deleteFinishedTimer(consensus_relay_timer_);

    deleteFinishedTimer(vc_trigger_msg_);
    if (resdb_server_handle_) {
        std::cerr << "[FINISH-PROBE] r" << replicaId_ << " calling StopServer" << std::endl;
        ResdbOmnetStopServer(resdb_server_handle_);
        std::cerr << "[FINISH-PROBE] r" << replicaId_ << " after StopServer" << std::endl;
        ResdbOmnetDestroyServer(resdb_server_handle_);
        resdb_server_handle_ = nullptr;
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

    std::cerr << "[FINISH-PROBE] r" << replicaId_ << " calling DemoBaseApplLayer::finish" << std::endl;
    DemoBaseApplLayer::finish();
    std::cerr << "[FINISH-PROBE] r" << replicaId_ << " DONE" << std::endl;
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
    if (bft->getFromReplicaId() == replicaId_) return;   // no self-delivery
    if (current_phase_ == ConsensusPhase::DEPARTED) return;  // Gap 9: zombie filter

    if (crashCommsDisabled_) {
        std::cout << "[CRASH-RX-DROP] r" << replicaId_
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
        if (bft->getToReplicaId() == replicaId_ || bft->getToReplicaId() == -1)
            handleArrivalEcho(bft);
        return;
    }

    if (msgType == kArrivalCertType) {
        handleArrivalCert(bft);
        return;
    }

    if (msgType == kStoppedDistanceAttestationType) {
        handleStoppedDistanceAttestation(bft);
        return;
    }

    if (msgType == kStoppedDistanceEchoType) {
        if (bft->getToReplicaId() == replicaId_ || bft->getToReplicaId() == -1)
            handleStoppedDistanceEcho(bft);
        return;
    }

    if (msgType == kStoppedDistanceCertType) {
        handleStoppedDistanceCert(bft);
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
            if (order_applied_ || has_pending_order) {
                cancelEvent(vc_trigger_msg_);
                delete vc_trigger_msg_;
                vc_trigger_msg_ = nullptr;
                std::cout << "[VC-DEBUG] r" << replicaId_
                          << " canceled follower vc_trigger on relayed delivered order at "
                          << simTime()
                          << " order_applied=" << order_applied_
                          << " pending_order=" << has_pending_order << "\n";
            } else {
                cancelEvent(vc_trigger_msg_);
                scheduleAt(simTime() + pbft_vc_timeout_sec_, vc_trigger_msg_);
                std::cout << "[VC-DEBUG] r" << replicaId_
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
            if (order_applied_ || has_pending_order) {
                cancelEvent(vc_trigger_msg_);
                delete vc_trigger_msg_;
                vc_trigger_msg_ = nullptr;
                std::cout << "[VC-DEBUG] r" << replicaId_
                          << " canceled follower vc_trigger on delivered order at "
                          << simTime()
                          << " order_applied=" << order_applied_
                          << " pending_order=" << has_pending_order << "\n";
            } else {
                cancelEvent(vc_trigger_msg_);
                scheduleAt(simTime() + pbft_vc_timeout_sec_, vc_trigger_msg_);
                std::cout << "[VC-DEBUG] r" << replicaId_
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
    const int f = tolerated_faults_ >= 0 ? tolerated_faults_ : (total_vehicles_ - 1) / 3;
    return gossip_acc_.count(gossip_epoch_, gossip_order_bytes_) >= f + 1;
}

void ResDBIntersectionApp::triggerGossip(uint32_t epoch,
                                          const std::vector<uint8_t>& order_bytes)
{
    if (order_bytes.empty()) return;
    if (isEpochTombstoned(epoch) ||
            (cancel_consensus_pending_ && epoch == cancelled_epoch_)) {
        std::cout << "[GOSSIP-SKIP] r" << replicaId_
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
        ec_private_key_, ec_pub_key_, inner.data(), (uint32_t)inner.size());
    if (!signed_payload.empty()) {
        sendBFTMessage(-1, signed_payload, kDecisionGossipType);
        std::cout << "[GOSSIP-SEND] r" << replicaId_ << " epoch=" << epoch
                  << " retry=0 t=" << simTime() << "\n";
    }
    gossip_retry_count_++;
    scheduleNextGossip();
}

void ResDBIntersectionApp::scheduleNextGossip()
{
    if (!gossip_enabled_ || gossip_order_bytes_.empty()) return;
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
        std::cout << "[GOSSIP-RECV] r" << replicaId_ << " dropped forged gossip from "
                  << bft->getFromReplicaId() << "\n";
        return;
    }

    uint32_t epoch;
    std::vector<uint8_t> order_bytes;
    if (!resdb_gossip::parse(view.resdbBytes, view.resdbLen, epoch, order_bytes)) return;

    if (isEpochTombstoned(epoch)) {
        std::cout << "[TYPE9-RECV] r" << replicaId_
                  << " dropped tombstoned epoch=" << epoch << "\n";
        return;
    }

    // Epoch guard: drop stale gossip from a previous round.
    if (has_committed_order_ && epoch <= last_committed_epoch_) return;
    if (hasCompletedReplicaEpoch(replicaId_, epoch)) {
        std::cout << "[TYPE9-RECV] r" << replicaId_
                  << " epoch=" << epoch
                  << " already departed; skipping\n";
        return;
    }

    // Count this vote even if I've already applied my own order — same fix
    // as CANCEL-commit gossip: order_applied_ becomes true for myself right
    // when I start my own retry loop, so if counting stopped here,
    // decisionGossipPropagationConfirmed() could never observe enough peers
    // to let that loop stop early.
    int f         = tolerated_faults_ >= 0 ? tolerated_faults_ : (total_vehicles_ - 1) / 3;
    int threshold = f + 1;
    bool reached  = gossip_acc_.add(bft->getFromReplicaId(), epoch, order_bytes, threshold);

    if (order_applied_) {
        std::cout << "[TYPE9-RECV] r" << replicaId_ << " from=" << bft->getFromReplicaId()
                  << " order_applied=true, skipping\n";
        return;
    }

    std::cout << "[GOSSIP-RECV] r" << replicaId_ << " from=" << bft->getFromReplicaId()
              << " epoch=" << epoch
              << " count=" << gossip_acc_.count(epoch, order_bytes)
              << "/" << threshold << " t=" << simTime() << "\n";

    if (reached) applyGossipOrder(order_bytes, epoch);
}

bool ResDBIntersectionApp::applyGossipOrder(const std::vector<uint8_t>& order_bytes,
                                             uint32_t epoch)
{
    if (order_applied_) return false;
    if (hasCompletedReplicaEpoch(replicaId_, epoch)) {
        std::cout << "[GOSSIP-APPLY] r" << replicaId_
                  << " refused completed epoch=" << epoch << "\n";
        return false;
    }
    if (isEpochTombstoned(epoch)) {
        std::cout << "[GOSSIP-APPLY] r" << replicaId_
                  << " refused tombstoned epoch=" << epoch << "\n";
        return false;
    }
    std::cout << "[GOSSIP-APPLY] r" << replicaId_
              << " epoch=" << epoch << " t=" << simTime() << "\n";
    // Push into the same queue onOrderDecided uses.  The 1 ms transport-poll
    // timer will call processOrders() and apply it on the next tick.
    {
        std::lock_guard<std::mutex> lk(orders_mutex_);
        pending_orders_.push_back(order_bytes);
    }
    last_committed_epoch_ = epoch;
    has_committed_order_ = true;
    committed_order_bytes_ = order_bytes;
    stopGossip();
    gossip_acc_.reset();
    cert_relay_tracker_.reset();
    announcement_relay_tracker_.reset();
    cancelPendingConsensusRelays("gossip-order-adopted");
    return true;
}

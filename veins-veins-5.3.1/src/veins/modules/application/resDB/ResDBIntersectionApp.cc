#include "veins/modules/application/resDB/ResDBIntersectionApp.h"
#include "veins/modules/bftsmart/BFTMessage_m.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>

#include "veins/modules/application/resDB/ResdbV2VWire.h"
#include "veins/modules/bftsmart/BFTMessage_m.h"

using namespace veins;

Define_Module(veins::ResDBIntersectionApp);

// ── Message type constants ────────────────────────────────────────────────────
namespace {
constexpr int kArrivalAnnounceType   = 1;   // ARRIVAL_ANNOUNCE  (broadcast)
constexpr int kArrivalEchoType       = 4;   // ARRIVAL_ECHO      (unicast)
constexpr int kArrivalCertType       = 5;   // ARRIVAL_CERT      (broadcast)
constexpr int kResdbConsensusMsgType = 8;   // ResDB PBFT bytes  (signed)
}

// ── Static helpers ────────────────────────────────────────────────────────────

static std::vector<std::string> splitStr(const std::string& s, char delim) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : s) {
        if (c == delim) { parts.push_back(cur); cur.clear(); }
        else cur += c;
    }
    parts.push_back(cur);
    return parts;
}

static std::string toHex(const std::vector<uint8_t>& v) {
    static const char* D = "0123456789abcdef";
    std::string out; out.reserve(v.size() * 2);
    for (uint8_t b : v) { out += D[b >> 4]; out += D[b & 0xf]; }
    return out;
}
static std::string toHex(const uint8_t* p, size_t len) {
    return toHex(std::vector<uint8_t>(p, p + len));
}

static std::vector<uint8_t> fromHex(const std::string& s) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < s.size(); i += 2)
        out.push_back((uint8_t)std::stoi(s.substr(i, 2), nullptr, 16));
    return out;
}

static std::string dirToStr(ResDBIntersectionApp::Direction d) {
    switch (d) {
        case ResDBIntersectionApp::DIR_LEFT:  return "L";
        case ResDBIntersectionApp::DIR_RIGHT: return "R";
        default:                              return "S";
    }
}
static ResDBIntersectionApp::Direction strToDir(const std::string& s) {
    if (s == "L") return ResDBIntersectionApp::DIR_LEFT;
    if (s == "R") return ResDBIntersectionApp::DIR_RIGHT;
    return ResDBIntersectionApp::DIR_STRAIGHT;
}
static const char* phaseToStr(int p) {
    switch (p) {
        case 0: return "IDLE";
        case 1: return "COLLECTING_CERTS";
        case 2: return "WAITING_FOR_CLEARANCE";
        case 3: return "PULLING_FORWARD";
        case 4: return "EXECUTING";
        case 5: return "DEPARTED";
    }
    return "UNKNOWN";
}

// ── Destructor ────────────────────────────────────────────────────────────────

ResDBIntersectionApp::~ResDBIntersectionApp()
{
    if (smoke_test_msg_)          { cancelAndDelete(smoke_test_msg_);          smoke_test_msg_          = nullptr; }
    if (transport_poll_msg_)      { cancelAndDelete(transport_poll_msg_);      transport_poll_msg_      = nullptr; }
    if (time_tick_msg_)           { cancelAndDelete(time_tick_msg_);           time_tick_msg_           = nullptr; }
    if (propose_timeout_msg_)     { cancelAndDelete(propose_timeout_msg_);     propose_timeout_msg_     = nullptr; }
    if (cert_retry_timer_)        { cancelAndDelete(cert_retry_timer_);        cert_retry_timer_        = nullptr; }
    if (initial_announce_msg_)    { cancelAndDelete(initial_announce_msg_);    initial_announce_msg_    = nullptr; }
    if (stop_sign_timeout_msg_)   { cancelAndDelete(stop_sign_timeout_msg_);   stop_sign_timeout_msg_   = nullptr; }
    if (consensus_timeout_msg_)   { cancelAndDelete(consensus_timeout_msg_);   consensus_timeout_msg_   = nullptr; }
    if (resume_msg_)              { cancelAndDelete(resume_msg_);              resume_msg_              = nullptr; }
    if (clearance_poll_msg_)      { cancelAndDelete(clearance_poll_msg_);      clearance_poll_msg_      = nullptr; }
    if (broadcastArrivalAnnouncement_timer_) { cancelAndDelete(broadcastArrivalAnnouncement_timer_); broadcastArrivalAnnouncement_timer_ = nullptr; }
    if (resdb_server_handle_) {
        ResdbOmnetDestroyServer(resdb_server_handle_);
        resdb_server_handle_ = nullptr;
    }
    if (ec_private_key_) {
        EVP_PKEY_free(ec_private_key_);
        ec_private_key_ = nullptr;
    }
}

// ── initialize ────────────────────────────────────────────────────────────────

void ResDBIntersectionApp::initialize(int stage)
{
    DemoBaseApplLayer::initialize(stage);

    if (stage == 0) {
        replicaId_                = par("replicaId").intValue();
        useRadioTransport_        = par("useRadioTransport").boolValue();
        transport_poll_interval_  = par("transportPollInterval").doubleValue();
        enable_sim_time_provider_ = par("enableSimTimeProvider").boolValue();
        time_tick_interval_       = par("timeTickInterval").doubleValue();
        broadcast_arrival_announcement_interval_ = par("broadcastArrivalAnnouncementIntervalSec").doubleValue();
        cert_collection_timeout_  = par("certCollectionTimeoutSec").doubleValue();
        debug_cert_protocol_    = par("debugCertProtocol").boolValue();
        enable_cert_retries_    = par("enableArrivalCertRetries").boolValue();
        cert_retry_interval_    = par("arrivalCertRetryIntervalSec").doubleValue();
        cert_retry_max_         = par("arrivalCertRetryMax").intValue();

        stop_distance_         = par("stopDistance").doubleValue();
        //stop distance formula = stop distance * (total vehicles / 2)
        total_vehicles_        = par("totalVehicles").intValue();
        stop_distance_ = stop_distance_ * (total_vehicles_ / 2);

        cruise_speed_mps_      = par("cruiseSpeedMps").doubleValue();
        is_ambulance_          = par("isAmbulance").boolValue();
        trigger_join_time_     = par("triggerJoinTimeSec").doubleValue();
        arrival_slot_sec_      = par("arrivalSlotSec").doubleValue();
        stop_sign_timeout_sec_ = par("stopSignTimeoutSec").doubleValue();
        consensus_timeout_sec_ = par("consensusTimeoutSec").doubleValue();
        clearance_poll_period_sec_ = par("clearancePollPeriodSec").doubleValue();
        clearance_timeout_sec_     = par("clearanceTimeoutSec").doubleValue();
        intended_direction_    = par("intendedDirection").stdstringValue();
        intended_lane_         = par("intendedLane").stdstringValue();
        is_byzantine_        = par("isByzantine").boolValue();
        byzantine_type_      = static_cast<ByzantineType>(par("byzantineType").intValue());
        pbft_vc_timeout_sec_ = par("pbftVcTimeoutSec").doubleValue();

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

        registerTransport();
        ResdbOmnetSetOrderCallback(resdb_server_handle_,
                                   &ResDBIntersectionApp::onOrderDecided, this);
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

        // For SILENT_PRIMARY: silence the PBFT communicator so the primary's PBFT
        // replica also drops all outbound messages.  Followers' complaint timers then
        // fire and ResDB's built-in VIEW_CHANGE / NEW_VIEW exchange runs naturally.
        if (is_byzantine_ && byzantine_type_ == BYZANTINE_SILENT_PRIMARY) {
            ResdbOmnetSetPbftSilent(resdb_server_handle_, 1);
            std::cout << "[BYZANTINE] r" << replicaId_
                      << " SILENT_PRIMARY: PBFT communicator silenced\n";
        }
    }

    if (stage == 1) {
        if (par("smokeTestBroadcast").boolValue()) {
            smoke_test_msg_ = new cMessage("resdbSmokeTest");
            scheduleAt(simTime() + 0.05, smoke_test_msg_);
        }
        // Staggered one-shot arrival announcement (replicates V2VProxyModule triggerJoinTimer).
        initial_announce_msg_ = new cMessage("resdbInitialAnnounce");
        scheduleAt(simTime() + trigger_join_time_ + replicaId_ * arrival_slot_sec_,
                   initial_announce_msg_);

        // // Periodic re-announce fallback (in case peers miss the initial broadcast).
        // state_announce_msg_ = new cMessage("resdbStateAnnounce");
        // scheduleAt(simTime() + state_announce_interval_, state_announce_msg_);

        broadcastArrivalAnnouncement_timer_ = new cMessage("resdbBroadcastArrivalAnnouncement");
        scheduleAt(simTime() + broadcast_arrival_announcement_interval_, broadcastArrivalAnnouncement_timer_);
    }
}

// ── handleSelfMsg ─────────────────────────────────────────────────────────────

void ResDBIntersectionApp::handleSelfMsg(cMessage* msg)
{
    if (msg == smoke_test_msg_) {
        smoke_test_msg_ = nullptr;
        const uint8_t probe[] = {'R','E','S','D','B','T','S','T'};
        ResdbOmnetTestBroadcast(resdb_server_handle_, probe, sizeof(probe));
        delete msg; return;
    }

    if (msg == transport_poll_msg_) {
        drainOutboundQueue();
        processOrders();
        // Detect primary change after view-change.
        if (resdb_server_handle_) {
            int current_primary = ResdbOmnetGetPrimary(resdb_server_handle_);
            if (current_primary != last_known_primary_) {
                std::cout << "[VC-DEBUG] r" << replicaId_
                          << " primary changed: " << last_known_primary_
                          << " -> " << current_primary
                          << " phase=" << phaseToStr(current_phase_)
                          << " propose_submitted=" << propose_submitted_
                          << " order_applied=" << order_applied_ << "\n";
                last_known_primary_ = current_primary;
                if (current_primary == replicaId_ && !order_applied_ &&
                    !propose_submitted_ &&
                    current_phase_ != ConsensusPhase::DEPARTED) {
                    propose_submitted_ = false;
                    std::cout << "[VC-DEBUG] r" << replicaId_
                              << " became primary, re-proposing in phase="
                              << phaseToStr(current_phase_) << "\n";
                    proposeAll();
                } else if (current_primary == replicaId_) {
                    std::cout << "[VC-DEBUG] r" << replicaId_
                              << " became primary but skipped re-propose"
                              << " propose_submitted=" << propose_submitted_
                              << " order_applied=" << order_applied_
                              << " phase=" << phaseToStr(current_phase_) << "\n";
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

    if (msg == initial_announce_msg_) {
        initial_announce_msg_ = nullptr;
        broadcastArrivalAnnouncement();
        delete msg; return;
    }

    if (msg == broadcastArrivalAnnouncement_timer_) {
        // Periodic self-message: never delete after scheduleAt() — the same cMessage must stay owned
        // by the FES until cancelled (see transport_poll_msg_ / time_tick_msg_ above).
        if (physically_observed_cars_.size() != total_vehicles_ && !order_applied_) {
            broadcastArrivalAnnouncement();
            scheduleAt(simTime() + broadcast_arrival_announcement_interval_, broadcastArrivalAnnouncement_timer_);
            std::cout << "[ANN-SEND] Replica " << replicaId_ << " rescheduled arrival-announcement timer\n";
        } else {
            broadcastArrivalAnnouncement_timer_ = nullptr;
            delete msg;
        }
        return;
    }

    if (msg == cert_retry_timer_) {
        if (cert_pending_retries_.carId.empty() || propose_submitted_ || order_applied_) {
            stopCertBroadcastRetries();
            return;
        }
        cert_retry_count_++;
        sendBFTMessage(-1, serializeArrivalCert(cert_pending_retries_), kArrivalCertType);
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

   
    if (msg == propose_timeout_msg_) {
        propose_timeout_msg_ = nullptr;
        if (!propose_submitted_) {
            if (!entered_stop_zone_) {
                deferred_propose_after_cert_timeout_ = true;
                std::cout << "[ResDB r" << replicaId_
                          << "] cert-collection timeout before stop zone — defer propose until stop line\n";
                delete msg;
                return;
            }
            std::cout << "[ResDB r" << replicaId_
                      << "] cert-collection timeout — proposing with available certs\n";
            if (debug_cert_protocol_) {
                int f = (total_vehicles_ - 1) / 3;
                int need = f + 1;
                std::string myCarId = "veh" + std::to_string(replicaId_);
                size_t myEchoes = my_received_echoes_.count(myCarId)
                    ? my_received_echoes_[myCarId].size() : 0;
                std::cout << "[CERT-DEBUG] timeout snapshot r" << replicaId_
                          << " myEchoesTowardCert=" << myEchoes << "/" << need
                          << " cert_broadcast_=" << cert_broadcast_
                          << " collected_certs=" << collected_certs_.size()
                          << "/" << total_vehicles_ << "\n";
            }
            // Only primary proposes; follower vc_trigger_msg_ is scheduled at stop-zone entry.
            int primary = ResdbOmnetGetPrimary(resdb_server_handle_);
            if (primary == replicaId_) {
                proposeAll();
            }
        }
        delete msg; return;
    }

    if (msg == vc_trigger_msg_) {
        vc_trigger_msg_ = nullptr;
        if (!order_applied_) {
            std::cout << "[VC-TRIGGER] r" << replicaId_
                      << " forcing view change at " << simTime()
                      << " phase=" << phaseToStr(current_phase_);
            if (stop_time_ >= SIMTIME_ZERO)
                std::cout << " stop_to_vc_sec=" << (simTime() - stop_time_).dbl();
            std::cout << "\n";
            // TriggerViewChangeNow() in bridge: ChangeStatue(READY_VIEW_CHANGE) +
            // SendViewChangeMsg() directly.  All downstream VC timers (TYPE_VIEWCHANGE,
            // TYPE_NEWVIEW) use SleepForUs driven by SimTimeProvider → sim-time.
            ResdbOmnetForceViewChange(resdb_server_handle_);
        }
        delete msg; return;
    }

    if (msg == stop_sign_timeout_msg_) {
        stop_sign_timeout_msg_ = nullptr;
        if (!order_applied_) {
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
        if (!order_applied_) {
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
        resumeVehicle(pending_resume_position_);
        delete msg; return;
    }

    if (msg == clearance_poll_msg_) {
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

        scheduleAt(simTime() + clearance_poll_period_sec_, clearance_poll_msg_);
        return;
    }

    DemoBaseApplLayer::handleSelfMsg(msg);
}

// ── handlePositionUpdate ──────────────────────────────────────────────────────

void ResDBIntersectionApp::handlePositionUpdate(cObject* obj)
{
    DemoBaseApplLayer::handlePositionUpdate(obj);

    // Gap 9: detect departure and lock out cert protocol for this vehicle.
    if (current_phase_ == ConsensusPhase::EXECUTING && order_applied_) {
        std::string myCarId = "veh" + std::to_string(replicaId_);
        if (vehicleHasClearedIntersectionTraCI(myCarId)) {
            current_phase_ = ConsensusPhase::DEPARTED;
            cleared_time_ = simTime();
            std::cout << "[DEPARTED] Replica " << replicaId_ << " cleared intersection t=" << simTime() << "\n";
            std::cout << "[METRICS " << replicaId_ << "] Total Latency (cleared-stop): "
            << (cleared_time_ - stop_time_) << std::endl;

        }
    }

    if (order_applied_) return;

    double dist = getDistanceToIntersection();
    if (dist >= stop_distance_ || dist <= 0) return;

    if (!entered_stop_zone_) {
        entered_stop_zone_ = true;
        stop_time_ = simTime();
        current_phase_ = ConsensusPhase::WAITING_FOR_CLEARANCE;
        std::cout << "[METRICS " << replicaId_ << "] Stop_Time: " << stop_time_ << "\n";

        stopVehicle();
        discoverLane();

        // Safety fallback timers.
        stop_sign_timeout_msg_ = new cMessage("resdbStopSignTimeout");
        scheduleAt(simTime() + stop_sign_timeout_sec_, stop_sign_timeout_msg_);
        consensus_timeout_msg_ = new cMessage("resdbConsensusTimeout");
        scheduleAt(simTime() + consensus_timeout_sec_, consensus_timeout_msg_);
        std::cout << "[VC-DEBUG] r" << replicaId_
                  << " stop-zone timers armed stop_sign_deadline="
                  << simTime() + stop_sign_timeout_sec_
                  << " consensus_deadline=" << simTime() + consensus_timeout_sec_ << "\n";

        int primary = ResdbOmnetGetPrimary(resdb_server_handle_);
        if (replicaId_ == primary && !propose_submitted_) {
            if (deferred_propose_after_cert_timeout_) {
                deferred_propose_after_cert_timeout_ = false;
                std::cout << "[ResDB r" << replicaId_
                          << "] stop zone: deferred cert-timeout — proposing with available certs\n";
                proposeAll();
            } else if ((int)collected_certs_.size() >= total_vehicles_) {
                proposeAll();
            } else if (!propose_timeout_msg_ || !propose_timeout_msg_->isScheduled()) {
                if (!propose_timeout_msg_)
                    propose_timeout_msg_ = new cMessage("resdbProposeTimeout");
                scheduleAt(simTime() + cert_collection_timeout_, propose_timeout_msg_);
            }
        } else if (replicaId_ != primary && !vc_trigger_msg_ && !order_applied_) {
            // Follower stop zone entry: arm VC trigger in case primary is silent.
            // Delay = cert-collection window the primary gets + our extra grace period.
            double vc_delay = cert_collection_timeout_.dbl() + pbft_vc_timeout_sec_;
            vc_trigger_msg_ = new cMessage("vc_trigger");
            scheduleAt(simTime() + vc_delay, vc_trigger_msg_);
            std::cout << "[VC-DEBUG] r" << replicaId_
                      << " follower stop zone: vc_trigger scheduled at "
                      << simTime() + vc_delay << " (primary=" << primary
                      << ", vc_delay_sec=" << vc_delay << ")\n";
        }
    }
}

// ── finish ────────────────────────────────────────────────────────────────────

void ResDBIntersectionApp::finish()
{
    // Stop global monitor as soon as the first replica enters finish(), before
    // long ResdbOmnetStopServer joins (and before stats.cpp sleep can emit again).
    ResdbOmnetStopGlobalStats();

    if (vc_trigger_msg_) {
        cancelEvent(vc_trigger_msg_);
        delete vc_trigger_msg_; vc_trigger_msg_ = nullptr;
    }
    if (resdb_server_handle_) {
        ResdbOmnetStopServer(resdb_server_handle_);
        ResdbOmnetDestroyServer(resdb_server_handle_);
        resdb_server_handle_ = nullptr;
    }
    DemoBaseApplLayer::finish();
}

// ── registerTransport ─────────────────────────────────────────────────────────

void ResDBIntersectionApp::registerTransport()
{
    ResdbOmnetTransportCallbacks cbs;
    cbs.send_to   = IV2VTransport::c_send_to;
    cbs.broadcast = IV2VTransport::c_broadcast;
    cbs.ctx       = transport_.get();
    ResdbOmnetSetTransport(resdb_server_handle_, &cbs);
}

// ── VeinsTransport ────────────────────────────────────────────────────────────

void ResDBIntersectionApp::VeinsTransport::sendTo(int toReplica,
                                                   const uint8_t* data, uint32_t len)
{ app_->enqueueOutbound(toReplica, data, len); }

void ResDBIntersectionApp::VeinsTransport::broadcast(const uint8_t* data, uint32_t len)
{ app_->enqueueOutbound(-1, data, len); }

void ResDBIntersectionApp::enqueueOutbound(int toReplicaId,
                                            const uint8_t* data, uint32_t len)
{
    if (!data || len == 0) return;
    PendingOutboundPacket pkt;
    pkt.toReplicaId = toReplicaId;
    pkt.resdbBytes.assign(data, data + len);
    std::lock_guard<std::mutex> lk(outbound_mutex_);
    outbound_queue_.push_back(std::move(pkt));
}

// drainOutboundQueue: signs and sends queued PBFT (Type-8) packets over the radio.
void ResDBIntersectionApp::drainOutboundQueue()
{
    std::deque<PendingOutboundPacket> local;
    {
        std::lock_guard<std::mutex> lk(outbound_mutex_);
        if (outbound_queue_.empty()) return;
        local.swap(outbound_queue_);
    }

    for (auto& pkt : local) {
        if (pkt.resdbBytes.empty() || !ec_private_key_) continue;

        std::vector<uint8_t> signed_payload = resdbwire::packSignedPacket(
            ec_private_key_, ec_pub_key_,
            pkt.resdbBytes.data(), (uint32_t)pkt.resdbBytes.size());
        if (signed_payload.empty()) continue;

        BFTMessage* bft = new BFTMessage();
        bft->setFromReplicaId(replicaId_);
        bft->setToReplicaId(pkt.toReplicaId);
        bft->setMessageType(kResdbConsensusMsgType);
        bft->setSequenceNum(sequenceNumber_++);
        bft->setTimestamp(simTime());
        bft->setPayloadArraySize(signed_payload.size());
        for (size_t i = 0; i < signed_payload.size(); ++i)
            bft->setPayload(i, signed_payload[i]);
        bft->setPayloadLength((int)signed_payload.size());
        bft->setRecipientAddress(LAddress::L2BROADCAST());
        bft->setChannelNumber((int)veins::Channel::cch);
        bft->addBitLength(par("headerLength"));
        bft->addBitLength((int)(signed_payload.size() * 8));

        // Per-replica stagger + jitter — same knobs as cert-protocol broadcasts.
        // replica i transmits at: i * broadcastSlotSec + uniform(jitterMin, jitterMax)
        // This prevents simultaneous PREPARE/COMMIT from all replicas colliding at
        // the MAC layer.
        double slot  = par("broadcastSlotSec").doubleValue();
        double jmin  = par("broadcastJitterMin").doubleValue();
        double jmax  = par("broadcastJitterMax").doubleValue();
        double delay = replicaId_ * slot + ((jmax > jmin) ? uniform(jmin, jmax) : 0.0);
        std::cout << "[TYPE8-DRAIN] r" << replicaId_ << " to=" << pkt.toReplicaId
                  << " resdbLen=" << pkt.resdbBytes.size()
                  << " signedLen=" << signed_payload.size()
                  << " delay=" << delay << " t=" << simTime() << "\n";
        sendDelayedDown(bft, delay);
    }
}

// ── sendBFTMessage: cert-protocol radio send (no PBFT crypto) ─────────────────

void ResDBIntersectionApp::sendBFTMessage(int toReplicaId,
                                           const std::vector<uint8_t>& payload,
                                           int msgType)
{
    if (payload.empty()) return;
    BFTMessage* bft = new BFTMessage();
    bft->setFromReplicaId(replicaId_);
    bft->setToReplicaId(toReplicaId);
    bft->setMessageType(msgType);
    bft->setSequenceNum(sequenceNumber_++);
    bft->setTimestamp(simTime());
    bft->setPayloadArraySize(payload.size());
    for (size_t i = 0; i < payload.size(); ++i)
        bft->setPayload(i, payload[i]);
    bft->setPayloadLength((int)payload.size());
    bft->setRecipientAddress(LAddress::L2BROADCAST());
    bft->setChannelNumber((int)veins::Channel::cch);
    bft->addBitLength(par("headerLength"));
    bft->addBitLength((int)(payload.size() * 8));
    double delaySec = 0;
    if (msgType == kArrivalEchoType) {
        delaySec = replicaId_ * par("viewAgreementSlotSec").doubleValue()
            + uniform(par("viewJitterMin").doubleValue(), par("viewJitterMax").doubleValue());
    } else if (msgType == kArrivalCertType) {
        delaySec = uniform(par("viewJitterMin").doubleValue(), par("viewJitterMax").doubleValue());
    } else if (msgType == kArrivalAnnounceType) {
        delaySec = replicaId_ * par("arrivalSlotSec").doubleValue()
            + uniform(par("broadcastJitterMin").doubleValue(), par("broadcastJitterMax").doubleValue());
    } else {
        delaySec = replicaId_ * par("broadcastSlotSec").doubleValue()
            + uniform(par("broadcastJitterMin").doubleValue(), par("broadcastJitterMax").doubleValue());
    }

    if (debug_cert_protocol_) {
        const char* kind = (msgType == kArrivalAnnounceType)   ? "ANN"
                           : (msgType == kArrivalEchoType)     ? "ECHO"
                           : (msgType == kArrivalCertType)     ? "CERT"
                           : "?";
        std::cout << "[CERT-DEBUG] sendBFT r" << replicaId_ << " kind=" << kind
                  << " type=" << msgType << " toReplicaId=" << toReplicaId
                  << " payloadBytes=" << payload.size() << " scheduleDelay=" << delaySec
                  << "s t=" << simTime() << "\n";
    }
    sendDelayedDown(bft, delaySec);
}

// ── onWSM ─────────────────────────────────────────────────────────────────────

void ResDBIntersectionApp::onWSM(BaseFrame1609_4* wsm)
{
    auto* bft = dynamic_cast<BFTMessage*>(wsm);
    if (!bft) return;
    if (bft->getFromReplicaId() == replicaId_) return;   // no self-delivery
    if (current_phase_ == ConsensusPhase::DEPARTED) return;  // Gap 9: zombie filter

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

    // ── Type 8: ResDB PBFT consensus bytes ────────────────────────────────────
    if (msgType == kResdbConsensusMsgType) {
        if (bft->getToReplicaId() != -1 && bft->getToReplicaId() != replicaId_) return;

        int plen = bft->getPayloadArraySize();
        if (plen <= 0) return;
        std::vector<uint8_t> buf((size_t)plen);
        for (int i = 0; i < plen; ++i) buf[i] = bft->getPayload(i);

        resdbwire::SignedPacketView view;
        if (!resdbwire::unpackSignedPacket(buf.data(), (uint32_t)buf.size(), &view)) return;
        if (view.resdbLen == 0) return;

        if (!CryptoAuth::instance().verifyBytes(view.pubKey, view.resdbBytes, view.resdbLen,
                                                view.sig, view.sigLen)) {
            std::cout << "[TYPE8-RECV] r" << replicaId_ << " dropped forged packet from "
                      << bft->getFromReplicaId() << "\n";
            return;
        }
        std::cout << "[TYPE8-RECV] r" << replicaId_ << " from=" << bft->getFromReplicaId()
                  << " resdbLen=" << view.resdbLen << " t=" << simTime() << "\n";
        int primary = ResdbOmnetGetPrimary(resdb_server_handle_);
        if (primary >= 0 && bft->getFromReplicaId() == primary)
            stopCertBroadcastRetries();

        ResdbOmnetDeliverPacket(resdb_server_handle_, bft->getFromReplicaId(),
                                view.resdbBytes, view.resdbLen);
    }
}

// ── Serialization helpers ─────────────────────────────────────────────────────

std::vector<uint8_t> ResDBIntersectionApp::serializeArrivalAnnouncement(
        const ArrivalAnnouncement& ann)
{
    std::stringstream ss;
    ss << std::setprecision(17);
    ss << ann.carId          << "|"
       << ann.laneId         << "|"
       << ann.lane           << "|"
       << ann.positionInLane << "|"
       << dirToStr(ann.direction) << "|"
       << (ann.isAmbulance ? "1" : "0") << "|"
       << ann.claimedArrivalTime << "|"
       << ann.epoch          << "|"
       << toHex(ann.ambulanceCertBytes) << "|"
       << toHex(ann.ambulanceSigBytes)  << "|"
       << ann.signature.size() << "|";
    std::string hdr = ss.str();
    std::vector<uint8_t> result(hdr.begin(), hdr.end());
    result.insert(result.end(), ann.signature.begin(), ann.signature.end());
    return result;
}

ResDBIntersectionApp::ArrivalAnnouncement
ResDBIntersectionApp::deserializeArrivalAnnouncement(BFTMessage* msg)
{
    std::vector<uint8_t> payload(msg->getPayloadArraySize());
    for (size_t i = 0; i < payload.size(); i++) payload[i] = msg->getPayload(i);
    std::string s(payload.begin(), payload.end());
    auto parts = splitStr(s, '|');
    ArrivalAnnouncement ann;
    if (parts.size() >= 11) {
        ann.carId              = parts[0];
        ann.laneId             = parts[1];
        ann.lane               = parts[2];
        ann.positionInLane     = std::stoi(parts[3]);
        ann.direction          = strToDir(parts[4]);
        ann.isAmbulance        = (parts[5] == "1");
        ann.claimedArrivalTime = std::stod(parts[6]);
        ann.epoch              = std::stoi(parts[7]);
        ann.ambulanceCertBytes = fromHex(parts[8]);
        ann.ambulanceSigBytes  = fromHex(parts[9]);
        int siglen = std::stoi(parts[10]);
        size_t p = s.find('|');
        for (int k = 1; k < 11 && p != std::string::npos; ++k) p = s.find('|', p + 1);
        size_t offset = (p != std::string::npos) ? p + 1 : s.size();
        if (offset < payload.size() && offset + (size_t)siglen <= payload.size())
            ann.signature.assign(payload.begin() + offset, payload.begin() + offset + siglen);
    }
    return ann;
}

std::vector<uint8_t> ResDBIntersectionApp::serializeArrivalEcho(const ArrivalEcho& echo)
{
    std::vector<uint8_t> pubVec(echo.signerPubKey, echo.signerPubKey + CRYPTO_PUBKEY_BYTES);
    std::vector<uint8_t> sigVec(echo.signature, echo.signature + echo.signatureLen);
    std::stringstream ss;
    ss << echo.echoingReplicaId << "|"
       << echo.targetCarId      << "|"
       << echo.lane             << "|"
       << echo.positionInLane   << "|"
       << dirToStr(echo.direction) << "|"
       << (echo.isAmbulance ? "1" : "0") << "|"
       << echo.epoch            << "|"
       << toHex(pubVec) << "," << toHex(sigVec);
    std::string s = ss.str();
    return std::vector<uint8_t>(s.begin(), s.end());
}

ResDBIntersectionApp::ArrivalEcho
ResDBIntersectionApp::deserializeArrivalEcho(BFTMessage* msg)
{
    std::vector<uint8_t> payload(msg->getPayloadArraySize());
    for (size_t i = 0; i < payload.size(); i++) payload[i] = msg->getPayload(i);
    std::string s(payload.begin(), payload.end());
    auto parts = splitStr(s, '|');
    ArrivalEcho echo;
    std::memset(echo.signerPubKey, 0, CRYPTO_PUBKEY_BYTES);
    std::memset(echo.signature, 0, CRYPTO_SIG_MAX_BYTES);
    echo.signatureLen = 0;
    if (parts.size() >= 8) {
        echo.echoingReplicaId = std::stoi(parts[0]);
        echo.targetCarId      = parts[1];
        echo.lane             = parts[2];
        echo.positionInLane   = std::stoi(parts[3]);
        echo.direction        = strToDir(parts[4]);
        echo.isAmbulance      = (parts[5] == "1");
        echo.epoch            = std::stoi(parts[6]);
        const std::string& sf = parts[7];
        size_t comma = sf.find(',');
        if (comma != std::string::npos) {
            auto pubVec = fromHex(sf.substr(0, comma));
            auto sigVec = fromHex(sf.substr(comma + 1));
            if (pubVec.size() == CRYPTO_PUBKEY_BYTES)
                std::memcpy(echo.signerPubKey, pubVec.data(), CRYPTO_PUBKEY_BYTES);
            if (sigVec.size() <= CRYPTO_SIG_MAX_BYTES) {
                std::memcpy(echo.signature, sigVec.data(), sigVec.size());
                echo.signatureLen = static_cast<uint8_t>(sigVec.size());
            }
        }
    }
    return echo;
}

std::vector<uint8_t> ResDBIntersectionApp::serializeArrivalCert(const ArrivalCert& cert)
{
    std::stringstream ss;
    ss << cert.carId          << "|"
       << cert.lane           << "|"
       << cert.positionInLane << "|"
       << dirToStr(cert.direction) << "|"
       << (cert.isAmbulance ? "1" : "0") << "|"
       << cert.epoch;
    for (const auto& echo : cert.echoes) {
        std::vector<uint8_t> pubVec(echo.signerPubKey, echo.signerPubKey + CRYPTO_PUBKEY_BYTES);
        std::vector<uint8_t> sigVec(echo.signature, echo.signature + echo.signatureLen);
        ss << "|" << echo.echoingReplicaId << ":"
           << toHex(pubVec) << "," << toHex(sigVec);
    }
    std::string s = ss.str();
    return std::vector<uint8_t>(s.begin(), s.end());
}

ResDBIntersectionApp::ArrivalCert
ResDBIntersectionApp::deserializeArrivalCert(BFTMessage* msg)
{
    std::vector<uint8_t> payload(msg->getPayloadArraySize());
    for (size_t i = 0; i < payload.size(); i++) payload[i] = msg->getPayload(i);
    std::string s(payload.begin(), payload.end());
    auto parts = splitStr(s, '|');
    ArrivalCert cert;
    if (parts.size() < 6) return cert;
    cert.carId          = parts[0];
    cert.lane           = parts[1];
    cert.positionInLane = std::stoi(parts[2]);
    cert.direction      = strToDir(parts[3]);
    cert.isAmbulance    = (parts[4] == "1");
    cert.epoch          = std::stoi(parts[5]);
    for (size_t i = 6; i < parts.size(); i++) {
        size_t colon = parts[i].find(':');
        if (colon == std::string::npos) continue;
        ArrivalEcho echo;
        std::memset(echo.signerPubKey, 0, CRYPTO_PUBKEY_BYTES);
        std::memset(echo.signature, 0, CRYPTO_SIG_MAX_BYTES);
        echo.signatureLen = 0;
        echo.echoingReplicaId = std::stoi(parts[i].substr(0, colon));
        std::string sf = parts[i].substr(colon + 1);
        size_t comma = sf.find(',');
        if (comma != std::string::npos) {
            auto pubVec = fromHex(sf.substr(0, comma));
            auto sigVec = fromHex(sf.substr(comma + 1));
            if (pubVec.size() == CRYPTO_PUBKEY_BYTES)
                std::memcpy(echo.signerPubKey, pubVec.data(), CRYPTO_PUBKEY_BYTES);
            if (sigVec.size() <= CRYPTO_SIG_MAX_BYTES) {
                std::memcpy(echo.signature, sigVec.data(), sigVec.size());
                echo.signatureLen = static_cast<uint8_t>(sigVec.size());
            }
        }
        echo.targetCarId      = cert.carId;
        echo.lane             = cert.lane;
        echo.positionInLane   = cert.positionInLane;
        echo.direction        = cert.direction;
        echo.isAmbulance      = cert.isAmbulance;
        echo.epoch            = cert.epoch;
        cert.echoes.push_back(echo);
    }
    return cert;
}

// ── tryStartCertCollectionTimer (align with V2V: deadline from first leader observation) ──

void ResDBIntersectionApp::tryStartCertCollectionTimer()
{
    if (!resdb_server_handle_ || propose_submitted_) return;
    if (replicaId_ != ResdbOmnetGetPrimary(resdb_server_handle_)) return;
    if ((int)collected_certs_.size() >= total_vehicles_) return;
    if (cert_collection_started_) return;
    if (propose_timeout_msg_ && propose_timeout_msg_->isScheduled()) return;

    if (!propose_timeout_msg_)
        propose_timeout_msg_ = new cMessage("resdbProposeTimeout");
    scheduleAt(simTime() + cert_collection_timeout_, propose_timeout_msg_);
    cert_collection_started_ = true;
    std::cout << "[ResDB r" << replicaId_
              << "] Leader: cert-collection deadline armed at first observation (timeout="
              << cert_collection_timeout_ << "s)\n";
}

// ── broadcastArrivalAnnouncement (port of V2VArrivalProtocol::broadcastArrivalAnnouncement) ──

void ResDBIntersectionApp::broadcastArrivalAnnouncement()
{
    if (current_phase_ == ConsensusPhase::DEPARTED || order_applied_) return;
    if (!mobility) return;

    TraCICommandInterface* traci = mobility->getCommandInterface();
    if (!traci) return;

    std::string myCarId = "veh" + std::to_string(replicaId_);
    ArrivalAnnouncement ann;
    ann.carId = myCarId;

    try {
        ann.laneId = traci->vehicle(myCarId).getLaneId();
    } catch (...) { ann.laneId = ""; }

    // intendedLane NED param takes precedence when explicitly set (N/S/E/W).
    // This bypasses SUMO edge-naming ambiguity (reversed edges start with '-',
    // internal junction edges start with ':', etc.).
    // Fall back to TraCI lane-ID parsing only when intendedLane is empty.
    const bool lane_override = (intended_lane_ == "N" || intended_lane_ == "S" ||
                                intended_lane_ == "E" || intended_lane_ == "W");
    if (lane_override) {
        ann.lane = intended_lane_;
    } else if (!ann.laneId.empty()) {
        char c = std::toupper(ann.laneId[0]);
        ann.lane = (c=='N'||c=='S'||c=='E'||c=='W') ? std::string(1,c) : "N";
    } else {
        ann.lane = "N";
    }

    if (is_byzantine_ && byzantine_type_ == BYZANTINE_FALSE_LANE) {
        ann.laneId = "BYZANTINE_FAKE_LANE";
        ann.lane   = "X";
        std::cout << "[BYZANTINE] r" << replicaId_ << " FALSE_LANE: laneId=BYZANTINE_FAKE_LANE\n";
    }

    {
        int rank = 1;
        auto it = std::find(lane_queue_.begin(), lane_queue_.end(), myCarId);
        if (it != lane_queue_.end()) rank = (int)(it - lane_queue_.begin()) + 1;
        ann.positionInLane = rank;
    }

    ann.direction          = strToDir(intended_direction_);
    ann.isAmbulance        = is_ambulance_;
    ann.claimedArrivalTime = simTime().dbl();
    ann.epoch              = (int)current_epoch_;

    // ECDSA P-256 self-signed claim.
    if (ec_private_key_) {
        std::string toSign = ann.carId + ":" + ann.laneId + ":" +
                             std::to_string(ann.positionInLane) + ":" +
                             std::to_string(ann.claimedArrivalTime) + ":" +
                             std::to_string(ann.epoch);
        uint8_t sigOut[CRYPTO_SIG_MAX_BYTES]; uint8_t sigLen = 0;
        if (CryptoAuth::instance().signBytes(ec_private_key_,
                reinterpret_cast<const uint8_t*>(toSign.c_str()), toSign.size(),
                sigOut, sigLen))
            ann.signature.assign(sigOut, sigOut + sigLen);
    }

    // Self-store.
    {
        VehicleState selfVS;
        selfVS.vehicleId       = myCarId;
        selfVS.lane            = ann.lane;
        selfVS.positionInLane  = ann.positionInLane;
        selfVS.direction       = ann.direction;
        selfVS.isAmbulance     = ann.isAmbulance;
        selfVS.arrival_time_us = (uint64_t)(ann.claimedArrivalTime * 1e6);
        local_vehicle_states_[myCarId]             = selfVS;
        arrival_announcements_received_.insert(myCarId);
        physically_observed_cars_.insert(myCarId);
    }

    if (is_byzantine_ && byzantine_type_ == BYZANTINE_EQUIVOCATOR) {
        int n = total_vehicles_;
        for (int peerId = 0; peerId < n; ++peerId) {
            if (peerId == replicaId_) continue;
            ArrivalAnnouncement annByz = ann;
            annByz.direction = (peerId < n / 2) ? DIR_LEFT : DIR_RIGHT;
            std::cout << "[BYZANTINE] r" << replicaId_ << " EQUIVOCATOR: peer "
                      << peerId << " dir=" << (peerId < n/2 ? "L" : "R") << "\n";
            sendBFTMessage(peerId, serializeArrivalAnnouncement(annByz), kArrivalAnnounceType);
        }
    } else {
        std::vector<uint8_t> payload = serializeArrivalAnnouncement(ann);
        sendBFTMessage(-1, payload, kArrivalAnnounceType);
        std::cout << "[ANN-BROADCAST] Replica " << replicaId_ << " broadcast ARRIVAL_ANNOUNCE at t="
                  << simTime() << " lane=" << ann.lane << "\n";
    }

    tryStartCertCollectionTimer();
}

// ── handleArrivalAnnouncement ─────────────────────────────────────────────────

void ResDBIntersectionApp::handleArrivalAnnouncement(BFTMessage* msg)
{
    std::cout << "[ANN-RECV] Replica " << replicaId_ << " received ARRIVAL_ANNOUNCE from "
              << msg->getFromReplicaId() << " at t=" << simTime() << "\n";
    ArrivalAnnouncement ann = deserializeArrivalAnnouncement(msg);
    if (ann.carId.empty()) return;

    // Dedup.
    if (local_vehicle_states_.count(ann.carId)) return;

    // Verify lane via TraCI.
    VerificationResult result = verifyCarPosition(ann.carId, ann.laneId, ann.positionInLane, 1e9);

    if (!result.isValid) {
        if (result.actualLaneId.empty()) return;  // not in simulation at all
        // Physically present but wrong lane — record without echoing.
        VehicleState vs;
        vs.vehicleId      = ann.carId;
        char c = result.actualLaneId.empty() ? 'N' : std::toupper(result.actualLaneId[0]);
        vs.lane           = (c=='N'||c=='S'||c=='E'||c=='W') ? std::string(1,c) : "N";
        vs.positionInLane = ann.positionInLane;
        vs.direction      = ann.direction;
        vs.isAmbulance    = false;  // don't trust ambulance claim from lane-liar
        vs.arrival_time_us = (uint64_t)(ann.claimedArrivalTime * 1e6);
        local_vehicle_states_[ann.carId] = vs;
        arrival_announcements_received_.insert(ann.carId);
        physically_observed_cars_.insert(ann.carId);
        std::cout << "[ANN-RECV] Replica " << replicaId_ << " FALSE_LANE from "
                  << ann.carId << " — no echo\n";
        tryStartCertCollectionTimer();
        return;
    }

    // Build and store VehicleState.
    VehicleState vs;
    vs.vehicleId       = ann.carId;
    vs.lane            = ann.lane;
    vs.positionInLane  = ann.positionInLane;
    vs.direction       = ann.direction;
    vs.isAmbulance     = ann.isAmbulance;  // trust for now; hardened cert path future work
    vs.arrival_time_us = (uint64_t)(ann.claimedArrivalTime * 1e6);
    local_vehicle_states_[ann.carId] = vs;
    arrival_announcements_received_.insert(ann.carId);
    physically_observed_cars_.insert(ann.carId);

    sendArrivalEcho(ann);

    std::cout << "[ANN-RECV] Replica " << replicaId_ << " stored VehicleState for "
              << ann.carId << " (" << physically_observed_cars_.size()
              << "/" << total_vehicles_ << " observed)\n";

    //cancel broadcastArrivalAnnouncement timer once we have observed all vehicles at the intersection
    if (physically_observed_cars_.size() == total_vehicles_) {
        std::cout << "[ANN-RECV] Replica " << replicaId_ << " all vehicles observed at intersection, cancelling broadcastArrivalAnnouncement timer\n";

        if (broadcastArrivalAnnouncement_timer_) { cancelAndDelete(broadcastArrivalAnnouncement_timer_); broadcastArrivalAnnouncement_timer_ = nullptr; }

         
        current_phase_ = ConsensusPhase::COLLECTING_CERTS;

    }

    tryStartCertCollectionTimer();
}

// ── sendArrivalEcho ───────────────────────────────────────────────────────────

void ResDBIntersectionApp::sendArrivalEcho(const ArrivalAnnouncement& ann)
{
    if (!ec_private_key_) return;
    std::string toSign = ann.carId + ":" + ann.lane + ":" +
        std::to_string(ann.positionInLane) + ":" +
        (ann.direction == DIR_LEFT ? "L" : ann.direction == DIR_RIGHT ? "R" : "S") +
        ":" + (ann.isAmbulance ? "1" : "0") +
        ":" + std::to_string(replicaId_);

    ArrivalEcho echo;
    echo.echoingReplicaId = replicaId_;
    echo.targetCarId      = ann.carId;
    echo.lane             = ann.lane;
    echo.positionInLane   = ann.positionInLane;
    echo.direction        = ann.direction;
    echo.isAmbulance      = ann.isAmbulance;
    echo.epoch            = ann.epoch;
    std::memcpy(echo.signerPubKey, ec_pub_key_, CRYPTO_PUBKEY_BYTES);
    std::memset(echo.signature, 0, CRYPTO_SIG_MAX_BYTES);
    echo.signatureLen = 0;

    if (!CryptoAuth::instance().signBytes(ec_private_key_,
            reinterpret_cast<const uint8_t*>(toSign.c_str()), toSign.size(),
            echo.signature, echo.signatureLen)) {
        std::cerr << "[ECHO-SEND] Replica " << replicaId_ << " sign failed for echo of "
                  << ann.carId << "\n";
        return;
    }

    if (is_byzantine_ && byzantine_type_ == BYZANTINE_INVALID_SIG) {
        echo.signature[0] = echo.signature[1] =
        echo.signature[2] = echo.signature[3] = 0xDE;
        echo.signatureLen = 4;
        std::cout << "[BYZANTINE] r" << replicaId_ << " INVALID_SIG: corrupted echo for "
                  << ann.carId << "\n";
    }

    int targetId = extractReplicaId(ann.carId);
    sendBFTMessage(targetId, serializeArrivalEcho(echo), kArrivalEchoType);
    std::cout << "[ECHO-SEND] Replica " << replicaId_ << " → " << ann.carId
              << " ARRIVAL_ECHO sigLen=" << (int)echo.signatureLen << "\n";
    if (debug_cert_protocol_) {
        std::cout << "[CERT-DEBUG] sendArrivalEcho r" << replicaId_ << " targetReplicaId=" << targetId
                  << " signPayload=\"" << toSign << "\"\n";
    }
}

// ── handleArrivalEcho ─────────────────────────────────────────────────────────

void ResDBIntersectionApp::handleArrivalEcho(BFTMessage* msg)
{
    ArrivalEcho echo = deserializeArrivalEcho(msg);
    if (debug_cert_protocol_)
        std::cout << "[CERT-DEBUG] handleArrivalEcho r" << replicaId_
                  << " frameFrom=r" << msg->getFromReplicaId()
                  << " echo.target=" << echo.targetCarId
                  << " echo.signer=r" << echo.echoingReplicaId << "\n";
    std::string myCarId = "veh" + std::to_string(replicaId_);
    if (echo.targetCarId != myCarId) {
        if (debug_cert_protocol_)
            std::cout << "[CERT-DEBUG] handleArrivalEcho r" << replicaId_
                      << " wrong target: echo for " << echo.targetCarId
                      << " (my " << myCarId << ") from r" << echo.echoingReplicaId << "\n";
        return;
    }
    if (cert_broadcast_) {
        if (debug_cert_protocol_)
            std::cout << "[CERT-DEBUG] handleArrivalEcho r" << replicaId_
                      << " drop echo from r" << echo.echoingReplicaId
                      << " (cert_broadcast_ already true)\n";
        return;
    }

    auto& echoes = my_received_echoes_[myCarId];
    for (const auto& e : echoes)
        if (e.echoingReplicaId == echo.echoingReplicaId) {
            if (debug_cert_protocol_)
                std::cout << "[CERT-DEBUG] handleArrivalEcho r" << replicaId_
                          << " dedup echo from r" << echo.echoingReplicaId << "\n";
            return;  // dedup
        }

    echoes.push_back(echo);
    std::cout << "[ECHO-RECV] Replica " << replicaId_ << " received echo from "
              << echo.echoingReplicaId << " (" << echoes.size() << " so far)\n";

    int f        = (total_vehicles_ - 1) / 3;
    int required = f + 1;
    if (debug_cert_protocol_)
        std::cout << "[CERT-DEBUG] handleArrivalEcho r" << replicaId_
                  << " progress " << echoes.size() << "/" << required
                  << " (f=" << f << ")\n";
    if ((int)echoes.size() >= required) {
        cert_broadcast_ = true;
        ArrivalCert cert;
        cert.carId = myCarId;
        if (local_vehicle_states_.count(myCarId)) {
            const VehicleState& sv = local_vehicle_states_.at(myCarId);
            cert.lane           = sv.lane;
            cert.positionInLane = sv.positionInLane;
            cert.direction      = sv.direction;
            cert.isAmbulance    = sv.isAmbulance;
        }
        cert.epoch  = (int)current_epoch_;
        cert.echoes = echoes;
        std::cout << "[CERT-ASSEMBLE] Replica " << replicaId_ << " assembled ARRIVAL_CERT with "
                  << cert.echoes.size() << " echoes — broadcasting\n";
        broadcastArrivalCert(cert);
    }
}

// ── broadcastArrivalCert ──────────────────────────────────────────────────────

void ResDBIntersectionApp::scheduleNextCertRetry()
{
    if (!enable_cert_retries_ || cert_pending_retries_.carId.empty()) return;
    if (propose_submitted_ || order_applied_) {
        stopCertBroadcastRetries();
        return;
    }
    if (!cert_retry_timer_)
        cert_retry_timer_ = new cMessage("resdbCertRetry");
    scheduleAt(simTime() + cert_retry_interval_, cert_retry_timer_);
}

void ResDBIntersectionApp::stopCertBroadcastRetries()
{
    if (cert_retry_timer_) {
        if (cert_retry_timer_->isScheduled())
            cancelEvent(cert_retry_timer_);
        delete cert_retry_timer_;
        cert_retry_timer_ = nullptr;
    }
    cert_pending_retries_.carId.clear();
    cert_pending_retries_.echoes.clear();
    cert_retry_count_ = 0;
}

void ResDBIntersectionApp::broadcastArrivalCert(const ArrivalCert& cert)
{
    sendBFTMessage(-1, serializeArrivalCert(cert), kArrivalCertType);
    std::cout << "[CERT-BROADCAST] Replica " << replicaId_ << " broadcast ARRIVAL_CERT for "
              << cert.carId << "\n";

    if (enable_cert_retries_) {
        cert_pending_retries_ = cert;
        cert_retry_count_   = 0;
        if (cert_retry_timer_) {
            if (cert_retry_timer_->isScheduled())
                cancelEvent(cert_retry_timer_);
        } else {
            cert_retry_timer_ = new cMessage("resdbCertRetry");
        }
        scheduleNextCertRetry();
    }

    // OMNeT++ modules don't receive their own channel broadcasts — self-store.
    if (!collected_certs_.count(cert.carId)) {
        collected_certs_[cert.carId] = cert;
        std::cout << "[CERT-STORED-SELF] Replica " << replicaId_ << " self-stored cert for "
                  << cert.carId << " (" << collected_certs_.size() << "/" << total_vehicles_ << ")\n";
        // If primary and in stop zone and all certs now collected → propose immediately.
        if (replicaId_ == ResdbOmnetGetPrimary(resdb_server_handle_)
                && entered_stop_zone_ && !propose_submitted_) {
            if ((int)collected_certs_.size() >= total_vehicles_) {
                if (propose_timeout_msg_) {
                    cancelEvent(propose_timeout_msg_);
                    delete propose_timeout_msg_;
                    propose_timeout_msg_ = nullptr;
                }
                proposeAll();
            }
        }
    }
}

// ── handleArrivalCert ─────────────────────────────────────────────────────────

void ResDBIntersectionApp::handleArrivalCert(BFTMessage* msg)
{
    ArrivalCert cert = deserializeArrivalCert(msg);
    if (cert.carId.empty()) return;
    if (!validateArrivalCert(cert)) {
        std::cout << "[CERT-INVALID] Replica " << replicaId_ << " dropped ARRIVAL_CERT from "
                  << cert.carId << "\n";
        return;
    }
    if (collected_certs_.count(cert.carId)) return;  // dedup

    collected_certs_[cert.carId] = cert;
    std::cout << "[CERT-STORED] Replica " << replicaId_ << " stored ARRIVAL_CERT for "
              << cert.carId << " (" << collected_certs_.size() << "/" << total_vehicles_ << ")\n";

    // Reconstruct VehicleState if the announce was lost.
    if (!local_vehicle_states_.count(cert.carId)) {
        VehicleState vs;
        vs.vehicleId      = cert.carId;
        vs.lane           = cert.lane;
        vs.positionInLane = cert.positionInLane;
        vs.direction      = cert.direction;
        vs.isAmbulance    = cert.isAmbulance;
        local_vehicle_states_[cert.carId] = vs;
        physically_observed_cars_.insert(cert.carId);
    }

    // Primary: if in stop zone and all certs collected → propose.
    if (replicaId_ == ResdbOmnetGetPrimary(resdb_server_handle_)
            && entered_stop_zone_ && !propose_submitted_) {
        if ((int)collected_certs_.size() >= total_vehicles_) {
            if (propose_timeout_msg_) {
                cancelEvent(propose_timeout_msg_);
                delete propose_timeout_msg_;
                propose_timeout_msg_ = nullptr;
            }
            proposeAll();
        }
    }
}

// ── validateArrivalCert ───────────────────────────────────────────────────────

bool ResDBIntersectionApp::validateArrivalCert(const ArrivalCert& cert)
{
    int f = (total_vehicles_ - 1) / 3;
    int required = f + 1;
    if ((int)cert.echoes.size() < required) {
        if (debug_cert_protocol_)
            std::cout << "[CERT-DEBUG] validateArrivalCert fail: carId=" << cert.carId
                      << " echo_count=" << cert.echoes.size() << " need>=" << required << "\n";
        return false;
    }

    std::set<int> seen;
    int valid = 0;
    for (const auto& echo : cert.echoes) {
        if (!seen.insert(echo.echoingReplicaId).second) {
            if (debug_cert_protocol_)
                std::cout << "[CERT-DEBUG] validateArrivalCert skip duplicate signer r"
                          << echo.echoingReplicaId << " carId=" << cert.carId << "\n";
            continue;
        }
        if (echo.signatureLen == 0) {
            if (debug_cert_protocol_)
                std::cout << "[CERT-DEBUG] validateArrivalCert skip empty sig r"
                          << echo.echoingReplicaId << " carId=" << cert.carId << "\n";
            continue;
        }
        std::string dir = (cert.direction == DIR_LEFT ? "L" :
                           cert.direction == DIR_RIGHT ? "R" : "S");
        std::string toSign = cert.carId + ":" + cert.lane + ":" +
            std::to_string(cert.positionInLane) + ":" + dir + ":" +
            (cert.isAmbulance ? "1" : "0") + ":" +
            std::to_string(echo.echoingReplicaId);
        if (CryptoAuth::instance().verifyBytes(
                echo.signerPubKey,
                reinterpret_cast<const uint8_t*>(toSign.c_str()), toSign.size(),
                echo.signature, echo.signatureLen)) {
            valid++;
        } else if (debug_cert_protocol_) {
            std::cout << "[CERT-DEBUG] validateArrivalCert verify FAIL carId=" << cert.carId
                      << " signer=r" << echo.echoingReplicaId
                      << " signedPayload=\"" << toSign << "\" sigLen=" << (int)echo.signatureLen
                      << "\n";
        }
    }
    if (valid >= required)
        return true;
    if (debug_cert_protocol_)
        std::cout << "[CERT-DEBUG] validateArrivalCert fail: carId=" << cert.carId
                  << " valid_sigs=" << valid << " need=" << required << "\n";
    return false;
}

// ── proposeAll ────────────────────────────────────────────────────────────────

void ResDBIntersectionApp::proposeAll()
{
    if (propose_submitted_) {
        std::cout << "[VC-DEBUG] r" << replicaId_
                  << " proposeAll skipped: already submitted at " << propose_time_ << "\n";
        return;
    }
    deferred_propose_after_cert_timeout_ = false;
    stopCertBroadcastRetries();
    propose_submitted_ = true;
    propose_time_ = simTime();
    current_phase_ = ConsensusPhase::WAITING_FOR_CLEARANCE;
    std::cout << "[METRICS " << replicaId_ << "] ProposeAll_Submit_Time: " << propose_time_ << "\n";
    std::cout << "[VC-TRACE] r" << replicaId_
              << " proposeAll context phase=" << phaseToStr(current_phase_)
              << " certs=" << collected_certs_.size() << "/" << total_vehicles_
              << " observed=" << physically_observed_cars_.size()
              << " primary=" << ResdbOmnetGetPrimary(resdb_server_handle_);
    if (stop_time_ >= SIMTIME_ZERO)
        std::cout << " stop_to_propose_sec=" << (simTime() - stop_time_).dbl();
    std::cout << "\n";

    // Ensure own entry exists (self-announce may not have been processed by cert protocol yet).
    std::string myCarId = "veh" + std::to_string(replicaId_);
    if (!collected_certs_.count(myCarId)) {
        // Build a minimal self-cert if we don't have f+1 echoes yet.
        ArrivalCert selfCert;
        selfCert.carId         = myCarId;
        selfCert.isAmbulance   = is_ambulance_;
        selfCert.epoch         = (int)current_epoch_;
        if (local_vehicle_states_.count(myCarId)) {
            const VehicleState& sv = local_vehicle_states_.at(myCarId);
            selfCert.lane           = sv.lane;
            selfCert.positionInLane = sv.positionInLane;
            selfCert.direction      = sv.direction;
        }
        collected_certs_[myCarId] = selfCert;
    }
    // Ensure own arrival_time_us is set if missing.
    if (local_vehicle_states_.count(myCarId)
            && local_vehicle_states_[myCarId].arrival_time_us == 0) {
        local_vehicle_states_[myCarId].arrival_time_us =
            (stop_time_ >= SIMTIME_ZERO)
                ? (uint64_t)stop_time_.inUnit(SIMTIME_US)
                : (uint64_t)simTime().inUnit(SIMTIME_US);
    }

    // Lane string → numeric encoding (0=N,1=S,2=E,3=W).
    auto laneCode = [](const std::string& l) -> uint8_t {
        if (l == "S") return 1;
        if (l == "E") return 2;
        if (l == "W") return 3;
        return 0;  // "N" or unknown
    };
    // Direction enum → numeric (0=Straight,1=Left,2=Right).
    auto dirCode = [](Direction d) -> uint8_t {
        if (d == DIR_LEFT)  return 1;
        if (d == DIR_RIGHT) return 2;
        return 0;
    };

    // Pack ResdbProposeHdr + ResdbVehicleEntry per collected cert.
    // Vehicles that have a cert → SIGNED (cyber_status=1).
    // Missing vehicles → QUIET (cyber_status=0, sim_time_us=UINT64_MAX).
    std::set<int> present_ids;
    std::vector<ResdbVehicleEntry> entries;
    int f_val = (total_vehicles_ - 1) / 3;
    (void)f_val;  // f threshold already enforced at cert-collection time
    for (const auto& kv : collected_certs_) {
        ResdbVehicleEntry e{};
        e.replica_id   = extractReplicaId(kv.first);
        e.is_ambulance = kv.second.isAmbulance ? 1 : 0;
        e.cyber_status = 1;  // SIGNED — has a valid cert with f+1 echoes
        if (local_vehicle_states_.count(kv.first)) {
            const VehicleState& vs = local_vehicle_states_.at(kv.first);
            e.sim_time_us      = vs.arrival_time_us;
            e.lane             = laneCode(vs.lane);
            e.direction        = dirCode(vs.direction);
            e.position_in_lane = static_cast<uint8_t>(
                std::min(vs.positionInLane, 255));
        } else {
            // ResDB executor keys batching on lane/direction; leaving lane=0
            // makes every car look like North and forces singleton batches.
            const ArrivalCert& c = kv.second;
            e.lane             = laneCode(c.lane);
            e.direction        = dirCode(c.direction);
            e.position_in_lane = static_cast<uint8_t>(std::min(c.positionInLane, 255));
        }
        if (e.sim_time_us == 0)
            e.sim_time_us = (uint64_t)simTime().inUnit(SIMTIME_US);
        entries.push_back(e);
        present_ids.insert(e.replica_id);
    }
    // Add synthetic QUIET entries for any missing replica IDs.
    for (int rid = 0; rid < total_vehicles_; ++rid) {
        if (!present_ids.count(rid)) {
            ResdbVehicleEntry quiet{};
            quiet.replica_id      = rid;
            quiet.sim_time_us     = UINT64_MAX;  // QUIET sentinel
            quiet.is_ambulance    = 0;
            quiet.lane            = 0;
            quiet.direction       = 0;
            quiet.position_in_lane = 0;
            quiet.cyber_status    = 0;  // QUIET — no f+1 echoes by timeout
            entries.push_back(quiet);
            std::cout << "[ResDB r" << replicaId_ << "] proposeAll: QUIET entry for replica " << rid << "\n";
        }
    }

    uint32_t n = (uint32_t)entries.size();
    if (n == 0) {
        std::cout << "[ResDB r" << replicaId_ << "] proposeAll: no entries, aborting\n";
        return;
    }

    size_t total = sizeof(ResdbProposeHdr) + n * sizeof(ResdbVehicleEntry);
    std::vector<uint8_t> buf(total);
    uint8_t* p = buf.data();

    ResdbProposeHdr hdr;
    hdr.epoch               = current_epoch_;
    hdr.leader_id           = replicaId_;
    hdr.propose_sim_time_us = (uint64_t)simTime().inUnit(SIMTIME_US);
    hdr.n_vehicles          = n;
    std::memcpy(p, &hdr, sizeof(hdr)); p += sizeof(hdr);
    for (const auto& e : entries) {
        std::memcpy(p, &e, sizeof(e)); p += sizeof(e);
    }

    // Byzantine primary fault injection.
    if (is_byzantine_ && byzantine_type_ == BYZANTINE_SILENT_PRIMARY) {
        std::cout << "[BYZANTINE] r" << replicaId_
                  << " SILENT_PRIMARY: suppressing propose at " << simTime() << "\n";
        return;
    }
    if (is_byzantine_ && byzantine_type_ == BYZANTINE_BAD_PROPOSAL) {
        hdr.n_vehicles = hdr.n_vehicles + 1;  // fails PreVerify check 2
        std::memcpy(buf.data(), &hdr, sizeof(hdr));
        std::cout << "[BYZANTINE] r" << replicaId_
                  << " BAD_PROPOSAL: corrupted n_vehicles=" << hdr.n_vehicles
                  << " at " << simTime() << "\n";
    }

    int rc = ResdbOmnetTriggerConsensus(resdb_server_handle_, buf.data(), (uint32_t)buf.size());
    std::cout << "[ResDB r" << replicaId_ << "] TriggerConsensus rc=" << rc
              << " vehicles=" << n << "\n";
}

// ── onOrderDecided (ResDB worker thread) ─────────────────────────────────────

/*static*/ void ResDBIntersectionApp::onOrderDecided(void* ctx,
                                                      const uint8_t* bytes, uint32_t len)
{
    auto* app = static_cast<ResDBIntersectionApp*>(ctx);
    if (!bytes || len == 0) return;
    std::vector<uint8_t> copy(bytes, bytes + len);
    std::lock_guard<std::mutex> lk(app->orders_mutex_);
    app->pending_orders_.push_back(std::move(copy));
}

// ── processOrders (simulation thread, called from transport poll) ─────────────

void ResDBIntersectionApp::processOrders()
{
    std::deque<std::vector<uint8_t>> local;
    {
        std::lock_guard<std::mutex> lk(orders_mutex_);
        if (pending_orders_.empty()) return;
        local.swap(pending_orders_);
    }

    for (const auto& dec : local) {
        if (order_applied_) break;
        if (dec.size() < sizeof(ResdbOrderHdr)) continue;

        ResdbOrderHdr ohdr;
        std::memcpy(&ohdr, dec.data(), sizeof(ohdr));
        // New format: n_vehicles × ResdbVehicleDecision (8 bytes each).
        if (dec.size() < sizeof(ResdbOrderHdr) +
                         ohdr.n_vehicles * sizeof(ResdbVehicleDecision)) continue;

        const ResdbVehicleDecision* decisions = reinterpret_cast<const ResdbVehicleDecision*>(
            dec.data() + sizeof(ResdbOrderHdr));

        std::cout << "[METRICS " << replicaId_ << "] Order_Decided_Time: " << simTime()
                  << " n_batches=" << ohdr.n_batches << "\n";
        if (propose_time_ >= SIMTIME_ZERO) {
            std::cout << "[VC-TRACE] r" << replicaId_
                      << " propose_to_order_sec=" << (simTime() - propose_time_).dbl()
                      << " stop_to_order_sec="
                      << ((stop_time_ >= SIMTIME_ZERO) ? (simTime() - stop_time_).dbl() : -1.0)
                      << "\n";
        }

        // Cancel safety timers now that consensus delivered.
        if (vc_trigger_msg_) {
            cancelEvent(vc_trigger_msg_);
            delete vc_trigger_msg_; vc_trigger_msg_ = nullptr;
        }
        if (stop_sign_timeout_msg_) {
            cancelEvent(stop_sign_timeout_msg_);
            delete stop_sign_timeout_msg_; stop_sign_timeout_msg_ = nullptr;
        }
        if (consensus_timeout_msg_) {
            cancelEvent(consensus_timeout_msg_);
            delete consensus_timeout_msg_; consensus_timeout_msg_ = nullptr;
        }

        // Find own batch index.
        int my_batch = -1;
        for (uint32_t i = 0; i < ohdr.n_vehicles; ++i)
            if (decisions[i].replica_id == replicaId_)
                { my_batch = (int)decisions[i].batch_index; break; }
        if (my_batch < 0) continue;

        order_applied_ = true;
        stopCertBroadcastRetries();
        current_phase_ = ConsensusPhase::EXECUTING;
        my_batch_index_ = my_batch;

        // Collect vehicles in the preceding batch (needed for clearance gating).
        preceding_batch_cars_.clear();
        if (my_batch > 0) {
            for (uint32_t i = 0; i < ohdr.n_vehicles; ++i)
                if ((int)decisions[i].batch_index == my_batch - 1)
                    preceding_batch_cars_.push_back(decisions[i].replica_id);
        }

        std::cout << "[METRICS " << replicaId_ << "] Batch_Assignment: batch="
                  << my_batch << " preceding_count=" << preceding_batch_cars_.size() << "\n";

        if (my_batch == 0) {
            // Batch 0: go immediately — no predecessors.
            std::cout << "[METRICS " << replicaId_ << "] Resume_Time: " << simTime()
                      << " (batch=0)\n";
            resumeVehicle(0);
        } else {
            // Wait for all vehicles in batch (my_batch - 1) to clear the
            // intersection via TraCI. Mirrors V2VOrderProtocol.cc executeBatch().
            clearance_started_ = simTime();
            std::cout << "[CLEARANCE r" << replicaId_ << "] batch=" << my_batch
                      << " waiting for " << preceding_batch_cars_.size()
                      << " vehicle(s) in batch " << (my_batch - 1) << " to clear\n";
            if (!clearance_poll_msg_)
                clearance_poll_msg_ = new cMessage("resdbClearancePoll");
            if (clearance_poll_msg_->isScheduled()) cancelEvent(clearance_poll_msg_);
            scheduleAt(simTime() + clearance_poll_period_sec_, clearance_poll_msg_);
        }
    }
}




void ResDBIntersectionApp::resumeVehicle(int position_in_order)
{
    TraCICommandInterface::Vehicle* vc = mobility ? mobility->getVehicleCommandInterface() : nullptr;
    if (!vc) return;
    std::cout << "[V2VResDB r" << replicaId_ << "] resumeVehicle position=" << position_in_order
              << " speed=" << cruise_speed_mps_ << " t=" << simTime() << "\n";
    // vc->setSpeedMode(31);
    mobility->getVehicleCommandInterface()->setSpeedMode(0);
    mobility->getVehicleCommandInterface()->setSpeed(30);  // Release control to SUMO
 
    
    // vc->setSpeed(cruise_speed_mps_);
}

// ── verifyCarPosition (port of V2VArrivalProtocol::verifyCarPosition) ─────────

ResDBIntersectionApp::VerificationResult
ResDBIntersectionApp::verifyCarPosition(const std::string& carId,
                                         const std::string& claimedLane,
                                         double claimedPosition, double tolerance)
{
    if (!mobility) return {false, "NO_TRACI"};
    TraCICommandInterface* traci = mobility->getCommandInterface();
    if (!traci) return {false, "NO_TRACI"};
    std::list<std::string> ids = traci->getVehicleIds();
    if (std::find(ids.begin(), ids.end(), carId) == ids.end())
        return {false, "NO_VEHICLE"};
    try {
        auto v = traci->vehicle(carId);
        std::string actualLane = v.getLaneId();
        double actualPos       = v.getLanePosition();
        if (actualLane != claimedLane)
            return {false, "WRONG_LANE", actualLane, actualPos};
        if (std::abs(actualPos - claimedPosition) > tolerance)
            return {false, "WRONG_POSITION", actualLane, actualPos};
        return {true, "OK", actualLane, actualPos};
    } catch (...) { return {false, "TRACI_ERROR"}; }
}

// ── extractReplicaId ──────────────────────────────────────────────────────────

int ResDBIntersectionApp::extractReplicaId(const std::string& carId) const
{
    try { return std::stoi(carId.substr(3)); } catch (...) { return -1; }
}

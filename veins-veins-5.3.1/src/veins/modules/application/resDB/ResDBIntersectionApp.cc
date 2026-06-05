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
#include <sstream>
#include <stdexcept>

#include "veins/modules/application/resDB/ResdbV2VWire.h"
#include "veins/modules/application/resDB/ResDBDecisionGossip.h"

using namespace veins;
using namespace veins::resdb_app_util;

Define_Module(veins::ResDBIntersectionApp);

// ── Destructor ────────────────────────────────────────────────────────────────

ResDBIntersectionApp::~ResDBIntersectionApp()
{
    if (smoke_test_msg_)          { cancelAndDelete(smoke_test_msg_);          smoke_test_msg_          = nullptr; }
    if (transport_poll_msg_)      { cancelAndDelete(transport_poll_msg_);      transport_poll_msg_      = nullptr; }
    if (time_tick_msg_)           { cancelAndDelete(time_tick_msg_);           time_tick_msg_           = nullptr; }
    if (propose_timeout_msg_)     { cancelAndDelete(propose_timeout_msg_);     propose_timeout_msg_     = nullptr; }
    if (cert_retry_timer_)        { cancelAndDelete(cert_retry_timer_);        cert_retry_timer_        = nullptr; }
    if (gossip_timer_)            { cancelAndDelete(gossip_timer_);            gossip_timer_            = nullptr; }
    if (initial_announce_msg_)    { cancelAndDelete(initial_announce_msg_);    initial_announce_msg_    = nullptr; }
    if (stop_sign_timeout_msg_)   { cancelAndDelete(stop_sign_timeout_msg_);   stop_sign_timeout_msg_   = nullptr; }
    if (consensus_timeout_msg_)   { cancelAndDelete(consensus_timeout_msg_);   consensus_timeout_msg_   = nullptr; }
    if (resume_msg_)              { cancelAndDelete(resume_msg_);              resume_msg_              = nullptr; }
    if (clearance_poll_msg_)      { cancelAndDelete(clearance_poll_msg_);      clearance_poll_msg_      = nullptr; }
    if (broadcastArrivalAnnouncement_timer_) { cancelAndDelete(broadcastArrivalAnnouncement_timer_); broadcastArrivalAnnouncement_timer_ = nullptr; }
    if (channel_metrics_timer_) {
        cancelAndDelete(channel_metrics_timer_);
        channel_metrics_timer_ = nullptr;
    }
    if (channel_metrics_) {
        cModule* nic = getParentModule() ? getParentModule()->getSubmodule("nic") : nullptr;
        cModule* mac = nic ? nic->getSubmodule("mac1609_4") : nullptr;
        if (mac && mac->isSubscribed(Mac1609_4::sigChannelBusy, channel_metrics_))
            mac->unsubscribe(Mac1609_4::sigChannelBusy, channel_metrics_);
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

        const double view_change_timeout_sec = par("pbftVcTimeoutSec").doubleValue();
        const double view_change_timeout_scale_sec = std::floor(static_cast<double>(total_vehicles_) / 5.0);
        pbft_vc_timeout_sec_ = (view_change_timeout_sec + view_change_timeout_scale_sec);

        debug_cert_protocol_    = par("debugCertProtocol").boolValue();
        debug_order_delivery_ = par("debugOrderDelivery").boolValue();
        enable_cert_retries_    = par("enableArrivalCertRetries").boolValue();
        cert_retry_interval_    = par("arrivalCertRetryIntervalSec").doubleValue();
        cert_retry_max_         = par("arrivalCertRetryMax").intValue();

        gossip_enabled_          = par("enableDecisionGossip").boolValue();
        gossip_initial_interval_ = par("decisionGossipInitialIntervalSec").doubleValue();
        gossip_max_retries_      = par("decisionGossipMaxRetries").intValue();

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

        const int ambulance_replica_id = par("ambulanceReplicaId").intValue();
        if (ambulance_replica_id >= 0) {
            is_ambulance_ = (replicaId_ == ambulance_replica_id);

            std::cout << "[AMBULANCE BINDING] r" << replicaId_
                      << " ambulanceReplicaId=" << ambulance_replica_id
                      << " -> isAmbulance=" << (is_ambulance_ ? 1 : 0) << "\n";
        }

        moduleIsAmbulance = is_ambulance_;


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
        // Staggered one-shot arrival announcement.
        initial_announce_msg_ = new cMessage("resdbInitialAnnounce");
        scheduleAt(simTime() + trigger_join_time_ + replicaId_ * arrival_slot_sec_,
                   initial_announce_msg_);

        // // Periodic re-announce fallback (in case peers miss the initial broadcast).
        // state_announce_msg_ = new cMessage("resdbStateAnnounce");
        // scheduleAt(simTime() + state_announce_interval_, state_announce_msg_);

        broadcastArrivalAnnouncement_timer_ = new cMessage("resdbBroadcastArrivalAnnouncement");
        scheduleAt(simTime() + broadcast_arrival_announcement_interval_, broadcastArrivalAnnouncement_timer_);

        if (par("enableChannelMetricsCsv").boolValue() && replicaId_ < total_vehicles_) {
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
            if (mac)
                mac->subscribe(Mac1609_4::sigChannelBusy, channel_metrics_);
            else
                std::cerr << "[ChannelMetrics] r" << replicaId_ << " no nic/mac1609_4 — utilization stays 0\n";
            channel_metrics_timer_ = new cMessage("channelMetricsTick");
            scheduleAt(simTime() + 0.1, channel_metrics_timer_);
        }
    }
}

// ── handleSelfMsg ─────────────────────────────────────────────────────────────

void ResDBIntersectionApp::handleSelfMsg(cMessage* msg)
{
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
                if (current_primary == replicaId_ && !order_applied_ &&
                    !propose_submitted_ &&
                    current_phase_ != ConsensusPhase::DEPARTED) {
                    propose_submitted_ = false;
                    std::cout << "[VC-DEBUG] r" << replicaId_
                              << " became primary, re-proposing in phase="
                              << phaseToStr(current_phase_) << " t=" << simTime() << "\n";
                    proposeAll();
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
        if (!cert_broadcast_ && !order_applied_ && !propose_submitted_) {
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

    if (msg == gossip_timer_) {
        if (gossip_order_bytes_.empty()) return;
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
        processOrders();
        if (!order_applied_) {
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
            ResdbOmnetForceViewChange(resdb_server_handle_);
            std::cout << "[APP-VC] r" << replicaId_
                      << " ResdbOmnetForceViewChange returned t=" << simTime() << "\n";
        }
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

    if (moduleIsAmbulance && !ambulanceColorSet && mobility && mobility->getVehicleCommandInterface()) {
        std::cout << "[AMBULANCE COLOR] r" << replicaId_ << " setting color to red\n";
        mobility->getVehicleCommandInterface()->setColor(TraCIColor(255, 0, 0, 255));
        ambulanceColorSet = true;
    }

    discoverLane();

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

        int primary = ResdbOmnetGetPrimary(resdb_server_handle_);
        if (replicaId_ == primary && !propose_submitted_) {
            if ((int)collected_certs_.size() >= total_vehicles_) {
                proposeAll();
            } else if (deferred_propose_after_cert_timeout_) {
                deferred_propose_after_cert_timeout_ = false;
                std::cout << "[ResDB r" << replicaId_
                          << "] stop zone: deferred cert-timeout — re-arming collection (timeout="
                          << cert_collection_timeout_ << "s)\n";
                tryStartCertCollectionTimer(true);
            } else {
                tryStartCertCollectionTimer(false);
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

void ResDBIntersectionApp::recordIntersectionDeparture(simtime_t departedAt)
{
    if (departureTime >= SIMTIME_ZERO) return;

    departureTime = departedAt;
    cleared_time_ = departedAt;
    is_departed_ = true;
    current_phase_ = ConsensusPhase::DEPARTED;

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
    
    std::cerr << "[FINISH-PROBE] r" << replicaId_
    << " handle=" << resdb_server_handle_ << std::endl;

    if (resdb_server_handle_) {
        ResdbOmnetUpdateSimTimeUs(resdb_server_handle_,
                                  std::numeric_limits<int64_t>::max());
    }
    std::cerr << "[FINISH-PROBE] r" << replicaId_ << " after UpdateSimTime" << std::endl;
    // Stop global monitor (stats thread); cv fix in stats.cpp ensures it wakes
    // immediately rather than sleeping for monitor_sleep_time_ wall-clock seconds.
    ResdbOmnetStopGlobalStats();
    std::cerr << "[FINISH-PROBE] r" << replicaId_ << " after StopGlobalStats" << std::endl;

    if (vc_trigger_msg_) {
        cancelEvent(vc_trigger_msg_);
        delete vc_trigger_msg_; vc_trigger_msg_ = nullptr;
    }
    if (resdb_server_handle_) {
        std::cerr << "[FINISH-PROBE] r" << replicaId_ << " calling StopServer" << std::endl;
        ResdbOmnetStopServer(resdb_server_handle_);
        std::cerr << "[FINISH-PROBE] r" << replicaId_ << " after StopServer" << std::endl;
        ResdbOmnetDestroyServer(resdb_server_handle_);
        resdb_server_handle_ = nullptr;
    }
    if (channel_metrics_timer_) {
        cancelAndDelete(channel_metrics_timer_);
        channel_metrics_timer_ = nullptr;
    }
    if (channel_metrics_) {
        cModule* nic = getParentModule() ? getParentModule()->getSubmodule("nic") : nullptr;
        cModule* mac = nic ? nic->getSubmodule("mac1609_4") : nullptr;
        if (mac && mac->isSubscribed(Mac1609_4::sigChannelBusy, channel_metrics_))
            mac->unsubscribe(Mac1609_4::sigChannelBusy, channel_metrics_);
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

    // ── Type 8: ResDB PBFT consensus bytes ────────────────────────────────────
    if (msgType == kResdbConsensusMsgType) {
        // Leader is active — cert retransmits are no longer useful.
        if (cert_retry_timer_ && cert_retry_timer_->isScheduled())
            cancelEvent(cert_retry_timer_);

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

void ResDBIntersectionApp::triggerGossip(uint32_t epoch,
                                          const std::vector<uint8_t>& order_bytes)
{
    if (order_bytes.empty()) return;
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

    if (order_applied_) {
        if (gossip_enabled_ && has_committed_order_ && epoch == last_committed_epoch_
                && order_bytes == committed_order_bytes_
                && gossip_order_bytes_.empty()) {
            std::cout << "[GOSSIP-RELAY] r" << replicaId_ << " from=" << bft->getFromReplicaId()
                      << " epoch=" << epoch << " t=" << simTime() << "\n";
            triggerGossip(epoch, committed_order_bytes_);
        } else {
            std::cout << "[TYPE9-RECV] r" << replicaId_ << " from=" << bft->getFromReplicaId()
                      << " order_applied=true, skipping\n";
        }
        return;
    }

    // Epoch guard: drop stale gossip from a previous round.
    if (has_committed_order_ && epoch <= last_committed_epoch_) return;

    int f         = (total_vehicles_ - 1) / 3;
    int threshold = f + 1;
    bool reached  = gossip_acc_.add(bft->getFromReplicaId(), epoch, order_bytes, threshold);

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
    consensus_relay_seen_.clear();
    return true;
}

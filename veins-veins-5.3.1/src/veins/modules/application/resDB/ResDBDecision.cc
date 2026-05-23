#include "veins/modules/application/resDB/ResDBIntersectionApp.h"
#include "veins/modules/application/resDB/ResDBUtil.h"
#include "veins/modules/application/resDB/ResdbV2VWire.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <set>

using namespace veins;
using namespace veins::resdb_app_util;

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
    if (cert_collection_start_time_ > SIMTIME_ZERO && propose_time_ >= cert_collection_start_time_) {
        std::cout << "[METRICS " << replicaId_ << "] Cert_Collection_Duration: "
                  << (propose_time_ - cert_collection_start_time_).dbl() << "s\n";
    }
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
            e.direction        = directionCode(vs.direction);
            e.position_in_lane = static_cast<uint8_t>(
                std::min(vs.positionInLane, 255));
        } else {
            // ResDB executor keys batching on lane/direction; leaving lane=0
            // makes every car look like North and forces singleton batches.
            const ArrivalCert& c = kv.second;
            e.lane             = laneCode(c.lane);
            e.direction        = directionCode(c.direction);
            e.position_in_lane = static_cast<uint8_t>(std::min(c.positionInLane, 255));
        }
        if (e.sim_time_us == 0)
            e.sim_time_us = (uint64_t)simTime().inUnit(SIMTIME_US);
        std::cout << "[PROPOSE-PACK] r" << replicaId_
                  << " entry rid=" << e.replica_id
                  << " lane=" << (int)e.lane
                  << " pos=" << (int)e.position_in_lane
                  << " ambu=" << (int)e.is_ambulance
                  << " cert.isAmbu=" << (kv.second.isAmbulance ? 1 : 0) << "\n";
        entries.push_back(e);
        present_ids.insert(e.replica_id);
    }
    // Add synthetic QUIET entries for any missing replica IDs.
    // Lane and position_in_lane are physically observable (sensors catch a lie)
    // so we use the primary's observed announcement state directly.
    // direction is intentional cyber-state and requires f+1 cert signatures —
    // it stays 0 (unknown) for QUIET entries.
    for (int rid = 0; rid < total_vehicles_; ++rid) {
        if (!present_ids.count(rid)) {
            ResdbVehicleEntry quiet{};
            quiet.replica_id   = rid;
            quiet.sim_time_us  = UINT64_MAX;  // QUIET sentinel
            quiet.is_ambulance = 0;
            quiet.direction    = 0;  // cert-only — unknown without f+1 echoes
            quiet.cyber_status = 0;  // QUIET — no f+1 echoes by timeout
            std::string quietCarId = "veh" + std::to_string(rid);
            if (local_vehicle_states_.count(quietCarId)) {
                const VehicleState& vs = local_vehicle_states_.at(quietCarId);
                quiet.lane             = laneCode(vs.lane);
                quiet.position_in_lane = static_cast<uint8_t>(
                    std::min(vs.positionInLane, 255));
            } else {
                quiet.lane             = 0;
                quiet.position_in_lane = 0;
            }
            entries.push_back(quiet);
            std::cout << "[ResDB r" << replicaId_
                      << "] proposeAll: QUIET entry for replica " << rid
                      << " lane=" << (int)quiet.lane
                      << " pos=" << (int)quiet.position_in_lane << "\n";
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
    size_t pending_after = 0;
    {
        std::lock_guard<std::mutex> lk(app->orders_mutex_);
        app->pending_orders_.push_back(std::move(copy));
        pending_after = app->pending_orders_.size();
    }
    std::cout << "[ORDER-ENQ] r" << app->replicaId_ << " len=" << len
              << " pending_after=" << pending_after
              << " order_applied=" << app->order_applied_
              << " phase=" << phaseToStr(static_cast<int>(app->current_phase_)) << std::endl;
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

    if (debug_order_delivery_) {
        std::cout << "[ORDER-DEQ] r" << replicaId_ << " n=" << local.size()
                  << " order_applied=" << order_applied_
                  << " phase=" << phaseToStr(current_phase_) << "\n";
    }

    size_t ord_idx = 0;
    for (const auto& dec : local) {
        ++ord_idx;
        if (order_applied_) {
            if (debug_order_delivery_) {
                std::cout << "[ORDER-TAIL-DROP] r" << replicaId_
                          << " skipping_remaining=" << (local.size() - ord_idx + 1)
                          << " (order_applied already)\n";
            }
            break;
        }
        if (dec.size() < sizeof(ResdbOrderHdr)) {
            if (debug_order_delivery_) {
                std::cout << "[ORDER-SKIP] r" << replicaId_
                          << " reason=short_hdr len=" << dec.size() << "\n";
            }
            continue;
        }

        ResdbOrderHdr ohdr;
        std::memcpy(&ohdr, dec.data(), sizeof(ohdr));
        // New format: n_vehicles × ResdbVehicleDecision (8 bytes each).
        if (dec.size() < sizeof(ResdbOrderHdr) +
                         ohdr.n_vehicles * sizeof(ResdbVehicleDecision)) {
            if (debug_order_delivery_) {
                std::cout << "[ORDER-SKIP] r" << replicaId_ << " reason=short_body len="
                          << dec.size() << " n_vehicles=" << ohdr.n_vehicles << "\n";
            }
            continue;
        }

        const ResdbVehicleDecision* decisions = reinterpret_cast<const ResdbVehicleDecision*>(
            dec.data() + sizeof(ResdbOrderHdr));

        std::cout << "[METRICS " << replicaId_ << "] Order_Decided_Time: " << simTime()
                  << " n_batches=" << ohdr.n_batches << "\n";
        
        has_committed_order_ = true;
        if (propose_time_ >= SIMTIME_ZERO) {
            double bft_sim  = (simTime() - propose_time_).dbl();
            double stop_dec = (stop_time_ >= SIMTIME_ZERO) ? (simTime() - stop_time_).dbl() : -1.0;
            std::cout << "[VC-TRACE] r" << replicaId_
                      << " propose_to_order_sec=" << bft_sim
                      << " stop_to_order_sec=" << stop_dec << "\n";
            std::cout << "[PHASE_SUMMARY " << replicaId_ << "] epoch=" << current_epoch_
                      << " PROPOSE_ALL_BFT(sim)=" << std::to_string(bft_sim) << "s"
                      << " stop_to_decision(sim)="
                      << (stop_dec >= 0.0 ? std::to_string(stop_dec) + "s" : "N/A") << "\n";
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
        if (my_batch < 0) {
            std::cout << "[ORDER-WARN] r" << replicaId_
                      << " committed order has no slot for this replica_id (n_vehicles="
                      << ohdr.n_vehicles << ")\n";
            if (debug_order_delivery_) {
                std::cout << "[ORDER-WARN] r" << replicaId_ << " decision_ids:";
                for (uint32_t i = 0; i < ohdr.n_vehicles; ++i)
                    std::cout << " " << decisions[i].replica_id;
                std::cout << "\n";
            }
            continue;
        }

        order_applied_ = true;
        if (stop_time_ < SIMTIME_ZERO)
            stop_time_ = simTime();  // car was en-route when order arrived; use order time as fallback
        stopCertBroadcastRetries();

        // Trigger post-consensus gossip so stragglers can catch up.
        if (gossip_enabled_) {
            last_committed_epoch_ = ohdr.epoch;
            has_committed_order_  = true;
            committed_order_bytes_ = dec;
            if (gossip_order_bytes_.empty())
                triggerGossip(ohdr.epoch, dec);
        }
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



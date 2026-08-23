// Copyright (C) 2026 Mathesh Kumar
// SPDX-License-Identifier: GPL-3.0-or-later

#include "v2vbft/app/ResDBIntersectionApp.h"
#include "v2vbft/app/ResDBUtil.h"
#include "v2vbft/app/ResdbV2VWire.h"


#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <set>

using namespace veins;
using namespace veins::resdb_app_util;

bool ResDBIntersectionApp::isReplicaConfiguredByzantine(int replicaId) const
{
    if (replicaId == ctx_.replicaId_)
        return is_byzantine_;

    cModule* system = getSimulation() ? getSimulation()->getSystemModule() : nullptr;
    cModule* node = system ? system->getSubmodule("node", replicaId) : nullptr;
    cModule* appl = node ? node->getSubmodule("appl") : nullptr;
    if (!appl)
        return false;
    try {
        return appl->par("isByzantine").boolValue();
    } catch (...) {
        return false;
    }
}

void ResDBIntersectionApp::proposeAll()
{
    // Static intersection units are quorum/vote/relay/execute members only — they
    // never originate a proposal (no arrival state, never cert-primary). Hard
    // chokepoint so no path (cert-store, timeout, view-change) can make a unit propose.
    if (is_intersection_unit_) {
        std::cout << "[UNIT] r" << ctx_.replicaId_
                  << " proposeAll suppressed (intersection unit never proposes)\n";
        return;
    }
    const bool rollbackOrderEpoch =
        ctx_.cancel_pending_ && ctx_.discovery_.state == DiscoveryState::COMPLETE &&
        ctx_.current_epoch_ == rollback_new_epoch_;
    if (cancel_consensus_pending_) {
        std::cout << "[CANCEL-PROPOSE] r" << ctx_.replicaId_
                  << " proposeAll redirected while cancel_consensus_pending\n";
        trySubmitCancelProposal("proposeAll-redirect");
        return;
    }
    if (ctx_.cancel_pending_ && !rollbackOrderEpoch) {
        std::cout << "[ROLLBACK-PROPOSE] r" << ctx_.replicaId_
                  << " proposeAll blocked outside recovery ORDER epoch\n";
        return;
    }
    if (rollback_cancel_initiated_ && !rollbackOrderEpoch) {
        std::cout << "[CANCEL-PROPOSE] r" << ctx_.replicaId_
                  << " normal propose suppressed while cancel in progress\n";
        return;
    }
    if (ctx_.propose_submitted_) {
        std::cout << "[VC-DEBUG] r" << ctx_.replicaId_
                  << " proposeAll skipped: already submitted at " << propose_time_ << "\n";
        return;
    }
    if (ctx_.discovery_.state != DiscoveryState::COMPLETE) {
        std::cout << "[DISCOVERY-PROPOSE-WAIT] r" << ctx_.replicaId_
                  << " state=" << discoveryStateName()
                  << " epoch=" << ctx_.discovery_.epoch << "\n";
        return;
    }
    if (!order_candidate_ || order_candidate_->epoch != ctx_.current_epoch_) {
        evaluateOrderReadiness("proposeAll-candidate-missing");
        return;
    }
    const OrderCandidate& candidate = *order_candidate_;
    const int orderPrimary = currentOrderPrimary();
    if (orderPrimary < 0) {
        std::cout << "[CERT-PRIMARY] r" << ctx_.replicaId_
                  << " proposeAll skipped: no ORDER primary yet\n";
        return;
    }
    if (orderPrimary != ctx_.replicaId_) {
        std::cout << "[CERT-PRIMARY] r" << ctx_.replicaId_
                  << " proposeAll skipped: order_primary=" << orderPrimary << "\n";
        return;
    }
    if (!order_vc_authoritative_ &&
            ResdbOmnetSetPrimaryFromCert(ctx_.resdb_server_handle_,
                                         candidate.initialPrimary) != 0) {
        std::cout << "[CERT-PRIMARY] r" << ctx_.replicaId_
                  << " proposeAll skipped: failed to install PBFT primary"
                  << " cert_primary=" << candidate.initialPrimary << "\n";
        return;
    }
    stopCertBroadcastRetries();
    stopStopZoneCertGossip();
    ctx_.propose_submitted_ = true;
    propose_time_ = simTime();
    ctx_.current_phase_ = ConsensusPhase::WAITING_FOR_CLEARANCE;
    std::cout << "[METRICS " << ctx_.replicaId_ << "] ProposeAll_Submit_Time: " << propose_time_ << "\n";
    if (ctx_.discovery_.collectionStartedAt > SIMTIME_ZERO &&
            propose_time_ >= ctx_.discovery_.collectionStartedAt) {
        std::cout << "[METRICS " << ctx_.replicaId_ << "] Cert_Collection_Duration: "
                  << (propose_time_ - ctx_.discovery_.collectionStartedAt).dbl() << "s\n";
    }
    std::cout << "[VC-TRACE] r" << ctx_.replicaId_
              << " proposeAll context phase=" << phaseToStr(ctx_.current_phase_)
              << " static_certs=" << candidate.voterIds.size() << "/"
              << (rollbackOrderEpoch ? minRollbackMembershipSize() : ctx_.total_vehicles_)
              << " all_certs=" << candidate.certs.size()
              << " observed=" << candidate.observedIntents.size()
              << " cert_primary=" << candidate.initialPrimary
              << " order_primary=" << orderPrimary
              << " authority=" << (order_vc_authoritative_ ? "pbft-vc" : "cert")
              << " pbft_primary=" << ResdbOmnetGetPrimary(ctx_.resdb_server_handle_);
    if (stop_time_ >= SIMTIME_ZERO)
        std::cout << " stop_to_propose_sec=" << (simTime() - stop_time_).dbl();
    std::cout << "\n";

    std::string myCarId = "veh" + std::to_string(ctx_.replicaId_);
    // Ensure own arrival_time_us is set if missing.
    // Pack ResdbProposeHdr + ResdbVehicleEntry per collected cert.
    // Vehicles that have a cert → SIGNED (cyber_status=1).
    // Missing vehicles → QUIET (cyber_status=0, sim_time_us=UINT64_MAX).
    std::set<int> present_ids;
    std::vector<ResdbVehicleEntry> entries;
    for (const auto& kv : candidate.certs) {
        const int rid = extractReplicaId(kv.first);
        const bool eligible = rollbackOrderEpoch
            ? shouldIncludeInRollbackMembership(rid)
            : (rid >= 0 && (rid < ctx_.total_vehicles_ || kv.second.isAmbulance));
        if (!eligible) {
            std::cout << "[PROPOSE-PACK] r" << ctx_.replicaId_
                      << " skip regular late/static-external cert rid=" << rid
                      << " car=" << kv.first
                      << " totalVehicles=" << ctx_.total_vehicles_
                      << " epoch=" << ctx_.current_epoch_ << "\n";
            continue;
        }
        ResdbVehicleEntry e{};
        e.replica_id   = rid;
        e.is_ambulance = kv.second.isAmbulance ? 1 : 0;
        e.cyber_status = 1;  // SIGNED — has a valid cert with f+1 echoes
        if (candidate.vehicleStates.count(kv.first)) {
            const VehicleState& vs = candidate.vehicleStates.at(kv.first);
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
        std::cout << "[PROPOSE-PACK] r" << ctx_.replicaId_
                  << " entry rid=" << e.replica_id
                  << " lane=" << (int)e.lane
                  << " pos=" << (int)e.position_in_lane
                  << " ambu=" << (int)e.is_ambulance
                  << " cert.isAmbu=" << (kv.second.isAmbulance ? 1 : 0) << "\n";
        entries.push_back(e);
        present_ids.insert(e.replica_id);
    }
    // Add QUIET entries only for observed intents that missed certification.
    // Lane and position_in_lane are physically observable (sensors catch a lie)
    // so we use the primary's observed announcement state directly.
    // direction is intentional cyber-state and requires f+1 cert signatures —
    // it stays 0 (unknown) for QUIET entries.
    uint64_t proposal_honest_opportunities = 0;
    auto appendQuiet = [&](int rid, const VehicleState* vs) {
        const bool target_is_byzantine = isReplicaConfiguredByzantine(rid);
        if (!target_is_byzantine)
            proposal_honest_opportunities++;
        if (!present_ids.count(rid)) {
            ResdbVehicleEntry quiet{};
            quiet.replica_id   = rid;
            quiet.sim_time_us  = UINT64_MAX;  // QUIET sentinel
            quiet.is_ambulance = 0;
            quiet.direction    = 0;  // cert-only — unknown without f+1 echoes
            quiet.cyber_status = 0;  // QUIET — no f+1 echoes by timeout
            if (vs) {
                quiet.lane             = laneCode(vs->lane);
                quiet.position_in_lane = static_cast<uint8_t>(
                    std::min(vs->positionInLane, 255));
            } else {
                quiet.lane             = 0;
                quiet.position_in_lane = 0;
            }
            entries.push_back(quiet);
            if (!target_is_byzantine) {
                quietHonestVehicles_++;
            }
            std::cout << "[ResDB r" << ctx_.replicaId_
                      << "] proposeAll: QUIET entry for replica " << rid
                      << " lane=" << (int)quiet.lane
                      << " pos=" << (int)quiet.position_in_lane
                      << " target_is_byzantine=" << (target_is_byzantine ? 1 : 0)
                      << "\n";
        }
    };
    if (rollbackOrderEpoch) {
        for (const auto& kv : candidate.vehicleStates) {
            const int rid = extractReplicaId(kv.first);
            if (shouldIncludeInRollbackMembership(rid))
                appendQuiet(rid, &kv.second);
        }
    } else {
        for (const auto& kv : candidate.vehicleStates) {
            const int rid = extractReplicaId(kv.first);
            if (rid >= 0 && (rid < ctx_.total_vehicles_ || kv.second.isAmbulance))
                appendQuiet(rid, &kv.second);
        }
    }
    // In anchored/forced-view mode (ctx_.tolerated_faults_ >= 0, i.e. the rollback path)
    // the bridge derives the ORDER active-voter view from the proposal entries. Add
    // the static units as QUIET entries so they land in that view and vote like cars.
    // They are never scheduled to cross: QUIET → singleton batch, non-existent SUMO id
    // clears immediately, and processOrders() routes units to execute-without-cross.
    // (Normal mode uses the full server.config quorum, so units already vote there.)
    if (ctx_.tolerated_faults_ >= 0) {
        for (int uid : staticUnitReplicaIds()) {
            if (present_ids.count(uid)) continue;
            ResdbVehicleEntry unit{};
            unit.replica_id       = uid;
            unit.sim_time_us      = UINT64_MAX;  // QUIET sentinel
            unit.is_ambulance     = 0;
            unit.direction        = 0;
            unit.cyber_status     = 0;           // QUIET — never a scheduled crosser
            unit.lane             = 0;
            unit.position_in_lane = 255;         // sort after all real vehicles
            entries.push_back(unit);
            present_ids.insert(uid);
            std::cout << "[UNIT] r" << ctx_.replicaId_
                      << " proposeAll: QUIET unit entry replica " << uid
                      << " (forced-view voter, not scheduled) epoch=" << ctx_.current_epoch_
                      << "\n";
        }
    }
    quietHonestOpportunities_ += proposal_honest_opportunities;
    const double quiet_honest_rate =
        quietHonestOpportunities_ > 0
            ? (100.0 * static_cast<double>(quietHonestVehicles_) /
               static_cast<double>(quietHonestOpportunities_))
            : 0.0;
    std::cout << "[METRICS " << ctx_.replicaId_ << "] Quiet_Honest_Vehicles: "
              << quietHonestVehicles_ << "\n";
    std::cout << "[METRICS " << ctx_.replicaId_ << "] Quiet_Honest_Opportunities: "
              << quietHonestOpportunities_ << "\n";
    std::cout << "[METRICS " << ctx_.replicaId_ << "] Quiet_Honest_Rate: "
              << quiet_honest_rate << "\n";

    uint32_t n = (uint32_t)entries.size();
    if (n == 0) {
        std::cout << "[ResDB r" << ctx_.replicaId_ << "] proposeAll: no entries, aborting\n";
        ctx_.propose_submitted_ = false;
        return;
    }

    // Crash-recovery ORDER carries a CLEAR evidence trailer (spec §12) so a
    // follower that missed CLEAR gossip can still validate/adopt it
    // atomically with the ORDER itself. Epoch 0 and Scenario-15 emergency
    // rollback never populate incidentRegistry_, so clearCerts stays empty.
    std::vector<std::vector<uint8_t>> clearCerts = candidate.clearCerts;
    if (rollbackOrderEpoch) {
        if (fabricated_clearance_attack_active_ && clearCerts.empty()) {
            // Preserve the proposal's wire shape so the experiment isolates
            // certificate validation rather than malformed-packet rejection.
            // Zero echoes makes this certificate cryptographically invalid.
            ClearCert forged;
            forged.cancelledEpoch = cancelled_epoch_;
            for (const auto& kv : incidentRegistry_) {
                if (kv.first.cancelledEpoch != cancelled_epoch_) continue;
                forged.executingBatch = kv.first.executingBatch;
                break;
            }
            clearCerts.push_back(serializeClearCert(forged));
            std::cout << "[BYZANTINE-FABRICATED-CLEARANCE] r" << ctx_.replicaId_
                      << " cancelled_epoch=" << forged.cancelledEpoch
                      << " new_epoch=" << ctx_.current_epoch_
                      << " batch=" << forged.executingBatch
                      << " forged_echoes=0"
                      << " action=attach-invalid-clear\n";
        }
    }
    size_t trailerSize = 0;
    if (!clearCerts.empty()) {
        trailerSize = sizeof(ResdbOrderEvidenceHdr);
        for (const auto& c : clearCerts) trailerSize += sizeof(uint32_t) + c.size();
    }

    size_t total = sizeof(ResdbProposeHdr) + n * sizeof(ResdbVehicleEntry) + trailerSize;
    std::vector<uint8_t> buf(total);
    uint8_t* p = buf.data();

    ResdbProposeHdr hdr;
    hdr.epoch               = ctx_.current_epoch_;
    hdr.leader_id           = ctx_.replicaId_;
    hdr.propose_sim_time_us = (uint64_t)simTime().inUnit(SIMTIME_US);
    hdr.n_vehicles          = n;
    std::memcpy(p, &hdr, sizeof(hdr)); p += sizeof(hdr);
    for (const auto& e : entries) {
        std::memcpy(p, &e, sizeof(e)); p += sizeof(e);
    }
    if (!clearCerts.empty()) {
        ResdbOrderEvidenceHdr ehdr{};
        ehdr.magic = RESDB_ORDER_EVIDENCE_MAGIC;
        ehdr.version = 1;
        ehdr.n_clear_certs = static_cast<uint16_t>(clearCerts.size());
        std::memcpy(p, &ehdr, sizeof(ehdr)); p += sizeof(ehdr);
        for (const auto& c : clearCerts) {
            uint32_t clen = static_cast<uint32_t>(c.size());
            std::memcpy(p, &clen, sizeof(clen)); p += sizeof(clen);
            std::memcpy(p, c.data(), c.size()); p += c.size();
        }
    }

    // Byzantine primary fault injection.
    if (is_byzantine_ && byzantine_type_ == BYZANTINE_SILENT_PRIMARY) { applyByzantineSilentPrimary(); return; }
    if (is_byzantine_ && byzantine_type_ == BYZANTINE_BAD_PROPOSAL)   applyByzantineBadProposal(hdr, buf);
    if (is_byzantine_ && byzantine_type_ == BYZANTINE_FAKE_AMBULANCE)  applyByzantineFakeAmbulance(buf.data() + sizeof(ResdbProposeHdr), n);
    if (is_byzantine_ && byzantine_type_ == BYZANTINE_TAMPER_LANE)     applyByzantineTamperLane(buf.data() + sizeof(ResdbProposeHdr), n);

    int rc = ResdbOmnetTriggerConsensus(ctx_.resdb_server_handle_, buf.data(), (uint32_t)buf.size());
    if (rollbackOrderEpoch) {
        std::cout << "[ROLLBACK-PROPOSE] r" << ctx_.replicaId_
                  << " rc=" << rc
                  << " cancelled_epoch=" << cancelled_epoch_
                  << " new_epoch=" << rollback_new_epoch_
                  << " normal_proposeAll=1 vehicles=" << n << "\n";
    }
    std::cout << "[ResDB r" << ctx_.replicaId_ << "] TriggerConsensus rc=" << rc
              << " vehicles=" << n << "\n";
}

// ── Crash detection ──────────────────────────────────────────────────────────
// Mirrors the kSafe table in resdb_intersection_scheduler.cc.  Runs on the
// COMMITTED order using cert lanes (not proposal lanes) so a Byzantine-spoofed
// proposal that sneaks through without the firewall is caught here.

bool ResDBIntersectionApp::detectUnsafeBatch(
    const ResdbVehicleDecision* decisions, uint32_t n, uint32_t n_batches)
{
    bool detected = false;
    static const uint8_t kSafe[12][4] = {
        {0, 0, 1, 0}, {2, 0, 3, 0}, {0, 2, 1, 2}, {0, 2, 2, 2},
        {0, 2, 3, 2}, {1, 2, 2, 2}, {1, 2, 3, 2}, {2, 2, 3, 2},
        {0, 2, 1, 0}, {1, 2, 0, 0}, {2, 2, 3, 0}, {3, 2, 2, 0},
    };
    auto isSafe = [&](uint8_t la, uint8_t da, uint8_t lb, uint8_t db) {
        if (la == lb) return false;
        for (const auto& p : kSafe)
            if ((la==p[0]&&da==p[1]&&lb==p[2]&&db==p[3]) ||
                (la==p[2]&&da==p[3]&&lb==p[0]&&db==p[1])) return true;
        return false;
    };

    // Group replica IDs by batch.
    std::vector<std::vector<int32_t>> batches(n_batches);
    for (uint32_t i = 0; i < n; ++i)
        if (decisions[i].batch_index < n_batches)
            batches[decisions[i].batch_index].push_back(decisions[i].replica_id);

    std::lock_guard<std::mutex> lk(certs_mutex_);
    for (uint32_t b = 0; b < n_batches; ++b) {
        const auto& members = batches[b];
        if (members.size() < 2) continue;
        for (size_t i = 0; i < members.size(); ++i) {
            auto itA = ctx_.collected_certs_.find("veh" + std::to_string(members[i]));
            if (itA == ctx_.collected_certs_.end()) continue;
            uint8_t la = laneCode(itA->second.lane);
            uint8_t da = directionCode(itA->second.direction);
            for (size_t j = i + 1; j < members.size(); ++j) {
                auto itB = ctx_.collected_certs_.find("veh" + std::to_string(members[j]));
                if (itB == ctx_.collected_certs_.end()) continue;
                uint8_t lb = laneCode(itB->second.lane);
                uint8_t db = directionCode(itB->second.direction);
                if (!isSafe(la, da, lb, db)) {
                    detected = true;
                    std::string crashRef = "unsafe_batch:" + std::to_string(ctx_.current_epoch_) +
                        ":" + std::to_string(b) +
                        ":veh" + std::to_string(std::min(members[i], members[j])) +
                        "+veh" + std::to_string(std::max(members[i], members[j]));
                    std::cout << "[CRASH_DETECTED] r" << ctx_.replicaId_
                              << " epoch=" << ctx_.current_epoch_
                              << " batch=" << b
                              << " veh" << members[i]
                              << "(cert lane=" << itA->second.lane << ")"
                              << " + veh" << members[j]
                              << "(cert lane=" << itB->second.lane << ")"
                              << " — unsafe pair committed by Byzantine leader\n";
                    maybeTriggerCrashRollback(crashRef);
                }
            }
        }
    }
    return detected;
}

bool ResDBIntersectionApp::detectFalsePriorityGrant(
    const ResdbVehicleDecision* decisions, uint32_t n)
{
    if (fake_ambulance_proposal_replica_id_ < 0) return false;
    if (ResdbOmnetGetPrimary(ctx_.resdb_server_handle_) != ctx_.replicaId_) {
        std::cout << "[CONSENSUS_ATTACK_OUTCOME] r" << ctx_.replicaId_
                  << " epoch=" << ctx_.current_epoch_
                  << " fault=FAKE_AMBULANCE"
                  << " outcome=PREVERIFY_BLOCKED_OR_VIEW_CHANGE_RECOVERED"
                  << " target=veh" << fake_ambulance_proposal_replica_id_
                  << "\n";
        fake_ambulance_proposal_replica_id_ = -1;
        return false;
    }

    uint32_t batch = UINT32_MAX;
    for (uint32_t i = 0; i < n; ++i) {
        if (decisions[i].replica_id == fake_ambulance_proposal_replica_id_) {
            batch = decisions[i].batch_index;
            break;
        }
    }
    if (batch == UINT32_MAX) return false;

    std::lock_guard<std::mutex> lk(certs_mutex_);
    auto it = ctx_.collected_certs_.find(
        "veh" + std::to_string(fake_ambulance_proposal_replica_id_));
    if (it == ctx_.collected_certs_.end() || it->second.isAmbulance) return false;

    std::cout << "[FALSE_PRIORITY_GRANTED] r" << ctx_.replicaId_
              << " epoch=" << ctx_.current_epoch_
              << " veh" << fake_ambulance_proposal_replica_id_
              << " committed_batch=" << batch
              << " proposal_ambulance=1 cert_ambulance=0"
              << " — Byzantine leader fake ambulance proposal committed\n";
    return true;
}

void ResDBIntersectionApp::detectConsensusAttackOutcome(
    const ResdbVehicleDecision* decisions, uint32_t n, uint32_t n_batches)
{
    const bool unsafe_batch = detectUnsafeBatch(decisions, n, n_batches);
    const bool false_priority = detectFalsePriorityGrant(decisions, n);

    auto logOutcome = [&](const char* fault, const char* outcome, const std::string& detail) {
        std::cout << "[CONSENSUS_ATTACK_OUTCOME] r" << ctx_.replicaId_
                  << " epoch=" << ctx_.current_epoch_
                  << " fault=" << fault
                  << " outcome=" << outcome;
        if (!detail.empty()) std::cout << " " << detail;
        std::cout << "\n";
    };

    if (!is_byzantine_) {
        if (unsafe_batch) {
            logOutcome("TAMPER_LANE", "UNSAFE_ORDER_COMMITTED",
                       "detector=cert_lane_batch_check");
        }
        std::lock_guard<std::mutex> lk(certs_mutex_);
        for (const auto& carId : uncertified_ambulance_claimers_) {
            auto it = ctx_.collected_certs_.find(carId);
            if (it == ctx_.collected_certs_.end()) continue;
            if (it->second.isAmbulance &&
                !cert_gate_rejected_ambulance_claimers_.count(carId)) {
                logOutcome("FAKE_AMBULANCE_FOLLOWER",
                           "UNCERTIFIED_PRIORITY_CLAIM_COMMITTED",
                           carId + " cert_ambulance=1");
            } else if (cert_gate_rejected_ambulance_claimers_.count(carId) &&
                       !it->second.isAmbulance) {
                logOutcome("FAKE_AMBULANCE_FOLLOWER",
                           "CERT_GATE_BLOCKED_OR_NOT_CERTIFIED",
                           carId + " cert_ambulance=0");
            }
        }
        return;
    }

    switch (byzantine_type_) {
    case BYZANTINE_FALSE_LANE: {
        std::lock_guard<std::mutex> lk(certs_mutex_);
        auto it = ctx_.collected_certs_.find("veh" + std::to_string(ctx_.replicaId_));
        if (it == ctx_.collected_certs_.end()) {
            const std::string carId = "veh" + std::to_string(ctx_.replicaId_);
            const size_t signerCount = my_received_echoes_.count(carId)
                ? my_received_echoes_.at(carId).size() : 0;
            const int threshold = (ctx_.tolerated_faults_ >= 0
                ? ctx_.tolerated_faults_ : (ctx_.total_vehicles_ - 1) / 3) + 1;
            std::cout << "[FALSE-LANE-COLLUSION-BLOCK] target=" << carId
                      << " signers=" << signerCount
                      << " threshold=" << threshold
                      << " reason=insufficient-signers\n";
            logOutcome("FALSE_LANE", "BLOCKED_NO_VALID_CERT",
                       "malicious_input=fake_lane");
        } else if (it->second.lane != "N" && it->second.lane != "S" &&
                   it->second.lane != "E" && it->second.lane != "W") {
            std::cout << "[FALSE-LANE-COLLUSION-COMMIT] target=veh" << ctx_.replicaId_
                      << " claimed=" << it->second.lane
                      << " actual=" << intended_lane_
                      << " epoch=" << ctx_.current_epoch_ << "\n";
            logOutcome("FALSE_LANE", "MALICIOUS_INPUT_COMMITTED",
                       "cert_lane=" + it->second.lane);
        } else {
            logOutcome("FALSE_LANE", "BLOCKED_OR_CANONICALIZED",
                       "cert_lane=" + it->second.lane);
        }
        break;
    }
    case BYZANTINE_INVALID_SIG:
        logOutcome("INVALID_SIG", "ORDER_COMMITTED_INVALID_ECHO_DROPPED",
                   "malicious_input=bad_echo_signature");
        break;
    case BYZANTINE_EQUIVOCATOR:
        logOutcome("EQUIVOCATOR", "ORDER_COMMITTED_WITH_CERT_QUORUM",
                   "malicious_input=divergent_direction");
        break;
    case BYZANTINE_SILENT_PRIMARY:
        logOutcome("SILENT_PRIMARY", "RECOVERED_AFTER_VIEW_CHANGE",
                   "malicious_input=no_proposal");
        break;
    case BYZANTINE_BAD_PROPOSAL:
        if (bad_proposal_injected_) {
            logOutcome("BAD_PROPOSAL", "MALFORMED_PROPOSAL_REJECTED_AND_RECOVERED",
                       "malicious_input=bad_n_vehicles");
        }
        break;
    case BYZANTINE_FAKE_AMBULANCE:
        if (fake_ambulance_proposal_replica_id_ >= 0) {
            logOutcome("FAKE_AMBULANCE",
                       false_priority ? "FALSE_PRIORITY_GRANTED" : "COMMITTED_NO_CERT_MISMATCH_FOUND",
                       "target=veh" + std::to_string(fake_ambulance_proposal_replica_id_));
        }
        break;
    case BYZANTINE_FAKE_AMBULANCE_FOLLOWER:
        logOutcome("FAKE_AMBULANCE_FOLLOWER", "FAULT_INJECTED_LOCAL_ONLY",
                   "detector=honest_receivers");
        break;
    case BYZANTINE_TAMPER_LANE:
        if (tamper_lane_proposal_replica_id_ >= 0 || unsafe_batch) {
            logOutcome("TAMPER_LANE",
                       unsafe_batch ? "UNSAFE_ORDER_COMMITTED" : "COMMITTED_NO_UNSAFE_PAIR_FOUND",
                       tamper_lane_proposal_replica_id_ >= 0
                           ? "target=veh" + std::to_string(tamper_lane_proposal_replica_id_)
                           : "");
        }
        break;
    case BYZANTINE_HONEST:
        break;
    }
}

// ── Byzantine primary fault injection helpers ─────────────────────────────────

void ResDBIntersectionApp::applyByzantineSilentPrimary()
{
    std::cout << "[BYZANTINE] r" << ctx_.replicaId_
              << " SILENT_PRIMARY: suppressing propose at " << simTime() << "\n";
}

void ResDBIntersectionApp::applyByzantineBadProposal(ResdbProposeHdr& hdr, std::vector<uint8_t>& buf)
{
    bad_proposal_injected_ = true;
    hdr.n_vehicles = hdr.n_vehicles + 1;  // fails PreVerify check 2
    std::memcpy(buf.data(), &hdr, sizeof(hdr));
    std::cout << "[BYZANTINE] r" << ctx_.replicaId_
              << " BAD_PROPOSAL: corrupted n_vehicles=" << hdr.n_vehicles
              << " at " << simTime() << "\n";
}

void ResDBIntersectionApp::applyByzantineFakeAmbulance(uint8_t* base, uint32_t n)
{
    // Flip is_ambulance 0→1 for the first non-ambulance entry in the proposal.
    // Caught by PreVerify Check 10 (is_ambulance mismatch vs cert). Without the
    // firewall this commits and the fake car gets ambulance crossing priority.
    for (uint32_t i = 0; i < n; ++i) {
        ResdbVehicleEntry e;
        std::memcpy(&e, base + i * sizeof(e), sizeof(e));
        if (e.is_ambulance == 0) {
            e.is_ambulance = 1;
            std::memcpy(base + i * sizeof(e), &e, sizeof(e));
            fake_ambulance_proposal_replica_id_ = e.replica_id;
            std::cout << "[BYZANTINE] r" << ctx_.replicaId_
                      << " FAKE_AMBULANCE: marked replica " << e.replica_id
                      << " as ambulance in proposal at " << simTime() << "\n";
            return;
        }
    }
}

void ResDBIntersectionApp::applyByzantineTamperLane(uint8_t* base, uint32_t n)
{
    // Crash attack: disguise the front E-lane car as S-lane (position=0 sorts before
    // all real S cars) so the scheduler sees N-STRAIGHT + "S"-STRAIGHT → kSafe {0,0,1,0}
    // → same batch.  N car (going south) and E car (going west) enter the center
    // simultaneously → CRASH.  No S car is quieted, so the real S queue proceeds
    // normally and no approach-lane rear-ends occur.
    // Caught by PreVerify Check 10 (cert lane=E, proposal lane=S).
    // Without firewall: CRASH. With firewall: Check 10 rejects → view change → no crash.

    // Find the front E-lane car (smallest position_in_lane) and disguise it as S with position=0.
    uint32_t best_i   = n;
    uint8_t  best_pos = 255;
    for (uint32_t i = 0; i < n; ++i) {
        ResdbVehicleEntry e;
        std::memcpy(&e, base + i * sizeof(e), sizeof(e));
        if (e.lane == 2 /* E */ && e.cyber_status == 1 &&
            e.position_in_lane < best_pos) {
            best_pos = e.position_in_lane;
            best_i   = i;
        }
    }
    if (best_i < n) {
        ResdbVehicleEntry e;
        std::memcpy(&e, base + best_i * sizeof(e), sizeof(e));
        uint8_t orig = e.lane;
        e.lane             = 1;
        e.position_in_lane = 0;
        std::memcpy(base + best_i * sizeof(e), &e, sizeof(e));
        tamper_lane_proposal_replica_id_ = e.replica_id;
        std::cout << "[BYZANTINE] r" << ctx_.replicaId_
                  << " TAMPER_LANE: replica " << e.replica_id
                  << " lane " << (int)orig << "→S(1) — N+E batch → CRASH\n";
    }
}

// ── certSnapshotCallback (ResDB worker thread) ───────────────────────────────
// Called by the bridge pre-verify to get the replica IDs this node has certs for.

/*static*/ void ResDBIntersectionApp::certSnapshotCallback(
    void* ctx, ResdbCertEntry* out, uint32_t* cnt)
{
    auto* app = static_cast<ResDBIntersectionApp*>(ctx);
    std::lock_guard<std::mutex> lk(app->certs_mutex_);
    uint32_t capacity = *cnt;
    uint32_t i = 0;
    for (const auto& kv : app->ctx_.collected_certs_) {
        if (i >= capacity) break;
        const std::string& carId = kv.first;
        if (carId.size() > 3) {
            try {
                const ArrivalCert& cert = kv.second;
                out[i].replica_id       = std::stoi(carId.substr(3));
                out[i].lane             = laneCode(cert.lane);
                out[i].position_in_lane = static_cast<uint8_t>(
                    std::min(cert.positionInLane, 255));
                out[i].direction        = directionCode(cert.direction);
                out[i].is_ambulance     = cert.isAmbulance ? 1 : 0;
                ++i;
            } catch (...) {}
        }
    }
    *cnt = i;
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
    std::cout << "[ORDER-ENQ] r" << app->ctx_.replicaId_ << " len=" << len
              << " pending_after=" << pending_after
              << " order_applied=" << app->ctx_.order_applied_
              << " phase=" << phaseToStr(static_cast<int>(app->ctx_.current_phase_)) << std::endl;
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
        std::cout << "[ORDER-DEQ] r" << ctx_.replicaId_ << " n=" << local.size()
                  << " order_applied=" << ctx_.order_applied_
                  << " phase=" << phaseToStr(ctx_.current_phase_) << "\n";
    }

    size_t ord_idx = 0;
    for (const auto& dec : local) {
        ++ord_idx;
        if (dec.size() >= sizeof(ResdbCancelDecisionHdr)) {
            ResdbCancelDecisionHdr dh{};
            std::memcpy(&dh, dec.data(), sizeof(dh));
            if (dh.magic == RESDB_CANCEL_DECISION_MAGIC) {
                handleCancelCommitDecision(dec);
                continue;
            }
        }
        if (dec.size() < sizeof(ResdbOrderHdr)) {
            if (debug_order_delivery_) {
                std::cout << "[ORDER-SKIP] r" << ctx_.replicaId_
                          << " reason=short_hdr len=" << dec.size() << "\n";
            }
            continue;
        }

        ResdbOrderHdr ohdr;
        std::memcpy(&ohdr, dec.data(), sizeof(ohdr));
        if (hasCompletedReplicaEpoch(ctx_.replicaId_, ohdr.epoch)) {
            std::cout << "[ORDER-TAIL-DROP] r" << ctx_.replicaId_
                      << " skipping epoch=" << ohdr.epoch
                      << " reason=replica-already-departed\n";
            continue;
        }
        if (tombstoned_epochs_.count(ohdr.epoch) || isEpochTombstoned(ohdr.epoch)) {
            std::cout << "[ORDER-SKIP] r" << ctx_.replicaId_
                      << " reason=tombstoned epoch=" << ohdr.epoch << "\n";
            continue;
        }
        if (ctx_.order_applied_ && ohdr.epoch <= ctx_.current_epoch_) {
            if (debug_order_delivery_) {
                std::cout << "[ORDER-TAIL-DROP] r" << ctx_.replicaId_
                          << " skipping epoch=" << ohdr.epoch
                          << " current_epoch=" << ctx_.current_epoch_
                          << " (order_applied already)\n";
            }
            continue;
        }
        // New format: n_vehicles × ResdbVehicleDecision (8 bytes each).
        if (dec.size() < sizeof(ResdbOrderHdr) +
                         ohdr.n_vehicles * sizeof(ResdbVehicleDecision)) {
            if (debug_order_delivery_) {
                std::cout << "[ORDER-SKIP] r" << ctx_.replicaId_ << " reason=short_body len="
                          << dec.size() << " n_vehicles=" << ohdr.n_vehicles << "\n";
            }
            continue;
        }
        clearConsensusRetries("order-committed");

        const ResdbVehicleDecision* decisions = reinterpret_cast<const ResdbVehicleDecision*>(
            dec.data() + sizeof(ResdbOrderHdr));

        committed_order_batches_.assign(ohdr.n_batches, {});
        {
            std::lock_guard<std::mutex> lk(committed_view_mutex_);
            ctx_.committed_order_vehicle_ids_.clear();
            for (uint32_t i = 0; i < ohdr.n_vehicles; ++i)
                ctx_.committed_order_vehicle_ids_.insert(decisions[i].replica_id);
        }
        for (uint32_t i = 0; i < ohdr.n_vehicles; ++i) {
            if (decisions[i].batch_index < ohdr.n_batches)
                committed_order_batches_[decisions[i].batch_index].push_back(decisions[i].replica_id);
        }

        // Adopt any CLEAR evidence trailer the executor forwarded from the
        // proposal (spec §12.1) — lets a replica that missed CLEAR gossip
        // still transition BLOCKING->CLEARED atomically with this ORDER,
        // rather than depending solely on separately-gossiped CLEAR_CERT.
        {
            const size_t decisions_end = sizeof(ResdbOrderHdr) +
                static_cast<size_t>(ohdr.n_vehicles) * sizeof(ResdbVehicleDecision);
            if (dec.size() >= decisions_end + sizeof(ResdbOrderEvidenceHdr)) {
                ResdbOrderEvidenceHdr ehdr;
                std::memcpy(&ehdr, dec.data() + decisions_end, sizeof(ehdr));
                size_t off = decisions_end + sizeof(ehdr);
                if (ehdr.magic == RESDB_ORDER_EVIDENCE_MAGIC) {
                    for (uint16_t i = 0; i < ehdr.n_clear_certs; ++i) {
                        if (off + sizeof(uint32_t) > dec.size()) break;
                        uint32_t certLen = 0;
                        std::memcpy(&certLen, dec.data() + off, sizeof(certLen));
                        off += sizeof(uint32_t);
                        if (off + certLen > dec.size()) break;
                        const uint8_t* certStart = dec.data() + off;
                        ClearCert cert = deserializeClearCert(certStart, certLen);
                        off += certLen;
                        if (cert.echoes.empty() || !validateClearCert(cert)) continue;
                        onIncidentCleared(BlockedIncident{cert.cancelledEpoch, cert.executingBatch},
                                          std::vector<uint8_t>(certStart, certStart + certLen));
                    }
                }
            }
        }

        if (ctx_.cancel_pending_ && ohdr.epoch == rollback_new_epoch_ &&
                hasBlockingIncidentForEpoch(cancelled_epoch_)) {
            const bool boxOccupied = anyVehicleInConflictBoxTraCI();
            std::cout << "[UNSAFE-RECOVERY-ORDER] r" << ctx_.replicaId_
                      << " cancelled_epoch=" << cancelled_epoch_
                      << " new_epoch=" << ohdr.epoch
                      << " incident_state=BLOCKING"
                      << " box_occupied=" << (boxOccupied ? 1 : 0)
                      << " action=accepted-without-valid-clear\n";
        }

        if (ctx_.cancel_pending_ && ohdr.epoch == rollback_new_epoch_) {
            ctx_.cancel_pending_ = false;
            cancel_state_ = CancelState::INACTIVE;
            cancel_active_batch_ = -1;
            cancel_primary_ = -1;
            cancel_leader_candidates_.clear();
            rollback_cancel_initiated_ = false;
            stopCancelCertRetries();
            stopClearCertRetries();
            cancelClearCertCandidate("order-applied");
            cancelClearCertRelay("order-applied");
            // Spec §8.4: a valid ORDER(e+1) supersedes WAIT the same way
            // CLEAR does — clear local WAIT state before continuing.
            stopWait("order-applied");
            resetOrderCandidate("order-applied");
            std::cout << "[ROLLBACK-COMMIT] r" << ctx_.replicaId_
                      << " cancelled_epoch=" << cancelled_epoch_
                      << " new_epoch=" << ohdr.epoch
                      << " t=" << simTime() << "\n";
        }

        if (ctx_.cancel_pending_ && ohdr.epoch == cancelled_epoch_) {
            std::cout << "[ORDER-SKIP] r" << ctx_.replicaId_
                      << " reason=cancel_pending cancelled_epoch="
                      << cancelled_epoch_ << "\n";
            continue;
        }

        detectConsensusAttackOutcome(decisions, ohdr.n_vehicles, ohdr.n_batches);

        std::cout << "[METRICS " << ctx_.replicaId_ << "] Order_Decided_Time: " << simTime()
                  << " n_batches=" << ohdr.n_batches << "\n";
        
        ctx_.has_committed_order_ = true;
        deactivateDiscovery("order-applied");
        ctx_.last_committed_epoch_ = ohdr.epoch;
        committed_order_bytes_ = dec;
        if (propose_time_ >= SIMTIME_ZERO) {
            double bft_sim  = (simTime() - propose_time_).dbl();
            double stop_dec = (stop_time_ >= SIMTIME_ZERO) ? (simTime() - stop_time_).dbl() : -1.0;
            std::cout << "[VC-TRACE] r" << ctx_.replicaId_
                      << " propose_to_order_sec=" << bft_sim
                      << " stop_to_order_sec=" << stop_dec << "\n";
            std::cout << "[PHASE_SUMMARY " << ctx_.replicaId_ << "] epoch=" << ctx_.current_epoch_
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

        // Static intersection units are never scheduled to cross. They execute the
        // committed order by recording it (already stored above) and gossiping it for
        // stragglers, then stop — no batch / clearance / resume. This is the intended
        // terminal path, distinct from the vehicle [ORDER-WARN] "no slot" case below.
        if (is_intersection_unit_) {
            ctx_.order_applied_ = true;
            if (ctx_.gossip_enabled_ && gossip_order_bytes_.empty())
                triggerGossip(ohdr.epoch, dec);
            std::cout << "[UNIT] r" << ctx_.replicaId_
                      << " executed committed order epoch=" << ohdr.epoch
                      << " n_batches=" << ohdr.n_batches
                      << " (records order, does not cross)\n";
            continue;
        }

        // Find own batch index.
        int my_batch = -1;
        for (uint32_t i = 0; i < ohdr.n_vehicles; ++i)
            if (decisions[i].replica_id == ctx_.replicaId_)
                { my_batch = (int)decisions[i].batch_index; break; }
        if (my_batch < 0) {
            std::cout << "[ORDER-WARN] r" << ctx_.replicaId_
                      << " committed order has no slot for this replica_id (n_vehicles="
                      << ohdr.n_vehicles << ")\n";
            if (debug_order_delivery_) {
                std::cout << "[ORDER-WARN] r" << ctx_.replicaId_ << " decision_ids:";
                for (uint32_t i = 0; i < ohdr.n_vehicles; ++i)
                    std::cout << " " << decisions[i].replica_id;
                std::cout << "\n";
            }
            if (ohdr.epoch == ctx_.current_epoch_) {
                ctx_.order_applied_ = true;
                std::cout << "[ORDER-WARN] r" << ctx_.replicaId_
                          << " no slot in current epoch; staying stopped/excluded\n";
                // A late ambulance excluded from the just-committed order must force a
                // rollback: broadcast its emergency arrival (peers witness it → each echoes
                // a CANCEL, forming the f+1 CANCEL cert) and (re)arm the announce timer to
                // keep re-broadcasting until it is admitted to a committed order. Without
                // this, deactivateDiscovery("order-applied") + the ctx_.order_applied_ guard
                // silence the ambulance and no rollback is ever triggered.
                if (is_ambulance_ && ctx_.enableRollback_) {
                    std::cout << "[AMBULANCE-EXCLUDED] r" << ctx_.replicaId_
                              << " ambulance not in committed epoch=" << ohdr.epoch
                              << " (n_vehicles=" << ohdr.n_vehicles
                              << "); forcing emergency arrival to trigger rollback\n";
                    broadcastArrivalAnnouncement(/*forceEmergency=*/true);
                    if (!broadcastArrivalAnnouncement_timer_)
                        broadcastArrivalAnnouncement_timer_ =
                            new cMessage("resdbBroadcastArrivalAnnouncement");
                    if (!broadcastArrivalAnnouncement_timer_->isScheduled())
                        scheduleAt(simTime() + broadcast_arrival_announcement_interval_,
                                   broadcastArrivalAnnouncement_timer_);
                }
            }
            continue;
        }

        ctx_.order_applied_ = true;
        if (stop_time_ < SIMTIME_ZERO)
            stop_time_ = simTime();  // car was en-route when order arrived; use order time as fallback
        stopCertBroadcastRetries();

        // Trigger post-consensus gossip so stragglers can catch up.
        if (ctx_.gossip_enabled_) {
            ctx_.has_committed_order_  = true;
            if (gossip_order_bytes_.empty())
                triggerGossip(ohdr.epoch, dec);
        }
        ctx_.current_phase_ = ConsensusPhase::EXECUTING;
        my_batch_index_ = my_batch;

        // Collect vehicles in the preceding batch (needed for clearance gating).
        preceding_batch_cars_.clear();
        if (my_batch > 0) {
            for (uint32_t i = 0; i < ohdr.n_vehicles; ++i)
                if ((int)decisions[i].batch_index == my_batch - 1)
                    preceding_batch_cars_.push_back(decisions[i].replica_id);
        }

        std::cout << "[METRICS " << ctx_.replicaId_ << "] Batch_Assignment: batch="
                  << my_batch << " preceding_count=" << preceding_batch_cars_.size() << "\n";

        if (my_batch == 0) {
            // Batch 0: go immediately — no predecessors.
            std::cout << "[METRICS " << ctx_.replicaId_ << "] Resume_Time: " << simTime()
                      << " (batch=0)\n";
            resumeVehicle(0);
        } else {
            // Wait for all vehicles in batch (my_batch - 1) to clear the
            // intersection via TraCI. Mirrors V2VOrderProtocol.cc executeBatch().
            clearance_started_ = simTime();
            std::cout << "[CLEARANCE r" << ctx_.replicaId_ << "] batch=" << my_batch
                      << " waiting for " << preceding_batch_cars_.size()
                      << " vehicle(s) in batch " << (my_batch - 1) << " to clear\n";
            if (!preceding_batch_poll_msg_)
                preceding_batch_poll_msg_ = new cMessage("resdbClearancePoll");
            if (preceding_batch_poll_msg_->isScheduled()) cancelEvent(preceding_batch_poll_msg_);
            scheduleAt(simTime() + preceding_batch_poll_period_sec_, preceding_batch_poll_msg_);
        }
    }
}

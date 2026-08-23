#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "v2vbft/app/ResDBDecisionGossip.h"
#include "v2vbft/app/ResDBWitnessCert.h"
#include "v2vbft/protocol/RollbackTypes.h"

// ── State owned solely by the cancel/rollback/clear protocol ─────────────────
//
// Measured: these members are referenced by ResDBRollbackProtocol.cc and by no
// other implementation file. They were flat in ResDBIntersectionApp among 237
// others, so nothing stopped an unrelated protocol reaching into them and
// nothing said which concern they belonged to.
//
// Grouped by the three certificate families the rollback protocol runs and the
// round state they share. See protocol/RollbackTypes.h for the wire types.

namespace veins {

// A cancel that reached consensus, retained so late gossip can be reconciled
// against what this replica already committed.
struct CommittedCancelInfo {
    uint64_t cancel_seq = 0;
    uint8_t payload_digest[32] = {};
    std::vector<uint8_t> attestation_bytes;
    bool gossip_adopted = false;
};

struct RollbackState {
    // ── CANCEL (types 12/13): abandon a committed epoch ──────────────────────
    // Dedup/threshold bookkeeping for f+1 echoes, plus propagation tracking so
    // a certificate is relayed once rather than once per hearing.
    WitnessEchoCollector cancel_echo_collector_;
    std::set<std::string> cancel_echo_sent_;
    std::set<std::string> cancel_cert_seen_;
    std::set<std::string> cancel_cert_relayed_;
    std::map<std::string, std::set<int>> cancel_cert_carriers_;  // key -> distinct relaying replicas
    resdb_gossip::GossipAccumulator cancel_gossip_acc_;
    bool cancel_leader_attack_logged_ = false;

    // ── CLEAR (types 15/16): a blocked batch is passable again ───────────────
    WitnessEchoCollector clear_echo_collector_;
    std::set<std::string> clear_echo_sent_;
    std::set<std::string> clear_cert_seen_;
    std::set<std::string> clear_cert_relayed_;
    std::string clear_cert_candidate_key_;
    std::set<std::string> clear_cert_candidate_keys_;
    int clear_cert_candidate_rank_ = -1;
    bool fabricated_clearance_attack_logged_ = false;

    // ── The rollback round itself ────────────────────────────────────────────
    std::map<uint32_t, CommittedCancelInfo> committed_cancels_;
    std::vector<uint8_t> rollback_justification_;
    CancelReason rollback_reason_ = CANCEL_CRASH;
    std::string rollback_reason_ref_;
    // Membership is dynamic during rollback: the set of replicas expected to
    // vote is fixed when the round opens so late arrivals cannot change quorum
    // underneath it.
    int rollback_expected_membership_size_ = 0;

    // ── WAIT (type 17): advisory heartbeat, leader side ──────────────────────
    // Not a certificate and not quorum-based — purely an advisory that the
    // incident is still being worked, so followers defer rather than time out.
    bool wait_leader_active_ = false;
    uint32_t wait_leader_heartbeat_index_ = 0;
};

} // namespace veins

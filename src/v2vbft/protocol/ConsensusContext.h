#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>

#include <openssl/evp.h>

#include "v2vbft/crypto/CryptoAuth.h"
#include "v2vbft/protocol/ArrivalTypes.h"
#include "v2vbft/protocol/Primitives.h"

// ── The state every protocol component shares ────────────────────────────────
//
// Measured, not guessed: of the 237 member variables on ResDBIntersectionApp,
// exactly these are read or written by four or more of its implementation
// files. Everything else belongs to one or two components and can move with
// them. Naming this set is what lets a component be handed the state it needs
// instead of a pointer to the whole module.
//
// Deliberately NOT here:
//   - the OMNeT timers (cMessage*). Only the module can schedule or cancel a
//     message, so parking the pointers in a passive struct would separate them
//     from the one object able to act on them. Timer ownership moves to the
//     components that use them, which is a different change.
//   - anything used by three files or fewer; those follow their component.
//
// This is a plain aggregate with no invariants of its own. It is a named
// bundle of shared state, not an abstraction pretending the state is private —
// the protocol genuinely does share these fields, and hiding that behind
// accessors would add ceremony without adding safety.

namespace v2vbft {

struct ConsensusContext {
    // ── Identity and configuration ───────────────────────────────────────────
    // Fixed during initialize() from NED parameters, then read-only.
    int replicaId_ = 0;
    int total_vehicles_ = 4;
    int tolerated_faults_ = -1;      // f; -1 until derived from total_vehicles_
    bool enableRollback_ = false;
    bool gossip_enabled_ = false;

    // ── Current round ────────────────────────────────────────────────────────
    uint32_t current_epoch_ = 0;
    ConsensusPhase current_phase_ = IDLE;
    uint32_t last_committed_epoch_ = 0;
    bool has_committed_order_ = false;
    bool order_applied_ = false;
    bool propose_submitted_ = false;
    bool cancel_pending_ = false;
    DiscoveryRound discovery_;
    std::map<std::string, ArrivalCert> collected_certs_;
    std::set<int> committed_order_vehicle_ids_;

    // ── Vehicle lifecycle ────────────────────────────────────────────────────
    // Set once the vehicle has crossed; it then runs on in "zombie mode",
    // still relaying for others but no longer competing for the junction.
    bool is_departed_ = false;

    // ── Handles ──────────────────────────────────────────────────────────────
    // Owned by the module, not by this struct: it neither allocates nor frees
    // them. resdb_server_handle_ is the opaque C-ABI handle to this vehicle's
    // ResilientDB replica.
    void* resdb_server_handle_ = nullptr;
    EVP_PKEY* ec_private_key_ = nullptr;
    uint8_t ec_pub_key_[CRYPTO_PUBKEY_BYTES] = {};
};

} // namespace v2vbft

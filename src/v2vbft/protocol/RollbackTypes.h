#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <omnetpp.h>

#include "v2vbft/crypto/CryptoAuth.h"

// ── Cancel / rollback / clear protocol types ─────────────────────────────────
//
// Undoing an already-committed order when reality contradicts it: a crash
// blocks the executing batch, or an ambulance arrives after the order was
// fixed. Three related certificate families live here:
//
//   CANCEL (12/13)  f+1 witnesses that the committed epoch must be abandoned
//   CLEAR  (15/16)  f+1 witnesses that a blocked batch is passable again
//   WAIT   (17)     a signed advisory heartbeat — NOT a certificate, no quorum
//
// Extracted verbatim from ResDBIntersectionApp's private nested types.

namespace veins {

enum CancelReason {
    CANCEL_CRASH = 0,
    CANCEL_EMERGENCY = 1,
};

enum class CancelState {
    INACTIVE,
    WITNESSING,
    DRAINING,
    CONSENSUS,
    COMMITTED,
};

// Type 12: one replica's signed witness that the committed epoch must be cancelled.
struct CancelEcho {
    int          echoingReplicaId = -1;
    uint32_t     cancelledEpoch = 0;
    CancelReason reason = CANCEL_CRASH;
    std::string  reasonRef;
    uint8_t      signerPubKey[CRYPTO_PUBKEY_BYTES] = {};
    uint8_t      signature[CRYPTO_SIG_MAX_BYTES] = {};
    uint8_t      signatureLen = 0;
};

// Type 13: f+1 cancel echoes assembled.
struct CancelCert {
    uint32_t                cancelledEpoch = 0;
    CancelReason            reason = CANCEL_CRASH;
    std::string             reasonRef;
    std::vector<CancelEcho> echoes;
};

// Scenario 16 CLEAR: structurally identical f+1 physical-evidence certificate
// to BLOCKED, but its subject is always a BlockedIncident (batch-scoped), so it
// carries cancelledEpoch/executingBatch directly instead of a generic
// reasonRef string.
struct ClearEcho {
    int      echoingReplicaId = -1;
    uint32_t cancelledEpoch = 0;
    uint32_t executingBatch = 0;
    uint8_t  signerPubKey[CRYPTO_PUBKEY_BYTES] = {};
    uint8_t  signature[CRYPTO_SIG_MAX_BYTES] = {};
    uint8_t  signatureLen = 0;
};

struct ClearCert {
    uint32_t                cancelledEpoch = 0;
    uint32_t                executingBatch = 0;
    std::vector<ClearEcho>  echoes;
};

// Scenario 16 WAIT (spec §8): one signed advisory heartbeat from the ordinary
// next-epoch certificate primary — not PBFT, not an f+1 certificate, no
// quorum. Wire layout matches the spec exactly.
#pragma pack(push, 1)
struct WaitHeartbeatPayload {
    uint32_t magic = 0;
    uint16_t version = 1;
    uint16_t _pad = 0;
    uint32_t cancelledEpoch = 0;
    uint32_t executingBatch = 0;
    int32_t  leaderId = -1;
    uint32_t heartbeatIndex = 0;
    uint64_t sentAtSimUs = 0;
    uint64_t validUntilSimUs = 0;
};
#pragma pack(pop)

static constexpr uint32_t kWaitHeartbeatMagic = 0x57414954u; // "WAIT"

// Follower's view of the most recently accepted heartbeat for the incident it
// is currently deferring on.
struct WaitHeartbeatState {
    uint32_t cancelledEpoch = 0;
    uint32_t executingBatch = 0;
    int      leaderId = -1;
    uint32_t lastHeartbeatIndex = 0;
    omnetpp::simtime_t validUntil = SIMTIME_ZERO;
    bool     active = false;
};

// Scenario 16: the authoritative subject of a BLOCKED/CLEAR incident is the
// obstruction of an executing committed batch, not any one wrecked vehicle.
struct BlockedIncident {
    uint32_t cancelledEpoch = 0;
    uint32_t executingBatch = 0;
    bool operator<(const BlockedIncident& o) const
    {
        return cancelledEpoch != o.cancelledEpoch
            ? cancelledEpoch < o.cancelledEpoch
            : executingBatch < o.executingBatch;
    }
};

enum class IncidentState { BLOCKING, CLEARED };

struct IncidentRecord {
    IncidentState state = IncidentState::BLOCKING;
    std::vector<uint8_t> blockedCertBytes;
    std::vector<uint8_t> clearCertBytes;
};

} // namespace veins

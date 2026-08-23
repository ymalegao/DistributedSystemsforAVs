#pragma once

// Copyright (C) 2026 Mathesh Kumar
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdint>
#include <string>
#include <vector>

#include <omnetpp.h>

#include "v2vbft/crypto/CryptoAuth.h"
#include "v2vbft/protocol/Primitives.h"

// ── Arrival-certificate protocol types ───────────────────────────────────────
//
// The ANN(1) → ECHO(4) → CERT(5) exchange that establishes, with f+1 signed
// witnesses, that a vehicle really is where it claims to be before its entry
// may enter a consensus proposal.
//
// Extracted verbatim from ResDBIntersectionApp's private nested types.

namespace v2vbft {

// One vehicle's claim about itself, as carried in a proposal.
struct VehicleState {
    std::string vehicleId;
    std::string lane;
    int         positionInLane  = 1;
    Direction   direction       = DIR_STRAIGHT;
    bool        isAmbulance     = false;
    uint64_t    arrival_time_us = 0;
};

// Type 1: a vehicle announces its arrival at the junction.
struct ArrivalAnnouncement {
    std::string          carId;
    std::string          laneId;
    std::string          lane;
    int                  positionInLane      = 1;
    Direction            direction           = DIR_STRAIGHT;
    bool                 isAmbulance         = false;
    double               claimedArrivalTime  = 0.0;
    int                  epoch               = 0;
    std::vector<uint8_t> ambulanceCertBytes;
    std::vector<uint8_t> ambulanceSigBytes;
    std::vector<uint8_t> signature;
};

// Type 4: one replica's signed witness that an announcement matches ground truth.
struct ArrivalEcho {
    int         echoingReplicaId = -1;
    std::string targetCarId;
    std::string lane;
    int         positionInLane = 1;
    Direction   direction      = DIR_STRAIGHT;
    bool        isAmbulance    = false;
    int         epoch          = 0;
    uint8_t     signerPubKey[CRYPTO_PUBKEY_BYTES] = {};
    uint8_t     signature[CRYPTO_SIG_MAX_BYTES]   = {};
    uint8_t     signatureLen = 0;
};

// Type 5: f+1 echoes assembled — the certificate a proposal may cite.
struct ArrivalCert {
    std::string              carId;
    std::string              lane;
    int                      positionInLane = 1;
    Direction                direction      = DIR_STRAIGHT;
    bool                     isAmbulance    = false;
    int                      epoch          = 0;
    std::vector<ArrivalEcho> echoes;
};

// ── Discovery round ──────────────────────────────────────────────────────────
// Tracks how long to keep listening for new arrivals before closing the round
// and proposing. Closing too early drops a vehicle; too late stalls everyone.

enum class DiscoveryState {
    INACTIVE,
    COLLECTING,
    DRAINING_CERTS,
    COMPLETE,
};

enum class LocalCertState {
    NOT_ASSEMBLED,
    QUEUED,
    AIRED,
};

enum class DiscoveryCloseReason {
    NONE,
    STABILIZED,   // no new intent heard for the stabilisation window
    DEADLINE,     // hard cap reached
};

struct DiscoveryRound {
    DiscoveryState state = DiscoveryState::INACTIVE;
    uint32_t epoch = 0;
    omnetpp::simtime_t lastNewIntentAt = -1;
    omnetpp::simtime_t collectionStartedAt = SIMTIME_ZERO;
    LocalCertState localCert = LocalCertState::NOT_ASSEMBLED;
    DiscoveryCloseReason closeReason = DiscoveryCloseReason::NONE;

    void reset(uint32_t newEpoch, omnetpp::simtime_t now)
    {
        state = DiscoveryState::COLLECTING;
        epoch = newEpoch;
        lastNewIntentAt = now;
        collectionStartedAt = SIMTIME_ZERO;
        localCert = LocalCertState::NOT_ASSEMBLED;
        closeReason = DiscoveryCloseReason::NONE;
    }

    bool localCertAssembled() const
    {
        return localCert != LocalCertState::NOT_ASSEMBLED;
    }

    bool localCertAired() const
    {
        return localCert == LocalCertState::AIRED;
    }
};

} // namespace v2vbft

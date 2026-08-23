#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "v2vbft/protocol/ArrivalTypes.h"
#include "v2vbft/protocol/RollbackTypes.h"

// ── The per-round proposal snapshot ──────────────────────────────────────────
//
// Sits at the top of the protocol type layering because it is the one type
// that spans both families: a proposal cites arrival certificates and, when it
// is a recovery round, the cancel justification that caused it.

namespace v2vbft {

// Immutable input snapshot for one ORDER round. Consensus still uses the
// existing wire formats; this only prevents late discovery/incident state from
// changing the proposal after readiness has been established.
struct OrderCandidate {
    uint32_t epoch = 0;
    bool recovery = false;
    uint32_t cancelledEpoch = 0;
    CancelReason rollbackReason = CANCEL_CRASH;
    int initialPrimary = -1;
    std::map<std::string, ArrivalCert> certs;
    std::map<std::string, VehicleState> vehicleStates;
    std::set<std::string> observedIntents;
    std::vector<uint8_t> cancelJustification;
    std::vector<std::vector<uint8_t>> clearCerts;
    std::vector<int> voterIds;
};

} // namespace v2vbft

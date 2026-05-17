#pragma once

#include <cstdint>
#include <map>
#include <vector>

// Post-consensus order gossip helpers (Type 9).
//
// After PBFT commits, every deciding replica broadcasts a lightweight TYPE9
// frame carrying its own EC-signed copy of the decided order.  A straggler
// that missed the PBFT storm collects these and applies the order as soon as
// f+1 distinct verified senders agree on the same (epoch, order_bytes).
//
// This file contains only pure logic: serialization, parsing, and vote
// accumulation.  EC signing/verification and OMNeT timer scheduling remain
// in ResDBIntersectionApp (which holds the private key and cMessage objects).

namespace resdb_gossip {

// --- Serialization -----------------------------------------------------------

// Build the TYPE9 inner payload: epoch(4B) || order_bytes.
// The caller signs this with packSignedPacket before sending.
std::vector<uint8_t> serialize(uint32_t epoch,
                               const std::vector<uint8_t>& order_bytes);

// Parse a TYPE9 inner payload (already outer-sig-verified by the caller).
// Returns true and fills the output parameters on success.
bool parse(const uint8_t* inner, uint32_t len,
           uint32_t& epoch_out, std::vector<uint8_t>& order_out);

// --- Vote accumulator --------------------------------------------------------

// Accumulates verified gossip votes from distinct senders for each epoch.
// Not thread-safe — callers must only invoke this on the simulation thread.
struct GossipAccumulator {
    // Record a verified vote.  Returns true if the count of senders that
    // agreed on the same order_bytes for this epoch has reached threshold.
    bool add(int sender_id, uint32_t epoch,
             const std::vector<uint8_t>& order_bytes, int threshold);

    // Count how many distinct senders agreed on ref_order for this epoch.
    int count(uint32_t epoch, const std::vector<uint8_t>& ref_order) const;

    void reset();

private:
    // epoch → { sender_id → order_bytes }
    std::map<uint32_t, std::map<int, std::vector<uint8_t>>> entries_;
};

} // namespace resdb_gossip

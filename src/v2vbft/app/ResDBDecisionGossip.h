#pragma once

// Copyright (C) 2026 Mathesh Kumar
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "veins/modules/application/ieee80211p/DemoBaseApplLayer.h"

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

// Stored custody copy for delayed announce gossip.
struct PendingRelay {
    std::string carId;
    uint32_t epoch;
    std::vector<uint8_t> serializedAnnounce;  // original signed bytes, unchanged
    double firstRelayedAt;
    int relayCount;
};

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

// --- Cert relay dedup --------------------------------------------------------

// Tracks which ARRIVAL_CERT car IDs this node has already relayed.
// Acceptance rule differs from GossipAccumulator: a cert is relayed as soon
// as it passes validateArrivalCert (f+1 signatures), not after f+1 matching
// senders.  Each carId is relayed at most once per epoch.
struct CertRelayTracker {
    // Returns true the first time carId is seen (caller should relay).
    // Returns false on subsequent calls (caller should skip).
    bool tryRelay(const std::string& carId);

    void reset();

private:
    std::set<std::string> relayed_;
};

// --- Arrival announce gossip -------------------------------------------------

// Build an announce-gossip inner payload: epoch(4B) || original ARRIVAL_ANNOUNCE bytes.
// The original announce bytes are not modified or re-signed by the carrier.
std::vector<uint8_t> serializeAnnouncement(uint32_t epoch,
                                           const std::vector<uint8_t>& announce_bytes);

// Parse an announce-gossip payload.
bool parseAnnouncement(const uint8_t* inner, uint32_t len,
                       uint32_t& epoch_out, std::vector<uint8_t>& announce_out);

// --- Consensus relay gossip --------------------------------------------------

// Build a consensus-relay inner payload: epoch(4B) || original raw ResDB bytes.
// The original ResDB bytes are not modified or re-signed by the carrier.
std::vector<uint8_t> serializeConsensusRelay(uint32_t epoch,
                                             const std::vector<uint8_t>& resdb_bytes);

// Parse a consensus-relay payload.
bool parseConsensusRelay(const uint8_t* inner, uint32_t len,
                         uint32_t& epoch_out, std::vector<uint8_t>& resdb_out);

// Tracks which (epoch, carId) announcements this node has already relayed.
struct AnnouncementRelayTracker {
    bool tryRelay(uint32_t epoch, const std::string& carId);
    void reset();

private:
    std::set<std::string> relayed_;
};

} // namespace resdb_gossip

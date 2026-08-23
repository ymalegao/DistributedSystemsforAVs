#include "v2vbft/app/ResDBDecisionGossip.h"

#include <cstring>

namespace resdb_gossip {

// --- Serialization -----------------------------------------------------------

std::vector<uint8_t> serialize(uint32_t epoch,
                               const std::vector<uint8_t>& order_bytes)
{
    std::vector<uint8_t> out(4 + order_bytes.size());
    std::memcpy(out.data(), &epoch, 4);
    std::memcpy(out.data() + 4, order_bytes.data(), order_bytes.size());
    return out;
}

bool parse(const uint8_t* inner, uint32_t len,
           uint32_t& epoch_out, std::vector<uint8_t>& order_out)
{
    if (inner == nullptr || len < 4) return false;
    std::memcpy(&epoch_out, inner, 4);
    order_out.assign(inner + 4, inner + len);
    return !order_out.empty();
}

// --- Vote accumulator --------------------------------------------------------

bool GossipAccumulator::add(int sender_id, uint32_t epoch,
                             const std::vector<uint8_t>& order_bytes, int threshold)
{
    entries_[epoch][sender_id] = order_bytes;
    return count(epoch, order_bytes) >= threshold;
}

int GossipAccumulator::count(uint32_t epoch,
                              const std::vector<uint8_t>& ref_order) const
{
    auto it = entries_.find(epoch);
    if (it == entries_.end()) return 0;
    int n = 0;
    for (auto& [sid, ob] : it->second)
        if (ob == ref_order) n++;
    return n;
}

void GossipAccumulator::reset()
{
    entries_.clear();
}

// --- Cert relay dedup --------------------------------------------------------

bool CertRelayTracker::tryRelay(const std::string& carId)
{
    return relayed_.insert(carId).second;  // true only on first insertion
}

void CertRelayTracker::reset()
{
    relayed_.clear();
}

// --- Arrival announce gossip -------------------------------------------------

std::vector<uint8_t> serializeAnnouncement(uint32_t epoch,
                                           const std::vector<uint8_t>& announce_bytes)
{
    return serialize(epoch, announce_bytes);
}

bool parseAnnouncement(const uint8_t* inner, uint32_t len,
                       uint32_t& epoch_out, std::vector<uint8_t>& announce_out)
{
    return parse(inner, len, epoch_out, announce_out);
}

// --- Consensus relay gossip --------------------------------------------------

std::vector<uint8_t> serializeConsensusRelay(uint32_t epoch,
                                             const std::vector<uint8_t>& resdb_bytes)
{
    return serialize(epoch, resdb_bytes);
}

bool parseConsensusRelay(const uint8_t* inner, uint32_t len,
                         uint32_t& epoch_out, std::vector<uint8_t>& resdb_out)
{
    return parse(inner, len, epoch_out, resdb_out);
}

bool AnnouncementRelayTracker::tryRelay(uint32_t epoch, const std::string& carId)
{
    return relayed_.insert(std::to_string(epoch) + ":" + carId).second;
}

void AnnouncementRelayTracker::reset()
{
    relayed_.clear();
}

} // namespace resdb_gossip

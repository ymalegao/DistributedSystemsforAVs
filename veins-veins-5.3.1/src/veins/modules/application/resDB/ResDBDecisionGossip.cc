#include "veins/modules/application/resDB/ResDBDecisionGossip.h"

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

} // namespace resdb_gossip

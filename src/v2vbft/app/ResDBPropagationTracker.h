#pragma once

#include <map>
#include <set>
#include <string>

namespace v2vbft {
namespace resdb_propagation {

// Generic bookkeeping for propagation of an already-validated semantic fact.
//
// Authentication and active-membership validation deliberately stay at the
// protocol boundary, where the caller has the signed packet and current view.
// The method name makes that contract explicit: callers must not record an
// unverified transport-level sender ID.
class AuthenticatedPropagationTracker {
public:
    bool observeAuthenticated(const std::string& semanticKey, int carrierId)
    {
        if (semanticKey.empty() || carrierId < 0) return false;
        return carriers_[semanticKey].insert(carrierId).second;
    }

    int count(const std::string& semanticKey) const
    {
        auto it = carriers_.find(semanticKey);
        return it == carriers_.end() ? 0 : static_cast<int>(it->second.size());
    }

    bool confirmed(const std::string& semanticKey, int threshold) const
    {
        return threshold > 0 && count(semanticKey) >= threshold;
    }

    void reset(const std::string& semanticKey)
    {
        carriers_.erase(semanticKey);
    }

    void reset()
    {
        carriers_.clear();
    }

private:
    std::map<std::string, std::set<int>> carriers_;
};

}  // namespace resdb_propagation
} // namespace v2vbft

#pragma once

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "platform/proto/resdb.pb.h"

namespace resdb {

struct OmnetForcedView {
  uint32_t epoch = 0;
  uint64_t seq = 0;
  std::string request_hash;
  int primary_omnet = -1;
  std::vector<int> active_omnet_ids;
  int f = 0;
  int quorum = 1;

  bool IsActiveOmnet(int replica_id) const {
    return std::binary_search(active_omnet_ids.begin(), active_omnet_ids.end(),
                              replica_id);
  }

  bool IsActiveResdbSender(int64_t sender_id) const {
    return IsActiveOmnet(ResdbSenderToOmnet(sender_id));
  }

  int PrimaryResdbId() const { return primary_omnet + 1; }

  static int ResdbSenderToOmnet(int64_t sender_id) {
    if (sender_id <= 0) return -1;
    return static_cast<int>(sender_id - 1);
  }
};

class OmnetForcedViewRegistry {
 public:
  void InstallPending(const OmnetForcedView& view) {
    std::lock_guard<std::mutex> lk(mu_);
    OmnetForcedView normalized = Normalize(view);
    pending_by_hash_[normalized.request_hash] = normalized;
    latest_ = normalized;
    LogInstall("pending", normalized);
  }

  bool InstallForRequest(const Request& request, const OmnetForcedView& view) {
    if (request.hash().empty()) return false;
    std::lock_guard<std::mutex> lk(mu_);
    OmnetForcedView normalized = Normalize(view);
    normalized.seq = request.seq();
    normalized.request_hash = request.hash();
    ViewKey key{normalized.epoch, normalized.seq, normalized.request_hash};
    auto it = views_.find(key);
    if (it != views_.end() &&
        it->second.active_omnet_ids != normalized.active_omnet_ids) {
      std::cout << "[ACTIVE-VIEW-REJECT] reason=conflicting-membership"
                << " epoch=" << normalized.epoch
                << " seq=" << normalized.seq
                << " hash=" << normalized.request_hash << "\n";
      return false;
    }
    views_[key] = normalized;
    pending_by_hash_[normalized.request_hash] = normalized;
    latest_ = normalized;
    LogInstall("request", normalized);
    return true;
  }

  std::optional<OmnetForcedView> FindForRequest(const Request& request) {
    if (request.hash().empty()) return std::nullopt;
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& kv : views_) {
      const OmnetForcedView& view = kv.second;
      if (view.seq == request.seq() && view.request_hash == request.hash()) {
        return view;
      }
    }
    auto pending = pending_by_hash_.find(request.hash());
    if (pending == pending_by_hash_.end()) return std::nullopt;
    OmnetForcedView view = pending->second;
    if (request.seq() != 0) {
      view.seq = request.seq();
      ViewKey key{view.epoch, view.seq, view.request_hash};
      views_[key] = view;
      latest_ = view;
      LogInstall("promoted", view);
    }
    return view;
  }

  std::optional<OmnetForcedView> Latest() const {
    std::lock_guard<std::mutex> lk(mu_);
    return latest_;
  }

  bool HasAny() const {
    std::lock_guard<std::mutex> lk(mu_);
    return latest_.has_value();
  }

 private:
  struct ViewKey {
    uint32_t epoch = 0;
    uint64_t seq = 0;
    std::string hash;

    bool operator<(const ViewKey& other) const {
      if (epoch != other.epoch) return epoch < other.epoch;
      if (seq != other.seq) return seq < other.seq;
      return hash < other.hash;
    }
  };

  static OmnetForcedView Normalize(OmnetForcedView view) {
    std::sort(view.active_omnet_ids.begin(), view.active_omnet_ids.end());
    view.active_omnet_ids.erase(
        std::unique(view.active_omnet_ids.begin(), view.active_omnet_ids.end()),
        view.active_omnet_ids.end());
    int n = static_cast<int>(view.active_omnet_ids.size());
    view.f = n > 0 ? (n - 1) / 3 : 0;
    view.quorum = std::max(2 * view.f + 1, 1);
    return view;
  }

  static void LogInstall(const char* mode, const OmnetForcedView& view) {
    std::cout << "[ACTIVE-VIEW] mode=" << (mode ? mode : "?")
              << " epoch=" << view.epoch
              << " seq=" << view.seq
              << " hash=" << view.request_hash
              << " N=" << view.active_omnet_ids.size()
              << " f=" << view.f
              << " quorum=" << view.quorum
              << " primary=r" << view.primary_omnet
              << " members=";
    for (size_t i = 0; i < view.active_omnet_ids.size(); ++i) {
      if (i) std::cout << ",";
      std::cout << view.active_omnet_ids[i];
    }
    std::cout << "\n";
  }

  mutable std::mutex mu_;
  std::map<ViewKey, OmnetForcedView> views_;
  std::map<std::string, OmnetForcedView> pending_by_hash_;
  std::optional<OmnetForcedView> latest_;
};

}  // namespace resdb

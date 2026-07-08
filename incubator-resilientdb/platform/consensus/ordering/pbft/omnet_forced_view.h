#pragma once

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "platform/proto/resdb.pb.h"

namespace resdb {

inline int BftQuorumSize(int n, int f) {
  if (n <= 0) return 0;
  if (f < 0 || f > (n - 1) / 3) {
    throw std::invalid_argument("invalid BFT f for active voter set");
  }
  return std::max((n + f + 2) / 2, 1);
}

struct OmnetForcedView {
  uint32_t epoch = 0;
  uint64_t seq = 0;
  std::string request_hash;
  int primary_omnet = -1;
  std::vector<int> active_omnet_ids;
  bool f_override = false;
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
    if (request.hash().empty()) {
      std::cout << "[ACTIVE-VIEW-INSTALL] mode=request result=reject"
                << " reason=empty-hash"
                << " seq=" << request.seq() << "\n";
      return false;
    }
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
    if (request.hash().empty()) {
      if (ShouldDebug(request)) {
        std::cout << "[ACTIVE-VIEW-FIND] result=miss"
                  << " reason=empty-hash"
                  << " seq=" << request.seq() << "\n";
      }
      return std::nullopt;
    }
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& kv : views_) {
      const OmnetForcedView& view = kv.second;
      if (view.seq == request.seq() && view.request_hash == request.hash()) {
        if (ShouldDebug(request)) {
          LogFind("exact", request, view, views_.size(),
                  pending_by_hash_.size(), latest_);
        }
        return view;
      }
    }
    auto pending = pending_by_hash_.find(request.hash());
    if (pending == pending_by_hash_.end()) {
      if (ShouldDebug(request)) {
        LogFindMiss("no-pending-for-hash", request, views_.size(),
                    pending_by_hash_.size(), latest_);
      }
      return std::nullopt;
    }
    OmnetForcedView view = pending->second;
    if (request.seq() != 0) {
      view.seq = request.seq();
      ViewKey key{view.epoch, view.seq, view.request_hash};
      views_[key] = view;
      latest_ = view;
      LogInstall("promoted", view);
    }
    if (ShouldDebug(request)) {
      LogFind("pending", request, view, views_.size(), pending_by_hash_.size(),
              latest_);
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
    if (!view.f_override) {
      view.f = n > 0 ? (n - 1) / 3 : 0;
    }
    view.quorum = BftQuorumSize(n, view.f);
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

  static bool ShouldDebug(const Request& request) {
    return request.seq() >= 2 || request.hash().rfind("omnet-tx-", 0) == 0;
  }

  static void LogFind(const char* result, const Request& request,
                      const OmnetForcedView& view, size_t view_count,
                      size_t pending_count,
                      const std::optional<OmnetForcedView>& latest) {
    std::cout << "[ACTIVE-VIEW-FIND] result=" << (result ? result : "?")
              << " req_seq=" << request.seq()
              << " req_hash=" << request.hash()
              << " view_epoch=" << view.epoch
              << " view_seq=" << view.seq
              << " view_hash=" << view.request_hash
              << " N=" << view.active_omnet_ids.size()
              << " quorum=" << view.quorum
              << " primary=r" << view.primary_omnet
              << " views=" << view_count
              << " pending=" << pending_count;
    if (latest) {
      std::cout << " latest_epoch=" << latest->epoch
                << " latest_seq=" << latest->seq
                << " latest_hash=" << latest->request_hash;
    }
    std::cout << "\n";
  }

  static void LogFindMiss(const char* reason, const Request& request,
                          size_t view_count, size_t pending_count,
                          const std::optional<OmnetForcedView>& latest) {
    std::cout << "[ACTIVE-VIEW-FIND] result=miss"
              << " reason=" << (reason ? reason : "?")
              << " req_seq=" << request.seq()
              << " req_hash=" << request.hash()
              << " views=" << view_count
              << " pending=" << pending_count;
    if (latest) {
      std::cout << " latest_epoch=" << latest->epoch
                << " latest_seq=" << latest->seq
                << " latest_hash=" << latest->request_hash;
    }
    std::cout << "\n";
  }

  mutable std::mutex mu_;
  std::map<ViewKey, OmnetForcedView> views_;
  std::map<std::string, OmnetForcedView> pending_by_hash_;
  std::optional<OmnetForcedView> latest_;
};

}  // namespace resdb

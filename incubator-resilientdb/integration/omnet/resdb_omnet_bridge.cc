#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "chain/storage/memory_db.h"
#include "executor/common/transaction_manager.h"
#include "integration/omnet/resdb_omnet_bridge.h"
#include "integration/omnet/resdb_intersection_scheduler.h"
#include "platform/config/resdb_config_utils.h"
#include "platform/consensus/ordering/pbft/checkpoint_manager.h"
#include "platform/consensus/ordering/pbft/commitment.h"
#include "platform/consensus/ordering/pbft/consensus_manager_pbft.h"
#include "platform/consensus/ordering/pbft/response_manager.h"
#include "platform/consensus/ordering/pbft/viewchange_manager.h"
#include "platform/networkstrate/replica_communicator.h"
#include "platform/networkstrate/service_network.h"
#include "platform/proto/resdb.pb.h"
#include "platform/statistic/stats.h"
#include "common/utils/sim_time_provider.h"

namespace {

int ResdbIdToOmnetReplica(int64_t resdb_node_id) {
  if (resdb_node_id <= 0) return -1;
  return static_cast<int>(resdb_node_id - 1);
}

struct ProposalView {
  const uint8_t* data = nullptr;
  size_t len = 0;
  bool is_rollback = false;
  ResdbRollbackHdr rollback{};
};

bool ParseNormalProposal(const uint8_t* data, size_t len, ResdbProposeHdr* hdr_out) {
  if (data == nullptr || len < sizeof(ResdbProposeHdr)) return false;
  ResdbProposeHdr hdr;
  std::memcpy(&hdr, data, sizeof(hdr));
  if (hdr.n_vehicles > 10000) return false;
  const size_t needed = sizeof(ResdbProposeHdr) +
      static_cast<size_t>(hdr.n_vehicles) * sizeof(ResdbVehicleEntry);
  if (len < needed) return false;
  if (hdr_out) *hdr_out = hdr;
  return true;
}

bool UnwrapRollbackIfPresent(const std::string& raw, ProposalView* view) {
  if (view == nullptr) return false;
  view->data = reinterpret_cast<const uint8_t*>(raw.data());
  view->len = raw.size();
  view->is_rollback = false;

  if (raw.size() < sizeof(ResdbRollbackHdr) + sizeof(ResdbProposeHdr))
    return ParseNormalProposal(view->data, view->len, nullptr);

  ResdbRollbackHdr rhdr;
  std::memcpy(&rhdr, raw.data(), sizeof(rhdr));
  if (rhdr.reason > 1) return ParseNormalProposal(view->data, view->len, nullptr);
  if (rhdr.new_epoch != rhdr.cancelled_epoch + 1)
    return ParseNormalProposal(view->data, view->len, nullptr);
  const size_t inner_off = sizeof(ResdbRollbackHdr) + rhdr.justification_len;
  if (inner_off > raw.size()) return ParseNormalProposal(view->data, view->len, nullptr);

  ResdbProposeHdr inner_hdr;
  const uint8_t* inner = reinterpret_cast<const uint8_t*>(raw.data()) + inner_off;
  const size_t inner_len = raw.size() - inner_off;
  if (!ParseNormalProposal(inner, inner_len, &inner_hdr))
    return ParseNormalProposal(view->data, view->len, nullptr);
  if (inner_hdr.epoch != rhdr.new_epoch)
    return ParseNormalProposal(view->data, view->len, nullptr);

  view->data = inner;
  view->len = inner_len;
  view->is_rollback = true;
  view->rollback = rhdr;
  return true;
}

bool StructurallyValidCancelJustification(const std::string& raw,
                                          const ResdbRollbackHdr& rhdr) {
  const size_t off = sizeof(ResdbRollbackHdr);
  if (raw.size() < off + rhdr.justification_len) return false;
  if (rhdr.reason > 1 || rhdr.justification_len == 0) return false;
  std::string just(raw.data() + off, rhdr.justification_len);
  if (rhdr.reason == 0) {
    if (just.find("CANCEL_CERT|") != 0) return false;
    std::unordered_set<std::string> signers;
    std::stringstream ss(just);
    std::string part;
    int idx = 0;
    while (std::getline(ss, part, '|')) {
      if (idx++ < 4) continue;
      size_t colon = part.find(':');
      if (colon != std::string::npos)
        signers.insert(part.substr(0, colon));
    }
    return !signers.empty();
  }
  // Emergency rollback carries the ambulance ARRIVAL_CERT bytes. Full CA /
  // P-256 verification remains on the Veins side in this pass.
  return !just.empty();
}

bool BuildForcedViewCandidate(const ProposalView& view,
                              const ResdbProposeHdr& hdr,
                              int expected_replicas,
                              const resdb::Request* request,
                              resdb::OmnetForcedView* out) {
  if (!view.is_rollback || out == nullptr) return false;
  if (hdr.n_vehicles == 0 || static_cast<int>(hdr.n_vehicles) > expected_replicas)
    return false;
  if (hdr.leader_id < 0 || hdr.leader_id >= expected_replicas) return false;

  const uint8_t* p = view.data + sizeof(ResdbProposeHdr);
  std::vector<int> members;
  members.reserve(hdr.n_vehicles);
  for (uint32_t i = 0; i < hdr.n_vehicles; ++i) {
    ResdbVehicleEntry e;
    std::memcpy(&e, p, sizeof(e));
    p += sizeof(e);
    if (e.replica_id < 0 || e.replica_id >= expected_replicas) return false;
    members.push_back(e.replica_id);
  }
  std::sort(members.begin(), members.end());
  if (std::adjacent_find(members.begin(), members.end()) != members.end())
    return false;
  if (!std::binary_search(members.begin(), members.end(), hdr.leader_id))
    return false;

  if (request != nullptr) {
    int sender_omnet = ResdbIdToOmnetReplica(request->sender_id());
    if (sender_omnet >= 0 && sender_omnet != hdr.leader_id) {
      std::cout << "[ACTIVE-VIEW-REJECT] reason=sender-not-leader"
                << " sender=" << sender_omnet
                << " leader=" << hdr.leader_id
                << " epoch=" << hdr.epoch
                << " seq=" << request->seq()
                << " hash=" << request->hash() << "\n";
      return false;
    }
  }

  resdb::OmnetForcedView candidate;
  candidate.epoch = hdr.epoch;
  candidate.seq = request ? request->seq() : 0;
  candidate.request_hash = request ? request->hash() : std::string();
  candidate.primary_omnet = hdr.leader_id;
  candidate.active_omnet_ids = std::move(members);
  *out = std::move(candidate);
  return true;
}

// ── IntersectionExecutor ──────────────────────────────────────────────────────
// Parses the ProposeAll wire format, runs deterministic intersection scheduling,
// and emits OrderDecision bytes.

class IntersectionExecutor : public resdb::TransactionManager {
 public:
  IntersectionExecutor() : resdb::TransactionManager() {}

  void SetOrderCallback(ResdbOrderDecidedFn cb, void* ctx) {
    std::lock_guard<std::mutex> lk(cb_mutex_);
    cb_ = cb;
    ctx_ = ctx;
  }

  std::unique_ptr<std::string> ExecuteData(const std::string& data) override {
    std::cout << "[EXECUTOR] ExecuteData called bytes=" << data.size() << "\n";
    ProposalView view;
    if (!UnwrapRollbackIfPresent(data, &view)) {
      std::cout << "[EXECUTOR] payload parse failed\n";
      return std::make_unique<std::string>();
    }
    if (view.is_rollback) {
      std::cout << "[ROLLBACK-COMMIT] executor unwrap"
                << " cancelled_epoch=" << view.rollback.cancelled_epoch
                << " new_epoch=" << view.rollback.new_epoch
                << " reason=" << static_cast<int>(view.rollback.reason)
                << " TODO=resdb_dynamic_N_reconfiguration_pending\n";
    }
    const uint8_t* p = view.data;
    size_t remaining = view.len;

    if (remaining < sizeof(ResdbProposeHdr)) {
      std::cout << "[EXECUTOR] payload too short\n";
      return std::make_unique<std::string>();
    }

    ResdbProposeHdr hdr;
    std::memcpy(&hdr, p, sizeof(hdr));
    p += sizeof(hdr);
    remaining -= sizeof(hdr);

    if (remaining < hdr.n_vehicles * sizeof(ResdbVehicleEntry))
      return std::make_unique<std::string>();

    std::vector<ResdbVehicleEntry> entries(hdr.n_vehicles);
    for (uint32_t i = 0; i < hdr.n_vehicles; ++i) {
      std::memcpy(&entries[i], p, sizeof(ResdbVehicleEntry));
      p += sizeof(ResdbVehicleEntry);
    }

    // Diagnostic: dump every entry so we can verify is_ambulance / position_in_lane.
    static const char* kLaneName[] = {"N","S","E","W"};
    static const char* kDirName[]  = {"Str","L","R"};
    std::cout << "[EXECUTOR] entries epoch=" << hdr.epoch << " n=" << hdr.n_vehicles << ":";
    for (const auto& e : entries) {
      std::cout << " r" << e.replica_id
                << "(lane=" << (e.lane < 4 ? kLaneName[e.lane] : "?")
                << " pos=" << (int)e.position_in_lane
                << " dir=" << (e.direction < 3 ? kDirName[e.direction] : "?")
                << " ambu=" << (int)e.is_ambulance
                << " cyber=" << (int)e.cyber_status << ")";
    }
    std::cout << "\n";

    auto schedule = resdb::omnet::BuildIntersectionSchedule(hdr, entries);
    const std::string& result = schedule.order_bytes;
    const uint32_t n = static_cast<uint32_t>(entries.size());
    const uint32_t n_batches = schedule.n_batches;
    const int ambu_lane = schedule.ambulance_lane;

    {
      std::lock_guard<std::mutex> lk(cb_mutex_);
      if (cb_) {
        std::cout << "[EXECUTOR] callback fired epoch=" << hdr.epoch
                  << " n=" << n << " n_batches=" << n_batches
                  << " ambu_lane=" << ambu_lane << "\n";
        cb_(ctx_,
            reinterpret_cast<const uint8_t*>(result.data()),
            static_cast<uint32_t>(result.size()));
      } else {
        std::cout << "[EXECUTOR] WARNING: no callback registered — order lost\n";
      }
    }

    std::unique_ptr<std::string> result_ptr = std::make_unique<std::string>(result);
    {
      const uint8_t* dbg = reinterpret_cast<const uint8_t*>(result_ptr->data());
      ResdbOrderHdr dbg_hdr;
      std::memcpy(&dbg_hdr, dbg, sizeof(dbg_hdr));
      std::cout << "[EXECUTOR] OrderDecision: epoch=" << dbg_hdr.epoch
                << " n_vehicles=" << dbg_hdr.n_vehicles
                << " n_batches=" << dbg_hdr.n_batches << " decisions=[";
      dbg += sizeof(dbg_hdr);
      for (uint32_t i = 0; i < dbg_hdr.n_vehicles; ++i) {
        ResdbVehicleDecision vd;
        std::memcpy(&vd, dbg + i * sizeof(vd), sizeof(vd));
        std::cout << " veh=" << vd.replica_id << " batch=" << vd.batch_index;
      }
      std::cout << " ]\n";
    }
    return result_ptr;
  }

 private:
  ResdbOrderDecidedFn cb_ = nullptr;
  void* ctx_ = nullptr;
  std::mutex cb_mutex_;
};

// ── OmnetReplicaCommunicator ──────────────────────────────────────────────────
// Serialises every outbound PBFT message into a ResDBMessage envelope and
// (a) queues it for radio delivery via the transport callbacks, and
// (b) self-injects the same bytes back into the local ServiceNetwork.
//
// Self-inject is the key fix for the PREPARE → COMMIT stall:
// In a real TCP deployment each replica unicasts to all peers including
// itself, so every replica's OWN PREPARE counts in its local bitset.
// Our radio model filters self-broadcasts at the MAC layer
// (bft->getFromReplicaId() == replicaId_ → return), so without self-inject
// each follower sees only 2 PREPAREs instead of the required 3 (2f+1, f=1).

class OmnetReplicaCommunicator : public resdb::ReplicaCommunicator {
 public:
  OmnetReplicaCommunicator(const std::vector<resdb::ReplicaInfo>& replicas,
                           ResdbOmnetTransportCallbacks* transport,
                           int self_replica,
                           resdb::ServiceNetwork* local_server)
      : resdb::ReplicaCommunicator(replicas,
                                   /*verifier=*/nullptr,
                                   /*is_use_long_conn=*/false,
                                   /*epoll_num=*/0,
                                   /*tcp_batch=*/1,
                                   /*start_background_threads=*/false),
        transport_(transport),
        self_replica_(self_replica),
        local_server_(local_server) {}

  // When true, this node drops all outbound PBFT messages (PRE_PREPARE, PREPARE,
  // COMMIT, VIEW_CHANGE, etc.) so it is Byzantine at the PBFT protocol level.
  // Used by BYZANTINE_SILENT_PRIMARY to ensure followers' complaint timers fire.
  void SetPbftSilent(bool silent) {
    is_pbft_silent_.store(silent);
    // One string per line: avoids merged lines when another thread logs mid-chain.
    std::ostringstream line;
    line << "[PBFT-SILENT] r" << self_replica_ << " silent=" << (silent ? 1 : 0)
         << '\n';
    std::cout << line.str() << std::flush;
  }

  // Broadcast: radio + self-inject so own vote counts in the local collector.
  int SendMessage(const google::protobuf::Message& message) override {
    if (is_pbft_silent_.load()) {
      std::ostringstream line;
      line << "[PBFT-SILENT] r" << self_replica_ << " drop broadcast type="
           << message.GetTypeName() << '\n';
      std::cout << line.str() << std::flush;
      return 0;  // Byzantine PBFT primary: drop all outbound
    }
    std::string payload;
    if (!message.SerializeToString(&payload)) return -1;
    resdb::ResDBMessage wire;
    wire.set_data(payload);  // copy — reused below for self-inject
    wire.mutable_signature()->set_signature("x");
    std::string bytes;
    if (!wire.SerializeToString(&bytes)) return -1;

    if (transport_ && transport_->broadcast) {
      transport_->broadcast(transport_->ctx,
                            reinterpret_cast<const uint8_t*>(bytes.data()),
                            static_cast<uint32_t>(bytes.size()));
      std::cout << "[OMNET-BROADCAST] r" << self_replica_
                << " len=" << bytes.size() << "\n";
    }

    // Self-inject: replicate TCP loopback so this replica's vote is counted.
    if (local_server_) {
      local_server_->InjectInboundPacket(bytes.data(), bytes.size());
    }
    return 0;
  }

  int SendMessage(const google::protobuf::Message& message,
                  const resdb::ReplicaInfo& replica_info) override {
    if (is_pbft_silent_.load()) {
      std::ostringstream line;
      line << "[PBFT-SILENT] r" << self_replica_ << " drop unicast type="
           << message.GetTypeName()
           << " to=" << ResdbIdToOmnetReplica(replica_info.id()) << '\n';
      std::cout << line.str() << std::flush;
      return 0;
    }
    if (!transport_ || !transport_->send_to) return -1;
    std::string payload;
    if (!message.SerializeToString(&payload)) return -1;
    resdb::ResDBMessage wire;
    wire.set_data(std::move(payload));
    wire.mutable_signature()->set_signature("x");
    std::string bytes;
    if (!wire.SerializeToString(&bytes)) return -1;
    int to = ResdbIdToOmnetReplica(replica_info.id());
    if (to < 0 || to == self_replica_) return 0;
    transport_->send_to(transport_->ctx, to,
                        reinterpret_cast<const uint8_t*>(bytes.data()),
                        static_cast<uint32_t>(bytes.size()));
    std::cout << "[OMNET-SEND] r" << self_replica_ << " → r" << to
              << " len=" << bytes.size() << "\n";
    return 0;
  }

  void BroadCast(const google::protobuf::Message& message) override {
    (void)SendMessage(message);
  }

  void SendMessage(const google::protobuf::Message& message,
                   int64_t node_id) override {
    if (is_pbft_silent_.load()) {
      std::ostringstream line;
      line << "[PBFT-SILENT] r" << self_replica_ << " drop direct-send type="
           << message.GetTypeName()
           << " to=" << ResdbIdToOmnetReplica(node_id) << '\n';
      std::cout << line.str() << std::flush;
      return;
    }
    int to = ResdbIdToOmnetReplica(node_id);
    if (to < 0 || !transport_ || !transport_->send_to) return;
    std::string payload;
    if (!message.SerializeToString(&payload)) return;
    resdb::ResDBMessage wire;
    wire.set_data(std::move(payload));
    wire.mutable_signature()->set_signature("x");
    std::string bytes;
    if (!wire.SerializeToString(&bytes)) return;
    transport_->send_to(transport_->ctx, to,
                        reinterpret_cast<const uint8_t*>(bytes.data()),
                        static_cast<uint32_t>(bytes.size()));
  }

 private:
  ResdbOmnetTransportCallbacks* transport_;
  int self_replica_ = -1;
  resdb::ServiceNetwork* local_server_ = nullptr;
  std::atomic<bool> is_pbft_silent_{false};
};

// ── OmnetConsensusManagerPBFT ─────────────────────────────────────────────────

class OmnetConsensusManagerPBFT : public resdb::ConsensusManagerPBFT {
 public:
  OmnetConsensusManagerPBFT(const resdb::ResDBConfig& config,
                             std::unique_ptr<resdb::TransactionManager> executor,
                             ResdbOmnetTransportCallbacks* transport)
      : resdb::ConsensusManagerPBFT(config, std::move(executor)),
        transport_(transport) {}

  void SetTransport(ResdbOmnetTransportCallbacks* transport,
                    resdb::ServiceNetwork* local_server = nullptr) {
    transport_ = transport;
    local_server_ = local_server;
    UpdateBroadCastClient();
    resdb::ReplicaCommunicator* comm = GetBroadCastClient();
    if (commitment_)          commitment_->SetReplicaCommunicator(comm);
    if (response_manager_)    response_manager_->SetReplicaCommunicator(comm);
    if (checkpoint_manager_)  checkpoint_manager_->SetReplicaCommunicator(comm);
    if (view_change_manager_) view_change_manager_->SetReplicaCommunicator(comm);
  }

  void SetVcTimeoutUs(int64_t us) {
    vc_timeout_us_ = us;
    if (view_change_manager_)
      view_change_manager_->SetTimeoutLength(static_cast<uint64_t>(us));
  }

  void SetPbftSilent(bool silent) {
    if (omnet_comm_) omnet_comm_->SetPbftSilent(silent);
  }

  // Directly initiate VC via TriggerViewChangeNow() — bypasses the complaint /
  // checkpoint chain entirely.  All downstream VC timers (TYPE_VIEWCHANGE,
  // TYPE_NEWVIEW) use SleepForUs, which is driven by SimTimeProvider →
  // OMNeT++ sim-time via time_tick_msg_.
  void TriggerViewChange() {
    if (!view_change_manager_) return;
    LOG(INFO) << "[VC-FORCE] TriggerViewChangeNow sim_us="
              << resdb::SimTimeProvider::NowUs();
    view_change_manager_->TriggerViewChangeNow();
  }

 protected:
  std::unique_ptr<resdb::ReplicaCommunicator> GetReplicaClient(
      const std::vector<resdb::ReplicaInfo>& replicas,
      bool /*is_use_long_conn*/ = false) override {
    auto comm = std::make_unique<OmnetReplicaCommunicator>(
        replicas, transport_,
        ResdbIdToOmnetReplica(config_.GetSelfInfo().id()),
        local_server_);
    omnet_comm_ = comm.get();
    return comm;
  }

 private:
  ResdbOmnetTransportCallbacks* transport_;
  resdb::ServiceNetwork* local_server_ = nullptr;
  int64_t vc_timeout_us_ = 3000000;  // 3 s default; overridden by SetVcTimeoutUs
  OmnetReplicaCommunicator* omnet_comm_ = nullptr;  // raw pointer; owned by base class
};

// ── Handle ────────────────────────────────────────────────────────────────────

struct CertCheckState {
  std::mutex          mu;
  ResdbCertSnapshotFn fn  = nullptr;
  void*               ctx = nullptr;
};

struct ResdbOmnetServerHandle {
  std::unique_ptr<resdb::ServiceNetwork> server;
  ResdbOmnetTransportCallbacks transport{nullptr, nullptr, nullptr};
  void* channel_ptr = nullptr;
  std::thread server_thread;
  bool server_thread_started = false;
  OmnetConsensusManagerPBFT* consensus = nullptr;
  IntersectionExecutor* executor = nullptr;
  int64_t vc_timeout_us = 3000000;  // 3 s default
  std::shared_ptr<CertCheckState> cert_state;
};

}  // namespace

// ── C API ─────────────────────────────────────────────────────────────────────

extern "C" int ResdbOmnetBridgeVersion() { return 1; }

extern "C" void* ResdbOmnetCreateKvServer(char* config_file,
                                          char* private_key_file,
                                          char* cert_file,
                                          char* /*logging_dir*/) {
  if (!config_file) return nullptr;

  auto executor_owned = std::make_unique<IntersectionExecutor>();
  auto* executor_ptr = executor_owned.get();

  std::unique_ptr<resdb::ResDBConfig> config = resdb::GenerateResDBConfig(
      std::string(config_file),
      private_key_file ? std::string(private_key_file) : std::string(),
      cert_file ? std::string(cert_file) : std::string());
  if (!config) return nullptr;

  config->SetHeartBeatEnabled(false);
  config->SetSignatureVerifierEnabled(false);

  auto service = std::make_unique<OmnetConsensusManagerPBFT>(
      *config, std::move(executor_owned), /*transport=*/nullptr);
  auto* service_ptr = service.get();
  auto forced_views = std::make_shared<resdb::OmnetForcedViewRegistry>();
  service_ptr->SetOmnetForcedViewRegistry(forced_views);

  // Basic pre-verify: proposal vehicle slots must fit within static server.config.
  // expected = total pre-keyed replicas (server.config size). Normal epoch-0 proposals
  // may carry fewer entries when late replicas are physically absent (rollback scenario
  // 15: 18 static keys, 16 at intersection, 2 arrive later). Rollback proposals use
  // forced-M with |M| <= expected.
  const int expected = static_cast<int>(config->GetReplicaInfos().size());
  auto cert_state = std::make_shared<CertCheckState>();

  // RESDB_NO_FIREWALL=1 disables all 10 PreVerify checks for "firewall-off" experiments.
  // Demonstrates that Byzantine proposals can be committed by PBFT but still cannot
  // cause physical crashes (executor size guard + intersection scheduler still hold).
  const char* no_fw_env = getenv("RESDB_NO_FIREWALL");
  const bool firewall_disabled = (no_fw_env && std::string(no_fw_env) == "1");
  if (firewall_disabled) {
    LOG(WARNING) << "[OMNET-PREVERIFY] RESDB_NO_FIREWALL=1 — all 10 PreVerify checks DISABLED";
    service_ptr->SetPreVerifyFunc([expected, service_ptr](const resdb::Request& req) -> bool {
      if (req.type() != resdb::Request::TYPE_PRE_PREPARE &&
          req.type() != resdb::Request::TYPE_NEW_TXNS) {
        return true;
      }
      resdb::BatchUserRequest batch;
      if (!batch.ParseFromString(req.data()) || batch.user_requests_size() <= 0)
        return true;
      const std::string& d = batch.user_requests(0).request().data();
      ProposalView view;
      if (!UnwrapRollbackIfPresent(d, &view) || !view.is_rollback) return true;
      ResdbProposeHdr hdr;
      std::memcpy(&hdr, view.data, sizeof(hdr));
      if (!StructurallyValidCancelJustification(d, view.rollback)) return true;
      resdb::OmnetForcedView forced_view;
      if (!BuildForcedViewCandidate(view, hdr, expected, &req, &forced_view))
        return true;
      if (req.type() == resdb::Request::TYPE_PRE_PREPARE) {
        service_ptr->InstallOmnetForcedViewForRequest(req, forced_view);
      } else {
        service_ptr->InstallOmnetPendingForcedView(forced_view);
      }
      return true;
    });
  } else {
  service_ptr->SetPreVerifyFunc([expected, cert_state, service_ptr](const resdb::Request& req) -> bool {
    if (req.type() != resdb::Request::TYPE_PRE_PREPARE &&
        req.type() != resdb::Request::TYPE_NEW_TXNS) {
      return true;
    }
    // In this integration path, PRE_PREPARE carries serialized BatchUserRequest,
    // and each UserRequest contains the raw Propose payload.
    resdb::BatchUserRequest batch;
    if (!batch.ParseFromString(req.data())) {
      LOG(ERROR) << "[OMNET-PREVERIFY] reject: failed to parse BatchUserRequest"
                 << " pre_prepare_data_size=" << req.data().size();
      return false;
    }
    if (batch.user_requests_size() <= 0) {
      LOG(ERROR) << "[OMNET-PREVERIFY] reject: empty BatchUserRequest";
      return false;
    }
    const auto& wrapped = batch.user_requests(0).request();
    const std::string& d = wrapped.data();
    ProposalView view;
    if (!UnwrapRollbackIfPresent(d, &view)) {
      LOG(ERROR) << "[OMNET-PREVERIFY] reject: payload too short or malformed"
                 << " size=" << d.size();
      return false;
    }
    ResdbProposeHdr hdr;
    std::memcpy(&hdr, view.data, sizeof(hdr));
    if (req.type() == resdb::Request::TYPE_NEW_TXNS && !view.is_rollback) {
      return true;
    }
    const bool installing_pre_prepare =
        view.is_rollback && req.type() == resdb::Request::TYPE_PRE_PREPARE;
    const bool installing_pending =
        view.is_rollback && req.type() == resdb::Request::TYPE_NEW_TXNS;
    if (view.is_rollback &&
        !StructurallyValidCancelJustification(d, view.rollback)) {
      LOG(ERROR) << "[OMNET-PREVERIFY] reject: rollback Check11 invalid cancel justification"
                 << " cancelled_epoch=" << view.rollback.cancelled_epoch
                 << " new_epoch=" << view.rollback.new_epoch
                 << " reason=" << static_cast<int>(view.rollback.reason);
      return false;
    }
    // Normal proposals: allow hdr.n_vehicles <= expected so epoch 0 can commit only
    // the cars physically at the intersection (e.g. 16 present, 18 static replicas).
    if (!view.is_rollback &&
        (hdr.n_vehicles == 0 || static_cast<int>(hdr.n_vehicles) > expected)) {
      LOG(ERROR) << "[OMNET-PREVERIFY] reject: vehicle count out of range"
                 << " hdr.n_vehicles=" << hdr.n_vehicles
                 << " expected_max=" << expected
                 << " epoch=" << hdr.epoch;
      return false;
    }
    if (view.is_rollback) {
      if (hdr.n_vehicles == 0 || static_cast<int>(hdr.n_vehicles) > expected) {
        LOG(ERROR) << "[OMNET-PREVERIFY] reject: rollback membership size out of range"
                   << " hdr.n_vehicles=" << hdr.n_vehicles
                   << " expected_max=" << expected
                   << " epoch=" << hdr.epoch;
        return false;
      }
      LOG(WARNING) << "[OMNET-PREVERIFY] rollback dynamic membership accepted at payload layer"
                   << " n_vehicles=" << hdr.n_vehicles
                   << " fixed_resdb_reconfiguration=TODO";
    }
    const size_t needed_size =
        sizeof(ResdbProposeHdr) + hdr.n_vehicles * sizeof(ResdbVehicleEntry);
    if (view.len < needed_size) {
      LOG(ERROR) << "[OMNET-PREVERIFY] reject: payload too short for entries"
                 << " size=" << view.len
                 << " need_at_least=" << needed_size
                 << " n_vehicles=" << hdr.n_vehicles;
      return false;
    }
    // Check no duplicate replica IDs.
    std::vector<int32_t> ids;
    const uint8_t* p = view.data + sizeof(ResdbProposeHdr);
    for (uint32_t i = 0; i < hdr.n_vehicles; ++i) {
      ResdbVehicleEntry e;
      std::memcpy(&e, p, sizeof(e));
      ids.push_back(e.replica_id);
      p += sizeof(e);
    }
    std::sort(ids.begin(), ids.end());
    auto dup_it = std::adjacent_find(ids.begin(), ids.end());
    if (dup_it != ids.end()) {
      LOG(ERROR) << "[OMNET-PREVERIFY] reject: duplicate replica id"
                 << " replica_id=" << *dup_it
                 << " n_vehicles=" << hdr.n_vehicles;
      return false;
    }
    // Check 4: all vehicle-slot / replica indices in valid range [0, expected).
    for (int32_t id : ids) {
      if (id < 0 || id >= static_cast<int32_t>(expected)) {
        LOG(ERROR) << "[OMNET-PREVERIFY] reject: replica_id out of range"
                   << " id=" << id << " expected=" << expected;
        return false;
      }
    }
    // Check 5+6: non-zero timestamps (UINT64_MAX allowed as QUIET sentinel),
    //           boolean is_ambulance, and valid cyber_status.
    {
      const uint8_t* ep = view.data + sizeof(ResdbProposeHdr);
      for (uint32_t i = 0; i < hdr.n_vehicles; ++i) {
        ResdbVehicleEntry e;
        std::memcpy(&e, ep, sizeof(e));
        if (e.sim_time_us == 0) {
          LOG(ERROR) << "[OMNET-PREVERIFY] reject: zero sim_time_us"
                     << " replica_id=" << e.replica_id;
          return false;
        }
        // UINT64_MAX is the QUIET sentinel — valid.
        if (e.is_ambulance > 1) {
          LOG(ERROR) << "[OMNET-PREVERIFY] reject: invalid is_ambulance"
                     << " value=" << static_cast<int>(e.is_ambulance)
                     << " replica_id=" << e.replica_id;
          return false;
        }
        if (e.cyber_status > 1) {
          LOG(ERROR) << "[OMNET-PREVERIFY] reject: invalid cyber_status"
                     << " value=" << static_cast<int>(e.cyber_status)
                     << " replica_id=" << e.replica_id;
          return false;
        }
        ep += sizeof(e);
      }
    }
    // Check 7: leader_id is a valid replica index.
    if (hdr.leader_id < 0 || hdr.leader_id >= static_cast<int32_t>(expected)) {
      LOG(ERROR) << "[OMNET-PREVERIFY] reject: leader_id out of range"
                 << " leader_id=" << hdr.leader_id << " expected=" << expected;
      return false;
    }
    if (!view.is_rollback) {
      int32_t proposal_cert_primary = std::numeric_limits<int32_t>::max();
      bool leader_is_signed = false;
      const uint8_t* ep = view.data + sizeof(ResdbProposeHdr);
      for (uint32_t i = 0; i < hdr.n_vehicles; ++i) {
        ResdbVehicleEntry e;
        std::memcpy(&e, ep, sizeof(e));
        if (e.replica_id >= 0 && e.replica_id < static_cast<int32_t>(expected) &&
            e.cyber_status == 1 && e.sim_time_us != UINT64_MAX) {
          proposal_cert_primary = std::min(proposal_cert_primary, e.replica_id);
          if (e.replica_id == hdr.leader_id) leader_is_signed = true;
        }
        ep += sizeof(e);
      }
      if (proposal_cert_primary == std::numeric_limits<int32_t>::max()) {
        LOG(ERROR) << "[OMNET-PREVERIFY] reject: normal proposal has no signed static cert"
                   << " leader_id=" << hdr.leader_id
                   << " epoch=" << hdr.epoch;
        return false;
      }
      if (hdr.leader_id != proposal_cert_primary || !leader_is_signed) {
        LOG(ERROR) << "[OMNET-PREVERIFY] reject: leader is not cert-primary"
                   << " leader_id=" << hdr.leader_id
                   << " proposal_cert_primary=" << proposal_cert_primary
                   << " leader_signed=" << (leader_is_signed ? 1 : 0)
                   << " epoch=" << hdr.epoch;
        return false;
      }
      if (req.type() == resdb::Request::TYPE_PRE_PREPARE) {
        const uint64_t incoming_view = req.current_view();
        service_ptr->SetPrimary(static_cast<uint32_t>(hdr.leader_id + 1),
                                incoming_view);
        LOG(INFO) << "[OMNET-PREVERIFY] installed cert-primary"
                  << " leader_id=" << hdr.leader_id
                  << " resdb_primary=" << (hdr.leader_id + 1)
                  << " view=" << incoming_view
                  << " epoch=" << hdr.epoch;
      }
    }
    // Check 8: deterministic sort always has a unique tiebreaker because
    // checks 3+4 guarantee unique valid replica IDs. Log that we verified.

    // Checks 9 & 10 — cert-omission guard + state-field verification.
    // Mirrors Java OrderRequestVerifier Checks 7 & 8 (re-execution not needed
    // in C++ since the schedule is computed post-consensus by all replicas).
    //
    // Check 9: if this follower holds certs for > f cars the leader marked
    //          QUIET, reject (Byzantine QUIET suppression).
    //          Up to f omissions tolerated as plausible channel loss.
    //
    // Check 10: for every SIGNED entry whose cert this follower holds, the
    //           proposal's lane/position_in_lane/direction/is_ambulance must
    //           match the cert-attested values.  Any mismatch is Byzantine
    //           state-field tampering; zero tolerance (cert is ground truth).
    {
      std::unique_lock<std::mutex> lk(cert_state->mu);
      ResdbCertSnapshotFn snap_fn  = cert_state->fn;
      void*               snap_ctx = cert_state->ctx;
      lk.unlock();

      if (snap_fn != nullptr) {
        // Build a map of proposal entries keyed by replica_id.
        std::unordered_map<int32_t, ResdbVehicleEntry> proposal_map;
        const uint8_t* ep = view.data + sizeof(ResdbProposeHdr);
        for (uint32_t i = 0; i < hdr.n_vehicles; ++i) {
          ResdbVehicleEntry e;
          std::memcpy(&e, ep, sizeof(e));
          proposal_map[e.replica_id] = e;
          ep += sizeof(e);
        }

        // Fetch local cert snapshot (replica ID + attested state).
        static constexpr uint32_t kMaxReplicas = 256;
        ResdbCertEntry cert_entries[kMaxReplicas];
        uint32_t cert_count = kMaxReplicas;
        snap_fn(snap_ctx, cert_entries, &cert_count);

        const int f = (expected - 1) / 3;
        int omitted = 0;
        std::unordered_set<int32_t> cert_backed_ids;
        for (uint32_t i = 0; i < cert_count; ++i) {
          cert_backed_ids.insert(cert_entries[i].replica_id);
        }

        if (!view.is_rollback) {
          for (uint32_t i = 0; i < cert_count; ++i) {
            const ResdbCertEntry& ce = cert_entries[i];
            if (ce.replica_id >= 0 &&
                ce.replica_id < static_cast<int32_t>(expected) &&
                ce.replica_id < hdr.leader_id) {
              LOG(ERROR) << "[OMNET-PREVERIFY] reject: local lower cert-primary"
                         << " local_cert=" << ce.replica_id
                         << " leader_id=" << hdr.leader_id
                         << " epoch=" << hdr.epoch;
              return false;
            }
          }
        }

        for (const auto& kv : proposal_map) {
          const ResdbVehicleEntry& pe = kv.second;
          if (pe.is_ambulance && (pe.cyber_status == 0 ||
                                  pe.sim_time_us == UINT64_MAX ||
                                  cert_backed_ids.find(pe.replica_id) == cert_backed_ids.end())) {
            LOG(ERROR) << "[OMNET-PREVERIFY] reject: uncertified ambulance priority"
                       << " replica_id=" << pe.replica_id
                       << " cyber_status=" << (int)pe.cyber_status
                       << " sim_time_us=" << pe.sim_time_us
                       << " epoch=" << hdr.epoch;
            return false;
          }
        }

        for (uint32_t i = 0; i < cert_count; ++i) {
          const ResdbCertEntry& ce = cert_entries[i];
          auto it = proposal_map.find(ce.replica_id);
          if (it == proposal_map.end()) continue;
          const ResdbVehicleEntry& pe = it->second;

          // Check 9: QUIET suppression.
          if (pe.cyber_status == 0 || pe.sim_time_us == UINT64_MAX) {
            ++omitted;
            continue;  // state fields are undefined for QUIET entries
          }

          // Check 10: state-field match for SIGNED entries.
          if (pe.lane             != ce.lane             ||
              pe.position_in_lane != ce.position_in_lane ||
              pe.direction        != ce.direction        ||
              pe.is_ambulance     != ce.is_ambulance) {
            LOG(ERROR) << "[OMNET-PREVERIFY] reject: state-field mismatch"
                       << " replica_id=" << ce.replica_id
                       << " cert(lane=" << (int)ce.lane
                       << " pos="       << (int)ce.position_in_lane
                       << " dir="       << (int)ce.direction
                       << " ambu="      << (int)ce.is_ambulance << ")"
                       << " proposal(lane=" << (int)pe.lane
                       << " pos="           << (int)pe.position_in_lane
                       << " dir="           << (int)pe.direction
                       << " ambu="          << (int)pe.is_ambulance << ")"
                       << " epoch=" << hdr.epoch;
            return false;
          }
        }

        if (omitted > f) {
          LOG(ERROR) << "[OMNET-PREVERIFY] reject: cert-omission "
                     << omitted << " local cert(s) QUIET in proposal"
                     << " (threshold f+1=" << (f + 1) << ")"
                     << " epoch=" << hdr.epoch;
          return false;
        }
        if (omitted > 0) {
          LOG(WARNING) << "[OMNET-PREVERIFY] cert-omission TOLERATED "
                       << omitted << "/" << f << " (<=f, channel loss)"
                       << " epoch=" << hdr.epoch;
        }
      }
    }

    if (view.is_rollback) {
      resdb::OmnetForcedView forced_view;
      if (!BuildForcedViewCandidate(view, hdr, expected, &req, &forced_view)) {
        LOG(ERROR) << "[OMNET-PREVERIFY] reject: rollback forced-M invalid"
                   << " n_vehicles=" << hdr.n_vehicles
                   << " leader_id=" << hdr.leader_id
                   << " epoch=" << hdr.epoch
                   << " request_type=" << req.type()
                   << " seq=" << req.seq()
                   << " hash=" << req.hash();
        return false;
      }
      if (installing_pre_prepare) {
        if (!service_ptr->InstallOmnetForcedViewForRequest(req, forced_view)) {
          LOG(ERROR) << "[OMNET-PREVERIFY] reject: forced-M install failed"
                     << " epoch=" << hdr.epoch
                     << " seq=" << req.seq()
                     << " hash=" << req.hash();
          return false;
        }
      } else if (installing_pending) {
        service_ptr->InstallOmnetPendingForcedView(forced_view);
      }
    }

    LOG(INFO) << "[OMNET-PREVERIFY] pass: all 10 checks ok"
              << " n_vehicles=" << hdr.n_vehicles << " epoch=" << hdr.epoch;
    return true;
  });
  }  // end else (firewall enabled)

  auto server = std::make_unique<resdb::ServiceNetwork>(
      *config, std::move(service), /*enable_network_acceptor=*/false);
  if (!server) return nullptr;

  // Force sim-time mode active immediately so GetCurrentTime() never falls back
  // to wall-clock inside ResDB threads. The OMNeT++ tick loop will update this
  // with the real sim-time each millisecond; 1 µs is a safe non-zero sentinel.
  resdb::SimTimeProvider::UpdateNowUs(1);

  auto* handle = new ResdbOmnetServerHandle();
  handle->server     = std::move(server);
  handle->consensus  = service_ptr;
  handle->executor   = executor_ptr;
  handle->cert_state = cert_state;
  return handle;
}

extern "C" int ResdbOmnetRunServer(void* server_handle) {
  if (!server_handle) return -1;
  auto* h = static_cast<ResdbOmnetServerHandle*>(server_handle);
  if (!h->server) return -1;
  if (h->server_thread_started) return 0;
  h->server_thread_started = true;
  h->server_thread = std::thread([h]() { h->server->Run(); });
  return 0;
}

extern "C" int ResdbOmnetStopServer(void* server_handle) {
  if (!server_handle) return -1;
  auto* h = static_cast<ResdbOmnetServerHandle*>(server_handle);
  std::cerr << "[STOP-SERVER] UpdateNowUs(MAX)\n" << std::flush;
  // OMNeT++ stops calling ResdbOmnetUpdateSimTimeUs during finish(), but ResDB
  // worker threads may still be inside LockFreeQueue::Pop()'s
  // SimTimeProvider::SleepUntilUs() loops. Wake them by advancing sim time to
  // end-of-teardown (still sim time, not wall clock).
  resdb::SimTimeProvider::UpdateNowUs(std::numeric_limits<uint64_t>::max());
  std::cerr << "[STOP-SERVER] calling server->Stop()\n" << std::flush;
  if (h->server) h->server->Stop();
  std::cerr << "[STOP-SERVER] joining server_thread\n" << std::flush;
  if (h->server_thread_started && h->server_thread.joinable())
    h->server_thread.join();
  h->server_thread_started = false;
  std::cerr << "[STOP-SERVER] done\n" << std::flush;
  return 0;
}

extern "C" void ResdbOmnetStopGlobalStats(void) {
  resdb::Stats::GetGlobalStats()->Stop();
}

extern "C" void ResdbOmnetDestroyServer(void* server_handle) {
  if (!server_handle) return;
  auto* h = static_cast<ResdbOmnetServerHandle*>(server_handle);
  if (h->server_thread_started) ResdbOmnetStopServer(server_handle);
  delete h;
}

extern "C" void* ResdbOmnetCreateNullHandle() {
  return new ResdbOmnetServerHandle();
}

extern "C" int ResdbOmnetSetTransport(void* server_handle,
                                      ResdbOmnetTransportCallbacks* cbs) {
  if (!server_handle || !cbs) return -1;
  auto* h = static_cast<ResdbOmnetServerHandle*>(server_handle);
  h->transport = *cbs;
  // Pass local_server so OmnetReplicaCommunicator can self-inject broadcasts,
  // replicating TCP loopback and ensuring own votes are counted in collectors.
  if (h->consensus)
    h->consensus->SetTransport(&h->transport, h->server.get());
  return 0;
}

extern "C" int ResdbOmnetSetChannel(void* server_handle, void* channel_ptr) {
  if (!server_handle) return -1;
  static_cast<ResdbOmnetServerHandle*>(server_handle)->channel_ptr = channel_ptr;
  return 0;
}

extern "C" int ResdbOmnetDeliverPacket(void* server_handle, int from_replica,
                                       const uint8_t* data, uint32_t len) {
  if (!server_handle || !data || len == 0) return -1;
  auto* h = static_cast<ResdbOmnetServerHandle*>(server_handle);
  if (!h->server) return -1;
  std::cout << "[BRIDGE-DELIVER] from=" << from_replica << " len=" << len << "\n";
  int rc = h->server->InjectInboundPacket(reinterpret_cast<const char*>(data),
                                          static_cast<size_t>(len));
  if (rc != 0)
    std::cout << "[BRIDGE-DELIVER] InjectInboundPacket returned " << rc << "\n";
  return rc;
}

extern "C" int ResdbOmnetUpdateSimTimeUs(void* server_handle, int64_t now_us) {
  if (!server_handle || now_us < 0) return -1;
  resdb::SimTimeProvider::UpdateNowUs(static_cast<uint64_t>(now_us));
  return 0;
}

extern "C" int ResdbOmnetTestBroadcast(void* server_handle,
                                       const uint8_t* data, uint32_t len) {
  if (!server_handle) return -1;
  auto* h = static_cast<ResdbOmnetServerHandle*>(server_handle);
  if (!h->transport.broadcast) return -1;
  h->transport.broadcast(h->transport.ctx, data, len);
  return 0;
}

// ── Step 5 ────────────────────────────────────────────────────────────────────

extern "C" int ResdbOmnetTriggerConsensus(void* server_handle,
                                          const uint8_t* payload, uint32_t len) {
  if (!server_handle || !payload || len == 0) return -1;
  auto* h = static_cast<ResdbOmnetServerHandle*>(server_handle);
  if (!h->server) return -1;

  // Inject TYPE_NEW_TXNS directly into the primary's commitment pipeline,
  // bypassing ResponseManager::DoBatch which would try to send to self
  // (self_replica_ == to) and be silently dropped.
  resdb::BatchUserRequest batch;
  auto* ur = batch.add_user_requests();
  ur->mutable_request()->set_type(resdb::Request::TYPE_CLIENT_REQUEST);
  ur->mutable_request()->set_data(
      std::string(reinterpret_cast<const char*>(payload), len));
  ur->set_id(0);

  resdb::Request req;
  req.set_type(resdb::Request::TYPE_NEW_TXNS);
  req.set_proxy_id(
      static_cast<int64_t>(h->consensus ? h->consensus->GetPrimary() : 1));
  std::string batch_bytes;
  if (!batch.SerializeToString(&batch_bytes)) return -1;
  req.set_data(batch_bytes);

  static std::atomic<uint64_t> tx_counter{0};
  req.set_hash("omnet-tx-" + std::to_string(tx_counter++));

  {
    std::string raw(reinterpret_cast<const char*>(payload), len);
    ProposalView view;
    if (UnwrapRollbackIfPresent(raw, &view) && view.is_rollback) {
      ResdbProposeHdr hdr;
      std::memcpy(&hdr, view.data, sizeof(hdr));
      const int expected = h->consensus
                               ? static_cast<int>(h->consensus->GetReplicas().size())
                               : 0;
      if (!StructurallyValidCancelJustification(raw, view.rollback)) {
        std::cout << "[BRIDGE-TRIGGER] reject rollback: invalid justification"
                  << " hash=" << req.hash() << "\n";
        return -1;
      }
      resdb::OmnetForcedView forced_view;
      if (!BuildForcedViewCandidate(view, hdr, expected, nullptr,
                                    &forced_view)) {
        std::cout << "[BRIDGE-TRIGGER] reject rollback: invalid forced M"
                  << " hash=" << req.hash()
                  << " epoch=" << hdr.epoch
                  << " n=" << hdr.n_vehicles
                  << " leader=" << hdr.leader_id << "\n";
        return -1;
      }
      forced_view.request_hash = req.hash();
      if (h->consensus) {
        h->consensus->InstallOmnetPendingForcedView(forced_view);
      }
    }
  }

  resdb::ResDBMessage wire;
  std::string req_bytes;
  if (!req.SerializeToString(&req_bytes)) return -1;
  wire.set_data(req_bytes);
  wire.mutable_signature()->set_signature("x");
  std::string wire_bytes;
  if (!wire.SerializeToString(&wire_bytes)) return -1;

  std::cout << "[BRIDGE-TRIGGER] TriggerConsensus injecting TYPE_NEW_TXNS"
            << " wire_bytes=" << wire_bytes.size()
            << " payload_len=" << len << "\n";
  int rc2 = h->server->InjectInboundPacket(wire_bytes.data(), wire_bytes.size());
  if (rc2 != 0)
    std::cout << "[BRIDGE-TRIGGER] InjectInboundPacket returned " << rc2 << "\n";
  return rc2;
}

extern "C" int ResdbOmnetSetOrderCallback(void* server_handle,
                                          ResdbOrderDecidedFn cb, void* ctx) {
  if (!server_handle) return -1;
  auto* h = static_cast<ResdbOmnetServerHandle*>(server_handle);
  if (h->executor) h->executor->SetOrderCallback(cb, ctx);
  return 0;
}

extern "C" int ResdbOmnetGetPrimary(void* server_handle) {
  if (!server_handle) return 0;
  auto* h = static_cast<ResdbOmnetServerHandle*>(server_handle);
  if (!h->consensus) return 0;
  if (auto forced = h->consensus->GetLatestOmnetForcedView()) {
    if (forced->primary_omnet >= 0) return forced->primary_omnet;
  }
  int id = ResdbIdToOmnetReplica(
      static_cast<int64_t>(h->consensus->GetPrimary()));
  return id < 0 ? 0 : id;
}

extern "C" int ResdbOmnetSetPrimaryFromCert(void* server_handle,
                                            int primary_omnet) {
  if (!server_handle || primary_omnet < 0) return -1;
  auto* h = static_cast<ResdbOmnetServerHandle*>(server_handle);
  if (!h->consensus) return -1;
  const auto replicas = h->consensus->GetReplicas();
  if (primary_omnet >= static_cast<int>(replicas.size())) return -1;
  const int current = ResdbIdToOmnetReplica(
      static_cast<int64_t>(h->consensus->GetPrimary()));
  if (current == primary_omnet) return 0;
  const uint64_t next_view = h->consensus->GetVersion() + 1;
  h->consensus->SetPrimary(static_cast<uint32_t>(primary_omnet + 1),
                           next_view);
  std::cout << "[CERT-PRIMARY] installed PBFT primary"
            << " omnet=" << primary_omnet
            << " resdb=" << (primary_omnet + 1)
            << " view=" << next_view << "\n";
  return 0;
}

extern "C" int ResdbOmnetGetPacketRequestType(const uint8_t* data, uint32_t len) {
  if (!data || len == 0) return -1;
  resdb::ResDBMessage wire;
  if (!wire.ParseFromArray(data, static_cast<int>(len))) return -1;
  resdb::Request req;
  if (!req.ParseFromString(wire.data())) return -1;
  return req.type();
}

extern "C" int ResdbOmnetGetPacketRequestInfo(const uint8_t* data, uint32_t len,
                                              ResdbPacketRequestInfo* info) {
  if (!info) return -1;
  std::memset(info, 0, sizeof(*info));
  info->type = -1;

  if (!data || len == 0) return -1;
  resdb::ResDBMessage wire;
  if (!wire.ParseFromArray(data, static_cast<int>(len))) return -1;
  resdb::Request req;
  if (!req.ParseFromString(wire.data())) return -1;

  info->parse_ok = 1;
  info->type = req.type();
  info->current_view = req.current_view();
  info->seq = req.seq();
  info->sender_id = req.sender_id();
  info->primary_id = req.primary_id();
  info->proxy_id = req.proxy_id();
  info->current_executed_seq = req.current_executed_seq();
  info->data_len = static_cast<uint32_t>(req.data().size());
  info->hash_len = static_cast<uint32_t>(req.hash().size());
  return 0;
}

extern "C" const char* ResdbOmnetRequestTypeName(int request_type) {
  switch (request_type) {
    case resdb::Request::TYPE_NONE: return "TYPE_NONE";
    case resdb::Request::TYPE_HEART_BEAT: return "TYPE_HEART_BEAT";
    case resdb::Request::TYPE_CLIENT_REQUEST: return "TYPE_CLIENT_REQUEST";
    case resdb::Request::TYPE_PRE_PREPARE: return "TYPE_PRE_PREPARE";
    case resdb::Request::TYPE_PREPARE: return "TYPE_PREPARE";
    case resdb::Request::TYPE_COMMIT: return "TYPE_COMMIT";
    case resdb::Request::TYPE_CLIENT_CERT: return "TYPE_CLIENT_CERT";
    case resdb::Request::TYPE_RESPONSE: return "TYPE_RESPONSE";
    case resdb::Request::TYPE_RECOVERY_DATA: return "TYPE_RECOVERY_DATA";
    case resdb::Request::TYPE_RECOVERY_DATA_RESP: return "TYPE_RECOVERY_DATA_RESP";
    case resdb::Request::TYPE_CHECKPOINT: return "TYPE_CHECKPOINT";
    case resdb::Request::TYPE_QUERY: return "TYPE_QUERY";
    case resdb::Request::TYPE_REPLICA_STATE: return "TYPE_REPLICA_STATE";
    case resdb::Request::TYPE_NEW_TXNS: return "TYPE_NEW_TXNS";
    case resdb::Request::TYPE_GEO_REQUEST: return "TYPE_GEO_REQUEST";
    case resdb::Request::TYPE_VIEWCHANGE: return "TYPE_VIEWCHANGE";
    case resdb::Request::TYPE_NEWVIEW: return "TYPE_NEWVIEW";
    case resdb::Request::TYPE_CUSTOM_QUERY: return "TYPE_CUSTOM_QUERY";
    case resdb::Request::TYPE_CUSTOM_CONSENSUS: return "TYPE_CUSTOM_CONSENSUS";
    case resdb::Request::TYPE_STATUS_SYNC: return "TYPE_STATUS_SYNC";
    default: return "TYPE_UNKNOWN";
  }
}

extern "C" int ResdbOmnetRemoveReplica(void* server_handle, int /*replica_id*/) {
  return server_handle ? 0 : -1;
}

extern "C" int ResdbOmnetSetVcTimeoutUs(void* server_handle, int64_t timeout_us) {
  if (!server_handle || timeout_us <= 0) return -1;
  auto* h = static_cast<ResdbOmnetServerHandle*>(server_handle);
  h->vc_timeout_us = timeout_us;
  if (h->consensus) h->consensus->SetVcTimeoutUs(timeout_us);
  std::cout << "[VC-BRIDGE] SetVcTimeoutUs timeout_us=" << timeout_us << "\n";
  return 0;
}

extern "C" int ResdbOmnetForceViewChange(void* server_handle) {
  if (!server_handle) return -1;
  auto* h = static_cast<ResdbOmnetServerHandle*>(server_handle);
  if (!h->consensus) return -1;
  std::cout << "[VC-BRIDGE] ForceViewChange requested\n";
  h->consensus->TriggerViewChange();
  return 0;
}

extern "C" int ResdbOmnetSetPbftSilent(void* server_handle, int silent) {
  if (!server_handle) return -1;
  auto* h = static_cast<ResdbOmnetServerHandle*>(server_handle);
  if (!h->consensus) return -1;
  std::cout << "[VC-BRIDGE] SetPbftSilent silent=" << silent << "\n";
  h->consensus->SetPbftSilent(silent != 0);
  return 0;
}

extern "C" int ResdbOmnetMarkReplicaInactive(void* server_handle,
                                             int replica_id,
                                             uint32_t min_epoch) {
  if (!server_handle) return -1;
  auto* h = static_cast<ResdbOmnetServerHandle*>(server_handle);
  if (!h->consensus) return -1;
  h->consensus->SetPbftSilent(true);
  std::cout << "[ACTIVE-DEPART] r" << replica_id
            << " min_epoch=" << min_epoch
            << " participation=inactive\n";
  return 0;
}

extern "C" int ResdbOmnetSetCertSnapshotFn(void* server_handle,
                                            ResdbCertSnapshotFn fn, void* ctx) {
  if (!server_handle) return -1;
  auto* h = static_cast<ResdbOmnetServerHandle*>(server_handle);
  if (!h->cert_state) return -1;
  std::lock_guard<std::mutex> lk(h->cert_state->mu);
  h->cert_state->fn  = fn;
  h->cert_state->ctx = ctx;
  return 0;
}

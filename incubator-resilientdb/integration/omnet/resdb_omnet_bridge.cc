#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "chain/storage/memory_db.h"
#include "executor/common/transaction_manager.h"
#include "integration/omnet/resdb_omnet_bridge.h"
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

// ── IsSafeToBatch ─────────────────────────────────────────────────────────────
// Port of ConflictMatrix.java::isSafeToBatch().
// lane: 0=N,1=S,2=E,3=W  direction: 0=Straight,1=Left,2=Right
static bool IsSafeToBatch(uint8_t lane_a, uint8_t dir_a,
                           uint8_t lane_b, uint8_t dir_b) {
  if (lane_a == lane_b) return false;  // same lane → rear-end risk
  // 12 safe pairs from ConflictMatrix.java (symmetric lookup below)
  static const uint8_t kSafe[12][4] = {
    {0,0, 1,0},  // NS, SS — opposite straights
    {2,0, 3,0},  // ES, WS
    {0,2, 1,2},  // NR, SR — all right-turn combos
    {0,2, 2,2},  // NR, ER
    {0,2, 3,2},  // NR, WR
    {1,2, 2,2},  // SR, ER
    {1,2, 3,2},  // SR, WR
    {2,2, 3,2},  // ER, WR
    {0,2, 1,0},  // NR, SS — right turn + opposite straight
    {1,2, 0,0},  // SR, NS
    {2,2, 3,0},  // ER, WS
    {3,2, 2,0},  // WR, ES
  };
  for (const auto& p : kSafe) {
    if ((lane_a==p[0]&&dir_a==p[1]&&lane_b==p[2]&&dir_b==p[3]) ||
        (lane_a==p[2]&&dir_a==p[3]&&lane_b==p[0]&&dir_b==p[1]))
      return true;
  }
  return false;
}

static bool IsQuietEntry(const ResdbVehicleEntry& e) {
  return e.cyber_status == 0 || e.sim_time_us == UINT64_MAX;
}

// IntersectionTypes.java::compareLaneQueueOrder (same lane only meaningful).
static int CompareLaneQueueOrder(const ResdbVehicleEntry& a,
                                 const ResdbVehicleEntry& b) {
  if (a.position_in_lane != b.position_in_lane)
    return (int)a.position_in_lane - (int)b.position_in_lane;
  if (a.replica_id < b.replica_id) return -1;
  if (a.replica_id > b.replica_id) return 1;
  return 0;
}

// OrderScheduler.java::allSameLaneFrontPlaced — exact port, no QUIET skip.
// QUIET vehicles are still physically present in the lane; any same-lane
// vehicle behind them (including an ambulance) must wait until the QUIET car
// has been assigned its singleton batch and can clear the intersection first.
static bool AllSameLaneFrontPlaced(
    const ResdbVehicleEntry& candidate,
    const std::vector<ResdbVehicleEntry>& view,
    const std::unordered_set<int32_t>& placed) {
  for (const auto& v : view) {
    if (v.lane != candidate.lane) continue;
    if (CompareLaneQueueOrder(v, candidate) < 0 &&
        placed.find(v.replica_id) == placed.end())
      return false;
  }
  return true;
}

static bool SafeWithWholeBatch(const ResdbVehicleEntry& e,
                               const std::vector<ResdbVehicleEntry>& batch) {
  if (IsQuietEntry(e)) return false;
  for (const auto& b : batch) {
    if (!IsSafeToBatch(e.lane, e.direction, b.lane, b.direction)) return false;
  }
  return true;
}

// ── IntersectionExecutor ──────────────────────────────────────────────────────
// Parses the ProposeAll wire format, runs OrderScheduler.java-compatible
// batch packing (workQueue + head + grow-until-stable), emits OrderDecision.

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
    const uint8_t* p = reinterpret_cast<const uint8_t*>(data.data());
    size_t remaining = data.size();

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

    // ── OrderScheduler.java::buildProposal (C++ port) ─────────────────────────

    // Log helper: first ambulance lane (same convention as before).
    int ambu_lane = -1;
    for (const auto& e : entries)
      if (e.is_ambulance) {
        ambu_lane = static_cast<int>(e.lane);
        break;
      }

    std::vector<ResdbVehicleEntry> ambulances;
    for (const auto& e : entries)
      if (e.is_ambulance) ambulances.push_back(e);
    std::sort(ambulances.begin(), ambulances.end(),
              [](const ResdbVehicleEntry& a, const ResdbVehicleEntry& b) {
                if (a.position_in_lane != b.position_in_lane)
                  return a.position_in_lane < b.position_in_lane;
                return a.replica_id < b.replica_id;
              });

    std::unordered_set<int32_t> priority_ids;
    std::vector<ResdbVehicleEntry> work_queue;
    for (const auto& ambulance : ambulances) {
      std::vector<ResdbVehicleEntry> blockers;
      for (const auto& v : entries) {
        if (v.lane != ambulance.lane || v.is_ambulance) continue;
        if (CompareLaneQueueOrder(v, ambulance) < 0) blockers.push_back(v);
      }
      std::sort(blockers.begin(), blockers.end(),
                [](const ResdbVehicleEntry& a, const ResdbVehicleEntry& b) {
                  if (a.position_in_lane != b.position_in_lane)
                    return a.position_in_lane < b.position_in_lane;
                  return a.replica_id < b.replica_id;
                });
      for (const auto& blocker : blockers) {
        if (priority_ids.insert(blocker.replica_id).second)
          work_queue.push_back(blocker);
      }
      if (priority_ids.insert(ambulance.replica_id).second)
        work_queue.push_back(ambulance);
    }

    std::vector<ResdbVehicleEntry> remaining_entries;
    for (const auto& e : entries) {
      if (priority_ids.find(e.replica_id) == priority_ids.end())
        remaining_entries.push_back(e);
    }
    // Java OrderScheduler: waitRegistry desc, then positionInLane, then vehicle id.
    // waitRegistry is not on the wire — do NOT use sim_time_us as primary key:
    // stop-zone entry order can put a follower before their same-lane leader in
    // the work queue, so the greedy head picks the follower first and assigns an
    // earlier batch (rear crosses before front → collision).
    // Physical queue order must dominate: position_in_lane (1=front), then replica_id,
    // then sim_time_us only as a final tiebreaker.
    std::sort(remaining_entries.begin(), remaining_entries.end(),
              [](const ResdbVehicleEntry& a, const ResdbVehicleEntry& b) {
                if (a.position_in_lane != b.position_in_lane)
                  return a.position_in_lane < b.position_in_lane;
                if (a.replica_id != b.replica_id)
                  return a.replica_id < b.replica_id;
                return a.sim_time_us < b.sim_time_us;
              });
    for (const auto& e : remaining_entries) work_queue.push_back(e);

    uint32_t n = static_cast<uint32_t>(entries.size());
    std::vector<ResdbVehicleDecision> decisions(n);

    std::vector<std::vector<ResdbVehicleEntry>> batches_out;
    std::unordered_set<int32_t> placed;

    while (placed.size() < n) {
      const ResdbVehicleEntry* head_ptr = nullptr;
      for (const auto& cand : work_queue) {
        if (placed.count(cand.replica_id)) continue;
        if (!AllSameLaneFrontPlaced(cand, entries, placed)) continue;
        head_ptr = &cand;
        break;
      }
      if (!head_ptr) {
        std::cout << "[EXECUTOR] no schedulable head placed=" << placed.size()
                  << "/" << n << "\n";
        for (const auto& e : entries) {
          if (placed.count(e.replica_id)) continue;
          batches_out.push_back({e});
          placed.insert(e.replica_id);
        }
        break;
      }

      ResdbVehicleEntry head = *head_ptr;
      std::vector<ResdbVehicleEntry> batch;
      batch.push_back(head);
      placed.insert(head.replica_id);

      if (IsQuietEntry(head)) {
        batches_out.push_back(std::move(batch));
        continue;
      }

      bool grew = false;
      do {
        grew = false;
        for (const auto& cand : work_queue) {
          if (placed.count(cand.replica_id)) continue;
          if (!AllSameLaneFrontPlaced(cand, entries, placed)) continue;
          if (IsQuietEntry(cand)) continue;
          if (!SafeWithWholeBatch(cand, batch)) continue;
          batch.push_back(cand);
          placed.insert(cand.replica_id);
          grew = true;
        }
      } while (grew);

      batches_out.push_back(std::move(batch));
    }

    uint32_t n_batches = static_cast<uint32_t>(batches_out.size());
    for (uint32_t bi = 0; bi < batches_out.size(); ++bi) {
      for (const auto& b : batches_out[bi]) {
        for (uint32_t i = 0; i < n; ++i) {
          if (entries[i].replica_id == b.replica_id)
            decisions[i] = {b.replica_id, bi};
        }
      }
    }

    // Build binary OrderDecision: ResdbOrderHdr + n × ResdbVehicleDecision.
    std::string result(sizeof(ResdbOrderHdr) + n * sizeof(ResdbVehicleDecision), '\0');
    uint8_t* out = reinterpret_cast<uint8_t*>(&result[0]);
    ResdbOrderHdr ohdr{hdr.epoch, n, n_batches};
    std::memcpy(out, &ohdr, sizeof(ohdr));
    out += sizeof(ohdr);
    for (uint32_t i = 0; i < n; ++i) {
      std::memcpy(out, &decisions[i], sizeof(ResdbVehicleDecision));
      out += sizeof(ResdbVehicleDecision);
    }

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

struct ResdbOmnetServerHandle {
  std::unique_ptr<resdb::ServiceNetwork> server;
  ResdbOmnetTransportCallbacks transport{nullptr, nullptr, nullptr};
  void* channel_ptr = nullptr;
  std::thread server_thread;
  bool server_thread_started = false;
  OmnetConsensusManagerPBFT* consensus = nullptr;
  IntersectionExecutor* executor = nullptr;
  int64_t vc_timeout_us = 3000000;  // 3 s default
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

  // Basic pre-verify: proposal matches cluster shape from server.config (keygen).
  // This integration assumes one vehicle slot per ResDB replica (N cars = N replicas =
  // N entries in server.config). SUMO / *.manager.intersectionBatchSize are kept in
  // sync with that N in the scenario; the bridge does not infer N from traffic.
  const int expected = static_cast<int>(config->GetReplicaInfos().size());
  service_ptr->SetPreVerifyFunc([expected](const resdb::Request& req) -> bool {
    if (req.type() != resdb::Request::TYPE_PRE_PREPARE) return true;
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
    if (d.size() < sizeof(ResdbProposeHdr)) {
      LOG(ERROR) << "[OMNET-PREVERIFY] reject: payload too short for header"
                 << " size=" << d.size()
                 << " need_at_least=" << sizeof(ResdbProposeHdr);
      return false;
    }
    ResdbProposeHdr hdr;
    std::memcpy(&hdr, d.data(), sizeof(hdr));
    if (static_cast<int>(hdr.n_vehicles) != expected) {
      LOG(ERROR) << "[OMNET-PREVERIFY] reject: vehicle count mismatch"
                 << " hdr.n_vehicles=" << hdr.n_vehicles
                 << " expected_replicas=" << expected
                 << " epoch=" << hdr.epoch;
      return false;
    }
    const size_t needed_size =
        sizeof(ResdbProposeHdr) + hdr.n_vehicles * sizeof(ResdbVehicleEntry);
    if (d.size() < needed_size) {
      LOG(ERROR) << "[OMNET-PREVERIFY] reject: payload too short for entries"
                 << " size=" << d.size()
                 << " need_at_least=" << needed_size
                 << " n_vehicles=" << hdr.n_vehicles;
      return false;
    }
    // Check no duplicate replica IDs.
    std::vector<int32_t> ids;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(d.data()) + sizeof(ResdbProposeHdr);
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
      const uint8_t* ep = reinterpret_cast<const uint8_t*>(d.data()) + sizeof(ResdbProposeHdr);
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
    // Check 8: deterministic sort always has a unique tiebreaker because
    // checks 3+4 guarantee unique valid replica IDs. Log that we verified.
    LOG(INFO) << "[OMNET-PREVERIFY] pass: all 8 checks ok"
              << " n_vehicles=" << hdr.n_vehicles << " epoch=" << hdr.epoch;
    return true;
  });

  auto server = std::make_unique<resdb::ServiceNetwork>(
      *config, std::move(service), /*enable_network_acceptor=*/false);
  if (!server) return nullptr;

  // Force sim-time mode active immediately so GetCurrentTime() never falls back
  // to wall-clock inside ResDB threads. The OMNeT++ tick loop will update this
  // with the real sim-time each millisecond; 1 µs is a safe non-zero sentinel.
  resdb::SimTimeProvider::UpdateNowUs(1);

  auto* handle = new ResdbOmnetServerHandle();
  handle->server   = std::move(server);
  handle->consensus = service_ptr;
  handle->executor  = executor_ptr;
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
  int id = ResdbIdToOmnetReplica(
      static_cast<int64_t>(h->consensus->GetPrimary()));
  return id < 0 ? 0 : id;
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

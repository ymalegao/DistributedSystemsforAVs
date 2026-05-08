#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "chain/storage/memory_db.h"
#include "executor/common/transaction_manager.h"
#include "integration/omnet/resdb_omnet_bridge.h"
#include "platform/config/resdb_config_utils.h"
#include "platform/consensus/ordering/pbft/consensus_manager_pbft.h"
#include "platform/networkstrate/replica_communicator.h"
#include "platform/networkstrate/service_network.h"
#include "platform/proto/resdb.pb.h"
#include "common/utils/sim_time_provider.h"

namespace {

int ResdbIdToOmnetReplica(int64_t resdb_node_id) {
  if (resdb_node_id <= 0) return -1;
  return static_cast<int>(resdb_node_id - 1);
}

// ── IntersectionExecutor ──────────────────────────────────────────────────────
// Parses the custom binary ProposeAll wire format, sorts vehicles, emits the
// custom binary OrderDecision wire format via the registered C callback.

class IntersectionExecutor : public resdb::TransactionManager {
 public:
  IntersectionExecutor() : resdb::TransactionManager() {}

  void SetOrderCallback(ResdbOrderDecidedFn cb, void* ctx) {
    std::lock_guard<std::mutex> lk(cb_mutex_);
    cb_ = cb;
    ctx_ = ctx;
  }

  std::unique_ptr<std::string> ExecuteData(const std::string& data) override {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(data.data());
    size_t remaining = data.size();

    if (remaining < sizeof(ResdbProposeHdr)) {
      return std::make_unique<std::string>();
    }

    ResdbProposeHdr hdr;
    std::memcpy(&hdr, p, sizeof(hdr));
    p += sizeof(hdr);
    remaining -= sizeof(hdr);

    if (remaining < hdr.n_vehicles * sizeof(ResdbVehicleEntry)) {
      return std::make_unique<std::string>();
    }

    std::vector<ResdbVehicleEntry> entries(hdr.n_vehicles);
    for (uint32_t i = 0; i < hdr.n_vehicles; ++i) {
      std::memcpy(&entries[i], p, sizeof(ResdbVehicleEntry));
      p += sizeof(ResdbVehicleEntry);
    }

    // Sort: ambulances first, then by arrival sim_time_us ascending.
    std::sort(entries.begin(), entries.end(),
              [](const ResdbVehicleEntry& a, const ResdbVehicleEntry& b) {
                if (a.is_ambulance != b.is_ambulance)
                  return a.is_ambulance > b.is_ambulance;
                return a.sim_time_us < b.sim_time_us;
              });

    // Build binary OrderDecision: ResdbOrderHdr + n × int32_t.
    uint32_t n = static_cast<uint32_t>(entries.size());
    std::string result(sizeof(ResdbOrderHdr) + n * sizeof(int32_t), '\0');
    uint8_t* out = reinterpret_cast<uint8_t*>(&result[0]);

    ResdbOrderHdr ohdr{hdr.epoch, n};
    std::memcpy(out, &ohdr, sizeof(ohdr));
    out += sizeof(ohdr);
    for (const auto& e : entries) {
      std::memcpy(out, &e.replica_id, sizeof(int32_t));
      out += sizeof(int32_t);
    }

    {
      std::lock_guard<std::mutex> lk(cb_mutex_);
      if (cb_) {
        cb_(ctx_,
            reinterpret_cast<const uint8_t*>(result.data()),
            static_cast<uint32_t>(result.size()));
      }
    }

    return std::make_unique<std::string>(result);
  }

 private:
  ResdbOrderDecidedFn cb_ = nullptr;
  void* ctx_ = nullptr;
  std::mutex cb_mutex_;
};

// ── OmnetReplicaCommunicator ──────────────────────────────────────────────────

class OmnetReplicaCommunicator : public resdb::ReplicaCommunicator {
 public:
  explicit OmnetReplicaCommunicator(const std::vector<resdb::ReplicaInfo>& replicas,
                                    ResdbOmnetTransportCallbacks* transport,
                                    int self_replica)
      : resdb::ReplicaCommunicator(replicas,
                                   /*verifier=*/nullptr,
                                   /*is_use_long_conn=*/false,
                                   /*epoll_num=*/0,
                                   /*tcp_batch=*/1,
                                   /*start_background_threads=*/false),
        transport_(transport),
        self_replica_(self_replica) {}

  int SendMessage(const google::protobuf::Message& message) override {
    if (!transport_ || !transport_->broadcast) return -1;
    std::string payload;
    if (!message.SerializeToString(&payload)) return -1;
    resdb::ResDBMessage wire;
    wire.set_data(std::move(payload));
    std::string bytes;
    if (!wire.SerializeToString(&bytes)) return -1;
    transport_->broadcast(transport_->ctx,
                          reinterpret_cast<const uint8_t*>(bytes.data()),
                          static_cast<uint32_t>(bytes.size()));
    return 0;
  }

  int SendMessage(const google::protobuf::Message& message,
                  const resdb::ReplicaInfo& replica_info) override {
    if (!transport_ || !transport_->send_to) return -1;
    std::string payload;
    if (!message.SerializeToString(&payload)) return -1;
    resdb::ResDBMessage wire;
    wire.set_data(std::move(payload));
    std::string bytes;
    if (!wire.SerializeToString(&bytes)) return -1;
    int to = ResdbIdToOmnetReplica(replica_info.id());
    if (to < 0 || to == self_replica_) return 0;
    transport_->send_to(transport_->ctx, to,
                        reinterpret_cast<const uint8_t*>(bytes.data()),
                        static_cast<uint32_t>(bytes.size()));
    return 0;
  }

  void BroadCast(const google::protobuf::Message& message) override {
    (void)SendMessage(message);
  }

  void SendMessage(const google::protobuf::Message& message,
                   int64_t node_id) override {
    int to = ResdbIdToOmnetReplica(node_id);
    if (to < 0 || !transport_ || !transport_->send_to) return;
    std::string payload;
    if (!message.SerializeToString(&payload)) return;
    resdb::ResDBMessage wire;
    wire.set_data(std::move(payload));
    std::string bytes;
    if (!wire.SerializeToString(&bytes)) return;
    transport_->send_to(transport_->ctx, to,
                        reinterpret_cast<const uint8_t*>(bytes.data()),
                        static_cast<uint32_t>(bytes.size()));
  }

 private:
  ResdbOmnetTransportCallbacks* transport_;
  int self_replica_ = -1;
};

// ── OmnetConsensusManagerPBFT ─────────────────────────────────────────────────

class OmnetConsensusManagerPBFT : public resdb::ConsensusManagerPBFT {
 public:
  OmnetConsensusManagerPBFT(const resdb::ResDBConfig& config,
                             std::unique_ptr<resdb::TransactionManager> executor,
                             ResdbOmnetTransportCallbacks* transport)
      : resdb::ConsensusManagerPBFT(config, std::move(executor)),
        transport_(transport) {}

  void SetTransport(ResdbOmnetTransportCallbacks* transport) {
    transport_ = transport;
    UpdateBroadCastClient();
  }

 protected:
  std::unique_ptr<resdb::ReplicaCommunicator> GetReplicaClient(
      const std::vector<resdb::ReplicaInfo>& replicas,
      bool /*is_use_long_conn*/ = false) override {
    return std::make_unique<OmnetReplicaCommunicator>(
        replicas, transport_, ResdbIdToOmnetReplica(config_.GetSelfInfo().id()));
  }

 private:
  ResdbOmnetTransportCallbacks* transport_;
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

  // Basic pre-verify: correct vehicle count, no duplicate IDs.
  const int expected = static_cast<int>(config->GetReplicaInfos().size());
  service_ptr->SetPreVerifyFunc([expected](const resdb::Request& req) -> bool {
    if (req.type() != resdb::Request::TYPE_PRE_PREPARE) return true;
    const std::string& d = req.data();
    if (d.size() < sizeof(ResdbProposeHdr)) return false;
    ResdbProposeHdr hdr;
    std::memcpy(&hdr, d.data(), sizeof(hdr));
    if (static_cast<int>(hdr.n_vehicles) != expected) return false;
    if (d.size() < sizeof(ResdbProposeHdr) + hdr.n_vehicles * sizeof(ResdbVehicleEntry))
      return false;
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
    return std::adjacent_find(ids.begin(), ids.end()) == ids.end();
  });

  auto server = std::make_unique<resdb::ServiceNetwork>(
      *config, std::move(service), /*enable_network_acceptor=*/false);
  if (!server) return nullptr;

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
  if (h->server) h->server->Stop();
  if (h->server_thread_started && h->server_thread.joinable())
    h->server_thread.join();
  h->server_thread_started = false;
  return 0;
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
  if (h->consensus) h->consensus->SetTransport(&h->transport);
  return 0;
}

extern "C" int ResdbOmnetSetChannel(void* server_handle, void* channel_ptr) {
  if (!server_handle) return -1;
  static_cast<ResdbOmnetServerHandle*>(server_handle)->channel_ptr = channel_ptr;
  return 0;
}

extern "C" int ResdbOmnetDeliverPacket(void* server_handle, int /*from_replica*/,
                                       const uint8_t* data, uint32_t len) {
  if (!server_handle || !data || len == 0) return -1;
  auto* h = static_cast<ResdbOmnetServerHandle*>(server_handle);
  if (!h->server) return -1;
  return h->server->InjectInboundPacket(reinterpret_cast<const char*>(data),
                                        static_cast<size_t>(len));
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

  resdb::Request req;
  req.set_type(resdb::Request::TYPE_CLIENT_REQUEST);
  req.set_data(std::string(reinterpret_cast<const char*>(payload), len));

  resdb::ResDBMessage wire;
  std::string req_bytes;
  if (!req.SerializeToString(&req_bytes)) return -1;
  wire.set_data(req_bytes);
  std::string wire_bytes;
  if (!wire.SerializeToString(&wire_bytes)) return -1;

  return h->server->InjectInboundPacket(wire_bytes.data(), wire_bytes.size());
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

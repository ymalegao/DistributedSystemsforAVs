#pragma once

#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include <cstdio>

#include <openssl/evp.h>

#include "veins/modules/application/ieee80211p/DemoBaseApplLayer.h"
#include "veins/modules/application/resDB/IV2VTransport.h"
#include "veins/modules/application/resDB/crypto/CryptoAuth.h"
#include "integration/omnet/resdb_omnet_bridge.h"

namespace veins {

class VEINS_API ResDBIntersectionApp : public DemoBaseApplLayer {

public:
    ~ResDBIntersectionApp() override;

protected:
    void initialize(int stage) override;
    void handleSelfMsg(cMessage* msg) override;
    void handlePositionUpdate(cObject* obj) override;
    void finish() override;

    void onBSM(DemoSafetyMessage* bsm) override {}
    void onWSM(BaseFrame1609_4* wsm) override;
    void onWSA(DemoServiceAdvertisment* wsa) override {}

private:
    // ── Step-2 logging transport ──────────────────────────────────────────────
    class LoggingTransport : public IV2VTransport {
    public:
        explicit LoggingTransport(int rid) : rid_(rid) {}
        void sendTo(int to, const uint8_t*, uint32_t len) override {
            fprintf(stderr, "[ResDB-TRANSPORT r%d] unicast → r%d  %u bytes\n", rid_, to, len);
        }
        void broadcast(const uint8_t*, uint32_t len) override {
            fprintf(stderr, "[ResDB-TRANSPORT r%d] broadcast  %u bytes\n", rid_, len);
        }
    private:
        int rid_;
    };

    // ── Step-3 radio transport ────────────────────────────────────────────────
    class VeinsTransport : public IV2VTransport {
    public:
        explicit VeinsTransport(ResDBIntersectionApp* app) : app_(app) {}
        void sendTo(int toReplica, const uint8_t* data, uint32_t len) override;
        void broadcast(const uint8_t* data, uint32_t len) override;
    private:
        ResDBIntersectionApp* app_;
    };

    struct PendingOutboundPacket {
        int toReplicaId = -1;
        std::vector<uint8_t> resdbBytes;
    };

    // ── Step 5: order decided C callback ─────────────────────────────────────
    static void onOrderDecided(void* ctx, const uint8_t* bytes, uint32_t len);

    // ── Helpers ───────────────────────────────────────────────────────────────
    void registerTransport();
    void enqueueOutbound(int toReplicaId, const uint8_t* data, uint32_t len);
    void drainOutboundQueue();
    void broadcastStateAnnounce();
    void proposeAll();
    void processOrders();
    void stopVehicle();
    void resumeVehicle(int position_in_order);
    double getDistanceToIntersection() const;

    // ── State ─────────────────────────────────────────────────────────────────
    void*  resdb_server_handle_ = nullptr;
    std::unique_ptr<IV2VTransport> transport_;

    cMessage* smoke_test_msg_       = nullptr;
    cMessage* transport_poll_msg_   = nullptr;
    cMessage* time_tick_msg_        = nullptr;
    cMessage* state_announce_msg_   = nullptr;
    cMessage* propose_timeout_msg_  = nullptr;

    simtime_t transport_poll_interval_  = 0.001;
    simtime_t time_tick_interval_       = 0.001;
    simtime_t state_announce_interval_  = 0.1;
    simtime_t cert_collection_timeout_  = 2.0;
    bool      enable_sim_time_provider_ = true;

    int      replicaId_         = 0;
    uint32_t sequenceNumber_    = 0;
    bool     useRadioTransport_ = false;

    std::mutex outbound_mutex_;
    std::deque<PendingOutboundPacket> outbound_queue_;

    EVP_PKEY* ec_private_key_ = nullptr;
    uint8_t   ec_pub_key_[CRYPTO_PUBKEY_BYTES] = {};

    std::string config_file_;
    std::string private_key_file_;
    std::string cert_file_;
    std::string log_dir_;

    // ── Step 5: vehicle state collection (key = replica_id) ──────────────────
    std::map<int, ResdbVehicleEntry> collected_states_;
    std::mutex states_mutex_;

    // ── Step 5: pending order decisions ──────────────────────────────────────
    std::deque<std::vector<uint8_t>> pending_orders_;
    std::mutex orders_mutex_;

    // ── Step 5: control flow ──────────────────────────────────────────────────
    bool      entered_stop_zone_ = false;
    bool      propose_submitted_ = false;
    bool      order_applied_     = false;
    uint32_t  current_epoch_     = 0;
    simtime_t stop_time_         = -1;
    simtime_t propose_time_      = -1;

    // ── Step 5: params ────────────────────────────────────────────────────────
    double intersection_x_   = 300.0;
    double intersection_y_   = 300.0;
    double stop_distance_    = 5.0;
    int    total_vehicles_   = 4;
    double cruise_speed_mps_ = 14.0;
    bool   is_ambulance_     = false;
};

} // namespace veins

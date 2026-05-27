#pragma once

#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include <cstdio>

#include <openssl/evp.h>

#include "veins/modules/application/ieee80211p/DemoBaseApplLayer.h"
#include "veins/modules/application/resDB/IV2VTransport.h"
#include "veins/modules/application/resDB/crypto/CryptoAuth.h"
#include "veins/modules/application/resDB/ResDBDecisionGossip.h"
#include "integration/omnet/resdb_omnet_bridge.h"

class ChannelMetrics;

namespace veins {

class BFTMessage;

class VEINS_API ResDBIntersectionApp : public DemoBaseApplLayer {

public:
    ~ResDBIntersectionApp() override;
    enum Direction { DIR_STRAIGHT = 0, DIR_LEFT = 1, DIR_RIGHT = 2 };
    void recordIntersectionDeparture(simtime_t departedAt);


protected:
    void initialize(int stage) override;
    void handleSelfMsg(cMessage* msg) override;
    void handlePositionUpdate(cObject* obj) override;
    void finish() override;

    void onBSM(DemoSafetyMessage* bsm) override {}
    void onWSM(BaseFrame1609_4* wsm) override;
    void onWSA(DemoServiceAdvertisment* wsa) override {}

private:

   
    // ── Phase state machine ───────────────────────────────────────────────────
    // Byzantine fault injection types
    enum ByzantineType {
        BYZANTINE_HONEST         = 0,  // Normal behavior
        BYZANTINE_FALSE_LANE     = 1,  // Claims wrong lane in ARRIVAL_ANNOUNCE
        BYZANTINE_INVALID_SIG    = 2,  // Attaches garbage signature bytes
        BYZANTINE_EQUIVOCATOR    = 3,  // Sends different direction to different peers
        BYZANTINE_SILENT_PRIMARY = 4,  // Primary suppresses proposeAll() — triggers VC
        BYZANTINE_BAD_PROPOSAL   = 5,  // Primary corrupts n_vehicles — PreVerify rejects
    };

    // Consensus phases
    enum ConsensusPhase {
        IDLE,
        COLLECTING_CERTS,      // Cars broadcast ARRIVAL_ANNOUNCE; replicas echo; f+1 certs assembled
        WAITING_FOR_CLEARANCE, // Waiting for clearance from intersection controller
        PULLING_FORWARD,       // Pulling forward to stop line
        EXECUTING,             // Cars crossing intersection
        DEPARTED,              // Car has crossed intersection (zombie mode)
    };
    ConsensusPhase current_phase_ = IDLE;

    // ── Arrival-cert direction ────────────────────────────────────────────────

    // ── Arrival cert protocol structs ─────────────────────────────────────────
    struct VehicleState {
        std::string vehicleId;
        std::string lane;
        int         positionInLane  = 1;
        Direction   direction       = DIR_STRAIGHT;
        bool        isAmbulance     = false;
        uint64_t    arrival_time_us = 0;
    };

    struct ArrivalAnnouncement {
        std::string          carId;
        std::string          laneId;
        std::string          lane;
        int                  positionInLane      = 1;
        Direction            direction           = DIR_STRAIGHT;
        bool                 isAmbulance         = false;
        double               claimedArrivalTime  = 0.0;
        int                  epoch               = 0;
        std::vector<uint8_t> ambulanceCertBytes;
        std::vector<uint8_t> ambulanceSigBytes;
        std::vector<uint8_t> signature;
    };

    struct ArrivalEcho {
        int         echoingReplicaId = -1;
        std::string targetCarId;
        std::string lane;
        int         positionInLane = 1;
        Direction   direction      = DIR_STRAIGHT;
        bool        isAmbulance    = false;
        int         epoch          = 0;
        uint8_t     signerPubKey[CRYPTO_PUBKEY_BYTES] = {};
        uint8_t     signature[CRYPTO_SIG_MAX_BYTES]   = {};
        uint8_t     signatureLen = 0;
    };

    struct ArrivalCert {
        std::string              carId;
        std::string              lane;
        int                      positionInLane = 1;
        Direction                direction      = DIR_STRAIGHT;
        bool                     isAmbulance    = false;
        int                      epoch          = 0;
        std::vector<ArrivalEcho> echoes;
    };

    struct VerificationResult {
        bool        isValid  = false;
        std::string reason;
        std::string actualLaneId;
        double      actualPosition = 0.0;
    };

    // ── Transport ─────────────────────────────────────────────────────────────
    class LoggingTransport : public IV2VTransport {
    public:
        explicit LoggingTransport(int rid);
        void sendTo(int to, const uint8_t*, uint32_t len) override;
        void broadcast(const uint8_t*, uint32_t len) override;
       
    private:
        int rid_;
    };

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

    void registerTransport();
    void enqueueOutbound(int toReplicaId, const uint8_t* data, uint32_t len);
    void drainOutboundQueue();
    void handleResdbConsensusMessage(BFTMessage* bft);
    void sendBFTMessage(int toReplicaId, const std::vector<uint8_t>& payload, int msgType);

    // ── Arrival cert protocol ────────────────────────────────────────────────
    void tryStartCertCollectionTimer(bool rearm = false);
    void broadcastArrivalAnnouncement();
    void handleArrivalAnnouncement(BFTMessage* msg);
    void sendArrivalEcho(const ArrivalAnnouncement& ann);
    void handleArrivalEcho(BFTMessage* msg);
    void broadcastArrivalCert(const ArrivalCert& cert);
    void scheduleNextCertRetry();
    void stopCertBroadcastRetries();
    void handleArrivalCert(BFTMessage* msg);

    // ── Post-consensus order gossip (Type 9) ──────────────────────────────────
    void triggerGossip(uint32_t epoch, const std::vector<uint8_t>& order_bytes);
    void scheduleNextGossip();
    void stopGossip();
    void handleDecisionGossip(BFTMessage* bft);
    bool applyGossipOrder(const std::vector<uint8_t>& order_bytes, uint32_t epoch);
    bool validateArrivalCert(const ArrivalCert& cert);

    std::vector<uint8_t> serializeArrivalAnnouncement(const ArrivalAnnouncement& ann);
    ArrivalAnnouncement  deserializeArrivalAnnouncement(BFTMessage* msg);
    std::vector<uint8_t> serializeArrivalEcho(const ArrivalEcho& echo);
    ArrivalEcho          deserializeArrivalEcho(BFTMessage* msg);
    std::vector<uint8_t> serializeArrivalCert(const ArrivalCert& cert);
    ArrivalCert          deserializeArrivalCert(BFTMessage* msg);

    // ── ResDB decision handling ───────────────────────────────────────────────
    static void onOrderDecided(void* ctx, const uint8_t* bytes, uint32_t len);
    void proposeAll();
    void processOrders();

    // ── TraCI helpers (ported from V2VTraCI.cc) ───────────────────────────────
    double getDistanceToIntersection();
    void   discoverLane();
    bool   vehicleHasClearedIntersectionTraCI(const std::string& carId);
    void   stopVehicle();
    void   resumeVehicle(int position_in_order);
    bool   isApproachingIntersection();
    bool checkIfDeparted();
    VerificationResult verifyCarPosition(const std::string& carId,
                                         const std::string& claimedLane,
                                         double claimedPosition, double tolerance);
    int    extractReplicaId(const std::string& carId) const;

    

    // ── State ─────────────────────────────────────────────────────────────────
    void*  resdb_server_handle_ = nullptr;
    std::unique_ptr<IV2VTransport> transport_;

    cMessage* smoke_test_msg_          = nullptr;
    cMessage* transport_poll_msg_      = nullptr;
    cMessage* time_tick_msg_           = nullptr;
    cMessage* broadcastArrivalAnnouncement_timer_ = nullptr;
    cMessage* propose_timeout_msg_     = nullptr;
    cMessage* cert_retry_timer_        = nullptr;
    cMessage* initial_announce_msg_    = nullptr;
    cMessage* stop_sign_timeout_msg_   = nullptr;
    cMessage* consensus_timeout_msg_   = nullptr;
    cMessage* resume_msg_              = nullptr;
    int       pending_resume_position_ = 0;
    cMessage* vc_trigger_msg_          = nullptr;  // follower VC trigger after primary silence
    cMessage* channel_metrics_timer_   = nullptr;  // 100 ms CSV flush (channel + SINR)

    // Batch-aware clearance gating (mirrors V2VOrderProtocol.cc executeBatch logic).
    // my_batch_index_:        which batch this vehicle belongs to (0-based).
    // preceding_batch_cars_:  replica IDs of all vehicles in batch_index - 1.
    //                         All must clear the intersection before we resume.
    cMessage*                clearance_poll_msg_       = nullptr;
    int                      my_batch_index_           = -1;
    std::vector<int>         preceding_batch_cars_;     // replica IDs to wait on
    simtime_t                clearance_started_        = -1;
    double                   clearance_poll_period_sec_ = 0.1;
    double                   clearance_timeout_sec_     = 8.0;

    // Legacy single-predecessor fields kept for existing clearance-poll handler
    // (now polls all preceding_batch_cars_ instead of one car).
    std::vector<int>         decided_order_;
    int                      my_position_in_order_    = -1;  // position within batch (unused, kept for logs)

    simtime_t transport_poll_interval_  = 0.001;
    simtime_t time_tick_interval_       = 0.001;
    simtime_t broadcast_arrival_announcement_interval_ = 0.5;
    simtime_t cert_collection_timeout_  = 2.0;
    bool      enable_sim_time_provider_ = true;

    int      replicaId_         = 0;
    uint32_t sequenceNumber_    = 0;
    bool     useRadioTransport_ = false;
    bool moduleIsAmbulance = false;
    bool ambulanceColorSet = false;

    unsigned int sentMessages_ = 0;
    unsigned int receivedMessages_ = 0;


    

  
    

    std::mutex outbound_mutex_;
    std::deque<PendingOutboundPacket> outbound_queue_;

    EVP_PKEY* ec_private_key_ = nullptr;
    uint8_t   ec_pub_key_[CRYPTO_PUBKEY_BYTES] = {};

    std::string config_file_;
    std::string private_key_file_;
    std::string cert_file_;
    std::string log_dir_;

    // ── Cert protocol state ───────────────────────────────────────────────────
    std::map<std::string, VehicleState>             local_vehicle_states_;
    std::map<std::string, ArrivalCert>              collected_certs_;
    std::map<std::string, std::vector<ArrivalEcho>> my_received_echoes_;
    std::set<std::string>                           physically_observed_cars_;
    std::set<std::string>                           arrival_announcements_received_;
    std::set<std::string>                           echoed_cars_;  // cars we actually sent an echo to (not FALSE_LANE)
    bool cert_collection_started_ = false;
    simtime_t cert_collection_start_time_ = SIMTIME_ZERO;
    bool deferred_propose_after_cert_timeout_ = false;

    bool   enable_cert_retries_      = true;
    double cert_retry_interval_      = 0.1;
    int    cert_retry_max_           = 30;
    ArrivalCert cert_pending_retries_{};
    int    cert_retry_count_         = 0;
    bool cert_broadcast_          = false;

    // ── Post-consensus order gossip (Type 9) ──────────────────────────────────
    static constexpr int kDecisionGossipType = 9;

    resdb_gossip::GossipAccumulator  gossip_acc_;
    resdb_gossip::CertRelayTracker   cert_relay_tracker_;
    cMessage*            gossip_timer_              = nullptr;
    int                  gossip_retry_count_        = 0;
    uint32_t             gossip_epoch_              = 0;
    std::vector<uint8_t> gossip_order_bytes_;
    uint32_t             last_committed_epoch_      = 0;
    bool                 has_committed_order_       = false;
    std::vector<uint8_t> committed_order_bytes_;
    bool                 gossip_enabled_            = false;
    double               gossip_initial_interval_   = 0.5;
    int                  gossip_max_retries_        = 5;

    // ── TraCI lane state ──────────────────────────────────────────────────────
    bool        lane_discovered_ = false;
    bool        is_stopped_      = false;
    bool        is_departed_ = false;
    std::string my_lane_id_;
    std::string car_ahead_;
    std::vector<std::string> lane_queue_;
    double stop_distance_;
    double car_ahead_stop_pos = -1.0;
    std::string myLaneTriggerCar;

    //metrics
    simtime_t stopTime = -1;          // When the car physically entered the stop zone
    simtime_t departureTime = -1;     // When the car physically cleared the intersection zone


    // ── Pending order decisions (from ResDB worker thread) ────────────────────
    std::deque<std::vector<uint8_t>> pending_orders_;
    std::mutex orders_mutex_;

    // ── Control flow ──────────────────────────────────────────────────────────
    bool      debug_cert_protocol_ = false;
    bool      debug_order_delivery_ = false;

    bool      entered_stop_zone_ = false;
    bool      propose_submitted_ = false;
    bool      order_applied_     = false;
    uint32_t  current_epoch_     = 0;
    simtime_t stop_time_         = -1;
    simtime_t cleared_time_    = -1;
    simtime_t propose_time_      = -1;

    // ── Params ────────────────────────────────────────────────────────────────
    int    total_vehicles_        = 4;
    double cruise_speed_mps_      = 14.0;
    bool   is_ambulance_          = false;
    bool          is_byzantine_   = false;
    ByzantineType byzantine_type_ = BYZANTINE_HONEST;
    int           last_known_primary_ = 0;
    double        pbft_vc_timeout_sec_ = 3.0;
    double trigger_join_time_     = 0.5;
    double arrival_slot_sec_      = 0.1;
    double stop_sign_timeout_sec_ = 10.0;
    double consensus_timeout_sec_ = 30.0;
    std::string intended_direction_ = "S";
    std::string intended_lane_      = "";   // explicit N/S/E/W override; empty = auto-detect from TraCI

    // Per-vehicle channel utilization + SINR CSV (optional; see NED enableChannelMetricsCsv)
    ChannelMetrics* channel_metrics_ = nullptr;
};

} // namespace veins

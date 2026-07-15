#pragma once

#include <deque>
#include <array>
#include <cstdint>
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
        BYZANTINE_SILENT_PRIMARY          = 4,  // Primary suppresses proposeAll() — triggers VC
        BYZANTINE_BAD_PROPOSAL            = 5,  // Primary corrupts n_vehicles — PreVerify rejects
        BYZANTINE_FAKE_AMBULANCE          = 6,  // Primary flips is_ambulance 0→1 for non-ambulance car — PreVerify Check 10 rejects
        BYZANTINE_FAKE_AMBULANCE_FOLLOWER = 7,  // Follower claims isAmbulance=true without cert — cert gate catches this
        BYZANTINE_TAMPER_LANE             = 8,  // Primary: quiet real S car + reassign E car's lane to S → scheduler batches N+E simultaneously → CRASH
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

    enum class DiscoveryState {
        INACTIVE,
        COLLECTING,
        DRAINING_CERTS,
        COMPLETE,
    };

    enum class LocalCertState {
        NOT_ASSEMBLED,
        QUEUED,
        AIRED,
    };

    enum class DiscoveryCloseReason {
        NONE,
        STABILIZED,
        DEADLINE,
    };

    struct DiscoveryRound {
        DiscoveryState state = DiscoveryState::INACTIVE;
        uint32_t epoch = 0;
        simtime_t lastNewIntentAt = -1;
        simtime_t collectionStartedAt = SIMTIME_ZERO;
        LocalCertState localCert = LocalCertState::NOT_ASSEMBLED;
        DiscoveryCloseReason closeReason = DiscoveryCloseReason::NONE;

        void reset(uint32_t newEpoch, simtime_t now)
        {
            state = DiscoveryState::COLLECTING;
            epoch = newEpoch;
            lastNewIntentAt = now;
            collectionStartedAt = SIMTIME_ZERO;
            localCert = LocalCertState::NOT_ASSEMBLED;
            closeReason = DiscoveryCloseReason::NONE;
        }

        bool localCertAssembled() const
        {
            return localCert != LocalCertState::NOT_ASSEMBLED;
        }

        bool localCertAired() const
        {
            return localCert == LocalCertState::AIRED;
        }
    };

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

    enum CancelReason {
        CANCEL_CRASH = 0,
        CANCEL_EMERGENCY = 1,
    };

    enum class CancelState {
        INACTIVE,
        WITNESSING,
        DRAINING,
        CONSENSUS,
        COMMITTED,
    };

    struct CancelEcho {
        int          echoingReplicaId = -1;
        uint32_t     cancelledEpoch = 0;
        CancelReason reason = CANCEL_CRASH;
        std::string  reasonRef;
        uint8_t      signerPubKey[CRYPTO_PUBKEY_BYTES] = {};
        uint8_t      signature[CRYPTO_SIG_MAX_BYTES] = {};
        uint8_t      signatureLen = 0;
    };

    struct CancelCert {
        uint32_t                cancelledEpoch = 0;
        CancelReason            reason = CANCEL_CRASH;
        std::string             reasonRef;
        std::vector<CancelEcho> echoes;
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

    struct ConsensusRetryKey {
        uint64_t view = 0;
        uint64_t seq = 0;
        std::array<uint8_t, 32> requestHash{};

        bool operator<(const ConsensusRetryKey& other) const
        {
            if (view != other.view) return view < other.view;
            if (seq != other.seq) return seq < other.seq;
            return requestHash < other.requestHash;
        }
    };

    struct ConsensusRetryPacket {
        std::vector<uint8_t> resdbBytes;
        int type = 0;
        int attempts = 0;
    };

    class ConsensusRetryManager {
    public:
        using PhaseMap = std::map<int, ConsensusRetryPacket>;
        using InstanceMap = std::map<ConsensusRetryKey, PhaseMap>;

        bool remember(const std::vector<uint8_t>& bytes,
                      const ResdbPacketRequestInfo& info);
        bool empty() const { return instances_.empty(); }
        size_t size() const;
        void clear() { instances_.clear(); }
        InstanceMap& instances() { return instances_; }

    private:
        InstanceMap instances_;
    };

    // Discovery frames are held for the replicaId*slot stagger so a round can
    // stop ANN/ECHO traffic while allowing already-created CERTs to drain.
    struct PendingDiscoveryTx {
        int toReplicaId = -1;
        int msgType = 0;
        std::vector<uint8_t> payload;
        simtime_t fireTime;
        uint32_t epoch = 0;
        bool localCert = false;
        bool witnessTraffic = false;
    };

    void registerTransport();
    void enqueueOutbound(int toReplicaId, const uint8_t* data, uint32_t len);
    void drainOutboundQueue();
    void sendConsensusBytes(const std::vector<uint8_t>& bytes, int toReplicaId,
                            const char* source, int retryAttempt = 0);
    void rememberConsensusRetry(const std::vector<uint8_t>& bytes,
                                const ResdbPacketRequestInfo& info);
    void retryConsensusPackets();
    void clearConsensusRetries(const char* reason);
    void handleResdbConsensusMessage(BFTMessage* bft);
    void handleResdbConsensusRelay(BFTMessage* bft);
    void maybeRelayResdbConsensusBytes(const uint8_t* data, uint32_t len,
                                       const ResdbPacketRequestInfo& info,
                                       const char* source);
    bool isConsensusRelayEligible(const ResdbPacketRequestInfo& info) const;
    std::string consensusRelayKey(const uint8_t* data, uint32_t len,
                                  const ResdbPacketRequestInfo& info) const;
    void sendBFTMessage(int toReplicaId, const std::vector<uint8_t>& payload, int msgType,
                        bool localCert = false, bool witnessTraffic = false);
    void sendBFTMessageNow(int toReplicaId, const std::vector<uint8_t>& payload, int msgType);
    bool isDiscoveryAirMsgType(int msgType) const;
    bool discoveryAcceptsNewTx(int msgType, bool localCert, bool witnessTraffic) const;
    void enqueueDiscoveryTx(int toReplicaId, const std::vector<uint8_t>& payload,
                            int msgType, simtime_t fireTime, bool localCert,
                            bool witnessTraffic);
    void scheduleDiscoveryTxFlush();
    void flushDueDiscoveryTxs();
    void cancelPendingDiscoveryTxs(const char* reason);
    void discardPendingDiscoveryNonCerts(const char* reason);
    bool hasPendingDiscoveryCerts(uint32_t epoch) const;

    // ── Arrival cert protocol ────────────────────────────────────────────────
    void startDiscoveryRound(const char* reason);
    void armDiscoveryTimers(const char* reason);
    void noteDiscoveryIntent(const std::string& carId, const char* source);
    bool discoveryViewCertified(std::vector<int>* missing = nullptr) const;
    void maybeAdvanceDiscovery(const char* reason, bool deadline = false);
    void beginDiscoveryDrain(const char* reason, bool deadline);
    void maybeCompleteDiscoveryDrain(const char* reason);
    void finishDiscoveryRound(const char* reason);
    void deactivateDiscovery(const char* reason);
    const char* discoveryStateName() const;
    void broadcastArrivalAnnouncement();
    void attachAmbulanceCryptoToAnnouncement(ArrivalAnnouncement& ann);
    void handleArrivalAnnouncement(BFTMessage* msg);
    void handleArrivalAnnouncement(BFTMessage* msg, bool viaGossip, int carrierReplicaId);
    void gossipArrivalAnnouncement(const ArrivalAnnouncement& ann,
                                   const std::vector<uint8_t>& announceBytes);
    void sendArrivalAnnouncementGossipPayload(const std::string& carId,
                                             uint32_t epoch,
                                             const std::vector<uint8_t>& announceBytes,
                                             const char* reason);
    void handleArrivalAnnouncementGossip(BFTMessage* msg);
    void sendArrivalEcho(const ArrivalAnnouncement& ann);
    void handleArrivalEcho(BFTMessage* msg);
    void broadcastArrivalCert(const ArrivalCert& cert);
    void scheduleNextCertRetry();
    void stopCertBroadcastRetries();
    void startStopZoneCertGossip(const char* reason, bool immediate = true);
    void scheduleNextStopZoneCertGossip();
    void stopStopZoneCertGossip();
    void broadcastCollectedCerts(const char* reason);
    void handleArrivalCert(BFTMessage* msg);

    // ── Post-consensus order gossip (Type 9) ──────────────────────────────────
    void triggerGossip(uint32_t epoch, const std::vector<uint8_t>& order_bytes);
    void scheduleNextGossip();
    void stopGossip();
    void handleDecisionGossip(BFTMessage* bft);
    bool applyGossipOrder(const std::vector<uint8_t>& order_bytes, uint32_t epoch);
    bool validateArrivalCert(const ArrivalCert& cert);

    // ── Cancel / rollback protocol (Types 12, 13) ─────────────────────────────
    bool maybeTriggerEmergencyRollbackFromAnnouncement(const ArrivalAnnouncement& ann);
    bool maybeTriggerEmergencyRollbackFromCert(const ArrivalCert& cert);
    void maybeTriggerCrashRollback(const std::string& reasonRef);
    void sendCancelEcho(uint32_t cancelledEpoch, CancelReason reason, const std::string& reasonRef);
    void handleCancelEcho(BFTMessage* msg);
    void broadcastCancelCert(const CancelCert& cert);
    void handleCancelCert(BFTMessage* msg);
    bool validateCancelCert(const CancelCert& cert) const;
    std::vector<uint8_t> serializeCancelEcho(const CancelEcho& echo) const;
    CancelEcho deserializeCancelEcho(BFTMessage* msg) const;
    std::vector<uint8_t> serializeCancelCert(const CancelCert& cert) const;
    CancelCert deserializeCancelCert(BFTMessage* msg) const;
    void scheduleNextCancelCertRetry();
    void stopCancelCertRetries();
    void beginCancelDrain(const char* reason);
    void finishCancelDrain();
    const char* cancelStateName() const;
    void handleValidCancelJustification(uint32_t cancelledEpoch,
                                        CancelReason reason,
                                        const std::string& reasonRef,
                                        const std::vector<uint8_t>& justification);
    bool isRecallable();
    void beginPostCancelDiscovery(uint32_t cancelledEpoch,
                                  CancelReason reason,
                                  const std::string& reasonRef,
                                  const std::vector<uint8_t>& justification);
    std::vector<int> rollbackCertedCandidates() const;
    std::vector<int> rollbackMembershipCandidates() const;
    int minRollbackVoteN() const;
    int minRollbackMembershipSize() const;
    bool isRollbackPerEpochMode() const;
    int chooseRollbackProposer();
    int designatedRollbackUnavailableReporter() const;
    void logDiscoveryDiagnostics(const char* reason) const;
    bool shouldIncludeInRollbackMembership(int replicaId) const;
    void trySubmitCancelProposal(const char* reason);
    void proposeCancel();
    int chooseCancelProposer();
    std::vector<int> cancelElectorateCandidates() const;
    std::vector<int> cancelLeaderCandidatesForBatch(int activeBatch) const;
    int perceivedActiveBatch() const;
    bool isEpochTombstoned(uint32_t epoch) const;
    void handleCancelCommitDecision(const std::vector<uint8_t>& dec);
    std::vector<uint8_t> buildCancelCommitRef(uint32_t cancelledEpoch) const;
    bool hasCommittedCancel(uint32_t epoch) const;
    void triggerCancelCommitGossip(uint32_t cancelledEpoch,
                                   const std::vector<uint8_t>& attestation);
    void handleCancelCommitGossip(BFTMessage* bft);
    void broadcastCancelCommitAttestation(const ResdbCancelDecisionHdr& dh);
    void trySubmitRollbackProposal(const char* reason);
    std::string cancelReasonKey(uint32_t epoch, CancelReason reason,
                                const std::string& reasonRef) const;
    std::string cancelSignPayload(uint32_t cancelledEpoch, CancelReason reason,
                                  const std::string& reasonRef,
                                  int echoingReplicaId) const;

    std::vector<uint8_t> serializeArrivalAnnouncement(const ArrivalAnnouncement& ann);
    ArrivalAnnouncement  deserializeArrivalAnnouncement(BFTMessage* msg);
    std::vector<uint8_t> serializeArrivalEcho(const ArrivalEcho& echo);
    ArrivalEcho          deserializeArrivalEcho(BFTMessage* msg);
    std::vector<uint8_t> serializeArrivalCert(const ArrivalCert& cert);
    ArrivalCert          deserializeArrivalCert(BFTMessage* msg);

    // ── ResDB decision handling ───────────────────────────────────────────────
    static void onOrderDecided(void* ctx, const uint8_t* bytes, uint32_t len);
    static void certSnapshotCallback(void* ctx, ResdbCertEntry* out, uint32_t* cnt);
    void proposeAll();
    void processOrders();
    bool isReplicaConfiguredByzantine(int replicaId) const;
    static bool hasCompletedReplicaEpoch(int replicaId, uint32_t epoch);
    static void markCompletedReplicaEpoch(int replicaId, uint32_t epoch);
    void detectConsensusAttackOutcome(const ResdbVehicleDecision* decisions, uint32_t n, uint32_t n_batches);
    bool detectUnsafeBatch(const ResdbVehicleDecision* decisions, uint32_t n, uint32_t n_batches);
    bool detectFalsePriorityGrant(const ResdbVehicleDecision* decisions, uint32_t n);
    void applyByzantineSilentPrimary();
    void applyByzantineBadProposal(ResdbProposeHdr& hdr, std::vector<uint8_t>& buf);
    void applyByzantineFakeAmbulance(uint8_t* base, uint32_t n);
    void applyByzantineTamperLane(uint8_t* base, uint32_t n);
    int countStaticCollectedCerts() const;
    int CertPrimary() const;
    // Tolerated Byzantine faults f. Explicit toleratedFaults par wins; otherwise
    // derived from the PBFT membership size N (num_replicas_), so f scales when
    // static intersection units join the quorum.
    int toleratedF() const;

    // ── TraCI helpers (ported from V2VTraCI.cc) ───────────────────────────────
    // Command interface for TraCI queries. Vehicles use their own TraCIMobility;
    // static intersection units (no mobility) fall back to the global manager so
    // they can still witness/verify vehicles. Returns nullptr if neither exists.
    TraCICommandInterface* getTraCI() const;
    double getDistanceToIntersection();
    bool   isInOrPastConflictBox();
    int    countRollbackPerceivedVehicles() const;
    void   discoverLane();
    bool   vehicleHasClearedIntersectionTraCI(const std::string& carId) const;
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
    cMessage* discovery_deadline_msg_  = nullptr;
    cMessage* discovery_settle_msg_    = nullptr;
    cMessage* cert_retry_timer_        = nullptr;
    cMessage* cert_gossip_timer_       = nullptr;
    cMessage* initial_announce_msg_    = nullptr;
    cMessage* stop_sign_timeout_msg_   = nullptr;
    cMessage* consensus_timeout_msg_   = nullptr;
    cMessage* resume_msg_              = nullptr;
    cMessage* cancel_cert_retry_timer_ = nullptr;
    cMessage* cancel_drain_timer_      = nullptr;
    cMessage* consensus_retry_timer_   = nullptr;
    cMessage* rollback_vc_timer_       = nullptr;
    cMessage* cancel_vc_timer_         = nullptr;
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
    simtime_t discovery_intent_settle_  = 1.5;
    bool      enable_sim_time_provider_ = true;

    int      replicaId_         = 0;
    uint32_t sequenceNumber_    = 0;
    bool     useRadioTransport_ = false;
    bool moduleIsAmbulance = false;
    bool ambulanceColorSet = false;
    int      tolerated_faults_ = -1;
    int      configured_consensus_quorum_ = -1;
    int      configured_cert_threshold_ = -1;

    unsigned int sentMessages_ = 0;
    unsigned int receivedMessages_ = 0;
    uint64_t sentPayloadBytes_ = 0;
    uint64_t quietHonestVehicles_ = 0;
    uint64_t quietHonestOpportunities_ = 0;


    

  
    

    std::mutex outbound_mutex_;
    std::deque<PendingOutboundPacket> outbound_queue_;

    EVP_PKEY* ec_private_key_ = nullptr;
    EVP_PKEY* ambulance_private_key_ = nullptr;
    std::vector<uint8_t> my_ambulance_cert_bytes_;
    uint8_t   ec_pub_key_[CRYPTO_PUBKEY_BYTES] = {};

    std::string config_file_;
    std::string private_key_file_;
    std::string cert_file_;
    std::string log_dir_;

    // ── Cert protocol state ───────────────────────────────────────────────────
    // Guards collected_certs_ against concurrent access from ResDB pre-verify threads.
    mutable std::mutex                              certs_mutex_;
    std::map<std::string, VehicleState>             local_vehicle_states_;
    std::map<std::string, ArrivalCert>              collected_certs_;
    std::map<std::string, std::vector<ArrivalEcho>> my_received_echoes_;
    std::set<std::string>                           observed_intent_cars_;
    std::set<std::string>                           arrival_announcements_received_;
    std::set<std::string>                           echoed_cars_;  // cars we actually sent an echo to (not FALSE_LANE)
    std::set<std::string>                           uncertified_ambulance_claimers_;
    std::set<std::string>                           cert_gate_rejected_ambulance_claimers_;
    DiscoveryRound discovery_;

    bool   enable_cert_retries_      = true;
    double cert_retry_interval_      = 0.1;
    int    cert_retry_max_           = 30;
    ArrivalCert cert_pending_retries_{};
    int    cert_retry_count_         = 0;
    bool cert_broadcast_          = false;
    simtime_t cert_gossip_deadline_ = -1;

    // ── Post-consensus order gossip (Type 9) ──────────────────────────────────
    static constexpr int kDecisionGossipType = 9;
    static constexpr int kArrivalAnnounceGossipType = 10;
    static constexpr int kResdbConsensusRelayType = 11;
    static constexpr int kCancelEchoType = 12;
    static constexpr int kCancelCertType = 13;
    static constexpr int kCancelCommitGossipType = 14;

    resdb_gossip::GossipAccumulator  gossip_acc_;
    resdb_gossip::CertRelayTracker   cert_relay_tracker_;
    resdb_gossip::AnnouncementRelayTracker announcement_relay_tracker_;
    std::set<std::string> consensus_relay_seen_;
    std::vector<PendingDiscoveryTx> pending_discovery_txs_;
    cMessage*            discovery_tx_flush_timer_ = nullptr;
    std::map<std::pair<uint32_t, std::string>, resdb_gossip::PendingRelay> pending_relays_;
    cMessage*            gossip_timer_              = nullptr;
    int                  gossip_retry_count_        = 0;
    uint32_t             gossip_epoch_              = 0;
    std::vector<uint8_t> gossip_order_bytes_;
    uint32_t             last_committed_epoch_      = 0;
    bool                 has_committed_order_       = false;
    std::vector<uint8_t> committed_order_bytes_;
    std::set<int>        committed_order_vehicle_ids_;
    std::vector<std::vector<int>> committed_order_batches_;
    std::set<uint32_t>   tombstoned_epochs_;
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
    // PBFT membership size N = vehicles + static intersection units. Defaults to
    // total_vehicles_ when no units are configured (totalReplicas = -1).
    int    num_replicas_          = 4;
    // When true, this module is a static intersection unit (RSU-hosted): quorum
    // participant + witness/echo + executor, but never announces/stops/crosses/proposes.
    bool   is_intersection_unit_  = false;
    double cruise_speed_mps_      = 14.0;
    bool   is_ambulance_          = false;
    bool          is_byzantine_          = false;
    ByzantineType byzantine_type_        = BYZANTINE_HONEST;
    bool          byzantine_pbft_silent_ = false;
    bool          enableAmbulanceCertGate_ = false;  // when true, rejects ambulance claims with no ambulanceCertBytes
    int           last_known_primary_ = 0;
    bool          bad_proposal_injected_ = false;
    int           fake_ambulance_proposal_replica_id_ = -1;
    int           tamper_lane_proposal_replica_id_ = -1;
    double        pbft_vc_timeout_sec_ = 3.0;
    bool          enableRollback_ = false;
    double        cancel_cert_retry_interval_sec_ = 0.1;
    int           cancel_cert_retry_max_ = 10;
    double        consensus_retry_interval_sec_ = 0.12;
    int           consensus_retry_max_ = 8;
    double        braking_decel_mps2_ = 4.5;
    double        processing_latency_margin_ = 2.0;
    double        rollback_vc_timeout_sec_ = 3.0;
    bool          rollback_fault_mode_per_epoch_ = true;
    bool          cancel_consensus_pending_ = false;
    bool          cancel_propose_submitted_ = false;
    CancelState   cancel_state_ = CancelState::INACTIVE;
    int           cancel_active_batch_ = -1;
    int           cancel_primary_ = -1;
    std::vector<int> cancel_leader_candidates_;
    std::vector<uint8_t> cancel_cert_bytes_;
    struct CommittedCancelInfo {
        uint64_t cancel_seq = 0;
        uint8_t  payload_digest[32] = {};
        std::vector<uint8_t> attestation_bytes;
        bool gossip_adopted = false;
    };
    std::map<uint32_t, CommittedCancelInfo> committed_cancels_;
    resdb_gossip::GossipAccumulator cancel_gossip_acc_;
    cMessage* cancel_gossip_timer_ = nullptr;
    int cancel_gossip_retry_count_ = 0;
    uint32_t cancel_gossip_epoch_ = 0;
    std::vector<uint8_t> cancel_gossip_bytes_;
    bool          cancel_pending_ = false;
    bool          rollback_cancel_initiated_ = false;
    uint32_t      cancelled_epoch_ = 0;
    uint32_t      rollback_new_epoch_ = 0;
    CancelReason  rollback_reason_ = CANCEL_CRASH;
    std::string   rollback_reason_ref_;
    std::vector<uint8_t> rollback_justification_;
    bool          rollback_local_recallable_ = false;
    bool          rollback_propose_submitted_ = false;
    int           rollback_expected_membership_size_ = 0;
    int           rollback_rotation_index_ = 0;
    std::map<std::string, std::vector<CancelEcho>> cancel_echoes_;
    std::set<std::string> cancel_echo_sent_;
    std::set<std::string> cancel_cert_seen_;
    std::set<std::string> cancel_cert_relayed_;
    CancelCert    cancel_cert_pending_retries_{};
    int           cancel_cert_retry_count_ = 0;
    ConsensusRetryManager consensus_retry_manager_;
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

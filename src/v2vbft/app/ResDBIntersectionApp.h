#pragma once

// Copyright (C) 2026 Mathesh Kumar
// SPDX-License-Identifier: GPL-3.0-or-later

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
#include "veins/modules/mobility/traci/IIntersectionApp.h"
#include "v2vbft/v2vbft.h"
#include "v2vbft/app/IV2VTransport.h"
#include "v2vbft/crypto/CryptoAuth.h"
#include "v2vbft/protocol/ConsensusContext.h"
#include "v2vbft/protocol/RollbackState.h"
#include "v2vbft/protocol/Primitives.h"
#include "v2vbft/protocol/ArrivalTypes.h"
#include "v2vbft/protocol/RollbackTypes.h"
#include "v2vbft/protocol/OrderCandidate.h"
#include "v2vbft/app/ResDBDecisionGossip.h"
#include "v2vbft/app/ResDBPropagationTracker.h"
#include "v2vbft/app/ResDBWitnessCert.h"
#include "integration/omnet/resdb_omnet_bridge.h"

class ChannelMetrics;

namespace veins {

class BFTMessage;

class V2VBFT_API ResDBIntersectionApp : public DemoBaseApplLayer,
                                        public IIntersectionApp {

public:
    ~ResDBIntersectionApp() override;
    // ── IIntersectionApp ─────────────────────────────────────────────────────
    // Called by TraCIScenarioManager, which knows only the interface.
    void recordIntersectionDeparture(simtime_t departedAt) override;
    /** Mute this replica after manager-side crash freeze (Scenario 16). */
    void disableCrashComms(const char* reason) override;

protected:
    void initialize(int stage) override;
    void handleSelfMsg(cMessage* msg) override;

    // ── Timer handlers ───────────────────────────────────────────────────────
    // One per self-message; dispatchTimer() routes to them via a table. These
    // were anonymous blocks in a 677-line if-chain. Each is the seam along
    // which a timer moves to the component that owns it.
    bool dispatchTimer(cMessage* msg);
    void onCrashMacGrace(cMessage* msg);
    void onChannelMetrics(cMessage* msg);
    void onSmokeTest(cMessage* msg);
    void onTransportPoll(cMessage* msg);
    void onTimeTick(cMessage* msg);
    void onConsensusRetry(cMessage* msg);
    void onConsensusRelay(cMessage* msg);
    void onCancelDrain(cMessage* msg);
    void onInitialAnnounce(cMessage* msg);
    void onBroadcastArrivalAnnouncement(cMessage* msg);
    void onCertRetry(cMessage* msg);
    void onDiscoveryTxFlush(cMessage* msg);
    void onCertGossip(cMessage* msg);
    void onGossip(cMessage* msg);
    void onCancelGossip(cMessage* msg);
    void onCancelCertRetry(cMessage* msg);
    void onClearCertRetry(cMessage* msg);
    void onClearCertCandidate(cMessage* msg);
    void onClearCertRelay(cMessage* msg);
    void onWaitLeaderSend(cMessage* msg);
    void onWaitFollowerExpiry(cMessage* msg);
    void onDiscoveryDeadline(cMessage* msg);
    void onDiscoverySettle(cMessage* msg);
    void onVcTrigger(cMessage* msg);
    void onCancelVc(cMessage* msg);
    void onStopSignTimeout(cMessage* msg);
    void onConsensusTimeout(cMessage* msg);
    void onResume(cMessage* msg);
    void onPrecedingBatchPoll(cMessage* msg);

    void handlePositionUpdate(cObject* obj) override;
    void finish() override;

    void onBSM(DemoSafetyMessage* bsm) override {}
    void onWSM(BaseFrame1609_4* wsm) override;
    void onWSA(DemoServiceAdvertisment* wsa) override {}

private:

   
    // Protocol types now live in protocol/ — see Primitives.h, ArrivalTypes.h,
    // RollbackTypes.h and OrderCandidate.h. They were private nested types here,
    // which meant no component could name a shared type without naming this class.

    // The state shared by four or more of this class's implementation files.
    // See protocol/ConsensusContext.h for how the membership was decided.
    ConsensusContext ctx_;

    // State owned solely by the cancel/rollback/clear protocol.
    RollbackState rollback_;


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

    struct PendingConsensusRelay {
        std::string key;
        std::vector<uint8_t> raw;
        ResdbPacketRequestInfo info{};
        std::string source;
        simtime_t fireTime;
        uint32_t relayEpoch = 0;
        int rank = -1;
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
                                       const char* source,
                                       uint32_t relayEpoch);
    int consensusRelayCarrierThreshold() const;
    int consensusRelayRank(const ResdbPacketRequestInfo& info) const;
    bool consensusRelayCarrierIsAuthenticated(
        int carrier, const uint8_t pubKey[CRYPTO_PUBKEY_BYTES]) const;
    void observeConsensusRelayCarrier(const std::string& key, int carrier,
                                      const ResdbPacketRequestInfo& info);
    void scheduleConsensusRelay(const std::string& key,
                                const uint8_t* data, uint32_t len,
                                const ResdbPacketRequestInfo& info,
                                const char* source,
                                uint32_t relayEpoch);
    void scheduleConsensusRelayFlush();
    void flushDueConsensusRelays();
    void cancelPendingConsensusRelays(const char* reason);
    bool consensusRelayPhaseComplete(
        const std::vector<uint8_t>& raw,
        const ResdbPacketRequestInfo& info) const;
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
    void broadcastArrivalAnnouncement(bool forceEmergency = false);
    void attachAmbulanceCryptoToAnnouncement(ArrivalAnnouncement& ann);
    void handleArrivalAnnouncement(BFTMessage* msg);
    void handleArrivalAnnouncement(BFTMessage* msg, bool viaGossip, int carrierReplicaId);
    void gossipArrivalAnnouncement(const ArrivalAnnouncement& ann,
                                   const std::vector<uint8_t>& announceBytes);
    void sendArrivalAnnouncementGossipPayload(const std::string& carId,
                                             uint32_t epoch,
                                             const std::vector<uint8_t>& announceBytes,
                                             const char* reason,
                                             bool forceEmergency = false);
    void handleArrivalAnnouncementGossip(BFTMessage* msg);
    void sendArrivalEcho(const ArrivalAnnouncement& ann);
    void collectArrivalEcho(const ArrivalEcho& echo, const char* source);
    bool isExactFalseLaneClaim(const ArrivalAnnouncement& ann) const;
    bool shouldColludeOnFalseLane(const ArrivalAnnouncement& ann) const;
    bool isArrivalSignerEligible(int replicaId) const;
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
    // interval(k) = min(baseSec * evidence_retry_factor_^attempt, capSec),
    // plus the existing broadcastJitterMin/Max jitter (spec §11.1).
    double backoffDelaySec(double baseSec, double capSec, int attempt) const;
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
    // True once rollback_.cancel_gossip_acc_ has seen this attestation gossiped by f+1
    // distinct peers — i.e. propagation is well underway independent of my
    // own broadcasts, so further retries add little and just cost channel
    // time. Not a witness-certificate concept (nothing is being assembled
    // into a cert here, just confirming an already-decided fact has spread),
    // so this stays local rather than living in ResDBWitnessCert.h.
    bool cancelGossipPropagationConfirmed() const;
    // Same idea for ordinary decision gossip (Type 9): true once gossip_acc_
    // has seen f+1 distinct peers gossip the same order_bytes for this epoch.
    bool decisionGossipPropagationConfirmed() const;
    // Same idea for my own CANCEL_CERT retry: true once f+1 distinct peers
    // have relayed/shown me the same cert (rollback_.cancel_cert_carriers_), tracked
    // separately from rollback_.cancel_cert_seen_/rollback_.cancel_cert_relayed_ (those are
    // dedup sets, not per-sender counts).
    bool cancelCertPropagationConfirmed(const std::string& key) const;
    void handleCancelCommitGossip(BFTMessage* bft);
    void broadcastCancelCommitAttestation(const ResdbCancelDecisionHdr& dh);
    void evaluateOrderReadiness(const char* reason);
    std::shared_ptr<const OrderCandidate> buildOrderCandidate() const;
    int currentOrderPrimary() const;
    void armOrderSuspicionTimer(const char* reason);
    void resetOrderCandidate(const char* reason);
    std::string cancelReasonKey(uint32_t epoch, CancelReason reason,
                                const std::string& reasonRef) const;
    std::string cancelSignPayload(uint32_t cancelledEpoch, CancelReason reason,
                                  const std::string& reasonRef,
                                  int echoingReplicaId) const;
    static std::string formatBlockedBatchRef(uint32_t cancelledEpoch, uint32_t executingBatch);
    static bool parseBlockedBatchRef(const std::string& ref, BlockedIncident& out);
    // CancelReason/CancelEcho <-> WitnessKind/WitnessEcho adapters for the shared
    // witness-cert machinery (ResDBWitnessCert.h). Members because CancelReason/
    // CancelEcho are private nested types.
    static WitnessKind toWitnessKind(CancelReason r);
    static WitnessEcho toWitnessEcho(const CancelEcho& e);
    static CancelEcho toCancelEcho(const WitnessEcho& we, uint32_t epoch, CancelReason reason,
                                    const std::string& reasonRef);
    // Registers the incident as BLOCKING independently of whether this cert
    // becomes the singleton CANCEL justification (spec §7.2 rule 6) — must run
    // even when CANCEL for the epoch is already pending/committed.
    void registerBlockedIncidentIfCrash(const CancelCert& cert);

    // ── Clear protocol (Types 15, 16) ─────────────────────────────────────────
    void sendClearEcho(uint32_t cancelledEpoch, uint32_t executingBatch);
    void handleClearEcho(BFTMessage* msg);
    void scheduleClearCertCandidate(const ClearCert& cert);
    void cancelClearCertCandidate(const char* reason);
    void broadcastClearCert(const ClearCert& cert);
    void scheduleClearCertRelay(const ClearCert& cert, const std::string& key);
    void cancelClearCertRelay(const char* reason);
    void sendClearCertCarrier(const ClearCert& cert, const char* marker);
    void handleClearCert(BFTMessage* msg);
    std::string clearSemanticKey(uint32_t cancelledEpoch, uint32_t executingBatch) const;
    std::vector<int> clearPropagationMembers() const;
    int clearPropagationRank() const;
    bool clearCarrierIsActiveMember(int replicaId) const;
    int clearPropagationThreshold() const;
    bool clearPropagationConfirmed(const std::string& key) const;
    bool validateClearCert(const ClearCert& cert) const;
    std::vector<uint8_t> serializeClearEcho(const ClearEcho& echo) const;
    ClearEcho deserializeClearEcho(BFTMessage* msg) const;
    std::vector<uint8_t> serializeClearCert(const ClearCert& cert) const;
    ClearCert deserializeClearCert(BFTMessage* msg) const;
    ClearCert deserializeClearCert(const uint8_t* data, uint32_t len) const;
    // Trampoline registered with the bridge as ResdbClearEvidenceFn (spec
    // §5.1) — runs on a ResDB worker thread during PBFT PreVerify.
    static int clearEvidenceCallback(void* ctx, uint32_t cancelledEpoch,
                                     const uint8_t* certBytes, uint32_t certLen);
    void scheduleNextClearCertRetry();
    void stopClearCertRetries();
    static WitnessEcho toWitnessEcho(const ClearEcho& e);
    static ClearEcho toClearEcho(const WitnessEcho& we, uint32_t cancelledEpoch, uint32_t executingBatch);
    // Transitions the incident BLOCKING->CLEARED (first-write-wins, mirrors
    // registerBlockedIncidentIfCrash) and, if that unblocks the epoch, kicks
    // the recovery proposer that was withheld pending this evidence.
    void onIncidentCleared(const BlockedIncident& incident, const std::vector<uint8_t>& clearCertBytes);
    // True iff at least one incident registered under cancelledEpoch is still
    // BLOCKING. False (not "unknown") when no incident was ever registered for
    // this epoch — an emergency (ambulance) rollback has no incident and must
    // never be gated by this check.
    bool hasBlockingIncidentForEpoch(uint32_t cancelledEpoch) const;

    // ── WAIT advisory heartbeat (Type 17) ─────────────────────────────────────
    // True iff discovery is COMPLETE for rollback_new_epoch_ and there is a
    // matching BLOCKING incident for cancelled_epoch_ — the condition under
    // which the ordinary cert primary should be sending WAIT and followers
    // should be willing to accept it. Writes the incident to *outIncident.
    bool waitConditionsHold(BlockedIncident* outIncident) const;
    // Re-evaluates waitConditionsHold()/CertPrimary() and either sends the
    // next heartbeat + reschedules wait_leader_send_timer_, or cancels it if
    // I'm no longer the expected leader or conditions no longer hold.
    void maybeSendWaitHeartbeat(const char* reason);
    void sendWaitHeartbeat(const BlockedIncident& incident);
    void handleWaitHeartbeat(BFTMessage* msg);
    // Cancels both WAIT timers and clears follower state. Idempotent.
    void stopWait(const char* reason);

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
    // Replica IDs of the static intersection units: [ctx_.total_vehicles_, num_replicas_).
    // Units are permanent PBFT members, so they are injected into every forced-view
    // membership (ORDER entries as QUIET, CANCEL electorate, rollback membership) so
    // they vote in the rollback path exactly like cars — but are never scheduled to cross.
    std::vector<int> staticUnitReplicaIds() const;
    bool isStaticUnitReplica(int replicaId) const;

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
    bool   vehicleInConflictBoxTraCI(const std::string& carId) const;
    bool   anyVehicleInConflictBoxTraCI() const;
    double vehicleSpeedTraCI(const std::string& carId) const;
    void   stopVehicle();
    void   resumeVehicle(int position_in_order);
    bool   isApproachingIntersection();
    bool checkIfDeparted();
    VerificationResult verifyCarPosition(const std::string& carId,
                                         const std::string& claimedLane,
                                         double claimedPosition, double tolerance);
    int    extractReplicaId(const std::string& carId) const;

    // ── State ─────────────────────────────────────────────────────────────────
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
    cMessage* clear_cert_retry_timer_  = nullptr;
    cMessage* clear_cert_candidate_timer_ = nullptr;
    cMessage* clear_cert_relay_timer_ = nullptr;
    cMessage* wait_leader_send_timer_       = nullptr;
    cMessage* wait_follower_expiry_timer_   = nullptr;
    cMessage* cancel_drain_timer_      = nullptr;
    cMessage* consensus_retry_timer_   = nullptr;
    cMessage* cancel_vc_timer_         = nullptr;
    int       pending_resume_position_ = 0;
    cMessage* vc_trigger_msg_          = nullptr;  // follower VC trigger after primary silence
    cMessage* channel_metrics_timer_   = nullptr;  // 100 ms CSV flush (channel + SINR)

    // Batch-aware clearance gating (mirrors V2VOrderProtocol.cc executeBatch logic).
    // my_batch_index_:        which batch this vehicle belongs to (0-based).
    // preceding_batch_cars_:  replica IDs of all vehicles in batch_index - 1.
    //                         All must clear the intersection before we resume.
    // Also carries the crash-dwell perception scan (see handleSelfMsg) since it
    // already ticks on a fixed period and already queries other vehicles via TraCI.
    cMessage*                preceding_batch_poll_msg_       = nullptr;
    int                      my_batch_index_           = -1;
    std::vector<int>         preceding_batch_cars_;     // replica IDs to wait on
    simtime_t                clearance_started_        = -1;
    double                   preceding_batch_poll_period_sec_ = 0.1;
    double                   clearance_timeout_sec_     = 8.0;

    // Scenario 16: manager freezes/tows; app only mutes when asked.
    bool                     crashCommsDisabled_       = false;
    cMessage*                crash_mac_grace_msg_      = nullptr;
    double                   crash_mac_grace_sec_      = 0.2;

    // Scenario 16: crash-dwell perception, scanned inside preceding_batch_poll_msg_
    // (see handleSelfMsg). Gated purely on ctx_.enableRollback_ — no separate flag.
    std::map<std::string, simtime_t> crash_dwell_since_;   // veh id -> first-qualified time
    std::set<std::string>            crash_echoed_targets_; // one echo per incident, local guard
    double                           crash_dwell_sec_     = 2.0;
    double                           crash_speed_eps_     = 0.1;

    // Scenario 16: CLEAR empty-box dwell, scanned in the same poll tick as
    // crash-dwell above. Keyed per incident (not per-vehicle) since the
    // clearance predicate is "no vehicle occupies the conflict box", not a
    // per-target check.
    std::map<BlockedIncident, simtime_t> clear_dwell_since_;
    std::set<BlockedIncident>            clear_echoed_incidents_; // one echo per incident, local guard
    double                               clear_dwell_sec_     = 1.0;
    double                               clear_cert_candidate_slot_sec_ = 0.1;

    // Scenario 16 WAIT (spec §8). Leader-side: only meaningful while I am
    // the expected CertPrimary(); follower-side: the last heartbeat I
    // accepted, if any.
    WaitHeartbeatState wait_follower_state_;
    double             wait_heartbeat_interval_sec_ = 1.0;
    double             wait_heartbeat_max_deferral_sec_ = 2.5;
    double             wait_clock_skew_sec_ = 0.0;

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

    uint32_t sequenceNumber_    = 0;
    bool     useRadioTransport_ = false;
    bool moduleIsAmbulance = false;
    bool ambulanceColorSet = false;
    int      configured_consensus_quorum_ = -1;
    int      configured_cert_threshold_ = -1;

    unsigned int sentMessages_ = 0;
    unsigned int receivedMessages_ = 0;
    uint64_t sentPayloadBytes_ = 0;
    uint64_t quietHonestVehicles_ = 0;
    uint64_t quietHonestOpportunities_ = 0;

    // Per-message-type send counters, keyed by BFTMessage messageType.
    // The totals above cannot say what a protocol layer costs, so the ablations
    // could only compare whole configurations. Split by type, the arrival-cert
    // layer (announce/echo/cert) can be priced against PBFT ordering (type 8)
    // from a single normal run, with no counterfactual build.
    std::map<int, unsigned int> sentMessagesByType_;
    std::map<int, uint64_t> sentBytesByType_;

    /** Record one outbound frame against both the totals and its type. */
    void recordSent(int msgType, size_t bytes)
    {
        sentMessages_++;
        sentPayloadBytes_ += bytes;
        sentMessagesByType_[msgType]++;
        sentBytesByType_[msgType] += bytes;
    }


    

  
    

    std::mutex outbound_mutex_;
    std::deque<PendingOutboundPacket> outbound_queue_;

    EVP_PKEY* ambulance_private_key_ = nullptr;
    std::vector<uint8_t> my_ambulance_cert_bytes_;

    std::string config_file_;
    std::string private_key_file_;
    std::string cert_file_;
    std::string log_dir_;

    // ── Cert protocol state ───────────────────────────────────────────────────
    // Guards ctx_.collected_certs_ against concurrent access from ResDB pre-verify threads.
    mutable std::mutex                              certs_mutex_;
    std::map<std::string, VehicleState>             local_vehicle_states_;
    std::map<std::string, std::vector<ArrivalEcho>> my_received_echoes_;
    std::set<std::string>                           observed_intent_cars_;
    std::set<std::string>                           arrival_announcements_received_;
    std::set<std::string>                           echoed_cars_;  // cars we actually sent an echo to (not FALSE_LANE)
    std::set<std::string>                           uncertified_ambulance_claimers_;
    std::set<std::string>                           cert_gate_rejected_ambulance_claimers_;

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
    static constexpr int kClearEchoType = 15;
    static constexpr int kClearCertType = 16;
    static constexpr int kWaitHeartbeatType = 17;

    resdb_gossip::GossipAccumulator  gossip_acc_;
    resdb_gossip::CertRelayTracker   cert_relay_tracker_;
    resdb_gossip::AnnouncementRelayTracker announcement_relay_tracker_;
    std::set<std::string> consensus_relay_seen_;
    std::map<std::string, std::set<int>> consensus_relay_carriers_;
    std::map<std::string, PendingConsensusRelay> pending_consensus_relays_;
    cMessage*            consensus_relay_timer_ = nullptr;
    std::vector<PendingDiscoveryTx> pending_discovery_txs_;
    cMessage*            discovery_tx_flush_timer_ = nullptr;
    std::map<std::pair<uint32_t, std::string>, resdb_gossip::PendingRelay> pending_relays_;
    cMessage*            gossip_timer_              = nullptr;
    int                  gossip_retry_count_        = 0;
    uint32_t             gossip_epoch_              = 0;
    std::vector<uint8_t> gossip_order_bytes_;
    std::vector<uint8_t> committed_order_bytes_;
    // Guards ctx_.committed_order_vehicle_ids_: written on the sim thread inside
    // processOrders(), but also read from a ResDB worker thread by the CLEAR
    // evidence-validation callback (invoked from PBFT PreVerify).
    mutable std::mutex   committed_view_mutex_;
    std::vector<std::vector<int>> committed_order_batches_;
    std::set<uint32_t>   tombstoned_epochs_;
    double               gossip_initial_interval_   = 0.5;
    int                  gossip_max_retries_        = 5;

    // ── TraCI lane state ──────────────────────────────────────────────────────
    bool        lane_discovered_ = false;
    bool        is_stopped_      = false;
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
    simtime_t stop_time_         = -1;
    simtime_t cleared_time_    = -1;
    simtime_t propose_time_      = -1;

    // ── Params ────────────────────────────────────────────────────────────────
    // PBFT membership size N = vehicles + static intersection units. Defaults to
    // ctx_.total_vehicles_ when no units are configured (totalReplicas = -1).
    int    num_replicas_          = 4;
    // Number of static intersection units (the top num_units_ replica IDs are units).
    int    num_units_             = 0;
    // When true, this module is a static intersection unit (RSU-hosted): quorum
    // participant + witness/echo + executor, but never announces/stops/crosses/proposes.
    bool   is_intersection_unit_  = false;
    double cruise_speed_mps_      = 14.0;
    bool   is_ambulance_          = false;
    bool          is_byzantine_          = false;
    ByzantineType byzantine_type_        = BYZANTINE_HONEST;
    bool          byzantine_pbft_silent_ = false;
    bool          enableAmbulanceCertGate_ = false;  // when true, rejects ambulance claims with no ambulanceCertBytes
    bool          enable_arrival_position_gate_ = true;
    std::set<int> false_lane_colluder_ids_;
    int           last_known_primary_ = 0;
    bool          bad_proposal_injected_ = false;
    int           fake_ambulance_proposal_replica_id_ = -1;
    int           tamper_lane_proposal_replica_id_ = -1;
    double        pbft_vc_timeout_sec_ = 3.0;
    double        cancel_cert_retry_interval_sec_ = 0.1;
    double        evidence_retry_base_sec_ = 0.1;
    double        evidence_retry_factor_ = 2.0;
    double        evidence_retry_cap_sec_ = 2.0;
    double        cancel_gossip_retry_base_sec_ = 0.25;
    double        cancel_gossip_retry_cap_sec_ = 4.0;
    int           cancel_cert_retry_max_ = 10;
    double        consensus_retry_interval_sec_ = 0.12;
    int           consensus_retry_max_ = 8;
    int           consensus_relay_carrier_cap_ = 2;
    double        consensus_relay_base_delay_sec_ = 0.02;
    double        consensus_relay_slot_sec_ = 0.02;
    double        braking_decel_mps2_ = 4.5;
    double        processing_latency_margin_ = 2.0;
    double        rollback_vc_timeout_sec_ = 3.0;
    bool          inject_suppress_initial_cancel_leader_ = false;
    bool          enable_cancel_leader_failover_ = true;
    bool          inject_fabricated_clearance_leader_ = false;
    bool          enable_recovery_clear_evidence_gate_ = true;
    bool          fabricated_clearance_attack_active_ = false;
    bool          rollback_fault_mode_per_epoch_ = true;
    bool          cancel_consensus_pending_ = false;
    bool          cancel_propose_submitted_ = false;
    CancelState   cancel_state_ = CancelState::INACTIVE;
    int           cancel_active_batch_ = -1;
    int           cancel_primary_ = -1;
    std::vector<int> cancel_leader_candidates_;
    std::vector<uint8_t> cancel_cert_bytes_;
    cMessage* cancel_gossip_timer_ = nullptr;
    int cancel_gossip_retry_count_ = 0;
    uint32_t cancel_gossip_epoch_ = 0;
    std::vector<uint8_t> cancel_gossip_bytes_;
    bool          rollback_cancel_initiated_ = false;
    uint32_t      cancelled_epoch_ = 0;
    uint32_t      rollback_new_epoch_ = 0;
    bool          rollback_local_recallable_ = false;
    int           cancel_rotation_index_ = 0;
    std::shared_ptr<const OrderCandidate> order_candidate_;
    bool          order_vc_requested_ = false;
    bool          order_vc_authoritative_ = false;
    std::map<BlockedIncident, IncidentRecord> incidentRegistry_;
    CancelCert    cancel_cert_pending_retries_{};
    int           cancel_cert_retry_count_ = 0;
    resdb_propagation::AuthenticatedPropagationTracker clear_propagation_tracker_;
    ClearCert     clear_cert_candidate_{};
    ClearCert     clear_cert_pending_relay_{};
    std::string   clear_cert_pending_relay_key_;
    ClearCert     clear_cert_pending_retries_{};
    int           clear_cert_retry_count_ = 0;
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

//
// V2VProxyModule.h
// OMNeT++ module that bridges BFT-SMaRt Java processes to Veins V2V communication
//

#pragma once

#include "veins/veins.h"
#include "veins/modules/application/ieee80211p/DemoBaseApplLayer.h"
#include "veins/modules/bftsmart/bftsmart_demo_intersection_IntersectionServer.h"
#include "veins/modules/bftsmart/BFTMessage_m.h"
#include <jni.h>
#include <map>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
namespace veins {



class VEINS_API V2VProxyModule : public DemoBaseApplLayer {
public:
    V2VProxyModule();
    ~V2VProxyModule() override;
    simtime_t consensusStartTime;
    
    // Separate timing for View and Order consensus
    simtime_t viewConsensusStartTime;
    simtime_t viewConsensusEndTime;
    simtime_t orderConsensusStartTime;
    simtime_t orderConsensusEndTime;
    simtime_t orderCollectionWindowStart;
    simtime_t orderCollectionWindowEnd;
    // OMNeT++ lifecycle
    void initialize(int stage) override;
    void finish() override;
    std::queue<double> pendingResumeDelays;

    // JNI Bridge Interface - called from Java via JNI
    static V2VProxyModule* getProxyForReplica(int replicaId);
    bool sendMessageToReplica(int fromReplicaId, int toReplicaId, const uint8_t* data, int dataLen);
    void registerJavaCallback(JNIEnv* env, jobject javaObject);
    bool isRadioBusy() const { return radioBusy; }  // For reactive yield pattern
    bool isDeparted = false;
    bool checkIfDeparted();
    void notifyJavaDeparted();
    bool zombieFilter();

    void resumeVehicle(double delaySeconds = 0.0);  // Resume vehicle movement after assigned delay
    bool alreadyAtStopLine = false;
    bool isWaitingForClearance = true;
    void handlePositionUpdate(cObject* obj) override;
    void handleOrderDecision(const std::string& orderDecision);
    void parseAndNotifyDecision(const std::string& decision);
    void flushReliabilityQueue();
    void onViewAgreed(const std::set<std::string>& agreedView);
    void resetForNextRound();
    bool pendingReconfigFlush = false;
    void scheduleReconfigFlush();

    // Pending ORDER decision queued by JNI thread for safe processing on main thread.
    // cancelEvent() and parseAndNotifyDecision() must never be called from a JNI thread
    // because OMNeT++'s FES (std::map) is not thread-safe; concurrent inserts/removes
    // from multiple JNI threads cause the std::_Rb_tree_insert_and_rebalance SIGSEGV.
    std::string pendingOrderDecision;
    bool pendingCancelOrderTimer = false;
    

protected:
    // Message handling
    void onBSM(DemoSafetyMessage* bsm) override;
    void onWSM(BaseFrame1609_4* wsm) override;
    void onWSA(DemoServiceAdvertisment* wsa) override;
    

    std::chrono::time_point<std::chrono::high_resolution_clock> realViewConsensusStart;
    std::chrono::time_point<std::chrono::high_resolution_clock> realOrderConsensusStart;
    std::chrono::time_point<std::chrono::high_resolution_clock> realViewConsensusEnd;
    std::chrono::time_point<std::chrono::high_resolution_clock> realOrderConsensusEnd;
    bool orderDecisionCallbackSeen = false;
    double lastOrderBftRequestRttMs = -1.0;


    void handleSelfMsg(cMessage* msg) override;
    void handleLowerMsg(cMessage* msg) override;
    std::set<std::string> proposeView();
    bool javaReady = false;
    cMessage* checkJavaReadyTimer = nullptr;
    bool checkJavaReplicaStatus();
    cMessage* retxCheckTimer;

    void triggerRetransmissionCheckViaJNI();

    // Time synchronization
    void syncTimeToJava();

    // BFT Message handling
    void sendBFTMessage(int fromReplicaId, int toReplicaId, const std::vector<uint8_t>& data, int messageType);

    // Java callback - delivers message to Java
    void deliverMessageToJava(int fromReplicaId, const uint8_t* data, int dataLen);

    struct WitnessSignature {
        int witnessReplicaId;
        std::vector<uint8_t> signature;  // Mock: hash of (carId, laneId, pos, time, epoch)
        double witnessTimestamp;
    };

    struct ReadyQC {
        std::string carId;           // "veh0", "veh1", etc.
        std::string laneId;          // TraCI lane ID (e.g., ":J0_1_0")
        double positionInLane;       // Meters from lane start
        double verifiedArrival;      // Earliest witness timestamp
        int epoch;                   // Prevents replay attacks
        std::vector<WitnessSignature> signatures;  // f+1 signatures
   
        bool isValid(int f) const { return signatures.size() >= f + 1; }
    };
   
    
   
    struct ArrivalAnnouncement {
        std::string carId;
        std::string laneId;
        double positionInLane;
        double claimedArrivalTime;
        int epoch;
        std::vector<uint8_t> signature;  // Self-signed
    };
   
    struct WitnessResponse {
        std::string targetCarId;
        int witnessReplicaId;
        bool verified;               // Did TraCI confirm?
        std::vector<uint8_t> signature;
        double witnessTimestamp;
    };

    // View Consensus structures (NEW - Phase 1)
    struct ViewProposal {
        int proposerReplicaId;
        std::set<std::string> observedCars;  // Cars I can see via TraCI
        double proposalTimestamp;
        std::vector<uint8_t> signature;  // Self-signed proposal
    };

    struct ViewAgreement {
        int agreingReplicaId;
        std::set<std::string> agreedView;  // The view I'm signing
        std::vector<uint8_t> signature;     // Signature over hash(view set)
    };

    // ReadyQC Management (Phase 2 - after view is established)
    std::map<std::string, ReadyQC> verifiedPool;  // Completed ReadyQCs
    std::map<std::string, ReadyQC> nextEpochPool; // Buffer for epoch+1 QCs received before local epoch increments
    std::map<std::string, ArrivalAnnouncement> pendingAnnouncements;
    std::map<std::string, std::vector<WitnessResponse>> collectedWitnesses;
    /** Track which cars' arrival announcements this replica has received (for debugging drop detection) */
    std::set<std::string> arrivalAnnouncementsReceived;
    std::set<int> readyQCAcks;  // Track which replicas have ACKed our ReadyQC
    double readyQCTimeoutSec = 10.0;  // Longer timeout for testing
    int currentEpoch = 0;
    bool hasProposedOrder = false;


    // View consensus state
    ViewProposal myViewProposal;  // My proposed view
    std::map<std::set<std::string>, std::vector<ViewAgreement>> viewVotes;  // view -> signatures
    std::set<std::string> establishedView;  // View that got f+1 signatures
    bool viewEstablished = false;

    // Neighbor tracking
    std::set<int> neighborsInRange;
    double witnessRange = 150.0;

    // Consensus phases (CORRECTED ORDER)
    enum ConsensusPhase {
        IDLE,
        PROPOSING_VIEW,     // Phase 1a: Each car proposes who they can see
        VIEW_AGREEMENT,     // Phase 1b: Collecting f+1 V2V agreement signatures
        VIEW_CONSENSUS,     // Phase 1c: Waiting for BFT consensus on view
        COLLECTING_QC,      // Phase 2: Collecting ReadyQCs (after view established)
        ORDER_CONSENSUS,    // Phase 3: BFT agreeing on traversal order
        WAITING_FOR_CLEARANCE, // Phase 4: Waiting for clearance from intersection controller
        PULLING_FORWARD,      // Phase 5: Pulling forward to stop line
        EXECUTING,           // Cars crossing intersection

        DEPARTED,            //NEW: Car has crossed intersection (zombie mode)
    };
    ConsensusPhase currentPhase = IDLE;
    std::set<std::string> agreedView;
    std::vector<std::string> agreedOrder;

    // Timers
    cMessage* readyQCTimeoutTimer = nullptr;
    cMessage* witnessGossipTimer = nullptr;
    cMessage* viewConsensusTimer = nullptr;
    

private:
    // Configuration
    int replicaId;
    int serviceChannel;

    std::string myLaneId;
    std::vector<std::string> laneQueue;  // ordered front-to-back by lane position
    std::string carAhead;                // car directly in front of me in my lane
    double carAheadStopPos = -1.0;       // their last known lane position before departing
    bool laneDiscovered = false;
    std::string myLaneTriggerCar;
    void discoverLane();
    
    // Statistics
    simsignal_t bftMsgSentSignal;
    simsignal_t bftMsgReceivedSignal;

    int getMaxMessagesPerTick();
    
    // Message counters
    unsigned int sentMessages;
    unsigned int receivedMessages;
    unsigned int sequenceNumber;
    bool shouldFlush;
    bool warmupJVM(JNIEnv* env);
    simtime_t stopTime = -1;          // When the car physically stopped
    simtime_t clearanceTimeout;
    int bftMessagesReceived = 0;      // Counter for received messages
    
    // JNI integration
    JavaVM* jvm;
    jobject javaCallbackObject;  // Global reference to Java V2VNativeBridge instance
    jmethodID deliverMessageMethod;
    std::mutex jniMutex;

    // BFTSmart replica thread
    jobject bftReplicaThread;  // Java thread running IntersectionServer

    // JVM management helpers
    bool createOrAttachJVM();
    bool registerJNINativeMethods(JNIEnv* env);
    void startBFTSmartReplica();
    void stopBFTSmartReplica();

    std::set<std::string> expectedToGo;      // Cars that should cross this round (from ORDER decision)
    std::set<std::string> confirmedDeparted; // Cars we've confirmed as departed
    simtime_t clearanceStartTime = 0;        // When we started waiting for clearance
    double CLEARANCE_TIMEOUT = 10.0;           // Max wait for clearance after ORDER delivery (seconds)
    simtime_t viewSignatureCollectionStartTime = 0;  
    simtime_t viewSignatureCollectionEndTime = 0;  
    simtime_t orderSignatureCollectionStartTime = 0;  
    simtime_t orderSignatureCollectionEndTime = 0; 
    
    bool myReadyQCComplete = false;          // I have f+1 witnesses and built my own QC
    bool orderCollectionActive = false;      // In gossip/collect window
    bool orderBagProposed = false;           // I have sent at least one ORDER bag this round
    bool orderDecisionReceived = false;      // stop retries when Java notifies decision
    bool orderBagCloseFlag = false;          // close flag used when bag was first proposed
    bool delayedOrderSubmitScheduled = false; // true while a delayed ORDER submit is pending

    simtime_t orderCollectionDeadline;       // e.g. simTime() + 0.300
    int orderBagRetransmitCount = 0;
    double orderDelayGap = 0.0;              // Optional delay between VIEW complete and ORDER submit (seconds)
    std::string pendingOrderPayload;         // Serialized ORDER payload awaiting delayed submit
    int pendingOrderEpoch = -1;              // Epoch associated with pendingOrderPayload
    int pendingOrderViewHash = 0;            // View hash associated with pendingOrderPayload
    simtime_t firstOrderBagProposalTime = 0; // First ORDER_PROPOSE timestamp seen locally this epoch
    int firstOrderBagProposerReplica = -1;   // Which replica produced the first local bag this epoch
    simtime_t lastRoundResetTime = -1;       // When resetForNextRound advanced us into currentEpoch
    int lastRoundResetEpoch = -1;            // Epoch index entered at last resetForNextRound
    cMessage* orderCollectDeadlineTimer;
    cMessage* orderGossipRetransmitTimer; //optional
    cMessage* orderBagRetransmitTimer;
    cMessage* orderDelayTimer;
    void startOrderCollectionWindowIfNeeded();
    int countDistinctFrontLanesInPool();
    std::vector<ReadyQC> buildOrderBagQCs();
    std::string serializeOrderBagRequest(const std::vector<ReadyQC>& bag, bool closeFlag);
    std::string serializeReadyQCForJava(const ReadyQC& qc);
    void proposeOrderBagNow(const std::string& reason);
    bool isCarAtFrontOfLane(const std::string& carId, const std::string& laneId);
    bool isMyQCFrontMostKnownInLaneFromPool();
    // Static registry for JNI lookup
    static std::map<int, V2VProxyModule*> replicaProxyMap;
    static std::mutex registryMutex;
    static JavaVM* sharedJVM; 
    static std::mutex jvmMutex; 
    // Message queue (for handling async messages from Java)
    struct PendingMessage {
        int fromReplicaId;
        int toReplicaId;
        std::vector<uint8_t> data;
    };
    std::queue<PendingMessage> messageQueue;
    std::condition_variable queueCondVar;  // For blocking Java when queue full
    static const size_t MAX_QUEUE_SIZE = 1000;  // Small queue to keep Java in sync
    
    // Radio busy tracking for reactive yield pattern
    bool radioBusy;
    cMessage* radioReadyMsg;  // Fires when transmission completes
    jmethodID onRadioReadyMethod;  // Java callback
    jclass clockClass;
    jmethodID updateTimeMethod;
    cMessage* processQueueTimer;
    cMessage* startBFTTimer;  // Timer to start BFT after initialization
    cMessage* triggerJoinTimer;  // Timer to trigger JOIN when car reaches intersection
    
    // Trigger JOIN via JNI (now takes request string - declaration moved to bottom)
    bool joinTriggered;  // Track if we've already triggered


    // Reactive yield - notify Java when radio is ready
    void notifyJavaRadioReady();

    void notifyJavaNewBatchSize(int newBatchSize); 

    // Intersection management
    double intersectionX;
    double intersectionY;
    double stopDistance;

    // Intersection physics for delay calculation
    double intersectionWidth;
    double avgSpeed;
    double safetyGap;
    int MAX_MESSAGES_PER_TICK;
    bool waitingForConsensus;
    bool hasRequestedCrossing;
    bool isStopped;
    cMessage* checkPositionTimer;
    cMessage* consensusTimeoutTimer;  // Timer for consensus timeout fallback
    std::atomic<bool> phase2Pending{false}; // Use atomic for thread safety
    cMessage* startReadyQCCollectionMsg;
    double consensusTimeoutSec;       // Configurable timeout duration

    double getDistanceToIntersection();
    bool isApproachingIntersection();
    void stopVehicle();

    int getCurrentViewLeader(const std::set<std::string>& agreedView);
    bool amITheLeader(const std::set<std::string>& agreedView);

    // TraCI-based verification for witness/ReadyQC (car present, lane, position)
    struct VerificationResult {
        bool isValid;
        std::string reason;
    };
    VerificationResult verifyCarPosition(const std::string& carId,
        const std::string& claimedLane,
        double claimedPosition,
        double tolerance = 5.0);

    bool verifyNoLeaderInLane(const std::string& carId, const std::string& laneId);


    std::vector<uint8_t> signArrivalClaim(const ArrivalAnnouncement& announcement);
    std::vector<uint8_t> signWitnessClaim(const ArrivalAnnouncement& ann, double witnessTime, int witnessId);

    // Serialization functions
    std::vector<uint8_t> serializeViewProposal(const ViewProposal& proposal);
    ViewProposal deserializeViewProposal(BFTMessage* bftMsg);
    std::vector<uint8_t> serializeViewAgreement(const ViewAgreement& agreement);
    ViewAgreement deserializeViewAgreement(BFTMessage* bftMsg);
    
    std::vector<uint8_t> serializeArrivalAnnouncement(const ArrivalAnnouncement& ann);
    V2VProxyModule::ArrivalAnnouncement deserializeArrivalAnnouncement(BFTMessage* bftMsg);
    std::vector<uint8_t> serializeWitnessResponse(const WitnessResponse& witness);
    V2VProxyModule::WitnessResponse deserializeWitnessResponse(BFTMessage* bftMsg);
    std::vector<uint8_t> serializeReadyQC(const ReadyQC& qc);
    V2VProxyModule::ReadyQC deserializeReadyQC(BFTMessage* bftMsg);
    std::string serializeReadyQCToString(const ReadyQC& qc);

    // Message handlers
    void handleViewProposal(BFTMessage* bftMsg);      // NEW: Phase 1
    void handleViewAgreement(BFTMessage* bftMsg);     // NEW: Phase 1
    void handleArrivalAnnouncement(BFTMessage* bftMsg);  // Phase 2
    void handleWitnessResponse(BFTMessage* bftMsg);      // Phase 2
    void handleReadyQCComplete(BFTMessage* bftMsg);      // Phase 2
    void handleReadyQCAck(BFTMessage* bftMsg);           // Phase 2
    void assembleAndBroadcastReadyQC();
    void handleBFTMessage(BFTMessage* bftMsg);
    void handlepreConsensusMessages(BFTMessage* bftMsg);

    // Three-phase consensus (CORRECTED)
    void initiateViewProposal();          // Phase 1a: Detect visible cars
    void broadcastViewProposal();         // Phase 1b: Collect V2V agreements
    void processViewAgreements();         // Phase 1b: Check if we have f+1 V2V agreement
    void submitViewToBFTConsensus(const std::set<std::string>& view, 
                                   const std::vector<ViewAgreement>& v2vSigs);  // Phase 1c: BFT consensus
    void startReadyQCCollection();        // Phase 2: Begin after view established
    void broadcastArrivalAnnouncement();  // Phase 2: Announce arrival for witnessing
    void triggerOrderConsensus();         // Phase 3: BFT on order
    
    bool triggerJoinViaJNI(const std::string& request);
    bool triggerGlobalResetViaJNI(const std::vector<int>& departedReplicas);
    int extractReplicaIdFromCarId(const std::string& carId);
    
    // View detection (uses TraCI sensors)
    std::set<std::string> getVisibleVehicles(double maxRange);
    std::vector<uint8_t> signViewProposal(const std::set<std::string>& viewSet);
    


};

} // namespace veins








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

namespace veins {

class VEINS_API V2VProxyModule : public DemoBaseApplLayer {
public:
    V2VProxyModule();
    ~V2VProxyModule() override;
    simtime_t consensusStartTime;

    // OMNeT++ lifecycle
    void initialize(int stage) override;
    void finish() override;
    std::queue<double> pendingResumeDelays;

    // JNI Bridge Interface - called from Java via JNI
    static V2VProxyModule* getProxyForReplica(int replicaId);
    bool sendMessageToReplica(int fromReplicaId, int toReplicaId, const uint8_t* data, int dataLen);
    void registerJavaCallback(JNIEnv* env, jobject javaObject);
    bool isRadioBusy() const { return radioBusy; }  // For reactive yield pattern

    void resumeVehicle(double delaySeconds = 0.0);  // Resume vehicle movement after assigned delay
    void handlePositionUpdate(cObject* obj) override;
    void flushReliabilityQueue();
protected:
    // Message handling
    void onBSM(DemoSafetyMessage* bsm) override;
    void onWSM(BaseFrame1609_4* wsm) override;
    void onWSA(DemoServiceAdvertisment* wsa) override;

    void handleSelfMsg(cMessage* msg) override;
    void handleLowerMsg(cMessage* msg) override;

    // Time synchronization
    void syncTimeToJava();

    // BFT Message handling
    void handleBFTMessage(BFTMessage* bftMsg);
    void sendBFTMessage(int fromReplicaId, int toReplicaId, const std::vector<uint8_t>& data);

    // Java callback - delivers message to Java
    void deliverMessageToJava(int fromReplicaId, const uint8_t* data, int dataLen);
    

private:
    // Configuration
    int replicaId;
    int serviceChannel;
    
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
    
    // Trigger JOIN via JNI
    bool triggerJoinViaJNI();
    bool joinTriggered;  // Track if we've already triggered

    // Reactive yield - notify Java when radio is ready
    void notifyJavaRadioReady();

    // Intersection management
    double intersectionX;
    double intersectionY;
    double stopDistance;

    // Intersection physics for delay calculation
    double intersectionWidth;
    double avgSpeed;
    double safetyGap;
    const int MAX_MESSAGES_PER_TICK;
    bool waitingForConsensus;
    bool hasRequestedCrossing;
    bool isStopped;
    cMessage* checkPositionTimer;
    cMessage* consensusTimeoutTimer;  // Timer for consensus timeout fallback
    double consensusTimeoutSec;       // Configurable timeout duration

    double getDistanceToIntersection();
    bool isApproachingIntersection();
    void stopVehicle();
   
};

} // namespace veins








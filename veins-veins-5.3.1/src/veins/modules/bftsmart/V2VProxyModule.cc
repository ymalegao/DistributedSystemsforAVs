//
// V2VProxyModule.cc
// Implementation of V2V Proxy Module for BFT-SMaRt
//

#include "veins/modules/bftsmart/V2VProxyModule.h"
#include "veins/base/utils/SimpleAddress.h"
#include "veins/modules/utility/Consts80211p.h"
#include <algorithm>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <thread>

#define XXH_INLINE_ALL
#include "xxhash.h"

using namespace veins;

static int completedConsensusCount = 0;
static const int BATCH_SIZE = 12;
static bool logged100m = false;
// Base64 encoding table

void V2VProxyModule::zombieFilter() {
    if (isDeparted) {
        std::cout << "[V2VProxy " << replicaId << "] ZOMBIE: Not executing action (departed)" << std::endl;
        return;
    }
}
Define_Module(veins::V2VProxyModule);

// Static member initialization
std::map<int, V2VProxyModule*> V2VProxyModule::replicaProxyMap;
std::mutex V2VProxyModule::registryMutex;
JavaVM* V2VProxyModule::sharedJVM = nullptr;
std::mutex V2VProxyModule::jvmMutex;


// XXHash32 wrapper for deterministic hashing across C++ and Java
int32_t computeXXHash32(const std::string& str) {
    // Use seed = 0 to match Java: xxhash.hash(dataBytes, 0, dataBytes.length, 0)
    return XXH32(str.c_str(), str.length(), 0);
}


cMessage* resumeMsg = nullptr;


V2VProxyModule::V2VProxyModule()
    : DemoBaseApplLayer()
    , replicaId(-1)
    , serviceChannel(2)
    , sentMessages(0)
    , receivedMessages(0)
    , sequenceNumber(0)
    , jvm(nullptr)
    , javaReady(false)
    , checkJavaReadyTimer(nullptr)
    , retxCheckTimer(nullptr)
    , javaCallbackObject(nullptr)
    , deliverMessageMethod(nullptr)
    , bftReplicaThread(nullptr)
    , radioBusy(false)
    , radioReadyMsg(nullptr)
    , onRadioReadyMethod(nullptr)
    , clockClass(nullptr)
    , updateTimeMethod(nullptr)
    , processQueueTimer(nullptr)
    , triggerJoinTimer(nullptr)
    , joinTriggered(false)
    , intersectionX(0.0)
    , intersectionY(0.0)
    , stopDistance(10.0)
    , intersectionWidth(25.0)
    , avgSpeed(10.0)
    , safetyGap(2.0)
    , waitingForConsensus(false)
    , hasRequestedCrossing(false)
    , isStopped(false)
    , checkPositionTimer(nullptr)
    , consensusTimeoutTimer(nullptr)
    , shouldFlush(false)
    , consensusTimeoutSec(80.0)  // Default 40 seconds
    , MAX_MESSAGES_PER_TICK(getMaxMessagesPerTick())
    , startReadyQCCollectionMsg(nullptr)
    , consensusStartTime(0)
    , viewConsensusStartTime(0)
    , viewConsensusEndTime(0)
    , orderConsensusStartTime(0)
    , orderConsensusEndTime(0)
    , orderCollectDeadlineTimer(nullptr)
    , orderGossipRetransmitTimer(nullptr)
    , orderBagRetransmitTimer(nullptr)
    , orderCollectionActive(false)
    , orderBagProposed(false)
    , orderDecisionReceived(false)
    , orderCollectionDeadline(0)
    , orderBagRetransmitCount(0)
    , myReadyQCComplete(false)
    , orderBagCloseFlag(false)
{

}

V2VProxyModule::~V2VProxyModule()
{
    // Cancel and delete timers
    cancelAndDelete(processQueueTimer);
    cancelAndDelete(triggerJoinTimer);
    cancelAndDelete(radioReadyMsg);
    cancelAndDelete(checkPositionTimer);
    cancelAndDelete(consensusTimeoutTimer);
    cancelAndDelete(checkJavaReadyTimer);
    cancelAndDelete(startReadyQCCollectionMsg);
    cancelAndDelete(retxCheckTimer);
    cancelAndDelete(orderCollectDeadlineTimer);
    cancelAndDelete(orderGossipRetransmitTimer);
    cancelAndDelete(orderBagRetransmitTimer);
    // Clean up JNI global reference
    if (javaCallbackObject && jvm) {
        JNIEnv* env;
        jvm->AttachCurrentThread((void**)&env, nullptr);
        env->DeleteGlobalRef(javaCallbackObject);
        javaCallbackObject = nullptr;
    }

    // Unregister from static map
    {
        std::lock_guard<std::mutex> lock(registryMutex);
        replicaProxyMap.erase(replicaId);
    }
}

void V2VProxyModule::initialize(int stage)
{
    DemoBaseApplLayer::initialize(stage);

    if (stage == 0) {
        // Get parameters
        std::string sumoId = mobility->getExternalId(); 
        
        // Dynamically strip the "veh" prefix and become that number
        if (sumoId.find("veh") != std::string::npos) {
            std::string idStr = sumoId.substr(3); 
            try {
                replicaId = std::stoi(idStr);
                std::cout << "[IDENTITY BINDING] I am physically " << sumoId 
                          << ". Officially setting my replicaId to " << replicaId << "!" << std::endl;
            } catch (...) {
                std::cerr << "[ERROR] Could not parse replica ID from " << sumoId << std::endl;
                replicaId = -1;
            }
        } else {
            replicaId = -1; // Catches RSUs or pedestrians
        }

        if (replicaId < 0) {
            std::cerr << "[ERROR V2VProxy] Invalid replicaId (" << sumoId << "), skipping initialization." << std::endl;
            return;
        }

        serviceChannel = par("serviceChannel");

        // Get intersection parameters
        intersectionX = par("intersectionX").doubleValue();
        intersectionY = par("intersectionY").doubleValue();
        stopDistance = par("stopDistance").doubleValue();
        intersectionWidth = par("intersectionWidth").doubleValue();
        avgSpeed = par("avgSpeed").doubleValue();
        safetyGap = par("safetyGap").doubleValue();
        consensusTimeoutSec = par("consensusTimeoutSec").doubleValue();

        // Initialize signals
        bftMsgSentSignal = registerSignal("bftMsgSent");
        bftMsgReceivedSignal = registerSignal("bftMsgReceived");

        // Create timer for processing queued messages
        processQueueTimer = new cMessage("processQueue");

        // Create message for starting ready QC collection
        startReadyQCCollectionMsg = new cMessage("startReadyQCCollectionMsg");

        // Create message for radio ready notification (reactive yield pattern)
        radioReadyMsg = new cMessage("radioReady");

        // Create timer for checking position
        checkPositionTimer = new cMessage("checkPosition");

        // Create timer for consensus timeout fallback
        consensusTimeoutTimer = new cMessage("consensusTimeout");

        // Create timers for ORDER bag gossip/collection phase
        orderCollectDeadlineTimer = new cMessage("orderCollectDeadline");
        orderGossipRetransmitTimer = new cMessage("orderGossipRetransmit");
        orderBagRetransmitTimer = new cMessage("orderBagRetransmit");

        // Create timer for checking Java readiness
        checkJavaReadyTimer = new cMessage("checkJavaReady");
        retxCheckTimer = new cMessage("retransmissionCheck");
        
        
        

        scheduleAt(simTime() + 0.1, retxCheckTimer);
        scheduleAt(simTime() + 0.5, checkJavaReadyTimer); // Start checking after 0.5s

        EV_INFO << "V2VProxyModule initialized for replica " << replicaId << std::endl;
        std::cout << "[V2VProxy " << replicaId << "] Intersection at (" << intersectionX << ", " << intersectionY << "), stop distance=" << stopDistance << "m" << std::endl;

        // Register in global map for JNI lookup
        {
            std::lock_guard<std::mutex> lock(registryMutex);
            replicaProxyMap[replicaId] = this;
        }
        
        std::cout << "[DEBUG V2VProxy " << replicaId << "] Replica registered in proxy map" << std::endl;
        
        // Start periodic timer to poll message queue (since Java threads can't call scheduleAt)
        scheduleAt(simTime() + 0.1, processQueueTimer);  // Start polling after 100ms
        std::cout << "[DEBUG V2VProxy " << replicaId << "] Periodic queue timer started" << std::endl;
        std::cout << "[DEBUG V2VProxy " << replicaId << "] Timer scheduled for t=" << (simTime() + 0.1) 
                  << ", isScheduled=" << (processQueueTimer->isScheduled() ? "YES" : "NO") << std::endl;
        
        // Create or attach to JVM
        if (createOrAttachJVM()) {
            std::cout << "[DEBUG V2VProxy " << replicaId << "] JVM ready" << std::endl;
            
            // Start BFTSmart replica in Java
            startBFTSmartReplica();
        } else {
            std::cerr << "[ERROR V2VProxy " << replicaId << "] Failed to initialize JVM" << std::endl;
        }
        
        // Start position checking to trigger consensus when approaching intersection
        // With StateManager bypass, replicas initialize in ~1-2s, so start checking early
        scheduleAt(simTime() + 0.5, checkPositionTimer);
        std::cout << "[DEBUG V2VProxy " << replicaId << "] Position checking will start at t=0.5s" << std::endl;
        //collectedWitnesses.clear();
        //
        //collectedWitnesses[myCarId].push_back(WitnessSignature());
    }
}

void V2VProxyModule::finish()
{
    DemoBaseApplLayer::finish();
    
    // Stop BFTSmart replica
    stopBFTSmartReplica();
    
    EV_INFO << "V2VProxyModule replica " << replicaId << " statistics:" << std::endl;
    EV_INFO << "  Messages sent: " << sentMessages << std::endl;
    EV_INFO << "  Messages received: " << receivedMessages << std::endl;
    std::cout << "[METRICS " << replicaId << "] Messages received: " << receivedMessages << std::endl;
    std::cout << "[METRICS " << replicaId << "] Messages sent: " << sentMessages << std::endl;
    

    std::cout << "[DEBUG V2VProxy " << replicaId << "] Finishing V2VProxyModule" << std::endl;
    // Cancel timer
    if (processQueueTimer) {
        cancelAndDelete(processQueueTimer);
        processQueueTimer = nullptr;
    }
}

// Static method for JNI to find the right proxy instance
V2VProxyModule* V2VProxyModule::getProxyForReplica(int replicaId)
{
    std::lock_guard<std::mutex> lock(registryMutex);
    auto it = replicaProxyMap.find(replicaId);
    if (it != replicaProxyMap.end()) {
        return it->second;
    }
    return nullptr;
}

// Called from JNI when Java wants to send a message
bool V2VProxyModule::sendMessageToReplica(int fromReplicaId, int toReplicaId, const uint8_t* data, int dataLen)
{
    std::cout << "[V2V-SEND] Replica " << replicaId << ": Java->C++ sendMessageToReplica("
              << fromReplicaId << "->" << toReplicaId << ", " << dataLen << " bytes) at t=" << simTime() << std::endl;
    
    // Create message
    PendingMessage pendingMsg;
    pendingMsg.fromReplicaId = fromReplicaId;
    pendingMsg.toReplicaId = toReplicaId;
    pendingMsg.data.assign(data, data + dataLen);
    
    // THROTTLE: Wait if queue is full (synchronizes Java with simulation time)
    {
        std::unique_lock<std::mutex> lock(jniMutex);

        if (messageQueue.size() >= MAX_QUEUE_SIZE) {
            std::cout << "[V2V-SEND] Replica " << replicaId << ": DROPPED - Queue is full (" << MAX_QUEUE_SIZE << ")" << std::endl;
            return false;
        }
        
        // Queue has space - add message
        std::cout << "[V2V-SEND] Replica " << replicaId << ": QUEUED - Queue size now " << (messageQueue.size() + 1) 
                  << "/" << MAX_QUEUE_SIZE << std::endl;
        messageQueue.push(pendingMsg);
    }
    
    return true;
}

// Register Java callback object for delivering received messages
void V2VProxyModule::registerJavaCallback(JNIEnv* env, jobject javaObject)
{
    std::cout << "[DEBUG V2VProxy " << replicaId << "] registerJavaCallback called" << std::endl;
    
    std::lock_guard<std::mutex> lock(jniMutex);
    
    // Get JVM reference
    env->GetJavaVM(&jvm);
    
    // Create global reference to Java object
    javaCallbackObject = env->NewGlobalRef(javaObject);
    
    // Get the deliverMessage method
    jclass cls = env->GetObjectClass(javaObject);
    deliverMessageMethod = env->GetMethodID(cls, "deliverMessage", "(I[B)V");
    
    if (!deliverMessageMethod) {
        std::cerr << "[ERROR V2VProxy " << replicaId << "] Failed to find deliverMessage method in Java class" << std::endl;
    } else {
        std::cout << "[DEBUG V2VProxy " << replicaId << "] Java callback registered successfully" << std::endl;
    }
}

// Handle received BFT messages from V2V
void V2VProxyModule::handleBFTMessage(BFTMessage* bftMsg)
{
    std::cout << "[V2V-CONSENSUS] Replica " << replicaId << ": handleBFTMessage (consensus msg) at t=" << simTime() << std::endl;
    ASSERT(bftMsg);
    syncTimeToJava();

    receivedMessages++;
    emit(bftMsgReceivedSignal, receivedMessages);

    int fromReplicaId = bftMsg->getFromReplicaId();
    int toReplicaId = bftMsg->getToReplicaId();

    std::cout << "[V2V-CONSENSUS] Replica " << replicaId << ": Message #" << receivedMessages
              << " from=" << fromReplicaId << " to=" << toReplicaId
              << " (broadcast=" << (toReplicaId == -1 ? "YES" : "NO") << ")" << std::endl;
    
    // Check if message is for us or broadcast
    if (toReplicaId == replicaId || toReplicaId == -1) {
        // std::cout << "[V2V-CONSENSUS] Replica " << replicaId << ": Message IS for us (toReplicaId=" << toReplicaId 
        //           << "), extracting payload and delivering to Java..." << std::endl;
        // Get raw binary payload directly (no base64 decoding needed)
        size_t n = bftMsg->getPayloadArraySize();
        std::vector<uint8_t> buf(n);
        for (size_t i = 0; i < n; ++i) buf[i] = bftMsg->getPayload(i);
        // std::cout << "[V2V-CONSENSUS] Replica " << replicaId << ": Extracted " << n 
        //           << " bytes, calling deliverMessageToJava..." << std::endl;
        deliverMessageToJava(fromReplicaId, buf.data(), (int)buf.size());
        // std::cout << "[V2V-CONSENSUS] Replica " << replicaId << ": deliverMessageToJava returned" << std::endl;




    } else {
        std::cout << "[V2V-CONSENSUS] Replica " << replicaId << ": Message NOT for us (toReplicaId=" << toReplicaId 
                  << " != " << replicaId << "), ignoring broadcast" << std::endl;
    }

    // delete bftMsg;
}

// Deliver message to Java through JNI callback
void V2VProxyModule::deliverMessageToJava(int fromReplicaId, const uint8_t* data, int dataLen)
{
    std::cout << "[V2V-JNI] Replica " << replicaId << ": deliverMessageToJava(" << fromReplicaId 
              << ", " << dataLen << " bytes)" << std::endl;
    std::lock_guard<std::mutex> lock(jniMutex);
    std::cout << "Got the lock" << std::endl;

    if (!jvm || !javaCallbackObject || !deliverMessageMethod) {
        std::cout << "[V2V-JNI] Replica " << replicaId << ": ERROR - Cannot deliver, missing components: "
                  << " jvm=" << (jvm ? "OK" : "NULL")
                  << " callback=" << (javaCallbackObject ? "OK" : "NULL")
                  << " method=" << (deliverMessageMethod ? "OK" : "NULL") << std::endl;
        return;  // ← MOVED INSIDE the if block!
    }

    // Log successful delivery (occasionally)
    static int deliverCount = 0;
    if (++deliverCount <= 10 || deliverCount % 100 == 1) {
        std::cout << "[V2VProxy " << replicaId << "] Delivering message #" << deliverCount
                  << " from replica " << fromReplicaId << " to Java (" << dataLen << " bytes)" << std::endl;
    }
    
    JNIEnv* env;
    jvm->AttachCurrentThread((void**)&env, nullptr);
    
    // Create Java byte array
    jbyteArray javaData = env->NewByteArray(dataLen);
    env->SetByteArrayRegion(javaData, 0, dataLen, (jbyte*)data);
    
    // Call Java method
    std::cout << "[V2V-JNI] Replica " << replicaId << ": Calling Java deliverMessage method..." << std::endl;
    std::cout.flush();
    env->CallVoidMethod(javaCallbackObject, deliverMessageMethod, fromReplicaId, javaData);
    std::cout << "[V2V-JNI] Replica " << replicaId << ": Java deliverMessage call returned" << std::endl;
    std::cout.flush();
    
    // Check for exceptions - PRINT THEM, don't silently hide!
    if (env->ExceptionCheck()) {
        std::cerr << "[V2V-JNI]  Replica " << replicaId << ": JAVA EXCEPTION in deliverMessage!" << std::endl;
        std::cerr << "[V2V-JNI] Exception details:" << std::endl;
        env->ExceptionDescribe();  // Print full stack trace to stderr
        env->ExceptionClear();
        std::cerr << "[V2V-JNI] Exception cleared, continuing..." << std::endl;
    } else {
        std::cout << "[V2V-JNI] Replica " << replicaId << ": No Java exception, delivery successful" << std::endl;
    }
    
    env->DeleteLocalRef(javaData);
}

// Send BFT message via V2V
void V2VProxyModule::sendBFTMessage(int fromReplicaId, int toReplicaId, const std::vector<uint8_t>& data, int messageType)
{
    
    if (isDeparted && messageType != 0) {
        std::cout << "[V2VProxy " << replicaId << "] ZOMBIE: Blocking V2V message type "
                  << messageType << " (departed)" << std::endl;
        return;  // Don't send
    }
    
    
    
    
    
    sentMessages++;
    std::cout << "[V2V-BROADCAST] Replica " << replicaId << ": *** SENDING message #" << sentMessages
              << " from=" << fromReplicaId << " to=" << toReplicaId 
              << " (broadcast=" << (toReplicaId == -1 ? "YES" : "NO") << ")"
              << " msgType=" << messageType << " size=" << data.size() << " bytes at t=" << simTime() << std::endl;

    // Send raw binary data directly (no base64 overhead)
    std::string rawPayload(reinterpret_cast<const char*>(data.data()), data.size());

    BFTMessage* bftMsg = new BFTMessage();

    bftMsg->setFromReplicaId(fromReplicaId);
    bftMsg->setToReplicaId(toReplicaId);
    bftMsg->setMessageType(messageType);  // 0=consensus
    bftMsg->setSequenceNum(sequenceNumber++);
    bftMsg->setTimestamp(simTime());
    bftMsg->setPayloadArraySize(data.size());  // Raw binary data
    for (size_t i = 0; i < data.size(); i++) {
        bftMsg->setPayload(i, data[i]);
    }
    bftMsg->setPayloadLength(data.size());  // Binary size

    // Set wave parameters for broadcast
    // Use CCH (178) for BFT messages - all nodes always listen on CCH
    // Using SCH would require synchronized channel switching
    bftMsg->setChannelNumber(static_cast<int>(veins::Channel::cch));
    bftMsg->addBitLength(par("headerLength"));
    bftMsg->addBitLength(rawPayload.length() * 8);

    emit(bftMsgSentSignal, sentMessages);
    
    // CRITICAL: Set recipient address for V2V broadcast
    bftMsg->setRecipientAddress(LAddress::L2BROADCAST());
    std::cout << "[V2V-BROADCAST] Replica " << replicaId << ": Packet created, broadcasting to LAddress::L2BROADCAST()" << std::endl;

    // REACTIVE YIELD: Mark radio as busy
    double jitter = uniform(0.001, 0.10);
    double delay = replicaId * 0.01 + jitter;
    std::cout << "[V2V-BROADCAST] Replica " << replicaId << ": Calling sendDelayed() with jitter=" << jitter << "s..." << std::endl;
    sendDelayed(bftMsg, delay, lowerLayerOut);
    std::cout << "[V2V-BROADCAST] Replica " << replicaId << ": sendDelayed() returned - packet transmitted to OMNeT++ network layer" << std::endl;
    // radioBusy = true;
    
    
    
    // Send as WSM (Wave Short Message)
    // sendDown(bftMsg);
    
    // // Schedule radioReady callback after estimated transmission time
    // // At 6 Mbps, 600 bytes takes ~0.8ms. Add margin for MAC contention.
    // double txTime = (encodedPayload.length() * 8) / 6e6;
    // double margin = 0.002;  // 2ms for MAC delays
    // if (!radioReadyMsg->isScheduled()) {
    //     scheduleAt(simTime() + jitter + txTime + margin, radioReadyMsg);
    // }
}

void V2VProxyModule::syncTimeToJava() {
    if (!jvm || !clockClass || !updateTimeMethod) {
        // Optional: Print once to debug, but don't crash
        // std::cerr << "[Warning] syncTimeToJava skipped: Clock not initialized" << std::endl;
        return;
    }
    JNIEnv* env;
    jint res = jvm->GetEnv((void**)&env, JNI_VERSION_1_8);
    if (res == JNI_EDETACHED) {
        jvm->AttachCurrentThread((void**)&env, nullptr);
    }
    env->CallStaticVoidMethod(clockClass, updateTimeMethod, simTime().dbl());    
    // Send current OMNeT++ simulation time (in seconds)
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
}

void V2VProxyModule::flushReliabilityQueue() {
    std::cout << "[FLUSH] Replica " << replicaId << ": flushReliabilityQueue() called" << std::endl;

    // 1. CLEAR THE QUEUE (Full Wipe - Clean Slate for next round)
    std::queue<PendingMessage> empty;
    std::swap(messageQueue, empty);
    queueCondVar.notify_all();
    
    // 2. CANCEL RETRANSMISSIONS (Always safe to do, queue is empty)
    if (retxCheckTimer && retxCheckTimer->isScheduled()) {
        std::cout << "[FLUSH] Replica " << replicaId << ": CANCELLING retxCheckTimer (Consensus Complete)" << std::endl;
        cancelEvent(retxCheckTimer);
    }



    // 3. CONDITIONALLY MANAGE THE MAIN RADIO TIMER
    if (currentPhase == WAITING_FOR_CLEARANCE) {
        // THE CAR IS STAYING: Keep the radio awake for the next round!
        std::cout << "[FLUSH] Replica " << replicaId << ": Car is waiting for next round. Keeping processQueueTimer ALIVE." << std::endl;
        
        // (If for some reason it isn't scheduled, start it now)
        if (!processQueueTimer->isScheduled()) {
             scheduleAt(simTime() + 0.05, processQueueTimer);
        }
        
    } else {
        // THE CAR IS LEAVING: Safe to shut down the radio loop.
        if (processQueueTimer && processQueueTimer->isScheduled()) {
            std::cout << "[FLUSH] Replica " << replicaId << ": Car is departing. CANCELLING processQueueTimer!" << std::endl;
            cancelEvent(processQueueTimer);
        } else {
            std::cout << "[FLUSH] Replica " << replicaId << ": processQueueTimer already inactive." << std::endl;
        }
    }



    std::cout << "[V2VProxy " << replicaId << "] Reliability queue flushed." << std::endl;
}


int V2VProxyModule::getMaxMessagesPerTick() {
    const int CHANNEL_CAPACITY = 75;
    const double TARGET_UTIL = 0.85;
    return std::max(2, (int)(CHANNEL_CAPACITY * TARGET_UTIL / BATCH_SIZE)); 
}

bool V2VProxyModule::checkJavaReplicaStatus() {
    if (!jvm) return false;

    std::lock_guard<std::mutex> lock(jvmMutex);

    JNIEnv* env;
    jvm->AttachCurrentThread((void**)&env, nullptr);

    jclass serverRunnerClass = env->FindClass("bftsmart/demo/intersection/ServerRunner");
    if (!serverRunnerClass) return false;

    // Check if replica is actually ready (not just "runner created")
    jmethodID statusMethod = env->GetStaticMethodID(serverRunnerClass,
        "getStatus", "(I)Ljava/lang/String;");

    if (statusMethod) {
        jstring statusStr = (jstring) env->CallStaticObjectMethod(
            serverRunnerClass, statusMethod, replicaId);

        if (statusStr) {
            const char* statusChars = env->GetStringUTFChars(statusStr, nullptr);
            std::string status(statusChars);
            env->ReleaseStringUTFChars(statusStr, statusChars);

            return (status == "READY");  // Only true when BFT is actually initialized
        }
    }

    return false;
}

void V2VProxyModule::triggerRetransmissionCheckViaJNI() {
    if (!jvm || !javaReady) return;
    std::lock_guard<std::mutex> lock(jniMutex);
    JNIEnv* env;

    jvm->AttachCurrentThread((void**)&env, nullptr);
    jclass reliabilityClass = env->FindClass("bftsmart/communication/V2V/ReliableV2VMessaging");

    if (!reliabilityClass){
        env->ExceptionDescribe();
        env->ExceptionClear();
        return;
    }

    // Step 1: Call checkRetransmissionsForAllReplicas() to queue retransmissions
    jmethodID checkMethod = env->GetStaticMethodID(reliabilityClass, "checkRetransmissionsForAllReplicas", "()V");
    if (checkMethod) {
        env->CallStaticVoidMethod(reliabilityClass, checkMethod);
    }

    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        return;
    }

    // Step 2: Get pending retransmissions for this replica (as byte arrays)
    jmethodID getRetxMethod = env->GetStaticMethodID(reliabilityClass, "getPendingRetransmissionsForReplica", "(I)[[B");
    if (!getRetxMethod) {
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: getPendingRetransmissionsForReplica method not found!" << std::endl;
        return;
    }

    jobjectArray retxArray = (jobjectArray) env->CallStaticObjectMethod(reliabilityClass, getRetxMethod, replicaId);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        return;
    }

    if (retxArray == nullptr) {
        return; // No retransmissions
    }

    // Step 3: Send each retransmission via OMNeT++ (now safe - not in JNI call)
    jsize retxCount = env->GetArrayLength(retxArray);
    if (retxCount > 0) {
        std::cout << "[V2VProxy " << replicaId << "] Sending " << retxCount << " queued retransmissions" << std::endl;
    }

    for (jsize i = 0; i < retxCount; i++) {
        jbyteArray byteArray = (jbyteArray) env->GetObjectArrayElement(retxArray, i);
        if (byteArray == nullptr) continue;

        jsize len = env->GetArrayLength(byteArray);
        std::vector<uint8_t> data(len);
        env->GetByteArrayRegion(byteArray, 0, len, (jbyte*)data.data());

        // Use sendBFTMessage for proper broadcast - sets fromReplicaId, toReplicaId=-1,
        // recipientAddress, channel, etc. The envelope has correct from/to inside payload.
        std::cout << "[V2VProxy " << replicaId << "] sending retransmission #" << i << " from replica " << replicaId << " to all replicas" << std::endl;
        sendBFTMessage(replicaId, -1, data, 0);
        sentMessages++;

        env->DeleteLocalRef(byteArray);
    }

    env->DeleteLocalRef(retxArray);
}
void V2VProxyModule::handleSelfMsg(cMessage* msg)
{
    if (replicaId < 0) { return; }
    static int selfMsgCount = 0;

    // Filter out frequent logs to keep output readable
    if (msg != processQueueTimer && msg != checkPositionTimer) {
        std::cout << "[HANDLE-SELF-MSG] Replica " << replicaId << ": Message #" << (++selfMsgCount) 
                  << " at t=" << simTime() << " msgName=" << msg->getName() << std::endl;
    }

    // =========================================================================
    // 1. PROCESS QUEUE TIMER (Heartbeat)
    // =========================================================================
    if (msg == processQueueTimer) {
        // std::cout << "[V2V-QUEUE-TIMER] Replica " << replicaId << ": *** TIMER FIRED at t=" << simTime() << " ***" << std::endl;
        
        // Process queued messages
        std::vector<PendingMessage> toProcess;
        {
            std::lock_guard<std::mutex> lock(jniMutex);
            int count = 0;
            // std::cout << "[V2V-QUEUE] Replica " << replicaId << ": Processing " << messageQueue.size() 
            //           << " queued message(s) at t=" << simTime() << std::endl;

            while (!messageQueue.empty() && count < MAX_MESSAGES_PER_TICK) {
                toProcess.push_back(messageQueue.front());
                messageQueue.pop();
                count++;
            }
            if (messageQueue.size() < MAX_QUEUE_SIZE) {
                queueCondVar.notify_all();
            }
        }
        
        if (!toProcess.empty()) {
            std::cout << "[V2V-QUEUE] Replica " << replicaId << ": Dequeuing " << toProcess.size() << " messages" << std::endl;
            for (size_t i = 0; i < toProcess.size(); i++) {
                const auto& pending = toProcess[i];
                std::cout << "[V2V-QUEUE] Replica " << replicaId << ": [" << (i+1) << "/" << toProcess.size() 
                          << "] Sending " << pending.fromReplicaId << "->" << pending.toReplicaId 
                          << ", " << pending.data.size() << " bytes" << std::endl;
                sendBFTMessage(pending.fromReplicaId, pending.toReplicaId, pending.data, 0);
            }
        }

        // Phase 2 Trigger Check
        if (phase2Pending) {
            std::cout << "[V2VProxy " << replicaId << "] Main thread detected Phase 2 flag. Starting collection..." << std::endl;
            startReadyQCCollection(); 
            phase2Pending = false;
        }

        // RESUME LOGIC (Moved inside timer for reliability)
        {
            std::lock_guard<std::mutex> lock(jniMutex); 
            
            if (!pendingResumeDelays.empty()) {
                shouldFlush = true;
                std::cout << "[RESUME] Replica " << replicaId << ": pendingResumeDelays NOT empty (" 
                          << pendingResumeDelays.size() << "), setting shouldFlush=true" << std::endl;
            }
                
            while (!pendingResumeDelays.empty()) {
                completedConsensusCount++;
                double delay = pendingResumeDelays.front();
                pendingResumeDelays.pop();
                
                // Mark the end of Order consensus
                orderConsensusEndTime = simTime();
                realOrderConsensusEnd = std::chrono::high_resolution_clock::now();
                auto realOrderConsensusDuration = std::chrono::duration_cast<std::chrono::milliseconds>(realOrderConsensusEnd - realOrderConsensusStart);
                std::cout << "[METRICS " << replicaId << "] Order_Consensus_Duration: " << realOrderConsensusDuration.count() << "ms" << std::endl;
                
                
                // Calculate durations
                simtime_t viewConsensusDuration = viewConsensusEndTime - viewConsensusStartTime;
                simtime_t orderConsensusDuration = orderConsensusEndTime - orderConsensusStartTime;
                simtime_t totalConsensusDuration = orderConsensusEndTime - consensusStartTime;
                
                std::cout << "\n========== CONSENSUS METRICS (Replica " << replicaId << ") ==========" << std::endl;
                // View Signature Collection Metrics
                // View Consensus Metrics
                std::cout << "[METRICS " << replicaId << "] === VIEW SIGNATURE COLLECTION ===" << std::endl;
                std::cout << "[METRICS " << replicaId << "] View_Signature_Collection_Start: " << viewSignatureCollectionStartTime << std::endl;
                std::cout << "[METRICS " << replicaId << "] View_Signature_Collection_End: " << viewSignatureCollectionEndTime << std::endl;
                std::cout << "[METRICS " << replicaId << "] View_Signature_Collection_Duration: " << (viewSignatureCollectionEndTime - viewSignatureCollectionStartTime).dbl() << " seconds" << std::endl;

             

                std::cout << "[METRICS " << replicaId << "] === VIEW CONSENSUS ===" << std::endl;
                std::cout << "[METRICS " << replicaId << "] View_Consensus_Start: " << viewConsensusStartTime << std::endl;
                std::cout << "[METRICS " << replicaId << "] View_Consensus_End:   " << viewConsensusEndTime << std::endl;
                std::cout << "[METRICS " << replicaId << "] View_Consensus_Latency: " << viewConsensusDuration.dbl() << " seconds" << std::endl;
                


                // Order Signature Collection Metrics
                std::cout << "[METRICS " << replicaId << "] === ORDER SIGNATURE COLLECTION ===" << std::endl;
                
                std::cout << "[METRICS " << replicaId << "] Order_Signature_Collection_Start: " << orderSignatureCollectionStartTime << std::endl;
                std::cout << "[METRICS " << replicaId << "] Order_Signature_Collection_End: " << orderSignatureCollectionEndTime << std::endl;
                std::cout << "[METRICS " << replicaId << "] Order_Signature_Collection_Duration: " << (orderSignatureCollectionEndTime - orderSignatureCollectionStartTime).dbl() << " seconds" << std::endl;

                // Order Consensus Metrics
                if (orderConsensusStartTime == 0) {
                    orderConsensusStartTime = orderSignatureCollectionEndTime;
                }
                std::cout << "[METRICS " << replicaId << "] === ORDER CONSENSUS ===" << std::endl;
                std::cout << "[METRICS " << replicaId << "] Order_Consensus_Start: " << orderConsensusStartTime << std::endl;
                std::cout << "[METRICS " << replicaId << "] Order_Consensus_End:   " << orderConsensusEndTime << std::endl;
                std::cout << "[METRICS " << replicaId << "] Order_Consensus_Latency: " << orderConsensusDuration.dbl() << " seconds" << std::endl;
                
                // Overall Metrics
                std::cout << "[METRICS " << replicaId << "] === OVERALL ===" << std::endl;
                std::cout << "[METRICS " << replicaId << "] Total_Consensus_Start: " << consensusStartTime << std::endl;
                std::cout << "[METRICS " << replicaId << "] Total_Consensus_End:   " << orderConsensusEndTime << std::endl;
                std::cout << "[METRICS " << replicaId << "] Total_Consensus_Duration: " << totalConsensusDuration.dbl() << " seconds" << std::endl;
                
                // Vehicle timing
                simtime_t scheduledResumeTime = orderConsensusEndTime + delay;
                simtime_t totalWaitTime = scheduledResumeTime - stopTime;
                
                std::cout << "[METRICS " << replicaId << "] Stop_Time: " << stopTime << std::endl;
                std::cout << "[METRICS " << replicaId << "] Projected_Total_Wait: " << totalWaitTime.dbl() << " seconds" << std::endl;
                std::cout << "[METRICS " << replicaId << "] Scheduled_Resume: " << scheduledResumeTime.dbl() << std::endl;
                
                // Message statistics
                std::cout << "[METRICS " << replicaId << "] Messages_Sent: " << sentMessages << std::endl;
                std::cout << "[METRICS " << replicaId << "] Messages_Received: " << receivedMessages << std::endl;
                
                std::cout << "========================================================\n" << std::endl;
                
                // Cancel timeout timer - consensus succeeded!
                if (consensusTimeoutTimer->isScheduled()) {
                    cancelEvent(consensusTimeoutTimer);
                    std::cout << "[METRICS " << replicaId << "] (consensus succeeded : TRUE)" << std::endl;
                }
    
                // Schedule the ACTUAL movement message
                std::cout << "[V2VProxy " << replicaId << "] Main thread scheduling resume in " << delay << "s" << std::endl;
                cMessage* moveMsg = new cMessage("resumeVehicle");
                scheduleAt(simTime() + delay, moveMsg);
            }
    
            if (pendingReconfigFlush) {
                std::cout << "[FLUSH] Replica " << replicaId << ": pendingReconfigFlush=true, calling resetForNextRound()" << std::endl;
                resetForNextRound();
                shouldFlush = false;
                std::cout << "[FLUSH] Replica " << replicaId << ": flushReliabilityQueue() returned" << std::endl;
                return; 
            }
        }
        
        // if (completedConsensusCount == BATCH_SIZE) {
        //     std::cout << "[METRICS " << replicaId << "] All Consensus Completed" << std::endl;
        //     endSimulation();
        // }
        
        // Reschedule timer
        // std::cout << "[V2V-QUEUE-TIMER] Replica " << replicaId << ": Rescheduling for t=" 
        //           << (simTime() + 0.05) << std::endl;
        scheduleAt(simTime() + 0.05, processQueueTimer);
        return;
    }

    // =========================================================================
    // 2. RESUME VEHICLE (By Name Check)
    // =========================================================================
    // Fix: Check by name string because the pointer 'resumeMsg' is not valid for new messages
    if (strcmp(msg->getName(), "resumeVehicle") == 0) {
        std::cout << "[V2VProxy " << replicaId << "] Resume message received at t=" << simTime()
                  << " currentPhase=" << currentPhase << std::endl;
        delete msg;

        // Only resume if we are actually waiting for the GO signal (ORDER_CONSENSUS).
        // Ignore spurious/early resume (e.g. from re-entrant JNI during retx check or duplicate Java callback).
        if (currentPhase != ORDER_CONSENSUS) {
            std::cout << "[V2VProxy " << replicaId << "] IGNORING resume - phase is " << currentPhase
                      << " (expected ORDER_CONSENSUS). Car must not move yet." << std::endl;
            return;
        }
        flushReliabilityQueue();


        simtime_t resumeTime = simTime();
        std::cout << "[METRICS " << replicaId << "] Resume_Time: " << resumeTime << std::endl;
        std::cout << "[METRICS " << replicaId << "] Stop_Time: " << stopTime << std::endl;

        // Time to resume vehicle movement after consensus delay
        std::cout << "[V2VProxy " << replicaId << "] Resume delay expired, resuming vehicle now!" << std::endl;

        if (mobility && mobility->getVehicleCommandInterface()) {
            currentPhase = EXECUTING;
            mobility->getVehicleCommandInterface()->setSpeedMode(0);
            mobility->getVehicleCommandInterface()->setSpeed(30);  // Release control to SUMO
            isStopped = false;
            waitingForConsensus = false;
            std::cout << "[V2VProxy " << replicaId << "] Vehicle RESUMED movement at t=" << simTime() << std::endl;
        } else {
            std::cerr << "[V2VProxy " << replicaId << "] WARNING: mobility or command interface null, cannot resume" << std::endl;
        }
        return;
    }

   // =========================================================================
    // 3. CHECK POSITION TIMER (Consolidated)
    // =========================================================================
    if (msg == checkPositionTimer) {

        // ---------------------------------------------------------------------
        // RULE 0: PREVENT RUNNING THE RED LIGHT
        // If we crossed the line and we are NOT allowed to execute, STOP!
        double distance = getDistanceToIntersection();
        // ---------------------------------------------------------------------
        // We use stopDistance as the physical line. We only apply brakes if we're 
        // getting close to it (e.g. stopDistance + 2.0) but Krauss will stop us AT the car ahead.
        // If we get right up to the line, force the stop.


        // if (distance < stopDistance + 0.1 && !isStopped && currentPhase != EXECUTING && currentPhase != DEPARTED) {
        //     std::cout << "[V2VProxy " << replicaId << "] Reached intersection line. Stopping." << std::endl;
        //     stopVehicle();
        // }

        // ---------------------------------------------------------------------
        // RULE 2: THE CLEARANCE WATCHER (Waiting for the 4 cars to leave)
        // ---------------------------------------------------------------------
        if (isWaitingForClearance) {
            std::set<std::string> visibleNow = getVisibleVehicles(100.0);

            // A. Check if any of the 4 front cars vanished from radar
            for (const std::string& carId : expectedToGo) {
                if (visibleNow.find(carId) == visibleNow.end() &&
                    confirmedDeparted.find(carId) == confirmedDeparted.end()) {
                    confirmedDeparted.insert(carId);
                    std::cout << "[CLEARANCE] " << carId << " departed ("
                              << confirmedDeparted.size() << "/" << expectedToGo.size() << ")" << std::endl;
                }
            }

            // B. Instantly hit the gas if the car ahead of me got the GO signal
            if (!myLaneTriggerCar.empty() && expectedToGo.find(myLaneTriggerCar) != expectedToGo.end()) {
                std::cout << "[CLEARANCE] " << myLaneTriggerCar << " got the GO signal! veh"
                          << replicaId << " tailing it to the stop line." << std::endl;

                if (isStopped) {
                    isStopped = false;
                    mobility->getVehicleCommandInterface()->setSpeedMode(31); 
                    mobility->getVehicleCommandInterface()->setSpeed(-1); // SUMO takes over physics
                }
                
                myLaneTriggerCar = "";
                
                // Also trigger our clearance watch to end early so we can move to IDLE
                // and start our new round once we hit the line.
                // We know the car ahead left, so we are essentially cleared.
            }

            // C. Once all 4 cars are GONE, trigger the next round!
            bool allDeparted = (!expectedToGo.empty() && confirmedDeparted.size() == expectedToGo.size());
            bool timedOut = (simTime() >= clearanceStartTime + CLEARANCE_TIMEOUT);

            if (allDeparted || timedOut) {
                int remainingCars = getVisibleVehicles(300.0).size();
                notifyJavaNewBatchSize(remainingCars);
                laneDiscovered = false;
                discoverLane();

                std::cout << "[CLEARANCE] Intersection is completely clear! Ready to propose." << std::endl;

                isWaitingForClearance = false;
                currentPhase = IDLE;   // Reset phase to trigger Rule 3
                joinTriggered = false; // Reset trigger so Rule 3 fires

                // Release any explicitly stopped car so SUMO can advance it to the
                // new stop line.  Rule 0 will re-stop it (and Rule 3 will fire) once
                // it arrives there.
                if (isStopped) {
                    isStopped = false;
                    mobility->getVehicleCommandInterface()->setSpeedMode(31);
                    mobility->getVehicleCommandInterface()->setSpeed(-1);
                }
            }
        } else if (currentPhase == EXECUTING) {
            // Wait for myself to cross
            if (isDeparted) {
                // Done crossing
            }
        }

        // ---------------------------------------------------------------------
        // RULE 3: NEW ROUND TRIGGER
        // If we are parked at the line and the intersection is clear, PROPOSE!
        // ---------------------------------------------------------------------
        distance = getDistanceToIntersection();

        // Ensure we are physically AT the intersection AND the first car in the lane
        // If there's a car ahead of us (carAhead != ""), we are just waiting in the queue
        if (distance < stopDistance + 1.5 && currentPhase == IDLE && !joinTriggered) {

            // Re-discover lane to make sure `carAhead` is accurate this tick.
            // Force a fresh scan so stale entries (e.g. a departed veh4) are cleared.
            laneDiscovered = false;
            discoverLane();

            if (carAhead.empty()) {
                std::cout << "[V2VProxy " << replicaId << "] ===== APPROACHING INTERSECTION =====" << std::endl;
                stopVehicle();
                initiateViewProposal();  // START THE NEW ROUND
                viewSignatureCollectionStartTime = simTime(); 
                
                joinTriggered = true;
                stopTime = simTime();
                hasRequestedCrossing = true;
                waitingForConsensus = true;
                consensusStartTime = simTime();
                
                if (!consensusTimeoutTimer->isScheduled()) {
                     scheduleAt(simTime() + consensusTimeoutSec, consensusTimeoutTimer);
                }
            } else {
                // If there's a car ahead but we haven't crossed, 
                // just wait naturally. SUMO Krauss physics holds us behind them.
                // We won't propose until they leave and we become the new front car.
                // Make sure we are in IDLE so we keep checking
                currentPhase = IDLE;

                // Ensure SUMO physics takes over and closes the gap if front car leaves.
                if (isStopped && !carAhead.empty()) {
                    double ahead_speed = 0.0;
                    try {
                        auto traciCmd = mobility->getCommandInterface();
                        if (traciCmd) {
                            std::list<std::string> activeVehicles = traciCmd->getVehicleIds();
                            if (std::find(activeVehicles.begin(), activeVehicles.end(), carAhead) != activeVehicles.end()) {
                                ahead_speed = traciCmd->vehicle(carAhead).getSpeed();
                            } else {
                                // If the car ahead is no longer in SUMO, treat it as departed (speed > 0.1)
                                // so we pull forward immediately.
                                ahead_speed = 999.0;
                            }
                        }
                    } catch (...) {}

                    // Only roll up if the ahead car is moving (or departed), otherwise stay braked.
                    if (ahead_speed > 0.1) {
                         isStopped = false;
                         mobility->getVehicleCommandInterface()->setSpeedMode(0);
                         mobility->getVehicleCommandInterface()->setSpeed(-1);
                    }
                }
            }
        }

        // Keep the heartbeat alive!
        scheduleAt(simTime() + 0.1, checkPositionTimer);
        return;
    }
    // =========================================================================
    // 4. OTHER TIMERS
    // =========================================================================

    if (msg == readyQCTimeoutTimer) {
        std::cout << "[V2VProxy " << replicaId << "] ReadyQC timeout (deprecated in new flow)" << std::endl;
        delete msg;
        readyQCTimeoutTimer = nullptr;
        return;
    }

    if (msg == viewConsensusTimer) {
        if (currentPhase == VIEW_CONSENSUS) {
            std::cout << "[V2VProxy " << replicaId << "] View consensus timeout - still waiting for BFT response" << std::endl;
        }
        return;
    }
    
    if (msg == retxCheckTimer) {
        syncTimeToJava(); 
        triggerRetransmissionCheckViaJNI();
        scheduleAt(simTime() + 0.5, retxCheckTimer);
        std::cout << "[V2VProxy " << replicaId << "] Retransmission check at t=" << simTime() << std::endl;
        return;
    }
    
    if (msg == checkJavaReadyTimer) {
        if (!javaReady) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            bool isReady = checkJavaReplicaStatus();
            if (isReady) {
                javaReady = true;
                std::cout << "[V2VProxy " << replicaId << "] *** JAVA READY at t="<< simTime() << " ***" << std::endl;
                
                // CRITICAL FIX: Only schedule if NOT ALREADY scheduled!
                if (!checkPositionTimer->isScheduled()) {
                    scheduleAt(simTime() + 0.1, checkPositionTimer);
                }
            } else {
                std::cout << "[V2VProxy " << replicaId << "] Waiting for Java (t="<< simTime() << ")..." << std::endl;
                scheduleAt(simTime() + 0.1, checkJavaReadyTimer);
            }
        }
        return;
    }

    if (msg == startReadyQCCollectionMsg) {
        std::cout << "[V2VProxy " << replicaId << "] Starting Phase 2 at t=" << simTime() << std::endl;
        double jitter = uniform(0.010, 0.050);
        scheduleAt(simTime() + jitter, startReadyQCCollectionMsg);
      //  startReadyQCCollection();
        // Do not delete member variables that might be reused, or set to null
        delete msg;
        startReadyQCCollectionMsg = nullptr; 
        return;
    }
    
    if (msg == triggerJoinTimer) {
        std::cout << "[DEBUG V2VProxy " << replicaId << "] triggerJoinTimer (deprecated in TPWC) at t=" << simTime() << std::endl;
        return;
    } 
    
    if (msg == radioReadyMsg) {
        radioBusy = false;
        {
            std::lock_guard<std::mutex> lock(jniMutex);
            queueCondVar.notify_all();
        }
        notifyJavaRadioReady();
        return;
    } 
    
    if (msg == consensusTimeoutTimer) {
        std::cout << "[V2VProxy " << replicaId << "] *** CONSENSUS TIMEOUT *** No quorum reached after "
                  << consensusTimeoutSec << "s" << std::endl;
        std::cout << "[METRICS " << replicaId << "] (consensus succeeded : FALSE)" << std::endl;
        std::cout << "[V2VProxy " << replicaId << "] FALLBACK: Resuming without coordination" << std::endl;
        resumeVehicle(0.0);
        waitingForConsensus = false;
        return;
    }

    // =========================================================================
    // 5. ORDER COLLECTION WINDOW
    // =========================================================================
    if (msg == orderCollectDeadlineTimer) {
        if (!orderDecisionReceived && orderCollectionActive) {
            if (!orderBagProposed) {
                // Never proposed yet — propose now with closeFlag=true
                proposeOrderBagNow("DEADLINE");
            } else {
                // Already proposed early — send one final retransmit with closeFlag=true
                // to signal Java to finalize with whatever it has
                orderBagCloseFlag = true;
                auto bag = buildOrderBagQCs();
                if (!bag.empty()) {
                    std::cout << "[ORDER-BAG] Replica " << replicaId
                              << " DEADLINE retransmit with closeFlag=true at t=" << simTime() << std::endl;
                    triggerJoinViaJNI(serializeOrderBagRequest(bag, true));
                }
            }
        }
        return;
    }

    if (msg == orderGossipRetransmitTimer) {
        // Re-broadcast own ReadyQC with jitter to improve distribution during collection window
       
        std::string myCarId = "veh" + std::to_string(replicaId);
        if (myReadyQCComplete && !orderDecisionReceived && orderCollectionActive && isMyQCFrontMostKnownInLaneFromPool()) {
            if (verifiedPool.count(myCarId)) {
                std::vector<uint8_t> payload = serializeReadyQC(verifiedPool[myCarId]);
                sendBFTMessage(replicaId, -1, payload, 3);
                std::cout << "[ORDER-GOSSIP] Replica " << replicaId
                          << " re-broadcast own ReadyQC at t=" << simTime() << std::endl;
            }
            // Reschedule once more if well before deadline
            if (simTime() < orderCollectionDeadline - SimTime(0.05)) {
                scheduleAt(simTime() + uniform(0.05, 0.12), orderGossipRetransmitTimer);
            }
        }
        return;
    }

    if (msg == orderBagRetransmitTimer) {
        if (orderDecisionReceived) return;
        if (orderBagRetransmitCount >= 0) return;  // cap retries

        orderBagRetransmitCount++;

        auto bag = buildOrderBagQCs(); // rebuild so it can improve as pool grows
        if (!bag.empty()) {
            triggerJoinViaJNI(serializeOrderBagRequest(bag, orderBagCloseFlag));
        }
        scheduleAt(simTime() + uniform(0.05, 0.12), orderBagRetransmitTimer);
        return;
    }

    std::cout << "[HANDLE-SELF-MSG] Replica " << replicaId << ": msg=" << msg->getName() << std::endl;
    DemoBaseApplLayer::handleSelfMsg(msg);
}

void V2VProxyModule::handleLowerMsg(cMessage* msg)
{
    if (replicaId < 0) {return; }
    
    static int lowerMsgCount = 0;
    if (++lowerMsgCount % 50 == 1 || lowerMsgCount <= 20) {
        std::cout << "[HANDLE-LOWER-MSG] Replica " << replicaId << ": Message #" << lowerMsgCount 
                  << " at t=" << simTime() << " msgType=" << msg->getName() << std::endl;
    }



    
    BFTMessage* bftMsg = dynamic_cast<BFTMessage*>(msg);
    if (bftMsg) {
        
        if (isDeparted) {
            // The car has crossed the intersection. Destroy the packet immediately.
            delete bftMsg;
            return; 
        }
        
        std::cout << "[V2V-RECEIVE] Replica " << replicaId << ": *** RECEIVED packet from OMNeT++ at t=" << simTime() 
                  << " from=" << bftMsg->getFromReplicaId() << " to=" << bftMsg->getToReplicaId() 
                  << " msgType=" << bftMsg->getMessageType() << std::endl;
        handlepreConsensusMessages(bftMsg);
    } else {
        DemoBaseApplLayer::handleLowerMsg(msg);
    }
}


// Notify Java that radio is ready (reactive yield pattern)
void V2VProxyModule::notifyJavaRadioReady()
{
    std::lock_guard<std::mutex> lock(jvmMutex);
    
    if (!sharedJVM || !javaCallbackObject) return;
    
    JNIEnv* env;
    int envStat = sharedJVM->GetEnv((void**)&env, JNI_VERSION_1_8);
    if (envStat == JNI_EDETACHED) {
        sharedJVM->AttachCurrentThread((void**)&env, nullptr);
    }
    
    // Get onRadioReady method if not cached
    if (!onRadioReadyMethod) {
        jclass cls = env->GetObjectClass(javaCallbackObject);
        onRadioReadyMethod = env->GetMethodID(cls, "onRadioReady", "()V");
        if (!onRadioReadyMethod) {
            // Method not found - Java hasn't implemented it yet, that's okay
            env->ExceptionClear();
            return;
        }
    }
    
    // Call Java's onRadioReady()
    env->CallVoidMethod(javaCallbackObject, onRadioReadyMethod);
    
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }
}

bool V2VProxyModule::triggerJoinViaJNI(const std::string& request )
{
    std::cout << "[V2VProxy " << replicaId << "] triggerJoinViaJNI('" << request << "') called at t=" << simTime() << std::endl;

    // Note: Removed joinTriggered flag check - in TPWC we need multiple consensus rounds
    // (VIEW, ORDER, then VIEW again for next batch, etc.)

    std::lock_guard<std::mutex> lock(jvmMutex);
    
    if (!sharedJVM) {
        std::cerr << "[ERROR V2VProxy " << replicaId << "] No JVM available for triggerJoin" << std::endl;
        return false;
    }
    
    JNIEnv* env;
    int envStat = sharedJVM->GetEnv((void**)&env, JNI_VERSION_1_8);
    if (envStat == JNI_EDETACHED) {
        sharedJVM->AttachCurrentThread((void**)&env, nullptr);
    }
    
    // Find ServerRunner class
    jclass serverRunnerClass = env->FindClass("bftsmart/demo/intersection/ServerRunner");
    if (!serverRunnerClass) {
        std::cerr << "[ERROR V2VProxy " << replicaId << "] Failed to find ServerRunner class" << std::endl;
        env->ExceptionDescribe();
        env->ExceptionClear();
        return false;
    }

    // Check barrier status to see if replicas are coordinating
    jmethodID barrierMethod = env->GetStaticMethodID(serverRunnerClass, "getBarrierStatus", "()Ljava/lang/String;");
    if (barrierMethod) {
        jstring barrierStr = (jstring) env->CallStaticObjectMethod(serverRunnerClass, barrierMethod);
        if (barrierStr) {
            const char* barrierChars = env->GetStringUTFChars(barrierStr, nullptr);
            std::string barrierStatus(barrierChars);
            env->ReleaseStringUTFChars(barrierStr, barrierChars);

            // Only log occasionally to avoid spam
            static int pollCount = 0;
            if (++pollCount % 10 == 1) {
                std::cout << "[V2VProxy " << replicaId << "] Barrier: " << barrierStatus << std::endl;
            }
        }
    }

    jmethodID statusMethod = env->GetStaticMethodID(serverRunnerClass, "getStatus", "(I)Ljava/lang/String;");
    if (statusMethod) {
        jstring statusStr = (jstring) env->CallStaticObjectMethod(serverRunnerClass, statusMethod, replicaId);
        const char* statusChars = env->GetStringUTFChars(statusStr, nullptr);
        std::string status(statusChars);
        env->ReleaseStringUTFChars(statusStr, statusChars);

        if (status != "READY") {
            std::cout << "[V2VProxy " << replicaId << "] FAILED: Server Status = '" << status
                      << "' (expected 'READY') at t=" << simTime() << std::endl;
            return false;
        }
        std::cout << "[V2VProxy " << replicaId << "] PASSED: Server Status = 'READY'" << std::endl;
    }

    jmethodID readyMethod = env->GetStaticMethodID(serverRunnerClass, "isReplicaReady", "(I)Z");
    if (readyMethod) {
        jboolean isReady = env->CallStaticBooleanMethod(serverRunnerClass, readyMethod, replicaId);
        
        if (!isReady) {
            std::cout << "[V2VProxy " << replicaId << "] FAILED: isReplicaReady() = false at t="
                      << simTime() << std::endl;
            return false; // Return false so handleSelfMsg reschedules the timer
        }
        std::cout << "[V2VProxy " << replicaId << "] PASSED: isReplicaReady() = true" << std::endl;
    } else {
        std::cerr << "[ERROR] Could not find isReplicaReady method!" << std::endl;
        return false;
    }
    
    // Get the static triggerJoinForReplica method with String parameter
    jmethodID triggerMethod = env->GetStaticMethodID(serverRunnerClass, "triggerJoinForReplica", "(ILjava/lang/String;)V");

    if (triggerMethod) {
        // Convert C++ string to Java string
        jstring jRequest = env->NewStringUTF(request.c_str());

        // Call Java method with replica ID and request string
        env->CallStaticVoidMethod(serverRunnerClass, triggerMethod, replicaId, jRequest);

        // Clean up local reference
        env->DeleteLocalRef(jRequest);

        if (!env->ExceptionCheck()) {
            std::cout << "[V2VProxy " << replicaId << "] SUCCESS: Triggered consensus request '"
                      << request << "' at t=" << simTime() << std::endl;
            return true; // Success!
        } else {
            std::cerr << "[V2VProxy " << replicaId << "] Exception calling triggerJoinForReplica" << std::endl;
            env->ExceptionDescribe();
            env->ExceptionClear();
        }
    } else {
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: Could not find triggerJoinForReplica(I, String) method" << std::endl;
    }

    return false;

}

void V2VProxyModule::onBSM(DemoSafetyMessage* bsm)
{
    // Not used for BFT communication
    delete bsm;
}

void V2VProxyModule::onWSM(BaseFrame1609_4* wsm)
{
    // Check if it's a BFT message
    BFTMessage* bftMsg = dynamic_cast<BFTMessage*>(wsm);
    if (bftMsg) {
        handlepreConsensusMessages(bftMsg);
    } else {
        delete wsm;
    }
}

void V2VProxyModule::onWSA(DemoServiceAdvertisment* wsa)
{
    // Not used
    delete wsa;
}

// ============================================================================
// JVM MANAGEMENT
// ============================================================================

bool V2VProxyModule::warmupJVM(JNIEnv* env)
{
    try {
        jclass bridgeClass = env->FindClass("bftsmart/communication/V2V/V2VNativeBridge");
        if (!bridgeClass) {
            std::cerr << "[V2VProxy] ERROR: Failed to find V2VNativeBridge class" << std::endl;
            return false;
        }
        jmethodID warmupMethod = env->GetStaticMethodID(bridgeClass, "nativeWarmupPing", "()V");
        
        if (!warmupMethod) {
            std::cerr << "[V2VProxy] ERROR: Failed to find nativeWarmupPing method" << std::endl;
            return false;
    
        }

        env->CallStaticVoidMethod(bridgeClass, warmupMethod);

        if (env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
            return false;
        }

        return true;
    } catch (const std::exception& e) {
        std::cerr << "[V2VProxy] ERROR: Failed to warm up JVM: " << e.what() << std::endl;
        return false;
    }

}

bool V2VProxyModule::createOrAttachJVM()
{
    std::lock_guard<std::mutex> lock(jvmMutex);

    // If JVM already exists, attach this thread
    if (sharedJVM != nullptr) {
        EV_INFO << "Replica " << replicaId << ": Attaching to existing JVM" << std::endl;

        JNIEnv* env;
        jint result = sharedJVM->AttachCurrentThread((void**)&env, nullptr);
        if (result != JNI_OK) {
            EV_ERROR << "Failed to attach to JVM: " << result << std::endl;
            return false;
        }

        jvm = sharedJVM;
        return true;
    }

    // Create new JVM (first replica only)
    EV_INFO << "Replica " << replicaId << ": Creating new JVM" << std::endl;

    JavaVMInitArgs vm_args;
    JavaVMOption options[14];  // Increased for intersection physics parameters + entropy fix
    int optionIndex = 0;

    // Set classpath to include BFTSmart JARs
    // Include all JARs from the build/install/library/lib directory
    std::string classpath = "-Djava.class.path="
        "/home/yash/omnetpp/omnetpp-6.2.0/bftsmart/library/build/classes/java/main:"
        "/home/yash/omnetpp/omnetpp-6.2.0/bftsmart/library/build/resources/main:"
        "/home/yash/omnetpp/omnetpp-6.2.0/bftsmart/library/build/install/library/lib/BFT-SMaRt.jar:"
        "/home/yash/omnetpp/omnetpp-6.2.0/bftsmart/library/build/install/library/lib/BenchmarkExecutor.jar:"
        "/home/yash/omnetpp/omnetpp-6.2.0/bftsmart/library/build/install/library/lib/bcpkix-jdk15on-1.69.jar:"
        "/home/yash/omnetpp/omnetpp-6.2.0/bftsmart/library/build/install/library/lib/bcprov-jdk15on-1.69.jar:"
        "/home/yash/omnetpp/omnetpp-6.2.0/bftsmart/library/build/install/library/lib/bcutil-jdk15on-1.69.jar:"
        "/home/yash/omnetpp/omnetpp-6.2.0/bftsmart/library/build/install/library/lib/commons-codec-1.15.jar:"
        "/home/yash/omnetpp/omnetpp-6.2.0/bftsmart/library/build/install/library/lib/core-0.1.4.jar:"
        "/home/yash/omnetpp/omnetpp-6.2.0/bftsmart/library/build/install/library/lib/logback-classic-1.2.5.jar:"
        "/home/yash/omnetpp/omnetpp-6.2.0/bftsmart/library/build/install/library/lib/logback-core-1.2.5.jar:"
        "/home/yash/omnetpp/omnetpp-6.2.0/bftsmart/library/build/install/library/lib/netty-all-4.1.67.Final.jar:"
        "/home/yash/omnetpp/omnetpp-6.2.0/bftsmart/library/build/install/library/lib/slf4j-api-1.7.32.jar:"
        "/home/yash/omnetpp/omnetpp-6.2.0/bftsmart/library/build/install/library/lib/lz4-1.3.0.jar";

    options[optionIndex++].optionString = const_cast<char*>(classpath.c_str());

    // Set library path for JNI library
    std::string libpath = "-Djava.library.path="
        "/home/yash/omnetpp/omnetpp-6.2.0/bftsmart/library/native/lib";
    options[optionIndex++].optionString = const_cast<char*>(libpath.c_str());

    // Enable verbose output for debugging
    // options[optionIndex++].optionString = const_cast<char*>("-verbose:jni");
    // options[optionIndex++].optionString = const_cast<char*>("-verbose:class");

    // Tell BFTSmart to use V2V communication instead of TCP
    std::string useV2V = "-Dbftsmart.communication.useV2V=true";
    options[optionIndex++].optionString = const_cast<char*>(useV2V.c_str());

    // Tell V2VNativeBridge we're in embedded mode (library already loaded)
    std::string embeddedMode = "-Dbftsmart.jni.embedded=true";
    options[optionIndex++].optionString = const_cast<char*>(embeddedMode.c_str());

    // Intersection physics parameters for delay calculation
    std::string widthProp = "-Dintersection.width=" + std::to_string(intersectionWidth);
    options[optionIndex++].optionString = const_cast<char*>(widthProp.c_str());

    std::string speedProp = "-Dintersection.avgSpeed=" + std::to_string(avgSpeed);
    options[optionIndex++].optionString = const_cast<char*>(speedProp.c_str());

    std::string safetyProp = "-Dintersection.safetyGap=" + std::to_string(safetyGap);
    options[optionIndex++].optionString = const_cast<char*>(safetyProp.c_str());

    // Memory settings
    options[optionIndex++].optionString = const_cast<char*>("-Xms256m");
    options[optionIndex++].optionString = const_cast<char*>("-Xmx1024m");

    // CRITICAL FIX: Force non-blocking entropy source for crypto operations
    // Without this, SecureRandom/ECDSA signing can block indefinitely in WSL2/VMs
    // waiting for /dev/random entropy. This uses /dev/urandom instead (non-blocking).
    options[optionIndex++].optionString = const_cast<char*>("-Djava.security.egd=file:/dev/./urandom");

    vm_args.version = JNI_VERSION_1_8;
    vm_args.nOptions = optionIndex;
    vm_args.options = options;
    vm_args.ignoreUnrecognized = JNI_FALSE;

    JNIEnv* env;
    jint result = JNI_CreateJavaVM(&sharedJVM, (void**)&env, &vm_args);

    if (result != JNI_OK) {
        EV_ERROR << "Failed to create JVM: " << result << std::endl;
        return false;
    }

    jvm = sharedJVM;
    std::cout << "[V2VProxy] JVM created successfully" << std::endl;
    
    // Manually register JNI native methods (required in embedded mode)
    std::cout << "[V2VProxy] Registering JNI native methods..." << std::endl;
    if (!registerJNINativeMethods(env)) {
        std::cerr << "[V2VProxy] ERROR: Failed to register JNI native methods" << std::endl;
        return false;
    }
    std::cout << "[V2VProxy] JNI native methods registered successfully" << std::endl;

    std::cout << "[V2VProxy] Warming up JVM (crypto + JNI) before starting replicas..." << std::endl;

    if (!warmupJVM(env)) {
        std::cerr << "[V2VProxy] ERROR: Failed to warm up JVM" << std::endl;
        return false;
    }
    std::cout << "[V2VProxy] JVM warmed up successfully" << std::endl;
    
    return true;
}

// Forward declare JNI functions from V2VJNIBridge.cc
extern "C" {
    JNIEXPORT void JNICALL Java_bftsmart_communication_V2V_V2VNativeBridge_nativeInit(JNIEnv*, jobject, jint);
    JNIEXPORT jboolean JNICALL Java_bftsmart_communication_V2V_V2VNativeBridge_nativeSendMessage(JNIEnv*, jobject, jint, jint, jbyteArray);
    JNIEXPORT void JNICALL Java_bftsmart_communication_V2V_V2VNativeBridge_nativeShutdown(JNIEnv*, jobject, jint);
    JNIEXPORT jboolean JNICALL Java_bftsmart_communication_V2V_V2VNativeBridge_nativeIsRadioBusy(JNIEnv*, jobject, jint);
    JNIEXPORT void JNICALL Java_bftsmart_communication_V2V_V2VNativeBridge_nativeWarmupPing(JNIEnv*, jclass);
}

// Forward declarations for JNI callbacks implemented in V2VJNIBridge.cc
extern "C" {
    JNIEXPORT void JNICALL Java_bftsmart_demo_intersection_IntersectionServer_notifyViewAgreed
        (JNIEnv*, jobject, jint, jstring);
    JNIEXPORT void JNICALL Java_bftsmart_demo_intersection_IntersectionServer_notifyVehicleCanGo
        (JNIEnv*, jobject, jint, jdouble);
    JNIEXPORT void JNICALL Java_bftsmart_demo_intersection_IntersectionServer_notifyReconfigComplete
        (JNIEnv*, jobject, jint);
}



extern "C" {
    JNIEXPORT void JNICALL Java_bftsmart_demo_intersection_IntersectionServer_notifyOrderDecided
    (JNIEnv*, jobject, jint, jstring);
}

// Implementation of isRadioBusy native method
// NOTE: notifyViewAgreed and notifyVehicleCanGo are implemented in V2VJNIBridge.cc
JNIEXPORT jboolean JNICALL Java_bftsmart_communication_V2V_V2VNativeBridge_nativeIsRadioBusy(JNIEnv* env, jobject obj, jint replicaId)
{
    V2VProxyModule* proxy = V2VProxyModule::getProxyForReplica(replicaId);
    if (proxy) {
        return proxy->isRadioBusy() ? JNI_TRUE : JNI_FALSE;
    }
    return JNI_FALSE;  // If no proxy, assume not busy
}

bool V2VProxyModule::registerJNINativeMethods(JNIEnv* env)
{
    // Find the V2VNativeBridge class
    jclass bridgeClass = env->FindClass("bftsmart/communication/V2V/V2VNativeBridge");
    if (!bridgeClass) {
        std::cerr << "[V2VProxy] ERROR: Failed to find V2VNativeBridge class" << std::endl;
        env->ExceptionDescribe();
        env->ExceptionClear();
        return false;
    }

    jclass localClockCls = env->FindClass("bftsmart/communication/V2V/SimulationClock");
    if (!localClockCls) {
        std::cerr << "[V2VProxy] ERROR: Failed to find SimulationClock class" << std::endl;
        env->ExceptionDescribe();
        env->ExceptionClear();
        return false;
    }
    clockClass = (jclass) env->NewGlobalRef(localClockCls);
    updateTimeMethod = env->GetStaticMethodID(clockClass, "updateTime", "(D)V");

    // Define the native methods to register
    JNINativeMethod methods[] = {
        {const_cast<char*>("nativeInit"), const_cast<char*>("(I)V"), (void*)&Java_bftsmart_communication_V2V_V2VNativeBridge_nativeInit},
        {const_cast<char*>("nativeSendMessage"), const_cast<char*>("(II[B)Z"), (void*)&Java_bftsmart_communication_V2V_V2VNativeBridge_nativeSendMessage},
        {const_cast<char*>("nativeShutdown"), const_cast<char*>("(I)V"), (void*)&Java_bftsmart_communication_V2V_V2VNativeBridge_nativeShutdown},
        {const_cast<char*>("nativeIsRadioBusy"), const_cast<char*>("(I)Z"), (void*)&Java_bftsmart_communication_V2V_V2VNativeBridge_nativeIsRadioBusy},
        {const_cast<char*>("nativeWarmupPing"), const_cast<char*>("()V"), (void*)&Java_bftsmart_communication_V2V_V2VNativeBridge_nativeWarmupPing}

    };

    // Register the methods
    if (env->RegisterNatives(bridgeClass, methods, 5) != 0) {
        std::cerr << "[V2VProxy] ERROR: Failed to register native methods" << std::endl;
        env->ExceptionDescribe();
        env->ExceptionClear();
        return false;
    }

    std::cout << "[V2VProxy] Successfully registered 4 JNI native methods" << std::endl;
    // return true;

    jclass intersectionServerClass = env->FindClass("bftsmart/demo/intersection/IntersectionServer");
    if (!intersectionServerClass) {
        std::cerr << "[V2VProxy] ERROR: Failed to find IntersectionServer class" << std::endl;
        env->ExceptionDescribe();
        env->ExceptionClear();
        return false;
    }

    JNINativeMethod serverMethods[] = {
        {const_cast<char*>("notifyVehicleCanGo"), const_cast<char*>("(ID)V"), (void*)&Java_bftsmart_demo_intersection_IntersectionServer_notifyVehicleCanGo},
        {const_cast<char*>("notifyViewAgreed"), const_cast<char*>("(ILjava/lang/String;)V"), (void*)&Java_bftsmart_demo_intersection_IntersectionServer_notifyViewAgreed},
        {const_cast<char*>("notifyOrderDecided"), const_cast<char*>("(ILjava/lang/String;)V"), (void*)&Java_bftsmart_demo_intersection_IntersectionServer_notifyOrderDecided},
        {const_cast<char*>("notifyReconfigComplete"), const_cast<char*>("(I)V"), (void*)&Java_bftsmart_demo_intersection_IntersectionServer_notifyReconfigComplete}
    };

    if (env->RegisterNatives(intersectionServerClass, serverMethods, 4) != 0) {
        std::cerr << "[V2VProxy] ERROR: Failed to register IntersectionServer native methods" << std::endl;
        env->ExceptionDescribe();
        env->ExceptionClear();
        return false;
    }

    std::cout << "[V2VProxy] Successfully registered 4 IntersectionServer JNI native methods" << std::endl;
    return true;


}

void V2VProxyModule::startBFTSmartReplica()
{
    if (!jvm) {
        EV_ERROR << "Cannot start BFTSmart replica: JVM not initialized" << std::endl;
        return;
    }

    JNIEnv* env;
    jvm->AttachCurrentThread((void**)&env, nullptr);

    std::cout << "[V2VProxyModule] Starting BFTSmart replica " << replicaId << " in background Java thread" << std::endl;

    // Find ServerRunner class (wrapper that runs IntersectionServer in a thread)
    jclass runnerClass = env->FindClass("bftsmart/demo/intersection/ServerRunner");
    if (!runnerClass) {
        std::cerr << "[V2VProxyModule] ERROR: Failed to find ServerRunner class" << std::endl;
        env->ExceptionDescribe();
        return;
    }

    // Get ServerRunner constructor: ServerRunner(int replicaId, int numCars)
    jmethodID runnerCtor = env->GetMethodID(runnerClass, "<init>", "(II)V");
    if (!runnerCtor) {
        std::cerr << "[V2VProxyModule] ERROR: Failed to find ServerRunner constructor" << std::endl;
        env->ExceptionDescribe();
        return;
    }

    // Create ServerRunner instance (this is fast, doesn't block)
    jobject runnerInstance = env->NewObject(runnerClass, runnerCtor, replicaId, BATCH_SIZE);
    if (!runnerInstance) {
        std::cerr << "[V2VProxyModule] ERROR: Failed to create ServerRunner instance" << std::endl;
        env->ExceptionDescribe();
        return;
    }

    // Find Thread class
    jclass threadClass = env->FindClass("java/lang/Thread");
    if (!threadClass) {
        std::cerr << "[V2VProxyModule] ERROR: Failed to find Thread class" << std::endl;
        env->ExceptionDescribe();
        return;
    }

    // Get Thread constructor: Thread(Runnable target)
    jmethodID threadCtor = env->GetMethodID(threadClass, "<init>", "(Ljava/lang/Runnable;)V");
    if (!threadCtor) {
        std::cerr << "[V2VProxyModule] ERROR: Failed to find Thread constructor" << std::endl;
        env->ExceptionDescribe();
        return;
    }

    // Create Thread with ServerRunner as the Runnable
    jobject thread = env->NewObject(threadClass, threadCtor, runnerInstance);
    if (!thread) {
        std::cerr << "[V2VProxyModule] ERROR: Failed to create Thread" << std::endl;
        env->ExceptionDescribe();
        return;
    }

    // Get Thread.start() method
    jmethodID startMethod = env->GetMethodID(threadClass, "start", "()V");
    if (!startMethod) {
        std::cerr << "[V2VProxyModule] ERROR: Failed to find Thread.start method" << std::endl;
        env->ExceptionDescribe();
        return;
    }

    // Start the thread (THIS RETURNS IMMEDIATELY - non-blocking!)
    std::cout << "[V2VProxyModule] Starting Java thread for replica " << replicaId << std::endl;
    env->CallVoidMethod(thread, startMethod);

    // Keep a global reference to the thread
    bftReplicaThread = env->NewGlobalRef(thread);

    std::cout << "[V2VProxyModule] BFTSmart replica " << replicaId << " thread started (non-blocking)" << std::endl;

    // Check for exceptions
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
}

void V2VProxyModule::stopBFTSmartReplica()
{
    if (bftReplicaThread && jvm) {
        JNIEnv* env;
        jvm->AttachCurrentThread((void**)&env, nullptr);

        // TODO: Call shutdown method on IntersectionServer if it has one

        env->DeleteGlobalRef(bftReplicaThread);
        bftReplicaThread = nullptr;

        EV_INFO << "BFTSmart replica " << replicaId << " stopped" << std::endl;
    }
}

// ============================================================================
// INTERSECTION MANAGEMENT
// ============================================================================

void V2VProxyModule::handlePositionUpdate(cObject* obj)
{
    if (replicaId < 0) {  return; }
    DemoBaseApplLayer::handlePositionUpdate(obj);
    std::cout << "[POSITION UPDATE] Replica " << replicaId << " at time " << simTime() << std::endl;
    
    std::cout << "[POSITION UPDATE] Replica " << replicaId << " current phase: " << currentPhase << "Has departed: " << isDeparted << std::endl;

    if (!isDeparted && currentPhase == EXECUTING) {
        checkIfDeparted();
    }
    // Position updates are handled via checkPositionTimer for efficiency
}

double V2VProxyModule::getDistanceToIntersection()
{
    if (!mobility || !mobility->getCommandInterface()) {
        return 1e10;
    }

    try {
        std::string myId = "veh" + std::to_string(replicaId);
        std::string myLaneId = mobility->getCommandInterface()->vehicle(myId).getLaneId();
        
        // If we are on an internal lane (inside the intersection, typical starts with ':')
        // then distance to intersection is effectively 0
        if (myLaneId.empty() || myLaneId.front() == ':') {
            return 0.0;
        }

        // Only calculate distance to intersection if the vehicle is on one of the incoming edges
        // otherwise, it has already crossed and shouldn't trigger "approaching" behavior again
        // Typically, incoming edges end at the node (e.g. N2C ends at C)
        // Check if edge is outgoing (e.g C2E) and return large value if so
        // std::string edgeId = mobility->getCommandInterface()->vehicle(myId).getRoadId();
        // if (edgeId.length() >= 3 && edgeId.substr(0, 1) == "C") {
        //      return 1e10; // We are on an outgoing edge, we've passed the intersection
        // }

        double laneLength = mobility->getCommandInterface()->lane(myLaneId).getLength();
        double currentPos = mobility->getCommandInterface()->vehicle(myId).getLanePosition();
        
        // Return distance to the end of the lane
        return std::max(0.0, laneLength - currentPos);
    } catch (...) {
        // Fallback or if vehicle is not yet placed on a valid edge
        return 1e10;
    }
}

bool V2VProxyModule::isApproachingIntersection()
{
    double distance = getDistanceToIntersection();
    return distance < stopDistance && distance > 0;
}

void V2VProxyModule::stopVehicle()
{
    if (!isStopped && mobility && mobility->getVehicleCommandInterface()) {
        mobility->getVehicleCommandInterface()->setSpeedMode(0);
        mobility->getVehicleCommandInterface()->setSpeed(0);
       
        isStopped = true;
        discoverLane();
        std::cout << "[V2VProxy " << replicaId << "] Vehicle STOPPED at intersection (distance=" << getDistanceToIntersection() << "m)" << std::endl;
    } else {
        std::cout << "[V2VProxy " << replicaId << "] Vehicle already stopped" << std::endl;
        isStopped = true;
        discoverLane();

    }
}
void V2VProxyModule::resumeVehicle(double delaySeconds)
{
    // This method is called from JNI (Java thread), so we MUST schedule
    std::lock_guard<std::mutex> lock(jniMutex);
    if (delaySeconds >= 99999.0) {
        //not in final decision
        return;
    
    }
    pendingResumeDelays.push(delaySeconds);
    std::cout << "[RESUME] Replica " << replicaId << ": JNI received GO signal with delay=" << delaySeconds 
              << "s. Queued for main thread (queue size=" << pendingResumeDelays.size() << ")" << std::endl;
    std::cout << "[RESUME] Replica " << replicaId << ": WARNING - This will trigger shouldFlush on next self-message!" << std::endl;
    // any OMNeT++ operations on the simulation thread via a self-message
 
}

void V2VProxyModule::resetForNextRound() {
    std::cout << "[V2VProxy " << replicaId << "] resetForNextRound triggered" << std::endl;
    
    // Reset basic flags for a new round
    joinTriggered = false;
    orderBagProposed = false;
    orderCollectionActive = false;
    myReadyQCComplete = false;
    orderDecisionReceived = false;
    currentEpoch++;         
    establishedView.clear();
    verifiedPool.clear();

    // The cars waiting for clearing need to transition here securely on OMNeT++ thread.
    // This includes WAIT cars that were in ORDER_CONSENSUS AND background cars (2nd/3rd
    // in queue) that were in IDLE because they never reached the stop line to propose.
    if (currentPhase != EXECUTING && currentPhase != DEPARTED) {
        currentPhase = WAITING_FOR_CLEARANCE;
        isWaitingForClearance = true;
        clearanceStartTime = simTime();
    }

    // Attempt to start a new round immediately if we're not waiting for clearance
    if (currentPhase == IDLE || currentPhase == EXECUTING) {
        // ... (Do not alter IDLE / EXECUTING state right now, wait for `checkPositionTimer`)
    }
    
    // Reset other flags
    hasProposedOrder = false;
    viewEstablished = false;
    phase2Pending = false;
    hasRequestedCrossing = false;
    logged100m = false;
    waitingForConsensus = false;

    // Reset ORDER bag collection state
    orderBagCloseFlag = false;
    orderBagRetransmitCount = 0;
    alreadyAtStopLine = false;

   



    // Cancel order-collection timers
    if (orderCollectDeadlineTimer && orderCollectDeadlineTimer->isScheduled()) {
        cancelEvent(orderCollectDeadlineTimer);
    }
    if (orderGossipRetransmitTimer && orderGossipRetransmitTimer->isScheduled()) {
        cancelEvent(orderGossipRetransmitTimer);
    }
    if (orderBagRetransmitTimer && orderBagRetransmitTimer->isScheduled()) {
        cancelEvent(orderBagRetransmitTimer);
    }

    verifiedPool.clear();
    establishedView.clear();
    collectedWitnesses.clear();
    arrivalAnnouncementsReceived.clear();

    viewVotes.clear();
    shouldFlush = false;
    pendingReconfigFlush = false;

    flushReliabilityQueue();

    // Release control back to SUMO

    // if (distanceToStopLine > 5.0) {
    //     // We are far away, creep forward at 2 m/s
    //     mobility->getVehicleCommandInterface()->setSpeed(0.0);
    // } else if (distanceToStopLine > 0.5) {
    //     // We are very close, apply the brakes to stop in 2 seconds
    //     mobility->getVehicleCommandInterface()->slowDown(0.0, 2.0); // (targetSpeed, timeToAchieveInSeconds)
    // } else {
    //     // We are AT the line. Full stop. DO NOT USE -1.
    //     mobility->getVehicleCommandInterface()->setSpeed(0.0);
    // }

    // CRITICAL: Reschedule checkPositionTimer
    if (checkPositionTimer && !checkPositionTimer->isScheduled()) {
        scheduleAt(simTime() + 0.1, checkPositionTimer);
        std::cout << "[RESET] Replica " << replicaId << ": Rescheduled checkPositionTimer for clearance/departure checks" << std::endl;
    }
}


// ============================================================================
// READYQC MANAGEMENT
// ============================================================================

V2VProxyModule::VerificationResult V2VProxyModule::verifyCarPosition(const std::string& carId,
    const std::string& claimedLane,
    double claimedPosition,
    double tolerance) {
    // mobility comes from DemoBaseApplLayer (veins) - this node's TraCIMobility
    if (!mobility) {
        return {false, "NO_TRACI"};
    }
    TraCICommandInterface* traci = mobility->getCommandInterface();
    if (!traci) {
        return {false, "NO_TRACI"};
    }
    // Check vehicle exists (vehicle(id) returns by value, no null)
    std::list<std::string> ids = traci->getVehicleIds();
    if (std::find(ids.begin(), ids.end(), carId) == ids.end()) {
        return {false, "NO_VEHICLE"};
    }
    // vehicle(carId) returns Vehicle by value (TraCICommandInterface::Vehicle)
    TraCICommandInterface::Vehicle targetVeh = traci->vehicle(carId);
    std::string actualLane = targetVeh.getLaneId();
    double actualPosition = targetVeh.getLanePosition();

    if (actualLane != claimedLane) {
        return {false, "WRONG_LANE"};
    }
    if (std::abs(actualPosition - claimedPosition) > tolerance) {
        return {false, "WRONG_POSITION"};
    }
    return {true, "OK"};
}

// TraCI Vehicle::getLeader(distance) returns (leaderId, distanceToLeader).
// If leaderId is empty, there is no vehicle ahead within that distance -> car is at front of lane.
bool V2VProxyModule::verifyNoLeaderInLane(const std::string& carId, const std::string& laneId) {
    if (!mobility) return false;
    TraCICommandInterface* traci = mobility->getCommandInterface();
    if (!traci) return false;
    std::list<std::string> ids = traci->getVehicleIds();
    if (std::find(ids.begin(), ids.end(), carId) == ids.end()) return false;

    TraCICommandInterface::Vehicle targetVeh = traci->vehicle(carId);
    std::pair<std::string, double> leader = targetVeh.getLeader(100.0);

    if (leader.first.empty()) {
        return true;  // No vehicle ahead within 100m -> at front of lane
    }
    // Leader exists: check if in same lane (then car is NOT at front)
    TraCICommandInterface::Vehicle leaderVeh = traci->vehicle(leader.first);
    std::string leaderLane = leaderVeh.getLaneId();
    return (leaderLane != laneId);  // at front of our lane if leader is in a different lane
}

std::vector<uint8_t> V2VProxyModule::signArrivalClaim(const ArrivalAnnouncement& announcement) {
    std::string data = announcement.carId + ":" + announcement.laneId + ":" +
                       std::to_string(announcement.positionInLane) + ":" +
                       std::to_string(announcement.claimedArrivalTime) + ":" +
                       std::to_string(announcement.epoch);
    int32_t hash = computeXXHash32(data);
    std::vector<uint8_t> sig(sizeof(int32_t));
    std::memcpy(sig.data(), &hash, sizeof(int32_t));
    return sig;
}


std::vector<uint8_t> V2VProxyModule::signWitnessClaim(const ArrivalAnnouncement& ann, double witnessTime, int witnessId) {
    // Debug logging (first 20 signatures) - BEFORE formatting
    static int callCount = 0;
    if (++callCount <= 20) {
        std::cout << "[SIGN_RAW] Witness " << witnessId << " RAW values: pos=" << std::setprecision(17) << ann.positionInLane 
                  << ", arrival=" << ann.claimedArrivalTime << ", witnessTime=" << witnessTime << std::endl;
    }
    
    // CRITICAL: Format doubles with EXACTLY 6 decimal places for consistency!
    char posBuf[32], arrivalBuf[32], timestampBuf[32];
    std::snprintf(posBuf, sizeof(posBuf), "%.6f", ann.positionInLane);
    std::snprintf(arrivalBuf, sizeof(arrivalBuf), "%.6f", ann.claimedArrivalTime);
    std::snprintf(timestampBuf, sizeof(timestampBuf), "%.6f", witnessTime);
    
    std::string data = ann.carId + ":" + ann.laneId + ":" +
                      std::string(posBuf) + ":" +
                      std::string(arrivalBuf) + ":" +
                      std::to_string(ann.epoch) + ":" +
                      std::string(timestampBuf) + ":" +
                      std::to_string(witnessId);

    int32_t hash = computeXXHash32(data);

    // Debug logging (first 20 signatures)
    if (callCount <= 20) {
        std::cout << "[SIGN_C++] Witness " << witnessId << " signing: \"" << data << "\"" << std::endl;
        std::cout << "[SIGN_C++] XXHash32 result: " << hash << std::endl;
        std::cout << "[SIGN_C++] Bytes (little-endian): [";
        uint8_t* hashBytes = reinterpret_cast<uint8_t*>(&hash);
        for (int i = 0; i < 4; i++) {
            if (i > 0) std::cout << ", ";
            std::cout << (int)hashBytes[i];
        }
        std::cout << "]" << std::endl;
    }

    std::vector<uint8_t> sig(sizeof(int32_t));
    std::memcpy(sig.data(), &hash, sizeof(int32_t));
    return sig;
}


void V2VProxyModule::broadcastArrivalAnnouncement() {
   
    // ZOMBIE FILTER: Departed cars don't broadcast arrival announcements
    V2VProxyModule::zombieFilter();
    std::string myCarId = "veh" + std::to_string(replicaId);
    ArrivalAnnouncement announcement;
    announcement.carId = myCarId;
    // Get lane info from TraCI Vehicle interface
    if (!mobility) {
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: No mobility for broadcastArrivalAnnouncement" << std::endl;
        return;
    }
    TraCICommandInterface* traci = mobility->getCommandInterface();
    if (!traci) {
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: No TraCI interface" << std::endl;
        return;
    }
    TraCICommandInterface::Vehicle myVeh = traci->vehicle(myCarId);
    announcement.laneId = myVeh.getLaneId();
    announcement.positionInLane = myVeh.getLanePosition();
    announcement.claimedArrivalTime = simTime().dbl();
    announcement.epoch = currentEpoch;
    announcement.signature = signArrivalClaim(announcement);

    // CRITICAL: Serialize then deserialize to ensure we store the same precision as witnesses will see!
    std::vector<uint8_t> payload = serializeArrivalAnnouncement(announcement);

    // Create a mock BFTMessage for deserialization
    BFTMessage* mockMsg = new BFTMessage();
    mockMsg->setPayloadArraySize(payload.size());
    for (size_t i = 0; i < payload.size(); i++) {
        mockMsg->setPayload(i, payload[i]);
    }
    ArrivalAnnouncement canonicalAnn = deserializeArrivalAnnouncement(mockMsg);
    delete mockMsg;

    // Store the canonical (serialized/deserialized) version
    pendingAnnouncements[myCarId] = canonicalAnn;

    sendBFTMessage(replicaId, -1, payload, 1);  // Broadcast, type=1;
    std::cout << "[ANN-BROADCAST] Replica " << replicaId << " (" << myCarId << ") broadcast arrival announcement at t=" << simTime() << std::endl;
}


void V2VProxyModule::handleArrivalAnnouncement(BFTMessage* bftMsg) {
    ArrivalAnnouncement ann = deserializeArrivalAnnouncement(bftMsg);
    VerificationResult result = verifyCarPosition(ann.carId, ann.laneId, ann.positionInLane);
    if (!result.isValid) {
        std::cout << "[ANN-RECV] Replica " << replicaId << " INVALID announcement from " << ann.carId << ": " << result.reason << std::endl;
        return;
    }
    arrivalAnnouncementsReceived.insert(ann.carId);
    size_t n = arrivalAnnouncementsReceived.size();
    size_t expected = establishedView.empty() ? 8 : establishedView.size();
    std::cout << "[ANN-RECV] Replica " << replicaId << " received announcement FROM " << ann.carId << " at t=" << simTime()
              << " (has " << n << "/" << expected << ": ";
    for (const auto& c : arrivalAnnouncementsReceived) std::cout << c << " ";
    std::cout << ")" << std::endl;

    WitnessResponse witness;
    witness.targetCarId = ann.carId;
    witness.witnessReplicaId = replicaId;
    witness.verified = true;
    witness.witnessTimestamp = simTime().dbl();
    witness.signature = signWitnessClaim(ann, witness.witnessTimestamp, replicaId);

    int targetReplicaId = this->extractReplicaIdFromCarId(ann.carId);
    std::vector<uint8_t> payload = serializeWitnessResponse(witness);
    sendBFTMessage(replicaId, targetReplicaId, payload, 2);
    std::cout << "[WITNESS-SEND] Replica " << replicaId << " sent witness response TO " << ann.carId << " (replica " << targetReplicaId << ") at t=" << simTime() << std::endl;
}


void V2VProxyModule::handleWitnessResponse(BFTMessage* bftMsg) {
    WitnessResponse witness = deserializeWitnessResponse(bftMsg);
    std::string myCarId = "veh" + std::to_string(replicaId);

    if (witness.targetCarId != myCarId) {
        std::cout << "[WITNESS-RECV] Replica " << replicaId << " IGNORING witness (meant for " << witness.targetCarId << ", not " << myCarId << ") from replica " << witness.witnessReplicaId << std::endl;
        return;
    }

    // Guard: view must be established before we can compute f
    if (establishedView.empty()) {
        std::cout << "[WITNESS-RECV] Replica " << replicaId << " view not yet established, dropping witness from replica " << witness.witnessReplicaId << std::endl;
        return;
    }

    // Dedup: ignore retransmitted witnesses from the same replica
    for (const auto& w : collectedWitnesses[myCarId]) {
        if (w.witnessReplicaId == witness.witnessReplicaId) {
            std::cout << "[WITNESS-RECV] Replica " << replicaId << " DEDUP: ignoring duplicate witness from replica " << witness.witnessReplicaId << std::endl;
            return;
        }
    }

    collectedWitnesses[myCarId].push_back(witness);
    size_t total = collectedWitnesses[myCarId].size();
    int localviewBatchSize = (int)establishedView.size();
    int f = (localviewBatchSize - 1) / 3;
    int required = f + 1;
    std::cout << "localviewBatchSize: " << localviewBatchSize << std::endl;
    std::cout << "f: " << f << std::endl;
    std::cout << "required: " << required << std::endl;
    std::cout << "[WITNESS-RECV] Replica " << replicaId << " ACCEPTED witness from replica " << witness.witnessReplicaId
              << " for " << myCarId << " (have " << total << "/" << required << " witnesses) at t=" << simTime() << std::endl;


    // Only trigger ONCE when we first reach f+1 witnesses
    if (collectedWitnesses[myCarId].size() == required) {
        std::cout << "[V2VProxy " << replicaId << "] *** ReadyQC COMPLETE! *** "
                  << "(" << required << "/" << required << " witnesses)" << std::endl;

        if (readyQCTimeoutTimer && readyQCTimeoutTimer->isScheduled()) {
            cancelEvent(readyQCTimeoutTimer);
        }
        
        orderSignatureCollectionEndTime = simTime(); //we are done collecting order signatures

        // NEW FLOW: Stay in COLLECTING_QC phase (view already agreed in Phase 1)
        // Don't change phase here - we'll move to ORDER_CONSENSUS when all cars have ReadyQCs
        
      assembleAndBroadcastReadyQC();
    }
}


void V2VProxyModule::assembleAndBroadcastReadyQC() {
    // ZOMBIE FILTER: Departed cars don't assemble and broadcast ReadyQCs
   V2VProxyModule::zombieFilter();
    std::string myCarId = "veh" + std::to_string(replicaId);
    
    // Check if pendingAnnouncements exists - if not, populate from current TraCI state
    if (pendingAnnouncements.find(myCarId) == pendingAnnouncements.end() || 
        pendingAnnouncements[myCarId].laneId.empty()) {
        std::cout << "[ASSEMBLE_QC] Replica " << replicaId << " WARNING: pendingAnnouncements[" << myCarId 
                  << "] missing or empty! Populating from current TraCI state..." << std::endl;
        
        if (!mobility || !mobility->getCommandInterface()) {
            std::cerr << "[ASSEMBLE_QC] ERROR: Cannot populate - mobility or TraCI unavailable!" << std::endl;
            return;
        }
        
        TraCICommandInterface* traci = mobility->getCommandInterface();
        TraCICommandInterface::Vehicle myVeh = traci->vehicle(myCarId);
        pendingAnnouncements[myCarId].carId = myCarId;
        pendingAnnouncements[myCarId].laneId = myVeh.getLaneId();
        pendingAnnouncements[myCarId].positionInLane = myVeh.getLanePosition();
        pendingAnnouncements[myCarId].claimedArrivalTime = simTime().dbl();
        pendingAnnouncements[myCarId].epoch = currentEpoch;
        pendingAnnouncements[myCarId].signature = signArrivalClaim(pendingAnnouncements[myCarId]);
        
        std::cout << "[ASSEMBLE_QC] Populated from TraCI: lane=" << pendingAnnouncements[myCarId].laneId
                  << ", pos=" << pendingAnnouncements[myCarId].positionInLane << std::endl;
    }
    
    ReadyQC qc;
    qc.carId = myCarId;
    qc.laneId = pendingAnnouncements[myCarId].laneId;
    qc.positionInLane = pendingAnnouncements[myCarId].positionInLane;
    qc.verifiedArrival = pendingAnnouncements[myCarId].claimedArrivalTime;
    qc.epoch = pendingAnnouncements[myCarId].epoch;

    std::cout << "[ASSEMBLE_QC] Replica " << replicaId << " assembling ReadyQC for " << myCarId << std::endl;
    std::cout << "[ASSEMBLE_QC] QC data: carId=" << qc.carId << ", lane=" << qc.laneId 
              << ", pos=" << qc.positionInLane << ", arrival=" << qc.verifiedArrival << std::endl;

    for (const auto& witness : collectedWitnesses[myCarId]) {
        WitnessSignature sig;
        sig.witnessReplicaId = witness.witnessReplicaId;
        sig.signature = witness.signature;
        sig.witnessTimestamp = witness.witnessTimestamp;
        
        std::cout << "[ASSEMBLE_QC] Adding witness " << sig.witnessReplicaId 
                  << " timestamp=" << sig.witnessTimestamp 
                  << " sigBytes=[";
        for (size_t i = 0; i < sig.signature.size() && i < 4; i++) {
            if (i > 0) std::cout << ", ";
            std::cout << (int)sig.signature[i];
        }
        std::cout << "]" << std::endl;
        
        // VERIFY IMMEDIATELY on C++ side!
        // Debug RAW values BEFORE formatting
        std::cout << "[VERIFY_RAW] Witness " << sig.witnessReplicaId << " RAW values: qc.pos=" << std::setprecision(17) << qc.positionInLane 
                  << ", qc.arrival=" << qc.verifiedArrival << ", sig.witnessTime=" << sig.witnessTimestamp << std::endl;
        
        // CRITICAL: Must format doubles with EXACTLY 6 decimal places to match signing!
        char posBuf[32], arrivalBuf[32], timestampBuf[32];
        std::snprintf(posBuf, sizeof(posBuf), "%.6f", qc.positionInLane);
        std::snprintf(arrivalBuf, sizeof(arrivalBuf), "%.6f", qc.verifiedArrival);
        std::snprintf(timestampBuf, sizeof(timestampBuf), "%.6f", sig.witnessTimestamp);
        
        std::string reconstructedData = qc.carId + ":" + qc.laneId + ":" +
                                       std::string(posBuf) + ":" +
                                       std::string(arrivalBuf) + ":" +
                                       std::to_string(qc.epoch) + ":" +
                                       std::string(timestampBuf) + ":" +
                                       std::to_string(sig.witnessReplicaId);
        int32_t expectedHash = computeXXHash32(reconstructedData);
        
        int32_t actualHash;
        if (sig.signature.size() < sizeof(int32_t)) {
            std::cerr << "[VERIFY_C++] ERROR: Signature too small! size=" << sig.signature.size()  << ", expected at least " << sizeof(int32_t) << " bytes" << std::endl;
            continue;
        }
        std::cout << "Memcpying actual hash" << std::endl;
        
        std::memcpy(&actualHash, sig.signature.data(), sizeof(int32_t));
        
        std::cout << "[VERIFY_C++] Data: \"" << reconstructedData << "\"" << std::endl;
        std::cout << "[VERIFY_C++] Expected: " << expectedHash << ", Actual: " << actualHash 
                  << ", Match: " << (expectedHash == actualHash ? "true" : "FALSE!!!") << std::endl;
        
        qc.signatures.push_back(sig);
    }

    verifiedPool[myCarId] = qc;

    // Broadcast ReadyQC on V2V so other cars can store it locally
    std::vector<uint8_t> payload = serializeReadyQC(qc);
    sendBFTMessage(replicaId, -1, payload, 3);
    std::cout << "[V2VProxy " << replicaId << "] *** BROADCASTED ReadyQC for " << myCarId << " ***" << std::endl;

    myReadyQCComplete = true;

    if (isCarAtFrontOfLane(myCarId, qc.laneId)) {
        std::cout << "[BUILD_ORDER_BAG_FINAL] car " << myCarId << " is at front of lane " << qc.laneId << ", starting order collection window" << std::endl;
        startOrderCollectionWindowIfNeeded();
    }
 
    // Propose to BFT immediately. BFT batches ORDER_PROPOSE from all cars; Java decides
    // when it has ReadyQCs for all cars in agreedView (across batches).
    // triggerOrderConsensus();

    // if (!hasProposedOrder) {
    //     hasProposedOrder = true;
    //     triggerOrderConsensus();
    // }
}

int V2VProxyModule::countDistinctFrontLanesInPool() {
    std::unordered_map<std::string, double> bestPosByLane;

    for (const auto& kv : verifiedPool) {
        const ReadyQC& qc = kv.second;
        if (qc.epoch != currentEpoch) continue;
        if (!establishedView.empty() && establishedView.count(qc.carId) == 0) continue;

        if (!isCarAtFrontOfLane(qc.carId, qc.laneId)) {
            continue;
        }
        auto it = bestPosByLane.find(qc.laneId);
        if (it == bestPosByLane.end() || qc.positionInLane > it->second) {
            bestPosByLane[qc.laneId] = qc.positionInLane;
        }
    }
    
    std::string bestPosByLaneStr = "";
    for (const auto& kv : bestPosByLane) {
        bestPosByLaneStr += kv.first + ":" + std::to_string(kv.second) + ",";
    }
    std::cout << "[COUNT_DISTINCT_FRONT_LANES] Best positions by lane: " << bestPosByLaneStr << std::endl;
    return (int)bestPosByLane.size();
}

void V2VProxyModule::startOrderCollectionWindowIfNeeded() {
    if (orderCollectionActive || orderDecisionReceived) return;

    orderCollectionActive = true;
    orderBagProposed = false;
    orderBagRetransmitCount = 0;
    orderCollectionDeadline = simTime() + SimTime(2.0); // 1 second sim time

    // schedule deadline
    scheduleAt(orderCollectionDeadline, orderCollectDeadlineTimer);

    // optional: schedule a few extra ReadyQC re-broadcasts with jitter
    
    scheduleAt(simTime() + uniform(0.01, 0.04), orderGossipRetransmitTimer);

    std::cout << "[ORDER-COLLECT] Replica " << replicaId
       << " started collection window until " << orderCollectionDeadline << "\n";

    // Early close if we already have enough lane fronts
    if (countDistinctFrontLanesInPool() >= 4) {
       
        proposeOrderBagNow("EARLY_4_LANES");
    }
}
// Pick one front-most QC per lane from verifiedPool for current epoch/view
std::vector<V2VProxyModule::ReadyQC> V2VProxyModule::buildOrderBagQCs() {
    std::unordered_map<std::string, ReadyQC> frontByLane;
    std::string myCarId = "veh" + std::to_string(replicaId);

    for (const auto& [carId, qc] : verifiedPool) {
        // Filter: must be in established view
        if (!establishedView.empty() && establishedView.count(qc.carId) == 0) continue;

        // Filter: current epoch only (and later add viewHash/viewId match too)
        if (qc.epoch != currentEpoch) continue;

        // Optional: validate QC signatures before using
        // if (!validateReadyQC(qc)) continue;
        if (!isCarAtFrontOfLane(qc.carId, qc.laneId)) {
            continue; 
        }

        auto it = frontByLane.find(qc.laneId);
        if (it == frontByLane.end()) {
            frontByLane[qc.laneId] = qc;
        } else {
            const ReadyQC& cur = it->second;

            // IMPORTANT: larger positionInLane = closer to intersection (your setup)
            bool qcIsMoreFront = (qc.positionInLane > cur.positionInLane);

            // Stable tie-breakers (deterministic)
            bool tieBreak =
                (std::abs(qc.positionInLane - cur.positionInLane) < 1e-6 &&
                 (qc.verifiedArrival < cur.verifiedArrival ||
                  (std::abs(qc.verifiedArrival - cur.verifiedArrival) < 1e-6 &&
                   qc.carId < cur.carId)));

            if (qcIsMoreFront || tieBreak) {
                frontByLane[qc.laneId] = qc;
            }
        }
    }

    std::vector<ReadyQC> bag;
    bag.reserve(frontByLane.size());
    for (auto& [lane, qc] : frontByLane) {
        bag.push_back(qc);
    }

    for (const auto& [lane, qc] : frontByLane) {
        std::cout << "[BUILD_ORDER_BAG_FINAL] lane=" << lane
                  << " car=" << qc.carId
                  << " pos=" << std::setprecision(15) << qc.positionInLane
                  << " arr=" << qc.verifiedArrival
                  << " epoch=" << qc.epoch << std::endl;
    }

    // Deterministic ordering of bag entries
    std::sort(bag.begin(), bag.end(), [](const ReadyQC& a, const ReadyQC& b) {
        if (a.laneId != b.laneId) return a.laneId < b.laneId;
        if (std::abs(a.positionInLane - b.positionInLane) > 1e-6) return a.positionInLane > b.positionInLane; // front first
        if (std::abs(a.verifiedArrival - b.verifiedArrival) > 1e-6) return a.verifiedArrival < b.verifiedArrival;
        return a.carId < b.carId;
    });

    return bag;
}

void V2VProxyModule::proposeOrderBagNow(const std::string& reason) {
    if (orderDecisionReceived) return;

    auto bag = buildOrderBagQCs();
    if (bag.empty()) {
        std::cout << "[ORDER-BAG] Replica " << replicaId << " no QCs to propose (" << reason << ")\n";
        return;
    }

    // closeFlag=true means Java should decide even partial candidates (used at deadline)
    bool closeFlag = (reason == "DEADLINE");
    orderBagCloseFlag = closeFlag;

    if (!orderBagProposed) {
        // First proposal — record ORDER consensus start time and set phase
        orderConsensusStartTime = simTime();
        realOrderConsensusStart = std::chrono::high_resolution_clock::now();
        currentPhase = ORDER_CONSENSUS;
        std::cout << "[METRICS " << replicaId << "] Order_Consensus_Start: " << orderConsensusStartTime << std::endl;
    }

    std::string payload = serializeOrderBagRequest(bag, closeFlag);
    std::cout << "[ORDER-BAG] Replica " << replicaId << " proposing bag size=" << bag.size()
              << " reason=" << reason << " closeFlag=" << closeFlag << " at t=" << simTime() << std::endl;

    triggerJoinViaJNI(payload);

    orderBagProposed = true;

    // Retransmit until order decision arrives (duplicates are de-duped by Java)
    if (!orderBagRetransmitTimer->isScheduled()) {
        scheduleAt(simTime() + uniform(0.05, 0.10), orderBagRetransmitTimer);
    }
}

std::string V2VProxyModule::serializeOrderBagRequest(const std::vector<ReadyQC>& bag, bool closeFlag) {
    // Compute a viewHash from the sorted established view so Java can verify epoch/view binding
    std::string viewStr;
    for (const auto& car : establishedView) {  // std::set is always sorted
        if (!viewStr.empty()) viewStr += ",";
        viewStr += car;
    }
    int32_t viewHashVal = computeXXHash32(viewStr);

    std::ostringstream ss;
    ss << "ORDER_PROPOSE:";
    ss << currentEpoch << ":";
    ss << viewHashVal << ":";
    ss << (closeFlag ? "true" : "false") << ":";

    for (size_t i = 0; i < bag.size(); i++) {
        if (i > 0) ss << "||";
        ss << serializeReadyQCForJava(bag[i]);
    }

    return ss.str();
}

std::string V2VProxyModule::serializeReadyQCForJava(const ReadyQC& qc) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(6);

    // fixed fields
    ss << qc.carId << ":"
       << qc.laneId << ":"
       << qc.positionInLane << ":"
       << qc.verifiedArrival << ":"
       << qc.epoch;

    // signatures
    if (!qc.signatures.empty()) {
        ss << ":";
        for (size_t i = 0; i < qc.signatures.size(); i++) {
            if (i > 0) ss << "|";

            // convert first 4 bytes signature -> int32 (same way Java expects decimal hash)
            int32_t hash = 0;
            if (qc.signatures[i].signature.size() >= sizeof(int32_t)) {
                std::memcpy(&hash, qc.signatures[i].signature.data(), sizeof(int32_t));
            }

            ss << qc.signatures[i].witnessReplicaId << ","
               << qc.signatures[i].witnessTimestamp << ","
               << hash;
        }
    }

    return ss.str();
}


void V2VProxyModule::triggerOrderConsensus() {
    // Mark the start of Order consensus
    
    
    
    orderConsensusStartTime = simTime();
    std::cout << "[METRICS " << replicaId << "] Order_Consensus_Start: " << orderConsensusStartTime << std::endl;
    
    // View is already agreed - just trigger order consensus
    // All replicas have verifiedCars from VIEW phase
    // Java will check if our car is in agreedView
    std::string request = "ORDER_PROPOSE";
    std::string myCarId = "veh" + std::to_string(replicaId);

    // Check if verifiedPool has valid ReadyQC (should have been populated by assembleAndBroadcastReadyQC)
    if (verifiedPool.find(myCarId) == verifiedPool.end() || verifiedPool[myCarId].laneId.empty()) {
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: verifiedPool[" << myCarId 
                  << "] missing or empty! Cannot propose ORDER. Call assembleAndBroadcastReadyQC() first." << std::endl;
        return;
    }

    ReadyQC qc = verifiedPool[myCarId];
    std::string qcString = serializeReadyQCToString(qc);
    std::cout << "[V2VProxy " << replicaId << "] Serialized ReadyQC: " << qcString << std::endl;
    request += ":" + qcString;
    std::cout << "[V2VProxy " << replicaId << "] Request: " << request << std::endl;

    currentPhase = ORDER_CONSENSUS;
    triggerJoinViaJNI(request);

    std::cout << "[V2VProxy " << replicaId << "] Triggered ORDER consensus" << std::endl;
}

 

void V2VProxyModule::handlepreConsensusMessages(BFTMessage* bftMsg) {
    zombieFilter();
    if (replicaId < 0) {
        return;
    }

    
    int msgType = bftMsg->getMessageType();

    std::cout << "[V2V-DISPATCH] Replica " << replicaId << ": handlepreConsensusMessages at t=" << simTime() 
              << " msgType=" << msgType;
    
    switch (msgType) {
        case 0:  // BFT_CONSENSUS
            std::cout << " (BFT_CONSENSUS) - forwarding to handleBFTMessage -> Java" << std::endl;
            handleBFTMessage(bftMsg);
            break;
        case 1:  // ARRIVAL_ANNOUNCE
            std::cout << " (ARRIVAL_ANNOUNCE)" << std::endl;
            handleArrivalAnnouncement(bftMsg);
            break;
        case 2:  // WITNESS_RESPONSE
            std::cout << " (WITNESS_RESPONSE)" << std::endl;
            handleWitnessResponse(bftMsg);
            break;
        case 3:  // READYQC_COMPLETE
            std::cout << " (READYQC_COMPLETE)" << std::endl;
            handleReadyQCComplete(bftMsg);
            break;
        case 4:  // VIEW_PROPOSAL (Phase 1b - V2V agreement)
            std::cout << " (VIEW_PROPOSAL)" << std::endl;
            handleViewProposal(bftMsg);
            break;
        case 5:  // VIEW_AGREEMENT (Phase 1b - V2V signatures)
            std::cout << " (VIEW_AGREEMENT)" << std::endl;
            handleViewAgreement(bftMsg);
            break;
        default:
            std::cout << " (UNKNOWN)" << std::endl;
            EV_WARN << "Unknown message type: " << msgType << "\n";
    }
    delete bftMsg;
}


void V2VProxyModule::handleOrderDecision(const std::string& orderDecision) {
    std::cout << "[V2VProxy " << replicaId << "] handleOrderDecision: " << orderDecision << std::endl;

    // Stop retransmit timer — decision is in
    orderDecisionReceived = true;

    parseAndNotifyDecision(orderDecision);
}

std::vector<std::string> split(const std::string& s, char delimiter) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(s);
    while (std::getline(tokenStream, token, delimiter)) {
        tokens.push_back(token);
    }
    return tokens;
}


void V2VProxyModule::parseAndNotifyDecision(const std::string& decision) {
    std::cout << "[V2VProxy " << replicaId << "] parseAndNotifyDecision: " << decision << std::endl;

    // Decision format: "veh4:POS:0;veh5:POS:1;veh0:POS:2;veh1:POS:3"
    // Split by ';' to get individual car entries
    std::vector<std::string> carEntries = split(decision, ';');

    std::string myCarId = "veh" + std::to_string(replicaId);

    // Clear previous tracking
    expectedToGo.clear();
    confirmedDeparted.clear();
    bool foundMyself = false;
    int myPosition = -1;

    for (const std::string& entry : carEntries) {
        std::vector<std::string> parts = split(entry, ':');
        if (parts.size() >= 2) {
            std::string carId = parts[0];
            std::string action = parts[1];  // "GO" or "WAIT"

            if (action == "GO") {

                expectedToGo.insert(carId);  // Track expected departures
                if (carId == myCarId) {
                    foundMyself = true;
                    myPosition = std::stoi(parts[2]);
                }
            }
        }
    }


    if (foundMyself) {
         // Calculate delay based on position
        // Position 0 goes immediately, position 1 waits for car 0 to clear, etc.
        // Example: position 0 → 0s, position 1 → 2s, position 2 → 4s, position 3 → 6s
        double delaySeconds = myPosition;  // 1.5 seconds between cars

        std::cout << "[V2VProxy " << replicaId << "] Resuming vehicle in " << delaySeconds << " seconds" << std::endl;
        resumeVehicle(delaySeconds);

        return;
    }

    // We use a much safer approach than getRoadId (which crashes SUMO with 0xb4)
    // If ANY car in expectedToGo was in our discovered laneQueue (within 100m)
    // then our lane has a goer.
    bool myLaneHasGoer = false;
    std::string laneGoer = "";
    
    for (const auto& goingCar : expectedToGo) {
        if (std::find(laneQueue.begin(), laneQueue.end(), goingCar) != laneQueue.end()) {
            myLaneHasGoer = true;
            laneGoer = goingCar;
            break;
        }
    }

    // What if the front car is > 100m away and not in laneQueue?
    // As a trailing car, if we are in WAITING_FOR_CLEARANCE or IDLE but have a `carAhead`, 
    // we can just check if ANY car ahead of us (via TraCI getLeader if available, or just letting
    // Krauss physics accordion us up) is moving. 
    // Since `laneQueue` has a 100m radius, cars 3+ should still accordion up because of `Rule 3`
    // ahead_speed checks instead of needing to explicitly trigger `myLaneTriggerCar`.
    
    if (myLaneHasGoer) {
        std::cout << "[V2VProxy " << replicaId << "] My lane (road) has a GOer: " << laneGoer << std::endl;
        myLaneTriggerCar = laneGoer;
    }
    // Verify we got a valid position
    if (!foundMyself) {
        std::cout << "[V2VProxy " << replicaId << "] No valid GO position assigned (will wait for next round)" << std::endl;
        // Flag main thread to call resetForNextRound (cannot call OMNeT++ scheduleAt from JNI thread)
        scheduleReconfigFlush();
    }
    return;
    

   

}
// ============================================================================
// HELPER FUNCTIONS
// ============================================================================


void V2VProxyModule::discoverLane() {
    if (laneDiscovered) return;
 
    auto* traci = mobility->getVehicleCommandInterface();
    TraCICommandInterface* traciCmd = mobility->getCommandInterface();
    if (!traci) return;
    std::string myId = "veh" + std::to_string(replicaId);
    myLaneId = traciCmd->vehicle(myId).getLaneId();
    double mypos = traciCmd->vehicle(myId).getLanePosition();

    std::vector<std::pair<double, std::string>> inLane;
    for (const auto& otherId : traciCmd->getVehicleIds()) {
        if (otherId == myId) continue;
        auto v = traciCmd->vehicle(otherId);
        if (v.getLaneId() == myLaneId) {
            inLane.push_back({v.getLanePosition(), otherId});

        }
    }


    std::sort(inLane.begin(), inLane.end());

    laneQueue.clear();
    for (auto &[pos, id] : inLane){
        laneQueue.push_back(id);

    }

    carAhead ="";
    carAheadStopPos = -1.0;
    for (auto& [pos, id] : inLane){
        if (pos > mypos) {
            carAhead = id;
            carAheadStopPos = pos;
            break;
        }
        
    }
    laneDiscovered = true;
    std::cout << "[V2VProxy " << replicaId << "] Lane discovered: " << myLaneId << std::endl;
    std::cout << "[V2VProxy " << replicaId << "] Car ahead: " << carAhead << std::endl;
    std::cout << "[V2VProxy " << replicaId << "] Car ahead stop pos: " << carAheadStopPos << std::endl;

}

int V2VProxyModule::extractReplicaIdFromCarId(const std::string& carId) {
    // "veh0" → 0, "veh1" → 1, etc.
    if (carId.substr(0, 3) == "veh") {
        return std::stoi(carId.substr(3));
    }
    return -1;
}

std::string signatureBytesToString(const std::vector<uint8_t>& sigBytes) {
    // Mock signatures are 4-byte int32_t hash - just convert to decimal string
    if (sigBytes.size() != sizeof(int32_t)) {
        return "0";
    }

    int32_t hashValue;
    std::memcpy(&hashValue, sigBytes.data(), sizeof(int32_t));
    return std::to_string(hashValue);
}

std::vector<uint8_t> stringToSignatureBytes(const std::string& sigStr) {
    // Convert decimal string back to 8-byte size_t
    int32_t hashValue = std::stoi(sigStr);
    std::vector<uint8_t> bytes(sizeof(int32_t));
    std::memcpy(bytes.data(), &hashValue, sizeof(int32_t));
    return bytes;
}


void V2VProxyModule::scheduleReconfigFlush() {
    // Thread-safe flag update
    std::lock_guard<std::mutex> lock(jniMutex);
    pendingReconfigFlush = true;
    std::cout << "[JNI] Flagged pendingReconfigFlush=true for main thread." << std::endl;
}

// ============================================================================
// SERIALIZATION FUNCTIONS
// ============================================================================

// ============================================================================
// VIEW CONSENSUS SERIALIZATION (Phase 1)
// ============================================================================

std::vector<uint8_t> V2VProxyModule::serializeViewProposal(const ViewProposal& proposal) {
    // Format for V2V: proposerId|carList|sig1,sig2,sig3
    // carList is comma-separated sorted list
    std::stringstream ss;
    ss << proposal.proposerReplicaId << "|";
    
    // Sort cars for deterministic ordering
    std::vector<std::string> sortedCars(proposal.observedCars.begin(), proposal.observedCars.end());
    std::sort(sortedCars.begin(), sortedCars.end());
    
    for (size_t i = 0; i < sortedCars.size(); i++) {
        if (i > 0) ss << ",";
        ss << sortedCars[i];
    }
    
    ss << "|" << proposal.proposalTimestamp << "|" << proposal.signature.size() << "|";
    
    std::string header = ss.str();
    std::vector<uint8_t> result(header.begin(), header.end());
    result.insert(result.end(), proposal.signature.begin(), proposal.signature.end());
    
    return result;
}

V2VProxyModule::ViewProposal V2VProxyModule::deserializeViewProposal(BFTMessage* bftMsg) {
    std::vector<uint8_t> payload(bftMsg->getPayloadArraySize());
    for (size_t i = 0; i < payload.size(); i++) {
        payload[i] = bftMsg->getPayload(i);
    }
    
    std::string s(payload.begin(), payload.end());
    std::vector<std::string> parts = split(s, '|');
    
    ViewProposal proposal;
    if (parts.size() >= 4) {
        proposal.proposerReplicaId = std::stoi(parts[0]);
        
        // Parse comma-separated car list
        if (!parts[1].empty()) {
            std::vector<std::string> cars = split(parts[1], ',');
            proposal.observedCars.insert(cars.begin(), cars.end());
        }
        
        proposal.proposalTimestamp = std::stod(parts[2]);
        
        int siglen = std::stoi(parts[3]);
        size_t offset = s.find_last_of('|') + 1;
        
        if (offset < payload.size() && offset + siglen <= payload.size()) {
            proposal.signature.assign(payload.begin() + offset, payload.begin() + offset + siglen);
        }
    }
    
    return proposal;
}

std::vector<uint8_t> V2VProxyModule::serializeViewAgreement(const ViewAgreement& agreement) {
    // Format: agreingReplicaId|carList|siglen|sig
    std::stringstream ss;
    ss << agreement.agreingReplicaId << "|";
    
    // Sort cars for deterministic ordering
    std::vector<std::string> sortedCars(agreement.agreedView.begin(), agreement.agreedView.end());
    std::sort(sortedCars.begin(), sortedCars.end());
    
    for (size_t i = 0; i < sortedCars.size(); i++) {
        if (i > 0) ss << ",";
        ss << sortedCars[i];
    }
    
    ss << "|" << agreement.signature.size() << "|";
    
    std::string header = ss.str();
    std::vector<uint8_t> result(header.begin(), header.end());
    result.insert(result.end(), agreement.signature.begin(), agreement.signature.end());
    
    return result;
}

V2VProxyModule::ViewAgreement V2VProxyModule::deserializeViewAgreement(BFTMessage* bftMsg) {
    std::vector<uint8_t> payload(bftMsg->getPayloadArraySize());
    for (size_t i = 0; i < payload.size(); i++) {
        payload[i] = bftMsg->getPayload(i);
    }
    
    std::string s(payload.begin(), payload.end());
    std::vector<std::string> parts = split(s, '|');
    
    ViewAgreement agreement;
    if (parts.size() >= 3) {
        agreement.agreingReplicaId = std::stoi(parts[0]);
        
        // Parse comma-separated car list
        if (!parts[1].empty()) {
            std::vector<std::string> cars = split(parts[1], ',');
            agreement.agreedView.insert(cars.begin(), cars.end());
        }
        
        int siglen = std::stoi(parts[2]);
        size_t offset = s.find_last_of('|') + 1;
        
        if (offset < payload.size() && offset + siglen <= payload.size()) {
            agreement.signature.assign(payload.begin() + offset, payload.begin() + offset + siglen);
        }
    }
    
    return agreement;
}

// ============================================================================
// READYQC SERIALIZATION (Phase 2)
// ============================================================================

std::vector<uint8_t> V2VProxyModule::serializeArrivalAnnouncement(const ArrivalAnnouncement& ann) {
    // Format: carId|laneId|position|time|epoch|siglen|sig
    // CRITICAL: Use high precision to preserve exact double values!
    std::stringstream ss;
    ss << std::setprecision(17);  // Full double precision
    ss << ann.carId << "|" << ann.laneId << "|"
       << ann.positionInLane << "|" << ann.claimedArrivalTime << "|"
       << ann.epoch << "|" << ann.signature.size() << "|";

    std::string header = ss.str();
    std::vector<uint8_t> result(header.begin(), header.end());
    result.insert(result.end(), ann.signature.begin(), ann.signature.end());

    return result;
}

V2VProxyModule::ArrivalAnnouncement V2VProxyModule::deserializeArrivalAnnouncement(BFTMessage* bftMsg) {
    std::vector<uint8_t> payload(bftMsg->getPayloadArraySize());
    for (size_t i = 0; i < payload.size(); i++) {
        payload[i] = bftMsg->getPayload(i);
    }

    std::string s(payload.begin(), payload.end());
    std::vector<std::string> parts = split(s, '|');

    ArrivalAnnouncement ann;
    if (parts.size() >= 6) {
        ann.carId = parts[0];
        ann.laneId = parts[1];
        ann.positionInLane = std::stod(parts[2]);
        ann.claimedArrivalTime = std::stod(parts[3]);
        ann.epoch = std::stoi(parts[4]);

        int siglen = std::stoi(parts[5]);
        size_t offset = s.find_last_of('|') + 1;

        if (offset < payload.size() && offset + siglen <= payload.size()) { 
            ann.signature.assign(payload.begin() + offset, payload.begin() + offset + siglen);

        }
    }

    return ann;
}

std::vector<uint8_t> V2VProxyModule::serializeWitnessResponse(const WitnessResponse& witness) {
    // Format: targetCarId|witnessReplicaId|verified|timestamp|siglen|sig
    std::stringstream ss;
    ss << std::setprecision(17);  // Full double precision
    ss << witness.targetCarId << "|" << witness.witnessReplicaId << "|"
       << (witness.verified ? "1" : "0") << "|" << witness.witnessTimestamp << "|"
       << witness.signature.size() << "|";

    std::string header = ss.str();
    std::vector<uint8_t> result(header.begin(), header.end());
    result.insert(result.end(), witness.signature.begin(), witness.signature.end());

    return result;
}

V2VProxyModule::WitnessResponse V2VProxyModule::deserializeWitnessResponse(BFTMessage* bftMsg) {
    std::cout << "[V2VProxy " << replicaId << "] deserializeWitnessResponse called at t=" << simTime() << std::endl;
    std::vector<uint8_t> payload(bftMsg->getPayloadArraySize());
    for (size_t i = 0; i < payload.size(); i++) {
        payload[i] = bftMsg->getPayload(i);
    }

    std::string s(payload.begin(), payload.end());
    std::vector<std::string> parts = split(s, '|');

    WitnessResponse witness;
    if (parts.size() >= 5) {
        witness.targetCarId = parts[0];
        witness.witnessReplicaId = std::stoi(parts[1]);
        witness.verified = (parts[2] == "1");
        witness.witnessTimestamp = std::stod(parts[3]);

        int siglen = std::stoi(parts[4]);

        // CRITICAL: Calculate offset from header parts, NOT find_last_of('|') 
        // because signature bytes can contain '|' (byte 124)!
        std::string header = parts[0] + "|" + parts[1] + "|" + parts[2] + "|" + parts[3] + "|" + parts[4] + "|";
        size_t offset = header.size();
        std::cout << "[DESER_WITNESS] siglen=" << siglen << ", offset=" << offset << ", payload.size()=" << payload.size() << std::endl;

        if (offset + siglen <= payload.size()) {
            witness.signature.assign(payload.begin() + offset, payload.begin() + offset + siglen);
            std::cout << "[DESER_WITNESS] Extracted " << witness.signature.size() << " signature bytes" << std::endl;
        } else {
            std::cerr << "[DESER_WITNESS] ERROR: Signature extraction failed! offset=" << offset << ", siglen=" << siglen << ", payload.size()=" << payload.size() << std::endl;
        }
    }

    std::cout << "[V2VProxy " << replicaId << "] deserializeWitnessResponse completed at t=" << simTime() << std::endl;

    return witness;
}

std::vector<uint8_t> V2VProxyModule::serializeReadyQC(const ReadyQC& qc) {
    // For V2V broadcast - simple binary format
    std::stringstream ss;
    ss << std::setprecision(17);  // Full double precision
    ss << qc.carId << "|" << qc.laneId << "|"
       << qc.positionInLane << "|" << qc.verifiedArrival << "|"
       << qc.epoch << "|" << qc.signatures.size();

    for (const auto& sig : qc.signatures) {
        ss << "|" << sig.witnessReplicaId << "," << sig.witnessTimestamp << "," << sig.signature.size();
    }
    ss << "|";

    std::string header = ss.str();
    std::vector<uint8_t> result(header.begin(), header.end());

    // Append all signature bytes
    for (const auto& sig : qc.signatures) {
        result.insert(result.end(), sig.signature.begin(), sig.signature.end());
    }

    return result;
}

V2VProxyModule::ReadyQC V2VProxyModule::deserializeReadyQC(BFTMessage* bftMsg) {
    std::vector<uint8_t> payload(bftMsg->getPayloadArraySize());
    for (size_t i = 0; i < payload.size(); i++) {
        payload[i] = bftMsg->getPayload(i);
    }

    std::string s(payload.begin(), payload.end());
    std::vector<std::string> parts = split(s, '|');

    ReadyQC qc;
    if (parts.size() < 6) return qc;

    qc.carId = parts[0];
    qc.laneId = parts[1];
    qc.positionInLane = std::stod(parts[2]);
    qc.verifiedArrival = std::stod(parts[3]);
    qc.epoch = std::stoi(parts[4]);

    int numSigs = std::stoi(parts[5]);

    // Reconstruct the text header to compute offset to raw signature bytes.
    // Format: carId|laneId|pos|arrival|epoch|numSigs|witId1,ts1,size1|...|<raw bytes>
    std::string header = parts[0] + "|" + parts[1] + "|" + parts[2] + "|" +
                         parts[3] + "|" + parts[4] + "|" + parts[5];
    for (int i = 0; i < numSigs && (6 + i) < (int)parts.size(); i++) {
        header += "|" + parts[6 + i];
    }
    header += "|";  // final separator before raw bytes
    size_t byteOffset = header.size();

    // Parse each signature's metadata and extract its raw bytes
    for (int i = 0; i < numSigs && (6 + i) < (int)parts.size(); i++) {
        std::vector<std::string> sigParts = split(parts[6 + i], ',');
        if (sigParts.size() >= 3) {
            WitnessSignature sig;
            sig.witnessReplicaId = std::stoi(sigParts[0]);
            sig.witnessTimestamp = std::stod(sigParts[1]);
            int sigSize = std::stoi(sigParts[2]);

            if (byteOffset + sigSize <= payload.size()) {
                sig.signature.assign(payload.begin() + byteOffset,
                                     payload.begin() + byteOffset + sigSize);
                byteOffset += sigSize;
            } else {
                std::cerr << "[DESER_QC] ERROR: Signature bytes out of range for sig " << i
                          << " byteOffset=" << byteOffset << " sigSize=" << sigSize
                          << " payload.size()=" << payload.size() << std::endl;
            }
            qc.signatures.push_back(sig);
        }
    }

    return qc;
}

std::string V2VProxyModule::serializeReadyQCToString(const ReadyQC& qc) {
    // For JNI to Java: "carId:laneId:pos:arrival:epoch:sig1|sig2|..."
    // CRITICAL: Format doubles with %.6f to match signing code!
    char posBuf[32], arrivalBuf[32];
    std::snprintf(posBuf, sizeof(posBuf), "%.6f", qc.positionInLane);
    std::snprintf(arrivalBuf, sizeof(arrivalBuf), "%.6f", qc.verifiedArrival);

    std::stringstream ss;
    ss << qc.carId << ":" << qc.laneId << ":"
       << posBuf << ":" << arrivalBuf << ":"
       << qc.epoch << ":";

    // Append signatures: "witnessId,timestamp,decimalHash|..."
    for (size_t i = 0; i < qc.signatures.size(); i++) {
        if (i > 0) ss << "|";
        const WitnessSignature& sig = qc.signatures[i];

        // Format timestamp with %.6f to match signing
        char timestampBuf[32];
        std::snprintf(timestampBuf, sizeof(timestampBuf), "%.6f", sig.witnessTimestamp);

        ss << sig.witnessReplicaId << ","
           << timestampBuf << ","
           << signatureBytesToString(sig.signature);
    }

    return ss.str();
}

// ============================================================================
// TWO-PHASE CONSENSUS FUNCTIONS
// ============================================================================


// In V2VProxyModule.cc
// ============================================================================
// PHASE 1: VIEW CONSENSUS (Agreement on WHO is at intersection)
// ============================================================================

std::set<std::string> V2VProxyModule::getVisibleVehicles(double maxRange) {
    std::set<std::string> visible;
    
    if (!mobility) {
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: No mobility for getVisibleVehicles" << std::endl;
        return visible;
    }
    
    TraCICommandInterface* traci = mobility->getCommandInterface();
    if (!traci) {
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: No TraCI for getVisibleVehicles" << std::endl;
        return visible;
    }
    
    std::string myCarId = "veh" + std::to_string(replicaId);
    visible.insert(myCarId);
    
    // Get all vehicles in simulation
    std::list<std::string> allIds = traci->getVehicleIds();
    
    // Get my vehicle's position
    Coord myPos = mobility->getPositionAt(simTime());
    
    // Use OMNeT++ module hierarchy to find other vehicles
    cModule* network = getSimulation()->getSystemModule();
    if (!network) return visible;

    // Iterate through all vehicles in the network
    for (cModule::SubmoduleIterator it(network); !it.end(); ++it) {
        cModule* veh = *it;
        if (!veh) continue;

        // Find the V2VProxyModule in this vehicle (usually under "appl")
        cModule* applModule = veh->getSubmodule("appl");
        if (!applModule) continue;

        V2VProxyModule* otherProxy = dynamic_cast<V2VProxyModule*>(applModule);
        if (!otherProxy || otherProxy == this) {
           continue;
        }  // skip myself

        // Get the other vehicle's mobility
        if (!otherProxy->mobility) continue;

        if (otherProxy->isDeparted) continue;

        // Get other vehicle's position
        Coord otherPos = otherProxy->mobility->getPositionAt(simTime());

        // Calculate Euclidean distance
        double distance = myPos.distance(otherPos);

        // Filter by maxRange
        if (distance <= maxRange) {
            std::string otherId = "veh" + std::to_string(otherProxy->replicaId);
            visible.insert(otherId);
        }
    }
    
    return visible;
}

std::vector<uint8_t> V2VProxyModule::signViewProposal(const std::set<std::string>& viewSet) {
    // Create deterministic string from sorted set
    std::string viewString;
    for (const std::string& carId : viewSet) {  // Set is already sorted
        if (!viewString.empty()) viewString += ",";
        viewString += carId;
    }
    
    // Add signer's replica ID for uniqueness (matches Java verifyViewSignature)
    viewString += ":" + std::to_string(replicaId);
    
    // Sign with XXHash32
    int32_t hash = computeXXHash32(viewString);
    std::vector<uint8_t> sig(sizeof(int32_t));
    std::memcpy(sig.data(), &hash, sizeof(int32_t));
    
    std::cout << "[VIEW_SIGN] Replica " << replicaId << " signed view: \"" 
              << viewString << "\" -> hash=" << hash << std::endl;
    
    return sig;
}


void V2VProxyModule::initiateViewProposal() {
    if (currentPhase != IDLE) {
        std::cout << "[V2VProxy " << replicaId << "] Cannot initiate view - already in phase " 
                  << currentPhase << std::endl;
        return;
    }
    
    std::cout << "[V2VProxy " << replicaId << "] ===== PHASE 1a: DETECTING VISIBLE CARS =====" << std::endl;
    
    currentPhase = PROPOSING_VIEW;
    
    // Detect who I can see via TraCI (300m DSRC range)
    std::set<std::string> visibleCars = getVisibleVehicles(300.0);
    
    std::cout << "[V2VProxy " << replicaId << "] My local view: {";
    for (const auto& car : visibleCars) {
        std::cout << car << " ";
    }
    std::cout << "}" << std::endl;
    
    // Create my view proposal
    myViewProposal.proposerReplicaId = replicaId;
    myViewProposal.observedCars = visibleCars;
    myViewProposal.proposalTimestamp = simTime().dbl();
    myViewProposal.signature = signViewProposal(visibleCars);
    
    // Broadcast to neighbors for V2V agreement (Phase 1b)
    std::cout << "[V2VProxy " << replicaId << "] ===== PHASE 1b: COLLECTING V2V AGREEMENTS =====" << std::endl;
    broadcastViewProposal();
    
    currentPhase = VIEW_AGREEMENT;
}

void V2VProxyModule::broadcastViewProposal() {
     // ZOMBIE FILTER: Departed cars don't broadcast views
    V2VProxyModule::zombieFilter();
    
    
    
    std::cout << "[V2VProxy " << replicaId << "] Broadcasting view proposal via V2V..." << std::endl;
    
    std::vector<uint8_t> payload = serializeViewProposal(myViewProposal);
    sendBFTMessage(replicaId, -1, payload, 4);  // messageType=4 (VIEW_PROPOSAL)
    
    std::cout << "[V2VProxy " << replicaId << "] Broadcasted view with " 
              << myViewProposal.observedCars.size() << " cars" << std::endl;
}

void V2VProxyModule::handleViewProposal(BFTMessage* bftMsg) {
    ViewProposal proposal = deserializeViewProposal(bftMsg);
    
    std::cout << "[V2VProxy " << replicaId << "] Received view proposal from replica " 
              << proposal.proposerReplicaId << std::endl;
    std::cout << "[V2VProxy " << replicaId << "]   Their view: {";
    for (const auto& car : proposal.observedCars) {
        std::cout << car << " ";
    }
    std::cout << "}" << std::endl;
    
    // Check if I agree with this view
    std::set<std::string> myView = getVisibleVehicles(300.0);
    
    std::cout << "[V2VProxy " << replicaId << "]   My view: {";
    for (const auto& car : myView) {
        std::cout << car << " ";
    }
    std::cout << "}" << std::endl;
    
    // Do I see exactly the same cars?
    if (proposal.observedCars == myView) {
        std::cout << "[V2VProxy " << replicaId << "] ✓ AGREEMENT: Views match! Sending V2V signature..." << std::endl;
        
        // Create agreement message
        ViewAgreement agreement;
        agreement.agreingReplicaId = replicaId;
        agreement.agreedView = proposal.observedCars;
        agreement.signature = signViewProposal(proposal.observedCars);
        
        // Send unicast back to proposer
        std::vector<uint8_t> payload = serializeViewAgreement(agreement);
        sendBFTMessage(replicaId, proposal.proposerReplicaId, payload, 5);  // messageType=5
        
    } else {
        std::cout << "[V2VProxy " << replicaId << "] ✗ DISAGREEMENT: Views don't match. Not signing." << std::endl;
    }
}
void V2VProxyModule::handleViewAgreement(BFTMessage* bftMsg) {
    ViewAgreement agreement = deserializeViewAgreement(bftMsg);
    
    // 1. Get a reference to the specific vote list for this view
    auto& votes = viewVotes[agreement.agreedView];

    // 2. Check if this specific replica has already voted for this view
    bool alreadyVoted = false;
    for (const auto& existingVote : votes) {
        if (existingVote.agreingReplicaId == agreement.agreingReplicaId) {
            alreadyVoted = true;
            break; 
        }
    }

    // 3. Only proceed if this is a new, unique voter
    if (alreadyVoted) {
        std::cout << "[V2VProxy " << replicaId << "] IGNORING duplicate V2V agreement from replica " 
                  << agreement.agreingReplicaId << std::endl;
        return; // Exit early so we don't re-trigger consensus or increment counters
    }

    // 4. Record the unique vote
    votes.push_back(agreement);
    
    int voteCount = votes.size();
    std::cout << "[V2VProxy " << replicaId << "] Received NEW unique V2V agreement from " 
              << agreement.agreingReplicaId << ". Total unique votes: " << voteCount << std::endl;
    
    // 5. Check if we have f+1 V2V agreements on this view
    std::set<std::string> myView = getVisibleVehicles(300.0);

    int f = (myView.size() - 1) / 3;
    int required = f + 1;
    
    // Use '==' instead of '>=' to ensure we only trigger the BFT submission once
    if (voteCount >= required && !viewEstablished) {
        viewSignatureCollectionEndTime = simTime(); //we are done collecting view signatures
        std::cout << "[V2VProxy " << replicaId << "] ===== PHASE 1c: SUBMITTING TO BFT CONSENSUS =====" << std::endl;
        std::cout << "[V2VProxy " << replicaId << "] Collected f+1=" << required 
                  << " V2V signatures for view: {";
        for (const auto& car : agreement.agreedView) {
            std::cout << car << " ";
        }
        std::cout << "}" << std::endl;
        
        viewEstablished = true;
        
        // Submit the view with the accumulated unique V2V signatures to BFT-SMaRt
        submitViewToBFTConsensus(agreement.agreedView, votes);
    }
}

void V2VProxyModule::submitViewToBFTConsensus(const std::set<std::string>& view, 
                                                const std::vector<ViewAgreement>& v2vSigs) {
    // Mark the start of View consensus
    viewConsensusStartTime = simTime();
    realViewConsensusStart = std::chrono::high_resolution_clock::now();
    std::cout << "[V2VProxy " << replicaId << "] Submitting view to BFT-SMaRt consensus..." << std::endl;
    std::cout << "[METRICS " << replicaId << "] View_Consensus_Start: " << viewConsensusStartTime << std::endl;
    
    // Format for Java: "VIEW_PROPOSE:proposerId:carList:sig1|sig2|sig3"
    // Example: "VIEW_PROPOSE:0:veh0,veh1,veh2:replicaId,hash|replicaId,hash|..."
    
    std::stringstream ss;
    ss << "VIEW_PROPOSE:" << replicaId << ":";
    
    // Add sorted car list
    std::vector<std::string> sortedCars(view.begin(), view.end());
    std::sort(sortedCars.begin(), sortedCars.end());
    for (size_t i = 0; i < sortedCars.size(); i++) {
        if (i > 0) ss << ",";
        ss << sortedCars[i];
    }
    ss << ":";
    
    // Add V2V signatures
    for (size_t i = 0; i < v2vSigs.size(); i++) {
        if (i > 0) ss << "|";
        
        const ViewAgreement& sig = v2vSigs[i];
        
        // Convert signature bytes to decimal hash value
        if (sig.signature.size() >= 4) {
            int32_t hashValue;
            std::memcpy(&hashValue, sig.signature.data(), sizeof(int32_t));
            ss << sig.agreingReplicaId << "," << hashValue;
        }
    }
    
    std::string request = ss.str();
    std::cout << "[V2VProxy " << replicaId << "] BFT request: " << request << std::endl;
    
    // Trigger BFT consensus via JNI
    if (!triggerJoinViaJNI(request)) {
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: Failed to submit view to BFT" << std::endl;
    }
}

void V2VProxyModule::onViewAgreed(const std::set<std::string>& agreedView) {
    std::lock_guard<std::mutex> lock(jniMutex);
    
    // Mark the end of View consensus
    viewConsensusEndTime = simTime();
    simtime_t viewConsensusDuration = viewConsensusEndTime - viewConsensusStartTime;
    realViewConsensusEnd = std::chrono::high_resolution_clock::now();
    auto realViewConsensusDuration = std::chrono::duration_cast<std::chrono::milliseconds>(realViewConsensusEnd - realViewConsensusStart);
    std::cout << "[METRICS " << replicaId << "] View_Consensus_Duration: " << realViewConsensusDuration.count() << "ms" << std::endl;
    std::cout << "[V2VProxy " << replicaId << "] Phase 2 flag SET via JNI thread." << std::endl;
    std::cout << "[METRICS " << replicaId << "] View_Consensus_End: " << viewConsensusEndTime << std::endl;
    std::cout << "[METRICS " << replicaId << "] View_Consensus_Latency: " << viewConsensusDuration.dbl() << " seconds" << std::endl;
    
    establishedView = agreedView;
    viewEstablished = true;
    phase2Pending = true;


    // std::cout << "[V2VProxy " << replicaId << "] ===== VIEW CONSENSUS COMPLETE (Java callback) =====" << std::endl;
    // std::cout << "[V2VProxy " << replicaId << "] Agreed view (" << agreedView.size() << " cars): {";
    // for (const auto& car : agreedView) {
    //     std::cout << car << " ";
    // }
    // std::cout << "}" << std::endl;
    
    // // Store the agreed view
    // establishedView = agreedView;
    // viewEstablished = true;
    
    // // Transition from Phase 1c → Phase 2
    // //startReadyQCCollection();
    // scheduleAt(simTime(), startReadyQCCollectionMsg);
    std::cout << "[V2VProxy " << replicaId << "] Scheduled Phase 2 transition for t BUT THIS IN VIEW AGREED=" << simTime() << std::endl;
}

void V2VProxyModule::startReadyQCCollection() {
    std::cout << "[V2VProxy " << replicaId << "] ===== PHASE 2: STARTING READYQC COLLECTION =====" << std::endl;
    std::cout << "[V2VProxy " << replicaId << "] View members: " << establishedView.size() << " cars" << std::endl;
    arrivalAnnouncementsReceived.clear();
    currentPhase = COLLECTING_QC;
    orderSignatureCollectionStartTime = simTime();
    std::string myCarId = "veh" + std::to_string(replicaId);
    if (establishedView.find(myCarId) != establishedView.end()) {
        std::cout << "[V2VProxy " << replicaId << "] I AM in the view - broadcasting arrival announcement" << std::endl;
        // Broadcast arrival announcement so neighbors can witness
        broadcastArrivalAnnouncement();
    } else {
        std::cout << "[V2VProxy " << replicaId << "] I am NOT in the view - will act as witness only" << std::endl;
    }
}

// ============================================================================
// LANE FRONT DETECTION
// ============================================================================

/**
 * Returns true iff carId is the front-most vehicle in laneId (i.e. no other
 * active, non-departed vehicle in the same SUMO lane has a higher lanePosition).
 *
 * Key design decision: we use SUMO's lanePosition (metres from lane start)
 * as the ordering metric rather than Euclidean distance to the intersection
 * centre.  Euclidean distance is ambiguous for cross-directional comparisons
 * and would use "this" proxy's position instead of carId's position if called
 * from a different module.  lanePosition is unambiguous: higher value = further
 * along the lane = closer to the junction stop line.
 */
bool V2VProxyModule::isCarAtFrontOfLane(const std::string& carId,
                                         const std::string& laneId) {
    if (!mobility) return false;
    TraCICommandInterface* traci = mobility->getCommandInterface();
    if (!traci) return false;

    // Confirm carId exists in the simulation right now
    std::list<std::string> allIds = traci->getVehicleIds();
    if (std::find(allIds.begin(), allIds.end(), carId) == allIds.end()) {
        std::cout << "[FRONT_CHECK] " << carId << " not found in TraCI vehicle list" << std::endl;
        return false;
    }

    std::vector<std::pair<std::string,double>> sameLane;
    for (const auto& vid : allIds) {
        try {
            auto v = traci->vehicle(vid);
            if (v.getLaneId() == laneId) {
                sameLane.push_back({vid, v.getLanePosition()});
            }
        } catch (...) {}
    }
    std::sort(sameLane.begin(), sameLane.end(),
            [](auto& a, auto& b){ return a.second > b.second; }); // high->low

    std::cout << "[FRONT_CHECK] Lane snapshot " << laneId << ": ";
    for (auto& p : sameLane) std::cout << p.first << "=" << p.second << " ";
    std::cout << std::endl;

    // Confirm carId is actually in laneId (not in an internal junction lane, etc.)
    std::string carActualLane = traci->vehicle(carId).getLaneId();
    if (carActualLane != laneId) {
        std::cout << "[FRONT_CHECK] " << carId << " is in lane " << carActualLane
                  << ", not " << laneId << std::endl;
        return false;
    }

    // Higher lanePosition  =  further along the lane  =  closer to the junction.
    double carLanePos = traci->vehicle(carId).getLanePosition();
    std::cout << "[FRONT_CHECK] " << carId << " lanePos=" << carLanePos
              << " in lane " << laneId << std::endl;

    // Walk the module tree to find every other active vehicle
    cModule* network = getSimulation()->getSystemModule();
    if (!network) return true;  // can't enumerate, assume front

    for (cModule::SubmoduleIterator it(network); !it.end(); ++it) {
        cModule* veh = *it;
        if (!veh) continue;

        cModule* applModule = veh->getSubmodule("appl");
        if (!applModule) continue;

        V2VProxyModule* otherProxy = dynamic_cast<V2VProxyModule*>(applModule);
        if (!otherProxy) continue;

        std::string otherId = "veh" + std::to_string(otherProxy->replicaId);
        if (otherId == carId)         continue;  // skip self
        if (otherProxy->isDeparted)   continue;  // skip cars that have left
        if (!otherProxy->mobility)    continue;

        // Only look at vehicles TraCI currently knows about
        if (std::find(allIds.begin(), allIds.end(), otherId) == allIds.end()) continue;

        // Only compare against cars in the same approach lane
        std::string otherLane = traci->vehicle(otherId).getLaneId();
        if (otherLane != laneId) continue;

        double otherLanePos = traci->vehicle(otherId).getLanePosition();
        if (otherLanePos > carLanePos) {
            // Another car in the same lane is further ahead → carId is not at front
            std::cout << "[FRONT_CHECK] " << carId << " NOT front of " << laneId
                      << ": " << otherId << " at pos=" << otherLanePos
                      << " > " << carLanePos << std::endl;
            return false;
        }
    }

    std::cout << "[FRONT_CHECK] " << carId << " IS front of " << laneId
              << " (lanePos=" << carLanePos << ")" << std::endl;
    return true;
}



/**
 * Returns true iff our car's entry in verifiedPool has the highest positionInLane
 * among all pooled QCs in the same lane.  Used as a gossip-pool-based front check
 * (works without TraCI and cross-module calls).
 */
bool V2VProxyModule::isMyQCFrontMostKnownInLaneFromPool() {
    std::string myCarId = "veh" + std::to_string(replicaId);
    auto myIt = verifiedPool.find(myCarId);
    if (myIt == verifiedPool.end()) return false;

    const ReadyQC& myQC = myIt->second;
    for (const auto& kv : verifiedPool) {
        if (kv.first == myCarId) continue;
        if (kv.second.laneId != myQC.laneId) continue;
        if (kv.second.epoch  != myQC.epoch)  continue;
        if (kv.second.positionInLane > myQC.positionInLane) {
            // Another pooled QC in the same lane is further ahead
            return false;
        }
    }
    return true;
}

// Deprecated - replaced by initiateViewProposal
std::set<std::string> V2VProxyModule::proposeView() {
    std::cerr << "[V2VProxy " << replicaId << "] WARNING: proposeView() is deprecated. Use initiateViewProposal()" << std::endl;
    return getVisibleVehicles(300.0);
}

// OLD FUNCTION REMOVED - replaced by submitViewToBFTConsensus()
// View consensus now uses 3-phase approach:
// 1. initiateViewProposal() - detect visible cars
// 2. Collect f+1 V2V agreements
// 3. submitViewToBFTConsensus() - BFT consensus on agreed view

// void V2VProxyModule::handleReadyQCComplete(BFTMessage* bftMsg) {
//     ReadyQC qc = deserializeReadyQC(bftMsg);

//     // Store ReadyQC from another car
//     verifiedPool[qc.carId] = qc;

//     std::cout << "[V2VProxy " << replicaId << "] Received ReadyQC from " << qc.carId
//               << " (" << verifiedPool.size() << " total)" << std::endl;



//     std::string myCarId = "veh" + std::to_string(replicaId);
//     if (!hasProposedOrder && qc.carId == myCarId) {
//         hasProposedOrder = true;
//         triggerOrderConsensus();
//     } else if (hasProposedOrder) {
//         std::cout << "[V2VProxy " << replicaId << "] Already proposed ORDER, ignoring ReadyQC from " << qc.carId << std::endl;
        
//     }else{
//         std::cout << "[V2VProxy " << replicaId << "] Received ReadyQC from " << qc.carId   << ", waiting for MY ReadyQC (MY ID: " << myCarId << ")" << std::endl;
//     }
// }


void V2VProxyModule::handleReadyQCComplete(BFTMessage* bftMsg) {
    ReadyQC qc = deserializeReadyQC(bftMsg);

    // Epoch guard
    if (qc.epoch != currentEpoch) {
        std::cout << "[READYQC] Replica " << replicaId
                  << " ignoring stale QC from " << qc.carId
                  << " epoch=" << qc.epoch << " current=" << currentEpoch << std::endl;
        return;
    }

    // View guard (optional but recommended)
    if (!establishedView.empty() && establishedView.count(qc.carId) == 0) {
        std::cout << "[READYQC] Replica " << replicaId
                  << " ignoring QC from non-view car " << qc.carId << std::endl;
        return;
    }

    // Dedupe / overwrite per car (one QC per car per epoch)
    bool isNew = (verifiedPool.find(qc.carId) == verifiedPool.end());
    verifiedPool[qc.carId] = qc;

    std::cout << "[V2VProxy " << replicaId << "] Received ReadyQC from " << qc.carId
              << " (" << verifiedPool.size() << " total in pool)"
              << (isNew ? " [NEW]" : " [UPDATE]") << std::endl;

    std::string myCarId = "veh" + std::to_string(replicaId);

    // If self-delivered broadcast arrives and myReadyQCComplete wasn't set yet, set/start
    if (qc.carId == myCarId && !myReadyQCComplete) {
        myReadyQCComplete = true;
        if (isCarAtFrontOfLane(myCarId, qc.laneId)) {
            startOrderCollectionWindowIfNeeded();
        } else {
            std::cout << "[ORDER-COLLECT] Replica " << replicaId 
                      << " is NOT at the front of lane " << qc.laneId 
                      << " - staying quiet." << std::endl;
        }
    }

    // New flow: do NOT trigger immediate ORDER consensus here
    // Instead consider early close of collection window
    if (orderCollectionActive && !orderBagProposed && !orderDecisionReceived) {
        int frontLanes = countDistinctFrontLanesInPool();
        std::cout << "[ORDER-COLLECT] Replica " << replicaId
                  << " front-lanes-in-pool=" << frontLanes << std::endl;

        if (frontLanes >= 4) {
            proposeOrderBagNow("EARLY_4_LANES");
        }
    }
}

//ZOMBIE MODE FUNCTIONS
bool V2VProxyModule::checkIfDeparted()
{
    if (isDeparted) return true;  // Already departed

    if (!mobility || !traciVehicle) return false;

    // Check if we've moved significantly past the intersection
    double dist = getDistanceToIntersection();

    // Negative distance means we've passed the intersection
    // Or if distance > 100m on the far side (well past intersection)
    if (dist < -15.0 || (currentPhase == EXECUTING && dist > 15.0)) {
        isDeparted = true;
        currentPhase = DEPARTED;

        std::cout << "[V2VProxy " << replicaId << "] ===== VEHICLE DEPARTED =====" << std::endl;
        std::cout << "[V2VProxy " << replicaId << "] Distance: " << dist << "m" << std::endl;
        std::cout << "[V2VProxy " << replicaId << "] Entering ZOMBIE mode (no more V2V)" << std::endl;
        std::cout << "[V2VProxy " << replicaId << "] Phase: " << currentPhase << std::endl;

        // Notify Java that we've departed
        notifyJavaDeparted();

        return true;
    }

    return false;
}

void V2VProxyModule::notifyJavaDeparted()
{
    std::lock_guard<std::mutex> lock(jvmMutex);

    if (!sharedJVM) {
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: Cannot notify Java - JVM not available" << std::endl;
        return;
    }

    JNIEnv* env;
    sharedJVM->AttachCurrentThread((void**)&env, nullptr);

    jclass serverRunnerClass = env->FindClass("bftsmart/demo/intersection/ServerRunner");
    if (!serverRunnerClass) {
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: ServerRunner class not found" << std::endl;
        env->ExceptionDescribe();
        sharedJVM->DetachCurrentThread();
        return;
    }

    jmethodID notifyMethod = env->GetStaticMethodID(serverRunnerClass,
                                                     "notifyVehicleDeparted",
                                                     "(I)V");
    if (!notifyMethod) {
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: notifyVehicleDeparted method not found" << std::endl;
        env->ExceptionDescribe();
        sharedJVM->DetachCurrentThread();
        return;
    }

    env->CallStaticVoidMethod(serverRunnerClass, notifyMethod, replicaId);

    // Check for JNI exceptions
    if (env->ExceptionCheck()) {
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: Exception calling notifyVehicleDeparted" << std::endl;
        env->ExceptionDescribe();
        env->ExceptionClear();
    } else {
        std::cout << "[V2VProxy " << replicaId << "] Notified Java of departure (zombie mode activated)" << std::endl;
    }

    sharedJVM->DetachCurrentThread();
}



void V2VProxyModule::notifyJavaNewBatchSize(int newBatchSize) {
    std::lock_guard<std::mutex> lock(jvmMutex);                                                                                                                                    
                                                                                                                                                                                
    if (!sharedJVM) {                                                                                                                                                              
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: Cannot notify batch size - JVM not available" << std::endl;                                                            
        return;                                                                                                                                                                    
    }                                                                                                                                                                              
                                                                                                                                                                                
    JNIEnv* env;                                                                                                                                                                   
    sharedJVM->AttachCurrentThread((void**) &env, nullptr);                                                                                                                        
                                                                                                                                                                                
    jclass serverRunnerClass = env->FindClass("bftsmart/demo/intersection/ServerRunner");                                                                                          
    if (!serverRunnerClass) {                                                                                                                                                      
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: ServerRunner class not found" << std::endl;                                                                            
        sharedJVM->DetachCurrentThread();                                                                                                                                          
        return;                                                                                                                                                                    
    }                                                                                                                                                                              
                                                                                                                                                                                
    jmethodID notifyMethod = env->GetStaticMethodID(serverRunnerClass,                                                                                                             
                                                    "notifyBatchSize",                                                                                                            
                                                    "(II)V");  // (replicaId, batchSize)                                                                                          
    if (!notifyMethod) {                                                                                                                                                           
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: notifyBatchSize method not found" << std::endl;                                                                        
        sharedJVM->DetachCurrentThread();                                                                                                                                           
        return;                                                                                                                                                                    
    }                                                                                                                                                                              

                                                                                                                                                                                
    env->CallStaticVoidMethod(serverRunnerClass, notifyMethod, replicaId, newBatchSize);                                                                                              
                                                                                                                                                                                
    std::cout << "[V2VProxy " << replicaId << "] Notified Java of batch size: " << newBatchSize << std::endl;                                                                         
                                                                                                                                                                                
    sharedJVM->DetachCurrentThread();                                                                                                                                              
 }                                                                                                                                                                                  
                                                   

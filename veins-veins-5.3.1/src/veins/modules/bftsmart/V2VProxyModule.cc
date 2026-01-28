//
// V2VProxyModule.cc
// Implementation of V2V Proxy Module for BFT-SMaRt
//

#include "veins/modules/bftsmart/V2VProxyModule.h"
#include "veins/base/utils/SimpleAddress.h"
#include "veins/modules/utility/Consts80211p.h"
#include <cstring>
#include <sstream>
#include <iomanip>
#include <chrono>


using namespace veins;

static int completedConsensusCount = 0;
static const int BATCH_SIZE = 8;
// Base64 encoding table
static const char base64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Base64 encode binary data to string (safe for null bytes)
static std::string base64_encode(const uint8_t* data, size_t len) {
    std::string ret;
    ret.reserve(((len + 2) / 3) * 4);
    
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = ((uint32_t)data[i]) << 16;
        if (i + 1 < len) n |= ((uint32_t)data[i + 1]) << 8;
        if (i + 2 < len) n |= data[i + 2];
        
        ret.push_back(base64_chars[(n >> 18) & 0x3F]);
        ret.push_back(base64_chars[(n >> 12) & 0x3F]);
        ret.push_back((i + 1 < len) ? base64_chars[(n >> 6) & 0x3F] : '=');
        ret.push_back((i + 2 < len) ? base64_chars[n & 0x3F] : '=');
    }
    return ret;
}

// Base64 decode string to binary data
static std::vector<uint8_t> base64_decode(const std::string& encoded) {
    std::vector<uint8_t> ret;
    if (encoded.empty()) return ret;
    
    // Build decode table
    int decode_table[256];
    memset(decode_table, -1, sizeof(decode_table));
    for (int i = 0; i < 64; i++) {
        decode_table[(unsigned char)base64_chars[i]] = i;
    }
    
    ret.reserve((encoded.size() / 4) * 3);
    
    for (size_t i = 0; i < encoded.size(); i += 4) {
        int n0 = decode_table[(unsigned char)encoded[i]];
        int n1 = (i + 1 < encoded.size()) ? decode_table[(unsigned char)encoded[i + 1]] : 0;
        int n2 = (i + 2 < encoded.size() && encoded[i + 2] != '=') ? decode_table[(unsigned char)encoded[i + 2]] : 0;
        int n3 = (i + 3 < encoded.size() && encoded[i + 3] != '=') ? decode_table[(unsigned char)encoded[i + 3]] : 0;
        
        if (n0 < 0 || n1 < 0) continue;
        
        ret.push_back((n0 << 2) | (n1 >> 4));
        if (i + 2 < encoded.size() && encoded[i + 2] != '=') {
            ret.push_back(((n1 & 0xF) << 4) | (n2 >> 2));
        }
        if (i + 3 < encoded.size() && encoded[i + 3] != '=') {
            ret.push_back(((n2 & 0x3) << 6) | n3);
        }
    }
    return ret;
}

Define_Module(veins::V2VProxyModule);

// Static member initialization
std::map<int, V2VProxyModule*> V2VProxyModule::replicaProxyMap;
std::mutex V2VProxyModule::registryMutex;
JavaVM* V2VProxyModule::sharedJVM = nullptr;
std::mutex V2VProxyModule::jvmMutex;

V2VProxyModule::V2VProxyModule()
    : DemoBaseApplLayer()
    , replicaId(-1)
    , serviceChannel(2)
    , sentMessages(0)
    , receivedMessages(0)
    , sequenceNumber(0)
    , jvm(nullptr)
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
    , stopDistance(50.0)
    , intersectionWidth(25.0)
    , avgSpeed(10.0)
    , safetyGap(2.0)
    , waitingForConsensus(false)
    , hasRequestedCrossing(false)
    , isStopped(false)
    , checkPositionTimer(nullptr)
    , consensusTimeoutTimer(nullptr)
    , shouldFlush(false)
    , consensusTimeoutSec(40.0)  // Default 40 seconds
    , MAX_MESSAGES_PER_TICK(getMaxMessagesPerTick())
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
        replicaId = par("replicaId");
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

        // Create message for radio ready notification (reactive yield pattern)
        radioReadyMsg = new cMessage("radioReady");

        // Create timer for checking position
        checkPositionTimer = new cMessage("checkPosition");

        // Create timer for consensus timeout fallback
        consensusTimeoutTimer = new cMessage("consensusTimeout");

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
    // Create message
    PendingMessage pendingMsg;
    pendingMsg.fromReplicaId = fromReplicaId;
    pendingMsg.toReplicaId = toReplicaId;
    pendingMsg.data.assign(data, data + dataLen);
    
    // THROTTLE: Wait if queue is full (synchronizes Java with simulation time)
    {
        std::unique_lock<std::mutex> lock(jniMutex);

        if (messageQueue.size() >= MAX_QUEUE_SIZE) {
            std::cout << "[DEBUG V2VProxy " << replicaId << "] Queue is full - dropping message" << std::endl;
            return false;
        }
        
       
        // Queue has space - add message
        std::cout << "[DEBUG V2VProxy " << replicaId << "] Queue has space - adding message" << std::endl;
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
    ASSERT(bftMsg);
    syncTimeToJava();

    receivedMessages++;
    emit(bftMsgReceivedSignal, receivedMessages);

    int fromReplicaId = bftMsg->getFromReplicaId();
    int toReplicaId = bftMsg->getToReplicaId();

    // Log ALL received messages for debugging (first 20, then occasionally)
    if (receivedMessages <= 20 || receivedMessages % 100 == 0) {
        std::cout << "[V2VProxy " << replicaId << "] *** RECEIVED V2V message #" << receivedMessages
                  << " from=" << fromReplicaId << " to=" << toReplicaId
                  << " (broadcast=" << (toReplicaId == -1 ? "YES" : "NO") << ")"
                  << " at t=" << simTime() << std::endl;
    }
    
    // Check if message is for us or broadcast
    if (toReplicaId == replicaId || toReplicaId == -1) {
        // Get base64-encoded payload and decode to binary
        std::string encodedPayload = bftMsg->getPayload();
        std::vector<uint8_t> data = base64_decode(encodedPayload);

        // Deliver to Java via JNI
        deliverMessageToJava(fromReplicaId, data.data(), data.size());
    }
    // Silently ignore messages not for us (they're broadcast)

    delete bftMsg;
}

// Deliver message to Java through JNI callback
void V2VProxyModule::deliverMessageToJava(int fromReplicaId, const uint8_t* data, int dataLen)
{
    std::lock_guard<std::mutex> lock(jniMutex);

    if (!jvm || !javaCallbackObject || !deliverMessageMethod) {
        // CRITICAL: Log when messages are being dropped due to unregistered callback
        static int dropCount = 0;
        if (++dropCount <= 10 || dropCount % 50 == 1) {
            std::cout << "[V2VProxy " << replicaId << "] WARNING: Dropping message #" << dropCount
                      << " from replica " << fromReplicaId << " (callback not registered yet!)"
                      << " jvm=" << (jvm ? "OK" : "NULL")
                      << " callback=" << (javaCallbackObject ? "OK" : "NULL")
                      << " method=" << (deliverMessageMethod ? "OK" : "NULL")
                      << std::endl;
        }
        return;
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
    env->CallVoidMethod(javaCallbackObject, deliverMessageMethod, fromReplicaId, javaData);
    
    // Check for exceptions (silently clear them)
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }
    
    env->DeleteLocalRef(javaData);
}

// Send BFT message via V2V
void V2VProxyModule::sendBFTMessage(int fromReplicaId, int toReplicaId, const std::vector<uint8_t>& data)
{
    // Log outgoing messages (first 20, then occasionally)
    sentMessages++; // Move increment here to count before sending
    if (sentMessages <= 20 || sentMessages % 100 == 0) {
        std::cout << "[V2VProxy " << replicaId << "] *** SENDING V2V message #" << sentMessages
                  << " from=" << fromReplicaId << " to=" << toReplicaId
                  << " size=" << data.size() << " bytes at t=" << simTime() << std::endl;
    }

    // Base64 encode binary data to preserve null bytes
    std::string encodedPayload = base64_encode(data.data(), data.size());

    BFTMessage* bftMsg = new BFTMessage();
    
    bftMsg->setFromReplicaId(fromReplicaId);
    bftMsg->setToReplicaId(toReplicaId);
    bftMsg->setMessageType(0);  // 0=consensus
    bftMsg->setSequenceNum(sequenceNumber++);
    bftMsg->setTimestamp(simTime());
    bftMsg->setPayloadLength(data.size());  // Original binary size
    bftMsg->setPayload(encodedPayload.c_str());  // Base64 encoded string
    
    // Set wave parameters for broadcast
    // Use CCH (178) for BFT messages - all nodes always listen on CCH
    // Using SCH would require synchronized channel switching
    bftMsg->setChannelNumber(static_cast<int>(veins::Channel::cch));
    bftMsg->addBitLength(par("headerLength"));
    bftMsg->addBitLength(encodedPayload.length() * 8);

    emit(bftMsgSentSignal, sentMessages);
    
    // CRITICAL: Set recipient address for V2V broadcast
    bftMsg->setRecipientAddress(LAddress::L2BROADCAST());

    // REACTIVE YIELD: Mark radio as busy
    double jitter = uniform(0.001, 0.010);
    sendDelayed(bftMsg, jitter, lowerLayerOut);
    radioBusy = true;
    
    
    // Send as WSM (Wave Short Message)
    //sendDown(bftMsg);
    
    // Schedule radioReady callback after estimated transmission time
    // At 6 Mbps, 600 bytes takes ~0.8ms. Add margin for MAC contention.
    double txTime = (encodedPayload.length() * 8) / 6e6;
    double margin = 0.002;  // 2ms for MAC delays
    if (!radioReadyMsg->isScheduled()) {
        scheduleAt(simTime() + jitter + txTime + margin, radioReadyMsg);
    }
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


void V2VProxyModule::flushReliabilityQueue(){

    // Cancel the timer if scheduled (but don't delete it - it may be reused)
    if (processQueueTimer && processQueueTimer->isScheduled()) {
        cancelEvent(processQueueTimer);
    }

    // Clear all pending messages
    std::queue<PendingMessage> empty;

    std::swap(messageQueue, empty);

    // Wake up any Java threads waiting on the queue
    queueCondVar.notify_all();

    std::cout << "[V2VProxy " << replicaId << "] Reliability queue flushed (consensus complete)" << std::endl;
}


int V2VProxyModule::getMaxMessagesPerTick() {
    const int CHANNEL_CAPACITY = 75;
    const double TARGET_UTIL = 0.85;
    return std::max(2, (int)(CHANNEL_CAPACITY * TARGET_UTIL / BATCH_SIZE)); 
}

void V2VProxyModule::handleSelfMsg(cMessage* msg)
{
    {
        std::lock_guard<std::mutex> lock(jniMutex); // Reuse existing mutex
        
        if (!pendingResumeDelays.empty()) {
            shouldFlush = true;
        }
            
        while (!pendingResumeDelays.empty()) {
            completedConsensusCount++;
            double delay = pendingResumeDelays.front();
            pendingResumeDelays.pop();
            simtime_t endTime = simTime();
            
            simtime_t duration = endTime - consensusStartTime;
            std::cout << "[METRICS " << replicaId << "] Consensus Duration: " << duration.dbl() << " seconds" << std::endl;
            std::cout << "[METRICS " << replicaId << "] Consensus Completed!" << std::endl;
            std::cout << "[METRICS " << replicaId << "] Consensus Start Time: " << consensusStartTime << std::endl;
            std::cout << "[METRICS " << replicaId << "] Consensus End Time:   " << endTime << std::endl;
            std::cout << "[METRICS " << replicaId << "] Consensus Latency:    " << duration.dbl() << " seconds" << std::endl;
            simtime_t scheduledResumeTime = endTime + delay;
            simtime_t totalWaitTime = scheduledResumeTime - stopTime ; //5 because they spawn at 5 seconds
            std::cout << "[METRICS " << replicaId << "] Projected_Total_Wait: " << totalWaitTime.dbl() << std::endl;
            std::cout << "[METRICS " << replicaId << "] Scheduled_Resume: " << scheduledResumeTime.dbl() << std::endl;
            std::cout << "[METRICS " << replicaId << "] Messages Sent: " << sentMessages << std::endl;
            std::cout << "[METRICS " << replicaId << "] Messages Received: " << receivedMessages << std::endl;
            // Cancel timeout timer - consensus succeeded!
            if (consensusTimeoutTimer->isScheduled()) {
                cancelEvent(consensusTimeoutTimer);
                std::cout << "[METRICS " << replicaId << "] (consensus succeeded : TRUE)" << std::endl;
            }

            // We are now on the main thread, so scheduleAt is safe!
            std::cout << "[V2VProxy " << replicaId << "] Main thread scheduling resume in " << delay << "s" << std::endl;

            cMessage* resumeMsg = new cMessage("resumeVehicle");
            scheduleAt(simTime() + delay, resumeMsg);

        }

        if (shouldFlush) {
            //flushReliabilityQueue();
            std::cout << "Not flushing reliability queue" << std::endl;
        }
        
        
    }
    if (completedConsensusCount == BATCH_SIZE) {
        std::cout << "[METRICS " << replicaId << "] All Consensus Completed" << std::endl;
        endSimulation();
    }
    
    syncTimeToJava();
    
    if (msg == processQueueTimer) {
        // Process LIMITED queued messages (don't flood the V2V channel)
        // During BFT init, reduce traffic to minimize collisions
        std::vector<PendingMessage> toProcess;
        
        {
            std::lock_guard<std::mutex> lock(jniMutex);
            int count = 0;
            while (!messageQueue.empty() && count < MAX_MESSAGES_PER_TICK) {
                toProcess.push_back(messageQueue.front());
                messageQueue.pop();
                count++;
            }
            
            // Signal waiting Java threads if queue has space now
            if (messageQueue.size() < MAX_QUEUE_SIZE) {
                queueCondVar.notify_all();
            }
        }
        
        if (!toProcess.empty()) {
            // Log progress
            static int timerCount = 0;
            if (++timerCount % 50 == 1) {
                std::cout << "[V2VProxy " << replicaId << "] Timer #" << timerCount 
                          << " at t=" << simTime() << ", sending " << toProcess.size() << " messages" << std::endl;
            }
            
            for (const auto& pending : toProcess) {
                sendBFTMessage(pending.fromReplicaId, pending.toReplicaId, pending.data);
            }
        }
        
        // Reschedule timer - 50ms gives more time between transmissions to reduce collisions
        scheduleAt(simTime() + 0.05, processQueueTimer);
        
    } else if (msg == checkPositionTimer) {
        double distance = getDistanceToIntersection();

        // Log when getting close (within 100m)
        if (distance < 100.0 && distance > stopDistance) {
            static bool logged100m = false;
            if (!logged100m) {
                std::cout << "[V2VProxy " << replicaId << "] Approaching! Distance="
                          << distance << "m at t=" << simTime() << std::endl;
                logged100m = true;
            }
        }

        // Check if approaching intersection
        if (isApproachingIntersection() && !hasRequestedCrossing && !waitingForConsensus) {
            std::cout << "[V2VProxy " << replicaId << "] *** STOPPING at intersection! ***" << std::endl;
            std::cout << "[V2VProxy " << replicaId << "] Distance=" << distance
                      << "m, t=" << simTime() << std::endl;

            // Stop the vehicle
            stopVehicle();
            stopTime = simTime();

            // Trigger BFT consensus via JNI
            bool success = triggerJoinViaJNI();
            if (success) {
                // Only set flags if successful
                hasRequestedCrossing = true;
                waitingForConsensus = true;
                consensusStartTime = simTime();
                std::cout << "[V2VProxy " << replicaId << "] Consensus Timer STARTED at " << consensusStartTime << std::endl;

                // Start timeout timer (fallback if consensus fails)
                scheduleAt(simTime() + consensusTimeoutSec, consensusTimeoutTimer);
                std::cout << "[V2VProxy " << replicaId << "] Consensus timeout set for " << consensusTimeoutSec << "s" << std::endl;
            } else {
                std::cout << "[V2VProxy " << replicaId << "] Failed to trigger JOIN, will retry..." << std::endl;
                // Don't set flags - will retry on next timer tick
            }
        }

        // Keep checking position every 0.1s
        scheduleAt(simTime() + 0.1, checkPositionTimer);

    } else if (msg == triggerJoinTimer) {
        // Time to trigger JOIN request via JNI!
        std::cout << "[DEBUG V2VProxy " << replicaId << "] *** TRIGGER JOIN at t=" << simTime() << " ***" << std::endl;
        bool success = triggerJoinViaJNI();
        if (!success) {
            scheduleAt(simTime() + 1.0, triggerJoinTimer);
        }
        // Don't reschedule - this is a one-time trigger

    } else if (msg == radioReadyMsg) {
        // REACTIVE YIELD: Radio is now free, notify Java
        radioBusy = false;
        
        // Notify waiting Java threads
        {
            std::lock_guard<std::mutex> lock(jniMutex);
            queueCondVar.notify_all();
        }
        
        // Call Java's onRadioReady callback if registered
        notifyJavaRadioReady();

    } else if (strcmp(msg->getName(), "resumeVehicle") == 0) {  
        simtime_t resumeTime = simTime();
        // simtime_t totalWait = resumeTime - stopTime;
        // std::cout << "[METRICS " << replicaId << "] Total_Wait_Time: " << totalWait.dbl() << " seconds" << std::endl;
        std::cout << "[METRICS " << replicaId << "] Resume_Time: " << resumeTime << std::endl;
        std::cout << "[METRICS " << replicaId << "] Stop_Time: " << stopTime << std::endl;

        // Time to resume vehicle movement after consensus delay
        std::cout << "[V2VProxy " << replicaId << "] Resume delay expired, resuming vehicle now!" << std::endl;

        if (mobility && mobility->getVehicleCommandInterface()) {
            mobility->getVehicleCommandInterface()->setSpeed(-1);  // Release control to SUMO
            isStopped = false;
            waitingForConsensus = false;
            std::cout << "[V2VProxy " << replicaId << "] Vehicle RESUMED movement at t=" << simTime() << std::endl;
        }

        delete msg;  // Clean up the self-message

    } else if (msg == consensusTimeoutTimer) {
        // TIMEOUT FALLBACK: Consensus didn't complete in time
        // Without quorum, we can't make a BFT decision, so just let the car go
        std::cout << "[V2VProxy " << replicaId << "] *** CONSENSUS TIMEOUT *** No quorum reached after "
                  << consensusTimeoutSec << "s" << std::endl;
        std::cout << "[METRICS " << replicaId << "] (consensus succeeded : FALSE)" << std::endl;
        std::cout << "[V2VProxy " << replicaId << "] FALLBACK: Resuming without coordination (letting SUMO handle intersection)" << std::endl;

        // Just resume - let VEINS/SUMO's collision avoidance handle it
        resumeVehicle(0.0);
        waitingForConsensus = false;

    } else {
        DemoBaseApplLayer::handleSelfMsg(msg);
    }
}

void V2VProxyModule::handleLowerMsg(cMessage* msg)
{
    BFTMessage* bftMsg = dynamic_cast<BFTMessage*>(msg);
    if (bftMsg) {
        handleBFTMessage(bftMsg);
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

bool V2VProxyModule::triggerJoinViaJNI()
{
    std::cout << "[V2VProxy " << replicaId << "] triggerJoinViaJNI() called at t=" << simTime() << std::endl;

    if (joinTriggered) {
        std::cout << "[DEBUG V2VProxy " << replicaId << "] JOIN already triggered, skipping" << std::endl;
        return false;
    }

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
    
    // Get the static triggerJoinForReplica method
    jmethodID triggerMethod = env->GetStaticMethodID(serverRunnerClass, "triggerJoinForReplica", "(I)V");
    
    if (triggerMethod) {
        env->CallStaticVoidMethod(serverRunnerClass, triggerMethod, replicaId);

        if (!env->ExceptionCheck()) {
            joinTriggered = true;
            std::cout << "[V2VProxy " << replicaId << "] SUCCESS: Triggered Join at t="
                      << simTime() << std::endl;
            return true; // Success!
        }
    }
    
    return false;
    env->ExceptionClear();

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
        handleBFTMessage(bftMsg);
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
    JavaVMOption options[13];  // Increased for intersection physics parameters
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
        "/home/yash/omnetpp/omnetpp-6.2.0/bftsmart/library/build/install/library/lib/slf4j-api-1.7.32.jar";

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

//forward declare JNI functions from bftsmart_demo_intersection_IntersectionServer.h
extern "C" {
    JNIEXPORT void JNICALL Java_bftsmart_demo_intersection_IntersectionServer_notifyVehicleCanGo
        (JNIEnv*, jobject, jint, jdouble);
}


// Implementation of isRadioBusy native method
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
        {const_cast<char*>("notifyVehicleCanGo"), const_cast<char*>("(ID)V"), (void*)&Java_bftsmart_demo_intersection_IntersectionServer_notifyVehicleCanGo}
    };

    if (env->RegisterNatives(intersectionServerClass, serverMethods, 1) != 0) {
        std::cerr << "[V2VProxy] ERROR: Failed to register IntersectionServer native methods" << std::endl;
        env->ExceptionDescribe();
        env->ExceptionClear();
        return false;
    }

    std::cout << "[V2VProxy] Successfully registered 1 IntersectionServer JNI native method" << std::endl;
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
    DemoBaseApplLayer::handlePositionUpdate(obj);
    // Position updates are handled via checkPositionTimer for efficiency
}

double V2VProxyModule::getDistanceToIntersection()
{
    if (!mobility) {
        return 1e10;  // Very large number if mobility not available
    }

    Coord position = mobility->getPositionAt(simTime());
    double dx = intersectionX - position.x;
    double dy = intersectionY - position.y;
    return std::sqrt(dx*dx + dy*dy);
}

bool V2VProxyModule::isApproachingIntersection()
{
    double distance = getDistanceToIntersection();
    return distance < stopDistance && distance > 0;
}

void V2VProxyModule::stopVehicle()
{
    if (!isStopped && mobility && mobility->getVehicleCommandInterface()) {
        mobility->getVehicleCommandInterface()->setSpeed(0);
        isStopped = true;
        std::cout << "[V2VProxy " << replicaId << "] Vehicle STOPPED at intersection (distance=" << getDistanceToIntersection() << "m)" << std::endl;
    }
}
void V2VProxyModule::resumeVehicle(double delaySeconds)
{
    // This method is called from JNI (Java thread), so we MUST schedule
    std::lock_guard<std::mutex> lock(jniMutex);
    pendingResumeDelays.push(delaySeconds);
    std::cout << "[V2VProxy " << replicaId << "] JNI received GO signal. Queued for main thread." << std::endl;
    // any OMNeT++ operations on the simulation thread via a self-message
 
}

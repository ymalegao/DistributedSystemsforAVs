//
// V2VReliability.cc
// Extracted reliability and Java message bridge helpers for V2VProxyModule
//

#include "veins/modules/bftsmart/V2VProxyModule.h"
#include "veins/base/utils/SimpleAddress.h"
#include "veins/modules/utility/Consts80211p.h"
#include <algorithm>
#include <sstream>

using namespace veins;

bool V2VProxyModule::sendMessageToReplica(int fromReplicaId, int toReplicaId, const uint8_t* data, int dataLen)
{
    std::cout << "[TIMING] QUEUE  replica=" << replicaId << " " << fromReplicaId << "->" << toReplicaId
              << " bytes=" << dataLen << " simT=" << simTime() << "\n";
    
    // Create message
    PendingMessage pendingMsg;
    pendingMsg.fromReplicaId = fromReplicaId;
    pendingMsg.toReplicaId = toReplicaId;
    pendingMsg.data.assign(data, data + dataLen);
    
    // THROTTLE: Wait if queue is full (synchronizes Java with simulation time)
    {
        std::unique_lock<std::mutex> lock(jniMutex);

        if (messageQueue.size() >= MAX_QUEUE_SIZE) {
            std::cout << "[V2V-SEND] Replica " << replicaId << ": DROPPED - Queue is full (" << MAX_QUEUE_SIZE << ")" << "\n";
            return false;
        }
        
        // Queue has space - add message
        std::cout << "[V2V-SEND] Replica " << replicaId << ": QUEUED - Queue size now " << (messageQueue.size() + 1) 
                  << "/" << MAX_QUEUE_SIZE << "\n";
        messageQueue.push(pendingMsg);
    }
    
    return true;
}

// Enqueue a CLIENT_REQUEST_V2V (type=9) for broadcast by the leader.
// Called from Java via nativeBroadcastClientRequest JNI; processes on sim thread.
void V2VProxyModule::enqueueBroadcastClientRequest(int fromReplicaId, const std::vector<uint8_t>& data)
{
    std::cout << "[V2V-SEND] Replica " << replicaId << ": Enqueueing CLIENT_REQUEST_V2V broadcast ("
              << data.size() << " bytes) at t=" << simTime() << "\n";
    PendingMessage pendingMsg;
    pendingMsg.fromReplicaId = fromReplicaId;
    pendingMsg.toReplicaId   = -1;   // broadcast
    pendingMsg.data          = data;
    pendingMsg.messageType   = 9;    // CLIENT_REQUEST_V2V
    std::lock_guard<std::mutex> lock(jniMutex);
    if (messageQueue.size() >= MAX_QUEUE_SIZE) {
        std::cerr << "[V2V-SEND] Replica " << replicaId << ": CLIENT_REQUEST_V2V DROPPED — queue full\n";
        return;
    }
    messageQueue.push(pendingMsg);
}

// Register Java callback object for delivering received messages

void V2VProxyModule::registerJavaCallback(JNIEnv* env, jobject javaObject)
{
    std::cout << "[DEBUG V2VProxy " << replicaId << "] registerJavaCallback called" << "\n";
    
    std::lock_guard<std::mutex> lock(jniMutex);
    
    // Get JVM reference
    env->GetJavaVM(&jvm);
    
    // Create global reference to Java object
    javaCallbackObject = env->NewGlobalRef(javaObject);
    
    // Get the deliverMessage method
    jclass cls = env->GetObjectClass(javaObject);
    deliverMessageMethod = env->GetMethodID(cls, "deliverMessage", "(I[B)V");
    
    if (!deliverMessageMethod) {
        std::cerr << "[ERROR V2VProxy " << replicaId << "] Failed to find deliverMessage method in Java class" << "\n";
        env->ExceptionClear();
    } else {
        std::cout << "[DEBUG V2VProxy " << replicaId << "] Java callback registered successfully" << "\n";
    }
}

// Handle received BFT messages from V2V

void V2VProxyModule::handleBFTMessage(BFTMessage* bftMsg)
{
    std::cout << "[TIMING] ENTRY  replica=" << replicaId << " type=0 simT=" << simTime() << "\n";
    ASSERT(bftMsg);
    syncTimeToJava();

    receivedMessages++;
    emit(bftMsgReceivedSignal, receivedMessages);

    int fromReplicaId = bftMsg->getFromReplicaId();
    int toReplicaId = bftMsg->getToReplicaId();

    std::cout << "[V2V-CONSENSUS] Replica " << replicaId << ": Message #" << receivedMessages
              << " from=" << fromReplicaId << " to=" << toReplicaId
              << " (broadcast=" << (toReplicaId == -1 ? "YES" : "NO") << ")" << "\n";
    
    // Check if message is for us or broadcast
    if (toReplicaId == replicaId || toReplicaId == -1) {
        // std::cout << "[V2V-CONSENSUS] Replica " << replicaId << ": Message IS for us (toReplicaId=" << toReplicaId 
        //           << "), extracting payload and delivering to Java..." << "\n";
        // Get raw binary payload directly (no base64 decoding needed)
        size_t n = bftMsg->getPayloadArraySize();
        std::vector<uint8_t> buf(n);
        for (size_t i = 0; i < n; ++i) buf[i] = bftMsg->getPayload(i);
        // std::cout << "[V2V-CONSENSUS] Replica " << replicaId << ": Extracted " << n 
        //           << " bytes, calling deliverMessageToJava..." << "\n";
        deliverMessageToJava(fromReplicaId, buf.data(), (int)buf.size());
        // std::cout << "[V2V-CONSENSUS] Replica " << replicaId << ": deliverMessageToJava returned" << "\n";




    } else {
        std::cout << "[V2V-CONSENSUS] Replica " << replicaId << ": Message NOT for us (toReplicaId=" << toReplicaId 
                  << " != " << replicaId << "), ignoring broadcast" << "\n";
    }

    // delete bftMsg;
}

// Deliver message to Java through JNI callback

void V2VProxyModule::deliverMessageToJava(int fromReplicaId, const uint8_t* data, int dataLen)
{
    std::cout << "[V2V-JNI] Replica " << replicaId << ": deliverMessageToJava(" << fromReplicaId 
              << ", " << dataLen << " bytes)" << "\n";
    std::lock_guard<std::mutex> lock(jniMutex);
    std::cout << "Got the lock" << "\n";

    if (!jvm || !javaCallbackObject || !deliverMessageMethod) {
        std::cout << "[V2V-JNI] Replica " << replicaId << ": ERROR - Cannot deliver, missing components: "
                  << " jvm=" << (jvm ? "OK" : "NULL")
                  << " callback=" << (javaCallbackObject ? "OK" : "NULL")
                  << " method=" << (deliverMessageMethod ? "OK" : "NULL") << "\n";
        return;  // ← MOVED INSIDE the if block!
    }

    // Log successful delivery (occasionally)
    static int deliverCount = 0;
    if (++deliverCount <= 10 || deliverCount % 100 == 1) {
        std::cout << "[V2VProxy " << replicaId << "] Delivering message #" << deliverCount
                  << " from replica " << fromReplicaId << " to Java (" << dataLen << " bytes)" << "\n";
    }
    
    JNIEnvGuard jniGuard(jvm);
    if (!jniGuard.valid()) {
        std::cerr << "[V2V-JNI] Replica " << replicaId << ": Could not acquire JNIEnv" << "\n";
        return;
    }
    JNIEnv* env = jniGuard.env;

    if (dataLen <= 0 || dataLen > 65536) {
        std::cerr << "[V2V-JNI] Replica " << replicaId << ": Invalid dataLen=" << dataLen << ", skipping" << "\n";
        return;
    }

    jbyteArray javaData = env->NewByteArray(dataLen);
    if (!javaData) {
        std::cerr << "[V2V-JNI] Replica " << replicaId << ": NewByteArray failed" << "\n";
        return;
    }
    env->SetByteArrayRegion(javaData, 0, dataLen, (jbyte*)data);

    std::cout << "[V2V-JNI] Replica " << replicaId << ": Calling Java deliverMessage method..." << "\n";
    std::cout.flush();
    env->CallVoidMethod(javaCallbackObject, deliverMessageMethod, fromReplicaId, javaData);
    std::cout << "[V2V-JNI] Replica " << replicaId << ": Java deliverMessage call returned" << "\n";
    std::cout.flush();

    if (env->ExceptionCheck()) {
        std::cerr << "[V2V-JNI]  Replica " << replicaId << ": JAVA EXCEPTION in deliverMessage!" << "\n";
        env->ExceptionDescribe();
        env->ExceptionClear();
    } else {
        std::cout << "[V2V-JNI] Replica " << replicaId << ": No Java exception, delivery successful" << "\n";
    }

    env->DeleteLocalRef(javaData);
}

// Send BFT message via V2V

void V2VProxyModule::sendBFTMessage(int fromReplicaId, int toReplicaId, const std::vector<uint8_t>& data, int messageType)
{
    
    if (isDeparted && messageType != 0) {
        std::cout << "[V2VProxy " << replicaId << "] ZOMBIE: Blocking V2V message type "
                  << messageType << " (departed)" << "\n";
        return;  // Don't send
    }
    
    
    
    
    
    sentMessages++;
    std::cout << "[V2V-BROADCAST] Replica " << replicaId << ": *** SENDING message #" << sentMessages
              << " from=" << fromReplicaId << " to=" << toReplicaId 
              << " (broadcast=" << (toReplicaId == -1 ? "YES" : "NO") << ")"
              << " msgType=" << messageType << " size=" << data.size() << " bytes at t=" << simTime() << "\n";

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
    std::cout << "[V2V-BROADCAST] Replica " << replicaId << ": Packet created, broadcasting to LAddress::L2BROADCAST()" << "\n";

    // STAGGER STRATEGY (message-type specific for stability + low latency):
    // - ARRIVAL_ECHO (4): per-replica slot stagger so simultaneous echoes don't collide.
    // - ARRIVAL_CERT (5): tiny jitter only — each car broadcasts cert independently.
    // - WITNESS_RESPONSE (2): per-replica slot stagger (witnessSlotSec) to prevent all
    //   15 responses to a given car from colliding in a tight ackJitter window.
    // - BFT consensus (0), arrival announcement (1): deterministic slot stagger.
    double delay;
    if (messageType == 4) {
        // ARRIVAL_ECHO: per-replica slot stagger — echoes from all replicas go back to
        // announcing car; stagger avoids all arriving at once.
        delay = replicaId * par("viewAgreementSlotSec").doubleValue()
                + uniform(par("viewJitterMin").doubleValue(), par("viewJitterMax").doubleValue());
    } else if (messageType == 5) {
        // ARRIVAL_CERT: tiny jitter — each car broadcasts its own cert independently.
        delay = uniform(par("viewJitterMin").doubleValue(), par("viewJitterMax").doubleValue());
    } else if (messageType == 2) {
        // WITNESS_RESPONSE: per-replica slot stagger so all 15 responses to a given car
        // don't pile up in a 1.5ms ackJitter window and collide on the 802.11p channel.
        // 0.5ms slot × 16 replicas = 8ms max window — safely collision-free.
        delay = replicaId * par("witnessSlotSec").doubleValue()
                + uniform(par("ackJitterMin").doubleValue(), par("ackJitterMax").doubleValue());
    } else if (messageType == 6) {
        // READYQC_ACK: fast ACK return, jitter only
        delay = uniform(par("ackJitterMin").doubleValue(), par("ackJitterMax").doubleValue());
    } else if (messageType == 1) {
        // ARRIVAL_ANNOUNCE: larger per-replica slot so each car's witness responses
        // don't collide with other cars' witness responses on the 802.11p channel
        delay = replicaId * par("arrivalSlotSec").doubleValue()
                + uniform(par("broadcastJitterMin").doubleValue(), par("broadcastJitterMax").doubleValue());
    } else {
        // BFT (0), READYQC_COMPLETE (3): deterministic slot stagger
        delay = replicaId * par("broadcastSlotSec").doubleValue()
                + uniform(par("broadcastJitterMin").doubleValue(), par("broadcastJitterMax").doubleValue());
    }
    
    // double microStagger = 0.0;
    
    // // Check if it's a unicast message (assuming -1 is your broadcast ID)
    // if (toReplicaId != -1) {
    //     // 1.5ms gap between each unicast packet sent by THIS car
    //     microStagger = toReplicaId * 0.0015; 
    // }
    
    // delay += microStagger;
    std::cout << "[V2V-BROADCAST] Replica " << replicaId << ": Calling sendDelayed() with delay=" << delay << "s (msgType=" << messageType << ")..." << "\n";
    sendDelayed(bftMsg, delay, lowerLayerOut);
    std::cout << "[V2V-BROADCAST] Replica " << replicaId << ": sendDelayed() returned - packet transmitted to OMNeT++ network layer" << "\n";
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
        // std::cerr << "[Warning] syncTimeToJava skipped: Clock not initialized" << "\n";
        return;
    }
    JNIEnvGuard jniGuard(jvm);
    if (!jniGuard.valid()) return;
    JNIEnv* env = jniGuard.env;

    env->CallStaticVoidMethod(clockClass, updateTimeMethod, simTime().dbl());
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
}

void V2VProxyModule::flushReliabilityQueue() {
    std::cout << "[FLUSH] Replica " << replicaId << ": flushReliabilityQueue() called" << "\n";

    // 1. CLEAR THE QUEUE (Full Wipe - Clean Slate for next round)
    std::queue<PendingMessage> empty;
    std::swap(messageQueue, empty);
    queueCondVar.notify_all();
    
    // 2. CANCEL RETRANSMISSIONS (Always safe to do, queue is empty)
    if (retxCheckTimer && retxCheckTimer->isScheduled()) {
        std::cout << "[FLUSH] Replica " << replicaId << ": CANCELLING retxCheckTimer (Consensus Complete)" << "\n";
        cancelEvent(retxCheckTimer);
    }



    // 3. CONDITIONALLY MANAGE THE MAIN RADIO TIMER
    if (currentPhase == WAITING_FOR_CLEARANCE) {
        // THE CAR IS STAYING: Keep the radio awake for the next round!
        std::cout << "[FLUSH] Replica " << replicaId << ": Car is waiting for next round. Keeping processQueueTimer ALIVE." << "\n";
        
        // (If for some reason it isn't scheduled, start it now)
        if (!processQueueTimer->isScheduled()) {
             scheduleAt(simTime() + 0.05, processQueueTimer);
        }
        
    } else {
        // THE CAR IS LEAVING: Safe to shut down the radio loop.
        if (processQueueTimer && processQueueTimer->isScheduled()) {
            std::cout << "[FLUSH] Replica " << replicaId << ": Car is departing. CANCELLING processQueueTimer!" << "\n";
            cancelEvent(processQueueTimer);
        } else {
            std::cout << "[FLUSH] Replica " << replicaId << ": processQueueTimer already inactive." << "\n";
        }
    }



    std::cout << "[V2VProxy " << replicaId << "] Reliability queue flushed." << "\n";
}

int V2VProxyModule::getMaxMessagesPerTick() {
    // const int CHANNEL_CAPACITY = 100;
    // const double TARGET_UTIL = 0.85;
    // int localviewBatchSize = (int)establishedView.size();
    // if (localviewBatchSize > 0){
    //     std::cout << "using localViewBatchSize" << localviewBatchSize <<  std::endl;
    //     return std::max(2, (int)(CHANNEL_CAPACITY * TARGET_UTIL / localviewBatchSize)); 
    // }
    

    // return std::max(2, (int)(CHANNEL_CAPACITY * TARGET_UTIL / BATCH_SIZE)); 
    const double DSRC_CHANNEL_MBPS = 3.0;          // 802.11p effective rate
    const double MSG_SIZE_BITS     = 750 * 8.0;     // ~750 byte BFT msg
    const double TICK_SEC          = 0.05;          // processQueueTimer interval
    const double TARGET_UTIL       = 0.85;

    int n = std::max(1, (int)establishedView.size());
    double msgs_per_sec = (DSRC_CHANNEL_MBPS * 1e6 / MSG_SIZE_BITS) / n;
    return std::max(2, (int)(msgs_per_sec * TICK_SEC * TARGET_UTIL));   
}

bool V2VProxyModule::checkJavaReplicaStatus() {
    if (!jvm) return false;

    std::lock_guard<std::mutex> lock(jvmMutex);

    JNIEnvGuard jniGuard(jvm);
    if (!jniGuard.valid()) return false;
    JNIEnv* env = jniGuard.env;

    jclass serverRunnerClass = env->FindClass("bftsmart/demo/intersection/ServerRunner");
    if (!serverRunnerClass) return false;

    jmethodID statusMethod = env->GetStaticMethodID(serverRunnerClass,
        "getStatus", "(I)Ljava/lang/String;");
    if (!statusMethod) return false;

    jstring statusStr = (jstring) env->CallStaticObjectMethod(
        serverRunnerClass, statusMethod, replicaId);
    if (!statusStr) return false;

    const char* statusChars = env->GetStringUTFChars(statusStr, nullptr);
    std::string status(statusChars);
    env->ReleaseStringUTFChars(statusStr, statusChars);
    env->DeleteLocalRef(statusStr);
    return (status == "READY");
}

void V2VProxyModule::triggerRetransmissionCheckViaJNI() {
    if (!jvm || !javaReady) return;
    std::lock_guard<std::mutex> lock(jniMutex);

    JNIEnvGuard jniGuard(jvm);
    if (!jniGuard.valid()) return;
    JNIEnv* env = jniGuard.env;

    jclass reliabilityClass = env->FindClass("bftsmart/communication/V2V/ReliableV2VMessaging");
    if (!reliabilityClass) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        return;
    }

    // Step 1: Check retransmissions for this replica only
    jmethodID checkMethod = env->GetStaticMethodID(reliabilityClass, "checkRetransmissionsForReplica", "(I)V");
    if (checkMethod) {
        env->CallStaticVoidMethod(reliabilityClass, checkMethod, (jint)replicaId);
    }
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        return;
    }

    // Step 2: Get pending retransmissions for this replica (as byte arrays)
    jmethodID getRetxMethod = env->GetStaticMethodID(reliabilityClass, "getPendingRetransmissionsForReplica", "(I)[[B");
    if (!getRetxMethod) {
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: getPendingRetransmissionsForReplica method not found!" << "\n";
        return;
    }

    jobjectArray retxArray = (jobjectArray) env->CallStaticObjectMethod(reliabilityClass, getRetxMethod, replicaId);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        return;
    }
    if (retxArray == nullptr) return;

    // Step 3: Send each retransmission via OMNeT++
    jsize retxCount = env->GetArrayLength(retxArray);
    if (retxCount > 0) {
        std::cout << "[V2VProxy " << replicaId << "] Sending " << retxCount << " queued retransmissions" << "\n";
    }

    for (jsize i = 0; i < retxCount; i++) {
        jbyteArray byteArray = (jbyteArray) env->GetObjectArrayElement(retxArray, i);
        if (byteArray == nullptr) continue;

        jsize len = env->GetArrayLength(byteArray);
        std::vector<uint8_t> data(len);
        env->GetByteArrayRegion(byteArray, 0, len, (jbyte*)data.data());

        sendBFTMessage(replicaId, -1, data, 0);
        sentMessages++;
        env->DeleteLocalRef(byteArray);
    }

    env->DeleteLocalRef(retxArray);
}

void V2VProxyModule::handlepreConsensusMessages(BFTMessage* bftMsg) {
    if (zombieFilter()) return;
    if (replicaId < 0) {
        return;
    }

    
    int msgType = bftMsg->getMessageType();

    std::cout << "[V2V-DISPATCH] Replica " << replicaId << ": handlepreConsensusMessages at t=" << simTime() 
              << " msgType=" << msgType;
    
    switch (msgType) {
        case 0:  // BFT_CONSENSUS
            std::cout << " (BFT_CONSENSUS) - forwarding to handleBFTMessage -> Java" << "\n";
            handleBFTMessage(bftMsg);
            break;
        case 1:  // ARRIVAL_ANNOUNCE
            std::cout << " (ARRIVAL_ANNOUNCE)" << "\n";
            handleArrivalAnnouncement(bftMsg);
            break;
        case 4:  // ARRIVAL_ECHO — replica echoes back to announcing car
            std::cout << " (ARRIVAL_ECHO)" << "\n";
            handleArrivalEcho(bftMsg);
            break;
        case 5:  // ARRIVAL_CERT — car broadcasts f+1-echo certificate
            std::cout << " (ARRIVAL_CERT)" << "\n";
            handleArrivalCert(bftMsg);
            break;
        case 7:  // EXECUTING — batch crossing, locks new arrivals
            std::cout << " (EXECUTING)" << "\n";
            handleExecutingMessage(bftMsg);
            break;
        case 9:  // CLIENT_REQUEST_V2V — PROPOSE_ALL TOMMessage broadcast by leader
            std::cout << " (CLIENT_REQUEST_V2V)" << "\n";
            handleClientRequestBroadcast(bftMsg);
            break;
        default:
            std::cout << " (UNKNOWN)" << "\n";
            EV_WARN << "Unknown message type: " << msgType << "\n";
    }
    delete bftMsg;
}

// Delivers a PROPOSE_ALL TOMMessage received via type-9 V2V broadcast to this
// follower's TOMLayer. Runs on the OMNeT++ simulation thread (dequeued by
// processQueueTimer) so it is safe to call JNI here without re-entering OMNeT++.
void V2VProxyModule::handleClientRequestBroadcast(BFTMessage* bftMsg)
{
    std::cout << "[TIMING] ENTRY  replica=" << replicaId << " type=9 simT=" << simTime() << "\n";
    syncTimeToJava();  // fix: was missing, caused stale SimulationClock during PROPOSE generation
    if (!sharedJVM || !intersectionServerGlobalClass || !deliverInjectedClientRequestMethod) {
        EV_WARN << "Replica " << replicaId << ": handleClientRequestBroadcast: JNI not ready, dropping\n";
        return;
    }

    JNIEnv* env;
    bool attached = false;
    if (sharedJVM->GetEnv((void**)&env, JNI_VERSION_1_8) == JNI_EDETACHED) {
        sharedJVM->AttachCurrentThread((void**)&env, nullptr);
        attached = true;
    }

    int dataLen = static_cast<int>(bftMsg->getPayloadArraySize());
    jbyteArray jba = env->NewByteArray(dataLen);
    std::vector<uint8_t> buf(dataLen);
    for (int i = 0; i < dataLen; i++) buf[i] = static_cast<uint8_t>(bftMsg->getPayload(i));
    env->SetByteArrayRegion(jba, 0, dataLen, reinterpret_cast<const jbyte*>(buf.data()));

    env->CallStaticVoidMethod(intersectionServerGlobalClass,
                              deliverInjectedClientRequestMethod,
                              static_cast<jint>(replicaId), jba);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
    env->DeleteLocalRef(jba);

    if (attached) sharedJVM->DetachCurrentThread();
}

//
// V2VProxyModule.cc
// Implementation of V2V Proxy Module for BFT-SMaRt
//

#include "veins/modules/bftsmart/V2VProxyModule.h"
#include "veins/modules/bftsmart/crypto/CryptoAuth.h"
#include "veins/base/utils/SimpleAddress.h"
#include "veins/modules/utility/Consts80211p.h"
#include <algorithm>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <limits>
#include <cmath>
#include <cctype>
#include <openssl/evp.h>

#define XXH_INLINE_ALL
#include "xxhash.h"

using namespace veins;

// Forward declarations for static helpers defined later in this file
static std::string dirToStr(V2VProxyModule::Direction d);
static V2VProxyModule::Direction strToDir(const std::string& s);

static int completedConsensusCount = 0;
static const int BATCH_SIZE = 16;
static bool logged100m = false;
static std::map<int, std::map<int, double>> orderLatencyByEpochAndReplica;
static std::set<int> printedOrderLatencyAvgEpochs;
static std::map<int, std::map<int, double>> orderBftRttByEpochAndReplica;
static std::set<int> printedOrderBftRttAvgEpochs;
static std::map<int, std::map<int, double>> viewLatencyByEpochAndReplica;
static std::set<int> printedViewLatencyAvgEpochs;
static std::map<int, std::map<int, double>> orderQcWindowByEpochAndReplica;
static std::set<int> printedOrderQcWindowAvgEpochs;
static std::map<int, std::map<int, double>> resetToViewEndByEpochAndReplica;
static std::set<int> printedResetToViewEndAvgEpochs;
static std::map<int, int> stopSignFailuresByEpoch;
static std::map<int, std::map<int,int>>    messagesSentByEpochAndReplica;
static std::map<int, std::map<int,int>>    messagesRecvByEpochAndReplica;
static std::map<int, std::map<int,double>> totalConsDurByEpochAndReplica;
static std::set<int> printedMsgAvgEpochs;
static std::set<int> printedTotalDurAvgEpochs;
// Base64 encoding table

bool V2VProxyModule::zombieFilter() {
    if (isDeparted) {
        std::cout << "[V2VProxy " << replicaId << "] ZOMBIE: Not executing action (departed)" << "\n";
        return true;
    }
    return false;
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
    , stopSignTimeoutTimer(nullptr)
    , shouldFlush(false)
    , consensusTimeoutSec(80.0)  // Default 40 seconds
    , stopSignTimeoutSec(10.0)
    , MAX_MESSAGES_PER_TICK(BATCH_SIZE)  // Will be updated by notifyJavaNewBatchSize() when BFT group size is known
    , startReadyQCCollectionMsg(nullptr)
    , consensusStartTime(0)
    , viewConsensusStartTime(0)
    , viewConsensusEndTime(0)
    , orderConsensusStartTime(0)
    , orderConsensusEndTime(0)
    , orderCollectDeadlineTimer(nullptr)
    , orderGossipRetransmitTimer(nullptr)
    , orderBagRetransmitTimer(nullptr)
    , orderDelayTimer(nullptr)
    , orderCollectionActive(false)
    , orderBagProposed(false)
    , orderDecisionReceived(false)
    , delayedOrderSubmitScheduled(false)
    , orderCollectionDeadline(0)
    , orderBagRetransmitCount(0)
    , orderDelayGap(0.0)
    , pendingOrderEpoch(-1)
    , pendingOrderViewHash(0)
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
    cancelAndDelete(stopSignTimeoutTimer);
    cancelAndDelete(checkJavaReadyTimer);
    cancelAndDelete(startReadyQCCollectionMsg);
    cancelAndDelete(retxCheckTimer);   
    cancelAndDelete(readyQCTimeoutTimer);
    cancelAndDelete(orderCollectDeadlineTimer);
    cancelAndDelete(orderGossipRetransmitTimer);
    cancelAndDelete(orderBagRetransmitTimer);
    cancelAndDelete(orderDelayTimer);
    // Clean up JNI global reference
    if (javaCallbackObject && jvm) {
        JNIEnv* env;
        jvm->AttachCurrentThread((void**)&env, nullptr);
        env->DeleteGlobalRef(javaCallbackObject);
        javaCallbackObject = nullptr;
    }

    if (ambulancePrivateKey) {
        EVP_PKEY_free(static_cast<EVP_PKEY*>(ambulancePrivateKey));
        ambulancePrivateKey = nullptr;
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
                          << ". Officially setting my replicaId to " << replicaId << "!" << "\n";
            } catch (...) {
                std::cerr << "[ERROR] Could not parse replica ID from " << sumoId << "\n";
                replicaId = -1;
            }
        } else {
            replicaId = -1; // Catches RSUs or pedestrians
        }

        if (replicaId < 0) {
            std::cerr << "[ERROR V2VProxy] Invalid replicaId (" << sumoId << "), skipping initialization." << "\n";
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
        stopSignTimeoutSec = par("stopSignTimeoutSec").doubleValue();
        orderDelayGap = par("orderDelayGap").doubleValue();
        isByzantine = par("isByzantine").boolValue();
        byzantineType = static_cast<ByzantineType>(par("byzantineType").intValue());
        if (isByzantine) {
            std::cout << "[BYZANTINE] Replica " << replicaId
                      << " is BYZANTINE, type=" << byzantineType << "\n";
        }

        {
            int ambTarget = par("ambulanceReplicaId").intValue();
            if (ambTarget >= 0)
                moduleIsAmbulance = (replicaId == ambTarget);
            else
                moduleIsAmbulance = par("isAmbulance").boolValue();
        }

        if (moduleIsAmbulance) {
            if (ambulancePrivateKey) {
                EVP_PKEY_free(static_cast<EVP_PKEY*>(ambulancePrivateKey));
                ambulancePrivateKey = nullptr;
            }
            myAmbulanceCertBytes.clear();
            uint8_t pub[CRYPTO_PUBKEY_BYTES];
            EVP_PKEY* pk = CryptoAuth::instance().generateKeyPair(pub);
            VehicleCert cert = CryptoAuth::instance().issueCert(pub, "ambulance", "Emergency_CA");
            myAmbulanceCertBytes.assign(reinterpret_cast<const uint8_t*>(&cert),
                                        reinterpret_cast<const uint8_t*>(&cert) + sizeof(VehicleCert));
            ambulancePrivateKey = pk;
            std::cout << "[AMBULANCE] Replica " << replicaId << " auto-issued Emergency_CA cert ("
                      << myAmbulanceCertBytes.size() << " bytes, sizeof(VehicleCert)=" << sizeof(VehicleCert)
                      << ")" << "\n";
        }

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
        stopSignTimeoutTimer = new cMessage("stopSignTimeout");

        // Create timers for ORDER bag gossip/collection phase
        orderCollectDeadlineTimer = new cMessage("orderCollectDeadline");
        orderGossipRetransmitTimer = new cMessage("orderGossipRetransmit");
        orderBagRetransmitTimer = new cMessage("orderBagRetransmit");
        orderDelayTimer = new cMessage("orderDelaySubmit");

        // Create timer for checking Java readiness
        checkJavaReadyTimer = new cMessage("checkJavaReady");
        retxCheckTimer = new cMessage("retransmissionCheck");
        readyQCTimeoutTimer = new cMessage("readyQCRetransmit");
        
        
        

        scheduleAt(simTime() + 0.02, retxCheckTimer);
        scheduleAt(simTime() + 0.5, checkJavaReadyTimer); // Start checking after 0.5s

        EV_INFO << "V2VProxyModule initialized for replica " << replicaId << "\n";
        std::cout << "[V2VProxy " << replicaId << "] Intersection at (" << intersectionX << ", " << intersectionY << "), stop distance=" << stopDistance << "m" << "\n";

        // Register in global map for JNI lookup
        {
            std::lock_guard<std::mutex> lock(registryMutex);
            replicaProxyMap[replicaId] = this;
        }
        
        std::cout << "[DEBUG V2VProxy " << replicaId << "] Replica registered in proxy map" << "\n";
        
        // Start periodic timer to poll message queue (since Java threads can't call scheduleAt)
        scheduleAt(simTime() + 0.1, processQueueTimer);  // Start polling after 100ms
        std::cout << "[DEBUG V2VProxy " << replicaId << "] Periodic queue timer started" << "\n";
        std::cout << "[DEBUG V2VProxy " << replicaId << "] Timer scheduled for t=" << (simTime() + 0.1) 
                  << ", isScheduled=" << (processQueueTimer->isScheduled() ? "YES" : "NO") << "\n";
        
        // Create or attach to JVM
        if (createOrAttachJVM()) {
            std::cout << "[DEBUG V2VProxy " << replicaId << "] JVM ready" << "\n";
            
            // Start BFTSmart replica in Java
            startBFTSmartReplica();
        } else {
            std::cerr << "[ERROR V2VProxy " << replicaId << "] Failed to initialize JVM" << "\n";
        }
        
        // Start position checking to trigger consensus when approaching intersection
        // With StateManager bypass, replicas initialize in ~1-2s, so start checking early
        scheduleAt(simTime() + 0.5, checkPositionTimer);
        std::cout << "[DEBUG V2VProxy " << replicaId << "] Position checking will start at t=0.5s" << "\n";
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
    
    EV_INFO << "V2VProxyModule replica " << replicaId << " statistics:" << "\n";
    EV_INFO << "  Messages sent: " << sentMessages << "\n";
    EV_INFO << "  Messages received: " << receivedMessages << "\n";
    std::cout << "[METRICS " << replicaId << "] Messages received: " << receivedMessages << "\n";
    std::cout << "[METRICS " << replicaId << "] Messages sent: " << sentMessages << "\n";
    

    std::cout << "[DEBUG V2VProxy " << replicaId << "] Finishing V2VProxyModule" << "\n";
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
              << fromReplicaId << "->" << toReplicaId << ", " << dataLen << " bytes) at t=" << simTime() << "\n";
    
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
    } else {
        std::cout << "[DEBUG V2VProxy " << replicaId << "] Java callback registered successfully" << "\n";
    }
}

// Handle received BFT messages from V2V
void V2VProxyModule::handleBFTMessage(BFTMessage* bftMsg)
{
    std::cout << "[V2V-CONSENSUS] Replica " << replicaId << ": handleBFTMessage (consensus msg) at t=" << simTime() << "\n";
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
    
    JNIEnv* env;
    jvm->AttachCurrentThread((void**)&env, nullptr);
    
    // Create Java byte array
    jbyteArray javaData = env->NewByteArray(dataLen);
    env->SetByteArrayRegion(javaData, 0, dataLen, (jbyte*)data);
    
    // Call Java method
    std::cout << "[V2V-JNI] Replica " << replicaId << ": Calling Java deliverMessage method..." << "\n";
    std::cout.flush();
    env->CallVoidMethod(javaCallbackObject, deliverMessageMethod, fromReplicaId, javaData);
    std::cout << "[V2V-JNI] Replica " << replicaId << ": Java deliverMessage call returned" << "\n";
    std::cout.flush();
    
    // Check for exceptions - PRINT THEM, don't silently hide!
    if (env->ExceptionCheck()) {
        std::cerr << "[V2V-JNI]  Replica " << replicaId << ": JAVA EXCEPTION in deliverMessage!" << "\n";
        std::cerr << "[V2V-JNI] Exception details:" << "\n";
        env->ExceptionDescribe();  // Print full stack trace to stderr
        env->ExceptionClear();
        std::cerr << "[V2V-JNI] Exception cleared, continuing..." << "\n";
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
    // - VIEW_PROPOSAL (4): tiny jitter only — replicas must sample the car-set simultaneously.
    // - VIEW_AGREEMENT (5): per-replica slot + tiny jitter — stagger replies to proposer.
    // - WITNESS_RESPONSE (2): per-replica slot stagger (witnessSlotSec) to prevent all
    //   15 responses to a given car from colliding in a tight ackJitter window.
    // - READYQC_ACK (6): jitter-only — fast ACK return path.
    // - BFT consensus (0), arrival announcement (1), READYQC_COMPLETE (3): deterministic
    //   slot stagger (tighter 3ms slot) to avoid collisions while reducing tail delay.
    double delay;
    if (messageType == 4) {
        // VIEW_PROPOSAL: tiny jitter only — all replicas must sample the car-set
        // at nearly the same sim-time so their views agree.
        delay = uniform(par("viewJitterMin").doubleValue(), par("viewJitterMax").doubleValue());
    } else if (messageType == 5) {
        // VIEW_AGREEMENT: per-replica slot stagger so 15 simultaneous replies
        // don't all collide at the proposer.  View is already sampled at proposal
        // receipt, so staggering the reply is safe.
        delay = replicaId * par("viewAgreementSlotSec").doubleValue()
                + uniform(par("viewJitterMin").doubleValue(), par("viewJitterMax").doubleValue());
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

    // Step 1: Check retransmissions for this replica only (H2 fix: was checkRetransmissionsForAllReplicas)
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

    if (retxArray == nullptr) {
        return; // No retransmissions
    }

    // Step 3: Send each retransmission via OMNeT++ (now safe - not in JNI call)
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

        // Use sendBFTMessage for proper broadcast - sets fromReplicaId, toReplicaId=-1,
        // recipientAddress, channel, etc. The envelope has correct from/to inside payload.
        std::cout << "[V2VProxy " << replicaId << "] sending retransmission #" << i << " from replica " << replicaId << " to all replicas" << "\n";
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
                  << " at t=" << simTime() << " msgName=" << msg->getName() << "\n";
    }

    // =========================================================================
    // 1. PROCESS QUEUE TIMER (Heartbeat)
    // =========================================================================
    if (msg == processQueueTimer) {
        // std::cout << "[V2V-QUEUE-TIMER] Replica " << replicaId << ": *** TIMER FIRED at t=" << simTime() << " ***" << "\n";
        
        // Process queued messages
        std::vector<PendingMessage> toProcess;
        {
            std::lock_guard<std::mutex> lock(jniMutex);
            int count = 0;
            // std::cout << "[V2V-QUEUE] Replica " << replicaId << ": Processing " << messageQueue.size() 
            //           << " queued message(s) at t=" << simTime() << "\n";

            while (!messageQueue.empty()) {
                toProcess.push_back(messageQueue.front());
                messageQueue.pop();
                count++;
            }
            if (messageQueue.size() < MAX_QUEUE_SIZE) {
                queueCondVar.notify_all();
            }
        }
        
        if (!toProcess.empty()) {
            std::cout << "[V2V-QUEUE] Replica " << replicaId << ": Dequeuing " << toProcess.size() << " messages" << "\n";
            for (size_t i = 0; i < toProcess.size(); i++) {
                const auto& pending = toProcess[i];
                std::cout << "[V2V-QUEUE] Replica " << replicaId << ": [" << (i+1) << "/" << toProcess.size() 
                          << "] Sending " << pending.fromReplicaId << "->" << pending.toReplicaId 
                          << ", " << pending.data.size() << " bytes" << "\n";
                sendBFTMessage(pending.fromReplicaId, pending.toReplicaId, pending.data, 0);
            }
        }

        // Phase 2 is gone in new protocol — Java leader handles ORDER internally.
        // (phase2Pending check removed)

        // RESUME LOGIC (Moved inside timer for reliability)
        {
            std::lock_guard<std::mutex> lock(jniMutex); 
            
            if (!pendingResumeDelays.empty()) {
                shouldFlush = true;
                std::cout << "[RESUME] Replica " << replicaId << ": pendingResumeDelays NOT empty (" 
                          << pendingResumeDelays.size() << "), setting shouldFlush=true" << "\n";
            }
                
            while (!pendingResumeDelays.empty()) {
                completedConsensusCount++;
                double delay = pendingResumeDelays.front();
                pendingResumeDelays.pop();
                
                // Mark the end of the local post-decision pipeline on the simulation thread.
                orderConsensusEndTime = simTime();
                if (!orderDecisionCallbackSeen) {
                    // Fallback when callback timing was not captured (should be rare).
                    realOrderConsensusEnd = std::chrono::high_resolution_clock::now();
                }
                if (orderDecisionCallbackSeen && realOrderConsensusStart.time_since_epoch().count() > 0) {
                    auto realOrderConsensusDuration =
                        std::chrono::duration_cast<std::chrono::milliseconds>(realOrderConsensusEnd - realOrderConsensusStart);
                    lastOrderBftRequestRttMs = static_cast<double>(realOrderConsensusDuration.count());
                    std::cout << "[METRICS " << replicaId
                              << "] Order_BFT_Request_RTT_ms: " << lastOrderBftRequestRttMs << "\n";
                } else {
                    lastOrderBftRequestRttMs = -1.0;
                }
                
                
                // Compute robust timing anchors before deriving durations.
                // Some callbacks run on followers without setting all start fields.
                simtime_t effectiveViewStart = viewConsensusStartTime;
                bool usedViewStartFallback = false;
                if (effectiveViewStart <= 0) {
                    if (viewSignatureCollectionEndTime > 0) {
                        effectiveViewStart = viewSignatureCollectionEndTime;
                    } else if (viewSignatureCollectionStartTime > 0) {
                        effectiveViewStart = viewSignatureCollectionStartTime;
                    } else {
                        effectiveViewStart = consensusStartTime;
                    }
                    usedViewStartFallback = true;
                }
                simtime_t effectiveViewEnd = viewConsensusEndTime;
                if (effectiveViewEnd < effectiveViewStart) {
                    effectiveViewEnd = effectiveViewStart;
                }
                simtime_t viewConsensusDuration = effectiveViewEnd - effectiveViewStart;

                simtime_t effectiveOrderStart = orderConsensusStartTime;
                bool usedOrderStartFallback = false;
                if (effectiveOrderStart <= 0) {
                    if (orderSignatureCollectionEndTime > 0) {
                        effectiveOrderStart = orderSignatureCollectionEndTime;
                    } else if (orderSignatureCollectionStartTime > 0) {
                        effectiveOrderStart = orderSignatureCollectionStartTime;
                    } else {
                        effectiveOrderStart = orderConsensusEndTime;
                    }
                    usedOrderStartFallback = true;
                }
                simtime_t effectiveOrderEnd = orderConsensusEndTime;
                if (effectiveOrderEnd < effectiveOrderStart) {
                    effectiveOrderEnd = effectiveOrderStart;
                }
                simtime_t orderConsensusDuration = effectiveOrderEnd - effectiveOrderStart;

                simtime_t effectiveTotalStart = consensusStartTime;
                if (effectiveTotalStart <= 0) {
                    effectiveTotalStart = effectiveViewStart;
                }
                if (effectiveOrderEnd < effectiveTotalStart) {
                    effectiveTotalStart = effectiveOrderEnd;
                }
                simtime_t totalConsensusDuration = effectiveOrderEnd - effectiveTotalStart;

                simtime_t effectiveOrderWindowStart = orderCollectionWindowStart;
                bool usedGossipFallback = false;
                if (effectiveOrderWindowStart <= 0 || orderCollectionWindowEnd < effectiveOrderWindowStart) {
                    effectiveOrderWindowStart = orderCollectionWindowEnd;
                    usedGossipFallback = true;
                }
                simtime_t laneLeaderGossipDuration = orderCollectionWindowEnd - effectiveOrderWindowStart;

                // --- Phase timeline (sim time, seconds): V2V collect → VIEW BFT → ORDER prep → ORDER BFT ---
                auto fmtPhaseSec = [](double v) -> std::string {
                    if (v < 0.0 || std::isnan(v)) {
                        return std::string("N/A");
                    }
                    std::ostringstream o;
                    o << std::fixed << std::setprecision(4) << v << "s";
                    return o.str();
                };
                const double durViewV2vSig =
                    (viewSignatureCollectionEndTime > viewSignatureCollectionStartTime
                     && viewSignatureCollectionStartTime > 0)
                        ? (viewSignatureCollectionEndTime - viewSignatureCollectionStartTime).dbl()
                        : -1.0;
                const double gapV2vToViewBft =
                    (viewConsensusStartTime > 0 && viewSignatureCollectionEndTime > 0
                     && viewConsensusStartTime >= viewSignatureCollectionEndTime)
                        ? (viewConsensusStartTime - viewSignatureCollectionEndTime).dbl()
                        : -1.0;
                const double durViewBftSim = viewConsensusDuration.dbl();
                const double gapViewBftToOrderSig =
                    (orderSignatureCollectionStartTime > 0 && viewConsensusEndTime > 0
                     && orderSignatureCollectionStartTime >= viewConsensusEndTime)
                        ? (orderSignatureCollectionStartTime - viewConsensusEndTime).dbl()
                        : -1.0;
                const double durOrderSigCollect =
                    (orderSignatureCollectionEndTime > orderSignatureCollectionStartTime
                     && orderSignatureCollectionStartTime > 0)
                        ? (orderSignatureCollectionEndTime - orderSignatureCollectionStartTime).dbl()
                        : -1.0;
                const double gapOrderSigToOrderBft =
                    (orderConsensusStartTime > 0 && orderSignatureCollectionEndTime > 0
                     && orderConsensusStartTime >= orderSignatureCollectionEndTime)
                        ? (orderConsensusStartTime - orderSignatureCollectionEndTime).dbl()
                        : -1.0;
                const double durOrderBftSim = orderConsensusDuration.dbl();
                const double durLaneGossip = laneLeaderGossipDuration.dbl();
                
                std::cout << "\n========== CONSENSUS METRICS (Replica " << replicaId << ") epoch=" << currentEpoch
                          << " ==========" << "\n";
                std::cout << "[PHASE_SUMMARY " << replicaId << "] "
                          << "V2V_VIEW_SIG=" << fmtPhaseSec(durViewV2vSig)
                          << " gap_to_VIEW_BFT=" << fmtPhaseSec(gapV2vToViewBft)
                          << " VIEW_BFT(sim)=" << fmtPhaseSec(durViewBftSim)
                          << " gap_to_ORDER_SIG=" << fmtPhaseSec(gapViewBftToOrderSig)
                          << " ORDER_SIG_collect=" << fmtPhaseSec(durOrderSigCollect)
                          << " gap_to_ORDER_BFT=" << fmtPhaseSec(gapOrderSigToOrderBft)
                          << " lane_gossip=" << fmtPhaseSec(durLaneGossip)
                          << " ORDER_BFT(sim)=" << fmtPhaseSec(durOrderBftSim)
                          << " total_pipeline(sim)=" << fmtPhaseSec(totalConsensusDuration.dbl())
                          << "\n";

                // View Signature Collection Metrics
                // View Consensus Metrics
                std::cout << "[METRICS " << replicaId << "] === VIEW SIGNATURE COLLECTION (V2V f+1) ===" << "\n";
                std::cout << "[METRICS " << replicaId << "] View_Signature_Collection_Start: " << viewSignatureCollectionStartTime << "\n";
                std::cout << "[METRICS " << replicaId << "] View_Signature_Collection_End: " << viewSignatureCollectionEndTime << "\n";
                std::cout << "[METRICS " << replicaId << "] View_Signature_Collection_Duration: " << fmtPhaseSec(durViewV2vSig) << "\n";

                std::cout << "[METRICS " << replicaId << "] === VIEW CONSENSUS (BFT VIEW_PROPOSE → delivery) ===" << "\n";
                std::cout << "[METRICS " << replicaId << "] View_Consensus_Start: " << effectiveViewStart << "\n";
                std::cout << "[METRICS " << replicaId << "] View_Consensus_End:   " << effectiveViewEnd << "\n";
                std::cout << "[METRICS " << replicaId << "] View_Consensus_Latency: " << fmtPhaseSec(durViewBftSim) << "\n";

                // Per-round metric: average view consensus latency over 4 cars in this epoch
                viewLatencyByEpochAndReplica[currentEpoch][replicaId] = viewConsensusDuration.dbl();
                {
                    const auto& epochViewLatencies = viewLatencyByEpochAndReplica[currentEpoch];
                    if (epochViewLatencies.size() >= 4 && printedViewLatencyAvgEpochs.count(currentEpoch) == 0) {
                        double sumLatency = 0.0;
                        for (const auto& kv : epochViewLatencies) {
                            sumLatency += kv.second;
                        }
                        double avgLatency = sumLatency / epochViewLatencies.size();
                        printedViewLatencyAvgEpochs.insert(currentEpoch);
                        std::cout << "[ROUND-METRICS] Epoch " << currentEpoch
                                  << " Avg_View_Consensus_Latency_4Cars: " << avgLatency
                                  << " seconds (replicasCounted=" << epochViewLatencies.size() << ")" << "\n";
                    }
                }

                // Order Signature Collection Metrics
                std::cout << "[METRICS " << replicaId << "] === ORDER SIGNATURE / QC COLLECTION ===" << "\n";
                
                std::cout << "[METRICS " << replicaId << "] Order_Signature_Collection_Start: " << orderSignatureCollectionStartTime << "\n";
                std::cout << "[METRICS " << replicaId << "] Order_Signature_Collection_End: " << orderSignatureCollectionEndTime << "\n";
                std::cout << "[METRICS " << replicaId << "] Order_Signature_Collection_Duration: " << fmtPhaseSec(durOrderSigCollect) << "\n";

                std::cout << "[METRICS " << replicaId << "] LaneLeader_Gossip_Duration: " << fmtPhaseSec(durLaneGossip) << "\n";

                // Order Consensus Metrics
                std::cout << "[METRICS " << replicaId << "] === ORDER CONSENSUS (BFT ORDER_PROPOSE → decision) ===" << "\n";
                std::cout << "[METRICS " << replicaId << "] Order_Consensus_Start: " << effectiveOrderStart << "\n";
                std::cout << "[METRICS " << replicaId << "] Order_Consensus_End:   " << effectiveOrderEnd << "\n";
                std::cout << "[METRICS " << replicaId << "] Order_Consensus_Latency: " << fmtPhaseSec(durOrderBftSim) << "\n";
                if (lastOrderBftRequestRttMs >= 0.0) {
                    std::cout << "[METRICS " << replicaId
                              << "] Order_BFT_Request_RTT: " << (lastOrderBftRequestRttMs / 1000.0)
                              << " seconds" << "\n";
                } else {
                    std::cout << "[METRICS " << replicaId
                              << "] Order_BFT_Request_RTT: N/A (no local ORDER submit)" << "\n";
                }
                
                // Overall Metrics
                std::cout << "[METRICS " << replicaId << "] === OVERALL (first stop → ORDER decision) ===" << "\n";
                std::cout << "[METRICS " << replicaId << "] Total_Consensus_Start: " << effectiveTotalStart << "\n";
                std::cout << "[METRICS " << replicaId << "] Total_Consensus_End:   " << effectiveOrderEnd << "\n";
                std::cout << "[METRICS " << replicaId << "] Total_Consensus_Duration: " << fmtPhaseSec(totalConsensusDuration.dbl()) << "\n";
                if (usedViewStartFallback || usedOrderStartFallback || usedGossipFallback) {
                    std::cout << "[METRICS " << replicaId << "] Timing_Fallbacks:"
                              << " viewStart=" << (usedViewStartFallback ? "1" : "0")
                              << " orderStart=" << (usedOrderStartFallback ? "1" : "0")
                              << " gossipWindow=" << (usedGossipFallback ? "1" : "0")
                              << "\n";
                }

                // Per-round metric: average order consensus latency over 4 cars in this epoch.
                orderLatencyByEpochAndReplica[currentEpoch][replicaId] = orderConsensusDuration.dbl();
                const auto& epochLatencies = orderLatencyByEpochAndReplica[currentEpoch];
                if (epochLatencies.size() >= 4 && printedOrderLatencyAvgEpochs.count(currentEpoch) == 0) {
                    double sumLatency = 0.0;
                    for (const auto& kv : epochLatencies) {
                        sumLatency += kv.second;
                    }
                    double avgLatency = sumLatency / epochLatencies.size();
                    printedOrderLatencyAvgEpochs.insert(currentEpoch);
                    std::cout << "[ROUND-METRICS] Epoch " << currentEpoch
                              << " Avg_Order_Consensus_Latency_4Cars: " << avgLatency
                              << " seconds (replicasCounted=" << epochLatencies.size() << ")" << "\n";

                    // Stop-sign failures for this epoch
                    int nFail = stopSignFailuresByEpoch.count(currentEpoch) ?
                                stopSignFailuresByEpoch.at(currentEpoch) : 0;
                    std::cout << "[ROUND-METRICS] Epoch " << currentEpoch
                              << " Avg_StopSign_Failures: " << (double)nFail
                              << " (count)" << "\n";

                    // Average messages sent/received per replica
                    if (printedMsgAvgEpochs.count(currentEpoch) == 0) {
                        auto& sm = messagesSentByEpochAndReplica[currentEpoch];
                        auto& rm = messagesRecvByEpochAndReplica[currentEpoch];
                        if (!sm.empty()) {
                            double avgSent = 0, avgRecv = 0;
                            for (auto& kv : sm) avgSent += kv.second;
                            for (auto& kv : rm) avgRecv += kv.second;
                            avgSent /= sm.size(); avgRecv /= rm.size();
                            std::cout << "[ROUND-METRICS] Epoch " << currentEpoch
                                      << " Avg_Messages_Sent_PerReplica: " << avgSent
                                      << " (count)" << "\n";
                            std::cout << "[ROUND-METRICS] Epoch " << currentEpoch
                                      << " Avg_Messages_Received_PerReplica: " << avgRecv
                                      << " (count)" << "\n";
                            printedMsgAvgEpochs.insert(currentEpoch);
                        }
                    }

                    // Average total consensus duration (stop→resume, GO cars only)
                    if (printedTotalDurAvgEpochs.count(currentEpoch) == 0) {
                        auto& td = totalConsDurByEpochAndReplica[currentEpoch];
                        if (td.size() >= 4) {
                            double sum = 0;
                            for (auto& kv : td) sum += kv.second;
                            std::cout << "[ROUND-METRICS] Epoch " << currentEpoch
                                      << " Avg_Total_Consensus_Duration: " << (sum / td.size())
                                      << " seconds (replicasCounted=" << td.size() << ")" << "\n";
                            printedTotalDurAvgEpochs.insert(currentEpoch);
                        }
                    }
                }
                if (lastOrderBftRequestRttMs >= 0.0) {
                    orderBftRttByEpochAndReplica[currentEpoch][replicaId] = lastOrderBftRequestRttMs / 1000.0;
                }
                const auto& epochBftRtt = orderBftRttByEpochAndReplica[currentEpoch];
                if (!epochBftRtt.empty() && printedOrderBftRttAvgEpochs.count(currentEpoch) == 0) {
                    double sumRtt = 0.0;
                    for (const auto& kv : epochBftRtt) {
                        sumRtt += kv.second;
                    }
                    double avgRtt = sumRtt / epochBftRtt.size();
                    printedOrderBftRttAvgEpochs.insert(currentEpoch);
                    std::cout << "[ROUND-METRICS] Epoch " << currentEpoch
                              << " Avg_Order_BFT_Request_RTT_Submitter: " << avgRtt
                              << " seconds (submittersCounted=" << epochBftRtt.size() << ")" << "\n";
                }
                
                // Vehicle timing
                simtime_t scheduledResumeTime = orderConsensusEndTime + delay;
                simtime_t totalWaitTime = scheduledResumeTime - stopTime;
                
                std::cout << "[METRICS " << replicaId << "] Stop_Time: " << stopTime << "\n";
                std::cout << "[METRICS " << replicaId << "] Projected_Total_Wait: " << totalWaitTime.dbl() << " seconds" << "\n";
                std::cout << "[METRICS " << replicaId << "] Scheduled_Resume: " << scheduledResumeTime.dbl() << "\n";
                
                // Message statistics
                std::cout << "[METRICS " << replicaId << "] Messages_Sent: " << sentMessages << "\n";
                std::cout << "[METRICS " << replicaId << "] Messages_Received: " << receivedMessages << "\n";

                // Accumulate per-epoch message/duration metrics
                messagesSentByEpochAndReplica[currentEpoch][replicaId] = (int)sentMessages;
                messagesRecvByEpochAndReplica[currentEpoch][replicaId] = (int)receivedMessages;
                totalConsDurByEpochAndReplica[currentEpoch][replicaId] = totalConsensusDuration.dbl();

                std::cout << "========================================================\n" << "\n";
                
                // Cancel timeout timer - consensus succeeded!
                if (consensusTimeoutTimer->isScheduled()) {
                    cancelEvent(consensusTimeoutTimer);
                    std::cout << "[METRICS " << replicaId << "] (consensus succeeded : TRUE)" << "\n";
                }
                if (stopSignTimeoutTimer->isScheduled()) {
                    cancelEvent(stopSignTimeoutTimer);
                }
    
                // Schedule the ACTUAL movement message
                std::cout << "[V2VProxy " << replicaId << "] Main thread scheduling resume in " << delay << "s" << "\n";
                cMessage* moveMsg = new cMessage("resumeVehicle");
                scheduleAt(simTime() + delay, moveMsg);
            }

            // Drain pending ORDER decision queued by handleOrderDecision() on a JNI thread.
            // cancelEvent() is safe here because we are on the OMNeT++ main thread.
            // parseAndNotifyDecision() is called AFTER the lock is released because it
            // calls resumeVehicle() which re-acquires jniMutex (would deadlock if still held).
            if (pendingCancelOrderTimer) {
                pendingCancelOrderTimer = false;
                if (orderDelayTimer && orderDelayTimer->isScheduled()) {
                    cancelEvent(orderDelayTimer);
                    std::cout << "[ORDER] Replica " << replicaId
                              << ": cancelled orderDelayTimer on main thread" << "\n";
                }
                // ORDER consensus succeeded — no need for the stop-sign fallback any more.
                // Vehicles in later batches would otherwise time out before their batch fires.
                if (stopSignTimeoutTimer && stopSignTimeoutTimer->isScheduled()) {
                    cancelEvent(stopSignTimeoutTimer);
                    std::cout << "[ORDER] Replica " << replicaId
                              << ": cancelled stopSignTimeoutTimer (ORDER decided)" << "\n";
                }
            }
        }  // jniMutex released here — safe to call parseAndNotifyDecision now

        {
            std::string decision;
            {
                std::lock_guard<std::mutex> lock(jniMutex);
                decision = pendingOrderDecision;
                pendingOrderDecision.clear();
            }
            if (!decision.empty()) {
                std::cout << "[ORDER] Replica " << replicaId
                          << ": main thread processing deferred ORDER decision: " << decision << "\n";
                parseAndNotifyDecision(decision);
            }
        }

        // if (completedConsensusCount == BATCH_SIZE) {
        //     std::cout << "[METRICS " << replicaId << "] All Consensus Completed" << "\n";
        //     endSimulation();
        // }
        
        // Reschedule timer
        // std::cout << "[V2V-QUEUE-TIMER] Replica " << replicaId << ": Rescheduling for t=" 
        //           << (simTime() + 0.05) << "\n";
        scheduleAt(simTime() + 0.05, processQueueTimer);
        return;
    }

    // =========================================================================
    // 2. RESUME VEHICLE (By Name Check)
    // =========================================================================
    // Fix: Check by name string because the pointer 'resumeMsg' is not valid for new messages
    if (strcmp(msg->getName(), "resumeVehicle") == 0) {
        std::cout << "[V2VProxy " << replicaId << "] Resume message received at t=" << simTime()
                  << " currentPhase=" << currentPhase << "\n";
        delete msg;

        // Only ignore if already moving or departed — a resumeVehicle message is only
        // ever scheduled by notifyVehicleCanGo (GO car) or the consensus timeout fallback.
        // WAIT cars never get a resumeVehicle scheduled, so any message here is legitimate.
        // We must NOT check currentPhase == ORDER_CONSENSUS because resetForNextRound() or
        // startReadyQCCollection() can change the phase between when the message is scheduled
        // and when it fires (same simulation timestep, later event ID).
        if (currentPhase == EXECUTING || currentPhase == DEPARTED) {
            std::cout << "[V2VProxy " << replicaId << "] IGNORING resume - already in phase " << currentPhase
                      << " (EXECUTING or DEPARTED)." << "\n";
            return;
        }
        flushReliabilityQueue();


        simtime_t resumeTime = simTime();
        std::cout << "[METRICS " << replicaId << "] Resume_Time: " << resumeTime << "\n";
        std::cout << "[METRICS " << replicaId << "] Stop_Time: " << stopTime << "\n";

        // Time to resume vehicle movement after consensus delay
        std::cout << "[V2VProxy " << replicaId << "] Resume delay expired, resuming vehicle now!" << "\n";

        if (mobility && mobility->getVehicleCommandInterface()) {
            currentPhase = EXECUTING;
            isWaitingForClearance = false;  // Prevent clearance watcher from interfering with GO car
            mobility->getVehicleCommandInterface()->setSpeedMode(0);
            mobility->getVehicleCommandInterface()->setSpeed(30);  // Release control to SUMO
            isStopped = false;
            waitingForConsensus = false;
            std::cout << "[V2VProxy " << replicaId << "] Vehicle RESUMED movement at t=" << simTime() << "\n";
        } else {
            std::cerr << "[V2VProxy " << replicaId << "] WARNING: mobility or command interface null, cannot resume" << "\n";
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
        //     std::cout << "[V2VProxy " << replicaId << "] Reached intersection line. Stopping." << "\n";
        //     stopVehicle();
        // }

        // ---------------------------------------------------------------------
        // RULE 2: THE CLEARANCE WATCHER (Waiting for the 4 cars to leave)
        // ---------------------------------------------------------------------
        if (isWaitingForClearance) {
            // A. Batch advance only after TraCI says each batch car is past the junction (departure
            //    edge C2*), not when they leave wireless range (radar falsely cleared early for rear cars).
            for (const std::string& carId : expectedToGo) {
                if (confirmedDeparted.find(carId) != confirmedDeparted.end())
                    continue;
                if (vehicleHasClearedIntersectionTraCI(carId)) {
                    confirmedDeparted.insert(carId);
                    std::cout << "[CLEARANCE] " << carId << " cleared intersection (TraCI) ("
                              << confirmedDeparted.size() << "/" << expectedToGo.size() << ")" << "\n";
                }
            }

            // B. Instantly hit the gas if the car ahead of me got the GO signal
            if (!myLaneTriggerCar.empty() && expectedToGo.find(myLaneTriggerCar) != expectedToGo.end()) {
                std::cout << "[CLEARANCE] " << myLaneTriggerCar << " got the GO signal! veh"
                          << replicaId << " tailing it to the stop line." << "\n";

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

            // C. Once the current batch's cars are GONE, advance to next batch (or reset if done).
            bool allDeparted = (!currentBatchExpected.empty() &&
                                confirmedDeparted.size() == currentBatchExpected.size());
            bool timedOut = (simTime() >= clearanceStartTime + CLEARANCE_TIMEOUT);

            if (allDeparted || timedOut) {
                if (timedOut && !allDeparted) {
                    std::cout << "[CLEARANCE] Timeout! Advancing batch anyway ("
                              << confirmedDeparted.size() << "/" << currentBatchExpected.size()
                              << " departed)\n";
                }

                int nextIdx = currentBatchIndex + 1;
                if (nextIdx < (int)pendingBatches.size()) {
                    // More batches remain — execute next batch
                    std::cout << "[CLEARANCE] Batch " << currentBatchIndex
                              << " cleared — advancing to batch " << nextIdx << "\n";
                    isWaitingForClearance = false;
                    laneDiscovered = false;
                    discoverLane();
                    executeBatch(nextIdx);
                } else {
                    // All batches done — trigger global reset for next epoch
                    std::cout << "[CLEARANCE] All batches cleared! Triggering next epoch.\n";
                    int remainingCars = getVisibleVehicles(300.0).size();
                    notifyJavaNewBatchSize(remainingCars);
                    laneDiscovered = false;
                    discoverLane();

                    std::vector<int> departedIds;
                    for (const auto& batch : pendingBatches)
                        for (const auto& carIdStr : batch)
                            departedIds.push_back(extractReplicaIdFromCarId(carIdStr));
                    triggerGlobalResetViaJNI(departedIds);

                    isWaitingForClearance = false;
                    currentPhase = IDLE;
                    joinTriggered = false;

                    if (isStopped) {
                        isStopped = false;
                        mobility->getVehicleCommandInterface()->setSpeedMode(31);
                        mobility->getVehicleCommandInterface()->setSpeed(-1);
                    }
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

            // Log arrival time for ALL cars entering the intersection zone (lane leader or queued).
            // This is used to compute true total intersection delay (arrival → resume).
            std::cout << "[METRICS " << replicaId << "] Arrival_Time: " << simTime() << "\n";

            if (carAhead.empty() || firstOrderBagProposalTime == 0) {
                std::cout << "[V2VProxy " << replicaId << "] ===== APPROACHING INTERSECTION =====" << "\n";
                stopVehicle();
                // NEW PROTOCOL: Broadcast ARRIVAL_ANNOUNCE (with full VehicleState) immediately.
                // VIEW_PROPOSAL is sent after all BATCH_SIZE announcements are collected.
                currentPhase = PROPOSING_VIEW;  // signals handleArrivalAnnouncement to trigger view proposal
                broadcastArrivalAnnouncement();
                viewSignatureCollectionStartTime = simTime(); 
                
                joinTriggered = true;
                stopTime = simTime();
                hasRequestedCrossing = true;
                waitingForConsensus = true;
                consensusStartTime = simTime();
                
                if (!consensusTimeoutTimer->isScheduled()) {
                     scheduleAt(simTime() + consensusTimeoutSec, consensusTimeoutTimer);
                }
                // Stop-sign timer: only for lane leaders (carAhead empty = physically first in queue)
                if (carAhead.empty() && !stopSignTimeoutTimer->isScheduled()) {
                    scheduleAt(simTime() + stopSignTimeoutSec, stopSignTimeoutTimer);
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

        // Retry a pending VIEW_PROPOSE if Java became ready between attempts
        if (!pendingViewProposalRequest.empty() && javaReady) {
            std::cout << "[V2VProxy " << replicaId << "] Retrying pending VIEW_PROPOSE from checkPosition\n";
            if (triggerJoinViaJNI(pendingViewProposalRequest)) {
                pendingViewProposalRequest.clear();
            }
        }

        // Keep the heartbeat alive!
        scheduleAt(simTime() + 0.05, checkPositionTimer);
        return;
    }
    // =========================================================================
    // 4. OTHER TIMERS
    // =========================================================================

    if (msg == readyQCTimeoutTimer) {
        // Retransmit ARRIVAL_ANNOUNCE if we haven't collected enough witnesses yet
        std::string myCarId = "veh" + std::to_string(replicaId);
        
        std::string laneForFrontCheck;
        auto annIt = pendingAnnouncements.find(myCarId);
        if (annIt != pendingAnnouncements.end() && !annIt->second.laneId.empty()) {
            laneForFrontCheck = annIt->second.laneId;
        } else if (mobility && mobility->getCommandInterface()) {
            try {
                laneForFrontCheck = mobility->getCommandInterface()->vehicle(myCarId).getLaneId();
            } catch (...) {
                laneForFrontCheck.clear();
            }
        }

        // bool isLineLeader = !laneForFrontCheck.empty() &&
        //                     isCarAtFrontOfLane(myCarId, laneForFrontCheck);
        if (!establishedView.empty() && establishedView.count(myCarId) &&
            currentPhase == COLLECTING_QC) {
            int f = ((int)establishedView.size() - 1) / 3;
            int required = f + 1;
            size_t have = collectedWitnesses[myCarId].size();
            if ((int)have < required) {
                std::cout << "[V2VProxy " << replicaId << "] ARRIVAL retransmit: only "
                          << have << "/" << required << " witnesses. Rebroadcasting..." << "\n";
                broadcastArrivalAnnouncement();
                // Reschedule for another attempt
                double retryStagger = replicaId * 0.025; // 25ms gap between cars
                double retryJitter = uniform(0.001, 0.010); // 1-10ms randomness
                scheduleAt(simTime() + 0.1 + retryStagger + retryJitter, readyQCTimeoutTimer);
                return;
            }
        }
        // QC already complete — nothing to do
        return;
    }

    if (msg == viewConsensusTimer) {
        if (currentPhase == VIEW_CONSENSUS) {
            std::cout << "[V2VProxy " << replicaId << "] View consensus timeout - still waiting for BFT response" << "\n";
        }
        return;
    }
    
    if (msg == retxCheckTimer) {
        syncTimeToJava();
        triggerRetransmissionCheckViaJNI();
        double jitter = uniform(0.001, 0.005);
        scheduleAt(simTime() + 0.001 + jitter, retxCheckTimer);
        return;
    }
    
    if (msg == checkJavaReadyTimer) {
        if (!javaReady) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            bool isReady = checkJavaReplicaStatus();
            if (isReady) {
                javaReady = true;
                std::cout << "[V2VProxy " << replicaId << "] *** JAVA READY at t="<< simTime() << " ***" << "\n";

                // Retry a VIEW_PROPOSE that failed earlier because Java wasn't ready yet
                if (!pendingViewProposalRequest.empty()) {
                    std::cout << "[V2VProxy " << replicaId << "] Retrying pending VIEW_PROPOSE\n";
                    if (triggerJoinViaJNI(pendingViewProposalRequest)) {
                        pendingViewProposalRequest.clear();
                    }
                    // If it still fails, it will be retried again next tick
                }

                // CRITICAL FIX: Only schedule if NOT ALREADY scheduled!
                if (!checkPositionTimer->isScheduled()) {
                    scheduleAt(simTime() + 0.1, checkPositionTimer);
                }
            } else {
                std::cout << "[V2VProxy " << replicaId << "] Waiting for Java (t="<< simTime() << ")..." << "\n";
                scheduleAt(simTime() + 0.1, checkJavaReadyTimer);
            }
        }
        return;
    }

    if (msg == startReadyQCCollectionMsg) {
        std::cout << "[V2VProxy " << replicaId << "] Starting Phase 2 at t=" << simTime() << "\n";
        double jitter = uniform(0.001, 0.005);
        scheduleAt(simTime() + jitter, startReadyQCCollectionMsg);
      //  startReadyQCCollection();
        // Do not delete member variables that might be reused, or set to null
        delete msg;
        startReadyQCCollectionMsg = nullptr; 
        return;
    }
    
    if (msg == triggerJoinTimer) {
        std::cout << "[DEBUG V2VProxy " << replicaId << "] triggerJoinTimer (deprecated in TPWC) at t=" << simTime() << "\n";
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
                  << consensusTimeoutSec << "s" << "\n";
        std::cout << "[METRICS " << replicaId << "] (consensus succeeded : FALSE)" << "\n";
        std::cout << "[V2VProxy " << replicaId << "] FALLBACK: Resuming without coordination" << "\n";
        resumeVehicle(0.0);
        waitingForConsensus = false;
        return;
    }

    if (msg == stopSignTimeoutTimer) {
        if (!waitingForConsensus) return;  // idempotency guard

        std::cout << "[V2VProxy " << replicaId << "] *** STOP-SIGN TIMEOUT *** "
                  << stopSignTimeoutSec << "s elapsed without BFT GO — releasing to SUMO" << "\n";

        // Cancel all active BFT timers
        if (readyQCTimeoutTimer && readyQCTimeoutTimer->isScheduled())           cancelEvent(readyQCTimeoutTimer);
        if (orderCollectDeadlineTimer && orderCollectDeadlineTimer->isScheduled()) cancelEvent(orderCollectDeadlineTimer);
        if (orderGossipRetransmitTimer && orderGossipRetransmitTimer->isScheduled()) cancelEvent(orderGossipRetransmitTimer);
        if (orderBagRetransmitTimer && orderBagRetransmitTimer->isScheduled())   cancelEvent(orderBagRetransmitTimer);
        if (orderDelayTimer && orderDelayTimer->isScheduled())                   cancelEvent(orderDelayTimer);
        if (consensusTimeoutTimer && consensusTimeoutTimer->isScheduled())       cancelEvent(consensusTimeoutTimer);

        // Log failure
        stopSignFailuresByEpoch[currentEpoch]++;
        std::cout << "[METRICS " << replicaId << "] StopSign_Timeout: 1" << "\n";
        std::cout << "[METRICS " << replicaId << "] StopSign_Timeout_SimTime: "
                  << simTime() << "\n";

        // Reset state flags
        isStopped = false;
        waitingForConsensus = false;
        isWaitingForClearance = false;

        // Release SUMO
        if (mobility && mobility->getVehicleCommandInterface()) {
            mobility->getVehicleCommandInterface()->setSpeedMode(0);
            mobility->getVehicleCommandInterface()->setSpeed(30);
        }

        // Tell Java this replica departed
        notifyJavaDeparted();
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
                int viewLeader = establishedView.empty() ? -1 : getCurrentViewLeader(establishedView);
                bool iAmViewLeader = (viewLeader == replicaId);
                if (!bag.empty() && iAmViewLeader) {
                    std::cout << "[ORDER-BAG] Replica " << replicaId
                              << " DEADLINE retransmit with closeFlag=true at t=" << simTime() << "\n";
                    triggerJoinViaJNI(serializeOrderBagRequest(bag, true));
                } else if (!iAmViewLeader) {
                    std::cout << "[ORDER-BAG] Replica " << replicaId
                              << " skipping DEADLINE retransmit (not view leader, leader=" << viewLeader << ")"
                              << "\n";
                }
            }
        }
        return;
    }

    if (msg == orderGossipRetransmitTimer) {
        std::string myCarId = "veh" + std::to_string(replicaId);
        if (myReadyQCComplete && !orderDecisionReceived && orderCollectionActive && isMyQCFrontMostKnownInLaneFromPool()) {
            // Count un-ACKed peers — if all have ACKed, no need to retransmit
            int unackedCount = 0;
            for (const auto& peerStr : establishedView) {
                if (peerStr == myCarId) continue;
                int peerId = extractReplicaIdFromCarId(peerStr);
                if (peerId != -1 && readyQCAcks.count(peerId) == 0) unackedCount++;
            }

            if (unackedCount == 0) {
                std::cout << "[ORDER-GOSSIP] Replica " << replicaId
                          << " all peers ACKed, stopping gossip retransmit at t=" << simTime() << "\n";
                return;
            }

            if (verifiedPool.count(myCarId)) {
                // Single broadcast — cheap, reaches all peers in one packet
                std::vector<uint8_t> payload = serializeReadyQC(verifiedPool[myCarId]);
                sendBFTMessage(replicaId, -1, payload, 3);
                std::cout << "[ORDER-GOSSIP] Replica " << replicaId
                          << " re-broadcast own ReadyQC (" << unackedCount
                          << " peers not yet ACKed) at t=" << simTime() << "\n";
            }

            // Reschedule while there are un-ACKed peers and before deadline. Scale interval
            // to remaining window so 2-3 retries fit before deadline (helps small-n / final epoch).
            double remaining = (orderCollectionDeadline - simTime()).dbl();
            double interval = std::min(0.35, remaining / 3.0);
            if (interval < 0.02) interval = 0.02;
            if (remaining > 0.20 && simTime() < orderCollectionDeadline - SimTime(0.20)) {
                scheduleAt(simTime() + interval, orderGossipRetransmitTimer);
            }
        }
        return;
    }

    if (msg == orderBagRetransmitTimer) {
        if (orderDecisionReceived) return;
        // Single-submitter mode: submit ORDER exactly once per round.
        // Do not retransmit ORDER_PROPOSE via JNI, or we reintroduce batching variance.
        int viewLeader = establishedView.empty() ? -1 : getCurrentViewLeader(establishedView);
        bool iAmViewLeader = (viewLeader == replicaId);
        if (iAmViewLeader) {
            std::cout << "[ORDER-BAG] Replica " << replicaId
                      << " retransmit timer fired but suppressed (single-submit mode)" << "\n";
        }
        return;
    }

    if (msg == orderDelayTimer) {
        if (!delayedOrderSubmitScheduled) return;
        delayedOrderSubmitScheduled = false;

        if (orderDecisionReceived || pendingOrderPayload.empty()) {
            pendingOrderPayload.clear();
            pendingOrderEpoch = -1;
            pendingOrderViewHash = 0;
            return;
        }

        // Epoch/view binding: only submit if still in same ORDER round context.
        std::string viewStr;
        for (const auto& car : establishedView) {
            if (!viewStr.empty()) viewStr += ",";
            viewStr += car;
        }
        int32_t currentViewHash = computeXXHash32(viewStr);
        if (pendingOrderEpoch != currentEpoch || pendingOrderViewHash != currentViewHash) {
            std::cout << "[ORDER-DELAY] Replica " << replicaId
                      << " dropping delayed ORDER submit due to epoch/view mismatch"
                      << " pendingEpoch=" << pendingOrderEpoch
                      << " currentEpoch=" << currentEpoch
                      << " pendingViewHash=" << pendingOrderViewHash
                      << " currentViewHash=" << currentViewHash
                      << "\n";
            pendingOrderPayload.clear();
            pendingOrderEpoch = -1;
            pendingOrderViewHash = 0;
            return;
        }

        std::cout << "[ORDER-DELAY] Replica " << replicaId
                  << " firing delayed ORDER submit at t=" << simTime()
                  << " gap=" << orderDelayGap << "s"
                  << " epoch=" << pendingOrderEpoch
                  << "\n";
        triggerJoinViaJNI(pendingOrderPayload);
        pendingOrderPayload.clear();
        pendingOrderEpoch = -1;
        pendingOrderViewHash = 0;
        return;
    }

    std::cout << "[HANDLE-SELF-MSG] Replica " << replicaId << ": msg=" << msg->getName() << "\n";
    
    if (isDeparted) {
        delete msg;
        return;
    }
    
    DemoBaseApplLayer::handleSelfMsg(msg);
}

void V2VProxyModule::handleLowerMsg(cMessage* msg)
{
    if (replicaId < 0) {return; }
    
    static int lowerMsgCount = 0;
    if (++lowerMsgCount % 50 == 1 || lowerMsgCount <= 20) {
        std::cout << "[HANDLE-LOWER-MSG] Replica " << replicaId << ": Message #" << lowerMsgCount 
                  << " at t=" << simTime() << " msgType=" << msg->getName() << "\n";
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
                  << " msgType=" << bftMsg->getMessageType() << "\n";
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
    std::cout << "[V2VProxy " << replicaId << "] triggerJoinViaJNI('" << request << "') called at t=" << simTime() << "\n";

    // Note: Removed joinTriggered flag check - in TPWC we need multiple consensus rounds
    // (VIEW, ORDER, then VIEW again for next batch, etc.)

    std::lock_guard<std::mutex> lock(jvmMutex);
    
    if (!sharedJVM) {
        std::cerr << "[ERROR V2VProxy " << replicaId << "] No JVM available for triggerJoin" << "\n";
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
        std::cerr << "[ERROR V2VProxy " << replicaId << "] Failed to find ServerRunner class" << "\n";
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
                std::cout << "[V2VProxy " << replicaId << "] Barrier: " << barrierStatus << "\n";
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
                      << "' (expected 'READY') at t=" << simTime() << "\n";
            return false;
        }
        std::cout << "[V2VProxy " << replicaId << "] PASSED: Server Status = 'READY'" << "\n";
    }

    jmethodID readyMethod = env->GetStaticMethodID(serverRunnerClass, "isReplicaReady", "(I)Z");
    if (readyMethod) {
        jboolean isReady = env->CallStaticBooleanMethod(serverRunnerClass, readyMethod, replicaId);
        
        if (!isReady) {
            std::cout << "[V2VProxy " << replicaId << "] FAILED: isReplicaReady() = false at t="
                      << simTime() << "\n";
            return false; // Return false so handleSelfMsg reschedules the timer
        }
        std::cout << "[V2VProxy " << replicaId << "] PASSED: isReplicaReady() = true" << "\n";
    } else {
        std::cerr << "[ERROR] Could not find isReplicaReady method!" << "\n";
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
                      << request << "' at t=" << simTime() << "\n";
            return true; // Success!
        } else {
            std::cerr << "[V2VProxy " << replicaId << "] Exception calling triggerJoinForReplica" << "\n";
            env->ExceptionDescribe();
            env->ExceptionClear();
        }
    } else {
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: Could not find triggerJoinForReplica(I, String) method" << "\n";
    }

    return false;

}

bool V2VProxyModule::triggerGlobalResetViaJNI(const std::vector<int>& departedReplicas)
{
    std::cout << "[V2VProxy " << replicaId << "] triggerGlobalResetViaJNI() called at t=" << simTime() << "\n";

    std::lock_guard<std::mutex> lock(jvmMutex);
    
    if (!sharedJVM) {
        std::cerr << "[ERROR V2VProxy " << replicaId << "] No JVM available for triggerGlobalReset" << "\n";
        return false;
    }
    
    JNIEnv* env;
    int envStat = sharedJVM->GetEnv((void**)&env, JNI_VERSION_1_8);
    if (envStat == JNI_EDETACHED) {
        sharedJVM->AttachCurrentThread((void**)&env, nullptr);
    }
    
    // Find ReliableV2VMessaging class
    jclass messagingClass = env->FindClass("bftsmart/communication/V2V/ReliableV2VMessaging");
    if (!messagingClass) {
        std::cerr << "[ERROR V2VProxy " << replicaId << "] Failed to find ReliableV2VMessaging class" << "\n";
        env->ExceptionDescribe();
        env->ExceptionClear();
        return false;
    }

    // Get the static globalResetV2V method taking int[]
    jmethodID resetMethod = env->GetStaticMethodID(messagingClass, "globalResetV2V", "([I)V");

    if (resetMethod) {
        jintArray jDepartedArray = nullptr;
        if (!departedReplicas.empty()) {
            jDepartedArray = env->NewIntArray(departedReplicas.size());
            // Need cast because jint is usually long on 64-bit windows, but usually int on linux.
            // On linux jint is int32_t. data() of vector<int> is safe for SetIntArrayRegion.
            env->SetIntArrayRegion(jDepartedArray, 0, departedReplicas.size(), (const jint*)departedReplicas.data());
        }

        // Call Java method
        env->CallStaticVoidMethod(messagingClass, resetMethod, jDepartedArray);

        if (jDepartedArray) {
            env->DeleteLocalRef(jDepartedArray);
        }

        if (!env->ExceptionCheck()) {
            std::cout << "[V2VProxy " << replicaId << "] SUCCESS: Triggered globalResetV2V via JNI at t=" << simTime() << "\n";
            return true; // Success!
        } else {
            std::cerr << "[V2VProxy " << replicaId << "] Exception calling globalResetV2V" << "\n";
            env->ExceptionDescribe();
            env->ExceptionClear();
        }
    } else {
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: Could not find globalResetV2V([I)V method" << "\n";
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
            std::cerr << "[V2VProxy] ERROR: Failed to find V2VNativeBridge class" << "\n";
            return false;
        }
        jmethodID warmupMethod = env->GetStaticMethodID(bridgeClass, "nativeWarmupPing", "()V");
        
        if (!warmupMethod) {
            std::cerr << "[V2VProxy] ERROR: Failed to find nativeWarmupPing method" << "\n";
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
        std::cerr << "[V2VProxy] ERROR: Failed to warm up JVM: " << e.what() << "\n";
        return false;
    }

}

bool V2VProxyModule::createOrAttachJVM()
{
    std::lock_guard<std::mutex> lock(jvmMutex);

    // If JVM already exists, attach this thread
    if (sharedJVM != nullptr) {
        EV_INFO << "Replica " << replicaId << ": Attaching to existing JVM" << "\n";

        JNIEnv* env;
        jint result = sharedJVM->AttachCurrentThread((void**)&env, nullptr);
        if (result != JNI_OK) {
            EV_ERROR << "Failed to attach to JVM: " << result << "\n";
            return false;
        }

        jvm = sharedJVM;

        // Each replica needs its own clockClass/updateTimeMethod so syncTimeToJava()
        // works on non-first replicas. Without this, only the JVM-creating replica
        // (replica 0) can update SimulationClock; after it departs the clock freezes,
        // which makes envelope.timestampMs == now forever and suppresses all reliability
        // retransmissions for subsequent epochs.
        jclass localClockCls = env->FindClass("bftsmart/communication/V2V/SimulationClock");
        if (localClockCls) {
            clockClass = (jclass) env->NewGlobalRef(localClockCls);
            updateTimeMethod = env->GetStaticMethodID(clockClass, "updateTime", "(D)V");
            env->DeleteLocalRef(localClockCls);
        } else {
            std::cerr << "[V2VProxy " << replicaId << "] WARNING: SimulationClock class not found on attach" << "\n";
            env->ExceptionClear();
        }

        return true;
    }

    // Create new JVM (first replica only)
    EV_INFO << "Replica " << replicaId << ": Creating new JVM" << "\n";

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
        EV_ERROR << "Failed to create JVM: " << result << "\n";
        return false;
    }

    jvm = sharedJVM;
    std::cout << "[V2VProxy] JVM created successfully" << "\n";
    
    // Manually register JNI native methods (required in embedded mode)
    std::cout << "[V2VProxy] Registering JNI native methods..." << "\n";
    if (!registerJNINativeMethods(env)) {
        std::cerr << "[V2VProxy] ERROR: Failed to register JNI native methods" << "\n";
        return false;
    }
    std::cout << "[V2VProxy] JNI native methods registered successfully" << "\n";

    std::cout << "[V2VProxy] Warming up JVM (crypto + JNI) before starting replicas..." << "\n";

    if (!warmupJVM(env)) {
        std::cerr << "[V2VProxy] ERROR: Failed to warm up JVM" << "\n";
        return false;
    }
    std::cout << "[V2VProxy] JVM warmed up successfully" << "\n";
    
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
    JNIEXPORT void JNICALL Java_bftsmart_demo_intersection_IntersectionServer_notifyWipeComplete
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
        std::cerr << "[V2VProxy] ERROR: Failed to find V2VNativeBridge class" << "\n";
        env->ExceptionDescribe();
        env->ExceptionClear();
        return false;
    }

    jclass localClockCls = env->FindClass("bftsmart/communication/V2V/SimulationClock");
    if (!localClockCls) {
        std::cerr << "[V2VProxy] ERROR: Failed to find SimulationClock class" << "\n";
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
        std::cerr << "[V2VProxy] ERROR: Failed to register native methods" << "\n";
        env->ExceptionDescribe();
        env->ExceptionClear();
        return false;
    }

    std::cout << "[V2VProxy] Successfully registered 4 JNI native methods" << "\n";
    // return true;

    jclass intersectionServerClass = env->FindClass("bftsmart/demo/intersection/IntersectionServer");
    if (!intersectionServerClass) {
        std::cerr << "[V2VProxy] ERROR: Failed to find IntersectionServer class" << "\n";
        env->ExceptionDescribe();
        env->ExceptionClear();
        return false;
    }

    JNINativeMethod serverMethods[] = {
        {const_cast<char*>("notifyVehicleCanGo"),    const_cast<char*>("(ID)V"),                (void*)&Java_bftsmart_demo_intersection_IntersectionServer_notifyVehicleCanGo},
        {const_cast<char*>("notifyViewAgreed"),      const_cast<char*>("(ILjava/lang/String;)V"),(void*)&Java_bftsmart_demo_intersection_IntersectionServer_notifyViewAgreed},
        {const_cast<char*>("notifyOrderDecided"),    const_cast<char*>("(ILjava/lang/String;)V"),(void*)&Java_bftsmart_demo_intersection_IntersectionServer_notifyOrderDecided},
        {const_cast<char*>("notifyWipeComplete"),    const_cast<char*>("(I)V"),                 (void*)&Java_bftsmart_demo_intersection_IntersectionServer_notifyWipeComplete}
    };

    if (env->RegisterNatives(intersectionServerClass, serverMethods, 4) != 0) {
        std::cerr << "[V2VProxy] ERROR: Failed to register IntersectionServer native methods" << "\n";
        env->ExceptionDescribe();
        env->ExceptionClear();
        return false;
    }

    std::cout << "[V2VProxy] Successfully registered 4 IntersectionServer JNI native methods" << "\n";
    return true;


}

void V2VProxyModule::startBFTSmartReplica()
{
    if (!jvm) {
        EV_ERROR << "Cannot start BFTSmart replica: JVM not initialized" << "\n";
        return;
    }

    JNIEnv* env;
    jvm->AttachCurrentThread((void**)&env, nullptr);

    std::cout << "[V2VProxyModule] Starting BFTSmart replica " << replicaId << " in background Java thread" << "\n";

    // Find ServerRunner class (wrapper that runs IntersectionServer in a thread)
    jclass runnerClass = env->FindClass("bftsmart/demo/intersection/ServerRunner");
    if (!runnerClass) {
        std::cerr << "[V2VProxyModule] ERROR: Failed to find ServerRunner class" << "\n";
        env->ExceptionDescribe();
        return;
    }

    // Get ServerRunner constructor: ServerRunner(int replicaId, int numCars)
    jmethodID runnerCtor = env->GetMethodID(runnerClass, "<init>", "(II)V");
    if (!runnerCtor) {
        std::cerr << "[V2VProxyModule] ERROR: Failed to find ServerRunner constructor" << "\n";
        env->ExceptionDescribe();
        return;
    }

    // Create ServerRunner instance (this is fast, doesn't block)
    jobject runnerInstance = env->NewObject(runnerClass, runnerCtor, replicaId, BATCH_SIZE);
    if (!runnerInstance) {
        std::cerr << "[V2VProxyModule] ERROR: Failed to create ServerRunner instance" << "\n";
        env->ExceptionDescribe();
        return;
    }

    // Find Thread class
    jclass threadClass = env->FindClass("java/lang/Thread");
    if (!threadClass) {
        std::cerr << "[V2VProxyModule] ERROR: Failed to find Thread class" << "\n";
        env->ExceptionDescribe();
        return;
    }

    // Get Thread constructor: Thread(Runnable target)
    jmethodID threadCtor = env->GetMethodID(threadClass, "<init>", "(Ljava/lang/Runnable;)V");
    if (!threadCtor) {
        std::cerr << "[V2VProxyModule] ERROR: Failed to find Thread constructor" << "\n";
        env->ExceptionDescribe();
        return;
    }

    // Create Thread with ServerRunner as the Runnable
    jobject thread = env->NewObject(threadClass, threadCtor, runnerInstance);
    if (!thread) {
        std::cerr << "[V2VProxyModule] ERROR: Failed to create Thread" << "\n";
        env->ExceptionDescribe();
        return;
    }

    // Get Thread.start() method
    jmethodID startMethod = env->GetMethodID(threadClass, "start", "()V");
    if (!startMethod) {
        std::cerr << "[V2VProxyModule] ERROR: Failed to find Thread.start method" << "\n";
        env->ExceptionDescribe();
        return;
    }

    // Start the thread (THIS RETURNS IMMEDIATELY - non-blocking!)
    std::cout << "[V2VProxyModule] Starting Java thread for replica " << replicaId << "\n";
    env->CallVoidMethod(thread, startMethod);

    // Keep a global reference to the thread
    bftReplicaThread = env->NewGlobalRef(thread);

    std::cout << "[V2VProxyModule] BFTSmart replica " << replicaId << " thread started (non-blocking)" << "\n";

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

        EV_INFO << "BFTSmart replica " << replicaId << " stopped" << "\n";
    }
}

// ============================================================================
// INTERSECTION MANAGEMENT
// ============================================================================

void V2VProxyModule::handlePositionUpdate(cObject* obj)
{
    if (replicaId < 0) {  return; }
    DemoBaseApplLayer::handlePositionUpdate(obj);
    std::cout << "[POSITION UPDATE] Replica " << replicaId << " at time " << simTime() << "\n";
    
    std::cout << "[POSITION UPDATE] Replica " << replicaId << " current phase: " << currentPhase << "Has departed: " << isDeparted << "\n";

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

bool V2VProxyModule::vehicleHasClearedIntersectionTraCI(const std::string& carId)
{
    if (!mobility || !mobility->getCommandInterface()) return false;
    TraCICommandInterface* traci = mobility->getCommandInterface();
    try {
        TraCICommandInterface::Vehicle v = traci->vehicle(carId);
        std::string laneId = v.getLaneId();
        if (laneId.empty()) return false;
        // SUMO internal / junction lanes — still inside the conflict region
        if (laneId.front() == ':') return false;

        std::string roadId = v.getRoadId();
        // Four-way nets here use outgoing edges C2N, C2S, C2E, C2W from the center node.
        bool onDepartureLeg = (roadId.size() >= 2
                               && std::toupper(static_cast<unsigned char>(roadId[0])) == 'C'
                               && std::toupper(static_cast<unsigned char>(roadId[1])) == '2');
        if (onDepartureLeg) {
            constexpr double kMinMetersOnDeparture = 3.0;
            return v.getLanePosition() >= kMinMetersOnDeparture;
        }
        return false;
    } catch (...) {
        // Vehicle left the simulation
        return true;
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
        mobility->getVehicleCommandInterface()->setSpeedMode(31);
        mobility->getVehicleCommandInterface()->setSpeed(-1);
       
        isStopped = true;
        discoverLane();
        std::cout << "[V2VProxy " << replicaId << "] Vehicle STOPPED at intersection (distance=" << getDistanceToIntersection() << "m)" << "\n";
    } else {
        std::cout << "[V2VProxy " << replicaId << "] Vehicle already stopped" << "\n";
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
    // Dedup: two paths can call resumeVehicle for the same round
    // (notifyOrderDecided JNI callback AND sendConsensusRequest reply).
    // Only the first call per round is accepted; reset in resetForNextRound().
    if (!pendingResumeDelays.empty()) {
        std::cout << "[RESUME] Replica " << replicaId << ": Duplicate GO signal (delay=" << delaySeconds
                  << "s) ignored — already queued " << pendingResumeDelays.size() << " entry(s)" << "\n";
        return;
    }
    pendingResumeDelays.push(delaySeconds);
    std::cout << "[RESUME] Replica " << replicaId << ": JNI received GO signal with delay=" << delaySeconds 
              << "s. Queued for main thread (queue size=" << pendingResumeDelays.size() << ")" << "\n";
    std::cout << "[RESUME] Replica " << replicaId << ": WARNING - This will trigger shouldFlush on next self-message!" << "\n";
    // any OMNeT++ operations on the simulation thread via a self-message
 
}

void V2VProxyModule::resetForNextRound() {
    std::cout << "[V2VProxy " << replicaId << "] resetForNextRound triggered" << "\n";
    const simtime_t resetNow = simTime();
    
    // Reset basic flags for a new round
    joinTriggered = false;
    orderBagProposed = false;
    orderCollectionActive = false;
    myReadyQCComplete = false;
    orderDecisionReceived = false;
    currentEpoch++;
    lastRoundResetTime = resetNow;
    lastRoundResetEpoch = currentEpoch;
    establishedView.clear();
    viewState.clear();               // NEW: clear VehicleState map for next epoch
    arrivalAnnouncementsReceived.clear();
    pendingBatches.clear();
    currentBatchIndex = 0;
    currentBatchExpected.clear();
    verifiedPool.clear();
    // Drain any QCs that arrived while we were still on the old epoch
    for (auto& kv : nextEpochPool) {
        if (kv.second.epoch == currentEpoch) {
            verifiedPool[kv.first] = kv.second;
            std::cout << "[READYQC] Replica " << replicaId
                      << " promoted buffered QC from " << kv.first
                      << " epoch=" << kv.second.epoch << "\n";
        }
    }
    nextEpochPool.clear();

    // The cars waiting for clearing need to transition here securely on OMNeT++ thread.
    // This includes WAIT cars that were in ORDER_CONSENSUS AND background cars (2nd/3rd
    // in queue) that were in IDLE because they never reached the stop line to propose.
    // Also handles DEPARTED: resetForNextRound is ONLY called for cars that haven't
    // physically crossed yet, so DEPARTED here means a false-positive departure flag
    // (e.g. from a stale checkIfDeparted call) that must be cleared.
    isDeparted = false;
    if (currentPhase != EXECUTING) {
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
    pendingViewProposalRequest.clear();
    hasRequestedCrossing = false;
    logged100m = false;
    waitingForConsensus = false;

    // Reset ORDER bag collection state
    orderBagCloseFlag = false;
    orderBagRetransmitCount = 0;
    alreadyAtStopLine = false;



    //tracking per round not overall
    receivedMessages = 0;
    sentMessages = 0;

   



    // Cancel witness retransmit timer
    if (readyQCTimeoutTimer && readyQCTimeoutTimer->isScheduled()) {
        cancelEvent(readyQCTimeoutTimer);
    }

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
    if (orderDelayTimer && orderDelayTimer->isScheduled()) {
        cancelEvent(orderDelayTimer);
    }
    delayedOrderSubmitScheduled = false;
    pendingOrderPayload.clear();
    pendingOrderEpoch = -1;
    pendingOrderViewHash = 0;

    // verifiedPool and establishedView already cleared above (after epoch increment + drain)
    collectedWitnesses.clear();
    arrivalAnnouncementsReceived.clear();

    // Clear stale clearance-watch sets from the previous epoch.
    // Without this, checkPositionTimer can fire at the same sim-tick as the reset
    // and see confirmedDeparted == expectedToGo (both filled from the prior epoch),
    // concluding "all departed" instantly and skipping the real clearance wait.
    expectedToGo.clear();
    confirmedDeparted.clear();

    viewVotes.clear();
    shouldFlush = false;

    // Reset per-round timing metrics so next round starts with clean slate
    orderSignatureCollectionStartTime = 0;
    orderSignatureCollectionEndTime = 0;
    orderCollectionWindowStart = 0;
    orderCollectionWindowEnd = 0;
    firstOrderBagProposalTime = 0;
    firstOrderBagProposerReplica = -1;
    viewConsensusStartTime = 0;
    viewConsensusEndTime = 0;
    orderConsensusStartTime = 0;
    orderConsensusEndTime = 0;
    orderDecisionCallbackSeen = false;
    lastOrderBftRequestRttMs = -1.0;
    consensusStartTime = 0;

    flushReliabilityQueue();

    if (retxCheckTimer && !retxCheckTimer->isScheduled()) {
        scheduleAt(simTime() + 0.02, retxCheckTimer);
        std::cout << "[RESET] Replica " << replicaId << ": Rescheduled retxCheckTimer for reliability queue checks" << "\n";
    }

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
        std::cout << "[RESET] Replica " << replicaId << ": Rescheduled checkPositionTimer for clearance/departure checks" << "\n";
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

void V2VProxyModule::attachAmbulanceCryptoToAnnouncement(ArrivalAnnouncement& ann)
{
    ann.isAmbulance = false;
    ann.ambulanceCertBytes.clear();
    ann.ambulanceSigBytes.clear();
    if (!moduleIsAmbulance)
        return;

    ann.isAmbulance = true;
    ann.ambulanceCertBytes = myAmbulanceCertBytes;
    if (!ambulancePrivateKey || myAmbulanceCertBytes.size() != sizeof(VehicleCert)) {
        std::cerr << "[AMBULANCE] Replica " << replicaId << " attach failed: key="
                  << (ambulancePrivateKey ? "ok" : "null") << " certBytes="
                  << myAmbulanceCertBytes.size() << " expected " << sizeof(VehicleCert) << "\n";
        return;
    }

    const std::string ambPayload = ann.carId + ":" + ann.lane + ":"
                                 + std::to_string(ann.positionInLane) + ":"
                                 + dirToStr(ann.direction) + ":AMBULANCE";
    uint8_t sigOut[CRYPTO_SIG_MAX_BYTES];
    uint8_t sigLen = 0;
    EVP_PKEY* pk = static_cast<EVP_PKEY*>(ambulancePrivateKey);
    if (!CryptoAuth::instance().signBytes(pk,
            reinterpret_cast<const uint8_t*>(ambPayload.c_str()), ambPayload.size(),
            sigOut, sigLen)) {
        std::cerr << "[AMBULANCE] Replica " << replicaId << " signBytes failed for payload=" << ambPayload
                  << "\n";
        return;
    }
    ann.ambulanceSigBytes.assign(sigOut, sigOut + sigLen);
}


std::vector<uint8_t> V2VProxyModule::signWitnessClaim(const ArrivalAnnouncement& ann, double witnessTime, int witnessId) {
    // Debug logging (first 20 signatures) - BEFORE formatting
    static int callCount = 0;
    if (++callCount <= 20) {
        std::cout << "[SIGN_RAW] Witness " << witnessId << " RAW values: pos=" << std::setprecision(17) << ann.positionInLane 
                  << ", arrival=" << ann.claimedArrivalTime << ", witnessTime=" << witnessTime << "\n";
    }
    
    // CRITICAL: Format doubles with EXACTLY 6 decimal places for consistency!
    char posBuf[32], arrivalBuf[32], timestampBuf[32];
    std::snprintf(posBuf, sizeof(posBuf), "%d", ann.positionInLane);
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
        std::cout << "[SIGN_C++] Witness " << witnessId << " signing: \"" << data << "\"" << "\n";
        std::cout << "[SIGN_C++] XXHash32 result: " << hash << "\n";
        std::cout << "[SIGN_C++] Bytes (little-endian): [";
        uint8_t* hashBytes = reinterpret_cast<uint8_t*>(&hash);
        for (int i = 0; i < 4; i++) {
            if (i > 0) std::cout << ", ";
            std::cout << (int)hashBytes[i];
        }
        std::cout << "]" << "\n";
    }

    std::vector<uint8_t> sig(sizeof(int32_t));
    std::memcpy(sig.data(), &hash, sizeof(int32_t));
    return sig;
}


std::vector<uint8_t> V2VProxyModule::generateByzantinePayload(
    ByzantineType type, const ArrivalAnnouncement& honest, int targetReplicaId)
{
    switch (type) {

        case BYZANTINE_FALSE_LANE: {
            // Claim a completely wrong lane so verifyCarPosition() rejects this car
            // and no honest node will witness it — it will be excluded from ORDER.
            ArrivalAnnouncement corrupted = honest;
            corrupted.laneId = "BYZANTINE_FAKE_LANE";
            corrupted.signature = signArrivalClaim(corrupted); // internally consistent but wrong
            std::cout << "[BYZANTINE] Replica " << replicaId
                      << " FALSE_LANE: claiming laneId=" << corrupted.laneId
                      << " (real=" << honest.laneId << ")" << "\n";
            return serializeArrivalAnnouncement(corrupted);
        }

        case BYZANTINE_INVALID_SIG: {
            // Real position data but a garbage 4-byte signature.
            // The Java QC assembler will reject this car's self-signed claim.
            ArrivalAnnouncement corrupted = honest;
            corrupted.signature = std::vector<uint8_t>(4, 0xDE); // 0xDE 0xDE 0xDE 0xDE
            std::cout << "[BYZANTINE] Replica " << replicaId
                      << " INVALID_SIG: broadcasting corrupt signature" << "\n";
            return serializeArrivalAnnouncement(corrupted);
        }

        case BYZANTINE_EQUIVOCATOR: {
            // Send epoch N+1 to even replica IDs, honest epoch N to odd replica IDs.
            // From different receivers' perspectives this car is "in different rounds",
            // which violates BFT-SMaRt's agreement property.
            ArrivalAnnouncement corrupted = honest;
            if (targetReplicaId >= 0 && targetReplicaId % 2 == 0) {
                corrupted.epoch = honest.epoch + 1;
                std::cout << "[BYZANTINE] Replica " << replicaId
                          << " EQUIVOCATOR: sending epoch=" << corrupted.epoch
                          << " to peer " << targetReplicaId
                          << " (honest epoch=" << honest.epoch << ")" << "\n";
            }
            corrupted.signature = signArrivalClaim(corrupted);
            return serializeArrivalAnnouncement(corrupted);
        }

        default: // BYZANTINE_HONEST — should not reach here
            return serializeArrivalAnnouncement(honest);
    }
}


void V2VProxyModule::broadcastArrivalAnnouncement() {
   
    // ZOMBIE FILTER: Departed cars don't broadcast arrival announcements
    if (zombieFilter()) return;
    std::string myCarId = "veh" + std::to_string(replicaId);
    ArrivalAnnouncement announcement;
    announcement.carId = myCarId;
    // Get lane info from TraCI Vehicle interface
    if (!mobility) {
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: No mobility for broadcastArrivalAnnouncement" << "\n";
        return;
    }
    TraCICommandInterface* traci = mobility->getCommandInterface();
    if (!traci) {
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: No TraCI interface" << "\n";
        return;
    }
    TraCICommandInterface::Vehicle myVeh = traci->vehicle(myCarId);
    announcement.laneId = myVeh.getLaneId();
    // Derive cardinal lane from TraCI lane ID (first character: 'n','s','e','w' prefix)
    // Convention: lane IDs start with the approach direction letter in upper-case
    {
        std::string lid = announcement.laneId;
        if (!lid.empty()) {
            char c = std::toupper(lid[0]);
            announcement.lane = (c == 'N' || c == 'S' || c == 'E' || c == 'W')
                                 ? std::string(1, c) : "N"; // default fallback
        } else { announcement.lane = "N"; }
    }
    // positionInLane: integer rank (1 = front). Derive from TraCI position and lane queue.
    // For now, use lane queue rank if available; fallback to 1.
    {
        int rank = 1;
        if (!laneQueue.empty()) {
            auto it = std::find(laneQueue.begin(), laneQueue.end(), myCarId);
            if (it != laneQueue.end()) rank = (int)(it - laneQueue.begin()) + 1;
        }
        announcement.positionInLane = rank;
    }
    // Direction: read from omnetpp.ini parameter "intendedDirection" (S/L/R), default S
    {
        std::string dirParam = par("intendedDirection").stdstringValue();
        announcement.direction = strToDir(dirParam);
    }
    attachAmbulanceCryptoToAnnouncement(announcement);
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

    // Self-store in viewState so handleArrivalAnnouncement's threshold check includes us
    {
        VehicleState selfVS;
        selfVS.vehicleId    = canonicalAnn.carId;
        selfVS.lane         = canonicalAnn.lane;
        selfVS.positionInLane = canonicalAnn.positionInLane;
        selfVS.direction    = canonicalAnn.direction;
        selfVS.isAmbulance  = canonicalAnn.isAmbulance;
        viewState[myCarId]  = selfVS;
        arrivalAnnouncementsReceived.insert(myCarId);
    }

    if (isByzantine && byzantineType != BYZANTINE_HONEST) {
        if (byzantineType == BYZANTINE_EQUIVOCATOR) {
            // Unicast different payloads to each peer so they see different epochs.
            // We iterate the established view; fall back to replicaProxyMap if not yet set.
            std::set<std::string> targets = establishedView.empty()
                ? std::set<std::string>() : establishedView;
            if (targets.empty()) {
                std::lock_guard<std::mutex> lock(registryMutex);
                for (const auto& kv : replicaProxyMap) {
                    if (kv.first != replicaId)
                        targets.insert("veh" + std::to_string(kv.first));
                }
            }
            for (const auto& peerStr : targets) {
                int peerId = extractReplicaIdFromCarId(peerStr);
                if (peerId < 0 || peerId == replicaId) continue;
                std::vector<uint8_t> byzantinePayload =
                    generateByzantinePayload(byzantineType, canonicalAnn, peerId);
                sendBFTMessage(replicaId, peerId, byzantinePayload, 1);
            }
            std::cout << "[BYZANTINE] Replica " << replicaId
                      << " EQUIVOCATOR: sent " << targets.size() << " divergent unicasts\n";
        } else {
            // FALSE_LANE or INVALID_SIG: single broadcast with corrupted payload
            std::vector<uint8_t> byzantinePayload =
                generateByzantinePayload(byzantineType, canonicalAnn, -1);
            sendBFTMessage(replicaId, -1, byzantinePayload, 1);
        }
    } else {
        sendBFTMessage(replicaId, -1, payload, 1);  // Broadcast, type=1
    }
    std::cout << "[ANN-BROADCAST] Replica " << replicaId << " (" << myCarId << ") broadcast arrival announcement at t=" << simTime() << "\n";
}


void V2VProxyModule::handleArrivalAnnouncement(BFTMessage* bftMsg) {
    ArrivalAnnouncement ann = deserializeArrivalAnnouncement(bftMsg);

    // Stage 11: if intersection is locked (EXECUTING in progress), buffer new arrivals
    // that are not already part of the pendingBatches schedule.
    if (intersectionLocked) {
        bool inSchedule = false;
        for (const auto& batch : pendingBatches) {
            for (const auto& carId : batch) {
                if (carId == ann.carId) { inSchedule = true; break; }
            }
            if (inSchedule) break;
        }
        if (!inSchedule) {
            // New car arrived while intersection is locked
            if (std::find(bufferedNewArrivals.begin(), bufferedNewArrivals.end(), ann.carId)
                    == bufferedNewArrivals.end()) {
                bufferedNewArrivals.push_back(ann.carId);
                std::cout << "[ANN-RECV] Replica " << replicaId
                          << " BUFFERED new arrival " << ann.carId
                          << " (intersectionLocked, pending preemption)\n";
            }
            checkPreemptionConditions();
            return;
        }
    }

    // Dedup: skip if we already have a VehicleState for this car
    if (viewState.count(ann.carId)) {
        std::cout << "[ANN-RECV] Replica " << replicaId << " DEDUP: already have VehicleState for " << ann.carId << "\n";
        return;
    }

    // In the new protocol positionInLane is a rank (1=front), not a lane-position in metres.
    // verifyCarPosition checks lane membership + numeric distance; pass tolerance=1e9 to
    // effectively skip the distance check and only verify that the car is in the claimed lane.
    VerificationResult result = verifyCarPosition(ann.carId, ann.laneId, ann.positionInLane, 1e9);
    if (!result.isValid) {
        std::cout << "[ANN-RECV] Replica " << replicaId << " INVALID announcement from " << ann.carId << ": " << result.reason << "\n";
        return;
    }

    // ---- Ambulance certificate verification ----
    bool effectiveIsAmbulance = ann.isAmbulance;
    if (ann.isAmbulance && ann.ambulanceCertBytes.size() == sizeof(VehicleCert)) {
        VehicleCert cert;
        std::memcpy(&cert, ann.ambulanceCertBytes.data(), sizeof(VehicleCert));
        std::string role = CryptoAuth::instance().verifyCert(cert);
        if (role != "ambulance") {
            std::cerr << "[ANN-RECV] Replica " << replicaId
                      << " DOWNGRADE: cert role='" << role << "' for " << ann.carId << "\n";
            effectiveIsAmbulance = false;
        } else if (!ann.ambulanceSigBytes.empty() &&
                   ann.ambulanceSigBytes.size() <= CRYPTO_SIG_MAX_BYTES) {
            // Verify self-signature over "vehicleId:lane:posInLane:direction:AMBULANCE"
            std::string payload = ann.carId + ":" + ann.lane + ":"
                                + std::to_string(ann.positionInLane) + ":"
                                + dirToStr(ann.direction) + ":AMBULANCE";
            bool sigOk = CryptoAuth::instance().verifyBytes(
                cert.publicKey,
                reinterpret_cast<const uint8_t*>(payload.c_str()), payload.size(),
                ann.ambulanceSigBytes.data(),
                static_cast<uint8_t>(ann.ambulanceSigBytes.size()));
            if (!sigOk) {
                std::cerr << "[ANN-RECV] Replica " << replicaId
                          << " DOWNGRADE: ambulance sig invalid for " << ann.carId << "\n";
                effectiveIsAmbulance = false;
            }
        } else {
            effectiveIsAmbulance = false; // cert present but no sig to verify
        }
    } else if (ann.isAmbulance && !ann.ambulanceCertBytes.empty()) {
        // Cert was provided but wrong size → downgrade (malformed cert)
        std::cerr << "[ANN-RECV] Replica " << replicaId
                  << " DOWNGRADE: ambulance cert wrong size (" << ann.ambulanceCertBytes.size()
                  << " vs " << sizeof(VehicleCert) << ") for " << ann.carId << "\n";
        effectiveIsAmbulance = false;
    }
    // If ambulanceCertBytes is empty, trust ann.isAmbulance as-is (no-cert testing)

    // ---- Build and store VehicleState ----
    VehicleState vs;
    vs.vehicleId      = ann.carId;
    vs.lane           = ann.lane;
    vs.positionInLane = ann.positionInLane;
    vs.direction      = ann.direction;
    vs.isAmbulance    = effectiveIsAmbulance;

    viewState[ann.carId] = vs;
    arrivalAnnouncementsReceived.insert(ann.carId);

    size_t n        = viewState.size();
    size_t expected = BATCH_SIZE;
    std::cout << "[ANN-RECV] Replica " << replicaId << " stored VehicleState for " << ann.carId
              << " (have " << n << "/" << expected << " VehicleStates)\n";

    // ---- When all VehicleStates collected: build and broadcast VIEW_PROPOSAL ----
    if (n == expected && currentPhase == PROPOSING_VIEW && !viewEstablished) {
        std::cout << "[ANN-RECV] Replica " << replicaId
                  << " has all " << expected << " VehicleStates — initiating view proposal\n";
        initiateViewProposal();
    }
}


void V2VProxyModule::handleWitnessResponse(BFTMessage* bftMsg) {
    WitnessResponse witness = deserializeWitnessResponse(bftMsg);
    std::string myCarId = "veh" + std::to_string(replicaId);

    if (witness.targetCarId != myCarId) {
        std::cout << "[WITNESS-RECV] Replica " << replicaId << " IGNORING witness (meant for " << witness.targetCarId << ", not " << myCarId << ") from replica " << witness.witnessReplicaId << "\n";
        return;
    }

    // Guard: view must be established before we can compute f
    if (establishedView.empty()) {
        std::cout << "[WITNESS-RECV] Replica " << replicaId << " view not yet established, dropping witness from replica " << witness.witnessReplicaId << "\n";
        return;
    }

    // Dedup: ignore retransmitted witnesses from the same replica
    for (const auto& w : collectedWitnesses[myCarId]) {
        if (w.witnessReplicaId == witness.witnessReplicaId) {
            std::cout << "[WITNESS-RECV] Replica " << replicaId << " DEDUP: ignoring duplicate witness from replica " << witness.witnessReplicaId << "\n";
            return;
        }
    }

    collectedWitnesses[myCarId].push_back(witness);
    size_t total = collectedWitnesses[myCarId].size();
    int localviewBatchSize = (int)establishedView.size();
    int f = (localviewBatchSize - 1) / 3;
    int required = f + 1;
    std::cout << "localviewBatchSize: " << localviewBatchSize << "\n";
    std::cout << "f: " << f << "\n";
    std::cout << "required: " << required << "\n";
    std::cout << "[WITNESS-RECV] Replica " << replicaId << " ACCEPTED witness from replica " << witness.witnessReplicaId
              << " for " << myCarId << " (have " << total << "/" << required << " witnesses) at t=" << simTime() << "\n";


    // Only trigger ONCE when we first reach f+1 witnesses
    if (collectedWitnesses[myCarId].size() == required) {
        std::cout << "[V2VProxy " << replicaId << "] *** ReadyQC COMPLETE! *** "
                  << "(" << required << "/" << required << " witnesses)" << "\n";

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
   if (zombieFilter()) return;
    std::string myCarId = "veh" + std::to_string(replicaId);
    
    // Check if pendingAnnouncements exists - if not, populate from current TraCI state
    if (pendingAnnouncements.find(myCarId) == pendingAnnouncements.end() || 
        pendingAnnouncements[myCarId].laneId.empty()) {
        std::cout << "[ASSEMBLE_QC] Replica " << replicaId << " WARNING: pendingAnnouncements[" << myCarId 
                  << "] missing or empty! Populating from current TraCI state..." << "\n";
        
        if (!mobility || !mobility->getCommandInterface()) {
            std::cerr << "[ASSEMBLE_QC] ERROR: Cannot populate - mobility or TraCI unavailable!" << "\n";
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
                  << ", pos=" << pendingAnnouncements[myCarId].positionInLane << "\n";
    }
    
    ReadyQC qc;
    qc.carId = myCarId;
    qc.laneId = pendingAnnouncements[myCarId].laneId;
    qc.positionInLane = pendingAnnouncements[myCarId].positionInLane;
    qc.verifiedArrival = pendingAnnouncements[myCarId].claimedArrivalTime;
    qc.epoch = pendingAnnouncements[myCarId].epoch;

    std::cout << "[ASSEMBLE_QC] Replica " << replicaId << " assembling ReadyQC for " << myCarId << "\n";
    std::cout << "[ASSEMBLE_QC] QC data: carId=" << qc.carId << ", lane=" << qc.laneId 
              << ", pos=" << qc.positionInLane << ", arrival=" << qc.verifiedArrival << "\n";

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
        std::cout << "]" << "\n";
        
        // VERIFY IMMEDIATELY on C++ side!
        // Debug RAW values BEFORE formatting
        std::cout << "[VERIFY_RAW] Witness " << sig.witnessReplicaId << " RAW values: qc.pos=" << std::setprecision(17) << qc.positionInLane 
                  << ", qc.arrival=" << qc.verifiedArrival << ", sig.witnessTime=" << sig.witnessTimestamp << "\n";
        
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
            std::cerr << "[VERIFY_C++] ERROR: Signature too small! size=" << sig.signature.size()  << ", expected at least " << sizeof(int32_t) << " bytes" << "\n";
            continue;
        }
        std::cout << "Memcpying actual hash" << "\n";
        
        std::memcpy(&actualHash, sig.signature.data(), sizeof(int32_t));
        
        std::cout << "[VERIFY_C++] Data: \"" << reconstructedData << "\"" << "\n";
        std::cout << "[VERIFY_C++] Expected: " << expectedHash << ", Actual: " << actualHash 
                  << ", Match: " << (expectedHash == actualHash ? "true" : "FALSE!!!") << "\n";
        
        qc.signatures.push_back(sig);
    }

    verifiedPool[myCarId] = qc;

    // Broadcast ReadyQC on V2V so other cars can store it locally
    std::vector<uint8_t> payload = serializeReadyQC(qc);
    sendBFTMessage(replicaId, -1, payload, 3);
    std::cout << "[V2VProxy " << replicaId << "] *** BROADCASTED ReadyQC for " << myCarId << " ***" << "\n";

    myReadyQCComplete = true;

    if (isCarAtFrontOfLane(myCarId, qc.laneId)) {
        std::cout << "[BUILD_ORDER_BAG_FINAL] car " << myCarId << " is at front of lane " << qc.laneId << ", starting order collection window" << "\n";
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
        // View guard: only count QCs from established view members
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
    std::cout << "[COUNT_DISTINCT_FRONT_LANES] Best positions by lane: " << bestPosByLaneStr << "\n";
    return (int)bestPosByLane.size();
}

void V2VProxyModule::startOrderCollectionWindowIfNeeded() {
    if (orderCollectionActive || orderDecisionReceived) return;

    orderCollectionActive = true;
    orderBagProposed = false;
    orderBagRetransmitCount = 0;
    orderCollectionWindowStart = simTime();
    int viewLeader = establishedView.empty() ? -1 : getCurrentViewLeader(establishedView);
    bool iAmViewLeader = (viewLeader == replicaId);
    std::cout << "[METRICS " << replicaId << "] Order_QC_Window_Start: " << simTime()
            << " lanes_ready=" << countDistinctFrontLanesInPool() << "\n";
    std::cout << "[ORDER-DIAG] Replica " << replicaId
              << " epoch=" << currentEpoch
              << " phase=" << currentPhase
              << " viewSize=" << establishedView.size()
              << " poolSize=" << verifiedPool.size()
              << " viewLeader=" << viewLeader
              << " iAmViewLeader=" << (iAmViewLeader ? "true" : "false")
              << "\n";
    // Adaptive ORDER collection window: base + per-replica time, clamped to [min, max]
    // so late rounds (small n) finish sooner and final epoch can complete.
    double orderCollectMinSec = par("orderCollectMinSec").doubleValue();
    double orderCollectPerReplicaSec = par("orderCollectPerReplicaSec").doubleValue();
    double orderCollectMaxSec = par("orderCollectMaxSec").doubleValue();
    size_t n = establishedView.empty() ? 4 : establishedView.size();
    double windowSec = orderCollectMinSec + n * orderCollectPerReplicaSec;
    if (windowSec > orderCollectMaxSec) windowSec = orderCollectMaxSec;
    if (windowSec < orderCollectMinSec) windowSec = orderCollectMinSec;
    orderCollectionDeadline = simTime() + SimTime(windowSec);

    // schedule deadline
    scheduleAt(orderCollectionDeadline, orderCollectDeadlineTimer);

    // optional: schedule a few extra ReadyQC re-broadcasts with jitter
    scheduleAt(simTime() + uniform(0.001, 0.005), orderGossipRetransmitTimer);

    std::cout << "[ORDER-COLLECT] Replica " << replicaId
       << " started collection window until " << orderCollectionDeadline << "\n";

    // Early close if we already have enough lane fronts.
    // Always 4 lanes in this intersection; Byzantine/missing cars are handled
    // by the deadline timer proposing with whatever partial QCs exist.
    if (countDistinctFrontLanesInPool() >= 4) {
        orderBagCloseFlag = true;
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

        // Epoch filter removed here — verifiedPool already gates by epoch+view in handleReadyQCComplete

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
                  << " epoch=" << qc.epoch << "\n";
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
    int viewLeader = establishedView.empty() ? -1 : getCurrentViewLeader(establishedView);
    bool iAmViewLeader = (viewLeader == replicaId);
    if (!iAmViewLeader) {
        std::cout << "[ORDER-BAG] Replica " << replicaId
                  << " skipping ORDER submit reason=" << reason
                  << " (not view leader, leader=" << viewLeader << ")" << "\n";
        return;
    }
    
    orderCollectionWindowEnd = simTime();
    const double orderQcWindowDurationSec = (orderCollectionWindowEnd - orderCollectionWindowStart).dbl();
    std::cout << "[METRICS " << replicaId << "] Order_QC_Window_End: " << simTime()
            << " reason=" << reason
            << " lanes=" << countDistinctFrontLanesInPool()
            << " duration=" << orderQcWindowDurationSec << "s\n";
    orderQcWindowByEpochAndReplica[currentEpoch][replicaId] = orderQcWindowDurationSec;
    {
        const auto& epochQcWindows = orderQcWindowByEpochAndReplica[currentEpoch];
        if (epochQcWindows.size() >= 4 && printedOrderQcWindowAvgEpochs.count(currentEpoch) == 0) {
            double sumWindow = 0.0;
            double minWindow = std::numeric_limits<double>::max();
            double maxWindow = 0.0;
            for (const auto& kv : epochQcWindows) {
                sumWindow += kv.second;
                minWindow = std::min(minWindow, kv.second);
                maxWindow = std::max(maxWindow, kv.second);
            }
            double avgWindow = sumWindow / epochQcWindows.size();
            printedOrderQcWindowAvgEpochs.insert(currentEpoch);
            std::cout << "[ROUND-METRICS] Epoch " << currentEpoch
                      << " Avg_Order_QC_Window_4Cars: " << avgWindow
                      << " seconds (spread=" << (maxWindow - minWindow)
                      << "s, replicasCounted=" << epochQcWindows.size() << ")" << "\n";
        }
    }
    if (orderDecisionReceived) return;

    auto bag = buildOrderBagQCs();
    if (bag.empty()) {
        std::cout << "[ORDER-BAG] Replica " << replicaId << " no QCs to propose (" << reason << ")\n";
        return;
    }

    // closeFlag=true means Java should decide even partial candidates (used at deadline)
    bool closeFlag = orderBagCloseFlag || (reason == "DEADLINE");
    orderBagCloseFlag = closeFlag;

    if (!orderBagProposed) {
        // First proposal — record ORDER consensus start time and set phase
        orderConsensusStartTime = simTime();
        realOrderConsensusStart = std::chrono::high_resolution_clock::now();
        orderDecisionCallbackSeen = false;
        lastOrderBftRequestRttMs = -1.0;
        currentPhase = ORDER_CONSENSUS;
        std::cout << "[METRICS " << replicaId << "] Order_Consensus_Start: " << orderConsensusStartTime << "\n";
    }

    if (firstOrderBagProposalTime == 0) {
        firstOrderBagProposalTime = simTime();
        firstOrderBagProposerReplica = replicaId;
    }

    double sigToOrderStartGap = -1.0;
    if (orderSignatureCollectionEndTime > 0) {
        sigToOrderStartGap = (orderConsensusStartTime - orderSignatureCollectionEndTime).dbl();
    }
    std::cout << "[ORDER-DIAG] Replica " << replicaId
              << " epoch=" << currentEpoch
              << " reason=" << reason
              << " closeFlag=" << (closeFlag ? "true" : "false")
              << " bagSize=" << bag.size()
              << " viewSize=" << establishedView.size()
              << " frontLanes=" << countDistinctFrontLanesInPool()
              << " viewLeader=" << viewLeader
              << " iAmViewLeader=" << (iAmViewLeader ? "true" : "false")
              << " sigEndToOrderStartGap=" << sigToOrderStartGap
              << " firstLocalBagReplica=" << firstOrderBagProposerReplica
              << " firstLocalBagTime=" << firstOrderBagProposalTime
              << "\n";
    if (reason == "DEADLINE") {
        std::cout << "[ORDER-DIAG] Replica " << replicaId
                  << " DEADLINE_HIT epoch=" << currentEpoch
                  << " windowStart=" << orderCollectionWindowStart
                  << " deadline=" << orderCollectionDeadline
                  << " windowDuration=" << (orderCollectionWindowEnd - orderCollectionWindowStart).dbl()
                  << "s"
                  << "\n";
    }

    std::string payload = serializeOrderBagRequest(bag, closeFlag);
    std::cout << "[ORDER-BAG] Replica " << replicaId << " proposing bag size=" << bag.size()
              << " reason=" << reason << " closeFlag=" << closeFlag << " at t=" << simTime() << "\n";

    // 1. Kill the ARRIVAL_ANNOUNCE retransmit timer
    if (readyQCTimeoutTimer && readyQCTimeoutTimer->isScheduled()) {
        cancelEvent(readyQCTimeoutTimer);
        std::cout << "[V2VProxy " << replicaId << "] Cancelled ARRIVAL retransmit timer on ORDER submit" << "\n";
    }

    // 2. Kill the Gossip retransmit timer
    if (orderGossipRetransmitTimer && orderGossipRetransmitTimer->isScheduled()) {
        cancelEvent(orderGossipRetransmitTimer);
        std::cout << "[V2VProxy " << replicaId << "] Cancelled Gossip retransmit timer on ORDER submit" << "\n";
    }
    
    if (orderDelayGap > 0.0) {
        // Delay only the ORDER submit action; keep all other collection logic unchanged.
        std::string viewStr;
        for (const auto& car : establishedView) {
            if (!viewStr.empty()) viewStr += ",";
            viewStr += car;
        }
        pendingOrderPayload = payload;
        pendingOrderEpoch = currentEpoch;
        pendingOrderViewHash = computeXXHash32(viewStr);
        delayedOrderSubmitScheduled = true;
        if (orderDelayTimer->isScheduled()) {
            cancelEvent(orderDelayTimer);
        }
        std::cout << "[ORDER-DELAY] Replica " << replicaId
                  << " scheduled delayed ORDER submit at t=" << (simTime() + orderDelayGap)
                  << " gap=" << orderDelayGap << "s"
                  << " epoch=" << pendingOrderEpoch
                  << "\n";
        scheduleAt(simTime() + orderDelayGap, orderDelayTimer);
        orderBagProposed = true;
    } else {
        bool submitted = triggerJoinViaJNI(payload);
        if (submitted) {
            orderBagProposed = true;
        } else {
            // Java not ready yet — reschedule deadline so we retry in 0.5s
            std::cout << "[ORDER-BAG] Replica " << replicaId
                      << " triggerJoinViaJNI FAILED (reason=" << reason << "), rescheduling deadline in 0.5s\n";
            if (!orderCollectDeadlineTimer->isScheduled()) {
                scheduleAt(simTime() + 0.5, orderCollectDeadlineTimer);
            }
        }
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
    std::cout << "[METRICS " << replicaId << "] Order_Consensus_Start: " << orderConsensusStartTime << "\n";
    
    // View is already agreed - just trigger order consensus
    // All replicas have verifiedCars from VIEW phase
    // Java will check if our car is in agreedView
    std::string request = "ORDER_PROPOSE";
    std::string myCarId = "veh" + std::to_string(replicaId);

    // Check if verifiedPool has valid ReadyQC (should have been populated by assembleAndBroadcastReadyQC)
    if (verifiedPool.find(myCarId) == verifiedPool.end() || verifiedPool[myCarId].laneId.empty()) {
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: verifiedPool[" << myCarId 
                  << "] missing or empty! Cannot propose ORDER. Call assembleAndBroadcastReadyQC() first." << "\n";
        return;
    }

    ReadyQC qc = verifiedPool[myCarId];
    std::string qcString = serializeReadyQCToString(qc);
    std::cout << "[V2VProxy " << replicaId << "] Serialized ReadyQC: " << qcString << "\n";
    request += ":" + qcString;
    std::cout << "[V2VProxy " << replicaId << "] Request: " << request << "\n";

    currentPhase = ORDER_CONSENSUS;
    triggerJoinViaJNI(request);

    std::cout << "[V2VProxy " << replicaId << "] Triggered ORDER consensus" << "\n";
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
            // --- THE GLOBAL "SHUT UP" SIGNAL ---
            // Only cancel the arrival retransmit timer once this replica's own QC
            // is complete.  If we cancel unconditionally, any BFT VIEW-consensus
            // retransmit that arrives *after* phase-2 starts (while the 0.5s
            // initial timer is still pending) would silence the retransmit before
            // the first retry fires, starving replicas that missed the initial
            // flood due to simultaneous-transmission collisions.
            if (myReadyQCComplete && readyQCTimeoutTimer && readyQCTimeoutTimer->isScheduled()) {
                cancelEvent(readyQCTimeoutTimer);
                std::cout << "[V2VProxy " << replicaId << "] Cancelled ARRIVAL timer because BFT started (QC already complete)!" << "\n";
            }
            if (orderGossipRetransmitTimer && orderGossipRetransmitTimer->isScheduled()) {
                cancelEvent(orderGossipRetransmitTimer);
                std::cout << "[V2VProxy " << replicaId << "] Cancelled Gossip timer because BFT started!" << "\n";
            }

            handleBFTMessage(bftMsg);
            break;
        case 1:  // ARRIVAL_ANNOUNCE
            std::cout << " (ARRIVAL_ANNOUNCE)" << "\n";
            handleArrivalAnnouncement(bftMsg);
            break;
        case 2:  // WITNESS_RESPONSE
            std::cout << " (WITNESS_RESPONSE)" << "\n";
            handleWitnessResponse(bftMsg);
            break;
        case 3:  // READYQC_COMPLETE
            std::cout << " (READYQC_COMPLETE)" << "\n";
            handleReadyQCComplete(bftMsg);
            break;
        case 4:  // VIEW_PROPOSAL (Phase 1b - V2V agreement)
            std::cout << " (VIEW_PROPOSAL)" << "\n";
            handleViewProposal(bftMsg);
            break;
        case 5:  // VIEW_AGREEMENT (Phase 1b - V2V signatures)
            std::cout << " (VIEW_AGREEMENT)" << "\n";
            handleViewAgreement(bftMsg);
            break;
        case 6:  // READYQC_ACK (Gossip Retransmit ACK)
            std::cout << " (READYQC_ACK)" << "\n";
            handleReadyQCAck(bftMsg);
            break;
        case 7:  // EXECUTING — batch crossing, locks new arrivals
            std::cout << " (EXECUTING)" << "\n";
            handleExecutingMessage(bftMsg);
            break;
        default:
            std::cout << " (UNKNOWN)" << "\n";
            EV_WARN << "Unknown message type: " << msgType << "\n";
    }
    delete bftMsg;
}


void V2VProxyModule::handleOrderDecision(const std::string& orderDecision) {
    std::cout << "[V2VProxy " << replicaId << "] handleOrderDecision: " << orderDecision << "\n";

    // THREAD SAFETY: this method is called from JNI threads (Java BFT-SMaRt).
    // In the final epoch every departing replica proactively notifies every other
    // departing replica, so up to N JNI threads can call handleOrderDecision on
    // the SAME proxy concurrently.  OMNeT++'s FES is a std::map (red-black tree)
    // that is NOT thread-safe; calling cancelEvent() or scheduleAt() from a JNI
    // thread while the OMNeT++ main thread is also touching the FES causes the
    // std::_Rb_tree_insert_and_rebalance SIGSEGV seen in the crash log.
    //
    // Fix: only update mutex-protected flags here.  The main thread's
    // processQueueTimer callback drains pendingOrderDecision and performs all
    // OMNeT++ scheduler calls safely on the simulation thread.
    std::lock_guard<std::mutex> lock(jniMutex);

    orderDecisionReceived = true;
    pendingCancelOrderTimer = true;   // main thread will call cancelEvent safely
    delayedOrderSubmitScheduled = false;
    pendingOrderPayload.clear();
    pendingOrderEpoch = -1;
    pendingOrderViewHash = 0;
    orderDecisionCallbackSeen = true;
    realOrderConsensusEnd = std::chrono::high_resolution_clock::now();

    // Dedup: first JNI thread to arrive wins; later duplicate calls are ignored.
    if (pendingOrderDecision.empty()) {
        pendingOrderDecision = orderDecision;
    }
    // parseAndNotifyDecision() and cancelEvent() are deferred to the main thread.
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
    std::cout << "[V2VProxy " << replicaId << "] parseAndNotifyDecision: " << decision << "\n";

    // New batch format: "veh0:BATCH:0;veh1:BATCH:0;veh2:BATCH:1;veh3:BATCH:2"
    pendingBatches.clear();
    currentBatchIndex = 0;
    currentBatchExpected.clear();
    confirmedDeparted.clear();
    expectedToGo.clear();

    std::vector<std::string> carEntries = split(decision, ';');
    for (const std::string& entry : carEntries) {
        std::vector<std::string> parts = split(entry, ':');
        if (parts.size() >= 3 && parts[1] == "BATCH") {
            int batchIdx = std::stoi(parts[2]);
            while ((int)pendingBatches.size() <= batchIdx) pendingBatches.push_back({});
            pendingBatches[batchIdx].push_back(parts[0]);
        }
    }

    if (pendingBatches.empty()) {
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: no BATCH entries in decision: " << decision << "\n";
        return;
    }

    std::cout << "[V2VProxy " << replicaId << "] ORDER decided: " << pendingBatches.size()
              << " batch(es) — starting batch 0\n";
    executeBatch(0);
}

void V2VProxyModule::executeBatch(int idx) {
    if (idx < 0 || idx >= (int)pendingBatches.size()) {
        // All batches done — trigger global reset
        std::cout << "[BATCH] Replica " << replicaId << " all " << pendingBatches.size()
                  << " batches complete — triggering global reset\n";
        std::vector<int> departedIds;
        for (const auto& batch : pendingBatches)
            for (const auto& carId : batch)
                departedIds.push_back(extractReplicaIdFromCarId(carId));
        notifyJavaNewBatchSize(0);
        triggerGlobalResetViaJNI(departedIds);
        return;
    }

    currentBatchIndex = idx;
    currentBatchExpected.clear();
    confirmedDeparted.clear();
    expectedToGo.clear();

    std::string myCarId = "veh" + std::to_string(replicaId);
    bool myTurn = false;

    for (const auto& carId : pendingBatches[idx]) {
        currentBatchExpected.insert(carId);
        expectedToGo.insert(carId);
        if (carId == myCarId) myTurn = true;
    }

    // Set trailing-car trigger for accordion movement
    myLaneTriggerCar = "";
    for (const auto& carId : pendingBatches[idx]) {
        if (carId != myCarId &&
            std::find(laneQueue.begin(), laneQueue.end(), carId) != laneQueue.end()) {
            myLaneTriggerCar = carId;
            break;
        }
    }

    std::cout << "[BATCH] Replica " << replicaId << " executing batch " << idx << " (";
    for (const auto& c : currentBatchExpected) std::cout << c << " ";
    std::cout << ") myTurn=" << myTurn << "\n";

    if (myTurn) {
        broadcastExecutingMessage(idx);  // Stage 11: lock intersection for late arrivals
        resumeVehicle(0.0);              // Batch ordering IS the safety delay
    }

    isWaitingForClearance = true;
    clearanceStartTime = simTime();
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

    // All vehicles on this lane (including self). TraCI lane position increases along the lane
    // toward the intersection on approach edges, so larger position = closer to the junction.
    std::vector<std::pair<double, std::string>> inLane;
    for (const auto& vid : traciCmd->getVehicleIds()) {
        auto v = traciCmd->vehicle(vid);
        if (v.getLaneId() == myLaneId) {
            inLane.push_back({v.getLanePosition(), vid});
        }
    }

    // Immediate leader toward the intersection: smallest lane position strictly greater than ours.
    carAhead = "";
    carAheadStopPos = -1.0;
    double bestAbove = std::numeric_limits<double>::infinity();
    for (const auto& [pos, id] : inLane) {
        if (pos > mypos && pos < bestAbove) {
            bestAbove = pos;
            carAhead = id;
            carAheadStopPos = pos;
        }
    }

    // Front of platoon first (highest lane position = rank 1) so positionInLane matches Java ORDER logic.
    std::sort(inLane.begin(), inLane.end(),
              [](const std::pair<double, std::string>& a, const std::pair<double, std::string>& b) {
                  return a.first > b.first;
              });

    laneQueue.clear();
    for (const auto& [pos, id] : inLane) {
        laneQueue.push_back(id);
    }
    laneDiscovered = true;
    std::cout << "[V2VProxy " << replicaId << "] Lane discovered: " << myLaneId << "\n";
    std::cout << "[V2VProxy " << replicaId << "] Car ahead: " << carAhead << "\n";
    std::cout << "[V2VProxy " << replicaId << "] Car ahead stop pos: " << carAheadStopPos << "\n";

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


void V2VProxyModule::handleWipeComplete() {
    // Called from JNI thread via notifyWipeComplete after doWipeAndReinit() completes in Java.
    // Stage 11 (epoch preemption) will flesh this out. For now: reset local protocol state
    // and increment epoch so C++ commands the new participants to re-announce.
    std::lock_guard<std::mutex> lock(jniMutex);
    std::cout << "[WIPE] Replica " << replicaId << " handleWipeComplete — resetting for new epoch\n";
    pendingBatches.clear();
    currentBatchIndex = 0;
    currentBatchExpected.clear();
    confirmedDeparted.clear();
    expectedToGo.clear();
    viewState.clear();
    arrivalAnnouncementsReceived.clear();
    viewEstablished = false;
    currentEpoch++;
    currentPhase = IDLE;
    intersectionLocked = false;
    bufferedNewArrivals.clear();
    // Main thread will detect IDLE and trigger fresh ARRIVAL_ANNOUNCE when ready
}

// ============================================================================
// SERIALIZATION FUNCTIONS
// ============================================================================

// ============================================================================
// VIEW CONSENSUS SERIALIZATION (Phase 1)
// ============================================================================

std::vector<uint8_t> V2VProxyModule::serializeViewProposal(const ViewProposal& proposal) {
    // Format: proposerId|vehicleStatesStr|timestamp|siglen|sig
    // vehicleStatesStr: "veh0|N|1|S|0;veh1|S|1|L|0" (semicolon between cars, pipe within)
    std::stringstream ss;
    ss << proposal.proposerReplicaId << "|"
       << proposal.vehicleStatesStr  << "|"
       << proposal.proposalTimestamp << "|"
       << proposal.signature.size() << "|";

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

    // Format: proposerId|vehicleStatesStr|timestamp|siglen|sig
    // vehicleStatesStr contains '|' within each car record, so we can't just split('|').
    // Parse by finding the first '|' (proposerId), then scan for the ';'-delimited vehicleStatesStr
    // region, then the remaining fields.
    // Easier: locate field boundaries by counting: field[0] ends at 1st '|', field[3] (siglen)
    // is between the 3rd-from-last and 2nd-from-last '|' before the binary blob.
    // Safest: the text header ends at the 4th '|' that belongs to our fixed fields (proposerId,
    // vehicleStatesStr, timestamp, siglen). But vehicleStatesStr itself contains '|'.
    // Solution: store vehicleStatesStr as field[1] by using a different delimiter.
    // -- The vehicleStatesStr ends at the semicolon sequence; since the outer delimiter is '|'
    //    and the inner car-field delimiter is also '|', we use a different approach:
    //    After proposerId, the vehicleStatesStr ends at the LAST ';'-terminated segment before
    //    the timestamp float. This is ambiguous. Instead: at serialization we ensure
    //    vehicleStatesStr uses ';' between cars and ',' within records to avoid '|' conflicts.
    //    BUT for now vehicleStatesStr uses '|' within records (veh0|N|1|S|0;...).
    //    Simple parse: split on '|', then fields[1..5N] are the vehicleStates records interleaved
    //    with ';' as car separators. The final three '|'-delimited tokens are: timestamp, siglen, sig.
    //
    // We adopt the simplest correct approach: scan the raw string for the pattern
    // "proposerId|<vsStr>|<double>|<int>|<blob>" by:
    //   1. Find first '|' → proposerId
    //   2. Find last occurrence of "|\d+|" pattern before the blob → that's "|siglen|"
    //   3. Everything between step 1 and the previous '|' before siglen is vsStr+"|"+timestamp

    std::string s(payload.begin(), payload.end());
    ViewProposal proposal;

    // Step 1: extract proposerId
    size_t p1 = s.find('|');
    if (p1 == std::string::npos) return proposal;
    proposal.proposerReplicaId = std::stoi(s.substr(0, p1));

    // The rest: "<vsStr>|<timestamp>|<siglen>|<blob>"
    // We know siglen is a small non-negative integer. Scan backwards from payload end.
    // The binary blob starts right after the 4th '|' from right in the text part.
    // Walk backward: find siglen first (it's the last text field before the blob).
    // Strategy: scan for last '|<digits>|' near the end of the text area.
    // We find: last '|' that is followed only by digits until another '|' or end.
    size_t p_siglen = std::string::npos;
    size_t p_ts     = std::string::npos;

    // Walk backwards from the end to find |siglen|
    for (size_t i = s.size(); i > p1 + 1; ) {
        --i;
        if (s[i] != '|') continue;
        // Check if s[i+1..next_pipe] is all digits
        size_t j = i + 1;
        while (j < s.size() && s[j] != '|' && (s[j] >= '0' && s[j] <= '9')) ++j;
        if (j < s.size() && s[j] == '|' && j > i + 1) {
            // Candidate: s[i+1..j-1] is the siglen, s[j] is the pipe before the blob
            p_siglen = i;  // position of '|' before siglen
            // Now find the '|' before timestamp (one more '|' backwards)
            for (size_t k = p_siglen; k > p1 + 1; ) {
                --k;
                if (s[k] == '|') { p_ts = k; break; }
            }
            break;
        }
    }

    if (p_siglen == std::string::npos || p_ts == std::string::npos) return proposal;

    // vehicleStatesStr is between p1+1 and p_ts
    proposal.vehicleStatesStr = s.substr(p1 + 1, p_ts - p1 - 1);
    proposal.proposalTimestamp = std::stod(s.substr(p_ts + 1, p_siglen - p_ts - 1));
    int siglen = std::stoi(s.substr(p_siglen + 1));  // from siglen field start (digits only)

    // Actual siglen field ends at the pipe after it
    size_t blob_start_pipe = s.find('|', p_siglen + 1);
    size_t offset = (blob_start_pipe != std::string::npos) ? blob_start_pipe + 1 : s.size();

    if (offset < payload.size() && offset + (size_t)siglen <= payload.size()) {
        proposal.signature.assign(payload.begin() + offset, payload.begin() + offset + siglen);
    }

    // Also populate observedCars from vehicleStatesStr (carId is first pipe-field of each record)
    if (!proposal.vehicleStatesStr.empty()) {
        std::vector<std::string> recs = split(proposal.vehicleStatesStr, ';');
        for (const auto& rec : recs) {
            size_t pp = rec.find('|');
            if (pp != std::string::npos) {
                proposal.observedCars.insert(rec.substr(0, pp));
            } else if (!rec.empty()) {
                proposal.observedCars.insert(rec);
            }
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
        // Find offset of raw bytes by locating the 3rd '|' scanning forward
        // through the text header only.  find_last_of('|') is wrong because
        // the binary signature bytes can contain 0x7C ('|'), causing it to
        // land inside the payload and produce an empty or corrupt signature.
        size_t p1 = s.find('|');
        size_t p2 = (p1 != std::string::npos) ? s.find('|', p1 + 1) : std::string::npos;
        size_t p3 = (p2 != std::string::npos) ? s.find('|', p2 + 1) : std::string::npos;
        size_t offset = (p3 != std::string::npos) ? p3 + 1 : s.size();

        if (offset < payload.size() && offset + (size_t)siglen <= payload.size()) {
            agreement.signature.assign(payload.begin() + offset, payload.begin() + offset + siglen);
        }
    }

    return agreement;
}

// ============================================================================
// READYQC SERIALIZATION (Phase 2)
// ============================================================================

static std::string dirToStr(V2VProxyModule::Direction d) {
    switch (d) {
        case V2VProxyModule::DIR_LEFT:     return "L";
        case V2VProxyModule::DIR_RIGHT:    return "R";
        default:                           return "S";
    }
}

static V2VProxyModule::Direction strToDir(const std::string& s) {
    if (s == "L") return V2VProxyModule::DIR_LEFT;
    if (s == "R") return V2VProxyModule::DIR_RIGHT;
    return V2VProxyModule::DIR_STRAIGHT;
}

// Hex encode/decode helpers for ambulance cert bytes
static std::string toHex(const std::vector<uint8_t>& v) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(v.size() * 2);
    for (uint8_t b : v) { out += digits[b >> 4]; out += digits[b & 0xf]; }
    return out;
}

static std::vector<uint8_t> fromHex(const std::string& s) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        out.push_back((uint8_t)std::stoi(s.substr(i, 2), nullptr, 16));
    }
    return out;
}

std::vector<uint8_t> V2VProxyModule::serializeArrivalAnnouncement(const ArrivalAnnouncement& ann) {
    // New format: carId|laneId|lane|posInLane|direction|isAmbulance|time|epoch
    //             |certHex|sigHex|selfSigLen|selfSig
    // Fields 0-7 are text; certHex/sigHex are empty strings when !isAmbulance.
    std::stringstream ss;
    ss << std::setprecision(17);
    ss << ann.carId           << "|"
       << ann.laneId          << "|"
       << ann.lane            << "|"
       << ann.positionInLane  << "|"
       << dirToStr(ann.direction) << "|"
       << (ann.isAmbulance ? "1" : "0") << "|"
       << ann.claimedArrivalTime << "|"
       << ann.epoch           << "|"
       << toHex(ann.ambulanceCertBytes) << "|"
       << toHex(ann.ambulanceSigBytes)  << "|"
       << ann.signature.size() << "|";

    std::string header = ss.str();
    std::vector<uint8_t> result(header.begin(), header.end());
    result.insert(result.end(), ann.signature.begin(), ann.signature.end());
    return result;
}

V2VProxyModule::ArrivalAnnouncement V2VProxyModule::deserializeArrivalAnnouncement(BFTMessage* bftMsg) {
    std::vector<uint8_t> payload(bftMsg->getPayloadArraySize());
    for (size_t i = 0; i < payload.size(); i++) payload[i] = bftMsg->getPayload(i);

    std::string s(payload.begin(), payload.end());
    std::vector<std::string> parts = split(s, '|');

    ArrivalAnnouncement ann;
    // Minimum fields: 0..10 text + binary self-sig
    if (parts.size() >= 11) {
        ann.carId             = parts[0];
        ann.laneId            = parts[1];
        ann.lane              = parts[2];
        ann.positionInLane    = std::stoi(parts[3]);
        ann.direction         = strToDir(parts[4]);
        ann.isAmbulance       = (parts[5] == "1");
        ann.claimedArrivalTime= std::stod(parts[6]);
        ann.epoch             = std::stoi(parts[7]);
        ann.ambulanceCertBytes= fromHex(parts[8]);
        ann.ambulanceSigBytes = fromHex(parts[9]);

        int siglen = std::stoi(parts[10]);
        // Locate 11th '|' to find the binary self-sig offset
        size_t p = s.find('|');
        for (int k = 1; k < 11 && p != std::string::npos; ++k) p = s.find('|', p + 1);
        size_t offset = (p != std::string::npos) ? p + 1 : s.size();
        if (offset < payload.size() && offset + (size_t)siglen <= payload.size()) {
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
    std::cout << "[V2VProxy " << replicaId << "] deserializeWitnessResponse called at t=" << simTime() << "\n";
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
        std::cout << "[DESER_WITNESS] siglen=" << siglen << ", offset=" << offset << ", payload.size()=" << payload.size() << "\n";

        if (offset + siglen <= payload.size()) {
            witness.signature.assign(payload.begin() + offset, payload.begin() + offset + siglen);
            std::cout << "[DESER_WITNESS] Extracted " << witness.signature.size() << " signature bytes" << "\n";
        } else {
            std::cerr << "[DESER_WITNESS] ERROR: Signature extraction failed! offset=" << offset << ", siglen=" << siglen << ", payload.size()=" << payload.size() << "\n";
        }
    }

    std::cout << "[V2VProxy " << replicaId << "] deserializeWitnessResponse completed at t=" << simTime() << "\n";

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
                          << " payload.size()=" << payload.size() << "\n";
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
    
    // ZOMBIE FILTER: Departed cars don't see anyone
    if (isDeparted) {
        std::set<std::string> visible;
        visible.insert("veh" + std::to_string(replicaId));  // Only themselves
        return visible;
    }
    
    std::set<std::string> visible;
    
    if (!mobility) {
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: No mobility for getVisibleVehicles" << "\n";
        return visible;
    }
    
    TraCICommandInterface* traci = mobility->getCommandInterface();
    if (!traci) {
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: No TraCI for getVisibleVehicles" << "\n";
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

// Build the canonical semicolon-pipe vehicleStates string from the local viewState map.
// Format: "veh0|N|1|S|0;veh1|S|1|L|0;veh2|W|2|R|1"  (sorted by vehicleId for determinism)
std::string V2VProxyModule::buildVehicleStatesStr() const {
    std::vector<std::pair<std::string, VehicleState>> sorted(viewState.begin(), viewState.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    std::string result;
    for (const auto& kv : sorted) {
        const VehicleState& vs = kv.second;
        if (!result.empty()) result += ";";
        result += vs.vehicleId + "|" + vs.lane + "|"
               + std::to_string(vs.positionInLane) + "|"
               + dirToStr(vs.direction) + "|"
               + (vs.isAmbulance ? "1" : "0");
    }
    return result;
}

std::vector<uint8_t> V2VProxyModule::signViewProposal(const std::set<std::string>& viewSet) {
    // NEW: sign the vehicleStatesStr (not just carId list) to cover VehicleState data.
    // Input: vehicleStatesStr + ":" + signerReplicaId
    // Falls back to carId-list signing when viewState is empty (e.g. early VIEW_PROPOSAL from old code).
    std::string toSign;
    std::string vsStr = buildVehicleStatesStr();
    if (!vsStr.empty()) {
        toSign = vsStr + ":" + std::to_string(replicaId);
    } else {
        // Fallback: legacy carId-list format
        for (const std::string& carId : viewSet) {
            if (!toSign.empty()) toSign += ",";
            toSign += carId;
        }
        toSign += ":" + std::to_string(replicaId);
    }

    int32_t hash = computeXXHash32(toSign);
    std::vector<uint8_t> sig(sizeof(int32_t));
    std::memcpy(sig.data(), &hash, sizeof(int32_t));

    std::cout << "[VIEW_SIGN] Replica " << replicaId << " signed vehicleStates hash=" << hash << "\n";
    return sig;
}


void V2VProxyModule::initiateViewProposal() {
    // Called when all BATCH_SIZE ARRIVAL_ANNOUNCEs are collected (or IDLE → PROPOSING_VIEW at arrival).
    if (currentPhase != PROPOSING_VIEW) {
        std::cout << "[V2VProxy " << replicaId << "] Cannot initiate view proposal - phase="
                  << currentPhase << " (expected PROPOSING_VIEW)\n";
        return;
    }

    std::cout << "[V2VProxy " << replicaId << "] ===== PHASE 1b: BROADCASTING VIEW_PROPOSAL =====\n";

    // Build the vehicleStates string from the collected viewState map
    std::string vsStr = buildVehicleStatesStr();

    // Collect car IDs for observedCars field
    std::set<std::string> carIds;
    for (const auto& kv : viewState) carIds.insert(kv.first);

    std::cout << "[V2VProxy " << replicaId << "] vehicleStatesStr: " << vsStr << "\n";

    myViewProposal.proposerReplicaId = replicaId;
    myViewProposal.vehicleStatesStr  = vsStr;
    myViewProposal.observedCars      = carIds;
    myViewProposal.proposalTimestamp = simTime().dbl();
    myViewProposal.signature         = signViewProposal(carIds);  // signs vsStr + replicaId

    broadcastViewProposal();
    currentPhase = VIEW_AGREEMENT;
}

void V2VProxyModule::broadcastViewProposal() {
     // ZOMBIE FILTER: Departed cars don't broadcast views
    if (zombieFilter()) return;
    
    
    
    std::cout << "[V2VProxy " << replicaId << "] Broadcasting view proposal via V2V..." << "\n";
    
    std::vector<uint8_t> payload = serializeViewProposal(myViewProposal);
    sendBFTMessage(replicaId, -1, payload, 4);  // messageType=4 (VIEW_PROPOSAL)
    
    std::cout << "[V2VProxy " << replicaId << "] Broadcasted view with " 
              << myViewProposal.observedCars.size() << " cars" << "\n";
}

void V2VProxyModule::handleViewProposal(BFTMessage* bftMsg) {
    
    // ZOMBIE FILTER: Departed cars don't accept view proposals
    if (zombieFilter()) return;
    
    ViewProposal proposal = deserializeViewProposal(bftMsg);
    
    std::cout << "[V2VProxy " << replicaId << "] Received view proposal from replica " 
              << proposal.proposerReplicaId << "\n";
    std::cout << "[V2VProxy " << replicaId << "]   Their view: {";
    for (const auto& car : proposal.observedCars) {
        std::cout << car << " ";
    }
    std::cout << "}" << "\n";
    
    // Build my vehicleStates string and compare to the proposal
    std::string myVsStr = buildVehicleStatesStr();

    std::cout << "[V2VProxy " << replicaId << "]   My vehicleStatesStr: " << myVsStr << "\n";
    std::cout << "[V2VProxy " << replicaId << "]   Proposal vehicleStatesStr: " << proposal.vehicleStatesStr << "\n";

    if (!proposal.vehicleStatesStr.empty() && proposal.vehicleStatesStr == myVsStr) {
        std::cout << "[V2VProxy " << replicaId << "] AGREEMENT: vehicleStates match! Sending V2V signature...\n";

        // Must match Java verifyViewSignature: XXHash32(vehicleStatesStr + ":" + signingReplicaId)
        std::string toSign = proposal.vehicleStatesStr + ":" + std::to_string(replicaId);
        int32_t hash = computeXXHash32(toSign);
        std::vector<uint8_t> sig(sizeof(int32_t));
        std::memcpy(sig.data(), &hash, sizeof(int32_t));

        ViewAgreement agreement;
        agreement.agreingReplicaId = replicaId;
        agreement.agreedView       = proposal.observedCars;
        agreement.signature        = sig;

        std::vector<uint8_t> payload = serializeViewAgreement(agreement);
        sendBFTMessage(replicaId, proposal.proposerReplicaId, payload, 5);
    } else {
        std::cout << "[V2VProxy " << replicaId << "] DISAGREEMENT: vehicleStates don't match (have "
                  << viewState.size() << "/" << BATCH_SIZE << " VehicleStates). Not signing.\n";
    }
}

// Use the AGREED view (not live visibility) so all replicas elect the same leader.
int V2VProxyModule::getCurrentViewLeader(const std::set<std::string>& agreedView) {
    if (agreedView.empty()) return -1;
    
    int minId = INT_MAX;
    
    for (const std::string& veh : agreedView) {
        try {
            int currentId = std::stoi(veh.substr(3));
            minId = std::min(minId, currentId);
        } catch (const std::exception& e) {
            EV_ERROR << "[VIEW] Failed to parse vehicle ID from: " << veh << "\n";
        }
    }

    return minId;
}

bool V2VProxyModule::amITheLeader(const std::set<std::string>& agreedView) {
    return (getCurrentViewLeader(agreedView) == replicaId);
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
                  << agreement.agreingReplicaId << "\n";
        return;
    }

    // 4. Validate signature BEFORE counting — a vote with an empty/malformed
    //    signature will be skipped during serialization, so counting it here
    //    would make voteCount drift above the actual number of sigs Java receives.
    if (agreement.signature.size() < 4) {
        std::cout << "[V2VProxy " << replicaId << "] IGNORING vote from replica "
                  << agreement.agreingReplicaId << " — signature.size()="
                  << agreement.signature.size() << " (need >=4)" << "\n";
        return;
    }

    // 5. Record the unique, valid vote
    votes.push_back(agreement);

    int voteCount = votes.size();
    std::cout << "[V2VProxy " << replicaId << "] Received NEW valid V2V agreement from " 
              << agreement.agreingReplicaId << ". Valid votes: " << voteCount << "\n";
    
    // 5. Check if we have f+1 V2V agreements on this view
    // std::set<std::string> myView = getVisibleVehicles(300.0);
    int viewSize = agreement.agreedView.size();
    int f = (viewSize - 1) / 3;

    int required = f + 1;
    std::cout << "[V2VProxy " << replicaId << "] NEW agreement from " 
              << agreement.agreingReplicaId << ". Bucket for this exact view has: " 
              << voteCount << "/" << required << " votes. (View Size: " << viewSize << ")" << "\n";
    
    if (voteCount >= required && !viewEstablished) {
        viewSignatureCollectionEndTime = simTime();
        viewEstablished = true;

        std::cout << "[V2VProxy " << replicaId << "] ===== PHASE 1c: SUBMITTING TO BFT CONSENSUS =====" << "\n";
        std::cout << "[V2VProxy " << replicaId << "] Collected f+1=" << required 
                  << " V2V signatures for view: {";
        for (const auto& car : agreement.agreedView) {
            std::cout << car << " ";
        }
        std::cout << "}" << "\n";

       //submitViewToBFTConsensus(agreement.agreedView, votes);

        // Use the AGREED view (not live visibility) to elect the leader deterministically.
        // // All replicas with the same agreedView will compute the same leader.
        if (amITheLeader(agreement.agreedView)) {
            std::cout << "[V2VProxy " << replicaId << "] ===== PHASE 1c: LEADER SUBMITTING TO BFT =====" << "\n";
            submitViewToBFTConsensus(agreement.agreedView, votes);
        } else {
            std::cout << "[V2VProxy " << replicaId << "] I am a follower. Waiting for BFT delivery via Java callback." << "\n";
        }
    }
}

void V2VProxyModule::submitViewToBFTConsensus(const std::set<std::string>& view,
                                                const std::vector<ViewAgreement>& v2vSigs) {
    viewConsensusStartTime = simTime();
    realViewConsensusStart = std::chrono::high_resolution_clock::now();
    std::cout << "[V2VProxy " << replicaId << "] Submitting view to BFT-SMaRt consensus...\n";
    std::cout << "[METRICS " << replicaId << "] View_Consensus_Start: " << viewConsensusStartTime << "\n";

    // New wire format: "VIEW_PROPOSE:<proposerId>:<vehicleStatesStr>:<viewSignatures>"
    // vehicleStatesStr: "veh0|N|1|S|0;veh1|S|1|L|0;..."
    // viewSignatures:   "replicaId,XXHash32Decimal|replicaId,XXHash32Decimal|..."
    //   XXHash32 input: vehicleStatesStr + ":" + signingReplicaId (matches Java verifyViewSignature)

    std::string vsStr = buildVehicleStatesStr();

    std::stringstream ss;
    ss << "VIEW_PROPOSE:" << replicaId << ":" << vsStr << ":";

    // Append collected VIEW_AGREEMENT signatures
    bool firstSig = true;
    for (const ViewAgreement& sig : v2vSigs) {
        if (sig.signature.size() >= 4) {
            if (!firstSig) ss << "|";
            int32_t hashValue;
            std::memcpy(&hashValue, sig.signature.data(), sizeof(int32_t));
            ss << sig.agreingReplicaId << "," << hashValue;
            firstSig = false;
        } else {
            std::cerr << "[V2VProxy " << replicaId << "] WARNING: skipping sig from replica "
                      << sig.agreingReplicaId << " (size=" << sig.signature.size() << ")\n";
        }
    }

    std::string request = ss.str();
    std::cout << "[V2VProxy " << replicaId << "] BFT VIEW_PROPOSE: " << request << "\n";

    if (!triggerJoinViaJNI(request)) {
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: Failed to submit view to BFT — storing for retry when Java ready\n";
        pendingViewProposalRequest = request;  // will be retried by checkJavaReadyTimer
    }
}

void V2VProxyModule::onViewAgreed(const std::set<std::string>& agreedView) {
    std::lock_guard<std::mutex> lock(jniMutex);

    // Idempotency guard: phase2 must only be triggered once.
    // The leader can receive this callback TWICE:
    //   1. From appExecuteBatch (delivery thread, fires for ALL replicas), and
    //   2. From sendConsensusRequest's proxy reply (only the invokeOrdered caller).
    // Followers only receive it once from appExecuteBatch.
    if (phase2Pending) {
        std::cout << "[V2VProxy " << replicaId << "] onViewAgreed called again - already triggered Phase 2, ignoring." << "\n";
        return;
    }
    
    // Mark the end of View consensus
    viewConsensusEndTime = simTime();
    simtime_t viewConsensusDuration = viewConsensusEndTime - viewConsensusStartTime;
    realViewConsensusEnd = std::chrono::high_resolution_clock::now();
    auto realViewConsensusDuration = std::chrono::duration_cast<std::chrono::milliseconds>(realViewConsensusEnd - realViewConsensusStart);
    std::cout << "[METRICS " << replicaId << "] View_Consensus_Duration: " << realViewConsensusDuration.count() << "ms" << "\n";
    std::cout << "[V2VProxy " << replicaId << "] Phase 2 flag SET via JNI thread." << "\n";
    std::cout << "[METRICS " << replicaId << "] View_Consensus_End: " << viewConsensusEndTime << "\n";
    std::cout << "[METRICS " << replicaId << "] View_Consensus_Latency: " << viewConsensusDuration.dbl() << " seconds" << "\n";
    if (lastRoundResetTime >= 0 && lastRoundResetEpoch == currentEpoch) {
        const double resetToViewEndSec = (viewConsensusEndTime - lastRoundResetTime).dbl();
        std::cout << "[ROUND-DIAG] Replica " << replicaId
                  << " epoch=" << currentEpoch
                  << " resetToViewEnd=" << resetToViewEndSec
                  << " seconds (resetAt=" << lastRoundResetTime
                  << ", viewEnd=" << viewConsensusEndTime << ")" << "\n";
        resetToViewEndByEpochAndReplica[currentEpoch][replicaId] = resetToViewEndSec;
        const auto& epochResetToView = resetToViewEndByEpochAndReplica[currentEpoch];
        if (epochResetToView.size() >= 4 && printedResetToViewEndAvgEpochs.count(currentEpoch) == 0) {
            double sumDelay = 0.0;
            for (const auto& kv : epochResetToView) {
                sumDelay += kv.second;
            }
            printedResetToViewEndAvgEpochs.insert(currentEpoch);
            std::cout << "[ROUND-METRICS] Epoch " << currentEpoch
                      << " Avg_ResetToViewEnd_4Cars: " << (sumDelay / epochResetToView.size())
                      << " seconds (replicasCounted=" << epochResetToView.size() << ")" << "\n";
        }
    }
    
    // Within-epoch H1 fix: once VIEW is decided, any remaining VIEW WRITE/ACCEPT retransmissions
    // are stale and should not spill into the ORDER broadcast window.
    // // Clear this replica's reliability-layer unacked queue now (before ORDER starts).
    // if (jvm && javaReady) {
    //     JNIEnv* env;
    //     jvm->AttachCurrentThread((void**)&env, nullptr);
    //     jclass cls = env->FindClass("bftsmart/communication/V2V/ReliableV2VMessaging");
    //     if (cls) {
    //         jmethodID m = env->GetStaticMethodID(cls, "clearUnackedForReplica", "(I)V");
    //         if (m) env->CallStaticVoidMethod(cls, m, (jint)replicaId);
    //         if (env->ExceptionCheck()) env->ExceptionClear();
    //     }
    // }

    establishedView = agreedView;
    viewEstablished = true;
    // NEW PROTOCOL: No Phase 2. Java leader internally submits ORDER_PROPOSE after VIEW consensus.
    // C++ waits for notifyOrderDecided JNI callback to start batch execution.
    std::cout << "[V2VProxy " << replicaId << "] VIEW CONSENSUS COMPLETE at t=" << simTime()
              << " — waiting for Java ORDER_PROPOSE consensus\n";
}

void V2VProxyModule::startReadyQCCollection() {
    std::cout << "[V2VProxy " << replicaId << "] ===== PHASE 2: STARTING READYQC COLLECTION =====" << "\n";
    std::cout << "[V2VProxy " << replicaId << "] View members: " << establishedView.size() << " cars" << "\n";
    arrivalAnnouncementsReceived.clear();
    readyQCAcks.clear();
    currentPhase = COLLECTING_QC;
    orderSignatureCollectionEndTime = 0;  // Reset so end can't precede start
    orderSignatureCollectionStartTime = simTime();
    std::string myCarId = "veh" + std::to_string(replicaId);
    if (establishedView.find(myCarId) != establishedView.end()) {
        std::cout << "[V2VProxy " << replicaId << "] I AM in the view - broadcasting arrival announcement" << "\n";
        // Broadcast arrival announcement so neighbors can witness
        broadcastArrivalAnnouncement();
        arrivalAnnouncementsReceived.insert(myCarId);
        // Schedule retransmit in case the broadcast was lost due to CSMA collision
        if (readyQCTimeoutTimer && !readyQCTimeoutTimer->isScheduled()) {
            scheduleAt(simTime() + 0.5, readyQCTimeoutTimer);
        }
    } else {
        std::cout << "[V2VProxy " << replicaId << "] I am NOT in the view - will act as witness only" << "\n";
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
        std::cout << "[FRONT_CHECK] " << carId << " not found in TraCI vehicle list" << "\n";
        return false;
    }

    std::vector<std::pair<std::string,double>> sameLane;
    for (const auto& vid : allIds) {
        try {

             int otherRepId = extractReplicaIdFromCarId(vid);
        V2VProxyModule* otherProxy = getProxyForReplica(otherRepId);
        if (otherProxy && otherProxy->isDeparted) continue; // CRITICAL: Ignore departed cars
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
    std::cout << "\n";

    // Confirm carId is actually in laneId (not in an internal junction lane, etc.)
    std::string carActualLane = traci->vehicle(carId).getLaneId();
    if (carActualLane != laneId) {
        std::cout << "[FRONT_CHECK] " << carId << " is in lane " << carActualLane
                  << ", not " << laneId << "\n";
        return false;
    }

    // Higher lanePosition  =  further along the lane  =  closer to the junction.
    double carLanePos = traci->vehicle(carId).getLanePosition();
    std::cout << "[FRONT_CHECK] " << carId << " lanePos=" << carLanePos
              << " in lane " << laneId << "\n";

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
                      << " > " << carLanePos << "\n";
            return false;
        }
    }

    std::cout << "[FRONT_CHECK] " << carId << " IS front of " << laneId
              << " (lanePos=" << carLanePos << ")" << "\n";
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
    std::cerr << "[V2VProxy " << replicaId << "] WARNING: proposeView() is deprecated. Use initiateViewProposal()" << "\n";
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
//               << " (" << verifiedPool.size() << " total)" << "\n";



//     std::string myCarId = "veh" + std::to_string(replicaId);
//     if (!hasProposedOrder && qc.carId == myCarId) {
//         hasProposedOrder = true;
//         triggerOrderConsensus();
//     } else if (hasProposedOrder) {
//         std::cout << "[V2VProxy " << replicaId << "] Already proposed ORDER, ignoring ReadyQC from " << qc.carId << "\n";
        
//     }else{
//         std::cout << "[V2VProxy " << replicaId << "] Received ReadyQC from " << qc.carId   << ", waiting for MY ReadyQC (MY ID: " << myCarId << ")" << "\n";
//     }
// }


void V2VProxyModule::handleReadyQCComplete(BFTMessage* bftMsg) {
    ReadyQC qc = deserializeReadyQC(bftMsg);

    // Epoch + View guard
    // If the sender is in the established VIEW (agreed via BFT), the view guard is
    // the authoritative check. Accept its QC regardless of epoch skew — late-joining
    // cars and staggered resetForNextRound() calls can produce arbitrary epoch gaps.
    // A newer QC (higher epoch) always overwrites an older one in verifiedPool.
    bool senderInView = !establishedView.empty() && establishedView.count(qc.carId) > 0;
    if (senderInView) {
        auto it = verifiedPool.find(qc.carId);
        if (it != verifiedPool.end() && it->second.epoch > qc.epoch) {
            // Already have a fresher QC from this view member — discard the older one
            std::cout << "[READYQC] Replica " << replicaId
                      << " ignoring older QC from VIEW member " << qc.carId
                      << " (stored epoch=" << it->second.epoch
                      << " > incoming epoch=" << qc.epoch << ")" << "\n";
            return;
        }
        if (qc.epoch != currentEpoch) {
            std::cout << "[READYQC] Replica " << replicaId
                      << " accepting off-epoch QC from VIEW member " << qc.carId
                      << " epoch=" << qc.epoch << " (local=" << currentEpoch << ")" << "\n";
        }
    } else {
        // Not in established view — buffer epoch+1, accept epoch, drop everything else
        if (qc.epoch == currentEpoch + 1) {
            nextEpochPool[qc.carId] = qc;
            std::cout << "[READYQC] Replica " << replicaId
                      << " buffering ahead-epoch QC from non-view car " << qc.carId
                      << " epoch=" << qc.epoch << " (local=" << currentEpoch << ")" << "\n";
            return;
        }
        if (qc.epoch != currentEpoch) {
            std::cout << "[READYQC] Replica " << replicaId
                      << " ignoring QC from non-view car " << qc.carId
                      << " epoch=" << qc.epoch << " current=" << currentEpoch << "\n";
            return;
        }
        // epoch == currentEpoch and not in view: fall through to view guard below
    }

    // Secondary view guard for the non-view, same-epoch path
    if (!senderInView && !establishedView.empty()) {
        std::cout << "[READYQC] Replica " << replicaId
                  << " ignoring QC from non-view car " << qc.carId << "\n";
        return;
    }

    // Dedupe / overwrite per car (one QC per car per epoch)
    bool isNew = (verifiedPool.find(qc.carId) == verifiedPool.end());
    verifiedPool[qc.carId] = qc;

    std::cout << "[V2VProxy " << replicaId << "] Received ReadyQC from " << qc.carId
              << " (" << verifiedPool.size() << " total in pool)"
              << (isNew ? " [NEW]" : " [UPDATE]") << "\n";

    std::string myCarId = "veh" + std::to_string(replicaId);

    // Send ACK back to the sender
    if (bftMsg->getFromReplicaId() != replicaId && senderInView) {
        std::vector<uint8_t> emptyPayload;
        sendBFTMessage(replicaId, bftMsg->getFromReplicaId(), emptyPayload, 6); // msgType 6 = READYQC_ACK
    }

    // If self-delivered broadcast arrives and myReadyQCComplete wasn't set yet, set/start
    if (qc.carId == myCarId && !myReadyQCComplete) {
        myReadyQCComplete = true;
        if (isCarAtFrontOfLane(myCarId, qc.laneId)) {
            startOrderCollectionWindowIfNeeded();
        } else {
            std::cout << "[ORDER-COLLECT] Replica " << replicaId 
                      << " is NOT at the front of lane " << qc.laneId 
                      << " - staying quiet." << "\n";
        }
    }

    // New flow: do NOT trigger immediate ORDER consensus here
    // Instead consider early close of collection window
    if (orderCollectionActive && !orderBagProposed && !orderDecisionReceived) {
        int frontLanes = countDistinctFrontLanesInPool();
        std::cout << "[ORDER-COLLECT] Replica " << replicaId
                  << " front-lanes-in-pool=" << frontLanes << "\n";


        int viewLeader = establishedView.empty() ? -1 : getCurrentViewLeader(establishedView);
        bool iAmViewLeader = (viewLeader == replicaId);


        if (frontLanes >= 4 && iAmViewLeader) {
            orderBagCloseFlag = true;
            proposeOrderBagNow("EARLY_4_LANES");
        }
    }
}

void V2VProxyModule::handleReadyQCAck(BFTMessage* bftMsg) {
    if (currentPhase != COLLECTING_QC && currentPhase != ORDER_CONSENSUS) return;
    if (bftMsg->getToReplicaId() != replicaId) return;
    int ackSenderId = bftMsg->getFromReplicaId();
    
    if (readyQCAcks.count(ackSenderId) == 0) {
        std::cout << "[ORDER-GOSSIP] Replica " << replicaId << " received ReadyQC ACK from replica " << ackSenderId << "\n";
        readyQCAcks.insert(ackSenderId);
    }
    
    // Check if we have received ACKs from all active peers in the view
    bool allAcked = true;
    std::string myCarId = "veh" + std::to_string(replicaId);
    for (const auto& peerStr : establishedView) {
        if (peerStr == myCarId) continue;
        int peerId = extractReplicaIdFromCarId(peerStr);
        if (peerId != -1 && readyQCAcks.count(peerId) == 0) {
            allAcked = false;
            break;
        }
    }
    
    if (allAcked) {
        if (orderGossipRetransmitTimer && orderGossipRetransmitTimer->isScheduled()) {
            std::cout << "[ORDER-GOSSIP] Replica " << replicaId << " has received ReadyQC ACKs from all peers. Gossip complete." << "\n";
            cancelEvent(orderGossipRetransmitTimer);
            std::cout << "[ORDER-GOSSIP] Replica " << replicaId << " canceled orderGossipRetransmitTimer." << "\n";
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

        std::cout << "[V2VProxy " << replicaId << "] ===== VEHICLE DEPARTED =====" << "\n";
        std::cout << "[V2VProxy " << replicaId << "] Distance: " << dist << "m" << "\n";
        std::cout << "[V2VProxy " << replicaId << "] Entering ZOMBIE mode (no more V2V)" << "\n";
        std::cout << "[V2VProxy " << replicaId << "] Phase: " << currentPhase << "\n";

        if (moduleIsAmbulance && stopTime > 0) {
            std::cout << "[AMBULANCE_METRICS] veh" << replicaId
                      << " sim_wait_stop_to_departure_sec=" << (simTime() - stopTime).dbl()
                      << " epoch=" << currentEpoch << "\n";
        }

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
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: Cannot notify Java - JVM not available" << "\n";
        return;
    }

    JNIEnv* env;
    sharedJVM->AttachCurrentThread((void**)&env, nullptr);

    jclass serverRunnerClass = env->FindClass("bftsmart/demo/intersection/ServerRunner");
    if (!serverRunnerClass) {
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: ServerRunner class not found" << "\n";
        env->ExceptionDescribe();
        sharedJVM->DetachCurrentThread();
        return;
    }

    jmethodID notifyMethod = env->GetStaticMethodID(serverRunnerClass,
                                                     "notifyVehicleDeparted",
                                                     "(I)V");
    if (!notifyMethod) {
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: notifyVehicleDeparted method not found" << "\n";
        env->ExceptionDescribe();
        sharedJVM->DetachCurrentThread();
        return;
    }

    env->CallStaticVoidMethod(serverRunnerClass, notifyMethod, replicaId);

    // Check for JNI exceptions
    if (env->ExceptionCheck()) {
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: Exception calling notifyVehicleDeparted" << "\n";
        env->ExceptionDescribe();
        env->ExceptionClear();
    } else {
        std::cout << "[V2VProxy " << replicaId << "] Notified Java of departure (zombie mode activated)" << "\n";
    }

    sharedJVM->DetachCurrentThread();
}



void V2VProxyModule::notifyJavaNewBatchSize(int newBatchSize) {
    // Update rate limiter using BFT group size (not VIEW size).
    // Allow at least newBatchSize messages/tick so BFT phases don't stall:
    // ceil(n/rate) ticks × 50ms × 3 phases = BFT latency.
    MAX_MESSAGES_PER_TICK = std::max(newBatchSize, 2);
    std::cout << "[V2VProxy " << replicaId << "] MAX_MESSAGES_PER_TICK updated to " << MAX_MESSAGES_PER_TICK
              << " for bftGroupSize=" << newBatchSize << "\n";

    std::lock_guard<std::mutex> lock(jvmMutex);                                                                                                                                    
                                                                                                                                                                                
    if (!sharedJVM) {                                                                                                                                                              
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: Cannot notify batch size - JVM not available" << "\n";                                                            
        return;                                                                                                                                                                    
    }                                                                                                                                                                              
                                                                                                                                                                                
    JNIEnv* env;                                                                                                                                                                   
    sharedJVM->AttachCurrentThread((void**) &env, nullptr);                                                                                                                        
                                                                                                                                                                                
    jclass serverRunnerClass = env->FindClass("bftsmart/demo/intersection/ServerRunner");                                                                                          
    if (!serverRunnerClass) {                                                                                                                                                      
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: ServerRunner class not found" << "\n";                                                                            
        sharedJVM->DetachCurrentThread();                                                                                                                                          
        return;                                                                                                                                                                    
    }                                                                                                                                                                              
                                                                                                                                                                                
    jmethodID notifyMethod = env->GetStaticMethodID(serverRunnerClass,                                                                                                             
                                                    "notifyBatchSize",                                                                                                            
                                                    "(II)V");  // (replicaId, batchSize)                                                                                          
    if (!notifyMethod) {                                                                                                                                                           
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: notifyBatchSize method not found" << "\n";                                                                        
        sharedJVM->DetachCurrentThread();                                                                                                                                           
        return;                                                                                                                                                                    
    }                                                                                                                                                                              

                                                                                                                                                                                
    env->CallStaticVoidMethod(serverRunnerClass, notifyMethod, replicaId, newBatchSize);                                                                                              
                                                                                                                                                                                
    std::cout << "[V2VProxy " << replicaId << "] Notified Java of batch size: " << newBatchSize << "\n";                                                                         
                                                                                                                                                                                
    sharedJVM->DetachCurrentThread();                                                                                                                                              
 }                                                                                                                                                                                  
                                                   

// ============================================================================
// STAGE 11: EPOCH PREEMPTION
// ============================================================================

void V2VProxyModule::broadcastExecutingMessage(int batchIndex) {
    std::string payload = "veh" + std::to_string(replicaId) + ":"
                        + std::to_string(currentEpoch)  + ":"
                        + std::to_string(batchIndex)    + ":EXECUTING";
    std::vector<uint8_t> data(payload.begin(), payload.end());
    sendBFTMessage(replicaId, -1, data, MSG_EXECUTING);
    intersectionLocked = true;
    std::cout << "[EXECUTING] Replica " << replicaId << " broadcast EXECUTING epoch="
              << currentEpoch << " batch=" << batchIndex << "\n";
}

void V2VProxyModule::handleExecutingMessage(BFTMessage* bftMsg) {
    size_t n = bftMsg->getPayloadArraySize();
    std::string payload;
    payload.reserve(n);
    for (size_t i = 0; i < n; i++) payload += static_cast<char>(bftMsg->getPayload(i));

    std::istringstream ss(payload);
    std::string vehicleId, epochStr, batchStr, tag;
    std::getline(ss, vehicleId, ':');
    std::getline(ss, epochStr,  ':');
    std::getline(ss, batchStr,  ':');
    std::getline(ss, tag,       ':');

    int msgEpoch = -1;
    try { msgEpoch = std::stoi(epochStr); } catch (...) {}

    if (msgEpoch != currentEpoch) {
        std::cout << "[EXECUTING] Replica " << replicaId << " ignoring stale EXECUTING from "
                  << vehicleId << " epoch=" << msgEpoch
                  << " (my epoch=" << currentEpoch << ")\n";
        return;
    }

    intersectionLocked = true;
    std::cout << "[EXECUTING] Replica " << replicaId << " locked intersection (from "
              << vehicleId << " batch=" << batchStr << ")\n";

    checkPreemptionConditions();
}

void V2VProxyModule::checkPreemptionConditions() {
    if (!intersectionLocked) return;

    int waitingCars = 0;
    for (int i = currentBatchIndex + 1; i < (int)pendingBatches.size(); i++) {
        waitingCars += (int)pendingBatches[i].size();
    }

    if (!bufferedNewArrivals.empty()) {
        triggerEpochPreemption("new_arrival:" + bufferedNewArrivals[0]);
        return;
    }

    if (waitingCars > 0 && waitingCars < 4) {
        triggerEpochPreemption("pool_below_threshold:" + std::to_string(waitingCars));
    }
}

void V2VProxyModule::triggerEpochPreemption(const std::string& reason) {
    std::cout << "[PREEMPT] Replica " << replicaId << " epoch preemption: " << reason << "\n";

    std::vector<int> newParticipants;
    std::set<int>    seen;

    for (int i = currentBatchIndex + 1; i < (int)pendingBatches.size(); i++) {
        for (const auto& carId : pendingBatches[i]) {
            int rid = extractReplicaIdFromCarId(carId);
            if (rid >= 0 && seen.insert(rid).second) {
                newParticipants.push_back(rid);
                waitRegistry[carId]++;
            }
        }
    }

    for (const auto& carId : bufferedNewArrivals) {
        int rid = extractReplicaIdFromCarId(carId);
        if (rid >= 0 && seen.insert(rid).second) {
            newParticipants.push_back(rid);
        }
    }
    bufferedNewArrivals.clear();

    if (newParticipants.empty()) {
        std::cout << "[PREEMPT] No participants for new epoch — skipping wipe\n";
        return;
    }
    if ((int)newParticipants.size() < 4) {
        std::cout << "[PREEMPT] Only " << newParticipants.size()
                  << " participants — below BFT threshold, skipping wipe\n";
        return;
    }

    triggerWipeAndReinitViaJNI(newParticipants);
}

bool V2VProxyModule::triggerWipeAndReinitViaJNI(const std::vector<int>& newParticipants) {
    std::lock_guard<std::mutex> lock(jvmMutex);
    if (!sharedJVM) {
        std::cerr << "[PREEMPT] No JVM for wipeAndReinit\n";
        return false;
    }

    JNIEnv* env;
    sharedJVM->AttachCurrentThread((void**)&env, nullptr);

    jclass serverRunnerClass = env->FindClass("bftsmart/demo/intersection/ServerRunner");
    if (!serverRunnerClass) {
        std::cerr << "[PREEMPT] ServerRunner class not found\n";
        sharedJVM->DetachCurrentThread();
        return false;
    }

    jmethodID wipeMethod = env->GetStaticMethodID(serverRunnerClass,
                                                    "wipeAndReinitForReplica",
                                                    "(I[I)V");
    if (!wipeMethod) {
        std::cerr << "[PREEMPT] wipeAndReinitForReplica method not found\n";
        sharedJVM->DetachCurrentThread();
        return false;
    }

    jintArray jParticipants = env->NewIntArray((jsize)newParticipants.size());
    std::vector<jint> jints(newParticipants.begin(), newParticipants.end());
    env->SetIntArrayRegion(jParticipants, 0, (jsize)newParticipants.size(), jints.data());

    env->CallStaticVoidMethod(serverRunnerClass, wipeMethod,
                               (jint)replicaId, jParticipants);

    if (env->ExceptionCheck()) {
        std::cerr << "[PREEMPT] Exception in wipeAndReinitForReplica\n";
        env->ExceptionDescribe();
        env->ExceptionClear();
        sharedJVM->DetachCurrentThread();
        return false;
    }

    std::cout << "[PREEMPT] Replica " << replicaId << " triggered wipeAndReinit for "
              << newParticipants.size() << " participants\n";
    sharedJVM->DetachCurrentThread();
    return true;
}

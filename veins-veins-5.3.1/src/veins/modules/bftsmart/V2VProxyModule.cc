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
#include <limits>
#include <cmath>
#include <cctype>
#include <openssl/evp.h>

#define XXH_INLINE_ALL
#include "xxhash.h"

using namespace veins;

// Shared helper defined in V2VJVMLifecycle.cc
int32_t computeXXHash32(const std::string& str);

static int completedConsensusCount = 0;
static bool logged100m = false;
static std::map<int, int> stopSignFailuresByEpoch;
static std::map<int, std::map<int,int>>    messagesSentByEpochAndReplica;
static std::map<int, std::map<int,int>>    messagesRecvByEpochAndReplica;
static std::set<int> printedMsgAvgEpochs;
// Base64 encoding table

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
    , stopDistance(5.0)
    , intersectionWidth(25.0)
    , avgSpeed(10.0)
    , safetyGap(2.0)
    , waitingForConsensus(false)
    , hasRequestedCrossing(false)
    , isStopped(false)
    , checkPositionTimer(nullptr)
    , consensusTimeoutTimer(nullptr)
    , stopSignTimeoutTimer(nullptr)
    , certCollectionTimeoutTimer(nullptr)
    , shouldFlush(false)
    , consensusTimeoutSec(80.0)  // Default 40 seconds
    , stopSignTimeoutSec(10.0)
    , certCollectionTimeoutSec(1.5)
    , MAX_MESSAGES_PER_TICK(BATCH_SIZE)  // Will be updated by notifyJavaNewBatchSize() when BFT group size is known
    , consensusStartTime(0)
    , orderConsensusStartTime(0)
    , orderConsensusEndTime(0)
    , proposeAllSubmitTime(0)
    , orderDelayTimer(nullptr)
    , orderDecisionReceived(false)
    , delayedOrderSubmitScheduled(false)
    , orderDelayGap(0.0)
    , pendingOrderEpoch(-1)
    , pendingOrderViewHash(0)
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
    cancelAndDelete(certCollectionTimeoutTimer);
    cancelAndDelete(checkJavaReadyTimer);
    cancelAndDelete(retxCheckTimer);   
    cancelAndDelete(orderDelayTimer);
    // Clean up JNI global references
    if (jvm) {
        JNIEnvGuard jniGuard(jvm);
        if (jniGuard.valid()) {
            JNIEnv* env = jniGuard.env;
            if (javaCallbackObject)          { env->DeleteGlobalRef(javaCallbackObject);         javaCallbackObject         = nullptr; }
            if (clockClass)                  { env->DeleteGlobalRef(clockClass);                  clockClass                  = nullptr; }
            if (intersectionServerGlobalClass){ env->DeleteGlobalRef(intersectionServerGlobalClass); intersectionServerGlobalClass = nullptr; }
            if (bftReplicaThread)            { env->DeleteGlobalRef(bftReplicaThread);            bftReplicaThread            = nullptr; }
        }
    }

    if (ambulancePrivateKey) {
        EVP_PKEY_free(static_cast<EVP_PKEY*>(ambulancePrivateKey));
        ambulancePrivateKey = nullptr;
    }
    if (replicaPrivateKey) {
        EVP_PKEY_free(static_cast<EVP_PKEY*>(replicaPrivateKey));
        replicaPrivateKey = nullptr;
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
        totalVehicles_ = par("totalVehicles").intValue();
        intersectionWidth = par("intersectionWidth").doubleValue();
        avgSpeed = par("avgSpeed").doubleValue();
        safetyGap = par("safetyGap").doubleValue();
        consensusTimeoutSec = par("consensusTimeoutSec").doubleValue();
        stopSignTimeoutSec = par("stopSignTimeoutSec").doubleValue();
        orderDelayGap = par("orderDelayGap").doubleValue();
        configuredLeaderReplicaId = par("leaderReplicaId").intValue();
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

        // Per-replica ECDSA P-256 keypair (for witness echoes + self-signed claims).
        // Generated for EVERY replica regardless of role (IEEE 1609.2 model: every
        // V2X station has its own EC key). Embedded compressed pubkey travels with
        // each ARRIVAL_ECHO so any peer can verify without a shared key registry.
        if (replicaPrivateKey) {
            EVP_PKEY_free(static_cast<EVP_PKEY*>(replicaPrivateKey));
            replicaPrivateKey = nullptr;
        }
        replicaPrivateKey = CryptoAuth::instance().generateKeyPair(myReplicaPubKey);
        std::cout << "[CRYPTO] Replica " << replicaId
                  << " generated per-replica ECDSA P-256 keypair (pubKey="
                  << CRYPTO_PUBKEY_BYTES << " bytes)" << "\n";

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
        stopSignTimeoutTimer = new cMessage("stopSignTimeout");
        certCollectionTimeoutTimer = new cMessage("certCollectionTimeout");
        certCollectionTimeoutSec = par("certCollectionTimeoutSec").doubleValue();

        orderDelayTimer = new cMessage("orderDelaySubmit");

        // Create timer for checking Java readiness
        checkJavaReadyTimer = new cMessage("checkJavaReady");
        retxCheckTimer = new cMessage("retransmissionCheck");

        // Early cert discovery: schedule ARRIVAL_ANNOUNCE at spawn_time + triggerJoinTime
        // + per-replica slot. Using simTime() (spawn time) as the base avoids scheduling
        // in the past (vehicles spawn at different SUMO times). The slot stagger prevents
        // simultaneous 802.11p collisions regardless of JVM startup timing.
        triggerJoinTimer = new cMessage("earlyAnnounce");
        double triggerJoinTime = par("triggerJoinTime").doubleValue();
        double announceSlot    = par("arrivalSlotSec").doubleValue();
        scheduleAt(simTime() + triggerJoinTime + replicaId * announceSlot, triggerJoinTimer);

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
        //testing finish

       
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
            for (size_t i = 0; i < toProcess.size(); i++) {
                const auto& pending = toProcess[i];
                std::cout << "[TIMING] EXIT   replica=" << replicaId << " " << pending.fromReplicaId
                          << "->" << pending.toReplicaId << " bytes=" << pending.data.size()
                          << " simT=" << simTime() << "\n";
                // Self-delivery: fromId == toId == replicaId.
                // Schedule via sim-time self-message so the delay is in simulation time,
                // not instant. This matches how RAFT routes all messages (including self)
                // through sendDelayedDown().
                if (pending.fromReplicaId == replicaId && pending.toReplicaId == replicaId) {
                    double delay = replicaId * par("broadcastSlotSec").doubleValue()
                                 + uniform(par("broadcastJitterMin").doubleValue(),
                                           par("broadcastJitterMax").doubleValue());
                    BFTMessage* selfMsg = new BFTMessage("bftSelfDeliver");
                    selfMsg->setFromReplicaId(replicaId);
                    selfMsg->setToReplicaId(replicaId);
                    selfMsg->setMessageType(100); // SELF_DELIVERY sentinel
                    selfMsg->setPayloadArraySize(pending.data.size());
                    for (size_t j = 0; j < pending.data.size(); j++)
                        selfMsg->setPayload(j, pending.data[j]);
                    selfMsg->setPayloadLength(pending.data.size());
                    scheduleAt(simTime() + delay, selfMsg);
                    std::cout << "[V2V-SELF] Replica " << replicaId
                              << ": Scheduled self-delivery at t+" << delay << "s\n";
                } else {
                    sendBFTMessage(pending.fromReplicaId, pending.toReplicaId, pending.data, pending.messageType);
                }
            }
        }

        // Phase 2 is gone in new protocol — Java leader handles ORDER internally.

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
                const bool hasCertCollectionPhase =
                    certCollectionStartTime > 0 &&
                    proposeAllSubmitTime > 0 &&
                    proposeAllSubmitTime >= certCollectionStartTime;
                const bool hasOrderConsensusSim =
                    orderConsensusStartTime > 0 &&
                    orderConsensusEndTime >= orderConsensusStartTime;
                const bool hasStopToDecisionPipeline =
                    consensusStartTime > 0 &&
                    orderConsensusEndTime >= consensusStartTime;

                simtime_t orderConsensusDuration = 0;
                if (hasOrderConsensusSim) {
                    orderConsensusDuration = orderConsensusEndTime - orderConsensusStartTime;
                }

                simtime_t stopToDecisionDuration = 0;
                if (hasStopToDecisionPipeline) {
                    stopToDecisionDuration = orderConsensusEndTime - consensusStartTime;
                }

                // --- Phase timeline (sim time, seconds): V2V collect → VIEW BFT → ORDER prep → ORDER BFT ---
                auto fmtPhaseSec = [](double v) -> std::string {
                    if (v < 0.0 || std::isnan(v)) {
                        return std::string("N/A");
                    }
                    std::ostringstream o;
                    o << std::fixed << std::setprecision(4) << v << "s";
                    return o.str();
                };
                auto fmtSimInstant = [](simtime_t t) -> std::string {
                    if (t <= 0) {
                        return std::string("N/A");
                    }
                    std::ostringstream o;
                    o << t;
                    return o.str();
                };
                const double durCertCollection =
                    hasCertCollectionPhase
                        ? (proposeAllSubmitTime - certCollectionStartTime).dbl()
                        : -1.0;
                const double durStopToDecisionSim =
                    hasStopToDecisionPipeline ? stopToDecisionDuration.dbl() : -1.0;
                const double durProposeAllSim = 
                    (proposeAllSubmitTime > 0 && orderConsensusEndTime >= proposeAllSubmitTime)
                        ? (orderConsensusEndTime - proposeAllSubmitTime).dbl()
                        : -1.0;
                
                std::cout << "\n========== CONSENSUS METRICS (Replica " << replicaId << ") epoch=" << currentEpoch
                          << " ==========" << "\n";
                std::cout << "[PHASE_SUMMARY " << replicaId << "] "
                          << "Cert_Collection=" << fmtPhaseSec(durCertCollection)
                          << " PROPOSE_ALL_BFT(sim)=" << fmtPhaseSec(durProposeAllSim)
                          << " stop_to_decision(sim)=" << fmtPhaseSec(durStopToDecisionSim)
                          << "\n";

                std::cout << "[METRICS " << replicaId << "] === CERT COLLECTION (V2V f+1 ARRIVAL_CERTs) ===" << "\n";
                std::cout << "[METRICS " << replicaId << "] Cert_Collection_Start: " << certCollectionStartTime << "\n";
                std::cout << "[METRICS " << replicaId << "] Cert_Collection_End: " << fmtSimInstant(proposeAllSubmitTime) << "\n";
                std::cout << "[METRICS " << replicaId << "] Cert_Collection_Duration: " << fmtPhaseSec(durCertCollection) << "\n";

                std::cout << "[METRICS " << replicaId << "] === PROPOSE_ALL CONSENSUS (single round BFT) ===" << "\n";
                std::cout << "[METRICS " << replicaId << "] ProposeAll_Submit_Time: " << fmtSimInstant(proposeAllSubmitTime) << "\n";
                std::cout << "[METRICS " << replicaId << "] ProposeAll_Delivery_Time: " << fmtSimInstant(orderConsensusEndTime) << "\n";

                
                std::cout << "[METRICS " << replicaId << "] === PIPELINE (stop → ORDER decision) ===" << "\n";
                std::cout << "[METRICS " << replicaId << "] Stop_To_OrderDecision_Start: " << fmtSimInstant(consensusStartTime) << "\n";
                std::cout << "[METRICS " << replicaId << "] Stop_To_OrderDecision_End:   " << fmtSimInstant(orderConsensusEndTime) << "\n";
                std::cout << "[METRICS " << replicaId << "] Stop_To_OrderDecision_Time: " << fmtPhaseSec(durStopToDecisionSim) << "\n";
                
                // Vehicle timing
                simtime_t scheduledResumeTime = orderConsensusEndTime + delay;
                simtime_t totalWaitTime = scheduledResumeTime - stopTime;
                
                std::cout << "[METRICS " << replicaId << "] Stop_Time: " << stopTime << "\n";
                std::cout << "[METRICS " << replicaId << "] Projected_Total_Wait: " << totalWaitTime.dbl() << " seconds" << "\n";
                std::cout << "[METRICS " << replicaId << "] Scheduled_Resume: " << scheduledResumeTime.dbl() << "\n";
                
                // Message statistics
                std::cout << "[METRICS " << replicaId << "] Messages_Sent: " << sentMessages << "\n";
                std::cout << "[METRICS " << replicaId << "] Messages_Received: " << receivedMessages << "\n";

                // Accumulate per-epoch message metrics
                messagesSentByEpochAndReplica[currentEpoch][replicaId] = (int)sentMessages;
                messagesRecvByEpochAndReplica[currentEpoch][replicaId] = (int)receivedMessages;
                if (printedMsgAvgEpochs.count(currentEpoch) == 0) {
                    auto& sm = messagesSentByEpochAndReplica[currentEpoch];
                    auto& rm = messagesRecvByEpochAndReplica[currentEpoch];
                    if (sm.size() >= 4 && rm.size() >= 4) {
                        double avgSent = 0.0;
                        double avgRecv = 0.0;
                        for (const auto& kv : sm) avgSent += kv.second;
                        for (const auto& kv : rm) avgRecv += kv.second;
                        avgSent /= sm.size();
                        avgRecv /= rm.size();
                        std::cout << "[ROUND-METRICS] Epoch " << currentEpoch
                                  << " Avg_Messages_Sent_PerReplica: " << avgSent
                                  << " (count)" << "\n";
                        std::cout << "[ROUND-METRICS] Epoch " << currentEpoch
                                  << " Avg_Messages_Received_PerReplica: " << avgRecv
                                  << " (count)" << "\n";

                        const int nFail = stopSignFailuresByEpoch.count(currentEpoch) ?
                                          stopSignFailuresByEpoch.at(currentEpoch) : 0;
                        std::cout << "[ROUND-METRICS] Epoch " << currentEpoch
                                  << " Avg_StopSign_Failures: " << static_cast<double>(nFail)
                                  << " (count)" << "\n";
                        printedMsgAvgEpochs.insert(currentEpoch);
                    }
                }

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
                // Stop retransmissions immediately — consensus is done and any still-pending
                // unacked messages are redundant.  The C++ epoch-boundary cleanup will still
                // run later to advance-only sync receivers' expected sequence numbers.
                // Cancelling here (rather than waiting for resumeVehicle) eliminates the
                // retransmit bursts that inflate Messages_Sent and skew delivery_ratio.
                if (retxCheckTimer && retxCheckTimer->isScheduled()) {
                    cancelEvent(retxCheckTimer);
                    std::cout << "[ORDER] Replica " << replicaId
                              << ": cancelled retxCheckTimer (ORDER decided)" << "\n";
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

   if (completedConsensusCount == BATCH_SIZE) {
            std::cout << "[METRICS " << replicaId << "] All Consensus Completed" << "\n";
        }
        
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
        // We must NOT gate on a specific consensus phase because resetForNextRound() or
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
                    enteredStopZone = false;

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
        // RULE 3a: EARLY CERT DISCOVERY (retry path)
        // Primary trigger is triggerJoinTimer (globally staggered). This retry
        // fires only if the JVM wasn't ready at the scheduled slot time.
        // ---------------------------------------------------------------------
        if (javaReady && !joinTriggered && currentPhase == IDLE) {
            laneDiscovered = false;
            discoverLane();
            currentPhase = COLLECTING_CERTS;
            broadcastArrivalAnnouncement();
            certCollectionStartTime = simTime();
            certCollectionStarted = true;
            joinTriggered = true;
            std::cout << "[V2VProxy " << replicaId << "] Early cert discovery started at t=" << simTime()
                      << " (JVM-ready retry)\n";
        }

        // ---------------------------------------------------------------------
        // RULE 3b: STOP ZONE — proposal trigger
        // Effective zone = stopDistance * (totalVehicles / 2). All vehicles stop
        // here; the lane leader proposes immediately if certs are ready.
        // ---------------------------------------------------------------------
        distance = getDistanceToIntersection();

        // Queued followers: record stopTime at first ring entry (matches Raft's distance-threshold
        // primary path — may still be moving). For cars that stop outside the ring, fall back to
        // speed < 0.5 detection. No ORDER-time fallback; unset stopTime stays -1.
        if (currentPhase == COLLECTING_CERTS
                && stopTime < SIMTIME_ZERO
                && !carAhead.empty()) {
            if (distance < stopDistance * (totalVehicles_ / 2.0)) {
                stopTime = simTime();
                std::cout << "[METRICS " << replicaId << "] Arrival_Time: " << stopTime << "\n";
                std::cout << "[METRICS " << replicaId << "] Stop_Time: " << stopTime << "\n";
                std::cout << "[V2VProxy " << replicaId
                          << "] Queued behind lane leader; recording stop from ring entry at t="
                          << stopTime << "\n";
            } else {
                try {
                    if (mobility && mobility->getVehicleCommandInterface()) {
                        const double speed = mobility->getVehicleCommandInterface()->getSpeed();
                        if (speed < 0.5) {
                            stopTime = simTime();
                            std::cout << "[METRICS " << replicaId << "] Arrival_Time: " << stopTime << "\n";
                            std::cout << "[METRICS " << replicaId << "] Stop_Time: " << stopTime << "\n";
                            std::cout << "[V2VProxy " << replicaId
                                      << "] Queued behind lane leader; recording stop from speed poll at t="
                                      << stopTime << "\n";
                        }
                    }
                } catch (...) {
                }
            }
        }

        if (distance < stopDistance * (totalVehicles_ / 2.0)
                && currentPhase == COLLECTING_CERTS && !enteredStopZone) {

            enteredStopZone = true;

            laneDiscovered = false;
            discoverLane();

            std::cout << "[METRICS " << replicaId << "] Arrival_Time: " << simTime() << "\n";
            std::cout << "[V2VProxy " << replicaId << "] ===== ENTERED STOP ZONE =====" << "\n";
            if (stopTime < SIMTIME_ZERO) {
                stopTime = simTime();
            }

            stopVehicle();
            if (stopTime < SIMTIME_ZERO) {
                stopTime = simTime();
            }
            hasRequestedCrossing = true;
            waitingForConsensus = true;
            consensusStartTime = simTime();

            if (!consensusTimeoutTimer->isScheduled())
                scheduleAt(simTime() + consensusTimeoutSec, consensusTimeoutTimer);
            if (carAhead.empty() && !stopSignTimeoutTimer->isScheduled())
                scheduleAt(simTime() + stopSignTimeoutSec, stopSignTimeoutTimer);

            // Lane leader: propose immediately if all certs already collected during travel;
            // otherwise start a short deadline timer for any remaining stragglers.
            if (carAhead.empty() && amITheLeader(physicallyObservedCars) && !proposeAllSubmitted) {
                if (collectedCerts.size() >= (size_t)BATCH_SIZE) {
                    proposeAllSubmitted = true;
                    std::cout << "[V2VProxy " << replicaId << "] All certs ready at stop zone — proposing immediately\n";
                    submitProposeAllToBFT();
                } else if (certCollectionTimeoutTimer && !certCollectionTimeoutTimer->isScheduled()) {
                    scheduleAt(simTime() + certCollectionTimeoutSec, certCollectionTimeoutTimer);
                }
            }
        }

        // Retry a pending VIEW_PROPOSE if Java became ready between attempts
        if (!pendingProposeAllRequest.empty() && javaReady) {
            std::cout << "[V2VProxy " << replicaId << "] Retrying pending PROPOSE_ALL from checkPosition\n";
            if (triggerJoinViaJNI(pendingProposeAllRequest)) {
                pendingProposeAllRequest.clear();
            }
        }

        // Keep the heartbeat alive!
        scheduleAt(simTime() + 0.05, checkPositionTimer);
        return;
    }
    // =========================================================================
    // 4. OTHER TIMERS
    // =========================================================================
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
                if (!pendingProposeAllRequest.empty()) {
                    std::cout << "[V2VProxy " << replicaId << "] Retrying pending PROPOSE_ALL\n";
                    if (triggerJoinViaJNI(pendingProposeAllRequest)) {
                        pendingProposeAllRequest.clear();
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
    if (msg == triggerJoinTimer) {
        // Early cert discovery: broadcast ARRIVAL_ANNOUNCE at the globally-staggered slot.
        // Fires regardless of javaReady so the announce goes out at the scheduled time.
        // The announcement is retried once Java becomes ready if JVM isn't up yet.
        if (!joinTriggered && currentPhase == IDLE) {
            if (javaReady) {
                laneDiscovered = false;
                discoverLane();
                currentPhase = COLLECTING_CERTS;
                broadcastArrivalAnnouncement();
                certCollectionStartTime = simTime();
                certCollectionStarted = true;
                joinTriggered = true;
                std::cout << "[V2VProxy " << replicaId << "] Early cert discovery started at t=" << simTime() << "\n";
            } else {
                // JVM not ready yet — retry on next checkPositionTimer tick
                std::cout << "[V2VProxy " << replicaId << "] Early announce slot at t=" << simTime()
                          << " but JVM not ready — will retry via checkPosition\n";
            }
        }
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
    
    if (msg == certCollectionTimeoutTimer) {
        // Exclusive Fallback: not all ARRIVAL_CERTs received within certCollectionTimeoutSec.
        // Force-submit PROPOSE_ALL with whatever certs exist; uncertified cars become QUIET.
        if (proposeAllSubmitted) {
            return;  // Already reached quorum normally — nothing to do.
        }
        if (zombieFilter()) return;
        if (!amITheLeader(physicallyObservedCars)) return;  // only leader submits

        // The propose must only happen once the leader is in the stop zone — the timer
        // may have been scheduled early (during travel, on first announcement receipt).
        // Reschedule in short increments until stop-zone entry has occurred.
        if (!enteredStopZone) {
            scheduleAt(simTime() + 0.5, certCollectionTimeoutTimer);
            return;
        }

        std::cout << "[V2VProxy " << replicaId << "] *** CERT-COLLECTION TIMEOUT *** after "
                  << certCollectionTimeoutSec << "s — forcing PROPOSE_ALL with QUIET vehicles ("
                  << collectedCerts.size() << "/" << physicallyObservedCars.size() << " certs)\n";

        if (physicallyObservedCars.empty()) {
            std::cout << "[V2VProxy " << replicaId << "] certCollection timeout: no physical observations, nothing to submit\n";
            return;
        }

        proposeAllSubmitted = true;
        std::cout << "[V2VProxy " << replicaId << "] ===== EXCLUSIVE FALLBACK: LEADER FORCE-SUBMITTING TO BFT =====\n";
        submitProposeAllToBFT();
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

    // Self-delivery BFT message: scheduled by processQueueTimer to give self-messages
    // a sim-time delay instead of instant delivery, matching RAFT's radio-path treatment.
    BFTMessage* bftSelf = dynamic_cast<BFTMessage*>(msg);
    if (bftSelf && bftSelf->getMessageType() == 100) {
        size_t n = bftSelf->getPayloadArraySize();
        std::vector<uint8_t> buf(n);
        for (size_t i = 0; i < n; ++i) buf[i] = bftSelf->getPayload(i);
        std::cout << "[V2V-SELF] Replica " << replicaId << ": Self-delivery fired at t=" << simTime()
                  << " (" << n << " bytes)\n";
        deliverMessageToJava(replicaId, buf.data(), (int)buf.size());
        delete bftSelf;
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

void V2VProxyModule::handlePositionUpdate(cObject* obj)
{
    if (replicaId < 0) {  return; }
    DemoBaseApplLayer::handlePositionUpdate(obj);

    if (moduleIsAmbulance && !ambulanceColorSet && mobility && mobility->getVehicleCommandInterface()) {
        mobility->getVehicleCommandInterface()->setColor(TraCIColor(255, 0, 0, 255));
        ambulanceColorSet = true;
    }
    std::cout << "[POSITION UPDATE] Replica " << replicaId << " at time " << simTime() << "\n";
    
    std::cout << "[POSITION UPDATE] Replica " << replicaId << " current phase: " << currentPhase << "Has departed: " << isDeparted << "\n";

    if (!isDeparted && currentPhase == EXECUTING) {
        checkIfDeparted();
    }
    // Position updates are handled via checkPositionTimer for efficiency
}

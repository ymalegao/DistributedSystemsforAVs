//
// V2VEpochPreemption.cc
// Extracted epoch preemption and wipe helpers for V2VProxyModule
//

#include "veins/modules/bftsmart/V2VProxyModule.h"
#include <algorithm>

using namespace veins;

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
    myReceivedEchoes.clear();
    collectedCerts.clear();
    physicallyObservedCars.clear();
    certCollectionStarted = false;
    certBroadcast = false;
    if (certCollectionTimeoutTimer && certCollectionTimeoutTimer->isScheduled())
        cancelEvent(certCollectionTimeoutTimer);
    currentEpoch++;
    currentPhase = IDLE;
    intersectionLocked = false;
    bufferedNewArrivals.clear();
    // Main thread will detect IDLE and trigger fresh ARRIVAL_ANNOUNCE when ready
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
                                                                                                                                                                                
    JNIEnvGuard jniGuard(sharedJVM);
    if (!jniGuard.valid()) {
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: Could not acquire JNIEnv for notifyBatchSize" << "\n";
        return;
    }
    JNIEnv* env = jniGuard.env;

    jclass serverRunnerClass = env->FindClass("bftsmart/demo/intersection/ServerRunner");
    if (!serverRunnerClass) {
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: ServerRunner class not found" << "\n";
        return;
    }

    jmethodID notifyMethod = env->GetStaticMethodID(serverRunnerClass,
                                                    "notifyBatchSize",
                                                    "(II)V");
    if (!notifyMethod) {
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: notifyBatchSize method not found" << "\n";
        return;
    }

    env->CallStaticVoidMethod(serverRunnerClass, notifyMethod, replicaId, newBatchSize);

    std::cout << "[V2VProxy " << replicaId << "] Notified Java of batch size: " << newBatchSize << "\n";
}                                                                                                                                                                                  
                                                   

// ============================================================================
// STAGE 11: EPOCH PREEMPTION
// ============================================================================

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

    JNIEnvGuard jniGuard(sharedJVM);
    if (!jniGuard.valid()) {
        std::cerr << "[PREEMPT] Could not acquire JNIEnv for wipeAndReinit\n";
        return false;
    }
    JNIEnv* env = jniGuard.env;

    jclass serverRunnerClass = env->FindClass("bftsmart/demo/intersection/ServerRunner");
    if (!serverRunnerClass) {
        std::cerr << "[PREEMPT] ServerRunner class not found\n";
        return false;
    }

    jmethodID wipeMethod = env->GetStaticMethodID(serverRunnerClass,
                                                    "wipeAndReinitForReplica",
                                                    "(I[I)V");
    if (!wipeMethod) {
        std::cerr << "[PREEMPT] wipeAndReinitForReplica method not found\n";
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
        return false;
    }

    std::cout << "[PREEMPT] Replica " << replicaId << " triggered wipeAndReinit for "
              << newParticipants.size() << " participants\n";
    return true;
}

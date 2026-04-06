//
// V2VOrderProtocol.cc
// Extracted ORDER decision and execution helpers for V2VProxyModule
//

#include "veins/modules/bftsmart/V2VProxyModule.h"
#include <algorithm>
#include <chrono>
#include <sstream>

using namespace veins;

static std::vector<std::string> split(const std::string& s, char delimiter) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(s);
    while (std::getline(tokenStream, token, delimiter)) {
        tokens.push_back(token);
    }
    return tokens;
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


int V2VProxyModule::extractReplicaIdFromCarId(const std::string& carId) {
    // "veh0" → 0, "veh1" → 1, etc.
    if (carId.substr(0, 3) == "veh") {
        return std::stoi(carId.substr(3));
    }
    return -1;
}



void V2VProxyModule::broadcastExecutingMessage(int batchIndex) {
    std::string payload = "veh" + std::to_string(replicaId) + ":"
                        + std::to_string(currentEpoch)  + ":"
                        + std::to_string(batchIndex)    + ":EXECUTING";
    std::vector<uint8_t> data(payload.begin(), payload.end());
    sendBFTMessage(replicaId, -1, data, EXECUTING);
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

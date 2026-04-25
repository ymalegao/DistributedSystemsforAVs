//
// V2VTraCI.cc
// Extracted TraCI and vehicle control helpers for V2VProxyModule
//

#include "veins/modules/bftsmart/V2VProxyModule.h"
#include <algorithm>
#include <cctype>
#include <limits>

using namespace veins;

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
    orderDecisionReceived = false;
    currentEpoch++;
    lastRoundResetTime = resetNow;
    lastRoundResetEpoch = currentEpoch;
    establishedView.clear();
    localVehicleStates.clear();
    arrivalAnnouncementsReceived.clear();
    pendingBatches.clear();
    currentBatchIndex = 0;
    currentBatchExpected.clear();

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
    proposeAllSubmitted = false;
    pendingProposeAllRequest.clear();
    hasRequestedCrossing = false;
    waitingForConsensus = false;

    // Reset ORDER bag collection state
    alreadyAtStopLine = false;



    //tracking per round not overall
    receivedMessages = 0;
    sentMessages = 0;

   



    // Reset per-car cert collection state
    myReceivedEchoes.clear();
    collectedCerts.clear();
    physicallyObservedCars.clear();
    certCollectionStarted = false;
    certBroadcast = false;
    if (certCollectionTimeoutTimer && certCollectionTimeoutTimer->isScheduled()) {
        cancelEvent(certCollectionTimeoutTimer);
    }
    if (orderDelayTimer && orderDelayTimer->isScheduled()) {
        cancelEvent(orderDelayTimer);
    }
    delayedOrderSubmitScheduled = false;
    pendingOrderPayload.clear();
    pendingOrderEpoch = -1;
    pendingOrderViewHash = 0;

    arrivalAnnouncementsReceived.clear();

    // Clear stale clearance-watch sets from the previous epoch.
    // Without this, checkPositionTimer can fire at the same sim-tick as the reset
    // and see confirmedDeparted == expectedToGo (both filled from the prior epoch),
    // concluding "all departed" instantly and skipping the real clearance wait.
    expectedToGo.clear();
    confirmedDeparted.clear();

    shouldFlush = false;

    // Reset per-round timing metrics so next round starts with clean slate
    orderSignatureCollectionStartTime = 0;
    orderSignatureCollectionEndTime = 0;
    orderCollectionWindowStart = 0;
    orderCollectionWindowEnd = 0;
    firstOrderBagProposalTime = 0;
    firstOrderBagProposerReplica = -1;
    orderConsensusStartTime = 0;
    orderConsensusEndTime = 0;
    proposeAllSubmitTime = 0;
    orderDecisionCallbackSeen = false;
    lastOrderBftRequestRttMs = -1.0;
    lastProposeAllConsensusWallSec = -1.0;
    lastProposeAllConsensusEpoch = -1;
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
        const double departTimeSec = simTime().dbl();
        const double stopTimeSec = (stopTime >= SIMTIME_ZERO) ? stopTime.dbl() : -1.0;
        const double waitStopToDepartureSec =
            (stopTime >= SIMTIME_ZERO) ? (departTimeSec - stopTimeSec) : -1.0;
        const char* vehicleRole = moduleIsAmbulance ? "ambulance" : "normal";

        std::cout << "[V2VProxy " << replicaId << "] ===== VEHICLE DEPARTED =====" << "\n";
        std::cout << "[V2VProxy " << replicaId << "] Distance: " << dist << "m" << "\n";
        std::cout << "[V2VProxy " << replicaId << "] Entering ZOMBIE mode (no more V2V)" << "\n";
        std::cout << "[V2VProxy " << replicaId << "] Phase: " << currentPhase << "\n";
        std::cout << "[CAR-METRICS] veh" << replicaId
                  << " role=" << vehicleRole
                  << " epoch=" << currentEpoch
                  << " stop_time=" << stopTimeSec
                  << " depart_time=" << departTimeSec
                  << " wait_stop_to_departure_sec=" << waitStopToDepartureSec
                  << "\n";

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

    JNIEnvGuard jniGuard(sharedJVM);
    if (!jniGuard.valid()) {
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: Could not acquire JNIEnv for notifyJavaDeparted" << "\n";
        return;
    }
    JNIEnv* env = jniGuard.env;

    jclass serverRunnerClass = env->FindClass("bftsmart/demo/intersection/ServerRunner");
    if (!serverRunnerClass) {
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: ServerRunner class not found" << "\n";
        env->ExceptionDescribe();
        return;
    }

    jmethodID notifyMethod = env->GetStaticMethodID(serverRunnerClass,
                                                     "notifyVehicleDeparted",
                                                     "(I)V");
    if (!notifyMethod) {
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: notifyVehicleDeparted method not found" << "\n";
        env->ExceptionDescribe();
        return;
    }

    env->CallStaticVoidMethod(serverRunnerClass, notifyMethod, replicaId);

    if (env->ExceptionCheck()) {
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: Exception calling notifyVehicleDeparted" << "\n";
        env->ExceptionDescribe();
        env->ExceptionClear();
    } else {
        std::cout << "[V2VProxy " << replicaId << "] Notified Java of departure (zombie mode activated)" << "\n";
    }
}

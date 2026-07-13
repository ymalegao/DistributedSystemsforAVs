//
// ResDBTraCI.cc
// TraCI and vehicle control helpers for ResDBIntersectionApp.
//

#include "veins/modules/application/resDB/ResDBIntersectionApp.h"
#include "veins/modules/mobility/traci/TraCIScenarioManager.h"
#include <algorithm>
#include <cctype>
#include <limits>
#include <cmath>

using namespace veins;

double ResDBIntersectionApp::getDistanceToIntersection()
{
    if (!mobility || !mobility->getCommandInterface()) {
        return 1e10;
    }

    try {
        std::string myId = "veh" + std::to_string(replicaId_);
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

bool ResDBIntersectionApp::isInOrPastConflictBox()
{
    if (is_departed_ || current_phase_ == ConsensusPhase::DEPARTED) return true;
    if (!mobility || !mobility->getCommandInterface()) return true;

    try {
        std::string myId = "veh" + std::to_string(replicaId_);
        TraCICommandInterface::Vehicle v =
            mobility->getCommandInterface()->vehicle(myId);
        std::string laneId = v.getLaneId();
        if (laneId.empty()) return true;
        if (laneId.front() == ':') return true;

        std::string roadId = v.getRoadId();
        return roadId.size() >= 2
            && std::toupper(static_cast<unsigned char>(roadId[0])) == 'C'
            && std::toupper(static_cast<unsigned char>(roadId[1])) == '2';
    } catch (...) {
        return true;
    }
}

int ResDBIntersectionApp::countRollbackPerceivedVehicles() const
{
    if (!mobility || !mobility->getCommandInterface()) return total_vehicles_;
    TraCICommandInterface* traci = mobility->getCommandInterface();
    try {
        int count = 0;
        for (const auto& vid : traci->getVehicleIds()) {
            TraCICommandInterface::Vehicle v = traci->vehicle(vid);
            const std::string laneId = v.getLaneId();
            if (laneId.empty()) continue;
            if (laneId.front() == ':') continue;

            const std::string roadId = v.getRoadId();
            const bool onDepartureLeg = roadId.size() >= 2
                && std::toupper(static_cast<unsigned char>(roadId[0])) == 'C'
                && std::toupper(static_cast<unsigned char>(roadId[1])) == '2';
            if (onDepartureLeg) continue;

            ++count;
        }
        return count > 0 ? count : total_vehicles_;
    } catch (...) {
        return total_vehicles_;
    }
}

bool ResDBIntersectionApp::vehicleHasClearedIntersectionTraCI(const std::string& carId) const
{
    if (!mobility || !mobility->getCommandInterface()) return false;
    TraCICommandInterface* traci = mobility->getCommandInterface();
    try {
        // Pre-check existence before querying position. debug-on-errors=true raises
        // SIGINT before cRuntimeError propagates, bypassing catch blocks entirely —
        // so we must not let the throw happen at all.
        std::list<std::string> active = traci->getVehicleIds();
        if (std::find(active.begin(), active.end(), carId) == active.end()) {
            return true;  // Already left the simulation → treat as cleared
        }

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

bool ResDBIntersectionApp::isApproachingIntersection()
{
    double distance = getDistanceToIntersection();
    return distance < stop_distance_ && distance > 0;
}

void ResDBIntersectionApp::stopVehicle()
{
    if (!is_stopped_ && mobility && mobility->getVehicleCommandInterface()) {
        mobility->getVehicleCommandInterface()->setSpeedMode(31);
        mobility->getVehicleCommandInterface()->setSpeed(-1);  // release override; SUMO holds at red light
        is_stopped_ = true;
        discoverLane();
        std::cout << "[ResDBIntersection " << replicaId_ << "] Vehicle STOPPED at intersection (distance=" << getDistanceToIntersection() << "m)" << "\n";
    } else {
        std::cout << "[ResDBIntersection " << replicaId_ << "] Vehicle already stopped" << "\n";
        is_stopped_ = true;
        discoverLane();
    }
}

void ResDBIntersectionApp::discoverLane() {
    if (lane_discovered_) return;
 
    auto* traci = mobility->getVehicleCommandInterface();
    TraCICommandInterface* traciCmd = mobility->getCommandInterface();
    if (!traci) return;
    std::string myId = "veh" + std::to_string(replicaId_);
    my_lane_id_ = traciCmd->vehicle(myId).getLaneId();
    double mypos = traciCmd->vehicle(myId).getLanePosition();

    // All vehicles on this lane (including self). TraCI lane position increases along the lane
    // toward the intersection on approach edges, so larger position = closer to the junction.
    std::vector<std::pair<double, std::string>> inLane;
    for (const auto& vid : traciCmd->getVehicleIds()) {
        auto v = traciCmd->vehicle(vid);
        if (v.getLaneId() == my_lane_id_) {
            inLane.push_back({v.getLanePosition(), vid});
        }
    }

    // Immediate leader toward the intersection: smallest lane position strictly greater than ours.
    car_ahead_ = "";
    car_ahead_stop_pos = -1.0;
    double bestAbove = std::numeric_limits<double>::infinity();
    for (const auto& [pos, id] : inLane) {
        if (pos > mypos && pos < bestAbove) {
            bestAbove = pos;
            car_ahead_ = id;
            car_ahead_stop_pos = pos;
        }
    }

    // Front of platoon first (highest lane position = rank 1) so scheduling sees physical queue order.
    std::sort(inLane.begin(), inLane.end(),
              [](const std::pair<double, std::string>& a, const std::pair<double, std::string>& b) {
                  return a.first > b.first;
              });

    lane_queue_.clear();
    for (const auto& [pos, id] : inLane) {
        lane_queue_.push_back(id);
    }
    std::cout << "[ResDBIntersection " << replicaId_ << "] Lane queue: ";
    std::cout << "My position: " << mypos << "\n";
    for (const auto& id : lane_queue_) {
        std::cout << id << " ";
    }

    std::cout << "\n";
    lane_discovered_ = true;
    std::cout << "[ResDBIntersection " << replicaId_ << "] Lane discovered: " << my_lane_id_ << "\n";
    std::cout << "[ResDBIntersection " << replicaId_ << "] Car ahead: " << car_ahead_ << "\n";
    std::cout << "[ResDBIntersection " << replicaId_ << "] Car ahead stop pos: " << car_ahead_stop_pos << "\n";

}

bool ResDBIntersectionApp::checkIfDeparted()
{
    if (is_departed_) return true;  // Already departed

    if (!mobility || !traciVehicle) return false;

    // Check if we've moved significantly past the intersection
    double dist = getDistanceToIntersection();

    // Negative distance means we've passed the intersection
    // Or if distance > 100m on the far side (well past intersection)
    if (dist < -15.0 || (current_phase_ == ConsensusPhase::EXECUTING && dist > 15.0)) {
        is_departed_ = true;
        current_phase_ = ConsensusPhase::DEPARTED;
        clearConsensusRetries("departed");
        const double departTimeSec = simTime().dbl();
        const double stopTimeSec = (stopTime >= SIMTIME_ZERO) ? stopTime.dbl() : -1.0;
        const double waitStopToDepartureSec =
            (stopTime >= SIMTIME_ZERO) ? (departTimeSec - stopTimeSec) : -1.0;
        const char* vehicleRole = moduleIsAmbulance ? "ambulance" : "normal";

        std::cout << "[ResDBIntersection " << replicaId_ << "] ===== VEHICLE DEPARTED =====" << "\n";
        std::cout << "[ResDBIntersection " << replicaId_ << "] Distance: " << dist << "m" << "\n";
        std::cout << "[ResDBIntersection " << replicaId_ << "] Entering departed mode (no more V2V)" << "\n";
        std::cout << "[ResDBIntersection " << replicaId_ << "] Phase: " << current_phase_ << "\n";
        std::cout << "[CAR-METRICS] veh" << replicaId_
                  << " role=" << vehicleRole
                  << " epoch=" << current_epoch_
                  << " stop_time=" << stopTimeSec
                  << " depart_time=" << departTimeSec
                  << " wait_stop_to_departure_sec=" << waitStopToDepartureSec
                  << "\n";

        if (moduleIsAmbulance && stopTime > 0) {
            std::cout << "[AMBULANCE_METRICS] veh" << replicaId_
                      << " sim_wait_stop_to_departure_sec=" << (simTime() - stopTime).dbl()
                      << " epoch=" << current_epoch_ << "\n";
        }

        return true;
    }

    return false;
}

void ResDBIntersectionApp::resumeVehicle(int position_in_order)
{
    TraCICommandInterface::Vehicle* vc = mobility ? mobility->getVehicleCommandInterface() : nullptr;
    if (!vc) return;
    is_stopped_ = false;
    std::cout << "[V2VResDB r" << replicaId_ << "] resumeVehicle position=" << position_in_order
              << " speed=" << cruise_speed_mps_ << " t=" << simTime() << "\n";
    vc->setSpeedMode(0);        // re-enable SUMO safety checks (mirrors stopVehicle)

    vc->setSpeed(cruise_speed_mps_);
    if (position_in_order == 0) {
        if (auto* manager = TraCIScenarioManagerAccess().get()) {
            manager->notifyR0BatchStarted("veh" + std::to_string(replicaId_), 0);
        }
    }
}

// ── verifyCarPosition (port of V2VArrivalProtocol::verifyCarPosition) ─────────

ResDBIntersectionApp::VerificationResult
ResDBIntersectionApp::verifyCarPosition(const std::string& carId,
                                         const std::string& claimedLane,
                                         double claimedPosition, double tolerance)
{
    if (!mobility) return {false, "NO_TRACI"};
    TraCICommandInterface* traci = mobility->getCommandInterface();
    if (!traci) return {false, "NO_TRACI"};
    std::list<std::string> ids = traci->getVehicleIds();
    if (std::find(ids.begin(), ids.end(), carId) == ids.end())
        return {false, "NO_VEHICLE"};
    try {
        auto v = traci->vehicle(carId);
        std::string actualLane = v.getLaneId();
        double actualPos       = v.getLanePosition();
        if (actualLane != claimedLane)
            return {false, "WRONG_LANE", actualLane, actualPos};
        if (std::abs(actualPos - claimedPosition) > tolerance)
            return {false, "WRONG_POSITION", actualLane, actualPos};
        return {true, "OK", actualLane, actualPos};
    } catch (...) { return {false, "TRACI_ERROR"}; }
}

// ── extractReplicaId ──────────────────────────────────────────────────────────

int ResDBIntersectionApp::extractReplicaId(const std::string& carId) const
{
    try { return std::stoi(carId.substr(3)); } catch (...) { return -1; }
}

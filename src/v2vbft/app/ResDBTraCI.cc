//
// ResDBTraCI.cc
// TraCI and vehicle control helpers for ResDBIntersectionApp.
//

#include "v2vbft/app/ResDBIntersectionApp.h"
#include "veins/modules/mobility/traci/TraCIScenarioManager.h"
#include <algorithm>
#include <cctype>
#include <limits>
#include <cmath>

using namespace veins;
using namespace v2vbft;

TraCICommandInterface* ResDBIntersectionApp::getTraCI() const
{
    // Vehicles query TraCI through their own TraCIMobility. Static intersection
    // units have no mobility, so they fall back to the global scenario manager's
    // command interface — TraCI is a simulation-wide service, so a unit can still
    // observe/verify any vehicle to witness its arrival.
    if (mobility && mobility->getCommandInterface())
        return mobility->getCommandInterface();
    if (auto* mgr = TraCIScenarioManagerAccess().get())
        return mgr->getCommandInterface();
    return nullptr;
}

double ResDBIntersectionApp::getDistanceToIntersection()
{
    if (!mobility || !mobility->getCommandInterface()) {
        return 1e10;
    }

    try {
        std::string myId = "veh" + std::to_string(ctx_.replicaId_);
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
    if (ctx_.is_departed_ || ctx_.current_phase_ == ConsensusPhase::DEPARTED) return true;
    if (!mobility || !mobility->getCommandInterface()) return true;

    try {
        std::string myId = "veh" + std::to_string(ctx_.replicaId_);
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
    if (!mobility || !mobility->getCommandInterface()) return ctx_.total_vehicles_;
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
        return count > 0 ? count : ctx_.total_vehicles_;
    } catch (...) {
        return ctx_.total_vehicles_;
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

bool ResDBIntersectionApp::vehicleInConflictBoxTraCI(const std::string& carId) const
{
    if (!mobility || !mobility->getCommandInterface()) return false;
    TraCICommandInterface* traci = mobility->getCommandInterface();
    try {
        // Existence precheck first — see vehicleHasClearedIntersectionTraCI for why
        // (debug-on-errors=true would otherwise SIGINT before the catch runs).
        std::list<std::string> active = traci->getVehicleIds();
        if (std::find(active.begin(), active.end(), carId) == active.end()) return false;

        std::string laneId = traci->vehicle(carId).getLaneId();
        return !laneId.empty() && laneId.front() == ':';
    } catch (...) {
        return false;
    }
}

bool ResDBIntersectionApp::anyVehicleInConflictBoxTraCI() const
{
    if (!mobility || !mobility->getCommandInterface()) return false;
    TraCICommandInterface* traci = mobility->getCommandInterface();
    try {
        for (const auto& vid : traci->getVehicleIds()) {
            const std::string laneId = traci->vehicle(vid).getLaneId();
            if (!laneId.empty() && laneId.front() == ':') return true;
        }
        return false;
    } catch (...) {
        // Unknown box occupancy must not be mistaken for "clear".
        return true;
    }
}

double ResDBIntersectionApp::vehicleSpeedTraCI(const std::string& carId) const
{
    if (!mobility || !mobility->getCommandInterface()) return std::numeric_limits<double>::infinity();
    TraCICommandInterface* traci = mobility->getCommandInterface();
    try {
        std::list<std::string> active = traci->getVehicleIds();
        if (std::find(active.begin(), active.end(), carId) == active.end())
            return std::numeric_limits<double>::infinity();
        return traci->vehicle(carId).getSpeed();
    } catch (...) {
        return std::numeric_limits<double>::infinity();
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
        std::cout << "[ResDBIntersection " << ctx_.replicaId_ << "] Vehicle STOPPED at intersection (distance=" << getDistanceToIntersection() << "m)" << "\n";
    } else {
        std::cout << "[ResDBIntersection " << ctx_.replicaId_ << "] Vehicle already stopped" << "\n";
        is_stopped_ = true;
        discoverLane();
    }
}

void ResDBIntersectionApp::discoverLane() {
    if (lane_discovered_) return;
    // Defense-in-depth: units never reach here (handlePositionUpdate/stopVehicle are
    // vehicle-only), but never dereference a null mobility if they somehow do.
    if (!mobility) return;

    auto* traci = mobility->getVehicleCommandInterface();
    TraCICommandInterface* traciCmd = mobility->getCommandInterface();
    if (!traci) return;
    std::string myId = "veh" + std::to_string(ctx_.replicaId_);
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
    std::cout << "[ResDBIntersection " << ctx_.replicaId_ << "] Lane queue: ";
    std::cout << "My position: " << mypos << "\n";
    for (const auto& id : lane_queue_) {
        std::cout << id << " ";
    }

    std::cout << "\n";
    lane_discovered_ = true;
    std::cout << "[ResDBIntersection " << ctx_.replicaId_ << "] Lane discovered: " << my_lane_id_ << "\n";
    std::cout << "[ResDBIntersection " << ctx_.replicaId_ << "] Car ahead: " << car_ahead_ << "\n";
    std::cout << "[ResDBIntersection " << ctx_.replicaId_ << "] Car ahead stop pos: " << car_ahead_stop_pos << "\n";

}

bool ResDBIntersectionApp::checkIfDeparted()
{
    if (ctx_.is_departed_) return true;  // Already departed

    if (!mobility || !traciVehicle) return false;

    // Check if we've moved significantly past the intersection
    double dist = getDistanceToIntersection();

    // Negative distance means we've passed the intersection
    // Or if distance > 100m on the far side (well past intersection)
    if (dist < -15.0 || (ctx_.current_phase_ == ConsensusPhase::EXECUTING && dist > 15.0)) {
        ctx_.is_departed_ = true;
        ctx_.current_phase_ = ConsensusPhase::DEPARTED;
        clearConsensusRetries("departed");
        const double departTimeSec = simTime().dbl();
        const double stopTimeSec = (stopTime >= SIMTIME_ZERO) ? stopTime.dbl() : -1.0;
        const double waitStopToDepartureSec =
            (stopTime >= SIMTIME_ZERO) ? (departTimeSec - stopTimeSec) : -1.0;
        const char* vehicleRole = moduleIsAmbulance ? "ambulance" : "normal";

        std::cout << "[ResDBIntersection " << ctx_.replicaId_ << "] ===== VEHICLE DEPARTED =====" << "\n";
        std::cout << "[ResDBIntersection " << ctx_.replicaId_ << "] Distance: " << dist << "m" << "\n";
        std::cout << "[ResDBIntersection " << ctx_.replicaId_ << "] Entering departed mode (no more V2V)" << "\n";
        std::cout << "[ResDBIntersection " << ctx_.replicaId_ << "] Phase: " << ctx_.current_phase_ << "\n";
        std::cout << "[CAR-METRICS] veh" << ctx_.replicaId_
                  << " role=" << vehicleRole
                  << " epoch=" << ctx_.current_epoch_
                  << " stop_time=" << stopTimeSec
                  << " depart_time=" << departTimeSec
                  << " wait_stop_to_departure_sec=" << waitStopToDepartureSec
                  << "\n";

        if (moduleIsAmbulance && stopTime > 0) {
            std::cout << "[AMBULANCE_METRICS] veh" << ctx_.replicaId_
                      << " sim_wait_stop_to_departure_sec=" << (simTime() - stopTime).dbl()
                      << " epoch=" << ctx_.current_epoch_ << "\n";
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
    std::cout << "[V2VResDB r" << ctx_.replicaId_ << "] resumeVehicle position=" << position_in_order
              << " speed=" << cruise_speed_mps_ << " t=" << simTime() << "\n";
    vc->setSpeedMode(0);        // re-enable SUMO safety checks (mirrors stopVehicle)

    vc->setSpeed(cruise_speed_mps_);
    if (position_in_order == 0) {
        if (auto* manager = TraCIScenarioManagerAccess().get()) {
            manager->notifyR0BatchStarted("veh" + std::to_string(ctx_.replicaId_), 0);
        }
    }
}

// ── verifyCarPosition (port of V2VArrivalProtocol::verifyCarPosition) ─────────

VerificationResult
ResDBIntersectionApp::verifyCarPosition(const std::string& carId,
                                         const std::string& claimedLane,
                                         double claimedPosition, double tolerance)
{
    // Route via getTraCI() so static intersection units (no own mobility) can also
    // witness/verify vehicles through the global manager command interface.
    TraCICommandInterface* traci = getTraCI();
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

// ── Scenario 16: app-side mute only (freeze/tow owned by TraCIScenarioManager) ─

void ResDBIntersectionApp::disableCrashComms(const char* reason)
{
    // Called cross-module from TraCIScenarioManager::freezeCrashWreck(); this
    // method schedules/cancels events owned by *this* module, so we must
    // switch simulation context or scheduleAt()/cancelEvent() below abort
    // with "lacks Enter_Method()".
    Enter_Method_Silent("disableCrashComms");

    if (crashCommsDisabled_) return;
    crashCommsDisabled_ = true;

    {
        std::lock_guard<std::mutex> lk(outbound_mutex_);
        if (!outbound_queue_.empty()) {
            std::cout << "[CRASH-TX-CLEAR] r" << ctx_.replicaId_
                      << " outbound_queue=" << outbound_queue_.size()
                      << " reason=" << (reason ? reason : "crash") << "\n";
            outbound_queue_.clear();
        }
    }
    pending_discovery_txs_.clear();
    if (discovery_tx_flush_timer_ && discovery_tx_flush_timer_->isScheduled())
        cancelEvent(discovery_tx_flush_timer_);

    stopCertBroadcastRetries();
    stopCancelCertRetries();
    stopClearCertRetries();
    cancelClearCertCandidate("crash");
    cancelClearCertRelay("crash");
    clearConsensusRetries(reason ? reason : "crash");

    if (gossip_timer_ && gossip_timer_->isScheduled()) cancelEvent(gossip_timer_);
    if (cancel_gossip_timer_ && cancel_gossip_timer_->isScheduled())
        cancelEvent(cancel_gossip_timer_);
    if (cert_gossip_timer_ && cert_gossip_timer_->isScheduled())
        cancelEvent(cert_gossip_timer_);
    if (broadcastArrivalAnnouncement_timer_ &&
            broadcastArrivalAnnouncement_timer_->isScheduled())
        cancelEvent(broadcastArrivalAnnouncement_timer_);
    if (resume_msg_ && resume_msg_->isScheduled()) cancelEvent(resume_msg_);
    if (preceding_batch_poll_msg_ && preceding_batch_poll_msg_->isScheduled())
        cancelEvent(preceding_batch_poll_msg_);

    if (ctx_.resdb_server_handle_)
        ResdbOmnetSetPbftSilent(ctx_.resdb_server_handle_, 1);

    std::cout << "[CRASH-COMMS-DISABLE] r" << ctx_.replicaId_
              << " reason=" << (reason ? reason : "crash")
              << " t=" << simTime() << "\n";

    if (!crash_mac_grace_msg_)
        crash_mac_grace_msg_ = new cMessage("resdbCrashMacGrace");
    if (crash_mac_grace_msg_->isScheduled()) cancelEvent(crash_mac_grace_msg_);
    scheduleAt(simTime() + crash_mac_grace_sec_, crash_mac_grace_msg_);
}

// ── Turn direction, read from the route SUMO will actually drive ─────────────

namespace {

// Approach edge X2C into exit edge C2Y, as SUMO's own connection dir attribute
// classifies it in both intersection networks. Facing south out of the north
// approach, east is the left turn -- the table encodes that once so no caller
// has to reason about compass geometry.
char turnFor(char from, char to)
{
    switch (from) {
        case 'N': return to == 'S' ? 'S' : to == 'E' ? 'L' : to == 'W' ? 'R' : '?';
        case 'S': return to == 'N' ? 'S' : to == 'W' ? 'L' : to == 'E' ? 'R' : '?';
        case 'E': return to == 'W' ? 'S' : to == 'S' ? 'L' : to == 'N' ? 'R' : '?';
        case 'W': return to == 'E' ? 'S' : to == 'N' ? 'L' : to == 'S' ? 'R' : '?';
        default:  return '?';
    }
}

} // namespace

std::string ResDBIntersectionApp::resolveIntendedDirection() const
{
    if (!mobility || !mobility->getCommandInterface()) return intended_direction_;

    std::string entry, exit;
    try {
        const std::string myId = mobility->getExternalId();
        for (const auto& edge : mobility->getCommandInterface()->vehicle(myId).getPlannedRoadIds()) {
            // Approach edges are "<dir>2C", exit edges "C2<dir>"; internal
            // junction edges start with ':' and are skipped.
            if (edge.size() == 3 && edge[1] == '2') {
                if (edge[2] == 'C' && entry.empty()) entry = edge;
                else if (edge[0] == 'C' && exit.empty()) exit = edge;
            }
        }
    } catch (...) {
        return intended_direction_;   // TraCI does not know this vehicle yet
    }
    if (entry.empty() || exit.empty()) return intended_direction_;

    const char t = turnFor(entry[0], exit[2]);
    return t == '?' ? intended_direction_ : std::string(1, t);
}

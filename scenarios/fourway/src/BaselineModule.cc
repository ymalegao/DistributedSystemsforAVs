#include "veins/modules/application/traci/TraCIDemo11p.h"
#include "veins/modules/mobility/traci/TraCIMobility.h"
#include "veins/base/utils/Coord.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iostream>
#include <list>
#include <regex>
#include <string>

using namespace omnetpp;
using namespace veins;

class BaselineModule : public TraCIDemo11p {
  protected:

    std::string carId;
    bool ambulanceColorSet = false;
    int ambulanceReplicaId = -1;
    int replicaId = -1;
    int epoch = 0;
    double intersectionX = 300.0;
    double intersectionY = 300.0;
    double stopDistance = 50.0;
    double departDistance = 20.0;
    double stoppedSpeed = 0.5;

    bool arrived = false;
    bool stopped = false;
    bool enteredIntersection = false;
    bool departed = false;

    simtime_t arrivalTime = -1;
    simtime_t stopTime = -1;
    simtime_t departTime = -1;

    void initialize(int stage) override;
    void handlePositionUpdate(cObject* obj) override;
    void onWSM(BaseFrame1609_4* frame) override;
    void finish() override;

    int inferReplicaId() const;
    double distanceToIntersection() const;
    bool vehicleHasClearedIntersectionTraCI() const;
    void emitMetrics();
};

Define_Module(BaselineModule);

void BaselineModule::initialize(int stage)
{
    TraCIDemo11p::initialize(stage);
    if (stage == 0) {
        ambulanceReplicaId = par("ambulanceReplicaId");
        carId = par("carId").stringValue();
        intersectionX = par("intersectionX").doubleValue();
        intersectionY = par("intersectionY").doubleValue();
        stopDistance = par("stopDistance").doubleValue();
        departDistance = par("departDistance").doubleValue();
        stoppedSpeed = par("stoppedSpeed").doubleValue();
    }
}

int BaselineModule::inferReplicaId() const
{
    if (!mobility) return -1;
    std::smatch match;
    const std::string externalId = mobility->getExternalId();
    if (std::regex_match(externalId, match, std::regex("^veh([0-9]+)$"))) {
        return std::stoi(match[1]);
    }
    return getParentModule() ? getParentModule()->getIndex() : -1;
}

double BaselineModule::distanceToIntersection() const
{
    if (!mobility || !mobility->getCommandInterface()) return 1e10;
    try {
        const std::string id = carId.empty() ? mobility->getExternalId() : carId;
        auto* traci = mobility->getCommandInterface();
        TraCICommandInterface::Vehicle v = traci->vehicle(id);
        std::string laneId = v.getLaneId();
        if (laneId.empty() || laneId.front() == ':') return 0.0;

        double laneLength = traci->lane(laneId).getLength();
        double currentPos = v.getLanePosition();
        return std::max(0.0, laneLength - currentPos);
    } catch (...) {
        return 1e10;
    }
}

bool BaselineModule::vehicleHasClearedIntersectionTraCI() const
{
    if (!mobility || !mobility->getCommandInterface()) return false;
    TraCICommandInterface* traci = mobility->getCommandInterface();
    try {
        const std::string id = carId.empty() ? mobility->getExternalId() : carId;

        std::list<std::string> active = traci->getVehicleIds();
        if (std::find(active.begin(), active.end(), id) == active.end()) {
            return true;
        }

        TraCICommandInterface::Vehicle v = traci->vehicle(id);
        std::string laneId = v.getLaneId();
        if (laneId.empty()) return false;
        if (laneId.front() == ':') return false;

        std::string roadId = v.getRoadId();
        bool onDepartureLeg = (roadId.size() >= 2
                               && std::toupper(static_cast<unsigned char>(roadId[0])) == 'C'
                               && std::toupper(static_cast<unsigned char>(roadId[1])) == '2');
        if (onDepartureLeg) {
            constexpr double kMinMetersOnDeparture = 3.0;
            return v.getLanePosition() >= kMinMetersOnDeparture;
        }
        return false;
    } catch (...) {
        return true;
    }
}

void BaselineModule::handlePositionUpdate(cObject* obj)
{
    DemoBaseApplLayer::handlePositionUpdate(obj);
    if (!mobility || departed) return;

    if (replicaId < 0) {
        replicaId = inferReplicaId();
        epoch = replicaId >= 0 ? replicaId / 4 : 0;
        if (carId.empty() && replicaId >= 0) {
            carId = "veh" + std::to_string(replicaId);
        }
    }

    if (ambulanceReplicaId >= 0 && replicaId == ambulanceReplicaId && !ambulanceColorSet && mobility->getVehicleCommandInterface()) {
        std::cout << "[AMBULANCE COLOR] r" << replicaId << " setting color to red\n";
        mobility->getVehicleCommandInterface()->setColor(TraCIColor(255, 0, 0, 255));
        ambulanceColorSet = true;
    }

    const double distance = distanceToIntersection();
    const double speed = mobility->getSpeed();

    if (!arrived && distance <= stopDistance) {
        arrived = true;
        arrivalTime = simTime();
        std::cout << "[METRICS " << replicaId << "] Arrival_Time: " << arrivalTime.dbl() << endl;
        std::cout << "[METRICS " << replicaId << "] Messages_Sent: 0" << endl;
        std::cout << "[METRICS " << replicaId << "] Messages_Received: 0" << endl;
    }

    if (arrived && !stopped && speed <= stoppedSpeed) {
        stopped = true;
        stopTime = simTime();
        std::cout << "[METRICS " << replicaId << "] Stop_Time: " << stopTime.dbl() << endl;
    }

    if (arrived && distance <= departDistance) {
        enteredIntersection = true;
        if (!stopped) {
            stopped = true;
            stopTime = arrivalTime;
            std::cout << "[METRICS " << replicaId << "] Stop_Time: " << stopTime.dbl() << endl;
        }
    }

    if (enteredIntersection && vehicleHasClearedIntersectionTraCI()) {
        departed = true;
        departTime = simTime();
        std::cout << "[METRICS " << replicaId << "] Resume_Time: " << departTime.dbl() << endl;
        emitMetrics();
    }
}

void BaselineModule::onWSM(BaseFrame1609_4* frame)
{
    (void) frame;
}

void BaselineModule::emitMetrics()
{
    const bool isAmbulance = replicaId == ambulanceReplicaId;
    const char* role = isAmbulance ? "ambulance" : "normal";
    const double stop = stopTime < SIMTIME_ZERO ? -1.0 : stopTime.dbl();
    const double depart = departTime < SIMTIME_ZERO ? -1.0 : departTime.dbl();
    const double wait = (stop >= 0.0 && depart >= 0.0) ? depart - stop : -1.0;

    std::cout << "[CAR-METRICS] veh" << replicaId
            << " role=" << role
            << " epoch=" << epoch
            << " stop_time=" << stop
            << " depart_time=" << depart
            << " wait_stop_to_departure_sec=" << wait
            << endl;

    if (isAmbulance) {
        std::cout << "[AMBULANCE_METRICS] veh" << replicaId
                << " sim_wait_stop_to_departure_sec=" << wait
                << " epoch=" << epoch
                << endl;
    }
}

void BaselineModule::finish()
{
    if (arrived && !departed) {
        departTime = simTime();
        emitMetrics();
    }
    DemoBaseApplLayer::finish();
}

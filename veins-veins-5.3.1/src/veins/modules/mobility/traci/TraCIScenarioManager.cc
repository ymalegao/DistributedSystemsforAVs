//
// Copyright (C) 2006-2017 Christoph Sommer <sommer@ccs-labs.org>
//
// Documentation for these modules is at http://veins.car2x.org/
//
// SPDX-License-Identifier: GPL-2.0-or-later
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//

#include <fstream>
#include <iostream>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include <iterator>
#include <cstdlib>
#include <cctype>

#include "veins/modules/mobility/traci/TraCIScenarioManager.h"
#include "veins/base/connectionManager/ChannelAccess.h"
#include "veins/modules/mobility/traci/TraCICommandInterface.h"
#include "veins/modules/mobility/traci/TraCIConstants.h"
#include "veins/modules/mobility/traci/TraCIMobility.h"
#include "veins/modules/application/resDB/ResDBIntersectionApp.h"
#include "veins/modules/obstacle/ObstacleControl.h"
#include "veins/modules/world/traci/trafficLight/TraCITrafficLightInterface.h"

using namespace veins::TraCIConstants;

using veins::AnnotationManagerAccess;
using veins::TraCIBuffer;
using veins::TraCICoord;
using veins::TraCIScenarioManager;
using veins::TraCITrafficLightInterface;

Define_Module(veins::TraCIScenarioManager);

const simsignal_t TraCIScenarioManager::traciInitializedSignal = registerSignal("org_car2x_veins_modules_mobility_traciInitialized");
const simsignal_t TraCIScenarioManager::traciModulePreInitSignal = registerSignal("org_car2x_veins_modules_mobility_traciModulePreInit");
const simsignal_t TraCIScenarioManager::traciModuleAddedSignal = registerSignal("org_car2x_veins_modules_mobility_traciModuleAdded");
const simsignal_t TraCIScenarioManager::traciModuleRemovedSignal = registerSignal("org_car2x_veins_modules_mobility_traciModuleRemoved");
const simsignal_t TraCIScenarioManager::traciModuleUpdatedSignal = registerSignal("org_car2x_veins_modules_mobility_traciModuleUpdated");
const simsignal_t TraCIScenarioManager::traciTrafficLightPreInitSignal = registerSignal("org_car2x_veins_modules_mobility_traciTrafficLightPreInit");
const simsignal_t TraCIScenarioManager::traciTrafficLightAddedSignal = registerSignal("org_car2x_veins_modules_mobility_traciTrafficLightAdded");
const simsignal_t TraCIScenarioManager::traciTrafficLightRemovedSignal = registerSignal("org_car2x_veins_modules_mobility_traciTrafficLightRemoved");
const simsignal_t TraCIScenarioManager::traciTrafficLightUpdatedSignal = registerSignal("org_car2x_veins_modules_mobility_traciTrafficLightUpdated");
const simsignal_t TraCIScenarioManager::traciTimestepBeginSignal = registerSignal("org_car2x_veins_modules_mobility_traciTimestepBegin");
const simsignal_t TraCIScenarioManager::traciTimestepEndSignal = registerSignal("org_car2x_veins_modules_mobility_traciTimestepEnd");

TraCIScenarioManager::TraCIScenarioManager()
    : connection(nullptr)
    , commandIfc(nullptr)
    , connectAndStartTrigger(nullptr)
    , executeOneTimestepTrigger(nullptr)
    , r0LateEmergencySpawnTrigger(nullptr)
    , crashSupervisorPollTrigger_(nullptr)
    , world(nullptr)
{
}

TraCIScenarioManager::~TraCIScenarioManager()
{
    if (connection) {
        TraCIBuffer buf = connection->query(CMD_CLOSE, TraCIBuffer());
    }
    if (connectAndStartTrigger) {
        cancelAndDelete(connectAndStartTrigger);
        connectAndStartTrigger = nullptr;
    }
    if (executeOneTimestepTrigger) {
        cancelAndDelete(executeOneTimestepTrigger);
        executeOneTimestepTrigger = nullptr;
    }
    if (r0LateEmergencySpawnTrigger) {
        cancelAndDelete(r0LateEmergencySpawnTrigger);
        r0LateEmergencySpawnTrigger = nullptr;
    }
    if (crashSupervisorPollTrigger_) {
        cancelAndDelete(crashSupervisorPollTrigger_);
        crashSupervisorPollTrigger_ = nullptr;
    }
}

namespace {

std::vector<std::string> getMapping(std::string el)
{

    // search for string protection characters '
    char protection = '\'';
    size_t first = el.find(protection);
    size_t second;
    size_t eq;
    std::string type, value;
    std::vector<std::string> mapping;

    if (first == std::string::npos) {
        // there's no string protection, simply split by '='
        cStringTokenizer stk(el.c_str(), "=");
        mapping = stk.asVector();
    }
    else {
        // if there's string protection, we need to find a matching delimiter
        second = el.find(protection, first + 1);
        // ensure that a matching delimiter exists, and that it is at the end
        if (second == std::string::npos || second != el.size() - 1) throw cRuntimeError("invalid syntax for mapping \"%s\"", el.c_str());

        // take the value of the mapping as the text within the quotes
        value = el.substr(first + 1, second - first - 1);

        if (first == 0) {
            // if the string starts with a quote, there's only the value
            mapping.push_back(value);
        }
        else {
            // search for the equal sign
            eq = el.find('=');
            // this must be the character before the quote
            if (eq == std::string::npos || eq != first - 1) {
                throw cRuntimeError("invalid syntax for mapping \"%s\"", el.c_str());
            }
            else {
                type = el.substr(0, eq);
            }
            mapping.push_back(type);
            mapping.push_back(value);
        }
    }
    return mapping;
}

} // namespace

TraCIScenarioManager::TypeMapping TraCIScenarioManager::parseMappings(std::string parameter, std::string parameterName, bool allowEmpty)
{

    /**
     * possible syntaxes
     *
     * "a"          : assign module type "a" to all nodes (for backward compatibility)
     * "a=b"        : assign module type "b" to vehicle type "a". the presence of any other vehicle type in the simulation will cause the simulation to stop
     * "a=b c=d"    : assign module type "b" to vehicle type "a" and "d" to "c". the presence of any other vehicle type in the simulation will cause the simulation to stop
     * "a=b c=d *=e": everything which is not of vehicle type "a" or "b", assign module type "e"
     * "a=b c=0"    : for vehicle type "c" no module should be instantiated
     * "a=b c=d *=0": everything which is not of vehicle type a or c should not be instantiated
     *
     * For display strings key-value pairs needs to be protected with single quotes, as they use an = sign as the type mappings. For example
     * *.manager.moduleDisplayString = "'i=block/process'"
     * *.manager.moduleDisplayString = "a='i=block/process' b='i=misc/sun'"
     *
     * moduleDisplayString can also be left empty:
     * *.manager.moduleDisplayString = ""
     */

    unsigned int i;
    TypeMapping map;

    // tokenizer to split into mappings ("a=b c=d", -> ["a=b", "c=d"])
    cStringTokenizer typesTz(parameter.c_str(), " ");
    // get all mappings
    std::vector<std::string> typeMappings = typesTz.asVector();
    // and check that there exists at least one
    if (typeMappings.size() == 0) {
        if (!allowEmpty)
            throw cRuntimeError("parameter \"%s\" is empty", parameterName.c_str());
        else
            return map;
    }

    // loop through all mappings
    for (i = 0; i < typeMappings.size(); i++) {

        // tokenizer to find the mapping from vehicle type to module type
        std::string typeMapping = typeMappings[i];

        std::vector<std::string> mapping = getMapping(typeMapping);

        if (mapping.size() == 1) {
            // we are where there is no actual assignment
            // "a": this is good
            // "a b=c": this is not
            if (typeMappings.size() != 1)
                // stop simulation with an error
                throw cRuntimeError("parameter \"%s\" includes multiple mappings, but \"%s\" is not mapped to any vehicle type", parameterName.c_str(), mapping[0].c_str());
            else
                // all vehicle types should be instantiated with this module type
                map["*"] = mapping[0];
        }
        else {

            // check that mapping is valid (a=b and not like a=b=c)
            if (mapping.size() != 2) throw cRuntimeError("invalid syntax for mapping \"%s\" for parameter \"%s\"", typeMapping.c_str(), parameterName.c_str());
            // check that the mapping does not already exist
            if (map.find(mapping[0]) != map.end()) throw cRuntimeError("duplicated mapping for vehicle type \"%s\" for parameter \"%s\"", mapping[0].c_str(), parameterName.c_str());

            // finally save the mapping
            map[mapping[0]] = mapping[1];
        }
    }

    return map;
}

void TraCIScenarioManager::parseModuleTypes()
{

    TypeMapping::iterator i;
    std::vector<std::string> typeKeys, nameKeys, displayStringKeys;

    std::string moduleTypes = par("moduleType").stdstringValue();
    std::string moduleNames = par("moduleName").stdstringValue();
    std::string moduleDisplayStrings = par("moduleDisplayString").stdstringValue();

    moduleType = parseMappings(moduleTypes, "moduleType", false);
    moduleName = parseMappings(moduleNames, "moduleName", false);
    moduleDisplayString = parseMappings(moduleDisplayStrings, "moduleDisplayString", true);

    // perform consistency check. for each vehicle type in moduleType there must be a vehicle type
    // in moduleName (and in moduleDisplayString if moduleDisplayString is not empty)

    // get all the keys
    for (i = moduleType.begin(); i != moduleType.end(); i++) typeKeys.push_back(i->first);
    for (i = moduleName.begin(); i != moduleName.end(); i++) nameKeys.push_back(i->first);
    for (i = moduleDisplayString.begin(); i != moduleDisplayString.end(); i++) displayStringKeys.push_back(i->first);

    // sort them (needed for intersection)
    std::sort(typeKeys.begin(), typeKeys.end());
    std::sort(nameKeys.begin(), nameKeys.end());
    std::sort(displayStringKeys.begin(), displayStringKeys.end());

    std::vector<std::string> intersection;

    // perform set intersection
    std::set_intersection(typeKeys.begin(), typeKeys.end(), nameKeys.begin(), nameKeys.end(), std::back_inserter(intersection));
    if (intersection.size() != typeKeys.size() || intersection.size() != nameKeys.size()) throw cRuntimeError("keys of mappings of moduleType and moduleName are not the same");

    if (displayStringKeys.size() == 0) return;

    intersection.clear();
    std::set_intersection(typeKeys.begin(), typeKeys.end(), displayStringKeys.begin(), displayStringKeys.end(), std::back_inserter(intersection));
    if (intersection.size() != displayStringKeys.size()) throw cRuntimeError("keys of mappings of moduleType and moduleDisplayString are not the same");
}

void TraCIScenarioManager::initialize(int stage)
{
    cSimpleModule::initialize(stage);
    if (stage != 1) {
        return;
    }

    trafficLightModuleType = par("trafficLightModuleType").stdstringValue();
    trafficLightModuleName = par("trafficLightModuleName").stdstringValue();
    trafficLightModuleDisplayString = par("trafficLightModuleDisplayString").stdstringValue();
    trafficLightModuleIds.clear();
    std::istringstream filterstream(par("trafficLightFilter").stdstringValue());
    std::copy(std::istream_iterator<std::string>(filterstream), std::istream_iterator<std::string>(), std::back_inserter(trafficLightModuleIds));

    connectAt = par("connectAt");
    firstStepAt = par("firstStepAt");
    updateInterval = par("updateInterval");
    if (firstStepAt == -1) firstStepAt = connectAt + updateInterval;
    parseModuleTypes();
    penetrationRate = par("penetrationRate").doubleValue();
    ignoreGuiCommands = par("ignoreGuiCommands");
    order = par("order");
    ignoreUnknownSubscriptionResults = par("ignoreUnknownSubscriptionResults");
    host = par("host").stdstringValue();
    port = getPortNumber();
    if (port == -1) {
        throw cRuntimeError("TraCI Port autoconfiguration failed, set 'port' != -1 in omnetpp.ini or provide VEINS_TRACI_PORT environment variable.");
    }
    autoShutdown = par("autoShutdown");
    shutdownOnIntersectionBatchCleared = par("shutdownOnIntersectionBatchCleared").boolValue();
    intersectionBatchSize = par("intersectionBatchSize").intValue();
    if (intersectionBatchSize < 1) {
        throw cRuntimeError("TraCIScenarioManager: intersectionBatchSize must be >= 1");
    }
    intersectionDepartureMinMeters = par("intersectionDepartureMinMeters").doubleValue();
    enableR0Supervisor = par("enableR0Supervisor").boolValue();
    r0SpawnAfterCleared = par("r0SpawnAfterCleared").intValue();
    if (r0SpawnAfterCleared < 1) {
        throw cRuntimeError("TraCIScenarioManager: r0SpawnAfterCleared must be >= 1");
    }
    lateEmergencyDeltaSec = par("lateEmergencyDeltaSec");
    r0LateSpawnDepartPos = par("r0LateSpawnDepartPos").doubleValue();
    r0LateSpawnRetrySec = par("r0LateSpawnRetrySec");
    r0LateSpawnMaxRetries = par("r0LateSpawnMaxRetries").intValue();
    r0LateNormalVehicleId = par("r0LateNormalVehicleId").stdstringValue();
    r0LateNormalType = par("r0LateNormalType").stdstringValue();
    r0LateNormalRoute = par("r0LateNormalRoute").stdstringValue();
    r0LateEmergencyVehicleId = par("r0LateEmergencyVehicleId").stdstringValue();
    r0LateEmergencyType = par("r0LateEmergencyType").stdstringValue();
    r0LateEmergencyRoute = par("r0LateEmergencyRoute").stdstringValue();
    enableCrashSupervisor = par("enableCrashSupervisor").boolValue();
    crashWreckCount = par("crashWreckCount").intValue();
    if (crashWreckCount < 1) {
        throw cRuntimeError("TraCIScenarioManager: crashWreckCount must be >= 1");
    }
    crashPollPeriodSec = par("crashPollPeriodSec");
    crashOnBoxEntrySec = par("crashOnBoxEntrySec");
    clearDelaySec = par("clearDelaySec");

    annotations = AnnotationManagerAccess().getIfExists();

    roi.clear();
    roi.addRoads(par("roiRoads"));
    roi.addRectangles(par("roiRects"));

    areaSum = 0;
    nextNodeVectorIndex = 0;
    hosts.clear();
    subscribedVehicles.clear();
    trafficLights.clear();
    activeVehicleCount = 0;
    parkingVehicleCount = 0;
    drivingVehicleCount = 0;
    hadActiveVehicles = false;
    vehiclesClearedIntersection.clear();
    r0LateSpawnScheduled = false;
    r0LateSpawnDone = false;
    r0LateSpawnRetryCount = 0;
    autoShutdownTriggered = false;
    crashBatch0Members_.clear();
    crashWreckIds_.clear();
    crashInjected_.clear();
    crashTowed_.clear();
    crashConflictOccupantsAtInjection_.clear();
    crashUnsafeEntrants_.clear();
    crashPendingInjectAt_.clear();
    crashTowAt_.clear();
    physicalApproachByVehicle_.clear();
    unsafeConflictPairs_.clear();
    physicalCollisionVehicles_.clear();
    crashSelectDone_ = false;

    world = FindModule<BaseWorldUtility*>::findGlobalModule();

    vehicleObstacleControl = FindModule<VehicleObstacleControl*>::findGlobalModule();

    ASSERT(firstStepAt > connectAt);
    connectAndStartTrigger = new cMessage("connect");
    scheduleAt(connectAt, connectAndStartTrigger);
    executeOneTimestepTrigger = new cMessage("step");
    scheduleAt(firstStepAt, executeOneTimestepTrigger);
    r0LateEmergencySpawnTrigger = new cMessage("r0LateEmergencySpawn");
    crashSupervisorPollTrigger_ = new cMessage("crashSupervisorPoll");

    EV_DEBUG << "initialized TraCIScenarioManager" << endl;
}

void TraCIScenarioManager::init_traci()
{
    auto* commandInterface = getCommandInterface();
    {
        auto apiVersion = commandInterface->getVersion();
        EV_DEBUG << "TraCI server \"" << apiVersion.second << "\" reports API version " << apiVersion.first << endl;
        commandInterface->setApiVersion(apiVersion.first);
        if (order != -1) {
            commandInterface->setOrder(order);
        }
    }

    {
        // query and set road network boundaries
        auto networkBoundaries = commandInterface->initNetworkBoundaries(par("margin"));
        if (world != nullptr && ((connection->traci2omnet(networkBoundaries.second).x > world->getPgs()->x) || (connection->traci2omnet(networkBoundaries.first).y > world->getPgs()->y))) {
            EV_WARN << "WARNING: Playground size (" << world->getPgs()->x << ", " << world->getPgs()->y << ") might be too small for vehicle at network bounds (" << connection->traci2omnet(networkBoundaries.second).x << ", " << connection->traci2omnet(networkBoundaries.first).y << ")" << endl;
        }
    }

    {
        // subscribe to list of departed and arrived vehicles, as well as simulation time
        simtime_t beginTime = 0;
        simtime_t endTime = SimTime::getMaxTime();
        std::string objectId = "";
        std::list<uint8_t> variables;
        variables.push_back(VAR_DEPARTED_VEHICLES_IDS);
        variables.push_back(VAR_ARRIVED_VEHICLES_IDS);
        variables.push_back(commandInterface->getTimeStepCmd());
        if (commandInterface->getApiVersion() >= 18) {
            variables.push_back(VAR_COLLIDING_VEHICLES_IDS);
        }
        variables.push_back(VAR_TELEPORT_STARTING_VEHICLES_IDS);
        variables.push_back(VAR_TELEPORT_ENDING_VEHICLES_IDS);
        variables.push_back(VAR_PARKING_STARTING_VEHICLES_IDS);
        variables.push_back(VAR_PARKING_ENDING_VEHICLES_IDS);
        uint8_t variableNumber = variables.size();
        TraCIBuffer buf1 = TraCIBuffer();
        buf1 << beginTime << endTime << objectId << variableNumber;
        for (auto variable : variables) {
            buf1 << variable;
        }
        TraCIBuffer buf = connection->query(CMD_SUBSCRIBE_SIM_VARIABLE, buf1);

        processSubcriptionResult(buf);
        ASSERT(buf.eof());
    }

    {
        // subscribe to list of vehicle ids
        simtime_t beginTime = 0;
        simtime_t endTime = SimTime::getMaxTime();
        std::string objectId = "";
        uint8_t variableNumber = 1;
        uint8_t variable1 = ID_LIST;
        TraCIBuffer buf = connection->query(CMD_SUBSCRIBE_VEHICLE_VARIABLE, TraCIBuffer() << beginTime << endTime << objectId << variableNumber << variable1);
        processSubcriptionResult(buf);
        ASSERT(buf.eof());
    }

    if (!trafficLightModuleType.empty()) {
        // initialize traffic lights
        cModule* parentmod = getParentModule();
        if (!parentmod) {
            throw cRuntimeError("Parent Module not found (for traffic light creation)");
        }
        cModuleType* tlModuleType = cModuleType::get(trafficLightModuleType.c_str());

        // query traffic lights via TraCI
        std::list<std::string> trafficLightIds = commandInterface->getTrafficlightIds();
#if OMNETPP_BUILDNUM >= 1525
#else
        size_t nrOfTrafficLights = trafficLightIds.size();
#endif
        int cnt = 0;
        for (std::list<std::string>::iterator i = trafficLightIds.begin(); i != trafficLightIds.end(); ++i) {
            std::string tlId = *i;
            if ((!trafficLightModuleIds.empty()) && (std::find(trafficLightModuleIds.begin(), trafficLightModuleIds.end(), tlId) == trafficLightModuleIds.end())) {
                continue; // filter only selected elements
            }

            Coord position = commandInterface->junction(tlId).getPosition();

#if OMNETPP_BUILDNUM >= 1525
            parentmod->setSubmoduleVectorSize(trafficLightModuleName.c_str(), cnt + 1);
            cModule* module = tlModuleType->create(trafficLightModuleName.c_str(), parentmod, cnt);
#else
            cModule* module = tlModuleType->create(trafficLightModuleName.c_str(), parentmod, nrOfTrafficLights, cnt);
#endif
            module->par("externalId") = tlId;
            module->finalizeParameters();
            module->getDisplayString().parse(trafficLightModuleDisplayString.c_str());
            module->buildInside();
            module->scheduleStart(simTime() + updateInterval);

            cModule* tlIfSubmodule = module->getSubmodule("tlInterface");
            // initialize traffic light interface with current program
            TraCITrafficLightInterface* tlIfModule = dynamic_cast<TraCITrafficLightInterface*>(tlIfSubmodule);
            tlIfModule->preInitialize(tlId, position, updateInterval);

            // initialize mobility for positioning
            BaseMobility* mobiSubmodule = check_and_cast<BaseMobility*>(module->getSubmodule("mobility"));
            mobiSubmodule->setStartPosition(position);

            emit(traciTrafficLightPreInitSignal, module);

            module->callInitialize();
            trafficLights[tlId] = module;

            emit(traciTrafficLightAddedSignal, module);

            subscribeToTrafficLightVariables(tlId); // subscribe after module is in trafficLights
            cnt++;
        }
    }

    std::vector<ObstacleControl*> obstaclesModules = FindModule<ObstacleControl*>::findSubModules(getSimulation()->getSystemModule());

    for (ObstacleControl* obstacles : obstaclesModules) {
        if (obstacles) {
            {
                // get list of polygons
                std::list<std::string> ids = commandInterface->getPolygonIds();
                for (std::list<std::string>::iterator i = ids.begin(); i != ids.end(); ++i) {
                    std::string id = *i;
                    std::string typeId = commandInterface->polygon(id).getTypeId();
                    if (!obstacles->isTypeSupported(typeId)) continue;
                    std::list<Coord> coords = commandInterface->polygon(id).getShape();
                    std::vector<Coord> shape;
                    std::copy(coords.begin(), coords.end(), std::back_inserter(shape));
                    for (auto p : shape) {
                        if ((p.x < 0) || (p.y < 0) || (p.x > world->getPgs()->x) || (p.y > world->getPgs()->y)) {
                            EV_WARN << "WARNING: Playground (" << world->getPgs()->x << ", " << world->getPgs()->y << ") will not fit radio obstacle at (" << p.x << ", " << p.y << ")" << endl;
                        }
                    }
                    obstacles->addFromTypeAndShape(id, typeId, shape);
                }
            }
        }
    }

    traciInitialized = true;
    emit(traciInitializedSignal, true);

    // draw and calculate area of rois
    for (std::list<std::pair<TraCICoord, TraCICoord>>::const_iterator r = roi.getRectangles().begin(), end = roi.getRectangles().end(); r != end; ++r) {
        TraCICoord first = r->first;
        TraCICoord second = r->second;

        std::list<Coord> pol;

        Coord a = connection->traci2omnet(first);
        Coord b = connection->traci2omnet(TraCICoord(first.x, second.y));
        Coord c = connection->traci2omnet(second);
        Coord d = connection->traci2omnet(TraCICoord(second.x, first.y));

        pol.push_back(a);
        pol.push_back(b);
        pol.push_back(c);
        pol.push_back(d);

        // draw polygon for region of interest
        if (annotations) {
            annotations->drawPolygon(pol, "black");
        }

        // calculate region area
        double ab = a.distance(b);
        double ad = a.distance(d);
        double area = ab * ad;
        areaSum += area;
    }
}

void TraCIScenarioManager::preNetworkFinish()
{
    while (hosts.begin() != hosts.end()) {
        deleteManagedModule(hosts.begin()->first);
    }
}

void TraCIScenarioManager::finish()
{
    recordScalar("roiArea", areaSum);
}

void TraCIScenarioManager::handleMessage(cMessage* msg)
{
    if (msg->isSelfMessage()) {
        handleSelfMsg(msg);
        return;
    }
    throw cRuntimeError("TraCIScenarioManager doesn't handle messages from other modules");
}

void TraCIScenarioManager::handleSelfMsg(cMessage* msg)
{
    if (msg == connectAndStartTrigger) {
        connection.reset(TraCIConnection::connect(this, host.c_str(), port));
        commandIfc.reset(new TraCICommandInterface(this, *connection, ignoreGuiCommands));
        init_traci();
        return;
    }
    if (msg == executeOneTimestepTrigger) {
        executeOneTimestep();
        return;
    }
    if (msg == r0LateEmergencySpawnTrigger) {
        tryR0LateEmergencySpawn();
        return;
    }
    if (msg == crashSupervisorPollTrigger_) {
        pollCrashSupervisor();
        return;
    }
    throw cRuntimeError("TraCIScenarioManager received unknown self-message");
}

void TraCIScenarioManager::preInitializeModule(cModule* mod, const std::string& nodeId, const Coord& position, const std::string& road_id, double speed, Heading heading, VehicleSignalSet signals)
{
    // pre-initialize TraCIMobility
    auto mobilityModules = getSubmodulesOfType<TraCIMobility>(mod);
    for (auto mm : mobilityModules) {
        mm->preInitialize(nodeId, position, road_id, speed, heading);
    }
}

void TraCIScenarioManager::updateModulePosition(cModule* mod, const Coord& p, const std::string& edge, double speed, Heading heading, VehicleSignalSet signals)
{
    // update position in TraCIMobility
    auto mobilityModules = getSubmodulesOfType<TraCIMobility>(mod);
    for (auto mm : mobilityModules) {
        mm->nextPosition(p, edge, speed, heading, signals);
    }
}

// name: host;Car;i=vehicle.gif
void TraCIScenarioManager::addModule(std::string nodeId, std::string type, std::string name, std::string displayString, const Coord& position, std::string road_id, double speed, Heading heading, VehicleSignalSet signals, double length, double height, double width)
{

    if (hosts.find(nodeId) != hosts.end()) throw cRuntimeError("tried adding duplicate module");

    double option1 = hosts.size() / (hosts.size() + unEquippedHosts.size() + 1.0);
    double option2 = (hosts.size() + 1) / (hosts.size() + unEquippedHosts.size() + 1.0);

    if (fabs(option1 - penetrationRate) < fabs(option2 - penetrationRate)) {
        unEquippedHosts.insert(nodeId);
        return;
    }

    int32_t nodeVectorIndex = nextNodeVectorIndex++;

    cModule* parentmod = getParentModule();
    if (!parentmod) throw cRuntimeError("Parent Module not found");

    cModuleType* nodeType = cModuleType::get(type.c_str());
    if (!nodeType) throw cRuntimeError("Module Type \"%s\" not found", type.c_str());

#if OMNETPP_BUILDNUM >= 1525
    parentmod->setSubmoduleVectorSize(name.c_str(), nodeVectorIndex + 1);
    cModule* mod = nodeType->create(name.c_str(), parentmod, nodeVectorIndex);
#else
    // TODO: this trashes the vectsize member of the cModule, although nobody seems to use it
    cModule* mod = nodeType->create(name.c_str(), parentmod, nodeVectorIndex, nodeVectorIndex);
#endif
    mod->finalizeParameters();
    if (displayString.length() > 0) {
        mod->getDisplayString().parse(displayString.c_str());
    }
    mod->buildInside();
    mod->scheduleStart(simTime() + updateInterval);

    preInitializeModule(mod, nodeId, position, road_id, speed, heading, signals);

    emit(traciModulePreInitSignal, mod);

    mod->callInitialize();
    hosts[nodeId] = mod;

    // post-initialize TraCIMobility
    auto mobilityModules = getSubmodulesOfType<TraCIMobility>(mod);
    for (auto mm : mobilityModules) {
        mm->changePosition();
    }

    if (vehicleObstacleControl) {
        std::vector<AntennaPosition> initialAntennaPositions;
        for (auto& caModule : getSubmodulesOfType<ChannelAccess>(mod, true)) {
            initialAntennaPositions.push_back(caModule->getAntennaPosition());
        }
        ASSERT(mobilityModules.size() == 1);
        auto mm = mobilityModules[0];
        double offset = mm->getHostPositionOffset();
        const MobileHostObstacle* vo = vehicleObstacleControl->add(MobileHostObstacle(initialAntennaPositions, mm, length, offset, width, height));
        vehicleObstacles[mm] = vo;
    }

    emit(traciModuleAddedSignal, mod);
}

cModule* TraCIScenarioManager::getManagedModule(std::string nodeId)
{
    if (hosts.find(nodeId) == hosts.end()) return nullptr;
    return hosts[nodeId];
}

bool TraCIScenarioManager::isModuleUnequipped(std::string nodeId)
{
    if (unEquippedHosts.find(nodeId) == unEquippedHosts.end()) return false;
    return true;
}

void TraCIScenarioManager::deleteManagedModule(std::string nodeId)
{
    cModule* mod = getManagedModule(nodeId);
    if (!mod) throw cRuntimeError("no vehicle with Id \"%s\" found", nodeId.c_str());

    emit(traciModuleRemovedSignal, mod);

    auto cas = getSubmodulesOfType<ChannelAccess>(mod, true);
    for (auto ca : cas) {
        cModule* nic = ca->getParentModule();
        auto connectionManager = ChannelAccess::getConnectionManager(nic);
        connectionManager->unregisterNic(nic);
    }
    if (vehicleObstacleControl) {
        for (cModule::SubmoduleIterator iter(mod); !iter.end(); iter++) {
            cModule* submod = *iter;
            TraCIMobility* mm = dynamic_cast<TraCIMobility*>(submod);
            if (!mm) continue;
            auto vo = vehicleObstacles.find(mm);
            ASSERT(vo != vehicleObstacles.end());
            vehicleObstacleControl->erase(vo->second);
        }
    }

    hosts.erase(nodeId);
    mod->callFinish();
    mod->deleteModule();
}

void TraCIScenarioManager::executeOneTimestep()
{

    EV_DEBUG << "Triggering TraCI server simulation advance to t=" << simTime() << endl;

    simtime_t targetTime = simTime();

    emit(traciTimestepBeginSignal, targetTime);

    if (isConnected()) {
        TraCIBuffer buf = connection->query(CMD_SIMSTEP2, TraCIBuffer() << targetTime);

        uint32_t count;
        buf >> count;
        EV_DEBUG << "Getting " << count << " subscription results" << endl;
        for (uint32_t i = 0; i < count; ++i) {
            processSubcriptionResult(buf);
        }
    }

    emit(traciTimestepEndSignal, targetTime);

    if (!autoShutdownTriggered) scheduleAt(simTime() + updateInterval, executeOneTimestepTrigger);
}

void TraCIScenarioManager::subscribeToVehicleVariables(std::string vehicleId)
{
    // subscribe to some attributes of the vehicle
    simtime_t beginTime = 0;
    simtime_t endTime = SimTime::getMaxTime();
    std::string objectId = vehicleId;
    std::list<uint8_t> variables;
    variables.push_back(VAR_POSITION);
    variables.push_back(VAR_ROAD_ID);
    variables.push_back(VAR_SPEED);
    variables.push_back(VAR_ANGLE);
    variables.push_back(VAR_SIGNALS);
    variables.push_back(VAR_LENGTH);
    variables.push_back(VAR_HEIGHT);
    variables.push_back(VAR_WIDTH);
    uint8_t variableNumber = variables.size();

    TraCIBuffer buf1;
    buf1 << beginTime << endTime << objectId << variableNumber;
    for (auto variable : variables) {
        buf1 << variable;
    }
    TraCIBuffer buf = connection->query(CMD_SUBSCRIBE_VEHICLE_VARIABLE, buf1);
    processSubcriptionResult(buf);
    ASSERT(buf.eof());
}

void TraCIScenarioManager::unsubscribeFromVehicleVariables(std::string vehicleId)
{
    // subscribe to some attributes of the vehicle
    simtime_t beginTime = 0;
    simtime_t endTime = SimTime::getMaxTime();
    std::string objectId = vehicleId;
    uint8_t variableNumber = 0;

    try {
        TraCIBuffer buf = connection->query(CMD_SUBSCRIBE_VEHICLE_VARIABLE, TraCIBuffer() << beginTime << endTime << objectId << variableNumber);
        ASSERT(buf.eof());
    } catch (cRuntimeError& e) {
        // SUMO auto-drops subscriptions when a vehicle is removed/teleported; ignore stale unsub.
        EV_WARN << "Ignoring TraCI unsubscribe for " << vehicleId << ": " << e.what() << endl;
    }
}
void TraCIScenarioManager::subscribeToTrafficLightVariables(std::string tlId)
{
    // subscribe to some attributes of the traffic light system
    simtime_t beginTime = 0;
    simtime_t endTime = SimTime::getMaxTime();
    std::string objectId = tlId;
    uint8_t variableNumber = 4;
    uint8_t variable1 = TL_CURRENT_PHASE;
    uint8_t variable2 = TL_CURRENT_PROGRAM;
    uint8_t variable3 = TL_NEXT_SWITCH;
    uint8_t variable4 = TL_RED_YELLOW_GREEN_STATE;

    TraCIBuffer buf = connection->query(CMD_SUBSCRIBE_TL_VARIABLE, TraCIBuffer() << beginTime << endTime << objectId << variableNumber << variable1 << variable2 << variable3 << variable4);
    processSubcriptionResult(buf);
    ASSERT(buf.eof());
}

void TraCIScenarioManager::unsubscribeFromTrafficLightVariables(std::string tlId)
{
    // unsubscribe from some attributes of the traffic light system
    // this method is mainly for completeness as traffic lights are not supposed to be removed at runtime

    simtime_t beginTime = 0;
    simtime_t endTime = SimTime::getMaxTime();
    std::string objectId = tlId;
    uint8_t variableNumber = 0;

    TraCIBuffer buf = connection->query(CMD_SUBSCRIBE_TL_VARIABLE, TraCIBuffer() << beginTime << endTime << objectId << variableNumber);
    ASSERT(buf.eof());
}

void TraCIScenarioManager::processTrafficLightSubscription(std::string objectId, TraCIBuffer& buf)
{
    cModule* tlIfSubmodule = trafficLights[objectId]->getSubmodule("tlInterface");
    TraCITrafficLightInterface* tlIfModule = dynamic_cast<TraCITrafficLightInterface*>(tlIfSubmodule);
    if (!tlIfModule) {
        throw cRuntimeError("Could not find traffic light module %s", objectId.c_str());
    }

    uint8_t variableNumber_resp;
    buf >> variableNumber_resp;
    for (uint8_t j = 0; j < variableNumber_resp; ++j) {
        uint8_t response_type;
        buf >> response_type;
        uint8_t isokay;
        buf >> isokay;
        if (isokay != RTYPE_OK) {
            std::string description = buf.readTypeChecked<std::string>(TYPE_STRING);
            if (isokay == RTYPE_NOTIMPLEMENTED) {
                throw cRuntimeError("TraCI server reported subscribing to 0x%2x not implemented (\"%s\"). Might need newer version.", response_type, description.c_str());
            }
            else {
                throw cRuntimeError("TraCI server reported error subscribing to variable 0x%2x (\"%s\").", response_type, description.c_str());
            }
        }
        switch (response_type) {
        case TL_CURRENT_PHASE:
            tlIfModule->setCurrentPhaseByNr(buf.readTypeChecked<int32_t>(TYPE_INTEGER), false);
            break;

        case TL_CURRENT_PROGRAM:
            tlIfModule->setCurrentLogicById(buf.readTypeChecked<std::string>(TYPE_STRING), false);
            break;

        case TL_NEXT_SWITCH:
            tlIfModule->setNextSwitch(buf.readTypeChecked<simtime_t>(getCommandInterface()->getTimeType()), false);
            break;

        case TL_RED_YELLOW_GREEN_STATE:
            tlIfModule->setCurrentState(buf.readTypeChecked<std::string>(TYPE_STRING), false);
            break;

        default:
            throw cRuntimeError("Received unhandled traffic light subscription result; type: 0x%02x", response_type);
            break;
        }
    }

    emit(traciTrafficLightUpdatedSignal, trafficLights[objectId]);
}

void TraCIScenarioManager::processSimSubscription(std::string objectId, TraCIBuffer& buf)
{
    uint8_t variableNumber_resp;
    buf >> variableNumber_resp;
    for (uint8_t j = 0; j < variableNumber_resp; ++j) {
        uint8_t variable1_resp;
        buf >> variable1_resp;
        uint8_t isokay;
        buf >> isokay;
        if (isokay != RTYPE_OK) {
            uint8_t varType;
            buf >> varType;
            ASSERT(varType == TYPE_STRING);
            std::string description;
            buf >> description;
            if (isokay == RTYPE_NOTIMPLEMENTED) throw cRuntimeError("TraCI server reported subscribing to variable 0x%2x not implemented (\"%s\"). Might need newer version.", variable1_resp, description.c_str());
            throw cRuntimeError("TraCI server reported error subscribing to variable 0x%2x (\"%s\").", variable1_resp, description.c_str());
        }

        if (variable1_resp == VAR_DEPARTED_VEHICLES_IDS) {
            uint8_t varType;
            buf >> varType;
            ASSERT(varType == TYPE_STRINGLIST);
            uint32_t count;
            buf >> count;
            EV_DEBUG << "TraCI reports " << count << " departed vehicles." << endl;
            for (uint32_t i = 0; i < count; ++i) {
                std::string idstring;
                buf >> idstring;
                // adding modules is handled on the fly when entering/leaving the ROI
            }

            activeVehicleCount += count;
            drivingVehicleCount += count;
            if (count > 0) hadActiveVehicles = true;
        }
        else if (variable1_resp == VAR_ARRIVED_VEHICLES_IDS) {
            uint8_t varType;
            buf >> varType;
            ASSERT(varType == TYPE_STRINGLIST);
            uint32_t count;
            buf >> count;
            EV_DEBUG << "TraCI reports " << count << " arrived vehicles." << endl;
            for (uint32_t i = 0; i < count; ++i) {
                std::string idstring;
                buf >> idstring;

                if (subscribedVehicles.find(idstring) != subscribedVehicles.end()) {
                    subscribedVehicles.erase(idstring);
                    // no unsubscription via TraCI possible/necessary as of SUMO 1.0.0 (the vehicle has arrived)
                }

                // check if this object has been deleted already (e.g. because it was outside the ROI)
                cModule* mod = getManagedModule(idstring);
                if (mod) deleteManagedModule(idstring);

                if (unEquippedHosts.find(idstring) != unEquippedHosts.end()) {
                    unEquippedHosts.erase(idstring);
                }
            }

            activeVehicleCount -= count;
            drivingVehicleCount -= count;
        }
        else if (variable1_resp == VAR_TELEPORT_STARTING_VEHICLES_IDS) {
            uint8_t varType;
            buf >> varType;
            ASSERT(varType == TYPE_STRINGLIST);
            uint32_t count;
            buf >> count;
            EV_DEBUG << "TraCI reports " << count << " vehicles starting to teleport." << endl;
            for (uint32_t i = 0; i < count; ++i) {
                std::string idstring;
                buf >> idstring;

                // check if this object has been deleted already (e.g. because it was outside the ROI)
                cModule* mod = getManagedModule(idstring);
                if (mod) deleteManagedModule(idstring);

                if (unEquippedHosts.find(idstring) != unEquippedHosts.end()) {
                    unEquippedHosts.erase(idstring);
                }
            }

            activeVehicleCount -= count;
            drivingVehicleCount -= count;
        }
        else if (variable1_resp == VAR_TELEPORT_ENDING_VEHICLES_IDS) {
            uint8_t varType;
            buf >> varType;
            ASSERT(varType == TYPE_STRINGLIST);
            uint32_t count;
            buf >> count;
            EV_DEBUG << "TraCI reports " << count << " vehicles ending teleport." << endl;
            for (uint32_t i = 0; i < count; ++i) {
                std::string idstring;
                buf >> idstring;
                // adding modules is handled on the fly when entering/leaving the ROI
            }

            activeVehicleCount += count;
            drivingVehicleCount += count;
            if (count > 0) hadActiveVehicles = true;
        }
        else if (variable1_resp == VAR_PARKING_STARTING_VEHICLES_IDS) {
            uint8_t varType;
            buf >> varType;
            ASSERT(varType == TYPE_STRINGLIST);
            uint32_t count;
            buf >> count;
            EV_DEBUG << "TraCI reports " << count << " vehicles starting to park." << endl;
            for (uint32_t i = 0; i < count; ++i) {
                std::string idstring;
                buf >> idstring;

                cModule* mod = getManagedModule(idstring);
                auto mobilityModules = getSubmodulesOfType<TraCIMobility>(mod);
                for (auto mm : mobilityModules) {
                    mm->changeParkingState(true);
                }
            }

            parkingVehicleCount += count;
            drivingVehicleCount -= count;
        }
        else if (variable1_resp == VAR_PARKING_ENDING_VEHICLES_IDS) {
            uint8_t varType;
            buf >> varType;
            ASSERT(varType == TYPE_STRINGLIST);
            uint32_t count;
            buf >> count;
            EV_DEBUG << "TraCI reports " << count << " vehicles ending to park." << endl;
            for (uint32_t i = 0; i < count; ++i) {
                std::string idstring;
                buf >> idstring;

                cModule* mod = getManagedModule(idstring);
                auto mobilityModules = getSubmodulesOfType<TraCIMobility>(mod);
                for (auto mm : mobilityModules) {
                    mm->changeParkingState(false);
                }
            }
            parkingVehicleCount -= count;
            drivingVehicleCount += count;
        }
        else if (variable1_resp == getCommandInterface()->getTimeStepCmd()) {
            uint8_t varType;
            buf >> varType;
            ASSERT(varType == getCommandInterface()->getTimeType());
            simtime_t serverTimestep;
            buf >> serverTimestep;
            EV_DEBUG << "TraCI reports current time step as " << serverTimestep << " s." << endl;
            simtime_t omnetTimestep = simTime();
            ASSERT(omnetTimestep == serverTimestep);
        }
        else if (variable1_resp == VAR_COLLIDING_VEHICLES_IDS) {
            uint8_t varType;
            buf >> varType;
            ASSERT(varType == TYPE_STRINGLIST);
            uint32_t count;
            buf >> count;
            EV_DEBUG << "TraCI reports " << count << " collided vehicles." << endl;
            for (uint32_t i = 0; i < count; ++i) {
                std::string idstring;
                buf >> idstring;
                if (physicalCollisionVehicles_.insert(idstring).second) {
                    std::cout << "[PHYSICAL-COLLISION] vehicle=" << idstring
                              << " t=" << simTime() << "\n";
                }
                cModule* mod = getManagedModule(idstring);
                if (mod) {
                    auto mobilityModules = getSubmodulesOfType<TraCIMobility>(mod);
                    for (auto mm : mobilityModules) {
                        mm->collisionOccurred(true);
                    }
                }
            }
        }
        else {
            throw cRuntimeError("Received unhandled sim subscription result");
        }
    }

    pollIntersectionCooccupancy();

    // Global, manager-level watchdog:
    // once traffic existed and all vehicles are gone, end the whole simulation.
    if (autoShutdown && hadActiveVehicles && activeVehicleCount == 0 && !autoShutdownTriggered) {
        autoShutdownTriggered = true;
        EV_INFO << "Auto-shutdown: all vehicles departed, ending simulation." << endl;
        endSimulation();
    }
}

void TraCIScenarioManager::pollIntersectionCooccupancy()
{
    if (!commandIfc) return;
    std::vector<std::pair<std::string, char>> occupants;
    for (const auto& id : commandIfc->getVehicleIds()) {
        try {
            auto vehicle = commandIfc->vehicle(id);
            const std::string roadId = vehicle.getRoadId();
            const std::string laneId = vehicle.getLaneId();
            if (!roadId.empty()) {
                const char approach = std::toupper(static_cast<unsigned char>(roadId.front()));
                if ((approach == 'N' || approach == 'S' || approach == 'E' || approach == 'W') &&
                        roadId.size() >= 3 && roadId[1] == '2') {
                    physicalApproachByVehicle_[id] = approach;
                }
            }
            if (!laneId.empty() && laneId.front() == ':') {
                auto it = physicalApproachByVehicle_.find(id);
                if (it != physicalApproachByVehicle_.end()) occupants.push_back(*it);
            }
        } catch (...) {
        }
    }

    auto opposite = [](char a, char b) {
        return (a == 'N' && b == 'S') || (a == 'S' && b == 'N') ||
               (a == 'E' && b == 'W') || (a == 'W' && b == 'E');
    };
    for (size_t i = 0; i < occupants.size(); ++i) {
        for (size_t j = i + 1; j < occupants.size(); ++j) {
            char a = occupants[i].second;
            char b = occupants[j].second;
            if (a == b || opposite(a, b)) continue; // all configured routes are straight
            std::string first = occupants[i].first;
            std::string second = occupants[j].first;
            if (second < first) {
                std::swap(first, second);
                std::swap(a, b);
            }
            if (!unsafeConflictPairs_.insert({first, second}).second) continue;
            std::cout << "[UNSAFE-CONFLICT-COOCCUPANCY] first=" << first
                      << " first_approach=" << a
                      << " second=" << second
                      << " second_approach=" << b
                      << " t=" << simTime() << "\n";
        }
    }
}

bool TraCIScenarioManager::vehiclePastIntersectionDepartureLeg(const std::string& vehicleId)
{
    if (!commandIfc) {
        return false;
    }
    try {
        TraCICommandInterface::Vehicle v = getCommandInterface()->vehicle(vehicleId);
        std::string laneId = v.getLaneId();
        if (laneId.empty()) {
            return false;
        }
        // SUMO internal / junction lanes — still inside the conflict region
        if (laneId.front() == ':') {
            return false;
        }

        std::string roadId = v.getRoadId();
        bool onDepartureLeg = (roadId.size() >= 2 && std::toupper(static_cast<unsigned char>(roadId[0])) == 'C' && std::toupper(static_cast<unsigned char>(roadId[1])) == '2');
        if (onDepartureLeg) {
            bool cleared = v.getLanePosition() >= intersectionDepartureMinMeters;
            if (cleared) {
                notifyIntersectionDeparture(vehicleId);
            }
            return cleared;
        }
        return false;
    } catch (...) {
        // Vehicle left the simulation (aligned with V2VTraCI::vehicleHasClearedIntersectionTraCI)
        return true;
    }
}

void TraCIScenarioManager::notifyIntersectionDeparture(const std::string& vehicleId)
{
    cModule* car = getManagedModule(vehicleId);
    if (!car) return;

    for (auto* app : getSubmodulesOfType<ResDBIntersectionApp>(car, true)) {
        app->recordIntersectionDeparture(simTime());
    }
}

void TraCIScenarioManager::tryShutdownOnIntersectionBatchCleared(const std::string& vehicleId)
{
    if (!vehiclePastIntersectionDepartureLeg(vehicleId)) {
        return;
    }
    auto inserted = vehiclesClearedIntersection.insert(vehicleId);
    if (enableR0Supervisor) {
        maybeScheduleR0LateEmergencySpawn(vehicleId);
    }
    if (!shutdownOnIntersectionBatchCleared || autoShutdownTriggered) {
        return;
    }
    if (inserted.second) {
        tryShutdownOnTerminalVehicleCount(vehicleId, "crossed");
    }
}

void TraCIScenarioManager::tryShutdownOnTerminalVehicleCount(
    const std::string& latestVehicleId, const char* latestOutcome)
{
    if (!shutdownOnIntersectionBatchCleared || autoShutdownTriggered) return;

    // A crash-wait-clear run deliberately vaporizes the wrecks rather than
    // letting them traverse the departure leg.  They are terminal members of
    // the experiment nonetheless.  Requiring intersectionBatchSize physical
    // departures made the completion condition impossible (14 crossings + 2
    // tows in the 16-car scenario), leaving only periodic timers in the FES.
    std::set<std::string> terminalVehicles = vehiclesClearedIntersection;
    if (enableCrashSupervisor) {
        terminalVehicles.insert(crashTowed_.begin(), crashTowed_.end());
    }
    EV_INFO << "Intersection batch: " << terminalVehicles.size() << "/"
            << intersectionBatchSize << " terminal (crossed="
            << vehiclesClearedIntersection.size() << ", towed="
            << (enableCrashSupervisor ? crashTowed_.size() : 0)
            << ", latest=" << latestVehicleId
            << ":" << (latestOutcome ? latestOutcome : "unknown") << ")" << endl;
    if ((int) terminalVehicles.size() >= intersectionBatchSize) {
        autoShutdownTriggered = true;
        EV_INFO << "Auto-shutdown: " << intersectionBatchSize
                << " vehicles reached a terminal intersection outcome (crossed or crash-towed)."
                << endl;
        endSimulation();
    }
}

void TraCIScenarioManager::notifyR0BatchStarted(const std::string& vehicleId, int batchIndex)
{
    if (batchIndex != 0) {
        return;
    }
    if (enableCrashSupervisor) {
        onCrashBatch0Started(vehicleId);
    }
    if (!enableR0Supervisor) {
        return;
    }
    if (r0LateSpawnScheduled || r0LateSpawnDone || autoShutdownTriggered) {
        return;
    }
    r0LateSpawnScheduled = true;
    simtime_t when = simTime() + lateEmergencyDeltaSec;
    if (r0LateEmergencySpawnTrigger->isScheduled()) {
        cancelEvent(r0LateEmergencySpawnTrigger);
    }
    scheduleAt(when, r0LateEmergencySpawnTrigger);
    std::cout << "[R0-SUPERVISOR] scheduled late vehicle injection"
              << " trigger=batch-start"
              << " trigger_vehicle=" << vehicleId
              << " batch=" << batchIndex
              << " at=" << when
              << " delta=" << lateEmergencyDeltaSec
              << "\n";
}

void TraCIScenarioManager::maybeScheduleR0LateEmergencySpawn(const std::string& vehicleId)
{
    if (r0LateSpawnScheduled || r0LateSpawnDone || autoShutdownTriggered) {
        return;
    }
    if ((int) vehiclesClearedIntersection.size() < r0SpawnAfterCleared) {
        return;
    }
    r0LateSpawnScheduled = true;
    simtime_t when = simTime() + lateEmergencyDeltaSec;
    if (r0LateEmergencySpawnTrigger->isScheduled()) {
        cancelEvent(r0LateEmergencySpawnTrigger);
    }
    scheduleAt(when, r0LateEmergencySpawnTrigger);
    std::cout << "[R0-SUPERVISOR] scheduled late vehicle injection"
              << " trigger=clearance"
              << " trigger_vehicle=" << vehicleId
              << " cleared=" << vehiclesClearedIntersection.size()
              << " threshold=" << r0SpawnAfterCleared
              << " at=" << when
              << " delta=" << lateEmergencyDeltaSec
              << "\n";
}

void TraCIScenarioManager::tryR0LateEmergencySpawn()
{
    if (!enableR0Supervisor || r0LateSpawnDone || !commandIfc) {
        return;
    }

    bool normalOk = true;
    bool emergencyOk = true;
    auto vehicleExists = [this](const std::string& vehicleId) {
        std::list<std::string> ids = commandIfc->getVehicleIds();
        return std::find(ids.begin(), ids.end(), vehicleId) != ids.end();
    };
    try {
        normalOk = vehicleExists(r0LateNormalVehicleId) ||
            commandIfc->addVehicle(
                r0LateNormalVehicleId, r0LateNormalType, r0LateNormalRoute,
                simTime(), r0LateSpawnDepartPos, TraCICommandInterface::DEPART_SPEED_MAX,
                TraCICommandInterface::DEPART_LANE_BEST);
        emergencyOk = vehicleExists(r0LateEmergencyVehicleId) ||
            commandIfc->addVehicle(
                r0LateEmergencyVehicleId, r0LateEmergencyType, r0LateEmergencyRoute,
                simTime(), r0LateSpawnDepartPos, TraCICommandInterface::DEPART_SPEED_MAX,
                TraCICommandInterface::DEPART_LANE_BEST);
    } catch (const std::exception& e) {
        normalOk = false;
        emergencyOk = false;
        std::cout << "[R0-SUPERVISOR] late vehicle injection threw: " << e.what() << "\n";
    } catch (...) {
        normalOk = false;
        emergencyOk = false;
        std::cout << "[R0-SUPERVISOR] late vehicle injection threw unknown exception\n";
    }

    if (normalOk && emergencyOk) {
        r0LateSpawnDone = true;
        std::cout << "[R0-SUPERVISOR] injected late vehicles"
                  << " normal=" << r0LateNormalVehicleId
                  << " route=" << r0LateNormalRoute
                  << " emergency=" << r0LateEmergencyVehicleId
                  << " route=" << r0LateEmergencyRoute
                  << " depart_pos=" << r0LateSpawnDepartPos
                  << " t=" << simTime()
                  << "\n";
        return;
    }

    r0LateSpawnRetryCount++;
    std::cout << "[R0-SUPERVISOR] late vehicle injection failed"
              << " normal_ok=" << normalOk
              << " emergency_ok=" << emergencyOk
              << " retry=" << r0LateSpawnRetryCount
              << "/" << r0LateSpawnMaxRetries
              << "\n";
    if (r0LateSpawnMaxRetries <= 0 || r0LateSpawnRetryCount < r0LateSpawnMaxRetries) {
        scheduleAt(simTime() + r0LateSpawnRetrySec, r0LateEmergencySpawnTrigger);
    }
}

void TraCIScenarioManager::onCrashBatch0Started(const std::string& vehicleId)
{
    crashBatch0Members_.insert(vehicleId);
    if (crashSelectDone_) return;
    if ((int) crashBatch0Members_.size() < crashWreckCount) return;

    std::vector<std::string> members(crashBatch0Members_.begin(), crashBatch0Members_.end());
    std::sort(members.begin(), members.end(), [](const std::string& a, const std::string& b) {
        auto idOf = [](const std::string& s) {
            try {
                return (s.size() > 3) ? std::stoi(s.substr(3)) : 0;
            } catch (...) {
                return 0;
            }
        };
        return idOf(a) < idOf(b);
    });
    crashWreckIds_.assign(members.begin(), members.begin() + crashWreckCount);
    crashSelectDone_ = true;

    std::cout << "[CRASH-SELECT] manager batch=0 wrecks=";
    for (size_t i = 0; i < crashWreckIds_.size(); ++i) {
        if (i) std::cout << ",";
        std::cout << crashWreckIds_[i];
    }
    std::cout << " t=" << simTime() << "\n";

    if (crashSupervisorPollTrigger_ && !crashSupervisorPollTrigger_->isScheduled()) {
        scheduleAt(simTime() + crashPollPeriodSec, crashSupervisorPollTrigger_);
    }
}

bool TraCIScenarioManager::vehicleOnInternalConflictLane(const std::string& vehicleId) const
{
    if (!commandIfc) return false;
    try {
        std::list<std::string> ids = commandIfc->getVehicleIds();
        if (std::find(ids.begin(), ids.end(), vehicleId) == ids.end()) return false;
        const std::string laneId = commandIfc->vehicle(vehicleId).getLaneId();
        return !laneId.empty() && laneId.front() == ':';
    } catch (...) {
        return false;
    }
}

void TraCIScenarioManager::freezeCrashWreck(const std::string& vehicleId)
{
    if (!commandIfc || crashInjected_.count(vehicleId)) return;
    try {
        auto v = commandIfc->vehicle(vehicleId);
        const std::string laneId = v.getLaneId();
        const double speed = v.getSpeed();
        // Absolute TraCI hold (speedMode 0). Do not reuse stopVehicle()'s setSpeed(-1).
        v.setSpeedMode(0);
        v.setSpeed(0.0);
        const bool firstCrashInjection = crashInjected_.empty();
        crashInjected_.insert(vehicleId);
        crashTowAt_[vehicleId] = simTime() + clearDelaySec;

        // Snapshot vehicles already sharing the box at injection. They are
        // not evidence that a fabricated recovery ORDER admitted new traffic;
        // only later entrants while a wreck remains are counted below.
        if (firstCrashInjection) {
            for (const auto& id : commandIfc->getVehicleIds()) {
                if (vehicleOnInternalConflictLane(id))
                    crashConflictOccupantsAtInjection_.insert(id);
            }
        }

        std::cout << "[CRASH-INJECT] manager " << vehicleId
                  << " t=" << simTime()
                  << " lane=" << laneId
                  << " speed=" << speed
                  << " tow_at=" << crashTowAt_[vehicleId] << "\n";

        if (cModule* host = getManagedModule(vehicleId)) {
            if (auto* appl = dynamic_cast<ResDBIntersectionApp*>(host->getSubmodule("appl"))) {
                appl->disableCrashComms("crash-inject");
            }
        }
    } catch (const std::exception& e) {
        std::cout << "[CRASH-INJECT-FAIL] manager " << vehicleId
                  << " err=" << e.what() << " t=" << simTime() << "\n";
    } catch (...) {
        std::cout << "[CRASH-INJECT-FAIL] manager " << vehicleId
                  << " err=unknown t=" << simTime() << "\n";
    }
}

void TraCIScenarioManager::towCrashWreck(const std::string& vehicleId)
{
    if (!commandIfc || crashTowed_.count(vehicleId)) return;
    try {
        std::list<std::string> ids = commandIfc->getVehicleIds();
        if (std::find(ids.begin(), ids.end(), vehicleId) == ids.end()) {
            crashTowed_.insert(vehicleId);
            subscribedVehicles.erase(vehicleId);
            std::cout << "[TOW] manager " << vehicleId
                      << " already_absent t=" << simTime() << "\n";
            if (getManagedModule(vehicleId)) deleteManagedModule(vehicleId);
            tryShutdownOnTerminalVehicleCount(vehicleId, "towed");
            return;
        }

        // Unsubscribe while the vehicle still exists. Erasing subscribedVehicles
        // alone (without unsub) leaves SUMO delivering the next 8 variable
        // updates as CMD_GET 0xa4 "Vehicle is not known". Unsub-after-remove
        // instead yields 0xd4 "subscription to remove was not found".
        if (subscribedVehicles.erase(vehicleId) > 0) {
            unsubscribeFromVehicleVariables(vehicleId);
        }
        commandIfc->vehicle(vehicleId).remove(/* REMOVE_VAPORIZED */ 0x03);
        crashTowed_.insert(vehicleId);
        std::cout << "[TOW] manager " << vehicleId << " t=" << simTime() << "\n";

        if (getManagedModule(vehicleId)) {
            deleteManagedModule(vehicleId);
        }
        tryShutdownOnTerminalVehicleCount(vehicleId, "towed");
    } catch (const std::exception& e) {
        std::cout << "[TOW-FAIL] manager " << vehicleId
                  << " err=" << e.what() << " t=" << simTime() << "\n";
    } catch (...) {
        std::cout << "[TOW-FAIL] manager " << vehicleId
                  << " err=unknown t=" << simTime() << "\n";
    }
}

void TraCIScenarioManager::pollCrashSupervisor()
{
    if (!enableCrashSupervisor || !commandIfc) return;

    int activeWrecksInConflict = 0;
    for (const auto& wreckId : crashWreckIds_) {
        if (crashInjected_.count(wreckId) && !crashTowed_.count(wreckId) &&
                vehicleOnInternalConflictLane(wreckId)) {
            ++activeWrecksInConflict;
        }
    }
    if (activeWrecksInConflict > 0) {
        for (const auto& id : commandIfc->getVehicleIds()) {
            if (std::find(crashWreckIds_.begin(), crashWreckIds_.end(), id) !=
                    crashWreckIds_.end()) continue;
            if (crashConflictOccupantsAtInjection_.count(id)) continue;
            if (!vehicleOnInternalConflictLane(id)) continue;
            if (crashUnsafeEntrants_.insert(id).second) {
                std::cout << "[CRASH-COOCCUPANCY] entrant=" << id
                          << " active_wrecks=" << activeWrecksInConflict
                          << " t=" << simTime() << "\n";
            }
        }
    }

    bool workRemaining = false;
    for (const auto& vehicleId : crashWreckIds_) {
        if (crashTowed_.count(vehicleId)) continue;
        workRemaining = true;

        if (!crashInjected_.count(vehicleId)) {
            auto pending = crashPendingInjectAt_.find(vehicleId);
            if (pending != crashPendingInjectAt_.end()) {
                if (simTime() >= pending->second) {
                    freezeCrashWreck(vehicleId);
                    crashPendingInjectAt_.erase(pending);
                }
            } else if (vehicleOnInternalConflictLane(vehicleId)) {
                if (crashOnBoxEntrySec > SIMTIME_ZERO) {
                    crashPendingInjectAt_[vehicleId] = simTime() + crashOnBoxEntrySec;
                    std::cout << "[CRASH-BOX-ENTRY] manager " << vehicleId
                              << " inject_at=" << crashPendingInjectAt_[vehicleId] << "\n";
                } else {
                    freezeCrashWreck(vehicleId);
                }
            }
        } else {
            // Re-assert freeze so SUMO cannot coast the wreck out of the box.
            try {
                auto v = commandIfc->vehicle(vehicleId);
                v.setSpeedMode(0);
                v.setSpeed(0.0);
            } catch (...) {
            }
            auto towIt = crashTowAt_.find(vehicleId);
            if (towIt != crashTowAt_.end() && simTime() >= towIt->second) {
                towCrashWreck(vehicleId);
            }
        }
    }

    if (workRemaining && crashSupervisorPollTrigger_) {
        scheduleAt(simTime() + crashPollPeriodSec, crashSupervisorPollTrigger_);
    }
}

void TraCIScenarioManager::processVehicleSubscription(std::string objectId, TraCIBuffer& buf)
{
    bool isSubscribed = (subscribedVehicles.find(objectId) != subscribedVehicles.end());
    double px;
    double py;
    std::string edge;
    double speed;
    double angle_traci;
    int signals;
    double length;
    double height;
    double width;
    int numRead = 0;

    uint8_t variableNumber_resp;
    buf >> variableNumber_resp;
    for (uint8_t j = 0; j < variableNumber_resp; ++j) {
        uint8_t variable1_resp;
        buf >> variable1_resp;
        uint8_t isokay;
        buf >> isokay;
        if (isokay != RTYPE_OK) {
            uint8_t varType;
            buf >> varType;
            ASSERT(varType == TYPE_STRING);
            std::string errormsg;
            buf >> errormsg;
            if (isSubscribed) {
                if (isokay == RTYPE_NOTIMPLEMENTED) throw cRuntimeError("TraCI server reported subscribing to vehicle variable 0x%2x not implemented (\"%s\"). Might need newer version.", variable1_resp, errormsg.c_str());
                throw cRuntimeError("TraCI server reported error subscribing to vehicle variable 0x%2x (\"%s\").", variable1_resp, errormsg.c_str());
            }
        }
        else if (variable1_resp == ID_LIST) {
            uint8_t varType;
            buf >> varType;
            ASSERT(varType == TYPE_STRINGLIST);
            uint32_t count;
            buf >> count;
            EV_DEBUG << "TraCI reports " << count << " active vehicles." << endl;
            ASSERT(count == activeVehicleCount);
            std::set<std::string> drivingVehicles;
            for (uint32_t i = 0; i < count; ++i) {
                std::string idstring;
                buf >> idstring;
                drivingVehicles.insert(idstring);
            }

            // check for vehicles that need subscribing to
            std::set<std::string> needSubscribe;
            std::set_difference(drivingVehicles.begin(), drivingVehicles.end(), subscribedVehicles.begin(), subscribedVehicles.end(), std::inserter(needSubscribe, needSubscribe.begin()));
            for (std::set<std::string>::const_iterator i = needSubscribe.begin(); i != needSubscribe.end(); ++i) {
                subscribedVehicles.insert(*i);
                subscribeToVehicleVariables(*i);
            }

            // check for vehicles that need unsubscribing from
            std::set<std::string> needUnsubscribe;
            std::set_difference(subscribedVehicles.begin(), subscribedVehicles.end(), drivingVehicles.begin(), drivingVehicles.end(), std::inserter(needUnsubscribe, needUnsubscribe.begin()));
            for (std::set<std::string>::const_iterator i = needUnsubscribe.begin(); i != needUnsubscribe.end(); ++i) {
                subscribedVehicles.erase(*i);
                unsubscribeFromVehicleVariables(*i);
            }
        }
        else if (variable1_resp == VAR_POSITION) {
            uint8_t varType;
            buf >> varType;
            ASSERT(varType == POSITION_2D);
            buf >> px;
            buf >> py;
            numRead++;
        }
        else if (variable1_resp == VAR_ROAD_ID) {
            uint8_t varType;
            buf >> varType;
            ASSERT(varType == TYPE_STRING);
            buf >> edge;
            numRead++;
        }
        else if (variable1_resp == VAR_SPEED) {
            uint8_t varType;
            buf >> varType;
            ASSERT(varType == TYPE_DOUBLE);
            buf >> speed;
            numRead++;
        }
        else if (variable1_resp == VAR_ANGLE) {
            uint8_t varType;
            buf >> varType;
            ASSERT(varType == TYPE_DOUBLE);
            buf >> angle_traci;
            numRead++;
        }
        else if (variable1_resp == VAR_SIGNALS) {
            uint8_t varType;
            buf >> varType;
            ASSERT(varType == TYPE_INTEGER);
            buf >> signals;
            numRead++;
        }
        else if (variable1_resp == VAR_LENGTH) {
            uint8_t varType;
            buf >> varType;
            ASSERT(varType == TYPE_DOUBLE);
            buf >> length;
            numRead++;
        }
        else if (variable1_resp == VAR_HEIGHT) {
            uint8_t varType;
            buf >> varType;
            ASSERT(varType == TYPE_DOUBLE);
            buf >> height;
            numRead++;
        }
        else if (variable1_resp == VAR_WIDTH) {
            uint8_t varType;
            buf >> varType;
            ASSERT(varType == TYPE_DOUBLE);
            buf >> width;
            numRead++;
        }
        else if (ignoreUnknownSubscriptionResults) {
            static bool haveWarned = false;
            uint8_t varType;
            buf >> varType;
            if (!haveWarned) {
                EV_WARN << "Warning: Got a variable that I don't care about (variable " << variable1_resp << ", type " << varType << "). Trying my best to ignore it. This warning will not be repeated." << std::endl;
                haveWarned = true;
            }
            if (varType == TYPE_STRING) {
                std::string foo;
                buf >> foo;
            }
            else if (varType == TYPE_DOUBLE) {
                double foo;
                buf >> foo;
            }
            else if (varType == TYPE_COLOR) {
                TraCIColor res(0, 0, 0, 0);
                buf >> res.red;
                buf >> res.green;
                buf >> res.blue;
                buf >> res.alpha;
            }
            else if (varType == POSITION_3D) {
                double x, y, z;
                buf >> x;
                buf >> y;
                buf >> z;
            }
            else {
                throw cRuntimeError("Received unhandled (and non-ignorable) vehicle subscription result");
            }
        }
        else {
            throw cRuntimeError("Received unhandled vehicle subscription result");
        }
    }

    // bail out if we didn't want to receive these subscription results
    if (!isSubscribed) return;

    // make sure we got updates for all attributes
    if (numRead != 8) return;

    Coord p = connection->traci2omnet(TraCICoord(px, py));
    if ((p.x < 0) || (p.y < 0)) throw cRuntimeError("received bad node position (%.2f, %.2f), translated to (%.2f, %.2f)", px, py, p.x, p.y);

    Heading heading = connection->traci2omnetHeading(angle_traci);

    // Global: same clearance predicate as V2VProxy (C2* + min m); count distinct vehicles, end at intersectionBatchSize.
    tryShutdownOnIntersectionBatchCleared(objectId);

    cModule* mod = getManagedModule(objectId);

    // is it in the ROI?
    bool inRoi = !roi.hasConstraints() ? true : (roi.onAnyRectangle(TraCICoord(px, py)) || roi.partOfRoads(edge));
    if (!inRoi) {
        if (mod) {
            deleteManagedModule(objectId);
            EV_DEBUG << "Vehicle #" << objectId << " left region of interest" << endl;
        }
        else if (unEquippedHosts.find(objectId) != unEquippedHosts.end()) {
            unEquippedHosts.erase(objectId);
            EV_DEBUG << "Vehicle (unequipped) # " << objectId << " left region of interest" << endl;
        }
        return;
    }

    if (isModuleUnequipped(objectId)) {
        return;
    }

    if (!mod) {
        // no such module - need to create
        std::string vType = commandIfc->vehicle(objectId).getTypeId();
        std::string mType, mName, mDisplayString;
        TypeMapping::iterator iType, iName, iDisplayString;

        TypeMapping::iterator i;
        iType = moduleType.find(vType);
        if (iType == moduleType.end()) {
            iType = moduleType.find("*");
            if (iType == moduleType.end()) throw cRuntimeError("cannot find a module type for vehicle type \"%s\"", vType.c_str());
        }
        mType = iType->second;
        // search for module name
        iName = moduleName.find(vType);
        if (iName == moduleName.end()) {
            iName = moduleName.find(std::string("*"));
            if (iName == moduleName.end()) throw cRuntimeError("cannot find a module name for vehicle type \"%s\"", vType.c_str());
        }
        mName = iName->second;
        if (moduleDisplayString.size() != 0) {
            iDisplayString = moduleDisplayString.find(vType);
            if (iDisplayString == moduleDisplayString.end()) {
                iDisplayString = moduleDisplayString.find("*");
                if (iDisplayString == moduleDisplayString.end()) throw cRuntimeError("cannot find a module display string for vehicle type \"%s\"", vType.c_str());
            }
            mDisplayString = iDisplayString->second;
        }
        else {
            mDisplayString = "";
        }

        if (mType != "0") {
            addModule(objectId, mType, mName, mDisplayString, p, edge, speed, heading, VehicleSignalSet(signals), length, height, width);
            EV_DEBUG << "Added vehicle #" << objectId << endl;
        }
    }
    else {
        // module existed - update position
        EV_DEBUG << "module " << objectId << " moving to " << p.x << "," << p.y << endl;
        updateModulePosition(mod, p, edge, speed, heading, VehicleSignalSet(signals));
        emit(traciModuleUpdatedSignal, mod);
    }
}

void TraCIScenarioManager::processSubcriptionResult(TraCIBuffer& buf)
{
    uint8_t cmdLength_resp;
    buf >> cmdLength_resp;
    uint32_t cmdLengthExt_resp;
    buf >> cmdLengthExt_resp;
    uint8_t commandId_resp;
    buf >> commandId_resp;
    std::string objectId_resp;
    buf >> objectId_resp;

    if (commandId_resp == RESPONSE_SUBSCRIBE_VEHICLE_VARIABLE) {
        processVehicleSubscription(objectId_resp, buf);
    }
    else if (commandId_resp == RESPONSE_SUBSCRIBE_SIM_VARIABLE) {
        processSimSubscription(objectId_resp, buf);
    }
    else if (commandId_resp == RESPONSE_SUBSCRIBE_TL_VARIABLE) {
        processTrafficLightSubscription(objectId_resp, buf);
    }
    else {
        throw cRuntimeError("Received unhandled subscription result");
    }
}

int TraCIScenarioManager::getPortNumber() const
{
    int port = par("port");
    if (port != -1) {
        return port;
    }

    // search for externally configured traci port
    const char* env_port = std::getenv("VEINS_TRACI_PORT");
    if (env_port != nullptr) {
        port = std::atoi(env_port);
    }

    return port;
}

void TraCIScenarioManager::lifecycleEvent(SimulationLifecycleEventType eventType, cObject* details)
{
    if (eventType == LF_PRE_NETWORK_FINISH) {
        preNetworkFinish();
    }
}

void TraCIScenarioManager::listenerRemoved()
{
    delete this;
}

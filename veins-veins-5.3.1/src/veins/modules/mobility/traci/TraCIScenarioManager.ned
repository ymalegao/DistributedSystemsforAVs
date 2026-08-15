//
// Copyright (C) 2006-2012 Christoph Sommer <christoph.sommer@uibk.ac.at>
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

package org.car2x.veins.modules.mobility.traci;

//
// Base class for a TraCIScenarioManager.
// To manually launch a TraCI server, run "sumo -c <my.sumo.cfg> --remote-port 9999
//
// See the Veins website <a href="http://veins.car2x.org/"> for a tutorial, documentation, and publications </a>.
//
// @author Christoph Sommer, David Eckhoff
//
// @see TraCIMobility
// @see TraCIScenarioManager
//

simple TraCIScenarioManager
{
    parameters:
        @display("i=block/network2");
        @signal[org_car2x_veins_modules_mobility_traciInitialized](type=bool);
        @signal[org_car2x_veins_modules_mobility_traciModulePreInit](type=cModule);
        @signal[org_car2x_veins_modules_mobility_traciModuleAdded](type=cModule);
        @signal[org_car2x_veins_modules_mobility_traciModuleRemoved](type=cModule);
        @signal[org_car2x_veins_modules_mobility_traciModuleUpdated](type=cModule);
        @signal[org_car2x_veins_modules_mobility_traciTrafficLightPreInit](type=cModule);
        @signal[org_car2x_veins_modules_mobility_traciTrafficLightAdded](type=cModule);
        @signal[org_car2x_veins_modules_mobility_traciTrafficLightRemoved](type=cModule);
        @signal[org_car2x_veins_modules_mobility_traciTrafficLightUpdated](type=cModule);
        @signal[org_car2x_veins_modules_mobility_traciTimestepBegin](type=simtime_t);
        @signal[org_car2x_veins_modules_mobility_traciTimestepEnd](type=simtime_t);
        @class(veins::TraCIScenarioManager);
        double connectAt @unit("s") = default(0s);  // when to connect to TraCI server (must be the initial timestep of the server)
        double firstStepAt @unit("s") = default(-1s);  // when to start synchronizing with the TraCI server (-1: immediately after connecting)
        double updateInterval @unit("s") = default(1s);  // time interval of hosts' position updates
        string moduleType = default("org.car2x.veins.nodes.Car");  // module type to be used in the simulation for each managed vehicle
        string moduleName = default("node");  // module name to be used in the simulation for each managed vehicle
        // module displayString to be used in the simulation for each managed vehicle
        // display strings key-value pairs needs to be protected with single quotes, as they use an = sign as the type mappings. For example
        // <pre>
        // *.manager.moduleDisplayString = "'i=block/process'"
        // *.manager.moduleDisplayString = "a='i=block/process' b='i=misc/sun'"
        // </pre>
        //
        // moduleDisplayString can also be left empty:
        // <pre>
        // *.manager.moduleDisplayString = ""
        // </pre>
        string moduleDisplayString = default("*='i=veins/node/car;is=vs'");
        string trafficLightModuleType = default("");  // module type to be used in the simulation for each managed traffic light
        string trafficLightModuleName = default("tls");  // module name to be used in the simulation for each managed traffic light
        string trafficLightFilter = default("");  // filter string to select which tls shall be subscribed, list sumo IDs separated by spaces
        string trafficLightModuleDisplayString = default("i=veins/node/trafficlight;is=vs");  // module displayString to be used in the simulation for each managed traffic light
        string host = default("localhost");  // server hostname
        int port = default(9999);  // server port (-1: automatic)
        int seed = default(-1); // seed value to set in launch configuration, if missing (-1: current run number)
        bool autoShutdown = default(true);  // Shutdown module as soon as no more vehicles are in the simulation
        bool shutdownOnIntersectionBatchCleared = default(false);  // If true, end simulation globally when intersectionBatchSize distinct vehicles satisfy C2* departure-leg clearance (same idea as V2VTraCI)
        int intersectionBatchSize = default(4);  // Number of vehicles that must clear (TraCI predicate) before endSimulation
        double intersectionDepartureMinMeters @unit("m") = default(3m);  // Min lane position on C2* edge to count as cleared
        bool enableR0Supervisor = default(false);  // R0-only supervisor: inject late emergency pair after initial batch progress
        int r0SpawnAfterCleared = default(1);  // Schedule late injection after this many vehicles satisfy departure-leg clearance
        double lateEmergencyDeltaSec @unit("s") = default(0s);  // Delay from the manager trigger to late vehicle insertion
        double r0LateSpawnDepartPos @unit("m") = default(250m);  // Position on incoming lane for injected vehicles
        double r0LateSpawnRetrySec @unit("s") = default(0.2s);  // Retry interval if SUMO rejects insertion because the spot is blocked
        int r0LateSpawnMaxRetries = default(25);
        string r0LateNormalVehicleId = default("veh16");
        string r0LateNormalType = default("car");
        string r0LateNormalRoute = default("rN");
        string r0LateEmergencyVehicleId = default("veh17");
        string r0LateEmergencyType = default("ambulance");
        string r0LateEmergencyRoute = default("rE");
        // Scenario 16 crash supervisor: freeze two batch-0 vehicles in the conflict box, then tow.
        bool enableCrashSupervisor = default(false);
        int crashWreckCount = default(2);  // lowest replica IDs from notified batch-0 members
        double crashPollPeriodSec @unit("s") = default(0.1s);
        double crashOnBoxEntrySec @unit("s") = default(0s);  // delay after first internal-lane sighting
        double clearDelaySec @unit("s") = default(10s);  // tow delay after freeze
        int margin = default(25);  // margin to add to all received vehicle positions
        string roiRoads = default("");  // which roads (e.g. "hwy1 hwy2") are considered to consitute the region of interest, if not empty
        string roiRects = default("");  // which rectangles (e.g. "0,0-10,10 20,20-30,30) are considered to consitute the region of interest, if not empty. Note that these rectangles have to use TraCI (SUMO) coordinates and not OMNeT++. They can be easily read from sumo-gui.
        double penetrationRate = default(1); //the probability of a vehicle being equipped with Car2X technology
        bool ignoreGuiCommands = default(false); // whether to ignore all TraCI commands that only make sense when the server has a graphical user interface
        int order = default(-1); // specific position in the multi-client execution order of the TraCI server to request upon connecting (-1: do not request a position)
        bool ignoreUnknownSubscriptionResults = default(false); // whether to (try and) ignore any subscription result we did not request (but another client might have)
}

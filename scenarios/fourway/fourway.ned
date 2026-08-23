// Use org.car2x.veins prefix - this matches the actual package declarations in Veins NED files
import org.car2x.veins.base.connectionManager.ConnectionManager;
import org.car2x.veins.base.modules.BaseWorldUtility;
import org.car2x.veins.modules.mobility.traci.TraCIScenarioManagerLaunchd;
import org.car2x.veins.modules.obstacle.ObstacleControl;
import org.car2x.veins.modules.obstacle.VehicleObstacleControl;
import org.car2x.veins.nodes.Car; // Veins 5.3 uses 'Car' not 'TraCINode'
import org.car2x.veins.nodes.RSU;
import org.car2x.veins.nodes.TrafficLight;  // Add this import


network fourWay
{
    parameters:
        double playgroundSizeX @unit(m) = default(2500m);
        double playgroundSizeY @unit(m) = default(2500m);
        double playgroundSizeZ @unit(m) = default(50m);
        int numNodes = default(0);
        int numTls   = default(0);
        int numRsu   = default(0);
        int numIntersectionUnits = default(0);  // static PBFT consensus units (one per lane); 0 = none

    @display("bgb=2500,2500");

    submodules:
        world: BaseWorldUtility {
            parameters:
                playgroundSizeX = parent.playgroundSizeX;
                playgroundSizeY = parent.playgroundSizeY;
                playgroundSizeZ = parent.playgroundSizeZ;
                @display("p=30,0;i=misc/globe");
        }

        connectionManager: ConnectionManager {
            @display("p=100,100");
        }

        manager: org.car2x.veins.modules.mobility.traci.TraCIScenarioManagerLaunchd {
            @display("p=100,50");
        }

        obstacles: ObstacleControl {
            @display("p=100,150");
        }

        // Required name for PhyLayer80211p / VehicleObstacleShadowing (see VehicleObstacleControlAccess)
        vehicleObstacles: VehicleObstacleControl {
            @display("p=100,175");
        }

        node[numNodes]: Car;
        rsu[numRsu]: RSU;
        tls[numTls]: TrafficLight;  // Add this line

        // Static intersection units: RSU-hosted PBFT replicas (radio + static
        // position, no TraCIMobility). They join the quorum, witness/echo vehicle
        // arrivals, and execute the committed order, but never announce/stop/cross.
        // Green antenna-tower icon so they stand out from vehicles in Qtenv (BaseMobility
        // only overwrites the position tag, so the icon/size/color persist).
        iu[numIntersectionUnits]: RSU {
            @display("i=device/antennatower,#00aa00;is=l");
        }

    // Radios are wired at runtime by the ConnectionManager via sendDirect, so the
    // static nodes' veinsradioIn gates are intentionally left unconnected here
    // (mirrors veins' base Scenario.ned). Required once iu[]/rsu[]/tls[] are non-empty.
    connections allowunconnected:

}

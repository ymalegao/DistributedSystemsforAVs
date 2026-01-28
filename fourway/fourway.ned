// Use org.car2x.veins prefix - this matches the actual package declarations in Veins NED files
import org.car2x.veins.base.connectionManager.ConnectionManager;
import org.car2x.veins.base.modules.BaseWorldUtility;
import org.car2x.veins.modules.mobility.traci.TraCIScenarioManagerLaunchd;
import org.car2x.veins.modules.obstacle.ObstacleControl;
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

        node[numNodes]: Car;
        rsu[numRsu]: RSU;
        tls[numTls]: TrafficLight;  // Add this line

}

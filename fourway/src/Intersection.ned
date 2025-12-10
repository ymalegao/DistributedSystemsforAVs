package src;

import org.car2x.veins.base.modules.IBaseApplLayer;

simple IntersectionApp like IBaseApplLayer
{
    parameters:
        @class(IntersectionApp);
        @display("i=block/app");
        
        // Parameters from DemoBaseApplLayer (REQUIRED)
        int headerLength = default(88bit) @unit(bit);
        bool sendBeacons = default(false);  // Set to false since you're not using beacons
        int beaconLengthBits = default(256bit) @unit(bit);
        int beaconUserPriority = default(7);
        double beaconInterval = default(1s) @unit(s);
        int dataLengthBits = default(1024bit) @unit(bit);
        bool dataOnSch = default(false);
        int dataUserPriority = default(7);
        bool avoidBeaconSynchronization = default(true);
        bool sendWSA = default(false);
        int wsaLengthBits = default(250bit) @unit(bit);
        double wsaInterval = default(1s) @unit(s);  // THIS WAS MISSING
        
        // Your custom parameters
        string gatewayHost = default("127.0.0.1");
        int gatewayPort = default(9001);
        string carId = default("CAR_1");
        string direction = default("straight");
        double stopDistance @unit(m) = default(50m);
        double intersectionX @unit(m) = default(0m);
        double intersectionY @unit(m) = default(0m);
        
    gates:
        input lowerLayerIn;
        output lowerLayerOut;
        input lowerControlIn;
        output lowerControlOut;
}
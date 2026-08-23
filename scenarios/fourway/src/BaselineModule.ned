package src;

import org.car2x.veins.modules.application.traci.TraCIDemo11p;

simple BaselineModule extends TraCIDemo11p
{
    parameters:
        @class(BaselineModule);
        @display("i=block/app");

        sendBeacons = default(false);
        dataOnSch = default(false);
        int leaderReplicaId = default(-1);
        string carId = default("");
        int ambulanceReplicaId = default(-1);
        bool isByzantine = default(false);
        int byzantineType = default(0);
        string channelMetricsCsvDir = default("");
        double intersectionX @unit(m) = default(300m);
        double intersectionY @unit(m) = default(300m);
        double stopDistance @unit(m) = default(50m);
        double departDistance @unit(m) = default(20m);
        double stoppedSpeed @unit(mps) = default(0.5mps);
}

//
// V2VProxyModule.ned
// OMNeT++ module that acts as proxy between BFT-SMaRt Java process and Veins V2V
//

package org.car2x.veins.modules.bftsmart;

import org.car2x.veins.modules.application.ieee80211p.DemoBaseApplLayer;

// V2V Proxy Module for BFT-SMaRt
// This module bridges JNI calls from Java BFT-SMaRt replicas to OMNeT++ V2V communication
simple V2VProxyModule extends DemoBaseApplLayer
{
    parameters:
        @class(V2VProxyModule);
        // Replica identification
        int replicaId = default(-1);

        // Simulation time to trigger JOIN request (seconds)
        // Each replica triggers at triggerJoinTime + (replicaId * 0.5s) to stagger
        // Default is 12s assuming cars spawn around t=10s
        double triggerJoinTime @unit(s) = default(1s);

        // Service channel for V2V
        int serviceChannel = default(2);

        // Intersection parameters for consensus triggering
        double intersectionX @unit(m) = default(104m);
        double intersectionY @unit(m) = default(82m);
        double stopDistance @unit(m) = default(50m);

        // Intersection physics for delay calculation
        double intersectionWidth @unit(m) = default(25m);  // Distance to cross intersection
        double avgSpeed @unit(mps) = default(10mps);       // Average vehicle speed (m/s)
        double safetyGap @unit(s) = default(2s);           // Safety buffer between cars

        // Consensus timeout (fallback if BFT fails to reach quorum)
        // After this timeout, vehicles resume without coordination
        double consensusTimeoutSec @unit(s) = default(40s);

        // Display
        @display("i=block/app2");
        
        // Statistics
        @signal[bftMsgSent](type=long);
        @signal[bftMsgReceived](type=long);
        @statistic[bftMsgSent](title="BFT messages sent"; source=bftMsgSent; record=count,vector);
        @statistic[bftMsgReceived](title="BFT messages received"; source=bftMsgReceived; record=count,vector);
        
    gates:
        // Inherits gates from BaseApplLayer
        // input lowerLayerIn;
        // output lowerLayerOut;
        // input lowerControlIn;
        // output lowerControlOut;
}









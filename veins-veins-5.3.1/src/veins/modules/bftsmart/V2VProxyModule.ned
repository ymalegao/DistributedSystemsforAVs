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

        // Optional delay between VIEW completion and ORDER submit (leader only).
        // Useful for diagnostics to test residual channel-congestion effects.
        double orderDelayGap @unit(s) = default(0s);

        // V2V send-delay knobs (Relaxed for MAC Layer Stability)
        double viewJitterMin @unit(s) = default(0.001s);   
        double viewJitterMax @unit(s) = default(0.005s);
        // Bump to 2ms. 16 replicas * 2ms = 32ms window. Safe for CSMA.
        double viewAgreementSlotSec @unit(s) = default(0.025s); 

        double ackJitterMin @unit(s) = default(0.001s);    
        // Bump to 10ms. ACKs are low priority, spread them out to avoid choking the channel.
        double ackJitterMax @unit(s) = default(0.010s);

        // Bump to 2ms. This is the main culprit in your logs. 0.5ms is too fast.
        double witnessSlotSec @unit(s) = default(0.002s); 

        double broadcastSlotSec @unit(s) = default(0.005s);   
        double arrivalSlotSec @unit(s) = default(0.025s);     
        double broadcastJitterMin @unit(s) = default(0.0001s);
        double broadcastJitterMax @unit(s) = default(0.001s); 

        // ORDER collection window
        double orderCollectMinSec @unit(s) = default(4.0s);
        double orderCollectPerReplicaSec @unit(s) = default(0.08s);
        double orderCollectMaxSec @unit(s) = default(5s);

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









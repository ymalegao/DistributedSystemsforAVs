//
// ResDBIntersectionApp.ned — Steps 1-5: full ResDB PBFT consensus path.
//

package org.car2x.veins.modules.application.resDB;

import org.car2x.veins.modules.application.ieee80211p.DemoBaseApplLayer;

simple ResDBIntersectionApp extends DemoBaseApplLayer
{
    parameters:
        @class(veins::ResDBIntersectionApp);

        // ── Replica identity ──────────────────────────────────────────────────
        int replicaId = default(0);

        // ── Config path resolution (two mutually exclusive strategies) ────────
        string resdbConfigFile     = default("");
        string resdbPrivateKeyFile = default("");
        string resdbCertFile       = default("");
        string resdbLogDir         = default("");
        string resdbCryptoDir      = default("");

        // ── Optional per-vehicle channel utilization + SINR CSV (100 ms windows) ─
        // Writes channel_V<replicaId>.csv and sinr_V<replicaId>.csv (see sinr/ChannelMetrics).
        bool   enableChannelMetricsCsv = default(false);
        string channelMetricsCsvDir     = default("");  // empty → resdbLogDir, or "." if both empty

        // ── Step-2 transport smoke test ───────────────────────────────────────
        bool smokeTestBroadcast = default(false);

        // ── Step-3 radio transport ────────────────────────────────────────────
        bool useRadioTransport = default(true);
        double transportPollInterval = default(0.001);
        double resdbBroadcastJitterMin = default(0.0002);
        double resdbBroadcastJitterMax = default(0.001);

        // MAC-layer staggering for cert protocol.
        double viewJitterMin @unit(s) = default(0.001s);
        double viewJitterMax @unit(s) = default(0.005s);
        double viewAgreementSlotSec @unit(s) = default(0.025s);
        double broadcastJitterMin @unit(s) = default(0.0001s);
        double broadcastJitterMax @unit(s) = default(0.001s);
        double broadcastSlotSec @unit(s) = default(0.005s);

        // ── Step-4 time virtualization ────────────────────────────────────────
        bool enableSimTimeProvider = default(true);
        double timeTickInterval = default(0.001);

        // ── Step-5 intersection geometry ──────────────────────────────────────
        double intersectionX @unit(m) = default(300m);  // accepted for .ini compat; not used in code
        double intersectionY @unit(m) = default(300m);  // accepted for .ini compat; not used in code
        double stopDistance  @unit(m) = default(5m);   // distance-to-lane-end threshold (TraCI-based)
        int    totalVehicles          = default(4);

        // ── PBFT membership vs vehicle count ──────────────────────────────────
        // totalReplicas = number of PBFT replicas N (vehicles + static intersection
        // units). -1 → falls back to totalVehicles (back-compat: no units present).
        // f/quorum/PreVerify range use N; the arrival-cert/proposal/schedule layer
        // stays keyed to totalVehicles (only vehicles announce, hold certs, cross).
        int    totalReplicas          = default(-1);
        // Static intersection unit: participates in PBFT (vote/relay/execute) and
        // witnesses+echoes vehicle arrivals, but never announces, stops, crosses,
        // or proposes. Runs on an RSU node with no TraCIMobility.
        bool   isIntersectionUnit     = default(false);

        // ── Fault-tolerance experiment knob ─────────────────────────────────
        int    toleratedFaults         = default(-1);  // intended f; -1 = derive from static ResDB replica count

        // ── Step-5 cert collection / proposal ────────────────────────────────
        bool debugCertProtocol = default(false); // verbose ARRIVAL_ECHO / ARRIVAL_CERT tracing
        // Trace ResDB order callback → processOrders (enqueue/dequeue, skips, tail drops).
        bool debugOrderDelivery = default(true);
        bool enableArrivalCertRetries = default(true);
        double arrivalCertRetryIntervalSec @unit(s) = default(0.1s);
        int arrivalCertRetryMax = default(30); // extra ARRIVAL_CERT sends after first; 0 = unlimited until stop
        double certCollectionTimeoutSec @unit(s) = default(2s);
        double discoveryIntentSettleSec @unit(s) = default(1.5s);
        double stateAnnounceIntervalSec @unit(s) = default(0.1s);
        double broadcastArrivalAnnouncementIntervalSec @unit(s) = default(0.1s);

        // ── Staggered initial announcement ────────────────────────────────────
        double triggerJoinTimeSec @unit(s) = default(0.5s);
        double arrivalSlotSec     @unit(s) = default(0.025s);

        // ── Safety fallback timeouts ──────────────────────────────────────────
        double stopSignTimeoutSec  @unit(s) = default(10s);
        double consensusTimeoutSec @unit(s) = default(30s);

        // ── Step-5 TraCI / crossing ───────────────────────────────────────────
        double cruiseSpeedMps      = default(13.89);   // m/s handed to TraCI on resume
        double safetyGapS          = default(1.5);    // legacy fixed-stagger fallback (unused once TraCI clearance gating is on)
        double clearancePollPeriodSec @unit(s) = default(0.1s); // poll interval while waiting for predecessor to clear
        double clearanceTimeoutSec    @unit(s) = default(120s);   // hard fallback if predecessor never clears
        bool   isAmbulance         = default(false);  // ambulance gets top priority in schedule
        int    ambulanceReplicaId  = default(-1);     // if >=0, marks veh<id> as ambulance after SUMO identity binding
        bool   isByzantine         = default(false);  // enables Byzantine fault injection
        int    byzantineType       = default(0);      // 0=honest 1=false_lane 2=invalid_sig 3=equivocator 4=silent_primary 5=bad_proposal 6=fake_ambulance 7=fake_ambulance_follower 8=tamper_lane
        bool   byzantinePbftSilent = default(false);  // when true, Byzantine replica drops PBFT outbound messages
        bool   enableAmbulanceCertGate = default(false); // when true, rejects ambulance claims with empty ambulanceCertBytes (ablation toggle)

        // ── Post-consensus order gossip (Type 9) ──────────────────────────────
        bool   enableDecisionGossip                        = default(true);
        double decisionGossipInitialIntervalSec @unit(s)  = default(0.5s);
        int    decisionGossipMaxRetries                    = default(5);

        // ── Cancel / rollback protocol ───────────────────────────────────────
        bool   enableRollback                              = default(false);
        double cancelCertRetryIntervalSec @unit(s)         = default(0.1s);
        int    cancelCertRetryMax                          = default(10);
        double consensusRetryIntervalSec @unit(s)          = default(0.12s);
        int    consensusRetryMax                           = default(8);
        double brakingDecelMps2                            = default(4.5);
        double processingLatencyMargin                     = default(2.0);
        double rollbackVcTimeoutSec @unit(s)               = default(3s);
        string rollbackFaultMode                           = default("per_epoch"); // per_epoch | anchored

        // ── Milestone 4: view-change (VC) support ─────────────────────────────
        // PBFT complaint-timer duration in sim-seconds. A follower arms this
        // only after its local discovery round reaches COMPLETE.
        double pbftVcTimeoutSec @unit(s) = default(3s);
        string intendedDirection   = default("S");    // S/L/R
        string intendedLane        = default("");     // N/S/E/W — explicit cardinal lane (bypasses SUMO edge name parsing); empty = auto-detect from TraCI

        @display("i=block/app2");
}

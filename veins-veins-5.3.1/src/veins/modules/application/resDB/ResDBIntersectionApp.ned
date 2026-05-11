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

        // ── Step-2 transport smoke test ───────────────────────────────────────
        bool smokeTestBroadcast = default(false);

        // ── Step-3 radio transport ────────────────────────────────────────────
        bool useRadioTransport = default(true);
        double transportPollInterval = default(0.001);
        double resdbBroadcastJitterMin = default(0.0002);
        double resdbBroadcastJitterMax = default(0.001);

        // MAC-layer staggering for cert protocol (same knobs as V2VProxyModule.ned / V2VReliability.cc)
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

        // ── Step-5 cert collection / proposal ────────────────────────────────
        bool debugCertProtocol = default(false); // verbose ARRIVAL_ECHO / ARRIVAL_CERT tracing
        bool enableArrivalCertRetries = default(true);
        double arrivalCertRetryIntervalSec @unit(s) = default(0.1s);
        int arrivalCertRetryMax = default(30); // extra ARRIVAL_CERT sends after first; 0 = unlimited until stop
        double certCollectionTimeoutSec @unit(s) = default(2s);
        double stateAnnounceIntervalSec @unit(s) = default(0.1s);
        double broadcastArrivalAnnouncementIntervalSec @unit(s) = default(0.1s);

        // ── Step-5 staggered initial announcement (port from V2VProxyModule) ──
        double triggerJoinTimeSec @unit(s) = default(0.5s);
        double arrivalSlotSec     @unit(s) = default(0.025s);

        // ── Step-5 safety fallback timeouts (port from V2VProxyModule) ────────
        double stopSignTimeoutSec  @unit(s) = default(10s);
        double consensusTimeoutSec @unit(s) = default(30s);

        // ── Step-5 TraCI / crossing ───────────────────────────────────────────
        double cruiseSpeedMps      = default(14.0);   // m/s handed to TraCI on resume
        double safetyGapS          = default(1.5);    // legacy fixed-stagger fallback (unused once TraCI clearance gating is on)
        double clearancePollPeriodSec @unit(s) = default(0.1s); // poll interval while waiting for predecessor to clear
        double clearanceTimeoutSec    @unit(s) = default(8s);   // hard fallback if predecessor never clears
        bool   isAmbulance         = default(false);  // ambulance gets top priority in schedule
        bool   isByzantine         = default(false);  // enables Byzantine fault injection
        int    byzantineType       = default(0);      // 0=honest 1=false_lane 2=invalid_sig 3=equivocator 4=silent_primary 5=bad_proposal

        // ── Milestone 4: view-change (VC) support ─────────────────────────────
        // PBFT complaint-timer duration in sim-seconds.  After certCollectionTimeoutSec
        // fires on a follower with no order, the follower waits this long before forcing
        // a view-change via ResdbOmnetForceViewChange().
        double pbftVcTimeoutSec @unit(s) = default(3s);
        string intendedDirection   = default("S");    // S/L/R — replicated from V2VProxyModule
        string intendedLane        = default("");     // N/S/E/W — explicit cardinal lane (bypasses SUMO edge name parsing); empty = auto-detect from TraCI

        @display("i=block/app2");
}

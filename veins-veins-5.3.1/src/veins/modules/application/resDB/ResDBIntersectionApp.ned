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

        // ── Step-4 time virtualization ────────────────────────────────────────
        bool enableSimTimeProvider = default(true);
        double timeTickInterval = default(0.001);

        // ── Step-5 intersection geometry (matches V2VProxyModule params) ──────
        double intersectionX @unit(m) = default(300m);
        double intersectionY @unit(m) = default(300m);
        double stopDistance  @unit(m) = default(5m);
        int    totalVehicles          = default(4);

        // ── Step-5 cert collection / proposal ────────────────────────────────
        double certCollectionTimeoutSec @unit(s) = default(2s);
        double stateAnnounceIntervalSec @unit(s) = default(0.1s);

        // ── Step-5 TraCI / crossing ───────────────────────────────────────────
        double cruiseSpeedMps = default(14.0);   // m/s handed to TraCI on resume
        double safetyGapS     = default(1.5);    // seconds between consecutive crossings
        bool   isAmbulance    = default(false);  // ambulance gets top priority in schedule

        @display("i=block/app2");
}

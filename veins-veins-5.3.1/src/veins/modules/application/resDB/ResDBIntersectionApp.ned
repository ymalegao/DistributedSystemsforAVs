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
        double transportPollInterval = default(0.5);
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
        double timeTickInterval = default(0.5);

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
        // Number of static intersection units in the run. Units are the top
        // intersectionUnitCount replica IDs, i.e. [totalReplicas - count, totalReplicas).
        // Every replica (vehicles included) needs this so it knows which IDs are units.
        int    intersectionUnitCount  = default(0);

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
        double precedingBatchPollPeriodSec @unit(s) = default(0.1s); // poll interval while waiting for predecessor to clear; also drives crash-dwell perception (§ crashDwellSec)
        double clearanceTimeoutSec    @unit(s) = default(120s);   // hard fallback if predecessor never clears
        bool   isAmbulance         = default(false);  // ambulance gets top priority in schedule
        int    ambulanceReplicaId  = default(-1);     // if >=0, marks veh<id> as ambulance after SUMO identity binding
        bool   isByzantine         = default(false);  // enables Byzantine fault injection
        int    byzantineType       = default(0);      // 0=honest 1=false_lane 2=invalid_sig 3=equivocator 4=silent_primary 5=bad_proposal 6=fake_ambulance 7=fake_ambulance_follower 8=tamper_lane
        bool   byzantinePbftSilent = default(false);  // when true, Byzantine replica drops PBFT outbound messages
        bool   enableAmbulanceCertGate = default(false); // when true, rejects ambulance claims with empty ambulanceCertBytes (ablation toggle)
        string falseLaneColluderIds = default(""); // comma-separated replica IDs selected by --randomize N F
        bool   enableArrivalPositionGate = default(true); // physical lane/position witness gate; false only for D3

        // ── Post-consensus order gossip (Type 9) ──────────────────────────────
        bool   enableDecisionGossip                        = default(true);
        double decisionGossipInitialIntervalSec @unit(s)  = default(0.5s);
        int    decisionGossipMaxRetries                    = default(5);

        // ── Cancel / rollback protocol ───────────────────────────────────────
        bool   enableRollback                              = default(false);
        double cancelCertRetryIntervalSec @unit(s)         = default(0.1s);
        int    cancelCertRetryMax                          = default(10);
        // Exponential backoff (spec §11) for CANCEL_CERT/CLEAR_CERT retry and
        // CANCEL-commit gossip. Added after observing that fixed-interval
        // retries kept re-adding channel load right on top of the PBFT
        // PREPARE/COMMIT broadcast for the same event, shrinking an already
        // thin quorum margin (14 real voters, quorum 11) under a synchronized
        // burst. interval(k) = min(base * factor^k, cap) + jitter.
        double evidenceRetryBaseSec @unit(s)                = default(0.1s);
        double evidenceRetryFactor                          = default(2.0);
        double evidenceRetryCapSec @unit(s)                 = default(2.0s);
        double cancelGossipRetryBaseSec @unit(s)            = default(0.25s);
        double cancelGossipRetryCapSec @unit(s)             = default(4.0s);
        double consensusRetryIntervalSec @unit(s)          = default(0.12s);
        int    consensusRetryMax                           = default(8);
        // Deterministic TYPE11 suppression. The live carrier target is
        // min(type11RelayCarrierCap, f+1), with f derived from the configured
        // membership. Relay selection itself uses no random jitter.
        int    type11RelayCarrierCap                       = default(2);
        double type11RelayBaseDelaySec @unit(s)            = default(0.02s);
        double type11RelaySlotSec @unit(s)                 = default(0.02s);
        double brakingDecelMps2                            = default(4.5);
        double processingLatencyMargin                     = default(2.0);
        double rollbackVcTimeoutSec @unit(s)               = default(3s);
        // Scenario-15/16 protocol-surface experiment. When enabled, whichever
        // replica is deterministically selected as the first CANCEL proposer
        // suppresses that proposal. This targets the leader role after the
        // schedule is known instead of assuming a fixed replica will occupy it.
        bool   injectSuppressInitialCancelLeader           = default(false);
        // Guard/ablation switch. Honest replicas arm the same deterministic
        // proposer lease; on expiry they advance to the next candidate.
        bool   enableCancelLeaderFailover                  = default(true);
        // Scenario-16 fabricated-clearance experiment. The selected epoch-1
        // recovery proposer submits once while the incident is still BLOCKING,
        // carrying a structurally present but invalid CLEAR certificate.
        bool   injectFabricatedClearanceLeader             = default(false);
        // Guard/ablation switch for voter-side CLEAR certificate validation.
        // The structural trailer remains required in both modes.
        bool   enableRecoveryClearEvidenceGate             = default(true);
        string rollbackFaultMode                           = default("per_epoch"); // per_epoch | anchored

        // ── Scenario 16: app mute grace after manager freeze (freeze/tow is manager-owned)
        double crashMacGraceSec @unit(s)                   = default(0.2s);
        // Crash-dwell perception (BLOCKED trigger): a witness treats a batch
        // vehicle as crashed once it's stationary in the conflict box for this
        // long. Scanned inside precedingBatchPollPeriodSec; no separate enable
        // flag — gated by enableRollback.
        double crashDwellSec @unit(s)                      = default(2.0s);
        double crashSpeedEps                                = default(0.1); // m/s stationary threshold
        // Clearance dwell (CLEAR trigger): a witness treats the conflict box as
        // clear once no vehicle occupies it for this long. Scanned in the same
        // precedingBatchPollPeriodSec tick as crash-dwell above.
        double clearDwellSec @unit(s)                       = default(1.0s);
        // Deterministic fallback slot for competing CLEAR_CERT assemblers and
        // relays. 100 ms exceeds the configured per-replica radio staggering,
        // allowing a valid earlier carrier to cancel later candidates.
        double clearCertCandidateSlotSec @unit(s)            = default(0.1s);
        // Advisory WAIT heartbeat (spec §8) — one signed message from the
        // ordinary next-epoch certificate primary while BLOCKING, purely to
        // delay local leader-suspicion timers. No quorum, no PBFT.
        double waitHeartbeatIntervalSec @unit(s)            = default(1.0s);
        double waitHeartbeatMaxDeferralSec @unit(s)         = default(2.5s);
        double waitClockSkewSec @unit(s)                    = default(0s);

        // ── Milestone 4: view-change (VC) support ─────────────────────────────
        // PBFT complaint-timer duration in sim-seconds. A follower arms this
        // only after its local discovery round reaches COMPLETE.
        double pbftVcTimeoutSec @unit(s) = default(3s);
        string intendedDirection   = default("S");    // S/L/R
        string intendedLane        = default("");     // N/S/E/W — explicit cardinal lane (bypasses SUMO edge name parsing); empty = auto-detect from TraCI

        @display("i=block/app2");
}

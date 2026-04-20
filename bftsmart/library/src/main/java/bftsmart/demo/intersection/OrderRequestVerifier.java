package bftsmart.demo.intersection;

import bftsmart.tom.core.messages.TOMMessage;
import bftsmart.tom.server.RequestVerifier;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * Byzantine Firewall for the single-round PROPOSE_ALL protocol.
 *
 * Runs on every follower before it sends WRITE (i.e., before voting).
 * If any check fails the batch is rejected, which triggers a BFT leader change.
 *
 * For PROPOSE_ALL messages, the following checks are applied:
 *   1. Signature validity       f+1 ECDSA P-256 (SHA256withECDSA) V2V witness signatures on the vehicleStates are present and valid
 *   2. No phantoms              every vehicleId in the schedule appears in the vehicleStates
 *   3. No duplicates            no vehicleId appears in more than one batch
 *   4. Collision safety         every pair within a batch passes ConflictMatrix.isSafeToBatch()
 *   5. Lane queue order         same-lane total order (positionInLane, then numeric veh id): if A is
 *                               ahead of B then A's batch index must be strictly less than B's
 *   6. Leader Rejection Rule    a QUIET vehicle (failed cert) must occupy an exclusive singleton batch
 *   7. ARRIVAL_CERT omission    [EP5/G3 censorship resistance] — any car whose ARRIVAL_CERT is already
 *                               in this follower's local C++ collectedCerts MUST appear as SIGNED in the
 *                               proposal; if omitted, ≥f+1 honest followers reject, RequestsTimer fires,
 *                               view-change occurs within epoch e (not epoch e+1).
 *                               Requires getCertSnapshot() JNI pull from C++ collectedCerts.
 *                               Returns empty set when JNI unavailable (unit-test safe; falls back to
 *                               prior EP5 guarantee).
 *   8. Deterministic re-exec    [anti-computation-fraud / safety] — the follower independently rebuilds
 *                               the schedule from the proposal's vehicleStates and its own waitRegistry
 *                               (kept in sync via snapshot/install) and compares with the submitted
 *                               orderBagStr.  A mismatch means the leader computed a wrong schedule.
 *                               Pure Java; no cross-layer state needed.
 *                               NOTE: does NOT give EP5 censorship resistance — a BL could omit a car
 *                               from vehicleStatesStr before this check runs.  Check 7 closes that gap.
 *
 * All other message types pass through without validation.
 */
public class OrderRequestVerifier implements RequestVerifier {

    private final IntersectionServer server;

    OrderRequestVerifier(IntersectionServer server) {
        this.server = server;
    }

    @Override
    public boolean isValidRequest(TOMMessage request) {
        String cmd = new String(request.getContent(), StandardCharsets.UTF_8).trim();

        if (cmd.startsWith("PROPOSE_ALL:")) {
            return validateProposeAll(cmd.substring("PROPOSE_ALL:".length()));
        }

        // Unknown command types pass through; appExecuteBatch will handle/reject them.
        return true;
    }

    /**
     * Validates the PROPOSE_ALL payload.
     *
     * Payload format (after stripping "PROPOSE_ALL:" prefix):
     *   <proposerId>:<vehicleStatesStr>:<perCarCerts>:<orderBagStr>
     *
     * vehicleStatesStr has no ':', perCarCerts has no ':', so split with limit=4 is safe.
     * orderBagStr may contain ':' (epoch:assignments) and is captured as parts[3].
     */
    private boolean validateProposeAll(String payload) {
        String[] parts = payload.split(":", 4);
        if (parts.length < 4) {
            System.err.println("[VERIFIER] PROPOSE_ALL payload too short: " + payload);
            return false;
        }

        String vehicleStatesStr = parts[1];
        String perCarCertsStr   = parts[2];
        String orderBagStr      = parts[3];

        

        // ---- Check 1: Per-car ARRIVAL_CERT validation ----
        // Each SIGNED car must have f+1 valid echo signatures (physically verified by peers).
        List<VehicleState> states = ViewConsensusProtocol.parseVehicleStates(vehicleStatesStr);
        if (states.isEmpty()) {
            System.err.println("[VERIFIER] PROPOSE_ALL: no vehicle states parsed");
            return false;
        }

        Map<String, List<ViewConsensusProtocol.EchoSig>> certs =
                ViewConsensusProtocol.parsePerCarCerts(perCarCertsStr);
        if (!ViewConsensusProtocol.validatePerCarCerts(states, certs)) {
            System.err.println("[VERIFIER] PROPOSE_ALL: per-car cert validation failed");
            return false;
        }

        // Build stateMap for schedule validation (all vehicles from vehicleStates)
        Map<String, VehicleState> stateMap = new HashMap<>();
        for (VehicleState vs : states) {
            stateMap.put(vs.vehicleId, vs);
        }

        // ---- Parse schedule ----
        OrderBag bag = OrderScheduler.parseOrderBag(orderBagStr);
        if (bag == null) {
            System.err.println("[VERIFIER] PROPOSE_ALL: could not parse OrderBag");
            return false;
        }

        // ---- Check 2 & 3: No phantoms + No duplicates ----
        Set<String> scheduled = new HashSet<>();
        for (Batch b : bag.batches) {
            for (String vid : b.vehicleIds) {
                if (!stateMap.containsKey(vid)) {
                    System.err.println("[VERIFIER] PROPOSE_ALL: phantom vehicleId in schedule: " + vid);
                    return false;
                }
                if (!scheduled.add(vid)) {
                    System.err.println("[VERIFIER] PROPOSE_ALL: duplicate vehicleId in schedule: " + vid);
                    return false;
                }
            }
        }

        // ---- Check 4: Collision safety ----
        for (Batch b : bag.batches) {
            for (int i = 0; i < b.vehicleIds.size(); i++) {
                VehicleState vsA = stateMap.get(b.vehicleIds.get(i));
                for (int j = i + 1; j < b.vehicleIds.size(); j++) {
                    VehicleState vsB = stateMap.get(b.vehicleIds.get(j));
                    if (vsA == null || vsB == null) {
                        System.err.println("[VERIFIER] PROPOSE_ALL: null state in batch — rejecting");
                        return false;
                    }
                    if (!ConflictMatrix.isSafeToBatch(vsA, vsB)) {
                        System.err.println("[VERIFIER] PROPOSE_ALL: conflicting pair in same batch: "
                                + vsA.vehicleId + " and " + vsB.vehicleId);
                        return false;
                    }
                }
            }
        }

        // ---- Check 5: Same-lane queue order (front vehicle must be in earlier batch) ----
        for (VehicleState behind : stateMap.values()) {
            if (!scheduled.contains(behind.vehicleId)) continue;
            for (VehicleState front : stateMap.values()) {
                if (!scheduled.contains(front.vehicleId)) continue;
                if (!front.lane.equals(behind.lane)) continue;
                if (IntersectionTypes.compareLaneQueueOrder(front, behind) >= 0) continue;
                int frontBi  = OrderScheduler.batchIndexOf(bag, front.vehicleId);
                int behindBi = OrderScheduler.batchIndexOf(bag, behind.vehicleId);
                if (frontBi >= behindBi) {
                    System.err.println("[VERIFIER] PROPOSE_ALL: same-lane queue violation: "
                            + front.vehicleId + " (pos " + front.positionInLane + ", batch " + frontBi
                            + ") must be before " + behind.vehicleId + " (pos "
                            + behind.positionInLane + ", batch " + behindBi + ") — rejecting");
                    return false;
                }
            }
        }

        // ---- Check 6: Leader Rejection Rule — QUIET vehicle must not be co-scheduled ----
        // A vehicle marked QUIET (no f+1 cyber VIEW_AGREEMENT signatures) must occupy an
        // exclusive singleton batch. If the leader schedules it alongside any other vehicle,
        // followers immediately reject the proposal.
        for (Batch b : bag.batches) {
            boolean hasQuiet = false;
            for (String vid : b.vehicleIds) {
                VehicleState vs = stateMap.get(vid);
                if (vs != null && "QUIET".equals(vs.cyberStatus)) {
                    hasQuiet = true;
                    break;
                }
            }
            if (hasQuiet && b.vehicleIds.size() > 1) {
                System.err.println("[VERIFIER] PROPOSE_ALL: Leader Rejection Rule — QUIET vehicle "
                        + "co-scheduled in batch of size " + b.vehicleIds.size()
                        + ": " + b.vehicleIds + " — rejecting");
                return false;
            }
        }

        // ---- Check 7: ARRIVAL_CERT omission guard (EP5/G3 censorship resistance) ----
        // If this follower has already collected a valid ARRIVAL_CERT for a car (C++ side),
        // that car MUST appear as SIGNED in the proposal.  A Byzantine leader that omits
        // such a car will be rejected by ≥f+1 honest followers, preventing 2f+1 WRITE
        // votes.  RequestsTimer fires → intra-epoch view-change → new honest leader
        // re-includes the car → decision within epoch e (not epoch e+1).
        // Returns empty set when JNI is unavailable (e.g. unit-test context), which
        // is safe: no rejection occurs and the protocol falls back to prior EP5 guarantees.
        Set<String> localCertIds = server.getCertSnapshot();
        for (String certId : localCertIds) {
            VehicleState vs = stateMap.get(certId);
            if (vs == null || !"SIGNED".equals(vs.cyberStatus)) {
                System.err.println("[VERIFIER] PROPOSE_ALL: cert-omission — "
                        + certId + " in local collectedCerts but not SIGNED in proposal — rejecting");
                return false;
            }
        }

        // ---- Check 8: Deterministic schedule re-execution (anti-computation fraud) ----
        // Followers independently rebuild the schedule from the proposal's own vehicleStates
        // and their local waitRegistry (which is kept in sync across view-changes via
        // snapshot/install).  Any mismatch means the leader submitted a malformed or
        // miscomputed ordering and is rejected.
        // NOTE: This check detects computation fraud (safety) but NOT censorship (EP5) —
        // a BL could still omit a car from vehicleStatesStr before this check runs.
        // Check 7 above closes that gap.
        String expectedSchedule = OrderScheduler.serializeOrderBagForBFT(
                OrderScheduler.buildProposal(stateMap, bag.epoch, server.getWaitRegistry()));
        if (!expectedSchedule.equals(orderBagStr)) {
            System.err.println("[VERIFIER] PROPOSE_ALL: schedule mismatch — leader submitted "
                    + orderBagStr + " but follower recomputed " + expectedSchedule + " — rejecting");
            return false;
        }

        return true;
    }
}

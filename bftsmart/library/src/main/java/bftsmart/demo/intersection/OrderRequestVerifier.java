package bftsmart.demo.intersection;

import bftsmart.tom.core.messages.TOMMessage;
import bftsmart.tom.server.RequestVerifier;
import java.nio.charset.StandardCharsets;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;

/**
 * Byzantine Firewall for the ORDER_PROPOSE round.
 *
 * Runs on every follower before it sends WRITE (i.e., before voting).
 * If any check fails the batch is rejected, which triggers a BFT leader change.
 *
 * Checks applied (ORDER_PROPOSE messages only; all others pass through):
 *   1. Completeness   every vehicleId in agreedViewState appears exactly once
 *   2. No duplicates  no vehicleId appears in more than one batch
 *   3. Safety         every pair within a batch passes ConflictMatrix.isSafeToBatch()
 *   4. Lane queue     same-lane total order (positionInLane, then numeric veh id): if A is ahead of B
 *                     then A's batch index must be strictly less than B's
 */
public class OrderRequestVerifier implements RequestVerifier {

    private final IntersectionServer server;

    OrderRequestVerifier(IntersectionServer server) {
        this.server = server;
    }

    @Override
    public boolean isValidRequest(TOMMessage request) {
        String cmd = new String(request.getContent(), StandardCharsets.UTF_8).trim();
        if (!cmd.startsWith("ORDER_PROPOSE:")) return true; // not an ORDER  pass through

        Map<String, VehicleState> view = server.agreedViewState;
        if (view == null) return true; // VIEW not yet agreed; pass through and let appExecuteBatch handle

        // Strip "ORDER_PROPOSE:" prefix, then delegate to payload parser
        String payload = cmd.substring("ORDER_PROPOSE:".length());
        OrderBag bag = OrderScheduler.parseOrderBag(payload);
        if (bag == null) {
            System.err.println("[VERIFIER] Could not parse OrderBag  rejecting");
            return false;
        }

        // ---- Check 1 & 2: Completeness + No Duplicates ----
        Set<String> bagged = new HashSet<>();
        for (Batch b : bag.batches) {
            for (String vid : b.vehicleIds) {
                if (!bagged.add(vid)) {
                    System.err.println("[VERIFIER] Duplicate vehicleId in OrderBag: " + vid);
                    return false;
                }
            }
        }
        if (!bagged.equals(view.keySet())) {
            System.err.println("[VERIFIER] OrderBag car set " + bagged
                    + " does not match agreedView " + view.keySet());
            return false;
        }

        // ---- Check 3: Collision Safety ----
        for (Batch b : bag.batches) {
            for (int i = 0; i < b.vehicleIds.size(); i++) {
                VehicleState vsA = view.get(b.vehicleIds.get(i));
                for (int j = i + 1; j < b.vehicleIds.size(); j++) {
                    VehicleState vsB = view.get(b.vehicleIds.get(j));
                    if (vsA == null || vsB == null) {
                        System.err.println("[VERIFIER] Unknown vehicleId in batch  rejecting");
                        return false;
                    }
                    if (!ConflictMatrix.isSafeToBatch(vsA, vsB)) {
                        System.err.println("[VERIFIER] Conflicting pair in same batch: "
                                + vsA.vehicleId + " and " + vsB.vehicleId);
                        return false;
                    }
                }
            }
        }

        // ---- Check 4: Same-lane queue order (compareLaneQueueOrder: front before back) ----
        for (VehicleState behind : view.values()) {
            for (VehicleState front : view.values()) {
                if (!front.lane.equals(behind.lane)) continue;
                if (IntersectionTypes.compareLaneQueueOrder(front, behind) >= 0) continue;
                int frontBi = batchIndexOf(bag, front.vehicleId);
                int behindBi = batchIndexOf(bag, behind.vehicleId);
                if (frontBi >= behindBi) {
                    System.err.println("[VERIFIER] Same-lane queue violation: " + front.vehicleId
                            + " (pos " + front.positionInLane + ", batch " + frontBi
                            + ") must be before " + behind.vehicleId + " (pos "
                            + behind.positionInLane + ", batch " + behindBi + ")  rejecting");
                    return false;
                }
            }
        }

        return true;
    }

    private int batchIndexOf(OrderBag bag, String vehicleId) {
        return OrderScheduler.batchIndexOf(bag, vehicleId);
    }
}

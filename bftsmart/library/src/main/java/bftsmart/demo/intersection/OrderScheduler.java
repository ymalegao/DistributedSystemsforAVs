package bftsmart.demo.intersection;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

final class OrderScheduler {

    /**
     * Parse the ORDER_PROPOSE payload.
     * Format: "<epoch>:<veh0:0;veh1:0;veh2:1;veh3:2>"
     */
    static OrderBag parseOrderBag(String payload) {
        if (payload == null || payload.isEmpty()) {
            return null;
        }
        try {
            String[] top = payload.split(":", 2);
            int epoch = Integer.parseInt(top[0].trim());
            OrderBag bag = new OrderBag(epoch);

            if (top.length < 2 || top[1].trim().isEmpty()) {
                return bag;
            }

            int maxBatchIndex = -1;
            String[] entries = top[1].split(";");
            for (String entry : entries) {
                String[] keyValue = entry.split(":");
                if (keyValue.length == 2) {
                    maxBatchIndex = Math.max(maxBatchIndex, Integer.parseInt(keyValue[1].trim()));
                }
            }
            for (int idx = 0; idx <= maxBatchIndex; idx++) {
                bag.batches.add(new Batch());
            }

            for (String entry : entries) {
                String[] keyValue = entry.split(":");
                if (keyValue.length != 2) {
                    continue;
                }
                String vehicleId = keyValue[0].trim();
                int batchIndex = Integer.parseInt(keyValue[1].trim());
                bag.batches.get(batchIndex).vehicleIds.add(vehicleId);
            }
            return bag;
        } catch (Exception e) {
            System.err.println("[ORDER-PARSE] Failed to parse OrderBag: " + e.getMessage());
            return null;
        }
    }

    static String serializeOrderBagForJNI(OrderBag bag) {
        StringBuilder sb = new StringBuilder();
        for (int batchIndex = 0; batchIndex < bag.batches.size(); batchIndex++) {
            for (String vehicleId : bag.batches.get(batchIndex).vehicleIds) {
                if (sb.length() > 0) {
                    sb.append(';');
                }
                sb.append(vehicleId).append(":BATCH:").append(batchIndex);
            }
        }
        return sb.toString();
    }

    static String serializeOrderBagForBFT(OrderBag bag) {
        StringBuilder sb = new StringBuilder();
        for (int batchIndex = 0; batchIndex < bag.batches.size(); batchIndex++) {
            for (String vehicleId : bag.batches.get(batchIndex).vehicleIds) {
                if (sb.length() > 0) {
                    sb.append(';');
                }
                sb.append(vehicleId).append(':').append(batchIndex);
            }
        }
        return bag.epoch + ":" + sb;
    }

    /**
     * Build the leader ORDER proposal from the agreed view.
     */
    static OrderBag buildProposal(Map<String, VehicleState> view, int epoch, Map<String, Integer> waitRegistry) {
        if (view == null || view.isEmpty()) {
            return new OrderBag(epoch);
        }

        List<VehicleState> ambulances = new ArrayList<>();
        for (VehicleState vehicleState : view.values()) {
            if (vehicleState.isAmbulance) {
                ambulances.add(vehicleState);
            }
        }
        ambulances.sort(Comparator
                .comparingInt((VehicleState vehicleState) -> vehicleState.positionInLane)
                .thenComparingInt(vehicleState -> IntersectionTypes.vehicleIdNumericOrder(vehicleState.vehicleId)));

        Set<String> priorityIds = new HashSet<>();
        List<VehicleState> workQueue = new ArrayList<>();
        for (VehicleState ambulance : ambulances) {
            List<VehicleState> blockers = new ArrayList<>();
            for (VehicleState vehicleState : view.values()) {
                if (!vehicleState.lane.equals(ambulance.lane) || vehicleState.isAmbulance) {
                    continue;
                }
                if (IntersectionTypes.compareLaneQueueOrder(vehicleState, ambulance) < 0) {
                    blockers.add(vehicleState);
                }
            }
            blockers.sort(Comparator
                    .comparingInt((VehicleState vehicleState) -> vehicleState.positionInLane)
                    .thenComparingInt(vehicleState -> IntersectionTypes.vehicleIdNumericOrder(vehicleState.vehicleId)));
            for (VehicleState blocker : blockers) {
                if (priorityIds.add(blocker.vehicleId)) {
                    workQueue.add(blocker);
                }
            }
            if (priorityIds.add(ambulance.vehicleId)) {
                workQueue.add(ambulance);
            }
        }

        List<VehicleState> remaining = new ArrayList<>();
        for (VehicleState vehicleState : view.values()) {
            if (!priorityIds.contains(vehicleState.vehicleId)) {
                remaining.add(vehicleState);
            }
        }
        remaining.sort(Comparator
                .comparingInt((VehicleState vehicleState) -> -(waitRegistry.getOrDefault(vehicleState.vehicleId, 0)))
                .thenComparingInt(vehicleState -> vehicleState.positionInLane)
                .thenComparingInt(vehicleState -> IntersectionTypes.vehicleIdNumericOrder(vehicleState.vehicleId)));
        workQueue.addAll(remaining);

        StringBuilder workQueueLog = new StringBuilder();
        for (VehicleState vehicleState : workQueue) {
            if (workQueueLog.length() > 0) {
                workQueueLog.append(',');
            }
            workQueueLog.append(vehicleState.vehicleId);
        }
        System.out.println("[LEADER] workQueue=" + workQueueLog);

        OrderBag bag = new OrderBag(epoch);
        Set<String> placed = new HashSet<>();
        while (placed.size() < view.size()) {
            VehicleState head = null;
            for (VehicleState vehicleState : workQueue) {
                if (placed.contains(vehicleState.vehicleId)) {
                    continue;
                }
                if (!allSameLaneFrontPlaced(vehicleState, view, placed)) {
                    continue;
                }
                head = vehicleState;
                break;
            }
            if (head == null) {
                System.err.println("[LEADER] buildProposal: no schedulable head (placed="
                        + placed.size() + "/" + view.size() + ")");
                break;
            }

            Batch batch = new Batch();
            batch.vehicleIds.add(head.vehicleId);
            placed.add(head.vehicleId);

            boolean grew;
            do {
                grew = false;
                for (VehicleState candidate : workQueue) {
                    if (placed.contains(candidate.vehicleId)) {
                        continue;
                    }
                    if (!allSameLaneFrontPlaced(candidate, view, placed)) {
                        continue;
                    }
                    boolean safe = true;
                    for (String inBatch : batch.vehicleIds) {
                        if (!ConflictMatrix.isSafeToBatch(view.get(inBatch), candidate)) {
                            safe = false;
                            break;
                        }
                    }
                    if (safe) {
                        batch.vehicleIds.add(candidate.vehicleId);
                        placed.add(candidate.vehicleId);
                        grew = true;
                    }
                }
            } while (grew);

            bag.batches.add(batch);
        }

        for (VehicleState ambulance : ambulances) {
            int ambulanceBatchIndex = batchIndexOf(bag, ambulance.vehicleId);
            StringBuilder blockers = new StringBuilder();
            for (VehicleState vehicleState : view.values()) {
                if (!vehicleState.lane.equals(ambulance.lane)) {
                    continue;
                }
                if (IntersectionTypes.compareLaneQueueOrder(vehicleState, ambulance) >= 0) {
                    continue;
                }
                if (blockers.length() > 0) {
                    blockers.append(';');
                }
                blockers.append(vehicleState.vehicleId)
                        .append("@batch")
                        .append(batchIndexOf(bag, vehicleState.vehicleId));
            }
            System.out.println("[AMBULANCE_SCHED] " + ambulance.vehicleId + " batch="
                    + ambulanceBatchIndex + " blockersAhead=" + blockers);
        }

        System.out.println("[LEADER] Built OrderBag epoch=" + epoch + " batches=" + bag.batches.size()
                + " totalCars=" + placed.size());
        return bag;
    }

    private static boolean allSameLaneFrontPlaced(
            VehicleState candidate, Map<String, VehicleState> view, Set<String> placed) {
        for (VehicleState vehicleState : view.values()) {
            if (!vehicleState.lane.equals(candidate.lane)) {
                continue;
            }
            if (IntersectionTypes.compareLaneQueueOrder(vehicleState, candidate) < 0
                    && !placed.contains(vehicleState.vehicleId)) {
                return false;
            }
        }
        return true;
    }

    static int batchIndexOf(OrderBag bag, String vehicleId) {
        for (int i = 0; i < bag.batches.size(); i++) {
            if (bag.batches.get(i).vehicleIds.contains(vehicleId)) {
                return i;
            }
        }
        return -1;
    }

    private OrderScheduler() {}
}

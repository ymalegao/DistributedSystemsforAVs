package bftsmart.demo.intersection;

import java.io.Serializable;
import java.util.ArrayList;
import java.util.List;

public final class IntersectionTypes {

    /** Parse "vehN" suffix; numeric order so veh9 sorts before veh10. */
    static int vehicleIdNumericOrder(String vehicleId) {
        if (vehicleId == null || !vehicleId.startsWith("veh")) {
            return 0;
        }
        try {
            return Integer.parseInt(vehicleId.substring(3));
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    /**
     * Total order along a lane: lower positionInLane is closer to the intersection.
     * Ties break by numeric vehicle id so the queue order is deterministic.
     */
    static int compareLaneQueueOrder(VehicleState a, VehicleState b) {
        int byPosition = Integer.compare(a.positionInLane, b.positionInLane);
        if (byPosition != 0) {
            return byPosition;
        }
        return Integer.compare(vehicleIdNumericOrder(a.vehicleId), vehicleIdNumericOrder(b.vehicleId));
    }

    private IntersectionTypes() {}
}

final class ViewProposal implements Serializable {
    int proposerReplicaId;
    List<VehicleState> vehicleStates = new ArrayList<>();
    List<ViewSignature> v2vSignatures = new ArrayList<>();
}

final class ViewSignature implements Serializable {
    int signingReplicaId;
    byte[] signatureBytes;
}

final class VehicleState implements Serializable {
    String vehicleId;
    String lane;
    int positionInLane;
    String direction;
    boolean isAmbulance;
    /** "SIGNED" if this vehicle's replica sent a VIEW_AGREEMENT vote; "QUIET" otherwise. */
    String cyberStatus = "SIGNED";

    /** Extracts the numeric replica id from the "vehN" vehicleId format. */
    int replicaId() {
        return Integer.parseInt(vehicleId.substring(3));
    }
}

final class Batch implements Serializable {
    final List<String> vehicleIds = new ArrayList<>();
}

final class OrderBag implements Serializable {
    final int epoch;
    final List<Batch> batches = new ArrayList<>();

    OrderBag(int epoch) {
        this.epoch = epoch;
    }
}

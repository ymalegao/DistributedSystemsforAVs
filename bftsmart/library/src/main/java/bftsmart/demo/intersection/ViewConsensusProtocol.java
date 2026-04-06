package bftsmart.demo.intersection;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import net.jpountz.xxhash.XXHash32;
import net.jpountz.xxhash.XXHashFactory;

final class ViewConsensusProtocol {
    private static final XXHash32 XXHASH = XXHashFactory.fastestInstance().hash32();

    /**
     * Parse the semicolon-delimited VehicleState records from a VIEW_PROPOSE payload.
     * Format: "veh0|N|1|S|0;veh1|S|1|L|0;veh2|W|2|R|1"
     */
    static List<VehicleState> parseVehicleStates(String vehicleStatesStr) {
        List<VehicleState> states = new ArrayList<>();
        if (vehicleStatesStr == null || vehicleStatesStr.isEmpty()) {
            return states;
        }

        for (String record : vehicleStatesStr.split(";")) {
            String[] fields = record.trim().split("\\|");
            if (fields.length < 5) {
                System.err.println("[VIEW] Malformed VehicleState record: " + record);
                continue;
            }
            try {
                VehicleState vehicleState = new VehicleState();
                vehicleState.vehicleId = fields[0].trim();
                vehicleState.lane = fields[1].trim();
                vehicleState.positionInLane = Integer.parseInt(fields[2].trim());
                vehicleState.direction = fields[3].trim();
                vehicleState.isAmbulance = "1".equals(fields[4].trim());
                states.add(vehicleState);
            } catch (Exception e) {
                System.err.println("[VIEW] Error parsing VehicleState record '" + record + "': " + e.getMessage());
            }
        }
        return states;
    }

    static String serializeVehicleStates(List<VehicleState> states) {
        StringBuilder sb = new StringBuilder();
        for (VehicleState vehicleState : states) {
            if (sb.length() > 0) {
                sb.append(';');
            }
            sb.append(vehicleState.vehicleId).append('|')
                    .append(vehicleState.lane).append('|')
                    .append(vehicleState.positionInLane).append('|')
                    .append(vehicleState.direction).append('|')
                    .append(vehicleState.isAmbulance ? '1' : '0');
        }
        return sb.toString();
    }

    static List<ViewSignature> parseViewSignatures(String sigString) {
        List<ViewSignature> signatures = new ArrayList<>();
        if (sigString == null || sigString.isEmpty()) {
            return signatures;
        }

        for (String sigPart : sigString.split("\\|")) {
            String[] fields = sigPart.split(",");
            if (fields.length < 2) {
                continue;
            }
            try {
                ViewSignature signature = new ViewSignature();
                signature.signingReplicaId = Integer.parseInt(fields[0].trim());
                long hashValue = Long.parseLong(fields[1].trim());
                ByteBuffer buffer = ByteBuffer.allocate(4);
                buffer.order(ByteOrder.LITTLE_ENDIAN);
                buffer.putInt((int) hashValue);
                signature.signatureBytes = buffer.array();
                signatures.add(signature);
            } catch (Exception e) {
                System.err.println("[VIEW] Error parsing view signature: " + e.getMessage());
            }
        }
        return signatures;
    }

    static boolean validateViewProposal(ViewProposal proposal) {
        int groupSize = proposal.vehicleStates.size();
        int faultTolerance = (groupSize - 1) / 3;

        if (proposal.v2vSignatures.size() < faultTolerance + 1) {
            System.err.println("[VIEW] Insufficient V2V signatures: " + proposal.v2vSignatures.size()
                    + " (need " + (faultTolerance + 1) + ")");
            return false;
        }

        String vehicleStatesString = serializeVehicleStates(proposal.vehicleStates);
        for (ViewSignature signature : proposal.v2vSignatures) {
            if (!verifyViewSignature(vehicleStatesString, signature)) {
                System.err.println("[VIEW] Invalid V2V signature from replica " + signature.signingReplicaId);
                return false;
            }
        }
        return true;
    }

    private static boolean verifyViewSignature(String vehicleStatesStr, ViewSignature signature) {
        String input = vehicleStatesStr + ":" + signature.signingReplicaId;
        byte[] dataBytes = input.getBytes(StandardCharsets.UTF_8);
        int expectedHash = XXHASH.hash(dataBytes, 0, dataBytes.length, 0);

        if (signature.signatureBytes == null || signature.signatureBytes.length < 4) {
            return false;
        }

        ByteBuffer buffer = ByteBuffer.wrap(signature.signatureBytes);
        buffer.order(ByteOrder.LITTLE_ENDIAN);
        int actualHash = buffer.getInt();

        System.out.println("[VIEW_VERIFY] Replica " + signature.signingReplicaId
                + " input=\"" + input + "\" expected=" + expectedHash
                + " actual=" + actualHash + " match=" + (expectedHash == actualHash));
        return expectedHash == actualHash;
    }

    private ViewConsensusProtocol() {}
}

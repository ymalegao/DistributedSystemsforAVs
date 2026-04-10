package bftsmart.demo.intersection;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
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
                // 6th field (optional): cyberStatus = "SIGNED" or "QUIET"
                if (fields.length >= 6 && !fields[5].trim().isEmpty()) {
                    vehicleState.cyberStatus = fields[5].trim();
                }
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

    /**
     * Parse the per-car certs string from a PROPOSE_ALL payload.
     * Format: "veh0~r1,12345|r2,67890;veh1~r0,11111|r1,22222"
     * Returns Map&lt;vehicleId, List&lt;int[2]&gt;&gt; where int[2] = {replicaId, xxhashDecimal}
     */
    static Map<String, List<int[]>> parsePerCarCerts(String certsStr) {
        Map<String, List<int[]>> result = new HashMap<>();
        if (certsStr == null || certsStr.isEmpty()) return result;
        for (String carEntry : certsStr.split(";")) {
            String[] parts = carEntry.split("~", 2);
            if (parts.length < 2) continue;
            String carId = parts[0].trim();
            List<int[]> sigs = new ArrayList<>();
            for (String sigEntry : parts[1].split("\\|")) {
                String[] rv = sigEntry.split(",", 2);
                if (rv.length < 2) continue;
                try {
                    int rId   = Integer.parseInt(rv[0].trim());
                    int hash  = (int) Long.parseLong(rv[1].trim());  // int32 stored as decimal
                    sigs.add(new int[]{rId, hash});
                } catch (NumberFormatException ignored) {}
            }
            result.put(carId, sigs);
        }
        return result;
    }

    /**
     * Validates that each SIGNED car has f+1 valid per-car echo signatures.
     * Echo signature: XXHash32(carId:lane:pos:dir:isAmb:replicaId)
     * where dir = "L"/"R"/"S" and isAmb = "1"/"0".
     * f = (numCars - 1) / 3
     */
    static boolean validatePerCarCerts(List<VehicleState> states,
                                       Map<String, List<int[]>> certs) {
        int n = states.size();
        int f = (n - 1) / 3;
        int required = f + 1;
        for (VehicleState vs : states) {
            if (!"SIGNED".equals(vs.cyberStatus)) continue;
            List<int[]> sigs = certs.get(vs.vehicleId);
            if (sigs == null || sigs.isEmpty()) {
                System.err.println("[CERT-VERIFY] " + vs.vehicleId
                        + " is SIGNED but has no certs");
                return false;
            }
            Set<Integer> seen = new HashSet<>();
            int validCount = 0;
            for (int[] rv : sigs) {
                int rId = rv[0]; int claimedHash = rv[1];
                if (!seen.add(rId)) continue;  // dedup
                // C++ signs: XXHash32(carId:lane:pos:dir:isAmb:echoingReplicaId)
                String isAmbStr = vs.isAmbulance ? "1" : "0";
                String toSign = vs.vehicleId + ":" + vs.lane + ":"
                        + vs.positionInLane + ":" + vs.direction + ":"
                        + isAmbStr + ":" + rId;
                byte[] data = toSign.getBytes(StandardCharsets.UTF_8);
                int expected = XXHASH.hash(data, 0, data.length, 0);
                System.out.println("[CERT-VERIFY] " + vs.vehicleId + " echo from r" + rId
                        + " toSign=\"" + toSign + "\" expected=" + expected
                        + " claimed=" + claimedHash + " match=" + (expected == claimedHash));
                if (expected == claimedHash) validCount++;
            }
            if (validCount < required) {
                System.err.println("[CERT-VERIFY] " + vs.vehicleId + " has only "
                        + validCount + " valid sigs (need " + required + ")");
                return false;
            }
        }
        return true;
    }

    private ViewConsensusProtocol() {}
}

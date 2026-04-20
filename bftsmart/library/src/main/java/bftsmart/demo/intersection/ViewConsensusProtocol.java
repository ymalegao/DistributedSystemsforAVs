package bftsmart.demo.intersection;

import java.math.BigInteger;
import java.nio.charset.StandardCharsets;
import java.security.AlgorithmParameters;
import java.security.KeyFactory;
import java.security.PublicKey;
import java.security.Signature;
import java.security.spec.ECGenParameterSpec;
import java.security.spec.ECParameterSpec;
import java.security.spec.ECPoint;
import java.security.spec.ECPublicKeySpec;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * Witness-echo (ARRIVAL_CERT) verification for the V2V consensus layer.
 *
 * <p>Each per-car cert is a list of {@link EchoSig}s: {replicaId, uncompressed
 * P-256 pubkey, DER-encoded ECDSA signature}. The signed message is the same
 * UTF-8 string the C++ replica signs:
 * {@code carId:lane:positionInLane:direction:isAmbulance:replicaId}.
 *
 * <p>Algorithm: {@code SHA256withECDSA} over secp256r1 (NIST P-256). This
 * mirrors the IEEE 1609.2 / ETSI TS 103 097 V2X security profile already used
 * by the ambulance Emergency_CA cert path on the C++ side. The previous
 * XXHash32 MAC has been retired here because it was forgeable by anyone who
 * knew the canonical message format and replicaId.
 */
final class ViewConsensusProtocol {

    /** Uncompressed P-256 public-key length: 0x04 || X(32) || Y(32). */
    private static final int P256_PUBKEY_LEN = 65;

    /** Cached secp256r1 (NIST P-256) parameter spec. */
    private static final ECParameterSpec EC_P256_PARAMS;
    static {
        try {
            AlgorithmParameters params = AlgorithmParameters.getInstance("EC");
            params.init(new ECGenParameterSpec("secp256r1"));
            EC_P256_PARAMS = params.getParameterSpec(ECParameterSpec.class);
        } catch (Exception e) {
            throw new ExceptionInInitializerError(
                    "Failed to initialize secp256r1 parameters: " + e.getMessage());
        }
    }

    /** A single witness echo signature attached to a per-car ARRIVAL_CERT. */
    static final class EchoSig {
        final int replicaId;
        final byte[] pubKey;     // 65-byte uncompressed P-256 public key
        final byte[] signature;  // DER-encoded ECDSA signature

        EchoSig(int replicaId, byte[] pubKey, byte[] signature) {
            this.replicaId = replicaId;
            this.pubKey = pubKey;
            this.signature = signature;
        }
    }

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
     * Parse the per-car ECDSA cert string from a PROPOSE_ALL payload.
     *
     * <p>Format: {@code "veh0~r1,pubKeyHex1,sigHex1|r2,pubKeyHex2,sigHex2;veh1~..."}.
     * Each echo entry is {@code replicaId,uncompressedP256PubKeyHex,DERECDSASigHex}.
     * Malformed entries are skipped (validation will fail naturally for under-quorum).
     */
    static Map<String, List<EchoSig>> parsePerCarCerts(String certsStr) {
        Map<String, List<EchoSig>> result = new HashMap<>();
        if (certsStr == null || certsStr.isEmpty()) return result;
        for (String carEntry : certsStr.split(";")) {
            String[] parts = carEntry.split("~", 2);
            if (parts.length < 2) continue;
            String carId = parts[0].trim();
            List<EchoSig> sigs = new ArrayList<>();
            for (String sigEntry : parts[1].split("\\|")) {
                String[] fields = sigEntry.split(",", 3);
                if (fields.length < 3) continue;
                try {
                    int rId = Integer.parseInt(fields[0].trim());
                    byte[] pubKey = hexToBytes(fields[1].trim());
                    byte[] sigBytes = hexToBytes(fields[2].trim());
                    sigs.add(new EchoSig(rId, pubKey, sigBytes));
                } catch (Exception ignored) {
                    // Malformed echo; ignore. validatePerCarCerts will enforce f+1 valid.
                }
            }
            result.put(carId, sigs);
        }
        return result;
    }

    /**
     * Validates that each SIGNED car has f+1 valid per-car echo signatures.
     *
     * <p>Each echo is an ECDSA P-256 (SHA256withECDSA) signature, by the
     * echoing replica's per-replica keypair, over the canonical UTF-8 string
     * {@code carId:lane:pos:dir:isAmb:replicaId} (dir = "L"/"R"/"S",
     * isAmb = "1"/"0"). f = (numCars - 1) / 3.
     */
    static boolean validatePerCarCerts(List<VehicleState> states,
                                       Map<String, List<EchoSig>> certs) {
        int n = states.size();
        int f = (n - 1) / 3;
        int required = f + 1;
        for (VehicleState vs : states) {
            if (!"SIGNED".equals(vs.cyberStatus)) continue;
            List<EchoSig> sigs = certs.get(vs.vehicleId);
            if (sigs == null || sigs.isEmpty()) {
                System.err.println("[CERT-VERIFY] " + vs.vehicleId
                        + " is SIGNED but has no certs");
                return false;
            }
            Set<Integer> seen = new HashSet<>();
            int validCount = 0;
            for (EchoSig echo : sigs) {
                if (!seen.add(echo.replicaId)) continue;  // dedup
                String isAmbStr = vs.isAmbulance ? "1" : "0";
                String toSign = vs.vehicleId + ":" + vs.lane + ":"
                        + vs.positionInLane + ":" + vs.direction + ":"
                        + isAmbStr + ":" + echo.replicaId;
                byte[] data = toSign.getBytes(StandardCharsets.UTF_8);
                boolean ok = verifyEcdsaP256(echo.pubKey, data, echo.signature);
                System.out.println("[CERT-VERIFY] " + vs.vehicleId + " echo from r"
                        + echo.replicaId + " toSign=\"" + toSign
                        + "\" sigLen=" + echo.signature.length
                        + " match=" + ok);
                if (ok) validCount++;
            }
            if (validCount < required) {
                System.err.println("[CERT-VERIFY] " + vs.vehicleId + " has only "
                        + validCount + " valid sigs (need " + required + ")");
                return false;
            }
        }
        return true;
    }

    // ------------------------------------------------------------------------
    // ECDSA P-256 verification helpers
    // ------------------------------------------------------------------------

    /**
     * Verify a SHA256withECDSA signature using a raw uncompressed P-256 public key.
     * Returns false on any decoding/verification failure (never throws).
     */
    private static boolean verifyEcdsaP256(byte[] uncompressedPubKey, byte[] data, byte[] sig) {
        if (uncompressedPubKey == null || uncompressedPubKey.length != P256_PUBKEY_LEN
                || uncompressedPubKey[0] != 0x04) {
            return false;
        }
        if (sig == null || sig.length == 0) {
            return false;
        }
        try {
            PublicKey pub = decodeUncompressedP256(uncompressedPubKey);
            Signature verifier = Signature.getInstance("SHA256withECDSA");
            verifier.initVerify(pub);
            verifier.update(data);
            return verifier.verify(sig);
        } catch (Exception e) {
            return false;
        }
    }

    /** Reconstruct an ECPublicKey from a 65-byte uncompressed secp256r1 encoding. */
    private static PublicKey decodeUncompressedP256(byte[] uncompressed) throws Exception {
        byte[] xBytes = new byte[32];
        byte[] yBytes = new byte[32];
        System.arraycopy(uncompressed, 1, xBytes, 0, 32);
        System.arraycopy(uncompressed, 33, yBytes, 0, 32);
        BigInteger x = new BigInteger(1, xBytes);
        BigInteger y = new BigInteger(1, yBytes);
        ECPoint point = new ECPoint(x, y);
        ECPublicKeySpec spec = new ECPublicKeySpec(point, EC_P256_PARAMS);
        return KeyFactory.getInstance("EC").generatePublic(spec);
    }

    /** Lower-case ASCII hex -> bytes. Tolerant of odd length (truncates trailing nibble). */
    private static byte[] hexToBytes(String hex) {
        int len = hex.length() & ~1;  // round down to even
        byte[] out = new byte[len / 2];
        for (int i = 0; i < len; i += 2) {
            int hi = Character.digit(hex.charAt(i), 16);
            int lo = Character.digit(hex.charAt(i + 1), 16);
            if (hi < 0 || lo < 0) {
                throw new NumberFormatException("Bad hex at " + i);
            }
            out[i / 2] = (byte) ((hi << 4) | lo);
        }
        return out;
    }

    private ViewConsensusProtocol() {}
}

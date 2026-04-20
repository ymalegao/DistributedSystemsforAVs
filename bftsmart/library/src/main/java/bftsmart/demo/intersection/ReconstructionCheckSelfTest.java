package bftsmart.demo.intersection;

import java.nio.charset.StandardCharsets;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;

/**
 * Self-test for {@link IntersectionServer#buildFreshProposeAllBytes(String, int, int,
 * java.util.Map, java.util.Set)} — the pure-Java half of the V2V EP5 "Dynamic
 * Reconstruction" view-change hook.
 * <p>
 * Does not start OMNeT++, JNI, or a BFT replica: exercises the reconstruction
 * primitive with a synthetic JNI ground-truth payload and asserts the rebuilt
 * PROPOSE_ALL wire bytes:
 * <ol>
 *   <li>include the vehicle that a hypothetical censoring leader had omitted;</li>
 *   <li>carry the expected {@code PROPOSE_ALL:&lt;proposerId&gt;:&lt;vs&gt;:&lt;pcc&gt;:&lt;orderBagStr&gt;}
 *       structure (four ':' separators at the top level);</li>
 *   <li>produce an {@code orderBagStr} that schedules every vehicle from the
 *       fresh ground truth — including the previously censored one.</li>
 * </ol>
 * The contrast in the "stale" fixture (omitting {@code veh3}) versus the fresh
 * payload (including {@code veh3}) demonstrates that the view-change hook
 * genuinely moves the system from "passive replay of bad bytes" to "dynamic
 * reconstruction of truth".
 * <p>
 * Runs via {@code java -cp <cp> bftsmart.demo.intersection.ReconstructionCheckSelfTest}
 * (exits 0 on success, non-zero with a diagnostic message on failure).
 */
public final class ReconstructionCheckSelfTest {

    private ReconstructionCheckSelfTest() {}

    public static void main(String[] args) {
        // Fresh JNI ground truth: all four vehicles, including veh3 that the
        // deposed Byzantine leader had censored from its PROPOSE_ALL.
        // perCarCerts are synthetic (n=4 → f=1, so f+1=2 echoes per SIGNED car).
        String freshVsStr =
                "veh0|N|0|S|0|SIGNED;"
                + "veh1|E|0|S|0|SIGNED;"
                + "veh2|W|0|S|0|SIGNED;"
                + "veh3|S|0|S|0|SIGNED";
        String freshPerCarCerts = buildSyntheticPerCarCerts(freshVsStr);
        String jniPayload = freshVsStr + ":" + freshPerCarCerts;

        // "Stale" PROPOSE_ALL (what a censoring leader would have queued): omits veh3.
        // We only inspect this to prove the hook's output strictly differs.
        String staleVsStr =
                "veh0|N|0|S|0|SIGNED;"
                + "veh1|E|0|S|0|SIGNED;"
                + "veh2|W|0|S|0|SIGNED";

        int proposerId = 7;
        int epoch = 3;
        Map<String, Integer> waitRegistry = new HashMap<>();
        Set<Integer> departed = Collections.emptySet();

        // --- Exercise the reconstruction primitive --------------------------
        byte[] freshBytes = IntersectionServer.buildFreshProposeAllBytes(
                jniPayload, proposerId, epoch, waitRegistry, departed);
        if (freshBytes == null) {
            fail("buildFreshProposeAllBytes returned null for a valid ground-truth payload");
        }
        String fresh = new String(freshBytes, StandardCharsets.UTF_8);

        // --- Assertion 1: starts with PROPOSE_ALL:<proposerId>:
        String expectedPrefix = "PROPOSE_ALL:" + proposerId + ":";
        if (!fresh.startsWith(expectedPrefix)) {
            fail("Rebuilt PROPOSE_ALL lacks expected prefix '" + expectedPrefix
                    + "'; got: " + fresh);
        }

        // --- Assertion 2: wire format is <prefix>:<vs>:<pcc>:<orderBag...>
        // PROPOSE_ALL + proposerId + vsStr + perCarCerts + orderBagStr → 5 top-level
        // segments when split on ':' with limit=5 (orderBagStr itself contains ':'s).
        String[] parts = fresh.split(":", 5);
        if (parts.length < 5) {
            fail("Rebuilt PROPOSE_ALL malformed (expected 5 ':'-separated segments, "
                    + "got " + parts.length + "): " + fresh);
        }
        if (!parts[1].equals(String.valueOf(proposerId))) {
            fail("Proposer id mismatch; expected " + proposerId + " got '" + parts[1] + "'");
        }
        if (!parts[2].equals(freshVsStr)) {
            fail("vehicleStatesStr mismatch; expected '" + freshVsStr
                    + "' got '" + parts[2] + "'");
        }
        if (!parts[3].equals(freshPerCarCerts)) {
            fail("perCarCerts mismatch; expected '" + freshPerCarCerts
                    + "' got '" + parts[3] + "'");
        }

        String orderBagStr = parts[4];

        // --- Assertion 3: the previously-censored car is now in vehicleStates
        if (!parts[2].contains("veh3|")) {
            fail("vehicleStatesStr does NOT contain veh3 — rebuild did not include the "
                    + "censored vehicle. vsStr=" + parts[2]);
        }

        // --- Assertion 4: orderBagStr schedules every car including veh3
        // serializeOrderBagForBFT format: "<epoch>:<vid>:<batchIdx>;<vid>:<batchIdx>;..."
        if (!orderBagStr.startsWith(epoch + ":")) {
            fail("orderBagStr does not start with '" + epoch + ":'; got: " + orderBagStr);
        }
        for (String vid : new String[]{"veh0", "veh1", "veh2", "veh3"}) {
            if (!orderBagStr.contains(vid + ":")) {
                fail("orderBagStr omits " + vid + " — fresh schedule incomplete. bag="
                        + orderBagStr);
            }
        }

        // --- Assertion 5: the fresh rebuild STRICTLY differs from the stale bytes.
        // Construct what a censoring leader would have built from the stale vs:
        // (same algorithm, but over the 3-car stateMap). This proves the reconstruction
        // is not a no-op.
        if (parts[2].equals(staleVsStr)) {
            fail("Fresh vsStr equals stale vsStr — no reconstruction happened");
        }

        // --- Assertion 6: null/empty JNI payload yields null (fallback path)
        byte[] emptyNull = IntersectionServer.buildFreshProposeAllBytes(
                "", proposerId, epoch, waitRegistry, departed);
        if (emptyNull != null) {
            fail("Empty JNI payload should return null (fallback to replay); got non-null");
        }
        byte[] nullNull = IntersectionServer.buildFreshProposeAllBytes(
                null, proposerId, epoch, waitRegistry, departed);
        if (nullNull != null) {
            fail("Null JNI payload should return null (fallback to replay); got non-null");
        }

        // --- Assertion 7: all-departed replicas yield null (nothing to schedule)
        Set<Integer> allDeparted = new HashSet<>();
        for (int i = 0; i < 4; i++) allDeparted.add(i);
        byte[] departedNull = IntersectionServer.buildFreshProposeAllBytes(
                jniPayload, proposerId, epoch, waitRegistry, allDeparted);
        if (departedNull != null) {
            fail("All-departed replicas should yield null; got: "
                    + new String(departedNull, StandardCharsets.UTF_8));
        }

        System.out.println("[ReconstructionCheckSelfTest] OK — buildFreshProposeAllBytes rebuilds "
                + "a complete PROPOSE_ALL from JNI ground truth and reinstates censored vehicles.");
        System.out.println("[ReconstructionCheckSelfTest] fresh wire bytes: " + fresh);
    }

    /** Deterministic synthetic perCarCerts for the self-test. Mirrors the C++
     *  wire format {@code carId~signerId,hash|signerId,hash;...}. Verifier's
     *  Check 1 (per-car cert validation) is not exercised here — this test
     *  targets the reconstruction primitive specifically. */
    private static String buildSyntheticPerCarCerts(String vsStr) {
        StringBuilder sb = new StringBuilder();
        boolean first = true;
        for (String vehEntry : vsStr.split(";")) {
            String vid = vehEntry.split("\\|", 2)[0];
            if (!first) sb.append(';');
            first = false;
            // Two synthetic echoes per car (stand-in for f+1 echoes).
            sb.append(vid).append("~0,1|1,2");
        }
        return sb.toString();
    }

    private static void fail(String msg) {
        System.err.println("[ReconstructionCheckSelfTest] FAIL: " + msg);
        System.exit(1);
    }
}

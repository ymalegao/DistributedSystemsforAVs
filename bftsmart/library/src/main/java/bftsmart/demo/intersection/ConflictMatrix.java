package bftsmart.demo.intersection;

import java.util.Collections;
import java.util.HashSet;
import java.util.Set;

/**
 * Standard 4-way intersection conflict matrix.
 *
 * A movement pair is "safe" when both vehicles can traverse simultaneously
 * without collision.  Rules are symmetric and hardcoded from standard
 * traffic-engineering principles for a signalised 4-way intersection.
 *
 * Movement key format: "<Lane><Dir>"
 *   Lane : N, S, E, W
 *   Dir  : S (Straight), L (Left turn), R (Right turn)
 * Example: "NS" = North-Straight, "EL" = East-Left, "WR" = West-Right
 */
public final class ConflictMatrix {

    /** Normalised pair key — alphabetical order, double-underscore separator. */
    private static String key(String a, String b) {
        return (a.compareTo(b) <= 0) ? a + "__" + b : b + "__" + a;
    }

    private static final Set<String> SAFE;

    static {
        Set<String> s = new HashSet<>();

        // --- Opposite-direction straights (parallel lanes, no path crossing) ---
        s.add(key("NS", "SS")); // North-Straight + South-Straight
        s.add(key("ES", "WS")); // East-Straight  + West-Straight

        // --- All right-turn pairs (short hooks, do not enter the central box) ---
        s.add(key("NR", "SR"));
        s.add(key("NR", "ER"));
        s.add(key("NR", "WR"));
        s.add(key("SR", "ER"));
        s.add(key("SR", "WR"));
        s.add(key("ER", "WR"));

        // --- Right turn + opposite straight (exit mouths must not clash) ---
        // N-Right exits East;  S-Straight exits North  → different mouths → SAFE
        s.add(key("NR", "SS"));
        // S-Right exits West;  N-Straight exits South  → different mouths → SAFE
        s.add(key("SR", "NS"));
        // E-Right exits South; W-Straight exits East   → different mouths → SAFE
        s.add(key("ER", "WS"));
        // W-Right exits North; E-Straight exits West   → different mouths → SAFE
        s.add(key("WR", "ES"));

        // Everything else is unsafe:
        //   • Perpendicular straights (T-bone)
        //   • Any left turn (sweeps the central box)
        //   • Right turn vs perpendicular straight sharing an exit mouth
        //     (e.g. N-Right exits East; E-Straight also uses the East entry — merge conflict)
        //   • Same-lane pairs (rear-end) — caught separately in isSafeToBatch()

        SAFE = Collections.unmodifiableSet(s);
    }

    /**
     * Returns {@code true} if vehicles {@code a} and {@code b} can cross
     * simultaneously without collision risk.
     */
    public static boolean isSafeToBatch(IntersectionServer.VehicleState a,
                                        IntersectionServer.VehicleState b) {
        if (a.lane.equals(b.lane)) return false; // same lane → rear-end risk
        String keyA = a.lane + dirCode(a.direction);
        String keyB = b.lane + dirCode(b.direction);
        return SAFE.contains(key(keyA, keyB));
    }

    private static String dirCode(String direction) {
        if (direction == null) return "S";
        switch (direction) {
            case "L": return "L";
            case "R": return "R";
            default:  return "S"; // "S" or unknown → treat as Straight
        }
    }

    private ConflictMatrix() {}
}

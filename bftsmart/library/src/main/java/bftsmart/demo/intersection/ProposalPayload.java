package bftsmart.demo.intersection;

/**
 * Immutable value type for the PROPOSE_ALL wire payload.
 *
 * Full payload (after stripping the "PROPOSE_ALL:" prefix, post-schedule):
 *   {@code <proposerStr>:<vehicleStatesStr>:<perCarCertsStr>:<orderBagStr>}
 *
 * Incoming C++ format (full wire string, pre-schedule):
 *   {@code PROPOSE_ALL:<proposerStr>:<vehicleStatesStr>:<perCarCertsStr>}
 */
final class ProposalPayload {
    final String proposerStr;
    final String vehicleStatesStr;
    final String perCarCertsStr;
    /** Null for incoming C++ packets (no schedule yet). */
    final String orderBagStr;

    private ProposalPayload(String proposerStr, String vehicleStatesStr,
                            String perCarCertsStr, String orderBagStr) {
        this.proposerStr = proposerStr;
        this.vehicleStatesStr = vehicleStatesStr;
        this.perCarCertsStr = perCarCertsStr;
        this.orderBagStr = orderBagStr;
    }

    /**
     * Parses the payload after stripping the {@code "PROPOSE_ALL:"} prefix.
     * Format: {@code <proposerStr>:<vehicleStatesStr>:<perCarCertsStr>:<orderBagStr>}
     * Returns {@code null} if the payload is null or has fewer than 4 colon-delimited parts.
     */
    static ProposalPayload parse(String payload) {
        if (payload == null) return null;
        String[] p = payload.split(":", 4);
        if (p.length < 4) return null;
        return new ProposalPayload(p[0], p[1], p[2], p[3]);
    }

    /**
     * Parses from the full PROPOSE_ALL wire request sent by C++ (no schedule appended yet).
     * Format: {@code PROPOSE_ALL:<proposerStr>:<vehicleStatesStr>:<perCarCertsStr>}
     * Returns {@code null} if the request is malformed or does not start with {@code "PROPOSE_ALL"}.
     */
    static ProposalPayload parseIncoming(String request) {
        if (request == null) return null;
        String[] p = request.split(":", 4);
        if (p.length < 4 || !"PROPOSE_ALL".equals(p[0])) return null;
        return new ProposalPayload(p[1], p[2], p[3], null);
    }
}

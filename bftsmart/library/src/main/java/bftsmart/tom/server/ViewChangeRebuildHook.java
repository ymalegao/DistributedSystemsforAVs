package bftsmart.tom.server;

import bftsmart.clientsmanagement.ClientsManager;

/**
 * Hook invoked on the new leader during BFT-SMaRt view-change recovery,
 * between {@code ClientsManager.resetAlreadyProposed()} and
 * {@code TOMLayer.createPropose()} inside
 * {@code Synchronizer.catch_up()}.
 * <p>
 * The default catch-up behavior replays whatever bytes sit in the pending
 * request queue — which is unsafe when the deposed leader was Byzantine
 * and the queued request is the censored payload that triggered the
 * view-change in the first place. Applications with a notion of fresh
 * "ground truth" (e.g. the V2V intersection protocol, whose C++
 * {@code collectedCerts} hold the physically-verified set of vehicles)
 * can use this hook to rebuild the pending request from current state
 * before {@code createPropose()} reads it, guaranteeing the new leader
 * does not just repeat the censored bytes.
 * <p>
 * The hook runs on the catch-up thread and must be synchronous: any
 * fresh request it injects must be visible to the immediately-following
 * {@link bftsmart.tom.core.TOMLayer#createPropose} call. Implementations
 * should be a no-op when they cannot produce a fresh proposal (e.g.
 * JNI unavailable in unit tests), which safely falls back to the prior
 * replay behavior and preserves the existing view-change semantics for
 * non-semantic failures (timeouts, silent leader, etc.).
 * <p>
 * This interface is optional; {@code ServiceReplica} tolerates a null
 * hook so the stock BFT-SMaRt demos are unaffected.
 */
public interface ViewChangeRebuildHook {

    /**
     * Called on the new leader inside {@code Synchronizer.catch_up()}.
     *
     * @param cm       the leader's {@code ClientsManager}; implementations may
     *                 evict stale pending requests and inject fresh ones via
     *                 {@code cm.requestReceived(...)} or an equivalent path.
     * @param regency  the new regency (leader epoch) being installed.
     */
    void rebuildPendingProposals(ClientsManager cm, int regency);

    /**
     * Called on EVERY replica at the top of {@code Synchronizer.finalise()}
     * during view-change recovery (i.e. on followers as well as the new
     * leader). Implementations should evict any stale pending request that
     * was carried over from the deposed leader's censored round so the
     * follower-side {@code RequestsTimer} does not fire ~timeout later and
     * trigger a redundant leader change.
     * <p>
     * The new leader has typically already evicted via
     * {@link #rebuildPendingProposals(ClientsManager, int)} in
     * {@code catch_up()}; calling this a second time on the leader is a
     * harmless no-op (the pending list is already empty). The default
     * implementation does nothing, preserving stock BFT-SMaRt behavior.
     *
     * @param cm       this replica's {@code ClientsManager}.
     * @param regency  the new regency (leader epoch) being installed.
     */
    default void evictStaleProposals(ClientsManager cm, int regency) {
        // no-op by default
    }
}

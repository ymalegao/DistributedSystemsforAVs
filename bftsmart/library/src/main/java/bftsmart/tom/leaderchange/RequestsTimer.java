/**
Copyright (c) 2007-2013 Alysson Bessani, Eduardo Alchieri, Paulo Sousa, and the authors indicated in the @author tags

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/
package bftsmart.tom.leaderchange;

import java.util.Iterator;
import java.util.LinkedList;
import java.util.ListIterator;
import java.util.Timer;
import java.util.TimerTask;
import java.util.TreeSet;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ThreadLocalRandom;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.locks.ReentrantReadWriteLock;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;

import bftsmart.communication.ServerCommunicationSystem;
import bftsmart.communication.V2V.SimulationClock;
import bftsmart.reconfiguration.ServerViewController;
import bftsmart.tom.core.TOMLayer;
import bftsmart.tom.core.messages.TOMMessage;
import bftsmart.tom.util.TOMUtil;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

/**
 * This thread serves as a manager for all timers of pending requests.
 *
 */
public class RequestsTimer {
    
    private Logger logger = LoggerFactory.getLogger(this.getClass());

    private Timer timer = new Timer("request timer");
    private RequestTimerTask rtTask = null;
    private TOMLayer tomLayer; // TOM layer
    private long timeout;
    private long shortTimeout;
    private TreeSet<TOMMessage> watched = new TreeSet<TOMMessage>();
    private ReentrantReadWriteLock rwLock = new ReentrantReadWriteLock();
    
    private boolean enabled = true;

    // Java-side wall-clock jitter is redundant: every BFT message goes through
    // C++ sendBFTMessage() which applies replicaId * broadcastSlotSec (5ms) +
    // uniform(broadcastJitterMin, broadcastJitterMax) in sim-time before hitting
    // the 802.11p medium. That provides a 75ms sim-time deterministic spread
    // across N=16 replicas — far better collision avoidance than a wall-clock
    // random delay. The Java jitter only delayed messages entering the JNI queue,
    // adding latency with no benefit. Default is 0; override via
    // `-Dbftsmart.lc_jitter_wall_ms=<N>` if needed for experiments.
    private static final long JITTER_WALL_MS =
            Long.getLong("bftsmart.lc_jitter_wall_ms", 0L);

    // How often SendStopTask wakes on the wall-clock Timer to check whether a
    // fresh STOP emission is due. Must be small so we sample sim-time often
    // enough under heavy load (where sim time runs 5–20× slower than wall
    // time), but not so small that the poll itself eats CPU.
    // Override via `-Dbftsmart.stop_retx_wall_ms=<N>`.
    private static final long STOP_RETX_WALL_MS =
            Long.getLong("bftsmart.stop_retx_wall_ms", 200L);

    // Minimum SIM-TIME gap between successive STOP emissions from the same
    // replica for the same regency. Under a Byzantine leader at N=12+, the
    // simulation runs roughly 10× slower than wall time during LC. A
    // wall-clock-scheduled re-emit every 1 s would then fire ~10×/sim-s per
    // replica, which means ~100+ STOP broadcasts/sim-s across the whole view.
    // That level of 802.11p channel saturation starves the ordered SYNC
    // retransmissions and makes replicas time out into reg=2 before SYNC
    // delivers. Gating on sim-time keeps the broadcast rate stable regardless
    // of sim:wall ratio.
    // Override via `-Dbftsmart.stop_retx_sim_ms=<N>`.
    private static final long STOP_RETX_SIM_MS =
            Long.getLong("bftsmart.stop_retx_sim_ms", 200L);

    // Number of blind full-STOP emissions per regency before SendStopTask
    // switches to NACK-driven mode. Under 802.11p loss at N=16 a single STOP
    // often doesn't seed enough peers to reach 2f+1, so we fire a handful of
    // blind STOPs to prime the network, then let the compact STOP_NACK
    // bitmask drive targeted resends for the long tail.
    //
    // History: was temporarily set to Integer.MAX_VALUE (NACK disabled) to
    // test whether a denser blind-only cadence would converge. It doesn't:
    // at 200ms sim-cadence × 14 honest emitters × ~64% delivery under
    // channel saturation the new leader systematically ends up 1–3 STOPs
    // short of 2f+1 and has no recovery mechanism. With NACK re-enabled,
    // replicas that are still short after STOP_BLIND_EMITS primings switch
    // to emitting a compact bitmask listing only the peers they haven't
    // heard from; honest peers (including those that have already advanced
    // past Phase 2 locally but not yet been cancelled by SYNC — their STOP
    // is still in currentStopByRegency) unicast their STOP back with a
    // ≤20ms wall jitter. This both (a) drains the missing tail and (b)
    // stays off-channel when the tail is empty, unlike blind broadcast.
    // Override via `-Dbftsmart.stop_blind_emits=<N>`.
    private static final int STOP_BLIND_EMITS =
            Integer.getInteger("bftsmart.stop_blind_emits", 3);

    // Byzantine-NACK DoS cap: a malicious replica could NACK everyone on
    // every regency with mask=0xFFFF. Bound how many times we honor a NACK
    // from a single peer per regency.
    //
    // History: was 3. Replies are now broadcast to all peers (not unicast
    // to the NACKer), so each reply is much more channel-efficient. However
    // under 802.11p saturation even broadcasts can be dropped, so we keep
    // several retries. At the same time, raising this too high risks
    // flooding — a Byzantine NACKer can trigger up to NACK_REPLIES_PER_PEER
    // full-STOP broadcasts from every other replica. 10 is a reasonable
    // middle ground: 10 independent broadcast attempts for each missing
    // pid in the NACKer's mask, deferred by random jitter, giving ample
    // coverage under moderate channel loss without excessive flooding.
    // Override via `-Dbftsmart.nack_replies_per_peer=<N>`.
    private static final int NACK_REPLIES_PER_PEER =
            Integer.getInteger("bftsmart.nack_replies_per_peer", 10);

    private ServerCommunicationSystem communication; // Communication system between replicas
    private ServerViewController controller; // Reconfiguration manager
    
    private HashMap <Integer, Timer> stopTimers = new HashMap<>();

    // Per-regency sim-time of the last actual STOP broadcast. Shared across
    // the one-shot SendStopTask instances so we throttle by sim-time rather
    // than firing on every wall-clock wakeup.
    private final ConcurrentHashMap<Integer, Long> lastStopSimEmitMs = new ConcurrentHashMap<>();

    // STOP currently being re-emitted per regency. We stash a reference here
    // so a received STOP_NACK can look up our original STOP and unicast it
    // back to the NACK sender without having to reach into SendStopTask.
    private final ConcurrentHashMap<Integer, LCMessage> currentStopByRegency = new ConcurrentHashMap<>();

    // Per-regency blind-STOP emission count. Kept outside SendStopTask so the
    // counter survives the one-shot task chain (each wake creates a new
    // task, but state belongs to the regency).
    private final ConcurrentHashMap<Integer, Integer> blindEmitCount = new ConcurrentHashMap<>();

    // Single-choke-point LC escalation debounce. {@link
    // bftsmart.tom.core.Synchronizer#triggerTimeout(java.util.List)} consults
    // this AT THE POINT IT IS ABOUT TO ADVANCE {@code nextReg}. Without this
    // check, {@link bftsmart.consensus.roles.Acceptor} can call triggerTimeout
    // directly on a null-propose condition, which bypasses
    // {@link #run_lc_protocol()}'s debounce and escalates reg=r → r+1 the
    // moment Phase 2 for r installs locally (because
    // {@link bftsmart.tom.core.Synchronizer#startSynchronization(int)} flips
    // {@link #Enabled(boolean)} back to true at line 544 AS SOON AS the
    // 2f+1 STOP quorum fires at the local replica, well before SYNC for r
    // propagates). In the failing N=16 run (22:47:22 log), replica 9 hit
    // distinct=11 for reg=1, sent STOPDATA, and within ~5 sim-s was emitting
    // STOP for reg=2 via exactly this Acceptor path.
    //
    // Sim-time gate provides a BFT-liveness escape: if reg=r genuinely
    // stalls (e.g., the new leader is also faulty), we must still allow
    // escalation eventually. LC_ESCALATION_GAP_SIM_MS should be long
    // enough to cover a successful LC end-to-end on 802.11p at the largest
    // view size we care about (empirically 8–12 sim-s at N=16), plus
    // margin.
    private volatile boolean lcEpochInFlight = false;
    private volatile long lcEpochStartedSimMs = 0L;
    private static final long LC_ESCALATION_GAP_SIM_MS =
            Long.getLong("bftsmart.lc_escalation_gap_sim_ms", 15000L);

    // regency -> (nackerPid -> number of resends already sent to that peer).
    // DoS cap; reset when the regency is installed via stopSTOP().
    private final ConcurrentHashMap<Integer, ConcurrentHashMap<Integer, Integer>> nackReplyCount = new ConcurrentHashMap<>();

    // Per-regency set of acceptor pids whose STOP we have actually observed
    // on the wire. Maintained independently of LCManager.stops because
    // Synchronizer.startSynchronization wipes LCManager.stops[reg] the
    // instant it crosses 2f+1 (line ~540: removeStops(nextReg)) to avoid
    // memory leaks — at which point getStopSenders() is worthless for
    // computing a missing-peers bitmask. This set is ONLY used by the
    // STOP_NACK transport; it never feeds the BFT quorum.
    private final ConcurrentHashMap<Integer, Set<Integer>> heardByRegency = new ConcurrentHashMap<>();

    // Shared daemon scheduler used to add a small wall-ms jitter between a
    // STOP_NACK arrival and our resend, preventing synchronized CSMA-CA
    // collisions when multiple honest replicas respond to the same NACK.
    // Single-thread is plenty: the work per firing is just a send call.
    private final ScheduledExecutorService nackReplyExec =
            Executors.newSingleThreadScheduledExecutor(r -> {
                Thread t = new Thread(r, "LC-NACK-reply");
                t.setDaemon(true);
                return t;
            });
    
    //private Storage st1 = new Storage(100000);
    //private Storage st2 = new Storage(10000);
    /**
     * Creates a new instance of RequestsTimer
     * @param tomLayer TOM layer
     */
    public RequestsTimer(TOMLayer tomLayer, ServerCommunicationSystem communication, ServerViewController controller) {
        this.tomLayer = tomLayer;
        
        this.communication = communication;
        this.controller = controller;
        
        this.timeout = this.controller.getStaticConf().getRequestTimeout();
        this.shortTimeout = -1;
    }

    public void setShortTimeout(long shortTimeout) {
        this.shortTimeout = shortTimeout;
    }

    /** Non-negative jitter in wall-ms, uniform over [0, JITTER_WALL_MS). */
    private static long jitter() {
        if (JITTER_WALL_MS <= 0) return 0L;
        return ThreadLocalRandom.current().nextLong(JITTER_WALL_MS);
    }

    public void startTimer() {
        if (rtTask == null) {
            long t = (shortTimeout > -1 ? shortTimeout : timeout);
            //shortTimeout = -1;
            rtTask = new RequestTimerTask();
            if (controller.getCurrentViewN() > 1) timer.schedule(rtTask, t + jitter());
        }
    }
    
    public void stopTimer() {
        if (rtTask != null) {
            rtTask.cancel();
            rtTask = null;
        }
    }
    
    public void Enabled(boolean phase) {
        
        enabled = phase;
    }
    
    public boolean isEnabled() {
    	return enabled;
    }
    
    /**
     * Creates a timer for the given request
     * @param request Request to which the timer is being createf for
     */
    public void watch(TOMMessage request) {
        //long startInstant = System.nanoTime();
        rwLock.writeLock().lock();
        watched.add(request);
        if (watched.size() >= 1 && enabled) startTimer();
        rwLock.writeLock().unlock();
    }

    /**
     * Cancels a timer for a given request
     * @param request Request whose timer is to be canceled
     */
    public void unwatch(TOMMessage request) {
        //long startInstant = System.nanoTime();
        rwLock.writeLock().lock();
        if (watched.remove(request) && watched.isEmpty()) stopTimer();
        rwLock.writeLock().unlock();
    }

    /**
     * Cancels all timers for all messages
     */
    public void clearAll() {
        TOMMessage[] requests = new TOMMessage[watched.size()];
        rwLock.writeLock().lock();
        
        watched.toArray(requests);

        for (TOMMessage request : requests) {
            if (request != null && watched.remove(request) && watched.isEmpty() && rtTask != null) {
                rtTask.cancel();
                rtTask = null;
            }
        }
        rwLock.writeLock().unlock();
    }
    
    public void run_lc_protocol() {

        long t = (shortTimeout > -1 ? shortTimeout : timeout);

        // Compare in SIM TIME. receptionTimestamp was stamped from
        // SimulationClock in ClientsManager, so this delta is sim-time-correct
        // even when wall time runs arbitrarily slower than sim time during
        // heavy BFT rounds (especially under Byzantine leaders).
        long nowSimMs = SimulationClock.currentTimeMillis();

        LinkedList<TOMMessage> pendingRequests = new LinkedList<>();

        try {

            rwLock.readLock().lock();

            for (Iterator<TOMMessage> i = watched.iterator(); i.hasNext();) {
                TOMMessage request = i.next();
                if ((nowSimMs - request.receptionTimestamp) > t) {
                    pendingRequests.add(request);
                }
            }

        } finally {

            rwLock.readLock().unlock();
        }

        if (!pendingRequests.isEmpty()) {

            logger.info("The following requests timed out: " + pendingRequests);

            for (ListIterator<TOMMessage> li = pendingRequests.listIterator(); li.hasNext(); ) {
                TOMMessage request = li.next();
                if (!request.timeout) {

                    logger.info("Forwarding requests {} to leader", request);

                    request.signed = request.serializedMessageSignature != null;
                    tomLayer.forwardRequestToLeader(request);
                    request.timeout = true;
                    li.remove();
                }
            }

            if (!pendingRequests.isEmpty()) {
                // No per-nextReg debounce here any more: the real escalation
                // guard lives inside {@link
                // bftsmart.tom.core.Synchronizer#triggerTimeout(java.util.List)},
                // where it ALSO covers the Acceptor null-propose entry path and
                // carries a sim-time-based BFT-liveness escape (if a regency
                // genuinely stalls for LC_ESCALATION_GAP_SIM_MS, the next call
                // is allowed through). Running a separate "once per nextReg"
                // filter at this layer would prevent that liveness escape,
                // because nextReg stops advancing exactly when reg=r is stuck.
                // triggerTimeout is cheap when escalation is suppressed (just
                // processOutOfContextSTOPs + startSynchronization), so calling
                // it on every matured wake is fine.
                logger.info("Attempting to start leader change for requests {}", pendingRequests);
                tomLayer.getSynchronizer().triggerTimeout(pendingRequests);
            }
            else {
                rtTask = new RequestTimerTask();
                timer.schedule(rtTask, t + jitter());
            }
        } else {

            logger.debug("Timeout triggered with no expired requests");

            rtTask = new RequestTimerTask();
            timer.schedule(rtTask, t + jitter());
        }

    }
    
    public void setSTOP(int regency, LCMessage stop) {

        stopSTOP(regency);

        SendStopTask stopTask = new SendStopTask(stop);
        Timer stopTimer = new Timer("Stop message");

        // Wall-clock schedule so the Timer fires often enough to sample
        // sim-time; the actual emission cadence is gated on SIM time inside
        // SendStopTask.run().
        stopTimer.schedule(stopTask, STOP_RETX_WALL_MS + jitter());

        stopTimers.put(regency, stopTimer);
    }

    /**
     * Reschedule the STOP wakeup for the given regency. Creates a fresh
     * {@link SendStopTask} each time (Java's {@link TimerTask} is single-use)
     * while preserving the sim-time emission state via
     * {@link #lastStopSimEmitMs}.
     */
    private void rescheduleSTOP(int regency, LCMessage stop) {
        stopSTOP(regency);
        SendStopTask stopTask = new SendStopTask(stop);
        Timer stopTimer = new Timer("Stop message");
        stopTimer.schedule(stopTask, STOP_RETX_WALL_MS + jitter());
        stopTimers.put(regency, stopTimer);
    }
    
    public void stopAllSTOPs() {
        Iterator stops = getTimers().iterator();
        while (stops.hasNext()) {
            stopSTOP((Integer) stops.next());
        }
    }
    
    public void stopSTOP(int regency){

        Timer stopTimer = stopTimers.remove(regency);
        if (stopTimer != null) stopTimer.cancel();
        // Note: we intentionally preserve lastStopSimEmitMs[regency] across
        // intra-regency restarts (rescheduleSTOP calls stopSTOP + setSTOP on
        // every wakeup) so the sim-time throttle holds. Entries for fully
        // installed regencies do leak — negligible: one long per regency.
        //
        // NACK-related per-regency state is only meaningful while the STOP
        // task is scheduled; the call site distinguishes "intra-regency
        // reschedule" (rescheduleSTOP) from "regency installed" by whether
        // rescheduleSTOP immediately re-puts the Timer. To avoid wiping
        // state on every wakeup we do NOT clear here; instead, the install
        // path (Synchronizer) should call dropRegencyState(regency).
    }

    /**
     * Release per-regency NACK and emit-count state once the regency is
     * installed. Safe to call more than once. Separate from {@link
     * #stopSTOP(int)} because stopSTOP is re-used on every intra-regency
     * rescheduleSTOP and we don't want to lose counts mid-regency.
     */
    public synchronized void dropRegencyState(int regency) {
        currentStopByRegency.remove(regency);
        blindEmitCount.remove(regency);
        nackReplyCount.remove(regency);
        lastStopSimEmitMs.remove(regency);
        heardByRegency.remove(regency);
        // Re-arm the escalation debounce: this LC episode has been fully
        // installed (SYNC delivered and consumed), so a future timeout for
        // some later regency is welcome to escalate again.
        lcEpochInFlight = false;
        lcEpochStartedSimMs = 0L;
    }

    /**
     * Atomic test-and-claim for initiating a leader-change escalation.
     * Called from {@link bftsmart.tom.core.Synchronizer#triggerTimeout} at
     * the point where {@code nextReg} is about to be advanced. Returns
     * {@code true} iff the caller has successfully claimed the right to
     * escalate; callers that receive {@code false} MUST NOT advance
     * {@code nextReg} and should return from triggerTimeout after running
     * their idempotent bookkeeping (processOutOfContextSTOPs +
     * startSynchronization).
     *
     * Semantics: one escalation per "LC epoch", with a sim-time-based
     * liveness escape after {@link #LC_ESCALATION_GAP_SIM_MS}. The epoch
     * ends when {@link #dropRegencyState(int)} fires from the SYNC install
     * path, which resets the in-flight flag.
     */
    public synchronized boolean tryClaimLCEpoch() {
        long now = SimulationClock.currentTimeMillis();
        if (lcEpochInFlight && (now - lcEpochStartedSimMs) < LC_ESCALATION_GAP_SIM_MS) {
            return false;
        }
        lcEpochInFlight = true;
        lcEpochStartedSimMs = now;
        return true;
    }

    /**
     * Called from {@link bftsmart.communication.MessageHandler} every time a
     * STOP is delivered (before the Synchronizer state machine may or may
     * not admit it into LCManager.stops). This is the authoritative source
     * of "peers I've heard a STOP from for this regency" from the transport
     * layer's point of view, used exclusively by the STOP_NACK path.
     */
    public void recordHeardStop(int regency, int fromPid) {
        if (regency < 0 || fromPid < 0) return;
        heardByRegency.computeIfAbsent(regency, r -> ConcurrentHashMap.newKeySet()).add(fromPid);
    }
    
    public Set<Integer> getTimers() {
        
        return ((HashMap <Integer,Timer>) stopTimers.clone()).keySet();
        
    }
    
    public void shutdown() {
        timer.cancel();
        stopAllSTOPs();
        nackReplyExec.shutdownNow();
        LoggerFactory.getLogger(this.getClass()).info("RequestsTimer stopped.");

    }

    /**
     * Receiver side of the STOP_NACK transport. Called from
     * {@code MessageHandler} when an {@link LCMessage} of type
     * {@link TOMUtil#STOP_NACK} arrives. If our bit is set in the mask and
     * we are still actively emitting STOPs for the NACKer's regency, we
     * unicast our current STOP back to that replica after a small wall-ms
     * jitter.
     *
     * Security/correctness: this method never touches {@link
     * bftsmart.tom.leaderchange.LCManager#addStop(int, int)}. The 2f+1
     * quorum still requires real, authenticated STOP messages.
     */
    public void handleStopNack(int regency, int fromPid, int mask) {
        int myId = controller.getStaticConf().getProcessId();
        if (myId < 0 || myId >= 32) return;               // mask ceiling
        if (((mask >> myId) & 1) == 0) return;            // not about us
        if (fromPid == myId) return;                      // reflected copy

        LCMessage myStop = currentStopByRegency.get(regency);
        if (myStop == null) return;                       // regency installed or never started

        ConcurrentHashMap<Integer, Integer> counts =
                nackReplyCount.computeIfAbsent(regency, r -> new ConcurrentHashMap<>());
        // Atomic increment + cap: returns the NEW count after increment.
        int newCount = counts.merge(fromPid, 1, Integer::sum);
        if (newCount > NACK_REPLIES_PER_PEER) {
            logger.debug("STOP_NACK from " + fromPid + " reg=" + regency
                    + " ignored (reply cap " + NACK_REPLIES_PER_PEER + " reached)");
            return;
        }

        final long jitterWallMs = 0; // C++ sendDelayed slot stagger makes wall-clock jitter redundant
        // Broadcast to ALL peers, not just the NACKer. On 802.11p every
        // "unicast" is physically a broadcast anyway (no real unicast at the
        // MAC layer), so there is no efficiency loss. More importantly, when
        // 10 replicas are simultaneously stuck at distinct=9 and all need
        // the same missing STOP (e.g. from pid 0), a single broadcast reply
        // advances all 10 at once. Unicasting only to the NACKer requires
        // 10 × N separate successful frame deliveries on a saturated channel
        // vs a single broadcast delivery observed in the empirical logs.
        final int[] target = controller.getCurrentViewOtherAcceptors();
        final LCMessage stopToSend = myStop;

        logger.info("Responding to STOP_NACK from " + fromPid + " reg=" + regency
                + " (resend " + (newCount + 1) + "/" + NACK_REPLIES_PER_PEER
                + ", jitter=" + jitterWallMs + "ms)");

        nackReplyExec.schedule(() -> {
            try {
                communication.send(target, stopToSend);
            } catch (Exception e) {
                logger.warn("STOP_NACK reply send failed reg=" + regency
                        + " to=" + fromPid + ": " + e.getMessage());
            }
        }, jitterWallMs, TimeUnit.MILLISECONDS);
    }
    
    class RequestTimerTask extends TimerTask {

        @Override
        /**
         * This is the code for the TimerTask. It executes the timeout for the first
         * message on the watched list.
         */
        public void run() {
            
            int[] myself = new int[1];
            myself[0] = controller.getStaticConf().getProcessId();

            communication.send(myself, new LCMessage(-1, TOMUtil.TRIGGER_LC_LOCALLY, -1, null));

        }
    }
    
    class SendStopTask extends TimerTask {

        private LCMessage stop;

        public SendStopTask(LCMessage stop) {
            this.stop = stop;
        }

        @Override
        /**
         * Wakes on the wall-clock {@link Timer} every STOP_RETX_WALL_MS.
         * Gates the actual broadcast on SIM-TIME elapsed since last emission
         * (tracked per-regency in {@link #lastStopSimEmitMs}) so channel
         * pressure stays bounded regardless of the wall:sim ratio. Always
         * reschedules a successor task; the chain is cancelled when the
         * regency is installed via {@link #stopSTOP(int)}.
         *
         * Emission mode depends on {@link #blindEmitCount}:
         *   - First {@link #STOP_BLIND_EMITS} fires: broadcast the real
         *     STOP to all peers (primes the Phase-1 quorum at receivers).
         *   - After that: broadcast a compact STOP_NACK whose bitmask names
         *     the acceptors this replica has not yet heard a STOP from.
         *     Honest peers in that mask re-emit their STOP (see
         *     {@link RequestsTimer#handleStopNack(int, int, int)}).
         * If the local replica has already reached quorum we skip emission
         * entirely this wakeup.
         */
        public void run() {
            int reg = stop.getReg();
            long nowSimMs = SimulationClock.currentTimeMillis();
            long lastSim = lastStopSimEmitMs.getOrDefault(reg, Long.MIN_VALUE / 2);
            long sinceSim = nowSimMs - lastSim;

            // Critical short-circuit: once the local replica has transitioned
            // past Phase 1 for this regency (Synchronizer.startSynchronization
            // advanced lastReg and wiped LCManager.stops[reg]), STOPs from us
            // cannot help any peer's BFT quorum (our previous STOPs are
            // already counted) and NACKs would be computed over an empty
            // LCManager.stops set, producing meaningless full-mask bursts
            // that saturate the channel and starve STOPDATA/SYNC. We stop
            // firing until the regency is fully installed (SYNC arrives →
            // removeSTOPretransmissions → stopSTOP → Timer cancel), but
            // still chain rescheduleSTOP so the external cancel path is
            // unchanged.
            int lastReg = tomLayer.getSynchronizer().getLCManager().getLastReg();
            boolean phase1DoneLocally = (lastReg >= reg);

            if (sinceSim >= STOP_RETX_SIM_MS && !phase1DoneLocally) {
                currentStopByRegency.put(reg, this.stop);

                int emits = blindEmitCount.getOrDefault(reg, 0);
                if (emits < STOP_BLIND_EMITS) {
                    logger.info("Re-transmitting STOP message to install regency "
                            + reg + " (sim-gap=" + sinceSim + "ms, blind "
                            + (emits + 1) + "/" + STOP_BLIND_EMITS + ")");
                    communication.send(controller.getCurrentViewOtherAcceptors(), this.stop);
                } else {
                    int mask = buildMissingMask(reg);
                    int missingCount = Integer.bitCount(mask);
                    int heardCount = heardByRegency.getOrDefault(reg, new HashSet<>()).size();
                    if (mask != 0) {
                        LCMessage nack = new LCMessage(
                                controller.getStaticConf().getProcessId(),
                                TOMUtil.STOP_NACK, reg, null);
                        nack.missingNodesMask = mask;
                        logger.info("Emitting STOP_NACK reg=" + reg
                                + " missing_mask=0x" + Integer.toHexString(mask)
                                + " missing_count=" + missingCount
                                + " heard=" + heardCount);
                        communication.send(controller.getCurrentViewOtherAcceptors(), nack);
                    } else {
                        logger.info("STOP_NACK mask empty at reg=" + reg
                                + " (heard=" + heardCount + "), sending full STOP");
                        communication.send(controller.getCurrentViewOtherAcceptors(), this.stop);
                    }
                }
                // Advance the emission counter on EVERY wake that actually
                // transmitted, regardless of which branch we took. Previously
                // this increment lived inside the NACK branch only, so while
                // we were in blind-emit mode the counter stayed at 0 forever
                // and we never transitioned to NACK — the channel kept
                // retransmitting full STOPs and receivers like the new leader
                // never got the 2f+1 they needed (see LC_INVESTIGATION.md,
                // "Stage 2 attempt, fix round 2").
                blindEmitCount.put(reg, emits + 1);
                lastStopSimEmitMs.put(reg, nowSimMs);
            } else if (phase1DoneLocally) {
                logger.debug("SendStopTask reg=" + reg
                        + " skipped: lastReg=" + lastReg
                        + " (Phase 1 installed locally; awaiting SYNC)");
            }
            rescheduleSTOP(reg, this.stop);
        }

    }

    /**
     * Build a bitmask of acceptor pids this replica has NOT yet heard a
     * STOP from for the given regency. Bit i == 1 means "missing pid i".
     * Self is always excluded (the sending replica's STOP is added to
     * {@link bftsmart.tom.leaderchange.LCManager} on triggerTimeout).
     */
    private int buildMissingMask(int regency) {
        int myId = controller.getStaticConf().getProcessId();
        // Use our transport-layer "heard" set, NOT LCManager.stops. See
        // comment on {@link #heardByRegency}: LCManager.stops is wiped
        // mid-LC by Synchronizer, so it's not a reliable source for the
        // NACK mask. The transport-level set persists until
        // {@link #dropRegencyState(int)} is invoked on install.
        Set<Integer> heard = heardByRegency.getOrDefault(regency, new HashSet<>());
        int mask = 0;
        int[] acceptors = controller.getCurrentViewAcceptors();
        for (int pid : acceptors) {
            if (pid == myId) continue;
            if (pid < 0 || pid >= 32) continue;
            if (!heard.contains(pid)) mask |= (1 << pid);
        }
        return mask;
    }
}

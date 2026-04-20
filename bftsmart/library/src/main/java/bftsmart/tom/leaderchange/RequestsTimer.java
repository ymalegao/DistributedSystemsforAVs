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
import java.util.concurrent.ThreadLocalRandom;
import java.util.concurrent.locks.ReentrantReadWriteLock;
import java.util.HashMap;
import java.util.Set;

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

    // Maximum jitter (wall-ms) added to every Timer.schedule() call so that the
    // initial STOP burst and subsequent STOP retransmissions from distinct
    // replicas de-synchronise. Over 802.11p, a synchronised broadcast by N
    // replicas at the same sim-instant collides catastrophically; adding
    // [0, JITTER_WALL_MS) of per-schedule spread turns a thundering herd into
    // a staggered one and massively improves STOP delivery probability.
    // Override via `-Dbftsmart.lc_jitter_wall_ms=<N>`.
    private static final long JITTER_WALL_MS =
            Long.getLong("bftsmart.lc_jitter_wall_ms", 500L);

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
            Long.getLong("bftsmart.stop_retx_sim_ms", 1000L);

    private ServerCommunicationSystem communication; // Communication system between replicas
    private ServerViewController controller; // Reconfiguration manager
    
    private HashMap <Integer, Timer> stopTimers = new HashMap<>();

    // Per-regency sim-time of the last actual STOP broadcast. Shared across
    // the one-shot SendStopTask instances so we throttle by sim-time rather
    // than firing on every wall-clock wakeup.
    private final HashMap<Integer, Long> lastStopSimEmitMs = new HashMap<>();
    
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

    }
    
    public Set<Integer> getTimers() {
        
        return ((HashMap <Integer,Timer>) stopTimers.clone()).keySet();
        
    }
    
    public void shutdown() {
        timer.cancel();
        stopAllSTOPs();
        LoggerFactory.getLogger(this.getClass()).info("RequestsTimer stopped.");

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
         */
        public void run() {
            int reg = stop.getReg();
            long nowSimMs = SimulationClock.currentTimeMillis();
            long lastSim = lastStopSimEmitMs.getOrDefault(reg, Long.MIN_VALUE / 2);
            long sinceSim = nowSimMs - lastSim;
            if (sinceSim >= STOP_RETX_SIM_MS) {
                logger.info("Re-transmitting STOP message to install regency "
                        + reg + " (sim-gap=" + sinceSim + "ms)");
                communication.send(controller.getCurrentViewOtherAcceptors(), this.stop);
                lastStopSimEmitMs.put(reg, nowSimMs);
            }
            rescheduleSTOP(reg, this.stop);
        }

    }
}

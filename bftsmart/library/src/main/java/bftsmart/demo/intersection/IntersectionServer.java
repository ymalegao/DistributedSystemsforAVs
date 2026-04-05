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
package bftsmart.demo.intersection;

import bftsmart.tom.MessageContext;
import bftsmart.tom.ServiceReplica;
import bftsmart.tom.ServiceProxy;
import java.nio.charset.StandardCharsets;
import bftsmart.tom.server.defaultservices.DefaultRecoverable;
import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.ObjectInput;
import java.io.ObjectInputStream;
import java.io.ObjectOutput;
import java.io.ObjectOutputStream;
import java.io.Serializable;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.*;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.security.Security;
import org.bouncycastle.jce.provider.BouncyCastleProvider;
import net.jpountz.xxhash.XXHashFactory;
import net.jpountz.xxhash.XXHash32;
import bftsmart.reconfiguration.Reconfiguration;
import bftsmart.reconfiguration.ReconfigureReply;

import bftsmart.tom.util.KeyLoader;
import bftsmart.reconfiguration.util.ECDSAKeyLoader;
import bftsmart.tom.core.TOMLayer;
import bftsmart.communication.V2V.SimulationClock;

/**
 * BFT replicated service for managing an intersection.
 * Cars send requests in the format: "CAR_ID:direction:arrival_time"
 * The server maintains a queue of waiting cars and allows the car with
 * the earliest arrival time to proceed through the intersection.
 * 
 * @author Intersection Demo
 */
public final class IntersectionServer extends DefaultRecoverable {
    private static final Map<Integer, IntersectionServer> readyServers = new ConcurrentHashMap<>();

    // XXHash for mock signatures (deterministic across C++ and Java)
    private static final XXHash32 xxhash = XXHashFactory.fastestInstance().hash32();

    // BATCH-OF-4 CONSENSUS MODE
    // Buffer for incoming JOIN requests - we wait until we have 4 unique cars
    private Map<String, Double> joinBuffer = new HashMap<>(); // carId -> arrivalTime
    // Volatile: updated by BFT delivery thread, read by proxy/polling thread
    private volatile String finalDecision = null; // The single decision for all 4 cars
    private volatile boolean batchProcessed = false;
    private volatile boolean orderProposeSubmitted = false; // dedup: only one invokeOrdered per ORDER round

    // Old fields (kept for compatibility)
    private Map<String, Integer> waitMap = new HashMap<>(); // carId -> wait
    private String lastLeaver = null;
    private long roundNumber = 0;
    private int iterations = 0;
    private int processId;
    private ServiceReplica replica;
    private int numCars;

    private static final int BATCH_SIZE = 16;

    private Map<String, Integer> waitRegistry = new HashMap<>();

    // Dynamic batch sizing (updated by C++ after clearance tracking)
    private int currentBatchSize = BATCH_SIZE; // Default 8, updated dynamically
    private int currentF = (BATCH_SIZE - 1) / 3; // Dynamic fault tolerance (f = 2 initially)
    // Leader-path diagnostics
    private volatile long lastForcedLeaderSetWallMs = 0L;
    private volatile int lastForcedLeaderId = -1;
    private volatile int lastForcedLeaderViewId = -1;
    private volatile int lastAppliedInMemoryViewId = -1;
    private volatile int lastNotifiedReconfigViewId = -1;
    private volatile int lastFirstOrderSeenEpoch = -1;
    private volatile int lastLoggedViewCid = -1;
    private volatile int lastLoggedOrderCid = -1;
    // Global dedup for expensive cleanup side-effects: run once per (viewId, departedReplica).
    private static final Set<String> clearedUnackedKeys = ConcurrentHashMap.newKeySet();

    // Intersection physics parameters for delay calculation
    // These can be configured via system properties or hardcoded
    private double intersectionWidth; // meters - distance to cross the intersection
    private double avgSpeed; // m/s - average vehicle speed
    private double safetyGap; // seconds - buffer time between cars
    private long lastDecisionSimTime = -1;
    private long viewConsensusStartWall;
    private long viewConsensusEndWall;
    private long orderConsensusStartWall;
    private long orderConsensusEndWall;
    private final long experimentStartWall = System.currentTimeMillis();

    // Phase 1: View consensus state
    private Map<Set<String>, List<ViewProposal>> viewProposals = new HashMap<>();
    // Volatile: updated by BFT delivery thread, read by proxy/polling thread
    volatile Map<String, VehicleState> agreedViewState = null; // vehicleId to VehicleState
    private volatile boolean viewPhaseComplete = false;

    // Phase 2: ORDER state (leader builds OrderBag from agreedViewState)
    private volatile boolean orderPhaseComplete = false;
    private volatile boolean orderNotifiedThisRound = false;

    private bftsmart.tom.ServiceProxy localClientProxy = null;

    private Set<Integer> departedReplicas = new HashSet<>(); // Replica IDs that have left
    private final Object departedLock = new Object(); // Thread-safe access

    /** Notify C++ that wipeAndReinit completed; C++ will then command re-announce. */
    private native void notifyWipeComplete(int processId);

    // View Consensus Data (Phase 1)
    static class ViewProposal implements Serializable {
        int proposerReplicaId;
        Set<String> observedCars; // kept for legacy VIEW_AGREE matching; actual state in vehicleStates
        List<VehicleState> vehicleStates; // full per-car state with f+1 witness sigs
        List<ViewSignature> v2vSignatures; // f+1 signatures over the vehicleStates string
    }

    static class ViewSignature implements Serializable {
        int signingReplicaId;
        byte[] signatureBytes; // XXHash32 over vehicleStates_semicolon_string + ":" + proposerReplicaId
    }

    /** Per-vehicle state agreed via VIEW consensus (replaces ReadyQCData). */
    static class VehicleState implements Serializable {
        String vehicleId;     // "veh0", "veh1", etc.
        String lane;          // "N", "S", "E", "W"
        int positionInLane;   // 1 = front of lane
        String direction;     // "S" (Straight) | "L" (Left) | "R" (Right)
        boolean isAmbulance;
        List<WitnessSignature> signatures; // f+1 witness sigs on the full state
    }

    /** Parse "vehN" suffix; numeric order so veh9 sorts before veh10 (string order would not). */
    static int vehicleIdNumericOrder(String vehicleId) {
        if (vehicleId == null || !vehicleId.startsWith("veh")) return 0;
        try {
            return Integer.parseInt(vehicleId.substring(3));
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    /**
     * Total order along a lane: lower positionInLane = closer to intersection;
     * ties broken by numeric vehicle id so queue order is deterministic.
     */
    static int compareLaneQueueOrder(VehicleState a, VehicleState b) {
        int c = Integer.compare(a.positionInLane, b.positionInLane);
        if (c != 0) return c;
        return Integer.compare(vehicleIdNumericOrder(a.vehicleId), vehicleIdNumericOrder(b.vehicleId));
    }

    /** An ordered set of vehicles that can cross simultaneously. */
    static class Batch implements Serializable {
        List<String> vehicleIds;
        Batch() { vehicleIds = new ArrayList<>(); }
    }

    /** The ordered schedule output by the leader and agreed via ORDER consensus. */
    static class OrderBag implements Serializable {
        int epoch;
        List<Batch> batches;
        OrderBag(int epoch) { this.epoch = epoch; this.batches = new ArrayList<>(); }
    }

    static class WitnessSignature implements Serializable {
        int witnessReplicaId;
        byte[] signatureBytes; // XXHash32 mock
        double witnessTimestamp;
    }

    public IntersectionServer(int id, int numCars) {
        System.out.println("[Server " + id + "] DEBUG: Constructor started.");
        if (Security.getProvider("BC") == null) {
            Security.addProvider(new BouncyCastleProvider());
            System.out.println("[Server " + id + "] DEBUG: Bouncy Castle Provider registered.");
        }

        String workingDir = System.getProperty("user.dir");
        System.out.println("==================================================");
        System.out.println("[DEBUG] JVM Working Directory: " + workingDir);
        System.out.println("[DEBUG] LOOK HERE FOR CONFIG: " + workingDir + "/config");
        System.out.println("==================================================");
        this.waitMap = new HashMap<>();
        this.processId = id;
        this.numCars = numCars;

        // Initialize intersection physics parameters from system properties or defaults
        this.intersectionWidth = Double.parseDouble(System.getProperty("intersection.width", "25.0"));
        this.avgSpeed = Double.parseDouble(System.getProperty("intersection.avgSpeed", "10.0"));
        this.safetyGap = Double.parseDouble(System.getProperty("intersection.safetyGap", "2.0"));

        System.out.println("[Server " + id + "] Intersection Physics: width=" + intersectionWidth +
                "m, avgSpeed=" + avgSpeed + "m/s, safetyGap=" + safetyGap + "s");

        System.out.println("[Server " + id + "] DEBUG: About to create ServiceReplica (This might block)...");
        System.out.println("[Server " + id + "] DEBUG: Thread = " + Thread.currentThread().getName());
        System.out.println("[Server " + id + "] DEBUG: Current time = " + System.currentTimeMillis());

        try {
            System.out.println("[Server " + id + "] DEBUG: Calling new ServiceReplica(" + id + ", ...)");
            this.replica = new ServiceReplica(id, this, this, new OrderRequestVerifier(this));
            if (replica != null) {
                bftsmart.communication.ServerCommunicationSystem commSystem = replica.getServerCommunicationSystem();
                if (commSystem != null) {
                    bftsmart.communication.V2V.V2VServersCommunicationLayer v2vLayer = (bftsmart.communication.V2V.V2VServersCommunicationLayer) commSystem
                            .getServersConn();
                    if (v2vLayer != null) {
                        v2vLayer.setServer(this);
                        System.out
                                .println("[Server " + id + "] Set server reference in V2V layer for zombie filtering");
                    }

                }
            }
        } catch (Exception e) {
            System.err.println("[Server " + processId + "] CRITICAL ERROR creating ServiceReplica:");
            e.printStackTrace();
            throw e; // Rethrow so ServerRunner sees the error
        }

        System.out.println("[Server " + id + "] DEBUG: *** ServiceReplica constructor RETURNED! ***");
        System.out.println("[Server " + id + "] DEBUG: About to register in readyServers map...");

        readyServers.put(id, this);
        System.out.println("[IntersectionServer " + id + "] ========================================");
        // System.out.println("[IntersectionServer " + id + "] *** REGISTERED as Ready!
        // ***");
        System.out.println("[IntersectionServer " + id + "] *** Waiting for OMNeT++ trigger. ***");
        long simMs = SimulationClock.currentTimeMillis();
        System.out.println(
                "[IntersectionServer " + id + "] simtime (ms) = " + simMs + " (sec = " + (simMs / 1000.0) + ")");
        System.out.println("[IntersectionServer " + id + "] ========================================");

        // NOTE: We no longer auto-trigger sendCarRequest() via Thread.sleep!
        // Instead, OMNeT++ will call triggerJoin() when the car reaches the
        // intersection.
        // This keeps BFT in sync with simulation time.
    }

    public static boolean isServerReady(int id) {
        return readyServers.containsKey(id);
    }

    /**
     * Mark a replica as departed (zombie mode).
     * Called via JNI from C++ when vehicle crosses intersection.
     * 
     * @param replicaId The replica ID that departed
     */
    public void markReplicaDeparted(int replicaId) {
        synchronized (departedLock) {
            departedReplicas.add(replicaId);
            bftsmart.communication.V2V.ReliableV2VMessaging.removeInstance(replicaId);

            System.out.println("[ZOMBIE] Replica " + replicaId + " (veh" + replicaId +
                    ") marked as DEPARTED");
            System.out.println("[ZOMBIE] Total zombies: " + departedReplicas.size());
            System.out.println("[ZOMBIE] Remaining active cars: " + (agreedViewState != null ? agreedViewState.size() : 0));
        }
    }

    /**
     * Check if a replica is departed (zombie).
     * 
     * @param replicaId The replica ID to check
     * @return true if replica has departed, false otherwise
     */
    public boolean isReplicaDeparted(int replicaId) {
        synchronized (departedLock) {
            return departedReplicas.contains(replicaId);
        }
    }

    /**
     * Update the batch size and recalculate fault tolerance.
     * Called via JNI from C++ after clearance tracking determines actual batch
     * size.
     * 
     * @param batchSize The actual number of active cars for this round
     */
    public void updateBatchSize(int batchSize) {
        this.currentBatchSize = batchSize;
        this.currentF = (batchSize - 1) / 3;

        System.out.println("[BATCH] Updated batch size: " + batchSize);
        System.out.println("[BATCH] New fault tolerance: f=" + currentF);
        System.out.println("[BATCH] New quorum: 2f+1=" + (2 * currentF + 1));
    }

    /**
     * Native method to notify C++ that VIEW consensus has completed.
     * Called after Phase 1c to transition to Phase 2 (ReadyQC collection).
     */
    private native void notifyViewAgreed(int replicaId, String viewMembers);

    private native void notifyOrderDecided(int replicaId, String orderDecision);

    /**
     * Native method to notify C++ OMNeT++ that consensus has completed
     * and the vehicle can resume movement.
     */
    private native void notifyVehicleCanGo(int replicaId, double delaySeconds);

    /**
     * TPWC: Called by OMNeT++ via JNI with VIEW_PROPOSE or ORDER_PROPOSE request.
     * 
     * @param request The consensus request (e.g., "VIEW_PROPOSE:veh0:lane:..." or
     *                "ORDER_PROPOSE")
     */
    public void triggerConsensusRequest(String request) {
        System.out.println("[IntersectionServer " + processId + "] triggerConsensusRequest: " + request);

        // Run in a separate thread to not block the JNI call
        new Thread(() -> {
            try {
                int delayMs = processId;
                Thread.sleep(delayMs + 10);
                sendConsensusRequest(request);
            } catch (Exception e) {
                System.err.println(
                        "[IntersectionServer " + processId + "] Error in triggerConsensusRequest: " + e.getMessage());
                e.printStackTrace();
            }
        }).start();
    }

    /**
     * Legacy method for backwards compatibility (old JOIN-based flow)
     */
    @Deprecated
    public void triggerJoin() {
        System.out.println("[IntersectionServer " + processId + "] triggerJoin() called by OMNeT++");

        // Run in a separate thread to not block the JNI call
        new Thread(() -> {
            try {
                sendCarRequest();
            } catch (Exception e) {
                System.err.println("[IntersectionServer " + processId + "] Error in triggerJoin: " + e.getMessage());
                e.printStackTrace();
            }
        }).start();
    }

    /**
     * Timeout for invokeOrdered (seconds). If consensus never delivers, we avoid
     * blocking forever.
     */
    private static final int CONSENSUS_REQUEST_TIMEOUT_SEC = 3600;

    /**
     * TPWC: Send consensus request (VIEW_PROPOSE or ORDER_PROPOSE) and handle
     * response.
     * If invokeOrdered never returns, the usual cause is consensus not deciding
     * (e.g. replicas
     * stuck in ACCEPT phase or delivery never running). Check logs for [DELIVERY]
     * to see if
     * receiveMessages/executeBatch ran; if not, consensus did not complete.
     */
    private void sendConsensusRequest(String request) {
        if (this.localClientProxy == null) {
            int clientId = this.processId + 1000;
            System.out.println(
                    "[SERVER " + this.processId + "] Initializing persistent ServiceProxy for client " + clientId);
            this.localClientProxy = new bftsmart.tom.ServiceProxy(clientId);

            // Give Netty a tiny buffer to establish initial TCP connections on first boot
            try {
                Thread.sleep(100);
                System.out.println("[PROXY_INIT " + processId + "] proxy created, wall_offset=" +
                        (System.currentTimeMillis() - experimentStartWall) + "ms");
            } catch (InterruptedException e) {
            }
        }

        if (isReplicaDeparted(processId)) {
            System.out.println("[SERVER " + processId + "] Departed — ignoring consensus request.");
            return;
        }

        if (request.contains("ORDER_PROPOSE")) {
            if (orderProposeSubmitted) {
                System.out.println("[SERVER " + processId + "] ORDER_PROPOSE already submitted this round — skipping duplicate.");
                return;
            }
            orderProposeSubmitted = true;
        }

        System.out.println("[SERVER " + this.processId + "] >>> Calling invokeOrdered...");

        try {
            if (request.contains("VIEW_PROPOSE")) {
                // Epoch boundary reset: clear all V2V carry-over state globally so
                // stale retransmissions/sequence expectations from prior epochs do not
                // pollute the new VIEW/ORDER round.
                bftsmart.communication.V2V.ReliableV2VMessaging
                    .globalResetV2V(null);
                viewConsensusStartWall = System.currentTimeMillis();
            }
            if (request.contains("ORDER_PROPOSE")) {
                orderConsensusStartWall = System.currentTimeMillis();
            }
            if (request.contains("VIEW_PROPOSE")) {
                System.out.println("[INVOKE_START " + processId + "] wall_offset=" +
                        (System.currentTimeMillis() - experimentStartWall) + "ms");
            }
            long proxyStart = System.nanoTime();
            byte[] reply;
            try {
                reply = invokeOrderedWithTimeout(this.localClientProxy, request.getBytes(StandardCharsets.UTF_8));
            } catch (NullPointerException npe) {
                // Stale proxy after reconfig: Netty channel to a departed replica is null.
                // This VIEW_PROPOSE will be null (no retry), but consensus still completes via
                // other replicas. The delivery callback will notify C++ regardless.
                // Null the proxy so ORDER_PROPOSE gets a fresh one with the post-reconfig view.
                System.err.println("[SERVER " + processId
                        + "] Netty NPE on stale proxy — nulling for fresh creation on next request.");
                try {
                    this.localClientProxy.close();
                } catch (Exception ignored) {
                }
                this.localClientProxy = null;
                reply = null;
            }
            long proxyEnd = System.nanoTime();
            System.out.println("[PROFILING " + processId + "] proxy.invokeOrdered took: " +
                    ((proxyEnd - proxyStart) / 1_000_000.0) + " ms");
            if (reply == null)
                return;

            if (request.contains("VIEW_PROPOSE")) {

                viewConsensusEndWall = System.currentTimeMillis();
                System.out.println("[BFTCONSENSUS " + this.processId + "] View consensus time: "
                        + (viewConsensusEndWall - viewConsensusStartWall) + "ms");
            }

            if (request.contains("ORDER_PROPOSE")) {
                orderConsensusEndWall = System.currentTimeMillis();
                System.out.println("[BFTCONSENSUS " + this.processId + "] Order consensus time: "
                        + (orderConsensusEndWall - orderConsensusStartWall) + "ms");
            }

            String replyStr = new String(reply, StandardCharsets.UTF_8);
            System.out.println("[SERVER " + this.processId + "] Consensus reply: " + replyStr);

            // while (replyStr.contains("BUFFERING")) {
            // System.out.println("[SERVER " + processId + "] Waiting for peer
            // proposals...");
            // Thread.sleep(300); // Wait for other replicas to send their VIEW_PROPOSE
            // System.out.println("[SERVER " + processId + "] Waiting for other replicas to
            // send their VIEW_PROPOSE...");
            // // Poll local volatile state updated by the BFT delivery thread.
            // // This avoids extra GET_STATE traffic and ensures the proxy thread observes
            // quorum once reached.

            // if (replyStr.contains("ORDER") && orderPhaseComplete && finalDecision !=
            // null) {
            // replyStr = finalDecision;
            // break;
            // }

            // if (replyStr.contains("VIEW") && viewPhaseComplete && agreedView != null) {
            // replyStr = "VIEW_AGREED:" + String.join(",", agreedView);
            // break;
            // }

            // }

            // Handle response based on type
            if (replyStr.startsWith("VIEW_AGREED:")) {
                // VIEW consensus complete - trigger ORDER consensus
                System.out
                        .println("[SERVER " + this.processId + "] VIEW consensus complete, triggering ORDER in c++...");

                try {

                    String viewStr = replyStr.substring("VIEW_AGREED:".length());

                    // Fallback for safety (though unlikely if consensus worked)
                    if (viewStr.isEmpty()) {
                        viewStr = "[]";
                    }

                    System.out.println("Processing request. Active View: " + viewStr);

                    notifyViewAgreed(this.processId, viewStr);
                    System.out.println("[VIEW] Notified C++ of agreed view for replica " + this.processId);

                } catch (UnsatisfiedLinkError e) {
                    System.err.println("[VIEW] Warning: Could not notify C++ (JNI not available): " + e.getMessage());
                }

                // // Send ORDER_PROPOSE request (with same timeout)
                // String orderReq = "ORDER_PROPOSE";
                // byte[] orderReply = invokeOrderedWithTimeout(proxy,
                // orderReq.getBytes(StandardCharsets.UTF_8));
                // if (orderReply == null) return;
                // String orderReplyStr = new String(orderReply, StandardCharsets.UTF_8);
                // System.out.println("[SERVER " + processId + "] ORDER reply: " +
                // orderReplyStr);

                // // Parse final decision and notify C++
                // parseAndNotifyDecision(orderReplyStr);
                //

            } else if (replyStr.startsWith("veh")) {
                // Final decision from ORDER consensus
                parseAndNotifyDecision(replyStr);

            } else if (replyStr.contains("BUFFERING")) {
                System.out.println("[SERVER " + processId + "] Still buffering: " + replyStr);

            } else if (replyStr.contains("VOTING_ONLY")) {
                System.out.println("[SERVER " + processId + "] Not in view, voting only");

            } else {
                System.out.println("[SERVER " + processId + "] Unexpected reply: " + replyStr);
            }

        } catch (Exception e) {
            System.err.println("[SERVER " + processId + "] Error in consensus request: " + e.getMessage());
            e.printStackTrace();
            try {
                if (this.localClientProxy != null)
                    this.localClientProxy.close();
            } catch (Exception ignored) {
            }
            this.localClientProxy = null;
        }
    }

    /**
     * Call proxy.invokeOrdered with a timeout.
     * Returns null on timeout.
     * Throws NullPointerException (unwrapped) if BFT-SMaRt NPE's internally
     * (e.g. a Netty channel to a departed replica is null), so the caller
     * can distinguish "stale proxy" from "consensus timed out" and recreate.
     */
    private byte[] invokeOrderedWithTimeout(ServiceProxy proxy, byte[] requestBytes) throws NullPointerException {
        ExecutorService executor = Executors.newSingleThreadExecutor();
        try {
            Future<byte[]> future = executor.submit(() -> proxy.invokeOrdered(requestBytes));
            return future.get(CONSENSUS_REQUEST_TIMEOUT_SEC, TimeUnit.SECONDS);
        } catch (TimeoutException e) {
            System.err.println(
                    "[SERVER " + processId + "] >>> invokeOrdered TIMED OUT after " + CONSENSUS_REQUEST_TIMEOUT_SEC
                            + "s. Consensus may not be completing (check if [DELIVERY] appears in replica logs).");
            return null;
        } catch (java.util.concurrent.ExecutionException e) {
            // Unwrap: if the root cause is a NPE it means a Netty channel to a
            // departed replica is null. Re-throw so sendConsensusRequest can recreate
            // the proxy and retry.
            if (e.getCause() instanceof NullPointerException) {
                throw (NullPointerException) e.getCause();
            }
            System.err.println("[SERVER " + processId + "] invokeOrdered failed: " + e.getMessage());
            e.printStackTrace();
            return null;
        } catch (Exception e) {
            System.err.println("[SERVER " + processId + "] invokeOrdered failed: " + e.getMessage());
            e.printStackTrace();
            return null;
        } finally {
            executor.shutdownNow();
        }
    }

    /**
     * Parse final decision and notify C++ to resume vehicle.
     */
    private void parseAndNotifyDecision(String decision) {
        System.out.println("[SERVER " + processId + "] >>> parseAndNotifyDecision called with: " + decision);
        // ORDER execution is BATCH-based; C++ parseAndNotifyDecision + executeBatch drives TraCI.
        // Legacy GO: format is obsolete here — do not call notifyVehicleCanGo (would use wrong :GO: check).
        if (decision.contains(":BATCH:")) {
            System.out.println("[SERVER " + processId + "] BATCH ORDER — C++ handles resume; skipping legacy GO parse");
            return;
        }
        String myCarId = "veh" + processId;

        // Parse the decision string to find this car's position
        // Format: "veh0:GO:0;veh1:GO:1;veh2:GO:2;veh3:GO:3"
        if (decision.contains(myCarId + ":GO:")) {
            for (String entry : decision.split(";")) {
                if (entry.startsWith(myCarId + ":")) {
                    String[] parts = entry.split(":");
                    if (parts.length >= 3) {
                        int position = Integer.parseInt(parts[2]);
                        double delaySeconds = calculateDelayFromPosition(position);

                        System.out.println("[SERVER " + processId + "] Final decision: Position=" + position +
                                ", Delay=" + delaySeconds + "s");

                        // Notify C++ OMNeT++ that the vehicle can go
                        try {
                            System.out.println("[SERVER " + processId + "] >>> Calling notifyVehicleCanGo with delay: "
                                    + delaySeconds);
                            notifyVehicleCanGo(processId, delaySeconds);
                            System.out.println("[SERVER " + processId + "] >>> notifyVehicleCanGo returned!");
                        } catch (UnsatisfiedLinkError e) {
                            System.err.println("[SERVER " + processId + "] Warning: Could not call native method: "
                                    + e.getMessage());
                        }
                        return;
                    }
                }
            }
            System.err.println("[SERVER " + processId + "] ERROR: Could not parse my position from: " + decision);
        } else {
            notifyVehicleCanGo(processId, 999999.0);
            System.out.println("[SERVER " + processId + "] Not in final decision (will go immediately)");
        }
    }

    private void sendCarRequest() {
        int clientId = 1000 + processId;
        String carId = "CAR_" + processId;

        try (ServiceProxy proxy = new ServiceProxy(clientId)) {
            String joinReq = "JOIN:" + carId;
            byte[] joinReply = proxy.invokeOrdered(joinReq.getBytes(StandardCharsets.UTF_8));
            String joinReplyStr = new String(joinReply, StandardCharsets.UTF_8);
            System.out.println("[SERVER " + processId + "] JOIN reply: " + joinReplyStr);

            // Parse the full decision string to extract THIS car's position
            // Format: "CAR_0:POS:0;CAR_1:POS:1;CAR_2:POS:2;CAR_3:POS:3"
            if (joinReplyStr.contains(carId + ":POS:")) {
                // Find this car's decision in the full string
                String myDecision = null;
                for (String entry : joinReplyStr.split(";")) {
                    if (entry.startsWith(carId + ":")) {
                        myDecision = entry;
                        break;
                    }
                }

                if (myDecision != null) {
                    String[] parts = myDecision.split(":");
                    int position = parts.length >= 3 ? Integer.parseInt(parts[2]) : 0;

                    // Calculate delay based on position and intersection physics
                    double delaySeconds = calculateDelayFromPosition(position);

                    System.out.println("[SERVER " + processId + "] Consensus reached! Position: " + position +
                            " - Calculated delay: " + delaySeconds + "s");

                    // Notify C++ OMNeT++ that the vehicle can go
                    try {
                        notifyVehicleCanGo(processId, delaySeconds);
                    } catch (UnsatisfiedLinkError e) {
                        System.err.println("[SERVER " + processId
                                + "] Warning: Could not call native method (JNI not available): " + e.getMessage());
                    }
                } else {
                    System.err.println(
                            "[SERVER " + processId + "] ERROR: Could not find my decision in: " + joinReplyStr);
                }
            } else if (joinReplyStr.contains(":WAIT")) {
                System.out.println("[SERVER " + processId + "] Consensus says WAIT, vehicle remains stopped");
            } else if (joinReplyStr.contains(":JOINED_BUFFERING")) {
                System.out.println("[SERVER " + processId + "] Still buffering, waiting for consensus...");
            }

            Thread.sleep(2000); // just to stagger the demo

            String leaveReq = "LEAVE:" + carId;
            byte[] leaveReply = proxy.invokeOrdered(leaveReq.getBytes(StandardCharsets.UTF_8));
            String replyStr = new String(leaveReply, StandardCharsets.UTF_8);

            System.out.println("[SERVER " + processId + "] LEAVE reply: " + replyStr);

            if (processId == 0 && replyStr.contains(":GO")) {
                System.out.println("[CAR_0] Left intersection (application level).");
                System.out.println("[CAR_0] Replica 0 staying alive to vote on reconfiguration (adding replica 4)...");
                System.out.println("[CAR_0] Will remove self and exit after replica 4 joins.");
                // Don't exit - let external script handle reconfiguration sequence
            }

        } catch (Exception e) {
            System.err.println("[SERVER " + processId + "] Error sending car request: " + e.getMessage());
            e.printStackTrace();
        }
    }

    /**
     * Calculate delay based on position in queue using intersection physics.
     *
     * Position 0 goes immediately.
     * Each subsequent position waits for:
     * - Time for previous car to clear intersection (intersectionWidth / avgSpeed)
     * - Safety gap (buffer between cars)
     *
     * @param position Queue position (0 = first car)
     * @return Delay in seconds before this vehicle can enter intersection
     */
    private double calculateDelayFromPosition(int position) {
        if (position <= 0) {
            return 0.0; // First car goes immediately
        }

        // Time for one car to cross + safety buffer
        double slotDuration = (intersectionWidth / avgSpeed) + safetyGap;

        // Delay = position * slotDuration
        double delay = position * slotDuration;

        System.out.println("[SERVER " + processId + "] Position " + position +
                " -> delay=" + String.format("%.2f", delay) + "s " +
                "(slot=" + String.format("%.2f", slotDuration) + "s)");

        return delay;
    }

    /**
     * Parse a single car entry in the format "CAR_ID:direction:arrival_time"
     * 
     * @param carEntry The car entry string
     * @return An array containing [carId, direction, arrivalTime] or null if
     *         parsing fails
     */
    private String[] parseCarEntry(String carEntry) {
        if (carEntry == null || carEntry.trim().isEmpty()) {
            return null;
        }

        String[] parts = carEntry.split(":");
        if (parts.length != 3) {
            return null;
        }

        try {
            // Validate that arrival_time is a valid double
            Double.parseDouble(parts[2].trim());
            return new String[] { parts[0].trim(), parts[1].trim(), parts[2].trim() };
        } catch (NumberFormatException e) {
            return null;
        }
    }

    private static final class Cmd {
        enum Type {
            JOIN, LEAVE, GET_STATE,
            VIEW_PROPOSE,   // Round 1: agree on who is present + their full state
            ORDER_PROPOSE,  // Round 2: leader proposes OrderBag; followers validate via RequestVerifier
            WIPE_STATE      // Internal: C++ triggered epoch preemption
        }

        Type type;
        String carId;
    }

    private Cmd parseCommand(String req) {
        String[] parts = req.split(":", 2);
        if (parts.length == 0 || parts[0].trim().isEmpty()) {
            throw new IllegalArgumentException("Invalid command: " + req);
        }
        Cmd cmd = new Cmd();
        cmd.type = Cmd.Type.valueOf(parts[0].trim().toUpperCase(java.util.Locale.ROOT));
        cmd.carId = (parts.length > 1 && !parts[1].trim().isEmpty()) ? parts[1].trim() : null;
        return cmd;
    }

    /**
     * Parse a request containing multiple cars in the format:
     * "CAR_ID1:direction1:arrival_time1;CAR_ID2:direction2:arrival_time2;..."
     * 
     * @param request The request string
     * @return Array of car data arrays, or null if parsing fails
     */
    private String[][] parseMultiCarRequest(String request) {
        if (request == null || request.trim().isEmpty()) {
            return null;
        }

        String[] carEntries = request.split(";");
        if (carEntries.length == 0) {
            return null;
        }

        String[][] cars = new String[carEntries.length][];
        for (int i = 0; i < carEntries.length; i++) {
            String[] carData = parseCarEntry(carEntries[i]);
            if (carData == null) {
                return null; // Invalid car entry
            }
            cars[i] = carData;
        }

        return cars;
    }

    private byte[][] generateByzantineResponse(byte[][] commands, java.util.List<String[]> allCars,
            String honestCarId) {

        System.err.println("[BYZANTINE] Replica " + processId + " is lying! Returning malicious response.");
        byte[][] replies = new byte[commands.length][];
        String maliciousCarId = null;
        if (allCars.size() > 0) {
            maliciousCarId = allCars.get(allCars.size() - 1)[0]; // Last car gets malicious response
            System.err.println("[BYZANTINE] Malicious decision: " + maliciousCarId + " gets GO (WRONG!)");
        }

        for (int i = 0; i < commands.length; i++) {
            String requestStr = new String(commands[i], StandardCharsets.UTF_8);
            String[][] cars = parseMultiCarRequest(requestStr);
            if (cars == null || cars.length == 0) {
                replies[i] = "ERROR: Invalid request format".getBytes(StandardCharsets.UTF_8);
                continue;
            }

            StringBuilder byzantine_response = new StringBuilder();
            boolean first = true;
            for (String[] carData : cars) {
                String carId = carData[0];
                String decision;
                if (carId.equals(maliciousCarId)) {
                    decision = "GO";
                } else {
                    decision = "WAIT";
                }

                if (!first) {
                    byzantine_response.append(";");
                }
                byzantine_response.append(carId).append(":").append(decision);
                first = false;
            }
            replies[i] = byzantine_response.toString().getBytes(StandardCharsets.UTF_8);
        }
        return replies;
    }

    @Override
    public byte[] appExecuteUnordered(byte[] command, MessageContext msgCtx) {
        // Unordered requests not supported for intersection control
        // Return error message
        return "ERROR: Unordered requests not supported".getBytes();
    }

    private String makeHonestDecision(java.util.Map<String, Double> buffer) {
        // 1. Convert Map to List
        java.util.List<java.util.Map.Entry<String, Double>> sortedCars = new java.util.ArrayList<>(buffer.entrySet());

        // 2. Sort Honestly (Arrival Time)
        sortedCars.sort((a, b) -> {
            int timeCompare = Double.compare(a.getValue(), b.getValue());
            if (timeCompare != 0)
                return timeCompare;
            return a.getKey().compareTo(b.getKey());
        });

        // 3. Build String: "CAR_0:POS:0;CAR_1:POS:1..."
        StringBuilder sb = new StringBuilder();
        for (int pos = 0; pos < sortedCars.size(); pos++) {
            if (pos > 0)
                sb.append(";");
            String carId = sortedCars.get(pos).getKey();
            sb.append(carId).append(":POS:").append(pos);
        }
        return sb.toString();
    }

    private String makeByzantineDecision(java.util.Map<String, Double> buffer) {
        System.err.println("[BYZANTINE] Generating MALICIOUS decision...");

        // 1. Get the list (same as honest)
        java.util.List<java.util.Map.Entry<String, Double>> maliciousList = new java.util.ArrayList<>(
                buffer.entrySet());

        // 2. Sort Honestly first...
        maliciousList.sort((a, b) -> {
            int timeCompare = Double.compare(a.getValue(), b.getValue());
            if (timeCompare != 0)
                return timeCompare;
            return a.getKey().compareTo(b.getKey());
        });

        // 3. ...THEN REVERSE IT (The Lie)
        // The last car (Car 3) becomes First (Pos 0)
        java.util.Collections.reverse(maliciousList);

        // 4. Build the Lying String
        StringBuilder sb = new StringBuilder();
        for (int pos = 0; pos < maliciousList.size(); pos++) {
            if (pos > 0)
                sb.append(";");
            String carId = maliciousList.get(pos).getKey();
            // We assign them the position in the REVERSED list
            sb.append(carId).append(":POS:").append(pos);
        }

        String lie = sb.toString();
        System.err.println("[BYZANTINE] The Lie is: " + lie);
        return lie;
    }

    // === View Consensus Helper Methods (Phase 1) ===

    /**
     * Parse the semicolon-delimited VehicleState records from a VIEW_PROPOSE payload.
     * Format: "veh0|N|1|S|0;veh1|S|1|L|0;veh2|W|2|R|1"
     * Fields per record: vehicleId|lane|posInLane|direction|isAmbulance
     */
    private List<VehicleState> parseVehicleStates(String vehicleStatesStr) {
        List<VehicleState> states = new ArrayList<>();
        if (vehicleStatesStr == null || vehicleStatesStr.isEmpty()) return states;

        for (String record : vehicleStatesStr.split(";")) {
            String[] f = record.trim().split("\\|");
            if (f.length < 5) {
                System.err.println("[VIEW] Malformed VehicleState record: " + record);
                continue;
            }
            try {
                VehicleState vs = new VehicleState();
                vs.vehicleId     = f[0].trim();
                vs.lane          = f[1].trim();
                vs.positionInLane= Integer.parseInt(f[2].trim());
                vs.direction     = f[3].trim();
                vs.isAmbulance   = "1".equals(f[4].trim());
                vs.signatures    = new ArrayList<>();
                states.add(vs);
            } catch (Exception e) {
                System.err.println("[VIEW] Error parsing VehicleState record '" + record + "': " + e.getMessage());
            }
        }
        return states;
    }

    /** Serialise VehicleState list back to the semicolon-pipe wire format. */
    private String serializeVehicleStates(List<VehicleState> states) {
        StringBuilder sb = new StringBuilder();
        for (VehicleState vs : states) {
            if (sb.length() > 0) sb.append(';');
            sb.append(vs.vehicleId).append('|')
              .append(vs.lane).append('|')
              .append(vs.positionInLane).append('|')
              .append(vs.direction).append('|')
              .append(vs.isAmbulance ? '1' : '0');
        }
        return sb.toString();
    }

    private List<ViewSignature> parseViewSignatures(String sigString) {
        // Format: "replicaId,hash|replicaId,hash|..."
        List<ViewSignature> signatures = new ArrayList<>();
        if (sigString == null || sigString.isEmpty()) return signatures;

        for (String sigPart : sigString.split("\\|")) {
            String[] fields = sigPart.split(",");
            if (fields.length >= 2) {
                try {
                    ViewSignature sig = new ViewSignature();
                    sig.signingReplicaId = Integer.parseInt(fields[0].trim());
                    long hashValue = Long.parseLong(fields[1].trim());
                    ByteBuffer buffer = ByteBuffer.allocate(4);
                    buffer.order(ByteOrder.LITTLE_ENDIAN);
                    buffer.putInt((int) hashValue);
                    sig.signatureBytes = buffer.array();
                    signatures.add(sig);
                } catch (Exception e) {
                    System.err.println("[VIEW] Error parsing view signature: " + e.getMessage());
                }
            }
        }
        return signatures;
    }

    private boolean validateViewProposal(ViewProposal proposal) {
        int groupSize = proposal.vehicleStates != null ? proposal.vehicleStates.size()
                                                       : proposal.observedCars.size();
        int f = (groupSize - 1) / 3;

        if (proposal.v2vSignatures.size() < f + 1) {
            System.err.println("[VIEW] Insufficient V2V signatures: " +
                    proposal.v2vSignatures.size() + " (need " + (f + 1) + ")");
            return false;
        }

        // Build the vehicleStates string for verification (must match C++ construction)
        String vsStr = proposal.vehicleStates != null
                ? serializeVehicleStates(proposal.vehicleStates) : "";

        for (ViewSignature sig : proposal.v2vSignatures) {
            if (!verifyViewSignature(vsStr, sig)) {
                System.err.println("[VIEW] Invalid V2V signature from replica " + sig.signingReplicaId);
                return false;
            }
        }
        return true;
    }

    /**
     * Verify a single ViewSignature against the vehicleStates semicolon string.
     * XXHash32 input: vehicleStatesStr + ":" + signingReplicaId  (matches C++)
     */
    private boolean verifyViewSignature(String vehicleStatesStr, ViewSignature sig) {
        String input = vehicleStatesStr + ":" + sig.signingReplicaId;
        byte[] dataBytes = input.getBytes(StandardCharsets.UTF_8);
        int expectedHash = xxhash.hash(dataBytes, 0, dataBytes.length, 0);

        if (sig.signatureBytes == null || sig.signatureBytes.length < 4) return false;

        ByteBuffer buffer = ByteBuffer.wrap(sig.signatureBytes);
        buffer.order(ByteOrder.LITTLE_ENDIAN);
        int actualHash = buffer.getInt();

        System.out.println("[VIEW_VERIFY] Replica " + sig.signingReplicaId +
                " input=\"" + input + "\" expected=" + expectedHash +
                " actual=" + actualHash + " match=" + (expectedHash == actualHash));
        return expectedHash == actualHash;
    }

    // === Two-Phase Witnessed Consensus Helper Methods (Phase 2) ===

    private List<WitnessSignature> parseSignatures(String sigString) {
        // Format: "replicaId,timestamp,decimalHash|replicaId,timestamp,decimalHash|..."
        // Mock signatures are 4-byte XXHash32 encoded as decimal strings
        List<WitnessSignature> signatures = new ArrayList<>();

        if (sigString == null || sigString.isEmpty()) {
            return signatures;
        }

        String[] sigParts = sigString.split("\\|");
        for (String sigPart : sigParts) {
            String[] fields = sigPart.split(",");
            if (fields.length >= 3) {
                try {
                    WitnessSignature sig = new WitnessSignature();
                    sig.witnessReplicaId = Integer.parseInt(fields[0]);
                    sig.witnessTimestamp = Double.parseDouble(fields[1]);

                    // Convert decimal string to 4-byte array (int32_t XXHash32 from C++)
                    long hashValue = Long.parseLong(fields[2].trim());
                    ByteBuffer buffer = ByteBuffer.allocate(4);
                    buffer.order(ByteOrder.LITTLE_ENDIAN); // Match C++ byte order
                    buffer.putInt((int) hashValue);
                    sig.signatureBytes = buffer.array();

                    signatures.add(sig);
                } catch (Exception e) {
                    System.err.println("[PARSE] Error parsing signature: " + e.getMessage());
                }
            }
        }

        return signatures;
    }

    // =========================================================================
    // ORDER BAG HELPERS (new format)
    // =========================================================================

    /**
     * Parse the ORDER_PROPOSE payload.
     * Format: {@code "<epoch>:<veh0:0;veh1:0;veh2:1;veh3:2>"}
     * where each entry is {@code <vehicleId>:<batchIndex>}.
     */
    OrderBag parseNewOrderBag(String payload) {
        if (payload == null || payload.isEmpty()) return null;
        try {
            String[] top = payload.split(":", 2);
            int epoch = Integer.parseInt(top[0].trim());
            OrderBag bag = new OrderBag(epoch);

            if (top.length < 2 || top[1].trim().isEmpty()) return bag; // empty bag

            // Find highest batch index first to size the list
            int maxIdx = -1;
            String[] entries = top[1].split(";");
            for (String entry : entries) {
                String[] kv = entry.split(":");
                if (kv.length == 2) maxIdx = Math.max(maxIdx, Integer.parseInt(kv[1].trim()));
            }
            for (int idx = 0; idx <= maxIdx; idx++) bag.batches.add(new Batch());

            for (String entry : entries) {
                String[] kv = entry.split(":");
                if (kv.length == 2) {
                    String vid = kv[0].trim();
                    int batchIdx = Integer.parseInt(kv[1].trim());
                    bag.batches.get(batchIdx).vehicleIds.add(vid);
                }
            }
            return bag;
        } catch (Exception e) {
            System.err.println("[ORDER-PARSE] Failed to parse OrderBag: " + e.getMessage());
            return null;
        }
    }

    /**
     * Serialise an OrderBag to the JNI wire format:
     * {@code "veh0:BATCH:0;veh1:BATCH:0;veh2:BATCH:1"}
     */
    private String serializeOrderBagForJNI(OrderBag bag) {
        StringBuilder sb = new StringBuilder();
        for (int bIdx = 0; bIdx < bag.batches.size(); bIdx++) {
            for (String vid : bag.batches.get(bIdx).vehicleIds) {
                if (sb.length() > 0) sb.append(';');
                sb.append(vid).append(":BATCH:").append(bIdx);
            }
        }
        return sb.toString();
    }

    /**
     * Serialise an OrderBag for submission as an ORDER_PROPOSE BFT request.
     * Format: {@code "<epoch>:<veh0:0;veh1:0;veh2:1>"}
     */
    private String serializeOrderBagForBFT(OrderBag bag) {
        StringBuilder sb = new StringBuilder();
        for (int bIdx = 0; bIdx < bag.batches.size(); bIdx++) {
            for (String vid : bag.batches.get(bIdx).vehicleIds) {
                if (sb.length() > 0) sb.append(';');
                sb.append(vid).append(':').append(bIdx);
            }
        }
        return bag.epoch + ":" + sb;
    }

    // =========================================================================
    // LEADER SCHEDULING ALGORITHM
    // =========================================================================

    /** True when every same-lane car strictly ahead in {@link #compareLaneQueueOrder} is already placed. */
    private static boolean allSameLaneFrontPlaced(VehicleState c, Map<String, VehicleState> view,
                                                  Set<String> placed) {
        for (VehicleState v : view.values()) {
            if (!v.lane.equals(c.lane)) continue;
            if (compareLaneQueueOrder(v, c) < 0 && !placed.contains(v.vehicleId))
                return false;
        }
        return true;
    }

    private static int batchIndexOfOrderBag(OrderBag bag, String vehicleId) {
        for (int i = 0; i < bag.batches.size(); i++) {
            if (bag.batches.get(i).vehicleIds.contains(vehicleId)) return i;
        }
        return -1;
    }

    /**
     * Build the OrderBag for the given epoch from the agreed view.
     *
     * Priority order (work queue):
     *   1. Ambulance blockers (same lane, positionInLane &lt; ambulance), front-to-back
     *   2. Ambulances (verified isAmbulance=true)
     *   3. Remaining cars sorted by waitRegistry desc → positionInLane asc → vehicleId
     *
     * Batch construction: only cars whose same-lane front cars are already scheduled may
     * be placed (so a rear car never shares a batch with or precedes someone still ahead
     * in its lane). Within that, greedily pack ConflictMatrix-safe cars; repeat until
     * all placed.
     */
    OrderBag buildProposal(Map<String, VehicleState> view, int epoch) {
        if (view == null || view.isEmpty()) return new OrderBag(epoch);

        // Separate ambulances and normal cars
        List<VehicleState> ambulances = new ArrayList<>();
        for (VehicleState vs : view.values()) {
            if (vs.isAmbulance) ambulances.add(vs);
        }
        ambulances.sort(Comparator
                .comparingInt((VehicleState vs) -> vs.positionInLane)
                .thenComparingInt(vs -> vehicleIdNumericOrder(vs.vehicleId)));

        // Build work queue
        Set<String> priorityIds = new HashSet<>(); // blockers + ambulances
        List<VehicleState> workQueue = new ArrayList<>();

        for (VehicleState amb : ambulances) {
            // Collect blockers (same lane, ahead of ambulance in queue order; excludes ambulance)
            List<VehicleState> blockers = new ArrayList<>();
            for (VehicleState vs : view.values()) {
                if (!vs.lane.equals(amb.lane)) continue;
                if (vs.isAmbulance) continue;
                if (compareLaneQueueOrder(vs, amb) < 0) {
                    blockers.add(vs);
                }
            }
            blockers.sort(Comparator
                    .comparingInt((VehicleState vs) -> vs.positionInLane)
                    .thenComparingInt(vs -> vehicleIdNumericOrder(vs.vehicleId)));
            for (VehicleState b : blockers) {
                if (priorityIds.add(b.vehicleId)) workQueue.add(b);
            }
            if (priorityIds.add(amb.vehicleId)) workQueue.add(amb);
        }

        // Remaining cars sorted by waitRegistry (desc) → positionInLane (asc) → vehicleId
        List<VehicleState> remaining = new ArrayList<>();
        for (VehicleState vs : view.values()) {
            if (!priorityIds.contains(vs.vehicleId)) remaining.add(vs);
        }
        remaining.sort(Comparator
                .comparingInt((VehicleState vs) -> -(waitRegistry.getOrDefault(vs.vehicleId, 0)))
                .thenComparingInt(vs -> vs.positionInLane)
                .thenComparingInt(vs -> vehicleIdNumericOrder(vs.vehicleId)));
        workQueue.addAll(remaining);

        StringBuilder wq = new StringBuilder();
        for (VehicleState v : workQueue) {
            if (wq.length() > 0) wq.append(',');
            wq.append(v.vehicleId);
        }
        System.out.println("[LEADER] workQueue=" + wq);

        // Greedy batch construction with same-lane queue gating
        OrderBag bag = new OrderBag(epoch);
        Set<String> placed = new HashSet<>();
        while (placed.size() < view.size()) {
            VehicleState head = null;
            for (VehicleState vs : workQueue) {
                if (placed.contains(vs.vehicleId)) continue;
                if (!allSameLaneFrontPlaced(vs, view, placed)) continue;
                head = vs;
                break;
            }
            if (head == null) {
                System.err.println("[LEADER] buildProposal: no schedulable head (placed=" + placed.size()
                        + "/" + view.size() + ")");
                break;
            }

            Batch batch = new Batch();
            batch.vehicleIds.add(head.vehicleId);
            placed.add(head.vehicleId);

            boolean grew;
            do {
                grew = false;
                for (VehicleState candidate : workQueue) {
                    if (placed.contains(candidate.vehicleId)) continue;
                    if (!allSameLaneFrontPlaced(candidate, view, placed)) continue;
                    boolean safe = true;
                    for (String inBatch : batch.vehicleIds) {
                        if (!ConflictMatrix.isSafeToBatch(view.get(inBatch), candidate)) {
                            safe = false;
                            break;
                        }
                    }
                    if (safe) {
                        batch.vehicleIds.add(candidate.vehicleId);
                        placed.add(candidate.vehicleId);
                        grew = true;
                    }
                }
            } while (grew);

            bag.batches.add(batch);
        }

        for (VehicleState amb : ambulances) {
            int ambBi = batchIndexOfOrderBag(bag, amb.vehicleId);
            StringBuilder blk = new StringBuilder();
            for (VehicleState o : view.values()) {
                if (!o.lane.equals(amb.lane)) continue;
                if (compareLaneQueueOrder(o, amb) >= 0) continue; // not ahead of ambulance
                if (blk.length() > 0) blk.append(';');
                blk.append(o.vehicleId).append("@batch").append(batchIndexOfOrderBag(bag, o.vehicleId));
            }
            System.out.println("[AMBULANCE_SCHED] " + amb.vehicleId + " batch=" + ambBi
                    + " blockersAhead=" + blk);
        }

        System.out.println("[LEADER] Built OrderBag epoch=" + epoch + " batches=" + bag.batches.size()
                + " totalCars=" + placed.size());
        return bag;
    }

    /**
     * Called by the leader thread (spawned from VIEW consensus delivery) to submit
     * the ORDER_PROPOSE request via localClientProxy.  Must run on a separate thread
     * to avoid deadlocking the BFT delivery thread.
     */
    private void submitOrderPropose(Map<String, VehicleState> view, int epoch) {
        try {
            // Build the OrderBag deterministically from the agreed view
            OrderBag bag = buildProposal(view, epoch);
            if (bag == null) {
                System.err.println("[LEADER] buildProposal returned null — cannot submit ORDER_PROPOSE");
                return;
            }

            String payload = "ORDER_PROPOSE:" + serializeOrderBagForBFT(bag);
            System.out.println("[LEADER] Submitting ORDER_PROPOSE: " + payload);

            // Lazily create client proxy
            if (localClientProxy == null) {
                int clientId = this.processId + 1000;
                localClientProxy = new bftsmart.tom.ServiceProxy(clientId);
                Thread.sleep(80);
            }
            byte[] result = localClientProxy.invokeOrdered(payload.getBytes(StandardCharsets.UTF_8));
            if (result != null) {
                System.out.println("[LEADER] ORDER_PROPOSE completed, reply=" + new String(result, StandardCharsets.UTF_8));
            }
        } catch (Exception e) {
            System.err.println("[LEADER] submitOrderPropose failed: " + e.getMessage());
        }
    }


    private void resetForNextRound() {
        viewPhaseComplete = false;
        orderPhaseComplete = false;
        orderNotifiedThisRound = false;
        agreedViewState = null;
        roundNumber++;
        System.out.println("[RESET] ===== STARTING ROUND " + roundNumber + " =====");
    }

    /**
     * Full wipe triggered by C++ epoch preemption.
     * Resets all protocol state and reconfigures BFT-SMaRt for the new participant set.
     * Called by C++ via JNI (ServerRunner.wipeAndReinitForReplica).
     */
    public void doWipeAndReinit(int[] newParticipants) {
        System.out.println("[WIPE] doWipeAndReinit called for replica " + processId +
                " with " + newParticipants.length + " participants");

        // 1. Reset all volatile protocol state
        resetForNextRound();

        // 2. Close and null the client proxy (will be lazily recreated)
        if (localClientProxy != null) {
            try { localClientProxy.close(); } catch (Exception e) { /* ignore */ }
            localClientProxy = null;
        }

        // 3. Reconfigure BFT-SMaRt for new N via svc.reconfigureTo()
        try {
            bftsmart.reconfiguration.ServerViewController svc =
                    this.replica.getReplicaContext().getSVController();
            bftsmart.reconfiguration.views.View current = svc.getCurrentView();

            int newF = (newParticipants.length - 1) / 3;
            int newViewId = current.getId() + 1;

            // Build dense address array — all vehicle IDs are pre-registered in hosts.config
            java.net.InetSocketAddress[] addrs = new java.net.InetSocketAddress[newParticipants.length];
            for (int i = 0; i < newParticipants.length; i++) {
                addrs[i] = current.getAddress(newParticipants[i]);
            }

            bftsmart.reconfiguration.views.View newView =
                    new bftsmart.reconfiguration.views.View(newViewId, newParticipants, newF, addrs);
            svc.reconfigureTo(newView);

            System.out.println("[WIPE] Reconfigured BFT to N=" + newParticipants.length +
                    " f=" + newF + " viewId=" + newViewId);
        } catch (Exception e) {
            System.err.println("[WIPE] Error during svc.reconfigureTo: " + e.getMessage());
            e.printStackTrace();
        }

        // 4. Notify C++ that wipe is complete; C++ will then command re-announce
        try {
            notifyWipeComplete(processId);
        } catch (UnsatisfiedLinkError e) {
            System.err.println("[WIPE] JNI notifyWipeComplete unavailable: " + e.getMessage());
        }
    }

    @Override
    public byte[][] appExecuteBatch(byte[][] commands, MessageContext[] msgCtxs, boolean fromConsensus) {
        iterations++;
        long appStartTime = System.nanoTime();
        byte[][] replies = new byte[commands.length][];
        roundNumber++;
        boolean triggerRoundReset = false;

        try {
            // --- PREPARE: Decode all commands first ---
            Cmd[] decoded = new Cmd[commands.length];
            for (int i = 0; i < commands.length; i++) {
                String reqStr = new String(commands[i], StandardCharsets.UTF_8).trim();
                if (reqStr.isEmpty())
                    throw new IllegalArgumentException("Empty command at index " + i);
                decoded[i] = parseCommand(reqStr);
            }

            // =========================================================================
            // PHASE 1: LOGIC & DECISION (Scan the whole batch to update state)
            // =========================================================================

            // Scan for a valid VIEW_PROPOSE; on success populate agreedViewState
            for (Cmd cmd : decoded) {
                if (viewPhaseComplete) break;
                if (cmd.type != Cmd.Type.VIEW_PROPOSE) continue;
                if (cmd.carId == null || cmd.carId.equals("NONE")) continue;
                if (isReplicaDeparted(processId)) continue;

                try {
                    // New wire format: "<proposerId>:<vehicleStates>:<viewSignatures>"
                    // vehicleStates uses ';' between cars and '|' within fields
                    String[] parts = cmd.carId.split(":", 3);
                    if (parts.length < 3) continue;

                    ViewProposal proposal = new ViewProposal();
                    proposal.proposerReplicaId = Integer.parseInt(parts[0].trim());
                    proposal.vehicleStates     = parseVehicleStates(parts[1]);
                    proposal.v2vSignatures     = parseViewSignatures(parts[2]);
                    // Reconstruct observedCars for legacy compatibility
                    proposal.observedCars = new HashSet<>();
                    for (VehicleState vs : proposal.vehicleStates) proposal.observedCars.add(vs.vehicleId);

                    if (!validateViewProposal(proposal)) continue;

                    // Build agreedViewState, filtering departed replicas
                    Map<String, VehicleState> newViewState = new LinkedHashMap<>();
                    for (VehicleState vs : proposal.vehicleStates) {
                        int rid = Integer.parseInt(vs.vehicleId.substring(3));
                        if (!isReplicaDeparted(rid)) newViewState.put(vs.vehicleId, vs);
                    }

                    this.agreedViewState  = newViewState;
                    this.viewPhaseComplete = true;

                    String resultString = String.join(",", new TreeSet<>(newViewState.keySet()));
                    System.out.println("[SERVER] VIEW CONSENSUS REACHED. Cars=" + resultString);
                    System.out.println("[SERVER] wall_offset=" + (System.currentTimeMillis() - experimentStartWall) + "ms");

                    try {
                        notifyViewAgreed(this.processId, resultString);
                    } catch (UnsatisfiedLinkError e) {
                        System.err.println("[VIEW] JNI notifyViewAgreed unavailable: " + e.getMessage());
                    }

                    // If I am the leader, submit ORDER_PROPOSE in a new thread
                    final Map<String, VehicleState> frozenView = Collections.unmodifiableMap(newViewState);
                    final int frozenEpoch = (int) roundNumber;
                    final int leaderIdSnapshot = this.processId; // forcedLeader stored elsewhere; use min ID
                    // Determine leader: minimum processId in the agreed view
                    int minId = newViewState.keySet().stream()
                            .mapToInt(id -> Integer.parseInt(id.substring(3)))
                            .min().orElse(-1);
                    if (this.processId == minId) {
                        new Thread(() -> {
                            try {
                                Thread.sleep(this.processId * 2L + 20); // small stagger
                                submitOrderPropose(frozenView, frozenEpoch);
                            } catch (Exception ex) {
                                System.err.println("[ORDER] Leader ORDER_PROPOSE thread failed: " + ex.getMessage());
                            }
                        }, "order-propose-" + processId).start();
                    }

                    break; // found valid evidence — stop scanning

                } catch (Exception e) {
                    System.err.println("[VIEW] Error parsing VIEW_PROPOSE in scan phase: " + e.getMessage());
                }
            }

            // VIEW scan complete — agreedViewState is set if valid evidence was found

            // =========================================================================
            // PHASE 2: REPLY GENERATION (Respond to clients based on Phase 1 state)
            // =========================================================================

            for (int i = 0; i < commands.length; i++) {
                Cmd cmd = decoded[i];
                String reply = "ERROR:Unknown command";

                switch (cmd.type) {
                    case VIEW_PROPOSE:
                        long viewSimMs = SimulationClock.currentTimeMillis();
                        int viewCid = -1;
                        if (msgCtxs != null && i < msgCtxs.length && msgCtxs[i] != null) {
                            viewCid = msgCtxs[i].getConsensusId();
                        }
                        if (viewCid != lastLoggedViewCid) {
                            System.out.println("[VIEW_CID " + processId + "] consensus_id=" + viewCid
                                    + " epoch=" + roundNumber + " at t=" + viewSimMs + "ms");
                            lastLoggedViewCid = viewCid;
                        }

                        if (isReplicaDeparted(processId)) {
                            reply = "DEPARTED";
                        } else if (viewPhaseComplete && agreedViewState != null) {
                            reply = "VIEW_AGREED:" + String.join(",", new TreeSet<>(agreedViewState.keySet()));
                        } else {
                            reply = "VIEW_REJECTED:No_Valid_Evidence";
                        }
                        break;

                    case ORDER_PROPOSE:
                        // Round 2: leader proposed OrderBag; RequestVerifier has already
                        // validated completeness, safety, and ambulance priority before this fires.
                        if (isReplicaDeparted(processId)) {
                            reply = "DEPARTED";
                        } else if (viewPhaseComplete && !orderPhaseComplete) {
                            // Parse the OrderBag from the request payload
                            // Format: "ORDER_PROPOSE:<epoch>:<veh0:0;veh1:0;veh2:1;veh3:2>"
                            OrderBag bag = parseNewOrderBag(cmd.carId);
                            if (bag != null) {
                                this.orderPhaseComplete  = true;
                                this.orderNotifiedThisRound = true;
                                triggerRoundReset = true;

                                String batchDecision = serializeOrderBagForJNI(bag);
                                System.out.println("[ORDER] Committed OrderBag epoch=" + bag.epoch
                                        + " batches=" + bag.batches.size() + " decision=" + batchDecision);

                                // Notify this replica's C++ module
                                try {
                                    notifyOrderDecided(processId, batchDecision);
                                } catch (UnsatisfiedLinkError e) {
                                    System.err.println("[ORDER] JNI notifyOrderDecided unavailable: " + e.getMessage());
                                }
                                // Proactively notify all other replicas in the agreed view
                                if (agreedViewState != null) {
                                    for (String vid : agreedViewState.keySet()) {
                                        int rid = Integer.parseInt(vid.substring(3));
                                        if (rid == processId) continue;
                                        try { notifyOrderDecided(rid, batchDecision); }
                                        catch (UnsatisfiedLinkError e) { /* ignore */ }
                                    }
                                }
                                reply = batchDecision;
                            } else {
                                reply = "ERROR:Could not parse OrderBag";
                            }
                        } else if (orderPhaseComplete) {
                            reply = "ORDER_ALREADY_DECIDED";
                        } else {
                            reply = "ERROR:View not complete yet";
                        }
                        break;

                    case JOIN: // Legacy
                        reply = "JOIN:LEGACY COMMANDS ARE NOT SUPPORTED";
                        break;
                    case LEAVE: // Legacy
                        reply = "LEAVE:LEGACY COMMANDS ARE NOT SUPPORTED";
                        break;
                    case GET_STATE: // Legacy
                        reply = "GET_STATE:LEGACY COMMANDS ARE NOT SUPPORTED";
                        break;

                    default:
                        reply = "ERROR:UNKNOWN COMMAND: " + cmd.type;
                        break;
                }
                replies[i] = reply.getBytes(StandardCharsets.UTF_8);
            }

            // Log Stats
            double appTimeMs = (System.nanoTime() - appStartTime) / 1_000_000.0;
            // System.out.println("Batch processed in " + String.format("%.3f", appTimeMs) +
            // " ms");
            if (triggerRoundReset) {
                resetForNextRound();
            }
        } catch (

        Exception ex) {
            System.err.println("Batch Error: " + ex.getMessage());
            ex.printStackTrace();
        }
        return replies;
    }

    public static void main(String[] args) {
        if (args.length < 2) {
            System.out.println("Use: java IntersectionServer <processId> <numCars>");
            System.exit(-1);
        }
        int numCars = Integer.parseInt(args[1]);
        new IntersectionServer(Integer.parseInt(args[0]), numCars);

    }

    @SuppressWarnings("unchecked")
    @Override
    public void installSnapshot(byte[] state) {
        try (ObjectInput in = new ObjectInputStream(new ByteArrayInputStream(state))) {
            joinBuffer = (Map<String, Double>) in.readObject();
            finalDecision = (String) in.readObject();
            batchProcessed = in.readBoolean();
            roundNumber = in.readLong();
            System.out.println("[STATE] Snapshot installed. Buffer size: " + joinBuffer.size() +
                    ", Batch processed: " + batchProcessed);
        } catch (IOException | ClassNotFoundException e) {
            System.err.println("[ERROR] Error deserializing state: " + e.getMessage());
            joinBuffer = new HashMap<>();
            finalDecision = null;
            batchProcessed = false;
            roundNumber = 0;
        }
    }

    @Override
    public byte[] getSnapshot() {
        try (ByteArrayOutputStream bos = new ByteArrayOutputStream();
                ObjectOutput out = new ObjectOutputStream(bos)) {
            out.writeObject(joinBuffer);
            out.writeObject(finalDecision);
            out.writeBoolean(batchProcessed);
            out.writeLong(roundNumber);
            out.flush();
            System.out.println("[STATE] Snapshot taken. Buffer size: " + joinBuffer.size() +
                    ", Batch processed: " + batchProcessed);
            return bos.toByteArray();
        } catch (IOException ioe) {
            System.err.println("[ERROR] Error serializing state: " + ioe.getMessage());
            return "ERROR".getBytes(StandardCharsets.UTF_8);
        }
    }
}

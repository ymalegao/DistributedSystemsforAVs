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

import bftsmart.clientsmanagement.ClientsManager;
import bftsmart.communication.V2V.SimulationClock;
import bftsmart.tom.MessageContext;
import bftsmart.tom.ServiceProxy;
import bftsmart.tom.ServiceReplica;
import bftsmart.tom.core.messages.TOMMessage;
import bftsmart.tom.core.messages.TOMMessageType;
import bftsmart.tom.server.ViewChangeRebuildHook;
import bftsmart.tom.server.defaultservices.DefaultRecoverable;
import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.ObjectInput;
import java.io.ObjectInputStream;
import java.io.ObjectOutput;
import java.io.ObjectOutputStream;
import java.nio.charset.StandardCharsets;
import java.security.Security;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeSet;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import org.bouncycastle.jce.provider.BouncyCastleProvider;

/**
 * BFT replicated service for the single-round PROPOSE_ALL intersection protocol.
 *
 * The C++ proposer sends one packet:
 *   PROPOSE_ALL:<proposerId>:<vehicleStatesStr>:<perCarCerts>
 *
 * The Java leader appends the computed schedule and submits a single invokeOrdered call:
 *   PROPOSE_ALL:<proposerId>:<vehicleStatesStr>:<perCarCerts>:<orderBagStr>
 *
 * OrderRequestVerifier validates per-car ARRIVAL_CERT signatures (f+1 per SIGNED car)
 * and the proposed schedule before every follower votes. On delivery, all replicas call notifyOrderDecided.
 */
public final class IntersectionServer extends DefaultRecoverable implements ViewChangeRebuildHook {
    private static final Map<Integer, IntersectionServer> readyServers = new ConcurrentHashMap<>();
    private static final int CONSENSUS_REQUEST_TIMEOUT_SEC = 3600;

    private long roundNumber = 0;
    private final int processId;
    private final ServiceReplica replica;
    private final Map<String, Integer> waitRegistry = new HashMap<>();
    private final long experimentStartWall = System.currentTimeMillis();
    private final Set<Integer> departedReplicas = new HashSet<>();
    private final Object departedLock = new Object();

    private volatile boolean proposeAllSubmitted = false;
    volatile Map<String, VehicleState> agreedViewState = null;

    private ServiceProxy localClientProxy = null;
    private long consensusStartWall;

    /** Notify C++ that wipeAndReinit completed; C++ will command re-announce. */
    private native void notifyWipeComplete(int processId);

    /** Notify C++ of the single-round PROPOSE_ALL wall-clock consensus latency. */
    private native void notifyProposeAllConsensusMetric(int replicaId, int epoch, double wallSeconds);

    /** Kept for JNI registration compatibility (not called in the single-round protocol). */
    private native void notifyViewAgreed(int replicaId, String viewMembers);

    /** Notify C++ that ORDER consensus completed. */
    private native void notifyOrderDecided(int replicaId, String orderDecision);

    @SuppressWarnings("unused")
    private native void notifyVehicleCanGo(int replicaId, double delaySeconds);

    /**
     * JNI pull: returns the key set of this replica's C++ collectedCerts map.
     * Called only from getCertSnapshot(); do not call directly.
     */
    private native Set<String> nativeGetCertSnapshot(int replicaId);

    /**
     * JNI pull: returns "<vehicleStatesStr>:<perCarCerts>" built from this replica's
     * current C++ collectedCerts. Called only from getFreshProposePayload().
     */
    private native String nativeGetFreshProposePayload(int replicaId);

    /**
     * Returns a consistent snapshot of carIds in C++ collectedCerts at the moment of the call.
     * Survives intra-epoch BFT view-changes (collectedCerts is not cleared by Java-side
     * view-change machinery). Cleared at epoch boundary when C++ handleWipeComplete() fires.
     * Returns an empty set when JNI is unavailable (e.g. unit-test context).
     */
    /** Returns the BFT fault tolerance parameter f from system.config. */
    public int getF() {
        try {
            return replica.getReplicaContext().getStaticConfiguration().getF();
        } catch (Exception e) {
            return 5; // safe fallback for N=16
        }
    }

    public Set<String> getCertSnapshot() {
        try {
            Set<String> raw = nativeGetCertSnapshot(processId);
            return (raw != null)
                    ? Collections.unmodifiableSet(new HashSet<>(raw))
                    : Collections.emptySet();
        } catch (UnsatisfiedLinkError e) {
            System.err.println("[SERVER] getCertSnapshot JNI unavailable: " + e.getMessage());
            return Collections.emptySet();
        }
    }

    /**
     * Returns "&lt;vehicleStatesStr&gt;:&lt;perCarCerts&gt;" built from this replica's current
     * C++ collectedCerts (ground truth). Called by the view-change rebuild hook
     * ({@link #rebuildPendingProposals}) so the new leader can construct a fresh
     * PROPOSE_ALL that includes any vehicle the previous Byzantine leader censored.
     * <p>
     * Returns an empty string when JNI is unavailable (e.g. unit-test context);
     * callers must treat empty-string as "no fresh build possible — fall back to
     * the existing replay-pending-request behavior" so prior EP5 guarantees hold.
     */
    public String getFreshProposePayload() {
        try {
            String raw = nativeGetFreshProposePayload(processId);
            return raw != null ? raw : "";
        } catch (UnsatisfiedLinkError e) {
            System.err.println("[SERVER] getFreshProposePayload JNI unavailable: " + e.getMessage());
            return "";
        }
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

        this.processId = id;

        System.out.println("[Server " + id + "] DEBUG: About to create ServiceReplica (This might block)...");
        System.out.println("[Server " + id + "] DEBUG: Thread = " + Thread.currentThread().getName());
        System.out.println("[Server " + id + "] DEBUG: Current time = " + System.currentTimeMillis());

        try {
            System.out.println("[Server " + id + "] DEBUG: Calling new ServiceReplica(" + id + ", ...)");
            this.replica = new ServiceReplica(id, this, this, new OrderRequestVerifier(this), this);
            if (replica != null) {
                bftsmart.communication.ServerCommunicationSystem commSystem = replica.getServerCommunicationSystem();
                if (commSystem != null) {
                    bftsmart.communication.V2V.V2VServersCommunicationLayer v2vLayer =
                            (bftsmart.communication.V2V.V2VServersCommunicationLayer) commSystem.getServersConn();
                    if (v2vLayer != null) {
                        v2vLayer.setServer(this);
                        System.out.println("[Server " + id + "] Set server reference in V2V layer for zombie filtering");
                    }
                }
            }
        } catch (Exception e) {
            System.err.println("[Server " + processId + "] CRITICAL ERROR creating ServiceReplica:");
            e.printStackTrace();
            throw e;
        }

        System.out.println("[Server " + id + "] DEBUG: *** ServiceReplica constructor RETURNED! ***");
        readyServers.put(id, this);
        System.out.println("[IntersectionServer " + id + "] ========================================");
        System.out.println("[IntersectionServer " + id + "] *** Waiting for OMNeT++ trigger. ***");
        long simMs = SimulationClock.currentTimeMillis();
        System.out.println("[IntersectionServer " + id + "] simtime (ms) = " + simMs
                + " (sec = " + (simMs / 1000.0) + ")");
        System.out.println("[IntersectionServer " + id + "] ========================================");
    }

    public static boolean isServerReady(int id) {
        return readyServers.containsKey(id);
    }

    /**
     * Mark a replica as departed (zombie mode).
     * Called via JNI from C++ when vehicle crosses intersection.
     */
    public void markReplicaDeparted(int replicaId) {
        synchronized (departedLock) {
            departedReplicas.add(replicaId);
            bftsmart.communication.V2V.ReliableV2VMessaging.removeInstance(replicaId);

            System.out.println("[ZOMBIE] Replica " + replicaId + " (veh" + replicaId + ") marked as DEPARTED");
            System.out.println("[ZOMBIE] Total zombies: " + departedReplicas.size());
            System.out.println("[ZOMBIE] Remaining active cars: "
                    + (agreedViewState != null ? agreedViewState.size() : 0));
        }
    }

    public boolean isReplicaDeparted(int replicaId) {
        synchronized (departedLock) {
            return departedReplicas.contains(replicaId);
        }
    }

    public void updateBatchSize(int batchSize) {
        int faultTolerance = Math.max(0, (batchSize - 1) / 3);
        System.out.println("[BATCH] Updated active batch size hint: " + batchSize);
        System.out.println("[BATCH] Derived fault tolerance hint: f=" + faultTolerance);
        System.out.println("[BATCH] Derived quorum hint: 2f+1=" + (2 * faultTolerance + 1));
    }

    /**
     * Called by OMNeT++ via JNI with the PROPOSE_ALL request.
     */
    public void triggerConsensusRequest(String request) {
        System.out.println("[IntersectionServer " + processId + "] triggerConsensusRequest: " + request);

        new Thread(() -> {
            try {
                int delayMs = processId;
                Thread.sleep(delayMs + 10L);
                sendConsensusRequest(request);
            } catch (Exception e) {
                System.err.println("[IntersectionServer " + processId
                        + "] Error in triggerConsensusRequest: " + e.getMessage());
                e.printStackTrace();
            }
        }).start();
    }

    /**
     * Send a single PROPOSE_ALL through BFT-SMaRt.
     *
     * Incoming from C++:
     *   PROPOSE_ALL:<proposerId>:<vehicleStatesStr>:<perCarCerts>
     *
     * This method appends the computed schedule and calls invokeOrdered once:
     *   PROPOSE_ALL:<proposerId>:<vehicleStatesStr>:<perCarCerts>:<orderBagStr>
     */
    private void sendConsensusRequest(String request) {
        if (this.localClientProxy == null) {
            int clientId = this.processId + 1000;
            System.out.println("[SERVER " + this.processId
                    + "] Initializing persistent ServiceProxy for client " + clientId);
            this.localClientProxy = new ServiceProxy(clientId);

            try {
                Thread.sleep(100);
                System.out.println("[PROXY_INIT " + processId + "] proxy created, wall_offset="
                        + (System.currentTimeMillis() - experimentStartWall) + "ms");
            } catch (InterruptedException ignored) {
                Thread.currentThread().interrupt();
            }
        }

        if (isReplicaDeparted(processId)) {
            System.out.println("[SERVER " + processId + "] Departed; ignoring consensus request.");
            return;
        }

        if (proposeAllSubmitted) {
            System.out.println("[SERVER " + processId
                    + "] PROPOSE_ALL already submitted this round; skipping duplicate.");
            return;
        }
        proposeAllSubmitted = true;

        // Split C++ packet: PROPOSE_ALL:<proposerId>:<vehicleStatesStr>:<perCarCerts>
        // vehicleStatesStr and perCarCerts contain no ':', so limit=4 is safe.
        String[] top = request.split(":", 4);
        if (top.length < 4 || !"PROPOSE_ALL".equals(top[0])) {
            System.err.println("[SERVER " + processId + "] Unexpected request format: " + request);
            proposeAllSubmitted = false;
            return;
        }
        String proposerStr      = top[1];
        String vehicleStatesStr = top[2];
        String perCarCertsStr   = top[3];


        

        // Build view map (filter departed vehicles)
        List<VehicleState> states = ViewConsensusProtocol.parseVehicleStates(vehicleStatesStr);
        Map<String, VehicleState> viewMap = new LinkedHashMap<>();
        for (VehicleState vs : states) {
            int rid = Integer.parseInt(vs.vehicleId.substring(3));
            if (!isReplicaDeparted(rid)) {
                viewMap.put(vs.vehicleId, vs);
            }
        }

        // Leader computes the deterministic schedule
        int epoch = (int) roundNumber;
        OrderBag bag = OrderScheduler.buildProposal(viewMap, epoch, waitRegistry);
        String schedulePart = OrderScheduler.serializeOrderBagForBFT(bag);

        // Full message for BFT-SMaRt ordered consensus:
        // PROPOSE_ALL:<proposerId>:<vehicleStatesStr>:<perCarCerts>:<orderBagStr>
        String fullRequest = "PROPOSE_ALL:" + proposerStr + ":" + vehicleStatesStr + ":"
                + perCarCertsStr + ":" + schedulePart;

        System.out.println("[SERVER " + processId + "] >>> Calling invokeOrdered for PROPOSE_ALL...");
        bftsmart.communication.V2V.ReliableV2VMessaging.globalResetV2V(null);
        consensusStartWall = System.currentTimeMillis();
        System.out.println("[INVOKE_START " + processId + "] wall_offset="
                + (System.currentTimeMillis() - experimentStartWall) + "ms");

        try {
            long proxyStart = System.nanoTime();
            byte[] reply;
            try {
                reply = invokeOrderedWithTimeout(this.localClientProxy,
                        fullRequest.getBytes(StandardCharsets.UTF_8));
            } catch (NullPointerException npe) {
                System.err.println("[SERVER " + processId
                        + "] Netty NPE on stale proxy; nulling for fresh creation on next request.");
                try {
                    this.localClientProxy.close();
                } catch (Exception ignored) {
                }
                this.localClientProxy = null;
                proposeAllSubmitted = false;
                return;
            }
            long proxyEnd = System.nanoTime();
            System.out.println("[PROFILING " + processId + "] proxy.invokeOrdered took: "
                    + ((proxyEnd - proxyStart) / 1_000_000.0) + " ms");

            long consensusEndWall = System.currentTimeMillis();
            // Emit the BFTCONSENSUS metric only if appExecuteBatch hasn't already
            // emitted+reset it. In the honest-leader path appExecuteBatch fires
            // before invokeOrdered returns and zeros consensusStartWall, so this
            // branch is skipped (preventing a duplicate sample). In the rare case
            // where invokeOrdered returns before delivery (or appExecuteBatch
            // missed it), this remains the fallback emit point so analyze_log.py
            // still gets a sample.
            if (consensusStartWall > 0) {
                double consensusWallSeconds = (consensusEndWall - consensusStartWall) / 1000.0;
                System.out.println("[BFTCONSENSUS " + processId + "] PROPOSE_ALL consensus time epoch="
                        + epoch + ": " + (consensusEndWall - consensusStartWall) + "ms");
                try {
                    notifyProposeAllConsensusMetric(processId, epoch, consensusWallSeconds);
                } catch (UnsatisfiedLinkError e) {
                    System.err.println("[BFTCONSENSUS] JNI notifyProposeAllConsensusMetric unavailable: "
                            + e.getMessage());
                }
                consensusStartWall = 0;
            }

            if (reply == null) {
                return;
            }

            String replyStr = new String(reply, StandardCharsets.UTF_8);
            System.out.println("[SERVER " + processId + "] Consensus reply: " + replyStr);

        } catch (Exception e) {
            System.err.println("[SERVER " + processId + "] Error in consensus request: " + e.getMessage());
            e.printStackTrace();
            try {
                if (this.localClientProxy != null) {
                    this.localClientProxy.close();
                }
            } catch (Exception ignored) {
            }
            this.localClientProxy = null;
        }
    }

    private byte[] invokeOrderedWithTimeout(ServiceProxy proxy, byte[] requestBytes)
            throws NullPointerException {
        ExecutorService executor = Executors.newSingleThreadExecutor();
        try {
            Future<byte[]> future = executor.submit(() -> proxy.invokeOrdered(requestBytes));
            return future.get(CONSENSUS_REQUEST_TIMEOUT_SEC, TimeUnit.SECONDS);
        } catch (TimeoutException e) {
            System.err.println("[SERVER " + processId + "] >>> invokeOrdered TIMED OUT after "
                    + CONSENSUS_REQUEST_TIMEOUT_SEC
                    + "s. Consensus may not be completing (check if [DELIVERY] appears in replica logs).");
            return null;
        } catch (java.util.concurrent.ExecutionException e) {
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

    @Override
    public byte[] appExecuteUnordered(byte[] command, MessageContext msgCtx) {
        return "ERROR: Unordered requests not supported".getBytes(StandardCharsets.UTF_8);
    }

    private void resetForNextRound() {
        proposeAllSubmitted = false;
        agreedViewState = null;
        roundNumber++;
        System.out.println("[RESET] ===== STARTING ROUND " + roundNumber + " =====");
    }

    /**
     * Full wipe triggered by C++ epoch preemption.
     */
    public void doWipeAndReinit(int[] newParticipants) {
        System.out.println("[WIPE] doWipeAndReinit called for replica " + processId
                + " with " + newParticipants.length + " participants");

        resetForNextRound();

        if (localClientProxy != null) {
            try {
                localClientProxy.close();
            } catch (Exception ignored) {
            }
            localClientProxy = null;
        }

        try {
            bftsmart.reconfiguration.ServerViewController svc =
                    this.replica.getReplicaContext().getSVController();
            bftsmart.reconfiguration.views.View current = svc.getCurrentView();

            int newF = (newParticipants.length - 1) / 3;
            int newViewId = current.getId() + 1;

            java.net.InetSocketAddress[] addrs = new java.net.InetSocketAddress[newParticipants.length];
            for (int i = 0; i < newParticipants.length; i++) {
                addrs[i] = current.getAddress(newParticipants[i]);
            }

            bftsmart.reconfiguration.views.View newView =
                    new bftsmart.reconfiguration.views.View(newViewId, newParticipants, newF, addrs);
            svc.reconfigureTo(newView);

            System.out.println("[WIPE] Reconfigured BFT to N=" + newParticipants.length
                    + " f=" + newF + " viewId=" + newViewId);
        } catch (Exception e) {
            System.err.println("[WIPE] Error during svc.reconfigureTo: " + e.getMessage());
            e.printStackTrace();
        }

        try {
            notifyWipeComplete(processId);
        } catch (UnsatisfiedLinkError e) {
            System.err.println("[WIPE] JNI notifyWipeComplete unavailable: " + e.getMessage());
        }
    }

    @Override
    public byte[][] appExecuteBatch(byte[][] commands, MessageContext[] msgCtxs, boolean fromConsensus) {
        long appStartTime = System.nanoTime();
        byte[][] replies = new byte[commands.length][];
        boolean triggerRoundReset = false;

        try {
            Cmd[] decoded = new Cmd[commands.length];
            for (int i = 0; i < commands.length; i++) {
                String reqStr = new String(commands[i], StandardCharsets.UTF_8).trim();
                if (reqStr.isEmpty()) {
                    throw new IllegalArgumentException("Empty command at index " + i);
                }
                decoded[i] = parseCommand(reqStr);
            }

            for (int i = 0; i < commands.length; i++) {
                Cmd cmd = decoded[i];
                String reply;

                switch (cmd.type) {
                    case PROPOSE_ALL: {
                        if (isReplicaDeparted(processId)) {
                            reply = "DEPARTED";
                            break;
                        }

                        // payload: "<proposerId>:<vehicleStatesStr>:<perCarCerts>:<orderBagStr>"
                        // vehicleStatesStr and perCarCerts have no ':', so limit=4 is safe.
                        String[] parts = cmd.payload != null ? cmd.payload.split(":", 4) : new String[0];
                        if (parts.length < 4) {
                            reply = "ERROR:Malformed PROPOSE_ALL payload";
                            break;
                        }

                        String vehicleStatesStr = parts[1];
                        String orderBagStr      = parts[3]; // epoch:veh0:0;veh1:0;...

                        // Build agreed view (filter departed vehicles)
                        List<VehicleState> states = ViewConsensusProtocol.parseVehicleStates(vehicleStatesStr);
                        Map<String, VehicleState> newViewState = new LinkedHashMap<>();
                        for (VehicleState vs : states) {
                            int rid = Integer.parseInt(vs.vehicleId.substring(3));
                            if (!isReplicaDeparted(rid)) {
                                newViewState.put(vs.vehicleId, vs);
                            }
                        }
                        this.agreedViewState = newViewState;

                        // Parse and execute the committed schedule
                        OrderBag bag = OrderScheduler.parseOrderBag(orderBagStr);
                        if (bag == null) {
                            reply = "ERROR:Could not parse OrderBag";
                            break;
                        }

                        //

                        triggerRoundReset = true;
                        String batchDecision = OrderScheduler.serializeOrderBagForJNI(bag);

                        int consensusId = -1;
                        if (msgCtxs != null && i < msgCtxs.length && msgCtxs[i] != null) {
                            consensusId = msgCtxs[i].getConsensusId();
                        }
                        System.out.println("[ORDER] Committed PROPOSE_ALL consensus_id=" + consensusId
                                + " epoch=" + bag.epoch
                                + " batches=" + bag.batches.size()
                                + " decision=" + batchDecision);

                        // Emit PROPOSE_ALL consensus latency from the original
                        // submitter (the only replica with consensusStartWall > 0).
                        // Doing it here — at delivery — instead of after
                        // invokeOrdered() returns is required for the EP5
                        // Byzantine-leader rebuild path: the rebuilt request is
                        // injected under a synthetic clientId so the deposed
                        // leader's localClientProxy.invokeOrdered() never gets a
                        // reply and would block until CONSENSUS_REQUEST_TIMEOUT_SEC,
                        // leaving analyze_log.py with no sample (-> bft_decision_time_s=null).
                        // We zero consensusStartWall after emitting so the
                        // post-invokeOrdered block in submitViewToBFTConsensus
                        // does not double-print.
                        if (consensusStartWall > 0) {
                            long deliveredWall = System.currentTimeMillis();
                            double consensusWallSeconds = (deliveredWall - consensusStartWall) / 1000.0;
                            System.out.println("[BFTCONSENSUS " + processId
                                    + "] PROPOSE_ALL consensus time epoch=" + bag.epoch
                                    + ": " + (deliveredWall - consensusStartWall) + "ms");
                            try {
                                notifyProposeAllConsensusMetric(processId, bag.epoch, consensusWallSeconds);
                            } catch (UnsatisfiedLinkError e) {
                                System.err.println("[BFTCONSENSUS] JNI notifyProposeAllConsensusMetric unavailable: "
                                        + e.getMessage());
                            }
                            consensusStartWall = 0;
                        }
                        System.out.println("[SERVER] Cars=" + String.join(",",
                                new TreeSet<>(newViewState.keySet()))
                                + " wall_offset=" + (System.currentTimeMillis() - experimentStartWall) + "ms");

                        // Notify this replica's vehicle
                        try {
                            notifyOrderDecided(processId, batchDecision);
                        } catch (UnsatisfiedLinkError e) {
                            System.err.println("[ORDER] JNI notifyOrderDecided unavailable: " + e.getMessage());
                        }
                        // Notify other vehicles managed by this JVM process
                        for (String vehicleId : newViewState.keySet()) {
                            int rid = Integer.parseInt(vehicleId.substring(3));
                            if (rid == processId) continue;
                            try {
                                notifyOrderDecided(rid, batchDecision);
                            } catch (UnsatisfiedLinkError ignored) {
                            }
                        }

                        // Update wait registry for fairness tracking
                        for (String vehicleId : newViewState.keySet()) {
                            waitRegistry.merge(vehicleId, 1, Integer::sum);
                        }

                        reply = batchDecision;
                        break;
                    }

                    default:
                        reply = "ERROR:UNKNOWN COMMAND: " + cmd.type;
                        break;
                }

                replies[i] = reply.getBytes(StandardCharsets.UTF_8);
            }

            double appTimeMs = (System.nanoTime() - appStartTime) / 1_000_000.0;
            System.out.println("[PROFILE] appExecuteBatch took " + appTimeMs + "ms");
            if (triggerRoundReset) {
                resetForNextRound();
            }
        } catch (Exception ex) {
            System.err.println("Batch Error: " + ex.getMessage());
            ex.printStackTrace();
        }
        return replies;
    }

    private static final class Cmd {
        enum Type {
            PROPOSE_ALL
        }

        Type type;
        String payload;
    }

    private Cmd parseCommand(String req) {
        String[] parts = req.split(":", 2);
        if (parts.length == 0 || parts[0].trim().isEmpty()) {
            throw new IllegalArgumentException("Invalid command: " + req);
        }
        Cmd cmd = new Cmd();
        cmd.type = Cmd.Type.valueOf(parts[0].trim().toUpperCase(java.util.Locale.ROOT));
        cmd.payload = (parts.length > 1 && !parts[1].trim().isEmpty()) ? parts[1].trim() : null;
        return cmd;
    }

    /**
     * Package-private: exposes the wait-registry for deterministic re-execution
     * in OrderRequestVerifier (Check 8). Returns an unmodifiable view; callers
     * must not cache it across consensus rounds.
     */
    Map<String, Integer> getWaitRegistry() {
        return Collections.unmodifiableMap(waitRegistry);
    }

    // ---------------------------------------------------------------------
    // ViewChangeRebuildHook (EP5 "Dynamic Reconstruction")
    //
    // Called on the NEW leader from Synchronizer.catch_up() between
    // ClientsManager.resetAlreadyProposed() and TOMLayer.createPropose().
    // Purpose: replace whatever stale PROPOSE_ALL the deposed Byzantine leader
    // queued (the bytes that triggered Check 7 and hence the view-change) with
    // a fresh proposal rebuilt from the current C++ collectedCerts ground truth.
    // Without this, catch_up would replay the censored bytes and the liveness
    // stall would persist within epoch e.
    // ---------------------------------------------------------------------

    /** Synthetic clientId used exclusively for hook-injected fresh proposals;
     *  kept off the live ServiceProxy's sequence space (processId + 1000). */
    private static final int REBUILD_CLIENT_ID_OFFSET = 2000;

    @Override
    public void rebuildPendingProposals(ClientsManager cm, int regency) {
        String freshPayload = getFreshProposePayload();
        if (freshPayload == null || freshPayload.isEmpty()) {
            System.out.println("[REBUILD " + processId + "] JNI ground truth unavailable; "
                    + "falling back to replay of pending bytes (regency=" + regency + ")");
            return;
        }

        byte[] freshRequest = buildFreshProposeAllBytes(freshPayload);
        if (freshRequest == null) {
            System.err.println("[REBUILD " + processId + "] could not assemble fresh PROPOSE_ALL "
                    + "from payload '" + freshPayload + "'; falling back to replay");
            return;
        }

        int evicted = evictAllPendingProposeAll(cm);
        System.out.println("[REBUILD " + processId + "] evicted " + evicted
                + " stale pending request(s) across all clients");

        boolean accepted = injectFreshProposeAll(cm, freshRequest, regency);
        System.out.println("[REBUILD " + processId + "] injected fresh PROPOSE_ALL accepted="
                + accepted + " len=" + freshRequest.length + " regency=" + regency);
    }

    @Override
    public void evictStaleProposals(ClientsManager cm, int regency) {
        // Runs on every replica at the top of Synchronizer.finalise() during
        // view-change recovery. The new leader has already evicted in
        // rebuildPendingProposals (called from catch_up); calling this again
        // returns 0 evictions and is a safe no-op. Followers, however, reach
        // finalise() via processSYNC() and need this call so their
        // RequestsTimer stops watching the deposed Byzantine PROPOSE_ALL —
        // otherwise the timer fires T_request later and forces a needless
        // leader-change to regency+1.
        int evicted = evictAllPendingProposeAll(cm);
        System.out.println("[REBUILD " + processId + "] follower-side evict: removed "
                + evicted + " stale pending request(s) (regency=" + regency + ")");
    }

    /**
     * Instance wrapper: assembles fresh PROPOSE_ALL bytes using this replica's
     * {@code processId}, {@code roundNumber}, {@code waitRegistry}, and departed set.
     */
    byte[] buildFreshProposeAllBytes(String jniPayload) {
        return buildFreshProposeAllBytes(
                jniPayload,
                this.processId,
                (int) this.roundNumber,
                this.waitRegistry,
                this.departedReplicas);
    }

    /**
     * Pure static primitive for view-change reconstruction: parses the JNI-fetched
     * ground truth {@code <vehicleStatesStr>:<perCarCerts>}, filters out departed
     * replicas, re-runs {@link OrderScheduler#buildProposal} with the caller-provided
     * {@code waitRegistry}, and returns the fully-assembled
     * {@code PROPOSE_ALL:<proposerId>:<vsStr>:<pcc>:<orderBagStr>} wire bytes.
     *
     * <p>Returns {@code null} when the JNI payload is malformed/empty or all
     * vehicles are departed — callers must treat null as "no fresh build possible;
     * fall back to replay of pending bytes".
     *
     * <p>Side-effect-free so the self-test can invoke it without a live
     * ServiceReplica / JNI environment.
     */
    static byte[] buildFreshProposeAllBytes(
            String jniPayload,
            int proposerId,
            int epoch,
            Map<String, Integer> waitRegistry,
            Set<Integer> departedReplicas) {
        if (jniPayload == null || jniPayload.isEmpty()) {
            return null;
        }
        int split = jniPayload.indexOf(':');
        if (split < 0) {
            return null;
        }
        String vehicleStatesStr = jniPayload.substring(0, split);
        String perCarCertsStr   = jniPayload.substring(split + 1);

        List<VehicleState> states = ViewConsensusProtocol.parseVehicleStates(vehicleStatesStr);
        if (states.isEmpty()) {
            return null;
        }
        Map<String, VehicleState> stateMap = new LinkedHashMap<>();
        for (VehicleState vs : states) {
            try {
                int rid = Integer.parseInt(vs.vehicleId.substring(3));
                if (departedReplicas == null || !departedReplicas.contains(rid)) {
                    stateMap.put(vs.vehicleId, vs);
                }
            } catch (NumberFormatException ex) {
                stateMap.put(vs.vehicleId, vs);
            }
        }
        if (stateMap.isEmpty()) {
            return null;
        }

        Map<String, Integer> wr = (waitRegistry != null) ? waitRegistry : Collections.emptyMap();
        OrderBag bag = OrderScheduler.buildProposal(stateMap, epoch, wr);
        String orderBagStr = OrderScheduler.serializeOrderBagForBFT(bag);
        String fullRequest = "PROPOSE_ALL:" + proposerId + ":"
                + vehicleStatesStr + ":" + perCarCertsStr + ":" + orderBagStr;
        return fullRequest.getBytes(StandardCharsets.UTF_8);
    }

    /**
     * Purges every pending request across every known client. Safe in the V2V
     * single-round protocol: the only thing in-flight at a view-change is the
     * deposed leader's stale PROPOSE_ALL (replicas do not pipeline requests).
     * Returns the total number of requests removed.
     */
    private int evictAllPendingProposeAll(ClientsManager cm) {
        int total = 0;
        for (int cid : cm.getKnownClientIds()) {
            total += cm.removePendingForClient(cid);
        }
        return total;
    }

    /**
     * Injects {@code freshBytes} as a new pending request under a dedicated
     * rebuild clientId so we do not collide with the live {@code localClientProxy}
     * sequence space. Uses {@code fromClient=false} in
     * {@link ClientsManager#requestReceived} to bypass the client-signature check
     * (acceptable since we are the BFT leader and this is an intra-replica
     * synthetic submission, analogous to BFT-SMaRt's own forwarded-request path).
     */
    private boolean injectFreshProposeAll(ClientsManager cm, byte[] freshBytes, int regency) {
        int rebuildClientId = processId + REBUILD_CLIENT_ID_OFFSET;
        int session = 0;
        int sequence = regency; // monotonically increasing with each view-change
        int viewId = (replica != null && replica.getReplicaContext() != null)
                ? replica.getReplicaContext().getSVController().getCurrentViewId()
                : 0;
        TOMMessage fresh = new TOMMessage(
                rebuildClientId, session, sequence, sequence,
                freshBytes, viewId, TOMMessageType.ORDERED_REQUEST);
        fresh.signed = false;
        fresh.serializedMessage = TOMMessage.messageToBytes(fresh);
        return cm.requestReceived(fresh, false, null);
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
            agreedViewState = (Map<String, VehicleState>) in.readObject();
            Map<String, Integer> restoredWaitRegistry = (Map<String, Integer>) in.readObject();
            roundNumber = in.readLong();

            waitRegistry.clear();
            waitRegistry.putAll(restoredWaitRegistry);
            proposeAllSubmitted = false;

            System.out.println("[STATE] Snapshot installed. agreedView="
                    + (agreedViewState != null ? agreedViewState.size() : 0)
                    + ", waitRegistry=" + waitRegistry.size()
                    + ", round=" + roundNumber);
        } catch (IOException | ClassNotFoundException e) {
            System.err.println("[ERROR] Error deserializing state: " + e.getMessage());
            agreedViewState = null;
            waitRegistry.clear();
            proposeAllSubmitted = false;
            roundNumber = 0;
        }
    }

    @Override
    public byte[] getSnapshot() {
        try (ByteArrayOutputStream bos = new ByteArrayOutputStream();
                ObjectOutput out = new ObjectOutputStream(bos)) {
            out.writeObject(agreedViewState);
            out.writeObject(new HashMap<>(waitRegistry));
            out.writeLong(roundNumber);
            out.flush();
            System.out.println("[STATE] Snapshot taken. agreedView="
                    + (agreedViewState != null ? agreedViewState.size() : 0)
                    + ", waitRegistry=" + waitRegistry.size()
                    + ", round=" + roundNumber);
            return bos.toByteArray();
        } catch (IOException ioe) {
            System.err.println("[ERROR] Error serializing state: " + ioe.getMessage());
            return "ERROR".getBytes(StandardCharsets.UTF_8);
        }
    }
}

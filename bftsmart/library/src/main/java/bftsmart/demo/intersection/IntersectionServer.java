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

import bftsmart.communication.V2V.SimulationClock;
import bftsmart.tom.MessageContext;
import bftsmart.tom.ServiceProxy;
import bftsmart.tom.ServiceReplica;
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
 * BFT replicated service for the active VIEW/ORDER intersection protocol.
 */
public final class IntersectionServer extends DefaultRecoverable {
    private static final Map<Integer, IntersectionServer> readyServers = new ConcurrentHashMap<>();
    private static final int BATCH_SIZE = 16;
    private static final int CONSENSUS_REQUEST_TIMEOUT_SEC = 3600;

    private long roundNumber = 0;
    private final int processId;
    private final ServiceReplica replica;
    private final Map<String, Integer> waitRegistry = new HashMap<>();
    private final long experimentStartWall = System.currentTimeMillis();
    private final Set<Integer> departedReplicas = new HashSet<>();
    private final Object departedLock = new Object();

    private volatile boolean orderProposeSubmitted = false;
    private volatile boolean viewPhaseComplete = false;
    private volatile boolean orderPhaseComplete = false;
    private volatile int lastLoggedViewCid = -1;
    volatile Map<String, VehicleState> agreedViewState = null;

    private ServiceProxy localClientProxy = null;
    private long viewConsensusStartWall;
    private long viewConsensusEndWall;
    private long orderConsensusStartWall;
    private long orderConsensusEndWall;

    /** Notify C++ that wipeAndReinit completed; C++ will then command re-announce. */
    private native void notifyWipeComplete(int processId);

    /** Notify C++ that VIEW consensus completed. */
    private native void notifyViewAgreed(int replicaId, String viewMembers);

    /** Notify C++ that ORDER consensus completed. */
    private native void notifyOrderDecided(int replicaId, String orderDecision);

    /**
     * Deprecated compatibility callback kept so the JNI registration still matches
     * the native bridge, even though the active protocol does not use GO delays.
     */
    @SuppressWarnings("unused")
    private native void notifyVehicleCanGo(int replicaId, double delaySeconds);

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
            this.replica = new ServiceReplica(id, this, this, new OrderRequestVerifier(this));
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

    /**
     * Check if a replica is departed (zombie).
     */
    public boolean isReplicaDeparted(int replicaId) {
        synchronized (departedLock) {
            return departedReplicas.contains(replicaId);
        }
    }

    /**
     * Batch size updates are informational now; the active protocol derives quorum
     * from the view carried in each request.
     */
    public void updateBatchSize(int batchSize) {
        int faultTolerance = Math.max(0, (batchSize - 1) / 3);
        System.out.println("[BATCH] Updated active batch size hint: " + batchSize);
        System.out.println("[BATCH] Derived fault tolerance hint: f=" + faultTolerance);
        System.out.println("[BATCH] Derived quorum hint: 2f+1=" + (2 * faultTolerance + 1));
    }

    /**
     * Called by OMNeT++ via JNI with VIEW_PROPOSE or ORDER_PROPOSE request.
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
     * Send consensus request (VIEW_PROPOSE or ORDER_PROPOSE) and handle response.
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

        if (request.contains("ORDER_PROPOSE")) {
            if (orderProposeSubmitted) {
                System.out.println("[SERVER " + processId
                        + "] ORDER_PROPOSE already submitted this round; skipping duplicate.");
                return;
            }
            orderProposeSubmitted = true;
        }

        System.out.println("[SERVER " + this.processId + "] >>> Calling invokeOrdered...");

        try {
            if (request.contains("VIEW_PROPOSE")) {
                bftsmart.communication.V2V.ReliableV2VMessaging.globalResetV2V(null);
                viewConsensusStartWall = System.currentTimeMillis();
                System.out.println("[INVOKE_START " + processId + "] wall_offset="
                        + (System.currentTimeMillis() - experimentStartWall) + "ms");
            }
            if (request.contains("ORDER_PROPOSE")) {
                orderConsensusStartWall = System.currentTimeMillis();
            }

            long proxyStart = System.nanoTime();
            byte[] reply;
            try {
                reply = invokeOrderedWithTimeout(this.localClientProxy, request.getBytes(StandardCharsets.UTF_8));
            } catch (NullPointerException npe) {
                System.err.println("[SERVER " + processId
                        + "] Netty NPE on stale proxy; nulling for fresh creation on next request.");
                try {
                    this.localClientProxy.close();
                } catch (Exception ignored) {
                }
                this.localClientProxy = null;
                return;
            }
            long proxyEnd = System.nanoTime();
            System.out.println("[PROFILING " + processId + "] proxy.invokeOrdered took: "
                    + ((proxyEnd - proxyStart) / 1_000_000.0) + " ms");
            if (reply == null) {
                return;
            }

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

            if (replyStr.startsWith("VIEW_AGREED:")) {
                String viewStr = replyStr.substring("VIEW_AGREED:".length());
                if (viewStr.isEmpty()) {
                    viewStr = "[]";
                }
                System.out.println("Processing request. Active View: " + viewStr);
                try {
                    notifyViewAgreed(this.processId, viewStr);
                    System.out.println("[VIEW] Notified C++ of agreed view for replica " + this.processId);
                } catch (UnsatisfiedLinkError e) {
                    System.err.println("[VIEW] Warning: Could not notify C++ (JNI not available): "
                            + e.getMessage());
                }
            } else if (replyStr.startsWith("veh")) {
                System.out.println("[SERVER " + processId
                        + "] ORDER reply observed; batch execution is driven by JNI delivery callbacks.");
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
                if (this.localClientProxy != null) {
                    this.localClientProxy.close();
                }
            } catch (Exception ignored) {
            }
            this.localClientProxy = null;
        }
    }

    /**
     * Call proxy.invokeOrdered with a timeout.
     */
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

    /**
     * Called by the leader thread spawned from VIEW consensus delivery to submit
     * ORDER_PROPOSE via localClientProxy.
     */
    private void submitOrderPropose(Map<String, VehicleState> view, int epoch) {
        try {
            OrderBag bag = OrderScheduler.buildProposal(view, epoch, waitRegistry);
            String payload = "ORDER_PROPOSE:" + OrderScheduler.serializeOrderBagForBFT(bag);
            System.out.println("[LEADER] Submitting ORDER_PROPOSE: " + payload);

            if (localClientProxy == null) {
                int clientId = this.processId + 1000;
                localClientProxy = new ServiceProxy(clientId);
                Thread.sleep(80);
            }
            byte[] result = localClientProxy.invokeOrdered(payload.getBytes(StandardCharsets.UTF_8));
            if (result != null) {
                System.out.println("[LEADER] ORDER_PROPOSE completed, reply="
                        + new String(result, StandardCharsets.UTF_8));
            }
        } catch (Exception e) {
            System.err.println("[LEADER] submitOrderPropose failed: " + e.getMessage());
        }
    }

    private void resetForNextRound() {
        viewPhaseComplete = false;
        orderPhaseComplete = false;
        orderProposeSubmitted = false;
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
        roundNumber++;
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

            for (Cmd cmd : decoded) {
                if (viewPhaseComplete || cmd.type != Cmd.Type.VIEW_PROPOSE) {
                    continue;
                }
                if (cmd.payload == null || cmd.payload.equals("NONE") || isReplicaDeparted(processId)) {
                    continue;
                }

                try {
                    String[] parts = cmd.payload.split(":", 3);
                    if (parts.length < 3) {
                        continue;
                    }

                    ViewProposal proposal = new ViewProposal();
                    proposal.proposerReplicaId = Integer.parseInt(parts[0].trim());
                    proposal.vehicleStates = ViewConsensusProtocol.parseVehicleStates(parts[1]);
                    proposal.v2vSignatures = ViewConsensusProtocol.parseViewSignatures(parts[2]);

                    if (!ViewConsensusProtocol.validateViewProposal(proposal)) {
                        continue;
                    }

                    Map<String, VehicleState> newViewState = new LinkedHashMap<>();
                    for (VehicleState vehicleState : proposal.vehicleStates) {
                        int replicaId = Integer.parseInt(vehicleState.vehicleId.substring(3));
                        if (!isReplicaDeparted(replicaId)) {
                            newViewState.put(vehicleState.vehicleId, vehicleState);
                        }
                    }

                    this.agreedViewState = newViewState;
                    this.viewPhaseComplete = true;

                    String resultString = String.join(",", new TreeSet<>(newViewState.keySet()));
                    System.out.println("[SERVER] VIEW CONSENSUS REACHED. Cars=" + resultString);
                    System.out.println("[SERVER] wall_offset="
                            + (System.currentTimeMillis() - experimentStartWall) + "ms");

                    try {
                        notifyViewAgreed(this.processId, resultString);
                    } catch (UnsatisfiedLinkError e) {
                        System.err.println("[VIEW] JNI notifyViewAgreed unavailable: " + e.getMessage());
                    }

                    final Map<String, VehicleState> frozenView = Collections.unmodifiableMap(newViewState);
                    final int frozenEpoch = (int) roundNumber;
                    int minId = newViewState.keySet().stream()
                            .mapToInt(id -> Integer.parseInt(id.substring(3)))
                            .min()
                            .orElse(-1);
                    if (this.processId == minId) {
                        new Thread(() -> {
                            try {
                                Thread.sleep(this.processId * 2L + 20L);
                                submitOrderPropose(frozenView, frozenEpoch);
                            } catch (Exception ex) {
                                System.err.println("[ORDER] Leader ORDER_PROPOSE thread failed: "
                                        + ex.getMessage());
                            }
                        }, "order-propose-" + processId).start();
                    }

                    break;
                } catch (Exception e) {
                    System.err.println("[VIEW] Error parsing VIEW_PROPOSE in scan phase: " + e.getMessage());
                }
            }

            for (int i = 0; i < commands.length; i++) {
                Cmd cmd = decoded[i];
                String reply;

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
                        if (isReplicaDeparted(processId)) {
                            reply = "DEPARTED";
                        } else if (viewPhaseComplete && !orderPhaseComplete) {
                            OrderBag bag = OrderScheduler.parseOrderBag(cmd.payload);
                            if (bag != null) {
                                this.orderPhaseComplete = true;
                                triggerRoundReset = true;

                                String batchDecision = OrderScheduler.serializeOrderBagForJNI(bag);
                                System.out.println("[ORDER] Committed OrderBag epoch=" + bag.epoch
                                        + " batches=" + bag.batches.size()
                                        + " decision=" + batchDecision);

                                try {
                                    notifyOrderDecided(processId, batchDecision);
                                } catch (UnsatisfiedLinkError e) {
                                    System.err.println("[ORDER] JNI notifyOrderDecided unavailable: "
                                            + e.getMessage());
                                }
                                if (agreedViewState != null) {
                                    for (String vehicleId : agreedViewState.keySet()) {
                                        int replicaId = Integer.parseInt(vehicleId.substring(3));
                                        if (replicaId == processId) {
                                            continue;
                                        }
                                        try {
                                            notifyOrderDecided(replicaId, batchDecision);
                                        } catch (UnsatisfiedLinkError ignored) {
                                        }
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

                    default:
                        reply = "ERROR:UNKNOWN COMMAND: " + cmd.type;
                        break;
                }

                replies[i] = reply.getBytes(StandardCharsets.UTF_8);
            }

            double appTimeMs = (System.nanoTime() - appStartTime) / 1_000_000.0;
            if (appTimeMs < 0) {
                System.out.println("[PROFILE] appExecuteBatch timer underflow");
            }
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
            VIEW_PROPOSE,
            ORDER_PROPOSE
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
            viewPhaseComplete = in.readBoolean();
            orderPhaseComplete = in.readBoolean();
            roundNumber = in.readLong();

            waitRegistry.clear();
            waitRegistry.putAll(restoredWaitRegistry);
            orderProposeSubmitted = orderPhaseComplete;

            System.out.println("[STATE] Snapshot installed. agreedView="
                    + (agreedViewState != null ? agreedViewState.size() : 0)
                    + ", waitRegistry=" + waitRegistry.size()
                    + ", round=" + roundNumber);
        } catch (IOException | ClassNotFoundException e) {
            System.err.println("[ERROR] Error deserializing state: " + e.getMessage());
            agreedViewState = null;
            waitRegistry.clear();
            viewPhaseComplete = false;
            orderPhaseComplete = false;
            orderProposeSubmitted = false;
            roundNumber = 0;
        }
    }

    @Override
    public byte[] getSnapshot() {
        try (ByteArrayOutputStream bos = new ByteArrayOutputStream();
                ObjectOutput out = new ObjectOutputStream(bos)) {
            out.writeObject(agreedViewState);
            out.writeObject(new HashMap<>(waitRegistry));
            out.writeBoolean(viewPhaseComplete);
            out.writeBoolean(orderPhaseComplete);
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

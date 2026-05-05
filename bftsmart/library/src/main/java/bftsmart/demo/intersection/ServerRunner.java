package bftsmart.demo.intersection;

import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.io.File;
import bftsmart.communication.V2V.SimulationClock;

/**
 * Wrapper to run IntersectionServer in a separate Java thread.
 * This prevents blocking the OMNeT++ simulation during BFTSmart initialization.
 */
public class ServerRunner implements Runnable {
    private final int replicaId;
    private final int numCars;
    private IntersectionServer server;
    private volatile String initStatus = "Pending";
    private static final int BATCH_SIZE = 8;

    // Static registry so OMNeT++ can find servers by replica ID
    private static final ConcurrentHashMap<Integer, ServerRunner> registry = new ConcurrentHashMap<>();

    // COORDINATION BARRIER: All replicas wait here before creating ServiceReplica
    private static final CountDownLatch allRunnersReady = new CountDownLatch(BATCH_SIZE); // Wait for 8 replicas
    // private static final CountDownLatch startSignal = new CountDownLatch(1); //
    // Released when all are ready
    private static final CountDownLatch allTCPServersReady = new CountDownLatch(BATCH_SIZE);

    public ServerRunner(int replicaId, int numCars) {
        this.replicaId = replicaId;
        this.numCars = numCars;
        registry.put(replicaId, this);
        System.out.println("[ServerRunner " + replicaId + "] Created, ready to start in thread");
    }

    private void cleanSavedState() {
        try {
            String workingDir = System.getProperty("user.dir");
            File configDir = new File(workingDir, "config");

            // BFT-SMaRt stores views in 'currentView' or 'currentView.{id}'
            File[] files = configDir.listFiles();
            if (files != null) {
                for (File f : files) {
                    if (f.getName().startsWith("currentView")) {
                        boolean deleted = deleteRecursive(f);
                        System.out.println("[ServerRunner] DELETED stale state: " + f.getName() + " (" + deleted + ")");
                    }
                }
            }
        } catch (Exception e) {
            System.err.println("[ServerRunner] Warning: Could not clean state: " + e.getMessage());
        }
    }

    private boolean deleteRecursive(File file) {
        if (file.isDirectory()) {
            File[] children = file.listFiles();
            if (children != null) {
                for (File child : children)
                    deleteRecursive(child);
            }
        }
        return file.delete();
    }

    @Override
    public void run() {

        initStatus = "WaitingForPeers";
        System.out.println("[ServerRunner " + replicaId + "] Thread started, waiting for all peers...");

        try {
            // PHASE 1: Signal that this runner is ready
            allRunnersReady.countDown();
            System.out.println("[ServerRunner " + replicaId + "] Signaled ready diod thios build  (" +
                    (BATCH_SIZE - allRunnersReady.getCount()) + "/" + BATCH_SIZE + " ready)");

            // PHASE 2: Wait for all replicas to reach this point (with timeout)
            // Generous timeout to test if initialization is the bottleneck
            boolean allReady = allRunnersReady.await(120, TimeUnit.SECONDS);
            if (!allReady) {
                initStatus = "ERROR: Timeout waiting for peers (only " +
                        (BATCH_SIZE - allRunnersReady.getCount()) + "/" + BATCH_SIZE + " ready)";
                System.err.println("[ServerRunner " + replicaId + "] " + initStatus);
                return;
            }

            // PHASE 3: All replicas ready, but DON'T stagger here!
            // Problem: Staggering causes callbacks to register at different times
            // Solution: Let C++ rate-limiting handle collision avoidance

            if (replicaId == 0) { // Only one node needs to do this, or all can try
                cleanSavedState();
            }

            // STAGGER: Add random startup delay to reduce V2V collision storms
            // long startupDelay = new java.util.Random().nextInt(100); // 0 to 2 seconds
            // delay
            // System.out.println("[ServerRunner " + replicaId + "] Staggering startup by "
            // + startupDelay + "ms to reduce collisions...");
            // Thread.sleep(100);

            initStatus = "Initializing";
            System.out.println("[ServerRunner " + replicaId + "] Creating IntersectionServer after stagger delay...");

            // This will block during BFT consensus initialization, but now all peers are
            // listening
            this.server = new IntersectionServer(replicaId, numCars);
            allTCPServersReady.countDown();
            System.out.println("[ServerRunner " + replicaId + "] TCP server ready ("
                    + (BATCH_SIZE - allTCPServersReady.getCount()) + "/" + BATCH_SIZE + " TCP servers ready)");
            boolean allTCPReady = allTCPServersReady.await(120, TimeUnit.SECONDS);
            if (!allTCPReady) {
                initStatus = "ERROR: Timeout waiting for TCP servers (only "
                        + (BATCH_SIZE - allTCPServersReady.getCount()) + "/" + BATCH_SIZE + " ready)";
                System.err.println("[ServerRunner " + replicaId + "] " + initStatus);
                return;
            }
            System.out.println("[ServerRunner " + replicaId + "] All " + BATCH_SIZE + " TCP servers confirmed ready!");
            initStatus = "READY";
            System.out.println("[ServerRunner " + replicaId + "] ========================================");
            System.out.println("[ServerRunner " + replicaId + "] *** REPLICA IS NOW READY ***");
            System.out.println("[ServerRunner " + replicaId + "] *** SimTime: " + SimulationClock.currentTimeMillis() + " ms");
            System.out.println("[ServerRunner " + replicaId + "] ========================================");

        } catch (InterruptedException e) {
            initStatus = "ERROR: Interrupted during startup";
            System.err.println("[ServerRunner " + replicaId + "] Interrupted: " + e.getMessage());
            Thread.currentThread().interrupt();
        } catch (Throwable e) {
            initStatus = "ERROR: " + e.toString();
            System.err.println("[ServerRunner " + replicaId + "] CRITICAL ERROR:");
            e.printStackTrace();
        }
    }

    public static String getStatus(int replicaId) {
        ServerRunner runner = registry.get(replicaId);
        if (runner == null)
            return "No Runner Found";
        return runner.initStatus;
    }

    public IntersectionServer getServer() {
        return server;
    }

    /**
     * Called by OMNeT++ via JNI to trigger a consensus request (VIEW_PROPOSE or
     * ORDER_PROPOSE).
     * This is the simulation-time-driven way to start consensus.
     * 
     * @param request The request string (e.g., "VIEW_PROPOSE:veh0:lane:pos:..." or
     *                "ORDER_PROPOSE")
     */
    public void triggerConsensusRequest(String request) {
        if (server != null) {
            server.triggerConsensusRequest(request);
        } else {
            System.err.println("[ServerRunner " + replicaId + "] Cannot trigger consensus - server not ready yet!");
        }
    }

    /**
     * Static method for OMNeT++ to call consensus trigger on a specific replica.
     * 
     * @param replicaId The replica ID
     * @param request   The request string (VIEW_PROPOSE or ORDER_PROPOSE with data)
     */
    public static void triggerJoinForReplica(int replicaId, String request) {
        ServerRunner runner = registry.get(replicaId);
        if (runner != null) {
            runner.triggerConsensusRequest(request);
        } else {
            System.err.println("[ServerRunner] No runner found for replica " + replicaId);
        }
    }

    /**
     * Called by C++ via JNI when a vehicle has departed (crossed intersection).
     * Marks the replica as zombie so it stops participating in proposals.
     * 
     * @param replicaId The replica ID that departed
     */
    public static void notifyVehicleDeparted(int replicaId) {
        System.out.println("[ServerRunner] notifyVehicleDeparted called for replica " + replicaId);

        ServerRunner runner = registry.get(replicaId);
        if (runner != null && runner.server != null) {
            runner.server.markReplicaDeparted(replicaId);
            System.out.println("[ServerRunner] Replica " + replicaId + " departed (zombie mode activated)");
        } else {
            System.err.println("[ServerRunner] Cannot mark replica " + replicaId +
                    " as departed - server not found");
            System.err.println("[ServerRunner] Registry contains: " + registry.keySet());
        }
    }

    /**
     * Called by C++ via JNI to notify the actual batch size after collection phase.
     * 
     * @param replicaId The replica reporting the batch size
     * @param batchSize The number of cars detected in the batch
     */
    public static void notifyBatchSize(int replicaId, int batchSize) {
        System.out.println("[ServerRunner] Replica " + replicaId + " detected batch size: " + batchSize);

        ServerRunner runner = registry.get(replicaId);
        if (runner != null && runner.server != null) {
            runner.server.updateBatchSize(batchSize);
        } else {
            System.err.println("[ServerRunner] Cannot update batch size - server " + replicaId + " not found");
        }
    }

    /**
     * Check if the server is ready (fully initialized).
     */
    public boolean isReady() {
        return server != null;
    }

    /**
     * Static method to check if a specific replica is ready.
     */
    public static boolean isReplicaReady(int replicaId) {
        ServerRunner runner = registry.get(replicaId);
        return runner != null && runner.isReady();
    }

    /**
     * Get the count of how many runners are waiting at the barrier.
     * Useful for debugging coordination issues.
     */
    public static String getBarrierStatus() {
        long waiting = BATCH_SIZE - allRunnersReady.getCount();
        return waiting + "/" + BATCH_SIZE + " runners ready";
    }

    /**
     * Called by C++ via JNI on epoch preemption.
     * Resets BFT state and reconfigures for the new participant set.
     * C++ must call this before commanding re-announcement.
     *
     * @param replicaId      The replica being wiped
     * @param newParticipants Array of replica IDs that will form the new epoch
     */
    public static void wipeAndReinitForReplica(int replicaId, int[] newParticipants) {
        System.out.println("[ServerRunner] wipeAndReinitForReplica called for replica " + replicaId
                + " newN=" + newParticipants.length);
        ServerRunner runner = registry.get(replicaId);
        if (runner != null && runner.server != null) {
            runner.server.doWipeAndReinit(newParticipants);
        } else {
            System.err.println("[ServerRunner] Cannot wipe — server " + replicaId + " not found");
        }
    }
}

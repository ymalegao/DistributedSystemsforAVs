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
    
    // Old fields (kept for compatibility)
    private Map<String, Integer> waitMap = new HashMap<>(); // carId -> wait
    private String lastLeaver = null;
    private long roundNumber = 0;
    private int iterations = 0;
    private int processId;
    private ServiceReplica replica;
    private int numCars;
    
    private static final int BATCH_SIZE = 12;

    private Map<String, Integer> waitRegistry = new HashMap<>();

    // Dynamic batch sizing (updated by C++ after clearance tracking)
    private int currentBatchSize = BATCH_SIZE;  // Default 8, updated dynamically
    private int currentF = (BATCH_SIZE - 1) / 3;  // Dynamic fault tolerance (f = 2 initially)

    // Intersection physics parameters for delay calculation
    // These can be configured via system properties or hardcoded
    private double intersectionWidth;   // meters - distance to cross the intersection
    private double avgSpeed;            // m/s - average vehicle speed
    private double safetyGap;           // seconds - buffer time between cars
    private long lastDecisionSimTime = -1;
    private long viewConsensusStartWall;
    private long viewConsensusEndWall;
    private long orderConsensusStartWall;
    private long orderConsensusEndWall;
    private final long experimentStartWall = System.currentTimeMillis();


    // Phase 1: View consensus state
    private Map<Set<String>, List<ViewProposal>> viewProposals = new HashMap<>();  // view -> proposals
    // Volatile: updated by BFT delivery thread, read by proxy/polling thread
    private volatile Set<String> agreedView = null;
    private volatile boolean viewPhaseComplete = false;

    // Phase 2: ReadyQC state (after view established)
    private Map<String, ReadyQCData> verifiedCars = new HashMap<>();


    // Volatile: updated by BFT delivery thread, read by proxy/polling thread
    private volatile List<String> agreedOrder = null;
    private volatile boolean orderPhaseComplete = false;

    private bftsmart.tom.ServiceProxy localClientProxy = null;


    private Set<Integer> departedReplicas = new HashSet<>();  // Replica IDs that have left
    private final Object departedLock = new Object();  // Thread-safe access

    public native void notifyReconfigComplete(int processId);
 
    // View Consensus Data (Phase 1)
    static class ViewProposal implements Serializable {
        int proposerReplicaId;
        Set<String> observedCars;       // Cars visible to this replica
        List<ViewSignature> v2vSignatures;  // f+1 V2V signatures agreeing on this view
    }

    static class ViewSignature implements Serializable {
        int signingReplicaId;
        byte[] signatureBytes;      // Hash of sorted car list
    }

    // ReadyQC Data (Phase 2 - after view established)
    static class ReadyQCData implements Serializable {
        String carId;              // "veh0", "veh1", etc.
        String laneId;             // TraCI lane ID
        double positionInLane;     // Meters from lane start
        double verifiedArrival;    // Earliest witness timestamp
        int epoch;                 // Prevents replay
        List<WitnessSignature> signatures;  // f+1 signatures
    }

    static class WitnessSignature implements Serializable {
        int witnessReplicaId;
        byte[] signatureBytes;     // Mock hash signature
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
            this.replica = new ServiceReplica(id, this, this);
            if (replica != null){
                bftsmart.communication.ServerCommunicationSystem commSystem =
                replica.getServerCommunicationSystem();
                if (commSystem != null) {
                    bftsmart.communication.V2V.V2VServersCommunicationLayer v2vLayer =
                        (bftsmart.communication.V2V.V2VServersCommunicationLayer)
                        commSystem.getServersConn();
                    if (v2vLayer != null) {
                        v2vLayer.setServer(this);
                        System.out.println("[Server " + id + "] Set server reference in V2V layer for zombie filtering");
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
        // System.out.println("[IntersectionServer " + id + "] *** REGISTERED as Ready! ***");
        System.out.println("[IntersectionServer " + id + "] *** Waiting for OMNeT++ trigger. ***");
        long simMs = SimulationClock.currentTimeMillis();
        System.out.println("[IntersectionServer " + id + "] simtime (ms) = " + simMs + " (sec = " + (simMs / 1000.0) + ")");
        System.out.println("[IntersectionServer " + id + "] ========================================");
        
        // NOTE: We no longer auto-trigger sendCarRequest() via Thread.sleep!
        // Instead, OMNeT++ will call triggerJoin() when the car reaches the intersection.
        // This keeps BFT in sync with simulation time.
    }

    public static boolean isServerReady(int id) {
        return readyServers.containsKey(id);
    }



    /**
   * Mark a replica as departed (zombie mode).
   * Called via JNI from C++ when vehicle crosses intersection.
   * @param replicaId The replica ID that departed
   */
    public void markReplicaDeparted(int replicaId) {
        synchronized (departedLock) {
            departedReplicas.add(replicaId);

            // Remove from verifiedCars (no longer active)
            String carId = "veh" + replicaId;
            verifiedCars.remove(carId);

            System.out.println("[ZOMBIE] Replica " + replicaId + " (" + carId +
                            ") marked as DEPARTED");
            System.out.println("[ZOMBIE] Total zombies: " + departedReplicas.size());
            System.out.println("[ZOMBIE] Remaining active cars: " + verifiedCars.size());
        }
    }

     /**
   * Check if a replica is departed (zombie).
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
     * Called via JNI from C++ after clearance tracking determines actual batch size.
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
     * @param request The consensus request (e.g., "VIEW_PROPOSE:veh0:lane:..." or "ORDER_PROPOSE")
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
                System.err.println("[IntersectionServer " + processId + "] Error in triggerConsensusRequest: " + e.getMessage());
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

    /** Timeout for invokeOrdered (seconds). If consensus never delivers, we avoid blocking forever. */
    private static final int CONSENSUS_REQUEST_TIMEOUT_SEC = 3600;

    /**
     * TPWC: Send consensus request (VIEW_PROPOSE or ORDER_PROPOSE) and handle response.
     * If invokeOrdered never returns, the usual cause is consensus not deciding (e.g. replicas
     * stuck in ACCEPT phase or delivery never running). Check logs for [DELIVERY] to see if
     * receiveMessages/executeBatch ran; if not, consensus did not complete.
     */
    private void sendConsensusRequest(String request) {
        if (this.localClientProxy == null) {
            int clientId = this.processId + 1000;
            System.out.println("[SERVER " + this.processId + "] Initializing persistent ServiceProxy for client " + clientId);
            this.localClientProxy = new bftsmart.tom.ServiceProxy(clientId);
            
            // Give Netty a tiny buffer to establish initial TCP connections on first boot
            try {
                Thread.sleep(250);
            } catch (InterruptedException e) { }
        }

        if (isReplicaDeparted(processId)) {
            System.out.println("[SERVER " + processId + "] Departed — ignoring consensus request.");
            return;
        }
        
        System.out.println("[SERVER " + this.processId + "] >>> Calling invokeOrdered...");

        try {
            if (request.contains("VIEW_PROPOSE")) {
                viewConsensusStartWall = System.currentTimeMillis();
            }
            if (request.contains("ORDER_PROPOSE")) {
                orderConsensusStartWall = System.currentTimeMillis();
            }
            byte[] reply = invokeOrderedWithTimeout(this.localClientProxy, request.getBytes(StandardCharsets.UTF_8));
                if (reply == null) return;

                if (request.contains("VIEW_PROPOSE")) {
                    
                    viewConsensusEndWall = System.currentTimeMillis();
                    System.out.println("[BFTCONSENSUS " + this.processId + "] View consensus time: " + (viewConsensusEndWall - viewConsensusStartWall) + "ms");
                }
                
                    
                if (request.contains("ORDER_PROPOSE")) {
                    orderConsensusEndWall = System.currentTimeMillis();
                    System.out.println("[BFTCONSENSUS " + this.processId + "] Order consensus time: " + (orderConsensusEndWall - orderConsensusStartWall) + "ms");
                }

            String replyStr = new String(reply, StandardCharsets.UTF_8);
            System.out.println("[SERVER " + this.processId + "] Consensus reply: " + replyStr);


            // while (replyStr.contains("BUFFERING")) {
            //     System.out.println("[SERVER " + processId + "] Waiting for peer proposals...");
            //     Thread.sleep(300); // Wait for other replicas to send their VIEW_PROPOSE
            //     System.out.println("[SERVER " + processId + "] Waiting for other replicas to send their VIEW_PROPOSE...");
            //     // Poll local volatile state updated by the BFT delivery thread.
            //     // This avoids extra GET_STATE traffic and ensures the proxy thread observes quorum once reached.
                
            //     if (replyStr.contains("ORDER") && orderPhaseComplete && finalDecision != null) {
            //         replyStr = finalDecision;
            //         break;
            //     }

            //     if (replyStr.contains("VIEW") && viewPhaseComplete && agreedView != null) {
            //         replyStr = "VIEW_AGREED:" + String.join(",", agreedView);
            //         break;
            //     }
                
               
            // }

            // Handle response based on type
            if (replyStr.startsWith("VIEW_AGREED:")) {
                // VIEW consensus complete - trigger ORDER consensus
                System.out.println("[SERVER " + this.processId + "] VIEW consensus complete, triggering ORDER in c++...");
                
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
                // byte[] orderReply = invokeOrderedWithTimeout(proxy, orderReq.getBytes(StandardCharsets.UTF_8));
                // if (orderReply == null) return;
                // String orderReplyStr = new String(orderReply, StandardCharsets.UTF_8);
                // System.out.println("[SERVER " + processId + "] ORDER reply: " + orderReplyStr);

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
        }
    }

    /**
     * Call proxy.invokeOrdered with a timeout. Returns null on timeout so caller can exit instead of blocking forever.
     */
    private byte[] invokeOrderedWithTimeout(ServiceProxy proxy, byte[] requestBytes) {
        ExecutorService executor = Executors.newSingleThreadExecutor();
        try {
            Future<byte[]> future = executor.submit(() -> proxy.invokeOrdered(requestBytes));
            return future.get(CONSENSUS_REQUEST_TIMEOUT_SEC, TimeUnit.SECONDS);
        } catch (TimeoutException e) {
            System.err.println("[SERVER " + processId + "] >>> invokeOrdered TIMED OUT after " + CONSENSUS_REQUEST_TIMEOUT_SEC + "s. Consensus may not be completing (check if [DELIVERY] appears in replica logs).");
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
                            System.out.println("[SERVER " + processId + "] >>> Calling notifyVehicleCanGo with delay: " + delaySeconds);
                            notifyVehicleCanGo(processId, delaySeconds);
                            System.out.println("[SERVER " + processId + "] >>> notifyVehicleCanGo returned!");
                        } catch (UnsatisfiedLinkError e) {
                            System.err.println("[SERVER " + processId + "] Warning: Could not call native method: " + e.getMessage());
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
                    System.err.println("[SERVER " + processId + "] Warning: Could not call native method (JNI not available): " + e.getMessage());
                }
            } else {
                System.err.println("[SERVER " + processId + "] ERROR: Could not find my decision in: " + joinReplyStr);
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
     *   - Time for previous car to clear intersection (intersectionWidth / avgSpeed)
     *   - Safety gap (buffer between cars)
     *
     * @param position Queue position (0 = first car)
     * @return Delay in seconds before this vehicle can enter intersection
     */
    private double calculateDelayFromPosition(int position) {
        if (position <= 0) {
            return 0.0;  // First car goes immediately
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
     * @param carEntry The car entry string
     * @return An array containing [carId, direction, arrivalTime] or null if parsing fails
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
            return new String[]{parts[0].trim(), parts[1].trim(), parts[2].trim()};
        } catch (NumberFormatException e) {
            return null;
        }
    }

    private static final class Cmd {
        enum Type { JOIN, LEAVE, GET_STATE, VIEW_PROPOSE, ORDER_PROPOSE }
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
    
   

    private byte[][] generateByzantineResponse(byte[][] commands, java.util.List<String[]> allCars, String honestCarId) {
    
        System.err.println("[BYZANTINE] Replica " + processId + " is lying! Returning malicious response.");
        byte[][] replies = new byte[commands.length][];
        String maliciousCarId = null;
        if (allCars.size() > 0 ){
            maliciousCarId = allCars.get(allCars.size() - 1)[0]; // Last car gets malicious response
            System.err.println("[BYZANTINE] Malicious decision: " + maliciousCarId + " gets GO (WRONG!)");
        }

        for (int i = 0; i < commands.length; i++){
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
                }else{
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
    java.util.List<java.util.Map.Entry<String, Double>> sortedCars = 
        new java.util.ArrayList<>(buffer.entrySet());
    
    // 2. Sort Honestly (Arrival Time)
    sortedCars.sort((a, b) -> {
        int timeCompare = Double.compare(a.getValue(), b.getValue());
        if (timeCompare != 0) return timeCompare;
        return a.getKey().compareTo(b.getKey());
    });

    // 3. Build String: "CAR_0:POS:0;CAR_1:POS:1..."
    StringBuilder sb = new StringBuilder();
    for (int pos = 0; pos < sortedCars.size(); pos++) {
        if (pos > 0) sb.append(";");
        String carId = sortedCars.get(pos).getKey();
        sb.append(carId).append(":POS:").append(pos);
    }
    return sb.toString();
}

private String makeByzantineDecision(java.util.Map<String, Double> buffer) {
    System.err.println("[BYZANTINE] Generating MALICIOUS decision...");

    // 1. Get the list (same as honest)
    java.util.List<java.util.Map.Entry<String, Double>> maliciousList = 
        new java.util.ArrayList<>(buffer.entrySet());
    
    // 2. Sort Honestly first...
    maliciousList.sort((a, b) -> {
        int timeCompare = Double.compare(a.getValue(), b.getValue());
        if (timeCompare != 0) return timeCompare;
        return a.getKey().compareTo(b.getKey());
    });
    
    // 3. ...THEN REVERSE IT (The Lie)
    // The last car (Car 3) becomes First (Pos 0)
    java.util.Collections.reverse(maliciousList);

    // 4. Build the Lying String
    StringBuilder sb = new StringBuilder();
    for (int pos = 0; pos < maliciousList.size(); pos++) {
        if (pos > 0) sb.append(";");
        String carId = maliciousList.get(pos).getKey();
        // We assign them the position in the REVERSED list
        sb.append(carId).append(":POS:").append(pos);
    }
    
    String lie = sb.toString();
    System.err.println("[BYZANTINE] The Lie is: " + lie);
    return lie;
}

// === View Consensus Helper Methods (Phase 1) ===

private List<ViewSignature> parseViewSignatures(String sigString) {
    // Format: "replicaId,hash|replicaId,hash|..."
    List<ViewSignature> signatures = new ArrayList<>();
    
    if (sigString == null || sigString.isEmpty()) {
        return signatures;
    }
    
    String[] sigParts = sigString.split("\\|");
    for (String sigPart : sigParts) {
        String[] fields = sigPart.split(",");
        if (fields.length >= 2) {
            try {
                ViewSignature sig = new ViewSignature();
                sig.signingReplicaId = Integer.parseInt(fields[0]);
                
                // Convert decimal string to 4-byte array (XXHash32)
                long hashValue = Long.parseLong(fields[1].trim());
                ByteBuffer buffer = ByteBuffer.allocate(4);
                buffer.order(ByteOrder.LITTLE_ENDIAN);
                buffer.putInt((int)hashValue);
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
    int f = currentF;
    
    // Check we have f+1 V2V signatures
    if (proposal.v2vSignatures.size() < f + 1) {
        System.err.println("[VIEW] Insufficient V2V signatures: " + 
                          proposal.v2vSignatures.size() + " (need " + (f+1) + ")");
        return false;
    }
    
    // Verify each V2V signature is a valid hash of the view
    for (ViewSignature sig : proposal.v2vSignatures) {
        if (!verifyViewSignature(proposal.observedCars, sig)) {
            System.err.println("[VIEW] Invalid V2V signature from replica " + sig.signingReplicaId);
            return false;
        }
    }
    
    return true;
}

private boolean verifyViewSignature(Set<String> viewSet, ViewSignature sig) {
    // Create deterministic string from sorted set
    List<String> sorted = new ArrayList<>(viewSet);
    Collections.sort(sorted);
    String viewString = String.join(",", sorted);
    
    // Add replica ID for uniqueness (matches C++ signViewProposal)
    viewString += ":" + sig.signingReplicaId;
    
    // Compute expected hash
    byte[] dataBytes = viewString.getBytes(StandardCharsets.UTF_8);
    int expectedHash = xxhash.hash(dataBytes, 0, dataBytes.length, 0);
    
    // Extract actual hash from signature
    if (sig.signatureBytes == null || sig.signatureBytes.length < 4) {
        return false;
    }
    
    ByteBuffer buffer = ByteBuffer.wrap(sig.signatureBytes);
    buffer.order(ByteOrder.LITTLE_ENDIAN);
    int actualHash = buffer.getInt();
    
    System.out.println("[VIEW_VERIFY] Replica " + sig.signingReplicaId + 
                      " view=\"" + viewString + "\" expected=" + expectedHash + 
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
                buffer.putInt((int)hashValue);
                sig.signatureBytes = buffer.array();

                signatures.add(sig);
            } catch (Exception e) {
                System.err.println("[PARSE] Error parsing signature: " + e.getMessage());
            }
        }
    }

    return signatures;
}

private boolean validateReadyQC(ReadyQCData qc) {
    int f = currentF;
    // Check f+1 signatures
    if (qc.signatures.size() < f + 1) {
        System.err.println("[VIEW] Insufficient signatures for " + qc.carId + ": " +
                          qc.signatures.size() + " (need " + (f+1) + ")");
        return false;
    }

    // Verify each mock signature (hash check)
    for (WitnessSignature sig : qc.signatures) {
        if (!verifyMockSignature(qc, sig)) {
            System.err.println("[VIEW] Invalid signature from witness " + sig.witnessReplicaId);
            return false;
        }
    }

    // Check same-lane ordering invariant
    for (ReadyQCData existing : verifiedCars.values()) {
        if (existing.laneId.equals(qc.laneId)) {
            // Same lane - verify ordering
            if (existing.positionInLane < qc.positionInLane &&
                existing.verifiedArrival > qc.verifiedArrival) {
                System.err.println("[VIEW] ORDERING VIOLATION: " +
                    existing.carId + " (pos=" + existing.positionInLane + ", arr=" + existing.verifiedArrival + ") " +
                    "ahead but arrived later than " + qc.carId +
                    " (pos=" + qc.positionInLane + ", arr=" + qc.verifiedArrival + ")");
                return false;  // Byzantine attempt
            }
        }
    }

    return true;
}


private static int verifyCallCount = 0; 

private boolean verifyMockSignature(ReadyQCData qc, WitnessSignature sig) {
    // Reconstruct hash from fields (must match C++ signWitnessClaim)
    // CRITICAL: Must match C++ std::to_string() formatting (6 decimal places)
    String data = qc.carId + ":" + qc.laneId + ":" +
                  String.format("%.6f", qc.positionInLane) + ":" + 
                  String.format("%.6f", qc.verifiedArrival) + ":" +
                  qc.epoch + ":" + 
                  String.format("%.6f", sig.witnessTimestamp) + ":" +
                  sig.witnessReplicaId;

    System.out.println("[VERIFY] Reconstructed data: " + data);
    // Use XXHash32 for deterministic hashing across C++ and Java
    byte[] dataBytes = data.getBytes(StandardCharsets.UTF_8);
    int expectedHash = xxhash.hash(dataBytes, 0, dataBytes.length, 0);  // seed = 0

    // Convert signature bytes to int (4 bytes from C++ int32_t)
    if (sig.signatureBytes == null || sig.signatureBytes.length < 4) {
        System.err.println("[VERIFY] Invalid signature bytes: " + 
                          (sig.signatureBytes == null ? "null" : sig.signatureBytes.length + " bytes"));
        return false;
    }

    ByteBuffer buffer = ByteBuffer.wrap(sig.signatureBytes);
    buffer.order(ByteOrder.LITTLE_ENDIAN); // Match C++ byte order
    int actualHash = buffer.getInt();  // Read 4 bytes as int



    if (++verifyCallCount <= 5) {
        System.out.println("[VERIFY_JAVA] === Verifying witness " + sig.witnessReplicaId + " ===");
        System.out.println("[VERIFY_JAVA] Data string: \"" + data + "\"");
        System.out.println("[VERIFY_JAVA] XXHash32(data): " + expectedHash);
        System.out.print("[VERIFY_JAVA] Signature bytes: [");
        for (int i = 0; i < sig.signatureBytes.length; i++) {
            if (i > 0) System.out.print(", ");
            System.out.print(sig.signatureBytes[i]);  // Print as signed to match C++
        }
        System.out.println("]");
        System.out.println("[VERIFY_JAVA] Hash from signature: " + actualHash);
        System.out.println("[VERIFY_JAVA] Match: " + (expectedHash == actualHash));
        System.out.flush();
    }

    return expectedHash == actualHash;
}

private String computeOrderBatch(int maxGoCars) {

    // Build deterministic map of cars in agreed view that have a verified QC
    Map<String, ReadyQCData> viewCars = new HashMap<>();

    if (agreedView != null) {
        for (String carId : agreedView) {
            int replicaId = Integer.parseInt(carId.substring(3));
            if (isReplicaDeparted(replicaId)) continue;

            ReadyQCData qc = verifiedCars.get(carId);
            if (qc != null) {
                viewCars.put(carId, qc);
            }
        }
    }

    System.out.println("[SERVER " + processId + "] View Cars: " + viewCars);

    // 1) One front-most car per lane
    Map<String, ReadyQCData> frontCars = new HashMap<>();
    for (ReadyQCData car : viewCars.values()) {
        ReadyQCData cur = frontCars.get(car.laneId);
        // Higher positionInLane = closer to intersection (your convention)
        if (cur == null || car.positionInLane > cur.positionInLane) {
            frontCars.put(car.laneId, car);
        }
    }

    System.out.println("[SERVER " + processId + "] Front Cars: " + frontCars);

    List<ReadyQCData> candidates = new ArrayList<>(frontCars.values());
    System.out.println("[SERVER " + processId + "] Candidates: " + candidates);

    // 2) Sort front candidates deterministically using wait registry, then arrival, then carId
    candidates.sort((a, b) -> {
        int waitA = waitRegistry.getOrDefault(a.carId, 0);
        int waitB = waitRegistry.getOrDefault(b.carId, 0);

        int waitCompare = Integer.compare(waitB, waitA); // larger wait first
        if (waitCompare != 0) return waitCompare;

        int timeCompare = Double.compare(a.verifiedArrival, b.verifiedArrival); // earlier first
        if (timeCompare != 0) return timeCompare;

        return a.carId.compareTo(b.carId);
    });

    System.out.println("[SERVER " + processId + "] Sorted Candidates: " + candidates);

    // IMPORTANT FIX:
    // max winners is bounded by the number of front-lane candidates, NOT by total viewCars
    int requestedWinners = Math.min(maxGoCars, candidates.size());

    System.out.println("[SERVER " + processId + "] Requested Winners: " + requestedWinners);
    System.out.println("[SERVER " + processId + "] Candidate Count: " + candidates.size());

    // Precompute rank in sorted candidate list (avoid indexOf/object identity issues)
    Map<String, Integer> rankByCarId = new HashMap<>();
    for (int i = 0; i < candidates.size(); i++) {
        rankByCarId.put(candidates.get(i).carId, i);
    }

    // 3) Select winners safely
    Set<String> selectedToGo = new HashSet<>();
    for (int i = 0; i < requestedWinners; i++) {
        selectedToGo.add(candidates.get(i).carId);
    }

    System.out.println("[SERVER " + processId + "] Selected To Go: " + selectedToGo);

    // 4) Build decision string deterministically (sort IDs!)
    List<String> sortedCarIds = new ArrayList<>(viewCars.keySet());
    Collections.sort(sortedCarIds);

    StringBuilder sb = new StringBuilder();
    boolean first = true;

    for (String carId : sortedCarIds) {
        ReadyQCData car = viewCars.get(carId);

        if (!first) sb.append(";");

        if (selectedToGo.contains(carId)) {
            int pos = rankByCarId.getOrDefault(carId, -1);
            sb.append(carId).append(":GO:").append(pos);
            waitRegistry.remove(carId);
        } else {
            int newWait = waitRegistry.getOrDefault(carId, 0) + 1;
            waitRegistry.put(carId, newWait);
            sb.append(carId).append(":WAIT:").append(newWait);
        }

        first = false;
    }

    return sb.toString();
}

//  ORDER BAG PARSING
/** Envelope parsed from an ORDER_PROPOSE bag sent by C++. */
private static class OrderBag {
    int epoch;
    int viewHashVal;
    boolean closeFlag;
    List<ReadyQCData> qcs = new ArrayList<>();
}

/**
 * Parse the bag payload that arrives as {@code cmd.carId} after
 * {@code parseCommand()} splits on the first {@code ":"}.
 * Expected format: {@code <epoch>:<viewHash>:<closeFlag>:<qc1>||<qc2>||...}
 * Each {@code qcN} string is in the format accepted by {@link #parseReadyQC}.
 */
private OrderBag parseOrderBag(String bagData) {
    // Split on ":" with limit 4 so the QC portion (which contains colons) stays intact
    String[] parts = bagData.split(":", 4);
    if (parts.length < 4) {
        System.err.println("[ORDER-BAG] Cannot parse bag envelope (need 4 fields): " + bagData);
        return null;
    }

    OrderBag bag = new OrderBag();
    try {
        bag.epoch       = Integer.parseInt(parts[0].trim());
        bag.viewHashVal = Integer.parseInt(parts[1].trim());
        bag.closeFlag   = Boolean.parseBoolean(parts[2].trim());
    } catch (NumberFormatException e) {
        System.err.println("[ORDER-BAG] Error parsing bag header fields: " + e.getMessage());
        return null;
    }

    // QCs are delimited by "||"
    String[] qcStrings = parts[3].split("\\|\\|");
    for (String qcStr : qcStrings) {
        qcStr = qcStr.trim();
        if (!qcStr.isEmpty()) {
            ReadyQCData qc = parseReadyQC(qcStr);
            if (qc != null) {
                bag.qcs.add(qc);
            } else {
                System.err.println("[ORDER-BAG] Failed to parse QC entry in bag: " + qcStr);
            }
        }
    }

    System.out.println("[ORDER-BAG] Parsed bag: epoch=" + bag.epoch
            + " closeFlag=" + bag.closeFlag + " qcs=" + bag.qcs.size());
    return bag;
}



private ReadyQCData parseReadyQC(String qcData) {
    try {
        // Don't limit the split yet, because a double-colon will add an extra chunk
        String[] parts = qcData.split(":");
        if (parts.length < 5) return null;

        ReadyQCData qc = new ReadyQCData();
        qc.carId = parts[0];
        
        int offset = 0;
        
        // If parts[1] is empty, it means we hit a SUMO internal lane (":C_1_0")
        if (parts[1].isEmpty()) {
            offset = 1; // Shift all math indices by 1 to skip the extra split
            qc.laneId = ":" + parts[2]; 
        } else {
            qc.laneId = parts[1];
        }

        // Apply the offset to grab the correct numbers
        qc.positionInLane = Double.parseDouble(parts[2 + offset]);
        qc.verifiedArrival = Double.parseDouble(parts[3 + offset]);
        qc.epoch = Integer.parseInt(parts[4 + offset]);
        
        // Reconstruct the signature string from whatever is left
        if (parts.length > (5 + offset)) {
            // Re-join the remaining parts in case the signatures also contained colons
            StringBuilder sigs = new StringBuilder();
            for (int i = 5 + offset; i < parts.length; i++) {
                if (i > 5 + offset) sigs.append(":");
                sigs.append(parts[i]);
            }
            qc.signatures = parseSignatures(sigs.toString());
        } else {
            qc.signatures = new ArrayList<>();
        }

        return qc;
    } catch (Exception e) {
        System.err.println("[JAVA] Error parsing QC string: " + e.getMessage());
        return null;
    }
}

public void triggerBatchedLeave(int[] departingReplicas) {
    // Run in a new thread to prevent deadlocking the BFT proxy!
    new Thread(() -> {
        System.out.println("[RECONFIG] Initiating batched BFT LEAVE for: " + Arrays.toString(departingReplicas));

        try {
            // 1. Setup config (empty string defaults to BFT-SMaRt's standard "config/" folder)
            String configDir = "";

            // 2. Use a dedicated ADMIN client ID (not a replica ID)
            // BFTSmart typically reserves high IDs for admin clients (e.g., 7001)
            // Make sure this ID has corresponding keys in config/keys/
            int adminId = 7001; 

            // 3. Load the cryptographic keys to sign the request
            System.out.println("[RECONFIG] Loading keys for admin ID " + adminId);
            KeyLoader keyLoader = null;
            try {
                keyLoader = new ECDSAKeyLoader(adminId, configDir, false, "ECDSA");
                System.out.println("[RECONFIG] KeyLoader created successfully");
            } catch (Exception e) {
                System.err.println("[RECONFIG ERROR] Failed to load keys for admin " + adminId + ": " + e.getMessage());
                System.err.println("[RECONFIG ERROR] Make sure config/keys/publickey" + adminId + " and privatekey" + adminId + " exist!");
                e.printStackTrace();
                return;
            }

            // 4. Initialize the reconfiguration client
            Reconfiguration rec = new Reconfiguration(adminId, configDir, keyLoader);
            System.out.println("[RECONFIG] Reconfiguration client created");

            // 5. Connect the underlying ServiceProxy (REQUIRED by your version)
            rec.connect();
            System.out.println("[RECONFIG] Connected to BFT network via ServiceProxy");

            // 6. Queue up all removals in one shot
            for (int id : departingReplicas) {
                rec.removeServer(id);
                System.out.println("[RECONFIG] Queued removal of replica " + id);
            }
            System.out.println("[RECONFIG] All removals queued: " + Arrays.toString(departingReplicas));

            // 7. Execute once! (This triggers a single consensus round)
            System.out.println("[RECONFIG] Executing reconfiguration request (waiting for consensus)...");
            long startTime = System.currentTimeMillis();
            ReconfigureReply reply = rec.execute();
            long duration = System.currentTimeMillis() - startTime;

            System.out.println("[RECONFIG] Reconfiguration consensus completed in " + duration + "ms");
            System.out.println("[RECONFIG] Reply: " + (reply != null ? reply.toString() : "null"));

            // 8. Clean up the connections
            rec.close();
            System.out.println("[RECONFIG] Closed reconfiguration client");

            System.out.println("[RECONFIG] ===== BATCH REMOVAL COMPLETE =====");
            System.out.println("[RECONFIG] Removed replicas: " + Arrays.toString(departingReplicas));
            System.out.println("[RECONFIG] Notifying C++ that reconfiguration is complete...");

            notifyReconfigComplete(processId);
            
        } catch (Exception e) {
            System.err.println("[RECONFIG ERROR] Failed to execute BFT LEAVE: " + e.getMessage());
            e.printStackTrace();
        }
        
    }).start();
}

private void resetForNextRound() {
    // Keep verifiedCars but remove cars that have left
    // (C++ will notify which cars have departed)

    // Reset consensus phases
    viewPhaseComplete = false;
    orderPhaseComplete = false;
    agreedView = null;
    agreedOrder = null;
    finalDecision = null;

    roundNumber++;

    this.verifiedCars.clear();


    System.out.println("[RESET] ===== STARTING ROUND " + roundNumber + " =====");
    System.out.println("[RESET] Remaining cars in pool: " + verifiedCars.size());
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
            if (reqStr.isEmpty()) throw new IllegalArgumentException("Empty command at index " + i);
            decoded[i] = parseCommand(reqStr);
        }

        // =========================================================================
        // PHASE 1: LOGIC & DECISION (Scan the whole batch to update state)
        // =========================================================================
        
        Set<String> validCarsUnion = new HashSet<>();
        boolean foundNewValidEvidence = false;

        // Scan for VIEW_PROPOSE to build the Union
        for (Cmd cmd : decoded) {
            if (cmd.type == Cmd.Type.VIEW_PROPOSE && cmd.carId != null && !cmd.carId.equals("NONE")) {
                if (isReplicaDeparted(processId)) continue; 

                try {
                    // Manual Parsing (Logic extracted from your switch case)
                    String[] parts = cmd.carId.split(":", 3);
                    if (parts.length >= 3) {
                        ViewProposal proposal = new ViewProposal();
                        proposal.proposerReplicaId = Integer.parseInt(parts[0]);
                        String[] carArray = parts[1].split(",");
                        proposal.observedCars = new HashSet<>(Arrays.asList(carArray));
                        proposal.v2vSignatures = parseViewSignatures(parts[2]);

                        // CRITICAL: The "F+1" Check
                        // We check validity immediately. We do NOT wait for other replicas.
                        if (validateViewProposal(proposal)) {
                            for (String car : proposal.observedCars) {
                                int rid = Integer.parseInt(car.substring(3));
                                if (!isReplicaDeparted(rid)) {
                                    validCarsUnion.add(car);
                                }
                            }
                            foundNewValidEvidence = true;
                            
                            // Debug log
                            // System.out.println("[VIEW] Found valid evidence from Replica " + proposal.proposerReplicaId);
                        }
                    }
                } catch (Exception e) {
                    System.err.println("[VIEW] Error parsing proposal in scan phase: " + e.getMessage());
                }
            }
        }

        // DECIDE IMMEDIATELY if we have evidence and haven't decided yet
        if (!viewPhaseComplete && foundNewValidEvidence) {
            // Deterministic Sort
            List<String> sortedView = new ArrayList<>(validCarsUnion);
            Collections.sort(sortedView);
            
            // LOCK IN THE DECISION
            this.agreedView = new HashSet<>(sortedView);
            this.viewPhaseComplete = true;

            String resultString = String.join(",", sortedView);
            System.out.println("[SERVER] CONSENSUS REACHED (One-Round). Final View: " + resultString);

            System.out.println("[SERVER] REACHED AT WALL TIME = " +  (System.currentTimeMillis() - experimentStartWall) + "ms"  );
            
            // // Notify C++
            // try {
            //     notifyViewAgreed(processId, resultString);
            // } catch (UnsatisfiedLinkError e) {
            //     System.err.println("JNI Error: " + e.getMessage());
            // }
        }

        // =========================================================================
        // PHASE 2: REPLY GENERATION (Respond to clients based on Phase 1 state)
        // =========================================================================

        for (int i = 0; i < commands.length; i++) {
            Cmd cmd = decoded[i];
            String reply = "ERROR:Unknown command";

            switch (cmd.type) {
                case VIEW_PROPOSE:
                    if (isReplicaDeparted(processId)) {
                        reply = "DEPARTED";
                    } else if (viewPhaseComplete) {
                        // SUCCESS: Everyone gets the agreed view (even if they were late)
                        reply = "VIEW_AGREED:" + String.join(",", agreedView);
                    } else {
                        // FAILURE: Batch contained only invalid junk (rare)
                        reply = "VIEW_REJECTED:No_Valid_Evidence";
                    }
                    break;

                case ORDER_PROPOSE:
                    // Bag-based ORDER phase: each car proposes a bag of ReadyQCs gathered
                    // during a V2V gossip window. Java de-dupes by carId and decides as
                    // soon as at least one front-lane candidate has a verified QC.
                    if (isReplicaDeparted(processId)) {
                        reply = "DEPARTED";
                    } else if (viewPhaseComplete && !orderPhaseComplete) {
                        String bagData = cmd.carId;
                        if (bagData != null && !bagData.isEmpty()) {
                            OrderBag bag = parseOrderBag(bagData);
                            if (bag != null) {
                                // Insert each validated QC (dedup by carId)
                                for (ReadyQCData qc : bag.qcs) {
                                    if (qc != null && !verifiedCars.containsKey(qc.carId)) {
                                        if (validateReadyQC(qc)) {
                                            verifiedCars.put(qc.carId, qc);
                                            System.out.println("[ORDER] Server " + processId
                                                    + " accepted QC for " + qc.carId
                                                    + " (pool size=" + verifiedCars.size() + ")");
                                        } else {
                                            System.out.println("[ORDER] Server " + processId
                                                    + " rejected invalid QC for "
                                                    + (qc.carId != null ? qc.carId : "null"));
                                        }
                                    }
                                }
                            }
                        }

                        if (agreedView != null) {
                            // Attempt to produce a decision from current verifiedCars.
                            // Decide as soon as at least one front-lane candidate is available.
                            String tentativeDecision = computeOrderBatch(4);
                            boolean hasGocar = tentativeDecision != null
                                    && tentativeDecision.contains(":GO:");
                            if (hasGocar) {
                                this.finalDecision = tentativeDecision;
                                this.orderPhaseComplete = true;
                                triggerRoundReset = true;
                                List<Integer> departingIdsList = new ArrayList<>();
                                for (String entry : this.finalDecision.split(";")) {
                                    if (entry.contains(":GO:")) {
                                        // Extract the integer ID (e.g., "veh3" -> 3)
                                        String carIdStr = entry.split(":")[0];
                                        departingIdsList.add(Integer.parseInt(carIdStr.substring(3)));
                                    }
                                }

                                System.out.println("[SERVER " + processId + "] Departing IDs List: " + departingIdsList);

                                // CRITICAL FIX: Only ONE replica (the leader with lowest non-departing ID) triggers reconfig
                                // This prevents 8 concurrent reconfig requests from conflicting
                                try {
                                    notifyOrderDecided(processId, finalDecision);
                                    System.out.println("[ORDER] NOTIFIED C++ ORDER DECIDED: " + finalDecision);
                                } catch (UnsatisfiedLinkError e) {
                                    System.err.println("JNI Error: " + e.getMessage());
                                }
                                
                                if (!departingIdsList.isEmpty()) {
                                    try {
                                        bftsmart.reconfiguration.ServerViewController svc = this.replica.getReplicaContext().getSVController();
                                        bftsmart.reconfiguration.views.View currentView = svc.getCurrentView();
                                        TOMLayer tom = svc.getTOMLayer();
                                
                                        List<Integer> newProcessList = new ArrayList<>();
                                        for (int id : currentView.getProcesses()) {
                                            if (!departingIdsList.contains(id)) {
                                                newProcessList.add(id);
                                            }
                                        }
                                        
                                        int[] newProcesses = newProcessList.stream().mapToInt(Integer::intValue).toArray();
                                        int newF = (newProcesses.length - 1) / 3;
                                        int newViewId = currentView.getId() + 1;
                                
                                        int maxId = -1;
                                        for (int p : newProcesses) if (p > maxId) maxId = p;
                                        
                                        java.net.InetSocketAddress[] newAddresses = new java.net.InetSocketAddress[maxId + 1];
                                        for (int p : newProcesses) {
                                            newAddresses[p] = currentView.getAddress(p);
                                        }

                                        bftsmart.reconfiguration.views.View newView = 
                                        new bftsmart.reconfiguration.views.View(newViewId, newProcesses, newF, newAddresses);

                                        // Apply view
                                        svc.reconfigureTo(newView);

                                        // Drop stale sockets
                                        try {
                                            bftsmart.communication.ServerCommunicationSystem cs = tom.getCommunication();
                                            cs.updateServersConnections();
                                            System.out.println("[SERVER " + processId + "] Server connections refreshed for new view " + newViewId);
                                            Thread.sleep(150);
                                        } catch (Exception csEx) {
                                            System.err.println("[SERVER " + processId + "] Warning: could not refresh connections");
                                        }

                                        // Set Leader Safely!
                                        if (newProcesses.length > 0) {
                                            int forcedLeader = Arrays.stream(newProcesses).min().getAsInt();
                                            tom.execManager.setNewLeader(forcedLeader);

                                            if (processId == forcedLeader) {
                                                tom.imAmTheLeader();
                                                System.out.println("[SERVER " + processId + "] I am the new forced leader!");
                                            } else {
                                                System.out.println("[SERVER " + processId + "] Acknowledging forced leader is " + forcedLeader);
                                            }
                                        }

                                        // Clean up app state
                                        for (int depId : departingIdsList) {
                                            markReplicaDeparted(depId);
                                            verifiedCars.remove("veh" + depId);
                                        }
                                        updateBatchSize(newProcesses.length);

                                    } catch (Exception e) {
                                        System.err.println("[SERVER " + processId + "] ERROR applying in-memory view: " + e.getMessage());
                                    }

                                    // =========================================================================
                                    // 3. START NEXT ROUND
                                    // Now that the old connections are dead, tell the remaining cars they 
                                    // can safely reset their C++ state and start Round 2.
                                    // =========================================================================
                                    if (!departingIdsList.contains(processId)) {
                                        System.out.println("[SERVER " + processId + "] Notifying C++ that reconfiguration is complete...");
                                        notifyReconfigComplete(processId);
                                    } else {
                                        System.out.println("[SERVER " + processId + "] I am departing - NOT notifying C++ (will exit soon)");
                                        if (this.localClientProxy != null) {
                                            this.localClientProxy.close();
                                            this.localClientProxy = null;
                                        }
                                    }
                                

                                reply = finalDecision;


                                }
                                    
                                
                            
                                
                                // reply = finalDecision;
                            } else {
                                int received = verifiedCars.size();
                                int needed = (agreedView != null) ? agreedView.size() : 0;
                                reply = "ORDER_BUFFERING:Have " + received + "/" + needed;
                            }
                        
                        
                        } else if (orderPhaseComplete) {
                            reply = finalDecision;
                        } else {
                            reply = "ERROR:View not complete yet";
                        }
                        break;
                    }

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
        // System.out.println("Batch processed in " + String.format("%.3f", appTimeMs) + " ms");
        if (triggerRoundReset) {
            resetForNextRound();
        }
    } catch (Exception ex) {
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


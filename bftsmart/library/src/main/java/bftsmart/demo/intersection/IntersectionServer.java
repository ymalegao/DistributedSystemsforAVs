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
import java.util.HashMap;
import java.util.Map;

/**
 * BFT replicated service for managing an intersection.
 * Cars send requests in the format: "CAR_ID:direction:arrival_time"
 * The server maintains a queue of waiting cars and allows the car with
 * the earliest arrival time to proceed through the intersection.
 * 
 * @author Intersection Demo
 */
public final class IntersectionServer extends DefaultRecoverable {
    
    // BATCH-OF-4 CONSENSUS MODE
    // Buffer for incoming JOIN requests - we wait until we have 4 unique cars
    private Map<String, Double> joinBuffer = new HashMap<>(); // carId -> arrivalTime
    private String finalDecision = null; // The single decision for all 4 cars
    private boolean batchProcessed = false;
    
    // Old fields (kept for compatibility)
    private Map<String, Integer> waitMap = new HashMap<>(); // carId -> wait
    private String lastLeaver = null;
    private long roundNumber = 0;
    private int iterations = 0;
    private int processId;
    private ServiceReplica replica;
    private int numCars;
    
    private static final int BATCH_SIZE = 4; // Wait for 4 cars
    private static final int CROSSING_TIME_SECONDS = 5; // Time between cars

    
    public IntersectionServer(int id, int numCars) {
        this.waitMap = new HashMap<>();
        this.replica = new ServiceReplica(id, this, this);
        this.processId = id;
        this.numCars = numCars;
        if (numCars > 0) {
            new Thread(() -> {
                try{
                    // Replica 4 (CAR_E) doesn't auto-send - waits for manual JOIN command
                    if (processId == 4) {
                        System.out.println("[CAR_E/Replica 4] Waiting for manual JOIN command...");
                        return;
                    }
                    
                    Thread.sleep(6000);
                    Thread.sleep((long)(Math.random() * 50));

                    sendCarRequest();
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                }
            }).start();
        }
    }

    private void sendCarRequest() {
    int clientId = 1000 + processId;
    String carId = "CAR_" + processId;

    try (ServiceProxy proxy = new ServiceProxy(clientId)) {
        String joinReq = "JOIN:" + carId;
        byte[] joinReply = proxy.invokeOrdered(joinReq.getBytes(StandardCharsets.UTF_8));
        System.out.println("[SERVER " + processId + "] JOIN reply: " + new String(joinReply, StandardCharsets.UTF_8));

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
        enum Type { JOIN, LEAVE, GET_STATE }
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
  
    @Override
public byte[][] appExecuteBatch(byte[][] commands, MessageContext[] msgCtxs, boolean fromConsensus) {
    iterations++;
    long appStartTime = System.nanoTime();
    byte[][] replies = new byte[commands.length][];
    roundNumber++;

    try {
        // Decode all commands
        Cmd[] decoded = new Cmd[commands.length];
        for (int i = 0; i < commands.length; i++) {
            String reqStr = new String(commands[i], StandardCharsets.UTF_8).trim();
            if (reqStr.isEmpty()) {
                throw new IllegalArgumentException("Empty command at index " + i);
            }
            decoded[i] = parseCommand(reqStr);
        }

        // === BATCH-OF-4 CONSENSUS MODE ===
        
        // Step 1: Collect JOIN requests (with timestamps from V2V)
        for (Cmd cmd : decoded) {
            if (cmd.type == Cmd.Type.JOIN && cmd.carId != null && !batchProcessed) {
                // Extract timestamp if present (format: "JOIN:CAR_1:10.5")
                double arrivalTime = System.currentTimeMillis() / 1000.0; // Default to now
                
                if (!joinBuffer.containsKey(cmd.carId)) {
                    joinBuffer.put(cmd.carId, arrivalTime);
                    System.out.println("[BATCH] Buffered JOIN from " + cmd.carId + 
                                     " (buffer size: " + joinBuffer.size() + "/" + BATCH_SIZE + ")");
                }
            }
        }

        // Step 2: Check if we have 4 unique cars - if so, RUN CONSENSUS
        if (!batchProcessed && joinBuffer.size() >= BATCH_SIZE) {
            System.out.println("\n[BATCH] ===== TRIGGERING CONSENSUS: We have " + BATCH_SIZE + " cars! =====");
            
            // Sort cars by arrival time (or alphabetically if tied)
            java.util.List<java.util.Map.Entry<String, Double>> sortedCars = 
                new java.util.ArrayList<>(joinBuffer.entrySet());
            sortedCars.sort((a, b) -> {
                int timeCompare = Double.compare(a.getValue(), b.getValue());
                if (timeCompare != 0) return timeCompare;
                return a.getKey().compareTo(b.getKey()); // Alphabetical tiebreaker
            });
            
            // Build the SINGLE decision for all 4 cars
            // Format: "CAR_0:GO:0;CAR_1:GO:5;CAR_2:GO:10;CAR_3:GO:15"
            StringBuilder decisionBuilder = new StringBuilder();
            for (int pos = 0; pos < sortedCars.size() && pos < BATCH_SIZE; pos++) {
                String carId = sortedCars.get(pos).getKey();
                int delaySeconds = pos * CROSSING_TIME_SECONDS;
                
                if (pos > 0) decisionBuilder.append(";");
                decisionBuilder.append(carId).append(":GO:").append(delaySeconds);
            }
            
            finalDecision = decisionBuilder.toString();
            batchProcessed = true;
            
            System.out.println("[BATCH] ===== CONSENSUS COMPLETE =====");
            System.out.println("[BATCH] Final Decision: " + finalDecision);
            System.out.println("[BATCH] All subsequent requests will receive this decision\n");
        }

        // Step 3: Build replies for this batch
        for (int i = 0; i < commands.length; i++) {
            Cmd cmd = decoded[i];
            String carId = cmd.carId;
            String reply;

            switch (cmd.type) {
                case JOIN:
                    if (batchProcessed) {
                        // Consensus already done - tell them to wait for LEAVE
                        reply = carId + ":JOINED_BATCH_READY";
                    } else {
                        // Still buffering
                        reply = carId + ":JOINED_BUFFERING=" + joinBuffer.size() + "/" + BATCH_SIZE;
                    }
                    break;

                case LEAVE:
                    if (batchProcessed && finalDecision != null) {
                        // Return the SINGLE decision to everyone
                        reply = finalDecision;
                    } else {
                        // Consensus not ready yet
                        reply = carId + ":WAIT_FOR_CONSENSUS";
                    }
                    break;

                case GET_STATE:
                    reply = "STATE:buffer=" + joinBuffer.size() + 
                           ";batchProcessed=" + batchProcessed + 
                           ";decision=" + (finalDecision != null ? finalDecision : "NONE");
                    break;

                default:
                    reply = "ERROR:Unknown command";
            }

            replies[i] = reply.getBytes(StandardCharsets.UTF_8);
        }

        double appTimeMs = (System.nanoTime() - appStartTime) / 1_000_000.0;
        System.out.println("(" + iterations + ") Round " + roundNumber + " processed "
                + commands.length + " cmd(s). App time: " + String.format("%.3f", appTimeMs) + " ms");
        System.out.println("   joinBuffer = " + joinBuffer);
        System.out.println("   batchProcessed = " + batchProcessed);
        if (finalDecision != null) {
            System.out.println("   finalDecision = " + finalDecision);
        }

        return replies;

    } catch (Exception ex) {
        System.err.println("(" + iterations + ") Error processing batch: " + ex.getMessage());
        ex.printStackTrace();
        for (int i = 0; i < replies.length; i++) {
            if (replies[i] == null) {
                replies[i] = "ERROR: Processing failed".getBytes(StandardCharsets.UTF_8);
            }
        }
        return replies;
    }
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


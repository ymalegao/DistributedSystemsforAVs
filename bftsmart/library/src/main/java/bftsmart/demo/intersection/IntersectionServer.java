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
    
    // Map of car_id -> arrival_time for cars currently waiting
    private Map<String, Double> waitingCars;
    private int iterations = 0;
    private ServiceReplica replica;
    private int numCars;
    private int processId;
    
    public IntersectionServer(int id, int numCars) {
        this.waitingCars = new HashMap<>();
        this.numCars = numCars;
        this.processId = id;
        this.replica = new ServiceReplica(id, this, this);

        if (numCars > 0) {
            new Thread(() -> {
                try{
                    Thread.sleep(6000);
                    Thread.sleep((long)(Math.random() * 50));

                    sendCarRequest();
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                }
            }).start();
        }
    }

    private void sendCarRequest(){
        try{
            int clientId = 1000 + processId;
            String carId = "CAR_" + processId;
            String directions[] = {"left", "straight", "right"};
            String direction = directions[processId % directions.length];
            double arrivalTime;
            if (processId == 0) {
                arrivalTime = 0.001;
                System.err.println("[SERVER " + processId + "] Car " + carId + " is lying about arrival time: claiming " + arrivalTime);

            }else{
                arrivalTime = System.currentTimeMillis() / 1000.0;
            }
            String request = carId + ":" + direction + ":" + String.format("%.1f", arrivalTime);
            System.out.println("═══════════════════════════════════════════════════════════");
            System.out.println("[SERVER " + processId + "] Sending car request as client " + clientId);
            System.out.println("  Request: " + request);
            System.out.println("  Waiting for " + numCars + " total cars before consensus starts...");
            System.out.println("═══════════════════════════════════════════════════════════");
            System.out.println();

            try (ServiceProxy proxy = new ServiceProxy(clientId)) {
                byte[] requestBytes = request.getBytes("UTF-8");
                long startTime = System.nanoTime();
                byte[] reply = proxy.invokeOrdered(requestBytes);
                long endTime = System.nanoTime();
                long consensusTimeNs = (endTime - startTime);

                if (reply != null) {
                    String replyStr = new String(reply, "UTF-8").trim();
                    System.out.println("[SERVER " + processId + "] Response received in " + String.format("%.3f", consensusTimeNs / 1_000_000.0) + " ms");
                    System.out.println("[SERVER " + processId + "] Response: " + replyStr);
                    System.out.println("═══════════════════════════════════════════════════════════");
                } else {
                    System.out.println("[SERVER " + processId + "] Consensus timed out");
                }
            } catch (IOException e) {
                System.err.println("[SERVER " + processId + "] Error sending car request: " + e.getMessage());
                e.printStackTrace();
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
    
    /**
     * Find the car with the smallest arrival time.
     * In case of ties, break by car_id (lexicographically smallest).
     * @return The car_id with the smallest arrival time, or null if no cars are waiting
     */
    private String findEarliestCar() {
        if (waitingCars.isEmpty()) {
            return null;
        }
        
        String earliestCarId = null;
        double earliestTime = Double.MAX_VALUE;
        
        for (Map.Entry<String, Double> entry : waitingCars.entrySet()) {
            double time = entry.getValue();
            String carId = entry.getKey();
            
            if (time < earliestTime || 
                (time == earliestTime && (earliestCarId == null || carId.compareTo(earliestCarId) < 0))) {
                earliestTime = time;
                earliestCarId = carId;
            }
        }
        
        return earliestCarId;
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
        
        // Measure application execution time
        long appStartTime = System.nanoTime();
        
        byte[][] replies = new byte[commands.length][];
        
        try {
            // Collect all cars from all requests in this batch
            java.util.List<String[]> allCars = new java.util.ArrayList<>();
            
            for (int i = 0; i < commands.length; i++) {
                String requestStr = new String(commands[i], "UTF-8");
                String[][] cars = parseMultiCarRequest(requestStr);
                
                if (cars != null && cars.length > 0) {
                    for (String[] car : cars) {
                        allCars.add(car);
                    }
                }
            }
            
            System.out.println("(" + iterations + ") Processing batch of " + commands.length + " request(s) with " + allCars.size() + " total car(s) in single consensus round");
            System.out.println("(" + iterations + ") Total cars in batch: " + allCars.size() + " (expected: " + numCars + ")");
            
            // First, add all cars from all requests to waitingCars (for tracking)
            for (String[] carData : allCars) {
                String carId = carData[0];
                String direction = carData[1];
                double arrivalTime = Double.parseDouble(carData[2]);
                
                waitingCars.put(carId, arrivalTime);
                System.out.println("  - Car " + carId + " arrived at time " + arrivalTime + 
                                 " (direction: " + direction + ")");
            }
            System.out.println("  Total waiting cars: " + waitingCars.size());
            
            // Find the earliest car AMONG ALL CARS IN THIS BATCH
            String earliestCarId = null;
            double earliestTime = Double.MAX_VALUE;
            
            for (String[] carData : allCars) {
                String carId = carData[0];
                double arrivalTime = Double.parseDouble(carData[2]);
                
                if (arrivalTime < earliestTime || 
                    (arrivalTime == earliestTime && (earliestCarId == null || carId.compareTo(earliestCarId) < 0))) {
                    earliestTime = arrivalTime;
                    earliestCarId = carId;
                }
            }
            
            if (earliestCarId == null && allCars.size() > 0) {
                System.err.println("(" + iterations + ") No earliest car found (should not happen)");
                // Return error for all requests
                for (int i = 0; i < replies.length; i++) {
                    replies[i] = "ERROR: No earliest car found".getBytes("UTF-8");
                }
                return replies;
            }
            
            if (earliestCarId != null) {
                System.out.println("  → Earliest car in this batch: " + earliestCarId + " (time: " + earliestTime + ")");
            }

            // //ADD byzantine behavior here
            // if (processId == 0) {
            //     return generateByzantineResponse(commands, allCars, earliestCarId);
            // }

            
            // Process each request and build responses
            for (int i = 0; i < commands.length; i++) {
                String requestStr = new String(commands[i], "UTF-8");
                String[][] cars = parseMultiCarRequest(requestStr);
                
                if (cars == null || cars.length == 0) {
                    replies[i] = "ERROR: Invalid request format".getBytes("UTF-8");
                    continue;
                }
                
                // Build response for all cars in this request
                // Format: "CAR_ID1:GO;CAR_ID2:WAIT;CAR_ID3:WAIT;..."
                StringBuilder response = new StringBuilder();
                boolean first = true;
                
                for (String[] carData : cars) {
                    String carId = carData[0];
                    String decision;
                    
                    if (carId.equals(earliestCarId)) {
                        // This car gets GO - remove it from waiting queue
                        decision = "GO";
                        waitingCars.remove(carId);
                        System.out.println("  ✓ Car " + carId + " can GO (earliest in this batch). Removed from queue.");
                    } else {
                        // All other cars must WAIT (they stay in waitingCars for future processing)
                        decision = "WAIT";
                        System.out.println("  ⏸ Car " + carId + " must WAIT");
                    }
                    
                    if (!first) {
                        response.append(";");
                    }
                    response.append(carId).append(":").append(decision);
                    first = false;
                }
                
                replies[i] = response.toString().getBytes("UTF-8");
            }
            
            // Calculate application execution time
            long appEndTime = System.nanoTime();
            long appTimeNs = appEndTime - appStartTime;
            double appTimeMs = appTimeNs / 1_000_000.0;
            double appTimeUs = appTimeNs / 1_000.0;
            
            System.out.println("(" + iterations + ") Batch processing complete. " + commands.length + " request(s) processed.");
            System.out.println("(" + iterations + ") Application execution time: " + 
                             String.format("%.3f", appTimeMs) + " ms (" + 
                             String.format("%.2f", appTimeUs) + " μs)");
            
            return replies;
            
        } catch (Exception ex) {
            // Calculate execution time even on error
            long appEndTime = System.nanoTime();
            long appTimeNs = appEndTime - appStartTime;
            double appTimeMs = appTimeNs / 1_000_000.0;
            
            System.err.println("(" + iterations + ") Error processing batch: " + ex.getMessage());
            System.err.println("(" + iterations + ") Application execution time (before error): " + 
                             String.format("%.3f", appTimeMs) + " ms");
            ex.printStackTrace();
            
            // Return error for all requests
            for (int i = 0; i < replies.length; i++) {
                replies[i] = "ERROR: Processing failed".getBytes(StandardCharsets.UTF_8);
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
        try {
            ByteArrayInputStream bis = new ByteArrayInputStream(state);
            ObjectInput in = new ObjectInputStream(bis);
            waitingCars = (Map<String, Double>) in.readObject();
            in.close();
            bis.close();
            System.out.println("[STATE] Snapshot installed. Waiting cars: " + waitingCars.size());
        } catch (IOException | ClassNotFoundException e) {
            System.err.println("[ERROR] Error deserializing state: " + e.getMessage());
            waitingCars = new HashMap<>(); // Reset to empty map on error
        }
    }

    @Override
    public byte[] getSnapshot() {
        try {
            ByteArrayOutputStream bos = new ByteArrayOutputStream();
            ObjectOutput out = new ObjectOutputStream(bos);
            out.writeObject(waitingCars);
            out.flush();
            bos.flush();
            out.close();
            bos.close();
            System.out.println("[STATE] Snapshot taken. Waiting cars: " + waitingCars.size());
            return bos.toByteArray();
        } catch (IOException ioe) {
            System.err.println("[ERROR] Error serializing state: " + ioe.getMessage());
            return "ERROR".getBytes();
        }
    }
}


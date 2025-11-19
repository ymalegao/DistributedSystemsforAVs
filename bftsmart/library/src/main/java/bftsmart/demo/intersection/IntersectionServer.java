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
    private Map<String, Integer> waitMap = new HashMap<>(); // carId -> wait
    private String lastLeaver = null;
    private long roundNumber = 0;
    private int iterations = 0;
    private int processId;
    private ServiceReplica replica;
    private int numCars;

    
    public IntersectionServer(int id, int numCars) {
        this.waitMap = new HashMap<>();
        this.replica = new ServiceReplica(id, this, this);
        this.processId = id;
        this.numCars = numCars;
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
        System.out.println("[SERVER " + processId + "] LEAVE reply: " + new String(leaveReply, StandardCharsets.UTF_8));
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

    try {
        // 1. Tick
        for (Map.Entry<String, Integer> entry : waitMap.entrySet()) {
            entry.setValue(entry.getValue() + 1);
        }
        roundNumber++;

        // 2. Decode commands
        Cmd[] decoded = new Cmd[commands.length];
        for (int i = 0; i < commands.length; i++) {
            String reqStr = new String(commands[i], StandardCharsets.UTF_8).trim();
            if (reqStr.isEmpty()) {
                throw new IllegalArgumentException("Empty command at index " + i);
            }
            decoded[i] = parseCommand(reqStr);
        }

        // 3. Apply JOINs
        for (Cmd cmd : decoded) {
            if (cmd.type == Cmd.Type.JOIN && cmd.carId != null) {
                waitMap.putIfAbsent(cmd.carId, 1);
            }
        }

        // 4. Compute winner
        String winner = null;
        int bestWait = -1;
        for (Map.Entry<String, Integer> entry : waitMap.entrySet()) {
            String car = entry.getKey();
            int w = entry.getValue();
            if (w > bestWait || (w == bestWait && (winner == null || car.compareTo(winner) < 0))) {
                bestWait = w;
                winner = car;
            }
        }

        // 5. Process commands + build replies
        boolean winnerLeft = false;
        for (int i = 0; i < commands.length; i++) {
            Cmd cmd = decoded[i];
            String carId = cmd.carId;
            String reply;

            switch (cmd.type) {
                case LEAVE:
                    if (carId == null || !waitMap.containsKey(carId)) {
                        reply = (carId == null ? "UNKNOWN" : carId) + ":NOT_IN_QUEUE";
                    } else if (!winnerLeft && carId.equals(winner)) {
                        waitMap.remove(carId);
                        lastLeaver = carId;
                        winnerLeft = true;
                        reply = carId + ":GO";
                    } else {
                        reply = carId + ":WAIT";
                    }
                    break;

                case JOIN:
                    int wait = waitMap.getOrDefault(carId, -1);
                    reply = carId + ":JOINED_WAIT=" + wait;
                    break;

                case GET_STATE:
                    //Should this be used for the new node to get the state?
                    reply = "STATE:" + waitMap + ";WINNER=" + winner + ";ROUND=" + roundNumber;
                    break;

                default:
                    reply = "ERROR:Unknown command";
            }

            replies[i] = reply.getBytes(StandardCharsets.UTF_8);
        }

        double appTimeMs = (System.nanoTime() - appStartTime) / 1_000_000.0;
        System.out.println("(" + iterations + ") Round " + roundNumber + " processed "
                + commands.length + " cmd(s). App time: " + String.format("%.3f", appTimeMs) + " ms");
        System.out.println("   waitMap = " + waitMap);
        System.out.println("   winner  = " + winner + ", lastLeaver = " + lastLeaver);

        return replies;

    } catch (Exception ex) {
        System.err.println("(" + iterations + ") Error processing batch: " + ex.getMessage());
        ex.printStackTrace();
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
        try (ObjectInput in = new ObjectInputStream(new ByteArrayInputStream(state))) {
            waitMap = (Map<String, Integer>) in.readObject();
            lastLeaver = (String) in.readObject();
            roundNumber = in.readLong();
            System.out.println("[STATE] Snapshot installed. Waiting cars: " + waitMap.size());
        } catch (IOException | ClassNotFoundException e) {
            System.err.println("[ERROR] Error deserializing state: " + e.getMessage());
            waitMap = new HashMap<>();
            lastLeaver = null;
            roundNumber = 0;
        }
    }

    @Override
    public byte[] getSnapshot() {
        try (ByteArrayOutputStream bos = new ByteArrayOutputStream();
            ObjectOutput out = new ObjectOutputStream(bos)) {
            out.writeObject(waitMap);
            out.writeObject(lastLeaver);
            out.writeLong(roundNumber);
            out.flush();
            System.out.println("[STATE] Snapshot taken. Waiting cars: " + waitMap.size());
            return bos.toByteArray();
        } catch (IOException ioe) {
            System.err.println("[ERROR] Error serializing state: " + ioe.getMessage());
            return "ERROR".getBytes(StandardCharsets.UTF_8);
        }
    }
}


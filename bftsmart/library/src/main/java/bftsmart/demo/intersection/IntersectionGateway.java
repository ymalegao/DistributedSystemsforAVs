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

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.PrintWriter;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import bftsmart.tom.ServiceProxy;

/**
 * TCP Gateway that bridges OMNeT++ and BFT-SMaRt.
 * 
 * Accepts simple line-based TCP protocol from OMNeT++:
 *   REQUEST_CROSS <carId> <direction> <arrivalTime>
 * 
 * Returns:
 *   DECISION <qcId> <orderPosition> <fullOrderAsCommaList>
 * 
 * Usage:
 *   java ... IntersectionGateway <clientId> <listenPort>
 * 
 * Example:
 *   java ... IntersectionGateway 1 9001
 * 
 * @author Intersection Gateway
 */
public class IntersectionGateway {
    
    // Time (in seconds) for one car to clear the intersection
    private static final int CROSSING_TIME_SECONDS = 5;
    
    private final int clientId;
    private final int listenPort;
    private ServiceProxy serviceProxy;
    private ServerSocket serverSocket;
    
    public IntersectionGateway(int clientId, int listenPort) {
        this.clientId = clientId;
        this.listenPort = listenPort;
    }
    
    /**
     * Start the gateway server
     */
    public void start() throws IOException {
        // Create ServiceProxy for BFT-SMaRt communication
        System.out.println("[Gateway " + clientId + "] Creating ServiceProxy with clientId=" + clientId);
        serviceProxy = new ServiceProxy(clientId);
        
        // Open TCP server socket
        System.out.println("[Gateway " + clientId + "] Opening TCP server socket on port " + listenPort);
        serverSocket = new ServerSocket(listenPort);
        System.out.println("[Gateway " + clientId + "] Gateway ready. Listening on port " + listenPort);
        
        // Accept connections
        while (true) {
            try {
                Socket clientSocket = serverSocket.accept();
                System.out.println("[Gateway " + clientId + "] Accepted connection from " + 
                                 clientSocket.getRemoteSocketAddress());
                
                // Handle each client connection in a separate thread
                new Thread(() -> handleClient(clientSocket)).start();
            } catch (IOException e) {
                if (serverSocket.isClosed()) {
                    System.out.println("[Gateway " + clientId + "] Server socket closed, shutting down");
                    break;
                }
                System.err.println("[Gateway " + clientId + "] Error accepting connection: " + e.getMessage());
                e.printStackTrace();
            }
        }
    }
    
    /**
     * Handle a single client connection
     */
    private void handleClient(Socket clientSocket) {
        try (BufferedReader in = new BufferedReader(
                new InputStreamReader(clientSocket.getInputStream(), StandardCharsets.UTF_8));
             PrintWriter out = new PrintWriter(
                clientSocket.getOutputStream(), true)) {
            
            String line;
            boolean isHttpRequest = false;
            int lineCount = 0;
            
            while ((line = in.readLine()) != null) {
                line = line.trim();
                lineCount++;
                
                // Detect HTTP requests on first line (GET, POST, PUT, etc.)
                if (lineCount == 1 && (line.startsWith("GET ") || line.startsWith("POST ") || 
                    line.startsWith("PUT ") || line.startsWith("DELETE ") || line.startsWith("HEAD ") ||
                    line.startsWith("OPTIONS ") || line.startsWith("PATCH ") || line.contains("HTTP/"))) {
                    isHttpRequest = true;
                    System.out.println("[Gateway " + clientId + "] Rejected HTTP request (line 1): " + line);
                    out.println("ERROR: This gateway uses a simple line-based protocol, not HTTP.");
                    out.println("Expected format: REQUEST_CROSS <carId> <direction> <arrivalTime>");
                    // Read and discard remaining HTTP headers to avoid log spam
                    while ((line = in.readLine()) != null && !line.trim().isEmpty()) {
                        // Discard HTTP headers
                    }
                    break; // Close connection
                }
                
                // Skip HTTP headers if we detected HTTP (shouldn't happen, but just in case)
                if (line.contains("HTTP/") || (lineCount > 1 && line.contains(": ") && 
                    (line.startsWith("Host:") || line.startsWith("Connection:") || 
                     line.startsWith("Cache-Control:") || line.startsWith("sec-ch-ua")))) {
                    // Likely an HTTP header, skip it silently
                    continue;
                }
                
                if (line.isEmpty()) {
                    continue;
                }
                
                System.out.println("[Gateway " + clientId + "] Received: " + line);
                
                // Parse request
                String response = processRequest(line);
                
                // Send response
                out.println(response);
                System.out.println("[Gateway " + clientId + "] Sent: " + response);
            }
            
            if (!isHttpRequest) {
                System.out.println("[Gateway " + clientId + "] Client disconnected");
            }
            
        } catch (IOException e) {
            System.err.println("[Gateway " + clientId + "] Error handling client: " + e.getMessage());
            e.printStackTrace();
        } finally {
            try {
                clientSocket.close();
            } catch (IOException e) {
                // Ignore
            }
        }
    }
    
    /**
     * Process a request line and return response
     * 
     * Input format: REQUEST_CROSS <carId> <direction> <arrivalTime>
     * Output format: DECISION <qcId> <orderPosition> <fullOrderAsCommaList>
     * 
     * The gateway performs two BFT operations:
     * 1. JOIN:carId - to register the car in the intersection queue
     * 2. LEAVE:carId - to request crossing (returns GO if winner, WAIT otherwise)
     */
    private String processRequest(String requestLine) {
        try {
            // Parse: REQUEST_CROSS <carId> <direction> <arrivalTime>
            String[] parts = requestLine.split("\\s+");
            if (parts.length != 4 || !parts[0].equals("REQUEST_CROSS")) {
                return "ERROR: Invalid request format. Expected: REQUEST_CROSS <carId> <direction> <arrivalTime>";
            }
            
            String carId = parts[1];
            String direction = parts[2]; // Note: direction is accepted but not used by current server protocol
            String arrivalTimeStr = parts[3];
            
            // Validate arrival time is a number
            try {
                Double.parseDouble(arrivalTimeStr);
            } catch (NumberFormatException e) {
                return "ERROR: Invalid arrival time: " + arrivalTimeStr;
            }
            
            // === BATCH-OF-4 CONSENSUS PROTOCOL ===
            
            // Step 1: Send JOIN request (this buffers the car)
            String joinRequest = "JOIN:" + carId;
            byte[] joinRequestBytes = joinRequest.getBytes(StandardCharsets.UTF_8);
            
            System.out.println("[Gateway " + clientId + "] Sending JOIN to BFT-SMaRt: " + joinRequest);
            long startTime = System.nanoTime();
            byte[] joinReplyBytes = serviceProxy.invokeOrdered(joinRequestBytes);
            
            if (joinReplyBytes == null) {
                return "ERROR: No reply from BFT-SMaRt for JOIN";
            }
            
            String joinReply = new String(joinReplyBytes, StandardCharsets.UTF_8).trim();
            System.out.println("[Gateway " + clientId + "] Received JOIN reply: " + joinReply);
            
            // Step 2: Send LEAVE request (this retrieves the final decision)
            // The server will return the FULL decision for ALL 4 cars once consensus is done
            String leaveRequest = "LEAVE:" + carId;
            byte[] leaveRequestBytes = leaveRequest.getBytes(StandardCharsets.UTF_8);
            
            System.out.println("[Gateway " + clientId + "] Sending LEAVE to BFT-SMaRt: " + leaveRequest);
            byte[] leaveReplyBytes = serviceProxy.invokeOrdered(leaveRequestBytes);
            long endTime = System.nanoTime();
            
            if (leaveReplyBytes == null) {
                return "ERROR: No reply from BFT-SMaRt for LEAVE";
            }
            
            // Parse response from server
            // Format: "CAR_0:GO:0;CAR_1:GO:5;CAR_2:GO:10;CAR_3:GO:15"
            String batchDecision = new String(leaveReplyBytes, StandardCharsets.UTF_8).trim();
            System.out.println("[Gateway " + clientId + "] Received batch decision: " + batchDecision);
            
            long consensusTimeMs = (endTime - startTime) / 1_000_000;
            System.out.println("[Gateway " + clientId + "] Consensus time: " + consensusTimeMs + " ms");
            
            // Handle special responses
            if (batchDecision.contains("WAIT_FOR_CONSENSUS")) {
                return "ERROR: Consensus not ready yet (shouldn't happen with proper batching)";
            }
            
            // Parse the batch decision to extract MY car's decision
            String[] carDecisions = batchDecision.split(";");
            String myDecision = null;
            int myPosition = -1;
            
            for (int pos = 0; pos < carDecisions.length; pos++) {
                String carDecision = carDecisions[pos].trim();
                
                // Format: "CAR_0:GO:0" or "CAR_1:GO:5"
                if (carDecision.startsWith(carId + ":")) {
                    myDecision = carDecision;
                    myPosition = pos;
                    break;
                }
            }
            
            if (myDecision == null) {
                return "ERROR: Car " + carId + " not found in batch decision: " + batchDecision;
            }
            
            System.out.println("[Gateway " + clientId + "] Car " + carId + " at position " + myPosition);
            System.out.println("[Gateway " + clientId + "] Sending decision: " + myDecision);
            
            // Return format: "DECISION <qcId> <orderPosition> <carId:GO:delay>"
            String qcId = String.valueOf(clientId);
            return String.format("DECISION %s %s %s", qcId, myPosition, myDecision);
            
        } catch (Exception e) {
            System.err.println("[Gateway " + clientId + "] Error processing request: " + e.getMessage());
            e.printStackTrace();
            return "ERROR: " + e.getMessage();
        }
    }
    
    /**
     * Parse decision from server response
     */
    private String parseDecision(String replyStr, String expectedCarId) {
        // Response format can be:
        // - "CAR_ID:GO"
        // - "CAR_ID:WAIT"
        // - "CAR_ID:JOINED_WAIT=1"
        // - Multiple: "CAR_ID1:GO;CAR_ID2:WAIT;..."
        
        if (replyStr.contains(";")) {
            // Multiple cars - find the one matching expectedCarId
            String[] decisions = replyStr.split(";");
            for (String decision : decisions) {
                String[] parts = decision.split(":");
                if (parts.length >= 2 && parts[0].equals(expectedCarId)) {
                    return parts[0] + ":" + parts[1];
                }
            }
            // If not found, return first one
            if (decisions.length > 0) {
                return decisions[0];
            }
        } else {
            // Single decision
            return replyStr;
        }
        
        return replyStr;
    }
    
    /**
     * Shutdown the gateway
     */
    public void shutdown() {
        try {
            if (serverSocket != null && !serverSocket.isClosed()) {
                serverSocket.close();
            }
            if (serviceProxy != null) {
                serviceProxy.close();
            }
            System.out.println("[Gateway " + clientId + "] Shutdown complete");
        } catch (IOException e) {
            System.err.println("[Gateway " + clientId + "] Error during shutdown: " + e.getMessage());
        }
    }
    
    public static void main(String[] args) {
        if (args.length < 2) {
            System.out.println("Usage: java ... IntersectionGateway <clientId> <listenPort>");
            System.out.println();
            System.out.println("Example:");
            System.out.println("  java ... IntersectionGateway 1 9001");
            System.out.println("  java ... IntersectionGateway 2 9002");
            System.exit(-1);
        }
        
        int clientId = Integer.parseInt(args[0]);
        int listenPort = Integer.parseInt(args[1]);
        
        IntersectionGateway gateway = new IntersectionGateway(clientId, listenPort);
        
        // Add shutdown hook
        Runtime.getRuntime().addShutdownHook(new Thread(() -> {
            System.out.println("\n[Gateway " + clientId + "] Shutdown signal received");
            gateway.shutdown();
        }));
        
        try {
            gateway.start();
        } catch (IOException e) {
            System.err.println("[Gateway " + clientId + "] Failed to start: " + e.getMessage());
            e.printStackTrace();
            System.exit(1);
        }
    }
}


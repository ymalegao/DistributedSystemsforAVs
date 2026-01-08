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

import java.io.IOException;
import bftsmart.tom.ServiceProxy;

/**
 * Client for the BFT replicated intersection service.
 * Sends car arrival requests in the format: "CAR_ID1:direction1:arrival_time1;CAR_ID2:direction2:arrival_time2;..."
 * All cars in one request are processed in a single BFT consensus round.
 * 
 * Usage:
 *   Multiple cars (semicolon-separated): IntersectionClient <clientId> <carId1>:<dir1>:<time1> <carId2>:<dir2>:<time2> ...
 *   Multiple cars (auto-generated): IntersectionClient <clientId> <numCars>
 * 
 * @author Intersection Demo
 */
public class IntersectionClient {

    public static void main(String[] args) throws IOException {
        if (args.length < 2) {
            System.out.println("Usage (single request with multiple cars):");
            System.out.println("  java ... IntersectionClient <clientId> <carId1>:<direction1>:<arrivalTime1>;<carId2>:<direction2>:<arrivalTime2>;...");
            System.out.println();
            System.out.println("Usage (space-separated car entries):");
            System.out.println("  java ... IntersectionClient <clientId> <carId1>:<dir1>:<time1> <carId2>:<dir2>:<time2> ...");
            System.out.println();
            System.out.println("Usage (auto-generate for testing):");
            System.out.println("  java ... IntersectionClient <clientId> <numCars>");
            System.out.println();
            System.out.println("Examples:");
            System.out.println("  java ... IntersectionClient 1000 \"CAR_1:straight:12.3;CAR_2:right:10.1;CAR_3:left:15.7;CAR_4:straight:11.2\"");
            System.out.println("  java ... IntersectionClient 1000 CAR_1:left:12.3 CAR_2:right:10.1 CAR_3:straight:15.7");
            System.out.println("  java ... IntersectionClient 1000 4  (generates 4 cars: CAR_1 to CAR_4)");
            System.exit(-1);
        }
        
        int clientId = Integer.parseInt(args[0]);
        
        try (ServiceProxy intersectionProxy = new ServiceProxy(clientId)) {
            
            String request;
            
            // Check if this is auto-generate mode (just 2 args: clientId and numCars)
            if (args.length == 2 && !args[1].contains(":")) {
                // Auto-generate multiple cars for testing
                int numCars = Integer.parseInt(args[1]);
                System.out.println("Auto-generating " + numCars + " cars for testing...");
                System.out.println();
                
                StringBuilder requestBuilder = new StringBuilder();
                for (int i = 1; i <= numCars; i++) {
                    String carId = "CAR_" + i;
                    String direction = (i % 3 == 0) ? "left" : (i % 3 == 1) ? "straight" : "right";
                    // Generate different arrival times - make CAR_2 earliest for testing
                    double arrivalTime = 10.0 + (i * 0.5); // CAR_1=10.5, CAR_2=11.0, CAR_3=11.5, CAR_4=12.0
                    if (i == 2) {
                        arrivalTime = 10.1; // Make CAR_2 earliest
                    }
                    
                    if (i > 1) {
                        requestBuilder.append(";");
                    }
                    requestBuilder.append(carId).append(":").append(direction).append(":").append(arrivalTime);
                    System.out.println("  Car: " + carId + ", Direction: " + direction + ", Arrival Time: " + arrivalTime);
                }
                request = requestBuilder.toString();
                
            } else if (args.length == 2 && args[1].contains(";")) {
                // Single argument that's already semicolon-separated
                request = args[1];
                System.out.println("Using provided request string:");
                String[] cars = request.split(";");
                for (String car : cars) {
                    System.out.println("  " + car);
                }
                System.out.println();
                
            } else {
                // Multiple space-separated arguments - join with semicolons
                StringBuilder requestBuilder = new StringBuilder();
                for (int i = 1; i < args.length; i++) {
                    if (i > 1) {
                        requestBuilder.append(";");
                    }
                    requestBuilder.append(args[i]);
                }
                request = requestBuilder.toString();
                System.out.println("Building request from space-separated arguments:");
                String[] cars = request.split(";");
                for (String car : cars) {
                    System.out.println("  " + car);
                }
                System.out.println();
            }
            
            sendMultiCarRequest(intersectionProxy, request);
        }
    }
    
    /**
     * Send a multi-car request to the intersection server in a single consensus round.
     * Request format: "CAR_ID1:direction1:arrival_time1;CAR_ID2:direction2:arrival_time2;..."
     * Response format: "CAR_ID1:GO;CAR_ID2:WAIT;CAR_ID3:WAIT;..."
     * 
     * @param proxy The ServiceProxy to use
     * @param request The request string with all cars (semicolon-separated)
     */
    private static void sendMultiCarRequest(ServiceProxy proxy, String request) throws IOException {
        byte[] requestBytes = request.getBytes("UTF-8");
        
        System.out.println();
        System.out.println("Sending request (single consensus round):");
        System.out.println("  " + request);
        System.out.println();
        
        // Measure consensus time (end-to-end: from client send to response receive)
        long startTime = System.nanoTime();
        
        // Send ordered request through consensus (all cars processed together)
        byte[] reply = proxy.invokeOrdered(requestBytes);
        
        long endTime = System.nanoTime();
        long consensusTimeNs = endTime - startTime;
        double consensusTimeMs = consensusTimeNs / 1_000_000.0;
        double consensusTimeUs = consensusTimeNs / 1_000.0;
        
        System.out.println("═══════════════════════════════════════════════════════════");
        System.out.println("CONSENSUS TIMING:");
        System.out.println("  Total consensus time: " + String.format("%.3f", consensusTimeMs) + " ms (" + 
                          String.format("%.2f", consensusTimeUs) + " μs)");
        System.out.println("═══════════════════════════════════════════════════════════");
        System.out.println();
        
        if (reply != null) {
            String replyStr = new String(reply, "UTF-8").trim();
            System.out.println("Response received:");
            System.out.println("  " + replyStr);
            System.out.println();
            
            // Parse and display individual car decisions
            String[] decisions = replyStr.split(";");
            for (String decision : decisions) {
                String[] parts = decision.split(":");
                if (parts.length == 2) {
                    String carId = parts[0];
                    String result = parts[1];
                    
                    if (result.equals("GO")) {
                        System.out.println("  ✓ Car " + carId + " can GO - proceed through intersection!");
                    } else if (result.equals("WAIT")) {
                        System.out.println("  ⏸ Car " + carId + " must WAIT");
                    } else {
                        System.out.println("  ⚠ Car " + carId + " - Unexpected result: " + result);
                    }
                }
            }
        } else {
            System.out.println("ERROR! No reply received.");
        }
        System.out.println();
    }
}


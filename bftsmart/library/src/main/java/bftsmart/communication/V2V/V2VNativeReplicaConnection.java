package bftsmart.communication.V2V;

import java.io.*;

/**
 * V2V Replica connection using JNI bridge (NO TCP SOCKETS!)
 * This replaces V2VReplicaConnection for true V2V communication via OMNeT++
 */
public class V2VNativeReplicaConnection {
    private final int myReplicaId;
    private final int targetReplicaId;
    private final V2VNativeBridge bridge;

    public V2VNativeReplicaConnection(int myReplicaId, int targetReplicaId, V2VNativeBridge bridge) {
        this.myReplicaId = myReplicaId;
        this.targetReplicaId = targetReplicaId;
        this.bridge = bridge;

        System.out.println("[V2VNativeReplicaConnection] Created JNI connection from "
            + myReplicaId + " to " + targetReplicaId);
    }

    /**
     * Send envelope via JNI to OMNeT++ which will broadcast via V2V
     * Uses reactive yield pattern - waits if radio is busy
     */
    public synchronized void send(V2VMessageEnvelope envelope) {
        System.out.println("[V2VNativeConnection] send() called: " + myReplicaId + "->" + targetReplicaId 
                          + " seqNum=" + envelope.sequenceNum + " type=" + envelope.type);
        System.out.flush();
        try {
            // Serialize envelope to bytes first
            System.out.println("[V2VNativeConnection] Serializing envelope...");
            ByteArrayOutputStream baos = new ByteArrayOutputStream();
            ObjectOutputStream oos = new ObjectOutputStream(baos);
            oos.writeObject(envelope);
            oos.flush();
            byte[] data = baos.toByteArray();
            System.out.println("[V2VNativeConnection] Serialized to " + data.length + " bytes");

            // REACTIVE YIELD: Wait while radio is busy (syncs Java with simulation time)
            // int waitCount = 0;
            // while (bridge.isRadioBusy()) {
            //     try {
            //         Thread.sleep(1);  // Small sleep to avoid busy-wait
            //         waitCount++;
            //         if (waitCount > 5000) {  // 5 second timeout
            //             System.err.println("[V2VNativeReplicaConnection] Radio busy timeout!");
            //             break;
            //         }
            //     } catch (InterruptedException e) {
            //         Thread.currentThread().interrupt();
            //         break;
            //     }
            // }

            // Send via JNI (OMNeT++ will broadcast via V2V)
            System.out.println("[V2VNativeConnection] Calling bridge.sendMessage(-1, " + data.length + " bytes)...");
            System.out.flush();
            boolean success = bridge.sendMessage(-1, data);
            System.out.println("[V2VNativeConnection] bridge.sendMessage returned: " + success);
            System.out.flush();

            if (!success) {
                System.err.println("[V2VNativeReplicaConnection] Failed to queue message to V2V");
            }

        } catch (IOException e) {
            System.err.println("[V2VNativeReplicaConnection] ERROR serializing: " + e.getMessage());
        }
    }

    public void close() {
        // No resources to clean up (JNI bridge is shared)
        System.out.println("[V2VNativeReplicaConnection] Closed connection to replica " + targetReplicaId);
    }
}



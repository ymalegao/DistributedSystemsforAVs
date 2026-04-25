package bftsmart.communication.V2V;

import bftsmart.communication.SystemMessage;
import bftsmart.communication.server.ServersCommunicationLayerInterface;
import bftsmart.reconfiguration.ServerViewController;
import bftsmart.tom.ServiceReplica;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.HashMap;
import java.util.Arrays;
import javax.crypto.SecretKey;
import bftsmart.communication.client.RequestReceiver;
// import bftsmart.communication.V2V.SimulationTimer;


 /**
   * V2V-based communication layer for BFT-SMaRt replicas.
   * Replaces TCP/SSL connections with V2V message passing via socket bridge to Veins.
   */

public class V2VServersCommunicationLayer extends Thread implements ServersCommunicationLayerInterface {
    private final ServerViewController controller;
    private final LinkedBlockingQueue<SystemMessage> inQueue;
    private final int me; //replica ID
    private boolean doWork = true;

    private final int veinsListenPort; //port where veins sends v2v messages to this replica
    public final HashMap<Integer, V2VNativeReplicaConnection> replicaConnections;

    private final ReliableV2VMessaging reliabilityLayer;
    private RequestReceiver requestReceiver;  // Not final - can be set later via setRequestReceiver()
    private bftsmart.demo.intersection.IntersectionServer intersectionServer;
    // private SimulationTimer simulationTimer;
    public V2VServersCommunicationLayer(
        ServerViewController controller,
        LinkedBlockingQueue<SystemMessage> inQueue,
        ServiceReplica replica,
        int veinsListenPort,
        RequestReceiver requestReceiver) {
            this.controller = controller;
            this.inQueue = inQueue;
            this.me = controller.getStaticConf().getProcessId();
            this.veinsListenPort = veinsListenPort;
            this.replicaConnections = new HashMap<>();
            this.reliabilityLayer = new ReliableV2VMessaging(me, this);
            this.requestReceiver = requestReceiver;
            initReplicaConnections();
            // startV2VListener();

            // CRITICAL: Start the communication thread (same as standard ServersCommunicationLayer)
            System.out.println("[V2V Layer " + me + "] DEBUG: About to start communication thread...");
            start();
            System.out.println("[V2V Layer " + me + "] DEBUG: start() called, thread should be running now");
        }
    
    private void initReplicaConnections(){
        int[] replicaIds = controller.getCurrentViewProcesses();
        System.out.println("==============================================");
        System.out.println("[V2V Layer " + me + "] INITIALIZING CONNECTIONS");
        System.out.println("    My ID: " + me);
        System.out.println("    All replicas: " + Arrays.toString(replicaIds));

        // Create V2VNativeBridge for JNI communication
        System.out.println("[V2V Layer " + me + "] DEBUG: About to create V2VNativeBridge callback...");
        V2VNativeBridge.MessageReceiverCallback callback = (fromReplicaId, messageData) -> {
            System.out.println("[V2V Layer " + me + "] Received message from replica " + fromReplicaId + " via JNI");
            // Deserialize and deliver message
            try {
                java.io.ByteArrayInputStream bis = new java.io.ByteArrayInputStream(messageData);
                java.io.ObjectInputStream ois = new java.io.ObjectInputStream(bis);
                V2VMessageEnvelope envelope = (V2VMessageEnvelope) ois.readObject();
                reliabilityLayer.handleIncomingMessage(envelope);
            } catch (Exception e) {
                System.err.println("[V2V Layer " + me + "] ERROR deserializing message: " + e.getMessage());
                e.printStackTrace();
            }
        };
        System.out.println("[V2V Layer " + me + "] DEBUG: Callback created, creating V2VNativeBridge...");
        V2VNativeBridge bridge = new V2VNativeBridge(me, callback);
        System.out.println("[V2V Layer " + me + "] DEBUG: Bridge created and callback registered!");

        // Small delay to ensure ALL replicas have registered callbacks before anyone starts sending
        // This prevents "callback not registered" errors when messages arrive
        try {
            System.out.println("[V2V Layer " + me + "] DEBUG: Waiting 500ms for all peers to register callbacks...");
            Thread.sleep(500);
        } catch (InterruptedException e) {
            e.printStackTrace();
        }
        System.out.println("[V2V Layer " + me + "] DEBUG: Creating connections...");
        for (int replicaId: replicaIds){
            if (replicaId != me){
                V2VNativeReplicaConnection conn = new V2VNativeReplicaConnection(me, replicaId, bridge);
                replicaConnections.put(replicaId, conn);
                System.out.println("    -> Native JNI connection to replica " + replicaId);
            }
        }
        System.out.println("==============================================");
    }

     /**
   * Set the IntersectionServer reference for zombie filtering.
   * @param server The IntersectionServer instance
   */
    public void setServer(bftsmart.demo.intersection.IntersectionServer server) {
        this.intersectionServer = server;
        System.out.println("[V2V Layer " + me + "] IntersectionServer reference set");
    }


    /**
     * Thread run method - required because this class extends Thread.
     * BFT-SMaRt calls start() on the communication layer during ServiceReplica construction.
     *
     * For V2V, we don't need a traditional accept() loop since messages arrive via JNI callbacks.
     * This thread just needs to stay alive to keep the communication system running.
     */
    @Override
    public void run() {
        System.out.println("==============================================");
        System.out.println("[V2V Layer " + me + "] Communication thread STARTED");
        System.out.println("    Using JNI callbacks for message delivery");
        System.out.println("==============================================");

        // Keep thread alive while system is running
        while (doWork) {
            try {
                // Sleep for 100ms intervals to allow clean shutdown
                Thread.sleep(100);
            } catch (InterruptedException e) {
                System.out.println("[V2V Layer " + me + "] Communication thread interrupted, shutting down...");
                Thread.currentThread().interrupt();
                break;
            }
        }

        System.out.println("[V2V Layer " + me + "] Communication thread STOPPED");
    }

    public void setRequestReceiver(RequestReceiver requestReceiver){
        this.requestReceiver = requestReceiver;
        System.out.println("[V2V Layer " + me + "] Request receiver set - ready to handle client requests");


    }

    public void deliverToBFTSmart(SystemMessage message){
        // REMOVED: No zombie filtering - using BFT reconfiguration instead
        // Reconfiguration consensus needs messages from all replicas!

        // Mark authenticated: messages arrive via V2V physical channel (known replicas)
        // or are self-sent (e.g. TRIGGER_LC_LOCALLY). MAC auth is not used in this V2V model.
        message.authenticated = true;

        System.out.println("=== [V2V Layer " + me + "] DELIVER to BFT-SMaRt ===");
        System.out.println("    Message type: " + message.getClass().getSimpleName());
        System.out.println("    From replica: " + message.getSender());
        System.out.println("    InQueue size before: " + inQueue.size());
        try{
            inQueue.put(message);
            System.out.println("    InQueue size after: " + inQueue.size());
            System.out.println("=== [V2V Layer " + me + "] DELIVER successful ===");
        } catch (InterruptedException e) {
            System.err.println("=== [V2V Layer " + me + "] DELIVER ERROR ===");
            System.err.println("    Error: " + e.getMessage());
            e.printStackTrace();
        }
    }


    //called by BFTSmart consensus
    public void send(int[] targets, SystemMessage sm, boolean serializeClassHeaders){
        System.out.println("=== [V2V Layer " + me + "] SEND called ===");
        System.out.println("    Targets: " + Arrays.toString(targets));
        System.out.println("    Message type: " + sm.getClass().getSimpleName());
        System.out.println("    Sender: " + sm.getSender());
        System.out.flush();  // Force immediate output

        boolean deliveredToSelf = false;
        int remoteTargetCount = 0;

        for (int target: targets){
            if (target == me){
                // if (intersectionServer != null && intersectionServer.isReplicaDeparted(target)) {
                //     System.out.println("    -> Skipping send to zombie replica " + target +
                //                      " (message type: " + sm.getClass().getSimpleName() + ")");
                //     continue;  // Skip this target
                // }
                
                deliveredToSelf = true;
                deliverToBFTSmart(sm);
                
            }else{
                remoteTargetCount++;
            }
        }

        if (remoteTargetCount > 1) {
            System.out.println("    -> MULTICAST MODE: Broadcasting to " + remoteTargetCount + " targets");
            System.out.flush();
            V2VNativeReplicaConnection anyConn = replicaConnections.values().iterator().next();
            if (anyConn != null){
                System.out.println("    -> Calling reliabilityLayer.sendMulticast...");
                System.out.flush();
                reliabilityLayer.sendMulticast(targets, sm, anyConn);
                System.out.println("    -> reliabilityLayer.sendMulticast returned");
                System.out.flush();
            } else {
                System.err.println("    -> ERROR: No connection available for multicast!");
                System.err.flush();
            }

        }else{
            for (int target: targets){
                if (target == me) continue;

                V2VNativeReplicaConnection conn = replicaConnections.get(target);
                if (conn != null){
                    System.out.println("    -> UNICAST to replica " + target);
                    System.out.flush();
                    reliabilityLayer.sendReliable(target, sm, conn);
                }
            }
        }
        System.out.println("=== [V2V Layer " + me + "] SEND complete ===");
        System.out.flush();
    }
        
    //     for (int target: targets){
    //         if (target == me){
    //             System.out.println("    -> Self-delivery to replica " + target);
    //             deliverToBFTSmart(sm);
    //             continue;
    //         }

    //         V2VNativeReplicaConnection conn = replicaConnections.get(target);
    //         if (conn != null){
    //             System.out.println("    -> Sending to replica " + target + " via V2V");
    //             reliabilityLayer.sendReliable(target, sm, conn);
    //         }else{
    //             System.err.println("    -> ERROR: No connection found for replica " + target);
    //         }
    //     }
    //     System.out.println("=== [V2V Layer " + me + "] SEND complete ===");
    // }

    public void shutdown(){
        doWork = false;
        for (V2VNativeReplicaConnection conn: replicaConnections.values()){
            conn.close();
        }
    }

    public SecretKey getSecretKey(int id){
        return null;
    }

    public void joinViewReceived() {
        System.out.println("==============================================");
        System.out.println("[V2V Layer " + me + "] joinViewReceived() called");
        System.out.println("    Replica " + me + " has joined the view");
        System.out.println("    V2V connections already established via JNI");
        System.out.println("==============================================");

    }
    public void updateConnections() {
        System.out.println("==============================================");
        System.out.println("[V2V Layer " + me + "] updateConnections() called");
        int[] currentView = controller.getCurrentViewProcesses();
        System.out.println("    Current view: " + Arrays.toString(currentView));

        // Remove connections to replicas no longer in view
        replicaConnections.keySet().removeIf(replicaId -> {
            boolean inView = false;
            for (int id : currentView) {
                if (id == replicaId) {
                    inView = true;
                    break;
                }
            }
            if (!inView) {
                System.out.println("    -> Removing connection to replica " + replicaId + " (not in view)");
                V2VNativeReplicaConnection conn = replicaConnections.get(replicaId);
                if (conn != null) {
                    conn.close();
                }
            }
            return !inView;
        });

        // Add connections to new replicas
        for (int replicaId : currentView) {
            if (replicaId != me && !replicaConnections.containsKey(replicaId)) {
                System.out.println("    -> Adding connection to new replica " + replicaId);
                V2VNativeBridge bridge = new V2VNativeBridge(me, null); // Reuse existing bridge
                V2VNativeReplicaConnection conn = new V2VNativeReplicaConnection(me, replicaId, bridge);
                replicaConnections.put(replicaId, conn);
            }
        }
        System.out.println("==============================================");
    }


}
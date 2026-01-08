package bftsmart.communication.V2V;

import bftsmart.communication.SystemMessage;
import bftsmart.communication.server.ServersCommunicationLayerInterface;
import bftsmart.reconfiguration.ServerViewController;
import bftsmart.tom.ServiceReplica;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.HashMap;
import java.io.*;
import java.net.*;
import java.util.Arrays;
import javax.crypto.SecretKey;
import bftsmart.tom.core.messages.TOMMessage;
import bftsmart.communication.client.RequestReceiver;



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
    public final HashMap<Integer, V2VReplicaConnection> replicaConnections;

    private final ReliableV2VMessaging reliabilityLayer;
    private final RequestReceiver requestReceiver;
    
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
            startV2VListener();
        }
    
    private void initReplicaConnections(){
        int[] replicaIds = controller.getCurrentViewProcesses();
        System.out.println("==============================================");
        System.out.println("[V2V Layer " + me + "] INITIALIZING CONNECTIONS");
        System.out.println("    My ID: " + me);
        System.out.println("    All replicas: " + Arrays.toString(replicaIds));

        for (int replicaId: replicaIds){
            if (replicaId != me){
                int targetPort = 10000 + replicaId;
                V2VReplicaConnection conn = new V2VReplicaConnection(replicaId, "localhost", targetPort);
                replicaConnections.put(replicaId, conn);
                System.out.println("    -> Connection to replica " + replicaId + " at port " + targetPort);
            }
        }
        System.out.println("==============================================");
    }

    private void startV2VListener(){
        new Thread(() -> {
            try (ServerSocket listener = new ServerSocket(veinsListenPort)){
                System.out.println("==============================================");
                System.out.println("[V2V Layer " + me + "] LISTENER STARTED");
                System.out.println("    Listening on port: " + veinsListenPort);
                System.out.println("==============================================");

                while (doWork){
                    System.out.println("[V2V Layer " + me + "] Waiting for incoming connection...");
                    Socket clientSocket = listener.accept();
                    System.out.println("[V2V Layer " + me + "] Connection accepted from: " + clientSocket.getRemoteSocketAddress());
                    new Thread(()-> handleIncomingV2VMessage(clientSocket)).start();
                }
            } catch (IOException e) {
                System.err.println("==============================================");
                System.err.println("[V2V Layer " + me + "] LISTENER ERROR");
                System.err.println("    Error: " + e.getMessage());
                System.err.println("==============================================");
                e.printStackTrace();
            }


        }).start();
    }

    private void handleIncomingV2VMessage(Socket socket){
        System.out.println("=== [V2V Layer " + me + "] RECEIVE: New connection accepted ===");
        try(ObjectInputStream in = new ObjectInputStream(socket.getInputStream())){
            V2VMessageEnvelope envelope = (V2VMessageEnvelope) in.readObject();
            System.out.println("=== [V2V Layer " + me + "] RECEIVE: Message decoded ===");
            System.out.println("    From replica: " + envelope.fromReplicaId);
            System.out.println("    Sequence number: " + envelope.sequenceNum);
            System.out.println("    Envelope type: " + envelope.type);
            System.out.println("    Payload size: " + (envelope.serializedMessage != null ? envelope.serializedMessage.length + " bytes" : "null"));

            //pass to reliability layer for ordering/deduplication or retransmission -- this is going to be based on reliable UDP I think? 
            if (envelope.serializedMessage != null){
                Object msg = deserialize(envelope.serializedMessage);
                if (msg instanceof TOMMessage){
                    System.out.println("    -> Message is a TOMMessage");
                    //handle this by delivering to client handler. 
                }else{
                    reliabilityLayer.handleIncomingMessage(envelope);
                    System.out.println("=== [V2V Layer " + me + "] RECEIVE: Passed to reliability layer ===");

                }
            }
            
           

        } catch (Exception e) {
            System.err.println("=== [V2V Layer " + me + "] RECEIVE ERROR ===");
            System.err.println("    Error: " + e.getMessage());
            e.printStackTrace();
        }
    }

    private Object deserialize(byte[] serialized){
        try(ByteArrayInputStream bis = new ByteArrayInputStream(serialized);
            ObjectInputStream ois = new ObjectInputStream(bis)){
                return ois.readObject();
            } catch (Exception e) {
                System.err.println("    -> ERROR: Failed to deserialize message");
                e.printStackTrace();
                return null;
            }
        }


    public void deliverToBFTSmart(SystemMessage message){
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
        
        for (int target: targets){
            if (target == me){
                System.out.println("    -> Self-delivery to replica " + target);
                deliverToBFTSmart(sm);
                continue;
            }

            V2VReplicaConnection conn = replicaConnections.get(target);
            if (conn != null){
                System.out.println("    -> Sending to replica " + target + " via V2V");
                reliabilityLayer.sendReliable(target, sm, conn);
            }else{
                System.err.println("    -> ERROR: No connection found for replica " + target);
            }
        }
        System.out.println("=== [V2V Layer " + me + "] SEND complete ===");
    }

    public void shutdown(){
        doWork = false;
        for (V2VReplicaConnection conn: replicaConnections.values()){
            conn.close();
        }
    }

    public SecretKey getSecretKey(int id){
        return null;
    }

    public void joinViewReceived() {}
    public void updateConnections() {}


}
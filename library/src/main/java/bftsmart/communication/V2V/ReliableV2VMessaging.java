package bftsmart.communication.V2V;

import bftsmart.communication.SystemMessage;
import java.io.*;
import java.util.*;
import java.util.concurrent.*;

public class ReliableV2VMessaging {
    private final int myReplicaId;
    private final V2VServersCommunicationLayer parent;

    private final ConcurrentHashMap<Integer, Long> sendSeqNums = new ConcurrentHashMap<>();

    private final ConcurrentHashMap<Integer, Long> expectedSeqNums = new ConcurrentHashMap<>();

    private final ConcurrentHashMap<Integer, PriorityQueue<V2VMessageEnvelope>> recieveBuffers = new ConcurrentHashMap<>();

    private final ConcurrentHashMap<Integer, ConcurrentHashMap<Long, V2VMessageEnvelope>> unackedMessages = new ConcurrentHashMap<>();

    private final ConcurrentHashMap<Integer, ConcurrentHashMap<Long, Integer>> retransmissionAttempts = new ConcurrentHashMap<>();

    private final ScheduledExecutorService retxScheduler = Executors.newScheduledThreadPool(1);

    private static final long RETX_TIMEOUT_MS = 100; //retrty after 100ms

    private static final int MAX_RETX_ATTEMPTS = 5;  

    public ReliableV2VMessaging(int myReplicaId, V2VServersCommunicationLayer parent) {
        this.myReplicaId = myReplicaId;
        this.parent = parent;

        retxScheduler.scheduleAtFixedRate(this::checkRetransmissions, RETX_TIMEOUT_MS, RETX_TIMEOUT_MS, TimeUnit.MILLISECONDS);


        

    
    }

    public void sendReliable(int targetId, SystemMessage message, V2VReplicaConnection conn){
        System.out.println("    [Reliability " + myReplicaId + "] sendReliable() called");
        System.out.println("        Target: " + targetId);
        System.out.println("        Message type: " + message.getClass().getSimpleName());
        
        try{
            //get next sequence number for this destination
            long seq = sendSeqNums.computeIfAbsent(targetId, k->0L); //explain this line later
            sendSeqNums.put(targetId, seq + 1);
            System.out.println("        Sequence number: " + seq);


            //serialize the message
            ByteArrayOutputStream baos = new ByteArrayOutputStream();
            ObjectOutputStream oos = new ObjectOutputStream(baos);
            oos.writeObject(message);
            oos.flush();
            byte[] payload = baos.toByteArray();
            System.out.println("        Payload size: " + payload.length + " bytes");


            long ackNum = expectedSeqNums.getOrDefault(targetId, 0L);
            V2VMessageEnvelope envelope = new V2VMessageEnvelope(
                V2VMessageEnvelope.MessageType.DATA,
                myReplicaId,
                targetId,
                seq,
                ackNum, //where does ackNum come from?
                payload

            );

            conn.send(envelope);
            unackedMessages.computeIfAbsent(targetId, k->new ConcurrentHashMap<>()).put(seq, envelope);
            System.out.println("        ✓ [Reliability " + myReplicaId + "] Sent seq=" + seq + " to replica " + targetId);
        } catch (IOException e) {
            System.err.println("        ✗ [Reliability " + myReplicaId + "] Error sending message: " + e.getMessage());
            e.printStackTrace();
        }
        
    }

    public void handleIncomingMessage(V2VMessageEnvelope envelope){
        System.out.println("    [Reliability " + myReplicaId + "] handleIncomingMessage() called");
        System.out.println("        Type: " + envelope.type);
        System.out.println("        From: " + envelope.fromReplicaId);
        System.out.println("        SeqNum: " + envelope.sequenceNum);
        
        if (envelope.type == V2VMessageEnvelope.MessageType.ACK){
            System.out.println("        -> Processing ACK");
            handleAck(envelope);
            return;
        }

        int senderId = envelope.fromReplicaId;
        long seq = envelope.sequenceNum;
        long expected = expectedSeqNums.getOrDefault(senderId, 0L);
        System.out.println("        Expected seq: " + expected);

        sendAck(senderId, seq);

        if ( seq < expected) {
            //duplcate ignore
            System.out.println("        -> DUPLICATE (seq < expected), ignoring");
            return;
        }

        if (seq == expected){
            System.out.println("        -> IN ORDER, delivering immediately");
            deliverMessage(envelope);
            expectedSeqNums.put(senderId, expected + 1);

            //check buffer for any subsequent inorder messages
            drainReceiveBuffer(senderId);
            

        }else{
            System.out.println("        -> OUT OF ORDER (seq=" + seq + ", expected=" + expected + "), buffering");
            recieveBuffers.computeIfAbsent(senderId, k->new PriorityQueue<>(Comparator.comparingLong(e->e.sequenceNum))).add(envelope); // need to explain this line later as well

        }

    }

    private void deliverMessage(V2VMessageEnvelope envelope){
        System.out.println("    [Reliability " + myReplicaId + "] deliverMessage() - deserializing payload");
        try{
            ByteArrayInputStream bais = new ByteArrayInputStream(envelope.serializedMessage);
            ObjectInputStream ois = new ObjectInputStream(bais);
            SystemMessage message = (SystemMessage) ois.readObject();
            System.out.println("        Deserialized: " + message.getClass().getSimpleName());
            System.out.println("        From replica: " + message.getSender());

            message.authenticated = true;

            parent.deliverToBFTSmart(message);

        }catch (Exception e){
            System.err.println("        ✗ [Reliability " + myReplicaId + "] Error delivering message: " + e.getMessage());
            e.printStackTrace();
        }
    }

    private void drainReceiveBuffer(int senderId){
        PriorityQueue<V2VMessageEnvelope> buffer = recieveBuffers.get(senderId);
        if (buffer == null) return;

        long expected = expectedSeqNums.get(senderId);

        while (!buffer.isEmpty() && buffer.peek().sequenceNum == expected){
            deliverMessage(buffer.poll());
            expected++;
            expectedSeqNums.put(senderId, expected);
        }
    }

    private void handleAck(V2VMessageEnvelope ack){
        int from = ack.fromReplicaId;
        long ackNum = ack.ackNum;

        ConcurrentHashMap<Long, V2VMessageEnvelope> pending = unackedMessages.get(from);
        if (pending != null){
            pending.remove(ackNum);
            System.out.println("[Reliability " + myReplicaId + "] Received ack for seq=" + ackNum + " from replica " + from);
            
            // Clean up retransmission attempt counter
            ConcurrentHashMap<Long, Integer> targetAttempts = retransmissionAttempts.get(from);
            if (targetAttempts != null) {
                targetAttempts.remove(ackNum);
            }
        }
    }

    private void sendAck(int targetId, long ackNum){
        V2VReplicaConnection conn = parent.replicaConnections.get(targetId);
        if (conn != null ){
            V2VMessageEnvelope ack = V2VMessageEnvelope.createAck(myReplicaId, targetId, ackNum);
            conn.send(ack);
            System.out.println("[Reliability " + myReplicaId + "] Sent ack for seq=" + ackNum + " to replica " + targetId);
        }
    }

    private void checkRetransmissions(){
        for (Map.Entry<Integer, ConcurrentHashMap<Long, V2VMessageEnvelope>> entry: unackedMessages.entrySet()){
            int targetId = entry.getKey();
            ConcurrentHashMap<Long, V2VMessageEnvelope> pending = entry.getValue();

            long now = System.currentTimeMillis();
            List<Long> toRemove = new ArrayList<>();
            
            for (V2VMessageEnvelope envelope: pending.values()){
                if (now - envelope.timestampMs > RETX_TIMEOUT_MS){
                    // Get or initialize attempt count for this message
                    ConcurrentHashMap<Long, Integer> targetAttempts = retransmissionAttempts.computeIfAbsent(targetId, k -> new ConcurrentHashMap<>());
                    int attempts = targetAttempts.getOrDefault(envelope.sequenceNum, 0);
                    
                    if (attempts >= MAX_RETX_ATTEMPTS) {
                        // Max retransmissions reached, give up on this message
                        System.err.println("[Reliability " + myReplicaId + "] ✗ Max retransmission attempts (" + MAX_RETX_ATTEMPTS + ") reached for seq=" + envelope.sequenceNum + " to replica " + targetId + ", dropping message");
                        toRemove.add(envelope.sequenceNum);
                        targetAttempts.remove(envelope.sequenceNum);
                    } else {
                        // Retransmit and increment attempt count
                        V2VReplicaConnection conn = parent.replicaConnections.get(targetId);
                        if (conn != null){
                            System.out.println("[Reliability " + myReplicaId + "] Retransmitting seq=" + envelope.sequenceNum + " to replica " + targetId + " (attempt " + (attempts + 1) + "/" + MAX_RETX_ATTEMPTS + ")");
                            conn.send(envelope);
                            targetAttempts.put(envelope.sequenceNum, attempts + 1);
                        }
                    }
                }
            }
            
            // Remove messages that exceeded max attempts
            for (Long seq : toRemove) {
                pending.remove(seq);
            }
        }
    }

}

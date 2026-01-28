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



    private final ConcurrentHashMap<Integer, Set<Long>> pendingAcks = new ConcurrentHashMap<>();

    private final ScheduledExecutorService retxScheduler = Executors.newScheduledThreadPool(1);

    private static final long RETX_TIMEOUT_MS = 1000; //retry after 500ms

    private static final int MAX_RETX_ATTEMPTS = 20;

    // Add jitter to reduce collision probability during retransmissions
    private final java.util.Random jitterRandom = new java.util.Random();

    public ReliableV2VMessaging(int myReplicaId, V2VServersCommunicationLayer parent) {
        this.myReplicaId = myReplicaId;
        this.parent = parent;

        // DISABLED: Automatic retransmissions use real-time scheduler, incompatible with simulation time
        // Rely on piggybacked ACKs during consensus instead
        retxScheduler.scheduleAtFixedRate(this::checkRetransmissions, RETX_TIMEOUT_MS, RETX_TIMEOUT_MS, TimeUnit.MILLISECONDS);
        System.out.println("[Reliability " + myReplicaId + "] Automatic retransmissions DISABLED - using piggybacked ACKs only");


    }

    public void sendReliable(int targetId, SystemMessage message, V2VNativeReplicaConnection conn){
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
            // long timestampMs = SimulationClock.currentTimeMillis();
            V2VMessageEnvelope envelope = new V2VMessageEnvelope(
                V2VMessageEnvelope.MessageType.DATA,
                myReplicaId,
                targetId,
                seq,
                ackNum, //where does ackNum come from?
                payload

            );

            
            Set<Long> acksForTarget = pendingAcks.get(targetId);
            if (acksForTarget != null && !acksForTarget.isEmpty()){
                Iterator<Long> it = acksForTarget.iterator();
                int count = 0;
                while (it.hasNext() && count < 40){
                    envelope.piggybackedAcks.add(it.next());
                    it.remove();
                    count++;
                }
                if (count > 0){
                    System.out.println("        [Reliability " + myReplicaId + "] Piggybacked " + count + " ACKs to replica " + targetId);

                }
            
            
            }

            conn.send(envelope);
            unackedMessages.computeIfAbsent(targetId, k->new ConcurrentHashMap<>()).put(seq, envelope);
            System.out.println("        [Reliability " + myReplicaId + "] Sent seq=" + seq + " to replica " + targetId);
        } catch (IOException e) {
            System.err.println("       [Reliability " + myReplicaId + "] Error sending message: " + e.getMessage());
            e.printStackTrace();
        }
        
    }

    public void handleIncomingMessage(V2VMessageEnvelope envelope){
        System.out.println("    [Reliability " + myReplicaId + "] handleIncomingMessage() called");
        System.out.println("        Type: " + envelope.type);
        System.out.println("        From: " + envelope.fromReplicaId);
        System.out.println("        SeqNum: " + envelope.sequenceNum);

        if (envelope.piggybackedAcks != null && !envelope.piggybackedAcks.isEmpty()){
            CompletableFuture.runAsync(() -> {
                for (Long ackedSeq : envelope.piggybackedAcks){
                    processAck(envelope.fromReplicaId, ackedSeq);
                }
            });
        
        
        }
        
        if (envelope.type == V2VMessageEnvelope.MessageType.ACK){
            System.out.println("        -> Processing ACK");
            CompletableFuture.runAsync(() -> handleAck(envelope));            
            return;
        }

        int senderId = envelope.fromReplicaId;
        long seq = envelope.sequenceNum;
        long expected = expectedSeqNums.getOrDefault(senderId, 0L);
        System.out.println("        Expected seq: " + expected);

        pendingAcks.computeIfAbsent(senderId, k -> ConcurrentHashMap.newKeySet()).add(seq);

        // CompletableFuture.runAsync(() -> {
        //     sendAck(senderId, seq);
        // });

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

    private void processAck(int from, long ackNum) {
        ConcurrentHashMap<Long, V2VMessageEnvelope> pending = unackedMessages.get(from);
        if (pending != null) {
            if (pending.remove(ackNum) != null) {
                // Only log if we actually removed something (deduplicate logs)
                // System.out.println("[Reliability " + myReplicaId + "] ACK confirmed for seq=" + ackNum);
            }
            // Clean up retransmission attempt counter
            ConcurrentHashMap<Long, Integer> targetAttempts = retransmissionAttempts.get(from);
            if (targetAttempts != null) {
                targetAttempts.remove(ackNum);
            }
        }
    }




    private void deliverMessage(V2VMessageEnvelope envelope){
        System.out.println("    [Reliability " + myReplicaId + "] deliverMessage() - deserializing payload");
        try{
            ByteArrayInputStream bais = new ByteArrayInputStream(envelope.serializedMessage);
            ObjectInputStream ois = new ObjectInputStream(bais);
            SystemMessage message = (SystemMessage) ois.readObject();
            System.out.println("        Deserialized: " + message.getClass().getSimpleName());
            System.out.println("        DEBUG: envelope.fromReplicaId=" + envelope.fromReplicaId +
                             ", message.getSender()=" + message.getSender());

            // CRITICAL DEBUG: Check if sender mismatch
            if (envelope.fromReplicaId != message.getSender()) {
                System.err.println("        *** SENDER MISMATCH! envelope says " + envelope.fromReplicaId +
                                 " but message says " + message.getSender() + " ***");
            }

            message.authenticated = true;

            parent.deliverToBFTSmart(message);

        }catch (Exception e){
            System.err.println("         [Reliability " + myReplicaId + "] Error delivering message: " + e.getMessage());
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
        V2VNativeReplicaConnection conn = parent.replicaConnections.get(targetId);
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

            long now = SimulationClock.currentTimeMillis();
            List<Long> toRemove = new ArrayList<>();

            for (V2VMessageEnvelope envelope: pending.values()){
                // Add jitter: RETX_TIMEOUT_MS ± 30% random variation
                // This spreads out retransmissions to reduce collision storms
                long jitter = (long) (RETX_TIMEOUT_MS * 0.3 * jitterRandom.nextDouble());
                long timeoutWithJitter = RETX_TIMEOUT_MS + jitter - (long)(RETX_TIMEOUT_MS * 0.15); // Center around RETX_TIMEOUT_MS
                envelope.currentTimeout = timeoutWithJitter;
                if (now - envelope.timestampMs > envelope.currentTimeout){
                    System.out.println("[Reliability] Resending seq=" + envelope.sequenceNum + " because " + (now - envelope.timestampMs) + "ms (sim time) passed.");
                    // Get or initialize attempt count for this message
                    envelope.currentTimeout = Math.min(envelope.currentTimeout * 2, 8000);
                    ConcurrentHashMap<Long, Integer> targetAttempts = retransmissionAttempts.computeIfAbsent(targetId, k -> new ConcurrentHashMap<>());
                    int attempts = targetAttempts.getOrDefault(envelope.sequenceNum, 0);

                    if (attempts >= MAX_RETX_ATTEMPTS) {
                        // Max retransmissions reached, give up on this message
                        System.err.println("[Reliability " + myReplicaId + "] Max retransmission attempts (" + MAX_RETX_ATTEMPTS + ") reached for seq=" + envelope.sequenceNum + " to replica " + targetId + ", dropping message");
                        toRemove.add(envelope.sequenceNum);
                        targetAttempts.remove(envelope.sequenceNum);
                    } else {
                        // Retransmit with timestamp update for next jitter calculation
                        V2VNativeReplicaConnection conn = parent.replicaConnections.get(targetId);
                        if (conn != null){
                            System.out.println("[Reliability " + myReplicaId + "] Retransmitting seq=" + envelope.sequenceNum + " to replica " + targetId + " (attempt " + (attempts + 1) + "/" + MAX_RETX_ATTEMPTS + " with jitter=" + jitter + "ms)");
                            
                            envelope.timestampMs = now; // Update timestamp for next retry jitter
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

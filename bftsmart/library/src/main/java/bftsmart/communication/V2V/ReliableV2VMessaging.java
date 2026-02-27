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

    private static final ConcurrentHashMap<Integer, ReliableV2VMessaging> instances = new ConcurrentHashMap<>();

    // Queue for retransmissions to avoid JNI reentrant calls (serialized as byte arrays)
    private final List<byte[]> pendingRetransmissions = Collections.synchronizedList(new ArrayList<>());

    private final ConcurrentHashMap<Integer, Set<Long>> pendingAcks = new ConcurrentHashMap<>();

    private final ScheduledExecutorService retxScheduler = Executors.newScheduledThreadPool(1);

    private static final long RETX_TIMEOUT_MS = 1000; //retry after 500ms

    private static final int MAX_RETX_ATTEMPTS = 20;

    

    // Add jitter to reduce collision probability during retransmissions
    private final java.util.Random jitterRandom = new java.util.Random();

    public ReliableV2VMessaging(int myReplicaId, V2VServersCommunicationLayer parent) {
        this.myReplicaId = myReplicaId;
        this.parent = parent;
        instances.put(myReplicaId, this);
        // DISABLED: Retransmissions handled by C++ retxCheckTimer using simulation time
        // Java's ScheduledExecutorService uses real-time which is incompatible with simulation time
        // retxScheduler.scheduleAtFixedRate(this::checkRetransmissions, RETX_TIMEOUT_MS, RETX_TIMEOUT_MS, TimeUnit.MILLISECONDS);
        System.out.println("[Reliability " + myReplicaId + "] Java scheduler DISABLED - C++ handles retransmissions with sim-time");


    }

    public static void checkRetransmissionsForAllReplicas() {
        for (ReliableV2VMessaging instance : instances.values()) {
            List<V2VMessageEnvelope> toRetransmit = instance.checkRetransmissions();
            // IMPORTANT: Serialize here, then C++ sends directly (avoids JNI reentrant deadlock!)
            for (V2VMessageEnvelope envelope : toRetransmit) {
                try {
                    ByteArrayOutputStream baos = new ByteArrayOutputStream();
                    ObjectOutputStream oos = new ObjectOutputStream(baos);
                    oos.writeObject(envelope);
                    oos.flush();
                    byte[] data = baos.toByteArray();
                    instance.pendingRetransmissions.add(data);
                } catch (IOException e) {
                    System.err.println("[Reliability " + instance.myReplicaId + "] ERROR serializing retransmission: " + e.getMessage());
                }
            }
        }
    }

    /**
     * Get and clear pending retransmissions for a specific replica (as serialized byte arrays).
     * Called by C++ after checkRetransmissionsForAllReplicas() returns.
     */
    public static byte[][] getPendingRetransmissionsForReplica(int replicaId) {
        ReliableV2VMessaging instance = instances.get(replicaId);
        if (instance == null) {
            return new byte[0][];
        }
        synchronized (instance.pendingRetransmissions) {
            byte[][] result = instance.pendingRetransmissions.toArray(new byte[0][]);
            instance.pendingRetransmissions.clear();
            System.out.println("[Reliability " + replicaId + "] Returning " + result.length + " pending retransmissions to C++");
            return result;
        }
    }

    public void sendMulticast(int[] targetIds, SystemMessage message, V2VNativeReplicaConnection conn){
        System.out.println("    [Reliability " + myReplicaId + "] sendMulticast() to " + targetIds.length + " targets");
        System.out.println("        Message class: " + message.getClass().getSimpleName());
        System.out.flush();

        try{
            System.out.println("        Serializing SystemMessage...");
            ByteArrayOutputStream baos = new ByteArrayOutputStream();
            ObjectOutputStream oos = new ObjectOutputStream(baos);
            oos.writeObject(message);
            oos.flush();
            byte[] payload = baos.toByteArray();
            System.out.println("        Payload size: " + payload.length + " bytes");
            System.out.flush();

            int firstTarget = -1;
            long firstSeq = -1;
            V2VMessageEnvelope broadcastEnvelope = null;


            for (int targetId: targetIds){
                if (targetId == myReplicaId) continue; 
                long seq = sendSeqNums.computeIfAbsent(targetId, k->0L);
                sendSeqNums.put(targetId, seq + 1);
                V2VMessageEnvelope envelope = new V2VMessageEnvelope(
                    V2VMessageEnvelope.MessageType.DATA,
                    myReplicaId,
                    targetId,
                    seq,
                    0L,
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
                }

                unackedMessages.computeIfAbsent(targetId, k->new ConcurrentHashMap<>()).put(seq, envelope);

                if (firstTarget == -1){
                    firstTarget = targetId;
                    firstSeq = seq;
                    broadcastEnvelope = envelope;
                }
                System.out.println("        -> Registered seq=" + seq + " for target " + targetId);
            }

            if (broadcastEnvelope != null) {
                System.out.println("        -> Calling conn.send(envelope) for broadcast...");
                System.out.flush();
                conn.send(broadcastEnvelope);
                System.out.println("    [Reliability] Broadcasted (physical: 1 transmission, logical: "
                    + targetIds.length + " targets, first_seq=" + firstSeq + ")");
                System.out.flush();
            } else {
                System.err.println("    [Reliability] ERROR: broadcastEnvelope is null!");
                System.err.flush();
            }

        } catch (IOException e) {
            System.err.println("    [Reliability " + myReplicaId + "] Error sending multicast: " + e.getMessage());
            e.printStackTrace();
        }

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
            // CompletableFuture.runAsync(() -> {
            //     for (Long ackedSeq : envelope.piggybackedAcks){
            //         processAck(envelope.fromReplicaId, ackedSeq);
            //     }
            // });
            for (Long ackedSeq : envelope.piggybackedAcks){
                processAck(envelope.fromReplicaId, ackedSeq);
            }
        
        
        }
        
        if (envelope.type == V2VMessageEnvelope.MessageType.ACK){
            System.out.println("        -> Processing ACK");
            handleAck(envelope);
            return;
        }

        int senderId = envelope.fromReplicaId;
        long seq = envelope.sequenceNum;
        long expected = expectedSeqNums.getOrDefault(senderId, 0L);
        System.out.println("        Expected seq: " + expected);

        pendingAcks.computeIfAbsent(senderId, k -> ConcurrentHashMap.newKeySet()).add(seq);

        // CompletableFuture.runAsync(() -> {
         //   sendAck(senderId, seq);
        // });
        // sendAck(senderId, seq);

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

    public List<V2VMessageEnvelope> checkRetransmissions(){
        long now = SimulationClock.currentTimeMillis();
        System.out.println("[Reliability " + myReplicaId + "] checkRetransmissions() at t=" + now + "ms, unackedMessages size=" + unackedMessages.size());
        List<V2VMessageEnvelope> toRetransmit = new ArrayList<>();

        for (Map.Entry<Integer, ConcurrentHashMap<Long, V2VMessageEnvelope>> entry: unackedMessages.entrySet()){
            int targetId = entry.getKey();
            ConcurrentHashMap<Long, V2VMessageEnvelope> pending = entry.getValue();

           // System.out.println("[Reliability " + myReplicaId + "] Checking " + pending.size() + " pending messages to replica " + targetId);
            List<Long> toRemove = new ArrayList<>();

            for (V2VMessageEnvelope envelope: pending.values()){
                // Add jitter: RETX_TIMEOUT_MS + 30% random variation
                // This spreads out retransmissions to reduce collision storms
                long jitter = (long) (RETX_TIMEOUT_MS * 0.3 * jitterRandom.nextDouble());
                long timeoutWithJitter = RETX_TIMEOUT_MS + jitter - (long)(RETX_TIMEOUT_MS * 0.15); // Center around RETX_TIMEOUT_MS
                envelope.currentTimeout = timeoutWithJitter;

                long timeSince = now - envelope.timestampMs;
                System.out.println("[Reliability " + myReplicaId + "] seq=" + envelope.sequenceNum + " to " + targetId + ": timeSince=" + timeSince + "ms, timeout=" + envelope.currentTimeout + "ms");

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
                        // Queue for retransmission (don't send directly - causes JNI reentrant deadlock!)
                        System.out.println("[Reliability " + myReplicaId + "] Queuing retransmit seq=" + envelope.sequenceNum + " to replica " + targetId + " (attempt " + (attempts + 1) + "/" + MAX_RETX_ATTEMPTS + " with jitter=" + jitter + "ms)");

                        envelope.timestampMs = now; // Update timestamp for next retry jitter
                        toRetransmit.add(envelope);
                        targetAttempts.put(envelope.sequenceNum, attempts + 1);
                    }
                }
            }
            
            // Remove messages that exceeded max attempts
            for (Long seq : toRemove) {
                pending.remove(seq);
            }
        }
        return toRetransmit;
    }

}

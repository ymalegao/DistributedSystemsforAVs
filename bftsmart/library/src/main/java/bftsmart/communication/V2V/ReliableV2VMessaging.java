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
    // Broadcast/multicast path: one sender-wide sequence space, ordered/deduped
    // separately from unicast.
    private final java.util.concurrent.atomic.AtomicLong broadcastSeqNum = new java.util.concurrent.atomic.AtomicLong(
            0);
    private static final long BROADCAST_SEQ_FLAG = (1L << 62);
    private static final long BROADCAST_SEQ_MASK = ~BROADCAST_SEQ_FLAG;
    private final ConcurrentHashMap<Integer, Long> expectedBroadcastSeqNums = new ConcurrentHashMap<>();
    private final ConcurrentHashMap<Integer, PriorityQueue<V2VMessageEnvelope>> broadcastReceiveBuffers = new ConcurrentHashMap<>();

    private final ConcurrentHashMap<Integer, ConcurrentHashMap<Long, V2VMessageEnvelope>> unackedMessages = new ConcurrentHashMap<>();

    private final ConcurrentHashMap<Integer, ConcurrentHashMap<Long, Integer>> retransmissionAttempts = new ConcurrentHashMap<>();

    private static final ConcurrentHashMap<Integer, ReliableV2VMessaging> instances = new ConcurrentHashMap<>();

    // Queue for retransmissions to avoid JNI reentrant calls (serialized as byte
    // arrays)
    private final List<byte[]> pendingRetransmissions = Collections.synchronizedList(new ArrayList<>());

    private final ConcurrentHashMap<Integer, Set<Long>> pendingAcks = new ConcurrentHashMap<>();

    private final ScheduledExecutorService retxScheduler = Executors.newScheduledThreadPool(1);

    private static final long RETX_TIMEOUT_MS = 12;

    private static final int MAX_RETX_ATTEMPTS = 20;

    // Add jitter to reduce collision probability during retransmissions
    private final java.util.Random jitterRandom = new java.util.Random();

    public ReliableV2VMessaging(int myReplicaId, V2VServersCommunicationLayer parent) {
        this.myReplicaId = myReplicaId;
        this.parent = parent;
        instances.put(myReplicaId, this);
        // DISABLED: Retransmissions handled by C++ retxCheckTimer using simulation time
        // Java's ScheduledExecutorService uses real-time which is incompatible with
        // simulation time
        // retxScheduler.scheduleAtFixedRate(this::checkRetransmissions,
        // RETX_TIMEOUT_MS, RETX_TIMEOUT_MS, TimeUnit.MILLISECONDS);
        System.out.println("[Reliability " + myReplicaId
                + "] Java scheduler DISABLED - C++ handles retransmissions with sim-time");

    }

    /**
     * Check retransmissions for a single replica only.
     * H2 fix: replaces checkRetransmissionsForAllReplicas() to eliminate N-fold
     * amplification.
     * Each C++ replica calls this for itself only, not for all replicas.
     */
    public static void checkRetransmissionsForReplica(int replicaId) {
        ReliableV2VMessaging instance = instances.get(replicaId);
        if (instance == null)
            return;
        List<V2VMessageEnvelope> toRetransmit = instance.checkRetransmissions();
        for (V2VMessageEnvelope envelope : toRetransmit) {
            try {
                ByteArrayOutputStream baos = new ByteArrayOutputStream();
                ObjectOutputStream oos = new ObjectOutputStream(baos);
                oos.writeObject(envelope);
                oos.flush();
                instance.pendingRetransmissions.add(baos.toByteArray());
            } catch (IOException e) {
                System.err
                        .println("[Reliability " + replicaId + "] ERROR serializing retransmission: " + e.getMessage());
            }
        }
    }

    // Kept for compatibility but should not be used — causes N-fold amplification
    public static void checkRetransmissionsForAllReplicas() {
        for (ReliableV2VMessaging instance : instances.values()) {
            List<V2VMessageEnvelope> toRetransmit = instance.checkRetransmissions();
            for (V2VMessageEnvelope envelope : toRetransmit) {
                try {
                    ByteArrayOutputStream baos = new ByteArrayOutputStream();
                    ObjectOutputStream oos = new ObjectOutputStream(baos);
                    oos.writeObject(envelope);
                    oos.flush();
                    instance.pendingRetransmissions.add(baos.toByteArray());
                } catch (IOException e) {
                    System.err.println("[Reliability " + instance.myReplicaId + "] ERROR serializing retransmission: "
                            + e.getMessage());
                }
            }
        }
    }

    /**
     * Clear all unacked message queues for a specific replica.
     * Called from C++ at epoch boundaries to prevent cross-epoch retransmit
     * pollution (H1).
     */
    public static void clearUnackedForReplica(int replicaId) {
        ReliableV2VMessaging instance = instances.get(replicaId);
        if (instance != null) {
            int cleared = 0;
            for (ConcurrentHashMap<Long, V2VMessageEnvelope> map : instance.unackedMessages.values()) {
                cleared += map.size();
                map.clear();
            }
            instance.retransmissionAttempts.clear();
            instance.pendingRetransmissions.clear();
            System.out
                    .println("[Reliability " + replicaId + "] Epoch boundary: cleared " + cleared + " unacked entries");
        }
    }

    /**
     * Global epoch reset across all live reliability instances.
     * This intentionally drops cross-epoch carry-over in-flight state.
     *
     * IMPORTANT: sequence/high-watermark state is intentionally preserved.
     * Resetting send/expected sequence numbers to 0 while sockets are still alive
     * can
     * cause old in-flight packets to be misclassified, resulting in stalls or
     * replays.
     */
    public static void globalResetV2V(int[] departedReplicas) {
        if (departedReplicas != null) {
            for (int depId : departedReplicas) {
                removeInstance(depId);
                clearUnackedToReplica(depId);
            }
        }

        int instanceCount = 0;
        int totalUnackedCleared = 0;
        int totalRxBufferedCleared = 0;
        int totalBroadcastRxBufferedCleared = 0;

        for (ReliableV2VMessaging instance : instances.values()) {
            instanceCount++;

            for (ConcurrentHashMap<Long, V2VMessageEnvelope> map : instance.unackedMessages.values()) {
                totalUnackedCleared += map.size();
                map.clear();
            }
            for (PriorityQueue<V2VMessageEnvelope> buffer : instance.recieveBuffers.values()) {
                totalRxBufferedCleared += buffer.size();
                buffer.clear();
            }
            for (PriorityQueue<V2VMessageEnvelope> buffer : instance.broadcastReceiveBuffers.values()) {
                totalBroadcastRxBufferedCleared += buffer.size();
                buffer.clear();
            }

            instance.unackedMessages.clear();
            instance.recieveBuffers.clear();
            instance.broadcastReceiveBuffers.clear();
            instance.retransmissionAttempts.clear();
            instance.pendingAcks.clear();
            instance.pendingRetransmissions.clear();
        }

        System.out.println("[Reliability] globalResetV2V: instances=" + instanceCount
                + ", unackedCleared=" + totalUnackedCleared
                + ", rxBufferedCleared=" + totalRxBufferedCleared
                + ", broadcastRxBufferedCleared=" + totalBroadcastRxBufferedCleared);
    }

    /**
     * Purge a departed replica's reliability instance and all cross-references.
     * This prevents zombie instances from accumulating across reconfig epochs.
     */
    public static void removeInstance(int replicaId) {
        ReliableV2VMessaging removed = instances.remove(replicaId);
        if (removed != null) {
            removed.unackedMessages.clear();
            removed.recieveBuffers.clear();
            removed.broadcastReceiveBuffers.clear();
            removed.retransmissionAttempts.clear();
            removed.pendingAcks.clear();
            removed.pendingRetransmissions.clear();
            removed.sendSeqNums.clear();
            removed.expectedSeqNums.clear();
            removed.expectedBroadcastSeqNums.clear();
            removed.broadcastSeqNum.set(0L);
        }

        int referencesCleared = 0;
        for (ReliableV2VMessaging instance : instances.values()) {
            if (instance.unackedMessages.remove(replicaId) != null) {
                referencesCleared++;
            }
            if (instance.retransmissionAttempts.remove(replicaId) != null) {
                referencesCleared++;
            }
            if (instance.recieveBuffers.remove(replicaId) != null) {
                referencesCleared++;
            }
            if (instance.broadcastReceiveBuffers.remove(replicaId) != null) {
                referencesCleared++;
            }
            if (instance.pendingAcks.remove(replicaId) != null) {
                referencesCleared++;
            }
            if (instance.sendSeqNums.remove(replicaId) != null) {
                referencesCleared++;
            }
            // CRITICAL FIX: DO NOT remove expectedSeqNums or expectedBroadcastSeqNums.
            // If we remove them, followers forget the sequence state of the departed car.
            // Any trailing messages still arriving over the air will have seq > 0, but the
            // follower will now expect 0, causing them to be buffered indefinitely and
            // breaking the final phase of consensus for the followers!
            // if (instance.expectedSeqNums.remove(replicaId) != null) {
            // referencesCleared++;
            // }
            // if (instance.expectedBroadcastSeqNums.remove(replicaId) != null) {
            // referencesCleared++;
            // }
        }

        System.out.println("[Reliability] removeInstance(" + replicaId + "): instanceRemoved=" + (removed != null)
                + ", crossReferencesCleared=" + referencesCleared
                + ", remainingInstances=" + instances.size());
    }

    /**
     * Full epoch-boundary cleanup for a replica.
     *
     * Bug: clearUnackedForReplica() drops in-flight messages before their ACK
     * arrives.
     * The receivers' expectedBroadcastSeqNums/expectedSeqNums are then stuck
     * waiting
     * for a sequence number that will never come, blocking all future messages from
     * that sender (H2).
     *
     * Fix: after clearing the sender's outgoing state, fast-forward every other
     * replica's receive expectation for messages FROM replicaId to match the
     * sender's current sequence position, and discard any stale buffered messages.
     * This is safe because ORDER consensus has already decided — all needed
     * messages
     * were delivered to a quorum. Any remaining in-flight messages are redundant.
     */
    public static void epochBoundaryCleanupForReplica(int replicaId) {
        ReliableV2VMessaging sender = instances.get(replicaId);
        if (sender == null)
            return;

        // 1. Clear sender outgoing state (same as clearUnackedForReplica)
        int cleared = 0;
        for (ConcurrentHashMap<Long, V2VMessageEnvelope> map : sender.unackedMessages.values()) {
            cleared += map.size();
            map.clear();
        }
        sender.retransmissionAttempts.clear();
        sender.pendingRetransmissions.clear();
        System.out.println("[Reliability " + replicaId + "] Epoch boundary: cleared " + cleared + " unacked entries");

        // 2. Fast-forward all receivers' expectations for messages FROM replicaId.
        // ADVANCE-ONLY: only increase the expected seq, never rewind it.
        // A receiver that has already advanced past the sender's current position
        // (received messages in-order) must not be rewound — that would cause it
        // to re-accept old messages it already processed.
        // A receiver that is stuck below the sender's current position has an
        // unreachable gap; advancing it unblocks future epoch messages.
        long currentBroadcastIdx = sender.broadcastSeqNum.get();
        for (ReliableV2VMessaging receiver : instances.values()) {
            if (receiver.myReplicaId == replicaId)
                continue;
            // Broadcast: advance only
            long currentExpected = receiver.expectedBroadcastSeqNums.getOrDefault(replicaId, 0L);
            if (currentBroadcastIdx > currentExpected) {
                receiver.expectedBroadcastSeqNums.put(replicaId, currentBroadcastIdx);
                receiver.broadcastReceiveBuffers.remove(replicaId);
            }
            // Unicast: advance only
            Long unicastNext = sender.sendSeqNums.get(receiver.myReplicaId);
            if (unicastNext != null) {
                long currentUnicastExpected = receiver.expectedSeqNums.getOrDefault(replicaId, 0L);
                if (unicastNext > currentUnicastExpected) {
                    receiver.expectedSeqNums.put(replicaId, unicastNext);
                    receiver.recieveBuffers.remove(replicaId);
                }
            }
        }
        System.out.println("[Reliability " + replicaId + "] Epoch boundary: advance-only sync to broadcastIdx="
                + currentBroadcastIdx);
    }

    /**
     * Remove all unacked entries that were sent TO departedId across every live
     * instance.
     * Called from the reconfig block when a replica departs so that no live replica
     * keeps retransmitting messages to a zombie node (those ACKs will never
     * arrive).
     */
    public static void clearUnackedToReplica(int departedId) {
        int total = 0;
        for (ReliableV2VMessaging instance : instances.values()) {
            ConcurrentHashMap<Long, V2VMessageEnvelope> bucket = instance.unackedMessages.remove(departedId);
            if (bucket != null) {
                total += bucket.size();
            }
            ConcurrentHashMap<Long, Integer> attempts = instance.retransmissionAttempts.remove(departedId);
            // pendingRetransmissions contains serialized envelopes; filter out ones
            // addressed to departedId
            synchronized (instance.pendingRetransmissions) {
                instance.pendingRetransmissions.removeIf(bytes -> {
                    try {
                        java.io.ObjectInputStream ois = new java.io.ObjectInputStream(
                                new java.io.ByteArrayInputStream(bytes));
                        V2VMessageEnvelope env = (V2VMessageEnvelope) ois.readObject();
                        return env.toReplicaId == departedId;
                    } catch (Exception e) {
                        return false;
                    }
                });
            }
        }
        System.out.println("[Reliability] clearUnackedToReplica(" + departedId + "): removed " + total
                + " unacked entries across all instances");
    }

    /**
     * Get and clear pending retransmissions for a specific replica (as serialized
     * byte arrays).
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
            System.out.println(
                    "[Reliability " + replicaId + "] Returning " + result.length + " pending retransmissions to C++");
            return result;
        }
    }

    public void sendMulticast(int[] targetIds, SystemMessage message, V2VNativeReplicaConnection conn) {
        System.out.println("    [Reliability " + myReplicaId + "] sendMulticast() to " + targetIds.length + " targets");
        System.out.println("        Message class: " + message.getClass().getSimpleName());
        System.out.flush();

        try {
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

            long sendTimeMs = SimulationClock.currentTimeMillis();
            // One seq for the whole physical broadcast. Flagged to avoid collisions with
            // per-target unicast seqs.
            long broadcastIdx = broadcastSeqNum.getAndIncrement();
            long broadcastSeq = BROADCAST_SEQ_FLAG | (broadcastIdx & BROADCAST_SEQ_MASK);
            for (int targetId : targetIds) {
                if (targetId == myReplicaId)
                    continue;
                    
                // Fix for dynamic replica counts: do not expect ACKs from departed replicas
                if (!instances.containsKey(targetId)) {
                    continue;
                }
                
                V2VMessageEnvelope envelope = new V2VMessageEnvelope(
                        V2VMessageEnvelope.MessageType.DATA,
                        myReplicaId,
                        targetId,
                        broadcastSeq,
                        0L,
                        payload);
                envelope.isBroadcast = true;
                envelope.timestampMs = sendTimeMs;
                envelope.currentTimeout = RETX_TIMEOUT_MS;
                // NOTE: Do not use per-target ACK piggybacking here; only one envelope is
                // physically sent.
                unackedMessages.computeIfAbsent(targetId, k -> new ConcurrentHashMap<>()).put(broadcastSeq, envelope);

                if (firstTarget == -1) {
                    firstTarget = targetId;
                    firstSeq = broadcastSeq;
                    broadcastEnvelope = envelope;
                }
                System.out.println("        -> Registered broadcastSeq=" + broadcastSeq + " for target " + targetId);
            }

            if (broadcastEnvelope != null) {
                // Attach ALL pending ACKs (tagged by original sender) to the one physical
                // broadcast.
                for (Map.Entry<Integer, Set<Long>> entry : pendingAcks.entrySet()) {
                    Set<Long> acks = entry.getValue();
                    if (acks != null && !acks.isEmpty()) {
                        broadcastEnvelope.broadcastAcks.put(entry.getKey(), new ArrayList<>(acks));
                        acks.clear();
                    }
                }
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

    public void sendReliable(int targetId, SystemMessage message, V2VNativeReplicaConnection conn) {
        if (!instances.containsKey(targetId)) {
            System.out.println("    [Reliability " + myReplicaId + "] sendReliable() called for departed target " + targetId + " - ignoring");
            return;
        }
        
        System.out.println("    [Reliability " + myReplicaId + "] sendReliable() called");
        System.out.println("        Target: " + targetId);
        System.out.println("        Message type: " + message.getClass().getSimpleName());

        try {
            // get next sequence number for this destination
            long seq = sendSeqNums.computeIfAbsent(targetId, k -> 0L); // explain this line later
            sendSeqNums.put(targetId, seq + 1);
            System.out.println("        Sequence number: " + seq);

            // serialize the message
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
                    ackNum,
                    payload);
            envelope.timestampMs = SimulationClock.currentTimeMillis();
            envelope.currentTimeout = RETX_TIMEOUT_MS;

            Set<Long> acksForTarget = pendingAcks.get(targetId);
            if (acksForTarget != null && !acksForTarget.isEmpty()) {
                Iterator<Long> it = acksForTarget.iterator();
                int count = 0;
                while (it.hasNext() && count < 40) {
                    envelope.piggybackedAcks.add(it.next());
                    it.remove();
                    count++;
                }
                if (count > 0) {
                    System.out.println("        [Reliability " + myReplicaId + "] Piggybacked " + count
                            + " ACKs to replica " + targetId);

                }

            }

            conn.send(envelope);
            unackedMessages.computeIfAbsent(targetId, k -> new ConcurrentHashMap<>()).put(seq, envelope);
            System.out.println("        [Reliability " + myReplicaId + "] Sent seq=" + seq + " to replica " + targetId);
        } catch (IOException e) {
            System.err.println("       [Reliability " + myReplicaId + "] Error sending message: " + e.getMessage());
            e.printStackTrace();
        }

    }

    public void handleIncomingMessage(V2VMessageEnvelope envelope) {
        System.out.println("    [Reliability " + myReplicaId + "] handleIncomingMessage() called");
        System.out.println("        Type: " + envelope.type);
        System.out.println("        From: " + envelope.fromReplicaId);
        System.out.println("        SeqNum: " + envelope.sequenceNum);

        if (envelope.piggybackedAcks != null && !envelope.piggybackedAcks.isEmpty()) {
            for (Long ackedSeq : envelope.piggybackedAcks) {
                processAck(envelope.fromReplicaId, ackedSeq);
            }
        }

        // For true broadcasts, ACKs are tagged by original sender id.
        if (envelope.broadcastAcks != null && !envelope.broadcastAcks.isEmpty()) {
            ArrayList<Long> myAcks = envelope.broadcastAcks.get(myReplicaId);
            if (myAcks != null) {
                for (Long ackedSeq : myAcks) {
                    // pendingAcks now stores broadcast ACKs WITH the BROADCAST_SEQ_FLAG already
                    // set (fixed), so the flag application below is idempotent but kept for
                    // safety: FLAG | ((FLAG|idx) & ~FLAG) == FLAG|idx.
                    long ackKey = (envelope.isBroadcast || ((envelope.sequenceNum & BROADCAST_SEQ_FLAG) != 0))
                            ? (BROADCAST_SEQ_FLAG | (ackedSeq & BROADCAST_SEQ_MASK))
                            : ackedSeq;
                    processAck(envelope.fromReplicaId, ackKey);
                }
            }
        }

        if (envelope.type == V2VMessageEnvelope.MessageType.ACK) {
            System.out.println("        -> Processing ACK");
            handleAck(envelope);
            return;
        }

        int senderId = envelope.fromReplicaId;
        long seq = envelope.sequenceNum;
        final boolean isBroadcast = envelope.isBroadcast || ((seq & BROADCAST_SEQ_FLAG) != 0);
        long expected;
        long seqForOrdering;
        if (isBroadcast) {
            seqForOrdering = (seq & BROADCAST_SEQ_MASK);
            expected = expectedBroadcastSeqNums.getOrDefault(senderId, 0L);
            System.out.println("        Expected broadcast seq: " + expected);
        } else {
            seqForOrdering = seq;
            expected = expectedSeqNums.getOrDefault(senderId, 0L);
            System.out.println("        Expected seq: " + expected);
        }

        // Store ACKs using the SAME key that the sender used in unackedMessages:
        //   broadcasts: BROADCAST_SEQ_FLAG | idx  (flag MUST be present so that
        //     processAck() finds the entry whether the ACK travels via broadcastAcks
        //     or via piggybackedAcks on a later unicast message).
        //   unicasts: plain per-target seq (no flag).
        // The broadcastAcks processing path re-applies the flag anyway (idempotent),
        // but the piggybackedAcks path passes the value straight to processAck, so
        // the flag must already be set here or the unackedMessages.remove() silently
        // fails and the broadcast is retransmitted unnecessarily.
        long ackStoreKey = isBroadcast ? (BROADCAST_SEQ_FLAG | seqForOrdering) : seqForOrdering;
        pendingAcks.computeIfAbsent(senderId, k -> ConcurrentHashMap.newKeySet()).add(ackStoreKey);

        // CompletableFuture.runAsync(() -> {
        // sendAck(senderId, seq);
        // });
        // sendAck(senderId, seq);

        if (seqForOrdering < expected) {
            // duplcate ignore
            System.out.println("        -> DUPLICATE (seq < expected), ignoring");
            return;
        }

        if (seqForOrdering == expected) {
            System.out.println("        -> IN ORDER, delivering immediately");
            deliverMessage(envelope);
            if (isBroadcast) {
                expectedBroadcastSeqNums.put(senderId, expected + 1);
                drainBroadcastReceiveBuffer(senderId);
            } else {
                expectedSeqNums.put(senderId, expected + 1);
                drainReceiveBuffer(senderId);
            }

        } else {
            System.out.println("        -> OUT OF ORDER (seq=" + seq + ", expected=" + expected + "), buffering");
            if (isBroadcast) {
                broadcastReceiveBuffers
                        .computeIfAbsent(senderId, k -> new PriorityQueue<>(
                                Comparator.comparingLong(e -> (e.sequenceNum & BROADCAST_SEQ_MASK))))
                        .add(envelope);
            } else {
                recieveBuffers
                        .computeIfAbsent(senderId,
                                k -> new PriorityQueue<>(Comparator.comparingLong(e -> e.sequenceNum)))
                        .add(envelope); // need to explain this line later as well
            }

        }

    }

    private void processAck(int from, long ackNum) {
        ConcurrentHashMap<Long, V2VMessageEnvelope> pending = unackedMessages.get(from);
        if (pending != null) {
            if (pending.remove(ackNum) != null) {
                // Only log if we actually removed something (deduplicate logs)
                // System.out.println("[Reliability " + myReplicaId + "] ACK confirmed for seq="
                // + ackNum);
            }
            // Clean up retransmission attempt counter
            ConcurrentHashMap<Long, Integer> targetAttempts = retransmissionAttempts.get(from);
            if (targetAttempts != null) {
                targetAttempts.remove(ackNum);
            }
        }
    }

    private void deliverMessage(V2VMessageEnvelope envelope) {
        System.out.println("    [Reliability " + myReplicaId + "] deliverMessage() - deserializing payload");
        try {
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

        } catch (Exception e) {
            System.err
                    .println("         [Reliability " + myReplicaId + "] Error delivering message: " + e.getMessage());
            e.printStackTrace();
        }
    }

    private void drainReceiveBuffer(int senderId) {
        PriorityQueue<V2VMessageEnvelope> buffer = recieveBuffers.get(senderId);
        if (buffer == null)
            return;

        long expected = expectedSeqNums.get(senderId);

        while (!buffer.isEmpty() && buffer.peek().sequenceNum == expected) {
            deliverMessage(buffer.poll());
            expected++;
            expectedSeqNums.put(senderId, expected);
        }
    }

    private void drainBroadcastReceiveBuffer(int senderId) {
        PriorityQueue<V2VMessageEnvelope> buffer = broadcastReceiveBuffers.get(senderId);
        if (buffer == null)
            return;

        long expected = expectedBroadcastSeqNums.getOrDefault(senderId, 0L);
        while (!buffer.isEmpty() && ((buffer.peek().sequenceNum & BROADCAST_SEQ_MASK) == expected)) {
            deliverMessage(buffer.poll());
            expected++;
            expectedBroadcastSeqNums.put(senderId, expected);
        }
    }

    private void handleAck(V2VMessageEnvelope ack) {
        int from = ack.fromReplicaId;
        long ackNum = ack.ackNum;

        ConcurrentHashMap<Long, V2VMessageEnvelope> pending = unackedMessages.get(from);
        if (pending != null) {
            pending.remove(ackNum);
            System.out.println(
                    "[Reliability " + myReplicaId + "] Received ack for seq=" + ackNum + " from replica " + from);

            // Clean up retransmission attempt counter
            ConcurrentHashMap<Long, Integer> targetAttempts = retransmissionAttempts.get(from);
            if (targetAttempts != null) {
                targetAttempts.remove(ackNum);
            }
        }
    }

    private void sendAck(int targetId, long ackNum) {
        V2VNativeReplicaConnection conn = parent.replicaConnections.get(targetId);
        if (conn != null) {
            V2VMessageEnvelope ack = V2VMessageEnvelope.createAck(myReplicaId, targetId, ackNum);
            conn.send(ack);
            System.out.println(
                    "[Reliability " + myReplicaId + "] Sent ack for seq=" + ackNum + " to replica " + targetId);
        }
    }

    public List<V2VMessageEnvelope> checkRetransmissions() {
        long now = SimulationClock.currentTimeMillis();
        System.out.println("[Reliability " + myReplicaId + "] checkRetransmissions() at t=" + now
                + "ms, unackedMessages size=" + unackedMessages.size());
        List<V2VMessageEnvelope> toRetransmit = new ArrayList<>();
        // When using broadcast/multicast, the same physical broadcastSeq is tracked
        // under many
        // per-target buckets. We must deduplicate so we only retransmit one physical
        // broadcast.
        HashSet<Long> broadcastSeqQueued = new HashSet<>();

        for (Map.Entry<Integer, ConcurrentHashMap<Long, V2VMessageEnvelope>> entry : unackedMessages.entrySet()) {
            int targetId = entry.getKey();
            ConcurrentHashMap<Long, V2VMessageEnvelope> pending = entry.getValue();

            System.out.println("[Reliability " + myReplicaId + "] Checking " + pending.size()
                    + " pending messages to replica " + targetId);
            List<Long> toRemove = new ArrayList<>();

            ConcurrentHashMap<Long, Integer> targetAttempts = retransmissionAttempts.computeIfAbsent(targetId,
                    k -> new ConcurrentHashMap<>());
            for (V2VMessageEnvelope envelope : pending.values()) {
                // Preserve timeout/backoff across checks; do not reset every scan.
                if (envelope.currentTimeout <= 0) {
                    envelope.currentTimeout = RETX_TIMEOUT_MS;
                }
                long timeSince = now - envelope.timestampMs;
                System.out.println("[Reliability " + myReplicaId + "] seq=" + envelope.sequenceNum + " to " + targetId
                        + ": timeSince=" + timeSince + "ms, timeout=" + envelope.currentTimeout + "ms");

                if (now - envelope.timestampMs > envelope.currentTimeout) {
                    System.out.println("[Reliability] Resending seq=" + envelope.sequenceNum + " because "
                            + (now - envelope.timestampMs) + "ms (sim time) passed.");
                    int attempts = targetAttempts.getOrDefault(envelope.sequenceNum, 0);

                    if (attempts >= MAX_RETX_ATTEMPTS) {
                        // Max retransmissions reached, give up on this message
                        System.err.println("[Reliability " + myReplicaId + "] Max retransmission attempts ("
                                + MAX_RETX_ATTEMPTS + ") reached for seq=" + envelope.sequenceNum + " to replica "
                                + targetId + ", dropping message");
                        toRemove.add(envelope.sequenceNum);
                        targetAttempts.remove(envelope.sequenceNum);
                    } else {
                        // Queue for retransmission (don't send directly - causes JNI reentrant
                        // deadlock!)
                        System.out.println("[Reliability " + myReplicaId + "] Queuing retransmit seq="
                                + envelope.sequenceNum + " to replica " + targetId + " (attempt " + (attempts + 1) + "/"
                                + MAX_RETX_ATTEMPTS + ")");

                        envelope.currentTimeout = Math.min(envelope.currentTimeout * 2, 8000);
                        envelope.timestampMs = now; // Update timestamp for next retry jitter
                        // Deduplicate broadcast retransmits: only one physical broadcast per seq.
                        if (envelope.isBroadcast || ((envelope.sequenceNum & BROADCAST_SEQ_FLAG) != 0)) {
                            if (broadcastSeqQueued.add(envelope.sequenceNum)) {
                                toRetransmit.add(envelope);
                            }
                        } else {
                            toRetransmit.add(envelope);
                        }
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

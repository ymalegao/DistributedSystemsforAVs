package bftsmart.communication.V2V;
import bftsmart.communication.SystemMessage;
import bftsmart.communication.V2V.*;
import java.io.Serializable;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
//wrapper for BFTSmart messages sent over V2V -adds reliable metadata (sequence number, ack, etc)
public class V2VMessageEnvelope implements Serializable {
    private static final long serialVersionUID = 1L;

    public enum MessageType {
        DATA,  //bftsmart message
        ACK, //ack
        NACK,
        HEARTBEAT //keep alive
    }

    public final MessageType type;
    public final int fromReplicaId;
    public final int toReplicaId;
    public final long sequenceNum;
    public final long ackNum;
    public long currentTimeout;
    public ArrayList<Long> piggybackedAcks = new ArrayList<>();
    // For true broadcast-style multicasts: broadcaster tags ACKs by original sender.
    // broadcastAcks[originalSenderId] = list of sequence numbers the broadcaster received from originalSenderId.
    public HashMap<Integer, ArrayList<Long>> broadcastAcks = new HashMap<>();
    // True when this envelope represents a V2V multicast/broadcast delivery.
    public boolean isBroadcast = false;
    // True for fire-and-forget traffic that must skip sender-side retransmission
    // and receiver-side sequence ordering. Used for leader-change (LC) messages:
    // LC handling is idempotent at the application layer (distinct-sender sets,
    // regency gating), so reliability-layer ordering/retx would only add channel
    // pressure with no correctness benefit. Default false preserves behaviour
    // for legacy envelopes deserialized from older senders.
    public boolean isUnordered = false;

    public final byte[] serializedMessage;


    public long timestampMs;


    public V2VMessageEnvelope(MessageType type, int fromReplicaId, int toReplicaId, long sequenceNum, long ackNum, byte[] payload) {
        this.type = type;
        this.fromReplicaId = fromReplicaId;
        this.toReplicaId = toReplicaId;
        this.sequenceNum = sequenceNum;
        this.ackNum = ackNum;
        this.serializedMessage = payload;
        this.timestampMs = SimulationClock.currentTimeMillis();
        this.currentTimeout = 1000;

    }

    public static V2VMessageEnvelope createAck(int from, int to, long ackNum){
        return new V2VMessageEnvelope(MessageType.ACK, from, to, 0, ackNum, null);
    }



}

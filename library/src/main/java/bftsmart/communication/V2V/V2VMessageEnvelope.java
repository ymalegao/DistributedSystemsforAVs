package bftsmart.communication.V2V;
import bftsmart.communication.SystemMessage;

import java.io.Serializable;



//wrapper for BFTSmart messages sent over V2V -adds reliable metadata (sequence number, ack, etc)
public class V2VMessageEnvelope implements Serializable {
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

    public final byte[] serializedMessage;


    public final long timestampMs;


    public V2VMessageEnvelope(MessageType type, int fromReplicaId, int toReplicaId, long sequenceNum, long ackNum, byte[] payload) {
        this.type = type;
        this.fromReplicaId = fromReplicaId;
        this.toReplicaId = toReplicaId;
        this.sequenceNum = sequenceNum;
        this.ackNum = ackNum;
        this.serializedMessage = payload;
        this.timestampMs = System.currentTimeMillis();
    }

    public static V2VMessageEnvelope createAck(int from, int to, long ackNum){
        return new V2VMessageEnvelope(MessageType.ACK, from, to, 0, ackNum, null);
    }


    
}

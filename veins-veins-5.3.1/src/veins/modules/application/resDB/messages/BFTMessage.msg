//
// BFTMessage.msg
// OMNeT++ message definition for V2V cert-protocol frames (ResDB path)
//

import veins.base.utils.Coord;
import veins.modules.messages.BaseFrame1609_4;

namespace veins;

// messageType values:
// 0 = BFT_CONSENSUS       - Legacy BFT-SMaRt protocol messages
// 1 = ARRIVAL_ANNOUNCE    - Car broadcasts arrival claim
// 2 = WITNESS_RESPONSE    - (legacy, unused)
// 3 = READYQC_COMPLETE    - (legacy, unused)
// 4 = ARRIVAL_ECHO        - Replica echoes back to announcing car (physical check passed)
// 5 = ARRIVAL_CERT        - Car broadcasts f+1-echo certificate to all replicas
// 7 = EXECUTING           - Batch crossing notification, locks new arrivals
// 10 = ARRIVAL_ANNOUNCE_GOSSIP - Signed carrier frame with original announce bytes

packet BFTMessage extends BaseFrame1609_4 {
    int fromReplicaId;        // Source replica ID
    int toReplicaId;          // Destination replica ID (-1 for broadcast)
    int messageType;          // See messageType values above
    unsigned int sequenceNum; // Message sequence number
    simtime_t timestamp;      // When message was created
    int payloadLength;        // Length of the BFT payload
    uint8_t  payload[];      // Serialized payload bytes
}

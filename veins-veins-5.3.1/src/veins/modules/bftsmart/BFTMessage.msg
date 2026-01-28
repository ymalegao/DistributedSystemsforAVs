//
// BFTMessage.msg
// OMNeT++ message definition for BFT-SMaRt V2V communication
//

import veins.base.utils.Coord;
import veins.modules.messages.BaseFrame1609_4;

namespace veins;

// Message carrying BFT-SMaRt protocol data over V2V
packet BFTMessage extends BaseFrame1609_4 {
    int fromReplicaId;        // Source replica ID
    int toReplicaId;          // Destination replica ID (-1 for broadcast)
    int messageType;          // 0=consensus, 1=view-change, 2=ack, etc.
    unsigned int sequenceNum; // Message sequence number
    simtime_t timestamp;      // When message was created
    int payloadLength;        // Length of the BFT payload
    string payload;           // Serialized BFT-SMaRt message (base64 encoded)
}
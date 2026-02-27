//
// BFTMessage.msg
// OMNeT++ message definition for BFT-SMaRt V2V communication
//

import veins.base.utils.Coord;
import veins.modules.messages.BaseFrame1609_4;

namespace veins;

// messageType values:
// 0 = BFT_CONSENSUS       - Existing BFT-SMaRt protocol messages
// 1 = ARRIVAL_ANNOUNCE    - Car broadcasts arrival claim
// 2 = WITNESS_RESPONSE    - Neighbor sends witness signature
// 3 = READYQC_COMPLETE    - Car broadcasts completed ReadyQC
// 4 = VIEW_PROPOSAL       - Propose set of cars with valid ReadyQCs
// 5 = ORDER_PROPOSAL      - Propose ordering based on verifiedArrival

// Message carrying BFT-SMaRt protocol data over V2V
packet BFTMessage extends BaseFrame1609_4 {
    int fromReplicaId;        // Source replica ID
    int toReplicaId;          // Destination replica ID (-1 for broadcast)
    int messageType;          // See messageType values above
    unsigned int sequenceNum; // Message sequence number
    simtime_t timestamp;      // When message was created
    int payloadLength;        // Length of the BFT payload
    uint8_t  payload[];      // Serialized BFT-SMaRt message (base64 encoded)
}
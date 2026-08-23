// Copyright (C) 2026 Mathesh Kumar
// SPDX-License-Identifier: GPL-3.0-or-later

//
// BFTMessage.msg
// OMNeT++ message definition for V2V cert-protocol frames (ResDB path)
//

import veins.base.utils.Coord;
import veins.modules.messages.BaseFrame1609_4;

namespace v2vbft;

// messageType values:
// 0 = BFT_CONSENSUS       - Reserved/unused in the ResDB path
// 1 = ARRIVAL_ANNOUNCE    - Car broadcasts arrival claim
// 2 = WITNESS_RESPONSE    - (legacy, unused)
// 3 = READYQC_COMPLETE    - (legacy, unused)
// 4 = ARRIVAL_ECHO        - Replica echoes back to announcing car (physical check passed)
// 5 = ARRIVAL_CERT        - Car broadcasts f+1-echo certificate to all replicas
// 7 = EXECUTING           - Batch crossing notification, locks new arrivals
// 8 = RESDB_CONSENSUS     - Signed ResDB PBFT radio frame
// 9 = DECISION_GOSSIP     - Signed post-consensus order gossip
// 10 = ARRIVAL_ANNOUNCE_GOSSIP - Signed carrier frame with original announce bytes
// 11 = RESDB_CONSENSUS_RELAY - Signed relay of raw ResDB PBFT bytes
// 12 = CANCEL_ECHO        - Rollback cancel echo
// 13 = CANCEL_CERT        - Rollback cancel certificate
// 14 = CANCEL_COMMIT_GOSSIP - Signed post-consensus cancel-commit gossip

packet BFTMessage extends veins::BaseFrame1609_4 {
    int fromReplicaId;        // Source replica ID
    int toReplicaId;          // Destination replica ID (-1 for broadcast)
    int messageType;          // See messageType values above
    unsigned int sequenceNum; // Message sequence number
    simtime_t timestamp;      // When message was created
    int payloadLength;        // Length of the BFT payload
    uint8_t  payload[];      // Serialized payload bytes
}

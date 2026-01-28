#!/bin/bash
# Analyze collision and packet statistics from OMNeT++ results

if [ -z "$1" ]; then
    echo "Usage: $0 <result-file.sca>"
    echo "Example: $0 results/BFTOverV2V-#0.sca"
    exit 1
fi

RESULT_FILE="$1"

BATCH_SIZE=8

echo "========================================"
echo "Collision & Packet Loss Analysis"
echo "========================================"
echo ""


for node in $(seq 0 $((BATCH_SIZE - 1))); do
    echo "--- Node[$node] ---"

    # PHY collisions
    phy_coll=$(grep "node\[$node\].nic.phy80211p ncollisions" "$RESULT_FILE" | awk '{print $NF}')

    # MAC collisions
    mac_coll=$(grep "node\[$node\].nic.mac1609_4 collisions:count" "$RESULT_FILE" | awk '{print $NF}')

    # Packets sent
    sent=$(grep "node\[$node\].nic.mac1609_4 SentPackets" "$RESULT_FILE" | awk '{print $NF}')

    # Packets received
    recv_uni=$(grep "node\[$node\].nic.mac1609_4 ReceivedUnicastPackets" "$RESULT_FILE" | awk '{print $NF}')
    recv_bc=$(grep "node\[$node\].nic.mac1609_4 ReceivedBroadcasts" "$RESULT_FILE" | awk '{print $NF}')

    # Lost packets
    snir_lost=$(grep "node\[$node\].nic.mac1609_4 SNIRLostPackets" "$RESULT_FILE" | awk '{print $NF}')
    rxtx_lost=$(grep "node\[$node\].nic.mac1609_4 RXTXLostPackets" "$RESULT_FILE" | awk '{print $NF}')
    total_lost=$(grep "node\[$node\].nic.mac1609_4 TotalLostPackets" "$RESULT_FILE" | awk '{print $NF}')
    dropped=$(grep "node\[$node\].nic.mac1609_4 DroppedPacketsInMac" "$RESULT_FILE" | awk '{print $NF}')

    echo "  Sent: ${sent:-0} packets"
    echo "  Received: Unicast=${recv_uni:-0}, Broadcast=${recv_bc:-0}"
    echo "  PHY Collisions: ${phy_coll:-0}"
    echo "  MAC Collisions: ${mac_coll:-0}"
    echo "  SNIR Lost (bad signal): ${snir_lost:-0}"
    echo "  RXTX Lost (simultaneous RX/TX): ${rxtx_lost:-0}"
    echo "  Total Lost: ${total_lost:-0}"
    echo "  Dropped in MAC: ${dropped:-0}"
    echo ""
done

echo "========================================"
echo "Summary"
echo "========================================"
total_sent=$(grep "SentPackets" "$RESULT_FILE" | awk '{sum+=$NF} END {print sum}')
total_recv=$(grep "ReceivedBroadcasts" "$RESULT_FILE" | awk '{sum+=$NF} END {print sum}')
total_collisions=$(grep "phy80211p ncollisions" "$RESULT_FILE" | awk '{sum+=$NF} END {print sum}')
total_lost=$(grep "TotalLostPackets" "$RESULT_FILE" | awk '{sum+=$NF} END {print sum}')

echo "Total Packets Sent: ${total_sent:-0}"
echo "Total Broadcasts Received: ${total_recv:-0}"
echo "Total PHY Collisions: ${total_collisions:-0}"
echo "Total Lost Packets: ${total_lost:-0}"

if [ -n "$total_sent" ] && [ "$total_sent" -gt 0 ]; then
    loss_rate=$(awk "BEGIN {printf \"%.2f\", ($total_lost / $total_sent) * 100}")
    echo "Packet Loss Rate: ${loss_rate}%"
fi

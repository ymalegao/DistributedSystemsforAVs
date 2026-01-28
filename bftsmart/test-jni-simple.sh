#!/bin/bash

BFTSMART_DIR="/home/yash/omnetpp/omnetpp-6.2.0/bftsmart/library"
BFTSMART_BUILD="$BFTSMART_DIR/build/install/library"

echo "=========================================="
echo "Starting BFT-SMaRt Java Replicas"
echo "=========================================="
echo ""
echo "⚠️  Make sure OMNeT++ simulation is RUNNING first!"
echo ""
read -p "Press Enter when OMNeT++ is running..."

cleanup() {
    echo ""
    echo "Cleaning up Java processes..."
    pkill -f "IntersectionServer"
    pkill -f "IntersectionClient"
    sleep 1
}

trap cleanup EXIT

# Start 4 BFTSmart replicas
echo "[1/2] Starting 4 BFT-SMaRt replicas..."
cd "$BFTSMART_BUILD"
for i in 0 1 2 3; do
    echo "      Starting replica $i..."
    nix-shell -p jdk11 --run "./smartrun.sh -Dbftsmart.communication.useV2V=true \
      bftsmart.demo.intersection.IntersectionServer $i 4" \
      > /tmp/replica-$i.log 2>&1 &
    sleep 3
done

echo "      All replicas started!"
echo ""
echo "Replicas are running. Check logs:"
echo "  Replica 0: tail -f /tmp/replica-0.log"
echo "  Replica 1: tail -f /tmp/replica-1.log"
echo "  Replica 2: tail -f /tmp/replica-2.log"
echo "  Replica 3: tail -f /tmp/replica-3.log"
echo ""
echo "Waiting 10 seconds for system to stabilize..."
sleep 10

# Send test request
echo "[2/2] Sending test consensus request..."
nix-shell -p jdk11 --run "./smartrun.sh \
  bftsmart.demo.intersection.IntersectionClient 1000 4"

echo ""
echo "=========================================="
echo "Test completed! Check OMNeT++ for V2V traffic"
echo "Press Ctrl+C to stop replicas"
echo "=========================================="

# Keep running so replicas stay alive
wait
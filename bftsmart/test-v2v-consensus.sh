#!/bin/bash

# Test BFT-SMaRt consensus with V2V communication layer
# This script:
# 1. Starts the V2V bridge (simulates Veins forwarding)
# 2. Starts 4 BFT-SMaRt replicas with V2V enabled
# 3. Sends a client request
# 4. Cleans up

BFTSMART_DIR="/home/yash/omnetpp/omnetpp-6.2.0/bftsmart/library"
BFTSMART_BUILD="$BFTSMART_DIR/build/install/library"

# Make sure build exists
if [ ! -f "$BFTSMART_BUILD/smartrun.sh" ]; then
    echo "ERROR: BFT-SMaRt not built. Run: cd $BFTSMART_DIR && ./gradlew installDist"
    exit 1
fi

cd "$BFTSMART_BUILD"

echo "=========================================="
echo "BFT-SMaRt V2V Consensus Test"
echo "=========================================="
echo ""

# Cleanup function
cleanup() {
    echo ""
    echo "Cleaning up..."
    pkill -f "TestV2VBridge"
    pkill -f "IntersectionServer"
    pkill -f "IntersectionClient"
    sleep 1
    echo "Done!"
}

trap cleanup EXIT

# Step 1: Start V2V Bridge
echo "[1/3] Starting V2V bridge (simulates Veins)..."
./smartrun.sh bftsmart.communication.V2V.test.TestV2VBridge \
  > /tmp/v2v-bridge.log 2>&1 &
BRIDGE_PID=$!

sleep 2
echo "      Bridge started (PID: $BRIDGE_PID)"
echo ""

# Step 2: Start 4 replicas with V2V enabled
echo "[2/3] Starting 4 BFT-SMaRt replicas with V2V..."
for i in 0 1 2 3; do
    echo "      Starting replica $i..."
    ./smartrun.sh -Dbftsmart.communication.useV2V=true \
      bftsmart.demo.intersection.IntersectionServer $i 4 \
      > /tmp/replica-$i.log 2>&1 &

    sleep 2
done

echo "      All replicas started!"
echo ""

# Wait for replicas to initialize
echo "Waiting 10 seconds for replicas to initialize..."
sleep 10
echo ""


# Step 3: Send client request via gateway
echo "[3/3] Sending client request (4 cars)..."
echo ""
./smartrun.sh bftsmart.demo.intersection.IntersectionClient 1000 4 
echo ""
echo "=========================================="
echo "Test completed!"
echo "=========================================="
echo ""
echo "Check logs:"
echo "  Bridge:    tail -f /tmp/v2v-bridge.log"
echo "  Replica 0: tail -f /tmp/replica-0.log"
echo "  Replica 1: tail -f /tmp/replica-1.log"
echo "  Replica 2: tail -f /tmp/replica-2.log"
echo "  Replica 3: tail -f /tmp/replica-3.log"
echo ""

# Keep script running for a bit to see output
sleep 2

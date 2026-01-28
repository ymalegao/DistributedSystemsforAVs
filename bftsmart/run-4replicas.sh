#!/bin/bash
#
# Run OMNeT++ simulation with 4 BFTSmart replicas
# Each replica's output is filtered to a separate log file
#

cd /home/yash/omnetpp/fourway

echo "=========================================="
echo "Starting OMNeT++ with 4 BFTSmart Replicas"
echo "=========================================="
echo ""

# Set up library paths
NIX_LIB_PATHS=$(ldd /home/yash/omnetpp/omnetpp-6.2.0/lib/liboppsim_dbg.so | \
    grep /nix/store | \
    sed 's/.*=> //' | \
    sed 's/ (0x.*//' | \
    xargs -n1 dirname | \
    sort -u | \
    tr '\n' ':')

JAVA_HOME=${JAVA_HOME:-/usr/lib/jvm/java-11-openjdk-amd64}
JVM_LIB_PATH="${JAVA_HOME}/lib/server"

export LD_LIBRARY_PATH="${NIX_LIB_PATHS}:${JVM_LIB_PATH}:${LD_LIBRARY_PATH}"

echo "Running simulation..."
echo ""

# Run simulation and tee output to logs
./fourway -n /home/yash/veins-veins-5.3.1/src/veins:. \
    -u Cmdenv \
    -c BFT4Replicas \
    --sim-time-limit=60s \
    2>&1 | tee /tmp/bft-all-replicas.log

echo ""
echo "=========================================="
echo "Simulation Complete!"
echo "=========================================="
echo ""
echo "Full log: /tmp/bft-all-replicas.log"
echo ""
echo "To extract individual replica logs:"
echo "  grep 'Replica 0\\|\\[V2V Layer 0\\]\\|V2VNativeBridge0:' /tmp/bft-all-replicas.log > /tmp/replica-0.log"
echo "  grep 'Replica 1\\|\\[V2V Layer 1\\]\\|V2VNativeBridge1:' /tmp/bft-all-replicas.log > /tmp/replica-1.log"
echo "  grep 'Replica 2\\|\\[V2V Layer 2\\]\\|V2VNativeBridge2:' /tmp/bft-all-replicas.log > /tmp/replica-2.log"
echo "  grep 'Replica 3\\|\\[V2V Layer 3\\]\\|V2VNativeBridge3:' /tmp/bft-all-replicas.log > /tmp/replica-3.log"
echo ""
echo "To see JNI bridge activity:"
echo "  grep 'JNI\\|Message queued' /tmp/bft-all-replicas.log | head -100"
echo ""



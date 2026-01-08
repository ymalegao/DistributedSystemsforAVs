#!/bin/bash

# Launch script for IntersectionGateway instances
# Starts 4 gateways (one per car) on ports 9001-9004

SCRIPT_DIR="/home/yash/omnetpp/omnetpp-6.2.0/bftsmart/library/build/install/library"
cd "$SCRIPT_DIR" || {
    echo "Error: Cannot change to directory $SCRIPT_DIR"
    exit 1
}

# Check if smartrun.sh exists
if [ ! -f "$SCRIPT_DIR/smartrun.sh" ]; then
    echo "Error: smartrun.sh not found in $SCRIPT_DIR"
    exit 1
fi

echo "=========================================="
echo "Starting BFT-Smart Intersection Gateways"
echo "=========================================="
echo ""

# Array to store gateway PIDs
declare -a GATEWAY_PIDS=()

# Function to start a gateway in background
start_gateway_bg() {
    local client_id=$1
    local port=$2
    local log_file="$SCRIPT_DIR/gateway_${client_id}.log"
    
    echo "Starting gateway ${client_id} on port ${port}..."
    ./smartrun.sh bftsmart.demo.intersection.IntersectionGateway $client_id $port > "$log_file" 2>&1 &
    local pid=$!
    GATEWAY_PIDS+=($pid)
    echo "  Gateway ${client_id} started (PID: $pid, log: $log_file)"
    
    # Wait a bit for the gateway to start
    sleep 1
}

# Function to cleanup on exit
cleanup() {
    echo ""
    echo "=== Shutting down gateways ==="
    for pid in "${GATEWAY_PIDS[@]}"; do
        if kill -0 "$pid" 2>/dev/null; then
            echo "Stopping gateway (PID: $pid)..."
            kill "$pid" 2>/dev/null
        fi
    done
    wait
    echo "All gateways stopped."
    exit 0
}

# Set up trap to cleanup on script exit
trap cleanup EXIT INT TERM

# Start 4 gateways (one per car)
echo "=== Starting 4 Intersection Gateways ==="
start_gateway_bg 1 9001
start_gateway_bg 2 9002
start_gateway_bg 3 9003
start_gateway_bg 4 9004

echo ""
echo "All gateways started!"
echo ""
echo "Gateway ports:"
echo "  Gateway 1: localhost:9001"
echo "  Gateway 2: localhost:9002"
echo "  Gateway 3: localhost:9003"
echo "  Gateway 4: localhost:9004"
echo ""
echo "Gateway logs: $SCRIPT_DIR/gateway_*.log"
echo ""
echo "Test with netcat:"
echo "  echo 'REQUEST_CROSS CAR_1 straight 12.34' | nc localhost 9001"
echo ""
echo "Press Ctrl+C to stop all gateways"

# Keep script running so gateways stay alive
wait



#!/bin/bash

# Simple script to start BFT infrastructure for OMNeT++ integration
# Just starts servers and gateways, then waits for connections
# No automatic testing or reconfiguration

SCRIPT_DIR="/home/yash/omnetpp/omnetpp-6.2.0/bftsmart/library/build/install/library"

# Change to the script directory
cd "$SCRIPT_DIR" || {
    echo "Error: Cannot change to directory $SCRIPT_DIR"
    exit 1
}

# Check if smartrun.sh exists
if [ ! -f "$SCRIPT_DIR/smartrun.sh" ]; then
    echo "Error: smartrun.sh not found in $SCRIPT_DIR"
    echo "Please build the project first or check the SCRIPT_DIR path"
    exit 1
fi

# Function to kill any existing IntersectionServer or IntersectionGateway processes
kill_existing_processes() {
    echo "Checking for existing IntersectionServer and IntersectionGateway processes..."
    local existing_servers=$(pgrep -f "IntersectionServer" 2>/dev/null)
    local existing_gateways=$(pgrep -f "IntersectionGateway" 2>/dev/null)
    
    if [ -n "$existing_servers" ] || [ -n "$existing_gateways" ]; then
        echo "Found existing processes, killing them..."
        if [ -n "$existing_servers" ]; then
            echo "  Killing existing IntersectionServer processes: $existing_servers"
            kill $existing_servers 2>/dev/null
            sleep 2
            pgrep -f "IntersectionServer" | xargs kill -9 2>/dev/null
        fi
        if [ -n "$existing_gateways" ]; then
            echo "  Killing existing IntersectionGateway processes: $existing_gateways"
            kill $existing_gateways 2>/dev/null
            sleep 1
            pgrep -f "IntersectionGateway" | xargs kill -9 2>/dev/null
        fi
        echo "Waiting for ports to be released..."
        sleep 2
    else
        echo "No existing processes found."
    fi
}

# Cleanup function
cleanup() {
    echo ""
    echo "=========================================="
    echo "Shutting down BFT infrastructure..."
    echo "=========================================="
    if [ ${#GATEWAY_PIDS[@]} -gt 0 ]; then
        echo "Stopping gateways..."
        for pid in "${GATEWAY_PIDS[@]}"; do
            if kill -0 "$pid" 2>/dev/null; then
                echo "  Stopping gateway (PID: $pid)"
                kill "$pid" 2>/dev/null
            fi
        done
    fi
    if [ ${#SERVER_PIDS[@]} -gt 0 ]; then
        echo "Stopping servers..."
        for pid in "${SERVER_PIDS[@]}"; do
            if kill -0 "$pid" 2>/dev/null; then
                echo "  Stopping server (PID: $pid)"
                kill "$pid" 2>/dev/null
            fi
        done
    fi
    echo "Waiting for processes to exit..."
    sleep 2
    echo "BFT infrastructure stopped."
    exit 0
}

# Set trap to cleanup on script exit
trap cleanup EXIT INT TERM

# Kill any existing processes
kill_existing_processes

# Function to get the replica-to-replica port for a server ID
get_replica_port() {
    local server_id=$1
    case $server_id in
        0) echo 11001 ;;
        1) echo 11011 ;;
        2) echo 11021 ;;
        3) echo 11031 ;;
        *) echo 0 ;;
    esac
}

# Function to check if a port is listening
wait_for_port() {
    local port=$1
    local max_wait=60
    local waited=0
    
    echo "Waiting for port $port to be ready..."
    while [ $waited -lt $max_wait ]; do
        if lsof -Pi :$port -sTCP:LISTEN -t >/dev/null 2>&1; then
            echo "Port $port is now listening!"
            return 0
        fi
        sleep 1
        waited=$((waited + 1))
    done
    
    echo "Warning: Port $port did not become ready within $max_wait seconds"
    return 1
}

# Arrays to store PIDs
declare -a SERVER_PIDS=()
declare -a GATEWAY_PIDS=()

# Function to start a server in background
start_server() {
    local server_id=$1
    local total_servers=$2
    local log_file="$SCRIPT_DIR/server_${server_id}.log"
    
    echo "Starting server $server_id..."
    cd "$SCRIPT_DIR"
    ./smartrun.sh bftsmart.demo.intersection.IntersectionServer $server_id 0 > "$log_file" 2>&1 &
    local pid=$!
    SERVER_PIDS+=($pid)
    echo "  Server $server_id started (PID: $pid, log: $log_file)"
    cd - > /dev/null
}

# Function to start a gateway in background
start_gateway() {
    local client_id=$1
    local port=$2
    local log_file="$SCRIPT_DIR/gateway_${client_id}.log"
    
    echo "Starting gateway $client_id on port $port..."
    cd "$SCRIPT_DIR"
    ./smartrun.sh bftsmart.demo.intersection.IntersectionGateway $client_id $port > "$log_file" 2>&1 &
    local pid=$!
    GATEWAY_PIDS+=($pid)
    echo "  Gateway $client_id started (PID: $pid, log: $log_file)"
    cd - > /dev/null
    sleep 1
}

echo "=========================================="
echo "Starting BFT Infrastructure for OMNeT++"
echo "=========================================="
echo ""

# Start 4 BFT servers sequentially
echo "=== Starting 4 BFT-SMaRt Servers ==="
for i in 0 1 2 3; do
    start_server $i 4
    
    # Wait for server port before starting next (except last)
    if [ $i -lt 3 ]; then
        REPLICA_PORT=$(get_replica_port $i)
        wait_for_port $REPLICA_PORT
        echo "Giving server $i time to initialize..."
        sleep 2
    fi
done

echo ""
echo "All 4 servers started. Waiting 5 seconds for full initialization..."
sleep 5

# Start 4 gateways
echo ""
echo "=== Starting 4 TCP Gateways ==="
start_gateway 1 9001
start_gateway 2 9002
start_gateway 3 9003
start_gateway 4 9004

echo ""
echo "=========================================="
echo "BFT Infrastructure Ready!"
echo "=========================================="
echo ""
echo "Servers running:"
echo "  - Server 0 (PID: ${SERVER_PIDS[0]})"
echo "  - Server 1 (PID: ${SERVER_PIDS[1]})"
echo "  - Server 2 (PID: ${SERVER_PIDS[2]})"
echo "  - Server 3 (PID: ${SERVER_PIDS[3]})"
echo ""
echo "Gateways listening:"
echo "  - Gateway 1: localhost:9001 (PID: ${GATEWAY_PIDS[0]})"
echo "  - Gateway 2: localhost:9002 (PID: ${GATEWAY_PIDS[1]})"
echo "  - Gateway 3: localhost:9003 (PID: ${GATEWAY_PIDS[2]})"
echo "  - Gateway 4: localhost:9004 (PID: ${GATEWAY_PIDS[3]})"
echo ""
echo "Logs:"
echo "  - Server logs: $SCRIPT_DIR/server_*.log"
echo "  - Gateway logs: $SCRIPT_DIR/gateway_*.log"
echo ""
echo "=========================================="
echo "Waiting for OMNeT++ connections..."
echo "=========================================="
echo ""
echo "You can now start your OMNeT++ simulation."
echo "The BFT infrastructure will process REQUEST_CROSS commands from cars."
echo ""
echo "Press Ctrl+C to stop all BFT processes."
echo ""

# Wait indefinitely (until Ctrl+C)
while true; do
    sleep 1
    # Optional: Check if processes are still alive
    for pid in "${SERVER_PIDS[@]}"; do
        if ! kill -0 "$pid" 2>/dev/null; then
            echo "Warning: Server process $pid has died!"
        fi
    done
    for pid in "${GATEWAY_PIDS[@]}"; do
        if ! kill -0 "$pid" 2>/dev/null; then
            echo "Warning: Gateway process $pid has died!"
        fi
    done
done
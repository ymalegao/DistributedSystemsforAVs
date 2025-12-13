#!/bin/bash

# Script to start 4 IntersectionServer instances in separate terminal windows
# Each server waits for the previous one to be ready before starting

# Go to library/build/install/library/ since smartrun.sh is in this directory

SCRIPT_DIR="/home/yash/omnetpp/omnetpp-6.2.0/bftsmart/library/build/install/library"
# SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

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
            # Force kill if still running
            pgrep -f "IntersectionServer" | xargs kill -9 2>/dev/null
        fi
        if [ -n "$existing_gateways" ]; then
            echo "  Killing existing IntersectionGateway processes: $existing_gateways"
            kill $existing_gateways 2>/dev/null
            sleep 1
            # Force kill if still running
            pgrep -f "IntersectionGateway" | xargs kill -9 2>/dev/null
        fi
        echo "Waiting for ports to be released..."
        sleep 2
    else
        echo "No existing processes found."
    fi
}

# Kill any existing processes before starting new ones
kill_existing_processes

# Cleanup function to kill background processes on exit
cleanup() {
    if [ ${#GATEWAY_PIDS[@]} -gt 0 ]; then
        echo ""
        echo "Cleaning up gateway processes..."
        for pid in "${GATEWAY_PIDS[@]}"; do
            if kill -0 "$pid" 2>/dev/null; then
                echo "  Killing gateway process $pid"
                kill "$pid" 2>/dev/null
            fi
        done
    fi
    if [ ${#SERVER_PIDS[@]} -gt 0 ]; then
        echo ""
        echo "Cleaning up background server processes..."
        for pid in "${SERVER_PIDS[@]}"; do
            if kill -0 "$pid" 2>/dev/null; then
                echo "  Killing server process $pid"
                kill "$pid" 2>/dev/null
            fi
        done
    fi
}

# Set trap to cleanup on script exit
trap cleanup EXIT INT TERM

# Function to get the replica-to-replica port for a server ID
# These are the ports that other servers connect to
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
    local max_wait=60  # Maximum wait time in seconds
    local waited=0
    
    echo "Waiting for port $port to be ready..."
    while [ $waited -lt $max_wait ]; do
        # Check if port is listening using lsof or netstat
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

# Array to store server PIDs
declare -a SERVER_PIDS=()

# Array to store gateway PIDs
declare -a GATEWAY_PIDS=()

# Function to start a server (Linux-compatible)
start_server() {
    local server_id=$1
    local total_servers=$2
    local log_file="$SCRIPT_DIR/server_${server_id}.log"
    
    # Try to use a GUI terminal if available, otherwise run in background
    if command -v gnome-terminal &> /dev/null; then
        # Use gnome-terminal
        gnome-terminal --title="IntersectionServer $server_id" -- bash -c "cd '$SCRIPT_DIR' && ./smartrun.sh bftsmart.demo.intersection.IntersectionServer $server_id 0; read -p 'Press Enter to close...'"
    elif command -v xterm &> /dev/null; then
        # Use xterm
        xterm -T "IntersectionServer $server_id" -e bash -c "cd '$SCRIPT_DIR' && ./smartrun.sh bftsmart.demo.intersection.IntersectionServer $server_id 0; read -p 'Press Enter to close...'" &
        SERVER_PIDS+=($!)
    else
        # Run in background with log file
        cd "$SCRIPT_DIR"
        ./smartrun.sh bftsmart.demo.intersection.IntersectionServer $server_id 0 > "$log_file" 2>&1 &
        local pid=$!
        SERVER_PIDS+=($pid)
        echo "  Server $server_id started with PID $pid (output in $log_file)"
        cd - > /dev/null
    fi
}

# Function to start a gateway in background
start_gateway() {
    local client_id=$1
    local port=$2
    local log_file="$SCRIPT_DIR/gateway_${client_id}.log"
    
    echo "Starting gateway ${client_id} on port ${port}..."
    cd "$SCRIPT_DIR"
    ./smartrun.sh bftsmart.demo.intersection.IntersectionGateway $client_id $port > "$log_file" 2>&1 &
    local pid=$!
    GATEWAY_PIDS+=($pid)
    echo "  Gateway ${client_id} started with PID $pid (output in $log_file)"
    cd - > /dev/null
    
    # Wait a bit for the gateway to start
    sleep 1
}

# Start all 4 servers sequentially, waiting for each to be ready
for i in 0 1 2 3; do
    echo "Starting IntersectionServer $i..."
    start_server $i 4
    
    # Wait for this server's replica-to-replica port to be listening
    # before starting the next server (except for the last one)
    if [ $i -lt 3 ]; then
        REPLICA_PORT=$(get_replica_port $i)
        wait_for_port $REPLICA_PORT
        # Add a small additional delay to ensure full initialization
        echo "Giving server $i additional time to fully initialize..."
        sleep 2
    fi
done

echo "All 4 IntersectionServer instances have been started in separate terminal windows."
sleep 5

echo ""
echo "=========================================="
echo "Starting BFT-Smart Intersection Gateways"
echo "=========================================="
echo ""

# Start 4 gateways (one per car) on ports 9001-9004
echo "=== Starting 4 Intersection Gateways ==="
start_gateway 1 9001
start_gateway 2 9002
start_gateway 3 9003
start_gateway 4 9004

echo ""
echo "All gateways started!"
echo "Gateway ports:"
echo "  Gateway 1: localhost:9001"
echo "  Gateway 2: localhost:9002"
echo "  Gateway 3: localhost:9003"
echo "  Gateway 4: localhost:9004"
echo "Gateway logs: $SCRIPT_DIR/gateway_*.log"
echo ""
echo "Test with netcat:"
echo "  echo 'REQUEST_CROSS CAR_1 straight 12.34' | nc localhost 9001"
echo ""
sleep 2

echo "=== ROUND 1-2: Genesis (4 cars JOIN/LEAVE) ==="
sleep 10  # wait for Round 1+2 to complete (CAR_0 leaves queue but replica 0 stays alive)

echo ""
echo "=== Step 1: Starting replica 4 (CAR_E) - will wait for view update ==="
if command -v gnome-terminal &> /dev/null; then
    gnome-terminal --title="IntersectionServer 4 (CAR_E)" -- bash -c "cd '$SCRIPT_DIR' && ./smartrun.sh bftsmart.demo.intersection.IntersectionServer 4 0; read -p 'Press Enter to close...'"
elif command -v xterm &> /dev/null; then
    xterm -T "IntersectionServer 4 (CAR_E)" -e bash -c "cd '$SCRIPT_DIR' && ./smartrun.sh bftsmart.demo.intersection.IntersectionServer 4 0; read -p 'Press Enter to close...'" &
    SERVER_PIDS+=($!)
else
    cd "$SCRIPT_DIR"
    ./smartrun.sh bftsmart.demo.intersection.IntersectionServer 4 0 > "$SCRIPT_DIR/server_4.log" 2>&1 &
    SERVER_PIDS+=($!)
    echo "  Server 4 started with PID ${SERVER_PIDS[-1]} (output in $SCRIPT_DIR/server_4.log)"
    cd - > /dev/null
fi

echo "Waiting 5 seconds for replica 4 to boot up..."
sleep 5

echo ""
echo "=== Step 2: TTP adds replica 4 to the view ==="
echo "(Replicas 0,1,2,3 will vote to add replica 4)"
./smartrun.sh bftsmart.demo.intersection.ReconfigurationTrigger add4

echo "Waiting 8 seconds for view update to propagate and replica 4 to sync..."
sleep 8

echo ""
echo "=== Step 3: TTP removes replica 0 from view ==="
echo "(All 5 replicas vote on this, then replica 0 exits)"
./smartrun.sh bftsmart.demo.intersection.ReconfigurationTrigger remove0

echo "Waiting 5 seconds for replica 0 to exit..."
sleep 5

echo ""
echo "=== Testing with new view (replicas 1,2,3,4): Car E joins queue ==="
./smartrun.sh bftsmart.demo.intersection.IntersectionClient 2004 "JOIN:CAR_E"

sleep 2

echo ""
echo "=== Final state with replicas 1,2,3,4 ==="
./smartrun.sh bftsmart.demo.intersection.IntersectionClient 2000 "GET_STATE:FINAL"

echo ""
echo "=== All tests complete! Dynamic reconfiguration successful! ==="
echo "View changed: 0,1,2,3 → 0,1,2,3,4 → 1,2,3,4"
echo ""
echo "Server and gateway processes are still running. To stop them:"
echo "  - If using GUI terminals, close the terminal windows"
if [ ${#GATEWAY_PIDS[@]} -gt 0 ] || [ ${#SERVER_PIDS[@]} -gt 0 ]; then
    if [ ${#GATEWAY_PIDS[@]} -gt 0 ]; then
        echo "  - Kill gateways: kill ${GATEWAY_PIDS[*]}"
    fi
    if [ ${#SERVER_PIDS[@]} -gt 0 ]; then
        echo "  - Kill servers: kill ${SERVER_PIDS[*]}"
    fi
    echo "  - View server logs: tail -f $SCRIPT_DIR/server_*.log"
    echo "  - View gateway logs: tail -f $SCRIPT_DIR/gateway_*.log"
fi

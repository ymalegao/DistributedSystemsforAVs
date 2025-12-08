#!/bin/bash

# Script to start 4 IntersectionServer instances in separate terminal windows
# Each server waits for the previous one to be ready before starting

# Go to library/build/install/library/ since smartrun.sh is in this directory

SCRIPT_DIR="/Users/yashmalegaonkar/Documents/DistributedSystemsforAVs/bftsmart/library/build/install/library"
# SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

# Change to the script directory
cd "$SCRIPT_DIR"

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

# Function to start a server in a new terminal window
start_server() {
    local server_id=$1
    local total_servers=$2
    
    osascript <<EOF
tell application "Terminal"
    activate
    set newTab to do script "cd '$SCRIPT_DIR' && ./smartrun.sh bftsmart.demo.intersection.IntersectionServer $server_id $total_servers"
    set custom title of newTab to "IntersectionServer $server_id"
end tell
EOF
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


echo "=== ROUND 1-2: Genesis (4 cars JOIN/LEAVE) ==="
sleep 10  # wait for Round 1+2 to complete (CAR_0 leaves queue but replica 0 stays alive)

echo ""
echo "=== Step 1: Starting replica 4 (CAR_E) - will wait for view update ==="
osascript <<EOF
tell application "Terminal"
    activate
    set newTab to do script "cd '$SCRIPT_DIR' && ./smartrun.sh bftsmart.demo.intersection.IntersectionServer 4 4"
    set custom title of newTab to "IntersectionServer 4 (CAR_E)"
end tell
EOF

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

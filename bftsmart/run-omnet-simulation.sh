#!/bin/bash
# Wrapper script to run OMNeT++ simulation with correct library paths
# This ensures opp_run can find the Nix libraries it was built against

# Log file for all output
LOG_FILE="/tmp/bft-all-replicas.log"
OLD_VIEW_FILE="/home/yash/omnetpp/fourway/config/currentView"

if [ -f "$OLD_VIEW_FILE" ]; then
    rm "$OLD_VIEW_FILE"
fi

# Find Nix library paths from an existing Nix-built library
NIX_LIB_PATHS=$(ldd /home/yash/omnetpp/omnetpp-6.2.0/lib/liboppsim_dbg.so | \
    grep /nix/store | \
    sed 's/.*=> //' | \
    sed 's/ (0x.*//' | \
    xargs -n1 dirname | \
    sort -u | \
    tr '\n' ':')

# Set LD_LIBRARY_PATH to include Nix libraries
JAVA_HOME=${JAVA_HOME:-/usr/lib/jvm/java-11-openjdk-amd64}
JVM_LIB_PATH="${JAVA_HOME}/lib/server"

export LD_LIBRARY_PATH="${NIX_LIB_PATHS}:${JVM_LIB_PATH}:${LD_LIBRARY_PATH}"

# Clear log file and start logging
> "$LOG_FILE"
echo "=========================================="
echo "Running OMNeT++ Simulation with Nix Libraries"
echo "=========================================="
echo "LD_LIBRARY_PATH configured with Nix paths"
echo "Full log: $LOG_FILE"
echo ""

# Check if a simulation path was provided
if [ -z "$1" ]; then
    echo "Usage: $0 <path-to-simulation-directory>"
    echo "Example: $0 /home/yash/veins-veins-5.3.1/examples/fourway"
    exit 1
fi

SIM_DIR="$1"
shift  # Remove first argument, keep the rest for opp_run

if [ ! -d "$SIM_DIR" ]; then
    echo "ERROR: Simulation directory not found: $SIM_DIR"
    exit 1
fi

cd "$SIM_DIR" || exit 1
echo "Changed to simulation directory: $SIM_DIR"
echo ""

# Check for veins executable and run with tee to capture all output
if [ -x "./veins" ]; then
    echo "Starting simulation..."
    ./veins "$@" 2>&1 | tee -a "$LOG_FILE"
elif [ -x "./fourway" ]; then
    echo "Starting simulation..."
    ./fourway -n /home/yash/veins-veins-5.3.1/src/veins:. "$@" 2>&1 | tee -a "$LOG_FILE"
else
    # Fall back to opp_run
    echo "Starting with opp_run..."
    opp_run "$@" 2>&1 | tee -a "$LOG_FILE"
fi

echo ""
echo "=========================================="
echo "Simulation finished. Full log saved to: $LOG_FILE"
echo "=========================================="

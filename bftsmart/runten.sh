#!/bin/bash
# Wrapper script to run OMNeT++ simulation 10 times for benchmarking
# Calls run-omnet-simulation.sh for each run to ensure clean environment

echo "=========================================="
echo "OMNeT++ Benchmark Wrapper (10x Runs)"
echo "=========================================="

# CHECK ARGUMENTS
# ---------------------------------------------------------
if [ -z "$1" ]; then
    echo "Usage: $0 <path-to-simulation-directory> [OMNeT arguments]"
    echo "Example: $0 /home/yash/omnetpp/fourway -c EightVehiclesBFTOverV2V"
    exit 1
fi

SIM_DIR="$1"
shift  # Remove the path argument, keep the rest for run-omnet-simulation.sh

if [ ! -d "$SIM_DIR" ]; then
    echo "ERROR: Simulation directory not found: $SIM_DIR"
    exit 1
fi

# Find the run-omnet-simulation.sh script
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUN_SCRIPT="$SCRIPT_DIR/run-omnet-simulation.sh"

if [ ! -x "$RUN_SCRIPT" ]; then
    echo "ERROR: run-omnet-simulation.sh not found at: $RUN_SCRIPT"
    exit 1
fi

echo "Using runner script: $RUN_SCRIPT"
echo "Simulation directory: $SIM_DIR"

# 3. PREPARE OUTPUT DIRECTORY
# ---------------------------------------------------------
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
BENCH_DIR="$SIM_DIR/benchmarks/batch_$TIMESTAMP"
mkdir -p "$BENCH_DIR"
echo "Logs will be saved to: $BENCH_DIR"
echo ""

# START BENCHMARK LOOP (10 RUNS)
# ---------------------------------------------------------
RUNS=15

for ((i=1; i<=RUNS; i++))
do
    echo "--- Starting Run #$i of $RUNS ---"

    # Define log file for this run
    LOG_FILE="$BENCH_DIR/run_$i.log"

    # Call run-omnet-simulation.sh (just like manual runs)
    # This handles ALL environment setup, cleanup, and execution
    "$RUN_SCRIPT" "$SIM_DIR" "$@" > "$LOG_FILE" 2>&1
    SIM_EXIT_CODE=$?

    echo "   [Process] Simulation exited with code: $SIM_EXIT_CODE"

    # Verify success
    if grep -q "Consensus Completed" "$LOG_FILE" 2>/dev/null; then
        echo "   [Result] Run #$i: SUCCESS (Log: run_$i.log)"
    else
        echo "   [Result] Run #$i: POTENTIAL FAILURE (Check run_$i.log)"
    fi

    # Wait between runs (match manual timing)
    # INCREASED: 60s allows TCP TIME_WAIT to clear (default 60s on Linux)
    echo "   [Cleanup] Waiting 60s for TCP ports to be released..."
    sleep 60
done

echo ""
echo "=========================================="
echo "Benchmark Batch Complete."
echo "Results stored in: $BENCH_DIR"
echo "You can now run: python3 parse_metrics.py $BENCH_DIR"
echo "=========================================="
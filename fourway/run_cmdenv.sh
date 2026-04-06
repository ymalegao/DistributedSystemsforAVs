#!/bin/bash
# Run with Cmdenv (no GUI) for debugging

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
VEINS_ROOT="${VEINS_ROOT:-${REPO_ROOT}/veins-veins-5.3.1}"

exec "${REPO_ROOT}/bftsmart/run-omnet-simulation.sh" \
    "${SCRIPT_DIR}" \
    -m \
    -u Cmdenv \
    -c WithChannelSwitching \
    -n ".:${VEINS_ROOT}/src/veins:${VEINS_ROOT}/examples/veins" \
    --image-path="${VEINS_ROOT}/images" \
    -l "${VEINS_ROOT}/src/veins" \
    omnetpp.ini \
    "$@"

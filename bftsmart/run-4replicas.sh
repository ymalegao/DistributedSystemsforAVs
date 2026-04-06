#!/bin/bash
#
# Run OMNeT++ simulation with 4 BFT-SMaRt replicas using the portable launcher.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

exec "${REPO_ROOT}/bftsmart/run-omnet-simulation.sh" \
    "${REPO_ROOT}/fourway" \
    -u Cmdenv \
    -c BFT4Replicas \
    --sim-time-limit=60s \
    "$@"


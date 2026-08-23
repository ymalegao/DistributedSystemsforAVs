#!/bin/bash
# Debug script for fourway simulation

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
VEINS_ROOT="${VEINS_ROOT:-${REPO_ROOT}/.external/veins}"

gdb -ex "set pagination off" -ex "run" -ex "bt" -ex "quit" --args ./fourway \
    -m \
    -u Cmdenv \
    -c WithChannelSwitching \
    -n ".:${VEINS_ROOT}/src/veins:${REPO_ROOT}/src/v2vbft" \
    --image-path="${VEINS_ROOT}/images" \
    -l "${VEINS_ROOT}/src/veins" \
    omnetpp.ini \
    "$@" 2>&1 | tail -100

#!/bin/bash
# Debug script for fourway simulation

VEINS_ROOT="/mnt/c/Users/yashm/src/veins-veins-5.3.1"

# Run with GDB
gdb -ex "set pagination off" -ex "run" -ex "bt" -ex "quit" --args ./fourway \
    -m \
    -u Cmdenv \
    -c WithChannelSwitching \
    -n ".:${VEINS_ROOT}/src/veins:${VEINS_ROOT}/examples/veins" \
    --image-path="${VEINS_ROOT}/images" \
    -l "${VEINS_ROOT}/src/veins" \
    omnetpp.ini 2>&1 | tail -100


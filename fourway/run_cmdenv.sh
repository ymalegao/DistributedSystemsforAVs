#!/bin/bash
# Run with Cmdenv (no GUI) for debugging

VEINS_ROOT="/mnt/c/Users/yashm/src/veins-veins-5.3.1"

./fourway \
    -m \
    -u Cmdenv \
    -c WithChannelSwitching \
    -n ".:${VEINS_ROOT}/src/veins:${VEINS_ROOT}/examples/veins" \
    --image-path="${VEINS_ROOT}/images" \
    -l "${VEINS_ROOT}/src/veins" \
    omnetpp.ini


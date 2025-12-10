#!/bin/bash
# Corrected run script for fourway simulation with Veins

# Set Veins paths
VEINS_ROOT="/mnt/c/Users/yashm/src/veins-veins-5.3.1"

# Run the simulation with CORRECTED NED path pointing to veins directory
./fourway \
    -m \
    -u Qtenv \
    -c WithChannelSwitching \
    -n ".:${VEINS_ROOT}/src/veins:${VEINS_ROOT}/examples/veins" \
    --image-path="${VEINS_ROOT}/images" \
    -l "${VEINS_ROOT}/src/veins" \
    omnetpp.ini


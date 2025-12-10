#!/bin/bash
# Run script for fourway simulation with Veins

# Source OMNeT++ environment
source ~/omnetpp/omnetpp-6.2.0/setenv 2>/dev/null || true

# Set Veins paths
VEINS_ROOT="/mnt/c/Users/yashm/src/veins-veins-5.3.1"

# Run the simulation
./fourway \
    -m \
    -u Qtenv \
    -c WithChannelSwitching \
    -n ".:${VEINS_ROOT}/src:${VEINS_ROOT}/examples/veins" \
    --image-path="${VEINS_ROOT}/images" \
    -l "${VEINS_ROOT}/src/veins" \
    omnetpp.ini


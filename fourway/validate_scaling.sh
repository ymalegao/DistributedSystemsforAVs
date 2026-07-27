#!/usr/bin/env bash
set -uo pipefail
FOURWAY=/home/mathesh/Documents/code/v2v/DistributedSystemsforAVs/fourway
cd "$FOURWAY"
# label:config pairs for the 8 cells (4-veh uses existing configs)
CELLS=(
  "OFF4:FourVehiclesResDB"
  "ON4:FourVehiclesFourUnitsResDB"
  "OFF8:ScaleOff8"
  "ON8:ScaleOn8"
  "OFF12:ScaleOff12"
  "ON12:ScaleOn12"
  "OFF16:ScaleOff16"
  "ON16:ScaleOn16"
)
mkdir -p /tmp/scaling_validate
for cell in "${CELLS[@]}"; do
  lbl="${cell%%:*}"; cfg="${cell##*:}"
  log="/tmp/scaling_validate/${lbl}.log"
  timeout 500 ./run-resdb-simulation.sh -f omnetpp.ini -f scaling_study.ini \
    -u Cmdenv -c "$cfg" --sim-time-limit=30s > "$log" 2>&1
  rc=$?
  c=$(grep -c Order_Decided_Time "$log")
  rej=$(grep -c 'vehicle count out of range' "$log")
  endt=$(grep -oE 'at t=[0-9.]+s' "$log" | tail -1)
  echo "VALIDATE ${lbl} (${cfg}) exit=${rc} commits=${c} range_rejects=${rej} end=${endt}"
done
echo "VALIDATE_DONE"

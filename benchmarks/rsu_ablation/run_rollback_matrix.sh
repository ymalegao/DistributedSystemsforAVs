#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# Rollback ablation under FAULT PRESSURE: does the emergency rollback still
# recover when vehicles go silent, with vs without static intersection units?
#
#   OFF = EighteenVehRollbackNoUnits    (18 veh, N=18)  CANCEL |E|=16, quorum=11
#   ON  = EighteenVehFourUnitsRollback  (18 veh + 4 IU, N=22) |E|=20, quorum=13
#
# k = number of PBFT-silent vehicles (node[1..k]). Units never go silent, so:
#   OFF loses quorum at k >= 6  (16-6 = 10 < 11)
#   ON  loses quorum at k >= 8  (20-8 = 12 < 13)   <- units buy 2 extra faults
# node[16]/node[17] (late normal + ambulance) are deliberately NOT silenced:
# silencing the ambulance would kill the rollback *trigger*, not test the quorum.
#
# The late-ambulance trigger is known to be intermittent, so run MULTIPLE reps and
# report a success RATE — single runs are not trustworthy here.
#
# Run INSIDE opp_env:
#   benchmarks/rsu_ablation/run_rollback_matrix.sh "0 6 7" 3 400
# ─────────────────────────────────────────────────────────────────────────────
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
FOURWAY="$REPO/fourway"
RESULTS="$HERE/results_rollback"
FASTXML="$FOURWAY/config_fast.xml"
KLIST="${1:-0 6 7}"
REPS="${2:-3}"
TIMEOUT="${3:-400}"
mkdir -p "$RESULTS"

gen_ini() {   # $1 = k ; prints path
  local k="$1" f="$RESULTS/_rbfault_k${1}.ini"
  {
    echo "# auto-generated — k=${k} PBFT-silent vehicles, fast channel, 5ms poll"
    echo "[General]"
    echo "*.**.nic.phy80211p.analogueModels = xmldoc(\"${FASTXML}\")"
    echo "*.node[*].appl.transportPollInterval = 0.005"
    echo "*.node[*].appl.timeTickInterval      = 0.005"
    echo "*.iu[*].appl.transportPollInterval   = 0.005"
    echo "*.iu[*].appl.timeTickInterval        = 0.005"
    # The measured outcome (did epoch-0 ORDER commit, or did vehicles fall back to
    # stop-sign timeouts?) is decided by ~t=20s. Cap sim-time so every cell is bounded
    # and the sweep stays tractable — we do not need the cars to finish crossing.
    echo "sim-time-limit = 25s"
    local i
    for ((i=1; i<=k; i++)); do
      echo "*.node[${i}].appl.isByzantine = true"
      echo "*.node[${i}].appl.byzantinePbftSilent = true"
    done
  } > "$f"
  printf '%s\n' "$f"
}

run_one() {   # $1=label $2=config $3=k $4=rep
  local label="$1" config="$2" k="$3" rep="$4" ini log rc start wall
  ini="$(gen_ini "$k")"
  log="$RESULTS/${label}_k${k}_rep${rep}.log"
  start=$(date +%s)
  ( cd "$FOURWAY" && timeout "$TIMEOUT" \
      ./run-resdb-simulation.sh -f "$FOURWAY/omnetpp.ini" -f "$ini" \
        -u Cmdenv -c "$config" ) > "$log" 2>&1
  rc=$?; wall=$(( $(date +%s) - start ))
  local ord fb
  ord=$(grep -c 'Order_Decided_Time' "$log")
  fb=$(grep -c 'StopSign_Timeout: 1' "$log")
  echo "  ${label} k=${k} rep=${rep} exit=${rc} wall=${wall}s order_commits=${ord} timeout_fallbacks=${fb}"
}

echo "=== rollback ablation: k=[${KLIST}] reps=${REPS} timeout=${TIMEOUT}s ==="
for rep in $(seq 1 "$REPS"); do
  for k in $KLIST; do
    run_one OFF EighteenVehRollbackNoUnits    "$k" "$rep"
    run_one ON  EighteenVehFourUnitsRollback  "$k" "$rep"
  done
done
echo "RB_MATRIX_DONE results in ${RESULTS}"

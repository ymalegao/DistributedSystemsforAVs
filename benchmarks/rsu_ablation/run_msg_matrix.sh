#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# 18-vehicle MESSAGE-COUNT study (focused). Unlike run_rollback_matrix.sh (which
# measures commit availability), this measures total messages sent per run — which
# is only emitted in finish(), so runs MUST end cleanly. We force that with a hard
# --sim-time-limit passed on the COMMAND LINE (highest OMNeT++ precedence, beats the
# [General] sim-time-limit=50000s that otherwise lets runs overrun and get killed).
#
#   OFF = EighteenVehRollbackNoUnits    (18 veh, N=18)
#   ON  = EighteenVehFourUnitsRollback  (18 veh + 4 IU, N=22)
#
# Results land in results_rollback_msg/ so the availability study is untouched.
#   benchmarks/rsu_ablation/run_msg_matrix.sh "0 4 6 8" 2 250 30
# ─────────────────────────────────────────────────────────────────────────────
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
FOURWAY="$REPO/fourway"
RESULTS="$HERE/results_rollback_msg"
FASTXML="$FOURWAY/config_fast.xml"
KLIST="${1:-0 4 6 8}"
REPS="${2:-2}"
TIMEOUT="${3:-250}"
SIMLIMIT="${4:-30}"     # sim seconds; forced via --sim-time-limit so finish() fires
mkdir -p "$RESULTS"

gen_ini() {   # $1 = k ; prints path
  local k="$1" f="$RESULTS/_msgfault_k${1}.ini"
  {
    echo "# auto-generated — k=${k} PBFT-silent vehicles, fast channel, 5ms poll"
    echo "[General]"
    echo "*.**.nic.phy80211p.analogueModels = xmldoc(\"${FASTXML}\")"
    echo "*.node[*].appl.transportPollInterval = 0.005"
    echo "*.node[*].appl.timeTickInterval      = 0.005"
    echo "*.iu[*].appl.transportPollInterval   = 0.005"
    echo "*.iu[*].appl.timeTickInterval        = 0.005"
    local i
    for ((i=1; i<=k; i++)); do
      echo "*.node[${i}].appl.isByzantine = true"
      echo "*.node[${i}].appl.byzantinePbftSilent = true"
    done
  } > "$f"
  printf '%s\n' "$f"
}

# total messages = sum of LAST cumulative Messages_Sent per replica (finish()-time)
tot_msgs() {
  awk 'match($0,/\[METRICS ([0-9]+)\] Messages_Sent: ([0-9]+)/,m){last[m[1]]=m[2]}
       END{s=0;for(r in last)s+=last[r];print s+0}' "$1"
}

run_one() {   # $1=label $2=config $3=k $4=rep
  local label="$1" config="$2" k="$3" rep="$4" ini log rc start wall
  ini="$(gen_ini "$k")"
  log="$RESULTS/${label}_k${k}_rep${rep}.log"
  start=$(date +%s)
  ( cd "$FOURWAY" && timeout "$TIMEOUT" \
      ./run-resdb-simulation.sh -f "$FOURWAY/omnetpp.ini" -f "$ini" \
        -u Cmdenv -c "$config" --sim-time-limit=${SIMLIMIT}s ) > "$log" 2>&1
  rc=$?; wall=$(( $(date +%s) - start ))
  local ord fb msgs endt
  ord=$(grep -c 'Order_Decided_Time' "$log")
  fb=$(grep -c 'StopSign_Timeout: 1' "$log")
  msgs=$(tot_msgs "$log")
  endt=$(grep -oE '\*\* Event #[0-9]+ +t=[0-9.]+' "$log" | tail -1 | grep -oE 't=[0-9.]+')
  echo "  ${label} k=${k} rep=${rep} exit=${rc} wall=${wall}s end=${endt} commits=${ord} fallbacks=${fb} msgs=${msgs}"
}

echo "=== 18-veh MESSAGE study: k=[${KLIST}] reps=${REPS} sim-limit=${SIMLIMIT}s timeout=${TIMEOUT}s ==="
for rep in $(seq 1 "$REPS"); do
  for k in $KLIST; do
    run_one OFF EighteenVehRollbackNoUnits    "$k" "$rep"
    run_one ON  EighteenVehFourUnitsRollback  "$k" "$rep"
  done
done
echo "MSG_MATRIX_DONE results in ${RESULTS}"

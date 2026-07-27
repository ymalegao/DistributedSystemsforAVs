#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# Scaling matrix: 4/8/12/16 vehicles, with/without 4 RSU units, at k=0 (min) and
# k=k_max (each config's silent-fault frontier, N-quorum). Captures consensus
# commit + total messages/run (finish()-only, so a hard --sim-time-limit is used).
#
# k_max per config (N - quorum):
#   OFF4=1 ON4=3 | OFF8=3 ON8=5 | OFF12=5 ON12=5 | OFF16=5 ON16=7
#
# Run INSIDE opp_env:  run_scaling_matrix.sh [reps] [simlimit] [timeout]
# ─────────────────────────────────────────────────────────────────────────────
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
FOURWAY="$REPO/fourway"
RESULTS="$HERE/results_scaling"
FASTXML="$FOURWAY/config_fast.xml"
REPS="${1:-2}"
SIMLIMIT="${2:-32}"
TIMEOUT="${3:-700}"
mkdir -p "$RESULTS"

# cell = "ARM V CONFIG KMAX"
CELLS=(
  "OFF 4  FourVehiclesResDB           1"
  "ON  4  FourVehiclesFourUnitsResDB  3"
  "OFF 8  ScaleOff8                   3"
  "ON  8  ScaleOn8                    5"
  "OFF 12 ScaleOff12                  5"
  "ON  12 ScaleOn12                   5"
  "OFF 16 ScaleOff16                  5"
  "ON  16 ScaleOn16                   7"
)

gen_ini() {   # $1=k ; prints path (fast channel + 5ms poll + silence node[1..k])
  local k="$1" f="$RESULTS/_sfault_k${1}.ini"
  {
    echo "[General]"
    echo "*.**.nic.phy80211p.analogueModels = xmldoc(\"${FASTXML}\")"
    echo "*.node[*].appl.transportPollInterval = 0.005"
    echo "*.node[*].appl.timeTickInterval      = 0.005"
    echo "*.iu[*].appl.transportPollInterval   = 0.005"
    echo "*.iu[*].appl.timeTickInterval        = 0.005"
    local i
    for ((i=1;i<=k;i++)); do
      echo "*.node[${i}].appl.isByzantine = true"
      echo "*.node[${i}].appl.byzantinePbftSilent = true"
    done
  } > "$f"
  printf '%s\n' "$f"
}

tot_msgs() { awk 'match($0,/\[METRICS ([0-9]+)\] Messages_Sent: ([0-9]+)/,m){last[m[1]]=m[2]}
                  END{s=0;for(r in last)s+=last[r];print s+0}' "$1"; }

run_one() {   # $1=arm $2=V $3=config $4=k $5=rep
  local arm="$1" V="$2" cfg="$3" k="$4" rep="$5" ini log rc
  ini="$(gen_ini "$k")"
  log="$RESULTS/${arm}_v${V}_k${k}_rep${rep}.log"
  ( cd "$FOURWAY" && timeout "$TIMEOUT" \
      ./run-resdb-simulation.sh -f "$FOURWAY/omnetpp.ini" -f "$FOURWAY/scaling_study.ini" \
        -f "$ini" -u Cmdenv -c "$cfg" --sim-time-limit=${SIMLIMIT}s ) > "$log" 2>&1
  rc=$?
  echo "  ${arm} v${V} k${k} rep${rep} exit=${rc} commits=$(grep -c Order_Decided_Time "$log") msgs=$(tot_msgs "$log")"
}

echo "=== scaling matrix: reps=${REPS} simlimit=${SIMLIMIT}s timeout=${TIMEOUT}s ==="
for rep in $(seq 1 "$REPS"); do
  for cell in "${CELLS[@]}"; do
    read -r arm V cfg kmax <<< "$cell"
    run_one "$arm" "$V" "$cfg" 0     "$rep"
    run_one "$arm" "$V" "$cfg" "$kmax" "$rep"
  done
done
echo "SCALING_MATRIX_DONE results in ${RESULTS}"

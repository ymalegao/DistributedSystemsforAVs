#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# Ablation studies for the V2V-BFT intersection protocol.
#
# Every study sweeps vehicle count where the claim scales with traffic, because
# a single operating point cannot show a frontier moving or an effect growing
# with load. Logs are named ab<N>_<arm>_n<veh>_k<k>_rep<r>.log; the plotter
# keys cells by (study, arm, k, n).
#
#   1  RSU on/off      — commit rate vs injected faults, and throughput, vs N
#   3  baseline        — actuated traffic light vs ours: delay and throughput vs N
#   4  priority        — ambulance wait vs queue length
#   5  rollback        — late-emergency outcome and its throughput cost
#
# Usage (inside opp_env, with veins_launchd running on :9999):
#   benchmarks/ablations/run_ablations.sh <1|3|4|5|all> [reps]
# ─────────────────────────────────────────────────────────────────────────────
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
FOURWAY="$REPO/scenarios/fourway"
FASTXML="$FOURWAY/config_fast.xml"
WHICH="${1:-all}"
REPS="${2:-3}"

RES="${RESULTS_DIR:-$HERE/results}"
mkdir -p "$RES"

# Vehicle counts and their scenario-name prefixes.
NS=(4 8 12 16 20)
declare -A WORD=( [4]=Four [8]=Eight [12]=Twelve [16]=Sixteen [20]=Twenty )

# ── Lane arms ────────────────────────────────────────────────────────────────
# Every study runs twice: once on the one-lane network, once on the two-lane
# one. The two are never plotted on shared axes (see plotter/figures/_lanes.py);
# each produces its own figure.
#
# The lane rides in the log's ARM field, which plotter.io.logparse already
# parses -- its arm pattern is lazy, so "OFF_2lane" survives the _n/_k split.
# The one-lane arm keeps the BARE name on purpose: run()'s cache keys on the log
# filename, so renaming it would orphan every existing one-lane log and force a
# full re-sweep of work that is already measured and unchanged.
LANES=(1lane 2lane)
lane_sfx() { [[ "$1" == 1lane ]] && echo "" || echo "_$1"; }

declare -A TWO_BASE=( [4]=FourVehiclesTwoLaneScaleResDB  [8]=EightVehiclesTwoLaneScaleResDB
                      [12]=TwelveVehiclesTwoLaneScaleResDB [16]=SixteenVehiclesTwoLaneResDB
                      [20]=TwentyVehiclesTwoLaneScaleResDB )

# Scenario config for (N, lane, kind). kind: base | units | tl
cfg_for() {
  local n="$1" lane="$2" kind="$3"
  case "$kind:$lane" in
    base:1lane)  echo "${WORD[$n]}VehiclesResDB" ;;
    base:2lane)  echo "${TWO_BASE[$n]}" ;;
    units:1lane) echo "${WORD[$n]}VehiclesFourUnitsResDB" ;;
    units:2lane) echo "${WORD[$n]}VehiclesTwoLaneFourUnitsResDB" ;;
    tl:1lane)    echo "tl${n}veh" ;;
    tl:2lane)    echo "tl${n}veh2lane" ;;
    *) echo "cfg_for: bad kind/lane $kind:$lane" >&2; return 1 ;;
  esac
}
# Faults a view of N tolerates: f = (N-1)/3.
f_of() { echo $((($1 - 1) / 3)); }

# Silent replicas a view of N survives before quorum (2f+1) becomes
# unreachable. This, NOT f, is where consensus actually stops: at 12 vehicles
# f is 3 but 5 replicas can fall silent and 7 still remain to form the quorum.
# Sweeping only to f+1 left every count except 4 and 16 at a flat 100%, which
# locates nothing. Mirrors aggregate.tolerated_faults().
tolerated_of() { local f; f=$(f_of "$1"); echo $(( $1 - (2 * f + 1) )); }

# OMNeT++ orders node[] by SUMO vehicle ID lexicographically, so for n>9 the
# node index differs from the replica ID (n=16: veh10 < veh2, so node[2]=veh10).
# Injecting a fault at the wrong index silences a vehicle we did not choose and
# still produces a plausible-looking run, so every injection goes through here.
replica_to_node_idx() {
  local _n="$1" _r="$2" _count=0 _i
  for (( _i=0; _i<_n; _i++ )); do
    [[ $_i -ne $_r && "veh${_i}" < "veh${_r}" ]] && (( _count++ ))
  done
  echo $_count
}

common_ini() {   # $1=path  $2=repetition (seeds SUMO)
  {
    echo "[General]"
    echo "*.**.nic.phy80211p.analogueModels = xmldoc(\"${FASTXML}\")"
    echo "*.node[*].appl.transportPollInterval = 0.005"
    echo "*.node[*].appl.timeTickInterval      = 0.005"
    echo "*.iu[*].appl.transportPollInterval   = 0.005"
    echo "*.iu[*].appl.timeTickInterval        = 0.005"
    # SUMO's RNG seed, which is what draws each vehicle's turn from its route
    # distribution. TraCIScenarioManager defaults this to the OMNeT run number,
    # and every cell here is run #0 -- so without varying it the repetitions
    # replay one identical scenario rather than sampling the turn demand.
    echo "*.manager.seed = ${2:-1}"
  } > "$1"
}

# Silence replicas 1..k (never the primary at 0, so the run tests quorum loss
# rather than leader failure).
silence_k() {   # $1=ini $2=n $3=k
  local ini="$1" n="$2" k="$3" r idx
  for (( r=1; r<=k; r++ )); do
    idx=$(replica_to_node_idx "$n" "$r")
    echo "*.node[${idx}].appl.isByzantine = true"        >> "$ini"
    echo "*.node[${idx}].appl.byzantinePbftSilent = true" >> "$ini"
  done
}

run() {   # $1=logname $2=config ; $3.. = extra args
  local name="$1" cfg="$2"; shift 2
  local log="$RES/$name"
  # Resumable: a log that reached finish() is a complete cell, so a re-invoked
  # matrix fills gaps instead of re-running hours of work. Partial logs (killed
  # run, crash before finish) have no [METRICS line and are re-run.
  if [[ "${FORCE_RERUN:-0}" != 1 && -s "$log" ]] && grep -q '^\[METRICS ' "$log"; then
    echo "    -> cached"
    return
  fi
  # LOG_FILE must be per-run: the runner used to hardcode one path, and two
  # interleaved runs produced a log with two finish markers that still parsed.
  ( cd "$FOURWAY" && LOG_FILE="$RES/.${name}.simlog" timeout 900 \
      "$REPO/tools/run-resdb-simulation.sh" -f "$FOURWAY/omnetpp.ini" "$@" \
      --debug-on-errors=false -u Cmdenv -c "$cfg" ) > "$log" 2>&1
  local rc=$?
  echo "    -> rc=$rc decided=$(grep -c Order_Decided_Time "$log") cars=$(grep -c CAR-METRICS "$log")"
}

# ── 1: RSU on/off, swept over vehicle count and injected faults ──────────────
ablation1() {
  echo "### Ablation 1: RSU on/off across N and k ###"
  for r in $(seq 1 "$REPS"); do
    for n in "${NS[@]}"; do
      # k must reach one past the point quorum becomes unreachable, or nothing
      # fails and the panel is a flat line locating nothing.
      for k in $(seq 0 $(( $(tolerated_of "$n") + 1 ))); do
        local ini="$RES/_a1_n${n}_k${k}.ini"; common_ini "$ini" "$r"; silence_k "$ini" "$n" "$k"
        for lane in "${LANES[@]}"; do
          local sfx; sfx=$(lane_sfx "$lane")
          echo "  N=$n k=$k rep=$r $lane OFF"
          run "ab1_OFF${sfx}_n${n}_k${k}_rep${r}.log" "$(cfg_for "$n" "$lane" base)"  -f "$ini"
          echo "  N=$n k=$k rep=$r $lane ON"
          run "ab1_ON${sfx}_n${n}_k${k}_rep${r}.log"  "$(cfg_for "$n" "$lane" units)" -f "$ini"
        done
      done
    done
  done
}

# ── 3: actuated traffic light vs ours, across load ───────────────────────────
ablation3() {
  echo "### Ablation 3: baseline vs ours across N ###"
  for r in $(seq 1 "$REPS"); do
    for n in "${NS[@]}"; do
      # Both arms take the same seeded ini so they draw the SAME turn assignment
      # in a given repetition -- without it an arm replays one fixed turn draw
      # per N, has no error bars, and its line moves with whichever draw it got.
      local ini="$RES/_a3.ini"; common_ini "$ini" "$r"
      # Actuated traffic light. The AIM surveys report fixed-time as the
      # most-used comparison (46%) while calling for actuated or adaptive
      # instead, so this is the baseline the field actually asks for.
      for lane in "${LANES[@]}"; do
        local sfx; sfx=$(lane_sfx "$lane")
        echo "  N=$n rep=$r $lane trafficlight"
        run "ab3_tl${sfx}_n${n}_rep${r}.log"   "$(cfg_for "$n" "$lane" tl)"   -f "$ini"
        echo "  N=$n rep=$r $lane ours"
        run "ab3_ours${sfx}_n${n}_rep${r}.log" "$(cfg_for "$n" "$lane" base)" -f "$ini"
      done
    done
  done
}

# ── 4: priority on/off, across load ──────────────────────────────────────────
# The ambulance is the last replica, so it starts behind the queue: the effect
# is supposed to grow with traffic, which is invisible at N=4.
ablation4() {
  echo "### Ablation 4: priority on/off across N ###"
  for r in $(seq 1 "$REPS"); do
    for n in "${NS[@]}"; do
      local amb=$(( n - 1 )) idx; idx=$(replica_to_node_idx "$n" "$amb")
      local on="$RES/_a4on_n${n}.ini"; common_ini "$on" "$r"
      echo "*.node[${idx}].appl.isAmbulance = true" >> "$on"
      local off="$RES/_a4off_n${n}.ini"; common_ini "$off" "$r"
      for lane in "${LANES[@]}"; do
        local sfx; sfx=$(lane_sfx "$lane")
        echo "  N=$n rep=$r $lane priority-on"
        run "ab4_prio${sfx}_n${n}_rep${r}.log"   "$(cfg_for "$n" "$lane" base)" -f "$on"
        echo "  N=$n rep=$r $lane priority-off"
        run "ab4_noprio${sfx}_n${n}_rep${r}.log" "$(cfg_for "$n" "$lane" base)" -f "$off"
      done
    done
  done
}

# ── 5: rollback on/off on the late-emergency scenario ────────────────────────
# enableRollback=true is set in the scenario's own [Config], so a [General]
# override in a second -f file loses. The OFF arm must be a derived section.
# sim-time-limit is 90s so the ambulance physically clears; at 30s the claim
# could only ever be "re-ordered into the schedule".
ablation5() {
  echo "### Ablation 5: matched late-priority arrivals ###"
  local count total replicas lane sfx cfg r mode ini name
  # count>1 needs a bigger node[] vector than the 18-slot base network
  # provides (r18/r19 currently spawn in SUMO but never get a ResDB replica,
  # so they never clear). Keep the correctness comparison at count=1 until
  # the network topology is scaled up.
  for count in 1; do
    total=$((17 + count)); replicas=$((total + 4))
    if [[ ! -f "$FOURWAY/resdb_crypto_rb_${replicas}/server.config" ]]; then
      "$REPO/tools/gen_crypto_dir.sh" "$replicas" "$FOURWAY/resdb_crypto_rb_${replicas}" || return 1
    fi
    for lane in "${LANES[@]}"; do
      sfx=$(lane_sfx "$lane")
      if [[ "$lane" == 1lane ]]; then cfg=EighteenVehFourUnitsRollback
      else cfg=EighteenVehTwoLaneFourUnitsRollback; fi
      for r in $(seq 1 "$REPS"); do
        for mode in on off; do
          ini="$RES/_a5_${mode}${sfx}_n${total}_rep${r}.ini"
          common_ini "$ini" "$r"
          {
            echo "[Config Ab5Validated]"
            echo "extends = $cfg"
            echo "*.manager.intersectionBatchSize = $total"
            echo "*.manager.r0LateEmergencyCount = $count"
            echo "*.node[*].appl.ambulanceReplicaCount = $count"
            echo "*.node[*].appl.totalReplicas = $replicas"
            echo "*.iu[*].appl.totalReplicas = $replicas"
            echo "*.node[*].appl.resdbCryptoDir = \"resdb_crypto_rb_${replicas}\""
            echo "*.iu[*].appl.resdbCryptoDir = \"resdb_crypto_rb_${replicas}\""
            for i in 0 1 2 3; do echo "*.iu[$i].appl.replicaId = $((total+i))"; done
            if [[ "$mode" == off ]]; then
              echo "*.node[*].appl.enableRollback = false"
              echo "*.iu[*].appl.enableRollback = false"
              echo "*.node[*].appl.enableNextRound = true"
              echo "*.iu[*].appl.enableNextRound = true"
            fi
          } >> "$ini"
          name="ab5_rollback_${mode}${sfx}_n${total}_rep${r}.log"
          run "$name" Ab5Validated -f "$ini" --sim-time-limit=90s
        done
      done
    done
  done
}

case "$WHICH" in
  1) ablation1 ;; 3) ablation3 ;;
  4) ablation4 ;; 5) ablation5 ;;
  all) ablation1; ablation3; ablation4; ablation5 ;;
  *) echo "usage: run_ablations.sh <1|3|4|5|all> [reps]"; exit 1 ;;
esac
echo "ABLATION_${WHICH}_DONE results in $RES"

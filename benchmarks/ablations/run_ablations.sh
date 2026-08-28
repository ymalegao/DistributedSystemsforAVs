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
#   2  attack          — attack success AND liveness vs number of colluders
#   3  baseline        — actuated traffic light vs ours: delay and throughput vs N
#   4  priority        — ambulance wait vs queue length
#   5  rollback        — late-emergency outcome and its throughput cost
#
# Usage (inside opp_env, with veins_launchd running on :9999):
#   benchmarks/ablations/run_ablations.sh <1|2|3|4|5|all> [reps]
# ─────────────────────────────────────────────────────────────────────────────
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
FOURWAY="$REPO/scenarios/fourway"
FASTXML="$FOURWAY/config_fast.xml"
WHICH="${1:-all}"
REPS="${2:-3}"

RES="$HERE/results"
mkdir -p "$RES"

# Vehicle counts and their scenario-name prefixes.
NS=(4 8 12 16 20)
declare -A WORD=( [4]=Four [8]=Eight [12]=Twelve [16]=Sixteen [20]=Twenty )
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
# rather than leader failure, which ablation 2 covers separately).
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
  if [[ -s "$log" ]] && grep -q '^\[METRICS ' "$log"; then
    echo "    -> cached"
    return
  fi
  # LOG_FILE must be per-run: the runner used to hardcode one path, and two
  # interleaved runs produced a log with two finish markers that still parsed.
  ( cd "$FOURWAY" && LOG_FILE="$RES/.${name}.simlog" timeout 900 \
      "$REPO/tools/run-resdb-simulation.sh" -f "$FOURWAY/omnetpp.ini" "$@" \
      -u Cmdenv -c "$cfg" ) > "$log" 2>&1
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
        echo "  N=$n k=$k rep=$r OFF"; run "ab1_OFF_n${n}_k${k}_rep${r}.log" "${WORD[$n]}VehiclesResDB" -f "$ini"
        echo "  N=$n k=$k rep=$r ON";  run "ab1_ON_n${n}_k${k}_rep${r}.log"  "${WORD[$n]}VehiclesFourUnitsResDB" -f "$ini"
      done
    done
  done
}

# ── 2: attack, swept over number of colluding Byzantine proposers ────────────
# A bar of "attack committed" is 100 vs 0 and says nothing. Sweeping colluders
# to f+1 tests the assumption the firewall actually rests on, and reports
# liveness alongside safety -- refusing to commit is not the same as being fine.
# The swept value rides in the filename's k field, so here k means colluders.
ablation2() {
  echo "### Ablation 2: attack vs colluder count ###"
  for r in $(seq 1 "$REPS"); do
    for n in 4 8 16; do
      local f; f=$(f_of "$n")
      for (( c=1; c<=f+1; c++ )); do
        local ini="$RES/_a2_n${n}_c${c}.ini"; common_ini "$ini" "$r"
        # Colluders take the HIGHEST replica ids, never the lowest. CertPrimary()
        # elects the smallest certified id, so this keeps every proposer honest
        # and isolates the certificate gate from leader succession. With the
        # colluders at 0..c-1 instead, the first c leaders are all corrupt and
        # the round dies of leader starvation rather than of the attack.
        #
        # Type 7, not 6: type 6 is injected while building a proposal (see the
        # "Byzantine primary fault injection" block in ResDBDecision.cc) and so
        # does nothing at all on a replica that never leads. Type 7 lies in the
        # ARRIVAL_ANNOUNCE, which every replica sends regardless of role.
        local j idx
        for (( j=0; j<c; j++ )); do
          idx=$(replica_to_node_idx "$n" "$(( n - 1 - j ))")
          echo "*.node[${idx}].appl.isByzantine = true" >> "$ini"
          echo "*.node[${idx}].appl.byzantineType = 7"  >> "$ini"
        done
        # The arms are the ambulance cert gate on vs off, NOT --no-firewall.
        # --no-firewall disables PreVerify, which never sees this attack: type 7
        # lies in the ANNOUNCE, that lie enters a legitimate certificate, and
        # PreVerify only checks proposal-against-certificate, so the two agree
        # and nothing is flagged. enableAmbulanceCertGate is the switch that
        # actually rejects an ambulance claim carrying no certificate, so it is
        # the only on/off that makes this a controlled comparison. Written into
        # the ini directly rather than via --cert-gate, whose line lands in a
        # scenario ini that is only generated on some invocation paths.
        local gate="$RES/_a2_n${n}_c${c}_gate.ini"
        cp "$ini" "$gate"
        echo "*.node[*].appl.enableAmbulanceCertGate = true" >> "$gate"
        echo "  N=$n colluders=$c rep=$r ours (cert gate on)"
        run "ab2_ours_n${n}_k${c}_rep${r}.log"    "${WORD[$n]}VehiclesResDB" -f "$gate"
        echo "  N=$n colluders=$c rep=$r vanilla (no gate)"
        run "ab2_vanilla_n${n}_k${c}_rep${r}.log" "${WORD[$n]}VehiclesResDB" -f "$ini"
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
      echo "  N=$n rep=$r trafficlight"; run "ab3_tl_n${n}_rep${r}.log" "tl${n}veh" -f "$ini"
      echo "  N=$n rep=$r ours";     run "ab3_ours_n${n}_rep${r}.log" "${WORD[$n]}VehiclesResDB" -f "$ini"
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
      echo "  N=$n rep=$r priority-on";  run "ab4_prio_n${n}_rep${r}.log"   "${WORD[$n]}VehiclesResDB" -f "$on"
      local off="$RES/_a4off_n${n}.ini"; common_ini "$off" "$r"
      echo "  N=$n rep=$r priority-off"; run "ab4_noprio_n${n}_rep${r}.log" "${WORD[$n]}VehiclesResDB" -f "$off"
    done
  done
}

# ── 5: rollback on/off on the late-emergency scenario ────────────────────────
# enableRollback=true is set in the scenario's own [Config], so a [General]
# override in a second -f file loses. The OFF arm must be a derived section.
# sim-time-limit is 90s so the ambulance physically clears; at 30s the claim
# could only ever be "re-ordered into the schedule".
ablation5() {
  echo "### Ablation 5: rollback on/off ###"
  local offini="$HERE/ab5_off.ini"
  local on="$RES/_a5on.ini"
  # Both inis are rebuilt per repetition: they carry *.manager.seed, which is
  # what varies the turn draw between reps. Building them once before the loop
  # gave every repetition the same scenario -- and referenced $r before the loop
  # defined it, which under set -u aborted the study outright.
  for r in $(seq 1 "$REPS"); do
    {
      echo "[General]"
      echo "*.**.nic.phy80211p.analogueModels = xmldoc(\"${FASTXML}\")"
      echo "*.node[*].appl.transportPollInterval = 0.005"
      echo "*.node[*].appl.timeTickInterval      = 0.005"
      echo "*.iu[*].appl.transportPollInterval   = 0.005"
      echo "*.iu[*].appl.timeTickInterval        = 0.005"
      echo "*.manager.seed = $r"
      echo ""
      echo "[Config Ab5RollbackOff]"
      echo "extends = EighteenVehFourUnitsRollback"
      echo "*.node[*].appl.enableRollback = false"
      echo "*.iu[*].appl.enableRollback   = false"
    } > "$offini"
    common_ini "$on" "$r"
    echo "  rep=$r rollback-on"
    run "ab5_rollback_on_rep${r}.log"  EighteenVehFourUnitsRollback -f "$on"     --sim-time-limit=90s
    echo "  rep=$r rollback-off"
    run "ab5_rollback_off_rep${r}.log" Ab5RollbackOff               -f "$offini" --sim-time-limit=90s
  done
}

case "$WHICH" in
  1) ablation1 ;; 2) ablation2 ;; 3) ablation3 ;;
  4) ablation4 ;; 5) ablation5 ;;
  all) ablation1; ablation3; ablation4; ablation2; ablation5 ;;
  *) echo "usage: run_ablations.sh <1|2|3|4|5|all> [reps]"; exit 1 ;;
esac
echo "ABLATION_${WHICH}_DONE results in $RES"

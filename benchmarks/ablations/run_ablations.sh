#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# Ablation studies for the V2V-BFT intersection protocol (presentation set).
# Runs on the CURRENT branch (no merge). All 4-vehicle (fast) except #5.
#
#   1  RSU on/off          — overhead (msgs/latency) + fault tolerance (commit rate)
#   2  vanilla vs our BFT  — under fake_ambulance attack: does it commit (unsafe)?
#   3  baseline vs ours    — SUMO all-way-stop vs our protocol: total delay
#   4  priority on/off      — ambulance scheduled first vs FIFO: its wait time
#   5  rollback on/off      — late-ambulance emergency: safely handled? (slow)
#
# Usage (inside opp_env shell):
#   benchmarks/ablations/run_ablations.sh <1|2|3|4|5|all> [reps]
# Logs land in benchmarks/ablations/results/ab<N>_<arm>_k<k>_rep<r>.log
# ─────────────────────────────────────────────────────────────────────────────
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
FOURWAY="$REPO/fourway"
RES="$HERE/results"
FASTXML="$FOURWAY/config_fast.xml"
WHICH="${1:-all}"
REPS="${2:-3}"
mkdir -p "$RES"

# common overrides: fast channel + 5ms bridge poll (speed; outcome-invariant)
common_ini() {   # $1=path ; extra lines appended by caller via >>
  {
    echo "[General]"
    echo "*.**.nic.phy80211p.analogueModels = xmldoc(\"${FASTXML}\")"
    echo "*.node[*].appl.transportPollInterval = 0.005"
    echo "*.node[*].appl.timeTickInterval      = 0.005"
    echo "*.iu[*].appl.transportPollInterval   = 0.005"
    echo "*.iu[*].appl.timeTickInterval        = 0.005"
  } > "$1"
}

run() {   # $1=logname $2=config ; $3.. = extra args to run-resdb-simulation.sh
  local log="$RES/$1"; local cfg="$2"; shift 2
  ( cd "$FOURWAY" && timeout 300 ./run-resdb-simulation.sh \
      -f "$FOURWAY/omnetpp.ini" "$@" -u Cmdenv -c "$cfg" ) > "$log" 2>&1
  echo "$?"
}

# ── Ablation 1: RSU on/off ───────────────────────────────────────────────────
ablation1() {
  echo "### Ablation 1: RSU on/off (overhead + fault tolerance) ###"
  for r in $(seq 1 "$REPS"); do
    for k in 0 1 2; do
      local ini="$RES/_a1_k${k}.ini"; common_ini "$ini"
      for ((i=1;i<=k;i++)); do
        echo "*.node[${i}].appl.isByzantine = true"        >> "$ini"
        echo "*.node[${i}].appl.byzantinePbftSilent = true" >> "$ini"
      done
      rc=$(run "ab1_OFF_k${k}_rep${r}.log" FourVehiclesResDB          -f "$ini")
      echo "  OFF k=$k rep=$r rc=$rc"
      rc=$(run "ab1_ON_k${k}_rep${r}.log"  FourVehiclesFourUnitsResDB -f "$ini")
      echo "  ON  k=$k rep=$r rc=$rc"
    done
  done
}

# ── Ablation 2: vanilla vs our BFT under fake_ambulance attack ────────────────
ablation2() {
  echo "### Ablation 2: vanilla vs our BFT (fake_ambulance attack) ###"
  local ini="$RES/_a2.ini"; common_ini "$ini"
  # Byzantine leader (node[0]) proposes a fake ambulance (byzType 6)
  echo "*.node[0].appl.isByzantine = true" >> "$ini"
  echo "*.node[0].appl.byzantineType = 6"  >> "$ini"
  for r in $(seq 1 "$REPS"); do
    rc=$(run "ab2_ours_rep${r}.log"    FourVehiclesResDB -f "$ini")               # firewall ON (default)
    echo "  ours(firewall) rep=$r rc=$rc"
    rc=$(run "ab2_vanilla_rep${r}.log" FourVehiclesResDB -f "$ini" --no-firewall) # firewall OFF
    echo "  vanilla(no-fw)  rep=$r rc=$rc"
  done
}

# ── Ablation 3: SUMO all-way-stop baseline vs our protocol ────────────────────
ablation3() {
  echo "### Ablation 3: all-way-stop baseline vs our protocol (delay) ###"
  for r in $(seq 1 "$REPS"); do
    rc=$(run "ab3_baseline_rep${r}.log" baseline4veh)                  # BaselineModule, no BFT
    echo "  baseline rep=$r rc=$rc"
    local ini="$RES/_a3.ini"; common_ini "$ini"
    rc=$(run "ab3_ours_rep${r}.log"     FourVehiclesResDB -f "$ini")
    echo "  ours     rep=$r rc=$rc"
  done
}

# ── Ablation 4: priority on/off (one ambulance) ──────────────────────────────
ablation4() {
  echo "### Ablation 4: priority on/off (node[3] ambulance) ###"
  for r in $(seq 1 "$REPS"); do
    local on="$RES/_a4on.ini"; common_ini "$on"
    echo "*.node[3].appl.isAmbulance = true" >> "$on"     # gets top priority in schedule
    rc=$(run "ab4_prio_rep${r}.log"   FourVehiclesResDB -f "$on")
    echo "  priority-on  rep=$r rc=$rc"
    local off="$RES/_a4off.ini"; common_ini "$off"        # no ambulance → FIFO/normal
    rc=$(run "ab4_noprio_rep${r}.log" FourVehiclesResDB -f "$off")
    echo "  priority-off rep=$r rc=$rc"
  done
}

# ── Ablation 5: rollback on/off (late-ambulance emergency, SLOW) ──────────────
# NOTE: enableRollback=true is set in omnetpp.ini [General], so a plain -f override
# loses to first-match-wins. The OFF arm therefore uses a DERIVED config section
# (ab5_off.ini: [Config Ab5RollbackOff] extends the rollback config, sets
# enableRollback=false) — a more-specific section beats [General] regardless of order.
ablation5() {
  echo "### Ablation 5: rollback on/off (emergency scenario, slow) ###"
  local offini="$HERE/ab5_off.ini"
  {
    echo "[General]"
    echo "*.**.nic.phy80211p.analogueModels = xmldoc(\"${FASTXML}\")"
    echo "*.node[*].appl.transportPollInterval = 0.005"
    echo "*.node[*].appl.timeTickInterval      = 0.005"
    echo "*.iu[*].appl.transportPollInterval   = 0.005"
    echo "*.iu[*].appl.timeTickInterval        = 0.005"
    echo ""
    echo "[Config Ab5RollbackOff]"
    echo "extends = EighteenVehFourUnitsRollback"
    echo "*.node[*].appl.enableRollback = false"
    echo "*.iu[*].appl.enableRollback   = false"
  } > "$offini"
  for r in $(seq 1 "$REPS"); do
    local on="$RES/_a5on.ini"; common_ini "$on"
    ( cd "$FOURWAY" && timeout 900 ./run-resdb-simulation.sh -f "$FOURWAY/omnetpp.ini" -f "$on" \
        -u Cmdenv -c EighteenVehFourUnitsRollback --sim-time-limit=30s ) > "$RES/ab5_rollback_on_rep${r}.log" 2>&1
    echo "  rollback-on  rep=$r rc=$?"
    ( cd "$FOURWAY" && timeout 900 ./run-resdb-simulation.sh -f "$FOURWAY/omnetpp.ini" -f "$offini" \
        -u Cmdenv -c Ab5RollbackOff --sim-time-limit=30s ) > "$RES/ab5_rollback_off_rep${r}.log" 2>&1
    echo "  rollback-off rep=$r rc=$?"
  done
}

# ── Ablation 6: vanilla BFT (no f+1 firewall) vs our BFT — normal, cost ───────
# Option A vanilla = --no-firewall (disables the f+1 pre-verification, per the
# project's definition of vanilla BFT). Same scenario both arms; only difference is
# the firewall. Normal operation (no attack) -> isolates the COST of the addition
# (latency of the verification step); the SAFETY benefit is Ablation 2.
ablation6() {
  echo "### Ablation 6: vanilla BFT vs our BFT (normal, cost) ###"
  for r in $(seq 1 "$REPS"); do
    local ini="$RES/_a6.ini"; common_ini "$ini"
    rc=$(run "ab6_ours_rep${r}.log"    FourVehiclesResDB -f "$ini")                # firewall ON (ours)
    echo "  ours    rep=$r rc=$rc"
    rc=$(run "ab6_vanilla_rep${r}.log" FourVehiclesResDB -f "$ini" --no-firewall)  # firewall OFF (vanilla)
    echo "  vanilla rep=$r rc=$rc"
  done
}

case "$WHICH" in
  1) ablation1 ;;
  2) ablation2 ;;
  3) ablation3 ;;
  4) ablation4 ;;
  5) ablation5 ;;
  6) ablation6 ;;
  all) ablation1; ablation3; ablation4; ablation2; ablation6; ablation5 ;;
  *) echo "usage: run_ablations.sh <1|2|3|4|5|6|all> [reps]"; exit 1 ;;
esac
echo "ABLATION_${WHICH}_DONE results in $RES"

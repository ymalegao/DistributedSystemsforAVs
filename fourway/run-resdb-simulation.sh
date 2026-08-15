#!/bin/bash
# Wrapper script to run OMNeT++ ResDB simulations with portable repo-relative paths.
#
# Usage:
#   ./run-resdb-simulation.sh [sim-dir] [opp_run args...]
#   ./run-resdb-simulation.sh --randomize <N> <F> [options] [sim-dir] [opp_run args...]
#
# --randomize <N> <F>
#   Picks one ambulance and F Byzantine nodes (all FALSE_LANE) from N vehicles
#   at random per run, writes fourway/random_scenario.ini, and injects it.
#   Replica 0 is eligible as a FALSE_LANE follower unless it is reserved with
#   --byzleader or selected as the ambulance.
#
# --byzleader <ID>
#   Reserve replica <ID> as the Byzantine silent leader.
#   That replica is excluded from ambulance selection and from the C++ FALSE_LANE
#   pool so the two fault layers don't overlap.
#
# --no-ambulance
#   Do not assign any ambulance in random_scenario.ini
#   (*.node[*].appl.ambulanceReplicaId = -1).
#
# --leader <ID>
#   Set replica <ID> as the initial ResDB consensus leader by writing a
#   leader_override.ini that sets *.node[*].appl.leaderReplicaId = <ID>.
#
# --channel-metrics-dir <PATH>
#   Write channel_metrics_override.ini so *.node[*].appl.channelMetricsCsvDir
#   points at PATH (absolute path recommended). Used by experiment_orchestrator.py
#   to place channel_*.csv / sinr_*.csv under benchmarks/.../run_<rep>/.
#
# --tolerated-f <F>
#   Configure the intended consensus fault-tolerance point for fixed-physical-N
#   latency-curve experiments. When used with --randomize, writes
#   *.node[*].appl.toleratedFaults = F into random_scenario.ini.
#
# --baseline
#   SUMO-controlled baseline mode. Reuses ambulance randomization, but disables
#   Byzantine and leader mutations.
#
# --rollback-late-emergency
#   Fixed 18-replica rollback scenario: veh0..veh15 commit epoch 0, two cars
#   clear the intersection, then the manager injects veh16 (late normal car)
#   and veh17 (late ambulance), forcing rollback to M=16. Rollback and
#   ambulance cert gate are enabled, and the 18-vehicle rollback launch config
#   is used.
#
# --crash-wait-clear
#   Scenario 16 Stage 1a harness: 16 vehicles commit ORDER(0), then two
#   batch-0 vehicles are TraCI-force-frozen in the conflict box with radios
#   silenced after a MAC grace window, and towed after clearDelaySec.
#   Uses the normal 16-veh launchd; collision.action=none is in bft_16veh.sumo.cfg.
#
# --suppress-initial-cancel-leader
#   Protocol-surface fault injection for Scenario 15/16: the deterministic
#   round-0 CANCEL proposer withholds the proposal after a valid f+1 cert.
#
# --disable-cancel-leader-failover
#   Ablation paired with the suppression attack. Honest replicas do not arm
#   the shared proposer lease, demonstrating the unguarded liveness failure.
#
# --fabricate-clearance
#   Scenario-16 Byzantine recovery leader submits ORDER(e+1) while the crash
#   incident is still BLOCKING, with an invalid zero-echo CLEAR certificate.
#
# --disable-recovery-clear-evidence-gate
#   Ablation paired with --fabricate-clearance. Voters accept the structurally
#   present CLEAR trailer without validating its threshold signatures.
#
# Examples:
#   ./run-resdb-simulation.sh -u Cmdenv -c FourVehiclesResDB
#   ./run-resdb-simulation.sh --randomize 12 3 -u Cmdenv -c TwelveVehiclesResDB
#   ./run-resdb-simulation.sh --randomize 16 4 --byzleader 0 -u Cmdenv -c SixteenVehiclesResDB
#   ./run-resdb-simulation.sh --leader 4 -u Cmdenv -c EightVehiclesResDB
#   ./run-resdb-simulation.sh --leader 7 --randomize 8 2 -u Cmdenv -c EightVehiclesResDB
#   ./run-resdb-simulation.sh --randomize 16 4 --no-firewall -u Cmdenv -c SixteenVehiclesResDB
#   ./run-resdb-simulation.sh --randomize 16 0 --byzleader 0 --leader-byz-type 6 --no-firewall -u Cmdenv -c SixteenVehiclesResDB
#   ./run-resdb-simulation.sh --randomize 20 0 --tolerated-f 1 -u Cmdenv -c TwentyVehiclesResDB

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

export VEINS_ROOT="${VEINS_ROOT:-${REPO_ROOT}/veins-veins-5.3.1}"
export FOURWAY_ROOT="${FOURWAY_ROOT:-${REPO_ROOT}/fourway}"

SIM_PID=""
_CLEANING_UP=0

# Ctrl-C / TERM must kill ./fourway even when it is wedged in OMNeT's
# crash signal handler (TERM/INT are ignored there; only KILL works).
cleanup() {
  if [[ "${_CLEANING_UP}" -eq 1 ]]; then
    return
  fi
  _CLEANING_UP=1
  trap - EXIT INT TERM HUP

  # SIM_PID is cleared only after a clean wait(); if it is still set we were
  # interrupted (or the binary is wedged) — force-kill and sweep leftovers.
  local tracked="${SIM_PID:-}"
  if [[ -n "${tracked}" ]]; then
    if kill -0 "${tracked}" 2>/dev/null; then
      echo ""
      echo "Interrupted — force-killing simulation PID ${tracked} (and children)..."
      pkill -KILL -P "${tracked}" 2>/dev/null || true
      kill -KILL "${tracked}" 2>/dev/null || true
    fi
    if [[ -n "${SIM_DIR:-}" ]]; then
      pkill -KILL -f "${SIM_DIR}/fourway" 2>/dev/null || true
      pkill -KILL -f "${SIM_DIR}/veins" 2>/dev/null || true
    fi
    pkill -KILL -f '[.]/fourway( |$)' 2>/dev/null || true
    SIM_PID=""
  fi

  echo ""
  echo "=========================================="
  echo "Simulation ended (exit/interrupt). Full log: ${LOG_FILE:-/tmp/resdb-simulation.log}"
  echo "=========================================="
}

# Run sim in background so we own its PID (pipe-to-tee foreground
# otherwise leaves fourway alive after Ctrl-C).
run_sim() {
  "$@" > >(tee -a "${LOG_FILE}") 2>&1 &
  SIM_PID=$!
  set +e
  wait "${SIM_PID}"
  local rc=$?
  set -e
  SIM_PID=""
  return "${rc}"
}


infer_omnetpp_root() {
    if [[ -n "${OMNETPP_ROOT:-}" ]]; then
        return
    fi
    if [[ -n "${__omnetpp_root_dir:-}" ]]; then
        OMNETPP_ROOT="${__omnetpp_root_dir}"
        return
    fi
    if command -v opp_configfilepath >/dev/null 2>&1; then
        local configfile
        configfile="$(opp_configfilepath 2>/dev/null || true)"
        if [[ -n "${configfile}" ]]; then
            OMNETPP_ROOT="$(cd "$(dirname "${configfile}")" && pwd)"
        fi
    fi
}

find_omnetpp_runtime_lib() {
    local candidate
    for candidate in \
        "${OMNETPP_ROOT}/lib/liboppsim_dbg.dylib" \
        "${OMNETPP_ROOT}/lib/liboppsim.dylib" \
        "${OMNETPP_ROOT}/lib/liboppsim_dbg.so" \
        "${OMNETPP_ROOT}/lib/liboppsim.so"
    do
        if [[ -f "${candidate}" ]]; then
            printf '%s\n' "${candidate}"
            return 0
        fi
    done
    return 1
}

append_ld_library_path() {
    local extra_path="$1"
    if [[ -z "${extra_path}" ]]; then
        return
    fi
    if [[ -n "${LD_LIBRARY_PATH:-}" ]]; then
        export LD_LIBRARY_PATH="${extra_path}:${LD_LIBRARY_PATH}"
    else
        export LD_LIBRARY_PATH="${extra_path}"
    fi
    # macOS dyld does not use LD_LIBRARY_PATH. Nix-built OMNeT++ / mixed OpenSSL without
    # this yields dyld: missing symbol called (exit 134) when loading libveins / bridge.
    if [[ "$(uname -s)" == "Darwin" ]]; then
        if [[ -n "${DYLD_LIBRARY_PATH:-}" ]]; then
            export DYLD_LIBRARY_PATH="${extra_path}:${DYLD_LIBRARY_PATH}"
        else
            export DYLD_LIBRARY_PATH="${extra_path}"
        fi
    fi
}

has_ned_path_arg() {
    local arg
    for arg in "$@"; do
        if [[ "${arg}" == "-n" || "${arg}" == "--ned-path" ]]; then
            return 0
        fi
    done
    return 1
}

# ---------------------------------------------------------------------------
# Random scenario generation
# ---------------------------------------------------------------------------
# Writes fourway/random_scenario.ini with randomized ambulance + Byzantine
# node assignments.  The file uses [General] so it applies to any config.
# Prints the path to the generated ini file on stdout; all other output goes
# to stderr so callers can safely capture the path with $(...).
# Usage: generate_random_scenario <N> <F> <sim_dir> <byz_leader|-1> <allow_r0_follower> <no_ambulance> <leader_byz_type> <follower_byz_type>
#   byz_leader:        replica ID reserved as silent leader (-1 = none)
#   allow_r0_follower: legacy compatibility flag; replica 0 is allowed by default
#   no_ambulance:      "1" = force no ambulance (ambulanceReplicaId=-1)
#   leader_byz_type:   byzantineType for the byz leader (default 5=bad_proposal; 6=fake_ambulance)
#   follower_byz_type: byzantineType for Byzantine followers (default 1=false_lane; 7=fake_ambulance_follower)
# ---------------------------------------------------------------------------
generate_random_scenario() {
    local n="$1"
    local f="$2"
    local sim_dir="$3"
    local byz_leader="$4"   # -1 means no byz leader
    local allow_r0_follower="${5:-0}"
    local no_ambulance="${6:-0}"
    local leader_byz_type="${7:-5}"
    local follower_byz_type="${8:-1}"

    local out_ini="${sim_dir}/random_scenario.ini"

    # Pure-bash random sampling — no Python subprocess needed, avoids LD_LIBRARY_PATH
    # conflicts between Nix libs and the system Python binary.

    # ---------------------------------------------------------------------------
    # replica_to_node_idx <n> <replica_id>
    # OMNeT++ orders node[] by SUMO vehicle ID lexicographically.
    # For n<=9 the order is numeric, so node[i]=vehi.
    # For n>9 it differs: e.g. n=16 → veh10<veh2, so node[2]=veh10 (replica 10).
    # This function counts how many veh{j} (j≠replica, 0≤j<n) sort before veh{replica}.
    # ---------------------------------------------------------------------------
    replica_to_node_idx() {
        local _n="$1" _r="$2"
        local _count=0 _i
        local _target="veh${_r}"
        for (( _i=0; _i<_n; _i++ )); do
            [[ $_i -ne $_r && "veh${_i}" < "${_target}" ]] && (( _count++ ))
        done
        echo $_count
    }

    # 1. Pick ambulance (replica ID) from all replicas except the byz leader,
    #    unless --no-ambulance is set.
    local AMB_ID
    if [[ "${no_ambulance}" == "1" ]]; then
        AMB_ID=-1
    elif [[ "${byz_leader}" -ge 0 ]]; then
        local amb_pool=()
        local k
        for (( k=0; k<n; k++ )); do
            [[ $k -ne $byz_leader ]] && amb_pool+=("$k")
        done
        AMB_ID=${amb_pool[$(( RANDOM % ${#amb_pool[@]} ))]}
    else
        AMB_ID=$(( RANDOM % n ))
    fi

    # 2. Build FALSE_LANE candidate pool (replica IDs): exclude ambulance and byz leader.
    #    Replica 0 is now a valid follower fault candidate; cert-primary leadership
    #    no longer assumes replica 0 is the normal proposer.
    #    Fisher-Yates shuffle, take first F.
    local available=()
    local i
    for (( i=0; i<n; i++ )); do
        [[ $i -eq $byz_leader ]] && continue
        if [[ "${AMB_ID}" -ge 0 && $i -eq $AMB_ID ]]; then
            continue
        fi
        available+=("$i")
    done
    local avail_len=${#available[@]}
    local pick=$(( f < avail_len ? f : avail_len ))
    if (( f > avail_len )); then
        echo "WARNING: Requested F=${f} Byzantine followers but only ${avail_len} candidate(s) after exclusions; using ${pick}." >&2
    fi
    for (( i=0; i<pick; i++ )); do
        local j=$(( i + RANDOM % (avail_len - i) ))
        local tmp=${available[$i]}
        available[$i]=${available[$j]}
        available[$j]=$tmp
    done
    local BYZ_ARRAY=()
    if (( pick > 0 )); then
        BYZ_ARRAY=("${available[@]:0:$pick}")
    fi
    local BYZ_IDS=""
    if ((${#BYZ_ARRAY[@]} > 0)); then
        BYZ_IDS="${BYZ_ARRAY[*]}"   # space-separated replica IDs
    fi

    echo "==========================================" >&2
    echo "Randomized scenario:" >&2
    if [[ "${AMB_ID}" -ge 0 ]]; then
        echo "  Ambulance node  : ${AMB_ID} (veh${AMB_ID})" >&2
    else
        echo "  Ambulance node  : none" >&2
    fi
    if [[ "${byz_leader}" -ge 0 ]]; then
        echo "  Byzantine leader: replica ${byz_leader} (C++ FALSE_LANE)" >&2
    else
        echo "  Byzantine leader: none" >&2
        echo "  FALSE_LANE pool: replica 0 eligible as follower" >&2
    fi
    echo "  Byzantine nodes : ${BYZ_IDS:-none} (C++ FALSE_LANE, replica IDs)" >&2
    echo "  Output ini      : ${out_ini}" >&2
    echo "==========================================" >&2

    # Write the OMNeT++ ini override file.
    # Reset all nodes first so stale Byzantine settings from the base config are cleared.
    # IMPORTANT: isByzantine is set by OMNeT++ node[] INDEX, not replica ID.
    # For n>9 these differ because OMNeT++ sorts nodes by SUMO ID lexicographically.
    # Use replica_to_node_idx to convert before writing.
    {
        echo "# Auto-generated by run-resdb-simulation.sh --randomize"
        echo "# Do not edit by hand — regenerated each run."
        echo "[General]"
        echo "*.node[*].appl.ambulanceReplicaId = ${AMB_ID}"
        local colluder_csv=""
        if (( follower_byz_type == 1 && pick > 0 )); then
            local colluder_id
            for colluder_id in "${BYZ_ARRAY[@]}"; do
                [[ -n "${colluder_csv}" ]] && colluder_csv+=","
                colluder_csv+="${colluder_id}"
            done
        fi
        echo "*.node[*].appl.falseLaneColluderIds = \"${colluder_csv}\""
        if [[ "${DISABLE_ARRIVAL_POSITION_GATE}" -eq 1 ]]; then
            echo "*.node[*].appl.enableArrivalPositionGate = false"
        fi
        if [[ -n "${TOLERATED_F:-}" ]]; then
            echo "*.node[*].appl.toleratedFaults = ${TOLERATED_F}"
        fi
        # Byz leader: Byzantine at C++ layer
        if [[ "${byz_leader}" -ge 0 ]]; then
            local leader_node
            leader_node="$(replica_to_node_idx "${n}" "${byz_leader}")"
            echo "*.node[${leader_node}].appl.isByzantine = true"
            echo "*.node[${leader_node}].appl.byzantineType = ${leader_byz_type}   # byz leader replica ${byz_leader}"
        fi
        # FALSE_LANE nodes (C++ arrival layer only)
        if (( pick > 0 )); then
            for replica_id in "${BYZ_ARRAY[@]}"; do
                local node_idx
                node_idx="$(replica_to_node_idx "${n}" "${replica_id}")"
                echo "*.node[${node_idx}].appl.isByzantine = true"
                echo "*.node[${node_idx}].appl.byzantineType = ${follower_byz_type}   # follower byz type (replica ${replica_id})"
                if (( follower_byz_type != 1 )); then
                    echo "*.node[${node_idx}].appl.byzantinePbftSilent = true   # PBFT omission fault (replica ${replica_id})"
                fi
            done
        fi
        # Cert gate override (written when --cert-gate is passed)
        if [[ -n "${CERT_GATE_LINE:-}" ]]; then
            echo "${CERT_GATE_LINE}"
        fi
    } > "${out_ini}"

    # Only the ini path goes to stdout so $(...) captures just the path
    printf '%s\n' "${out_ini}"
}

# ---------------------------------------------------------------------------
# Argument parsing: strip our flags before forwarding the rest to opp_run
# ---------------------------------------------------------------------------
RANDOMIZE=0
RANDOMIZE_N=""
RANDOMIZE_F=""
BYZ_LEADER=-1       # -1 = no designated byz leader
BYZ_LEADER_TYPE=5    # byzantineType for the byz leader (5=bad_proposal, 6=fake_ambulance)
BYZ_FOLLOWER_TYPE=1  # byzantineType for Byzantine followers (1=false_lane, 7=fake_ambulance_follower)
CERT_GATE_LINE=""   # set by --cert-gate: adds enableAmbulanceCertGate=true to scenario ini
DISABLE_ARRIVAL_POSITION_GATE=0
ALLOW_REPLICA0_BYZ_FOLLOWER=1
NO_AMBULANCE=0
INITIAL_LEADER=""   # "" = use default (replica 0)
CHANNEL_METRICS_DIR=""  # "" = do not override (use omnetpp.ini / NED default)
BASELINE=0
ROLLBACK_LATE_EMERGENCY=0
CRASH_WAIT_CLEAR=0
SUPPRESS_INITIAL_CANCEL_LEADER=0
ENABLE_CANCEL_LEADER_FAILOVER=1
FABRICATE_CLEARANCE=0
ENABLE_RECOVERY_CLEAR_EVIDENCE_GATE=1
TOLERATED_F=""
EXTRA_INI_ARG=()

args=("$@")
filtered_args=()
i=0
while [[ $i -lt ${#args[@]} ]]; do
    case "${args[$i]}" in
        --randomize)
            RANDOMIZE=1
            i=$(( i + 1 ))
            RANDOMIZE_N="${args[$i]}"
            i=$(( i + 1 ))
            RANDOMIZE_F="${args[$i]}"
            ;;
        --byzleader)
            i=$(( i + 1 ))
            BYZ_LEADER="${args[$i]}"
            ;;
        --leader-byz-type)
            i=$(( i + 1 ))
            BYZ_LEADER_TYPE="${args[$i]}"
            ;;
        --follower-byz-type)
            i=$(( i + 1 ))
            BYZ_FOLLOWER_TYPE="${args[$i]}"
            ;;
        --no-firewall)
            export RESDB_NO_FIREWALL=1
            ;;
        --cert-gate)
            CERT_GATE_LINE="*.node[*].appl.enableAmbulanceCertGate = true"
            ;;
        --disable-arrival-position-gate)
            DISABLE_ARRIVAL_POSITION_GATE=1
            ;;
        --allow-replica0-byz-follower)
            ALLOW_REPLICA0_BYZ_FOLLOWER=1
            ;;
        --no-ambulance)
            NO_AMBULANCE=1
            ;;
        --leader)
            i=$(( i + 1 ))
            INITIAL_LEADER="${args[$i]}"
            ;;
        --channel-metrics-dir)
            i=$(( i + 1 ))
            CHANNEL_METRICS_DIR="${args[$i]}"
            ;;
        --tolerated-f)
            i=$(( i + 1 ))
            TOLERATED_F="${args[$i]}"
            ;;
        --baseline)
            BASELINE=1
            ;;
        --rollback-late-emergency)
            ROLLBACK_LATE_EMERGENCY=1
            ;;
        --crash-wait-clear)
            CRASH_WAIT_CLEAR=1
            ;;
        --suppress-initial-cancel-leader)
            SUPPRESS_INITIAL_CANCEL_LEADER=1
            ;;
        --disable-cancel-leader-failover)
            ENABLE_CANCEL_LEADER_FAILOVER=0
            ;;
        --fabricate-clearance)
            FABRICATE_CLEARANCE=1
            ;;
        --disable-recovery-clear-evidence-gate)
            ENABLE_RECOVERY_CLEAR_EVIDENCE_GATE=0
            ;;
        *)
            filtered_args+=("${args[$i]}")
            ;;
    esac
    i=$(( i + 1 ))
done
set -- "${filtered_args[@]+"${filtered_args[@]}"}"

if [[ "${BASELINE}" -eq 1 ]]; then
    BYZ_LEADER=-1
    ALLOW_REPLICA0_BYZ_FOLLOWER=1
    INITIAL_LEADER=""
fi

if [[ "${ROLLBACK_LATE_EMERGENCY}" -eq 1 && "${CRASH_WAIT_CLEAR}" -eq 1 ]]; then
    echo "ERROR: --rollback-late-emergency and --crash-wait-clear are mutually exclusive." >&2
    exit 1
fi
if [[ "${SUPPRESS_INITIAL_CANCEL_LEADER}" -eq 1 &&
      "${ROLLBACK_LATE_EMERGENCY}" -ne 1 && "${CRASH_WAIT_CLEAR}" -ne 1 ]]; then
    echo "ERROR: --suppress-initial-cancel-leader requires a rollback/crash scenario." >&2
    exit 1
fi
if [[ "${FABRICATE_CLEARANCE}" -eq 1 && "${CRASH_WAIT_CLEAR}" -ne 1 ]]; then
    echo "ERROR: --fabricate-clearance requires --crash-wait-clear." >&2
    exit 1
fi
if [[ "${ENABLE_RECOVERY_CLEAR_EVIDENCE_GATE}" -eq 0 &&
      "${FABRICATE_CLEARANCE}" -ne 1 ]]; then
    echo "ERROR: --disable-recovery-clear-evidence-gate requires --fabricate-clearance." >&2
    exit 1
fi

# ---------------------------------------------------------------------------

infer_omnetpp_root
if [[ -z "${OMNETPP_ROOT:-}" ]]; then
    echo "ERROR: OMNETPP_ROOT is not set and could not be inferred." >&2
    echo "Source the OMNeT++ shell environment or export OMNETPP_ROOT before running this script." >&2
    exit 1
fi
export OMNETPP_ROOT

NIX_LIB_PATHS=""
if OMNETPP_RUNTIME_LIB="$(find_omnetpp_runtime_lib)"; then
    if [[ "$(uname -s)" == "Darwin" ]]; then
        NIX_LIB_PATHS="$(otool -L "${OMNETPP_RUNTIME_LIB}" 2>/dev/null | \
            awk '/\/nix\/store\// {print $1}' | \
            xargs -n1 dirname 2>/dev/null | \
            sort -u | \
            tr '\n' ':')"
    else
        NIX_LIB_PATHS="$(ldd "${OMNETPP_RUNTIME_LIB}" 2>/dev/null | \
            grep /nix/store | \
            sed 's/.*=> //' | \
            sed 's/ (0x.*//' | \
            xargs -r -n1 dirname | \
            sort -u | \
            tr '\n' ':')"
    fi
else
    echo "ERROR: Could not find liboppsim(_dbg).so or liboppsim(_dbg).dylib under ${OMNETPP_ROOT}/lib" >&2
    exit 1
fi

if [[ -n "${NIX_LIB_PATHS}" ]]; then
    append_ld_library_path "${NIX_LIB_PATHS%:}"
fi

# ResDB OMNeT bridge (same default as Veins makefrag; override with RESDB_ROOT).
_RESDB_ROOT="${RESDB_ROOT:-${REPO_ROOT}/incubator-resilientdb}"
_RESDB_LIB_DIR="${_RESDB_ROOT}/bazel-bin/integration/omnet"
if [[ -f "${_RESDB_LIB_DIR}/libresdb_omnet_bridge.dylib" || -f "${_RESDB_LIB_DIR}/libresdb_omnet_bridge.so" ]]; then
    append_ld_library_path "${_RESDB_LIB_DIR}"
fi

if [[ $# -eq 0 || "${1}" == -* ]]; then
    SIM_DIR="${FOURWAY_ROOT}"
else
    SIM_DIR="$1"
    shift
fi

if [[ ! -d "${SIM_DIR}" ]]; then
    echo "ERROR: Simulation directory not found: ${SIM_DIR}" >&2
    exit 1
fi

# Drop generated overlays this invocation will not recreate, so a prior scenario
# (e.g. scenario 15's rollback_late_emergency.ini) cannot linger on disk and
# get re-applied by a later -f chain or a manual opp_run.
[[ "${RANDOMIZE}" -eq 1 ]] || rm -f "${SIM_DIR}/random_scenario.ini"
[[ "${ROLLBACK_LATE_EMERGENCY}" -eq 1 ]] || rm -f "${SIM_DIR}/rollback_late_emergency.ini"
[[ "${CRASH_WAIT_CLEAR}" -eq 1 ]] || rm -f "${SIM_DIR}/crash_wait_clear.ini"
[[ -n "${INITIAL_LEADER}" ]] || rm -f "${SIM_DIR}/leader_override.ini"
[[ -n "${CHANNEL_METRICS_DIR}" ]] || rm -f "${SIM_DIR}/channel_metrics_override.ini"

# Generate random scenario after SIM_DIR is known
if [[ "${RANDOMIZE}" -eq 1 ]]; then
    if [[ -z "${RANDOMIZE_N}" || -z "${RANDOMIZE_F}" ]]; then
        echo "ERROR: --randomize requires <N> <F> arguments" >&2
        exit 1
    fi
    if [[ -n "${TOLERATED_F}" ]]; then
        if ! [[ "${TOLERATED_F}" =~ ^[0-9]+$ ]]; then
            echo "ERROR: --tolerated-f requires a non-negative integer." >&2
            exit 1
        fi
        MAX_VALID_F=$(( (RANDOMIZE_N - 1) / 3 ))
        if (( TOLERATED_F > MAX_VALID_F )); then
            echo "ERROR: --tolerated-f ${TOLERATED_F} invalid for N=${RANDOMIZE_N}; require f <= floor((N-1)/3)=${MAX_VALID_F}." >&2
            exit 1
        fi
        TOLERATED_Q=$(( (RANDOMIZE_N + TOLERATED_F + 2) / 2 ))
        if (( TOLERATED_Q > RANDOMIZE_N )); then
            echo "ERROR: --tolerated-f ${TOLERATED_F} yields quorum q(N,f)=${TOLERATED_Q} > N=${RANDOMIZE_N}." >&2
            exit 1
        fi
    fi
    # Seed bash $RANDOM in this process for generate_random_scenario. Bash only
    # honors a RANDOM assignment made inside its own running process; an
    # inherited/exported RANDOM from a parent shell does not reseed a freshly
    # started bash, so the orchestrator passes the seed via SCENARIO_RANDOM_SEED
    # and we assign it to RANDOM here, immediately before the draw.
    if [[ -n "${SCENARIO_RANDOM_SEED:-}" ]]; then
        RANDOM="${SCENARIO_RANDOM_SEED}"
    fi
    RANDOM_INI="$(generate_random_scenario "${RANDOMIZE_N}" "${RANDOMIZE_F}" "${SIM_DIR}" "${BYZ_LEADER}" "${ALLOW_REPLICA0_BYZ_FOLLOWER}" "${NO_AMBULANCE}" "${BYZ_LEADER_TYPE}" "${BYZ_FOLLOWER_TYPE}")"
    # When any -f flag is given, OMNeT++ stops auto-loading omnetpp.ini.
    # Explicitly load omnetpp.ini first, then the override file so it wins.
    EXTRA_INI_ARG=(-f "omnetpp.ini" -f "${RANDOM_INI}")
elif [[ -n "${TOLERATED_F}" ]]; then
    echo "ERROR: --tolerated-f currently requires --randomize so toleratedFaults can be written into random_scenario.ini." >&2
    exit 1
fi

if [[ "${ROLLBACK_LATE_EMERGENCY}" -eq 1 ]]; then
    ROLLBACK_INI="${SIM_DIR}/rollback_late_emergency.ini"
    {
        echo "# Auto-generated by run-resdb-simulation.sh --rollback-late-emergency"
        echo "# Do not edit by hand — regenerated each run."
        # This file is loaded while -c EighteenVehiclesResDB is active.  Put
        # overrides in that named section: its inherited sim-time-limit takes
        # precedence over the same key in [General].
        echo "[Config EighteenVehiclesResDB]"
        # Honest/guarded recovery gets the normal 120s bound. Scenario 17 is a
        # deliberately stalled liveness ablation, so record a bounded 60s
        # observation window instead of waiting for its impossible departure
        # predicate.
        if [[ "${ENABLE_CANCEL_LEADER_FAILOVER}" -eq 0 ]]; then
            echo "sim-time-limit = 60s"
        else
            echo "sim-time-limit = 120s"
        fi
        echo "*.manager.launchConfig = xmldoc(\"resdb_bft_18veh_rollback_late.launchd.xml\")"
        # All 16 original vehicles plus the two dynamically spawned vehicles
        # must reach a terminal departure.  A target of 16 let the manager end
        # the run before veh16/veh17 reached the stop zone, so the honest
        # scenario recorded no CANCEL and no recovery ORDER at all.
        echo "*.manager.intersectionBatchSize = 18"
        echo "*.manager.enableR0Supervisor = true"
        echo "*.manager.r0SpawnAfterCleared = 1"
        echo "*.manager.lateEmergencyDeltaSec = 0s"
        echo "*.manager.r0LateSpawnDepartPos = 200m"
        echo "*.manager.r0LateSpawnRetrySec = 0.2s"
        echo "*.manager.r0LateSpawnMaxRetries = 25"
        echo "*.manager.r0LateNormalVehicleId = \"veh16\""
        echo "*.manager.r0LateNormalType = \"car\""
        echo "*.manager.r0LateNormalRoute = \"rN\""
        echo "*.manager.r0LateEmergencyVehicleId = \"veh17\""
        echo "*.manager.r0LateEmergencyType = \"ambulance\""
        echo "*.manager.r0LateEmergencyRoute = \"rE\""
        # totalVehicles=16 (NOT 18) so epoch 0's proposeAll does not QUIET-pad the
        # late vehicle slots: the padding loop runs rid in [0, totalVehicles).
        # Late replicas 16/17 still get correct ids from NED node[16]/node[17].replicaId
        # because they spawn last (node index == veh number).
        echo "*.node[*].appl.totalVehicles = 16"
        echo "*.node[*].appl.toleratedFaults = 5"
        echo "*.node[*].appl.rollbackFaultMode = \"per_epoch\""
        echo "*.node[*].appl.ambulanceReplicaId = 17"
        echo "*.node[*].appl.enableRollback = true"
        echo "*.node[*].appl.enableAmbulanceCertGate = true"
        echo "*.node[*].appl.cancelCertRetryIntervalSec = 0.1s"
        echo "*.node[*].appl.cancelCertRetryMax = 20"
        echo "*.node[*].appl.rollbackVcTimeoutSec = 8s"
        if [[ "${SUPPRESS_INITIAL_CANCEL_LEADER}" -eq 1 ]]; then
            echo "*.node[*].appl.injectSuppressInitialCancelLeader = true"
        fi
        if [[ "${ENABLE_CANCEL_LEADER_FAILOVER}" -eq 0 ]]; then
            echo "*.node[*].appl.enableCancelLeaderFailover = false"
        fi
        echo "*.node[*].appl.brakingDecelMps2 = 4.5"
        echo "*.node[*].appl.processingLatencyMargin = 2.0"
    } > "${ROLLBACK_INI}"
    echo "  Rollback late-emergency scenario: 16 commit, inject veh16 normal + veh17 ambulance after 1 vehicle clears (rollback_late_emergency.ini)" >&2
    if [[ ${#EXTRA_INI_ARG[@]} -gt 0 ]]; then
        EXTRA_INI_ARG+=(-f "${ROLLBACK_INI}")
    else
        EXTRA_INI_ARG=(-f "omnetpp.ini" -f "${ROLLBACK_INI}")
    fi
fi

if [[ "${CRASH_WAIT_CLEAR}" -eq 1 ]]; then
    CRASH_INI="${SIM_DIR}/crash_wait_clear.ini"
    {
        echo "# Auto-generated by run-resdb-simulation.sh --crash-wait-clear"
        echo "# Do not edit by hand — regenerated each run."
        echo "#"
        echo "# IMPORTANT: overrides must live under [Config SixteenVehiclesResDB],"
        echo "# not [General]. OMNeT++ gives named config sections higher priority"
        echo "# than General."
        echo "[Config SixteenVehiclesResDB]"
        echo "sim-time-limit = 120s"
        echo "*.manager.enableCrashSupervisor = true"
        echo "*.manager.crashWreckCount = 2"
        echo "*.manager.crashPollPeriodSec = 0.1s"
        echo "*.manager.crashOnBoxEntrySec = 1s"
        echo "*.manager.clearDelaySec = 30s"
        echo "*.node[*].appl.totalVehicles = 16"
        echo "*.node[*].appl.toleratedFaults = 5"
        echo "*.node[*].appl.ambulanceReplicaId = -1"
        echo "*.node[*].appl.enableRollback = true"
        echo "*.node[*].appl.crashMacGraceSec = 0.2s"
        if [[ "${SUPPRESS_INITIAL_CANCEL_LEADER}" -eq 1 ]]; then
            echo "*.node[*].appl.injectSuppressInitialCancelLeader = true"
        fi
        if [[ "${ENABLE_CANCEL_LEADER_FAILOVER}" -eq 0 ]]; then
            echo "*.node[*].appl.enableCancelLeaderFailover = false"
        fi
        if [[ "${FABRICATE_CLEARANCE}" -eq 1 ]]; then
            echo "*.node[*].appl.injectFabricatedClearanceLeader = true"
        fi
        if [[ "${ENABLE_RECOVERY_CLEAR_EVIDENCE_GATE}" -eq 0 ]]; then
            echo "*.node[*].appl.enableRecoveryClearEvidenceGate = false"
        fi
    } > "${CRASH_INI}"
    echo "  Crash wait-clear scenario: 16 commit ORDER(0), freeze two batch-0 wrecks, tow after clearDelaySec (crash_wait_clear.ini)" >&2
    if [[ ${#EXTRA_INI_ARG[@]} -gt 0 ]]; then
        EXTRA_INI_ARG+=(-f "${CRASH_INI}")
    else
        EXTRA_INI_ARG=(-f "omnetpp.ini" -f "${CRASH_INI}")
    fi
fi

# Write a leader_override.ini to set the C++ PROPOSE_ALL leader if --leader was given.
# The C++ amITheLeader() checks configuredLeaderReplicaId (NED param leaderReplicaId),
# which is set here via OMNeT++ ini override.
LEADER_INI=""
if [[ -n "${INITIAL_LEADER}" ]]; then
    LEADER_INI="${SIM_DIR}/leader_override.ini"
    {
        echo "# Auto-generated by run-resdb-simulation.sh --leader"
        echo "# Do not edit by hand — regenerated each run."
        echo "[General]"
        echo "*.node[*].appl.leaderReplicaId = ${INITIAL_LEADER}"
    } > "${LEADER_INI}"
    echo "  Set PROPOSE_ALL leader: replica ${INITIAL_LEADER} (leader_override.ini)"
    # Inject the leader ini. If --randomize already set EXTRA_INI_ARG, append to it;
    # otherwise start with omnetpp.ini (required when any -f flag is used).
    if [[ ${#EXTRA_INI_ARG[@]} -gt 0 ]]; then
        EXTRA_INI_ARG+=(-f "${LEADER_INI}")
    else
        EXTRA_INI_ARG=(-f "omnetpp.ini" -f "${LEADER_INI}")
    fi
fi

# Channel metrics CSV/SINR output directory (overrides omnetpp.ini; applied last).
if [[ -n "${CHANNEL_METRICS_DIR}" ]]; then
    CM_INI="${SIM_DIR}/channel_metrics_override.ini"
    {
        echo "# Auto-generated by run-resdb-simulation.sh --channel-metrics-dir"
        echo "# Do not edit by hand — regenerated each run."
        echo "[General]"
        # OMNeT++ string parameter; quote path for spaces/special chars.
        printf '*.node[*].appl.channelMetricsCsvDir = "%s"\n' "${CHANNEL_METRICS_DIR}"
    } > "${CM_INI}"
    echo "  Channel metrics CSV dir: ${CHANNEL_METRICS_DIR} (channel_metrics_override.ini)" >&2
    if [[ ${#EXTRA_INI_ARG[@]} -gt 0 ]]; then
        EXTRA_INI_ARG+=(-f "${CM_INI}")
    else
        EXTRA_INI_ARG=(-f "omnetpp.ini" -f "${CM_INI}")
    fi
fi

# Overridable so concurrent runs (e.g. the golden harness) do not clobber one
# another's log; line 120 already reads it defensively.
LOG_FILE="${LOG_FILE:-/tmp/resdb-simulation.log}"
trap cleanup EXIT INT TERM HUP

> "${LOG_FILE}"
echo "=========================================="
echo "Running OMNeT++ ResDB Simulation"
echo "=========================================="
echo "Repo root: ${REPO_ROOT}"
echo "OMNeT++ root: ${OMNETPP_ROOT}"
echo "VEINS_ROOT: ${VEINS_ROOT}"
echo "Simulation dir: ${SIM_DIR}"
if [[ -n "${NIX_LIB_PATHS}" ]]; then
    echo "Loader path: prepended discovered Nix dirs (LD_LIBRARY_PATH; DYLD_LIBRARY_PATH on macOS)"
else
    echo "No Nix runtime paths detected from liboppsim; using existing loader paths"
fi
if [[ -f "${_RESDB_LIB_DIR}/libresdb_omnet_bridge.dylib" || -f "${_RESDB_LIB_DIR}/libresdb_omnet_bridge.so" ]]; then
    echo "Loader path: prepended ResDB bridge dir ${_RESDB_LIB_DIR}"
fi
echo "Full log: ${LOG_FILE}"
echo ""

cd "${SIM_DIR}" || exit 1

if [[ -x "./veins" ]]; then
    echo "Starting simulation..."
    run_sim ./veins "${EXTRA_INI_ARG[@]+"${EXTRA_INI_ARG[@]}"}" "$@"
elif [[ -x "./fourway" ]]; then
    echo "Starting simulation..."
    cmd=(./fourway)
    if ! has_ned_path_arg "$@"; then
        cmd+=(-n "${VEINS_ROOT}/src/veins:.")
    fi
    cmd+=("${EXTRA_INI_ARG[@]+"${EXTRA_INI_ARG[@]}"}" "$@")
    run_sim "${cmd[@]}"
else
    echo "Starting with opp_run..."
    run_sim opp_run "${EXTRA_INI_ARG[@]+"${EXTRA_INI_ARG[@]}"}" "$@"
fi

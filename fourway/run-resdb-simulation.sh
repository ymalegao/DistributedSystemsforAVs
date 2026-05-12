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
#   Without --byzleader, replica 0 is never chosen as a FALSE_LANE follower (it is
#   usually the consensus leader); use --allow-replica0-byz-follower to allow replica 0.
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
# Examples:
#   ./run-resdb-simulation.sh -u Cmdenv -c FourVehiclesResDB
#   ./run-resdb-simulation.sh --randomize 12 3 -u Cmdenv -c TwelveVehiclesResDB
#   ./run-resdb-simulation.sh --randomize 16 4 --byzleader 0 -u Cmdenv -c SixteenVehiclesResDB
#   ./run-resdb-simulation.sh --leader 4 -u Cmdenv -c EightVehiclesResDB
#   ./run-resdb-simulation.sh --leader 7 --randomize 8 2 -u Cmdenv -c EightVehiclesResDB

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

export VEINS_ROOT="${VEINS_ROOT:-${REPO_ROOT}/veins-veins-5.3.1}"
export FOURWAY_ROOT="${FOURWAY_ROOT:-${REPO_ROOT}/fourway}"

cleanup() {
  echo ""
  echo "=========================================="
  echo "Simulation ended (exit/interrupt). Full log: ${LOG_FILE}"
  echo "=========================================="
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
# Usage: generate_random_scenario <N> <F> <sim_dir> <byz_leader|-1> <allow_r0_follower> <no_ambulance>
#   byz_leader:        replica ID reserved as silent leader (-1 = none)
#   allow_r0_follower: "1" = allow replica 0 in FALSE_LANE pool when byz_leader=-1; else exclude 0
#   no_ambulance:      "1" = force no ambulance (ambulanceReplicaId=-1)
# ---------------------------------------------------------------------------
generate_random_scenario() {
    local n="$1"
    local f="$2"
    local sim_dir="$3"
    local byz_leader="$4"   # -1 means no byz leader
    local allow_r0_follower="${5:-0}"
    local no_ambulance="${6:-0}"

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
    #    When there is no --byzleader, also exclude replica 0 so follower faults never
    #    overlap default leader semantics (unless allow_r0_follower=1).
    #    Fisher-Yates shuffle, take first F.
    local available=()
    local i
    for (( i=0; i<n; i++ )); do
        [[ $i -eq $byz_leader ]] && continue
        if [[ "${AMB_ID}" -ge 0 && $i -eq $AMB_ID ]]; then
            continue
        fi
        if [[ "${byz_leader}" -lt 0 && "${allow_r0_follower}" != "1" && $i -eq 0 ]]; then
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
        if [[ "${allow_r0_follower}" != "1" ]]; then
            echo "  FALSE_LANE pool: replica 0 excluded (Byz follower vs leader disambiguation)" >&2
        fi
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
        # Byz leader: Byzantine at C++ layer (FALSE_LANE)
        if [[ "${byz_leader}" -ge 0 ]]; then
            local leader_node
            leader_node="$(replica_to_node_idx "${n}" "${byz_leader}")"
            echo "*.node[${leader_node}].appl.isByzantine = true"
            echo "*.node[${leader_node}].appl.byzantineType = 1   # FALSE_LANE (byz leader replica ${byz_leader})"
        fi
        # FALSE_LANE nodes (C++ arrival layer only)
        if (( pick > 0 )); then
            for replica_id in "${BYZ_ARRAY[@]}"; do
                local node_idx
                node_idx="$(replica_to_node_idx "${n}" "${replica_id}")"
                echo "*.node[${node_idx}].appl.isByzantine = true"
                echo "*.node[${node_idx}].appl.byzantineType = 1   # FALSE_LANE (replica ${replica_id})"
            done
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
ALLOW_REPLICA0_BYZ_FOLLOWER=0
NO_AMBULANCE=0
INITIAL_LEADER=""   # "" = use default (replica 0)
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
        *)
            filtered_args+=("${args[$i]}")
            ;;
    esac
    i=$(( i + 1 ))
done
set -- "${filtered_args[@]+"${filtered_args[@]}"}"

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

# Generate random scenario after SIM_DIR is known
if [[ "${RANDOMIZE}" -eq 1 ]]; then
    if [[ -z "${RANDOMIZE_N}" || -z "${RANDOMIZE_F}" ]]; then
        echo "ERROR: --randomize requires <N> <F> arguments" >&2
        exit 1
    fi
    RANDOM_INI="$(generate_random_scenario "${RANDOMIZE_N}" "${RANDOMIZE_F}" "${SIM_DIR}" "${BYZ_LEADER}" "${ALLOW_REPLICA0_BYZ_FOLLOWER}" "${NO_AMBULANCE}")"
    # When any -f flag is given, OMNeT++ stops auto-loading omnetpp.ini.
    # Explicitly load omnetpp.ini first, then the override file so it wins.
    EXTRA_INI_ARG=(-f "omnetpp.ini" -f "${RANDOM_INI}")
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

LOG_FILE="/tmp/resdb-simulation.log"
trap cleanup EXIT

> "${LOG_FILE}"
echo "=========================================="
echo "Running OMNeT++ ResDB Simulation"
echo "=========================================="
echo "Repo root: ${REPO_ROOT}"
echo "OMNeT++ root: ${OMNETPP_ROOT}"
echo "VEINS_ROOT: ${VEINS_ROOT}"
echo "Simulation dir: ${SIM_DIR}"
if [[ -n "${NIX_LIB_PATHS}" ]]; then
    echo "LD_LIBRARY_PATH configured with discovered Nix paths"
else
    echo "No Nix runtime paths detected; using existing loader paths"
fi
echo "Full log: ${LOG_FILE}"
echo ""

cd "${SIM_DIR}" || exit 1

if [[ -x "./veins" ]]; then
    echo "Starting simulation..."
    ./veins "${EXTRA_INI_ARG[@]+"${EXTRA_INI_ARG[@]}"}" "$@" 2>&1 | tee -a "${LOG_FILE}"
elif [[ -x "./fourway" ]]; then
    echo "Starting simulation..."
    cmd=(./fourway)
    if ! has_ned_path_arg "$@"; then
        cmd+=(-n "${VEINS_ROOT}/src/veins:.")
    fi
    cmd+=("${EXTRA_INI_ARG[@]+"${EXTRA_INI_ARG[@]}"}" "$@")
    "${cmd[@]}" 2>&1 | tee -a "${LOG_FILE}"
else
    echo "Starting with opp_run..."
    opp_run "${EXTRA_INI_ARG[@]+"${EXTRA_INI_ARG[@]}"}" "$@" 2>&1 | tee -a "${LOG_FILE}"
fi

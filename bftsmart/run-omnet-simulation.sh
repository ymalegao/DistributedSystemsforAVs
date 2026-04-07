#!/bin/bash
# Wrapper script to run OMNeT++ simulations with portable repo-relative paths.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

export BFTSMART_ROOT="${BFTSMART_ROOT:-${REPO_ROOT}/bftsmart}"
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

infer_omnetpp_root
if [[ -z "${OMNETPP_ROOT:-}" ]]; then
    echo "ERROR: OMNETPP_ROOT is not set and could not be inferred." >&2
    echo "Source the OMNeT++ shell environment or export OMNETPP_ROOT before running this script." >&2
    exit 1
fi
export OMNETPP_ROOT

JAVA_HOME="${JAVA_HOME:-/usr/lib/jvm/java-11-openjdk-amd64}"
JVM_LIB_PATH="${JAVA_HOME}/lib/server"

NIX_LIB_PATHS=""
if OMNETPP_RUNTIME_LIB="$(find_omnetpp_runtime_lib)"; then
    NIX_LIB_PATHS="$(ldd "${OMNETPP_RUNTIME_LIB}" 2>/dev/null | \
        grep /nix/store | \
        sed 's/.*=> //' | \
        sed 's/ (0x.*//' | \
        xargs -r -n1 dirname | \
        sort -u | \
        tr '\n' ':')"
else
    echo "ERROR: Could not find liboppsim_dbg.so or liboppsim.so under ${OMNETPP_ROOT}/lib" >&2
    exit 1
fi

if [[ -n "${NIX_LIB_PATHS}" ]]; then
    append_ld_library_path "${NIX_LIB_PATHS%:}"
fi
if [[ -d "${JVM_LIB_PATH}" ]]; then
    append_ld_library_path "${JVM_LIB_PATH}"
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

LOG_FILE="/tmp/bft-all-replicas.log"
trap cleanup EXIT
OLD_VIEW_FILE="${SIM_DIR}/config/currentView"
rm -f "${OLD_VIEW_FILE}"

> "${LOG_FILE}"
echo "=========================================="
echo "Running OMNeT++ Simulation"
echo "=========================================="
echo "Repo root: ${REPO_ROOT}"
echo "OMNeT++ root: ${OMNETPP_ROOT}"
echo "BFTSMART_ROOT: ${BFTSMART_ROOT}"
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
    ./veins "$@" 2>&1 | tee -a "${LOG_FILE}"
elif [[ -x "./fourway" ]]; then
    echo "Starting simulation..."
    cmd=(./fourway)
    if ! has_ned_path_arg "$@"; then
        cmd+=(-n "${VEINS_ROOT}/src/veins:.")
    fi
    cmd+=("$@")
    "${cmd[@]}" 2>&1 | tee -a "${LOG_FILE}"
else
    echo "Starting with opp_run..."
    opp_run "$@" 2>&1 | tee -a "${LOG_FILE}"
fi


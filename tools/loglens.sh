#!/usr/bin/env bash
# loglens.sh — categorized viewer for resdb-simulation.log during rollback
# (CANCEL/BLOCKED/CLEAR/WAIT) debugging. Pulls just the marker lines that
# matter for a given category instead of re-typing the same grep chains.
#
# Usage:
#   ./scripts/loglens.sh [log_file] [category ...]
#   ./scripts/loglens.sh                       # default log, all categories
#   ./scripts/loglens.sh /tmp/resdb-simulation.log cancel clear
#   ./scripts/loglens.sh --replica 12 clear     # filter to r12 only (any log)
#   ./scripts/loglens.sh --list                 # list category names and exit
#
# Categories (each prints a header, matching line count, and the lines):
#   crash      crash injection / tow / freeze
#   incident   BLOCKING/CLEARED incident registry transitions
#   cancel     CANCEL echo/cert/witnessing/drain/consensus/commit
#   discovery  DISCOVERY-BEGIN / DISCOVERY-COMPLETE (post-cancel epoch focus)
#   clear      CLEAR perceive/echo/cert/relay/retx
#   rollback   ROLLBACK-PROPOSE / ROLLBACK-BEGIN / ROLLBACK-COMMIT
#   vc         View-change / timeout / rotation signals — the ones that
#              matter for the WAIT-necessity question (VC-TRIGGER, APP-VC,
#              rollback_rotation_index_ increments, ROLLBACK-VC-UNSUPPORTED)
#   order      ORDER decided/applied/committed, batch assignment, evidence
#   pbft       One-line-per-second PBFT quorum/commit SUMMARY (counts, not
#              the full per-vote spam — use `raw` for that)
#   raw <pat>  Pass-through: grep -n <pat> on the log
#
# Default (no categories given): runs crash, incident, cancel, discovery,
# clear, rollback, vc, order — skips pbft (too noisy for a first look).
set -euo pipefail

LOG="/tmp/resdb-simulation.log"
REPLICA=""
CATS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --replica)
      REPLICA="$2"; shift 2 ;;
    --list)
      grep -E '^#   [a-z]+ ' "$0" | sed 's/^# //'
      exit 0 ;;
    -h|--help)
      sed -n '2,29p' "$0"
      exit 0 ;;
    *)
      if [[ -f "$1" ]]; then
        LOG="$1"
      else
        CATS+=("$1")
      fi
      shift ;;
  esac
done

if [[ ! -f "${LOG}" ]]; then
  echo "log not found: ${LOG}" >&2
  exit 1
fi

if [[ ${#CATS[@]} -eq 0 ]]; then
  CATS=(crash incident cancel discovery clear rollback vc order)
fi

section() {
  local title="$1" pattern="$2"
  local n
  n="$(grep -cE "${pattern}" "${LOG}" || true)"
  echo ""
  echo "════════ ${title} (${n} lines) ════════"
  if [[ "${n}" -eq 0 ]]; then
    return
  fi
  if [[ -n "${REPLICA}" ]]; then
    grep -nE "${pattern}" "${LOG}" | grep -E " r${REPLICA}[^0-9]| r${REPLICA}\$" || echo "  (none for r${REPLICA})"
  else
    grep -nE "${pattern}" "${LOG}"
  fi
}

for cat in "${CATS[@]}"; do
  case "${cat}" in
    crash)
      section "CRASH / TOW / FREEZE" '\[CRASH-INJECT\]|\[CRASH-INJECT-FAIL\]|\[TOW\]|\[TOW-FAIL\]|\[CRASH-COMMS-DISABLE\]|\[CRASH-SELECT\]|\[CRASH-PERCEIVE\]|\[CRASH-RX-DROP\]'
      ;;
    incident)
      section "INCIDENT REGISTRY (BLOCKING/CLEARED)" '\[INCIDENT-REGISTER\]'
      ;;
    cancel)
      section "CANCEL (echo/cert/witnessing/drain/consensus/commit)" '\[CANCEL-WITNESSING\]|\[CANCEL-ECHO\]|\[CANCEL-ECHO-DUP\]|\[CANCEL-CERT|\[CANCEL-DRAIN\]|\[CANCEL-CONSENSUS\]|\[CANCEL-PROPOSE|\[CANCEL-QUORUM\]|\[CANCEL-COMMIT\]|\[CANCEL-RELAY\]|\[CANCEL-UNAVAILABLE\]|\[CANCEL-GOSSIP-SEND\]|\[CANCEL-JUSTIFY-GATE\]|\[TOMBSTONE\]'
      ;;
    discovery)
      section "DISCOVERY (begin/complete)" '\[DISCOVERY-BEGIN\]|\[DISCOVERY-COMPLETE\]|\[DISCOVERY-VIEW\]'
      ;;
    clear)
      section "CLEAR (perceive/echo/cert/relay/retx)" '\[CLEAR-PERCEIVE\]|\[CLEAR-ECHO\]|\[CLEAR-CERT|\[CLEAR-RELAY\]'
      ;;
    rollback)
      section "ROLLBACK (propose/begin/commit/unavailable)" '\[ROLLBACK-PROPOSE\]|\[ROLLBACK-BEGIN\]|\[ROLLBACK-COMMIT\]|\[ROLLBACK-UNAVAILABLE\]|\[ROLLBACK-TRIGGER\]|\[ROLLBACK-PROPOSER-CHECK\]'
      ;;
    vc)
      section "VIEW-CHANGE / TIMEOUT / ROTATION (the WAIT-necessity signals)" '\[VC-TRIGGER\]|\[VC-TIMEOUT\]|\[VC-DEBUG\]|\[APP-VC\]|\[ROLLBACK-VC-UNSUPPORTED\]|rollbackVcTimer|rollback_rotation_index_|\[HALT-LOCAL\].*timeout|ForceViewChange'
      ;;
    order)
      section "ORDER (decided/applied/evidence)" '\[ORDER-ENQ\]|\[ORDER-DEQ\]|\[ORDER-SKIP\]|\[ORDER-TAIL-DROP\]|\[ORDER-WARN\]|\[EPOCH-VIEW\]|\[EPOCH-VIEW-REJECT\]|METRICS.*Order_Decided_Time|METRICS.*Batch_Assignment'
      ;;
    pbft)
      echo ""
      echo "════════ PBFT SUMMARY (quorum reached, per self+seq) ════════"
      grep -oE '\[PBFT-QUORUM\] self=[0-9]+ seq=[0-9]+ hash=\S+ source=\S+ quorum=[0-9]+ N=[0-9]+ epoch=[0-9]+' "${LOG}" \
        | sort -u | sort -t= -k2 -n
      echo ""
      echo "════════ VOTE-DROP summary (reason counts) ════════"
      grep -oE '\[VOTE-DROP\].*reason=\S+' "${LOG}" | grep -oE 'reason=\S+' | sort | uniq -c | sort -rn
      ;;
    raw)
      echo "raw category needs a pattern; use: $0 ${LOG} raw '<pattern>'" >&2
      ;;
    *)
      # treat unknown category as a raw pattern passthrough
      section "RAW: ${cat}" "${cat}"
      ;;
  esac
done

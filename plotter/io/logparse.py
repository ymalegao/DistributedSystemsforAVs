"""The single source of truth for reading simulation logs.

Every pattern the analyzers depend on lives in PATTERNS below. Before this
module the same regexes were re-declared in each analyzer -- Order_Decided_Time
and the Messages_Sent pattern appeared in four scripts each -- so a change to
the C++ log format silently broke some figures and not others. Change a pattern
here and every figure follows.

Parsing is one pass per file extracting all fields, because the files are large
and the parse is I/O bound; selecting fields per figure would mean re-reading.
"""
import re
from pathlib import Path

from .schema import RunKey, RunRecord

# ── log line patterns ────────────────────────────────────────────────────────
# Emitted by the Veins app layer / ResilientDB bridge. Keep the comment next to
# each pattern saying which side emits it, so a C++ rename can be traced here.

# ResDBDecision.cc, on PBFT commit callback.
RE_ORDER_DECIDED = re.compile(r"Order_Decided_Time")

# ResDBIntersectionApp.cc finish(): cumulative per-replica counter.
RE_MESSAGES_SENT = re.compile(r"\[METRICS (\d+)\]\s+Messages_Sent:\s+(\d+)")

# ResDBTraCI.cc: vehicle crossed on the stop-sign timeout, i.e. BFT bypassed.
RE_STOPSIGN_TIMEOUT = re.compile(r"StopSign_Timeout:\s+1")

# Bridge: the active-view sizing actually used for this run.
RE_QUORUM = re.compile(r"voteN=(\d+) f=(\d+) quorum=(\d+)")

# ResDBRollbackProtocol.cc: the late-ambulance cancel/rollback actually ran.
RE_ROLLBACK_FIRED = re.compile(r"CANCEL-COMMIT|ROLLBACK-BEGIN")

# ── run filename ─────────────────────────────────────────────────────────────
# Two layouts in use: ARM_k<K>_rep<R>.log (fixed vehicle count) and
# ARM_v<V>_k<K>_rep<R>.log (scaling study). One pattern covers both; v is None
# when the study fixes the vehicle count.
RE_RUN_NAME = re.compile(r"(OFF|ON)(?:_v(\d+))?_k(\d+)_rep(\d+)\.log$")


def parse_run_name(path) -> RunKey | None:
    """RunKey from a log filename, or None if it is not a run log."""
    m = RE_RUN_NAME.search(Path(path).name)
    if not m:
        return None
    arm, v, k, rep = m.groups()
    return RunKey(arm=arm, k=int(k), v=int(v) if v else None, rep=int(rep))


def parse_log(path, key: RunKey | None = None) -> RunRecord:
    """Read one run log into a RunRecord.

    errors="ignore" because simulation logs can be truncated mid-line when a
    run is killed at the sim-time cap; a partial final line must not lose the
    whole run.
    """
    if key is None:
        key = parse_run_name(path)
    rec = RunRecord(key=key)

    with open(path, errors="ignore") as fh:
        for line in fh:
            if RE_ORDER_DECIDED.search(line):
                rec.orders += 1
                rec.committed = True

            m = RE_MESSAGES_SENT.search(line)
            if m:
                # cumulative: last value per replica wins
                rec.msgs_by_replica[int(m.group(1))] = int(m.group(2))

            if RE_STOPSIGN_TIMEOUT.search(line):
                rec.fallbacks += 1

            if rec.quorum is None:
                m = RE_QUORUM.search(line)
                if m:
                    rec.vote_n = int(m.group(1))
                    rec.f = int(m.group(2))
                    rec.quorum = int(m.group(3))

            if RE_ROLLBACK_FIRED.search(line):
                rec.rollback_fired = True

    return rec

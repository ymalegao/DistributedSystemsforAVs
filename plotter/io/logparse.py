"""The single source of truth for reading simulation logs.

Every pattern the figures depend on lives in this module. Before it, the same
regexes were re-declared in each analyzer -- Order_Decided_Time and the
Messages_Sent pattern appeared in four scripts each -- so a change to the C++
log format broke some figures and not others, silently. Change a pattern here
and every figure follows.

One pass per file extracting all fields: the logs are large and the parse is
I/O bound, so selecting fields per figure would mean re-reading them.
"""
import re
from pathlib import Path

from .schema import RunKey, RunRecord

# ── log line patterns ────────────────────────────────────────────────────────
# Each notes which side emits it, so a C++ rename can be traced back here.

# ResDBDecision.cc, on the PBFT commit callback. The n_batches suffix is the
# size of the schedule the commit produced.
RE_ORDER_DECIDED = re.compile(r"Order_Decided_Time")
RE_ORDER_BATCHES = re.compile(r"\[METRICS (\d+)\]\s+Order_Decided_Time:.*n_batches=(\d+)")

# Bridge: the active-view sizing this run actually used.
RE_QUORUM_VOTE = re.compile(r"voteN=(\d+) f=(\d+) quorum=(\d+)")

# ResDBTraCI.cc: vehicle crossed on the stop-sign timeout, i.e. BFT bypassed.
# This is the safety-relevant degradation, so a refactor must not change it.
RE_STOPSIGN_TIMEOUT = re.compile(r"StopSign_Timeout:\s+1")

# ResDBIntersectionApp.cc finish(): cumulative per-replica counter.
RE_MESSAGES_SENT = re.compile(r"\[METRICS (\d+)\]\s+Messages_Sent:\s+(\d+)")

# End-to-end PBFT ordering latency for the PROPOSE_ALL round.
RE_BFT_LATENCY = re.compile(r"PROPOSE_ALL_BFT\(sim\)=([\d.]+)s")

# Arrival-certificate formation latency (the f+1 layer above vanilla PBFT).
RE_CERT_LATENCY = re.compile(r"Cert_Creation_Latency:\s+([\d.]+)")

# Bridge PreVerify: a fake-ambulance transaction was granted priority, i.e. the
# f+1 firewall did NOT reject the attack.
RE_FALSE_PRIORITY = re.compile(r"FALSE_PRIORITY_GRANTED")

# ResDBTraCI.cc: when each vehicle stopped at, and resumed through, the junction.
RE_STOP_TIME = re.compile(r"\[METRICS (\d+)\]\s+Stop_Time:\s+([\d.]+)")
RE_RESUME_TIME = re.compile(r"\[METRICS (\d+)\]\s+Resume_Time:\s+([\d.]+)")

# ResDBRollbackProtocol.cc: the late-ambulance cancel/rollback actually ran.
RE_ROLLBACK_FIRED = re.compile(r"CANCEL-COMMIT|ROLLBACK-BEGIN")

# ── run filename ─────────────────────────────────────────────────────────────
# Layouts in use, all produced by run_ablations.sh:
#   ab1_OFF_k0_rep1.log        arm + swept k
#   ab2_ours_rep1.log          arm only
#   ab5_rollback_on_rep1.log   arm containing an underscore
# The arm is lazy and _k is optional, so "rollback_on" stays one arm while
# "OFF_k0" splits into arm + k.
RE_RUN_NAME = re.compile(
    r"ab(?P<study>\d+)_(?P<arm>.+?)(?:_k(?P<k>\d+))?_rep(?P<rep>\d+)\.log$"
)


def parse_run_name(path) -> RunKey | None:
    """RunKey from a log filename, or None if it is not an ablation run log."""
    m = RE_RUN_NAME.search(Path(path).name)
    if not m:
        return None
    k = m.group("k")
    return RunKey(
        study=int(m.group("study")),
        arm=m.group("arm"),
        k=int(k) if k is not None else None,
        rep=int(m.group("rep")),
    )


def parse_log(path, key: RunKey | None = None) -> RunRecord:
    """Read one run log into a RunRecord.

    errors="ignore" because logs can be truncated mid-line when a run is killed
    at the sim-time cap; a partial final line must not cost us the whole run.
    """
    if key is None:
        key = parse_run_name(path)
    rec = RunRecord(key=key)

    with open(path, errors="ignore") as fh:
        for line in fh:
            if RE_ORDER_DECIDED.search(line):
                rec.committed = True

            if m := RE_MESSAGES_SENT.search(line):
                # cumulative: last value per replica wins
                rec.msgs_by_replica[int(m.group(1))] = int(m.group(2))

            if m := RE_BFT_LATENCY.search(line):
                rec.bft_latency_s.append(float(m.group(1)))

            if m := RE_CERT_LATENCY.search(line):
                rec.cert_latency_s.append(float(m.group(1)))

            if RE_FALSE_PRIORITY.search(line):
                rec.false_priority_granted += 1

            if m := RE_STOP_TIME.search(line):
                rec.stop_time[int(m.group(1))] = float(m.group(2))

            if m := RE_RESUME_TIME.search(line):
                rec.resume_time[int(m.group(1))] = float(m.group(2))

            if RE_ROLLBACK_FIRED.search(line):
                rec.rollback_fired = True

    return rec

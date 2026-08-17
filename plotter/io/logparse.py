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

# Bridge: the quorum this run actually required. The previous pattern here
# looked for "voteN= f= quorum=", which the simulator never emits, so the field
# was silently always absent.
RE_QUORUM = re.compile(r"\[PBFT-QUORUM\].*?quorum=(\d+)")

# ResDBArrivalProtocol: how long the f+1 certificate round took to collect.
RE_CERT_COLLECTION = re.compile(r"Cert_Collection_Duration:\s+([\d.]+)s")

# ResDBDecision: stop -> consensus decided. Distinct from the ordering latency
# below, which starts at proposal; this includes waiting to be able to propose,
# and is the delay a passenger actually experiences before release.
RE_STOP_TO_DECISION = re.compile(r"stop_to_decision\(sim\)=([\d.]+)s")

# ResDBTraCI.cc: vehicle crossed on the stop-sign timeout, i.e. BFT bypassed.
# This is the safety-relevant degradation, so a refactor must not change it.
RE_STOPSIGN_TIMEOUT = re.compile(r"StopSign_Timeout:\s+1")

# ResDBIntersectionApp.cc finish(): cumulative per-replica counters.
RE_MESSAGES_SENT = re.compile(r"\[METRICS (\d+)\]\s+Messages_Sent:\s+(\d+)")
RE_MESSAGES_RECV = re.compile(r"\[METRICS (\d+)\]\s+Messages_Received:\s+(\d+)")
RE_BYTES_SENT = re.compile(r"\[METRICS (\d+)\]\s+Bytes_Sent:\s+(\d+)")

# ResDBIntersectionApp.cc: consensus did not decide within the timeout.
RE_CONSENSUS_TIMEOUT = re.compile(r"Consensus_Timeout:\s+1")

# ResDBIntersectionApp.cc finish(): one line per BFTMessage type. Lets a
# protocol layer be priced from a normal run instead of a counterfactual build.
RE_SENT_BY_TYPE = re.compile(
    r"\[METRICS (\d+)\]\s+Sent_By_Type:\s+type=(\d+)\s+msgs=(\d+)\s+bytes=(\d+)"
)

# ResDBIntersectionApp.cc / ResDBTraCI.cc, and the all-way-stop baseline module.
# Both emit this identical line on departure, which is what lets ablation 3
# compare the two arms on the same measurement. Intersection units never depart
# and so never emit it.
RE_CAR_METRICS = re.compile(
    r"\[CAR-METRICS\]\s+veh(\d+)\s+role=(\w+)\s+epoch=(\d+)\s+"
    r"stop_time=(-?[\d.]+)\s+depart_time=(-?[\d.]+)"
)

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
#   ab1_ON_n8_k2_rep1.log      arm + vehicle count + swept k
#   ab2_ours_rep1.log          arm only
#   ab5_rollback_on_rep1.log   arm containing an underscore
# The arm is lazy and both _n and _k are optional, so "rollback_on" stays one
# arm while "OFF_k0" splits into arm + k. Logs predating the N sweep carry no
# _n and parse with n=None.
RE_RUN_NAME = re.compile(
    r"ab(?P<study>\d+)_(?P<arm>.+?)(?:_n(?P<n>\d+))?(?:_k(?P<k>\d+))?"
    r"_rep(?P<rep>\d+)\.log$"
)


def parse_run_name(path) -> RunKey | None:
    """RunKey from a log filename, or None if it is not an ablation run log."""
    m = RE_RUN_NAME.search(Path(path).name)
    if not m:
        return None
    k, n = m.group("k"), m.group("n")
    return RunKey(
        study=int(m.group("study")),
        arm=m.group("arm"),
        k=int(k) if k is not None else None,
        rep=int(m.group("rep")),
        n=int(n) if n is not None else None,
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

            if m := RE_MESSAGES_RECV.search(line):
                rec.recv_by_replica[int(m.group(1))] = int(m.group(2))

            if m := RE_BYTES_SENT.search(line):
                rec.bytes_by_replica[int(m.group(1))] = int(m.group(2))

            if RE_STOPSIGN_TIMEOUT.search(line):
                rec.stopsign_timeouts += 1

            if RE_CONSENSUS_TIMEOUT.search(line):
                rec.consensus_timeouts += 1

            if m := RE_SENT_BY_TYPE.search(line):
                # keyed by (replica, type) for the same reason as the totals:
                # the counter is cumulative, so a repeat line must overwrite
                # rather than add before the sum across replicas.
                key = (int(m.group(1)), int(m.group(2)))
                rec.sent_by_type[key] = (int(m.group(3)), int(m.group(4)))

            if m := RE_CAR_METRICS.search(line):
                # First departure per vehicle wins: the TraCI distance check and
                # the scenario-manager hook are two paths to the same event, and
                # under rollback a vehicle can re-depart in a later epoch.
                veh = int(m.group(1))
                if veh not in rec.depart_at:
                    rec.role[veh] = m.group(2)
                    stop = float(m.group(4))
                    if stop >= 0:                 # -1 = never recorded a stop
                        rec.stop_at[veh] = stop
                    rec.depart_at[veh] = float(m.group(5))

            if m := RE_BFT_LATENCY.search(line):
                rec.bft_latency_s.append(float(m.group(1)))

            if m := RE_CERT_LATENCY.search(line):
                rec.cert_latency_s.append(float(m.group(1)))

            if m := RE_CERT_COLLECTION.search(line):
                rec.cert_collection_s.append(float(m.group(1)))

            if m := RE_STOP_TO_DECISION.search(line):
                rec.stop_to_decision_s.append(float(m.group(1)))

            if m := RE_QUORUM.search(line):
                rec.quorum = int(m.group(1))

            if RE_FALSE_PRIORITY.search(line):
                rec.false_priority_granted += 1

            if m := RE_STOP_TIME.search(line):
                rec.stop_time[int(m.group(1))] = float(m.group(2))

            if m := RE_RESUME_TIME.search(line):
                rec.resume_time[int(m.group(1))] = float(m.group(2))

            if RE_ROLLBACK_FIRED.search(line):
                rec.rollback_fired = True

    return rec

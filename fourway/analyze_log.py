#!/usr/bin/env python3
"""
BFT-SMaRt V2V Intersection Log Analyzer.

Tracks the single-round PROPOSE_ALL protocol and per-car lifecycle metrics, including:
  - PROPOSE_ALL consensus latency (sim time): ResDB clusters Order_Decided into waves; per wave
      leader→commit = median(commit) − max(submit before first commit in wave);
      first_submit→commit = median(commit) − min(same submits) (incl. failed primary / VC).
      BFT-SMaRt fallback (PHASE_SUMMARY only): both metrics default to the same mean.
  - wait time at the intersection (stop -> leave)
  - arrival -> resume and arrival -> leave timing
  - throughput using the user-defined formula
  - ambulance vs normal vehicle comparisons

Batch / plotting workflow (same base --save-to, e.g. benchmarks/Priority4cars):
  python analyze_log.py combined.log --save-to benchmarks/Priority4cars --scenario 1 --cars 4
  # writes benchmarks/Priority4cars/no_amb/4veh_0.json (then 4veh_1.json, ...)
  # --scenario: 1=no ambulance, 2=honest ambulance, 3=Byz followers (legacy),
#             4=Byz leader with ambulance, 5=Byz leader no ambulance,
#             6=Byz followers with no ambulance, 15=R0 late-emergency rollback
  # plot_wait_time_cdf.py pools all matching Nveh_*.json per folder.
"""

import sys
import re
import os
import argparse
import statistics
import xml.etree.ElementTree as ET
from collections import defaultdict

try:
    import matplotlib.pyplot as plt
except Exception:
    plt = None

_parser = argparse.ArgumentParser(description="BFT-SMaRt V2V log analyzer")
_parser.add_argument("log_file", nargs="?", default="/tmp/bft-all-replicas.log",
                     help="Path to combined replica log (default: /tmp/bft-all-replicas.log)")
_parser.add_argument("--save-to", metavar="DIR",
                     help="Copy metrics into DIR as <N>veh_<i>.log for later batch analysis")
_parser.add_argument(
    "--scenario", type=int, choices=[1, 2, 3, 4, 5, 6, 15], default=None,
    metavar="N",
    help="Save under a scenario subfolder (requires --save-to): "
         "1=no ambulance, 2=honest ambulance, 3=ambulance+Byzantine followers, "
         "4=ambulance+Byzantine leader, 5=no ambulance+Byzantine leader, "
         "6=no ambulance+Byzantine followers, 15=R0 late-emergency rollback. "
         "Use same base DIR and --cars for each run; "
         "plot_wait_time_cdf.py pools all matching <N>veh_*.json in that folder.")
_parser.add_argument("--cars", type=int, default=None,
                     help="Total cars/replicas in the scenario (e.g. 12 or 16). "
                          "If omitted, inferred from --save-to path or defaults to 16.")
_parser.add_argument("--plots-dir", metavar="DIR",
                     help="Directory for generated plots/CSV. Defaults to the log file directory.")
_parser.add_argument(
    "--output-format",
    choices=["partner", "legacy"],
    default="partner",
    help="JSON output schema. 'partner' matches partnersv2v raft_results.json-style per-vehicle array.",
)
_parser.add_argument(
    "--coordination-method",
    default="pbft",
    help="Value for JSON field coordination_method in partner-format output (default: pbft).",
)
_parser.add_argument(
    "--no-scenario-subdir",
    action="store_true",
    help="Write metrics directly under --save-to (do not append no_amb/amb_honest/...). "
         "Use when --save-to already includes the scenario folder (e.g. .../amb_honest/run_0).",
)
_parser.add_argument(
    "--baseline-tripinfo",
    metavar="XML",
    help="Load SUMO tripinfo output for SUMO-controlled baseline runs.",
)
_parser.add_argument(
    "--scenario-ini",
    metavar="INI",
    help="Optional random_scenario.ini used to identify the priority vehicle.",
)
_args = _parser.parse_args()

# Scenario subdirs (match plot_wait_time_cdf.DEFAULT_BAR_SERIES globs)
SCENARIO_SUBDIR = {
    1: "no_amb",
    2: "amb_honest",
    3: "amb_byz_follower",
    4: "amb_byz_leader",
    5: "no_amb_byz_leader",
    6: "no_amb_byz_follower",
    15: "rollback_emergency_dynamic_n",
}
SCENARIO_LABEL = {
    1: "no ambulance",
    2: "honest ambulance",
    3: "ambulance + Byzantine followers",
    4: "ambulance + Byzantine leader",
    5: "no ambulance + Byzantine leader",
    6: "no ambulance + Byzantine followers",
    15: "R0 late-emergency rollback",
}

LOG_FILE = _args.log_file
SAVE_TO = _args.save_to
if _args.scenario is not None:
    if not SAVE_TO:
        print("analyze_log: --scenario requires --save-to <base_dir>", file=sys.stderr)
        sys.exit(2)
    if not _args.no_scenario_subdir:
        SAVE_TO = os.path.join(SAVE_TO, SCENARIO_SUBDIR[_args.scenario])
PLOTS_DIR = _args.plots_dir or os.path.dirname(os.path.abspath(LOG_FILE)) or "."

def _infer_cars_from_save_to(save_to: str or None) -> int or None:
    if not save_to:
        return None
    # Try a few common naming conventions:
    # - ".../Priority12cars/..." or ".../Priority16cars/no_amb/..."
    # - ".../12veh_0.log" or folder ".../12vehRuns"
    # - ".../2phase12Honest" or ".../phase12..."
    hay = os.path.abspath(save_to)
    m = re.search(r"Priority\s*(\d+)\s*cars?", hay, flags=re.IGNORECASE)
    if m:
        try:
            return int(m.group(1))
        except ValueError:
            pass
    patterns = [
        r'(\d+)\s*veh',
        r'phase\s*(\d+)',
        r'(\d+)(?=Honest)',
    ]
    for pat in patterns:
        m = re.search(pat, hay, flags=re.IGNORECASE)
        if m:
            try:
                return int(m.group(1))
            except ValueError:
                pass
    return None

CARS = _args.cars or _infer_cars_from_save_to(SAVE_TO) or 16
N_EPOCHS = max(1, CARS // 4)

# Epoch → group size mapping (4 cars depart each round)
EPOCH_N = {epoch: max(CARS - 4 * epoch, 0) for epoch in range(N_EPOCHS)}

def bft_quorum(n_active: int, f: int) -> int:
    if n_active <= 0:
        raise ValueError("n_active must be positive")
    if f < 0 or f > (n_active - 1) // 3:
        raise ValueError("invalid f for active membership")
    return (n_active + f + 2) // 2

# ── Patterns ────────────────────────────────────────────────────────────────
RE_ROUND_METRIC = re.compile(
    r'\[ROUND-METRICS\] Epoch (\d+) Avg_([\w]+): ([\d.]+) seconds')

RE_DIAG_EPOCH = re.compile(
    r'\[ORDER-DIAG\] Replica (\d+) epoch=(\d+)')

# Vehicle Lifecycle Diagnostics
RE_ORDER_DECISION = re.compile(
    r'\[V2VProxy\s+\d+\] handleOrderDecision: (.*)|\[ORDER\].*DECIDED.*: (.*)'
)
RE_ORDER_COMMITTED = re.compile(r'\[ORDER\] Committed OrderBag epoch=(\d+) batches=\d+ decision=(.*)')
RE_BATCH_ENTRY = re.compile(r'(veh\d+):BATCH:(\d+)')

RE_MESSAGES_SENT     = re.compile(r'\[METRICS (\d+)\]\s+(?:Messages_Sent|Sent Messages): (\d+)')
RE_MESSAGES_RECEIVED = re.compile(r'\[METRICS (\d+)\]\s+(?:Messages_Received|Received Messages): (\d+)')
RE_BYTES_SENT        = re.compile(r'\[METRICS (\d+)\]\s+Bytes_Sent: (\d+)')
RE_QUIET_HONEST_VEHICLES = re.compile(r'\[METRICS (\d+)\]\s+Quiet_Honest_Vehicles: (\d+)')
RE_QUIET_HONEST_OPPORTUNITIES = re.compile(r'\[METRICS (\d+)\]\s+Quiet_Honest_Opportunities: (\d+)')
RE_QUIET_HONEST_RATE = re.compile(r'\[METRICS (\d+)\]\s+Quiet_Honest_Rate: ([-\d.]+)')
RE_ARRIVAL_TIME      = re.compile(r'\[METRICS (\d+)\] Arrival_Time: ([\d.]+)')
RE_STOP_TIME         = re.compile(r'\[METRICS (\d+)\] Stop_Time: ([\d.]+)')
RE_RESUME_TIME       = re.compile(r'\[METRICS (\d+)\] Resume_Time: ([\d.]+)')
# ResDB: chronological (rep, sim_time_s) for cluster-based commit waves (see
# compute_resdb_submit_to_commit_by_wave).
RE_RESDB_PROPOSE_SUBMIT = re.compile(
    r'\[METRICS (\d+)\] ProposeAll_Submit_Time:\s*(\S+)')
RE_RESDB_ORDER_DECIDED = re.compile(
    r'\[METRICS (\d+)\] Order_Decided_Time:\s*(\S+)')
RE_CERT_START = re.compile(r'\[METRICS (\d+)\] Cert_Collection_Start:\s*(\S+)')
RE_VC_TRIGGER = re.compile(r'\[VC-TRIGGER\]\s+r(\d+)\s+forcing view change at\s+(\S+)')
RE_PRIMARY_CHANGED = re.compile(
    r'\[VC-DEBUG\]\s+r(\d+)\s+primary changed:\s+(\d+)\s+->\s+(\d+)\s+t=(\S+)'
)
RE_GOSSIP_APPLY = re.compile(r'\[GOSSIP-APPLY\]\s+r(\d+)\s+epoch=(\d+)\s+t=(\S+)')
RE_GOSSIP_SEND = re.compile(r'\[GOSSIP-SEND\]\s+r(\d+)\s+epoch=(\d+)\s+retry=(\d+)\s+t=(\S+)')
RE_BYZANTINE_CONFIG = re.compile(r'\[BYZANTINE\]\s+r(\d+)\s+([A-Z_]+)')
RE_PREVERIFY_STATE_MISMATCH = re.compile(
    r'\[OMNET-PREVERIFY\]\s+reject:\s+state-field mismatch'
)
RE_CONSENSUS_ATTACK_OUTCOME = re.compile(
    r'\[CONSENSUS_ATTACK_OUTCOME\]\s+r(\d+)\s+epoch=(\d+)\s+'
    r'fault=([A-Z_]+)\s+outcome=([A-Z_]+)(?:\s+(.*))?'
)
RE_FALSE_PRIORITY_GRANTED = re.compile(
    r'\[FALSE_PRIORITY_GRANTED\]\s+r(\d+)\s+epoch=(\d+)\s+veh(\d+)'
)
RE_CRASH_DETECTED = re.compile(
    r'\[CRASH_DETECTED\]\s+r(\d+)\s+epoch=(\d+)\s+batch=(\d+)'
)
RE_STOPSIGN_TIMEOUT  = re.compile(r'\[METRICS (\d+)\] StopSign_Timeout: 1')
RE_AMBULANCE_SCHED   = re.compile(r'\[AMBULANCE_SCHED\] (veh\d+) batch=')
RE_CAR_METRICS       = re.compile(
    r'\[CAR-METRICS\] (veh\d+) role=(ambulance|normal) epoch=(\d+) '
    r'stop_time=([-\d.]+) depart_time=([-\d.]+) wait_stop_to_departure_sec=([-\d.]+)')
RE_AMBULANCE_WAIT    = re.compile(
    r'\[AMBULANCE_METRICS\] (veh\d+) sim_wait_stop_to_departure_sec=([-\d.]+) epoch=(\d+)')
RE_BYZANTINE_INJECTION = re.compile(
    r'\[BYZANTINE INJECTION\] Replica (\d+) CID=(\d+) intentionally broadcasting corrupted consensus hash')

RE_PHASE_SUMMARY = re.compile(
    r'\[PHASE_SUMMARY (\d+)\](?:\s+epoch=(\d+))? .*PROPOSE_ALL_BFT\(sim\)=(?:([\d.]+)s|N/A) .*stop_to_decision\(sim\)=(?:([\d.]+)s|N/A)'
)
RE_CONSENSUS_HEADER = re.compile(r'CONSENSUS METRICS \(Replica (\d+)\) epoch=(\d+)')
RE_CERT_DUR = re.compile(r'\[METRICS (\d+)\] Cert_Collection_Duration: ([\d.]+)s')
RE_CERT_CREATION_LATENCY = re.compile(r'\[METRICS (\d+)\] Cert_Creation_Latency: ([-\d.]+)s')
# ResDB batch assignment (from IntersectionExecutor batch packer)
RE_BATCH_ASSIGN = re.compile(r'\[METRICS (\d+)\] Batch_Assignment: batch=(\d+)')
RE_TOLERATED_F_CONFIG = re.compile(
    r'\[TOLERATED-F-(?:APP|CONFIG)\].*tolerated_f=(-?\d+)'
    r'.*static_n=(\d+).*quorum=(-?\d+).*cert_threshold=(-?\d+)'
)
RE_R0_CANCEL_WITNESS = re.compile(
    r'\[CANCEL-WITNESS\]\s+r(\d+).*cancelled_epoch=(\d+).*ref=([^\s]+)'
)
RE_R0_CANCEL_ECHO = re.compile(
    r'\[CANCEL-ECHO\]\s+r(\d+)\s+epoch=(\d+)\s+reason=(\d+)\s+ref=([^\s]+)'
)
RE_R0_CANCEL_CERT = re.compile(
    r'\[CANCEL-CERT\]\s+r(\d+)\s+broadcast\s+key=([^\s]+)\s+echoes=(\d+)'
)
RE_R0_ROLLBACK_QUORUM = re.compile(
    r'\[ROLLBACK-QUORUM\]\s+r(\d+)\s+\|M\|=(\d+)\s+f_anchored=(\d+)'
    r'\s+quorum=(\d+)\s+proposer=r(\d+).*new_epoch=(\d+)'
)
RE_R0_ROLLBACK_QUORUM_V3 = re.compile(
    r'\[ROLLBACK-QUORUM\]\s+r(\d+)\s+mode=(per_epoch|anchored)'
    r'\s+voteN=(\d+)\s+sceneN=(\d+)\s+f=(\d+)\s+quorum=(\d+)'
    r'\s+proposer=r(\d+).*new_epoch=(\d+)'
)
RE_R0_ROLLBACK_RECONFIG = re.compile(
    r'\[ROLLBACK-RECONFIG\]\s+r(\d+)\s+mode=(per_epoch|anchored)'
    r'\s+oldEpoch=(\d+)\s+newEpoch=(\d+)\s+voteN=(\d+)\s+sceneN=(\d+)'
    r'\s+newF=(\d+)\s+quorum=(\d+)'
)
RE_R0_ROLLBACK_UNAVAILABLE = re.compile(
    r'\[ROLLBACK-UNAVAILABLE\].*\|M\|=(\d+).*f_anchored=(\d+)'
    r'(?:\s+f_dynamic=(-?\d+))?.*need>=(\d+)'
)
RE_R0_ROLLBACK_UNAVAILABLE_V3 = re.compile(
    r'\[ROLLBACK-UNAVAILABLE\].*mode=(per_epoch|anchored)'
    r'(?:\s+reason=[^\s]+)?\s+voteN=(\d+)(?:\s+f_anchored=(\d+))?'
    r'(?:\s+f=(\d+))?\s+need>=(\d+)'
)
RE_R0_ROLLBACK_PROPOSE_SKIP_MINUS_ONE = re.compile(
    r'\[ROLLBACK-PROPOSE\].*proposer=r-1'
)
RE_R0_ROLLBACK_BEGIN = re.compile(
    r'\[ROLLBACK-BEGIN\]\s+r(\d+)\s+cancelled_epoch=(\d+)\s+new_epoch=(\d+)'
)
RE_R0_CANCEL_COMMIT = re.compile(
    r'\[CANCEL-COMMIT\]\s+r(\d+)\s+cancelled_epoch=(\d+)\s+seq=(\d+)\s+'
    r'(?:quorum=(\d+)\s+)?source=([^\s]+)'
)
RE_R0_ROLLBACK_COMMIT = re.compile(
    r'\[ROLLBACK-COMMIT\]\s+r(\d+)\s+cancelled_epoch=(\d+)\s+new_epoch=(\d+)'
)
RE_R0_ROLLBACK_PROPOSE_RC = re.compile(
    r'\[ROLLBACK-PROPOSE\]\s+r(\d+)\s+rc=(-?\d+)'
)
RE_R0_ROLLBACK_DISCOVERY_SNAPSHOT = re.compile(
    r'\[ROLLBACK-DISCOVERY\]\s+r(\d+)\s+snapshot\s+reason=([^\s]+)\s+'
    r'mode=(per_epoch|anchored)\s+signed=(\d+)\s+quiet=(\d+)\s+'
    r'voteN=(\d+)\s+sceneN=(\d+)\s+expectedN=(\d+)'
)
RE_R0_ACTIVE_VIEW_EPOCH1 = re.compile(
    r'\[ACTIVE-VIEW\]\s+mode=(?:pending|promoted|request)\s+epoch=1\s+'
    r'seq=(\d+)\s+hash=([^\s]+)\s+N=(\d+)\s+f=(\d+)\s+'
    r'quorum=(\d+)\s+primary=r(\d+)'
)
RE_R0_EXECUTOR_ORDER_EPOCH = re.compile(
    r'\[EXECUTOR\]\s+OrderDecision:\s+epoch=(\d+).*?'
    r'n_vehicles=(\d+)\s+n_batches=(\d+)(?:\s+decisions=\[(.*)\])?'
)
RE_PBFT_NEW_REQ = re.compile(
    r'\[PBFT-NEW-REQ\]\s+primary=(\d+)\s+broadcasting PRE_PREPARE seq=(\d+)'
)

# Pre-computed summary metrics (written by --save-to or the C++ side)
RE_RUN_METRIC       = re.compile(r'\[RUN-METRICS\] ([\w_]+):\s*([-\d.]+)')
RE_ROUND_METRIC_CAR = re.compile(
    r'\[ROUND-METRICS\] Epoch (\d+) Arrival_To_Resume_Time (veh\d+):\s*([\d.]+) seconds')
RE_ROUND_METRIC_KEY = re.compile(
    r'\[ROUND-METRICS\] Epoch (\d+) ([A-Za-z_][\w_]*):\s*([-\d.]+)')

# ── Data stores ─────────────────────────────────────────────────────────────
round_metrics = defaultdict(dict)      # epoch → {metric_name: value}

# Track which epoch we're in for each replica (coarse: by ORDER-DIAG)
replica_epoch = {}                     # replica → last seen epoch

# Instrument for Vehicle Lifecycle
go_decision_epoch = {}  # carId -> epoch it was granted GO

# New per-epoch data stores
replica_arrival_time = {}                   # replica -> first arrival time in intersection zone
replica_stop_time   = {}                    # replica -> stop_time (float)
replica_resume_time = {}                    # replica -> resume_time (float)
epoch_messages_sent = defaultdict(list)     # epoch -> [sent_count, ...]
epoch_messages_recv = defaultdict(list)     # epoch -> [recv_count, ...]
epoch_bytes_sent    = defaultdict(list)     # epoch -> [payload_bytes, ...]
epoch_total_dur     = defaultdict(list)     # epoch -> [duration, ...]
epoch_total_dur_by_car = defaultdict(dict)  # epoch -> {carId: duration}
epoch_failures      = defaultdict(int)      # epoch -> count
epoch_cert_creation_latency = defaultdict(list)  # epoch -> [cert f+1 assembly latency seconds]
ambulance_ids       = set()
car_metrics         = defaultdict(dict)     # carId -> {metric_name: value}
replica_batch_index = {}                    # replica_id (int) -> batch_index (int)
r0_metrics = {
    "enabled": False,
    "failure_stage": None,
    "cancel_witness_count": 0,
    "cancel_echo_count": 0,
    "cancel_cert_count": 0,
    "bad_quorum_count": 0,
    "rollback_unavailable_count": 0,
    "rollback_unavailable_max_m": 0,
    "rollback_unavailable_f": None,
    "rollback_unavailable_need": None,
    "rollback_quorums": [],
    "rollback_reconfigs": [],
    "rollback_discovery_snapshots": [],
    "rollback_active_views": [],
    "cancel_commit_count": 0,
    "rollback_commit_count": 0,
    "rollback_commit_replica_ids": [],
    "epoch1_order_decision_count": 0,
    "epoch1_order_n_vehicles": None,
    "epoch1_order_n_batches": None,
    "epoch1_order_vehicle_ids": [],
    "rollback_begin_seen": False,
    "rollback_epoch1_committed": False,
    "rollback_propose_rc0": False,
    "rollback_pbft_new_req_after_propose": False,
    "proposer_minus_one_skip_count": 0,
    "rollback_mode": None,
}

# Byzantine injection tracking
byzantine_by_epoch   = defaultdict(int)    # epoch -> count of [BYZANTINE INJECTION] events
byzantine_by_replica = defaultdict(int)    # replica -> count
byzantine_total      = 0                   # across the whole run

# Per-replica message counts and fallback tracking
replica_messages_sent    = {}   # replica -> messages_sent count (last value seen)
replica_messages_recv    = {}   # replica -> messages_received count (last value seen)
replica_bytes_sent       = {}   # replica -> payload bytes sent (last value seen)
replica_stopsign_timeout = set()  # replica IDs that hit StopSign_Timeout (used fallback)
replica_quiet_honest_vehicles = {}       # replica -> honest vehicles marked quiet
replica_quiet_honest_opportunities = {}  # replica -> honest vehicle opportunities
replica_quiet_honest_rate = {}           # replica -> percentage

# Pre-computed summary metrics read back from saved logs
run_metrics           = {}                 # key -> float  (from [RUN-METRICS] lines)
propose_all_sim_raw   = defaultdict(list)  # epoch -> [seconds, ...]
stop_to_decision_sim_raw = defaultdict(list) # epoch -> [seconds, ...]
# ResDB: (replica_id, sim_time_s) in log order — used to derive commit waves.
resdb_propose_submit_events = []
resdb_order_decided_events = []
replica_cert_collection_start = {}  # replica -> cert collection start time (sim s)
cert_collection_start_by_epoch = {}  # epoch -> earliest cert collection start time (sim s)
replica_vc_trigger_time = {}        # replica -> VC trigger time (sim s)
replica_primary_change_times = defaultdict(list)  # replica -> [t_s, ...]
replica_gossip_apply_time = {}      # replica -> t_s when gossip order applied
replica_gossip_send_time_by_epoch = {}  # (replica, epoch) -> earliest t_s for GOSSIP-SEND
replica_byzantine_types = defaultdict(set)  # replica -> {"SILENT_PRIMARY", ...}
vc_trigger_times_by_epoch = defaultdict(list)      # epoch -> [t_s, ...]
primary_change_times_by_epoch = defaultdict(list)  # epoch -> [t_s, ...]
order_decided_times_by_epoch = defaultdict(list)   # epoch -> [t_s, ...]
propose_submit_times_by_epoch = defaultdict(list)  # epoch -> [t_s, ...]
attack_outcomes = []
attack_outcome_keys = set()
attack_outcomes_by_epoch = defaultdict(list)
attack_failures_by_epoch = defaultdict(int)
experiment_fault_tolerance = {
    "tolerated_f": None,
    "static_replicas": None,
    "consensus_quorum": None,
    "cert_threshold": None,
}
attack_success_outcomes = {
    "MALICIOUS_INPUT_COMMITTED",
    "ORDER_COMMITTED_AFTER_MALFORMED_PROPOSAL",
    "FALSE_PRIORITY_GRANTED",
    "UNCERTIFIED_PRIORITY_CLAIM_COMMITTED",
    "UNSAFE_ORDER_COMMITTED",
}

def record_attack_outcome(
    replica: int,
    epoch: int,
    fault: str,
    outcome: str,
    detail: str = "",
) -> None:
    row = {
        "replica": replica,
        "epoch": epoch,
        "fault": fault,
        "outcome": outcome,
        "detail": detail,
        "attack_success": outcome in attack_success_outcomes,
    }
    attack_outcomes.append(row)
    key = (epoch, fault, outcome, detail)
    if key not in attack_outcome_keys:
        attack_outcome_keys.add(key)
        attack_outcomes_by_epoch[epoch].append(row)
        if row["attack_success"]:
            attack_failures_by_epoch[epoch] += 1

def _safe_float(value, default=None):
    try:
        return float(value)
    except (TypeError, ValueError):
        return default

def _read_ambulance_id_from_ini(path):
    if not path or not os.path.isfile(path):
        return None
    pat = re.compile(r'^\s*\*\.node\[\*\]\.appl\.ambulanceReplicaId\s*=\s*(-?\d+)\s*$')
    with open(path, "r", errors="replace") as f_ini:
        for line in f_ini:
            m = pat.search(line)
            if m:
                val = int(m.group(1))
                return val if val >= 0 else None
    return None

def load_baseline_tripinfo(path, scenario_ini=None):
    if not path:
        return
    if not os.path.isfile(path):
        print(f"WARNING: baseline tripinfo not found: {path}", file=sys.stderr)
        return

    amb_id = _read_ambulance_id_from_ini(scenario_ini)
    if amb_id is None and _args.scenario in (2, 3, 4):
        amb_id = 0
    if amb_id is not None:
        ambulance_ids.add(f"veh{amb_id}")

    root = ET.parse(path).getroot()
    for elem in root.findall(".//tripinfo"):
        cid = elem.get("id")
        if not cid or not cid.startswith("veh"):
            continue
        try:
            rep = int(cid[3:])
        except ValueError:
            continue

        depart = _safe_float(elem.get("depart"))
        arrival = _safe_float(elem.get("arrival"))
        waiting = _safe_float(elem.get("waitingTime"), 0.0)
        duration = _safe_float(elem.get("duration"))
        time_loss = _safe_float(elem.get("timeLoss"))
        role = "ambulance" if cid == f"veh{amb_id}" else "normal"
        epoch = min(rep // 4, N_EPOCHS - 1)

        cm = car_metrics[cid]
        cm["role"] = role
        cm["epoch"] = epoch
        cm["arrival_time"] = depart
        cm["stop_time"] = depart
        cm["resume_time"] = arrival
        cm["depart_time"] = arrival
        cm["wait_intersection"] = waiting
        cm["stop_to_resume"] = waiting
        cm["resume_to_depart"] = 0.0
        cm["arrival_to_depart"] = duration
        cm["messages_sent"] = 0
        cm["messages_received"] = 0
        cm["delivery_ratio"] = None
        cm["estimated_loss_rate"] = None
        cm["used_fallback"] = False
        cm["baseline_time_loss_s"] = time_loss

def percentile(values, q):
    if not values:
        return None
    if len(values) == 1:
        return values[0]
    vals = sorted(values)
    idx = (len(vals) - 1) * q
    lo = int(idx)
    hi = min(lo + 1, len(vals) - 1)
    frac = idx - lo
    return vals[lo] + (vals[hi] - vals[lo]) * frac

def jains_fairness(values):
    vals = [v for v in values if v is not None]
    if not vals:
        return None
    denom = len(vals) * sum(v * v for v in vals)
    if denom == 0:
        return 1.0
    return (sum(vals) ** 2) / denom

def fmt_stat(values):
    if not values:
        return "N/A"
    return (
        f"mean={statistics.mean(values):.4f}s "
        f"median={statistics.median(values):.4f}s "
        f"p95={percentile(values, 0.95):.4f}s "
        f"max={max(values):.4f}s "
        f"n={len(values)}"
    )


def compute_resdb_submit_to_commit_by_wave(
    submit_events,
    order_events,
    cluster_gap_s=2.0,
    max_epochs=16,
):
    """
    ResDB / single-log: cluster Order_Decided sim times into commit waves (replicas see
    the same decision within cluster_gap_s). For each wave:
      t_commit = median(Order_Decided times in the wave)
      t_last_submit = max(ProposeAll_Submit_Time) in the submit window (winning proposal)
      t_first_submit = min(ProposeAll_Submit_Time) in the submit window (earliest attempt)
    Then:
      leader_submit_to_commit_s = t_commit - t_last_submit  (winning leader's PBFT slice)
      first_submit_to_commit_s   = t_commit - t_first_submit (includes failed primary + VC)
    Submit window: prev_wave_max_order < submit_time <= min(Order_Decided in cluster).
    Epoch index = min(wave_index, max_epochs - 1) when folding long runs.
    Returns a list of dicts (one per wave).
    """
    if not order_events:
        return []

    order_times = sorted(T for _, T in order_events)
    prev = order_times[0]
    clusters = [[prev]]
    for t in order_times[1:]:
        if t - prev > cluster_gap_s:
            clusters.append([t])
        else:
            clusters[-1].append(t)
        prev = t

    out = []
    prev_commit_hi = -1.0
    for wi, cl in enumerate(clusters):
        t_commit = statistics.median(cl)
        t_lo = min(cl)
        # Submits strictly before the first replica's Order_Decided in this wave.
        submits_in_window = [
            (r, t)
            for (r, t) in submit_events
            if prev_commit_hi < t < t_lo
        ]
        if not submits_in_window:
            submits_in_window = [
                (r, t)
                for (r, t) in submit_events
                if prev_commit_hi < t <= t_lo + 1e-6
            ]
        if not submits_in_window:
            prev_commit_hi = max(cl)
            continue
        ts = [t for _, t in submits_in_window]
        t_first = min(ts)
        t_last = max(ts)
        winning_rep = max(r for r, t in submits_in_window if t == t_last)
        ep = min(wi, max(0, max_epochs - 1))
        out.append({
            "epoch": ep,
            "wave_index": wi,
            "t_commit": t_commit,
            "leader_submit_to_commit_s": t_commit - t_last,
            "first_submit_to_commit_s": t_commit - t_first,
            "winning_rep": winning_rep,
        })
        prev_commit_hi = max(cl)
    return out


# ── Parse ────────────────────────────────────────────────────────────────────
print(f"Parsing {LOG_FILE} ...")
current_gossip_epoch = 0              # rough tracker for gossip events

if not os.path.isfile(LOG_FILE) and _args.baseline_tripinfo:
    _log_lines = []
else:
    with open(LOG_FILE, "r", errors="replace") as f:
        _log_lines = list(f)

for line in _log_lines:
        # ROUND-METRICS
        m = RE_ROUND_METRIC.search(line)
        if m:
            epoch, metric, val = int(m.group(1)), m.group(2), float(m.group(3))
            round_metrics[epoch][metric] = val
            continue

        # ORDER-DIAG – epoch tracker
        m = RE_DIAG_EPOCH.search(line)
        if m:
            rep, epoch = int(m.group(1)), int(m.group(2))
            replica_epoch[rep] = epoch
            current_gossip_epoch = epoch
            if r0_metrics["rollback_begin_seen"] and epoch == 1:
                r0_metrics["rollback_epoch1_committed"] = True
            continue

        m = RE_ORDER_COMMITTED.search(line)
        if m:
            epoch = int(m.group(1)) - 1
            decision = m.group(2)
            for car, _batch_idx in RE_BATCH_ENTRY.findall(decision):
                if car not in go_decision_epoch:
                    go_decision_epoch[car] = epoch
                    car_metrics[car]["epoch"] = epoch
            continue

        m = RE_ORDER_DECISION.search(line)
        if m:
            decision = m.group(1) or m.group(2) or ""
            for car, _batch_idx in RE_BATCH_ENTRY.findall(decision):
                if car not in go_decision_epoch:
                    go_decision_epoch[car] = current_gossip_epoch
                    car_metrics[car]["epoch"] = current_gossip_epoch
            continue

        m = RE_AMBULANCE_SCHED.search(line)
        if m:
            ambulance_ids.add(m.group(1))
            continue

        m = RE_MESSAGES_SENT.search(line)
        if m:
            rep, val = int(m.group(1)), int(m.group(2))
            ep = replica_epoch.get(rep, current_gossip_epoch)
            epoch_messages_sent[ep].append(val)
            replica_messages_sent[rep] = val
            continue

        m = RE_MESSAGES_RECEIVED.search(line)
        if m:
            rep, val = int(m.group(1)), int(m.group(2))
            ep = replica_epoch.get(rep, current_gossip_epoch)
            epoch_messages_recv[ep].append(val)
            replica_messages_recv[rep] = val
            continue

        m = RE_BYTES_SENT.search(line)
        if m:
            rep, val = int(m.group(1)), int(m.group(2))
            ep = replica_epoch.get(rep, current_gossip_epoch)
            epoch_bytes_sent[ep].append(val)
            replica_bytes_sent[rep] = val
            continue

        m = RE_QUIET_HONEST_VEHICLES.search(line)
        if m:
            replica_quiet_honest_vehicles[int(m.group(1))] = int(m.group(2))
            continue

        m = RE_QUIET_HONEST_OPPORTUNITIES.search(line)
        if m:
            replica_quiet_honest_opportunities[int(m.group(1))] = int(m.group(2))
            continue

        m = RE_QUIET_HONEST_RATE.search(line)
        if m:
            replica_quiet_honest_rate[int(m.group(1))] = float(m.group(2))
            continue

        m = RE_ARRIVAL_TIME.search(line)
        if m:
            rep = int(m.group(1))
            # Keep only the first arrival (car enters zone once per epoch)
            if rep not in replica_arrival_time:
                replica_arrival_time[rep] = float(m.group(2))
            continue

        m = RE_STOP_TIME.search(line)
        if m:
            replica_stop_time[int(m.group(1))] = float(m.group(2))
            continue

        m = RE_RESUME_TIME.search(line)
        if m:
            replica_resume_time[int(m.group(1))] = float(m.group(2))
            continue

        m = RE_RESDB_PROPOSE_SUBMIT.search(line)
        if m:
            rep = int(m.group(1))
            try:
                t_s = float(m.group(2))
                resdb_propose_submit_events.append((rep, t_s))
                ep = replica_epoch.get(rep, current_gossip_epoch)
                propose_submit_times_by_epoch[ep].append(t_s)
            except ValueError:
                pass
            continue

        m = RE_RESDB_ORDER_DECIDED.search(line)
        if m:
            rep = int(m.group(1))
            try:
                t_s = float(m.group(2))
                resdb_order_decided_events.append((rep, t_s))
                ep = replica_epoch.get(rep, current_gossip_epoch)
                order_decided_times_by_epoch[ep].append(t_s)
            except ValueError:
                pass
            continue

        m = RE_CERT_START.search(line)
        if m:
            rep = int(m.group(1))
            try:
                t_s = float(m.group(2))
                replica_cert_collection_start[rep] = t_s
                ep = replica_epoch.get(rep, current_gossip_epoch)
                prev = cert_collection_start_by_epoch.get(ep)
                cert_collection_start_by_epoch[ep] = t_s if prev is None else min(prev, t_s)
            except ValueError:
                pass
            continue

        m = RE_VC_TRIGGER.search(line)
        if m:
            rep = int(m.group(1))
            try:
                t_s = float(m.group(2))
                replica_vc_trigger_time[rep] = t_s
                ep = replica_epoch.get(rep, current_gossip_epoch)
                vc_trigger_times_by_epoch[ep].append(t_s)
            except ValueError:
                pass
            continue

        m = RE_PRIMARY_CHANGED.search(line)
        if m:
            rep = int(m.group(1))
            try:
                t_s = float(m.group(4))
                replica_primary_change_times[rep].append(t_s)
                ep = replica_epoch.get(rep, current_gossip_epoch)
                primary_change_times_by_epoch[ep].append(t_s)
            except ValueError:
                pass
            continue

        m = RE_GOSSIP_APPLY.search(line)
        if m:
            rep = int(m.group(1))
            try:
                replica_gossip_apply_time[rep] = float(m.group(3))
            except ValueError:
                pass
            continue

        m = RE_GOSSIP_SEND.search(line)
        if m:
            rep = int(m.group(1))
            ep = int(m.group(2))
            try:
                t_s = float(m.group(4))
                key = (rep, ep)
                prev = replica_gossip_send_time_by_epoch.get(key)
                replica_gossip_send_time_by_epoch[key] = t_s if prev is None else min(prev, t_s)
            except ValueError:
                pass
            continue

        m = RE_BYZANTINE_CONFIG.search(line)
        if m:
            rep = int(m.group(1))
            replica_byzantine_types[rep].add(m.group(2))
            continue

        m = RE_PREVERIFY_STATE_MISMATCH.search(line)
        if m:
            active_faults = {
                fault
                for faults in replica_byzantine_types.values()
                for fault in faults
                if fault in ("FAKE_AMBULANCE", "TAMPER_LANE")
            }
            for fault in sorted(active_faults):
                record_attack_outcome(
                    -1,
                    current_gossip_epoch,
                    fault,
                    "PREVERIFY_BLOCKED_STATE_MISMATCH",
                    "check=10",
                )
            continue

        m = RE_CONSENSUS_ATTACK_OUTCOME.search(line)
        if m:
            rep = int(m.group(1))
            epoch = int(m.group(2))
            fault = m.group(3)
            outcome = m.group(4)
            detail = (m.group(5) or "").strip()
            record_attack_outcome(rep, epoch, fault, outcome, detail)
            continue

        m = RE_FALSE_PRIORITY_GRANTED.search(line)
        if m:
            rep = int(m.group(1))
            epoch = int(m.group(2))
            veh = int(m.group(3))
            record_attack_outcome(
                rep, epoch, "FAKE_AMBULANCE", "FALSE_PRIORITY_GRANTED",
                f"target=veh{veh}",
            )
            continue

        m = RE_CRASH_DETECTED.search(line)
        if m:
            rep = int(m.group(1))
            epoch = int(m.group(2))
            batch = int(m.group(3))
            record_attack_outcome(
                rep, epoch, "TAMPER_LANE", "UNSAFE_ORDER_COMMITTED",
                f"batch={batch}",
            )
            continue

        m = RE_STOPSIGN_TIMEOUT.search(line)
        if m:
            rep = int(m.group(1))
            ep = replica_epoch.get(rep, current_gossip_epoch)
            epoch_failures[ep] += 1
            replica_stopsign_timeout.add(rep)
            continue

        m = RE_CAR_METRICS.search(line)
        if m:
            car_id = m.group(1)
            role = m.group(2)
            epoch = int(m.group(3))
            stop_t = float(m.group(4))
            depart_t = float(m.group(5))
            wait_t = float(m.group(6))
            if role == "ambulance":
                ambulance_ids.add(car_id)
            car_metrics[car_id]["role"] = role
            car_metrics[car_id]["epoch"] = epoch
            car_metrics[car_id]["stop_time"] = None if stop_t < 0 else stop_t
            car_metrics[car_id]["depart_time"] = depart_t
            car_metrics[car_id]["wait_intersection"] = None if wait_t < 0 else wait_t
            continue

        m = RE_AMBULANCE_WAIT.search(line)
        if m:
            car_id = m.group(1)
            ambulance_ids.add(car_id)
            car_metrics[car_id]["role"] = "ambulance"
            car_metrics[car_id]["epoch"] = int(m.group(3))
            car_metrics[car_id]["wait_intersection"] = float(m.group(2))
            continue

        m = RE_BYZANTINE_INJECTION.search(line)
        if m:
            replica = int(m.group(1))
            cid = int(m.group(2))
            epoch = cid // 2   # CID 0,2,4,6→epoch 0,1,2,3  (VIEW & ORDER share the same epoch)
            byzantine_by_epoch[epoch] += 1
            byzantine_by_replica[replica] += 1
            continue

        m = RE_CONSENSUS_HEADER.search(line)
        if m:
            rep, epoch = int(m.group(1)), int(m.group(2))
            replica_epoch[rep] = epoch
            current_gossip_epoch = epoch
            continue

        m = RE_PHASE_SUMMARY.search(line)
        if m:
            rep = int(m.group(1))
            if m.group(2) is not None:
                ep = int(m.group(2))
                replica_epoch[rep] = ep
            else:
                ep = replica_epoch.get(rep, current_gossip_epoch)
            car_id = f"veh{rep}"
            if m.group(3):
                val = float(m.group(3))
                propose_all_sim_raw[ep].append(val)
                car_metrics[car_id]["propose_all_consensus_sim_s"] = val
            if m.group(4):
                val = float(m.group(4))
                stop_to_decision_sim_raw[ep].append(val)
                car_metrics[car_id]["stop_to_decision_sim_s"] = val
            continue

        m = RE_CERT_DUR.search(line)
        if m:
            rep, val = int(m.group(1)), float(m.group(2))
            ep = replica_epoch.get(rep, current_gossip_epoch)
            car_id = f"veh{rep}"
            car_metrics[car_id]["cert_collection_s"] = val
            round_metrics[ep].setdefault("Cert_Collection_Duration", val)
            continue

        m = RE_CERT_CREATION_LATENCY.search(line)
        if m:
            rep, val = int(m.group(1)), float(m.group(2))
            ep = replica_epoch.get(rep, current_gossip_epoch)
            car_id = f"veh{rep}"
            car_metrics[car_id]["cert_creation_s"] = val
            epoch_cert_creation_latency[ep].append(val)
            round_metrics[ep].setdefault("Cert_Creation_Latency", val)
            continue

        m = RE_BATCH_ASSIGN.search(line)
        if m:
            rep, batch = int(m.group(1)), int(m.group(2))
            replica_batch_index[rep] = batch
            car_metrics[f"veh{rep}"]["batch_index"] = batch
            continue

        m = RE_TOLERATED_F_CONFIG.search(line)
        if m:
            experiment_fault_tolerance["tolerated_f"] = int(m.group(1))
            experiment_fault_tolerance["static_replicas"] = int(m.group(2))
            experiment_fault_tolerance["consensus_quorum"] = int(m.group(3))
            experiment_fault_tolerance["cert_threshold"] = int(m.group(4))
            try:
                expected_q = bft_quorum(
                    experiment_fault_tolerance["static_replicas"],
                    experiment_fault_tolerance["tolerated_f"],
                )
                if expected_q != experiment_fault_tolerance["consensus_quorum"]:
                    r0_metrics["bad_quorum_count"] += 1
            except ValueError:
                r0_metrics["bad_quorum_count"] += 1
            continue

        m = RE_R0_CANCEL_WITNESS.search(line)
        if m:
            r0_metrics["enabled"] = True
            r0_metrics["cancel_witness_count"] += 1
            continue

        m = RE_R0_CANCEL_ECHO.search(line)
        if m:
            r0_metrics["enabled"] = True
            r0_metrics["cancel_echo_count"] += 1
            continue

        m = RE_R0_CANCEL_CERT.search(line)
        if m:
            r0_metrics["enabled"] = True
            r0_metrics["cancel_cert_count"] += 1
            continue

        m = RE_R0_ROLLBACK_BEGIN.search(line)
        if m:
            r0_metrics["enabled"] = True
            r0_metrics["rollback_begin_seen"] = True
            continue

        m = RE_R0_CANCEL_COMMIT.search(line)
        if m:
            r0_metrics["enabled"] = True
            r0_metrics["cancel_commit_count"] += 1
            continue

        m = RE_R0_ROLLBACK_COMMIT.search(line)
        if m:
            r0_metrics["enabled"] = True
            r0_metrics["rollback_commit_count"] += 1
            r0_metrics["rollback_commit_replica_ids"].append(int(m.group(1)))
            if int(m.group(3)) == 1:
                r0_metrics["rollback_epoch1_committed"] = True
            continue

        m = RE_R0_ROLLBACK_PROPOSE_RC.search(line)
        if m:
            r0_metrics["enabled"] = True
            if int(m.group(2)) == 0:
                r0_metrics["rollback_propose_rc0"] = True
            continue

        m = RE_R0_ROLLBACK_DISCOVERY_SNAPSHOT.search(line)
        if m:
            r0_metrics["enabled"] = True
            r0_metrics["rollback_mode"] = m.group(3)
            r0_metrics["rollback_discovery_snapshots"].append({
                "replica": int(m.group(1)),
                "reason": m.group(2),
                "mode": m.group(3),
                "signed": int(m.group(4)),
                "quiet": int(m.group(5)),
                "vote_n": int(m.group(6)),
                "scene_n": int(m.group(7)),
                "expected_n": int(m.group(8)),
            })
            continue

        m = RE_R0_ACTIVE_VIEW_EPOCH1.search(line)
        if m:
            r0_metrics["enabled"] = True
            view = {
                "seq": int(m.group(1)),
                "hash": m.group(2),
                "vote_n": int(m.group(3)),
                "f": int(m.group(4)),
                "quorum_used": int(m.group(5)),
                "proposer": int(m.group(6)),
                "new_epoch": 1,
            }
            try:
                view["expected_quorum"] = bft_quorum(view["vote_n"], view["f"])
            except ValueError:
                view["expected_quorum"] = None
            if view["seq"] in (0, 3) and view["expected_quorum"] != view["quorum_used"]:
                r0_metrics["bad_quorum_count"] += 1
            r0_metrics["rollback_active_views"].append(view)
            continue

        m = RE_R0_EXECUTOR_ORDER_EPOCH.search(line)
        if m:
            epoch = int(m.group(1))
            n_vehicles = int(m.group(2))
            n_batches = int(m.group(3))
            if epoch == 1 and n_vehicles > 0:
                r0_metrics["enabled"] = True
                r0_metrics["epoch1_order_decision_count"] += 1
                r0_metrics["epoch1_order_n_vehicles"] = n_vehicles
                r0_metrics["epoch1_order_n_batches"] = n_batches
                decision_blob = m.group(4) or ""
                order_vehicle_ids = [
                    int(v) for v in re.findall(r'\bveh=(\d+)\b', decision_blob)
                ]
                if order_vehicle_ids:
                    r0_metrics["epoch1_order_vehicle_ids"] = order_vehicle_ids
                r0_metrics["rollback_epoch1_committed"] = True
            continue

        m = RE_PBFT_NEW_REQ.search(line)
        if m:
            if r0_metrics["rollback_propose_rc0"]:
                r0_metrics["rollback_pbft_new_req_after_propose"] = True
            continue

        m = RE_R0_ROLLBACK_QUORUM_V3.search(line)
        if m:
            r0_metrics["enabled"] = True
            vote_n = int(m.group(3))
            f_used = int(m.group(5))
            quorum_used = int(m.group(6))
            mode = m.group(2)
            r0_metrics["rollback_mode"] = mode
            try:
                expected_q = bft_quorum(vote_n, f_used)
            except ValueError:
                expected_q = None
            if expected_q != quorum_used:
                r0_metrics["bad_quorum_count"] += 1
            r0_metrics["rollback_quorums"].append({
                "mode": mode,
                "vote_n": vote_n,
                "scene_n": int(m.group(4)),
                "f": f_used,
                "quorum_used": quorum_used,
                "expected_quorum": expected_q,
                "proposer": int(m.group(7)),
                "new_epoch": int(m.group(8)),
            })
            continue

        m = RE_R0_ROLLBACK_RECONFIG.search(line)
        if m:
            r0_metrics["enabled"] = True
            r0_metrics["rollback_mode"] = m.group(2)
            r0_metrics["rollback_reconfigs"].append({
                "mode": m.group(2),
                "old_epoch": int(m.group(3)),
                "new_epoch": int(m.group(4)),
                "vote_n": int(m.group(5)),
                "scene_n": int(m.group(6)),
                "new_f": int(m.group(7)),
                "quorum": int(m.group(8)),
            })
            continue

        m = RE_R0_ROLLBACK_QUORUM.search(line)
        if m:
            r0_metrics["enabled"] = True
            member_count = int(m.group(2))
            f_anchored = int(m.group(3))
            quorum_used = int(m.group(4))
            try:
                expected_q = bft_quorum(member_count, f_anchored)
            except ValueError:
                expected_q = None
            if expected_q != quorum_used:
                r0_metrics["bad_quorum_count"] += 1
            r0_metrics["rollback_quorums"].append({
                "member_count": member_count,
                "f_anchored": f_anchored,
                "quorum_used": quorum_used,
                "expected_quorum": expected_q,
                "proposer": int(m.group(5)),
                "new_epoch": int(m.group(6)),
            })
            continue

        m = RE_R0_ROLLBACK_UNAVAILABLE_V3.search(line)
        if m:
            r0_metrics["enabled"] = True
            r0_metrics["rollback_unavailable_count"] += 1
            vote_n = int(m.group(2))
            need = int(m.group(5))
            r0_metrics["rollback_mode"] = m.group(1)
            if vote_n > r0_metrics["rollback_unavailable_max_m"]:
                r0_metrics["rollback_unavailable_max_m"] = vote_n
                f_val = m.group(3) or m.group(4)
                r0_metrics["rollback_unavailable_f"] = int(f_val) if f_val else None
                r0_metrics["rollback_unavailable_need"] = need
            continue

        m = RE_R0_ROLLBACK_UNAVAILABLE.search(line)
        if m:
            r0_metrics["enabled"] = True
            r0_metrics["rollback_unavailable_count"] += 1
            member_count = int(m.group(1))
            if member_count > r0_metrics["rollback_unavailable_max_m"]:
                r0_metrics["rollback_unavailable_max_m"] = member_count
                r0_metrics["rollback_unavailable_f"] = int(m.group(2))
                r0_metrics["rollback_unavailable_need"] = int(m.group(4))
            continue

        if RE_R0_ROLLBACK_PROPOSE_SKIP_MINUS_ONE.search(line):
            r0_metrics["enabled"] = True
            r0_metrics["proposer_minus_one_skip_count"] += 1
            continue

        # [RUN-METRICS] summary lines (read back from saved log files)
        m = RE_RUN_METRIC.search(line)
        if m:
            run_metrics[m.group(1)] = float(m.group(2))
            continue

        # Per-car [ROUND-METRICS] Arrival_To_Resume_Time
        m = RE_ROUND_METRIC_CAR.search(line)
        if m:
            epoch, car_id, val = int(m.group(1)), m.group(2), float(m.group(3))
            car_metrics[car_id].setdefault("epoch", epoch)
            go_decision_epoch.setdefault(car_id, epoch)
            car_metrics[car_id]["stop_to_resume"] = val
            continue

        # General [ROUND-METRICS] Epoch N Key: value (catch remaining epoch-keyed lines)
        m = RE_ROUND_METRIC_KEY.search(line)
        if m:
            epoch, key, val = int(m.group(1)), m.group(2), float(m.group(3))
            round_metrics[epoch].setdefault(key, val)
            continue

if _args.baseline_tripinfo:
    load_baseline_tripinfo(_args.baseline_tripinfo, _args.scenario_ini)

print("Done.\n")
byzantine_total = sum(byzantine_by_epoch.values())

if experiment_fault_tolerance["tolerated_f"] is None and "Tolerated_F" in run_metrics:
    experiment_fault_tolerance["tolerated_f"] = int(run_metrics["Tolerated_F"])
    experiment_fault_tolerance["static_replicas"] = int(
        run_metrics.get("Static_Replicas", -1)
    )
    experiment_fault_tolerance["consensus_quorum"] = int(
        run_metrics.get("Consensus_Quorum", -1)
    )
    experiment_fault_tolerance["cert_threshold"] = int(
        run_metrics.get("Cert_Threshold", -1)
    )

if _args.scenario == 15:
    r0_metrics["enabled"] = True
if r0_metrics["enabled"]:
    if r0_metrics["bad_quorum_count"] > 0:
        r0_metrics["failure_stage"] = "bad_quorum"
    elif (r0_metrics["proposer_minus_one_skip_count"] > 0
          and not r0_metrics["rollback_quorums"]
          and r0_metrics["rollback_unavailable_count"] == 0):
        r0_metrics["failure_stage"] = "silent_proposer_minus_one"
    elif (r0_metrics["rollback_unavailable_count"] > 0
          and not r0_metrics["rollback_quorums"]):
        r0_metrics["failure_stage"] = "rollback_unavailable"
    elif r0_metrics["cancel_echo_count"] == 0:
        r0_metrics["failure_stage"] = "no_echo"
    elif r0_metrics["cancel_cert_count"] == 0:
        r0_metrics["failure_stage"] = "no_cert"
    elif r0_metrics["rollback_epoch1_committed"]:
        latest_view = (
            r0_metrics["rollback_active_views"][-1]
            if r0_metrics["rollback_active_views"]
            else None
        )
        if latest_view and latest_view.get("expected_quorum") != latest_view.get("quorum_used"):
            r0_metrics["failure_stage"] = "unexpected_epoch1_quorum"
        elif latest_view:
            r0_metrics["failure_stage"] = "ok_epoch1_commit"
        else:
            r0_metrics["failure_stage"] = "ok_epoch1_commit_no_view_log"
    elif _args.scenario == 15 and r0_metrics["rollback_quorums"]:
        last = r0_metrics["rollback_quorums"][-1]
        committed = (
            r0_metrics["rollback_epoch1_committed"]
            or r0_metrics["rollback_pbft_new_req_after_propose"]
        )
        if not committed:
            r0_metrics["failure_stage"] = "rollback_propose_no_commit"
        elif last.get("mode") == "per_epoch":
            if last.get("vote_n", 0) >= 12 and last.get("f") == 4 and last.get("quorum_used") == 10:
                r0_metrics["failure_stage"] = "ok_per_epoch_reconfig"
            else:
                r0_metrics["failure_stage"] = "unexpected_per_epoch_quorum"
        else:
            r0_metrics["failure_stage"] = "ok_rollback_quorum"
    elif r0_metrics["rollback_quorums"]:
        committed = (
            r0_metrics["rollback_epoch1_committed"]
            or r0_metrics["rollback_pbft_new_req_after_propose"]
        )
        if committed:
            r0_metrics["failure_stage"] = "ok_rollback_quorum"
        else:
            r0_metrics["failure_stage"] = "rollback_propose_no_commit"
    else:
        r0_metrics["failure_stage"] = "unknown_pending_full_predicates"
    order_members = set(r0_metrics["epoch1_order_vehicle_ids"])
    commit_members = set(r0_metrics["rollback_commit_replica_ids"])
    r0_metrics["rollback_commit_extra_replica_ids"] = sorted(
        commit_members - order_members
    )
    r0_metrics["rollback_commit_missing_replica_ids"] = sorted(
        order_members - commit_members
    )

def normalize_java_epoch(raw_epoch):
    if raw_epoch in round_metrics and (raw_epoch - 1) not in round_metrics:
        return raw_epoch
    if (raw_epoch - 1) in round_metrics and raw_epoch not in round_metrics:
        return raw_epoch - 1
    if 0 <= raw_epoch - 1 < N_EPOCHS:
        return raw_epoch - 1
    if 0 <= raw_epoch < N_EPOCHS:
        return raw_epoch
    return raw_epoch

# ResDB: commit waves from Order_Decided, then:
#   ProposeAll_Consensus_Sim = median(commit wave) - max(submit in window)  (winning leader→commit)
#   ProposeAll_FirstSubmit_To_OrderCommit_Sim = median(commit) - min(submit) (failed primary + VC + …)
_resdb_waves = compute_resdb_submit_to_commit_by_wave(
    resdb_propose_submit_events,
    resdb_order_decided_events,
    cluster_gap_s=2.0,
    max_epochs=N_EPOCHS,
)
_epochs_resdb_submit_order = set()
for row in _resdb_waves:
    ep = row["epoch"]
    round_metrics[ep]["ProposeAll_Consensus_Sim"] = row["leader_submit_to_commit_s"]
    round_metrics[ep]["ProposeAll_FirstSubmit_To_OrderCommit_Sim"] = row[
        "first_submit_to_commit_s"
    ]
    _epochs_resdb_submit_order.add(ep)
    car_metrics[f"veh{row['winning_rep']}"]["propose_all_consensus_sim_s"] = row[
        "leader_submit_to_commit_s"
    ]
    car_metrics[f"veh{row['winning_rep']}"]["propose_all_first_submit_to_commit_sim_s"] = row[
        "first_submit_to_commit_s"
    ]

for ep, samples in propose_all_sim_raw.items():
    if ep in _epochs_resdb_submit_order:
        continue
    if samples:
        mean_bft = statistics.mean(samples)
        round_metrics[ep]["ProposeAll_Consensus_Sim"] = mean_bft
        # No ResDB submit/order lines: PHASE_SUMMARY is one leader path — first submit == last.
        round_metrics[ep]["ProposeAll_FirstSubmit_To_OrderCommit_Sim"] = mean_bft

for ep, samples in stop_to_decision_sim_raw.items():
    if samples:
        round_metrics[ep]["Stop_To_Decision_Sim"] = statistics.mean(samples)

# Compute per-epoch total durations: arrival at intersection zone → resume.
# Uses Arrival_Time (car first enters zone, even if queued) so queue wait is included.
# Falls back to Stop_Time if Arrival_Time is absent (older logs).
for rep, resume_t in replica_resume_time.items():
    arrival_t = replica_arrival_time.get(rep, replica_stop_time.get(rep))
    if arrival_t is not None:
        dur = resume_t - arrival_t
        car_id = f"veh{rep}"
        ep = go_decision_epoch.get(car_id)
        if isinstance(ep, int):
            epoch_total_dur[ep].append(dur)
            epoch_total_dur_by_car[ep][car_id] = dur

for rep, arrival_t in replica_arrival_time.items():
    car_metrics[f"veh{rep}"]["arrival_time"] = arrival_t
for rep, stop_t in replica_stop_time.items():
    car_metrics[f"veh{rep}"]["stop_time"] = stop_t
for rep, resume_t in replica_resume_time.items():
    car_metrics[f"veh{rep}"]["resume_time"] = resume_t

# ── Per-car phase breakdown: cert_wait / bft_wait / queue_wait ───────────────
# For each epoch, find the leader (only replica with cert_collection_s set).
# Derive proposeSubmitTime = orderTime - bftSim, then for every car in that epoch:
#   cert_wait  = max(0, proposeSubmitTime - car_stop_time)  [waiting for cert collection]
#   bft_wait   = bftSim                                      [stable BFT consensus]
#   queue_wait = resume_time - orderTime                     [0 for batch-0, >0 for later batches]
for ep in sorted(set(car_metrics[c].get("epoch", -1) for c in car_metrics)):
    if ep < 0:
        continue
    # Only compute phases for epochs where we found the PROPOSE_ALL leader
    # (cert_collection_s is only logged by the replica that submits PROPOSE_ALL).
    leader_car = None
    for cid, cm in car_metrics.items():
        if cm.get("epoch") == ep and cm.get("cert_collection_s") is not None:
            leader_car = cid
            break
    if leader_car is None:
        continue  # No leader data for this epoch → leave phase fields absent
    lcm = car_metrics[leader_car]
    ep_bft = lcm.get("propose_all_consensus_sim_s")
    l_stop = lcm.get("stop_time")
    l_std  = lcm.get("stop_to_decision_sim_s")
    if None in (ep_bft, l_stop, l_std):
        continue
    ep_order_time   = l_stop + l_std
    ep_propose_time = ep_order_time - ep_bft
    round_metrics[ep]["epoch_order_sim_time"]   = ep_order_time
    round_metrics[ep]["epoch_propose_sim_time"]  = ep_propose_time
    # Only tag cars actually in this epoch (epoch tag must match)
    for cid, cm in car_metrics.items():
        if cm.get("epoch") != ep:
            continue
        c_stop   = cm.get("stop_time")
        c_resume = cm.get("resume_time")
        c_std    = cm.get("stop_to_decision_sim_s")
        if c_stop is None:
            continue
        c_order_end = (c_stop + c_std) if c_std is not None else ep_order_time
        cm["cert_wait_s"]      = max(0.0, ep_propose_time - c_stop)
        cm["bft_wait_s"]       = ep_bft
        # Order delivery: gap between when leader decided and when THIS replica got it.
        # Non-zero because 802.11p DECIDE delivery is not instantaneous.
        cm["order_delivery_s"] = round(c_order_end - ep_order_time, 4)
        cm["queue_wait_s"]     = round(c_resume - c_order_end, 4) if c_resume is not None else None

# Per-car message counts, fallback flag, delivery ratio, and BFT decision time
for rep, sent in replica_messages_sent.items():
    car_metrics[f"veh{rep}"]["messages_sent"] = sent
for rep, recv in replica_messages_recv.items():
    car_metrics[f"veh{rep}"]["messages_received"] = recv
for rep, sent_bytes in replica_bytes_sent.items():
    car_metrics[f"veh{rep}"]["bytes_sent"] = sent_bytes
for rep in replica_stopsign_timeout:
    car_metrics[f"veh{rep}"]["used_fallback"] = True

for car_id, metrics in car_metrics.items():
    rep  = int(car_id[3:])
    sent = metrics.get("messages_sent")
    recv = metrics.get("messages_received")
    ep   = metrics.get("epoch")
    n    = EPOCH_N.get(ep, 0) if isinstance(ep, int) else 0
    if sent is not None and recv is not None and n > 1:
        expected = sent * (n - 1)
        ratio = recv / expected if expected > 0 else None
        metrics["delivery_ratio"] = ratio
        metrics["estimated_loss_rate"] = (1.0 - ratio) if ratio is not None else None

# Per-epoch throughput (user definition):
#   throughput_veh_per_s = n_vehicles_in_epoch / mean(depart - stop) over those vehicles
#   throughput_s_per_veh = mean(depart - stop) / n_vehicles_in_epoch  (stored value)
# `wait_intersection` already equals `depart_time - stop_time` per car (line ~694).
# Cars with missing stop or depart are naturally excluded.
epoch_depart_times   = defaultdict(list) # epoch -> sorted depart_times (kept for diagnostics)
epoch_throughput     = {}                # epoch -> s/veh under the user formula
epoch_wait_normal    = defaultdict(list) # epoch -> [wait_s, ...] for normal cars
epoch_wait_ambulance = defaultdict(list) # epoch -> [wait_s, ...] for ambulance cars

for car_id, metrics in car_metrics.items():
    if "role" not in metrics:
        metrics["role"] = "ambulance" if car_id in ambulance_ids else "normal"
    arrival_t = metrics.get("arrival_time")
    stop_t = metrics.get("stop_time")
    resume_t = metrics.get("resume_time")
    depart_t = metrics.get("depart_time")
    if stop_t is not None and depart_t is not None and "wait_intersection" not in metrics:
        metrics["wait_intersection"] = depart_t - stop_t
    base_t = stop_t if stop_t is not None else arrival_t
    if base_t is not None and resume_t is not None:
        metrics["stop_to_resume"] = resume_t - base_t
    if resume_t is not None and depart_t is not None:
        metrics["resume_to_depart"] = depart_t - resume_t
    if arrival_t is not None and depart_t is not None:
        metrics["arrival_to_depart"] = depart_t - arrival_t

for car_id, metrics in car_metrics.items():
    ep = metrics.get("epoch")
    if not isinstance(ep, int):
        continue
    dep = metrics.get("depart_time")
    if dep is not None:
        epoch_depart_times[ep].append(dep)
    wait_t = metrics.get("wait_intersection")
    role   = metrics.get("role", "normal")
    if wait_t is not None:
        if role == "ambulance":
            epoch_wait_ambulance[ep].append(wait_t)
        else:
            epoch_wait_normal[ep].append(wait_t)

for ep in set(list(epoch_wait_normal.keys()) + list(epoch_wait_ambulance.keys())):
    waits_ep = epoch_wait_normal.get(ep, []) + epoch_wait_ambulance.get(ep, [])
    if waits_ep:
        mean_wait = statistics.mean(waits_ep)
        if mean_wait > 0:
            epoch_throughput[ep] = mean_wait / len(waits_ep)  # s/veh

os.makedirs(PLOTS_DIR, exist_ok=True)

def _ms_or_none(seconds):
    return None if seconds is None else seconds * 1000.0

def _first_event_time_by_replica(events):
    """Return replica->first_time for [(rep, t_s), ...] in log order."""
    out = {}
    for rep, t_s in events:
        if rep not in out:
            out[rep] = t_s
    return out

def _all_event_times_by_replica(events):
    """Return replica->sorted list of times for [(rep, t_s), ...]."""
    out = defaultdict(list)
    for rep, t_s in events:
        out[rep].append(t_s)
    for rep in out:
        out[rep].sort()
    return out

def _view_change_duration_ms(replica_id: int):
    """Per-replica VC duration: VC-TRIGGER time -> first subsequent primary-changed time."""
    if replica_id not in replica_vc_trigger_time:
        return None
    t0 = replica_vc_trigger_time[replica_id]
    for t1 in replica_primary_change_times.get(replica_id, []):
        if t1 >= t0:
            return (t1 - t0) * 1000.0
    return None

def _epoch_view_change_duration_ms(epoch: int):
    """Global VC duration for the epoch: earliest VC-TRIGGER -> earliest primary-changed."""
    if epoch not in vc_trigger_times_by_epoch or epoch not in primary_change_times_by_epoch:
        return None
    if not vc_trigger_times_by_epoch[epoch] or not primary_change_times_by_epoch[epoch]:
        return None
    t0 = min(vc_trigger_times_by_epoch[epoch])
    t1 = min(primary_change_times_by_epoch[epoch])
    return max(0.0, (t1 - t0) * 1000.0)

def _epoch_commit_time(epoch: int):
    """Approximate commit/decision baseline as earliest Order_Decided_Time in the epoch."""
    ts = order_decided_times_by_epoch.get(epoch, [])
    return min(ts) if ts else None

def _epoch_first_propose_time(epoch: int):
    ts = propose_submit_times_by_epoch.get(epoch, [])
    return min(ts) if ts else None

def _epoch_winning_propose_time(epoch: int):
    """
    ProposeAll_Submit_Time that most likely led to the committed order.

    Heuristic: choose the latest propose time <= earliest Order_Decided_Time in the epoch.
    This excludes an early/byzantine propose that never committed before a view change.
    """
    commit_t = _epoch_commit_time(epoch)
    ts = propose_submit_times_by_epoch.get(epoch, [])
    if not ts:
        return None
    if commit_t is None:
        return max(ts)
    eligible = [t for t in ts if t <= commit_t]
    return max(eligible) if eligible else min(ts)

def _epoch_cert_collection_duration_ms(epoch: int):
    rm = round_metrics.get(epoch, {})
    if "Cert_Collection_Duration" in rm and rm["Cert_Collection_Duration"] is not None:
        return rm["Cert_Collection_Duration"] * 1000.0
    t_start = cert_collection_start_by_epoch.get(epoch)
    t_prop = _epoch_first_propose_time(epoch)
    if t_start is not None and t_prop is not None and t_prop >= t_start:
        return (t_prop - t_start) * 1000.0
    return None

def write_metrics_json(path):
    """Write all extracted metrics to a structured JSON file for downstream plotting."""
    import json

    def _stat(values):
        if not values:
            return None
        return {
            "mean": statistics.mean(values),
            "median": statistics.median(values),
            "p95": percentile(values, 0.95),
            "min": min(values),
            "max": max(values),
            "n": len(values),
        }

    epochs_out = []
    for ep in range(N_EPOCHS):
        n = EPOCH_N[ep]
        rm = round_metrics.get(ep, {})
        tp = epoch_throughput.get(ep)
        wn = epoch_wait_normal.get(ep, [])
        wa = epoch_wait_ambulance.get(ep, [])
        byz = byzantine_by_epoch.get(ep, 0)
        msgs_sent = epoch_messages_sent.get(ep, [])
        msgs_recv = epoch_messages_recv.get(ep, [])
        bytes_sent = epoch_bytes_sent.get(ep, [])
        cert_creation = epoch_cert_creation_latency.get(ep, [])
        propose_all_sim = rm.get("ProposeAll_Consensus_Sim")
        first_submit_commit = rm.get("ProposeAll_FirstSubmit_To_OrderCommit_Sim")
        epochs_out.append({
            "epoch": ep,
            "n_replicas": n,
            "tolerated_f": experiment_fault_tolerance["tolerated_f"],
            "static_replicas": experiment_fault_tolerance["static_replicas"],
            "consensus_quorum": experiment_fault_tolerance["consensus_quorum"],
            "cert_threshold": experiment_fault_tolerance["cert_threshold"],
            "propose_all_consensus_latency_sim_s": propose_all_sim,
            "propose_all_first_submit_to_commit_sim_s": first_submit_commit,
            "stop_to_decision_sim_s": rm.get("Stop_To_Decision_Sim"),
            "cert_creation_latency_ms": (
                statistics.mean(cert_creation) * 1000.0 if cert_creation else None
            ),
            "throughput_s_per_veh": tp,
            "throughput_veh_per_s": (1.0 / tp) if tp not in (None, 0) else None,
            "wait_normal_s": _stat(wn),
            "wait_ambulance_s": _stat(wa),
            "byzantine_injections": byz,
            "consensus_success": bool(propose_all_sim),
            "avg_messages_sent_per_replica": (sum(msgs_sent) / len(msgs_sent)) if msgs_sent else None,
            "avg_messages_recv_per_replica": (sum(msgs_recv) / len(msgs_recv)) if msgs_recv else None,
            "avg_bytes_sent_per_replica": (sum(bytes_sent) / len(bytes_sent)) if bytes_sent else None,
            "total_bytes_sent": sum(bytes_sent) if bytes_sent else None,
            "total_megabytes_sent": (sum(bytes_sent) / (1024.0 * 1024.0)) if bytes_sent else None,
            "stop_sign_failures": epoch_failures.get(ep, 0),
        })

    cars_out = []
    for car_id in sorted(car_metrics.keys(), key=lambda cid: int(cid[3:])):
        m = car_metrics[car_id]
        cars_out.append({
            "car_id": car_id,
            "role": m.get("role", "normal"),
            "epoch": m.get("epoch"),
            "arrival_time_s": m.get("arrival_time"),
            "stop_time_s": m.get("stop_time"),
            "resume_time_s": m.get("resume_time"),
            "depart_time_s": m.get("depart_time"),
            "wait_intersection_s": m.get("wait_intersection"),
            "stop_to_resume_s": m.get("stop_to_resume"),
            "resume_to_depart_s": m.get("resume_to_depart"),
            "arrival_to_depart_s": m.get("arrival_to_depart"),
            # Message-level metrics (matches RAFT messages_sent/received)
            "messages_sent": m.get("messages_sent"),
            "messages_received": m.get("messages_received"),
            "bytes_sent": m.get("bytes_sent"),
            "megabytes_sent": (
                m.get("bytes_sent") / (1024.0 * 1024.0)
                if m.get("bytes_sent") is not None else None
            ),
            # delivery_ratio = received / (sent * (N-1)); heuristic for message loss
            "delivery_ratio": m.get("delivery_ratio"),
            "estimated_loss_rate": m.get("estimated_loss_rate"),
            # Fallback: True if this vehicle hit a StopSign_Timeout
            "used_fallback": m.get("used_fallback", False),
            "propose_all_consensus_sim_s": m.get("propose_all_consensus_sim_s"),
            "propose_all_first_submit_to_commit_sim_s": m.get(
                "propose_all_first_submit_to_commit_sim_s"
            ),
            "stop_to_decision_sim_s": m.get("stop_to_decision_sim_s"),
            # Phase breakdown: stop → PROPOSE_ALL submit → ORDER decision → resume
            "cert_wait_s": m.get("cert_wait_s"),
            "bft_wait_s": m.get("bft_wait_s"),
            "order_delivery_s": m.get("order_delivery_s"),
            "queue_wait_s": m.get("queue_wait_s"),
        })

    fa = jains_fairness(all_waits)
    fn = jains_fairness(normal_waits)

    # Run-level delivery and fallback metrics (mirrors RAFT delivery_ratio / fallback_rate)
    _all_dr = [m["delivery_ratio"] for m in car_metrics.values()
               if m.get("delivery_ratio") is not None]
    _run_dr = statistics.mean(_all_dr) if _all_dr else None
    _run_lr = (1.0 - _run_dr) if _run_dr is not None else None
    _fallback_n = sum(1 for m in car_metrics.values() if m.get("used_fallback", False))
    _run_fallback_rate = _fallback_n / CARS if CARS > 0 else None
    _run_bytes_sent = sum(replica_bytes_sent.values()) if replica_bytes_sent else None
    _quiet_honest_n = sum(replica_quiet_honest_vehicles.values())
    _quiet_honest_d = sum(replica_quiet_honest_opportunities.values())
    _quiet_honest_rate = (
        100.0 * _quiet_honest_n / _quiet_honest_d
        if _quiet_honest_d > 0 else None
    )
    _attack_outcomes_unique = [
        row
        for ep in sorted(attack_outcomes_by_epoch)
        for row in attack_outcomes_by_epoch[ep]
    ]
    _attack_failures_total = sum(attack_failures_by_epoch.values())

    overall = {
        "cars": CARS,
        "epochs": N_EPOCHS,
        "fault_tolerance_experiment": experiment_fault_tolerance,
        "throughput_s_per_veh": throughput,
        "throughput_veh_per_s": throughput_vps,
        "throughput_formula": "n_cars / mean(depart_time - stop_time)  (s/veh stored as inverse)",
        "wait_all_s": _stat(all_waits),
        "wait_normal_s": _stat(normal_waits),
        "wait_ambulance_s": _stat(ambulance_waits),
        "jains_fairness_all": fa,
        "jains_fairness_normal": fn,
        "ambulance_priority_gain_s": (statistics.mean(normal_waits) - statistics.mean(ambulance_waits))
            if normal_waits and ambulance_waits else None,
        "ambulance_priority_ratio": (statistics.mean(ambulance_waits) / statistics.mean(normal_waits))
            if normal_waits and ambulance_waits and statistics.mean(normal_waits) != 0 else None,
        # Batch throughput: vehicles / (last depart - first stop) when batch data available.
        "batch_throughput_veh_per_s": (
            (lambda: CARS / (max(departs) - min(stops))
             if (stops := [m["stop_time"] for m in car_metrics.values() if m.get("stop_time") is not None])
                and (departs := [m["depart_time"] for m in car_metrics.values() if m.get("depart_time") is not None])
                and max(departs) > min(stops) else None)()
        ),
        "batch_index_distribution": (
            {str(bi): sum(1 for v in replica_batch_index.values() if v == bi)
             for bi in sorted(set(replica_batch_index.values()))}
            if replica_batch_index else None
        ),
        "propose_all_consensus_latency_sim_s": _stat([
            round_metrics[ep]["ProposeAll_Consensus_Sim"]
            for ep in range(N_EPOCHS)
            if round_metrics.get(ep, {}).get("ProposeAll_Consensus_Sim", 0) > 0
        ]),
        "propose_all_first_submit_to_commit_latency_sim_s": _stat([
            round_metrics[ep]["ProposeAll_FirstSubmit_To_OrderCommit_Sim"]
            for ep in range(N_EPOCHS)
            if round_metrics.get(ep, {}).get("ProposeAll_FirstSubmit_To_OrderCommit_Sim", 0) > 0
        ]),
        "cert_creation_latency_ms": _stat([
            value * 1000.0
            for ep in range(N_EPOCHS)
            for value in epoch_cert_creation_latency.get(ep, [])
            if value > 0
        ]),
        "stop_to_decision_sim_s": _stat([
            round_metrics[ep]["Stop_To_Decision_Sim"]
            for ep in range(N_EPOCHS)
            if round_metrics.get(ep, {}).get("Stop_To_Decision_Sim", 0) > 0
        ]),
        "total_byzantine_injections": byzantine_total,
        "byzantine_by_epoch": dict(byzantine_by_epoch),
        "byzantine_by_replica": dict(byzantine_by_replica),
        "attack_success": _attack_failures_total > 0,
        "attack_failures_total": _attack_failures_total,
        "attack_failures_by_epoch": dict(attack_failures_by_epoch),
        "attack_outcomes": _attack_outcomes_unique,
        "attack_outcome_lines_total": len(attack_outcomes),
        "stop_to_resume_s": _stat(stop_to_resume_all),
        "resume_to_depart_s": _stat(resume_to_depart_all),
        # Delivery and fallback metrics (same definitions as RAFT counterpart)
        # delivery_ratio = received / (sent * (N-1)); heuristic for message loss
        "delivery_ratio": _run_dr,
        "estimated_loss_rate": _run_lr,
        # fallback_rate = fraction of vehicles that hit StopSign_Timeout this run
        "fallback_rate": _run_fallback_rate,
        "fallback_count": _fallback_n,
        # Payload bytes emitted onto the radio, post signing/wrapping.
        "bytes_sent_total": _run_bytes_sent,
        "megabytes_sent_total": (
            _run_bytes_sent / (1024.0 * 1024.0)
            if _run_bytes_sent is not None else None
        ),
        "quiet_honest_count": _quiet_honest_n,
        "quiet_honest_opportunities": _quiet_honest_d,
        "quiet_honest_rate_percent": _quiet_honest_rate,
        "r0_late_emergency": r0_metrics if r0_metrics["enabled"] else None,
    }

    if _args.output_format == "legacy":
        out = {"overall": overall, "per_epoch": epochs_out, "per_car": cars_out}
    else:
        propose_by_replica = _first_event_time_by_replica(resdb_propose_submit_events)
        decided_by_replica = _first_event_time_by_replica(resdb_order_decided_events)
        decided_all_by_replica = _all_event_times_by_replica(resdb_order_decided_events)

        vehicles_out = []
        for rep in range(CARS):
            cid = f"veh{rep}"
            m = car_metrics.get(cid, {})
            ep = m.get("epoch")

            stop_s = m.get("stop_time")
            depart_s = m.get("depart_time")
            stopped_ms = _ms_or_none(stop_s)
            passed_ms = _ms_or_none(depart_s)

            total_wait_ms = None
            if stop_s is not None and depart_s is not None:
                total_wait_ms = max(0.0, (depart_s - stop_s) * 1000.0)

            # decision_latency_ms (plotting): "first ProposeAll_Submit_Time (incl. byzantine leader) -> this replica received decision".
            decision_latency_ms = None
            first_propose_t = _epoch_first_propose_time(ep) if isinstance(ep, int) else None
            decide_recv_t = (decided_all_by_replica.get(rep) or [None])[0]
            if decide_recv_t is None and isinstance(ep, int):
                decide_recv_t = replica_gossip_send_time_by_epoch.get((rep, ep))
            if first_propose_t is not None and decide_recv_t is not None and decide_recv_t >= first_propose_t:
                decision_latency_ms = (decide_recv_t - first_propose_t) * 1000.0

            # Full decision latency (end-to-end for protocol): first propose submit in the epoch
            # (incl. a byzantine propose that later triggers view-change) -> this replica received decision.
            #
            # This matches the common "Replica decided - Byzproposed" interpretation.
            full_decision_latency_ms = None
            if isinstance(ep, int) and decide_recv_t is not None:
                t_start = _epoch_first_propose_time(ep)
                if t_start is not None and decide_recv_t >= t_start:
                    full_decision_latency_ms = (decide_recv_t - t_start) * 1000.0

            # Extra debugging metric: winning propose (post-view-change) -> decision receive.
            winning_propose_latency_ms = None
            if isinstance(ep, int) and decide_recv_t is not None:
                t_win_prop = _epoch_winning_propose_time(ep)
                if t_win_prop is not None and decide_recv_t >= t_win_prop:
                    winning_propose_latency_ms = (decide_recv_t - t_win_prop) * 1000.0

            # Treat decision as "via gossip" only if the first time this replica logged
            # Order_Decided_Time is at/after its first GOSSIP-APPLY.
            decision_via_gossip = False
            if rep in replica_gossip_apply_time:
                gossip_t = replica_gossip_apply_time[rep]
                first_decided_t = (decided_all_by_replica.get(rep) or [None])[0]
                if first_decided_t is None or first_decided_t >= gossip_t:
                    decision_via_gossip = True

            vehicles_out.append({
                "vehicle_id": rep,
                # Partner field name; we map our ambulance role to priority.
                "is_priority_vehicle": (m.get("role") == "ambulance"),
                "coordination_method": _args.coordination_method,
                "transport": "wave",
                "cluster_mode": "allVehicles",
                "timestamps_ms": {
                    "stopped": stopped_ms,
                    "passed": passed_ms,
                },
                "durations_ms": {
                    # No direct equivalent in PBFT; keep 0 and report VC in bft_stats.
                    "leader_election_time_ms": 0.0,
                    "decision_latency_ms": decision_latency_ms,
                    "total_wait_time": total_wait_ms,
                },
                "messages": {
                    "sent": m.get("messages_sent"),
                    "received": m.get("messages_received"),
                    "bytes_sent": m.get("bytes_sent"),
                    "megabytes_sent": (
                        m.get("bytes_sent") / (1024.0 * 1024.0)
                        if m.get("bytes_sent") is not None else None
                    ),
                },
                "bft_stats": {
                    "tolerated_f": experiment_fault_tolerance["tolerated_f"],
                    "static_replicas": experiment_fault_tolerance["static_replicas"],
                    "consensus_quorum": experiment_fault_tolerance["consensus_quorum"],
                    "cert_threshold": experiment_fault_tolerance["cert_threshold"],
                    "cert_collection_duration_ms": (
                        _epoch_cert_collection_duration_ms(ep) if isinstance(ep, int) else None
                    ),
                    "cert_creation_latency_ms": (
                        m.get("cert_creation_s") * 1000.0
                        if m.get("cert_creation_s") is not None else None
                    ),
                    "view_change_triggered": rep in replica_vc_trigger_time,
                    # Global epoch-level VC duration (earliest trigger -> earliest primary installed).
                    "view_change_duration_ms": _epoch_view_change_duration_ms(ep) if isinstance(ep, int) else None,
                    "decision_via_gossip": decision_via_gossip,
                    "used_fallback": bool(m.get("used_fallback", False)),
                    "byzantine_injections_replica": byzantine_by_replica.get(rep, 0),
                    "byzantine_types": (
                        sorted(replica_byzantine_types.get(rep, set()))
                        if replica_byzantine_types.get(rep) else None
                    ),
                    "full_decision_latency_ms": full_decision_latency_ms,
                    "winning_propose_to_receive_ms": winning_propose_latency_ms,
                    "quiet_honest_vehicles": replica_quiet_honest_vehicles.get(rep),
                    "quiet_honest_opportunities": replica_quiet_honest_opportunities.get(rep),
                    "quiet_honest_rate_percent": replica_quiet_honest_rate.get(rep),
                    "attack_outcomes_replica": [
                        row for row in attack_outcomes if row["replica"] == rep
                    ] or None,
                    "attack_success_replica": any(
                        row["attack_success"] for row in attack_outcomes
                        if row["replica"] == rep
                    ),
                },
            })

        out = vehicles_out
    with open(path, "w", encoding="utf-8") as f:
        json.dump(out, f, indent=2, default=str)

def write_car_metrics_csv(path):
    with open(path, "w", encoding="utf-8") as f:
        f.write(
            "car_id,role,epoch,arrival_time,stop_time,resume_time,depart_time,"
            "wait_intersection,stop_to_resume,resume_to_depart,arrival_to_depart\n"
        )
        for car_id in sorted(car_metrics.keys(), key=lambda cid: int(cid[3:])):
            metrics = car_metrics[car_id]
            row = [
                car_id,
                metrics.get("role", "normal"),
                metrics.get("epoch", ""),
                metrics.get("arrival_time", ""),
                metrics.get("stop_time", ""),
                metrics.get("resume_time", ""),
                metrics.get("depart_time", ""),
                metrics.get("wait_intersection", ""),
                metrics.get("stop_to_resume", ""),
                metrics.get("resume_to_depart", ""),
                metrics.get("arrival_to_depart", ""),
            ]
            f.write(",".join(str(v) for v in row) + "\n")

def save_waittime_plot(path):
    if plt is None:
        return False
    grouped = {"normal": [], "ambulance": []}
    for metrics in car_metrics.values():
        wait_t = metrics.get("wait_intersection")
        role = metrics.get("role", "normal")
        if wait_t is not None:
            grouped.setdefault(role, []).append(wait_t)
    if not grouped["normal"] and not grouped["ambulance"]:
        return False

    fig, ax = plt.subplots(figsize=(8, 5))
    labels = []
    values = []
    colors = []
    for role in ("normal", "ambulance"):
        if grouped.get(role):
            labels.append(role.title())
            values.append(grouped[role])
            colors.append("#4C78A8" if role == "normal" else "#E45756")
    # Matplotlib API compatibility:
    # - newer versions accept tick_labels=
    # - older versions use labels=
    try:
        bp = ax.boxplot(values, tick_labels=labels, patch_artist=True, showfliers=True)
    except TypeError:
        bp = ax.boxplot(values, labels=labels, patch_artist=True, showfliers=True)
    for patch, color in zip(bp["boxes"], colors):
        patch.set_facecolor(color)
        patch.set_alpha(0.6)
    for idx, series in enumerate(values, start=1):
        ax.scatter([idx] * len(series), series, color=colors[idx - 1], alpha=0.85, zorder=3)
    ax.set_title("Wait Time At Intersection: Ambulance vs Normal Vehicles")
    ax.set_ylabel("Wait time (stop to leave) [s]")
    ax.grid(axis="y", linestyle="--", alpha=0.35)
    fig.tight_layout()
    fig.savefig(path, dpi=200)
    plt.close(fig)
    return True

def save_waittime_by_car_plot(path):
    if plt is None:
        return False
    rows = []
    for car_id, metrics in car_metrics.items():
        wait_t = metrics.get("wait_intersection")
        if wait_t is None:
            continue
        rows.append((int(car_id[3:]), car_id, wait_t, metrics.get("role", "normal")))
    if not rows:
        return False
    rows.sort()
    labels = [row[1] for row in rows]
    waits = [row[2] for row in rows]
    colors = ["#E45756" if row[3] == "ambulance" else "#4C78A8" for row in rows]
    fig, ax = plt.subplots(figsize=(max(8, len(rows) * 0.55), 4.8))
    ax.bar(labels, waits, color=colors)
    ax.set_title("Per-Car Wait Time At Intersection")
    ax.set_ylabel("Wait time (stop to leave) [s]")
    ax.set_xlabel("Vehicle")
    ax.grid(axis="y", linestyle="--", alpha=0.35)
    fig.tight_layout()
    fig.savefig(path, dpi=200)
    plt.close(fig)
    return True

# ── Print Report ─────────────────────────────────────────────────────────────
BOLD = "\033[1m"
RESET = "\033[0m"
RED   = "\033[31m"
GRN   = "\033[32m"
YEL   = "\033[33m"
CYN   = "\033[36m"

print("=" * 72)
print(f"{BOLD}BFT-SMaRt V2V Intersection — Epoch Latency Breakdown{RESET}")
print("=" * 72)

for epoch in range(N_EPOCHS):
    n  = EPOCH_N[epoch]
    rm = round_metrics.get(epoch, {})
    propose_all_sim = rm.get("ProposeAll_Consensus_Sim")
    first_submit_commit = rm.get("ProposeAll_FirstSubmit_To_OrderCommit_Sim")
    print(f"\n{BOLD}── Epoch {epoch}  (n={n} replicas) ──{RESET}")
    if propose_all_sim is not None and propose_all_sim > 0:
        print(f"  PROPOSE_ALL latency:     {CYN}{propose_all_sim:.4f}s{RESET}  (sim-time, leader submit→commit)")
        if first_submit_commit is not None and first_submit_commit > 0:
            print(f"  First submit→commit:    {YEL}{first_submit_commit:.4f}s{RESET}  (incl. failed primary / VC)")
    else:
        print(f"  PROPOSE_ALL latency:     {RED}N/A — epoch did not complete{RESET}")

print(f"\n{'=' * 72}")
print(f"{BOLD}Per-Car Lifecycle Metrics{RESET}")
print(f"{'─' * 72}")
print(f"  {'Car':>6}  {'Role':>10}  {'Wait@Int':>10}  {'Stop→Resume':>13}  {'Resume→Leave':>14}  {'Arr→Leave':>11}")
for car_id in sorted(car_metrics.keys(), key=lambda cid: int(cid[3:])):
    metrics = car_metrics[car_id]
    wait_t = metrics.get("wait_intersection")
    arr_res = metrics.get("stop_to_resume")
    res_dep = metrics.get("resume_to_depart")
    arr_dep = metrics.get("arrival_to_depart")
    role = metrics.get("role", "normal")
    print(
        f"  {car_id:>6}  {role:>10}  "
        f"{(f'{wait_t:.4f}s' if wait_t is not None else 'N/A'):>10}  "
        f"{(f'{arr_res:.4f}s' if arr_res is not None else 'N/A'):>13}  "
        f"{(f'{res_dep:.4f}s' if res_dep is not None else 'N/A'):>14}  "
        f"{(f'{arr_dep:.4f}s' if arr_dep is not None else 'N/A'):>11}"
    )

# Phase breakdown: cert_wait / bft_wait / queue_wait
_has_phase = any(car_metrics[c].get("cert_wait_s") is not None for c in car_metrics)
if _has_phase:
    print(f"\n{'─' * 72}")
    print(f"{BOLD}Per-Car Phase Breakdown  (stop → cert done → BFT → ORDER delivery → resume){RESET}")
    print(f"  {'Car':>6}  {'Role':>10}  {'CertWait':>9}  {'BFT':>6}  {'Deliver':>8}  {'Queue':>7}  {'Total':>7}")
    print(f"{'─' * 72}")
    for car_id in sorted(car_metrics.keys(), key=lambda cid: int(cid[3:])):
        cm = car_metrics[car_id]
        cw  = cm.get("cert_wait_s")
        bw  = cm.get("bft_wait_s")
        dw  = cm.get("order_delivery_s")
        qw  = cm.get("queue_wait_s")
        std = cm.get("stop_to_decision_sim_s")
        role = cm.get("role", "normal")
        role_tag = f" {CYN}←AMB{RESET}" if role == "ambulance" else ""
        def _f(v): return f"{v:.2f}s" if v is not None else "N/A"
        print(
            f"  {car_id:>6}  {role:>10}  "
            f"{_f(cw):>9}  {_f(bw):>6}  {_f(dw):>8}  {_f(qw):>7}  {_f(std):>7}"
            f"{role_tag}"
        )

normal_waits = [m["wait_intersection"] for m in car_metrics.values()
                if m.get("role", "normal") == "normal" and m.get("wait_intersection") is not None]
ambulance_waits = [m["wait_intersection"] for m in car_metrics.values()
                   if m.get("role") == "ambulance" and m.get("wait_intersection") is not None]
all_waits = [m["wait_intersection"] for m in car_metrics.values()
             if m.get("wait_intersection") is not None]
stop_to_resume_all = [m["stop_to_resume"] for m in car_metrics.values()
                      if m.get("stop_to_resume") is not None]
resume_to_depart_all = [m["resume_to_depart"] for m in car_metrics.values()
                        if m.get("resume_to_depart") is not None]

throughput = None
throughput_vps = None
depart_times = sorted(
    m["depart_time"] for m in car_metrics.values() if m.get("depart_time") is not None
)

# Throughput (user definition):
#   vehicles / mean(depart - stop) over all vehicles with both timestamps.
#   `all_waits` (computed above) is exactly the per-car (depart - stop) list.
if all_waits:
    n_w = len(all_waits)
    mean_wait_all = statistics.mean(all_waits)
    if mean_wait_all > 0:
        throughput_vps = n_w / mean_wait_all          # vehicles per second
        throughput     = mean_wait_all / n_w          # seconds per vehicle (1/vps)

missing_departure = [
    car_id for car_id, m in sorted(car_metrics.items(), key=lambda kv: int(kv[0][3:]))
    if m.get("stop_time") is not None and m.get("depart_time") is None
]

print(f"\n{'=' * 72}")
print(f"{BOLD}Paper-Friendly Summary Metrics{RESET}")
print(f"{'─' * 72}")
if experiment_fault_tolerance["tolerated_f"] is not None:
    print(
        "  Fault tolerance experiment:      "
        f"f={experiment_fault_tolerance['tolerated_f']} "
        f"static_n={experiment_fault_tolerance['static_replicas']} "
        f"quorum={experiment_fault_tolerance['consensus_quorum']} "
        f"cert={experiment_fault_tolerance['cert_threshold']}"
    )
if r0_metrics["enabled"]:
    stage = r0_metrics["failure_stage"] or "unknown"
    print(
        "  R0 late-emergency rollback:      "
        f"failure_stage={stage} "
        f"witnesses={r0_metrics['cancel_witness_count']} "
        f"echoes={r0_metrics['cancel_echo_count']} "
        f"certs={r0_metrics['cancel_cert_count']} "
        f"bad_quorum={r0_metrics['bad_quorum_count']} "
        f"cancel_commit={r0_metrics['cancel_commit_count']} "
        f"rollback_unavailable={r0_metrics['rollback_unavailable_count']} "
        f"max_M={r0_metrics['rollback_unavailable_max_m']} "
        f"need={r0_metrics['rollback_unavailable_need']} "
        f"rollback_commit={r0_metrics['rollback_commit_count']} "
        f"rollback_commit_extra={r0_metrics.get('rollback_commit_extra_replica_ids', [])} "
        f"epoch1_commit={int(r0_metrics['rollback_epoch1_committed'])} "
        f"epoch1_order_n={r0_metrics['epoch1_order_n_vehicles']} "
        f"epoch1_order_vehicles={r0_metrics['epoch1_order_vehicle_ids']} "
        f"pbft_new_req={int(r0_metrics['rollback_pbft_new_req_after_propose'])}"
    )
if throughput is not None:
    print(f"  Throughput (user definition): {throughput:.6f}s per vehicle")
    if throughput_vps is not None:
        print(f"  Throughput (user definition): {throughput_vps:.6f} vehicles/s")
    print(
        "    Formula: cars / mean(depart - stop) = "
        f"{len(all_waits)} / {statistics.mean(all_waits):.4f} = "
        f"{throughput_vps:.6f} veh/s"
    )
elif "Throughput_User_Definition" in run_metrics:
    throughput = run_metrics["Throughput_User_Definition"]
    throughput_vps = (1.0 / throughput) if throughput not in (None, 0) else None
    print(f"  Throughput (user definition): {throughput:.6f}s per vehicle  {CYN}[from [RUN-METRICS]]{RESET}")
    if throughput_vps is not None:
        print(f"  Throughput (standard):        {throughput_vps:.6f} vehicles/s  {CYN}[derived]{RESET}")
else:
    print(f"  Throughput (user definition): {YEL}N/A{RESET} "
          f"(need per-car depart_time metrics in the log)")
if missing_departure:
    print(f"  Missing leave timestamps:         {', '.join(missing_departure)}")
    print(f"    This log predates the new [CAR-METRICS] departure line or the native side was not rebuilt.")

def _fmt_or_run(values, run_key, label):
    if values:
        print(f"  {label}: {fmt_stat(values)}")
    elif run_key in run_metrics:
        print(f"  {label}: {run_metrics[run_key]:.4f}s  {CYN}[from [RUN-METRICS]]{RESET}")
    else:
        print(f"  {label}: N/A")

_fmt_or_run(all_waits,            "Wait_Intersection_All_Mean",      "Wait@intersection, all cars     ")
_fmt_or_run(normal_waits,         "Wait_Intersection_Normal_Mean",   "Wait@intersection, normal cars  ")
_fmt_or_run(ambulance_waits,      "Wait_Intersection_Ambulance_Mean","Wait@intersection, ambulance    ")
_fmt_or_run(stop_to_resume_all,   "Stop_To_Resume_Mean",             "Stop→resume (stop to GO signal) ")
_fmt_or_run(resume_to_depart_all, "Resume_To_Depart_Mean",           "Resume→leave, all cars          ")

fairness_all = jains_fairness(all_waits)
fairness_normal = jains_fairness(normal_waits)
if fairness_all is not None:
    print(f"  Jain fairness index (all waits):  {fairness_all:.4f}")
elif "Jains_Fairness_All" in run_metrics:
    print(f"  Jain fairness index (all waits):  {run_metrics['Jains_Fairness_All']:.4f}  {CYN}[from [RUN-METRICS]]{RESET}")
if fairness_normal is not None:
    print(f"  Jain fairness index (normal):     {fairness_normal:.4f}")
elif "Jains_Fairness_Normal" in run_metrics:
    print(f"  Jain fairness index (normal):     {run_metrics['Jains_Fairness_Normal']:.4f}  {CYN}[from [RUN-METRICS]]{RESET}")
if normal_waits and ambulance_waits:
    amb_mean = statistics.mean(ambulance_waits)
    norm_mean = statistics.mean(normal_waits)
    ratio = (amb_mean / norm_mean) if norm_mean else float('inf')
    delta = norm_mean - amb_mean
    print(f"  Ambulance priority gain:          "
          f"ambulance_mean={amb_mean:.4f}s vs normal_mean={norm_mean:.4f}s "
          f"(delta={delta:+.4f}s, ratio={ratio:.4f})")
elif "Ambulance_Priority_Gain" in run_metrics:
    gain  = run_metrics["Ambulance_Priority_Gain"]
    ratio = run_metrics.get("Ambulance_Priority_Ratio", float('nan'))
    print(f"  Ambulance priority gain:          {gain:.4f}s (ratio={ratio:.4f})  {CYN}[from [RUN-METRICS]]{RESET}")

bytes_sent_total = sum(replica_bytes_sent.values()) if replica_bytes_sent else None
quiet_honest_count = sum(replica_quiet_honest_vehicles.values())
quiet_honest_opportunities = sum(replica_quiet_honest_opportunities.values())
quiet_honest_rate = (
    100.0 * quiet_honest_count / quiet_honest_opportunities
    if quiet_honest_opportunities > 0 else None
)
if bytes_sent_total is not None:
    print(f"  Payload bytes sent:               {bytes_sent_total} bytes ({bytes_sent_total / (1024.0 * 1024.0):.6f} MB)")
elif "Bytes_Sent_Total" in run_metrics:
    bytes_sent_total = int(run_metrics["Bytes_Sent_Total"])
    print(f"  Payload bytes sent:               {bytes_sent_total} bytes ({bytes_sent_total / (1024.0 * 1024.0):.6f} MB)  {CYN}[from [RUN-METRICS]]{RESET}")
if quiet_honest_rate is not None:
    print(f"  Honest vehicles marked QUIET:     {quiet_honest_count}/{quiet_honest_opportunities} ({quiet_honest_rate:.6f}%)")
elif "Quiet_Honest_Rate" in run_metrics:
    print(f"  Honest vehicles marked QUIET:     {run_metrics['Quiet_Honest_Rate']:.6f}%  {CYN}[from [RUN-METRICS]]{RESET}")

attack_failures_total = sum(attack_failures_by_epoch.values())
if attack_outcomes or "Attack_Failures_Total" in run_metrics:
    if not attack_outcomes:
        attack_failures_total = int(run_metrics.get("Attack_Failures_Total", 0))
    attack_col = RED if attack_failures_total > 0 else GRN
    print(f"  Consensus attack success:         {attack_col}{1 if attack_failures_total > 0 else 0}{RESET} "
          f"(unique failures={attack_failures_total}, raw outcome lines={len(attack_outcomes)})")
    for ep in sorted(attack_outcomes_by_epoch):
        for row in attack_outcomes_by_epoch[ep]:
            marker = "SUCCESS" if row["attack_success"] else "BLOCKED"
            print(f"    epoch={row['epoch']} fault={row['fault']} outcome={row['outcome']} {marker}")

# ── Per-epoch breakdown: wait + throughput ────────────────────────────────────
print(f"\n{'=' * 72}")
print(f"{BOLD}Per-Epoch Intersection Performance{RESET}")
print(f"{'─' * 72}")
print(f"  {'Epoch':>5}  {'N':>4}  {'Throughput':>12}  {'WaitNorm(mean)':>16}  {'WaitAmb(mean)':>15}  {'Byzantine':>10}  {'Attack':>8}")
for ep in range(N_EPOCHS):
    n = EPOCH_N[ep]
    rm_ep = round_metrics.get(ep, {})
    tp = epoch_throughput.get(ep) or rm_ep.get("Throughput_User_Definition")
    tp_str = f"{tp:.4f}s/veh" if tp is not None else "N/A"
    wn = epoch_wait_normal.get(ep, [])
    wa = epoch_wait_ambulance.get(ep, [])
    wn_val = statistics.mean(wn) if wn else rm_ep.get("Wait_Normal_Mean")
    wa_val = statistics.mean(wa) if wa else rm_ep.get("Wait_Ambulance_Mean")
    wn_str = f"{wn_val:.4f}s" if wn_val is not None else "N/A"
    wa_str = f"{wa_val:.4f}s" if wa_val is not None else "N/A"
    byz = byzantine_by_epoch.get(ep, 0) or int(run_metrics.get(f"Byzantine_Injections_Epoch{ep}", 0))
    byz_col = RED if byz > 0 else GRN
    atk = attack_failures_by_epoch.get(ep, 0) or int(run_metrics.get(f"Attack_Failures_Epoch{ep}", 0))
    atk_col = RED if atk > 0 else GRN
    print(f"  {ep:>5}  {n:>4}  {tp_str:>12}  {wn_str:>16}  {wa_str:>15}  {byz_col}{byz:>10}{RESET}  {atk_col}{atk:>8}{RESET}")

# ── Byzantine Fault Analysis ─────────────────────────────────────────────────
byz_total_display = byzantine_total or int(run_metrics.get("Byzantine_Injections_Total", 0))
if byz_total_display > 0:
    print(f"\n{'=' * 72}")
    print(f"{BOLD}Byzantine Fault Analysis{RESET}")
    print(f"{'─' * 72}")
    print(f"  Total Byzantine hash corruptions: {RED}{byz_total_display}{RESET}")
    byz_by_ep = {ep: (byzantine_by_epoch.get(ep, 0) or
                      int(run_metrics.get(f"Byzantine_Injections_Epoch{ep}", 0)))
                 for ep in range(N_EPOCHS)}
    if any(byz_by_ep.values()):
        print(f"  {'Epoch':>5}  {'Injections':>12}")
        for ep in range(N_EPOCHS):
            if byz_by_ep[ep]:
                print(f"  {ep:>5}  {byz_by_ep[ep]:>12}")
    if byzantine_by_replica:
        print(f"  {'Replica':>8}  {'Injections':>12}")
        for rep in sorted(byzantine_by_replica):
            print(f"  {rep:>8}  {byzantine_by_replica[rep]:>12}")
    consensus_ok = all(
        round_metrics.get(ep, {}).get("ProposeAll_Consensus_Sim")
        for ep in range(N_EPOCHS))
    if consensus_ok:
        print(f"  {GRN}Consensus correctness: all epochs completed → BFT safety upheld{RESET}")
    else:
        print(f"  {RED}WARNING: some epochs did not complete — check above for details{RESET}")

if not SAVE_TO:
    csv_path  = os.path.join(PLOTS_DIR, "car_metrics.csv")
    json_path = os.path.join(PLOTS_DIR, "metrics.json")
    write_car_metrics_csv(csv_path)
    write_metrics_json(json_path)
    print(f"  Saved per-car CSV:                {csv_path}")
    print(f"  Saved full metrics JSON:          {json_path}")

if not SAVE_TO:
    wait_plot_path = os.path.join(PLOTS_DIR, "waittime_ambulance_vs_normal.png")
    per_car_plot_path = os.path.join(PLOTS_DIR, "waittime_per_car.png")
    wait_plot_saved = save_waittime_plot(wait_plot_path)
    per_car_plot_saved = save_waittime_by_car_plot(per_car_plot_path)
    if wait_plot_saved:
        print(f"  Saved wait-role plot:             {wait_plot_path}")
    elif plt is None:
        print(f"  Saved wait-role plot:             matplotlib not available")
    else:
        print(f"  Saved wait-role plot:             insufficient wait-time data")
    if per_car_plot_saved:
        print(f"  Saved per-car wait plot:          {per_car_plot_path}")

# ── Save log to collection dir ────────────────────────────────────────────────
if SAVE_TO:
    os.makedirs(SAVE_TO, exist_ok=True)
    existing = [f for f in os.listdir(SAVE_TO) if re.match(rf'{CARS}veh_\d+\.log$', f)]
    idx  = len(existing)
    dest = os.path.join(SAVE_TO, f"{CARS}veh_{idx}.log")

    def _first_present(ep, *keys):
        rm = round_metrics.get(ep, {})
        for key in keys:
            if key in rm:
                return rm[key]
        return None

    # Write a canonical metrics summary with one line per metric.
    with open(dest, 'w') as f:
        for ep in range(N_EPOCHS):
            # Per-car durations (one line per car) so downstream scripts can build CDFs.
            # These are lifecycle timings, not consensus latency.
            per_car = epoch_total_dur_by_car.get(ep, {})
            if per_car:
                for car_id, dur in sorted(per_car.items(), key=lambda kv: int(kv[0][3:])):
                    f.write(f"[ROUND-METRICS] Epoch {ep} Arrival_To_Resume_Time {car_id}: "
                            f"{dur:.6f} seconds\n")

            if epoch_messages_sent[ep] and epoch_messages_recv[ep]:
                avg_sent = sum(epoch_messages_sent[ep]) / len(epoch_messages_sent[ep])
                avg_recv = sum(epoch_messages_recv[ep]) / len(epoch_messages_recv[ep])
                f.write(f"[ROUND-METRICS] Epoch {ep} Avg_Messages_Sent_PerReplica: "
                        f"{avg_sent:.3f} (count={len(epoch_messages_sent[ep])})\n")
                f.write(f"[ROUND-METRICS] Epoch {ep} Avg_Messages_Received_PerReplica: "
                        f"{avg_recv:.3f} (count={len(epoch_messages_recv[ep])})\n")
            else:
                avg_sent = _first_present(ep, "Avg_Messages_Sent_PerReplica", "Messages_Sent_PerReplica")
                avg_recv = _first_present(ep, "Avg_Messages_Received_PerReplica", "Messages_Received_PerReplica")
                if avg_sent is not None:
                    f.write(f"[ROUND-METRICS] Epoch {ep} Avg_Messages_Sent_PerReplica: {avg_sent:.3f}\n")
                if avg_recv is not None:
                    f.write(f"[ROUND-METRICS] Epoch {ep} Avg_Messages_Received_PerReplica: {avg_recv:.3f}\n")
            if epoch_bytes_sent[ep]:
                total_bytes_ep = sum(epoch_bytes_sent[ep])
                avg_bytes_ep = total_bytes_ep / len(epoch_bytes_sent[ep])
                f.write(f"[ROUND-METRICS] Epoch {ep} Avg_Bytes_Sent_PerReplica: "
                        f"{avg_bytes_ep:.3f} (count={len(epoch_bytes_sent[ep])})\n")
                f.write(f"[ROUND-METRICS] Epoch {ep} Total_Bytes_Sent: {total_bytes_ep}\n")
            if epoch_total_dur[ep]:
                avg_dur = sum(epoch_total_dur[ep]) / len(epoch_total_dur[ep])
                f.write(f"[ROUND-METRICS] Epoch {ep} Avg_Arrival_To_Resume_Time: "
                        f"{avg_dur:.6f} seconds (goCars={len(epoch_total_dur[ep])})\n")
            fail_val = epoch_failures.get(ep)
            if fail_val is None:
                fail_val = _first_present(ep, "Avg_StopSign_Failures", "StopSign_Failures")
            if fail_val is None:
                fail_val = 0
            f.write(f"[ROUND-METRICS] Epoch {ep} Avg_StopSign_Failures: "
                    f"{float(fail_val):.3f} (count)\n")

        cert_creation_all = [
            value
            for values in epoch_cert_creation_latency.values()
            for value in values
            if value > 0
        ]
        if cert_creation_all:
            f.write("[RUN-METRICS] Cert_Creation_Latency_Mean: "
                    f"{statistics.mean(cert_creation_all):.6f} seconds\n")
            f.write("[RUN-METRICS] Cert_Creation_Latency_P95: "
                    f"{percentile(cert_creation_all, 0.95):.6f} seconds\n")
        if throughput is not None:
            f.write(f"[RUN-METRICS] Throughput_User_Definition: {throughput:.6f} seconds_per_vehicle\n")
        if throughput_vps is not None:
            f.write(f"[RUN-METRICS] Throughput_Vehicles_Per_Second: {throughput_vps:.6f} vehicles_per_second\n")
        if all_waits:
            f.write(f"[RUN-METRICS] Wait_Intersection_All_Mean: {statistics.mean(all_waits):.6f} seconds\n")
            f.write(f"[RUN-METRICS] Wait_Intersection_All_P95: {percentile(all_waits, 0.95):.6f} seconds\n")
        if normal_waits:
            f.write(f"[RUN-METRICS] Wait_Intersection_Normal_Mean: {statistics.mean(normal_waits):.6f} seconds\n")
        if ambulance_waits:
            f.write(f"[RUN-METRICS] Wait_Intersection_Ambulance_Mean: {statistics.mean(ambulance_waits):.6f} seconds\n")
        if stop_to_resume_all:
            f.write(f"[RUN-METRICS] Stop_To_Resume_Mean: {statistics.mean(stop_to_resume_all):.6f} seconds\n")
        if resume_to_depart_all:
            f.write(f"[RUN-METRICS] Resume_To_Depart_Mean: {statistics.mean(resume_to_depart_all):.6f} seconds\n")

        if experiment_fault_tolerance["tolerated_f"] is not None:
            f.write(f"[RUN-METRICS] Tolerated_F: {experiment_fault_tolerance['tolerated_f']}\n")
            f.write(f"[RUN-METRICS] Static_Replicas: {experiment_fault_tolerance['static_replicas']}\n")
            f.write(f"[RUN-METRICS] Consensus_Quorum: {experiment_fault_tolerance['consensus_quorum']}\n")
            f.write(f"[RUN-METRICS] Cert_Threshold: {experiment_fault_tolerance['cert_threshold']}\n")
        if r0_metrics["enabled"]:
            f.write(f"[RUN-METRICS] R0_Enabled: 1\n")
            f.write(f"[RUN-METRICS] R0_Cancel_Witness_Count: {r0_metrics['cancel_witness_count']}\n")
            f.write(f"[RUN-METRICS] R0_Cancel_Echo_Count: {r0_metrics['cancel_echo_count']}\n")
            f.write(f"[RUN-METRICS] R0_Cancel_Cert_Count: {r0_metrics['cancel_cert_count']}\n")
            f.write(f"[RUN-METRICS] R0_Cancel_Commit_Count: {r0_metrics['cancel_commit_count']}\n")
            f.write(f"[RUN-METRICS] R0_Bad_Quorum_Count: {r0_metrics['bad_quorum_count']}\n")
            f.write(f"[RUN-METRICS] R0_Rollback_Unavailable_Count: {r0_metrics['rollback_unavailable_count']}\n")
            f.write(f"[RUN-METRICS] R0_Rollback_Unavailable_Max_M: {r0_metrics['rollback_unavailable_max_m']}\n")
            if r0_metrics["rollback_unavailable_f"] is not None:
                f.write(f"[RUN-METRICS] R0_Rollback_Unavailable_F: {r0_metrics['rollback_unavailable_f']}\n")
            if r0_metrics["rollback_unavailable_need"] is not None:
                f.write(f"[RUN-METRICS] R0_Rollback_Unavailable_Need: {r0_metrics['rollback_unavailable_need']}\n")
            f.write(f"[RUN-METRICS] R0_Rollback_Commit_Count: {r0_metrics['rollback_commit_count']}\n")
            if r0_metrics["rollback_commit_replica_ids"]:
                committers = ",".join(
                    str(v) for v in sorted(set(r0_metrics["rollback_commit_replica_ids"]))
                )
                f.write(f"[RUN-METRICS] R0_Rollback_Commit_Replicas: {committers}\n")
            extra_committers = r0_metrics.get("rollback_commit_extra_replica_ids", [])
            if extra_committers:
                extra = ",".join(str(v) for v in extra_committers)
                f.write(f"[RUN-METRICS] R0_Rollback_Commit_Extra_Replicas: {extra}\n")
            missing_committers = r0_metrics.get("rollback_commit_missing_replica_ids", [])
            if missing_committers:
                missing = ",".join(str(v) for v in missing_committers)
                f.write(f"[RUN-METRICS] R0_Rollback_Commit_Missing_Replicas: {missing}\n")
            f.write(f"[RUN-METRICS] R0_Epoch1_OrderDecision_Count: {r0_metrics['epoch1_order_decision_count']}\n")
            if r0_metrics["epoch1_order_n_vehicles"] is not None:
                f.write(f"[RUN-METRICS] R0_Epoch1_Order_N_Vehicles: {r0_metrics['epoch1_order_n_vehicles']}\n")
            if r0_metrics["epoch1_order_n_batches"] is not None:
                f.write(f"[RUN-METRICS] R0_Epoch1_Order_N_Batches: {r0_metrics['epoch1_order_n_batches']}\n")
            if r0_metrics["epoch1_order_vehicle_ids"]:
                vehicles = ",".join(str(v) for v in r0_metrics["epoch1_order_vehicle_ids"])
                f.write(f"[RUN-METRICS] R0_Epoch1_Order_Vehicles: {vehicles}\n")
            f.write(f"[RUN-METRICS] R0_Failure_Stage: {r0_metrics['failure_stage']}\n")

        # Jain's fairness
        if jains_fairness(all_waits) is not None:
            f.write(f"[RUN-METRICS] Jains_Fairness_All: {jains_fairness(all_waits):.6f}\n")
        if jains_fairness(normal_waits) is not None:
            f.write(f"[RUN-METRICS] Jains_Fairness_Normal: {jains_fairness(normal_waits):.6f}\n")
        if normal_waits and ambulance_waits:
            amb_mean  = statistics.mean(ambulance_waits)
            norm_mean = statistics.mean(normal_waits)
            f.write(f"[RUN-METRICS] Ambulance_Priority_Gain: {norm_mean - amb_mean:.6f} seconds\n")
            f.write(f"[RUN-METRICS] Ambulance_Priority_Ratio: {(amb_mean / norm_mean):.6f}\n")

        # Byzantine fault injection summary
        f.write(f"[RUN-METRICS] Byzantine_Injections_Total: {byzantine_total}\n")
        for ep in range(N_EPOCHS):
            byz = byzantine_by_epoch.get(ep, 0)
            f.write(f"[RUN-METRICS] Byzantine_Injections_Epoch{ep}: {byz}\n")

        attack_failures_total = sum(attack_failures_by_epoch.values())
        f.write(f"[RUN-METRICS] Attack_Success: {1 if attack_failures_total > 0 else 0}\n")
        f.write(f"[RUN-METRICS] Attack_Failures_Total: {attack_failures_total}\n")
        f.write(f"[RUN-METRICS] Attack_Outcome_Lines_Total: {len(attack_outcomes)}\n")
        for ep in range(N_EPOCHS):
            f.write(f"[RUN-METRICS] Attack_Failures_Epoch{ep}: {attack_failures_by_epoch.get(ep, 0)}\n")
        for ep in sorted(attack_outcomes_by_epoch):
            for row in attack_outcomes_by_epoch[ep]:
                detail = f" {row['detail']}" if row.get("detail") else ""
                f.write(
                    f"[ATTACK-OUTCOME] epoch={row['epoch']} fault={row['fault']} "
                    f"outcome={row['outcome']} success={1 if row['attack_success'] else 0}"
                    f"{detail}\n"
                )

        # Delivery ratio and fallback rate (comparable to RAFT counterparts)
        _all_dr = [m["delivery_ratio"] for m in car_metrics.values()
                   if m.get("delivery_ratio") is not None]
        if _all_dr:
            _run_dr = statistics.mean(_all_dr)
            f.write(f"[RUN-METRICS] Delivery_Ratio: {_run_dr:.6f}\n")
            f.write(f"[RUN-METRICS] Estimated_Loss_Rate: {1.0 - _run_dr:.6f}\n")
        _fallback_n = sum(1 for m in car_metrics.values() if m.get("used_fallback", False))
        f.write(f"[RUN-METRICS] Fallback_Count: {_fallback_n}\n")
        if CARS > 0:
            f.write(f"[RUN-METRICS] Fallback_Rate: {_fallback_n / CARS:.6f}\n")

        if bytes_sent_total is not None:
            f.write(f"[RUN-METRICS] Bytes_Sent_Total: {bytes_sent_total}\n")
            f.write(f"[RUN-METRICS] Megabytes_Sent_Total: {bytes_sent_total / (1024.0 * 1024.0):.6f}\n")
        f.write(f"[RUN-METRICS] Quiet_Honest_Count: {quiet_honest_count}\n")
        f.write(f"[RUN-METRICS] Quiet_Honest_Opportunities: {quiet_honest_opportunities}\n")
        if quiet_honest_rate is not None:
            f.write(f"[RUN-METRICS] Quiet_Honest_Rate: {quiet_honest_rate:.6f}\n")

        # Per-epoch throughput and wait
        for ep in range(N_EPOCHS):
            tp = epoch_throughput.get(ep)
            if tp is not None:
                f.write(f"[ROUND-METRICS] Epoch {ep} Throughput_User_Definition: {tp:.6f} seconds_per_vehicle\n")
                if tp != 0:
                    f.write(f"[ROUND-METRICS] Epoch {ep} Throughput_Vehicles_Per_Second: {(1.0 / tp):.6f} vehicles_per_second\n")
            wn = epoch_wait_normal.get(ep, [])
            wa = epoch_wait_ambulance.get(ep, [])
            if wn:
                f.write(f"[ROUND-METRICS] Epoch {ep} Wait_Normal_Mean: {statistics.mean(wn):.6f} seconds\n")
            if wa:
                f.write(f"[ROUND-METRICS] Epoch {ep} Wait_Ambulance_Mean: {statistics.mean(wa):.6f} seconds\n")

        # Also copy the JSON alongside the .log
        json_dest = os.path.join(SAVE_TO, f"{CARS}veh_{idx}.json")
        write_metrics_json(json_dest)

    saved_size = os.path.getsize(dest)
    print(f"Log saved → {dest}  (run #{idx}, {saved_size} bytes — round/run metrics summary)")
    print(f"JSON saved → {os.path.join(SAVE_TO, f'{CARS}veh_{idx}.json')}")
    if _args.scenario is not None:
        print(
            f"Scenario {_args.scenario} ({SCENARIO_LABEL[_args.scenario]}): "
            f"pool these files in plot_wait_time_cdf (glob {CARS}veh_*.json under this folder)."
        )

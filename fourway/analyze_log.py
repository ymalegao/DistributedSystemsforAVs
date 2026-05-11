#!/usr/bin/env python3
"""
BFT-SMaRt V2V Intersection Log Analyzer.

Tracks the single-round PROPOSE_ALL protocol and per-car lifecycle metrics, including:
  - PROPOSE_ALL consensus latency (Java BFT layer)
  - wait time at the intersection (stop -> leave)
  - arrival -> resume and arrival -> leave timing
  - throughput using the user-defined formula
  - ambulance vs normal vehicle comparisons

Batch / plotting workflow (same base --save-to, e.g. benchmarks/Priority4cars):
  python analyze_log.py combined.log --save-to benchmarks/Priority4cars --scenario 1 --cars 4
  # writes benchmarks/Priority4cars/no_amb/4veh_0.json (then 4veh_1.json, ...)
  # --scenario: 1=no ambulance, 2=honest ambulance, 3=Byz followers (legacy),
  #             4=Byz leader with ambulance, 5=Byz leader no ambulance,
  #             6=Byz followers with no ambulance
  # plot_wait_time_cdf.py pools all matching Nveh_*.json per folder.
"""

import sys
import re
import os
import argparse
import statistics
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
    "--scenario", type=int, choices=[1, 2, 3, 4, 5, 6], default=None,
    metavar="N",
    help="Save under a scenario subfolder (requires --save-to): "
         "1=no ambulance, 2=honest ambulance, 3=ambulance+Byzantine followers, "
         "4=ambulance+Byzantine leader, 5=no ambulance+Byzantine leader, "
         "6=no ambulance+Byzantine followers. Use same base DIR and --cars for each run; "
         "plot_wait_time_cdf.py pools all matching <N>veh_*.json in that folder.")
_parser.add_argument("--cars", type=int, default=None,
                     help="Total cars/replicas in the scenario (e.g. 12 or 16). "
                          "If omitted, inferred from --save-to path or defaults to 16.")
_parser.add_argument("--plots-dir", metavar="DIR",
                     help="Directory for generated plots/CSV. Defaults to the log file directory.")
_parser.add_argument(
    "--no-scenario-subdir",
    action="store_true",
    help="Write metrics directly under --save-to (do not append no_amb/amb_honest/...). "
         "Use when --save-to already includes the scenario folder (e.g. .../amb_honest/run_0).",
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
}
SCENARIO_LABEL = {
    1: "no ambulance",
    2: "honest ambulance",
    3: "ambulance + Byzantine followers",
    4: "ambulance + Byzantine leader",
    5: "no ambulance + Byzantine leader",
    6: "no ambulance + Byzantine followers",
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
RE_RESUME = re.compile(r'\[RESUME\] Replica (\d+): JNI received GO signal')
RE_RESUME_MSG = re.compile(r'\[HANDLE-SELF-MSG\] Replica (\d+): .*msgName=resumeVehicle')

RE_MESSAGES_SENT     = re.compile(r'\[METRICS (\d+)\] Messages_Sent: (\d+)')
RE_MESSAGES_RECEIVED = re.compile(r'\[METRICS (\d+)\] Messages_Received: (\d+)')
RE_ARRIVAL_TIME      = re.compile(r'\[METRICS (\d+)\] Arrival_Time: ([\d.]+)')
RE_STOP_TIME         = re.compile(r'\[METRICS (\d+)\] Stop_Time: ([\d.]+)')
RE_RESUME_TIME       = re.compile(r'\[METRICS (\d+)\] Resume_Time: ([\d.]+)')
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
    r'\[PHASE_SUMMARY (\d+)\] .*PROPOSE_ALL_BFT\(sim\)=(?:([\d.]+)s|N/A) .*stop_to_decision\(sim\)=(?:([\d.]+)s|N/A)'
)
RE_CONSENSUS_HEADER = re.compile(r'CONSENSUS METRICS \(Replica (\d+)\) epoch=(\d+)')
RE_CERT_DUR = re.compile(r'\[METRICS (\d+)\] Cert_Collection_Duration: ([\d.]+)s')
# ResDB batch assignment (from IntersectionExecutor batch packer)
RE_BATCH_ASSIGN = re.compile(r'\[METRICS (\d+)\] Batch_Assignment: batch=(\d+)')

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
resume_signal_received = set() # set of replica IDs
resume_msg_handled = set()     # set of replica IDs

# New per-epoch data stores
replica_arrival_time = {}                   # replica -> first arrival time in intersection zone
replica_stop_time   = {}                    # replica -> stop_time (float)
replica_resume_time = {}                    # replica -> resume_time (float)
epoch_messages_sent = defaultdict(list)     # epoch -> [sent_count, ...]
epoch_messages_recv = defaultdict(list)     # epoch -> [recv_count, ...]
epoch_total_dur     = defaultdict(list)     # epoch -> [duration, ...]
epoch_total_dur_by_car = defaultdict(dict)  # epoch -> {carId: duration}
epoch_failures      = defaultdict(int)      # epoch -> count
ambulance_ids       = set()
car_metrics         = defaultdict(dict)     # carId -> {metric_name: value}
replica_batch_index = {}                    # replica_id (int) -> batch_index (int)

# Byzantine injection tracking
byzantine_by_epoch   = defaultdict(int)    # epoch -> count of [BYZANTINE INJECTION] events
byzantine_by_replica = defaultdict(int)    # replica -> count
byzantine_total      = 0                   # across the whole run

# Per-replica message counts and fallback tracking
replica_messages_sent    = {}   # replica -> messages_sent count (last value seen)
replica_messages_recv    = {}   # replica -> messages_received count (last value seen)
replica_stopsign_timeout = set()  # replica IDs that hit StopSign_Timeout (used fallback)

# Pre-computed summary metrics read back from saved logs
run_metrics           = {}                 # key -> float  (from [RUN-METRICS] lines)
propose_all_sim_raw   = defaultdict(list)  # epoch -> [seconds, ...]
stop_to_decision_sim_raw = defaultdict(list) # epoch -> [seconds, ...]

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

# ── Parse ────────────────────────────────────────────────────────────────────
print(f"Parsing {LOG_FILE} ...")
current_gossip_epoch = 0              # rough tracker for gossip events

with open(LOG_FILE, "r", errors="replace") as f:
    for line in f:
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

        m = RE_RESUME.search(line)
        if m:
            resume_signal_received.add(int(m.group(1)))
            continue

        m = RE_RESUME_MSG.search(line)
        if m:
            resume_msg_handled.add(int(m.group(1)))
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
            ep = replica_epoch.get(rep, current_gossip_epoch)
            car_id = f"veh{rep}"
            if m.group(2):
                val = float(m.group(2))
                propose_all_sim_raw[ep].append(val)
                car_metrics[car_id]["propose_all_consensus_sim_s"] = val
            if m.group(3):
                val = float(m.group(3))
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

        m = RE_BATCH_ASSIGN.search(line)
        if m:
            rep, batch = int(m.group(1)), int(m.group(2))
            replica_batch_index[rep] = batch
            car_metrics[f"veh{rep}"]["batch_index"] = batch
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

print("Done.\n")
byzantine_total = sum(byzantine_by_epoch.values())

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

for ep, samples in propose_all_sim_raw.items():
    if samples:
        round_metrics[ep]["ProposeAll_Consensus_Sim"] = statistics.mean(samples)

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
        propose_all_sim = rm.get("ProposeAll_Consensus_Sim")
        epochs_out.append({
            "epoch": ep,
            "n_replicas": n,
            "propose_all_consensus_latency_sim_s": propose_all_sim,
            "stop_to_decision_sim_s": rm.get("Stop_To_Decision_Sim"),
            "throughput_s_per_veh": tp,
            "throughput_veh_per_s": (1.0 / tp) if tp not in (None, 0) else None,
            "wait_normal_s": _stat(wn),
            "wait_ambulance_s": _stat(wa),
            "byzantine_injections": byz,
            "consensus_success": bool(propose_all_sim),
            "avg_messages_sent_per_replica": (sum(msgs_sent) / len(msgs_sent)) if msgs_sent else None,
            "avg_messages_recv_per_replica": (sum(msgs_recv) / len(msgs_recv)) if msgs_recv else None,
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
            # delivery_ratio = received / (sent * (N-1)); heuristic for message loss
            "delivery_ratio": m.get("delivery_ratio"),
            "estimated_loss_rate": m.get("estimated_loss_rate"),
            # Fallback: True if this vehicle hit a StopSign_Timeout
            "used_fallback": m.get("used_fallback", False),
            "propose_all_consensus_sim_s": m.get("propose_all_consensus_sim_s"),
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

    overall = {
        "cars": CARS,
        "epochs": N_EPOCHS,
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
            (lambda stops, departs: CARS / (max(departs) - min(stops))
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
        "stop_to_decision_sim_s": _stat([
            round_metrics[ep]["Stop_To_Decision_Sim"]
            for ep in range(N_EPOCHS)
            if round_metrics.get(ep, {}).get("Stop_To_Decision_Sim", 0) > 0
        ]),
        "total_byzantine_injections": byzantine_total,
        "byzantine_by_epoch": dict(byzantine_by_epoch),
        "byzantine_by_replica": dict(byzantine_by_replica),
        "stop_to_resume_s": _stat(stop_to_resume_all),
        "resume_to_depart_s": _stat(resume_to_depart_all),
        # Delivery and fallback metrics (same definitions as RAFT counterpart)
        # delivery_ratio = received / (sent * (N-1)); heuristic for message loss
        "delivery_ratio": _run_dr,
        "estimated_loss_rate": _run_lr,
        # fallback_rate = fraction of vehicles that hit StopSign_Timeout this run
        "fallback_rate": _run_fallback_rate,
        "fallback_count": _fallback_n,
    }

    out = {"overall": overall, "per_epoch": epochs_out, "per_car": cars_out}
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
    print(f"\n{BOLD}── Epoch {epoch}  (n={n} replicas) ──{RESET}")
    if propose_all_sim is not None and propose_all_sim > 0:
        print(f"  PROPOSE_ALL latency:     {CYN}{propose_all_sim:.4f}s{RESET}  (sim-time)")
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

# ── Per-epoch breakdown: wait + throughput ────────────────────────────────────
print(f"\n{'=' * 72}")
print(f"{BOLD}Per-Epoch Intersection Performance{RESET}")
print(f"{'─' * 72}")
print(f"  {'Epoch':>5}  {'N':>4}  {'Throughput':>12}  {'WaitNorm(mean)':>16}  {'WaitAmb(mean)':>15}  {'Byzantine':>10}")
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
    print(f"  {ep:>5}  {n:>4}  {tp_str:>12}  {wn_str:>16}  {wa_str:>15}  {byz_col}{byz:>10}{RESET}")

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

csv_path  = os.path.join(PLOTS_DIR, "car_metrics.csv")
json_path = os.path.join(PLOTS_DIR, "metrics.json")
write_car_metrics_csv(csv_path)
write_metrics_json(json_path)
print(f"  Saved per-car CSV:                {csv_path}")
print(f"  Saved full metrics JSON:          {json_path}")

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

print(f"\n{'=' * 72}")
print(f"{BOLD}Decision / Resume Coverage{RESET}")
print(f"{'─' * 72}")
print(f"  {'Car':>6}  {'Decision Epoch':>14}  {'JNI Resume':>12}  {'Main Thread Resume':>18}")
all_cars = [f"veh{i}" for i in range(CARS)]
for car in all_cars:
    rep_id = int(car[3:])
    epoch = go_decision_epoch.get(car, "NONE")
    ep_str = f"{epoch}" if isinstance(epoch, int) else f"{RED}NONE{RESET}"
    
    jni_res = f"{GRN}YES{RESET}" if rep_id in resume_signal_received else f"{RED}NO{RESET}"
    msg_han = f"{GRN}YES{RESET}" if rep_id in resume_msg_handled else f"{RED}NO{RESET}"
    
    print(f"  {car:>6}  {ep_str:>14}  {jni_res:>12}  {msg_han:>18}")

print()

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

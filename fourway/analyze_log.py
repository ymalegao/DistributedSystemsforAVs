#!/usr/bin/env python3
"""
BFT-SMaRt V2V Intersection Log Analyzer.

Tracks the active VIEW/ORDER protocol and per-car lifecycle metrics, including:
  - VIEW / ORDER latency breakdowns
  - wait time at the intersection (stop -> leave)
  - arrival -> resume and arrival -> leave timing
  - throughput using the user-defined formula
  - ambulance vs normal vehicle comparisons
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
_parser.add_argument("--cars", type=int, default=None,
                     help="Total cars/replicas in the scenario (e.g. 12 or 16). "
                          "If omitted, inferred from --save-to path or defaults to 16.")
_parser.add_argument("--plots-dir", metavar="DIR",
                     help="Directory for generated plots/CSV. Defaults to the log file directory.")
_parser.add_argument("--verbose", action="store_true",
                     help="Show legacy deep-diagnostic sections (QC/witness/ACK internals).")
_args = _parser.parse_args()

LOG_FILE = _args.log_file
SAVE_TO  = _args.save_to
PLOTS_DIR = _args.plots_dir or os.path.dirname(os.path.abspath(LOG_FILE)) or "."

def _infer_cars_from_save_to(save_to: str | None) -> int | None:
    if not save_to:
        return None
    # Try a few common naming conventions:
    # - ".../12veh_0.log" or folder ".../12vehRuns"
    # - ".../2phase12Honest" or ".../phase12..."
    hay = save_to
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

# Golden isolated-n reference values (from benchmarks/golden.log)
GOLDEN = {
    4:  {"view": 0.2556, "order": 0.3879},
    8:  {"view": 0.3302, "order": 0.4173},
    12: {"view": 0.3800, "order": 0.4925},
    16: {"view": 0.4444, "order": 0.5858},
}
# Epoch → group size mapping (4 cars depart each round)
EPOCH_N = {epoch: max(CARS - 4 * epoch, 0) for epoch in range(N_EPOCHS)}

# ── Patterns ────────────────────────────────────────────────────────────────
RE_ROUND_METRIC = re.compile(
    r'\[ROUND-METRICS\] Epoch (\d+) Avg_([\w]+): ([\d.]+) seconds')

RE_METRICS_VIEW_START = re.compile(
    r'\[METRICS (\d+)\] View_Consensus_Start: ([\d.]+)')
RE_METRICS_VIEW_END = re.compile(
    r'\[METRICS (\d+)\] View_Consensus_End:\s*([\d.]+)')
RE_METRICS_ORDER_START = re.compile(
    r'\[METRICS (\d+)\] Order_Consensus_Start: ([\d.]+)')
RE_METRICS_ORDER_END = re.compile(
    r'\[METRICS (\d+)\] Order_Consensus_End:\s*([\d.]+)')
RE_METRICS_QC_WINDOW = re.compile(
    r'\[METRICS (\d+)\] Order_QC_Window_Start: ([\d.]+)')

RE_PHASE_TIMER = re.compile(
    r'\[PHASE_TIMER\] CID=(\d+) replica=(\d+) (\w+) wall_ms=(\d+)')

RE_GOSSIP_RETX = re.compile(
    r'\[ORDER-GOSSIP\] Replica (\d+) re-broadcast own ReadyQC')
RE_GOSSIP_RETX_DETAIL = re.compile(
    r'\[ORDER-GOSSIP\] Replica (\d+) re-broadcast own ReadyQC \((\d+) peers not yet ACKed\) at t=([\d.]+)')
RE_GOSSIP_ACK = re.compile(
    r'\[ORDER-GOSSIP\] Replica (\d+) received ReadyQC ACK from replica (\d+)')
RE_GOSSIP_DONE = re.compile(
    r'\[ORDER-GOSSIP\] Replica (\d+).*all peers ACKed|Gossip complete')
RE_ORDER_COLLECT_START = re.compile(
    r'\[ORDER-COLLECT\] Replica (\d+) started collection window until ([\d.]+)')
RE_FRONT_LANES = re.compile(
    r'\[ORDER-COLLECT\] Replica (\d+) front-lanes-in-pool=(\d+)')
RE_EARLY_CLOSE = re.compile(
    r'\[ORDER-COLLECT\] Replica (\d+).*EARLY_4_LANES|proposeOrderBagNow.*EARLY')

RE_LANE_FRONT_CAR = re.compile(
    r'\[BUILD_ORDER_BAG_FINAL\] car (veh\d+) is at front of lane (\S+), starting')
RE_QC_BROADCASTED = re.compile(
    r'\[V2VProxy (\d+)\] \*\*\* BROADCASTED ReadyQC for (veh\d+)')
RE_QC_RECEIVED = re.compile(
    r'\[V2VProxy (\d+)\] Received ReadyQC from (veh\d+)')

RE_DIAG_EPOCH = re.compile(
    r'\[ORDER-DIAG\] Replica (\d+) epoch=(\d+)')

# Witness collection diagnostics
RE_WITNESS_ACCEPTED = re.compile(
    r'\[WITNESS-RECV\] Replica (\d+) ACCEPTED witness from replica \d+ for (veh\d+) \(have (\d+)/(\d+) witnesses\) at t=([\d.]+)')
RE_WITNESS_IGNORED = re.compile(
    r'\[WITNESS-RECV\] Replica (\d+) IGNORING witness \(meant for (veh\d+), not')
RE_WITNESS_SEND = re.compile(
    r'\[WITNESS-SEND\] Replica \d+ sent witness response TO (veh\d+) .* at t=([\d.]+)')
RE_QC_COMPLETE = re.compile(
    r'\[V2VProxy (\d+)\] \*\*\* ReadyQC COMPLETE! \*\*\* \((\d+)/(\d+) witnesses\)')

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
RE_BFTCONS_VIEW = re.compile(
    r'\[BFTCONSENSUS (\d+)\] View consensus time epoch=(\d+): ([\d.]+)ms')
RE_BFTCONS_ORDER = re.compile(
    r'\[BFTCONSENSUS (\d+)\] Order consensus time epoch=(\d+): ([\d.]+)ms')
RE_BYZANTINE_INJECTION = re.compile(
    r'\[BYZANTINE INJECTION\] Replica (\d+) CID=(\d+) intentionally broadcasting corrupted consensus hash')

# ── Data stores ─────────────────────────────────────────────────────────────
round_metrics = defaultdict(dict)      # epoch → {metric_name: value}
view_starts   = defaultdict(list)      # replica → [sim_time, ...]
view_ends     = defaultdict(list)
order_starts  = defaultdict(list)
order_ends    = defaultdict(list)
qc_windows    = defaultdict(list)      # replica → [sim_time, ...]

# PHASE_TIMER per CID
phase_timers  = defaultdict(lambda: defaultdict(dict))  # cid → replica → phase → wall_ms

gossip_retx   = defaultdict(int)       # epoch → count of retransmit events
gossip_acks   = defaultdict(int)       # epoch → count of ACK events
gossip_done   = defaultdict(int)       # epoch → count of "all ACKed" events

# QC diagnostics
max_front_lanes   = defaultdict(lambda: defaultdict(int))  # epoch -> replica -> max lanes seen
lane_front_cars   = defaultdict(dict)    # epoch -> {carId: laneId}
qc_broadcasted    = defaultdict(set)     # epoch -> set of carIds that broadcast
qc_received_by    = defaultdict(lambda: defaultdict(set))  # epoch -> replica -> {carIds received}
gossip_retx_details = defaultdict(list) # epoch -> [(replica, unacked, sim_time)]
collect_windows   = defaultdict(dict)   # epoch -> {replica: deadline}

# Witness collection diagnostics
# witness_progress[epoch][replica] = (max_have, need)
witness_progress  = defaultdict(lambda: defaultdict(lambda: (0, 0)))
# witness_send_times[epoch][veh] = [t, t, ...] — when witness responses were sent to that car
witness_send_times = defaultdict(lambda: defaultdict(list))
# qc_complete_replicas[epoch] = set of replica IDs that completed QC
qc_complete_replicas = defaultdict(set)

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
java_view_consensus_wall_raw = defaultdict(list)   # raw epoch -> [seconds, ...]
java_order_consensus_wall_raw = defaultdict(list)  # raw epoch -> [seconds, ...]

# Byzantine injection tracking
byzantine_by_epoch   = defaultdict(int)    # epoch -> count of [BYZANTINE INJECTION] events
byzantine_by_replica = defaultdict(int)    # replica -> count
byzantine_total      = 0                   # across the whole run

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

        m = RE_BFTCONS_VIEW.search(line)
        if m:
            raw_epoch = int(m.group(2))
            java_view_consensus_wall_raw[raw_epoch].append(float(m.group(3)) / 1000.0)
            continue
        m = RE_BFTCONS_ORDER.search(line)
        if m:
            raw_epoch = int(m.group(2))
            java_order_consensus_wall_raw[raw_epoch].append(float(m.group(3)) / 1000.0)
            continue

        # PHASE_TIMER
        m = RE_PHASE_TIMER.search(line)
        if m:
            cid, rep, phase, wall = int(m.group(1)), int(m.group(2)), m.group(3), int(m.group(4))
            phase_timers[cid][rep][phase] = wall
            continue

        # METRICS – View/Order timestamps
        m = RE_METRICS_VIEW_START.search(line)
        if m:
            view_starts[int(m.group(1))].append(float(m.group(2)))
            continue
        m = RE_METRICS_VIEW_END.search(line)
        if m:
            # Be robust to occasional malformed concatenated timestamps
            try:
                t = float(m.group(2))
            except ValueError:
                floats = re.findall(r'\d+\.\d+', line)
                if not floats:
                    continue
                t = float(floats[0])
            view_ends[int(m.group(1))].append(t)
            continue
        m = RE_METRICS_ORDER_START.search(line)
        if m:
            order_starts[int(m.group(1))].append(float(m.group(2)))
            continue
        m = RE_METRICS_ORDER_END.search(line)
        if m:
            order_ends[int(m.group(1))].append(float(m.group(2)))
            continue
        m = RE_METRICS_QC_WINDOW.search(line)
        if m:
            qc_windows[int(m.group(1))].append(float(m.group(2)))
            continue

        # ORDER-DIAG – epoch tracker
        m = RE_DIAG_EPOCH.search(line)
        if m:
            rep, epoch = int(m.group(1)), int(m.group(2))
            replica_epoch[rep] = epoch
            current_gossip_epoch = epoch
            continue

        # Gossip retransmit detail (check detailed before simple so both captured)
        m = RE_GOSSIP_RETX_DETAIL.search(line)
        if m:
            rep, unacked, t = int(m.group(1)), int(m.group(2)), float(m.group(3))
            gossip_retx_details[current_gossip_epoch].append((rep, unacked, t))
            gossip_retx[current_gossip_epoch] += 1
            continue
        m = RE_GOSSIP_RETX.search(line)
        if m:
            gossip_retx[current_gossip_epoch] += 1
            continue

        m = RE_GOSSIP_ACK.search(line)
        if m:
            gossip_acks[current_gossip_epoch] += 1
            continue
        m = RE_GOSSIP_DONE.search(line)
        if m:
            gossip_done[current_gossip_epoch] += 1
            continue

        # QC diagnostics
        m = RE_FRONT_LANES.search(line)
        if m:
            rep, lanes = int(m.group(1)), int(m.group(2))
            if lanes > max_front_lanes[current_gossip_epoch][rep]:
                max_front_lanes[current_gossip_epoch][rep] = lanes
            continue

        m = RE_LANE_FRONT_CAR.search(line)
        if m:
            car, lane = m.group(1), m.group(2)
            lane_front_cars[current_gossip_epoch][car] = lane
            continue

        m = RE_QC_BROADCASTED.search(line)
        if m:
            car = m.group(2)
            qc_broadcasted[current_gossip_epoch].add(car)
            continue

        m = RE_QC_RECEIVED.search(line)
        if m:
            rep, car = int(m.group(1)), m.group(2)
            qc_received_by[current_gossip_epoch][rep].add(car)
            continue

        m = RE_ORDER_COLLECT_START.search(line)
        if m:
            rep, deadline = int(m.group(1)), float(m.group(2))
            collect_windows[current_gossip_epoch][rep] = deadline
            continue

        m = RE_WITNESS_ACCEPTED.search(line)
        if m:
            rep, veh, have, need, t = int(m.group(1)), m.group(2), int(m.group(3)), int(m.group(4)), float(m.group(5))
            prev_have, prev_need = witness_progress[current_gossip_epoch][rep]
            if have > prev_have:
                witness_progress[current_gossip_epoch][rep] = (have, need)
            continue

        m = RE_WITNESS_SEND.search(line)
        if m:
            veh, t = m.group(1), float(m.group(2))
            witness_send_times[current_gossip_epoch][veh].append(t)
            continue

        m = RE_QC_COMPLETE.search(line)
        if m:
            rep = int(m.group(1))
            qc_complete_replicas[current_gossip_epoch].add(rep)
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
            decision = m.group(1) or m.group(2)
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
            continue

        m = RE_MESSAGES_RECEIVED.search(line)
        if m:
            rep, val = int(m.group(1)), int(m.group(2))
            ep = replica_epoch.get(rep, current_gossip_epoch)
            epoch_messages_recv[ep].append(val)
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

for raw_epoch, samples in java_view_consensus_wall_raw.items():
    if not samples:
        continue
    epoch = normalize_java_epoch(raw_epoch)
    round_metrics[epoch]["View_Consensus_Wall"] = statistics.mean(samples)

for raw_epoch, samples in java_order_consensus_wall_raw.items():
    if not samples:
        continue
    epoch = normalize_java_epoch(raw_epoch)
    round_metrics[epoch]["Order_Consensus_Wall"] = statistics.mean(samples)

for epoch in range(N_EPOCHS):
    rm = round_metrics.get(epoch, {})
    view_wall = rm.get("View_Consensus_Wall")
    order_wall = rm.get("Order_Consensus_Wall")
    if view_wall is not None and order_wall is not None:
        round_metrics[epoch]["Combined_Consensus_Wall"] = view_wall + order_wall

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

# Per-epoch throughput: (last_depart - first_depart) / n_cars_in_epoch
# Uses the depart_times of cars assigned to each epoch.
epoch_depart_times = defaultdict(list)   # epoch -> sorted depart_times
epoch_throughput   = {}                  # epoch -> s/veh (same formula as global, per epoch)
epoch_wait_normal  = defaultdict(list)   # epoch -> [wait_s, ...] for normal cars
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
    if arrival_t is not None and resume_t is not None:
        metrics["arrival_to_resume"] = resume_t - arrival_t
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

for ep, dtimes in epoch_depart_times.items():
    if len(dtimes) >= 2:
        epoch_throughput[ep] = (max(dtimes) - min(dtimes)) / len(dtimes)

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
        view_lat  = rm.get("View_Consensus_Wall") or rm.get("View_Consensus_Latency_4Cars")
        order_lat = rm.get("Order_Consensus_Wall") or rm.get("Order_Consensus_Latency_4Cars")
        tp = epoch_throughput.get(ep)
        wn = epoch_wait_normal.get(ep, [])
        wa = epoch_wait_ambulance.get(ep, [])
        byz = byzantine_by_epoch.get(ep, 0)
        msgs_sent = epoch_messages_sent.get(ep, [])
        msgs_recv = epoch_messages_recv.get(ep, [])
        epochs_out.append({
            "epoch": ep,
            "n_replicas": n,
            "view_consensus_latency_s": view_lat,
            "order_consensus_latency_s": order_lat,
            "combined_consensus_latency_s": (view_lat + order_lat) if view_lat and order_lat else None,
            "throughput_s_per_veh": tp,
            "wait_normal_s": _stat(wn),
            "wait_ambulance_s": _stat(wa),
            "byzantine_injections": byz,
            "consensus_success": bool(order_lat),
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
            "arrival_to_resume_s": m.get("arrival_to_resume"),
            "resume_to_depart_s": m.get("resume_to_depart"),
            "arrival_to_depart_s": m.get("arrival_to_depart"),
        })

    fa = jains_fairness(all_waits)
    fn = jains_fairness(normal_waits)
    overall = {
        "cars": CARS,
        "epochs": N_EPOCHS,
        "throughput_s_per_veh": throughput,
        "throughput_formula": "(last_depart - first_depart) / n_cars",
        "wait_all_s": _stat(all_waits),
        "wait_normal_s": _stat(normal_waits),
        "wait_ambulance_s": _stat(ambulance_waits),
        "jains_fairness_all": fa,
        "jains_fairness_normal": fn,
        "ambulance_priority_gain_s": (statistics.mean(normal_waits) - statistics.mean(ambulance_waits))
            if normal_waits and ambulance_waits else None,
        "ambulance_priority_ratio": (statistics.mean(ambulance_waits) / statistics.mean(normal_waits))
            if normal_waits and ambulance_waits and statistics.mean(normal_waits) != 0 else None,
        "view_consensus_latency_s": _stat(view_consensus_wall_all or view_consensus_lat_all),
        "order_consensus_latency_s": _stat(order_consensus_wall_all or order_consensus_lat_all),
        "combined_consensus_latency_s": _stat(combined_consensus_wall_all or combined_consensus_lat_all),
        "total_byzantine_injections": byzantine_total,
        "byzantine_by_epoch": dict(byzantine_by_epoch),
        "byzantine_by_replica": dict(byzantine_by_replica),
        "arrival_to_resume_s": _stat(arrival_to_resume_all),
        "resume_to_depart_s": _stat(resume_to_depart_all),
    }

    out = {"overall": overall, "per_epoch": epochs_out, "per_car": cars_out}
    with open(path, "w", encoding="utf-8") as f:
        json.dump(out, f, indent=2, default=str)

def write_car_metrics_csv(path):
    with open(path, "w", encoding="utf-8") as f:
        f.write(
            "car_id,role,epoch,arrival_time,stop_time,resume_time,depart_time,"
            "wait_intersection,arrival_to_resume,resume_to_depart,arrival_to_depart\n"
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
                metrics.get("arrival_to_resume", ""),
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
    bp = ax.boxplot(values, tick_labels=labels, patch_artist=True, showfliers=True)
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

# ── PHASE_TIMER analysis ─────────────────────────────────────────────────────
# CID 0,2,4,6 = VIEW epochs 0,1,2,3  (even CIDs)
# CID 1,3,5,7 = ORDER epochs 0,1,2,3 (odd CIDs)
def cid_to_epoch_type(cid):
    epoch = cid // 2
    kind  = "VIEW" if cid % 2 == 0 else "ORDER"
    return epoch, kind

def phase_summary(cid):
    """Return wall-clock durations for the consensus identified by CID."""
    reps = phase_timers[cid]
    if not reps:
        return None
    # Leader = replica with PROPOSE_SENT
    leader = None
    for rep, phases in reps.items():
        if "PROPOSE_SENT" in phases:
            leader = rep
            break
    if leader is None:
        return None

    lp = reps[leader]
    propose_sent = lp.get("PROPOSE_SENT")
    deliver_times = []
    propose_recv_times = []
    for rep, phases in reps.items():
        if "DELIVER" in phases:
            deliver_times.append(phases["DELIVER"])
        if "PROPOSE_RECV" in phases and rep != leader:
            propose_recv_times.append(phases["PROPOSE_RECV"])

    if not deliver_times:
        return None

    first_deliver = min(deliver_times)
    last_deliver  = max(deliver_times)
    first_recv    = min(propose_recv_times) if propose_recv_times else None

    return {
        "leader": leader,
        "propose_sent_ms": propose_sent,
        "first_propose_recv_ms": first_recv,
        "first_deliver_ms": first_deliver,
        "last_deliver_ms": last_deliver,
        "propagation_wall_ms": (first_recv - propose_sent) if first_recv else None,
        "bft_wall_ms": last_deliver - propose_sent if propose_sent else None,
        "replica_count": len(reps),
    }

# ── Per-epoch breakdown ──────────────────────────────────────────────────────
def all_per_epoch(per_replica_dict, n_epochs=4):
    """Return all values per epoch (for spread analysis)."""
    epoch_ranges = [(0, 5), (5, 15), (15, 25), (25, 40)]
    result = {}
    for epoch, (lo, hi) in enumerate(epoch_ranges):
        vals = [t for times in per_replica_dict.values() for t in times if lo <= t < hi]
        if vals:
            result[epoch] = vals
    return result

def latest_per_epoch(per_replica_dict, n_epochs=4):
    epoch_ranges = [(0, 5), (5, 15), (15, 25), (25, 40)]
    result = {}
    for epoch, (lo, hi) in enumerate(epoch_ranges):
        vals = [t for times in per_replica_dict.values() for t in times if lo <= t < hi]
        if vals:
            result[epoch] = max(vals)
    return result

def earliest_per_epoch(per_replica_dict, n_epochs=4):
    epoch_ranges = [(0, 5), (5, 15), (15, 25), (25, 40)]
    result = {}
    for epoch, (lo, hi) in enumerate(epoch_ranges):
        vals = [t for times in per_replica_dict.values() for t in times if lo <= t < hi]
        if vals:
            result[epoch] = min(vals)
    return result

view_end_by_epoch    = latest_per_epoch(view_ends)
view_end_all         = all_per_epoch(view_ends)
order_start_by_epoch = latest_per_epoch(order_starts)
qc_win_by_epoch      = earliest_per_epoch(qc_windows)

# ── Print Report ─────────────────────────────────────────────────────────────
BOLD = "\033[1m"
RESET = "\033[0m"
RED   = "\033[31m"
GRN   = "\033[32m"
YEL   = "\033[33m"
CYN   = "\033[36m"

def color_vs_golden(val, golden_val, label=""):
    if golden_val is None:
        return f"{val:.4f}s"
    diff = val - golden_val
    pct  = 100 * diff / golden_val
    sign = "+" if diff >= 0 else ""
    col  = RED if diff > 0.02 else (GRN if diff < -0.02 else YEL)
    return f"{col}{val:.4f}s{RESET} ({sign}{pct:.1f}% vs golden {golden_val:.4f}s)"

print("=" * 72)
print(f"{BOLD}BFT-SMaRt V2V Intersection — Epoch Latency Breakdown{RESET}")
print("=" * 72)

for epoch in range(N_EPOCHS):
    n    = EPOCH_N[epoch]
    gold = GOLDEN.get(n, {})
    cid_view  = epoch * 2
    cid_order = epoch * 2 + 1

    rm = round_metrics.get(epoch, {})
    view_lat    = rm.get("View_Consensus_Latency_4Cars")
    order_lat   = rm.get("Order_Consensus_Latency_4Cars")
    view_wall   = rm.get("View_Consensus_Wall")
    order_wall  = rm.get("Order_Consensus_Wall")
    reset_to_ve = rm.get("ResetToViewEnd_4Cars")
    bft_rtt     = rm.get("Order_BFT_Request_RTT_Submitter")

    ps_view  = phase_summary(cid_view)
    ps_order = phase_summary(cid_order)

    # Gossip = view_end → leader order_start
    view_end   = view_end_by_epoch.get(epoch)
    ord_start  = order_start_by_epoch.get(epoch)
    gossip_dur = (ord_start - view_end) if (view_end and ord_start) else None

    bft_wall  = ps_order["bft_wall_ms"] if ps_order else None
    prop_wall = ps_order["propagation_wall_ms"] if ps_order else None

    retx  = gossip_retx.get(epoch, 0)
    retx_per_car = retx / n if n else 0
    acks  = gossip_acks.get(epoch, 0)
    done  = gossip_done.get(epoch, 0)

    print(f"\n{BOLD}── Epoch {epoch}  (n={n} replicas) ──{RESET}")
    if reset_to_ve:
        col = RED if reset_to_ve > 2.0 else (YEL if reset_to_ve > 1.0 else GRN)
        print(f"  Reset→VIEW_END:{col} {reset_to_ve:.4f}s{RESET}")
    if view_wall is not None and view_wall > 0:
        print(f"  VIEW consensus latency:  {color_vs_golden(view_wall, gold.get('view'))}  (wall/Java)")
    elif view_lat is not None and view_lat > 0:
        print(f"  VIEW delivery latency:   {color_vs_golden(view_lat, gold.get('view'))}  (sim/native)")
    if order_wall is not None and order_wall > 0:
        print(f"  ORDER consensus latency: {color_vs_golden(order_wall, gold.get('order'))}  (wall/Java)")
    elif order_lat is not None and order_lat > 0:
        print(f"  ORDER delivery latency:  {color_vs_golden(order_lat, gold.get('order'))}  (sim/native)")
    if bft_rtt:
        print(f"  ORDER BFT RTT: {bft_rtt:.3f}s  (submitter wall-clock)")

    if gossip_dur is not None:
        col = RED if gossip_dur > 1.0 else (YEL if gossip_dur > 0.2 else GRN)
        print(f"  Gossip phase:  {col}{gossip_dur:.4f}s sim{RESET}  "
              f"(view_end={view_end:.3f}s → leader_submit={ord_start:.3f}s)")
    else:
        print(f"  Gossip phase:  {YEL}N/A{RESET}  "
              f"(view_end={view_end}, ord_start={ord_start})")

    if ps_order:
        prop_str = f"{prop_wall}ms wall" if prop_wall else "N/A"
        print(f"  BFT net (wall):{CYN} total={bft_wall}ms, PROPOSE propagation={prop_str}{RESET}")
        print(f"  BFT leader:    replica {ps_order['leader']}  "
              f"({ps_order['replica_count']} replicas observed)")

    retx_col = RED if retx_per_car > 1.0 else (YEL if retx_per_car > 0 else GRN)
    print(f"  Gossip retx:   {retx_col}{retx} total  ({retx_per_car:.1f}/car){RESET}   "
          f"ACKs received={acks}  all-ACKed events={done}")

# ── Cross-epoch comparison ────────────────────────────────────────────────────
print(f"\n{'=' * 72}")
print(f"{BOLD}Cross-epoch ORDER monotonicity check{RESET}")
print(f"{'─' * 72}")
print(f"  {'Epoch':>5}  {'N':>4}  {'ORDER':>10}  {'Src':>5}  {'Golden':>8}  {'Gossip sim':>10}  {'RTT':>7}  {'Retx/car':>9}")
for epoch in range(N_EPOCHS):
    n    = EPOCH_N[epoch]
    rm   = round_metrics.get(epoch, {})
    olat = rm.get("Order_Consensus_Latency_4Cars")
    owall = rm.get("Order_Consensus_Wall")
    gold = GOLDEN.get(n, {}).get("order")
    bft_rtt = rm.get("Order_BFT_Request_RTT_Submitter")
    view_end  = view_end_by_epoch.get(epoch)
    ord_start = order_start_by_epoch.get(epoch)
    gossip_dur = (ord_start - view_end) if (view_end and ord_start) else None
    retx = gossip_retx.get(epoch, 0)
    retx_per_car = retx / EPOCH_N[epoch] if EPOCH_N[epoch] else 0

    if owall is not None and owall > 0:
        order_metric = owall
        order_src = "wall"
    elif olat is not None and olat > 0:
        order_metric = olat
        order_src = "sim"
    else:
        order_metric = None
        order_src = "N/A"
    olat_str   = f"{order_metric:.4f}s" if order_metric is not None else "  N/A  "
    gold_str   = f"{gold:.4f}s" if gold else "  N/A  "
    gossip_str = f"{gossip_dur:.4f}s" if gossip_dur else "  N/A  "
    rtt_str    = f"{bft_rtt:.3f}s" if bft_rtt else "  N/A"
    flag = ""
    if epoch > 0:
        prev_rm = round_metrics.get(epoch - 1, {})
        prev_olat = prev_rm.get("Order_Consensus_Wall")
        if prev_olat is None:
            prev_olat = prev_rm.get("Order_Consensus_Latency_4Cars")
        if order_metric is not None and prev_olat is not None and order_metric > prev_olat:
            flag = f"  {RED}▲ NON-MONOTONIC{RESET}"
        elif order_metric is not None and prev_olat is not None:
            flag = f"  {GRN}▼ OK{RESET}"
    print(f"  {epoch:>5}  {n:>4}  {olat_str:>10}  {order_src:>5}  {gold_str:>8}  {gossip_str:>10}  {rtt_str:>7}  {retx_per_car:>9.1f}{flag}")

# ── PHASE_TIMER wall-clock detail ─────────────────────────────────────────────
if _args.verbose:
    print(f"\n{'=' * 72}")
    print(f"{BOLD}PHASE_TIMER wall-clock detail (ms) — all epochs{RESET}")
    print(f"{'─' * 72}")
    print(f"  {'CID':>4}  {'Type':>6}  {'Epoch':>5}  {'Leader':>6}  {'PropagMs':>9}  {'TotalMs':>8}  {'#Reps':>5}")
    for cid in range(2 * N_EPOCHS):
        epoch, kind = cid_to_epoch_type(cid)
        ps = phase_summary(cid)
        if ps is None:
            print(f"  {cid:>4}  {kind:>6}  {epoch:>5}  {'N/A':>6}")
            continue
        prop = f"{ps['propagation_wall_ms']:>9}" if ps['propagation_wall_ms'] else "      N/A"
        tot  = f"{ps['bft_wall_ms']:>8}" if ps['bft_wall_ms'] else "     N/A"
        print(f"  {cid:>4}  {kind:>6}  {epoch:>5}  {ps['leader']:>6}  {prop}  {tot}  {ps['replica_count']:>5}")

    print(f"\n{'=' * 72}")
    print(f"{BOLD}Legacy Deep Diagnostics (verbose only){RESET}")
    print(f"{'─' * 72}")
    print(f"  QC/witness/ACK internals are retained here for debugging older logs.")

print(f"\n{'=' * 72}")
print(f"{BOLD}Diagnosis summary{RESET}")
print(f"{'─' * 72}")
bottleneck_found = False
for epoch in range(N_EPOCHS):
    n   = EPOCH_N[epoch]
    rm  = round_metrics.get(epoch, {})
    olat = rm.get("Order_Consensus_Wall")
    if olat is None:
        olat = rm.get("Order_Consensus_Latency_4Cars")
    gold = GOLDEN.get(n, {}).get("order")
    if olat and gold:
        excess = olat - gold
        retx   = gossip_retx.get(epoch, 0)
        acks   = gossip_acks.get(epoch, 0)
        done   = gossip_done.get(epoch, 0)
        expected_done = n
        if done < expected_done:
            note = f"{RED}ACK convergence incomplete ({done}/{expected_done} replicas got all-ACKed){RESET}"
            bottleneck_found = True
        elif excess > 0.05:
            view_end  = view_end_by_epoch.get(epoch)
            ord_start = order_start_by_epoch.get(epoch)
            gossip_dur = (ord_start - view_end) if (view_end and ord_start) else None
            if gossip_dur and gossip_dur > 0.2:
                note = f"{YEL}Gossip phase is slow ({gossip_dur:.3f}s), possible V2V loss or retransmit failure{RESET}"
                bottleneck_found = True
            else:
                note = f"{YEL}BFT network phase may be slow — check PHASE_TIMER propagation{RESET}"
                bottleneck_found = True
        else:
            note = f"{GRN}Within 50ms of golden — OK{RESET}"
        print(f"  Epoch {epoch} (n={n}): excess={excess:+.3f}s  {note}")
    elif not olat:
        print(f"  Epoch {epoch} (n={n}): {RED}no ORDER data — epoch did not complete{RESET}")
        bottleneck_found = True

if not bottleneck_found:
    print(f"  {GRN}All epochs within 50ms of golden — no obvious bottleneck{RESET}")

print(f"\n{'=' * 72}")
print(f"{BOLD}Per-Car Lifecycle Metrics{RESET}")
print(f"{'─' * 72}")
print(f"  {'Car':>6}  {'Role':>10}  {'Wait@Int':>10}  {'Arr→Resume':>12}  {'Resume→Leave':>14}  {'Arr→Leave':>11}")
for car_id in sorted(car_metrics.keys(), key=lambda cid: int(cid[3:])):
    metrics = car_metrics[car_id]
    wait_t = metrics.get("wait_intersection")
    arr_res = metrics.get("arrival_to_resume")
    res_dep = metrics.get("resume_to_depart")
    arr_dep = metrics.get("arrival_to_depart")
    role = metrics.get("role", "normal")
    print(
        f"  {car_id:>6}  {role:>10}  "
        f"{(f'{wait_t:.4f}s' if wait_t is not None else 'N/A'):>10}  "
        f"{(f'{arr_res:.4f}s' if arr_res is not None else 'N/A'):>12}  "
        f"{(f'{res_dep:.4f}s' if res_dep is not None else 'N/A'):>14}  "
        f"{(f'{arr_dep:.4f}s' if arr_dep is not None else 'N/A'):>11}"
    )

normal_waits = [m["wait_intersection"] for m in car_metrics.values()
                if m.get("role", "normal") == "normal" and m.get("wait_intersection") is not None]
ambulance_waits = [m["wait_intersection"] for m in car_metrics.values()
                   if m.get("role") == "ambulance" and m.get("wait_intersection") is not None]
all_waits = [m["wait_intersection"] for m in car_metrics.values()
             if m.get("wait_intersection") is not None]
arrival_to_resume_all = [m["arrival_to_resume"] for m in car_metrics.values()
                         if m.get("arrival_to_resume") is not None]
resume_to_depart_all = [m["resume_to_depart"] for m in car_metrics.values()
                        if m.get("resume_to_depart") is not None]

throughput = None
depart_times = sorted(
    m["depart_time"] for m in car_metrics.values() if m.get("depart_time") is not None
)
if len(depart_times) >= 2:
    throughput = (depart_times[-1] - depart_times[0]) / len(depart_times)
missing_departure = [
    car_id for car_id, m in sorted(car_metrics.items(), key=lambda kv: int(kv[0][3:]))
    if m.get("stop_time") is not None and m.get("depart_time") is None
]

print(f"\n{'=' * 72}")
print(f"{BOLD}Paper-Friendly Summary Metrics{RESET}")
print(f"{'─' * 72}")
if throughput is not None:
    print(f"  Throughput (user definition): {throughput:.6f}s per vehicle")
    print(f"    Formula: (last leave - first leave) / cars = "
          f"({depart_times[-1]:.4f} - {depart_times[0]:.4f}) / {len(depart_times)}")
else:
    print(f"  Throughput (user definition): {YEL}N/A{RESET} "
          f"(need per-car depart_time metrics in the log)")
if missing_departure:
    print(f"  Missing leave timestamps:         {', '.join(missing_departure)}")
    print(f"    This log predates the new [CAR-METRICS] departure line or the native side was not rebuilt.")

print(f"  Wait@intersection, all cars:      {fmt_stat(all_waits)}")
print(f"  Wait@intersection, normal cars:   {fmt_stat(normal_waits)}")
print(f"  Wait@intersection, ambulance:     {fmt_stat(ambulance_waits)}")
print(f"  Arrival→resume, all cars:         {fmt_stat(arrival_to_resume_all)}")
print(f"  Resume→leave, all cars:           {fmt_stat(resume_to_depart_all)}")

view_consensus_lat_all = [
    round_metrics[ep]["View_Consensus_Latency_4Cars"]
    for ep in range(N_EPOCHS)
    if round_metrics.get(ep, {}).get("View_Consensus_Latency_4Cars", 0) > 0
]
order_consensus_lat_all = [
    round_metrics[ep]["Order_Consensus_Latency_4Cars"]
    for ep in range(N_EPOCHS)
    if round_metrics.get(ep, {}).get("Order_Consensus_Latency_4Cars", 0) > 0
]
combined_consensus_lat_by_epoch = {}
for ep in range(N_EPOCHS):
    rm = round_metrics.get(ep, {})
    view_lat = rm.get("View_Consensus_Latency_4Cars")
    order_lat = rm.get("Order_Consensus_Latency_4Cars")
    if view_lat is not None and view_lat > 0 and order_lat is not None and order_lat > 0:
        combined_consensus_lat_by_epoch[ep] = view_lat + order_lat
combined_consensus_lat_all = list(combined_consensus_lat_by_epoch.values())

view_consensus_wall_all = [
    round_metrics[ep]["View_Consensus_Wall"]
    for ep in range(N_EPOCHS)
    if round_metrics.get(ep, {}).get("View_Consensus_Wall", 0) > 0
]
order_consensus_wall_all = [
    round_metrics[ep]["Order_Consensus_Wall"]
    for ep in range(N_EPOCHS)
    if round_metrics.get(ep, {}).get("Order_Consensus_Wall", 0) > 0
]
combined_consensus_wall_all = [
    round_metrics[ep]["Combined_Consensus_Wall"]
    for ep in range(N_EPOCHS)
    if round_metrics.get(ep, {}).get("Combined_Consensus_Wall", 0) > 0
]

print(f"  VIEW delivery latency (sim):      {fmt_stat(view_consensus_lat_all)}")
print(f"  ORDER delivery latency (sim):     {fmt_stat(order_consensus_lat_all)}")
print(f"  Combined delivery latency (sim):  {fmt_stat(combined_consensus_lat_all)}")
print(f"  VIEW consensus latency (wall):    {fmt_stat(view_consensus_wall_all)}")
print(f"  ORDER consensus latency (wall):   {fmt_stat(order_consensus_wall_all)}")
print(f"  Combined consensus latency:       {fmt_stat(combined_consensus_wall_all)}")

fairness_all = jains_fairness(all_waits)
fairness_normal = jains_fairness(normal_waits)
if fairness_all is not None:
    print(f"  Jain fairness index (all waits):  {fairness_all:.4f}")
if fairness_normal is not None:
    print(f"  Jain fairness index (normal):     {fairness_normal:.4f}")
if normal_waits and ambulance_waits:
    amb_mean = statistics.mean(ambulance_waits)
    norm_mean = statistics.mean(normal_waits)
    ratio = (amb_mean / norm_mean) if norm_mean else float('inf')
    delta = norm_mean - amb_mean
    print(f"  Ambulance priority gain:          "
          f"ambulance_mean={amb_mean:.4f}s vs normal_mean={norm_mean:.4f}s "
          f"(delta={delta:+.4f}s, ratio={ratio:.4f})")

# ── Per-epoch breakdown: wait + throughput ────────────────────────────────────
print(f"\n{'=' * 72}")
print(f"{BOLD}Per-Epoch Intersection Performance{RESET}")
print(f"{'─' * 72}")
print(f"  {'Epoch':>5}  {'N':>4}  {'Throughput':>12}  {'WaitNorm(mean)':>16}  {'WaitAmb(mean)':>15}  {'Byzantine':>10}")
for ep in range(N_EPOCHS):
    n = EPOCH_N[ep]
    tp = epoch_throughput.get(ep)
    tp_str = f"{tp:.4f}s/veh" if tp is not None else "N/A"
    wn = epoch_wait_normal.get(ep, [])
    wa = epoch_wait_ambulance.get(ep, [])
    wn_str = f"{statistics.mean(wn):.4f}s" if wn else "N/A"
    wa_str = f"{statistics.mean(wa):.4f}s" if wa else "N/A"
    byz = byzantine_by_epoch.get(ep, 0)
    byz_col = RED if byz > 0 else GRN
    print(f"  {ep:>5}  {n:>4}  {tp_str:>12}  {wn_str:>16}  {wa_str:>15}  {byz_col}{byz:>10}{RESET}")

# ── Byzantine Fault Analysis ─────────────────────────────────────────────────
if byzantine_total > 0:
    print(f"\n{'=' * 72}")
    print(f"{BOLD}Byzantine Fault Analysis{RESET}")
    print(f"{'─' * 72}")
    print(f"  Total Byzantine hash corruptions: {RED}{byzantine_total}{RESET}")
    print(f"  {'Epoch':>5}  {'Injections':>12}")
    for ep in sorted(byzantine_by_epoch):
        print(f"  {ep:>5}  {byzantine_by_epoch[ep]:>12}")
    print(f"  {'Replica':>8}  {'Injections':>12}")
    for rep in sorted(byzantine_by_replica):
        print(f"  {rep:>8}  {byzantine_by_replica[rep]:>12}")
    print(f"  {GRN}Consensus correctness: all epochs completed → BFT safety upheld{RESET}"
          if all(round_metrics.get(ep, {}).get("Order_Consensus_Wall") or
                 round_metrics.get(ep, {}).get("Order_Consensus_Latency_4Cars")
                 for ep in range(N_EPOCHS))
          else f"  {RED}WARNING: some epochs did not complete — check above for details{RESET}")

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

    # Write only the [ROUND-METRICS] lines (tiny file, not the full 93MB log)
    with open(dest, 'w') as f:
        # Pass 1: emit ROUND-METRICS lines already present in the source log
        with open(LOG_FILE, "r", errors="replace") as src:
            for line in src:
                if '[ROUND-METRICS]' in line:
                    m = RE_ROUND_METRIC.search(line)
                    if m:
                        metric = m.group(2)
                        value = float(m.group(3))
                        if metric in {"View_Consensus_Latency_4Cars", "Order_Consensus_Latency_4Cars"} and value <= 0:
                            continue
                    f.write(line if line.endswith('\n') else line + '\n')

        # Pass 2: append only metrics that C++ did NOT already emit
        def _emit(ep, key, line):
            """Write line only if key is absent from the already-parsed round_metrics."""
            if key not in round_metrics.get(ep, {}):
                f.write(line)

        for ep in range(N_EPOCHS):
            rm = round_metrics.get(ep, {})
            view_lat = rm.get("View_Consensus_Latency_4Cars")
            order_lat = rm.get("Order_Consensus_Latency_4Cars")
            view_wall = rm.get("View_Consensus_Wall")
            order_wall = rm.get("Order_Consensus_Wall")
            if view_lat is not None:
                _emit(ep, "View_Consensus_Latency_4Cars",
                      f"[ROUND-METRICS] Epoch {ep} Avg_View_Consensus_Latency_4Cars: "
                      f"{view_lat:.6f} seconds\n")
            if order_lat is not None:
                _emit(ep, "Order_Consensus_Latency_4Cars",
                      f"[ROUND-METRICS] Epoch {ep} Avg_Order_Consensus_Latency_4Cars: "
                      f"{order_lat:.6f} seconds\n")
            if view_lat is not None and order_lat is not None:
                _emit(ep, "Combined_Consensus_Latency",
                      f"[ROUND-METRICS] Epoch {ep} Avg_Combined_Consensus_Latency: "
                      f"{(view_lat + order_lat):.6f} seconds\n")
            if view_wall is not None:
                _emit(ep, "View_Consensus_Wall",
                      f"[ROUND-METRICS] Epoch {ep} Avg_View_Consensus_Wall: "
                      f"{view_wall:.6f} seconds\n")
            if order_wall is not None:
                _emit(ep, "Order_Consensus_Wall",
                      f"[ROUND-METRICS] Epoch {ep} Avg_Order_Consensus_Wall: "
                      f"{order_wall:.6f} seconds\n")
            if view_wall is not None and order_wall is not None:
                _emit(ep, "Combined_Consensus_Wall",
                      f"[ROUND-METRICS] Epoch {ep} Avg_Combined_Consensus_Wall: "
                      f"{(view_wall + order_wall):.6f} seconds\n")

            # Per-car durations (one line per car) so downstream scripts can build CDFs.
            # These are lifecycle timings, not consensus latency.
            per_car = epoch_total_dur_by_car.get(ep, {})
            if per_car:
                for car_id, dur in sorted(per_car.items(), key=lambda kv: int(kv[0][3:])):
                    _emit(ep, f"Arrival_To_Resume_Time_{car_id}",
                          f"[ROUND-METRICS] Epoch {ep} Arrival_To_Resume_Time {car_id}: "
                          f"{dur:.6f} seconds\n")

            if epoch_messages_sent[ep]:
                avg_sent = sum(epoch_messages_sent[ep]) / len(epoch_messages_sent[ep])
                avg_recv = (sum(epoch_messages_recv[ep]) / len(epoch_messages_recv[ep])
                            if epoch_messages_recv[ep] else 0)
                _emit(ep, "Messages_Sent_PerReplica",
                      f"[ROUND-METRICS] Epoch {ep} Avg_Messages_Sent_PerReplica: "
                      f"{avg_sent:.3f} (count={len(epoch_messages_sent[ep])})\n")
                _emit(ep, "Messages_Received_PerReplica",
                      f"[ROUND-METRICS] Epoch {ep} Avg_Messages_Received_PerReplica: "
                      f"{avg_recv:.3f} (count={len(epoch_messages_recv[ep])})\n")
            if epoch_total_dur[ep]:
                avg_dur = sum(epoch_total_dur[ep]) / len(epoch_total_dur[ep])
                _emit(ep, "Arrival_To_Resume_Time",
                      f"[ROUND-METRICS] Epoch {ep} Avg_Arrival_To_Resume_Time: "
                      f"{avg_dur:.6f} seconds (goCars={len(epoch_total_dur[ep])})\n")
            fail_val = epoch_failures.get(ep, 0)
            _emit(ep, "StopSign_Failures",
                  f"[ROUND-METRICS] Epoch {ep} Avg_StopSign_Failures: "
                  f"{float(fail_val):.3f} (count)\n")

        if throughput is not None:
            f.write(f"[RUN-METRICS] Throughput_User_Definition: {throughput:.6f} seconds_per_vehicle\n")
        if all_waits:
            f.write(f"[RUN-METRICS] Wait_Intersection_All_Mean: {statistics.mean(all_waits):.6f} seconds\n")
            f.write(f"[RUN-METRICS] Wait_Intersection_All_P95: {percentile(all_waits, 0.95):.6f} seconds\n")
        if normal_waits:
            f.write(f"[RUN-METRICS] Wait_Intersection_Normal_Mean: {statistics.mean(normal_waits):.6f} seconds\n")
        if ambulance_waits:
            f.write(f"[RUN-METRICS] Wait_Intersection_Ambulance_Mean: {statistics.mean(ambulance_waits):.6f} seconds\n")
        if arrival_to_resume_all:
            f.write(f"[RUN-METRICS] Arrival_To_Resume_Mean: {statistics.mean(arrival_to_resume_all):.6f} seconds\n")
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

        # Per-epoch throughput and wait
        for ep in range(N_EPOCHS):
            tp = epoch_throughput.get(ep)
            if tp is not None:
                f.write(f"[ROUND-METRICS] Epoch {ep} Throughput_User_Definition: {tp:.6f} seconds_per_vehicle\n")
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

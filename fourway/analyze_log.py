#!/usr/bin/env python3
"""
BFT-SMaRt V2V Intersection Log Analyzer
Breaks down VIEW and ORDER consensus latency per epoch into:
  - Gossip phase duration (QC collection)
  - BFT network phase (PROPOSE → DELIVER)
  - Gossip retransmit count
  - ACK convergence
  - QC pool progress per replica (front-lanes-in-pool)
  - Missing QC diagnostics
Compares against golden isolated-n values.
"""

import sys
import re
import os
import shutil
import argparse
import statistics
from collections import defaultdict

_parser = argparse.ArgumentParser(description="BFT-SMaRt V2V log analyzer")
_parser.add_argument("log_file", nargs="?", default="/tmp/bft-all-replicas.log",
                     help="Path to combined replica log (default: /tmp/bft-all-replicas.log)")
_parser.add_argument("--save-to", metavar="DIR",
                     help="Copy log into DIR as 16veh_<i>.log for later batch analysis")
_args = _parser.parse_args()

LOG_FILE = _args.log_file
SAVE_TO  = _args.save_to

# Golden isolated-n reference values (from benchmarks/golden.log)
GOLDEN = {
    4:  {"view": 0.2556, "order": 0.3879},
    8:  {"view": 0.3302, "order": 0.4173},
    12: {"view": 0.3800, "order": 0.4925},
    16: {"view": 0.4444, "order": 0.5858},
}
# Epoch → group size mapping (4 cars depart each round)
EPOCH_N = {0: 16, 1: 12, 2: 8, 3: 4}

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
RE_GO_STRING = re.compile(r'(veh\d+):GO:\d+')
RE_RESUME = re.compile(r'\[RESUME\] Replica (\d+): JNI received GO signal')
RE_RESUME_MSG = re.compile(r'\[HANDLE-SELF-MSG\] Replica (\d+): .*msgName=resumeVehicle')

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
            view_ends[int(m.group(1))].append(float(m.group(2)))
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

        m = RE_ORDER_DECISION.search(line)
        if m:
            decision = m.group(1) or m.group(2)
            cars = RE_GO_STRING.findall(decision)
            for car in cars:
                if car not in go_decision_epoch:
                    go_decision_epoch[car] = current_gossip_epoch
            continue

        m = RE_RESUME.search(line)
        if m:
            resume_signal_received.add(int(m.group(1)))
            continue

        m = RE_RESUME_MSG.search(line)
        if m:
            resume_msg_handled.add(int(m.group(1)))
            continue

print("Done.\n")

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

for epoch in range(4):
    n    = EPOCH_N[epoch]
    gold = GOLDEN.get(n, {})
    cid_view  = epoch * 2
    cid_order = epoch * 2 + 1

    rm = round_metrics.get(epoch, {})
    view_lat    = rm.get("View_Consensus_Latency_4Cars")
    order_lat   = rm.get("Order_Consensus_Latency_4Cars")
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
    if view_lat:
        print(f"  VIEW  latency: {color_vs_golden(view_lat, gold.get('view'))}")
    if order_lat:
        print(f"  ORDER latency: {color_vs_golden(order_lat, gold.get('order'))}")
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
print(f"  {'Epoch':>5}  {'N':>4}  {'ORDER sim':>10}  {'Golden':>8}  {'Gossip sim':>10}  {'RTT':>7}  {'Retx/car':>9}")
for epoch in range(4):
    n    = EPOCH_N[epoch]
    rm   = round_metrics.get(epoch, {})
    olat = rm.get("Order_Consensus_Latency_4Cars")
    gold = GOLDEN.get(n, {}).get("order")
    bft_rtt = rm.get("Order_BFT_Request_RTT_Submitter")
    view_end  = view_end_by_epoch.get(epoch)
    ord_start = order_start_by_epoch.get(epoch)
    gossip_dur = (ord_start - view_end) if (view_end and ord_start) else None
    retx = gossip_retx.get(epoch, 0)
    retx_per_car = retx / EPOCH_N[epoch] if EPOCH_N[epoch] else 0

    olat_str   = f"{olat:.4f}s" if olat else "  N/A  "
    gold_str   = f"{gold:.4f}s" if gold else "  N/A  "
    gossip_str = f"{gossip_dur:.4f}s" if gossip_dur else "  N/A  "
    rtt_str    = f"{bft_rtt:.3f}s" if bft_rtt else "  N/A"
    flag = ""
    if epoch > 0:
        prev_olat = round_metrics.get(epoch-1, {}).get("Order_Consensus_Latency_4Cars")
        if olat and prev_olat and olat > prev_olat:
            flag = f"  {RED}▲ NON-MONOTONIC{RESET}"
        elif olat and prev_olat:
            flag = f"  {GRN}▼ OK{RESET}"
    print(f"  {epoch:>5}  {n:>4}  {olat_str:>10}  {gold_str:>8}  {gossip_str:>10}  {rtt_str:>7}  {retx_per_car:>9.1f}{flag}")

# ── PHASE_TIMER wall-clock detail ─────────────────────────────────────────────
print(f"\n{'=' * 72}")
print(f"{BOLD}PHASE_TIMER wall-clock detail (ms) — all epochs{RESET}")
print(f"{'─' * 72}")
print(f"  {'CID':>4}  {'Type':>6}  {'Epoch':>5}  {'Leader':>6}  {'PropagMs':>9}  {'TotalMs':>8}  {'#Reps':>5}")
for cid in range(8):
    epoch, kind = cid_to_epoch_type(cid)
    ps = phase_summary(cid)
    if ps is None:
        print(f"  {cid:>4}  {kind:>6}  {epoch:>5}  {'N/A':>6}")
        continue
    prop = f"{ps['propagation_wall_ms']:>9}" if ps['propagation_wall_ms'] else "      N/A"
    tot  = f"{ps['bft_wall_ms']:>8}" if ps['bft_wall_ms'] else "     N/A"
    print(f"  {cid:>4}  {kind:>6}  {epoch:>5}  {ps['leader']:>6}  {prop}  {tot}  {ps['replica_count']:>5}")

# ── QC Collection Diagnostics ─────────────────────────────────────────────────
print(f"\n{'=' * 72}")
print(f"{BOLD}QC Collection diagnostics per epoch{RESET}")
print(f"{'─' * 72}")
for epoch in range(4):
    n = EPOCH_N[epoch]
    fronts = lane_front_cars.get(epoch, {})
    broadcast = qc_broadcasted.get(epoch, set())
    fl = max_front_lanes.get(epoch, {})
    retx_det = gossip_retx_details.get(epoch, [])
    cw = collect_windows.get(epoch, {})

    print(f"\n  Epoch {epoch} (n={n}):")
    if fronts:
        front_str = ", ".join(f"{car}({lane})" for car, lane in sorted(fronts.items()))
        print(f"    Lane-front cars ({len(fronts)}/4 expected): {front_str}")
    else:
        print(f"    Lane-front cars: none detected")

    # Pool progress per replica
    if fl:
        # Focus on lane-front replicas (the ones that opened collection windows)
        front_rep_ids = sorted(set(int(c[3:]) for c in fronts.keys()))
        all_rep_ids = sorted(fl.keys())
        print(f"    Pool progress (max front-lanes-in-pool per replica):")
        for rep in all_rep_ids:
            mx = fl[rep]
            col = GRN if mx >= 4 else (YEL if mx >= 3 else RED)
            marker = " ← LEADER" if (cw.get(rep) and rep == min(front_rep_ids, default=-1)) else ""
            # Show which replica is the view leader (lowest ID among lane-front cars, heuristic)
            print(f"      replica {rep:>2}: {col}{mx}/4{RESET}{marker}")

    # Missing QCs at the lowest-numbered collection-window replica (likely leader)
    if qc_received_by.get(epoch) and fronts:
        # Find which replica opened a collection window first (min ID among those with windows)
        window_openers = sorted(cw.keys()) if cw else []
        if window_openers:
            leader_rep = window_openers[0]
            received = qc_received_by[epoch].get(leader_rep, set())
            received_with_self = received | {f"veh{leader_rep}"}
            missing = broadcast - received_with_self
            missing_fronts = missing & set(fronts.keys())
            if missing_fronts:
                print(f"    {RED}Leader (replica {leader_rep}) MISSING lane-front QCs: {sorted(missing_fronts)}{RESET}")
            elif missing:
                print(f"    {YEL}Leader (replica {leader_rep}) missing non-front QCs: {sorted(missing)}{RESET}")
            else:
                print(f"    {GRN}Leader (replica {leader_rep}) received all broadcast QCs{RESET}")

    # Retransmit details
    if retx_det:
        print(f"    Retransmits ({len(retx_det)} events):")
        for rep, unacked, t in retx_det:
            print(f"      replica {rep} at t={t:.3f}s  ({unacked} peers unACKed)")
    else:
        print(f"    Retransmits: {YEL}none{RESET}")

# ── Witness Collection Diagnostics ───────────────────────────────────────────
print(f"\n{'=' * 72}")
print(f"{BOLD}Witness collection diagnostics per epoch{RESET}")
print(f"{'─' * 72}")
print(f"  (Detects channel collisions: if witness sends all pile up in a tight window,")
print(f"   most are lost to CSMA contention and replicas stall below QC threshold.)")
for epoch in range(4):
    n = EPOCH_N[epoch]
    wp = witness_progress.get(epoch, {})
    wst = witness_send_times.get(epoch, {})
    completed = qc_complete_replicas.get(epoch, set())

    print(f"\n  Epoch {epoch} (n={n}):")

    # Per-replica witness progress (only replicas that received at least 1 witness)
    if wp:
        print(f"    Witness progress per replica (max received / threshold):")
        for rep in sorted(wp.keys()):
            have, need = wp[rep]
            done_flag = f" {GRN}→ QC COMPLETE{RESET}" if rep in completed else (
                        f" {RED}→ STALLED (need {need}){RESET}" if need > 0 and have < need else "")
            col = GRN if rep in completed else (YEL if have > 0 else RED)
            print(f"      replica {rep:>2} (veh{rep}): {col}{have}/{need}{RESET}{done_flag}")
        # Replicas that received no witnesses at all
        no_witness = [r for r in range(n) if r not in wp and r not in completed]
        # Only flag replicas that should be collecting (lane-front cars)
        fronts = lane_front_cars.get(epoch, {})
        front_rep_ids = set(int(c[3:]) for c in fronts.keys())
        stalled_fronts = front_rep_ids - completed - set(wp.keys())
        if stalled_fronts:
            print(f"    {RED}Lane-front replicas with ZERO witnesses received: {sorted(stalled_fronts)}{RESET}")
    else:
        print(f"    No witness receipts recorded.")

    # Witness send-time collision analysis per car
    if wst:
        print(f"    Witness send-time window per car (collision risk if window < 2ms × n_senders):")
        for veh in sorted(wst.keys()):
            times = sorted(wst[veh])
            if len(times) < 2:
                window_ms = 0.0
            else:
                window_ms = (times[-1] - times[0]) * 1000.0
            n_sent = len(times)
            t_min = times[0] if times else 0
            t_max = times[-1] if times else 0
            # Danger: if n_sent witnesses arrive in < 2ms each they will collide
            min_safe_ms = n_sent * 0.5  # 802.11p frame ~0.5ms
            col = RED if window_ms < min_safe_ms else (YEL if window_ms < min_safe_ms * 2 else GRN)
            print(f"      {veh}: {n_sent} witnesses sent in {col}{window_ms:.2f}ms{RESET} window "
                  f"(t={t_min:.4f}s–{t_max:.4f}s)  min_safe≈{min_safe_ms:.1f}ms")
    else:
        print(f"    No witness send events recorded.")

    # Summary
    n_completed = len(completed)
    col = GRN if n_completed == n else (YEL if n_completed > 0 else RED)
    print(f"    QC completed: {col}{n_completed}/{n} replicas{RESET}")

# ── Gossip ACK convergence ────────────────────────────────────────────────────
print(f"\n{'=' * 72}")
print(f"{BOLD}Gossip ACK convergence per epoch{RESET}")
print(f"{'─' * 72}")
print(f"  {'Epoch':>5}  {'N':>4}  {'Retx events':>12}  {'Retx/car':>9}  {'ACKs':>6}  {'All-ACK events':>15}")
for epoch in range(4):
    n    = EPOCH_N[epoch]
    retx = gossip_retx.get(epoch, 0)
    acks = gossip_acks.get(epoch, 0)
    done = gossip_done.get(epoch, 0)
    retx_per_car = retx / n if n else 0
    print(f"  {epoch:>5}  {n:>4}  {retx:>12}  {retx_per_car:>9.1f}  {acks:>6}  {done:>15}")

print(f"\n{'=' * 72}")
print(f"{BOLD}VIEW_END spread and gossip impact per epoch{RESET}")
print(f"{'─' * 72}")
print(f"  {'Epoch':>5}  {'N':>4}  {'MinVEnd':>8}  {'MaxVEnd':>8}  {'Spread':>8}  {'GossipDur':>10}  {'Note'}")
for epoch in range(4):
    n = EPOCH_N[epoch]
    ve_all = view_end_all.get(epoch, [])
    if not ve_all:
        print(f"  {epoch:>5}  {n:>4}  (no VIEW_END data)")
        continue
    ve_min = min(ve_all)
    ve_max = max(ve_all)
    spread = ve_max - ve_min
    gossip_dur = None
    if view_end_by_epoch.get(epoch) and order_start_by_epoch.get(epoch):
        gossip_dur = order_start_by_epoch[epoch] - view_end_by_epoch[epoch]
    g_str = f"{gossip_dur:.4f}s" if gossip_dur else "  N/A  "
    note = ""
    if gossip_dur and gossip_dur > spread + 0.15:
        retx_extra = gossip_dur - spread
        note = f"{YEL}~{retx_extra:.3f}s beyond spread → ~{retx_extra/0.085:.0f} retransmit cycles needed{RESET}"
    elif gossip_dur and gossip_dur <= spread + 0.10:
        note = f"{GRN}gossip ≈ spread (immediate after last VIEW_END){RESET}"
    spread_col = RED if spread > 0.1 else (YEL if spread > 0.02 else GRN)
    print(f"  {epoch:>5}  {n:>4}  {ve_min:8.3f}s  {ve_max:8.3f}s  {spread_col}{spread:8.3f}s{RESET}  {g_str:>10}  {note}")

print(f"\n{'=' * 72}")
print(f"{BOLD}Diagnosis summary{RESET}")
print(f"{'─' * 72}")
bottleneck_found = False
for epoch in range(4):
    n   = EPOCH_N[epoch]
    rm  = round_metrics.get(epoch, {})
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
print(f"{BOLD}Vehicle Lifecycle Diagnostics{RESET}")
print(f"{'─' * 72}")
print(f"  {'Car':>6}  {'GO Epoch':>10}  {'JNI RESUME':>12}  {'OMNeT JNI-MSG':>15}")
all_cars = [f"veh{i}" for i in range(16)]
for car in all_cars:
    rep_id = int(car[3:])
    epoch = go_decision_epoch.get(car, "NONE")
    ep_str = f"{epoch}" if isinstance(epoch, int) else f"{RED}NONE{RESET}"
    
    jni_res = f"{GRN}YES{RESET}" if rep_id in resume_signal_received else f"{RED}NO{RESET}"
    msg_han = f"{GRN}YES{RESET}" if rep_id in resume_msg_handled else f"{RED}NO{RESET}"
    
    print(f"  {car:>6}  {ep_str:>10}  {jni_res:>12}  {msg_han:>15}")

print()

# ── Save log to collection dir ────────────────────────────────────────────────
if SAVE_TO:
    os.makedirs(SAVE_TO, exist_ok=True)
    existing = [f for f in os.listdir(SAVE_TO) if re.match(r'16veh_\d+\.log$', f)]
    idx  = len(existing)
    dest = os.path.join(SAVE_TO, f"16veh_{idx}.log")
    shutil.copy2(LOG_FILE, dest)
    print(f"Log saved → {dest}  (run #{idx})")

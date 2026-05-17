#!/usr/bin/env python3
"""
diagnose_pbft_delivery.py  —  pinpoints why replicas miss consensus.

Usage:
    python fourway/diagnose_pbft_delivery.py /tmp/resdb-simulation.log
    python fourway/diagnose_pbft_delivery.py benchmarks/Priority16cars/run_0/16veh_0.log

Reads sinr_V*.csv from the same directory as the log (silently skipped if absent).
"""

import re
import sys
import glob
import os
from collections import defaultdict

# ─── helpers ────────────────────────────────────────────────────────────────

def _int(s):
    try:
        return int(s)
    except (TypeError, ValueError):
        return None

def _float(s):
    try:
        return float(s)
    except (TypeError, ValueError):
        return None

# ─── parsing ────────────────────────────────────────────────────────────────

RE_CERT_BCAST   = re.compile(r'\[CERT-BROADCAST\] Replica r(\d+) broadcast ARRIVAL_CERT for (\S+)')
RE_CERT_STORED  = re.compile(r'\[CERT-STORED\] Replica (\d+) stored ARRIVAL_CERT for (\S+) \((\d+)/(\d+)\)')
RE_T8_DRAIN     = re.compile(r'\[TYPE8-DRAIN\] r(\d+) to=(-?\d+) .*? t=([0-9.]+)')
RE_T8_RECV      = re.compile(r'\[TYPE8-RECV\] r(\d+) from=(\d+) .*? t=([0-9.]+)')
RE_TIMEOUT      = re.compile(r'\[METRICS (\d+)\] StopSign_Timeout: 1')
RE_VC_TRIGGER   = re.compile(r'\[VC-TRIGGER\] r(\d+) forcing view change at ([0-9.]+).*propose_submitted=(\d)')
RE_VC_TIMEOUT   = re.compile(r'\[VC-TIMEOUT\] r(\d+) stop_sign_timeout_fired at=([0-9.]+).*propose_submitted=(\d).*stop_to_timeout_sec=([0-9.]+)')
RE_ORDER_DEC    = re.compile(r'\[METRICS (\d+)\] Order_Decided_Time: ([0-9.]+)')
RE_STOP_TIME    = re.compile(r'\[METRICS (\d+)\] Stop_Time: ([0-9.]+)')
RE_TOTAL_LAT    = re.compile(r'\[METRICS (\d+)\] Total Latency \(cleared-stop\): ([0-9.]+)')
RE_VC_TRACE     = re.compile(r'\[VC-TRACE\] r(\d+) proposeAll.*stop_to_propose_sec=([0-9.]+)')
# ResDB executor events
RE_EXEC_CALLED  = re.compile(r'\[EXECUTOR\] r(\d+) ExecuteData called')
RE_EXEC_DROP    = re.compile(r'\[EXECUTOR\] r(\d+) omnet_r=\d+ .*?drop_to_collector=(\d+).*?committed_status_has_seq=(\d+)')

def parse_log(path):
    data = {
        'cert_bcast':    [],   # (sender_id, car_id)
        'cert_stored':   [],   # (receiver_id, car_id, count, total)
        't8_drain':      [],   # (sender_id, to_id, time)
        't8_recv':       [],   # (receiver_id, sender_id, time)
        'timeouts':      set(),
        'vc_trigger':    {},   # replica_id → (time, propose_submitted)
        'vc_timeout':    {},   # replica_id → (time, propose_submitted, stop_to_timeout)
        'order_decided': {},   # replica_id → time
        'stop_time':     {},   # replica_id → time
        'total_lat':     {},   # replica_id → latency
        'propose_sub':   {},   # replica_id → 0|1  (from VC lines)
        'exec_called':   set(),  # replicas where ExecuteData was called
        'exec_dropped':  {},   # replica_id → (drop_to_collector, committed_status_has_seq)
    }

    with open(path) as f:
        for line in f:
            m = RE_CERT_BCAST.search(line)
            if m:
                data['cert_bcast'].append((_int(m.group(1)), m.group(2)))
                continue

            m = RE_CERT_STORED.search(line)
            if m:
                data['cert_stored'].append((_int(m.group(1)), m.group(2), _int(m.group(3)), _int(m.group(4))))
                continue

            m = RE_T8_DRAIN.search(line)
            if m:
                data['t8_drain'].append((_int(m.group(1)), _int(m.group(2)), _float(m.group(3))))
                continue

            m = RE_T8_RECV.search(line)
            if m:
                data['t8_recv'].append((_int(m.group(1)), _int(m.group(2)), _float(m.group(3))))
                continue

            m = RE_TIMEOUT.search(line)
            if m:
                data['timeouts'].add(_int(m.group(1)))
                continue

            m = RE_VC_TRIGGER.search(line)
            if m:
                rid, t, ps = _int(m.group(1)), _float(m.group(2)), _int(m.group(3))
                data['vc_trigger'][rid] = (t, ps)
                data['propose_sub'][rid] = ps
                continue

            m = RE_VC_TIMEOUT.search(line)
            if m:
                rid = _int(m.group(1))
                data['vc_timeout'][rid] = (_float(m.group(2)), _int(m.group(3)), _float(m.group(4)))
                data['propose_sub'][rid] = _int(m.group(3))
                continue

            m = RE_ORDER_DEC.search(line)
            if m:
                data['order_decided'][_int(m.group(1))] = _float(m.group(2))
                continue

            m = RE_STOP_TIME.search(line)
            if m:
                data['stop_time'][_int(m.group(1))] = _float(m.group(2))
                continue

            m = RE_TOTAL_LAT.search(line)
            if m:
                data['total_lat'][_int(m.group(1))] = _float(m.group(2))
                continue

            m = RE_EXEC_CALLED.search(line)
            if m:
                data['exec_called'].add(_int(m.group(1)))
                continue

            m = RE_EXEC_DROP.search(line)
            if m:
                rid = _int(m.group(1))
                data['exec_dropped'][rid] = (_int(m.group(2)), _int(m.group(3)))
                continue

    return data


def load_sinr(log_dir, replicas):
    """Load sinr_V{id}.csv for each replica from log_dir. Returns {replica_id: [(time, mean_db, min_db)]}."""
    sinr = {}
    for rid in replicas:
        path = os.path.join(log_dir, f"sinr_V{rid}.csv")
        if not os.path.exists(path):
            continue
        rows = []
        with open(path) as f:
            next(f)  # skip header
            for line in f:
                parts = line.strip().split(',')
                if len(parts) >= 4:
                    rows.append((_float(parts[0]), _float(parts[2]), _float(parts[3])))
        sinr[rid] = rows
    return sinr


# ─── analysis ───────────────────────────────────────────────────────────────

def cert_analysis(data):
    total_vehs = 0
    for _, _, _, tot in data['cert_stored']:
        if tot and tot > total_vehs:
            total_vehs = tot

    # car_ids seen in CERT-BROADCAST
    all_cars = set(car for _, car in data['cert_bcast'])
    if not all_cars:
        # fall back to cert_stored entries
        for _, car, _, _ in data['cert_stored']:
            all_cars.add(car)

    # build per-replica cert set
    stored = defaultdict(set)
    for rid, car, _, _ in data['cert_stored']:
        stored[rid].add(car)

    return stored, all_cars, total_vehs


def t8_analysis(data):
    # rx counts and unique senders per receiver
    rx_count = defaultdict(int)
    unique_senders = defaultdict(set)
    for recv, sender, _ in data['t8_recv']:
        if recv is not None and sender is not None:
            rx_count[recv] += 1
            unique_senders[recv].add(sender)
    # drain count per sender
    drain_count = defaultdict(int)
    for sender, _, _ in data['t8_drain']:
        drain_count[sender] += 1
    return rx_count, unique_senders, drain_count


def sinr_window(sinr_rows, t_start, t_end):
    rows = [(t, m, mn) for t, m, mn in sinr_rows if t_start <= t <= t_end]
    if not rows:
        return None, None, len(rows)
    means = [m for _, m, _ in rows]
    mins  = [mn for _, _, mn in rows]
    return sum(means)/len(means), min(mins), len(rows)


# ─── report ─────────────────────────────────────────────────────────────────

def run(log_path):
    log_dir = os.path.dirname(os.path.abspath(log_path))
    print(f"\nlog: {log_path}")

    data = parse_log(log_path)

    stored, all_cars, total_vehs = cert_analysis(data)
    rx_count, unique_senders, drain_count = t8_analysis(data)

    # all replica IDs seen anywhere
    all_replicas = set()
    for rid, _ in data['cert_bcast']:         all_replicas.add(rid)
    for rid, _, _, _ in data['cert_stored']:  all_replicas.add(rid)
    for rid, _, _ in data['t8_drain']:        all_replicas.add(rid)
    for rid, _, _ in data['t8_recv']:         all_replicas.add(rid)
    for rid in data['timeouts']:              all_replicas.add(rid)
    for rid in data['order_decided']:         all_replicas.add(rid)
    all_replicas = sorted(r for r in all_replicas if r is not None)

    N = len(all_replicas) if all_replicas else total_vehs
    f = (N - 1) // 3
    quorum = 2 * f + 1

    # PBFT storm window
    times = [t for _, _, t in data['t8_drain'] if t is not None]
    t8_start = min(times) if times else None
    t8_end   = max(times) if times else None

    # SINR for timed-out replicas
    sinr_data = load_sinr(log_dir, data['timeouts']) if data['timeouts'] else {}

    # ── Section 1: Cert delivery ──────────────────────────────────────────
    print(f"\n{'='*60}")
    print(f" SECTION 1 — ARRIVAL_CERT (Type 5) delivery")
    print(f"{'='*60}")
    print(f" N={N}  f={f}  need proposeAll: {total_vehs or N} certs\n")
    print(f"  {'Replica':>7}  {'Certs':>9}  {'Timeout':>7}  Missing")
    print(f"  {'-'*7}  {'-'*9}  {'-'*7}  -------")

    cert_stuck = []
    for rid in all_replicas:
        got   = len(stored[rid])
        need  = total_vehs or N
        timed = "YES *" if rid in data['timeouts'] else ""
        miss  = sorted(all_cars - stored[rid])
        miss_str = str(miss) if miss else "-"
        print(f"  {rid:>7}  {got:>4}/{need:<4}  {timed:>7}  {miss_str}")
        if got < need:
            cert_stuck.append(rid)

    # ── Section 2: proposeAll status ──────────────────────────────────────
    print(f"\n{'='*60}")
    print(f" SECTION 2 — proposeAll() submission status")
    print(f"{'='*60}\n")
    for rid in all_replicas:
        ps  = data['propose_sub'].get(rid, None)
        od  = data['order_decided'].get(rid)
        st  = data['stop_time'].get(rid)
        lat = data['total_lat'].get(rid)
        vct = data['vc_trigger'].get(rid)

        if ps is not None or rid in data['timeouts'] or od is not None:
            ps_str  = ("submitted" if ps == 1 else "NOT submitted") if ps is not None else "unknown"
            od_str  = f"YES @ t={od:.3f}s" if od else "NO"
            lat_str = f"{lat:.1f}s" if lat else "?"
            vc_str  = f"VC @ t={vct[0]:.1f}s" if vct else ""
            print(f"  Replica {rid:>2}: propose={ps_str:>13}  order_decided={od_str:<20}  latency={lat_str}  {vc_str}")

    # ── Section 3: TYPE8 PBFT delivery ───────────────────────────────────
    print(f"\n{'='*60}")
    print(f" SECTION 3 — TYPE8 (PBFT) delivery")
    print(f"{'='*60}")
    if t8_start is not None:
        print(f" Storm window: t={t8_start:.3f}s → t={t8_end:.3f}s  ({t8_end-t8_start:.3f}s duration)\n")
    print(f"  {'Replica':>7}  {'RX frames':>9}  {'Unique senders':>14}  {'Quorum':>6}  Status")
    print(f"  {'-'*7}  {'-'*9}  {'-'*14}  {'-'*6}  ------")
    for rid in all_replicas:
        rx  = rx_count[rid]
        us  = len(unique_senders[rid])
        ok  = "OK" if us >= quorum else f"SHORT by {quorum - us}"
        print(f"  {rid:>7}  {rx:>9}  {us:>14}  {quorum:>6}  {ok}")

    # ── Section 4: ResDB executor state ──────────────────────────────────
    print(f"\n{'='*60}")
    print(f" SECTION 4 — ResDB executor state")
    print(f"{'='*60}\n")
    print(f"  {'Replica':>7}  {'ExecuteData':>11}  {'drop_to_collector':>17}  {'committed_status_has_seq':>23}")
    print(f"  {'-'*7}  {'-'*11}  {'-'*17}  {'-'*23}")
    for rid in all_replicas:
        called  = "YES" if rid in data['exec_called'] else "no"
        drop    = data['exec_dropped'].get(rid)
        drop_s  = f"drop={drop[0]} status_has_seq={drop[1]}" if drop else "-"
        print(f"  {rid:>7}  {called:>11}  {drop_s}")

    # ── Section 5: Failure timeline ───────────────────────────────────────
    if data['timeouts']:
        print(f"\n{'='*60}")
        print(f" SECTION 5 — Failure timeline")
        print(f"{'='*60}\n")
        for rid in sorted(data['timeouts']):
            st  = data['stop_time'].get(rid, '?')
            vct = data['vc_trigger'].get(rid)
            vco = data['vc_timeout'].get(rid)
            lat = data['total_lat'].get(rid, '?')
            print(f"  Replica {rid}:")
            print(f"    stop_time    = {st}s")
            if vct:
                print(f"    vc_trigger   = {vct[0]}s  (propose_submitted={vct[1]})")
            if vco:
                print(f"    vc_timeout   = {vco[0]}s  (stop_to_timeout={vco[2]}s)")
            print(f"    total_latency= {lat}s")

            # SINR during PBFT storm
            if rid in sinr_data and t8_start is not None:
                mean_db, min_db, n_bins = sinr_window(sinr_data[rid], t8_start, t8_end)
                if mean_db is not None:
                    print(f"    SINR @ storm : mean={mean_db:.1f} dB  min={min_db:.1f} dB  ({n_bins} bins)")
                else:
                    print(f"    SINR @ storm : no data in window")
            elif sinr_data:
                print(f"    SINR         : no CSV found for this replica")
            print()

    # ── Section 6: Verdict ───────────────────────────────────────────────
    print(f"\n{'='*60}")
    print(f" VERDICT")
    print(f"{'='*60}\n")

    timed_list = sorted(data['timeouts'])
    if not timed_list:
        print("  No StopSign_Timeout events — all replicas decided successfully.")
        return

    print(f"  Timed-out replicas: {timed_list}")
    print()

    for rid in timed_list:
        us   = len(unique_senders[rid])
        drop = data['exec_dropped'].get(rid)
        got  = len(stored[rid]) + 1  # +1 for self-cert via CERT-STORED-SELF
        need = total_vehs or N

        print(f"  ── Replica {rid} ──")

        if rid not in data['exec_called'] and not drop and us < quorum:
            # Executor never reached — pure PBFT quorum failure
            print(f"    CAUSE: PBFT quorum failure")
            print(f"    TYPE8 received from {us} unique senders — need {quorum}.")
            print(f"    ResDB executor was never invoked (no COMMIT quorum).")
            print(f"    This is a wireless delivery problem for Type 8 (PBFT) frames.")
            print(f"    Fix: improve TYPE8 delivery to this replica.")
            print(f"         → check geometry / SINR / add retransmission or gossip for COMMIT phase")

        elif drop and drop[1] == 1:
            # Executor reached but dropped because committed_status already has seq
            print(f"    CAUSE: ResDB 'committed_status_has_seq' collision")
            print(f"    The executor received a COMMIT for this seq but dropped it")
            print(f"    because committed_status already had that sequence number,")
            print(f"    without ever calling Execute → application never gets the order.")
            print(f"    TYPE8 frames were adequate ({us} unique senders ≥ quorum {quorum}).")
            print(f"    Fix: ResDB-OMNeT bridge must guarantee Execute fires before")
            print(f"         committed_status is set, or replay committed-but-unexecuted seqs.")

        elif rid not in data['exec_called'] and us >= quorum:
            # Enough PBFT frames but executor never called — unknown reason
            print(f"    CAUSE: Unknown — {us} unique senders ≥ quorum {quorum} but executor never called.")
            print(f"    Check ResDB PBFT state machine for this replica (view mismatch? threading?)")

        else:
            print(f"    CAUSE: Unknown — check log manually.")

        if got < need:
            print(f"    NOTE: Also missing {need-got} ARRIVAL_CERTs ({got}/{need}) —")
            print(f"          correlated symptom of poor wireless geometry, not direct cause.")
        print()

    print()


if __name__ == '__main__':
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <path-to-simulation.log>")
        sys.exit(1)
    run(sys.argv[1])

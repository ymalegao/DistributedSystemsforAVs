#!/usr/bin/env python3
"""
Plot total messages sent per run for the 18-vehicle study (results_rollback_msg/).

Unlike analyze_rollback.py (which plots commit availability), this plots the
*message cost*. It is only meaningful because run_msg_matrix.sh forces a hard
--sim-time-limit so every run ends cleanly and finish() emits Messages_Sent
(the metric is finish()-only; killed runs report nothing — see run_msg_matrix.sh).

Per run: total messages = sum over replicas of the LAST cumulative Messages_Sent,
plus whether an ORDER committed (to explain dips: a failed run does less work).

Usage: python3 analyze_msg.py [results_rollback_msg_dir]
"""
import os, re, sys, glob, statistics
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
RESULTS = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "results_rollback_msg")
FIGS = os.path.join(HERE, "figures"); os.makedirs(FIGS, exist_ok=True)  # consolidated output
RE_NAME = re.compile(r"(OFF|ON)_k(\d+)_rep(\d+)\.log$")
RE_MSG  = re.compile(r"\[METRICS (\d+)\]\s+Messages_Sent:\s+(\d+)")
RE_ORD  = re.compile(r"Order_Decided_Time")


def parse(path):
    last, committed = {}, False
    with open(path, errors="ignore") as fh:
        for line in fh:
            m = RE_MSG.search(line)
            if m:
                last[int(m.group(1))] = int(m.group(2))
            if RE_ORD.search(line):
                committed = True
    return {"msgs": sum(last.values()), "committed": 1 if committed else 0,
            "emitted": len(last) > 0}


def main():
    runs = defaultdict(list)
    for p in sorted(glob.glob(os.path.join(RESULTS, "*.log"))):
        m = RE_NAME.search(os.path.basename(p))
        if m:
            runs[(m.group(1), int(m.group(2)))].append(parse(p))
    if not runs:
        print(f"No logs in {RESULTS} — run run_msg_matrix.sh first."); return
    ks = sorted({k for _, k in runs})

    def cell(lbl, k):
        # only average runs that actually emitted the metric (clean finish)
        recs = [r for r in runs.get((lbl, k), []) if r["emitted"]]
        if not recs:
            return None, 0, None
        msgs = statistics.mean(r["msgs"] for r in recs)
        commit_rate = statistics.mean(r["committed"] for r in recs)
        return msgs, len(recs), commit_rate

    print("\n=== 18-veh messages per run (clean-finish only) ===")
    print(f"{'k':>2} │ {'OFF msgs (n, commit%)':^26} │ {'ON msgs (n, commit%)':^26}")
    print("─" * 62)
    rows = []
    for k in ks:
        om, on_, oc = cell("OFF", k)
        nm, nn, nc = cell("ON", k)
        f = lambda v, p="{:.0f}": "n/a" if v is None else p.format(v)
        print(f"{k:>2} │ OFF {f(om):>6} (n={on_}, {f(oc,'{:.0%}')})   │ "
              f"ON {f(nm):>6} (n={nn}, {f(nc,'{:.0%}')})")
        rows.append(dict(k=k, off=om, on=nm, offc=oc, onc=nc))

    try:
        import matplotlib; matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except Exception as e:
        print(f"(no matplotlib: {e})"); return

    xs = [r["k"] for r in rows]
    w = 0.38
    fig, ax = plt.subplots(figsize=(7.6, 4.6))
    ob = ax.bar([x - w/2 for x in xs], [r["off"] or 0 for r in rows], w,
                label="without RSU (N=18)", color="#d1495b")
    nb = ax.bar([x + w/2 for x in xs], [r["on"] or 0 for r in rows], w,
                label="with 4 RSU (N=22)", color="#2e8b57")
    # annotate bars where consensus FAILED — that is what causes the message dip
    for r in rows:
        for val, xoff, rate in ((r["off"], -w/2, r["offc"]), (r["on"], w/2, r["onc"])):
            if val is not None and rate is not None and rate < 0.5:
                ax.annotate("consensus\nfailed", xy=(r["k"] + xoff, val),
                            xytext=(0, 4), textcoords="offset points",
                            ha="center", va="bottom", fontsize=7, color="#555")
    ax.set_xlabel("PBFT-silent vehicles (k)")
    ax.set_ylabel("total messages sent per run (at t=30s)")
    ax.set_title("18-vehicle message cost: RSU traffic holds until consensus fails\n"
                 "(uniform 30s sim cap; a dip = the run stopped committing, not efficiency)",
                 fontsize=10)
    ax.set_xticks(xs); ax.legend(); ax.grid(alpha=.3, axis="y")
    fig.tight_layout()
    out = os.path.join(FIGS, "18veh_msg_cost.png")
    fig.savefig(out, dpi=130)
    print(f"\nPlot: {out}")


if __name__ == "__main__":
    main()

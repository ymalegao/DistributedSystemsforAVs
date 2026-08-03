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
        # cost when it works: average messages over COMMITTED runs only (never blend a
        # failed run into the cost). Returns (mean, n_committed, n_total, rate, lo, hi).
        recs = [r for r in runs.get((lbl, k), []) if r["emitted"]]
        ntot = len(recs)
        rate = statistics.mean(r["committed"] for r in recs) if recs else None
        ok = [r["msgs"] for r in recs if r["committed"]]
        if not ok:
            return None, 0, ntot, rate, None, None
        return statistics.mean(ok), len(ok), ntot, rate, min(ok), max(ok)

    print("\n=== 18-veh messages per COMMITTED run ===")
    print(f"{'k':>2} │ {'OFF msgs (ncommit, rate)':^28} │ {'ON msgs (ncommit, rate)':^28}")
    print("─" * 66)
    rows = []
    for k in ks:
        om, onk, ont, oc, olo, ohi = cell("OFF", k)
        nm, nnk, nnt, nc, nlo, nhi = cell("ON", k)
        f = lambda v, p="{:.0f}": "n/a" if v is None else p.format(v)
        print(f"{k:>2} │ OFF {f(om):>6} (n={onk}/{ont}, {f(oc,'{:.0%}')})   │ "
              f"ON {f(nm):>6} (n={nnk}/{nnt}, {f(nc,'{:.0%}')})")
        rows.append(dict(k=k, off=om, on=nm, offc=oc, onc=nc,
                         off_lo=olo, off_hi=ohi, on_lo=nlo, on_hi=nhi))

    try:
        import matplotlib; matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except Exception as e:
        print(f"(no matplotlib: {e})"); return

    xs = [r["k"] for r in rows]
    w = 0.38
    fig, ax = plt.subplots(figsize=(7.6, 4.6))
    def draw(side, mkey, lokey, hikey, color, label):
        drew = False
        for r in rows:
            x = r["k"] + side*w/2
            m = r[mkey]
            if m is not None:
                yerr = [[m - r[lokey]], [r[hikey] - m]]
                ax.bar(x, m, w, color=color, label=(label if not drew else None),
                       yerr=yerr, capsize=3, ecolor="#333"); drew = True
            else:  # no committed run at this k → mark failure, no bar
                ax.plot(x, 0, marker="x", color=color, ms=9, mew=2.5)
                ax.annotate("consensus\nfailed", xy=(x, 0), xytext=(0, 6),
                            textcoords="offset points", ha="center", va="bottom",
                            fontsize=7, color=color)
    draw(-1, "off", "off_lo", "off_hi", "#d1495b", "without RSU (N=18)")
    draw(+1, "on",  "on_lo",  "on_hi",  "#2e8b57", "with 4 RSU (N=22)")
    ax.set_xlabel("PBFT-silent vehicles (k)")
    ax.set_ylabel("messages per successful run (at t=30s)")
    ax.set_title("18-vehicle message cost per SUCCESSFUL run\n"
                 "(committed runs only; failed cells marked, not averaged)", fontsize=10)
    ax.set_xticks(xs); ax.set_ylim(bottom=0); ax.legend(); ax.grid(alpha=.3, axis="y")
    fig.tight_layout()
    out = os.path.join(FIGS, "18veh_msg_cost.png")
    fig.savefig(out, dpi=130)
    print(f"\nPlot: {out}")


if __name__ == "__main__":
    main()

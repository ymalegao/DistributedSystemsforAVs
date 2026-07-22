#!/usr/bin/env python3
"""
Analyze the 18-vehicle fault-pressure ablation (results_rollback/).

Question: as vehicles go PBFT-silent, does the intersection keep reaching BFT
consensus — with vs without 4 static intersection units?

Per run we extract:
  committed  - did an epoch-0 ORDER commit at all? (Order_Decided_Time present)
  fallbacks  - how many vehicles crossed via the stop-sign TIMEOUT instead
               (i.e. BFT agreement bypassed — the safety-relevant degradation)
  quorum     - the active-view quorum the run actually used

Emits a table, report_rollback.md, and rollback_availability.png.
Usage: python3 analyze_rollback.py [results_rollback_dir]
"""
import os, re, sys, glob, statistics
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
RESULTS = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "results_rollback")

RE_NAME = re.compile(r"(OFF|ON)_k(\d+)_rep(\d+)\.log$")
RE_ORD  = re.compile(r"Order_Decided_Time")
RE_FB   = re.compile(r"StopSign_Timeout:\s+1")
RE_Q    = re.compile(r"voteN=(\d+) f=(\d+) quorum=(\d+)")


def parse(path):
    orders = fallbacks = 0
    quorum = voteN = None
    with open(path, errors="ignore") as fh:
        for line in fh:
            if RE_ORD.search(line):
                orders += 1
            if RE_FB.search(line):
                fallbacks += 1
            m = RE_Q.search(line)
            if m and quorum is None:
                voteN, quorum = int(m.group(1)), int(m.group(3))
    return {"committed": 1 if orders else 0, "orders": orders,
            "fallbacks": fallbacks, "quorum": quorum, "voteN": voteN}


def main():
    runs = defaultdict(list)
    for p in sorted(glob.glob(os.path.join(RESULTS, "*.log"))):
        m = RE_NAME.search(os.path.basename(p))
        if m:
            runs[(m.group(1), int(m.group(2)))].append(parse(p))
    if not runs:
        print(f"No logs in {RESULTS} — run run_rollback_matrix.sh first.")
        return

    ks = sorted({k for _, k in runs})

    def rate(label, k):
        recs = runs.get((label, k), [])
        return (statistics.mean(r["committed"] for r in recs) if recs else None), len(recs)

    def meanf(label, k, key):
        recs = runs.get((label, k), [])
        vals = [r[key] for r in recs if r[key] is not None]
        return statistics.mean(vals) if vals else None

    print("\n=== 18-vehicle fault pressure: consensus availability ===")
    print(f"{'k':>2} │ {'consensus committed':^25} │ {'timeout fallbacks (mean)':^26}")
    print("─" * 62)
    rows = []
    for k in ks:
        offr, offn = rate("OFF", k); onr, onn = rate("ON", k)
        offf = meanf("OFF", k, "fallbacks"); onf = meanf("ON", k, "fallbacks")
        f = lambda x, p="{:.0%}": "n/a" if x is None else p.format(x)
        print(f"{k:>2} │ OFF {f(offr):>5} (n={offn})  ON {f(onr):>5} (n={onn}) │ "
              f"OFF {f(offf,'{:.1f}'):>5}   ON {f(onf,'{:.1f}'):>5}")
        rows.append(dict(k=k, off=offr, on=onr, offf=offf, onf=onf,
                         offn=offn, onn=onn,
                         offq=meanf("OFF", k, "quorum"), onq=meanf("ON", k, "quorum")))

    md = ["# 18-vehicle fault pressure — consensus availability with/without RSU\n",
          "**Setup:** 18-vehicle late-emergency scenario. OFF = no units (N=18, active "
          "view 16, quorum 11). ON = +4 static intersection units (N=22, active view ~19-20, "
          "quorum 13). `k` = PBFT-silent vehicles (`node[1..k]`); units never go silent, and "
          "`node[16]/[17]` (late normal + ambulance) are never silenced.\n",
          "**Settings:** fast channel, 5 ms bridge poll, `sim-time-limit=25s` (the outcome is "
          "decided by ~t=20s, so runs are capped rather than run to completion).\n",
          "**Metric:** did an epoch-0 ORDER commit at all? If not, vehicles cross via the "
          "stop-sign **timeout fallback** — BFT agreement bypassed, the safety-relevant "
          "degradation this protocol exists to prevent.\n",
          "**Caveat on the fallback column:** the intended `sim-time-limit = 25s` does *not* "
          "actually apply (the generated ini sets it under `[General]`, but the scenario's own "
          "`[Config ...]` section is more specific and wins), so runs continue to t≈65-77s and "
          "some are cut off by the wall-clock timeout instead. The **committed** column is "
          "robust — the outcome is decided by t≈20s, which every run reaches. The **fallback "
          "count** is not directly comparable across cells, since it keeps accumulating for as "
          "long as a given run happened to survive.\n",
          "**Rep counts vary per k** (n shown below): the k=0/4/5/6 cells were re-run with 3 "
          "reps, while k=1/2/3 retain 2 reps from the earlier sweep of the same script and "
          "settings. Weight the cells accordingly.\n",
          "| k silent | consensus OFF (n) | consensus ON (n) | fallbacks OFF | fallbacks ON |",
          "|---|---|---|---|---|"]
    for r in rows:
        g = lambda x, p="{:.0%}": "n/a" if x is None else p.format(x)
        md.append(f"| {r['k']} | {g(r['off'])} ({r['offn']}) | {g(r['on'])} ({r['onn']}) | "
                  f"{g(r['offf'],'{:.1f}')} | {g(r['onf'],'{:.1f}')} |")
    with open(os.path.join(RESULTS, "report_rollback.md"), "w") as fh:
        fh.write("\n".join(md))
    print(f"\nReport: {os.path.join(RESULTS,'report_rollback.md')}")

    try:
        import matplotlib; matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except Exception as e:
        print(f"(no matplotlib: {e})"); return
    xs = [r["k"] for r in rows]
    fig, ax = plt.subplots(figsize=(7.2, 4.4))
    ax.plot(xs, [(r["off"] or 0)*100 for r in rows], "o-", color="#d1495b",
            label="without RSU (N=18, quorum 11)")
    ax.plot(xs, [(r["on"] or 0)*100 for r in rows], "s-", color="#2e8b57",
            label="with 4 RSU (N=22, quorum 13)")
    ax.set_xlabel("PBFT-silent vehicles (k)")
    ax.set_ylabel("runs reaching BFT consensus (%)")
    ax.set_title("18-vehicle intersection under fault pressure:\n"
                 "RSUs keep consensus alive where it otherwise collapses", fontsize=11)
    ax.set_xticks(xs); ax.set_ylim(-5, 105); ax.legend(); ax.grid(alpha=.3)
    fig.tight_layout()
    out = os.path.join(RESULTS, "rollback_availability.png")
    fig.savefig(out, dpi=130)
    print(f"Plot: {out}")


if __name__ == "__main__":
    main()

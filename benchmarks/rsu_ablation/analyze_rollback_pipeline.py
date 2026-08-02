#!/usr/bin/env python3
"""
Rollback-SPECIFIC plot from the 18-vehicle fault-pressure logs (results_rollback/).

Unlike rollback_availability.py (did *an* order commit), this traces the emergency
ROLLBACK pipeline itself and asks: under fault pressure, does the late ambulance
actually get safely re-ordered — with vs without RSU?

Pipeline stages per run (each = marker present at least once):
  ep0    epoch-0 ORDER committed      (Order_Decided_Time)  -- rollback prerequisite
  wit    late ambulance witnessed     (CANCEL-WITNESS)
  cc     cancellation BFT-committed    (CANCEL-COMMIT)       -- the rollback succeeded
  reord  new epoch re-order started    (ROLLBACK-BEGIN)

Emits:
  rollback_pipeline.png  -- 2 panels (k=0 baseline, k=6 stress): stage-reach, OFF vs ON
  rollback_success.png   -- cancel-committed rate vs k, OFF vs ON (the frontier)
  report_rollback_pipeline.md

Usage: python3 analyze_rollback_pipeline.py [results_rollback_dir]
"""
import os, re, sys, glob, statistics
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
RESULTS = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "results_rollback")
FIGS = os.path.join(HERE, "figures"); os.makedirs(FIGS, exist_ok=True)  # consolidated output
RE_NAME = re.compile(r"(OFF|ON)_k(\d+)_rep(\d+)\.log$")
STAGES = [("ep0", "Order_Decided_Time", "epoch-0\ncommitted"),
          ("wit", "CANCEL-WITNESS",     "ambulance\nwitnessed"),
          ("cc",  "CANCEL-COMMIT",      "cancel\nCOMMITTED"),
          ("reord","ROLLBACK-BEGIN",    "re-order\nstarted")]


def parse(path):
    hit = {k: False for k, _, _ in STAGES}
    with open(path, errors="ignore") as fh:
        txt = fh.read()
    for k, marker, _ in STAGES:
        hit[k] = marker in txt
    return hit


def main():
    runs = defaultdict(list)
    for p in sorted(glob.glob(os.path.join(RESULTS, "*.log"))):
        m = RE_NAME.search(os.path.basename(p))
        if m:
            runs[(m.group(1), int(m.group(2)))].append(parse(p))
    if not runs:
        print(f"No logs in {RESULTS}"); return
    ks = sorted({k for _, k in runs})

    def frac(arm, k, stage):
        recs = runs.get((arm, k), [])
        return (statistics.mean(1 if r[stage] else 0 for r in recs), len(recs)) if recs else (None, 0)

    # report
    md = ["# Emergency rollback pipeline vs fault pressure (with/without RSU)\n",
          "Fraction of runs reaching each rollback stage. ep0=epoch-0 committed "
          "(prerequisite), wit=ambulance witnessed, **cc=cancellation BFT-committed "
          "(rollback succeeded)**, reord=re-order started. Late-ambulance trigger is "
          "intermittent, so low-k `wit/cc` can dip even when ep0=100%.\n",
          "| k | arm | ep0 | wit | cc | reord | n |", "|---|---|---|---|---|---|---|"]
    for k in ks:
        for arm in ("OFF", "ON"):
            row = [frac(arm, k, s)[0] for s, _, _ in STAGES]
            n = frac(arm, k, "ep0")[1]
            if n == 0: continue
            g = lambda x: "n/a" if x is None else f"{x:.0%}"
            md.append(f"| {k} | {arm} | {g(row[0])} | {g(row[1])} | {g(row[2])} | {g(row[3])} | {n} |")
    with open(os.path.join(FIGS, "18veh_rollback_report.md"), "w") as fh:
        fh.write("\n".join(md))

    try:
        import matplotlib; matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        import numpy as np
    except Exception as e:
        print(f"(no matplotlib: {e})"); return

    # Figure 1: pipeline funnel at k=0 (baseline) and a stress k (max available)
    stress_k = max(k for k in ks if any(frac("OFF", k, "ep0")[1] for _ in [0]))
    # pick a stress k where OFF has died but ON survives, if present (prefer 6)
    stress_k = 6 if 6 in ks else ks[-1]
    panels = [(0, "no faults (k=0)"), (stress_k, f"under fault pressure (k={stress_k})")]
    fig, axes = plt.subplots(1, 2, figsize=(12, 4.8), sharey=True)
    x = np.arange(len(STAGES)); w = 0.38
    for ax, (k, title) in zip(axes, panels):
        off = [ (frac("OFF", k, s)[0] or 0)*100 for s,_,_ in STAGES ]
        on  = [ (frac("ON",  k, s)[0] or 0)*100 for s,_,_ in STAGES ]
        ax.bar(x-w/2, off, w, label="without RSU", color="#d1495b")
        ax.bar(x+w/2, on,  w, label="with 4 RSU",  color="#2e8b57")
        ax.set_xticks(x); ax.set_xticklabels([lbl for _,_,lbl in STAGES], fontsize=8)
        ax.set_title(title, fontsize=11); ax.set_ylim(0,105); ax.grid(alpha=.3, axis="y")
        ax.legend(fontsize=8)
    axes[0].set_ylabel("runs reaching stage (%)")
    fig.suptitle("Emergency rollback pipeline: RSU keeps the late-ambulance rollback "
                 "available under fault pressure", fontsize=12)
    fig.tight_layout()
    fig.savefig(os.path.join(FIGS, "18veh_rollback_pipeline.png"), dpi=130)

    # Figure 2: rollback success (cancel-committed) rate vs k
    fig, ax = plt.subplots(figsize=(7.4, 4.6))
    for arm, col, mk in (("OFF","#d1495b","o"), ("ON","#2e8b57","s")):
        ys = [ (frac(arm, k, "cc")[0]) for k in ks ]
        ax.plot(ks, [ (y*100 if y is not None else float("nan")) for y in ys ],
                mk+"-", color=col, label=("without RSU" if arm=="OFF" else "with 4 RSU"))
    ax.set_xlabel("PBFT-silent vehicles (k)")
    ax.set_ylabel("runs where cancellation BFT-committed (%)")
    ax.set_title("Rollback success under fault pressure\n"
                 "(cancel-committed = the late ambulance was safely re-ordered)", fontsize=11)
    ax.set_xticks(ks); ax.set_ylim(-5,105); ax.legend(); ax.grid(alpha=.3)
    fig.tight_layout()
    fig.savefig(os.path.join(FIGS, "18veh_rollback_success.png"), dpi=130)
    print("Report: report_rollback_pipeline.md")
    print("Plots: rollback_pipeline.png, rollback_success.png")


if __name__ == "__main__":
    main()

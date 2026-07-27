#!/usr/bin/env python3
"""
Scaling study: consensus rate + messages/run vs intersection size (4/8/12/16
vehicles), with and without 4 RSU units, at min (k=0) and max (k=frontier) fault.

Logs are named  <ARM>_v<V>_k<K>_rep<R>.log  in results_scaling/, ARM in {OFF,ON}.
Messages_Sent is finish()-only, so runs use a hard --sim-time-limit (see
run_scaling_matrix.sh); only runs that actually emitted the metric are averaged.

Emits report_scaling.md, scaling_consensus.png, scaling_msgs.png.
Usage: python3 analyze_scaling.py [results_scaling_dir]
"""
import os, re, sys, glob, statistics
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
RESULTS = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "results_scaling")
RE_NAME = re.compile(r"(OFF|ON)_v(\d+)_k(\d+)_rep(\d+)\.log$")
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
    # runs[(arm, V, k)] = [rec, ...]
    runs = defaultdict(list)
    for p in sorted(glob.glob(os.path.join(RESULTS, "*.log"))):
        m = RE_NAME.search(os.path.basename(p))
        if m:
            runs[(m.group(1), int(m.group(2)), int(m.group(3)))].append(parse(p))
    if not runs:
        print(f"No logs in {RESULTS} — run run_scaling_matrix.sh first."); return

    Vs = sorted({V for _, V, _ in runs})
    # which k is "min" and "max" for each (arm,V): min = smallest k seen, max = largest
    def ks_for(arm, V):
        return sorted({k for (a, vv, k) in runs if a == arm and vv == V})

    def rate(arm, V, k):
        recs = runs.get((arm, V, k), [])
        return (statistics.mean(r["committed"] for r in recs), len(recs)) if recs else (None, 0)

    def msgs(arm, V, k):
        recs = [r for r in runs.get((arm, V, k), []) if r["emitted"]]
        return statistics.mean(r["msgs"] for r in recs) if recs else None

    print("\n=== Scaling study: consensus rate + messages vs vehicle count ===")
    hdr = f"{'V':>3} │ {'arm':>3} │ {'k_min rate/msgs':^20} │ {'k_max rate/msgs':^22}"
    print(hdr); print("─" * len(hdr))
    table = []
    for V in Vs:
        for arm in ("OFF", "ON"):
            ks = ks_for(arm, V)
            if not ks:
                continue
            kmin, kmax = ks[0], ks[-1]
            r0, n0 = rate(arm, V, kmin); rmax, nmax = rate(arm, V, kmax)
            m0 = msgs(arm, V, kmin); mmax = msgs(arm, V, kmax)
            f = lambda x, p="{:.0%}": "n/a" if x is None else p.format(x)
            g = lambda x: "n/a" if x is None else f"{x:.0f}"
            print(f"{V:>3} │ {arm:>3} │ k{kmin}: {f(r0):>4}/{g(m0):>6}  │ "
                  f"k{kmax}: {f(rmax):>4}/{g(mmax):>6} (n={nmax})")
            table.append(dict(V=V, arm=arm, kmin=kmin, kmax=kmax,
                              r0=r0, rmax=rmax, m0=m0, mmax=mmax))

    # ── markdown ──
    md = ["# Scaling study — consensus rate & message cost vs intersection size\n",
          "4/8/12/16 vehicles, with vs without 4 RSU units, at k=0 (min) and "
          "k=frontier (max = N−quorum silent vehicles). Fast channel, 5 ms poll, "
          "hard sim-time cap so Messages_Sent (finish()-only) is emitted.\n",
          "| V | arm | k_min | rate@min | msgs@min | k_max | rate@max | msgs@max |",
          "|---|---|---|---|---|---|---|---|"]
    for t in table:
        f = lambda x, p="{:.0%}": "n/a" if x is None else p.format(x)
        g = lambda x: "n/a" if x is None else f"{x:.0f}"
        md.append(f"| {t['V']} | {t['arm']} | {t['kmin']} | {f(t['r0'])} | {g(t['m0'])} | "
                  f"{t['kmax']} | {f(t['rmax'])} | {g(t['mmax'])} |")
    with open(os.path.join(RESULTS, "report_scaling.md"), "w") as fh:
        fh.write("\n".join(md))

    try:
        import matplotlib; matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except Exception as e:
        print(f"(no matplotlib: {e})"); return

    def series(arm, which, metric):
        # which: 'min' or 'max'; metric: rate or msgs
        out = []
        for V in Vs:
            t = next((x for x in table if x["V"] == V and x["arm"] == arm), None)
            if not t:
                out.append(None); continue
            if metric == "rate":
                out.append((t["r0"] if which == "min" else t["rmax"]))
            else:
                out.append((t["m0"] if which == "min" else t["mmax"]))
        return out

    styles = {("OFF","min"):("#d1495b","o","--","without RSU, k=0"),
              ("ON","min"): ("#2e8b57","s","--","with RSU, k=0"),
              ("OFF","max"):("#d1495b","o","-","without RSU, k=max (frontier)"),
              ("ON","max"): ("#2e8b57","s","-","with RSU, k=max (frontier)")}

    # Figure 1: consensus rate
    fig, ax = plt.subplots(figsize=(7.8, 4.8))
    for (arm, which), (col, mk, ls, lab) in styles.items():
        ys = [(y*100 if y is not None else None) for y in series(arm, which, "rate")]
        ax.plot(Vs, [y if y is not None else float("nan") for y in ys],
                mk+ls, color=col, label=lab, alpha=0.9)
    ax.set_xlabel("vehicles at intersection"); ax.set_ylabel("runs reaching consensus (%)")
    ax.set_title("Consensus availability vs intersection size\n(dashed = no faults, solid = at each config's fault frontier)", fontsize=11)
    ax.set_xticks(Vs); ax.set_ylim(-5,105); ax.legend(fontsize=8); ax.grid(alpha=.3)
    fig.tight_layout(); fig.savefig(os.path.join(RESULTS,"scaling_consensus.png"), dpi=130)

    # Figure 2: messages per run
    fig, ax = plt.subplots(figsize=(7.8, 4.8))
    for (arm, which), (col, mk, ls, lab) in styles.items():
        ys = series(arm, which, "msgs")
        ax.plot(Vs, [y if y is not None else float("nan") for y in ys],
                mk+ls, color=col, label=lab, alpha=0.9)
    ax.set_xlabel("vehicles at intersection"); ax.set_ylabel("total messages sent per run")
    ax.set_title("Message cost vs intersection size\n(dashed = no faults, solid = at fault frontier)", fontsize=11)
    ax.set_xticks(Vs); ax.legend(fontsize=8); ax.grid(alpha=.3)
    fig.tight_layout(); fig.savefig(os.path.join(RESULTS,"scaling_msgs.png"), dpi=130)
    print(f"\nReport: {os.path.join(RESULTS,'report_scaling.md')}")
    print(f"Plots: scaling_consensus.png, scaling_msgs.png")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""
Analyze the with/without-RSU ablation runs produced by run_matrix.sh.

Reads results/<LABEL>_k<K>_rep<R>.log and, per run, extracts:
  committed        - did a PBFT ORDER actually commit? (Order_Decided_Time present)
  consensus_latency- propose->commit sim seconds (from PHASE_SUMMARY PROPOSE_ALL_BFT)
  msgs_sent        - total radio messages sent across replicas (finish() values)
  vehicles_crossed - how many cars physically cleared the intersection
  timeouts         - replicas that fell back via Consensus_/StopSign_Timeout

Emits: a comparison table (stdout), report.md, and (if matplotlib present)
frontier.png (commit success vs #faults) + overhead.png + latency.png.

Usage:  python3 analyze_ablation.py [results_dir]
"""
import os, re, sys, glob, statistics
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
RESULTS = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "results")

RE_NAME   = re.compile(r"(OFF|ON)_k(\d+)_rep(\d+)\.log$")
RE_ORDER  = re.compile(r"\[METRICS (\d+)\]\s+Order_Decided_Time:")
RE_BFT    = re.compile(r"PROPOSE_ALL_BFT\(sim\)=([\d.]+)s")
RE_SENT   = re.compile(r"\[METRICS (\d+)\]\s+Messages_Sent:\s+(\d+)")
RE_RECV   = re.compile(r"\[METRICS (\d+)\]\s+Messages_Received:\s+(\d+)")
RE_BYTES  = re.compile(r"\[METRICS (\d+)\]\s+Bytes_Sent:\s+(\d+)")
RE_CLEAR  = re.compile(r"Replica (\d+) cleared intersection")
RE_CTO    = re.compile(r"\[METRICS (\d+)\]\s+Consensus_Timeout:\s+1")
RE_STO    = re.compile(r"\[METRICS (\d+)\]\s+StopSign_Timeout:\s+1")


def parse_log(path):
    ordered, sent, recv, byts, cleared, cto, sto = set(), {}, {}, {}, set(), set(), set()
    lat = []
    with open(path, errors="ignore") as fh:
        for line in fh:
            m = RE_ORDER.search(line);  ordered.add(int(m.group(1)))         if m else None
            m = RE_BFT.search(line);    lat.append(float(m.group(1)))        if m else None
            m = RE_SENT.search(line);   sent.__setitem__(int(m.group(1)), int(m.group(2))) if m else None
            m = RE_RECV.search(line);   recv.__setitem__(int(m.group(1)), int(m.group(2))) if m else None
            m = RE_BYTES.search(line);  byts.__setitem__(int(m.group(1)), int(m.group(2))) if m else None
            m = RE_CLEAR.search(line);  cleared.add(int(m.group(1)))         if m else None
            m = RE_CTO.search(line);    cto.add(int(m.group(1)))             if m else None
            m = RE_STO.search(line);    sto.add(int(m.group(1)))             if m else None
    return {
        "committed": 1 if ordered else 0,
        "n_ordered": len(ordered),
        "consensus_latency": statistics.mean(lat) if lat else None,
        "msgs_sent": sum(sent.values()),
        "msgs_recv": sum(recv.values()),
        "bytes_sent": sum(byts.values()),
        "vehicles_crossed": len(cleared),
        "timeouts": len(cto | sto),
    }


def main():
    runs = defaultdict(list)   # (label,k) -> [record,...]
    for path in sorted(glob.glob(os.path.join(RESULTS, "*.log"))):
        m = RE_NAME.search(os.path.basename(path))
        if not m:
            continue
        label, k = m.group(1), int(m.group(2))
        runs[(label, k)].append(parse_log(path))
    if not runs:
        print(f"No result logs found in {RESULTS} (run run_matrix.sh first).")
        return

    def agg(label, k, key):
        recs = runs.get((label, k), [])
        vals = [r[key] for r in recs if r[key] is not None]
        return (statistics.mean(vals) if vals else None), len(recs)

    def spread(label, k, key):
        """Std-dev across reps (0 if <2 samples) — used for error bars."""
        recs = runs.get((label, k), [])
        vals = [r[key] for r in recs if r[key] is not None]
        return statistics.pstdev(vals) if len(vals) > 1 else 0.0

    ks = sorted({k for (_, k) in runs})
    # ── table ──
    hdr = f"{'k':>2} │ {'committed':^17} │ {'consensus latency (s)':^23} │ {'msgs sent (total)':^19}"
    sep = "─" * len(hdr)
    lines = [hdr, sep]
    rows = []
    for k in ks:
        offc, offn = agg("OFF", k, "committed")
        onc,  onn  = agg("ON",  k, "committed")
        offl, _ = agg("OFF", k, "consensus_latency")
        onl,  _ = agg("ON",  k, "consensus_latency")
        offm, _ = agg("OFF", k, "msgs_sent")
        onm,  _ = agg("ON",  k, "msgs_sent")
        def f(x, fmt="{:.2f}"): return "  n/a" if x is None else fmt.format(x)
        lines.append(
            f"{k:>2} │ OFF {f(offc,'{:.0%}'):>5}  ON {f(onc,'{:.0%}'):>5} │ "
            f"OFF {f(offl):>7}  ON {f(onl):>7} │ OFF {f(offm,'{:.0f}'):>6} ON {f(onm,'{:.0f}'):>6}")
        rows.append(dict(k=k, off_c=offc, on_c=onc, off_l=offl, on_l=onl,
                         off_m=offm, on_m=onm, off_n=offn, on_n=onn,
                         off_l_sd=spread("OFF", k, "consensus_latency"),
                         on_l_sd=spread("ON", k, "consensus_latency"),
                         off_m_sd=spread("OFF", k, "msgs_sent"),
                         on_m_sd=spread("ON", k, "msgs_sent")))
    table = "\n".join(lines)
    print("\n=== WITH / WITHOUT RSU — ablation summary ===\n" + table + "\n")

    # ── markdown report ──
    md = ["# With / Without RSU — ablation report\n",
          "**Setup:** 4 vehicles. OFF = no units (N=4, f=1). "
          "ON = +4 static intersection units (N=8, f=2). "
          "`k` = number of PBFT-silent replicas (omission faults on node[1..k]).\n",
          "**Simulation settings (identical across every cell, so OFF/ON are directly "
          "comparable):** fast channel (`config_fast.xml`, no obstacle shadowing) and a "
          "5 ms ResDB bridge poll/tick (`transportPollInterval`/`timeTickInterval`) "
          "instead of the 1 ms default. Commit-success is invariant to these; latency and "
          "message counts are reported *under this setting* and should not be compared "
          "against runs made at 1 ms.\n",
          "| k faults | committed OFF | committed ON | latency OFF (s) | latency ON (s) | msgs OFF | msgs ON |",
          "|---|---|---|---|---|---|---|"]
    for r in rows:
        def g(x, fmt="{:.2f}"): return "n/a" if x is None else fmt.format(x)
        md.append(f"| {r['k']} | {g(r['off_c'],'{:.0%}')} | {g(r['on_c'],'{:.0%}')} | "
                  f"{g(r['off_l'])} | {g(r['on_l'])} | {g(r['off_m'],'{:.0f}')} | {g(r['on_m'],'{:.0f}')} |")
    md += ["",
           "**Reading it:** the *frontier* is commit-success vs faults — OFF collapses once "
           "`k > 1`, ON survives to `k = 2` (the units supply the extra quorum). The *cost* is "
           "the higher `msgs sent` for ON: the price of the added fault tolerance.\n"]
    report = os.path.join(RESULTS, "report.md")
    with open(report, "w") as fh:
        fh.write("\n".join(md))
    print(f"Report written: {report}")

    # ── plots (optional) ──
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except Exception as e:
        print(f"(matplotlib unavailable: {e} — skipping plots)")
        return
    xs = [r["k"] for r in rows]
    # frontier
    fig, ax = plt.subplots(figsize=(7.2, 4.4))
    ax.plot(xs, [(r["off_c"] or 0)*100 for r in rows], "o-", label="without RSU (N=4, f=1)", color="#d1495b")
    ax.plot(xs, [(r["on_c"] or 0)*100 for r in rows], "s-", label="with RSU (N=8, f=2)", color="#2e8b57")
    ax.set_xlabel("PBFT-silent replicas (k)"); ax.set_ylabel("consensus committed (%)")
    ax.set_title("Fault-tolerance frontier:\nRSU units extend the survivable fault count", fontsize=11)
    ax.set_xticks(xs); ax.set_ylim(-5, 105); ax.legend(); ax.grid(alpha=.3)
    fig.tight_layout(); fig.savefig(os.path.join(RESULTS, "frontier.png"), dpi=130)
    # overhead
    fig, ax = plt.subplots(figsize=(6, 4))
    w = .35
    ax.bar([x-w/2 for x in xs], [r["off_m"] or 0 for r in rows], w, label="without RSU", color="#d1495b")
    ax.bar([x+w/2 for x in xs], [r["on_m"] or 0 for r in rows], w, label="with RSU", color="#2e8b57")
    ax.set_xlabel("PBFT-silent replicas (k)"); ax.set_ylabel("total messages sent")
    ax.set_title("Cost of RSU units: added consensus traffic")
    ax.set_xticks(xs); ax.legend(); ax.grid(alpha=.3, axis="y")
    fig.tight_layout(); fig.savefig(os.path.join(RESULTS, "overhead.png"), dpi=130)

    # latency — the *other* cost of units: a bigger quorum takes longer to commit.
    # Points where consensus never committed (e.g. OFF at k=2) have no latency and are
    # deliberately left as a gap + annotated, rather than plotted as zero.
    fig, ax = plt.subplots(figsize=(7.2, 4.4))
    for key, sdkey, lab, col, mark in (
            ("off_l", "off_l_sd", "without RSU (N=4, f=1)", "#d1495b", "o"),
            ("on_l",  "on_l_sd",  "with RSU (N=8, f=2)",    "#2e8b57", "s")):
        pts = [(r["k"], r[key], r[sdkey]) for r in rows if r[key] is not None]
        if pts:
            ax.errorbar([p[0] for p in pts], [p[1] for p in pts],
                        yerr=[p[2] for p in pts], fmt=mark + "-", capsize=4,
                        label=lab, color=col)
    for r in rows:   # annotate non-committing cells
        if r["off_l"] is None:
            ax.annotate("no commit\n(consensus failed)", xy=(r["k"], 0),
                        xytext=(r["k"], 0), ha="center", va="bottom",
                        fontsize=8, color="#d1495b")
    ax.set_xlabel("PBFT-silent replicas (k)")
    ax.set_ylabel("consensus latency, propose→commit (s)")
    ax.set_title("Cost of RSU units:\nconsensus latency (mean ± sd over reps)", fontsize=11)
    ax.set_xticks(xs); ax.set_ylim(bottom=0); ax.legend(); ax.grid(alpha=.3)
    fig.tight_layout(); fig.savefig(os.path.join(RESULTS, "latency.png"), dpi=130)
    print("Plots written: " + ", ".join(os.path.join(RESULTS, p) for p in
          ("frontier.png", "overhead.png", "latency.png")))


if __name__ == "__main__":
    main()

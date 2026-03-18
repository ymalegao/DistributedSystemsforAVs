"""
BFT-SMaRt V2V Intersection — multi-run averager and plotter.

Reads 16veh_0.log, 16veh_1.log, … from a collection directory
(produced by analyze_log.py --save-to <dir>), extracts per-epoch
[ROUND-METRICS] values, averages across runs with ±σ error bars,
and produces a 4-subplot figure.

Usage:
    python analyze_two_phase_v2_avg_runs.py <collection_dir> [output.png]
"""

import os
import re
import sys
import numpy as np
import matplotlib.pyplot as plt
from matplotlib.patches import Patch
from collections import defaultdict

# ── Config ────────────────────────────────────────────────────────────────────
EPOCH_N = {0: 16, 1: 12, 2: 8, 3: 4}
CARS_PER_BATCH = 4

GOLDEN_ORDER = {16: 0.5858, 12: 0.4925, 8: 0.4173, 4: 0.3879}
GOLDEN_VIEW  = {16: 0.4444, 12: 0.3800, 8: 0.3302, 4: 0.2556}

RE_ROUND_METRIC = re.compile(
    r'\[ROUND-METRICS\] Epoch (\d+) Avg_([\w]+): ([\d.]+) seconds')

METRICS = [
    "View_Consensus_Latency_4Cars",
    "Order_Consensus_Latency_4Cars",
    "Order_BFT_Request_RTT_Submitter",
    "ResetToViewEnd_4Cars",
]

# ── Parsing ───────────────────────────────────────────────────────────────────
def parse_run_log(filepath):
    """Parse [ROUND-METRICS] lines → {epoch: {metric: val}}."""
    data = {}
    with open(filepath, "r", errors="replace") as f:
        for line in f:
            m = RE_ROUND_METRIC.search(line)
            if m:
                epoch  = int(m.group(1))
                metric = m.group(2)
                val    = float(m.group(3))
                data.setdefault(epoch, {})[metric] = val
    return data


def load_collection(dir_path):
    """Load all 16veh_*.log files from dir_path → list of per-run dicts."""
    files = sorted(
        f for f in os.listdir(dir_path)
        if re.match(r'16veh_\d+\.log$', f)
    )
    if not files:
        print(f"No 16veh_*.log files found in {dir_path}")
        return []

    runs = []
    for fname in files:
        data = parse_run_log(os.path.join(dir_path, fname))
        if data:
            epochs_found = sorted(data.keys())
            runs.append(data)
            print(f"  {fname}: epochs {epochs_found}")
        else:
            print(f"  {fname}: no ROUND-METRICS found — skipped")
    return runs


def compute_stats(runs):
    """
    For each epoch and metric, compute mean, std, and raw values across runs.
    Also derives:
      Total_Consensus_Duration = View_lat + Order_lat  (pure BFT decision time)
      Throughput               = CARS_PER_BATCH / Total_Consensus_Duration
    """
    stats = {}
    for epoch in range(4):
        stats[epoch] = {}
        for m in METRICS:
            vals = [
                run[epoch][m]
                for run in runs
                if epoch in run and m in run[epoch]
            ]
            if vals:
                stats[epoch][m] = {
                    "mean": float(np.mean(vals)),
                    "std":  float(np.std(vals)),
                    "vals": vals,
                    "n":    len(vals),
                }

        # Derived metrics
        vm = stats[epoch].get("View_Consensus_Latency_4Cars", {})
        om = stats[epoch].get("Order_Consensus_Latency_4Cars", {})
        if vm and om:
            totals = [v + o for v, o in zip(vm["vals"], om["vals"])]
            stats[epoch]["Total_Consensus_Duration"] = {
                "mean": float(np.mean(totals)),
                "std":  float(np.std(totals)),
                "vals": totals,
                "n":    len(totals),
            }
            tputs = [CARS_PER_BATCH / t for t in totals if t > 0]
            stats[epoch]["Throughput"] = {
                "mean": float(np.mean(tputs)),
                "std":  float(np.std(tputs)),
                "vals": tputs,
                "n":    len(tputs),
            }
    return stats


# ── Plotting ──────────────────────────────────────────────────────────────────
EPOCH_COLORS = ["#2196F3", "#4CAF50", "#FF9800", "#F44336"]


def _bar_epoch(ax, x, means, stds, colors, n_runs, alpha=0.85):
    ax.bar(
        x, means, color=colors, alpha=alpha,
        yerr=(stds if n_runs > 1 else None),
        capsize=5, error_kw={"elinewidth": 1.5},
        edgecolor="black", linewidth=0.5,
    )


def plot_results(stats, n_runs, output_file):
    epochs   = [0, 1, 2, 3]
    x        = np.arange(len(epochs))
    n_labels = [f"N={EPOCH_N[e]}" for e in epochs]

    def mean(e, key):
        return stats[e].get(key, {}).get("mean", 0)

    def std(e, key):
        return stats[e].get(key, {}).get("std", 0)

    view_means   = [mean(e, "View_Consensus_Latency_4Cars")  for e in epochs]
    view_stds    = [std(e,  "View_Consensus_Latency_4Cars")  for e in epochs]
    order_means  = [mean(e, "Order_Consensus_Latency_4Cars") for e in epochs]
    order_stds   = [std(e,  "Order_Consensus_Latency_4Cars") for e in epochs]
    total_means  = [mean(e, "Total_Consensus_Duration")      for e in epochs]
    total_stds   = [std(e,  "Total_Consensus_Duration")      for e in epochs]
    rtt_means    = [mean(e, "Order_BFT_Request_RTT_Submitter") for e in epochs]
    rtt_stds     = [std(e,  "Order_BFT_Request_RTT_Submitter") for e in epochs]
    tput_means   = [mean(e, "Throughput")                    for e in epochs]
    tput_stds    = [std(e,  "Throughput")                    for e in epochs]

    golden_order = [GOLDEN_ORDER[EPOCH_N[e]] for e in epochs]
    golden_view  = [GOLDEN_VIEW[EPOCH_N[e]]  for e in epochs]

    run_note = f"n={n_runs} run{'s' if n_runs > 1 else ''}" + (", error bars = σ" if n_runs > 1 else "")

    fig, axes = plt.subplots(2, 2, figsize=(14, 10))
    fig.suptitle(
        f"BFT-SMaRt V2V Intersection — Consensus Latency Scaling  ({run_note})",
        fontsize=14, fontweight="bold",
    )

    # ── (0,0) ORDER latency ──────────────────────────────────────────────────
    ax = axes[0, 0]
    _bar_epoch(ax, x, order_means, order_stds, EPOCH_COLORS, n_runs)
    ax.plot(x, golden_order, "k--", marker="o", markersize=5,
            linewidth=1.5, label="Golden (isolated-N)", zorder=5)
    ax.set_title("ORDER Consensus Latency", fontweight="bold")
    ax.set_ylabel("Latency (s)")
    ax.set_xticks(x); ax.set_xticklabels(n_labels)
    ax.legend(fontsize=9); ax.grid(True, axis="y", linestyle="--", alpha=0.5)

    # ── (0,1) VIEW latency ───────────────────────────────────────────────────
    ax = axes[0, 1]
    _bar_epoch(ax, x, view_means, view_stds, EPOCH_COLORS, n_runs)
    ax.plot(x, golden_view, "k--", marker="o", markersize=5,
            linewidth=1.5, label="Golden (isolated-N)", zorder=5)
    ax.set_title("VIEW Consensus Latency", fontweight="bold")
    ax.set_ylabel("Latency (s)")
    ax.set_xticks(x); ax.set_xticklabels(n_labels)
    ax.legend(fontsize=9); ax.grid(True, axis="y", linestyle="--", alpha=0.5)

    # ── (1,0) BFT RTT + O(N) reference ──────────────────────────────────────
    ax = axes[1, 0]
    _bar_epoch(ax, x, rtt_means, rtt_stds, EPOCH_COLORS, n_runs)
    if rtt_means[0] > 0:
        scale  = rtt_means[0] / EPOCH_N[0]
        on_ref = [scale * EPOCH_N[e] for e in epochs]
        ax.plot(x, on_ref, "r--", linewidth=1.5, alpha=0.7,
                label="O(N) reference", zorder=5)
        ax.legend(fontsize=9)
    ax.set_title("BFT Request RTT (wall-clock)", fontweight="bold")
    ax.set_ylabel("RTT (s)")
    ax.set_xticks(x); ax.set_xticklabels(n_labels)
    ax.grid(True, axis="y", linestyle="--", alpha=0.5)

    # ── (1,1) Stacked View + Order (Total) ───────────────────────────────────
    ax = axes[1, 1]
    ax.bar(x, view_means,  color=EPOCH_COLORS, alpha=0.9,
           edgecolor="black", linewidth=0.5, hatch="//", label="VIEW")
    ax.bar(x, order_means, bottom=view_means, color=EPOCH_COLORS, alpha=0.5,
           edgecolor="black", linewidth=0.5, label="ORDER")
    if n_runs > 1:
        ax.errorbar(x, total_means, yerr=total_stds, fmt="none",
                    ecolor="black", capsize=5, elinewidth=1.5)
    ax.set_title(f"Total Consensus Duration (VIEW + ORDER)\n= vehicle decision wait time",
                 fontweight="bold", fontsize=10)
    ax.set_ylabel("Duration (s)")
    ax.set_xticks(x); ax.set_xticklabels(n_labels)
    legend_elements = [
        Patch(facecolor="gray", hatch="//", edgecolor="black", label="VIEW latency"),
        Patch(facecolor="gray", alpha=0.5, edgecolor="black", label="ORDER latency"),
    ]
    ax.legend(handles=legend_elements, fontsize=9)
    ax.grid(True, axis="y", linestyle="--", alpha=0.5)

    plt.tight_layout(rect=[0, 0, 1, 0.95])
    plt.savefig(output_file, dpi=300, bbox_inches="tight")
    print(f"\nPlot saved → {output_file}")


# ── Summary table ─────────────────────────────────────────────────────────────
def print_summary(stats, n_runs):
    print(f"\n{'':=<90}")
    print(f"Summary ({n_runs} run{'s' if n_runs > 1 else ''})")
    print(f"{'':─<90}")
    hdr = (f"  {'Ep':>2}  {'N':>4}  "
           f"{'VIEW mean':>10}  {'±σ':>6}  "
           f"{'ORDER mean':>11}  {'±σ':>6}  "
           f"{'Total mean':>11}  {'±σ':>6}  "
           f"{'RTT mean':>9}  {'±σ':>6}  "
           f"{'#runs':>5}")
    print(hdr)
    for e in range(4):
        n  = EPOCH_N[e]
        vm = stats[e].get("View_Consensus_Latency_4Cars", {})
        om = stats[e].get("Order_Consensus_Latency_4Cars", {})
        tm = stats[e].get("Total_Consensus_Duration", {})
        rm = stats[e].get("Order_BFT_Request_RTT_Submitter", {})
        print(
            f"  {e:>2}  {n:>4}  "
            f"{vm.get('mean',0):>9.4f}s  {vm.get('std',0):>5.4f}s  "
            f"{om.get('mean',0):>10.4f}s  {om.get('std',0):>5.4f}s  "
            f"{tm.get('mean',0):>10.4f}s  {tm.get('std',0):>5.4f}s  "
            f"{rm.get('mean',0):>8.3f}s  {rm.get('std',0):>5.3f}s  "
            f"{om.get('n', 0):>5}"
        )
    print()

    # Monotonicity check across runs
    mono_count = 0
    for run_idx in range(n_runs):
        order_vals = []
        for e in range(4):
            vals = stats[e].get("Order_Consensus_Latency_4Cars", {}).get("vals", [])
            if run_idx < len(vals):
                order_vals.append(vals[run_idx])
        if len(order_vals) == 4 and all(order_vals[i] >= order_vals[i+1] for i in range(3)):
            mono_count += 1
    if n_runs > 1:
        print(f"  Strict ORDER monotonicity: {mono_count}/{n_runs} runs ({100*mono_count/n_runs:.0f}%)")


# ── Main ──────────────────────────────────────────────────────────────────────
def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <collection_dir> [output.png]")
        print(f"  collection_dir: directory with 16veh_0.log, 16veh_1.log, ...")
        sys.exit(1)

    collection_dir = sys.argv[1]
    output_file    = (sys.argv[2] if len(sys.argv) > 2
                      else os.path.join(collection_dir, "bft_v2v_results.png"))

    print(f"Loading runs from {collection_dir} ...")
    runs = load_collection(collection_dir)
    if not runs:
        sys.exit(1)

    print(f"\nLoaded {len(runs)} run(s). Computing statistics...")
    stats = compute_stats(runs)

    print_summary(stats, len(runs))
    plot_results(stats, len(runs), output_file)


if __name__ == "__main__":
    main()

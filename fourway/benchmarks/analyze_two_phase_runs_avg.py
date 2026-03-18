import csv
import math
import os
import re
from collections import defaultdict

import matplotlib.pyplot as plt
import numpy as np


# ==========================================
# CONFIGURATION
# ==========================================
BENCHMARK_DIR = "/home/yash/omnetpp/fourway/benchmarks"
CONFIG_DIR = "2phase12Honest"
OUTPUT_CSV = "round_metrics_avg.csv"
OUTPUT_PLOT = "round_metrics_avg.png"

# Keep a stable metric order in console, CSV, and plot.
METRIC_ORDER = [
    "Avg_ResetToViewEnd_4Cars",
    "Avg_View_Consensus_Latency_4Cars",
    "Avg_Order_Consensus_Latency_4Cars",
    "Avg_Order_BFT_Request_RTT_Submitter",
]


def parse_round_metrics(log_path):
    """
    Parse one run log and return:
      epoch_metrics[epoch][metric_name] = value

    Notes:
    - The same [ROUND-METRICS] line may appear multiple times in a log.
      We keep the first value for each (epoch, metric) pair.
    """
    pattern = re.compile(
        r"\[ROUND-METRICS\]\s+Epoch\s+(\d+)\s+([A-Za-z0-9_]+):\s+([-\d.]+)\s+seconds"
    )

    epoch_metrics = defaultdict(dict)

    with open(log_path, "r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            match = pattern.search(line)
            if not match:
                continue

            epoch = int(match.group(1))
            metric_name = match.group(2)
            metric_value = float(match.group(3))

            if metric_name not in epoch_metrics[epoch]:
                epoch_metrics[epoch][metric_name] = metric_value

    return epoch_metrics


def aggregate_runs(config_path):
    """Aggregate all run*.log files under one configuration directory."""
    log_files = sorted(
        f
        for f in os.listdir(config_path)
        if f.startswith("run") and f.endswith(".log")
    )
    if not log_files:
        raise FileNotFoundError(f"No run*.log files found in {config_path}")

    metric_values = defaultdict(lambda: defaultdict(list))
    runs_loaded = 0

    for log_file in log_files:
        run_path = os.path.join(config_path, log_file)
        run_data = parse_round_metrics(run_path)
        runs_loaded += 1

        for epoch, metrics in run_data.items():
            for metric_name, value in metrics.items():
                metric_values[epoch][metric_name].append(value)

    return log_files, runs_loaded, metric_values


def summarize(metric_values):
    """
    Convert raw values into:
      summary[epoch][metric_name] = {mean, std, count}
    """
    summary = defaultdict(dict)

    for epoch in sorted(metric_values.keys()):
        for metric_name in sorted(metric_values[epoch].keys()):
            values = metric_values[epoch][metric_name]
            if not values:
                continue
            arr = np.array(values, dtype=float)
            summary[epoch][metric_name] = {
                "mean": float(np.mean(arr)),
                "std": float(np.std(arr)),
                "count": int(arr.size),
            }

    return summary


def write_csv(summary, out_path):
    rows = []
    for epoch in sorted(summary.keys()):
        metric_names = sorted(
            summary[epoch].keys(),
            key=lambda name: METRIC_ORDER.index(name)
            if name in METRIC_ORDER
            else len(METRIC_ORDER),
        )
        for metric_name in metric_names:
            stats = summary[epoch][metric_name]
            rows.append(
                {
                    "epoch": epoch,
                    "metric": metric_name,
                    "mean_seconds": stats["mean"],
                    "std_seconds": stats["std"],
                    "samples": stats["count"],
                }
            )

    with open(out_path, "w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=["epoch", "metric", "mean_seconds", "std_seconds", "samples"],
        )
        writer.writeheader()
        writer.writerows(rows)


def print_summary(summary, run_count):
    print(f"\nAveraged across {run_count} runs\n")
    for epoch in sorted(summary.keys()):
        print(f"Epoch {epoch}")
        metric_names = sorted(
            summary[epoch].keys(),
            key=lambda name: METRIC_ORDER.index(name)
            if name in METRIC_ORDER
            else len(METRIC_ORDER),
        )
        for metric_name in metric_names:
            stats = summary[epoch][metric_name]
            print(
                f"  {metric_name:<40} "
                f"mean={stats['mean']:.6f}s  std={stats['std']:.6f}s  n={stats['count']}"
            )
        print()


def make_plot(summary, out_path):
    epochs = sorted(summary.keys())
    if not epochs:
        print("No epochs to plot.")
        return

    metric_names = []
    for name in METRIC_ORDER:
        if any(name in summary[e] for e in epochs):
            metric_names.append(name)
    for e in epochs:
        for name in summary[e].keys():
            if name not in metric_names:
                metric_names.append(name)

    n_metrics = len(metric_names)
    cols = 2
    rows = int(math.ceil(n_metrics / cols))
    fig, axes = plt.subplots(rows, cols, figsize=(14, 4 * rows))

    if isinstance(axes, np.ndarray):
        flat_axes = axes.flatten()
    else:
        flat_axes = [axes]

    for i, metric_name in enumerate(metric_names):
        ax = flat_axes[i]
        means = []
        stds = []
        counts = []
        for epoch in epochs:
            stats = summary[epoch].get(metric_name)
            if stats:
                means.append(stats["mean"])
                stds.append(stats["std"])
                counts.append(str(stats["count"]))
            else:
                means.append(np.nan)
                stds.append(0.0)
                counts.append("0")

        x = np.arange(len(epochs))
        ax.errorbar(x, means, yerr=stds, marker="o", capsize=4, linewidth=1.8)
        ax.set_xticks(x)
        ax.set_xticklabels([f"Epoch {e}" for e in epochs])
        ax.set_title(metric_name)
        ax.set_ylabel("Seconds")
        ax.grid(True, axis="y", linestyle="--", alpha=0.5)

        for xi, yi, n in zip(x, means, counts):
            if not np.isnan(yi):
                ax.annotate(f"n={n}", (xi, yi), textcoords="offset points", xytext=(0, 8), ha="center")

    for j in range(n_metrics, len(flat_axes)):
        fig.delaxes(flat_axes[j])

    fig.suptitle("2-Phase Round Metrics: Mean +/- Std across Runs", fontsize=14, fontweight="bold")
    plt.tight_layout(rect=[0, 0, 1, 0.96])
    plt.savefig(out_path, dpi=300)
    plt.close(fig)


def main():
    config_path = os.path.join(BENCHMARK_DIR, CONFIG_DIR)
    if not os.path.isdir(config_path):
        raise FileNotFoundError(f"Configuration directory not found: {config_path}")

    print(f"Processing configuration: {config_path}")
    log_files, run_count, metric_values = aggregate_runs(config_path)
    print(f"Loaded {len(log_files)} run files: {', '.join(log_files)}")

    summary = summarize(metric_values)
    if not summary:
        print("No [ROUND-METRICS] entries found.")
        return

    print_summary(summary, run_count)

    csv_path = os.path.join(config_path, OUTPUT_CSV)
    write_csv(summary, csv_path)
    print(f"CSV written: {csv_path}")

    plot_path = os.path.join(config_path, OUTPUT_PLOT)
    make_plot(summary, plot_path)
    print(f"Plot written: {plot_path}")


if __name__ == "__main__":
    main()

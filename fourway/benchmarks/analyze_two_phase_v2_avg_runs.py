"""
BFT-SMaRt V2V Intersection — Corrected multi-config plotter.
X-Axis = Starting Configuration (Scenario)
Bars = Epochs (Batch 1, Batch 2, etc.)
"""

import os
import re
import sys
import numpy as np
import matplotlib.pyplot as plt

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_DIRS = [
    os.path.join(SCRIPT_DIR, "2phase4Honest"),
    os.path.join(SCRIPT_DIR, "2phase8Honest"),
    os.path.join(SCRIPT_DIR, "2phase12Honest"),
    os.path.join(SCRIPT_DIR, "2phase16Honest"),
]

RE_ROUND_METRIC = re.compile(r'\[ROUND-METRICS\] Epoch (\d+) Avg_([\w]+): ([\d.]+) seconds')

METRICS = [
    "View_Consensus_Latency_4Cars",
    "Order_Consensus_Latency_4Cars",
    "Order_BFT_Request_RTT_Submitter",
    "Total_Consensus_Duration",
    "Messages_Sent_PerReplica",
    "Messages_Received_PerReplica",
    "StopSign_Failures",
]

# Colors for Epochs (Batch 1, Batch 2, Batch 3, Batch 4)
EPOCH_COLORS = ["#2196F3", "#4CAF50", "#FF9800", "#F44336"]
MAX_EPOCHS = 4

def infer_starting_n(dir_path):
    for f in os.listdir(dir_path):
        m = re.match(r'^(\d+)veh_\d+\.log$', f)
        if m: return int(m.group(1))
    return None

def parse_run_log(filepath):
    data = {}
    with open(filepath, "r", errors="replace") as f:
        for line in f:
            m = RE_ROUND_METRIC.search(line)
            if m:
                epoch = int(m.group(1))
                metric = m.group(2)
                val = float(m.group(3))
                data.setdefault(epoch, {})[metric] = val
    return data

def load_collection(dir_path, starting_n):
    pat = re.compile(rf'^{starting_n}veh_\d+\.log$')
    files = sorted(f for f in os.listdir(dir_path) if pat.match(f))
    runs = []
    for fname in files:
        epoch_data = parse_run_log(os.path.join(dir_path, fname))
        if epoch_data: runs.append(epoch_data)
    return runs

def compute_stats(runs):
    stats = {}
    all_epochs = sorted({ep for run in runs for ep in run})
    for ep in all_epochs:
        stats[ep] = {}
        for metric in METRICS:
            vals = [run[ep][metric] for run in runs if ep in run and metric in run[ep]]
            if vals:
                stats[ep][metric] = {
                    "mean": float(np.mean(vals)),
                    "std":  float(np.std(vals)),
                    "n":    len(vals),
                }
    return stats

def _get(stats, ep, metric, field):
    return stats.get(ep, {}).get(metric, {}).get(field, 0)

def plot_results(all_stats, output_file):
    # all_stats is sorted by starting_n
    n_configs = len(all_stats)
    config_labels = [f"{sn} Cars" for _, sn, _ in all_stats]
    x = np.arange(n_configs)
    
    bar_w = 0.2
    offsets = np.array([-1.5, -0.5, 0.5, 1.5]) * bar_w

    def grouped_bars(ax, metric, multiply=1.0):
        for epoch in range(MAX_EPOCHS):
            means = []
            stds = []
            for _, _, sd in all_stats:
                if epoch in sd:
                    means.append(_get(sd, epoch, metric, "mean") * multiply)
                    stds.append(_get(sd, epoch, metric, "std") * multiply)
                else:
                    means.append(0)
                    stds.append(0)
            
            # Only plot if this epoch has data in at least one config
            if any(m > 0 for m in means):
                ax.bar(
                    x + offsets[epoch], means, bar_w,
                    color=EPOCH_COLORS[epoch], alpha=0.85,
                    yerr=stds, capsize=4, error_kw={"elinewidth": 1.3},
                    edgecolor="black", linewidth=0.4,
                    label=f"Batch {epoch+1}"
                )
        
        ax.set_xticks(x)
        ax.set_xticklabels(config_labels, fontweight='bold')
        ax.legend(fontsize=9)
        ax.grid(True, axis="y", linestyle="--", alpha=0.4)

    fig, axes = plt.subplots(2, 3, figsize=(20, 12))
    fig.suptitle("BFT-SMaRt V2V — Consensus Latency Scaling (Grouped by Scenario)", fontsize=16, fontweight="bold")

    grouped_bars(axes[0, 0], "Order_Consensus_Latency_4Cars", multiply=1000)
    axes[0, 0].set_title("ORDER Consensus Latency", fontweight="bold")
    axes[0, 0].set_ylabel("Latency (ms)")

    grouped_bars(axes[0, 1], "View_Consensus_Latency_4Cars", multiply=1000)
    axes[0, 1].set_title("VIEW Consensus Latency", fontweight="bold")
    axes[0, 1].set_ylabel("Latency (ms)")

    grouped_bars(axes[0, 2], "Order_BFT_Request_RTT_Submitter")
    axes[0, 2].set_title("BFT Request RTT (wall-clock)", fontweight="bold")
    axes[0, 2].set_ylabel("RTT (s)")

    grouped_bars(axes[1, 0], "Total_Consensus_Duration")
    axes[1, 0].set_title("Total Consensus Duration\n(Physical Wait Time)", fontweight="bold")
    axes[1, 0].set_ylabel("Duration (s)")

    # Messages
    ax = axes[1, 1]
    half = bar_w * 0.45
    for epoch in range(MAX_EPOCHS):
        s_m, s_s, r_m, r_s = [], [], [], []
        for _, _, sd in all_stats:
            s_m.append(_get(sd, epoch, "Messages_Sent_PerReplica", "mean"))
            s_s.append(_get(sd, epoch, "Messages_Sent_PerReplica", "std"))
            r_m.append(_get(sd, epoch, "Messages_Received_PerReplica", "mean"))
            r_s.append(_get(sd, epoch, "Messages_Received_PerReplica", "std"))
            
        if any(m > 0 for m in s_m):
            bx = x + offsets[epoch]
            ax.bar(bx - half/2, s_m, half, color=EPOCH_COLORS[epoch], alpha=0.85,
                   yerr=s_s, capsize=3, edgecolor="black", linewidth=0.4, label=f"B{epoch+1} sent")
            ax.bar(bx + half/2, r_m, half, color=EPOCH_COLORS[epoch], alpha=0.40,
                   yerr=r_s, capsize=3, edgecolor="black", linewidth=0.4, hatch="//", label=f"B{epoch+1} recv")
            
    ax.set_xticks(x); ax.set_xticklabels(config_labels, fontweight='bold')
    ax.set_ylabel("Message count")
    ax.set_title("Messages per Replica (avg)", fontweight="bold")
    ax.legend(fontsize=7, ncol=2)
    ax.grid(True, axis="y", linestyle="--", alpha=0.4)

    grouped_bars(axes[1, 2], "StopSign_Failures")
    axes[1, 2].set_title("Stop-Sign Failures per Round", fontweight="bold")
    axes[1, 2].set_ylabel("Failure count (avg)")
    axes[1, 2].set_ylim(0, 4.5)

    plt.tight_layout(rect=[0, 0, 1, 0.95])
    plt.savefig(output_file, dpi=300, bbox_inches="tight")
    print(f"\nPlot saved → {output_file}")

def main():
    args = sys.argv[1:]
    output_file = None
    dir_paths = []
    i = 0
    while i < len(args):
        if args[i] == "--output" and i + 1 < len(args):
            output_file = args[i + 1]
            i += 2
        else:
            dir_paths.append(args[i])
            i += 1

    if not dir_paths:
        dir_paths = [d for d in DEFAULT_DIRS if os.path.isdir(d)]

    if output_file is None:
        output_file = os.path.join(os.path.dirname(os.path.abspath(dir_paths[0])), "bft_v2v_multiconfig.png")

    all_stats = []
    for dir_path in dir_paths:
        starting_n = infer_starting_n(dir_path)
        if starting_n is None: continue
        label = os.path.basename(dir_path.rstrip("/"))
        runs = load_collection(dir_path, starting_n)
        if runs:
            stats = compute_stats(runs)
            all_stats.append((label, starting_n, stats))

    all_stats.sort(key=lambda t: t[1])
    plot_results(all_stats, output_file)

if __name__ == "__main__":
    main()
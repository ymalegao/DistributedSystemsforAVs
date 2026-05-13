#!/usr/bin/env python3
"""
Channel utilization static plots for benchmark layouts (partner plot_heatmap style).

  <benchmark_root>/<scenario>/run_*/channel_V*.csv

Outputs (default under --root):
  channel_utilization_heatmap.png   — one panel per scenario (time × vehicle, run-averaged)
  channel_utilization_timeseries.png — mean utilization over all vehicles vs time per scenario

Usage:
  python3 benchmarks/plot_channel_utilization_heatmap.py
  python3 benchmarks/plot_channel_utilization_heatmap.py --root benchmarks/Priority8cars
"""

from __future__ import annotations

import argparse
import math
import os
import sys

import matplotlib.pyplot as plt
import numpy as np

from benchmark_metrics_io import (
    TIME_GRID_STEP,
    discover_scenarios,
    load_channel_scenario_averaged_union_vids,
)

UTILIZATION_CONGESTION_THRESHOLD = 0.30
CMAP = "YlOrRd"


def _subplot_grid(n: int) -> tuple[int, int]:
    if n <= 0:
        return 1, 1
    ncols = min(6, max(1, n))
    nrows = int(math.ceil(n / ncols))
    return nrows, ncols


def plot_heatmap_grid(
    benchmark_root: str,
    scenario_names: list[str],
    out_path: str,
) -> bool:
    cache: dict[str, tuple[np.ndarray, list[int], np.ndarray]] = {}
    vmax = 0.0
    for name in scenario_names:
        scenario_dir = os.path.join(benchmark_root, name)
        got = load_channel_scenario_averaged_union_vids(scenario_dir, None)
        if got is None:
            continue
        tg, vids, mat = got
        cache[name] = (np.array(tg), vids, mat)
        if mat.size:
            v = float(np.nanmax(mat))
            if v > vmax:
                vmax = v
    vmax = max(vmax, UTILIZATION_CONGESTION_THRESHOLD)
    if not cache:
        print("No channel_V*.csv data — skipping heatmap.")
        return False

    n = len(cache)
    nrows, ncols = _subplot_grid(n)
    fig, axes = plt.subplots(nrows, ncols, figsize=(3.2 * ncols, 3.0 * nrows), squeeze=False)
    bench = os.path.basename(os.path.normpath(benchmark_root))
    fig.suptitle(
        f"Channel utilization — {bench}\n(run-averaged; common colour scale)",
        fontsize=12,
        fontweight="bold",
    )

    ordered = [s for s in scenario_names if s in cache]
    im_ref = None
    for idx, name in enumerate(ordered):
        r, c = divmod(idx, ncols)
        ax = axes[r][c]
        tg, vids, mat = cache[name]
        time_edges = np.append(tg, tg[-1] + TIME_GRID_STEP)
        y_edges = np.arange(len(vids) + 1)
        im = ax.pcolormesh(time_edges, y_edges, mat, cmap=CMAP, vmin=0.0, vmax=vmax, shading="flat")
        im_ref = im
        ax.set_yticks(np.arange(len(vids)) + 0.5)
        ax.set_yticklabels([f"V{v}" for v in vids], fontsize=6)
        mean_u = float(np.nanmean(mat))
        ax.set_title(name.replace("_", " "), fontsize=9, fontweight="bold")
        ax.text(
            0.99,
            0.97,
            f"mean={mean_u:.3f}",
            transform=ax.transAxes,
            ha="right",
            va="top",
            fontsize=7,
            bbox=dict(boxstyle="round,pad=0.2", fc="white", alpha=0.75),
        )
        ax.set_xlabel("Time (s)", fontsize=8)
        if c == 0:
            ax.set_ylabel("Vehicle", fontsize=8)

    for j in range(len(ordered), nrows * ncols):
        r, c = divmod(j, ncols)
        axes[r][c].set_visible(False)

    if im_ref is not None:
        fig.subplots_adjust(right=0.88)
        cbar_ax = fig.add_axes([0.90, 0.12, 0.02, 0.76])
        cbar = fig.colorbar(im_ref, cax=cbar_ax)
        cbar.set_label("Channel utilization", fontsize=10)
        cbar.ax.axhline(UTILIZATION_CONGESTION_THRESHOLD, color="black", linewidth=1.0, linestyle="--")

    fig.savefig(out_path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"  Saved: {out_path}")
    return True


def plot_timeseries(
    benchmark_root: str,
    scenario_names: list[str],
    out_path: str,
) -> bool:
    series: list[tuple[str, np.ndarray, np.ndarray]] = []
    for name in scenario_names:
        scenario_dir = os.path.join(benchmark_root, name)
        got = load_channel_scenario_averaged_union_vids(scenario_dir, None)
        if got is None:
            continue
        tg, _vids, mat = got
        mean_v = np.nanmean(mat, axis=0)
        if np.all(np.isnan(mean_v)):
            continue
        series.append((name, np.array(tg), mean_v))

    if not series:
        print("No channel data — skipping timeseries.")
        return False

    fig, ax = plt.subplots(figsize=(12, 5))
    cmap = plt.cm.tab10(np.linspace(0, 1, min(10, max(1, len(series)))))
    ymax = UTILIZATION_CONGESTION_THRESHOLD * 1.5
    for i, (name, tg, y) in enumerate(series):
        color = cmap[i % len(cmap)]
        ax.plot(tg, y, color=color, linewidth=1.4, label=name, alpha=0.9)
        peak = float(np.nanmax(y))
        if peak > ymax:
            ymax = peak * 1.1

    ax.axhline(
        UTILIZATION_CONGESTION_THRESHOLD,
        color="red",
        linewidth=1.0,
        linestyle="--",
        alpha=0.6,
        label=f"Threshold ({UTILIZATION_CONGESTION_THRESHOLD:.0%})",
    )
    bench = os.path.basename(os.path.normpath(benchmark_root))
    ax.set_title(
        f"Mean channel utilization (fleet) — {bench}\n(run-averaged, mean over vehicles)",
        fontsize=11,
        fontweight="bold",
    )
    ax.set_xlabel("Simulation time (s)", fontsize=10)
    ax.set_ylabel("Mean utilization", fontsize=10)
    ax.set_ylim(0, ymax)
    ax.grid(True, alpha=0.3)
    ax.legend(loc="upper right", fontsize=8, ncol=2)
    fig.tight_layout()
    fig.savefig(out_path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"  Saved: {out_path}")
    return True


def main() -> int:
    here = os.path.dirname(os.path.abspath(__file__))
    default_root = os.path.join(here, "Priority4cars")

    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=default_root, help="Benchmark folder (scenario subfolders)")
    ap.add_argument(
        "--output-dir",
        default="",
        help="Where to write PNGs (default: same as --root)",
    )
    ap.add_argument("--scenarios", default="", help="Comma-separated scenario names (default: all with CSVs)")
    args = ap.parse_args()

    benchmark_root = os.path.abspath(args.root)
    if not os.path.isdir(benchmark_root):
        print(f"ERROR: not a directory: {benchmark_root}", file=sys.stderr)
        return 1

    out_dir = os.path.abspath(args.output_dir) if args.output_dir.strip() else benchmark_root
    os.makedirs(out_dir, exist_ok=True)

    discovered = discover_scenarios(benchmark_root, "channel_V*.csv")
    if args.scenarios.strip():
        wanted = {x.strip() for x in args.scenarios.split(",") if x.strip()}
        scenario_names = [n for n in sorted(wanted) if n in discovered]
    else:
        scenario_names = discovered

    if not scenario_names:
        print(f"No scenarios with channel_V*.csv under {benchmark_root}")
        return 1

    h_ok = plot_heatmap_grid(
        benchmark_root,
        scenario_names,
        os.path.join(out_dir, "channel_utilization_heatmap.png"),
    )
    t_ok = plot_timeseries(
        benchmark_root,
        scenario_names,
        os.path.join(out_dir, "channel_utilization_timeseries.png"),
    )
    return 0 if (h_ok or t_ok) else 1


if __name__ == "__main__":
    sys.exit(main())

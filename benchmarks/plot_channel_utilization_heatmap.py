#!/usr/bin/env python3
"""
Channel utilization static plots for benchmark layouts (partner plot_heatmap style).

  <benchmark_root>/<scenario>/run_*/channel_V*.csv

Default outputs under benchmarks/:
  channel_utilization_heatmap.png   — scenario × fleet-size panels
  channel_utilization_timeseries.png — one panel per fleet size, scenario lines

Usage:
  python3 benchmarks/plot_channel_utilization_heatmap.py
  python3 benchmarks/plot_channel_utilization_heatmap.py --root benchmarks/Priority8cars
  python3 benchmarks/plot_channel_utilization_heatmap.py --roots benchmarks/Priority4cars,benchmarks/Priority8cars
"""

from __future__ import annotations

import argparse
import math
import os
import sys
import warnings

import matplotlib.pyplot as plt
import numpy as np

from benchmark_metrics_io import (
    TIME_GRID_STEP,
    discover_scenarios,
    load_channel_scenario_averaged_union_vids,
)

UTILIZATION_CONGESTION_THRESHOLD = 0.30
CMAP = "YlOrRd"
DEFAULT_PRIORITY_ROOTS = ("Priority4cars", "Priority8cars", "Priority12cars", "Priority16cars")


def _load_channel_scenario_averaged_union_vids(
    scenario_dir: str,
    time_grid_ref: list[float] | None,
) -> tuple[list[float], list[int], np.ndarray] | None:
    # Some run/time bins are empty after unioning time grids across repeated runs.
    with warnings.catch_warnings():
        warnings.filterwarnings("ignore", message="Mean of empty slice", category=RuntimeWarning)
        return load_channel_scenario_averaged_union_vids(scenario_dir, time_grid_ref)


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
        got = _load_channel_scenario_averaged_union_vids(scenario_dir, None)
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
        got = _load_channel_scenario_averaged_union_vids(scenario_dir, None)
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


def plot_collective_heatmap_grid(
    benchmark_roots: list[str],
    scenario_names: list[str],
    out_path: str,
) -> bool:
    cache: dict[tuple[str, str], tuple[np.ndarray, list[int], np.ndarray]] = {}
    vmax = 0.0
    for root in benchmark_roots:
        for name in scenario_names:
            got = _load_channel_scenario_averaged_union_vids(os.path.join(root, name), None)
            if got is None:
                continue
            tg, vids, mat = got
            cache[(root, name)] = (np.array(tg), vids, mat)
            if mat.size:
                v = float(np.nanmax(mat))
                if v > vmax:
                    vmax = v
    vmax = max(vmax, UTILIZATION_CONGESTION_THRESHOLD)
    if not cache:
        print("No channel_V*.csv data — skipping collective heatmap.")
        return False

    available_scenarios = [
        s for s in scenario_names if any((root, s) in cache for root in benchmark_roots)
    ]
    nrows = len(available_scenarios)
    ncols = len(benchmark_roots)
    fig, axes = plt.subplots(
        nrows,
        ncols,
        figsize=(3.2 * ncols, 2.6 * nrows),
        squeeze=False,
    )
    fig.suptitle(
        "Channel utilization by fleet size and scenario\n(run-averaged; common colour scale)",
        fontsize=12,
        fontweight="bold",
    )

    im_ref = None
    for r, scenario in enumerate(available_scenarios):
        for c, root in enumerate(benchmark_roots):
            ax = axes[r][c]
            got = cache.get((root, scenario))
            if got is None:
                ax.set_visible(False)
                continue
            tg, vids, mat = got
            time_edges = np.append(tg, tg[-1] + TIME_GRID_STEP)
            y_edges = np.arange(len(vids) + 1)
            im = ax.pcolormesh(
                time_edges,
                y_edges,
                mat,
                cmap=CMAP,
                vmin=0.0,
                vmax=vmax,
                shading="flat",
            )
            im_ref = im
            if r == 0:
                ax.set_title(os.path.basename(os.path.normpath(root)), fontsize=9, fontweight="bold")
            if c == 0:
                ax.set_ylabel(f"{scenario.replace('_', ' ')}\nVehicle", fontsize=8)
            ax.set_xlabel("Time (s)", fontsize=7)
            ax.set_yticks(np.arange(len(vids)) + 0.5)
            if c == 0:
                ax.set_yticklabels([f"V{v}" for v in vids], fontsize=5)
            else:
                ax.set_yticklabels([])
            mean_u = float(np.nanmean(mat))
            ax.text(
                0.98,
                0.96,
                f"mean={mean_u:.3f}",
                transform=ax.transAxes,
                ha="right",
                va="top",
                fontsize=6,
                bbox=dict(boxstyle="round,pad=0.2", fc="white", alpha=0.75),
            )

    if im_ref is not None:
        fig.subplots_adjust(right=0.90)
        cbar_ax = fig.add_axes([0.92, 0.12, 0.015, 0.76])
        cbar = fig.colorbar(im_ref, cax=cbar_ax)
        cbar.set_label("Channel utilization", fontsize=10)
        cbar.ax.axhline(UTILIZATION_CONGESTION_THRESHOLD, color="black", linewidth=1.0, linestyle="--")

    fig.savefig(out_path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"  Saved: {out_path}")
    return True


def plot_collective_timeseries(
    benchmark_roots: list[str],
    scenario_names: list[str],
    out_path: str,
) -> bool:
    nrows, ncols = _subplot_grid(len(benchmark_roots))
    fig, axes = plt.subplots(nrows, ncols, figsize=(6.2 * ncols, 3.6 * nrows), squeeze=False)
    fig.suptitle(
        "Mean channel utilization by fleet size\n(run-averaged, mean over vehicles)",
        fontsize=12,
        fontweight="bold",
    )
    any_series = False
    cmap = plt.cm.tab10(np.linspace(0, 1, min(10, max(1, len(scenario_names)))))
    for idx, root in enumerate(benchmark_roots):
        r, c = divmod(idx, ncols)
        ax = axes[r][c]
        ymax = UTILIZATION_CONGESTION_THRESHOLD * 1.5
        for i, name in enumerate(scenario_names):
            got = _load_channel_scenario_averaged_union_vids(os.path.join(root, name), None)
            if got is None:
                continue
            tg, _vids, mat = got
            mean_v = np.nanmean(mat, axis=0)
            if np.all(np.isnan(mean_v)):
                continue
            any_series = True
            ax.plot(tg, mean_v, color=cmap[i % len(cmap)], linewidth=1.2, label=name, alpha=0.9)
            peak = float(np.nanmax(mean_v))
            if peak > ymax:
                ymax = peak * 1.1
        ax.axhline(
            UTILIZATION_CONGESTION_THRESHOLD,
            color="red",
            linewidth=1.0,
            linestyle="--",
            alpha=0.6,
        )
        ax.set_title(os.path.basename(os.path.normpath(root)), fontsize=10, fontweight="bold")
        ax.set_xlabel("Simulation time (s)", fontsize=8)
        ax.set_ylabel("Mean utilization", fontsize=8)
        ax.set_ylim(0, ymax)
        ax.grid(True, alpha=0.3)
        ax.legend(fontsize=7, ncol=2)

    for idx in range(len(benchmark_roots), nrows * ncols):
        r, c = divmod(idx, ncols)
        axes[r][c].set_visible(False)

    if not any_series:
        plt.close(fig)
        print("No channel data — skipping collective timeseries.")
        return False
    fig.tight_layout()
    fig.savefig(out_path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"  Saved: {out_path}")
    return True


def main() -> int:
    here = os.path.dirname(os.path.abspath(__file__))

    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default="", help="Single benchmark folder (legacy mode)")
    ap.add_argument(
        "--roots",
        default="",
        help="Comma-separated benchmark roots (default: Priority4/8/12/16 next to this script)",
    )
    ap.add_argument(
        "--output-dir",
        default="",
        help="Where to write PNGs (default: same as --root)",
    )
    ap.add_argument("--scenarios", default="", help="Comma-separated scenario names (default: all with CSVs)")
    args = ap.parse_args()

    if args.roots.strip():
        benchmark_roots = [
            os.path.abspath(p.strip()) for p in args.roots.split(",") if p.strip()
        ]
    elif args.root.strip():
        benchmark_roots = [os.path.abspath(args.root.strip())]
    else:
        benchmark_roots = [
            os.path.join(here, name)
            for name in DEFAULT_PRIORITY_ROOTS
            if os.path.isdir(os.path.join(here, name))
        ]

    missing_roots = [root for root in benchmark_roots if not os.path.isdir(root)]
    if missing_roots:
        for root in missing_roots:
            print(f"ERROR: not a directory: {root}", file=sys.stderr)
        return 1
    if not benchmark_roots:
        print("ERROR: no benchmark roots found.", file=sys.stderr)
        return 1

    out_dir = os.path.abspath(args.output_dir) if args.output_dir.strip() else (
        benchmark_roots[0] if args.root.strip() and not args.roots.strip() else here
    )
    os.makedirs(out_dir, exist_ok=True)

    discovered_by_root = {
        root: discover_scenarios(root, "channel_V*.csv") for root in benchmark_roots
    }
    discovered = sorted({name for names in discovered_by_root.values() for name in names})
    if args.scenarios.strip():
        wanted = {x.strip() for x in args.scenarios.split(",") if x.strip()}
        scenario_names = [n for n in sorted(wanted) if n in discovered]
    else:
        scenario_names = discovered

    if not scenario_names:
        print("No scenarios with channel_V*.csv under requested benchmark roots")
        return 1

    if len(benchmark_roots) == 1 and args.root.strip() and not args.roots.strip():
        benchmark_root = benchmark_roots[0]
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
    else:
        h_ok = plot_collective_heatmap_grid(
            benchmark_roots,
            scenario_names,
            os.path.join(out_dir, "channel_utilization_heatmap.png"),
        )
        t_ok = plot_collective_timeseries(
            benchmark_roots,
            scenario_names,
            os.path.join(out_dir, "channel_utilization_timeseries.png"),
        )
    return 0 if (h_ok or t_ok) else 1


if __name__ == "__main__":
    sys.exit(main())

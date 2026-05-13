#!/usr/bin/env python3
"""
SINR plots for benchmark layouts (partner plot_sinr style).

  <benchmark_root>/<scenario>/run_*/sinr_V*.csv

CSV columns expected: time_s, sinr_db_mean (and optionally sinr_db_min) — ChannelMetrics output.

Default outputs under --root:
  sinr_heatmap.png
  sinr_timeseries.png
  sinr_animation.gif

Usage:
  python3 benchmarks/plot_sinr.py
  python3 benchmarks/plot_sinr.py --root benchmarks/Priority16cars --no-animation
"""

from __future__ import annotations

import argparse
import math
import os
import sys

import matplotlib.animation as animation
import matplotlib.pyplot as plt
import numpy as np

from benchmark_metrics_io import (
    TIME_GRID_STEP,
    discover_scenarios,
    infer_n_rows_sinr,
    load_sinr_scenario_averaged_padded,
    load_sinr_scenario_averaged_union_vids,
)

CMAP = "plasma"
HOLD_FRAMES = 8
FPS = 4


def _subplot_grid(n: int) -> tuple[int, int]:
    if n <= 0:
        return 1, 1
    ncols = min(6, max(1, n))
    nrows = int(math.ceil(n / ncols))
    return nrows, ncols


def plot_heatmap(
    benchmark_root: str,
    scenario_names: list[str],
    out_path: str,
) -> bool:
    cache: dict[str, tuple[np.ndarray, list[int], np.ndarray]] = {}
    vmin, vmax = float("inf"), float("-inf")
    for name in scenario_names:
        scenario_dir = os.path.join(benchmark_root, name)
        got = load_sinr_scenario_averaged_union_vids(scenario_dir, None)
        if got is None:
            continue
        tg, vids, mat = got
        cache[name] = (np.array(tg), vids, mat)
        valid = mat[~np.isnan(mat)]
        if valid.size:
            vmin = min(vmin, float(valid.min()))
            vmax = max(vmax, float(valid.max()))

    if not cache or math.isinf(vmin):
        print("No sinr_V*.csv data — skipping heatmap.")
        return False

    n = len(cache)
    nrows, ncols = _subplot_grid(n)
    fig, axes = plt.subplots(nrows, ncols, figsize=(3.2 * ncols, 3.0 * nrows), squeeze=False)
    fig.patch.set_facecolor("#1a1a2e")
    bench = os.path.basename(os.path.normpath(benchmark_root))
    fig.suptitle(f"Per-vehicle SINR (dB) — {bench}\n(run-averaged)", fontsize=12, color="white", fontweight="bold")

    ordered = [s for s in scenario_names if s in cache]
    im_ref = None
    for idx, name in enumerate(ordered):
        r, c = divmod(idx, ncols)
        ax = axes[r][c]
        ax.set_facecolor("#1a1a2e")
        tg, vids, mat = cache[name]
        te = np.append(tg, tg[-1] + TIME_GRID_STEP)
        ye = np.arange(len(vids) + 1)
        im = ax.pcolormesh(te, ye, mat, cmap=CMAP, vmin=vmin, vmax=vmax, shading="flat")
        im_ref = im
        ax.set_yticks(np.arange(len(vids)) + 0.5)
        ax.set_yticklabels([f"V{v}" for v in vids], fontsize=6, color="white")
        ax.set_title(name.replace("_", " "), fontsize=9, color="white", fontweight="bold")
        ax.tick_params(colors="white", labelsize=7)
        ax.set_xlabel("Time (s)", fontsize=8, color="white")
        if c == 0:
            ax.set_ylabel("Vehicle", fontsize=8, color="white")
        for sp in ax.spines.values():
            sp.set_edgecolor("#444")

    for j in range(len(ordered), nrows * ncols):
        r, c = divmod(j, ncols)
        axes[r][c].set_visible(False)

    if im_ref is not None:
        fig.subplots_adjust(right=0.88)
        cb_ax = fig.add_axes([0.90, 0.12, 0.02, 0.76])
        cb = fig.colorbar(im_ref, cax=cb_ax)
        cb.set_label("SINR (dB)", fontsize=10, color="white")
        cb.ax.yaxis.set_tick_params(color="white")
        plt.setp(cb.ax.yaxis.get_ticklabels(), color="white")

    fig.savefig(out_path, dpi=120, bbox_inches="tight", facecolor=fig.get_facecolor())
    plt.close(fig)
    print(f"  Saved: {out_path}")
    return True


def plot_timeseries(benchmark_root: str, scenario_names: list[str], out_path: str) -> bool:
    series: list[tuple[str, np.ndarray, np.ndarray]] = []
    for name in scenario_names:
        scenario_dir = os.path.join(benchmark_root, name)
        got = load_sinr_scenario_averaged_union_vids(scenario_dir, None)
        if got is None:
            continue
        tg, _vids, mat = got
        mean_v = np.nanmean(mat, axis=0)
        if np.all(np.isnan(mean_v)):
            continue
        series.append((name, np.array(tg), mean_v))

    if not series:
        print("No SINR data — skipping timeseries.")
        return False

    fig, ax = plt.subplots(figsize=(12, 5))
    fig.patch.set_facecolor("#1a1a2e")
    ax.set_facecolor("#1a1a2e")
    cmap = plt.cm.tab10(np.linspace(0, 1, min(10, max(1, len(series)))))
    for i, (name, tg, y) in enumerate(series):
        ax.plot(tg, y, color=cmap[i % len(cmap)], linewidth=1.4, label=name, alpha=0.88)
    ax.tick_params(colors="white", labelsize=9)
    for sp in ax.spines.values():
        sp.set_edgecolor("#444")
    bench = os.path.basename(os.path.normpath(benchmark_root))
    ax.set_title(f"Fleet-mean SINR (dB) — {bench}", fontsize=12, color="white", fontweight="bold")
    ax.set_xlabel("Time (s)", fontsize=10, color="white")
    ax.set_ylabel("Mean SINR (dB)", fontsize=10, color="white")
    ax.legend(loc="upper right", fontsize=8, ncol=2, facecolor="#1a1a2e", labelcolor="white")
    ax.grid(True, alpha=0.25, color="white")
    fig.tight_layout()
    fig.savefig(out_path, dpi=120, bbox_inches="tight", facecolor=fig.get_facecolor())
    plt.close(fig)
    print(f"  Saved: {out_path}")
    return True


def generate_animation_gif(
    benchmark_root: str,
    scenario_names: list[str],
    out_path: str,
    n_rows: int,
) -> bool:
    raw: dict[str, tuple[list[float], np.ndarray]] = {}
    all_times: set[float] = set()
    all_vals: list[np.ndarray] = []

    for name in scenario_names:
        scenario_dir = os.path.join(benchmark_root, name)
        result = load_sinr_scenario_averaged_padded(scenario_dir, None, n_rows)
        if result is None:
            print(f"  [{name}] No sinr_V*.csv — skipping.")
            continue
        tg, mat = result
        raw[name] = (tg, mat)
        for t in tg:
            all_times.add(round(t, 3))
        valid = mat[~np.isnan(mat)]
        if valid.size:
            all_vals.append(valid)

    if not raw:
        return False

    time_grid = np.array(sorted(all_times))
    scenarios: dict[str, np.ndarray] = {}
    for name in raw:
        scenario_dir = os.path.join(benchmark_root, name)
        r2 = load_sinr_scenario_averaged_padded(scenario_dir, list(time_grid), n_rows)
        if r2:
            scenarios[name] = r2[1]

    if all_vals:
        combined = np.concatenate(all_vals)
        shared_vmin = float(np.nanpercentile(combined, 2))
        shared_vmax = float(np.nanpercentile(combined, 98))
    else:
        shared_vmin, shared_vmax = -10.0, 30.0
    print(f"  SINR animation colour scale: vmin={shared_vmin:.1f} dB  vmax={shared_vmax:.1f} dB")

    time_edges = np.append(time_grid, time_grid[-1] + TIME_GRID_STEP)
    y_edges = np.arange(n_rows + 1)
    frames: list[tuple[str, np.ndarray]] = []
    for name in scenario_names:
        if name in scenarios:
            for _ in range(HOLD_FRAMES):
                frames.append((name, scenarios[name]))

    bench_label = os.path.basename(os.path.normpath(benchmark_root))
    fig, ax = plt.subplots(figsize=(14, 5))
    fig.patch.set_facecolor("#1a1a2e")
    ax.set_facecolor("#1a1a2e")
    dummy = np.full((n_rows, len(time_grid)), np.nan)
    mesh = ax.pcolormesh(
        time_edges, y_edges, dummy, cmap=CMAP, vmin=shared_vmin, vmax=shared_vmax, shading="flat"
    )
    cbar = fig.colorbar(mesh, ax=ax, pad=0.02)
    cbar.set_label("SINR (dB)", fontsize=11, color="white")
    cbar.ax.yaxis.set_tick_params(color="white")
    plt.setp(cbar.ax.yaxis.get_ticklabels(), color="white")
    ax.set_xlabel("Simulation time (s)", fontsize=12, color="white")
    ax.set_ylabel("Vehicle ID", fontsize=12, color="white")
    ax.set_yticks(np.arange(n_rows) + 0.5)
    ax.set_yticklabels([f"V{v}" for v in range(n_rows)], fontsize=7, color="white")
    ax.tick_params(colors="white")
    for spine in ax.spines.values():
        spine.set_edgecolor("white")
    title = ax.set_title("", fontsize=13, fontweight="bold", color="white")

    def update(frame_idx: int):
        scenario, matrix = frames[frame_idx]
        mesh.set_array(matrix.ravel())
        title.set_text(f"SINR (dB) — {bench_label} — {scenario}")
        return mesh, title

    anim = animation.FuncAnimation(fig, update, frames=len(frames), interval=1000 // FPS, blit=False)
    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    anim.save(out_path, writer=animation.PillowWriter(fps=FPS))
    plt.close(fig)
    print(f"  Saved: {out_path}")
    return True


def main() -> int:
    here = os.path.dirname(os.path.abspath(__file__))
    default_root = os.path.join(here, "Priority4cars")

    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=default_root, help="Benchmark folder")
    ap.add_argument(
        "--output-dir",
        default="",
        help="Output directory for PNG/GIF (default: same as --root)",
    )
    ap.add_argument("--scenarios", default="", help="Comma-separated scenario names")
    ap.add_argument("--no-animation", action="store_true", help="Skip GIF generation")
    ap.add_argument("--max-vehicles", type=int, default=0, help="Pad heatmap rows 0..N-1 (default: infer)")
    args = ap.parse_args()

    benchmark_root = os.path.abspath(args.root)
    if not os.path.isdir(benchmark_root):
        print(f"ERROR: not a directory: {benchmark_root}", file=sys.stderr)
        return 1

    out_dir = os.path.abspath(args.output_dir) if args.output_dir.strip() else benchmark_root
    os.makedirs(out_dir, exist_ok=True)

    discovered = discover_scenarios(benchmark_root, "sinr_V*.csv")
    if args.scenarios.strip():
        wanted = {x.strip() for x in args.scenarios.split(",") if x.strip()}
        scenario_names = [n for n in sorted(wanted) if n in discovered]
    else:
        scenario_names = discovered

    if not scenario_names:
        print(f"No scenarios with sinr_V*.csv under {benchmark_root}")
        return 1

    h_ok = plot_heatmap(
        benchmark_root,
        scenario_names,
        os.path.join(out_dir, "sinr_heatmap.png"),
    )
    t_ok = plot_timeseries(
        benchmark_root,
        scenario_names,
        os.path.join(out_dir, "sinr_timeseries.png"),
    )

    anim_ok = False
    if not args.no_animation:
        cap = 64
        n_rows = args.max_vehicles if args.max_vehicles > 0 else infer_n_rows_sinr(benchmark_root, scenario_names, cap)
        n_rows = max(n_rows, 1)
        anim_ok = generate_animation_gif(
            benchmark_root,
            scenario_names,
            os.path.join(out_dir, "sinr_animation.gif"),
            n_rows,
        )
        if not anim_ok:
            print("No data for SINR animation.")

    return 0 if (h_ok or t_ok or anim_ok) else 1


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""
SINR plots for benchmark layouts (partner plot_sinr style).

  <benchmark_root>/<scenario>/run_*/sinr_V*.csv

CSV columns expected: time_s, sinr_db_mean (and optionally sinr_db_min) — ChannelMetrics output.

Default outputs under benchmarks/:
  sinr_heatmap.png
  sinr_timeseries.png
  sinr_animation.gif

Usage:
  python3 benchmarks/plot_sinr.py
  python3 benchmarks/plot_sinr.py --root benchmarks/Priority16cars --no-animation
  python3 benchmarks/plot_sinr.py --roots benchmarks/Priority4cars,benchmarks/Priority8cars
"""

from __future__ import annotations

import argparse
import math
import os
import sys
import warnings
import re

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
if _REPO_ROOT not in sys.path:
    sys.path.insert(0, _REPO_ROOT)

import matplotlib.animation as animation
import matplotlib.pyplot as plt
import numpy as np

from plotter.io.benchmark_metrics_io import (
    TIME_GRID_STEP,
    discover_scenarios,
    infer_n_rows_sinr,
    load_sinr_scenario_averaged_padded,
    load_sinr_scenario_averaged_union_vids,
)
from plotter.io.partner_metrics_io import load_partner_sinr_padded, load_partner_sinr_union_vids

CMAP = "plasma"
HOLD_FRAMES = 8
FPS = 4
DEFAULT_PRIORITY_ROOTS = ("Priority4cars", "Priority8cars", "Priority16cars", "Priority20cars")
PARTNER_SCENARIO = "partner_allVehicles"


def _infer_vehicle_count_from_root(root: str) -> int | None:
    base = os.path.basename(os.path.normpath(root))
    m = re.search(r"(\d+)", base)
    if not m:
        return None
    try:
        return int(m.group(1))
    except ValueError:
        return None


def _scenario_label(name: str) -> str:
    if name == PARTNER_SCENARIO:
        return "RAFT allVehicles (partner)"
    return name.replace("_", " ")


def _load_sinr_scenario_averaged_union_vids(
    scenario_dir: str,
    time_grid_ref: list[float] | None,
) -> tuple[list[float], list[int], np.ndarray] | None:
    # Some run/time bins are empty after unioning time grids across repeated runs.
    with warnings.catch_warnings():
        warnings.filterwarnings("ignore", message="Mean of empty slice", category=RuntimeWarning)
        return load_sinr_scenario_averaged_union_vids(scenario_dir, time_grid_ref)


def _load_sinr_scenario_averaged_padded(
    scenario_dir: str,
    time_grid_ref: list[float] | None,
    n_rows: int,
) -> tuple[list[float], np.ndarray] | None:
    with warnings.catch_warnings():
        warnings.filterwarnings("ignore", message="Mean of empty slice", category=RuntimeWarning)
        return load_sinr_scenario_averaged_padded(scenario_dir, time_grid_ref, n_rows)


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
    include_partner: bool,
) -> bool:
    cache: dict[str, tuple[np.ndarray, list[int], np.ndarray]] = {}
    vmin, vmax = float("inf"), float("-inf")
    vc = _infer_vehicle_count_from_root(benchmark_root) if include_partner else None
    for name in scenario_names:
        if name == PARTNER_SCENARIO:
            if vc is None:
                continue
            got_p = load_partner_sinr_union_vids(vc)
            if got_p is None:
                continue
            tg, vids, mat = got_p
        else:
            scenario_dir = os.path.join(benchmark_root, name)
            got = _load_sinr_scenario_averaged_union_vids(scenario_dir, None)
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
        ax.set_title(_scenario_label(name), fontsize=9, color="white", fontweight="bold")
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


def plot_timeseries(
    benchmark_root: str,
    scenario_names: list[str],
    out_path: str,
    include_partner: bool,
) -> bool:
    series: list[tuple[str, np.ndarray, np.ndarray]] = []
    vc = _infer_vehicle_count_from_root(benchmark_root) if include_partner else None
    for name in scenario_names:
        if name == PARTNER_SCENARIO:
            if vc is None:
                continue
            got_p = load_partner_sinr_union_vids(vc)
            if got_p is None:
                continue
            tg, _vids, mat = got_p
        else:
            scenario_dir = os.path.join(benchmark_root, name)
            got = _load_sinr_scenario_averaged_union_vids(scenario_dir, None)
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
        if name == PARTNER_SCENARIO:
            ax.plot(
                tg,
                y,
                color="white",
                linestyle="--",
                linewidth=1.8,
                label=_scenario_label(name),
                alpha=0.9,
            )
        else:
            ax.plot(
                tg,
                y,
                color=cmap[i % len(cmap)],
                linewidth=1.4,
                label=_scenario_label(name),
                alpha=0.88,
            )
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
    vc = _infer_vehicle_count_from_root(benchmark_root)

    for name in scenario_names:
        if name == PARTNER_SCENARIO:
            if vc is None:
                continue
            result = load_partner_sinr_padded(vc, None, n_rows)
        else:
            scenario_dir = os.path.join(benchmark_root, name)
            result = _load_sinr_scenario_averaged_padded(scenario_dir, None, n_rows)
        if result is None:
            if name == PARTNER_SCENARIO:
                print(f"  [{name}] No partner sinr_V*.csv for {vc}veh — skipping.")
            else:
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
        if name == PARTNER_SCENARIO:
            if vc is None:
                continue
            r2 = load_partner_sinr_padded(vc, list(time_grid), n_rows)
        else:
            scenario_dir = os.path.join(benchmark_root, name)
            r2 = _load_sinr_scenario_averaged_padded(scenario_dir, list(time_grid), n_rows)
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
        label = _scenario_label(scenario)
        title.set_text(f"SINR (dB) — {bench_label} — {label}")
        return mesh, title

    anim = animation.FuncAnimation(fig, update, frames=len(frames), interval=1000 // FPS, blit=False)
    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    anim.save(out_path, writer=animation.PillowWriter(fps=FPS))
    plt.close(fig)
    print(f"  Saved: {out_path}")
    return True


def plot_collective_heatmap(
    benchmark_roots: list[str],
    scenario_names: list[str],
    out_path: str,
    include_partner: bool,
) -> bool:
    cache: dict[tuple[str, str], tuple[np.ndarray, list[int], np.ndarray]] = {}
    vmin, vmax = float("inf"), float("-inf")
    for root in benchmark_roots:
        for name in scenario_names:
            if name == PARTNER_SCENARIO:
                if not include_partner:
                    continue
                vc = _infer_vehicle_count_from_root(root)
                if vc is None:
                    continue
                got_p = load_partner_sinr_union_vids(vc)
                if got_p is None:
                    continue
                tg, vids, mat = got_p
            else:
                got = _load_sinr_scenario_averaged_union_vids(os.path.join(root, name), None)
                if got is None:
                    continue
                tg, vids, mat = got
            cache[(root, name)] = (np.array(tg), vids, mat)
            valid = mat[~np.isnan(mat)]
            if valid.size:
                vmin = min(vmin, float(valid.min()))
                vmax = max(vmax, float(valid.max()))

    if not cache or math.isinf(vmin):
        print("No sinr_V*.csv data — skipping collective heatmap.")
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
    fig.patch.set_facecolor("#1a1a2e")
    fig.suptitle(
        "Per-vehicle SINR (dB) by fleet size and scenario\n(run-averaged; common colour scale)",
        fontsize=12,
        color="white",
        fontweight="bold",
    )

    im_ref = None
    for r, scenario in enumerate(available_scenarios):
        for c, root in enumerate(benchmark_roots):
            ax = axes[r][c]
            ax.set_facecolor("#1a1a2e")
            got = cache.get((root, scenario))
            if got is None:
                ax.set_visible(False)
                continue
            tg, vids, mat = got
            im = ax.pcolormesh(
                np.append(tg, tg[-1] + TIME_GRID_STEP),
                np.arange(len(vids) + 1),
                mat,
                cmap=CMAP,
                vmin=vmin,
                vmax=vmax,
                shading="flat",
            )
            im_ref = im
            if r == 0:
                ax.set_title(
                    os.path.basename(os.path.normpath(root)),
                    fontsize=9,
                    color="white",
                    fontweight="bold",
                )
            ax.set_yticks(np.arange(len(vids)) + 0.5)
            if c == 0:
                ax.set_ylabel(f"{_scenario_label(scenario)}\nVehicle", fontsize=8, color="white")
                ax.set_yticklabels([f"V{v}" for v in vids], fontsize=5, color="white")
            else:
                ax.set_yticklabels([])
            ax.set_xlabel("Time (s)", fontsize=7, color="white")
            ax.tick_params(colors="white", labelsize=6)
            for sp in ax.spines.values():
                sp.set_edgecolor("#444")

    if im_ref is not None:
        fig.subplots_adjust(right=0.90)
        cb_ax = fig.add_axes([0.92, 0.12, 0.015, 0.76])
        cb = fig.colorbar(im_ref, cax=cb_ax)
        cb.set_label("SINR (dB)", fontsize=10, color="white")
        cb.ax.yaxis.set_tick_params(color="white")
        plt.setp(cb.ax.yaxis.get_ticklabels(), color="white")

    fig.savefig(out_path, dpi=120, bbox_inches="tight", facecolor=fig.get_facecolor())
    plt.close(fig)
    print(f"  Saved: {out_path}")
    return True


def plot_collective_timeseries(
    benchmark_roots: list[str],
    scenario_names: list[str],
    out_path: str,
    include_partner: bool,
) -> bool:
    nrows, ncols = _subplot_grid(len(benchmark_roots))
    fig, axes = plt.subplots(nrows, ncols, figsize=(6.2 * ncols, 3.6 * nrows), squeeze=False)
    fig.patch.set_facecolor("#1a1a2e")
    fig.suptitle("Fleet-mean SINR (dB) by fleet size", fontsize=12, color="white", fontweight="bold")
    any_series = False
    cmap = plt.cm.tab10(np.linspace(0, 1, min(10, max(1, len(scenario_names)))))
    for idx, root in enumerate(benchmark_roots):
        r, c = divmod(idx, ncols)
        ax = axes[r][c]
        ax.set_facecolor("#1a1a2e")
        for i, name in enumerate(scenario_names):
            if name == PARTNER_SCENARIO:
                if not include_partner:
                    continue
                vc = _infer_vehicle_count_from_root(root)
                if vc is None:
                    continue
                got_p = load_partner_sinr_union_vids(vc)
                if got_p is None:
                    continue
                tg, _vids, mat = got_p
                style = dict(color="white", linestyle="--", linewidth=1.6, alpha=0.9)
            else:
                got = _load_sinr_scenario_averaged_union_vids(os.path.join(root, name), None)
                if got is None:
                    continue
                tg, _vids, mat = got
                style = dict(color=cmap[i % len(cmap)], linewidth=1.2, alpha=0.88)
            mean_v = np.nanmean(mat, axis=0)
            if np.all(np.isnan(mean_v)):
                continue
            any_series = True
            ax.plot(tg, mean_v, label=_scenario_label(name), **style)
        ax.set_title(os.path.basename(os.path.normpath(root)), fontsize=10, color="white", fontweight="bold")
        ax.set_xlabel("Simulation time (s)", fontsize=8, color="white")
        ax.set_ylabel("Mean SINR (dB)", fontsize=8, color="white")
        ax.tick_params(colors="white", labelsize=8)
        ax.grid(True, alpha=0.25, color="white")
        ax.legend(loc="upper right", fontsize=7, ncol=2, facecolor="#1a1a2e", labelcolor="white")
        for sp in ax.spines.values():
            sp.set_edgecolor("#444")

    for idx in range(len(benchmark_roots), nrows * ncols):
        r, c = divmod(idx, ncols)
        axes[r][c].set_visible(False)

    if not any_series:
        plt.close(fig)
        print("No SINR data — skipping collective timeseries.")
        return False
    fig.tight_layout()
    fig.savefig(out_path, dpi=120, bbox_inches="tight", facecolor=fig.get_facecolor())
    plt.close(fig)
    print(f"  Saved: {out_path}")
    return True


def generate_collective_animation_gif(
    benchmark_roots: list[str],
    scenario_names: list[str],
    out_path: str,
    n_rows_by_root: dict[str, int],
) -> bool:
    raw: dict[tuple[str, str], tuple[list[float], np.ndarray]] = {}
    all_times: set[float] = set()
    all_vals: list[np.ndarray] = []
    for root in benchmark_roots:
        for name in scenario_names:
            if name == PARTNER_SCENARIO:
                vc = _infer_vehicle_count_from_root(root)
                if vc is None:
                    continue
                result = load_partner_sinr_padded(vc, None, n_rows_by_root[root])
            else:
                scenario_dir = os.path.join(root, name)
                if not os.path.isdir(scenario_dir):
                    continue
                result = _load_sinr_scenario_averaged_padded(scenario_dir, None, n_rows_by_root[root])
            if result is None:
                continue
            tg, mat = result
            raw[(root, name)] = (tg, mat)
            for t in tg:
                all_times.add(round(t, 3))
            valid = mat[~np.isnan(mat)]
            if valid.size:
                all_vals.append(valid)

    if not raw:
        return False

    time_grid = np.array(sorted(all_times))
    scenarios: dict[tuple[str, str], np.ndarray] = {}
    for root, name in raw:
        if name == PARTNER_SCENARIO:
            vc = _infer_vehicle_count_from_root(root)
            if vc is None:
                continue
            result = load_partner_sinr_padded(vc, list(time_grid), n_rows_by_root[root])
        else:
            result = _load_sinr_scenario_averaged_padded(
                os.path.join(root, name),
                list(time_grid),
                n_rows_by_root[root],
            )
        if result:
            scenarios[(root, name)] = result[1]

    if all_vals:
        combined = np.concatenate(all_vals)
        shared_vmin = float(np.nanpercentile(combined, 2))
        shared_vmax = float(np.nanpercentile(combined, 98))
    else:
        shared_vmin, shared_vmax = -10.0, 30.0
    print(f"  SINR animation colour scale: vmin={shared_vmin:.1f} dB  vmax={shared_vmax:.1f} dB")

    available_scenarios = [
        name for name in scenario_names if any((root, name) in scenarios for root in benchmark_roots)
    ]
    frames: list[str] = []
    for name in available_scenarios:
        frames.extend([name] * HOLD_FRAMES)
    if not frames:
        return False

    time_edges = np.append(time_grid, time_grid[-1] + TIME_GRID_STEP)
    n_subplot_rows, n_subplot_cols = min(2, len(benchmark_roots)), int(math.ceil(len(benchmark_roots) / 2))
    fig, axes = plt.subplots(
        n_subplot_rows,
        n_subplot_cols,
        figsize=(7.0 * n_subplot_cols, 4.2 * n_subplot_rows),
        squeeze=False,
    )
    fig.patch.set_facecolor("#1a1a2e")
    meshes: dict[str, object] = {}
    titles: dict[str, object] = {}
    for idx, root in enumerate(benchmark_roots):
        r, c = divmod(idx, n_subplot_cols)
        ax = axes[r][c]
        ax.set_facecolor("#1a1a2e")
        n_rows = n_rows_by_root[root]
        mesh = ax.pcolormesh(
            time_edges,
            np.arange(n_rows + 1),
            np.full((n_rows, len(time_grid)), np.nan),
            cmap=CMAP,
            vmin=shared_vmin,
            vmax=shared_vmax,
            shading="flat",
        )
        meshes[root] = mesh
        titles[root] = ax.set_title(
            os.path.basename(os.path.normpath(root)),
            fontsize=12,
            fontweight="bold",
            color="white",
        )
        ax.set_xlabel("Simulation time (s)", fontsize=10, color="white")
        ax.set_ylabel("Vehicle ID", fontsize=10, color="white")
        ax.set_yticks(np.arange(n_rows) + 0.5)
        ax.set_yticklabels([f"V{v}" for v in range(n_rows)], fontsize=6, color="white")
        ax.tick_params(colors="white")
        for sp in ax.spines.values():
            sp.set_edgecolor("white")

    for idx in range(len(benchmark_roots), n_subplot_rows * n_subplot_cols):
        r, c = divmod(idx, n_subplot_cols)
        axes[r][c].set_visible(False)

    cbar = fig.colorbar(next(iter(meshes.values())), ax=axes.ravel().tolist(), pad=0.02)
    cbar.set_label("SINR (dB)", fontsize=11, color="white")
    cbar.ax.yaxis.set_tick_params(color="white")
    plt.setp(cbar.ax.yaxis.get_ticklabels(), color="white")
    suptitle = fig.suptitle("", fontsize=14, color="white", fontweight="bold")

    def update(frame_idx: int):
        scenario = frames[frame_idx]
        artists: list[object] = [suptitle]
        for root in benchmark_roots:
            mat = scenarios.get((root, scenario))
            if mat is None:
                mat = np.full((n_rows_by_root[root], len(time_grid)), np.nan)
            meshes[root].set_array(mat.ravel())
            artists.extend([meshes[root], titles[root]])
        suptitle.set_text(f"SINR (dB) — {_scenario_label(scenario)}")
        return artists

    anim = animation.FuncAnimation(fig, update, frames=len(frames), interval=1000 // FPS, blit=False)
    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    anim.save(out_path, writer=animation.PillowWriter(fps=FPS))
    plt.close(fig)
    print(f"  Saved: {out_path}")
    return True


def main() -> int:
    here = os.path.abspath(os.path.join(os.path.dirname(__file__), "../..", "benchmarks"))

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
        help="Output directory for PNG/GIF (default: same as --root)",
    )
    ap.add_argument("--scenarios", default="", help="Comma-separated scenario names")
    ap.add_argument(
        "--partner-data",
        action="store_true",
        help="Overlay RAFT partner data (allVehicles) from /home/yash/partnersv2v/res.",
    )
    ap.add_argument("--no-animation", action="store_true", help="Skip GIF generation")
    ap.add_argument("--max-vehicles", type=int, default=0, help="Pad heatmap rows 0..N-1 (default: infer)")
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
        root: discover_scenarios(root, "sinr_V*.csv") for root in benchmark_roots
    }
    discovered = sorted({name for names in discovered_by_root.values() for name in names})
    if args.scenarios.strip():
        wanted = {x.strip() for x in args.scenarios.split(",") if x.strip()}
        scenario_names = [n for n in sorted(wanted) if n in discovered]
    else:
        scenario_names = discovered
    if args.partner_data and PARTNER_SCENARIO not in scenario_names:
        scenario_names = list(scenario_names) + [PARTNER_SCENARIO]

    if not scenario_names:
        print("No scenarios with sinr_V*.csv under requested benchmark roots")
        return 1

    single_root_legacy = len(benchmark_roots) == 1 and args.root.strip() and not args.roots.strip()
    if single_root_legacy:
        benchmark_root = benchmark_roots[0]
        h_ok = plot_heatmap(
            benchmark_root,
            scenario_names,
            os.path.join(out_dir, "sinr_heatmap.png"),
            include_partner=args.partner_data,
        )
        t_ok = plot_timeseries(
            benchmark_root,
            scenario_names,
            os.path.join(out_dir, "sinr_timeseries.png"),
            include_partner=args.partner_data,
        )
    else:
        h_ok = plot_collective_heatmap(
            benchmark_roots,
            scenario_names,
            os.path.join(out_dir, "sinr_heatmap.png"),
            include_partner=args.partner_data,
        )
        t_ok = plot_collective_timeseries(
            benchmark_roots,
            scenario_names,
            os.path.join(out_dir, "sinr_timeseries.png"),
            include_partner=args.partner_data,
        )

    anim_ok = False
    if not args.no_animation:
        cap = 64
        if single_root_legacy:
            benchmark_root = benchmark_roots[0]
            n_rows = (
                args.max_vehicles
                if args.max_vehicles > 0
                else infer_n_rows_sinr(benchmark_root, scenario_names, cap)
            )
            n_rows = max(n_rows, 1)
            anim_ok = generate_animation_gif(
                benchmark_root,
                scenario_names,
                os.path.join(out_dir, "sinr_animation.gif"),
                n_rows,
            )
        else:
            n_rows_by_root: dict[str, int] = {}
            for root in benchmark_roots:
                if args.max_vehicles > 0:
                    n_rows_by_root[root] = args.max_vehicles
                else:
                    root_scenarios = [s for s in scenario_names if s in discovered_by_root[root]]
                    n_rows_by_root[root] = max(infer_n_rows_sinr(root, root_scenarios, cap), 1)
            anim_ok = generate_collective_animation_gif(
                benchmark_roots,
                scenario_names,
                os.path.join(out_dir, "sinr_animation.gif"),
                n_rows_by_root,
            )
        if not anim_ok:
            print("No data for SINR animation.")

    return 0 if (h_ok or t_ok or anim_ok) else 1


if __name__ == "__main__":
    sys.exit(main())

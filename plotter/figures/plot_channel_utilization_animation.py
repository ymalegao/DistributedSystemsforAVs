#!/usr/bin/env python3
"""
Channel utilization heatmap animation for ResDB / Veins benchmark layouts.

Expected layout (CSV from ChannelMetrics + enableChannelMetricsCsv):

  <benchmark_root>/<scenario_name>/run_*/channel_V*.csv

If there are no run_* subfolders, CSVs may sit directly in <scenario_name>/.

By default, the script reads all priority benchmark roots found next to this file:

  Priority4cars, Priority8cars, Priority12cars, Priority16cars

and writes one collective GIF with one panel per fleet size.

Usage:
  python3 benchmarks/plot_channel_utilization_animation.py
  python3 benchmarks/plot_channel_utilization_animation.py --root benchmarks/Priority8cars
  python3 benchmarks/plot_channel_utilization_animation.py --roots benchmarks/Priority4cars,benchmarks/Priority8cars
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
    infer_n_rows_channel,
    load_channel_scenario_averaged_padded,
)
from plotter.io.partner_metrics_io import load_partner_channel_padded

CMAP = "YlOrRd"
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


def _load_channel_scenario_averaged_padded(
    scenario_dir: str,
    time_grid_ref: list[float] | None,
    n_rows: int,
) -> tuple[list[float], np.ndarray] | None:
    # Collective plots use a union time grid, so some run/time bins are expected to be empty.
    with warnings.catch_warnings():
        warnings.filterwarnings("ignore", message="Mean of empty slice", category=RuntimeWarning)
        return load_channel_scenario_averaged_padded(scenario_dir, time_grid_ref, n_rows)


def generate_gif(
    benchmark_root: str,
    scenario_names: list[str],
    out_path: str,
    n_rows: int,
    title_prefix: str,
) -> bool:
    raw: dict[str, tuple[list[float], np.ndarray]] = {}
    all_times: set[float] = set()
    vc = _infer_vehicle_count_from_root(benchmark_root)

    for name in scenario_names:
        if name == PARTNER_SCENARIO:
            if vc is None:
                continue
            result = load_partner_channel_padded(vc, None, n_rows)
        else:
            scenario_dir = os.path.join(benchmark_root, name)
            result = _load_channel_scenario_averaged_padded(scenario_dir, None, n_rows)
        if result is None:
            if name == PARTNER_SCENARIO:
                print(f"  [{name}] No partner channel_V*.csv for {vc}veh — skipping.")
            else:
                print(f"  [{name}] No channel_V*.csv under {scenario_dir} — skipping.")
            continue
        tg, mat = result
        raw[name] = (tg, mat)
        for t in tg:
            all_times.add(round(t, 3))

    if not raw:
        print("No scenario data found. Enable OMNeT++ appl.enableChannelMetricsCsv and place")
        print("channel_V*.csv under each scenario's run_* (or directly in the scenario folder).")
        return False

    time_grid = np.array(sorted(all_times))

    scenarios: dict[str, np.ndarray] = {}
    for name in raw:
        if name == PARTNER_SCENARIO:
            if vc is None:
                continue
            result = load_partner_channel_padded(vc, list(time_grid), n_rows)
        else:
            scenario_dir = os.path.join(benchmark_root, name)
            result = _load_channel_scenario_averaged_padded(scenario_dir, list(time_grid), n_rows)
        if result:
            scenarios[name] = result[1]

    global_vmax = 0.05
    for mat in scenarios.values():
        v = float(np.nanmax(mat))
        if v > global_vmax:
            global_vmax = v
    print(f"  vmax (channel utilization colour scale): {global_vmax:.4f}")

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
        time_edges, y_edges, dummy, cmap=CMAP, vmin=0.0, vmax=global_vmax, shading="flat"
    )

    cbar = fig.colorbar(mesh, ax=ax, pad=0.02)
    cbar.set_label("Channel utilization", fontsize=11, color="white")
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
        label = "RAFT allVehicles (partner)" if scenario == PARTNER_SCENARIO else scenario
        title.set_text(f"{title_prefix} — {bench_label} — {label}")
        return mesh, title

    anim = animation.FuncAnimation(
        fig, update, frames=len(frames), interval=1000 // FPS, blit=False
    )
    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    anim.save(out_path, writer=animation.PillowWriter(fps=FPS))
    plt.close(fig)
    print(f"  Saved: {out_path}")
    return True


def _subplot_grid(n: int) -> tuple[int, int]:
    ncols = min(2, max(1, n))
    nrows = int(math.ceil(n / ncols))
    return nrows, ncols


def generate_collective_gif(
    benchmark_roots: list[str],
    scenario_names: list[str],
    out_path: str,
    n_rows_by_root: dict[str, int],
    title_prefix: str,
) -> bool:
    raw: dict[tuple[str, str], tuple[list[float], np.ndarray]] = {}
    all_times: set[float] = set()

    for root in benchmark_roots:
        for name in scenario_names:
            if name == PARTNER_SCENARIO:
                vc = _infer_vehicle_count_from_root(root)
                if vc is None:
                    continue
                result = load_partner_channel_padded(vc, None, n_rows_by_root[root])
            else:
                scenario_dir = os.path.join(root, name)
                if not os.path.isdir(scenario_dir):
                    continue
                result = _load_channel_scenario_averaged_padded(
                    scenario_dir,
                    None,
                    n_rows_by_root[root],
                )
            if result is None:
                continue
            tg, mat = result
            raw[(root, name)] = (tg, mat)
            for t in tg:
                all_times.add(round(t, 3))

    if not raw:
        print("No scenario data found across the requested benchmark roots.")
        print("Enable OMNeT++ appl.enableChannelMetricsCsv and place channel_V*.csv under each run_*.")
        return False

    time_grid = np.array(sorted(all_times))
    if len(time_grid) == 0:
        print("No channel time samples found.")
        return False

    scenarios: dict[tuple[str, str], np.ndarray] = {}
    global_vmax = 0.05
    for root, name in raw:
        if name == PARTNER_SCENARIO:
            vc = _infer_vehicle_count_from_root(root)
            if vc is None:
                continue
            result = load_partner_channel_padded(vc, list(time_grid), n_rows_by_root[root])
        else:
            scenario_dir = os.path.join(root, name)
            result = _load_channel_scenario_averaged_padded(
                scenario_dir,
                list(time_grid),
                n_rows_by_root[root],
            )
        if not result:
            continue
        mat = result[1]
        scenarios[(root, name)] = mat
        if mat.size:
            v = float(np.nanmax(mat))
            if v > global_vmax:
                global_vmax = v

    available_scenarios = [
        name for name in scenario_names if any((root, name) in scenarios for root in benchmark_roots)
    ]
    if not available_scenarios:
        print("No requested scenarios have channel data across the benchmark roots.")
        return False

    print(f"  vmax (channel utilization colour scale): {global_vmax:.4f}")

    time_edges = np.append(time_grid, time_grid[-1] + TIME_GRID_STEP)
    n_panels = len(benchmark_roots)
    n_subplot_rows, n_subplot_cols = _subplot_grid(n_panels)
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
        dummy = np.full((n_rows, len(time_grid)), np.nan)
        mesh = ax.pcolormesh(
            time_edges,
            np.arange(n_rows + 1),
            dummy,
            cmap=CMAP,
            vmin=0.0,
            vmax=global_vmax,
            shading="flat",
        )
        meshes[root] = mesh
        label = os.path.basename(os.path.normpath(root))
        titles[root] = ax.set_title(label, fontsize=12, fontweight="bold", color="white")
        ax.set_xlabel("Simulation time (s)", fontsize=10, color="white")
        ax.set_ylabel("Vehicle ID", fontsize=10, color="white")
        ax.set_yticks(np.arange(n_rows) + 0.5)
        ax.set_yticklabels([f"V{v}" for v in range(n_rows)], fontsize=6, color="white")
        ax.tick_params(colors="white")
        for spine in ax.spines.values():
            spine.set_edgecolor("white")

    for idx in range(n_panels, n_subplot_rows * n_subplot_cols):
        r, c = divmod(idx, n_subplot_cols)
        axes[r][c].set_visible(False)

    cbar = fig.colorbar(next(iter(meshes.values())), ax=axes.ravel().tolist(), pad=0.02)
    cbar.set_label("Channel utilization", fontsize=11, color="white")
    cbar.ax.yaxis.set_tick_params(color="white")
    plt.setp(cbar.ax.yaxis.get_ticklabels(), color="white")

    suptitle = fig.suptitle("", fontsize=14, fontweight="bold", color="white")
    frames: list[str] = []
    for name in available_scenarios:
        frames.extend([name] * HOLD_FRAMES)

    def update(frame_idx: int):
        scenario = frames[frame_idx]
        artists: list[object] = [suptitle]
        for root in benchmark_roots:
            mesh = meshes[root]
            mat = scenarios.get((root, scenario))
            if mat is None:
                mat = np.full((n_rows_by_root[root], len(time_grid)), np.nan)
            mesh.set_array(mat.ravel())
            titles[root].set_text(os.path.basename(os.path.normpath(root)))
            artists.extend([mesh, titles[root]])
        label = "RAFT allVehicles (partner)" if scenario == PARTNER_SCENARIO else scenario.replace("_", " ")
        suptitle.set_text(f"{title_prefix} — {label}")
        return artists

    anim = animation.FuncAnimation(
        fig,
        update,
        frames=len(frames),
        interval=1000 // FPS,
        blit=False,
    )
    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    anim.save(out_path, writer=animation.PillowWriter(fps=FPS))
    plt.close(fig)
    print(f"  Saved: {out_path}")
    return True


def main() -> int:
    # Keep defaults rooted at the repository, even when this file is run directly.
    here = os.path.abspath(os.path.join(os.path.dirname(__file__), "../..", "benchmarks"))

    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument(
        "--root",
        default="",
        help="Single benchmark folder containing scenario subfolders (legacy mode)",
    )
    ap.add_argument(
        "--roots",
        default="",
        help="Comma-separated benchmark roots (default: Priority4/8/12/16 next to this script)",
    )
    ap.add_argument(
        "--output",
        default="",
        help="Output GIF path (default: benchmarks/channel_utilization_animation.gif, or <root>/... in --root mode)",
    )
    ap.add_argument(
        "--max-vehicles",
        type=int,
        default=0,
        help="Heatmap rows 0..N-1 (default: infer from CSV indices, cap 64)",
    )
    ap.add_argument(
        "--scenarios",
        default="",
        help="Comma-separated scenario folder names to include (default: all that have CSVs)",
    )
    ap.add_argument(
        "--title-prefix",
        default="Channel utilization (mean over runs)",
        help="Short title prefix for each frame",
    )
    ap.add_argument(
        "--partner-data",
        action="store_true",
        help="Include RAFT partner data (allVehicles) from /home/yash/partnersv2v/res as an extra frame.",
    )
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

    discovered_by_root = {
        root: discover_scenarios(root, "channel_V*.csv") for root in benchmark_roots
    }
    discovered = sorted({name for names in discovered_by_root.values() for name in names})
    if args.scenarios.strip():
        wanted = {x.strip() for x in args.scenarios.split(",") if x.strip()}
        scenario_names = [n for n in sorted(wanted) if n in discovered]
        missing = wanted - set(discovered)
        if missing:
            print(f"WARNING: no data for scenarios: {sorted(missing)}")
    else:
        scenario_names = discovered
    if args.partner_data and PARTNER_SCENARIO not in scenario_names:
        scenario_names = list(scenario_names) + [PARTNER_SCENARIO]

    if not scenario_names:
        print("No scenarios with channel_V*.csv under requested benchmark roots:")
        for root in benchmark_roots:
            print(f"  {root}")
        return 1

    out_path = args.output.strip()
    if not out_path:
        if args.root.strip() and not args.roots.strip():
            out_path = os.path.join(benchmark_roots[0], "channel_utilization_animation.gif")
        else:
            out_path = os.path.join(here, "channel_utilization_animation.gif")

    cap = 64
    n_rows_by_root: dict[str, int] = {}
    for root in benchmark_roots:
        if args.max_vehicles and args.max_vehicles > 0:
            n_rows = args.max_vehicles
        else:
            root_scenarios = [s for s in scenario_names if s in discovered_by_root[root]]
            n_rows = infer_n_rows_channel(root, root_scenarios, cap)
            n_rows = max(n_rows, 1)
        n_rows_by_root[root] = n_rows

    print("Benchmark roots:")
    for root in benchmark_roots:
        label = os.path.basename(os.path.normpath(root))
        print(f"  {label}: {root} ({n_rows_by_root[root]} row(s))")
    print(f"Scenarios ({len(scenario_names)}): {', '.join(scenario_names)}")

    if len(benchmark_roots) == 1 and args.root.strip() and not args.roots.strip():
        benchmark_root = benchmark_roots[0]
        ok = generate_gif(
            benchmark_root,
            scenario_names,
            out_path,
            n_rows_by_root[benchmark_root],
            args.title_prefix,
        )
    else:
        ok = generate_collective_gif(
            benchmark_roots,
            scenario_names,
            out_path,
            n_rows_by_root,
            args.title_prefix,
        )
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""
Channel utilization heatmap animation for ResDB / Veins benchmark layouts.

Expected layout (CSV from ChannelMetrics + enableChannelMetricsCsv):

  <benchmark_root>/<scenario_name>/run_*/channel_V*.csv

If there are no run_* subfolders, CSVs may sit directly in <scenario_name>/.

Example (4-car priority suite):

  python3 plot_channel_utilization_animation.py --root Priority4cars

Writes channel_utilization_animation.gif next to --root by default.

Usage:
  python3 benchmarks/plot_channel_utilization_animation.py
  python3 benchmarks/plot_channel_utilization_animation.py --root benchmarks/Priority8cars
"""

from __future__ import annotations

import argparse
import csv
import glob
import os
import sys

import matplotlib.animation as animation
import matplotlib.pyplot as plt
import numpy as np

TIME_GRID_STEP = 0.1
CMAP = "YlOrRd"
HOLD_FRAMES = 8
FPS = 4


def load_utilization_run(run_dir: str) -> dict[int, list[tuple[float, float]]]:
    data: dict[int, list[tuple[float, float]]] = {}
    pattern = os.path.join(run_dir, "channel_V*.csv")
    for path in sorted(glob.glob(pattern)):
        try:
            with open(path, newline="", encoding="utf-8") as f:
                reader = csv.DictReader(f)
                if not reader.fieldnames or "time_s" not in reader.fieldnames:
                    print(f"WARNING: skip {path} (missing time_s header)")
                    continue
                util_key = (
                    "channel_utilization"
                    if "channel_utilization" in reader.fieldnames
                    else None
                )
                if util_key is None:
                    print(f"WARNING: skip {path} (missing channel_utilization header)")
                    continue
                rows = [
                    (float(r["time_s"]), float(r[util_key]))
                    for r in reader
                    if r.get("time_s") not in (None, "") and r.get(util_key) not in (None, "")
                ]
            if rows:
                base = os.path.basename(path)
                vid = int(base.replace("channel_V", "").replace(".csv", ""))
                data[vid] = rows
        except Exception as e:
            print(f"WARNING: could not read {path}: {e}")
    return data


def run_directories_for_scenario(scenario_dir: str) -> list[str]:
    runs = sorted(glob.glob(os.path.join(scenario_dir, "run_*")))
    runs = [d for d in runs if os.path.isdir(d) and glob.glob(os.path.join(d, "channel_V*.csv"))]
    if runs:
        return runs
    if glob.glob(os.path.join(scenario_dir, "channel_V*.csv")):
        return [scenario_dir]
    return []


def max_vehicle_id(utilization_data: dict[int, list[tuple[float, float]]]) -> int:
    return max(utilization_data.keys()) if utilization_data else -1


def build_padded_matrix(
    utilization_data: dict[int, list[tuple[float, float]]],
    time_grid: list[float],
    n_rows: int,
) -> np.ndarray:
    t_index = {round(t, 3): i for i, t in enumerate(time_grid)}
    matrix = np.full((n_rows, len(time_grid)), np.nan)
    for vid, rows in utilization_data.items():
        if vid < 0 or vid >= n_rows:
            continue
        for t, u in rows:
            t_key = round(t, 3)
            if t_key in t_index:
                matrix[vid, t_index[t_key]] = u
    return matrix


def load_scenario_averaged(
    scenario_dir: str,
    time_grid_ref: list[float] | None,
    n_rows: int,
) -> tuple[list[float], np.ndarray] | None:
    run_dirs = run_directories_for_scenario(scenario_dir)
    if not run_dirs:
        return None

    all_times: set[float] = set()
    all_data: list[dict[int, list[tuple[float, float]]]] = []
    for run_dir in run_dirs:
        data = load_utilization_run(run_dir)
        if data:
            all_data.append(data)
            for rows in data.values():
                for t, _ in rows:
                    all_times.add(round(t, 3))

    if not all_data:
        return None

    time_grid = sorted(all_times) if time_grid_ref is None else list(time_grid_ref)
    matrices = [build_padded_matrix(d, time_grid, n_rows) for d in all_data]
    stacked = np.stack(matrices, axis=0)
    return time_grid, np.nanmean(stacked, axis=0)


def discover_scenarios(benchmark_root: str) -> list[str]:
    names = []
    for name in sorted(os.listdir(benchmark_root)):
        if name.startswith("."):
            continue
        path = os.path.join(benchmark_root, name)
        if not os.path.isdir(path):
            continue
        if run_directories_for_scenario(path):
            names.append(name)
    return names


def infer_n_rows(benchmark_root: str, scenario_names: list[str], cap: int) -> int:
    m = -1
    for s in scenario_names:
        for run_dir in run_directories_for_scenario(os.path.join(benchmark_root, s)):
            data = load_utilization_run(run_dir)
            m = max(m, max_vehicle_id(data))
    if m < 0:
        return 1
    return min(cap, m + 1)


def generate_gif(
    benchmark_root: str,
    scenario_names: list[str],
    out_path: str,
    n_rows: int,
    title_prefix: str,
) -> bool:
    raw: dict[str, tuple[list[float], np.ndarray]] = {}
    all_times: set[float] = set()

    for name in scenario_names:
        scenario_dir = os.path.join(benchmark_root, name)
        result = load_scenario_averaged(scenario_dir, None, n_rows)
        if result is None:
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
        scenario_dir = os.path.join(benchmark_root, name)
        result = load_scenario_averaged(scenario_dir, list(time_grid), n_rows)
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
        title.set_text(f"{title_prefix} — {bench_label} — {scenario}")
        return mesh, title

    anim = animation.FuncAnimation(
        fig, update, frames=len(frames), interval=1000 // FPS, blit=False
    )
    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    anim.save(out_path, writer=animation.PillowWriter(fps=FPS))
    plt.close(fig)
    print(f"  Saved: {out_path}")
    return True


def main() -> int:
    here = os.path.dirname(os.path.abspath(__file__))
    default_root = os.path.join(here, "Priority4cars")

    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument(
        "--root",
        default=default_root,
        help=f"Benchmark folder containing scenario subfolders (default: {default_root})",
    )
    ap.add_argument(
        "--output",
        default="",
        help="Output GIF path (default: <root>/channel_utilization_animation.gif)",
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
    args = ap.parse_args()

    benchmark_root = os.path.abspath(args.root)
    if not os.path.isdir(benchmark_root):
        print(f"ERROR: not a directory: {benchmark_root}", file=sys.stderr)
        return 1

    discovered = discover_scenarios(benchmark_root)
    if args.scenarios.strip():
        wanted = {x.strip() for x in args.scenarios.split(",") if x.strip()}
        scenario_names = [n for n in sorted(wanted) if n in discovered]
        missing = wanted - set(discovered)
        if missing:
            print(f"WARNING: no data for scenarios: {sorted(missing)}")
    else:
        scenario_names = discovered

    if not scenario_names:
        all_subdirs = sorted(
            n
            for n in os.listdir(benchmark_root)
            if not n.startswith(".") and os.path.isdir(os.path.join(benchmark_root, n))
        )
        print(f"No scenarios with channel_V*.csv under: {benchmark_root}")
        if all_subdirs:
            print(f"Found {len(all_subdirs)} subfolder(s) (add channel_V*.csv under each run_*): "
                  + ", ".join(all_subdirs[:20])
                  + (" …" if len(all_subdirs) > 20 else ""))
        return 1

    cap = 64
    if args.max_vehicles and args.max_vehicles > 0:
        n_rows = args.max_vehicles
    else:
        n_rows = infer_n_rows(benchmark_root, scenario_names, cap)
        n_rows = max(n_rows, 1)

    out_path = args.output.strip()
    if not out_path:
        out_path = os.path.join(benchmark_root, "channel_utilization_animation.gif")

    print(f"Benchmark root: {benchmark_root}")
    print(f"Scenarios ({len(scenario_names)}): {', '.join(scenario_names)}")
    print(f"Matrix rows (vehicles 0..{n_rows - 1}): {n_rows}")

    ok = generate_gif(
        benchmark_root,
        scenario_names,
        out_path,
        n_rows,
        args.title_prefix,
    )
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

"""
Shared CSV discovery / loading for benchmark suites.

Layout:
  <benchmark_root>/<scenario>/run_*/channel_V*.csv
  <benchmark_root>/<scenario>/run_*/sinr_V*.csv

CSVs may also live directly under <scenario>/ if there are no run_* dirs.
"""

from __future__ import annotations

import csv
import glob
import json
import os
from typing import Iterable

import numpy as np


def metric_json_paths(
    benchmark_base_dir: str,
    family_prefix: str,
    vehicle_count: int,
    scenario_subdir: str,
) -> list[str]:
    """Return analyze_log JSONs for e.g. Priority16cars/no_amb/run_*/16veh_*.json."""
    pattern = os.path.join(
        benchmark_base_dir,
        f"{family_prefix}{vehicle_count}cars",
        scenario_subdir,
        "run_*",
        f"{vehicle_count}veh_*.json",
    )
    return sorted(glob.glob(pattern))


def _vehicle_wait_s(vehicle: dict) -> float | None:
    if "wait_intersection_s" in vehicle:
        try:
            return float(vehicle.get("wait_intersection_s"))
        except (TypeError, ValueError):
            pass
    durs = vehicle.get("durations_ms")
    if isinstance(durs, dict):
        try:
            ms = float(durs.get("total_wait_time"))
            return ms / 1000.0
        except (TypeError, ValueError):
            pass
    ts = vehicle.get("timestamps_ms")
    if isinstance(ts, dict):
        try:
            stopped = float(ts.get("stopped", 0) or 0)
            passed = float(ts.get("passed", 0) or 0)
            if passed > 0 and stopped > 0 and passed >= stopped:
                return (passed - stopped) / 1000.0
        except (TypeError, ValueError):
            pass
    return None


def load_wait_samples_from_metric_jsons(
    paths: Iterable[str],
    role_filter: str = "all",
    exclude_fallback: bool = True,
) -> list[float]:
    """Load wait samples from partner-style or analyze_log legacy JSON files."""
    waits: list[float] = []
    for path in paths:
        try:
            with open(path, encoding="utf-8") as f:
                data = json.load(f)
        except Exception as e:
            print(f"WARNING: could not read {path}: {e}")
            continue
        if isinstance(data, list):
            vehicles = [v for v in data if isinstance(v, dict)]
        elif isinstance(data, dict) and isinstance(data.get("per_car"), list):
            vehicles = [v for v in data["per_car"] if isinstance(v, dict)]
        else:
            vehicles = []
        for v in vehicles:
            if exclude_fallback:
                used_fb = v.get("used_fallback")
                if used_fb is None and isinstance(v.get("bft_stats"), dict):
                    used_fb = v["bft_stats"].get("used_fallback")
                if used_fb is True or v.get("coordination_method") == "fallback":
                    continue
            role = v.get("role")
            if not isinstance(role, str):
                role = "ambulance" if bool(v.get("is_priority_vehicle", False)) else "normal"
            if role_filter == "normal" and role != "normal":
                continue
            if role_filter == "ambulance" and role != "ambulance":
                continue
            wt = _vehicle_wait_s(v)
            if wt is not None and wt > 0:
                waits.append(wt)
    return waits


def run_directories_for_scenario(scenario_dir: str, csv_glob: str) -> list[str]:
    runs = sorted(glob.glob(os.path.join(scenario_dir, "run_*")))
    runs = [d for d in runs if os.path.isdir(d) and glob.glob(os.path.join(d, csv_glob))]
    if runs:
        return runs
    if glob.glob(os.path.join(scenario_dir, csv_glob)):
        return [scenario_dir]
    return []


def discover_scenarios(benchmark_root: str, csv_glob: str) -> list[str]:
    names: list[str] = []
    for name in sorted(os.listdir(benchmark_root)):
        if name.startswith("."):
            continue
        path = os.path.join(benchmark_root, name)
        if not os.path.isdir(path):
            continue
        if run_directories_for_scenario(path, csv_glob):
            names.append(name)
    return names


def load_channel_run(run_dir: str) -> dict[int, list[tuple[float, float]]]:
    data: dict[int, list[tuple[float, float]]] = {}
    pattern = os.path.join(run_dir, "channel_V*.csv")
    for path in sorted(glob.glob(pattern)):
        try:
            with open(path, newline="", encoding="utf-8") as f:
                reader = csv.DictReader(f)
                if not reader.fieldnames or "time_s" not in reader.fieldnames:
                    continue
                if "channel_utilization" not in reader.fieldnames:
                    continue
                rows = [
                    (float(r["time_s"]), float(r["channel_utilization"]))
                    for r in reader
                    if r.get("time_s") not in (None, "")
                    and r.get("channel_utilization") not in (None, "")
                ]
            if rows:
                base = os.path.basename(path)
                vid = int(base.replace("channel_V", "").replace(".csv", ""))
                data[vid] = rows
        except Exception as e:
            print(f"WARNING: could not read {path}: {e}")
    return data


def load_sinr_run(run_dir: str) -> dict[int, list[tuple[float, float]]]:
    """Per-vehicle rows: (time_s, sinr_db_mean)."""
    data: dict[int, list[tuple[float, float]]] = {}
    pattern = os.path.join(run_dir, "sinr_V*.csv")
    for path in sorted(glob.glob(pattern)):
        try:
            with open(path, newline="", encoding="utf-8") as f:
                reader = csv.DictReader(f)
                if not reader.fieldnames or "time_s" not in reader.fieldnames:
                    continue
                key = "sinr_db_mean" if "sinr_db_mean" in reader.fieldnames else None
                if key is None:
                    continue
                rows: list[tuple[float, float]] = []
                for r in reader:
                    try:
                        rows.append((float(r["time_s"]), float(r[key])))
                    except (ValueError, KeyError):
                        pass
            if rows:
                base = os.path.basename(path)
                vid = int(base.replace("sinr_V", "").replace(".csv", ""))
                data[vid] = rows
        except Exception as e:
            print(f"WARNING: could not read {path}: {e}")
    return data


def max_vehicle_id(rows_by_vid: dict[int, list[tuple[float, float]]]) -> int:
    return max(rows_by_vid.keys()) if rows_by_vid else -1


def build_padded_matrix(
    data: dict[int, list[tuple[float, float]]],
    time_grid: list[float],
    n_rows: int,
) -> np.ndarray:
    t_index = {round(t, 3): i for i, t in enumerate(time_grid)}
    matrix = np.full((n_rows, len(time_grid)), np.nan)
    for vid, rows in data.items():
        if vid < 0 or vid >= n_rows:
            continue
        for t, u in rows:
            k = round(t, 3)
            if k in t_index:
                matrix[vid, t_index[k]] = u
    return matrix


def build_matrix_sorted_vids(
    data: dict[int, list[tuple[float, float]]],
    time_grid: list[float],
    sorted_vids: list[int],
) -> np.ndarray:
    t_index = {round(t, 3): i for i, t in enumerate(time_grid)}
    mat = np.full((len(sorted_vids), len(time_grid)), np.nan)
    row_of = {v: i for i, v in enumerate(sorted_vids)}
    for vid, rows in data.items():
        if vid not in row_of:
            continue
        i = row_of[vid]
        for t, val in rows:
            k = round(t, 3)
            if k in t_index:
                mat[i, t_index[k]] = val
    return mat


def load_channel_scenario_averaged_padded(
    scenario_dir: str,
    time_grid_ref: list[float] | None,
    n_rows: int,
) -> tuple[list[float], np.ndarray] | None:
    run_dirs = run_directories_for_scenario(scenario_dir, "channel_V*.csv")
    if not run_dirs:
        return None
    all_times: set[float] = set()
    all_data: list[dict[int, list[tuple[float, float]]]] = []
    for run_dir in run_dirs:
        d = load_channel_run(run_dir)
        if d:
            all_data.append(d)
            for rows in d.values():
                for t, _ in rows:
                    all_times.add(round(t, 3))
    if not all_data:
        return None
    time_grid = sorted(all_times) if time_grid_ref is None else list(time_grid_ref)
    matrices = [build_padded_matrix(d, time_grid, n_rows) for d in all_data]
    return time_grid, np.nanmean(np.stack(matrices, axis=0), axis=0)


def load_channel_scenario_averaged_union_vids(
    scenario_dir: str,
    time_grid_ref: list[float] | None,
) -> tuple[list[float], list[int], np.ndarray] | None:
    """Mean over runs; rows follow sorted vehicle ids present in any run."""
    run_dirs = run_directories_for_scenario(scenario_dir, "channel_V*.csv")
    if not run_dirs:
        return None
    all_times: set[float] = set()
    all_vids: set[int] = set()
    all_data: list[dict[int, list[tuple[float, float]]]] = []
    for run_dir in run_dirs:
        d = load_channel_run(run_dir)
        if d:
            all_data.append(d)
            all_vids.update(d.keys())
            for rows in d.values():
                for t, _ in rows:
                    all_times.add(round(t, 3))
    if not all_data:
        return None
    sorted_vids = sorted(all_vids)
    time_grid = sorted(all_times) if time_grid_ref is None else list(time_grid_ref)
    matrices = [build_matrix_sorted_vids(d, time_grid, sorted_vids) for d in all_data]
    return time_grid, sorted_vids, np.nanmean(np.stack(matrices, axis=0), axis=0)


def load_sinr_scenario_averaged_padded(
    scenario_dir: str,
    time_grid_ref: list[float] | None,
    n_rows: int,
) -> tuple[list[float], np.ndarray] | None:
    run_dirs = run_directories_for_scenario(scenario_dir, "sinr_V*.csv")
    if not run_dirs:
        return None
    all_times: set[float] = set()
    all_data: list[dict[int, list[tuple[float, float]]]] = []
    for run_dir in run_dirs:
        d = load_sinr_run(run_dir)
        if d:
            all_data.append(d)
            for rows in d.values():
                for t, _ in rows:
                    all_times.add(round(t, 3))
    if not all_data:
        return None
    time_grid = sorted(all_times) if time_grid_ref is None else list(time_grid_ref)
    matrices = [build_padded_matrix(d, time_grid, n_rows) for d in all_data]
    return time_grid, np.nanmean(np.stack(matrices, axis=0), axis=0)


def load_sinr_scenario_averaged_union_vids(
    scenario_dir: str,
    time_grid_ref: list[float] | None,
) -> tuple[list[float], list[int], np.ndarray] | None:
    run_dirs = run_directories_for_scenario(scenario_dir, "sinr_V*.csv")
    if not run_dirs:
        return None
    all_times: set[float] = set()
    all_vids: set[int] = set()
    all_data: list[dict[int, list[tuple[float, float]]]] = []
    for run_dir in run_dirs:
        d = load_sinr_run(run_dir)
        if d:
            all_data.append(d)
            all_vids.update(d.keys())
            for rows in d.values():
                for t, _ in rows:
                    all_times.add(round(t, 3))
    if not all_data:
        return None
    sorted_vids = sorted(all_vids)
    time_grid = sorted(all_times) if time_grid_ref is None else list(time_grid_ref)
    matrices = [build_matrix_sorted_vids(d, time_grid, sorted_vids) for d in all_data]
    return time_grid, sorted_vids, np.nanmean(np.stack(matrices, axis=0), axis=0)


def infer_n_rows_channel(
    benchmark_root: str,
    scenario_names: Iterable[str],
    cap: int = 64,
) -> int:
    m = -1
    for s in scenario_names:
        for run_dir in run_directories_for_scenario(
            os.path.join(benchmark_root, s), "channel_V*.csv"
        ):
            m = max(m, max_vehicle_id(load_channel_run(run_dir)))
    if m < 0:
        return 1
    return min(cap, m + 1)


def infer_n_rows_sinr(
    benchmark_root: str,
    scenario_names: Iterable[str],
    cap: int = 64,
) -> int:
    m = -1
    for s in scenario_names:
        for run_dir in run_directories_for_scenario(
            os.path.join(benchmark_root, s), "sinr_V*.csv"
        ):
            m = max(m, max_vehicle_id(load_sinr_run(run_dir)))
    if m < 0:
        return 1
    return min(cap, m + 1)


TIME_GRID_STEP = 0.1

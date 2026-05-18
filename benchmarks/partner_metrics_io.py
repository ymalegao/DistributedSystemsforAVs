"""
Partner (RAFT) metrics loaders for apples-to-apples overlay plots.

Hardcoded partner layout (from /home/yash/partnersv2v):
  /home/yash/partnersv2v/res/simple_raftwave_<N>veh_allVehicles/run_*/...

This module intentionally provides only the "allVehicles" scenario, since that
is the only comparable setup to our BFT runs (all vehicles participate).
"""

from __future__ import annotations

import csv
import glob
import json
import os
import warnings
from pathlib import Path
from typing import Any

import numpy as np

PARTNER_ROOT = Path("/home/yash/partnersv2v/res")
PARTNER_PROTOCOL_PREFIX = "simple_raftwave"
PARTNER_MODE = "allVehicles"


def partner_scenario_dir(vehicle_count: int, mode: str = PARTNER_MODE) -> Path:
    return PARTNER_ROOT / f"{PARTNER_PROTOCOL_PREFIX}_{vehicle_count}veh_{mode}"


def partner_run_dirs(vehicle_count: int, mode: str, file_glob: str) -> list[Path]:
    scenario = partner_scenario_dir(vehicle_count, mode)
    if not scenario.is_dir():
        return []
    runs = sorted(Path(p) for p in glob.glob(str(scenario / "run_*")))
    out: list[Path] = []
    for d in runs:
        if d.is_dir() and glob.glob(str(d / file_glob)):
            out.append(d)
    return out


def _load_csv_series(
    run_dir: Path, pattern: str, value_key: str
) -> dict[int, list[tuple[float, float]]]:
    data: dict[int, list[tuple[float, float]]] = {}
    for path in sorted(glob.glob(str(run_dir / pattern))):
        try:
            with open(path, newline="", encoding="utf-8") as f:
                reader = csv.DictReader(f)
                if not reader.fieldnames or "time_s" not in reader.fieldnames:
                    continue
                if value_key not in reader.fieldnames:
                    continue
                rows: list[tuple[float, float]] = []
                for r in reader:
                    try:
                        rows.append((float(r["time_s"]), float(r[value_key])))
                    except (ValueError, KeyError, TypeError):
                        pass
            if rows:
                base = os.path.basename(path)
                vid = int(base.split("_V", 1)[1].split(".", 1)[0])
                data[vid] = rows
        except Exception:
            # Best-effort: partner data might have truncated files.
            pass
    return data


def _build_matrix_sorted_vids(
    data: dict[int, list[tuple[float, float]]],
    time_grid: list[float],
    sorted_vids: list[int],
) -> np.ndarray:
    t_index = {round(t, 3): i for i, t in enumerate(time_grid)}
    mat = np.full((len(sorted_vids), len(time_grid)), np.nan)
    row_of = {v: i for i, v in enumerate(sorted_vids)}
    for vid, rows in data.items():
        i = row_of.get(vid)
        if i is None:
            continue
        for t, val in rows:
            k = round(t, 3)
            j = t_index.get(k)
            if j is not None:
                mat[i, j] = val
    return mat


def load_partner_channel_union_vids(
    vehicle_count: int,
    mode: str = PARTNER_MODE,
    time_grid_ref: list[float] | None = None,
) -> tuple[list[float], list[int], np.ndarray] | None:
    """Mean over runs; rows follow sorted vehicle ids present in any run."""
    run_dirs = partner_run_dirs(vehicle_count, mode, "channel_V*.csv")
    if not run_dirs:
        return None
    all_times: set[float] = set()
    all_vids: set[int] = set()
    all_runs: list[dict[int, list[tuple[float, float]]]] = []
    for rd in run_dirs:
        d = _load_csv_series(rd, "channel_V*.csv", "channel_utilization")
        if not d:
            continue
        all_runs.append(d)
        all_vids.update(d.keys())
        for rows in d.values():
            for t, _ in rows:
                all_times.add(round(t, 3))
    if not all_runs:
        return None
    sorted_vids = sorted(all_vids)
    time_grid = sorted(all_times) if time_grid_ref is None else list(time_grid_ref)
    mats = [_build_matrix_sorted_vids(d, time_grid, sorted_vids) for d in all_runs]
    with warnings.catch_warnings():
        warnings.filterwarnings("ignore", message="Mean of empty slice", category=RuntimeWarning)
        mean_mat = np.nanmean(np.stack(mats, axis=0), axis=0)
    return time_grid, sorted_vids, mean_mat


def _union_to_padded(
    sorted_vids: list[int],
    union_mat: np.ndarray,
    n_rows: int,
) -> np.ndarray:
    out = np.full((n_rows, union_mat.shape[1]), np.nan)
    for i, vid in enumerate(sorted_vids):
        if 0 <= vid < n_rows:
            out[vid, :] = union_mat[i, :]
    return out


def load_partner_channel_padded(
    vehicle_count: int,
    time_grid_ref: list[float] | None,
    n_rows: int,
    mode: str = PARTNER_MODE,
) -> tuple[list[float], np.ndarray] | None:
    got = load_partner_channel_union_vids(vehicle_count, mode=mode, time_grid_ref=time_grid_ref)
    if got is None:
        return None
    tg, vids, mat = got
    return tg, _union_to_padded(vids, mat, n_rows)


def load_partner_sinr_union_vids(
    vehicle_count: int,
    mode: str = PARTNER_MODE,
    time_grid_ref: list[float] | None = None,
) -> tuple[list[float], list[int], np.ndarray] | None:
    """Mean over runs; rows follow sorted vehicle ids present in any run."""
    run_dirs = partner_run_dirs(vehicle_count, mode, "sinr_V*.csv")
    if not run_dirs:
        return None
    all_times: set[float] = set()
    all_vids: set[int] = set()
    all_runs: list[dict[int, list[tuple[float, float]]]] = []
    for rd in run_dirs:
        d = _load_csv_series(rd, "sinr_V*.csv", "sinr_db_mean")
        if not d:
            continue
        all_runs.append(d)
        all_vids.update(d.keys())
        for rows in d.values():
            for t, _ in rows:
                all_times.add(round(t, 3))
    if not all_runs:
        return None
    sorted_vids = sorted(all_vids)
    time_grid = sorted(all_times) if time_grid_ref is None else list(time_grid_ref)
    mats = [_build_matrix_sorted_vids(d, time_grid, sorted_vids) for d in all_runs]
    with warnings.catch_warnings():
        warnings.filterwarnings("ignore", message="Mean of empty slice", category=RuntimeWarning)
        mean_mat = np.nanmean(np.stack(mats, axis=0), axis=0)
    return time_grid, sorted_vids, mean_mat


def load_partner_sinr_padded(
    vehicle_count: int,
    time_grid_ref: list[float] | None,
    n_rows: int,
    mode: str = PARTNER_MODE,
) -> tuple[list[float], np.ndarray] | None:
    got = load_partner_sinr_union_vids(vehicle_count, mode=mode, time_grid_ref=time_grid_ref)
    if got is None:
        return None
    tg, vids, mat = got
    return tg, _union_to_padded(vids, mat, n_rows)


def _read_json_tolerant(path: Path) -> Any:
    """Partner simulator may truncate files; try to salvage a list JSON."""
    raw = path.read_text(encoding="utf-8", errors="ignore")
    try:
        return json.loads(raw)
    except json.JSONDecodeError:
        s = raw.rstrip()
        if not s:
            raise
        if not s.endswith("]"):
            # Common truncation: missing closing bracket.
            s2 = s.rstrip(", \n\t")
            if s2.endswith("}"):
                s2 = s2 + "\n]"
            else:
                s2 = s2 + "]"
            return json.loads(s2)
        raise


def iter_partner_raft_runs(
    vehicle_count: int,
    mode: str = PARTNER_MODE,
) -> list[list[dict[str, Any]]]:
    """Return list of runs, each run is a list of per-vehicle dicts."""
    run_dirs = partner_run_dirs(vehicle_count, mode, "raft_results.json")
    out: list[list[dict[str, Any]]] = []
    for rd in run_dirs:
        p = rd / "raft_results.json"
        if not p.exists():
            continue
        try:
            data = _read_json_tolerant(p)
            if isinstance(data, list):
                out.append([v for v in data if isinstance(v, dict)])
        except Exception:
            pass
    return out


def partner_wait_time_samples_s(
    vehicle_count: int,
    mode: str = PARTNER_MODE,
    include_fallback: bool = False,
) -> list[float]:
    """Pool per-vehicle total wait samples (seconds) across all partner runs."""
    pooled: list[float] = []
    for run in iter_partner_raft_runs(vehicle_count, mode):
        for v in run:
            if not include_fallback and v.get("coordination_method") == "fallback":
                continue
            durs = v.get("durations_ms") if isinstance(v.get("durations_ms"), dict) else {}
            wt = durs.get("total_wait_time")
            try:
                ms = float(wt)
            except (TypeError, ValueError):
                continue
            if ms > 0:
                pooled.append(ms / 1000.0)
    return pooled


def partner_run_metrics(vehicle_count: int, mode: str = PARTNER_MODE) -> list[dict[str, float]]:
    """
    Per-run metrics matching the subset we overlay:
      throughput_veh_per_s, fallback_rate, messages_sent, messages_received
    """
    out: list[dict[str, float]] = []
    for run in iter_partner_raft_runs(vehicle_count, mode):
        if not run:
            continue
        num_veh = len(run)
        raft_veh = [v for v in run if v.get("coordination_method") != "fallback"]

        # Throughput definition mirrors partner plot_comparison: raft vehicles only.
        wait_ms: list[float] = []
        for v in raft_veh:
            ts = v.get("timestamps_ms") if isinstance(v.get("timestamps_ms"), dict) else {}
            stopped = ts.get("stopped", 0)
            passed = ts.get("passed", 0)
            try:
                stopped_f = float(stopped)
                passed_f = float(passed)
            except (TypeError, ValueError):
                continue
            if stopped_f > 0 and passed_f > 0 and passed_f >= stopped_f:
                wait_ms.append(passed_f - stopped_f)
        throughput = 0.0
        if wait_ms:
            avg_wait_ms = float(np.mean(np.array(wait_ms, dtype=float)))
            if avg_wait_ms > 0:
                throughput = float(len(raft_veh)) / (avg_wait_ms / 1000.0)

        fallback_rate = (sum(1 for v in run if v.get("coordination_method") == "fallback") / num_veh) if num_veh else 0.0

        total_sent = 0.0
        total_recv = 0.0
        for v in run:
            msgs = v.get("messages") if isinstance(v.get("messages"), dict) else {}
            try:
                total_sent += float(msgs.get("sent", 0) or 0)
            except (TypeError, ValueError):
                pass
            try:
                total_recv += float(msgs.get("received", 0) or 0)
            except (TypeError, ValueError):
                pass

        out.append(
            {
                "throughput_veh_per_s": throughput,
                "fallback_rate": float(fallback_rate),
                "messages_sent": float(total_sent),
                "messages_received": float(total_recv),
            }
        )
    return out

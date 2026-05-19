#!/usr/bin/env python3
"""
Plots from analyze_log metrics JSON (per_car + overall):

1. CDF — 2×2 panels for n ∈ {4,8,12,16}; each panel shows ECDFs for every scenario that
   has JSON for that n. ``wait_intersection_s``; ★ = priority vehicle when role=all.
   Use ``--scenario`` for a single-panel legacy CDF instead.
2. Bar charts — same 2×2 layout: x = scenario, one bar per scenario per panel.
   Throughput, fallback, and three PNGs for messages (``bft_messages_sent.png``, etc.).
3. Ambulance vs normal — 2×2 panels for mean ``wait_intersection_s`` per role.

Edit ``DEFAULT_BAR_SERIES`` (paths per n). Panel order: top-left n=4, top-right n=8,
bottom-left n=12, bottom-right n=16.
"""

from __future__ import annotations

import argparse
import glob
import json
import re
import statistics
import sys
from pathlib import Path

import numpy as np

try:
    import matplotlib.pyplot as plt
    from matplotlib.lines import Line2D
    from matplotlib.patches import Patch
except ImportError:
    plt = None
    Line2D = None  # type: ignore[misc, assignment]
    Patch = None  # type: ignore[misc, assignment]

# -----------------------------------------------------------------------------
# Legacy single-panel CDF only when using --scenario (list of label, paths, role).
# -----------------------------------------------------------------------------
DEFAULT_SCENARIOS: list[tuple[str, list[str], str]] = []

# -----------------------------------------------------------------------------
# Bar charts + default 2×2 CDF: globs pool all runs from analyze_log --save-to + --scenario.
# Subdirs: no_amb, amb_honest, amb_byz_follower, amb_byz_leader (see analyze_log.py).
# Each list entry: globs first (pooled runs), then optional legacy single-file fallback
# if the subfolder is still empty. Remove legacy files once subdirs are populated to avoid
# double-counting the same run.
# -----------------------------------------------------------------------------
DEFAULT_BAR_SERIES: list[tuple[str, dict[int, list[str]]]] = [
    (
        "No ambulance (all regular)",
        {
            4: [
                "Priority4cars/no_amb/run_*/4veh_*.json",
                "Priority4cars/Normal.json",
            ],
            8: [
                "Priority8cars/no_amb/run_*/8veh_*.json",
                "Priority8cars/Normal.json",
            ],
            12: [
                "Priority12cars/no_amb/run_*/12veh_*.json",
                "Priority12cars/12Normal_1.json",
            ],
            16: [
                "Priority16cars/no_amb/run_*/16veh_*.json",
                "Priority16cars/Normal.json",
            ],
            20: [
                "Priority20cars/no_amb/run_*/20veh_*.json",
            ],
        },
    ),
    (
        "Ambulance (honest)",
        {
            4: [
                "Priority4cars/amb_honest/run_*/4veh_*.json",
            ],
            8: [
                "Priority8cars/amb_honest/run_*/8veh_*.json",
            ],
            12: [
                "Priority12cars/amb_honest/run_*/12veh_*.json",
            ],
            16: [
                "Priority16cars/amb_honest/run_*/16veh_*.json",
            ],
            20: [
                "Priority20cars/amb_honest/run_*/20veh_*.json",
            ],
        },
    ),
    (
        "Ambulance + Byzantine followers",
        {
            4: [
                "Priority4cars/amb_byz_follower/run_*/4veh_*.json",
            ],
            8: [
                "Priority8cars/amb_byz_follower/run_*/8veh_*.json",
            ],
            12: [
                "Priority12cars/amb_byz_follower/run_*/12veh_*.json",
            ],
            16: [
                "Priority16cars/amb_byz_follower/run_*/16veh_*.json",
            ],
            20: [
                "Priority20cars/amb_byz_follower/run_*/20veh_*.json",
            ],
        },
    ),
    (
        "Ambulance + Byzantine leader",
        {
            4: [
                "Priority4cars/amb_byz_leader/run_*/4veh_*.json",
            ],
            8: [
                "Priority8cars/amb_byz_leader/run_*/8veh_*.json",
            ],
            12: [
                "Priority12cars/amb_byz_leader/run_*/12veh_*.json",
            ],
            16: [
                "Priority16cars/amb_byz_leader/run_*/16veh_*.json",
            ],
            20: [
                "Priority20cars/amb_byz_leader/run_*/20veh_*.json",
            ],
        },
    ),
]

# Fixed panel order for 2×2 grids: top row n=4, n=8; bottom row n=16, n=20
# (Partner does not include n=12; keep apples-to-apples by default.)
GRID_VEHICLE_COUNTS = [4, 8, 16, 20]

# Bar-chart / inference: all fleet sizes referenced above
DEFAULT_VEHICLE_COUNTS = list(GRID_VEHICLE_COUNTS)

DEFAULT_COLORS = [
    "#1f77b4",
    "#ff7f0e",
    "#2ca02c",
    "#d62728",
    "#9467bd",
    "#8c564b",
    "#e377c2",
    "#7f7f7f",
]
DEFAULT_LINESTYLES = ["-", "--", "-.", ":", (0, (3, 1, 1, 1)), (0, (5, 2)), "-", "--"]

BFT_BAR_SUPTITLE = (
    "BFT-SMaRt intersection coordination (metrics from analyze_log JSON)"
)

# Partner-style colors: ambulance vs normal (mean wait bars)
AMB_BAR_COLOR = "#e74c3c"
NORM_BAR_COLOR = "#95a5a6"
NO_PRIO_BAR_COLOR = "#7f8c8d"

# Partner overlay (RAFT allVehicles scenario only)
from partner_metrics_io import partner_run_metrics, partner_wait_time_samples_s

PARTNER_LABEL = "RAFT allVehicles (partner)"


def _resolve_paths(patterns: list[str], base_dir: Path) -> list[Path]:
    out: list[Path] = []
    for p in patterns:
        raw = Path(p)
        if not raw.is_absolute():
            raw = base_dir / raw
        if any(ch in p for ch in "*?["):
            matches = sorted(glob.glob(str(raw), recursive=True))
            out.extend(Path(m) for m in matches)
        else:
            if raw.exists():
                out.append(raw)
    seen = set()
    uniq: list[Path] = []
    for q in out:
        r = q.resolve()
        if r not in seen:
            seen.add(r)
            uniq.append(q)
    return uniq


def load_wait_samples(
    json_path: Path,
    role_filter: str,
    exclude_fallback: bool,
) -> list[float]:
    with open(json_path, encoding="utf-8") as f:
        data = json.load(f)
    # New format: list[per-vehicle dict] (partner-like).
    # Legacy format: {"per_car": [...], "overall": {...}}
    cars: list[dict] = []
    if isinstance(data, list):
        cars = [v for v in data if isinstance(v, dict)]
    elif isinstance(data, dict):
        raw = data.get("per_car")
        if isinstance(raw, list):
            cars = [v for v in raw if isinstance(v, dict)]
    if not cars:
        return []

    waits: list[float] = []
    for v in cars:
        if exclude_fallback:
            used_fb = v.get("used_fallback")
            if used_fb is None and isinstance(v.get("bft_stats"), dict):
                used_fb = v["bft_stats"].get("used_fallback")
            if used_fb is None:
                used_fb = (v.get("coordination_method") == "fallback")
            if used_fb is True:
                continue
        # Role / priority filtering.
        is_prio = bool(v.get("is_priority_vehicle", False))
        role = v.get("role")
        if isinstance(role, str):
            role_s = role
        else:
            role_s = "ambulance" if is_prio else "normal"
        if role_filter == "normal" and role_s != "normal":
            continue
        if role_filter == "ambulance" and role_s != "ambulance":
            continue

        wt_s: float | None = None
        if "wait_intersection_s" in v:
            try:
                wt_s = float(v.get("wait_intersection_s"))
            except (TypeError, ValueError):
                wt_s = None
        if wt_s is None and isinstance(v.get("durations_ms"), dict):
            try:
                ms = float(v["durations_ms"].get("total_wait_time"))
                wt_s = ms / 1000.0
            except (TypeError, ValueError):
                wt_s = None
        if wt_s is None and isinstance(v.get("timestamps_ms"), dict):
            try:
                stopped = float(v["timestamps_ms"].get("stopped", 0) or 0)
                passed = float(v["timestamps_ms"].get("passed", 0) or 0)
                if passed > 0 and stopped > 0 and passed >= stopped:
                    wt_s = (passed - stopped) / 1000.0
            except (TypeError, ValueError):
                wt_s = None
        if wt_s is not None and wt_s > 0:
            waits.append(float(wt_s))
    return waits


def scenario_waits(
    patterns: list[str],
    base_dir: Path,
    role_filter: str,
    exclude_fallback: bool,
) -> list[float]:
    paths = _resolve_paths(patterns, base_dir)
    pooled: list[float] = []
    for jp in paths:
        pooled.extend(
            load_wait_samples(jp, role_filter, exclude_fallback=exclude_fallback)
        )
    return pooled


def scenario_ambulance_waits(
    patterns: list[str],
    base_dir: Path,
    exclude_fallback: bool,
) -> list[float]:
    """Priority-vehicle waits only (for markers when plotting role=all)."""
    paths = _resolve_paths(patterns, base_dir)
    pooled: list[float] = []
    for jp in paths:
        pooled.extend(
            load_wait_samples(jp, "ambulance", exclude_fallback=exclude_fallback)
        )
    return pooled


def ecdf_value_at(sorted_waits: np.ndarray, w: float) -> float:
    """Empirical F(w) = fraction of samples with wait <= w."""
    if sorted_waits.size == 0:
        return float("nan")
    return float(np.searchsorted(sorted_waits, w, side="right")) / float(
        sorted_waits.size
    )


def pooled_waits_by_role(
    patterns: list[str],
    base_dir: Path,
    exclude_fallback: bool,
) -> tuple[list[float], list[float]]:
    """
    Pool wait_intersection_s across all resolved JSON paths, split by role.
    Same definition as CDF (stop → depart).
    """
    amb: list[float] = []
    norm: list[float] = []
    for path in _resolve_paths(patterns, base_dir):
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
        cars: list[dict] = []
        if isinstance(data, list):
            cars = [v for v in data if isinstance(v, dict)]
        elif isinstance(data, dict):
            raw = data.get("per_car")
            if isinstance(raw, list):
                cars = [v for v in raw if isinstance(v, dict)]
        if not cars:
            continue
        for c in cars:
            if not isinstance(c, dict):
                continue
            if exclude_fallback:
                used_fb = c.get("used_fallback")
                if used_fb is None and isinstance(c.get("bft_stats"), dict):
                    used_fb = c["bft_stats"].get("used_fallback")
                if used_fb is None:
                    used_fb = (c.get("coordination_method") == "fallback")
                if used_fb is True:
                    continue
            # load_wait_samples already role-filters; split here using is_priority_vehicle/role.
            role = c.get("role")
            if not isinstance(role, str):
                role = "ambulance" if bool(c.get("is_priority_vehicle", False)) else "normal"
            # Find this vehicle's wait time to assign to a bucket.
            wt_s = None
            if "wait_intersection_s" in c:
                try:
                    wt_s = float(c.get("wait_intersection_s"))
                except (TypeError, ValueError):
                    wt_s = None
            if wt_s is None and isinstance(c.get("durations_ms"), dict):
                try:
                    wt_s = float(c["durations_ms"].get("total_wait_time")) / 1000.0
                except (TypeError, ValueError):
                    wt_s = None
            if wt_s is None and isinstance(c.get("timestamps_ms"), dict):
                try:
                    stopped = float(c["timestamps_ms"].get("stopped", 0) or 0)
                    passed = float(c["timestamps_ms"].get("passed", 0) or 0)
                    if passed > 0 and stopped > 0 and passed >= stopped:
                        wt_s = (passed - stopped) / 1000.0
                except (TypeError, ValueError):
                    wt_s = None
            if wt_s is None or wt_s <= 0:
                continue
            if role == "ambulance":
                amb.append(float(wt_s))
            elif role == "normal":
                norm.append(float(wt_s))
    return amb, norm


def mean_or_none(vals: list[float]) -> float | None:
    return statistics.mean(vals) if vals else None


def role_for_scenario_cdf(label: str) -> str:
    """Role filter for CDF when splitting by scenario label."""
    if "no ambulance" in label.lower():
        return "normal"
    return "all"


def short_cdf_legend_label(label: str) -> str:
    return label.replace(" (all regular)", "").strip()


def ecdf_line(y: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Partner-identical: one (x,y) per sample — sorted wait vs i/n (ties → vertical segments)."""
    y = np.sort(np.asarray(y, dtype=float))
    n = len(y)
    if n == 0:
        return y, np.array([])
    cdf = np.arange(1, n + 1, dtype=float) / n
    return y, cdf


def ecdf_binned_smooth(
    y: np.ndarray,
    n_bins: int,
    blur_radius: int,
) -> tuple[np.ndarray, np.ndarray]:
    """
    Smoother *display* curve: histogram of waits, cumulative proportion, then moving-average
    blur with re-normalization and max-accumulate so CDF stays monotone. Not identical to the
    empirical CDF at every point — use for figures only; statistics should use ecdf_line.
    """
    y = np.asarray(y, dtype=float)
    n = len(y)
    if n == 0:
        return np.array([]), np.array([])
    lo, hi = float(np.min(y)), float(np.max(y))
    if hi <= lo:
        return np.array([lo, hi]), np.array([0.0, 1.0])
    nb = max(8, min(n_bins, max(20, n // 2)))
    hist, edges = np.histogram(y, bins=nb, range=(lo, hi), density=False)
    cdf = np.cumsum(hist) / float(n)
    x = edges[1:]
    if blur_radius > 0 and len(cdf) > 2 * blur_radius + 1:
        k = 2 * blur_radius + 1
        kernel = np.ones(k, dtype=float) / float(k)
        pad = blur_radius
        cdf_p = np.pad(cdf, (pad, pad), mode="edge")
        cdf = np.convolve(cdf_p, kernel, mode="valid")
    cdf = np.maximum.accumulate(cdf)
    mx = float(cdf[-1]) if len(cdf) else 1.0
    if mx > 1e-12:
        cdf = cdf / mx
    cdf = np.clip(cdf, 0.0, 1.0)
    return x, cdf


# --- Bar chart: extract per-run scalars from metrics JSON ---------------------


def _overall_stat_mean(overall: dict, key: str) -> float | None:
    """Mean from analyze_log ``_stat()`` dict, or scalar float, or None."""
    raw = overall.get(key)
    if isinstance(raw, dict):
        v = raw.get("mean")
        try:
            return float(v) if v is not None else None
        except (TypeError, ValueError):
            return None
    if raw is not None:
        try:
            return float(raw)
        except (TypeError, ValueError):
            return None
    return None


def _format_bft_latency_ms(y_ms: float) -> str:
    """Compact label for BFT latency points (ms, sim time)."""
    if y_ms >= 10_000:
        return f"{y_ms / 1000:.1f}s"
    if y_ms >= 1000:
        return f"{y_ms / 1000:.2f}s"
    if y_ms >= 100:
        return f"{y_ms:.0f}"
    if y_ms >= 10:
        return f"{y_ms:.1f}"
    return f"{y_ms:.2f}"


def _files_by_vc_includes_path_marker(
    files_by_vc: dict[int, list[str]], marker: str
) -> bool:
    """True if any glob string contains ``marker`` (e.g. ``amb_byz_leader``)."""
    m = marker.strip("/")
    for globs in files_by_vc.values():
        for pat in globs or []:
            if m in str(pat).replace("\\", "/"):
                return True
    return False


def extract_run_metrics(path: Path) -> dict[str, float | None]:
    """Map partner-style names to values from one analyze_log metrics JSON."""
    with open(path, encoding="utf-8") as f:
        data = json.load(f)
    # New format: list[per-vehicle dict] (partner-like, produced by our updated logger).
    if isinstance(data, list):
        per_car = [v for v in data if isinstance(v, dict)]
        num_veh = len(per_car)

        def used_fallback(v: dict) -> bool:
            fb = v.get("used_fallback")
            if fb is None and isinstance(v.get("bft_stats"), dict):
                fb = v["bft_stats"].get("used_fallback")
            if fb is None:
                fb = (v.get("coordination_method") == "fallback")
            return fb is True

        non_fb = [v for v in per_car if not used_fallback(v)]
        fallback_rate = (
            (sum(1 for v in per_car if used_fallback(v)) / num_veh) if num_veh else 0.0
        )

        # Throughput: same definition as partner (vehicles / average wait seconds).
        waits_s: list[float] = []
        for v in non_fb:
            ts = v.get("timestamps_ms") if isinstance(v.get("timestamps_ms"), dict) else {}
            try:
                stopped = float(ts.get("stopped", 0) or 0)
                passed = float(ts.get("passed", 0) or 0)
            except (TypeError, ValueError):
                stopped, passed = 0.0, 0.0
            if passed > 0 and stopped > 0 and passed >= stopped:
                waits_s.append((passed - stopped) / 1000.0)
                continue
            # Fallback if timestamps missing: durations_ms.total_wait_time
            if isinstance(v.get("durations_ms"), dict) and v["durations_ms"].get("total_wait_time") is not None:
                try:
                    waits_s.append(float(v["durations_ms"]["total_wait_time"]) / 1000.0)
                except (TypeError, ValueError):
                    pass
        throughput = 0.0
        if waits_s:
            avg_wait = float(np.mean(np.array([w for w in waits_s if w > 0], dtype=float)))
            if avg_wait > 0:
                throughput = float(len(non_fb)) / avg_wait

        sent = 0.0
        recv = 0.0
        for v in per_car:
            msgs = v.get("messages") if isinstance(v.get("messages"), dict) else {}
            try:
                sent += float(msgs.get("sent", 0) or 0)
            except (TypeError, ValueError):
                pass
            try:
                recv += float(msgs.get("received", 0) or 0)
            except (TypeError, ValueError):
                pass

        # "Consensus latency" proxy: mean decision latency (ms) across non-fallback vehicles.
        decision_lat_s: list[float] = []
        for v in non_fb:
            durs = v.get("durations_ms") if isinstance(v.get("durations_ms"), dict) else {}
            try:
                ms = float(durs.get("decision_latency_ms", 0) or 0)
            except (TypeError, ValueError):
                continue
            if ms > 0:
                decision_lat_s.append(ms / 1000.0)

        consensus_latency_sim_s = float(np.mean(np.array(decision_lat_s, dtype=float))) if decision_lat_s else None

        # "First submit → commit" proxy for byzantine-leader/view-change cases:
        # use bft_stats.full_decision_latency_ms when present.
        full_decision_lat_s: list[float] = []
        for v in non_fb:
            bs = v.get("bft_stats") if isinstance(v.get("bft_stats"), dict) else {}
            try:
                ms = float(bs.get("full_decision_latency_ms", 0) or 0)
            except (TypeError, ValueError):
                continue
            if ms > 0:
                full_decision_lat_s.append(ms / 1000.0)
        first_submit_to_commit_latency_sim_s = (
            float(np.mean(np.array(full_decision_lat_s, dtype=float))) if full_decision_lat_s else None
        )

        return {
            "throughput": float(throughput),
            "fallback_rate": float(fallback_rate),
            "messages_sent": float(sent),
            "messages_received": float(recv),
            "estimated_loss_rate": None,
            "consensus_latency_sim_s": consensus_latency_sim_s,
            "first_submit_to_commit_latency_sim_s": first_submit_to_commit_latency_sim_s,
        }

    # Legacy format: {"overall": {...}, "per_car": [...]}
    o = data.get("overall") or {}
    per_car = data.get("per_car") if isinstance(data.get("per_car"), list) else []

    sent = 0
    recv = 0
    for c in per_car:
        if not isinstance(c, dict):
            continue
        s = c.get("messages_sent")
        r = c.get("messages_received")
        if s is not None:
            try:
                sent += int(s)
            except (TypeError, ValueError):
                pass
        if r is not None:
            try:
                recv += int(r)
            except (TypeError, ValueError):
                pass

    def fget(key: str) -> float | None:
        v = o.get(key)
        if v is None:
            return None
        try:
            return float(v)
        except (TypeError, ValueError):
            return None

    # propose_all_consensus_latency_sim_s: per-round mean (sim time).
    # propose_all_first_submit_to_commit_latency_sim_s: first submit→order commit
    # (analyze_log: ProposeAll_FirstSubmit_To_OrderCommit_Sim), incl. failed primary / VC.
    consensus_latency_sim_s = _overall_stat_mean(o, "propose_all_consensus_latency_sim_s")
    first_submit_to_commit_latency_sim_s = _overall_stat_mean(
        o, "propose_all_first_submit_to_commit_latency_sim_s"
    )

    return {
        "throughput": fget("throughput_veh_per_s"),
        "fallback_rate": fget("fallback_rate"),
        "messages_sent": float(sent),
        "messages_received": float(recv),
        "estimated_loss_rate": fget("estimated_loss_rate"),
        "consensus_latency_sim_s": consensus_latency_sim_s,
        "first_submit_to_commit_latency_sim_s": first_submit_to_commit_latency_sim_s,
    }


def mean_std(values: list[float | None]) -> tuple[float, float]:
    vals = [float(v) for v in values if v is not None]
    if not vals:
        return 0.0, 0.0
    if len(vals) == 1:
        return vals[0], 0.0
    a = np.array(vals, dtype=float)
    return float(np.mean(a)), float(np.std(a, ddof=1))


def load_config(path: Path) -> tuple[
    list[tuple[str, list[str], str]],
    list[tuple[str, dict[int, list[str]]]] | None,
    list[int] | None,
    str | None,
]:
    with open(path, encoding="utf-8") as f:
        cfg = json.load(f)

    scenarios = cfg.get("scenarios")
    if not isinstance(scenarios, list):
        raise ValueError("config must contain a 'scenarios' array")
    scen_out: list[tuple[str, list[str], str]] = []
    for s in scenarios:
        if not isinstance(s, dict):
            continue
        label = s.get("label") or s.get("id") or "scenario"
        files = s.get("files") or s.get("paths") or []
        if not isinstance(files, list):
            files = [str(files)]
        rf = (s.get("role_filter") or s.get("role") or "all").lower()
        if rf not in ("all", "normal", "ambulance"):
            rf = "all"
        scen_out.append((str(label), [str(x) for x in files], rf))

    bar_series = None
    vehicle_counts = None
    bar_title_note = None
    bc = cfg.get("bar_charts")
    if isinstance(bc, dict):
        vehicle_counts = bc.get("vehicle_counts")
        if vehicle_counts is not None and not isinstance(vehicle_counts, list):
            vehicle_counts = None
        bar_title_note = bc.get("title_note")
        if bar_title_note is not None:
            bar_title_note = str(bar_title_note)
        raw_series = bc.get("series")
        if isinstance(raw_series, list):
            bar_series = []
            for s in raw_series:
                if not isinstance(s, dict):
                    continue
                lab = str(s.get("label") or s.get("id") or "series")
                fbc = s.get("files_by_vc") or s.get("files_by_vehicle_count") or {}
                if not isinstance(fbc, dict):
                    continue
                d: dict[int, list[str]] = {}
                for k, v in fbc.items():
                    try:
                        vc = int(k)
                    except (TypeError, ValueError):
                        continue
                    if isinstance(v, list):
                        d[vc] = [str(x) for x in v]
                    elif v:
                        d[vc] = [str(v)]
                bar_series.append((lab, d))

    return scen_out, bar_series, vehicle_counts, bar_title_note


def derive_bar_series_from_scenarios(
    scenarios: list[tuple[str, list[str], str]],
) -> list[tuple[str, dict[int, list[str]]]]:
    """Map CDF scenario paths to {vehicle_count: [json paths]} using e.g. 16veh_0.json."""
    out: list[tuple[str, dict[int, list[str]]]] = []
    default_vc = 16
    for label, files, _rf in scenarios:
        if not files:
            out.append((label, {}))
            continue
        vc_map: dict[int, list[str]] = {}
        for pat in files:
            m = re.search(r"(\d+)\s*veh", pat, re.I)
            vc = int(m.group(1)) if m else default_vc
            vc_map.setdefault(vc, []).append(pat)
        out.append((label, vc_map))
    return out


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="CDF + bar charts from analyze_log metrics JSON."
    )
    p.add_argument(
        "--base-dir",
        type=Path,
        default=None,
        help="Resolve relative paths here (default: directory of this script)",
    )
    p.add_argument(
        "--config",
        type=Path,
        default=None,
        help="JSON with scenarios[]; optional bar_charts { vehicle_counts, series[], title_note }",
    )
    p.add_argument(
        "--scenario",
        action="append",
        default=None,
        metavar="SPEC",
        help='CDF only. Repeatable. "Label|file1.json,file2.json|all"',
    )
    p.add_argument(
        "--include-fallback",
        action="store_true",
        help="CDF: include used_fallback=true vehicles",
    )
    p.add_argument(
        "--plots",
        type=str,
        default="all",
        help="Comma-separated: cdf, bars, amb, all (default: all = cdf+bars+amb)",
    )
    p.add_argument("-o", "--output", type=Path, default=None, help="CDF output PNG (pdf sibling)")
    p.add_argument(
        "--output-dir",
        type=Path,
        default=None,
        help="Directory for bar-chart PNG/PDF (default: same folder as CDF)",
    )
    p.add_argument("--title", type=str, default=None, help="CDF figure suptitle override")
    p.add_argument("--bar-prefix", type=str, default="bft_", help="Filename prefix for bar charts")
    p.add_argument(
        "--cdf-no-ambulance-marker",
        action="store_true",
        help="CDF: do not plot ★ at the ambulance wait when role_filter is 'all'",
    )
    p.add_argument(
        "--cdf-smooth",
        action="store_true",
        help="CDF: smoother display curve (histogram + blurred cumulative). "
        "Default CDF matches RAFT partner code (sort + i/n + plot); "
        "simulation ties still look 'steppy' without this flag.",
    )
    p.add_argument(
        "--cdf-smooth-bins",
        type=int,
        default=56,
        help="Histogram bins for --cdf-smooth (default: 56)",
    )
    p.add_argument(
        "--cdf-smooth-blur",
        type=int,
        default=2,
        help="Moving-average radius on binned CDF for --cdf-smooth (default: 2)",
    )
    p.add_argument(
        "--partner-data",
        action="store_true",
        help="Overlay RAFT partner data (allVehicles) from /home/yash/partnersv2v/res.",
    )
    p.add_argument("--no-show", action="store_true", help="Do not plt.show()")
    return p.parse_args()


def parse_scenario_spec(spec: str) -> tuple[str, list[str], str]:
    parts = spec.split("|")
    if len(parts) < 2:
        raise ValueError(
            f'Invalid --scenario "{spec}". Use Label|file1.json,file2.json|all'
        )
    label = parts[0].strip()
    files = [x.strip() for x in parts[1].split(",") if x.strip()]
    rf = parts[2].strip().lower() if len(parts) > 2 else "all"
    if rf not in ("all", "normal", "ambulance"):
        rf = "all"
    return label, files, rf


def _cdf_xy_from_waits(
    waits: list[float],
    use_smooth: bool,
    smooth_bins: int,
    smooth_blur: int,
) -> tuple[np.ndarray, np.ndarray]:
    arr = np.asarray(waits, dtype=float)
    if use_smooth:
        return ecdf_binned_smooth(arr, smooth_bins, smooth_blur)
    return ecdf_line(arr)


def plot_cdf(
    series: list[
        tuple[
            str,
            np.ndarray,
            np.ndarray,
            int,
            list[float] | None,
            np.ndarray | None,
        ]
    ],
    out: Path,
    title: str | None,
    cdf_smooth_note: str = "",
) -> None:
    assert plt is not None
    ncurves = len(series)
    figsize = (11, 6.8) if ncurves > 6 else (9, 5.5)
    fig, ax = plt.subplots(figsize=figsize)
    any_amb_marker = False
    for i, tup in enumerate(series):
        label, x, y, n, amb_waits = tup[:5]
        sorted_for_star = tup[5] if len(tup) > 5 else None
        color = DEFAULT_COLORS[i % len(DEFAULT_COLORS)]
        ls = DEFAULT_LINESTYLES[i % len(DEFAULT_LINESTYLES)]
        ax.plot(
            x,
            y,
            color=color,
            linestyle=ls,
            linewidth=2,
            label=f"{label} (n={n})",
        )
        if amb_waits:
            base = (
                sorted_for_star
                if sorted_for_star is not None and sorted_for_star.size
                else np.sort(np.asarray(x, dtype=float))
            )
            for w in amb_waits:
                if w is None or w <= 0:
                    continue
                fy = ecdf_value_at(base, float(w))
                if np.isnan(fy):
                    continue
                ax.scatter(
                    [float(w)],
                    [fy],
                    color=color,
                    s=120,
                    marker="*",
                    zorder=6,
                    edgecolors="black",
                    linewidths=0.6,
                    label="_nolegend_",
                )
                any_amb_marker = True
    ax.set_xlabel("Total wait time (s)", fontsize=11)
    ax.set_ylabel("CDF", fontsize=11)
    ax.set_ylim(0, 1.05)
    ax.set_xlim(left=0)
    ax.grid(True, alpha=0.3)
    handles, leg_labels = ax.get_legend_handles_labels()
    if any_amb_marker and Line2D is not None:
        star = Line2D(
            [0],
            [0],
            linestyle="None",
            marker="*",
            color="gold",
            markeredgecolor="black",
            markeredgewidth=0.6,
            markersize=14,
            label="Ambulance wait (priority vehicle)",
        )
        ax.legend(handles + [star], leg_labels + [star.get_label()], fontsize=9, loc="lower right")
    else:
        ax.legend(fontsize=9, loc="lower right")
    suptitle = title or (
        "CDF of total wait time per vehicle (BFT intersection)\n"
        "Stop at intersection → depart; fallback vehicles excluded"
        + (
            "\n★ = ambulance wait on that scenario’s curve (role=all)"
            if any_amb_marker
            else ""
        )
        + cdf_smooth_note
    )
    fig.suptitle(suptitle, fontsize=12, fontweight="bold")
    fig.tight_layout()
    out.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out, dpi=150, bbox_inches="tight")
    fig.savefig(out.with_suffix(".pdf"), bbox_inches="tight")
    print(f"Wrote {out}")
    print(f"Wrote {out.with_suffix('.pdf')}")
    plt.close(fig)


def plot_cdf_grid(
    base_dir: Path,
    bar_series: list[tuple[str, dict[int, list[str]]]],
    out: Path,
    title: str | None,
    exclude_fallback: bool,
    no_ambulance_marker: bool,
    cdf_smooth: bool,
    cdf_smooth_bins: int,
    cdf_smooth_blur: int,
    include_partner: bool,
) -> None:
    """
    One figure, 2×2 subplots: each panel is fleet size n ∈ {4,8,12,16}.
    Within each panel, one ECDF per scenario that has JSON paths for that n.
    """
    assert plt is not None
    fig, axes = plt.subplots(2, 2, figsize=(12.5, 10), sharey=True)
    axes_flat = axes.ravel()
    any_star_global = False
    scenario_list = [(lab, d) for lab, d in bar_series]

    for ax_idx, n in enumerate(GRID_VEHICLE_COUNTS):
        ax = axes_flat[ax_idx]
        curves_in_panel = 0
        for scen_i, (label, files_by_vc) in enumerate(scenario_list):
            paths_raw = files_by_vc.get(n) or []
            if not paths_raw:
                continue
            rf = role_for_scenario_cdf(label)
            waits = scenario_waits(paths_raw, base_dir, rf, exclude_fallback)
            if not waits:
                print(
                    f"CDF skip panel n={n}: {label!r} paths={paths_raw!r}",
                    file=sys.stderr,
                )
                continue
            xs, ys = _cdf_xy_from_waits(
                waits, cdf_smooth, cdf_smooth_bins, cdf_smooth_blur
            )
            color = DEFAULT_COLORS[scen_i % len(DEFAULT_COLORS)]
            ls = DEFAULT_LINESTYLES[scen_i % len(DEFAULT_LINESTYLES)]
            leg = short_cdf_legend_label(label)
            ax.plot(
                xs,
                ys,
                color=color,
                linestyle=ls,
                linewidth=2,
                label=f"{leg} (n={len(waits)})",
            )
            if rf == "all" and not no_ambulance_marker:
                aw = scenario_ambulance_waits(paths_raw, base_dir, exclude_fallback)
                if aw:
                    sorted_all = np.sort(np.asarray(waits, dtype=float))
                    for w in aw:
                        if w is None or w <= 0:
                            continue
                        fy = ecdf_value_at(sorted_all, float(w))
                        if np.isnan(fy):
                            continue
                        ax.scatter(
                            [float(w)],
                            [fy],
                            color=color,
                            s=100,
                            marker="*",
                            zorder=6,
                            edgecolors="black",
                            linewidths=0.5,
                        )
                        any_star_global = True
            curves_in_panel += 1

        if include_partner:
            waits_p = partner_wait_time_samples_s(
                n, include_fallback=(not exclude_fallback)
            )
            if waits_p:
                xs_p, ys_p = _cdf_xy_from_waits(
                    waits_p, cdf_smooth, cdf_smooth_bins, cdf_smooth_blur
                )
                ax.plot(
                    xs_p,
                    ys_p,
                    color="black",
                    linestyle="--",
                    linewidth=2.2,
                    label=f"{PARTNER_LABEL} (n={len(waits_p)})",
                )
                curves_in_panel += 1

        ax.set_title(f"n = {n} vehicles", fontsize=11, fontweight="bold")
        ax.set_xlabel("Total wait time (s)", fontsize=10)
        if ax_idx in (0, 2):
            ax.set_ylabel("CDF", fontsize=10)
        ax.set_ylim(0, 1.05)
        ax.set_xlim(left=0)
        ax.grid(True, alpha=0.3)
        if curves_in_panel:
            ax.legend(fontsize=7, loc="lower right")
        if curves_in_panel == 0:
            ax.text(
                0.5,
                0.5,
                "No JSON paths for this n",
                ha="center",
                va="center",
                transform=ax.transAxes,
                fontsize=11,
                color="gray",
            )

    smooth_extra = (
        "\n(smoothed display: binned + blurred; ★ uses exact empirical F(w))"
        if cdf_smooth
        else ""
    )
    supt = title or (
        (
            "CDF of total wait time per vehicle (BFT intersection)\n"
            "Stop → depart; one panel per fleet size n; ★ = priority vehicle when role=all"
            if any_star_global
            else "CDF of total wait time per vehicle (BFT intersection)\n"
            "Stop → depart; one panel per fleet size n"
        )
        + smooth_extra
    )
    fig.suptitle(supt, fontsize=12, fontweight="bold")
    fig.tight_layout()
    out.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out, dpi=150, bbox_inches="tight")
    fig.savefig(out.with_suffix(".pdf"), bbox_inches="tight")
    print(f"Wrote {out}")
    print(f"Wrote {out.with_suffix('.pdf')}")
    plt.close(fig)


def _bar_metric_panel(
    ax,
    n: int,
    series_keys: list[str],
    data: dict[str, dict[int, dict[str, dict[str, float]]]],
    metric: str,
    scale: float,
    text_fn,
) -> None:
    x = np.arange(len(series_keys))
    bw = 0.62
    for i, sk in enumerate(series_keys):
        cell = data.get(sk, {}).get(n, {}).get(metric, {})
        m = float(cell.get("mean", 0) or 0) * scale
        s = float(cell.get("std", 0) or 0) * scale
        ax.bar(
            x[i],
            m,
            bw,
            yerr=s,
            color=DEFAULT_COLORS[i % len(DEFAULT_COLORS)],
            alpha=0.85,
            capsize=4,
            edgecolor="black",
            linewidth=0.5,
        )
        ax.text(
            x[i],
            m,
            text_fn(m),
            ha="center",
            va="bottom",
            fontsize=7,
            fontweight="bold",
        )
    ax.set_xticks(x)
    ax.set_xticklabels(
        [short_cdf_legend_label(sk) for sk in series_keys],
        rotation=16,
        ha="right",
        fontsize=7,
    )
    ax.set_title(f"n = {n}", fontsize=10, fontweight="bold")
    ax.grid(True, alpha=0.3, axis="y")
    ax.set_ylim(bottom=0)


def plot_bars(
    base_dir: Path,
    bar_series: list[tuple[str, dict[int, list[str]]]],
    vehicle_counts: list[int],
    output_dir: Path,
    prefix: str,
    title_note: str,
    include_partner: bool,
) -> None:
    """2×2 subplots per figure: each panel is one fleet size n; x = scenario."""
    assert plt is not None
    output_dir.mkdir(parents=True, exist_ok=True)

    if not any(files_by_vc for _, files_by_vc in bar_series):
        print("Bar charts: no series with non-empty files_by_vc; skipping.", file=sys.stderr)
        return

    metrics_keys = [
        "throughput",
        "fallback_rate",
        "messages_sent",
        "messages_received",
        "estimated_loss_rate",
        "consensus_latency_sim_s",
        "first_submit_to_commit_latency_sim_s",
    ]
    data: dict[str, dict[int, dict[str, dict[str, float]]]] = {}

    for label, files_by_vc in bar_series:
        if not any(files_by_vc.values()):
            continue
        data[label] = {}
        for vc in vehicle_counts:
            paths_raw = files_by_vc.get(vc) or []
            paths = _resolve_paths(paths_raw, base_dir) if paths_raw else []
            if not paths:
                continue
            run_metrics = [extract_run_metrics(p) for p in paths]
            cell: dict[str, dict[str, float]] = {}
            for mk in metrics_keys:
                m, s = mean_std([rm.get(mk) for rm in run_metrics])
                cell[mk] = {"mean": m, "std": s}
            data[label][vc] = cell

    if not data:
        print("Bar charts: no data after loading JSONs; skipping.", file=sys.stderr)
        return

    series_keys = [lab for lab, _ in bar_series if lab in data]
    # Latency line plot: keep apples-to-apples with partner plot_comparison decisionLatency:
    # use per-vehicle decision latency (seconds) aggregated per run.
    # (We still compute first_submit_to_commit_latency_sim_s for diagnostics, but it is not used here.)
    consensus_line_metric_by_label: dict[str, str] = {lab: "consensus_latency_sim_s" for lab, _ in bar_series if lab in data}
    title_suffix = f"\n{title_note}" if title_note else ""

    def grid_axes():
        fig, axes = plt.subplots(2, 2, figsize=(12, 9))
        return fig, axes.ravel()

    # --- Throughput (line chart): x = vehicle count, line = scenario ---
    fig, ax = plt.subplots(figsize=(8.5, 5.5))
    fig.suptitle(
        f"{BFT_BAR_SUPTITLE}{title_suffix}\n"
        "System throughput by fleet size (overall.throughput_veh_per_s)",
        fontsize=11,
        fontweight="bold",
    )
    x = np.array(GRID_VEHICLE_COUNTS, dtype=float)
    has_any = False
    for i, sk in enumerate(series_keys):
        y_vals: list[float] = []
        for n in GRID_VEHICLE_COUNTS:
            cell = data.get(sk, {}).get(n, {}).get("throughput", {})
            m = float(cell.get("mean", 0) or 0)
            y_vals.append(m if m > 0 else np.nan)
        if all(np.isnan(v) for v in y_vals):
            continue
        y_arr = np.array(y_vals, dtype=float)
        mask = ~np.isnan(y_arr)
        ax.plot(
            x[mask],
            y_arr[mask],
            color=DEFAULT_COLORS[i % len(DEFAULT_COLORS)],
            linestyle=DEFAULT_LINESTYLES[i % len(DEFAULT_LINESTYLES)],
            linewidth=2,
            marker="o",
            markersize=6,
            label=short_cdf_legend_label(sk),
        )
        has_any = True

    if include_partner:
        y_p: list[float] = []
        for n in GRID_VEHICLE_COUNTS:
            runs = partner_run_metrics(n)
            vals = [rm.get("throughput_veh_per_s", 0.0) for rm in runs]
            vals_f = [float(v) for v in vals if v is not None and float(v) > 0]
            y_p.append(float(np.mean(np.array(vals_f, dtype=float))) if vals_f else np.nan)
        y_p_arr = np.array(y_p, dtype=float)
        mask_p = ~np.isnan(y_p_arr)
        if mask_p.any():
            ax.plot(
                x[mask_p],
                y_p_arr[mask_p],
                color="black",
                linestyle="--",
                linewidth=2.4,
                marker="s",
                markersize=6,
                label=PARTNER_LABEL,
            )
            has_any = True

    if has_any:
        ax.set_xlabel("Number of vehicles", fontsize=10, fontweight="bold")
        ax.set_ylabel("Vehicles / second", fontsize=10, fontweight="bold")
        ax.set_xticks(GRID_VEHICLE_COUNTS)
        ax.set_xticklabels([f"{n} veh" for n in GRID_VEHICLE_COUNTS], fontsize=9)
        ax.grid(True, alpha=0.3)
        ax.set_ylim(bottom=0)
        ax.legend(fontsize=8, loc="best")
        fig.tight_layout()
    else:
        ax.text(
            0.5,
            0.5,
            "No throughput data available",
            ha="center",
            va="center",
            transform=ax.transAxes,
            fontsize=11,
            color="gray",
        )
        fig.tight_layout()
    tp = output_dir / f"{prefix}throughput_comparison.png"
    fig.savefig(tp, dpi=150, bbox_inches="tight")
    fig.savefig(tp.with_suffix(".pdf"), bbox_inches="tight")
    print(f"Wrote {tp}")
    plt.close(fig)

    # --- Fallback (2×2) ---
    fig_fb, axes_fb = grid_axes()
    fig_fb.suptitle(
        f"{BFT_BAR_SUPTITLE}{title_suffix}\n"
        "Fallback: overall.fallback_rate (%)",
        fontsize=11,
        fontweight="bold",
    )
    for ax_idx, n in enumerate(GRID_VEHICLE_COUNTS):

        def _tf(m: float) -> str:
            return f"{m:.1f}%"

        ax = axes_fb[ax_idx]
        x = np.arange(len(series_keys))
        bw = 0.62
        for i, sk in enumerate(series_keys):
            cell = data.get(sk, {}).get(n, {}).get("fallback_rate", {})
            m = float(cell.get("mean", 0) or 0) * 100.0
            s = float(cell.get("std", 0) or 0) * 100.0
            ax.bar(
                x[i],
                m,
                bw,
                yerr=s,
                color=DEFAULT_COLORS[i % len(DEFAULT_COLORS)],
                alpha=0.85,
                capsize=4,
                edgecolor="black",
                linewidth=0.5,
            )
            if m > 0:
                ax.text(x[i], m, _tf(m), ha="center", va="bottom", fontsize=7, fontweight="bold")
        ax.set_xticks(x)
        ax.set_xticklabels(
            [short_cdf_legend_label(sk) for sk in series_keys],
            rotation=16,
            ha="right",
            fontsize=7,
        )
        ax.set_title(f"n = {n}", fontsize=10, fontweight="bold")
        ax.grid(True, alpha=0.3, axis="y")
        ax.set_ylim(bottom=0)
        if ax_idx in (0, 2):
            ax.set_ylabel("Fallback rate (%)", fontsize=9, fontweight="bold")
    fig_fb.tight_layout()
    fb = output_dir / f"{prefix}fallback_rate.png"
    fig_fb.savefig(fb, dpi=150, bbox_inches="tight")
    fig_fb.savefig(fb.with_suffix(".pdf"), bbox_inches="tight")
    print(f"Wrote {fb}")
    plt.close(fig_fb)

    # --- Messages: one 2×2 figure per metric ---
    msg_configs = [
        ("messages_sent", "Messages sent (total per run)", "Messages", 1.0),
        ("messages_received", "Messages received (total per run)", "Messages", 1.0),
        ("estimated_loss_rate", "Est. message loss (overall)", "Loss rate (%)", 100.0),
    ]
    for metric, mtitle, ylabel, scale in msg_configs:
        fig_m, axes_m = grid_axes()
        fig_m.suptitle(
            f"{BFT_BAR_SUPTITLE}{title_suffix}\n{mtitle}",
            fontsize=11,
            fontweight="bold",
        )
        for ax_idx, n in enumerate(GRID_VEHICLE_COUNTS):
            ax = axes_m[ax_idx]

            def text_fn(m: float, sc: float = scale) -> str:
                return f"{m:.1f}%" if sc == 100.0 else f"{m:.0f}"

            x = np.arange(len(series_keys))
            bw = 0.62
            for i, sk in enumerate(series_keys):
                cell = data.get(sk, {}).get(n, {}).get(metric, {})
                m = float(cell.get("mean", 0) or 0) * scale
                s = float(cell.get("std", 0) or 0) * scale
                ax.bar(
                    x[i],
                    m,
                    bw,
                    yerr=s,
                    color=DEFAULT_COLORS[i % len(DEFAULT_COLORS)],
                    alpha=0.85,
                    capsize=4,
                    edgecolor="black",
                    linewidth=0.5,
                )
                if m > 0 or (metric == "estimated_loss_rate"):
                    ax.text(
                        x[i],
                        m,
                        text_fn(m),
                        ha="center",
                        va="bottom",
                        fontsize=7,
                        fontweight="bold",
                    )
            ax.set_xticks(x)
            ax.set_xticklabels(
                [short_cdf_legend_label(sk) for sk in series_keys],
                rotation=16,
                ha="right",
                fontsize=7,
            )
            ax.set_title(f"n = {n}", fontsize=10, fontweight="bold")
            ax.grid(True, alpha=0.3, axis="y")
            ax.set_ylim(bottom=0)
            if ax_idx in (0, 2):
                ax.set_ylabel(ylabel, fontsize=9, fontweight="bold")
        fig_m.tight_layout()
        mp = output_dir / f"{prefix}{metric}.png"
        fig_m.savefig(mp, dpi=150, bbox_inches="tight")
        fig_m.savefig(mp.with_suffix(".pdf"), bbox_inches="tight")
        print(f"Wrote {mp}")
        plt.close(fig_m)

    # --- BFT latency (sim time): x = vehicle count, line = scenario ---
    # Default: overall.propose_all_consensus_latency_sim_s (per-round mean).
    # Ambulance + Byzantine leader (paths under amb_byz_leader/): overall
    # .propose_all_first_submit_to_commit_latency_sim_s (ProposeAll_FirstSubmit_To_OrderCommit_Sim).
    # Wall-clock *_s fields are not plotted (Java CPU, not sim protocol time).
    fig_cl, ax_cl = plt.subplots(figsize=(8.5, 5.5))
    fig_cl.suptitle(
        f"{BFT_BAR_SUPTITLE}{title_suffix}\n"
        "Decision latency by fleet size (sim time): per-vehicle decision_latency_ms mean (run-averaged)",
        fontsize=11,
        fontweight="bold",
    )
    x_cl = np.array(GRID_VEHICLE_COUNTS, dtype=float)
    has_cl = False
    positive_y_vals_cl: list[float] = []
    cl_plotted: list[tuple[int, str, list[float]]] = []  # series idx, label, y (ms)
    for i, sk in enumerate(series_keys):
        y_vals_cl: list[float] = []
        cl_mk = consensus_line_metric_by_label.get(sk, "consensus_latency_sim_s")
        for n in GRID_VEHICLE_COUNTS:
            cell = data.get(sk, {}).get(n, {}).get(cl_mk, {})
            m = float(cell.get("mean", 0) or 0) if isinstance(cell, dict) else 0.0
            y_ms = m * 1000.0 if m > 0 else float("nan")  # convert to ms
            y_vals_cl.append(y_ms)
            if y_ms == y_ms and y_ms > 0:
                positive_y_vals_cl.append(y_ms)
        if all(v != v for v in y_vals_cl):  # all NaN
            continue
        leg = short_cdf_legend_label(sk)
        y_arr = np.array(y_vals_cl, dtype=float)
        mask = ~np.isnan(y_arr)
        ax_cl.plot(
            x_cl[mask],
            y_arr[mask],
            color=DEFAULT_COLORS[i % len(DEFAULT_COLORS)],
            linestyle=DEFAULT_LINESTYLES[i % len(DEFAULT_LINESTYLES)],
            linewidth=2,
            marker="o",
            markersize=6,
            label=leg,
        )
        cl_plotted.append((i, leg, list(y_arr)))
        has_cl = True

    if include_partner:
        y_partner: list[float] = []
        for n in GRID_VEHICLE_COUNTS:
            runs = partner_run_metrics(n)
            vals = [rm.get("decision_latency_s", 0.0) for rm in runs]
            vals_f = [float(v) for v in vals if v is not None and float(v) > 0]
            y_partner.append(float(np.mean(np.array(vals_f, dtype=float))) * 1000.0 if vals_f else float("nan"))
        y_partner_arr = np.array(y_partner, dtype=float)
        mask_p = ~np.isnan(y_partner_arr)
        if mask_p.any():
            ax_cl.plot(
                x_cl[mask_p],
                y_partner_arr[mask_p],
                color="black",
                linestyle="--",
                linewidth=2.4,
                marker="s",
                markersize=6,
                label=PARTNER_LABEL,
            )
            for yv in y_partner_arr:
                if yv == yv and yv > 0:
                    positive_y_vals_cl.append(yv)
            has_cl = True
    if has_cl:
        ax_cl.set_xlabel("Number of vehicles", fontsize=10, fontweight="bold")
        ax_cl.set_ylabel("Decision latency (ms, sim time)", fontsize=10, fontweight="bold")
        ax_cl.set_xticks(GRID_VEHICLE_COUNTS)
        ax_cl.set_xticklabels([f"{n} veh" for n in GRID_VEHICLE_COUNTS], fontsize=9)
        ax_cl.grid(True, alpha=0.3, which="both")
        if positive_y_vals_cl:
            min_y_cl = min(positive_y_vals_cl)
            max_y_cl = max(positive_y_vals_cl)
            if max_y_cl / min_y_cl >= 50:
                ax_cl.set_yscale("log")
                ax_cl.set_ylabel(
                    "Decision latency (ms, sim time, log scale)",
                    fontsize=10,
                    fontweight="bold",
                )
                ax_cl.set_ylim(bottom=max(min_y_cl / 1.5, 1.0), top=max_y_cl * 1.25)
            else:
                ax_cl.set_ylim(bottom=0)
        else:
            ax_cl.set_ylim(bottom=0)
        n_cl_series = len(cl_plotted)
        print("Decision latency (ms, sim time):")
        for i, leg, y_vals_cl in cl_plotted:
            color = DEFAULT_COLORS[i % len(DEFAULT_COLORS)]
            parts: list[str] = []
            for n, yv in zip(GRID_VEHICLE_COUNTS, y_vals_cl):
                if yv == yv and yv > 0:
                    parts.append(f"n={n}: {_format_bft_latency_ms(yv)}")
                    x_off = (i - (n_cl_series - 1) / 2.0) * 12.0
                    y_off = 8.0 + (i % 2) * 5.0
                    ax_cl.annotate(
                        _format_bft_latency_ms(yv),
                        (float(n), yv),
                        textcoords="offset points",
                        xytext=(x_off, y_off),
                        ha="center",
                        va="bottom",
                        fontsize=6,
                        color=color,
                        clip_on=False,
                    )
            if parts:
                print(f"  {leg}: {', '.join(parts)}")
        ax_cl.legend(fontsize=8, loc="best")
        fig_cl.tight_layout()
    else:
        ax_cl.text(0.5, 0.5, "No consensus latency data available",
                   ha="center", va="center", transform=ax_cl.transAxes,
                   fontsize=11, color="gray")
        fig_cl.tight_layout()
    cl_path = output_dir / f"{prefix}consensus_latency.png"
    fig_cl.savefig(cl_path, dpi=150, bbox_inches="tight")
    fig_cl.savefig(cl_path.with_suffix(".pdf"), bbox_inches="tight")
    print(f"Wrote {cl_path}")
    plt.close(fig_cl)


def plot_ambulance_vs_normal_wait(
    base_dir: Path,
    output_dir: Path,
    prefix: str,
    title_note: str,
    exclude_fallback: bool,
    include_partner: bool,
) -> None:
    """
    Apples-to-apples priority benefit summary (partner plotPriority-style, simplified):
    For each n ∈ {4,8,12,16}:
      - BFT priority vehicle mean wait (amb_honest)
      - BFT normal vehicles mean wait (amb_honest)
      - BFT no-priority mean wait (no_amb; all vehicles)
      - (optional) RAFT priority vehicle mean wait (allVehicles)
      - (optional) RAFT no-priority mean wait (allVehicles_nopriority; all vehicles)
    """
    assert plt is not None

    output_dir.mkdir(parents=True, exist_ok=True)

    title_suffix = f"\n{title_note}" if title_note else ""
    fig, axes = plt.subplots(2, 2, figsize=(12, 9), sharey=True)
    axes_flat = axes.ravel()

    for ax_idx, vc in enumerate(GRID_VEHICLE_COUNTS):
        ax = axes_flat[ax_idx]
        # BFT data (our repo): honest ambulance vs no-ambulance baseline.
        amb_patterns = [f"Priority{vc}cars/amb_honest/run_*/{vc}veh_*.json"]
        no_prio_patterns = [f"Priority{vc}cars/no_amb/run_*/{vc}veh_*.json"]

        bft_prio, bft_norm = pooled_waits_by_role(amb_patterns, base_dir, exclude_fallback)
        bft_no_prio_all = scenario_waits(no_prio_patterns, base_dir, "all", exclude_fallback)

        bft_prio_mean = mean_or_none(bft_prio)
        bft_norm_mean = mean_or_none(bft_norm)
        bft_no_prio_mean = mean_or_none(bft_no_prio_all)

        labels = ["BFT priority", "BFT normal", "BFT no priority"]
        values: list[float | None] = [bft_prio_mean, bft_norm_mean, bft_no_prio_mean]
        colors = [AMB_BAR_COLOR, NORM_BAR_COLOR, NO_PRIO_BAR_COLOR]
        hatches = ["", "", "//"]

        if include_partner:
            from partner_metrics_io import partner_priority_and_normal_wait_means_s, partner_wait_time_samples_s

            raft_prio_mean, _raft_norm_mean = partner_priority_and_normal_wait_means_s(vc, mode="allVehicles")
            raft_no_prio_all = partner_wait_time_samples_s(vc, mode="allVehicles_nopriority")
            raft_no_prio_mean = mean_or_none(raft_no_prio_all) if raft_no_prio_all else None

            labels.extend(["RAFT priority", "RAFT no priority"])
            values.extend([raft_prio_mean, raft_no_prio_mean])
            colors.extend(["black", "black"])
            hatches.extend(["", "//"])

        x = np.arange(len(labels), dtype=float)
        ymax = max([float(v) for v in values if v is not None] + [0.0])
        for i, (lab, val, col, hat) in enumerate(zip(labels, values, colors, hatches)):
            if val is None:
                continue
            ax.bar(
                x[i],
                float(val),
                0.72,
                color=col,
                alpha=0.9,
                edgecolor="black",
                linewidth=0.7,
                hatch=hat,
            )
            ax.text(
                x[i],
                float(val) + 0.05,
                f"{float(val):.1f}s",
                ha="center",
                va="bottom",
                fontsize=8,
                fontweight="bold",
            )

        ax.set_xticks(x)
        ax.set_xticklabels(labels, rotation=16, ha="right", fontsize=8)
        ax.set_title(f"n = {vc} vehicles", fontsize=11, fontweight="bold")
        ax.set_xlim(-0.6, len(labels) - 0.4)
        top = ymax * 1.22 + 0.35 if ymax > 0 else 1.0
        ax.set_ylim(0, top)
        ax.grid(True, alpha=0.3, axis="y")
        if ax_idx in (0, 2):
            ax.set_ylabel("Mean wait time (s)", fontsize=10, fontweight="bold")

    fig.suptitle(
        f"Priority benefit: wait time at intersection{title_suffix}\n"
        "Stop → depart (mean wait per vehicle); BFT uses amb_honest vs no_amb; RAFT uses allVehicles vs allVehicles_nopriority",
        fontsize=12,
        fontweight="bold",
    )
    fig.tight_layout()
    out = output_dir / f"{prefix}ambulance_vs_normal_wait.png"
    fig.savefig(out, dpi=150, bbox_inches="tight")
    fig.savefig(out.with_suffix(".pdf"), bbox_inches="tight")
    print(f"Wrote {out}")
    print(f"Wrote {out.with_suffix('.pdf')}")
    plt.close(fig)


def main() -> int:
    args = parse_args()
    if plt is None:
        print("matplotlib is required: pip install matplotlib", file=sys.stderr)
        return 1

    plot_modes = {x.strip().lower() for x in args.plots.split(",")}
    if "all" in plot_modes:
        plot_modes |= {"cdf", "bars", "amb"}

    script_dir = Path(__file__).resolve().parent
    base_dir = args.base_dir.resolve() if args.base_dir else script_dir

    bar_series_cfg: list[tuple[str, dict[int, list[str]]]] | None = None
    vehicle_counts_cfg: list[int] | None = None
    bar_title_note: str | None = None

    if args.config:
        scenarios, bar_series_cfg, vehicle_counts_cfg, bar_title_note = load_config(
            args.config.resolve()
        )
    elif args.scenario:
        scenarios = [parse_scenario_spec(s) for s in args.scenario]
    else:
        scenarios = list(DEFAULT_SCENARIOS)  # legacy single-panel CDF; empty → use grid from bar_series

    if bar_series_cfg is None:
        # Without --config, bar/amb plots use DEFAULT_BAR_SERIES (12 & 16 columns). CDF uses scenarios
        # (e.g. 8 rows for 12 vs 16); deriving bar_series from many CDF rows would break bar layout.
        if args.config is not None:
            bar_series_cfg = derive_bar_series_from_scenarios(scenarios)
            if not any(d for _, d in bar_series_cfg):
                bar_series_cfg = list(DEFAULT_BAR_SERIES)
        else:
            bar_series_cfg = list(DEFAULT_BAR_SERIES)

    if vehicle_counts_cfg is None:
        inferred = set(GRID_VEHICLE_COUNTS)
        for _, d in bar_series_cfg:
            inferred.update(d.keys())
        vehicle_counts_cfg = sorted(inferred) if inferred else list(GRID_VEHICLE_COUNTS)

    exclude_fallback = not args.include_fallback

    out_cdf = args.output
    if out_cdf is None:
        # Prefer writing under the largest available benchmark root so the default output
        # lives next to the newest data (e.g. Priority20cars when present).
        for n in (20, 16, 12, 8, 4):
            cand = base_dir / f"Priority{n}cars"
            if cand.is_dir():
                out_cdf = cand / "cdf_wait_time.png"
                break
        else:
            out_cdf = base_dir / "cdf_wait_time.png"
    out_cdf = out_cdf.resolve()

    out_dir_bars = args.output_dir
    if out_dir_bars is None:
        out_dir_bars = out_cdf.parent
    else:
        out_dir_bars = out_dir_bars.resolve()

    cdf_smooth_note = (
        "\n(smoothed display: binned + blurred CDF; ★ uses exact empirical F(w))"
        if args.cdf_smooth
        else ""
    )

    # --- CDF ---
    if "cdf" in plot_modes:
        if args.scenario:
            series: list[
                tuple[
                    str,
                    np.ndarray,
                    np.ndarray,
                    int,
                    list[float] | None,
                    np.ndarray | None,
                ]
            ] = []
            for i, (label, patterns, role_filter) in enumerate(scenarios):
                if not patterns:
                    continue
                waits = scenario_waits(patterns, base_dir, role_filter, exclude_fallback)
                if not waits:
                    print(
                        f"CDF skip (no data): {label!r} patterns={patterns!r}",
                        file=sys.stderr,
                    )
                    continue
                sorted_raw = np.sort(np.asarray(waits, dtype=float))
                xs, ys = _cdf_xy_from_waits(
                    waits,
                    args.cdf_smooth,
                    args.cdf_smooth_bins,
                    args.cdf_smooth_blur,
                )
                amb_mark: list[float] | None = None
                if role_filter == "all" and not args.cdf_no_ambulance_marker:
                    aw = scenario_ambulance_waits(patterns, base_dir, exclude_fallback)
                    amb_mark = aw if aw else None
                series.append(
                    (label, xs, ys, len(waits), amb_mark, sorted_raw)
                )

            if not series:
                print("CDF: no scenarios with data.", file=sys.stderr)
                if "cdf" in plot_modes and "bars" not in plot_modes and "amb" not in plot_modes:
                    return 1
            else:
                plot_cdf(series, out_cdf, args.title, cdf_smooth_note)
        else:
            plot_cdf_grid(
                base_dir,
                bar_series_cfg,
                out_cdf,
                args.title,
                exclude_fallback,
                args.cdf_no_ambulance_marker,
                args.cdf_smooth,
                args.cdf_smooth_bins,
                args.cdf_smooth_blur,
                args.partner_data,
            )

    # --- Bars ---
    if "bars" in plot_modes:
        plot_bars(
            base_dir,
            bar_series_cfg,
            vehicle_counts_cfg,
            out_dir_bars,
            args.bar_prefix,
            bar_title_note or "",
            args.partner_data,
        )

    # --- Ambulance vs normal mean wait ---
    if "amb" in plot_modes:
        plot_ambulance_vs_normal_wait(
            base_dir,
            out_dir_bars,
            args.bar_prefix,
            bar_title_note or "",
            exclude_fallback,
            args.partner_data,
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

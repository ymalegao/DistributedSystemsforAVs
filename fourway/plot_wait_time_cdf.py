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
                "benchmarks/Priority4cars/no_amb/4veh_*.json",
                "benchmarks/Priority4cars/Normal.json",
            ],
            8: [
                "benchmarks/Priority8cars/no_amb/8veh_*.json",
                "benchmarks/Priority8cars/Normal.json",
            ],
            12: [
                "benchmarks/Priority12cars/no_amb/12veh_*.json",
                "benchmarks/Priority12cars/12Normal_1.json",
            ],
            16: [
                "benchmarks/Priority16cars/no_amb/16veh_*.json",
                "benchmarks/Priority16cars/Normal.json",
            ],
        },
    ),
    (
        "Ambulance (honest)",
        {
            4: [
                "benchmarks/Priority4cars/amb_honest/4veh_[5-9].json",
            ],
            8: [
                "benchmarks/Priority8cars/amb_honest/8veh_[5-9].json",
            ],
            12: [
                "benchmarks/Priority12cars/amb_honest/12veh_[5-9].json",
            ],
            16: [
                "benchmarks/Priority16cars/amb_honest/16veh_[5-9].json",
            ],
        },
    ),
    (
        "Ambulance + Byzantine followers",
        {
            4: [
                "benchmarks/Priority4cars/amb_byz_follower/4veh_[5-9].json",
            ],
            8: [
                "benchmarks/Priority8cars/amb_byz_follower/8veh_[5-9].json",
            ],
            12: [
                "benchmarks/Priority12cars/amb_byz_follower/12veh_[5-9].json",
            ],
            16: [
                "benchmarks/Priority16cars/amb_byz_follower/16veh_[5-9].json",
            ],
        },
    ),
    (
        "Ambulance + Byzantine leader",
        {
            4: [
                "benchmarks/Priority4cars/amb_byz_leader/4veh_[5-9].json",
            ],
            8: [
                "benchmarks/Priority8cars/amb_byz_leader/8veh_[5-9].json",
            ],
            12: [
                "benchmarks/Priority12cars/amb_byz_leader/12veh_[5-9].json",
            ],
            16: [
                "benchmarks/Priority16cars/amb_byz_leader/16veh_[5-9].json",
            ],
        },
    ),
]

# Fixed panel order for 2×2 grids: top row n=4, n=8; bottom row n=12, n=16
GRID_VEHICLE_COUNTS = [4, 8, 12, 16]

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


def _resolve_paths(patterns: list[str], base_dir: Path) -> list[Path]:
    out: list[Path] = []
    for p in patterns:
        raw = Path(p)
        if not raw.is_absolute():
            raw = base_dir / raw
        if any(ch in p for ch in "*?["):
            out.extend(sorted(raw.parent.glob(raw.name)))
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
    cars = data.get("per_car")
    if not isinstance(cars, list):
        return []

    waits: list[float] = []
    for v in cars:
        if not isinstance(v, dict):
            continue
        if exclude_fallback and v.get("used_fallback") is True:
            continue
        role = v.get("role", "normal")
        if role_filter == "normal" and role != "normal":
            continue
        if role_filter == "ambulance" and role != "ambulance":
            continue

        wt = v.get("wait_intersection_s")
        if wt is None:
            continue
        try:
            x = float(wt)
        except (TypeError, ValueError):
            continue
        if x > 0:
            waits.append(x)
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
        cars = data.get("per_car")
        if not isinstance(cars, list):
            continue
        for c in cars:
            if not isinstance(c, dict):
                continue
            if exclude_fallback and c.get("used_fallback") is True:
                continue
            wt = c.get("wait_intersection_s")
            if wt is None:
                continue
            try:
                w = float(wt)
            except (TypeError, ValueError):
                continue
            if w <= 0:
                continue
            role = c.get("role", "normal")
            if role == "ambulance":
                amb.append(w)
            elif role == "normal":
                norm.append(w)
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


def extract_run_metrics(path: Path) -> dict[str, float | None]:
    """Map partner-style names to values from one analyze_log metrics JSON."""
    with open(path, encoding="utf-8") as f:
        data = json.load(f)
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

    return {
        "throughput": fget("throughput_veh_per_s"),
        "fallback_rate": fget("fallback_rate"),
        "messages_sent": float(sent),
        "messages_received": float(recv),
        "estimated_loss_rate": fget("estimated_loss_rate"),
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
    title_suffix = f"\n{title_note}" if title_note else ""

    def grid_axes():
        fig, axes = plt.subplots(2, 2, figsize=(12, 9))
        return fig, axes.ravel()

    # --- Throughput (2×2) ---
    fig, axes = grid_axes()
    fig.suptitle(
        f"{BFT_BAR_SUPTITLE}{title_suffix}\n"
        "Throughput: overall.throughput_veh_per_s (vehicles / second)",
        fontsize=11,
        fontweight="bold",
    )
    for ax_idx, n in enumerate(GRID_VEHICLE_COUNTS):
        _bar_metric_panel(
            axes[ax_idx],
            n,
            series_keys,
            data,
            "throughput",
            1.0,
            lambda m: f"{m:.3f}",
        )
        if ax_idx in (0, 2):
            axes[ax_idx].set_ylabel("Vehicles / second", fontsize=9, fontweight="bold")
    if Patch is not None:
        axes[0].legend(
            handles=[
                Patch(
                    facecolor=DEFAULT_COLORS[i % len(DEFAULT_COLORS)],
                    edgecolor="black",
                    linewidth=0.5,
                )
                for i in range(len(series_keys))
            ],
            labels=[short_cdf_legend_label(sk) for sk in series_keys],
            fontsize=7,
            loc="upper right",
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


def plot_ambulance_vs_normal_wait(
    base_dir: Path,
    bar_series: list[tuple[str, dict[int, list[str]]]],
    vehicle_counts: list[int],
    output_dir: Path,
    prefix: str,
    title_note: str,
    exclude_fallback: bool,
) -> None:
    """
    Mean wait (wait_intersection_s) for ambulance vs normal vehicles, per scenario.
    2×2 subplots: one panel per n ∈ {4,8,12,16}.
    """
    assert plt is not None

    output_dir.mkdir(parents=True, exist_ok=True)

    if not any(d.get(vc) for vc in GRID_VEHICLE_COUNTS for _, d in bar_series):
        print(
            "Ambulance vs normal: no paths for grid fleet sizes; skipping.",
            file=sys.stderr,
        )
        return

    n_scen = len(bar_series)
    if n_scen == 0:
        return

    title_suffix = f"\n{title_note}" if title_note else ""
    fig, axes = plt.subplots(2, 2, figsize=(12, 9), sharey=True)
    axes_flat = axes.ravel()

    for ax_idx, vc in enumerate(GRID_VEHICLE_COUNTS):
        ax = axes_flat[ax_idx]
        x = np.arange(n_scen, dtype=float)
        width = 0.34
        amb_vals: list[float | None] = []
        norm_vals: list[float | None] = []
        labels_short: list[str] = []

        for lab, files_by_vc in bar_series:
            labels_short.append(lab)
            paths = files_by_vc.get(vc) or []
            if not paths:
                amb_vals.append(None)
                norm_vals.append(None)
                continue
            am, nm = pooled_waits_by_role(paths, base_dir, exclude_fallback)
            amb_vals.append(mean_or_none(am))
            norm_vals.append(mean_or_none(nm))

        ymax = 0.0
        for ma, mn in zip(amb_vals, norm_vals):
            for v in (ma, mn):
                if v is not None:
                    ymax = max(ymax, float(v))

        for i in range(n_scen):
            ma, mn = amb_vals[i], norm_vals[i]
            if ma is not None and mn is not None:
                ax.bar(
                    x[i] - width / 2,
                    ma,
                    width,
                    color=AMB_BAR_COLOR,
                    alpha=0.9,
                    edgecolor="black",
                    linewidth=0.7,
                    label="_nolegend_",
                )
                ax.bar(
                    x[i] + width / 2,
                    mn,
                    width,
                    color=NORM_BAR_COLOR,
                    alpha=0.9,
                    edgecolor="black",
                    linewidth=0.7,
                    label="_nolegend_",
                )
                for xv, val in ((x[i] - width / 2, ma), (x[i] + width / 2, mn)):
                    if val is not None and val > 0:
                        ax.text(
                            xv,
                            float(val) + 0.05,
                            f"{float(val):.1f}s",
                            ha="center",
                            va="bottom",
                            fontsize=8,
                            fontweight="bold",
                        )
            elif mn is not None:
                ax.bar(
                    x[i],
                    mn,
                    width * 1.65,
                    color=NORM_BAR_COLOR,
                    alpha=0.9,
                    edgecolor="black",
                    linewidth=0.7,
                )
                ax.text(
                    x[i],
                    float(mn) + 0.05,
                    f"{float(mn):.1f}s",
                    ha="center",
                    va="bottom",
                    fontsize=8,
                    fontweight="bold",
                )
            elif ma is not None:
                ax.bar(
                    x[i],
                    ma,
                    width * 1.65,
                    color=AMB_BAR_COLOR,
                    alpha=0.9,
                    edgecolor="black",
                    linewidth=0.7,
                )
                ax.text(
                    x[i],
                    float(ma) + 0.05,
                    f"{float(ma):.1f}s",
                    ha="center",
                    va="bottom",
                    fontsize=8,
                    fontweight="bold",
                )

        ax.set_xticks(x)
        ax.set_xticklabels(labels_short, rotation=18, ha="right", fontsize=8)
        ax.set_title(f"n = {vc} vehicles", fontsize=11, fontweight="bold")
        ax.set_xlim(-0.6, n_scen - 0.4)
        top = ymax * 1.22 + 0.35 if ymax > 0 else 1.0
        ax.set_ylim(0, top)
        ax.grid(True, alpha=0.3, axis="y")
        if ax_idx in (0, 2):
            ax.set_ylabel("Mean wait time (s)", fontsize=10, fontweight="bold")

    fig.suptitle(
        f"Wait time at intersection: ambulance vs normal vehicles{title_suffix}\n"
        "Mean of wait_intersection_s (stop → depart); pooled over listed JSON runs",
        fontsize=12,
        fontweight="bold",
    )
    if Patch is not None:
        legend_elements = [
            Patch(facecolor=AMB_BAR_COLOR, edgecolor="black", label="Ambulance (mean)"),
            Patch(facecolor=NORM_BAR_COLOR, edgecolor="black", label="Normal vehicles (mean)"),
        ]
        axes_flat[0].legend(handles=legend_elements, fontsize=9, loc="upper right")
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
        out_cdf = base_dir / "benchmarks" / "Priority16cars" / "cdf_wait_time.png"
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
        )

    # --- Ambulance vs normal mean wait ---
    if "amb" in plot_modes:
        plot_ambulance_vs_normal_wait(
            base_dir,
            bar_series_cfg,
            vehicle_counts_cfg,
            out_dir_bars,
            args.bar_prefix,
            bar_title_note or "",
            exclude_fallback,
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

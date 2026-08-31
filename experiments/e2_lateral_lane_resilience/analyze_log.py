#!/usr/bin/env python3
"""Exploratory analysis for E2 lateral-lane knob selection (δ, k, σ_lat).

Joins the one-dimensional sweeps used for the paper knob-selection claim:

  δ   ← Phase2TwoLaneDeltaBCompletionGrid  (σ=0.5, k=3 fixed)
  k   ← Phase2TwoLaneKSweep                (δ=1.75, σ=0.5 fixed)
  σ   ← Phase2TwoLaneSigmaSweep            (δ=1.75, k=3 fixed)

Also summarizes Phase2TwoLaneGrid (mixed locked matrix) and notes that
Phase2TwoLaneParameterSweeps/shared_anchor is a staging anchor, not a sweep.

IEEE/ICRA sizing belongs in a separate publication script — not here.

Usage:
  python analyze_log.py
  python analyze_log.py --no-plots
"""

from __future__ import annotations

import argparse
import csv
import math
from collections import defaultdict
from pathlib import Path
from typing import Any


F = 5
MODE_OP = {"sigma_lat_m": 0.5, "delta_m": 1.75, "k": 2.0}  # e2 manifest operating point

WONG = {
    1: "#0072B2",
    2: "#009E73",
    3: "#E69F00",
    4: "#CC79A7",
    5: "#D55E00",
    6: "#000000",
}


def _read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def _num(row: dict[str, str], key: str, default: float = math.nan) -> float:
    value = row.get(key, "")
    if value in (None, ""):
        return default
    return float(value)


def _throughput(row: dict[str, str]) -> float:
    for key in ("mean_throughput_veh_per_min", "throughput_veh_per_min"):
        if row.get(key) not in (None, ""):
            return float(row[key])
    return math.nan


def _wait(row: dict[str, str]) -> float:
    for key in ("mean_wait_s", "wait_mean_s"):
        if row.get(key) not in (None, ""):
            return float(row[key])
    return math.nan


def _resolve_paths(results_root: Path) -> dict[str, Path]:
    return {
        "delta": results_root
        / "Phase2TwoLaneDeltaBCompletionGrid"
        / "delta_b_completion_aggregates.csv",
        "k": results_root / "Phase2TwoLaneKSweep" / "k_sweep_full_aggregates.csv",
        "sigma": results_root / "Phase2TwoLaneSigmaSweep" / "sigma_sweep_full_aggregates.csv",
        "grid": results_root / "Phase2TwoLaneGrid" / "two_lane_grid_aggregates.csv",
        "param_sweeps": results_root / "Phase2TwoLaneParameterSweeps",
    }


def _print_header(paths: dict[str, Path]) -> None:
    print("=" * 78)
    print("E2 Lateral Lane Resilience — exploratory analyze_log (knob selection)")
    print("=" * 78)
    print(
        f"manifest operating point: σ_lat={MODE_OP['sigma_lat_m']}, "
        f"δ={MODE_OP['delta_m']}, k={MODE_OP['k']}"
    )
    print(f"f={F}  →  b=f shoulder, b=f+1 Byzantine cliff")
    print()
    for name, path in paths.items():
        status = "OK" if path.exists() else "MISSING"
        print(f"  [{status}] {name:<12} {path}")
    print()


def _print_axis_table(
    title: str,
    rows: list[dict[str, str]],
    x_key: str,
    fixed: str,
) -> None:
    print(f"--- {title} ---")
    print(f"fixed: {fixed}")
    xs = sorted({_num(r, x_key) for r in rows if _num(r, "b") >= 1})
    bs = sorted({int(_num(r, "b")) for r in rows if _num(r, "b") >= 1})
    corner = f"b\\{x_key[:6]}"
    header = f"{corner:>10}" + "".join(f"{x:>8.2g}" for x in xs)
    print(header)
    print("-" * len(header))
    by = {(int(_num(r, "b")), _num(r, x_key)): r for r in rows if _num(r, "b") >= 1}
    for b in bs:
        cells = []
        for x in xs:
            r = by.get((b, x))
            if r is None:
                cells.append(f"{'—':>8}")
            else:
                cells.append(f"{_num(r, 'false_certificate_rate'):>8.2f}")
        mark = "  ←f" if b == F else ("  ←f+1" if b == F + 1 else "")
        print(f"{b:>10}" + "".join(cells) + mark)
    print()


def _print_operating_slice(k_rows: list[dict[str, str]]) -> None:
    print("--- Operating-point slice from KSweep (δ=1.75, σ=0.5) ---")
    print("Why this matters: manifest locks k=2; sigma/delta sweeps were often run at k=3.")
    for k in (1.0, 2.0, 3.0):
        print(f"  k={k:g}:")
        group = sorted(
            (r for r in k_rows if abs(_num(r, "k") - k) < 1e-9),
            key=lambda r: _num(r, "b"),
        )
        for r in group:
            b = int(_num(r, "b"))
            print(
                f"    b={b}  FE={_num(r, 'false_certificate_rate'):.2f} "
                f"[{_num(r, 'wilson95_low'):.2f},{_num(r, 'wilson95_high'):.2f}]  "
                f"thr={_throughput(r):.1f} veh/min  wait={_wait(r):.1f} s"
            )
    print()


def _print_grid_summary(grid_rows: list[dict[str, str]]) -> None:
    print("--- Phase2TwoLaneGrid (mixed locked matrix; not a clean 1-D sweep) ---")
    cells: dict[tuple[float, float, float], list[dict[str, str]]] = defaultdict(list)
    for r in grid_rows:
        cells[(_num(r, "sigma_lat_m"), _num(r, "delta_m"), _num(r, "k"))].append(r)
    print(f"  unique (σ, δ, k) cells: {len(cells)}")
    for (sig, delta, k), rows in sorted(cells.items()):
        bs = [int(_num(r, "b")) for r in rows]
        fes = [round(_num(r, "false_certificate_rate"), 2) for r in rows]
        print(f"  σ={sig:g} δ={delta:g} k={k:g}: b={bs} FE={fes}")
    # Cross-check overlap with KSweep at OP-like cell
    print()
    print("  Note: Grid mixes axes; use Δ/k/σ full sweeps for the selection claim.")
    print()


def _print_param_sweeps_note(path: Path) -> None:
    print("--- Phase2TwoLaneParameterSweeps ---")
    if not path.exists():
        print("  missing")
        print()
        return
    anchors = sorted({p.parent for p in path.rglob("16veh_*.json")})
    print(f"  shared_anchor run folders with analyzer JSON: {len(anchors)}")
    if anchors:
        # show first anchor path pattern
        rel = anchors[0].relative_to(path)
        print(f"  example: {rel}")
        print("  This is a staging/shared anchor (σ=0.5, δ=1.75, k=3), not a knob sweep.")
    print()


def _recommend() -> None:
    print("=" * 78)
    print("Candidate publication plot compositions")
    print("=" * 78)
    print(
        "1. COMBINED three-panel (RECOMMENDED for knob-selection claim): "
        "one double-column figure with (a) FE vs δ, (b) FE vs k, (c) FE vs σ_lat; "
        "curves colored by Byzantine count b; b=f solid shoulder, b=f+1 dashed cliff. "
        "Wilson 95% CI on each point. Captions fix the other two knobs per panel. "
        "This is exactly the 'we chose (δ*, k*, σ*) from this matrix' figure."
    )
    print(
        "2. Three separate single-column figures: same data, one knob each. "
        "Use only if the combined figure gets too dense in layout review."
    )
    print(
        "3. Grid heatmap / locked-matrix scatter: exploratory only — Grid is not "
        "orthogonal in one axis, so it weakens a selection claim."
    )
    print(
        "4. Do NOT use ParameterSweeps as a panel source — it is an anchor, not a sweep."
    )
    print()
    print(
        "Caveat to flag in caption/text: Δ and σ sweeps fix k=3, while the manifest "
        "operating point uses k=2. Panel (b) is the place that justifies locking k=2; "
        "panels (a)/(c) show sensitivity at the more permissive k=3 gate."
    )
    print()
    print(
        "Recommendation: ship (1) as the E2 selection figure. Optionally add a later "
        "k-only safety+throughput companion if you need the efficiency story."
    )
    print()


def _plot_rate_lines(ax: Any, rows: list[dict[str, str]], x_key: str, xlabel: str) -> None:
    import numpy as np

    for b in range(1, F + 2):
        group = sorted(
            (r for r in rows if int(_num(r, "b")) == b),
            key=lambda r: _num(r, x_key),
        )
        if not group:
            continue
        x = [_num(r, x_key) for r in group]
        y = [_num(r, "false_certificate_rate") for r in group]
        lo = [_num(r, "wilson95_low") for r in group]
        hi = [_num(r, "wilson95_high") for r in group]
        yerr = [
            [max(0.0, yy - ll) for yy, ll in zip(y, lo)],
            [max(0.0, hh - yy) for yy, hh in zip(y, hi)],
        ]
        color = WONG[b]
        ls = "--" if b == F + 1 else "-"
        lw = 2.2 if b in (F, F + 1) else 1.4
        label = f"b={b}" + (" (=f)" if b == F else (" (=f+1)" if b == F + 1 else ""))
        ax.plot(x, y, color=color, linestyle=ls, linewidth=lw, marker="o", markersize=4, label=label)
        ax.errorbar(x, y, yerr=yerr, fmt="none", ecolor=color, alpha=0.45, capsize=2, linewidth=1)
    ax.set_xlabel(xlabel)
    ax.set_ylabel("False certificate rate")
    ax.set_ylim(-0.05, 1.08)
    ax.grid(True, alpha=0.35, linestyle="--")


def _save_plots(
    delta_rows: list[dict[str, str]],
    k_rows: list[dict[str, str]],
    sigma_rows: list[dict[str, str]],
    out_dir: Path,
) -> None:
    try:
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib not available; skipping exploratory PNGs.")
        return

    out_dir.mkdir(parents=True, exist_ok=True)

    # Combined 3-panel — the selection-claim candidate
    fig, axes = plt.subplots(1, 3, figsize=(12.5, 3.8), sharey=True)
    _plot_rate_lines(
        axes[0],
        [r for r in delta_rows if _num(r, "b") >= 1],
        "delta_m",
        r"Claim displacement $\delta$ [m]",
    )
    axes[0].set_title(r"(a) Adversary axis  ($\sigma_{lat}=0.5$, $k=3$)")
    _plot_rate_lines(axes[1], k_rows, "k", r"Gate tolerance $k$  ($\tau=k\sigma$)")
    axes[1].set_title(r"(b) Defender knob  ($\delta=1.75$ m, $\sigma_{lat}=0.5$)")
    axes[1].set_ylabel("")
    _plot_rate_lines(
        axes[2],
        sigma_rows,
        "sigma_lat_m",
        r"Lateral noise $\sigma_{lat}$ [m]",
    )
    axes[2].set_title(r"(c) Environment axis  ($\delta=1.75$ m, $k=3$)")
    axes[2].set_ylabel("")

    # Mark operating point on each panel
    axes[0].axvline(MODE_OP["delta_m"], color="0.4", linestyle=":", linewidth=1.0, alpha=0.8)
    axes[1].axvline(MODE_OP["k"], color="0.4", linestyle=":", linewidth=1.0, alpha=0.8)
    axes[2].axvline(MODE_OP["sigma_lat_m"], color="0.4", linestyle=":", linewidth=1.0, alpha=0.8)

    handles, labels = axes[1].get_legend_handles_labels()
    fig.legend(handles, labels, loc="lower center", ncol=6, frameon=False, bbox_to_anchor=(0.5, -0.08))
    fig.suptitle(
        "E2 exploratory: false-certificate rate across δ / k / σ (dotted = manifest OP)",
        y=1.05,
        fontsize=11,
    )
    fig.tight_layout()
    path = out_dir / "explore_knob_selection_three_panel.png"
    fig.savefig(path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"wrote {path}")

    # Individual panels for review
    for stem, rows, x_key, xlabel, title in (
        (
            "explore_delta_axis.png",
            [r for r in delta_rows if _num(r, "b") >= 1],
            "delta_m",
            r"$\delta$ [m]",
            "δ axis (DeltaB completion)",
        ),
        (
            "explore_k_axis.png",
            k_rows,
            "k",
            r"$k$",
            "k axis (KSweep full)",
        ),
        (
            "explore_sigma_axis.png",
            sigma_rows,
            "sigma_lat_m",
            r"$\sigma_{lat}$ [m]",
            "σ axis (SigmaSweep full)",
        ),
    ):
        fig, ax = plt.subplots(figsize=(5.5, 3.8))
        _plot_rate_lines(ax, rows, x_key, xlabel)
        ax.set_title(title)
        ax.legend(frameon=False, fontsize=8, loc="best")
        fig.tight_layout()
        out = out_dir / stem
        fig.savefig(out, dpi=150, bbox_inches="tight")
        plt.close(fig)
        print(f"wrote {out}")

    # k safety + throughput companion
    fig, axes = plt.subplots(1, 2, figsize=(9.5, 3.8))
    _plot_rate_lines(axes[0], k_rows, "k", r"$k$")
    axes[0].set_title("Safety")
    axes[0].axvline(MODE_OP["k"], color="0.4", linestyle=":", linewidth=1.0)
    for b in range(1, F + 2):
        group = sorted(
            (r for r in k_rows if int(_num(r, "b")) == b),
            key=lambda r: _num(r, "k"),
        )
        if not group:
            continue
        axes[1].plot(
            [_num(r, "k") for r in group],
            [_throughput(r) for r in group],
            color=WONG[b],
            linestyle="--" if b == F + 1 else "-",
            marker="o",
            markersize=4,
            label=f"b={b}",
        )
    axes[1].axvline(MODE_OP["k"], color="0.4", linestyle=":", linewidth=1.0)
    axes[1].set_xlabel(r"$k$")
    axes[1].set_ylabel("Throughput [veh/min]")
    axes[1].set_title("Throughput")
    axes[1].grid(True, alpha=0.35, linestyle="--")
    handles, labels = axes[1].get_legend_handles_labels()
    fig.legend(handles, labels, loc="lower center", ncol=6, frameon=False, bbox_to_anchor=(0.5, -0.08))
    fig.suptitle("E2 exploratory: defender knob k — safety vs throughput", y=1.05, fontsize=11)
    fig.tight_layout()
    path = out_dir / "explore_k_safety_throughput.png"
    fig.savefig(path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"wrote {path}")
    print()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--results-root",
        type=Path,
        default=Path(__file__).resolve().parent / "results",
        help="e2 results/ directory containing Phase2TwoLane* folders",
    )
    parser.add_argument(
        "--plots-dir",
        type=Path,
        default=None,
        help="Exploratory PNG output (default: <results-root>/explore_plots)",
    )
    parser.add_argument("--no-plots", action="store_true")
    args = parser.parse_args()

    results_root = args.results_root.resolve()
    paths = _resolve_paths(results_root)
    _print_header(paths)

    missing = [name for name, path in paths.items() if name != "param_sweeps" and not path.exists()]
    if missing:
        raise SystemExit(f"missing required aggregates: {missing}")

    delta_rows = _read_csv(paths["delta"])
    k_rows = _read_csv(paths["k"])
    sigma_rows = _read_csv(paths["sigma"])
    grid_rows = _read_csv(paths["grid"])

    _print_axis_table(
        "δ sweep (DeltaB completion): FE rate",
        delta_rows,
        "delta_m",
        "σ_lat=0.5, k=3",
    )
    _print_axis_table(
        "k sweep (KSweep full): FE rate",
        k_rows,
        "k",
        "δ=1.75, σ_lat=0.5",
    )
    _print_axis_table(
        "σ sweep (SigmaSweep full): FE rate",
        sigma_rows,
        "sigma_lat_m",
        "δ=1.75, k=3",
    )
    _print_operating_slice(k_rows)
    _print_grid_summary(grid_rows)
    _print_param_sweeps_note(paths["param_sweeps"])
    _recommend()

    if not args.no_plots:
        plots_dir = (args.plots_dir or (results_root / "explore_plots")).resolve()
        _save_plots(delta_rows, k_rows, sigma_rows, plots_dir)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

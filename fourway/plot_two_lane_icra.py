#!/usr/bin/env python3
"""Generate publication-oriented plots for the two-lane perception experiments.

The main sweep figure is deliberately empirical.  Theory appears once, in a
separate two-stage calibration figure:

  1. analytic Gaussian gate q0 -> measured per-witness q0;
  2. binomial certificate prediction -> measured false-certificate rate.

This avoids putting q0 and a second calculated probability on the axes of the
same "collapse" plot and makes the sensor/protocol composition explicit.
"""

from __future__ import annotations

import argparse
import csv
import math
from pathlib import Path
from typing import Iterable

import matplotlib.pyplot as plt
from matplotlib.lines import Line2D


F = 5
COLORS = plt.get_cmap("viridis")([0.08, 0.25, 0.42, 0.59, 0.76, 0.93])


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def number(row: dict[str, str], key: str, default: float = math.nan) -> float:
    value = row.get(key, "")
    if value in (None, ""):
        return default
    return float(value)


def wilson(successes: int, total: int, z: float = 1.959963984540054) -> tuple[float, float]:
    if total <= 0:
        return math.nan, math.nan
    p = successes / total
    denom = 1.0 + z * z / total
    center = (p + z * z / (2.0 * total)) / denom
    radius = z * math.sqrt(p * (1.0 - p) / total + z * z / (4.0 * total * total)) / denom
    return max(0.0, center - radius), min(1.0, center + radius)


def normal_cdf(x: float) -> float:
    return 0.5 * (1.0 + math.erf(x / math.sqrt(2.0)))


def analytic_q0(sigma: float, delta: float, k: float) -> float:
    if sigma <= 0:
        return 1.0 if abs(delta) <= 0 else 0.0
    scaled = abs(delta) / sigma
    return normal_cdf(scaled + k) - normal_cdf(scaled - k)


def b_label(b: int) -> str:
    if b == F:
        return f"b={b} (=f, shoulder)"
    if b == F + 1:
        return f"b={b} (=f+1, cliff)"
    return f"b={b}"


def line_style(b: int) -> dict[str, object]:
    return {
        "color": COLORS[b - 1],
        "linestyle": "--" if b == F + 1 else "-",
        "linewidth": 2.7 if b in (F, F + 1) else 1.7,
        "alpha": 1.0 if b in (F, F + 1) else 0.82,
        "marker": "o",
        "markersize": 5.0,
    }


def plot_rate_lines(ax: plt.Axes, rows: list[dict[str, str]], x_key: str, xlabel: str) -> None:
    for b in range(1, F + 2):
        group = sorted((r for r in rows if int(float(r["b"])) == b), key=lambda r: number(r, x_key))
        if not group:
            continue
        x = [number(r, x_key) for r in group]
        y = [number(r, "false_certificate_rate") for r in group]
        lo = [number(r, "wilson95_low") for r in group]
        hi = [number(r, "wilson95_high") for r in group]
        style = line_style(b)
        ax.plot(x, y, label=b_label(b), **style)
        ax.errorbar(
            x,
            y,
            yerr=[[max(0.0, yy - ll) for yy, ll in zip(y, lo)],
                  [max(0.0, hh - yy) for yy, hh in zip(y, hi)]],
            fmt="none",
            ecolor=style["color"],
            alpha=0.45,
            capsize=2,
            linewidth=1,
        )
    ax.set_xlabel(xlabel)
    ax.set_ylim(-0.04, 1.04)
    ax.set_ylabel("False certificate / unsafe co-occupancy rate")
    ax.grid(True, alpha=0.22)


def save(fig: plt.Figure, output_dir: Path, stem: str) -> None:
    fig.savefig(output_dir / f"{stem}.png", dpi=220, bbox_inches="tight")
    fig.savefig(output_dir / f"{stem}.pdf", bbox_inches="tight")
    plt.close(fig)


def safety_axes(delta_rows: list[dict[str, str]], k_rows: list[dict[str, str]], sigma_rows: list[dict[str, str]], output_dir: Path) -> None:
    fig, axes = plt.subplots(1, 3, figsize=(14.2, 4.25), sharey=True)
    plot_rate_lines(axes[0], [r for r in delta_rows if number(r, "b") >= 1], "delta_m", r"Claim displacement $\delta$ (m)")
    axes[0].set_title(r"(a) Adversary axis: $\sigma_{lat}=0.5$, $k=3$")
    plot_rate_lines(axes[1], k_rows, "k", r"Gate tolerance $k$ in $\tau=k\sigma$")
    axes[1].set_title(r"(b) Defender knob: $\delta=1.75$ m, $\sigma_{lat}=0.5$")
    axes[1].set_ylabel("")
    plot_rate_lines(axes[2], sigma_rows, "sigma_lat_m", r"Lateral noise $\sigma_{lat}$ (m)")
    axes[2].set_title(r"(c) Environment axis: $\delta=1.75$ m, $k=3$")
    axes[2].set_ylabel("")
    handles, labels = axes[2].get_legend_handles_labels()
    fig.legend(handles, labels, loc="lower center", ncol=6, frameon=False, bbox_to_anchor=(0.5, -0.08))
    fig.suptitle("Evidence-gated safety across adversary, defender, and environment axes", y=1.03, fontsize=14)
    fig.tight_layout()
    save(fig, output_dir, "icra_two_lane_safety_axes")


def k_performance(k_rows: list[dict[str, str]], output_dir: Path) -> None:
    fig, axes = plt.subplots(1, 3, figsize=(13.8, 4.15))
    plot_rate_lines(axes[0], k_rows, "k", r"Gate tolerance $k$")
    axes[0].set_title("(a) Physical consequence")

    for b in range(1, F + 2):
        group = sorted((r for r in k_rows if int(float(r["b"])) == b), key=lambda r: number(r, "k"))
        if not group:
            continue
        style = line_style(b)
        axes[1].plot([number(r, "k") for r in group], [number(r, "throughput_veh_per_min") for r in group], label=b_label(b), **style)
        axes[2].plot([number(r, "k") for r in group], [number(r, "wait_mean_s") for r in group], label=b_label(b), **style)

    axes[1].set_title("(b) Completed throughput")
    axes[1].set_xlabel(r"Gate tolerance $k$")
    axes[1].set_ylabel("Throughput (vehicles/min)")
    axes[2].set_title("(c) Mean wait")
    axes[2].set_xlabel(r"Gate tolerance $k$")
    axes[2].set_ylabel("Mean wait (s)")
    for ax in axes[1:]:
        ax.grid(True, alpha=0.22)
    handles, labels = axes[2].get_legend_handles_labels()
    fig.legend(handles, labels, loc="lower center", ncol=6, frameon=False, bbox_to_anchor=(0.5, -0.08))
    fig.suptitle(r"Defender knob $k$: safety and end-to-end performance ($\delta=1.75$ m, $\sigma_{lat}=0.5$)", y=1.03, fontsize=14)
    fig.tight_layout()
    save(fig, output_dir, "icra_k_safety_performance")


def unique_cells(sources: Iterable[list[dict[str, str]]]) -> list[dict[str, str]]:
    cells: dict[tuple[float, float, float, float], dict[str, str]] = {}
    for rows in sources:
        for row in rows:
            b = number(row, "b")
            if b < 1:
                continue
            key = (b, number(row, "sigma_lat_m"), number(row, "delta_m"), number(row, "k"))
            cells[key] = row
    return list(cells.values())


def model_validation(rows: list[dict[str, str]], output_dir: Path) -> None:
    fig, axes = plt.subplots(1, 2, figsize=(10.3, 4.35))
    ax_q0, ax_cert = axes

    for b in range(1, F + 2):
        group = [r for r in rows if int(number(r, "b")) == b]
        color = COLORS[b - 1]
        for row in group:
            sigma, delta, k = number(row, "sigma_lat_m"), number(row, "delta_m"), number(row, "k")
            x = analytic_q0(sigma, delta, k)
            y = number(row, "pooled_q0_empirical")
            accepted = int(number(row, "honest_lane_accepts_total", 0))
            evaluated = int(number(row, "honest_lane_evaluations_total", 0))
            lo, hi = wilson(accepted, evaluated)
            if not math.isnan(y):
                ax_q0.errorbar(x, y, yerr=[[max(0.0, y - lo)], [max(0.0, hi - y)]], fmt="o", color=color, alpha=0.55, markersize=4, capsize=1.5)

            pred = number(row, "mean_binomial_prediction")
            observed = number(row, "false_certificate_rate")
            obs_lo, obs_hi = number(row, "wilson95_low"), number(row, "wilson95_high")
            if not math.isnan(pred):
                ax_cert.errorbar(pred, observed, yerr=[[max(0.0, observed - obs_lo)], [max(0.0, obs_hi - observed)]], fmt="o", color=color, alpha=0.62, markersize=4, capsize=1.5)

    for ax in axes:
        ax.plot([0, 1], [0, 1], color="black", linestyle="--", linewidth=1.25, label="ideal agreement")
        ax.set_xlim(-0.03, 1.03)
        ax.set_ylim(-0.03, 1.03)
        ax.grid(True, alpha=0.22)

    ax_q0.set_title("(a) Sensor-model validation")
    ax_q0.set_xlabel(r"Analytic Gaussian $q_0$")
    ax_q0.set_ylabel(r"Measured witness false-accept rate $\hat q_0$")
    ax_cert.set_title("(b) Protocol-model validation")
    ax_cert.set_xlabel(r"Binomial prediction using measured $h$ and $b_{sig}$")
    ax_cert.set_ylabel("Measured false-certificate rate")

    legend_handles = [Line2D([0], [0], marker="o", linestyle="none", color=COLORS[b - 1], label=b_label(b)) for b in range(1, F + 2)]
    legend_handles.append(Line2D([0], [0], linestyle="--", color="black", label="ideal agreement"))
    fig.legend(handles=legend_handles, loc="lower center", ncol=7, frameon=False, bbox_to_anchor=(0.5, -0.09))
    fig.suptitle("Two-stage calibration: physical sensor gate, then f+1 certificate composition", y=1.03, fontsize=14)
    fig.tight_layout()
    save(fig, output_dir, "icra_two_stage_model_validation")


def write_captions(output_dir: Path) -> None:
    text = """ICRA plot notes
===============

icra_two_lane_safety_axes
  Empirical false-certificate rate (identical to attack-induced conflicting
  co-occupancy in these runs) with Wilson 95% intervals.  The three panels
  isolate the adversary axis delta, defender knob k, and environmental sensor
  noise sigma_lat.  b=f is the probabilistic shoulder; b=f+1 is a Byzantine
  cliff/control and is drawn dashed.

icra_k_safety_performance
  End-to-end consequence of changing k.  Safety, completed throughput, and
  mean wait are measured from the same completion runs.  The present attacked
  fixture should not be described as a Pareto tradeoff unless the data show
  one; unsafe co-occupancy itself can reduce throughput.

icra_two_stage_model_validation
  Panel (a) validates the analytic Gaussian gate probability against raw,
  per-witness observations.  Panel (b) then validates the f+1 composition:
  each cell's prediction uses the measured honest evaluator count h and the
  Byzantine support b_sig actually present in finalized certificates.  Thus
  neither panel plots one calculated probability against another calculated
  probability: each theoretical x-coordinate is compared with an empirical
  outcome on y.
"""
    (output_dir / "icra_plot_notes.txt").write_text(text, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()

    root = args.repo_root.resolve()
    output_dir = (args.output_dir or root / "benchmarks" / "Phase2ICRAFigures").resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    delta_path = root / "benchmarks" / "Phase2TwoLaneDeltaBGrid" / "two_lane_delta_b_full_aggregates.csv"
    k_path = root / "benchmarks" / "Phase2TwoLaneKSweep" / "k_sweep_full_aggregates.csv"
    sigma_path = root / "benchmarks" / "Phase2TwoLaneSigmaSweep" / "sigma_sweep_full_aggregates.csv"
    for path in (delta_path, k_path, sigma_path):
        if not path.exists():
            raise SystemExit(f"missing required aggregate: {path}")

    delta_rows, k_rows, sigma_rows = read_csv(delta_path), read_csv(k_path), read_csv(sigma_path)
    safety_axes(delta_rows, k_rows, sigma_rows, output_dir)
    k_performance(k_rows, output_dir)
    model_validation(unique_cells((delta_rows, k_rows, sigma_rows)), output_dir)
    write_captions(output_dir)
    print(f"Wrote ICRA figures to {output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

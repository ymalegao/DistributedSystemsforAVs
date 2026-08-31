#!/usr/bin/env python3
"""Camera-ready IEEE/ICRA figures for the Evaluation section (writing/icra_eval.tex).

Regenerate::

    python3 fourway/plot_icra_camera_ready.py
    cp experiments/shared/results/Phase2ICRAFigures/icra_*.pdf writing/figures/

DATA_SOURCES
============
Each figure MUST use the curated aggregate named below.  Do NOT substitute
``two_lane_grid_aggregates.csv`` or smoke-tier benchmarks for full sweeps.

Figure (in paper)              Source file(s)                          Filters / notes
-----------------------------  --------------------------------------  ---------------------------
icra_two_lane_safety_axes      E2 delta_b_completion_aggregates.csv    panel (a): sigma=0.5, k=3
                               E2 k_sweep_full_aggregates.csv          panel (b): sigma=0.5, delta=1.75
                               E2 sigma_sweep_full_aggregates.csv      panel (c): delta=1.75, k=3
icra_model_validation_q0       union of the three sweeps above         Stage-1 q0 vs analytic
icra_model_validation_cert     union of the three sweeps above         Stage-2 binomial vs measured
icra_k_operating_point         E5 honest_operating_k-full_aggregates   honest q1 vs k (delta=0)
                               E2 k_sweep_full_aggregates.csv          shoulder FP, b=f only
icra_scaling_crossover         E5 two_lane_scale_adversarial_full_*    20 reps/cell; NOT *_smoke_*
icra_direction_ablation        E4 direction_ablation_straight_heavy_*  attack_kind=FALSE_DIRECTION
icra_rsu_resilience            RSU_* constants in this file            provisional; unmerged branch

Generated but NOT in icra_eval.tex:
  icra_gate_accept_delta       fourway/adjacent_lane_calibration/canonical_validation_summary.json
  icra_direction_fp_fn         same as direction ablation
  icra_two_stage_model_validation  combined Stage-1+2 (superseded by split panels)

Anti-patterns (will produce wrong plots):
  - two_lane_grid_aggregates.csv for delta panel -- mixed axes, not a delta sweep
  - two_lane_scale_adversarial_smoke_* for scaling -- 1 rep/cell (N=8 cliff artifact)
  - benchmarks/Phase2TwoLaneDeltaBGrid/* if curated E2 copy exists

No system LaTeX is required: matplotlib's built-in mathtext renders the
$\\mathrm{...}$-style unit labels IEEE expects without `text.usetex`.
"""

from __future__ import annotations

import csv
import json
import math
from pathlib import Path

import matplotlib.pyplot as plt
from matplotlib.lines import Line2D

F = 5  # tolerated Byzantine count at N=16 (the frozen operating point)

# Expected one-axis sweep sizes (b=1..6 unless noted).  Checked in validate_sweep_data().
EXPECTED_SWEEP_CELLS = {
    "delta": 18,   # 6 b x 3 delta @ sigma=0.5, k=3
    "k": 18,       # 6 b x 3 k @ sigma=0.5, delta=1.75
    "sigma": 42,   # 6 b x 7 sigma @ delta=1.75, k=3
}
EXPECTED_ADVERSARIAL_ROWS = 12  # 4 N x 3 roles, 20 reps each
EXPECTED_HONEST_K_ROWS = 3        # k in {1, 2, 3}

# Wong/Okabe-Ito colorblind-safe palette (skip yellow: poor contrast on white).
WONG = ["#000000", "#E69F00", "#56B4E9", "#009E73", "#0072B2", "#D55E00", "#CC79A7"]

DIRECTION_MODE_LABEL = {
    "all_singleton": ("all-singleton", WONG[0]),
    "eligibility_off": ("eligibility off", WONG[5]),
    "eligibility_on": ("eligibility on", WONG[4]),
}
DIRECTION_MODE_ORDER = ["all_singleton", "eligibility_off", "eligibility_on"]


def setup_icra_style(column: str = "double", font_size: int = 8) -> tuple[float, float]:
    width = 3.5 if column == "single" else 7.0
    height = width * 0.40 if column == "double" else width * 0.68
    plt.rcParams.update({
        "font.family": "serif",
        "mathtext.fontset": "stix",
        "figure.figsize": (width, height),
        "font.size": font_size,
        "axes.labelsize": font_size,
        "axes.titlesize": font_size,
        "xtick.labelsize": font_size - 1,
        "ytick.labelsize": font_size - 1,
        "legend.fontsize": font_size - 2,
        "lines.linewidth": 1.2,
        "lines.markersize": 3.5,
        "grid.linewidth": 0.5,
        "grid.alpha": 0.4,
        "grid.linestyle": "--",
        "pdf.fonttype": 42,
        "ps.fonttype": 42,
    })
    return width, height


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
        return f"$b={b}$ (=f, shoulder)"
    if b == F + 1:
        return f"$b={b}$ (=f+1, cliff)"
    return f"$b={b}$"


def line_style(b: int) -> dict[str, object]:
    return {
        "color": WONG[(b - 1) % len(WONG)],
        "linestyle": "--" if b == F + 1 else "-",
        "linewidth": 1.8 if b in (F, F + 1) else 1.1,
        "alpha": 1.0 if b in (F, F + 1) else 0.75,
        "marker": "o",
        "markersize": 3.5,
    }


def save(fig: plt.Figure, output_dir: Path, stem: str) -> None:
    fig.savefig(output_dir / f"{stem}.pdf", bbox_inches="tight")
    fig.savefig(output_dir / f"{stem}.png", dpi=300, bbox_inches="tight")
    plt.close(fig)


def load_calibration_summary(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def calibration_delta_row(summary: dict, delta_m: float) -> dict:
    for row in summary["deltas"]:
        if abs(float(row["delta_m"]) - delta_m) < 1e-9:
            return row
    raise KeyError(f"delta_m={delta_m} not in calibration summary")


def calibration_rate(row: dict, k: float) -> tuple[float, float, float]:
    """Return (rate, wilson_low, wilson_high) for quantized pooled accept rate."""
    cell = row["pooled"][f"{k:g}"]
    return float(cell["rate"]), float(cell["low"]), float(cell["high"])


def calibration_regime(delta_m: float, boundary: float = 1.6) -> str:
    if abs(delta_m - boundary) <= 1e-12:
        return "boundary"
    if delta_m < boundary:
        return "true_lane"
    return "false_lane"


def pooled_q1_fn(row: dict[str, str]) -> float:
    q1 = number(row, "pooled_q1_empirical")
    if not math.isnan(q1):
        return max(0.0, 1.0 - q1)
    accepts = number(row, "honest_lane_accepts_total", 0)
    evaluated = number(row, "honest_lane_evaluations_total", 0)
    if evaluated <= 0:
        return math.nan
    return max(0.0, 1.0 - accepts / evaluated)


def pooled_q0_fp(row: dict[str, str]) -> float:
    q0 = number(row, "pooled_q0_empirical")
    if not math.isnan(q0):
        return q0
    accepts = number(row, "honest_lane_accepts_total", 0)
    evaluated = number(row, "honest_lane_evaluations_total", 0)
    if evaluated <= 0:
        return math.nan
    return accepts / evaluated


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
            x, y,
            yerr=[[max(0.0, yy - ll) for yy, ll in zip(y, lo)],
                  [max(0.0, hh - yy) for yy, hh in zip(y, hi)]],
            fmt="none", ecolor=style["color"], alpha=0.4, capsize=1.5, linewidth=0.8,
        )
    ax.set_xlabel(xlabel)
    ax.set_ylim(-0.04, 1.04)
    ax.grid(True, alpha=0.25)


def fig_safety_axes(delta_rows, k_rows, sigma_rows, output_dir: Path) -> None:
    setup_icra_style("double")
    fig, axes = plt.subplots(1, 3, figsize=(7.0, 1.85), sharey=True)
    plot_rate_lines(axes[0], [r for r in delta_rows if number(r, "b") >= 1], "delta_m",
                     r"Claim displacement $\delta$ [$\mathrm{m}$]")
    axes[0].set_title(r"(a) Adversary axis", fontsize=8)
    axes[0].set_ylabel("False-cert. / unsafe\nco-occ. rate")
    plot_rate_lines(axes[1], k_rows, "k", r"Gate tolerance $k$")
    axes[1].set_title(r"(b) Defender knob", fontsize=8)
    plot_rate_lines(axes[2], sigma_rows, "sigma_lat_m", r"Lateral noise $\sigma_{lat}$ [$\mathrm{m}$]")
    axes[2].set_title(r"(c) Environment axis", fontsize=8)
    handles, labels = axes[2].get_legend_handles_labels()
    fig.subplots_adjust(bottom=0.18, wspace=0.08)
    fig.legend(handles, labels, loc="lower center", ncol=4, frameon=False, bbox_to_anchor=(0.5, -0.08))
    save(fig, output_dir, "icra_two_lane_safety_axes")


def fig_k_operating_point(
    k_rows: list[dict[str, str]],
    honest_k_rows: list[dict[str, str]],
    output_dir: Path,
) -> None:
    """Honest q1 target vs shoulder false-cert as k varies — matches Exp~1 writing."""
    setup_icra_style("single")
    fig, (ax_q1, ax_fp) = plt.subplots(2, 1, figsize=(3.5, 3.8), sharex=True)

    honest = sorted(honest_k_rows, key=lambda r: number(r, "k"))
    if honest:
        x = [number(r, "k") for r in honest]
        y = [number(r, "pooled_q1_empirical") for r in honest]
        lo = [number(r, "q1_wilson95_low") for r in honest]
        hi = [number(r, "q1_wilson95_high") for r in honest]
        ax_q1.errorbar(
            x, y,
            yerr=[[max(0.0, yy - ll) for yy, ll in zip(y, lo)],
                  [max(0.0, hh - yy) for yy, hh in zip(y, hi)]],
            fmt="o-", color=WONG[3], markersize=4.0, linewidth=1.4,
            capsize=2.0, elinewidth=0.8, label=r"Measured $\hat q_1$",
        )
        for idx, row in enumerate(honest):
            ax_q1.plot(number(row, "k"), number(row, "analytic_q1"), "s",
                       color=WONG[0], markersize=3.5,
                       label=r"Analytic $q_1$" if idx == 0 else None)
    ax_q1.axhline(0.95, color="0.45", linestyle=":", linewidth=1.0)
    ax_q1.axvline(2.0, color=WONG[4], linestyle=":", linewidth=1.0, alpha=0.85)
    ax_q1.text(2.04, 0.62, r"$k{=}2$", fontsize=6, color=WONG[4])
    ax_q1.text(1.05, 0.955, r"95\% target", fontsize=6, color="0.45")
    ax_q1.set_ylabel(r"Witness accept rate $\hat q_1$")
    ax_q1.set_ylim(0.55, 1.02)
    ax_q1.set_title(r"(a) Honest admission ($\delta{=}0$)", fontsize=8)
    ax_q1.grid(True, alpha=0.25)
    ax_q1.legend(frameon=False, fontsize=6, loc="lower right")

    shoulder = sorted(
        (r for r in k_rows if int(float(r["b"])) == F),
        key=lambda r: number(r, "k"),
    )
    if shoulder:
        x = [number(r, "k") for r in shoulder]
        y = [number(r, "false_certificate_rate") for r in shoulder]
        lo = [number(r, "wilson95_low") for r in shoulder]
        hi = [number(r, "wilson95_high") for r in shoulder]
        ax_fp.errorbar(
            x, y,
            yerr=[[max(0.0, yy - ll) for yy, ll in zip(y, lo)],
                  [max(0.0, hh - yy) for yy, hh in zip(y, hi)]],
            fmt="o-", color=WONG[4], markersize=4.0, linewidth=1.4,
            capsize=2.0, elinewidth=0.8,
        )
    ax_fp.axvline(2.0, color=WONG[4], linestyle=":", linewidth=1.0, alpha=0.85)
    ax_fp.set_xlabel(r"Gate tolerance $k$")
    ax_fp.set_ylabel("False-cert.\ rate")
    ax_fp.set_ylim(-0.02, 1.02)
    ax_fp.set_xticks([1.0, 2.0, 3.0])
    ax_fp.set_title(r"(b) Shoulder attack ($b{=}f$)", fontsize=8)
    ax_fp.grid(True, alpha=0.25)
    fig.subplots_adjust(hspace=0.35)
    save(fig, output_dir, "icra_k_operating_point")


def fig_gate_accept_delta(calibration: dict, output_dir: Path) -> None:
    """Witness accept rate vs claim displacement — offline gate calibration."""
    setup_icra_style("single")
    fig, ax = plt.subplots(figsize=(3.5, 2.2))
    k_values = sorted(float(k) for k in calibration["k_values"])
    k_colors = {2.0: WONG[4], 2.5: WONG[3], 3.0: WONG[2]}

    for k in k_values:
        xs, ys, lo, hi = [], [], [], []
        for row in calibration["deltas"]:
            delta = float(row["delta_m"])
            if calibration_regime(delta) == "boundary":
                continue
            rate, wlo, whi = calibration_rate(row, k)
            xs.append(delta)
            ys.append(rate)
            lo.append(wlo)
            hi.append(whi)
        color = k_colors.get(k, WONG[0])
        ax.plot(xs, ys, marker="o", color=color, linewidth=1.2, markersize=3.0,
                label=rf"$k={k:g}$")
        ax.fill_between(xs, lo, hi, color=color, alpha=0.12)

    ax.axvline(1.6, color="0.35", linestyle=":", linewidth=0.9)
    ax.axvline(1.75, color=WONG[4], linestyle="--", linewidth=0.9, alpha=0.7)
    ax.text(1.78, 0.08, r"OP $\delta$", fontsize=6, color=WONG[4])
    ax.set_xlabel(r"Claim displacement $\delta$ [$\mathrm{m}$]")
    ax.set_ylabel(r"P(witness accept)")
    ax.set_xlim(-0.05, 3.35)
    ax.set_ylim(-0.02, 1.02)
    ax.grid(True, alpha=0.25)
    ax.legend(frameon=False, fontsize=6, loc="upper right", title="Tolerance")
    fig.subplots_adjust(bottom=0.16)
    save(fig, output_dir, "icra_gate_accept_delta")


def fig_direction_ablation(rows: list[dict[str, str]], output_dir: Path) -> None:
    """Exp 3: layer ablation — unsafe co-occupancy and throughput."""
    setup_icra_style("single")
    fig, (ax_unsafe, ax_thr) = plt.subplots(1, 2, figsize=(3.5, 2.2))
    attacked = [r for r in rows if r.get("attack_kind") == "FALSE_DIRECTION"]
    by_mode = {m: next(r for r in attacked if r.get("ablation_mode") == m) for m in DIRECTION_MODE_ORDER}

    modes = DIRECTION_MODE_ORDER
    labels = [DIRECTION_MODE_LABEL[m][0] for m in modes]
    colors = [DIRECTION_MODE_LABEL[m][1] for m in modes]
    x = list(range(len(modes)))

    unsafe = [number(by_mode[m], "unsafe_cooccupancy_rate") for m in modes]
    ax_unsafe.bar(x, unsafe, color=colors, width=0.55)
    ax_unsafe.set_ylim(-0.04, 1.04)
    ax_unsafe.set_ylabel("Unsafe co-occupancy")
    ax_unsafe.set_title("(a) Physical FP", fontsize=8)
    ax_unsafe.set_xticks(x)
    ax_unsafe.set_xticklabels(labels, rotation=25, ha="right", fontsize=5.5)
    ax_unsafe.grid(True, alpha=0.25, axis="y")

    throughput = [number(by_mode[m], "mean_throughput_veh_per_min") for m in modes]
    ax_thr.bar(x, throughput, color=colors, width=0.55)
    ax_thr.set_ylabel(r"Throughput [veh/min]")
    ax_thr.set_ylim(0, max(throughput) * 1.12)
    ax_thr.set_title("(b) Service", fontsize=8)
    ax_thr.set_xticks(x)
    ax_thr.set_xticklabels(labels, rotation=25, ha="right", fontsize=5.5)
    ax_thr.grid(True, alpha=0.25, axis="y")

    fig.subplots_adjust(bottom=0.28, wspace=0.38)
    save(fig, output_dir, "icra_direction_ablation")


def fig_direction_fp_fn(rows: list[dict[str, str]], output_dir: Path) -> None:
    """Exp 4: FP/FN proxy only (Option C companion panel)."""
    setup_icra_style("single")
    fig, ax = plt.subplots(figsize=(3.5, 2.4))
    attacked = [r for r in rows if r.get("attack_kind") == "FALSE_DIRECTION"]
    by_mode = {m: next(r for r in attacked if r.get("ablation_mode") == m) for m in DIRECTION_MODE_ORDER}
    modes = DIRECTION_MODE_ORDER
    labels = [DIRECTION_MODE_LABEL[m][0] for m in modes]
    x = list(range(len(modes)))
    width = 0.36
    fp_vals = [number(by_mode[m], "false_eligibility_rate") for m in modes]
    fn_vals = [number(by_mode[m], "mean_signed_unknown_singleton_percent") / 100.0 for m in modes]
    ax.bar([i - width / 2 for i in x], fp_vals, width=width, color=WONG[5], label="FP (false eligibility)")
    ax.bar([i + width / 2 for i in x], fn_vals, width=width, color=WONG[3], label="FN proxy (signed-unknown)")
    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=20, ha="right", fontsize=6)
    ax.set_ylim(-0.04, 1.04)
    ax.set_ylabel("Rate")
    ax.set_title("Direction cue FP / FN proxy", fontsize=8)
    ax.grid(True, alpha=0.25, axis="y")
    ax.legend(frameon=False, fontsize=6, loc="upper right")
    fig.subplots_adjust(bottom=0.28)
    save(fig, output_dir, "icra_direction_fp_fn")


def unique_cells(sources) -> list[dict[str, str]]:
    cells: dict[tuple, dict[str, str]] = {}
    for rows in sources:
        for row in rows:
            b = number(row, "b")
            if b < 1:
                continue
            key = (b, number(row, "sigma_lat_m"), number(row, "delta_m"), number(row, "k"))
            cells[key] = row
    return list(cells.values())


def fig_model_validation(rows, output_dir: Path) -> None:
    setup_icra_style("double")
    fig, axes = plt.subplots(1, 2, figsize=(7.0, 2.9))
    ax_q0, ax_cert = axes

    for b in range(1, F + 2):
        group = [r for r in rows if int(number(r, "b")) == b]
        color = WONG[(b - 1) % len(WONG)]
        for row in group:
            sigma, delta, k = number(row, "sigma_lat_m"), number(row, "delta_m"), number(row, "k")
            x = analytic_q0(sigma, delta, k)
            y = number(row, "pooled_q0_empirical")
            accepted = int(number(row, "honest_lane_accepts_total", 0))
            evaluated = int(number(row, "honest_lane_evaluations_total", 0))
            lo, hi = wilson(accepted, evaluated)
            if not math.isnan(y):
                ax_q0.errorbar(x, y, yerr=[[max(0.0, y - lo)], [max(0.0, hi - y)]],
                                fmt="o", color=color, alpha=0.6, markersize=3.2, capsize=1.2, elinewidth=0.7)

            pred = number(row, "mean_binomial_prediction")
            observed = number(row, "false_certificate_rate")
            obs_lo, obs_hi = number(row, "wilson95_low"), number(row, "wilson95_high")
            if not math.isnan(pred):
                ax_cert.errorbar(pred, observed, yerr=[[max(0.0, observed - obs_lo)], [max(0.0, obs_hi - observed)]],
                                  fmt="o", color=color, alpha=0.65, markersize=3.2, capsize=1.2, elinewidth=0.7)

    for ax in axes:
        ax.plot([0, 1], [0, 1], color="black", linestyle="--", linewidth=1.0)
        ax.set_xlim(-0.03, 1.03)
        ax.set_ylim(-0.03, 1.03)
        ax.grid(True, alpha=0.25)

    ax_q0.set_title("(a) Sensor-model validation", fontsize=8)
    ax_q0.set_xlabel(r"Analytic Gaussian $q_0$")
    ax_q0.set_ylabel(r"Measured witness false-accept rate $\hat q_0$")
    ax_cert.set_title("(b) Protocol-model validation", fontsize=8)
    ax_cert.set_xlabel(r"Binomial prediction (measured $h$, $b_{sig}$)")
    ax_cert.set_ylabel("Measured false-certificate rate")

    legend_handles = [Line2D([0], [0], marker="o", linestyle="none", color=WONG[(b - 1) % len(WONG)], label=b_label(b))
                       for b in range(1, F + 2)]
    legend_handles.append(Line2D([0], [0], linestyle="--", color="black", label="ideal agreement"))
    fig.subplots_adjust(bottom=0.18, wspace=0.30)
    fig.legend(handles=legend_handles, loc="lower center", ncol=4, frameon=False, bbox_to_anchor=(0.5, -0.10))
    save(fig, output_dir, "icra_two_stage_model_validation")


def fig_model_validation_q0(rows, output_dir: Path) -> None:
    setup_icra_style("single")
    fig, ax = plt.subplots(figsize=(3.5, 2.4))
    for b in range(1, F + 2):
        color = WONG[(b - 1) % len(WONG)]
        for row in [r for r in rows if int(number(r, "b")) == b]:
            x = analytic_q0(number(row, "sigma_lat_m"), number(row, "delta_m"), number(row, "k"))
            y = number(row, "pooled_q0_empirical")
            accepted = int(number(row, "honest_lane_accepts_total", 0))
            evaluated = int(number(row, "honest_lane_evaluations_total", 0))
            lo, hi = wilson(accepted, evaluated)
            if not math.isnan(y):
                ax.errorbar(x, y, yerr=[[max(0.0, y - lo)], [max(0.0, hi - y)]],
                            fmt="o", color=color, alpha=0.65, markersize=3.0, capsize=1.2, elinewidth=0.7)
    ax.plot([0, 1], [0, 1], color="black", linestyle="--", linewidth=1.0)
    ax.set_xlim(-0.03, 1.03)
    ax.set_ylim(-0.03, 1.03)
    ax.set_xlabel(r"Analytic Gaussian $q_0$")
    ax.set_ylabel(r"Measured $\hat q_0$")
    ax.grid(True, alpha=0.25)
    fig.tight_layout(pad=0.3)
    save(fig, output_dir, "icra_model_validation_q0")


def fig_model_validation_cert(rows, output_dir: Path) -> None:
    setup_icra_style("single")
    fig, ax = plt.subplots(figsize=(3.5, 2.4))
    for b in range(1, F + 2):
        color = WONG[(b - 1) % len(WONG)]
        for row in [r for r in rows if int(number(r, "b")) == b]:
            pred = number(row, "mean_binomial_prediction")
            observed = number(row, "false_certificate_rate")
            obs_lo, obs_hi = number(row, "wilson95_low"), number(row, "wilson95_high")
            if not math.isnan(pred):
                ax.errorbar(pred, observed,
                            yerr=[[max(0.0, observed - obs_lo)], [max(0.0, obs_hi - observed)]],
                            fmt="o", color=color, alpha=0.65, markersize=3.0, capsize=1.2, elinewidth=0.7)
    ax.plot([0, 1], [0, 1], color="black", linestyle="--", linewidth=1.0)
    ax.set_xlim(-0.03, 1.03)
    ax.set_ylim(-0.03, 1.03)
    ax.set_xlabel(r"Binomial prediction")
    ax.set_ylabel("False-cert.\ rate")
    ax.grid(True, alpha=0.25)
    fig.tight_layout(pad=0.3)
    save(fig, output_dir, "icra_model_validation_cert")




ROLE_LABEL = {
    "honest": ("honest ($b=0$)", WONG[3], "-"),
    "shoulder_bf": (r"shoulder ($b=f$)", WONG[4], "-"),
    "cliff_bf1": (r"cliff ($b=f{+}1$)", WONG[5], "--"),
}
ROLE_ORDER = ["honest", "shoulder_bf", "cliff_bf1"]


def fig_scaling_crossover(rows: list[dict[str, str]], output_dir: Path) -> None:
    width, height = setup_icra_style("single")
    fig, ax = plt.subplots(figsize=(width, height))
    by_role: dict[str, list[dict[str, str]]] = {r: [] for r in ROLE_ORDER}
    for row in rows:
        role = row.get("role", "")
        if role in by_role:
            by_role[role].append(row)
    for role in ROLE_ORDER:
        group = sorted(by_role[role], key=lambda r: number(r, "n"))
        if not group:
            continue
        label, color, style = ROLE_LABEL[role]
        x = [number(r, "n") for r in group]
        y = [number(r, "conflicting_cooccupancy_rate") for r in group]
        lo = [number(r, "conflicting_cooccupancy_wilson95_low") for r in group]
        hi = [number(r, "conflicting_cooccupancy_wilson95_high") for r in group]
        ax.plot(x, y, label=label, color=color, linestyle=style, marker="o",
                linewidth=1.6, markersize=3.5)
        ax.errorbar(x, y, yerr=[[max(0.0, yy - ll) for yy, ll in zip(y, lo)],
                                [max(0.0, hh - yy) for yy, hh in zip(y, hi)]],
                    fmt="none", ecolor=color, alpha=0.45, capsize=1.5, linewidth=0.8)
    ax.set_xlabel(r"Intersection size $N$ [$\mathrm{vehicles}$]")
    ax.set_ylabel("Conflicting co-occupancy rate")
    ax.set_xticks(sorted({number(r, "n") for r in rows}))
    ax.set_ylim(-0.04, 1.04)
    ax.grid(True, alpha=0.25)
    ax.legend(frameon=False, loc="upper right")
    fig.tight_layout()
    save(fig, output_dir, "icra_scaling_crossover")


# --- Exp 6: RSU resilience floor -------------------------------------------
# Transcribed from origin/feature/rsu-rollback-ablation @ 5b3e52fb
# (benchmarks/rsu_ablation/figures/4veh_report.md,
#  benchmarks/rsu_ablation/figures/18veh_availability_report.md).
# Branch is not merged/reviewed -- treat as provisional.
RSU_18VEH_K = [0, 1, 2, 3, 4, 5, 6, 7, 8]
RSU_18VEH_OFF = [1.00, 1.00, 1.00, 1.00, 1.00, 0.50, 0.00, 0.00, 0.00]  # n=6 each
RSU_18VEH_ON = [1.00, 1.00, 1.00, 1.00, 1.00, 1.00, 1.00, 0.83, 0.00]  # n=6 each

RSU_4VEH_K = [0, 1, 2, 3]
RSU_4VEH_MSGS_OFF = [943, 932, 295, 294]  # k>=2: OFF fails, silent replicas stop transmitting
RSU_4VEH_MSGS_ON = [1198, 1144, 1066, 740]
RSU_4VEH_LAT_OFF = [0.06, 0.07, None, None]
RSU_4VEH_LAT_ON = [0.08, 0.09, 0.10, 0.10]


def fig_rsu_resilience(output_dir: Path) -> None:
    setup_icra_style("single")
    fig, (ax_avail, ax_cost) = plt.subplots(2, 1, figsize=(3.5, 3.6), sharex=False)

    ax_avail.plot(RSU_18VEH_K, RSU_18VEH_OFF, label="OFF ($N{=}18$)", color=WONG[5],
                  linestyle="--", marker="o", linewidth=1.6, markersize=3.5)
    ax_avail.plot(RSU_18VEH_K, RSU_18VEH_ON, label="ON ($N{=}22$, $+4$ RSU)", color=WONG[4],
                  linestyle="-", marker="o", linewidth=1.6, markersize=3.5)
    ax_avail.set_xlabel(r"Silent (omission-faulty) replicas $k$")
    ax_avail.set_ylabel("Epoch-0 commit success rate")
    ax_avail.set_ylim(-0.04, 1.04)
    ax_avail.set_title("(a) Fault-tolerance frontier", fontsize=8)
    ax_avail.grid(True, alpha=0.25)
    ax_avail.legend(frameon=False, loc="lower left")

    ax_cost.plot(RSU_4VEH_K, RSU_4VEH_MSGS_OFF, label="OFF ($N{=}4$)", color=WONG[5],
                 linestyle="--", marker="s", linewidth=1.6, markersize=3.5)
    ax_cost.plot(RSU_4VEH_K, RSU_4VEH_MSGS_ON, label="ON ($N{=}8$, $+4$ RSU)", color=WONG[4],
                 linestyle="-", marker="s", linewidth=1.6, markersize=3.5)
    ax_cost.set_xlabel(r"Silent replicas $k$")
    ax_cost.set_ylabel(r"Messages sent per run")
    ax_cost.set_title("(b) Message cost ($N{=}4$)", fontsize=8)
    ax_cost.grid(True, alpha=0.25)
    ax_cost.legend(frameon=False, loc="upper right")
    ax_cost.annotate("OFF fails\nfrom $k{=}2$", xy=(2, 295), xytext=(1.05, 520),
                      fontsize=6, color=WONG[5], arrowprops=dict(arrowstyle="->", color=WONG[5], lw=0.7))

    fig.subplots_adjust(bottom=0.20, wspace=0.32)
    save(fig, output_dir, "icra_rsu_resilience")


def filter_sweep_rows(
    rows: list[dict[str, str]],
    *,
    sigma_lat_m: float | None = None,
    delta_m: float | None = None,
    k: float | None = None,
    min_b: int = 1,
) -> list[dict[str, str]]:
    """Keep aggregate rows matching frozen sweep axes (Exp~1 one-axis sweeps)."""
    out: list[dict[str, str]] = []
    for row in rows:
        if int(float(row["b"])) < min_b:
            continue
        if sigma_lat_m is not None and abs(number(row, "sigma_lat_m") - sigma_lat_m) >= 1e-9:
            continue
        if delta_m is not None and abs(number(row, "delta_m") - delta_m) >= 1e-9:
            continue
        if k is not None and abs(number(row, "k") - k) >= 1e-9:
            continue
        out.append(row)
    return out


def validate_sweep_data(
    delta_rows: list[dict[str, str]],
    k_rows: list[dict[str, str]],
    sigma_rows: list[dict[str, str]],
    adversarial_rows: list[dict[str, str]],
    honest_k_rows: list[dict[str, str]],
) -> None:
    """Fail fast if a wrong aggregate file was wired in (see module DATA_SOURCES)."""
    checks = [
        ("delta sweep", len(delta_rows), EXPECTED_SWEEP_CELLS["delta"],
         "E2 delta_b_completion_aggregates.csv @ sigma=0.5, k=3"),
        ("k sweep", len(k_rows), EXPECTED_SWEEP_CELLS["k"],
         "E2 k_sweep_full_aggregates.csv @ sigma=0.5, delta=1.75"),
        ("sigma sweep", len(sigma_rows), EXPECTED_SWEEP_CELLS["sigma"],
         "E2 sigma_sweep_full_aggregates.csv @ delta=1.75, k=3"),
        ("adversarial scaling", len(adversarial_rows), EXPECTED_ADVERSARIAL_ROWS,
         "E5 two_lane_scale_adversarial_full_aggregates.csv (not smoke)"),
        ("honest k selection", len(honest_k_rows), EXPECTED_HONEST_K_ROWS,
         "E5 honest_operating_k-full_aggregates.csv"),
    ]
    for label, got, want, hint in checks:
        if got != want:
            raise SystemExit(
                f"DATA_SOURCES mismatch for {label}: expected {want} rows, got {got}. "
                f"Use {hint}.  See plot_icra_camera_ready.py docstring."
            )

    delta_values = sorted({number(r, "delta_m") for r in delta_rows})
    if delta_values != [1.61, 1.75, 2.0]:
        raise SystemExit(
            f"delta sweep should cover delta in {{1.61, 1.75, 2.0}}, got {delta_values}"
        )

    n8_cliff = next(
        (r for r in adversarial_rows if r.get("role") == "cliff_bf1" and number(r, "n") == 8),
        None,
    )
    if n8_cliff is not None and number(n8_cliff, "conflicting_cooccupancy_rate") < 0.5:
        raise SystemExit(
            "N=8 cliff co-occupancy < 50% -- likely smoke-tier data "
            "(two_lane_scale_adversarial_smoke_*).  Use the full aggregate."
        )


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    output_dir = root / "experiments" / "shared" / "results" / "Phase2ICRAFigures"
    output_dir.mkdir(parents=True, exist_ok=True)

    e2 = root / "experiments" / "e2_lateral_lane_resilience" / "artifacts" / "curated"
    e4 = root / "experiments" / "e4_direction_eligibility" / "artifacts" / "curated"
    e5 = root / "experiments" / "e5_operating_point" / "artifacts" / "curated"
    calibration_path = root / "fourway" / "adjacent_lane_calibration" / "canonical_validation_summary.json"

    # Exp~1 one-axis sweeps (20 reps/cell).  Do NOT use two_lane_grid_aggregates.csv
    # for panel (a): that file mixes axes and only has one delta point at b=f.
    delta_rows = filter_sweep_rows(
        read_csv(e2 / "delta_b_completion_aggregates.csv"),
        sigma_lat_m=0.5,
        k=3.0,
    )
    k_rows = filter_sweep_rows(
        read_csv(e2 / "k_sweep_full_aggregates.csv"),
        sigma_lat_m=0.5,
        delta_m=1.75,
    )
    sigma_rows = filter_sweep_rows(
        read_csv(e2 / "sigma_sweep_full_aggregates.csv"),
        delta_m=1.75,
        k=3.0,
    )
    adversarial_rows = read_csv(e5 / "two_lane_scale_adversarial_full_aggregates.csv")
    direction_rows = read_csv(e4 / "direction_ablation_straight_heavy_full_aggregates.csv")
    calibration = load_calibration_summary(calibration_path)

    validation_rows = unique_cells((delta_rows, k_rows, sigma_rows))
    honest_k_rows = read_csv(e5 / "honest_operating_k-full_aggregates.csv")
    validate_sweep_data(delta_rows, k_rows, sigma_rows, adversarial_rows, honest_k_rows)
    fig_safety_axes(delta_rows, k_rows, sigma_rows, output_dir)
    fig_gate_accept_delta(calibration, output_dir)
    fig_model_validation_q0(validation_rows, output_dir)
    fig_model_validation_cert(validation_rows, output_dir)
    fig_k_operating_point(k_rows, honest_k_rows, output_dir)
    fig_model_validation(validation_rows, output_dir)
    fig_scaling_crossover(adversarial_rows, output_dir)
    fig_direction_ablation(direction_rows, output_dir)
    fig_direction_fp_fn(direction_rows, output_dir)
    fig_rsu_resilience(output_dir)

    print(f"Wrote camera-ready ICRA figures to {output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

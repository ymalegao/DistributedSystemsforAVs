#!/usr/bin/env python3
"""Exploratory analysis for E5 operating-point + adversarial scale.

Joins the two headline result roots:

  1) Phase2TwoLaneHonestOperatingSweep
     — honest k selection at σ_lat=0.5 (q1, certification, throughput)
  2) Phase2TwoLaneScaleAdversarialFull
     — N∈{4,8,16,20} × {honest, shoulder b=f, cliff b=f+1}

Optionally cross-checks the curated honest-scale aggregates (reused into
adversarial full). IEEE/ICRA sizing belongs in a separate publication script.

Usage:
  python analyze_log.py
  python analyze_log.py --no-plots
"""

from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path
from typing import Any


WONG = {
    "honest": "#0072B2",
    "shoulder_bf": "#E69F00",
    "cliff_bf1": "#D55E00",
}
ROLE_LABEL = {
    "honest": "honest",
    "shoulder_bf": r"shoulder $b=f$",
    "cliff_bf1": r"cliff $b=f{+}1$",
}
SELECTED = {"k": 2.0, "sigma_lat_m": 0.5, "delta_m": 1.75}


def _read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def _num(row: dict[str, Any], key: str, default: float = math.nan) -> float:
    value = row.get(key, "")
    if value in (None, ""):
        return default
    return float(value)


def _paths(results_root: Path) -> dict[str, Path]:
    return {
        "honest_csv": results_root
        / "Phase2TwoLaneHonestOperatingSweep"
        / "honest_operating_k-full_aggregates.csv",
        "honest_sum": results_root
        / "Phase2TwoLaneHonestOperatingSweep"
        / "honest_operating_k-full_summary.json",
        "adv_csv": results_root
        / "Phase2TwoLaneScaleAdversarialFull"
        / "two_lane_scale_adversarial_full_aggregates.csv",
        "adv_sum": results_root
        / "Phase2TwoLaneScaleAdversarialFull"
        / "two_lane_scale_adversarial_full_summary.json",
        "honest_scale_csv": results_root.parent
        / "artifacts"
        / "curated"
        / "two_lane_scale_honest_full_aggregates.csv",
    }


def _print_header(paths: dict[str, Path], honest_sum: dict[str, Any], adv_sum: dict[str, Any]) -> None:
    print("=" * 78)
    print("E5 Operating Point — exploratory analyze_log")
    print("=" * 78)
    print(
        f"manifest selected OP: k={SELECTED['k']:g}, "
        f"σ_lat={SELECTED['sigma_lat_m']}, δ={SELECTED['delta_m']}"
    )
    print(
        f"honest sweep: planned={honest_sum.get('planned_unique_runs')} "
        f"completed={honest_sum.get('completed_unique_runs')} "
        f"passed={honest_sum.get('passed')}  profile={honest_sum.get('profile')}"
    )
    print(f"honest purpose: {honest_sum.get('purpose')}")
    print(
        f"adversarial: planned={adv_sum.get('total_planned')} "
        f"(reused honest {adv_sum.get('reused_honest_completed')} + "
        f"new attack {adv_sum.get('new_attack_completed')}) "
        f"passed={adv_sum.get('passed')}  profile={adv_sum.get('profile')}"
    )
    print()
    for name, path in paths.items():
        print(f"  [{'OK' if path.exists() else 'MISSING'}] {name}: {path.name}")
    print()


def _print_honest_selection(rows: list[dict[str, str]], selection: dict[str, Any]) -> None:
    print("--- Honest k selection (σ_lat=0.5, b=0, δ=0) ---")
    print(
        f"{'k':>4} {'q1 emp':>8} {'q1 an':>8} {'cert':>6} {'quiet%':>7} "
        f"{'thr':>7} {'wait':>7} {'batch':>6} {'q1∈W':>5}"
    )
    print("-" * 72)
    for r in sorted(rows, key=lambda x: _num(x, "k")):
        print(
            f"{_num(r, 'k'):>4.1f} "
            f"{_num(r, 'pooled_q1_empirical'):>8.4f} "
            f"{_num(r, 'analytic_q1'):>8.4f} "
            f"{_num(r, 'lane_certification_rate'):>6.3f} "
            f"{_num(r, 'quiet_percent'):>7.2f} "
            f"{_num(r, 'throughput_veh_per_min'):>7.2f} "
            f"{_num(r, 'wait_mean_s'):>7.2f} "
            f"{_num(r, 'mean_batch_size'):>6.3f} "
            f"{str(r.get('q1_model_inside_wilson')):>5}"
        )
    print()
    print("orchestrator selection block:")
    print(f"  status: {selection.get('status')}")
    print(f"  system criterion: {selection.get('system_criterion')}")
    print(f"  system candidates k: {selection.get('system_candidate_k')}")
    print(f"  smallest system k: {selection.get('smallest_system_candidate_k')}")
    print(f"  strict sensor criterion: {selection.get('strict_sensor_criterion')}")
    print(f"  strict sensor candidates: {selection.get('strict_sensor_candidate_k')}")
    print(f"  note: {selection.get('note')}")
    print(
        "  Paper note: manifest locks k=2 (E2 attack resilience), not the "
        "strict-sensor k=3; honest sweep shows k=1–3 all meet the system criterion "
        "while q1 jumps from ~0.69 (k=1) → ~0.95 (k=2) → ~0.997 (k=3)."
    )
    print()


def _print_adversarial(rows: list[dict[str, str]]) -> None:
    print("--- Adversarial multiscale full (honest / shoulder b=f / cliff b=f+1) ---")
    print(
        f"{'N':>3} {'f':>2} {'role':<12} {'b':>3} "
        f"{'FE':>5} {'UC':>5} {'coll':>5} "
        f"{'thr':>7} {'wait':>7} {'batch':>6}"
    )
    print("-" * 72)
    for r in sorted(rows, key=lambda x: (int(x["n"]), int(x["configured_b"]))):
        print(
            f"{int(r['n']):>3} {int(r['f']):>2} {r['role']:<12} {int(r['configured_b']):>3} "
            f"{_num(r, 'false_certificate_rate'):>5.2f} "
            f"{_num(r, 'conflicting_cooccupancy_rate'):>5.2f} "
            f"{_num(r, 'sumo_collision_rate'):>5.2f} "
            f"{_num(r, 'mean_throughput_veh_per_min'):>7.2f} "
            f"{_num(r, 'mean_wait_s'):>7.2f} "
            f"{_num(r, 'mean_batch_size'):>6.3f}"
        )
    print()
    print(
        "Pattern: honest FE=UC=0 at every N; cliff b=f+1 FE=1.0 always; "
        "shoulder b=f is partial (0.15–0.50) and UC < FE (false authority does not "
        "always become unsafe co-occupancy). Throughput still scales with N."
    )
    print()


def _print_honest_scale_crosscheck(
    adv_rows: list[dict[str, str]], honest_scale: list[dict[str, str]]
) -> None:
    if not honest_scale:
        print("--- Honest-scale curated cross-check: missing ---")
        print()
        return
    print("--- Honest-scale curated vs adversarial reused honest rows ---")
    by_adv = {
        int(r["n"]): r for r in adv_rows if r["role"] == "honest"
    }
    mismatches = 0
    for r in honest_scale:
        n = int(r["n"])
        a = by_adv.get(n)
        if a is None:
            print(f"  N={n}: missing in adversarial aggregates")
            mismatches += 1
            continue
        for field_h, field_a in (
            ("mean_throughput_veh_per_min", "mean_throughput_veh_per_min"),
            ("mean_wait_s", "mean_wait_s"),
            ("unsafe_cooccupancy_rate", "conflicting_cooccupancy_rate"),
        ):
            hv, av = _num(r, field_h), _num(a, field_a)
            if abs(hv - av) > 1e-9:
                print(f"  N={n} {field_h}: honest_scale={hv} adv={av}")
                mismatches += 1
    print(f"  mismatches: {mismatches}")
    print()


def _recommend() -> None:
    print("=" * 78)
    print("Candidate publication plot compositions")
    print("=" * 78)
    print(
        "E5 is two linked claims: (1) lock the operating point from honest k×q1, "
        "(2) show that OP scales under honest / shoulder / cliff adversaries."
    )
    print(
        "1. TWO-PANEL double-column (RECOMMENDED): "
        "(a) honest k-selection — q1 emp vs analytic (±Wilson) and/or lane-cert "
        "rate vs k, mark selected k=2; "
        "(b) adversarial scale — FE (or UC) vs N for honest / shoulder / cliff "
        "with Wilson CI. Optionally overlay throughput as a twin axis or small "
        "inset if space allows."
    )
    print(
        "2. Separate figures: single-column honest k figure + single-column "
        "N-scale safety figure. Cleaner if page budget is generous."
    )
    print(
        "3. Avoid packing throughput+wait+FE into one busy 3×3 grid; the scale "
        "story is safety-first, efficiency second (throughput rises with N for "
        "all roles)."
    )
    print(
        "4. Caption must say k=2 is locked from the joint E2+E5 story "
        "(attack resilience + adequate honest q1≈0.95), not from strict-sensor "
        "k=3 alone."
    )
    print()
    print(
        "Recommendation: ship (1). Honest sweep alone is weak as a standalone "
        "main figure; adversarial scale alone omits why k=2. Together they are E5."
    )
    print()


def _save_plots(
    honest_rows: list[dict[str, str]],
    adv_rows: list[dict[str, str]],
    out_dir: Path,
) -> None:
    try:
        import matplotlib.pyplot as plt
        import numpy as np
    except ImportError:
        print("matplotlib not available; skipping exploratory PNGs.")
        return

    out_dir.mkdir(parents=True, exist_ok=True)

    # --- Combined two-panel candidate ---
    fig, (ax0, ax1) = plt.subplots(1, 2, figsize=(11.5, 4.0))

    honest_rows = sorted(honest_rows, key=lambda r: _num(r, "k"))
    ks = [_num(r, "k") for r in honest_rows]
    q1 = [_num(r, "pooled_q1_empirical") for r in honest_rows]
    q1_lo = [_num(r, "q1_wilson95_low") for r in honest_rows]
    q1_hi = [_num(r, "q1_wilson95_high") for r in honest_rows]
    q1_an = [_num(r, "analytic_q1") for r in honest_rows]
    ax0.errorbar(
        ks,
        q1,
        yerr=[
            [max(0.0, y - lo) for y, lo in zip(q1, q1_lo)],
            [max(0.0, hi - y) for y, hi in zip(q1, q1_hi)],
        ],
        fmt="o-",
        color=WONG["honest"],
        capsize=3,
        label=r"empirical $q_1$",
    )
    ax0.plot(ks, q1_an, "s--", color="#009E73", label=r"analytic $q_1$")
    ax0.axvline(SELECTED["k"], color="0.4", linestyle=":", linewidth=1.1, label=r"selected $k=2$")
    ax0.set_xlabel(r"Gate tolerance $k$")
    ax0.set_ylabel(r"Honest true-accept $q_1$")
    ax0.set_ylim(0.6, 1.02)
    ax0.set_xticks(ks)
    ax0.grid(True, alpha=0.35, linestyle="--")
    ax0.legend(frameon=False, fontsize=8)
    ax0.set_title(r"(a) Honest $k$ selection ($\sigma_{lat}=0.5$)")

    for role in ("honest", "shoulder_bf", "cliff_bf1"):
        group = sorted(
            (r for r in adv_rows if r["role"] == role),
            key=lambda r: int(r["n"]),
        )
        if not group:
            continue
        x = [int(r["n"]) for r in group]
        y = [_num(r, "false_certificate_rate") for r in group]
        lo = [_num(r, "false_certificate_wilson95_low") for r in group]
        hi = [_num(r, "false_certificate_wilson95_high") for r in group]
        ax1.errorbar(
            x,
            y,
            yerr=[
                [max(0.0, yy - ll) for yy, ll in zip(y, lo)],
                [max(0.0, hh - yy) for yy, hh in zip(y, hi)],
            ],
            fmt="o-",
            color=WONG[role],
            capsize=3,
            label=ROLE_LABEL[role],
            linestyle="--" if role == "cliff_bf1" else "-",
        )
    ax1.set_xlabel(r"Fleet size $N$")
    ax1.set_ylabel("False certificate rate")
    ax1.set_ylim(-0.05, 1.08)
    ax1.set_xticks([4, 8, 16, 20])
    ax1.grid(True, alpha=0.35, linestyle="--")
    ax1.legend(frameon=False, fontsize=8)
    ax1.set_title(r"(b) Adversarial scale at locked OP")

    fig.suptitle("E5 exploratory: operating-point selection and adversarial scaling", y=1.03)
    fig.tight_layout()
    path = out_dir / "explore_e5_op_and_scale.png"
    fig.savefig(path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"wrote {path}")

    # --- Throughput scale companion ---
    fig, ax = plt.subplots(figsize=(6.0, 3.8))
    for role in ("honest", "shoulder_bf", "cliff_bf1"):
        group = sorted(
            (r for r in adv_rows if r["role"] == role),
            key=lambda r: int(r["n"]),
        )
        ax.plot(
            [int(r["n"]) for r in group],
            [_num(r, "mean_throughput_veh_per_min") for r in group],
            marker="o",
            color=WONG[role],
            linestyle="--" if role == "cliff_bf1" else "-",
            label=ROLE_LABEL[role],
        )
    ax.set_xlabel(r"Fleet size $N$")
    ax.set_ylabel(r"Throughput [$\mathrm{veh/min}$]")
    ax.set_xticks([4, 8, 16, 20])
    ax.grid(True, alpha=0.35, linestyle="--")
    ax.legend(frameon=False, fontsize=8)
    ax.set_title("E5 exploratory: throughput vs N (companion)")
    fig.tight_layout()
    path = out_dir / "explore_e5_throughput_vs_n.png"
    fig.savefig(path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"wrote {path}")

    # --- UC vs N ---
    fig, ax = plt.subplots(figsize=(6.0, 3.8))
    for role in ("honest", "shoulder_bf", "cliff_bf1"):
        group = sorted(
            (r for r in adv_rows if r["role"] == role),
            key=lambda r: int(r["n"]),
        )
        y = [_num(r, "conflicting_cooccupancy_rate") for r in group]
        lo = [_num(r, "conflicting_cooccupancy_wilson95_low") for r in group]
        hi = [_num(r, "conflicting_cooccupancy_wilson95_high") for r in group]
        x = [int(r["n"]) for r in group]
        ax.errorbar(
            x,
            y,
            yerr=[
                [max(0.0, yy - ll) for yy, ll in zip(y, lo)],
                [max(0.0, hh - yy) for yy, hh in zip(y, hi)],
            ],
            fmt="o-",
            color=WONG[role],
            capsize=3,
            linestyle="--" if role == "cliff_bf1" else "-",
            label=ROLE_LABEL[role],
        )
    ax.set_xlabel(r"Fleet size $N$")
    ax.set_ylabel("Unsafe co-occupancy rate")
    ax.set_ylim(-0.05, 1.08)
    ax.set_xticks([4, 8, 16, 20])
    ax.grid(True, alpha=0.35, linestyle="--")
    ax.legend(frameon=False, fontsize=8)
    ax.set_title("E5 exploratory: unsafe co-occupancy vs N")
    fig.tight_layout()
    path = out_dir / "explore_e5_uc_vs_n.png"
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
    )
    parser.add_argument("--plots-dir", type=Path, default=None)
    parser.add_argument("--no-plots", action="store_true")
    args = parser.parse_args()

    results_root = args.results_root.resolve()
    paths = _paths(results_root)
    for key in ("honest_csv", "honest_sum", "adv_csv", "adv_sum"):
        if not paths[key].exists():
            raise SystemExit(f"missing {paths[key]}")

    honest_rows = _read_csv(paths["honest_csv"])
    adv_rows = _read_csv(paths["adv_csv"])
    honest_sum = json.loads(paths["honest_sum"].read_text(encoding="utf-8"))
    adv_sum = json.loads(paths["adv_sum"].read_text(encoding="utf-8"))
    honest_scale = _read_csv(paths["honest_scale_csv"]) if paths["honest_scale_csv"].exists() else []

    _print_header(paths, honest_sum, adv_sum)
    _print_honest_selection(honest_rows, honest_sum.get("operating_point_selection", {}))
    _print_adversarial(adv_rows)
    _print_honest_scale_crosscheck(adv_rows, honest_scale)
    _recommend()

    if not args.no_plots:
        plots_dir = (args.plots_dir or (results_root / "explore_plots")).resolve()
        _save_plots(honest_rows, adv_rows, plots_dir)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

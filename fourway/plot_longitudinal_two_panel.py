#!/usr/bin/env python3
"""Generate the reviewer-facing longitudinal sensor/protocol figures."""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path


CAPTION = (
    "Panel 1 characterizes sensor-level order error (comparison of two noisy "
    "observations gives sqrt(2)*sigma). Panel 2 characterizes protocol-level "
    "committed per-pair inversion (independent per-car absolute-distance "
    "certification composes q_dist with measured h and attempt-local available "
    "b_sig). Certificate-local b_sig is retained separately for forensic "
    "accounting and is not used to predict attempts that fail to finalize. "
    "The gate does not perform pairwise relative sensing, so the sqrt(2) "
    "calibration curve is not the protocol prediction. Whole-queue ordering is "
    "reported empirically without an independence overlay."
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--grid-summary", type=Path, required=True)
    parser.add_argument("--calibration-summary", type=Path)
    args = parser.parse_args()
    grid_path = args.grid_summary.resolve()
    output_dir = grid_path.parent
    calibration_path = args.calibration_summary or (
        Path(__file__).resolve().parent / "longitudinal_distance_calibration" /
        "canonical_validation_summary.json"
    )
    grid = json.loads(grid_path.read_text())
    calibration = json.loads(calibration_path.read_text())

    try:
        import matplotlib.pyplot as plt
    except ImportError as exc:
        raise SystemExit(f"matplotlib is required to generate the figure: {exc}")

    raw = calibration["raw_pair_inversion_cells"]
    f = 5
    protocol = sorted([
        row for row in grid.get("cells", [])
        if int(row["b"]) == f and float(row["sigma_long_m"]) == 1.0
    ], key=lambda row: row["reference_separation_m"])

    fig, (sensor_ax, protocol_ax) = plt.subplots(1, 2, figsize=(13, 5.2))
    for sigma in sorted({float(row["sigma_long_m"]) for row in raw}):
        rows = sorted([row for row in raw if float(row["sigma_long_m"]) == sigma],
                      key=lambda row: row["separation_m"])
        x = [row["separation_m"] for row in rows]
        empirical = [row["wilson"]["rate"] for row in rows]
        low = [row["wilson"]["low"] for row in rows]
        high = [row["wilson"]["high"] for row in rows]
        line = sensor_ax.plot(x, empirical, marker="o", label=f"sigma={sigma:g} m")[0]
        sensor_ax.fill_between(x, low, high, color=line.get_color(), alpha=0.16)
        sensor_ax.plot(x, [row["analytic_nominal"] for row in rows],
                       linestyle="--", color=line.get_color(), alpha=0.9)
    sensor_ax.set_title("Sensor validation: raw pair inversion")
    sensor_ax.set_xlabel("Front-reference separation s (m)")
    sensor_ax.set_ylabel("P(raw observed order inversion)")
    sensor_ax.set_ylim(bottom=-0.01)
    sensor_ax.grid(alpha=0.25)
    sensor_ax.legend(fontsize=8)

    if protocol:
        x = [row["reference_separation_m"] for row in protocol]
        ci = [row["committed_per_pair_inversion"] for row in protocol]
        y = [row["rate"] for row in ci]
        yerr = [[rate - row["low"] for rate, row in zip(y, ci)],
                [row["high"] - rate for rate, row in zip(y, ci)]]
        protocol_ax.errorbar(x, y, yerr=yerr, marker="o", capsize=4,
                             label="empirical committed pair +/-95% Wilson")
        protocol_ax.plot(x, [row["mean_protocol_prediction"] for row in protocol],
                         linestyle="--", marker="s", label="absolute-gate binomial prediction")
    protocol_ax.set_title("Protocol validation: committed per-pair inversion (b=f=5)")
    protocol_ax.set_xlabel("Front-reference separation s (m)")
    protocol_ax.set_ylabel("P(committed target/predecessor inversion)")
    protocol_ax.set_ylim(-0.03, 1.03)
    protocol_ax.grid(alpha=0.25)
    protocol_ax.legend(fontsize=8)
    fig.suptitle("Longitudinal uncertainty: sensor calibration and protocol composition")
    fig.tight_layout()
    figure_path = output_dir / "longitudinal_two_panel.png"
    fig.savefig(figure_path, dpi=220)
    plt.close(fig)

    (output_dir / "longitudinal_two_panel_caption.txt").write_text(CAPTION + "\n")
    with (output_dir / "longitudinal_whole_queue_empirical.csv").open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=(
            "reference_separation_m", "sigma_long_m", "b", "repetitions",
            "successes", "trials", "rate", "wilson_low", "wilson_high",
        ))
        writer.writeheader()
        for row in grid.get("cells", []):
            empirical = row["whole_queue_order_violation_empirical_only"]
            writer.writerow({
                "reference_separation_m": row["reference_separation_m"],
                "sigma_long_m": row["sigma_long_m"], "b": row["b"],
                "repetitions": row["repetitions"], "successes": empirical["successes"],
                "trials": empirical["trials"], "rate": empirical["rate"],
                "wilson_low": empirical["low"], "wilson_high": empirical["high"],
            })

    # Cheap operating-characteristic figure: re-decide the cached s=5 m,
    # sigma=1 m, b=f observations at k={2,2.5,3}; no simulation is repeated.
    selected = [row for row in grid.get("runs", [])
                if row["reference_separation_m"] == 5.0 and
                   row["sigma_long_m"] == 1.0 and row["b"] == f]
    if selected:
        ks = [2.0, 2.5, 3.0]
        empirical_rates, predictions, q1_empirical, q1_analytic = [], [], [], []
        for k in ks:
            decisions, predicted, true_accepts, true_trials, analytic_true = [], [], 0, 0, []
            for run in selected:
                point = next(row for row in run["offline_rethreshold"] if row["k"] == k)
                decisions.append(bool(point["counterfactual_threshold_met"]))
                if point["binomial_tail_prediction"] is not None:
                    predicted.append(float(point["binomial_tail_prediction"]))
                true_accepts += int(point.get("honest_true_accepts", 0))
                true_trials += int(point.get("honest_true_trials", 0))
                if point.get("analytic_q1_dist") is not None:
                    analytic_true.append(float(point["analytic_q1_dist"]))
            empirical_rates.append(sum(decisions) / len(decisions))
            predictions.append(sum(predicted) / len(predicted) if predicted else None)
            q1_empirical.append(true_accepts / true_trials if true_trials else None)
            q1_analytic.append(sum(analytic_true) / len(analytic_true) if analytic_true else None)
        fig, (false_ax, true_ax) = plt.subplots(1, 2, figsize=(11.5, 4.6))
        false_ax.plot(ks, empirical_rates, marker="o", label="offline re-thresholded outcome")
        false_ax.plot(ks, predictions, marker="s", linestyle="--", label="binomial prediction")
        false_ax.set_title("Protocol false-certificate operating point")
        false_ax.set_xlabel("Tolerance multiplier k")
        false_ax.set_ylabel("P(false certificate), s=5 m, sigma=1 m, b=f")
        false_ax.set_ylim(-0.03, 1.03)
        false_ax.grid(alpha=0.25)
        false_ax.legend()
        true_ax.plot(ks, q1_empirical, marker="o", label="empirical honest true-accept")
        true_ax.plot(ks, q1_analytic, marker="s", linestyle="--", label="Gaussian prediction")
        true_ax.set_title("Sensor true-accept operating point")
        true_ax.set_xlabel("Tolerance multiplier k")
        true_ax.set_ylabel("q1 for honest absolute-distance claims")
        true_ax.set_ylim(-0.03, 1.03)
        true_ax.grid(alpha=0.25)
        true_ax.legend()
        fig.suptitle("Gate operating characteristic from cached observations")
        fig.tight_layout()
        fig.savefig(output_dir / "longitudinal_k_rethreshold.png", dpi=220)
        plt.close(fig)

    print(f"Two-panel figure: {figure_path}")
    print(f"Caption: {output_dir / 'longitudinal_two_panel_caption.txt'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

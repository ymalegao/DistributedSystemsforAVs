#!/usr/bin/env python3
"""Stopped-queue longitudinal calibration (SUMO/TraCI + offline statistics)."""

from __future__ import annotations

import argparse
import json
import math
import os
import sys
from pathlib import Path
from typing import Any, Dict, Iterable, Sequence

import numpy as np

HERE = Path(__file__).resolve().parent
MANIFEST_PATH = HERE / "fixture_manifest.json"
QUANTUM_M = 0.01


def load_manifest(path: Path = MANIFEST_PATH) -> Dict[str, Any]:
    return json.loads(path.read_text())


def ensure_sumo_tools() -> str:
    candidates = []
    if os.environ.get("SUMO_HOME"):
        candidates.append(Path(os.environ["SUMO_HOME"]))
    candidates.extend(
        [Path("/Users/yashmalegaonkar/Documents/omnet/sumo-1.22.0"), Path.home() / "sumo"]
    )
    for root in candidates:
        if (root / "bin" / "sumo").is_file() and (root / "tools").is_dir():
            sys.path.insert(0, str(root / "tools"))
            os.environ.setdefault("SUMO_HOME", str(root))
            return str(root / "bin" / "sumo")
    raise RuntimeError("SUMO not found; set SUMO_HOME")


def quantize_cm(value: float) -> float:
    scaled = float(value) * 100.0
    return (math.floor(scaled + 0.5) if scaled >= 0 else math.ceil(scaled - 0.5)) / 100.0


def quantize_cm_array(values: np.ndarray) -> np.ndarray:
    scaled = np.asarray(values, dtype=np.float64) * 100.0
    return np.where(scaled >= 0.0, np.floor(scaled + 0.5), np.ceil(scaled - 0.5)) / 100.0


def wilson(successes: int, trials: int, z: float = 1.959963984540054) -> Dict[str, float]:
    p = successes / trials
    den = 1.0 + z * z / trials
    center = (p + z * z / (2.0 * trials)) / den
    half = z * math.sqrt(p * (1.0 - p) / trials + z * z / (4.0 * trials * trials)) / den
    return {"rate": p, "low": max(0.0, center - half), "high": min(1.0, center + half)}


def false_claim_probability(delta_m: float, sigma_m: float, k: float) -> float:
    """P(|N(0,sigma^2)-delta| <= k*sigma)."""
    from scipy.stats import norm

    if sigma_m == 0.0:
        return 1.0 if abs(delta_m) == 0.0 else 0.0
    ratio = abs(delta_m) / sigma_m
    return float(norm.cdf(k - ratio) - norm.cdf(-k - ratio))


def raw_inversion_probability(separation_m: float, sigma_m: float) -> float:
    from scipy.stats import norm

    if sigma_m == 0.0:
        return 0.0
    return float(norm.cdf(-separation_m / (math.sqrt(2.0) * sigma_m)))


def collect_stopped_queue(manifest: Dict[str, Any]) -> Dict[str, Any]:
    sumo = ensure_sumo_tools()
    import traci

    ids = manifest["vehicles_front_to_rear"]
    cfg = HERE / manifest["files"]["sumocfg"]
    traci.start([sumo, "-c", str(cfg), "--seed", "17"])
    snapshots = []
    try:
        for _ in range(20):
            traci.simulationStep()
            present = set(traci.vehicle.getIDList())
            if all(vehicle_id in present for vehicle_id in ids):
                snapshots.append(
                    {
                        vehicle_id: {
                            "lane_id": traci.vehicle.getLaneID(vehicle_id),
                            "lane_position_m": float(traci.vehicle.getLanePosition(vehicle_id)),
                            "speed_mps": float(traci.vehicle.getSpeed(vehicle_id)),
                        }
                        for vehicle_id in ids
                    }
                )
                if len(snapshots) >= 5:
                    break
    finally:
        traci.close()
    if len(snapshots) < 5:
        raise RuntimeError(f"did not observe all seven queue vehicles for five steps: {len(snapshots)}")

    edge_length = float(manifest["geometry"]["stop_line_lane_position_m"])
    final = snapshots[-1]
    distances = [quantize_cm(edge_length - final[v]["lane_position_m"]) for v in ids]
    separations = [quantize_cm(distances[i + 1] - distances[i]) for i in range(len(ids) - 1)]
    speeds = [snap[v]["speed_mps"] for snap in snapshots for v in ids]
    position_span = {
        v: max(s[v]["lane_position_m"] for s in snapshots) - min(s[v]["lane_position_m"] for s in snapshots)
        for v in ids
    }
    return {
        "snapshots": snapshots,
        "distances_to_stop_m": dict(zip(ids, distances)),
        "front_reference_separations_m": separations,
        "max_abs_speed_mps": max(abs(v) for v in speeds),
        "max_position_span_m": max(position_span.values()),
        "all_expected_lane": all(
            snap[v]["lane_id"] == manifest["geometry"]["lane_id"] for snap in snapshots for v in ids
        ),
    }


def validation_overlap(successes: int, trials: int, p_low: float, p_high: float, z: float) -> Dict[str, Any]:
    interval = wilson(successes, trials, z)
    passed = interval["low"] <= p_high + 1e-15 and interval["high"] >= p_low - 1e-15
    return {"successes": successes, "trials": trials, "wilson": interval, "analytic_low": p_low, "analytic_high": p_high, "pass": bool(passed)}


def run_calibration(output: Path | None = None) -> Dict[str, Any]:
    from scipy.stats import norm

    manifest = load_manifest()
    trace = collect_stopped_queue(manifest)
    sensing = manifest["sensing"]
    sigmas = [float(v) for v in sensing["sigma_long_m"]]
    ks = [float(v) for v in sensing["k_values"]]
    seeds = [int(v) for v in sensing["seeds"]]
    samples_per_seed = int(sensing["samples_per_seed"])
    eta = float(sensing["attack_ahead_margin_m"])
    ids = manifest["vehicles_front_to_rear"]
    dists = trace["distances_to_stop_m"]
    separations = trace["front_reference_separations_m"]
    n_false_cells = len(separations) * len(sigmas) * len(ks)
    n_pair_cells = len(separations) * len(sigmas)
    z_false = float(norm.ppf(1.0 - (0.05 / n_false_cells) / 2.0))
    z_pair = float(norm.ppf(1.0 - (0.05 / n_pair_cells) / 2.0))
    false_cells = []
    pair_cells = []

    for pair_index, separation in enumerate(separations):
        front_id, rear_id = ids[pair_index], ids[pair_index + 1]
        d_true_rear = float(dists[rear_id])
        d_true_front = float(dists[front_id])
        d_claim_rear = quantize_cm(d_true_front - eta)
        delta = quantize_cm(d_true_rear - d_claim_rear)
        for sigma in sigmas:
            residual_blocks = []
            inversion_successes = 0
            inversion_trials = 0
            for seed in seeds:
                rng = np.random.default_rng(seed + 1000 * pair_index + int(100 * sigma))
                rear_noise = rng.normal(0.0, sigma, samples_per_seed)
                front_noise = rng.normal(0.0, sigma, samples_per_seed)
                rear_obs = quantize_cm_array(d_true_rear + rear_noise)
                front_obs = quantize_cm_array(d_true_front + front_noise)
                residual_blocks.append(np.abs(rear_obs - d_claim_rear))
                inversion_successes += int(np.count_nonzero(rear_obs < front_obs))
                inversion_trials += samples_per_seed
            residuals = np.concatenate(residual_blocks)
            for k in ks:
                successes = int(np.count_nonzero(residuals <= k * sigma + 1e-12))
                # Quantizing the observation shifts the scalar residual by at most 0.5 cm.
                p_low = false_claim_probability(delta, sigma, max(0.0, k - 0.005 / sigma))
                p_high = false_claim_probability(delta, sigma, k + 0.005 / sigma)
                check = validation_overlap(successes, residuals.size, p_low, p_high, z_false)
                false_cells.append({
                    "pair": f"{front_id}>{rear_id}", "clearance_m": quantize_cm(separation - manifest["geometry"]["vehicle_length_m"]),
                    "separation_m": separation, "sigma_long_m": sigma, "k": k,
                    "d_true_rear_m": d_true_rear, "d_claim_rear_m": d_claim_rear,
                    "delta_long_m": delta, "analytic_nominal": false_claim_probability(delta, sigma, k), **check,
                })
            # Quantizing two observations perturbs their difference by at most one cm.
            p_lo = raw_inversion_probability(separation + 0.01, sigma)
            p_hi = raw_inversion_probability(max(0.0, separation - 0.01), sigma)
            pair_check = validation_overlap(inversion_successes, inversion_trials, p_lo, p_hi, z_pair)
            pair_cells.append({
                "pair": f"{front_id}>{rear_id}", "separation_m": separation, "sigma_long_m": sigma,
                "analytic_nominal": raw_inversion_probability(separation, sigma), **pair_check,
            })

    expected_sep = manifest["geometry"]["expected_front_reference_separations_m"]
    geometry_pass = all(abs(a - b) <= 0.02 for a, b in zip(separations, expected_sep))
    stopped_pass = trace["max_abs_speed_mps"] <= 1e-6 and trace["max_position_span_m"] <= 1e-6
    summary = {
        "fixture": manifest["name"],
        "trace": trace,
        "false_distance_cells": false_cells,
        "raw_pair_inversion_cells": pair_cells,
        "pass_criteria": {
            "all_vehicles_on_expected_lane": trace["all_expected_lane"],
            "physically_stopped_window": stopped_pass,
            "front_reference_spacing_matches_manifest": geometry_pass,
            "false_distance_analytic_agreement": all(c["pass"] for c in false_cells),
            "raw_pair_inversion_analytic_agreement": all(c["pass"] for c in pair_cells),
        },
    }
    summary["pass_criteria"]["checkpoint_pass"] = all(summary["pass_criteria"].values())
    output = output or HERE / manifest["files"]["canonical_summary"]
    output.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    print(f"checkpoint_pass={summary['pass_criteria']['checkpoint_pass']}")
    print(f"summary={output}")
    return summary


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    summary = run_calibration(args.output)
    return 0 if summary["pass_criteria"]["checkpoint_pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

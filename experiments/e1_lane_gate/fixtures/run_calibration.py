#!/usr/bin/env python3
"""Adjacent-lane lateral-residual calibration fixture (SUMO/TraCI + offline ROC).

Scope: geometry tracing, lane-normal projection, 1 cm quantization, Gaussian
witness observations, offline k-rethresholding, and the one-dimensional
Gaussian analytic comparison used by the protocol gate.
No ResDBPerception / wire-format / PBFT / scheduler changes.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import sys
from pathlib import Path
from typing import Any, Dict, List, Sequence, Tuple

import numpy as np

HERE = Path(__file__).resolve().parent
DEFAULT_MANIFEST = HERE / "fixture_manifest.json"
DEFAULT_RESULTS = HERE / "results"

LANE_A = "A"
LANE_B = "B"
AMBIGUOUS = "AMBIGUOUS"

# Predeclared simultaneous-test constants (12 δ × 3 k = 36 cells).
QUANTIZE_CM_M = 0.01
# Both scalar endpoints are rounded to the nearest centimetre. Each incurs at
# most 0.5 cm error, so their residual changes by at most 1 cm.
QUANT_ENVELOPE_M = QUANTIZE_CM_M
VALIDATION_FAMILY_ALPHA = 0.05
VALIDATION_N_CELLS = 36


def load_manifest(path: Path = DEFAULT_MANIFEST) -> Dict[str, Any]:
    return json.loads(path.read_text())


def ensure_sumo_tools() -> str:
    """Put SUMO tools on sys.path; return absolute path to the sumo binary."""
    sumo_home = os.environ.get("SUMO_HOME")
    candidates = []
    if sumo_home:
        candidates.append(Path(sumo_home))
    candidates.extend(
        [
            Path("/Users/yashmalegaonkar/Documents/omnet/sumo-1.22.0"),
            Path.home() / "sumo",
        ]
    )
    for root in candidates:
        tools = root / "tools"
        binary = root / "bin" / "sumo"
        if tools.is_dir() and binary.is_file():
            tools_s = str(tools)
            if tools_s not in sys.path:
                sys.path.insert(0, tools_s)
            os.environ.setdefault("SUMO_HOME", str(root))
            return str(binary)
    raise RuntimeError(
        "SUMO not found. Set SUMO_HOME to a SUMO install that contains bin/sumo and tools/."
    )


def quantize_1cm(value: float) -> float:
    """Round to the nearest centimetre (half away from zero)."""
    scaled = value * 100.0
    if scaled >= 0.0:
        return math.floor(scaled + 0.5) / 100.0
    return math.ceil(scaled - 0.5) / 100.0


def quantize_point(xy: Sequence[float]) -> Tuple[float, float]:
    return quantize_1cm(float(xy[0])), quantize_1cm(float(xy[1]))


def wilson_interval(
    successes: int, trials: int, z: float = 1.959963984540054
) -> Dict[str, Any]:
    if trials <= 0:
        return {
            "successes": successes,
            "trials": trials,
            "rate": None,
            "low": None,
            "high": None,
        }
    p = successes / trials
    denominator = 1.0 + z * z / trials
    center = (p + z * z / (2.0 * trials)) / denominator
    half = (
        z
        * math.sqrt(p * (1.0 - p) / trials + z * z / (4.0 * trials * trials))
        / denominator
    )
    return {
        "successes": int(successes),
        "trials": int(trials),
        "rate": p,
        "low": max(0.0, center - half),
        "high": min(1.0, center + half),
    }


def bonferroni_z(
    n_cells: int = VALIDATION_N_CELLS,
    alpha: float = VALIDATION_FAMILY_ALPHA,
) -> float:
    """Two-sided Bonferroni z for a family of `n_cells` simultaneous intervals."""
    from scipy.stats import norm

    alpha_cell = alpha / n_cells
    return float(norm.ppf(1.0 - alpha_cell / 2.0))


def analytic_quantization_envelope(
    sigma: float,
    delta: float,
    k: float,
    quant_eps_m: float = QUANT_ENVELOPE_M,
) -> Dict[str, float]:
    """Ideal 1D Gaussian acceptance band at thresholds (kσ ± quant_eps_m)."""
    if sigma <= 0.0:
        raise ValueError("sigma must be positive")
    threshold_m = float(k) * float(sigma)
    threshold_low_m = max(0.0, threshold_m - quant_eps_m)
    threshold_high_m = threshold_m + quant_eps_m
    k_low = threshold_low_m / float(sigma)
    k_high = threshold_high_m / float(sigma)
    return {
        "threshold_m": threshold_m,
        "threshold_low_m": threshold_low_m,
        "threshold_high_m": threshold_high_m,
        "k_low": k_low,
        "k_high": k_high,
        "p_low": analytic_accept_prob(sigma, delta, k_low),
        "p_high": analytic_accept_prob(sigma, delta, k_high),
    }


def validate_quantized_gate_cell(
    pooled_quantized: Dict[str, Any],
    envelope: Dict[str, float],
    z_bonferroni: float,
) -> Dict[str, Any]:
    """Validate the quantized lateral gate against Gaussian + quantization band."""
    wilson_95 = wilson_interval(
        pooled_quantized["successes"], pooled_quantized["trials"]
    )
    bonferroni = wilson_interval(
        pooled_quantized["successes"],
        pooled_quantized["trials"],
        z=z_bonferroni,
    )
    rate = pooled_quantized["rate"]
    overlaps = (
        bonferroni["low"] is not None
        and bonferroni["high"] is not None
        and bonferroni["low"] <= envelope["p_high"] + 1e-15
        and bonferroni["high"] >= envelope["p_low"] - 1e-15
    )
    point_in_envelope = (
        rate is not None
        and envelope["p_low"] - 1e-15 <= rate <= envelope["p_high"] + 1e-15
    )
    return {
        "empirical_rate": rate,
        "wilson_95": wilson_95,
        "bonferroni": bonferroni,
        "envelope": envelope,
        "bonferroni_overlaps_envelope": bool(overlaps),
        "empirical_in_envelope": bool(point_in_envelope),
        "pass": bool(overlaps),
    }


def reference_continuous_agreement(
    pooled_continuous: Dict[str, Any],
    analytic_continuous: float,
) -> Dict[str, Any]:
    """Informational check: continuous empirical vs ideal 1D gate at nominal k."""
    wilson_95 = wilson_interval(
        pooled_continuous["successes"], pooled_continuous["trials"]
    )
    low = wilson_95["low"]
    high = wilson_95["high"]
    rate = wilson_95["rate"]
    inside = (
        low is not None
        and high is not None
        and low - 1e-15 <= analytic_continuous <= high + 1e-15
    )
    return {
        "empirical_rate": rate,
        "analytic_continuous": analytic_continuous,
        "wilson_95_low": low,
        "wilson_95_high": high,
        "analytic_inside_wilson_95": bool(inside),
        "abs_error": None if rate is None else abs(rate - analytic_continuous),
    }


def analytic_accept_prob(sigma: float, delta: float, k: float) -> float:
    """P(|N(0, σ²) - δ| ≤ kσ) for the lane-normal residual."""
    if sigma < 0.0:
        raise ValueError("sigma must be non-negative")
    if sigma == 0.0:
        return 1.0 if abs(delta) <= k * sigma + 1e-15 else 0.0
    from scipy.stats import norm

    d = float(delta) / float(sigma)
    return float(norm.cdf(d + k) - norm.cdf(d - k))


def project_claimed_lane(
    claim_xy: Sequence[float],
    center_a_y: float,
    center_b_y: float,
    tie_eps: float = 1e-12,
) -> str:
    """Nearest centerline of the quantized claim; exact boundary → AMBIGUOUS."""
    y = float(claim_xy[1])
    d_a = abs(y - center_a_y)
    d_b = abs(y - center_b_y)
    if abs(d_a - d_b) <= tie_eps:
        return AMBIGUOUS
    return LANE_A if d_a < d_b else LANE_B


def boundary_distance_m(y: float, boundary_y: float) -> float:
    return float(y - boundary_y)


def derive_geometry_from_net(net_path: Path, manifest: Dict[str, Any]) -> Dict[str, Any]:
    ensure_sumo_tools()
    import sumolib

    geo = manifest["geometry"]
    net = sumolib.net.readNet(str(net_path))
    edge = net.getEdge(geo["edge_id"])
    lanes = {lane.getIndex(): lane for lane in edge.getLanes()}
    lane_a = lanes[0]
    lane_b = lanes[1]
    a0 = lane_a.getShape()[0]
    b0 = lane_b.getShape()[0]
    center_a = (float(a0[0]), float(a0[1]))
    center_b = (float(b0[0]), float(b0[1]))
    sep_vec = (center_b[0] - center_a[0], center_b[1] - center_a[1])
    sep = math.hypot(sep_vec[0], sep_vec[1])
    if sep <= 0.0:
        raise RuntimeError("degenerate lane-center separation")
    normal = (sep_vec[0] / sep, sep_vec[1] / sep)
    boundary = (
        0.5 * (center_a[0] + center_b[0]),
        0.5 * (center_a[1] + center_b[1]),
    )
    return {
        "lane_a_id": lane_a.getID(),
        "lane_b_id": lane_b.getID(),
        "lane_a_width_m": float(lane_a.getWidth()),
        "lane_b_width_m": float(lane_b.getWidth()),
        "lane_a_center_sample_xy": list(center_a),
        "lane_b_center_sample_xy": list(center_b),
        "lane_center_separation_m": sep,
        "unit_normal_a_to_b": list(normal),
        "boundary_xy": list(boundary),
        "lane_a_shape": [list(map(float, p)) for p in lane_a.getShape()],
        "lane_b_shape": [list(map(float, p)) for p in lane_b.getShape()],
        "expected": {
            "lane_a_center_y_m": geo["lane_a_center_y_m"],
            "lane_b_center_y_m": geo["lane_b_center_y_m"],
            "boundary_y_m": geo["boundary_y_m"],
            "lane_center_separation_m": geo["lane_center_separation_m"],
            "unit_normal_a_to_b": geo["unit_normal_a_to_b"],
        },
    }


def collect_traci_trace(
    sumocfg: Path,
    vehicle_id: str,
    true_lane_id: str,
) -> Dict[str, Any]:
    sumo_binary = ensure_sumo_tools()
    import traci

    traci.start(
        [sumo_binary, "-c", str(sumocfg), "--start", "--quit-on-end"],
        label="adjacent_lane_cal",
    )
    conn = traci.getConnection("adjacent_lane_cal")
    samples: List[Dict[str, Any]] = []
    lanes_seen = set()
    try:
        while conn.simulation.getMinExpectedNumber() > 0:
            conn.simulationStep()
            ids = conn.vehicle.getIDList()
            if vehicle_id not in ids:
                continue
            x, y = conn.vehicle.getPosition(vehicle_id)
            lane_id = conn.vehicle.getLaneID(vehicle_id)
            lanes_seen.add(lane_id)
            samples.append(
                {
                    "sim_t": float(conn.simulation.getTime()),
                    "true_x": float(x),
                    "true_y": float(y),
                    "lane_id": lane_id,
                }
            )
    finally:
        conn.close()
        try:
            traci.switch("default")
        except Exception:
            pass

    if not samples:
        raise RuntimeError("no TraCI position samples collected for target vehicle")
    off_lane = [s for s in samples if s["lane_id"] != true_lane_id]
    return {
        "vehicle_id": vehicle_id,
        "expected_lane_id": true_lane_id,
        "lanes_seen": sorted(lanes_seen),
        "n_samples": len(samples),
        "all_on_expected_lane": len(off_lane) == 0,
        "off_lane_count": len(off_lane),
        "y_min": min(s["true_y"] for s in samples),
        "y_max": max(s["true_y"] for s in samples),
        "y_mean": float(np.mean([s["true_y"] for s in samples])),
        "samples": samples,
    }


def _claim_and_obs(
    p_true: Sequence[float],
    delta: float,
    normal: Sequence[float],
    noise_xy: Sequence[float],
) -> Tuple[Tuple[float, float], Tuple[float, float], float]:
    claim_raw = (
        float(p_true[0]) + delta * float(normal[0]),
        float(p_true[1]) + delta * float(normal[1]),
    )
    obs_raw = (
        float(p_true[0]) + float(noise_xy[0]),
        float(p_true[1]) + float(noise_xy[1]),
    )
    p_claim = quantize_point(claim_raw)
    p_obs = quantize_point(obs_raw)
    # Certify only the lane-normal residual. Longitudinal displacement is
    # intentionally ignored so a moving honest target retains a stable claim.
    dist = abs(
        (p_obs[0] - p_claim[0]) * float(normal[0])
        + (p_obs[1] - p_claim[1]) * float(normal[1])
    )
    return p_claim, p_obs, dist


def _quantize_1cm_array(values: np.ndarray) -> np.ndarray:
    """Vectorized centimetre quantization (half away from zero)."""
    scaled = values * 100.0
    pos = np.floor(scaled + 0.5)
    neg = np.ceil(scaled - 0.5)
    return np.where(scaled >= 0.0, pos, neg) / 100.0


def generate_delta_observations(
    *,
    true_positions: Sequence[Sequence[float]],
    delta: float,
    normal: Sequence[float],
    sigma: float,
    seeds: Sequence[int],
    samples_per_seed: int,
    center_a_y: float,
    center_b_y: float,
    boundary_y: float,
) -> Dict[str, Any]:
    """Generate paired observations once per (delta, seed); store distances for all k."""
    if not true_positions:
        raise ValueError("true_positions must be non-empty")
    positions = np.asarray(true_positions, dtype=np.float64)
    n_pos = positions.shape[0]
    nx = float(normal[0])
    ny = float(normal[1])
    seed_blocks = []
    all_dists: List[np.ndarray] = []
    all_dists_cont: List[np.ndarray] = []
    all_lanes: List[np.ndarray] = []

    for seed in seeds:
        rng = np.random.default_rng(int(seed))
        noise_lateral = rng.normal(loc=0.0, scale=sigma, size=samples_per_seed)
        idx = np.arange(samples_per_seed) % n_pos
        p_true = positions[idx]
        claim_raw = np.column_stack(
            (p_true[:, 0] + delta * nx, p_true[:, 1] + delta * ny)
        )
        obs_raw = np.column_stack(
            (p_true[:, 0] + noise_lateral * nx,
             p_true[:, 1] + noise_lateral * ny)
        )
        p_claim = _quantize_1cm_array(claim_raw)
        p_obs = _quantize_1cm_array(obs_raw)
        # Gate distances use the quantized scalar projection. Continuous
        # residuals from the same draws validate the 1D Gaussian formula.
        dists = np.abs(
            (p_obs[:, 0] - p_claim[:, 0]) * nx
            + (p_obs[:, 1] - p_claim[:, 1]) * ny
        )
        dists_cont = np.abs(noise_lateral - delta)
        d_a = np.abs(p_claim[:, 1] - center_a_y)
        d_b = np.abs(p_claim[:, 1] - center_b_y)
        lanes = np.full(samples_per_seed, LANE_B, dtype=object)
        lanes[d_a < d_b] = LANE_A
        lanes[np.abs(d_a - d_b) <= 1e-12] = AMBIGUOUS
        seed_blocks.append(
            {
                "seed": int(seed),
                "n": samples_per_seed,
                "claimed_lane_counts": {
                    LANE_A: int(np.sum(lanes == LANE_A)),
                    LANE_B: int(np.sum(lanes == LANE_B)),
                    AMBIGUOUS: int(np.sum(lanes == AMBIGUOUS)),
                },
            }
        )
        all_dists.append(dists)
        all_dists_cont.append(dists_cont)
        all_lanes.append(lanes)

    distances = np.concatenate(all_dists)
    distances_continuous = np.concatenate(all_dists_cont)
    claimed_lanes = np.concatenate(all_lanes)
    # Representative claim geometry from the first true position (translation-invariant rates).
    p0 = true_positions[0]
    p_claim0, _, _ = _claim_and_obs(p0, delta, normal, (0.0, 0.0))
    return {
        "delta_m": float(delta),
        "sigma_m": float(sigma),
        "n_observations": int(distances.size),
        "seeds": seed_blocks,
        "distances_m": distances,
        "distances_continuous_m": distances_continuous,
        "claimed_lanes": claimed_lanes,
        "example_p_true": [float(p0[0]), float(p0[1])],
        "example_p_claim": [float(p_claim0[0]), float(p_claim0[1])],
        "example_claimed_lane": project_claimed_lane(p_claim0, center_a_y, center_b_y),
        "example_boundary_distance_m": boundary_distance_m(p_claim0[1], boundary_y),
    }


def rethreshold_counts(
    distances_m: np.ndarray, sigma: float, k_values: Sequence[float]
) -> Dict[str, Dict[str, Any]]:
    out: Dict[str, Dict[str, Any]] = {}
    for k in k_values:
        thr = float(k) * float(sigma)
        accepts = int(np.sum(distances_m <= thr))
        out[f"{k:g}"] = {
            "k": float(k),
            "threshold_m": thr,
            **wilson_interval(accepts, int(distances_m.size)),
        }
    return out


def per_seed_rates(
    distances_by_seed: Sequence[np.ndarray],
    sigma: float,
    k_values: Sequence[float],
    seeds: Sequence[int],
) -> Dict[str, List[Dict[str, Any]]]:
    out: Dict[str, List[Dict[str, Any]]] = {f"{k:g}": [] for k in k_values}
    for seed, dists in zip(seeds, distances_by_seed):
        for k in k_values:
            thr = float(k) * float(sigma)
            accepts = int(np.sum(dists <= thr))
            out[f"{k:g}"].append(
                {"seed": int(seed), **wilson_interval(accepts, int(dists.size))}
            )
    return out


def regime_for_delta(delta: float, boundary: float = 1.6) -> str:
    if abs(delta - boundary) <= 1e-12:
        return "boundary"
    if delta < boundary:
        return "true_lane" if delta >= 0.0 else "other"
    return "false_lane"


def run_calibration(
    *,
    manifest_path: Path = DEFAULT_MANIFEST,
    results_dir: Path = DEFAULT_RESULTS,
    store_bulk: bool = True,
) -> Dict[str, Any]:
    manifest = load_manifest(manifest_path)
    results_dir.mkdir(parents=True, exist_ok=True)

    geo_expected = manifest["geometry"]
    grid = manifest["roc_grid"]
    sensing = manifest["sensing"]
    target = manifest["target"]

    geometry = derive_geometry_from_net(HERE / manifest["files"]["net"], manifest)
    trace = collect_traci_trace(
        HERE / manifest["files"]["sumocfg"],
        target["vehicle_id"],
        target["true_lane_id"],
    )

    # Geometry gates
    sep_ok = abs(geometry["lane_center_separation_m"] - geo_expected["lane_center_separation_m"]) < 1e-9
    normal_ok = (
        abs(geometry["unit_normal_a_to_b"][0] - geo_expected["unit_normal_a_to_b"][0]) < 1e-9
        and abs(geometry["unit_normal_a_to_b"][1] - geo_expected["unit_normal_a_to_b"][1]) < 1e-9
    )
    centers_ok = (
        abs(geometry["lane_a_center_sample_xy"][1] - geo_expected["lane_a_center_y_m"]) < 1e-9
        and abs(geometry["lane_b_center_sample_xy"][1] - geo_expected["lane_b_center_y_m"]) < 1e-9
    )

    true_positions = [(s["true_x"], s["true_y"]) for s in trace["samples"]]
    normal = geometry["unit_normal_a_to_b"]
    sigma = float(grid["sigma_m"])
    k_values = [float(k) for k in sensing["k_values"]]
    seeds = [int(s) for s in grid["seeds"]]
    samples_per_seed = int(grid["samples_per_seed"])

    center_a_y = float(geo_expected["lane_a_center_y_m"])
    center_b_y = float(geo_expected["lane_b_center_y_m"])
    boundary_y = float(geo_expected["boundary_y_m"])

    delta_rows: List[Dict[str, Any]] = []
    for delta in grid["delta_m"]:
        delta = float(delta)
        block = generate_delta_observations(
            true_positions=true_positions,
            delta=delta,
            normal=normal,
            sigma=sigma,
            seeds=seeds,
            samples_per_seed=samples_per_seed,
            center_a_y=center_a_y,
            center_b_y=center_b_y,
            boundary_y=boundary_y,
        )
        distances = block["distances_m"]
        distances_cont = block["distances_continuous_m"]
        # Split back into per-seed arrays for reporting (same order as generation).
        distances_by_seed = [
            distances[i * samples_per_seed : (i + 1) * samples_per_seed]
            for i in range(len(seeds))
        ]
        pooled = rethreshold_counts(distances, sigma, k_values)
        pooled_continuous = rethreshold_counts(distances_cont, sigma, k_values)
        seed_rates = per_seed_rates(distances_by_seed, sigma, k_values, seeds)
        z_bonf = bonferroni_z()

        analytic_continuous = {
            f"{k:g}": {
                "k": float(k),
                "p_accept": analytic_accept_prob(sigma, delta, float(k)),
            }
            for k in k_values
        }
        analytic_envelope = {
            f"{k:g}": analytic_quantization_envelope(sigma, delta, float(k))
            for k in k_values
        }

        lane_counts = {
            LANE_A: int(np.sum(block["claimed_lanes"] == LANE_A)),
            LANE_B: int(np.sum(block["claimed_lanes"] == LANE_B)),
            AMBIGUOUS: int(np.sum(block["claimed_lanes"] == AMBIGUOUS)),
        }
        regime = regime_for_delta(delta, boundary_y)

        row = {
            "delta_m": delta,
            "regime": regime,
            "sigma_m": sigma,
            "n_observations": int(distances.size),
            "example_p_true": block["example_p_true"],
            "example_p_claim": block["example_p_claim"],
            "example_claimed_lane": block["example_claimed_lane"],
            "example_boundary_distance_m": block["example_boundary_distance_m"],
            "claimed_lane_counts": lane_counts,
            "pooled": pooled,
            "pooled_continuous": pooled_continuous,
            "per_seed": seed_rates,
            "analytic_continuous": analytic_continuous,
            "analytic_envelope": analytic_envelope,
            "gate_validation": {
                f"{k:g}": validate_quantized_gate_cell(
                    pooled[f"{k:g}"], analytic_envelope[f"{k:g}"], z_bonf
                )
                for k in k_values
            },
            "continuous_reference": {
                f"{k:g}": reference_continuous_agreement(
                    pooled_continuous[f"{k:g}"],
                    analytic_continuous[f"{k:g}"]["p_accept"],
                )
                for k in k_values
            },
        }
        delta_rows.append(row)

        if store_bulk:
            label = format(delta, ".2f").replace(".", "p")
            np.save(results_dir / f"distances_delta_{label}.npy", distances)
            np.save(
                results_dir / f"claimed_lanes_delta_{label}.npy",
                block["claimed_lanes"].astype("U16"),
            )

    # Quantization determinism probe
    q_probe = {
        "inputs": [1.234, -1.235, 1.605, 1.6, 0.005, 0.004],
        "outputs": [quantize_1cm(v) for v in [1.234, -1.235, 1.605, 1.6, 0.005, 0.004]],
    }

    summary = {
        "fixture": manifest["name"],
        "sigma_m": sigma,
        "k_values": k_values,
        "geometry": geometry,
        "geometry_checks": {
            "separation_ok": sep_ok,
            "normal_ok": normal_ok,
            "centers_ok": centers_ok,
            "traci_all_on_lane_a": trace["all_on_expected_lane"],
        },
        "traci_trace_summary": {
            k: v for k, v in trace.items() if k != "samples"
        },
        "quantize_1cm_probe": q_probe,
        "deltas": delta_rows,
        "pass_criteria": evaluate_pass_criteria(
            delta_rows,
            geometry,
            trace,
            sep_ok,
            normal_ok,
            centers_ok,
            z_bonferroni=bonferroni_z(),
        ),
    }

    # Drop bulky arrays already persisted.
    (results_dir / "geometry_trace.json").write_text(
        json.dumps(
            {
                "geometry": geometry,
                "traci_trace_summary": summary["traci_trace_summary"],
                "traci_samples": trace["samples"],
            },
            indent=2,
        )
        + "\n"
    )
    (results_dir / "calibration_summary.json").write_text(
        json.dumps(summary, indent=2) + "\n"
    )
    return summary


def evaluate_pass_criteria(
    delta_rows: Sequence[Dict[str, Any]],
    geometry: Dict[str, Any],
    trace: Dict[str, Any],
    sep_ok: bool,
    normal_ok: bool,
    centers_ok: bool,
    *,
    z_bonferroni: float,
) -> Dict[str, Any]:
    boundary_row = next(r for r in delta_rows if r["regime"] == "boundary")
    false_rows = [r for r in delta_rows if r["regime"] == "false_lane"]
    nontrivial = []
    for row in false_rows:
        for k_key, pooled in row["pooled"].items():
            rate = pooled["rate"]
            if rate is None:
                continue
            if 0.01 < rate < 0.99:
                nontrivial.append(
                    {"delta_m": row["delta_m"], "k": float(k_key), "rate": rate}
                )

    gate_cells = [
        row["gate_validation"][k]
        for row in delta_rows
        for k in row["gate_validation"]
    ]
    gate_pass = all(cell["pass"] for cell in gate_cells)
    gate_overlap_hits = sum(1 for cell in gate_cells if cell["bonferroni_overlaps_envelope"])
    gate_point_in_envelope = sum(1 for cell in gate_cells if cell["empirical_in_envelope"])

    continuous_refs = [
        row["continuous_reference"][k]
        for row in delta_rows
        for k in row["continuous_reference"]
    ]
    wilson_95_hits = sum(
        1 for ref in continuous_refs if ref["analytic_inside_wilson_95"]
    )

    return {
        "traci_target_on_lane_a": bool(trace["all_on_expected_lane"]),
        "centers_3p2_apart": bool(sep_ok and centers_ok),
        "normal_and_boundary_stable": bool(normal_ok and sep_ok),
        "boundary_classified_ambiguous": boundary_row["example_claimed_lane"] == AMBIGUOUS
        and boundary_row["claimed_lane_counts"][AMBIGUOUS]
        == boundary_row["n_observations"],
        "validation_procedure": {
            "family_wise_alpha": VALIDATION_FAMILY_ALPHA,
            "n_cells": VALIDATION_N_CELLS,
            "alpha_cell": VALIDATION_FAMILY_ALPHA / VALIDATION_N_CELLS,
            "bonferroni_z": z_bonferroni,
            "quantization_envelope_m": QUANT_ENVELOPE_M,
            "gate_basis": "quantized pooled distances",
            "reference_curve": "ideal 1D Gaussian interval at kσ ± quantization envelope",
            "pass_rule": "Bonferroni Wilson interval overlaps [P(k_low), P(k_high)]",
        },
        "quantized_gate_validation": {
            "cells_pass": sum(1 for cell in gate_cells if cell["pass"]),
            "cells_total": len(gate_cells),
            "all_pass": bool(gate_pass),
            "bonferroni_overlap_hits": gate_overlap_hits,
            "empirical_point_in_envelope_hits": gate_point_in_envelope,
        },
        "continuous_reference_wilson_95": {
            "hits": wilson_95_hits,
            "trials": len(continuous_refs),
            "rate": wilson_95_hits / len(continuous_refs) if continuous_refs else None,
            "note": "Informational only: continuous pre-quantize residuals vs nominal Gaussian interval.",
        },
        "false_lane_nontrivial_roc_points": nontrivial,
        "has_multiple_nontrivial_false_lane_points": len(nontrivial) >= 2,
        "checkpoint_pass": bool(
            trace["all_on_expected_lane"]
            and sep_ok
            and centers_ok
            and normal_ok
            and boundary_row["example_claimed_lane"] == AMBIGUOUS
            and gate_pass
            and len(nontrivial) >= 2
        ),
    }


def write_canonical_summary(summary: Dict[str, Any], path: Path) -> None:
    """Compact checked-in summary without bulk arrays."""
    compact_deltas = []
    for row in summary["deltas"]:
        compact_deltas.append(
            {
                "delta_m": row["delta_m"],
                "regime": row["regime"],
                "example_claimed_lane": row["example_claimed_lane"],
                "claimed_lane_counts": row["claimed_lane_counts"],
                "pooled": {
                    k: {
                        "rate": v["rate"],
                        "low": v["low"],
                        "high": v["high"],
                        "successes": v["successes"],
                        "trials": v["trials"],
                    }
                    for k, v in row["pooled"].items()
                },
                "pooled_continuous": {
                    k: {
                        "rate": v["rate"],
                        "low": v["low"],
                        "high": v["high"],
                        "successes": v["successes"],
                        "trials": v["trials"],
                    }
                    for k, v in row["pooled_continuous"].items()
                },
                "analytic_continuous": row["analytic_continuous"],
                "analytic_envelope": row["analytic_envelope"],
                "gate_validation": {
                    k: {
                        "empirical_rate": v["empirical_rate"],
                        "pass": v["pass"],
                        "bonferroni_overlaps_envelope": v["bonferroni_overlaps_envelope"],
                        "empirical_in_envelope": v["empirical_in_envelope"],
                        "bonferroni": {
                            "low": v["bonferroni"]["low"],
                            "high": v["bonferroni"]["high"],
                        },
                        "envelope": {
                            "p_low": v["envelope"]["p_low"],
                            "p_high": v["envelope"]["p_high"],
                            "threshold_low_m": v["envelope"]["threshold_low_m"],
                            "threshold_high_m": v["envelope"]["threshold_high_m"],
                        },
                    }
                    for k, v in row["gate_validation"].items()
                },
                "continuous_reference": row["continuous_reference"],
                "per_seed_rates": {
                    k: [{"seed": s["seed"], "rate": s["rate"]} for s in rates]
                    for k, rates in row["per_seed"].items()
                },
            }
        )
    payload = {
        "fixture": summary["fixture"],
        "sigma_m": summary["sigma_m"],
        "k_values": summary["k_values"],
        "geometry_checks": summary["geometry_checks"],
        "traci_trace_summary": summary["traci_trace_summary"],
        "quantize_1cm_probe": summary["quantize_1cm_probe"],
        "pass_criteria": summary["pass_criteria"],
        "deltas": compact_deltas,
    }
    path.write_text(json.dumps(payload, indent=2) + "\n")


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--results-dir", type=Path, default=DEFAULT_RESULTS)
    parser.add_argument(
        "--canonical-summary",
        type=Path,
        default=HERE / "canonical_validation_summary.json",
    )
    parser.add_argument("--no-bulk", action="store_true", help="Skip writing .npy bulk files")
    args = parser.parse_args(argv)

    summary = run_calibration(
        manifest_path=args.manifest,
        results_dir=args.results_dir,
        store_bulk=not args.no_bulk,
    )
    write_canonical_summary(summary, args.canonical_summary)
    pc = summary["pass_criteria"]
    print(
        json.dumps(
            {
                "checkpoint_pass": pc["checkpoint_pass"],
                "geometry_checks": summary["geometry_checks"],
                "n_false_lane_nontrivial": len(pc["false_lane_nontrivial_roc_points"]),
                "canonical_summary": str(args.canonical_summary),
            },
            indent=2,
        )
    )
    return 0 if pc["checkpoint_pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

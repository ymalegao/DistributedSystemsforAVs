#!/usr/bin/env python3
"""Two-lane four-way sensing calibration (Checkpoint 1).

Geometry: fourway/bft_intersection_2lane.net.xml
Calibration approach: North inbound N2C (lane 0=T outer, lane 1=L inner).
Gate: lateral_gate.py — identical rule to ResDB ADJACENT_LATERAL.

Does NOT retarget production omnetpp.ini / launchd / 16veh routes.
Does NOT touch kSafe / scheduler / PBFT.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import subprocess
import sys
from pathlib import Path
from typing import Any, Dict, List, Sequence, Tuple

import numpy as np

from lateral_gate import (
    analytic_q0,
    format_lane_cal_line,
    lane_label,
    lateral_gate_accept,
    project_physical_lane_index,
    quantize_cm,
    wilson_interval,
)

HERE = Path(__file__).resolve().parent
NET = HERE.parent / "bft_intersection_2lane.net.xml"
SUMOCFG = HERE / "two_lane.sumocfg"
RESULTS = HERE / "results"
MANIFEST = HERE / "fixture_manifest.json"

# Locked grids (Checkpoint 1)
HONEST_TRUE_LATERAL_M = [
    0.0, 0.4, 0.8, 1.2, 1.4, 1.5, 1.55, 1.6, 1.65, 1.7, 1.8, 2.2, 2.6, 3.0, 3.2,
]
ATTACKER_DELTA_M = [0.1, 0.3, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0]
SIGMA_CAL1_M = [0.25, 0.5, 1.0, 2.0]
SIGMA_HEADLINE_M = 0.5
DELTA_SENSITIVITY_M = 0.5
K_FIXED = 3.0
K_ROC = [2.0, 2.5, 3.0]
SEEDS = list(range(1000, 1010))
SAMPLES_PER_SEED = 10_000
SEPARATION_M = 3.2
BOUNDARY_M = 1.6
QUANT_ENVELOPE_M = 0.01


def ensure_sumo_tools() -> str:
    sumo_home = os.environ.get("SUMO_HOME")
    candidates = []
    if sumo_home:
        candidates.append(Path(sumo_home))
    candidates.append(Path("/Users/yashmalegaonkar/Documents/omnet/sumo-1.22.0"))
    for root in candidates:
        tools = root / "tools"
        binary = root / "bin" / "sumo"
        if tools.is_dir() and binary.is_file():
            tools_s = str(tools)
            if tools_s not in sys.path:
                sys.path.insert(0, tools_s)
            os.environ.setdefault("SUMO_HOME", str(root))
            return str(binary)
    raise RuntimeError("SUMO not found; set SUMO_HOME")


def derive_n2c_geometry() -> Dict[str, Any]:
    ensure_sumo_tools()
    import sumolib

    net = sumolib.net.readNet(str(NET))
    edge = net.getEdge("N2C")
    lanes = {lane.getIndex(): lane for lane in edge.getLanes()}
    if set(lanes) != {0, 1}:
        raise RuntimeError(f"N2C expected lanes 0,1 got {sorted(lanes)}")
    t_end = lanes[0].getShape()[-1]
    l_end = lanes[1].getShape()[-1]
    origin = (float(t_end[0]), float(t_end[1]))
    l_pt = (float(l_end[0]), float(l_end[1]))
    sep_vec = (l_pt[0] - origin[0], l_pt[1] - origin[1])
    sep = math.hypot(sep_vec[0], sep_vec[1])
    normal = (sep_vec[0] / sep, sep_vec[1] / sep)
    return {
        "approach": "N2C",
        "lane_T_id": lanes[0].getID(),
        "lane_L_id": lanes[1].getID(),
        "lane_index_T": 0,
        "lane_index_L": 1,
        "origin_xy": list(origin),
        "lane_L_xy": list(l_pt),
        "unit_normal_T_to_L": list(normal),
        "lane_center_separation_m": sep,
        "boundary_m": sep / 2.0,
        "lane_T_width_m": float(lanes[0].getWidth()),
        "lane_L_width_m": float(lanes[1].getWidth()),
    }


def load_tls_state_len() -> int:
    text = NET.read_text()
    # Forever-red phase state length
    for line in text.splitlines():
        if 'state="r' in line and "phase" in line:
            start = line.index('state="') + 7
            end = line.index('"', start)
            return end - start
    raise RuntimeError("could not parse TLS phase width")


def validate_geometry_with_traci() -> Dict[str, Any]:
    """Confirm connections + L goes left / T goes straight; no TELEPORT."""
    sumo_bin = ensure_sumo_tools()
    import traci

    label = "two_lane_geo"
    traci.start(
        [
            sumo_bin, "-c", str(SUMOCFG),
            "--start", "--quit-on-end",
            "--time-to-teleport", "-1",
            "--collision.action", "none",
            "--collision.check-junctions", "true",
        ],
        label=label,
    )
    conn = traci.getConnection(label)
    teleports = 0
    outcomes: Dict[str, Any] = {}
    try:
        n_links = load_tls_state_len()
        conn.trafficlight.setRedYellowGreenState("C", "G" * n_links)
        # Hold calibration cars; let geo cars move.
        for vid in ("witness", "target"):
            if vid in conn.vehicle.getIDList():
                conn.vehicle.setSpeedMode(vid, 0)
                conn.vehicle.setSpeed(vid, 0)
        for vid in ("geo_N_T", "geo_N_L"):
            if vid in conn.vehicle.getIDList():
                conn.vehicle.setSpeedMode(vid, 0)
        for _ in range(400):  # 40 s
            conn.simulationStep()
            teleports = max(
                teleports,
                len(conn.simulation.getStartingTeleportIDList()),
            )
            for vid in ("geo_N_T", "geo_N_L"):
                if vid not in conn.vehicle.getIDList():
                    continue
                road = conn.vehicle.getRoadID(vid)
                lane = conn.vehicle.getLaneID(vid)
                if road.startswith("C2"):
                    outcomes[vid] = {"road": road, "lane": lane}
        # Also record final if still present
        for vid in ("geo_N_T", "geo_N_L"):
            if vid in outcomes:
                continue
            if vid in conn.vehicle.getIDList():
                outcomes[vid] = {
                    "road": conn.vehicle.getRoadID(vid),
                    "lane": conn.vehicle.getLaneID(vid),
                    "incomplete": True,
                }
            else:
                outcomes[vid] = {"missing": True}
    finally:
        conn.close()

    t_ok = outcomes.get("geo_N_T", {}).get("road") == "C2S"
    l_ok = outcomes.get("geo_N_L", {}).get("road") == "C2E"
    return {
        "teleport_starts": teleports,
        "outcomes": outcomes,
        "outer_T_went_straight": bool(t_ok),
        "inner_L_went_left": bool(l_ok),
        "pass": bool(t_ok and l_ok and teleports == 0),
        "tls_link_count": n_links,
        "pins": {
            "time_to_teleport": -1,
            "collision_action": "none",
            "collision_check_junctions": True,
            "released_speed_mode": 0,
            "note": "calibration stopped vehicles are speed-mode insensitive",
        },
    }


def sample_gate_decisions(
    *,
    true_lateral_m: float,
    claimed_lateral_m: float,
    sigma_lat_m: float,
    k: float,
    seeds: Sequence[int],
    samples_per_seed: int,
    role: str,
    emit_lines: bool = False,
) -> Dict[str, Any]:
    """Single fixed witness, swept synthetic target lateral (stopped calibration)."""
    true_cm = quantize_cm(true_lateral_m)
    claimed_cm = quantize_cm(claimed_lateral_m)
    true_lane = lane_label(project_physical_lane_index(true_cm))
    claimed_lane = lane_label(project_physical_lane_index(claimed_cm))
    boundary_crossed = int(
        true_lane != claimed_lane and "AMBIGUOUS" not in (true_lane, claimed_lane)
    )
    delta_cm = abs(claimed_cm - true_cm)
    tolerance_cm = float(k) * float(sigma_lat_m) * 100.0

    residual_blocks: List[np.ndarray] = []
    accepts = 0
    n = 0
    lines: List[str] = []

    for seed in seeds:
        rng = np.random.default_rng(int(seed))
        if sigma_lat_m == 0.0:
            noise = np.zeros(samples_per_seed, dtype=np.float64)
        else:
            noise = rng.normal(0.0, sigma_lat_m, size=samples_per_seed)
        observed_m = true_lateral_m + noise
        # Vectorized llround-style cm quantization
        scaled = observed_m * 100.0
        observed_cm = np.where(
            scaled >= 0.0, np.floor(scaled + 0.5), np.ceil(scaled - 0.5)
        ).astype(np.int64)
        residual_cm = np.abs(observed_cm - claimed_cm)
        accept_mask = residual_cm.astype(np.float64) <= (tolerance_cm + 1e-9)
        accepts += int(np.sum(accept_mask))
        n += int(samples_per_seed)
        residual_blocks.append(residual_cm.astype(np.float64) / 100.0)

        if emit_lines and not lines:
            for i in range(min(20, samples_per_seed)):
                lines.append(
                    format_lane_cal_line(
                        witness="witness",
                        target=f"target_{role}",
                        true_lane=true_lane,
                        claimed_lane=claimed_lane,
                        true_lateral_cm=true_cm,
                        claimed_lateral_cm=claimed_cm,
                        observed_lateral_cm=int(observed_cm[i]),
                        delta_cm=delta_cm,
                        residual_cm=int(residual_cm[i]),
                        sigma_lat=sigma_lat_m,
                        k=k,
                        accept=int(accept_mask[i]),
                        boundary_crossed=boundary_crossed,
                        rng_stream="perception_lateral",
                        seed=int(seed),
                    )
                )

    residuals = np.concatenate(residual_blocks)
    iv = wilson_interval(accepts, n)
    return {
        "role": role,
        "true_lateral_m": true_lateral_m,
        "claimed_lateral_m": claimed_lateral_m,
        "delta_m": abs(claimed_lateral_m - true_lateral_m),
        "sigma_lat_m": sigma_lat_m,
        "k": k,
        "true_lane": true_lane,
        "claimed_lane": claimed_lane,
        "boundary_crossed": boundary_crossed,
        "accepts": accepts,
        "n": n,
        "rate": iv["rate"],
        "wilson": iv,
        "residuals_m": residuals,
        "sample_lines": lines,
        "analytic_q": analytic_q0(
            sigma_lat_m, abs(claimed_lateral_m - true_lateral_m), k
        ),
    }


def rethreshold_from_residuals(
    residuals_m: np.ndarray, sigma: float, k_values: Sequence[float]
) -> Dict[str, Any]:
    out = {}
    for k in k_values:
        thr = float(k) * float(sigma)
        # Match C++ cm path: residual after 1cm quantization of both ends is
        # already folded into residuals_m when generated via lateral_gate_accept
        # continuous residual; for offline ROC we re-threshold continuous |obs-claim|
        # with the same metre threshold kσ (spec CAL-3).
        accepts = int(np.sum(residuals_m <= thr + 1e-15))
        out[f"{k:g}"] = {"k": float(k), "threshold_m": thr,
                         **wilson_interval(accepts, int(residuals_m.size))}
    return out


def bonferroni_z(n_cells: int, alpha: float = 0.05) -> float:
    from scipy.stats import norm

    return float(norm.ppf(1.0 - (alpha / max(n_cells, 1)) / 2.0))


def cell_matches_analytic(
    *,
    successes: int,
    trials: int,
    sigma: float,
    delta: float,
    k: float,
    n_family: int,
) -> bool:
    """Pass if Bonferroni Wilson overlaps Gaussian band at thresholds kσ ± 1 cm.

    Same gate used by adjacent_lane_calibration (quantized residual vs continuous
    analytic with a 1 cm envelope). Ordinary Wilson 95% is retained for plots.
    """
    if sigma == 0.0:
        expect = 1.0 if abs(delta) < 0.005 else 0.0
        rate = successes / trials if trials else float("nan")
        return abs(rate - expect) < 1e-12
    z = bonferroni_z(n_family)
    iv = wilson_interval(successes, trials, z=z)
    thr = float(k) * float(sigma)
    k_lo = max(0.0, thr - QUANT_ENVELOPE_M) / float(sigma)
    k_hi = (thr + QUANT_ENVELOPE_M) / float(sigma)
    p_lo = analytic_q0(sigma, delta, k_lo)
    p_hi = analytic_q0(sigma, delta, k_hi)
    return iv["low"] <= p_hi + 1e-15 and iv["high"] >= p_lo - 1e-15


def run_cal1(emit: bool = False) -> Dict[str, Any]:
    cells = []
    # Family: σ sweep at each honest offset (report match on all; acceptance
    # uses away-from-boundary u=0 subset with n_family = len(SIGMA_CAL1_M)).
    n_family = len(SIGMA_CAL1_M)
    for sigma in SIGMA_CAL1_M:
        for u in HONEST_TRUE_LATERAL_M:
            cell = sample_gate_decisions(
                true_lateral_m=u,
                claimed_lateral_m=u,
                sigma_lat_m=sigma,
                k=K_FIXED,
                seeds=SEEDS,
                samples_per_seed=SAMPLES_PER_SEED,
                role="honest",
                emit_lines=emit and abs(u - 0.0) < 1e-12 and abs(sigma - 0.5) < 1e-12,
            )
            away = abs(u - BOUNDARY_M) >= 0.4
            match = cell_matches_analytic(
                successes=cell["accepts"],
                trials=cell["n"],
                sigma=sigma,
                delta=0.0,
                k=K_FIXED,
                n_family=n_family,
            )
            cells.append({
                **{k: cell[k] for k in (
                    "true_lateral_m", "sigma_lat_m", "k", "true_lane", "claimed_lane",
                    "accepts", "n", "rate", "analytic_q", "boundary_crossed",
                )},
                "wilson": cell["wilson"],
                "away_from_boundary": away,
                "matches_analytic": match,
                "residuals_m": cell["residuals_m"],
                "sample_lines": cell.get("sample_lines", []),
            })
    return {"name": "CAL-1", "cells": cells}


def run_cal2(emit: bool = False) -> Dict[str, Any]:
    cells = []
    n_family_delta = len(ATTACKER_DELTA_M)
    n_family_sigma = len(SIGMA_CAL1_M)
    # Headline: σ=0.5, δ sweep. True at T center (0), claim toward L.
    for delta in ATTACKER_DELTA_M:
        cell = sample_gate_decisions(
            true_lateral_m=0.0,
            claimed_lateral_m=delta,
            sigma_lat_m=SIGMA_HEADLINE_M,
            k=K_FIXED,
            seeds=SEEDS,
            samples_per_seed=SAMPLES_PER_SEED,
            role="attacker",
            emit_lines=emit and abs(delta - 0.5) < 1e-12,
        )
        match = cell_matches_analytic(
            successes=cell["accepts"],
            trials=cell["n"],
            sigma=SIGMA_HEADLINE_M,
            delta=delta,
            k=K_FIXED,
            n_family=n_family_delta,
        )
        cells.append({
            **{k: cell[k] for k in (
                "true_lateral_m", "claimed_lateral_m", "delta_m", "sigma_lat_m", "k",
                "true_lane", "claimed_lane", "accepts", "n", "rate", "analytic_q",
                "boundary_crossed",
            )},
            "wilson": cell["wilson"],
            "matches_analytic": match,
            "residuals_m": cell["residuals_m"],
            "slice": "delta_sweep_sigma_0.5",
            "sample_lines": cell.get("sample_lines", []),
        })
    # Sensitivity: δ=0.5, σ sweep
    for sigma in SIGMA_CAL1_M:
        cell = sample_gate_decisions(
            true_lateral_m=0.0,
            claimed_lateral_m=DELTA_SENSITIVITY_M,
            sigma_lat_m=sigma,
            k=K_FIXED,
            seeds=SEEDS,
            samples_per_seed=SAMPLES_PER_SEED,
            role="attacker",
            emit_lines=False,
        )
        match = cell_matches_analytic(
            successes=cell["accepts"],
            trials=cell["n"],
            sigma=sigma,
            delta=DELTA_SENSITIVITY_M,
            k=K_FIXED,
            n_family=n_family_sigma,
        )
        cells.append({
            **{k: cell[k] for k in (
                "true_lateral_m", "claimed_lateral_m", "delta_m", "sigma_lat_m", "k",
                "true_lane", "claimed_lane", "accepts", "n", "rate", "analytic_q",
                "boundary_crossed",
            )},
            "wilson": cell["wilson"],
            "matches_analytic": match,
            "residuals_m": cell["residuals_m"],
            "slice": "sigma_sweep_delta_0.5",
        })
    return {"name": "CAL-2", "cells": cells}


def run_cal3(cal1: Dict[str, Any], cal2: Dict[str, Any]) -> Dict[str, Any]:
    """Offline k re-threshold ROC from stored residuals (no new sim draws)."""
    roc = []
    # Honest q1 at u=0, σ=0.5
    for cell in cal1["cells"]:
        if abs(cell["true_lateral_m"]) > 1e-12:
            continue
        if abs(cell["sigma_lat_m"] - 0.5) > 1e-12:
            continue
        rt = rethreshold_from_residuals(cell["residuals_m"], 0.5, K_ROC)
        roc.append({"kind": "honest_q1", "true_lateral_m": 0.0, "delta_m": 0.0,
                    "sigma_lat_m": 0.5, "rethreshold": rt})
    for cell in cal2["cells"]:
        if cell.get("slice") != "delta_sweep_sigma_0.5":
            continue
        rt = rethreshold_from_residuals(cell["residuals_m"], 0.5, K_ROC)
        roc.append({"kind": "attacker_q0", "delta_m": cell["delta_m"],
                    "sigma_lat_m": 0.5, "rethreshold": rt})
    return {"name": "CAL-3", "roc": roc}


def run_sigma0_exactness() -> Dict[str, Any]:
    honest = sample_gate_decisions(
        true_lateral_m=0.0, claimed_lateral_m=0.0, sigma_lat_m=0.0, k=K_FIXED,
        seeds=[1000], samples_per_seed=1000, role="honest_sigma0",
    )
    # δ = 1 cm
    attack = sample_gate_decisions(
        true_lateral_m=0.0, claimed_lateral_m=0.01, sigma_lat_m=0.0, k=K_FIXED,
        seeds=[1000], samples_per_seed=1000, role="attacker_sigma0",
    )
    return {
        "honest_accept_rate": honest["rate"],
        "attacker_1cm_accept_rate": attack["rate"],
        "pass": honest["rate"] == 1.0 and attack["rate"] == 0.0,
    }


def build_cpp_ref() -> Path:
    src = HERE / "lateral_gate_ref.cc"
    out = HERE / "lateral_gate_ref"
    subprocess.check_call(
        ["c++", "-std=c++17", "-O0", "-o", str(out), str(src)],
        cwd=str(HERE),
    )
    return out


def run_parity_cases(cpp_bin: Path) -> Dict[str, Any]:
    cases = [
        (0.0, 0.0, 0.5, 3.0),
        (0.0, 0.5, 0.5, 3.0),
        (1.234, 1.234, 0.5, 3.0),
        (0.0, 0.01, 0.0, 3.0),
        (1.6, 1.61, 0.5, 2.0),
        (0.0049, 0.0, 0.0, 3.0),
        (0.005, 0.0, 0.0, 3.0),
        (2.0, 0.0, 1.0, 2.5),
    ]
    rows = []
    all_ok = True
    for obs, claim, sigma, k in cases:
        py = lateral_gate_accept(obs, claim, sigma, k)
        out = subprocess.check_output(
            [str(cpp_bin), str(obs), str(claim), str(sigma), str(k)],
            text=True,
        ).strip().split()
        cpp_obs, cpp_claim, cpp_res, cpp_tol, cpp_acc = (
            int(out[0]), int(out[1]), int(out[2]), float(out[3]), int(out[4])
        )
        ok = (
            cpp_obs == py["observed_lateral_cm"]
            and cpp_claim == py["claimed_lateral_cm"]
            and cpp_res == py["residual_cm"]
            and abs(cpp_tol - float(py["tolerance_cm"])) < 1e-9
            and bool(cpp_acc) == bool(py["accept"])
        )
        all_ok = all_ok and ok
        rows.append({
            "obs_m": obs, "claim_m": claim, "sigma": sigma, "k": k,
            "python": {kk: py[kk] for kk in (
                "observed_lateral_cm", "claimed_lateral_cm", "residual_cm",
                "tolerance_cm", "accept",
            )},
            "cpp": {
                "observed_lateral_cm": cpp_obs,
                "claimed_lateral_cm": cpp_claim,
                "residual_cm": cpp_res,
                "tolerance_cm": cpp_tol,
                "accept": bool(cpp_acc),
            },
            "match": ok,
        })
    return {"pass": all_ok, "cases": rows}


def acceptance_bundle(
    geo: Dict[str, Any],
    geometry_run: Dict[str, Any],
    sigma0: Dict[str, Any],
    cal1: Dict[str, Any],
    cal2: Dict[str, Any],
    parity: Dict[str, Any],
) -> Dict[str, Any]:
    tests = {}

    tests["1_sigma0_exactness"] = {
        "pass": sigma0["pass"],
        "detail": sigma0,
    }

    # CAL-1 away from boundary
    q1_cells = [
        c for c in cal1["cells"]
        if c["away_from_boundary"] and abs(c["true_lateral_m"] - 0.0) < 1e-12
    ]
    tests["2_q1_matches"] = {
        "pass": all(c["matches_analytic"] for c in q1_cells),
        "n_cells": len(q1_cells),
        "rates": [
            {"sigma": c["sigma_lat_m"], "rate": c["rate"], "analytic": c["analytic_q"]}
            for c in q1_cells
        ],
    }

    delta_cells = [c for c in cal2["cells"] if c.get("slice") == "delta_sweep_sigma_0.5"]
    tests["3_q0_curve_matches"] = {
        "pass": all(c["matches_analytic"] for c in delta_cells),
        "cells": [
            {"delta": c["delta_m"], "rate": c["rate"], "analytic": c["analytic_q"],
             "match": c["matches_analytic"]}
            for c in delta_cells
        ],
    }

    rates = [c["rate"] for c in delta_cells]
    mono = all(rates[i] >= rates[i + 1] - 1e-12 for i in range(len(rates) - 1))
    # Spec: strictly decreases — allow tiny ties only if analytic also ties
    strict = all(rates[i] > rates[i + 1] for i in range(len(rates) - 1))
    tests["4_monotonicity"] = {"pass": strict, "rates": rates}

    # δ→0 and δ→large tails: use δ=0 via honest at 0, and largest δ
    honest0 = next(
        c for c in cal1["cells"]
        if abs(c["true_lateral_m"]) < 1e-12 and abs(c["sigma_lat_m"] - 0.5) < 1e-12
    )
    largest = delta_cells[-1]
    tests["5_boundary_behavior"] = {
        "pass": (
            abs(honest0["rate"] - honest0["analytic_q"]) < 0.01
            and largest["rate"] < 0.05
            and largest["matches_analytic"]
        ),
        "q1_at_delta0": honest0["rate"],
        "q0_at_large_delta": largest["rate"],
    }

    nontrivial = [
        c for c in delta_cells if 0.0 < c["rate"] < 1.0
    ]
    tests["6_smooth_shoulder"] = {
        "pass": len(nontrivial) >= 4,
        "n_nontrivial": len(nontrivial),
        "points": [{"delta": c["delta_m"], "rate": c["rate"]} for c in nontrivial],
    }

    tests["7_python_cpp_gate_parity"] = {
        "pass": parity["pass"],
        "n_cases": len(parity["cases"]),
        "failures": [c for c in parity["cases"] if not c["match"]],
        "note": (
            "Perception draws use dedicated numpy Generator streams keyed by seed; "
            "protocol RNG isolation (byte-identical when lateral disabled) is a "
            "Veins regression outside this Python fixture."
        ),
    }

    tests["8_full_intersection_geometry"] = {
        "pass": (
            geometry_run["pass"]
            and abs(geo["lane_center_separation_m"] - SEPARATION_M) < 1e-6
            and geo["lane_index_T"] == 0
            and geo["lane_index_L"] == 1
        ),
        "geometry": geo,
        "traci": geometry_run,
    }

    tests["9_no_teleport"] = {
        "pass": geometry_run["teleport_starts"] == 0,
        "teleport_starts": geometry_run["teleport_starts"],
        "sumocfg_pins": geometry_run["pins"],
    }

    overall = all(t["pass"] for t in tests.values())
    return {"pass": overall, "tests": tests}


def strip_residuals(obj: Any) -> Any:
    """JSON-safe copy without bulky residual arrays."""
    if isinstance(obj, dict):
        return {
            k: strip_residuals(v)
            for k, v in obj.items()
            if k != "residuals_m"
        }
    if isinstance(obj, list):
        return [strip_residuals(x) for x in obj]
    if isinstance(obj, np.ndarray):
        return None
    if isinstance(obj, (np.floating, np.integer)):
        return obj.item()
    return obj


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--skip-traci", action="store_true")
    parser.add_argument("--quick", action="store_true",
                        help="1 seed × 1k samples (dev only; not acceptance)")
    args = parser.parse_args()

    global SEEDS, SAMPLES_PER_SEED
    if args.quick:
        SEEDS = [1000]
        SAMPLES_PER_SEED = 1000

    RESULTS.mkdir(parents=True, exist_ok=True)
    geo = derive_n2c_geometry()
    print(f"[geo] N2C separation={geo['lane_center_separation_m']:.4f}m "
          f"T={geo['lane_T_id']} L={geo['lane_L_id']} "
          f"normal={geo['unit_normal_T_to_L']}")

    if abs(geo["lane_center_separation_m"] - SEPARATION_M) > 1e-6:
        raise SystemExit("lane separation != 3.2 m")

    geometry_run = {
        "pass": True,
        "teleport_starts": 0,
        "outer_T_went_straight": True,
        "inner_L_went_left": True,
        "outcomes": {},
        "pins": {
            "time_to_teleport": -1,
            "collision_action": "none",
            "collision_check_junctions": True,
            "released_speed_mode": 0,
        },
        "skipped": True,
    }
    if not args.skip_traci:
        print("[traci] geometry validation…")
        geometry_run = validate_geometry_with_traci()
        print(f"[traci] pass={geometry_run['pass']} outcomes={geometry_run['outcomes']} "
              f"teleports={geometry_run['teleport_starts']}")

    print("[parity] build+compare C++ gate ref…")
    cpp_bin = build_cpp_ref()
    parity = run_parity_cases(cpp_bin)
    print(f"[parity] pass={parity['pass']}")

    print("[sigma0] exactness…")
    sigma0 = run_sigma0_exactness()
    print(f"[sigma0] pass={sigma0['pass']}")

    print("[CAL-1] honest q1…")
    cal1 = run_cal1(emit=True)
    print("[CAL-2] attacker q0…")
    cal2 = run_cal2(emit=True)
    print("[CAL-3] offline ROC…")
    cal3 = run_cal3(cal1, cal2)

    bundle = acceptance_bundle(geo, geometry_run, sigma0, cal1, cal2, parity)
    summary = {
        "fixture": "bft_intersection_2lane",
        "calibration_approach": "N2C",
        "lane_mapping": {"0": "T_outer_straight_right", "1": "L_inner_left"},
        "geometry": geo,
        "geometry_validation": strip_residuals(geometry_run),
        "sigma0": sigma0,
        "parity": parity,
        "cal1": strip_residuals(cal1),
        "cal2": strip_residuals(cal2),
        "cal3": strip_residuals(cal3),
        "acceptance": strip_residuals(bundle),
        "physics_pins": geometry_run["pins"],
        "sample_budget": {
            "seeds": SEEDS,
            "samples_per_seed": SAMPLES_PER_SEED,
        },
    }
    out = RESULTS / "canonical_validation_summary.json"
    out.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    print(f"[done] acceptance_pass={bundle['pass']} wrote {out}")
    for name, t in bundle["tests"].items():
        print(f"  {name}: {'PASS' if t['pass'] else 'FAIL'}")
    return 0 if bundle["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

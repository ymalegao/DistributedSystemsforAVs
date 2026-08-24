#!/usr/bin/env python3
"""Lane-local lateral residual gate — must match ResDBArrivalProtocol ADJACENT_LATERAL.

C++ reference (ResDBArrivalProtocol.cc):
  residualCm = |observedLateralCm - claimedLateralCm|
  toleranceCm = physicalGateK * lateralObservationSigmaM * 100
  accept iff residualCm <= toleranceCm

Centimetre quantization matches ResDBPerception / ownLateralClaimCm:
  cm = llround(meters * 100.0)   # half away from zero
"""

from __future__ import annotations

import math
from typing import Dict, Tuple


def quantize_cm(meters: float) -> int:
    """Match C++ std::llround(meters * 100.0)."""
    scaled = float(meters) * 100.0
    if scaled >= 0.0:
        return int(math.floor(scaled + 0.5))
    return int(math.ceil(scaled - 0.5))


def lateral_gate_accept(
    observed_lateral_m: float,
    claimed_lateral_m: float,
    sigma_lat_m: float,
    k: float = 3.0,
) -> Dict[str, object]:
    """Pure gate: same accept/reject as the C++ protocol path."""
    if sigma_lat_m < 0.0:
        raise ValueError("sigma_lat_m must be non-negative")
    if k < 0.0:
        raise ValueError("k must be non-negative")
    observed_cm = quantize_cm(observed_lateral_m)
    claimed_cm = quantize_cm(claimed_lateral_m)
    residual_cm = abs(observed_cm - claimed_cm)
    tolerance_cm = float(k) * float(sigma_lat_m) * 100.0
    accept = residual_cm <= tolerance_cm + 1e-9
    return {
        "observed_lateral_cm": observed_cm,
        "claimed_lateral_cm": claimed_cm,
        "residual_cm": residual_cm,
        "tolerance_cm": tolerance_cm,
        "sigma_lat_m": float(sigma_lat_m),
        "k": float(k),
        "accept": bool(accept),
    }


def project_physical_lane_index(
    lateral_cm: int, separation_m: float = 3.2
) -> int:
    """Match ResDBPerception::projectPhysicalLaneIndex. -1 = AMBIGUOUS at boundary."""
    boundary_cm = separation_m * 50.0  # 1.6 m → 160 cm
    if abs(float(lateral_cm) - boundary_cm) < 0.5:
        return -1
    return 0 if float(lateral_cm) < boundary_cm else 1


def lane_label(index: int) -> str:
    if index == 0:
        return "T"
    if index == 1:
        return "L"
    return "AMBIGUOUS"


def analytic_q0(sigma: float, delta: float, k: float) -> float:
    """Φ(k − Δ/σ) − Φ(−k − Δ/σ). No √2 (1-D residual vs fixed claimed centerline)."""
    if sigma < 0.0:
        raise ValueError("sigma must be non-negative")
    if sigma == 0.0:
        # After 1 cm quantization: accept only if |Δ| < 0.5 cm before round,
        # but exactness test uses δ ≥ 1 cm → reject. Closed form at σ=0:
        return 1.0 if abs(delta) <= 1e-15 else 0.0
    from scipy.stats import norm

    d = float(delta) / float(sigma)
    return float(norm.cdf(k - d) - norm.cdf(-k - d))


def wilson_interval(
    successes: int, trials: int, z: float = 1.959963984540054
) -> Dict[str, float]:
    if trials <= 0:
        return {"successes": successes, "trials": trials, "rate": float("nan"),
                "low": float("nan"), "high": float("nan")}
    p = successes / trials
    denom = 1.0 + z * z / trials
    center = (p + z * z / (2.0 * trials)) / denom
    half = z * math.sqrt(p * (1.0 - p) / trials + z * z / (4.0 * trials * trials)) / denom
    return {
        "successes": int(successes),
        "trials": int(trials),
        "rate": p,
        "low": max(0.0, center - half),
        "high": min(1.0, center + half),
    }


def format_lane_cal_line(
    *,
    witness: str,
    target: str,
    true_lane: str,
    claimed_lane: str,
    true_lateral_cm: int,
    claimed_lateral_cm: int,
    observed_lateral_cm: int,
    delta_cm: int,
    residual_cm: int,
    sigma_lat: float,
    k: float,
    accept: int,
    boundary_crossed: int,
    rng_stream: str,
    seed: int,
) -> str:
    return (
        f"[LANE-CAL] witness={witness} target={target} "
        f"trueLane={true_lane} claimedLane={claimed_lane} "
        f"trueLateral_cm={true_lateral_cm} claimedLateral_cm={claimed_lateral_cm} "
        f"observedLateral_cm={observed_lateral_cm} delta_cm={delta_cm} "
        f"residual_cm={residual_cm} sigma_lat={sigma_lat} k={k} "
        f"accept={accept} boundary_crossed={boundary_crossed} "
        f"rng_stream={rng_stream} seed={seed}"
    )

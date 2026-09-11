"""Pure experiment design and analysis for the N=16 longitudinal grid."""

from __future__ import annotations

import math
from pathlib import Path
from typing import Any, Dict, List, Sequence, Tuple


N = 16
TARGET = 15
PREDECESSOR = 11
SEPARATIONS_M = (5.0, 5.5, 6.5, 7.5, 9.5, 12.5)
SIGMAS_M = (0.25, 0.5, 1.0, 2.0)
MAIN_SIGMA_M = 1.0
ATTACK_AHEAD_MARGIN_M = 0.1
RETHRESHOLD_K = (2.0, 2.5, 3.0)


def manifest(
    profile: str, f: int, physical_gate_k: float = 3.0
) -> List[Dict[str, Any]]:
    if profile not in ("smoke", "full"):
        raise ValueError(f"unsupported longitudinal grid profile: {profile}")
    cells: Dict[Tuple[float, float, int, int], Dict[str, Any]] = {}

    def add(separation: float, sigma: float, b: int, rep: int, purpose: str) -> None:
        key = (separation, sigma, b, rep)
        row = cells.setdefault(key, {
            "n": N, "target": TARGET, "predecessor": PREDECESSOR,
            "reference_separation_m": separation, "sigma_long_m": sigma,
            "b": b, "rep": rep, "purposes": [],
        })
        if not math.isclose(float(physical_gate_k), 3.0):
            row["physical_gate_k"] = float(physical_gate_k)
        if purpose not in row["purposes"]:
            row["purposes"].append(purpose)

    if profile == "smoke":
        add(5.0, 1.0, f, 0, "shoulder_plumbing")
        add(5.0, 1.0, f + 1, 0, "cliff_plumbing")
        add(5.0, 2.0, f, 0, "sigma_plumbing")
        add(12.5, 1.0, f, 0, "large_separation_tail")
    else:
        for separation in SEPARATIONS_M:
            for rep in range(20):
                add(separation, MAIN_SIGMA_M, f, rep, "gap_shoulder_b_f")
        for sigma in SIGMAS_M:
            for rep in range(20):
                add(5.0, sigma, f, rep, "sigma_sensitivity_b_f")
        for b in range(f + 2):
            add(5.0, MAIN_SIGMA_M, b, 0,
                "cliff_b_f_plus_1" if b == f + 1 else "byzantine_boundary")
    return sorted(cells.values(), key=lambda row: (
        row["reference_separation_m"], row["sigma_long_m"], row["b"], row["rep"]
    ))


def output_dir(repo_root: Path, physical_gate_k: float) -> Path:
    if math.isclose(float(physical_gate_k), 3.0):
        return repo_root / "benchmarks" / "Phase2DistanceGrid"
    label = format(float(physical_gate_k), "g").replace(".", "p")
    return repo_root / "benchmarks" / f"Phase2DistanceGridK{label}"


def run_dir(repo_root: Path, cell: Dict[str, Any]) -> Path:
    separation = format(cell["reference_separation_m"], "g").replace(".", "p")
    sigma = format(cell["sigma_long_m"], "g").replace(".", "p")
    physical_gate_k = float(cell.get("physical_gate_k", 3.0))
    fixture_version = "fixture_v2" if math.isclose(physical_gate_k, 3.0) else "fixture_v3"
    return (output_dir(repo_root, physical_gate_k) / fixture_version /
            f"target_veh{cell['target']}" / f"b_{cell['b']}" /
            f"separation_{separation}_sigma_{sigma}" / f"run_{cell['rep']}")


def normal_cdf(value: float) -> float:
    return 0.5 * (1.0 + math.erf(value / math.sqrt(2.0)))


def absolute_claim_accept_probability(offset_m: float, sigma_m: float, k: float) -> float:
    if sigma_m == 0.0:
        return 1.0 if offset_m == 0.0 else 0.0
    tolerance = k * sigma_m
    return max(0.0, min(1.0,
        normal_cdf((offset_m + tolerance) / sigma_m) -
        normal_cdf((offset_m - tolerance) / sigma_m)))


def binomial_tail(n: int, probability: float | None, required: int) -> float | None:
    if probability is None:
        return None
    required = max(0, int(required))
    if required == 0:
        return 1.0
    if required > n:
        return 0.0
    return sum(math.comb(n, successes) * probability ** successes *
               (1.0 - probability) ** (n - successes)
               for successes in range(required, n + 1))


def wilson(successes: int, trials: int, z: float = 1.959963984540054) -> Dict[str, Any]:
    if trials <= 0:
        return {"successes": successes, "trials": trials, "rate": None, "low": None, "high": None}
    p = successes / trials
    denominator = 1.0 + z * z / trials
    center = (p + z * z / (2.0 * trials)) / denominator
    half = z * math.sqrt(p * (1.0 - p) / trials + z * z / (4.0 * trials * trials)) / denominator
    return {"successes": successes, "trials": trials, "rate": p,
            "low": max(0.0, center - half), "high": min(1.0, center + half)}


def mean(values: Sequence[float | int | None]) -> float | None:
    usable = [float(value) for value in values if value is not None]
    return sum(usable) / len(usable) if usable else None


def analyze_run(cell: Dict[str, Any], records: List[Dict[str, Any]], result_dir: Path, f: int) -> Dict[str, Any]:
    if not records:
        raise ValueError(f"empty analyzer output in {result_dir}")
    perception = records[0]["bft_stats"]["perception"]
    distance = perception["stopped_distance"]
    attack = perception.get("phase2_attack") or {}
    byzantine = set(attack.get("configured_byzantine_ids", []))
    target_name, predecessor_name = f"veh{TARGET}", f"veh{PREDECESSOR}"
    pair = next((row for row in distance.get("order_pairs", [])
                 if {row["a"], row["b"]} == {target_name, predecessor_name}), None)
    measured_separation = (abs(float(pair["true_a_m"]) - float(pair["true_b_m"]))
                           if pair else None)
    declaration = attack.get("distance_declaration")
    offset = float(declaration["offset_m"]) if declaration else 0.0
    sigma = float(cell["sigma_long_m"])
    physical_gate_k = float(cell.get("physical_gate_k", 3.0))
    analytic_q = (
        absolute_claim_accept_probability(offset, sigma, physical_gate_k)
        if cell["b"] else None
    )
    h = int(attack.get("h_distance") or 0)
    certificate_b_sig = int(attack.get("b_sig_distance") or 0)
    threshold = int(attack.get("threshold") or f + 1)
    evaluations = [row for row in distance.get("evaluations", [])
                   if row["target"] == target_name and row["witness"] not in byzantine]
    collection = [row for row in distance.get("collection_order", [])
                  if row["target"] == target_name and int(row["epoch"]) == 0]
    collected_signers = {int(row["signer"]) for row in collection}
    # The claimant's signed self-attestation enters the local collector directly
    # and therefore is not represented by a DIST-CERT-COLLECT radio record.
    self_support = 1 if TARGET in byzantine and int(cell["b"]) > 0 else 0
    external_byzantine_support = len((byzantine - {TARGET}) & collected_signers)
    available_b_sig = self_support + external_byzantine_support
    required = max(0, threshold - available_b_sig)
    true_claim_evaluations = [
        row for row in distance.get("evaluations", [])
        if row["target"] != target_name and row["witness"] not in byzantine
    ]
    rethreshold = []
    for k in RETHRESHOLD_K:
        tolerance_cm = int(math.floor(k * sigma * 100.0 + 0.5))
        honest_support = sum(bool(row["stationary"]) and int(row["residual_cm"]) <= tolerance_cm
                             for row in evaluations)
        analytic_q_k = absolute_claim_accept_probability(offset, sigma, k) if cell["b"] else None
        true_accepts = sum(bool(row["stationary"]) and int(row["residual_cm"]) <= tolerance_cm
                           for row in true_claim_evaluations)
        rethreshold.append({
            "k": k, "tolerance_cm": tolerance_cm,
            "honest_support": honest_support, "honest_trials": len(evaluations),
            "empirical_q_dist": honest_support / len(evaluations) if evaluations else None,
            "analytic_q_dist": analytic_q_k,
            "certificate_local_b_sig": certificate_b_sig,
            "attempt_available_b_sig": available_b_sig,
            "counterfactual_threshold_met": available_b_sig + honest_support >= threshold,
            "binomial_tail_prediction": binomial_tail(h, analytic_q_k, required),
            "honest_true_accepts": true_accepts,
            "honest_true_trials": len(true_claim_evaluations),
            "empirical_q1_dist": (
                true_accepts / len(true_claim_evaluations) if true_claim_evaluations else None
            ),
            "analytic_q1_dist": absolute_claim_accept_probability(0.0, sigma, k),
        })
    return {
        **cell, "run_dir": str(result_dir),
        "simulation_completed": len(records) == N,
        "single_evaluation_invariant_ok": distance.get("single_evaluation_invariant_ok", False),
        "target_distance_certified": bool(attack.get("distance_certified")),
        "committed_target_pair_present": pair is not None,
        "committed_target_pair_inversion": bool(pair and pair["inversion"]),
        "whole_queue_order_violation": int(distance.get("order_inversion_count", 0)) > 0,
        "whole_queue_inversion_pair_count": int(distance.get("order_inversion_count", 0)),
        "requested_reference_separation_m": float(cell["reference_separation_m"]),
        "measured_reference_separation_m": measured_separation,
        "distance_claim_offset_m": offset,
        "h_distance": h, "certificate_local_b_sig": certificate_b_sig,
        "attempt_available_b_sig": available_b_sig,
        "self_attestation_support_available": self_support,
        "external_byzantine_support_collected": external_byzantine_support,
        "required_honest_support": required, "threshold": threshold,
        "empirical_q_dist": attack.get("q0_distance"), "analytic_q_dist": analytic_q,
        "protocol_binomial_tail_prediction": binomial_tail(h, analytic_q, required),
        "prediction_basis": "absolute q_dist + measured h + attempt-local available b_sig",
        "collection_signer_arrival_order": [row["signer"] for row in collection],
        "collection_threshold_first_reached_at": next((i + 1 for i, row in enumerate(collection)
                                                        if row["signer_count"] >= row["threshold"]), None),
        "finalized_certificate_signers": attack.get("distance_certificate_signers", []),
        "offline_rethreshold": rethreshold,
    }


def aggregate(rows: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
    groups: Dict[Tuple[float, float, int], List[Dict[str, Any]]] = {}
    for row in rows:
        groups.setdefault((row["reference_separation_m"], row["sigma_long_m"], row["b"]), []).append(row)
    output = []
    for (separation, sigma, b), group in sorted(groups.items()):
        output.append({
            "reference_separation_m": separation, "sigma_long_m": sigma, "b": b,
            "physical_gate_k": float(group[0].get("physical_gate_k", 3.0)),
            "repetitions": len(group),
            "purposes": sorted({p for row in group for p in row["purposes"]}),
            "committed_per_pair_inversion": wilson(
                sum(bool(row["committed_target_pair_inversion"]) for row in group), len(group)),
            "whole_queue_order_violation_empirical_only": wilson(
                sum(bool(row["whole_queue_order_violation"]) for row in group), len(group)),
            "mean_protocol_prediction": mean([row["protocol_binomial_tail_prediction"] for row in group]),
            "mean_analytic_q_dist": mean([row["analytic_q_dist"] for row in group]),
            "mean_empirical_q_dist": mean([row["empirical_q_dist"] for row in group]),
            "mean_h_distance": mean([row["h_distance"] for row in group]),
            "mean_certificate_local_b_sig": mean([row["certificate_local_b_sig"] for row in group]),
            "mean_attempt_available_b_sig": mean([row["attempt_available_b_sig"] for row in group]),
            "mean_measured_reference_separation_m": mean([
                row["measured_reference_separation_m"] for row in group]),
        })
    return output

"""Locked N=16 full-intersection two-lane experiment manifest helpers.

The filename is retained so the already-reviewed manifest tests keep one source
of truth.  The runner uses this matrix only with the full four-way two-lane
fixture; the former straight-road experiment is retired.
"""

from __future__ import annotations

import math
import statistics
from collections import defaultdict
from pathlib import Path
from typing import Any, Dict, Iterable, List, Sequence, Tuple


N = 16
F = 5
TARGET = 0
MAIN_K = 3.0
MAIN_SIGMA_M = 0.5
MAIN_DELTA_M = 1.75

# Reviewed joint operating point.  These three cells validate composition of
# the already-calibrated lateral and longitudinal channels; they are not a new
# statistical sweep and deliberately keep one false channel per attack run so
# any physical consequence remains attributable.
COMBINED_K = 2.0
DIRECTION_ABLATION_REPS = 20
STRAIGHT_HEAVY_DIRECTION_FIXTURE_COUNT = 4
STRAIGHT_HEAVY_DIRECTION_CONFIGS: Tuple[str, ...] = tuple(
    f"SixteenVehiclesDirectionAblationFixture{fixture_id}ResDB"
    for fixture_id in range(STRAIGHT_HEAVY_DIRECTION_FIXTURE_COUNT)
)
STRAIGHT_HEAVY_MANEUVER_MIX = {"left": 4, "straight": 8, "right": 4}
COMBINED_SIGMA_LAT_M = 0.5
COMBINED_SIGMA_LONG_M = 1.0
COMBINED_DELTA_M = 1.75

DELTA_SHOULDER_M: Tuple[float, ...] = (1.61, 1.75, 2.0, 2.4, 3.2)
SIGMA_SENSITIVITY_M: Tuple[float, ...] = (
    0.0, 0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0,
)
BYZANTINE_BOUNDARY_B: Tuple[int, ...] = (0, 1, 2, 3, 4, 6)
K_TRADEOFF: Tuple[float, ...] = (2.0, 2.5, 3.0)

SHOULDER_REPS = 20
# Same seed budget as δ / k / σ panels. The prior BOUNDARY_REPS=5 scout
# produced a non-monotonic b-panel (seed-count artifact, not a real shape).
BOUNDARY_REPS = 20

EXPECTED_UNIQUE_RUNS = 400

# Focused cross-product used to resolve the Byzantine shoulder at three
# reviewed claim offsets.  The smoke intentionally contains attacks only;
# the full profile adds one non-repeated honest control at effective delta 0.
DELTA_B_SWEEP_M: Tuple[float, ...] = (1.61, 1.75, 2.0)
DELTA_B_SWEEP_B: Tuple[int, ...] = (1, 2, 3, 4, 5, 6)
DELTA_B_SWEEP_REPS = 20
DELTA_B_SMOKE_RUNS = 18
DELTA_B_FULL_ATTACK_RUNS = 360
DELTA_B_FULL_TOTAL_RUNS = 361

K_SWEEP: Tuple[float, ...] = (1.0, 2.0, 3.0)
SIGMA_SWEEP_M: Tuple[float, ...] = (0.1, 0.3, 0.5, 0.7, 1.0, 1.5, 2.0)
PARAMETER_SWEEP_B: Tuple[int, ...] = (1, 2, 3, 4, 5, 6)
PARAMETER_SWEEP_REPS = 20

# Honest operating-point selection. Delta is necessarily zero for an honest
# declaration; duplicating honest runs under nonzero "delta" labels would be
# scientifically misleading because the runtime claim bytes would be equal.
HONEST_K_SWEEP: Tuple[float, ...] = K_SWEEP
HONEST_SIGMA_SWEEP_M: Tuple[float, ...] = (0.0, *SIGMA_SWEEP_M)
HONEST_OPERATING_REPS = 20


def _key(row: Dict[str, Any]) -> Tuple[Any, ...]:
    return (
        int(row["b"]), float(row["sigma_lat_m"]),
        float(row["delta_m"]), float(row["k"]), int(row["rep"]),
    )


def manifest() -> List[Dict[str, Any]]:
    """Return the reviewed matrix, de-duplicated across overlapping slices."""
    cells: Dict[Tuple[Any, ...], Dict[str, Any]] = {}

    def add(*, b: int, sigma: float, delta: float, k: float,
            rep: int, purpose: str) -> None:
        row: Dict[str, Any] = {
            "b": b,
            "sigma_lat_m": sigma,
            "delta_m": delta,
            "k": k,
            "rep": rep,
            "purposes": [],
        }
        key = _key(row)
        if key not in cells:
            cells[key] = row
        if purpose not in cells[key]["purposes"]:
            cells[key]["purposes"].append(purpose)

    # Primary attack-offset shoulder at the realistic headline sigma.
    for delta in DELTA_SHOULDER_M:
        for rep in range(SHOULDER_REPS):
            add(b=F, sigma=MAIN_SIGMA_M, delta=delta, k=MAIN_K,
                rep=rep, purpose="delta_shoulder_b_f")

    # Environment sensitivity.  The sigma=.5/delta=1.75 slice overlaps above.
    for sigma in SIGMA_SENSITIVITY_M:
        for rep in range(SHOULDER_REPS):
            add(b=F, sigma=sigma, delta=MAIN_DELTA_M, k=MAIN_K,
                rep=rep, purpose="sigma_sensitivity_b_f")

    # Low-cost Byzantine boundary controls around the headline operating point.
    # b=f is already represented by 20 shoulder repetitions.
    for b in BYZANTINE_BOUNDARY_B:
        for rep in range(BOUNDARY_REPS):
            add(b=b, sigma=MAIN_SIGMA_M, delta=MAIN_DELTA_M, k=MAIN_K,
                rep=rep, purpose="byzantine_boundary")

    # Fresh protocol runs for k; k=3 overlaps the headline shoulder cell.
    for k in K_TRADEOFF:
        for rep in range(SHOULDER_REPS):
            add(b=F, sigma=MAIN_SIGMA_M, delta=MAIN_DELTA_M, k=k,
                rep=rep, purpose="k_tradeoff")

    rows = sorted(cells.values(), key=_key)
    if len(rows) != EXPECTED_UNIQUE_RUNS:
        raise AssertionError(
            f"locked adjacent-lane matrix changed: {len(rows)} != "
            f"{EXPECTED_UNIQUE_RUNS}"
        )
    return rows


def delta_b_manifest(profile: str) -> List[Dict[str, Any]]:
    """Return the focused delta x Byzantine cross-product.

    ``smoke`` runs one repetition for each of the 18 attack cells. ``full``
    runs 20 paired repetitions per attack cell and one honest control total,
    rather than incorrectly duplicating the effective-delta-zero control for
    every requested attack offset.
    """
    if profile not in ("smoke", "full"):
        raise ValueError(f"unknown delta-b profile: {profile}")

    rows: List[Dict[str, Any]] = []
    repetitions = 1 if profile == "smoke" else DELTA_B_SWEEP_REPS
    if profile == "full":
        rows.append({
            "b": 0,
            "sigma_lat_m": MAIN_SIGMA_M,
            "delta_m": 0.0,
            "k": MAIN_K,
            "rep": 0,
            "purposes": ["delta_b_honest_control"],
        })
    for delta in DELTA_B_SWEEP_M:
        for b in DELTA_B_SWEEP_B:
            for rep in range(repetitions):
                rows.append({
                    "b": b,
                    "sigma_lat_m": MAIN_SIGMA_M,
                    "delta_m": delta,
                    "k": MAIN_K,
                    "rep": rep,
                    "purposes": ["delta_b_attack_sweep"],
                })
    rows.sort(key=_key)
    expected = DELTA_B_SMOKE_RUNS if profile == "smoke" else DELTA_B_FULL_TOTAL_RUNS
    if len(rows) != expected or len({_key(row) for row in rows}) != expected:
        raise AssertionError(
            f"delta-b {profile} matrix changed: {len(rows)} != {expected}"
        )
    return rows


def parameter_sweep_manifest(sweep: str, profile: str) -> List[Dict[str, Any]]:
    """Return Figure B (k) or Figure C (sigma) cells.

    The common k=3, sigma=.5, delta=1.75 rows intentionally receive the same
    purpose and key so both sweeps share one completion-capable artifact set.
    """
    if sweep not in ("k", "sigma"):
        raise ValueError(f"unknown parameter sweep: {sweep}")
    if profile not in ("smoke", "full"):
        raise ValueError(f"unknown parameter sweep profile: {profile}")
    repetitions = 1 if profile == "smoke" else PARAMETER_SWEEP_REPS
    rows: List[Dict[str, Any]] = []
    x_values = K_SWEEP if sweep == "k" else SIGMA_SWEEP_M
    for x_value in x_values:
        for b in PARAMETER_SWEEP_B:
            for rep in range(repetitions):
                k = float(x_value) if sweep == "k" else MAIN_K
                sigma = MAIN_SIGMA_M if sweep == "k" else float(x_value)
                is_anchor = k == MAIN_K and sigma == MAIN_SIGMA_M
                rows.append({
                    "b": b,
                    "sigma_lat_m": sigma,
                    "delta_m": MAIN_DELTA_M,
                    "k": k,
                    "rep": rep,
                    "purposes": [
                        "parameter_sweep_shared_anchor" if is_anchor
                        else f"parameter_sweep_{sweep}"
                    ],
                })
    rows.sort(key=_key)
    expected_cells = len(x_values) * len(PARAMETER_SWEEP_B)
    expected = expected_cells * repetitions
    if len(rows) != expected or len({_key(row) for row in rows}) != expected:
        raise AssertionError(
            f"{sweep} {profile} matrix changed: {len(rows)} != {expected}"
        )
    return rows


def honest_operating_manifest(profile: str) -> List[Dict[str, Any]]:
    """Return the attack-free k x sigma operating-point matrix."""
    if profile not in ("smoke", "k-full", "full"):
        raise ValueError(f"unknown honest operating profile: {profile}")
    repetitions = 1 if profile == "smoke" else HONEST_OPERATING_REPS
    sigmas = (MAIN_SIGMA_M,) if profile == "k-full" else HONEST_SIGMA_SWEEP_M
    rows = [
        {
            "b": 0,
            "sigma_lat_m": sigma,
            "delta_m": 0.0,
            "k": k,
            "rep": rep,
            "purposes": ["parameter_sweep_honest_operating"],
        }
        for sigma in sigmas
        for k in HONEST_K_SWEEP
        for rep in range(repetitions)
    ]
    rows.sort(key=_key)
    expected = (
        len(sigmas) * len(HONEST_K_SWEEP) * repetitions
    )
    if len(rows) != expected or len({_key(row) for row in rows}) != expected:
        raise AssertionError(
            f"honest operating {profile} matrix changed: {len(rows)} != {expected}"
        )
    return rows


def combined_validation_manifest() -> List[Dict[str, Any]]:
    """Return the three sequential N=16 joint-noise validation cells."""
    rows = [
        {
            "name": "honest_joint_noise",
            "b": 0,
            "sigma_lat_m": COMBINED_SIGMA_LAT_M,
            "sigma_long_m": COMBINED_SIGMA_LONG_M,
            "delta_m": 0.0,
            "k": COMBINED_K,
            "rep": 0,
            "purposes": ["combined_operating_honest"],
        },
        {
            "name": "lane_shoulder_b_f_joint_noise",
            "b": F,
            "sigma_lat_m": COMBINED_SIGMA_LAT_M,
            "sigma_long_m": COMBINED_SIGMA_LONG_M,
            "delta_m": COMBINED_DELTA_M,
            "k": COMBINED_K,
            "rep": 0,
            "purposes": ["combined_operating_lane_shoulder"],
        },
        {
            "name": "lane_cliff_b_f_plus_1_joint_noise",
            "b": F + 1,
            "sigma_lat_m": COMBINED_SIGMA_LAT_M,
            "sigma_long_m": COMBINED_SIGMA_LONG_M,
            "delta_m": COMBINED_DELTA_M,
            "k": COMBINED_K,
            "rep": 0,
            "purposes": ["combined_operating_lane_cliff"],
        },
    ]
    if len(rows) != 3 or len({row["name"] for row in rows}) != 3:
        raise AssertionError("combined validation manifest changed")
    return rows


def combined_validation_run_dir(repo_root: Path, row: Dict[str, Any]) -> Path:
    return (
        repo_root / "benchmarks" / "Phase2TwoLaneCombinedValidation" /
        str(row["name"]) / f"run_{int(row['rep'])}"
    )


def direction_ablation_manifest(profile: str = "smoke") -> List[Dict[str, Any]]:
    """Honest and FALSE_DIRECTION rows for the three scheduling policies."""
    if profile not in ("smoke", "full"):
        raise ValueError(f"unknown direction-ablation profile: {profile}")
    repetitions = 1 if profile == "smoke" else DIRECTION_ABLATION_REPS
    rows: List[Dict[str, Any]] = []
    attacks = (
        ("honest", 0, "NONE"),
        ("false_direction_b_f", F, "FALSE_DIRECTION"),
    )
    modes = (
        ("eligibility_on", True, False),
        ("eligibility_off", False, False),
        ("all_singleton", True, True),
    )
    # Interleave the six policies within each repetition. Besides sharing the
    # same deterministic seed, this avoids comparing blocks run hours apart.
    for rep in range(repetitions):
        for attack_name, b, attack_kind in attacks:
            for mode, eligibility, singleton in modes:
                row = {
                    "name": f"{attack_name}_{mode}",
                    "ablation_mode": mode,
                    "direction_eligibility_enabled": eligibility,
                    "all_singleton_scheduling": singleton,
                    "attack_kind": attack_kind,
                    "b": b,
                    "sigma_lat_m": COMBINED_SIGMA_LAT_M,
                    "sigma_long_m": COMBINED_SIGMA_LONG_M,
                    "signal_error": 0.2,
                    "delta_m": 0.0,
                    "k": COMBINED_K,
                    "rep": rep,
                    "purposes": [f"direction_ablation_{profile}"],
                }
                if profile == "full":
                    row["profile"] = profile
                rows.append(row)
    expected = 6 if profile == "smoke" else 6 * DIRECTION_ABLATION_REPS
    if len(rows) != expected:
        raise AssertionError(
            f"direction-ablation {profile} matrix changed: {len(rows)} != {expected}"
        )
    return rows


def direction_ablation_run_dir(
    repo_root: Path, row: Dict[str, Any], profile: str = "smoke"
) -> Path:
    root = (
        "Phase2DirectionAblation" if profile == "smoke"
        else "Phase2DirectionAblationFull"
    )
    return (
        repo_root / "benchmarks" / root /
        str(row["name"]) / f"run_{int(row['rep'])}"
    )


def straight_heavy_direction_ablation_manifest(
    profile: str = "smoke",
) -> List[Dict[str, Any]]:
    """Return the headline 8S/4L/4R direction-ablation matrix.

    A repetition deterministically selects one checked-in fixture before the
    run starts.  All honest/attack and scheduling modes in that repetition use
    the same fixture and the same simulation seed.
    """
    if profile not in ("prerequisite", "smoke", "full"):
        raise ValueError(f"unknown straight-heavy direction profile: {profile}")
    repetitions = 1 if profile in ("prerequisite", "smoke") else DIRECTION_ABLATION_REPS
    attacks = (
        (("honest", 0, "NONE"),)
        if profile == "prerequisite" else
        (
            ("honest", 0, "NONE"),
            ("false_direction_b_f", F, "FALSE_DIRECTION"),
        )
    )
    modes = (
        (("eligibility_off", False, False),)
        if profile == "prerequisite" else
        (
            ("eligibility_on", True, False),
            ("eligibility_off", False, False),
            ("all_singleton", True, True),
        )
    )
    rows: List[Dict[str, Any]] = []
    for rep in range(repetitions):
        fixture_id = rep % STRAIGHT_HEAVY_DIRECTION_FIXTURE_COUNT
        for attack_name, b, attack_kind in attacks:
            for mode, eligibility, singleton in modes:
                rows.append({
                    "name": f"{attack_name}_{mode}",
                    "traffic_profile": "straight_heavy_8S_4L_4R",
                    "maneuver_mix": dict(STRAIGHT_HEAVY_MANEUVER_MIX),
                    "fixture_id": fixture_id,
                    "fixture_manifest": (
                        f"direction_ablation_straight_heavy_{fixture_id}_manifest.csv"
                    ),
                    "config": STRAIGHT_HEAVY_DIRECTION_CONFIGS[fixture_id],
                    "ablation_mode": mode,
                    "direction_eligibility_enabled": eligibility,
                    "all_singleton_scheduling": singleton,
                    "attack_kind": attack_kind,
                    "b": b,
                    "sigma_lat_m": COMBINED_SIGMA_LAT_M,
                    "sigma_long_m": COMBINED_SIGMA_LONG_M,
                    "signal_error": 0.2,
                    "delta_m": 0.0,
                    "k": COMBINED_K,
                    "rep": rep,
                    "profile": profile,
                    "purposes": [
                        f"direction_ablation_straight_heavy_{profile}"
                    ],
                })
    expected = {
        "prerequisite": 1,
        "smoke": 6,
        "full": 6 * DIRECTION_ABLATION_REPS,
    }[profile]
    if len(rows) != expected:
        raise AssertionError(
            f"straight-heavy direction {profile} matrix changed: "
            f"{len(rows)} != {expected}"
        )
    for rep in range(repetitions):
        rep_rows = [row for row in rows if int(row["rep"]) == rep]
        if len({(row["fixture_id"], row["config"]) for row in rep_rows}) != 1:
            raise AssertionError(f"direction fixture drift within repetition {rep}")
    return rows


def straight_heavy_direction_run_dir(
    repo_root: Path, row: Dict[str, Any], profile: str,
) -> Path:
    root = {
        "prerequisite": "Phase2DirectionAblationStraightHeavyPrerequisite",
        "smoke": "Phase2DirectionAblationStraightHeavy",
        "full": "Phase2DirectionAblationStraightHeavyFull",
    }[profile]
    return (
        repo_root / "benchmarks" / root /
        f"fixture_{int(row['fixture_id'])}" /
        str(row["name"]) / f"run_{int(row['rep'])}"
    )


def nested_colluders(b: int, target: int = TARGET, n: int = N) -> List[int]:
    if b <= 1:
        return []
    return [replica for replica in range(n) if replica != target][:b - 1]


def label(value: float) -> str:
    return format(value, "g").replace("-", "m").replace(".", "p")


def run_dir(repo_root: Path, row: Dict[str, Any]) -> Path:
    return (
        repo_root / "benchmarks" / "Phase2TwoLaneGrid" /
        f"b_{int(row['b'])}" /
        f"sigma_{label(float(row['sigma_lat_m']))}" /
        f"delta_{label(float(row['delta_m']))}" /
        f"k_{label(float(row['k']))}" /
        f"run_{int(row['rep'])}"
    )


def delta_b_run_dir(repo_root: Path, row: Dict[str, Any]) -> Path:
    return (
        repo_root / "benchmarks" / "Phase2TwoLaneDeltaBGrid" /
        f"b_{int(row['b'])}" /
        f"sigma_{label(float(row['sigma_lat_m']))}" /
        f"delta_{label(float(row['delta_m']))}" /
        f"k_{label(float(row['k']))}" /
        f"run_{int(row['rep'])}"
    )


def delta_b_completion_run_dir(repo_root: Path, row: Dict[str, Any]) -> Path:
    return (
        repo_root / "benchmarks" / "Phase2TwoLaneDeltaBCompletionGrid" /
        f"b_{int(row['b'])}" /
        f"sigma_{label(float(row['sigma_lat_m']))}" /
        f"delta_{label(float(row['delta_m']))}" /
        f"k_{label(float(row['k']))}" /
        f"run_{int(row['rep'])}"
    )


def parameter_sweep_run_dir(
    repo_root: Path, row: Dict[str, Any], sweep: str
) -> Path:
    is_anchor = "parameter_sweep_shared_anchor" in row.get("purposes", [])
    root = (
        repo_root / "benchmarks" / "Phase2TwoLaneParameterSweeps" / "shared_anchor"
        if is_anchor else
        repo_root / "benchmarks" /
        ("Phase2TwoLaneKSweep" if sweep == "k" else "Phase2TwoLaneSigmaSweep")
    )
    return (
        root / f"b_{int(row['b'])}" /
        f"sigma_{label(float(row['sigma_lat_m']))}" /
        f"delta_{label(float(row['delta_m']))}" /
        f"k_{label(float(row['k']))}" /
        f"run_{int(row['rep'])}"
    )


def honest_operating_run_dir(repo_root: Path, row: Dict[str, Any]) -> Path:
    return (
        repo_root / "benchmarks" / "Phase2TwoLaneHonestOperatingSweep" /
        f"sigma_{label(float(row['sigma_lat_m']))}" /
        f"k_{label(float(row['k']))}" /
        f"run_{int(row['rep'])}"
    )


def normal_cdf(value: float) -> float:
    return 0.5 * (1.0 + math.erf(value / math.sqrt(2.0)))


def q0_model(sigma_m: float, delta_m: float, k: float) -> float:
    if sigma_m == 0.0:
        return 1.0 if delta_m == 0.0 else 0.0
    ratio = delta_m / sigma_m
    return normal_cdf(ratio + k) - normal_cdf(ratio - k)


def q1_model(sigma_m: float, k: float) -> float:
    """True-claim acceptance for |N(0,sigma)| <= k*sigma."""
    if sigma_m == 0.0:
        return 1.0
    return 2.0 * normal_cdf(k) - 1.0


def wilson(successes: int, trials: int, z: float = 1.959963984540054) -> Tuple[float, float]:
    if trials <= 0:
        return (0.0, 1.0)
    p = successes / trials
    denom = 1.0 + z * z / trials
    center = (p + z * z / (2.0 * trials)) / denom
    radius = z * math.sqrt(p * (1.0 - p) / trials + z * z / (4.0 * trials * trials)) / denom
    return (max(0.0, center - radius), min(1.0, center + radius))


def _mean_present(rows: Sequence[Dict[str, Any]], field: str) -> float | None:
    values = [float(row[field]) for row in rows if row.get(field) is not None]
    return statistics.mean(values) if values else None


def _percentile(values: Sequence[float], quantile: float) -> float | None:
    if not values:
        return None
    ordered = sorted(float(value) for value in values)
    position = (len(ordered) - 1) * quantile
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    fraction = position - lower
    return ordered[lower] * (1.0 - fraction) + ordered[upper] * fraction


def aggregate(rows: Sequence[Dict[str, Any]]) -> List[Dict[str, Any]]:
    """Pool run outcomes by exact parameter cell with Wilson intervals."""
    grouped: Dict[Tuple[int, float, float, float], List[Dict[str, Any]]] = defaultdict(list)
    for row in rows:
        grouped[(
            int(row["b"]), float(row["sigma_lat_m"]),
            float(row["delta_m"]), float(row["k"]),
        )].append(row)

    output: List[Dict[str, Any]] = []
    for (b, sigma, delta, k), group in sorted(grouped.items()):
        trials = len(group)
        false_certs = sum(bool(row.get("false_lane_certificate")) for row in group)
        unsafe_runs = sum(bool(row.get("conflicting_cooccupancy")) for row in group)
        gate_induced = sum(bool(row.get("gate_induced_conflicting_cooccupancy")) for row in group)
        background_unsafe = sum(bool(row.get("background_conflicting_cooccupancy")) for row in group)
        low, high = wilson(false_certs, trials)
        unsafe_low, unsafe_high = wilson(unsafe_runs, trials)
        predictions = [
            float(row["binomial_tail_prediction"])
            for row in group
            if row.get("binomial_tail_prediction") is not None
        ]
        honest_accepts = sum(int(row.get("honest_lane_accepts", 0)) for row in group)
        honest_evaluations = sum(int(row.get("h_lane", 0)) for row in group)
        pooled_accept_rate = (
            honest_accepts / honest_evaluations if honest_evaluations else None
        )
        attempt_support = [
            int(row["b_sig_attempt"])
            for row in group if row.get("b_sig_attempt") is not None
        ]
        wait_samples = [
            float(value)
            for row in group for value in row.get("wait_samples_s", [])
        ]
        throughput_vps = _mean_present(group, "throughput_veh_per_s")
        throughput_vpm = _mean_present(group, "throughput_veh_per_min")
        wait_mean = statistics.mean(wait_samples) if wait_samples else None
        wait_p95 = _percentile(wait_samples, 0.95)
        quiet_percent = _mean_present(group, "quiet_percent")
        signed_unknown_percent = _mean_present(
            group, "signed_unknown_singleton_percent"
        )
        left_forced_percent = _mean_present(
            group, "left_table_forced_singleton_percent"
        )
        is_control = b == 0
        output.append({
            "b": b,
            "sigma_lat_m": sigma,
            "delta_m": delta,
            "effective_delta_m": 0.0 if is_control else delta,
            "claim_kind": "HONEST_CONTROL" if is_control else "FALSE_PHYSICAL_LANE",
            "k": k,
            "repetitions": trials,
            "false_certificate_applicable": not is_control,
            "false_lane_certificates": false_certs,
            "false_certificate_rate": false_certs / trials,
            "wilson95_low": low,
            "wilson95_high": high,
            "conflicting_cooccupancy_runs": unsafe_runs,
            "conflicting_cooccupancy_rate": unsafe_runs / trials,
            "conflicting_cooccupancy_wilson95_low": unsafe_low,
            "conflicting_cooccupancy_wilson95_high": unsafe_high,
            "gate_induced_conflicting_cooccupancy_runs": gate_induced,
            "background_conflicting_cooccupancy_runs": background_unsafe,
            "background_conflicting_cooccupancy_rate": background_unsafe / trials,
            "prediction_support_coverage": len(predictions) / trials,
            "mean_binomial_prediction": (
                sum(predictions) / len(predictions)
                if len(predictions) == trials else None
            ),
            "mean_binomial_prediction_available_runs": (
                sum(predictions) / len(predictions) if predictions else None
            ),
            "honest_lane_accepts_total": honest_accepts,
            "honest_lane_evaluations_total": honest_evaluations,
            "pooled_q0_empirical": None if is_control else pooled_accept_rate,
            "pooled_q1_empirical": pooled_accept_rate if is_control else None,
            "mean_honest_lane_accepts": honest_accepts / trials,
            "mean_h_lane": sum(float(row.get("h_lane", 0)) for row in group) / trials,
            "mean_b_sig": sum(float(row.get("b_sig_cert", row.get("b_sig", 0))) for row in group) / trials,
            "mean_b_sig_cert": sum(float(row.get("b_sig_cert", row.get("b_sig", 0))) for row in group) / trials,
            "mean_b_sig_attempt": (
                sum(attempt_support) / len(attempt_support) if attempt_support else None
            ),
            "b_sig_attempt_coverage": len(attempt_support) / trials,
            # Canonical plotting names plus compatibility aliases retained
            # for summaries produced before the completion sweeps.
            "throughput_veh_per_s": throughput_vps,
            "throughput_veh_per_min": throughput_vpm,
            "wait_mean_s": wait_mean,
            "wait_p95_s": wait_p95,
            "quiet_percent": quiet_percent,
            "singleton_quiet_percent": quiet_percent,
            "singleton_signed_unknown_percent": signed_unknown_percent,
            "singleton_left_turn_forced_percent": left_forced_percent,
            "mean_throughput_veh_per_s": throughput_vps,
            "mean_throughput_veh_per_min": throughput_vpm,
            "mean_wait_s": wait_mean,
            "p95_wait_s": wait_p95,
            "wait_sample_count": len(wait_samples),
            "mean_quiet_percent": quiet_percent,
            "mean_signed_unknown_singleton_percent": signed_unknown_percent,
            "mean_left_table_forced_singleton_percent": left_forced_percent,
            "mean_quiet_count": _mean_present(group, "quiet_count"),
            "mean_signed_unknown_singleton_count": _mean_present(
                group, "signed_unknown_singleton_count"
            ),
            "mean_left_table_forced_singleton_count": _mean_present(
                group, "left_table_forced_singleton_count"
            ),
            "mean_batch_size": _mean_present(group, "mean_batch_size"),
        })
    return output

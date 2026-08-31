#!/usr/bin/env python3
"""Exploratory analysis for E6 authenticated attack-defense demonstrations.

E6 is a multiseed *pass/fail* suite (attacks D,E,F,G,H,J,K × 5 seeds), not a
rate sweep. Primary rollup:

  experiments/e6_attack_defense/artifacts/curated/attack_defense_full_validation_summary.json

Per-run validation JSONs currently live under the compatibility root
benchmarks/Phase2AttackDefense/ (experiments/e6_attack_defense/results/ may be
empty). IEEE/ICRA styling belongs in a separate publication script.

Usage:
  python analyze_log.py
  python analyze_log.py --no-plots
"""

from __future__ import annotations

import argparse
import json
from collections import defaultdict
from pathlib import Path
from typing import Any


ATTACK_ORDER = ("D", "E", "F", "G", "H", "J", "K")
ATTACK_TITLE = {
    "D": "Equivocation",
    "E": "Physical-lane mutation",
    "F": "Unknown-direction upgrade",
    "G": "Certificate suppression",
    "H": "Silent primary",
    "J": "Cert-relay withholding",
    "K": "Consecutive Byz. primaries",
}

# Prefer the check that best names the defense for paper tables.
HEADLINE_CHECK = {
    "D": "second_variant_rejected_before_perception_or_echo",
    "E": "check10_rejected_mutated_physical_authority",
    "F": "check10_rejected_unknown_upgrade",
    "G": "check9_rejected_cert_omission",
    "H": "view_changed_to_nonbyzantine_successor",
    "J": "every_honest_replica_completed_with_full_certified_set",
    "K": "recovery_within_f_plus_1_primary_attempts",
}


def _repo_root() -> Path:
    here = Path(__file__).resolve()
    for parent in here.parents:
        if (parent / "experiments" / "e6_attack_defense").is_dir() and (
            parent / "benchmarks"
        ).is_dir():
            return parent
    raise FileNotFoundError("could not locate repo root")


def _load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def _find_headline_check(checks: dict[str, Any], attack: str) -> tuple[str, Any]:
    preferred = HEADLINE_CHECK.get(attack)
    if preferred and preferred in checks:
        return preferred, checks[preferred]
    # fall back: first failing check, else first True defense-like key
    for key, value in checks.items():
        if value is False:
            return key, value
    for key, value in checks.items():
        if value is True and any(
            tok in key for tok in ("reject", "recover", "view_change", "equivocation", "full_cert")
        ):
            return key, value
    if checks:
        key = next(iter(checks))
        return key, checks[key]
    return "—", None


def _mutation_or_framing(detail: dict[str, Any], attack: str) -> str:
    evidence = detail.get("evidence") or {}
    if evidence.get("mutation"):
        return str(evidence["mutation"]).split(":")[0]
    meta = detail.get("metadata") or {}
    if meta.get("mutation"):
        return str(meta["mutation"])
    if attack == "D":
        return "two reused signed direction variants"
    if attack == "J":
        return "f non-forwarders withhold cert relay"
    if attack == "K":
        return "r0 then r1 Byzantine primaries"
    return detail.get("name") or attack


def _print_header(rollup: dict[str, Any], results_dir: Path, bench_dir: Path) -> None:
    print("=" * 78)
    print("E6 Attack-Defense — exploratory analyze_log")
    print("Role: authenticated attack demonstrations (pass/fail), not a rate sweep")
    print("=" * 78)
    print(f"checkpoint         : {rollup.get('checkpoint')}")
    print(f"passed             : {rollup.get('passed')}")
    print(
        f"seed groups        : {rollup.get('completed_seed_groups')}/"
        f"{rollup.get('repetitions')}  "
        f"planned sims={rollup.get('planned_simulations')}"
    )
    print(f"execution          : {rollup.get('execution')}")
    print(f"e6 results dir     : {results_dir}  (entries may be empty)")
    print(f"compat data root   : {bench_dir}  exists={bench_dir.is_dir()}")
    notes = rollup.get("notes") or {}
    if notes:
        print("notes:")
        for key in sorted(notes):
            print(f"  {key}: {notes[key]}")
    print()


def _print_pass_matrix(rollup: dict[str, Any]) -> None:
    print("--- Pass matrix (attack × seed) ---")
    seeds = sorted(int(s["rep"]) for s in rollup.get("seed_results", []))
    header = f"{'atk':<4}" + "".join(f"{'s'+str(s):>5}" for s in seeds) + f"{'pass':>7}"
    print(header)
    print("-" * len(header))
    per = rollup.get("per_attack") or {}
    for attack in ATTACK_ORDER:
        cells = []
        for seed in rollup.get("seed_results", []):
            info = (seed.get("attacks") or {}).get(attack) or {}
            cells.append("  ✓" if info.get("passed") else "  ✗")
        agg = per.get(attack) or {}
        print(
            f"{attack:<4}"
            + "".join(cells)
            + f"  {agg.get('passed', '?')}/{agg.get('completed', '?')}"
        )
    print()


def _load_detail(path_str: str) -> dict[str, Any] | None:
    path = Path(path_str)
    if not path.is_file():
        return None
    return _load_json(path)


def _print_defense_table(rollup: dict[str, Any]) -> None:
    print("--- Defense headline per attack (rep 0 detail + all-seed pass) ---")
    print(
        f"{'atk':<4} {'title':<28} {'mutation / framing':<34} "
        f"{'headline check':<42} {'ok?':>4} {'5/5':>4}"
    )
    print("-" * 120)
    seed0 = next(
        (s for s in rollup.get("seed_results", []) if int(s.get("rep", -1)) == 0),
        None,
    )
    per = rollup.get("per_attack") or {}
    missing_details = 0
    for attack in ATTACK_ORDER:
        title = ATTACK_TITLE[attack]
        info0 = ((seed0 or {}).get("attacks") or {}).get(attack) or {}
        detail = _load_detail(info0.get("summary", ""))
        if detail is None:
            missing_details += 1
            mut, check, ok = "—", "—", "—"
        else:
            mut = _mutation_or_framing(detail, attack)[:34]
            check, value = _find_headline_check(detail.get("checks") or {}, attack)
            ok = "Y" if value is True else ("N" if value is False else "?")
            # If preferred G check missing, surface actual reject-like keys
            if attack == "G" and HEADLINE_CHECK["G"] not in (detail.get("checks") or {}):
                checks = detail.get("checks") or {}
                reject_keys = [k for k in checks if "reject" in k or "suppress" in k]
                if reject_keys:
                    check = reject_keys[0]
                    ok = "Y" if checks[check] is True else "N"
        agg = per.get(attack) or {}
        all_ok = "Y" if agg.get("passed") == agg.get("completed") == 5 else "N"
        print(
            f"{attack:<4} {title:<28} {mut:<34} {check:<42} {ok:>4} {all_ok:>4}"
        )
    if missing_details:
        print(f"\n  WARNING: {missing_details} attack detail JSONs missing on disk")
    print()


def _print_check_failures(rollup: dict[str, Any]) -> None:
    print("--- Failed checks across all seeds (should be empty if FULL_PASS) ---")
    failures = []
    for seed in rollup.get("seed_results", []):
        rep = seed.get("rep")
        for attack, info in (seed.get("attacks") or {}).items():
            detail = _load_detail(info.get("summary", ""))
            if detail is None:
                failures.append((attack, rep, "MISSING_DETAIL", None))
                continue
            if not detail.get("passed", info.get("passed")):
                failures.append((attack, rep, "PASSED_FALSE", None))
            for key, value in (detail.get("checks") or {}).items():
                if value is False:
                    failures.append((attack, rep, key, value))
    if not failures:
        print("  none")
    else:
        for attack, rep, key, value in failures:
            print(f"  {attack} rep={rep}: {key}={value}")
    print()


def _print_op_consistency(rollup: dict[str, Any]) -> None:
    print("--- Operating-point stamps in per-attack metadata ---")
    ops: dict[tuple, list[str]] = defaultdict(list)
    for seed in rollup.get("seed_results", []):
        for attack, info in (seed.get("attacks") or {}).items():
            detail = _load_detail(info.get("summary", ""))
            if not detail:
                continue
            op = (detail.get("metadata") or {}).get("operating_point") or {}
            key = (
                op.get("physical_gate_k"),
                op.get("sigma_lat_m"),
                op.get("sigma_long_m"),
                op.get("signal_error"),
            )
            ops[key].append(f"{attack}/r{seed.get('rep')}")
    for key, refs in ops.items():
        print(f"  k={key[0]} σ_lat={key[1]} σ_long={key[2]} signal_error={key[3]}  n={len(refs)}")
    print()


def _recommend() -> None:
    print("=" * 78)
    print("Paper role + candidate compositions")
    print("=" * 78)
    print(
        "Framing: E6 is a *demonstration / validation suite* — every attack is "
        "survived with an explicit rejecting check and honest recovery. It is not "
        "an FE-vs-parameter curve like E2/E5."
    )
    print(
        "1. TABLE (RECOMMENDED primary): one row per attack with columns "
        "Attack | Threat | Mutation | Rejecting check | Recovery | Seeds passed. "
        "This matches how the harness records evidence and reads cleanly in ICRA."
    )
    print(
        "2. Optional single-column pass-matrix heatmap (attack × seed): visual "
        "proof of 7×5 = 35/35. Weak alone; fine as a small inset next to the table."
    )
    print(
        "3. Do NOT invent continuous rate plots from this suite — there is no "
        "swept axis; n=5 seeds per attack is for multiseed confirmation."
    )
    print(
        "4. Relative to E2/E4/E5: E6 is supporting evidence that authenticated "
        "proposer/gossip faults are handled at the locked OP (k=2, σ_lat=0.5)."
    )
    print()
    print(
        "Recommendation: ship a compact table from the curated rollup + headline "
        "checks; keep the explore pass-matrix for slides if useful."
    )
    print()


def _save_plots(rollup: dict[str, Any], out_dir: Path) -> None:
    try:
        import matplotlib.pyplot as plt
        import numpy as np
    except ImportError:
        print("matplotlib not available; skipping exploratory PNGs.")
        return

    out_dir.mkdir(parents=True, exist_ok=True)
    seeds = [int(s["rep"]) for s in sorted(rollup.get("seed_results", []), key=lambda x: int(x["rep"]))]
    matrix = np.zeros((len(ATTACK_ORDER), len(seeds)))
    for i, attack in enumerate(ATTACK_ORDER):
        for j, seed in enumerate(sorted(rollup.get("seed_results", []), key=lambda x: int(x["rep"]))):
            info = (seed.get("attacks") or {}).get(attack) or {}
            matrix[i, j] = 1.0 if info.get("passed") else 0.0

    fig, ax = plt.subplots(figsize=(6.2, 3.8))
    im = ax.imshow(matrix, aspect="auto", cmap="Greens", vmin=0.0, vmax=1.0)
    ax.set_xticks(range(len(seeds)))
    ax.set_xticklabels([f"seed {s}" for s in seeds])
    ax.set_yticks(range(len(ATTACK_ORDER)))
    ax.set_yticklabels([f"{a}  {ATTACK_TITLE[a]}" for a in ATTACK_ORDER], fontsize=8)
    for i in range(matrix.shape[0]):
        for j in range(matrix.shape[1]):
            ax.text(
                j,
                i,
                "pass" if matrix[i, j] >= 0.5 else "FAIL",
                ha="center",
                va="center",
                fontsize=7,
                color="white" if matrix[i, j] >= 0.5 else "black",
            )
    ax.set_title("E6 exploratory: attack-defense pass matrix (35/35)")
    fig.colorbar(im, ax=ax, fraction=0.046, pad=0.04, label="passed")
    fig.tight_layout()
    path = out_dir / "explore_attack_pass_matrix.png"
    fig.savefig(path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"wrote {path}")

    # Bar of checks-passed fraction per attack on seed 0 (all should be 1.0)
    fig, ax = plt.subplots(figsize=(7.0, 3.6))
    xs, ys = [], []
    for attack in ATTACK_ORDER:
        seed0 = next(s for s in rollup["seed_results"] if int(s["rep"]) == 0)
        detail = _load_detail(seed0["attacks"][attack]["summary"])
        if not detail:
            xs.append(attack)
            ys.append(0.0)
            continue
        checks = detail.get("checks") or {}
        xs.append(attack)
        ys.append(sum(1 for v in checks.values() if v is True) / max(1, len(checks)))
    ax.bar(xs, ys, color="#0072B2", alpha=0.85)
    ax.set_ylim(0.0, 1.08)
    ax.set_ylabel("Fraction of checks True (seed 0)")
    ax.set_xlabel("Attack")
    ax.set_title("E6 exploratory: per-attack check completeness")
    ax.grid(True, axis="y", alpha=0.35, linestyle="--")
    for label, y in zip(xs, ys):
        ax.text(label, y + 0.02, f"{y:.2f}", ha="center", fontsize=8)
    fig.tight_layout()
    path = out_dir / "explore_check_fraction.png"
    fig.savefig(path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"wrote {path}")
    print()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--rollup",
        type=Path,
        default=Path(__file__).resolve().parent
        / "artifacts"
        / "curated"
        / "attack_defense_full_validation_summary.json",
    )
    parser.add_argument(
        "--results-dir",
        type=Path,
        default=Path(__file__).resolve().parent / "results",
    )
    parser.add_argument(
        "--plots-dir",
        type=Path,
        default=None,
    )
    parser.add_argument("--no-plots", action="store_true")
    args = parser.parse_args()

    rollup_path = args.rollup.resolve()
    if not rollup_path.is_file():
        raise SystemExit(f"missing rollup: {rollup_path}")
    rollup = _load_json(rollup_path)
    results_dir = args.results_dir.resolve()
    bench_dir = _repo_root() / "benchmarks" / "Phase2AttackDefense"

    _print_header(rollup, results_dir, bench_dir)
    _print_pass_matrix(rollup)
    _print_defense_table(rollup)
    _print_check_failures(rollup)
    _print_op_consistency(rollup)
    _recommend()

    if not args.no_plots:
        plots_dir = (
            args.plots_dir or (results_dir / "explore_plots")
        ).resolve()
        _save_plots(rollup, plots_dir)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Analyze adjacent-lane calibration results and print the gate ROC table."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any, Dict, Sequence

from run_calibration import (
    DEFAULT_MANIFEST,
    DEFAULT_RESULTS,
    HERE,
    analytic_accept_prob,
    load_manifest,
)

DEFAULT_SUMMARY = HERE / "canonical_validation_summary.json"


def load_summary(path: Path) -> Dict[str, Any]:
    return json.loads(path.read_text())


def format_rate(x: float | None) -> str:
    if x is None:
        return "   n/a  "
    return f"{x:8.5f}"


def print_roc(summary: Dict[str, Any]) -> None:
    k_values = summary["k_values"]
    print(
        f"Adjacent-lane gate ROC  σ={summary['sigma_m']} m\n"
        f"  quantized empirical / ideal 1D Gaussian interval at kσ; "
        f"Y/N = Bonferroni gate validation pass (36-cell family)"
    )
    print(
        f"{'delta':>7} {'regime':>10} {'claim':>10} "
        + " ".join(f"{'k='+str(k):>22}" for k in k_values)
    )
    for row in summary["deltas"]:
        cells = []
        for k in k_values:
            key = f"{k:g}"
            emp = row["pooled"][key]
            ana = row["analytic_continuous"][key]["p_accept"]
            ok = "Y" if row["gate_validation"][key]["pass"] else "N"
            cells.append(
                f"{format_rate(emp['rate'])}/{format_rate(ana)} {ok}"
            )
        print(
            f"{row['delta_m']:7.2f} {row['regime']:>10} {row['example_claimed_lane']:>10} "
            + " ".join(f"{c:>22}" for c in cells)
        )
    pc = summary["pass_criteria"]
    print()
    print("Pass criteria:")
    for key, value in pc.items():
        if key == "false_lane_nontrivial_roc_points":
            print(f"  {key}: {len(value)} points")
        else:
            print(f"  {key}: {value}")


def verify_analytic_helpers(manifest: Dict[str, Any]) -> None:
    from scipy.stats import norm

    sigma = float(manifest["roc_grid"]["sigma_m"])
    for k in manifest["sensing"]["k_values"]:
        q1 = analytic_accept_prob(sigma, 0.0, float(k))
        closed = 2.0 * norm.cdf(float(k)) - 1.0
        if abs(q1 - closed) > 1e-12:
            raise SystemExit(f"q1 closed form mismatch at k={k}: {q1} vs {closed}")


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--summary", type=Path, default=DEFAULT_SUMMARY)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument(
        "--results-dir",
        type=Path,
        default=DEFAULT_RESULTS,
        help="Unused except to document where bulk .npy files live.",
    )
    args = parser.parse_args(argv)

    manifest = load_manifest(args.manifest)
    verify_analytic_helpers(manifest)

    if not args.summary.is_file():
        raise SystemExit(
            f"Missing {args.summary}. Run run_calibration.py first."
        )
    summary = load_summary(args.summary)
    print_roc(summary)
    return 0 if summary["pass_criteria"]["checkpoint_pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

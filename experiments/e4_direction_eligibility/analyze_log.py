#!/usr/bin/env python3
"""Exploratory analysis for E4 direction-eligibility ablation (FP / FN proxy).

Loads the curated direction-ablation aggregates and prints per-mode breakdowns
for false-eligibility (FP) and signed-unknown singleton share (FN proxy).

IEEE/ICRA sizing belongs in fourway/plot_icra_camera_ready.py — not here.

Usage:
  python analyze_log.py
"""

from __future__ import annotations

import csv
import json
from pathlib import Path


HERE = Path(__file__).resolve().parent
AGG = HERE / "artifacts" / "curated" / "direction_ablation_straight_heavy_full_aggregates.csv"
SUMMARY = HERE / "artifacts" / "curated" / "direction_ablation_straight_heavy_full_summary.json"


def main() -> int:
    if not AGG.is_file():
        raise SystemExit(f"Missing aggregates: {AGG}")

    rows = list(csv.DictReader(AGG.open(newline="", encoding="utf-8")))
    attacked = [r for r in rows if r.get("attack_kind") == "FALSE_DIRECTION"]

    print("E4 direction ablation — FALSE_DIRECTION (b=5)\n")
    print(f"{'mode':<18} {'FP elig.':>10} {'FN proxy':>10} {'unsafe':>8} {'thruput':>10}")
    for row in attacked:
        mode = row["ablation_mode"]
        fp = float(row["false_eligibility_rate"])
        fn = float(row["mean_signed_unknown_singleton_percent"]) / 100.0
        unsafe = float(row["unsafe_cooccupancy_rate"])
        thr = float(row["mean_throughput_veh_per_min"])
        print(f"{mode:<18} {fp:10.3f} {fn:10.3f} {unsafe:8.3f} {thr:10.2f}")

    print("\nRecommendation: grouped FP/FN bars per ablation mode (attack runs only);")
    print("keep unsafe / throughput / wait as companion panels.")

    if SUMMARY.is_file():
        meta = json.loads(SUMMARY.read_text())
        print(f"\nSummary keys: {', '.join(meta.keys())}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

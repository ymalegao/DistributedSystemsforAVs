#!/usr/bin/env python3
"""One-shot retest of E7 forged-BLOCKED and fabricated-CLEAR integrity rows."""
from __future__ import annotations

import json
import shlex
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import experiment_orchestrator as eo  # noqa: E402


def main() -> int:
    root = eo.E7_RESULT_ROOT / "smoke"
    rows = [
        r
        for r in eo._e7_rows()
        if r["name"]
        in ("integrity_forged_blocked", "integrity_fabricated_clear")
    ]
    if len(rows) != 2:
        raise SystemExit(f"expected 2 integrity rows, got {[r['name'] for r in rows]}")

    eo.run_key_generation(dry_run=False, scale=16)
    results = []
    for i, row in enumerate(rows, 1):
        rep = 0
        seed = eo.run_seed(eo.MASTER_SEED, 16, "E7_ROLLBACK_PAIRED", 0)
        run_dir = root / row["name"] / f"run_{rep}"
        print(f"\n--- E7 integrity {i}/2 {row['name']} seed={seed} ---", flush=True)
        command = [
            str(eo.RUN_SCRIPT),
            "--randomize",
            "16",
            "0",
            "--no-ambulance",
            "--tolerated-f",
            "5",
            str(eo.FOURWAY_DIR),
            "--compact-log",
            "--crash-wait-clear",
            "--crash-wreck-count",
            "1",
            "--crash-dwell",
            str(row["blocked_dwell_sec"]),
            "--approach-sigma",
            "0",
            "--signal-error",
            "0.2",
            "--direction-collection-window",
            "0.25",
            "--ego-longitudinal-sigma",
            "0",
            "--longitudinal-sigma",
            "1.0",
            "--lane-observation-mode",
            "ADJACENT_LATERAL",
            "--lateral-sigma",
            "0.5",
            "--adjacent-lane-separation",
            "3.2",
            "--physical-gate-k",
            "2",
            "--direction-eligibility",
            "on",
            "--simulation-seed",
            str(seed),
            "--**.scalar-recording=false",
            "--**.vector-recording=false",
            "-u",
            "Cmdenv",
            "--debug-on-errors=false",
            "-c",
            "SixteenVehiclesTwoLaneRollbackResDB",
        ] + row.get("extra", [])
        eo.run_in_bash_with_omnet(
            " ".join(shlex.quote(v) for v in command), dry_run=False
        )
        run_dir.mkdir(parents=True, exist_ok=True)
        shutil.copy2(eo.LOG_FILE, run_dir / "raw_simulation.log")
        (run_dir / "run_metadata.json").write_text(
            json.dumps(
                {"profile": "smoke", "row": row, "rep": rep, "seed": seed},
                indent=2,
                sort_keys=True,
            )
            + "\n"
        )
        result = eo._parse_e7_run(row, run_dir, rep)
        results.append(result)
        print(
            f"[{'PASS' if result['passed'] else 'FAIL'}] {row['name']} "
            f"checks={json.dumps(result['checks'], sort_keys=True)}",
            flush=True,
        )

    overall = all(r["passed"] for r in results)
    print("\n========== INTEGRITY RETEST ==========")
    print(f"Overall: {'PASS' if overall else 'FAIL'}")
    for r in results:
        fails = [k for k, v in r["checks"].items() if not v]
        print(
            f"  {r['name']}: {'PASS' if r['passed'] else 'FAIL'} "
            f"departed={r['departed_count']} fails={fails}"
        )
    return 0 if overall else 1


if __name__ == "__main__":
    raise SystemExit(main())

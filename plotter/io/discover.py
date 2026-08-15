"""Finding ablation run logs on disk and grouping them into cells."""
from collections import defaultdict
from pathlib import Path
from typing import Dict, List

from .logparse import parse_log, parse_run_name
from .schema import RunRecord

Runs = Dict[tuple, List[RunRecord]]


def load_runs(results_dir, study: int | None = None) -> Runs:
    """Parse run logs under results_dir, grouped by (study, arm, k).

    Returns {} when the directory is missing or holds no run logs; callers
    report that rather than raising, because "the matrix has not been run yet"
    is the normal reason and not an error.
    """
    results_dir = Path(results_dir)
    if not results_dir.is_dir():
        return {}

    runs: Runs = defaultdict(list)
    for path in sorted(results_dir.glob("*.log")):
        key = parse_run_name(path)
        if key is None or (study is not None and key.study != study):
            continue
        runs[key.cell()].append(parse_log(path, key))
    return dict(runs)


def arms(runs: Runs) -> List[str]:
    """Distinct arms present, in sorted order."""
    return sorted({cell[1] for cell in runs})


def ks(runs: Runs, arm: str | None = None) -> List[int]:
    """Sorted k values present, optionally restricted to one arm."""
    return sorted({
        cell[2] for cell in runs
        if cell[2] is not None and (arm is None or cell[1] == arm)
    })


def cell(runs: Runs, study: int, arm: str, k: int | None = None) -> List[RunRecord]:
    """The repetitions for one cell; empty when that cell was never run."""
    return runs.get((study, arm, k), [])

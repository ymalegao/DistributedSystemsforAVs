"""Finding run logs on disk and grouping them into matrix cells."""
from collections import defaultdict
from pathlib import Path
from typing import Dict, List

from .logparse import parse_log, parse_run_name
from .schema import RunRecord


def load_runs(results_dir) -> Dict[tuple, List[RunRecord]]:
    """Parse every run log under results_dir, grouped by (arm, v, k).

    Returns an empty dict when the directory is missing or holds no run logs;
    callers report that to the user rather than raising, because "you have not
    run the matrix yet" is the normal reason and not an error.
    """
    results_dir = Path(results_dir)
    if not results_dir.is_dir():
        return {}

    runs: Dict[tuple, List[RunRecord]] = defaultdict(list)
    for path in sorted(results_dir.glob("*.log")):
        key = parse_run_name(path)
        if key is None:
            continue
        runs[key.cell()].append(parse_log(path, key))
    return dict(runs)


def ks(runs: Dict[tuple, List[RunRecord]], arm=None, v=None) -> List[int]:
    """Sorted k values present, optionally restricted to one arm / vehicle count."""
    return sorted({
        cell[2] for cell in runs
        if (arm is None or cell[0] == arm) and (v is None or cell[1] == v)
    })


def vehicle_counts(runs: Dict[tuple, List[RunRecord]]) -> List[int]:
    """Sorted vehicle counts present (scaling study only)."""
    return sorted({cell[1] for cell in runs if cell[1] is not None})

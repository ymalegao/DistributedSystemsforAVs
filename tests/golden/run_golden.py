#!/usr/bin/env python3
"""Capture or check the structural invariants of the simulator.

    python3 tests/golden/run_golden.py capture      # record the baseline
    python3 tests/golden/run_golden.py check        # re-run and diff against it

Run `capture` on known-good code, refactor, then `check`. A clean check means
every structural invariant survived; see invariants.py for why timings are
deliberately excluded.

Must be run inside the opp_env shell (the simulator needs OMNETPP_ROOT).
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from tests.golden.invariants import Cell, compare, parse  # noqa: E402

HERE = Path(__file__).resolve().parent
BASELINE = HERE / "baseline.json"
LOG_DIR = HERE / "logs"
RUN_SCRIPT = REPO_ROOT / "fourway" / "run-resdb-simulation.sh"
SIM_LOG = Path("/tmp/resdb-simulation.log")

# The matrix. Each cell is (name, config, extra flags).
#
# Chosen to exercise the paths a decomposition can plausibly break, not just
# the happy path: an honest baseline, a Byzantine primary that must trigger a
# view change, a firewall-disabled arm (the f+1 pre-verification path), and the
# rollback scenario with and without units, which is where quorum sizing and
# the cancel/rollback state machine actually run.
# NOTE: --byzleader and --leader-byz-type each take a VALUE (replica id, and
# the ByzantineType from protocol/Primitives.h respectively). Passing
# --byzleader as a bare flag silently swallows the next argument and leaves a
# stray positional that OMNeT++ tries to open as an ini file.
CELLS = [
    ("honest_4veh",        "FourVehiclesResDB",           []),
    ("units_4veh",         "FourVehiclesFourUnitsResDB",  []),
    # type 4 = BYZANTINE_SILENT_PRIMARY: primary suppresses proposeAll(),
    # which must drive a view change rather than a stall.
    ("byz_silent_primary", "FourVehiclesResDB",           ["--byzleader", "0", "--leader-byz-type", "4"]),
    # type 6 = BYZANTINE_FAKE_AMBULANCE: exercises PreVerify Check 10.
    ("byz_fake_ambulance", "FourVehiclesResDB",           ["--byzleader", "0", "--leader-byz-type", "6"]),
    ("no_firewall_4veh",   "FourVehiclesResDB",           ["--no-firewall"]),
    ("rollback_units",     "EighteenVehFourUnitsRollbackFast", []),
    ("rollback_no_units",  "EighteenVehRollbackNoUnitsFast",   []),
]

DEFAULT_REPS = 3


# A run that never started must never be recorded as a run that failed to
# commit -- that silently bakes a broken cell into the baseline and makes any
# later comparison meaningless. These markers mean "the simulator refused the
# arguments", which is a harness bug, not protocol behaviour.
STARTUP_FAILURES = (
    "Cannot open ini file",
    "Unknown configuration",
    "Error: Cannot",
)


def run_one(config, flags, log_path) -> bool:
    """Run one simulation, copying its log aside.

    Returns False if the simulation never actually started, so the caller can
    abort rather than record a meaningless cell.
    """
    cmd = ["bash", str(RUN_SCRIPT), "-u", "Cmdenv", "-c", config, *flags]
    subprocess.run(cmd, cwd=RUN_SCRIPT.parent,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if not SIM_LOG.exists():
        return False
    log_path.write_bytes(SIM_LOG.read_bytes())

    text = log_path.read_text(errors="ignore")
    for marker in STARTUP_FAILURES:
        if marker in text:
            print(f"STARTUP FAILURE ({marker!r})")
            return False
    # A nonzero exit is not automatically a failure to record: the known TraCI
    # departed-vehicle crash fires after the commit. The invariants decide.
    return True


def collect(reps: int, only=None) -> dict:
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    signatures = {}
    for name, config, flags in CELLS:
        if only and name not in only:
            continue
        cell = Cell(name)
        for rep in range(1, reps + 1):
            log_path = LOG_DIR / f"{name}_rep{rep}.log"
            print(f"  {name} rep{rep} ... ", end="", flush=True)
            if not run_one(config, flags, log_path):
                # Loud, not silent: a cell we could not actually run must not
                # end up in the baseline looking like a legitimate result.
                raise SystemExit(
                    f"\nABORT: cell {name!r} did not start. Check its flags in CELLS "
                    f"and see {log_path}")
            inv = parse(log_path)
            cell.runs.append(inv)
            print("commit" if inv.committed else "NO COMMIT",
                  f"(replicas={len(inv.committing_replicas)}, batches={inv.n_batches})")
        signatures[name] = cell.signature()
    return signatures


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["capture", "check"])
    ap.add_argument("--reps", type=int, default=DEFAULT_REPS)
    ap.add_argument("--only", nargs="*", help="restrict to named cells")
    args = ap.parse_args()

    print(f"{args.mode}: {args.reps} reps per cell")
    signatures = collect(args.reps, args.only)

    if args.mode == "capture":
        BASELINE.write_text(json.dumps(signatures, indent=2, sort_keys=True) + "\n")
        print(f"\nBaseline written: {BASELINE}")
        return 0

    if not BASELINE.exists():
        print(f"No baseline at {BASELINE} — run 'capture' on known-good code first.")
        return 2

    diffs = compare(json.loads(BASELINE.read_text()), signatures)
    if not diffs:
        print("\nPASS — every structural invariant preserved.")
        return 0
    print(f"\nFAIL — {len(diffs)} invariant(s) changed:")
    for d in diffs:
        print(f"  {d}")
    return 1


if __name__ == "__main__":
    sys.exit(main())

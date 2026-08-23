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
import os
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from tests.golden.invariants import Cell, compare, parse  # noqa: E402

HERE = Path(__file__).resolve().parent
BASELINE = HERE / "baseline.json"
LOG_DIR = HERE / "logs"
RUN_SCRIPT = REPO_ROOT / "tools" / "run-resdb-simulation.sh"
SCENARIO_DIR = REPO_ROOT / "scenarios" / "fourway"

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
    """Run one simulation, writing its log straight to log_path.

    LOG_FILE is set per run rather than letting the script use its shared
    /tmp/resdb-simulation.log default: that default is a single mutable path,
    so any two runs in flight at once interleave into each other's log. It
    happened -- a manual run overlapped a harness run and produced a log with
    two 'Calling finish()' markers and doubled metrics, which the parser would
    happily have summarised as a legitimate result.

    Returns False if the run did not produce exactly one clean simulation, so
    the caller can abort rather than bake a bad cell into the baseline.
    """
    # Delete first. The run script tee -a APPENDS to LOG_FILE, so without this a
    # run that never starts (no veins_launchd, unbuilt binary) leaves the
    # previous run's log in place, log_path.exists() stays true, and the parser
    # happily re-reads stale results as a fresh pass. That turns "check" into a
    # no-op that reports PASS against whatever was on disk.
    log_path.unlink(missing_ok=True)

    env = {**os.environ, "LOG_FILE": str(log_path)}
    cmd = ["bash", str(RUN_SCRIPT), "-u", "Cmdenv", "-c", config, *flags]
    subprocess.run(cmd, cwd=SCENARIO_DIR, env=env,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if not log_path.exists():
        print("NO LOG PRODUCED")
        return False

    text = log_path.read_text(errors="ignore")
    for marker in STARTUP_FAILURES:
        if marker in text:
            print(f"STARTUP FAILURE ({marker!r})")
            return False

    # Exactly one finish(): zero means the run was killed before completing,
    # more than one means two runs interleaved into this file. Both look like
    # ordinary results to the parser, so they must be rejected here.
    finishes = text.count("Calling finish()")
    if finishes != 1:
        print(f"BAD LOG ({finishes} finish() markers, expected 1)")
        return False

    # A nonzero exit is not automatically a failure to record: the known TraCI
    # departed-vehicle segfault fires after the commit. The invariants decide.
    return True


def produce_logs(reps: int, only=None) -> None:
    """Run the simulations, writing one log per repetition.

    Kept separate from summarising so a partial capture is never wasted: the
    18-vehicle rollback cells take ~7 minutes per run, so a crash or a timeout
    part-way through must not cost the cells that already succeeded.
    """
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    for name, config, flags in CELLS:
        if only and name not in only:
            continue
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
            print("commit" if inv.committed else "NO COMMIT",
                  f"(replicas={len(inv.committing_replicas)}, batches={inv.n_batches})")


def summarize(only=None) -> dict:
    """Build signatures from whatever logs are on disk.

    Reading from logs rather than from the runs means the invariant set can be
    changed and the baseline rebuilt without re-running a ~55 minute matrix.
    """
    cells = {}
    for log_path in sorted(LOG_DIR.glob("*_rep*.log")):
        name = log_path.name.rsplit("_rep", 1)[0]
        if only and name not in only:
            continue
        # Same validity gate as run_one: a killed or interleaved log must not
        # reach the baseline just because it happens to be sitting on disk.
        finishes = log_path.read_text(errors="ignore").count("Calling finish()")
        if finishes != 1:
            print(f"  SKIP {log_path.name}: {finishes} finish() markers, expected 1")
            continue
        cells.setdefault(name, Cell(name)).runs.append(parse(log_path))
    return {name: cell.signature() for name, cell in sorted(cells.items())}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["capture", "check"])
    ap.add_argument("--reps", type=int, default=DEFAULT_REPS)
    ap.add_argument("--only", nargs="*", help="restrict to named cells")
    ap.add_argument("--from-logs", action="store_true",
                    help="skip running; rebuild from the logs already on disk")
    args = ap.parse_args()

    if args.from_logs:
        print("summarising existing logs (no simulations run)")
    else:
        print(f"{args.mode}: {args.reps} reps per cell")
        produce_logs(args.reps, args.only)

    # Always summarise every cell present on disk, so an incremental capture
    # (a few slow cells at a time) accumulates into one complete baseline.
    signatures = summarize()

    if args.mode == "capture":
        BASELINE.write_text(json.dumps(signatures, indent=2, sort_keys=True) + "\n")
        print(f"\nBaseline written: {BASELINE} ({len(signatures)} cells)")
        for name, sig in signatures.items():
            print(f"  {name:<22} reps={sig['reps']} commit_rate={sig['commit_rate']}")
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

#!/usr/bin/env python3
"""
Multi-scale ResDB/V2V experiment driver.

Regenerates ResDB keys once per N value, then runs
fourway/run-resdb-simulation.sh for each scenario × repetition, then analyze_log.py.
No build step — compile veins separately before running.

Layout: benchmarks/Priority<N>cars/<scenario_subdir>/run_<rep>/

Environment: use a shell where OMNeT++ is on PATH, or let this script source
$OMNETPP_ROOT/setenv (override with OMNETPP_SETENV). We do not call opp_env shell
(nested opp_env breaks; some opp_env builds lack shell -c). If your shell already
sourced OMNeT++, set ORCHESTRATOR_SKIP_OMNET_SOURCE=1 to skip re-sourcing.
"""

from __future__ import annotations

import argparse
import os
import random
import re
import shlex
import subprocess
import sys
from pathlib import Path
from typing import List, Sequence, Tuple

# --- Fixed in-repo (not CLI): reproducible draws for bash $RANDOM in run-resdb-simulation.sh ---
MASTER_SEED = 0xC0FFEE42

REPO_ROOT = Path(__file__).resolve().parent


def _resolve_omnet_setenv() -> Path:
    if os.environ.get("OMNETPP_SETENV"):
        return Path(os.environ["OMNETPP_SETENV"]).expanduser()
    root = os.environ.get("OMNETPP_ROOT")
    if root:
        candidate = Path(root).expanduser() / "setenv"
        if candidate.is_file():
            return candidate
    return Path("/omnet/omnetpp-6.2.0/setenv")


OMNET_SETENV = _resolve_omnet_setenv()
VEINS_DIR = REPO_ROOT / "veins-veins-5.3.1"
FOURWAY_DIR = REPO_ROOT / "fourway"
CONFIG_DIR = FOURWAY_DIR / "resdb_crypto"
RUN_SCRIPT = REPO_ROOT / "fourway" / "run-resdb-simulation.sh"
V2V_PROXY_H = VEINS_DIR / "src/veins/modules/application/resDB/ResDBV2VProxyModule.h"
LOG_FILE = Path("/tmp/resdb-simulation.log")

HOST_LINE = re.compile(r"^(\s*#?\s*)(\d+)(\s+127\.0\.0\.1\s+\d+\s+\d+)\s*$")

# Matches analyze_log.SCENARIO_SUBDIR values (for output paths)
SCENARIO_SUBDIR = {
    "ByzLeader_Ambulance": "amb_byz_leader",
    "ByzFollower_Ambulance": "amb_byz_follower",
    "ByzFollower_NoAmbulance": "no_amb_byz_follower",
    "Honest_Ambulance": "amb_honest",
    "No_Ambulance_Honest": "no_amb",
    "ByzLeader_NoAmbulance": "no_amb_byz_leader",
}

# analyze_log.py --scenario codes (also used by --scenario CLI flag)
ANALYZE_SCENARIO = {
    "ByzLeader_Ambulance": 4,
    "ByzFollower_Ambulance": 3,
    "ByzFollower_NoAmbulance": 6,
    "Honest_Ambulance": 2,
    "No_Ambulance_Honest": 1,
    "ByzLeader_NoAmbulance": 5,
}
SCENARIO_BY_CODE = {
    1: "No_Ambulance_Honest",
    2: "Honest_Ambulance",
    3: "ByzFollower_Ambulance",
    4: "ByzLeader_Ambulance",
    5: "ByzLeader_NoAmbulance",
    6: "ByzFollower_NoAmbulance",
}

DEFAULT_N_VALUES = (4, 8, 12, 16)
REPETITIONS = 5

SCENARIO_ORDER: Tuple[str, ...] = (
    "No_Ambulance_Honest",
    "ByzFollower_NoAmbulance",
    "ByzLeader_NoAmbulance",
    "Honest_Ambulance",
    "ByzFollower_Ambulance",
    "ByzLeader_Ambulance",
)


def bft_f(n: int) -> int:
    return (n - 1) // 3


def omnet_config_basename(n: int) -> str:
    names = {4: "Four", 8: "Eight", 12: "Twelve", 16: "Sixteen"}
    if n not in names:
        raise ValueError(f"Unsupported N={n}; expected one of {tuple(names)}")
    return names[n]


def omnet_config_name(n: int) -> str:
    return f"{omnet_config_basename(n)}VehiclesResDB"


def run_seed(master: int, n: int, scenario_name: str, rep: int) -> int:
    h = master & 0x7FFFFFFF
    for c in scenario_name:
        h = (h * 31 + ord(c)) & 0x7FFFFFFF
    return (h + n * 10007 + rep * 100003) & 0x7FFFFFFF


# def patch_hosts_config(path: Path, n: int) -> None:
#     lines = path.read_text().splitlines()
#     out: List[str] = []
#     for line in lines:
#         m = HOST_LINE.match(line.strip())
#         if not m:
#             out.append(line)
#             continue
#         rid = int(m.group(2))
#         rest = m.group(3)
#         if rid < n:
#             out.append(f"{rid}{rest}")
#         else:
#             out.append(f"#{rid}{rest}")
#     path.write_text("\n".join(out) + "\n")


# def patch_system_config(path: Path, n: int, *, clear_malicious: bool = True) -> None:
#     text = path.read_text()
#     f_val = bft_f(n)
#     text = re.sub(r"^system\.servers\.num\s*=.*$", f"system.servers.num = {n}", text, flags=re.M)
#     text = re.sub(r"^system\.servers\.f\s*=.*$", f"system.servers.f = {f_val}", text, flags=re.M)
#     view = ",".join(str(i) for i in range(n))
#     text = re.sub(r"^system\.initial\.view\s*=.*$", f"system.initial.view = {view}", text, flags=re.M)
#     if clear_malicious:
#         text = re.sub(
#             r"^system\.byzantine\.maliciousReplicaIds\s*=.*$",
#             "system.byzantine.maliciousReplicaIds = ",
#             text,
#             flags=re.M,
#         )
#     path.write_text(text)


# def patch_batch_sizes(n: int) -> None:
#     h = V2V_PROXY_H.read_text()
#     h, c = re.subn(
#         r"static const int BATCH_SIZE = \d+;",
#         f"static const int BATCH_SIZE = {n};",
#         h,
#         count=1,
#     )
#     if c != 1:
#         raise RuntimeError(f"Could not patch BATCH_SIZE in {V2V_PROXY_H}")
#     V2V_PROXY_H.write_text(h)

#     j = SERVER_RUNNER_JAVA.read_text()
#     j, c2 = re.subn(
#         r"private static final int BATCH_SIZE = \d+;",
#         f"private static final int BATCH_SIZE = {n};",
#         j,
#         count=1,
#     )
#     if c2 != 1:
#         raise RuntimeError(f"Could not patch BATCH_SIZE in {SERVER_RUNNER_JAVA}")
#     SERVER_RUNNER_JAVA.write_text(j)


def clear_stale_random_ini() -> None:
    """Remove random_scenario.ini before honest/no-ambulance scenarios so the previous run's overrides don't bleed in."""
    rnd = FOURWAY_DIR / "random_scenario.ini"
    if rnd.is_file():
        rnd.unlink()


def reset_resdb_keys(n: int) -> None:
    """
    Delete the cert and key files that gen_resdb_keys.sh will regenerate for N replicas,
    so a fresh keyset is produced on the next run_key_generation() call.
    Leaves all other files (scripts, server.config, admin keys) untouched.
    """
    for i in range(1, n + 1):
        for suffix in (".cert",):
            f = CONFIG_DIR / f"cert_{i}{suffix}"
            if f.is_file():
                f.unlink()
        for suffix in (".key.pri", ".key.pub"):
            f = CONFIG_DIR / f"node{i}{suffix}"
            if f.is_file():
                f.unlink()




def _skip_omnet_source() -> bool:
    """Set ORCHESTRATOR_SKIP_OMNET_SOURCE=1 when OMNeT++ is already on PATH (e.g. parent shell ran setenv)."""
    return os.environ.get("ORCHESTRATOR_SKIP_OMNET_SOURCE", "").strip().lower() in (
        "1",
        "true",
        "yes",
    )


def _omnet_bash_prefix() -> str:
    if _skip_omnet_source():
        return ""
    if OMNET_SETENV.is_file():
        return f"source {shlex.quote(str(OMNET_SETENV))} && "
    return ""


def run_in_bash_with_omnet(command: str, *, dry_run: bool) -> None:
    """
    Run `command` in bash. Prepends `source <setenv>` unless skipped — does NOT use `opp_env shell`,
    so it works when the parent is already an opp_env shell and with opp_env versions that lack `shell -c`.
    """
    prefix = _omnet_bash_prefix()
    if not prefix and not dry_run and not OMNET_SETENV.is_file() and not os.environ.get("OMNETPP_ROOT"):
        raise FileNotFoundError(
            f"OMNeT++ setenv not found: {OMNET_SETENV}. "
            "Set OMNETPP_ROOT or OMNETPP_SETENV, or export ORCHESTRATOR_SKIP_OMNET_SOURCE=1 "
            "if your shell already sourced OMNeT++."
        )
    inner = f"{prefix}{command}"
    print(f"+ bash -lc {shlex.quote(inner)}")
    if dry_run:
        return
    subprocess.run(["bash", "-lc", inner], cwd=REPO_ROOT, check=True)



def run_key_generation(*, dry_run: bool, scale: int) -> None:
    if not dry_run:
        reset_resdb_keys(scale)
    else:
        print(f"[dry-run] would reset_resdb_keys({scale}) (delete cert_1..cert_{scale}, node1..node{scale} key files)")
    run_in_bash_with_omnet(f"cd {shlex.quote(str(CONFIG_DIR))} && ./gen_resdb_keys.sh {scale}", dry_run=dry_run)
# def build_veins_and_bft(*, dry_run: bool) -> None:
#     run_in_bash_with_omnet(f"cd {shlex.quote(str(VEINS_DIR))} && make", dry_run=dry_run)
#     run_in_bash_with_omnet(f"cd {shlex.quote(str(BFT_LIBRARY))} && ./gradlew installDist", dry_run=dry_run)


# def apply_scale(n: int) -> None:
#     patch_hosts_config(CONFIG_DIR / "hosts.config", n)
#     patch_system_config(CONFIG_DIR / "system.config", n, clear_malicious=True)
#     patch_batch_sizes(n)


def randomize_args_for_scenario(n: int, scenario_name: str) -> List[str]:
    """
    Returns the extra flags for run-resdb-simulation.sh.

    No_Ambulance_Honest skips --randomize entirely so the script does NOT regenerate
    random_scenario.ini (which would otherwise force *.node[*].appl.ambulanceReplicaId
    even when F=0 and turn one car into an ambulance).
    """
    f = bft_f(n)
    if scenario_name == "ByzLeader_Ambulance":
        return ["--randomize", str(n), str(f - 1), "--byzleader", "0"]
    if scenario_name == "ByzFollower_Ambulance":
        return ["--randomize", str(n), str(f)]
    if scenario_name == "ByzFollower_NoAmbulance":
        return ["--randomize", str(n), str(f), "--no-ambulance"]
    if scenario_name == "ByzLeader_NoAmbulance":
        return ["--randomize", str(n), str(f - 1), "--byzleader", "0", "--no-ambulance"]
    if scenario_name == "Honest_Ambulance":
        return ["--randomize", str(n), "0"]
    if scenario_name == "No_Ambulance_Honest":
        return []
    raise ValueError(scenario_name)


def run_one_simulation(
    n: int,
    scenario_name: str,
    rep: int,
    *,
    dry_run: bool,
    randomize_leader: bool = False,
) -> None:
    cfg = omnet_config_name(n)
    extra = randomize_args_for_scenario(n, scenario_name)
    seed = run_seed(MASTER_SEED, n, scenario_name, rep)

    leader_args: List[str] = []
    if randomize_leader:
        # XOR with a constant so leader draw is independent of Byzantine node draw.
        rng = random.Random(run_seed(MASTER_SEED, n, scenario_name, rep) ^ 0xDEADBEEF)
        leader_id = rng.randint(0, n - 1)
        leader_args = ["--leader", str(leader_id)]
        print(f"  Leader: replica {leader_id} (random)")

    argv = [str(RUN_SCRIPT), str(FOURWAY_DIR), *leader_args, *extra, "-u", "Cmdenv", "-c", cfg]
    # Seed bash $RANDOM for generate_random_scenario in run-resdb-simulation.sh.
    # Harmless when --randomize is omitted (No_Ambulance_Honest).
    inner = f"export RANDOM={seed} && " + " ".join(shlex.quote(a) for a in argv)
    if dry_run:
        print(f"+ simulation (RANDOM={seed})")
    run_in_bash_with_omnet(inner, dry_run=dry_run)


def run_analyze(n: int, scenario_name: str, rep: int, *, dry_run: bool) -> None:
    sub = SCENARIO_SUBDIR[scenario_name]
    save_to = REPO_ROOT / "benchmarks" / f"Priority{n}cars" / sub / f"run_{rep}"
    save_to.mkdir(parents=True, exist_ok=True)
    analyze = REPO_ROOT / "fourway" / "analyze_log.py"
    scen = ANALYZE_SCENARIO[scenario_name]
    cmd: List[str] = [
        sys.executable,
        str(analyze),
        str(LOG_FILE),
        "--save-to",
        str(save_to),
        "--scenario",
        str(scen),
        "--cars",
        str(n),
        "--no-scenario-subdir",
    ]
    print(f"+ {' '.join(cmd)}")
    if dry_run:
        return
    subprocess.run(cmd, cwd=REPO_ROOT, check=True)


def parse_args(argv: Sequence[str] | None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument(
        "--config",
        nargs="+",
        type=int,
        metavar="N",
        default=list(DEFAULT_N_VALUES),
        help="Replica counts to run (e.g. 4 8 12 16). Key generation runs once when N changes.",
    )
    p.add_argument(
        "--reps",
        type=int,
        default=REPETITIONS,
        help=f"Repetitions per scenario (default: {REPETITIONS}).",
    )
    p.add_argument(
        "--scenario",
        nargs="+",
        type=int,
        choices=sorted(SCENARIO_BY_CODE),
        metavar="CODE",
        help="Restrict to one or more scenario codes (matches analyze_log.py): "
             "1=No_Ambulance_Honest, 2=Honest_Ambulance, 3=ByzFollower_Ambulance, "
             "4=ByzLeader_Ambulance, 5=ByzLeader_NoAmbulance, 6=ByzFollower_NoAmbulance. "
             "Default: all six scenarios in order 1,6,5,2,3,4.",
    )
    p.add_argument(
        "--start-rep",
        type=int,
        default=0,
        metavar="IDX",
        help="0-based rep index to start from (default: 0). With --reps R, runs IDX..IDX+R-1; "
             "useful to resume after a partial failure without overwriting earlier run_<i> dirs.",
    )
    p.add_argument(
        "--randomize-leader",
        action="store_true",
        help="Pick a random initial consensus leader (replica ID) per run instead of always using replica 0.",
    )
    p.add_argument(
        "--dry-run",
        action="store_true",
        help="Print commands only.",
    )
    return p.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    n_values = args.config
    for n in n_values:
        if n not in DEFAULT_N_VALUES:
            print(f"WARNING: N={n} is outside the usual set {DEFAULT_N_VALUES}; "
                  "omnetpp.ini may not define a matching [Config].", file=sys.stderr)

    if args.scenario:
        # Respect user-requested order and de-duplicate aliases that map to the same scenario.
        scenarios = tuple(dict.fromkeys(SCENARIO_BY_CODE[c] for c in args.scenario))
    else:
        scenarios = SCENARIO_ORDER

    if args.start_rep < 0:
        print("ERROR: --start-rep must be >= 0", file=sys.stderr)
        return 2

    rep_indices = list(range(args.start_rep, args.start_rep + args.reps))

    print(f"Scenarios: {', '.join(scenarios)}")
    print(f"N values:  {n_values}")
    print(f"Reps:      run_{rep_indices[0]}..run_{rep_indices[-1]}")

    for n in n_values:
        print(f"\n========== Scale N={n} ==========")
        if not args.dry_run:
            run_key_generation(dry_run=args.dry_run, scale=n)
        else:
            print(f"[dry-run] would run_key_generation({n})")

        print(f"========== Skipping Build (N={n}) ==========")
        # build_veins_and_bft(dry_run=args.dry_run)

        for scenario_name in scenarios:
            if scenario_name in ("Honest_Ambulance", "No_Ambulance_Honest"):
                if not args.dry_run:
                    clear_stale_random_ini()
                else:
                    print(f"[dry-run] would clear_stale_random_ini() before {scenario_name}")

            for i, rep in enumerate(rep_indices, start=1):
                print(f"\n--- {scenario_name} rep {i}/{args.reps} (run_{rep}) ---")
                run_one_simulation(n, scenario_name, rep, dry_run=args.dry_run, randomize_leader=args.randomize_leader)
                run_analyze(n, scenario_name, rep, dry_run=args.dry_run)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

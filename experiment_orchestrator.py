#!/usr/bin/env python3
"""
Multi-scale ResDB/V2V experiment driver.

Regenerates ResDB keys once per N value, then runs
fourway/run-resdb-simulation.sh for each scenario × repetition, then analyze_log.py.
No build step — compile veins separately before running.

Layout: benchmarks/Priority<N>cars/<scenario_subdir>/run_<rep>/
Channel metrics (channel_V*.csv, sinr_V*.csv) are written into that same run_<rep>
directory via run-resdb-simulation.sh --channel-metrics-dir (see fourway/run-resdb-simulation.sh).

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
import shutil
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
    "NoFW_ByzFollower_FalseLane": "no_fw_false_lane",
    "NoFW_ByzLeader_BadProposal": "no_fw_bad_proposal",
    "NoFW_ByzLeader_FakeAmbulance": "no_fw_fake_ambulance",
    "NoCertGate_ByzFollower_FakeAmbu": "no_cert_gate_fake_ambu",
    "CertGate_ByzFollower_FakeAmbu": "cert_gate_fake_ambu",
    "NoFW_ByzLeader_TamperLane": "no_fw_tamper_lane",
    "FW_ByzLeader_FakeAmbulance": "fw_fake_ambulance",
    "FW_ByzLeader_TamperLane": "fw_tamper_lane",
    "Emergency_Preempt_DynamicN": "rollback_emergency_dynamic_n",
    "Crash_Wait_Clear": "crash_wait_clear",
}

# analyze_log.py --scenario codes (also used by --scenario CLI flag)
ANALYZE_SCENARIO = {
    "ByzLeader_Ambulance": 4,
    "ByzFollower_Ambulance": 3,
    "ByzFollower_NoAmbulance": 6,
    "Honest_Ambulance": 2,
    "No_Ambulance_Honest": 1,
    "ByzLeader_NoAmbulance": 5,
    "NoFW_ByzFollower_FalseLane": 3,
    "NoFW_ByzLeader_BadProposal": 4,
    "NoFW_ByzLeader_FakeAmbulance": 4,
    "NoCertGate_ByzFollower_FakeAmbu": 3,
    "CertGate_ByzFollower_FakeAmbu": 3,
    "NoFW_ByzLeader_TamperLane": 5,
    "FW_ByzLeader_FakeAmbulance": 4,
    "FW_ByzLeader_TamperLane": 5,
    "Emergency_Preempt_DynamicN": 15,
    "Crash_Wait_Clear": 16,
}
SCENARIO_BY_CODE = {
    1: "No_Ambulance_Honest",
    2: "Honest_Ambulance",
    3: "ByzFollower_Ambulance",
    4: "ByzLeader_Ambulance",
    5: "ByzLeader_NoAmbulance",
    6: "ByzFollower_NoAmbulance",
    7: "NoFW_ByzFollower_FalseLane",
    8: "NoFW_ByzLeader_BadProposal",
    9: "NoFW_ByzLeader_FakeAmbulance",
    10: "NoCertGate_ByzFollower_FakeAmbu",
    11: "CertGate_ByzFollower_FakeAmbu",
    12: "NoFW_ByzLeader_TamperLane",
    13: "FW_ByzLeader_FakeAmbulance",
    14: "FW_ByzLeader_TamperLane",
    15: "Emergency_Preempt_DynamicN",
    16: "Crash_Wait_Clear",
}

DEFAULT_N_VALUES = (4, 8, 12, 16, 20)
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


def bft_quorum(n: int, f: int) -> int:
    if n <= 0:
        raise ValueError("N must be positive")
    if f < 0 or f > bft_f(n):
        raise ValueError("invalid f for N")
    return (n + f + 2) // 2


def omnet_config_basename(n: int) -> str:
    names = {4: "Four", 8: "Eight", 12: "Twelve", 16: "Sixteen", 18: "Eighteen", 20: "Twenty"}
    if n not in names:
        raise ValueError(f"Unsupported N={n}; expected one of {tuple(names)}")
    return names[n]


def omnet_config_name(n: int) -> str:
    return f"{omnet_config_basename(n)}VehiclesResDB"

def baseline_omnet_config_name(n: int) -> str:
    return f"baseline{n}veh"


def benchmark_run_dir(
    n: int,
    scenario_name: str,
    rep: int,
    *,
    baseline: bool = False,
    tolerated_f: int | None = None,
) -> Path:
    """Per-run output directory (JSON/logs from analyze_log; channel CSVs from the sim)."""
    sub = SCENARIO_SUBDIR[scenario_name]
    family = "BaselinePriority" if baseline else "Priority"
    base = REPO_ROOT / "benchmarks" / f"{family}{n}cars" / sub
    if tolerated_f is not None and not baseline:
        base = base / f"f_{tolerated_f}"
    return base / f"run_{rep}"


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


def randomize_args_for_scenario(n: int, scenario_name: str, tolerated_f: int) -> List[str]:
    """
    Returns the extra flags for run-resdb-simulation.sh.

    No_Ambulance_Honest skips --randomize entirely so the script does NOT regenerate
    random_scenario.ini (which would otherwise force *.node[*].appl.ambulanceReplicaId
    even when F=0 and turn one car into an ambulance).
    """
    f = tolerated_f
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
    if scenario_name == "NoFW_ByzFollower_FalseLane":
        return ["--randomize", str(n), str(f), "--no-firewall"]
    if scenario_name == "NoFW_ByzLeader_BadProposal":
        return ["--randomize", str(n), str(f - 1), "--byzleader", "0", "--no-firewall"]
    if scenario_name == "NoFW_ByzLeader_FakeAmbulance":
        return [
            "--randomize", str(n), str(f - 1), "--byzleader", "0",
            "--leader-byz-type", "6", "--no-firewall",
        ]
    if scenario_name == "NoCertGate_ByzFollower_FakeAmbu":
        return ["--randomize", str(n), str(f), "--follower-byz-type", "7"]
    if scenario_name == "CertGate_ByzFollower_FakeAmbu":
        return ["--randomize", str(n), str(f), "--follower-byz-type", "7", "--cert-gate"]
    if scenario_name == "NoFW_ByzLeader_TamperLane":
        return [
            "--randomize", str(n), "0", "--byzleader", "0",
            "--leader-byz-type", "8", "--no-ambulance", "--no-firewall",
        ]
    if scenario_name == "FW_ByzLeader_FakeAmbulance":
        return ["--randomize", str(n), str(f - 1), "--byzleader", "0", "--leader-byz-type", "6"]
    if scenario_name == "FW_ByzLeader_TamperLane":
        return [
            "--randomize", str(n), "0", "--byzleader", "0",
            "--leader-byz-type", "8", "--no-ambulance",
        ]
    if scenario_name == "Emergency_Preempt_DynamicN":
        if n != 18:
            raise ValueError("Emergency_Preempt_DynamicN is currently defined only for N=18")
        return ["--rollback-late-emergency"]
    if scenario_name == "Crash_Wait_Clear":
        if n != 16:
            raise ValueError("Crash_Wait_Clear is currently defined only for N=16")
        return ["--crash-wait-clear"]
    raise ValueError(scenario_name)

def baseline_randomize_args_for_scenario(n: int, scenario_name: str) -> List[str]:
    if scenario_name == "Honest_Ambulance":
        return ["--baseline", "--randomize", str(n), "0"]
    if scenario_name == "No_Ambulance_Honest":
        return ["--baseline"]
    raise ValueError(f"Baseline supports only no-ambulance and honest-ambulance scenarios, got {scenario_name}")


def run_one_simulation(
    n: int,
    scenario_name: str,
    rep: int,
    *,
    dry_run: bool,
    tolerated_f: int,
    explicit_tolerate: bool,
    randomize_leader: bool = False,
    baseline: bool = False,
) -> None:
    cfg = baseline_omnet_config_name(n) if baseline else omnet_config_name(n)
    extra = baseline_randomize_args_for_scenario(n, scenario_name) if baseline else randomize_args_for_scenario(n, scenario_name, tolerated_f)
    tolerate_args = (
        ["--tolerated-f", str(tolerated_f)]
        if not baseline and "--randomize" in extra
        else []
    )
    seed = run_seed(MASTER_SEED, n, scenario_name, rep)
    run_dir = benchmark_run_dir(
        n,
        scenario_name,
        rep,
        baseline=baseline,
        tolerated_f=tolerated_f if explicit_tolerate else None,
    )
    metrics_dir = str(run_dir.resolve())

    leader_args: List[str] = []
    if randomize_leader and not baseline:
        # XOR with a constant so leader draw is independent of Byzantine node draw.
        rng = random.Random(run_seed(MASTER_SEED, n, scenario_name, rep) ^ 0xDEADBEEF)
        leader_id = rng.randint(0, n - 1)
        leader_args = ["--leader", str(leader_id)]
        print(f"  Leader: replica {leader_id} (random)")

    if not dry_run:
        run_dir.mkdir(parents=True, exist_ok=True)

    argv = [
        str(RUN_SCRIPT),
        str(FOURWAY_DIR),
        "--channel-metrics-dir",
        metrics_dir,
        *leader_args,
        *tolerate_args,
        *extra,
        "-u",
        "Cmdenv",
        "-c",
        cfg,
    ]
    # Seed bash $RANDOM for generate_random_scenario in run-resdb-simulation.sh.
    # Harmless when --randomize is omitted (No_Ambulance_Honest).
    inner = f"export RANDOM={seed} && " + " ".join(shlex.quote(a) for a in argv)
    if dry_run:
        print(f"+ simulation (RANDOM={seed})")
    run_in_bash_with_omnet(inner, dry_run=dry_run)


def collect_baseline_sumo_outputs(n: int, run_dir: Path, *, dry_run: bool) -> Path:
    tripinfo = FOURWAY_DIR / f"baseline_{n}veh.tripinfo.xml"
    for suffix in ("tripinfo.xml", "summary.xml", "statistics.xml"):
        src = FOURWAY_DIR / f"baseline_{n}veh.{suffix}"
        dst = run_dir / src.name
        if dry_run:
            print(f"[dry-run] would copy {src} -> {dst}")
        elif src.is_file():
            shutil.copy2(src, dst)
    return tripinfo


def run_analyze(
    n: int,
    scenario_name: str,
    rep: int,
    *,
    dry_run: bool,
    tolerated_f: int,
    explicit_tolerate: bool,
    baseline: bool = False,
) -> None:
    save_to = benchmark_run_dir(
        n,
        scenario_name,
        rep,
        baseline=baseline,
        tolerated_f=tolerated_f if explicit_tolerate else None,
    )
    save_to.mkdir(parents=True, exist_ok=True)
    analyze = REPO_ROOT / "fourway" / "analyze_log.py"
    scen = ANALYZE_SCENARIO[scenario_name]
    tripinfo = collect_baseline_sumo_outputs(n, save_to, dry_run=dry_run) if baseline else None
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
    if baseline:
        cmd.extend([
            "--coordination-method",
            "sumo_baseline",
            "--baseline-tripinfo",
            str(tripinfo),
            "--scenario-ini",
            str(FOURWAY_DIR / "random_scenario.ini"),
        ])
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
        help="Restrict to one or more scenario codes: "
             "1=No_Ambulance_Honest, 2=Honest_Ambulance, 3=ByzFollower_Ambulance, "
             "4=ByzLeader_Ambulance, 5=ByzLeader_NoAmbulance, 6=ByzFollower_NoAmbulance, "
             "7=NoFW_ByzFollower_FalseLane, 8=NoFW_ByzLeader_BadProposal, "
             "9=NoFW_ByzLeader_FakeAmbulance, 10=NoCertGate_ByzFollower_FakeAmbu, "
             "11=CertGate_ByzFollower_FakeAmbu, 12=NoFW_ByzLeader_TamperLane, "
             "13=FW_ByzLeader_FakeAmbulance, 14=FW_ByzLeader_TamperLane, "
             "15=Emergency_Preempt_DynamicN, 16=Crash_Wait_Clear. "
             "Default: all six baseline scenarios in order 1,6,5,2,3,4.",
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
        "--tolerate",
        type=int,
        default=None,
        metavar="F",
        help="Fault tolerance f for ResDB active-view experiments. "
             "Defaults to max floor((N-1)/3) for each N. "
             "For Byzantine follower scenarios, this also sets the number of follower faults injected.",
    )
    p.add_argument(
        "--baseline",
        action="store_true",
        help="Run SUMO all-way-stop baseline configs for no-priority and priority-vehicle scenarios.",
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
        tolerate_n = args.tolerate if args.tolerate is not None else bft_f(n)
        if tolerate_n < 0:
            print("ERROR: --tolerate must be >= 0.", file=sys.stderr)
            return 2
        if tolerate_n > bft_f(n):
            print(
                f"ERROR: --tolerate {tolerate_n} invalid for N={n}; "
                f"require f <= floor((N-1)/3)={bft_f(n)}.",
                file=sys.stderr,
            )
            return 2
        if args.scenario and 15 in args.scenario and n != 18:
            print("ERROR: scenario 15 Emergency_Preempt_DynamicN is currently defined only for --config 18.", file=sys.stderr)
            return 2
        if args.scenario and 16 in args.scenario and n != 16:
            print("ERROR: scenario 16 Crash_Wait_Clear is currently defined only for --config 16.", file=sys.stderr)
            return 2

    if args.scenario:
        # Respect user-requested order and de-duplicate aliases that map to the same scenario.
        scenarios = tuple(dict.fromkeys(SCENARIO_BY_CODE[c] for c in args.scenario))
    else:
        scenarios = SCENARIO_ORDER

    if args.baseline:
        allowed = ("No_Ambulance_Honest", "Honest_Ambulance")
        scenarios = tuple(s for s in scenarios if s in allowed)
        if not scenarios:
            print("ERROR: --baseline only supports scenario codes 1 and 2.", file=sys.stderr)
            return 2

    if args.start_rep < 0:
        print("ERROR: --start-rep must be >= 0", file=sys.stderr)
        return 2

    rep_indices = list(range(args.start_rep, args.start_rep + args.reps))

    print(f"Scenarios: {', '.join(scenarios)}")
    print(f"N values:  {n_values}")
    print(f"Reps:      run_{rep_indices[0]}..run_{rep_indices[-1]}")
    print(f"Tolerate:  {args.tolerate if args.tolerate is not None else 'max per N'}")

    for n in n_values:
        tolerated_f = args.tolerate if args.tolerate is not None else bft_f(n)
        print(f"\n========== Scale N={n} ==========")
        print(f"========== Tolerated f={tolerated_f} (quorum={bft_quorum(n, tolerated_f)}, static N={n}) ==========")
        if not args.dry_run and not args.baseline:
            run_key_generation(dry_run=args.dry_run, scale=n)
        elif args.dry_run and not args.baseline:
            print(f"[dry-run] would run_key_generation({n})")
        else:
            print("========== Skipping Key Generation (baseline) ==========")

        print(f"========== Skipping Build (N={n}) ==========")
        # build_veins_and_bft(dry_run=args.dry_run)

        for scenario_name in scenarios:
            if scenario_name in ("Honest_Ambulance", "No_Ambulance_Honest", "Emergency_Preempt_DynamicN", "Crash_Wait_Clear"):
                if not args.dry_run:
                    clear_stale_random_ini()
                else:
                    print(f"[dry-run] would clear_stale_random_ini() before {scenario_name}")

            for i, rep in enumerate(rep_indices, start=1):
                print(f"\n--- {scenario_name} rep {i}/{args.reps} (run_{rep}) ---")
                run_one_simulation(
                    n,
                    scenario_name,
                    rep,
                    dry_run=args.dry_run,
                    tolerated_f=tolerated_f,
                    explicit_tolerate=args.tolerate is not None,
                    randomize_leader=args.randomize_leader,
                    baseline=args.baseline,
                )
                run_analyze(
                    n,
                    scenario_name,
                    rep,
                    dry_run=args.dry_run,
                    tolerated_f=tolerated_f,
                    explicit_tolerate=args.tolerate is not None,
                    baseline=args.baseline,
                )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

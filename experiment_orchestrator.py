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
import csv
import hashlib
import json
import math
import os
import random
import re
import shutil
import shlex
import statistics
import subprocess
import struct
import sys
from pathlib import Path
from typing import Any, Dict, List, Sequence, Tuple

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
    "Emergency_SuppressCancel_Unguarded": "rollback_suppress_cancel_unguarded",
    "Emergency_SuppressCancel_Guarded": "rollback_suppress_cancel_guarded",
    "Crash_FabricatedClear_Unguarded": "crash_fabricated_clear_unguarded",
    "Crash_FabricatedClear_Guarded": "crash_fabricated_clear_guarded",
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
    "Emergency_SuppressCancel_Unguarded": 15,
    "Emergency_SuppressCancel_Guarded": 15,
    "Crash_FabricatedClear_Unguarded": 16,
    "Crash_FabricatedClear_Guarded": 16,
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
    17: "Emergency_SuppressCancel_Unguarded",
    18: "Emergency_SuppressCancel_Guarded",
    19: "Crash_FabricatedClear_Unguarded",
    20: "Crash_FabricatedClear_Guarded",
}

DEFAULT_N_VALUES = (4, 8, 12, 16, 20)
SUPPORTED_N_VALUES = (4, 8, 12, 16, 18, 20)
REPETITIONS = 5

SCENARIO_ORDER: Tuple[str, ...] = (
    "No_Ambulance_Honest",
    "ByzFollower_NoAmbulance",
    "ByzLeader_NoAmbulance",
    "Honest_Ambulance",
    "ByzFollower_Ambulance",
    "ByzLeader_Ambulance",
)

PHASE1_VALIDATION_CELLS: Tuple[Dict[str, Any], ...] = (
    {"name": "e0_n4", "n": 4, "sigma": 0.0, "signal_error": 0.0, "kind": "e0"},
    {"name": "signal_error_n4", "n": 4, "sigma": 0.0, "signal_error": 1.0, "kind": "signal"},
    {"name": "lane_error_n4", "n": 4, "sigma": 2.0, "signal_error": 0.0, "kind": "lane"},
    {"name": "e0_n16", "n": 16, "sigma": 0.0, "signal_error": 0.0, "kind": "e0"},
)

PHASE2_FIXTURE_N = 16
PHASE2_FIXTURE_CONFIG = "SixteenVehiclesMixedResDB"
PHASE2_FIXTURE_MANIFEST = FOURWAY_DIR / "phase2_mixed_route_manifest.csv"
PHASE2_CUE_TRACE_PREFIX = "[PHASE2-CUE-TRACE]"
PHASE2_ATTACK_N = 16
PHASE2_ATTACK_CONFIG = "SixteenVehiclesResDB"
PHASE2_ATTACK_TARGET_DEFAULT = 0
PHASE2_SIGMAS = (0.0, 0.25, 0.5, 1.0, 2.0)
PHASE2_SIGNAL_ERRORS = (0.0, 0.02, 0.05, 0.10, 0.20, 0.30)


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
    injected_f: int | None = None,
    approach_sigma: float = 0.0,
    signal_error: float = 0.0,
) -> Path:
    """Per-run output directory (JSON/logs from analyze_log; channel CSVs from the sim)."""
    sub = SCENARIO_SUBDIR[scenario_name]
    family = "BaselinePriority" if baseline else "Priority"
    base = REPO_ROOT / "benchmarks" / f"{family}{n}cars" / sub
    if tolerated_f is not None and not baseline:
        base = base / f"f_{tolerated_f}"
    if injected_f is not None and not baseline:
        base = base / f"F_{injected_f}"
    if not baseline:
        sigma_label = format(approach_sigma, "g").replace(".", "p")
        signal_label = format(signal_error, "g").replace(".", "p")
        base = base / f"sigma_{sigma_label}_signal_{signal_label}"
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
    """Remove generated scenario overlays that must not bleed into the next run.

    run-resdb-simulation.sh also deletes overlays it will not recreate; this is
    a belt-and-suspenders cleanup before honest / scenario-15 / scenario-16 runs.
    """
    for name in (
        "random_scenario.ini",
        "rollback_late_emergency.ini",
        "crash_wait_clear.ini",
        "leader_override.ini",
        "perception_override.ini",
        "phase2_attack_override.ini",
    ):
        path = FOURWAY_DIR / name
        if path.is_file():
            path.unlink()


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


def randomize_args_for_scenario(
    n: int,
    scenario_name: str,
    tolerated_f: int,
    injected_f: int | None = None,
) -> List[str]:
    """
    Returns the extra flags for run-resdb-simulation.sh.

    No_Ambulance_Honest skips --randomize entirely so the script does NOT regenerate
    random_scenario.ini (which would otherwise force *.node[*].appl.ambulanceReplicaId
    even when F=0 and turn one car into an ambulance).
    """
    f = tolerated_f
    follower_f = f if injected_f is None else injected_f
    if scenario_name == "ByzLeader_Ambulance":
        return ["--randomize", str(n), str(f - 1), "--byzleader", "0"]
    if scenario_name == "ByzFollower_Ambulance":
        return ["--randomize", str(n), str(follower_f)]
    if scenario_name == "ByzFollower_NoAmbulance":
        return ["--randomize", str(n), str(follower_f), "--no-ambulance"]
    if scenario_name == "ByzLeader_NoAmbulance":
        return ["--randomize", str(n), str(f - 1), "--byzleader", "0", "--no-ambulance"]
    if scenario_name == "Honest_Ambulance":
        return ["--randomize", str(n), "0"]
    if scenario_name == "No_Ambulance_Honest":
        return []
    if scenario_name == "NoFW_ByzFollower_FalseLane":
        return ["--randomize", str(n), str(follower_f)]
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
    if scenario_name == "Emergency_SuppressCancel_Unguarded":
        if n != 18:
            raise ValueError("Emergency_SuppressCancel_Unguarded is defined only for N=18")
        return [
            "--rollback-late-emergency",
            "--suppress-initial-cancel-leader",
            "--disable-cancel-leader-failover",
        ]
    if scenario_name == "Emergency_SuppressCancel_Guarded":
        if n != 18:
            raise ValueError("Emergency_SuppressCancel_Guarded is defined only for N=18")
        return ["--rollback-late-emergency", "--suppress-initial-cancel-leader"]
    if scenario_name == "Crash_FabricatedClear_Unguarded":
        if n != 16:
            raise ValueError("Crash_FabricatedClear_Unguarded is defined only for N=16")
        return [
            "--crash-wait-clear",
            "--fabricate-clearance",
            "--disable-recovery-clear-evidence-gate",
        ]
    if scenario_name == "Crash_FabricatedClear_Guarded":
        if n != 16:
            raise ValueError("Crash_FabricatedClear_Guarded is defined only for N=16")
        return ["--crash-wait-clear", "--fabricate-clearance"]
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
    injected_f: int | None = None,
    paired_false_lane_seed: bool = False,
    randomize_leader: bool = False,
    baseline: bool = False,
    approach_sigma: float = 0.0,
    signal_error: float = 0.0,
    direction_collection_window: float = 0.25,
    extra_runner_args: Sequence[str] = (),
) -> None:
    cfg = baseline_omnet_config_name(n) if baseline else omnet_config_name(n)
    extra = baseline_randomize_args_for_scenario(n, scenario_name) if baseline else randomize_args_for_scenario(
        n, scenario_name, tolerated_f, injected_f
    )
    tolerate_args = (
        ["--tolerated-f", str(tolerated_f)]
        if not baseline and "--randomize" in extra
        else []
    )
    seed_name = "FALSE_LANE_PAIRED" if paired_false_lane_seed else scenario_name
    seed = run_seed(MASTER_SEED, n, seed_name, rep)
    run_dir = benchmark_run_dir(
        n,
        scenario_name,
        rep,
        baseline=baseline,
        tolerated_f=tolerated_f if explicit_tolerate else None,
        injected_f=injected_f,
        approach_sigma=approach_sigma,
        signal_error=signal_error,
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
        (run_dir / "perception_run_metadata.json").write_text(
            json.dumps({
                "simulation_seed": seed,
                "approach_sigma_m": approach_sigma,
                "signal_observation_error": signal_error,
                "direction_collection_window_sec": direction_collection_window,
                "config": cfg,
                "scenario": scenario_name,
                "rep": rep,
                "n": n,
            }, indent=2, sort_keys=True) + "\n"
        )

    argv = [
        str(RUN_SCRIPT),
        str(FOURWAY_DIR),
        "--channel-metrics-dir",
        metrics_dir,
        "--approach-sigma", str(approach_sigma),
        "--signal-error", str(signal_error),
        "--direction-collection-window", str(direction_collection_window),
        "--simulation-seed", str(seed),
        *leader_args,
        *tolerate_args,
        *extra,
        *extra_runner_args,
        "-u",
        "Cmdenv",
        "-c",
        cfg,
    ]
    # Seed bash $RANDOM for generate_random_scenario in run-resdb-simulation.sh.
    # `export RANDOM=<seed>` does not work here: RANDOM only reseeds when
    # assigned inside the bash process that reads it, not via an inherited/
    # exported value from a parent shell into a freshly started child bash
    # (the wrapper script runs as its own process). So we pass the seed through
    # a normal env var and the wrapper assigns it to RANDOM in its own process
    # immediately before drawing. Harmless when --randomize is omitted
    # (No_Ambulance_Honest).
    inner = f"export SCENARIO_RANDOM_SEED={seed} && " + " ".join(shlex.quote(a) for a in argv)
    if dry_run:
        print(f"+ {inner}")
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
    injected_f: int | None = None,
    baseline: bool = False,
    approach_sigma: float = 0.0,
    signal_error: float = 0.0,
) -> None:
    save_to = benchmark_run_dir(
        n,
        scenario_name,
        rep,
        baseline=baseline,
        tolerated_f=tolerated_f if explicit_tolerate else None,
        injected_f=injected_f,
        approach_sigma=approach_sigma,
        signal_error=signal_error,
    )
    if not dry_run:
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
        "--run-index",
        str(rep),
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


def _load_perception_summary(json_path: Path) -> Dict[str, Any]:
    """Load the run-level perception block repeated in each analyzer row."""
    records = json.loads(json_path.read_text())
    if not isinstance(records, list) or not records:
        raise ValueError(f"expected a nonempty analyzer record list in {json_path}")
    perception = records[0].get("bft_stats", {}).get("perception")
    if not isinstance(perception, dict):
        raise ValueError(f"missing bft_stats.perception in {json_path}")
    return perception


def _validate_phase1_run(
    cell: Dict[str, Any],
    rep: int,
    run_dir: Path,
    log_text: str,
) -> Dict[str, Any]:
    """Evaluate the Phase 1 exit invariants for one completed run."""
    n = int(cell["n"])
    json_path = run_dir / f"{n}veh_{rep}.json"
    checks: List[Dict[str, Any]] = []

    def check(name: str, passed: bool, detail: str) -> None:
        checks.append({"name": name, "passed": bool(passed), "detail": detail})

    try:
        perception = _load_perception_summary(json_path)
        analyzer_records = json.loads(json_path.read_text())
        run_safety = analyzer_records[0].get("run_safety", {})
        run_safety = run_safety if isinstance(run_safety, dict) else {}
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        check("analyzer output", False, str(exc))
        perception = {}
        run_safety = {}

    lane_evals = int(perception.get("lane_evaluation_count", 0))
    lane_accepts = int(perception.get("lane_accept_count", 0))
    certificates = perception.get("certificates", {})
    eligibility = perception.get("direction_eligibility", {})
    certificates = certificates if isinstance(certificates, dict) else {}
    eligibility = eligibility if isinstance(eligibility, dict) else {}

    check("perception evaluated", lane_evals > 0, f"lane evaluations={lane_evals}")
    duplicate_count = int(perception.get("duplicate_evaluation_count", -1))
    invariant_ok = perception.get("single_evaluation_invariant_ok") is True
    check(
        "single-shot cache",
        duplicate_count == 0 and invariant_ok,
        f"duplicates={duplicate_count}, analyzer_invariant={invariant_ok}",
    )
    perception_pairs = re.findall(
        r"\[PERC-EVAL\] witness=(\d+) target=veh(\d+)\b", log_text
    )
    self_evaluations = sum(witness == target for witness, target in perception_pairs)
    check(
        "no self perception evaluations",
        self_evaluations == 0,
        f"self evaluations={self_evaluations}",
    )
    self_rows = re.findall(
        r"\[SELF-ATTEST\] target=veh(\d+) epoch=(\d+) signer=(\d+) "
        r"claimHash=([0-9a-fA-F]+) observedCue=(\S+) status=VALID",
        log_text,
    )
    valid_self_rows = [row for row in self_rows if row[0] == row[2]]
    self_keys = {(target, epoch, signer, claim_hash) for target, epoch, signer, claim_hash, _ in valid_self_rows}
    check(
        "one self-attestation per claimant",
        len(valid_self_rows) == n and len(self_keys) == n,
        f"valid={len(valid_self_rows)}/{n}, distinct={len(self_keys)}/{n}",
    )
    radio_self_echoes = re.findall(
        r"\[ECHO-SEND\] Replica (\d+) .*? veh(\d+) ARRIVAL_ECHO", log_text
    )
    radio_self_echoes = [row for row in radio_self_echoes if row[0] == row[1]]
    check(
        "self-attestation stays off Type-4 radio",
        not radio_self_echoes,
        f"self ECHO-SEND records={len(radio_self_echoes)}",
    )
    analyzer_self_count = int(perception.get("self_attestation_count", -1))
    analyzer_self_duplicates = int(perception.get("self_attestation_duplicate_count", -1))
    check(
        "self-attestation analyzer invariant",
        analyzer_self_count == n and analyzer_self_duplicates == 0 and
            perception.get("self_attestation_invariant_ok") is True,
        f"count={analyzer_self_count}/{n}, duplicates={analyzer_self_duplicates}",
    )
    evidence = perception.get("certificate_evidence", {})
    evidence = evidence if isinstance(evidence, dict) else {}
    cert_self_counts = []
    duplicate_cert_signers = 0
    for signer_rows in evidence.values():
        if not isinstance(signer_rows, dict):
            continue
        cert_self_counts.append(sum(
            1 for row in signer_rows.values()
            if isinstance(row, dict) and row.get("self") is True
        ))
        duplicate_cert_signers += max(0, len(signer_rows) - n)
    check(
        "certificate self signer and N cap",
        len(cert_self_counts) == len(certificates) and
            all(count == 1 for count in cert_self_counts) and duplicate_cert_signers == 0 and
            all(int(row.get("echo_count", n + 1)) <= n for row in certificates.values()
                if isinstance(row, dict)),
        f"certificates={len(certificates)}, self-counts={cert_self_counts}, over-cap={duplicate_cert_signers}",
    )

    bad_validation_lines = log_text.count("[CERT-INVALID]") + log_text.count("verify FAIL")
    check("certificate validation", bad_validation_lines == 0, f"failure markers={bad_validation_lines}")
    timer_leaks = sum(
        log_text.count(name)
        for name in ("undisposed object: (omnetpp::cMessage) resdbArrivalCertFinalize",
                     "undisposed object: (omnetpp::cMessage) resdbDiscoveryTxFlush")
    )
    check("perception timers disposed", timer_leaks == 0, f"matching undisposed timers={timer_leaks}")

    departed = set(re.findall(r"\[DEPARTED\] Replica (\d+)\b", log_text))
    check("vehicles departed", len(departed) == n, f"unique departed={len(departed)}/{n}")
    unsafe_pairs = int(run_safety.get("unsafe_conflict_cooccupancy_pair_count", -1))
    collision_vehicles = int(run_safety.get("physical_collision_vehicle_count", -1))
    check(
        "physical safety",
        unsafe_pairs == 0 and collision_vehicles == 0,
        f"unsafe conflict pairs={unsafe_pairs}, collision vehicles={collision_vehicles}",
    )

    kind = str(cell["kind"])
    if kind == "e0":
        check("lane certificates", len(certificates) == n, f"certificates={len(certificates)}/{n}")
        signed = int(perception.get("signed_direction_count", -1))
        unknown = int(perception.get("signed_unknown_count", -1))
        quiet = int(perception.get("quiet_count", -1))
        check("E0 trust tiers", signed == n and unknown == 0 and quiet == 0,
              f"SIGNED-dir={signed}, SIGNED-UNKNOWN={unknown}, QUIET={quiet}")
        derived = [entry.get("derived") for entry in eligibility.values() if isinstance(entry, dict)]
        check("E0 direction", len(derived) == n and all(value == "S" for value in derived),
              f"STRAIGHT={derived.count('S')}/{n}")
        rng_rows = re.findall(r"\[PERCEPTION-RNG\] replica=(\d+) draws=(\d+)\b", log_text)
        zero_rng_ids = {replica for replica, draws in rng_rows if int(draws) == 0}
        check("E0 perception RNG", len(zero_rng_ids) == n,
              f"zero-draw replicas={len(zero_rng_ids)}/{n}")
        if n == 16:
            extra_echo_certs = sum(
                1 for entry in certificates.values()
                if isinstance(entry, dict) and int(entry.get("echo_count", 0)) > int(entry.get("threshold", 0))
            )
            check("collect beyond f+1", extra_echo_certs > 0,
                  f"certificates above threshold={extra_echo_certs}")
    elif kind == "signal":
        signed = int(perception.get("signed_direction_count", -1))
        unknown = int(perception.get("signed_unknown_count", -1))
        quiet = int(perception.get("quiet_count", -1))
        derived = [entry.get("derived") for entry in eligibility.values() if isinstance(entry, dict)]
        check("lane certificates survive cue noise", len(certificates) == n,
              f"certificates={len(certificates)}/{n}")
        check("signal-error trust tiers", signed == 0 and unknown == n and quiet == 0,
              f"SIGNED-dir={signed}, SIGNED-UNKNOWN={unknown}, QUIET={quiet}")
        check("signal-error direction", len(derived) == n and all(value == "U" for value in derived),
              f"UNKNOWN={derived.count('U')}/{n}")
    elif kind == "lane":
        lane_rate = perception.get("lane_accept_rate")
        rate_is_nontrivial = isinstance(lane_rate, (int, float)) and 0.0 <= float(lane_rate) < 1.0
        check("lane noise exercised", rate_is_nontrivial and lane_accepts < lane_evals,
              f"accepted={lane_accepts}/{lane_evals}, rate={lane_rate}")
    else:
        check("known validation cell", False, f"unknown kind={kind}")

    failures = [item for item in checks if not item["passed"]]
    return {
        "cell": cell["name"],
        "kind": kind,
        "n": n,
        "rep": rep,
        "result_dir": str(run_dir),
        "passed": not failures,
        "checks": checks,
        "failures": failures,
    }


def _extract_committed_order_reference(log_text: str, n: int) -> Dict[str, Any] | None:
    pattern = re.compile(
        rf"\[EXECUTOR\] OrderDecision: epoch=(\d+) n_vehicles={n} n_batches=(\d+) "
        r"decisions=\[(.*?)\]"
    )
    decision_pattern = re.compile(r"veh=(-?\d+) batch=(\d+)")
    for match in pattern.finditer(log_text):
        decisions = [(int(replica), int(batch))
                     for replica, batch in decision_pattern.findall(match.group(3))]
        if len(decisions) != n:
            continue
        wire = struct.pack("<III", int(match.group(1)), n, int(match.group(2)))
        wire += b"".join(struct.pack("<iI", replica, batch)
                         for replica, batch in decisions)
        return {
            "epoch": int(match.group(1)),
            "n_vehicles": n,
            "n_batches": int(match.group(2)),
            "decisions": [
                {"replica_id": replica, "batch_index": batch}
                for replica, batch in decisions
            ],
            "wire_hex": wire.hex(),
            "sha256": hashlib.sha256(wire).hexdigest(),
        }
    return None


def _capture_phase2b_golden_if_available() -> Path:
    golden_dir = REPO_ROOT / "benchmarks" / "Phase2SelfAttestation"
    golden_dir.mkdir(parents=True, exist_ok=True)
    golden_path = golden_dir / "pre_self_attestation_order_reference.json"
    if golden_path.is_file():
        return golden_path

    references: Dict[str, Any] = {}
    for n in (4, 16):
        raw_log = benchmark_run_dir(
            n, "No_Ambulance_Honest", 0,
            approach_sigma=0.0, signal_error=0.0,
        ) / "raw_simulation.log"
        if not raw_log.is_file():
            raise RuntimeError(f"missing pre-self-attestation reference log: {raw_log}")
        text = raw_log.read_text(errors="replace")
        if "[SELF-ATTEST]" in text:
            raise RuntimeError(
                f"reference log already contains self-attestation and cannot be frozen: {raw_log}"
            )
        order = _extract_committed_order_reference(text, n)
        if order is None:
            raise RuntimeError(f"could not reconstruct a complete order from {raw_log}")
        references[str(n)] = {"source": str(raw_log), "order": order}
    golden_path.write_text(json.dumps({
        "format": "ResdbOrderHdr followed by ResdbVehicleDecision records, little-endian",
        "references": references,
    }, indent=2, sort_keys=True) + "\n")
    return golden_path


def _validate_phase2b_order_references(golden_path: Path) -> List[Dict[str, Any]]:
    golden = json.loads(golden_path.read_text()).get("references", {})
    checks: List[Dict[str, Any]] = []
    for n in (4, 16):
        raw_log = benchmark_run_dir(
            n, "No_Ambulance_Honest", 0,
            approach_sigma=0.0, signal_error=0.0,
        ) / "raw_simulation.log"
        actual = _extract_committed_order_reference(raw_log.read_text(errors="replace"), n)
        expected = golden.get(str(n), {}).get("order")
        passed = actual is not None and expected is not None and actual["wire_hex"] == expected["wire_hex"]
        checks.append({
            "name": f"N={n} committed-order bytes",
            "passed": passed,
            "detail": (
                f"expected={expected.get('sha256') if expected else None} "
                f"actual={actual.get('sha256') if actual else None}"
            ),
        })
    return checks


def run_phase1_validation(args: argparse.Namespace, repetitions: int) -> int:
    """Run and self-check the fixed E0 plus nonzero Phase 1 smoke suite."""
    rep_indices = list(range(args.start_rep, args.start_rep + repetitions))
    scenario_name = "No_Ambulance_Honest"
    results: List[Dict[str, Any]] = []
    active_n: int | None = None

    print("Phase 1 validation: E0 N=4, signal-error N=4, lane-error N=4, E0 N=16")
    print(f"Reps: run_{rep_indices[0]}..run_{rep_indices[-1]}")
    for cell in PHASE1_VALIDATION_CELLS:
        n = int(cell["n"])
        if n != active_n:
            if args.dry_run:
                print(f"[dry-run] would run_key_generation({n})")
            else:
                run_key_generation(dry_run=False, scale=n)
            active_n = n

        tolerated_f = bft_f(n)
        for i, rep in enumerate(rep_indices, start=1):
            print(
                f"\n--- Phase 1 {cell['name']} rep {i}/{repetitions} (run_{rep}) "
                f"sigma={cell['sigma']} signal_error={cell['signal_error']} ---"
            )
            if args.dry_run:
                print("[dry-run] would clear_stale_random_ini()")
            else:
                clear_stale_random_ini()
            run_dir = benchmark_run_dir(
                n,
                scenario_name,
                rep,
                approach_sigma=float(cell["sigma"]),
                signal_error=float(cell["signal_error"]),
            )
            try:
                run_one_simulation(
                    n,
                    scenario_name,
                    rep,
                    dry_run=args.dry_run,
                    tolerated_f=tolerated_f,
                    explicit_tolerate=False,
                    approach_sigma=float(cell["sigma"]),
                    signal_error=float(cell["signal_error"]),
                    direction_collection_window=args.direction_collection_window,
                )
                if not args.dry_run and LOG_FILE.is_file():
                    run_dir.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(LOG_FILE, run_dir / "raw_simulation.log")
                run_analyze(
                    n,
                    scenario_name,
                    rep,
                    dry_run=args.dry_run,
                    tolerated_f=tolerated_f,
                    explicit_tolerate=False,
                    approach_sigma=float(cell["sigma"]),
                    signal_error=float(cell["signal_error"]),
                )
            except subprocess.CalledProcessError as exc:
                if LOG_FILE.is_file():
                    run_dir.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(LOG_FILE, run_dir / "raw_simulation.log")
                result = {
                    "cell": cell["name"],
                    "kind": cell["kind"],
                    "n": n,
                    "rep": rep,
                    "result_dir": str(run_dir),
                    "passed": False,
                    "checks": [{
                        "name": "simulation and analysis",
                        "passed": False,
                        "detail": f"command exited with status {exc.returncode}",
                    }],
                    "failures": [{
                        "name": "simulation and analysis",
                        "passed": False,
                        "detail": f"command exited with status {exc.returncode}",
                    }],
                }
                results.append(result)
                print(f"[FAIL] {cell['name']} run_{rep}: simulation/analysis exit status {exc.returncode}")
                continue
            if args.dry_run:
                print(f"[dry-run] would validate {run_dir}")
                continue
            result = _validate_phase1_run(cell, rep, run_dir, LOG_FILE.read_text(errors="replace"))
            (run_dir / "phase1_validation.json").write_text(
                json.dumps(result, indent=2, sort_keys=True) + "\n"
            )
            results.append(result)
            status = "PASS" if result["passed"] else "FAIL"
            print(f"[{status}] {cell['name']} run_{rep}")
            for item in result["checks"]:
                marker = "PASS" if item["passed"] else "FAIL"
                print(f"  {marker:4} {item['name']}: {item['detail']}")

    if args.dry_run:
        print("\n[dry-run] Phase 1 suite contains four fixed parameter cells; no validation was executed.")
        return 0

    summary_dir = REPO_ROOT / "benchmarks" / "Phase1Validation"
    summary_dir.mkdir(parents=True, exist_ok=True)
    summary_path = summary_dir / "phase1_validation_summary.json"
    overall_passed = bool(results) and all(result["passed"] for result in results)
    summary_path.write_text(json.dumps({
        "passed": overall_passed,
        "repetitions": repetitions,
        "start_rep": args.start_rep,
        "results": results,
    }, indent=2, sort_keys=True) + "\n")

    print("\n========== PHASE 1 VALIDATION SUMMARY ==========")
    for result in results:
        print(f"{'PASS' if result['passed'] else 'FAIL'}  {result['cell']} run_{result['rep']}  {result['result_dir']}")
    print(f"Overall: {'PASS' if overall_passed else 'FAIL'}")
    print(f"Machine-readable summary: {summary_path}")
    return 0 if overall_passed else 1


def run_phase2_self_attestation_validation(args: argparse.Namespace, repetitions: int) -> int:
    """Run Checkpoint 2B and stop before the E2/E4 attack checkpoint."""
    golden_path: Path | None = None
    if args.dry_run:
        print("[dry-run] would freeze pre-self N=4/N=16 committed-order bytes")
    else:
        try:
            golden_path = _capture_phase2b_golden_if_available()
        except RuntimeError as exc:
            print(f"ERROR: {exc}", file=sys.stderr)
            return 1

    phase1_rc = run_phase1_validation(args, repetitions)
    if args.dry_run:
        print("[dry-run] would compare N=4/N=16 committed-order bytes")
        print("[dry-run] would run N=4 signal-error=1 Byzantine-primary UNKNOWN upgrade test")
        return phase1_rc

    order_checks = _validate_phase2b_order_references(golden_path)
    if phase1_rc != 0 or not all(row["passed"] for row in order_checks):
        mutation = {
            "name": "Check 10 UNKNOWN upgrade",
            "passed": False,
            "detail": "not run because Phase 1 or committed-order comparison failed",
        }
    else:
        n = 4
        rep = args.start_rep
        scenario = "ByzLeader_NoAmbulance"
        run_key_generation(dry_run=False, scale=n)
        clear_stale_random_ini()
        try:
            run_one_simulation(
                n, scenario, rep,
                dry_run=False,
                tolerated_f=bft_f(n),
                explicit_tolerate=False,
                signal_error=1.0,
                direction_collection_window=args.direction_collection_window,
                extra_runner_args=("--leader-byz-type", "9"),
            )
            mutation_dir = REPO_ROOT / "benchmarks" / "Phase2SelfAttestation" / "unknown_upgrade" / f"run_{rep}"
            mutation_dir.mkdir(parents=True, exist_ok=True)
            shutil.copy2(LOG_FILE, mutation_dir / "raw_simulation.log")
            run_analyze(
                n, scenario, rep,
                dry_run=False,
                tolerated_f=bft_f(n),
                explicit_tolerate=False,
                signal_error=1.0,
            )
            text = LOG_FILE.read_text(errors="replace")
            injected = "UPGRADE_UNKNOWN_DIRECTION" in text
            rejected = bool(re.search(
                r"\[OMNET-PREVERIFY\] reject: state-field mismatch.*cert\([^\n]*dir=3[^\n]*proposal\([^\n]*dir=0",
                text,
            ))
            view_change = "[VC-TRIGGER]" in text or "primary-changed" in text
            decided = "Order_Decided_Time" in text
            departed = len(set(re.findall(r"\[DEPARTED\] Replica (\d+)\b", text))) == n
            passed = injected and rejected and view_change and decided and departed
            mutation = {
                "name": "Check 10 UNKNOWN upgrade",
                "passed": passed,
                "detail": (
                    f"injected={injected}, rejected={rejected}, view_change={view_change}, "
                    f"decided={decided}, departed={departed}"
                ),
                "result_dir": str(mutation_dir),
            }
            (mutation_dir / "phase2b_unknown_upgrade_validation.json").write_text(
                json.dumps(mutation, indent=2, sort_keys=True) + "\n"
            )
        except subprocess.CalledProcessError as exc:
            mutation = {
                "name": "Check 10 UNKNOWN upgrade",
                "passed": False,
                "detail": f"simulation or analysis exited with status {exc.returncode}",
            }

    summary_dir = REPO_ROOT / "benchmarks" / "Phase2SelfAttestation"
    summary_dir.mkdir(parents=True, exist_ok=True)
    checks = order_checks + [mutation]
    overall = phase1_rc == 0 and all(row["passed"] for row in checks)
    summary_path = summary_dir / "phase2_self_attestation_validation_summary.json"
    summary_path.write_text(json.dumps({
        "checkpoint": "2B",
        "passed": overall,
        "phase1_validation_passed": phase1_rc == 0,
        "golden_reference": str(golden_path),
        "checks": checks,
    }, indent=2, sort_keys=True) + "\n")
    print("\n========== PHASE 2B SELF-ATTESTATION VALIDATION ==========")
    for row in checks:
        print(f"{'PASS' if row['passed'] else 'FAIL'}  {row['name']}: {row['detail']}")
    print(f"Overall: {'PASS' if overall else 'FAIL'}")
    print(f"Machine-readable summary: {summary_path}")
    return 0 if overall else 1


def _phase2_attack_run_dir(
    experiment: str, target: int, b: int, sigma: float, signal_error: float, rep: int
) -> Path:
    sigma_label = format(sigma, "g").replace(".", "p")
    signal_label = format(signal_error, "g").replace(".", "p")
    return (
        REPO_ROOT / "benchmarks" / "Phase2Attacks" / experiment /
        f"target_veh{target}" / f"b_{b}" /
        f"sigma_{sigma_label}_signal_{signal_label}" / f"run_{rep}"
    )


def _phase2_attack_kind(experiment: str, b: int) -> str:
    if b == 0:
        return "NONE"
    return "WRONG_APPROACH" if experiment == "e2" else "FALSE_DIRECTION"


def _phase2_nested_colluders(target: int, b: int, n: int = PHASE2_ATTACK_N) -> List[int]:
    if b <= 1:
        return []
    candidates = [replica for replica in range(n) if replica != target]
    return candidates[:b - 1]


def _run_phase2_attack_cell(
    args: argparse.Namespace,
    *,
    experiment: str,
    b: int,
    sigma: float,
    signal_error: float,
    rep: int,
) -> Path:
    target = args.attack_target
    kind = _phase2_attack_kind(experiment, b)
    colluders = _phase2_nested_colluders(target, b)
    colluder_csv = ",".join(str(value) for value in colluders)
    seed = run_seed(MASTER_SEED, PHASE2_ATTACK_N, f"PHASE2_{experiment.upper()}", rep)
    run_dir = _phase2_attack_run_dir(experiment, target, b, sigma, signal_error, rep)
    if not args.dry_run:
        run_dir.mkdir(parents=True, exist_ok=True)
        (run_dir / "phase2_attack_run_metadata.json").write_text(json.dumps({
            "checkpoint": "2C",
            "experiment": experiment,
            "attack_kind": kind,
            "attack_target_replica_id": target,
            "actual_byzantine_count": b,
            "evidence_colluder_ids": colluders,
            "colluder_policy": "nested_ascending_replica_ids",
            "simulation_seed": seed,
            "approach_sigma_m": sigma,
            "signal_observation_error": signal_error,
            "direction_collection_window_sec": args.direction_collection_window,
            "config": PHASE2_ATTACK_CONFIG,
            "n": PHASE2_ATTACK_N,
            "cue_source": "controlled",
        }, indent=2, sort_keys=True) + "\n")

    argv = [
        str(RUN_SCRIPT), str(FOURWAY_DIR),
        "--compact-log",
        "--channel-metrics-dir", str(run_dir.resolve()),
        "--approach-sigma", str(sigma),
        "--signal-error", str(signal_error),
        "--direction-collection-window", str(args.direction_collection_window),
        "--simulation-seed", str(seed),
        "--phase2-attack-kind", kind,
        "--phase2-attack-target", str(target),
        "--phase2-evidence-colluders", colluder_csv,
        "--phase2-actual-b", str(b),
        "-u", "Cmdenv", "-c", PHASE2_ATTACK_CONFIG,
    ]
    inner = " ".join(shlex.quote(value) for value in argv)
    print(f"+ {inner}")
    run_in_bash_with_omnet(inner, dry_run=args.dry_run)
    if args.dry_run:
        return run_dir
    shutil.copy2(LOG_FILE, run_dir / "raw_simulation.log")
    analyze = [
        sys.executable, str(FOURWAY_DIR / "analyze_log.py"), str(LOG_FILE),
        "--save-to", str(run_dir), "--scenario", "1", "--cars", str(PHASE2_ATTACK_N),
        "--run-index", str(rep), "--no-scenario-subdir",
    ]
    print("+ " + " ".join(shlex.quote(value) for value in analyze))
    subprocess.run(analyze, cwd=REPO_ROOT, check=True)
    return run_dir


def _load_phase2_attack_summary(run_dir: Path, rep: int) -> Dict[str, Any]:
    records = json.loads((run_dir / f"{PHASE2_ATTACK_N}veh_{rep}.json").read_text())
    return records[0]["bft_stats"]["perception"]["phase2_attack"]


def _validate_phase2_attack_cell(
    *, experiment: str, b: int, sigma: float, signal_error: float,
    target: int, rep: int, run_dir: Path,
) -> Dict[str, Any]:
    checks: List[Dict[str, Any]] = []

    def check(name: str, passed: bool, detail: str) -> None:
        checks.append({"name": name, "passed": bool(passed), "detail": detail})

    text = (run_dir / "raw_simulation.log").read_text(errors="replace")
    summary = _load_phase2_attack_summary(run_dir, rep)
    expected_kind = _phase2_attack_kind(experiment, b)
    expected_colluders = _phase2_nested_colluders(target, b)
    expected_byzantine = [] if b == 0 else [target, *expected_colluders]
    check(
        "consistent attack configuration",
        summary["kind"] == expected_kind and summary["actual_b"] == b and
            summary["colluder_ids"] == expected_colluders and
            summary["configured_byzantine_ids"] == expected_byzantine and
            summary["config_replica_count"] == PHASE2_ATTACK_N and
            summary["config_mismatch_count"] == 0,
        f"kind={summary['kind']} b={summary['actual_b']} byz={summary['configured_byzantine_ids']} replicas={summary['config_replica_count']}",
    )
    declaration = summary.get("declaration")
    if b == 0:
        check("honest b=0 control", declaration is None,
              f"declaration={declaration}")
    elif experiment == "e2":
        check(
            "E2 declaration",
            declaration == {
                "kind": "WRONG_APPROACH", "actual_lane": "N", "claimed_lane": "E",
                "actual_direction": "S", "claimed_direction": "S",
            },
            f"declaration={declaration}",
        )
    else:
        check(
            "E4 declaration",
            declaration == {
                "kind": "FALSE_DIRECTION", "actual_lane": "N", "claimed_lane": "N",
                "actual_direction": "S", "claimed_direction": "R",
            },
            f"declaration={declaration}",
        )
    self_rows = re.findall(
        rf"\[SELF-ATTEST\] target=veh{target} epoch=0 signer={target} .* status=VALID", text
    )
    check("one target self-attestation", len(self_rows) == 1,
          f"records={len(self_rows)}")
    measured_colluders = set(summary["collusion_echo_signers"])
    check(
        "collusion echoes come only from configured nested set",
        measured_colluders.issubset(expected_colluders),
        f"configured={expected_colluders} measured={sorted(measured_colluders)}",
    )
    h_lane_ids = set(summary["h_lane_signers"])
    h_dir_ids = set(summary["h_dir_signers"])
    check(
        "honest h excludes Byzantine identities",
        not h_lane_ids.intersection(expected_byzantine) and
            not h_dir_ids.intersection(expected_byzantine),
        f"h_lane={sorted(h_lane_ids)} h_dir={sorted(h_dir_ids)} byz={expected_byzantine}",
    )
    cert_signers = set(summary["certificate_signers"])
    check(
        "target signer occurs at most once",
        summary["self_attestation_count"] in (0, 1) and
            (target in cert_signers) == (summary["self_attestation_count"] == 1),
        f"self={summary['self_attestation_count']} cert_signers={sorted(cert_signers)}",
    )
    check(
        "authenticated evidence accepted",
        "[ANN-ORIGIN-INVALID]" not in text and "verify FAIL" not in text and
            "[CERT-INVALID]" not in text,
        "no origin, echo-signature, or certificate validation failures",
    )
    if summary["lane_certified"]:
        expected_lane_bsig = len(cert_signers.intersection(expected_byzantine))
        expected_dir_bsig = len({
            signer for signer in cert_signers.intersection(expected_byzantine)
            if signer == target or signer in expected_colluders
        })
        check(
            "b_sig matches certificate bytes",
            summary["b_sig_lane"] == expected_lane_bsig and
                summary["b_sig_dir"] == expected_dir_bsig,
            f"lane={summary['b_sig_lane']}/{expected_lane_bsig} dir={summary['b_sig_dir']}/{expected_dir_bsig}",
        )
    else:
        check("no false b_sig without certificate", summary["b_sig_lane"] == 0,
              f"b_sig_lane={summary['b_sig_lane']}")
    outcome = summary.get("outcome") or {}
    if signal_error == 0.0 and sigma == 0.0 and b <= bft_f(PHASE2_ATTACK_N):
        false_outcome = bool(outcome.get("false_lane_certificate") or outcome.get("false_eligibility"))
        check("zero error b<=f blocks false outcome", not false_outcome,
              f"outcome={outcome}")
    if b == bft_f(PHASE2_ATTACK_N) + 1:
        channel_b_sig = (
            summary["b_sig_lane"] if experiment == "e2" else summary["b_sig_dir"]
        )
        crossed = (
            outcome.get("false_lane_certificate") if experiment == "e2"
            else outcome.get("false_eligibility")
        )
        if channel_b_sig >= summary["threshold"]:
            check(
                "measured b_sig=f+1 crosses without honest support",
                crossed is True and summary["required_honest_support"] == 0,
                f"b_sig={channel_b_sig} outcome={outcome} required_honest={summary['required_honest_support']}",
            )
        else:
            check(
                "partial Byzantine participation uses measured b_sig",
                summary["required_honest_support"] == summary["threshold"] - channel_b_sig,
                f"configured_b={b} b_sig={channel_b_sig} threshold={summary['threshold']} "
                f"required_honest={summary['required_honest_support']} outcome={outcome}",
            )
    if experiment == "e4":
        check("E4 lane certificate remains independent", summary["lane_certified"] is True,
              f"lane_certified={summary['lane_certified']}")
    departed = set(re.findall(r"\[DEPARTED\] Replica (\d+)\b", text))
    check("vehicles departed", len(departed) == PHASE2_ATTACK_N,
          f"departed={len(departed)}/{PHASE2_ATTACK_N}")
    failures = [row for row in checks if not row["passed"]]
    return {
        "experiment": experiment, "b": b, "sigma": sigma,
        "signal_error": signal_error, "rep": rep, "result_dir": str(run_dir),
        "passed": not failures, "checks": checks, "failures": failures,
        "accounting": summary,
    }


def run_phase2_attack_experiments(
    args: argparse.Namespace, repetitions: int, *, validation: bool
) -> int:
    experiments = ("e2", "e4") if validation else (args.experiment,)
    if validation:
        cells = [
            (experiment, b,
             1.0 if experiment == "e2" and nonzero else 0.0,
             0.20 if experiment == "e4" and nonzero else 0.0)
            for experiment in experiments
            for nonzero in (False, True)
            for b in range(0, bft_f(PHASE2_ATTACK_N) + 2)
        ]
    else:
        cells = [(args.experiment, args.inject_b, args.approach_sigma, args.signal_error)]

    if args.dry_run:
        print(f"[dry-run] would run_key_generation({PHASE2_ATTACK_N})")
    else:
        run_key_generation(dry_run=False, scale=PHASE2_ATTACK_N)
    results: List[Dict[str, Any]] = []
    for experiment, b, sigma, signal_error in cells:
        for rep in range(args.start_rep, args.start_rep + repetitions):
            print(f"\n--- Phase 2C {experiment.upper()} b={b} sigma={sigma:g} signal={signal_error:g} run_{rep} ---")
            if args.dry_run:
                print("[dry-run] would clear_stale_random_ini()")
            else:
                clear_stale_random_ini()
            try:
                run_dir = _run_phase2_attack_cell(
                    args, experiment=experiment, b=b, sigma=sigma,
                    signal_error=signal_error, rep=rep,
                )
                if args.dry_run:
                    continue
                result = _validate_phase2_attack_cell(
                    experiment=experiment, b=b, sigma=sigma,
                    signal_error=signal_error, target=args.attack_target,
                    rep=rep, run_dir=run_dir,
                )
            except (subprocess.CalledProcessError, OSError, ValueError, KeyError) as exc:
                result = {
                    "experiment": experiment, "b": b, "sigma": sigma,
                    "signal_error": signal_error, "rep": rep,
                    "passed": False, "checks": [],
                    "failures": [{"name": "simulation/analysis", "passed": False, "detail": str(exc)}],
                }
            results.append(result)
            print(f"[{'PASS' if result['passed'] else 'FAIL'}] {experiment} b={b} sigma={sigma:g} signal={signal_error:g}")
            for row in result.get("checks", []):
                print(f"  {'PASS' if row['passed'] else 'FAIL':4} {row['name']}: {row['detail']}")

    if args.dry_run:
        return 0
    summary_dir = REPO_ROOT / "benchmarks" / "Phase2Attacks"
    summary_dir.mkdir(parents=True, exist_ok=True)
    overall = bool(results) and all(row["passed"] for row in results)
    summary_path = summary_dir / (
        "phase2_attack_validation_summary.json" if validation
        else f"phase2_{args.experiment}_summary.json"
    )
    summary_path.write_text(json.dumps({
        "checkpoint": "2C", "validation": validation, "passed": overall,
        "colluder_policy": "nested_ascending_replica_ids",
        "nonzero_smoke": {"e2_sigma_m": 1.0, "e4_signal_error": 0.20},
        "results": results,
    }, indent=2, sort_keys=True) + "\n")
    print("\n========== PHASE 2C ATTACK VALIDATION SUMMARY ==========")
    print(f"Overall: {'PASS' if overall else 'FAIL'}")
    print(f"Machine-readable summary: {summary_path}")
    return 0 if overall else 1


def _phase2_pilot_manifest(
    honest_n: int = PHASE2_FIXTURE_N, *, profile: str = "full"
) -> List[Dict[str, Any]]:
    """Return the deduplicated, reviewable Phase 2 pilot matrix."""
    if profile not in ("grid", "full"):
        raise ValueError(f"unsupported Phase 2 pilot profile: {profile}")
    cells: Dict[Tuple[Any, ...], Dict[str, Any]] = {}

    def add(experiment: str, *, n: int, b: int, sigma: float,
            signal_error: float, rep: int, purpose: str) -> None:
        key = (experiment, n, b, sigma, signal_error, rep)
        if key not in cells:
            cells[key] = {
                "experiment": experiment, "n": n, "b": b,
                "sigma": sigma, "signal_error": signal_error, "rep": rep,
                "purposes": [],
            }
        if purpose not in cells[key]["purposes"]:
            cells[key]["purposes"].append(purpose)

    honest_repetitions = 1 if profile == "grid" else 10
    for sigma in PHASE2_SIGMAS:
        for rep in range(honest_repetitions):
            add("e1", n=honest_n, b=0, sigma=sigma, signal_error=0.0,
                rep=rep, purpose="honest_sigma")

    f = bft_f(PHASE2_ATTACK_N)
    for b in range(f + 2):
        for sigma in PHASE2_SIGMAS:
            add("e2", n=PHASE2_ATTACK_N, b=b, sigma=sigma, signal_error=0.0,
                rep=0, purpose="full_grid")
    if profile == "full":
        for sigma in PHASE2_SIGMAS[1:]:
            for rep in range(20):
                add("e2", n=PHASE2_ATTACK_N, b=f, sigma=sigma, signal_error=0.0,
                    rep=rep, purpose="shoulder_b_f")
        for rep in range(5):
            add("e2", n=PHASE2_ATTACK_N, b=f + 1, sigma=1.0, signal_error=0.0,
                rep=rep, purpose="boundary_b_f_plus_1")
        for rep in range(10):
            add("e2", n=PHASE2_ATTACK_N, b=f - 1, sigma=1.0, signal_error=0.0,
                rep=rep, purpose="boundary_b_f_minus_1")

    for signal_error in PHASE2_SIGNAL_ERRORS:
        for rep in range(honest_repetitions):
            add("e3", n=PHASE2_FIXTURE_N, b=0, sigma=0.0,
                signal_error=signal_error, rep=rep, purpose="honest_signal")

    for b in range(f + 2):
        for signal_error in PHASE2_SIGNAL_ERRORS:
            add("e4", n=PHASE2_ATTACK_N, b=b, sigma=0.0,
                signal_error=signal_error, rep=0, purpose="full_grid")
    if profile == "full":
        for signal_error in PHASE2_SIGNAL_ERRORS[1:]:
            for rep in range(20):
                add("e4", n=PHASE2_ATTACK_N, b=f, sigma=0.0,
                    signal_error=signal_error, rep=rep, purpose="shoulder_b_f")
        for rep in range(5):
            add("e4", n=PHASE2_ATTACK_N, b=f + 1, sigma=0.0, signal_error=0.20,
                rep=rep, purpose="boundary_b_f_plus_1")
        for rep in range(10):
            add("e4", n=PHASE2_ATTACK_N, b=f - 1, sigma=0.0, signal_error=0.20,
                rep=rep, purpose="boundary_b_f_minus_1")

    order = {"e1": 0, "e2": 1, "e3": 2, "e4": 3}
    return sorted(cells.values(), key=lambda row: (
        order[row["experiment"]], row["b"], row["sigma"],
        row["signal_error"], row["rep"],
    ))


def _phase2_pilot_run_dir(cell: Dict[str, Any]) -> Path:
    sigma = format(cell["sigma"], "g").replace(".", "p")
    signal = format(cell["signal_error"], "g").replace(".", "p")
    base = REPO_ROOT / "benchmarks" / "Phase2Pilots" / cell["experiment"]
    if cell["experiment"] in ("e2", "e4"):
        base = base / f"target_veh{PHASE2_ATTACK_TARGET_DEFAULT}" / f"b_{cell['b']}"
    else:
        base = base / f"n_{cell['n']}"
    return base / f"sigma_{sigma}_signal_{signal}" / f"run_{cell['rep']}"


def _phase2_pilot_json_path(cell: Dict[str, Any], run_dir: Path) -> Path:
    return run_dir / f"{cell['n']}veh_{cell['rep']}.json"


def _run_phase2_pilot_cell(args: argparse.Namespace, cell: Dict[str, Any]) -> Path:
    run_dir = _phase2_pilot_run_dir(cell)
    json_path = _phase2_pilot_json_path(cell, run_dir)
    metadata_path = run_dir / "phase2_pilot_run_metadata.json"
    if json_path.is_file() and metadata_path.is_file() and (run_dir / "raw_simulation.log").is_file():
        print(f"[resume] reuse {run_dir}")
        return run_dir

    experiment = cell["experiment"]
    seed = run_seed(MASTER_SEED, cell["n"], f"PHASE2_{experiment.upper()}", cell["rep"])
    config = PHASE2_FIXTURE_CONFIG if experiment == "e3" else omnet_config_name(cell["n"])
    kind = _phase2_attack_kind(experiment, cell["b"]) if experiment in ("e2", "e4") else "NONE"
    colluders = (
        _phase2_nested_colluders(PHASE2_ATTACK_TARGET_DEFAULT, cell["b"], cell["n"])
        if experiment in ("e2", "e4") else []
    )
    run_dir.mkdir(parents=True, exist_ok=True)
    metadata_path.write_text(json.dumps({
        "checkpoint": "2D-pilots", **cell,
        "attack_kind": kind,
        "attack_target_replica_id": (
            PHASE2_ATTACK_TARGET_DEFAULT if experiment in ("e2", "e4") else None
        ),
        "evidence_colluder_ids": colluders,
        "colluder_policy": "nested_ascending_replica_ids",
        "simulation_seed": seed,
        "direction_collection_window_sec": args.direction_collection_window,
        "config": config,
        "cue_source": "controlled" if experiment in ("e3", "e4") else "sumo_native_off",
        "k": 1,
    }, indent=2, sort_keys=True) + "\n")

    argv = [
        str(RUN_SCRIPT), str(FOURWAY_DIR),
        "--compact-log",
        "--channel-metrics-dir", str(run_dir.resolve()),
        "--approach-sigma", str(cell["sigma"]),
        "--signal-error", str(cell["signal_error"]),
        "--direction-collection-window", str(args.direction_collection_window),
        "--simulation-seed", str(seed),
    ]
    if experiment in ("e2", "e4"):
        argv.extend([
            "--phase2-attack-kind", kind,
            "--phase2-attack-target", str(PHASE2_ATTACK_TARGET_DEFAULT),
            "--phase2-evidence-colluders", ",".join(str(value) for value in colluders),
            "--phase2-actual-b", str(cell["b"]),
        ])
    argv.extend(["-u", "Cmdenv", "-c", config])
    inner = " ".join(shlex.quote(value) for value in argv)
    print(f"+ {inner}")
    run_in_bash_with_omnet(inner, dry_run=args.dry_run)
    if args.dry_run:
        return run_dir
    shutil.copy2(LOG_FILE, run_dir / "raw_simulation.log")
    analyze = [
        sys.executable, str(FOURWAY_DIR / "analyze_log.py"), str(LOG_FILE),
        "--save-to", str(run_dir), "--scenario", "1", "--cars", str(cell["n"]),
        "--run-index", str(cell["rep"]), "--no-scenario-subdir",
    ]
    print("+ " + " ".join(shlex.quote(value) for value in analyze))
    subprocess.run(analyze, cwd=REPO_ROOT, check=True)
    return run_dir


def _wilson_interval(successes: int, trials: int, z: float = 1.959963984540054) -> Dict[str, Any]:
    if trials <= 0:
        return {"successes": successes, "trials": trials, "rate": None, "low": None, "high": None}
    p = successes / trials
    denominator = 1.0 + z * z / trials
    center = (p + z * z / (2.0 * trials)) / denominator
    half = z * math.sqrt(p * (1.0 - p) / trials + z * z / (4.0 * trials * trials)) / denominator
    return {"successes": successes, "trials": trials, "rate": p,
            "low": max(0.0, center - half), "high": min(1.0, center + half)}


def _mean_or_none(values: Sequence[float | None]) -> float | None:
    usable = [float(value) for value in values if value is not None]
    return statistics.mean(usable) if usable else None


def _phase2_pilot_run_record(cell: Dict[str, Any], run_dir: Path) -> Dict[str, Any]:
    records = json.loads(_phase2_pilot_json_path(cell, run_dir).read_text())
    if not records:
        raise ValueError(f"empty analyzer output in {run_dir}")
    first = records[0]
    perception = first["bft_stats"]["perception"]
    metrology = first["bft_stats"]["movement_metrology"]
    run_metrics = first.get("run_metrics", {})
    attack = perception.get("phase2_attack") or {}
    waits_ms = [row["durations_ms"].get("total_wait_time") for row in records]
    waits_s = [value / 1000.0 for value in waits_ms if value is not None]
    batch_counts: Dict[int, int] = {}
    for row in records:
        batch = row["bft_stats"].get("batch_index")
        if batch is not None:
            batch_counts[int(batch)] = batch_counts.get(int(batch), 0) + 1
    batch_sizes = list(batch_counts.values())
    outcome = attack.get("outcome") or {}
    false_outcome = bool(
        outcome.get("false_lane_certificate") if cell["experiment"] == "e2"
        else outcome.get("false_eligibility") if cell["experiment"] == "e4"
        else False
    )
    direction_rows = perception.get("direction_eligibility", {})
    unlock_count = sum(1 for row in direction_rows.values() if row.get("derived") != "U")
    cert_latencies = [
        row["bft_stats"].get("cert_creation_latency_ms") for row in records
        if row["bft_stats"].get("cert_creation_latency_ms") is not None
    ]
    return {
        **cell, "run_dir": str(run_dir),
        "simulation_completed": len(records) == cell["n"] and len(waits_s) == cell["n"],
        "single_evaluation_invariant_ok": perception.get("single_evaluation_invariant_ok", False),
        "self_attestation_invariant_ok": perception.get("self_attestation_invariant_ok", False),
        "lane_evaluations": perception["lane_evaluation_count"],
        "lane_accepts": perception["lane_accept_count"],
        "cue_supports": perception.get("cue_support_count", 0),
        "cue_support_opportunities": perception.get("cue_support_opportunities", 0),
        "certificate_count": len(perception.get("certificates", {})),
        "direction_unlock_count": unlock_count,
        "direction_decision_count": len(direction_rows),
        "quiet_count": perception.get("quiet_count", 0),
        "signed_unknown_count": perception.get("signed_unknown_count", 0),
        "signed_direction_count": perception.get("signed_direction_count", 0),
        "left_table_forced_singleton_count": perception.get("left_table_forced_singleton_count", 0),
        "signed_unknown_singleton_count": perception.get("signed_unknown_singleton_count", 0),
        "batch_count": len(batch_sizes),
        "mean_batch_size": statistics.mean(batch_sizes) if batch_sizes else None,
        "scheduler_singleton_vehicle_count": sum(value for value in batch_sizes if value == 1),
        "throughput_veh_per_s": run_metrics.get("throughput_veh_per_s"),
        "mean_wait_s": statistics.mean(waits_s) if waits_s else None,
        "p95_wait_s": run_metrics.get("wait_all_s", {}).get("p95") if run_metrics else None,
        "mean_certificate_latency_ms": statistics.mean(cert_latencies) if cert_latencies else None,
        "h_lane": attack.get("h_lane"), "h_dir": attack.get("h_dir"),
        "b_sig_lane": attack.get("b_sig_lane"), "b_sig_dir": attack.get("b_sig_dir"),
        "q0_lane": attack.get("q0_lane"), "q0_dir": attack.get("q0_dir"),
        "binomial_tail_prediction": attack.get("binomial_tail_prediction"),
        "false_outcome": false_outcome,
        "conflicting_cooccupancy_pair_count": metrology["conflicting_cooccupancy_pair_count"],
        "sumo_collision_vehicle_count": metrology["sumo_collision_vehicle_count"],
        "movement_mismatches": metrology["planned_actual_mismatch_vehicles"],
    }


def _aggregate_phase2_pilots(rows: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
    groups: Dict[Tuple[Any, ...], List[Dict[str, Any]]] = {}
    for row in rows:
        key = (row["experiment"], row["n"], row["b"], row["sigma"], row["signal_error"])
        groups.setdefault(key, []).append(row)
    aggregates = []
    for key, group in sorted(groups.items()):
        experiment, n, b, sigma, signal_error = key
        evals = sum(row["lane_evaluations"] for row in group)
        accepts = sum(row["lane_accepts"] for row in group)
        cue_trials = sum(row["cue_support_opportunities"] for row in group)
        cue_supports = sum(row["cue_supports"] for row in group)
        false_successes = sum(bool(row["false_outcome"]) for row in group)
        cert_successes = sum(row["certificate_count"] for row in group)
        cert_trials = len(group) * n
        unlock_successes = sum(row["direction_unlock_count"] for row in group)
        unlock_trials = sum(row["direction_decision_count"] for row in group)
        false_ci = _wilson_interval(false_successes, len(group))
        prediction = _mean_or_none([row["binomial_tail_prediction"] for row in group])
        prediction_in_interval = (
            None if prediction is None else false_ci["low"] <= prediction <= false_ci["high"]
        )
        aggregates.append({
            "experiment": experiment, "n": n, "b": b, "sigma": sigma,
            "signal_error": signal_error, "repetitions": len(group),
            "purposes": sorted({purpose for row in group for purpose in row["purposes"]}),
            "lane_acceptance": _wilson_interval(accepts, evals),
            "cue_support": _wilson_interval(cue_supports, cue_trials),
            "certificate_completion": _wilson_interval(cert_successes, cert_trials),
            "direction_unlock": _wilson_interval(unlock_successes, unlock_trials),
            "false_outcome": false_ci,
            "mean_binomial_tail_prediction": prediction,
            "prediction_in_wilson_interval": prediction_in_interval,
            "mean_h_lane": _mean_or_none([row["h_lane"] for row in group]),
            "mean_h_dir": _mean_or_none([row["h_dir"] for row in group]),
            "mean_b_sig_lane": _mean_or_none([row["b_sig_lane"] for row in group]),
            "mean_b_sig_dir": _mean_or_none([row["b_sig_dir"] for row in group]),
            "mean_batch_size": _mean_or_none([row["mean_batch_size"] for row in group]),
            "mean_throughput_veh_per_s": _mean_or_none([row["throughput_veh_per_s"] for row in group]),
            "mean_certificate_latency_ms": _mean_or_none([row["mean_certificate_latency_ms"] for row in group]),
            "mean_wait_s": _mean_or_none([row["mean_wait_s"] for row in group]),
            "mean_p95_wait_s": _mean_or_none([row["p95_wait_s"] for row in group]),
            "quiet_count": sum(row["quiet_count"] for row in group),
            "signed_unknown_count": sum(row["signed_unknown_count"] for row in group),
            "signed_direction_count": sum(row["signed_direction_count"] for row in group),
            "left_table_forced_singleton_count": sum(row["left_table_forced_singleton_count"] for row in group),
            "scheduler_singleton_vehicle_count": sum(row["scheduler_singleton_vehicle_count"] for row in group),
            "conflicting_cooccupancy_pair_count": sum(row["conflicting_cooccupancy_pair_count"] for row in group),
            "sumo_collision_vehicle_count": sum(row["sumo_collision_vehicle_count"] for row in group),
            "movement_mismatch_count": sum(len(row["movement_mismatches"]) for row in group),
        })
    return aggregates


def run_phase2_pilots(args: argparse.Namespace) -> int:
    manifest = _phase2_pilot_manifest(
        args.phase2_pilot_honest_n, profile=args.phase2_pilot_profile
    )
    output_dir = REPO_ROOT / "benchmarks" / "Phase2Pilots"
    manifest_path = output_dir / "phase2_pilot_manifest.json"
    if args.dry_run:
        counts = {name: sum(row["experiment"] == name for row in manifest) for name in ("e1", "e2", "e3", "e4")}
        print(f"[dry-run] Phase 2 pilot unique runs={len(manifest)} counts={counts}")
        for row in manifest:
            print(f"[dry-run] {row}")
        return 0

    output_dir.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(json.dumps({
        "checkpoint": "2D-pilots", "profile": args.phase2_pilot_profile,
        "honest_e1_n": args.phase2_pilot_honest_n,
        "collection_window_sec": args.direction_collection_window,
        "k": 1, "unique_run_count": len(manifest), "runs": manifest,
    }, indent=2, sort_keys=True) + "\n")
    rows: List[Dict[str, Any]] = []
    current_n = None
    failures = []
    for index, cell in enumerate(manifest, 1):
        if current_n != cell["n"]:
            run_key_generation(dry_run=False, scale=cell["n"])
            current_n = cell["n"]
        print(f"\n--- Phase 2 pilot {index}/{len(manifest)} {cell} ---")
        clear_stale_random_ini()
        try:
            run_dir = _run_phase2_pilot_cell(args, cell)
            row = _phase2_pilot_run_record(cell, run_dir)
            rows.append(row)
            if not row["simulation_completed"]:
                failures.append({**cell, "error": "not all vehicles produced departure wait metrics"})
            if row["movement_mismatches"]:
                failures.append({**cell, "error": f"planned/actual movement mismatch: {row['movement_mismatches']}"})
            if not row["single_evaluation_invariant_ok"]:
                failures.append({**cell, "error": "duplicate perception evaluation detected"})
            if not row["self_attestation_invariant_ok"]:
                failures.append({**cell, "error": "self-attestation invariant failed"})
            if (cell["experiment"] in ("e2", "e4") and
                    cell["sigma"] == 0.0 and cell["signal_error"] == 0.0 and
                    cell["b"] <= bft_f(cell["n"]) and row["false_outcome"]):
                failures.append({**cell, "error": "zero-error b<=f produced a false outcome"})
        except (subprocess.CalledProcessError, OSError, ValueError, KeyError) as exc:
            failures.append({**cell, "error": str(exc)})
            print(f"[FAIL] {cell}: {exc}")

    aggregates = _aggregate_phase2_pilots(rows)
    theory_checks = [
        {
            "experiment": row["experiment"], "b": row["b"], "sigma": row["sigma"],
            "signal_error": row["signal_error"], "repetitions": row["repetitions"],
            "prediction": row["mean_binomial_tail_prediction"],
            "wilson_low": row["false_outcome"]["low"],
            "wilson_high": row["false_outcome"]["high"],
            "consistent": row["prediction_in_wilson_interval"],
        }
        for row in aggregates
        if row["experiment"] in ("e2", "e4") and row["repetitions"] >= 10 and
           row["mean_binomial_tail_prediction"] is not None
    ]
    theory_consistent = all(row["consistent"] for row in theory_checks)
    summary = {
        "checkpoint": "2D-pilots", "profile": args.phase2_pilot_profile,
        "passed": not failures and len(rows) == len(manifest) and theory_consistent,
        "honest_e1_n": args.phase2_pilot_honest_n,
        "collection_window_sec": args.direction_collection_window, "k": 1,
        "planned_unique_runs": len(manifest), "completed_unique_runs": len(rows),
        "failures": failures, "theory_checks": theory_checks,
        "theory_consistent": theory_consistent, "runs": rows, "cells": aggregates,
        "metrology_validation": str(REPO_ROOT / "benchmarks/Phase2Metrology/phase2_metrology_validation.json"),
    }
    (output_dir / "phase2_pilot_summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n"
    )
    run_fields = [key for key in rows[0] if key != "purposes"] if rows else []
    if rows:
        with (output_dir / "phase2_pilot_runs.csv").open("w", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=[*run_fields, "purposes"])
            writer.writeheader()
            for row in rows:
                writer.writerow({**row, "purposes": ";".join(row["purposes"])})
    print("\n========== PHASE 2 PILOT SUMMARY ==========")
    print(f"Completed: {len(rows)}/{len(manifest)}")
    print(f"Overall: {'PASS' if summary['passed'] else 'FAIL'}")
    print(f"Summary: {output_dir / 'phase2_pilot_summary.json'}")
    return 0 if summary["passed"] else 1


def _run_phase2_metrology_simulation(
    args: argparse.Namespace, *, name: str, config: str, cars: int, perception: bool,
) -> tuple[Path, Dict[str, Any]]:
    run_dir = REPO_ROOT / "benchmarks" / "Phase2Metrology" / name
    if not args.dry_run:
        run_dir.mkdir(parents=True, exist_ok=True)
    argv = [
        str(RUN_SCRIPT), str(FOURWAY_DIR),
        "--channel-metrics-dir", str(run_dir.resolve()),
    ]
    if perception:
        seed = run_seed(MASTER_SEED, cars, "PHASE2_METROLOGY", args.start_rep)
        argv.extend([
            "--approach-sigma", "0",
            "--signal-error", "0",
            "--direction-collection-window", str(args.direction_collection_window),
            "--simulation-seed", str(seed),
        ])
    argv.extend(["-u", "Cmdenv", "-c", config])
    inner = " ".join(shlex.quote(value) for value in argv)
    print(f"+ {inner}")
    run_in_bash_with_omnet(inner, dry_run=args.dry_run)
    if args.dry_run:
        return run_dir, {}
    shutil.copy2(LOG_FILE, run_dir / "raw_simulation.log")
    analyze = [
        sys.executable, str(FOURWAY_DIR / "analyze_log.py"), str(LOG_FILE),
        "--save-to", str(run_dir), "--scenario", "1", "--cars", str(cars),
        "--run-index", str(args.start_rep), "--no-scenario-subdir",
    ]
    print("+ " + " ".join(shlex.quote(value) for value in analyze))
    subprocess.run(analyze, cwd=REPO_ROOT, check=True)
    records = json.loads((run_dir / f"{cars}veh_{args.start_rep}.json").read_text())
    return run_dir, records[0]["bft_stats"]["movement_metrology"]


def run_phase2_metrology_validation(args: argparse.Namespace) -> int:
    summary_dir = REPO_ROOT / "benchmarks" / "Phase2Metrology"
    table_source = FOURWAY_DIR / "phase2_movement_conflicts.csv"
    table_artifact = summary_dir / "phase2_movement_conflicts.csv"
    summary_path = summary_dir / "phase2_metrology_validation.json"
    trace_path = summary_dir / "phase2_calibration_trace.csv"
    generator = [sys.executable, str(FOURWAY_DIR / "generate_phase2_movement_conflicts.py")]
    audit = [sys.executable, str(FOURWAY_DIR / "audit_ksafe_geometry.py")]
    if args.dry_run:
        print("+ " + " ".join(shlex.quote(value) for value in generator))
        print("+ " + " ".join(shlex.quote(value) for value in audit))
        print(f"[dry-run] would run_key_generation({PHASE2_FIXTURE_N})")
        for name, config, cars, perception in (
            ("mixed_fixture", PHASE2_FIXTURE_CONFIG, PHASE2_FIXTURE_N, True),
            ("conflicting", "Phase2MetrologyConflicting", 2, False),
            ("nonconflicting", "Phase2MetrologyNonConflicting", 2, False),
        ):
            print(f"\n--- Phase 2D {name} ---")
            _run_phase2_metrology_simulation(
                args, name=name, config=config, cars=cars, perception=perception,
            )
        return 0

    summary_dir.mkdir(parents=True, exist_ok=True)
    subprocess.run(generator, cwd=REPO_ROOT, check=True)
    subprocess.run(audit, cwd=REPO_ROOT, check=True)
    shutil.copy2(table_source, table_artifact)
    run_key_generation(dry_run=False, scale=PHASE2_FIXTURE_N)

    cases: Dict[str, Dict[str, Any]] = {}
    for name, config, cars, perception in (
        ("mixed_fixture", PHASE2_FIXTURE_CONFIG, PHASE2_FIXTURE_N, True),
        ("conflicting", "Phase2MetrologyConflicting", 2, False),
        ("nonconflicting", "Phase2MetrologyNonConflicting", 2, False),
    ):
        print(f"\n--- Phase 2D {name} ---")
        clear_stale_random_ini()
        run_dir, metrology = _run_phase2_metrology_simulation(
            args, name=name, config=config, cars=cars, perception=perception,
        )
        cases[name] = {
            "run_dir": str(run_dir),
            "metrology": metrology,
            "raw_text": (run_dir / "raw_simulation.log").read_text(errors="replace"),
        }

    with table_source.open(newline="") as handle:
        table_rows = list(csv.DictReader(handle))
    table = {
        (row["movement_a"], row["movement_b"]): bool(int(row["conflict"]))
        for row in table_rows
    }
    movement_names = sorted({row["movement_a"] for row in table_rows})
    manifest_rows = list(csv.DictReader((FOURWAY_DIR / "phase2_mixed_route_manifest.csv").open()))
    expected_fixture = {
        row["vehicle_id"]: {
            "planned_ingress": row["ingress_edge"],
            "planned_egress": row["egress_edge"],
            "movement": f"{row['intended_lane']}-{row['intended_direction']}",
        }
        for row in manifest_rows
    }
    checks: List[Dict[str, Any]] = []

    def check(name: str, passed: bool, detail: str) -> None:
        checks.append({"name": name, "passed": bool(passed), "detail": detail})

    check("12x12 table shape", len(table_rows) == 144 and len(movement_names) == 12,
          f"rows={len(table_rows)} movements={len(movement_names)}")
    symmetric = all(table[(a, b)] == table[(b, a)] for a in movement_names for b in movement_names)
    diagonal = all(table[(movement, movement)] for movement in movement_names)
    check("conflict table symmetric with conflicting diagonal", symmetric and diagonal,
          f"symmetric={symmetric} diagonal={diagonal}")
    audit_json = json.loads((FOURWAY_DIR / "phase2_ksafe_geometry_audit.json").read_text())
    check("kSafe geometry audit", audit_json["passed"] and not audit_json["unsafe_ksafe_entries"],
          f"counts={audit_json['counts']}")

    mixed = cases["mixed_fixture"]["metrology"]
    measured_fixture = {
        vehicle: {
            "planned_ingress": row["planned_ingress"],
            "planned_egress": row["planned_egress"],
            "movement": row["movement"],
        }
        for vehicle, row in mixed["ground_truth"].items()
    }
    check("all mixed-fixture movements observed", measured_fixture == expected_fixture,
          f"observed={len(measured_fixture)}/{PHASE2_FIXTURE_N}")
    actual = mixed["actual_egress"]
    check("all 12 planned and actual egresses agree",
          len(actual) == PHASE2_FIXTURE_N and all(row["match"] for row in actual.values()),
          f"observed={len(actual)}/{PHASE2_FIXTURE_N} mismatches={mixed['planned_actual_mismatch_vehicles']}")
    entries = mixed["entry_count_by_vehicle"]
    exits = mixed["exit_count_by_vehicle"]
    check("all fixture vehicles enter and exit conflict zone exactly once",
          len(entries) == PHASE2_FIXTURE_N and len(exits) == PHASE2_FIXTURE_N and
          all(value == 1 for value in entries.values()) and all(value == 1 for value in exits.values()),
          f"entries={len(entries)} exits={len(exits)}")

    expected_config = {
        "speed_mode": 0, "speed_mps": 14.0,
        "jm_ignore_foe_probability": 1.0,
        "collision_check_junctions": True,
        "collision_action": "none", "time_to_teleport_s": -1.0,
        "vehicles": ["veh0", "veh1"],
    }
    for name, expected_movements, expect_conflict in (
        ("conflicting", {"veh0": "N-S", "veh1": "E-S"}, True),
        ("nonconflicting", {"veh0": "N-S", "veh1": "S-S"}, False),
    ):
        result = cases[name]["metrology"]
        movements = {
            vehicle: row["movement"] for vehicle, row in result["ground_truth"].items()
        }
        check(f"{name} movement pair", movements == expected_movements,
              f"measured={movements}")
        check(f"{name} simultaneous conflict-zone occupancy",
              all(result["entry_count_by_vehicle"].get(vehicle) == 1 for vehicle in expected_movements),
              f"entries={result['entry_count_by_vehicle']}")
        conflict_count = result["conflicting_cooccupancy_pair_count"]
        check(f"{name} classifier outcome",
              (conflict_count > 0) == expect_conflict,
              f"expected_conflict={expect_conflict} pair_count={conflict_count}")
        check(f"{name} pinned SUMO/TraCI settings",
              result["calibration_config"] == expected_config and
              set(result["calibration_releases"]) == {"veh0", "veh1"},
              f"config={result['calibration_config']} releases={sorted(result['calibration_releases'])}")
        check(f"{name} actual egress agreement",
              len(result["actual_egress"]) == 2 and
              all(row["match"] for row in result["actual_egress"].values()),
              f"actual={result['actual_egress']}")
        check(f"{name} no teleportation",
              "Teleporting vehicle" not in cases[name]["raw_text"],
              "no SUMO teleport records")

    trace_fields = [
        "case", "record", "vehicle", "movement", "event", "road", "lane", "time_ms",
        "planned_ingress", "planned_egress", "actual_egress", "match",
        "other_vehicle", "other_movement",
    ]
    with trace_path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=trace_fields)
        writer.writeheader()
        for case_name in ("conflicting", "nonconflicting"):
            result = cases[case_name]["metrology"]
            for row in result["ground_truth"].values():
                writer.writerow({
                    "case": case_name,
                    "record": "ground_truth",
                    "vehicle": row["vehicle"],
                    "movement": row["movement"],
                    "time_ms": row["observed_at_ms"],
                    "planned_ingress": row["planned_ingress"],
                    "planned_egress": row["planned_egress"],
                })
            for row in result["conflict_zone_events"]:
                writer.writerow({"case": case_name, "record": "conflict_zone", **row})
            for row in result["actual_egress"].values():
                movement = result["ground_truth"][row["vehicle"]]["movement"]
                writer.writerow({
                    "case": case_name,
                    "record": "actual_egress",
                    "vehicle": row["vehicle"],
                    "movement": movement,
                    "time_ms": row["observed_at_ms"],
                    "planned_egress": row["planned_egress"],
                    "actual_egress": row["actual_egress"],
                    "match": row["match"],
                })
            for row in result["conflicting_cooccupancy_pairs"]:
                writer.writerow({
                    "case": case_name, "record": "conflicting_cooccupancy",
                    "vehicle": row["first"], "movement": row["first_movement"],
                    "other_vehicle": row["second"], "other_movement": row["second_movement"],
                    "time_ms": row["time_ms"],
                })

    overall = all(row["passed"] for row in checks)
    serializable_cases = {
        name: {"run_dir": row["run_dir"], "metrology": row["metrology"]}
        for name, row in cases.items()
    }
    summary_path.write_text(json.dumps({
        "checkpoint": "2D-metrology", "passed": overall,
        "conflict_table": str(table_artifact),
        "calibration_trace": str(trace_path),
        "checks": checks, "cases": serializable_cases,
        "sumo_collisions_are_secondary": True,
        "statistical_pilots_started": False,
    }, indent=2, sort_keys=True) + "\n")
    print("\n========== PHASE 2D METROLOGY VALIDATION SUMMARY ==========")
    for row in checks:
        print(f"{'PASS' if row['passed'] else 'FAIL'}  {row['name']}: {row['detail']}")
    print(f"Overall: {'PASS' if overall else 'FAIL'}")
    print(f"Conflict table: {table_artifact}")
    print(f"Calibration trace: {trace_path}")
    print(f"Machine-readable summary: {summary_path}")
    return 0 if overall else 1


def _phase2_fixture_run_dir(rep: int) -> Path:
    return REPO_ROOT / "benchmarks" / "Phase2Fixture" / f"run_{rep}"


def _read_phase2_fixture_manifest() -> Dict[str, Dict[str, Any]]:
    with PHASE2_FIXTURE_MANIFEST.open(newline="") as handle:
        rows = list(csv.DictReader(handle))
    required = {
        "vehicle_id", "node_index", "ingress_edge", "egress_edge",
        "intended_lane", "intended_direction", "expected_signal",
    }
    if not rows or set(rows[0]) != required:
        raise ValueError(
            f"{PHASE2_FIXTURE_MANIFEST} must contain exactly {sorted(required)}"
        )
    manifest: Dict[str, Dict[str, Any]] = {}
    for row in rows:
        vehicle = row["vehicle_id"]
        if vehicle in manifest:
            raise ValueError(f"duplicate vehicle {vehicle} in fixture manifest")
        row["node_index"] = int(row["node_index"])
        manifest[vehicle] = row
    if len(manifest) != PHASE2_FIXTURE_N:
        raise ValueError(
            f"fixture manifest must contain {PHASE2_FIXTURE_N} vehicles, got {len(manifest)}"
        )
    return manifest


def _movement_from_edges(ingress: str, egress: str) -> str | None:
    if len(ingress) != 3 or ingress[1:] != "2C":
        return None
    if len(egress) != 3 or egress[:2] != "C2":
        return None
    destination_by_movement = {
        "N": {"S": "S", "E": "L", "W": "R"},
        "S": {"N": "S", "W": "L", "E": "R"},
        "E": {"W": "S", "S": "L", "N": "R"},
        "W": {"E": "S", "N": "L", "S": "R"},
    }
    return destination_by_movement.get(ingress[0], {}).get(egress[2])


def _parse_phase2_cue_trace(log_text: str) -> List[Dict[str, Any]]:
    rows: List[Dict[str, Any]] = []
    for line in log_text.splitlines():
        marker = line.find(PHASE2_CUE_TRACE_PREFIX)
        if marker < 0:
            continue
        fields: Dict[str, str] = {}
        for token in line[marker + len(PHASE2_CUE_TRACE_PREFIX):].strip().split():
            if "=" in token:
                key, value = token.split("=", 1)
                fields[key] = value
        required = {
            "vehicle", "replica", "nodeIndex", "t", "road", "lane",
            "ingress", "egress", "declaredLane", "declaredDirection",
            "signalLeft", "signalRight", "signalEmergency", "derivedCue",
            "cueSource", "echoWindow",
        }
        if not required.issubset(fields):
            continue
        time_text = fields["t"][:-1] if fields["t"].endswith("s") else fields["t"]
        try:
            rows.append({
                "vehicle_id": fields["vehicle"],
                "replica_id": int(fields["replica"]),
                "node_index": int(fields["nodeIndex"]),
                "time_s": float(time_text),
                "road_id": fields["road"],
                "lane_id": fields["lane"],
                "ingress_edge": fields["ingress"],
                "egress_edge": fields["egress"],
                "declared_lane": fields["declaredLane"],
                "declared_direction": fields["declaredDirection"],
                "signal_left": int(fields["signalLeft"]),
                "signal_right": int(fields["signalRight"]),
                "signal_emergency": int(fields["signalEmergency"]),
                "derived_cue": fields["derivedCue"],
                "cue_source": fields["cueSource"],
                "echo_window": int(fields["echoWindow"]),
            })
        except ValueError:
            continue
    return rows


def _parse_phase2_perception_cues(log_text: str) -> Dict[str, List[str]]:
    cues: Dict[str, List[str]] = {}
    pattern = re.compile(
        r"\[PERC-EVAL\].*?target=(veh\d+).*?observedCue=(\S+)\s+knownCueSamples="
    )
    for target, cue in pattern.findall(log_text):
        cues.setdefault(target, []).append(cue)
    return cues


def _validate_phase2_fixture_run(rep: int, run_dir: Path, log_text: str) -> Dict[str, Any]:
    manifest = _read_phase2_fixture_manifest()
    trace = _parse_phase2_cue_trace(log_text)
    perception_cues = _parse_phase2_perception_cues(log_text)
    trace_by_vehicle: Dict[str, List[Dict[str, Any]]] = {
        vehicle: [row for row in trace if row["vehicle_id"] == vehicle]
        for vehicle in manifest
    }
    departed = {
        f"veh{match.group(1)}"
        for match in re.finditer(r"\[DEPARTED\]\s+Replica\s+(\d+)\s+cleared intersection", log_text)
    }

    checks: List[Dict[str, Any]] = []

    def check(name: str, passed: bool, detail: str) -> None:
        checks.append({"name": name, "passed": bool(passed), "detail": detail})

    route_results: Dict[str, Dict[str, Any]] = {}
    cue_results: Dict[str, Dict[str, Any]] = {}
    for vehicle, expected in manifest.items():
        rows = trace_by_vehicle[vehicle]
        observed_ingress = next(
            (row["ingress_edge"] for row in rows if row["ingress_edge"] != "-"), None
        )
        observed_egress = next(
            (row["egress_edge"] for row in reversed(rows) if row["egress_edge"] != "-"), None
        )
        node_indices = sorted({row["node_index"] for row in rows})
        declared_lanes = sorted({row["declared_lane"] for row in rows})
        declared_directions = sorted({row["declared_direction"] for row in rows})
        actual_direction = (
            _movement_from_edges(observed_ingress, observed_egress)
            if observed_ingress and observed_egress else None
        )
        route_ok = (
            observed_ingress == expected["ingress_edge"]
            and observed_egress == expected["egress_edge"]
            and actual_direction == expected["intended_direction"]
            and node_indices == [expected["node_index"]]
            and declared_lanes == [expected["intended_lane"]]
            and declared_directions == [expected["intended_direction"]]
        )
        route_results[vehicle] = {
            "expected": expected,
            "observed_ingress_edge": observed_ingress,
            "observed_egress_edge": observed_egress,
            "actual_direction": actual_direction,
            "observed_node_indices": node_indices,
            "observed_declared_lanes": declared_lanes,
            "observed_declared_directions": declared_directions,
            "passed": route_ok,
        }

        eligible = [
            row for row in rows
            if row["echo_window"] == 1 and row["road_id"] == expected["ingress_edge"]
        ]
        expected_signal = expected["expected_signal"]

        def signal_matches(row: Dict[str, Any]) -> bool:
            if expected_signal == "LEFT":
                return row["signal_left"] == 1 and row["signal_right"] == 0
            if expected_signal == "RIGHT":
                return row["signal_left"] == 0 and row["signal_right"] == 1
            return row["signal_left"] == 0 and row["signal_right"] == 0

        correct_signal = sum(1 for row in eligible if signal_matches(row))
        correct_cue = sum(
            1 for row in eligible if row["derived_cue"] == expected["intended_direction"]
        )
        signal_rate = correct_signal / len(eligible) if eligible else None
        cue_rate = correct_cue / len(eligible) if eligible else None
        evaluation_cues = perception_cues.get(vehicle, [])
        correct_evaluations = sum(
            1 for cue in evaluation_cues if cue == expected["intended_direction"]
        )
        evaluation_rate = (
            correct_evaluations / len(evaluation_cues) if evaluation_cues else None
        )
        turning = expected["intended_direction"] in ("L", "R")
        cue_results[vehicle] = {
            "intended_direction": expected["intended_direction"],
            "expected_signal": expected_signal,
            "echo_window_ingress_samples": len(eligible),
            "correct_native_signal_samples": correct_signal,
            "correct_native_signal_rate": signal_rate,
            "correct_derived_cue_samples": correct_cue,
            "correct_derived_cue_rate": cue_rate,
            "perception_evaluation_samples": len(evaluation_cues),
            "correct_perception_evaluation_cues": correct_evaluations,
            "correct_perception_evaluation_cue_rate": evaluation_rate,
            "turning_route": turning,
            "cue_policy_passed": bool(evaluation_cues) and (
                not turning or evaluation_rate >= 0.90
            ),
        }

    trace_path = run_dir / "phase2_route_trace.csv"
    trace_fields = [
        "vehicle_id", "replica_id", "node_index", "time_s", "road_id", "lane_id",
        "ingress_edge", "egress_edge", "declared_lane", "declared_direction",
        "signal_left", "signal_right", "signal_emergency", "derived_cue", "echo_window",
        "cue_source",
    ]
    with trace_path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=trace_fields)
        writer.writeheader()
        writer.writerows(trace)

    turning_results = [row for row in cue_results.values() if row["turning_route"]]
    controlled_turn_policy_passed = bool(turning_results) and all(
        row["cue_policy_passed"] for row in turning_results
    )
    cue_output = {
        "turn_cue_correctness_threshold": 0.90,
        "controlled_turn_cue_policy_passed": controlled_turn_policy_passed,
        "controlled_cue_source_enabled": True,
        "cue_source": "controlled",
        "vehicles": cue_results,
    }
    (run_dir / "phase2_cue_characterization.json").write_text(
        json.dumps(cue_output, indent=2, sort_keys=True) + "\n"
    )

    check("manifest vehicles traced",
          len(trace_by_vehicle) == PHASE2_FIXTURE_N and all(trace_by_vehicle.values()),
          f"traced={sum(bool(rows) for rows in trace_by_vehicle.values())}/{PHASE2_FIXTURE_N} rows={len(trace)}")
    bad_routes = [vehicle for vehicle, row in route_results.items() if not row["passed"]]
    check("manifest route and declaration match", not bad_routes,
          f"all {PHASE2_FIXTURE_N} matched" if not bad_routes else f"mismatches={','.join(bad_routes)}")
    missing_departures = sorted(set(manifest) - departed)
    check("all vehicles departed", not missing_departures,
          f"{PHASE2_FIXTURE_N}/{PHASE2_FIXTURE_N} departed" if not missing_departures else f"missing={','.join(missing_departures)}")
    no_sample = sorted(
        vehicle for vehicle, row in cue_results.items()
        if row["echo_window_ingress_samples"] == 0
    )
    check("cue samples in echo window", not no_sample,
          "all vehicles sampled" if not no_sample else f"missing={','.join(no_sample)}")
    deficient_turns = sorted(
        vehicle for vehicle, row in cue_results.items()
        if row["turning_route"] and not row["cue_policy_passed"]
    )
    check("controlled turning cues >= 90 percent", controlled_turn_policy_passed,
          "all eight turning routes passed" if not deficient_turns
          else f"deficient={','.join(deficient_turns)}")
    check("honest fixture", "[BYZANTINE]" not in log_text,
          "no Byzantine runtime records" if "[BYZANTINE]" not in log_text
          else "unexpected Byzantine runtime record")
    check("zero-error perception", "sigma=0 signal_error=0" in log_text,
          "identity lane and cue channels configured")
    controlled_vehicles = set(re.findall(
        r"\[PHASE2-CUE-CONTROL\]\s+vehicle=(veh\d+)\s+source=controlled", log_text
    ))
    check("controlled cue source active", controlled_vehicles == set(manifest),
          f"controlled={len(controlled_vehicles)}/{PHASE2_FIXTURE_N}")

    result = {
        "checkpoint": "2A",
        "rep": rep,
        "config": PHASE2_FIXTURE_CONFIG,
        "passed": all(item["passed"] for item in checks),
        "checks": checks,
        "route_results": route_results,
        "cue_characterization": cue_output,
        "artifacts": {
            "raw_log": str(run_dir / "raw_simulation.log"),
            "route_trace": str(trace_path),
            "cue_characterization": str(run_dir / "phase2_cue_characterization.json"),
        },
    }
    (run_dir / "phase2_fixture_validation.json").write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n"
    )
    return result


def run_phase2_fixture_validation(args: argparse.Namespace, repetitions: int) -> int:
    manifest = _read_phase2_fixture_manifest()
    print(f"Phase 2A fixture validation: honest N={PHASE2_FIXTURE_N} mixed S/L/R routes")
    print(f"Manifest: {PHASE2_FIXTURE_MANIFEST} ({len(manifest)} vehicles)")
    audit_script = FOURWAY_DIR / "audit_ksafe_geometry.py"
    audit_json_path = FOURWAY_DIR / "phase2_ksafe_geometry_audit.json"
    audit_csv_path = FOURWAY_DIR / "phase2_ksafe_geometry_audit.csv"
    if args.dry_run:
        print(f"+ {sys.executable} {audit_script}")
        ksafe_audit: Dict[str, Any] = {"passed": True, "dry_run": True}
    else:
        audit_proc = subprocess.run([sys.executable, str(audit_script)], cwd=REPO_ROOT)
        ksafe_audit = (
            json.loads(audit_json_path.read_text())
            if audit_json_path.is_file()
            else {"passed": False, "error": "audit JSON was not produced"}
        )
        if audit_proc.returncode != 0:
            ksafe_audit["passed"] = False
    run_key_generation(dry_run=args.dry_run, scale=PHASE2_FIXTURE_N)
    results: List[Dict[str, Any]] = []

    for rep in range(args.start_rep, args.start_rep + repetitions):
        run_dir = REPO_ROOT / "benchmarks" / "Phase2Fixture" / "controlled" / f"run_{rep}"
        seed = run_seed(MASTER_SEED, PHASE2_FIXTURE_N, "PHASE2_FIXTURE", rep)
        if not args.dry_run:
            run_dir.mkdir(parents=True, exist_ok=True)
            clear_stale_random_ini()
            (run_dir / "perception_run_metadata.json").write_text(json.dumps({
                "checkpoint": "2A",
                "simulation_seed": seed,
                "approach_sigma_m": 0.0,
                "signal_observation_error": 0.0,
                "direction_collection_window_sec": args.direction_collection_window,
                "config": PHASE2_FIXTURE_CONFIG,
                "scenario": "honest_mixed_maneuver_fixture",
                "rep": rep,
                "n": PHASE2_FIXTURE_N,
                "manifest": str(PHASE2_FIXTURE_MANIFEST),
                "cue_source": "controlled",
            }, indent=2, sort_keys=True) + "\n")
        else:
            print("[dry-run] would clear stale scenario/perception overlays")

        argv = [
            str(RUN_SCRIPT), str(FOURWAY_DIR),
            "--channel-metrics-dir", str(run_dir.resolve()),
            "--approach-sigma", "0",
            "--signal-error", "0",
            "--direction-collection-window", str(args.direction_collection_window),
            "--simulation-seed", str(seed),
            "-u", "Cmdenv", "-c", PHASE2_FIXTURE_CONFIG,
        ]
        inner = " ".join(shlex.quote(value) for value in argv)
        try:
            run_in_bash_with_omnet(inner, dry_run=args.dry_run)
        except subprocess.CalledProcessError as exc:
            if LOG_FILE.is_file() and not args.dry_run:
                shutil.copy2(LOG_FILE, run_dir / "raw_simulation.log")
            result = {
                "checkpoint": "2A", "rep": rep, "passed": False,
                "checks": [{
                    "name": "simulation", "passed": False,
                    "detail": f"command exited with status {exc.returncode}",
                }],
                "result_dir": str(run_dir),
            }
            if not args.dry_run:
                (run_dir / "phase2_fixture_validation.json").write_text(
                    json.dumps(result, indent=2, sort_keys=True) + "\n"
                )
            results.append(result)
            continue
        if args.dry_run:
            continue
        shutil.copy2(LOG_FILE, run_dir / "raw_simulation.log")
        result = _validate_phase2_fixture_run(
            rep, run_dir, (run_dir / "raw_simulation.log").read_text(errors="replace")
        )
        result["checks"].append({
            "name": "kSafe entries match SUMO connection geometry",
            "passed": bool(ksafe_audit.get("passed")),
            "detail": (
                f"unsafe_entries={ksafe_audit.get('counts', {}).get('UNSAFE_KSAFE_ENTRY', 'unknown')} "
                f"conservative_omissions={ksafe_audit.get('counts', {}).get('CONSERVATIVE_OMISSION', 'unknown')}"
            ),
        })
        result["ksafe_geometry_audit"] = ksafe_audit
        result["artifacts"]["ksafe_geometry_csv"] = str(audit_csv_path)
        result["artifacts"]["ksafe_geometry_json"] = str(audit_json_path)
        result["passed"] = all(item["passed"] for item in result["checks"])
        (run_dir / "phase2_fixture_validation.json").write_text(
            json.dumps(result, indent=2, sort_keys=True) + "\n"
        )
        result["result_dir"] = str(run_dir)
        results.append(result)
        print(f"[{'PASS' if result['passed'] else 'FAIL'}] fixture run_{rep}")
        for item in result["checks"]:
            print(f"  {'PASS' if item['passed'] else 'FAIL':4} {item['name']}: {item['detail']}")

    if args.dry_run:
        print("[dry-run] no fixture validation artifacts were written")
        return 0

    summary_dir = REPO_ROOT / "benchmarks" / "Phase2Fixture"
    summary_dir.mkdir(parents=True, exist_ok=True)
    overall = bool(results) and all(result["passed"] for result in results)
    summary_path = summary_dir / "phase2_fixture_validation_summary.json"
    summary_path.write_text(json.dumps({
        "checkpoint": "2A",
        "passed": overall,
        "repetitions": repetitions,
        "results": results,
    }, indent=2, sort_keys=True) + "\n")
    print("\n========== PHASE 2A FIXTURE VALIDATION SUMMARY ==========")
    print(f"Overall: {'PASS' if overall else 'FAIL'}")
    print(f"Machine-readable summary: {summary_path}")
    return 0 if overall else 1


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
        default=None,
        help=f"Repetitions per scenario (default: {REPETITIONS}; default with --phase1-validation: 1).",
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
             "15=Emergency_Preempt_DynamicN, 16=Crash_Wait_Clear, "
             "17=Emergency_SuppressCancel_Unguarded, "
             "18=Emergency_SuppressCancel_Guarded, "
             "19=Crash_FabricatedClear_Unguarded, "
             "20=Crash_FabricatedClear_Guarded. "
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
        "--inject-f",
        type=int,
        default=None,
        metavar="F",
        help="Number of Byzantine followers selected by --randomize N F, independently of --tolerate.",
    )
    p.add_argument(
        "--sweep-f",
        action="store_true",
        help="For FALSE_LANE follower scenarios, run injected F=0..tolerated_f+1 using paired nested selections.",
    )
    p.add_argument(
        "--baseline",
        action="store_true",
        help="Run SUMO all-way-stop baseline configs for no-priority and priority-vehicle scenarios.",
    )
    p.add_argument(
        "--approach-sigma",
        type=float,
        default=0.0,
        choices=(0.0, 0.25, 0.5, 1.0, 2.0),
        help="Approach-observation sigma in metres from the checked-in matrix catalog.",
    )
    p.add_argument(
        "--signal-error",
        type=float,
        default=0.0,
        help="Maneuver-cue categorical error probability in [0,1].",
    )
    p.add_argument(
        "--direction-collection-window",
        type=float,
        default=0.25,
        help="Seconds to collect additional echoes after first reaching f+1.",
    )
    p.add_argument(
        "--phase1-validation",
        action="store_true",
        help="Run and self-check the fixed Phase 1 suite: E0 at N=4/N=16 plus signal- and lane-error N=4 smokes.",
    )
    p.add_argument(
        "--phase2-fixture-validation",
        action="store_true",
        help=f"Run Checkpoint 2A only: honest N={PHASE2_FIXTURE_N} mixed-route and cue characterization.",
    )
    p.add_argument(
        "--phase2-self-attestation-validation",
        action="store_true",
        help="Run Checkpoint 2B only: Phase 1 revalidation, order-byte comparison, and Check 10 UNKNOWN-upgrade rejection.",
    )
    p.add_argument(
        "--phase2-attack-validation",
        action="store_true",
        help=f"Run Checkpoint 2C E2/E4 b=0..{bft_f(PHASE2_ATTACK_N) + 1} zero and approved nonzero smokes.",
    )
    p.add_argument(
        "--phase2-metrology-validation",
        action="store_true",
        help="Run Checkpoint 2D movement-table, mixed-fixture, and calibration validation only.",
    )
    p.add_argument(
        "--phase2-pilots",
        action="store_true",
        help="Run the approved, resumable E1-E4 statistical pilot matrix after metrology approval.",
    )
    p.add_argument(
        "--phase2-pilot-honest-n",
        type=int,
        choices=(PHASE2_FIXTURE_N,),
        default=PHASE2_FIXTURE_N,
        help=f"Straight-only vehicle count for E1; Phase 2 is fixed at N={PHASE2_FIXTURE_N}.",
    )
    p.add_argument(
        "--phase2-pilot-profile",
        choices=("grid", "full"),
        default="full",
        help=(
            "Phase 2 pilot scope: 'grid' runs one N=16 repetition per E1-E4 "
            "parameter cell (88 runs); 'full' adds the approved repetitions and "
            "shoulder/boundary checks (384 unique runs)."
        ),
    )
    p.add_argument(
        "--experiment",
        choices=("e2", "e4"),
        help="Run one Checkpoint 2C authenticated evidence-attack experiment.",
    )
    p.add_argument(
        "--inject-b",
        type=int,
        default=None,
        metavar="COUNT",
        help="Total Phase 2 Byzantine evidence identities, including the claimant.",
    )
    p.add_argument(
        "--attack-target",
        type=int,
        default=PHASE2_ATTACK_TARGET_DEFAULT,
        metavar="REPLICA",
        help="Phase 2 attack claimant replica ID (default: veh0).",
    )
    p.add_argument(
        "--dry-run",
        action="store_true",
        help="Print commands only.",
    )
    return p.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    fixed_validation = (
        args.phase1_validation or args.phase2_fixture_validation or
        args.phase2_self_attestation_validation
        or args.phase2_attack_validation
        or args.phase2_metrology_validation
        or args.phase2_pilots
    )
    repetitions = args.reps if args.reps is not None else (1 if fixed_validation else REPETITIONS)
    if repetitions <= 0:
        print("ERROR: --reps must be >= 1.", file=sys.stderr)
        return 2
    if not 0.0 <= args.signal_error <= 1.0:
        print("ERROR: --signal-error must be in [0,1].", file=sys.stderr)
        return 2
    if args.direction_collection_window < 0.0:
        print("ERROR: --direction-collection-window must be nonnegative.", file=sys.stderr)
        return 2
    n_values = args.config
    for n in n_values:
        if n not in SUPPORTED_N_VALUES:
            print(f"WARNING: N={n} is outside the supported set {SUPPORTED_N_VALUES}; "
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
        if args.scenario and any(code in args.scenario for code in (15, 17, 18)) and n != 18:
            print("ERROR: emergency rollback scenarios 15/17/18 require --config 18.", file=sys.stderr)
            return 2
        if args.scenario and any(code in args.scenario for code in (16, 19, 20)) and n != 16:
            print("ERROR: crash scenarios 16/19/20 require --config 16.", file=sys.stderr)
            return 2

    if args.sweep_f and args.inject_f is not None:
        print("ERROR: --sweep-f and --inject-f are mutually exclusive.", file=sys.stderr)
        return 2
    if args.inject_f is not None and args.inject_f < 0:
        print("ERROR: --inject-f must be >= 0.", file=sys.stderr)
        return 2

    if args.scenario:
        # Respect user-requested order and de-duplicate aliases that map to the same scenario.
        scenarios = tuple(dict.fromkeys(SCENARIO_BY_CODE[c] for c in args.scenario))
    elif args.sweep_f:
        scenarios = ("ByzFollower_Ambulance", "NoFW_ByzFollower_FalseLane")
    else:
        scenarios = SCENARIO_ORDER

    false_lane_scenarios = {
        "ByzFollower_Ambulance",
        "ByzFollower_NoAmbulance",
        "NoFW_ByzFollower_FalseLane",
    }
    if (args.sweep_f or args.inject_f is not None) and any(
        scenario not in false_lane_scenarios for scenario in scenarios
    ):
        print("ERROR: --inject-f/--sweep-f apply only to Type-1 FALSE_LANE follower scenarios 3, 6, and 7.", file=sys.stderr)
        return 2

    if args.baseline:
        allowed = ("No_Ambulance_Honest", "Honest_Ambulance")
        scenarios = tuple(s for s in scenarios if s in allowed)
        if not scenarios:
            print("ERROR: --baseline only supports scenario codes 1 and 2.", file=sys.stderr)
            return 2

    if args.start_rep < 0:
        print("ERROR: --start-rep must be >= 0", file=sys.stderr)
        return 2

    fixed_presets = sum(bool(value) for value in (
        args.phase1_validation,
        args.phase2_fixture_validation,
        args.phase2_self_attestation_validation,
        args.phase2_attack_validation,
        args.phase2_metrology_validation,
        args.phase2_pilots,
    ))
    if fixed_presets > 1:
        print("ERROR: choose only one fixed validation preset.", file=sys.stderr)
        return 2

    if fixed_presets:
        incompatible = []
        if args.baseline:
            incompatible.append("--baseline")
        if args.scenario:
            incompatible.append("--scenario")
        if args.sweep_f:
            incompatible.append("--sweep-f")
        if args.inject_f is not None:
            incompatible.append("--inject-f")
        if args.tolerate is not None:
            incompatible.append("--tolerate")
        if args.randomize_leader:
            incompatible.append("--randomize-leader")
        if args.approach_sigma != 0.0:
            incompatible.append("--approach-sigma")
        if args.signal_error != 0.0:
            incompatible.append("--signal-error")
        if args.experiment is not None:
            incompatible.append("--experiment")
        if args.inject_b is not None:
            incompatible.append("--inject-b")
        if args.attack_target != PHASE2_ATTACK_TARGET_DEFAULT:
            incompatible.append("--attack-target")
        if not args.phase2_pilots and args.phase2_pilot_honest_n != PHASE2_FIXTURE_N:
            incompatible.append("--phase2-pilot-honest-n")
        if not args.phase2_pilots and args.phase2_pilot_profile != "full":
            incompatible.append("--phase2-pilot-profile")
        if incompatible:
            print(
                "ERROR: the fixed validation preset owns its honest scenarios and fault settings; incompatible with "
                + ", ".join(incompatible),
                file=sys.stderr,
            )
            return 2
        if args.phase1_validation:
            return run_phase1_validation(args, repetitions)
        if args.phase2_self_attestation_validation:
            return run_phase2_self_attestation_validation(args, repetitions)
        if args.phase2_attack_validation:
            return run_phase2_attack_experiments(args, repetitions, validation=True)
        if args.phase2_metrology_validation:
            return run_phase2_metrology_validation(args)
        if args.phase2_pilots:
            metrology_summary = REPO_ROOT / "benchmarks/Phase2Metrology/phase2_metrology_validation.json"
            if not metrology_summary.is_file() or not json.loads(metrology_summary.read_text()).get("passed"):
                print("ERROR: passing Phase 2D metrology validation is required before pilots.", file=sys.stderr)
                return 2
            return run_phase2_pilots(args)
        return run_phase2_fixture_validation(args, repetitions)

    if args.experiment:
        if args.inject_b is None:
            print("ERROR: --experiment requires --inject-b.", file=sys.stderr)
            return 2
        if args.inject_b < 0 or args.inject_b > PHASE2_ATTACK_N:
            print(f"ERROR: --inject-b must be in [0,{PHASE2_ATTACK_N}].", file=sys.stderr)
            return 2
        if args.attack_target < 0 or args.attack_target >= PHASE2_ATTACK_N:
            print(f"ERROR: --attack-target must be in [0,{PHASE2_ATTACK_N - 1}].", file=sys.stderr)
            return 2
        return run_phase2_attack_experiments(args, repetitions, validation=False)
    if args.inject_b is not None:
        print("ERROR: --inject-b requires --experiment e2|e4.", file=sys.stderr)
        return 2

    rep_indices = list(range(args.start_rep, args.start_rep + repetitions))

    print(f"Scenarios: {', '.join(scenarios)}")
    print(f"N values:  {n_values}")
    print(f"Reps:      run_{rep_indices[0]}..run_{rep_indices[-1]}")
    print(f"Tolerate:  {args.tolerate if args.tolerate is not None else 'max per N'}")

    for n in n_values:
        tolerated_f = args.tolerate if args.tolerate is not None else bft_f(n)
        if args.inject_f is not None and args.inject_f > n:
            print(f"ERROR: --inject-f {args.inject_f} exceeds N={n}.", file=sys.stderr)
            return 2
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
            if scenario_name in (
                "Honest_Ambulance",
                "No_Ambulance_Honest",
                "Emergency_Preempt_DynamicN",
                "Crash_Wait_Clear",
                "Emergency_SuppressCancel_Unguarded",
                "Emergency_SuppressCancel_Guarded",
                "Crash_FabricatedClear_Unguarded",
                "Crash_FabricatedClear_Guarded",
            ):
                if not args.dry_run:
                    clear_stale_random_ini()
                else:
                    print(f"[dry-run] would clear_stale_random_ini() before {scenario_name}")

            injected_values = (
                list(range(0, tolerated_f + 2)) if args.sweep_f
                else [args.inject_f]
            )
            for injected_f in injected_values:
                for i, rep in enumerate(rep_indices, start=1):
                    suffix = f" F={injected_f}" if injected_f is not None else ""
                    print(f"\n--- {scenario_name}{suffix} rep {i}/{repetitions} (run_{rep}) ---")
                    run_one_simulation(
                        n,
                        scenario_name,
                        rep,
                        dry_run=args.dry_run,
                        tolerated_f=tolerated_f,
                        explicit_tolerate=args.tolerate is not None,
                        injected_f=injected_f,
                        paired_false_lane_seed=injected_f is not None,
                        randomize_leader=args.randomize_leader,
                        baseline=args.baseline,
                        approach_sigma=args.approach_sigma,
                        signal_error=args.signal_error,
                        direction_collection_window=args.direction_collection_window,
                    )
                    run_analyze(
                        n,
                        scenario_name,
                        rep,
                        dry_run=args.dry_run,
                        tolerated_f=tolerated_f,
                        explicit_tolerate=args.tolerate is not None,
                        injected_f=injected_f,
                        baseline=args.baseline,
                        approach_sigma=args.approach_sigma,
                        signal_error=args.signal_error,
                    )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

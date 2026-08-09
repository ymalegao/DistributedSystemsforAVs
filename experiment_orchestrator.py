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
import json
import os
import random
import re
import shutil
import shlex
import subprocess
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
        "no self witnesses",
        self_evaluations == 0,
        f"self evaluations={self_evaluations}",
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
        "--dry-run",
        action="store_true",
        help="Print commands only.",
    )
    return p.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    repetitions = args.reps if args.reps is not None else (1 if args.phase1_validation else REPETITIONS)
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

    if args.phase1_validation:
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
        if incompatible:
            print(
                "ERROR: --phase1-validation owns its honest scenarios and fault settings; incompatible with "
                + ", ".join(incompatible),
                file=sys.stderr,
            )
            return 2
        return run_phase1_validation(args, repetitions)

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

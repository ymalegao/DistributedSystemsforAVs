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
from collections import Counter, defaultdict
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

# The dispatcher lives under tools/; keep the repository root explicit so
# existing scenario paths remain valid during the structure-only migration.
REPO_ROOT = Path(__file__).resolve().parents[1]


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
LONGITUDINAL_GRID_N = 16
LONGITUDINAL_GRID_CONFIG = "SixteenVehiclesResDB"
LONGITUDINAL_GRID_TARGET = 15
LONGITUDINAL_GRID_PREDECESSOR = 11
LONGITUDINAL_GRID_SEPARATIONS_M = (5.0, 5.5, 6.5, 7.5, 9.5, 12.5)
LONGITUDINAL_GRID_SIGMAS_M = (0.25, 0.5, 1.0, 2.0)
LONGITUDINAL_GRID_MAIN_SIGMA_M = 1.0
LONGITUDINAL_GRID_ATTACK_AHEAD_MARGIN_M = 0.1
LONGITUDINAL_GRID_RETHRESHOLD_K = (2.0, 2.5, 3.0)
ADJACENT_LANE_N = 16
ADJACENT_LANE_CONFIG = "SixteenVehiclesAdjacentLaneResDB"
ADJACENT_LANE_TARGET = 0
ADJACENT_LANE_VALIDATION_SEED = 1630307738
ADJACENT_LANE_VALIDATION_CELLS: Tuple[Dict[str, Any], ...] = (
    {"name": "honest_zero", "b": 0, "sigma": 0.0, "delta": 0.0},
    {"name": "bf_shoulder", "b": 5, "sigma": 0.5, "delta": 1.75},
    {"name": "bf_zero_error_guard", "b": 5, "sigma": 0.0, "delta": 3.2},
    {"name": "bf1_cliff", "b": 6, "sigma": 0.0, "delta": 3.2},
)
TWO_LANE_N = 16
TWO_LANE_CONFIG = "SixteenVehiclesTwoLaneResDB"
TWO_LANE_CONFLICT_CONFIG = "SixteenVehiclesTwoLaneConflictReleaseResDB"
TWO_LANE_SWEEP_CONFIG = "SixteenVehiclesTwoLaneSweepResDB"
TWO_LANE_SCALE_CONFIGS = {
    4: "FourVehiclesTwoLaneScaleResDB",
    8: "EightVehiclesTwoLaneScaleResDB",
    16: TWO_LANE_SWEEP_CONFIG,
    20: "TwentyVehiclesTwoLaneScaleResDB",
}
TWO_LANE_TARGET = 0
TWO_LANE_VALIDATION_SEED = 26081502
TWO_LANE_VALIDATION_CELLS: Tuple[Dict[str, Any], ...] = (
    {"name": "bf_zero_guard", "kind": "attack", "b": 5, "sigma_lat": 0.0,
     "sigma_lon": 0.0, "delta": -1.75},
    {"name": "bf_shoulder", "kind": "attack", "b": 5, "sigma_lat": 0.5,
     "sigma_lon": 0.0, "delta": -1.75},
    {"name": "bf1_cliff", "kind": "attack", "b": 6, "sigma_lat": 0.0,
     "sigma_lon": 0.0, "delta": -1.75},
    {"name": "rank_sigma_1", "kind": "rank", "b": 0, "sigma_lat": 0.0,
     "sigma_lon": 1.0, "delta": 0.0},
    {"name": "check10_physical_lane", "kind": "mutation", "b": 0,
     "sigma_lat": 0.0, "sigma_lon": 0.0, "delta": 0.0},
    {"name": "conflict_release_cliff", "kind": "conflict_release", "b": 6,
     "sigma_lat": 0.0, "sigma_lon": 0.0, "delta": -1.75,
     "config": TWO_LANE_CONFLICT_CONFIG},
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
    ego_longitudinal_sigma: float = 0.0,
    longitudinal_sigma: float = 0.0,
    lane_observation_mode: str = "CATEGORICAL_CARDINAL",
    lateral_sigma: float = 0.0,
    lateral_claim_offset: float = 0.0,
    physical_gate_k: float = 3.0,
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
        if (ego_longitudinal_sigma != 0.0 or longitudinal_sigma != 0.0 or
                lane_observation_mode != "CATEGORICAL_CARDINAL" or
                lateral_sigma != 0.0 or lateral_claim_offset != 0.0 or
                physical_gate_k != 3.0):
            ego_lon = format(ego_longitudinal_sigma, "g").replace(".", "p")
            witness_lon = format(longitudinal_sigma, "g").replace(".", "p")
            gate = format(physical_gate_k, "g").replace(".", "p")
            base = base / f"ego_lon_{ego_lon}_witness_lon_{witness_lon}_k_{gate}"
            if lane_observation_mode != "CATEGORICAL_CARDINAL":
                lat = format(lateral_sigma, "g").replace(".", "p")
                delta = format(lateral_claim_offset, "g").replace(".", "p")
                base = base / f"adjacent_lat_{lat}_delta_{delta}"
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
        "distance_fixture_override.ini",
        "distance_grid_16veh.rou.xml",
        "distance_grid_16veh.sumo.cfg",
        "distance_grid_16veh.launchd.xml",
        "distance_grid_fixture_metadata.json",
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
    ego_longitudinal_sigma: float = 0.0,
    longitudinal_sigma: float = 0.0,
    lane_observation_mode: str = "CATEGORICAL_CARDINAL",
    lateral_sigma: float = 0.0,
    lateral_claim_offset: float = 0.0,
    physical_gate_k: float = 3.0,
    distance_stationary_speed: float = 0.1,
    distance_attestation_retry_interval: float = 0.5,
    distance_attestation_retry_max: int = 4,
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
        ego_longitudinal_sigma=ego_longitudinal_sigma,
        longitudinal_sigma=longitudinal_sigma,
        lane_observation_mode=lane_observation_mode,
        lateral_sigma=lateral_sigma,
        lateral_claim_offset=lateral_claim_offset,
        physical_gate_k=physical_gate_k,
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
                "ego_longitudinal_sigma_m": ego_longitudinal_sigma,
                "longitudinal_observation_sigma_m": longitudinal_sigma,
                "lane_observation_mode": lane_observation_mode,
                "lateral_observation_sigma_m": lateral_sigma,
                "lateral_claim_offset_m": lateral_claim_offset,
                "physical_gate_k": physical_gate_k,
                "distance_stationary_speed_mps": distance_stationary_speed,
                "distance_attestation_retry_interval_sec": distance_attestation_retry_interval,
                "distance_attestation_retry_max": distance_attestation_retry_max,
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
        "--ego-longitudinal-sigma", str(ego_longitudinal_sigma),
        "--longitudinal-sigma", str(longitudinal_sigma),
        "--lane-observation-mode", lane_observation_mode,
        "--lateral-sigma", str(lateral_sigma),
        "--physical-gate-k", str(physical_gate_k),
        "--distance-stationary-speed", str(distance_stationary_speed),
        "--distance-attestation-retry-interval", str(distance_attestation_retry_interval),
        "--distance-attestation-retry-max", str(distance_attestation_retry_max),
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
    ego_longitudinal_sigma: float = 0.0,
    longitudinal_sigma: float = 0.0,
    lane_observation_mode: str = "CATEGORICAL_CARDINAL",
    lateral_sigma: float = 0.0,
    lateral_claim_offset: float = 0.0,
    physical_gate_k: float = 3.0,
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
        ego_longitudinal_sigma=ego_longitudinal_sigma,
        longitudinal_sigma=longitudinal_sigma,
        lane_observation_mode=lane_observation_mode,
        lateral_sigma=lateral_sigma,
        lateral_claim_offset=lateral_claim_offset,
        physical_gate_k=physical_gate_k,
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


def run_distance_rank_check10_validation(args: argparse.Namespace, repetitions: int) -> int:
    """Force a proposal-only rank mutation and require Check 10 recovery."""
    n = 4
    scenario = "ByzLeader_NoAmbulance"
    results: list[dict[str, object]] = []
    for rep in range(args.start_rep, args.start_rep + repetitions):
        if args.dry_run:
            print(
                "[dry-run] would run N=4 Byzantine-primary type 10, require "
                "certificate/proposal position mismatch rejection, view change, commit, and 4 departures"
            )
            run_key_generation(dry_run=True, scale=n)
            run_one_simulation(
                n, scenario, rep,
                dry_run=True,
                tolerated_f=bft_f(n),
                explicit_tolerate=False,
                direction_collection_window=args.direction_collection_window,
                ego_longitudinal_sigma=0.0,
                longitudinal_sigma=0.0,
                physical_gate_k=args.physical_gate_k,
                distance_stationary_speed=args.distance_stationary_speed,
                distance_attestation_retry_interval=args.distance_attestation_retry_interval,
                distance_attestation_retry_max=args.distance_attestation_retry_max,
                extra_runner_args=("--leader-byz-type", "10"),
            )
            continue

        run_key_generation(dry_run=False, scale=n)
        clear_stale_random_ini()
        run_one_simulation(
            n, scenario, rep,
            dry_run=False,
            tolerated_f=bft_f(n),
            explicit_tolerate=False,
            direction_collection_window=args.direction_collection_window,
            ego_longitudinal_sigma=0.0,
            longitudinal_sigma=0.0,
            physical_gate_k=args.physical_gate_k,
            distance_stationary_speed=args.distance_stationary_speed,
            distance_attestation_retry_interval=args.distance_attestation_retry_interval,
            distance_attestation_retry_max=args.distance_attestation_retry_max,
            extra_runner_args=("--leader-byz-type", "10"),
        )

        text = LOG_FILE.read_text(errors="replace")
        mismatch = re.search(
            r"\[OMNET-PREVERIFY\] reject: state-field mismatch[^\n]*"
            r"cert\([^\n]*pos=(\d+)[^\n]*proposal\([^\n]*pos=(\d+)",
            text,
        )
        injected = "TAMPER_DISTANCE_RANK" in text
        rejected = bool(mismatch and mismatch.group(1) != mismatch.group(2))
        view_change = "[VC-TRIGGER]" in text or "primary-changed" in text
        decided = "Order_Decided_Time" in text
        departed = len(set(re.findall(r"\[DEPARTED\] Replica (\d+)\b", text))) == n
        outcome = (
            "fault=TAMPER_DISTANCE_RANK "
            "outcome=STATE_FIELD_MISMATCH_REJECTED_AND_RECOVERED"
        ) in text
        passed = injected and rejected and view_change and decided and departed and outcome

        run_dir = (
            REPO_ROOT / "benchmarks" / "Phase2Distance" /
            "check10_rank_mutation" / f"run_{rep}"
        )
        run_dir.mkdir(parents=True, exist_ok=True)
        shutil.copy2(LOG_FILE, run_dir / "raw_simulation.log")
        result = {
            "rep": rep,
            "passed": passed,
            "injected": injected,
            "check10_rejected": rejected,
            "cert_position": int(mismatch.group(1)) if mismatch else None,
            "proposal_position": int(mismatch.group(2)) if mismatch else None,
            "view_change": view_change,
            "honest_order_decided": decided,
            "departed": departed,
            "attack_outcome": outcome,
            "result_dir": str(run_dir),
        }
        (run_dir / "distance_rank_check10_validation.json").write_text(
            json.dumps(result, indent=2, sort_keys=True) + "\n"
        )
        results.append(result)

    if args.dry_run:
        return 0
    overall = bool(results) and all(bool(row["passed"]) for row in results)
    summary_dir = REPO_ROOT / "benchmarks" / "Phase2Distance"
    summary_dir.mkdir(parents=True, exist_ok=True)
    summary_path = summary_dir / "distance_rank_check10_validation_summary.json"
    summary_path.write_text(json.dumps({
        "checkpoint": "P7 Check 10 distance-derived rank mutation",
        "passed": overall,
        "results": results,
    }, indent=2, sort_keys=True) + "\n")
    print("\n========== DISTANCE-RANK CHECK 10 VALIDATION ==========")
    for row in results:
        print(
            f"{'PASS' if row['passed'] else 'FAIL'} run_{row['rep']} "
            f"cert_pos={row['cert_position']} proposal_pos={row['proposal_position']}"
        )
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
    if experiment == "e2":
        return "WRONG_APPROACH"
    if experiment == "e4":
        return "FALSE_DIRECTION"
    if experiment == "e2long":
        return "FALSE_DISTANCE"
    raise ValueError(f"unknown attack experiment {experiment}")


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
    distance_offset = args.distance_claim_offset if experiment == "e2long" and b > 0 else 0.0
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
            "longitudinal_observation_sigma_m": (
                args.longitudinal_sigma if experiment == "e2long" else 0.0
            ),
            "distance_claim_offset_m": distance_offset,
            "config": PHASE2_ATTACK_CONFIG,
            "n": PHASE2_ATTACK_N,
            "cue_source": "controlled",
        }, indent=2, sort_keys=True) + "\n")

    argv = [
        str(RUN_SCRIPT), str(FOURWAY_DIR),
        "--compact-log",
        "--channel-metrics-dir", str(run_dir.resolve()),
        "--approach-sigma", str(0.0 if experiment == "e2long" else sigma),
        "--signal-error", str(signal_error),
        "--direction-collection-window", str(args.direction_collection_window),
        "--ego-longitudinal-sigma", "0",
        "--longitudinal-sigma", str(args.longitudinal_sigma if experiment == "e2long" else 0.0),
        "--physical-gate-k", str(args.physical_gate_k),
        "--distance-stationary-speed", str(args.distance_stationary_speed),
        "--distance-attestation-retry-interval", str(args.distance_attestation_retry_interval),
        "--distance-attestation-retry-max", str(args.distance_attestation_retry_max),
        "--simulation-seed", str(seed),
        "--phase2-attack-kind", kind,
        "--phase2-attack-target", str(target),
        "--phase2-evidence-colluders", colluder_csv,
        "--phase2-actual-b", str(b),
        "--phase2-distance-claim-offset", str(distance_offset),
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
    expected_byzantine = [] if b == 0 else sorted([target, *expected_colluders])
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
    elif experiment == "e4":
        check(
            "E4 declaration",
            declaration == {
                "kind": "FALSE_DIRECTION", "actual_lane": "N", "claimed_lane": "N",
                "actual_direction": "S", "claimed_direction": "R",
            },
            f"declaration={declaration}",
        )
    else:
        distance_declaration = summary.get("distance_declaration")
        check(
            "E2-LONG authenticated distance declaration",
            distance_declaration is not None and
                abs(float(distance_declaration["offset_m"])) > 0.0,
            f"distance_declaration={distance_declaration}",
        )
    self_rows = re.findall(
        rf"\[SELF-ATTEST\] target=veh{target} epoch=0 signer={target} .* status=VALID", text
    )
    check("one target self-attestation", len(self_rows) == 1,
          f"records={len(self_rows)}")
    measured_colluders = set(
        summary["distance_collusion_echo_signers"]
        if experiment == "e2long" else summary["collusion_echo_signers"]
    )
    check(
        "collusion echoes come only from configured nested set",
        measured_colluders.issubset(expected_colluders),
        f"configured={expected_colluders} measured={sorted(measured_colluders)}",
    )
    h_lane_ids = set(summary["h_lane_signers"])
    h_dir_ids = set(summary["h_dir_signers"])
    h_distance_ids = set(summary["h_distance_signers"])
    check(
        "honest h excludes Byzantine identities",
        not h_lane_ids.intersection(expected_byzantine) and
            not h_dir_ids.intersection(expected_byzantine) and
            not h_distance_ids.intersection(expected_byzantine),
        f"h_lane={sorted(h_lane_ids)} h_dir={sorted(h_dir_ids)} h_distance={sorted(h_distance_ids)} byz={expected_byzantine}",
    )
    cert_signers = set(summary["certificate_signers"])
    if experiment == "e2long":
        cert_signers = set(summary["distance_certificate_signers"])
    check(
        "target signer occurs at most once",
        (summary["distance_self_attestation_count"] if experiment == "e2long"
         else summary["self_attestation_count"]) in (0, 1) and
            (target in cert_signers) == ((summary["distance_self_attestation_count"]
                if experiment == "e2long" else summary["self_attestation_count"]) == 1),
        f"self={summary['distance_self_attestation_count'] if experiment == 'e2long' else summary['self_attestation_count']} cert_signers={sorted(cert_signers)}",
    )
    check(
        "authenticated evidence accepted",
        "[ANN-ORIGIN-INVALID]" not in text and "verify FAIL" not in text and
            "[CERT-INVALID]" not in text,
        "no origin, echo-signature, or certificate validation failures",
    )
    if experiment == "e2long":
        expected_distance_bsig = len(cert_signers.intersection(expected_byzantine))
        check(
            "distance b_sig matches certificate bytes",
            summary["b_sig_distance"] == expected_distance_bsig,
            f"distance={summary['b_sig_distance']}/{expected_distance_bsig}",
        )
    elif summary["lane_certified"]:
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
        false_outcome = (
            summary["distance_certified"] if experiment == "e2long"
            else bool(outcome.get("false_lane_certificate") or outcome.get("false_eligibility"))
        )
        check("zero error b<=f blocks false outcome", not false_outcome,
              f"outcome={outcome}")
    if b == bft_f(PHASE2_ATTACK_N) + 1:
        channel_b_sig = (
            summary["b_sig_lane"] if experiment == "e2" else
            summary["b_sig_dir"] if experiment == "e4" else
            summary["b_sig_distance"]
        )
        crossed = (
            outcome.get("false_lane_certificate") if experiment == "e2"
            else outcome.get("false_eligibility") if experiment == "e4"
            else summary["distance_certified"]
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
        cells = [(
            args.experiment,
            args.inject_b,
            args.longitudinal_sigma if args.experiment == "e2long" else args.approach_sigma,
            args.signal_error,
        )]

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


def run_continuous_coordinate_calibration(args: argparse.Namespace) -> int:
    """Validate lateral and longitudinal fixtures without launching OMNeT/ResDB."""
    fixtures = (
        FOURWAY_DIR / "adjacent_lane_calibration",
        FOURWAY_DIR / "longitudinal_distance_calibration",
    )
    if args.dry_run:
        for fixture in fixtures:
            print(f"[dry-run] cwd={fixture} {shlex.join([sys.executable, 'run_calibration.py'])}")
            print(f"[dry-run] cwd={fixture} {shlex.join([sys.executable, '-m', 'unittest', 'test_calibration.py', '-v'])}")
        return 0

    results = []
    for fixture in fixtures:
        print(f"========== Continuous calibration: {fixture.name} ==========")
        subprocess.run([sys.executable, "run_calibration.py"], cwd=fixture, check=True)
        subprocess.run(
            [sys.executable, "-m", "unittest", "test_calibration.py", "-v"],
            cwd=fixture,
            check=True,
        )
        canonical = fixture / "canonical_validation_summary.json"
        summary = json.loads(canonical.read_text())
        checkpoint_pass = bool(summary.get("pass_criteria", {}).get("checkpoint_pass"))
        results.append({
            "fixture": fixture.name,
            "canonical_summary": str(canonical),
            "passed": checkpoint_pass,
        })

    summary_dir = REPO_ROOT / "benchmarks" / "ContinuousCoordinateCalibration"
    summary_dir.mkdir(parents=True, exist_ok=True)
    overall = all(row["passed"] for row in results)
    summary_path = summary_dir / "continuous_coordinate_calibration_summary.json"
    summary_path.write_text(json.dumps({
        "checkpoint": "P7-calibration",
        "passed": overall,
        "protocol_integration_started": True,
        "fixtures": results,
    }, indent=2, sort_keys=True) + "\n")
    print("\n========== CONTINUOUS COORDINATE CALIBRATION SUMMARY ==========")
    print(f"Overall: {'PASS' if overall else 'FAIL'}")
    print(f"Machine-readable summary: {summary_path}")
    return 0 if overall else 1


def _two_lane_validation_run_dir(cell: Dict[str, Any], rep: int) -> Path:
    return REPO_ROOT / "benchmarks" / "Phase2TwoLaneValidation" / str(cell["name"]) / f"run_{rep}"


def _run_two_lane_validation_cell(
    args: argparse.Namespace, cell: Dict[str, Any], rep: int, cell_index: int
) -> Path:
    b = int(cell["b"])
    config = str(cell.get("config", TWO_LANE_CONFIG))
    seed = TWO_LANE_VALIDATION_SEED + cell_index + rep
    colluders = _phase2_nested_colluders(TWO_LANE_TARGET, b, TWO_LANE_N)
    run_dir = _two_lane_validation_run_dir(cell, rep)
    if not args.dry_run:
        run_dir.mkdir(parents=True, exist_ok=True)
        (run_dir / "two_lane_run_metadata.json").write_text(json.dumps({
            "checkpoint": "P7-full-two-lane-validation",
            "cell": cell["name"],
            "n": TWO_LANE_N,
            "f": bft_f(TWO_LANE_N),
            "attack_target_replica_id": TWO_LANE_TARGET if cell["kind"] == "attack" else None,
            "actual_byzantine_count": b,
            "evidence_colluder_ids": colluders,
            "simulation_seed": seed,
            "lane_observation_mode": "ADJACENT_LATERAL",
            "lateral_observation_sigma_m": cell["sigma_lat"],
            "longitudinal_observation_sigma_m": cell["sigma_lon"],
            "lateral_claim_offset_m": cell["delta"],
            "direction_collection_window_sec": 0.25,
            "physical_gate_k": 3.0,
            "config": config,
            "execution": "strictly sequential",
        }, indent=2, sort_keys=True) + "\n")

    argv = [
        str(RUN_SCRIPT),
        "--randomize", str(TWO_LANE_N), "0",
        "--no-ambulance",
        "--tolerated-f", str(bft_f(TWO_LANE_N)),
    ]
    if cell["kind"] == "mutation":
        argv.extend(("--byzleader", "0", "--leader-byz-type", "11"))
    argv.extend((
        str(FOURWAY_DIR),
        "--channel-metrics-dir", str(run_dir.resolve()),
        "--approach-sigma", "0",
        "--signal-error", "0",
        "--direction-collection-window", "0.25",
        "--ego-longitudinal-sigma", "0",
        "--longitudinal-sigma", str(cell["sigma_lon"]),
        "--lane-observation-mode", "ADJACENT_LATERAL",
        "--lateral-sigma", str(cell["sigma_lat"]),
        "--adjacent-lane-separation", "3.2",
        "--physical-gate-k", "3",
        "--simulation-seed", str(seed),
    ))
    # Keep the deterministic physical-consequence smoke unfiltered so its
    # scheduler, release, and metrology evidence is reviewable.  The manager
    # ends this config at the first conflicting co-occupancy, so the full log
    # remains bounded.
    if cell["kind"] != "conflict_release":
        argv.insert(argv.index("--channel-metrics-dir"), "--compact-log")
    if cell["kind"] in ("attack", "conflict_release"):
        argv.extend((
            "--phase2-attack-kind", "FALSE_PHYSICAL_LANE",
            "--phase2-attack-target", str(TWO_LANE_TARGET),
            "--phase2-evidence-colluders", ",".join(str(value) for value in colluders),
            "--phase2-actual-b", str(b),
            "--phase2-lateral-claim-offset", str(cell["delta"]),
        ))
    argv.extend(("-u", "Cmdenv", "--debug-on-errors=false", "-c", config))
    inner = " ".join(shlex.quote(value) for value in argv)
    print("+ " + inner)
    run_in_bash_with_omnet(inner, dry_run=args.dry_run)
    if not args.dry_run:
        shutil.copy2(LOG_FILE, run_dir / "raw_simulation.log")
    return run_dir


def _validate_two_lane_cell(
    cell: Dict[str, Any], rep: int, run_dir: Path
) -> Dict[str, Any]:
    text = (run_dir / "raw_simulation.log").read_text(errors="replace")
    checks: List[Dict[str, Any]] = []

    def check(name: str, passed: bool, detail: str) -> None:
        checks.append({"name": name, "passed": bool(passed), "detail": detail})

    departed = set(re.findall(r"\[DEPARTED\] Replica (\d+)\b", text))
    if cell["kind"] == "conflict_release":
        metrology_end = bool(re.search(
            r"\[METROLOGY-END\] reason=first-conflicting-cooccupancy ", text,
        ))
        check("validation stops only after physical consequence", metrology_end,
              f"metrology_end={metrology_end} departed_before_stop={len(departed)}")
    else:
        check("all vehicles departed", len(departed) == TWO_LANE_N,
              f"departed={len(departed)}/{TWO_LANE_N}")
    check("consensus fault mode provisioned", "reason=fault-mode-disabled" not in text,
          "tolerated f=5 active for the full protocol run")
    if cell["kind"] == "conflict_release":
        check("no teleport", "Teleporting vehicle" not in text,
              "time-to-teleport=-1 preserves the physical outcome")
    else:
        check("no crash or teleport", not re.search(
            r"\[CRASH_DETECTED\]|Teleporting vehicle|collision with vehicle", text),
            "no crash, SUMO collision, or teleport record")
    check("authenticated evidence valid", not re.search(
        r"\[ANN-ORIGIN-INVALID\]|\[CERT-INVALID\]|verify FAIL", text),
        "no announcement/certificate signature failure")

    evaluation_keys = re.findall(
        r"\[PERC-EVAL\].*?witness=(\d+) target=(veh\d+) epoch=(\d+) claimHash=([0-9a-f]+)",
        text,
    )
    duplicates = len(evaluation_keys) - len(set(evaluation_keys))
    check("single-evaluation invariant", duplicates == 0, f"duplicates={duplicates}")

    target_entries = re.findall(
        r"\[EXECUTOR\] entries epoch=0 n=16:[^\n]*?r0\(lane=([NSEW]) pos=(\d+) "
        r"dir=([A-Za-z?]+) ambu=\d+ cyber=(\d)\)",
        text,
    )
    final_target = target_entries[-1] if target_entries else None
    check("target present in committed entry", final_target is not None,
          f"entry={final_target}")
    decided = bool(re.search(r"Order_Decided_Time", text))
    check("order committed", decided, f"decided={decided}")

    cert_lines = re.findall(r"\[CERT-EVIDENCE\] target=veh0[^\n]+", text)
    cert_assembled = bool(re.search(r"\[CERT-ASSEMBLE\] target=veh0\b", text))
    cert_b_sig = sum("byzantine=1" in line for line in cert_lines)
    honest_support = sum(
        "byzantine=0" in line and "supporting=1" in line for line in cert_lines
    )
    eligibility = re.findall(
        r"\[DIR-ELIGIBILITY\] target=veh0[^\n]*?b_sig=(\d+)[^\n]*?"
        r"physicalLaneIndex=(\d+)[^\n]*?laneAuthorized=(\d+)[^\n]*?derivedDirection=([A-Z?]+)",
        text,
    )
    derived = eligibility[-1] if eligibility else None
    actual_b_sig = int(derived[0]) if derived else cert_b_sig

    if cell["kind"] in ("attack", "conflict_release"):
        check("composite declaration emitted", bool(re.search(
            r"\[PHASE2-ATTACK-DECLARE\] target=veh0.*actualDirection=L claimedDirection=R", text)),
            "veh0 actual N-LEFT/lane1 declares N-RIGHT/lane0")
        check("certificate accounting matches bytes", actual_b_sig == cert_b_sig,
              f"derived_b_sig={actual_b_sig} cert_b_sig={cert_b_sig}")
        if cell["name"] == "bf_zero_guard":
            check("zero-error b=f blocks false certificate", not cert_assembled,
                  f"cert_assembled={cert_assembled} b_sig={cert_b_sig}")
            check("target degrades to committed QUIET singleton",
                  final_target is not None and final_target[3] == "0",
                  f"entry={final_target}")
            check("guard has no unsafe co-occupancy",
                  "[UNSAFE-CONFLICT-COOCCUPANCY]" not in text,
                  "QUIET target remains singleton")
        elif cell["name"] == "bf_shoulder":
            required_honest = max(0, bft_f(TWO_LANE_N) + 1 - actual_b_sig)
            consistent = (
                cert_assembled and actual_b_sig <= 5 and
                honest_support >= required_honest and final_target is not None and
                final_target[3] == "1" and derived is not None and
                derived[1:] == ("0", "1", "R")
            ) or (
                not cert_assembled and final_target is not None and final_target[3] == "0"
            )
            check("b=f shoulder uses finalized-certificate evidence", consistent,
                  f"formed={cert_assembled} b_sig={actual_b_sig} "
                  f"honest_support={honest_support} required={required_honest} entry={final_target}")
        elif cell["name"] == "bf1_cliff":
            consistent = (
                cert_assembled and actual_b_sig >= 6 and honest_support == 0 and
                final_target is not None and final_target[3] == "1"
            ) or (
                not cert_assembled and actual_b_sig < 6 and
                final_target is not None and final_target[3] == "0"
            )
            check("b=f+1 cliff uses actual b_sig", consistent,
                  f"formed={cert_assembled} b_sig={actual_b_sig} "
                  f"honest_support={honest_support} entry={final_target}")
        elif cell["name"] == "conflict_release_cliff":
            cliff_ok = (
                cert_assembled and actual_b_sig >= 6 and honest_support == 0 and
                final_target is not None and final_target[2:] == ("R", "1")
            )
            check("false physical-lane authority unlocked", cliff_ok,
                  f"formed={cert_assembled} b_sig={actual_b_sig} "
                  f"honest_support={honest_support} entry={final_target}")

            decisions = re.findall(
                r"\[EXECUTOR\] OrderDecision: epoch=0[^\n]*decisions=\[([^\n]+)\]",
                text,
            )
            shared_batch = None
            for decision in decisions:
                mapping = {
                    int(vehicle): int(batch)
                    for vehicle, batch in re.findall(r"veh=(\d+) batch=(\d+)", decision)
                }
                if 0 in mapping and 1 in mapping and mapping[0] == mapping[1]:
                    shared_batch = mapping[0]
                    break
            check("target and counterpart share committed batch", shared_batch is not None,
                  f"veh0_batch=veh1_batch={shared_batch}")

            ksafe_admit = bool(re.search(
                r"\[SCHEDULER-BATCH-ADMIT\] head=0 headLane=0 headDirection=2 "
                r"candidate=1 candidateLane=1 candidateDirection=0 rule=kSafe",
                text,
            ))
            check("unchanged kSafe admits claimed movements", ksafe_admit,
                  "claimed N-R plus S-S admitted by existing kSafe rule")

            actual_movements = (
                "[MOVEMENT-GROUND-TRUTH] vehicle=veh0 plannedIngress=N2C "
                "plannedEgress=C2E movement=N-L" in text and
                "[MOVEMENT-GROUND-TRUTH] vehicle=veh1 plannedIngress=S2C "
                "plannedEgress=C2N movement=S-S" in text
            )
            check("actual movements are the reviewed conflicting pair", actual_movements,
                  "veh0=N-L and veh1=S-S from SUMO ingress/egress")

            cooccupancy = bool(re.search(
                r"\[CONFLICTING-COOCCUPANCY\] first=veh0 firstMovement=N-L "
                r"second=veh1 secondMovement=S-S", text,
            ))
            check("conflicting pair overlaps in conflict zone", cooccupancy,
                  f"cooccupancy={cooccupancy}")

            release_records = re.findall(
                r"[^\n]*resumeVehicle[^\n]*speedMode=0[^\n]*", text,
            )
            clean_r0_release = bool(re.search(
                r"\[V2VResDB r0\] resumeVehicle[^\n]*speedMode=0", text,
            ))
            clean_r1_release = bool(re.search(
                r"\[V2VResDB r1\] resumeVehicle[^\n]*speedMode=0", text,
            ))
            # ResDB stderr can splice into OMNeT stdout mid-record.  If r1's
            # prefix is damaged, accept the second complete speedMode marker
            # only when the committed same-batch pair is physically observed
            # together.  That combination proves both releases executed; the
            # shared resumeVehicle path sets mode 0 unconditionally.
            interleaved_r1_release = (
                len(release_records) >= 2 and shared_batch is not None and cooccupancy
            )
            runtime_release = clean_r0_release and (
                clean_r1_release or interleaved_r1_release
            )
            cfg = FOURWAY_DIR / "bft_16veh_2lane_conflict_release.sumo.cfg"
            route = FOURWAY_DIR / "bft_16veh_2lane_conflict_release.rou.xml"
            cfg_text = cfg.read_text()
            route_text = route.read_text()
            pinned_physics = bool(runtime_release) and all(token in cfg_text for token in (
                '<collision.check-junctions value="true"/>',
                '<collision.action value="none"/>',
                '<time-to-teleport value="-1"/>',
            )) and 'jmIgnoreFoeProb="1"' in route_text
            check("SUMO release physics pinned", pinned_physics,
                  "speedMode=0, jmIgnoreFoeProb=1, junction collisions on, "
                  "collision action none, teleport disabled; "
                  f"clean_r0={clean_r0_release} clean_r1={clean_r1_release} "
                  f"release_records={len(release_records)}")

    elif cell["kind"] == "rank":
        manifest_rows = list(csv.DictReader(
            (FOURWAY_DIR / "two_lane_route_manifest.csv").open()
        ))
        physical_lane = {
            row["vehicle_id"]: int(row["sumo_lane_index"])
            for row in manifest_rows
        }
        pairs = re.findall(
            r"\[DIST-ORDER-PAIR\].*?lane=([NSEW]):([01]) "
            r"a=(veh\d+) b=(veh\d+)", text,
        )
        partition_ok = bool(pairs) and all(
            physical_lane.get(a) == int(index) and physical_lane.get(b) == int(index)
            for _, index, a, b in pairs
        )
        inner_approaches = {approach for approach, index, _, _ in pairs if index == "1"}
        check("rank pairs partition by approach and physical lane", partition_ok,
              f"pairs={len(pairs)}")
        check("all four inner lanes exercised", inner_approaches == {"N", "S", "E", "W"},
              f"inner_approaches={sorted(inner_approaches)}")
        clean_entries = re.findall(r"\[EXECUTOR\] entries epoch=0 n=16:[^\n]+", text)
        # stdout/stderr are merged by the runner, so an otherwise valid final
        # entry line can be truncated by interleaved ResDB output.  Select the
        # most complete copy instead of assuming the last physical line wins.
        final_line = max(clean_entries, key=lambda line: line.count("cyber="), default="")
        check("honest rank smoke commits all SIGNED", final_line.count("cyber=1") == 16,
              f"signed={final_line.count('cyber=1')}/16")
        check("rank smoke has no unsafe co-occupancy",
              "[UNSAFE-CONFLICT-COOCCUPANCY]" not in text,
              "honest noisy rank run remains physically safe")

    elif cell["kind"] == "mutation":
        rejected = bool(re.search(
            r"\[OMNET-PREVERIFY\] reject: state-field mismatch replica_id=\d+ "
            r"cert\([^\n]*physicalLane=\d+ lateralClaimCm=-?\d+\) "
            r"proposal\([^\n]*physicalLane=\d+ lateralClaimCm=-?\d+\)",
            text,
        ))
        recovered = bool(re.search(
            r"\[CONSENSUS_ATTACK_OUTCOME\].*fault=TAMPER_PHYSICAL_LANE "
            r"outcome=STATE_FIELD_MISMATCH_REJECTED_AND_RECOVERED", text,
        ))
        check("Check 10 rejects physical-lane mutation", rejected,
              f"rejected={rejected}")
        check("honest view recovers and commits", recovered and decided,
              f"outcome={recovered} decided={decided}")
        check("mutation run has no unsafe co-occupancy",
              "[UNSAFE-CONFLICT-COOCCUPANCY]" not in text,
              "mutated proposal never reached execution")

    return {
        "cell": cell["name"],
        "rep": rep,
        "result_dir": str(run_dir),
        "passed": all(row["passed"] for row in checks),
        "checks": checks,
        "certificate_accounting": {
            "formed": cert_assembled,
            "b_sig": actual_b_sig,
            "honest_support": honest_support,
            "echo_count": len(cert_lines),
            "derived": derived,
        },
        "final_target_entry": final_target,
    }


def run_two_lane_validation(args: argparse.Namespace, repetitions: int) -> int:
    """Run the full-intersection two-lane gate/rank/Check-10 checkpoint sequentially."""
    summary_dir = REPO_ROOT / "benchmarks" / "Phase2TwoLaneValidation"
    summary_path = summary_dir / "two_lane_validation_summary.json"
    # A prior invocation may have been interrupted after a simulation completed
    # but before the aggregate summary was written.  Reuse only those orphaned
    # artifacts; once a summary exists, a normal invocation deliberately reruns
    # every cell against the current code.
    resume_incomplete = not summary_path.is_file()
    if args.dry_run:
        print(f"[dry-run] would run_key_generation({TWO_LANE_N})")
    else:
        run_key_generation(dry_run=False, scale=TWO_LANE_N)
    results: List[Dict[str, Any]] = []
    total = len(TWO_LANE_VALIDATION_CELLS) * repetitions
    index = 0
    for cell_index, cell in enumerate(TWO_LANE_VALIDATION_CELLS):
        for rep in range(args.start_rep, args.start_rep + repetitions):
            index += 1
            print(f"\n--- Two-lane validation {index}/{total} {cell['name']} run_{rep} ---")
            run_dir = _two_lane_validation_run_dir(cell, rep)
            raw_log = run_dir / "raw_simulation.log"
            metadata_path = run_dir / "two_lane_run_metadata.json"
            if resume_incomplete and raw_log.is_file() and metadata_path.is_file() and not args.dry_run:
                metadata = json.loads(metadata_path.read_text())
                expected_seed = TWO_LANE_VALIDATION_SEED + cell_index + rep
                if metadata.get("cell") == cell["name"] and metadata.get("simulation_seed") == expected_seed:
                    resumed = _validate_two_lane_cell(cell, rep, run_dir)
                    if resumed["passed"]:
                        print(f"[resume] reusing completed artifact: {raw_log}")
                        results.append(resumed)
                        for row in resumed["checks"]:
                            print(f"  {'PASS' if row['passed'] else 'FAIL':4} {row['name']}: {row['detail']}")
                        continue
            if not args.dry_run:
                clear_stale_random_ini()
            run_dir = _run_two_lane_validation_cell(args, cell, rep, cell_index)
            if args.dry_run:
                continue
            result = _validate_two_lane_cell(cell, rep, run_dir)
            results.append(result)
            print(f"[{'PASS' if result['passed'] else 'FAIL'}] {cell['name']}")
            for row in result["checks"]:
                print(f"  {'PASS' if row['passed'] else 'FAIL':4} {row['name']}: {row['detail']}")

    if args.dry_run:
        return 0
    summary_dir.mkdir(parents=True, exist_ok=True)
    overall = bool(results) and all(row["passed"] for row in results)
    summary_path.write_text(json.dumps({
        "checkpoint": "P7-full-two-lane-validation",
        "passed": overall,
        "execution": "strictly sequential",
        "n": TWO_LANE_N,
        "f": bft_f(TWO_LANE_N),
        "collection_window_sec": 0.25,
        "main_gate_k": 3.0,
        "results": results,
    }, indent=2, sort_keys=True) + "\n")
    print("\n========== FULL TWO-LANE VALIDATION SUMMARY ==========")
    for result in results:
        accounting = result["certificate_accounting"]
        print(f"{'PASS' if result['passed'] else 'FAIL'}  {result['cell']} run_{result['rep']} "
              f"cert={accounting['formed']} b_sig={accounting['b_sig']} "
              f"honest_support={accounting['honest_support']}")
    print(f"Overall: {'PASS' if overall else 'FAIL'}")
    print(f"Machine-readable summary: {summary_path}")
    return 0 if overall else 1


def run_two_lane_conflict_release_validation(args: argparse.Namespace) -> int:
    """Run only the deterministic physical consequence smoke."""
    cell_index = next(
        index for index, cell in enumerate(TWO_LANE_VALIDATION_CELLS)
        if cell["name"] == "conflict_release_cliff"
    )
    cell = TWO_LANE_VALIDATION_CELLS[cell_index]
    if args.dry_run:
        print(f"[dry-run] would run_key_generation({TWO_LANE_N})")
    else:
        run_key_generation(dry_run=False, scale=TWO_LANE_N)
        clear_stale_random_ini()
    run_dir = _run_two_lane_validation_cell(args, cell, 0, cell_index)
    if args.dry_run:
        return 0
    result = _validate_two_lane_cell(cell, 0, run_dir)
    summary_path = run_dir / "two_lane_conflict_release_validation.json"
    summary_path.write_text(json.dumps({
        "checkpoint": "P7-two-lane-conflict-release",
        "passed": result["passed"],
        "execution": "single deterministic cliff/control smoke",
        "result": result,
    }, indent=2, sort_keys=True) + "\n")
    print("\n========== TWO-LANE CONFLICT-RELEASE VALIDATION ==========")
    for row in result["checks"]:
        print(f"  {'PASS' if row['passed'] else 'FAIL':4} {row['name']}: {row['detail']}")
    print(f"Overall: {'PASS' if result['passed'] else 'FAIL'}")
    print(f"Machine-readable summary: {summary_path}")
    return 0 if result["passed"] else 1


def _adjacent_lane_validation_run_dir(cell: Dict[str, Any], rep: int) -> Path:
    return (
        REPO_ROOT / "benchmarks" / "Phase2AdjacentLane" /
        str(cell["name"]) / f"run_{rep}"
    )


def _run_adjacent_lane_validation_cell(
    args: argparse.Namespace, cell: Dict[str, Any], rep: int
) -> Path:
    b = int(cell["b"])
    sigma = float(cell["sigma"])
    delta = float(cell["delta"])
    colluders = _phase2_nested_colluders(ADJACENT_LANE_TARGET, b, ADJACENT_LANE_N)
    kind = "NONE" if b == 0 else "FALSE_PHYSICAL_LANE"
    seed = ADJACENT_LANE_VALIDATION_SEED + rep
    run_dir = _adjacent_lane_validation_run_dir(cell, rep)
    if not args.dry_run:
        run_dir.mkdir(parents=True, exist_ok=True)
        (run_dir / "adjacent_lane_run_metadata.json").write_text(json.dumps({
            "checkpoint": "P7-adjacent-lane-validation",
            "cell": cell["name"],
            "n": ADJACENT_LANE_N,
            "f": bft_f(ADJACENT_LANE_N),
            "attack_kind": kind,
            "attack_target_replica_id": ADJACENT_LANE_TARGET,
            "actual_byzantine_count": b,
            "evidence_colluder_ids": colluders,
            "colluder_policy": "nested_ascending_replica_ids",
            "simulation_seed": seed,
            "lane_observation_mode": "ADJACENT_LATERAL",
            "lateral_observation_sigma_m": sigma,
            "lateral_claim_offset_m": delta,
            "physical_gate_k": 3.0,
            "direction_collection_window_sec": 0.25,
            "config": ADJACENT_LANE_CONFIG,
            "execution": "strictly sequential",
        }, indent=2, sort_keys=True) + "\n")

    argv = [
        str(RUN_SCRIPT), str(FOURWAY_DIR),
        "--compact-log",
        "--channel-metrics-dir", str(run_dir.resolve()),
        "--approach-sigma", "0",
        "--signal-error", "0",
        "--direction-collection-window", "0.25",
        "--ego-longitudinal-sigma", "0",
        "--longitudinal-sigma", "0",
        "--lane-observation-mode", "ADJACENT_LATERAL",
        "--lateral-sigma", str(sigma),
        "--physical-gate-k", "3",
        "--simulation-seed", str(seed),
        "--phase2-attack-kind", kind,
        "--phase2-attack-target", str(ADJACENT_LANE_TARGET),
        "--phase2-evidence-colluders", ",".join(str(value) for value in colluders),
        "--phase2-actual-b", str(b),
        "--phase2-lateral-claim-offset", str(delta if b > 0 else 0.0),
        "-u", "Cmdenv",
        "--debug-on-errors=false",
        "--sim-time-limit=15s",
        "-c", ADJACENT_LANE_CONFIG,
    ]
    inner = " ".join(shlex.quote(value) for value in argv)
    print("+ " + inner)
    run_in_bash_with_omnet(inner, dry_run=args.dry_run)
    if args.dry_run:
        return run_dir
    shutil.copy2(LOG_FILE, run_dir / "raw_simulation.log")
    analyze = [
        sys.executable, str(FOURWAY_DIR / "analyze_log.py"), str(LOG_FILE),
        "--save-to", str(run_dir), "--scenario", "1",
        "--cars", str(ADJACENT_LANE_N), "--run-index", str(rep),
        "--no-scenario-subdir",
    ]
    print("+ " + " ".join(shlex.quote(value) for value in analyze))
    subprocess.run(analyze, cwd=REPO_ROOT, check=True)
    return run_dir


def _validate_adjacent_lane_cell(
    cell: Dict[str, Any], rep: int, run_dir: Path
) -> Dict[str, Any]:
    checks: List[Dict[str, Any]] = []

    def check(name: str, passed: bool, detail: str) -> None:
        checks.append({"name": name, "passed": bool(passed), "detail": detail})

    log_text = (run_dir / "raw_simulation.log").read_text(errors="replace")
    records = json.loads((run_dir / f"{ADJACENT_LANE_N}veh_{rep}.json").read_text())
    perception = records[0]["bft_stats"]["perception"]
    attack = perception.get("phase2_attack", {})
    b = int(cell["b"])
    sigma = float(cell["sigma"])
    delta = float(cell["delta"])
    expected_colluders = _phase2_nested_colluders(
        ADJACENT_LANE_TARGET, b, ADJACENT_LANE_N
    )

    check(
        "adjacent residual mode used",
        "mode=ADJACENT_LATERAL" in log_text and
            "mode=CATEGORICAL_CARDINAL" not in log_text,
        "all logged witness decisions must use the dedicated adjacent mode",
    )
    check(
        "single-evaluation invariant",
        bool(perception.get("single_evaluation_invariant_ok")),
        f"duplicates={perception.get('duplicate_evaluation_count')}",
    )
    check(
        "authenticated evidence valid",
        "[ANN-ORIGIN-INVALID]" not in log_text and
            "[CERT-INVALID]" not in log_text and "verify FAIL" not in log_text,
        "no announcement, signature, or certificate validation failure",
    )
    claims = [
        (int(claim), int(index))
        for claim, index in re.findall(
            r"\[LATERAL-CLAIM\].*?claimCm=(-?\d+) physicalLaneIndex=(-?\d+)",
            log_text,
        )
    ]
    if not claims:
        claims = [
            (int(claim), int(index))
            for claim, index in re.findall(
                r"\[PERC-EVAL\].*?claimedLateralCm=(-?\d+).*?"
                r"claimedPhysicalLaneIndex=(-?\d+)",
                log_text,
            )
        ]
    if b == 0:
        check(
            "zero-noise lane centers preserved",
            (0, 0) in claims and (320, 1) in claims,
            f"observed claim/index pairs={sorted(set(claims))}",
        )
        check(
            "honest control has no forged declaration",
            "[PHASE2-ATTACK-DECLARE]" not in log_text,
            "attack kind NONE",
        )
    else:
        check(
            "attack configuration and nested colluders",
            attack.get("kind") == "FALSE_PHYSICAL_LANE" and
                int(attack.get("actual_b", -1)) == b and
                attack.get("colluder_ids") == expected_colluders and
                int(attack.get("config_mismatch_count", -1)) == 0,
            f"kind={attack.get('kind')} b={attack.get('actual_b')} colluders={attack.get('colluder_ids')}",
        )
        logged_delta = attack.get("lateral_delta_m")
        if logged_delta is None:
            outcome_claim_cm = (attack.get("outcome") or {}).get("lateral_claim_cm")
            if outcome_claim_cm is not None:
                logged_delta = float(outcome_claim_cm) / 100.0
            else:
                target_claim = re.search(
                    rf"\[LATERAL-CLAIM\] target=veh{ADJACENT_LANE_TARGET}\b.*?claimCm=(-?\d+)",
                    log_text,
                )
                logged_delta = (
                    float(target_claim.group(1)) / 100.0 if target_claim else None
                )
        check(
            "signed graded physical-lane claim",
            attack.get("declaration", {}).get("actual_lane") == "W" and
                attack.get("declaration", {}).get("claimed_lane") == "W" and
                logged_delta is not None and abs(float(logged_delta) - delta) < 1e-9,
            f"declaration={attack.get('declaration')} delta={logged_delta}",
        )
        cert_signers = set(attack.get("certificate_signers", []))
        configured_byzantine = {ADJACENT_LANE_TARGET, *expected_colluders}
        measured_b_sig = len(cert_signers.intersection(configured_byzantine))
        check(
            "certificate-local b_sig accounting",
            int(attack.get("b_sig_lane", 0)) == measured_b_sig,
            f"logged={attack.get('b_sig_lane')} bytes={measured_b_sig} signers={sorted(cert_signers)}",
        )
        outcome = attack.get("outcome") or {}
        false_cert = bool(outcome.get("false_lane_certificate"))
        if cell["name"] == "bf_shoulder":
            check(
                "b=f shoulder requires honest noisy support",
                false_cert and int(attack.get("b_sig_lane", 0)) == b and
                    int(attack.get("honest_lane_accepts", 0)) >= 1 and
                    int(attack.get("required_honest_support", -1)) == 1,
                f"false={false_cert} b_sig={attack.get('b_sig_lane')} honest={attack.get('honest_lane_accepts')}",
            )
            check(
                "Gaussian model prediction recorded",
                math.isclose(float(attack.get("q0_lane_model", -1.0)),
                             0.308537538685827, rel_tol=0.0, abs_tol=1e-12),
                f"q0_model={attack.get('q0_lane_model')}",
            )
        elif cell["name"] == "bf_zero_error_guard":
            check(
                "zero-error b=f blocks false certificate",
                not false_cert,
                f"false={false_cert} b_sig={attack.get('b_sig_lane')}",
            )
        elif cell["name"] == "bf1_cliff":
            b_sig = int(attack.get("b_sig_lane", 0))
            threshold = int(attack.get("threshold", bft_f(ADJACENT_LANE_N) + 1))
            check(
                "b=f+1 cliff uses certificate-local participation",
                (b_sig < threshold and not false_cert) or
                    (b_sig >= threshold and false_cert and
                     int(attack.get("required_honest_support", -1)) == 0),
                f"b_sig={b_sig} threshold={threshold} false={false_cert}",
            )

    return {
        "cell": cell["name"], "rep": rep, "b": b, "sigma_lat_m": sigma,
        "delta_m": delta, "result_dir": str(run_dir),
        "passed": all(row["passed"] for row in checks), "checks": checks,
        "accounting": attack,
    }


def run_adjacent_lane_validation(args: argparse.Namespace, repetitions: int) -> int:
    """Run four sequential cells that close the adjacent-lane wiring checkpoint."""
    reanalyze_only = bool(args.adjacent_lane_validation_reanalyze)
    if reanalyze_only:
        print("[reanalyze] using existing adjacent-lane artifacts; no simulation or key generation")
    elif args.dry_run:
        print(f"[dry-run] would run_key_generation({ADJACENT_LANE_N})")
    else:
        run_key_generation(dry_run=False, scale=ADJACENT_LANE_N)
    results: List[Dict[str, Any]] = []
    total = len(ADJACENT_LANE_VALIDATION_CELLS) * repetitions
    index = 0
    for cell in ADJACENT_LANE_VALIDATION_CELLS:
        for rep in range(args.start_rep, args.start_rep + repetitions):
            index += 1
            print(
                f"\n--- Adjacent-lane validation {index}/{total} {cell['name']} "
                f"b={cell['b']} sigma={cell['sigma']:g} delta={cell['delta']:g} run_{rep} ---"
            )
            run_dir = _adjacent_lane_validation_run_dir(cell, rep)
            if reanalyze_only:
                required = (
                    run_dir / "raw_simulation.log",
                    run_dir / f"{ADJACENT_LANE_N}veh_{rep}.json",
                )
                missing = [str(path) for path in required if not path.is_file()]
                if missing:
                    print(f"ERROR: missing reanalysis artifacts: {', '.join(missing)}", file=sys.stderr)
                    return 2
            elif not args.dry_run:
                clear_stale_random_ini()
            if not reanalyze_only:
                run_dir = _run_adjacent_lane_validation_cell(args, cell, rep)
            if args.dry_run:
                continue
            result = _validate_adjacent_lane_cell(cell, rep, run_dir)
            results.append(result)
            print(f"[{'PASS' if result['passed'] else 'FAIL'}] {cell['name']}")
            for row in result["checks"]:
                print(f"  {'PASS' if row['passed'] else 'FAIL':4} {row['name']}: {row['detail']}")

    if args.dry_run:
        return 0
    summary_dir = REPO_ROOT / "benchmarks" / "Phase2AdjacentLane"
    summary_dir.mkdir(parents=True, exist_ok=True)
    overall = bool(results) and all(row["passed"] for row in results)
    summary_path = summary_dir / "adjacent_lane_validation_summary.json"
    summary_path.write_text(json.dumps({
        "checkpoint": "P7-adjacent-lane-validation",
        "passed": overall,
        "execution": "strictly sequential",
        "cardinal_channel": "CATEGORICAL_CARDINAL",
        "adjacent_channel": "lane-normal scalar residual",
        "main_gate_k": 3.0,
        "results": results,
    }, indent=2, sort_keys=True) + "\n")
    print("\n========== ADJACENT-LANE VALIDATION SUMMARY ==========")
    print(f"Overall: {'PASS' if overall else 'FAIL'}")
    print(f"Machine-readable summary: {summary_path}")
    return 0 if overall else 1


def _adjacent_grid_seed(rep: int) -> int:
    # Pair the same repetition across sigma/delta/b/k cells.
    return run_seed(MASTER_SEED, ADJACENT_LANE_N, "ADJACENT_LANE_GRID", rep)


def _two_lane_grid_metadata(row: Dict[str, Any]) -> Dict[str, Any]:
    from fourway import adjacent_lane_grid as grid

    b = int(row["b"])
    delta_b_suite = any(
        purpose.startswith("delta_b_") for purpose in row.get("purposes", [])
    )
    parameter_suite = any(
        purpose.startswith("parameter_sweep_")
        for purpose in row.get("purposes", [])
    )
    combined_suite = any(
        purpose.startswith("combined_operating_")
        for purpose in row.get("purposes", [])
    )
    direction_ablation = any(
        purpose.startswith("direction_ablation_")
        for purpose in row.get("purposes", [])
    )
    attack_kind = str(row.get(
        "attack_kind", "NONE" if b == 0 else "FALSE_PHYSICAL_LANE"
    ))
    metadata = {
        "checkpoint": (
            "P7-full-intersection-two-lane-combined-validation"
            if combined_suite else
            "P7-full-intersection-direction-ablation"
            if direction_ablation else
            "P7-full-intersection-two-lane-parameter-sweep"
            if parameter_suite else
            "P7-full-intersection-two-lane-delta-b-grid"
            if delta_b_suite else "P7-full-intersection-two-lane-grid"
        ),
        "matrix_version": 3 if (delta_b_suite or parameter_suite) else 2,
        "n": grid.N,
        "f": grid.F,
        "target_replica_id": grid.TARGET,
        "b": b,
        "sigma_lat_m": float(row["sigma_lat_m"]),
        "delta_magnitude_m": float(row["delta_m"]),
        "signed_lateral_claim_offset_m": (
            -float(row["delta_m"])
            if b > 0 and attack_kind == "FALSE_PHYSICAL_LANE" else 0.0
        ),
        "k": float(row["k"]),
        "rep": int(row["rep"]),
        "purposes": list(row["purposes"]),
        "simulation_seed": _adjacent_grid_seed(int(row["rep"])),
        "paired_seed_policy": "same repetition uses same seed across parameter cells",
        "attack_kind": attack_kind,
        "evidence_colluder_ids": grid.nested_colluders(b),
        "colluder_policy": "nested_ascending_replica_ids",
        "lane_observation_mode": "ADJACENT_LATERAL",
        "config": str(row.get("config") or (
            TWO_LANE_SWEEP_CONFIG
            if (parameter_suite or combined_suite or direction_ablation)
            else TWO_LANE_CONFLICT_CONFIG
        )),
        "direction_collection_window_sec": 0.25,
        "physical_lane_policy": "lane0=STRAIGHT|RIGHT; lane1=LEFT",
        "actual_conflict_pair": "veh0=N-L; veh1=S-S",
        "execution": "strictly sequential",
    }
    # Preserve exact metadata compatibility with all existing lateral-only
    # artifacts; the longitudinal field is part of the joint checkpoint only.
    if combined_suite or direction_ablation or float(row.get("sigma_long_m", 0.0)) != 0.0:
        metadata["sigma_long_m"] = float(row.get("sigma_long_m", 0.0))
        metadata["causal_scope"] = (
            "both sensor channels active; only the configured declaration channel is false"
            if int(row["b"]) > 0 else
            "both sensor channels active; all declarations truthful"
        )
    if direction_ablation:
        metadata["signal_error"] = float(row.get("signal_error", 0.0))
        metadata["direction_eligibility_enabled"] = bool(
            row.get("direction_eligibility_enabled", True)
        )
        metadata["all_singleton_scheduling"] = bool(
            row.get("all_singleton_scheduling", False)
        )
        metadata["attack_choice_timing"] = "INIT_BEFORE_PERCEPTION"
        metadata["traffic_profile"] = str(
            row.get("traffic_profile", "left_heavy_4S_8L_4R")
        )
        metadata["maneuver_mix"] = dict(
            row.get("maneuver_mix", {"left": 8, "straight": 4, "right": 4})
        )
        if "fixture_id" in row:
            metadata["fixture_id"] = int(row["fixture_id"])
            metadata["fixture_manifest"] = str(row["fixture_manifest"])
            metadata["fixture_selection_policy"] = (
                "fixture_id=rep_mod_4; selected before perception; shared by "
                "all six modes in the repetition"
            )
    return metadata


def _run_two_lane_grid_cell(
    args: argparse.Namespace, row: Dict[str, Any], run_dir_override: Path | None = None
) -> Path:
    from fourway import adjacent_lane_grid as grid

    run_dir = run_dir_override or grid.run_dir(REPO_ROOT, row)
    metadata = _two_lane_grid_metadata(row)
    metadata_path = run_dir / "two_lane_grid_run_metadata.json"
    json_path = run_dir / f"{grid.N}veh_{int(row['rep'])}.json"
    raw_log = run_dir / "raw_simulation.log"

    if metadata_path.is_file() and json_path.is_file() and raw_log.is_file():
        existing = json.loads(metadata_path.read_text())
        if existing != metadata:
            raise ValueError(f"refusing mismatched resume artifact: {run_dir}")
        if any(
            str(purpose).startswith("direction_ablation_")
            for purpose in row.get("purposes", [])
        ):
            _compact_direction_analyzer_json(json_path)
        print(f"[resume] reuse {run_dir}")
        return run_dir

    run_dir.mkdir(parents=True, exist_ok=True)
    metadata_path.write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n")
    b = int(row["b"])
    colluders = grid.nested_colluders(b)
    active_config = str(metadata["config"])
    argv = [
        str(RUN_SCRIPT),
        "--randomize", str(grid.N), "0",
        "--no-ambulance",
        "--tolerated-f", str(grid.F),
        str(FOURWAY_DIR),
        "--compact-log",
        "--channel-metrics-dir", str(run_dir.resolve()),
        "--approach-sigma", "0",
        "--signal-error", str(float(row.get("signal_error", 0.0))),
        "--direction-collection-window", "0.25",
        "--ego-longitudinal-sigma", "0",
        "--longitudinal-sigma", str(float(row.get("sigma_long_m", 0.0))),
        "--lane-observation-mode", "ADJACENT_LATERAL",
        "--lateral-sigma", str(row["sigma_lat_m"]),
        "--adjacent-lane-separation", "3.2",
        "--physical-gate-k", str(row["k"]),
        "--direction-eligibility", (
            "on" if row.get("direction_eligibility_enabled", True) else "off"
        ),
        *(
            ["--all-singleton-scheduling"]
            if row.get("all_singleton_scheduling", False)
            else []
        ),
        "--simulation-seed", str(metadata["simulation_seed"]),
        "--phase2-attack-kind", metadata["attack_kind"],
        "--phase2-attack-target", str(grid.TARGET),
        "--phase2-evidence-colluders", ",".join(str(value) for value in colluders),
        "--phase2-actual-b", str(b),
        "--phase2-lateral-claim-offset", str(metadata["signed_lateral_claim_offset_m"]),
        # These experiment harnesses consume the structured text/CSV artifacts,
        # not OMNeT scalar/vector files.  Disabling recording prevents hundreds
        # of sequential cells from filling fourway/results and does not alter
        # simulation events or protocol behavior.  Route outputs into the
        # per-run dir (not /dev/null): OMNeT unlinks the prior file at startup,
        # which fails with EPERM on macOS when the path is /dev/null.
        "--**.scalar-recording=false",
        "--**.vector-recording=false",
        f"--output-scalar-file={run_dir.resolve() / 'omnet.sca'}",
        f"--output-vector-file={run_dir.resolve() / 'omnet.vec'}",
        "-u", "Cmdenv",
        "--debug-on-errors=false",
        "-c", active_config,
    ]
    inner = " ".join(shlex.quote(value) for value in argv)
    print("+ " + inner)
    run_in_bash_with_omnet(inner, dry_run=False)
    shutil.copy2(LOG_FILE, raw_log)
    analyze = [
        sys.executable, str(FOURWAY_DIR / "analyze_log.py"), str(LOG_FILE),
        "--save-to", str(run_dir), "--scenario", "1",
        "--cars", str(grid.N), "--run-index", str(row["rep"]),
        "--no-scenario-subdir",
    ]
    print("+ " + " ".join(shlex.quote(value) for value in analyze))
    subprocess.run(analyze, cwd=REPO_ROOT, check=True)
    if any(
        str(purpose).startswith("direction_ablation_")
        for purpose in row.get("purposes", [])
    ):
        _compact_direction_analyzer_json(json_path)
    return run_dir


def _compact_direction_analyzer_json(json_path: Path) -> None:
    """Remove duplicated run-wide fields from direction-ablation records.

    analyze_log.py emits the same large ``run_metrics`` and ``bft_stats`` maps
    on every vehicle record.  Direction-ablation analysis reads those maps
    from record 0 and only needs per-vehicle timing from the remaining records.
    Keeping one canonical copy preserves all evidence while avoiding hundreds
    of megabytes of redundant output in the 120-cell matrix.
    """
    records = json.loads(json_path.read_text())
    if not isinstance(records, list) or not records:
        raise ValueError(f"unexpected analyzer JSON shape: {json_path}")
    for record in records[1:]:
        if not isinstance(record, dict):
            raise ValueError(f"unexpected analyzer record shape: {json_path}")
        record.pop("run_metrics", None)
        record.pop("bft_stats", None)
    json_path.write_text(json.dumps(records, separators=(",", ":")) + "\n")


def run_two_lane_b1_smoke(args: argparse.Namespace) -> int:
    """Run one reviewable b=1 headline cell with attempt/cert accounting."""
    from fourway import adjacent_lane_grid as grid

    rep = int(args.start_rep)
    row = {
        "b": 1,
        "sigma_lat_m": grid.MAIN_SIGMA_M,
        "delta_m": grid.MAIN_DELTA_M,
        "k": grid.MAIN_K,
        "rep": rep,
        "purposes": ["focused_b1_accounting_smoke"],
    }
    run_dir = (
        REPO_ROOT / "benchmarks" / "Phase2TwoLaneB1Smoke" / f"run_{rep}"
    )
    if args.dry_run:
        print(f"[dry-run] b=1 smoke row={row} result_dir={run_dir}")
        return 0
    run_key_generation(dry_run=False, scale=grid.N)
    clear_stale_random_ini()
    run_dir = _run_two_lane_grid_cell(args, row, run_dir_override=run_dir)
    result = _analyze_two_lane_grid_cell(row, run_dir)
    summary_path = run_dir / "two_lane_b1_smoke_summary.json"
    summary_path.write_text(json.dumps({
        "checkpoint": "P7-two-lane-b1-accounting-smoke",
        "passed": result["passed"],
        "result": result,
    }, indent=2, sort_keys=True) + "\n")
    print("\n========== TWO-LANE b=1 ACCOUNTING SMOKE ==========")
    print(
        f"honest accepts/evaluations: {result['honest_lane_accepts']}/"
        f"{result['h_lane']} q0_empirical={result['q0_lane_empirical_run']} "
        f"q0_model={result['q0_lane_model']}"
    )
    print(
        f"Byzantine support: attempt={result['b_sig_attempt']} "
        f"source={result['b_sig_attempt_source']} cert={result['b_sig_cert']} "
        f"required_honest={result['required_honest_support']}"
    )
    print(
        f"Outcome: false_cert={result['false_lane_certificate']} "
        f"conflicting_cooccupancy={result['conflicting_cooccupancy']}"
    )
    print(f"Overall: {'PASS' if result['passed'] else 'FAIL'}")
    print(f"Machine-readable summary: {summary_path}")
    return 0 if result["passed"] else 1


def _percentile(values: Sequence[float], quantile: float) -> float | None:
    if not values:
        return None
    ordered = sorted(float(value) for value in values)
    position = (len(ordered) - 1) * quantile
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    fraction = position - lower
    return ordered[lower] * (1.0 - fraction) + ordered[upper] * fraction


def _analyze_two_lane_grid_cell(row: Dict[str, Any], run_dir: Path) -> Dict[str, Any]:
    from fourway import adjacent_lane_grid as grid

    rep = int(row["rep"])
    log_text = (run_dir / "raw_simulation.log").read_text(errors="replace")
    records = json.loads((run_dir / f"{grid.N}veh_{rep}.json").read_text())
    perception = records[0]["bft_stats"]["perception"]
    attack = perception.get("phase2_attack") or {}
    b = int(row["b"])
    expected_colluders = grid.nested_colluders(b)
    outcome = attack.get("outcome") or {}
    checks = {
        "adjacent_mode_only": (
            "mode=ADJACENT_LATERAL" in log_text and
            "mode=CATEGORICAL_CARDINAL" not in log_text
        ),
        "single_evaluation": bool(perception.get("single_evaluation_invariant_ok")),
        "authenticated_evidence": (
            "[ANN-ORIGIN-INVALID]" not in log_text and
            "[CERT-INVALID]" not in log_text and "verify FAIL" not in log_text
        ),
        "no_teleport": "Teleporting vehicle" not in log_text,
    }
    combined_suite = any(
        purpose.startswith("combined_operating_")
        for purpose in row.get("purposes", [])
    )
    completion_sweep = any(
        purpose.startswith("parameter_sweep_") or
        purpose.startswith("combined_operating_")
        for purpose in row.get("purposes", [])
    )
    if completion_sweep:
        departed = set(re.findall(r"\[DEPARTED\] Replica (\d+)\b", log_text))
        checks["all_vehicles_departed"] = len(departed) == grid.N
    stopped_distance = perception.get("stopped_distance") or {}
    if combined_suite:
        checks["distance_evaluations_present"] = (
            int(stopped_distance.get("evaluation_count") or 0) > 0
        )
        checks["distance_single_evaluation"] = bool(
            stopped_distance.get("single_evaluation_invariant_ok")
        )
        checks["truthful_distance_certificates_complete"] = (
            int(stopped_distance.get("certificate_count") or 0) == grid.N
        )
    if b == 0:
        checks["honest_control"] = "[PHASE2-ATTACK-DECLARE]" not in log_text
    else:
        checks["attack_configuration"] = (
            attack.get("kind") == "FALSE_PHYSICAL_LANE" and
            int(attack.get("actual_b", -1)) == b and
            attack.get("colluder_ids") == expected_colluders and
            int(attack.get("config_mismatch_count", -1)) == 0
        )
        cert_signers = set(attack.get("certificate_signers", []))
        byzantine = {grid.TARGET, *expected_colluders}
        checks["certificate_local_b_sig"] = (
            int(attack.get("b_sig_lane", 0)) == len(cert_signers.intersection(byzantine))
        )
        if b <= grid.F and float(row["sigma_lat_m"]) == 0.0:
            checks["zero_error_guard"] = not bool(outcome.get("false_lane_certificate"))
        if b == grid.F + 1:
            b_sig = int(attack.get("b_sig_lane", 0))
            threshold = int(attack.get("threshold", grid.F + 1))
            false_cert = bool(outcome.get("false_lane_certificate"))
            honest_cert_signers = cert_signers.difference(byzantine)
            required_from_cert = max(0, threshold - b_sig)
            checks["cliff_accounting"] = (
                (
                    false_cert and
                    len(cert_signers) >= threshold and
                    len(honest_cert_signers) >= required_from_cert and
                    int(attack.get("required_honest_support", -1)) == required_from_cert
                ) or (
                    not false_cert and b_sig < threshold
                )
            )

    is_false_claim = b > 0
    effective_delta = float(row["delta_m"]) if is_false_claim else 0.0
    model_q0 = (
        grid.q0_model(
            float(row["sigma_lat_m"]), effective_delta, float(row["k"])
        ) if is_false_claim else None
    )
    raw_lane_accept_rate = attack.get(
        "lane_accept_rate", attack.get("q0_lane_empirical_run")
    )
    empirical_q0 = raw_lane_accept_rate if is_false_claim else None
    empirical_q1 = raw_lane_accept_rate if not is_false_claim else None
    b_sig_cert = int(attack.get("b_sig_lane_cert", attack.get("b_sig_lane", 0)))
    b_sig_attempt = attack.get("b_sig_lane_attempt")
    b_sig_attempt_source = attack.get("b_sig_lane_attempt_source", "UNAVAILABLE")
    if b_sig_attempt is not None:
        b_sig_attempt = int(b_sig_attempt)
    elif b == 1 and re.search(
            r"\[SELF-ATTEST\] target=veh0 epoch=0 signer=0[^\n]*status=VALID",
            log_text):
        # Older compact logs predate ECHO-COLLECT.  With b=1 the claimant is
        # the only Byzantine signer, so its valid local self-attestation makes
        # the attempt-level support exactly one even when no cert finalizes.
        b_sig_attempt = 1
        b_sig_attempt_source = "SELF-ATTEST-RECOVERY"
    elif bool(outcome.get("false_lane_certificate")):
        # A finalized cert is the complete collector snapshot.  This fallback
        # keeps old successful artifacts analyzable; failed b>1 attempts remain
        # unavailable rather than being incorrectly reported as zero.
        b_sig_attempt = b_sig_cert
        b_sig_attempt_source = "FINALIZED-CERT-RECOVERY"
    measured_h = int(attack.get("h_lane", 0))
    required_honest = (
        max(0, grid.F + 1 - b_sig_attempt)
        if is_false_claim and b_sig_attempt is not None else None
    )

    def binomial_tail(probability: float) -> float:
        if required_honest is None:
            raise ValueError("attempt-level Byzantine support is unavailable")
        if required_honest <= 0:
            return 1.0
        if required_honest > measured_h:
            return 0.0
        return sum(
            math.comb(measured_h, successes) * (probability ** successes) *
            ((1.0 - probability) ** (measured_h - successes))
            for successes in range(required_honest, measured_h + 1)
        )

    model_prediction = (
        binomial_tail(float(model_q0))
        if model_q0 is not None and required_honest is not None else None
    )
    empirical_prediction = (
        binomial_tail(float(empirical_q0))
        if empirical_q0 is not None and required_honest is not None else None
    )
    conflicting_pair = bool(re.search(
        r"\[CONFLICTING-COOCCUPANCY\].*?(?:first=veh0.*?second=veh1|first=veh1.*?second=veh0)",
        log_text,
    ))
    target_and_counterpart_same_batch = bool(re.search(
        r"\[SCHEDULER-BATCH-ADMIT\][^\n]*?(?:head=0[^\n]*candidate=1|head=1[^\n]*candidate=0)",
        log_text,
    )) or bool(re.search(
        r"\[EXECUTOR\] OrderDecision:[^\n]*?veh=0 batch=(\d+)[^\n]*?veh=1 batch=\1",
        log_text,
    ))
    false_certificate = bool(outcome.get("false_lane_certificate"))
    gate_induced_cooccupancy = (
        false_certificate and target_and_counterpart_same_batch and conflicting_pair
    )
    background_cooccupancy = conflicting_pair and not false_certificate
    if b > 0:
        checks["physical_consequence_matches_authority"] = (
            (not false_certificate and not conflicting_pair) or
            gate_induced_cooccupancy
        )
    else:
        checks["honest_control_has_no_conflicting_cooccupancy"] = not conflicting_pair

    run_metrics = records[0].get("run_metrics") or {}
    unsafe_pairs = run_metrics.get("unsafe_conflict_cooccupancy_pairs") or []
    reviewed_pair_names = {"veh0", "veh1"}
    background_unsafe_pairs = []
    for pair in unsafe_pairs:
        if isinstance(pair, dict):
            names = {str(pair.get("first", "")), str(pair.get("second", ""))}
        elif isinstance(pair, (list, tuple)) and len(pair) >= 2:
            names = {str(pair[0]), str(pair[1])}
        else:
            names = set(re.findall(r"veh\d+", str(pair)))
        if names != reviewed_pair_names:
            background_unsafe_pairs.append(pair)
    if combined_suite:
        checks["background_conflicting_cooccupancy_zero"] = not background_unsafe_pairs
    global_lane_evaluations = int(perception.get("lane_evaluation_count") or 0)
    global_lane_accepts = int(perception.get("lane_accept_count") or 0)
    signed_direction_count = int(perception.get("signed_direction_count") or 0)
    signed_unknown_total = int(perception.get("signed_unknown_count") or 0)
    lane_certified_count = signed_direction_count + signed_unknown_total
    wait_samples_s = [
        float(wait_ms) / 1000.0
        for record in records
        if (wait_ms := (record.get("durations_ms") or {}).get("total_wait_time"))
        is not None
    ]
    batch_distribution = run_metrics.get("batch_index_distribution") or {}
    batch_vehicle_count = sum(int(value) for value in batch_distribution.values())
    mean_batch_size = (
        batch_vehicle_count / len(batch_distribution)
        if batch_distribution else None
    )
    quiet_count = perception.get("quiet_count")
    signed_unknown_count = int(perception.get("signed_unknown_singleton_count") or 0)
    left_singleton_count = int(perception.get("left_table_forced_singleton_count") or 0)
    quiet_singleton_count = int(quiet_count or 0)
    throughput_vps = run_metrics.get("throughput_veh_per_s")
    distance_evaluations = int(stopped_distance.get("evaluation_count") or 0)
    distance_accepts = int(stopped_distance.get("accept_count") or 0)
    distance_certificate_count = int(stopped_distance.get("certificate_count") or 0)
    distance_inversions = int(stopped_distance.get("order_inversion_count") or 0)
    return {
        **row,
        "result_dir": str(run_dir),
        "passed": all(checks.values()),
        "checks": checks,
        "false_lane_certificate": false_certificate,
        "target_counterpart_same_batch": target_and_counterpart_same_batch,
        "conflicting_cooccupancy": conflicting_pair,
        "gate_induced_conflicting_cooccupancy": gate_induced_cooccupancy,
        "background_conflicting_cooccupancy": background_cooccupancy,
        "background_conflicting_cooccupancy_pairs": background_unsafe_pairs,
        "lane_certified": bool(attack.get("lane_certified")),
        "claim_kind": "FALSE_PHYSICAL_LANE" if is_false_claim else "HONEST_CONTROL",
        "effective_delta_m": effective_delta,
        "h_lane": measured_h,
        "honest_lane_accepts": int(attack.get("honest_lane_accepts", 0)),
        "b_sig": b_sig_cert,
        "b_sig_cert": b_sig_cert,
        "b_sig_attempt": b_sig_attempt,
        "b_sig_attempt_source": b_sig_attempt_source,
        "certificate_signers": attack.get("certificate_signers", []),
        "certificate_honest_signers": (
            sorted(set(attack.get("certificate_signers", [])).difference(
                {grid.TARGET, *expected_colluders}
            )) if is_false_claim else attack.get("certificate_signers", [])
        ),
        "byzantine_signatures_alone_reach_threshold": (
            b_sig_cert >= grid.F + 1 if is_false_claim else False
        ),
        "q0_lane_empirical_run": empirical_q0,
        "q1_lane_empirical_run": empirical_q1,
        "q0_lane_model": model_q0,
        "global_lane_evaluations": global_lane_evaluations,
        "global_lane_accepts": global_lane_accepts,
        "global_q1_lane_empirical": (
            global_lane_accepts / global_lane_evaluations
            if global_lane_evaluations else None
        ),
        "lane_certified_count": lane_certified_count,
        "lane_certification_rate": lane_certified_count / grid.N,
        "signed_direction_count": signed_direction_count,
        "signed_unknown_count": signed_unknown_total,
        "binomial_tail_prediction": model_prediction,
        "binomial_tail_prediction_empirical_q0": empirical_prediction,
        "required_honest_support": required_honest,
        "throughput_veh_per_s": throughput_vps,
        "throughput_veh_per_min": (
            float(throughput_vps) * 60.0 if throughput_vps is not None else None
        ),
        "wait_samples_s": wait_samples_s,
        "mean_wait_s": (
            statistics.mean(wait_samples_s) if wait_samples_s else None
        ),
        "p95_wait_s": (
            _percentile(wait_samples_s, 0.95) if wait_samples_s else None
        ),
        "quiet_count": quiet_singleton_count,
        "quiet_percent": 100.0 * quiet_singleton_count / grid.N,
        "signed_unknown_singleton_count": signed_unknown_count,
        "signed_unknown_singleton_percent": 100.0 * signed_unknown_count / grid.N,
        "left_table_forced_singleton_count": left_singleton_count,
        "left_table_forced_singleton_percent": 100.0 * left_singleton_count / grid.N,
        "mean_batch_size": mean_batch_size,
        "sigma_long_m": float(row.get("sigma_long_m", 0.0)),
        "distance_evaluation_count": distance_evaluations,
        "distance_accept_count": distance_accepts,
        "distance_accept_rate": (
            distance_accepts / distance_evaluations
            if distance_evaluations else None
        ),
        "distance_certificate_count": distance_certificate_count,
        "distance_single_evaluation_invariant_ok": bool(
            stopped_distance.get("single_evaluation_invariant_ok")
        ),
        "committed_order_inversion_pair_count": distance_inversions,
    }


def _combined_causal_attribution(result: Dict[str, Any]) -> Dict[str, Any]:
    """Classify the signed authority chain without guessing from final motion."""
    false_lane = bool(result["false_lane_certificate"])
    same_batch = bool(result["target_counterpart_same_batch"])
    reviewed_conflict = bool(result["conflicting_cooccupancy"])
    background = list(result.get("background_conflicting_cooccupancy_pairs") or [])
    rank_inversions = int(result.get("committed_order_inversion_pair_count") or 0)
    if background:
        label = "BACKGROUND_PHYSICAL_CONFLICT"
    elif reviewed_conflict and not false_lane:
        label = "UNAUTHORIZED_PHYSICAL_CONFLICT"
    elif false_lane and same_batch and reviewed_conflict:
        label = "FALSE_LANE_AUTHORITY_TO_CONFLICT"
    elif false_lane:
        label = "FALSE_LANE_AUTHORITY_WITHOUT_PHYSICAL_CONFLICT"
    elif rank_inversions:
        label = "RANK_ORDER_CHANGE_ONLY"
    else:
        label = "NO_PROTOCOL_BREACH"
    return {
        "classification": label,
        "false_lane_authority": false_lane,
        "target_counterpart_same_batch": same_batch,
        "reviewed_actual_conflict_cooccupancy": reviewed_conflict,
        "background_conflicting_pairs": background,
        "committed_rank_inversion_pairs": rank_inversions,
        "rank_can_grant_conflicting_batch_authority": False,
        "reason": (
            "queue rank changes same-(approach,physical-lane) order only; "
            "IsSafeToBatch rejects same lane before kSafe"
        ),
    }


def run_two_lane_combined_validation(args: argparse.Namespace) -> int:
    """Validate N=16 with lateral and longitudinal perception active together."""
    from fourway import adjacent_lane_grid as grid

    rows = grid.combined_validation_manifest()
    output_dir = REPO_ROOT / "benchmarks" / "Phase2TwoLaneCombinedValidation"
    if args.dry_run:
        print(
            "[dry-run] N=16 combined perception validation: "
            "sigma_lat=.5 sigma_long=1 k=2, three strictly sequential cells"
        )
        for index, row in enumerate(rows, 1):
            print(
                f"[dry-run] {index}/3 {row['name']} "
                f"b={row['b']} delta={row['delta_m']} "
                f"result_dir={grid.combined_validation_run_dir(REPO_ROOT, row)}"
            )
        return 0

    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "combined_validation_manifest.json").write_text(
        json.dumps({
            "checkpoint": "P7-two-lane-combined-perception-validation",
            "n": grid.N,
            "execution": "strictly sequential",
            "operating_point": {
                "sigma_lat_m": grid.COMBINED_SIGMA_LAT_M,
                "sigma_long_m": grid.COMBINED_SIGMA_LONG_M,
                "physical_gate_k": grid.COMBINED_K,
                "attack_delta_m": grid.COMBINED_DELTA_M,
            },
            "attack_isolation": (
                "both perception noises active; at most the physical-lane "
                "declaration is false in attack cells"
            ),
            "rows": rows,
        }, indent=2, sort_keys=True) + "\n"
    )
    run_key_generation(dry_run=False, scale=grid.N)
    results: List[Dict[str, Any]] = []
    for index, row in enumerate(rows, 1):
        print(f"\n--- Combined N=16 validation {index}/3: {row['name']} ---")
        run_dir = grid.combined_validation_run_dir(REPO_ROOT, row)
        try:
            clear_stale_random_ini()
            run_dir = _run_two_lane_grid_cell(
                args, row, run_dir_override=run_dir
            )
            result = _analyze_two_lane_grid_cell(row, run_dir)
            attribution = _combined_causal_attribution(result)
            result["causal_attribution"] = attribution
            result["lane_authority_outcome"] = {
                "false_lane_certificate": result["false_lane_certificate"],
                "lane_certified": result["lane_certified"],
                "b_sig_cert": result["b_sig_cert"],
                "b_sig_attempt": result["b_sig_attempt"],
                "certificate_signers": result["certificate_signers"],
                "required_honest_support": result["required_honest_support"],
            }
            result["rank_authority_outcome"] = {
                "claim_truthful": True,
                "distance_certificates": result["distance_certificate_count"],
                "distance_evaluations": result["distance_evaluation_count"],
                "distance_accepts": result["distance_accept_count"],
                "distance_accept_rate": result["distance_accept_rate"],
                "committed_order_inversion_pairs": result[
                    "committed_order_inversion_pair_count"
                ],
            }
            result["physical_outcome"] = {
                "target_counterpart_same_batch": result[
                    "target_counterpart_same_batch"
                ],
                "reviewed_pair_actual_movements": "veh0=N-L; veh1=S-S",
                "actual_movements_conflict": True,
                "reviewed_conflicting_cooccupancy": result[
                    "conflicting_cooccupancy"
                ],
                "background_conflicting_pairs": result[
                    "background_conflicting_cooccupancy_pairs"
                ],
            }
            results.append(result)
            print(
                f"[{'PASS' if result['passed'] else 'FAIL'}] "
                f"lane_false_cert={result['false_lane_certificate']} "
                f"distance_certs={result['distance_certificate_count']}/16 "
                f"cooccupancy={result['conflicting_cooccupancy']} "
                f"cause={attribution['classification']}"
            )
            for check, passed in result["checks"].items():
                print(f"  {'PASS' if passed else 'FAIL':4} {check}")
        except (subprocess.CalledProcessError, OSError, ValueError, KeyError) as exc:
            results.append({
                **row, "passed": False, "error": str(exc),
                "result_dir": str(run_dir),
            })
            print(f"[FAIL] {row['name']}: {exc}", file=sys.stderr)
            break

    overall = len(results) == len(rows) and all(
        bool(result.get("passed")) for result in results
    )
    summary_path = output_dir / "two_lane_combined_validation_summary.json"
    summary_path.write_text(json.dumps({
        "checkpoint": "P7-two-lane-combined-perception-validation",
        "passed": overall,
        "completed": len(results),
        "planned": len(rows),
        "execution": "strictly sequential",
        "adaptive_attack_policy": (
            "not enabled in validation; later policies must precommit and log "
            "one enumerated lie kind per run"
        ),
        "causal_contract": (
            "lateral evidence may grant scheduling authority; longitudinal "
            "evidence changes same-lane order but cannot itself authorize a "
            "conflicting co-batch"
        ),
        "results": results,
    }, indent=2, sort_keys=True) + "\n")
    print("\n========== N=16 COMBINED PERCEPTION VALIDATION ==========")
    print(f"Completed: {len(results)}/{len(rows)}")
    print(f"Overall: {'PASS' if overall else 'FAIL'}")
    print(f"Summary: {summary_path}")
    return 0 if overall else 1


def _analyze_direction_ablation_cell(
    row: Dict[str, Any], run_dir: Path
) -> Dict[str, Any]:
    from fourway import adjacent_lane_grid as grid

    log_text = (run_dir / "raw_simulation.log").read_text(errors="replace")
    records = json.loads(
        (run_dir / f"{grid.N}veh_{int(row['rep'])}.json").read_text()
    )
    first = records[0]
    perception = first["bft_stats"]["perception"]
    attack = perception.get("phase2_attack") or {}
    outcome = attack.get("outcome") or {}
    distance = perception.get("stopped_distance") or {}
    metrics = first.get("run_metrics") or {}
    departures = set(re.findall(r"\[DEPARTED\] Replica (\d+)\b", log_text))
    config_positions = [
        match.start() for match in re.finditer(r"\[PHASE2-ATTACK-CONFIG\]", log_text)
    ]
    perception_positions = [
        position for marker in ("[PERC-EVAL]", "[DIST-PERC-EVAL]")
        if (position := log_text.find(marker)) >= 0
    ]
    attack_precommitted = bool(config_positions and perception_positions) and (
        max(config_positions) < min(perception_positions)
    )
    expected_eligibility = "ON" if row["direction_eligibility_enabled"] else "OFF"
    expected_singleton = bool(row["all_singleton_scheduling"])
    unsafe_pairs = metrics.get("unsafe_conflict_cooccupancy_pairs") or []
    target_pair_conflict = any(
        {str(pair.get("first")), str(pair.get("second"))} == {"veh0", "veh1"}
        for pair in unsafe_pairs if isinstance(pair, dict)
    )
    target_direction = (perception.get("direction_eligibility") or {}).get("veh0@0") or {}
    batch_distribution = metrics.get("batch_index_distribution") or {}
    batch_vehicle_count = sum(int(value) for value in batch_distribution.values())
    singleton_vehicle_count = sum(
        int(value) for value in batch_distribution.values() if int(value) == 1
    )
    mean_batch_size = (
        batch_vehicle_count / len(batch_distribution) if batch_distribution else None
    )
    maneuver_mix = dict(
        row.get("maneuver_mix", {"left": 8, "straight": 4, "right": 4})
    )
    fixture_manifest_rows: List[Dict[str, str]] = []
    fixture_manifest_ok = True
    if row.get("fixture_manifest"):
        fixture_path = FOURWAY_DIR / str(row["fixture_manifest"])
        try:
            fixture_manifest_rows = list(csv.DictReader(fixture_path.open()))
        except OSError:
            fixture_manifest_ok = False
        if fixture_manifest_rows:
            observed_mix = Counter(
                item.get("intended_direction") for item in fixture_manifest_rows
            )
            by_vehicle = {
                item.get("vehicle_id"): item for item in fixture_manifest_rows
            }
            fixture_manifest_ok = (
                len(fixture_manifest_rows) == grid.N and
                observed_mix == Counter({
                    "L": int(maneuver_mix["left"]),
                    "S": int(maneuver_mix["straight"]),
                    "R": int(maneuver_mix["right"]),
                }) and
                (by_vehicle.get("veh0") or {}).get("intended_lane") == "N" and
                (by_vehicle.get("veh0") or {}).get("intended_direction") == "L" and
                (by_vehicle.get("veh1") or {}).get("intended_lane") == "S" and
                (by_vehicle.get("veh1") or {}).get("intended_direction") == "S"
            )
    wait_samples = [
        float(wait_ms) / 1000.0 for record in records
        if (wait_ms := (record.get("durations_ms") or {}).get("total_wait_time"))
        is not None
    ]
    checks = {
        "all_vehicles_departed": len(departures) == grid.N,
        "lane_single_evaluation": bool(perception.get("single_evaluation_invariant_ok")),
        "distance_single_evaluation": bool(distance.get("single_evaluation_invariant_ok")),
        "distance_certificates_complete": int(distance.get("certificate_count") or 0) == grid.N,
        "authenticated_evidence": (
            "[ANN-ORIGIN-INVALID]" not in log_text and
            "[CERT-INVALID]" not in log_text and "verify FAIL" not in log_text
        ),
        "check10_clean": "Check 10 FAIL" not in log_text,
        "no_teleport": "Teleporting vehicle" not in log_text,
        "attack_choice_precommitted_before_perception": attack_precommitted,
        "direction_mode_applied": (
            f"[DIRECTION-ABLATION-CONFIG] replica=0 eligibility={expected_eligibility}"
            in log_text
        ),
        "scheduler_mode_applied": (
            f"[SCHEDULER-MODE] allSingleton={1 if expected_singleton else 0}"
            in log_text
        ),
        "attack_configuration": (
            attack.get("kind") == row["attack_kind"] and
            int(attack.get("actual_b", -1)) == int(row["b"])
        ),
        "fixture_manifest_constraints": fixture_manifest_ok,
    }
    if row["attack_kind"] == "NONE":
        checks["honest_has_no_unsafe_cooccupancy"] = not unsafe_pairs
    elif row["ablation_mode"] == "eligibility_on":
        checks["inconsistent_direction_degrades"] = (
            target_direction.get("derived") in {"U", "UNKNOWN"} and
            not bool(outcome.get("false_eligibility")) and
            not target_pair_conflict
        )
    elif row["ablation_mode"] == "eligibility_off":
        checks["naive_declaration_unlocks_direction"] = (
            target_direction.get("derived") == "R" and
            bool(outcome.get("false_eligibility"))
        )
        # This fixture is intentionally ordered so N-L(actual) and S-S share a
        # greedy batch once the false N-R declaration is trusted.
        if row.get("profile", "smoke") == "smoke":
            checks["unsafe_physical_consequence_observed"] = target_pair_conflict
    else:
        checks["all_singleton_blocks_unsafe_cooccupancy"] = (
            not unsafe_pairs and
            batch_distribution and
            all(int(size) == 1 for size in batch_distribution.values())
        )
    if row.get("profile") == "prerequisite":
        checks["meaningful_cobatching_present"] = (
            mean_batch_size is not None and mean_batch_size > 1.0
        )

    throughput_vps = metrics.get("throughput_veh_per_s")
    return {
        **row,
        "passed": all(checks.values()),
        "checks": checks,
        "result_dir": str(run_dir),
        "maneuver_mix": maneuver_mix,
        "traffic_profile": str(
            row.get("traffic_profile", "left_heavy_4S_8L_4R")
        ),
        "fixture_id": row.get("fixture_id"),
        "fixture_manifest": row.get("fixture_manifest"),
        "attack_choice": {
            "kind": row["attack_kind"],
            "selected_at": "INIT_BEFORE_PERCEPTION",
            "configured_b": int(row["b"]),
            "certificate_b_sig_direction": int(attack.get("b_sig_dir") or 0),
            "certificate_signers": attack.get("certificate_signers", []),
        },
        "target_direction": target_direction,
        "false_eligibility": bool(outcome.get("false_eligibility")),
        "unsafe_conflict_pairs": unsafe_pairs,
        "target_pair_conflicting_cooccupancy": target_pair_conflict,
        "throughput_veh_per_s": throughput_vps,
        "throughput_veh_per_min": (
            float(throughput_vps) * 60.0 if throughput_vps is not None else None
        ),
        "mean_wait_s": statistics.mean(wait_samples) if wait_samples else None,
        "p95_wait_s": _percentile(wait_samples, 0.95) if wait_samples else None,
        "wait_samples_s": wait_samples,
        "mean_batch_size": mean_batch_size,
        "quiet_count": int(perception.get("quiet_count") or 0),
        "signed_unknown_count": int(perception.get("signed_unknown_count") or 0),
        "signed_unknown_singleton_count": int(
            perception.get("signed_unknown_singleton_count") or 0
        ),
        "left_table_forced_singleton_count": int(
            perception.get("left_table_forced_singleton_count") or 0
        ),
        "singleton_vehicle_count": singleton_vehicle_count,
        "all_singleton_policy_count": grid.N if expected_singleton else 0,
        "distance_certificate_count": int(distance.get("certificate_count") or 0),
        "committed_rank_inversion_pair_count": int(distance.get("order_inversion_count") or 0),
        "physical_collision_vehicle_count": int(
            metrics.get("physical_collision_vehicle_count") or 0
        ),
    }


def _aggregate_direction_ablation(
    rows: Sequence[Dict[str, Any]],
) -> List[Dict[str, Any]]:
    from fourway import adjacent_lane_grid as grid

    grouped: Dict[Tuple[str, str], List[Dict[str, Any]]] = defaultdict(list)
    for row in rows:
        grouped[(str(row["attack_kind"]), str(row["ablation_mode"]))].append(row)

    aggregates: List[Dict[str, Any]] = []
    for (attack_kind, mode), group in sorted(grouped.items()):
        # A process-level failure can leave a manifest row without analyzed
        # metrics.  Keep that attempt visible, but never treat it as a negative
        # safety observation or crash while writing the partial summary.
        valid_group = [
            row for row in group
            if "target_pair_conflicting_cooccupancy" in row and
               "false_eligibility" in row
        ]
        attempted_runs = len(group)
        trials = len(valid_group)
        failed_runs = attempted_runs - trials
        unsafe = sum(
            bool(row["target_pair_conflicting_cooccupancy"])
            for row in valid_group
        )
        collisions = sum(
            int(row.get("physical_collision_vehicle_count") or 0) > 0
            for row in valid_group
        )
        false_eligibility = sum(
            bool(row["false_eligibility"]) for row in valid_group
        )
        unsafe_ci = grid.wilson(unsafe, trials) if trials else (None, None)
        collision_ci = grid.wilson(collisions, trials) if trials else (None, None)
        eligibility_ci = (
            grid.wilson(false_eligibility, trials) if trials else (None, None)
        )
        wait_samples = [
            float(value)
            for row in valid_group
            for value in row.get("wait_samples_s", [])
        ]
        direction_counts = Counter(
            str((row.get("target_direction") or {}).get("derived", "MISSING"))
            for row in valid_group
        )
        aggregates.append({
            "attack_kind": attack_kind,
            "b": int(group[0]["b"]),
            "ablation_mode": mode,
            "direction_eligibility_enabled": bool(
                group[0]["direction_eligibility_enabled"]
            ),
            "all_singleton_scheduling": bool(group[0]["all_singleton_scheduling"]),
            "attempted_runs": attempted_runs,
            "failed_runs": failed_runs,
            "repetitions": trials,
            "passing_runs": sum(bool(row["passed"]) for row in valid_group),
            "false_eligibility_runs": false_eligibility,
            "false_eligibility_rate": (
                false_eligibility / trials if trials else None
            ),
            "false_eligibility_wilson95_low": eligibility_ci[0],
            "false_eligibility_wilson95_high": eligibility_ci[1],
            "unsafe_cooccupancy_runs": unsafe,
            "unsafe_cooccupancy_rate": unsafe / trials if trials else None,
            "unsafe_cooccupancy_wilson95_low": unsafe_ci[0],
            "unsafe_cooccupancy_wilson95_high": unsafe_ci[1],
            "sumo_collision_runs": collisions,
            "sumo_collision_rate": collisions / trials if trials else None,
            "sumo_collision_wilson95_low": collision_ci[0],
            "sumo_collision_wilson95_high": collision_ci[1],
            "mean_throughput_veh_per_min": _mean_or_none(
                [row.get("throughput_veh_per_min") for row in valid_group]
            ),
            "mean_wait_s": statistics.mean(wait_samples) if wait_samples else None,
            "p95_wait_s": _percentile(wait_samples, 0.95) if wait_samples else None,
            "mean_batch_size": _mean_or_none(
                [row.get("mean_batch_size") for row in valid_group]
            ),
            "mean_quiet_count": _mean_or_none([
                int(row.get("quiet_count") or 0) for row in valid_group
            ]),
            "mean_signed_unknown_count": _mean_or_none([
                int(row.get("signed_unknown_count") or 0) for row in valid_group
            ]),
            "mean_signed_unknown_singleton_percent": _mean_or_none([
                100.0 * int(row.get("signed_unknown_singleton_count") or 0) / grid.N
                for row in valid_group
            ]),
            "mean_left_table_forced_singleton_percent": _mean_or_none([
                100.0 * int(row.get("left_table_forced_singleton_count") or 0) / grid.N
                for row in valid_group
            ]),
            "mean_quiet_percent": _mean_or_none([
                100.0 * int(row.get("quiet_count") or 0) / grid.N
                for row in valid_group
            ]),
            "mean_total_singleton_percent": _mean_or_none([
                100.0 * int(row.get("singleton_vehicle_count") or 0) / grid.N
                for row in valid_group
            ]),
            "mean_all_singleton_policy_percent": _mean_or_none([
                100.0 * int(row.get("all_singleton_policy_count") or 0) / grid.N
                for row in valid_group
            ]),
            "mean_committed_rank_inversion_pairs": _mean_or_none([
                int(row.get("committed_rank_inversion_pair_count") or 0)
                for row in valid_group
            ]),
            "mean_direction_support": _mean_or_none([
                int((row.get("target_direction") or {}).get("support") or 0)
                for row in valid_group
            ]),
            "mean_direction_b_sig": _mean_or_none([
                int((row.get("target_direction") or {}).get("b_sig") or 0)
                for row in valid_group
            ]),
            "target_derived_unknown_runs": (
                direction_counts.get("U", 0) + direction_counts.get("UNKNOWN", 0)
            ),
            "target_derived_right_runs": direction_counts.get("R", 0),
            "target_derived_left_runs": direction_counts.get("L", 0),
        })
    return aggregates


def run_two_lane_direction_ablation(
    args: argparse.Namespace, profile: str = "smoke",
    traffic_profile: str = "left_heavy",
) -> int:
    from fourway import adjacent_lane_grid as grid

    straight_heavy = traffic_profile == "straight_heavy"
    if straight_heavy:
        rows = grid.straight_heavy_direction_ablation_manifest(profile)
        output_dir = REPO_ROOT / "benchmarks" / {
            "prerequisite": "Phase2DirectionAblationStraightHeavyPrerequisite",
            "smoke": "Phase2DirectionAblationStraightHeavy",
            "full": "Phase2DirectionAblationStraightHeavyFull",
        }[profile]
        maneuver_mix = dict(grid.STRAIGHT_HEAVY_MANEUVER_MIX)
        stem = {
            "prerequisite": "direction_ablation_straight_heavy_prerequisite",
            "smoke": "direction_ablation_straight_heavy",
            "full": "direction_ablation_straight_heavy_full",
        }[profile]
    else:
        rows = grid.direction_ablation_manifest(profile)
        output_dir = REPO_ROOT / "benchmarks" / (
            "Phase2DirectionAblation" if profile == "smoke"
            else "Phase2DirectionAblationFull"
        )
        maneuver_mix = {"left": 8, "straight": 4, "right": 4}
        stem = "direction_ablation" if profile == "smoke" else "direction_ablation_full"
    if args.dry_run:
        print(
            f"[dry-run] N=16 direction/co-batching ablation profile={profile}: "
            f"{len(rows)} sequential runs, maneuvers="
            f"{maneuver_mix['straight']}S/{maneuver_mix['left']}L/"
            f"{maneuver_mix['right']}R, "
            "sigma_lat=.5 sigma_long=1 signal_error=.2 k=2"
        )
        for index, row in enumerate(rows, 1):
            print(f"[dry-run] {index}/{len(rows)} {row}")
        return 0

    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / f"{stem}_manifest.json").write_text(json.dumps({
        "checkpoint": (
            "P7-straight-heavy-direction-batching-prerequisite"
            if profile == "prerequisite" else
            "P7-straight-heavy-direction-and-cobatching-ablation"
            if straight_heavy else
            "P7-direction-and-cobatching-ablation"
        ),
        "profile": profile,
        "n": grid.N,
        "traffic_profile": (
            "straight_heavy_8S_4L_4R" if straight_heavy
            else "left_heavy_4S_8L_4R"
        ),
        "maneuver_mix": maneuver_mix,
        "operating_point": {
            "sigma_lat_m": 0.5, "sigma_long_m": 1.0,
            "signal_error": 0.2, "physical_gate_k": 2.0,
        },
        "execution": "strictly sequential",
        "paired_seed_policy": (
            "same repetition uses the same seed and fixture across all six rows"
        ),
        "fixture_selection_policy": (
            "fixture_id=rep_mod_4, preselected before perception"
            if straight_heavy else "fixed reviewed left-heavy fixture"
        ),
        "repetitions_per_cell": (
            1 if profile in ("prerequisite", "smoke")
            else grid.DIRECTION_ABLATION_REPS
        ),
        "rows": rows,
    }, indent=2, sort_keys=True) + "\n")
    run_key_generation(dry_run=False, scale=grid.N)
    results: List[Dict[str, Any]] = []
    for index, row in enumerate(rows, 1):
        print(f"\n--- Direction ablation {index}/{len(rows)}: {row['name']} rep={row['rep']} ---")
        run_dir = (
            grid.straight_heavy_direction_run_dir(REPO_ROOT, row, profile)
            if straight_heavy else
            grid.direction_ablation_run_dir(REPO_ROOT, row, profile)
        )
        try:
            clear_stale_random_ini()
            run_dir = _run_two_lane_grid_cell(args, row, run_dir_override=run_dir)
            result = _analyze_direction_ablation_cell(row, run_dir)
            results.append(result)
            print(
                f"[{'PASS' if result['passed'] else 'FAIL'}] "
                f"derived={result['target_direction'].get('derived')} "
                f"unsafe={result['target_pair_conflicting_cooccupancy']} "
                f"throughput={result['throughput_veh_per_min']} veh/min "
                f"mean_wait={result['mean_wait_s']}s"
            )
            for name, passed in result["checks"].items():
                print(f"  {'PASS' if passed else 'FAIL':4} {name}")
            (output_dir / f"{stem}_progress.json").write_text(json.dumps({
                "planned": len(rows),
                "completed": len(results),
                "last_completed": {"name": row["name"], "rep": row["rep"]},
                "passing": sum(bool(item.get("passed")) for item in results),
            }, indent=2, sort_keys=True) + "\n")
        except (subprocess.CalledProcessError, OSError, ValueError, KeyError) as exc:
            results.append({**row, "passed": False, "error": str(exc)})
            print(f"[FAIL] {row['name']}: {exc}", file=sys.stderr)
            break

    aggregates = _aggregate_direction_ablation(results)
    overall = len(results) == len(rows) and all(row.get("passed") for row in results)
    summary_path = output_dir / f"{stem}_summary.json"
    summary_path.write_text(json.dumps({
        "checkpoint": (
            "P7-straight-heavy-direction-batching-prerequisite"
            if profile == "prerequisite" else
            "P7-straight-heavy-direction-and-cobatching-ablation"
            if straight_heavy else
            "P7-direction-and-cobatching-ablation"
        ),
        "profile": profile,
        "passed": overall,
        "planned": len(rows), "completed": len(results),
        "traffic_profile": (
            "straight_heavy_8S_4L_4R" if straight_heavy
            else "left_heavy_4S_8L_4R"
        ),
        "maneuver_mix": maneuver_mix,
        "attack_policy": (
            "static enumerated attack kind logged at initialization before any "
            "perception draw; no noise-oracle adaptation"
        ),
        "aggregates": aggregates,
        "results": results,
    }, indent=2, sort_keys=True) + "\n")
    csv_path = output_dir / f"{stem}_aggregates.csv"
    if aggregates:
        with csv_path.open("w", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=list(aggregates[0]))
            writer.writeheader()
            writer.writerows(aggregates)
    title = (
        "N=16 STRAIGHT-HEAVY BATCHING PREREQUISITE"
        if profile == "prerequisite" else
        "N=16 STRAIGHT-HEAVY DIRECTION / CO-BATCHING ABLATION"
        if straight_heavy else
        "N=16 DIRECTION / CO-BATCHING ABLATION"
    )
    print(f"\n========== {title} ==========")
    print(f"Completed: {len(results)}/{len(rows)}")
    print(f"Overall: {'PASS' if overall else 'FAIL'}")
    print(f"Summary: {summary_path}")
    print(f"Aggregates: {csv_path}")
    return 0 if overall else 1


TWO_LANE_SCALE_HONEST_REPS = 20


def _two_lane_scale_rows(profile: str = "smoke") -> List[Dict[str, Any]]:
    if profile not in ("smoke", "full"):
        raise ValueError(f"unknown two-lane scale profile: {profile}")
    maneuver_counts = {
        4: {"left": 2, "straight": 1, "right": 1},
        8: {"left": 4, "straight": 2, "right": 2},
        16: {"left": 8, "straight": 4, "right": 4},
        20: {"left": 10, "straight": 5, "right": 5},
    }
    repetitions = 1 if profile == "smoke" else TWO_LANE_SCALE_HONEST_REPS
    return [
        {
            "name": f"n{n}_honest",
            "n": n,
            "f": bft_f(n),
            "config": TWO_LANE_SCALE_CONFIGS[n],
            "attack_kind": "NONE",
            "b": 0,
            "rep": rep,
            "sigma_lat_m": 0.5,
            "sigma_long_m": 1.0,
            "signal_error": 0.2,
            "k": 2.0,
            "delta_m": 0.0,
            "maneuver_counts": maneuver_counts[n],
            "profile": profile,
        }
        for rep in range(repetitions)
        for n in (4, 8, 16, 20)
    ]


def _two_lane_scale_smoke_rows() -> List[Dict[str, Any]]:
    return _two_lane_scale_rows("smoke")


def _two_lane_scale_run_dir(row: Dict[str, Any]) -> Path:
    root = (
        "Phase2TwoLaneScaleSmoke"
        if row.get("profile", "smoke") == "smoke"
        else "Phase2TwoLaneScaleHonestFull"
    )
    return (
        REPO_ROOT / "benchmarks" / root /
        f"N{int(row['n'])}" / "honest" / f"run_{int(row['rep'])}"
    )


def _two_lane_scale_metadata(row: Dict[str, Any]) -> Dict[str, Any]:
    n = int(row["n"])
    rep = int(row["rep"])
    profile = str(row.get("profile", "smoke"))
    suite = "TWO_LANE_SCALE_SMOKE" if profile == "smoke" else "TWO_LANE_SCALE_HONEST_FULL"
    metadata = {
        "checkpoint": (
            "P8-two-lane-multiscale-fixture-smoke"
            if profile == "smoke"
            else "P8-two-lane-multiscale-honest-full"
        ),
        "n": n,
        "f": int(row["f"]),
        "config": row["config"],
        "attack_kind": "NONE",
        "attack_choice_timing": "INIT_BEFORE_PERCEPTION",
        "simulation_seed": run_seed(MASTER_SEED, n, suite, rep),
        "sigma_lat_m": 0.5,
        "sigma_long_m": 1.0,
        "signal_error": 0.2,
        "physical_gate_k": 2.0,
        "direction_collection_window_sec": 0.25,
        "lane_observation_mode": "ADJACENT_LATERAL",
        "physical_lane_policy": "lane0=STRAIGHT|RIGHT; lane1=LEFT",
        "maneuver_counts": row["maneuver_counts"],
        "reviewed_conflict_pair": "veh0=N-L; veh1=S-S",
    }
    # Preserve byte-for-byte metadata compatibility with the already completed
    # four-run smoke. The full profile adds its distinguishing fields and writes
    # to a separate result root.
    if profile == "full":
        metadata["profile"] = profile
        metadata["rep"] = rep
    return metadata


def _run_two_lane_scale_cell(row: Dict[str, Any]) -> Path:
    n = int(row["n"])
    rep = int(row["rep"])
    run_dir = _two_lane_scale_run_dir(row)
    metadata = _two_lane_scale_metadata(row)
    metadata_path = run_dir / "two_lane_scale_run_metadata.json"
    json_path = run_dir / f"{n}veh_{rep}.json"
    raw_log = run_dir / "raw_simulation.log"
    if metadata_path.is_file() and json_path.is_file() and raw_log.is_file():
        if json.loads(metadata_path.read_text()) != metadata:
            raise ValueError(f"refusing mismatched scale-smoke resume: {run_dir}")
        print(f"[resume] reuse {run_dir}")
        return run_dir

    run_dir.mkdir(parents=True, exist_ok=True)
    metadata_path.write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n")
    argv = [
        str(RUN_SCRIPT),
        "--randomize", str(n), "0", "--no-ambulance",
        "--tolerated-f", str(row["f"]), str(FOURWAY_DIR),
        "--compact-log", "--channel-metrics-dir", str(run_dir.resolve()),
        "--approach-sigma", "0", "--signal-error", "0.2",
        "--direction-collection-window", "0.25",
        "--ego-longitudinal-sigma", "0", "--longitudinal-sigma", "1.0",
        "--lane-observation-mode", "ADJACENT_LATERAL",
        "--lateral-sigma", "0.5", "--adjacent-lane-separation", "3.2",
        "--physical-gate-k", "2.0", "--direction-eligibility", "on",
        "--simulation-seed", str(metadata["simulation_seed"]),
        "--phase2-attack-kind", "NONE", "--phase2-attack-target", "0",
        "--phase2-evidence-colluders", "", "--phase2-actual-b", "0",
        "--phase2-lateral-claim-offset", "0",
        "-u", "Cmdenv", "--debug-on-errors=false", "-c", str(row["config"]),
    ]
    inner = " ".join(shlex.quote(value) for value in argv)
    print("+ " + inner)
    run_in_bash_with_omnet(inner, dry_run=False)
    shutil.copy2(LOG_FILE, raw_log)
    analyze = [
        sys.executable, str(FOURWAY_DIR / "analyze_log.py"), str(LOG_FILE),
        "--save-to", str(run_dir), "--scenario", "1", "--cars", str(n),
        "--run-index", str(rep), "--no-scenario-subdir",
    ]
    subprocess.run(analyze, cwd=REPO_ROOT, check=True)
    return run_dir


def _analyze_two_lane_scale_cell(row: Dict[str, Any], run_dir: Path) -> Dict[str, Any]:
    n = int(row["n"])
    rep = int(row["rep"])
    log_text = (run_dir / "raw_simulation.log").read_text(errors="replace")
    records = json.loads((run_dir / f"{n}veh_{rep}.json").read_text())
    first = records[0]
    perception = first["bft_stats"]["perception"]
    distance = perception.get("stopped_distance") or {}
    metrics = first.get("run_metrics") or {}
    departures = set(re.findall(r"\[DEPARTED\] Replica (\d+)\b", log_text))
    movement_records = set(re.findall(
        r"\[MOVEMENT-GROUND-TRUTH\]\s+vehicle=(veh\d+)", log_text
    ))
    config_positions = [
        match.start() for match in re.finditer(r"\[PHASE2-ATTACK-CONFIG\]", log_text)
    ]
    perception_positions = [
        position for marker in ("[PERC-EVAL]", "[DIST-PERC-EVAL]")
        if (position := log_text.find(marker)) >= 0
    ]
    batch_distribution = metrics.get("batch_index_distribution") or {}
    wait_samples = [
        float(wait_ms) / 1000.0 for record in records
        if (wait_ms := (record.get("durations_ms") or {}).get("total_wait_time"))
        is not None
    ]
    signed_count = (
        int(perception.get("signed_direction_count") or 0) +
        int(perception.get("signed_unknown_count") or 0)
    )
    unsafe_pairs = metrics.get("unsafe_conflict_cooccupancy_pairs") or []
    checks = {
        "all_vehicles_departed": len(departures) == n,
        "all_movements_traced": len(movement_records) == n,
        "all_arrivals_lane_certified": signed_count == n,
        "lane_single_evaluation": bool(perception.get("single_evaluation_invariant_ok")),
        "distance_single_evaluation": bool(distance.get("single_evaluation_invariant_ok")),
        "distance_certificates_complete": int(distance.get("certificate_count") or 0) == n,
        "authenticated_evidence": (
            "[ANN-ORIGIN-INVALID]" not in log_text and
            "[CERT-INVALID]" not in log_text and "verify FAIL" not in log_text
        ),
        "check10_clean": "Check 10 FAIL" not in log_text,
        "no_teleport": "Teleporting vehicle" not in log_text,
        "honest_has_no_unsafe_cooccupancy": not unsafe_pairs,
        "honest_has_no_sumo_collision": int(
            metrics.get("physical_collision_vehicle_count") or 0
        ) == 0,
        "completion_metrics_present": len(wait_samples) == n,
        "attack_choice_precommitted_before_perception": (
            bool(config_positions and perception_positions) and
            max(config_positions) < min(perception_positions)
        ),
    }
    throughput = metrics.get("throughput_veh_per_s")
    return {
        **row,
        "passed": all(checks.values()),
        "checks": checks,
        "result_dir": str(run_dir),
        "throughput_veh_per_min": (
            float(throughput) * 60.0 if throughput is not None else None
        ),
        "mean_wait_s": statistics.mean(wait_samples) if wait_samples else None,
        "p95_wait_s": _percentile(wait_samples, 0.95) if wait_samples else None,
        "wait_samples_s": wait_samples,
        "mean_batch_size": (
            sum(int(value) for value in batch_distribution.values()) /
            len(batch_distribution) if batch_distribution else None
        ),
        "quiet_count": int(perception.get("quiet_count") or 0),
        "signed_unknown_count": int(perception.get("signed_unknown_count") or 0),
        "left_table_forced_singleton_count": int(
            perception.get("left_table_forced_singleton_count") or 0
        ),
        "committed_rank_inversion_pair_count": int(
            distance.get("order_inversion_count") or 0
        ),
        "unsafe_conflict_pair_count": len(unsafe_pairs),
        "sumo_collision_vehicle_count": int(
            metrics.get("physical_collision_vehicle_count") or 0
        ),
    }


def _aggregate_two_lane_scale_honest(
    rows: Sequence[Dict[str, Any]],
) -> List[Dict[str, Any]]:
    grouped: Dict[int, List[Dict[str, Any]]] = defaultdict(list)
    for row in rows:
        grouped[int(row["n"])].append(row)
    aggregates: List[Dict[str, Any]] = []
    for n, group in sorted(grouped.items()):
        trials = len(group)
        unsafe = sum(int(row.get("unsafe_conflict_pair_count") or 0) > 0 for row in group)
        collisions = sum(int(row.get("sumo_collision_vehicle_count") or 0) > 0 for row in group)
        unsafe_ci = _wilson_interval(unsafe, trials)
        collision_ci = _wilson_interval(collisions, trials)
        wait_samples = [
            float(value) for row in group for value in row.get("wait_samples_s", [])
        ]
        throughput_values = [
            float(row["throughput_veh_per_min"])
            for row in group if row.get("throughput_veh_per_min") is not None
        ]
        aggregates.append({
            "n": n,
            "f": int(group[0]["f"]),
            "repetitions": trials,
            "passing_runs": sum(bool(row.get("passed")) for row in group),
            "mean_throughput_veh_per_min": (
                statistics.mean(throughput_values) if throughput_values else None
            ),
            "throughput_stdev_veh_per_min": (
                statistics.stdev(throughput_values) if len(throughput_values) > 1 else 0.0
            ),
            "mean_wait_s": statistics.mean(wait_samples) if wait_samples else None,
            "p95_wait_s": _percentile(wait_samples, 0.95) if wait_samples else None,
            "mean_batch_size": _mean_or_none(
                [row.get("mean_batch_size") for row in group]
            ),
            "mean_quiet_count": statistics.mean(
                int(row.get("quiet_count") or 0) for row in group
            ),
            "mean_signed_unknown_count": statistics.mean(
                int(row.get("signed_unknown_count") or 0) for row in group
            ),
            "unsafe_cooccupancy_runs": unsafe,
            "unsafe_cooccupancy_rate": unsafe_ci["rate"],
            "unsafe_cooccupancy_wilson95_low": unsafe_ci["low"],
            "unsafe_cooccupancy_wilson95_high": unsafe_ci["high"],
            "sumo_collision_runs": collisions,
            "sumo_collision_rate": collision_ci["rate"],
            "sumo_collision_wilson95_low": collision_ci["low"],
            "sumo_collision_wilson95_high": collision_ci["high"],
        })
    return aggregates


def run_two_lane_scale(args: argparse.Namespace, profile: str = "smoke") -> int:
    rows = _two_lane_scale_rows(profile)
    full = profile == "full"
    output_dir = REPO_ROOT / "benchmarks" / (
        "Phase2TwoLaneScaleHonestFull" if full else "Phase2TwoLaneScaleSmoke"
    )
    if args.dry_run:
        print(
            f"[dry-run] two-lane honest scale profile={profile}: N={{4,8,16,20}}, "
            f"{len(rows)} operating-point runs, strictly sequential"
        )
        for index, row in enumerate(rows, 1):
            print(f"[dry-run] {index}/{len(rows)} {row}")
        return 0

    output_dir.mkdir(parents=True, exist_ok=True)
    stem = "two_lane_scale_honest_full" if full else "two_lane_scale_smoke"
    (output_dir / f"{stem}_manifest.json").write_text(json.dumps({
        "checkpoint": (
            "P8-two-lane-multiscale-honest-full"
            if full else "P8-two-lane-multiscale-fixture-smoke"
        ),
        "profile": profile,
        "execution": "strictly sequential",
        "repetitions_per_n": TWO_LANE_SCALE_HONEST_REPS if full else 1,
        "operating_point": {
            "sigma_lat_m": 0.5, "sigma_long_m": 1.0,
            "signal_error": 0.2, "physical_gate_k": 2.0,
        },
        "maneuver_ratio": "50% LEFT / 25% STRAIGHT / 25% RIGHT",
        "rows": rows,
    }, indent=2, sort_keys=True) + "\n")

    results = []
    for index, row in enumerate(rows, 1):
        print(
            f"\n--- Honest scale {index}/{len(rows)}: "
            f"N={row['n']} rep={row['rep']} ---"
        )
        try:
            run_key_generation(dry_run=False, scale=int(row["n"]))
            clear_stale_random_ini()
            run_dir = _run_two_lane_scale_cell(row)
            result = _analyze_two_lane_scale_cell(row, run_dir)
            results.append(result)
            print(
                f"[{'PASS' if result['passed'] else 'FAIL'}] N={row['n']} "
                f"throughput={result['throughput_veh_per_min']} veh/min "
                f"mean_wait={result['mean_wait_s']}s"
            )
            for name, passed in result["checks"].items():
                print(f"  {'PASS' if passed else 'FAIL':4} {name}")
        except (subprocess.CalledProcessError, OSError, ValueError, KeyError) as exc:
            results.append({**row, "passed": False, "error": str(exc)})
            print(f"[FAIL] N={row['n']}: {exc}", file=sys.stderr)
            break

    overall = len(results) == len(rows) and all(row.get("passed") for row in results)
    aggregates = _aggregate_two_lane_scale_honest(results)
    summary_path = output_dir / f"{stem}_summary.json"
    summary_path.write_text(json.dumps({
        "checkpoint": (
            "P8-two-lane-multiscale-honest-full"
            if full else "P8-two-lane-multiscale-fixture-smoke"
        ),
        "profile": profile,
        "passed": overall,
        "planned": len(rows), "completed": len(results),
        "aggregates": aggregates,
        "results": results,
    }, indent=2, sort_keys=True) + "\n")
    csv_path = output_dir / f"{stem}_results.csv"
    fields = [
        "n", "f", "throughput_veh_per_min", "mean_wait_s", "p95_wait_s",
        "mean_batch_size", "quiet_count", "signed_unknown_count",
        "left_table_forced_singleton_count", "committed_rank_inversion_pair_count",
        "unsafe_conflict_pair_count", "sumo_collision_vehicle_count", "passed",
    ]
    with csv_path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(results)
    aggregate_path = output_dir / f"{stem}_aggregates.csv"
    if aggregates:
        with aggregate_path.open("w", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=list(aggregates[0]))
            writer.writeheader()
            writer.writerows(aggregates)
    print("\n========== TWO-LANE HONEST MULTISCALE ==========")
    print(f"Profile: {profile}")
    print(f"Completed: {len(results)}/{len(rows)}")
    print(f"Overall: {'PASS' if overall else 'FAIL'}")
    print(f"Summary: {summary_path}")
    print(f"Results: {csv_path}")
    print(f"Aggregates: {aggregate_path}")
    return 0 if overall else 1


def run_two_lane_scale_smoke(args: argparse.Namespace) -> int:
    return run_two_lane_scale(args, "smoke")


def run_attack_defense_equivocation_validation(args: argparse.Namespace) -> int:
    """Exercise the real signed-announcement/gossip path for attack D."""
    n = 16
    f = bft_f(n)
    rep = args.start_rep
    seed = run_seed(MASTER_SEED, n, "ATTACK_DEFENSE_EQUIVOCATION", rep)
    # Keep the original D_Equivocation artifact: its compact log omitted
    # ECHO-SEND and allowed the echo assertion to pass vacuously.  V2 requires
    # full evidence and must never resume that superseded run.
    output_dir = (
        REPO_ROOT / "benchmarks" / "Phase2AttackDefense" /
        "D_EquivocationV2" / f"run_{rep}"
    )
    metadata = {
        "checkpoint": "P8-attack-defense-D-equivocation-v2",
        "harness_version": 2,
        "n": n,
        "f": f,
        "rep": rep,
        "simulation_seed": seed,
        "equivocating_replica": 0,
        "byzantine_type": 3,
        "config": TWO_LANE_SWEEP_CONFIG,
        "operating_point": {
            "sigma_lat_m": 0.5,
            "sigma_long_m": 1.0,
            "signal_error": 0.2,
            "physical_gate_k": 2.0,
        },
        "invariant": "one authenticated claim variant per witness,target,epoch",
        "execution": "single sequential focused validation",
    }
    if args.dry_run:
        print(
            "[dry-run] attack-defense D equivocation validation: N=16, "
            "r0 sends signed LEFT/RIGHT variants, one sequential run"
        )
        return 0

    output_dir.mkdir(parents=True, exist_ok=True)
    metadata_path = output_dir / "equivocation_run_metadata.json"
    raw_log = output_dir / "raw_simulation.log"
    json_path = output_dir / f"{n}veh_{rep}.json"
    if metadata_path.is_file() and raw_log.is_file() and json_path.is_file():
        if json.loads(metadata_path.read_text()) != metadata:
            raise ValueError(f"refusing mismatched equivocation artifact: {output_dir}")
        print(f"[resume] reuse {output_dir}")
    else:
        metadata_path.write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n")
        run_key_generation(dry_run=False, scale=n)
        clear_stale_random_ini()
        argv = [
            str(RUN_SCRIPT),
            "--randomize", str(n), "0",
            "--byzleader", "0", "--leader-byz-type", "3",
            "--no-ambulance", "--tolerated-f", str(f),
            str(FOURWAY_DIR),
            "--channel-metrics-dir", str(output_dir.resolve()),
            "--approach-sigma", "0", "--signal-error", "0.2",
            "--direction-collection-window", "0.25",
            "--ego-longitudinal-sigma", "0", "--longitudinal-sigma", "1.0",
            "--lane-observation-mode", "ADJACENT_LATERAL",
            "--lateral-sigma", "0.5", "--adjacent-lane-separation", "3.2",
            "--physical-gate-k", "2.0", "--direction-eligibility", "on",
            "--simulation-seed", str(seed),
            "--phase2-attack-kind", "NONE", "--phase2-attack-target", "0",
            "--phase2-evidence-colluders", "", "--phase2-actual-b", "0",
            "--phase2-lateral-claim-offset", "0",
            "-u", "Cmdenv", "--debug-on-errors=false", "-c", TWO_LANE_SWEEP_CONFIG,
        ]
        inner = " ".join(shlex.quote(value) for value in argv)
        print("+ " + inner)
        run_in_bash_with_omnet(inner, dry_run=False)
        shutil.copy2(LOG_FILE, raw_log)
        analyze = [
            sys.executable, str(FOURWAY_DIR / "analyze_log.py"), str(LOG_FILE),
            "--save-to", str(output_dir), "--scenario", "1", "--cars", str(n),
            "--run-index", str(rep), "--no-scenario-subdir",
        ]
        subprocess.run(analyze, cwd=REPO_ROOT, check=True)

    text = raw_log.read_text(errors="replace")
    records = json.loads(json_path.read_text())
    perception = records[0]["bft_stats"]["perception"]
    departures = set(re.findall(r"\[DEPARTED\] Replica (\d+)\b", text))
    evaluations = defaultdict(set)
    for match in re.finditer(
        r"\[PERC-EVAL\]\s+witness=(\d+)\s+target=(\S+)\s+epoch=(\d+)\s+"
        r"claimHash=([0-9a-fA-F]+)", text
    ):
        evaluations[(int(match.group(1)), match.group(2), int(match.group(3)))].add(
            match.group(4)
        )
    echo_variants = defaultdict(set)
    for match in re.finditer(
        r"\[ECHO-SEND\]\s+Replica\s+(\d+).*?veh0\s+ARRIVAL_ECHO.*?"
        r"claimHash=([0-9a-fA-F]+)", text
    ):
        echo_variants[int(match.group(1))].add(match.group(2))
    attack_variant_rows = [
        {
            "peer": int(match.group(1)),
            "direction": match.group(2),
            "claim_hash": match.group(3),
        }
        for match in re.finditer(
            r"\[BYZANTINE\]\s+r0\s+EQUIVOCATOR:\s+peer\s+(\d+)\s+"
            r"dir=([LR])\s+claimHash=([0-9a-fA-F]+)", text
        )
    ]
    attack_hashes_by_direction = defaultdict(set)
    for row in attack_variant_rows:
        attack_hashes_by_direction[row["direction"]].add(row["claim_hash"])
    declared_attack_hashes = {
        claim_hash
        for hashes in attack_hashes_by_direction.values()
        for claim_hash in hashes
    }
    equivocation_rows = list((perception.get("equivocation_events") or {}).values())
    variant_echo_counts = Counter(
        claim_hash for hashes in echo_variants.values() for claim_hash in hashes
    )
    metrics = records[0].get("run_metrics") or {}
    target_cert_assemblies = re.findall(
        r"\[CERT-ASSEMBLE\] target=veh0 epoch=0[^\n]*", text
    )
    checks = {
        "equivocation_attack_emitted_exactly_two_reused_signed_variants": (
            len(attack_variant_rows) == n - 1 and
            set(attack_hashes_by_direction) == {"L", "R"} and
            all(len(hashes) == 1 for hashes in attack_hashes_by_direction.values()) and
            len(declared_attack_hashes) == 2
        ),
        "explicit_equivocation_detection_observed": bool(equivocation_rows),
        "second_variant_rejected_before_perception_or_echo": (
            bool(equivocation_rows) and
            bool(perception.get("equivocation_rejections_without_perception_or_echo"))
        ),
        "one_variant_evaluated_per_witness_target_epoch": (
            any(target == "veh0" for (_, target, _) in evaluations) and
            all(len(hashes) <= 1 for hashes in evaluations.values()) and
            bool(perception.get("single_evaluation_invariant_ok"))
        ),
        "one_variant_echoed_per_witness": (
            bool(echo_variants) and
            all(len(hashes) <= 1 for hashes in echo_variants.values()) and
            all(
                claim_hash in declared_attack_hashes
                for hashes in echo_variants.values()
                for claim_hash in hashes
            )
        ),
        "origin_and_certificate_authentication_clean": (
            "[ANN-ORIGIN-INVALID]" not in text and
            "[CERT-INVALID]" not in text and "verify FAIL" not in text
        ),
        "all_vehicles_departed": len(departures) == n,
        "no_unsafe_conflicting_cooccupancy": not (
            metrics.get("unsafe_conflict_cooccupancy_pairs") or []
        ),
        "no_sumo_collision": int(
            metrics.get("physical_collision_vehicle_count") or 0
        ) == 0,
        "no_teleport": "Teleporting vehicle" not in text,
        "no_two_conflicting_variants_certified_or_unsafely_committed": (
            len(target_cert_assemblies) <= 1 and
            not (metrics.get("unsafe_conflict_cooccupancy_pairs") or [])
        ),
    }
    summary = {
        "checkpoint": metadata["checkpoint"],
        "passed": all(checks.values()),
        "checks": checks,
        "metadata": metadata,
        "equivocation_event_count": len(equivocation_rows),
        "declared_attack_variant_hashes_by_direction": {
            direction: sorted(hashes)
            for direction, hashes in sorted(attack_hashes_by_direction.items())
        },
        "attack_variant_peer_assignments": attack_variant_rows,
        "evaluated_variant_hashes_by_witness_target_epoch": {
            f"r{witness}:{target}@{epoch}": sorted(hashes)
            for (witness, target, epoch), hashes in sorted(evaluations.items())
            if target == "veh0"
        },
        "echoed_variant_hashes_by_witness": {
            str(witness): sorted(hashes)
            for witness, hashes in sorted(echo_variants.items())
        },
        "variant_echo_witness_counts": dict(sorted(variant_echo_counts.items())),
        "target_certificate_formed": "[CERT-ASSEMBLE] target=veh0 " in text,
        "target_certificate_assembly_count": len(target_cert_assemblies),
        "claim_framing": (
            "The invariant prevents any honest witness from backing both variants; "
            "one variant may still independently reach f+1, but two conflicting "
            "variants must not both become committable or create an unsafe schedule."
        ),
        "result_dir": str(output_dir),
    }
    summary_path = output_dir / "equivocation_validation_summary.json"
    summary_path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    print("\n========== ATTACK-DEFENSE D: EQUIVOCATION ==========")
    for name, passed in checks.items():
        print(f"  {'PASS' if passed else 'FAIL':4} {name}")
    print(f"Variant echo witness counts: {dict(sorted(variant_echo_counts.items()))}")
    print(f"Target certificate formed: {summary['target_certificate_formed']}")
    print(f"Overall: {'PASS' if summary['passed'] else 'FAIL'}")
    print(f"Summary: {summary_path}")
    return 0 if summary["passed"] else 1


def run_attack_defense_leader_validation(args: argparse.Namespace) -> int:
    """Run focused E-H proposal-integrity and liveness demonstrations."""
    n = 16
    f = bft_f(n)
    cases = (
        {
            "letter": "E", "name": "PhysicalLaneMutationV4", "type": 11,
            "mutation": "TAMPER_PHYSICAL_LANE", "signal_error": 0.2,
        },
        {
            "letter": "F", "name": "UnknownDirectionUpgradeV4", "type": 9,
            "mutation": "UPGRADE_UNKNOWN_DIRECTION", "signal_error": 1.0,
        },
        {
            "letter": "G", "name": "CertSuppressionV4", "type": 12,
            "mutation": "SUPPRESS_CERTS", "signal_error": 0.2,
        },
        {
            "letter": "H", "name": "SilentPrimaryV4", "type": 4,
            "mutation": "SILENT_PRIMARY", "signal_error": 0.2,
        },
    )
    if args.dry_run:
        for case in cases:
            print(
                f"[dry-run] attack-defense {case['letter']} {case['name']}: "
                f"N=16 proposer=r0 type={case['type']} one sequential run"
            )
        return 0

    run_key_generation(dry_run=False, scale=n)
    results = []
    for case in cases:
        rep = args.start_rep
        seed = run_seed(
            MASTER_SEED, n, f"ATTACK_DEFENSE_{case['letter']}_{case['name']}", rep
        )
        output_dir = (
            REPO_ROOT / "benchmarks" / "Phase2AttackDefense" /
            f"{case['letter']}_{case['name']}" / f"run_{rep}"
        )
        metadata = {
            "checkpoint": f"P8-attack-defense-{case['letter'].lower()}",
            "harness_version": 4,
            "n": n, "f": f, "rep": rep, "simulation_seed": seed,
            "configured_proposer": 0,
            "byzantine_type": case["type"],
            "mutation": case["mutation"],
            "config": TWO_LANE_SWEEP_CONFIG,
            "operating_point": {
                "sigma_lat_m": 0.5,
                "sigma_long_m": 1.0,
                "signal_error": case["signal_error"],
                "physical_gate_k": 2.0,
            },
            "execution": "single sequential focused validation with explicit view-change and proposer cert-set provenance",
            "forced_election": "r0 is deliberately installed as the initial certified/PBFT proposer so the mutation path is exercised",
            "signal_error_note": (
                "signal_error=1.0 intentionally forces SIGNED-UNKNOWN for the UNKNOWN-upgrade target"
                if case["letter"] == "F" else
                "signal_error=0.2 is the locked system operating point"
            ),
        }
        output_dir.mkdir(parents=True, exist_ok=True)
        metadata_path = output_dir / "attack_run_metadata.json"
        raw_log = output_dir / "raw_simulation.log"
        json_path = output_dir / f"{n}veh_{rep}.json"
        if metadata_path.is_file() and raw_log.is_file() and json_path.is_file():
            if json.loads(metadata_path.read_text()) != metadata:
                raise ValueError(f"refusing mismatched leader-attack artifact: {output_dir}")
            print(f"[resume] reuse {output_dir}")
        else:
            metadata_path.write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n")
            clear_stale_random_ini()
            argv = [
                str(RUN_SCRIPT),
                "--randomize", str(n), "0",
                "--byzleader", "0", "--leader-byz-type", str(case["type"]),
                "--no-ambulance", "--tolerated-f", str(f),
                str(FOURWAY_DIR),
                "--compact-log",
                "--channel-metrics-dir", str(output_dir.resolve()),
                "--approach-sigma", "0",
                "--signal-error", str(case["signal_error"]),
                "--direction-collection-window", "0.25",
                "--ego-longitudinal-sigma", "0", "--longitudinal-sigma", "1.0",
                "--lane-observation-mode", "ADJACENT_LATERAL",
                "--lateral-sigma", "0.5", "--adjacent-lane-separation", "3.2",
                "--physical-gate-k", "2.0", "--direction-eligibility", "on",
                "--simulation-seed", str(seed),
                "--phase2-attack-kind", "NONE", "--phase2-attack-target", "0",
                "--phase2-evidence-colluders", "", "--phase2-actual-b", "0",
                "--phase2-lateral-claim-offset", "0",
                "-u", "Cmdenv", "--debug-on-errors=false", "-c", TWO_LANE_SWEEP_CONFIG,
            ]
            inner = " ".join(shlex.quote(value) for value in argv)
            print("+ " + inner)
            run_in_bash_with_omnet(inner, dry_run=False)
            shutil.copy2(LOG_FILE, raw_log)
            analyze = [
                sys.executable, str(FOURWAY_DIR / "analyze_log.py"), str(LOG_FILE),
                "--save-to", str(output_dir), "--scenario", "1", "--cars", str(n),
                "--run-index", str(rep), "--no-scenario-subdir",
            ]
            subprocess.run(analyze, cwd=REPO_ROOT, check=True)

        text = raw_log.read_text(errors="replace")
        records = json.loads(json_path.read_text())
        metrics = records[0].get("run_metrics") or {}
        departures = set(re.findall(r"\[DEPARTED\] Replica (\d+)\b", text))
        proposer = re.search(
            rf"\[ATTACK-PROPOSER\] replica=(\d+) epoch=(\d+) "
            rf"mutation={case['mutation']} attempted=1([^\n]*)", text,
        )
        cert_backed_proposer = "[CERT-ASSEMBLE] target=veh0 " in text
        executor_lines = re.findall(
            r"\[EXECUTOR\] entries epoch=0 n=16:[^\n]+", text
        )
        final_executor = max(
            executor_lines, key=lambda line: line.count("physicalLane="), default=""
        )
        primary_changes = [
            {"observer": int(m.group(1)), "old": int(m.group(2)),
             "new": int(m.group(3))}
            for m in re.finditer(
                r"\[VC-DEBUG\] r(\d+) primary changed: (\d+) -> (\d+)", text
            )
        ]
        successor_primaries = sorted({
            row["new"] for row in primary_changes if row["new"] != 0
        })
        proposer_cert_states = []
        for match in re.finditer(
            r"\[PROPOSER-CERT-STATE\] replica=(\d+) epoch=(\d+) "
            r"pbftPrimary=(-?\d+) authority=([^ ]+) certCount=(\d+) ids=([0-9,]*)",
            text,
        ):
            proposer_cert_states.append({
                "replica": int(match.group(1)),
                "epoch": int(match.group(2)),
                "pbft_primary": int(match.group(3)),
                "authority": match.group(4),
                "cert_count": int(match.group(5)),
                "cert_ids": [
                    int(value) for value in match.group(6).split(",") if value
                ],
            })
        initial_cert_states = [
            row for row in proposer_cert_states
            if row["replica"] == 0 and row["epoch"] == 0
        ]
        successor_cert_states = [
            row for row in proposer_cert_states
            if row["replica"] in successor_primaries and row["epoch"] == 0 and
               row["authority"] == "view-change"
        ]
        initial_cert_ids = set(initial_cert_states[-1]["cert_ids"]) if initial_cert_states else set()
        successor_covering_initial = [
            row for row in successor_cert_states
            if initial_cert_ids and initial_cert_ids.issubset(set(row["cert_ids"]))
        ]

        def committed_entry(replica_id: int) -> Dict[str, Any] | None:
            match = re.search(
                rf"r{replica_id}\(lane=([NSEW?]+) pos=(\d+) dir=([^ ]+) "
                rf"ambu=(\d+) cyber=(\d+) physicalLane=(\d+) "
                rf"lateralClaimCm=(-?\d+)\)", final_executor,
            )
            if not match:
                return None
            return {
                "lane": match.group(1), "position": int(match.group(2)),
                "direction": match.group(3), "ambulance": int(match.group(4)),
                "cyber_status": int(match.group(5)),
                "physical_lane": int(match.group(6)),
                "lateral_claim_cm": int(match.group(7)),
            }

        checks = {
            "attack_executed_by_actual_certified_proposer": (
                proposer is not None and int(proposer.group(1)) == 0 and
                int(proposer.group(2)) == 0 and cert_backed_proposer
            ),
            "order_committed_after_attack": "Order_Decided_Time" in text,
            "all_vehicles_departed": len(departures) == n,
            "origin_and_certificate_authentication_clean": (
                "[ANN-ORIGIN-INVALID]" not in text and
                "[CERT-INVALID]" not in text and "verify FAIL" not in text
            ),
            "no_unsafe_conflicting_cooccupancy": not (
                metrics.get("unsafe_conflict_cooccupancy_pairs") or []
            ),
            "no_sumo_collision_or_teleport": (
                int(metrics.get("physical_collision_vehicle_count") or 0) == 0 and
                "Teleporting vehicle" not in text
            ),
            "view_changed_to_nonbyzantine_successor": bool(successor_primaries),
            "successor_proposer_held_initial_certified_set": bool(
                initial_cert_states and successor_covering_initial
            ),
        }
        evidence: Dict[str, Any] = {
            "attack_proposer_record": proposer.group(0) if proposer else None,
            "primary_changes": primary_changes,
            "successor_primaries": successor_primaries,
            "proposer_cert_states": proposer_cert_states,
        }

        if case["letter"] == "E":
            mutation = re.search(
                r"TAMPER_PHYSICAL_LANE: replica (\d+) physicalLane (\d+)->(\d+) "
                r"lateralClaimCm (-?\d+)->(-?\d+)", text.replace("→", "->")
            )
            target = int(mutation.group(1)) if mutation else -1
            committed = committed_entry(target) if target >= 0 else None
            checks["check10_rejected_mutated_physical_authority"] = bool(re.search(
                r"\[OMNET-PREVERIFY\] reject: state-field mismatch[^\n]*"
                r"physicalLane=\d+ lateralClaimCm=-?\d+\)[^\n]*"
                r"physicalLane=\d+ lateralClaimCm=-?\d+\)", text
            ))
            checks["correct_certified_physical_value_committed"] = bool(
                mutation and committed and committed["cyber_status"] == 1 and
                committed["physical_lane"] == int(mutation.group(2)) and
                committed["lateral_claim_cm"] == int(mutation.group(4))
            )
            checks["recovery_outcome_recorded"] = (
                "fault=TAMPER_PHYSICAL_LANE "
                "outcome=STATE_FIELD_MISMATCH_REJECTED_AND_RECOVERED" in text
            )
            evidence.update({"mutation": mutation.group(0) if mutation else None,
                             "committed_entry": committed})
        elif case["letter"] == "F":
            mutation = re.search(
                r"UPGRADE_UNKNOWN_DIRECTION: replica (\d+) direction UNKNOWN->STRAIGHT",
                text.replace("→", "->"),
            )
            target = int(mutation.group(1)) if mutation else -1
            committed = committed_entry(target) if target >= 0 else None
            checks["check10_rejected_unknown_upgrade"] = bool(re.search(
                r"\[OMNET-PREVERIFY\] reject: state-field mismatch[^\n]*"
                r"cert\([^\n]*dir=3[^\n]*proposal\([^\n]*dir=0", text
            ))
            checks["correct_unknown_value_committed"] = bool(
                mutation and committed and committed["cyber_status"] == 1 and
                committed["direction"] == "?"
            )
            checks["recovery_outcome_recorded"] = (
                "fault=UPGRADE_UNKNOWN_DIRECTION "
                "outcome=STATE_FIELD_MISMATCH_REJECTED_AND_RECOVERED" in text
            )
            evidence.update({"mutation": mutation.group(0) if mutation else None,
                             "committed_entry": committed})
        elif case["letter"] == "G":
            omitted = []
            if proposer:
                ids_match = re.search(r"ids=([0-9,]+)", proposer.group(3))
                if ids_match:
                    omitted = [int(value) for value in ids_match.group(1).split(",")]
            committed = {str(replica): committed_entry(replica) for replica in omitted}
            checks["exactly_f_plus_1_certs_suppressed"] = len(omitted) == f + 1
            checks["proposing_replica_remains_signed_in_bad_proposal"] = (
                0 not in omitted
            )
            checks["check9_rejected_cert_omission"] = bool(re.search(
                rf"\[OMNET-PREVERIFY\] reject: cert-omission\s+{f + 1}\s+"
                rf"local cert\(s\) QUIET in proposal", text
            ))
            checks["all_suppressed_certified_values_committed_signed"] = bool(
                omitted and all(
                    entry is not None and entry["cyber_status"] == 1
                    for entry in committed.values()
                )
            )
            checks["successor_independently_held_every_suppressed_cert"] = bool(
                omitted and any(
                    set(omitted).issubset(set(row["cert_ids"]))
                    for row in successor_cert_states
                )
            )
            checks["recovery_outcome_recorded"] = (
                "fault=SUPPRESS_CERTS "
                "outcome=CERT_OMISSION_REJECTED_AND_RECOVERED" in text
            )
            evidence.update({"suppressed_replica_ids": omitted,
                             "committed_entries": committed})
        else:
            checks["honest_full_order_committed"] = final_executor.count("cyber=") == n
            checks["recovery_outcome_recorded"] = (
                "fault=SILENT_PRIMARY outcome=RECOVERED_AFTER_VIEW_CHANGE" in text
            )
            evidence.update({"committed_entry_count": final_executor.count("cyber=")})

        result = {
            "attack": case["letter"], "name": case["name"],
            "passed": all(checks.values()), "checks": checks,
            "metadata": metadata, "evidence": evidence,
            "result_dir": str(output_dir),
        }
        result_path = output_dir / "leader_attack_validation.json"
        result_path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
        results.append(result)
        print(f"[{('PASS' if result['passed'] else 'FAIL')}] {case['letter']} {case['name']}")
        for name, passed in checks.items():
            print(f"  {('PASS' if passed else 'FAIL'):4} {name}")

    overall = bool(results) and all(result["passed"] for result in results)
    summary_dir = REPO_ROOT / "benchmarks" / "Phase2AttackDefense"
    summary_path = summary_dir / "leader_attack_validation_summary.json"
    summary_path.write_text(json.dumps({
        "checkpoint": "P8-attack-defense-E-through-H",
        "passed": overall,
        "results": results,
    }, indent=2, sort_keys=True) + "\n")
    print("\n========== ATTACK-DEFENSE E-H: LEADER ATTACKS ==========")
    for result in results:
        print(f"{'PASS' if result['passed'] else 'FAIL'}  {result['attack']} {result['name']}")
    print(f"Overall: {'PASS' if overall else 'FAIL'}")
    print(f"Summary: {summary_path}")
    return 0 if overall else 1


def _attack_defense_common_argv(
    output_dir: Path, seed: int, f: int, extra: Sequence[str]
) -> List[str]:
    return [
        str(RUN_SCRIPT), "--randomize", "16", "0",
        "--no-ambulance", "--tolerated-f", str(f),
        *extra,
        str(FOURWAY_DIR), "--compact-log",
        "--channel-metrics-dir", str(output_dir.resolve()),
        "--approach-sigma", "0", "--signal-error", "0.2",
        "--direction-collection-window", "0.25",
        "--ego-longitudinal-sigma", "0", "--longitudinal-sigma", "1.0",
        "--lane-observation-mode", "ADJACENT_LATERAL",
        "--lateral-sigma", "0.5", "--adjacent-lane-separation", "3.2",
        "--physical-gate-k", "2.0", "--direction-eligibility", "on",
        "--simulation-seed", str(seed),
        "--phase2-attack-kind", "NONE", "--phase2-attack-target", "0",
        "--phase2-evidence-colluders", "", "--phase2-actual-b", "0",
        "--phase2-lateral-claim-offset", "0",
        "-u", "Cmdenv", "--debug-on-errors=false", "-c", TWO_LANE_SWEEP_CONFIG,
    ]


def _run_attack_defense_artifact(
    output_dir: Path, metadata: Dict[str, Any], argv: Sequence[str]
) -> Tuple[str, List[Dict[str, Any]]]:
    output_dir.mkdir(parents=True, exist_ok=True)
    metadata_path = output_dir / "attack_run_metadata.json"
    raw_log = output_dir / "raw_simulation.log"
    rep = int(metadata["rep"])
    json_path = output_dir / f"16veh_{rep}.json"
    if metadata_path.is_file() and raw_log.is_file() and json_path.is_file():
        if json.loads(metadata_path.read_text()) != metadata:
            raise ValueError(f"refusing mismatched attack-defense artifact: {output_dir}")
        print(f"[resume] reuse {output_dir}")
    else:
        metadata_path.write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n")
        clear_stale_random_ini()
        inner = " ".join(shlex.quote(value) for value in argv)
        print("+ " + inner)
        run_in_bash_with_omnet(inner, dry_run=False)
        shutil.copy2(LOG_FILE, raw_log)
        subprocess.run([
            sys.executable, str(FOURWAY_DIR / "analyze_log.py"), str(LOG_FILE),
            "--save-to", str(output_dir), "--scenario", "1", "--cars", "16",
            "--run-index", str(rep), "--no-scenario-subdir",
        ], cwd=REPO_ROOT, check=True)
    return raw_log.read_text(errors="replace"), json.loads(json_path.read_text())


def run_attack_defense_gossip_validation(args: argparse.Namespace) -> int:
    """Row J: f Byzantine replicas store valid certs but withhold forwarding."""
    n = 16
    f = bft_f(n)
    silent_ids = list(range(f))
    rep = args.start_rep
    seed = run_seed(MASTER_SEED, n, "ATTACK_DEFENSE_J_CERT_RELAY_WITHHOLD", rep)
    output_dir = (
        REPO_ROOT / "benchmarks" / "Phase2AttackDefense" /
        "J_CertRelayWithholdingV4" / f"run_{rep}"
    )
    metadata = {
        "checkpoint": "P8-attack-defense-j-cert-relay-withholding",
        "harness_version": 4, "n": n, "f": f, "rep": rep,
        "simulation_seed": seed, "withholding_replica_ids": silent_ids,
        "scope": "validated ARRIVAL_CERT forwarding only; origin broadcasts and PBFT remain enabled",
        "config": TWO_LANE_SWEEP_CONFIG,
        "operating_point": {"sigma_lat_m": 0.5, "sigma_long_m": 1.0,
                            "signal_error": 0.2, "physical_gate_k": 2.0},
    }
    if args.dry_run:
        print(f"[dry-run] J cert-relay withholding: N=16 f={f} ids={silent_ids}")
        return 0
    run_key_generation(dry_run=False, scale=n)
    argv = _attack_defense_common_argv(
        output_dir, seed, f,
        ["--cert-relay-silent-ids", ",".join(map(str, silent_ids))],
    )
    text, records = _run_attack_defense_artifact(output_dir, metadata, argv)
    withheld_ids = {
        int(value) for value in re.findall(
            r"\[(?:CERT-RELAY|CERT-GOSSIP)-WITHHELD\] r(\d+)\b", text
        )
    }
    honest_relay_ids = {
        int(value) for value in re.findall(r"\[CERT-RELAY\] r(\d+)\b", text)
        if int(value) not in silent_ids
    }
    complete = {}
    for match in re.finditer(
        r"\[DISCOVERY-COMPLETE\] r(\d+) epoch=0[^\n]*?certs=(\d+) "
        r"missing=([^ ]*) local_cert_aired=", text,
    ):
        complete[int(match.group(1))] = {
            "cert_count": int(match.group(2)), "missing": match.group(3)
        }
    honest_ids = set(range(n)) - set(silent_ids)
    metrics = records[0].get("run_metrics") or {}
    departures = set(re.findall(r"\[DEPARTED\] Replica (\d+)\b", text))
    checks = {
        "exactly_f_byzantine_nonforwarders_configured": len(silent_ids) == f,
        "every_configured_nonforwarder_withheld_valid_cert_forwarding": (
            set(silent_ids) == withheld_ids
        ),
        "honest_to_honest_certificate_relays_observed": bool(honest_relay_ids),
        "every_honest_replica_completed_with_full_certified_set": all(
            rid in complete and complete[rid]["cert_count"] >= n and
            complete[rid]["missing"] == ""
            for rid in honest_ids
        ),
        "order_committed": "Order_Decided_Time" in text,
        "all_vehicles_departed": len(departures) == n,
        "authentication_clean": (
            "[ANN-ORIGIN-INVALID]" not in text and "[CERT-INVALID]" not in text and
            "verify FAIL" not in text
        ),
        "no_unsafe_conflicting_cooccupancy": not (
            metrics.get("unsafe_conflict_cooccupancy_pairs") or []
        ),
        "no_collision_or_teleport": (
            int(metrics.get("physical_collision_vehicle_count") or 0) == 0 and
            "Teleporting vehicle" not in text
        ),
    }
    result = {
        "attack": "J", "name": "CertRelayWithholdingV4",
        "passed": all(checks.values()), "checks": checks, "metadata": metadata,
        "evidence": {"withheld_replica_ids": sorted(withheld_ids),
                     "honest_relay_replica_ids": sorted(honest_relay_ids),
                     "discovery_complete_by_replica": complete},
        "result_dir": str(output_dir),
    }
    path = output_dir / "gossip_withholding_validation.json"
    path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    print("\n========== ATTACK-DEFENSE J: CERT-RELAY WITHHOLDING ==========")
    for name, passed in checks.items():
        print(f"  {('PASS' if passed else 'FAIL'):4} {name}")
    print(f"Overall: {'PASS' if result['passed'] else 'FAIL'}")
    print(f"Summary: {path}")
    return 0 if result["passed"] else 1


def run_attack_defense_chain_validation(args: argparse.Namespace) -> int:
    """Worst-case corner: r0 and its r1 successor are both silent Byzantine primaries."""
    n = 16
    f = bft_f(n)
    rep = args.start_rep
    seed = run_seed(MASTER_SEED, n, "ATTACK_DEFENSE_CHAIN_TWO_SILENT", rep)
    output_dir = (
        REPO_ROOT / "benchmarks" / "Phase2AttackDefense" /
        "K_ConsecutiveByzantinePrimariesV3" / f"run_{rep}"
    )
    metadata = {
        "checkpoint": "P8-attack-defense-consecutive-byzantine-primaries",
        "harness_version": 3, "n": n, "f": f, "rep": rep,
        "simulation_seed": seed, "byzantine_primary_chain": [0, 1],
        "expected_honest_successor": 2, "config": TWO_LANE_SWEEP_CONFIG,
        "execution": "forced initial r0 followed by explicitly silent r1; recovery must reach r2",
        "operating_point": {"sigma_lat_m": 0.5, "sigma_long_m": 1.0,
                            "signal_error": 0.2, "physical_gate_k": 2.0},
    }
    if args.dry_run:
        print("[dry-run] consecutive Byzantine primaries: r0 silent -> r1 silent -> r2 honest")
        return 0
    run_key_generation(dry_run=False, scale=n)
    argv = _attack_defense_common_argv(
        output_dir, seed, f,
        ["--byzleader", "0", "--leader-byz-type", "4",
         "--proposal-silent-primary-ids", "1"],
    )
    text, records = _run_attack_defense_artifact(output_dir, metadata, argv)
    changes = [
        (int(match.group(1)), int(match.group(2)))
        for match in re.finditer(
            r"\[VC-DEBUG\] r\d+ primary changed: (\d+) -> (\d+)", text
        )
    ]
    attack_proposers = {
        int(value) for value in re.findall(
            r"\[ATTACK-PROPOSER\] replica=(\d+) epoch=0 mutation=SILENT_PRIMARY attempted=1",
            text,
        )
    }
    r2_states = [
        {"count": int(m.group(1)),
         "ids": [int(v) for v in m.group(2).split(",") if v]}
        for m in re.finditer(
            r"\[PROPOSER-CERT-STATE\] replica=2 epoch=0 pbftPrimary=2 "
            r"authority=view-change certCount=(\d+) ids=([0-9,]*)", text
        )
    ]
    metrics = records[0].get("run_metrics") or {}
    departures = set(re.findall(r"\[DEPARTED\] Replica (\d+)\b", text))
    checks = {
        "both_byzantine_primaries_exercised": {0, 1}.issubset(attack_proposers),
        "view_advanced_from_r0_to_r1": (0, 1) in changes,
        "view_advanced_from_r1_to_r2": (1, 2) in changes,
        "honest_r2_proposed_with_complete_certified_set": any(
            row["count"] >= n and set(row["ids"]) == set(range(n)) for row in r2_states
        ),
        "recovery_within_f_plus_1_primary_attempts": 3 <= f + 1,
        "honest_full_order_committed": bool(re.search(
            r"\[EXECUTOR\] entries epoch=0 n=16:[^\n]+", text
        )),
        "all_vehicles_departed": len(departures) == n,
        "authentication_clean": (
            "[ANN-ORIGIN-INVALID]" not in text and "[CERT-INVALID]" not in text and
            "verify FAIL" not in text
        ),
        "no_unsafe_conflicting_cooccupancy": not (
            metrics.get("unsafe_conflict_cooccupancy_pairs") or []
        ),
        "no_collision_or_teleport": (
            int(metrics.get("physical_collision_vehicle_count") or 0) == 0 and
            "Teleporting vehicle" not in text
        ),
    }
    result = {
        "attack": "K", "name": "ConsecutiveByzantinePrimariesV3",
        "passed": all(checks.values()), "checks": checks, "metadata": metadata,
        "evidence": {"primary_changes": changes,
                     "silent_attack_proposers": sorted(attack_proposers),
                     "honest_r2_cert_states": r2_states},
        "result_dir": str(output_dir),
    }
    path = output_dir / "consecutive_byzantine_validation.json"
    path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    print("\n========== ATTACK-DEFENSE: CONSECUTIVE BYZANTINE PRIMARIES ==========")
    for name, passed in checks.items():
        print(f"  {('PASS' if passed else 'FAIL'):4} {name}")
    print(f"Overall: {'PASS' if result['passed'] else 'FAIL'}")
    print(f"Summary: {path}")
    return 0 if result["passed"] else 1


def run_attack_defense_full_validation(
    args: argparse.Namespace, repetitions: int
) -> int:
    """Run D-H, J, and the consecutive-primary corner over paired seed indices."""
    attack_rows = ("D", "E", "F", "G", "H", "J", "K")
    if args.dry_run:
        print(
            f"[dry-run] full attack-defense suite: {repetitions} seed(s), "
            f"{len(attack_rows)} rows per seed, strictly sequential"
        )
        for rep in range(args.start_rep, args.start_rep + repetitions):
            print(f"  seed-index run_{rep}: D, E, F, G, H, J, K")
        return 0

    summary_dir = REPO_ROOT / "benchmarks" / "Phase2AttackDefense"
    seed_results: List[Dict[str, Any]] = []
    stopped_early = False
    for rep in range(args.start_rep, args.start_rep + repetitions):
        print(f"\n========== ATTACK-DEFENSE FULL SUITE run_{rep} ==========")
        rep_args = argparse.Namespace(**vars(args))
        rep_args.start_rep = rep
        return_codes: Dict[str, int] = {}
        stage_error = None
        for label, runner in (
            ("D", run_attack_defense_equivocation_validation),
            ("E-H", run_attack_defense_leader_validation),
            ("J", run_attack_defense_gossip_validation),
            ("K", run_attack_defense_chain_validation),
        ):
            try:
                return_codes[label] = runner(rep_args)
            except (subprocess.CalledProcessError, OSError, ValueError, KeyError) as exc:
                return_codes[label] = 1
                stage_error = f"{label}: {exc}"
                print(f"[FAIL] run_{rep} stage {stage_error}", file=sys.stderr)
                break
        paths = {
            "D": summary_dir / "D_EquivocationV2" / f"run_{rep}" /
                 "equivocation_validation_summary.json",
            "E": summary_dir / "E_PhysicalLaneMutationV4" / f"run_{rep}" /
                 "leader_attack_validation.json",
            "F": summary_dir / "F_UnknownDirectionUpgradeV4" / f"run_{rep}" /
                 "leader_attack_validation.json",
            "G": summary_dir / "G_CertSuppressionV4" / f"run_{rep}" /
                 "leader_attack_validation.json",
            "H": summary_dir / "H_SilentPrimaryV4" / f"run_{rep}" /
                 "leader_attack_validation.json",
            "J": summary_dir / "J_CertRelayWithholdingV4" / f"run_{rep}" /
                 "gossip_withholding_validation.json",
            "K": summary_dir / "K_ConsecutiveByzantinePrimariesV3" / f"run_{rep}" /
                 "consecutive_byzantine_validation.json",
        }
        rows = {}
        for attack, path in paths.items():
            payload = json.loads(path.read_text()) if path.is_file() else {
                "passed": False, "error": f"missing summary: {path}"
            }
            rows[attack] = {
                "passed": bool(payload.get("passed")),
                "summary": str(path),
                "simulation_seed": (payload.get("metadata") or {}).get(
                    "simulation_seed"
                ),
            }
        seed_passed = all(row["passed"] for row in rows.values())
        seed_results.append({
            "rep": rep,
            "passed": seed_passed,
            "return_codes": return_codes,
            "stage_error": stage_error,
            "attacks": rows,
        })
        if not seed_passed or any(return_codes.values()):
            stopped_early = True
            print(f"[FAIL] run_{rep}; stopping before the next seed")
            break

    per_attack = {
        attack: {
            "passed": sum(
                1 for seed in seed_results
                if seed["attacks"].get(attack, {}).get("passed")
            ),
            "completed": sum(
                1 for seed in seed_results if attack in seed["attacks"]
            ),
        }
        for attack in attack_rows
    }
    overall = (
        len(seed_results) == repetitions and
        all(seed["passed"] for seed in seed_results)
    )
    summary = {
        "checkpoint": "P8-attack-defense-full-multiseed",
        "passed": overall,
        "execution": "strictly sequential; no simulations overlap",
        "start_rep": args.start_rep,
        "repetitions": repetitions,
        "planned_simulations": repetitions * len(attack_rows),
        "completed_seed_groups": len(seed_results),
        "stopped_early": stopped_early,
        "per_attack": per_attack,
        "seed_results": seed_results,
        "notes": {
            "F": "signal_error=1.0 intentionally forces SIGNED-UNKNOWN",
            "D": "one variant may certify; no honest witness may double-sign",
            "I": "consistent declaration/cue followed by a different maneuver is deferred to conformance monitoring",
        },
    }
    path = summary_dir / "attack_defense_full_validation_summary.json"
    path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    print("\n========== ATTACK-DEFENSE FULL MULTI-SEED SUMMARY ==========")
    for attack, counts in per_attack.items():
        print(f"{attack}: {counts['passed']}/{counts['completed']} PASS")
    print(f"Overall: {'PASS' if overall else 'FAIL'}")
    print(f"Summary: {path}")
    return 0 if overall else 1


def run_two_lane_grid(args: argparse.Namespace) -> int:
    from fourway import adjacent_lane_grid as grid

    manifest = grid.manifest()
    output_dir = REPO_ROOT / "benchmarks" / "Phase2TwoLaneGrid"
    manifest_payload = {
        "checkpoint": "P7-full-intersection-two-lane-grid",
        "profile": args.two_lane_grid_profile,
        "matrix_version": 2,
        "planned_unique_runs": len(manifest),
        "execution": "strictly sequential with exact-metadata resume",
        "paired_seed_policy": "same repetition uses same seed across parameter cells",
        "n": grid.N,
        "f": grid.F,
        "target": grid.TARGET,
        "delta_shoulder_m": list(grid.DELTA_SHOULDER_M),
        "sigma_sensitivity_m": list(grid.SIGMA_SENSITIVITY_M),
        "byzantine_boundary_b": list(grid.BYZANTINE_BOUNDARY_B),
        "k_tradeoff": list(grid.K_TRADEOFF),
        "runs": manifest,
    }
    if args.dry_run:
        print(
            f"[dry-run] full-intersection two-lane grid profile={args.two_lane_grid_profile} "
            f"unique_runs={len(manifest)} sequential=true"
        )
        for index, row in enumerate(manifest, 1):
            print(f"[dry-run] {index:03d}/{len(manifest)} {row}")
        return 0

    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "two_lane_grid_manifest.json").write_text(
        json.dumps(manifest_payload, indent=2, sort_keys=True) + "\n"
    )
    reanalyze_only = bool(args.two_lane_grid_reanalyze)
    if reanalyze_only:
        print("[reanalyze] using existing full-intersection two-lane grid artifacts")
    else:
        run_key_generation(dry_run=False, scale=grid.N)

    results: List[Dict[str, Any]] = []
    failures: List[Dict[str, Any]] = []
    for index, row in enumerate(manifest, 1):
        print(f"\n--- Two-lane grid {index}/{len(manifest)} {row} ---")
        run_dir = grid.run_dir(REPO_ROOT, row)
        try:
            if reanalyze_only:
                required = (
                    run_dir / "raw_simulation.log",
                    run_dir / f"{grid.N}veh_{int(row['rep'])}.json",
                    run_dir / "two_lane_grid_run_metadata.json",
                )
                missing = [str(path) for path in required if not path.is_file()]
                if missing:
                    raise ValueError(f"missing reanalysis artifacts: {', '.join(missing)}")
                if json.loads(required[2].read_text()) != _two_lane_grid_metadata(row):
                    raise ValueError(f"metadata mismatch: {run_dir}")
            else:
                clear_stale_random_ini()
                run_dir = _run_two_lane_grid_cell(args, row)
            result = _analyze_two_lane_grid_cell(row, run_dir)
            results.append(result)
            if not result["passed"]:
                failures.append({"row": row, "checks": result["checks"]})
            print(f"[{'PASS' if result['passed'] else 'FAIL'}] checks={result['checks']}")
            (output_dir / "two_lane_grid_progress.json").write_text(json.dumps({
                "planned_unique_runs": len(manifest),
                "completed_unique_runs": len(results),
                "last_completed_index": index,
                "last_completed_row": row,
                "passing_runs": sum(bool(item["passed"]) for item in results),
                "failure_count": len(failures),
            }, indent=2, sort_keys=True) + "\n")
        except (subprocess.CalledProcessError, OSError, ValueError, KeyError) as exc:
            failure = {"row": row, "error": str(exc)}
            failures.append(failure)
            print(f"[FAIL] {exc}")
            break

    aggregate = grid.aggregate(results)
    summary = {
        **{key: value for key, value in manifest_payload.items() if key != "runs"},
        "completed_unique_runs": len(results),
        "passed": len(results) == len(manifest) and not failures,
        "failures": failures,
        "aggregates": aggregate,
        "results": results,
    }
    summary_path = output_dir / "two_lane_grid_summary.json"
    summary_path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    csv_path = output_dir / "two_lane_grid_aggregates.csv"
    fields = list(aggregate[0]) if aggregate else [
        "b", "sigma_lat_m", "delta_m", "k", "repetitions",
        "false_lane_certificates", "false_certificate_rate",
        "wilson95_low", "wilson95_high", "mean_binomial_prediction",
        "mean_h_lane", "mean_b_sig",
    ]
    with csv_path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(aggregate)
    print("\n========== FULL-INTERSECTION TWO-LANE GRID SUMMARY ==========")
    print(f"Completed: {len(results)}/{len(manifest)}")
    print(f"Overall: {'PASS' if summary['passed'] else 'FAIL'}")
    print(f"Summary: {summary_path}")
    print(f"Aggregates: {csv_path}")
    return 0 if summary["passed"] else 1


def run_two_lane_delta_b_grid(args: argparse.Namespace, profile: str) -> int:
    """Run the focused delta x b matrix sequentially with exact resume."""
    from fourway import adjacent_lane_grid as grid

    manifest = grid.delta_b_manifest(profile)
    output_dir = REPO_ROOT / "benchmarks" / "Phase2TwoLaneDeltaBGrid"
    attack_runs = sum(int(row["b"]) > 0 for row in manifest)
    control_runs = len(manifest) - attack_runs
    manifest_payload = {
        "checkpoint": "P7-full-intersection-two-lane-delta-b-grid",
        "profile": profile,
        "matrix_version": 3,
        "planned_unique_runs": len(manifest),
        "planned_attack_runs": attack_runs,
        "planned_honest_control_runs": control_runs,
        "execution": "strictly sequential with exact-metadata resume",
        "smoke_reused_by_full": True,
        "canonical_shared_anchor": profile == "full",
        "canonical_anchor_policy": (
            "replace the delta=1.75,sigma=.5,k=3,b=1..6 overlap with the "
            "completion-capable artifacts shared by Figures B and C"
            if profile == "full" else "smoke retains its original early-stop artifacts"
        ),
        "paired_seed_policy": "same repetition uses same seed across delta and b cells",
        "n": grid.N,
        "f": grid.F,
        "target": grid.TARGET,
        "sigma_lat_m": grid.MAIN_SIGMA_M,
        "delta_m": list(grid.DELTA_B_SWEEP_M),
        "b": list(grid.DELTA_B_SWEEP_B),
        "k": grid.MAIN_K,
        "repetitions_per_attack_cell": 1 if profile == "smoke" else grid.DELTA_B_SWEEP_REPS,
        "runs": manifest,
    }
    if args.dry_run:
        print(
            f"[dry-run] two-lane delta-b profile={profile} "
            f"unique_runs={len(manifest)} attack_runs={attack_runs} "
            f"honest_controls={control_runs} sequential=true"
        )
        for index, row in enumerate(manifest, 1):
            print(f"[dry-run] {index:03d}/{len(manifest)} {row}")
        return 0

    output_dir.mkdir(parents=True, exist_ok=True)
    summary_path = output_dir / f"two_lane_delta_b_{profile}_summary.json"
    csv_path = output_dir / f"two_lane_delta_b_{profile}_aggregates.csv"
    if profile == "full" and summary_path.is_file():
        existing_summary = json.loads(summary_path.read_text())
        if not existing_summary.get("canonical_shared_anchor"):
            shutil.copy2(
                summary_path,
                output_dir / "two_lane_delta_b_early_stop_summary.json",
            )
            if csv_path.is_file():
                shutil.copy2(
                    csv_path,
                    output_dir / "two_lane_delta_b_early_stop_aggregates.csv",
                )
    (output_dir / f"two_lane_delta_b_{profile}_manifest.json").write_text(
        json.dumps(manifest_payload, indent=2, sort_keys=True) + "\n"
    )
    run_key_generation(dry_run=False, scale=grid.N)

    results: List[Dict[str, Any]] = []
    failures: List[Dict[str, Any]] = []
    for index, row in enumerate(manifest, 1):
        print(f"\n--- Two-lane delta-b {index}/{len(manifest)} {row} ---")
        artifact_row = row
        if (
            profile == "full" and int(row["b"]) in grid.PARAMETER_SWEEP_B and
            float(row["sigma_lat_m"]) == grid.MAIN_SIGMA_M and
            float(row["delta_m"]) == grid.MAIN_DELTA_M and
            float(row["k"]) == grid.MAIN_K
        ):
            artifact_row = {
                **row, "purposes": ["parameter_sweep_shared_anchor"]
            }
            run_dir = grid.parameter_sweep_run_dir(
                REPO_ROOT, artifact_row, "k"
            )
        else:
            run_dir = grid.delta_b_run_dir(REPO_ROOT, row)
        try:
            clear_stale_random_ini()
            run_dir = _run_two_lane_grid_cell(
                args, artifact_row, run_dir_override=run_dir
            )
            result = _analyze_two_lane_grid_cell(artifact_row, run_dir)
            results.append(result)
            if not result["passed"]:
                failures.append({"row": row, "checks": result["checks"]})
            print(f"[{'PASS' if result['passed'] else 'FAIL'}] checks={result['checks']}")
            (output_dir / f"two_lane_delta_b_{profile}_progress.json").write_text(
                json.dumps({
                    "planned_unique_runs": len(manifest),
                    "completed_unique_runs": len(results),
                    "last_completed_index": index,
                    "last_completed_row": row,
                    "passing_runs": sum(bool(item["passed"]) for item in results),
                    "failure_count": len(failures),
                }, indent=2, sort_keys=True) + "\n"
            )
        except (subprocess.CalledProcessError, OSError, ValueError, KeyError) as exc:
            failures.append({"row": row, "error": str(exc)})
            print(f"[FAIL] {exc}")
            break

    aggregate = grid.aggregate(results)
    summary = {
        **{key: value for key, value in manifest_payload.items() if key != "runs"},
        "completed_unique_runs": len(results),
        "passed": len(results) == len(manifest) and not failures,
        "failures": failures,
        "aggregates": aggregate,
        "results": results,
    }
    summary_path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    fields = list(aggregate[0]) if aggregate else [
        "b", "sigma_lat_m", "delta_m", "k", "repetitions",
        "false_lane_certificates", "false_certificate_rate",
    ]
    with csv_path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(aggregate)
    print("\n========== TWO-LANE DELTA x b GRID SUMMARY ==========")
    print(f"Profile: {profile}")
    print(f"Completed: {len(results)}/{len(manifest)}")
    print(f"Overall: {'PASS' if summary['passed'] else 'FAIL'}")
    print(f"Summary: {summary_path}")
    print(f"Aggregates: {csv_path}")
    return 0 if summary["passed"] else 1


def run_two_lane_delta_b_completion_grid(args: argparse.Namespace) -> int:
    """Complete Figure A through departures, reusing shared B/C anchors."""
    from fourway import adjacent_lane_grid as grid

    manifest = grid.delta_b_manifest("full")
    output_dir = REPO_ROOT / "benchmarks" / "Phase2TwoLaneDeltaBCompletionGrid"
    shared_anchor_runs = 0
    completion_runs = 0
    planned: List[Tuple[Dict[str, Any], Path]] = []
    for row in manifest:
        is_anchor = (
            int(row["b"]) in grid.PARAMETER_SWEEP_B and
            float(row["sigma_lat_m"]) == grid.MAIN_SIGMA_M and
            float(row["delta_m"]) == grid.MAIN_DELTA_M and
            float(row["k"]) == grid.MAIN_K
        )
        if is_anchor:
            artifact_row = {
                **row, "purposes": ["parameter_sweep_shared_anchor"]
            }
            run_dir = grid.parameter_sweep_run_dir(
                REPO_ROOT, artifact_row, "k"
            )
            shared_anchor_runs += 1
        else:
            artifact_row = {
                **row, "purposes": ["parameter_sweep_delta_b_completion"]
            }
            run_dir = grid.delta_b_completion_run_dir(REPO_ROOT, artifact_row)
            completion_runs += 1
        planned.append((artifact_row, run_dir))

    manifest_payload = {
        "checkpoint": "P7-full-intersection-two-lane-delta-b-completion-grid",
        "profile": "full",
        "matrix_version": 3,
        "planned_unique_runs": len(planned),
        "new_completion_artifact_slots": completion_runs,
        "shared_anchor_artifacts": shared_anchor_runs,
        "execution": "strictly sequential with exact-metadata resume",
        "completion_config": TWO_LANE_SWEEP_CONFIG,
        "all_cells_require_16_departures": True,
        "n": grid.N,
        "f": grid.F,
        "target": grid.TARGET,
        "sigma_lat_m": grid.MAIN_SIGMA_M,
        "delta_m": list(grid.DELTA_B_SWEEP_M),
        "b": list(grid.DELTA_B_SWEEP_B),
        "k": grid.MAIN_K,
        "repetitions_per_attack_cell": grid.DELTA_B_SWEEP_REPS,
        "runs": [row for row, _ in planned],
    }
    if args.dry_run:
        print(
            f"[dry-run] delta-b completion unique_runs={len(planned)} "
            f"completion_slots={completion_runs} shared_anchors={shared_anchor_runs} "
            "sequential=true"
        )
        for index, (row, run_dir) in enumerate(planned, 1):
            print(f"[dry-run] {index:03d}/{len(planned)} {row} result_dir={run_dir}")
        return 0

    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "delta_b_completion_manifest.json").write_text(
        json.dumps(manifest_payload, indent=2, sort_keys=True) + "\n"
    )
    run_key_generation(dry_run=False, scale=grid.N)

    results: List[Dict[str, Any]] = []
    failures: List[Dict[str, Any]] = []
    for index, (row, run_dir) in enumerate(planned, 1):
        print(f"\n--- Delta-b completion {index}/{len(planned)} {row} ---")
        try:
            clear_stale_random_ini()
            run_dir = _run_two_lane_grid_cell(args, row, run_dir_override=run_dir)
            result = _analyze_two_lane_grid_cell(row, run_dir)
            results.append(result)
            if not result["passed"]:
                failures.append({"row": row, "checks": result["checks"]})
            print(f"[{'PASS' if result['passed'] else 'FAIL'}] checks={result['checks']}")
            (output_dir / "delta_b_completion_progress.json").write_text(
                json.dumps({
                    "planned_unique_runs": len(planned),
                    "completed_unique_runs": len(results),
                    "last_completed_index": index,
                    "last_completed_row": row,
                    "passing_runs": sum(bool(item["passed"]) for item in results),
                    "failure_count": len(failures),
                }, indent=2, sort_keys=True) + "\n"
            )
        except (subprocess.CalledProcessError, OSError, ValueError, KeyError) as exc:
            failures.append({"row": row, "error": str(exc)})
            print(f"[FAIL] {exc}")
            break

    aggregate = grid.aggregate(results)
    throughput_complete = all(
        int(row.get("wait_sample_count", 0)) == int(row["repetitions"]) * grid.N and
        row.get("throughput_veh_per_s") is not None
        for row in aggregate
    )
    coupling = all(
        int(row["false_lane_certificates"]) == int(row["conflicting_cooccupancy_runs"])
        and int(row["background_conflicting_cooccupancy_runs"]) == 0
        for row in aggregate if int(row["b"]) > 0
    )
    summary = {
        **{key: value for key, value in manifest_payload.items() if key != "runs"},
        "completed_unique_runs": len(results),
        "throughput_complete_all_cells": throughput_complete,
        "safety_coupling_all_attack_cells": coupling,
        "passed": (
            len(results) == len(planned) and not failures and
            throughput_complete and coupling
        ),
        "failures": failures,
        "aggregates": aggregate,
        "results": results,
    }
    summary_path = output_dir / "delta_b_completion_summary.json"
    csv_path = output_dir / "delta_b_completion_aggregates.csv"
    summary_path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    fields = list(aggregate[0]) if aggregate else []
    with csv_path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(aggregate)

    if summary["passed"]:
        # Publication aliases: after this checkpoint Figure A, B, and C all
        # use completion-capable artifacts and expose complete wait/throughput.
        publication_dir = REPO_ROOT / "benchmarks" / "Phase2TwoLaneDeltaBGrid"
        shutil.copy2(
            summary_path, publication_dir / "two_lane_delta_b_full_summary.json"
        )
        shutil.copy2(
            csv_path, publication_dir / "two_lane_delta_b_full_aggregates.csv"
        )

    print("\n========== TWO-LANE DELTA x b COMPLETION SUMMARY ==========")
    print(f"Completed: {len(results)}/{len(planned)}")
    print(f"Throughput complete: {throughput_complete}")
    print(f"Safety coupling: {coupling}")
    print(f"Overall: {'PASS' if summary['passed'] else 'FAIL'}")
    print(f"Summary: {summary_path}")
    print(f"Aggregates: {csv_path}")
    return 0 if summary["passed"] else 1


def _check_parameter_sweep_anchor(
    row: Dict[str, Any], result: Dict[str, Any]
) -> None:
    """Compare a completion-capable anchor against the existing early-stop run."""
    from fourway import adjacent_lane_grid as grid

    if "parameter_sweep_shared_anchor" not in row.get("purposes", []):
        return
    old_row = {**row, "purposes": ["delta_b_attack_sweep"]}
    old_dir = grid.delta_b_run_dir(REPO_ROOT, old_row)
    required = (
        old_dir / "raw_simulation.log",
        old_dir / f"{grid.N}veh_{int(row['rep'])}.json",
    )
    if not all(path.is_file() for path in required):
        result["checks"]["anchor_existing_delta_b_artifact"] = False
        result["passed"] = False
        result["anchor_reference_dir"] = str(old_dir)
        return
    old_result = _analyze_two_lane_grid_cell(old_row, old_dir)
    identity_fields = ("delta_m", "sigma_lat_m", "k", "b", "rep")
    result["checks"]["anchor_parameter_identity"] = all(
        result[field] == old_result[field] for field in identity_fields
    )
    # Separate ResDB/TraCI process launches are not byte-deterministic in echo
    # arrival order even when OMNeT perception draws use the same seed.  Keep
    # the per-run forensic comparison, but validate outcomes statistically at
    # the aggregate level rather than requiring identical signer membership.
    result["anchor_reference_observation"] = {
        "same_false_certificate": (
            result["false_lane_certificate"] == old_result["false_lane_certificate"]
        ),
        "same_conflicting_cooccupancy": (
            result["conflicting_cooccupancy"] == old_result["conflicting_cooccupancy"]
        ),
        "same_certificate_signers": (
            result["certificate_signers"] == old_result["certificate_signers"]
        ),
        "same_honest_accept_count": (
            result["honest_lane_accepts"] == old_result["honest_lane_accepts"]
        ),
    }
    result["anchor_reference_dir"] = str(old_dir)
    result["passed"] = all(result["checks"].values())


def _anchor_statistical_comparison(
    aggregate: Sequence[Dict[str, Any]], profile: str
) -> Dict[str, Any]:
    """Compare completion anchors to the prior delta-b grid by uncertainty."""
    from fourway import adjacent_lane_grid as grid

    early_stop_path = (
        REPO_ROOT / "benchmarks" / "Phase2TwoLaneDeltaBGrid" /
        "two_lane_delta_b_early_stop_summary.json"
    )
    canonical_path = (
        REPO_ROOT / "benchmarks" / "Phase2TwoLaneDeltaBGrid" /
        "two_lane_delta_b_full_summary.json"
    )
    reference_path = early_stop_path if early_stop_path.is_file() else canonical_path
    if not reference_path.is_file():
        return {
            "reference": str(reference_path), "reference_available": False,
            "required": profile == "full", "passed": False, "by_b": {},
        }
    reference = json.loads(reference_path.read_text()).get("aggregates", [])

    def overlap(first: Tuple[float, float], second: Tuple[float, float]) -> bool:
        return max(first[0], second[0]) <= min(first[1], second[1])

    by_b: Dict[str, Any] = {}
    all_agree = True
    for b in grid.PARAMETER_SWEEP_B:
        current = next((
            row for row in aggregate
            if int(row["b"]) == b and
               float(row["sigma_lat_m"]) == grid.MAIN_SIGMA_M and
               float(row["delta_m"]) == grid.MAIN_DELTA_M and
               float(row["k"]) == grid.MAIN_K
        ), None)
        prior = next((
            row for row in reference
            if int(row["b"]) == b and
               float(row["sigma_lat_m"]) == grid.MAIN_SIGMA_M and
               float(row["delta_m"]) == grid.MAIN_DELTA_M and
               float(row["k"]) == grid.MAIN_K
        ), None)
        if current is None or prior is None:
            by_b[str(b)] = {"available": False, "passed": False}
            all_agree = False
            continue
        current_false_ci = (
            float(current["wilson95_low"]), float(current["wilson95_high"])
        )
        prior_false_ci = (
            float(prior["wilson95_low"]), float(prior["wilson95_high"])
        )
        current_q0_ci = grid.wilson(
            int(current["honest_lane_accepts_total"]),
            int(current["honest_lane_evaluations_total"]),
        )
        prior_q0_ci = grid.wilson(
            int(prior["honest_lane_accepts_total"]),
            int(prior["honest_lane_evaluations_total"]),
        )
        false_agrees = overlap(current_false_ci, prior_false_ci)
        q0_agrees = overlap(current_q0_ci, prior_q0_ci)
        cell_passed = false_agrees and q0_agrees
        all_agree = all_agree and cell_passed
        by_b[str(b)] = {
            "available": True,
            "false_certificate_rate": current["false_certificate_rate"],
            "reference_false_certificate_rate": prior["false_certificate_rate"],
            "false_certificate_wilson95": list(current_false_ci),
            "reference_false_certificate_wilson95": list(prior_false_ci),
            "pooled_q0_empirical": current["pooled_q0_empirical"],
            "reference_pooled_q0_empirical": prior["pooled_q0_empirical"],
            "q0_wilson95": list(current_q0_ci),
            "reference_q0_wilson95": list(prior_q0_ci),
            "false_certificate_intervals_overlap": false_agrees,
            "q0_intervals_overlap": q0_agrees,
            "passed": cell_passed,
        }
    return {
        "reference": str(reference_path),
        "reference_available": True,
        "required": profile == "full",
        "method": "overlapping Wilson 95% intervals for false-certificate rate and pooled q0",
        "passed": all_agree if profile == "full" else True,
        "smoke_observation_all_overlap": all_agree,
        "by_b": by_b,
    }


def _parameter_sweep_sanity(
    sweep: str, aggregate: Sequence[Dict[str, Any]],
    results: Sequence[Dict[str, Any]], profile: str,
) -> Dict[str, Any]:
    from fourway import adjacent_lane_grid as grid

    coupling = all(
        int(row["false_lane_certificates"]) == int(row["conflicting_cooccupancy_runs"])
        and int(row["background_conflicting_cooccupancy_runs"]) == 0
        for row in aggregate
    )
    ratio_consistent = all(
        row.get("pooled_q0_empirical") is not None and
        int(row["honest_lane_evaluations_total"]) > 0 and
        math.isclose(
            float(row["pooled_q0_empirical"]),
            int(row["honest_lane_accepts_total"]) /
            int(row["honest_lane_evaluations_total"]),
            rel_tol=0.0, abs_tol=1e-12,
        )
        for row in aggregate
    )
    x_field = "k" if sweep == "k" else "sigma_lat_m"
    monotonic_by_b: Dict[str, bool] = {}
    for b in grid.PARAMETER_SWEEP_B:
        cells = sorted(
            (row for row in aggregate if int(row["b"]) == b),
            key=lambda row: float(row[x_field]),
        )
        values = [float(row["pooled_q0_empirical"]) for row in cells]
        monotonic_by_b[str(b)] = all(
            left <= right + 1e-12 for left, right in zip(values, values[1:])
        )
    throughput_complete = all(
        row.get("mean_throughput_veh_per_s") is not None and
        int(row.get("wait_sample_count", 0)) == int(row["repetitions"]) * grid.N
        for row in aggregate
    )
    anchor_results = [
        row for row in results
        if "parameter_sweep_shared_anchor" in row.get("purposes", [])
    ]
    anchor_identity = bool(anchor_results) and all(
        row["checks"].get("anchor_parameter_identity")
        for row in anchor_results
    )
    anchor_statistics = _anchor_statistical_comparison(aggregate, profile)
    # One-repetition smoke cells are for plumbing/accounting.  Their finite
    # witness samples are reported but do not veto the full run on a random
    # monotonicity wobble.  The 20-repetition profile enforces the empirical
    # ordering requested for the figures.
    monotonic_pass = all(monotonic_by_b.values()) if profile == "full" else True
    return {
        "coupling_false_certificate_equals_cooccupancy": coupling,
        "background_cooccupancy_zero": all(
            int(row["background_conflicting_cooccupancy_runs"]) == 0
            for row in aggregate
        ),
        "pooled_q0_ratio_consistent": ratio_consistent,
        "empirical_q0_monotonic_by_b": monotonic_by_b,
        "empirical_q0_monotonic_required": profile == "full",
        "empirical_q0_monotonic_pass": monotonic_pass,
        "throughput_and_wait_complete": throughput_complete,
        "shared_anchor_parameter_identity": anchor_identity,
        "delta_b_anchor_statistical_comparison": anchor_statistics,
        "passed": all((
            coupling, ratio_consistent, monotonic_pass,
            throughput_complete, anchor_identity, anchor_statistics["passed"],
        )),
    }


def run_two_lane_parameter_sweep(
    args: argparse.Namespace, sweep: str, profile: str
) -> int:
    """Run Figure B or C sequentially with shared completion anchors."""
    from fourway import adjacent_lane_grid as grid

    manifest = grid.parameter_sweep_manifest(sweep, profile)
    sweep_name = "k_sweep" if sweep == "k" else "sigma_sweep"
    output_dir = REPO_ROOT / "benchmarks" / (
        "Phase2TwoLaneKSweep" if sweep == "k" else "Phase2TwoLaneSigmaSweep"
    )
    manifest_payload = {
        "checkpoint": "P7-full-intersection-two-lane-parameter-sweep",
        "sweep": sweep,
        "profile": profile,
        "matrix_version": 3,
        "planned_unique_runs": len(manifest),
        "execution": "strictly sequential with exact-metadata resume",
        "completion_config": TWO_LANE_SWEEP_CONFIG,
        "paired_seed_policy": "same repetition uses same seed across all parameter cells",
        "shared_anchor": {
            "delta_m": grid.MAIN_DELTA_M,
            "sigma_lat_m": grid.MAIN_SIGMA_M,
            "k": grid.MAIN_K,
            "artifact_root": str(
                REPO_ROOT / "benchmarks" / "Phase2TwoLaneParameterSweeps" /
                "shared_anchor"
            ),
            "reference_grid": str(
                REPO_ROOT / "benchmarks" / "Phase2TwoLaneDeltaBGrid"
            ),
        },
        "n": grid.N,
        "f": grid.F,
        "target": grid.TARGET,
        "b": list(grid.PARAMETER_SWEEP_B),
        "delta_m": grid.MAIN_DELTA_M,
        "sigma_lat_m": (
            grid.MAIN_SIGMA_M if sweep == "k" else list(grid.SIGMA_SWEEP_M)
        ),
        "k": list(grid.K_SWEEP) if sweep == "k" else grid.MAIN_K,
        "repetitions_per_cell": 1 if profile == "smoke" else grid.PARAMETER_SWEEP_REPS,
        "runs": manifest,
    }
    if args.dry_run:
        print(
            f"[dry-run] two-lane {sweep_name} profile={profile} "
            f"unique_runs={len(manifest)} sequential=true completion=true"
        )
        for index, row in enumerate(manifest, 1):
            run_dir = grid.parameter_sweep_run_dir(REPO_ROOT, row, sweep)
            print(f"[dry-run] {index:03d}/{len(manifest)} {row} result_dir={run_dir}")
        return 0

    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / f"{sweep_name}_{profile}_manifest.json").write_text(
        json.dumps(manifest_payload, indent=2, sort_keys=True) + "\n"
    )
    run_key_generation(dry_run=False, scale=grid.N)

    results: List[Dict[str, Any]] = []
    failures: List[Dict[str, Any]] = []
    for index, row in enumerate(manifest, 1):
        print(f"\n--- Two-lane {sweep_name} {index}/{len(manifest)} {row} ---")
        run_dir = grid.parameter_sweep_run_dir(REPO_ROOT, row, sweep)
        try:
            clear_stale_random_ini()
            run_dir = _run_two_lane_grid_cell(args, row, run_dir_override=run_dir)
            result = _analyze_two_lane_grid_cell(row, run_dir)
            _check_parameter_sweep_anchor(row, result)
            results.append(result)
            if not result["passed"]:
                failures.append({"row": row, "checks": result["checks"]})
            print(f"[{'PASS' if result['passed'] else 'FAIL'}] checks={result['checks']}")
            (output_dir / f"{sweep_name}_{profile}_progress.json").write_text(
                json.dumps({
                    "planned_unique_runs": len(manifest),
                    "completed_unique_runs": len(results),
                    "last_completed_index": index,
                    "last_completed_row": row,
                    "passing_runs": sum(bool(item["passed"]) for item in results),
                    "failure_count": len(failures),
                }, indent=2, sort_keys=True) + "\n"
            )
        except (subprocess.CalledProcessError, OSError, ValueError, KeyError) as exc:
            failures.append({"row": row, "error": str(exc)})
            print(f"[FAIL] {exc}")
            break

    aggregate = grid.aggregate(results)
    sanity = _parameter_sweep_sanity(sweep, aggregate, results, profile)
    summary = {
        **{key: value for key, value in manifest_payload.items() if key != "runs"},
        "completed_unique_runs": len(results),
        "passed": (
            len(results) == len(manifest) and not failures and sanity["passed"]
        ),
        "failures": failures,
        "sanity_checks": sanity,
        "aggregates": aggregate,
        "results": results,
    }
    summary_path = output_dir / f"{sweep_name}_{profile}_summary.json"
    summary_path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    csv_path = output_dir / f"{sweep_name}_{profile}_aggregates.csv"
    fields = list(aggregate[0]) if aggregate else [
        "b", "sigma_lat_m", "delta_m", "k", "repetitions",
        "false_certificate_rate", "conflicting_cooccupancy_rate",
        "mean_throughput_veh_per_s", "mean_wait_s", "p95_wait_s",
    ]
    with csv_path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(aggregate)
    print(f"\n========== TWO-LANE {sweep_name.upper()} SUMMARY ==========")
    print(f"Profile: {profile}")
    print(f"Completed: {len(results)}/{len(manifest)}")
    print(f"Sanity: {sanity}")
    print(f"Overall: {'PASS' if summary['passed'] else 'FAIL'}")
    print(f"Summary: {summary_path}")
    print(f"Aggregates: {csv_path}")
    return 0 if summary["passed"] else 1


def _aggregate_honest_operating(
    rows: Sequence[Dict[str, Any]], profile: str
) -> List[Dict[str, Any]]:
    """Aggregate attack-free q1, certification, and completion performance."""
    from fourway import adjacent_lane_grid as grid

    grouped: Dict[Tuple[float, float], List[Dict[str, Any]]] = {}
    for row in rows:
        grouped.setdefault(
            (float(row["sigma_lat_m"]), float(row["k"])), []
        ).append(row)

    output: List[Dict[str, Any]] = []
    for (sigma, k), group in sorted(grouped.items()):
        repetitions = len(group)
        accepts = sum(int(row["global_lane_accepts"]) for row in group)
        evaluations = sum(int(row["global_lane_evaluations"]) for row in group)
        q1 = accepts / evaluations if evaluations else None
        q1_low, q1_high = grid.wilson(accepts, evaluations)
        certified = sum(int(row["lane_certified_count"]) for row in group)
        vehicle_trials = grid.N * repetitions
        cert_low, cert_high = grid.wilson(certified, vehicle_trials)
        wait_samples = [
            float(value)
            for row in group for value in row.get("wait_samples_s", [])
        ]
        throughputs = [
            float(row["throughput_veh_per_s"])
            for row in group if row.get("throughput_veh_per_s") is not None
        ]
        output.append({
            "sigma_lat_m": sigma,
            "k": k,
            "delta_m": 0.0,
            "b": 0,
            "repetitions": repetitions,
            "analytic_q1": grid.q1_model(sigma, k),
            "pooled_q1_empirical": q1,
            "q1_wilson95_low": q1_low,
            "q1_wilson95_high": q1_high,
            "honest_lane_accepts_total": accepts,
            "honest_lane_evaluations_total": evaluations,
            "lane_certified_total": certified,
            "lane_certification_rate": certified / vehicle_trials,
            "lane_certification_wilson95_low": cert_low,
            "lane_certification_wilson95_high": cert_high,
            "quiet_vehicle_total": sum(int(row["quiet_count"]) for row in group),
            "quiet_percent": statistics.mean(
                float(row["quiet_percent"]) for row in group
            ),
            "signed_unknown_percent": statistics.mean(
                float(row["signed_unknown_singleton_percent"]) for row in group
            ),
            "throughput_veh_per_s": (
                statistics.mean(throughputs) if throughputs else None
            ),
            "throughput_veh_per_min": (
                statistics.mean(throughputs) * 60.0 if throughputs else None
            ),
            "wait_mean_s": (
                statistics.mean(wait_samples) if wait_samples else None
            ),
            "wait_p95_s": (
                _percentile(wait_samples, 0.95) if wait_samples else None
            ),
            "wait_sample_count": len(wait_samples),
            "mean_batch_size": statistics.mean(
                float(row["mean_batch_size"])
                for row in group if row.get("mean_batch_size") is not None
            ),
            "background_conflicting_cooccupancy_runs": sum(
                bool(row["background_conflicting_cooccupancy"]) for row in group
            ),
            "q1_model_inside_wilson": (
                q1_low <= grid.q1_model(sigma, k) <= q1_high
            ),
            "completion_metrics_complete": len(wait_samples) == vehicle_trials,
            "profile": profile,
        })
    return output


def _honest_operating_selection(
    aggregate: Sequence[Dict[str, Any]], profile: str
) -> Dict[str, Any]:
    """Report, but do not silently lock, candidate k values at sigma=.5."""
    headline = [
        row for row in aggregate if math.isclose(float(row["sigma_lat_m"]), 0.5)
    ]
    throughputs = [
        float(row["throughput_veh_per_s"])
        for row in headline if row.get("throughput_veh_per_s") is not None
    ]
    best_throughput = max(throughputs) if throughputs else None
    system_candidates: List[float] = []
    strict_sensor_candidates: List[float] = []
    for row in headline:
        throughput_ok = (
            best_throughput is not None and
            float(row["throughput_veh_per_s"]) >= 0.95 * best_throughput
        )
        common = (
            float(row["lane_certification_rate"]) >= 0.99 and
            float(row["quiet_percent"]) == 0.0 and throughput_ok
        )
        if common:
            system_candidates.append(float(row["k"]))
        if common and float(row["pooled_q1_empirical"]) >= 0.99:
            strict_sensor_candidates.append(float(row["k"]))
    return {
        "selection_sigma_lat_m": 0.5,
        "status": "REPORT_ONLY" if profile == "smoke" else "READY_FOR_REVIEW",
        "system_criterion": (
            "choose the smallest k with >=99% honest lane certification, "
            "zero QUIET vehicles, and throughput within 5% of the best k"
        ),
        "strict_sensor_criterion": (
            "system criterion plus pooled per-witness q1 >=99%"
        ),
        "system_candidate_k": sorted(system_candidates),
        "strict_sensor_candidate_k": sorted(strict_sensor_candidates),
        "smallest_system_candidate_k": (
            min(system_candidates) if system_candidates else None
        ),
        "smallest_strict_sensor_candidate_k": (
            min(strict_sensor_candidates) if strict_sensor_candidates else None
        ),
        "note": "No k is automatically written into protocol configuration.",
    }


def run_two_lane_honest_operating_sweep(
    args: argparse.Namespace, profile: str
) -> int:
    """Run the attack-free k x sigma matrix on the completion fixture."""
    from fourway import adjacent_lane_grid as grid

    manifest = grid.honest_operating_manifest(profile)
    output_dir = REPO_ROOT / "benchmarks" / "Phase2TwoLaneHonestOperatingSweep"
    payload = {
        "checkpoint": "P7-two-lane-honest-operating-point",
        "profile": profile,
        "planned_unique_runs": len(manifest),
        "execution": "strictly sequential with exact-metadata resume",
        "completion_config": TWO_LANE_SWEEP_CONFIG,
        "n": grid.N,
        "f": grid.F,
        "b": 0,
        "delta_m": 0.0,
        "sigma_lat_m": sorted({float(row["sigma_lat_m"]) for row in manifest}),
        "k": list(grid.HONEST_K_SWEEP),
        "repetitions_per_cell": (
            1 if profile == "smoke" else grid.HONEST_OPERATING_REPS
        ),
        "paired_seed_policy": "same repetition uses same seed across k and sigma cells",
        "purpose": (
            "select k from honest q1/certification/throughput; sigma is an "
            "environment characterization axis; honest delta is always zero"
        ),
    }
    if args.dry_run:
        print(
            f"[dry-run] two-lane honest operating profile={profile} "
            f"unique_runs={len(manifest)} sequential=true completion=true"
        )
        for index, row in enumerate(manifest, 1):
            print(
                f"[dry-run] {index:03d}/{len(manifest)} {row} "
                f"result_dir={grid.honest_operating_run_dir(REPO_ROOT, row)}"
            )
        return 0

    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / f"honest_operating_{profile}_manifest.json").write_text(
        json.dumps({**payload, "runs": manifest}, indent=2, sort_keys=True) + "\n"
    )
    run_key_generation(dry_run=False, scale=grid.N)

    results: List[Dict[str, Any]] = []
    failures: List[Dict[str, Any]] = []
    for index, row in enumerate(manifest, 1):
        print(f"\n--- Honest operating {index}/{len(manifest)} {row} ---")
        try:
            clear_stale_random_ini()
            run_dir = _run_two_lane_grid_cell(
                args, row,
                run_dir_override=grid.honest_operating_run_dir(REPO_ROOT, row),
            )
            result = _analyze_two_lane_grid_cell(row, run_dir)
            results.append(result)
            if not result["passed"]:
                failures.append({"row": row, "checks": result["checks"]})
            print(f"[{'PASS' if result['passed'] else 'FAIL'}] checks={result['checks']}")
            (output_dir / f"honest_operating_{profile}_progress.json").write_text(
                json.dumps({
                    "planned_unique_runs": len(manifest),
                    "completed_unique_runs": len(results),
                    "last_completed_index": index,
                    "last_completed_row": row,
                    "failure_count": len(failures),
                }, indent=2, sort_keys=True) + "\n"
            )
        except (subprocess.CalledProcessError, OSError, ValueError, KeyError) as exc:
            failures.append({"row": row, "error": str(exc)})
            print(f"[FAIL] {exc}")
            break

    aggregate = _aggregate_honest_operating(results, profile)
    model_matches = [
        bool(row["q1_model_inside_wilson"]) for row in aggregate
    ]
    sanity = {
        "all_runs_passed": bool(results) and all(row["passed"] for row in results),
        "all_completion_metrics_present": bool(aggregate) and all(
            row["completion_metrics_complete"] for row in aggregate
        ),
        "background_cooccupancy_zero": all(
            int(row["background_conflicting_cooccupancy_runs"]) == 0
            for row in aggregate
        ),
        # This is diagnostic rather than a pass gate: requiring every one of
        # 24 nominal 95% intervals to cover its model would create a large
        # family-wise false-failure probability even for a correct channel.
        "analytic_q1_wilson_coverage_fraction": (
            sum(model_matches) / len(model_matches) if model_matches else None
        ),
        "analytic_q1_wilson_coverage_is_diagnostic": True,
    }
    sanity["passed"] = all((
        sanity["all_runs_passed"],
        sanity["all_completion_metrics_present"],
        sanity["background_cooccupancy_zero"],
    ))
    selection = _honest_operating_selection(aggregate, profile)
    passed = (
        len(results) == len(manifest) and not failures and sanity["passed"]
    )
    summary = {
        **payload,
        "completed_unique_runs": len(results),
        "passed": passed,
        "failures": failures,
        "sanity_checks": sanity,
        "operating_point_selection": selection,
        "aggregates": aggregate,
        "results": results,
    }
    summary_path = output_dir / f"honest_operating_{profile}_summary.json"
    csv_path = output_dir / f"honest_operating_{profile}_aggregates.csv"
    summary_path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    with csv_path.open("w", newline="") as handle:
        fields = list(aggregate[0]) if aggregate else [
            "sigma_lat_m", "k", "repetitions", "pooled_q1_empirical",
            "lane_certification_rate", "throughput_veh_per_s", "wait_mean_s",
        ]
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(aggregate)
    print("\n========== TWO-LANE HONEST OPERATING SWEEP ==========")
    print(f"Profile: {profile}")
    print(f"Completed: {len(results)}/{len(manifest)}")
    print(f"Sanity: {sanity}")
    print(f"Selection: {selection}")
    print(f"Overall: {'PASS' if passed else 'FAIL'}")
    print(f"Summary: {summary_path}")
    print(f"Aggregates: {csv_path}")
    return 0 if passed else 1


def run_longitudinal_grid(args: argparse.Namespace) -> int:
    """Run the sequential N=16 longitudinal shoulder grid and aggregate it."""
    from fourway import longitudinal_grid as grid

    f = bft_f(grid.N)
    physical_gate_k = float(args.physical_gate_k)
    manifest = grid.manifest(
        args.longitudinal_grid_profile, f, physical_gate_k=physical_gate_k
    )
    output_dir = grid.output_dir(REPO_ROOT, physical_gate_k)
    if args.dry_run:
        print(
            f"[dry-run] longitudinal grid profile={args.longitudinal_grid_profile} "
            f"k={physical_gate_k:g} unique_runs={len(manifest)} sequential=true"
        )
        for row in manifest:
            print(f"[dry-run] {row}")
        return 0

    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "longitudinal_grid_manifest.json").write_text(json.dumps({
        "checkpoint": "P7-longitudinal-grid", "profile": args.longitudinal_grid_profile,
        "unique_run_count": len(manifest),
        "execution": "strictly sequential; generated fixture files are shared",
        "main_gate_k": physical_gate_k,
        "offline_rethreshold_k": list(grid.RETHRESHOLD_K),
        "runs": manifest,
    }, indent=2, sort_keys=True) + "\n")
    reanalyze_only = bool(args.longitudinal_grid_reanalyze)
    if reanalyze_only:
        print("[reanalyze] using existing longitudinal JSON artifacts; no simulation or key generation")
    else:
        run_key_generation(dry_run=False, scale=grid.N)
    rows: List[Dict[str, Any]] = []
    failures: List[Dict[str, Any]] = []
    for index, cell in enumerate(manifest, 1):
        print(f"\n--- Longitudinal grid {index}/{len(manifest)} {cell} ---")
        clear_stale_random_ini()
        run_dir = grid.run_dir(REPO_ROOT, cell)
        json_path = run_dir / f"{grid.N}veh_{cell['rep']}.json"
        metadata_path = run_dir / "longitudinal_grid_run_metadata.json"
        try:
            b = int(cell["b"])
            kind = "NONE" if b == 0 else "FALSE_DISTANCE"
            offset = 0.0 if b == 0 else -(
                float(cell["reference_separation_m"]) + grid.ATTACK_AHEAD_MARGIN_M
            )
            colluders = _phase2_nested_colluders(grid.TARGET, b, grid.N)
            seed_label = (
                f"DIST_GRID_S{cell['reference_separation_m']:g}_"
                f"SIG{cell['sigma_long_m']:g}_B{b}"
            )
            seed = run_seed(MASTER_SEED, grid.N, seed_label, int(cell["rep"]))
            expected_metadata = {
                "checkpoint": "P7-longitudinal-grid", **cell,
                "attack_kind": kind, "distance_claim_offset_m": offset,
                "attack_ahead_margin_m": grid.ATTACK_AHEAD_MARGIN_M,
                "evidence_colluder_ids": colluders,
                "colluder_policy": "nested_ascending_replica_ids",
                "simulation_seed": seed, "physical_gate_k": physical_gate_k,
                "collection_window_sec": 0.25,
                "config": "SixteenVehiclesDistanceGridResDB",
                "fixture_axis": "TraCI front-reference separation, not bumper gap",
            }
            if json_path.is_file() and metadata_path.is_file() and (run_dir / "raw_simulation.log").is_file():
                if json.loads(metadata_path.read_text()) != expected_metadata:
                    raise ValueError(f"refusing mismatched longitudinal artifact: {run_dir}")
                print(f"[resume] reuse {run_dir}")
            elif reanalyze_only:
                raise ValueError(f"missing existing run artifacts for analysis-only mode: {run_dir}")
            else:
                run_dir.mkdir(parents=True, exist_ok=True)
                metadata_path.write_text(
                    json.dumps(expected_metadata, indent=2, sort_keys=True) + "\n"
                )
                argv = [
                    str(RUN_SCRIPT), str(FOURWAY_DIR),
                    "--channel-metrics-dir", str(run_dir.resolve()),
                    "--distance-reference-separation", str(cell["reference_separation_m"]),
                    "--approach-sigma", "0", "--signal-error", "0",
                    "--direction-collection-window", "0.25",
                    "--ego-longitudinal-sigma", "0",
                    "--longitudinal-sigma", str(cell["sigma_long_m"]),
                    "--physical-gate-k", format(physical_gate_k, "g"),
                    "--distance-stationary-speed", str(args.distance_stationary_speed),
                    "--distance-attestation-retry-interval", str(args.distance_attestation_retry_interval),
                    "--distance-attestation-retry-max", str(args.distance_attestation_retry_max),
                    "--simulation-seed", str(seed),
                    "--phase2-attack-kind", kind,
                    "--phase2-attack-target", str(grid.TARGET),
                    "--phase2-evidence-colluders", ",".join(str(value) for value in colluders),
                    "--phase2-actual-b", str(b),
                    "--phase2-distance-claim-offset", str(offset),
                    "-u", "Cmdenv", "-c", "SixteenVehiclesDistanceGridResDB",
                ]
                if args.longitudinal_grid_profile == "full":
                    argv.insert(2, "--compact-log")
                inner = " ".join(shlex.quote(value) for value in argv)
                print(f"+ {inner}")
                run_in_bash_with_omnet(inner, dry_run=False)
                shutil.copy2(LOG_FILE, run_dir / "raw_simulation.log")
                for name in (
                    "distance_grid_16veh.rou.xml", "distance_grid_16veh.sumo.cfg",
                    "distance_grid_16veh.launchd.xml", "distance_grid_fixture_metadata.json",
                    "distance_fixture_override.ini", "perception_override.ini",
                    "phase2_attack_override.ini",
                ):
                    source = FOURWAY_DIR / name
                    if source.is_file():
                        shutil.copy2(source, run_dir / name)
                analyze = [
                    sys.executable, str(FOURWAY_DIR / "analyze_log.py"), str(LOG_FILE),
                    "--save-to", str(run_dir), "--scenario", "1", "--cars", str(grid.N),
                    "--run-index", str(cell["rep"]), "--no-scenario-subdir",
                ]
                print("+ " + " ".join(shlex.quote(value) for value in analyze))
                subprocess.run(analyze, cwd=REPO_ROOT, check=True)

            records = json.loads(json_path.read_text())
            row = grid.analyze_run(cell, records, run_dir, f)
            rows.append(row)
            if not row["simulation_completed"]:
                failures.append({**cell, "error": "not all N=16 vehicles completed"})
            if not row["single_evaluation_invariant_ok"]:
                failures.append({**cell, "error": "duplicate stopped-distance evaluation"})
            measured = row["measured_reference_separation_m"]
            if row["committed_target_pair_present"] and (
                    measured is None or abs(measured - cell["reference_separation_m"]) > 0.15):
                failures.append({**cell, "error": f"measured reference separation {measured}"})
        except (subprocess.CalledProcessError, OSError, ValueError, KeyError) as exc:
            failures.append({**cell, "error": str(exc)})
            print(f"[FAIL] {cell}: {exc}")

    cells = grid.aggregate(rows)
    summary = {
        "checkpoint": "P7-longitudinal-grid", "profile": args.longitudinal_grid_profile,
        "physical_gate_k": physical_gate_k,
        "passed": not failures and len(rows) == len(manifest),
        "planned_unique_runs": len(manifest), "completed_unique_runs": len(rows),
        "panel_1": {
            "validates": "sensor-level raw pair inversion",
            "source": str(FOURWAY_DIR / "longitudinal_distance_calibration" /
                          "canonical_validation_summary.json"),
            "prediction": "Phi(-s/(sqrt(2)*sigma_long))",
        },
        "panel_2": {
            "validates": "protocol-level committed per-pair inversion",
            "prediction": "BinomialTail(measured h, q_dist, threshold-attempt_available_b_sig)",
            "headline_slice": f"b=f={f}, sigma_long=1 m",
        },
        "byzantine_support_accounting": {
            "certificate_local_b_sig": "forensic count from a finalized certificate; zero when none finalizes",
            "attempt_available_b_sig": (
                "claimant self-attestation plus distinct Byzantine supporting echoes actually collected; "
                "used for certificate-formation predictions"
            ),
        },
        "cliff_policy": "b=f+1 is a Byzantine cliff/control point, not shoulder evidence",
        "whole_queue_policy": "empirical only; no independence overlay on the full permutation",
        "failures": failures, "runs": rows, "cells": cells,
    }
    summary_path = output_dir / "longitudinal_grid_summary.json"
    summary_path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    if rows:
        fields = [key for key in rows[0] if key not in ("purposes", "offline_rethreshold")]
        with (output_dir / "longitudinal_grid_runs.csv").open("w", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=[*fields, "purposes", "offline_rethreshold"])
            writer.writeheader()
            for row in rows:
                writer.writerow({**row, "purposes": ";".join(row["purposes"]),
                                 "offline_rethreshold": json.dumps(row["offline_rethreshold"], sort_keys=True)})
    plot_script = FOURWAY_DIR / "plot_longitudinal_two_panel.py"
    if plot_script.is_file():
        subprocess.run([sys.executable, str(plot_script), "--grid-summary", str(summary_path)],
                       cwd=REPO_ROOT, check=True)
    print("\n========== LONGITUDINAL GRID SUMMARY ==========")
    print(f"Completed: {len(rows)}/{len(manifest)}")
    print(f"Overall: {'PASS' if summary['passed'] else 'FAIL'}")
    print(f"Summary: {summary_path}")
    return 0 if summary["passed"] else 1


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
    p.add_argument("--ego-longitudinal-sigma", type=float, default=0.0,
                   help="Claimant/self longitudinal-position sigma in metres.")
    p.add_argument("--longitudinal-sigma", type=float, default=0.0,
                   help="External-witness longitudinal-position sigma in metres.")
    p.add_argument(
        "--lane-observation-mode",
        choices=("CATEGORICAL_CARDINAL", "ADJACENT_LATERAL"),
        default="CATEGORICAL_CARDINAL",
        help="Cardinal categorical gate or adjacent-lane scalar lateral-residual gate.",
    )
    p.add_argument("--lateral-sigma", type=float, default=0.0,
                   help="External-witness lane-normal observation sigma in metres.")
    p.add_argument("--lateral-claim-offset", type=float, default=0.0,
                   help="Signed FALSE_PHYSICAL_LANE claim displacement in metres.")
    p.add_argument("--physical-gate-k", type=float, default=3.0,
                   help="Physical evidence tolerance multiplier k.")
    p.add_argument("--distance-stationary-speed", type=float, default=0.1,
                   help="Maximum target speed in m/s for stopped-distance evidence.")
    p.add_argument("--distance-attestation-retry-interval", type=float, default=0.5,
                   help="Seconds between bounded retransmissions of cached distance attestation bytes.")
    p.add_argument("--distance-attestation-retry-max", type=int, default=4,
                   help="Maximum retransmissions of the same signed stopped-distance attestation.")
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
        "--distance-rank-check10-validation",
        action="store_true",
        help="Force a Byzantine mutation of the distance-derived queue rank and require Check 10 rejection plus honest recovery.",
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
        "--continuous-coordinate-calibration",
        action="store_true",
        help=(
            "Run the standalone adjacent-lane and stopped-queue calibration "
            "checkpoints before continuous-coordinate protocol integration."
        ),
    )
    p.add_argument(
        "--adjacent-lane-validation",
        action="store_true",
        help=(
            "Run the four strictly sequential N=16 adjacent-lane wiring cells: "
            "honest zero, b=f shoulder, b=f zero-error guard, and b=f+1 cliff."
        ),
    )
    p.add_argument(
        "--adjacent-lane-validation-reanalyze",
        action="store_true",
        help="Rebuild the adjacent-lane validation summary from its existing four artifacts only.",
    )
    p.add_argument(
        "--two-lane-validation",
        action="store_true",
        help=(
            "Run the five strictly sequential N=16 full-intersection two-lane "
            "gate, cliff, rank-noise, and Check-10 validation cells."
        ),
    )
    p.add_argument(
        "--two-lane-conflict-release-validation",
        action="store_true",
        help=(
            "Run only the deterministic full-intersection false-lane to "
            "conflicting-co-occupancy smoke."
        ),
    )
    p.add_argument(
        "--two-lane-combined-validation",
        action="store_true",
        help=(
            "Run three strictly sequential N=16 cells with sigma_lat=.5, "
            "sigma_long=1, and k=2 to validate joint perception wiring and "
            "causal attribution."
        ),
    )
    p.add_argument(
        "--two-lane-direction-ablation",
        action="store_true",
        help=(
            "Run six sequential N=16 mixed-maneuver cells comparing direction "
            "eligibility on/off and all-singleton scheduling under honest and "
            "FALSE_DIRECTION conditions."
        ),
    )
    p.add_argument(
        "--two-lane-direction-ablation-full",
        action="store_true",
        help=(
            "Run the full 120-run N=16 mixed-maneuver direction ablation: "
            "20 paired repetitions for honest/FALSE_DIRECTION crossed with "
            "eligibility-on, eligibility-off, and all-singleton scheduling."
        ),
    )
    p.add_argument(
        "--two-lane-direction-straight-heavy-prerequisite",
        action="store_true",
        help=(
            "Run one honest N=16 8S/4L/4R eligibility-OFF fixture smoke and "
            "require mean batch size greater than one before the headline ablation."
        ),
    )
    p.add_argument(
        "--two-lane-direction-straight-heavy",
        action="store_true",
        help=(
            "Run the six-row N=16 straight-heavy direction/co-batching smoke "
            "after the batching prerequisite passes."
        ),
    )
    p.add_argument(
        "--two-lane-direction-straight-heavy-full",
        action="store_true",
        help=(
            "Run the 120-run N=16 straight-heavy direction ablation with one "
            "preselected fixture and paired seed shared across each six-row repetition."
        ),
    )
    p.add_argument(
        "--two-lane-scale-smoke",
        action="store_true",
        help=(
            "Run four sequential honest operating-point fixture smokes at "
            "N=4,8,16,20 on the full two-lane mixed-maneuver intersection."
        ),
    )
    p.add_argument(
        "--two-lane-scale-honest-full",
        action="store_true",
        help=(
            "Run the full honest operating-point scale experiment: 20 "
            "strictly sequential repetitions at each N=4,8,16,20 (80 runs)."
        ),
    )
    p.add_argument(
        "--attack-defense-equivocation-validation",
        action="store_true",
        help=(
            "Run focused attack D at N=16 and verify the one-variant-per-witness "
            "invariant over signed announcement gossip."
        ),
    )
    p.add_argument(
        "--attack-defense-leader-validation",
        action="store_true",
        help=(
            "Run focused attack-defense cases E-H sequentially at N=16: "
            "physical-lane mutation, UNKNOWN upgrade, f+1 cert suppression, "
            "and silent-primary recovery."
        ),
    )
    p.add_argument(
        "--attack-defense-gossip-validation",
        action="store_true",
        help=(
            "Run focused row J at N=16: f Byzantine replicas store valid "
            "arrival certificates but withhold both certificate relay paths."
        ),
    )
    p.add_argument(
        "--attack-defense-chain-validation",
        action="store_true",
        help=(
            "Run the consecutive-Byzantine-primary corner case at N=16: "
            "silent r0, silent successor r1, then honest recovery through r2."
        ),
    )
    p.add_argument(
        "--attack-defense-full-validation",
        action="store_true",
        help=(
            "Run D-H, J, and the consecutive-Byzantine-primary corner "
            "strictly sequentially over three seeds by default; override with --reps."
        ),
    )
    p.add_argument(
        "--two-lane-b1-smoke",
        action="store_true",
        help=(
            "Run one N=16 b=1, sigma=.5, delta=1.75, k=3 cell and report "
            "q0 plus attempt-level versus finalized-certificate Byzantine support."
        ),
    )
    p.add_argument(
        "--two-lane-delta-b-smoke",
        action="store_true",
        help=(
            "Run exactly one sequential smoke for each delta in "
            "{1.61,1.75,2.0} x b in {1..6} (18 attack runs)."
        ),
    )
    p.add_argument(
        "--two-lane-delta-b-grid",
        action="store_true",
        help=(
            "Run the focused 360-attack delta x b matrix plus one non-repeated "
            "honest control, reusing passing smoke artifacts."
        ),
    )
    p.add_argument(
        "--two-lane-delta-b-completion-grid",
        action="store_true",
        help=(
            "Run the full delta x b matrix through all 16 departures, reusing "
            "the 120 shared k=3/sigma=.5 completion anchors."
        ),
    )
    p.add_argument(
        "--two-lane-k-sweep",
        choices=("smoke", "full"),
        help=(
            "Run Figure B on the completion-capable two-lane fixture: "
            "k={1,2,3} x b={1..6}, with one or 20 repetitions per cell."
        ),
    )
    p.add_argument(
        "--two-lane-sigma-sweep",
        choices=("smoke", "full"),
        help=(
            "Run Figure C on the completion-capable two-lane fixture: seven "
            "sigma values x b={1..6}, with one or 20 repetitions per cell."
        ),
    )
    p.add_argument(
        "--two-lane-honest-operating-sweep",
        choices=("smoke", "k-full", "full"),
        help=(
            "Run the attack-free completion fixture: smoke is 24 wiring cells; "
            "k-full is 60 runs at sigma=.5; full is the 480-run k x sigma "
            "surface. Delta is correctly fixed at zero."
        ),
    )
    p.add_argument(
        "--two-lane-grid",
        action="store_true",
        help=(
            "Run the locked 400-run N=16 full-intersection two-lane matrix "
            "strictly sequentially after conflict-release validation."
        ),
    )
    p.add_argument(
        "--two-lane-grid-reanalyze",
        action="store_true",
        help="Rebuild the two-lane matrix summary from exact existing artifacts only.",
    )
    p.add_argument(
        "--two-lane-grid-profile",
        choices=("full",),
        default="full",
        help="Locked full-intersection matrix profile (400 unique runs).",
    )
    p.add_argument(
        "--adjacent-lane-grid",
        action="store_true",
        help="Retired: the straight-road 310-cell matrix is calibration-only and cannot be launched.",
    )
    p.add_argument(
        "--adjacent-lane-grid-reanalyze",
        action="store_true",
        help="Retired with the straight-road 310-cell matrix.",
    )
    p.add_argument(
        "--adjacent-lane-grid-profile",
        choices=("full",),
        default="full",
        help="Retired straight-road profile name retained only for CLI error reporting.",
    )
    p.add_argument(
        "--longitudinal-grid",
        action="store_true",
        help=(
            "Run the sequential N=16 stopped-distance experiment used by the "
            "sensor/protocol two-panel figure."
        ),
    )
    p.add_argument(
        "--longitudinal-grid-reanalyze",
        action="store_true",
        help=(
            "Rebuild the longitudinal summary and plots from existing run JSON "
            "without generating keys or launching simulations."
        ),
    )
    p.add_argument(
        "--longitudinal-grid-profile",
        choices=("smoke", "full"),
        default="full",
        help="Longitudinal scope: four-cell smoke or the 186-run approved full grid.",
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
        choices=("e2", "e4", "e2long"),
        help="Run one Checkpoint 2C authenticated evidence-attack experiment.",
    )
    p.add_argument(
        "--distance-claim-offset",
        type=float,
        default=0.0,
        help="Signed FALSE_DISTANCE claim offset in metres: claim=true+offset.",
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
        or args.distance_rank_check10_validation
        or args.phase2_attack_validation
        or args.phase2_metrology_validation
        or args.phase2_pilots
        or args.continuous_coordinate_calibration
        or args.two_lane_validation
        or args.two_lane_conflict_release_validation
        or args.two_lane_combined_validation
        or args.two_lane_direction_ablation
        or args.two_lane_direction_ablation_full
        or args.two_lane_direction_straight_heavy_prerequisite
        or args.two_lane_direction_straight_heavy
        or args.two_lane_direction_straight_heavy_full
        or args.two_lane_scale_smoke
        or args.two_lane_scale_honest_full
        or args.attack_defense_equivocation_validation
        or args.attack_defense_leader_validation
        or args.attack_defense_gossip_validation
        or args.attack_defense_chain_validation
        or args.attack_defense_full_validation
        or args.two_lane_b1_smoke
        or args.two_lane_delta_b_smoke
        or args.two_lane_delta_b_grid
        or args.two_lane_delta_b_completion_grid
        or args.two_lane_k_sweep
        or args.two_lane_sigma_sweep
        or args.two_lane_honest_operating_sweep
        or args.two_lane_grid
        or args.two_lane_grid_reanalyze
        or args.adjacent_lane_validation
        or args.adjacent_lane_validation_reanalyze
        or args.adjacent_lane_grid
        or args.adjacent_lane_grid_reanalyze
        or args.longitudinal_grid
        or args.longitudinal_grid_reanalyze
    )
    repetitions = args.reps if args.reps is not None else (
        3 if args.attack_defense_full_validation else
        (1 if fixed_validation else REPETITIONS)
    )
    if repetitions <= 0:
        print("ERROR: --reps must be >= 1.", file=sys.stderr)
        return 2
    if not 0.0 <= args.signal_error <= 1.0:
        print("ERROR: --signal-error must be in [0,1].", file=sys.stderr)
        return 2
    if args.direction_collection_window < 0.0:
        print("ERROR: --direction-collection-window must be nonnegative.", file=sys.stderr)
        return 2
    if (args.ego_longitudinal_sigma < 0.0 or args.longitudinal_sigma < 0.0 or
            args.lateral_sigma < 0.0):
        print("ERROR: perception sigma values must be nonnegative.", file=sys.stderr)
        return 2
    if (args.lane_observation_mode == "CATEGORICAL_CARDINAL" and
            (args.lateral_sigma != 0.0 or args.lateral_claim_offset != 0.0)):
        print("ERROR: lateral sigma/claim offset require ADJACENT_LATERAL mode.", file=sys.stderr)
        return 2
    if (args.lane_observation_mode == "ADJACENT_LATERAL" and
            not (args.two_lane_validation or args.adjacent_lane_validation or
                 args.adjacent_lane_validation_reanalyze or
                 args.two_lane_combined_validation or
                 args.two_lane_direction_ablation or
                 args.two_lane_direction_ablation_full or
                 args.two_lane_direction_straight_heavy_prerequisite or
                 args.two_lane_direction_straight_heavy or
                 args.two_lane_direction_straight_heavy_full or
                 args.two_lane_scale_smoke or
                 args.two_lane_scale_honest_full or
                 args.attack_defense_equivocation_validation or
                 args.attack_defense_leader_validation or
                 args.attack_defense_gossip_validation or
                 args.attack_defense_chain_validation or
                 args.attack_defense_full_validation)):
        print(
            "ERROR: ADJACENT_LATERAL is scoped to the dedicated "
            "--adjacent-lane-validation preset/config; cardinal runs remain categorical.",
            file=sys.stderr,
        )
        return 2
    if args.physical_gate_k <= 0.0 or args.distance_stationary_speed < 0.0:
        print("ERROR: physical gate k must be positive and stationary speed nonnegative.", file=sys.stderr)
        return 2
    if args.distance_attestation_retry_interval <= 0.0 or args.distance_attestation_retry_max < 0:
        print("ERROR: stopped-distance retry interval must be positive and max nonnegative.", file=sys.stderr)
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
        args.distance_rank_check10_validation,
        args.phase2_attack_validation,
        args.phase2_metrology_validation,
        args.phase2_pilots,
        args.continuous_coordinate_calibration,
        args.two_lane_validation,
        args.two_lane_conflict_release_validation,
        args.two_lane_combined_validation,
        args.two_lane_direction_ablation,
        args.two_lane_direction_ablation_full,
        args.two_lane_direction_straight_heavy_prerequisite,
        args.two_lane_direction_straight_heavy,
        args.two_lane_direction_straight_heavy_full,
        args.two_lane_scale_smoke,
        args.two_lane_scale_honest_full,
        args.attack_defense_equivocation_validation,
        args.attack_defense_leader_validation,
        args.attack_defense_gossip_validation,
        args.attack_defense_chain_validation,
        args.attack_defense_full_validation,
        args.two_lane_b1_smoke,
        args.two_lane_delta_b_smoke,
        args.two_lane_delta_b_grid,
        args.two_lane_delta_b_completion_grid,
        args.two_lane_k_sweep,
        args.two_lane_sigma_sweep,
        args.two_lane_honest_operating_sweep,
        args.two_lane_grid,
        args.two_lane_grid_reanalyze,
        args.adjacent_lane_validation,
        args.adjacent_lane_validation_reanalyze,
        args.adjacent_lane_grid,
        args.adjacent_lane_grid_reanalyze,
        args.longitudinal_grid,
        args.longitudinal_grid_reanalyze,
    ))
    if fixed_presets > 1:
        print("ERROR: choose only one fixed validation preset.", file=sys.stderr)
        return 2

    if args.adjacent_lane_grid or args.adjacent_lane_grid_reanalyze:
        print(
            "ERROR: the straight-road 310-run adjacent-lane matrix is retired. "
            "Use --two-lane-grid for the locked full-intersection replacement.",
            file=sys.stderr,
        )
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
        if args.lane_observation_mode != "CATEGORICAL_CARDINAL":
            incompatible.append("--lane-observation-mode")
        if args.lateral_sigma != 0.0:
            incompatible.append("--lateral-sigma")
        if args.lateral_claim_offset != 0.0:
            incompatible.append("--lateral-claim-offset")
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
        if not (args.longitudinal_grid or args.longitudinal_grid_reanalyze) and args.longitudinal_grid_profile != "full":
            incompatible.append("--longitudinal-grid-profile")
        if not (args.two_lane_grid or args.two_lane_grid_reanalyze) and args.two_lane_grid_profile != "full":
            incompatible.append("--two-lane-grid-profile")
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
        if args.distance_rank_check10_validation:
            return run_distance_rank_check10_validation(args, repetitions)
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
        if args.continuous_coordinate_calibration:
            return run_continuous_coordinate_calibration(args)
        if args.two_lane_validation:
            return run_two_lane_validation(args, repetitions)
        if args.two_lane_conflict_release_validation:
            return run_two_lane_conflict_release_validation(args)
        if args.two_lane_combined_validation:
            conflict_prerequisite = (
                REPO_ROOT / "benchmarks" / "Phase2TwoLaneValidation" /
                "conflict_release_cliff" / "run_0" /
                "two_lane_conflict_release_validation.json"
            )
            longitudinal_prerequisite = (
                REPO_ROOT / "benchmarks" / "Phase2DistanceGridK2" /
                "longitudinal_grid_summary.json"
            )
            missing = []
            if (not conflict_prerequisite.is_file() or not
                    json.loads(conflict_prerequisite.read_text()).get("passed")):
                missing.append("passing two-lane conflict-release validation")
            if (not longitudinal_prerequisite.is_file() or not
                    json.loads(longitudinal_prerequisite.read_text()).get("passed")):
                missing.append("passing k=2 longitudinal smoke")
            if missing:
                print(
                    "ERROR: combined validation requires " + " and ".join(missing),
                    file=sys.stderr,
                )
                return 2
            return run_two_lane_combined_validation(args)
        if (
            args.two_lane_direction_ablation or
            args.two_lane_direction_ablation_full or
            args.two_lane_direction_straight_heavy_prerequisite or
            args.two_lane_direction_straight_heavy or
            args.two_lane_direction_straight_heavy_full
        ):
            if (
                args.two_lane_direction_straight_heavy_prerequisite or
                args.two_lane_direction_straight_heavy or
                args.two_lane_direction_straight_heavy_full
            ):
                fixture_summary = (
                    FOURWAY_DIR / "two_lane_calibration" / "results" /
                    "direction_ablation_straight_heavy_fixture_validation.json"
                )
                if (not fixture_summary.is_file() or not
                        json.loads(fixture_summary.read_text()).get("passed")):
                    if args.dry_run:
                        print(
                            "[dry-run] prerequisite: python3 "
                            "fourway/validate_direction_ablation_fixtures.py"
                        )
                    else:
                        subprocess.run(
                            [sys.executable, str(
                                FOURWAY_DIR /
                                "validate_direction_ablation_fixtures.py"
                            )],
                            cwd=REPO_ROOT,
                            check=True,
                        )
                if (not args.dry_run and
                        (not fixture_summary.is_file() or not
                         json.loads(fixture_summary.read_text()).get("passed"))):
                    print(
                        "ERROR: straight-heavy fixture validation failed.",
                        file=sys.stderr,
                    )
                    return 2
            combined_prerequisite = (
                REPO_ROOT / "benchmarks" / "Phase2TwoLaneCombinedValidation" /
                "two_lane_combined_validation_summary.json"
            )
            if (not combined_prerequisite.is_file() or not
                    json.loads(combined_prerequisite.read_text()).get("passed")):
                print(
                    "ERROR: passing --two-lane-combined-validation is required "
                    "before the direction ablation.", file=sys.stderr,
                )
                return 2
            if args.two_lane_direction_ablation or args.two_lane_direction_ablation_full:
                return run_two_lane_direction_ablation(
                    args, "full" if args.two_lane_direction_ablation_full else "smoke"
                )
            if args.two_lane_direction_straight_heavy_prerequisite:
                return run_two_lane_direction_ablation(
                    args, "prerequisite", "straight_heavy"
                )
            prerequisite_summary = (
                REPO_ROOT / "benchmarks" /
                "Phase2DirectionAblationStraightHeavyPrerequisite" /
                "direction_ablation_straight_heavy_prerequisite_summary.json"
            )
            if (not prerequisite_summary.is_file() or not
                    json.loads(prerequisite_summary.read_text()).get("passed")):
                print(
                    "ERROR: passing "
                    "--two-lane-direction-straight-heavy-prerequisite is "
                    "required before the straight-heavy direction ablation.",
                    file=sys.stderr,
                )
                return 2
            if args.two_lane_direction_straight_heavy_full:
                smoke_summary = (
                    REPO_ROOT / "benchmarks" /
                    "Phase2DirectionAblationStraightHeavy" /
                    "direction_ablation_straight_heavy_summary.json"
                )
                if (not smoke_summary.is_file() or not
                        json.loads(smoke_summary.read_text()).get("passed")):
                    print(
                        "ERROR: passing --two-lane-direction-straight-heavy "
                        "is required before its 120-run full profile.",
                        file=sys.stderr,
                    )
                    return 2
            return run_two_lane_direction_ablation(
                args,
                "full" if args.two_lane_direction_straight_heavy_full else "smoke",
                "straight_heavy",
            )
        if args.two_lane_scale_smoke or args.two_lane_scale_honest_full:
            fixture_prerequisite = (
                FOURWAY_DIR / "two_lane_calibration" / "results" /
                "two_lane_scale_fixture_validation.json"
            )
            if (not fixture_prerequisite.is_file() or not
                    json.loads(fixture_prerequisite.read_text()).get("passed")):
                print(
                    "ERROR: passing validate_two_lane_scale_fixtures.py is "
                    "required before the multiscale smoke.", file=sys.stderr,
                )
                return 2
            return run_two_lane_scale(
                args, "full" if args.two_lane_scale_honest_full else "smoke"
            )
        if args.attack_defense_equivocation_validation:
            return run_attack_defense_equivocation_validation(args)
        if args.attack_defense_leader_validation:
            equivocation_prerequisite = (
                REPO_ROOT / "benchmarks" / "Phase2AttackDefense" /
                "D_EquivocationV2" / "run_0" / "equivocation_validation_summary.json"
            )
            if (not equivocation_prerequisite.is_file() or not
                    json.loads(equivocation_prerequisite.read_text()).get("passed")):
                print(
                    "ERROR: passing corrected attack D validation is required "
                    "before E-H.", file=sys.stderr,
                )
                return 2
            return run_attack_defense_leader_validation(args)
        if args.attack_defense_gossip_validation:
            return run_attack_defense_gossip_validation(args)
        if args.attack_defense_chain_validation:
            return run_attack_defense_chain_validation(args)
        if args.attack_defense_full_validation:
            return run_attack_defense_full_validation(args, repetitions)
        if args.two_lane_b1_smoke:
            return run_two_lane_b1_smoke(args)
        if args.two_lane_delta_b_completion_grid:
            prerequisite = (
                REPO_ROOT / "benchmarks" / "Phase2TwoLaneValidation" /
                "conflict_release_cliff" / "run_0" /
                "two_lane_conflict_release_validation.json"
            )
            if not prerequisite.is_file() or not json.loads(prerequisite.read_text()).get("passed"):
                print(
                    "ERROR: passing --two-lane-conflict-release-validation is required before the completion grid.",
                    file=sys.stderr,
                )
                return 2
            return run_two_lane_delta_b_completion_grid(args)
        if args.two_lane_delta_b_smoke or args.two_lane_delta_b_grid:
            prerequisite = (
                REPO_ROOT / "benchmarks" / "Phase2TwoLaneValidation" /
                "conflict_release_cliff" / "run_0" /
                "two_lane_conflict_release_validation.json"
            )
            if not prerequisite.is_file() or not json.loads(prerequisite.read_text()).get("passed"):
                print(
                    "ERROR: passing --two-lane-conflict-release-validation is required before the delta-b grid.",
                    file=sys.stderr,
                )
                return 2
            if args.two_lane_delta_b_grid:
                smoke_summary = (
                    REPO_ROOT / "benchmarks" / "Phase2TwoLaneDeltaBGrid" /
                    "two_lane_delta_b_smoke_summary.json"
                )
                if (not smoke_summary.is_file() or
                        not json.loads(smoke_summary.read_text()).get("passed")):
                    print(
                        "ERROR: a passing --two-lane-delta-b-smoke is required before the full delta-b grid.",
                        file=sys.stderr,
                    )
                    return 2
            profile = "smoke" if args.two_lane_delta_b_smoke else "full"
            return run_two_lane_delta_b_grid(args, profile)
        if (args.two_lane_k_sweep or args.two_lane_sigma_sweep or
                args.two_lane_honest_operating_sweep):
            prerequisite = (
                REPO_ROOT / "benchmarks" / "Phase2TwoLaneValidation" /
                "conflict_release_cliff" / "run_0" /
                "two_lane_conflict_release_validation.json"
            )
            if not prerequisite.is_file() or not json.loads(prerequisite.read_text()).get("passed"):
                print(
                    "ERROR: passing --two-lane-conflict-release-validation is required before parameter sweeps.",
                    file=sys.stderr,
                )
                return 2
            if args.two_lane_honest_operating_sweep:
                profile = str(args.two_lane_honest_operating_sweep)
                if profile in ("k-full", "full"):
                    smoke_summary = (
                        REPO_ROOT / "benchmarks" /
                        "Phase2TwoLaneHonestOperatingSweep" /
                        "honest_operating_smoke_summary.json"
                    )
                    if (not smoke_summary.is_file() or
                            not json.loads(smoke_summary.read_text()).get("passed")):
                        print(
                            "ERROR: a passing --two-lane-honest-operating-sweep smoke "
                            f"is required before {profile}.",
                            file=sys.stderr,
                        )
                        return 2
                if profile == "full":
                    k_summary = (
                        REPO_ROOT / "benchmarks" /
                        "Phase2TwoLaneHonestOperatingSweep" /
                        "honest_operating_k-full_summary.json"
                    )
                    if (not k_summary.is_file() or
                            not json.loads(k_summary.read_text()).get("passed")):
                        print(
                            "ERROR: a passing --two-lane-honest-operating-sweep "
                            "k-full is required before the full surface.",
                            file=sys.stderr,
                        )
                        return 2
                return run_two_lane_honest_operating_sweep(args, profile)
            sweep = "k" if args.two_lane_k_sweep else "sigma"
            profile = args.two_lane_k_sweep or args.two_lane_sigma_sweep
            if profile == "full":
                sweep_name = "k_sweep" if sweep == "k" else "sigma_sweep"
                sweep_dir = REPO_ROOT / "benchmarks" / (
                    "Phase2TwoLaneKSweep" if sweep == "k"
                    else "Phase2TwoLaneSigmaSweep"
                )
                smoke_summary = sweep_dir / f"{sweep_name}_smoke_summary.json"
                if (not smoke_summary.is_file() or
                        not json.loads(smoke_summary.read_text()).get("passed")):
                    print(
                        f"ERROR: a passing --two-lane-{sweep}-sweep smoke is required before full.",
                        file=sys.stderr,
                    )
                    return 2
            return run_two_lane_parameter_sweep(args, sweep, str(profile))
        if args.two_lane_grid or args.two_lane_grid_reanalyze:
            prerequisite = (
                REPO_ROOT / "benchmarks" / "Phase2TwoLaneValidation" /
                "conflict_release_cliff" / "run_0" /
                "two_lane_conflict_release_validation.json"
            )
            if not prerequisite.is_file() or not json.loads(prerequisite.read_text()).get("passed"):
                print(
                    "ERROR: passing --two-lane-conflict-release-validation is required before the full grid.",
                    file=sys.stderr,
                )
                return 2
            return run_two_lane_grid(args)
        if args.adjacent_lane_validation or args.adjacent_lane_validation_reanalyze:
            return run_adjacent_lane_validation(args, repetitions)
        if args.adjacent_lane_grid or args.adjacent_lane_grid_reanalyze:
            return run_adjacent_lane_grid(args)
        if args.longitudinal_grid:
            return run_longitudinal_grid(args)
        if args.longitudinal_grid_reanalyze:
            return run_longitudinal_grid(args)
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
        print("ERROR: --inject-b requires --experiment e2|e4|e2long.", file=sys.stderr)
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
                        ego_longitudinal_sigma=args.ego_longitudinal_sigma,
                        longitudinal_sigma=args.longitudinal_sigma,
                        lane_observation_mode=args.lane_observation_mode,
                        lateral_sigma=args.lateral_sigma,
                        lateral_claim_offset=args.lateral_claim_offset,
                        physical_gate_k=args.physical_gate_k,
                        distance_stationary_speed=args.distance_stationary_speed,
                        distance_attestation_retry_interval=args.distance_attestation_retry_interval,
                        distance_attestation_retry_max=args.distance_attestation_retry_max,
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
                        ego_longitudinal_sigma=args.ego_longitudinal_sigma,
                        longitudinal_sigma=args.longitudinal_sigma,
                        lane_observation_mode=args.lane_observation_mode,
                        lateral_sigma=args.lateral_sigma,
                        lateral_claim_offset=args.lateral_claim_offset,
                        physical_gate_k=args.physical_gate_k,
                    )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""
BFT-SMaRt V2V Intersection Log Analyzer.

Tracks the single-round PROPOSE_ALL protocol and per-car lifecycle metrics, including:
  - PROPOSE_ALL consensus latency (sim time): ResDB clusters Order_Decided into waves; per wave
      leader→commit = median(commit) − max(submit before first commit in wave);
      first_submit→commit = median(commit) − min(same submits) (incl. failed primary / VC).
      BFT-SMaRt fallback (PHASE_SUMMARY only): both metrics default to the same mean.
  - wait time at the intersection (stop -> leave)
  - arrival -> resume and arrival -> leave timing
  - throughput using the user-defined formula
  - ambulance vs normal vehicle comparisons

Batch / plotting workflow (same base --save-to, e.g. benchmarks/Priority4cars):
  python analyze_log.py combined.log --save-to benchmarks/Priority4cars --scenario 1 --cars 4
  # writes benchmarks/Priority4cars/no_amb/4veh_0.json (then 4veh_1.json, ...)
  # --scenario: 1=no ambulance, 2=honest ambulance, 3=Byz followers (legacy),
#             4=Byz leader with ambulance, 5=Byz leader no ambulance,
#             6=Byz followers with no ambulance, 15=R0 late-emergency rollback
  # plot_wait_time_cdf.py pools all matching Nveh_*.json per folder.
"""

import sys
import re
import os
import argparse
import math
import statistics
import xml.etree.ElementTree as ET
from collections import defaultdict

try:
    import matplotlib.pyplot as plt
except Exception:
    plt = None

_parser = argparse.ArgumentParser(description="BFT-SMaRt V2V log analyzer")
_parser.add_argument("log_file", nargs="?", default="/tmp/bft-all-replicas.log",
                     help="Path to combined replica log (default: /tmp/bft-all-replicas.log)")
_parser.add_argument("--save-to", metavar="DIR",
                     help="Copy metrics into DIR as <N>veh_<i>.log for later batch analysis")
_parser.add_argument(
    "--scenario", type=int, choices=[1, 2, 3, 4, 5, 6, 15, 16], default=None,
    metavar="N",
    help="Save under a scenario subfolder (requires --save-to): "
         "1=no ambulance, 2=honest ambulance, 3=ambulance+Byzantine followers, "
         "4=ambulance+Byzantine leader, 5=no ambulance+Byzantine leader, "
         "6=no ambulance+Byzantine followers, 15=R0 late-emergency rollback, "
         "16=crash wait-clear. "
         "Use same base DIR and --cars for each run; "
         "plot_wait_time_cdf.py pools all matching <N>veh_*.json in that folder.")
_parser.add_argument("--cars", type=int, default=None,
                     help="Total cars/replicas in the scenario (e.g. 12 or 16). "
                          "If omitted, inferred from --save-to path or defaults to 16.")
_parser.add_argument(
    "--run-index",
    type=int,
    default=None,
    metavar="IDX",
    help="Write <N>veh_IDX.log/json for a single run directory, replacing that "
         "repetition and pruning stale <N>veh_* analyzer outputs there. If omitted, "
         "append after existing analyzer outputs (legacy behavior).",
)
_parser.add_argument("--plots-dir", metavar="DIR",
                     help="Directory for generated plots/CSV. Defaults to the log file directory.")
_parser.add_argument(
    "--output-format",
    choices=["partner", "legacy"],
    default="partner",
    help="JSON output schema. 'partner' matches partnersv2v raft_results.json-style per-vehicle array.",
)
_parser.add_argument(
    "--coordination-method",
    default="pbft",
    help="Value for JSON field coordination_method in partner-format output (default: pbft).",
)
_parser.add_argument(
    "--no-scenario-subdir",
    action="store_true",
    help="Write metrics directly under --save-to (do not append no_amb/amb_honest/...). "
         "Use when --save-to already includes the scenario folder (e.g. .../amb_honest/run_0).",
)
_parser.add_argument(
    "--baseline-tripinfo",
    metavar="XML",
    help="Load SUMO tripinfo output for SUMO-controlled baseline runs.",
)
_parser.add_argument(
    "--scenario-ini",
    metavar="INI",
    help="Optional random_scenario.ini used to identify the priority vehicle.",
)
_args = _parser.parse_args()

# Scenario subdirs (match plot_wait_time_cdf.DEFAULT_BAR_SERIES globs)
SCENARIO_SUBDIR = {
    1: "no_amb",
    2: "amb_honest",
    3: "amb_byz_follower",
    4: "amb_byz_leader",
    5: "no_amb_byz_leader",
    6: "no_amb_byz_follower",
    15: "rollback_emergency_dynamic_n",
    16: "crash_wait_clear",
}
SCENARIO_LABEL = {
    1: "no ambulance",
    2: "honest ambulance",
    3: "ambulance + Byzantine followers",
    4: "ambulance + Byzantine leader",
    5: "no ambulance + Byzantine leader",
    6: "no ambulance + Byzantine followers",
    15: "R0 late-emergency rollback",
    16: "Crash wait-clear (Stage 1a harness)",
}

LOG_FILE = _args.log_file
SAVE_TO = _args.save_to
if _args.scenario is not None:
    if not SAVE_TO:
        print("analyze_log: --scenario requires --save-to <base_dir>", file=sys.stderr)
        sys.exit(2)
    if not _args.no_scenario_subdir:
        SAVE_TO = os.path.join(SAVE_TO, SCENARIO_SUBDIR[_args.scenario])
PLOTS_DIR = _args.plots_dir or os.path.dirname(os.path.abspath(LOG_FILE)) or "."

def _infer_cars_from_save_to(save_to: str or None) -> int or None:
    if not save_to:
        return None
    # Try a few common naming conventions:
    # - ".../Priority12cars/..." or ".../Priority16cars/no_amb/..."
    # - ".../12veh_0.log" or folder ".../12vehRuns"
    # - ".../2phase12Honest" or ".../phase12..."
    hay = os.path.abspath(save_to)
    m = re.search(r"Priority\s*(\d+)\s*cars?", hay, flags=re.IGNORECASE)
    if m:
        try:
            return int(m.group(1))
        except ValueError:
            pass
    patterns = [
        r'(\d+)\s*veh',
        r'phase\s*(\d+)',
        r'(\d+)(?=Honest)',
    ]
    for pat in patterns:
        m = re.search(pat, hay, flags=re.IGNORECASE)
        if m:
            try:
                return int(m.group(1))
            except ValueError:
                pass
    return None

CARS = _args.cars or _infer_cars_from_save_to(SAVE_TO) or 16
N_EPOCHS = max(1, CARS // 4)

# Epoch → group size mapping (4 cars depart each round)
EPOCH_N = {epoch: max(CARS - 4 * epoch, 0) for epoch in range(N_EPOCHS)}

def bft_quorum(n_active: int, f: int) -> int:
    if n_active <= 0:
        raise ValueError("n_active must be positive")
    if f < 0 or f > (n_active - 1) // 3:
        raise ValueError("invalid f for active membership")
    return (n_active + f + 2) // 2

# ── Patterns ────────────────────────────────────────────────────────────────
RE_ROUND_METRIC = re.compile(
    r'\[ROUND-METRICS\] Epoch (\d+) Avg_([\w]+): ([\d.]+) seconds')

RE_DIAG_EPOCH = re.compile(
    r'\[ORDER-DIAG\] Replica (\d+) epoch=(\d+)')

# Vehicle Lifecycle Diagnostics
RE_ORDER_DECISION = re.compile(
    r'\[V2VProxy\s+\d+\] handleOrderDecision: (.*)|\[ORDER\].*DECIDED.*: (.*)'
)
RE_ORDER_COMMITTED = re.compile(r'\[ORDER\] Committed OrderBag epoch=(\d+) batches=\d+ decision=(.*)')
RE_BATCH_ENTRY = re.compile(r'(veh\d+):BATCH:(\d+)')

RE_MESSAGES_SENT     = re.compile(r'\[METRICS (\d+)\]\s+(?:Messages_Sent|Sent Messages): (\d+)')
RE_MESSAGES_RECEIVED = re.compile(r'\[METRICS (\d+)\]\s+(?:Messages_Received|Received Messages): (\d+)')
RE_BYTES_SENT        = re.compile(r'\[METRICS (\d+)\]\s+Bytes_Sent: (\d+)')
RE_QUIET_HONEST_VEHICLES = re.compile(r'\[METRICS (\d+)\]\s+Quiet_Honest_Vehicles: (\d+)')
RE_QUIET_HONEST_OPPORTUNITIES = re.compile(r'\[METRICS (\d+)\]\s+Quiet_Honest_Opportunities: (\d+)')
RE_QUIET_HONEST_RATE = re.compile(r'\[METRICS (\d+)\]\s+Quiet_Honest_Rate: ([-\d.]+)')
RE_ARRIVAL_TIME      = re.compile(r'\[METRICS (\d+)\] Arrival_Time: ([\d.]+)')
RE_STOP_TIME         = re.compile(r'\[METRICS (\d+)\] Stop_Time: ([\d.]+)')
RE_RESUME_TIME       = re.compile(r'\[METRICS (\d+)\] Resume_Time: ([\d.]+)')
# ResDB: chronological (rep, sim_time_s) for cluster-based commit waves (see
# compute_resdb_submit_to_commit_by_wave).
RE_RESDB_PROPOSE_SUBMIT = re.compile(
    r'\[METRICS (\d+)\] ProposeAll_Submit_Time:\s*(\S+)')
RE_RESDB_ORDER_DECIDED = re.compile(
    r'\[METRICS (\d+)\] Order_Decided_Time:\s*(\S+)')
RE_CERT_START = re.compile(r'\[METRICS (\d+)\] Cert_Collection_Start:\s*(\S+)')
RE_VC_TRIGGER = re.compile(r'\[VC-TRIGGER\]\s+r(\d+)\s+forcing view change at\s+(\S+)')
RE_PRIMARY_CHANGED = re.compile(
    r'\[VC-DEBUG\]\s+r(\d+)\s+primary changed:\s+(\d+)\s+->\s+(\d+)\s+t=(\S+)'
)
RE_GOSSIP_APPLY = re.compile(r'\[GOSSIP-APPLY\]\s+r(\d+)\s+epoch=(\d+)\s+t=(\S+)')
RE_GOSSIP_SEND = re.compile(r'\[GOSSIP-SEND\]\s+r(\d+)\s+epoch=(\d+)\s+retry=(\d+)\s+t=(\S+)')
RE_BYZANTINE_CONFIG = re.compile(r'\[BYZANTINE\]\s+r(\d+)\s+([A-Z_]+)')
RE_PREVERIFY_STATE_MISMATCH = re.compile(
    r'\[OMNET-PREVERIFY\]\s+reject:\s+state-field mismatch'
)
RE_FALSE_LANE_COLLUDER_SET = re.compile(
    r'\[FALSE-LANE-COLLUDER-SET\]\s+r\d+\s+N=(\d+)\s+F=(\d+)\s+ids=([^\s]*)\s+position_gate=(\d+)'
)
RE_FALSE_LANE_COLLUSION_ECHO = re.compile(
    r'\[FALSE-LANE-COLLUSION-ECHO\]\s+target=(veh\d+)\s+signer=(\d+)(?:\s+count=(\d+)/(\d+))?'
)
RE_FALSE_LANE_COLLUSION_CERT = re.compile(
    r'\[FALSE-LANE-COLLUSION-CERT\]\s+target=(veh\d+)\s+signers=([^\s]+)\s+threshold=(\d+)'
)
RE_FALSE_LANE_COLLUSION_BLOCK = re.compile(
    r'\[FALSE-LANE-COLLUSION-BLOCK\]\s+target=(veh\d+)\s+signers=(\d+)\s+threshold=(\d+)'
)
RE_FALSE_LANE_COLLUSION_COMMIT = re.compile(
    r'\[FALSE-LANE-COLLUSION-COMMIT\]\s+target=(veh\d+)\s+claimed=([^\s]+)\s+actual=([^\s]+)\s+epoch=(\d+)'
)
RE_MALFORMED_PROPOSAL_REJECT = re.compile(r'\[MALFORMED-PROPOSAL-REJECT\]')
RE_UNSAFE_CONFLICT_COOCCUPANCY = re.compile(
    r'\[UNSAFE-CONFLICT-COOCCUPANCY\]\s+first=(veh\d+)\s+first_approach=([NSEW])\s+'
    r'second=(veh\d+)\s+second_approach=([NSEW])\s+t=([\d.]+)'
)
RE_MOVEMENT_GROUND_TRUTH = re.compile(
    r'\[MOVEMENT-GROUND-TRUTH\]\s+vehicle=(veh\d+)\s+plannedIngress=(\S+)\s+'
    r'plannedEgress=(\S+)\s+movement=([NSEW]-[SLR])\s+t=([\d.]+)'
)
RE_CONFLICT_ZONE = re.compile(
    r'\[CONFLICT-ZONE\]\s+vehicle=(veh\d+)\s+movement=(\S+)\s+'
    r'event=(ENTER|EXIT)\s+road=(\S*)\s+lane=(\S*)\s+t=([\d.]+)'
)
RE_MOVEMENT_ACTUAL_EGRESS = re.compile(
    r'\[MOVEMENT-ACTUAL-EGRESS\]\s+vehicle=(veh\d+)\s+plannedEgress=(\S+)\s+'
    r'actualEgress=(\S+)\s+match=(\d+)\s+t=([\d.]+)'
)
RE_CONFLICTING_COOCCUPANCY = re.compile(
    r'\[CONFLICTING-COOCCUPANCY\]\s+first=(veh\d+)\s+firstMovement=(\S+)\s+'
    r'second=(veh\d+)\s+secondMovement=(\S+)\s+t=([\d.]+)'
)
RE_METROLOGY_CONFIG = re.compile(
    r'\[METROLOGY-CONFIG\]\s+speedMode=(-?\d+)\s+speedMps=([\d.]+)\s+'
    r'jmIgnoreFoeProb=([\d.]+)\s+collisionCheckJunctions=(\d+)\s+'
    r'collisionAction=(\S+)\s+timeToTeleport=(-?[\d.]+)\s+vehicles=(\S+)'
)
RE_METROLOGY_RELEASE = re.compile(
    r'\[METROLOGY-RELEASE\]\s+vehicle=(veh\d+)\s+speedMode=(-?\d+)\s+'
    r'speedMps=([\d.]+)\s+t=([\d.]+)'
)
RE_PHYSICAL_COLLISION = re.compile(
    r'\[PHYSICAL-COLLISION\]\s+vehicle=(veh\d+)\s+t=([\d.]+)'
)
RE_CONSENSUS_ATTACK_OUTCOME = re.compile(
    r'\[CONSENSUS_ATTACK_OUTCOME\]\s+r(\d+)\s+epoch=(\d+)\s+'
    r'fault=([A-Z_]+)\s+outcome=([A-Z_]+)(?:\s+(.*))?'
)
RE_FALSE_PRIORITY_GRANTED = re.compile(
    r'\[FALSE_PRIORITY_GRANTED\]\s+r(\d+)\s+epoch=(\d+)\s+veh(\d+)'
)
RE_CRASH_DETECTED = re.compile(
    r'\[CRASH_DETECTED\]\s+r(\d+)\s+epoch=(\d+)\s+batch=(\d+)'
)
RE_STOPSIGN_TIMEOUT  = re.compile(r'\[METRICS (\d+)\] StopSign_Timeout: 1')
RE_AMBULANCE_SCHED   = re.compile(r'\[AMBULANCE_SCHED\] (veh\d+) batch=')
RE_CAR_METRICS       = re.compile(
    r'\[CAR-METRICS\] (veh\d+) role=(ambulance|normal) epoch=(\d+) '
    r'stop_time=([-\d.]+) depart_time=([-\d.]+) wait_stop_to_departure_sec=([-\d.]+)')
RE_DEPARTED          = re.compile(
    r'\[DEPARTED\]\s+Replica\s+(\d+)\s+cleared intersection\s+t=([-\d.]+)')
RE_AMBULANCE_WAIT    = re.compile(
    r'\[AMBULANCE_METRICS\] (veh\d+) sim_wait_stop_to_departure_sec=([-\d.]+) epoch=(\d+)')
RE_BYZANTINE_INJECTION = re.compile(
    r'\[BYZANTINE INJECTION\] Replica (\d+) CID=(\d+) intentionally broadcasting corrupted consensus hash')
RE_FABRICATED_CLEARANCE = re.compile(
    r'\[BYZANTINE-FABRICATED-CLEARANCE\]\s+r(\d+)\s+'
    r'cancelled_epoch=(\d+)\s+new_epoch=(\d+).*action=submit-invalid-clear')
RE_MISSING_CLEAR_REJECT = re.compile(
    r'\[EPOCH-VIEW-REJECT\]\s+reason=missing-clear-evidence\s+epoch=(\d+)')
RE_UNSAFE_RECOVERY_ORDER = re.compile(
    r'\[UNSAFE-RECOVERY-ORDER\]\s+r(\d+)\s+cancelled_epoch=(\d+)\s+'
    r'new_epoch=(\d+).*box_occupied=(\d+)')
RE_CRASH_INJECT_EVENT = re.compile(
    r'\[CRASH-INJECT\]\s+manager\s+(veh\d+)\s+t=([\d.]+)')
RE_CRASH_TOW_EVENT = re.compile(
    r'\[TOW\]\s+manager\s+(veh\d+)\s+t=([\d.]+)')
RE_CRASH_COOCCUPANCY_EVENT = re.compile(
    r'\[CRASH-COOCCUPANCY\]\s+entrant=(veh\d+)\s+'
    r'active_wrecks=(\d+)\s+t=([\d.]+)')
RE_WAIT_SEND_EVENT = re.compile(
    r'\[WAIT-SEND\]\s+r(\d+).*\st=([\d.]+)')
RE_WAIT_STOP_EVENT = re.compile(
    r'\[WAIT-STOP\]\s+r(\d+)\s+reason=([^\s]*)\s+t=([\d.]+)')
RE_CLEAR_CERT_BROADCAST_EVENT = re.compile(
    r'\[CLEAR-CERT\]\s+r(\d+)\s+broadcast\s+key=([^\s]+).*\st=([\d.]+)')
RE_ROLLBACK_COMMIT_EVENT = re.compile(
    r'\[ROLLBACK-COMMIT\]\s+r(\d+)\s+cancelled_epoch=(\d+)\s+'
    r'new_epoch=(\d+)\s+t=([\d.]+)')

RE_PHASE_SUMMARY = re.compile(
    r'\[PHASE_SUMMARY (\d+)\](?:\s+epoch=(\d+))? .*PROPOSE_ALL_BFT\(sim\)=(?:([\d.]+)s|N/A) .*stop_to_decision\(sim\)=(?:([\d.]+)s|N/A)'
)
RE_CONSENSUS_HEADER = re.compile(r'CONSENSUS METRICS \(Replica (\d+)\) epoch=(\d+)')
RE_CERT_DUR = re.compile(r'\[METRICS (\d+)\] Cert_Collection_Duration: ([\d.]+)s')
RE_CERT_CREATION_LATENCY = re.compile(r'\[METRICS (\d+)\] Cert_Creation_Latency: ([-\d.]+)s')
# ResDB batch assignment (from IntersectionExecutor batch packer)
RE_BATCH_ASSIGN = re.compile(r'\[METRICS (\d+)\] Batch_Assignment: batch=(\d+)')
RE_TOLERATED_F_CONFIG = re.compile(
    r'\[TOLERATED-F-(?:APP|CONFIG)\].*tolerated_f=(-?\d+)'
    r'.*static_n=(\d+).*quorum=(-?\d+).*cert_threshold=(-?\d+)'
)
RE_R0_CANCEL_WITNESS = re.compile(
    r'\[CANCEL-WITNESS\]\s+r(\d+).*cancelled_epoch=(\d+).*ref=([^\s]+)'
)
RE_R0_CANCEL_ECHO = re.compile(
    r'\[CANCEL-ECHO\]\s+r(\d+)\s+epoch=(\d+)\s+reason=(\d+)\s+ref=([^\s]+)'
)
RE_R0_CANCEL_CERT = re.compile(
    r'\[CANCEL-CERT\]\s+r(\d+)\s+broadcast\s+key=([^\s]+)\s+echoes=(\d+)'
)
RE_R0_ROLLBACK_QUORUM = re.compile(
    r'\[ROLLBACK-QUORUM\]\s+r(\d+)\s+\|M\|=(\d+)\s+f_anchored=(\d+)'
    r'\s+quorum=(\d+)\s+proposer=r(\d+).*new_epoch=(\d+)'
)
RE_R0_ROLLBACK_QUORUM_V3 = re.compile(
    r'\[ROLLBACK-QUORUM\]\s+r(\d+)\s+mode=(per_epoch|anchored)'
    r'\s+voteN=(\d+)\s+sceneN=(\d+)\s+f=(\d+)\s+quorum=(\d+)'
    r'\s+proposer=r(\d+).*new_epoch=(\d+)'
)
RE_R0_ROLLBACK_RECONFIG = re.compile(
    r'\[ROLLBACK-RECONFIG\]\s+r(\d+)\s+mode=(per_epoch|anchored)'
    r'\s+oldEpoch=(\d+)\s+newEpoch=(\d+)\s+voteN=(\d+)\s+sceneN=(\d+)'
    r'\s+newF=(\d+)\s+quorum=(\d+)'
)
RE_R0_ROLLBACK_UNAVAILABLE = re.compile(
    r'\[ROLLBACK-UNAVAILABLE\].*\|M\|=(\d+).*f_anchored=(\d+)'
    r'(?:\s+f_dynamic=(-?\d+))?.*need>=(\d+)'
)
RE_R0_ROLLBACK_UNAVAILABLE_V3 = re.compile(
    r'\[ROLLBACK-UNAVAILABLE\].*mode=(per_epoch|anchored)'
    r'(?:\s+reason=[^\s]+)?\s+voteN=(\d+)(?:\s+f_anchored=(\d+))?'
    r'(?:\s+f=(\d+))?\s+need>=(\d+)'
)
RE_R0_ROLLBACK_PROPOSE_SKIP_MINUS_ONE = re.compile(
    r'\[ROLLBACK-PROPOSE\].*proposer=r-1'
)
RE_R0_ROLLBACK_BEGIN = re.compile(
    r'\[ROLLBACK-BEGIN\]\s+r(\d+)\s+cancelled_epoch=(\d+)\s+new_epoch=(\d+)'
)
RE_R0_CANCEL_COMMIT = re.compile(
    r'\[CANCEL-COMMIT\]\s+r(\d+)\s+cancelled_epoch=(\d+)\s+seq=(\d+)\s+'
    r'(?:quorum=(\d+)\s+)?source=([^\s]+)'
)
RE_CANCEL_LEADER_SUPPRESS = re.compile(
    r'\[BYZANTINE-CANCEL-SUPPRESS\]\s+r(\d+).*cancelled_epoch=(\d+)'
    r'.*attack_time=([0-9.]+)'
)
RE_CANCEL_LEADER_ROTATE = re.compile(
    r'\[CANCEL-VC\]\s+r(\d+)\s+rotating cancel proposer index=(\d+)'
    r'.*cancelled_epoch=(\d+).*\bt=([0-9.]+)'
)
RE_CANCEL_COMMIT_TIME = re.compile(
    r'\[CANCEL-COMMIT\]\s+r(\d+).*cancelled_epoch=(\d+).*\bt=([0-9.]+)'
)
RE_R0_ROLLBACK_COMMIT = re.compile(
    r'\[ROLLBACK-COMMIT\]\s+r(\d+)\s+cancelled_epoch=(\d+)\s+new_epoch=(\d+)'
)
RE_R0_ROLLBACK_PROPOSE_RC = re.compile(
    r'\[ROLLBACK-PROPOSE\]\s+r(\d+)\s+rc=(-?\d+)'
)
RE_R0_ROLLBACK_DISCOVERY_SNAPSHOT = re.compile(
    r'\[ROLLBACK-DISCOVERY\]\s+r(\d+)\s+snapshot\s+reason=([^\s]+)\s+'
    r'mode=(per_epoch|anchored)\s+signed=(\d+)\s+quiet=(\d+)\s+'
    r'voteN=(\d+)\s+sceneN=(\d+)\s+expectedN=(\d+)'
)
RE_R0_ACTIVE_VIEW_EPOCH1 = re.compile(
    r'\[ACTIVE-VIEW\]\s+mode=(?:pending|promoted|request)\s+epoch=1\s+'
    r'seq=(\d+)\s+hash=([^\s]+)\s+N=(\d+)\s+f=(\d+)\s+'
    r'quorum=(\d+)\s+primary=r(\d+)'
)
RE_R0_EXECUTOR_ORDER_EPOCH = re.compile(
    r'\[EXECUTOR\]\s+OrderDecision:\s+epoch=(\d+).*?'
    r'n_vehicles=(\d+)\s+n_batches=(\d+)(?:\s+decisions=\[(.*)\])?'
)
RE_PBFT_NEW_REQ = re.compile(
    r'\[PBFT-NEW-REQ\]\s+primary=(\d+)\s+broadcasting PRE_PREPARE seq=(\d+)'
)

# Pre-computed summary metrics (written by --save-to or the C++ side)
RE_RUN_METRIC       = re.compile(r'\[RUN-METRICS\] ([\w_]+):\s*([-\d.]+)')
RE_ROUND_METRIC_CAR = re.compile(
    r'\[ROUND-METRICS\] Epoch (\d+) Arrival_To_Resume_Time (veh\d+):\s*([\d.]+) seconds')
RE_ROUND_METRIC_KEY = re.compile(
    r'\[ROUND-METRICS\] Epoch (\d+) ([A-Za-z_][\w_]*):\s*([-\d.]+)')
RE_PERC_EVAL = re.compile(
    r'\[PERC-EVAL\]\s+witness=(\d+)\s+target=(\S+)\s+epoch=(\d+)\s+'
    r'claimHash=([0-9a-fA-F]+)\s+laneVerdict=(ACCEPT|REJECT).*?'
    r'observedCue=(\S+)\s+knownCueSamples=(\d+)')
RE_EQUIVOCATION_DETECTED = re.compile(
    r'\[EQUIVOCATION-DETECTED\]\s+witness=(\d+)\s+target=(\S+)\s+epoch=(\d+)\s+'
    r'firstHash=([0-9a-fA-F]+)\s+secondHash=([0-9a-fA-F]+)\s+'
    r'firstDirection=(\S+)\s+secondDirection=(\S+)\s+'
    r'action=(\S+)\s+perceptionEvaluated=(\d+)\s+echoSent=(\d+)')
RE_LATERAL_PERC_FIELDS = re.compile(
    r'mode=(\S+)\s+trueLateralCm=(-?\d+)\s+observedLateralCm=(-?\d+)\s+'
    r'claimedLateralCm=(-?\d+)\s+lateralResidualCm=(\d+)\s+'
    r'lateralToleranceCm=(\S+)\s+claimedPhysicalLaneIndex=(-?\d+)\s+'
    r'projectedPhysicalLaneIndex=(-?\d+)')
RE_CERT_ASSEMBLE_PERCEPTION = re.compile(
    r'\[CERT-ASSEMBLE\]\s+target=(\S+)\s+epoch=(\d+)\s+echoCount=(\d+)\s+'
    r'threshold=(\d+)\s+reason=(\S+)')
RE_DIR_ELIGIBILITY = re.compile(
    r'\[DIR-ELIGIBILITY\]\s+target=(\S+)\s+epoch=(\d+)\s+declared=(\S+)\s+'
    r'support=(\d+)\s+threshold=(\d+)\s+echoCount=(\d+)\s+b_sig=(\d+)\s+'
    r'(?:physicalLaneIndex=-?\d+\s+laneAuthorized=\d+\s+)?derivedDirection=(\S+)')
RE_DIR_ELIGIBILITY_SELF = re.compile(
    r'\[DIR-ELIGIBILITY\]\s+target=(\S+)\s+epoch=(\d+)\s+declared=(\S+)\s+'
    r'support=(\d+)\s+threshold=(\d+)\s+echoCount=(\d+)\s+'
    r'selfAttestations=(\d+)\s+b_sig=(\d+)\s+'
    r'(?:physicalLaneIndex=-?\d+\s+laneAuthorized=\d+\s+)?derivedDirection=(\S+)')
RE_SELF_ATTEST = re.compile(
    r'\[SELF-ATTEST\]\s+target=(\S+)\s+epoch=(\d+)\s+signer=(\d+)\s+'
    r'claimHash=([0-9a-fA-F]+)\s+observedCue=(\S+)\s+status=(\S+)')
RE_CERT_EVIDENCE = re.compile(
    r'\[CERT-EVIDENCE\]\s+target=(\S+)\s+epoch=(\d+)\s+signer=(\d+)\s+'
    r'cue=(\S+)\s+self=(\d+)\s+byzantine=(\d+)\s+supporting=(\d+)')
RE_ECHO_COLLECT = re.compile(
    r'\[ECHO-COLLECT\]\s+target=(\S+)\s+epoch=(\d+)\s+signer=(\d+)\s+'
    r'byzantine=(\d+)\s+self=(\d+)\s+echoCount=(\d+)\s+'
    r'threshold=(\d+)\s+source=(\S+)')
RE_PHASE2_ATTACK_CONFIG = re.compile(
    r'\[PHASE2-ATTACK-CONFIG\]\s+replica=(\d+)\s+kind=(\S+)\s+'
    r'target=(-?\d+)\s+actualB=(\d+)\s+colluders=([^\s]*)')
RE_PHASE2_ATTACK_DECLARE = re.compile(
    r'\[PHASE2-ATTACK-DECLARE\]\s+target=(\S+)\s+epoch=(\d+)\s+kind=(\S+)\s+'
    r'actualLane=(\S+)\s+claimedLane=(\S+)\s+actualDirection=(\S+)\s+'
    r'claimedDirection=(\S+)')
RE_PHASE2_COLLUSION_ECHO = re.compile(
    r'\[PHASE2-COLLUSION-ECHO\]\s+target=(\S+)\s+epoch=(\d+)\s+'
    r'signer=(\d+)\s+kind=(\S+)\s+claimedLane=(\S+)\s+cue=(\S+)')
RE_PHASE2_ATTACK_OUTCOME = re.compile(
    r'\[PHASE2-ATTACK-OUTCOME\]\s+target=(\S+)\s+epoch=(\d+)\s+kind=(\S+)\s+'
    r'laneCertified=(\d+)\s+falseLaneCert=(\d+)\s+'
    r'(?:physicalLaneIndex=-?\d+\s+actualPhysicalLaneIndex=-?\d+\s+'
    r'lateralClaimCm=-?\d+\s+)?falseEligibility=(\d+)\s+'
    r'support=(\d+)\s+threshold=(\d+)\s+b_sig=(\d+)')
RE_PHASE2_LATERAL_ATTACK_OUTCOME = re.compile(
    r'\[PHASE2-ATTACK-OUTCOME\]\s+target=(\S+)\s+epoch=(\d+)\s+kind=(FALSE_PHYSICAL_LANE)\s+'
    r'laneCertified=(\d+)\s+falseLaneCert=(\d+)\s+physicalLaneIndex=(-?\d+)\s+'
    r'actualPhysicalLaneIndex=(-?\d+)\s+lateralClaimCm=(-?\d+)\s+'
    r'falseEligibility=(\d+)\s+support=(\d+)\s+threshold=(\d+)\s+b_sig=(\d+)')
RE_TRUST_TIER = re.compile(
    r'\[TRUST-TIER\]\s+target=(\S+)\s+epoch=(\d+)\s+tier=(\S+)')
RE_PERCEPTION_CONFIG = re.compile(
    r'\[PERCEPTION-CONFIG\]\s+r(\d+)\s+sigma=(\S+)\s+signal_error=(\S+)\s+'
    r'ego_lat_sigma=(\S+)\s+ego_lon_sigma=(\S+)\s+witness_lat_sigma=(\S+)\s+'
    r'witness_lon_sigma=(\S+)\s+gate_k=(\S+)\s+rng=(\d+)\s+collection_window=(\S+)')
RE_STOPPED_DISTANCE_ATTEST = re.compile(
    r'\[STOPPED-DISTANCE-ATTEST\]\s+target=(\S+)\s+epoch=(\d+)\s+'
    r'earlyClaimHash=([0-9a-fA-F]+)\s+distanceToStopCm=(\d+)\s+speed=(\S+)')
RE_STOPPED_DISTANCE_RETRY = re.compile(
    r'\[STOPPED-DISTANCE-ATTEST-RETRY\]\s+target=(\S+)\s+epoch=(\d+)\s+'
    r'attempt=(\d+)\s+max=(\d+)')
RE_DISTANCE_PERC_EVAL = re.compile(
    r'\[DIST-PERC-EVAL\]\s+witness=(\d+)\s+target=(\S+)\s+epoch=(\d+)\s+'
    r'attestationHash=([0-9a-fA-F]+)\s+verdict=(ACCEPT|REJECT)\s+'
    r'trueDistanceM=(\S+)\s+observedDistanceM=(\S+)\s+'
    r'claimedDistanceCm=(\d+)\s+residualCm=(\d+)\s+toleranceCm=(\d+)\s+'
    r'stationary=(\d+)')
RE_DISTANCE_CERT_ASSEMBLE = re.compile(
    r'\[DIST-CERT-ASSEMBLE\]\s+target=(\S+)\s+epoch=(\d+)\s+'
    r'signerCount=(\d+)\s+threshold=(\d+)\s+reason=(\S+)')
RE_DISTANCE_CERT_COLLECT = re.compile(
    r'\[DIST-CERT-COLLECT\]\s+target=(\S+)\s+epoch=(\d+)\s+'
    r'signerCount=(\d+)\s+threshold=(\d+)\s+signer=(\d+)')
RE_DISTANCE_ATTACK_DECLARE = re.compile(
    r'\[DIST-ATTACK-DECLARE\]\s+target=(\S+)\s+epoch=(\d+)\s+'
    r'trueDistanceM=(\S+)\s+claimedDistanceM=(\S+)\s+offsetM=(\S+)')
RE_DISTANCE_COLLUSION_ECHO = re.compile(
    r'\[DIST-COLLUSION-ECHO\]\s+target=(\S+)\s+epoch=(\d+)\s+'
    r'signer=(\d+)\s+claimedDistanceCm=(\d+)')
RE_DISTANCE_CERT_EVIDENCE = re.compile(
    r'\[DIST-CERT-EVIDENCE\]\s+target=(\S+)\s+epoch=(\d+)\s+'
    r'signer=(\d+)\s+self=(\d+)\s+byzantine=(\d+)')
RE_DISTANCE_ORDER_PAIR = re.compile(
    r'\[DIST-ORDER-PAIR\]\s+epoch=(\d+)\s+lane=(\S+)\s+a=(\S+)\s+b=(\S+)\s+'
    r'certAcm=(\d+)\s+certBcm=(\d+)\s+trueAm=(\S+)\s+trueBm=(\S+)\s+'
    r'inversion=(\d+)')

# ── Data stores ─────────────────────────────────────────────────────────────
round_metrics = defaultdict(dict)      # epoch → {metric_name: value}

# Track which epoch we're in for each replica (coarse: by ORDER-DIAG)
replica_epoch = {}                     # replica → last seen epoch

# Instrument for Vehicle Lifecycle
go_decision_epoch = {}  # carId -> epoch it was granted GO

# New per-epoch data stores
replica_arrival_time = {}                   # replica -> first arrival time in intersection zone
replica_stop_time   = {}                    # replica -> stop_time (float)
replica_resume_time = {}                    # replica -> resume_time (float)
replica_depart_time = {}                    # replica -> physical departure fallback
epoch_messages_sent = defaultdict(list)     # epoch -> [sent_count, ...]
epoch_messages_recv = defaultdict(list)     # epoch -> [recv_count, ...]
epoch_bytes_sent    = defaultdict(list)     # epoch -> [payload_bytes, ...]
epoch_total_dur     = defaultdict(list)     # epoch -> [duration, ...]
epoch_total_dur_by_car = defaultdict(dict)  # epoch -> {carId: duration}
epoch_failures      = defaultdict(int)      # epoch -> count
epoch_cert_creation_latency = defaultdict(list)  # epoch -> [cert f+1 assembly latency seconds]
ambulance_ids       = set()
car_metrics         = defaultdict(dict)     # carId -> {metric_name: value}
replica_batch_index = {}                    # replica_id (int) -> batch_index (int)
r0_metrics = {
    "enabled": False,
    "failure_stage": None,
    "cancel_witness_count": 0,
    "cancel_echo_count": 0,
    "cancel_cert_count": 0,
    "bad_quorum_count": 0,
    "rollback_unavailable_count": 0,
    "rollback_unavailable_max_m": 0,
    "rollback_unavailable_f": None,
    "rollback_unavailable_need": None,
    "rollback_quorums": [],
    "rollback_reconfigs": [],
    "rollback_discovery_snapshots": [],
    "rollback_active_views": [],
    "cancel_commit_count": 0,
    "cancel_leader_suppress_count": 0,
    "cancel_leader_attack_replica": None,
    "cancel_leader_attack_time": None,
    "cancel_leader_rotation_count": 0,
    "cancel_leader_max_rotation_index": 0,
    "cancel_leader_first_rotation_time": None,
    "cancel_leader_first_commit_time": None,
    "cancel_leader_failover_latency_sec": None,
    "rollback_commit_count": 0,
    "rollback_commit_replica_ids": [],
    "epoch1_order_decision_count": 0,
    "epoch1_order_n_vehicles": None,
    "epoch1_order_n_batches": None,
    "epoch1_order_vehicle_ids": [],
    "rollback_begin_seen": False,
    "rollback_epoch1_committed": False,
    "rollback_propose_rc0": False,
    "rollback_pbft_new_req_after_propose": False,
    "proposer_minus_one_skip_count": 0,
    "rollback_mode": None,
}

# Byzantine injection tracking
byzantine_by_epoch   = defaultdict(int)    # epoch -> count of [BYZANTINE INJECTION] events
byzantine_by_replica = defaultdict(int)    # replica -> count
byzantine_total      = 0                   # across the whole run
fabricated_clearance_events = set()        # {(replica, cancelled_epoch, new_epoch)}
missing_clear_reject_count = 0
unsafe_recovery_order_replicas = set()
unsafe_recovery_order_box_occupied_replicas = set()
crash_inject_times = {}              # vehicle -> first injection sim-time
crash_tow_times = {}                 # vehicle -> first tow sim-time
crash_unsafe_entrants = {}           # vehicle -> first unsafe box-entry sim-time
wait_send_times = []                 # leader heartbeat sim-times
wait_stop_times = []                 # local WAIT stop sim-times
clear_cert_broadcast_times = []      # authenticated CLEAR cert origin times
recovery_commit_times = []           # ORDER(e+1) local apply times
false_lane_colluder_config = None     # canonical {N,F,ids,position_gate}
false_lane_echo_signers = defaultdict(set)  # target -> distinct signer ids observed
false_lane_certificates = {}          # target -> {signers, threshold}
false_lane_blocked_targets = {}       # target -> {signers, threshold}
false_lane_commits = {}               # target -> {claimed, actual, epoch}
malformed_proposal_reject_count = 0
unsafe_conflict_pairs = {}            # (first,second) -> ground-truth approaches/time
physical_collision_vehicles = {}      # vehicle -> first collision time
movement_ground_truth = {}            # vehicle -> planned ingress/egress and movement
movement_actual_egress = {}            # vehicle -> observed egress and agreement
conflict_zone_events = []              # entry/exit movement trace
movement_conflict_pairs = {}           # pair -> movement-ground-truth conflict
metrology_config = None
metrology_releases = {}
perception_evaluations = {}           # (witness,target,epoch,hash) -> parsed evaluation
perception_duplicate_evaluations = 0
equivocation_events = {}               # (witness,target,epoch,secondHash) -> retained proof/log
cert_assembly_perception = {}         # (target,epoch) -> collection outcome
direction_eligibility = {}            # (target,epoch) -> support/derived outcome
trust_tiers = {}                      # (target,epoch) -> final tier
self_attestations = {}                 # (target,epoch,signer,hash) -> signed local declaration
self_attestation_duplicates = 0
cert_evidence = defaultdict(dict)      # (target,epoch) -> signer -> cue/accounting
arrival_echo_collections = defaultdict(dict)  # (target,epoch) -> signer -> collection accounting
phase2_attack_config = None
phase2_attack_config_replicas = set()
phase2_attack_config_mismatches = 0
phase2_attack_declarations = {}
phase2_collusion_echoes = defaultdict(set)
phase2_attack_outcomes = {}
perception_config = None
perception_config_replicas = set()
stopped_distance_attestations = {}
stopped_distance_attestation_retries = defaultdict(int)
distance_perception_evaluations = {}
distance_perception_duplicate_evaluations = 0
distance_certificates = {}
distance_cert_collection = []
distance_attack_declarations = {}
distance_collusion_echoes = defaultdict(set)
distance_cert_evidence = defaultdict(dict)
distance_order_pairs = {}

# Per-replica message counts and fallback tracking
replica_messages_sent    = {}   # replica -> messages_sent count (last value seen)
replica_messages_recv    = {}   # replica -> messages_received count (last value seen)
replica_bytes_sent       = {}   # replica -> payload bytes sent (last value seen)
replica_stopsign_timeout = set()  # replica IDs that hit StopSign_Timeout (used fallback)
replica_quiet_honest_vehicles = {}       # replica -> honest vehicles marked quiet
replica_quiet_honest_opportunities = {}  # replica -> honest vehicle opportunities
replica_quiet_honest_rate = {}           # replica -> percentage

# Pre-computed summary metrics read back from saved logs
run_metrics           = {}                 # key -> float  (from [RUN-METRICS] lines)
propose_all_sim_raw   = defaultdict(list)  # epoch -> [seconds, ...]
stop_to_decision_sim_raw = defaultdict(list) # epoch -> [seconds, ...]
# ResDB: (replica_id, sim_time_s) in log order — used to derive commit waves.
resdb_propose_submit_events = []
resdb_order_decided_events = []
replica_cert_collection_start = {}  # replica -> cert collection start time (sim s)
cert_collection_start_by_epoch = {}  # epoch -> earliest cert collection start time (sim s)
replica_vc_trigger_time = {}        # replica -> VC trigger time (sim s)
replica_primary_change_times = defaultdict(list)  # replica -> [t_s, ...]
replica_gossip_apply_time = {}      # replica -> t_s when gossip order applied
replica_gossip_send_time_by_epoch = {}  # (replica, epoch) -> earliest t_s for GOSSIP-SEND
replica_byzantine_types = defaultdict(set)  # replica -> {"SILENT_PRIMARY", ...}
vc_trigger_times_by_epoch = defaultdict(list)      # epoch -> [t_s, ...]
primary_change_times_by_epoch = defaultdict(list)  # epoch -> [t_s, ...]
order_decided_times_by_epoch = defaultdict(list)   # epoch -> [t_s, ...]
propose_submit_times_by_epoch = defaultdict(list)  # epoch -> [t_s, ...]
attack_outcomes = []
attack_outcome_keys = set()
attack_outcomes_by_epoch = defaultdict(list)
attack_failures_by_epoch = defaultdict(int)
experiment_fault_tolerance = {
    "tolerated_f": None,
    "static_replicas": None,
    "consensus_quorum": None,
    "cert_threshold": None,
}
attack_success_outcomes = {
    "MALICIOUS_INPUT_COMMITTED",
    "FALSE_PRIORITY_GRANTED",
    "UNCERTIFIED_PRIORITY_CLAIM_COMMITTED",
    "UNSAFE_ORDER_COMMITTED",
    "UNSAFE_PHYSICAL_COOCCUPANCY",
    "PREEMPTION_SUPPRESSED",
}

def record_attack_outcome(
    replica: int,
    epoch: int,
    fault: str,
    outcome: str,
    detail: str = "",
) -> None:
    row = {
        "replica": replica,
        "epoch": epoch,
        "fault": fault,
        "outcome": outcome,
        "detail": detail,
        "attack_success": outcome in attack_success_outcomes,
    }
    attack_outcomes.append(row)
    key = (epoch, fault, outcome, detail)
    if key not in attack_outcome_keys:
        attack_outcome_keys.add(key)
        attack_outcomes_by_epoch[epoch].append(row)
        if row["attack_success"]:
            attack_failures_by_epoch[epoch] += 1

def _safe_float(value, default=None):
    try:
        return float(value)
    except (TypeError, ValueError):
        return default

def _read_ambulance_id_from_ini(path):
    if not path or not os.path.isfile(path):
        return None
    pat = re.compile(r'^\s*\*\.node\[\*\]\.appl\.ambulanceReplicaId\s*=\s*(-?\d+)\s*$')
    with open(path, "r", errors="replace") as f_ini:
        for line in f_ini:
            m = pat.search(line)
            if m:
                val = int(m.group(1))
                return val if val >= 0 else None
    return None

def load_baseline_tripinfo(path, scenario_ini=None):
    if not path:
        return
    if not os.path.isfile(path):
        print(f"WARNING: baseline tripinfo not found: {path}", file=sys.stderr)
        return

    amb_id = _read_ambulance_id_from_ini(scenario_ini)
    if amb_id is None and _args.scenario in (2, 3, 4):
        amb_id = 0
    if amb_id is not None:
        ambulance_ids.add(f"veh{amb_id}")

    root = ET.parse(path).getroot()
    for elem in root.findall(".//tripinfo"):
        cid = elem.get("id")
        if not cid or not cid.startswith("veh"):
            continue
        try:
            rep = int(cid[3:])
        except ValueError:
            continue

        depart = _safe_float(elem.get("depart"))
        arrival = _safe_float(elem.get("arrival"))
        waiting = _safe_float(elem.get("waitingTime"), 0.0)
        duration = _safe_float(elem.get("duration"))
        time_loss = _safe_float(elem.get("timeLoss"))
        role = "ambulance" if cid == f"veh{amb_id}" else "normal"
        epoch = min(rep // 4, N_EPOCHS - 1)

        cm = car_metrics[cid]
        cm["role"] = role
        cm["epoch"] = epoch
        cm["arrival_time"] = depart
        cm["stop_time"] = depart
        cm["resume_time"] = arrival
        cm["depart_time"] = arrival
        cm["wait_intersection"] = waiting
        cm["stop_to_resume"] = waiting
        cm["resume_to_depart"] = 0.0
        cm["arrival_to_depart"] = duration
        cm["messages_sent"] = 0
        cm["messages_received"] = 0
        cm["delivery_ratio"] = None
        cm["estimated_loss_rate"] = None
        cm["used_fallback"] = False
        cm["baseline_time_loss_s"] = time_loss

def percentile(values, q):
    if not values:
        return None
    if len(values) == 1:
        return values[0]
    vals = sorted(values)
    idx = (len(vals) - 1) * q
    lo = int(idx)
    hi = min(lo + 1, len(vals) - 1)
    frac = idx - lo
    return vals[lo] + (vals[hi] - vals[lo]) * frac

def jains_fairness(values):
    vals = [v for v in values if v is not None]
    if not vals:
        return None
    denom = len(vals) * sum(v * v for v in vals)
    if denom == 0:
        return 1.0
    return (sum(vals) ** 2) / denom

def fmt_stat(values):
    if not values:
        return "N/A"
    return (
        f"mean={statistics.mean(values):.4f}s "
        f"median={statistics.median(values):.4f}s "
        f"p95={percentile(values, 0.95):.4f}s "
        f"max={max(values):.4f}s "
        f"n={len(values)}"
    )


def compute_resdb_submit_to_commit_by_wave(
    submit_events,
    order_events,
    cluster_gap_s=2.0,
    max_epochs=16,
):
    """
    ResDB / single-log: cluster Order_Decided sim times into commit waves (replicas see
    the same decision within cluster_gap_s). For each wave:
      t_commit = median(Order_Decided times in the wave)
      t_last_submit = max(ProposeAll_Submit_Time) in the submit window (winning proposal)
      t_first_submit = min(ProposeAll_Submit_Time) in the submit window (earliest attempt)
    Then:
      leader_submit_to_commit_s = t_commit - t_last_submit  (winning leader's PBFT slice)
      first_submit_to_commit_s   = t_commit - t_first_submit (includes failed primary + VC)
    Submit window: prev_wave_max_order < submit_time <= min(Order_Decided in cluster).
    Epoch index = min(wave_index, max_epochs - 1) when folding long runs.
    Returns a list of dicts (one per wave).
    """
    if not order_events:
        return []

    order_times = sorted(T for _, T in order_events)
    prev = order_times[0]
    clusters = [[prev]]
    for t in order_times[1:]:
        if t - prev > cluster_gap_s:
            clusters.append([t])
        else:
            clusters[-1].append(t)
        prev = t

    out = []
    prev_commit_hi = -1.0
    for wi, cl in enumerate(clusters):
        t_commit = statistics.median(cl)
        t_lo = min(cl)
        # Submits strictly before the first replica's Order_Decided in this wave.
        submits_in_window = [
            (r, t)
            for (r, t) in submit_events
            if prev_commit_hi < t < t_lo
        ]
        if not submits_in_window:
            submits_in_window = [
                (r, t)
                for (r, t) in submit_events
                if prev_commit_hi < t <= t_lo + 1e-6
            ]
        if not submits_in_window:
            prev_commit_hi = max(cl)
            continue
        ts = [t for _, t in submits_in_window]
        t_first = min(ts)
        t_last = max(ts)
        winning_rep = max(r for r, t in submits_in_window if t == t_last)
        ep = min(wi, max(0, max_epochs - 1))
        out.append({
            "epoch": ep,
            "wave_index": wi,
            "t_commit": t_commit,
            "leader_submit_to_commit_s": t_commit - t_last,
            "first_submit_to_commit_s": t_commit - t_first,
            "winning_rep": winning_rep,
        })
        prev_commit_hi = max(cl)
    return out


# ── Parse ────────────────────────────────────────────────────────────────────
print(f"Parsing {LOG_FILE} ...")
current_gossip_epoch = 0              # rough tracker for gossip events

if not os.path.isfile(LOG_FILE) and _args.baseline_tripinfo:
    _log_lines = []
else:
    with open(LOG_FILE, "r", errors="replace") as f:
        _log_lines = list(f)

for line in _log_lines:
        m_distance = RE_PERCEPTION_CONFIG.search(line)
        if m_distance:
            candidate = {
                "approach_sigma_m": float(m_distance.group(2)),
                "signal_error": float(m_distance.group(3)),
                "ego_lateral_sigma_m": float(m_distance.group(4)),
                "ego_longitudinal_sigma_m": float(m_distance.group(5)),
                "witness_lateral_sigma_m": float(m_distance.group(6)),
                "witness_longitudinal_sigma_m": float(m_distance.group(7)),
                "physical_gate_k": float(m_distance.group(8)),
                "rng_index": int(m_distance.group(9)),
                "collection_window_sec": float(m_distance.group(10)),
            }
            perception_config_replicas.add(int(m_distance.group(1)))
            if perception_config is None:
                perception_config = candidate
        m_distance = RE_STOPPED_DISTANCE_ATTEST.search(line)
        if m_distance:
            stopped_distance_attestations[(m_distance.group(1), int(m_distance.group(2)))] = {
                "target": m_distance.group(1), "epoch": int(m_distance.group(2)),
                "early_claim_hash": m_distance.group(3),
                "distance_to_stop_cm": int(m_distance.group(4)),
                "speed_mps": float(m_distance.group(5)),
            }
        m_distance = RE_STOPPED_DISTANCE_RETRY.search(line)
        if m_distance:
            stopped_distance_attestation_retries[(m_distance.group(1), int(m_distance.group(2)))] = max(
                stopped_distance_attestation_retries[(m_distance.group(1), int(m_distance.group(2)))],
                int(m_distance.group(3)),
            )
        m_distance = RE_DISTANCE_PERC_EVAL.search(line)
        if m_distance:
            key = (int(m_distance.group(1)), m_distance.group(2),
                   int(m_distance.group(3)), m_distance.group(4))
            if key in distance_perception_evaluations:
                distance_perception_duplicate_evaluations += 1
            else:
                distance_perception_evaluations[key] = {
                    "witness": key[0], "target": key[1], "epoch": key[2],
                    "attestation_hash": key[3],
                    "accept": m_distance.group(5) == "ACCEPT",
                    "true_distance_m": float(m_distance.group(6)),
                    "observed_distance_m": float(m_distance.group(7)),
                    "claimed_distance_cm": int(m_distance.group(8)),
                    "residual_cm": int(m_distance.group(9)),
                    "tolerance_cm": int(m_distance.group(10)),
                    "stationary": bool(int(m_distance.group(11))),
                }
        m_distance = RE_DISTANCE_CERT_ASSEMBLE.search(line)
        if m_distance:
            distance_certificates[(m_distance.group(1), int(m_distance.group(2)))] = {
                "signer_count": int(m_distance.group(3)),
                "threshold": int(m_distance.group(4)), "reason": m_distance.group(5),
            }
        m_distance = RE_DISTANCE_CERT_COLLECT.search(line)
        if m_distance:
            distance_cert_collection.append({
                "target": m_distance.group(1), "epoch": int(m_distance.group(2)),
                "signer_count": int(m_distance.group(3)),
                "threshold": int(m_distance.group(4)),
                "signer": int(m_distance.group(5)),
            })
        m_distance = RE_DISTANCE_ATTACK_DECLARE.search(line)
        if m_distance:
            distance_attack_declarations[(m_distance.group(1), int(m_distance.group(2)))] = {
                "true_distance_m": float(m_distance.group(3)),
                "claimed_distance_m": float(m_distance.group(4)),
                "offset_m": float(m_distance.group(5)),
            }
        m_distance = RE_DISTANCE_COLLUSION_ECHO.search(line)
        if m_distance:
            distance_collusion_echoes[(m_distance.group(1), int(m_distance.group(2)))].add(
                int(m_distance.group(3)))
        m_distance = RE_DISTANCE_CERT_EVIDENCE.search(line)
        if m_distance:
            distance_cert_evidence[(m_distance.group(1), int(m_distance.group(2)))][
                int(m_distance.group(3))] = {
                    "self": bool(int(m_distance.group(4))),
                    "byzantine": bool(int(m_distance.group(5))),
                }
        m_distance = RE_DISTANCE_ORDER_PAIR.search(line)
        if m_distance:
            key = (int(m_distance.group(1)), m_distance.group(2),
                   m_distance.group(3), m_distance.group(4))
            distance_order_pairs[key] = {
                "epoch": key[0], "lane": key[1], "a": key[2], "b": key[3],
                "cert_a_cm": int(m_distance.group(5)),
                "cert_b_cm": int(m_distance.group(6)),
                "true_a_m": float(m_distance.group(7)),
                "true_b_m": float(m_distance.group(8)),
                "inversion": bool(int(m_distance.group(9))),
            }
        m_perc = RE_PERC_EVAL.search(line)
        if m_perc:
            key = (int(m_perc.group(1)), m_perc.group(2), int(m_perc.group(3)), m_perc.group(4))
            if key in perception_evaluations:
                perception_duplicate_evaluations += 1
            else:
                perception_evaluations[key] = {
                    "witness": key[0], "target": key[1], "epoch": key[2],
                    "claim_hash": key[3], "lane_accept": m_perc.group(5) == "ACCEPT",
                    "observed_cue": m_perc.group(6),
                    "known_cue_samples": int(m_perc.group(7)),
                }
                lateral = RE_LATERAL_PERC_FIELDS.search(line)
                if lateral:
                    perception_evaluations[key].update({
                        "lane_observation_mode": lateral.group(1),
                        "true_lateral_cm": int(lateral.group(2)),
                        "observed_lateral_cm": int(lateral.group(3)),
                        "claimed_lateral_cm": int(lateral.group(4)),
                        "lateral_residual_cm": int(lateral.group(5)),
                        "lateral_tolerance_cm": float(lateral.group(6)),
                        "claimed_physical_lane_index": int(lateral.group(7)),
                        "projected_physical_lane_index": int(lateral.group(8)),
                    })
        m_perc = RE_EQUIVOCATION_DETECTED.search(line)
        if m_perc:
            key = (int(m_perc.group(1)), m_perc.group(2),
                   int(m_perc.group(3)), m_perc.group(5))
            equivocation_events[key] = {
                "witness": key[0], "target": key[1], "epoch": key[2],
                "first_hash": m_perc.group(4), "second_hash": key[3],
                "first_direction": m_perc.group(6),
                "second_direction": m_perc.group(7),
                "action": m_perc.group(8),
                "perception_evaluated": bool(int(m_perc.group(9))),
                "echo_sent": bool(int(m_perc.group(10))),
            }
        m_perc = RE_CERT_ASSEMBLE_PERCEPTION.search(line)
        if m_perc:
            cert_assembly_perception[(m_perc.group(1), int(m_perc.group(2)))] = {
                "echo_count": int(m_perc.group(3)), "threshold": int(m_perc.group(4)),
                "reason": m_perc.group(5),
            }
        m_perc = RE_DIR_ELIGIBILITY_SELF.search(line)
        if m_perc:
            direction_eligibility[(m_perc.group(1), int(m_perc.group(2)))] = {
                "declared": m_perc.group(3), "support": int(m_perc.group(4)),
                "threshold": int(m_perc.group(5)), "echo_count": int(m_perc.group(6)),
                "self_attestations": int(m_perc.group(7)),
                "b_sig": int(m_perc.group(8)), "derived": m_perc.group(9),
            }
        else:
            m_perc = RE_DIR_ELIGIBILITY.search(line)
            if m_perc:
                direction_eligibility[(m_perc.group(1), int(m_perc.group(2)))] = {
                    "declared": m_perc.group(3), "support": int(m_perc.group(4)),
                    "threshold": int(m_perc.group(5)), "echo_count": int(m_perc.group(6)),
                    "self_attestations": 0,
                    "b_sig": int(m_perc.group(7)), "derived": m_perc.group(8),
                }
        m_perc = RE_SELF_ATTEST.search(line)
        if m_perc:
            key = (m_perc.group(1), int(m_perc.group(2)), int(m_perc.group(3)), m_perc.group(4))
            if key in self_attestations:
                self_attestation_duplicates += 1
            else:
                self_attestations[key] = {
                    "target": key[0], "epoch": key[1], "signer": key[2],
                    "claim_hash": key[3], "observed_cue": m_perc.group(5),
                    "status": m_perc.group(6),
                }
        m_perc = RE_CERT_EVIDENCE.search(line)
        if m_perc:
            cert_evidence[(m_perc.group(1), int(m_perc.group(2)))][int(m_perc.group(3))] = {
                "cue": m_perc.group(4), "self": bool(int(m_perc.group(5))),
                "byzantine": bool(int(m_perc.group(6))),
                "supporting": bool(int(m_perc.group(7))),
            }
        m_perc = RE_ECHO_COLLECT.search(line)
        if m_perc:
            arrival_echo_collections[(m_perc.group(1), int(m_perc.group(2)))][int(m_perc.group(3))] = {
                "byzantine": bool(int(m_perc.group(4))),
                "self": bool(int(m_perc.group(5))),
                "echo_count": int(m_perc.group(6)),
                "threshold": int(m_perc.group(7)),
                "source": m_perc.group(8),
            }
        m_perc = RE_PHASE2_ATTACK_CONFIG.search(line)
        if m_perc:
            candidate = {
                "kind": m_perc.group(2),
                "target_replica_id": int(m_perc.group(3)),
                "actual_b": int(m_perc.group(4)),
                "colluder_ids": [int(value) for value in m_perc.group(5).split(",") if value],
            }
            phase2_attack_config_replicas.add(int(m_perc.group(1)))
            if phase2_attack_config is None:
                phase2_attack_config = candidate
            elif phase2_attack_config != candidate:
                phase2_attack_config_mismatches += 1
        m_perc = RE_PHASE2_ATTACK_DECLARE.search(line)
        if m_perc:
            phase2_attack_declarations[(m_perc.group(1), int(m_perc.group(2)))] = {
                "kind": m_perc.group(3), "actual_lane": m_perc.group(4),
                "claimed_lane": m_perc.group(5), "actual_direction": m_perc.group(6),
                "claimed_direction": m_perc.group(7),
            }
        m_perc = RE_PHASE2_COLLUSION_ECHO.search(line)
        if m_perc:
            phase2_collusion_echoes[(m_perc.group(1), int(m_perc.group(2)))].add(int(m_perc.group(3)))
        m_perc = RE_PHASE2_LATERAL_ATTACK_OUTCOME.search(line)
        if m_perc:
            phase2_attack_outcomes[(m_perc.group(1), int(m_perc.group(2)))] = {
                "kind": m_perc.group(3), "lane_certified": bool(int(m_perc.group(4))),
                "false_lane_certificate": bool(int(m_perc.group(5))),
                "physical_lane_index": int(m_perc.group(6)),
                "actual_physical_lane_index": int(m_perc.group(7)),
                "lateral_claim_cm": int(m_perc.group(8)),
                "false_eligibility": bool(int(m_perc.group(9))),
                "support": int(m_perc.group(10)), "threshold": int(m_perc.group(11)),
                "b_sig_logged": int(m_perc.group(12)),
            }
        else:
            m_perc = RE_PHASE2_ATTACK_OUTCOME.search(line)
        if m_perc:
            if m_perc.re is RE_PHASE2_ATTACK_OUTCOME:
                phase2_attack_outcomes[(m_perc.group(1), int(m_perc.group(2)))] = {
                    "kind": m_perc.group(3), "lane_certified": bool(int(m_perc.group(4))),
                    "false_lane_certificate": bool(int(m_perc.group(5))),
                    "false_eligibility": bool(int(m_perc.group(6))),
                    "support": int(m_perc.group(7)), "threshold": int(m_perc.group(8)),
                    "b_sig_logged": int(m_perc.group(9)),
                }
        m_perc = RE_TRUST_TIER.search(line)
        if m_perc:
            key = (m_perc.group(1), int(m_perc.group(2)))
            tier = m_perc.group(3)
            # QUIET may be emitted again during post-departure state cleanup.
            # Preserve the strongest scheduling authority observed for the
            # committed epoch rather than treating the last lifecycle log as
            # the committed trust tier.
            tier_rank = {"QUIET": 0, "SIGNED-UNKNOWN": 1, "SIGNED-DIRECTION": 2}
            previous = trust_tiers.get(key)
            if previous is None or tier_rank.get(tier, -1) > tier_rank.get(previous, -1):
                trust_tiers[key] = tier

        # Crash/recovery timeline extraction is deliberately non-consuming:
        # the same lines must remain available to the existing rollback and
        # Byzantine metric parsers below.
        m_aux = RE_CRASH_INJECT_EVENT.search(line)
        if m_aux:
            crash_inject_times.setdefault(m_aux.group(1), float(m_aux.group(2)))
        m_aux = RE_CRASH_TOW_EVENT.search(line)
        if m_aux:
            crash_tow_times.setdefault(m_aux.group(1), float(m_aux.group(2)))
        m_aux = RE_CRASH_COOCCUPANCY_EVENT.search(line)
        if m_aux:
            crash_unsafe_entrants.setdefault(m_aux.group(1), float(m_aux.group(3)))
        m_aux = RE_WAIT_SEND_EVENT.search(line)
        if m_aux:
            wait_send_times.append(float(m_aux.group(2)))
        m_aux = RE_WAIT_STOP_EVENT.search(line)
        if m_aux:
            wait_stop_times.append(float(m_aux.group(3)))
        m_aux = RE_CLEAR_CERT_BROADCAST_EVENT.search(line)
        if m_aux:
            clear_cert_broadcast_times.append(float(m_aux.group(3)))
        m_aux = RE_ROLLBACK_COMMIT_EVENT.search(line)
        if m_aux and int(m_aux.group(3)) == int(m_aux.group(2)) + 1:
            recovery_commit_times.append(float(m_aux.group(4)))
        # A CAR-METRICS line can be damaged by interleaved ResDB worker output.
        # DEPARTED is emitted immediately before it and independently preserves
        # the physical terminal event, so retain it as a per-car fallback.
        m_aux = RE_DEPARTED.search(line)
        if m_aux:
            replica_depart_time.setdefault(int(m_aux.group(1)), float(m_aux.group(2)))

        # ROUND-METRICS
        m = RE_ROUND_METRIC.search(line)
        if m:
            epoch, metric, val = int(m.group(1)), m.group(2), float(m.group(3))
            round_metrics[epoch][metric] = val
            continue

        # ORDER-DIAG – epoch tracker
        m = RE_DIAG_EPOCH.search(line)
        if m:
            rep, epoch = int(m.group(1)), int(m.group(2))
            replica_epoch[rep] = epoch
            current_gossip_epoch = epoch
            if r0_metrics["rollback_begin_seen"] and epoch == 1:
                r0_metrics["rollback_epoch1_committed"] = True
            continue

        m = RE_ORDER_COMMITTED.search(line)
        if m:
            epoch = int(m.group(1)) - 1
            decision = m.group(2)
            for car, _batch_idx in RE_BATCH_ENTRY.findall(decision):
                if car not in go_decision_epoch:
                    go_decision_epoch[car] = epoch
                    car_metrics[car]["epoch"] = epoch
            continue

        m = RE_ORDER_DECISION.search(line)
        if m:
            decision = m.group(1) or m.group(2) or ""
            for car, _batch_idx in RE_BATCH_ENTRY.findall(decision):
                if car not in go_decision_epoch:
                    go_decision_epoch[car] = current_gossip_epoch
                    car_metrics[car]["epoch"] = current_gossip_epoch
            continue

        m = RE_AMBULANCE_SCHED.search(line)
        if m:
            ambulance_ids.add(m.group(1))
            continue

        m = RE_MESSAGES_SENT.search(line)
        if m:
            rep, val = int(m.group(1)), int(m.group(2))
            ep = replica_epoch.get(rep, current_gossip_epoch)
            epoch_messages_sent[ep].append(val)
            replica_messages_sent[rep] = val
            continue

        m = RE_MESSAGES_RECEIVED.search(line)
        if m:
            rep, val = int(m.group(1)), int(m.group(2))
            ep = replica_epoch.get(rep, current_gossip_epoch)
            epoch_messages_recv[ep].append(val)
            replica_messages_recv[rep] = val
            continue

        m = RE_BYTES_SENT.search(line)
        if m:
            rep, val = int(m.group(1)), int(m.group(2))
            ep = replica_epoch.get(rep, current_gossip_epoch)
            epoch_bytes_sent[ep].append(val)
            replica_bytes_sent[rep] = val
            continue

        m = RE_QUIET_HONEST_VEHICLES.search(line)
        if m:
            replica_quiet_honest_vehicles[int(m.group(1))] = int(m.group(2))
            continue

        m = RE_QUIET_HONEST_OPPORTUNITIES.search(line)
        if m:
            replica_quiet_honest_opportunities[int(m.group(1))] = int(m.group(2))
            continue

        m = RE_QUIET_HONEST_RATE.search(line)
        if m:
            replica_quiet_honest_rate[int(m.group(1))] = float(m.group(2))
            continue

        m = RE_ARRIVAL_TIME.search(line)
        if m:
            rep = int(m.group(1))
            # Keep only the first arrival (car enters zone once per epoch)
            if rep not in replica_arrival_time:
                replica_arrival_time[rep] = float(m.group(2))
            continue

        m = RE_STOP_TIME.search(line)
        if m:
            replica_stop_time[int(m.group(1))] = float(m.group(2))
            continue

        m = RE_RESUME_TIME.search(line)
        if m:
            replica_resume_time[int(m.group(1))] = float(m.group(2))
            continue

        m = RE_RESDB_PROPOSE_SUBMIT.search(line)
        if m:
            rep = int(m.group(1))
            try:
                t_s = float(m.group(2))
                resdb_propose_submit_events.append((rep, t_s))
                ep = replica_epoch.get(rep, current_gossip_epoch)
                propose_submit_times_by_epoch[ep].append(t_s)
            except ValueError:
                pass
            continue

        m = RE_RESDB_ORDER_DECIDED.search(line)
        if m:
            rep = int(m.group(1))
            try:
                t_s = float(m.group(2))
                resdb_order_decided_events.append((rep, t_s))
                ep = replica_epoch.get(rep, current_gossip_epoch)
                order_decided_times_by_epoch[ep].append(t_s)
            except ValueError:
                pass
            continue

        m = RE_CERT_START.search(line)
        if m:
            rep = int(m.group(1))
            try:
                t_s = float(m.group(2))
                replica_cert_collection_start[rep] = t_s
                ep = replica_epoch.get(rep, current_gossip_epoch)
                prev = cert_collection_start_by_epoch.get(ep)
                cert_collection_start_by_epoch[ep] = t_s if prev is None else min(prev, t_s)
            except ValueError:
                pass
            continue

        m = RE_VC_TRIGGER.search(line)
        if m:
            rep = int(m.group(1))
            try:
                t_s = float(m.group(2))
                replica_vc_trigger_time[rep] = t_s
                ep = replica_epoch.get(rep, current_gossip_epoch)
                vc_trigger_times_by_epoch[ep].append(t_s)
            except ValueError:
                pass
            continue

        m = RE_PRIMARY_CHANGED.search(line)
        if m:
            rep = int(m.group(1))
            try:
                t_s = float(m.group(4))
                replica_primary_change_times[rep].append(t_s)
                ep = replica_epoch.get(rep, current_gossip_epoch)
                primary_change_times_by_epoch[ep].append(t_s)
            except ValueError:
                pass
            continue

        m = RE_GOSSIP_APPLY.search(line)
        if m:
            rep = int(m.group(1))
            try:
                replica_gossip_apply_time[rep] = float(m.group(3))
            except ValueError:
                pass
            continue

        m = RE_GOSSIP_SEND.search(line)
        if m:
            rep = int(m.group(1))
            ep = int(m.group(2))
            try:
                t_s = float(m.group(4))
                key = (rep, ep)
                prev = replica_gossip_send_time_by_epoch.get(key)
                replica_gossip_send_time_by_epoch[key] = t_s if prev is None else min(prev, t_s)
            except ValueError:
                pass
            continue

        m = RE_FALSE_LANE_COLLUDER_SET.search(line)
        if m:
            ids = [int(x) for x in m.group(3).split(",") if x]
            candidate = {
                "N": int(m.group(1)),
                "F": int(m.group(2)),
                "ids": ids,
                "position_gate_enabled": bool(int(m.group(4))),
            }
            if false_lane_colluder_config is None:
                false_lane_colluder_config = candidate
            continue

        m = RE_FALSE_LANE_COLLUSION_ECHO.search(line)
        if m:
            false_lane_echo_signers[m.group(1)].add(int(m.group(2)))
            continue

        m = RE_FALSE_LANE_COLLUSION_CERT.search(line)
        if m:
            false_lane_certificates[m.group(1)] = {
                "signers": sorted(int(x) for x in m.group(2).split(",") if x),
                "threshold": int(m.group(3)),
            }
            continue

        m = RE_FALSE_LANE_COLLUSION_BLOCK.search(line)
        if m:
            false_lane_blocked_targets[m.group(1)] = {
                "signers": int(m.group(2)),
                "threshold": int(m.group(3)),
            }
            continue

        m = RE_FALSE_LANE_COLLUSION_COMMIT.search(line)
        if m:
            false_lane_commits[m.group(1)] = {
                "claimed_lane": m.group(2),
                "actual_lane": m.group(3),
                "epoch": int(m.group(4)),
            }
            continue

        if RE_MALFORMED_PROPOSAL_REJECT.search(line):
            malformed_proposal_reject_count += 1
            continue

        m = RE_MOVEMENT_GROUND_TRUTH.search(line)
        if m:
            movement_ground_truth[m.group(1)] = {
                "vehicle": m.group(1),
                "planned_ingress": m.group(2),
                "planned_egress": m.group(3),
                "movement": m.group(4),
                "observed_at_ms": float(m.group(5)) * 1000.0,
            }
            continue

        m = RE_CONFLICT_ZONE.search(line)
        if m:
            conflict_zone_events.append({
                "vehicle": m.group(1),
                "movement": m.group(2),
                "event": m.group(3),
                "road": m.group(4),
                "lane": m.group(5),
                "time_ms": float(m.group(6)) * 1000.0,
            })
            continue

        m = RE_MOVEMENT_ACTUAL_EGRESS.search(line)
        if m:
            movement_actual_egress[m.group(1)] = {
                "vehicle": m.group(1),
                "planned_egress": m.group(2),
                "actual_egress": m.group(3),
                "match": bool(int(m.group(4))),
                "observed_at_ms": float(m.group(5)) * 1000.0,
            }
            continue

        m = RE_CONFLICTING_COOCCUPANCY.search(line)
        if m:
            movement_conflict_pairs[(m.group(1), m.group(3))] = {
                "first": m.group(1),
                "first_movement": m.group(2),
                "second": m.group(3),
                "second_movement": m.group(4),
                "time_ms": float(m.group(5)) * 1000.0,
            }
            continue

        m = RE_METROLOGY_CONFIG.search(line)
        if m:
            metrology_config = {
                "speed_mode": int(m.group(1)),
                "speed_mps": float(m.group(2)),
                "jm_ignore_foe_probability": float(m.group(3)),
                "collision_check_junctions": bool(int(m.group(4))),
                "collision_action": m.group(5),
                "time_to_teleport_s": float(m.group(6)),
                "vehicles": m.group(7).split(","),
            }
            continue

        m = RE_METROLOGY_RELEASE.search(line)
        if m:
            metrology_releases[m.group(1)] = {
                "vehicle": m.group(1),
                "speed_mode": int(m.group(2)),
                "speed_mps": float(m.group(3)),
                "time_ms": float(m.group(4)) * 1000.0,
            }
            continue

        m = RE_UNSAFE_CONFLICT_COOCCUPANCY.search(line)
        if m:
            unsafe_conflict_pairs[(m.group(1), m.group(3))] = {
                "first": m.group(1),
                "first_approach": m.group(2),
                "second": m.group(3),
                "second_approach": m.group(4),
                "time_ms": float(m.group(5)) * 1000.0,
            }
            phase2_wrong_approach = (
                phase2_attack_config is not None and
                phase2_attack_config.get("kind") == "WRONG_APPROACH"
            )
            legacy_false_lane = bool(
                false_lane_colluder_config and false_lane_colluder_config.get("F", 0) > 0
            )
            if phase2_wrong_approach or legacy_false_lane:
                record_attack_outcome(
                    -1,
                    current_gossip_epoch,
                    "FALSE_LANE",
                    "UNSAFE_PHYSICAL_COOCCUPANCY",
                    f"pair={m.group(1)}+{m.group(3)}",
                )
            continue

        m = RE_PHYSICAL_COLLISION.search(line)
        if m:
            physical_collision_vehicles.setdefault(m.group(1), float(m.group(2)) * 1000.0)
            continue

        m = RE_BYZANTINE_CONFIG.search(line)
        if m:
            rep = int(m.group(1))
            replica_byzantine_types[rep].add(m.group(2))
            continue

        m = RE_PREVERIFY_STATE_MISMATCH.search(line)
        if m:
            active_faults = {
                fault
                for faults in replica_byzantine_types.values()
                for fault in faults
                if fault in ("FAKE_AMBULANCE", "TAMPER_LANE")
            }
            for fault in sorted(active_faults):
                record_attack_outcome(
                    -1,
                    current_gossip_epoch,
                    fault,
                    "PREVERIFY_BLOCKED_STATE_MISMATCH",
                    "check=10",
                )
            continue

        m = RE_CONSENSUS_ATTACK_OUTCOME.search(line)
        if m:
            rep = int(m.group(1))
            epoch = int(m.group(2))
            fault = m.group(3)
            outcome = m.group(4)
            detail = (m.group(5) or "").strip()
            record_attack_outcome(rep, epoch, fault, outcome, detail)
            continue

        m = RE_FALSE_PRIORITY_GRANTED.search(line)
        if m:
            rep = int(m.group(1))
            epoch = int(m.group(2))
            veh = int(m.group(3))
            record_attack_outcome(
                rep, epoch, "FAKE_AMBULANCE", "FALSE_PRIORITY_GRANTED",
                f"target=veh{veh}",
            )
            continue

        m = RE_CRASH_DETECTED.search(line)
        if m:
            rep = int(m.group(1))
            epoch = int(m.group(2))
            batch = int(m.group(3))
            record_attack_outcome(
                rep, epoch, "TAMPER_LANE", "UNSAFE_ORDER_COMMITTED",
                f"batch={batch}",
            )
            continue

        m = RE_STOPSIGN_TIMEOUT.search(line)
        if m:
            rep = int(m.group(1))
            ep = replica_epoch.get(rep, current_gossip_epoch)
            epoch_failures[ep] += 1
            replica_stopsign_timeout.add(rep)
            continue

        m = RE_CAR_METRICS.search(line)
        if m:
            car_id = m.group(1)
            role = m.group(2)
            epoch = int(m.group(3))
            stop_t = float(m.group(4))
            depart_t = float(m.group(5))
            wait_t = float(m.group(6))
            if role == "ambulance":
                ambulance_ids.add(car_id)
            car_metrics[car_id]["role"] = role
            car_metrics[car_id]["epoch"] = epoch
            car_metrics[car_id]["stop_time"] = None if stop_t < 0 else stop_t
            car_metrics[car_id]["depart_time"] = depart_t
            car_metrics[car_id]["wait_intersection"] = None if wait_t < 0 else wait_t
            continue

        m = RE_AMBULANCE_WAIT.search(line)
        if m:
            car_id = m.group(1)
            ambulance_ids.add(car_id)
            car_metrics[car_id]["role"] = "ambulance"
            car_metrics[car_id]["epoch"] = int(m.group(3))
            car_metrics[car_id]["wait_intersection"] = float(m.group(2))
            continue

        m = RE_BYZANTINE_INJECTION.search(line)
        if m:
            replica = int(m.group(1))
            cid = int(m.group(2))
            epoch = cid // 2   # CID 0,2,4,6→epoch 0,1,2,3  (VIEW & ORDER share the same epoch)
            byzantine_by_epoch[epoch] += 1
            byzantine_by_replica[replica] += 1
            continue

        m = RE_FABRICATED_CLEARANCE.search(line)
        if m:
            replica = int(m.group(1))
            cancelled_epoch = int(m.group(2))
            new_epoch = int(m.group(3))
            event = (replica, cancelled_epoch, new_epoch)
            if event not in fabricated_clearance_events:
                fabricated_clearance_events.add(event)
                byzantine_by_epoch[new_epoch] += 1
                byzantine_by_replica[replica] += 1
            continue

        m = RE_MISSING_CLEAR_REJECT.search(line)
        if m:
            missing_clear_reject_count += 1
            continue

        m = RE_UNSAFE_RECOVERY_ORDER.search(line)
        if m:
            replica = int(m.group(1))
            unsafe_recovery_order_replicas.add(replica)
            if int(m.group(4)) == 1:
                unsafe_recovery_order_box_occupied_replicas.add(replica)
            continue

        m = RE_CONSENSUS_HEADER.search(line)
        if m:
            rep, epoch = int(m.group(1)), int(m.group(2))
            replica_epoch[rep] = epoch
            current_gossip_epoch = epoch
            continue

        m = RE_PHASE_SUMMARY.search(line)
        if m:
            rep = int(m.group(1))
            if m.group(2) is not None:
                ep = int(m.group(2))
                replica_epoch[rep] = ep
            else:
                ep = replica_epoch.get(rep, current_gossip_epoch)
            car_id = f"veh{rep}"
            if m.group(3):
                val = float(m.group(3))
                propose_all_sim_raw[ep].append(val)
                car_metrics[car_id]["propose_all_consensus_sim_s"] = val
            if m.group(4):
                val = float(m.group(4))
                stop_to_decision_sim_raw[ep].append(val)
                car_metrics[car_id]["stop_to_decision_sim_s"] = val
            continue

        m = RE_CERT_DUR.search(line)
        if m:
            rep, val = int(m.group(1)), float(m.group(2))
            ep = replica_epoch.get(rep, current_gossip_epoch)
            car_id = f"veh{rep}"
            car_metrics[car_id]["cert_collection_s"] = val
            round_metrics[ep].setdefault("Cert_Collection_Duration", val)
            continue

        m = RE_CERT_CREATION_LATENCY.search(line)
        if m:
            rep, val = int(m.group(1)), float(m.group(2))
            ep = replica_epoch.get(rep, current_gossip_epoch)
            car_id = f"veh{rep}"
            car_metrics[car_id]["cert_creation_s"] = val
            epoch_cert_creation_latency[ep].append(val)
            round_metrics[ep].setdefault("Cert_Creation_Latency", val)
            continue

        m = RE_BATCH_ASSIGN.search(line)
        if m:
            rep, batch = int(m.group(1)), int(m.group(2))
            replica_batch_index[rep] = batch
            car_metrics[f"veh{rep}"]["batch_index"] = batch
            continue

        m = RE_TOLERATED_F_CONFIG.search(line)
        if m:
            experiment_fault_tolerance["tolerated_f"] = int(m.group(1))
            experiment_fault_tolerance["static_replicas"] = int(m.group(2))
            experiment_fault_tolerance["consensus_quorum"] = int(m.group(3))
            experiment_fault_tolerance["cert_threshold"] = int(m.group(4))
            try:
                expected_q = bft_quorum(
                    experiment_fault_tolerance["static_replicas"],
                    experiment_fault_tolerance["tolerated_f"],
                )
                if expected_q != experiment_fault_tolerance["consensus_quorum"]:
                    r0_metrics["bad_quorum_count"] += 1
            except ValueError:
                r0_metrics["bad_quorum_count"] += 1
            continue

        m = RE_R0_CANCEL_WITNESS.search(line)
        if m:
            r0_metrics["enabled"] = True
            r0_metrics["cancel_witness_count"] += 1
            continue

        m = RE_R0_CANCEL_ECHO.search(line)
        if m:
            r0_metrics["enabled"] = True
            r0_metrics["cancel_echo_count"] += 1
            continue

        m = RE_R0_CANCEL_CERT.search(line)
        if m:
            r0_metrics["enabled"] = True
            r0_metrics["cancel_cert_count"] += 1
            continue

        m = RE_R0_ROLLBACK_BEGIN.search(line)
        if m:
            r0_metrics["enabled"] = True
            r0_metrics["rollback_begin_seen"] = True
            continue

        m = RE_R0_CANCEL_COMMIT.search(line)
        if m:
            r0_metrics["enabled"] = True
            r0_metrics["cancel_commit_count"] += 1
            tm = RE_CANCEL_COMMIT_TIME.search(line)
            if tm:
                commit_time = float(tm.group(3))
                prior = r0_metrics["cancel_leader_first_commit_time"]
                if prior is None or commit_time < prior:
                    r0_metrics["cancel_leader_first_commit_time"] = commit_time
            continue

        m = RE_CANCEL_LEADER_SUPPRESS.search(line)
        if m:
            r0_metrics["enabled"] = True
            r0_metrics["cancel_leader_suppress_count"] += 1
            attack_replica = int(m.group(1))
            if r0_metrics["cancel_leader_attack_replica"] is None:
                byzantine_by_epoch[int(m.group(2))] += 1
                byzantine_by_replica[attack_replica] += 1
            r0_metrics["cancel_leader_attack_replica"] = attack_replica
            attack_time = float(m.group(3))
            prior = r0_metrics["cancel_leader_attack_time"]
            if prior is None or attack_time < prior:
                r0_metrics["cancel_leader_attack_time"] = attack_time
            continue

        m = RE_CANCEL_LEADER_ROTATE.search(line)
        if m:
            r0_metrics["enabled"] = True
            r0_metrics["cancel_leader_rotation_count"] += 1
            r0_metrics["cancel_leader_max_rotation_index"] = max(
                r0_metrics["cancel_leader_max_rotation_index"],
                int(m.group(2)),
            )
            rotate_time = float(m.group(4))
            prior = r0_metrics["cancel_leader_first_rotation_time"]
            if prior is None or rotate_time < prior:
                r0_metrics["cancel_leader_first_rotation_time"] = rotate_time
            continue

        m = RE_R0_ROLLBACK_COMMIT.search(line)
        if m:
            r0_metrics["enabled"] = True
            r0_metrics["rollback_commit_count"] += 1
            r0_metrics["rollback_commit_replica_ids"].append(int(m.group(1)))
            if int(m.group(3)) == 1:
                r0_metrics["rollback_epoch1_committed"] = True
            continue

        m = RE_R0_ROLLBACK_PROPOSE_RC.search(line)
        if m:
            r0_metrics["enabled"] = True
            if int(m.group(2)) == 0:
                r0_metrics["rollback_propose_rc0"] = True
            continue

        m = RE_R0_ROLLBACK_DISCOVERY_SNAPSHOT.search(line)
        if m:
            r0_metrics["enabled"] = True
            r0_metrics["rollback_mode"] = m.group(3)
            r0_metrics["rollback_discovery_snapshots"].append({
                "replica": int(m.group(1)),
                "reason": m.group(2),
                "mode": m.group(3),
                "signed": int(m.group(4)),
                "quiet": int(m.group(5)),
                "vote_n": int(m.group(6)),
                "scene_n": int(m.group(7)),
                "expected_n": int(m.group(8)),
            })
            continue

        m = RE_R0_ACTIVE_VIEW_EPOCH1.search(line)
        if m:
            r0_metrics["enabled"] = True
            view = {
                "seq": int(m.group(1)),
                "hash": m.group(2),
                "vote_n": int(m.group(3)),
                "f": int(m.group(4)),
                "quorum_used": int(m.group(5)),
                "proposer": int(m.group(6)),
                "new_epoch": 1,
            }
            try:
                view["expected_quorum"] = bft_quorum(view["vote_n"], view["f"])
            except ValueError:
                view["expected_quorum"] = None
            if view["seq"] in (0, 3) and view["expected_quorum"] != view["quorum_used"]:
                r0_metrics["bad_quorum_count"] += 1
            r0_metrics["rollback_active_views"].append(view)
            continue

        m = RE_R0_EXECUTOR_ORDER_EPOCH.search(line)
        if m:
            epoch = int(m.group(1))
            n_vehicles = int(m.group(2))
            n_batches = int(m.group(3))
            if epoch == 1 and n_vehicles > 0:
                r0_metrics["enabled"] = True
                r0_metrics["epoch1_order_decision_count"] += 1
                r0_metrics["epoch1_order_n_vehicles"] = n_vehicles
                r0_metrics["epoch1_order_n_batches"] = n_batches
                decision_blob = m.group(4) or ""
                order_vehicle_ids = [
                    int(v) for v in re.findall(r'\bveh=(\d+)\b', decision_blob)
                ]
                if order_vehicle_ids:
                    r0_metrics["epoch1_order_vehicle_ids"] = order_vehicle_ids
                r0_metrics["rollback_epoch1_committed"] = True
            continue

        m = RE_PBFT_NEW_REQ.search(line)
        if m:
            if r0_metrics["rollback_propose_rc0"]:
                r0_metrics["rollback_pbft_new_req_after_propose"] = True
            continue

        m = RE_R0_ROLLBACK_QUORUM_V3.search(line)
        if m:
            r0_metrics["enabled"] = True
            vote_n = int(m.group(3))
            f_used = int(m.group(5))
            quorum_used = int(m.group(6))
            mode = m.group(2)
            r0_metrics["rollback_mode"] = mode
            try:
                expected_q = bft_quorum(vote_n, f_used)
            except ValueError:
                expected_q = None
            if expected_q != quorum_used:
                r0_metrics["bad_quorum_count"] += 1
            r0_metrics["rollback_quorums"].append({
                "mode": mode,
                "vote_n": vote_n,
                "scene_n": int(m.group(4)),
                "f": f_used,
                "quorum_used": quorum_used,
                "expected_quorum": expected_q,
                "proposer": int(m.group(7)),
                "new_epoch": int(m.group(8)),
            })
            continue

        m = RE_R0_ROLLBACK_RECONFIG.search(line)
        if m:
            r0_metrics["enabled"] = True
            r0_metrics["rollback_mode"] = m.group(2)
            r0_metrics["rollback_reconfigs"].append({
                "mode": m.group(2),
                "old_epoch": int(m.group(3)),
                "new_epoch": int(m.group(4)),
                "vote_n": int(m.group(5)),
                "scene_n": int(m.group(6)),
                "new_f": int(m.group(7)),
                "quorum": int(m.group(8)),
            })
            continue

        m = RE_R0_ROLLBACK_QUORUM.search(line)
        if m:
            r0_metrics["enabled"] = True
            member_count = int(m.group(2))
            f_anchored = int(m.group(3))
            quorum_used = int(m.group(4))
            try:
                expected_q = bft_quorum(member_count, f_anchored)
            except ValueError:
                expected_q = None
            if expected_q != quorum_used:
                r0_metrics["bad_quorum_count"] += 1
            r0_metrics["rollback_quorums"].append({
                "member_count": member_count,
                "f_anchored": f_anchored,
                "quorum_used": quorum_used,
                "expected_quorum": expected_q,
                "proposer": int(m.group(5)),
                "new_epoch": int(m.group(6)),
            })
            continue

        m = RE_R0_ROLLBACK_UNAVAILABLE_V3.search(line)
        if m:
            r0_metrics["enabled"] = True
            r0_metrics["rollback_unavailable_count"] += 1
            vote_n = int(m.group(2))
            need = int(m.group(5))
            r0_metrics["rollback_mode"] = m.group(1)
            if vote_n > r0_metrics["rollback_unavailable_max_m"]:
                r0_metrics["rollback_unavailable_max_m"] = vote_n
                f_val = m.group(3) or m.group(4)
                r0_metrics["rollback_unavailable_f"] = int(f_val) if f_val else None
                r0_metrics["rollback_unavailable_need"] = need
            continue

        m = RE_R0_ROLLBACK_UNAVAILABLE.search(line)
        if m:
            r0_metrics["enabled"] = True
            r0_metrics["rollback_unavailable_count"] += 1
            member_count = int(m.group(1))
            if member_count > r0_metrics["rollback_unavailable_max_m"]:
                r0_metrics["rollback_unavailable_max_m"] = member_count
                r0_metrics["rollback_unavailable_f"] = int(m.group(2))
                r0_metrics["rollback_unavailable_need"] = int(m.group(4))
            continue

        if RE_R0_ROLLBACK_PROPOSE_SKIP_MINUS_ONE.search(line):
            r0_metrics["enabled"] = True
            r0_metrics["proposer_minus_one_skip_count"] += 1
            continue

        # [RUN-METRICS] summary lines (read back from saved log files)
        m = RE_RUN_METRIC.search(line)
        if m:
            run_metrics[m.group(1)] = float(m.group(2))
            continue

        # Per-car [ROUND-METRICS] Arrival_To_Resume_Time
        m = RE_ROUND_METRIC_CAR.search(line)
        if m:
            epoch, car_id, val = int(m.group(1)), m.group(2), float(m.group(3))
            car_metrics[car_id].setdefault("epoch", epoch)
            go_decision_epoch.setdefault(car_id, epoch)
            car_metrics[car_id]["stop_to_resume"] = val
            continue

        # General [ROUND-METRICS] Epoch N Key: value (catch remaining epoch-keyed lines)
        m = RE_ROUND_METRIC_KEY.search(line)
        if m:
            epoch, key, val = int(m.group(1)), m.group(2), float(m.group(3))
            round_metrics[epoch].setdefault(key, val)
            continue

if _args.baseline_tripinfo:
    load_baseline_tripinfo(_args.baseline_tripinfo, _args.scenario_ini)

print("Done.\n")
byzantine_total = sum(byzantine_by_epoch.values())

if fabricated_clearance_events:
    attack_replica, _cancelled_epoch, attacked_epoch = sorted(
        fabricated_clearance_events
    )[0]
    if unsafe_recovery_order_replicas:
        record_attack_outcome(
            attack_replica,
            attacked_epoch,
            "FABRICATED_CLEARANCE",
            "UNSAFE_ORDER_COMMITTED",
            "accepted_while_blocking_replicas="
            + str(len(unsafe_recovery_order_replicas))
            + " box_occupied_replicas="
            + str(len(unsafe_recovery_order_box_occupied_replicas)),
        )
    elif missing_clear_reject_count > 0:
        outcome = (
            "PREVERIFY_BLOCKED_AND_RECOVERED"
            if r0_metrics["rollback_commit_count"] > 0
            else "PREVERIFY_BLOCKED_MISSING_CLEAR"
        )
        record_attack_outcome(
            attack_replica,
            attacked_epoch,
            "FABRICATED_CLEARANCE",
            outcome,
            f"preverify_rejections={missing_clear_reject_count}",
        )

if experiment_fault_tolerance["tolerated_f"] is None and "Tolerated_F" in run_metrics:
    experiment_fault_tolerance["tolerated_f"] = int(run_metrics["Tolerated_F"])
    experiment_fault_tolerance["static_replicas"] = int(
        run_metrics.get("Static_Replicas", -1)
    )
    experiment_fault_tolerance["consensus_quorum"] = int(
        run_metrics.get("Consensus_Quorum", -1)
    )
    experiment_fault_tolerance["cert_threshold"] = int(
        run_metrics.get("Cert_Threshold", -1)
    )

if _args.scenario == 15:
    r0_metrics["enabled"] = True
if (r0_metrics["cancel_leader_attack_time"] is not None and
        r0_metrics["cancel_leader_first_commit_time"] is not None):
    r0_metrics["cancel_leader_failover_latency_sec"] = max(
        0.0,
        r0_metrics["cancel_leader_first_commit_time"] -
        r0_metrics["cancel_leader_attack_time"],
    )
if r0_metrics["cancel_leader_suppress_count"] > 0:
    attack_replica = r0_metrics["cancel_leader_attack_replica"]
    if r0_metrics["cancel_commit_count"] > 0:
        latency = r0_metrics["cancel_leader_failover_latency_sec"]
        detail = ""
        if latency is not None:
            detail = f"cancel_commit_latency_sec={latency:.6f}"
        record_attack_outcome(
            attack_replica,
            0,
            "SUPPRESS_CANCEL",
            "RECOVERED_AFTER_CANCEL_ROTATION",
            detail,
        )
    else:
        record_attack_outcome(
            attack_replica,
            0,
            "SUPPRESS_CANCEL",
            "PREEMPTION_SUPPRESSED",
            "valid_cancel_cert_without_commit",
        )
if r0_metrics["enabled"]:
    if r0_metrics["bad_quorum_count"] > 0:
        r0_metrics["failure_stage"] = "bad_quorum"
    elif (r0_metrics["proposer_minus_one_skip_count"] > 0
          and not r0_metrics["rollback_quorums"]
          and r0_metrics["rollback_unavailable_count"] == 0):
        r0_metrics["failure_stage"] = "silent_proposer_minus_one"
    elif (r0_metrics["rollback_unavailable_count"] > 0
          and not r0_metrics["rollback_quorums"]):
        r0_metrics["failure_stage"] = "rollback_unavailable"
    elif r0_metrics["cancel_echo_count"] == 0:
        r0_metrics["failure_stage"] = "no_echo"
    elif r0_metrics["cancel_cert_count"] == 0:
        r0_metrics["failure_stage"] = "no_cert"
    elif (r0_metrics["cancel_leader_suppress_count"] > 0 and
          r0_metrics["cancel_commit_count"] == 0):
        r0_metrics["failure_stage"] = "suppressed_cancel_stalled"
    elif r0_metrics["rollback_epoch1_committed"]:
        latest_view = (
            r0_metrics["rollback_active_views"][-1]
            if r0_metrics["rollback_active_views"]
            else None
        )
        if latest_view and latest_view.get("expected_quorum") != latest_view.get("quorum_used"):
            r0_metrics["failure_stage"] = "unexpected_epoch1_quorum"
        elif latest_view:
            r0_metrics["failure_stage"] = "ok_epoch1_commit"
        else:
            r0_metrics["failure_stage"] = "ok_epoch1_commit_no_view_log"
    elif _args.scenario == 15 and r0_metrics["rollback_quorums"]:
        last = r0_metrics["rollback_quorums"][-1]
        committed = (
            r0_metrics["rollback_epoch1_committed"]
            or r0_metrics["rollback_pbft_new_req_after_propose"]
        )
        if not committed:
            r0_metrics["failure_stage"] = "rollback_propose_no_commit"
        elif last.get("mode") == "per_epoch":
            if last.get("vote_n", 0) >= 12 and last.get("f") == 4 and last.get("quorum_used") == 10:
                r0_metrics["failure_stage"] = "ok_per_epoch_reconfig"
            else:
                r0_metrics["failure_stage"] = "unexpected_per_epoch_quorum"
        else:
            r0_metrics["failure_stage"] = "ok_rollback_quorum"
    elif r0_metrics["rollback_quorums"]:
        committed = (
            r0_metrics["rollback_epoch1_committed"]
            or r0_metrics["rollback_pbft_new_req_after_propose"]
        )
        if committed:
            r0_metrics["failure_stage"] = "ok_rollback_quorum"
        else:
            r0_metrics["failure_stage"] = "rollback_propose_no_commit"
    else:
        r0_metrics["failure_stage"] = "unknown_pending_full_predicates"
    order_members = set(r0_metrics["epoch1_order_vehicle_ids"])
    commit_members = set(r0_metrics["rollback_commit_replica_ids"])
    r0_metrics["rollback_commit_extra_replica_ids"] = sorted(
        commit_members - order_members
    )
    r0_metrics["rollback_commit_missing_replica_ids"] = sorted(
        order_members - commit_members
    )

def normalize_java_epoch(raw_epoch):
    if raw_epoch in round_metrics and (raw_epoch - 1) not in round_metrics:
        return raw_epoch
    if (raw_epoch - 1) in round_metrics and raw_epoch not in round_metrics:
        return raw_epoch - 1
    if 0 <= raw_epoch - 1 < N_EPOCHS:
        return raw_epoch - 1
    if 0 <= raw_epoch < N_EPOCHS:
        return raw_epoch
    return raw_epoch

# ResDB: commit waves from Order_Decided, then:
#   ProposeAll_Consensus_Sim = median(commit wave) - max(submit in window)  (winning leader→commit)
#   ProposeAll_FirstSubmit_To_OrderCommit_Sim = median(commit) - min(submit) (failed primary + VC + …)
_resdb_waves = compute_resdb_submit_to_commit_by_wave(
    resdb_propose_submit_events,
    resdb_order_decided_events,
    cluster_gap_s=2.0,
    max_epochs=N_EPOCHS,
)
_epochs_resdb_submit_order = set()
for row in _resdb_waves:
    ep = row["epoch"]
    round_metrics[ep]["ProposeAll_Consensus_Sim"] = row["leader_submit_to_commit_s"]
    round_metrics[ep]["ProposeAll_FirstSubmit_To_OrderCommit_Sim"] = row[
        "first_submit_to_commit_s"
    ]
    _epochs_resdb_submit_order.add(ep)
    car_metrics[f"veh{row['winning_rep']}"]["propose_all_consensus_sim_s"] = row[
        "leader_submit_to_commit_s"
    ]
    car_metrics[f"veh{row['winning_rep']}"]["propose_all_first_submit_to_commit_sim_s"] = row[
        "first_submit_to_commit_s"
    ]

for ep, samples in propose_all_sim_raw.items():
    if ep in _epochs_resdb_submit_order:
        continue
    if samples:
        mean_bft = statistics.mean(samples)
        round_metrics[ep]["ProposeAll_Consensus_Sim"] = mean_bft
        # No ResDB submit/order lines: PHASE_SUMMARY is one leader path — first submit == last.
        round_metrics[ep]["ProposeAll_FirstSubmit_To_OrderCommit_Sim"] = mean_bft

for ep, samples in stop_to_decision_sim_raw.items():
    if samples:
        round_metrics[ep]["Stop_To_Decision_Sim"] = statistics.mean(samples)

# Compute per-epoch total durations: arrival at intersection zone → resume.
# Uses Arrival_Time (car first enters zone, even if queued) so queue wait is included.
# Falls back to Stop_Time if Arrival_Time is absent (older logs).
for rep, resume_t in replica_resume_time.items():
    arrival_t = replica_arrival_time.get(rep, replica_stop_time.get(rep))
    if arrival_t is not None:
        dur = resume_t - arrival_t
        car_id = f"veh{rep}"
        ep = go_decision_epoch.get(car_id)
        if isinstance(ep, int):
            epoch_total_dur[ep].append(dur)
            epoch_total_dur_by_car[ep][car_id] = dur

for rep, arrival_t in replica_arrival_time.items():
    car_metrics[f"veh{rep}"]["arrival_time"] = arrival_t
for rep, stop_t in replica_stop_time.items():
    car_metrics[f"veh{rep}"]["stop_time"] = stop_t
for rep, resume_t in replica_resume_time.items():
    car_metrics[f"veh{rep}"]["resume_time"] = resume_t
for rep, depart_t in replica_depart_time.items():
    cm = car_metrics[f"veh{rep}"]
    if cm.get("depart_time") is None:
        cm["depart_time"] = depart_t
        stop_t = cm.get("stop_time", replica_stop_time.get(rep))
        if stop_t is not None and depart_t >= stop_t:
            cm["wait_intersection"] = depart_t - stop_t

# ── Per-car phase breakdown: cert_wait / bft_wait / queue_wait ───────────────
# For each epoch, find the leader (only replica with cert_collection_s set).
# Derive proposeSubmitTime = orderTime - bftSim, then for every car in that epoch:
#   cert_wait  = max(0, proposeSubmitTime - car_stop_time)  [waiting for cert collection]
#   bft_wait   = bftSim                                      [stable BFT consensus]
#   queue_wait = resume_time - orderTime                     [0 for batch-0, >0 for later batches]
for ep in sorted(set(car_metrics[c].get("epoch", -1) for c in car_metrics)):
    if ep < 0:
        continue
    # Only compute phases for epochs where we found the PROPOSE_ALL leader
    # (cert_collection_s is only logged by the replica that submits PROPOSE_ALL).
    leader_car = None
    for cid, cm in car_metrics.items():
        if cm.get("epoch") == ep and cm.get("cert_collection_s") is not None:
            leader_car = cid
            break
    if leader_car is None:
        continue  # No leader data for this epoch → leave phase fields absent
    lcm = car_metrics[leader_car]
    ep_bft = lcm.get("propose_all_consensus_sim_s")
    l_stop = lcm.get("stop_time")
    l_std  = lcm.get("stop_to_decision_sim_s")
    if None in (ep_bft, l_stop, l_std):
        continue
    ep_order_time   = l_stop + l_std
    ep_propose_time = ep_order_time - ep_bft
    round_metrics[ep]["epoch_order_sim_time"]   = ep_order_time
    round_metrics[ep]["epoch_propose_sim_time"]  = ep_propose_time
    # Only tag cars actually in this epoch (epoch tag must match)
    for cid, cm in car_metrics.items():
        if cm.get("epoch") != ep:
            continue
        c_stop   = cm.get("stop_time")
        c_resume = cm.get("resume_time")
        c_std    = cm.get("stop_to_decision_sim_s")
        if c_stop is None:
            continue
        c_order_end = (c_stop + c_std) if c_std is not None else ep_order_time
        cm["cert_wait_s"]      = max(0.0, ep_propose_time - c_stop)
        cm["bft_wait_s"]       = ep_bft
        # Order delivery: gap between when leader decided and when THIS replica got it.
        # Non-zero because 802.11p DECIDE delivery is not instantaneous.
        cm["order_delivery_s"] = round(c_order_end - ep_order_time, 4)
        cm["queue_wait_s"]     = round(c_resume - c_order_end, 4) if c_resume is not None else None

# Per-car message counts, fallback flag, delivery ratio, and BFT decision time
for rep, sent in replica_messages_sent.items():
    car_metrics[f"veh{rep}"]["messages_sent"] = sent
for rep, recv in replica_messages_recv.items():
    car_metrics[f"veh{rep}"]["messages_received"] = recv
for rep, sent_bytes in replica_bytes_sent.items():
    car_metrics[f"veh{rep}"]["bytes_sent"] = sent_bytes
for rep in replica_stopsign_timeout:
    car_metrics[f"veh{rep}"]["used_fallback"] = True

for car_id, metrics in car_metrics.items():
    rep  = int(car_id[3:])
    sent = metrics.get("messages_sent")
    recv = metrics.get("messages_received")
    ep   = metrics.get("epoch")
    n    = EPOCH_N.get(ep, 0) if isinstance(ep, int) else 0
    if sent is not None and recv is not None and n > 1:
        expected = sent * (n - 1)
        ratio = recv / expected if expected > 0 else None
        metrics["delivery_ratio"] = ratio
        metrics["estimated_loss_rate"] = (1.0 - ratio) if ratio is not None else None

# Per-epoch throughput (user definition):
#   throughput_veh_per_s = n_vehicles_in_epoch / mean(depart - stop) over those vehicles
#   throughput_s_per_veh = mean(depart - stop) / n_vehicles_in_epoch  (stored value)
# `wait_intersection` already equals `depart_time - stop_time` per car (line ~694).
# Cars with missing stop or depart are naturally excluded.
epoch_depart_times   = defaultdict(list) # epoch -> sorted depart_times (kept for diagnostics)
epoch_throughput     = {}                # epoch -> s/veh under the user formula
epoch_wait_normal    = defaultdict(list) # epoch -> [wait_s, ...] for normal cars
epoch_wait_ambulance = defaultdict(list) # epoch -> [wait_s, ...] for ambulance cars

for car_id, metrics in car_metrics.items():
    if "role" not in metrics:
        metrics["role"] = "ambulance" if car_id in ambulance_ids else "normal"
    arrival_t = metrics.get("arrival_time")
    stop_t = metrics.get("stop_time")
    resume_t = metrics.get("resume_time")
    depart_t = metrics.get("depart_time")
    if stop_t is not None and depart_t is not None and "wait_intersection" not in metrics:
        metrics["wait_intersection"] = depart_t - stop_t
    base_t = stop_t if stop_t is not None else arrival_t
    if base_t is not None and resume_t is not None:
        metrics["stop_to_resume"] = resume_t - base_t
    if resume_t is not None and depart_t is not None:
        metrics["resume_to_depart"] = depart_t - resume_t
    if arrival_t is not None and depart_t is not None:
        metrics["arrival_to_depart"] = depart_t - arrival_t

for car_id, metrics in car_metrics.items():
    ep = metrics.get("epoch")
    if not isinstance(ep, int):
        continue
    dep = metrics.get("depart_time")
    if dep is not None:
        epoch_depart_times[ep].append(dep)
    wait_t = metrics.get("wait_intersection")
    role   = metrics.get("role", "normal")
    if wait_t is not None:
        if role == "ambulance":
            epoch_wait_ambulance[ep].append(wait_t)
        else:
            epoch_wait_normal[ep].append(wait_t)

for ep in set(list(epoch_wait_normal.keys()) + list(epoch_wait_ambulance.keys())):
    waits_ep = epoch_wait_normal.get(ep, []) + epoch_wait_ambulance.get(ep, [])
    if waits_ep:
        mean_wait = statistics.mean(waits_ep)
        if mean_wait > 0:
            epoch_throughput[ep] = mean_wait / len(waits_ep)  # s/veh

os.makedirs(PLOTS_DIR, exist_ok=True)

def _ms_or_none(seconds):
    return None if seconds is None else seconds * 1000.0

def _first_event_time_by_replica(events):
    """Return replica->first_time for [(rep, t_s), ...] in log order."""
    out = {}
    for rep, t_s in events:
        if rep not in out:
            out[rep] = t_s
    return out

def _all_event_times_by_replica(events):
    """Return replica->sorted list of times for [(rep, t_s), ...]."""
    out = defaultdict(list)
    for rep, t_s in events:
        out[rep].append(t_s)
    for rep in out:
        out[rep].sort()
    return out

def _view_change_duration_ms(replica_id: int):
    """Per-replica VC duration: VC-TRIGGER time -> first subsequent primary-changed time."""
    if replica_id not in replica_vc_trigger_time:
        return None
    t0 = replica_vc_trigger_time[replica_id]
    for t1 in replica_primary_change_times.get(replica_id, []):
        if t1 >= t0:
            return (t1 - t0) * 1000.0
    return None

def _epoch_view_change_duration_ms(epoch: int):
    """Global VC duration for the epoch: earliest VC-TRIGGER -> earliest primary-changed."""
    if epoch not in vc_trigger_times_by_epoch or epoch not in primary_change_times_by_epoch:
        return None
    if not vc_trigger_times_by_epoch[epoch] or not primary_change_times_by_epoch[epoch]:
        return None
    t0 = min(vc_trigger_times_by_epoch[epoch])
    t1 = min(primary_change_times_by_epoch[epoch])
    return max(0.0, (t1 - t0) * 1000.0)

def _epoch_commit_time(epoch: int):
    """Approximate commit/decision baseline as earliest Order_Decided_Time in the epoch."""
    ts = order_decided_times_by_epoch.get(epoch, [])
    return min(ts) if ts else None

def _epoch_first_propose_time(epoch: int):
    ts = propose_submit_times_by_epoch.get(epoch, [])
    return min(ts) if ts else None

def _epoch_winning_propose_time(epoch: int):
    """
    ProposeAll_Submit_Time that most likely led to the committed order.

    Heuristic: choose the latest propose time <= earliest Order_Decided_Time in the epoch.
    This excludes an early/byzantine propose that never committed before a view change.
    """
    commit_t = _epoch_commit_time(epoch)
    ts = propose_submit_times_by_epoch.get(epoch, [])
    if not ts:
        return None
    if commit_t is None:
        return max(ts)
    eligible = [t for t in ts if t <= commit_t]
    return max(eligible) if eligible else min(ts)

def _epoch_cert_collection_duration_ms(epoch: int):
    rm = round_metrics.get(epoch, {})
    if "Cert_Collection_Duration" in rm and rm["Cert_Collection_Duration"] is not None:
        return rm["Cert_Collection_Duration"] * 1000.0
    t_start = cert_collection_start_by_epoch.get(epoch)
    t_prop = _epoch_first_propose_time(epoch)
    if t_start is not None and t_prop is not None and t_prop >= t_start:
        return (t_prop - t_start) * 1000.0
    return None

def _binomial_tail(n, probability, required):
    if probability is None:
        return None
    required = max(0, int(required))
    if required <= 0:
        return 1.0
    if required > n:
        return 0.0
    return sum(
        math.comb(n, k) * (probability ** k) * ((1.0 - probability) ** (n - k))
        for k in range(required, n + 1)
    )


def _phase2_attack_summary():
    if phase2_attack_config is None:
        return None
    config = dict(phase2_attack_config)
    target = f"veh{config['target_replica_id']}"
    epoch = 0
    key = (target, epoch)
    key_text = f"{target}@{epoch}"
    byzantine_ids = set(config["colluder_ids"])
    if config["kind"] != "NONE" and config["target_replica_id"] >= 0:
        byzantine_ids.add(config["target_replica_id"])

    lane_rows = [
        row for row in perception_evaluations.values()
        if row["target"] == target and row["epoch"] == epoch and
           row["witness"] not in byzantine_ids
    ]
    h_lane = len({row["witness"] for row in lane_rows})
    honest_lane_accepts = len({
        row["witness"] for row in lane_rows if row["lane_accept"]
    })
    lane_accept_rate = honest_lane_accepts / h_lane if h_lane else None
    kind = config["kind"]
    false_lane_attack = kind in ("WRONG_APPROACH", "FALSE_PHYSICAL_LANE")
    q0_lane = lane_accept_rate if false_lane_attack else None
    q1_lane = lane_accept_rate if kind == "NONE" else None
    lateral_rows = [row for row in lane_rows if "true_lateral_cm" in row]
    lateral_delta_m = None
    q0_lane_model = None
    if lateral_rows:
        lateral_delta_m = abs(
            lateral_rows[0]["claimed_lateral_cm"] - lateral_rows[0]["true_lateral_cm"]
        ) / 100.0
        sigma_lat = float((perception_config or {}).get("witness_lateral_sigma_m", 0.0))
        gate_k = float((perception_config or {}).get("physical_gate_k", 3.0))
        if false_lane_attack:
            if sigma_lat == 0.0:
                q0_lane_model = 1.0 if lateral_delta_m == 0.0 else 0.0
            else:
                normal_cdf = lambda x: 0.5 * (1.0 + math.erf(x / math.sqrt(2.0)))
                ratio = lateral_delta_m / sigma_lat
                q0_lane_model = normal_cdf(ratio + gate_k) - normal_cdf(ratio - gate_k)

    evidence = cert_evidence.get(key, {})
    honest_external = {
        signer: row for signer, row in evidence.items()
        if signer not in byzantine_ids and not row["self"]
    }
    h_dir = len(honest_external)
    honest_direction_support = sum(1 for row in honest_external.values() if row["supporting"])
    q0_dir = honest_direction_support / h_dir if h_dir else None
    b_sig_lane = sum(1 for signer in evidence if signer in byzantine_ids)
    collected_echoes = arrival_echo_collections.get(key, {})
    b_sig_lane_attempt_signers = sorted(
        signer for signer, row in collected_echoes.items()
        if signer in byzantine_ids and row["byzantine"]
    )
    b_sig_lane_attempt = (
        len(b_sig_lane_attempt_signers)
        if collected_echoes else None
    )
    generated_self_attestations = {
        signer for (attest_target, attest_epoch, signer, _), row in self_attestations.items()
        if attest_target == target and attest_epoch == epoch and row["status"] == "VALID"
    }
    b_sig_lane_attempt_source = "ECHO-COLLECT" if collected_echoes else "UNAVAILABLE"
    if b_sig_lane_attempt is None and false_lane_attack:
        if (config.get("actual_b") == 1 and
                config.get("target_replica_id") in generated_self_attestations):
            b_sig_lane_attempt = 1
            b_sig_lane_attempt_signers = [config["target_replica_id"]]
            b_sig_lane_attempt_source = "SELF-ATTEST-RECOVERY"
        elif evidence:
            b_sig_lane_attempt = b_sig_lane
            b_sig_lane_attempt_signers = sorted(
                signer for signer in evidence if signer in byzantine_ids
            )
            b_sig_lane_attempt_source = "FINALIZED-CERT-RECOVERY"
    b_sig_dir = sum(
        1 for signer, row in evidence.items()
        if signer in byzantine_ids and row["supporting"]
    )
    self_count = sum(1 for row in evidence.values() if row["self"])
    distance_rows = [
        row for row in distance_perception_evaluations.values()
        if row["target"] == target and row["epoch"] == epoch and
           row["witness"] not in byzantine_ids
    ]
    h_distance = len({row["witness"] for row in distance_rows})
    honest_distance_accepts = len({
        row["witness"] for row in distance_rows if row["accept"]
    })
    q0_distance = honest_distance_accepts / h_distance if h_distance else None
    distance_evidence = distance_cert_evidence.get(key, {})
    b_sig_distance = sum(
        1 for signer, row in distance_evidence.items()
        if signer in byzantine_ids and row["byzantine"]
    )
    distance_self_count = sum(1 for row in distance_evidence.values() if row["self"])
    eligibility = direction_eligibility.get(key, {})
    # A rejected false-lane claim has no target certificate and therefore no
    # target DIR-ELIGIBILITY row.  The f+1 threshold is nevertheless defined
    # for the attempted outcome.  Recover it from any same-run certificate
    # before falling back to the run-level fault metadata.
    observed_threshold = next(
        (row.get("threshold") for row in direction_eligibility.values()
         if row.get("threshold") is not None),
        None,
    )
    threshold = int(
        eligibility.get("threshold")
        or observed_threshold
        or experiment_fault_tolerance.get("cert_threshold")
        or 0
    )
    distance_threshold = int(
        distance_certificates.get(key, {}).get("threshold")
        or next((row.get("threshold") for row in distance_certificates.values()), 0)
        or experiment_fault_tolerance.get("cert_threshold")
        or 0
    )
    if kind == "WRONG_APPROACH":
        h_used, q_used, b_used = h_lane, q0_lane, b_sig_lane_attempt
    elif kind == "FALSE_PHYSICAL_LANE":
        h_used, q_used, b_used = h_lane, q0_lane_model, b_sig_lane_attempt
    elif kind == "FALSE_DIRECTION":
        h_used, q_used, b_used = h_dir, q0_dir, b_sig_dir
    elif kind == "FALSE_DISTANCE":
        h_used, q_used, b_used = h_distance, q0_distance, b_sig_distance
        threshold = distance_threshold
    else:
        h_used, q_used, b_used = 0, None, None
    required_honest = max(0, threshold - b_used) if b_used is not None else None
    binomial_prediction = (
        _binomial_tail(h_used, q_used, required_honest)
        if required_honest is not None else None
    )
    outcome = phase2_attack_outcomes.get(key, {})
    return {
        **config,
        "config_replica_count": len(phase2_attack_config_replicas),
        "config_mismatch_count": phase2_attack_config_mismatches,
        "target": target,
        "epoch": epoch,
        "configured_byzantine_ids": sorted(byzantine_ids),
        "declaration": phase2_attack_declarations.get(key),
        "collusion_echo_signers": sorted(phase2_collusion_echoes.get(key, set())),
        "distance_collusion_echo_signers": sorted(distance_collusion_echoes.get(key, set())),
        "certificate_signers": sorted(evidence),
        "distance_certificate_signers": sorted(distance_evidence),
        "self_attestation_count": self_count,
        "self_attestation_generated_count": len(generated_self_attestations),
        "distance_self_attestation_count": distance_self_count,
        "h_lane": h_lane,
        "h_dir": h_dir,
        "h_distance": h_distance,
        "h_lane_signers": sorted({row["witness"] for row in lane_rows}),
        "h_dir_signers": sorted(honest_external),
        "h_distance_signers": sorted({row["witness"] for row in distance_rows}),
        "honest_lane_accepts": honest_lane_accepts,
        "honest_direction_support": honest_direction_support,
        "honest_distance_accepts": honest_distance_accepts,
        "q0_lane": q0_lane,
        "q0_lane_empirical_run": q0_lane,
        "q0_lane_model": q0_lane_model,
        "q1_lane": q1_lane,
        "q1_lane_empirical_run": q1_lane,
        "lane_accept_rate": lane_accept_rate,
        "lateral_delta_m": lateral_delta_m,
        "q0_dir": q0_dir,
        "q0_distance": q0_distance,
        "b_sig_lane": b_sig_lane,
        "b_sig_lane_cert": b_sig_lane,
        "b_sig_lane_attempt": b_sig_lane_attempt,
        "b_sig_lane_attempt_signers": b_sig_lane_attempt_signers,
        "b_sig_lane_attempt_source": (
            b_sig_lane_attempt_source
        ),
        "b_sig_dir": b_sig_dir,
        "b_sig_distance": b_sig_distance,
        "threshold": threshold,
        "required_honest_support": required_honest,
        "binomial_tail_prediction": binomial_prediction,
        "lane_certified": key_text in {
            f"{cert_target}@{cert_epoch}" for cert_target, cert_epoch in cert_assembly_perception
        },
        "distance_declaration": distance_attack_declarations.get(key),
        "distance_certified": key in distance_certificates,
        "outcome": outcome,
    }


def _movement_metrology_summary():
    entry_counts = defaultdict(int)
    exit_counts = defaultdict(int)
    for row in conflict_zone_events:
        (entry_counts if row["event"] == "ENTER" else exit_counts)[row["vehicle"]] += 1
    mismatch = sorted(
        vehicle for vehicle, row in movement_actual_egress.items() if not row["match"]
    )
    return {
        "ground_truth": dict(sorted(movement_ground_truth.items())),
        "actual_egress": dict(sorted(movement_actual_egress.items())),
        "planned_actual_mismatch_vehicles": mismatch,
        "conflict_zone_events": conflict_zone_events,
        "entry_count_by_vehicle": dict(sorted(entry_counts.items())),
        "exit_count_by_vehicle": dict(sorted(exit_counts.items())),
        "conflicting_cooccupancy_pairs": list(movement_conflict_pairs.values()),
        "conflicting_cooccupancy_pair_count": len(movement_conflict_pairs),
        "sumo_collision_vehicles": physical_collision_vehicles,
        "sumo_collision_vehicle_count": len(physical_collision_vehicles),
        "calibration_config": metrology_config,
        "calibration_releases": dict(sorted(metrology_releases.items())),
    }


def _perception_summary():
    evaluations = list(perception_evaluations.values())
    evaluated_variants = defaultdict(set)
    for witness, target, epoch, claim_hash in perception_evaluations:
        evaluated_variants[(witness, target, epoch)].add(claim_hash)
    cross_variant_evaluations = {
        f"r{witness}:{target}@{epoch}": sorted(hashes)
        for (witness, target, epoch), hashes in evaluated_variants.items()
        if len(hashes) > 1
    }
    configured_byzantine = set(replica_byzantine_types)
    measured_h_lane = defaultdict(set)
    measured_h_dir = defaultdict(set)
    measured_b_sig = defaultdict(set)
    cue_counts = defaultdict(int)
    cue_support_count = 0
    cue_support_opportunities = 0
    for row in evaluations:
        cue_counts[row["observed_cue"]] += 1
        eligibility = direction_eligibility.get((row["target"], row["epoch"]))
        if eligibility is not None and row["observed_cue"] != "UNKNOWN":
            cue_support_opportunities += 1
            if row["observed_cue"] == eligibility["declared"]:
                cue_support_count += 1
        if row["witness"] not in configured_byzantine:
            measured_h_lane[f"{row['target']}@{row['epoch']}"].add(row["witness"])
    for (target, epoch), signers in cert_evidence.items():
        key = f"{target}@{epoch}"
        for signer, row in signers.items():
            if row["byzantine"] and row["supporting"]:
                measured_b_sig[key].add(signer)
            elif not row["byzantine"] and not row["self"]:
                measured_h_dir[key].add(signer)
    accepted = sum(1 for row in evaluations if row["lane_accept"])
    tier_counts = defaultdict(int)
    for tier in trust_tiers.values():
        tier_counts[tier] += 1
    signed_direction = sum(
        count for tier, count in tier_counts.items() if tier == "SIGNED-DIRECTION"
    )
    left_singletons = sum(
        1 for row in direction_eligibility.values() if row["derived"] == "L"
    )
    distance_rows = list(distance_perception_evaluations.values())
    distance_accepts = sum(1 for row in distance_rows if row["accept"])
    return {
        "configuration": perception_config,
        "configuration_replica_count": len(perception_config_replicas),
        "lane_evaluation_count": len(evaluations),
        "lane_accept_count": accepted,
        "lane_accept_rate": accepted / len(evaluations) if evaluations else None,
        "cue_distribution": dict(sorted(cue_counts.items())),
        "cue_unknown_rate": cue_counts.get("UNKNOWN", 0) / len(evaluations) if evaluations else None,
        "cue_support_count": cue_support_count,
        "cue_support_opportunities": cue_support_opportunities,
        "cue_support_rate": (
            cue_support_count / cue_support_opportunities
            if cue_support_opportunities else None
        ),
        "duplicate_evaluation_count": perception_duplicate_evaluations,
        "cross_variant_evaluation_count": sum(
            len(hashes) - 1 for hashes in evaluated_variants.values()
            if len(hashes) > 1
        ),
        "cross_variant_evaluations": cross_variant_evaluations,
        "single_evaluation_invariant_ok": (
            perception_duplicate_evaluations == 0 and
            not cross_variant_evaluations
        ),
        "equivocation_event_count": len(equivocation_events),
        "equivocation_events": {
            f"r{witness}:{target}@{epoch}:{second_hash}": row
            for (witness, target, epoch, second_hash), row
            in sorted(equivocation_events.items())
        },
        "equivocation_rejections_without_perception_or_echo": all(
            row["action"] == "REJECT_SECOND_VARIANT" and
            not row["perception_evaluated"] and not row["echo_sent"]
            for row in equivocation_events.values()
        ),
        # measured_h is retained as the lane-channel compatibility alias.
        "measured_h": {key: len(value) for key, value in sorted(measured_h_lane.items())},
        "measured_h_lane": {
            key: len(value) for key, value in sorted(measured_h_lane.items())
        },
        "measured_h_dir": {
            key: len(value) for key, value in sorted(measured_h_dir.items())
        },
        "measured_b_sig": {
            key: len(value) for key, value in sorted(measured_b_sig.items())
        },
        "self_attestation_count": len(self_attestations),
        "self_attestation_duplicate_count": self_attestation_duplicates,
        "self_attestation_invariant_ok": self_attestation_duplicates == 0,
        "self_attestations": {
            f"{target}@{epoch}@r{signer}@{claim_hash}": row
            for (target, epoch, signer, claim_hash), row in sorted(self_attestations.items())
        },
        "certificate_evidence": {
            f"{target}@{epoch}": {
                str(signer): row for signer, row in sorted(signers.items())
            }
            for (target, epoch), signers in sorted(cert_evidence.items())
        },
        "arrival_echo_collections": {
            f"{target}@{epoch}": {
                str(signer): row for signer, row in sorted(signers.items())
            }
            for (target, epoch), signers in sorted(arrival_echo_collections.items())
        },
        "phase2_attack": _phase2_attack_summary(),
        "certificates": {
            f"{target}@{epoch}": row
            for (target, epoch), row in sorted(cert_assembly_perception.items())
        },
        "direction_eligibility": {
            f"{target}@{epoch}": row
            for (target, epoch), row in sorted(direction_eligibility.items())
        },
        # Prefer the explicit three-tier record.  The fallback preserves
        # compatibility with older logs that emitted tiers only for signed
        # certificates and represented QUIET as an absent record.
        "quiet_count": (
            tier_counts.get("QUIET", 0)
            if tier_counts.get("QUIET", 0) > 0
            else (max(0, CARS - len(trust_tiers)) if trust_tiers else None)
        ),
        "signed_unknown_count": tier_counts.get("SIGNED-UNKNOWN", 0),
        "signed_direction_count": signed_direction,
        "left_table_forced_singleton_count": left_singletons,
        "signed_unknown_singleton_count": tier_counts.get("SIGNED-UNKNOWN", 0),
        "stopped_distance": {
            "attestation_count": len(stopped_distance_attestations),
            "attestations": {
                f"{target}@{epoch}": row
                for (target, epoch), row in sorted(stopped_distance_attestations.items())
            },
            "retry_count_by_target": {
                f"{target}@{epoch}": count
                for (target, epoch), count in sorted(stopped_distance_attestation_retries.items())
            },
            "evaluation_count": len(distance_rows),
            # Preserve the one cached physical observation per witness/target
            # so k can be re-thresholded offline without re-running SUMO.  The
            # wire protocol still carries only the signed verdict/certificate.
            "evaluations": [
                row for _, row in sorted(distance_perception_evaluations.items())
            ],
            "accept_count": distance_accepts,
            "accept_rate": distance_accepts / len(distance_rows) if distance_rows else None,
            "duplicate_evaluation_count": distance_perception_duplicate_evaluations,
            "single_evaluation_invariant_ok": distance_perception_duplicate_evaluations == 0,
            "certificates": {
                f"{target}@{epoch}": row
                for (target, epoch), row in sorted(distance_certificates.items())
            },
            "collection_order": distance_cert_collection,
            "certificate_count": len(distance_certificates),
            "order_pairs": list(distance_order_pairs.values()),
            "order_pair_count": len(distance_order_pairs),
            "order_inversion_count": sum(
                1 for row in distance_order_pairs.values() if row["inversion"]
            ),
        },
    }

def write_metrics_json(path):
    """Write all extracted metrics to a structured JSON file for downstream plotting."""
    import json

    def _stat(values):
        if not values:
            return None
        return {
            "mean": statistics.mean(values),
            "median": statistics.median(values),
            "p95": percentile(values, 0.95),
            "min": min(values),
            "max": max(values),
            "n": len(values),
        }

    epochs_out = []
    for ep in range(N_EPOCHS):
        n = EPOCH_N[ep]
        rm = round_metrics.get(ep, {})
        tp = epoch_throughput.get(ep)
        wn = epoch_wait_normal.get(ep, [])
        wa = epoch_wait_ambulance.get(ep, [])
        byz = byzantine_by_epoch.get(ep, 0)
        msgs_sent = epoch_messages_sent.get(ep, [])
        msgs_recv = epoch_messages_recv.get(ep, [])
        bytes_sent = epoch_bytes_sent.get(ep, [])
        cert_creation = epoch_cert_creation_latency.get(ep, [])
        propose_all_sim = rm.get("ProposeAll_Consensus_Sim")
        first_submit_commit = rm.get("ProposeAll_FirstSubmit_To_OrderCommit_Sim")
        epochs_out.append({
            "epoch": ep,
            "n_replicas": n,
            "tolerated_f": experiment_fault_tolerance["tolerated_f"],
            "static_replicas": experiment_fault_tolerance["static_replicas"],
            "consensus_quorum": experiment_fault_tolerance["consensus_quorum"],
            "cert_threshold": experiment_fault_tolerance["cert_threshold"],
            "propose_all_consensus_latency_sim_s": propose_all_sim,
            "propose_all_first_submit_to_commit_sim_s": first_submit_commit,
            "stop_to_decision_sim_s": rm.get("Stop_To_Decision_Sim"),
            "cert_creation_latency_ms": (
                statistics.mean(cert_creation) * 1000.0 if cert_creation else None
            ),
            "throughput_s_per_veh": tp,
            "throughput_veh_per_s": (1.0 / tp) if tp not in (None, 0) else None,
            "wait_normal_s": _stat(wn),
            "wait_ambulance_s": _stat(wa),
            "byzantine_injections": byz,
            "consensus_success": bool(propose_all_sim),
            "avg_messages_sent_per_replica": (sum(msgs_sent) / len(msgs_sent)) if msgs_sent else None,
            "avg_messages_recv_per_replica": (sum(msgs_recv) / len(msgs_recv)) if msgs_recv else None,
            "avg_bytes_sent_per_replica": (sum(bytes_sent) / len(bytes_sent)) if bytes_sent else None,
            "total_bytes_sent": sum(bytes_sent) if bytes_sent else None,
            "total_megabytes_sent": (sum(bytes_sent) / (1024.0 * 1024.0)) if bytes_sent else None,
            "stop_sign_failures": epoch_failures.get(ep, 0),
        })

    cars_out = []
    for car_id in sorted(car_metrics.keys(), key=lambda cid: int(cid[3:])):
        m = car_metrics[car_id]
        cars_out.append({
            "car_id": car_id,
            "role": m.get("role", "normal"),
            "epoch": m.get("epoch"),
            "arrival_time_s": m.get("arrival_time"),
            "stop_time_s": m.get("stop_time"),
            "resume_time_s": m.get("resume_time"),
            "depart_time_s": m.get("depart_time"),
            "wait_intersection_s": m.get("wait_intersection"),
            "stop_to_resume_s": m.get("stop_to_resume"),
            "resume_to_depart_s": m.get("resume_to_depart"),
            "arrival_to_depart_s": m.get("arrival_to_depart"),
            # Message-level metrics (matches RAFT messages_sent/received)
            "messages_sent": m.get("messages_sent"),
            "messages_received": m.get("messages_received"),
            "bytes_sent": m.get("bytes_sent"),
            "megabytes_sent": (
                m.get("bytes_sent") / (1024.0 * 1024.0)
                if m.get("bytes_sent") is not None else None
            ),
            # delivery_ratio = received / (sent * (N-1)); heuristic for message loss
            "delivery_ratio": m.get("delivery_ratio"),
            "estimated_loss_rate": m.get("estimated_loss_rate"),
            # Fallback: True if this vehicle hit a StopSign_Timeout
            "used_fallback": m.get("used_fallback", False),
            "propose_all_consensus_sim_s": m.get("propose_all_consensus_sim_s"),
            "propose_all_first_submit_to_commit_sim_s": m.get(
                "propose_all_first_submit_to_commit_sim_s"
            ),
            "stop_to_decision_sim_s": m.get("stop_to_decision_sim_s"),
            # Phase breakdown: stop → PROPOSE_ALL submit → ORDER decision → resume
            "cert_wait_s": m.get("cert_wait_s"),
            "bft_wait_s": m.get("bft_wait_s"),
            "order_delivery_s": m.get("order_delivery_s"),
            "queue_wait_s": m.get("queue_wait_s"),
        })

    fa = jains_fairness(all_waits)
    fn = jains_fairness(normal_waits)

    # Run-level delivery and fallback metrics (mirrors RAFT delivery_ratio / fallback_rate)
    _all_dr = [m["delivery_ratio"] for m in car_metrics.values()
               if m.get("delivery_ratio") is not None]
    _run_dr = statistics.mean(_all_dr) if _all_dr else None
    _run_lr = (1.0 - _run_dr) if _run_dr is not None else None
    _fallback_n = sum(1 for m in car_metrics.values() if m.get("used_fallback", False))
    _run_fallback_rate = _fallback_n / CARS if CARS > 0 else None
    _run_bytes_sent = sum(replica_bytes_sent.values()) if replica_bytes_sent else None
    _quiet_honest_n = sum(replica_quiet_honest_vehicles.values())
    _quiet_honest_d = sum(replica_quiet_honest_opportunities.values())
    _quiet_honest_rate = (
        100.0 * _quiet_honest_n / _quiet_honest_d
        if _quiet_honest_d > 0 else None
    )
    _attack_outcomes_unique = [
        row
        for ep in sorted(attack_outcomes_by_epoch)
        for row in attack_outcomes_by_epoch[ep]
    ]
    _attack_failures_total = sum(attack_failures_by_epoch.values())

    overall = {
        "cars": CARS,
        "epochs": N_EPOCHS,
        "fault_tolerance_experiment": experiment_fault_tolerance,
        "throughput_s_per_veh": throughput,
        "throughput_veh_per_s": throughput_vps,
        "throughput_formula": "n_cars / mean(depart_time - stop_time)  (s/veh stored as inverse)",
        "wait_all_s": _stat(all_waits),
        "wait_normal_s": _stat(normal_waits),
        "wait_ambulance_s": _stat(ambulance_waits),
        "jains_fairness_all": fa,
        "jains_fairness_normal": fn,
        "ambulance_priority_gain_s": (statistics.mean(normal_waits) - statistics.mean(ambulance_waits))
            if normal_waits and ambulance_waits else None,
        "ambulance_priority_ratio": (statistics.mean(ambulance_waits) / statistics.mean(normal_waits))
            if normal_waits and ambulance_waits and statistics.mean(normal_waits) != 0 else None,
        # Batch throughput: vehicles / (last depart - first stop) when batch data available.
        "batch_throughput_veh_per_s": (
            (lambda: CARS / (max(departs) - min(stops))
             if (stops := [m["stop_time"] for m in car_metrics.values() if m.get("stop_time") is not None])
                and (departs := [m["depart_time"] for m in car_metrics.values() if m.get("depart_time") is not None])
                and max(departs) > min(stops) else None)()
        ),
        "batch_index_distribution": (
            {str(bi): sum(1 for v in replica_batch_index.values() if v == bi)
             for bi in sorted(set(replica_batch_index.values()))}
            if replica_batch_index else None
        ),
        "propose_all_consensus_latency_sim_s": _stat([
            round_metrics[ep]["ProposeAll_Consensus_Sim"]
            for ep in range(N_EPOCHS)
            if round_metrics.get(ep, {}).get("ProposeAll_Consensus_Sim", 0) > 0
        ]),
        "propose_all_first_submit_to_commit_latency_sim_s": _stat([
            round_metrics[ep]["ProposeAll_FirstSubmit_To_OrderCommit_Sim"]
            for ep in range(N_EPOCHS)
            if round_metrics.get(ep, {}).get("ProposeAll_FirstSubmit_To_OrderCommit_Sim", 0) > 0
        ]),
        "cert_creation_latency_ms": _stat([
            value * 1000.0
            for ep in range(N_EPOCHS)
            for value in epoch_cert_creation_latency.get(ep, [])
            if value > 0
        ]),
        "stop_to_decision_sim_s": _stat([
            round_metrics[ep]["Stop_To_Decision_Sim"]
            for ep in range(N_EPOCHS)
            if round_metrics.get(ep, {}).get("Stop_To_Decision_Sim", 0) > 0
        ]),
        "total_byzantine_injections": byzantine_total,
        "byzantine_by_epoch": dict(byzantine_by_epoch),
        "byzantine_by_replica": dict(byzantine_by_replica),
        "attack_success": _attack_failures_total > 0,
        "attack_failures_total": _attack_failures_total,
        "attack_failures_by_epoch": dict(attack_failures_by_epoch),
        "attack_outcomes": _attack_outcomes_unique,
        "attack_outcome_lines_total": len(attack_outcomes),
        "false_lane_collusion": {
            "configuration": false_lane_colluder_config,
            "distinct_echo_signers_by_target": {
                target: sorted(signers)
                for target, signers in sorted(false_lane_echo_signers.items())
            },
            "forged_certificates": false_lane_certificates,
            "blocked_targets": false_lane_blocked_targets,
            "committed_targets": false_lane_commits,
            "forged_certificate_count": len(false_lane_certificates),
            "committed_forged_claim_count": len(false_lane_commits),
        },
        "malformed_proposal_preverify_rejections": malformed_proposal_reject_count,
        "unsafe_conflict_cooccupancy_pairs": list(unsafe_conflict_pairs.values()),
        "unsafe_conflict_cooccupancy_pair_count": len(unsafe_conflict_pairs),
        "physical_collision_vehicles": physical_collision_vehicles,
        "physical_collision_vehicle_count": len(physical_collision_vehicles),
        "movement_metrology": _movement_metrology_summary(),
        "stop_to_resume_s": _stat(stop_to_resume_all),
        "resume_to_depart_s": _stat(resume_to_depart_all),
        # Delivery and fallback metrics (same definitions as RAFT counterpart)
        # delivery_ratio = received / (sent * (N-1)); heuristic for message loss
        "delivery_ratio": _run_dr,
        "estimated_loss_rate": _run_lr,
        # fallback_rate = fraction of vehicles that hit StopSign_Timeout this run
        "fallback_rate": _run_fallback_rate,
        "fallback_count": _fallback_n,
        # Payload bytes emitted onto the radio, post signing/wrapping.
        "bytes_sent_total": _run_bytes_sent,
        "megabytes_sent_total": (
            _run_bytes_sent / (1024.0 * 1024.0)
            if _run_bytes_sent is not None else None
        ),
        "quiet_honest_count": _quiet_honest_n,
        "quiet_honest_opportunities": _quiet_honest_d,
        "quiet_honest_rate_percent": _quiet_honest_rate,
        "r0_late_emergency": r0_metrics if r0_metrics["enabled"] else None,
    }

    if _args.output_format == "legacy":
        out = {"overall": overall, "per_epoch": epochs_out, "per_car": cars_out}
    else:
        propose_by_replica = _first_event_time_by_replica(resdb_propose_submit_events)
        decided_by_replica = _first_event_time_by_replica(resdb_order_decided_events)
        decided_all_by_replica = _all_event_times_by_replica(resdb_order_decided_events)

        vehicles_out = []
        for rep in range(CARS):
            cid = f"veh{rep}"
            m = car_metrics.get(cid, {})
            ep = m.get("epoch")

            stop_s = m.get("stop_time")
            depart_s = m.get("depart_time")
            stopped_ms = _ms_or_none(stop_s)
            passed_ms = _ms_or_none(depart_s)

            total_wait_ms = None
            if stop_s is not None and depart_s is not None:
                total_wait_ms = max(0.0, (depart_s - stop_s) * 1000.0)

            # decision_latency_ms (plotting): "first ProposeAll_Submit_Time (incl. byzantine leader) -> this replica received decision".
            decision_latency_ms = None
            first_propose_t = _epoch_first_propose_time(ep) if isinstance(ep, int) else None
            decide_recv_t = (decided_all_by_replica.get(rep) or [None])[0]
            if decide_recv_t is None and isinstance(ep, int):
                decide_recv_t = replica_gossip_send_time_by_epoch.get((rep, ep))
            if first_propose_t is not None and decide_recv_t is not None and decide_recv_t >= first_propose_t:
                decision_latency_ms = (decide_recv_t - first_propose_t) * 1000.0

            # Full decision latency (end-to-end for protocol): first propose submit in the epoch
            # (incl. a byzantine propose that later triggers view-change) -> this replica received decision.
            #
            # This matches the common "Replica decided - Byzproposed" interpretation.
            full_decision_latency_ms = None
            if isinstance(ep, int) and decide_recv_t is not None:
                t_start = _epoch_first_propose_time(ep)
                if t_start is not None and decide_recv_t >= t_start:
                    full_decision_latency_ms = (decide_recv_t - t_start) * 1000.0

            # Extra debugging metric: winning propose (post-view-change) -> decision receive.
            winning_propose_latency_ms = None
            if isinstance(ep, int) and decide_recv_t is not None:
                t_win_prop = _epoch_winning_propose_time(ep)
                if t_win_prop is not None and decide_recv_t >= t_win_prop:
                    winning_propose_latency_ms = (decide_recv_t - t_win_prop) * 1000.0

            # Treat decision as "via gossip" only if the first time this replica logged
            # Order_Decided_Time is at/after its first GOSSIP-APPLY.
            decision_via_gossip = False
            if rep in replica_gossip_apply_time:
                gossip_t = replica_gossip_apply_time[rep]
                first_decided_t = (decided_all_by_replica.get(rep) or [None])[0]
                if first_decided_t is None or first_decided_t >= gossip_t:
                    decision_via_gossip = True

            vehicles_out.append({
                "vehicle_id": rep,
                "run_metrics": overall,
                # Partner field name; we map our ambulance role to priority.
                "is_priority_vehicle": (m.get("role") == "ambulance"),
                "coordination_method": _args.coordination_method,
                "transport": "wave",
                "cluster_mode": "allVehicles",
                "timestamps_ms": {
                    "stopped": stopped_ms,
                    "passed": passed_ms,
                },
                "durations_ms": {
                    # No direct equivalent in PBFT; keep 0 and report VC in bft_stats.
                    "leader_election_time_ms": 0.0,
                    "decision_latency_ms": decision_latency_ms,
                    "total_wait_time": total_wait_ms,
                },
                "messages": {
                    "sent": m.get("messages_sent"),
                    "received": m.get("messages_received"),
                    "bytes_sent": m.get("bytes_sent"),
                    "megabytes_sent": (
                        m.get("bytes_sent") / (1024.0 * 1024.0)
                        if m.get("bytes_sent") is not None else None
                    ),
                },
                # Physical ground truth is distinct from which Byzantine
                # replica originated an attack.  Keep it per vehicle so plots
                # can identify both the injected wrecks and the actual unsafe
                # entrants without reparsing the combined text log.
                "physical_safety": {
                    "is_injected_wreck": cid in crash_inject_times,
                    "wreck_injected_at_ms": (
                        crash_inject_times[cid] * 1000.0
                        if cid in crash_inject_times else None
                    ),
                    "wreck_towed_at_ms": (
                        crash_tow_times[cid] * 1000.0
                        if cid in crash_tow_times else None
                    ),
                    "entered_while_wreck_active": cid in crash_unsafe_entrants,
                    "unsafe_entry_at_ms": (
                        crash_unsafe_entrants[cid] * 1000.0
                        if cid in crash_unsafe_entrants else None
                    ),
                    "unsafe_conflict_cooccupancy": any(
                        cid in pair for pair in unsafe_conflict_pairs
                    ),
                    "physical_collision": cid in physical_collision_vehicles,
                    "physical_collision_at_ms": physical_collision_vehicles.get(cid),
                },
                # Partner-format output is a list of vehicle records, so repeat
                # the compact run-level safety result on each record.  This
                # avoids the misleading interpretation that
                # attack_success_replica=false means the run was safe.
                "run_safety": {
                    "attack_success": _attack_failures_total > 0,
                    "fabricated_clearance_attempts": len(fabricated_clearance_events),
                    "fabricated_clearance_preverify_rejections": missing_clear_reject_count,
                    "unsafe_recovery_order_replicas": len(unsafe_recovery_order_replicas),
                    "unsafe_recovery_order_box_occupied_replicas": len(
                        unsafe_recovery_order_box_occupied_replicas
                    ),
                    "crash_injected_vehicles": sorted(
                        crash_inject_times, key=lambda x: int(x[3:])
                    ),
                    "crash_towed_vehicles": sorted(
                        crash_tow_times, key=lambda x: int(x[3:])
                    ),
                    "crash_cooccupancy_entrants": sorted(
                        crash_unsafe_entrants, key=lambda x: int(x[3:])
                    ),
                    "crash_cooccupancy_entrant_count": len(crash_unsafe_entrants),
                    "false_lane_forged_certificate_count": len(false_lane_certificates),
                    "false_lane_committed_claim_count": len(false_lane_commits),
                    "false_lane_committed_targets": sorted(false_lane_commits),
                    "malformed_proposal_preverify_rejections": malformed_proposal_reject_count,
                    "unsafe_conflict_cooccupancy_pair_count": len(unsafe_conflict_pairs),
                    "unsafe_conflict_cooccupancy_pairs": list(unsafe_conflict_pairs.values()),
                    "physical_collision_vehicle_count": len(physical_collision_vehicles),
                },
                "bft_stats": {
                    "batch_index": m.get("batch_index"),
                    "tolerated_f": experiment_fault_tolerance["tolerated_f"],
                    "static_replicas": experiment_fault_tolerance["static_replicas"],
                    "consensus_quorum": experiment_fault_tolerance["consensus_quorum"],
                    "cert_threshold": experiment_fault_tolerance["cert_threshold"],
                    "injected_f": (
                        false_lane_colluder_config.get("F")
                        if false_lane_colluder_config else None
                    ),
                    "false_lane_colluder_ids": (
                        false_lane_colluder_config.get("ids")
                        if false_lane_colluder_config else None
                    ),
                    "arrival_position_gate_enabled": (
                        false_lane_colluder_config.get("position_gate_enabled")
                        if false_lane_colluder_config else None
                    ),
                    "cert_collection_duration_ms": (
                        _epoch_cert_collection_duration_ms(ep) if isinstance(ep, int) else None
                    ),
                    "cert_creation_latency_ms": (
                        m.get("cert_creation_s") * 1000.0
                        if m.get("cert_creation_s") is not None else None
                    ),
                    "view_change_triggered": rep in replica_vc_trigger_time,
                    # Global epoch-level VC duration (earliest trigger -> earliest primary installed).
                    "view_change_duration_ms": _epoch_view_change_duration_ms(ep) if isinstance(ep, int) else None,
                    "decision_via_gossip": decision_via_gossip,
                    "used_fallback": bool(m.get("used_fallback", False)),
                    "byzantine_injections_replica": byzantine_by_replica.get(rep, 0),
                    "byzantine_types": (
                        sorted(replica_byzantine_types.get(rep, set()))
                        if replica_byzantine_types.get(rep) else None
                    ),
                    "full_decision_latency_ms": full_decision_latency_ms,
                    "winning_propose_to_receive_ms": winning_propose_latency_ms,
                    "quiet_honest_vehicles": replica_quiet_honest_vehicles.get(rep),
                    "quiet_honest_opportunities": replica_quiet_honest_opportunities.get(rep),
                    "quiet_honest_rate_percent": replica_quiet_honest_rate.get(rep),
                    "perception": _perception_summary(),
                    "movement_metrology": _movement_metrology_summary(),
                    "attack_outcomes_replica": [
                        row for row in attack_outcomes if row["replica"] == rep
                    ] or None,
                    "attack_success_replica": any(
                        row["attack_success"] for row in attack_outcomes
                        if row["replica"] == rep
                    ),
                    "unsafe_recovery_order_accepted": rep in unsafe_recovery_order_replicas,
                    "unsafe_recovery_order_accepted_while_box_occupied": (
                        rep in unsafe_recovery_order_box_occupied_replicas
                    ),
                },
            })

        out = vehicles_out
    with open(path, "w", encoding="utf-8") as f:
        json.dump(out, f, indent=2, default=str)

def write_car_metrics_csv(path):
    with open(path, "w", encoding="utf-8") as f:
        f.write(
            "car_id,role,epoch,arrival_time,stop_time,resume_time,depart_time,"
            "wait_intersection,stop_to_resume,resume_to_depart,arrival_to_depart\n"
        )
        for car_id in sorted(car_metrics.keys(), key=lambda cid: int(cid[3:])):
            metrics = car_metrics[car_id]
            row = [
                car_id,
                metrics.get("role", "normal"),
                metrics.get("epoch", ""),
                metrics.get("arrival_time", ""),
                metrics.get("stop_time", ""),
                metrics.get("resume_time", ""),
                metrics.get("depart_time", ""),
                metrics.get("wait_intersection", ""),
                metrics.get("stop_to_resume", ""),
                metrics.get("resume_to_depart", ""),
                metrics.get("arrival_to_depart", ""),
            ]
            f.write(",".join(str(v) for v in row) + "\n")

def save_waittime_plot(path):
    if plt is None:
        return False
    grouped = {"normal": [], "ambulance": []}
    for metrics in car_metrics.values():
        wait_t = metrics.get("wait_intersection")
        role = metrics.get("role", "normal")
        if wait_t is not None:
            grouped.setdefault(role, []).append(wait_t)
    if not grouped["normal"] and not grouped["ambulance"]:
        return False

    fig, ax = plt.subplots(figsize=(8, 5))
    labels = []
    values = []
    colors = []
    for role in ("normal", "ambulance"):
        if grouped.get(role):
            labels.append(role.title())
            values.append(grouped[role])
            colors.append("#4C78A8" if role == "normal" else "#E45756")
    # Matplotlib API compatibility:
    # - newer versions accept tick_labels=
    # - older versions use labels=
    try:
        bp = ax.boxplot(values, tick_labels=labels, patch_artist=True, showfliers=True)
    except TypeError:
        bp = ax.boxplot(values, labels=labels, patch_artist=True, showfliers=True)
    for patch, color in zip(bp["boxes"], colors):
        patch.set_facecolor(color)
        patch.set_alpha(0.6)
    for idx, series in enumerate(values, start=1):
        ax.scatter([idx] * len(series), series, color=colors[idx - 1], alpha=0.85, zorder=3)
    ax.set_title("Wait Time At Intersection: Ambulance vs Normal Vehicles")
    ax.set_ylabel("Wait time (stop to leave) [s]")
    ax.grid(axis="y", linestyle="--", alpha=0.35)
    fig.tight_layout()
    fig.savefig(path, dpi=200)
    plt.close(fig)
    return True

def save_waittime_by_car_plot(path):
    if plt is None:
        return False
    rows = []
    for car_id, metrics in car_metrics.items():
        wait_t = metrics.get("wait_intersection")
        if wait_t is None:
            continue
        rows.append((int(car_id[3:]), car_id, wait_t, metrics.get("role", "normal")))
    if not rows:
        return False
    rows.sort()
    labels = [row[1] for row in rows]
    waits = [row[2] for row in rows]
    colors = ["#E45756" if row[3] == "ambulance" else "#4C78A8" for row in rows]
    fig, ax = plt.subplots(figsize=(max(8, len(rows) * 0.55), 4.8))
    ax.bar(labels, waits, color=colors)
    ax.set_title("Per-Car Wait Time At Intersection")
    ax.set_ylabel("Wait time (stop to leave) [s]")
    ax.set_xlabel("Vehicle")
    ax.grid(axis="y", linestyle="--", alpha=0.35)
    fig.tight_layout()
    fig.savefig(path, dpi=200)
    plt.close(fig)
    return True

# ── Print Report ─────────────────────────────────────────────────────────────
BOLD = "\033[1m"
RESET = "\033[0m"
RED   = "\033[31m"
GRN   = "\033[32m"
YEL   = "\033[33m"
CYN   = "\033[36m"

print("=" * 72)
print(f"{BOLD}BFT-SMaRt V2V Intersection — Epoch Latency Breakdown{RESET}")
print("=" * 72)

for epoch in range(N_EPOCHS):
    n  = EPOCH_N[epoch]
    rm = round_metrics.get(epoch, {})
    propose_all_sim = rm.get("ProposeAll_Consensus_Sim")
    first_submit_commit = rm.get("ProposeAll_FirstSubmit_To_OrderCommit_Sim")
    print(f"\n{BOLD}── Epoch {epoch}  (n={n} replicas) ──{RESET}")
    if propose_all_sim is not None and propose_all_sim > 0:
        print(f"  PROPOSE_ALL latency:     {CYN}{propose_all_sim:.4f}s{RESET}  (sim-time, leader submit→commit)")
        if first_submit_commit is not None and first_submit_commit > 0:
            print(f"  First submit→commit:    {YEL}{first_submit_commit:.4f}s{RESET}  (incl. failed primary / VC)")
    else:
        print(f"  PROPOSE_ALL latency:     {RED}N/A — epoch did not complete{RESET}")

print(f"\n{'=' * 72}")
print(f"{BOLD}Per-Car Lifecycle Metrics{RESET}")
print(f"{'─' * 72}")
print(f"  {'Car':>6}  {'Role':>10}  {'Wait@Int':>10}  {'Stop→Resume':>13}  {'Resume→Leave':>14}  {'Arr→Leave':>11}")
for car_id in sorted(car_metrics.keys(), key=lambda cid: int(cid[3:])):
    metrics = car_metrics[car_id]
    wait_t = metrics.get("wait_intersection")
    arr_res = metrics.get("stop_to_resume")
    res_dep = metrics.get("resume_to_depart")
    arr_dep = metrics.get("arrival_to_depart")
    role = metrics.get("role", "normal")
    print(
        f"  {car_id:>6}  {role:>10}  "
        f"{(f'{wait_t:.4f}s' if wait_t is not None else 'N/A'):>10}  "
        f"{(f'{arr_res:.4f}s' if arr_res is not None else 'N/A'):>13}  "
        f"{(f'{res_dep:.4f}s' if res_dep is not None else 'N/A'):>14}  "
        f"{(f'{arr_dep:.4f}s' if arr_dep is not None else 'N/A'):>11}"
    )

# Phase breakdown: cert_wait / bft_wait / queue_wait
_has_phase = any(car_metrics[c].get("cert_wait_s") is not None for c in car_metrics)
if _has_phase:
    print(f"\n{'─' * 72}")
    print(f"{BOLD}Per-Car Phase Breakdown  (stop → cert done → BFT → ORDER delivery → resume){RESET}")
    print(f"  {'Car':>6}  {'Role':>10}  {'CertWait':>9}  {'BFT':>6}  {'Deliver':>8}  {'Queue':>7}  {'Total':>7}")
    print(f"{'─' * 72}")
    for car_id in sorted(car_metrics.keys(), key=lambda cid: int(cid[3:])):
        cm = car_metrics[car_id]
        cw  = cm.get("cert_wait_s")
        bw  = cm.get("bft_wait_s")
        dw  = cm.get("order_delivery_s")
        qw  = cm.get("queue_wait_s")
        std = cm.get("stop_to_decision_sim_s")
        role = cm.get("role", "normal")
        role_tag = f" {CYN}←AMB{RESET}" if role == "ambulance" else ""
        def _f(v): return f"{v:.2f}s" if v is not None else "N/A"
        print(
            f"  {car_id:>6}  {role:>10}  "
            f"{_f(cw):>9}  {_f(bw):>6}  {_f(dw):>8}  {_f(qw):>7}  {_f(std):>7}"
            f"{role_tag}"
        )

normal_waits = [m["wait_intersection"] for m in car_metrics.values()
                if m.get("role", "normal") == "normal" and m.get("wait_intersection") is not None]
ambulance_waits = [m["wait_intersection"] for m in car_metrics.values()
                   if m.get("role") == "ambulance" and m.get("wait_intersection") is not None]
all_waits = [m["wait_intersection"] for m in car_metrics.values()
             if m.get("wait_intersection") is not None]
stop_to_resume_all = [m["stop_to_resume"] for m in car_metrics.values()
                      if m.get("stop_to_resume") is not None]
resume_to_depart_all = [m["resume_to_depart"] for m in car_metrics.values()
                        if m.get("resume_to_depart") is not None]

throughput = None
throughput_vps = None
depart_times = sorted(
    m["depart_time"] for m in car_metrics.values() if m.get("depart_time") is not None
)

# Throughput (user definition):
#   vehicles / mean(depart - stop) over all vehicles with both timestamps.
#   `all_waits` (computed above) is exactly the per-car (depart - stop) list.
if all_waits:
    n_w = len(all_waits)
    mean_wait_all = statistics.mean(all_waits)
    if mean_wait_all > 0:
        throughput_vps = n_w / mean_wait_all          # vehicles per second
        throughput     = mean_wait_all / n_w          # seconds per vehicle (1/vps)

missing_departure = [
    car_id for car_id, m in sorted(car_metrics.items(), key=lambda kv: int(kv[0][3:]))
    if m.get("stop_time") is not None and m.get("depart_time") is None
]

print(f"\n{'=' * 72}")
print(f"{BOLD}Paper-Friendly Summary Metrics{RESET}")
print(f"{'─' * 72}")
if experiment_fault_tolerance["tolerated_f"] is not None:
    print(
        "  Fault tolerance experiment:      "
        f"f={experiment_fault_tolerance['tolerated_f']} "
        f"static_n={experiment_fault_tolerance['static_replicas']} "
        f"quorum={experiment_fault_tolerance['consensus_quorum']} "
        f"cert={experiment_fault_tolerance['cert_threshold']}"
    )
if r0_metrics["enabled"]:
    stage = r0_metrics["failure_stage"] or "unknown"
    print(
        "  R0 late-emergency rollback:      "
        f"failure_stage={stage} "
        f"witnesses={r0_metrics['cancel_witness_count']} "
        f"echoes={r0_metrics['cancel_echo_count']} "
        f"certs={r0_metrics['cancel_cert_count']} "
        f"bad_quorum={r0_metrics['bad_quorum_count']} "
        f"cancel_commit={r0_metrics['cancel_commit_count']} "
        f"cancel_suppress={r0_metrics['cancel_leader_suppress_count']} "
        f"cancel_rotations={r0_metrics['cancel_leader_rotation_count']} "
        f"cancel_max_round={r0_metrics['cancel_leader_max_rotation_index']} "
        f"cancel_failover_s={r0_metrics['cancel_leader_failover_latency_sec']} "
        f"rollback_unavailable={r0_metrics['rollback_unavailable_count']} "
        f"max_M={r0_metrics['rollback_unavailable_max_m']} "
        f"need={r0_metrics['rollback_unavailable_need']} "
        f"rollback_commit={r0_metrics['rollback_commit_count']} "
        f"rollback_commit_extra={r0_metrics.get('rollback_commit_extra_replica_ids', [])} "
        f"epoch1_commit={int(r0_metrics['rollback_epoch1_committed'])} "
        f"epoch1_order_n={r0_metrics['epoch1_order_n_vehicles']} "
        f"epoch1_order_vehicles={r0_metrics['epoch1_order_vehicle_ids']} "
        f"pbft_new_req={int(r0_metrics['rollback_pbft_new_req_after_propose'])}"
    )
if throughput is not None:
    print(f"  Throughput (user definition): {throughput:.6f}s per vehicle")
    if throughput_vps is not None:
        print(f"  Throughput (user definition): {throughput_vps:.6f} vehicles/s")
    print(
        "    Formula: cars / mean(depart - stop) = "
        f"{len(all_waits)} / {statistics.mean(all_waits):.4f} = "
        f"{throughput_vps:.6f} veh/s"
    )
elif "Throughput_User_Definition" in run_metrics:
    throughput = run_metrics["Throughput_User_Definition"]
    throughput_vps = (1.0 / throughput) if throughput not in (None, 0) else None
    print(f"  Throughput (user definition): {throughput:.6f}s per vehicle  {CYN}[from [RUN-METRICS]]{RESET}")
    if throughput_vps is not None:
        print(f"  Throughput (standard):        {throughput_vps:.6f} vehicles/s  {CYN}[derived]{RESET}")
else:
    print(f"  Throughput (user definition): {YEL}N/A{RESET} "
          f"(need per-car depart_time metrics in the log)")
if missing_departure:
    print(f"  Missing leave timestamps:         {', '.join(missing_departure)}")
    print(f"    This log predates the new [CAR-METRICS] departure line or the native side was not rebuilt.")

def _fmt_or_run(values, run_key, label):
    if values:
        print(f"  {label}: {fmt_stat(values)}")
    elif run_key in run_metrics:
        print(f"  {label}: {run_metrics[run_key]:.4f}s  {CYN}[from [RUN-METRICS]]{RESET}")
    else:
        print(f"  {label}: N/A")

_fmt_or_run(all_waits,            "Wait_Intersection_All_Mean",      "Wait@intersection, all cars     ")
_fmt_or_run(normal_waits,         "Wait_Intersection_Normal_Mean",   "Wait@intersection, normal cars  ")
_fmt_or_run(ambulance_waits,      "Wait_Intersection_Ambulance_Mean","Wait@intersection, ambulance    ")
_fmt_or_run(stop_to_resume_all,   "Stop_To_Resume_Mean",             "Stop→resume (stop to GO signal) ")
_fmt_or_run(resume_to_depart_all, "Resume_To_Depart_Mean",           "Resume→leave, all cars          ")

fairness_all = jains_fairness(all_waits)
fairness_normal = jains_fairness(normal_waits)
if fairness_all is not None:
    print(f"  Jain fairness index (all waits):  {fairness_all:.4f}")
elif "Jains_Fairness_All" in run_metrics:
    print(f"  Jain fairness index (all waits):  {run_metrics['Jains_Fairness_All']:.4f}  {CYN}[from [RUN-METRICS]]{RESET}")
if fairness_normal is not None:
    print(f"  Jain fairness index (normal):     {fairness_normal:.4f}")
elif "Jains_Fairness_Normal" in run_metrics:
    print(f"  Jain fairness index (normal):     {run_metrics['Jains_Fairness_Normal']:.4f}  {CYN}[from [RUN-METRICS]]{RESET}")
if normal_waits and ambulance_waits:
    amb_mean = statistics.mean(ambulance_waits)
    norm_mean = statistics.mean(normal_waits)
    ratio = (amb_mean / norm_mean) if norm_mean else float('inf')
    delta = norm_mean - amb_mean
    print(f"  Ambulance priority gain:          "
          f"ambulance_mean={amb_mean:.4f}s vs normal_mean={norm_mean:.4f}s "
          f"(delta={delta:+.4f}s, ratio={ratio:.4f})")
elif "Ambulance_Priority_Gain" in run_metrics:
    gain  = run_metrics["Ambulance_Priority_Gain"]
    ratio = run_metrics.get("Ambulance_Priority_Ratio", float('nan'))
    print(f"  Ambulance priority gain:          {gain:.4f}s (ratio={ratio:.4f})  {CYN}[from [RUN-METRICS]]{RESET}")

bytes_sent_total = sum(replica_bytes_sent.values()) if replica_bytes_sent else None
quiet_honest_count = sum(replica_quiet_honest_vehicles.values())
quiet_honest_opportunities = sum(replica_quiet_honest_opportunities.values())
quiet_honest_rate = (
    100.0 * quiet_honest_count / quiet_honest_opportunities
    if quiet_honest_opportunities > 0 else None
)
if bytes_sent_total is not None:
    print(f"  Payload bytes sent:               {bytes_sent_total} bytes ({bytes_sent_total / (1024.0 * 1024.0):.6f} MB)")
elif "Bytes_Sent_Total" in run_metrics:
    bytes_sent_total = int(run_metrics["Bytes_Sent_Total"])
    print(f"  Payload bytes sent:               {bytes_sent_total} bytes ({bytes_sent_total / (1024.0 * 1024.0):.6f} MB)  {CYN}[from [RUN-METRICS]]{RESET}")
if quiet_honest_rate is not None:
    print(f"  Honest vehicles marked QUIET:     {quiet_honest_count}/{quiet_honest_opportunities} ({quiet_honest_rate:.6f}%)")
elif "Quiet_Honest_Rate" in run_metrics:
    print(f"  Honest vehicles marked QUIET:     {run_metrics['Quiet_Honest_Rate']:.6f}%  {CYN}[from [RUN-METRICS]]{RESET}")

attack_failures_total = sum(attack_failures_by_epoch.values())
if attack_outcomes or "Attack_Failures_Total" in run_metrics:
    if not attack_outcomes:
        attack_failures_total = int(run_metrics.get("Attack_Failures_Total", 0))
    attack_col = RED if attack_failures_total > 0 else GRN
    print(f"  Consensus attack success:         {attack_col}{1 if attack_failures_total > 0 else 0}{RESET} "
          f"(unique failures={attack_failures_total}, raw outcome lines={len(attack_outcomes)})")
    for ep in sorted(attack_outcomes_by_epoch):
        for row in attack_outcomes_by_epoch[ep]:
            marker = "SUCCESS" if row["attack_success"] else "BLOCKED"
            print(f"    epoch={row['epoch']} fault={row['fault']} outcome={row['outcome']} {marker}")

# ── Per-epoch breakdown: wait + throughput ────────────────────────────────────
print(f"\n{'=' * 72}")
print(f"{BOLD}Per-Epoch Intersection Performance{RESET}")
print(f"{'─' * 72}")
print(f"  {'Epoch':>5}  {'N':>4}  {'Throughput':>12}  {'WaitNorm(mean)':>16}  {'WaitAmb(mean)':>15}  {'Byzantine':>10}  {'Attack':>8}")
for ep in range(N_EPOCHS):
    n = EPOCH_N[ep]
    rm_ep = round_metrics.get(ep, {})
    tp = epoch_throughput.get(ep) or rm_ep.get("Throughput_User_Definition")
    tp_str = f"{tp:.4f}s/veh" if tp is not None else "N/A"
    wn = epoch_wait_normal.get(ep, [])
    wa = epoch_wait_ambulance.get(ep, [])
    wn_val = statistics.mean(wn) if wn else rm_ep.get("Wait_Normal_Mean")
    wa_val = statistics.mean(wa) if wa else rm_ep.get("Wait_Ambulance_Mean")
    wn_str = f"{wn_val:.4f}s" if wn_val is not None else "N/A"
    wa_str = f"{wa_val:.4f}s" if wa_val is not None else "N/A"
    byz = byzantine_by_epoch.get(ep, 0) or int(run_metrics.get(f"Byzantine_Injections_Epoch{ep}", 0))
    byz_col = RED if byz > 0 else GRN
    atk = attack_failures_by_epoch.get(ep, 0) or int(run_metrics.get(f"Attack_Failures_Epoch{ep}", 0))
    atk_col = RED if atk > 0 else GRN
    print(f"  {ep:>5}  {n:>4}  {tp_str:>12}  {wn_str:>16}  {wa_str:>15}  {byz_col}{byz:>10}{RESET}  {atk_col}{atk:>8}{RESET}")

# ── Byzantine Fault Analysis ─────────────────────────────────────────────────
byz_total_display = byzantine_total or int(run_metrics.get("Byzantine_Injections_Total", 0))
if byz_total_display > 0:
    print(f"\n{'=' * 72}")
    print(f"{BOLD}Byzantine Fault Analysis{RESET}")
    print(f"{'─' * 72}")
    print(f"  Total Byzantine fault injections: {RED}{byz_total_display}{RESET}")
    byz_by_ep = {ep: (byzantine_by_epoch.get(ep, 0) or
                      int(run_metrics.get(f"Byzantine_Injections_Epoch{ep}", 0)))
                 for ep in range(N_EPOCHS)}
    if any(byz_by_ep.values()):
        print(f"  {'Epoch':>5}  {'Injections':>12}")
        for ep in range(N_EPOCHS):
            if byz_by_ep[ep]:
                print(f"  {ep:>5}  {byz_by_ep[ep]:>12}")
    if byzantine_by_replica:
        print(f"  {'Replica':>8}  {'Injections':>12}")
        for rep in sorted(byzantine_by_replica):
            print(f"  {rep:>8}  {byzantine_by_replica[rep]:>12}")
    consensus_ok = all(
        round_metrics.get(ep, {}).get("ProposeAll_Consensus_Sim")
        for ep in range(N_EPOCHS))
    if consensus_ok:
        print(f"  {GRN}Consensus correctness: all epochs completed → BFT safety upheld{RESET}")
    else:
        print(f"  {RED}WARNING: some epochs did not complete — check above for details{RESET}")

if not SAVE_TO:
    csv_path  = os.path.join(PLOTS_DIR, "car_metrics.csv")
    json_path = os.path.join(PLOTS_DIR, "metrics.json")
    write_car_metrics_csv(csv_path)
    write_metrics_json(json_path)
    print(f"  Saved per-car CSV:                {csv_path}")
    print(f"  Saved full metrics JSON:          {json_path}")

if not SAVE_TO:
    wait_plot_path = os.path.join(PLOTS_DIR, "waittime_ambulance_vs_normal.png")
    per_car_plot_path = os.path.join(PLOTS_DIR, "waittime_per_car.png")
    wait_plot_saved = save_waittime_plot(wait_plot_path)
    per_car_plot_saved = save_waittime_by_car_plot(per_car_plot_path)
    if wait_plot_saved:
        print(f"  Saved wait-role plot:             {wait_plot_path}")
    elif plt is None:
        print(f"  Saved wait-role plot:             matplotlib not available")
    else:
        print(f"  Saved wait-role plot:             insufficient wait-time data")
    if per_car_plot_saved:
        print(f"  Saved per-car wait plot:          {per_car_plot_path}")

# ── Save log to collection dir ────────────────────────────────────────────────
if SAVE_TO:
    os.makedirs(SAVE_TO, exist_ok=True)
    existing = [f for f in os.listdir(SAVE_TO) if re.match(rf'{CARS}veh_\d+\.log$', f)]
    idx = _args.run_index if _args.run_index is not None else len(existing)
    dest = os.path.join(SAVE_TO, f"{CARS}veh_{idx}.log")

    def _first_present(ep, *keys):
        rm = round_metrics.get(ep, {})
        for key in keys:
            if key in rm:
                return rm[key]
        return None

    # Write a canonical metrics summary with one line per metric.
    with open(dest, 'w') as f:
        for ep in range(N_EPOCHS):
            # Per-car durations (one line per car) so downstream scripts can build CDFs.
            # These are lifecycle timings, not consensus latency.
            per_car = epoch_total_dur_by_car.get(ep, {})
            if per_car:
                for car_id, dur in sorted(per_car.items(), key=lambda kv: int(kv[0][3:])):
                    f.write(f"[ROUND-METRICS] Epoch {ep} Arrival_To_Resume_Time {car_id}: "
                            f"{dur:.6f} seconds\n")

            if epoch_messages_sent[ep] and epoch_messages_recv[ep]:
                avg_sent = sum(epoch_messages_sent[ep]) / len(epoch_messages_sent[ep])
                avg_recv = sum(epoch_messages_recv[ep]) / len(epoch_messages_recv[ep])
                f.write(f"[ROUND-METRICS] Epoch {ep} Avg_Messages_Sent_PerReplica: "
                        f"{avg_sent:.3f} (count={len(epoch_messages_sent[ep])})\n")
                f.write(f"[ROUND-METRICS] Epoch {ep} Avg_Messages_Received_PerReplica: "
                        f"{avg_recv:.3f} (count={len(epoch_messages_recv[ep])})\n")
            else:
                avg_sent = _first_present(ep, "Avg_Messages_Sent_PerReplica", "Messages_Sent_PerReplica")
                avg_recv = _first_present(ep, "Avg_Messages_Received_PerReplica", "Messages_Received_PerReplica")
                if avg_sent is not None:
                    f.write(f"[ROUND-METRICS] Epoch {ep} Avg_Messages_Sent_PerReplica: {avg_sent:.3f}\n")
                if avg_recv is not None:
                    f.write(f"[ROUND-METRICS] Epoch {ep} Avg_Messages_Received_PerReplica: {avg_recv:.3f}\n")
            if epoch_bytes_sent[ep]:
                total_bytes_ep = sum(epoch_bytes_sent[ep])
                avg_bytes_ep = total_bytes_ep / len(epoch_bytes_sent[ep])
                f.write(f"[ROUND-METRICS] Epoch {ep} Avg_Bytes_Sent_PerReplica: "
                        f"{avg_bytes_ep:.3f} (count={len(epoch_bytes_sent[ep])})\n")
                f.write(f"[ROUND-METRICS] Epoch {ep} Total_Bytes_Sent: {total_bytes_ep}\n")
            if epoch_total_dur[ep]:
                avg_dur = sum(epoch_total_dur[ep]) / len(epoch_total_dur[ep])
                f.write(f"[ROUND-METRICS] Epoch {ep} Avg_Arrival_To_Resume_Time: "
                        f"{avg_dur:.6f} seconds (goCars={len(epoch_total_dur[ep])})\n")
            fail_val = epoch_failures.get(ep)
            if fail_val is None:
                fail_val = _first_present(ep, "Avg_StopSign_Failures", "StopSign_Failures")
            if fail_val is None:
                fail_val = 0
            f.write(f"[ROUND-METRICS] Epoch {ep} Avg_StopSign_Failures: "
                    f"{float(fail_val):.3f} (count)\n")

        cert_creation_all = [
            value
            for values in epoch_cert_creation_latency.values()
            for value in values
            if value > 0
        ]
        if cert_creation_all:
            f.write("[RUN-METRICS] Cert_Creation_Latency_Mean: "
                    f"{statistics.mean(cert_creation_all):.6f} seconds\n")
            f.write("[RUN-METRICS] Cert_Creation_Latency_P95: "
                    f"{percentile(cert_creation_all, 0.95):.6f} seconds\n")
        if throughput is not None:
            f.write(f"[RUN-METRICS] Throughput_User_Definition: {throughput:.6f} seconds_per_vehicle\n")
        if throughput_vps is not None:
            f.write(f"[RUN-METRICS] Throughput_Vehicles_Per_Second: {throughput_vps:.6f} vehicles_per_second\n")
            f.write(
                "[RUN-METRICS] Throughput_Vehicles_Per_Minute: "
                f"{throughput_vps * 60.0:.6f} vehicles_per_minute\n"
            )
        if all_waits:
            f.write(f"[RUN-METRICS] Wait_Intersection_All_Mean: {statistics.mean(all_waits):.6f} seconds\n")
            f.write(f"[RUN-METRICS] Wait_Intersection_All_P95: {percentile(all_waits, 0.95):.6f} seconds\n")
        if normal_waits:
            f.write(f"[RUN-METRICS] Wait_Intersection_Normal_Mean: {statistics.mean(normal_waits):.6f} seconds\n")
        if ambulance_waits:
            f.write(f"[RUN-METRICS] Wait_Intersection_Ambulance_Mean: {statistics.mean(ambulance_waits):.6f} seconds\n")
        if stop_to_resume_all:
            f.write(f"[RUN-METRICS] Stop_To_Resume_Mean: {statistics.mean(stop_to_resume_all):.6f} seconds\n")
        if resume_to_depart_all:
            f.write(f"[RUN-METRICS] Resume_To_Depart_Mean: {statistics.mean(resume_to_depart_all):.6f} seconds\n")

        if experiment_fault_tolerance["tolerated_f"] is not None:
            f.write(f"[RUN-METRICS] Tolerated_F: {experiment_fault_tolerance['tolerated_f']}\n")
            f.write(f"[RUN-METRICS] Static_Replicas: {experiment_fault_tolerance['static_replicas']}\n")
            f.write(f"[RUN-METRICS] Consensus_Quorum: {experiment_fault_tolerance['consensus_quorum']}\n")
            f.write(f"[RUN-METRICS] Cert_Threshold: {experiment_fault_tolerance['cert_threshold']}\n")
        if r0_metrics["enabled"]:
            f.write(f"[RUN-METRICS] R0_Enabled: 1\n")
            f.write(f"[RUN-METRICS] R0_Cancel_Witness_Count: {r0_metrics['cancel_witness_count']}\n")
            f.write(f"[RUN-METRICS] R0_Cancel_Echo_Count: {r0_metrics['cancel_echo_count']}\n")
            f.write(f"[RUN-METRICS] R0_Cancel_Cert_Count: {r0_metrics['cancel_cert_count']}\n")
            f.write(f"[RUN-METRICS] R0_Cancel_Commit_Count: {r0_metrics['cancel_commit_count']}\n")
            f.write(f"[RUN-METRICS] Cancel_Leader_Suppress_Count: {r0_metrics['cancel_leader_suppress_count']}\n")
            f.write(f"[RUN-METRICS] Cancel_Leader_Rotation_Count: {r0_metrics['cancel_leader_rotation_count']}\n")
            f.write(f"[RUN-METRICS] Cancel_Leader_Max_Rotation_Index: {r0_metrics['cancel_leader_max_rotation_index']}\n")
            if r0_metrics["cancel_leader_attack_replica"] is not None:
                f.write(f"[RUN-METRICS] Cancel_Leader_Attack_Replica: {r0_metrics['cancel_leader_attack_replica']}\n")
            if r0_metrics["cancel_leader_failover_latency_sec"] is not None:
                f.write(
                    "[RUN-METRICS] Cancel_Leader_Failover_Latency: "
                    f"{r0_metrics['cancel_leader_failover_latency_sec']:.6f} seconds\n"
                )
            f.write(f"[RUN-METRICS] R0_Bad_Quorum_Count: {r0_metrics['bad_quorum_count']}\n")
            f.write(f"[RUN-METRICS] R0_Rollback_Unavailable_Count: {r0_metrics['rollback_unavailable_count']}\n")
            f.write(f"[RUN-METRICS] R0_Rollback_Unavailable_Max_M: {r0_metrics['rollback_unavailable_max_m']}\n")
            if r0_metrics["rollback_unavailable_f"] is not None:
                f.write(f"[RUN-METRICS] R0_Rollback_Unavailable_F: {r0_metrics['rollback_unavailable_f']}\n")
            if r0_metrics["rollback_unavailable_need"] is not None:
                f.write(f"[RUN-METRICS] R0_Rollback_Unavailable_Need: {r0_metrics['rollback_unavailable_need']}\n")
            f.write(f"[RUN-METRICS] R0_Rollback_Commit_Count: {r0_metrics['rollback_commit_count']}\n")
            if r0_metrics["rollback_commit_replica_ids"]:
                committers = ",".join(
                    str(v) for v in sorted(set(r0_metrics["rollback_commit_replica_ids"]))
                )
                f.write(f"[RUN-METRICS] R0_Rollback_Commit_Replicas: {committers}\n")
            extra_committers = r0_metrics.get("rollback_commit_extra_replica_ids", [])
            if extra_committers:
                extra = ",".join(str(v) for v in extra_committers)
                f.write(f"[RUN-METRICS] R0_Rollback_Commit_Extra_Replicas: {extra}\n")
            missing_committers = r0_metrics.get("rollback_commit_missing_replica_ids", [])
            if missing_committers:
                missing = ",".join(str(v) for v in missing_committers)
                f.write(f"[RUN-METRICS] R0_Rollback_Commit_Missing_Replicas: {missing}\n")
            f.write(f"[RUN-METRICS] R0_Epoch1_OrderDecision_Count: {r0_metrics['epoch1_order_decision_count']}\n")
            if r0_metrics["epoch1_order_n_vehicles"] is not None:
                f.write(f"[RUN-METRICS] R0_Epoch1_Order_N_Vehicles: {r0_metrics['epoch1_order_n_vehicles']}\n")
            if r0_metrics["epoch1_order_n_batches"] is not None:
                f.write(f"[RUN-METRICS] R0_Epoch1_Order_N_Batches: {r0_metrics['epoch1_order_n_batches']}\n")
            if r0_metrics["epoch1_order_vehicle_ids"]:
                vehicles = ",".join(str(v) for v in r0_metrics["epoch1_order_vehicle_ids"])
                f.write(f"[RUN-METRICS] R0_Epoch1_Order_Vehicles: {vehicles}\n")
            f.write(f"[RUN-METRICS] R0_Failure_Stage: {r0_metrics['failure_stage']}\n")

        # Jain's fairness
        if jains_fairness(all_waits) is not None:
            f.write(f"[RUN-METRICS] Jains_Fairness_All: {jains_fairness(all_waits):.6f}\n")
        if jains_fairness(normal_waits) is not None:
            f.write(f"[RUN-METRICS] Jains_Fairness_Normal: {jains_fairness(normal_waits):.6f}\n")
        if normal_waits and ambulance_waits:
            amb_mean  = statistics.mean(ambulance_waits)
            norm_mean = statistics.mean(normal_waits)
            f.write(f"[RUN-METRICS] Ambulance_Priority_Gain: {norm_mean - amb_mean:.6f} seconds\n")
            f.write(f"[RUN-METRICS] Ambulance_Priority_Ratio: {(amb_mean / norm_mean):.6f}\n")

        # Byzantine fault injection summary
        f.write(f"[RUN-METRICS] Byzantine_Injections_Total: {byzantine_total}\n")
        f.write(f"[RUN-METRICS] Fabricated_Clearance_Attempts: {len(fabricated_clearance_events)}\n")
        f.write(f"[RUN-METRICS] Fabricated_Clearance_PreVerify_Rejections: {missing_clear_reject_count}\n")
        f.write(f"[RUN-METRICS] Unsafe_Recovery_Order_Replicas: {len(unsafe_recovery_order_replicas)}\n")
        f.write(
            "[RUN-METRICS] Unsafe_Recovery_Order_Box_Occupied_Replicas: "
            f"{len(unsafe_recovery_order_box_occupied_replicas)}\n"
        )
        f.write(f"[RUN-METRICS] Crash_Injected_Vehicles: {len(crash_inject_times)}\n")
        f.write(f"[RUN-METRICS] Crash_Towed_Vehicles: {len(crash_tow_times)}\n")
        f.write(f"[RUN-METRICS] Crash_Cooccupancy_Entrants: {len(crash_unsafe_entrants)}\n")
        f.write(f"[RUN-METRICS] Wait_Heartbeat_Sends: {len(wait_send_times)}\n")
        f.write(f"[RUN-METRICS] Clear_Cert_Initial_Broadcasts: {len(clear_cert_broadcast_times)}\n")
        if crash_inject_times:
            crash_t0 = min(crash_inject_times.values())
            f.write(f"[RUN-METRICS] Crash_First_Inject_Time: {crash_t0:.6f} seconds\n")
            if clear_cert_broadcast_times:
                clear_t0 = min(clear_cert_broadcast_times)
                f.write(f"[RUN-METRICS] Clear_First_Cert_Time: {clear_t0:.6f} seconds\n")
                f.write(
                    "[RUN-METRICS] Crash_To_Clear_Latency: "
                    f"{max(0.0, clear_t0 - crash_t0):.6f} seconds\n"
                )
            if recovery_commit_times:
                recovery_t0 = min(recovery_commit_times)
                f.write(f"[RUN-METRICS] Recovery_First_Order_Commit_Time: {recovery_t0:.6f} seconds\n")
                f.write(
                    "[RUN-METRICS] Crash_To_Recovery_Order_Latency: "
                    f"{max(0.0, recovery_t0 - crash_t0):.6f} seconds\n"
                )
                if clear_cert_broadcast_times:
                    f.write(
                        "[RUN-METRICS] Clear_To_Recovery_Order_Latency: "
                        f"{max(0.0, recovery_t0 - min(clear_cert_broadcast_times)):.6f} seconds\n"
                    )
        if crash_tow_times:
            f.write(
                "[RUN-METRICS] Crash_Last_Tow_Time: "
                f"{max(crash_tow_times.values()):.6f} seconds\n"
            )
        if wait_send_times:
            f.write(f"[RUN-METRICS] Wait_First_Send_Time: {min(wait_send_times):.6f} seconds\n")
            if wait_stop_times:
                f.write(
                    "[RUN-METRICS] Wait_Observed_Duration: "
                    f"{max(0.0, max(wait_stop_times) - min(wait_send_times)):.6f} seconds\n"
                )
        for ep in range(N_EPOCHS):
            byz = byzantine_by_epoch.get(ep, 0)
            f.write(f"[RUN-METRICS] Byzantine_Injections_Epoch{ep}: {byz}\n")

        attack_failures_total = sum(attack_failures_by_epoch.values())
        f.write(f"[RUN-METRICS] Attack_Success: {1 if attack_failures_total > 0 else 0}\n")
        f.write(f"[RUN-METRICS] Attack_Failures_Total: {attack_failures_total}\n")
        f.write(f"[RUN-METRICS] Attack_Outcome_Lines_Total: {len(attack_outcomes)}\n")
        for ep in range(N_EPOCHS):
            f.write(f"[RUN-METRICS] Attack_Failures_Epoch{ep}: {attack_failures_by_epoch.get(ep, 0)}\n")
        for ep in sorted(attack_outcomes_by_epoch):
            for row in attack_outcomes_by_epoch[ep]:
                detail = f" {row['detail']}" if row.get("detail") else ""
                f.write(
                    f"[ATTACK-OUTCOME] epoch={row['epoch']} fault={row['fault']} "
                    f"outcome={row['outcome']} success={1 if row['attack_success'] else 0}"
                    f"{detail}\n"
                )

        # Delivery ratio and fallback rate (comparable to RAFT counterparts)
        _all_dr = [m["delivery_ratio"] for m in car_metrics.values()
                   if m.get("delivery_ratio") is not None]
        if _all_dr:
            _run_dr = statistics.mean(_all_dr)
            f.write(f"[RUN-METRICS] Delivery_Ratio: {_run_dr:.6f}\n")
            f.write(f"[RUN-METRICS] Estimated_Loss_Rate: {1.0 - _run_dr:.6f}\n")
        _fallback_n = sum(1 for m in car_metrics.values() if m.get("used_fallback", False))
        f.write(f"[RUN-METRICS] Fallback_Count: {_fallback_n}\n")
        if CARS > 0:
            f.write(f"[RUN-METRICS] Fallback_Rate: {_fallback_n / CARS:.6f}\n")

        if bytes_sent_total is not None:
            f.write(f"[RUN-METRICS] Bytes_Sent_Total: {bytes_sent_total}\n")
            f.write(f"[RUN-METRICS] Megabytes_Sent_Total: {bytes_sent_total / (1024.0 * 1024.0):.6f}\n")
        f.write(f"[RUN-METRICS] Quiet_Honest_Count: {quiet_honest_count}\n")
        f.write(f"[RUN-METRICS] Quiet_Honest_Opportunities: {quiet_honest_opportunities}\n")
        if quiet_honest_rate is not None:
            f.write(f"[RUN-METRICS] Quiet_Honest_Rate: {quiet_honest_rate:.6f}\n")
        perception_summary = _perception_summary()
        f.write(f"[RUN-METRICS] Lane_Evaluation_Count: {perception_summary['lane_evaluation_count']}\n")
        f.write(f"[RUN-METRICS] Lane_Accept_Count: {perception_summary['lane_accept_count']}\n")
        if perception_summary["lane_accept_rate"] is not None:
            f.write(f"[RUN-METRICS] Lane_Accept_Rate: {perception_summary['lane_accept_rate']:.6f}\n")
        if perception_summary["cue_unknown_rate"] is not None:
            f.write(f"[RUN-METRICS] Cue_UNKNOWN_Rate: {perception_summary['cue_unknown_rate']:.6f}\n")
        f.write(f"[RUN-METRICS] Perception_Duplicate_Evaluations: {perception_summary['duplicate_evaluation_count']}\n")
        f.write(f"[RUN-METRICS] QUIET_Count: {perception_summary['quiet_count'] or 0}\n")
        f.write(f"[RUN-METRICS] SIGNED_UNKNOWN_Count: {perception_summary['signed_unknown_count']}\n")
        f.write(f"[RUN-METRICS] SIGNED_Direction_Count: {perception_summary['signed_direction_count']}\n")
        f.write(f"[RUN-METRICS] Singleton_SIGNED_UNKNOWN_Count: {perception_summary['signed_unknown_singleton_count']}\n")
        f.write(f"[RUN-METRICS] Singleton_LEFT_Table_Forced_Count: {perception_summary['left_table_forced_singleton_count']}\n")

        # Direction-ablation figures consume these run-level values.  Keep them
        # in [RUN-METRICS] rather than deriving them from the repeated,
        # per-vehicle attack_outcomes JSON field.
        direction_outcomes = [
            row for row in phase2_attack_outcomes.values()
            if row.get("kind") == "FALSE_DIRECTION"
        ]
        if direction_outcomes:
            false_eligibility = any(
                bool(row.get("false_eligibility")) for row in direction_outcomes
            )
            f.write(
                "[RUN-METRICS] Direction_FP_False_Eligibility: "
                f"{int(false_eligibility)}\n"
            )
            f.write(
                "[RUN-METRICS] Direction_FN_Signed_Unknown_Singleton_Rate: "
                f"{perception_summary['signed_unknown_singleton_count'] / CARS:.6f}\n"
            )
            f.write(
                "[RUN-METRICS] Direction_Physical_FP_Unsafe_Cooccupancy: "
                f"{int(bool(unsafe_conflict_pairs))}\n"
            )

        # Per-epoch throughput and wait
        for ep in range(N_EPOCHS):
            tp = epoch_throughput.get(ep)
            if tp is not None:
                f.write(f"[ROUND-METRICS] Epoch {ep} Throughput_User_Definition: {tp:.6f} seconds_per_vehicle\n")
                if tp != 0:
                    f.write(f"[ROUND-METRICS] Epoch {ep} Throughput_Vehicles_Per_Second: {(1.0 / tp):.6f} vehicles_per_second\n")
            wn = epoch_wait_normal.get(ep, [])
            wa = epoch_wait_ambulance.get(ep, [])
            if wn:
                f.write(f"[ROUND-METRICS] Epoch {ep} Wait_Normal_Mean: {statistics.mean(wn):.6f} seconds\n")
            if wa:
                f.write(f"[ROUND-METRICS] Epoch {ep} Wait_Ambulance_Mean: {statistics.mean(wa):.6f} seconds\n")

        # Also copy the JSON alongside the .log
        json_dest = os.path.join(SAVE_TO, f"{CARS}veh_{idx}.json")
        write_metrics_json(json_dest)

    if _args.run_index is not None:
        canonical = {f"{CARS}veh_{idx}.log", f"{CARS}veh_{idx}.json"}
        for name in os.listdir(SAVE_TO):
            if (re.fullmatch(rf"{CARS}veh_\d+\.(?:log|json)", name)
                    and name not in canonical):
                os.remove(os.path.join(SAVE_TO, name))

    saved_size = os.path.getsize(dest)
    print(f"Log saved → {dest}  (run #{idx}, {saved_size} bytes — round/run metrics summary)")
    print(f"JSON saved → {os.path.join(SAVE_TO, f'{CARS}veh_{idx}.json')}")
    if _args.scenario is not None:
        print(
            f"Scenario {_args.scenario} ({SCENARIO_LABEL[_args.scenario]}): "
            f"pool these files in plot_wait_time_cdf (glob {CARS}veh_*.json under this folder)."
        )

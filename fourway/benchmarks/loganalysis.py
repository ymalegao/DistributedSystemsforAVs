import re
from datetime import datetime

log_file = "/tmp/bft-all-replicas.log"

# Parse PHASE_TIMER entries for each CID
phase_timers = {}

with open(log_file) as f:
    for line in f:
        if "[PHASE_TIMER]" in line:
            match_cid = re.search(r'CID=(\d+)', line)
            match_phase = re.search(r'(PROPOSE_SENT|PROPOSE_RECV|WRITE_SENT|WRITE_QUORUM|ACCEPT_QUORUM|DELIVER)', line)
            match_time = re.search(r'wall_ms=(\d+)', line)

            if match_cid and match_phase and match_time:
                cid = int(match_cid.group(1))
                phase = match_phase.group(1)
                wall_ms = int(match_time.group(1))

                if cid not in phase_timers:
                    phase_timers[cid] = {}

                if phase not in phase_timers[cid]:
                    phase_timers[cid][phase] = []

                phase_timers[cid][phase].append(wall_ms)

print("=== BFT CONSENSUS TIMING PER CID (EPOCH) ===\n")

# Map CID to epoch/description
cid_to_epoch = {
    1: "Epoch 0",
    3: "Epoch 0 (repeat)",
    5: "Epoch 1",
    7: "Epoch 2",
    9: "Epoch 3",
}

for cid in sorted(phase_timers.keys()):
    if cid in [1, 3, 5, 7, 9]:
        phases = phase_timers[cid]

        # Get min/max times for each phase
        phase_names = ['PROPOSE_SENT', 'PROPOSE_RECV', 'WRITE_SENT', 'WRITE_QUORUM', 'ACCEPT_QUORUM', 'DELIVER']

        print(f"CID={cid} ({cid_to_epoch.get(cid, 'Unknown')}):")

        start_time = None
        end_time = None

        for phase in phase_names:
            if phase in phases:
                times = phases[phase]
                min_t = min(times)
                max_t = max(times)

                if start_time is None:
                    start_time = min_t
                end_time = max_t

                # Convert to seconds (wall_ms is milliseconds)
                duration = (max_t - min_t) / 1000

                print(f"  {phase:15s}: {duration:6.3f}s (across all replicas)")

        # Total consensus time
        if start_time and end_time:
            total = (end_time - start_time) / 1000
            print(f"  TOTAL CONSENSUS:  {total:6.3f}s")

        print()
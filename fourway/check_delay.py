import re
from collections import defaultdict
sends = []
with open("/tmp/bft-all-replicas.log", "r") as f:
    for line in f:
        m = re.search(r'\[WITNESS-SEND\] Replica (\d+) sent witness response TO veh(\d+) \(replica \d+\) at t=([\d.]+)', line)
        if m:
            sends.append((int(m.group(1)), int(m.group(2)), float(m.group(3))))

by_target = defaultdict(list)
for r, t, time in sends:
    if t == 10:
        by_target[t].append((r, time))

for target, vals in by_target.items():
    print(f"Target: veh{target}")
    for r, time in sorted(vals, key=lambda x: x[1]):
        print(f"  Replica {r} sent at t={time:.6f}")

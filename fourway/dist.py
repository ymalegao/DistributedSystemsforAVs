import re
with open('/tmp/bft-all-replicas.log', 'r') as f:
    for line in f:
        m = re.search(r'\[POSITION UPDATE\] Replica 1 at time (\d+\.\d+)', line)
        if m:
            t = m.group(1)
            # get distance? Wait, distance is not in log.
            pass

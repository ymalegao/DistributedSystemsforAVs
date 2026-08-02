# With / Without RSU — ablation report

**Setup:** 4 vehicles. OFF = no units (N=4, f=1). ON = +4 static intersection units (N=8, f=2). `k` = number of PBFT-silent replicas (omission faults on node[1..k]).

**Simulation settings (identical across every cell, so OFF/ON are directly comparable):** fast channel (`config_fast.xml`, no obstacle shadowing) and a 5 ms ResDB bridge poll/tick (`transportPollInterval`/`timeTickInterval`) instead of the 1 ms default. Commit-success is invariant to these; latency and message counts are reported *under this setting* and should not be compared against runs made at 1 ms.

| k faults | committed OFF | committed ON | latency OFF (s) | latency ON (s) | msgs OFF | msgs ON |
|---|---|---|---|---|---|---|
| 0 | 100% | 100% | 0.06 | 0.08 | 943 | 1198 |
| 1 | 100% | 100% | 0.07 | 0.09 | 932 | 1144 |
| 2 | 0% | 100% | n/a | 0.10 | 295 | 1066 |
| 3 | 0% | 100% | n/a | 0.10 | 294 | 740 |

**Reading it:** the *frontier* is commit-success vs faults — OFF degrades at `k = 1` and collapses at `k = 2`, ON survives all three (the units supply the extra quorum). The *cost* is the higher `msgs sent` for ON: the price of the added fault tolerance. Note OFF's message count *falls* as `k` rises — that is silent replicas ceasing to transmit, i.e. the system dying, not efficiency.

## Who supplies the ARRIVAL_ECHOes

Measured from `received echo from <replica>` (replicas 0-3 are vehicles, 4-7 are units). This is the certificate layer, not the PBFT quorum:

| k faults | OFF: veh / unit echoes | ON: veh / unit echoes |
|---|---|---|
| 0 | 8.0 / 0.0 | 0.4 / 11.6 |
| 1 | 8.0 / 0.0 | 0.0 / 12.0 |
| 2 | 14.0 / 0.0 | 0.4 / 12.0 |
| 3 | 14.0 / 0.0 | 5.0 / 11.8 |

When no units are present, vehicles echo each other and certificates form normally. When units are present they win the race to the f+1 echo threshold, `cert_broadcast_` latches, and later vehicle echoes are dropped — so units end up carrying certificate formation. Consensus still commits either way; this is a property of the echo layer worth stating explicitly rather than an error.

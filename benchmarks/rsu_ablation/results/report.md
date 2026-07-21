# With / Without RSU — ablation report

**Setup:** 4 vehicles. OFF = no units (N=4, f=1). ON = +4 static intersection units (N=8, f=2). `k` = number of PBFT-silent replicas (omission faults on node[1..k]).

**Simulation settings (identical across every cell, so OFF/ON are directly comparable):** fast channel (`config_fast.xml`, no obstacle shadowing) and a 5 ms ResDB bridge poll/tick (`transportPollInterval`/`timeTickInterval`) instead of the 1 ms default. Commit-success is invariant to these; latency and message counts are reported *under this setting* and should not be compared against runs made at 1 ms.

| k faults | committed OFF | committed ON | latency OFF (s) | latency ON (s) | msgs OFF | msgs ON |
|---|---|---|---|---|---|---|
| 0 | 100% | 100% | 0.06 | 0.08 | 942 | 1193 |
| 1 | 100% | 100% | 0.07 | 0.09 | 932 | 1162 |
| 2 | 0% | 100% | n/a | 0.10 | 295 | 1162 |

**Reading it:** the *frontier* is commit-success vs faults — OFF collapses once `k > 1`, ON survives to `k = 2` (the units supply the extra quorum). The *cost* is the higher `msgs sent` for ON: the price of the added fault tolerance.

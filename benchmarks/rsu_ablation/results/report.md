# With / Without RSU — ablation report

**Setup:** 4 vehicles. OFF = no units (N=4, f=1). ON = +4 static intersection units (N=8, f=2). `k` = number of PBFT-silent replicas (omission faults on node[1..k]).

| k faults | committed OFF | committed ON | latency OFF (s) | latency ON (s) | msgs OFF | msgs ON |
|---|---|---|---|---|---|---|
| 0 | 100% | 100% | 0.03 | 0.05 | 936 | 1164 |
| 1 | 100% | 100% | 0.03 | 0.09 | 927 | 1161 |
| 2 | 0% | 100% | n/a | 0.06 | 295 | 1106 |

**Reading it:** the *frontier* is commit-success vs faults — OFF collapses once `k > 1`, ON survives to `k = 2` (the units supply the extra quorum). The *cost* is the higher `msgs sent` for ON: the price of the added fault tolerance.

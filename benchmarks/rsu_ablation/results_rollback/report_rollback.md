# 18-vehicle fault pressure — consensus availability with/without RSU

**Setup:** 18-vehicle late-emergency scenario. OFF = no units (N=18, active view 16, quorum 11). ON = +4 static intersection units (N=22, active view ~19-20, quorum 13). `k` = PBFT-silent vehicles (`node[1..k]`); units never go silent, and `node[16]/[17]` (late normal + ambulance) are never silenced.

**Settings:** fast channel, 5 ms bridge poll, `sim-time-limit=25s` (the outcome is decided by ~t=20s, so runs are capped rather than run to completion).

**Metric:** did an epoch-0 ORDER commit at all? If not, vehicles cross via the stop-sign **timeout fallback** — BFT agreement bypassed, the safety-relevant degradation this protocol exists to prevent.

**Caveat on the fallback column:** the intended `sim-time-limit = 25s` does *not* actually apply (the generated ini sets it under `[General]`, but the scenario's own `[Config ...]` section is more specific and wins), so runs continue to t≈65-77s and some are cut off by the wall-clock timeout instead. The **committed** column is robust — the outcome is decided by t≈20s, which every run reaches. The **fallback count** is not directly comparable across cells, since it keeps accumulating for as long as a given run happened to survive.

**Rep counts vary per k** (n shown below): the k=0/4/5/6 cells were re-run with 3 reps, while k=1/2/3 retain 2 reps from the earlier sweep of the same script and settings. Weight the cells accordingly.

| k silent | consensus OFF (n) | consensus ON (n) | fallbacks OFF | fallbacks ON |
|---|---|---|---|---|
| 0 | 100% (3) | 100% (3) | 0.0 | 0.0 |
| 1 | 100% (2) | 100% (2) | 0.0 | 0.0 |
| 2 | 100% (2) | 100% (2) | 0.0 | 0.0 |
| 3 | 100% (2) | 100% (2) | 0.0 | 0.0 |
| 4 | 67% (3) | 100% (3) | 3.0 | 0.0 |
| 5 | 67% (3) | 100% (3) | 5.3 | 0.0 |
| 6 | 0% (3) | 100% (3) | 16.0 | 0.0 |
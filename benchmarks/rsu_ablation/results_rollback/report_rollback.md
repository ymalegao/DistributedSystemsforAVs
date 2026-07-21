# 18-vehicle fault pressure — consensus availability with/without RSU

**Setup:** 18-vehicle late-emergency scenario. OFF = no units (N=18, active view 16, quorum 11). ON = +4 static intersection units (N=22, active view ~19-20, quorum 13). `k` = PBFT-silent vehicles (`node[1..k]`); units never go silent, and `node[16]/[17]` (late normal + ambulance) are never silenced.

**Settings:** fast channel, 5 ms bridge poll, `sim-time-limit=25s` (the outcome is decided by ~t=20s, so runs are capped rather than run to completion).

**Metric:** did an epoch-0 ORDER commit at all? If not, vehicles cross via the stop-sign **timeout fallback** — BFT agreement bypassed, the safety-relevant degradation this protocol exists to prevent.

| k silent | consensus OFF | consensus ON | fallbacks OFF | fallbacks ON |
|---|---|---|---|---|
| 0 | 100% | 100% | 0.0 | 0.0 |
| 1 | 100% | 100% | 0.0 | 0.0 |
| 2 | 100% | 100% | 0.0 | 0.0 |
| 3 | 100% | 100% | 0.0 | 0.0 |
| 4 | 100% | 100% | 0.0 | 0.0 |
| 5 | 0% | 100% | 8.0 | 0.0 |
| 6 | 0% | 100% | 16.0 | 0.0 |
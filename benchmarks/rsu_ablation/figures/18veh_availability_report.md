# 18-vehicle fault pressure — consensus availability with/without RSU

**Setup:** 18-vehicle late-emergency scenario. OFF = no units (N=18, active view 16, quorum 11). ON = +4 static intersection units (N=22, active view ~19-20, quorum 13). `k` = PBFT-silent vehicles (`node[1..k]`); units never go silent, and `node[16]/[17]` (late normal + ambulance) are never silenced.

**Settings:** fast channel, 5 ms bridge poll, hard `--sim-time-limit=30s` (applied on the command line, so it actually takes effect), **6 reps per k**.

**Metric:** did an epoch-0 ORDER commit at all? If not, vehicles cross via the stop-sign **timeout fallback** — BFT agreement bypassed, the safety-relevant degradation this protocol exists to prevent.

**Caveat on the fallback column:** every run now ends cleanly at the 30 s cap, but the stop-sign timeout fallbacks only fire *later* (t>30 in the uncapped scenario), so the fallback column is uniformly ~0 here and is **not** meaningful. Use the **committed** column — it is decided by t≈20s, which every run reaches, and is robust.

| k silent | consensus OFF (n) | consensus ON (n) | fallbacks OFF | fallbacks ON |
|---|---|---|---|---|
| 0 | 100% (6) | 100% (6) | 0.0 | 0.0 |
| 1 | 100% (6) | 100% (6) | 0.0 | 0.0 |
| 2 | 100% (6) | 100% (6) | 0.0 | 0.0 |
| 3 | 100% (6) | 100% (6) | 0.0 | 0.0 |
| 4 | 100% (6) | 100% (6) | 0.0 | 0.0 |
| 5 | 50% (6) | 100% (6) | 0.0 | 0.0 |
| 6 | 0% (6) | 100% (6) | 0.0 | 0.0 |
| 7 | 0% (6) | 83% (6) | 0.0 | 0.0 |
| 8 | 0% (6) | 0% (6) | 0.0 | 0.0 |
# Emergency rollback pipeline vs fault pressure (with/without RSU)

**Conditional funnel** — each stage is the fraction of runs that reached the *previous* stage. ep0 = epoch-0 committed (of all runs; the rollback prerequisite). trig = rollback triggered / ambulance witnessed (of committed runs) — this is the **intermittent late-ambulance trigger**, so it varies by luck, not fault level. cc = cancellation BFT-committed (of triggered runs); reord = re-order started (of cancelled runs). So cc/reord answer *'once triggered, does the rollback complete?'* and are ~100% whenever consensus is healthy, regardless of the flaky trigger.

| k | arm | ep0 (all) | trig (of ep0) | cc (of trig) | reord (of cc) | n |
|---|---|---|---|---|---|---|
| 0 | OFF | 100% | 100% | 67% | 100% | 6 |
| 0 | ON | 100% | 33% | 100% | 100% | 6 |
| 1 | OFF | 100% | 100% | 100% | 100% | 6 |
| 1 | ON | 100% | 50% | 100% | 100% | 6 |
| 2 | OFF | 100% | 100% | 83% | 100% | 6 |
| 2 | ON | 100% | 67% | 100% | 100% | 6 |
| 3 | OFF | 100% | 100% | 100% | 100% | 6 |
| 3 | ON | 100% | 83% | 100% | 100% | 6 |
| 4 | OFF | 100% | 100% | 100% | 100% | 6 |
| 4 | ON | 100% | 83% | 80% | 100% | 6 |
| 5 | OFF | 50% | 67% | 100% | 100% | 6 |
| 5 | ON | 100% | 83% | 100% | 100% | 6 |
| 6 | OFF | 0% | n/a | n/a | n/a | 6 |
| 6 | ON | 100% | 83% | 100% | 100% | 6 |
| 7 | OFF | 0% | n/a | n/a | n/a | 6 |
| 7 | ON | 83% | 100% | 100% | 100% | 6 |
| 8 | OFF | 0% | n/a | n/a | n/a | 6 |
| 8 | ON | 0% | n/a | n/a | n/a | 6 |
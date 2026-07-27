# Emergency rollback pipeline vs fault pressure (with/without RSU)

Fraction of runs reaching each rollback stage. ep0=epoch-0 committed (prerequisite), wit=ambulance witnessed, **cc=cancellation BFT-committed (rollback succeeded)**, reord=re-order started. Late-ambulance trigger is intermittent, so low-k `wit/cc` can dip even when ep0=100%.

| k | arm | ep0 | wit | cc | reord | n |
|---|---|---|---|---|---|---|
| 0 | OFF | 100% | 100% | 100% | 100% | 3 |
| 0 | ON | 100% | 100% | 100% | 100% | 3 |
| 1 | OFF | 100% | 100% | 100% | 100% | 2 |
| 1 | ON | 100% | 0% | 0% | 0% | 2 |
| 2 | OFF | 100% | 100% | 50% | 50% | 2 |
| 2 | ON | 100% | 50% | 50% | 50% | 2 |
| 3 | OFF | 100% | 100% | 100% | 100% | 2 |
| 3 | ON | 100% | 50% | 50% | 50% | 2 |
| 4 | OFF | 67% | 67% | 67% | 67% | 3 |
| 4 | ON | 100% | 100% | 100% | 100% | 3 |
| 5 | OFF | 67% | 67% | 67% | 67% | 3 |
| 5 | ON | 100% | 67% | 33% | 33% | 3 |
| 6 | OFF | 0% | 0% | 0% | 0% | 3 |
| 6 | ON | 100% | 67% | 67% | 67% | 3 |
| 7 | OFF | 0% | 0% | 0% | 0% | 3 |
| 7 | ON | 100% | 67% | 67% | 67% | 3 |
| 8 | OFF | 0% | 0% | 0% | 0% | 3 |
| 8 | ON | 0% | 0% | 0% | 0% | 3 |
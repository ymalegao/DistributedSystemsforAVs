# Scaling study — consensus rate & message cost vs intersection size

4/8/12/16 vehicles, with vs without 4 RSU units, at k=0 (min) and k=frontier (max = N−quorum silent vehicles). Fast channel, 5 ms poll, hard sim-time cap so Messages_Sent (finish()-only) is emitted.

| V | arm | k_min | rate@min | msgs@min | k_max | rate@max | msgs@max |
|---|---|---|---|---|---|---|---|
| 4 | OFF | 0 | 100% | 943 | 1 | 100% | 932 |
| 4 | ON | 0 | 100% | 1164 | 3 | 100% | 1150 |
| 8 | OFF | 0 | 100% | 3234 | 3 | 100% | 3020 |
| 8 | ON | 0 | 100% | 3425 | 5 | 50% | 3378 |
| 12 | OFF | 0 | 100% | 6266 | 5 | 50% | 6324 |
| 12 | ON | 0 | 100% | 7512 | 5 | 100% | 7387 |
| 16 | OFF | 0 | 100% | 11793 | 5 | 50% | 11587 |
| 16 | ON | 0 | 100% | 12582 | 7 | 50% | 12199 |
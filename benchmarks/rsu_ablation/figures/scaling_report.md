# Scaling study — consensus rate & message cost vs intersection size

4/8/12/16 vehicles, with vs without 4 RSU units, at k=0 (min) and k=frontier (max = N−quorum silent vehicles). Fast channel, 5 ms poll, hard sim-time cap so Messages_Sent (finish()-only) is emitted.

| V | arm | k_min | rate@min | msgs@min | k_max | rate@max | msgs@max |
|---|---|---|---|---|---|---|---|
| 4 | OFF | 0 | 100% | 942 | 1 | 100% | 932 |
| 4 | ON | 0 | 100% | 1174 | 3 | 100% | 1164 |
| 8 | OFF | 0 | 100% | 3220 | 3 | 67% | 3150 |
| 8 | ON | 0 | 100% | 3441 | 5 | 67% | 3299 |
| 12 | OFF | 0 | 100% | 6320 | 5 | 67% | 6176 |
| 12 | ON | 0 | 100% | 7530 | 5 | 67% | 7442 |
| 16 | OFF | 0 | 100% | 11585 | 5 | 67% | 11617 |
| 16 | ON | 0 | 100% | 12632 | 7 | 33% | 12308 |
# Intersection Conflict Matrix (the scheduler's "smart contract")

Defines which vehicle **movements may cross the intersection simultaneously** (in the
same committed batch). Two movements that are *not* marked safe must be **serialized**
(scheduled into different batches). This is what lets our protocol pack non-conflicting
crossings in parallel — the mechanism behind the throughput win in Ablation 3.

Source of truth: `kSafe[12][4]` in
`bridge/resdb_intersection_scheduler.cc`
(`IsSafeToBatch`). Visual: [figures/conflict_matrix.png](figures/conflict_matrix.png).

## Encoding
- **Lane** (approach): `0=N, 1=S, 2=E, 3=W`
- **Direction**: `0=straight (↑), 1=left (↰), 2=right (↱)`

## Rule
Movements `A=(laneA,dirA)` and `B=(laneB,dirB)` may share a batch **iff**:
1. `laneA ≠ laneB` — two cars from the **same approach lane never** cross together, **and**
2. neither is a **left turn** (`dir=1`) — left turns always cross **alone** (conservative), **and**
3. the pair is one of the safe classes below.

## The safe-to-batch classes (12 pairs total)
| class | pairs |
|---|---|
| **Opposing straights** (2) | N↑+S↑, E↑+W↑ |
| **Any two right turns** (6) | N↱+S↱, N↱+E↱, N↱+W↱, S↱+E↱, S↱+W↱, E↱+W↱ |
| **Right + opposing straight** (4) | N↑+S↱, N↱+S↑, E↑+W↱, E↱+W↑ |

Everything else conflicts. In particular **every left turn conflicts with everything**
(it needs the intersection to itself), and same-lane movements never batch.

## Why these are safe
- **Opposing straights** (N↑ / S↑) use disjoint through-lanes — no crossing paths.
- **Right turns** hug the near corner and don't cross opposing traffic, so any two rights,
  or a right plus the opposing straight, share no conflict point.
- **Left turns** cut across opposing and cross traffic, so they are never batched — the
  safe, conservative choice.

## How it's enforced
The scheduler groups the committed arrival order into batches, adding a car to the
current batch only if it `IsSafeToBatch` with **every** car already in it; otherwise it
starts a new batch. The same table is duplicated in `ResDBDecision.cc` (`detectUnsafeBatch`)
as a safety cross-check on the committed order.

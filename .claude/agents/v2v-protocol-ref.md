# V2V BFT Protocol — Proof Reference

Full implementation notes: `docs/ARCHITECTURE.md`. The protocol source is
`src/v2vbft/`; the consensus bridge is `bridge/`.

> Written against the retired BFT-SMaRt/JNI path. The consensus engine is now
> ResilientDB PBFT in-process, so "BFT-SMaRt" below means "the consensus layer"
> and the message flow, quorum rule and evaluation baseline still hold.

## Architecture

- **Single-round `PROPOSE_ALL`** protocol. Do NOT introduce a second `invokeOrdered` call.
- **Vehicles are the replicas** — highly mobile, dynamic, subject to epoch churn.
- **Pre-consensus V2V certification phase** (per-car `ARRIVAL_CERT`) runs before BFT-SMaRt.
- Quorum requirement: **f+1 `ARRIVAL_ECHO` signatures** (f = (BATCH_SIZE−1)/3).
- Evaluation baseline: **N=16, F=5**; view-changes complete at regency 1.

## Message Flow

```
ARRIVAL_ANNOUNCE (1) → ARRIVAL_ECHO (4) → ARRIVAL_CERT (5) → PROPOSE_ALL → decision → EXECUTING (7)
```

- `ARRIVAL_ECHO` signature: `XXHash32(carId:lane:pos:dir:isAmb:echoingReplicaId)`, seed=0.
- `PROPOSE_ALL` payload: `<proposerId>:<vehicleStatesStr>:<perCarCerts>:<orderBagStr>`
- `cyberStatus`: `SIGNED` (f+1 echoes validated) or `QUIET` (failed to produce valid cert).

## Ground-Truth Anchors (Proof-Critical)

- **`physicallyObservedCars`** — TraCI-verified set; populated by `handleArrivalAnnouncement()`.
- **`collectedCerts`** — certs with f+1 valid echoes; only SIGNED cars appear here.
- **`verifyCarPosition()`** — TraCI physical check; WRONG_LANE cars recorded with actual lane but not echoed.
- **`OrderScheduler.buildProposal()`** — **deterministic**; same cert set → identical proposal. Critical for EP2.
- **`OrderRequestVerifier.isValidRequest()`** — Java-side gate; 8 checks on every follower before vote: per-car cert validation (1), no-phantom/duplicate/collision/lane-order schedule checks (2-5), Leader Rejection Rule (6), cert-omission guard via `getCertSnapshot()` JNI (7, EP5 upgrade), deterministic re-exec equality (8, anti-computation-fraud).
- **`IntersectionServer.getCertSnapshot()`** — JNI pull of C++ `collectedCerts` key set; snapshot consistent within a call, survives intra-epoch view-changes, cleared at epoch boundary by `handleWipeComplete()`.
- **Leader Rejection Rule**: any batch containing a QUIET vehicle has size=1.

## Byzantine Fault Taxonomy

| Type | Layer | Behavior | Outcome |
|------|-------|----------|---------|
| `FALSE_LANE` | C++ V2V | Announces fake lane; TraCI catches mismatch | 0 valid echoes → QUIET |
| `INVALID_SIG` | C++ V2V | Corrupted echo signature | Fails `validateArrivalCert()` → QUIET |
| `EQUIVOCATOR` | C++ V2V | Sends different directions to different peers | At most f honest peers echo the same value |
| Silent leader | Java BFT | `isByzantineNode(me)` suppresses PROPOSE in `startConsensus()` | `RequestsTimer` → `STOP` → view-change |
| Hash tampering | Java BFT | Bitwise-inverts hash in WRITE/ACCEPT | Caught by honest replicas' math check; consensus still succeeds with 2f+1 honest votes |

## View-Change (Liveness)

- Silent leader: `RequestsTimer` fires after 3 s → all followers broadcast `STOP` → view-change.
- `resetAlreadyProposed()` (in `ClientsManager`) must run in `Synchronizer.catch_up()` before `createPropose()` — ensures new leader includes the real pending PROPOSE_ALL, not an empty batch.
- `ReliableV2VMessaging.java` filters incoming unicast frames by `toReplicaId` — do NOT remove; prevents `expectedSeqNums` drift that would make STOPDATAs look like DUPLICATEs.

## EP Proof Anchors

| Property | Formal Statement | Key Mechanism | Byzantine Threat |
|----------|-----------------|---------------|-----------------|
| **EP1 Validity** | An honest replica echoes only a TraCI-verified, physically observed lane. | `verifyCarPosition()` + `physicallyObservedCars` | `FALSE_LANE` |
| **EP2 Agreement** | No two honest vehicles decide different orderings in the same epoch. | Deterministic `buildProposal()` + `perCarCerts` validation + quorum intersection | `EQUIVOCATOR` forking certs |
| **EP3 Integrity** | A vehicle decides at most once per epoch. | Epoch-ID gating + `XXHash32` signature uniqueness | Replay / double-decide |
| **EP4 Lock-in** | Once a value is locked, no view-change can alter it. | `STOPDATA` sequence number fix + `alreadyProposed` semantics in `resetAlreadyProposed()` | Byzantine new leader proposing different value |
| **EP5 Termination** | Every honest vehicle eventually decides or aborts **within epoch e** (upgraded from "within f+1 epochs"). Three cases: (a) BL includes H1 → 2f+1 WRITE votes → decide in e; (b) BL omits H1 → ≥f+1 honest followers holding H1's cert reject via Check 7 → `RequestsTimer` → view-change within e → new honest leader → decide in e; (c) BL silent → same `RequestsTimer` path → decide in e. | `RequestsTimer` + view-change recovery + `OrderRequestVerifier` Check 7 cert-omission guard (`getCertSnapshot()` JNI) + Check 8 deterministic re-exec | Silent / censoring leader |
| **EP6 Abort** | Abort is safe; does not violate Agreement or Integrity. | Leader Rejection Rule — QUIET-only batch → singleton scheduling | QUIET-only batch edge case |

## Proof Dependencies

EP1 is a prerequisite for EP2 (Agreement rests on validity of echoes).
EP4 (Lock-in) supports EP5 (Termination) — view-change cannot undo a locked value.

## Cross-Language Layer Boundaries

- **C++ (`V2VArrivalProtocol`, `V2VProxyModule`)**: physical verification, cert assembly, QUIET classification.
- **Java (`OrderRequestVerifier`, `OrderScheduler`, `ViewConsensusProtocol`)**: cert re-validation, deterministic scheduling, BFT vote admission.
- Proofs spanning layers must make the boundary explicit.


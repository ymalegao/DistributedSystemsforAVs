# Handoff: Channel silence — what we send, what turns it off, what’s broken

**Audience:** next agent fixing Scenario-16 CLEAR+ORDER collision / post-commit channel noise
**Date:** 2026-07-23
**Log evidence:** `/tmp/resdb-simulation.log` (crash run; propose ~line 239703)
**Code:** `veins-veins-5.3.1/src/veins/modules/application/resDB/`

---

## 0. Problem statement (why this handoff exists)

Scenario 15 (ambulance / `CANCEL_EMERGENCY`) completes ORDER(e+1) under TYPE11 relay load.
Scenario 16 (crash / `CANCEL_CRASH`) stacks **CLEAR evidence + WAIT teardown + epoch-1 discovery + ORDER(e+1) PBFT + TYPE11 relays** into one ~0.1s window. Wall-clock explodes (`simsec/sec ≈ 0.002`); process often dies before COMMIT propagates.

Observed in latest crash log around r1 propose (`TriggerConsensus rc=0 vehicles=14` @ line ~239703):

| After propose (next ~2k lines) | Count |
|---|---|
| `CLEAR-ECHO` (mostly recv) | ~96 |
| Extra `CLEAR-CERT` assemblers | 2 |
| `CLEAR-RELAY` | 14 |
| `WAIT-STOP` | 2 |
| `TYPE11-SEND` | 28 |
| `TYPE11-DROP` | 381 |

Discovery completing and CLEAR continuing are **independent**. CLEAR does not stop because CertPrimary proposed.

---

## 1. Wire types (Veins BFTMessage `msgType`)

| Type | Name | Role |
|---:|---|---|
| 1 | ARRIVAL_ANNOUNCE | Discovery intent |
| 4 | ARRIVAL_ECHO | Per-announce TraCI-verified echo |
| 5 | ARRIVAL_CERT | f+1 arrival certificate |
| 8 | ResDB consensus | PRE_PREPARE / PREPARE / COMMIT (signed) |
| 9 | Decision gossip | Post-ORDER straggler catch-up |
| 10 | Announce gossip | Epidemic ANN relay |
| 11 | Consensus relay | Epidemic carrier for type-8 bytes |
| 12 | CANCEL_ECHO | BLOCKED or EMERGENCY witness |
| 13 | CANCEL_CERT | f+1 cancel / blocked cert |
| 14 | Cancel-commit gossip | Post-CANCEL decision attestation |
| 15 | CLEAR_ECHO | Empty-box witness |
| 16 | CLEAR_CERT | f+1 clear cert |
| 17 | WAIT heartbeat | Advisory liveness (leader only) |

Primary files: `ResDBUtil.h`, `ResDBIntersectionApp.h` (k*Type), `ResDBArrivalProtocol.cc`, `ResDBRollbackProtocol.cc`, `ResDBTransport.cc`, `ResDBDecision.cc`, `ResDBIntersectionApp.cc`.

---

## 2. Per-message: when it starts, what stops it

### 2.1 ARRIVAL_ANNOUNCE (type 1) + periodic timer

| | |
|---|---|
| **Start** | Approach / post-cancel discovery; `broadcastArrivalAnnouncement()`; periodic `broadcastArrivalAnnouncement_timer_` |
| **Stop** | Timer cancelled when discovery leaves `COLLECTING` (`beginDiscoveryDrain`), or `cert_broadcast_` set, or non-recallable during cancel, or crash MAC cleanup |
| **Propagation-aware stop?** | No — state-machine only |
| **Notes** | Intentionally continues until local cert assembled so witnesses keep re-echoing |

### 2.2 ARRIVAL_ECHO (type 4)

| | |
|---|---|
| **Start** | On verified announce (`echoed_cars_` insert) |
| **Stop** | **One-shot per carId** (`echoed_cars_`); no retry timer |
| **Propagation-aware stop?** | N/A (single send) |

### 2.3 ARRIVAL_CERT (type 5) + cert retry + stop-zone cert gossip

| | |
|---|---|
| **Start** | On local cert assemble: `broadcastArrivalCert` + optional `cert_retry_timer_`; cert-primary also `startStopZoneCertGossip` |
| **Stop retries** | `stopCertBroadcastRetries()` on discovery COMPLETE / DRAINING+aired / proposeAll / order apply / crash / max retries |
| **Stop zone gossip** | `stopStopZoneCertGossip()` on leave COLLECTING / proposeAll / deadline |
| **Propagation-aware stop?** | **No** — no “enough peers have my cert” |

### 2.4 ANN gossip (type 10)

| | |
|---|---|
| **Start** | On announce / stop-zone pending_relays flush |
| **Stop** | `AnnouncementRelayTracker` one-shot per (epoch, carId); pending_relays capped (`relayCount >= 3`) |
| **Propagation-aware stop?** | Dedup only |

### 2.5 PBFT type 8 + consensus retry

| | |
|---|---|
| **Start** | ResDB bridge outbound → TYPE8-DRAIN |
| **Stop retries** | `ConsensusRetryManager`: drop when prepare/commit vote progress ≥ quorum (`[PBFT-RETRY-STOP]`); `clearConsensusRetries` on order/cancel commit / depart / crash |
| **Propagation-aware stop?** | **Yes** (best pattern in the codebase) |

### 2.6 TYPE11 consensus relay

| | |
|---|---|
| **Start** | On hearing eligible TYPE8 (or TYPE11): first time `consensus_relay_seen_` key → **immediate** `TYPE11-SEND` |
| **“Stop”** | Second sighting of same raw bytes → **`[TYPE11-DROP] reason=duplicate` (log only — not a wire message)** |
| **Propagation-aware stop?** | Per-packet dedup only; **no suppress-on-overhear**; comment in `maybeRelayResdbConsensusBytes` says delay queue was removed |
| **Amplification** | With N in-range hearers: ~N TYPE11-SENDs per original vote → O(N²) receives/drops. Dominant CPU tax |

### 2.7 Decision gossip TYPE9

| | |
|---|---|
| **Start** | After order apply: `triggerGossip` |
| **Stop** | `stopGossip()` on: cancel/tombstone active; **`decisionGossipPropagationConfirmed()` (gossip_acc_ ≥ f+1)**; max retries; straggler apply |
| **Propagation-aware stop?** | **Yes** (recently wired) |

### 2.8 CANCEL_ECHO (type 12)

| | |
|---|---|
| **Start** | Crash observation or ambulance announce/cert → `sendCancelEcho` |
| **Stop** | **One-shot per collector key** (`cancel_echo_sent_`) |
| **Propagation-aware stop?** | N/A |

### 2.9 CANCEL_CERT (type 13) + retry + relay

| | |
|---|---|
| **Start** | Local collector hits f+1 → `broadcastCancelCert`; peer first-seen → one `CANCEL-RELAY` |
| **Stop retries** | `stopCancelCertRetries` on: cancel commit; **`cancelCertPropagationConfirmed` (≥ f+1 distinct carriers in `cancel_cert_carriers_`)**; phase `!cancel_consensus_pending_ && !cancel_pending_`; max retries |
| **Propagation-aware stop?** | **Yes** for *retry loop*; relay still one-shot per node |

### 2.10 Cancel-commit gossip (type 14)

| | |
|---|---|
| **Start** | On CANCEL commit / gossip-adopt → `triggerCancelCommitGossip` |
| **Stop** | **`cancelGossipPropagationConfirmed()` (cancel_gossip_acc_ ≥ f+1)**; max retries |
| **Propagation-aware stop?** | **Yes** (recently wired). Acc still counts after local tombstone so sender can observe peers |

### 2.11 CLEAR_ECHO (type 15) — **main Scenario-16 pain**

| | |
|---|---|
| **Start** | Empty-box dwell on `preceding_batch_poll_msg_` → `sendClearEcho` (one-shot via `clear_echo_sent_`) |
| **Recv path** | `handleClearEcho` always accumulates; logs `count=k/6` even for **k > 6**; if `count ≥ f+1 && !clear_cert_seen_` → `broadcastClearCert` |
| **Stop sending echoes** | One-shot only — **no global “CLEAR done / ORDER proposed” hush** |
| **Stop assembling** | Only when `clear_cert_seen_` already set (skip *new* cert broadcast). Late echoes still processed/logged |
| **Propagation-aware stop?** | **No** |
| **Known lockstep** | Comment at `beginPostCancelDiscovery` (~1250): all replicas re-arm clearance poll after CANCEL commit → dwell timers fire together → CLEAR_ECHO/CERT collide with ORDER(1) PREPARE. Partial stagger added (`replicaId_ * poll_period`); **not enough** to desync from propose |

### 2.12 CLEAR_CERT (type 16) + retry + relay

| | |
|---|---|
| **Start** | Each assembler that hits threshold first locally broadcasts; peers relay once (`clear_cert_relayed_`) |
| **Stop retries** | `stopClearCertRetries()` now called immediately in `onIncidentCleared` (and on order apply / max retries) |
| **Propagation-aware stop?** | **No** — unlike CANCEL_CERT, no `clear_cert_carriers_` / f+1 carrier stop. Multiple assemblers each broadcast until they personally see a cert |
| **After propose** | Still minting/relaying while PRE_PREPARE seq=3 starts (log: extra CLEAR-CERT + 14 CLEAR-RELAY after r1 propose) |

### 2.13 WAIT heartbeat (type 17)

| | |
|---|---|
| **Start** | After epoch-1 discovery COMPLETE, cert-primary while incident BLOCKING and no CLEAR |
| **Stop** | `stopWait(reason)`: **`clear-cert`**, **`order-applied`**, leader conditions fail, follower expiry |
| **Propagation-aware stop?** | State-based (BLOCKING/CLEARED), not peer-count |
| **Note** | `[WAIT-STOP]` is a **log**, not a message. Fanout of WAIT-STOP across N replicas is teardown logging when CLEAR lands |

### Emergency vs crash (why 15 works)

`registerBlockedIncidentIfCrash` only for `CANCEL_CRASH`.
Emergency: **no INCIDENT / no WAIT / no CLEAR / no `incident-blocking` gate**.
After CANCEL commit → discovery → ORDER(e+1) immediately. TYPE11 still loud, but **no CLEAR+WAIT stack** on the propose.

---

## 3. What already has “go quiet when propagated”

| Mechanism | Confirmed by |
|---|---|
| PBFT consensus retry | Quorum vote progress |
| Decision gossip (9) | `gossip_acc_ ≥ f+1` |
| Cancel-commit gossip (14) | `cancel_gossip_acc_ ≥ f+1` |
| CANCEL_CERT retry (13) | `cancel_cert_carriers_ ≥ f+1` |

## 4. What does **not** (gaps)

| Mechanism | Gap |
|---|---|
| **CLEAR_ECHO / CLEAR_CERT** | No hush after first valid CLEAR on wire; no carrier-based retry stop; echoes accepted past threshold; many assemblers each broadcast |
| **TYPE11 relay** | Eager flood; DROP is receive-side only; O(N²) |
| ARRIVAL_CERT retry | Fixed count / phase only |
| Post-CLEAR vs propose sequencing | Propose does not suppress in-flight CLEAR; CLEAR does not wait for channel quiet / does not yield to ORDER |

---

## 5. Intended state-machine silence (product intent)

Approximate desired quiet points:

```
ANN gossip     → quiet when discovery drains / consensus starts
ARRIVAL_CERT   → quiet when propose / enough peers have certs
PBFT TYPE8/11  → quiet when commit certificate / order applied
CANCEL_*       → quiet when cancel committed + commit gossip propagated
CLEAR_*        → quiet when ≥1 valid CLEAR_CERT widely held (or ORDER carries CLEAR)
WAIT           → quiet when CLEAR or ORDER
Decision gossip→ quiet when f+1 peers seen (already)
```

Scenario 16 currently violates CLEAR quiet relative to ORDER propose.

---

## 6. Suggested fix directions (for implementing agent)

Prefer surgical, match existing patterns. Do **not** invent a second WAIT/CLEAR consensus.

### A. CLEAR hush (highest ROI for Scenario 16)

1. **Stop minting CLEAR_CERT once `clear_cert_seen_` OR incident CLEARED OR `cancel_pending_` propose already submitted** (CertPrimary or any node that adopted CLEAR from ORDER trailer).
2. Mirror CANCEL_CERT: **`clear_cert_carriers_` + `clearCertPropagationConfirmed` → `stopClearCertRetries`**.
3. In `handleClearEcho`: if already CLEARED / `clear_cert_seen_`, **return before logging/broadcast** (kill `count=9/6` spam and late assembler broadcasts).
4. Optional: once CLEARED, **ignore further CLEAR_ECHO** for that key entirely.

### B. Desync CLEAR from ORDER propose

- Stronger stagger / don’t start CLEAR dwell until WAIT has been running for X, **or**
- CertPrimary: after CLEARED, **short channel settle** before `trySubmitRollbackProposal`, **or**
- Suppress CLEAR-RELAY while `propose_submitted_ && !order_applied_` for that epoch (risky for stragglers — prefer A first).

### C. TYPE11 (larger change; optional follow-on)

- Revisit suppress-on-overhear / relay only if not hearing ≥k carriers / relay only PRE_PREPARE + own votes, etc.
- Already acknowledged tradeoff in `ResDBTransport.cc` comments.

### D. Do not

- Don’t remove CLEAR evidence from ORDER trailer (safety).
- Don’t make WAIT a certificate.
- Don’t “broadcast DROP” — TYPE11-DROP is already log-only.

---

## 7. Verification plan

1. Re-run crash Scenario 16; around first `ROLLBACK-PROPOSE … rc=0`:
   - `CLEAR-CERT` assemblers after propose should be **≈0** (or 1 race).
   - `CLEAR-RELAY` after propose should collapse.
   - `CLEAR-ECHO` recv lines with `count > threshold` should vanish if hush applied.
2. Confirm stragglers still CLEARED via one cert or ORDER CLEAR trailer (`[INCIDENT-REGISTER] state=CLEARED`).
3. Confirm emergency Scenario 15 still has **0** CLEAR/WAIT and still completes.
4. Watch `simsec/sec` through seq=3 COMMIT; target: not stuck at ~0.002; COMMIT `TYPE8-RECV` appears.

Useful rg:

```bash
rg -n "ROLLBACK-PROPOSE.*rc=0|CLEAR-CERT\]|CLEAR-RELAY|CLEAR-ECHO|WAIT-STOP|PBFT-NEW-REQ|TYPE11-DROP|simsec/sec" /tmp/resdb-simulation.log
```

---

## 8. Key code pointers

| Topic | Location |
|---|---|
| CLEAR echo/cert | `ResDBRollbackProtocol.cc` `sendClearEcho`, `handleClearEcho`, `broadcastClearCert`, `onIncidentCleared` |
| CLEAR dwell lockstep comment | `beginPostCancelDiscovery` ~L1239–1269 |
| WAIT stop | `stopWait`, `waitConditionsHold`, handleWaitHeartbeat |
| TYPE11 flood | `ResDBTransport.cc` `maybeRelayResdbConsensusBytes`, `handleResdbConsensusRelay` |
| Propagation-confirmed patterns | `cancelCertPropagationConfirmed`, `cancelGossipPropagationConfirmed`, `decisionGossipPropagationConfirmed` |
| Propose gate on BLOCKING | `trySubmitRollbackProposal` + `hasBlockingIncidentForEpoch` |
| Emergency skip incident | `registerBlockedIncidentIfCrash` (`CANCEL_CRASH` only) |

---

## 9. One-sentence summary for the agent

**Echoes are mostly one-shot; the firehose is (1) every node assembling/relaying CLEAR after threshold and (2) TYPE11 epidemic relay — CLEAR has no propagation hush and keeps colliding with ORDER(e+1); port the CANCEL_CERT / gossip `*PropagationConfirmed` pattern to CLEAR and suppress post-CLEARED echo handling.**

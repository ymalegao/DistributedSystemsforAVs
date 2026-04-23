# BFT Leader-Change Under a Byzantine Leader on 802.11p V2V

Investigation log for making BFT-SMaRt's leader-change (LC) protocol
converge under a Byzantine leader when the underlying transport is
Veins/OMNeT++ 802.11p broadcast, rather than TCP.

## Problem

With a Byzantine leader injected (e.g. `--byzleader 0 --randomize 16 0 --sync-java`):

- Normal BFT consensus (3 rounds: PROPOSE → WRITE → ACCEPT) completes in ~2 sim-sec.
- Leader change never completes in a reasonable sim-time window (originally >14 sim-sec with
  zero STOPDATA or SYNC messages observed).
- Under wall-time timers the old runs "worked" (observed 24.6 wall-sec total,
  including post-LC consensus), but the timeout had to be hand-tuned per
  replica count (10 s for N=8, 15 s for N=12, 20 s for N=16), which is
  fundamentally incompatible with simulation time.

Goal: make LC complete in a bounded sim-time window using `SimulationClock`
for timeout *comparison* while choosing `Timer.schedule(...)` cadences that
actually converge in simulation time.

## Baseline architecture

Key pieces that interact:

| Component | File | Role |
|---|---|---|
| Request timer | `bftsmart/tom/leaderchange/RequestsTimer.java` | Fires when a client request is unordered longer than `timeout`. Triggers LC. Also owns `SendStopTask` that re-emits STOP for a regency. |
| Timeout stamper | `bftsmart/clientsmanagement/ClientsManager.java` | Stamps `receptionTimestamp` on each TOMMessage. |
| Sim clock | `bftsmart/communication/V2V/SimulationClock.java` | C++ → Java one-way clock (sim-sec × 1000). |
| Reliability layer | `bftsmart/communication/V2V/ReliableV2VMessaging.java` | Per-(sender,target) seq + exponential-backoff retx over the JNI→C++ broadcast. |
| JNI bridge | `bftsmart/communication/V2V/V2VNativeReplicaConnection.java` | `synchronized send()` that serializes an envelope and calls into C++ (`bridge.sendMessage`). |

## Changes applied (current state)

### 1. Sim-time stamping & comparison

- **`ClientsManager.java`**: `receptionTimestamp = SimulationClock.currentTimeMillis();`
- **`RequestsTimer.run_lc_protocol()`**: timeout *comparison* uses `SimulationClock.currentTimeMillis() - request.receptionTimestamp > t`.

### 2. Jitter on every `Timer.schedule(...)` call

In `RequestsTimer.java`:

```java
private static final long JITTER_WALL_MS =
        Long.getLong("bftsmart.lc_jitter_wall_ms", 500L);

private static long jitter() {
    return JITTER_WALL_MS <= 0
        ? 0L
        : ThreadLocalRandom.current().nextLong(JITTER_WALL_MS);
}
```

Applied to `startTimer()`, `run_lc_protocol()` reschedule, and `setSTOP()` —
de-correlates the simultaneous 16-replica STOP broadcast burst on the shared
802.11p channel. Without jitter, 16 replicas fire in lock-step at
`receptionTimestamp + timeout`, creating a catastrophic CSMA-CA collision event.

### 3. STOP re-emission: wall-clock poll + **sim-time throttle**

Originally `setSTOP(...)` rescheduled `SendStopTask` at the full `timeout`
(default 4000 ms wall). That was too slow for quorum growth. Later we moved to
a shorter wall interval — but **`java.util.Timer` is wall-clock**. When the
OMNeT++ run is CPU-bound, sim time can advance **5–20× slower than wall time**
during LC. A 1 s wall STOP cadence then becomes ~10 STOP broadcasts per
**simulated** second per replica, i.e. ~100+ cluster-wide STOP/s at N=12+,
which saturated 802.11p and **starved ordered SYNC retransmissions** (SYNC
arrived ~8 sim-s late; followers had already timed into reg=2 and discarded
stale SYNC).

**Fix:** keep a wall-clock `Timer` only as a *wakeup* to sample sim-time; gate
the actual `communication.send(...)` on elapsed **simulation** time since the
last STOP for that regency (`SimulationClock.currentTimeMillis()`), stored in
`lastStopSimEmitMs`.

```java
// Poll often (wall-ms); emit at most once per STOP_RETX_SIM_MS (sim-ms).
private static final long STOP_RETX_WALL_MS =
        Long.getLong("bftsmart.stop_retx_wall_ms", 200L);
private static final long STOP_RETX_SIM_MS =
        Long.getLong("bftsmart.stop_retx_sim_ms", 1000L);
```

`SendStopTask` is still one-shot per `Timer.schedule`; each `run()` either
emits (if sim-gap ≥ `STOP_RETX_SIM_MS`) or skips, then chains a fresh task via
`rescheduleSTOP(regency, stop)` while preserving `lastStopSimEmitMs`.

### 4. Per-receiver LC tagging for diagnostics

In `MessageHandler.java` LC branch, each received STOP/STOPDATA/SYNC is now
logged as:

```
[LC-RECV me=<myId> type=<T> reg=<R> from=<S> distinct=<k>] ...
```

Where `distinct` is the running count of unique senders this receiver has
seen for (type, regency). This is the critical metric: LC advances when any
receiver's `distinct` crosses `2f+1`.

### 5. Reliability-layer retx cap

In `ReliableV2VMessaging.java`:

```java
private static final long RETX_TIMEOUT_MS = 30;                  // base
private static final long RETX_MAX_BACKOFF_MS =
        Long.getLong("bftsmart.retx_max_backoff_ms", 250L);      // tune as needed
private static final int MAX_RETX_ATTEMPTS =
        Integer.getInteger("bftsmart.max_retx_attempts", 40);    // was 20
```

Original exponential backoff was `30 → 60 → … → 8000 ms` with 20-attempt
cap. For LC broadcasts whose ACKs never piggyback (Byzantine leader is
silent → no reverse traffic), this meant each broadcast slid into 8 sec
hibernation and retx died out within the LC-relevant window. Cap lowered to
keep late retries dense.

### 6. LC transport “Stage 1”: unordered **STOP only** + ordered STOPDATA/SYNC

**Problem:** Putting every `LCMessage` multicast into `unackedMessages` caused an
ACK/retransmit storm during STOP (all replicas emitting STOP with little
piggyback traffic).

**Approach:** In `ReliableV2VMessaging.sendMulticast()`, detect
`LCMessage` **type** via `TOMUtil.STOP` (value `3`). Only STOP is
fire-and-forget at the reliability layer:

- **Do not** register STOP in `unackedMessages` / retx.
- Set `V2VMessageEnvelope.isUnordered = true` for STOP.
- **Do not** consume a real ordered slot from `broadcastSeqNum` for STOP: use
  a **sentinel** broadcast index (`BROADCAST_SEQ_MASK`) so receivers who skip
  ordering for unordered traffic do not leave a “gap” that would later block
  the first ordered post-LC message (WRITE/SYNC) on the ordered path.

**Critical follow-ups (bugs we hit):**

1. **N=8 stall after LC:** If unordered STOP still incremented
   `broadcastSeqNum`, receivers never advanced `expectedBroadcastSeqNums` for
   those slots → first ordered broadcast looked OOO and sat in
   `broadcastReceiveBuffers` forever (0 ACCEPT). **Fix:** sentinel seq + no
   `getAndIncrement()` for unordered STOP.

2. **N=12 no SYNC:** Treating **all** LC types as unordered meant STOPDATA and
   SYNC had **no** reliability-layer retx; one PHY loss stalled LC. BFT only
   sends STOPDATA/SYNC **once** per phase — they must stay on the reliable path.
   **Fix:** scope `unordered` to `lcType == TOMUtil.STOP` only; STOPDATA (`4`)
   and SYNC (`5`) remain tracked and retransmitted.

`V2VMessageEnvelope.java` adds `isUnordered` (default `false` for legacy
deserialization). `handleIncomingMessage()` delivers unordered envelopes
immediately (no `expectedBroadcastSeqNums` / buffer path), since LC is
idempotent at the BFT layer (regency + distinct-sender accounting).

## Changes reverted (did not work)

### A. Aggressive polling in `run_lc_protocol`

Tried `POLL_INTERVAL_WALL_MS = 200` so the run_lc_protocol timer fires every
200 ms. Combined with non-idempotent LC trigger (each fire re-called
`Synchronizer.triggerTimeout`), this produced 2700+ `TRIGGER_LC_LOCALLY`
self-messages per run, constant cancellation/rescheduling of `SendStopTask`,
and caused STOP delivery to *collapse* (~20 deliveries in 14 sim-sec).

Reverted. `run_lc_protocol` reschedule now uses the full `timeout + jitter()`.

### B. Removing timed-out requests from `watched` after LC trigger

Tried `watched.remove(req);` after `triggerTimeout` to make LC triggering
"idempotent per request". Broke vanilla BFT-SMaRt semantics (watched set
should hold until the request is ordered) and contributed to the thrashing
above.

Reverted.

### C. Application-layer STOP burst (3× physical broadcast per emission)

Tried, in `ReliableV2VMessaging.sendMulticast()`, to detect `LCMessage` and
emit `conn.send(envelope)` three times back-to-back with `Thread.sleep(5–20 ms)`
between copies. Hypothesis: triple the first-shot PHY reach.

Result: delivery *dropped* to 7 STOPs in 6 sim-sec (vs 53 expected at
baseline rate). Root cause: `V2VNativeReplicaConnection.send()` is
`synchronized` and the `Thread.sleep` inside `sendMulticast` blocked the
`java.util.Timer` thread that owns `SendStopTask`, preventing the next
re-emission from firing. The burst also produced intra-sender channel
contention that interfered with itself.

Reverted. Single `conn.send(envelope)` per `sendMulticast` call.

## Experimental log (byzleader=0, --sync-java)

### N=16 (quorum = 11)

| # | Config (STOP-retx / V2V-cap / jitter) | sim sec | STOP deliv | max distinct | STOPDATA | Notes |
|---|---|---|---|---|---|---|
| 1 | 4000 ms / 8000 ms / 0 (original) | 14 | ~20 | — | 0 | LC never progressed; lots of "from myself". |
| 2 | 200 ms poll + 150–500 ms retx / 8000 ms V2V cap | 14 | 20–22 | — | 0 | Thrashing; reverted. |
| 3 | 4000 ms / 500 ms / 500 (jitter added) | 8 | 35 | — | 0 | 3× improvement over #1 per sim-sec. |
| 4 | 4000 ms / 500 ms / 500 | 18 | 160 | 7 | 0 | 0 STOPDATA. Sum distinct=87/240 pairs. Best channel utilisation seen. |
| 5 | 4000 ms / 100 ms / 500 | 18 | 160 | 7 | 0 | 100 ms cap alone didn't help vs 500 ms cap at this emission rate. |
| 6 | 1000 ms / 100 ms / 500 + **3× burst** | 6 | 7 | 2 | 0 | Worse than baseline. Burst reverted. |
| 7 | 1000 ms / 100 ms / 500 | 6 | 46 | 3 | 0 | More emissions, more channel contention, per-emit delivery dropped to ~1.6%. |
| 8 | 500 ms / 100 ms / 500 | 6 | 26 | 2 | 0 | Even more aggressive → even worse. Saturation confirmed. |

### N=4 (quorum = 3) — GROUND-TRUTH CORRECTNESS CASE

| # | Config | sim sec | STOP deliv | max distinct | STOPDATA | SYNC | Re-tx STOP | `stop_to_decision(sim)` |
|---|---|---|---|---|---|---|---|---|
| 9 | 1000 ms / 100 ms / 500 | 6.55 | 12 | **3 / 3 at all 4 receivers** | 3 | 3 | **0** | 6.55 s |

Key observations at N=4:

- 100 % pair reachability achieved on first-shot physical broadcast.
- Zero retransmissions needed — piggybacked ACKs (carried on each replica's
  next STOP re-emission within `STOP_RETX_WALL_MS + jitter`) cleared
  `unackedMessages` before `ReliableV2VMessaging` backoff engaged.
- Full LC protocol (STOP → STOPDATA → SYNC) + subsequent consensus round
  decided in 6.55 sim-sec ≈ 3× normal consensus (~2 s).
- **Proves protocol correctness** for every component we've touched:
  `SimulationClock`-based timeout comparison, `STOP_RETX_WALL_MS` cadence,
  jitter, new `LC-RECV distinct` tracking, reliability-layer retx cap.

### N=16 — after transport + STOP sim-throttle (current stack)

Observed (Byzantine leader, `--sync-java`, aggregated log):

- Example line: `[PHASE_SUMMARY 4] ... stop_to_decision(sim)=15.4000s` — LC +
  post-LC decision completes in bounded sim-time at N=16 (order of ~15 sim-s
  in this run vs previously “never” or multi-regency thrash).

Contributors to the win: STOP-only unordered path (no ACK storm on STOP),
STOPDATA/SYNC on reliable retx, sentinel broadcast seq for unordered STOP,
sim-time–gated STOP re-emission (bounded channel load vs wall-clock flood).

### Scaling interpretation

The N=4 vs N=16 gap is a wireless-scaling effect, not a protocol flaw.
Required per-pair reachability barely changes with N (quorum/N ≈ 75 % at
N=4, 69 % at N=16) but channel load grows ~quadratically. The
piggyback-ACK mechanism compounds: P(ACK) ≈ p₁·p₂ where p₁ is the STOP
delivery probability and p₂ is the subsequent broadcast's delivery
probability carrying the ACK back. Both drop together as N grows,
triggering retx storms that further depress p.

### Key finding: channel saturation is the ceiling

Per-sim-sec channel load estimate:

```
N_replicas × emissions_per_sec × peers × retx_per_emit × packet_size ≈ 802.11p_budget
```

At `1000 ms emit / 100 ms cap`: `16 × 1 × 15 × 40 ≈ 9600` pair-transmissions/sec at ~200 B =
~25 % of a 6 Mb/s channel used by LC traffic alone. Enough contention that
per-emission delivery *drops* faster than the extra emissions compensate.

Going from 1000 ms → 500 ms emission actually *reduced* total STOP deliveries
(46 → 26) and per-receiver distinct (3 → 2).

### Vanilla (#4) is the best observed steady state

- 160 STOP deliveries / 18 sim-sec = 8.9/sim-sec cluster-wide.
- 27 of 240 possible `(sender, receiver)` pairs ever connected at `cap=500`;
  87/240 at `cap=100`.
- Per-receiver distinct saturates around 7 by 18 sim-sec. Projecting to
  distinct=11 requires roughly 30–45 sim-sec at this rate.

## Why normal consensus succeeds and LC doesn't

Both are all-to-all broadcasts over the same channel. Differences:

1. **Temporal concentration.** Consensus ACCEPT fires in a tight burst
   (~tens of ms after a quorum of WRITE is received). LC STOP fires over
   multiple wall-seconds because each replica's timer has its own phase.
2. **Reverse traffic for ACKs.** Normal consensus sees frequent outgoing
   traffic per replica, so `ReliableV2VMessaging` piggyback-ACKs clear
   `unackedMessages` fast, and retx schedules don't climb the exponential
   backoff. During LC, the Byzantine leader is silent, followers only emit
   STOPs, and piggybacked ACKs travel on a thin channel (~1 STOP/sec/replica).
3. **Quorum semantics.** Consensus is "leader-to-all" for PROPOSE and
   "all-to-leader" for subsequent rounds — effectively a star topology. LC
   Phase 1 is literally "all-to-all accumulate 2f+1 distinct senders at
   *some* receiver", which needs far more per-pair reliability.

## Decision / status

**Implemented (Java):** sim-time request comparison + jitter; reliability retx
cap; `[LC-RECV]` diagnostics; **STOP-only** unordered multicast with sentinel
seq; **sim-time–throttled** STOP re-emission (`STOP_RETX_SIM_MS`); STOPDATA/SYNC
remain fully reliable.

**Still valuable (C++ / cross-layer):** one PHY frame per logical send, MAC
backoff tuning, optional STOP payload compaction / FEC-style bundling (see
optimizations below).

## Files modified

- `bftsmart/library/src/main/java/bftsmart/clientsmanagement/ClientsManager.java` — sim-time stamp.
- `bftsmart/library/src/main/java/bftsmart/tom/leaderchange/RequestsTimer.java` — sim-time compare, `JITTER_WALL_MS`, `STOP_RETX_WALL_MS` (poll), `STOP_RETX_SIM_MS` (emit gate), `lastStopSimEmitMs`, jitter on schedules, chained `SendStopTask`.
- `bftsmart/library/src/main/java/bftsmart/communication/MessageHandler.java` — per-receiver `[LC-RECV]` tag + `distinct` counter.
- `bftsmart/library/src/main/java/bftsmart/communication/V2V/ReliableV2VMessaging.java` — STOP-only unordered path; sentinel broadcast seq; `handleIncomingMessage` unordered fast path; retx cap / max attempts knobs.
- `bftsmart/library/src/main/java/bftsmart/communication/V2V/V2VMessageEnvelope.java` — `isUnordered` flag.

## Tuning knobs (all JVM system properties)

| Property | Default | Purpose |
|---|---|---|
| `bftsmart.lc_jitter_wall_ms` | 500 | Jitter ceiling on every LC-related `Timer.schedule`. |
| `bftsmart.stop_retx_wall_ms` | 200 | Wall-ms between **wakeups** that check whether a STOP emit is due (poll sim-time). |
| `bftsmart.stop_retx_sim_ms` | 1000 | Min **sim-ms** between actual STOP broadcasts from the same replica for the same regency. |
| `bftsmart.retx_max_backoff_ms` | 250 | Cap on `ReliableV2VMessaging` exponential backoff (tune with load). |
| `bftsmart.max_retx_attempts` | 40 | Drop-ceiling on retx for any one envelope. |

## Diagnostic recipes

```bash
# Per-receiver max distinct STOP senders (THE quorum metric)
grep -oE "LC-RECV me=[0-9]+ type=STOP reg=[0-9]+ from=[0-9]+ distinct=[0-9]+" /tmp/bft-all-replicas.log \
  | awk -F'[= ]' '{print $3, $7, $11}' \
  | awk '{key=$1"/"$2; if ($3>max[key]) max[key]=$3} END {for (k in max) print k, max[k]}' \
  | sort -t/ -k1,1n

# Phase progression
grep -c "of type STOP for regency" /tmp/bft-all-replicas.log
grep -c "of type STOPDATA"         /tmp/bft-all-replicas.log
grep -c "of type SYNC"             /tmp/bft-all-replicas.log

# Reliability-layer health
grep -c "Re-transmitting STOP"         /tmp/bft-all-replicas.log
grep -c "Max retransmission attempts"  /tmp/bft-all-replicas.log   # non-zero = dropping legitimate LC msgs
```

## Stage 2 attempt: STOP_NACK transport (and the bug we hit)

### Motivation

After we mined phase timing from the working run, Phase 1 consumed ~90% of
`stop_to_decision(sim)` (e.g. 104.7 wall-s of STOP flooding vs 11.5 s of
STOPDATA + 0.13 s of SYNC). The "noise floor" of blind STOP emission was the
dominant cost. Goal: replace the blind flood with a compact bitmask of
missing senders so offered channel load scales O(N) instead of O(N²).

### What we implemented

- `TOMUtil.STOP_NACK = 10` — new LC message type, transport-only.
- `LCMessage.missingNodesMask : int` — bitmask on the wire (bit i set →
  "sender hasn't heard a STOP from acceptor i for this regency").
- `LCManager.getStopSenders(regency)` — read-only accessor.
- `ReliableV2VMessaging.sendMulticast` — `STOP_NACK` now shares the same
  unordered / sentinel-seq path as `STOP` (no retx, no ACK storm).
- `RequestsTimer.SendStopTask` — after `STOP_BLIND_EMITS` (default 2) full
  STOPs, switch to emitting compact `STOP_NACK` at the existing
  `STOP_RETX_SIM_MS` cadence.
- `RequestsTimer.handleStopNack(reg, fromPid, mask)` — if our bit is set,
  unicast a fresh copy of our STOP to the NACKer after a wall-ms jitter,
  capped at `NACK_REPLIES_PER_PEER` (default 3) to bound Byzantine-NACK DoS.
- `MessageHandler` — intercepts `STOP_NACK` before it can reach
  `Synchronizer.deliverTimeoutRequest` (never enters `LCManager.addStop`,
  so 2f+1 quorum stays cryptographic-STOP-backed).

### Observed failure at N=16

Stop-sign timed out at **25.6 sim-s** (never reached). Log analysis:

| Metric | Value |
|---|---|
| Blind STOP emissions | 31 (≈ 2 per replica, as designed) |
| `Emitting STOP_NACK` lines | **239** |
| `Responding to STOP_NACK` lines | 136 |
| `Received regency change request` (addStop path) | 266 |
| STOPDATA deliveries | 8 (needed 11) |
| SYNC deliveries | **0** |
| Per-receiver max STOP distinct (LC-RECV) | 9–13 (fine) |
| NACK `missing_count` distribution | **all** at 14 or 15 |
| NACK `have=` diagnostic | **all** `have=0/10` |

### Root cause

`Synchronizer.startSynchronization` wipes `LCManager.stops[reg]` the moment
it crosses 2f+1, for memory-leak reasons (line ~540:
`lcManager.removeStops(nextReg)` immediately after
`setLastReg(nextReg)`). The `SendStopTask` timer keeps running until
`removeSTOPretransmissions` is triggered from `processSYNC` (line ~1190) —
which never fires in our failing run because no SYNC is reached.

Our `buildMissingMask` was reading `LCManager.getStopSenders(reg)`, so after
Phase 1 transitioned locally the set was **empty**, producing a persistent
full-peer NACK (`missing_count = N-1`). Every replica hit this state at
roughly the same sim-time. Each NACK then triggered up to ~10 peer STOP
resends. Instead of reducing channel load, NACK mode **amplified** it — the
exact opposite of the design goal. STOPDATA (large, single-shot, on the
reliable ordered path) couldn't fit through → 8/11 STOPDATA → no SYNC → 25 s
stop-sign fires.

The symptom (`have=0/10` on every NACK emission) is the fingerprint of
`removeStops(reg)` having already been called locally.

### Fix applied

- **`SendStopTask.run()`** short-circuits when
  `lcManager.getLastReg() >= stop.getReg()`: once Phase 1 has installed
  locally, we stop emitting anything (plain STOP or NACK) but keep the
  chained `rescheduleSTOP` alive so the external cancel path is unchanged.
- **`RequestsTimer.heardByRegency`** — transport-layer shadow of
  "peers whose STOP we have seen", maintained in `MessageHandler` via
  `recordHeardStop(reg, pid)` on every delivered STOP. Used exclusively by
  `buildMissingMask`, independent of `LCManager.stops`. This keeps the NACK
  mask accurate before, during, and after the LCManager wipe. The BFT
  quorum is still driven entirely by `LCManager.addStop` on real,
  authenticated STOPs.
- `dropRegencyState(reg)` (called from
  `Synchronizer.removeSTOPretransmissions` on install) also clears
  `heardByRegency[reg]`.

### Lessons / design notes

- Transport-layer retry loops that consult BFT-layer state (`LCManager`)
  are fragile: the BFT layer owns that state and is free to mutate it on
  phase transitions. Keep transport bookkeeping in the transport layer.
- Self-repairing protocols benefit from an explicit "we are done
  transmitting" signal; relying on the external cancel path creates
  windows where stale senders can saturate the channel.
- Any future extension (backoff, aggregation) must respect the
  "`lastReg >= reg` ⇒ silent" invariant or it will regress.

## Stage 2, round 2: NACK never activates + premature reg-2 escalation

### Observed failure (second N=16 run)

Stop-sign timed out again. Critical log signatures:

| Metric | Value |
|---|---|
| "Re-transmitting STOP message … blind X/2" observations | **208** |
| Distinct values of `X` across those 208 lines | **only `1/2`** |
| `Emitting STOP_NACK` lines cluster-wide | **0** |
| Cluster-wide `Sending STOP message to install regency 1` | 16 |
| Cluster-wide `Sending STOP message to install regency 2` | 8 |
| Replica 1 (new leader) `distinct=` max on STOP reg=1 | **8** (needed 11) |
| Replica 1 STOP reg=1 senders received | `{0,2,3,10,12,13,14,15}` (8 pids) |
| Replica 1 STOP reg=**2** senders received | `{4,5,6,7,8,9,11}` (7 pids) |
| Replica 1 STOPDATA reg=1 received | 8 (all buffered as **out of context**) |
| SYNC messages cluster-wide | 0 |

### Root cause — two coupled bugs

**Bug 1 — `blindEmitCount` counter never advances:** in
`SendStopTask.run()`, the per-regency `blindEmitCount` was only
incremented inside the NACK branch:

```java
if (emits < STOP_BLIND_EMITS) {
    // send full STOP — NO increment!
} else {
    // send NACK
    blindEmitCount.put(reg, emits + 1);   // only here
}
```

Every wake therefore read `emits == 0`, printed `blind 1/2`, broadcast a
full STOP, and left the counter untouched. The condition
`emits < STOP_BLIND_EMITS` was permanently true → **NACK mode unreachable**.
The entire Stage-2 NACK design shipped dark.

**Bug 2 — rapid `run_lc_protocol` re-entry escalates to regency 2 before
SYNC has a chance:** under 802.11p load the sim:wall ratio in Cmdenv runs
around 1:5–1:10. The wall-clock `RequestTimerTask` therefore fires
several times between sim-time threshold crossings. The `pendingRequests`
loop removes a request from the local list only on the first fire (once
`request.timeout == true`), but `triggerTimeout` is still called on every
subsequent fire as long as the request is in `watched` and its sim-time
elapsed exceeds `t`. That's fine at stable state, **but the moment
Phase 2 for regency 1 installs locally (`setLastReg(1)`), the
`nextReg == lastReg` gate inside `triggerTimeout` flips back on and
advances `nextReg` to 2** — i.e. the replica immediately joins regency 2
even though SYNC for regency 1 is still in flight. In the failing run,
8 of the 16 replicas made this jump, splitting the cluster's STOP
senders across two regencies. Replica 1 (the reg-1 new leader) ended up
with STOPs from only 8/15 peers for its own regency, permanently stuck
below 2f+1 = 11. Every STOPDATA it received from the faster peers was
correctly filed as "out of context" by `Synchronizer.deliverTimeoutRequest`
(line 871) because its own `lastReg` was still 0, and
`processOutOfContextSTOPDATA` never ran because `startSynchronization`'s
Phase-2 drain (line ~796) was never reached.

Bug 1 made Bug 2 fatal: with NACKs dark, replica 1 had no way to
solicit the missing 7 STOPs it needed.

### Fix applied

1. **`SendStopTask.run()` — always advance `blindEmitCount`.** Move the
   `blindEmitCount.put(reg, emits + 1)` out of the NACK-only branch so it
   runs on every wake that actually emits (blind STOP *or* NACK). After
   `STOP_BLIND_EMITS` full emissions, the task transitions to NACK mode
   as designed.

2. **`RequestsTimer.run_lc_protocol()` — debounce on `lastReg`.** Add a
   per-instance `lastTriggeredForLastReg` (init `-1`). Before calling
   `triggerTimeout`, check `currentLastReg > lastTriggeredForLastReg`; if
   not, skip and just reschedule `RequestTimerTask`. This collapses the
   wall-clock re-entry storm to one trigger per distinct `lastReg`
   value, so a replica can no longer jump into regency 2 the instant its
   own Phase 2 for regency 1 installs. Legitimate escalation is still
   permitted one-per-epoch: once Phase 2 installs and `lastReg` actually
   advances to 1, a *subsequent* sim-time-exceeded wake is allowed to
   run `triggerTimeout` once more (BFT liveness fallback).

3. **`dropRegencyState(reg)` — reset the debounce on install.** Set
   `lastTriggeredForLastReg = -1` alongside the other per-regency state
   cleanup. Future LC episodes (from a later burst of client timeouts)
   can fire again.

`handleStopNack` is intentionally *not* gated by `phase1DoneLocally`: a
replica that has already advanced past Phase 2 but has not yet been
cancelled by SYNC still holds its STOP in `currentStopByRegency`, and
must remain willing to answer NACKs from slow peers (subject to the
existing `NACK_REPLIES_PER_PEER` DoS cap).

### Why this lines up with the evidence

- With Bug 1 fixed, after 2 blind full STOPs the replicas that haven't
  yet seen 2f+1 start broadcasting compact STOP_NACKs. Replica 1's NACK
  asks for STOPs from the 7 pids it hasn't heard (`mask` with 7 bits
  set), and honest peers (including the 8 that already installed
  Phase 2 locally) unicast their original STOPs back with ≤20 ms wall
  jitter — no new BFT work, just transport resends.
- With Bug 2 fixed, none of those 8 early-finishers re-enter
  `triggerTimeout` while reg=1 is still the active LC. Their
  `SendStopTask` stays silent (`phase1DoneLocally` short-circuit) and
  they do not flood the channel with STOPs for regency 2. The STOP
  senders no longer split between reg=1 and reg=2.
- Net effect: the 2f+1 boundary for reg=1 is reached at replica 1, it
  transitions to Phase 2, drains the buffered STOPDATAs via
  `getOutOfContextLC(STOPDATA, 1)` at line 796, and issues SYNC.

## Stage 2, round 3: debounce bypassed via `Acceptor` + NACK disabled

### Observed failure (third N=16 run, 22:47:00 log)

Same end-state (LC never completes), different failure mode than round 2:

| Metric | Value |
|---|---|
| `Sending STOP message to install regency 1` | 14 (replicas 0,1,2,4,5,6,7,8,9,10,11,12,13,14 — missing 3, 15) |
| `Sending STOP message to install regency 2` | 9 |
| `Emitting STOP_NACK` cluster-wide | **0** (NACK dark again) |
| `blind X/Y` observations | `X` advances monotonically to **51**; `Y` = `2147483647` |
| Replica 1 STOP reg=1 max `distinct` | **9** (needed 11) |
| Replica 1 STOP reg=1 senders seen | `{0,2,3,5,6,7,11,12,13}` (9 pids) |
| Replica 1 STOP reg=2 messages received | 21 (noise, not useful) |
| `Sending STOPDATA of regency 1` | 9 (from reg=1 early-finishers) |
| SYNC messages cluster-wide | 0 |

### Root cause — two more coupled bugs

**Bug 3 — NACK disabled at config.** Round-2 flags were rolled forward with
`STOP_BLIND_EMITS = Integer.MAX_VALUE`, which makes the blind-to-NACK
transition unreachable by design (not a bug in the state machine; a bug
in the knob). So `blindEmitCount` advances to 51 but `emits < STOP_BLIND_EMITS`
never flips. Replica 1 has **no recovery mechanism** for the 6 STOPs it
never heard (likely radio-level loss from channel saturation; at 200 ms
sim-cadence × 14 emitters we observed ≈64% delivery rate to replica 1).

**Bug 4 — escalation debounce only covered the `run_lc_protocol` path.**
Round-2's fix added `lastTriggeredForLastReg` → `lastTriggeredForNextReg`
inside `RequestsTimer.run_lc_protocol`. That correctly suppresses the
wall-clock re-entry from the client-request timer. But the Acceptor
has a **second, independent entry into `Synchronizer.triggerTimeout`**:

```284:299:bftsmart/library/src/main/java/bftsmart/consensus/roles/Acceptor.java
			} else if (epoch.deserializedPropValue == null
					&& !tomLayer.isChangingLeader()) { // force a leader change
				tomLayer.getSynchronizer().triggerTimeout(new LinkedList<>());
			}
```

`isChangingLeader()` returns `!requestsTimer.isEnabled()`, and
`Synchronizer.startSynchronization` **flips `requestsTimer.Enabled(true)`
at line 544 the moment Phase 2 for the current regency installs
locally**, i.e. well before SYNC propagates and `dropRegencyState` fires.
With the Byzantine leader still silent, the same replica sees another
null-propose on the next consensus epoch, `isChangingLeader()` is now
false, and Acceptor calls `Synchronizer.triggerTimeout(emptyList)`
directly — completely bypassing the RequestsTimer debounce.
`triggerTimeout`'s own `nextReg == lastReg` gate is now satisfied (both
equal to the just-installed regency), so `setNextReg(lastReg + 1)`
advances to reg=2 and broadcasts a STOP.

Replica 9 in the log is the smoking gun: it hits `distinct=11` for
reg=1 at `22:47:22.289`, sends STOPDATA for reg=1 at `22:47:22.293`,
and within a few sim-seconds is emitting a STOP for reg=2 via exactly
this path. Eight other replicas cascade via
`startSynchronization(2)` once they have f+1 STOPs for reg=2 from
their peers (which also advances `nextReg` at line 494 without
claiming the epoch).

### Fix applied

1. **Re-enable NACK mode.** `STOP_BLIND_EMITS` default back to `3`.
   Override via `-Dbftsmart.stop_blind_emits=<N>` for experiments. At
   the current 200 ms sim-cadence, round-3 log shows three blind full
   STOPs ≈ 600 ms of priming before NACKs kick in — tight enough to
   seed the network, short enough to leave 5-plus sim-s of recovery
   window before the 15 sim-s liveness escape (see below) fires.

2. **Single-choke-point escalation debounce in `Synchronizer`.**
   Move the debounce out of `RequestsTimer.run_lc_protocol` and into
   the two places where `nextReg` actually advances:

   - `Synchronizer.triggerTimeout(...)`: at the top of the
     `nextReg == lastReg` block, call `requestsTimer.tryClaimLCEpoch()`.
     On failure, run `processOutOfContextSTOPs` + `startSynchronization`
     idempotently and return. This covers the Acceptor entry path as
     well as any other future caller.
   - `Synchronizer.startSynchronization(...)`: at the top of the
     Phase 1→2 block (`condition && nextReg == lastReg`), call the
     same `tryClaimLCEpoch()`. This covers the "f+1 STOPs arrived
     from peers, I hadn't triggered locally yet" case so that it also
     claims the epoch for later Acceptor fires on the same replica.

3. **`tryClaimLCEpoch` with sim-time liveness escape.** Implemented in
   `RequestsTimer` as:

   ```java
   private volatile boolean lcEpochInFlight = false;
   private volatile long    lcEpochStartedSimMs = 0L;
   private static final long LC_ESCALATION_GAP_SIM_MS =
       Long.getLong("bftsmart.lc_escalation_gap_sim_ms", 15000L);

   public synchronized boolean tryClaimLCEpoch() {
       long now = SimulationClock.currentTimeMillis();
       if (lcEpochInFlight && (now - lcEpochStartedSimMs) < LC_ESCALATION_GAP_SIM_MS) {
           return false;
       }
       lcEpochInFlight = true;
       lcEpochStartedSimMs = now;
       return true;
   }
   ```

   `lcEpochInFlight` is reset in `dropRegencyState`, which fires from
   `Synchronizer.removeSTOPretransmissions` on SYNC install. The
   sim-time gate ensures BFT liveness: if reg=r genuinely stalls (new
   leader is also faulty, for example), after `LC_ESCALATION_GAP_SIM_MS`
   the next call is allowed through and escalates to reg=r+1.

4. **Remove the now-redundant `run_lc_protocol` debounce.** With the
   central guard in place, keeping a per-nextReg filter upstream would
   actually *break* the liveness escape — `nextReg` stops advancing
   precisely when reg=r is stuck, so the upstream filter would keep
   suppressing even after the sim-time gate opens. `triggerTimeout` is
   cheap when the escalation is suppressed (idempotent
   `startSynchronization`), so calling it on every matured wake is
   fine.

### Why this lines up with the evidence

- Without the Acceptor-path bypass, once replica 9 crosses Phase 2 for
  reg=1 its null-propose-driven `triggerTimeout` call hits
  `tryClaimLCEpoch()` → `false` → returns without touching `nextReg`.
  No reg=2 STOPs leak out of early-finishers.
- Without reg=2 STOPs in the air, the `startSynchronization(2)` cascade
  path never arms (no f+1 STOPs for reg=2 ever accumulate), so the
  remaining cluster stays on reg=1.
- With NACK re-enabled, replica 1 switches from blind broadcast to
  compact-mask NACKs after 3 priming STOPs. The 5–6 peers it's missing
  unicast their cached `currentStopByRegency[1]` back (NACK-reply is
  not gated by `phase1DoneLocally`, by design from round 2). Replica 1
  crosses `distinct=11` → Phase 2 → STOPDATA drain → SYNC → decision.

### Jitter question

User asked whether lowering `JITTER_WALL_MS` (default 500 ms wall)
would help. **No.** Jitter spreads CSMA-CA contention across replicas;
reducing it increases collision probability on the shared 802.11p
channel and would drop delivery further. The round-3 delivery rate
(9/15 ≈ 64% for replica 1's reg=1 STOPs) is a symptom of channel
saturation, not of jitter misconfiguration. Fix is NACK-driven
targeted resends, not denser broadcast.

## Stage 2, round 4: NACK replies unicast to NACKer only → delivery failure

### Observed failure (fourth N=16 run, 23:47:00 log, after Claude-Code STOPDATA retx + nextReg debounce)

| Metric | Value |
|---|---|
| Reg=2 escalation | **None** — `tryClaimLCEpoch` suppressed all 5 escalation attempts ✓ |
| STOP_NACK emissions | **154** |
| NACK responses | **19** |
| Replicas that crossed distinct=11 (Phase 2) | **5** (`{0,2,3,6,8}`) |
| Replicas stuck at distinct=9 | **10** (`{4,5,7,9,10,11,12,13,14,15}`) |
| Replica 1 (leader) STOPDATAs received | **18 messages** but from only **5 unique senders** |
| Replica 1 `processSTOPDATA` progress | `lastCIDsSize` max = **5**, `collectsSize` = **18+** |
| `byzantineQuorum` threshold for SYNC | `(16+5)/2 = 10` → needs `lastCIDsSize > 10` |
| SYNC sent | **0** |

### Root cause — NACK replies unicasted to single NACKer

The NACK mechanism design unicasts the STOP reply to only the NACKer
(`send(new int[]{fromPid}, stopToSend)`). Under 802.11p this is still
physically a broadcast frame, but addressed logically to one peer, and
sent fire-and-forget (unordered/no retransmit in `ReliableV2VMessaging`).

At this point in the run:
- Replicas `{0,2,3}` crossed distinct=11 and have `phase1DoneLocally=true`
  → their `SendStopTask` no longer broadcasts STOPs periodically.
- 10 stuck replicas (distinct=9) emit NACKs every 200 ms asking for pids
  `{0,1,2,3}` whose STOPs never reached them.
- Each NACK triggers at most 3 unicast STOP replies (old `NACK_REPLIES_PER_PEER=3`).
- Those 19 unicast frames (fire-and-forget over a saturated channel) are
  nearly all lost: **zero new STOP arrivals at replica 4 after 23:52:07**
  despite 6 NACK responses being sent to it from 23:52:31 onwards.

With only 5 unique STOPDATA senders and `lastCIDsSize` capped at 5,
the SYNC condition `lastCIDsSize > byzantineQuorum=10` is permanently
unsatisfiable. The leader collects 18+ duplicate STOPDATAs but the
`processSTOPDATA` condition (`lastCIDsSize > 10 && collectsSize > 10`)
never fires because `lastCIDsSize` is deduplicated per sender.

### Why unicast is wrong

When 10 replicas are simultaneously at distinct=9 and missing the same
pid (e.g. pid 0), a unicast reply from replica 0 to NACKer X helps only
X. The other 9 stuck replicas learn nothing. In contrast, a broadcast
reply from replica 0 delivers to all 10 at once: one channel access
event advances all 10 from distinct=9 → 10, vs 10 separate events that
each compete on the saturated channel.

### Fix applied

1. **`handleStopNack` → broadcast reply.** Change the reply destination
   from `new int[]{fromPid}` to `controller.getCurrentViewOtherAcceptors()`.
   Each NACK reply is now a full broadcast, visible to all stuck replicas
   simultaneously. This mirrors the original blind-broadcast behaviour but
   demand-driven: only fires in response to an actual NACK.

2. **Raise `NACK_REPLIES_PER_PEER` from 3 → 10.** With broadcast replies
   being more channel-efficient (1 frame serves all stuck peers), we can
   afford more retry attempts per NACKer under channel loss conditions
   without flooding. The DoS bound remains: a Byzantine NACKer can induce
   at most 10 broadcast STOP replies per regency from each honest replica,
   capped by this value.

### What to expect

After the fix, the first NACK from any stuck replica causes the missing
early-finishers to broadcast their STOP to all. All 10 stuck replicas
receive it in the same MAC slot, advance from 9 → 10. The next NACK
(with updated mask, minus the now-heard pid) triggers a reply from the
next missing pid → all stuck replicas go 10 → 11. Phase 1 completes
cluster-wide. All 15 non-leader replicas install Phase 2 and unicast
STOPDATA to replica 1. With 11+ unique STOPDATA senders, `lastCIDsSize`
crosses `byzantineQuorum=10` and replica 1 calls `catch_up` → SYNC.

## Stage 2, round 5: STOPDATA seq-gap blocks Phase 2 + timer never cancels

### Observed failure (fifth N=16 run, 00:10:21 log)

| Metric | Value |
|---|---|
| Reg=2 escalation | **None** — epoch debounce holding ✓ |
| Replicas crossing distinct=11 (Phase 2 triggers) | **11** ({0,2,3,4,5,6,7,13,14,15} + leader 1 = 11 senders) |
| Replicas stuck at distinct=10 | **5** ({8,9,10,11,12}) — never install Phase 2 |
| `Sending STOPDATA of regency 1` | 15 |
| Unique STOPDATA senders at leader | **7** ({0,2,6,7,13,14,15}) |
| Missing senders: | {3,4,5} — crossed 11 but STOPDATAs never reached leader |
| `lastCIDsSize` max at leader | **8** (7 from others + 1 from leader itself) |
| `collectsSize` max | **20+** (inflating due to retransmissions; `SignedObject` identity equality) |
| `byzantineQuorum = (N+f)/2 = 10` | Needs `lastCIDsSize > 10` = 11 |
| SYNC sent | **0** |

### Root cause 1 — STOPDATA seq-gap blocks reliable delivery

STOPDATA was on the ordered/reliable path (`unordered = false` in
`ReliableV2VMessaging`). The application-level STOPDATA retransmission
timer (added by Claude) calls `communication.send(retxDest, stopdataMsg)`
on every 200ms-wall-tick. Each call consumes a new `broadcastSeqNum` slot
(because STOPDATA is an ordered send), creating:

```
Original:   seq = BROADCAST_FLAG | M       → registered in unackedMessages[leader]
Claude retx1: seq = BROADCAST_FLAG | (M+1) → new entry in unackedMessages[leader]
Claude retx2: seq = BROADCAST_FLAG | (M+2) → ...
```

The leader's `broadcastReceiveBuffers[sender]` expects seq=M first. When the
original seq=M frame is lost (channel saturation), the timer retransmissions
arrive at seq=M+1, M+2, M+3 and are buffered in order-waiting. The
reliability layer also retransmits seq=M, but if the channel is congested
all retransmissions also lose. The receiver is stuck forever waiting for
seq=M to fill the gap before it will deliver M+1, M+2.

Evidence: replicas 3, 4, 5 logged "Sending STOPDATA of regency 1" but
their STOPDATAs **never arrived at any receiver** — even with 45 total
application-level retransmissions. Zero delivery despite many attempts.

### Root cause 2 — STOPDATA retransmission timer never cancels

The original timer cancel condition was `lcManager.getLastReg() > retxRegency=1`.
After a successful reg=1 install, `lastReg` typically remains at `1` indefinitely
(it only becomes `>1` on a subsequent LC escalation), so the timer kept firing.

Even after adding a "cancel on quorum reach" condition, non-leader replicas still
did **not** cancel: they never observe the leader's STOPDATA quorum locally, so
`lcManager.getLastCIDsSize(1)` at a non-leader stays small. Result: all non-leaders
continued to unicast STOPDATA to the leader long after reg=1 had installed, and
the leader kept re-entering `processSTOPDATA` under constant load.

### Fix applied

1. **STOPDATA → unordered in `ReliableV2VMessaging`.** Added
   `TOMUtil.STOPDATA` to the unordered predicate alongside `STOP` and
   `STOP_NACK`. Now each retransmission is a standalone fire-and-forget
   frame — no ordered seq-gap stall on receive (this applies to both the
   physical-broadcast multicast path and the single-target `sendReliable(...)`
   path used by STOPDATA-to-leader). Multiple retransmissions arrive and are
   processed independently; `addLastCID` deduplicates by sender via
   `CertifiedDecision.equals()` so `lastCIDsSize` counts only unique
   contributors regardless of retransmission count. SYNC stays single-shot.

2. **Stop STOPDATA retx when the regency is installed locally.**
   The robust cancel point is when this replica installs regency `r` via SYNC
   (i.e., inside `finalise(...)`), not when it *infers* quorum locally.
   Implementation:
   - Track STOPDATA retx `Timer`s in `Synchronizer` and cancel them in
     `removeSTOPretransmissions(r)` for all `<= r`.
   - Keep the leader's `syncSentRegencies` guard for the installed regency
     (`removeIf(r < installed)` instead of `<=`) so duplicate STOPDATAs can't
     re-trigger `catch_up()` and re-broadcast SYNC.

### What to expect

With STOPDATA unordered, each retransmission attempt is an independent
delivery event. At ~200ms sim cadence, replicas 3/4/5 get dozens of
independent attempts instead of one attempt that creates a blocking seq
gap. Expected: 10 non-leader STOPDATA senders + leader's own = 11 unique
`lastCIDsSize` entries → `catch_up` → SYNC → consensus decision.

## Previously explored (kept for reference)

1. **C++ / Veins: verify one logical send → one PHY broadcast**  
   Confirm `bridge.sendMessage(-1, …)` maps to a single 802.11p transmission (no
   accidental batching or starvation of the consensus queue behind STOP-sized
   payloads).

2. **Payload size on the ordered path**  
   STOPDATA/SYNC can be large; compression or a slimmer LC wire format reduces
   airtime and collision probability (especially for the single-shot SYNC).

3. **Adaptive `STOP_RETX_SIM_MS`**  
   Start slightly faster when `distinct` is low, back off as quorum approaches,
   or tie to `system.totalordermulticast.timeout` so STOP rate scales with N
   without manual tuning.

4. **Byzantine-leader path: skip incrementing send seq when PROPOSE is dropped**  
   If the leader drops PROPOSE without transmitting, `broadcastSeqNum` can still
   advance → receivers buffer later ordered messages behind a gap. Guard the
   silent-leader branch so sequence state stays aligned with what actually left
   the radio.

5. **Stage 2: NACK-based LC (optional)**  
   If STOP must stay dense for very large N, receivers could broadcast a compact
   bitmask of missing STOP senders so replies are targeted instead of purely
   timer-driven floods (paper-friendly if evaluated against baseline).

6. **FEC-style STOP (research)**  
   Piggyback hashes or short digests of recent peer STOPs so one successful
   reception advances “heard from” counts — must remain compatible with BFT-SMaRt
   LC message validation (not trivial).

## Open questions for C++-side investigation

1. Does `V2VProxyModule.cc` turn every `bridge.sendMessage(-1, data)` into
   exactly one PHY broadcast frame, or is there queueing / rate-limiting
   that collapses bursts?
2. During LC, are there hidden ordering/buffering constraints in the
   `V2VArrivalProtocol` or the shared ingress ring that could be
   dropping LC envelopes preferentially?
3. Under load, what is the measured sim:wall ratio and queue depth at the JNI
   boundary? That drives how aggressive `STOP_RETX_WALL_MS` can be without CPU
   thrash.

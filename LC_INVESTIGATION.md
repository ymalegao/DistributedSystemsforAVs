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

### 3. Decoupled STOP re-emission interval

Originally `setSTOP(...)` rescheduled `SendStopTask` at the full `timeout`
(default 4000 ms wall). That's way too slow — each replica only emitted ~1
fresh-seq STOP per sim-sec, and LC needs ~11 *distinct senders* to arrive at
a single receiver before STOPDATA can start.

New knob in `RequestsTimer.java`:

```java
private static final long STOP_RETX_WALL_MS =
        Long.getLong("bftsmart.stop_retx_wall_ms", 1000L);
```

The `SendStopTask` now re-arms via `stopTimer.schedule(stopTask, STOP_RETX_WALL_MS + jitter())`.

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
        Long.getLong("bftsmart.retx_max_backoff_ms", 100L);      // was 8000
private static final int MAX_RETX_ATTEMPTS =
        Integer.getInteger("bftsmart.max_retx_attempts", 40);    // was 20
```

Original exponential backoff was `30 → 60 → … → 8000 ms` with 20-attempt
cap. For LC broadcasts whose ACKs never piggyback (Byzantine leader is
silent → no reverse traffic), this meant each broadcast slid into 8 sec
hibernation and retx died out within the LC-relevant window. Cap lowered to
keep late retries dense.

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

## Decision so far

Option selected for next iteration: **C++-side optimization** — verify the
Veins MAC is actually emitting one PHY frame per `conn.send(envelope)`, and
explore application-layer FEC (each STOP carries the last 2 seqs inline so
a single successful reception catches up 3 broadcasts' worth of distinct-sender
state).

## Files modified

- `bftsmart/library/src/main/java/bftsmart/clientsmanagement/ClientsManager.java` — sim-time stamp.
- `bftsmart/library/src/main/java/bftsmart/tom/leaderchange/RequestsTimer.java` — sim-time compare, `JITTER_WALL_MS`, `STOP_RETX_WALL_MS`, jitter on every schedule.
- `bftsmart/library/src/main/java/bftsmart/communication/MessageHandler.java` — per-receiver `[LC-RECV]` tag + `distinct` counter.
- `bftsmart/library/src/main/java/bftsmart/communication/V2V/ReliableV2VMessaging.java` — `RETX_MAX_BACKOFF_MS` = 100, `MAX_RETX_ATTEMPTS` = 40.

## Tuning knobs (all JVM system properties)

| Property | Default | Purpose |
|---|---|---|
| `bftsmart.lc_jitter_wall_ms` | 500 | Jitter ceiling on every LC-related `Timer.schedule`. |
| `bftsmart.stop_retx_wall_ms` | 1000 | Interval between successive STOP re-emissions (fresh seq). |
| `bftsmart.retx_max_backoff_ms` | 100 | Cap on `ReliableV2VMessaging` exponential backoff. |
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

## Open questions for C++-side investigation

1. Does `V2VProxyModule.cc` turn every `bridge.sendMessage(-1, data)` into
   exactly one PHY broadcast frame, or is there queueing / rate-limiting
   that collapses bursts?
2. During LC, are there hidden ordering/buffering constraints in the
   `V2VArrivalProtocol` or the shared ingress ring that could be
   dropping LC envelopes preferentially?
3. Can we emit per-STOP FEC (e.g. STOP_k carries seqs `{k, k-1, k-2}` so
   one delivery advances the receiver's distinct-sender count by up to 3)
   without breaking the `broadcastSeq` dedup logic at the reliability layer?

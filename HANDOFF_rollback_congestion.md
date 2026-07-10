# Handoff — Rollback (CANCEL(0) → ORDER(1)) Liveness Debugging

**Status:** ORDER(0) reliable. CANCEL(0) reliable-ish (13-16/16 depending on run).
ORDER(1)/seq=3 still does not commit, in any run so far. Root cause for the seq=3
failure is now well-evidenced: **relay-flood channel congestion caused by an
epidemic-relay race, not message ordering, not membership, not signal/geography.**

Log convention used throughout this session: `/tmp/resdb-simulation.log`,
freshly overwritten each run. Copy it before re-running if you want to keep a
specific run's evidence — nothing preserves old runs automatically.

---

## Ledger / ID conventions (load-bearing, easy to get backwards)

- PBFT `seq`: `ORDER(e) = seq 2e+1` (always odd), `CANCEL(e) = seq 2e+2` (always
  even). `seq=1`=ORDER(0), `seq=2`=CANCEL(0), `seq=3`=ORDER(1).
- ID spaces differ by exactly 1: PBFT `sender_id`/`self=` in logs is
  **omnet replica ID + 1**. `omnet_self=`/`r<N>` in logs is the omnet ID directly
  (matches `veh<N>` naming). E.g. `self=6 omnet_self=5` = veh5 = PBFT sender 6.
  Mixing these up (I did, more than once this session) produces false leads.
- The scenario: 16 vehicles (veh0–veh15) run ORDER(0) normally. Partway through,
  veh0 and veh6 depart (their batch clears) and an `[R0-SUPERVISOR]` mechanism
  injects two new vehicles — veh16 (normal) and veh17 (ambulance) — via
  `TraCIScenarioManager::tryR0LateEmergencySpawn`. veh17's presence is meant to
  trigger CANCEL(0) (`reason=1`/EMERGENCY, `ref=amb:veh17:...`), then discovery
  for ORDER(1) should include veh1–5,7–17 (14 originals + veh16/17, minus
  departed veh0/veh6 = 16 members).
- **The log is heavily corrupted by interleaved `std::cout <<` writes from
  concurrent threads with no lock** — many lines are torn/spliced with
  fragments of other lines. Always sanity-check a surprising grep result by
  looking for a *clean* version of the same line elsewhere, or by cross-checking
  with a differently-formatted log tag. This cost significant time this session
  (e.g. initially mis-attributing `sender=1` VOTE-DROPs, an off-by-one in
  interpreting `members=` as PBFT-space when it's actually omnet-space).

---

## Fixes made this session (in order, each confirmed before moving to the next)

### 1. Relay-forwarding over-eager staleness guards (`ResDBTransport.cc`)
Two guards were silently blackholing CANCEL(0)'s own relay traffic once a
replica had already committed ORDER(0):
- `maybeRelayResdbConsensusBytes`: gated on `order_applied_` (too broad — a
  replica whose own order applied must still relay CANCEL(e)/ORDER(e+1) traffic
  for others). Changed to gate on `current_phase_ == DEPARTED` instead, logging
  `[RELAY-GATE-DROP]` when it fires.
- `handleResdbConsensusRelay`: gated on `has_committed_order_ && epoch <=
  last_committed_epoch_` (wrong — CANCEL(e) shares its epoch tag with the
  just-committed ORDER(e), so this treated CANCEL's own live traffic as stale).
  Changed to `epoch < current_epoch_`, logging `[RELAY-GATE-DROP]
  reason=stale-epoch`.

**Effect:** CANCEL(0) went from committing on 0/16 replicas to committing
reliably on most/all. This fix is solid — evidenced by
`[COLLECTOR-COMMIT] seq=2` counts jumping from 0 to 13–16 across subsequent runs.

### 2. `pbft_observed_` scoped incorrectly (multiple files)
`pbft_observed_` gates four cert-discovery/announce timers (meant to mean "a
decision my discovery was feeding has been proposed, stop discovery chatter").
It was being set `true` by *any* consensus message, never reset, which broke
newly-injected vehicles (veh16/veh17): they'd overhear leftover epoch-0
traffic within ~0.3s of spawning and get silenced before ever announcing
themselves (`[ANN-SEND-STOP] r16 pbft_observed=1 cert_broadcast=0`).

Fixed in two rounds:
- Moved the setter from the outer dispatch (`ResDBIntersectionApp.cc`, TYPE8/TYPE11
  branches) into the actual handlers (`ResDBTransport.cc`:
  `handleResdbConsensusMessage`, `handleResdbConsensusRelay`), gated on
  `type == 3 /* PRE_PREPARE */` only (not PREPARE/COMMIT/relay chatter).
- **Then further gated on `seq % 2 == 1`** (ORDER only, not CANCEL) — CANCEL has
  no discovery phase (votes over the static provisioned set), so it structurally
  can never mean "my discovery concluded," for anyone. This was needed because
  CANCEL(0)'s own (legitimate, ambulance-triggered) PRE-PREPARE was itself
  silencing veh16/veh17 mid-announcement, ~0.7s after they spawned — not stale
  noise, a real but *wrong-instance* trigger.
- Added `pbft_observed_ = false;` to the reset block in `beginRollbackDiscovery()`
  (`ResDBRollbackProtocol.cc`) for correctness across repeated rollback cycles.
- Added `[PBFT-OBSERVED]` logging at both setter sites (source=type8/type11,
  seq, view) to make this directly verifiable in future runs.

**Effect:** confirmed via log — veh16/veh17 now successfully broadcast their own
`[CERT-BROADCAST]` before anything silences them. This fix is solid.

### 3. Consensus-relay suppress-on-overhear (`ResDBTransport.cc`, `.h`, `.ned`)
**This is the current open problem area — implemented, confirmed insufficient.**

Diagnosis: fixing #1 made CANCEL(0) relay reliably, but massively increased
relay volume (13→181+ TYPE11-SEND for a single instance), and that volume plus
concurrent cert-discovery traffic saturates the channel exactly when ORDER(1)'s
PRE-PREPARE/PREPARE/COMMIT need to land. Measured ~3000 messages/sec in the
1-second window around ORDER(1)'s proposal (vs ~18/sec in the equivalent window
before ORDER(0)'s proposal, which has no competing traffic).

First attempt: added a "suppress-on-overhear" delay — a node holds a
freshly-seen consensus message for a random window
(`relaySuppressDelayMinSec`/`MaxSec` in `ResDBIntersectionApp.ned`) before
relaying; if it overhears someone else relay the identical message first
(detected via the existing `consensus_relay_seen_` dedup key), it cancels its
own pending relay instead of also transmitting
(`[TYPE11-DROP] reason=suppressed-on-overhear`). New machinery: `PendingConsensusRelay`
struct, `pending_consensus_relays_` queue, `consensus_relay_flush_timer_`,
`scheduleConsensusRelayFlush()`/`flushDueConsensusRelays()`/`sendConsensusRelayNow()`.

Started with a 2–8ms window (too narrow, ~0% effective), widened to 5–30ms
after measuring ~3ms one-hop propagation+processing latency as the floor.

**Verified still insufficient after widening:**
- Per-message relay count still 8–18 (want 1–3).
- `suppressed-on-overhear` only ~7.7% of drops — most relays go out before any
  cancellation signal can arrive.
- **Root cause, confirmed directly in logs:** for one PRE-PREPARE, 10 nodes
  received it via direct TYPE8 within **130 microseconds of each other**
  (`t=20.720587580824` → `t=20.720587721012`). All 10 independently draw a
  random delay and schedule a relay. With N≈10 simultaneous hearers, the
  expected gap between the closest pair of independent draws over even a 25ms
  window is ~2.3ms — under the ~3ms propagation floor needed for a "winner"'s
  transmission to reach and cancel the others. **This is a property of order
  statistics with N≈10+ simultaneous draws, not a tunable-window problem** —
  no reasonably-sized window reliably produces a single winner at this N.

ORDER(1)/seq=3 still fails after this fix: zero PREPARE quorum, zero COMMIT
broadcasts, worse in one run than before the widen (max PREPARE count actually
dropped run to run — this scenario has real run-to-run randomness, don't treat
a single run's numbers as definitive without a few repeats).

---

## Next steps (not yet implemented — pick one and plan before coding)

1. **Relay only from TYPE11 receipt, never from direct TYPE8 hear.** Kills the
   echo-chamber (the N-simultaneous-direct-hearers problem), but has a
   bootstrapping gap: something still has to make the first hop from a TYPE8
   reception. Needs pairing with #3 below for that first hop, or an explicit
   "the sender's single designated first-relayer" rule.
2. **Wider suppress-on-overhear window (30–50ms+).** Same race, just slower —
   the order-statistics math says this helps monotonically but does not
   eliminate the problem, and it's pure added latency for a partial gain. Cheap
   to try, low expected payoff given the math above.
3. **Deterministic relay election** (e.g. lowest omnet ID among a message's
   direct hearers is the sole relayer, next-lowest takes over if it's
   unreachable/departed). No race at all — doesn't depend on beating a
   propagation-delay floor. Bigger structural change (fixed relay
   responsibility vs. epidemic flood) but the only option of the three that
   removes the root cause rather than trying to outrun it. Current lean, but
   unconfirmed — needs a real design pass (how does a node know who else heard
   it directly, without new signaling overhead?) before implementing.

My inclination is **#3, possibly combined with #1** — but this needs a proper
plan-mode pass (design open questions above) before touching code, given two
previous "fix" attempts this session each needed a follow-up correction after
looking wrong in the next run.

---

## Useful greps for the next session

```bash
# High-level success markers
rg -c '\[COLLECTOR-COMMIT\] seq=2' /tmp/resdb-simulation.log   # CANCEL(0)
rg -c '\[COLLECTOR-COMMIT\] seq=3' /tmp/resdb-simulation.log   # ORDER(1) — the target
rg -c '\[EXECUTOR\] OrderDecision: epoch=1' /tmp/resdb-simulation.log

# pbft_observed_ / discovery-silencing regression check (fixed, but verify it stays fixed)
rg '\[ANN-SEND-STOP\] r16|\[ANN-SEND-STOP\] r17' /tmp/resdb-simulation.log
rg '\[PBFT-OBSERVED\]' /tmp/resdb-simulation.log   # should only show odd seq

# Relay congestion diagnosis
rg -c '\[TYPE11-SEND\]' /tmp/resdb-simulation.log
rg -c '\[TYPE11-DROP\]' /tmp/resdb-simulation.log
rg -c 'reason=suppressed-on-overhear' /tmp/resdb-simulation.log
# per-message relay fan-out for seq=3 (want 1-3 per message, currently 8-18):
rg '\[TYPE11-SEND\].*seq=3 ' /tmp/resdb-simulation.log | rg -o 'inner=\S+ view=\d+ seq=\d+ sender=\d+' | sort | uniq -c | sort -rn

# PREPARE/COMMIT quorum progress for seq=3
rg '\[PBFT-COUNT\].*seq=3 type=PREPARE' /tmp/resdb-simulation.log | rg -o 'count=\d+ quorum=\d+' | sort -t= -k2 -n | uniq -c | tail -10
rg '\[PBFT-COUNT\].*seq=3 type=COMMIT' /tmp/resdb-simulation.log | rg -o 'count=\d+ quorum=\d+' | sort -t= -k2 -n | uniq -c | tail -10

# Message-density snapshot around a given proposal time T (integer seconds)
rg " t=<T>\.[0-9]+" /tmp/resdb-simulation.log | rg -o '^\[[A-Z0-9_-]+\]' | sort | uniq -c | sort -rn
```

---

## Files touched this session

```
veins-veins-5.3.1/src/veins/modules/application/resDB/ResDBTransport.cc          (relay gates, pbft_observed_ move+scope, suppress-on-overhear)
veins-veins-5.3.1/src/veins/modules/application/resDB/ResDBIntersectionApp.cc    (pbft_observed_ setter removal, relay flush timer wiring)
veins-veins-5.3.1/src/veins/modules/application/resDB/ResDBIntersectionApp.h     (PendingConsensusRelay struct + members)
veins-veins-5.3.1/src/veins/modules/application/resDB/ResDBIntersectionApp.ned   (relaySuppressDelayMinSec/MaxSec params)
veins-veins-5.3.1/src/veins/modules/application/resDB/ResDBRollbackProtocol.cc   (pbft_observed_ reset in beginRollbackDiscovery)
veins-veins-5.3.1/src/veins/modules/application/resDB/ResDBArrivalProtocol.cc    (pbft_observed_ gate on epidemic cert relay — from an earlier pass, still correct)
```

None of this is committed to git yet (working tree only) — check `git status`/`git diff`
before doing anything destructive.

---

## Caveats / things that turned out to be red herrings this session

- **`self=1` VOTE-DROP `reason=no-view detail=inactive-receiver` for seq=3** —
  not a bug. `self=1` = veh0, which departed; it's correctly told it's not in
  the active view. Don't re-chase this.
- **`members=` list in `[ACTIVE-VIEW]` logs is omnet-space**, not PBFT-sender-space
  (no +1 offset applied for that specific log line, unlike almost everywhere
  else). Membership computation itself (14 originals + veh16/17, minus
  veh0/veh6) has been verified correct.
- **"Missing-pre-prepare" VOTE-DROPs are not, by themselves, fatal** — some
  replicas still reach PREPARE quorum despite them (this is the original Stage-A
  hypothesis from `specv4.md`; it's real but secondary to the relay-congestion
  problem).
- **CANCEL(0) trigger was verified legitimate** (ambulance-triggered,
  `reason=1 ref=amb:veh17:...`), not a departure/CRASH shortcut — an earlier
  hypothesis of mine this session that turned out wrong on inspection.
- Run-to-run variance is real and significant (CANCEL(0) commit count alone
  varies 13–16/16 across otherwise-identical reruns) — don't conclude a fix
  worked or failed from a single run without at least a couple of repeats.

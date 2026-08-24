# Handoff — Scenario 16 Stage 2 (CLEAR + ORDER evidence) — done and verified

**Status:** Stage 2 of `scenario16_crash_wait_clear_spec.md` is implemented and
confirmed working end-to-end against a real `--scenario 16` / `--crash-wait-clear`
run: CANCEL commits, CLEAR forms (6+ trusted witnesses), the ORDER evidence
trailer is built and validated in PBFT PreVerify (zero rejects), and ORDER(1)
committed for all 14 replicas at N=14/f=4/quorum=10. Stage 3 (WAIT) is
deferred — confirmed empirically, not just by design intent, that it is not
needed for this scenario's honest path. Stage 4 (backoff/omission
enforcement) is deferred per spec, though partial backoff was added this
session for unrelated liveness reasons (see below).

Log convention: `/tmp/resdb-simulation.log`, freshly overwritten each run.
Copy it before re-running if you want to keep a specific run's evidence.

---

## What Stage 2 needed and where it landed

Full detail is in the code; this is the map.

- **CLEAR certificate machinery** (`ResDBIntersectionApp.h`/`ResDBRollbackProtocol.cc`):
  `ClearEcho`/`ClearCert` structs, `kClearEchoType=15`/`kClearCertType=16`,
  `sendClearEcho`/`handleClearEcho`/`validateClearCert`/`broadcastClearCert`/
  `handleClearCert` — structurally mirrors the existing CANCEL/BLOCKED pipeline,
  reusing `WitnessStatement`/`WitnessCertificateValidator`/`WitnessEchoCollector`
  from `ResDBWitnessCert.h` directly (no new crypto/threshold logic needed).
  `IncidentState` gained `CLEARED`; `IncidentRecord` gained `clearCertBytes`.
- **Empty-box dwell detection**: `anyVehicleInConflictBoxTraCI()` (new,
  `ResDBTraCI.cc`) + a scan folded into the *existing* `preceding_batch_poll_msg_`
  tick in `handleSelfMsg`, same pattern as Stage 1's crash-dwell scan.
- **The actual correctness fix** (`ResDBRollbackProtocol.cc:trySubmitRollbackProposal`):
  gated the rollback proposer on `hasBlockingIncidentForEpoch(cancelled_epoch_)`
  — before this, `ORDER(1)` was committing immediately after discovery COMPLETE,
  **before the wrecks were even towed**. This was the real bug Stage 2 needed to
  close, not something the spec's acceptance tests alone would have caught by
  inspection.
- **ORDER evidence trailer**: `ResdbOrderEvidenceHdr` (bridge header), appended
  in `ResDBDecision.cc::proposeAll()` for crash-recovery epochs, parsed/adopted
  in `processOrders()` (lets a straggler adopt CLEAR without needing its own
  gossip). Bridge-side: `ResdbOmnetSetClearEvidenceCallback` (new C-ABI,
  mirrors `ResdbOmnetSetOrderCallback`) lets the bridge's PBFT PreVerify reject
  a crash-recovery ORDER lacking valid CLEAR evidence *without* the bridge
  implementing its own weaker validator — it delegates to Veins'
  `validateClearCert` via callback. New bridge-side state: `CrashRecoveryState`
  (epochs requiring evidence, populated when the executor processes a
  CANCEL_CRASH decision) and `ClearEvidenceState` (callback storage) — both
  declared *before* `IntersectionExecutor` in `resdb_omnet_bridge.cc` (a
  forward-reference ordering bug bit this once; watch for it if adding more
  bridge-side shared state).
- **Thread-safety**: `committed_order_vehicle_ids_` is now guarded by a new
  `committed_view_mutex_`, since the CLEAR evidence callback runs on a ResDB
  worker thread (unlike everything else touching that field, which only ever
  ran on the sim thread before this).

## Two real bugs found and fixed mid-session (not in the original plan)

1. **CLEAR could never be perceived at all.** `handleValidCancelJustification`
   cancels `preceding_batch_poll_msg_` (via `stopVehicle()`'s cleanup) while
   CANCEL is being witnessed, and nothing ever rescheduled it. CLEAR's
   empty-box dwell scan rides that same poll — so after CANCEL committed, the
   timer it needed had been dead for ~10s. Fixed by re-arming the poll in
   `beginPostCancelDiscovery()` once CANCEL commits (with jitter — see below).
   The batch-resume logic sharing that handler stays safely inert throughout,
   gated by the pre-existing `cancel_pending_` check.
2. **Forward-reference bug in the bridge** (see above) — `CrashRecoveryState`
   was originally declared near `ResdbOmnetServerHandle` (used at the bottom
   of the file) but referenced inside `IntersectionExecutor` (declared near
   the top) — moved the struct definitions earlier.

## WAIT (Stage 3) — confirmed unnecessary, not just assumed

Checked directly in a successful run: `rollback_rotation_index_` never
incremented, neither `[VC-TRIGGER]` nor `[APP-VC]` fired anywhere, across the
full ~9s CLEAR-wait window. Traced why in the code: `rollback_vc_timer_` (the
recovery-proposer's own retry/rotation timer) only arms *after* `proposeAll()`
is actually called — gating that call on the incident being CLEARED means it
simply never arms while genuinely waiting. The ordinary PBFT view-change
trigger is separately suppressed whenever `cancel_pending_` is true. Net: no
known false-timeout path exists for this scenario as built. Leave Stage 3
deferred unless a future run shows otherwise.

## The CANCEL liveness fragility (found, NOT fixed — deliberately deferred)

Separately from CLEAR, found that **CANCEL's own PBFT consensus round can
stall permanently** with no recovery:

- CANCEL uses N=16/quorum=11 (spec §7.3, intentional — the two crashed/muted
  vehicles remain provisioned members). That's only a **3-vote margin** over
  the 14 real (non-muted) voters.
- Root-caused via the new `scripts/pbft_matrix.py` (see below): PREPARE
  delivery matrix showed most replicas receiving only 9-10 of the 14 real
  senders — one or two short of the 11 needed — so most replicas never
  transitioned to broadcasting their own COMMIT at all
  (`[PBFT-BCAST-COMMIT]` only fired for the few that hit quorum=11). This is
  real congestion eating the spec's own stated liveness margin, not a
  quorum-computation bug — `source=forced, N=16, quorum=11` was confirmed
  correct and consistent everywhere.
- **CANCEL has no proposer-rotation recovery.** `trySubmitCancelProposal()`
  only arms `cancel_vc_timer_` (the retry/rotation trigger) for the replica
  that *is* the current proposer. When that timer fires and rotates to a new
  proposer, the new proposer was never told — it never armed its own timer,
  so nothing ever prompts it to actually try. First-attempt failure = permanent
  stall. **User confirmed this is known/expected** (CANCEL has never supported
  view-change) and wants it addressed alongside the Byzantine scenario work
  later, not now.
- **Partial mitigation added this session**: exponential backoff (spec §11.1-11.3
  shape: `min(base*factor^k, cap)` + jitter) on CANCEL_CERT retry, CLEAR_CERT
  retry, and CANCEL-commit gossip — new `backoffDelaySec()` helper in
  `ResDBRollbackProtocol.cc`, new NED params `evidenceRetryBaseSec/Factor/CapSec`
  (0.1s/2.0/2.0s) and `cancelGossipRetryBaseSec/CapSec` (0.25s/4.0s). PBFT's own
  PRE_PREPARE/PREPARE/COMMIT retry was deliberately left fixed-interval per the
  spec's own reasoning (backing off phase-liveness traffic risks turning
  ordinary packet loss into long stalls) — this was a direct instruction, not
  an oversight.
- This does **not** fix the rotation gap — it only reduces the odds of hitting
  it by lowering channel contention during the critical quorum-forming window.
  The rotation mechanism itself is still a real gap.

## Log analysis: two new reusable scripts

The log is heavily corrupted by interleaved `std::cout` writes from concurrent
ResDB worker threads with no shared lock — e.g. real `self=4` and a
concurrently-written `5` from another thread can land as `self=45` in the
file, silently producing wrong numbers from naive grep/awk. This cost real
time before being caught (twice — see also the unrelated
`HANDOFF_rollback_congestion.md` from an earlier session, which hit the exact
same corruption issue independently).

- **`scripts/loglens.sh`** — categorized viewer (`crash`, `incident`, `cancel`,
  `discovery`, `clear`, `rollback`, `vc`, `order`, `pbft` sections). Start here
  for a first look at any run.
- **`scripts/pbft_matrix.py`** — the tool that actually cracked both root
  causes this session. Every parser is anchored to the full line (`^...$`)
  and drops anything that doesn't match cleanly, plus a plausibility filter
  (replica/self/sender IDs outside 0-20 are dropped as corruption, not
  reported as fact) for the narrower case where corruption lands *inside* a
  numeric field without breaking line structure. Subcommands: `matrix`
  (per-replica delivery matrix for a phase+seq — answers "systematic vs
  random loss"), `retry-arm` (which replicas ever attempted to send for a
  phase+seq), `quorum`, `forced-view`, `corruption`.

**ID-space convention** (also documented independently in
`HANDOFF_rollback_congestion.md` — easy to get backwards, confirmed twice
now): PBFT `sender_id`/`self=` in logs is **omnet replica ID + 1**.
`omnet_self=`/`r<N>` in logs is the omnet ID directly (matches `veh<N>`
naming). `pbft_matrix.py` currently reports raw `self=`/`sender=` values
(resdb-space) — convert by subtracting 1 to get the omnet/`veh<N>` ID.

## Suggested next session

1. Re-run scenario 16 a few times with the backoff changes in place; confirm
   CANCEL's stall doesn't recur, or at least recurs less often. Not guaranteed
   fixed — the rotation gap is still there.
2. Byzantine scenario work for 15 and 16 (new faults targeting CANCEL/BLOCKED/
   CLEAR specifically — forged CANCEL_ECHO/CLEAR_ECHO, a Byzantine CANCEL
   proposer, a leader omitting CLEAR evidence — user chose this over reusing
   the existing ordinary-path `ByzantineType` enum). Fold in CANCEL's
   proposer-rotation fix as part of this work, per the user's own framing.
3. Spec's remaining unverified acceptance tests (§20): the negative case (one
   wreck remains → no CLEAR_CERT should form — only the both-removed happy
   path has been tested), and "at least 10 seeds" per stage.

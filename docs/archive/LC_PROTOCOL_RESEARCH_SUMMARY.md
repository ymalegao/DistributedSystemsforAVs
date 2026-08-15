# Leader-Change Under Lossy V2V Broadcast: Problem, Design, and Rationale

This note is written for a **paper reviewer**: what problem we faced, what “fixing it” means, **why** we changed BFT-SMaRt’s leader-change (LC) interaction with the transport, and **why** each design choice is sensible without weakening BFT safety.

---

## 1. Background and motivation

### 1.1 System context

We study BFT-SMaRt-style consensus over **802.11p-style V2V broadcast** in OMNeT++/Veins, rather than over switched TCP links. In that setting:

- The medium is **shared**, **lossy**, and **contention-limited**.
- “Multicast” is often implemented as a **single physical broadcast** observed by many receivers, but the integration layer may still attach **per-receiver logical semantics** (sequence numbers, ACK expectations, retransmission queues).
- Simulation time and wall-clock time can diverge strongly under load, so **timeout comparisons** must be tied to **simulation time** if experiments are to be reproducible and meaningful.

### 1.2 What leader change is doing (BFT-SMaRt LC, conceptually)

Leader change is a **multi-phase agreement** whose purpose is to pick a new leader/regency and synchronize enough state to continue safely. At a high level (names vary by code path), the story is:

1. **Phase 1 — STOP quorum:** replicas collect evidence that enough distinct acceptors want to abandon the current leader for a new regency. This is the “liveness ignition” phase: it must happen quickly enough that the cluster does not fragment.
2. **Phase 2 — STOPDATA to new leader:** each replica ships enough local information for the new leader to compute the next safe state transition.
3. **Phase 3 — SYNC:** the new leader broadcasts the reconciled outcome so everyone can install the same view of history and resume consensus.

BFT safety is enforced by **cryptographic evidence and quorum thresholds** in these phases. **Transport optimizations must not replace those checks**; they may only affect *how quickly* honest evidence arrives.

### 1.3 Why this is hard on V2V (reviewer intuition)

On a wired datacenter fabric, “eventually delivered” is a reasonable engineering assumption for small control messages. On 802.11p:

- **Collisions and backoff** create correlated loss bursts.
- **Broadcast storms** from \(N\) replicas retransmitting on similar timers can **raise the loss probability for everyone**, including messages that are *more critical later* (STOPDATA, SYNC).
- **Asymmetric reception** is normal: two honest replicas can have very different “who I have heard from” sets at the same wall/sim time.

So the failure mode is not “BFT math is wrong,” it is **liveness under contention**: honest replicas remain stuck before cryptographic quorums are assembled, or they **advance locally at different speeds** and stop contributing to the *same* regency instance.

---

## 2. Problem statement (what was broken, in reviewer terms)

### 2.1 Observable symptoms

Under Byzantine-leader stress at \(N{=}16\), \(f{=}5\):

- Normal consensus rounds could still be fast when the leader behaves.
- Leader change often **did not terminate within the experiment window**:
  - long STOP storms,
  - missing STOPDATA at the new leader,
  - missing or duplicated SYNC-related progress,
  - occasional **regency splits** (subsets of replicas behaving as if the next regency had already started while others were still completing the previous one).

### 2.2 What “solving it” means (success criteria)

We consider the problem “addressed” when, under the same scenario:

1. **Phase 1 completes**: enough distinct STOP evidence is collected cluster-wide so honest replicas do not remain permanently below quorum thresholds.
2. **Phase 2 completes**: the new leader receives enough distinct STOPDATA contributions to satisfy the protocol’s Byzantine/CFT quorum checks and proceed.
3. **Phase 3 completes**: SYNC is emitted once per regency episode (not in a tight loop), and replicas converge rather than amplifying channel load after quorum is reached.
4. **No systematic regency split** caused purely by timer/replica-local state races (as opposed to true Byzantine-induced timeouts).

Metrics we used operationally include **sim-time from first STOP activity to leader decision/SYNC** (“stop-to-decision”), STOP/STOPDATA/SYNC visibility in logs, and **distinct-sender counts** per phase.

---

## 3. Root-cause decomposition (why naive LC + wireless integration fails)

We separate causes into three layers because reviewers will ask “which layer was wrong?”

### 3.1 Protocol-scheduling layer (when to start LC, when to escalate)

Leader change is triggered by timeouts and consensus anomalies. In a simulator:

- Wall-clock timers can fire **many times per unit simulation time** when the sim runs slowly.
- If “start LC / escalate regency” is not **idempotent** across those wakes, replicas can **re-enter escalation** after partial local progress (e.g., immediately after local Phase-2-style state advances), splitting the cluster’s STOP senders across regencies.

**Key point:** this is not “wireless is noisy”; it is **incorrect composition of real-time scheduling with a discrete-event simulation clock**.

### 3.2 Application-transport interface layer (what to retransmit, and how aggressively)

BFT-SMaRt LC uses **retransmissions** to overcome loss. Retransmissions are not free:

- If every replica blindly rebroadcasts full STOP messages at high rate, the channel can enter a regime where **the probability of delivering any particular STOP decreases** even though “more copies” are sent.
- This can starve later phases because STOPDATA/SYNC are typically **larger** and more sensitive to contention.

So the design question becomes: **how do we reduce the average airtime per “missing evidence” event without falsifying evidence?**

### 3.3 Reliability/ordering layer (STOPDATA path)

STOPDATA is logically “to the leader,” but the radio is still broadcast. A common integration bug pattern is:

- treat some LC messages as “unordered / fire-and-forget” on one API path (e.g., broadcast),
- but still apply **strict per-sender sequence numbers** on another API path (e.g., logical unicast to leader),

If STOPDATA retransmissions **consume new sequence numbers** while an earlier sequence number is lost, the leader’s receiver can enter a classic **sequence-gap stall**: it buffers future copies while waiting for a missing predecessor that will never be “filled in” under the current retransmission strategy.

**This is the reviewer-critical clarification:** the dominant STOPDATA stall we diagnosed was **not** “timeouts used wall time instead of sim time.” Sim-time timeouts matter for fairness of *when LC starts*, but they do not fix **ordered-delivery deadlock** created by inconsistent sequencing rules across send paths.

Additionally, because the PHY broadcast is visible to non-targets, receivers must enforce **logical destination filtering** for unicasts so unrelated replicas do not advance sequence state for traffic not addressed to them.

---

## 4. Design goals (what we optimized for)

1. **Preserve BFT safety:** no substitute for \(2f{+}1\) STOP evidence; no “trust hearsay”; STOP_NACK does not become a new quorum channel—it only prompts resends of already valid STOP messages.
2. **Improve liveness under contention:** reduce unnecessary correlated transmissions; prefer **targeted** recovery after an initial seeding burst.
3. **Make escalation idempotent:** “one LC episode” should not spawn multiple accidental regency bumps due to timer storms or alternate trigger paths.
4. **Make retransmission semantics consistent:** any message class that is intentionally **self-repaired at the application layer** must not simultaneously be treated as **strictly ordered** on a path where each repair attempt allocates a new sequence slot.

---

## 5. Phase 1 design: why NACK, and why it makes sense

### 5.1 Why not “just retransmit STOP faster”?

Blind periodic STOP flooding scales poorly on broadcast wireless:

- airtime grows with the number of replicas and retransmission rate,
- collisions increase superlinearly in congested regimes,
- the tail of “missing distinct senders” becomes harder—not easier—to finish.

So we need a **tail policy**: after a short initial blind burst (to seed the network quickly), replicas should switch to a **low-airtime** signal that identifies *which* evidence is still missing.

### 5.2 What STOP_NACK is (and is not)

**STOP_NACK is a transport hint**, not a new BFT message type for voting:

- It carries a compact bitmask describing which acceptor IDs the sender has **not yet heard** on the wire for the current regency.
- Receivers who see their bit set may resend their **original signed STOP** (same cryptographic object the protocol already required).
- The normal STOP path still performs **quorum accounting** using authenticated STOP messages.

**Reviewer safety question:** “Could a Byzantine node weaponize NACKs?”  
Yes, in principle—so the design includes **rate limits** on how many STOP resends a replica will perform per peer per regency (a DoS mitigation). Honest behavior remains safe because a NACK cannot fabricate quorum; it can only elicit additional copies of messages that must still pass validation.

### 5.3 Why a separate “heard” set is needed (design detail that matters)

During LC, internal LC bookkeeping may **prune** stop sets after local progress transitions (to avoid unbounded memory growth). That pruning is correct for the core state machine, but it is a **poor source of truth** for “who have I heard on the radio?” for NACK mask construction.

So we maintain a **transport-level heard set** keyed by regency:

- updated on STOP delivery at the message ingress point,
- used only for NACK mask computation,
- cleared when the regency is fully installed.

This avoids pathological masks (e.g., “everyone missing”) that would **amplify** traffic exactly when the channel is already worst.

### 5.4 Why broadcast replies to NACKs (in our final iteration)

When many replicas are simultaneously missing the **same** small set of early senders, unicast replies to each NACKer recreate a **coupon-collector** problem: many independent delivery events must succeed.

Broadcasting the prompted STOP reply (still as a standard STOP message for quorum purposes) aligns with the physical reality of 802.11p and can allow **one successful transmission** to advance many receivers at once.

Combined with bounded reply counts and jitter, this is a pragmatic trade: **more duplicates on success**, but fewer “missed opportunities” under loss.

### 5.5 Blind count vs NACK mode (why thresholds matter)

If the blind phase is too short, replicas enter NACK mode while their heard-set is still extremely sparse, producing **near-full masks** and NACK storms.

If the blind phase is too long, replicas remain in **high-airtime STOP flooding**, starving STOPDATA/SYNC.

So the blind/NACK boundary is a **knob** tied to observed contention and \(N\); it is not a universal constant. The engineering principle is: **enough blind emissions to seed**, then **NACK for tail completion**.

---

## 6. Escalation control design: why idempotence is non-optional

Even if STOP delivery improves, LC can still fail if replicas **do not agree on which regency episode** they are in.

We implemented **epoch-style claiming** for “start/escalate this LC step”:

- The claim must cover **all** entry points into escalation (not only the client-request timer path), because other modules can force LC entry on anomalies.
- The claim must allow a **liveness escape** on long stalls (otherwise a single stuck episode could deadlock the system), implemented as a simulation-time guard window.
- State must reset when a regency is **fully installed** so future episodes can start cleanly.

**Reviewer question:** “Is this standard BFT-SMaRt?”  
No—it is an **integration correctness** layer for a deployment where time and triggers are not aligned like in the original environment assumptions.

---

## 7. Phase 2/3 design: STOPDATA retransmission + transport semantics

### 7.1 Why application-level STOPDATA retransmission exists

STOPDATA payloads can be large; loss is catastrophic because the leader may never reach the STOPDATA quorum threshold to synthesize SYNC.

Application-level retransmission is a simple liveness hedge: **repeat the same logical message until the regency is installed**, gated by simulation time so it remains meaningful under variable sim speed.

### 7.2 Why STOPDATA must be “unordered” on *both* multicast and logical-unicast paths

This is the core “how/why” answer reviewers ask after reading a summary that mentions STOPDATA:

- If STOPDATA is treated as strict ordered traffic, then **every resend** that allocates a **new sequence number** can create a **gap** if an earlier number was lost.
- STOPDATA-to-leader is often sent via a **logical unicast API** even though the radio is broadcast.
- Therefore, “we made STOPDATA unordered on multicast” is insufficient if unicast still orders.

The consistent rule is: **STOPDATA is application-self-repaired**, so the transport should not impose a strictly increasing sequence contract that conflicts with “send again until success.”

### 7.3 Why logical destination filtering still matters

Because the physical layer is broadcast, every receiver may see a copy. Unordered delivery must therefore be:

- **deliver immediately** without waiting for sequence predecessors, **but**
- **only** if the frame is logically addressed to this node (or explicitly broadcast-intended).

Otherwise non-targets may corrupt their sequence state or deliver messages intended for the leader.

### 7.4 Why duplicate SYNC suppression matters

Once STOPDATA quorum is reached, duplicates may still arrive due to retransmissions. Without suppression, the leader may repeatedly enter “finalize SYNC” logic, re-broadcasting SYNC and wasting airtime precisely when the network is busiest.

A regency-scoped “SYNC already sent” guard is a **pure liveness/efficiency** measure: it does not change the first SYNC’s contents or quorum validity.

---

## 8. Safety vs liveness: what we did *not* change

We did not change the meaning of:

- \(2f{+}1\) STOP quorum requirements,
- STOPDATA/SYNC quorum thresholds,
- cryptographic validation paths for accepted messages.

We changed **scheduling, retransmission policy, transport classification, and duplicate suppression** so those checks are reached **more often** under loss and Byzantine triggering.

**Reviewer takeaway:** this is a **systems integration contribution** (wireless + discrete-event simulation + BFT LC), not a modification to BFT safety arguments.

---

## 9. Limitations and open reviewer questions

1. **Knob sensitivity:** blind/NACK thresholds and reply caps depend on \(N\), contention, and payload sizes; ablations belong in evaluation.
2. **Byzantine NACK bandwidth:** caps mitigate but do not eliminate misuse; stronger policies (token bucket, exponential backoff on NACK rate) may be needed at larger \(N\).
3. **Cross-layer coupling:** MAC scheduling (EDCA), frame sizing, and batching interact with these application choices; we treat them as future work unless measured jointly.

---

## 10. One-sentence conclusion

We kept BFT-SMaRt’s LC **safety-critical quorum logic** intact, and redesigned the **interaction between LC retransmission policy and wireless reliability/ordering** so that leader change can **finish under broadcast loss** instead of deadlocking in “almost-quorum” regimes or splitting regencies due to timer composition bugs.

---

## 11. Reviewer Challenges and Direct Answers

This section answers the exact hard questions a reviewer is likely to ask.

### Q1) “Why not just use a reliable multicast transport and keep LC untouched?”

**Short answer:** because “make transport deliver everything to everyone” is the wrong objective for this workload.

In this codebase, reliable delivery uses per-target sequence tracking, ACK processing, and unacked-message retransmission state. That model is appropriate for some traffic classes, but under dense LC episodes it can create ACK/retx amplification. Our objective for LC is narrower and protocol-aligned: deliver enough *valid* evidence to cross BFT quorums, not guarantee lossless delivery of every copy to every node.

NACK-based repair is intentionally quorum-centric:

- replicas first seed the network with a short blind STOP burst,
- then request only missing senders via compact bitmasks,
- and responders resend only what is needed for quorum progress.

So the design is not “hack around transport,” but “align transport effort with BFT liveness requirements.” This avoids paying full reliable-multicast cost for messages where BFT logic already tolerates loss/duplication.

### Q2) “Is the sim-time/timer issue just an OMNeT artifact?”

**Short answer:** no; OMNeT made it visible, but the systems lesson is general.

The failure class is: timeout-driven state transitions are not idempotent under scheduling jitter. In simulation, jitter appears as wall-time vs sim-time skew. In deployment, the same pattern appears via JVM pauses, OS scheduling delays, CPU contention, or radio-driver backpressure. If a node can re-enter escalation logic multiple times for one logical episode, regency split/liveness collapse can occur regardless of environment.

So the actionable claim is broader than simulation: **BFT LC escalation paths must be idempotent across all trigger entry points**.

### Q3) “Your blind/NACK threshold is a knob. What about dynamic \(N\)?”

**Current implementation:** the defaults are static (`STOP_BLIND_EMITS`, `STOP_RETX_SIM_MS`, `NACK_REPLIES_PER_PEER`), with JVM overrides.

**Why this is acceptable now:** it is explicit, reproducible, and easy to ablate in experiments.

**What is still missing (and should be stated):** adaptive tuning. A straightforward next step is to derive blind-burst count from active view size and short-term channel feedback, e.g.:

- increase blind count as \(N\) grows,
- reduce blind count when heard-set growth is fast,
- increase NACK reply cap only when missing-mask entropy remains high.

Because LC already has a shared membership view, this can be done deterministically per regency without adding new agreement messages.

### Q4) “Can colluding Byzantine nodes DoS via NACK storms?”

**Current protection in code:** bounded replies per peer/regency (`NACK_REPLIES_PER_PEER`) plus jitter.

**Important truth:** this mitigates but does not eliminate channel-pressure attacks. Also, by design, honest replicas may still answer NACKs after they locally pass Phase 1, because helping laggards can be necessary for global convergence before SYNC installs the regency.

So today’s design is a liveness-first compromise: keep helping, but cap amplification.

**Reviewer-facing limitation (explicit):** we do not yet enforce strict Phase-2/3 prioritization against adversarial NACK flooding. A stronger next defense is a per-regency token budget or phased policy (e.g., sharply reduce NACK service once local STOPDATA has been sent and collect quorum is near).

### Q5) “What about STOPDATA/SYNC size and 802.11p fragmentation?”

What the code clearly shows:

- no custom LC-layer fragmentation/reassembly logic is implemented in the Java LC path;
- payloads are serialized into envelopes and handed to the lower stack;
- our fixes target ordering/repair semantics (unordered self-repair where appropriate), not MTU-level packetization policy.

Therefore, fragmentation behavior depends on lower-layer network modeling/configuration. For the paper, we should explicitly report measured STOPDATA/SYNC serialized sizes and state whether runs stayed within single-frame limits in the chosen PHY/MAC configuration. If messages exceed practical frame size, fragmentation loss sensitivity is real and should be discussed as a threat to validity.

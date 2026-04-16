---
name: f+1 Phase Necessity Analysis
description: Formal verdict that f+1 pre-consensus phase closes G1/G2/G3 gaps that BFT-SMaRt alone cannot
type: project
---

Formal verdict confirmed 2026-04-15: the f+1 ARRIVAL_ECHO pre-consensus phase is
necessary at the protocol level (not merely an optimization) for three distinct reasons.

**G1 (Ghost Vehicle / EP1):** BFT-SMaRt authenticates signatures, not physical presence.
TraCI oracle is local-only; without f+1 aggregation, a ghost vehicle's signed request
is indistinguishable at the consensus layer. The f+1 phase requires f+1 independent
TraCI verifications before cert is valid.

**G2 (Equivocation / EP2):** Equivocator cannot dual-certify because 2(f+1) signers
required but only 2f+1 honest replicas exist. BFT-SMaRt quorum intersection prevents
conflicting decisions but does not prevent conflicting lane values from being admitted
into consensus in the first place.

**G3 (Censorship / EP5):** ARRIVAL_CERT multicast (not just send-to-leader) is load-bearing
for EP5. Without it, a new honest leader after view-change cannot recover suppressed
honest vehicle's cert from collectedCerts (which would be empty). Multicast ensures
every honest replica holds all certs before consensus begins.

**G2 Refinement (2026-04-15):** User counter-argument that equivocation doesn't break EP2
is *largely correct* for formal Agreement. BFT-SMaRt terminates on a single consistent
value. However the f+1 phase prevents an EQUIVOCATOR from controlling lane assignment
via view-change timing (dual-cert manipulation attack). Paper must scope formal theorem
to Agreement correctness; acknowledge lane-assignment manipulation as a separate threat.
f+1 is defense-in-depth for G2, not a hard correctness requirement if EP2 is scoped
narrowly to "no two honest replicas decide differently."

**G3 Refinement (2026-04-15):** OrderRequestVerifier CANNOT substitute for ARRIVAL_CERT
multicast. The verifier validates what is present in a proposal; it cannot detect
omissions of vehicles whose certs were unicast-to-leader and dropped. ARRIVAL_CERT
multicast is non-negotiable for EP5. View-change only recovers liveness if the new
honest leader has the cert in collectedCerts — which requires prior multicast.

**G3 Further Refinement — WAVE multicast assumption (2026-04-15):** Even under WAVE
multicast for ARRIVAL_ANNOUNCE, Protocol A (no cert phase) breaks EP5 because:
(a) OrderRequestVerifier detects what is present, not what is absent — Byzantine leader
omission still succeeds if it achieves 2f+1 WRITEs for the incomplete proposal;
(b) Under adversarial channel conditions, |R(H) ∩ H| < 2f+1 is possible (per-receiver
WAVE packet loss), so followers may legitimately reject H even if H is honest;
(c) Cert multicast creates a replicated artifact in collectedCerts that survives both
Byzantine leader omission and individual WAVE packet loss. Protocol A provides no
equivalent durability mechanism.

**WAVE equivocation nuance (2026-04-15):** WAVE broadcast does not make equivocation
structurally impossible. A Byzantine node can send two packets; adversarial channel
conditions (selective interference) can cause P1 to reach subset S1 and P2 to reach S2.
Protocol B closes this via Equivocator-Cannot-Dual-Certify lemma (requires 2f+2 honest
signers, but only 2f+1 exist). Protocol A has no equivalent structural closure.

**EP summary (Protocol A vs Protocol B, 2026-04-15 — REVISED after cert-clearing discovery):**
- EP1: Protocol A conditional (strengthened verifier needed; single-sensor viewpoint weakness)
- EP2: Protocol A holds in simulation (shared SUMO state); breaks in deployment with sensor heterogeneity
- EP3: Both hold; cert phase irrelevant
- EP4: Both hold; cert phase irrelevant
- EP5: REVISED AGAIN (2026-04-15 session 2) — isValidRequest() extension NOW IMPLEMENTED
  (user confirmed intent). Under unified node model + extended isValidRequest() + H2/H5,
  Protocol B holds EP5 INTRA-EPOCH. Cross-epoch f+1-epoch bound is fallback only (when H2
  fails, i.e., fewer than f+1 honest followers received cert before proposal). This is a
  FORMAL GUARANTEE, not defense-in-depth, under the stated hypotheses. Differential cert set
  edge case (honest leader's cert set lags a follower's) is a new liveness concern — requires
  H5 (cert convergence assumption) to exclude. Evaluation must validate H2/H5 empirically.
- EP6: Protocol A weakly holds (no explicit abort signal; stall-based exclusion only)

**Cert clearing semantics confirmed:**
- V2VEpochPreemption.cc clears collectedCerts on epoch boundary — semantically correct for
  mobile VANETs (stale certs would assert outdated physical facts, breaking EP1).
- V2VTraCI.cc per-round reset is researcher's own code; moot under single-round-per-epoch design.
- Do NOT persist certs across epoch boundaries: stale-cert attack would violate EP1.

**G1 Refinement (2026-04-15):** Strengthening OrderRequestVerifier to gate on
physicallyObservedCars partially closes the ghost gap at the Java layer, but creates
a new proof obligation that ARRIVAL_ANNOUNCE was multicast. The f+1 threshold is the
mechanism tying the Java verifier back to the C++ TraCI oracle across the language
boundary. Without it, a Byzantine sender can multicast a forged cert that passes
isValidRequest() because the Java layer cannot re-run TraCI.

**Why: Professor's "authenticated consensus is sufficient" argument fails** because it
conflates detection site with protection mechanism — TraCI can detect faults post-hoc
but cannot prevent invalid values from entering consensus decisions.

**How to apply:** When constructing EP1, EP2, EP5 proofs, always cite the specific
mechanism (TraCI local check, XXHash32 lane embedding, ARRIVAL_CERT multicast) that
makes each property hold. Never accept "BFT-SMaRt handles this" without tracing which
specific mechanism covers the cyber-physical gap. For G2, be explicit about whether
the paper claims only formal Agreement or also lane-assignment manipulation resistance.

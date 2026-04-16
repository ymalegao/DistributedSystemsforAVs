---
name: Confirmed Lemmas
description: Lemmas confirmed sound across sessions; load-bearing for EP1/EP2/EP5/EP6 proofs
type: reference
---

## Lemma: Ghost Vehicle Exclusion (EP1 basis)
A Byzantine vehicle B2 with no physical presence at the intersection cannot obtain f+1
ARRIVAL_ECHOs. Proof: each honest replica calls verifyCarPosition() before echoing;
TraCI returns no match; B2 can collect at most f echoes (from Byzantine colluders);
f < f+1. Therefore B2 is QUIET. Load-bearing for EP1 Validity.

## Lemma: Equivocator Cannot Dual-Certify (EP2 basis)
A Byzantine EQUIVOCATOR B1 cannot hold both a valid ARRIVAL_CERT for LANE_N and a
valid ARRIVAL_CERT for LANE_E in the same epoch. Proof: each cert requires f+1 distinct
honest signers; lane is embedded in XXHash32 signature input; an honest replica signs
at most one lane per carId per epoch (Integrity gate); therefore the two signer sets
require 2(f+1) = 2f+2 honest replicas, but only 2f+1 exist — contradiction.
Load-bearing for EP2 Agreement.

## Lemma 5.U: Cert Entry Implies TraCI Verification (Unified Node basis)
For any honest vehicle v and any car c: if c in collectedCerts_v^e then c was physically
verified by at least f+1 distinct honest replicas via verifyCarPosition() in epoch e.
Proof: cert assembly in V2VArrivalProtocol (C++) requires f+1 valid ARRIVAL_ECHO
signatures before writing to collectedCerts; each honest echo requires verifyCarPosition()
to succeed; at most f Byzantine replicas can contribute forged echoes; therefore f+1
echoes implies at least 1 honest echo from each of f+1 distinct honest replicas.
Load-bearing for the chain: TraCI (C++) → collectedCerts (C++) → isValidRequest() (Java).

## Lemma 5.E: Cert Propagation Before Proposal
If H1 assembles a valid ARRIVAL_CERT and multicasts it before leader L_e invokes
startConsensus(), then every honest vehicle v that receives the multicast has H1 in
collectedCerts_v^e. Load-bearing for EP5 intra-epoch proof.

## Lemma 5.Q: Quorum Rejection Under Partial Propagation
Let S be the set of honest followers that received H1's ARRIVAL_CERT before the proposal.
If |S| >= f+1, then a Byzantine leader BL cannot obtain 2f+1 WRITE votes for any proposal
omitting H1. Proof: each v in S rejects via extended isValidRequest(); WRITE votes are
bounded by f (Byzantine) + (2f+1 - |S|) <= 3f+1 - (f+1) = 2f < 2f+1. QED.
CRITICAL EDGE CASE: if |S| < f+1, the lemma does not apply and Byzantine omission
may succeed in this epoch. Cross-epoch f+1-epoch bound remains as fallback.

## Theorem EP5 Termination — REVISED (2026-04-15, intra-epoch under extension)
EP5 now holds INTRA-EPOCH under unified node model + extended isValidRequest(), subject
to hypotheses H1–H5:
  H1: H1 is physically present and assembles valid ARRIVAL_CERT within epoch e.
  H2: H1's cert reaches f+1 honest followers before startConsensus() is called.
  H3: collectedCerts is populated in C++ and readable by Java OrderRequestVerifier.
  H4: isValidRequest() extended per INV-isVR (reject if any cert in local collectedCerts
      is absent from proposal order bag).
  H5: Cert convergence — honest nodes converge to same collectedCerts before proposal
      (satisfied under reliable WAVE delivery within intersection communication radius).

PROOF CASES:
  Case 1 (BL includes H1): commits normally.
  Case 2 (BL omits H1): |S| >= f+1 by H2; Lemma 5.Q prevents quorum;
    RequestsTimer fires; view-change; new honest leader calls buildProposal();
    collectedCerts NOT cleared on intra-epoch view-change; H1 included; commits.
  Case 3 (silent BL): same path as Case 2 via RequestsTimer.

LATENCY BOUND: decided within tau_cert + tau_R + tau_vc (at most one view-change).
PRIOR BOUND DEMOTED: f+1-epoch cross-epoch bound now applies only when H2 fails.

## Key proof dependency confirmed
EP1 is prerequisite for EP2: Agreement rests on the validity of admitted certs.
If EP1 fails (ghost cert admitted), buildProposal() determinism alone cannot save EP2.

## Lemma A: Computation Integrity (Interpretation A, 2026-04-15)
If a follower F re-runs buildProposal() on the leader's declared <vehicleStates> and
<perCarCerts> and checks that the result equals the leader's <orderBag>, then F catches
any Byzantine leader that correctly declares all vehicles but computes a wrong orderBag
(wrong ordering, ambulance priority inversion, suboptimal batching). Formally: if
buildProposal(leader_inputs) != leader_orderBag, F rejects.
CRITICAL: Interpretation A is OMISSION-BLIND. A Byzantine leader that omits H1 from
both <vehicleStates> AND <perCarCerts> passes Interpretation A because the re-execution
is on the leader's own incomplete inputs.

## Lemma B: Proposal Completeness (Interpretation B / Firewall INV-isVR, 2026-04-15)
For every cert c in F.collectedCerts, F checks c.vehicleId appears in <orderBag>.
This is the sole mechanism catching Byzantine leader omission (censorship, G3).
Interpretation A does NOT provide this. Firewall is the completeness check;
Interpretation A is the correctness check. They are orthogonal threat surfaces.

## Combined Verifier V(A+B) — Theorem EP5 Revised Hypotheses (2026-04-15)
Adding Interpretation A alongside the firewall (INV-isVR) adds one new hypothesis:
  H6: waitRegistry synchrony — all honest replicas derive waitRegistry^e from the
      committed BFT decision log for prior epochs (not independent per-node state).
If H6 fails, Interpretation A produces false rejections from honest leaders (because
different waitRegistry values yield different buildProposal() outputs on same cert set).
Under H1–H6, the combined verifier guarantees that the decided orderBag equals
buildProposal(collectedCerts_L) — i.e., the correct output, not just some valid ordering.
Ambulance priority correctness is now a formal guarantee, not just a design property.

## buildProposal() signature note (confirmed from source)
OrderScheduler.buildProposal() takes Map<String,VehicleState> view, int epoch,
Map<String,Integer> waitRegistry. The waitRegistry is a SECOND input parameter beyond
the cert set. This is the source of H6 — waitRegistry must be synchronised across
honest replicas for Interpretation B (re-execute on local data) to match the leader.

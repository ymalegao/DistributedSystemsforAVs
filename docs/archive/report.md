% DISC 2026 Official Submission Template
% Strictly adheres to LIPIcs formatting and double-blind anonymity requirements.
\documentclass{lipics-v2021}

% Required Packages for Distributed Systems Mathematics and Vehicular Formatting
\usepackage{algorithm}
\usepackage{algpseudocode}
\usepackage{amsmath, amssymb, amsthm}
\usepackage{graphicx}
\usepackage{booktabs}
\usepackage{hyperref}
\usepackage{tikz}
\usetikzlibrary{fit, positioning, arrows.meta}

% Title and Metadata Definition
\title{Fair and Fault-Tolerant: Distributed Intersection Management with Priority Lane Flushing via BFT Consensus}
\titlerunning{Distributed Intersection Management via BFT Consensus}
\author{Anonymous Author(s)}{Anonymous Affiliation}{}{}{}
\authorrunning{Anonymous Author(s)}
\Copyright{Anonymous Author(s)}

% ACM Computing Classification System (CCS) required by LIPIcs
\ccsdesc{Computer systems organization~Dependable and fault-tolerant systems and networks}
\ccsdesc{Applied computing~Transportation}
\keywords{Byzantine Fault Tolerance, Autonomous Intersection Management, Connected and Autonomous Vehicles, Fairness, Simulation, JNI}

\begin{document}
\maketitle

\begin{abstract}
% The abstract must concisely summarize the entire paper in under 400 words.
% It must clearly state the problem: Vulnerabilities of centralized AIM and leader-based VTLs to Byzantine faults, and the failure of traditional BFT to account for spatial fairness and emergency preemption.
% It must outline the proposed solution: A novel BFT consensus adaptation featuring a mathematically verifiable fairness utility function and a cryptographic priority lane-flushing interrupt for emergency vehicles.
% It must detail the rigorous methodology: The unique JNI-bridged simulation architecture integrating BFT-SMaRt with the OMNeT++/SUMO/Veins stack to synchronize discrete-event network modeling with JVM execution.
% It must highlight the tangible contributions: Improved Jain's Fairness Index, reduced emergency traversal latency, and the open-source release of a standardized SUMO benchmark dataset for the distributed computing community.
\end{abstract}

\section{Introduction}
% Establish the broad context: The global shift toward Autonomous Intersection Management (AIM) to alleviate urban congestion and the critical vulnerabilities inherent in centralized infrastructure.
% Define the specific threat landscape: The susceptibility of Connected and Autonomous Vehicles (CAVs) to Byzantine faults, including sensor degradation, localized spoofing, and malicious actors.
% Articulate the primary research gap: While BFT ensures logical safety and liveness, it entirely lacks mechanisms for spatial-temporal fairness and urgent physical preemption.
% Explicitly state the paper's core contributions:
% 1. A BFT protocol optimized for physical intersection kinematics.
% 2. A verifiable fairness and priority mechanism enabling emergency lane flushing.
% 3. A novel, time-synchronized simulation architecture connecting BFT-SMaRt to Veins via JNI.
% 4. An open-source benchmark dataset of intersection routes.

The emergence of communication technologies such as Vehicle-to-Vehicle (V2V), Vehicle-to-Infrastructure (V2I), and Vehicle-to-Everything (V2X) is reshaping the automotive industry. As more CAVs (Connected Autonomous Vehicles) are rolled out, even with improvements to the agents that control the cars, there is no coordination among agents on the road. 

Modern autonomous vehicles incorporate a sophisticated array of local sensors, including Lidar, Radar, and IMUs, but these systems are still constrained. Lidar and Radar rely on line-of-sight and they cannot perceive objects obscured by a merging semi-truck or positioned around a blind corner. This limitation results in information asymmetry, as a vehicle may possess perfect data regarding its immediate three-meter vicinity but remain completely oblivious to a multi-vehicle collision fifty meters ahead. As already being proven, errors in local decision-making and sensor malfunctions introduce substantial risks where conflicting local decisions or erroneous sensor readings can precipitate accidents and fatalities. Even in cases with no sensor failures, it is possible for CAV’s today to not know what to do (https://missionlocal.org/2025/12/sf-waymo-halts-service-blackout/) 

To mitigate these risks, vehicles can establish communication with an external third party to create a centralized repository of information, which can then be disseminated to incoming vehicles. In this centralized approaches, vehicles transmit their collected data to a designated roadside unit, which then makes decisions. However, centralized approaches encounter several difficulties as the population of autonomous vehicles increases. They introduce scalability challenges, as all vehicles must transmit their data to a singular location. Furthermore, dependence on a single point renders the system susceptible to disruption by a single point of failure or malicious attacks, which could also compromise sensitive data from connected devices. Centralized devices are also not feasible due to the fact that they would need to be installed at every intersection or become the new cell tower. 

The Perception-Initiative-Consensus-Action (PICA) model describes a distributed decision-making framework for vehicles. In this scheme, each node makes an initial decision based on its local sensing and computing capabilities. The node then proposes its decision to the relevant group members, and the group uses a distributed consensus protocol to agree on the final course of action. Once consensus is reached, the action is performed.

Distributed computing typically uses some Crash-Fault-Tolerant (CFT) consensus algorithms, such as Paxos and Raft, to synchronize multiple nodes. However, these protocols operate under the assumption of a simple failure. They only assume that a node can crash, stop responding, or experience network delays, but always assume that the node is honest otherwise, and will not send malicious information or lie about its values. While CFT is perfectly fine for a controlled data center, in a V2V environment, it cannot be used. An intersection might have a range of automobiles that run their own ADAS stack, have their own encryption, etc. In this setting, failures are not just going to be crashes or a silent node. A vehicle might experience a hallucination due to environmental interference, or a bad actor might actively spoof network packets to hijack traffic flow, or a car may try to pass an intersection by lying about its arrival time. If a node maliciously broadcasts a message stating, "The intersection is clear, I am yielding," while actually speeding into the intersection, a CFT algorithm like Raft would implicitly trust the message, leading to a catastrophic collision, and allow them to go forward. 


\section{Motivation}
% Detail the failure modes of current decentralized systems like Virtual Traffic Lights (VTL) under adversarial conditions or sensor failure, proving the necessity of BFT.
% Introduce the socio-technical imperative for fairness. Explain why purely efficiency-driven throughput models are insufficient for modern urban environments, leading to minor-road starvation.
% Deeply analyze the "lane flushing" concept. Explain how emergency vehicles currently utilize hardware to preempt centralized traffic lights, and detail the mathematical and systemic complexity of replicating this absolute priority within a decentralized, asynchronous, leaderless BFT network without introducing denial-of-service vectors.

\section{Related Work}
% Categorize the literature review logically:
% 1. Decentralized Intersection Management: Review AIM, VTLs, and their limitations in mixed or adversarial traffic.
% 2. Consensus in Vehicular Networks: Critique the unsuitability of Proof-of-Work and Proof-of-Stake for ultra-low latency vehicular applications. Review the application of PBFT, HotStuff, and BFT-SMaRt to CAVs.
% 3. Fairness in Consensus: Contrast traditional cryptographic "order fairness" (preventing MEV/front-running) with the socio-physical priority required for traffic management.
% Clearly emphasize that no prior work successfully synthesizes BFT, spatial fairness utility, and secure emergency preemption simultaneously.

\section{System Model and Assumptions}
\subsection{Network and Communication Model}
% Define the underlying V2V communication layer assumptions, focusing on the 802.11p MAC/PHY layer characteristics simulated by OMNeT++.
% Define the synchrony bounds. Explicitly state the reliance on the Partial Synchrony model, which is the standard operational assumption for robust BFT protocols.
\subsection{Threat Model and Byzantine Faults}
% Define the required node ratio $N \geq 3f+1$. 
% Detail the specific capabilities of a Byzantine vehicle in this context: spoofing geographic locations, dropping consensus messages, or broadcasting fabricated emergency preemption claims.
\subsection{Kinematic and Intersection Grid Model}
% Mathematically define the physical intersection as a finite grid of mutually exclusive conflict zones. Define the parameters of acceptable vehicle trajectories and collision thresholds.

\section{The Proposed Protocol}
\subsubsection{Pre-Consensus: Per-Car Arrival Certification}

Before the BFT consensus round begins, each vehicle must earn an arrival certificate — a bundle of $f+1$ independent physical signatures from honest replicas. This is the core security mechanism that blocks Byzantine lane-lying attacks before the proposal ever reaches consensus.

\begin{itemize}
    \item \textbf{Phase 1 — ARRIVAL\_ANNOUNCE.} When a vehicle enters the intersection zone, it serializes its claimed state (lane, position in lane, intended turn direction, ambulance flag) and broadcasts an \texttt{ARRIVAL\_ANNOUNCE} message to all replicas.
    \item \textbf{Phase 2 — ARRIVAL\_ECHO.} Each replica that receives an \texttt{ARRIVAL\_ANNOUNCE} independently queries the TraCI physical layer to verify whether the vehicle is actually in the claimed lane. Three outcomes are possible:
    \begin{itemize}
        \item \textbf{Match (SIGNED path):} the TraCI position agrees with the claim. The replica signs the canonical string $(\texttt{carId}\|\texttt{lane}\|\texttt{pos}\|\texttt{dir}\|\texttt{isAmb}\|\texttt{replicaId})$ with its ECDSA P-256 private key (SHA-256withECDSA) and unicasts the resulting signature together with its 65-byte uncompressed P-256 public key back to the announcing vehicle as an \texttt{ARRIVAL\_ECHO}.
        \item \textbf{Mismatch (WRONG\_LANE):} the vehicle is physically present but lying about its lane. The replica records the vehicle with its true TraCI-observed lane in \texttt{physicallyObservedCars} but sends no echo, so the lying car accumulates zero valid signatures.
        \item \textbf{Absent (NO\_VEHICLE):} the message is silently dropped.
    \end{itemize}
    \item \textbf{Phase 3 — ARRIVAL\_CERT.} Once a vehicle accumulates $f+1$ distinct echo signatures (where $f = \lfloor (n-1)/3 \rfloor$ for $n$ vehicles in the epoch batch), it assembles them into an \texttt{ARRIVAL\_CERT} and broadcasts it. Because OMNeT++ modules do not receive their own channel broadcasts, the sender immediately self-stores the cert after broadcasting.
    \item \textbf{Phase 4 — Cert collection and timeout.} The leader (replica with minimum ID among \texttt{physicallyObservedCars}) starts a cert-collection timer on its first vehicle observation. The timer always fires even if a Byzantine vehicle never passes the physical check, since the expected count is always the fixed \texttt{BATCH\_SIZE}, not \texttt{physicallyObservedCars.size()}. If all \texttt{BATCH\_SIZE} certs arrive before the timer expires, the leader cancels the timer and submits immediately. On timeout the leader force-submits, marking any uncertified vehicle as \texttt{QUIET}.
\end{itemize}

\noindent \textbf{Why this blocks all arrival-layer Byzantine attacks.} A Byzantine vehicle that lies about its lane will fail the TraCI check at every honest replica, receiving zero echoes and therefore never producing a valid \texttt{ARRIVAL\_CERT}. It is included in the proposal as \texttt{QUIET} with its true TraCI-observed lane. A Byzantine vehicle that sends contradictory announcements (equivocation) can collect at most $f$ consistent echoes from replicas that saw the same version, which is insufficient.

% ─── Algorithm: ArrivalEchoDecide ─────────────────────────────────────────
\begin{algorithm}[H]
\caption{\textsc{ArrivalEchoDecide}$(v,\ replicas)$}
\begin{algorithmic}[1]
\State $v$ broadcasts \textsc{Arrival\_Announce}$(carId, lane, pos, dir, isAmb)$ to all replicas
\For{each replica $r$ that receives \textsc{Arrival\_Announce} from $v$}
    \State $result \gets \text{TraCI.verifyCarPosition}(v.carId, v.lane, v.pos)$
    \State record $v$ in $physicallyObservedCars$ with $result.actualLane$
    \If{$result = \text{MATCH}$}
        \State $m \gets \text{SHA-256}(v.carId \| v.lane \| v.pos \| v.dir \| v.isAmb \| r.id)$
        \State $\sigma_r \gets \text{ECDSA-P256.Sign}(sk_r,\; m)$
        \State send \textsc{Arrival\_Echo}$(v.carId,\; r.id,\; pk_r,\; \sigma_r)$ to $v$
    \ElsIf{$result = \text{WRONG\_LANE}$}
        \State \textbf{// record actual lane; send no echo}
    \EndIf
\EndFor
\State $v$ collects echoes; \textbf{when} $|echoes| \ge f+1$ (distinct $replicaIds$):
\State \quad broadcast \textsc{Arrival\_Cert}$(v.carId, echoes)$
\State \quad self-store cert in $collectedCerts$
\State Leader starts $certCollectionTimer$ on first observation
\State \textbf{On} $certCollectionTimer$ expiry or $|collectedCerts| = \text{BATCH\_SIZE}$:
\State \quad vehicles without a cert $\gets \text{QUIET}$
\State \quad invoke \textsc{SubmitViewToBFTConsensus}(collectedCerts)
\end{algorithmic}
\end{algorithm}
\subsubsection{Consensus Proposal and Follower Validation}

Once the leader has collected certs (or timed out), it calls \textsc{BuildProposal}() to compute the deterministic intersection schedule, then invokes a single BFT-SMaRt consensus round with the full payload:

\begin{center}
\texttt{PROPOSE\_ALL:<proposerId>:<vehicleStates>:<perCarCerts>:<orderBag>}
\end{center}

Before any follower sends its WRITE vote, \texttt{OrderRequestVerifier.isValidRequest()} executes a deterministic re-execution check to ensure the leader's proposal is valid. Because the \textsc{BuildProposal} algorithm is strictly deterministic, followers only need to verify the cryptographically signed inputs and then re-run the algorithm locally.

% ─── Algorithm: IsValidRequest ────────────────────────────────────────────
\begin{algorithm}[H]
\caption{\textsc{IsValidRequest}$(proposal,\ f,\ epoch,\ waitRegistry)$}
\begin{algorithmic}[1]

\Statex \textbf{// Step 1: Validate Per-Car Certificates}
\For{each SIGNED vehicle $v$ in $proposal.vehicleStates$}
    \State $validEchos \gets 0$
    \For{each $echo = (replicaId,\; pk,\; \sigma)$ in $proposal.perCarCerts[v.carId]$}
        \State $m \gets \text{SHA-256}(v.carId \| v.lane \| v.pos \| v.dir \| v.isAmb \| echo.replicaId)$
        \If{$\text{ECDSA-P256.Verify}(echo.pk,\; m,\; echo.\sigma) = \mathsf{true}$}
            \State $validEchos \gets validEchos + 1$
        \EndIf
    \EndFor
    
    \If{$validEchos < f + 1$}
        \State \Return \textbf{false} \Comment{Invalid arrival certificate}
    \EndIf
\EndFor

\Statex \textbf{// Step 2: Deterministic Re-execution}
\State $expectedSchedule \gets \textsc{BuildProposal}(proposal.vehicleStates,\ epoch,\ waitRegistry)$

\Statex \textbf{// Step 3: Compare against Leader's proposed schedule}
\If{$expectedSchedule \neq proposal.orderBag$}
    \State \Return \textbf{false} \Comment{Leader generated an invalid or malicious schedule}
\EndIf

\State \Return \textbf{true}

\end{algorithmic}
\end{algorithm}

If \textsc{IsValidRequest} returns \textbf{false}, the follower rejects the proposal, which triggers a BFT-SMaRt leader change.
% ─── Helper: IsSameLaneFrontPlaced ────────────────────────────────────────
\begin{algorithm}[H]
\caption{\textsc{IsSameLaneFrontPlaced}$(v,\ view,\ placed)$}
\begin{algorithmic}[1]
\Statex \textbf{// Returns true iff every vehicle in $v$'s lane that is ahead of $v$ in the queue}
\Statex \textbf{// has already been placed in the schedule.}
\For{each $u \in view$ s.t. $u.lane = v.lane$}
    \If{$\text{compareLaneQueueOrder}(u, v) < 0 \land u \notin placed$}
        \State \Return \textbf{false}
    \EndIf
\EndFor
\State \Return \textbf{true}
\end{algorithmic}
\end{algorithm}

% ─── Helper: isSafeToBatch ────────────────────────────────────────────────
\begin{algorithm}[H]
\caption{\textsc{isSafeToBatch}$(a,\ b)$}
\begin{algorithmic}[1]
\If{$a.lane = b.lane$}
    \State \Return \textbf{false} \Comment{rear-end risk}
\EndIf
\State $keyA \gets a.lane + \text{dirCode}(a.direction)$
\State $keyB \gets b.lane + \text{dirCode}(b.direction)$
\State \Return $\text{SAFE\_SET.contains}(\{keyA, keyB\})$
\end{algorithmic}
\end{algorithm}

% ─── Main Algorithm: BuildProposal ────────────────────────────────────────
\begin{algorithm}[H]
\caption{\textsc{BuildProposal}$(view,\ epoch,\ waitRegistry)$}
\begin{algorithmic}[1]

\State $schedule \gets []$
\State $placed \gets \emptyset$

\Statex \textbf{// Step 1: Identify priority work queue}
\State $ambulances \gets \{ v \in view \mid v.isAmbulance \}$
\State Sort $ambulances$ by ($positionInLane$ asc, $vehicleIdNumeric$ asc)

\State $workQueue \gets []$
\For{each ambulance $a \in ambulances$}
    \State $blockers \gets \{ v \in view \mid v.lane = a.lane \land \neg v.isAmbulance \land \text{compareLaneQueueOrder}(v, a) < 0 \}$
    \State Sort $blockers$ by ($positionInLane$ asc, $vehicleIdNumeric$ asc)
    \State append $blockers \to workQueue$ \Comment{blockers must cross before ambulance}
    \State append $a \to workQueue$
\EndFor

\Statex \textbf{// Step 2: Normal pool}
\State $remaining \gets \{ v \in view \mid v \notin workQueue \}$
\State Sort $remaining$ by ($-waitRegistry[v]$ desc, $positionInLane$ asc, $vehicleIdNumeric$ asc)
\State append $remaining \to workQueue$

\Statex \textbf{// Step 3: Greedy batch packing}
\While{$placed.size() < view.size()$}
    \State $head \gets \text{first } v \in workQueue \text{ s.t. } v \notin placed \land \textsc{IsSameLaneFrontPlaced}(v, view, placed)$
    \If{$head = \text{null}$}
        \State \textbf{break} \Comment{should not happen in valid input}
    \EndIf

    \State $batch \gets \{ head \}$
    \State $placed \gets placed \cup \{ head \}$

    \If{$head.cyberStatus = \text{QUIET}$}
        \State \textbf{// QUIET vehicle: exclusive singleton batch (Leader Rejection Rule)}
    \Else
        \State \textbf{// Greedy expand: add non-conflicting SIGNED vehicles}
        \Repeat
            \State $grew \gets \textbf{false}$
            \For{each $candidate \in workQueue$ s.t. $candidate \notin placed \land \textsc{IsSameLaneFrontPlaced}(candidate, view, placed) \land candidate.cyberStatus \neq \text{QUIET}$}
                \If{$\forall e \in batch: \textsc{isSafeToBatch}(e, candidate)$}
                    \State $batch \gets batch \cup \{ candidate \}$
                    \State $placed \gets placed \cup \{ candidate \}$
                    \State $grew \gets \textbf{true}$
                \EndIf
            \EndFor
        \Until{$\neg grew$}
    \EndIf
    
    \State append $batch \to schedule$
\EndWhile

\State \Return $schedule$

\end{algorithmic}
\end{algorithm}
\subsection{Consensus Core (Adapted from BFT-SMaRt)}
\label{subsec:consensus-core}

We adopt BFT-SMaRt~\cite{bessani2014state} as the underlying BFT engine with no changes to its safety-critical consensus logic. Normal-case consensus proceeds in three communication steps:

\begin{enumerate}
\item \textbf{PROPOSE.} The leader's \texttt{TOMLayer} batches the arriving PROPOSE\_ALL \texttt{TOMMessage} and broadcasts a PROPOSE to all replicas. In our system, this message is delivered to the leader itself via self-injection (zero latency, no radio) and to all followers via the type-9 V2V broadcast path. Because BFT-SMaRt's \texttt{OrderRequestVerifier} sits on the follower's receive path, the proposal is validated before any WRITE vote is cast.

\item \textbf{WRITE.} Each replica that receives a PROPOSE executes the eight-check validator (\texttt{OrderRequestVerifier.isValidRequest()}, described above). If all checks pass, it broadcasts a signed WRITE message. A replica waits for $2f+1$ WRITE messages before advancing.

\item \textbf{ACCEPT.} After collecting $2f+1$ WRITE messages, each replica broadcasts an ACCEPT. After collecting $2f+1$ ACCEPT messages, the replica decides: \texttt{IntersectionServer.appExecuteBatch()} is invoked, and the ORDER decision is delivered to C++ via \texttt{notifyOrderDecided()}.
\end{enumerate}

All inter-replica consensus messages (WRITE, ACCEPT, COMMIT) are routed through \texttt{V2VServersCommunicationLayer}, the drop-in replacement for BFT-SMaRt's TCP/SSL \texttt{ServersCommunicationLayer}, described in Section~\ref{subsec:v2v-layer}. The quorum thresholds, message formats, and state machine are unchanged from stock BFT-SMaRt.

\paragraph{Batch timeout.}
\texttt{TOMLayer} waits up to \texttt{batchTimeout}~=~100\,ms (sim time) for a batch to fill before forcing a proposal. The timeout is evaluated against \texttt{SimulationClock.currentTimeMillis()} rather than \texttt{System.currentTimeMillis()} to remain meaningful under variable simulation speeds.

\paragraph{Authentication.}
BFT-SMaRt originally authenticates inter-replica messages with HMAC-MD5 MACs. In the V2V model, all messages arrive via the 802.11p physical channel from known replica addresses; \texttt{V2VServersCommunicationLayer.deliverToBFTSmart()} marks messages \texttt{authenticated~=~true} on receipt, bypassing the MAC check. Byzantine replicas are handled by the $3f+1$ quorum thresholds and the \texttt{OrderRequestVerifier} application-layer checks rather than transport-level MACs.

\subsection{Byzantine Leader Change}
\label{subsec:lc}

Leader change is triggered when a replica's \texttt{RequestsTimer} detects no PROPOSE\_ALL progress within a sim-time window ($\mathtt{STOP\_RETX\_SIM\_MS} = 200\,\text{ms sim}$). Detection uses a dual-timer: a wall-clock Java timer polls every 200\,ms, gated by a comparison against \texttt{SimulationClock.currentTimeMillis()} so that timeout intervals remain reproducible across variable simulation speeds.

\paragraph{Phase 1 — STOP quorum.}
Each replica broadcasts a STOP message carrying its current regency number and local consensus state. Exactly $2f+1$ distinct-sender STOP messages are required before any replica may advance; this threshold is unchanged from standard BFT-SMaRt. To complete the quorum under 802.11p broadcast loss without blind flooding — which raises collision probability superlinearly in dense regimes — we adopt a NACK-based repair scheme. After a short blind burst ($\mathtt{STOP\_BLIND\_EMITS}$ transmissions to seed the network), replicas broadcast a compact bitmask of missing senders; any peer that sees its bit set resends its original signed STOP. A per-peer reply cap ($\mathtt{NACK\_REPLIES\_PER\_PEER}$ per regency) prevents Byzantine NACK flooding from monopolizing the channel. STOP\_NACK is a \emph{transport hint only}: it cannot fabricate quorum membership, as every STOP message must still pass cryptographic validation before being counted.

\paragraph{Phase 2 — STOPDATA to new leader.}
Each replica ships its local consensus state to the new leader. Because STOPDATA payloads may be retransmitted for liveness under loss, they are classified as \emph{unordered} on both the physical broadcast path and the logical unicast path to the leader. Imposing a strictly-increasing sequence contract on self-repaired traffic creates sequence-gap stalls: the leader buffers later copies while waiting for a lost predecessor that will never arrive. Unordered delivery is combined with logical destination filtering so non-target replicas do not corrupt their own sequence state.

\paragraph{Phase 3 — SYNC.}
Once the new leader collects sufficient STOPDATA, it broadcasts a SYNC message finalizing the new view. A per-regency ``SYNC already sent'' guard prevents re-entry when late STOPDATA copies arrive after SYNC has already been emitted.

\paragraph{Escalation idempotence.}
\texttt{tryClaimLCEpoch()} (synchronized) ensures that concurrent timer firings and BFT anomaly signals do not spawn multiple escalations for the same logical LC episode. \texttt{dropRegencyState()} (synchronized) resets the epoch claim when SYNC installs the new regency, so future episodes start cleanly.

\paragraph{Fast re-proposal: f+1 signatures enable immediate propose after leader eviction.}
This is a core property of the protocol design. Every \texttt{ARRIVAL\_CERT} carries $f+1$ independent ECDSA P-256 echo signatures from distinct honest replicas, each signature covering $(\mathtt{carId}\|\mathtt{lane}\|\mathtt{pos}\|\mathtt{dir}\|\mathtt{isAmb}\|\mathtt{replicaId})$ with the signer's full public key self-contained in the cert. Because the cert's validity is therefore cryptographically established \emph{without} the leader, a new leader installed after Byzantine predecessor eviction does \textbf{not} run a new ARRIVAL\_ANNOUNCE~$\to$~ARRIVAL\_ECHO collection round. Instead, it calls \texttt{getFreshProposePayload()} via JNI to fetch the already-validated certs from C++ \texttt{collectedCerts} — including any cars the Byzantine predecessor censored from its proposal — and immediately submits a PROPOSE\_ALL. EP5 (Termination) therefore completes in one additional BFT round rather than one full cert-collection interval plus one BFT round.

\subsection{Fairness and Priority Mechanism}
% Formally define the mathematical utility function for fairness, explicitly incorporating accumulated vehicle wait time and predefined vehicle class priority weights.
% Explain the algorithmic process by which the BFT leader proposes a batch of trajectories that demonstrably maximizes this fairness metric.
\subsection{Emergency Lane Flushing Mechanism}
% Detail the preemptive interrupt protocol sequentially.
% 1. The emergency vehicle broadcasts a cryptographically signed Priority-Interrupt message.
% 2. The $3f+1$ replicas verify the cryptographic signature to prevent malicious DoS attacks.
% 3. The current consensus epoch is halted via a forced view-change, and a specific, pre-computed "flush" trajectory set is forced through consensus to decelerate conflicting traffic.
\subsection{Safety and Liveness Guarantees}
% Provide rigorous argumentation showing that the lane flushing interrupt and forced view-change do not violate the core safety or liveness properties of the underlying BFT state machine. Defer complex, multi-page mathematical proofs to the appendix to respect the 15-page limit.

\section{Formal Property Verification}                                                                                          
                                                                                                                                  
  To demonstrate the rigor of the proposed protocol, we map the fundamental properties of Byzantine Fault Tolerance to our V2X    
environment. Unlike traditional BFT, which assumes a purely digital input, our protocol relies on a \textit{Physical-to-Digital   
Anchor} via the TraCI layer.                                                                                                      
                                                                                                                                  
  \subsection{Property 1: Validity (Sensor-Anchored Validity)}                                                                    
  A decided schedule $S$ is valid if it is composed of vehicle states that were either proposed by a correct leader or verified by
the physical layer.                                                                                                               
  \begin{itemize}                                                                                                                 
      \item \textbf{Unbound Case:} If the epoch is new, the decided schedule is a product of the deterministic                    
\textsc{BuildProposal} algorithm acting upon the current $v.states$.                                                              
      \item \textbf{Bound Case:} If the state is carried from a previous epoch (e.g., a vehicle's trailing state), the integrity  
is maintained by the $f+1$ signature threshold, ensuring the state was physically verified by a quorum of replicas in the previous
round.                                                                                                                            
  \end{itemize}                                                                                                                   
                                                                                                                                  
  \subsection{Property 2: Agreement (Quorum Intersection)}                                                                        
  No two correct processes (replicas) decide on a different intersection schedule for the same epoch.                             
  \begin{itemize}                                                                                                                 
      \item Since a decision requires a quorum of $2f+1$ \texttt{Accept} messages, and any two quorums of size $2f+1$ in a system 
of $3f+1$ nodes must intersect in at least one correct process, the deterministic nature of \textsc{BuildProposal} ensures that   
the resulting schedules are identical across all non-faulty nodes.                                                                
  \end{itemize}                                                                                                                   
                                                                                                                                  
  \subsection{Property 3: Integrity (Single-Decision Constraint)}                                                                 
  Every correct process decides on an arrival schedule at most once per epoch.                                                    
  \item Before a replica enters the \texttt{DECIDED} state, the local \texttt{accepted\_array} is nullified. Given the            
deterministic nature of the \textsc{ArrivalCert} collection, a vehicle cannot successfully re-generate a valid certificate once   
its state is finalized, preventing redundant or conflicting state transitions.                                                    
  \end{itemize}                                                                                                                   
                                                                                                                                  
  \subsection{Property 4: Lock-in (Epoch Persistence)}                                                                            
  If a correct process decides on a vehicle's trajectory in epoch $E$, no correct process will decide on a different trajectory   
for that vehicle in epoch $E+1$.                                                                                                  
  \begin{itemize}                                                                                                                 
      \item Because the $f+1$ threshold for \texttt{ARRIVAL\_CERT} requires physical verification from a majority of sensors, any 
attempt to change a vehicle's state in a subsequent epoch would require the vehicle to re-pass the TraCI physical check. The      
intersection of the $q_1$ quorum (current epoch) and $q_2$ (next epoch) ensures that the "truth" of the vehicle's position is     
preserved.                                                                                                                        
  \end{itemize}                                                                                                                   
                                                                                                                                  
  \subsection{Property 5: Termination (Liveness via Timer)}                                                                       
  If the leader is correct and the physical layer is operational, every correct process will eventually decide on a schedule.     
  \begin{itemize}                                                                                                                 
      \item The \texttt{certCollectionTimer} ensures liveness by preventing the protocol from hanging due to a Byzantine or       
"Quiet" vehicle. Even if a vehicle fails to accumulate $f+1$ signatures, the leader forces a submission, ensuring the system makes
progress regardless of individual vehicle failures.                                                                               
  \end{itemize}                                                                                                                   
                                                                                                                                  
  \subsection{Property 6: Abort Behavior (Protocol Resilience)}                                                                   
  If a correct process requests an \texttt{ABORT} (e.g., due to a detected physical inconsistency), it will eventually receive an 
\texttt{ABORTED} response.                                                                                                        
  \begin{itemize}                                                                                                                 
      \item A correct process receives an \texttt{ABORT} only if a quorum of replicas identifies a fundamental violation in the   
physical-to-digital mapping (e.g., an invalid \texttt{ARRIVAL\_CERT}), ensuring the system can revert to a safe state if the      
consensus is compromised.                                                                                                         
  \end{itemize}            

\section{Architectural Framework and Simulation Mechanics}
\label{sec:arch}

\subsection{Unified-Vehicle Architecture and V2V Communication}
\label{subsec:unified-car}

Each simulated vehicle is a \emph{unified node} that co-locates two subsystems within a single OMNeT++ process: a C++ layer running the Veins \texttt{V2VProxyModule} and supporting components, and a Java layer running the BFT-SMaRt \texttt{IntersectionServer} replica. The two layers communicate via a JNI bridge, providing zero simulated latency and zero channel load for intra-vehicle calls (time synchronization, cert snapshot retrieval, order decision delivery, and wipe notifications).

\begin{figure}[t]
\centering
\begin{tikzpicture}[
  every node/.style={font=\small},
  box/.style={draw, rounded corners=2pt, minimum width=2.6cm, minimum height=0.65cm, align=center, fill=white},
  vbox/.style={draw, thick, rounded corners=5pt, inner sep=8pt, fill=gray!5},
  jni/.style={<->, dashed, semithick},
  v2v/.style={<->, semithick, >=Stealth}
]
%% Vehicle i (left)
\node[box] (cppi)  at (0, 1.1)  {C++ Layer\\[-1pt]{\scriptsize V2VProxyModule}};
\node[box] (javai) at (3.3, 1.1){Java Layer\\[-1pt]{\scriptsize BFT-SMaRt}};
\node[box] (radi)  at (0, 0)    {802.11p NIC};
\draw[jni]          (cppi)       -- node[above,font=\scriptsize]{JNI} (javai);
\draw[semithick,->] (cppi.south) --                                   (radi.north);
\node[vbox, fit=(cppi)(javai)(radi),
      label={[font=\small\bfseries]above:Vehicle $i$}] {};
%% Vehicle j (right)
\node[box] (cppj)  at (8.0, 1.1) {C++ Layer\\[-1pt]{\scriptsize V2VProxyModule}};
\node[box] (javaj) at (11.3, 1.1){Java Layer\\[-1pt]{\scriptsize BFT-SMaRt}};
\node[box] (radj)  at (8.0, 0)   {802.11p NIC};
\draw[jni]          (cppj)       -- node[above,font=\scriptsize]{JNI} (javaj);
\draw[semithick,->] (cppj.south) --                                   (radj.north);
\node[vbox, fit=(cppj)(javaj)(radj),
      label={[font=\small\bfseries]above:Vehicle $j$}] {};
%% V2V broadcast channel
\draw[v2v] ([xshift=2pt]radi.east)
        -- node[below=4pt,font=\small]{802.11p broadcast channel}
           ([xshift=-2pt]radj.west);
\end{tikzpicture}
\caption{Unified vehicle architecture. Each simulated vehicle co-locates a C++ OMNeT++/Veins module and a Java BFT-SMaRt replica bound by JNI (zero simulated latency, zero channel load). \emph{All} inter-vehicle communication — BFT consensus messages (PROPOSE, WRITE, ACCEPT, COMMIT), leader-change messages (STOP, STOPDATA, SYNC), and application-layer messages (ARRIVAL\_ANNOUNCE, ARRIVAL\_ECHO, ARRIVAL\_CERT, PROPOSE\_ALL, EXECUTING) — traverses the shared 802.11p broadcast channel.}
\label{fig:unified-car}
\end{figure}

The central architectural invariant is: \textbf{all inter-vehicle communication traverses the 802.11p radio channel.} No TCP, shared memory, or direct inter-process communication exists between vehicles. This required replacing BFT-SMaRt's native TCP/Netty replica transport with a V2V communication layer, and routing PROPOSE\_ALL delivery — previously sent via \texttt{ServiceProxy} over TCP loopback, which modeled zero radio latency — through the 802.11p channel so that PROPOSE\_ALL delivery experiences realistic propagation delay, channel contention, and packet loss.

\paragraph{JNI interface summary.}
Outbound C++$\to$Java calls include: \texttt{triggerJoinForReplica} (hands PROPOSE\_ALL payload to the Java leader), \texttt{SimulationClock.updateTime} (syncs OMNeT++ sim time into Java every $\sim$20\,ms wall), \texttt{globalResetV2V} (notifies Java of departed replicas at epoch boundaries), and \texttt{deliverInjectedClientRequest} (delivers a type-9 V2V frame carrying the leader's serialized TOMMessage to a follower's TOMLayer). Inbound Java$\to$C++ native calls include: \texttt{notifyOrderDecided} (delivers the ORDER decision; C++ resumes vehicle movement), \texttt{notifyWipeComplete} (epoch wipe done; C++ re-triggers ARRIVAL\_ANNOUNCE), \texttt{nativeGetCertSnapshot} (pulls \texttt{collectedCerts} key set into Java for Check~7 censor guard), \texttt{nativeGetFreshProposePayload} (pulls fresh \texttt{vehicleStatesStr:perCarCerts} for the EP5 leader-change rebuild path), and \texttt{nativeBroadcastClientRequest} (leader broadcasts serialized TOMMessage via 802.11p).

\paragraph{Simulation-time synchronization.}
OMNeT++ sim time is the authoritative clock for all protocol timeouts. A C++ \texttt{retxCheckTimer} fires every $\sim$20\,ms wall-time and calls \texttt{SimulationClock.updateTime(simTime().dbl())} via JNI, updating a \texttt{volatile long} in the Java \texttt{SimulationClock} class. All Java timeout-gated logic — the \texttt{RequestsTimer} STOP gate, LC debounce, TOMLayer batch timeout, and ClientsManager request timestamps — compares against \texttt{SimulationClock.currentTimeMillis()}. Wall-clock calls (\texttt{System.currentTimeMillis()}) are restricted to off-critical-path performance instrumentation only.

\subsection{V2V Communication Layer for Java}
\label{subsec:v2v-layer}

\texttt{V2VServersCommunicationLayer} implements BFT-SMaRt's \texttt{ServersCommunicationLayerInterface} and replaces the original TCP/SSL \texttt{ServersCommunicationLayer} entirely. It is instantiated once per replica at startup and is the sole path for all inter-replica BFT message exchange. The substitution is transparent to BFT-SMaRt's consensus engine; no changes are made above the communication layer interface.

\paragraph{Send path.}
BFT-SMaRt calls \texttt{send(int[] targets, SystemMessage sm, ...)} for every outbound consensus message. The layer inspects the target set:
\begin{itemize}
  \item \textbf{Self}: delivered directly to the BFT-SMaRt \texttt{inQueue} via \texttt{deliverToBFTSmart()} with zero radio involvement.
  \item \textbf{Multiple remote targets}: \texttt{ReliableV2VMessaging.sendMulticast()} emits one physical broadcast frame carrying a single broadcast sequence number shared across all $N-1$ logical recipients.
  \item \textbf{Single remote target}: \texttt{ReliableV2VMessaging.sendReliable()} emits a unicast frame with a per-target sequence number.
\end{itemize}
In both remote cases the serialized \texttt{V2VMessageEnvelope} reaches \texttt{V2VNativeBridge.sendMessage()}, which calls the native method \texttt{nativeSendMessage(fromId, toId, bytes)} via JNI into C++. C++ enqueues the frame as a \texttt{PendingMessage} and emits it via \texttt{sendBFTMessage()} $\to$ \texttt{sendDelayed()} onto the 802.11p channel.

\paragraph{Receive path.}
When a type-0 V2V frame arrives at a replica's C++ module, \texttt{handlepreConsensusMessages()} hands the raw bytes to \texttt{V2VNativeBridge.deliverMessage(fromId, bytes)} via JNI. The bridge invokes its registered \texttt{MessageReceiverCallback}, which deserializes the \texttt{V2VMessageEnvelope} and passes it to \texttt{ReliableV2VMessaging.handleIncomingMessage()}. After reliability processing the unwrapped \texttt{SystemMessage} is placed on BFT-SMaRt's \texttt{inQueue}, where the \texttt{MessageHandler} thread consumes it.

\paragraph{Embedded-mode JNI.}
When the JVM is created by OMNeT++ (system property \texttt{bftsmart.jni.embedded=true}), the native library \texttt{libv2vjni.so} is already mapped into the process by the C++ loader. \texttt{V2VNativeBridge} detects this flag at class-load time and skips \texttt{System.loadLibrary()}, preventing a redundant load.

\subsection{Reliable Messaging over V2V}
\label{subsec:reliability}

The 802.11p broadcast medium provides no delivery guarantees: frames may be lost to collision, interference, or congestion. \texttt{ReliableV2VMessaging} wraps every BFT consensus message in a \texttt{V2VMessageEnvelope} and implements reliable, ordered point-to-point channels over this lossy substrate.

\paragraph{Sequence numbering and envelope metadata.}
Each sender maintains a monotonically increasing sequence counter per remote target (unicast path) and a single broadcast counter (multicast path). Broadcast sequence numbers have bit 62 set (\texttt{BROADCAST\_SEQ\_FLAG = 1L << 62}), forming a disjoint namespace from unicast sequences and allowing the receiver to classify an envelope's ordering path from the sequence number alone. The envelope also carries \texttt{fromReplicaId}, \texttt{toReplicaId} (for unicast destination filtering), \texttt{isBroadcast}, \texttt{isUnordered}, a sim-time \texttt{timestampMs}, and a \texttt{currentTimeout} for exponential-backoff retransmission.

\paragraph{Ordered receive.}
\texttt{handleIncomingMessage()} enforces strict ordered delivery per sender:
\begin{itemize}
  \item \emph{In-order} (seq $=$ expected): deliver immediately to the \texttt{inQueue}, advance the expected counter, and drain any consecutive frames from the receive buffer.
  \item \emph{Out-of-order} (seq $>$ expected): buffer in a min-heap \texttt{PriorityQueue} ordered by sequence number.
  \item \emph{Duplicate} (seq $<$ expected): drop silently.
\end{itemize}
Logical destination filtering is applied before any sequence tracking: unicast envelopes addressed to a different replica are dropped without updating the local expected-sequence counter. This is necessary because the physical 802.11p layer delivers every frame to every in-range node; without filtering, a unicast to replica $j$ would advance replica $k$'s expected counter, causing $k$ to misclassify its own future unicasts as duplicates.

\paragraph{ACK piggybacking.}
ACKs are not sent as independent frames. Received sequence numbers accumulate in a per-peer \texttt{pendingAcks} set and are attached to the next outgoing DATA envelope as \texttt{piggybackedAcks} (unicast) or \texttt{broadcastAcks} (multicast, tagged by original sender ID). On receipt of a piggybacked ACK, the sender removes the corresponding entry from \texttt{unackedMessages}, halting retransmission for that message.

\paragraph{Sim-time-gated retransmission.}
Java's \texttt{ScheduledExecutorService} is disabled; C++'s \texttt{retxCheckTimer} (fired on sim-time intervals) calls \texttt{checkRetransmissionsForReplica(id)}, ensuring retransmission cadence is reproducible across simulation speeds. Per-message timeout begins at 30\,ms sim-time and doubles on each miss, capped at 250\,ms (\texttt{RETX\_MAX\_BACKOFF\_MS}). After 40 attempts (\texttt{MAX\_RETX\_ATTEMPTS}) a message is dropped. Broadcast retransmission is deduplicated: a single physical re-emission per broadcast sequence number is queued regardless of how many per-target ACK timeouts fire simultaneously.

\paragraph{Unordered path for leader-change messages.}
STOP, STOP\_NACK, and STOPDATA are classified as \emph{unordered} (\texttt{isUnordered = true}). They bypass sender-side retransmission tracking and receiver-side sequence ordering entirely. This is required because LC messages are intentionally retransmitted at the application layer; placing them in the ordered sequence space would cause each retransmission to consume a new sequence slot, and a lost copy would stall all subsequent ordered traffic from the same sender indefinitely. LC idempotence is guaranteed at the BFT-SMaRt layer by regency gating and per-regency distinct-sender sets, so application-layer deduplication makes reliability-layer ordering redundant.

\paragraph{Epoch boundary and departure handling.}
At epoch boundaries, \texttt{epochBoundaryCleanupForReplica()} clears the departing replica's outgoing unacked state and advance-only synchronizes all remaining receivers' expected sequence numbers to the sender's current broadcast index, unblocking any sequence gaps left by interrupted in-flight messages. When a replica departs mid-epoch, \texttt{removeInstance()} purges its reliability state and \texttt{clearUnackedToReplica()} stops all retransmission directed at it. Critically, \texttt{expectedSeqNums} and \texttt{expectedBroadcastSeqNums} for a departed sender are \emph{not} reset: resetting them would cause any late-arriving in-flight frame to be misidentified as seq~=~0 and delivered out of order, corrupting consensus state at followers still receiving trailing messages.

\subsection{Benchmark Dataset Generation}
% Describe the rigorous creation of the SUMO route sets (.rou.xml). Detail the synthetic Origin-Destination matrices, the varying traffic densities, and the specific injection patterns for emergency vehicles and Byzantine actors.

\section{Evaluation and Results}
\subsection{Consensus Latency and Network Throughput}
% Present structured data on baseline latency. How does the protocol perform under high packet-loss environments simulated by OMNeT++?
\subsection{Traffic Efficiency and Kinematic Fairness}
% Evaluate the system using Jain's Fairness Index. Provide a comparative analysis between a standard FCFS BFT-AIM implementation and the proposed Fair-BFT-AIM.
\subsection{Emergency Vehicle Response Metrics}
% Present precise data on the time taken to achieve consensus on a lane flush and the subsequent reduction in emergency vehicle traverse time compared to non-preemptive baselines.

\section{Conclusion}
% Synthesize the impact of merging cryptographic consensus with physical fairness. Outline avenues for future work, such as scaling the protocol from a single isolated intersection to a multi-intersection urban corridor.

\appendix
\section{Formal Proofs of Safety and Liveness}
% Defer all exhaustive mathematical proofs regarding the emergency view-change mechanism here. This ensures the core narrative respects the 15-page guideline while satisfying the rigorous theoretical expectations of the DISC program committee.

\end{document}


  Revised EP Claim Table (Combined A+B)

  ┌─────────────────┬──────────────────────────┬───────────────────────────────────────────────────┐
  │       EP        │          Status          │                       Notes                       │
  ├─────────────────┼──────────────────────────┼───────────────────────────────────────────────────┤
  │ EP1 Validity    │ Formal — f+1 necessary   │ Cert = f+1 TraCI verifications                    │
  ├─────────────────┼──────────────────────────┼───────────────────────────────────────────────────┤
  │ EP2 Agreement   │ Formal — f+1 necessary   │ Equivocator-Cannot-Dual-Certify                   │
  ├─────────────────┼──────────────────────────┼───────────────────────────────────────────────────┤
  │ EP3 Integrity   │ BFT-SMaRt handles        │ Unchanged                                         │
  ├─────────────────┼──────────────────────────┼───────────────────────────────────────────────────┤
  │ EP4 Lock-in     │ BFT-SMaRt handles        │ Unchanged                                         │
  ├─────────────────┼──────────────────────────┼───────────────────────────────────────────────────┤
  │ EP5 Termination │ Intra-epoch, under H1–H6 │ H6 (waitRegistry synchrony) is the new obligation │
  ├─────────────────┼──────────────────────────┼───────────────────────────────────────────────────┤
  │ EP6 Abort       │ Unchanged                │ —                                                 │
  └─────────────────┴──────────────────────────┴───────────────────────────────────────────────────┘

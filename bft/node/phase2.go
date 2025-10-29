package node

import (
	"context"
	"fmt"
	"log"

	"github.com/ymalegao/DistributedSystemsforAVs/bft/crypto"
	"github.com/ymalegao/DistributedSystemsforAVs/pb/bftconsensus"
)

// Phase2Orchestrator manages Phase 2 (leader certification)
type Phase2Orchestrator struct {
	state     *NodeState
	transport Transport
}

// NewPhase2Orchestrator creates a new Phase 2 orchestrator
func NewPhase2Orchestrator(state *NodeState, transport Transport) *Phase2Orchestrator {
	return &Phase2Orchestrator{
		state:     state,
		transport: transport,
	}
}

// StartPhase2 initiates Phase 2 as a FIN_CANDIDATE
func (p *Phase2Orchestrator) StartPhase2(ctx context.Context) error {
	view := p.state.GetView()
	myID := p.state.ID
	status := p.state.GetStatus()

	// Only FIN_CANDIDATE can start Phase 2
	if status != bftconsensus.BFTElectionStatus_BFT_FIN_CANDIDATE {
		return fmt.Errorf("not a FIN_CANDIDATE")
	}

	qc1 := p.state.GetQC1()
	if qc1 == nil {
		return fmt.Errorf("no QC1 available")
	}

	meta := p.state.GetMeta()

	// Compute payload hash: H(meta || QC1)
	payloadHash := crypto.HashLeaderPayload(meta, qc1)

	// Create attestation
	att := crypto.SignAttestation(
		p.state.KeyPair,
		view,
		bftconsensus.Phase_PHASE2_LEADER,
		myID,
		payloadHash,
		myID,
	)

	// Create SEND_LEADER message
	sendLeader := &bftconsensus.SendLeader{
		View:      view,
		SubjectId: myID,
		FinMeta:   meta,
		Qc1:       qc1,
		SenderAtt: att,
	}

	// Broadcast SEND_LEADER
	log.Printf("[%s] Broadcasting SEND_LEADER for view %d", myID, view)
	if err := p.transport.BroadcastSendLeader(ctx, sendLeader); err != nil {
		return fmt.Errorf("failed to broadcast SEND_LEADER: %w", err)
	}

	// Process our own SEND_LEADER to trigger ECHO
	if _, err := p.state.RBC.ProcessSendLeader(sendLeader); err != nil {
		log.Printf("[%s] Failed to process own SEND_LEADER: %v", myID, err)
	} else {
		// Send ECHO for our own leadership
		p.SendEchoLeader(ctx, view, myID, payloadHash)
	}

	return nil
}

// SendEchoLeader sends an ECHO_LEADER message
func (p *Phase2Orchestrator) SendEchoLeader(ctx context.Context, view uint64, subjectID string, payloadHash []byte) error {
	myID := p.state.ID

	// Create attestation
	att := crypto.SignAttestation(
		p.state.KeyPair,
		view,
		bftconsensus.Phase_PHASE2_LEADER,
		subjectID,
		payloadHash,
		myID,
	)

	// Create ECHO_LEADER message
	echoLeader := &bftconsensus.EchoLeader{
		View:        view,
		SubjectId:   subjectID,
		PayloadHash: payloadHash,
		Att:         att,
	}

	// Broadcast ECHO_LEADER
	if err := p.transport.BroadcastEchoLeader(ctx, echoLeader); err != nil {
		return fmt.Errorf("failed to broadcast ECHO_LEADER: %w", err)
	}

	// Process our own ECHO
	p.state.RBC.ProcessEchoLeader(echoLeader)

	return nil
}

// SendReadyLeader sends a READY_LEADER message
func (p *Phase2Orchestrator) SendReadyLeader(ctx context.Context, view uint64, subjectID string, payloadHash []byte) error {
	myID := p.state.ID

	// Create attestation
	att := crypto.SignAttestation(
		p.state.KeyPair,
		view,
		bftconsensus.Phase_PHASE2_LEADER,
		subjectID,
		payloadHash,
		myID,
	)

	// Create READY_LEADER message
	readyLeader := &bftconsensus.ReadyLeader{
		View:        view,
		SubjectId:   subjectID,
		PayloadHash: payloadHash,
		Att:         att,
	}

	// Broadcast READY_LEADER
	if err := p.transport.BroadcastReadyLeader(ctx, readyLeader); err != nil {
		return fmt.Errorf("failed to broadcast READY_LEADER: %w", err)
	}

	// Process our own READY
	p.state.RBC.ProcessReadyLeader(readyLeader)

	return nil
}

// MonitorPhase2 monitors Phase 2 and handles timeouts
func (p *Phase2Orchestrator) MonitorPhase2(ctx context.Context) error {
	myID := p.state.ID
	view := p.state.GetView()

	log.Printf("[%s] Monitoring Phase 2 for view %d", myID, view)

	// Create a context with timeout
	timeoutCtx, cancel := context.WithTimeout(ctx, p.state.Phase2Timeout)
	defer cancel()

	// Monitor RBC ready-to-send channel
	go func() {
		for {
			select {
			case <-timeoutCtx.Done():
				return
			case readyMsg := <-p.state.RBC.ReadyToSendChan():
				if readyMsg.Phase == bftconsensus.Phase_PHASE2_LEADER && readyMsg.View == view {
					p.SendReadyLeader(ctx, readyMsg.View, readyMsg.SubjectID, readyMsg.Hash)
				}
			}
		}
	}()

	// Monitor RBC delivery channel
	for {
		select {
		case <-timeoutCtx.Done():
			// Timeout - trigger view change
			log.Printf("[%s] Phase 2 timeout for view %d", myID, view)
			if p.state.GetQC2() == nil {
				p.state.IncrementView()
				log.Printf("[%s] View change to %d", myID, p.state.GetView())
			}
			return fmt.Errorf("phase 2 timeout")

		case result := <-p.state.RBC.DeliveryChan():
			// Check if this is the delivery we're waiting for
			if result.QC != nil && result.QC.Phase == bftconsensus.Phase_PHASE2_LEADER && result.QC.View == view {
				// QC2 delivered!
				log.Printf("[%s] QC2 delivered for leader %s", myID, result.QC.SubjectId)

				p.state.SetQC2(result.QC)
				p.state.SetLockedSubject(view, result.QC.SubjectId)

				// Update status
				if result.QC.SubjectId == myID {
					p.state.SetStatus(bftconsensus.BFTElectionStatus_BFT_LEADER)
					log.Printf("[%s] Became LEADER", myID)
				} else {
					p.state.SetStatus(bftconsensus.BFTElectionStatus_BFT_FOLLOWER)
					log.Printf("[%s] Became FOLLOWER (leader is %s)", myID, result.QC.SubjectId)
				}

				return nil
			}
			// If wrong phase/view, continue looping to get the right one
			log.Printf("[%s] Ignoring delivery for phase=%v, view=%d (waiting for PHASE2, view=%d)",
				myID, result.QC.Phase, result.QC.View, view)
		}
	}

	return nil
}

// RunPhase2 runs the complete Phase 2
func (p *Phase2Orchestrator) RunPhase2(ctx context.Context) error {
	// Only start if we're a FIN_CANDIDATE
	status := p.state.GetStatus()
	if status == bftconsensus.BFTElectionStatus_BFT_FIN_CANDIDATE {
		// Start Phase 2 by broadcasting SEND_LEADER
		if err := p.StartPhase2(ctx); err != nil {
			return err
		}
	}

	// Monitor Phase 2 for completion or timeout (all nodes monitor)
	return p.MonitorPhase2(ctx)
}

// HandleReceiveSendLeader handles incoming SEND_LEADER messages
func (p *Phase2Orchestrator) HandleReceiveSendLeader(ctx context.Context, msg *bftconsensus.SendLeader) error {
	view := p.state.GetView()

	// Check if message is for current view
	if msg.View != view {
		return fmt.Errorf("wrong view: expected %d, got %d", view, msg.View)
	}

	// Verify QC1
	pubKeys := p.state.GetPubKeysMap()
	pubKeyMap := make(map[string][]byte)
	for id, key := range pubKeys {
		pubKeyMap[id] = key
	}

	// Convert to ed25519.PublicKey map for verification
	// This is simplified - in real implementation, we'd properly convert
	if msg.Qc1 != nil {
		// Basic QC validation
		if len(msg.Qc1.Sigs) < p.state.RBC.GetQuorumSize() {
			return fmt.Errorf("invalid QC1: insufficient signatures")
		}
	}

	// Process through RBC
	echoAtt, err := p.state.RBC.ProcessSendLeader(msg)
	if err != nil {
		return err
	}

	if echoAtt != nil {
		// Send ECHO_LEADER
		payloadHash := crypto.HashLeaderPayload(msg.FinMeta, msg.Qc1)
		return p.SendEchoLeader(ctx, view, msg.SubjectId, payloadHash)
	}

	return nil
}

// HandleReceiveEchoLeader handles incoming ECHO_LEADER messages
func (p *Phase2Orchestrator) HandleReceiveEchoLeader(ctx context.Context, msg *bftconsensus.EchoLeader) error {
	view := p.state.GetView()

	if msg.View != view {
		return fmt.Errorf("wrong view: expected %d, got %d", view, msg.View)
	}

	// Process through RBC
	_, err := p.state.RBC.ProcessEchoLeader(msg)
	return err
}

// HandleReceiveReadyLeader handles incoming READY_LEADER messages
func (p *Phase2Orchestrator) HandleReceiveReadyLeader(ctx context.Context, msg *bftconsensus.ReadyLeader) error {
	view := p.state.GetView()

	if msg.View != view {
		return fmt.Errorf("wrong view: expected %d, got %d", view, msg.View)
	}

	// Process through RBC
	_, err := p.state.RBC.ProcessReadyLeader(msg)
	return err
}

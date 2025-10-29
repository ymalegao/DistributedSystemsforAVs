package node

import (
	"context"
	"fmt"
	"log"

	"github.com/ymalegao/DistributedSystemsforAVs/bft/crypto"
	"github.com/ymalegao/DistributedSystemsforAVs/pb/bftconsensus"
)

// Phase1Orchestrator manages Phase 1 (candidate certification)
type Phase1Orchestrator struct {
	state     *NodeState
	transport Transport
}

// Transport interface for sending messages
type Transport interface {
	BroadcastSendVote(ctx context.Context, msg *bftconsensus.SendVote) error
	BroadcastEchoVote(ctx context.Context, msg *bftconsensus.EchoVote) error
	BroadcastReadyVote(ctx context.Context, msg *bftconsensus.ReadyVote) error
	BroadcastSendLeader(ctx context.Context, msg *bftconsensus.SendLeader) error
	BroadcastEchoLeader(ctx context.Context, msg *bftconsensus.EchoLeader) error
	BroadcastReadyLeader(ctx context.Context, msg *bftconsensus.ReadyLeader) error
}

// NewPhase1Orchestrator creates a new Phase 1 orchestrator
func NewPhase1Orchestrator(state *NodeState, transport Transport) *Phase1Orchestrator {
	return &Phase1Orchestrator{
		state:     state,
		transport: transport,
	}
}

// StartPhase1 initiates Phase 1 as a candidate
func (p *Phase1Orchestrator) StartPhase1(ctx context.Context) error {
	view := p.state.GetView()
	myID := p.state.ID
	meta := p.state.GetMeta()

	// Compute payload hash
	payloadHash := crypto.HashVehicleMeta(meta)

	// Create attestation
	att := crypto.SignAttestation(
		p.state.KeyPair,
		view,
		bftconsensus.Phase_PHASE1_VOTE,
		myID,
		payloadHash,
		myID,
	)

	// Create SEND_VOTE message
	sendVote := &bftconsensus.SendVote{
		View:      view,
		SubjectId: myID,
		Payload:   meta,
		SenderAtt: att,
	}

	// Broadcast SEND_VOTE
	log.Printf("[%s] Broadcasting SEND_VOTE for view %d", myID, view)
	if err := p.transport.BroadcastSendVote(ctx, sendVote); err != nil {
		return fmt.Errorf("failed to broadcast SEND_VOTE: %w", err)
	}

	p.state.mu.Lock()
	p.state.SentVotes++
	p.state.mu.Unlock()

	// Process our own SEND_VOTE to trigger ECHO
	if _, err := p.state.RBC.ProcessSendVote(sendVote); err != nil {
		log.Printf("[%s] Failed to process own SEND_VOTE: %v", myID, err)
	} else {
		// Send ECHO for our own vote
		p.SendEchoVote(ctx, view, myID, payloadHash)
	}

	return nil
}

// SendEchoVote sends an ECHO_VOTE message
func (p *Phase1Orchestrator) SendEchoVote(ctx context.Context, view uint64, subjectID string, payloadHash []byte) error {
	myID := p.state.ID

	// Create attestation
	att := crypto.SignAttestation(
		p.state.KeyPair,
		view,
		bftconsensus.Phase_PHASE1_VOTE,
		subjectID,
		payloadHash,
		myID,
	)

	// Create ECHO_VOTE message
	echoVote := &bftconsensus.EchoVote{
		View:        view,
		SubjectId:   subjectID,
		PayloadHash: payloadHash,
		Att:         att,
	}

	// Broadcast ECHO_VOTE
	if err := p.transport.BroadcastEchoVote(ctx, echoVote); err != nil {
		return fmt.Errorf("failed to broadcast ECHO_VOTE: %w", err)
	}

	// Process our own ECHO
	p.state.RBC.ProcessEchoVote(echoVote)

	return nil
}

// SendReadyVote sends a READY_VOTE message
func (p *Phase1Orchestrator) SendReadyVote(ctx context.Context, view uint64, subjectID string, payloadHash []byte) error {
	myID := p.state.ID

	// Create attestation
	att := crypto.SignAttestation(
		p.state.KeyPair,
		view,
		bftconsensus.Phase_PHASE1_VOTE,
		subjectID,
		payloadHash,
		myID,
	)

	// Create READY_VOTE message
	readyVote := &bftconsensus.ReadyVote{
		View:        view,
		SubjectId:   subjectID,
		PayloadHash: payloadHash,
		Att:         att,
	}

	// Broadcast READY_VOTE
	if err := p.transport.BroadcastReadyVote(ctx, readyVote); err != nil {
		return fmt.Errorf("failed to broadcast READY_VOTE: %w", err)
	}

	// Process our own READY
	p.state.RBC.ProcessReadyVote(readyVote)

	return nil
}

// MonitorPhase1 monitors Phase 1 and handles timeouts
func (p *Phase1Orchestrator) MonitorPhase1(ctx context.Context) error {
	myID := p.state.ID
	view := p.state.GetView()

	log.Printf("[%s] Monitoring Phase 1 for view %d", myID, view)

	// Create a context with timeout
	timeoutCtx, cancel := context.WithTimeout(ctx, p.state.Phase1Timeout)
	defer cancel()

	// Monitor RBC ready-to-send channel
	go func() {
		for {
			select {
			case <-timeoutCtx.Done():
				return
			case readyMsg := <-p.state.RBC.ReadyToSendChan():
				if readyMsg.Phase == bftconsensus.Phase_PHASE1_VOTE && readyMsg.View == view {
					p.SendReadyVote(ctx, readyMsg.View, readyMsg.SubjectID, readyMsg.Hash)
				}
			}
		}
	}()

	// Monitor RBC delivery channel
	for {
		select {
		case <-timeoutCtx.Done():
			// Timeout - trigger view change
			log.Printf("[%s] Phase 1 timeout for view %d", myID, view)
			if p.state.GetQC1() == nil {
				p.state.IncrementView()
				log.Printf("[%s] View change to %d", myID, p.state.GetView())
			}
			return fmt.Errorf("phase 1 timeout")

		case result := <-p.state.RBC.DeliveryChan():
			// Check if this is the delivery we're waiting for
			if result.QC != nil && result.QC.Phase == bftconsensus.Phase_PHASE1_VOTE && result.QC.View == view {
				// QC1 delivered!
				log.Printf("[%s] QC1 delivered for subject %s", myID, result.QC.SubjectId)

				p.state.SetQC1(result.QC)
				p.state.SetLockedSubject(view, result.QC.SubjectId)

				// Update status
				if result.QC.SubjectId == myID {
					p.state.SetStatus(bftconsensus.BFTElectionStatus_BFT_FIN_CANDIDATE)
					log.Printf("[%s] Became FIN_CANDIDATE", myID)
				} else {
					p.state.SetStatus(bftconsensus.BFTElectionStatus_BFT_FOLLOWER)
					log.Printf("[%s] Became FOLLOWER (locked on %s)", myID, result.QC.SubjectId)
				}

				return nil
			}
			// If wrong phase/view, continue looping to get the right one
			log.Printf("[%s] Ignoring delivery for phase=%v, view=%d (waiting for PHASE1, view=%d)",
				myID, result.QC.Phase, result.QC.View, view)
		}
	}

	return nil
}

// RunPhase1 runs the complete Phase 1
func (p *Phase1Orchestrator) RunPhase1(ctx context.Context) error {
	// Start Phase 1 by broadcasting SEND_VOTE
	if err := p.StartPhase1(ctx); err != nil {
		return err
	}

	// Monitor Phase 1 for completion or timeout
	return p.MonitorPhase1(ctx)
}

// HandleReceiveSendVote handles incoming SEND_VOTE messages
func (p *Phase1Orchestrator) HandleReceiveSendVote(ctx context.Context, msg *bftconsensus.SendVote) error {
	view := p.state.GetView()

	// Check if message is for current view
	if msg.View != view {
		return fmt.Errorf("wrong view: expected %d, got %d", view, msg.View)
	}

	// Process through RBC
	echoAtt, err := p.state.RBC.ProcessSendVote(msg)
	if err != nil {
		return err
	}

	if echoAtt != nil {
		// Send ECHO_VOTE
		payloadHash := crypto.HashVehicleMeta(msg.Payload)
		return p.SendEchoVote(ctx, view, msg.SubjectId, payloadHash)
	}

	return nil
}

// HandleReceiveEchoVote handles incoming ECHO_VOTE messages
func (p *Phase1Orchestrator) HandleReceiveEchoVote(ctx context.Context, msg *bftconsensus.EchoVote) error {
	view := p.state.GetView()

	if msg.View != view {
		return fmt.Errorf("wrong view: expected %d, got %d", view, msg.View)
	}

	// Process through RBC
	_, err := p.state.RBC.ProcessEchoVote(msg)
	return err
}

// HandleReceiveReadyVote handles incoming READY_VOTE messages
func (p *Phase1Orchestrator) HandleReceiveReadyVote(ctx context.Context, msg *bftconsensus.ReadyVote) error {
	view := p.state.GetView()

	if msg.View != view {
		return fmt.Errorf("wrong view: expected %d, got %d", view, msg.View)
	}

	// Process through RBC
	_, err := p.state.RBC.ProcessReadyVote(msg)
	return err
}

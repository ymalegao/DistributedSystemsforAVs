package node

import (
	"context"
	"log"

	"github.com/ymalegao/DistributedSystemsforAVs/pb/bftconsensus"
)

// BFTHandlers implements the gRPC service handlers
type BFTHandlers struct {
	bftconsensus.UnimplementedBFTConsensusUnaryServer

	state  *NodeState
	phase1 *Phase1Orchestrator
	phase2 *Phase2Orchestrator
}

// NewBFTHandlers creates a new BFT handlers instance
func NewBFTHandlers(state *NodeState, phase1 *Phase1Orchestrator, phase2 *Phase2Orchestrator) *BFTHandlers {
	return &BFTHandlers{
		state:  state,
		phase1: phase1,
		phase2: phase2,
	}
}

// ReceiveSendVote handles SEND_VOTE messages
func (h *BFTHandlers) ReceiveSendVote(ctx context.Context, msg *bftconsensus.SendVote) (*bftconsensus.WireAck, error) {
	// Delegate to Phase 1 orchestrator
	if err := h.phase1.HandleReceiveSendVote(ctx, msg); err != nil {
		log.Printf("[%s] Error handling SEND_VOTE from %s: %v", h.state.ID, msg.SubjectId, err)
		return &bftconsensus.WireAck{
			Ok:     false,
			Reason: err.Error(),
		}, nil // Return nil error so gRPC doesn't fail the call
	}

	h.state.mu.Lock()
	h.state.ReceivedVotes++
	h.state.mu.Unlock()

	return &bftconsensus.WireAck{Ok: true}, nil
}

// ReceiveEchoVote handles ECHO_VOTE messages
func (h *BFTHandlers) ReceiveEchoVote(ctx context.Context, msg *bftconsensus.EchoVote) (*bftconsensus.WireAck, error) {
	if err := h.phase1.HandleReceiveEchoVote(ctx, msg); err != nil {
		log.Printf("[%s] Error handling ECHO_VOTE from %s: %v", h.state.ID, msg.SubjectId, err)
		return &bftconsensus.WireAck{
			Ok:     false,
			Reason: err.Error(),
		}, nil
	}

	return &bftconsensus.WireAck{Ok: true}, nil
}

// ReceiveReadyVote handles READY_VOTE messages
func (h *BFTHandlers) ReceiveReadyVote(ctx context.Context, msg *bftconsensus.ReadyVote) (*bftconsensus.WireAck, error) {
	if err := h.phase1.HandleReceiveReadyVote(ctx, msg); err != nil {
		log.Printf("[%s] Error handling READY_VOTE from %s: %v", h.state.ID, msg.SubjectId, err)
		return &bftconsensus.WireAck{
			Ok:     false,
			Reason: err.Error(),
		}, nil
	}

	return &bftconsensus.WireAck{Ok: true}, nil
}

// ReceiveSendLeader handles SEND_LEADER messages
func (h *BFTHandlers) ReceiveSendLeader(ctx context.Context, msg *bftconsensus.SendLeader) (*bftconsensus.WireAck, error) {
	// Delegate to Phase 2 orchestrator
	if err := h.phase2.HandleReceiveSendLeader(ctx, msg); err != nil {
		log.Printf("[%s] Error handling SEND_LEADER from %s: %v", h.state.ID, msg.SubjectId, err)
		return &bftconsensus.WireAck{
			Ok:     false,
			Reason: err.Error(),
		}, nil
	}

	return &bftconsensus.WireAck{Ok: true}, nil
}

// ReceiveEchoLeader handles ECHO_LEADER messages
func (h *BFTHandlers) ReceiveEchoLeader(ctx context.Context, msg *bftconsensus.EchoLeader) (*bftconsensus.WireAck, error) {
	if err := h.phase2.HandleReceiveEchoLeader(ctx, msg); err != nil {
		log.Printf("[%s] Error handling ECHO_LEADER from %s: %v", h.state.ID, msg.SubjectId, err)
		return &bftconsensus.WireAck{
			Ok:     false,
			Reason: err.Error(),
		}, nil
	}

	return &bftconsensus.WireAck{Ok: true}, nil
}

// ReceiveReadyLeader handles READY_LEADER messages
func (h *BFTHandlers) ReceiveReadyLeader(ctx context.Context, msg *bftconsensus.ReadyLeader) (*bftconsensus.WireAck, error) {
	if err := h.phase2.HandleReceiveReadyLeader(ctx, msg); err != nil {
		log.Printf("[%s] Error handling READY_LEADER from %s: %v", h.state.ID, msg.SubjectId, err)
		return &bftconsensus.WireAck{
			Ok:     false,
			Reason: err.Error(),
		}, nil
	}

	return &bftconsensus.WireAck{Ok: true}, nil
}

// Gossip handles the bidirectional streaming RPC (optional, for future use)
func (h *BFTHandlers) Gossip(stream bftconsensus.BFTConsensus_GossipServer) error {
	// For now, we use unary RPCs
	// This can be implemented later for more efficient streaming
	return nil
}

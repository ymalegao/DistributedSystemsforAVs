package node

import (
	"context"
	"fmt"
	"log"
	"time"

	"github.com/ymalegao/DistributedSystemsforAVs/bft/transport"
	"github.com/ymalegao/DistributedSystemsforAVs/pb/bftconsensus"
)

// BFTNode represents a Byzantine Fault Tolerant consensus node
type BFTNode struct {
	state      *NodeState
	phase1     *Phase1Orchestrator
	phase2     *Phase2Orchestrator
	handlers   *BFTHandlers
	server     *transport.Server
	clientPool *transport.ClientPool
	transport  *transport.BroadcastTransport
}

// NewBFTNode creates a new BFT node
func NewBFTNode(id, plate, direction, addr string, n, f int) (*BFTNode, error) {
	// Create node state
	state, err := NewNodeState(id, plate, direction, n, f)
	if err != nil {
		return nil, fmt.Errorf("failed to create node state: %w", err)
	}

	// Create client pool
	clientPool := transport.NewClientPool()

	// Create broadcast transport
	broadcastTransport := transport.NewBroadcastTransport(clientPool)

	// Create phase orchestrators
	phase1 := NewPhase1Orchestrator(state, broadcastTransport)
	phase2 := NewPhase2Orchestrator(state, broadcastTransport)

	// Create handlers
	handlers := NewBFTHandlers(state, phase1, phase2)

	// Create server
	server, err := transport.NewServer(addr, handlers)
	if err != nil {
		return nil, fmt.Errorf("failed to create server: %w", err)
	}

	return &BFTNode{
		state:      state,
		phase1:     phase1,
		phase2:     phase2,
		handlers:   handlers,
		server:     server,
		clientPool: clientPool,
		transport:  broadcastTransport,
	}, nil
}

// Start starts the BFT node server
func (n *BFTNode) Start() error {
	log.Printf("[%s] Starting BFT node on %s", n.state.ID, n.server.Addr())

	// Start server in goroutine
	go func() {
		if err := n.server.Start(); err != nil {
			log.Printf("[%s] Server error: %v", n.state.ID, err)
		}
	}()

	return nil
}

// Stop stops the BFT node
func (n *BFTNode) Stop() {
	log.Printf("[%s] Stopping BFT node", n.state.ID)

	n.state.Shutdown()
	n.server.Stop()
	n.clientPool.Close()
}

// ConnectToPeer connects to a peer
func (n *BFTNode) ConnectToPeer(peerID, addr string, pubKey []byte, timeout time.Duration) error {
	// Add peer to state
	n.state.AddPeer(peerID, addr, pubKey)

	// Connect via client pool
	return n.clientPool.Connect(peerID, addr, timeout)
}

// RunConsensus runs the complete consensus protocol
func (n *BFTNode) RunConsensus(ctx context.Context) error {
	log.Printf("[%s] Starting consensus for view %d", n.state.ID, n.state.GetView())

	// Phase 1: Candidate certification
	log.Printf("[%s] === Phase 1: Candidate Certification ===", n.state.ID)
	if err := n.phase1.RunPhase1(ctx); err != nil {
		log.Printf("[%s] Phase 1 failed: %v", n.state.ID, err)
		return err
	}

	// Check if we have QC1
	qc1 := n.state.GetQC1()
	if qc1 == nil {
		return fmt.Errorf("phase 1 failed: no QC1")
	}

	log.Printf("[%s] Phase 1 complete: QC1 for %s", n.state.ID, qc1.SubjectId)

	// Phase 2: Leader certification
	log.Printf("[%s] === Phase 2: Leader Certification ===", n.state.ID)
	if err := n.phase2.RunPhase2(ctx); err != nil {
		log.Printf("[%s] Phase 2 failed: %v", n.state.ID, err)
		return err
	}

	// Check if we have QC2
	qc2 := n.state.GetQC2()
	if qc2 == nil {
		return fmt.Errorf("phase 2 failed: no QC2")
	}

	log.Printf("[%s] Phase 2 complete: Leader is %s", n.state.ID, qc2.SubjectId)

	return nil
}

// GetStatus returns the current status
func (n *BFTNode) GetStatus() bftconsensus.BFTElectionStatus {
	return n.state.GetStatus()
}

// GetLeaderID returns the leader ID (from QC2)
func (n *BFTNode) GetLeaderID() string {
	qc2 := n.state.GetQC2()
	if qc2 != nil {
		return qc2.SubjectId
	}
	return ""
}

// GetQC1 returns the Phase 1 quorum certificate
func (n *BFTNode) GetQC1() *bftconsensus.QuorumCert {
	return n.state.GetQC1()
}

// GetQC2 returns the Phase 2 quorum certificate
func (n *BFTNode) GetQC2() *bftconsensus.QuorumCert {
	return n.state.GetQC2()
}

// ID returns the node ID
func (n *BFTNode) ID() string {
	return n.state.ID
}

// GetPublicKey returns the node's public key
func (n *BFTNode) GetPublicKey() []byte {
	return n.state.KeyPair.PublicKey
}

// ResetForEpoch resets the node for a new epoch
func (n *BFTNode) ResetForEpoch() {
	n.state.ResetForEpoch()
}

// GetView returns the current view
func (n *BFTNode) GetView() uint64 {
	return n.state.GetView()
}

// SetView sets the current view
func (n *BFTNode) SetView(view uint64) {
	n.state.SetView(view)
}

// Addr returns the node's address
func (n *BFTNode) Addr() string {
	return n.server.Addr()
}

// SetByzantineBehavior sets Byzantine behavior for testing
func (n *BFTNode) SetByzantineBehavior(behavior interface{}) {
	// Pass the behavior directly - transport will do the type assertion
	n.transport.SetBehavior(behavior)
}

// SetVANETSimulator sets the VANET simulator for network simulation
func (n *BFTNode) SetVANETSimulator(sim interface{}) {
	// The transport expects a VANETSimulator interface
	// Pass it directly - transport will handle it
	if vanetSim, ok := sim.(interface {
		SimulatePacketDrop() bool
		SimulateLatency() time.Duration
	}); ok {
		n.transport.SetVANETSimulator(vanetSim)
		log.Printf("[%s] VANET simulator enabled", n.state.ID)
	}
}

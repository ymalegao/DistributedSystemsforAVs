package node

import (
	"context"
	"sync"
	"time"

	"github.com/ymalegao/DistributedSystemsforAVs/bft/crypto"
	"github.com/ymalegao/DistributedSystemsforAVs/bft/rbc"
	"github.com/ymalegao/DistributedSystemsforAVs/pb/bftconsensus"
)

// NodeState manages the state of a BFT node
type NodeState struct {
	mu sync.RWMutex

	// Identity
	ID        string
	Plate     string
	Direction string
	KeyPair   *crypto.KeyPair

	// Election state
	View          uint64
	Status        bftconsensus.BFTElectionStatus
	LockedSubject map[uint64]string // view -> subject_id

	// Quorum certificates
	QC1 *bftconsensus.QuorumCert // Phase 1 cert
	QC2 *bftconsensus.QuorumCert // Phase 2 cert

	// RBC instance
	RBC *rbc.RBC

	// Peers
	Peers   map[string]string // id -> address
	PubKeys map[string][]byte // id -> public key (ed25519)

	// Timers
	Phase1Timeout time.Duration
	Phase2Timeout time.Duration

	// Channels
	shutdownChan chan struct{}

	// Metrics
	SentVotes     uint32
	ReceivedVotes uint32
	ElectionTime  int64
}

// NewNodeState creates a new node state
func NewNodeState(id, plate, direction string, n, f int) (*NodeState, error) {
	kp, err := crypto.GenerateKeyPair()
	if err != nil {
		return nil, err
	}

	return &NodeState{
		ID:            id,
		Plate:         plate,
		Direction:     direction,
		KeyPair:       kp,
		View:          0,
		Status:        bftconsensus.BFTElectionStatus_BFT_INIT_CANDIDATE,
		LockedSubject: make(map[uint64]string),
		RBC:           rbc.NewRBC(n, f),
		Peers:         make(map[string]string),
		PubKeys:       make(map[string][]byte),
		Phase1Timeout: time.Duration(n*200) * time.Millisecond,
		Phase2Timeout: time.Duration(n*200) * time.Millisecond,
		shutdownChan:  make(chan struct{}),
	}, nil
}

// GetView returns the current view
func (ns *NodeState) GetView() uint64 {
	ns.mu.RLock()
	defer ns.mu.RUnlock()
	return ns.View
}

// SetView updates the view
func (ns *NodeState) SetView(view uint64) {
	ns.mu.Lock()
	defer ns.mu.Unlock()
	ns.View = view
}

// GetStatus returns the current status
func (ns *NodeState) GetStatus() bftconsensus.BFTElectionStatus {
	ns.mu.RLock()
	defer ns.mu.RUnlock()
	return ns.Status
}

// SetStatus updates the status
func (ns *NodeState) SetStatus(status bftconsensus.BFTElectionStatus) {
	ns.mu.Lock()
	defer ns.mu.Unlock()
	ns.Status = status
}

// GetQC1 returns the Phase 1 QC
func (ns *NodeState) GetQC1() *bftconsensus.QuorumCert {
	ns.mu.RLock()
	defer ns.mu.RUnlock()
	return ns.QC1
}

// SetQC1 updates the Phase 1 QC
func (ns *NodeState) SetQC1(qc *bftconsensus.QuorumCert) {
	ns.mu.Lock()
	defer ns.mu.Unlock()
	ns.QC1 = qc
}

// GetQC2 returns the Phase 2 QC
func (ns *NodeState) GetQC2() *bftconsensus.QuorumCert {
	ns.mu.RLock()
	defer ns.mu.RUnlock()
	return ns.QC2
}

// SetQC2 updates the Phase 2 QC
func (ns *NodeState) SetQC2(qc *bftconsensus.QuorumCert) {
	ns.mu.Lock()
	defer ns.mu.Unlock()
	ns.QC2 = qc
}

// GetLockedSubject returns the locked subject for a view
func (ns *NodeState) GetLockedSubject(view uint64) string {
	ns.mu.RLock()
	defer ns.mu.RUnlock()
	return ns.LockedSubject[view]
}

// SetLockedSubject sets the locked subject for a view
func (ns *NodeState) SetLockedSubject(view uint64, subjectID string) {
	ns.mu.Lock()
	defer ns.mu.Unlock()
	ns.LockedSubject[view] = subjectID
}

// GetMeta returns the vehicle metadata
func (ns *NodeState) GetMeta() *bftconsensus.BFTVehicleMeta {
	ns.mu.RLock()
	defer ns.mu.RUnlock()

	return &bftconsensus.BFTVehicleMeta{
		Id:             ns.ID,
		Plate:          ns.Plate,
		Direction:      ns.Direction,
		Status:         ns.Status,
		SentVotes:      ns.SentVotes,
		ReceivedVotes:  ns.ReceivedVotes,
		ElectionTimeMs: ns.ElectionTime,
	}
}

// AddPeer adds a peer to the node
func (ns *NodeState) AddPeer(id, addr string, pubKey []byte) {
	ns.mu.Lock()
	defer ns.mu.Unlock()

	ns.Peers[id] = addr
	ns.PubKeys[id] = pubKey
}

// GetPeers returns all peers
func (ns *NodeState) GetPeers() map[string]string {
	ns.mu.RLock()
	defer ns.mu.RUnlock()

	peers := make(map[string]string)
	for id, addr := range ns.Peers {
		peers[id] = addr
	}
	return peers
}

// GetPubKeys returns all public keys as ed25519.PublicKey map
func (ns *NodeState) GetPubKeysMap() map[string][]byte {
	ns.mu.RLock()
	defer ns.mu.RUnlock()

	keys := make(map[string][]byte)
	for id, key := range ns.PubKeys {
		keys[id] = key
	}
	return keys
}

// ResetForEpoch resets the node state for a new epoch
func (ns *NodeState) ResetForEpoch() {
	ns.mu.Lock()
	defer ns.mu.Unlock()

	ns.Status = bftconsensus.BFTElectionStatus_BFT_INIT_CANDIDATE
	ns.QC1 = nil
	ns.QC2 = nil
	ns.SentVotes = 0
	ns.ReceivedVotes = 0
	ns.ElectionTime = 0

	// Don't reset view or locked subjects - those persist across epochs

	// Reset RBC
	ns.RBC.Reset()
}

// IncrementView increments the view (for view change)
func (ns *NodeState) IncrementView() {
	ns.mu.Lock()
	defer ns.mu.Unlock()
	ns.View++
}

// Shutdown signals the node to shut down
func (ns *NodeState) Shutdown() {
	close(ns.shutdownChan)
}

// ShutdownChan returns the shutdown channel
func (ns *NodeState) ShutdownChan() <-chan struct{} {
	return ns.shutdownChan
}

// WaitWithContext waits for either context cancellation or shutdown
func (ns *NodeState) WaitWithContext(ctx context.Context) error {
	select {
	case <-ctx.Done():
		return ctx.Err()
	case <-ns.shutdownChan:
		return context.Canceled
	}
}

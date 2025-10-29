package rbc

import (
	"fmt"
	"sync"

	"github.com/ymalegao/DistributedSystemsforAVs/bft/crypto"
	"github.com/ymalegao/DistributedSystemsforAVs/pb/bftconsensus"
)

// RBCConfig holds configuration for RBC
type RBCConfig struct {
	N          int // total nodes
	F          int // max byzantine faults
	QuorumSize int // 2f + 1
}

// RBC implements the Reliable Broadcast protocol (SEND/ECHO/READY)
type RBC struct {
	config       RBCConfig
	store        *RBCStore
	attestations *AttestationStore // Store attestations for QC construction

	mu           sync.RWMutex
	deliveryChan chan *RBCResult
	readyToSend  chan *ReadyMessage // Channel for READY messages to send
}

// ReadyMessage represents a READY message to be sent
type ReadyMessage struct {
	View      uint64
	Phase     bftconsensus.Phase
	SubjectID string
	Hash      []byte
}

// AttestationStore stores attestations for QC construction
type AttestationStore struct {
	mu sync.RWMutex
	// key: "view:phase:subject:hash" -> map[senderID]*Attestation
	store map[string]map[string]*bftconsensus.Attestation
}

// NewAttestationStore creates a new attestation store
func NewAttestationStore() *AttestationStore {
	return &AttestationStore{
		store: make(map[string]map[string]*bftconsensus.Attestation),
	}
}

// Add adds an attestation
func (as *AttestationStore) Add(view uint64, phase bftconsensus.Phase, subjectID string, hash []byte, att *bftconsensus.Attestation) {
	as.mu.Lock()
	defer as.mu.Unlock()

	key := makeKey(view, phase, subjectID, hash)
	if as.store[key] == nil {
		as.store[key] = make(map[string]*bftconsensus.Attestation)
	}
	as.store[key][att.SignerId] = att
}

// GetAll returns all attestations for a given key
func (as *AttestationStore) GetAll(view uint64, phase bftconsensus.Phase, subjectID string, hash []byte) []*bftconsensus.Attestation {
	as.mu.RLock()
	defer as.mu.RUnlock()

	key := makeKey(view, phase, subjectID, hash)
	atts := as.store[key]
	if atts == nil {
		return nil
	}

	result := make([]*bftconsensus.Attestation, 0, len(atts))
	for _, att := range atts {
		result = append(result, att)
	}
	return result
}

// NewRBC creates a new RBC instance
func NewRBC(n, f int) *RBC {
	return &RBC{
		config: RBCConfig{
			N:          n,
			F:          f,
			QuorumSize: 2*f + 1,
		},
		store:        NewRBCStore(),
		attestations: NewAttestationStore(),
		deliveryChan: make(chan *RBCResult, 100),
		readyToSend:  make(chan *ReadyMessage, 100),
	}
}

// DeliveryChan returns the channel for RBC deliveries
func (r *RBC) DeliveryChan() <-chan *RBCResult {
	return r.deliveryChan
}

// ReadyToSendChan returns the channel for READY messages to send
func (r *RBC) ReadyToSendChan() <-chan *ReadyMessage {
	return r.readyToSend
}

// ValidatePayload checks if payload is valid and collision-free
// This is a placeholder - implement actual validation logic
func (r *RBC) ValidatePayload(payload interface{}) bool {
	// TODO: Implement actual validation
	return payload != nil
}

// ProcessSend handles SEND messages (Phase 1 or Phase 2)
func (r *RBC) ProcessSend(view uint64, phase bftconsensus.Phase, subjectID string, payloadHash []byte, payload interface{}, att *bftconsensus.Attestation) (*bftconsensus.Attestation, error) {
	// Check if already delivered
	if r.store.IsDelivered(view, phase, subjectID, payloadHash) {
		return nil, fmt.Errorf("already delivered")
	}

	// Check if already seen SEND
	if !r.store.MarkSeenSend(view, phase, subjectID, payloadHash) {
		return nil, fmt.Errorf("already seen SEND")
	}

	// Validate payload
	if !r.ValidatePayload(payload) {
		return nil, fmt.Errorf("invalid payload")
	}

	// Store payload
	r.store.SetPayload(view, phase, subjectID, payloadHash, payload)

	// Verify sender's attestation (in real impl, verify signature)
	// For now, we accept it

	// Return our ECHO attestation (to be signed by caller)
	return &bftconsensus.Attestation{
		View:        view,
		Phase:       phase,
		SubjectId:   subjectID,
		PayloadHash: payloadHash,
		// SignerId and Sig to be filled by caller
	}, nil
}

// ProcessEcho handles ECHO messages
func (r *RBC) ProcessEcho(view uint64, phase bftconsensus.Phase, subjectID string, payloadHash []byte, att *bftconsensus.Attestation) (bool, error) {
	// Check if already delivered
	if r.store.IsDelivered(view, phase, subjectID, payloadHash) {
		return false, nil
	}

	// Store the attestation
	r.attestations.Add(view, phase, subjectID, payloadHash, att)

	// Add echo count
	echoCount := r.store.AddEcho(view, phase, subjectID, payloadHash, att.SignerId)

	// Threshold for ECHO -> READY: f+1 ECHOs
	// But for Phase 2, we need n-f ECHOs
	threshold := r.config.F + 1
	if phase == bftconsensus.Phase_PHASE2_LEADER {
		threshold = r.config.N - r.config.F
	}

	// If we reach threshold and haven't sent READY yet, trigger READY
	if echoCount >= threshold && !r.store.HasSentReady(view, phase, subjectID, payloadHash) {
		if r.store.MarkSentReady(view, phase, subjectID, payloadHash) {
			// Signal to send READY
			r.readyToSend <- &ReadyMessage{
				View:      view,
				Phase:     phase,
				SubjectID: subjectID,
				Hash:      payloadHash,
			}
			return true, nil
		}
	}

	return false, nil
}

// ProcessReady handles READY messages
func (r *RBC) ProcessReady(view uint64, phase bftconsensus.Phase, subjectID string, payloadHash []byte, att *bftconsensus.Attestation) (bool, error) {
	// Check if already delivered
	if r.store.IsDelivered(view, phase, subjectID, payloadHash) {
		return false, nil
	}

	// Store the attestation
	r.attestations.Add(view, phase, subjectID, payloadHash, att)

	// Add ready count
	readyCount := r.store.AddReady(view, phase, subjectID, payloadHash, att.SignerId)

	// Amplification rule: if we see f READYs and haven't sent READY, send it
	if readyCount == r.config.F && !r.store.HasSentReady(view, phase, subjectID, payloadHash) {
		if r.store.MarkSentReady(view, phase, subjectID, payloadHash) {
			r.readyToSend <- &ReadyMessage{
				View:      view,
				Phase:     phase,
				SubjectID: subjectID,
				Hash:      payloadHash,
			}
		}
	}

	// Delivery rule: if we see 2f+1 READYs and haven't delivered, deliver
	if readyCount >= r.config.QuorumSize && !r.store.IsDelivered(view, phase, subjectID, payloadHash) {
		if r.store.MarkDelivered(view, phase, subjectID, payloadHash) {
			// Construct QC from READY attestations
			attestations := r.attestations.GetAll(view, phase, subjectID, payloadHash)

			// Filter only READY attestations (we stored all, but need only READYs for QC)
			readyAtts := make([]*bftconsensus.Attestation, 0)
			for _, att := range attestations {
				// In real impl, check if this is a READY attestation
				readyAtts = append(readyAtts, att)
				if len(readyAtts) >= r.config.QuorumSize {
					break
				}
			}

			qc := &bftconsensus.QuorumCert{
				View:        view,
				Phase:       phase,
				SubjectId:   subjectID,
				PayloadHash: payloadHash,
				Sigs:        readyAtts,
			}

			payload := r.store.GetPayload(view, phase, subjectID, payloadHash)

			// Deliver
			r.deliveryChan <- &RBCResult{
				QC:      qc,
				Payload: payload,
			}

			return true, nil
		}
	}

	return false, nil
}

// Reset clears all state (for view change)
func (r *RBC) Reset() {
	r.mu.Lock()
	defer r.mu.Unlock()

	r.store.Reset()
	r.attestations = NewAttestationStore()
}

// GetQuorumSize returns the quorum size
func (r *RBC) GetQuorumSize() int {
	return r.config.QuorumSize
}

// GetF returns the fault tolerance parameter
func (r *RBC) GetF() int {
	return r.config.F
}

// GetN returns the total number of nodes
func (r *RBC) GetN() int {
	return r.config.N
}

// Helper functions for specific phases

// ProcessSendVote handles SEND_VOTE (Phase 1)
func (r *RBC) ProcessSendVote(msg *bftconsensus.SendVote) (*bftconsensus.Attestation, error) {
	hash := crypto.HashVehicleMeta(msg.Payload)
	return r.ProcessSend(msg.View, bftconsensus.Phase_PHASE1_VOTE, msg.SubjectId, hash, msg.Payload, msg.SenderAtt)
}

// ProcessSendLeader handles SEND_LEADER (Phase 2)
func (r *RBC) ProcessSendLeader(msg *bftconsensus.SendLeader) (*bftconsensus.Attestation, error) {
	hash := crypto.HashLeaderPayload(msg.FinMeta, msg.Qc1)
	payload := map[string]interface{}{
		"meta": msg.FinMeta,
		"qc1":  msg.Qc1,
	}
	return r.ProcessSend(msg.View, bftconsensus.Phase_PHASE2_LEADER, msg.SubjectId, hash, payload, msg.SenderAtt)
}

// ProcessEchoVote handles ECHO_VOTE (Phase 1)
func (r *RBC) ProcessEchoVote(msg *bftconsensus.EchoVote) (bool, error) {
	return r.ProcessEcho(msg.View, bftconsensus.Phase_PHASE1_VOTE, msg.SubjectId, msg.PayloadHash, msg.Att)
}

// ProcessEchoLeader handles ECHO_LEADER (Phase 2)
func (r *RBC) ProcessEchoLeader(msg *bftconsensus.EchoLeader) (bool, error) {
	return r.ProcessEcho(msg.View, bftconsensus.Phase_PHASE2_LEADER, msg.SubjectId, msg.PayloadHash, msg.Att)
}

// ProcessReadyVote handles READY_VOTE (Phase 1)
func (r *RBC) ProcessReadyVote(msg *bftconsensus.ReadyVote) (bool, error) {
	return r.ProcessReady(msg.View, bftconsensus.Phase_PHASE1_VOTE, msg.SubjectId, msg.PayloadHash, msg.Att)
}

// ProcessReadyLeader handles READY_LEADER (Phase 2)
func (r *RBC) ProcessReadyLeader(msg *bftconsensus.ReadyLeader) (bool, error) {
	return r.ProcessReady(msg.View, bftconsensus.Phase_PHASE2_LEADER, msg.SubjectId, msg.PayloadHash, msg.Att)
}

package rbc

import (
	"fmt"
	"sync"

	"github.com/ymalegao/DistributedSystemsforAVs/pb/bftconsensus"
)

// RBCStore manages all RBC instances and their state
type RBCStore struct {
	mu        sync.RWMutex
	instances map[string]*RBCInstance // key: "view:phase:subject:hash"
}

// NewRBCStore creates a new RBC store
func NewRBCStore() *RBCStore {
	return &RBCStore{
		instances: make(map[string]*RBCInstance),
	}
}

// makeKey creates a unique key for an RBC instance
func makeKey(view uint64, phase bftconsensus.Phase, subjectID string, hash []byte) string {
	return fmt.Sprintf("%d:%d:%s:%x", view, phase, subjectID, hash)
}

// GetOrCreate retrieves or creates an RBC instance
func (s *RBCStore) GetOrCreate(view uint64, phase bftconsensus.Phase, subjectID string, hash []byte) *RBCInstance {
	s.mu.Lock()
	defer s.mu.Unlock()

	key := makeKey(view, phase, subjectID, hash)
	inst, exists := s.instances[key]
	if !exists {
		inst = NewRBCInstance(view, phase, subjectID)
		inst.PayloadHash = hash
		s.instances[key] = inst
	}
	return inst
}

// Get retrieves an existing RBC instance
func (s *RBCStore) Get(view uint64, phase bftconsensus.Phase, subjectID string, hash []byte) *RBCInstance {
	s.mu.RLock()
	defer s.mu.RUnlock()

	key := makeKey(view, phase, subjectID, hash)
	return s.instances[key]
}

// AddEcho records an ECHO from a sender
func (s *RBCStore) AddEcho(view uint64, phase bftconsensus.Phase, subjectID string, hash []byte, senderID string) int {
	inst := s.GetOrCreate(view, phase, subjectID, hash)

	s.mu.Lock()
	defer s.mu.Unlock()

	inst.EchoSenders[senderID] = true
	return len(inst.EchoSenders)
}

// AddReady records a READY from a sender
func (s *RBCStore) AddReady(view uint64, phase bftconsensus.Phase, subjectID string, hash []byte, senderID string) int {
	inst := s.GetOrCreate(view, phase, subjectID, hash)

	s.mu.Lock()
	defer s.mu.Unlock()

	inst.ReadySenders[senderID] = true
	return len(inst.ReadySenders)
}

// GetEchoCount returns the number of ECHO messages
func (s *RBCStore) GetEchoCount(view uint64, phase bftconsensus.Phase, subjectID string, hash []byte) int {
	s.mu.RLock()
	defer s.mu.RUnlock()

	key := makeKey(view, phase, subjectID, hash)
	inst, exists := s.instances[key]
	if !exists {
		return 0
	}
	return len(inst.EchoSenders)
}

// GetReadyCount returns the number of READY messages
func (s *RBCStore) GetReadyCount(view uint64, phase bftconsensus.Phase, subjectID string, hash []byte) int {
	s.mu.RLock()
	defer s.mu.RUnlock()

	key := makeKey(view, phase, subjectID, hash)
	inst, exists := s.instances[key]
	if !exists {
		return 0
	}
	return len(inst.ReadySenders)
}

// MarkSeenSend marks that we've seen the SEND message
func (s *RBCStore) MarkSeenSend(view uint64, phase bftconsensus.Phase, subjectID string, hash []byte) bool {
	inst := s.GetOrCreate(view, phase, subjectID, hash)

	s.mu.Lock()
	defer s.mu.Unlock()

	if inst.SeenSend {
		return false // Already seen
	}
	inst.SeenSend = true
	return true
}

// HasSeenSend checks if we've seen SEND
func (s *RBCStore) HasSeenSend(view uint64, phase bftconsensus.Phase, subjectID string, hash []byte) bool {
	s.mu.RLock()
	defer s.mu.RUnlock()

	key := makeKey(view, phase, subjectID, hash)
	inst, exists := s.instances[key]
	if !exists {
		return false
	}
	return inst.SeenSend
}

// MarkSentReady marks that we've sent READY
func (s *RBCStore) MarkSentReady(view uint64, phase bftconsensus.Phase, subjectID string, hash []byte) bool {
	inst := s.GetOrCreate(view, phase, subjectID, hash)

	s.mu.Lock()
	defer s.mu.Unlock()

	if inst.SentReady {
		return false // Already sent
	}
	inst.SentReady = true
	return true
}

// HasSentReady checks if we've sent READY
func (s *RBCStore) HasSentReady(view uint64, phase bftconsensus.Phase, subjectID string, hash []byte) bool {
	s.mu.RLock()
	defer s.mu.RUnlock()

	key := makeKey(view, phase, subjectID, hash)
	inst, exists := s.instances[key]
	if !exists {
		return false
	}
	return inst.SentReady
}

// MarkDelivered marks the instance as delivered
func (s *RBCStore) MarkDelivered(view uint64, phase bftconsensus.Phase, subjectID string, hash []byte) bool {
	inst := s.GetOrCreate(view, phase, subjectID, hash)

	s.mu.Lock()
	defer s.mu.Unlock()

	if inst.Delivered {
		return false // Already delivered
	}
	inst.Delivered = true
	return true
}

// IsDelivered checks if the instance has been delivered
func (s *RBCStore) IsDelivered(view uint64, phase bftconsensus.Phase, subjectID string, hash []byte) bool {
	s.mu.RLock()
	defer s.mu.RUnlock()

	key := makeKey(view, phase, subjectID, hash)
	inst, exists := s.instances[key]
	if !exists {
		return false
	}
	return inst.Delivered
}

// SetPayload stores the payload for an instance
func (s *RBCStore) SetPayload(view uint64, phase bftconsensus.Phase, subjectID string, hash []byte, payload interface{}) {
	inst := s.GetOrCreate(view, phase, subjectID, hash)

	s.mu.Lock()
	defer s.mu.Unlock()

	inst.Payload = payload
}

// GetPayload retrieves the payload for an instance
func (s *RBCStore) GetPayload(view uint64, phase bftconsensus.Phase, subjectID string, hash []byte) interface{} {
	s.mu.RLock()
	defer s.mu.RUnlock()

	key := makeKey(view, phase, subjectID, hash)
	inst, exists := s.instances[key]
	if !exists {
		return nil
	}
	return inst.Payload
}

// GetReadyAttestations collects attestations from READY messages
func (s *RBCStore) GetReadyAttestations(view uint64, phase bftconsensus.Phase, subjectID string, hash []byte) []*bftconsensus.Attestation {
	s.mu.RLock()
	defer s.mu.RUnlock()

	key := makeKey(view, phase, subjectID, hash)
	_, exists := s.instances[key]
	if !exists {
		return nil
	}

	// Note: In a real implementation, we'd store the actual attestations
	// For now, this is a placeholder - attestations should be stored separately
	return nil
}

// Reset clears all instances (for view change)
func (s *RBCStore) Reset() {
	s.mu.Lock()
	defer s.mu.Unlock()

	s.instances = make(map[string]*RBCInstance)
}

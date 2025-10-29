package rbc

import (
	"github.com/ymalegao/DistributedSystemsforAVs/pb/bftconsensus"
)

// MessageType distinguishes SEND/ECHO/READY
type MessageType int

const (
	MessageTypeSend MessageType = iota
	MessageTypeEcho
	MessageTypeReady
)

// RBCKey uniquely identifies an RBC instance
type RBCKey struct {
	View      uint64
	Phase     bftconsensus.Phase
	SubjectID string
}

// RBCInstance tracks state for a single RBC instance
type RBCInstance struct {
	Key         RBCKey
	PayloadHash []byte
	Payload     interface{} // *bftconsensus.BFTVehicleMeta for Phase1, or combined for Phase2

	// Deduplication
	SeenSend  bool
	SentReady bool
	Delivered bool

	// Counters: map[senderID]bool for distinct senders
	EchoSenders  map[string]bool
	ReadySenders map[string]bool
}

// NewRBCInstance creates a new RBC instance
func NewRBCInstance(view uint64, phase bftconsensus.Phase, subjectID string) *RBCInstance {
	return &RBCInstance{
		Key: RBCKey{
			View:      view,
			Phase:     phase,
			SubjectID: subjectID,
		},
		EchoSenders:  make(map[string]bool),
		ReadySenders: make(map[string]bool),
	}
}

// RBCResult represents the outcome of RBC delivery
type RBCResult struct {
	QC      *bftconsensus.QuorumCert
	Payload interface{}
}

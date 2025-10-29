package byz

import (
	"fmt"
	"log"
	"math/rand"
	"time"

	"github.com/ymalegao/DistributedSystemsforAVs/pb/bftconsensus"
)

// Key identifies a unique RBC instance
type Key struct {
	View      uint64
	Phase     bftconsensus.Phase
	SubjectID string
	Hash      string
}

// ByzBehavior defines how a Byzantine node behaves
type ByzBehavior interface {
	// Name returns the behavior name for logging
	Name() string

	// OnSendVote is called when sending SEND_VOTE messages
	// Returns modified messages (can return multiple for equivocation) and whether to drop
	OnSendVote(msg *bftconsensus.SendVote, peerIDs []string) ([]*MessageTarget, bool)

	// OnEchoVote is called when sending ECHO_VOTE messages
	OnEchoVote(msg *bftconsensus.EchoVote, peerIDs []string) ([]*MessageTarget, bool)

	// OnReadyVote is called when sending READY_VOTE messages
	OnReadyVote(msg *bftconsensus.ReadyVote, peerIDs []string) ([]*MessageTarget, bool)

	// OnSendLeader is called when sending SEND_LEADER messages
	OnSendLeader(msg *bftconsensus.SendLeader, peerIDs []string) ([]*MessageTarget, bool)

	// OnEchoLeader is called when sending ECHO_LEADER messages
	OnEchoLeader(msg *bftconsensus.EchoLeader, peerIDs []string) ([]*MessageTarget, bool)

	// OnReadyLeader is called when sending READY_LEADER messages
	OnReadyLeader(msg *bftconsensus.ReadyLeader, peerIDs []string) ([]*MessageTarget, bool)

	// PreHandle is called before processing a message
	PreHandle(kind string, key Key)

	// PostHandle is called after processing a message
	PostHandle(kind string, key Key)
}

// MessageTarget represents a message to be sent to specific peers
type MessageTarget struct {
	SendVote    *bftconsensus.SendVote
	EchoVote    *bftconsensus.EchoVote
	ReadyVote   *bftconsensus.ReadyVote
	SendLeader  *bftconsensus.SendLeader
	EchoLeader  *bftconsensus.EchoLeader
	ReadyLeader *bftconsensus.ReadyLeader
	PeerIDs     []string // If empty, send to all
}

// ============================================================================
// Honest Behavior (Baseline)
// ============================================================================

type HonestBehavior struct {
	NodeID string
}

func NewHonest(nodeID string) *HonestBehavior {
	return &HonestBehavior{NodeID: nodeID}
}

func (h *HonestBehavior) Name() string {
	return "Honest"
}

func (h *HonestBehavior) OnSendVote(msg *bftconsensus.SendVote, peerIDs []string) ([]*MessageTarget, bool) {
	return []*MessageTarget{{SendVote: msg, PeerIDs: peerIDs}}, false
}

func (h *HonestBehavior) OnEchoVote(msg *bftconsensus.EchoVote, peerIDs []string) ([]*MessageTarget, bool) {
	return []*MessageTarget{{EchoVote: msg, PeerIDs: peerIDs}}, false
}

func (h *HonestBehavior) OnReadyVote(msg *bftconsensus.ReadyVote, peerIDs []string) ([]*MessageTarget, bool) {
	return []*MessageTarget{{ReadyVote: msg, PeerIDs: peerIDs}}, false
}

func (h *HonestBehavior) OnSendLeader(msg *bftconsensus.SendLeader, peerIDs []string) ([]*MessageTarget, bool) {
	return []*MessageTarget{{SendLeader: msg, PeerIDs: peerIDs}}, false
}

func (h *HonestBehavior) OnEchoLeader(msg *bftconsensus.EchoLeader, peerIDs []string) ([]*MessageTarget, bool) {
	return []*MessageTarget{{EchoLeader: msg, PeerIDs: peerIDs}}, false
}

func (h *HonestBehavior) OnReadyLeader(msg *bftconsensus.ReadyLeader, peerIDs []string) ([]*MessageTarget, bool) {
	return []*MessageTarget{{ReadyLeader: msg, PeerIDs: peerIDs}}, false
}

func (h *HonestBehavior) PreHandle(kind string, key Key) {}

func (h *HonestBehavior) PostHandle(kind string, key Key) {}

// ============================================================================
// EquivocateSend: Send different payloads to different subsets
// ============================================================================

type EquivocateSendBehavior struct {
	NodeID      string
	equivocated map[string]bool // track which (view,phase,subject) we've equivocated
}

func NewEquivocateSend(nodeID string) *EquivocateSendBehavior {
	return &EquivocateSendBehavior{
		NodeID:      nodeID,
		equivocated: make(map[string]bool),
	}
}

func (e *EquivocateSendBehavior) Name() string {
	return "EquivocateSend"
}

func (e *EquivocateSendBehavior) OnSendVote(msg *bftconsensus.SendVote, peerIDs []string) ([]*MessageTarget, bool) {
	key := fmt.Sprintf("%d:%d:%s", msg.View, bftconsensus.Phase_PHASE1_VOTE, msg.SubjectId)

	// Only equivocate once per (view, phase, subject)
	if e.equivocated[key] {
		return []*MessageTarget{{SendVote: msg, PeerIDs: peerIDs}}, false
	}
	e.equivocated[key] = true

	// Split peers into two disjoint subsets
	mid := len(peerIDs) / 2
	subset1 := peerIDs[:mid]
	subset2 := peerIDs[mid:]

	if len(subset1) == 0 || len(subset2) == 0 {
		// Can't split, send normally
		return []*MessageTarget{{SendVote: msg, PeerIDs: peerIDs}}, false
	}

	log.Printf("[%s] ⚠️  EQUIVOCATING: Sending different SEND_VOTEs to %d and %d peers",
		e.NodeID, len(subset1), len(subset2))

	// Message 1: Original
	msg1 := msg

	// Message 2: Modified payload (change direction to make hash different)
	msg2 := &bftconsensus.SendVote{
		View:      msg.View,
		SubjectId: msg.SubjectId,
		Payload: &bftconsensus.BFTVehicleMeta{
			Id:             msg.Payload.Id,
			Plate:          msg.Payload.Plate,
			Direction:      msg.Payload.Direction + "-EQUIVOCATED", // Make it different
			Status:         msg.Payload.Status,
			SentVotes:      msg.Payload.SentVotes,
			ReceivedVotes:  msg.Payload.ReceivedVotes,
			ElectionTimeMs: msg.Payload.ElectionTimeMs,
		},
		SenderAtt: msg.SenderAtt, // Keep same attestation (malicious!)
	}

	return []*MessageTarget{
		{SendVote: msg1, PeerIDs: subset1},
		{SendVote: msg2, PeerIDs: subset2},
	}, false
}

func (e *EquivocateSendBehavior) OnEchoVote(msg *bftconsensus.EchoVote, peerIDs []string) ([]*MessageTarget, bool) {
	return []*MessageTarget{{EchoVote: msg, PeerIDs: peerIDs}}, false
}

func (e *EquivocateSendBehavior) OnReadyVote(msg *bftconsensus.ReadyVote, peerIDs []string) ([]*MessageTarget, bool) {
	return []*MessageTarget{{ReadyVote: msg, PeerIDs: peerIDs}}, false
}

func (e *EquivocateSendBehavior) OnSendLeader(msg *bftconsensus.SendLeader, peerIDs []string) ([]*MessageTarget, bool) {
	return []*MessageTarget{{SendLeader: msg, PeerIDs: peerIDs}}, false
}

func (e *EquivocateSendBehavior) OnEchoLeader(msg *bftconsensus.EchoLeader, peerIDs []string) ([]*MessageTarget, bool) {
	return []*MessageTarget{{EchoLeader: msg, PeerIDs: peerIDs}}, false
}

func (e *EquivocateSendBehavior) OnReadyLeader(msg *bftconsensus.ReadyLeader, peerIDs []string) ([]*MessageTarget, bool) {
	return []*MessageTarget{{ReadyLeader: msg, PeerIDs: peerIDs}}, false
}

func (e *EquivocateSendBehavior) PreHandle(kind string, key Key) {}

func (e *EquivocateSendBehavior) PostHandle(kind string, key Key) {}

// ============================================================================
// DoubleVote: ECHO/READY twice for conflicting payload_hash
// ============================================================================

type DoubleVoteBehavior struct {
	NodeID string
	voted  map[string][]byte // key -> first hash we voted for
	rng    *rand.Rand
}

func NewDoubleVote(nodeID string) *DoubleVoteBehavior {
	return &DoubleVoteBehavior{
		NodeID: nodeID,
		voted:  make(map[string][]byte),
		rng:    rand.New(rand.NewSource(rand.Int63())),
	}
}

func (d *DoubleVoteBehavior) Name() string {
	return "DoubleVote"
}

func (d *DoubleVoteBehavior) OnSendVote(msg *bftconsensus.SendVote, peerIDs []string) ([]*MessageTarget, bool) {
	return []*MessageTarget{{SendVote: msg, PeerIDs: peerIDs}}, false
}

func (d *DoubleVoteBehavior) OnEchoVote(msg *bftconsensus.EchoVote, peerIDs []string) ([]*MessageTarget, bool) {
	key := fmt.Sprintf("echo:%d:%d:%s", msg.View, msg.Att.Phase, msg.SubjectId)

	firstHash, exists := d.voted[key]
	if !exists {
		// First vote - remember it
		d.voted[key] = msg.PayloadHash
		return []*MessageTarget{{EchoVote: msg, PeerIDs: peerIDs}}, false
	}

	// Already voted - check if same hash
	if string(firstHash) == string(msg.PayloadHash) {
		return []*MessageTarget{{EchoVote: msg, PeerIDs: peerIDs}}, false
	}

	// Different hash! Double vote
	log.Printf("[%s] ⚠️  DOUBLE VOTING (ECHO): Voting for conflicting hash", d.NodeID)

	// Send both votes to different subsets
	mid := len(peerIDs) / 2
	if mid == 0 {
		mid = 1
	}

	// Original vote
	msg1 := &bftconsensus.EchoVote{
		View:        msg.View,
		SubjectId:   msg.SubjectId,
		PayloadHash: firstHash,
		Att:         msg.Att,
	}

	// Conflicting vote
	msg2 := msg

	return []*MessageTarget{
		{EchoVote: msg1, PeerIDs: peerIDs[:mid]},
		{EchoVote: msg2, PeerIDs: peerIDs[mid:]},
	}, false
}

func (d *DoubleVoteBehavior) OnReadyVote(msg *bftconsensus.ReadyVote, peerIDs []string) ([]*MessageTarget, bool) {
	key := fmt.Sprintf("ready:%d:%d:%s", msg.View, msg.Att.Phase, msg.SubjectId)

	firstHash, exists := d.voted[key]
	if !exists {
		// First vote - remember it
		d.voted[key] = msg.PayloadHash
		return []*MessageTarget{{ReadyVote: msg, PeerIDs: peerIDs}}, false
	}

	// Already voted - check if same hash
	if string(firstHash) == string(msg.PayloadHash) {
		return []*MessageTarget{{ReadyVote: msg, PeerIDs: peerIDs}}, false
	}

	// Different hash! Double vote
	log.Printf("[%s] ⚠️  DOUBLE VOTING (READY): Voting for conflicting hash", d.NodeID)

	// Send both votes to different subsets
	mid := len(peerIDs) / 2
	if mid == 0 {
		mid = 1
	}

	// Original vote
	msg1 := &bftconsensus.ReadyVote{
		View:        msg.View,
		SubjectId:   msg.SubjectId,
		PayloadHash: firstHash,
		Att:         msg.Att,
	}

	// Conflicting vote
	msg2 := msg

	return []*MessageTarget{
		{ReadyVote: msg1, PeerIDs: peerIDs[:mid]},
		{ReadyVote: msg2, PeerIDs: peerIDs[mid:]},
	}, false
}

func (d *DoubleVoteBehavior) OnSendLeader(msg *bftconsensus.SendLeader, peerIDs []string) ([]*MessageTarget, bool) {
	return []*MessageTarget{{SendLeader: msg, PeerIDs: peerIDs}}, false
}

func (d *DoubleVoteBehavior) OnEchoLeader(msg *bftconsensus.EchoLeader, peerIDs []string) ([]*MessageTarget, bool) {
	return []*MessageTarget{{EchoLeader: msg, PeerIDs: peerIDs}}, false
}

func (d *DoubleVoteBehavior) OnReadyLeader(msg *bftconsensus.ReadyLeader, peerIDs []string) ([]*MessageTarget, bool) {
	return []*MessageTarget{{ReadyLeader: msg, PeerIDs: peerIDs}}, false
}

func (d *DoubleVoteBehavior) PreHandle(kind string, key Key) {}

func (d *DoubleVoteBehavior) PostHandle(kind string, key Key) {}

// ============================================================================
// WithholdReady: ECHO but never READY (tests liveness margins)
// ============================================================================

type WithholdReadyBehavior struct {
	NodeID string
}

func NewWithholdReady(nodeID string) *WithholdReadyBehavior {
	return &WithholdReadyBehavior{NodeID: nodeID}
}

func (w *WithholdReadyBehavior) Name() string {
	return "WithholdReady"
}

func (w *WithholdReadyBehavior) OnSendVote(msg *bftconsensus.SendVote, peerIDs []string) ([]*MessageTarget, bool) {
	return []*MessageTarget{{SendVote: msg, PeerIDs: peerIDs}}, false
}

func (w *WithholdReadyBehavior) OnEchoVote(msg *bftconsensus.EchoVote, peerIDs []string) ([]*MessageTarget, bool) {
	return []*MessageTarget{{EchoVote: msg, PeerIDs: peerIDs}}, false
}

func (w *WithholdReadyBehavior) OnReadyVote(msg *bftconsensus.ReadyVote, peerIDs []string) ([]*MessageTarget, bool) {
	log.Printf("[%s] ⚠️  WITHHOLDING READY_VOTE", w.NodeID)
	return nil, true // Drop all READY messages
}

func (w *WithholdReadyBehavior) OnSendLeader(msg *bftconsensus.SendLeader, peerIDs []string) ([]*MessageTarget, bool) {
	return []*MessageTarget{{SendLeader: msg, PeerIDs: peerIDs}}, false
}

func (w *WithholdReadyBehavior) OnEchoLeader(msg *bftconsensus.EchoLeader, peerIDs []string) ([]*MessageTarget, bool) {
	return []*MessageTarget{{EchoLeader: msg, PeerIDs: peerIDs}}, false
}

func (w *WithholdReadyBehavior) OnReadyLeader(msg *bftconsensus.ReadyLeader, peerIDs []string) ([]*MessageTarget, bool) {
	log.Printf("[%s] ⚠️  WITHHOLDING READY_LEADER", w.NodeID)
	return nil, true // Drop all READY messages
}

func (w *WithholdReadyBehavior) PreHandle(kind string, key Key) {}

func (w *WithholdReadyBehavior) PostHandle(kind string, key Key) {}

// ============================================================================
// SelectiveForward: Forward to only a subset (simulate partial connectivity)
// ============================================================================

type SelectiveForwardBehavior struct {
	NodeID      string
	ForwardMask float64 // Fraction of peers to forward to (0.0 to 1.0)
	rng         *rand.Rand
}

func NewSelectiveForward(nodeID string, mask float64) *SelectiveForwardBehavior {
	return &SelectiveForwardBehavior{
		NodeID:      nodeID,
		ForwardMask: mask,
		rng:         rand.New(rand.NewSource(rand.Int63())),
	}
}

func (s *SelectiveForwardBehavior) Name() string {
	return fmt.Sprintf("SelectiveForward(%.1f%%)", s.ForwardMask*100)
}

func (s *SelectiveForwardBehavior) selectPeers(peerIDs []string) []string {
	numToSelect := int(float64(len(peerIDs)) * s.ForwardMask)
	if numToSelect == 0 && len(peerIDs) > 0 {
		numToSelect = 1 // Forward to at least one
	}

	// Randomly select subset
	selected := make([]string, numToSelect)
	perm := s.rng.Perm(len(peerIDs))
	for i := 0; i < numToSelect && i < len(peerIDs); i++ {
		selected[i] = peerIDs[perm[i]]
	}

	return selected
}

func (s *SelectiveForwardBehavior) OnSendVote(msg *bftconsensus.SendVote, peerIDs []string) ([]*MessageTarget, bool) {
	selected := s.selectPeers(peerIDs)
	log.Printf("[%s] ⚠️  SELECTIVE FORWARD: Sending to %d/%d peers", s.NodeID, len(selected), len(peerIDs))
	return []*MessageTarget{{SendVote: msg, PeerIDs: selected}}, false
}

func (s *SelectiveForwardBehavior) OnEchoVote(msg *bftconsensus.EchoVote, peerIDs []string) ([]*MessageTarget, bool) {
	selected := s.selectPeers(peerIDs)
	return []*MessageTarget{{EchoVote: msg, PeerIDs: selected}}, false
}

func (s *SelectiveForwardBehavior) OnReadyVote(msg *bftconsensus.ReadyVote, peerIDs []string) ([]*MessageTarget, bool) {
	selected := s.selectPeers(peerIDs)
	return []*MessageTarget{{ReadyVote: msg, PeerIDs: selected}}, false
}

func (s *SelectiveForwardBehavior) OnSendLeader(msg *bftconsensus.SendLeader, peerIDs []string) ([]*MessageTarget, bool) {
	selected := s.selectPeers(peerIDs)
	return []*MessageTarget{{SendLeader: msg, PeerIDs: selected}}, false
}

func (s *SelectiveForwardBehavior) OnEchoLeader(msg *bftconsensus.EchoLeader, peerIDs []string) ([]*MessageTarget, bool) {
	selected := s.selectPeers(peerIDs)
	return []*MessageTarget{{EchoLeader: msg, PeerIDs: selected}}, false
}

func (s *SelectiveForwardBehavior) OnReadyLeader(msg *bftconsensus.ReadyLeader, peerIDs []string) ([]*MessageTarget, bool) {
	selected := s.selectPeers(peerIDs)
	return []*MessageTarget{{ReadyLeader: msg, PeerIDs: selected}}, false
}

func (s *SelectiveForwardBehavior) PreHandle(kind string, key Key) {}

func (s *SelectiveForwardBehavior) PostHandle(kind string, key Key) {}

// ============================================================================
// Delay: Add artificial delays before sending (timing attacks)
// ============================================================================

type DelayBehavior struct {
	NodeID   string
	MinDelay time.Duration
	MaxDelay time.Duration
	rng      *rand.Rand
}

func NewDelay(nodeID string, minDelay, maxDelay time.Duration) *DelayBehavior {
	return &DelayBehavior{
		NodeID:   nodeID,
		MinDelay: minDelay,
		MaxDelay: maxDelay,
		rng:      rand.New(rand.NewSource(rand.Int63())),
	}
}

func (d *DelayBehavior) Name() string {
	return fmt.Sprintf("Delay(%v-%v)", d.MinDelay, d.MaxDelay)
}

func (d *DelayBehavior) addDelay() {
	delayRange := d.MaxDelay - d.MinDelay
	delay := d.MinDelay + time.Duration(d.rng.Int63n(int64(delayRange)))
	log.Printf("[%s] ⚠️  DELAYING message by %v", d.NodeID, delay)
	time.Sleep(delay)
}

func (d *DelayBehavior) OnSendVote(msg *bftconsensus.SendVote, peerIDs []string) ([]*MessageTarget, bool) {
	d.addDelay()
	return []*MessageTarget{{SendVote: msg, PeerIDs: peerIDs}}, false
}

func (d *DelayBehavior) OnEchoVote(msg *bftconsensus.EchoVote, peerIDs []string) ([]*MessageTarget, bool) {
	d.addDelay()
	return []*MessageTarget{{EchoVote: msg, PeerIDs: peerIDs}}, false
}

func (d *DelayBehavior) OnReadyVote(msg *bftconsensus.ReadyVote, peerIDs []string) ([]*MessageTarget, bool) {
	d.addDelay()
	return []*MessageTarget{{ReadyVote: msg, PeerIDs: peerIDs}}, false
}

func (d *DelayBehavior) OnSendLeader(msg *bftconsensus.SendLeader, peerIDs []string) ([]*MessageTarget, bool) {
	d.addDelay()
	return []*MessageTarget{{SendLeader: msg, PeerIDs: peerIDs}}, false
}

func (d *DelayBehavior) OnEchoLeader(msg *bftconsensus.EchoLeader, peerIDs []string) ([]*MessageTarget, bool) {
	d.addDelay()
	return []*MessageTarget{{EchoLeader: msg, PeerIDs: peerIDs}}, false
}

func (d *DelayBehavior) OnReadyLeader(msg *bftconsensus.ReadyLeader, peerIDs []string) ([]*MessageTarget, bool) {
	d.addDelay()
	return []*MessageTarget{{ReadyLeader: msg, PeerIDs: peerIDs}}, false
}

func (d *DelayBehavior) PreHandle(kind string, key Key) {}

func (d *DelayBehavior) PostHandle(kind string, key Key) {}

// ============================================================================
// BadSig: Corrupt signatures (will be rejected)
// ============================================================================

type BadSigBehavior struct {
	NodeID string
}

func NewBadSig(nodeID string) *BadSigBehavior {
	return &BadSigBehavior{NodeID: nodeID}
}

func (b *BadSigBehavior) Name() string {
	return "BadSig"
}

func (b *BadSigBehavior) corruptAttestation(att *bftconsensus.Attestation) *bftconsensus.Attestation {
	if att == nil {
		return att
	}

	// Corrupt the signature
	corruptedSig := make([]byte, len(att.Sig))
	copy(corruptedSig, att.Sig)
	if len(corruptedSig) > 0 {
		corruptedSig[0] ^= 0xFF // Flip bits
	}

	return &bftconsensus.Attestation{
		View:        att.View,
		Phase:       att.Phase,
		SubjectId:   att.SubjectId,
		PayloadHash: att.PayloadHash,
		SignerId:    att.SignerId,
		Sig:         corruptedSig,
	}
}

func (b *BadSigBehavior) OnSendVote(msg *bftconsensus.SendVote, peerIDs []string) ([]*MessageTarget, bool) {
	log.Printf("[%s] ⚠️  CORRUPTING signature in SEND_VOTE", b.NodeID)
	corrupted := &bftconsensus.SendVote{
		View:      msg.View,
		SubjectId: msg.SubjectId,
		Payload:   msg.Payload,
		SenderAtt: b.corruptAttestation(msg.SenderAtt),
	}
	return []*MessageTarget{{SendVote: corrupted, PeerIDs: peerIDs}}, false
}

func (b *BadSigBehavior) OnEchoVote(msg *bftconsensus.EchoVote, peerIDs []string) ([]*MessageTarget, bool) {
	corrupted := &bftconsensus.EchoVote{
		View:        msg.View,
		SubjectId:   msg.SubjectId,
		PayloadHash: msg.PayloadHash,
		Att:         b.corruptAttestation(msg.Att),
	}
	return []*MessageTarget{{EchoVote: corrupted, PeerIDs: peerIDs}}, false
}

func (b *BadSigBehavior) OnReadyVote(msg *bftconsensus.ReadyVote, peerIDs []string) ([]*MessageTarget, bool) {
	corrupted := &bftconsensus.ReadyVote{
		View:        msg.View,
		SubjectId:   msg.SubjectId,
		PayloadHash: msg.PayloadHash,
		Att:         b.corruptAttestation(msg.Att),
	}
	return []*MessageTarget{{ReadyVote: corrupted, PeerIDs: peerIDs}}, false
}

func (b *BadSigBehavior) OnSendLeader(msg *bftconsensus.SendLeader, peerIDs []string) ([]*MessageTarget, bool) {
	corrupted := &bftconsensus.SendLeader{
		View:      msg.View,
		SubjectId: msg.SubjectId,
		FinMeta:   msg.FinMeta,
		Qc1:       msg.Qc1,
		SenderAtt: b.corruptAttestation(msg.SenderAtt),
	}
	return []*MessageTarget{{SendLeader: corrupted, PeerIDs: peerIDs}}, false
}

func (b *BadSigBehavior) OnEchoLeader(msg *bftconsensus.EchoLeader, peerIDs []string) ([]*MessageTarget, bool) {
	corrupted := &bftconsensus.EchoLeader{
		View:        msg.View,
		SubjectId:   msg.SubjectId,
		PayloadHash: msg.PayloadHash,
		Att:         b.corruptAttestation(msg.Att),
	}
	return []*MessageTarget{{EchoLeader: corrupted, PeerIDs: peerIDs}}, false
}

func (b *BadSigBehavior) OnReadyLeader(msg *bftconsensus.ReadyLeader, peerIDs []string) ([]*MessageTarget, bool) {
	corrupted := &bftconsensus.ReadyLeader{
		View:        msg.View,
		SubjectId:   msg.SubjectId,
		PayloadHash: msg.PayloadHash,
		Att:         b.corruptAttestation(msg.Att),
	}
	return []*MessageTarget{{ReadyLeader: corrupted, PeerIDs: peerIDs}}, false
}

func (b *BadSigBehavior) PreHandle(kind string, key Key) {}

func (b *BadSigBehavior) PostHandle(kind string, key Key) {}

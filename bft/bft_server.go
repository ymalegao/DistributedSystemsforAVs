// BFT server implementation for RPC handlers
package bft

import (
	"context"
	"crypto/sha256"
	"encoding/binary"
	"fmt"
	"log"

	bftpb "github.com/ymalegao/DistributedSystemsforAVs/pb/bftconsensus"
)

// ----------------------
// Phase 1: Vote handlers
// ----------------------

// ReceiveSendVote handles SEND_VOTE messages in Phase 1
func (n *BFTNode) ReceiveSendVote(ctx context.Context, msg *bftpb.SendVote) (*bftpb.WireAck, error) {
	n.mu.Lock()
	defer n.mu.Unlock()

	V := msg.View
	s := msg.SubjectId
	payload := msg.Payload
	senderAtt := msg.SenderAtt

	// Check view match
	if V != n.view {
		return &bftpb.WireAck{Ok: false, Reason: "view mismatch"}, nil
	}

	// Already delivered
	if n.deliveredVote[V][s] {
		return &bftpb.WireAck{Ok: true}, nil
	}

	// Verify signature and already seen
	if !verifyAttestation(senderAtt) {
		return &bftpb.WireAck{Ok: false, Reason: "invalid signature"}, nil
	}

	// Initialize maps if needed
	if n.seenSendVote[V] == nil {
		n.seenSendVote[V] = make(map[string]bool)
	}
	if n.seenSendVote[V][s] {
		return &bftpb.WireAck{Ok: true}, nil
	}

	n.seenSendVote[V][s] = true

	// Validate payload
	if !validateMeta(payload) {
		return &bftpb.WireAck{Ok: false, Reason: "invalid payload"}, nil
	}

	// Check collision (no_collision check)
	noCollision := !collides(n.direction, payload.Direction)
	if !noCollision {
		return &bftpb.WireAck{Ok: false, Reason: "collision detected"}, nil
	}

	// Compute hash and broadcast ECHO_VOTE
	h := hashBytes([]byte(payload.Direction)) // simplified hash
	att := signAtt(n.id, V, bftpb.Phase_PHASE1_VOTE, s, h)

	// Broadcast echo asynchronously
	go n.broadcastEchoVote(V, s, string(h), att)

	return &bftpb.WireAck{Ok: true}, nil
}

// ReceiveEchoVote handles ECHO_VOTE messages
func (n *BFTNode) ReceiveEchoVote(ctx context.Context, msg *bftpb.EchoVote) (*bftpb.WireAck, error) {
	n.mu.Lock()
	defer n.mu.Unlock()

	V := msg.View
	s := msg.SubjectId
	h := string(msg.PayloadHash)
	senderAtt := msg.Att

	// Check view
	if V != n.view {
		return &bftpb.WireAck{Ok: true}, nil
	}

	// Verify attestation
	if !verifyAttestation(senderAtt) {
		return &bftpb.WireAck{Ok: true}, nil
	}

	u := senderAtt.SignerId
	if u == "" {
		return &bftpb.WireAck{Ok: true}, nil
	}

	// Initialize echo_count structure
	if n.echoCountVote[V][s] == nil {
		if n.echoCountVote[V] == nil {
			n.echoCountVote[V] = make(map[string]map[string]map[string]bool)
		}
		n.echoCountVote[V][s] = make(map[string]map[string]bool)
	}
	if n.echoCountVote[V][s][h] == nil {
		n.echoCountVote[V][s][h] = make(map[string]bool)
	}

	// Add sender to echo count
	n.echoCountVote[V][s][h][u] = true

	// Check if we should send READY (line 39-41)
	if len(n.echoCountVote[V][s][h]) >= n.f+1 {
		if n.sentReadyVote[V] == nil {
			n.sentReadyVote[V] = make(map[string]bool)
		}
		if !n.sentReadyVote[V][s] {
			n.sentReadyVote[V][s] = true
			// Broadcast READY asynchronously
			readyAtt := sign(n.id, V, bftpb.Phase_PHASE1_VOTE, s, msg.PayloadHash)
			go n.broadcastReadyVote(V, s, h, readyAtt)
		}
	}

	return &bftpb.WireAck{Ok: true}, nil
}

// ReceiveReadyVote handles READY_VOTE messages
func (n *BFTNode) ReceiveReadyVote(ctx context.Context, msg *bftpb.ReadyVote) (*bftpb.WireAck, error) {
	n.mu.Lock()
	defer n.mu.Unlock()

	V := msg.View
	s := msg.SubjectId
	h := string(msg.PayloadHash)
	senderAtt := msg.Att

	// Check view
	if V != n.view {
		return &bftpb.WireAck{Ok: true}, nil
	}

	// Verify attestation
	if !verifyAttestation(senderAtt) {
		return &bftpb.WireAck{Ok: true}, nil
	}

	u := senderAtt.SignerId
	if u == "" {
		return &bftpb.WireAck{Ok: true}, nil
	}

	// Initialize ready_count structure
	if n.readyCountVote[V][s] == nil {
		if n.readyCountVote[V] == nil {
			n.readyCountVote[V] = make(map[string]map[string]map[string]bool)
		}
		n.readyCountVote[V][s] = make(map[string]map[string]bool)
	}
	if n.readyCountVote[V][s][h] == nil {
		n.readyCountVote[V][s][h] = make(map[string]bool)
	}

	// Amplification rule (line 45-47)
	if len(n.readyCountVote[V][s][h]) == n.f && !n.sentReadyVote[V][s] {
		n.sentReadyVote[V][s] = true
		// Broadcast READY asynchronously
		readyAtt := signAtt(n.id, V, bftpb.Phase_PHASE1_VOTE, s, msg.PayloadHash)
		go n.broadcastReadyVote(V, s, h, readyAtt)
	}

	// Add sender to ready count
	n.readyCountVote[V][s][h][u] = true

	// Check for delivery (line 50-54)
	if len(n.readyCountVote[V][s][h]) >= 2*n.f+1 && !n.deliveredVote[V][s] {
		n.deliveredVote[V][s] = true
		n.lockedSubject[V] = s

		// Create QC1
		atts := []*bftpb.Attestation{senderAtt} // Simplified - collect actual attestations
		n.QC1 = &bftpb.QuorumCert{
			View:        V,
			Phase:       bftpb.Phase_PHASE1_VOTE,
			SubjectId:   s,
			PayloadHash: msg.PayloadHash,
			Sigs:        atts,
		}

		// Update status
		if s == n.id {
			n.status = bftpb.ElectionStatus_FIN_CANDIDATE
		} else {
			n.status = bftpb.ElectionStatus_FOLLOWER
		}

		log.Printf("[%s] QC1 delivered for subject %s", n.id, s)
	}

	return &bftpb.WireAck{Ok: true}, nil
}

// ----------------------
// Phase 2: Leader handlers
// ----------------------

// ReceiveSendLeader handles SEND_LEADER messages in Phase 2
func (n *BFTNode) ReceiveSendLeader(ctx context.Context, msg *bftpb.SendLeader) (*bftpb.WireAck, error) {
	n.mu.Lock()
	defer n.mu.Unlock()

	V := msg.View
	s := msg.SubjectId
	finMeta := msg.FinMeta
	qc1 := msg.Qc1
	senderAtt := msg.SenderAtt

	// Check view
	if V != n.view {
		return &bftpb.WireAck{Ok: false, Reason: "view mismatch"}, nil
	}

	// Already delivered
	if n.deliveredLeader[V][s] {
		return &bftpb.WireAck{Ok: true}, nil
	}

	// Verify attestation
	if !verifyAttestation(senderAtt) {
		return &bftpb.WireAck{Ok: false, Reason: "invalid signature"}, nil
	}

	// Initialize maps if needed
	if n.seenSendLeader[V] == nil {
		n.seenSendLeader[V] = make(map[string]bool)
	}
	if n.seenSendLeader[V][s] {
		return &bftpb.WireAck{Ok: true}, nil
	}

	n.seenSendLeader[V][s] = true

	// Verify QC1 and validate meta
	if !verifyQC(qc1) {
		return &bftpb.WireAck{Ok: false, Reason: "invalid QC1"}, nil
	}
	if !validateMeta(finMeta) {
		return &bftpb.WireAck{Ok: false, Reason: "invalid meta"}, nil
	}

	// Check collision
	noCollision := !collides(n.direction, finMeta.Direction)
	if !noCollision {
		return &bftpb.WireAck{Ok: false, Reason: "collision detected"}, nil
	}

	// Compute hash and broadcast ECHO_LEADER
	payload2 := append([]byte(finMeta.Direction), []byte(fmt.Sprintf("%d", qc1.View))...)
	h2 := hashBytes(payload2)
	att := signAtt(n.id, V, bftpb.Phase_PHASE2_LEADER, s, h2)

	// Broadcast echo asynchronously
	go n.broadcastEchoLeader(V, s, string(h2), att)

	return &bftpb.WireAck{Ok: true}, nil
}

// ReceiveEchoLeader handles ECHO_LEADER messages
func (n *BFTNode) ReceiveEchoLeader(ctx context.Context, msg *bftpb.EchoLeader) (*bftpb.WireAck, error) {
	n.mu.Lock()
	defer n.mu.Unlock()

	V := msg.View
	s := msg.SubjectId
	h2 := string(msg.PayloadHash)
	senderAtt := msg.Att

	// Check view
	if V != n.view {
		return &bftpb.WireAck{Ok: true}, nil
	}

	// Verify attestation
	if !verifyAttestation(senderAtt) {
		return &bftpb.WireAck{Ok: true}, nil
	}

	u := senderAtt.SignerId
	if u == "" {
		return &bftpb.WireAck{Ok: true}, nil
	}

	// Initialize echo_count structure
	if n.echoCountLeader[V][s] == nil {
		if n.echoCountLeader[V] == nil {
			n.echoCountLeader[V] = make(map[string]map[string]map[string]bool)
		}
		n.echoCountLeader[V][s] = make(map[string]map[string]bool)
	}
	if n.echoCountLeader[V][s][h2] == nil {
		n.echoCountLeader[V][s][h2] = make(map[string]bool)
	}

	// Add sender to echo count
	n.echoCountLeader[V][s][h2][u] = true

	// Check if we should send READY (line 79-81)
	if len(n.echoCountLeader[V][s][h2]) >= n.n-n.f {
		if n.sentReadyLeader[V] == nil {
			n.sentReadyLeader[V] = make(map[string]bool)
		}
		if !n.sentReadyLeader[V][s] {
			n.sentReadyLeader[V][s] = true
			// Broadcast READY asynchronously
			readyAtt := sign(n.id, V, bftpb.Phase_PHASE2_LEADER, s, msg.PayloadHash)
			go n.broadcastReadyLeader(V, s, h2, readyAtt)
		}
	}

	return &bftpb.WireAck{Ok: true}, nil
}

// ReceiveReadyLeader handles READY_LEADER messages
func (n *BFTNode) ReceiveReadyLeader(ctx context.Context, msg *bftpb.ReadyLeader) (*bftpb.WireAck, error) {
	n.mu.Lock()
	defer n.mu.Unlock()

	V := msg.View
	s := msg.SubjectId
	h2 := string(msg.PayloadHash)
	senderAtt := msg.Att

	// Check view
	if V != n.view {
		return &bftpb.WireAck{Ok: true}, nil
	}

	// Verify attestation
	if !verifyAttestation(senderAtt) {
		return &bftpb.WireAck{Ok: true}, nil
	}

	u := senderAtt.SignerId
	if u == "" {
		return &bftpb.WireAck{Ok: true}, nil
	}

	// Initialize ready_count structure
	if n.readyCountLeader[V][s] == nil {
		if n.readyCountLeader[V] == nil {
			n.readyCountLeader[V] = make(map[string]map[string]map[string]bool)
		}
		n.readyCountLeader[V][s] = make(map[string]map[string]bool)
	}
	if n.readyCountLeader[V][s][h2] == nil {
		n.readyCountLeader[V][s][h2] = make(map[string]bool)
	}

	// Amplification rule (line 84-86)
	if len(n.readyCountLeader[V][s][h2]) == n.f && !n.sentReadyLeader[V][s] {
		n.sentReadyLeader[V][s] = true
		// Broadcast READY asynchronously
		readyAtt := signAtt(n.id, V, bftpb.Phase_PHASE2_LEADER, s, msg.PayloadHash)
		go n.broadcastReadyLeader(V, s, h2, readyAtt)
	}

	// Add sender to ready count
	n.readyCountLeader[V][s][h2][u] = true

	// Check for delivery (line 89-93)
	if len(n.readyCountLeader[V][s][h2]) >= 2*n.f+1 && !n.deliveredLeader[V][s] {
		n.deliveredLeader[V][s] = true
		n.lockedSubject[V] = s

		// Create QC2
		atts := []*bftpb.Attestation{senderAtt} // Simplified - collect actual attestations
		n.QC2 = &bftpb.QuorumCert{
			View:        V,
			Phase:       bftpb.Phase_PHASE2_LEADER,
			SubjectId:   s,
			PayloadHash: msg.PayloadHash,
			Sigs:        atts,
		}

		// Update status
		if s == n.id {
			n.status = bftpb.ElectionStatus_LEADER
		} else {
			n.status = bftpb.ElectionStatus_FOLLOWER
		}

		log.Printf("[%s] QC2 delivered for subject %s", n.id, s)
	}

	return &bftpb.WireAck{Ok: true}, nil
}

// ----------------------
// Helper functions
// ----------------------

func verifyQC(qc *bftpb.QuorumCert) bool {
	if qc == nil {
		return false
	}
	// Verify quorum certificate
	// In production, verify actual signatures
	return len(qc.Sigs) >= 2
}

func collides(dir1, dir2 string) bool {
	return dir1 == dir2 || hasLeft(dir1) || hasLeft(dir2)
}

// Broadcast functions - send messages to all peers
func (n *BFTNode) broadcastEchoVote(V uint64, s, h string, att *bftpb.Attestation) {
	log.Printf("[%s] Broadcasting ECHO_VOTE for subject %s", n.id, s)

	msg := &bftpb.EchoVote{
		View:        V,
		SubjectId:   s,
		PayloadHash: []byte(h),
		Att:         att,
	}

	n.broadcastToPeers(func(peer bftpb.BFTConsensusUnaryClient) {
		_, _ = peer.ReceiveEchoVote(context.Background(), msg)
	})
}

func (n *BFTNode) broadcastReadyVote(V uint64, s, h string, att *bftpb.Attestation) {
	log.Printf("[%s] Broadcasting READY_VOTE for subject %s", n.id, s)

	msg := &bftpb.ReadyVote{
		View:        V,
		SubjectId:   s,
		PayloadHash: []byte(h),
		Att:         att,
	}

	n.broadcastToPeers(func(peer bftpb.BFTConsensusUnaryClient) {
		_, _ = peer.ReceiveReadyVote(context.Background(), msg)
	})
}

func (n *BFTNode) broadcastEchoLeader(V uint64, s, h string, att *bftpb.Attestation) {
	log.Printf("[%s] Broadcasting ECHO_LEADER for subject %s", n.id, s)

	msg := &bftpb.EchoLeader{
		View:        V,
		SubjectId:   s,
		PayloadHash: []byte(h),
		Att:         att,
	}

	n.broadcastToPeers(func(peer bftpb.BFTConsensusUnaryClient) {
		_, _ = peer.ReceiveEchoLeader(context.Background(), msg)
	})
}

func (n *BFTNode) broadcastReadyLeader(V uint64, s, h string, att *bftpb.Attestation) {
	log.Printf("[%s] Broadcasting READY_LEADER for subject %s", n.id, s)

	msg := &bftpb.ReadyLeader{
		View:        V,
		SubjectId:   s,
		PayloadHash: []byte(h),
		Att:         att,
	}

	n.broadcastToPeers(func(peer bftpb.BFTConsensusUnaryClient) {
		_, _ = peer.ReceiveReadyLeader(context.Background(), msg)
	})
}

// broadcastToPeers is a helper that broadcasts to all peers concurrently
func (n *BFTNode) broadcastToPeers(sendFn func(bftpb.BFTConsensusUnaryClient)) {
	n.mu.Lock()
	peers := make(map[string]bftpb.BFTConsensusUnaryClient, len(n.peers))
	for k, v := range n.peers {
		peers[k] = v
	}
	n.mu.Unlock()

	for _, peer := range peers {
		go sendFn(peer)
	}
}

// Helper to compute hash from byte slice
func hashBytes(data []byte) []byte {
	h := sha256.New()
	h.Write(data)
	return h.Sum(nil)
}

// Helper to sign data
func signAtt(signerID string, view uint64, phase bftpb.Phase, subjectID string, payloadHash []byte) *bftpb.Attestation {
	sigData := make([]byte, 8+len(subjectID)+len(signerID)+len(payloadHash)+4)
	binary.LittleEndian.PutUint64(sigData[0:8], view)
	sigData[8] = byte(phase)
	copy(sigData[9:9+len(subjectID)], subjectID)
	copy(sigData[9+len(subjectID):], payloadHash)

	sig := hashBytes(sigData)

	return &bftpb.Attestation{
		View:        view,
		Phase:       phase,
		SubjectId:   subjectID,
		PayloadHash: payloadHash,
		SignerId:    signerID,
		Sig:         sig,
	}
}

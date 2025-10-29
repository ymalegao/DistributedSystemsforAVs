// BFT client implementation for broadcasting and gossiping
package bft

import (
	"context"
	"crypto/sha256"
	"encoding/binary"
	"fmt"
	"log"
	"sync"
	"time"

	bftpb "github.com/ymalegao/DistributedSystemsforAVs/pb/bftconsensus"
)

// hash is a helper function
func hash(data []byte) []byte {
	h := sha256.New()
	h.Write(data)
	return h.Sum(nil)
}

// sign is a helper function
func sign(signerID string, view uint64, phase bftpb.Phase, subjectID string, payloadHash []byte) *bftpb.Attestation {
	sigData := make([]byte, 8+len(subjectID)+len(signerID)+len(payloadHash)+4)
	binary.LittleEndian.PutUint64(sigData[0:8], view)
	sigData[8] = byte(phase)
	copy(sigData[9:9+len(subjectID)], subjectID)
	copy(sigData[9+len(subjectID):], payloadHash)

	sig := hash(sigData)

	return &bftpb.Attestation{
		View:        view,
		Phase:       phase,
		SubjectId:   subjectID,
		PayloadHash: payloadHash,
		SignerId:    signerID,
		Sig:         sig,
	}
}

// BroadcastSendVote broadcasts a SEND_VOTE message to all peers
func (n *BFTNode) BroadcastSendVote(ctx context.Context) error {
	n.mu.Lock()
	V := n.view
	myMeta := &bftpb.VehicleMeta{
		Id:        n.id,
		Plate:     n.plate,
		Direction: n.direction,
		Status:    n.status,
	}
	payloadHash := hash([]byte(myMeta.Direction))
	senderAtt := sign(n.id, V, bftpb.Phase_PHASE1_VOTE, n.id, payloadHash)
	peers := n.peers
	n.mu.Unlock()

	msg := &bftpb.SendVote{
		View:      V,
		SubjectId: n.id,
		Payload:   myMeta,
		SenderAtt: senderAtt,
	}

	// Broadcast to all peers
	var wg sync.WaitGroup
	for peerID, client := range peers {
		wg.Add(1)
		go func(id string, cli bftpb.BFTConsensusUnaryClient) {
			defer wg.Done()
			_, err := cli.ReceiveSendVote(ctx, msg)
			if err != nil {
				log.Printf("[%s] Failed to send SEND_VOTE to %s: %v", n.id, id, err)
			}
		}(peerID, client)
	}
	wg.Wait()

	return nil
}

// BroadcastSendLeader broadcasts a SEND_LEADER message to all peers
func (n *BFTNode) BroadcastSendLeader(ctx context.Context, qc1 *bftpb.QuorumCert) error {
	n.mu.Lock()
	V := n.view
	finMeta := &bftpb.VehicleMeta{
		Id:        n.id,
		Plate:     n.plate,
		Direction: n.direction,
		Status:    bftpb.ElectionStatus_FIN_CANDIDATE,
	}
	payload2 := append([]byte(finMeta.Direction), []byte(fmt.Sprintf("%d", qc1.View))...)
	h2 := hash(payload2)
	senderAtt := sign(n.id, V, bftpb.Phase_PHASE2_LEADER, n.id, h2)
	peers := n.peers
	n.mu.Unlock()

	msg := &bftpb.SendLeader{
		View:      V,
		SubjectId: n.id,
		FinMeta:   finMeta,
		Qc1:       qc1,
		SenderAtt: senderAtt,
	}

	// Broadcast to all peers
	var wg sync.WaitGroup
	for peerID, client := range peers {
		wg.Add(1)
		go func(id string, cli bftpb.BFTConsensusUnaryClient) {
			defer wg.Done()
			_, err := cli.ReceiveSendLeader(ctx, msg)
			if err != nil {
				log.Printf("[%s] Failed to send SEND_LEADER to %s: %v", n.id, id, err)
			}
		}(peerID, client)
	}
	wg.Wait()

	return nil
}

// RunBFTConsensus runs the full BFT consensus algorithm
func (n *BFTNode) RunBFTConsensus(ctx context.Context, timeout time.Duration) (*bftpb.ElectionStatus, error) {
	// Phase 1: Initiate election by broadcasting SEND_VOTE
	if err := n.BroadcastSendVote(ctx); err != nil {
		return nil, fmt.Errorf("failed to broadcast SEND_VOTE: %w", err)
	}

	// Wait for QC1
	start := time.Now()
	for {
		if time.Since(start) > timeout {
			return nil, fmt.Errorf("Phase 1 timeout")
		}

		n.mu.Lock()
		hasQC1 := n.QC1 != nil
		n.mu.Unlock()

		if hasQC1 {
			break
		}

		time.Sleep(10 * time.Millisecond)
	}

	n.mu.Lock()
	qc1 := n.QC1
	isFinCandidate := n.status == bftpb.ElectionStatus_FIN_CANDIDATE
	n.mu.Unlock()

	if !isFinCandidate {
		// We became a follower
		return nil, nil
	}

	// Phase 2: As FIN_CANDIDATE, broadcast SEND_LEADER
	if err := n.BroadcastSendLeader(ctx, qc1); err != nil {
		return nil, fmt.Errorf("failed to broadcast SEND_LEADER: %w", err)
	}

	// Wait for QC2
	start = time.Now()
	for {
		if time.Since(start) > timeout {
			return nil, fmt.Errorf("Phase 2 timeout")
		}

		n.mu.Lock()
		hasQC2 := n.QC2 != nil
		n.mu.Unlock()

		if hasQC2 {
			break
		}

		time.Sleep(10 * time.Millisecond)
	}

	n.mu.Lock()
	status := n.status
	n.mu.Unlock()

	if status == bftpb.ElectionStatus_LEADER {
		return &status, nil
	}

	return &status, nil
}

// Promotes the node to FIN_CANDIDATE status
func (n *BFTNode) PromoteToFinCandidate() {
	n.mu.Lock()
	defer n.mu.Unlock()
	if n.status == bftpb.ElectionStatus_INIT_CANDIDATE {
		n.status = bftpb.ElectionStatus_FIN_CANDIDATE
	}
}

// DialPeer is a wrapper around DialPeer with proper locking
func (n *BFTNode) AddPeer(peerID string, client bftpb.BFTConsensusUnaryClient) {
	n.mu.Lock()
	defer n.mu.Unlock()
	n.peers[peerID] = client
}

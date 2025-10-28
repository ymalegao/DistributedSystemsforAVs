// vehicle/client.go
//
// Client-side "broadcast + quorum tally" for a Vehicle node.
// This calls OTHER vehicles' CandidateVote and LeaderElection RPCs
// using the generated gRPC client stubs, and advances our local
// node state from INIT_CANDIDATE → FIN_CANDIDATE → LEADER.
//
// Pair this with vehicle/server.go (the RPC handlers).
//
// Imports assume your generated code lives at:
//   pb "github.com/ymalegao/DistributedSystemsforAVs/pb/consensus"

package vehicle

import (
	"context"
	"errors"
	"fmt"
	"sync"
	"time"

	"golang.org/x/sync/errgroup"
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"

	pb "github.com/ymalegao/DistributedSystemsforAVs/pb/consensus"
)

var (
	// ErrConsensusTimeout means we didn't reach quorum before the caller's deadline.
	ErrConsensusTimeout = errors.New("consensus deadline exceeded")
)

// DialPeer dials a peer once and stores both the connection and the generated client.
// You can call this during simulation wiring (one-time per peer).
func (n *Node) DialPeer(peerID, targetAddr string, dialTimeout time.Duration) error {
	ctx, cancel := context.WithTimeout(context.Background(), dialTimeout)
	defer cancel()

	// For demos: insecure. Use grpc.WithTransportCredentials for TLS in production.
	cc, err := grpc.DialContext(ctx, targetAddr, grpc.WithTransportCredentials(insecure.NewCredentials()), grpc.WithBlock())
	if err != nil {
		return fmt.Errorf("dial %s: %w", targetAddr, err)
	}

	n.mu.Lock()
	defer n.mu.Unlock()
	if n.peers == nil {
		n.peers = make(map[string]pb.IntersectionConsensusClient)
	}
	n.peers[peerID] = pb.NewIntersectionConsensusClient(cc)
	return nil
}

// ClosePeers: optional helper if you choose to track connections separately for cleanup.
// (In this minimal version we didn't store *grpc.ClientConn, only clients.)
//
// func (n *Node) ClosePeers() {
//     // store []*grpc.ClientConn if you want to close them explicitly
// }

// ------------------------------
// Phase 1: CandidateVote broadcast
// ------------------------------
//
// solicitCandidateVotes concurrently calls CandidateVote on all peers.
// It returns the number of ACKs and a set of peer IDs who reported "no collision"
// relative to THIS node (so we may permit them to pass in parallel later).
//
// IMPORTANT:
// - We count *our own* self-vote before broadcasting (paper: everyone self-votes).
// - We set our electionTimeMs at the time we receive our **first** ACK,
//   matching the paper's notion of "last response time recorded" for tie-breaks.
func (n *Node) solicitCandidateVotes(ctx context.Context, perRPC time.Duration) (acks uint32, noCollisionFrom map[string]bool) {
	noCollisionFrom = map[string]bool{}

	// Note: receivedVotes is already initialized to 1 (self-vote) in ResetForEpoch()

	var (
		firstAckOnce sync.Once
		mu           sync.Mutex
	)

	g, ctx := errgroup.WithContext(ctx)
	for peerID, cli := range n.snapshotPeers() {
		// don't call ourselves
		if peerID == n.id {
			continue
		}
		peer := cli

		g.Go(func() error {
			// Apply VANET network simulation (packet loss, latency)
			n.mu.Lock()
			vanetSim := n.vanetSim
			n.mu.Unlock()
			
			if vanetSim != nil {
				// Check for packet loss
				if vanetSim.SimulatePacketDrop() {
					// Packet lost - simulate network failure
					return nil
				}
				
				// Apply network latency
				latency := vanetSim.SimulateLatency()
				select {
				case <-time.After(latency):
					// Continue with RPC after latency
				case <-ctx.Done():
					return nil
				}
			}
			
			// per-RPC timeout; the outer ctx enforces the overall consensus deadline
			rpcCtx, cancel := context.WithTimeout(ctx, perRPC)
			defer cancel()

			resp, err := peer.CandidateVote(rpcCtx, &pb.CandidateVoteRequest{From: n.meta()})
			if err != nil {
				// treat as a non-response; async model allows loss/delay
				return nil
			}

			if resp.Decision == pb.VoteDecision_ACKNOWLEDGED {
				// Record time of *first* ACK for tie-breaks
				firstAckOnce.Do(func() {
					n.mu.Lock()
					n.electionTimeMs = time.Now().UnixMilli()
					n.mu.Unlock()
				})

				mu.Lock()
				acks++
				if resp.DirectionNoCollision {
					noCollisionFrom[resp.Responder.Id] = true
				}
				mu.Unlock()
			}
			return nil
		})
	}
	_ = g.Wait()

	// Update our vote tally with new ACKs.
	n.mu.Lock()
	n.receivedVotes += acks
	n.mu.Unlock()

	return acks, noCollisionFrom
}

// SolicitCandidateVotes is a public wrapper for solicitCandidateVotes
func (n *Node) SolicitCandidateVotes(ctx context.Context, perRPC time.Duration) (uint32, map[string]bool) {
	return n.solicitCandidateVotes(ctx, perRPC)
}

// ------------------------------
// Phase 2: LeaderElection broadcast
// ------------------------------
//
// runLeaderElection is invoked when we've become a FIN_CANDIDATE,
// and it seeks a majority of ACKs from peers based on (votes, electionTime).
// Returns the number of ACKs received. Caller decides if we reached quorum.
func (n *Node) runLeaderElection(ctx context.Context, perRPC time.Duration) (acks uint32) {
	var mu sync.Mutex

	g, ctx := errgroup.WithContext(ctx)
	for peerID, cli := range n.snapshotPeers() {
		if peerID == n.id {
			continue
		}
		peer := cli

		g.Go(func() error {
			// Apply VANET network simulation (packet loss, latency)
			n.mu.Lock()
			vanetSim := n.vanetSim
			n.mu.Unlock()
			
			if vanetSim != nil {
				// Check for packet loss
				if vanetSim.SimulatePacketDrop() {
					// Packet lost - simulate network failure
					return nil
				}
				
				// Apply network latency
				latency := vanetSim.SimulateLatency()
				select {
				case <-time.After(latency):
					// Continue with RPC after latency
				case <-ctx.Done():
					return nil
				}
			}
			
			rpcCtx, cancel := context.WithTimeout(ctx, perRPC)
			defer cancel()

			resp, err := peer.LeaderElection(rpcCtx, &pb.LeaderElectionRequest{FinCandidate: n.meta()})
			if err != nil {
				return nil // non-response is fine in async model
			}

			if resp.Decision == pb.VoteDecision_ACKNOWLEDGED {
				mu.Lock()
				acks++
				mu.Unlock()
			} else {
				// Optional: adopt responder's tally/time if they're actually ahead.
				// (Paper's "override" behavior—keeps monotonic progress.)
				n.maybeAdoptIfBehind(resp.Responder)
			}
			return nil
		})
	}
	_ = g.Wait()
	return acks
}

// RunLeaderElection is a public wrapper for runLeaderElection
func (n *Node) RunLeaderElection(ctx context.Context, perRPC time.Duration) uint32 {
	return n.runLeaderElection(ctx, perRPC)
}

// maybeAdoptIfBehind updates our local (receivedVotes, electionTimeMs)
// if the responder is strictly ahead by the paper's comparison rule.
func (n *Node) maybeAdoptIfBehind(other *pb.VehicleMeta) {
	n.mu.Lock()
	defer n.mu.Unlock()

	// Algorithm 2 comparison: other is ahead if:
	// other.receivedVotes > our.receivedVotes OR 
	// (other.receivedVotes == our.receivedVotes AND other.electionTime > our.electionTime)
	// Note: Later election-time is better (more recent/fresh candidate)
	otherAhead := other.ReceivedVotes > n.receivedVotes ||
		(other.ReceivedVotes == n.receivedVotes && other.ElectionTimeMs < n.electionTimeMs)

	if otherAhead {
		n.status = pb.ElectionStatus_FOLLOWER
		n.receivedVotes = other.ReceivedVotes
		n.electionTimeMs = other.ElectionTimeMs
		n.electionReceivedVotes = other.ReceivedVotes
	}
}

// snapshotPeers safely copies the peers map so we can iterate without holding the main lock.
func (n *Node) snapshotPeers() map[string]pb.IntersectionConsensusClient {
	n.mu.Lock()
	defer n.mu.Unlock()
	cp := make(map[string]pb.IntersectionConsensusClient, len(n.peers))
	for k, v := range n.peers {
		cp[k] = v
	}
	return cp
}

// ------------------------------
// High-level: RunElectionCycle
// ------------------------------
//
// RunElectionCycle runs both phases with deadlines and quorum checks.
//
// Usage:
//   ctx, cancel := context.WithTimeout(context.Background(), 500*time.Millisecond) // overall deadline
//   defer cancel()
//   res, err := node.RunElectionCycle(ctx, 100*time.Millisecond)
//   if errors.Is(err, ErrConsensusTimeout) { ...fallback to vision... }
//   if res.BecameLeader { ...pass through intersection... }
//
// Returns a small struct describing what happened.
type ElectionResult struct {
	// Phase 1:
	CandidateAcks      uint32
	NoCollisionFromIDs map[string]bool

	// Phase 2:
	LeaderAcks   uint32
	BecameLeader bool
}

func (n *Node) RunElectionCycle(ctx context.Context, perRPC time.Duration) (ElectionResult, error) {
	var out ElectionResult

	// ---- Phase 1: everyone starts as INIT_CANDIDATE with a self-vote
	// (Call ResetForEpoch() from your simulator before invoking this method if needed.)

	acks, noCol := n.solicitCandidateVotes(ctx, perRPC)
	out.CandidateAcks = acks
	out.NoCollisionFromIDs = noCol

	// quorum check: include self-vote already in n.receivedVotes
	n.mu.Lock()
	hasQuorum := int(n.receivedVotes) >= n.quorum
	n.mu.Unlock()

	if !hasQuorum {
		// We didn't gather enough ACKs in time; let caller decide to fallback to vision.
		if ctx.Err() != nil {
			return out, ErrConsensusTimeout // overall deadline hit
		}
		// No quorum yet but not timed out—return so the caller can re-try or wait.
		return out, nil
	}

	// Promote to Fin-Candidate and run Phase 2
	n.PromoteToFinCandidate()

	leaderAcks := n.runLeaderElection(ctx, perRPC)
	out.LeaderAcks = leaderAcks

	// Only become leader if we actually reached quorum in the candidate phase
	n.mu.Lock()
	// currentTally := n.receivedVotes
	hasLeader := int(leaderAcks)+1 >= n.quorum // Need quorum in candidate phase
	if hasLeader {
		n.status = pb.ElectionStatus_LEADER
		out.BecameLeader = true
	}
	n.mu.Unlock()

	// If context expired anywhere along the way, surface it.
	if ctx.Err() != nil && !out.BecameLeader {
		return out, ErrConsensusTimeout
	}
	return out, nil
}

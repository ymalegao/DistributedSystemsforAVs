// vehicle/server.go
//
// A minimal Vehicle server that implements the generated gRPC service
// (IntersectionConsensusServer). This file shows how to:
//   - embed the generated "Unimplemented" server base
//   - implement CandidateVote and LeaderElection
//   - protect election state with a mutex
//   - start/stop a gRPC server and register this service
//
// You will call these server methods FROM OTHER VEHICLES using the generated
// client stub (pb.NewIntersectionConsensusClient). That client-side code
// typically lives in your simulator or a separate "vehicle/client.go" file.

package vehicle

import (
	"context"
	"fmt"
	"log"
	"net"
	"sync"
	"time"

	"google.golang.org/grpc"

	pb "github.com/ymalegao/DistributedSystemsforAVs/pb/consensus"
)

// Node holds one vehicle's local state *and* the gRPC server.
// Each vehicle process will have one of these.
type Node struct {
	// Embed the generated base; if you forget to implement an RPC,
	// you'll get a helpful error instead of a nil panic.
	pb.UnimplementedIntersectionConsensusServer

	// Identity / configuration
	id        string
	plate     string
	direction string // e.g., "N-straight", "E-left"
	addr      string // "host:port" to listen on

	// ---- Election state (Algorithm Table II) ----
	//
	// These fields are mutated from RPC handlers, so we guard them
	// with mu (equivalent to "lock access to shared resource" in the paper).
	mu                    sync.Mutex
	status                pb.ElectionStatus // INIT_CANDIDATE, FIN_CANDIDATE, FOLLOWER, LEADER
	sentVotes             uint32            // 0 or 1: whether we've cast our single vote yet
	receivedVotes         uint32            // count of ACKs we've received (include self-vote)
	electionTimeMs        int64             // election-time: time when leader decision was made
	electionReceivedVotes uint32            // election-received-votes: votes during election process
	noCollisionSet        map[string]bool   // vehicle IDs that don't collide with us
	messageReceiveTimes   map[string]int64  // Tm->n: time when vehicle N received message from vehicle M

	// quorum size for current cohort (e.g., ceil(N/2))
	quorum int

	// ---- Server plumbing ----
	grpcSrv *grpc.Server
	lis     net.Listener

	// Optionally, keep a place to store peer clients (for client-side calls).
	// This file focuses on server; your simulator can hold the clients.
	peers map[string]pb.IntersectionConsensusClient

	// ---- Network simulation ----
	vanetSim *VANETSimulator // Optional network simulator for VANET characteristics
}

// NewNode constructs a vehicle node with initial state.
// You can change status later per-epoch (INIT_CANDIDATE etc).
func NewNode(id, plate, direction, addr string, quorum int) *Node {
	return &Node{
		id:                    id,
		plate:                 plate,
		direction:             direction,
		addr:                  addr,
		status:                pb.ElectionStatus_INIT_CANDIDATE,
		sentVotes:             0, // we haven't voted yet
		receivedVotes:         0, // we'll include self-vote when epoch starts
		electionTimeMs:        0, // will be set when we receive first ACK
		electionReceivedVotes: 0, // votes during election process
		noCollisionSet:        make(map[string]bool),
		messageReceiveTimes:   make(map[string]int64),
		quorum:                quorum,
		peers:                 make(map[string]pb.IntersectionConsensusClient),
	}
}

// Start brings up the gRPC server and registers this vehicle's service.
// Call this once per process.
func (n *Node) Start() error {
	lis, err := net.Listen("tcp", n.addr)
	if err != nil {
		return fmt.Errorf("listen %s: %w", n.addr, err)
	}
	n.lis = lis

	// For demos, insecure is fine on localhost; in production, configure TLS creds.
	n.grpcSrv = grpc.NewServer()

	// Register this Node as the server implementation for the generated service.
	pb.RegisterIntersectionConsensusServer(n.grpcSrv, n)

	// Serve in a goroutine so Start() returns.
	go func() {
		_ = n.grpcSrv.Serve(lis)
	}()
	return nil
}

// Stop stops the gRPC server and frees the port.
func (n *Node) Stop() {
	if n.grpcSrv != nil {
		n.grpcSrv.GracefulStop()
	}
	if n.lis != nil {
		_ = n.lis.Close()
	}
}

// ResetForEpoch is a helper your simulator can call to begin a new "simultaneous entry" cohort.
// It sets us to INIT_CANDIDATE and applies the self-vote (paper: everyone starts as candidate and self-votes).
func (n *Node) ResetForEpoch() {
	n.mu.Lock()
	defer n.mu.Unlock()
	n.status = pb.ElectionStatus_INIT_CANDIDATE
	n.sentVotes = 0             // we haven't voted for anyone else yet (Algorithm 1: sent-votes = 0 at epoch start)
	n.receivedVotes = 1         // start with self-vote (each vehicle starts as candidate and self-votes)
	n.electionTimeMs = 0        // will be set when we receive first ACK
	n.electionReceivedVotes = 0 // reset election votes
	n.noCollisionSet = make(map[string]bool)
	n.messageReceiveTimes = make(map[string]int64)
}

// AddPeerClient lets your simulator hand this node a client for another vehicle.
// This file keeps peers optional to emphasize "server", but it's convenient to store them here.
func (n *Node) AddPeerClient(peerID string, cli pb.IntersectionConsensusClient) {
	n.mu.Lock()
	defer n.mu.Unlock()
	n.peers[peerID] = cli
}

// meta builds our VehicleMeta message (the generated struct) for replies & broadcasts.
func (n *Node) meta() *pb.VehicleMeta {
	return &pb.VehicleMeta{
		Id:             n.id,
		Plate:          n.plate,
		Direction:      n.direction,
		Status:         n.status,
		SentVotes:      n.sentVotes,
		ReceivedVotes:  n.receivedVotes,
		ElectionTimeMs: n.electionTimeMs,
	}
}

// -----------------------
// RPC: CandidateVote (Algorithm 1)
// -----------------------
//
// Semantics (from the paper):
//   - Each vehicle may vote exactly once per term (first request wins).
//   - On first vote: mark that we've voted, set electionTimeMs=now,
//     and return ACK with direction_no_collision (responder vs requester).
//   - Otherwise, return IGNORED.
//
// This method is invoked by *other* vehicles' clients.
// We protect state with a mutex to avoid race conditions.
//
// NOTE: We do NOT increment our "receivedVotes" here: this handler is about
// casting *our* outgoing vote for the requester. The requester increments
// their own receivedVotes when they tally ACKs on the client side.
func (n *Node) CandidateVote(ctx context.Context, req *pb.CandidateVoteRequest) (*pb.CandidateVoteResponse, error) {
	n.mu.Lock()
	defer n.mu.Unlock()

	// Record message receive time (Tm->n)
	currentTime := time.Now().UnixMilli()
	n.messageReceiveTimes[req.From.Id] = currentTime

	// If we haven't voted this term, cast our ONLY vote for the requester.
	if n.sentVotes == 0 {
		n.sentVotes = 1
		// Don't set electionTimeMs here - it should be set when we receive ACKs

		noCollision := !collides(n.direction, req.From.Direction)

		// DEBUG: Log when we vote for someone
		log.Printf("    VOTE CAST: [%s] voted for [%s]", n.id, req.From.Id)

		// If noCollision == true, the requester will add *us* to its no-collision list.
		return &pb.CandidateVoteResponse{
			Decision:             pb.VoteDecision_ACKNOWLEDGED,
			DirectionNoCollision: noCollision,
			Responder:            n.meta(),
		}, nil
	}

	// Already voted earlier → ignore subsequent requests.
	// DEBUG: Log when we ignore someone
	log.Printf("    VOTE IGNORED: [%s] ignoring request from [%s] (already voted for someone else)", n.id, req.From.Id)

	return &pb.CandidateVoteResponse{
		Decision:  pb.VoteDecision_IGNORED,
		Responder: n.meta(),
	}, nil
}

// -----------------------
// RPC: LeaderElection (Algorithm 2)
// -----------------------
//
// Semantics (from the paper):
//   - A Fin-Candidate sends LeaderElection requests including its tally/time.
//   - The receiver compares (receivedVotes, electionTimeMs):
//   - If the sender is strictly AHEAD -> ACK and demote self to FOLLOWER,
//     adopting sender's tally/time.
//   - Otherwise -> IGNORE; the sender should demote itself when it sees
//     that it's behind (paper's "override" rule).
//   - If we're already FOLLOWER, we generally ignore new requests for the current term.
func (n *Node) LeaderElection(ctx context.Context, req *pb.LeaderElectionRequest) (*pb.LeaderElectionResponse, error) {
	n.mu.Lock()
	defer n.mu.Unlock()

	sender := req.FinCandidate

	// Record message receive time (Tm->n)
	currentTime := time.Now().UnixMilli()
	n.messageReceiveTimes[sender.Id] = currentTime

	// Algorithm 2: Line 1 - Check if requester is already a Follower
	if n.status == pb.ElectionStatus_FOLLOWER {
		return &pb.LeaderElectionResponse{
			Decision:  pb.VoteDecision_IGNORED,
			Responder: n.meta(),
		}, nil
	}

	// Algorithm 2: Line 4 - Check if sender is ahead
	// sender is ahead if: sender.receivedVotes > our.receivedVotes OR
	// (sender.receivedVotes == our.receivedVotes AND sender.electionTime > our.electionTime)
	// Note: Later election-time is better (more recent/fresh candidate)
	senderAhead := sender.ReceivedVotes > n.receivedVotes ||
		(sender.ReceivedVotes == n.receivedVotes && sender.ElectionTimeMs < n.electionTimeMs)

	if senderAhead {
		// Algorithm 2: Lines 5-7 - We're behind, become follower and adopt sender's info
		n.status = pb.ElectionStatus_FOLLOWER
		n.receivedVotes = sender.ReceivedVotes
		n.electionTimeMs = sender.ElectionTimeMs
		n.electionReceivedVotes = sender.ReceivedVotes

		return &pb.LeaderElectionResponse{
			Decision:  pb.VoteDecision_ACKNOWLEDGED,
			Responder: n.meta(),
		}, nil
	}

	// Algorithm 2: Lines 9-11 - We're ahead or equal, sender should become follower
	// Note: In a real implementation, we would need to send this info back to the sender
	// For now, we just ignore the request
	return &pb.LeaderElectionResponse{
		Decision:  pb.VoteDecision_IGNORED,
		Responder: n.meta(),
	}, nil
}

// -----------------------
// Helper: collision predicate
// -----------------------
//
// Replace this with your actual conflict-region logic. For now:
// - Return true if we conservatively assume a collision (forces sequencing).
// - Return false if the two paths cannot intersect (permits parallel passing).
func collides(dirA, dirB string) bool {
	// Example simplistic policy:
	// - If directions are identical, assume collision.
	// - If either includes "left", be conservative and assume collision.
	if dirA == dirB {
		return true
	}
	if hasLeft(dirA) || hasLeft(dirB) {
		return true
	}
	// You can extend with a map of conflict pairs for your intersection type.
	return false
}

func hasLeft(s string) bool {
	return contains(s, "left")
}

func contains(s, sub string) bool {
	return len(s) >= len(sub) && ( // micro-optim for tiny strings; use strings.Contains otherwise
	func() bool {
		for i := 0; i+len(sub) <= len(s); i++ {
			match := true
			for j := 0; j < len(sub); j++ {
				if s[i+j] != sub[j] {
					match = false
					break
				}
			}
			if match {
				return true
			}
		}
		return false
	}())
}

// -----------------------
// Public getters/setters (useful to integrate with simulator)
// -----------------------

func (n *Node) ID() string                 { return n.id }
func (n *Node) Addr() string               { return n.addr }
func (n *Node) Direction() string          { return n.direction }
func (n *Node) Status() pb.ElectionStatus  { n.mu.Lock(); defer n.mu.Unlock(); return n.status }
func (n *Node) ReceivedVotes() uint32      { n.mu.Lock(); defer n.mu.Unlock(); return n.receivedVotes }
func (n *Node) PeerCount() int             { n.mu.Lock(); defer n.mu.Unlock(); return len(n.peers) }
func (n *Node) UpdateQuorum(newQuorum int) { n.mu.Lock(); defer n.mu.Unlock(); n.quorum = newQuorum }
func (n *Node) ElectionTimeMs() int64      { n.mu.Lock(); defer n.mu.Unlock(); return n.electionTimeMs }
func (n *Node) SetStatus(status pb.ElectionStatus) {
	n.mu.Lock()
	defer n.mu.Unlock()
	n.status = status
}
func (n *Node) ElectionReceivedVotes() uint32 {
	n.mu.Lock()
	defer n.mu.Unlock()
	return n.electionReceivedVotes
}

// PromoteToFinCandidate is what you call *after* you’ve reached quorum in Algorithm 1.
func (n *Node) PromoteToFinCandidate() {
	n.mu.Lock()
	defer n.mu.Unlock()
	n.status = pb.ElectionStatus_FIN_CANDIDATE
}

// BecomeLeader is what you call when you've received a majority in the leader election phase.
func (n *Node) BecomeLeader() {
	n.mu.Lock()
	defer n.mu.Unlock()
	n.status = pb.ElectionStatus_LEADER
}

func (n *Node) AdoptLeaderMeta(receivedVotes uint32, electionTimeMs int64) {
	n.mu.Lock()
	defer n.mu.Unlock()
	n.status = pb.ElectionStatus_FOLLOWER
	n.receivedVotes = receivedVotes
	n.electionTimeMs = electionTimeMs
}

// SetVANETSimulator sets the network simulator for this node
func (n *Node) SetVANETSimulator(sim *VANETSimulator) {
	n.mu.Lock()
	defer n.mu.Unlock()
	n.vanetSim = sim
}

// GetVANETSimulator returns the network simulator for this node
func (n *Node) GetVANETSimulator() *VANETSimulator {
	n.mu.Lock()
	defer n.mu.Unlock()
	return n.vanetSim
}

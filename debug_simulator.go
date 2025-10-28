package main

import (
	"context"
	"fmt"
	"log"
	"math/rand"
	"sort"
	"strings"
	"sync"
	"time"

	"github.com/ymalegao/DistributedSystemsforAVs/vehicle"
)

// DebugSimulator provides detailed logging for understanding the consensus algorithm
type DebugSimulator struct {
	vanetSim *vehicle.VANETSimulator
}

// NewDebugSimulator creates a debug simulator
func NewDebugSimulator() *DebugSimulator {
	// Setup with no packet loss for cleaner logs
	vanetConfig := vehicle.VANETConfig{
		PacketLossRate: 0.05, // No loss for debugging
		MinLatency:     10 * time.Millisecond,
		MaxLatency:     50 * time.Millisecond,
		Jitter:         5 * time.Millisecond,
	}
	
	return &DebugSimulator{
		vanetSim: vehicle.NewVANETSimulator(vanetConfig),
	}
}

// Vision-based fallback leader selection
func selectVisionFallbackLeader(nodes map[string]*vehicle.Node) string {
	ids := make([]string, 0, len(nodes))
	for id := range nodes {
		ids = append(ids, id)
	}
	sort.Strings(ids)
	return ids[0] // Select alphabetically first
}

// RunDebugExperiment runs a single experiment with detailed logging
// Returns: (success, leaderID, retryNeeded)
func (ds *DebugSimulator) RunDebugExperiment(vehicleCount int, portOffset int) (bool, string, bool) {
	fmt.Printf("\n========================================\n")
	fmt.Printf("DEBUG EXPERIMENT: %d Vehicles (ports starting at %d)\n", vehicleCount, 9000+portOffset)
	fmt.Printf("========================================\n\n")
	
	// Calculate quorum for majority
	quorumSize := vehicleCount/2 + 1
	
	// Create nodes
	nodes := make(map[string]*vehicle.Node)
	vehicleIDs := make([]string, vehicleCount)
	for i := 0; i < vehicleCount; i++ {
		id := fmt.Sprintf("vehicle%d", i+1)
		addr := fmt.Sprintf("localhost:%d", 9000+portOffset+i)
		
		node := vehicle.NewNode(id, id, "N-straight", addr, quorumSize)
		
		// Attach VANET simulator
		if ds.vanetSim != nil {
			node.SetVANETSimulator(ds.vanetSim)
		}
		
		nodes[id] = node
		vehicleIDs[i] = id
		
		// Start the gRPC server for this node
		go func(n *vehicle.Node) {
			if err := n.Start(); err != nil {
				log.Printf("Warning: failed to start server for %s: %v", n.ID(), err)
			}
		}(node)
	}
	
	// Wait for servers to start
	time.Sleep(100 * time.Millisecond)
	
	// Set up peer connections - full mesh
	log.Printf("DEBUG: Setting up peer connections...")
	for id, node := range nodes {
		for peerID, peerNode := range nodes {
			if peerID != id {
				if err := node.DialPeer(peerID, peerNode.Addr(), 2*time.Second); err != nil {
					log.Printf("Warning: failed to connect %s to %s: %v", id, peerID, err)
				}
			}
		}
	}
	log.Printf("DEBUG: Peer connections established\n")
	
	// Phase 1: Reset all nodes
	log.Printf("\n=== PHASE 1: Reset Epoch ===\n")
	for id, node := range nodes {
		node.ResetForEpoch()
		log.Printf("  [%s] Reset: sentVotes=0, receivedVotes=%d\n", id, node.ReceivedVotes())
	}
	
	// Phase 2: Candidate Vote Phase with Raft-style random timing variation
	log.Printf("\n=== PHASE 2: Candidate Vote Phase ===\n")
	
	// Use longer timeout for leader election to test fallback
	ctx, cancel := context.WithTimeout(context.Background(), 500*time.Millisecond)
	defer cancel()
	
	// Raft-style random delays: random between 0-200ms to stagger vote requests
	// This makes it highly unlikely for multiple vehicles to get same vote count at same time
	rng := rand.New(rand.NewSource(time.Now().UnixNano()))
	delayMap := make(map[string]time.Duration)
	for id := range nodes {
		// Random delay between 0-200ms (like Raft's random election timeout)
		delayMap[id] = time.Duration(rng.Intn(200)) * time.Millisecond
		log.Printf("  [%s] Scheduled to start votes after %v delay\n", id, delayMap[id])
	}
	
	var wg sync.WaitGroup
	candidateResults := make(map[string]bool)
	var mu sync.Mutex
	startTime := time.Now()
	
	for id, node := range nodes {
		wg.Add(1)
		go func(nodeID string, n *vehicle.Node) {
			defer wg.Done()
			
			// Random delay (Raft-style) to avoid perfect simultaneity
			delay := delayMap[nodeID]
			if delay > 0 {
				select {
				case <-time.After(delay):
					// Continue
				case <-ctx.Done():
					// Context expired during delay
					log.Printf("  [%s] Cancelled before starting votes\n", nodeID)
					mu.Lock()
					candidateResults[nodeID] = false
					mu.Unlock()
					return
				}
			}
			
			actualStartTime := time.Since(startTime)
			log.Printf("  [%s] Starting CandidateVote requests (after %v delay)\n", nodeID, actualStartTime)
			
			acks, _ := n.SolicitCandidateVotes(ctx, 100*time.Millisecond)
			
			totalVotes := int(acks) + 1 // +1 for self-vote
			
			mu.Lock()
			reachedQuorum := totalVotes >= quorumSize
			candidateResults[nodeID] = reachedQuorum
			
			log.Printf("  [%s] Got %d ACKs + 1 = %d votes total (need %d) - Quorum: %v\n", 
				nodeID, acks, totalVotes, quorumSize, reachedQuorum)
			
			// Show detailed vote count
			log.Printf("    [%s] receivedVotes field: %d\n", nodeID, n.ReceivedVotes())
			log.Printf("    [%s] Status: %s\n", nodeID, n.Status().String())
			
			mu.Unlock()
		}(id, node)
	}
	
	wg.Wait()
	
	// Phase 3: Determine FIN_CANDIDATEs
	log.Printf("\n=== PHASE 3: Determine FIN_CANDIDATEs ===\n")
	var finCandidates []string
	for nodeID, reachedQuorum := range candidateResults {
		if reachedQuorum {
			finCandidates = append(finCandidates, nodeID)
			nodes[nodeID].PromoteToFinCandidate()
			log.Printf("  [%s] → FIN_CANDIDATE (reached quorum)\n", nodeID)
		} else {
			log.Printf("  [%s] → NOT promoted (didn't reach quorum)\n", nodeID)
		}
	}
	
	if len(finCandidates) == 0 {
		log.Printf("\n❌ NO FIN_CANDIDATES - Consensus FAILED\n")
		
		// Show detailed state of each node
		log.Printf("\n=== Node States (for debugging) ===\n")
		for id, node := range nodes {
			log.Printf("  [%s] Status: %s, receivedVotes: %d\n", 
				id, node.Status().String(), node.ReceivedVotes())
		}
		
		// Clean up
		for _, node := range nodes {
			node.Stop()
		}
		// Wait longer to ensure ports are fully released
		time.Sleep(500 * time.Millisecond)
		
		return false, "", true // Retry needed
	}
	
	// Phase 4: Leader Election with timeout check
	log.Printf("\n=== PHASE 4: Leader Election ===\n")
	log.Printf("  FIN_CANDIDATEs: %v\n", finCandidates)
	
	leaderElectionStart := time.Now()
	leaderTimeout := 300 * time.Millisecond // 300ms timeout for leader election
	
	var leaderID string
	leaderElected := false
	
	for _, id := range finCandidates {
		// Check if we've exceeded the leader election timeout
		if time.Since(leaderElectionStart) > leaderTimeout {
			log.Printf("\n⏱️  LEADER ELECTION TIMEOUT after %v\n", leaderTimeout)
			log.Printf("  Switching to vision-based fallback decision method\n")
			
			// Vision-based fallback: select alphabetically first vehicle
			leaderID = selectVisionFallbackLeader(nodes)
			log.Printf("  Vision Fallback Leader: %s\n", leaderID)
			
			// Clean up
			for _, node := range nodes {
				node.Stop()
			}
			time.Sleep(100 * time.Millisecond)
			
			return false, leaderID, false // Success with fallback, no retry
		}
		
		node := nodes[id]
		
		log.Printf("\n  [%s] Running leader election...\n", id)
		log.Printf("    [%s] Current state: receivedVotes=%d, electionTime=%d\n", 
			id, node.ReceivedVotes(), node.ElectionTimeMs())
		
		leaderAcks := node.RunLeaderElection(ctx, 100*time.Millisecond)
		
		log.Printf("    [%s] Got %d leader ACKs + 1 = %d (need %d)\n", 
			id, leaderAcks, int(leaderAcks)+1, quorumSize)
		
		if int(leaderAcks)+1 >= quorumSize {
			leaderID = id
			nodes[id].BecomeLeader()
			log.Printf("    [%s] → LEADER! ✓\n", id)
			leaderElected = true
			break
		} else {
			log.Printf("    [%s] → Not enough ACKs\n", id)
		}
	}
	
	if leaderElected && leaderID != "" {
		log.Printf("\n✅ CONSENSUS SUCCESS: Leader = %s\n", leaderID)
		
		// Show final states
		log.Printf("\n=== Final Node States ===\n")
		for id, node := range nodes {
			log.Printf("  [%s] Status: %s, receivedVotes: %d\n", 
				id, node.Status().String(), node.ReceivedVotes())
		}
		
		// Clean up
		for _, node := range nodes {
			node.Stop()
		}
		// Wait longer to ensure ports are fully released
		time.Sleep(500 * time.Millisecond)
		
		return true, leaderID, false // Success, no retry needed
	} else {
		log.Printf("\n❌ NO LEADER ELECTED - Consensus FAILED\n")
		
		// Show detailed state of each node
		log.Printf("\n=== Node States (for debugging) ===\n")
		for id, node := range nodes {
			log.Printf("  [%s] Status: %s, receivedVotes: %d, electionTime: %d\n", 
				id, node.Status().String(), node.ReceivedVotes(), node.ElectionTimeMs())
		}
		
		// Clean up
		for _, node := range nodes {
			node.Stop()
		}
		// Wait longer to ensure ports are fully released
		time.Sleep(500 * time.Millisecond)
		
		return false, "", true // Retry needed
	}
}

// RunExperimentWithRetry runs an experiment with retry logic for remaining vehicles
func (ds *DebugSimulator) RunExperimentWithRetry(vehicleCount int, maxRetries int) {
	fmt.Println("\n" + strings.Repeat("=", 60))
	fmt.Printf("EXPERIMENT: %d Vehicles (max %d retries)\n", vehicleCount, maxRetries)
	fmt.Println(strings.Repeat("=", 60))
	
	remainingVehicles := vehicleCount
	attempt := 0
	
	for attempt < maxRetries && remainingVehicles > 0 {
		attempt++
		fmt.Printf("\n--- ATTEMPT %d: %d remaining vehicles ---\n", attempt, remainingVehicles)
		
		// Use different port offset for each retry to avoid conflicts
		portOffset := attempt * 20
		success, leaderID, retryNeeded := ds.RunDebugExperiment(remainingVehicles, portOffset)
		
		if success {
			fmt.Printf("\n✅ Leader elected: %s\n", leaderID)
			remainingVehicles-- // One vehicle passes
			if remainingVehicles == 0 {
				fmt.Printf("\n✅ ALL VEHICLES PASSED\n")
				return
			}
			// Wait before retry with remaining vehicles - longer delay to ensure ports are released
			fmt.Printf("\n⏳ Restarting consensus for remaining %d vehicles...\n\n", remainingVehicles)
			time.Sleep(1 * time.Second)
		} else if retryNeeded {
			fmt.Printf("\n❌ Consensus failed, retrying...\n\n")
			time.Sleep(1 * time.Second)
		} else {
			// Vision fallback was used
			fmt.Printf("\n⚠️  Vision fallback used, leader: %s\n", leaderID)
			remainingVehicles-- // One vehicle passes
			if remainingVehicles == 0 {
				fmt.Printf("\n✅ ALL VEHICLES PASSED (with vision fallback)\n")
				return
			}
			// Wait before retry with remaining vehicles - longer delay to ensure ports are released
			fmt.Printf("\n⏳ Restarting consensus for remaining %d vehicles...\n\n", remainingVehicles)
			time.Sleep(1 * time.Second)
		}
	}
	
	if remainingVehicles > 0 {
		fmt.Printf("\n❌ NOT ALL VEHICLES PASSED after %d attempts (%d remaining)\n", maxRetries, remainingVehicles)
	} else {
		fmt.Printf("\n✅ ALL VEHICLES PASSED after %d attempt(s)\n", attempt)
	}
}

func main() {
	log.SetFlags(log.Lmicroseconds)
	
	simulator := NewDebugSimulator()
	
	// Test with different vehicle counts
	fmt.Println("Debug Simulator - Tracing Consensus Algorithm")
	fmt.Println("==============================================")
	fmt.Println("Features:")
	fmt.Println("  - Raft-style random timing variation")
	fmt.Println("  - Vision-based fallback on timeout")
	fmt.Println("  - Retry/restart logic for remaining vehicles")
	fmt.Println()
	
	// Test with 4 vehicles
	fmt.Println("\n" + strings.Repeat("=", 60))
	fmt.Println("TEST 1: 4 Vehicles")
	fmt.Println(strings.Repeat("=", 60))
	simulator.RunExperimentWithRetry(4, 3)
	
	// Wait between experiments
	time.Sleep(2 * time.Second)
	
	// Test with 8 vehicles
	fmt.Println("\n" + strings.Repeat("=", 60))
	fmt.Println("TEST 2: 8 Vehicles")
	fmt.Println(strings.Repeat("=", 60))
	simulator.RunExperimentWithRetry(8, 3)
}

package main

import (
	"context"
	"fmt"
	"log"
	"os"
	"sync"
	"time"

	"github.com/ymalegao/DistributedSystemsforAVs/bft/byz"
	"github.com/ymalegao/DistributedSystemsforAVs/bft/node"
)

// ByzantineTestSimulator tests BFT consensus with Byzantine nodes
type ByzantineTestSimulator struct {
	nodes     map[string]*node.BFTNode
	n         int                        // total nodes
	f         int                        // max byzantine faults
	byzantine map[string]byz.ByzBehavior // Byzantine behaviors
}

// NewByzantineTestSimulator creates a new Byzantine test simulator
func NewByzantineTestSimulator(n, f int) *ByzantineTestSimulator {
	return &ByzantineTestSimulator{
		nodes:     make(map[string]*node.BFTNode),
		n:         n,
		f:         f,
		byzantine: make(map[string]byz.ByzBehavior),
	}
}

// CreateNodesWithByzantine creates n nodes, with specified Byzantine behaviors
func (sim *ByzantineTestSimulator) CreateNodesWithByzantine(portOffset int, byzantineNodes map[string]string) error {
	log.Printf("Creating %d BFT nodes (f=%d, quorum=%d) with %d Byzantine nodes",
		sim.n, sim.f, 2*sim.f+1, len(byzantineNodes))

	// Create nodes
	for i := 0; i < sim.n; i++ {
		id := fmt.Sprintf("vehicle%d", i+1)
		addr := fmt.Sprintf("localhost:%d", 10000+portOffset+i)
		direction := "N-straight"

		bftNode, err := node.NewBFTNode(id, id, direction, addr, sim.n, sim.f)
		if err != nil {
			return fmt.Errorf("failed to create node %s: %w", id, err)
		}

		// Check if this node should be Byzantine
		if behaviorType, isByzantine := byzantineNodes[id]; isByzantine {
			var behavior byz.ByzBehavior

			switch behaviorType {
			case "Honest":
				behavior = byz.NewHonest(id)
			case "EquivocateSend":
				behavior = byz.NewEquivocateSend(id)
			case "DoubleVote":
				behavior = byz.NewDoubleVote(id)
			default:
				behavior = byz.NewHonest(id)
			}

			sim.byzantine[id] = behavior
			bftNode.SetByzantineBehavior(behavior)
			log.Printf("Node %s configured as Byzantine: %s", id, behavior.Name())
		}

		sim.nodes[id] = bftNode

		// Start the server
		if err := bftNode.Start(); err != nil {
			return fmt.Errorf("failed to start node %s: %w", id, err)
		}

		log.Printf("Created node %s at %s", id, addr)
	}

	// Wait for servers to start
	time.Sleep(200 * time.Millisecond)

	return nil
}

// ConnectNodes establishes connections between all nodes
func (sim *ByzantineTestSimulator) ConnectNodes() error {
	log.Printf("Connecting nodes in full mesh topology")

	// Collect all peer info
	peerInfo := make(map[string]struct {
		addr   string
		pubKey []byte
	})

	for id, n := range sim.nodes {
		peerInfo[id] = struct {
			addr   string
			pubKey []byte
		}{
			addr:   n.Addr(),
			pubKey: n.GetPublicKey(),
		}
	}

	// Connect each node to all other nodes
	for id, n := range sim.nodes {
		for peerID, info := range peerInfo {
			if peerID != id {
				if err := n.ConnectToPeer(peerID, info.addr, info.pubKey, 2*time.Second); err != nil {
					log.Printf("Warning: failed to connect %s to %s: %v", id, peerID, err)
				}
			}
		}
	}

	log.Printf("All nodes connected")
	return nil
}

// RunConsensus runs consensus on all nodes
func (sim *ByzantineTestSimulator) RunConsensus(ctx context.Context) (string, time.Duration, error) {
	startTime := time.Now()

	log.Printf("=== Starting BFT Consensus (with Byzantine nodes) ===")

	var wg sync.WaitGroup
	errors := make(chan error, len(sim.nodes))

	// Run consensus on all nodes concurrently
	for id, n := range sim.nodes {
		wg.Add(1)
		go func(nodeID string, bftNode *node.BFTNode) {
			defer wg.Done()

			if err := bftNode.RunConsensus(ctx); err != nil {
				errors <- fmt.Errorf("node %s consensus failed: %w", nodeID, err)
			}
		}(id, n)
	}

	wg.Wait()
	close(errors)

	// Check for errors
	for err := range errors {
		log.Printf("Error: %v", err)
		return "", 0, err
	}

	consensusTime := time.Since(startTime)

	// Check consensus result
	leaderID := ""
	consensusReached := true
	honestAgreement := make(map[string]int) // leader -> count

	for id, n := range sim.nodes {
		nodeLeader := n.GetLeaderID()

		// Only count honest nodes for consensus
		if _, isByzantine := sim.byzantine[id]; !isByzantine {
			honestAgreement[nodeLeader]++

			if leaderID == "" {
				leaderID = nodeLeader
			} else if nodeLeader != leaderID {
				log.Printf("⚠️  Honest node %s disagrees: thinks leader is %s, but %s was expected",
					id, nodeLeader, leaderID)
				consensusReached = false
			}
		}

		byzMarker := ""
		if beh, ok := sim.byzantine[id]; ok {
			byzMarker = fmt.Sprintf(" [%s]", beh.Name())
		}
		log.Printf("Node %s%s: Status=%v, Leader=%s", id, byzMarker, n.GetStatus(), nodeLeader)
	}

	if !consensusReached {
		return "", consensusTime, fmt.Errorf("honest nodes did not reach consensus")
	}

	log.Printf("=== Consensus Reached: Leader is %s (time: %v) ===", leaderID, consensusTime)
	log.Printf("Honest node agreement: %d/%d nodes agree on %s",
		honestAgreement[leaderID], sim.n-len(sim.byzantine), leaderID)

	return leaderID, consensusTime, nil
}

// Cleanup stops all nodes
func (sim *ByzantineTestSimulator) Cleanup() {
	log.Printf("Cleaning up nodes")
	for _, n := range sim.nodes {
		n.Stop()
	}
	time.Sleep(500 * time.Millisecond)
}

func main() {
	// Enable logging
	log.SetFlags(log.Lmicroseconds | log.Lshortfile)
	log.SetOutput(os.Stdout)

	fmt.Println("=== Byzantine Fault Tolerant Consensus Test ===")
	fmt.Println()

	// Test 1: n=3, f=1 - SHOULD FAIL (violates n >= 3f+1)
	fmt.Println("Test 1: n=3, f=1 with 1 EquivocateSend Byzantine node (SHOULD FAIL)")
	fmt.Println("------------------------------------------------------------------------")
	fmt.Println("Note: n=3, f=1 violates BFT requirement n >= 3f+1 (need n >= 4)")
	fmt.Println()
	sim1 := NewByzantineTestSimulator(3, 1)

	byzantineNodes1 := map[string]string{
		"vehicle3": "DoubleVote",
	}

	if err := sim1.CreateNodesWithByzantine(0, byzantineNodes1); err != nil {
		log.Printf("Failed to create nodes: %v", err)
		sim1.Cleanup()
		return
	}

	if err := sim1.ConnectNodes(); err != nil {
		log.Printf("Failed to connect nodes: %v", err)
		sim1.Cleanup()
		return
	}

	ctx1, cancel1 := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel1()

	leaderID1, consensusTime1, err1 := sim1.RunConsensus(ctx1)

	// Check if Byzantine node became leader (safety violation)
	byzantineLeader := false
	for byzID := range byzantineNodes1 {
		if leaderID1 == byzID {
			byzantineLeader = true
			break
		}
	}

	// With n=3, f=1, the system CANNOT be Byzantine fault-tolerant
	// because quorum=3 requires ALL nodes (including the Byzantine one)
	honestNodes := sim1.n - len(byzantineNodes1)
	quorum := 2*sim1.f + 1

	if err1 != nil {
		fmt.Printf("✓ Test 1 PASSED: Consensus correctly FAILED with insufficient nodes\n")
		fmt.Printf("  Error: %v\n", err1)
	} else if byzantineLeader {
		fmt.Printf("✓ Test 1 PASSED: Byzantine node became leader (expected safety violation)\n")
		fmt.Printf("  Leader: %s, Time: %v\n", leaderID1, consensusTime1)
	} else if honestNodes < quorum {
		// Honest nodes alone cannot form a quorum - Byzantine node has veto power
		fmt.Printf("✗ Test 1 FAILED: Consensus succeeded but Byzantine node has VETO POWER\n")
		fmt.Printf("  Leader: %s, Time: %v\n", leaderID1, consensusTime1)
		fmt.Printf("  Honest nodes (%d) < Quorum (%d) - Byzantine node was REQUIRED\n", honestNodes, quorum)
		fmt.Printf("  System is fundamentally unsafe with n=%d, f=%d\n", sim1.n, sim1.f)
	} else {
		fmt.Printf("✓ Test 1 PASSED: Consensus reached with sufficient honest nodes\n")
		fmt.Printf("  Leader: %s, Time: %v\n", leaderID1, consensusTime1)
	}

	sim1.Cleanup()
	fmt.Println()

	// Test 1.5: n=4, f=1 - SHOULD PASS (satisfies n >= 3f+1)
	fmt.Println("Test 1.5: n=4, f=1 with 1 EquivocateSend Byzantine node (SHOULD PASS)")
	fmt.Println("------------------------------------------------------------------------")
	fmt.Println("Note: n=4, f=1 satisfies BFT requirement n >= 3f+1 (4 >= 4 ✓)")
	fmt.Println()
	sim1_5 := NewByzantineTestSimulator(4, 1)

	byzantineNodes1_5 := map[string]string{
		"vehicle4": "EquivocateSend",
	}

	if err := sim1_5.CreateNodesWithByzantine(20, byzantineNodes1_5); err != nil {
		log.Printf("Failed to create nodes: %v", err)
		sim1_5.Cleanup()
		return
	}

	if err := sim1_5.ConnectNodes(); err != nil {
		log.Printf("Failed to connect nodes: %v", err)
		sim1_5.Cleanup()
		return
	}

	ctx1_5, cancel1_5 := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel1_5()

	leaderID1_5, consensusTime1_5, err1_5 := sim1_5.RunConsensus(ctx1_5)

	// Check if Byzantine node became leader (safety violation)
	byzantineLeader1_5 := false
	for byzID := range byzantineNodes1_5 {
		if leaderID1_5 == byzID {
			byzantineLeader1_5 = true
			break
		}
	}

	if err1_5 != nil {
		fmt.Printf("✗ Test 1.5 FAILED: Consensus failed with proper n=4, f=1\n")
		fmt.Printf("  Error: %v\n", err1_5)
	} else if byzantineLeader1_5 {
		fmt.Printf("✗ Test 1.5 FAILED: Byzantine node became leader (safety violation!)\n")
		fmt.Printf("  Leader: %s, Time: %v\n", leaderID1_5, consensusTime1_5)
	} else {
		fmt.Printf("✓ Test 1.5 PASSED: Honest node elected despite 1 Byzantine node\n")
		fmt.Printf("  Leader: %s, Time: %v\n", leaderID1_5, consensusTime1_5)
	}

	sim1_5.Cleanup()
	fmt.Println()

	// Test 2: n=7, f=2 with 2 Byzantine nodes (1 EquivocateSend, 1 DoubleVote)
	fmt.Println("Test 2: n=7, f=2 with 2 Byzantine nodes (1 EquivocateSend, 1 DoubleVote)")
	fmt.Println("-------------------------------------------------------------------------------")
	sim2 := NewByzantineTestSimulator(7, 2)

	byzantineNodes2 := map[string]string{
		"vehicle6": "EquivocateSend",
		"vehicle7": "DoubleVote",
	}

	if err := sim2.CreateNodesWithByzantine(100, byzantineNodes2); err != nil {
		log.Printf("Failed to create nodes: %v", err)
		sim2.Cleanup()
		return
	}

	if err := sim2.ConnectNodes(); err != nil {
		log.Printf("Failed to connect nodes: %v", err)
		sim2.Cleanup()
		return
	}

	ctx2, cancel2 := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel2()

	leaderID2, consensusTime2, err2 := sim2.RunConsensus(ctx2)
	if err2 != nil {
		fmt.Printf("✗ Test 2 FAILED: %v\n", err2)
	} else {
		fmt.Printf("✓ Test 2 PASSED: Consensus reached despite 2 Byzantine nodes\n")
		fmt.Printf("  Leader: %s, Time: %v\n", leaderID2, consensusTime2)
	}

	sim2.Cleanup()
	fmt.Println()

	fmt.Println("=== Byzantine Testing Complete ===")
}

package main

import (
	"context"
	"encoding/csv"
	"fmt"
	"io"
	"log"
	"math/rand"
	"os"
	"sync"
	"time"

	"github.com/ymalegao/DistributedSystemsforAVs/vehicle"
)

// DataCollectionSimulator collects timing data for analysis
type DataCollectionSimulator struct {
	vanetSim *vehicle.VANETSimulator
}

// ExperimentData stores timing data for analysis
type ExperimentData struct {
	VehicleCount  int
	ConsensusTime time.Duration
	Success       bool
	LeaderID      string
}

// NewDataCollectionSimulator creates a data collection simulator
func NewDataCollectionSimulator() *DataCollectionSimulator {
	// Setup with minimal packet loss for realistic conditions
	vanetConfig := vehicle.VANETConfig{
		PacketLossRate: 0.05,
		MinLatency:     10 * time.Millisecond,
		MaxLatency:     50 * time.Millisecond,
		Jitter:         5 * time.Millisecond,
	}

	return &DataCollectionSimulator{
		vanetSim: vehicle.NewVANETSimulator(vanetConfig),
	}
}

// RunSingleExperiment runs a single consensus experiment and measures time
func (ds *DataCollectionSimulator) RunSingleExperiment(vehicleCount int, portOffset int) ExperimentData {
	// Calculate quorum for majority
	quorumSize := vehicleCount/2 + 1

	// Create nodes
	nodes := make(map[string]*vehicle.Node)
	for i := 0; i < vehicleCount; i++ {
		id := fmt.Sprintf("vehicle%d", i+1)
		addr := fmt.Sprintf("localhost:%d", 9000+portOffset+i)

		node := vehicle.NewNode(id, id, "N-straight", addr, quorumSize)

		// Attach VANET simulator
		if ds.vanetSim != nil {
			node.SetVANETSimulator(ds.vanetSim)
		}

		nodes[id] = node

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
	for id, node := range nodes {
		for peerID, peerNode := range nodes {
			if peerID != id {
				if err := node.DialPeer(peerID, peerNode.Addr(), 2*time.Second); err != nil {
					log.Printf("Warning: failed to connect %s to %s: %v", id, peerID, err)
				}
			}
		}
	}

	consensusStartTime := time.Now()

	// Phase 1: Reset all nodes
	for id, node := range nodes {
		node.ResetForEpoch()
		_ = id
	}

	// Phase 2: Candidate Vote Phase with Raft-style random timing variation
	ctx, cancel := context.WithTimeout(context.Background(), 500*time.Millisecond)
	defer cancel()

	// Raft-style random delays
	rng := rand.New(rand.NewSource(time.Now().UnixNano()))
	delayMap := make(map[string]time.Duration)
	for id := range nodes {
		delayMap[id] = time.Duration(rng.Intn(200)) * time.Millisecond
	}

	var wg sync.WaitGroup
	candidateResults := make(map[string]bool)
	var mu sync.Mutex

	for id, node := range nodes {
		wg.Add(1)
		go func(nodeID string, n *vehicle.Node) {
			defer wg.Done()

			delay := delayMap[nodeID]
			if delay > 0 {
				select {
				case <-time.After(delay):
				case <-ctx.Done():
					mu.Lock()
					candidateResults[nodeID] = false
					mu.Unlock()
					return
				}
			}

			acks, _ := n.SolicitCandidateVotes(ctx, 100*time.Millisecond)
			totalVotes := int(acks) + 1

			mu.Lock()
			reachedQuorum := totalVotes >= quorumSize
			candidateResults[nodeID] = reachedQuorum
			mu.Unlock()
		}(id, node)
	}

	wg.Wait()

	// Phase 3: Determine FIN_CANDIDATEs
	var finCandidates []string
	for nodeID, reachedQuorum := range candidateResults {
		if reachedQuorum {
			finCandidates = append(finCandidates, nodeID)
			nodes[nodeID].PromoteToFinCandidate()
		}
	}

	if len(finCandidates) == 0 {
		// Clean up
		for _, node := range nodes {
			node.Stop()
		}
		time.Sleep(500 * time.Millisecond)

		return ExperimentData{
			VehicleCount:  vehicleCount,
			ConsensusTime: time.Since(consensusStartTime),
			Success:       false,
		}
	}

	// Phase 4: Leader Election
	var leaderID string
	leaderElected := false

	for _, id := range finCandidates {
		node := nodes[id]
		leaderAcks := node.RunLeaderElection(ctx, 100*time.Millisecond)

		if int(leaderAcks)+1 >= quorumSize {
			leaderID = id
			nodes[id].BecomeLeader()
			leaderElected = true
			break
		}
	}

	consensusTime := time.Since(consensusStartTime)

	// Clean up
	for _, node := range nodes {
		node.Stop()
	}
	time.Sleep(500 * time.Millisecond)

	if leaderElected {
		return ExperimentData{
			VehicleCount:  vehicleCount,
			ConsensusTime: consensusTime,
			Success:       true,
			LeaderID:      leaderID,
		}
	}

	return ExperimentData{
		VehicleCount:  vehicleCount,
		ConsensusTime: consensusTime,
		Success:       false,
	}
}

// CollectData collects timing data for vehicles 3-8
func (ds *DataCollectionSimulator) CollectData(trialsPerCount int) map[int][]time.Duration {
	fmt.Println("=== Collecting Consensus Timing Data ===")
	fmt.Printf("Running %d trials for each vehicle count (3-8)\n\n", trialsPerCount)

	results := make(map[int][]time.Duration) // vehicleCount -> slice of consensus times

	// Run experiments for 3-8 vehicles
	for vehicleCount := 3; vehicleCount <= 8; vehicleCount++ {
		fmt.Printf("Testing %d vehicles... ", vehicleCount)

		for trial := 0; trial < trialsPerCount; trial++ {
			portOffset := trial * 20
			data := ds.RunSingleExperiment(vehicleCount, portOffset)

			if data.Success {
				results[vehicleCount] = append(results[vehicleCount], data.ConsensusTime)
				fmt.Printf(".")
			} else {
				fmt.Printf("x") // Failed trial
			}
		}
		fmt.Println()
	}

	return results
}

// PrintResults displays the results in a table format
func PrintResults(results map[int][]time.Duration) {
	fmt.Println("\n=== Results ===")
	fmt.Println("Vehicles | Avg Consensus Time (ms) | Trials")
	fmt.Println("---------|-------------------------|---------")

	successfulTrials := 0

	for vehicleCount := 3; vehicleCount <= 8; vehicleCount++ {
		if times, exists := results[vehicleCount]; exists && len(times) > 0 {
			var total time.Duration
			for _, t := range times {
				total += t
			}
			avgTime := total / time.Duration(len(times))

			fmt.Printf("   %d    |    %.2f ms              |   %d\n", vehicleCount,
				float64(avgTime.Nanoseconds())/1e6, len(times))
			successfulTrials += len(times)
		} else {
			fmt.Printf("   %d    |    No successful trials |   0\n", vehicleCount)
		}
	}

	fmt.Printf("\nTotal successful trials: %d\n", successfulTrials)
}

// WriteResultsToCSV writes the results to a CSV file
func WriteResultsToCSV(results map[int][]time.Duration, filename string) error {
	file, err := os.Create(filename)
	if err != nil {
		return fmt.Errorf("failed to create CSV file: %w", err)
	}
	defer file.Close()

	writer := csv.NewWriter(file)
	defer writer.Flush()

	// Write header
	if err := writer.Write([]string{"vehicles", "avg_consensus_time_ms"}); err != nil {
		return fmt.Errorf("failed to write CSV header: %w", err)
	}

	// Write data
	for vehicleCount := 3; vehicleCount <= 8; vehicleCount++ {
		if times, exists := results[vehicleCount]; exists && len(times) > 0 {
			var total time.Duration
			for _, t := range times {
				total += t
			}
			avgTime := total / time.Duration(len(times))
			avgTimeMs := float64(avgTime.Nanoseconds()) / 1e6

			if err := writer.Write([]string{
				fmt.Sprintf("%d", vehicleCount),
				fmt.Sprintf("%.2f", avgTimeMs),
			}); err != nil {
				return fmt.Errorf("failed to write CSV row: %w", err)
			}
		}
	}

	return nil
}

func main() {
	// Suppress logging by discarding output
	log.SetOutput(io.Discard)

	simulator := NewDataCollectionSimulator()

	// Collect data with 10 trials per vehicle count for better statistics
	results := simulator.CollectData(10)

	// Print results to console
	PrintResults(results)

	// Write results to CSV
	if err := WriteResultsToCSV(results, "consensus.csv"); err != nil {
		fmt.Fprintf(os.Stderr, "Error writing CSV: %v\n", err)
		os.Exit(1)
	}

	fmt.Println("\nResults written to consensus.csv")
}

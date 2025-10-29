package main

import (
	"context"
	"encoding/csv"
	"fmt"
	"io"
	"log"
	"os"
	"time"

	"github.com/ymalegao/DistributedSystemsforAVs/bft/byz"
	"github.com/ymalegao/DistributedSystemsforAVs/bft/node"
	"github.com/ymalegao/DistributedSystemsforAVs/bft/transport"
)

// ByzantineDataCollector collects timing data for BFT with Byzantine faults
type ByzantineDataCollector struct {
	useVANET bool
	vanetSim *transport.DefaultVANETSimulator
}

// ByzantineExperimentResult stores results of a Byzantine fault experiment
type ByzantineExperimentResult struct {
	VehicleCount      int
	F                 int
	ByzantineCount    int
	ByzantineBehavior string
	ConsensusTime     time.Duration
	Success           bool
	SafetyViolation   bool // Byzantine node became leader
	LeaderID          string
	ErrorType         string
}

// NewByzantineDataCollector creates a new Byzantine data collector
func NewByzantineDataCollector(useVANET bool) *ByzantineDataCollector {
	var vanetSim *transport.DefaultVANETSimulator
	if useVANET {
		// Create VANET simulator with same config as data_collection_simulator.go
		vanetSim = transport.NewVANETSimulator(transport.VANETConfig{
			PacketLossRate: 0.05, // 5% packet loss
			MinLatency:     10 * time.Millisecond,
			MaxLatency:     50 * time.Millisecond,
			Jitter:         5 * time.Millisecond,
		})
	}

	return &ByzantineDataCollector{
		useVANET: useVANET,
		vanetSim: vanetSim,
	}
}

// RunByzantineExperiment runs a single experiment with Byzantine nodes
func (bdc *ByzantineDataCollector) RunByzantineExperiment(
	vehicleCount int,
	byzantineCount int,
	behaviorType string,
	portOffset int,
) ByzantineExperimentResult {

	f := (vehicleCount - 1) / 3

	// Create nodes
	nodes := make(map[string]*node.BFTNode)
	byzantineNodes := make(map[string]bool)

	for i := 0; i < vehicleCount; i++ {
		id := fmt.Sprintf("vehicle%d", i+1)
		addr := fmt.Sprintf("localhost:%d", 12000+portOffset+i)
		direction := "N-straight"

		bftNode, err := node.NewBFTNode(id, id, direction, addr, vehicleCount, f)
		if err != nil {
			return ByzantineExperimentResult{
				VehicleCount:      vehicleCount,
				F:                 f,
				ByzantineCount:    byzantineCount,
				ByzantineBehavior: behaviorType,
				Success:           false,
				ErrorType:         "node_creation_failed",
			}
		}

		// Set VANET simulator if enabled
		if bdc.vanetSim != nil {
			bftNode.SetVANETSimulator(bdc.vanetSim)
		}

		// Make the last byzantineCount nodes Byzantine
		if i >= vehicleCount-byzantineCount {
			var behavior byz.ByzBehavior
			switch behaviorType {
			case "EquivocateSend":
				behavior = byz.NewEquivocateSend(id)
			case "DoubleVote":
				behavior = byz.NewDoubleVote(id)
			case "WithholdReady":
				behavior = byz.NewWithholdReady(id)
			case "SelectiveForward":
				behavior = byz.NewSelectiveForward(id, 0.5) // Forward to 50% of peers
			case "Delay":
				behavior = byz.NewDelay(id, 100*time.Millisecond, 500*time.Millisecond)
			default:
				behavior = byz.NewHonest(id)
			}
			bftNode.SetByzantineBehavior(behavior)
			byzantineNodes[id] = true
		}

		nodes[id] = bftNode

		if err := bftNode.Start(); err != nil {
			return ByzantineExperimentResult{
				VehicleCount:      vehicleCount,
				F:                 f,
				ByzantineCount:    byzantineCount,
				ByzantineBehavior: behaviorType,
				Success:           false,
				ErrorType:         "server_start_failed",
			}
		}
	}

	time.Sleep(200 * time.Millisecond)

	// Connect nodes in full mesh
	for id, bftNode := range nodes {
		for peerID, peerNode := range nodes {
			if peerID != id {
				if err := bftNode.ConnectToPeer(peerID, peerNode.Addr(), peerNode.GetPublicKey(), 2*time.Second); err != nil {
					// Continue even with connection errors
				}
			}
		}
	}

	time.Sleep(100 * time.Millisecond)

	// Start consensus timing
	consensusStartTime := time.Now()

	for _, bftNode := range nodes {
		bftNode.ResetForEpoch()
	}

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	// Track results
	completeChan := make(chan string, len(nodes))
	errorChan := make(chan error, len(nodes))

	for id, bftNode := range nodes {
		go func(nodeID string, n *node.BFTNode) {
			if err := n.RunConsensus(ctx); err != nil {
				errorChan <- fmt.Errorf("node %s: %w", nodeID, err)
			} else {
				completeChan <- n.GetLeaderID()
			}
		}(id, bftNode)
	}

	// Wait for results
	successCount := 0
	leaderID := ""
	leaderMap := make(map[string]int)

	for i := 0; i < len(nodes); i++ {
		select {
		case leader := <-completeChan:
			successCount++
			leaderMap[leader]++
			if leaderID == "" {
				leaderID = leader
			}
		case <-errorChan:
			// Error occurred
		case <-ctx.Done():
			break
		}
	}

	consensusTime := time.Since(consensusStartTime)

	// Clean up
	for _, bftNode := range nodes {
		bftNode.Stop()
	}
	time.Sleep(500 * time.Millisecond)

	// Analyze results
	honestNodeCount := vehicleCount - byzantineCount
	quorum := 2*f + 1

	// Check if Byzantine node became leader (safety violation)
	safetyViolation := byzantineNodes[leaderID]

	// Check if consensus succeeded
	consensusSuccess := false
	if successCount >= honestNodeCount {
		// Check if majority agreed on same leader
		for leader, count := range leaderMap {
			if count >= honestNodeCount {
				leaderID = leader
				consensusSuccess = true
				break
			}
		}
	}

	errorType := ""
	if !consensusSuccess {
		if successCount == 0 {
			errorType = "total_failure"
		} else if len(leaderMap) > 1 {
			errorType = "split_brain"
		} else if honestNodeCount < quorum {
			errorType = "insufficient_honest_nodes"
		} else {
			errorType = "timeout"
		}
	}

	return ByzantineExperimentResult{
		VehicleCount:      vehicleCount,
		F:                 f,
		ByzantineCount:    byzantineCount,
		ByzantineBehavior: behaviorType,
		ConsensusTime:     consensusTime,
		Success:           consensusSuccess,
		SafetyViolation:   safetyViolation,
		LeaderID:          leaderID,
		ErrorType:         errorType,
	}
}

// CollectByzantineData runs experiments with different Byzantine configurations
func (bdc *ByzantineDataCollector) CollectByzantineData() []ByzantineExperimentResult {
	fmt.Println("=== Byzantine Fault Tolerance Data Collection ===\n")

	results := []ByzantineExperimentResult{}

	// Test different Byzantine behaviors
	behaviors := []string{
		"EquivocateSend",
		"DoubleVote",
		"WithholdReady",
		"SelectiveForward",
		"Delay",
	}

	// Test configurations: (n, byzantine_count)
	// f is calculated automatically as floor((n-1)/3)
	configs := []struct {
		n        int
		byzCount int
		note     string
	}{
		{3, 0, "Baseline: no Byzantine, f=0"},
		{3, 1, "SHOULD FAIL: n=3 < 3f+1=4"},
		{4, 1, "Minimum BFT: n=4 = 3f+1"},
		{5, 1, "Extra safety: n=5 > 3f+1=4"},
		{6, 1, "More safety: n=6 > 3f+1=4"},
		{7, 1, "n=7, f=2, testing with 1 Byzantine"},
		{7, 2, "n=7, f=2, testing at capacity (2 Byzantine)"},
		{8, 2, "n=8, f=2, testing with 2 Byzantine"},
	}

	trialNum := 0
	for _, config := range configs {
		for _, behavior := range behaviors {
			// Skip Byzantine behaviors for baseline (no Byzantine nodes)
			if config.byzCount == 0 && behavior != "EquivocateSend" {
				continue
			}

			behaviorLabel := "None"
			if config.byzCount > 0 {
				behaviorLabel = behavior
			}

			f := (config.n - 1) / 3

			fmt.Printf("Testing n=%d, f=%d, Byzantine=%d (%s)... ",
				config.n, f, config.byzCount, behaviorLabel)

			portOffset := trialNum * 50
			result := bdc.RunByzantineExperiment(config.n, config.byzCount, behavior, portOffset)
			results = append(results, result)

			if result.Success {
				if result.SafetyViolation {
					fmt.Printf("⚠️  (Byzantine leader)\n")
				} else {
					fmt.Printf("✓\n")
				}
			} else {
				fmt.Printf("✗ (%s)\n", result.ErrorType)
			}

			trialNum++
			time.Sleep(100 * time.Millisecond) // Prevent port conflicts
		}
	}

	return results
}

// WriteResultsToCSV writes Byzantine experiment results to CSV
func WriteResultsToCSV(results []ByzantineExperimentResult, filename string) error {
	file, err := os.Create(filename)
	if err != nil {
		return err
	}
	defer file.Close()

	writer := csv.NewWriter(file)
	defer writer.Flush()

	// Write header
	writer.Write([]string{
		"vehicles", "f", "byzantine_count", "byzantine_behavior",
		"consensus_time_ms", "success", "safety_violation", "error_type",
	})

	// Write data
	for _, result := range results {
		writer.Write([]string{
			fmt.Sprintf("%d", result.VehicleCount),
			fmt.Sprintf("%d", result.F),
			fmt.Sprintf("%d", result.ByzantineCount),
			result.ByzantineBehavior,
			fmt.Sprintf("%.2f", float64(result.ConsensusTime.Nanoseconds())/1e6),
			fmt.Sprintf("%t", result.Success),
			fmt.Sprintf("%t", result.SafetyViolation),
			result.ErrorType,
		})
	}

	return nil
}

// PrintSummary prints a summary of the Byzantine fault experiments
func PrintSummary(results []ByzantineExperimentResult) {
	fmt.Println("\n=== Summary ===\n")

	// Group by configuration
	configMap := make(map[string][]ByzantineExperimentResult)
	for _, result := range results {
		key := fmt.Sprintf("n=%d,f=%d,byz=%d", result.VehicleCount, result.F, result.ByzantineCount)
		configMap[key] = append(configMap[key], result)
	}

	fmt.Println("Config | Byzantine Behavior | Success | Safety | Avg Time")
	fmt.Println("-------|-------------------|---------|--------|----------")

	for key, configResults := range configMap {
		for _, result := range configResults {
			successMark := "✗"
			if result.Success {
				successMark = "✓"
			}

			safetyMark := "✓"
			if result.SafetyViolation {
				safetyMark = "✗"
			}

			fmt.Printf("%-6s | %-17s | %-7s | %-6s | %.2f ms\n",
				key,
				result.ByzantineBehavior,
				successMark,
				safetyMark,
				float64(result.ConsensusTime.Nanoseconds())/1e6,
			)
		}
	}

	// Overall statistics
	totalTests := len(results)
	successCount := 0
	safetyViolations := 0

	for _, result := range results {
		if result.Success {
			successCount++
		}
		if result.SafetyViolation {
			safetyViolations++
		}
	}

	fmt.Printf("\nTotal tests: %d\n", totalTests)
	fmt.Printf("Successful: %d (%.1f%%)\n", successCount, float64(successCount)/float64(totalTests)*100)
	fmt.Printf("Safety violations: %d\n", safetyViolations)
}

func main() {
	log.SetOutput(io.Discard)

	// VANET simulation is now ON by default for fair comparison with regular consensus
	// Use --no-vanet flag to disable it for raw performance testing
	useVANET := true
	if len(os.Args) > 1 && os.Args[1] == "--no-vanet" {
		useVANET = false
		fmt.Println("=== Running WITHOUT VANET Simulation (raw performance) ===")
		fmt.Println("Note: This will NOT be comparable to data_collection_simulator.go results")
	} else {
		fmt.Println("=== Running WITH VANET Simulation (Fair Comparison Mode) ===")
		fmt.Println("Network conditions: 5% packet loss, 10-50ms latency, 5ms jitter")
		fmt.Println("This matches data_collection_simulator.go conditions for fair comparison")
		fmt.Println("Use --no-vanet flag for raw performance testing")
	}
	fmt.Println()

	collector := NewByzantineDataCollector(useVANET)
	results := collector.CollectByzantineData()

	PrintSummary(results)

	filename := "byzantine_fault_analysis_with_vanet.csv"
	if !useVANET {
		filename = "byzantine_fault_analysis.csv"
	}

	if err := WriteResultsToCSV(results, filename); err != nil {
		fmt.Fprintf(os.Stderr, "Error writing CSV: %v\n", err)
		os.Exit(1)
	}

	fmt.Printf("\nResults written to %s\n", filename)
}

// vehicle/vanet_simulator.go
//
// Network simulator for VANET (Vehicular Ad-hoc Network) characteristics:
// - Packet loss simulation
// - Network delay simulation
// - Connection quality variation
//
// This simulates realistic VANET conditions over Ethernet/gRPC

package vehicle

import (
	"math/rand"
	"time"
)

// VANETConfig holds network simulation parameters
type VANETConfig struct {
	PacketLossRate float64       // 0.0 (no loss) to 1.0 (100% loss)
	MinLatency     time.Duration // Minimum network latency
	MaxLatency     time.Duration // Maximum network latency
	Jitter         time.Duration // Random delay variation
}

// DefaultVANETConfig returns realistic VANET parameters
func DefaultVANETConfig() VANETConfig {
	return VANETConfig{
		PacketLossRate: 0.10,          // 10% packet loss (typical for VANET)
		MinLatency:     10 * time.Millisecond,
		MaxLatency:     50 * time.Millisecond,
		Jitter:         5 * time.Millisecond,
	}
}

// HighLossVANETConfig returns severe network conditions
func HighLossVANETConfig() VANETConfig {
	return VANETConfig{
		PacketLossRate: 0.30,          // 30% packet loss
		MinLatency:     20 * time.Millisecond,
		MaxLatency:     100 * time.Millisecond,
		Jitter:         10 * time.Millisecond,
	}
}

// NoLossVANETConfig returns ideal network conditions
func NoLossVANETConfig() VANETConfig {
	return VANETConfig{
		PacketLossRate: 0.0,
		MinLatency:     1 * time.Millisecond,
		MaxLatency:     5 * time.Millisecond,
		Jitter:         1 * time.Millisecond,
	}
}

// VANETSimulator wraps gRPC calls with network effects
type VANETSimulator struct {
	config VANETConfig
	rng    *rand.Rand
}

// NewVANETSimulator creates a new network simulator
func NewVANETSimulator(config VANETConfig) *VANETSimulator {
	return &VANETSimulator{
		config: config,
		rng:    rand.New(rand.NewSource(time.Now().UnixNano())),
	}
}

// GetConfig returns the current network configuration
func (vs *VANETSimulator) GetConfig() VANETConfig {
	return vs.config
}

// SetConfig updates the network configuration
func (vs *VANETSimulator) SetConfig(config VANETConfig) {
	vs.config = config
}

// SimulatePacketDrop returns true if a packet should be dropped
func (vs *VANETSimulator) SimulatePacketDrop() bool {
	return vs.rng.Float64() < vs.config.PacketLossRate
}

// SimulateLatency returns a simulated network delay
func (vs *VANETSimulator) SimulateLatency() time.Duration {
	latency := vs.config.MinLatency
	latencyRange := vs.config.MaxLatency - vs.config.MinLatency
	if latencyRange > 0 {
		latency += time.Duration(vs.rng.Float64() * float64(latencyRange))
	}
	if vs.config.Jitter > 0 {
		jitterAmount := time.Duration(vs.rng.Float64() * float64(vs.config.Jitter))
		latency += jitterAmount
	}
	return latency
}

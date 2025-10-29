package transport

import (
	"context"
	"fmt"
	"log"
	"math/rand"
	"sync"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"

	"github.com/ymalegao/DistributedSystemsforAVs/bft/byz"
	"github.com/ymalegao/DistributedSystemsforAVs/pb/bftconsensus"
)

// ClientPool manages connections to multiple peers
type ClientPool struct {
	mu      sync.RWMutex
	clients map[string]bftconsensus.BFTConsensusUnaryClient
	conns   map[string]*grpc.ClientConn
}

// NewClientPool creates a new client pool
func NewClientPool() *ClientPool {
	return &ClientPool{
		clients: make(map[string]bftconsensus.BFTConsensusUnaryClient),
		conns:   make(map[string]*grpc.ClientConn),
	}
}

// Connect establishes a connection to a peer
func (cp *ClientPool) Connect(peerID, addr string, timeout time.Duration) error {
	cp.mu.Lock()
	defer cp.mu.Unlock()

	// Check if already connected
	if _, exists := cp.clients[peerID]; exists {
		return nil
	}

	// Create context with timeout
	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()

	// Dial with context
	conn, err := grpc.DialContext(ctx, addr,
		grpc.WithTransportCredentials(insecure.NewCredentials()),
		grpc.WithBlock(),
	)
	if err != nil {
		return fmt.Errorf("failed to connect to %s (%s): %w", peerID, addr, err)
	}

	client := bftconsensus.NewBFTConsensusUnaryClient(conn)
	cp.clients[peerID] = client
	cp.conns[peerID] = conn

	log.Printf("Connected to peer %s at %s", peerID, addr)
	return nil
}

// GetClient returns the client for a peer
func (cp *ClientPool) GetClient(peerID string) (bftconsensus.BFTConsensusUnaryClient, error) {
	cp.mu.RLock()
	defer cp.mu.RUnlock()

	client, exists := cp.clients[peerID]
	if !exists {
		return nil, fmt.Errorf("no connection to peer %s", peerID)
	}

	return client, nil
}

// GetAllClients returns all connected clients
func (cp *ClientPool) GetAllClients() map[string]bftconsensus.BFTConsensusUnaryClient {
	cp.mu.RLock()
	defer cp.mu.RUnlock()

	clients := make(map[string]bftconsensus.BFTConsensusUnaryClient)
	for id, client := range cp.clients {
		clients[id] = client
	}
	return clients
}

// Close closes all connections
func (cp *ClientPool) Close() {
	cp.mu.Lock()
	defer cp.mu.Unlock()

	for peerID, conn := range cp.conns {
		if err := conn.Close(); err != nil {
			log.Printf("Error closing connection to %s: %v", peerID, err)
		}
	}

	cp.clients = make(map[string]bftconsensus.BFTConsensusUnaryClient)
	cp.conns = make(map[string]*grpc.ClientConn)
}

// VANETSimulator interface for network simulation
type VANETSimulator interface {
	SimulatePacketDrop() bool
	SimulateLatency() time.Duration
}

// VANETConfig holds network simulation parameters
type VANETConfig struct {
	PacketLossRate float64
	MinLatency     time.Duration
	MaxLatency     time.Duration
	Jitter         time.Duration
}

// DefaultVANETSimulator implements VANETSimulator with configurable network conditions
type DefaultVANETSimulator struct {
	config VANETConfig
	rng    *rand.Rand
	mu     sync.Mutex
}

// NewVANETSimulator creates a new VANET simulator
func NewVANETSimulator(config VANETConfig) *DefaultVANETSimulator {
	return &DefaultVANETSimulator{
		config: config,
		rng:    rand.New(rand.NewSource(time.Now().UnixNano())),
	}
}

// SimulatePacketDrop returns true if a packet should be dropped
func (vs *DefaultVANETSimulator) SimulatePacketDrop() bool {
	vs.mu.Lock()
	defer vs.mu.Unlock()
	return vs.rng.Float64() < vs.config.PacketLossRate
}

// SimulateLatency returns a simulated network delay
func (vs *DefaultVANETSimulator) SimulateLatency() time.Duration {
	vs.mu.Lock()
	defer vs.mu.Unlock()

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

// BroadcastTransport implements the Transport interface for broadcasting
type BroadcastTransport struct {
	pool     *ClientPool
	behavior byz.ByzBehavior
	vanetSim VANETSimulator
	vanetMu  sync.RWMutex
}

// NewBroadcastTransport creates a new broadcast transport
func NewBroadcastTransport(pool *ClientPool) *BroadcastTransport {
	return &BroadcastTransport{
		pool:     pool,
		behavior: nil, // Honest by default
		vanetSim: nil, // No VANET simulation by default
	}
}

// SetBehavior sets the Byzantine behavior for this transport
func (bt *BroadcastTransport) SetBehavior(behavior interface{}) {
	if byzBehavior, ok := behavior.(byz.ByzBehavior); ok {
		bt.behavior = byzBehavior
		log.Printf("[%s] Byzantine behavior set: %s", "BFT", byzBehavior.Name())
	}
}

// SetVANETSimulator sets the VANET simulator for network simulation
func (bt *BroadcastTransport) SetVANETSimulator(sim VANETSimulator) {
	bt.vanetMu.Lock()
	defer bt.vanetMu.Unlock()
	bt.vanetSim = sim
}

// applyVANETSimulation applies network simulation (packet loss, latency) before sending
// Returns true if message should be sent, false if dropped
func (bt *BroadcastTransport) applyVANETSimulation(ctx context.Context) bool {
	bt.vanetMu.RLock()
	sim := bt.vanetSim
	bt.vanetMu.RUnlock()

	if sim == nil {
		return true // No simulation, send immediately
	}

	// Check for packet loss
	if sim.SimulatePacketDrop() {
		return false // Drop message
	}

	// Apply network latency
	latency := sim.SimulateLatency()
	select {
	case <-time.After(latency):
		return true
	case <-ctx.Done():
		return false
	}
}

// BroadcastSendVote broadcasts a SEND_VOTE message to all peers
func (bt *BroadcastTransport) BroadcastSendVote(ctx context.Context, msg *bftconsensus.SendVote) error {
	clients := bt.pool.GetAllClients()
	peerIDs := make([]string, 0, len(clients))
	for id := range clients {
		peerIDs = append(peerIDs, id)
	}

	// Apply Byzantine behavior if set
	var targets []*byz.MessageTarget
	if bt.behavior != nil {
		var drop bool
		targets, drop = bt.behavior.OnSendVote(msg, peerIDs)
		if drop {
			return nil // Drop message
		}
	} else {
		// Honest behavior: send to all
		targets = []*byz.MessageTarget{{SendVote: msg, PeerIDs: peerIDs}}
	}

	var wg sync.WaitGroup
	errors := make(chan error, len(clients)*len(targets))

	for _, target := range targets {
		targetMsg := target.SendVote
		targetPeers := target.PeerIDs
		if len(targetPeers) == 0 {
			targetPeers = peerIDs // Send to all if not specified
		}

		for _, peerID := range targetPeers {
			client, exists := clients[peerID]
			if !exists {
				continue
			}

			wg.Add(1)
			go func(id string, c bftconsensus.BFTConsensusUnaryClient, m *bftconsensus.SendVote) {
				defer wg.Done()

				// Apply VANET network simulation (packet loss, latency)
				if !bt.applyVANETSimulation(ctx) {
					return // Message dropped or context cancelled
				}

				_, err := c.ReceiveSendVote(ctx, m)
				if err != nil {
					errors <- fmt.Errorf("failed to send SEND_VOTE to %s: %w", id, err)
				}
			}(peerID, client, targetMsg)
		}
	}

	wg.Wait()
	close(errors)

	// Collect errors (non-blocking)
	for err := range errors {
		log.Printf("Broadcast error: %v", err)
	}

	return nil
}

// BroadcastEchoVote broadcasts an ECHO_VOTE message to all peers
func (bt *BroadcastTransport) BroadcastEchoVote(ctx context.Context, msg *bftconsensus.EchoVote) error {
	clients := bt.pool.GetAllClients()
	peerIDs := make([]string, 0, len(clients))
	for id := range clients {
		peerIDs = append(peerIDs, id)
	}

	// Apply Byzantine behavior if set
	var targets []*byz.MessageTarget
	if bt.behavior != nil {
		var drop bool
		targets, drop = bt.behavior.OnEchoVote(msg, peerIDs)
		if drop {
			return nil
		}
	} else {
		targets = []*byz.MessageTarget{{EchoVote: msg, PeerIDs: peerIDs}}
	}

	var wg sync.WaitGroup
	errors := make(chan error, len(clients)*len(targets))

	for _, target := range targets {
		targetMsg := target.EchoVote
		targetPeers := target.PeerIDs
		if len(targetPeers) == 0 {
			targetPeers = peerIDs
		}

		for _, peerID := range targetPeers {
			client, exists := clients[peerID]
			if !exists {
				continue
			}

			wg.Add(1)
			go func(id string, c bftconsensus.BFTConsensusUnaryClient, m *bftconsensus.EchoVote) {
				defer wg.Done()

				// Apply VANET network simulation (packet loss, latency)
				if !bt.applyVANETSimulation(ctx) {
					return // Message dropped or context cancelled
				}

				_, err := c.ReceiveEchoVote(ctx, m)
				if err != nil {
					errors <- fmt.Errorf("failed to send ECHO_VOTE to %s: %w", id, err)
				}
			}(peerID, client, targetMsg)
		}
	}

	wg.Wait()
	close(errors)

	for err := range errors {
		log.Printf("Broadcast error: %v", err)
	}

	return nil
}

// BroadcastReadyVote broadcasts a READY_VOTE message to all peers
func (bt *BroadcastTransport) BroadcastReadyVote(ctx context.Context, msg *bftconsensus.ReadyVote) error {
	clients := bt.pool.GetAllClients()
	peerIDs := make([]string, 0, len(clients))
	for id := range clients {
		peerIDs = append(peerIDs, id)
	}

	// Apply Byzantine behavior if set
	var targets []*byz.MessageTarget
	if bt.behavior != nil {
		var drop bool
		targets, drop = bt.behavior.OnReadyVote(msg, peerIDs)
		if drop {
			return nil
		}
	} else {
		targets = []*byz.MessageTarget{{ReadyVote: msg, PeerIDs: peerIDs}}
	}

	var wg sync.WaitGroup
	errors := make(chan error, len(clients)*len(targets))

	for _, target := range targets {
		targetMsg := target.ReadyVote
		targetPeers := target.PeerIDs
		if len(targetPeers) == 0 {
			targetPeers = peerIDs
		}

		for _, peerID := range targetPeers {
			client, exists := clients[peerID]
			if !exists {
				continue
			}

			wg.Add(1)
			go func(id string, c bftconsensus.BFTConsensusUnaryClient, m *bftconsensus.ReadyVote) {
				defer wg.Done()

				// Apply VANET network simulation (packet loss, latency)
				if !bt.applyVANETSimulation(ctx) {
					return // Message dropped or context cancelled
				}

				_, err := c.ReceiveReadyVote(ctx, m)
				if err != nil {
					errors <- fmt.Errorf("failed to send READY_VOTE to %s: %w", id, err)
				}
			}(peerID, client, targetMsg)
		}
	}

	wg.Wait()
	close(errors)

	for err := range errors {
		log.Printf("Broadcast error: %v", err)
	}

	return nil
}

// BroadcastSendLeader broadcasts a SEND_LEADER message to all peers
func (bt *BroadcastTransport) BroadcastSendLeader(ctx context.Context, msg *bftconsensus.SendLeader) error {
	clients := bt.pool.GetAllClients()
	peerIDs := make([]string, 0, len(clients))
	for id := range clients {
		peerIDs = append(peerIDs, id)
	}

	// Apply Byzantine behavior if set
	var targets []*byz.MessageTarget
	if bt.behavior != nil {
		var drop bool
		targets, drop = bt.behavior.OnSendLeader(msg, peerIDs)
		if drop {
			return nil
		}
	} else {
		targets = []*byz.MessageTarget{{SendLeader: msg, PeerIDs: peerIDs}}
	}

	var wg sync.WaitGroup
	errors := make(chan error, len(clients)*len(targets))

	for _, target := range targets {
		targetMsg := target.SendLeader
		targetPeers := target.PeerIDs
		if len(targetPeers) == 0 {
			targetPeers = peerIDs
		}

		for _, peerID := range targetPeers {
			client, exists := clients[peerID]
			if !exists {
				continue
			}

			wg.Add(1)
			go func(id string, c bftconsensus.BFTConsensusUnaryClient, m *bftconsensus.SendLeader) {
				defer wg.Done()

				// Apply VANET network simulation (packet loss, latency)
				if !bt.applyVANETSimulation(ctx) {
					return // Message dropped or context cancelled
				}

				_, err := c.ReceiveSendLeader(ctx, m)
				if err != nil {
					errors <- fmt.Errorf("failed to send SEND_LEADER to %s: %w", id, err)
				}
			}(peerID, client, targetMsg)
		}
	}

	wg.Wait()
	close(errors)

	for err := range errors {
		log.Printf("Broadcast error: %v", err)
	}

	return nil
}

// BroadcastEchoLeader broadcasts an ECHO_LEADER message to all peers
func (bt *BroadcastTransport) BroadcastEchoLeader(ctx context.Context, msg *bftconsensus.EchoLeader) error {
	clients := bt.pool.GetAllClients()
	peerIDs := make([]string, 0, len(clients))
	for id := range clients {
		peerIDs = append(peerIDs, id)
	}

	// Apply Byzantine behavior if set
	var targets []*byz.MessageTarget
	if bt.behavior != nil {
		var drop bool
		targets, drop = bt.behavior.OnEchoLeader(msg, peerIDs)
		if drop {
			return nil
		}
	} else {
		targets = []*byz.MessageTarget{{EchoLeader: msg, PeerIDs: peerIDs}}
	}

	var wg sync.WaitGroup
	errors := make(chan error, len(clients)*len(targets))

	for _, target := range targets {
		targetMsg := target.EchoLeader
		targetPeers := target.PeerIDs
		if len(targetPeers) == 0 {
			targetPeers = peerIDs
		}

		for _, peerID := range targetPeers {
			client, exists := clients[peerID]
			if !exists {
				continue
			}

			wg.Add(1)
			go func(id string, c bftconsensus.BFTConsensusUnaryClient, m *bftconsensus.EchoLeader) {
				defer wg.Done()

				// Apply VANET network simulation (packet loss, latency)
				if !bt.applyVANETSimulation(ctx) {
					return // Message dropped or context cancelled
				}

				_, err := c.ReceiveEchoLeader(ctx, m)
				if err != nil {
					errors <- fmt.Errorf("failed to send ECHO_LEADER to %s: %w", id, err)
				}
			}(peerID, client, targetMsg)
		}
	}

	wg.Wait()
	close(errors)

	for err := range errors {
		log.Printf("Broadcast error: %v", err)
	}

	return nil
}

// BroadcastReadyLeader broadcasts a READY_LEADER message to all peers
func (bt *BroadcastTransport) BroadcastReadyLeader(ctx context.Context, msg *bftconsensus.ReadyLeader) error {
	clients := bt.pool.GetAllClients()
	peerIDs := make([]string, 0, len(clients))
	for id := range clients {
		peerIDs = append(peerIDs, id)
	}

	// Apply Byzantine behavior if set
	var targets []*byz.MessageTarget
	if bt.behavior != nil {
		var drop bool
		targets, drop = bt.behavior.OnReadyLeader(msg, peerIDs)
		if drop {
			return nil
		}
	} else {
		targets = []*byz.MessageTarget{{ReadyLeader: msg, PeerIDs: peerIDs}}
	}

	var wg sync.WaitGroup
	errors := make(chan error, len(clients)*len(targets))

	for _, target := range targets {
		targetMsg := target.ReadyLeader
		targetPeers := target.PeerIDs
		if len(targetPeers) == 0 {
			targetPeers = peerIDs
		}

		for _, peerID := range targetPeers {
			client, exists := clients[peerID]
			if !exists {
				continue
			}

			wg.Add(1)
			go func(id string, c bftconsensus.BFTConsensusUnaryClient, m *bftconsensus.ReadyLeader) {
				defer wg.Done()

				// Apply VANET network simulation (packet loss, latency)
				if !bt.applyVANETSimulation(ctx) {
					return // Message dropped or context cancelled
				}

				_, err := c.ReceiveReadyLeader(ctx, m)
				if err != nil {
					errors <- fmt.Errorf("failed to send READY_LEADER to %s: %w", id, err)
				}
			}(peerID, client, targetMsg)
		}
	}

	wg.Wait()
	close(errors)

	for err := range errors {
		log.Printf("Broadcast error: %v", err)
	}

	return nil
}

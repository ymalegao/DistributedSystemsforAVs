package transport

import (
	"fmt"
	"log"
	"net"

	"google.golang.org/grpc"

	"github.com/ymalegao/DistributedSystemsforAVs/pb/bftconsensus"
)

// Server wraps a gRPC server for BFT consensus
type Server struct {
	addr       string
	grpcServer *grpc.Server
	listener   net.Listener
}

// NewServer creates a new BFT server
func NewServer(addr string, handler bftconsensus.BFTConsensusUnaryServer) (*Server, error) {
	listener, err := net.Listen("tcp", addr)
	if err != nil {
		return nil, fmt.Errorf("failed to listen on %s: %w", addr, err)
	}

	grpcServer := grpc.NewServer()
	bftconsensus.RegisterBFTConsensusUnaryServer(grpcServer, handler)

	return &Server{
		addr:       addr,
		grpcServer: grpcServer,
		listener:   listener,
	}, nil
}

// Start starts the gRPC server
func (s *Server) Start() error {
	log.Printf("Starting BFT server on %s", s.addr)
	return s.grpcServer.Serve(s.listener)
}

// Stop stops the gRPC server
func (s *Server) Stop() {
	log.Printf("Stopping BFT server on %s", s.addr)
	s.grpcServer.GracefulStop()
}

// Addr returns the server address
func (s *Server) Addr() string {
	return s.addr
}

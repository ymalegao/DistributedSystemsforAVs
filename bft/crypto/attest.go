package crypto

import (
	"crypto/ed25519"
	"crypto/rand"
	"crypto/sha256"
	"encoding/binary"
	"fmt"

	"github.com/ymalegao/DistributedSystemsforAVs/pb/bftconsensus"
)

// KeyPair holds ed25519 public and private keys
type KeyPair struct {
	PublicKey  ed25519.PublicKey
	PrivateKey ed25519.PrivateKey
}

// GenerateKeyPair generates a new ed25519 key pair
func GenerateKeyPair() (*KeyPair, error) {
	pub, priv, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		return nil, fmt.Errorf("failed to generate keypair: %w", err)
	}
	return &KeyPair{
		PublicKey:  pub,
		PrivateKey: priv,
	}, nil
}

// HashPayload computes SHA256 hash of a protobuf message
func HashPayload(payload []byte) []byte {
	h := sha256.Sum256(payload)
	return h[:]
}

// HashVehicleMeta computes hash of BFTVehicleMeta
func HashVehicleMeta(meta *bftconsensus.BFTVehicleMeta) []byte {
	if meta == nil {
		return make([]byte, 32)
	}

	// Canonical encoding: id||plate||direction||status
	data := []byte(meta.Id + meta.Plate + meta.Direction)
	statusBuf := make([]byte, 4)
	binary.LittleEndian.PutUint32(statusBuf, uint32(meta.Status))
	data = append(data, statusBuf...)

	return HashPayload(data)
}

// HashLeaderPayload computes hash of (meta || QC1)
func HashLeaderPayload(meta *bftconsensus.BFTVehicleMeta, qc1 *bftconsensus.QuorumCert) []byte {
	metaHash := HashVehicleMeta(meta)
	qcBytes := CanonicalQCBytes(qc1)
	combined := append(metaHash, qcBytes...)
	return HashPayload(combined)
}

// CanonicalQCBytes creates canonical bytes for a QC
func CanonicalQCBytes(qc *bftconsensus.QuorumCert) []byte {
	if qc == nil {
		return make([]byte, 0)
	}

	// view||phase||subject_id||payload_hash
	viewBuf := make([]byte, 8)
	binary.LittleEndian.PutUint64(viewBuf, qc.View)

	phaseBuf := make([]byte, 4)
	binary.LittleEndian.PutUint32(phaseBuf, uint32(qc.Phase))

	data := append(viewBuf, phaseBuf...)
	data = append(data, []byte(qc.SubjectId)...)
	data = append(data, qc.PayloadHash...)

	return data
}

// MakeAttestationHash creates the canonical hash for signing
// H = Hash(view || phase || subject_id || payload_hash || signer_id)
func MakeAttestationHash(view uint64, phase bftconsensus.Phase, subjectID string, payloadHash []byte, signerID string) []byte {
	viewBuf := make([]byte, 8)
	binary.LittleEndian.PutUint64(viewBuf, view)

	phaseBuf := make([]byte, 4)
	binary.LittleEndian.PutUint32(phaseBuf, uint32(phase))

	data := viewBuf
	data = append(data, phaseBuf...)
	data = append(data, []byte(subjectID)...)
	data = append(data, payloadHash...)
	data = append(data, []byte(signerID)...)

	return HashPayload(data)
}

// SignAttestation creates a signed attestation
func SignAttestation(kp *KeyPair, view uint64, phase bftconsensus.Phase, subjectID string, payloadHash []byte, signerID string) *bftconsensus.Attestation {
	hash := MakeAttestationHash(view, phase, subjectID, payloadHash, signerID)
	signature := ed25519.Sign(kp.PrivateKey, hash)

	return &bftconsensus.Attestation{
		View:        view,
		Phase:       phase,
		SubjectId:   subjectID,
		PayloadHash: payloadHash,
		SignerId:    signerID,
		Sig:         signature,
	}
}

// VerifyAttestation verifies an attestation signature
func VerifyAttestation(att *bftconsensus.Attestation, pubKey ed25519.PublicKey) bool {
	if att == nil {
		return false
	}

	hash := MakeAttestationHash(att.View, att.Phase, att.SubjectId, att.PayloadHash, att.SignerId)
	return ed25519.Verify(pubKey, hash, att.Sig)
}

// VerifyQC verifies a quorum certificate has at least quorumSize valid signatures
func VerifyQC(qc *bftconsensus.QuorumCert, pubKeys map[string]ed25519.PublicKey, quorumSize int) bool {
	if qc == nil || len(qc.Sigs) < quorumSize {
		return false
	}

	// Check all signatures are valid and distinct
	seen := make(map[string]bool)
	validCount := 0

	for _, att := range qc.Sigs {
		// Must be distinct signers
		if seen[att.SignerId] {
			return false
		}
		seen[att.SignerId] = true

		// Must match QC parameters
		if att.View != qc.View || att.Phase != qc.Phase ||
			att.SubjectId != qc.SubjectId || string(att.PayloadHash) != string(qc.PayloadHash) {
			return false
		}

		// Must have valid signature
		pubKey, exists := pubKeys[att.SignerId]
		if !exists {
			return false
		}

		if VerifyAttestation(att, pubKey) {
			validCount++
		}
	}

	return validCount >= quorumSize
}

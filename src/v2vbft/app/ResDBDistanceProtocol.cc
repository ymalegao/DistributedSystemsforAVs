#include "veins/modules/application/resDB/ResDBIntersectionApp.h"
#include "veins/modules/application/resDB/ResDBUtil.h"
#include "veins/modules/application/resDB/messages/BFTMessage_m.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>

#include <openssl/evp.h>

using namespace omnetpp;
using namespace veins;
using namespace veins::resdb_app_util;

namespace {

std::vector<uint8_t> messagePayload(BFTMessage* msg)
{
    std::vector<uint8_t> out(msg ? msg->getPayloadArraySize() : 0);
    for (size_t i = 0; i < out.size(); ++i) out[i] = msg->getPayload(i);
    return out;
}

void setMessagePayload(BFTMessage& msg, const std::vector<uint8_t>& bytes)
{
    msg.setPayloadArraySize(bytes.size());
    for (size_t i = 0; i < bytes.size(); ++i) msg.setPayload(i, bytes[i]);
}

std::array<uint8_t, 32> sha256(const std::vector<uint8_t>& bytes)
{
    std::array<uint8_t, 32> out{};
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    unsigned int len = 0;
    const bool ok = ctx && EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1 &&
        EVP_DigestUpdate(ctx, bytes.data(), bytes.size()) == 1 &&
        EVP_DigestFinal_ex(ctx, out.data(), &len) == 1 && len == out.size();
    if (ctx) EVP_MD_CTX_free(ctx);
    if (!ok) out.fill(0);
    return out;
}

void appendU32(std::vector<uint8_t>& out, uint32_t value)
{
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>(value & 0xff));
}

bool readU32(const std::vector<uint8_t>& in, size_t& offset, uint32_t& value)
{
    if (offset + 4 > in.size()) return false;
    value = (static_cast<uint32_t>(in[offset]) << 24) |
        (static_cast<uint32_t>(in[offset + 1]) << 16) |
        (static_cast<uint32_t>(in[offset + 2]) << 8) |
        static_cast<uint32_t>(in[offset + 3]);
    offset += 4;
    return true;
}

} // namespace

std::string ResDBIntersectionApp::canonicalStoppedDistanceAttestationPayload(
    const StoppedDistanceAttestation& att) const
{
    return att.targetCarId + "|" + std::to_string(att.epoch) + "|" +
        hashHex(att.earlyClaimHash) + "|" + std::to_string(att.distanceToStopCm);
}

std::vector<uint8_t> ResDBIntersectionApp::serializeStoppedDistanceAttestation(
    const StoppedDistanceAttestation& att) const
{
    std::ostringstream ss;
    ss << canonicalStoppedDistanceAttestationPayload(att) << "|"
       << att.signature.size() << "|";
    const std::string hdr = ss.str();
    std::vector<uint8_t> out(hdr.begin(), hdr.end());
    out.insert(out.end(), att.signature.begin(), att.signature.end());
    out.resize(hdr.size() + CRYPTO_SIG_MAX_BYTES, 0);
    return out;
}

ResDBIntersectionApp::StoppedDistanceAttestation
ResDBIntersectionApp::deserializeStoppedDistanceAttestation(BFTMessage* msg) const
{
    StoppedDistanceAttestation att;
    const std::vector<uint8_t> bytes = messagePayload(msg);
    const std::string text(bytes.begin(), bytes.end());
    const auto parts = splitStr(text, '|');
    if (parts.size() < 6) return att;
    try {
        att.targetCarId = parts[0];
        att.epoch = std::stoi(parts[1]);
        const auto early = fromHex(parts[2]);
        if (early.size() != att.earlyClaimHash.size()) return {};
        std::copy(early.begin(), early.end(), att.earlyClaimHash.begin());
        att.distanceToStopCm = std::stoi(parts[3]);
        const int sigLen = std::stoi(parts[4]);
        size_t delimiter = 0;
        for (int i = 0; i < 5; ++i) {
            delimiter = text.find('|', delimiter);
            if (delimiter == std::string::npos) return {};
            ++delimiter;
        }
        if (sigLen <= 0 || sigLen > CRYPTO_SIG_MAX_BYTES ||
                delimiter + static_cast<size_t>(sigLen) > bytes.size()) return {};
        att.signature.assign(bytes.begin() + delimiter, bytes.begin() + delimiter + sigLen);
    } catch (...) {
        return {};
    }
    return att;
}

std::array<uint8_t, 32> ResDBIntersectionApp::stoppedDistanceAttestationHash(
    const StoppedDistanceAttestation& att) const
{
    return sha256(serializeStoppedDistanceAttestation(att));
}

bool ResDBIntersectionApp::validateStoppedDistanceAttestation(
    const StoppedDistanceAttestation& att) const
{
    const int origin = extractReplicaId(att.targetCarId);
    uint8_t key[CRYPTO_PUBKEY_BYTES] = {};
    if (origin < 0 || att.epoch != static_cast<int>(current_epoch_) ||
            att.distanceToStopCm < 0 || att.signature.empty() ||
            !WitnessKeyRegistry::instance().copyKey(origin, key)) return false;
    const std::string payload = canonicalStoppedDistanceAttestationPayload(att);
    return CryptoAuth::instance().verifyBytes(
        key, reinterpret_cast<const uint8_t*>(payload.data()), payload.size(),
        att.signature.data(), static_cast<uint8_t>(att.signature.size()));
}

std::string ResDBIntersectionApp::stoppedDistanceEchoSigningPayload(
    const StoppedDistanceEcho& echo) const
{
    return std::to_string(echo.echoingReplicaId) + "|" + echo.targetCarId + "|" +
        std::to_string(echo.epoch) + "|" + hashHex(echo.earlyClaimHash) + "|" +
        hashHex(echo.attestationHash) + "|" + std::to_string(echo.distanceToStopCm);
}

std::vector<uint8_t> ResDBIntersectionApp::serializeStoppedDistanceEcho(
    const StoppedDistanceEcho& echo) const
{
    const std::vector<uint8_t> pub(echo.signerPubKey,
        echo.signerPubKey + CRYPTO_PUBKEY_BYTES);
    const std::vector<uint8_t> sig(echo.signature,
        echo.signature + CRYPTO_SIG_MAX_BYTES);
    std::ostringstream ss;
    ss << stoppedDistanceEchoSigningPayload(echo) << "|" << toHex(pub) << ","
       << static_cast<int>(echo.signatureLen) << "," << toHex(sig);
    const std::string text = ss.str();
    return {text.begin(), text.end()};
}

ResDBIntersectionApp::StoppedDistanceEcho
ResDBIntersectionApp::deserializeStoppedDistanceEcho(BFTMessage* msg) const
{
    StoppedDistanceEcho echo;
    const auto bytes = messagePayload(msg);
    const std::string text(bytes.begin(), bytes.end());
    const auto parts = splitStr(text, '|');
    if (parts.size() < 7) return echo;
    try {
        echo.echoingReplicaId = std::stoi(parts[0]);
        echo.targetCarId = parts[1];
        echo.epoch = std::stoi(parts[2]);
        auto early = fromHex(parts[3]);
        auto attHash = fromHex(parts[4]);
        if (early.size() != echo.earlyClaimHash.size() ||
                attHash.size() != echo.attestationHash.size()) return {};
        std::copy(early.begin(), early.end(), echo.earlyClaimHash.begin());
        std::copy(attHash.begin(), attHash.end(), echo.attestationHash.begin());
        echo.distanceToStopCm = std::stoi(parts[5]);
        const std::string& auth = parts[6];
        const size_t c1 = auth.find(',');
        const size_t c2 = c1 == std::string::npos ? c1 : auth.find(',', c1 + 1);
        if (c1 == std::string::npos || c2 == std::string::npos) return {};
        const auto pub = fromHex(auth.substr(0, c1));
        const int sigLen = std::stoi(auth.substr(c1 + 1, c2 - c1 - 1));
        const auto sig = fromHex(auth.substr(c2 + 1));
        if (pub.size() != CRYPTO_PUBKEY_BYTES || sig.size() != CRYPTO_SIG_MAX_BYTES ||
                sigLen <= 0 || sigLen > CRYPTO_SIG_MAX_BYTES) return {};
        std::memcpy(echo.signerPubKey, pub.data(), pub.size());
        std::memcpy(echo.signature, sig.data(), sig.size());
        echo.signatureLen = static_cast<uint8_t>(sigLen);
    } catch (...) {
        return {};
    }
    return echo;
}

std::vector<uint8_t> ResDBIntersectionApp::serializeStoppedDistanceCert(
    const StoppedDistanceCert& cert) const
{
    // Length-prefixed binary envelope. The signed nested objects retain their
    // canonical encodings, but are not hex-expanded on the radio. This cuts a
    // Type-20 certificate roughly in half without changing any signed bytes.
    std::vector<uint8_t> out{'S', 'D', 'C', 1};
    const auto att = serializeStoppedDistanceAttestation(cert.attestation);
    appendU32(out, static_cast<uint32_t>(att.size()));
    out.insert(out.end(), att.begin(), att.end());
    appendU32(out, static_cast<uint32_t>(cert.echoes.size()));
    for (const auto& echo : cert.echoes) {
        const auto bytes = serializeStoppedDistanceEcho(echo);
        appendU32(out, static_cast<uint32_t>(bytes.size()));
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
    return out;
}

ResDBIntersectionApp::StoppedDistanceCert
ResDBIntersectionApp::deserializeStoppedDistanceCert(BFTMessage* msg) const
{
    StoppedDistanceCert cert;
    const auto bytes = messagePayload(msg);
    if (bytes.size() < 12 || bytes[0] != 'S' || bytes[1] != 'D' ||
            bytes[2] != 'C' || bytes[3] != 1) return cert;
    size_t offset = 4;
    uint32_t attLen = 0;
    if (!readU32(bytes, offset, attLen) || attLen == 0 ||
            offset + attLen > bytes.size()) return {};
    BFTMessage nested;
    setMessagePayload(nested, std::vector<uint8_t>(
        bytes.begin() + offset, bytes.begin() + offset + attLen));
    cert.attestation = deserializeStoppedDistanceAttestation(&nested);
    offset += attLen;
    uint32_t echoCount = 0;
    if (!readU32(bytes, offset, echoCount) ||
            echoCount > static_cast<uint32_t>(std::max(0, total_vehicles_ - 1))) return {};
    for (uint32_t i = 0; i < echoCount; ++i) {
        uint32_t echoLen = 0;
        if (!readU32(bytes, offset, echoLen) || echoLen == 0 ||
                offset + echoLen > bytes.size()) return {};
        BFTMessage echoMsg;
        setMessagePayload(echoMsg, std::vector<uint8_t>(
            bytes.begin() + offset, bytes.begin() + offset + echoLen));
        cert.echoes.push_back(deserializeStoppedDistanceEcho(&echoMsg));
        offset += echoLen;
    }
    if (offset != bytes.size()) return {};
    return cert;
}

void ResDBIntersectionApp::maybeBroadcastStoppedDistanceAttestation()
{
    if (stopped_distance_attestation_sent_ || !entered_stop_zone_ ||
            discovery_.state != DiscoveryState::COLLECTING || propose_submitted_ ||
            order_applied_ || crashCommsDisabled_ || !ec_private_key_) return;
    const std::string carId = "veh" + std::to_string(replicaId_);
    if (vehicleSpeedTraCI(carId) > distance_stationary_speed_mps_ || !mobility ||
            !mobility->getCommandInterface()) return;
    auto claimIt = local_claim_hashes_.find(carId);
    if (claimIt == local_claim_hashes_.end()) return;

    try {
        auto vehicle = mobility->getCommandInterface()->vehicle(carId);
        const std::string laneId = vehicle.getLaneId();
        if (laneId.empty() || laneId.front() == ':') return;
        const double laneLength = mobility->getCommandInterface()->lane(laneId).getLength();
        StoppedDistanceAttestation att;
        att.targetCarId = carId;
        att.epoch = static_cast<int>(current_epoch_);
        att.earlyClaimHash = claimIt->second;
        const double trueDistanceM =
            std::max(0.0, laneLength - vehicle.getLanePosition());
        const bool falseDistanceTarget =
            phase2_attack_kind_ == Phase2AttackKind::FALSE_DISTANCE &&
            replicaId_ == phase2_attack_target_replica_id_;
        const double claimDistanceM = std::max(0.0, trueDistanceM +
            (falseDistanceTarget ? phase2_distance_claim_offset_m_ : 0.0));
        att.distanceToStopCm = static_cast<int32_t>(std::llround(
            claimDistanceM * 100.0));
        const std::string payload = canonicalStoppedDistanceAttestationPayload(att);
        uint8_t signature[CRYPTO_SIG_MAX_BYTES] = {};
        uint8_t signatureLen = 0;
        if (!CryptoAuth::instance().signBytes(
                ec_private_key_, reinterpret_cast<const uint8_t*>(payload.data()),
                payload.size(), signature, signatureLen)) return;
        att.signature.assign(signature, signature + signatureLen);
        local_distance_attestation_ = att;
        stopped_distance_attestation_sent_ = true;
        stopped_distance_attestation_retry_count_ = 0;
        sendBFTMessage(-1, serializeStoppedDistanceAttestation(att),
                       kStoppedDistanceAttestationType);
        scheduleStoppedDistanceAttestationRetry();
        std::cout << "[STOPPED-DISTANCE-ATTEST] target=" << carId
                  << " epoch=" << att.epoch
                  << " earlyClaimHash=" << hashHex(att.earlyClaimHash)
                  << " distanceToStopCm=" << att.distanceToStopCm
                  << " speed=" << vehicle.getSpeed() << "\n";
        if (falseDistanceTarget)
            std::cout << "[DIST-ATTACK-DECLARE] target=" << carId
                      << " epoch=" << att.epoch
                      << " trueDistanceM=" << trueDistanceM
                      << " claimedDistanceM=" << claimDistanceM
                      << " offsetM=" << phase2_distance_claim_offset_m_ << "\n";
        const int required = (tolerated_faults_ >= 0 ? tolerated_faults_ :
            (total_vehicles_ - 1) / 3) + 1;
        if (required <= 1 && stopped_distance_finalize_timer_ &&
                !stopped_distance_finalize_timer_->isScheduled())
            scheduleAt(simTime() + direction_eligibility_collection_window_sec_,
                       stopped_distance_finalize_timer_);
    } catch (...) {
    }
}

void ResDBIntersectionApp::scheduleStoppedDistanceAttestationRetry()
{
    if (!stopped_distance_attestation_retry_timer_ ||
            stopped_distance_attestation_retry_timer_->isScheduled() ||
            !stopped_distance_attestation_sent_ || stopped_distance_cert_broadcast_ ||
            stopped_distance_attestation_retry_count_ >=
                stopped_distance_attestation_retry_max_ ||
            discovery_.state != DiscoveryState::COLLECTING || propose_submitted_ ||
            order_applied_ || crashCommsDisabled_ ||
            current_phase_ == ConsensusPhase::DEPARTED) return;
    scheduleAt(simTime() + stopped_distance_attestation_retry_interval_sec_,
               stopped_distance_attestation_retry_timer_);
}

void ResDBIntersectionApp::cancelStoppedDistanceAttestationRetry()
{
    if (stopped_distance_attestation_retry_timer_ &&
            stopped_distance_attestation_retry_timer_->isScheduled())
        cancelEvent(stopped_distance_attestation_retry_timer_);
}

void ResDBIntersectionApp::retryStoppedDistanceAttestation()
{
    if (!stopped_distance_attestation_sent_ || stopped_distance_cert_broadcast_ ||
            discovery_.state != DiscoveryState::COLLECTING || propose_submitted_ ||
            order_applied_ || crashCommsDisabled_ ||
            current_phase_ == ConsensusPhase::DEPARTED ||
            stopped_distance_attestation_retry_count_ >=
                stopped_distance_attestation_retry_max_) return;
    ++stopped_distance_attestation_retry_count_;
    sendBFTMessage(-1, serializeStoppedDistanceAttestation(local_distance_attestation_),
                   kStoppedDistanceAttestationType);
    std::cout << "[STOPPED-DISTANCE-ATTEST-RETRY] target="
              << local_distance_attestation_.targetCarId
              << " epoch=" << local_distance_attestation_.epoch
              << " attempt=" << stopped_distance_attestation_retry_count_
              << " max=" << stopped_distance_attestation_retry_max_ << "\n";
    scheduleStoppedDistanceAttestationRetry();
}

void ResDBIntersectionApp::handleStoppedDistanceAttestation(BFTMessage* msg)
{
    const StoppedDistanceAttestation att = deserializeStoppedDistanceAttestation(msg);
    if (!validateStoppedDistanceAttestation(att) || propose_submitted_ || order_applied_ ||
            current_phase_ == ConsensusPhase::DEPARTED) return;
    auto known = local_claim_hashes_.find(att.targetCarId);
    bool bound = known != local_claim_hashes_.end() && known->second == att.earlyClaimHash;
    if (!bound) {
        std::lock_guard<std::mutex> lk(certs_mutex_);
        auto cert = collected_certs_.find(att.targetCarId);
        bound = cert != collected_certs_.end() && cert->second.claimHash == att.earlyClaimHash;
    }
    if (!bound) {
        const std::string pendingKey = att.targetCarId + ":" + std::to_string(att.epoch)
            + ":" + hashHex(att.earlyClaimHash);
        pending_distance_attestations_.emplace(pendingKey, att);
        std::cout << "[DIST-ATTEST-PENDING] witness=" << replicaId_
                  << " target=" << att.targetCarId << " epoch=" << att.epoch
                  << " reason=early-claim-not-yet-known\n";
        return;
    }

    const auto attHash = stoppedDistanceAttestationHash(att);
    const std::string key = std::to_string(replicaId_) + ":" + att.targetCarId + ":" +
        std::to_string(att.epoch) + ":" + hashHex(attHash);
    if (stopped_distance_samples_.count(key)) return;
    const bool colluding =
        phase2_attack_kind_ == Phase2AttackKind::FALSE_DISTANCE &&
        extractReplicaId(att.targetCarId) == phase2_attack_target_replica_id_ &&
        phase2_evidence_colluder_ids_.count(replicaId_) > 0;
    const auto sample = colluding ? StoppedDistancePerceptionSample{} :
        perception_->observeStoppedDistance(
            att.targetCarId, simTime(), distance_stationary_speed_mps_);
    stopped_distance_samples_[key] = sample;
    const int32_t observedCm = static_cast<int32_t>(std::llround(
        sample.observedDistanceToStopM * 100.0));
    const int32_t residualCm = std::abs(observedCm - att.distanceToStopCm);
    const int32_t toleranceCm = static_cast<int32_t>(std::llround(
        par("physicalGateK").doubleValue() *
        par("longitudinalObservationSigmaM").doubleValue() * 100.0));
    const bool accept = colluding || (sample.detected && sample.stationary &&
        sample.distanceValid && residualCm <= toleranceCm);
    if (!colluding) {
        std::cout << "[DIST-PERC-EVAL] witness=" << replicaId_
                  << " target=" << att.targetCarId << " epoch=" << att.epoch
                  << " attestationHash=" << hashHex(attHash)
                  << " verdict=" << (accept ? "ACCEPT" : "REJECT")
                  << " trueDistanceM=" << sample.trueDistanceToStopM
                  << " observedDistanceM=" << sample.observedDistanceToStopM
                  << " claimedDistanceCm=" << att.distanceToStopCm
                  << " residualCm=" << residualCm << " toleranceCm=" << toleranceCm
                  << " stationary=" << (sample.stationary ? 1 : 0) << "\n";
    } else {
        std::cout << "[DIST-COLLUSION-ECHO] target=" << att.targetCarId
                  << " epoch=" << att.epoch << " signer=" << replicaId_
                  << " claimedDistanceCm=" << att.distanceToStopCm << "\n";
    }
    if (!accept) return;

    StoppedDistanceEcho echo;
    echo.echoingReplicaId = replicaId_;
    echo.targetCarId = att.targetCarId;
    echo.epoch = att.epoch;
    echo.earlyClaimHash = att.earlyClaimHash;
    echo.attestationHash = attHash;
    echo.distanceToStopCm = att.distanceToStopCm;
    std::memcpy(echo.signerPubKey, ec_pub_key_, CRYPTO_PUBKEY_BYTES);
    const std::string payload = stoppedDistanceEchoSigningPayload(echo);
    if (!CryptoAuth::instance().signBytes(
            ec_private_key_, reinterpret_cast<const uint8_t*>(payload.data()), payload.size(),
            echo.signature, echo.signatureLen)) return;
    sendBFTMessage(-1, serializeStoppedDistanceEcho(echo),
                   kStoppedDistanceEchoType, false, true);
}

void ResDBIntersectionApp::processPendingStoppedDistanceAttestation(
    const std::string& carId)
{
    for (auto it = pending_distance_attestations_.begin();
            it != pending_distance_attestations_.end();) {
        if (it->second.targetCarId != carId) {
            ++it;
            continue;
        }
        const StoppedDistanceAttestation att = it->second;
        it = pending_distance_attestations_.erase(it);
        BFTMessage msg;
        setMessagePayload(msg, serializeStoppedDistanceAttestation(att));
        handleStoppedDistanceAttestation(&msg);
    }
}

void ResDBIntersectionApp::collectStoppedDistanceEcho(const StoppedDistanceEcho& echo)
{
    if (!stopped_distance_attestation_sent_ || stopped_distance_cert_broadcast_ ||
            echo.targetCarId != local_distance_attestation_.targetCarId ||
            echo.epoch != local_distance_attestation_.epoch ||
            echo.earlyClaimHash != local_distance_attestation_.earlyClaimHash ||
            echo.attestationHash != stoppedDistanceAttestationHash(local_distance_attestation_) ||
            echo.distanceToStopCm != local_distance_attestation_.distanceToStopCm ||
            echo.signatureLen == 0 || !isArrivalSignerEligible(echo.echoingReplicaId) ||
            echo.echoingReplicaId == replicaId_ ||
            !WitnessKeyRegistry::instance().matches(echo.echoingReplicaId, echo.signerPubKey)) return;
    const std::string payload = stoppedDistanceEchoSigningPayload(echo);
    if (!CryptoAuth::instance().verifyBytes(
            echo.signerPubKey, reinterpret_cast<const uint8_t*>(payload.data()), payload.size(),
            echo.signature, echo.signatureLen)) return;
    for (const auto& prior : my_received_distance_echoes_)
        if (prior.echoingReplicaId == echo.echoingReplicaId) return;
    if (my_received_distance_echoes_.size() >= static_cast<size_t>(std::max(0, total_vehicles_ - 1)))
        return;
    my_received_distance_echoes_.push_back(echo);
    const int required = (tolerated_faults_ >= 0 ? tolerated_faults_ :
        (total_vehicles_ - 1) / 3) + 1;
    const int signerCount = 1 + static_cast<int>(my_received_distance_echoes_.size());
    std::cout << "[DIST-CERT-COLLECT] target=" << echo.targetCarId
              << " epoch=" << echo.epoch << " signerCount=" << signerCount
              << " threshold=" << required << " signer=" << echo.echoingReplicaId << "\n";
    if (signerCount >= required && stopped_distance_finalize_timer_ &&
            !stopped_distance_finalize_timer_->isScheduled()) {
        // The threshold proves at least one cached attestation transmission was
        // received. Stop retries immediately; continue only passive echo
        // collection during the existing post-threshold window.
        cancelStoppedDistanceAttestationRetry();
        scheduleAt(simTime() + direction_eligibility_collection_window_sec_,
                   stopped_distance_finalize_timer_);
    }
}

void ResDBIntersectionApp::handleStoppedDistanceEcho(BFTMessage* msg)
{
    collectStoppedDistanceEcho(deserializeStoppedDistanceEcho(msg));
}

bool ResDBIntersectionApp::finalizeLocalStoppedDistanceCert(const char* reason)
{
    if (!stopped_distance_attestation_sent_ || stopped_distance_cert_broadcast_) return false;
    const int required = (tolerated_faults_ >= 0 ? tolerated_faults_ :
        (total_vehicles_ - 1) / 3) + 1;
    if (1 + static_cast<int>(my_received_distance_echoes_.size()) < required) return false;
    cancelStoppedDistanceFinalizeTimer();
    cancelStoppedDistanceAttestationRetry();
    StoppedDistanceCert cert;
    cert.attestation = local_distance_attestation_;
    cert.echoes = my_received_distance_echoes_;
    if (!validateStoppedDistanceCert(cert)) return false;
    stopped_distance_cert_broadcast_ = true;
    discovery_.localDistanceCert = LocalCertState::QUEUED;
    const auto certBytes = serializeStoppedDistanceCert(cert);
    sendBFTMessage(-1, certBytes, kStoppedDistanceCertType, true);
    // One bounded origin retransmission uses the existing cancellable/drained
    // discovery queue. There is no new timer or resampling, and discovery
    // cannot complete until this queued certificate copy has aired or the
    // lifecycle explicitly cancels it.
    const simtime_t retryAt = simTime() +
        stopped_distance_attestation_retry_interval_sec_ +
        replicaId_ * par("arrivalSlotSec").doubleValue();
    enqueueDiscoveryTx(-1, certBytes, kStoppedDistanceCertType, retryAt, true, false);
    std::cout << "[DIST-CERT-RETRY-QUEUED] target="
              << cert.attestation.targetCarId << " epoch=" << cert.attestation.epoch
              << " retryAt=" << retryAt << "\n";
    {
        std::lock_guard<std::mutex> lk(certs_mutex_);
        collected_distance_certs_[cert.attestation.targetCarId] = cert;
    }
    std::cout << "[DIST-CERT-ASSEMBLE] target=" << cert.attestation.targetCarId
              << " epoch=" << cert.attestation.epoch
              << " signerCount=" << 1 + cert.echoes.size()
              << " threshold=" << required
              << " reason=" << (reason ? reason : "threshold") << "\n";
    const int claimant = extractReplicaId(cert.attestation.targetCarId);
    std::cout << "[DIST-CERT-EVIDENCE] target=" << cert.attestation.targetCarId
              << " epoch=" << cert.attestation.epoch << " signer=" << claimant
              << " self=1 byzantine="
              << (phase2_byzantine_replica_ids_.count(claimant) ? 1 : 0) << "\n";
    for (const auto& echo : cert.echoes)
        std::cout << "[DIST-CERT-EVIDENCE] target=" << cert.attestation.targetCarId
                  << " epoch=" << cert.attestation.epoch
                  << " signer=" << echo.echoingReplicaId
                  << " self=0 byzantine="
                  << (phase2_byzantine_replica_ids_.count(echo.echoingReplicaId) ? 1 : 0)
                  << "\n";
    maybeAdvanceDiscovery("distance-self-cert");
    return true;
}

void ResDBIntersectionApp::cancelStoppedDistanceFinalizeTimer()
{
    if (stopped_distance_finalize_timer_ && stopped_distance_finalize_timer_->isScheduled())
        cancelEvent(stopped_distance_finalize_timer_);
    cancelStoppedDistanceAttestationRetry();
}

bool ResDBIntersectionApp::validateStoppedDistanceCert(const StoppedDistanceCert& cert) const
{
    if (!validateStoppedDistanceAttestation(cert.attestation)) return false;
    const int required = (tolerated_faults_ >= 0 ? tolerated_faults_ :
        (total_vehicles_ - 1) / 3) + 1;
    if (1 + static_cast<int>(cert.echoes.size()) < required ||
            cert.echoes.size() > static_cast<size_t>(std::max(0, total_vehicles_ - 1))) return false;
    const auto attHash = stoppedDistanceAttestationHash(cert.attestation);
    std::set<int> seen{extractReplicaId(cert.attestation.targetCarId)};
    for (const auto& echo : cert.echoes) {
        if (!seen.insert(echo.echoingReplicaId).second ||
                !isArrivalSignerEligible(echo.echoingReplicaId) || echo.signatureLen == 0 ||
                echo.targetCarId != cert.attestation.targetCarId ||
                echo.epoch != cert.attestation.epoch ||
                echo.earlyClaimHash != cert.attestation.earlyClaimHash ||
                echo.attestationHash != attHash ||
                echo.distanceToStopCm != cert.attestation.distanceToStopCm ||
                !WitnessKeyRegistry::instance().matches(
                    echo.echoingReplicaId, echo.signerPubKey)) return false;
        const std::string payload = stoppedDistanceEchoSigningPayload(echo);
        if (!CryptoAuth::instance().verifyBytes(
                echo.signerPubKey, reinterpret_cast<const uint8_t*>(payload.data()), payload.size(),
                echo.signature, echo.signatureLen)) return false;
    }
    return true;
}

void ResDBIntersectionApp::handleStoppedDistanceCert(BFTMessage* msg)
{
    const StoppedDistanceCert cert = deserializeStoppedDistanceCert(msg);
    if (!validateStoppedDistanceCert(cert)) return;
    const std::string& carId = cert.attestation.targetCarId;
    {
        std::lock_guard<std::mutex> lk(certs_mutex_);
        if (collected_distance_certs_.count(carId)) return;
        auto arrival = collected_certs_.find(carId);
        if (arrival == collected_certs_.end() ||
                arrival->second.claimHash != cert.attestation.earlyClaimHash) return;
        collected_distance_certs_[carId] = cert;
    }
    std::cout << "[DIST-CERT-STORED] r" << replicaId_ << " target=" << carId
              << " epoch=" << cert.attestation.epoch
              << " distanceToStopCm=" << cert.attestation.distanceToStopCm << "\n";
    if (discovery_.state == DiscoveryState::COLLECTING)
        sendBFTMessage(-1, serializeStoppedDistanceCert(cert), kStoppedDistanceCertType);
    maybeAdvanceDiscovery("distance-cert-stored");
}

std::map<std::string, uint8_t> ResDBIntersectionApp::deriveQueueRanks(
    const std::map<std::string, ArrivalCert>& arrivals,
    const std::map<std::string, StoppedDistanceCert>& distances) const
{
    std::map<std::string, uint8_t> ranks;
    std::map<std::string, std::vector<std::pair<int32_t, std::string>>> queues;
    for (const auto& kv : arrivals) {
        auto distance = distances.find(kv.first);
        if (distance == distances.end()) continue;
        // Cardinal approach remains the scheduler lane. Within an adjacent-
        // lane fixture, the authenticated physical-lane index only separates
        // the longitudinal queues used to derive same-lane rank.
        queues[kv.second.lane + ":" + std::to_string(kv.second.physicalLaneIndex)].push_back(
            {distance->second.attestation.distanceToStopCm, kv.first});
    }
    for (auto& queue : queues) {
        std::sort(queue.second.begin(), queue.second.end(),
            [](const auto& a, const auto& b) {
                return a.first != b.first ? a.first < b.first : a.second < b.second;
            });
        for (size_t i = 0; i < queue.second.size(); ++i)
            ranks[queue.second[i].second] = static_cast<uint8_t>(
                std::min<size_t>(i + 1, 255));
    }
    return ranks;
}

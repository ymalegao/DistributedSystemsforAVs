#include "veins/modules/application/resDB/ResDBIntersectionApp.h"
#include "veins/modules/application/resDB/ResDBUtil.h"
#include "veins/modules/application/resDB/ResdbV2VWire.h"
#include "veins/modules/application/resDB/messages/BFTMessage_m.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <vector>
#include <openssl/evp.h>

using namespace veins;
using namespace veins::resdb_app_util;

namespace {

std::vector<uint8_t> payloadBytes(BFTMessage* msg)
{
    std::vector<uint8_t> payload(msg->getPayloadArraySize());
    for (size_t i = 0; i < payload.size(); ++i)
        payload[i] = msg->getPayload(i);
    return payload;
}

BFTMessage* makeArrivalAnnouncementMessage(const std::vector<uint8_t>& payload,
                                           int fromReplicaId,
                                           int toReplicaId)
{
    BFTMessage* msg = new BFTMessage("gossipedArrivalAnnouncement");
    msg->setFromReplicaId(fromReplicaId);
    msg->setToReplicaId(toReplicaId);
    msg->setPayloadArraySize(payload.size());
    for (size_t i = 0; i < payload.size(); ++i)
        msg->setPayload(i, payload[i]);
    msg->setPayloadLength((int)payload.size());
    return msg;
}

} // namespace

std::vector<uint8_t> ResDBIntersectionApp::serializeArrivalAnnouncement(
        const ArrivalAnnouncement& ann) const
{
    std::stringstream ss;
    ss << std::setprecision(17);
    ss << ann.carId          << "|"
       << ann.laneId         << "|"
       << ann.lane           << "|"
       << ann.positionInLane << "|"
       << dirToStr(ann.direction) << "|"
       << (ann.isAmbulance ? "1" : "0") << "|"
       << ann.claimedArrivalTime << "|"
       << ann.epoch          << "|"
       << toHex(ann.ambulanceCertBytes) << "|"
       << toHex(ann.ambulanceSigBytes)  << "|"
       << ann.signature.size() << "|";
    std::string hdr = ss.str();
    std::vector<uint8_t> result(hdr.begin(), hdr.end());
    result.insert(result.end(), ann.signature.begin(), ann.signature.end());
    result.resize(hdr.size() + CRYPTO_SIG_MAX_BYTES, 0);
    return result;
}

ResDBIntersectionApp::ArrivalAnnouncement
ResDBIntersectionApp::deserializeArrivalAnnouncement(BFTMessage* msg)
{
    std::vector<uint8_t> payload(msg->getPayloadArraySize());
    for (size_t i = 0; i < payload.size(); i++) payload[i] = msg->getPayload(i);
    std::string s(payload.begin(), payload.end());
    auto parts = splitStr(s, '|');
    ArrivalAnnouncement ann;
    if (parts.size() >= 11) {
        ann.carId              = parts[0];
        ann.laneId             = parts[1];
        ann.lane               = parts[2];
        ann.positionInLane     = std::stoi(parts[3]);
        ann.direction          = strToDir(parts[4]);
        ann.isAmbulance        = (parts[5] == "1");
        ann.claimedArrivalTime = std::stod(parts[6]);
        ann.epoch              = std::stoi(parts[7]);
        ann.ambulanceCertBytes = fromHex(parts[8]);
        ann.ambulanceSigBytes  = fromHex(parts[9]);
        int siglen = std::stoi(parts[10]);
        size_t p = s.find('|');
        for (int k = 1; k < 11 && p != std::string::npos; ++k) p = s.find('|', p + 1);
        size_t offset = (p != std::string::npos) ? p + 1 : s.size();
        if (offset < payload.size() && offset + (size_t)siglen <= payload.size())
            ann.signature.assign(payload.begin() + offset, payload.begin() + offset + siglen);
    }
    return ann;
}

std::vector<uint8_t> ResDBIntersectionApp::serializeArrivalEcho(const ArrivalEcho& echo)
{
    std::vector<uint8_t> pubVec(echo.signerPubKey, echo.signerPubKey + CRYPTO_PUBKEY_BYTES);
    std::vector<uint8_t> sigVec(echo.signature, echo.signature + CRYPTO_SIG_MAX_BYTES);
    std::stringstream ss;
    ss << echo.echoingReplicaId << "|"
       << echo.targetCarId      << "|"
       << echo.lane             << "|"
       << echo.positionInLane   << "|"
       << dirToStr(echo.direction) << "|"
       << static_cast<int>(echo.observedCue) << "|"
       << hashHex(echo.claimHash) << "|"
       << (echo.isAmbulance ? "1" : "0") << "|"
       << echo.epoch            << "|"
       << toHex(pubVec) << "," << static_cast<int>(echo.signatureLen)
       << "," << toHex(sigVec);
    std::string s = ss.str();
    return std::vector<uint8_t>(s.begin(), s.end());
}

ResDBIntersectionApp::ArrivalEcho
ResDBIntersectionApp::deserializeArrivalEcho(BFTMessage* msg)
{
    std::vector<uint8_t> payload(msg->getPayloadArraySize());
    for (size_t i = 0; i < payload.size(); i++) payload[i] = msg->getPayload(i);
    std::string s(payload.begin(), payload.end());
    auto parts = splitStr(s, '|');
    ArrivalEcho echo;
    std::memset(echo.signerPubKey, 0, CRYPTO_PUBKEY_BYTES);
    std::memset(echo.signature, 0, CRYPTO_SIG_MAX_BYTES);
    echo.signatureLen = 0;
    if (parts.size() >= 10) {
        echo.echoingReplicaId = std::stoi(parts[0]);
        echo.targetCarId      = parts[1];
        echo.lane             = parts[2];
        echo.positionInLane   = std::stoi(parts[3]);
        echo.direction        = strToDir(parts[4]);
        echo.observedCue      = ResDBPerception::cueFromCode(static_cast<uint8_t>(std::stoi(parts[5])));
        auto hashBytes        = fromHex(parts[6]);
        if (hashBytes.size() == echo.claimHash.size())
            std::copy(hashBytes.begin(), hashBytes.end(), echo.claimHash.begin());
        echo.isAmbulance      = (parts[7] == "1");
        echo.epoch            = std::stoi(parts[8]);
        const std::string& sf = parts[9];
        size_t firstComma = sf.find(',');
        size_t secondComma = firstComma == std::string::npos
            ? std::string::npos : sf.find(',', firstComma + 1);
        if (firstComma != std::string::npos && secondComma != std::string::npos) {
            auto pubVec = fromHex(sf.substr(0, firstComma));
            const int sigLen = std::stoi(sf.substr(firstComma + 1,
                                                   secondComma - firstComma - 1));
            auto sigVec = fromHex(sf.substr(secondComma + 1));
            if (pubVec.size() == CRYPTO_PUBKEY_BYTES)
                std::memcpy(echo.signerPubKey, pubVec.data(), CRYPTO_PUBKEY_BYTES);
            if (sigLen > 0 && sigLen <= CRYPTO_SIG_MAX_BYTES &&
                    sigVec.size() == CRYPTO_SIG_MAX_BYTES) {
                std::memcpy(echo.signature, sigVec.data(), CRYPTO_SIG_MAX_BYTES);
                echo.signatureLen = static_cast<uint8_t>(sigLen);
            }
        }
    }
    return echo;
}

std::vector<uint8_t> ResDBIntersectionApp::serializeArrivalCert(const ArrivalCert& cert)
{
    std::stringstream ss;
    ss << cert.carId          << "|"
       << cert.lane           << "|"
       << cert.positionInLane << "|"
       << dirToStr(cert.direction) << "|"
       << hashHex(cert.claimHash) << "|"
       << (cert.isAmbulance ? "1" : "0") << "|"
       << cert.epoch;
    for (const auto& echo : cert.echoes) {
        std::vector<uint8_t> pubVec(echo.signerPubKey, echo.signerPubKey + CRYPTO_PUBKEY_BYTES);
        std::vector<uint8_t> sigVec(echo.signature, echo.signature + CRYPTO_SIG_MAX_BYTES);
        ss << "|" << echo.echoingReplicaId << ":"
           << static_cast<int>(echo.observedCue) << ":"
           << toHex(pubVec) << "," << static_cast<int>(echo.signatureLen)
           << "," << toHex(sigVec);
    }
    std::string s = ss.str();
    return std::vector<uint8_t>(s.begin(), s.end());
}

ResDBIntersectionApp::ArrivalCert
ResDBIntersectionApp::deserializeArrivalCert(BFTMessage* msg)
{
    std::vector<uint8_t> payload(msg->getPayloadArraySize());
    for (size_t i = 0; i < payload.size(); i++) payload[i] = msg->getPayload(i);
    std::string s(payload.begin(), payload.end());
    auto parts = splitStr(s, '|');
    ArrivalCert cert;
    if (parts.size() < 7) return cert;
    cert.carId          = parts[0];
    cert.lane           = parts[1];
    cert.positionInLane = std::stoi(parts[2]);
    cert.direction      = strToDir(parts[3]);
    auto claimHashBytes = fromHex(parts[4]);
    if (claimHashBytes.size() != cert.claimHash.size()) return ArrivalCert{};
    std::copy(claimHashBytes.begin(), claimHashBytes.end(), cert.claimHash.begin());
    cert.isAmbulance    = (parts[5] == "1");
    cert.epoch          = std::stoi(parts[6]);
    for (size_t i = 7; i < parts.size(); i++) {
        size_t firstColon = parts[i].find(':');
        size_t secondColon = firstColon == std::string::npos
            ? std::string::npos : parts[i].find(':', firstColon + 1);
        if (firstColon == std::string::npos || secondColon == std::string::npos) continue;
        ArrivalEcho echo;
        std::memset(echo.signerPubKey, 0, CRYPTO_PUBKEY_BYTES);
        std::memset(echo.signature, 0, CRYPTO_SIG_MAX_BYTES);
        echo.signatureLen = 0;
        echo.echoingReplicaId = std::stoi(parts[i].substr(0, firstColon));
        echo.observedCue = ResDBPerception::cueFromCode(static_cast<uint8_t>(
            std::stoi(parts[i].substr(firstColon + 1, secondColon - firstColon - 1))));
        std::string sf = parts[i].substr(secondColon + 1);
        size_t firstComma = sf.find(',');
        size_t secondComma = firstComma == std::string::npos
            ? std::string::npos : sf.find(',', firstComma + 1);
        if (firstComma != std::string::npos && secondComma != std::string::npos) {
            auto pubVec = fromHex(sf.substr(0, firstComma));
            const int sigLen = std::stoi(sf.substr(firstComma + 1,
                                                   secondComma - firstComma - 1));
            auto sigVec = fromHex(sf.substr(secondComma + 1));
            if (pubVec.size() == CRYPTO_PUBKEY_BYTES)
                std::memcpy(echo.signerPubKey, pubVec.data(), CRYPTO_PUBKEY_BYTES);
            if (sigLen > 0 && sigLen <= CRYPTO_SIG_MAX_BYTES &&
                    sigVec.size() == CRYPTO_SIG_MAX_BYTES) {
                std::memcpy(echo.signature, sigVec.data(), CRYPTO_SIG_MAX_BYTES);
                echo.signatureLen = static_cast<uint8_t>(sigLen);
            }
        }
        echo.targetCarId      = cert.carId;
        echo.lane             = cert.lane;
        echo.positionInLane   = cert.positionInLane;
        echo.direction        = cert.direction;
        echo.claimHash        = cert.claimHash;
        echo.isAmbulance      = cert.isAmbulance;
        echo.epoch            = cert.epoch;
        cert.echoes.push_back(echo);
    }
    return cert;
}

std::string ResDBIntersectionApp::canonicalArrivalAnnouncementPayload(
    const ArrivalAnnouncement& ann) const
{
    std::ostringstream ss;
    ss << std::setprecision(17)
       << ann.carId << "|" << ann.epoch << "|" << ann.laneId << "|" << ann.lane << "|"
       << ann.positionInLane << "|" << dirToStr(ann.direction) << "|"
       << (ann.isAmbulance ? "1" : "0") << "|" << ann.claimedArrivalTime << "|"
       << toHex(ann.ambulanceCertBytes) << "|" << toHex(ann.ambulanceSigBytes);
    return ss.str();
}

std::array<uint8_t, 32> ResDBIntersectionApp::arrivalAnnouncementHash(
    const ArrivalAnnouncement& ann) const
{
    std::array<uint8_t, 32> out{};
    const std::vector<uint8_t> bytes = serializeArrivalAnnouncement(ann);
    unsigned int digestLen = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return out;
    const bool ok = EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1 &&
        EVP_DigestUpdate(ctx, bytes.data(), bytes.size()) == 1 &&
        EVP_DigestFinal_ex(ctx, out.data(), &digestLen) == 1 && digestLen == out.size();
    EVP_MD_CTX_free(ctx);
    if (!ok) out.fill(0);
    return out;
}

std::string ResDBIntersectionApp::hashHex(const std::array<uint8_t, 32>& hash)
{
    return toHex(hash.data(), hash.size());
}

bool ResDBIntersectionApp::verifyArrivalAnnouncementOrigin(const ArrivalAnnouncement& ann) const
{
    const int origin = extractReplicaId(ann.carId);
    uint8_t pubKey[CRYPTO_PUBKEY_BYTES] = {};
    if (origin < 0 || ann.signature.empty() ||
            !WitnessKeyRegistry::instance().copyKey(origin, pubKey)) return false;
    const std::string payload = canonicalArrivalAnnouncementPayload(ann);
    return CryptoAuth::instance().verifyBytes(
        pubKey, reinterpret_cast<const uint8_t*>(payload.data()), payload.size(),
        ann.signature.data(), static_cast<uint8_t>(ann.signature.size()));
}

std::string ResDBIntersectionApp::arrivalEchoSigningPayload(const ArrivalEcho& echo) const
{
    return echo.targetCarId + ":" + echo.lane + ":" +
        std::to_string(echo.positionInLane) + ":" + dirToStr(echo.direction) + ":" +
        std::to_string(static_cast<int>(echo.observedCue)) + ":" + hashHex(echo.claimHash) + ":" +
        (echo.isAmbulance ? "1" : "0") + ":" + std::to_string(echo.epoch) + ":" +
        std::to_string(echo.echoingReplicaId);
}

// ── Discovery-round state ─────────────────────────────────────────────────────

int ResDBIntersectionApp::countStaticCollectedCerts() const
{
    int count = 0;
    for (const auto& kv : collected_certs_) {
        const int rid = extractReplicaId(kv.first);
        if (cancel_pending_) {
            if (shouldIncludeInRollbackMembership(rid))
                ++count;
        } else if (rid >= 0 && rid < total_vehicles_) {
            ++count;
        }
    }
    return count;
}

int ResDBIntersectionApp::CertPrimary() const
{
    int primary = -1;
    for (const auto& kv : collected_certs_) {
        const int rid = extractReplicaId(kv.first);
        const bool eligible = cancel_pending_
            ? shouldIncludeInRollbackMembership(rid)
            : (rid >= 0 && rid < total_vehicles_);
        if (eligible && (primary < 0 || rid < primary)) {
            primary = rid;
        }
    }
    return primary;
}

const char* ResDBIntersectionApp::discoveryStateName() const
{
    switch (discovery_.state) {
    case DiscoveryState::INACTIVE: return "INACTIVE";
    case DiscoveryState::COLLECTING: return "COLLECTING";
    case DiscoveryState::DRAINING_CERTS: return "DRAINING_CERTS";
    case DiscoveryState::COMPLETE: return "COMPLETE";
    }
    return "UNKNOWN";
}

void ResDBIntersectionApp::startDiscoveryRound(const char* reason)
{
    if (current_phase_ == ConsensusPhase::DEPARTED || is_departed_) return;
    cancelArrivalCertFinalizeTimer();
    if (discovery_deadline_msg_->isScheduled()) cancelEvent(discovery_deadline_msg_);
    if (discovery_settle_msg_->isScheduled()) cancelEvent(discovery_settle_msg_);
    discovery_.reset(current_epoch_, simTime());
    resetOrderCandidate(reason ? reason : "discovery-start");
    std::cout << "[DISCOVERY-BEGIN] r" << replicaId_
              << " epoch=" << discovery_.epoch
              << " reason=" << (reason ? reason : "start")
              << " stop_zone=" << (entered_stop_zone_ ? 1 : 0)
              << " t=" << simTime() << "\n";
    if (entered_stop_zone_) armDiscoveryTimers(reason);
}

void ResDBIntersectionApp::armDiscoveryTimers(const char* reason)
{
    if (!entered_stop_zone_ || discovery_.state != DiscoveryState::COLLECTING ||
            propose_submitted_ || order_applied_) return;
    if (!discovery_deadline_msg_->isScheduled()) {
        scheduleAt(simTime() + cert_collection_timeout_, discovery_deadline_msg_);
        discovery_.collectionStartedAt = simTime();
        std::cout << "[DISCOVERY-DEADLINE] r" << replicaId_
                  << " epoch=" << discovery_.epoch
                  << " armed_for=" << simTime() + cert_collection_timeout_
                  << " reason=" << (reason ? reason : "arm") << "\n";
    }
    simtime_t settleAt = discovery_.lastNewIntentAt + discovery_intent_settle_;
    if (settleAt < simTime()) settleAt = simTime();
    if (discovery_settle_msg_->isScheduled()) cancelEvent(discovery_settle_msg_);
    scheduleAt(settleAt, discovery_settle_msg_);
}

void ResDBIntersectionApp::noteDiscoveryIntent(const std::string& carId, const char* source)
{
    if (carId.empty() || propose_submitted_ || order_applied_ ||
            current_phase_ == ConsensusPhase::DEPARTED) return;
    if (discovery_.state == DiscoveryState::DRAINING_CERTS ||
            discovery_.state == DiscoveryState::COMPLETE) {
        resetOrderCandidate("discovery-reopened");
        discovery_.state = DiscoveryState::COLLECTING;
        discovery_.closeReason = DiscoveryCloseReason::NONE;
        if (vc_trigger_msg_) {
            if (vc_trigger_msg_->isScheduled()) cancelEvent(vc_trigger_msg_);
            delete vc_trigger_msg_;
            vc_trigger_msg_ = nullptr;
        }
        if (!broadcastArrivalAnnouncement_timer_) {
            broadcastArrivalAnnouncement_timer_ =
                new cMessage("resdbBroadcastArrivalAnnouncement");
            scheduleAt(simTime() + broadcast_arrival_announcement_interval_,
                       broadcastArrivalAnnouncement_timer_);
        }
    }
    discovery_.lastNewIntentAt = simTime();
    std::cout << "[DISCOVERY-VIEW] r" << replicaId_
              << " epoch=" << discovery_.epoch
              << " new_intent=" << carId
              << " source=" << (source ? source : "announce")
              << " intents=" << observed_intent_cars_.size()
              << " certs=" << collected_certs_.size()
              << " t=" << simTime() << "\n";
    armDiscoveryTimers("new-intent");
}

bool ResDBIntersectionApp::discoveryViewCertified(std::vector<int>* missing) const
{
    bool hasEligibleIntent = false;
    bool hasLocalIntent = !cancel_pending_ || !rollback_local_recallable_;
    const std::string localCarId = "veh" + std::to_string(replicaId_);
    std::lock_guard<std::mutex> lk(certs_mutex_);
    for (const auto& carId : observed_intent_cars_) {
        const int rid = extractReplicaId(carId);
        if (cancel_pending_ && !shouldIncludeInRollbackMembership(rid)) continue;
        hasEligibleIntent = true;
        if (carId == localCarId) hasLocalIntent = true;
        if (!collected_certs_.count(carId)) {
            if (missing) missing->push_back(rid);
            else return false;
        }
    }
    return hasEligibleIntent && hasLocalIntent && (!missing || missing->empty());
}

void ResDBIntersectionApp::maybeAdvanceDiscovery(const char* reason, bool deadline)
{
    if (discovery_.state != DiscoveryState::COLLECTING || !entered_stop_zone_ ||
            propose_submitted_ || order_applied_) return;
    if (deadline) {
        beginDiscoveryDrain(reason, true);
        return;
    }
    if (simTime() < discovery_.lastNewIntentAt + discovery_intent_settle_) {
        armDiscoveryTimers("view-not-stable");
        return;
    }
    // Cold-start round: the ad hoc gossip topology has not converged yet
    // (no vehicle has had time to enter radio range of others or for relay
    // trees to form), so a quiet intent-set does not imply a complete view.
    // Only the hard per-round deadline may close epoch 0 early. Later
    // epochs (post-CANCEL reconvergence) keep the eager path below, since
    // by then the topology is already established.
    if (discovery_.epoch == 0) return;
    if (!discoveryViewCertified()) return;
    beginDiscoveryDrain(reason, false);
}

void ResDBIntersectionApp::beginDiscoveryDrain(const char* reason, bool deadline)
{
    if (discovery_.state != DiscoveryState::COLLECTING) return;
    // A local claimant that already reached f+1 must not lose its certificate
    // merely because the view closes before the post-threshold window expires.
    finalizeLocalArrivalCert(reason ? reason : "discovery-drain");
    discovery_.state = DiscoveryState::DRAINING_CERTS;
    discovery_.closeReason = deadline
        ? DiscoveryCloseReason::DEADLINE
        : DiscoveryCloseReason::STABILIZED;
    if (discovery_deadline_msg_ && discovery_deadline_msg_->isScheduled())
        cancelEvent(discovery_deadline_msg_);
    if (discovery_settle_msg_ && discovery_settle_msg_->isScheduled())
        cancelEvent(discovery_settle_msg_);
    if (broadcastArrivalAnnouncement_timer_) {
        if (broadcastArrivalAnnouncement_timer_->isScheduled())
            cancelEvent(broadcastArrivalAnnouncement_timer_);
        delete broadcastArrivalAnnouncement_timer_;
        broadcastArrivalAnnouncement_timer_ = nullptr;
    }
    stopStopZoneCertGossip();
    discardPendingDiscoveryNonCerts(reason ? reason : "view-closed");
    logDiscoveryDiagnostics(reason ? reason : "view-closed");
    std::cout << "[DISCOVERY-DRAIN] r" << replicaId_
              << " epoch=" << discovery_.epoch
              << " reason=" << (reason ? reason : "view-closed")
              << " deadline=" << (deadline ? 1 : 0)
              << " local_cert_assembled=" << (discovery_.localCertAssembled() ? 1 : 0)
              << " local_cert_aired=" << (discovery_.localCertAired() ? 1 : 0)
              << " t=" << simTime() << "\n";
    maybeCompleteDiscoveryDrain(reason);
}

void ResDBIntersectionApp::maybeCompleteDiscoveryDrain(const char* reason)
{
    if (discovery_.state != DiscoveryState::DRAINING_CERTS) return;
    if (discovery_.localCertAssembled() && !discovery_.localCertAired()) return;
    if (hasPendingDiscoveryCerts(discovery_.epoch)) return;
    finishDiscoveryRound(reason);
}

void ResDBIntersectionApp::finishDiscoveryRound(const char* reason)
{
    if (discovery_.state != DiscoveryState::DRAINING_CERTS) return;
    discovery_.state = DiscoveryState::COMPLETE;
    stopCertBroadcastRetries();
    stopStopZoneCertGossip();
    std::vector<int> missing;
    discoveryViewCertified(&missing);
    std::cout << "[DISCOVERY-COMPLETE] r" << replicaId_
              << " epoch=" << discovery_.epoch
              << " reason=" << (reason ? reason : "drained")
              << " intents=" << observed_intent_cars_.size()
              << " certs=" << collected_certs_.size()
              << " missing=";
    for (size_t i = 0; i < missing.size(); ++i) {
        if (i) std::cout << ",";
        std::cout << "r" << missing[i];
    }
    std::cout << " local_cert_aired=" << (discovery_.localCertAired() ? 1 : 0)
              << " t=" << simTime() << "\n";

    evaluateOrderReadiness("discovery-complete");
}

void ResDBIntersectionApp::deactivateDiscovery(const char* reason)
{
    cancelArrivalCertFinalizeTimer();
    if (discovery_deadline_msg_ && discovery_deadline_msg_->isScheduled())
        cancelEvent(discovery_deadline_msg_);
    if (discovery_settle_msg_ && discovery_settle_msg_->isScheduled())
        cancelEvent(discovery_settle_msg_);
    stopCertBroadcastRetries();
    stopStopZoneCertGossip();
    cancelPendingDiscoveryTxs(reason ? reason : "inactive");
    discovery_.state = DiscoveryState::INACTIVE;
}

// ── attachAmbulanceCryptoToAnnouncement (ported from legacy arrival path) ────

void ResDBIntersectionApp::attachAmbulanceCryptoToAnnouncement(ArrivalAnnouncement& ann)
{
    ann.isAmbulance = false;
    ann.ambulanceCertBytes.clear();
    ann.ambulanceSigBytes.clear();
    if (!is_ambulance_)
        return;

    ann.isAmbulance = true;
    ann.ambulanceCertBytes = my_ambulance_cert_bytes_;
    if (!ambulance_private_key_ ||
            my_ambulance_cert_bytes_.size() != sizeof(VehicleCert)) {
        std::cerr << "[AMBULANCE] r" << replicaId_
                  << " attach failed: key="
                  << (ambulance_private_key_ ? "ok" : "null")
                  << " certBytes=" << my_ambulance_cert_bytes_.size()
                  << " expected=" << sizeof(VehicleCert) << "\n";
        return;
    }

    const std::string ambPayload = ann.carId + ":" + ann.lane + ":"
        + std::to_string(ann.positionInLane) + ":"
        + dirToStr(ann.direction) + ":AMBULANCE";
    uint8_t sigOut[CRYPTO_SIG_MAX_BYTES];
    uint8_t sigLen = 0;
    if (!CryptoAuth::instance().signBytes(
            ambulance_private_key_,
            reinterpret_cast<const uint8_t*>(ambPayload.c_str()), ambPayload.size(),
            sigOut, sigLen)) {
        std::cerr << "[AMBULANCE] r" << replicaId_
                  << " signBytes failed for payload=" << ambPayload << "\n";
        return;
    }
    ann.ambulanceSigBytes.assign(sigOut, sigOut + sigLen);
}

// ── broadcastArrivalAnnouncement (port of V2VArrivalProtocol::broadcastArrivalAnnouncement) ──

void ResDBIntersectionApp::broadcastArrivalAnnouncement()
{
    if (discovery_.state != DiscoveryState::COLLECTING || propose_submitted_ ||
            current_phase_ == ConsensusPhase::DEPARTED || order_applied_ ||
            crashCommsDisabled_) return;
    if (!mobility) return;

    const std::string myCarId = "veh" + std::to_string(replicaId_);
    const std::string baseCacheKey = std::to_string(current_epoch_) + ":broadcast";
    auto cachedBase = cached_local_announcements_.find(baseCacheKey);
    if (cachedBase != cached_local_announcements_.end()) {
        if (is_byzantine_ && byzantine_type_ == BYZANTINE_EQUIVOCATOR) {
            for (int peerId = 0; peerId < total_vehicles_; ++peerId) {
                if (peerId == replicaId_) continue;
                const std::string peerKey = std::to_string(current_epoch_) +
                                            ":peer:" + std::to_string(peerId);
                auto cachedPeer = cached_local_announcements_.find(peerKey);
                if (cachedPeer != cached_local_announcements_.end())
                    sendBFTMessage(peerId,
                                   serializeArrivalAnnouncement(cachedPeer->second),
                                   kArrivalAnnounceType);
            }
        } else {
            sendBFTMessage(-1, serializeArrivalAnnouncement(cachedBase->second),
                           kArrivalAnnounceType);
            std::cout << "[ANN-BROADCAST] Replica " << replicaId_
                      << " rebroadcast cached ARRIVAL_ANNOUNCE at t=" << simTime()
                      << " lane=" << cachedBase->second.lane << "\n";
        }
        return;
    }

    TraCICommandInterface* traci = mobility->getCommandInterface();
    if (!traci) return;

    ArrivalAnnouncement ann;
    ann.carId = myCarId;

    try {
        ann.laneId = traci->vehicle(myCarId).getLaneId();
    } catch (...) { ann.laneId = ""; }

    // intendedLane NED param takes precedence when explicitly set (N/S/E/W).
    // This bypasses SUMO edge-naming ambiguity (reversed edges start with '-',
    // internal junction edges start with ':', etc.).
    // Fall back to TraCI lane-ID parsing only when intendedLane is empty.
    const bool lane_override = (intended_lane_ == "N" || intended_lane_ == "S" ||
                                intended_lane_ == "E" || intended_lane_ == "W");
    if (lane_override) {
        ann.lane = intended_lane_;
    } else if (!ann.laneId.empty()) {
        char c = std::toupper(ann.laneId[0]);
        ann.lane = (c=='N'||c=='S'||c=='E'||c=='W') ? std::string(1,c) : "N";
    } else {
        ann.lane = "N";
    }

    if (is_byzantine_ && byzantine_type_ == BYZANTINE_FALSE_LANE) {
        ann.laneId = "BYZANTINE_FAKE_LANE";
        ann.lane   = "X";
        std::cout << "[BYZANTINE] r" << replicaId_ << " FALSE_LANE: laneId=BYZANTINE_FAKE_LANE\n";
    }
    if (phase2_attack_kind_ == Phase2AttackKind::WRONG_APPROACH &&
            replicaId_ == phase2_attack_target_replica_id_) {
        ann.laneId = "PHASE2_CLAIM_E";
        ann.lane = "E";
    }

    // {
    //     // Use TraCI lane position: higher lanePos = closer to intersection.
    //     // Invert so positionInLane 1 = front (closest), larger = further back.
    //     double lp = 0.0;
    //     try { lp = traci->vehicle(myCarId).getLanePosition(); } catch (...) {}
    //     int inv = 255 - std::min(static_cast<int>(lp), 254);
    //     ann.positionInLane = std::max(1, inv);
    // }
    int rank = 1;
    auto it = std::find(lane_queue_.begin(), lane_queue_.end(), myCarId);
    if (it != lane_queue_.end()) {
        rank = std::distance(lane_queue_.begin(), it) + 1;
    }
    ann.positionInLane = rank;
    std::cout << "[ANN-BROADCAST] Replica " << replicaId_ << " positionInLane: " << rank << "\n";

    ann.direction          = strToDir(intended_direction_);
    if (phase2_attack_kind_ == Phase2AttackKind::FALSE_DIRECTION &&
            replicaId_ == phase2_attack_target_replica_id_) {
        ann.direction = DIR_RIGHT;
    }
    if (phase2_attack_kind_ != Phase2AttackKind::NONE &&
            replicaId_ == phase2_attack_target_replica_id_) {
        std::cout << "[PHASE2-ATTACK-DECLARE] target=" << myCarId
                  << " epoch=" << current_epoch_
                  << " kind=" << phase2AttackKindName()
                  << " actualLane=" << intended_lane_
                  << " claimedLane=" << ann.lane
                  << " actualDirection=" << intended_direction_
                  << " claimedDirection=" << dirToStr(ann.direction) << "\n";
    }
    ann.isAmbulance        = is_ambulance_;
    if (is_byzantine_ && byzantine_type_ == BYZANTINE_FAKE_AMBULANCE_FOLLOWER) {
        ann.isAmbulance = true;  // lie: claim ambulance without valid cert
        // ambulanceCertBytes intentionally left empty — cert gate catches this
        std::cout << "[BYZANTINE] r" << replicaId_
                  << " FAKE_AMBULANCE_FOLLOWER: claiming ambulance without cert\n";
    } else if (is_ambulance_) {
        attachAmbulanceCryptoToAnnouncement(ann);
    }
    ann.claimedArrivalTime = simTime().dbl();
    ann.epoch              = (int)current_epoch_;

    auto signAnnouncement = [this](ArrivalAnnouncement& claim) {
        claim.signature.clear();
        if (ec_private_key_) {
            const std::string toSign = canonicalArrivalAnnouncementPayload(claim);
        uint8_t sigOut[CRYPTO_SIG_MAX_BYTES]; uint8_t sigLen = 0;
        if (CryptoAuth::instance().signBytes(ec_private_key_,
                reinterpret_cast<const uint8_t*>(toSign.c_str()), toSign.size(),
                sigOut, sigLen))
                claim.signature.assign(sigOut, sigOut + sigLen);
        }
    };
    signAnnouncement(ann);
    cached_local_announcements_[baseCacheKey] = ann;

    // Self-store.
    {
        VehicleState selfVS;
        selfVS.vehicleId       = myCarId;
        selfVS.lane            = ann.lane;
        selfVS.positionInLane  = ann.positionInLane;
        selfVS.direction       = ann.direction;
        selfVS.isAmbulance     = ann.isAmbulance;
        selfVS.arrival_time_us = (uint64_t)(ann.claimedArrivalTime * 1e6);
        local_vehicle_states_[myCarId]             = selfVS;
        local_claim_hashes_[myCarId]               = arrivalAnnouncementHash(ann);
        arrival_announcements_received_.insert(myCarId);
        const bool newIntent = observed_intent_cars_.insert(myCarId).second;
        if (newIntent) noteDiscoveryIntent(myCarId, "self-announce");
    }

    // One declaration-bound claimant signature is inserted locally. It is not
    // a perception observation and never enters the Type-4 radio path.
    // Re-announcement firings use cached_local_announcements_, so this creation
    // path executes only once for a claim.
    addLocalSelfAttestation(ann);

    if (is_byzantine_ && byzantine_type_ == BYZANTINE_EQUIVOCATOR) {
        int n = total_vehicles_;
        for (int peerId = 0; peerId < n; ++peerId) {
            if (peerId == replicaId_) continue;
            ArrivalAnnouncement annByz = ann;
            annByz.direction = (peerId < n / 2) ? DIR_LEFT : DIR_RIGHT;
            signAnnouncement(annByz);
            cached_local_announcements_[std::to_string(current_epoch_) +
                                        ":peer:" + std::to_string(peerId)] = annByz;
            std::cout << "[BYZANTINE] r" << replicaId_ << " EQUIVOCATOR: peer "
                      << peerId << " dir=" << (peerId < n/2 ? "L" : "R") << "\n";
            sendBFTMessage(peerId, serializeArrivalAnnouncement(annByz), kArrivalAnnounceType);
        }
    } else {
        std::vector<uint8_t> payload = serializeArrivalAnnouncement(ann);
        sendBFTMessage(-1, payload, kArrivalAnnounceType);
        std::cout << "[ANN-BROADCAST] Replica " << replicaId_ << " broadcast ARRIVAL_ANNOUNCE at t="
                  << simTime() << " lane=" << ann.lane << "\n";
    }

}

// ── handleArrivalAnnouncement ─────────────────────────────────────────────────

void ResDBIntersectionApp::handleArrivalAnnouncement(BFTMessage* msg)
{
    handleArrivalAnnouncement(msg, false, -1);
}

void ResDBIntersectionApp::gossipArrivalAnnouncement(const ArrivalAnnouncement& ann,
                                                     const std::vector<uint8_t>& announceBytes)
{
    if (!gossip_enabled_ || announceBytes.empty() || ann.carId.empty()) return;
    if (discovery_.state != DiscoveryState::COLLECTING ||
            propose_submitted_ || order_applied_ || crashCommsDisabled_) return;
    if (!announcement_relay_tracker_.tryRelay((uint32_t)ann.epoch, ann.carId)) return;

    sendArrivalAnnouncementGossipPayload(
        ann.carId, (uint32_t)ann.epoch, announceBytes, "verified");
}

void ResDBIntersectionApp::sendArrivalAnnouncementGossipPayload(
    const std::string& carId,
    uint32_t epoch,
    const std::vector<uint8_t>& announceBytes,
    const char* reason)
{
    if (!gossip_enabled_ || announceBytes.empty() || carId.empty()) return;
    if (discovery_.state != DiscoveryState::COLLECTING ||
            propose_submitted_ || order_applied_ || crashCommsDisabled_) return;

    auto inner = resdb_gossip::serializeAnnouncement(epoch, announceBytes);
    auto signedPayload = resdbwire::packSignedPacket(
        ec_private_key_, ec_pub_key_, inner.data(), (uint32_t)inner.size());
    if (signedPayload.empty()) return;

    sendBFTMessage(-1, signedPayload, kArrivalAnnounceGossipType);
    std::cout << "[ANN-GOSSIP-SEND] r" << replicaId_
              << " car=" << carId
              << " epoch=" << epoch
              << " bytes=" << announceBytes.size()
              << " reason=" << (reason ? reason : "relay")
              << " t=" << simTime() << "\n";
}

void ResDBIntersectionApp::handleArrivalAnnouncementGossip(BFTMessage* msg)
{
    int plen = msg->getPayloadArraySize();
    if (plen <= 0) return;
    std::vector<uint8_t> buf((size_t)plen);
    for (int i = 0; i < plen; ++i) buf[i] = msg->getPayload(i);

    resdbwire::SignedPacketView view;
    if (!resdbwire::unpackSignedPacket(buf.data(), (uint32_t)buf.size(), &view)) return;
    if (view.resdbLen == 0) return;
    if (!CryptoAuth::instance().verifyBytes(view.pubKey, view.resdbBytes, view.resdbLen,
                                            view.sig, view.sigLen)) {
        std::cout << "[ANN-GOSSIP-RECV] r" << replicaId_
                  << " dropped forged announce gossip from r"
                  << msg->getFromReplicaId() << "\n";
        return;
    }

    uint32_t epoch = 0;
    std::vector<uint8_t> announceBytes;
    if (!resdb_gossip::parseAnnouncement(view.resdbBytes, view.resdbLen,
                                         epoch, announceBytes)) return;
    if (has_committed_order_ && epoch <= last_committed_epoch_) return;
    if ((int)epoch < (int)current_epoch_) return;

    BFTMessage* announceMsg = makeArrivalAnnouncementMessage(
        announceBytes, msg->getFromReplicaId(), replicaId_);
    handleArrivalAnnouncement(announceMsg, true, msg->getFromReplicaId());
    delete announceMsg;
}

void ResDBIntersectionApp::handleArrivalAnnouncement(BFTMessage* msg,
                                                     bool viaGossip,
                                                     int carrierReplicaId)
{
    if (discovery_.state != DiscoveryState::COLLECTING || propose_submitted_ ||
            order_applied_ || crashCommsDisabled_ ||
            current_phase_ == ConsensusPhase::DEPARTED) return;
    std::vector<uint8_t> announceBytes = payloadBytes(msg);
    ArrivalAnnouncement ann = deserializeArrivalAnnouncement(msg);
    if (ann.carId.empty()) return;
    if (ann.direction == DIR_UNKNOWN) return;
    if (!verifyArrivalAnnouncementOrigin(ann)) {
        std::cout << "[ANN-ORIGIN-INVALID] r" << replicaId_
                  << " target=" << ann.carId << " epoch=" << ann.epoch << "\n";
        return;
    }
    // Announcement gossip can carry our declaration back through a peer. Its
    // single self-attestation was inserted locally at claim creation; replaying
    // it through the witness path would invoke perception and offer a second
    // signing opportunity.
    if (extractReplicaId(ann.carId) == replicaId_) return;
    const auto claimHash = arrivalAnnouncementHash(ann);
    const std::string claimHashHex = hashHex(claimHash);
    const std::string cacheKey = std::to_string(ann.epoch) + ":" + ann.carId + ":" +
        claimHashHex;
    std::cout << "[ANN-RECV] Replica " << replicaId_ << " received ARRIVAL_ANNOUNCE from "
              << ann.carId << " frameFrom=" << msg->getFromReplicaId()
              << " via=" << (viaGossip ? "gossip" : "direct");
    if (viaGossip) std::cout << " carrier=" << carrierReplicaId;
    std::cout << " at t=" << simTime() << "\n";

    // Exactly one perception verdict per authenticated claim variant. Replays
    // of an accepted claim retransmit the byte-identical cached echo; replays
    // of a rejected claim retain that rejection. A different claimHash is a
    // different declaration and reaches the evaluation path below.
    if (cached_arrival_echoes_.count(cacheKey)) {
        if (!collected_certs_.count(ann.carId) &&
                !propose_submitted_ && !order_applied_) {
            pending_relays_[std::make_pair((uint32_t)ann.epoch, ann.carId)] = {
                ann.carId, (uint32_t)ann.epoch, announceBytes, simTime().dbl(), 0};
            sendArrivalEcho(ann);
        }
        return;
    }
    if (cached_arrival_rejections_.count(cacheKey)) return;

    const bool colludingFalseLane = shouldColludeOnFalseLane(ann);
    const bool phase2Collusion = shouldPhase2Collude(ann);
    VerificationResult result;
    const std::string sampleKey = cacheKey;
    ArrivalPerceptionSample sample;
    if (colludingFalseLane ||
            (phase2Collusion && phase2_attack_kind_ == Phase2AttackKind::WRONG_APPROACH)) {
        result = {true,
                  phase2Collusion ? "PHASE2_COLLUSION" : "BYZANTINE_COLLUSION",
                  ann.laneId,
                  static_cast<double>(ann.positionInLane)};
        sample.detected = true;
        sample.trueApproach = ann.lane.empty() ? '?' : ann.lane.front();
        sample.observedApproach = sample.trueApproach;
        sample.observedCue = static_cast<ObservedCue>(ann.direction);
        sample.trueCue = sample.observedCue;
        sample.knownCueSamples = 1;
        sample.observedAt = simTime();
        arrival_perception_samples_[sampleKey] = sample;
    } else {
        auto sampleIt = arrival_perception_samples_.find(sampleKey);
        if (sampleIt == arrival_perception_samples_.end()) {
            sample = perception_ ? perception_->observeArrival(ann.carId, simTime())
                                 : ArrivalPerceptionSample{};
            arrival_perception_samples_[sampleKey] = sample;
        } else {
            sample = sampleIt->second;
        }
        if (phase2Collusion &&
                phase2_attack_kind_ == Phase2AttackKind::FALSE_DIRECTION) {
            sample.observedCue = ObservedCue::RIGHT;
            sample.knownCueSamples = 1;
            arrival_perception_samples_[sampleKey] = sample;
        }
        const bool laneMatch = sample.detected && ann.lane.size() == 1 &&
            sample.observedApproach == ann.lane.front();
        result = {laneMatch,
                  !sample.detected ? "NO_PERCEPTION" : laneMatch ? "OK" : "WRONG_APPROACH",
                  sample.detected ? std::string(1, sample.observedApproach) : std::string(),
                  static_cast<double>(ann.positionInLane)};
        std::cout << "[PERC-EVAL] witness=" << replicaId_
                  << " target=" << ann.carId
                  << " epoch=" << ann.epoch
                  << " claimHash=" << claimHashHex
                  << " laneVerdict=" << (laneMatch ? "ACCEPT" : "REJECT")
                  << " trueLane=" << sample.trueApproach
                  << " claimedLane=" << ann.lane
                  << " observedLane=" << sample.observedApproach
                  << " observedCue=" << ResDBPerception::cueName(sample.observedCue)
                  << " knownCueSamples=" << sample.knownCueSamples << "\n";
    }

    if (!result.isValid) {
        cached_arrival_rejections_.insert(cacheKey);
        if (result.actualLaneId.empty()) return;  // not in simulation at all
        // Physically present but wrong lane — record without echoing.
        VehicleState vs;
        vs.vehicleId      = ann.carId;
        char c = result.actualLaneId.empty() ? 'N' : std::toupper(result.actualLaneId[0]);
        vs.lane           = (c=='N'||c=='S'||c=='E'||c=='W') ? std::string(1,c) : "N";
        vs.positionInLane = ann.positionInLane;
        vs.direction      = ann.direction;
        vs.isAmbulance    = false;  // don't trust ambulance claim from lane-liar
        vs.arrival_time_us = (uint64_t)(ann.claimedArrivalTime * 1e6);
        local_vehicle_states_[ann.carId] = vs;
        local_claim_hashes_[ann.carId] = claimHash;
        arrival_announcements_received_.insert(ann.carId);
        const bool newIntent = observed_intent_cars_.insert(ann.carId).second;
        if (newIntent) noteDiscoveryIntent(ann.carId, "invalid-lane-announce");
        std::cout << "[ANN-RECV] Replica " << replicaId_ << " FALSE_LANE from "
                  << ann.carId
                  << " via=" << (viaGossip ? "gossip" : "direct")
                  << " — no echo\n";
        return;
    }

    // Ambulance certificate verification (Emergency_CA VehicleCert + payload sig).
    bool effectiveIsAmbulance = ann.isAmbulance;
    if (ann.isAmbulance && ann.ambulanceCertBytes.size() == sizeof(VehicleCert)) {
        VehicleCert cert;
        std::memcpy(&cert, ann.ambulanceCertBytes.data(), sizeof(VehicleCert));
        std::string role = CryptoAuth::instance().verifyCert(cert);
        if (role != "ambulance") {
            std::cerr << "[ANN-RECV] r" << replicaId_
                      << " DOWNGRADE: cert role='" << role << "' for " << ann.carId << "\n";
            effectiveIsAmbulance = false;
        } else if (!ann.ambulanceSigBytes.empty() &&
                   ann.ambulanceSigBytes.size() <= CRYPTO_SIG_MAX_BYTES) {
            std::string payload = ann.carId + ":" + ann.lane + ":"
                + std::to_string(ann.positionInLane) + ":"
                + dirToStr(ann.direction) + ":AMBULANCE";
            if (!CryptoAuth::instance().verifyBytes(
                    cert.publicKey,
                    reinterpret_cast<const uint8_t*>(payload.c_str()), payload.size(),
                    ann.ambulanceSigBytes.data(),
                    static_cast<uint8_t>(ann.ambulanceSigBytes.size()))) {
                std::cerr << "[ANN-RECV] r" << replicaId_
                          << " DOWNGRADE: ambulance sig invalid for " << ann.carId << "\n";
                effectiveIsAmbulance = false;
            }
        } else {
            effectiveIsAmbulance = false;
        }
    } else if (ann.isAmbulance && !ann.ambulanceCertBytes.empty()) {
        std::cerr << "[ANN-RECV] r" << replicaId_
                  << " DOWNGRADE: ambulance cert wrong size ("
                  << ann.ambulanceCertBytes.size() << " vs " << sizeof(VehicleCert)
                  << ") for " << ann.carId << "\n";
        effectiveIsAmbulance = false;
    }
    // Cert gate: when enabled, reject uncertified or cryptographically invalid claims.
    const bool claimedAmbulance = ann.isAmbulance;
    ann.isAmbulance = effectiveIsAmbulance;
    if (claimedAmbulance && !effectiveIsAmbulance) {
        uncertified_ambulance_claimers_.insert(ann.carId);
    }
    if (enableAmbulanceCertGate_ && claimedAmbulance && !effectiveIsAmbulance) {
        cert_gate_rejected_ambulance_claimers_.insert(ann.carId);
        std::cout << "[CERT-GATE] r" << replicaId_
                  << " rejected uncertified ambulance claim from " << ann.carId << "\n";
        ann.isAmbulance = false;
    }

    // Build and store VehicleState.
    VehicleState vs;
    vs.vehicleId       = ann.carId;
    vs.lane            = ann.lane;
    vs.positionInLane  = ann.positionInLane;
    vs.direction       = ann.direction;
    vs.isAmbulance     = ann.isAmbulance;
    vs.arrival_time_us = (uint64_t)(ann.claimedArrivalTime * 1e6);
    local_vehicle_states_[ann.carId] = vs;
    local_claim_hashes_[ann.carId] = claimHash;
    arrival_announcements_received_.insert(ann.carId);
    const bool newIntent = observed_intent_cars_.insert(ann.carId).second;
    echoed_cars_.insert(ann.carId);  // record that we actually echoed this car
    pending_relays_[std::make_pair((uint32_t)ann.epoch, ann.carId)] = {
        ann.carId, (uint32_t)ann.epoch, announceBytes, simTime().dbl(), 0};

    maybeTriggerEmergencyRollbackFromAnnouncement(ann);
    sendArrivalEcho(ann);
    if (phase2Collusion) {
        std::cout << "[PHASE2-COLLUSION-ECHO] target=" << ann.carId
                  << " epoch=" << ann.epoch
                  << " signer=" << replicaId_
                  << " kind=" << phase2AttackKindName()
                  << " claimedLane=" << ann.lane
                  << " cue=" << ResDBPerception::cueName(
                         arrival_perception_samples_[sampleKey].observedCue)
                  << "\n";
    }
    gossipArrivalAnnouncement(ann, announceBytes);

    std::cout << "[ANN-RECV] Replica " << replicaId_ << " stored VehicleState for "
              << ann.carId << " via=" << (viaGossip ? "gossip" : "direct")
              << " (" << observed_intent_cars_.size()
              << "/" << total_vehicles_ << " observed)\n";
    if (newIntent) noteDiscoveryIntent(ann.carId, viaGossip ? "announce-gossip" : "announce-direct");
    if (discovery_.state != DiscoveryState::INACTIVE)
        current_phase_ = ConsensusPhase::COLLECTING_CERTS;
}

// ── sendArrivalEcho ───────────────────────────────────────────────────────────

void ResDBIntersectionApp::addLocalSelfAttestation(const ArrivalAnnouncement& ann)
{
    if (!ec_private_key_ || ann.carId != "veh" + std::to_string(replicaId_) ||
            discovery_.state != DiscoveryState::COLLECTING || propose_submitted_ ||
            order_applied_ || crashCommsDisabled_ ||
            current_phase_ == ConsensusPhase::DEPARTED) return;

    ArrivalEcho echo;
    echo.echoingReplicaId = replicaId_;
    echo.targetCarId      = ann.carId;
    echo.lane             = ann.lane;
    echo.positionInLane   = ann.positionInLane;
    echo.direction        = ann.direction;
    echo.observedCue      = static_cast<ObservedCue>(ann.direction);
    echo.claimHash        = arrivalAnnouncementHash(ann);
    echo.isAmbulance      = ann.isAmbulance;
    echo.epoch            = ann.epoch;
    std::memcpy(echo.signerPubKey, ec_pub_key_, CRYPTO_PUBKEY_BYTES);
    std::memset(echo.signature, 0, CRYPTO_SIG_MAX_BYTES);
    echo.signatureLen = 0;

    const std::string toSign = arrivalEchoSigningPayload(echo);
    if (!CryptoAuth::instance().signBytes(ec_private_key_,
            reinterpret_cast<const uint8_t*>(toSign.data()), toSign.size(),
            echo.signature, echo.signatureLen)) {
        std::cerr << "[SELF-ATTEST] target=" << ann.carId
                  << " epoch=" << ann.epoch << " signer=" << replicaId_
                  << " status=SIGN_FAILED\n";
        return;
    }

    std::cout << "[SELF-ATTEST] target=" << ann.carId
              << " epoch=" << ann.epoch
              << " signer=" << replicaId_
              << " claimHash=" << hashHex(echo.claimHash)
              << " observedCue=" << ResDBPerception::cueName(echo.observedCue)
              << " status=VALID\n";
    collectArrivalEcho(echo, "self-attest");
}

void ResDBIntersectionApp::sendArrivalEcho(const ArrivalAnnouncement& ann)
{
    if (!ec_private_key_ || discovery_.state != DiscoveryState::COLLECTING ||
            propose_submitted_ || order_applied_ || crashCommsDisabled_ ||
            current_phase_ == ConsensusPhase::DEPARTED) return;
    const bool collusionEcho = shouldColludeOnFalseLane(ann);
    // Do not dedup/suppress radio (re)transmission here: repeated/gossiped
    // forged announcements must be able to trigger a retransmit of this same
    // authenticated echo through the existing discovery retry behavior
    // (handleArrivalAnnouncement's re-echo path), otherwise one lost packet
    // permanently drops this colluder's signature from the certificate.
    // collectArrivalEcho() already dedupes by echoingReplicaId on the
    // claimant's side, so retransmission cannot inflate the distinct signer
    // count.
    const auto claimHash = arrivalAnnouncementHash(ann);
    const std::string cacheKey = std::to_string(ann.epoch) + ":" + ann.carId + ":" +
        hashHex(claimHash);
    auto cached = cached_arrival_echoes_.find(cacheKey);
    if (cached != cached_arrival_echoes_.end()) {
        sendBFTMessage(-1, serializeArrivalEcho(cached->second), kArrivalEchoType,
                       false, true);
        return;
    }
    if (cached_arrival_rejections_.count(cacheKey)) return;

    ArrivalEcho echo;
    echo.echoingReplicaId = replicaId_;
    echo.targetCarId      = ann.carId;
    echo.lane             = ann.lane;
    echo.positionInLane   = ann.positionInLane;
    echo.direction        = ann.direction;
    echo.claimHash        = claimHash;
    echo.isAmbulance      = ann.isAmbulance;
    echo.epoch            = ann.epoch;
    if (collusionEcho) {
        echo.observedCue = static_cast<ObservedCue>(ann.direction);
    } else {
        const std::string sampleKey = cacheKey;
        auto sampleIt = arrival_perception_samples_.find(sampleKey);
        if (sampleIt == arrival_perception_samples_.end()) {
            cached_arrival_rejections_.insert(cacheKey);
            return;
        }
        echo.observedCue = sampleIt->second.observedCue;
    }
    std::memcpy(echo.signerPubKey, ec_pub_key_, CRYPTO_PUBKEY_BYTES);
    std::memset(echo.signature, 0, CRYPTO_SIG_MAX_BYTES);
    echo.signatureLen = 0;

    const std::string toSign = arrivalEchoSigningPayload(echo);
    if (!CryptoAuth::instance().signBytes(ec_private_key_,
            reinterpret_cast<const uint8_t*>(toSign.c_str()), toSign.size(),
            echo.signature, echo.signatureLen)) {
        std::cerr << "[ECHO-SEND] Replica " << replicaId_ << " sign failed for echo of "
                  << ann.carId << "\n";
        return;
    }

    if (is_byzantine_ && byzantine_type_ == BYZANTINE_INVALID_SIG) {
        echo.signature[0] = echo.signature[1] =
        echo.signature[2] = echo.signature[3] = 0xDE;
        echo.signatureLen = 4;
        std::cout << "[BYZANTINE] r" << replicaId_ << " INVALID_SIG: corrupted echo for "
                  << ann.carId << "\n";
    }

    cached_arrival_echoes_[cacheKey] = echo;
    sendBFTMessage(-1, serializeArrivalEcho(echo), kArrivalEchoType,
                   false, true);
    std::cout << "[ECHO-SEND] Replica " << replicaId_ << " → " << ann.carId
              << " ARRIVAL_ECHO cue=" << ResDBPerception::cueName(echo.observedCue)
              << " claimHash=" << hashHex(echo.claimHash)
              << " sigLen=" << (int)echo.signatureLen << "\n";
    if (collusionEcho) {
        std::cout << "[FALSE-LANE-COLLUSION-ECHO] target=" << ann.carId
                  << " signer=" << replicaId_
                  << " epoch=" << ann.epoch
                  << " source=" << (extractReplicaId(ann.carId) == replicaId_ ? "self" : "peer")
                  << "\n";
    }
    if (debug_cert_protocol_) {
        std::cout << "[CERT-DEBUG] sendArrivalEcho r" << replicaId_ << " targetReplicaId=-1"
                  << " signPayload=\"" << toSign << "\"\n";
    }
}

bool ResDBIntersectionApp::isExactFalseLaneClaim(const ArrivalAnnouncement& ann) const
{
    const int target = extractReplicaId(ann.carId);
    return target >= 0 && target < total_vehicles_ &&
        ann.epoch == static_cast<int>(current_epoch_) &&
        ann.laneId == "BYZANTINE_FAKE_LANE" && ann.lane == "X";
}

bool ResDBIntersectionApp::shouldColludeOnFalseLane(const ArrivalAnnouncement& ann) const
{
    if (!is_byzantine_ || byzantine_type_ != BYZANTINE_FALSE_LANE) return false;
    const int target = extractReplicaId(ann.carId);
    return isExactFalseLaneClaim(ann) &&
        false_lane_colluder_ids_.count(replicaId_) > 0 &&
        false_lane_colluder_ids_.count(target) > 0;
}

const char* ResDBIntersectionApp::phase2AttackKindName() const
{
    switch (phase2_attack_kind_) {
    case Phase2AttackKind::WRONG_APPROACH: return "WRONG_APPROACH";
    case Phase2AttackKind::FALSE_DIRECTION: return "FALSE_DIRECTION";
    case Phase2AttackKind::NONE: return "NONE";
    }
    return "NONE";
}

bool ResDBIntersectionApp::isPhase2AttackTarget(const ArrivalAnnouncement& ann) const
{
    return phase2_attack_kind_ != Phase2AttackKind::NONE &&
        extractReplicaId(ann.carId) == phase2_attack_target_replica_id_ &&
        ann.epoch == static_cast<int>(current_epoch_);
}

bool ResDBIntersectionApp::shouldPhase2Collude(const ArrivalAnnouncement& ann) const
{
    return isPhase2AttackTarget(ann) &&
        phase2_evidence_colluder_ids_.count(replicaId_) > 0;
}

bool ResDBIntersectionApp::isArrivalSignerEligible(int signerId) const
{
    // ARRIVAL certificates establish the next ORDER electorate, so late
    // configured replicas may witness before that ORDER is committed. The
    // configured replica range is therefore the correct active discovery set.
    return signerId >= 0 && signerId < total_vehicles_;
}

void ResDBIntersectionApp::collectArrivalEcho(const ArrivalEcho& echo, const char* source)
{
    const std::string myCarId = "veh" + std::to_string(replicaId_);
    if (echo.targetCarId != myCarId || cert_broadcast_ ||
            discovery_.state != DiscoveryState::COLLECTING || propose_submitted_ ||
            order_applied_ || crashCommsDisabled_ ||
            current_phase_ == ConsensusPhase::DEPARTED) return;
    if (!isArrivalSignerEligible(echo.echoingReplicaId) || echo.signatureLen == 0) return;
    if (!WitnessKeyRegistry::instance().matches(echo.echoingReplicaId, echo.signerPubKey)) return;

    auto stateIt = local_vehicle_states_.find(myCarId);
    if (stateIt == local_vehicle_states_.end()) return;
    const VehicleState& state = stateIt->second;
    auto hashIt = local_claim_hashes_.find(myCarId);
    if (hashIt == local_claim_hashes_.end()) return;
    if (echo.epoch != static_cast<int>(current_epoch_) ||
            echo.lane != state.lane || echo.positionInLane != state.positionInLane ||
            echo.direction != state.direction || echo.isAmbulance != state.isAmbulance ||
            echo.claimHash != hashIt->second) {
        return;
    }
    const std::string toSign = arrivalEchoSigningPayload(echo);
    if (!CryptoAuth::instance().verifyBytes(
            echo.signerPubKey,
            reinterpret_cast<const uint8_t*>(toSign.c_str()), toSign.size(),
            echo.signature, echo.signatureLen)) {
        return;
    }

    auto& echoes = my_received_echoes_[myCarId];
    for (const auto& existing : echoes)
        if (existing.echoingReplicaId == echo.echoingReplicaId) return;
    if (echoes.size() >= static_cast<size_t>(std::max(0, total_vehicles_))) return;
    echoes.push_back(echo);

    const int f = tolerated_faults_ >= 0 ? tolerated_faults_ : (total_vehicles_ - 1) / 3;
    const int required = f + 1;
    std::cout << "[ECHO-RECV] Replica " << replicaId_ << " received echo from "
              << echo.echoingReplicaId << " (" << echoes.size() << " so far)\n";
    if (state.lane == "X") {
        std::cout << "[FALSE-LANE-COLLUSION-ECHO] target=" << myCarId
                  << " signer=" << echo.echoingReplicaId
                  << " count=" << echoes.size() << "/" << required
                  << " source=" << (source ? source : "unknown") << "\n";
    }
    if (static_cast<int>(echoes.size()) < required) return;
    armArrivalCertFinalizeTimer(myCarId, required);
}

void ResDBIntersectionApp::armArrivalCertFinalizeTimer(const std::string& carId, int required)
{
    if (cert_broadcast_ || discovery_.state != DiscoveryState::COLLECTING ||
            !arrival_cert_finalize_timer_ || arrival_cert_finalize_timer_->isScheduled()) return;
    arrival_cert_threshold_reached_at_ = simTime();
    const simtime_t finalizeAt = simTime() + direction_eligibility_collection_window_sec_;
    std::cout << "[CERT-COLLECT] target=" << carId
              << " epoch=" << current_epoch_
              << " echoCount=" << my_received_echoes_[carId].size()
              << " threshold=" << required
              << " thresholdReachedAt=" << arrival_cert_threshold_reached_at_
              << " finalizeAt=" << finalizeAt << "\n";
    if (direction_eligibility_collection_window_sec_ <= 0.0) {
        finalizeLocalArrivalCert("threshold-immediate");
    } else {
        scheduleAt(finalizeAt, arrival_cert_finalize_timer_);
    }
}

void ResDBIntersectionApp::cancelArrivalCertFinalizeTimer()
{
    if (arrival_cert_finalize_timer_ && arrival_cert_finalize_timer_->isScheduled())
        cancelEvent(arrival_cert_finalize_timer_);
    arrival_cert_threshold_reached_at_ = -1;
}

bool ResDBIntersectionApp::finalizeLocalArrivalCert(const char* reason)
{
    if (cert_broadcast_) return false;
    const std::string myCarId = "veh" + std::to_string(replicaId_);
    auto stateIt = local_vehicle_states_.find(myCarId);
    auto echoIt = my_received_echoes_.find(myCarId);
    auto hashIt = local_claim_hashes_.find(myCarId);
    const int f = tolerated_faults_ >= 0 ? tolerated_faults_ : (total_vehicles_ - 1) / 3;
    const int required = f + 1;
    if (stateIt == local_vehicle_states_.end() || echoIt == my_received_echoes_.end() ||
            hashIt == local_claim_hashes_.end() ||
            static_cast<int>(echoIt->second.size()) < required) return false;
    cancelArrivalCertFinalizeTimer();
    cert_broadcast_ = true;
    const VehicleState& state = stateIt->second;
    const auto& echoes = echoIt->second;
    ArrivalCert cert;
    cert.carId = myCarId;
    cert.lane = state.lane;
    cert.positionInLane = state.positionInLane;
    cert.direction = state.direction;
    cert.claimHash = hashIt->second;
    cert.isAmbulance = state.isAmbulance;
    cert.epoch = static_cast<int>(current_epoch_);
    cert.echoes = echoes;
    if (state.arrival_time_us > 0) {
        const double announceTimeSec = static_cast<double>(state.arrival_time_us) / 1000000.0;
        const double latencySec = simTime().dbl() - announceTimeSec;
        std::cout << "[METRICS " << replicaId_ << "] Cert_Created_Time: " << simTime()
                  << " car=" << myCarId << " announce_time=" << announceTimeSec
                  << " latency=" << latencySec << "s\n";
        std::cout << "[METRICS " << replicaId_
                  << "] Cert_Creation_Latency: " << latencySec << "s\n";
    }
    std::cout << "[CERT-ASSEMBLE] target=" << myCarId
              << " epoch=" << current_epoch_
              << " echoCount=" << cert.echoes.size()
              << " threshold=" << required
              << " reason=" << (reason ? reason : "unspecified") << "\n";
    const int support = directionSupport(cert);
    const Direction derived = eligibleDirection(cert);
    int bSig = 0;
    int selfAttestations = 0;
    for (const auto& echo : cert.echoes) {
        if (!isValidArrivalEchoForCert(echo, cert)) continue;
        const bool self = echo.echoingReplicaId == extractReplicaId(cert.carId);
        const bool supporting =
            echo.observedCue == static_cast<ObservedCue>(cert.direction);
        const bool byzantine = isReplicaConfiguredByzantine(echo.echoingReplicaId);
        if (self) ++selfAttestations;
        if (supporting && byzantine) ++bSig;
        std::cout << "[CERT-EVIDENCE] target=" << cert.carId
                  << " epoch=" << cert.epoch
                  << " signer=" << echo.echoingReplicaId
                  << " cue=" << ResDBPerception::cueName(echo.observedCue)
                  << " self=" << (self ? 1 : 0)
                  << " byzantine=" << (byzantine ? 1 : 0)
                  << " supporting=" << (supporting ? 1 : 0) << "\n";
    }
    std::cout << "[DIR-ELIGIBILITY] target=" << cert.carId
              << " epoch=" << cert.epoch
              << " declared=" << dirToStr(cert.direction)
              << " support=" << support
              << " threshold=" << required
              << " echoCount=" << cert.echoes.size()
              << " selfAttestations=" << selfAttestations
              << " b_sig=" << bSig
              << " derivedDirection=" << dirToStr(derived) << "\n";
    std::cout << "[TRUST-TIER] target=" << cert.carId
              << " epoch=" << cert.epoch
              << " tier=" << (derived == DIR_UNKNOWN ? "SIGNED-UNKNOWN" : "SIGNED-DIRECTION")
              << "\n";
    if (replicaId_ == phase2_attack_target_replica_id_ &&
            phase2_attack_kind_ != Phase2AttackKind::NONE) {
        const bool falseLaneCert =
            phase2_attack_kind_ == Phase2AttackKind::WRONG_APPROACH &&
            cert.lane != intended_lane_;
        const bool falseEligibility =
            phase2_attack_kind_ == Phase2AttackKind::FALSE_DIRECTION &&
            derived == cert.direction;
        std::cout << "[PHASE2-ATTACK-OUTCOME] target=" << cert.carId
                  << " epoch=" << cert.epoch
                  << " kind=" << phase2AttackKindName()
                  << " laneCertified=1"
                  << " falseLaneCert=" << (falseLaneCert ? 1 : 0)
                  << " falseEligibility=" << (falseEligibility ? 1 : 0)
                  << " support=" << support
                  << " threshold=" << required
                  << " b_sig=" << bSig << "\n";
    }
    if (state.lane == "X") {
        std::cout << "[FALSE-LANE-COLLUSION-CERT] target=" << myCarId
                  << " signers=";
        for (size_t i = 0; i < echoes.size(); ++i) {
            if (i) std::cout << ",";
            std::cout << echoes[i].echoingReplicaId;
        }
        std::cout << " threshold=" << required << "\n";
    }
    broadcastArrivalCert(cert);
    return true;
}

// ── handleArrivalEcho ─────────────────────────────────────────────────────────

void ResDBIntersectionApp::handleArrivalEcho(BFTMessage* msg)
{
    ArrivalEcho echo = deserializeArrivalEcho(msg);
    if (debug_cert_protocol_)
        std::cout << "[CERT-DEBUG] handleArrivalEcho r" << replicaId_
                  << " frameFrom=r" << msg->getFromReplicaId()
                  << " echo.target=" << echo.targetCarId
                  << " echo.signer=r" << echo.echoingReplicaId << "\n";
    std::string myCarId = "veh" + std::to_string(replicaId_);
    if (echo.targetCarId != myCarId) {
        if (debug_cert_protocol_)
            std::cout << "[CERT-DEBUG] handleArrivalEcho r" << replicaId_
                      << " wrong target: echo for " << echo.targetCarId
                      << " (my " << myCarId << ") from r" << echo.echoingReplicaId << "\n";
        return;
    }
    if (cert_broadcast_) {
        if (debug_cert_protocol_)
            std::cout << "[CERT-DEBUG] handleArrivalEcho r" << replicaId_
                      << " drop echo from r" << echo.echoingReplicaId
                      << " (cert_broadcast_ already true)\n";
        return;
    }

    collectArrivalEcho(echo, "radio");
}

// ── broadcastArrivalCert ──────────────────────────────────────────────────────

void ResDBIntersectionApp::scheduleNextCertRetry()
{
    if (!enable_cert_retries_ || cert_pending_retries_.carId.empty()) return;
    if (propose_submitted_ || order_applied_) {
        stopCertBroadcastRetries();
        return;
    }
    if (!cert_retry_timer_)
        cert_retry_timer_ = new cMessage("resdbCertRetry");
    scheduleAt(simTime() + cert_retry_interval_, cert_retry_timer_);
}

void ResDBIntersectionApp::stopCertBroadcastRetries()
{
    if (cert_retry_timer_) {
        if (cert_retry_timer_->isScheduled())
            cancelEvent(cert_retry_timer_);
        delete cert_retry_timer_;
        cert_retry_timer_ = nullptr;
    }
    cert_pending_retries_.carId.clear();
    cert_pending_retries_.echoes.clear();
    cert_retry_count_ = 0;
}

void ResDBIntersectionApp::broadcastCollectedCerts(const char* reason)
{
    std::vector<ArrivalCert> certs;
    {
        std::lock_guard<std::mutex> lk(certs_mutex_);
        certs.reserve(collected_certs_.size());
        for (const auto& kv : collected_certs_) {
            certs.push_back(kv.second);
        }
    }
    if (certs.empty()) return;

    for (const auto& cert : certs) {
        sendBFTMessage(-1, serializeArrivalCert(cert), kArrivalCertType);
    }
    std::cout << "[CERT-GOSSIP] r" << replicaId_
              << " broadcast collected certs count=" << certs.size()
              << " reason=" << (reason ? reason : "stop-zone")
              << " t=" << simTime() << "\n";
}

void ResDBIntersectionApp::startStopZoneCertGossip(const char* reason, bool immediate)
{
    if (discovery_.state != DiscoveryState::COLLECTING)
        return;

    const simtime_t newDeadline = simTime() + cert_collection_timeout_;
    if (cert_gossip_deadline_ < newDeadline)
        cert_gossip_deadline_ = newDeadline;

    if (immediate)
        broadcastCollectedCerts(reason);
    scheduleNextStopZoneCertGossip();
}

void ResDBIntersectionApp::scheduleNextStopZoneCertGossip()
{
    if (discovery_.state != DiscoveryState::COLLECTING)
        return;
    if (CertPrimary() != replicaId_)
        return;
    if (cert_gossip_deadline_ >= SIMTIME_ZERO && simTime() >= cert_gossip_deadline_)
        return;

    if (!cert_gossip_timer_)
        cert_gossip_timer_ = new cMessage("resdbCertGossip");
    if (cert_gossip_timer_->isScheduled())
        return;

    simtime_t interval = cert_retry_interval_;
    if (interval < 0.5)
        interval = 0.5;
    simtime_t next = simTime() + interval;
    if (cert_gossip_deadline_ >= SIMTIME_ZERO && next > cert_gossip_deadline_)
        next = cert_gossip_deadline_;
    scheduleAt(next, cert_gossip_timer_);
}

void ResDBIntersectionApp::stopStopZoneCertGossip()
{
    if (cert_gossip_timer_) {
        if (cert_gossip_timer_->isScheduled())
            cancelEvent(cert_gossip_timer_);
        delete cert_gossip_timer_;
        cert_gossip_timer_ = nullptr;
    }
    cert_gossip_deadline_ = -1;
}

void ResDBIntersectionApp::broadcastArrivalCert(const ArrivalCert& cert)
{
    discovery_.localCert = LocalCertState::QUEUED;
    sendBFTMessage(-1, serializeArrivalCert(cert), kArrivalCertType, true);
    std::cout << "[CERT-BROADCAST] Replica " << replicaId_ << " broadcast ARRIVAL_CERT for "
              << cert.carId << "\n";

    if (enable_cert_retries_) {
        cert_pending_retries_ = cert;
        cert_retry_count_   = 0;
        if (cert_retry_timer_) {
            if (cert_retry_timer_->isScheduled())
                cancelEvent(cert_retry_timer_);
        } else {
            cert_retry_timer_ = new cMessage("resdbCertRetry");
        }
        scheduleNextCertRetry();
    }

    // OMNeT++ modules don't receive their own channel broadcasts — self-store.
    int certPrimary = -1;
    if (!collected_certs_.count(cert.carId)) {
        int staticCerts = 0;
        int allCerts = 0;
        std::lock_guard<std::mutex> lk(certs_mutex_);
        collected_certs_[cert.carId] = cert;
        staticCerts = countStaticCollectedCerts();
        allCerts = (int)collected_certs_.size();
        certPrimary = CertPrimary();
        std::cout << "[CERT-STORED-SELF] Replica " << replicaId_ << " self-stored cert for "
                  << cert.carId << " static=(" << staticCerts << "/" << total_vehicles_
                  << ") all=" << allCerts
                  << " cert_primary=" << certPrimary << "\n";
        const bool emergencyCancelStarted = maybeTriggerEmergencyRollbackFromCert(cert);
        (void)emergencyCancelStarted;
    }
    if (entered_stop_zone_ && discovery_.state == DiscoveryState::COLLECTING) {
        startStopZoneCertGossip("self-cert-stored", replicaId_ == certPrimary);
    }
    maybeAdvanceDiscovery("self-cert");
}

// ── handleArrivalCert ─────────────────────────────────────────────────────────

void ResDBIntersectionApp::handleArrivalCert(BFTMessage* msg)
{
    ArrivalCert cert = deserializeArrivalCert(msg);
    if (cert.carId.empty()) return;
    if (!validateArrivalCert(cert)) {
        std::cout << "[CERT-INVALID] Replica " << replicaId_ << " dropped ARRIVAL_CERT from "
                  << cert.carId << "\n";
        return;
    }
    if (collected_certs_.count(cert.carId)) return;  // dedup

    {
        std::lock_guard<std::mutex> lk(certs_mutex_);
        collected_certs_[cert.carId] = cert;
    }
    int staticCerts = 0;
    int allCerts = 0;
    int certPrimary = -1;
    {
        std::lock_guard<std::mutex> lk(certs_mutex_);
        staticCerts = countStaticCollectedCerts();
        allCerts = (int)collected_certs_.size();
        certPrimary = CertPrimary();
    }
    std::cout << "[CERT-STORED] Replica " << replicaId_ << " stored ARRIVAL_CERT for "
              << cert.carId << " static=(" << staticCerts << "/" << total_vehicles_
              << ") all=" << allCerts
              << " cert_primary=" << certPrimary << "\n";

    // Reconstruct VehicleState if the announce was lost.
    if (!local_vehicle_states_.count(cert.carId)) {
        VehicleState vs;
        vs.vehicleId      = cert.carId;
        vs.lane           = cert.lane;
        vs.positionInLane = cert.positionInLane;
        vs.direction      = cert.direction;
        vs.isAmbulance    = cert.isAmbulance;
        local_vehicle_states_[cert.carId] = vs;
        const bool newIntent = observed_intent_cars_.insert(cert.carId).second;
        if (newIntent && discovery_.state == DiscoveryState::COLLECTING)
            noteDiscoveryIntent(cert.carId, "certificate");
    }

    // Epidemic relay: each node forwards each validated cert once.
    // cert already carries f+1 signatures so recipients can verify without
    // re-accumulating votes.  cert_relay_tracker_ deduplicates per carId.
    if (discovery_.state != DiscoveryState::COLLECTING) {
        std::cout << "[CERT-RELAY-STOP] r" << replicaId_ << " suppressed relay for "
                  << cert.carId << " order_applied=" << (order_applied_ ? 1 : 0)
                  << " propose_submitted=" << (propose_submitted_ ? 1 : 0)
                  << " discovery_state=" << discoveryStateName()
                  << " t=" << simTime() << "\n";
    } else if (cert_relay_tracker_.tryRelay(cert.carId)) {
        sendBFTMessage(-1, serializeArrivalCert(cert), kArrivalCertType);
        std::cout << "[CERT-RELAY] r" << replicaId_ << " relayed cert for "
                  << cert.carId << " t=" << simTime() << "\n";
    }
    if (entered_stop_zone_ && discovery_.state == DiscoveryState::COLLECTING) {
        startStopZoneCertGossip("cert-stored", replicaId_ == certPrimary);
    }

    const bool emergencyCancelStarted = maybeTriggerEmergencyRollbackFromCert(cert);

    (void)emergencyCancelStarted;
    maybeAdvanceDiscovery("cert-stored");
}

// ── validateArrivalCert ───────────────────────────────────────────────────────

bool ResDBIntersectionApp::validateArrivalCert(const ArrivalCert& cert)
{
    if (cert.direction == DIR_UNKNOWN || cert.carId.empty()) return false;
    int f = tolerated_faults_ >= 0 ? tolerated_faults_ : (total_vehicles_ - 1) / 3;
    int required = f + 1;
    if ((int)cert.echoes.size() < required) {
        if (debug_cert_protocol_)
            std::cout << "[CERT-DEBUG] validateArrivalCert fail: carId=" << cert.carId
                      << " echo_count=" << cert.echoes.size() << " need>=" << required << "\n";
        return false;
    }
    if (cert.echoes.size() > static_cast<size_t>(std::max(0, total_vehicles_))) {
        if (debug_cert_protocol_)
            std::cout << "[CERT-DEBUG] validateArrivalCert fail: carId=" << cert.carId
                      << " echo_count=" << cert.echoes.size() << " exceeds N="
                      << total_vehicles_ << "\n";
        return false;
    }

    std::set<int> seen;
    int valid = 0;
    for (const auto& echo : cert.echoes) {
        if (!isArrivalSignerEligible(echo.echoingReplicaId)) {
            if (debug_cert_protocol_)
                std::cout << "[CERT-DEBUG] validateArrivalCert skip ineligible signer r"
                          << echo.echoingReplicaId << " carId=" << cert.carId << "\n";
            continue;
        }
        if (!seen.insert(echo.echoingReplicaId).second) {
            if (debug_cert_protocol_)
                std::cout << "[CERT-DEBUG] validateArrivalCert reject duplicate signer r"
                          << echo.echoingReplicaId << " carId=" << cert.carId << "\n";
            return false;
        }
        if (echo.signatureLen == 0) {
            if (debug_cert_protocol_)
                std::cout << "[CERT-DEBUG] validateArrivalCert skip empty sig r"
                          << echo.echoingReplicaId << " carId=" << cert.carId << "\n";
            continue;
        }
        if (echo.targetCarId != cert.carId || echo.epoch != cert.epoch ||
                echo.lane != cert.lane || echo.positionInLane != cert.positionInLane ||
                echo.direction != cert.direction || echo.isAmbulance != cert.isAmbulance ||
                echo.claimHash != cert.claimHash) {
            if (debug_cert_protocol_)
                std::cout << "[CERT-DEBUG] validateArrivalCert skip semantic mismatch signer=r"
                          << echo.echoingReplicaId << " carId=" << cert.carId << "\n";
            continue;
        }
        if (!WitnessKeyRegistry::instance().matches(
                echo.echoingReplicaId, echo.signerPubKey)) {
            if (debug_cert_protocol_)
                std::cout << "[CERT-DEBUG] validateArrivalCert skip key mismatch signer=r"
                          << echo.echoingReplicaId << " carId=" << cert.carId << "\n";
            continue;
        }
        const std::string toSign = arrivalEchoSigningPayload(echo);
        if (CryptoAuth::instance().verifyBytes(
                echo.signerPubKey,
                reinterpret_cast<const uint8_t*>(toSign.c_str()), toSign.size(),
                echo.signature, echo.signatureLen)) {
            valid++;
        } else if (debug_cert_protocol_) {
            std::cout << "[CERT-DEBUG] validateArrivalCert verify FAIL carId=" << cert.carId
                      << " signer=r" << echo.echoingReplicaId
                      << " signedPayload=\"" << toSign << "\" sigLen=" << (int)echo.signatureLen
                      << "\n";
        }
    }
    if (valid >= required) {
        return true;
    }
    if (debug_cert_protocol_)
        std::cout << "[CERT-DEBUG] validateArrivalCert fail: carId=" << cert.carId
                  << " valid_sigs=" << valid << " need=" << required << "\n";
    return false;
}

int ResDBIntersectionApp::directionSupport(const ArrivalCert& cert) const
{
    std::set<int> seen;
    int support = 0;
    const ObservedCue declared = static_cast<ObservedCue>(cert.direction);
    for (const auto& echo : cert.echoes) {
        if (!seen.insert(echo.echoingReplicaId).second) continue;
        if (isValidArrivalEchoForCert(echo, cert) && echo.observedCue == declared) ++support;
    }
    return support;
}

bool ResDBIntersectionApp::isValidArrivalEchoForCert(
    const ArrivalEcho& echo, const ArrivalCert& cert) const
{
    if (!isArrivalSignerEligible(echo.echoingReplicaId) || echo.signatureLen == 0 ||
            echo.targetCarId != cert.carId || echo.epoch != cert.epoch ||
            echo.lane != cert.lane || echo.positionInLane != cert.positionInLane ||
            echo.direction != cert.direction || echo.isAmbulance != cert.isAmbulance ||
            echo.claimHash != cert.claimHash ||
            !WitnessKeyRegistry::instance().matches(echo.echoingReplicaId, echo.signerPubKey)) {
        return false;
    }
    const std::string payload = arrivalEchoSigningPayload(echo);
    return CryptoAuth::instance().verifyBytes(
        echo.signerPubKey, reinterpret_cast<const uint8_t*>(payload.data()), payload.size(),
        echo.signature, echo.signatureLen);
}

ResDBIntersectionApp::Direction ResDBIntersectionApp::eligibleDirection(
    const ArrivalCert& cert) const
{
    const int f = tolerated_faults_ >= 0 ? tolerated_faults_ : (total_vehicles_ - 1) / 3;
    return directionSupport(cert) >= f + 1 ? cert.direction : DIR_UNKNOWN;
}

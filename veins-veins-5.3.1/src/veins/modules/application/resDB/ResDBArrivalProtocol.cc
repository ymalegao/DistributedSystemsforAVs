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
        const ArrivalAnnouncement& ann)
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
    return result;
}

ArrivalAnnouncement
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
    std::vector<uint8_t> sigVec(echo.signature, echo.signature + echo.signatureLen);
    std::stringstream ss;
    ss << echo.echoingReplicaId << "|"
       << echo.targetCarId      << "|"
       << echo.lane             << "|"
       << echo.positionInLane   << "|"
       << dirToStr(echo.direction) << "|"
       << (echo.isAmbulance ? "1" : "0") << "|"
       << echo.epoch            << "|"
       << toHex(pubVec) << "," << toHex(sigVec);
    std::string s = ss.str();
    return std::vector<uint8_t>(s.begin(), s.end());
}

ArrivalEcho
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
    if (parts.size() >= 8) {
        echo.echoingReplicaId = std::stoi(parts[0]);
        echo.targetCarId      = parts[1];
        echo.lane             = parts[2];
        echo.positionInLane   = std::stoi(parts[3]);
        echo.direction        = strToDir(parts[4]);
        echo.isAmbulance      = (parts[5] == "1");
        echo.epoch            = std::stoi(parts[6]);
        const std::string& sf = parts[7];
        size_t comma = sf.find(',');
        if (comma != std::string::npos) {
            auto pubVec = fromHex(sf.substr(0, comma));
            auto sigVec = fromHex(sf.substr(comma + 1));
            if (pubVec.size() == CRYPTO_PUBKEY_BYTES)
                std::memcpy(echo.signerPubKey, pubVec.data(), CRYPTO_PUBKEY_BYTES);
            if (sigVec.size() <= CRYPTO_SIG_MAX_BYTES) {
                std::memcpy(echo.signature, sigVec.data(), sigVec.size());
                echo.signatureLen = static_cast<uint8_t>(sigVec.size());
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
       << (cert.isAmbulance ? "1" : "0") << "|"
       << cert.epoch;
    for (const auto& echo : cert.echoes) {
        std::vector<uint8_t> pubVec(echo.signerPubKey, echo.signerPubKey + CRYPTO_PUBKEY_BYTES);
        std::vector<uint8_t> sigVec(echo.signature, echo.signature + echo.signatureLen);
        ss << "|" << echo.echoingReplicaId << ":"
           << toHex(pubVec) << "," << toHex(sigVec);
    }
    std::string s = ss.str();
    return std::vector<uint8_t>(s.begin(), s.end());
}

ArrivalCert
ResDBIntersectionApp::deserializeArrivalCert(BFTMessage* msg)
{
    std::vector<uint8_t> payload(msg->getPayloadArraySize());
    for (size_t i = 0; i < payload.size(); i++) payload[i] = msg->getPayload(i);
    std::string s(payload.begin(), payload.end());
    auto parts = splitStr(s, '|');
    ArrivalCert cert;
    if (parts.size() < 6) return cert;
    cert.carId          = parts[0];
    cert.lane           = parts[1];
    cert.positionInLane = std::stoi(parts[2]);
    cert.direction      = strToDir(parts[3]);
    cert.isAmbulance    = (parts[4] == "1");
    cert.epoch          = std::stoi(parts[5]);
    for (size_t i = 6; i < parts.size(); i++) {
        size_t colon = parts[i].find(':');
        if (colon == std::string::npos) continue;
        ArrivalEcho echo;
        std::memset(echo.signerPubKey, 0, CRYPTO_PUBKEY_BYTES);
        std::memset(echo.signature, 0, CRYPTO_SIG_MAX_BYTES);
        echo.signatureLen = 0;
        echo.echoingReplicaId = std::stoi(parts[i].substr(0, colon));
        std::string sf = parts[i].substr(colon + 1);
        size_t comma = sf.find(',');
        if (comma != std::string::npos) {
            auto pubVec = fromHex(sf.substr(0, comma));
            auto sigVec = fromHex(sf.substr(comma + 1));
            if (pubVec.size() == CRYPTO_PUBKEY_BYTES)
                std::memcpy(echo.signerPubKey, pubVec.data(), CRYPTO_PUBKEY_BYTES);
            if (sigVec.size() <= CRYPTO_SIG_MAX_BYTES) {
                std::memcpy(echo.signature, sigVec.data(), sigVec.size());
                echo.signatureLen = static_cast<uint8_t>(sigVec.size());
            }
        }
        echo.targetCarId      = cert.carId;
        echo.lane             = cert.lane;
        echo.positionInLane   = cert.positionInLane;
        echo.direction        = cert.direction;
        echo.isAmbulance      = cert.isAmbulance;
        echo.epoch            = cert.epoch;
        cert.echoes.push_back(echo);
    }
    return cert;
}

// ── Discovery-round state ─────────────────────────────────────────────────────

int ResDBIntersectionApp::countStaticCollectedCerts() const
{
    int count = 0;
    for (const auto& kv : ctx_.collected_certs_) {
        const int rid = extractReplicaId(kv.first);
        if (ctx_.cancel_pending_) {
            if (shouldIncludeInRollbackMembership(rid))
                ++count;
        } else if (rid >= 0 && rid < ctx_.total_vehicles_) {
            ++count;
        }
    }
    return count;
}

int ResDBIntersectionApp::CertPrimary() const
{
    int primary = -1;
    for (const auto& kv : ctx_.collected_certs_) {
        const int rid = extractReplicaId(kv.first);
        const bool eligible = ctx_.cancel_pending_
            ? shouldIncludeInRollbackMembership(rid)
            : (rid >= 0 && rid < ctx_.total_vehicles_);
        if (eligible && (primary < 0 || rid < primary)) {
            primary = rid;
        }
    }
    return primary;
}

const char* ResDBIntersectionApp::discoveryStateName() const
{
    switch (ctx_.discovery_.state) {
    case DiscoveryState::INACTIVE: return "INACTIVE";
    case DiscoveryState::COLLECTING: return "COLLECTING";
    case DiscoveryState::DRAINING_CERTS: return "DRAINING_CERTS";
    case DiscoveryState::COMPLETE: return "COMPLETE";
    }
    return "UNKNOWN";
}

void ResDBIntersectionApp::startDiscoveryRound(const char* reason)
{
    if (ctx_.current_phase_ == ConsensusPhase::DEPARTED || ctx_.is_departed_) return;
    if (discovery_deadline_msg_->isScheduled()) cancelEvent(discovery_deadline_msg_);
    if (discovery_settle_msg_->isScheduled()) cancelEvent(discovery_settle_msg_);
    ctx_.discovery_.reset(ctx_.current_epoch_, simTime());
    resetOrderCandidate(reason ? reason : "discovery-start");
    std::cout << "[DISCOVERY-BEGIN] r" << ctx_.replicaId_
              << " epoch=" << ctx_.discovery_.epoch
              << " reason=" << (reason ? reason : "start")
              << " stop_zone=" << (entered_stop_zone_ ? 1 : 0)
              << " t=" << simTime() << "\n";
    if (entered_stop_zone_) armDiscoveryTimers(reason);
}

void ResDBIntersectionApp::armDiscoveryTimers(const char* reason)
{
    if (!entered_stop_zone_ || ctx_.discovery_.state != DiscoveryState::COLLECTING ||
            ctx_.propose_submitted_ || ctx_.order_applied_) return;
    if (!discovery_deadline_msg_->isScheduled()) {
        scheduleAt(simTime() + cert_collection_timeout_, discovery_deadline_msg_);
        ctx_.discovery_.collectionStartedAt = simTime();
        std::cout << "[DISCOVERY-DEADLINE] r" << ctx_.replicaId_
                  << " epoch=" << ctx_.discovery_.epoch
                  << " armed_for=" << simTime() + cert_collection_timeout_
                  << " reason=" << (reason ? reason : "arm") << "\n";
    }
    simtime_t settleAt = ctx_.discovery_.lastNewIntentAt + discovery_intent_settle_;
    if (settleAt < simTime()) settleAt = simTime();
    if (discovery_settle_msg_->isScheduled()) cancelEvent(discovery_settle_msg_);
    scheduleAt(settleAt, discovery_settle_msg_);
}

void ResDBIntersectionApp::noteDiscoveryIntent(const std::string& carId, const char* source)
{
    if (carId.empty() || ctx_.propose_submitted_ || ctx_.order_applied_ ||
            ctx_.current_phase_ == ConsensusPhase::DEPARTED) return;
    if (ctx_.discovery_.state == DiscoveryState::DRAINING_CERTS ||
            ctx_.discovery_.state == DiscoveryState::COMPLETE) {
        resetOrderCandidate("discovery-reopened");
        ctx_.discovery_.state = DiscoveryState::COLLECTING;
        ctx_.discovery_.closeReason = DiscoveryCloseReason::NONE;
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
    ctx_.discovery_.lastNewIntentAt = simTime();
    std::cout << "[DISCOVERY-VIEW] r" << ctx_.replicaId_
              << " epoch=" << ctx_.discovery_.epoch
              << " new_intent=" << carId
              << " source=" << (source ? source : "announce")
              << " intents=" << observed_intent_cars_.size()
              << " certs=" << ctx_.collected_certs_.size()
              << " t=" << simTime() << "\n";
    armDiscoveryTimers("new-intent");
}

bool ResDBIntersectionApp::discoveryViewCertified(std::vector<int>* missing) const
{
    bool hasEligibleIntent = false;
    bool hasLocalIntent = !ctx_.cancel_pending_ || !rollback_local_recallable_;
    const std::string localCarId = "veh" + std::to_string(ctx_.replicaId_);
    std::lock_guard<std::mutex> lk(certs_mutex_);
    for (const auto& carId : observed_intent_cars_) {
        const int rid = extractReplicaId(carId);
        if (ctx_.cancel_pending_ && !shouldIncludeInRollbackMembership(rid)) continue;
        hasEligibleIntent = true;
        if (carId == localCarId) hasLocalIntent = true;
        if (!ctx_.collected_certs_.count(carId)) {
            if (missing) missing->push_back(rid);
            else return false;
        }
    }
    return hasEligibleIntent && hasLocalIntent && (!missing || missing->empty());
}

void ResDBIntersectionApp::maybeAdvanceDiscovery(const char* reason, bool deadline)
{
    if (ctx_.discovery_.state != DiscoveryState::COLLECTING || !entered_stop_zone_ ||
            ctx_.propose_submitted_ || ctx_.order_applied_) return;
    if (deadline) {
        beginDiscoveryDrain(reason, true);
        return;
    }
    if (simTime() < ctx_.discovery_.lastNewIntentAt + discovery_intent_settle_) {
        armDiscoveryTimers("view-not-stable");
        return;
    }
    // Cold-start round: the ad hoc gossip topology has not converged yet
    // (no vehicle has had time to enter radio range of others or for relay
    // trees to form), so a quiet intent-set does not imply a complete view.
    // Only the hard per-round deadline may close epoch 0 early. Later
    // epochs (post-CANCEL reconvergence) keep the eager path below, since
    // by then the topology is already established.
    if (ctx_.discovery_.epoch == 0) return;
    if (!discoveryViewCertified()) return;
    beginDiscoveryDrain(reason, false);
}

void ResDBIntersectionApp::beginDiscoveryDrain(const char* reason, bool deadline)
{
    if (ctx_.discovery_.state != DiscoveryState::COLLECTING) return;
    ctx_.discovery_.state = DiscoveryState::DRAINING_CERTS;
    ctx_.discovery_.closeReason = deadline
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
    std::cout << "[DISCOVERY-DRAIN] r" << ctx_.replicaId_
              << " epoch=" << ctx_.discovery_.epoch
              << " reason=" << (reason ? reason : "view-closed")
              << " deadline=" << (deadline ? 1 : 0)
              << " local_cert_assembled=" << (ctx_.discovery_.localCertAssembled() ? 1 : 0)
              << " local_cert_aired=" << (ctx_.discovery_.localCertAired() ? 1 : 0)
              << " t=" << simTime() << "\n";
    maybeCompleteDiscoveryDrain(reason);
}

void ResDBIntersectionApp::maybeCompleteDiscoveryDrain(const char* reason)
{
    if (ctx_.discovery_.state != DiscoveryState::DRAINING_CERTS) return;
    if (ctx_.discovery_.localCertAssembled() && !ctx_.discovery_.localCertAired()) return;
    if (hasPendingDiscoveryCerts(ctx_.discovery_.epoch)) return;
    finishDiscoveryRound(reason);
}

void ResDBIntersectionApp::finishDiscoveryRound(const char* reason)
{
    if (ctx_.discovery_.state != DiscoveryState::DRAINING_CERTS) return;
    ctx_.discovery_.state = DiscoveryState::COMPLETE;
    stopCertBroadcastRetries();
    stopStopZoneCertGossip();
    std::vector<int> missing;
    discoveryViewCertified(&missing);
    std::cout << "[DISCOVERY-COMPLETE] r" << ctx_.replicaId_
              << " epoch=" << ctx_.discovery_.epoch
              << " reason=" << (reason ? reason : "drained")
              << " intents=" << observed_intent_cars_.size()
              << " certs=" << ctx_.collected_certs_.size()
              << " missing=";
    for (size_t i = 0; i < missing.size(); ++i) {
        if (i) std::cout << ",";
        std::cout << "r" << missing[i];
    }
    std::cout << " local_cert_aired=" << (ctx_.discovery_.localCertAired() ? 1 : 0)
              << " t=" << simTime() << "\n";

    evaluateOrderReadiness("discovery-complete");
}

void ResDBIntersectionApp::deactivateDiscovery(const char* reason)
{
    if (discovery_deadline_msg_ && discovery_deadline_msg_->isScheduled())
        cancelEvent(discovery_deadline_msg_);
    if (discovery_settle_msg_ && discovery_settle_msg_->isScheduled())
        cancelEvent(discovery_settle_msg_);
    stopCertBroadcastRetries();
    stopStopZoneCertGossip();
    cancelPendingDiscoveryTxs(reason ? reason : "inactive");
    ctx_.discovery_.state = DiscoveryState::INACTIVE;
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
        std::cerr << "[AMBULANCE] r" << ctx_.replicaId_
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
        std::cerr << "[AMBULANCE] r" << ctx_.replicaId_
                  << " signBytes failed for payload=" << ambPayload << "\n";
        return;
    }
    ann.ambulanceSigBytes.assign(sigOut, sigOut + sigLen);
}

// ── broadcastArrivalAnnouncement (port of V2VArrivalProtocol::broadcastArrivalAnnouncement) ──

void ResDBIntersectionApp::broadcastArrivalAnnouncement(bool forceEmergency)
{
    // forceEmergency lets a late ambulance keep announcing after ctx_.order_applied_ so peers
    // witness it and echo a CANCEL (the rollback trigger). Never override DEPARTED / no mobility.
    if (ctx_.current_phase_ == ConsensusPhase::DEPARTED) return;
    if (ctx_.order_applied_ && !forceEmergency) return;
    if (!mobility) return;

    TraCICommandInterface* traci = mobility->getCommandInterface();
    if (!traci) return;

    std::string myCarId = "veh" + std::to_string(ctx_.replicaId_);
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
        std::cout << "[BYZANTINE] r" << ctx_.replicaId_ << " FALSE_LANE: laneId=BYZANTINE_FAKE_LANE\n";
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
    std::cout << "[ANN-BROADCAST] Replica " << ctx_.replicaId_ << " positionInLane: " << rank << "\n";

    ann.direction          = strToDir(intended_direction_);
    ann.isAmbulance        = is_ambulance_;
    if (is_byzantine_ && byzantine_type_ == BYZANTINE_FAKE_AMBULANCE_FOLLOWER) {
        ann.isAmbulance = true;  // lie: claim ambulance without valid cert
        // ambulanceCertBytes intentionally left empty — cert gate catches this
        std::cout << "[BYZANTINE] r" << ctx_.replicaId_
                  << " FAKE_AMBULANCE_FOLLOWER: claiming ambulance without cert\n";
    } else if (is_ambulance_) {
        attachAmbulanceCryptoToAnnouncement(ann);
    }
    ann.claimedArrivalTime = simTime().dbl();
    ann.epoch              = (int)ctx_.current_epoch_;

    // ECDSA P-256 self-signed claim.
    if (ctx_.ec_private_key_) {
        std::string toSign = ann.carId + ":" + ann.laneId + ":" +
                             std::to_string(ann.positionInLane) + ":" +
                             std::to_string(ann.claimedArrivalTime) + ":" +
                             std::to_string(ann.epoch);
        uint8_t sigOut[CRYPTO_SIG_MAX_BYTES]; uint8_t sigLen = 0;
        if (CryptoAuth::instance().signBytes(ctx_.ec_private_key_,
                reinterpret_cast<const uint8_t*>(toSign.c_str()), toSign.size(),
                sigOut, sigLen))
            ann.signature.assign(sigOut, sigOut + sigLen);
    }

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
        arrival_announcements_received_.insert(myCarId);
        const bool newIntent = observed_intent_cars_.insert(myCarId).second;
        if (newIntent) noteDiscoveryIntent(myCarId, "self-announce");
    }

    if (is_byzantine_ && byzantine_type_ == BYZANTINE_EQUIVOCATOR) {
        int n = ctx_.total_vehicles_;
        for (int peerId = 0; peerId < n; ++peerId) {
            if (peerId == ctx_.replicaId_) continue;
            ArrivalAnnouncement annByz = ann;
            annByz.direction = (peerId < n / 2) ? DIR_LEFT : DIR_RIGHT;
            std::cout << "[BYZANTINE] r" << ctx_.replicaId_ << " EQUIVOCATOR: peer "
                      << peerId << " dir=" << (peerId < n/2 ? "L" : "R") << "\n";
            sendBFTMessage(peerId, serializeArrivalAnnouncement(annByz), kArrivalAnnounceType);
        }
    } else {
        std::vector<uint8_t> payload = serializeArrivalAnnouncement(ann);
        sendBFTMessage(-1, payload, kArrivalAnnounceType);
        std::cout << "[ANN-BROADCAST] Replica " << ctx_.replicaId_ << " broadcast ARRIVAL_ANNOUNCE at t="
                  << simTime() << " lane=" << ann.lane << "\n";
    }

    // Radio broadcast has no self-delivery. A coordinated FALSE_LANE claimant
    // is nevertheless one of the F Byzantine witnesses, so insert its own
    // authenticated echo through the same collector used for received echoes.
    if (shouldColludeOnFalseLane(ann)) sendArrivalEcho(ann);
}

// ── handleArrivalAnnouncement ─────────────────────────────────────────────────

void ResDBIntersectionApp::handleArrivalAnnouncement(BFTMessage* msg)
{
    handleArrivalAnnouncement(msg, false, -1);
}

void ResDBIntersectionApp::gossipArrivalAnnouncement(const ArrivalAnnouncement& ann,
                                                     const std::vector<uint8_t>& announceBytes)
{
    if (!ctx_.gossip_enabled_ || announceBytes.empty() || ann.carId.empty()) return;
    // A late ambulance excluded from the committed order is an emergency that must keep
    // propagating post-commit: replicas out of its radio range (notably the static units)
    // otherwise never witness it, so the CANCEL can't reach f+1 echoes. This mirrors the
    // in-flight-consensus case where gossip already floods the announcement to everyone.
    const bool ambulanceEmergency = ctx_.enableRollback_ && ann.isAmbulance &&
        ctx_.has_committed_order_ && !isEpochTombstoned((uint32_t)ann.epoch) &&
        !ctx_.committed_order_vehicle_ids_.count(extractReplicaId(ann.carId));
    if ((ctx_.propose_submitted_ || ctx_.order_applied_) && !ambulanceEmergency) return;
    if (!announcement_relay_tracker_.tryRelay((uint32_t)ann.epoch, ann.carId)) return;

    sendArrivalAnnouncementGossipPayload(
        ann.carId, (uint32_t)ann.epoch, announceBytes,
        ambulanceEmergency ? "ambulance-emergency" : "verified", ambulanceEmergency);
}

void ResDBIntersectionApp::sendArrivalAnnouncementGossipPayload(
    const std::string& carId,
    uint32_t epoch,
    const std::vector<uint8_t>& announceBytes,
    const char* reason,
    bool forceEmergency)
{
    if (!ctx_.gossip_enabled_ || announceBytes.empty() || carId.empty()) return;
    if ((ctx_.propose_submitted_ || ctx_.order_applied_) && !forceEmergency) return;

    auto inner = resdb_gossip::serializeAnnouncement(epoch, announceBytes);
    auto signedPayload = resdbwire::packSignedPacket(
        ctx_.ec_private_key_, ctx_.ec_pub_key_, inner.data(), (uint32_t)inner.size());
    if (signedPayload.empty()) return;

    sendBFTMessage(-1, signedPayload, kArrivalAnnounceGossipType);
    std::cout << "[ANN-GOSSIP-SEND] r" << ctx_.replicaId_
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
        std::cout << "[ANN-GOSSIP-RECV] r" << ctx_.replicaId_
                  << " dropped forged announce gossip from r"
                  << msg->getFromReplicaId() << "\n";
        return;
    }

    uint32_t epoch = 0;
    std::vector<uint8_t> announceBytes;
    if (!resdb_gossip::parseAnnouncement(view.resdbBytes, view.resdbLen,
                                         epoch, announceBytes)) return;

    BFTMessage* announceMsg = makeArrivalAnnouncementMessage(
        announceBytes, msg->getFromReplicaId(), ctx_.replicaId_);
    // Peek at the announcement: a late ambulance excluded from the committed order must
    // bypass the post-commit suppression below, otherwise its arrival for the (already
    // committed) epoch is dropped and out-of-range replicas never witness it to CANCEL.
    ArrivalAnnouncement peek = deserializeArrivalAnnouncement(announceMsg);
    const bool ambulanceEmergency = ctx_.enableRollback_ && peek.isAmbulance &&
        ctx_.has_committed_order_ && !isEpochTombstoned(epoch) &&
        !ctx_.committed_order_vehicle_ids_.count(extractReplicaId(peek.carId));
    if (!ambulanceEmergency) {
        if (ctx_.has_committed_order_ && epoch <= ctx_.last_committed_epoch_) { delete announceMsg; return; }
        if ((int)epoch < (int)ctx_.current_epoch_) { delete announceMsg; return; }
    }
    handleArrivalAnnouncement(announceMsg, true, msg->getFromReplicaId());
    delete announceMsg;
}

void ResDBIntersectionApp::handleArrivalAnnouncement(BFTMessage* msg,
                                                     bool viaGossip,
                                                     int carrierReplicaId)
{
    std::vector<uint8_t> announceBytes = payloadBytes(msg);
    ArrivalAnnouncement ann = deserializeArrivalAnnouncement(msg);
    if (ann.carId.empty()) return;
    std::cout << "[ANN-RECV] Replica " << ctx_.replicaId_ << " received ARRIVAL_ANNOUNCE from "
              << ann.carId << " frameFrom=" << msg->getFromReplicaId()
              << " via=" << (viaGossip ? "gossip" : "direct");
    if (viaGossip) std::cout << " carrier=" << carrierReplicaId;
    std::cout << " at t=" << simTime() << "\n";

    // Dedup: only re-echo if we VERIFIED and echoed this car before (not FALSE_LANE).
    // echoed_cars_ is populated only after verifyCarPosition passes, so FALSE_LANE
    // cars stored in local_vehicle_states_ are never re-echoed here.
    if (local_vehicle_states_.count(ann.carId)) {
        // Re-echo if we verified this car before but haven't received its cert yet.
        // Use !ctx_.collected_certs_.count(ann.carId), NOT !cert_broadcast_: the latter
        // is this replica's own flag and would incorrectly suppress re-echoes after
        // this replica assembles its own cert.
        if (echoed_cars_.count(ann.carId)
                && !ctx_.collected_certs_.count(ann.carId)
                && !ctx_.propose_submitted_ && !ctx_.order_applied_) {
            pending_relays_[std::make_pair((uint32_t)ann.epoch, ann.carId)] = {
                ann.carId, (uint32_t)ann.epoch, announceBytes, simTime().dbl(), 0};
            sendArrivalEcho(ann);
        }
        return;
    }

    const bool colludingFalseLane = shouldColludeOnFalseLane(ann);
    // D3 disables only physical endorsement. Coordinated Type-1 replicas also
    // deliberately endorse the exact shared forged value; every other claim
    // still follows the normal physical gate.
    VerificationResult result;
    if (!enable_arrival_position_gate_ || colludingFalseLane) {
        result = {true,
                  colludingFalseLane ? "BYZANTINE_COLLUSION" : "POSITION_GATE_DISABLED",
                  ann.laneId,
                  static_cast<double>(ann.positionInLane)};
    } else {
        result = verifyCarPosition(ann.carId, ann.laneId, ann.positionInLane, 1e9);
    }

    if (!result.isValid) {
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
        arrival_announcements_received_.insert(ann.carId);
        const bool newIntent = observed_intent_cars_.insert(ann.carId).second;
        if (newIntent) noteDiscoveryIntent(ann.carId, "invalid-lane-announce");
        std::cout << "[ANN-RECV] Replica " << ctx_.replicaId_ << " FALSE_LANE from "
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
            std::cerr << "[ANN-RECV] r" << ctx_.replicaId_
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
                std::cerr << "[ANN-RECV] r" << ctx_.replicaId_
                          << " DOWNGRADE: ambulance sig invalid for " << ann.carId << "\n";
                effectiveIsAmbulance = false;
            }
        } else {
            effectiveIsAmbulance = false;
        }
    } else if (ann.isAmbulance && !ann.ambulanceCertBytes.empty()) {
        std::cerr << "[ANN-RECV] r" << ctx_.replicaId_
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
        std::cout << "[CERT-GATE] r" << ctx_.replicaId_
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
    arrival_announcements_received_.insert(ann.carId);
    const bool newIntent = observed_intent_cars_.insert(ann.carId).second;
    echoed_cars_.insert(ann.carId);  // record that we actually echoed this car
    pending_relays_[std::make_pair((uint32_t)ann.epoch, ann.carId)] = {
        ann.carId, (uint32_t)ann.epoch, announceBytes, simTime().dbl(), 0};

    maybeTriggerEmergencyRollbackFromAnnouncement(ann);
    sendArrivalEcho(ann);
    gossipArrivalAnnouncement(ann, announceBytes);

    std::cout << "[ANN-RECV] Replica " << ctx_.replicaId_ << " stored VehicleState for "
              << ann.carId << " via=" << (viaGossip ? "gossip" : "direct")
              << " (" << observed_intent_cars_.size()
              << "/" << ctx_.total_vehicles_ << " observed)\n";
    if (newIntent) noteDiscoveryIntent(ann.carId, viaGossip ? "announce-gossip" : "announce-direct");
    if (ctx_.discovery_.state != DiscoveryState::INACTIVE)
        ctx_.current_phase_ = ConsensusPhase::COLLECTING_CERTS;
}

// ── sendArrivalEcho ───────────────────────────────────────────────────────────

void ResDBIntersectionApp::sendArrivalEcho(const ArrivalAnnouncement& ann)
{
    if (!ctx_.ec_private_key_) return;
    const bool collusionEcho = shouldColludeOnFalseLane(ann);
    // Do not dedup/suppress radio (re)transmission here: repeated/gossiped
    // forged announcements must be able to trigger a retransmit of this same
    // authenticated echo through the existing discovery retry behavior
    // (handleArrivalAnnouncement's re-echo path), otherwise one lost packet
    // permanently drops this colluder's signature from the certificate.
    // collectArrivalEcho() already dedupes by echoingReplicaId on the
    // claimant's side, so retransmission cannot inflate the distinct signer
    // count.
    std::string toSign = ann.carId + ":" + ann.lane + ":" +
        std::to_string(ann.positionInLane) + ":" +
        (ann.direction == DIR_LEFT ? "L" : ann.direction == DIR_RIGHT ? "R" : "S") +
        ":" + (ann.isAmbulance ? "1" : "0") +
        ":" + std::to_string(ctx_.replicaId_);

    ArrivalEcho echo;
    echo.echoingReplicaId = ctx_.replicaId_;
    echo.targetCarId      = ann.carId;
    echo.lane             = ann.lane;
    echo.positionInLane   = ann.positionInLane;
    echo.direction        = ann.direction;
    echo.isAmbulance      = ann.isAmbulance;
    echo.epoch            = ann.epoch;
    std::memcpy(echo.signerPubKey, ctx_.ec_pub_key_, CRYPTO_PUBKEY_BYTES);
    std::memset(echo.signature, 0, CRYPTO_SIG_MAX_BYTES);
    echo.signatureLen = 0;

    if (!CryptoAuth::instance().signBytes(ctx_.ec_private_key_,
            reinterpret_cast<const uint8_t*>(toSign.c_str()), toSign.size(),
            echo.signature, echo.signatureLen)) {
        std::cerr << "[ECHO-SEND] Replica " << ctx_.replicaId_ << " sign failed for echo of "
                  << ann.carId << "\n";
        return;
    }

    if (is_byzantine_ && byzantine_type_ == BYZANTINE_INVALID_SIG) {
        echo.signature[0] = echo.signature[1] =
        echo.signature[2] = echo.signature[3] = 0xDE;
        echo.signatureLen = 4;
        std::cout << "[BYZANTINE] r" << ctx_.replicaId_ << " INVALID_SIG: corrupted echo for "
                  << ann.carId << "\n";
    }

    int targetId = extractReplicaId(ann.carId);
    sendBFTMessage(-1, serializeArrivalEcho(echo), kArrivalEchoType,
                   false, true);
    std::cout << "[ECHO-SEND] Replica " << ctx_.replicaId_ << " → " << ann.carId
              << " ARRIVAL_ECHO sigLen=" << (int)echo.signatureLen << "\n";
    if (collusionEcho) {
        std::cout << "[FALSE-LANE-COLLUSION-ECHO] target=" << ann.carId
                  << " signer=" << ctx_.replicaId_
                  << " epoch=" << ann.epoch
                  << " source=" << (extractReplicaId(ann.carId) == ctx_.replicaId_ ? "self" : "peer")
                  << "\n";
        if (extractReplicaId(ann.carId) == ctx_.replicaId_) {
            collectArrivalEcho(echo, "collusion-self");
        }
    }
    if (debug_cert_protocol_) {
        std::cout << "[CERT-DEBUG] sendArrivalEcho r" << ctx_.replicaId_ << " targetReplicaId=-1"
                  << " signPayload=\"" << toSign << "\"\n";
    }
}

bool ResDBIntersectionApp::isExactFalseLaneClaim(const ArrivalAnnouncement& ann) const
{
    const int target = extractReplicaId(ann.carId);
    return target >= 0 && target < ctx_.total_vehicles_ &&
        ann.epoch == static_cast<int>(ctx_.current_epoch_) &&
        ann.laneId == "BYZANTINE_FAKE_LANE" && ann.lane == "X";
}

bool ResDBIntersectionApp::shouldColludeOnFalseLane(const ArrivalAnnouncement& ann) const
{
    if (!is_byzantine_ || byzantine_type_ != BYZANTINE_FALSE_LANE) return false;
    const int target = extractReplicaId(ann.carId);
    return isExactFalseLaneClaim(ann) &&
        false_lane_colluder_ids_.count(ctx_.replicaId_) > 0 &&
        false_lane_colluder_ids_.count(target) > 0;
}

bool ResDBIntersectionApp::isArrivalSignerEligible(int signerId) const
{
    // ARRIVAL certificates establish the next ORDER electorate, so late
    // configured replicas may witness before that ORDER is committed. The
    // configured replica range is therefore the correct active discovery set.
    return signerId >= 0 && signerId < ctx_.total_vehicles_;
}

void ResDBIntersectionApp::collectArrivalEcho(const ArrivalEcho& echo, const char* source)
{
    const std::string myCarId = "veh" + std::to_string(ctx_.replicaId_);
    if (echo.targetCarId != myCarId || cert_broadcast_) return;
    if (!isArrivalSignerEligible(echo.echoingReplicaId) || echo.signatureLen == 0) return;
    if (!WitnessKeyRegistry::instance().matches(echo.echoingReplicaId, echo.signerPubKey)) return;

    auto stateIt = local_vehicle_states_.find(myCarId);
    if (stateIt == local_vehicle_states_.end()) return;
    const VehicleState& state = stateIt->second;
    if (echo.epoch != static_cast<int>(ctx_.current_epoch_) ||
            echo.lane != state.lane || echo.positionInLane != state.positionInLane ||
            echo.direction != state.direction || echo.isAmbulance != state.isAmbulance) {
        return;
    }
    const std::string toSign = echo.targetCarId + ":" + echo.lane + ":" +
        std::to_string(echo.positionInLane) + ":" +
        (echo.direction == DIR_LEFT ? "L" : echo.direction == DIR_RIGHT ? "R" : "S") +
        ":" + (echo.isAmbulance ? "1" : "0") + ":" +
        std::to_string(echo.echoingReplicaId);
    if (!CryptoAuth::instance().verifyBytes(
            echo.signerPubKey,
            reinterpret_cast<const uint8_t*>(toSign.c_str()), toSign.size(),
            echo.signature, echo.signatureLen)) {
        return;
    }

    auto& echoes = my_received_echoes_[myCarId];
    for (const auto& existing : echoes)
        if (existing.echoingReplicaId == echo.echoingReplicaId) return;
    echoes.push_back(echo);

    const int f = ctx_.tolerated_faults_ >= 0 ? ctx_.tolerated_faults_ : (ctx_.total_vehicles_ - 1) / 3;
    const int required = f + 1;
    std::cout << "[ECHO-RECV] Replica " << ctx_.replicaId_ << " received echo from "
              << echo.echoingReplicaId << " (" << echoes.size() << " so far)\n";
    if (state.lane == "X") {
        std::cout << "[FALSE-LANE-COLLUSION-ECHO] target=" << myCarId
                  << " signer=" << echo.echoingReplicaId
                  << " count=" << echoes.size() << "/" << required
                  << " source=" << (source ? source : "unknown") << "\n";
    }
    if (static_cast<int>(echoes.size()) < required) return;

    cert_broadcast_ = true;
    ArrivalCert cert;
    cert.carId = myCarId;
    cert.lane = state.lane;
    cert.positionInLane = state.positionInLane;
    cert.direction = state.direction;
    cert.isAmbulance = state.isAmbulance;
    cert.epoch = static_cast<int>(ctx_.current_epoch_);
    cert.echoes = echoes;
    if (state.arrival_time_us > 0) {
        const double announceTimeSec = static_cast<double>(state.arrival_time_us) / 1000000.0;
        const double latencySec = simTime().dbl() - announceTimeSec;
        std::cout << "[METRICS " << ctx_.replicaId_ << "] Cert_Created_Time: " << simTime()
                  << " car=" << myCarId << " announce_time=" << announceTimeSec
                  << " latency=" << latencySec << "s\n";
        std::cout << "[METRICS " << ctx_.replicaId_
                  << "] Cert_Creation_Latency: " << latencySec << "s\n";
    }
    std::cout << "[CERT-ASSEMBLE] Replica " << ctx_.replicaId_
              << " assembled ARRIVAL_CERT with " << cert.echoes.size()
              << " echoes — broadcasting\n";
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
}

// ── handleArrivalEcho ─────────────────────────────────────────────────────────

void ResDBIntersectionApp::handleArrivalEcho(BFTMessage* msg)
{
    ArrivalEcho echo = deserializeArrivalEcho(msg);
    if (debug_cert_protocol_)
        std::cout << "[CERT-DEBUG] handleArrivalEcho r" << ctx_.replicaId_
                  << " frameFrom=r" << msg->getFromReplicaId()
                  << " echo.target=" << echo.targetCarId
                  << " echo.signer=r" << echo.echoingReplicaId << "\n";
    std::string myCarId = "veh" + std::to_string(ctx_.replicaId_);
    if (echo.targetCarId != myCarId) {
        if (debug_cert_protocol_)
            std::cout << "[CERT-DEBUG] handleArrivalEcho r" << ctx_.replicaId_
                      << " wrong target: echo for " << echo.targetCarId
                      << " (my " << myCarId << ") from r" << echo.echoingReplicaId << "\n";
        return;
    }
    if (cert_broadcast_) {
        if (debug_cert_protocol_)
            std::cout << "[CERT-DEBUG] handleArrivalEcho r" << ctx_.replicaId_
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
    if (ctx_.propose_submitted_ || ctx_.order_applied_) {
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
        certs.reserve(ctx_.collected_certs_.size());
        for (const auto& kv : ctx_.collected_certs_) {
            certs.push_back(kv.second);
        }
    }
    if (certs.empty()) return;

    for (const auto& cert : certs) {
        sendBFTMessage(-1, serializeArrivalCert(cert), kArrivalCertType);
    }
    std::cout << "[CERT-GOSSIP] r" << ctx_.replicaId_
              << " broadcast collected certs count=" << certs.size()
              << " reason=" << (reason ? reason : "stop-zone")
              << " t=" << simTime() << "\n";
}

void ResDBIntersectionApp::startStopZoneCertGossip(const char* reason, bool immediate)
{
    if (ctx_.discovery_.state != DiscoveryState::COLLECTING)
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
    if (ctx_.discovery_.state != DiscoveryState::COLLECTING)
        return;
    if (CertPrimary() != ctx_.replicaId_)
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
    ctx_.discovery_.localCert = LocalCertState::QUEUED;
    sendBFTMessage(-1, serializeArrivalCert(cert), kArrivalCertType, true);
    std::cout << "[CERT-BROADCAST] Replica " << ctx_.replicaId_ << " broadcast ARRIVAL_CERT for "
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
    if (!ctx_.collected_certs_.count(cert.carId)) {
        int staticCerts = 0;
        int allCerts = 0;
        std::lock_guard<std::mutex> lk(certs_mutex_);
        ctx_.collected_certs_[cert.carId] = cert;
        staticCerts = countStaticCollectedCerts();
        allCerts = (int)ctx_.collected_certs_.size();
        certPrimary = CertPrimary();
        std::cout << "[CERT-STORED-SELF] Replica " << ctx_.replicaId_ << " self-stored cert for "
                  << cert.carId << " static=(" << staticCerts << "/" << ctx_.total_vehicles_
                  << ") all=" << allCerts
                  << " cert_primary=" << certPrimary << "\n";
        const bool emergencyCancelStarted = maybeTriggerEmergencyRollbackFromCert(cert);
        (void)emergencyCancelStarted;
    }
    if (entered_stop_zone_ && ctx_.discovery_.state == DiscoveryState::COLLECTING) {
        startStopZoneCertGossip("self-cert-stored", ctx_.replicaId_ == certPrimary);
    }
    maybeAdvanceDiscovery("self-cert");
}

// ── handleArrivalCert ─────────────────────────────────────────────────────────

void ResDBIntersectionApp::handleArrivalCert(BFTMessage* msg)
{
    ArrivalCert cert = deserializeArrivalCert(msg);
    if (cert.carId.empty()) return;
    if (!validateArrivalCert(cert)) {
        std::cout << "[CERT-INVALID] Replica " << ctx_.replicaId_ << " dropped ARRIVAL_CERT from "
                  << cert.carId << "\n";
        return;
    }
    if (ctx_.collected_certs_.count(cert.carId)) return;  // dedup

    {
        std::lock_guard<std::mutex> lk(certs_mutex_);
        ctx_.collected_certs_[cert.carId] = cert;
    }
    int staticCerts = 0;
    int allCerts = 0;
    int certPrimary = -1;
    {
        std::lock_guard<std::mutex> lk(certs_mutex_);
        staticCerts = countStaticCollectedCerts();
        allCerts = (int)ctx_.collected_certs_.size();
        certPrimary = CertPrimary();
    }
    std::cout << "[CERT-STORED] Replica " << ctx_.replicaId_ << " stored ARRIVAL_CERT for "
              << cert.carId << " static=(" << staticCerts << "/" << ctx_.total_vehicles_
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
        if (newIntent) noteDiscoveryIntent(cert.carId, "certificate");
    }

    // Epidemic relay: each node forwards each validated cert once.
    // cert already carries f+1 signatures so recipients can verify without
    // re-accumulating votes.  cert_relay_tracker_ deduplicates per carId.
    if (ctx_.discovery_.state != DiscoveryState::COLLECTING) {
        std::cout << "[CERT-RELAY-STOP] r" << ctx_.replicaId_ << " suppressed relay for "
                  << cert.carId << " order_applied=" << (ctx_.order_applied_ ? 1 : 0)
                  << " propose_submitted=" << (ctx_.propose_submitted_ ? 1 : 0)
                  << " discovery_state=" << discoveryStateName()
                  << " t=" << simTime() << "\n";
    } else if (cert_relay_tracker_.tryRelay(cert.carId)) {
        sendBFTMessage(-1, serializeArrivalCert(cert), kArrivalCertType);
        std::cout << "[CERT-RELAY] r" << ctx_.replicaId_ << " relayed cert for "
                  << cert.carId << " t=" << simTime() << "\n";
    }
    if (entered_stop_zone_ && ctx_.discovery_.state == DiscoveryState::COLLECTING) {
        startStopZoneCertGossip("cert-stored", ctx_.replicaId_ == certPrimary);
    }

    const bool emergencyCancelStarted = maybeTriggerEmergencyRollbackFromCert(cert);

    (void)emergencyCancelStarted;
    maybeAdvanceDiscovery("cert-stored");
}

// ── validateArrivalCert ───────────────────────────────────────────────────────

bool ResDBIntersectionApp::validateArrivalCert(const ArrivalCert& cert)
{
    int f = toleratedF();
    int required = f + 1;
    if ((int)cert.echoes.size() < required) {
        if (debug_cert_protocol_)
            std::cout << "[CERT-DEBUG] validateArrivalCert fail: carId=" << cert.carId
                      << " echo_count=" << cert.echoes.size() << " need>=" << required << "\n";
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
                std::cout << "[CERT-DEBUG] validateArrivalCert skip duplicate signer r"
                          << echo.echoingReplicaId << " carId=" << cert.carId << "\n";
            continue;
        }
        if (echo.signatureLen == 0) {
            if (debug_cert_protocol_)
                std::cout << "[CERT-DEBUG] validateArrivalCert skip empty sig r"
                          << echo.echoingReplicaId << " carId=" << cert.carId << "\n";
            continue;
        }
        if (echo.targetCarId != cert.carId || echo.epoch != cert.epoch ||
                echo.lane != cert.lane || echo.positionInLane != cert.positionInLane ||
                echo.direction != cert.direction || echo.isAmbulance != cert.isAmbulance) {
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
        std::string dir = (cert.direction == DIR_LEFT ? "L" :
                           cert.direction == DIR_RIGHT ? "R" : "S");
        std::string toSign = cert.carId + ":" + cert.lane + ":" +
            std::to_string(cert.positionInLane) + ":" + dir + ":" +
            (cert.isAmbulance ? "1" : "0") + ":" +
            std::to_string(echo.echoingReplicaId);
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
    if (valid >= required)
        return true;
    if (debug_cert_protocol_)
        std::cout << "[CERT-DEBUG] validateArrivalCert fail: carId=" << cert.carId
                  << " valid_sigs=" << valid << " need=" << required << "\n";
    return false;
}

#include "veins/modules/application/resDB/ResDBIntersectionApp.h"
#include "veins/modules/application/resDB/ResDBUtil.h"
#include "veins/modules/application/resDB/messages/BFTMessage_m.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>

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

std::string cleanRef(std::string s)
{
    std::replace(s.begin(), s.end(), '|', '_');
    return s;
}

} // namespace

std::string ResDBIntersectionApp::cancelReasonKey(
    uint32_t epoch, CancelReason reason, const std::string& reasonRef) const
{
    return std::to_string(epoch) + ":" + std::to_string(static_cast<int>(reason)) +
        ":" + reasonRef;
}

std::string ResDBIntersectionApp::cancelSignPayload(
    uint32_t cancelledEpoch, CancelReason reason, const std::string& reasonRef,
    int echoingReplicaId) const
{
    return std::to_string(cancelledEpoch) + ":" +
        std::to_string(static_cast<int>(reason)) + ":" +
        reasonRef + ":" + std::to_string(echoingReplicaId);
}

std::vector<uint8_t> ResDBIntersectionApp::serializeCancelEcho(const CancelEcho& echo) const
{
    std::vector<uint8_t> pubVec(echo.signerPubKey,
                                echo.signerPubKey + CRYPTO_PUBKEY_BYTES);
    std::vector<uint8_t> sigVec(echo.signature,
                                echo.signature + echo.signatureLen);
    std::stringstream ss;
    ss << "CANCEL_ECHO|"
       << echo.cancelledEpoch << "|"
       << static_cast<int>(echo.reason) << "|"
       << cleanRef(echo.reasonRef) << "|"
       << echo.echoingReplicaId << "|"
       << toHex(pubVec) << "," << toHex(sigVec);
    std::string s = ss.str();
    return std::vector<uint8_t>(s.begin(), s.end());
}

ResDBIntersectionApp::CancelEcho
ResDBIntersectionApp::deserializeCancelEcho(BFTMessage* msg) const
{
    std::vector<uint8_t> payload = payloadBytes(msg);
    std::string s(payload.begin(), payload.end());
    auto parts = splitStr(s, '|');
    CancelEcho echo;
    std::memset(echo.signerPubKey, 0, CRYPTO_PUBKEY_BYTES);
    std::memset(echo.signature, 0, CRYPTO_SIG_MAX_BYTES);
    if (parts.size() >= 6 && parts[0] == "CANCEL_ECHO") {
        echo.cancelledEpoch = static_cast<uint32_t>(std::stoul(parts[1]));
        echo.reason = static_cast<CancelReason>(std::stoi(parts[2]));
        echo.reasonRef = parts[3];
        echo.echoingReplicaId = std::stoi(parts[4]);
        size_t comma = parts[5].find(',');
        if (comma != std::string::npos) {
            auto pubVec = fromHex(parts[5].substr(0, comma));
            auto sigVec = fromHex(parts[5].substr(comma + 1));
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

std::vector<uint8_t> ResDBIntersectionApp::serializeCancelCert(const CancelCert& cert) const
{
    std::stringstream ss;
    ss << "CANCEL_CERT|"
       << cert.cancelledEpoch << "|"
       << static_cast<int>(cert.reason) << "|"
       << cleanRef(cert.reasonRef);
    for (const auto& echo : cert.echoes) {
        std::vector<uint8_t> pubVec(echo.signerPubKey,
                                    echo.signerPubKey + CRYPTO_PUBKEY_BYTES);
        std::vector<uint8_t> sigVec(echo.signature,
                                    echo.signature + echo.signatureLen);
        ss << "|" << echo.echoingReplicaId << ":"
           << toHex(pubVec) << "," << toHex(sigVec);
    }
    std::string s = ss.str();
    return std::vector<uint8_t>(s.begin(), s.end());
}

ResDBIntersectionApp::CancelCert
ResDBIntersectionApp::deserializeCancelCert(BFTMessage* msg) const
{
    std::vector<uint8_t> payload = payloadBytes(msg);
    std::string s(payload.begin(), payload.end());
    auto parts = splitStr(s, '|');
    CancelCert cert;
    if (parts.size() < 4 || parts[0] != "CANCEL_CERT") return cert;
    cert.cancelledEpoch = static_cast<uint32_t>(std::stoul(parts[1]));
    cert.reason = static_cast<CancelReason>(std::stoi(parts[2]));
    cert.reasonRef = parts[3];
    for (size_t i = 4; i < parts.size(); ++i) {
        size_t colon = parts[i].find(':');
        if (colon == std::string::npos) continue;
        size_t comma = parts[i].find(',', colon + 1);
        if (comma == std::string::npos) continue;
        CancelEcho echo;
        echo.cancelledEpoch = cert.cancelledEpoch;
        echo.reason = cert.reason;
        echo.reasonRef = cert.reasonRef;
        echo.echoingReplicaId = std::stoi(parts[i].substr(0, colon));
        auto pubVec = fromHex(parts[i].substr(colon + 1, comma - colon - 1));
        auto sigVec = fromHex(parts[i].substr(comma + 1));
        if (pubVec.size() == CRYPTO_PUBKEY_BYTES)
            std::memcpy(echo.signerPubKey, pubVec.data(), CRYPTO_PUBKEY_BYTES);
        if (sigVec.size() <= CRYPTO_SIG_MAX_BYTES) {
            std::memcpy(echo.signature, sigVec.data(), sigVec.size());
            echo.signatureLen = static_cast<uint8_t>(sigVec.size());
        }
        cert.echoes.push_back(echo);
    }
    return cert;
}

bool ResDBIntersectionApp::validateCancelCert(const CancelCert& cert) const
{
    if (cert.reason != CANCEL_CRASH && cert.reason != CANCEL_EMERGENCY) return false;
    int f = (total_vehicles_ - 1) / 3;
    int required = f + 1;
    if ((int)cert.echoes.size() < required) return false;

    std::set<int> seen;
    int valid = 0;
    for (const auto& echo : cert.echoes) {
        if (echo.cancelledEpoch != cert.cancelledEpoch ||
                echo.reason != cert.reason ||
                echo.reasonRef != cert.reasonRef ||
                echo.signatureLen == 0) {
            continue;
        }
        if (!seen.insert(echo.echoingReplicaId).second) continue;
        std::string toSign = cancelSignPayload(
            cert.cancelledEpoch, cert.reason, cert.reasonRef, echo.echoingReplicaId);
        if (CryptoAuth::instance().verifyBytes(
                echo.signerPubKey,
                reinterpret_cast<const uint8_t*>(toSign.c_str()), toSign.size(),
                echo.signature, echo.signatureLen)) {
            valid++;
        }
    }
    return valid >= required;
}

void ResDBIntersectionApp::sendCancelEcho(
    uint32_t cancelledEpoch, CancelReason reason, const std::string& reasonRef)
{
    if (!enableRollback_ || !ec_private_key_) return;
    std::string key = cancelReasonKey(cancelledEpoch, reason, cleanRef(reasonRef));
    if (!cancel_echo_sent_.insert(key).second) return;

    CancelEcho echo;
    echo.echoingReplicaId = replicaId_;
    echo.cancelledEpoch = cancelledEpoch;
    echo.reason = reason;
    echo.reasonRef = cleanRef(reasonRef);
    std::memcpy(echo.signerPubKey, ec_pub_key_, CRYPTO_PUBKEY_BYTES);

    std::string toSign = cancelSignPayload(
        echo.cancelledEpoch, echo.reason, echo.reasonRef, echo.echoingReplicaId);
    if (!CryptoAuth::instance().signBytes(
            ec_private_key_, reinterpret_cast<const uint8_t*>(toSign.c_str()),
            toSign.size(), echo.signature, echo.signatureLen)) {
        return;
    }

    sendBFTMessage(-1, serializeCancelEcho(echo), kCancelEchoType);
    std::cout << "[CANCEL-ECHO] r" << replicaId_
              << " epoch=" << cancelledEpoch
              << " reason=" << static_cast<int>(reason)
              << " ref=" << echo.reasonRef << "\n";

    cancel_echoes_[key].push_back(echo);
}

void ResDBIntersectionApp::handleCancelEcho(BFTMessage* msg)
{
    if (!enableRollback_) return;
    CancelEcho echo = deserializeCancelEcho(msg);
    if (echo.echoingReplicaId < 0 || echo.signatureLen == 0) return;
    if (echo.reason != CANCEL_CRASH && echo.reason != CANCEL_EMERGENCY) return;

    std::string toSign = cancelSignPayload(
        echo.cancelledEpoch, echo.reason, echo.reasonRef, echo.echoingReplicaId);
    if (!CryptoAuth::instance().verifyBytes(
            echo.signerPubKey,
            reinterpret_cast<const uint8_t*>(toSign.c_str()), toSign.size(),
            echo.signature, echo.signatureLen)) {
        std::cout << "[CANCEL-ECHO] r" << replicaId_
                  << " dropped invalid echo from r" << echo.echoingReplicaId << "\n";
        return;
    }

    std::string key = cancelReasonKey(echo.cancelledEpoch, echo.reason, echo.reasonRef);
    auto& echoes = cancel_echoes_[key];
    for (const auto& existing : echoes) {
        if (existing.echoingReplicaId == echo.echoingReplicaId) return;
    }
    echoes.push_back(echo);
    int threshold = ((total_vehicles_ - 1) / 3) + 1;
    std::cout << "[CANCEL-ECHO] r" << replicaId_
              << " recv signer=r" << echo.echoingReplicaId
              << " key=" << key
              << " count=" << echoes.size() << "/" << threshold << "\n";

    if ((int)echoes.size() >= threshold && !cancel_cert_seen_.count(key)) {
        CancelCert cert;
        cert.cancelledEpoch = echo.cancelledEpoch;
        cert.reason = echo.reason;
        cert.reasonRef = echo.reasonRef;
        cert.echoes = echoes;
        broadcastCancelCert(cert);
    }
}

void ResDBIntersectionApp::scheduleNextCancelCertRetry()
{
    if (!enableRollback_ || cancel_cert_pending_retries_.echoes.empty()) return;
    if (!cancel_cert_retry_timer_)
        cancel_cert_retry_timer_ = new cMessage("resdbCancelCertRetry");
    scheduleAt(simTime() + cancel_cert_retry_interval_sec_, cancel_cert_retry_timer_);
}

void ResDBIntersectionApp::stopCancelCertRetries()
{
    if (cancel_cert_retry_timer_) {
        if (cancel_cert_retry_timer_->isScheduled()) cancelEvent(cancel_cert_retry_timer_);
        delete cancel_cert_retry_timer_;
        cancel_cert_retry_timer_ = nullptr;
    }
    cancel_cert_pending_retries_.echoes.clear();
    cancel_cert_retry_count_ = 0;
}

void ResDBIntersectionApp::broadcastCancelCert(const CancelCert& cert)
{
    if (!validateCancelCert(cert)) return;
    std::string key = cancelReasonKey(cert.cancelledEpoch, cert.reason, cert.reasonRef);
    cancel_cert_seen_.insert(key);
    std::vector<uint8_t> payload = serializeCancelCert(cert);
    sendBFTMessage(-1, payload, kCancelCertType);
    std::cout << "[CANCEL-CERT] r" << replicaId_
              << " broadcast key=" << key
              << " echoes=" << cert.echoes.size() << "\n";
    cancel_cert_pending_retries_ = cert;
    cancel_cert_retry_count_ = 0;
    scheduleNextCancelCertRetry();
    handleValidCancelJustification(cert.cancelledEpoch, cert.reason, cert.reasonRef, payload);
}

void ResDBIntersectionApp::handleCancelCert(BFTMessage* msg)
{
    if (!enableRollback_) return;
    CancelCert cert = deserializeCancelCert(msg);
    if (cert.reasonRef.empty()) return;
    if (!validateCancelCert(cert)) {
        std::cout << "[CANCEL-CERT] r" << replicaId_
                  << " dropped invalid cert from r" << msg->getFromReplicaId() << "\n";
        return;
    }

    std::string key = cancelReasonKey(cert.cancelledEpoch, cert.reason, cert.reasonRef);
    std::vector<uint8_t> payload = serializeCancelCert(cert);
    const bool firstSeen = cancel_cert_seen_.insert(key).second;
    if (cancel_cert_relayed_.insert(key).second) {
        sendBFTMessage(-1, payload, kCancelCertType);
        std::cout << "[CANCEL-RELAY] r" << replicaId_
                  << " key=" << key << " from=r" << msg->getFromReplicaId() << "\n";
    }
    if (firstSeen || !cancel_pending_)
        handleValidCancelJustification(cert.cancelledEpoch, cert.reason, cert.reasonRef, payload);
}

bool ResDBIntersectionApp::isRecallable()
{
    if (current_phase_ == ConsensusPhase::DEPARTED || is_departed_) return false;
    double dist = getDistanceToIntersection();
    double speed = 0.0;
    try {
        if (mobility && mobility->getVehicleCommandInterface())
            speed = mobility->getVehicleCommandInterface()->getSpeed();
    } catch (...) {
        speed = 0.0;
    }
    if (braking_decel_mps2_ <= 0.0) braking_decel_mps2_ = 4.5;
    double brakingDistance = (speed * speed) / (2.0 * braking_decel_mps2_);
    bool recallable = dist > brakingDistance + processing_latency_margin_;
    std::cout << "[HALT-LOCAL] r" << replicaId_
              << " recallable=" << (recallable ? 1 : 0)
              << " dist=" << dist
              << " speed=" << speed
              << " brake_dist=" << brakingDistance
              << " margin=" << processing_latency_margin_ << "\n";
    return recallable;
}

void ResDBIntersectionApp::handleValidCancelJustification(
    uint32_t cancelledEpoch, CancelReason reason, const std::string& reasonRef,
    const std::vector<uint8_t>& justification)
{
    if (!enableRollback_) return;
    const uint32_t epoch = cancelledEpoch;
    const bool recallable = isRecallable();
    rollback_local_recallable_ = recallable;
    if (recallable && current_phase_ != ConsensusPhase::DEPARTED) {
        if (resume_msg_ && resume_msg_->isScheduled()) cancelEvent(resume_msg_);
        if (clearance_poll_msg_ && clearance_poll_msg_->isScheduled()) cancelEvent(clearance_poll_msg_);
        stopVehicle();
    }
    std::cout << "[HALT-LOCAL] r" << replicaId_
              << " valid_cancel epoch=" << epoch
              << " reason=" << static_cast<int>(reason)
              << " recallable=" << (recallable ? 1 : 0)
              << " ref=" << reasonRef << "\n";
    beginRollbackDiscovery(epoch, reason, reasonRef, justification);
}

void ResDBIntersectionApp::beginRollbackDiscovery(
    uint32_t cancelledEpoch, CancelReason reason, const std::string& reasonRef,
    const std::vector<uint8_t>& justification)
{
    if (cancel_pending_ && cancelled_epoch_ == cancelledEpoch) return;
    cancel_pending_ = true;
    cancelled_epoch_ = cancelledEpoch;
    rollback_new_epoch_ = cancelledEpoch + 1;
    rollback_reason_ = reason;
    rollback_reason_ref_ = reasonRef;
    rollback_justification_ = justification;
    rollback_discovery_ready_ = false;
    rollback_propose_submitted_ = false;
    rollback_rotation_index_ = 0;

    current_epoch_ = rollback_new_epoch_;
    propose_submitted_ = false;
    order_applied_ = false;
    cert_broadcast_ = false;
    cert_collection_started_ = false;
    deferred_propose_after_cert_timeout_ = false;
    stopCertBroadcastRetries();

    {
        std::lock_guard<std::mutex> lk(certs_mutex_);
        collected_certs_.clear();
        local_vehicle_states_.clear();
        physically_observed_cars_.clear();
    }
    my_received_echoes_.clear();
    arrival_announcements_received_.clear();
    echoed_cars_.clear();
    announcement_relay_tracker_.reset();
    cert_relay_tracker_.reset();
    pending_relays_.clear();

    std::cout << "[ROLLBACK-BEGIN] r" << replicaId_
              << " cancelled_epoch=" << cancelled_epoch_
              << " new_epoch=" << rollback_new_epoch_
              << " reason=" << static_cast<int>(reason) << "\n";
    if (rollback_local_recallable_) {
        broadcastArrivalAnnouncement();
        tryStartCertCollectionTimer(true);
    } else {
        std::cout << "[ROLLBACK-DISCOVERY] r" << replicaId_
                  << " local vehicle non-recallable; not announcing into M"
                  << " cancelled_epoch=" << cancelled_epoch_
                  << " new_epoch=" << rollback_new_epoch_ << "\n";
    }
    if (!rollback_discovery_timer_)
        rollback_discovery_timer_ = new cMessage("rollbackDiscoveryTimer");
    if (rollback_discovery_timer_->isScheduled()) cancelEvent(rollback_discovery_timer_);
    scheduleAt(simTime() + rollback_discovery_timeout_sec_, rollback_discovery_timer_);
    std::cout << "[ROLLBACK-DISCOVERY] r" << replicaId_
              << " started timeout=" << rollback_discovery_timeout_sec_
              << "s cancelled_epoch=" << cancelled_epoch_
              << " new_epoch=" << rollback_new_epoch_ << "\n";
}

int ResDBIntersectionApp::minRollbackMembershipSize() const
{
    return std::min(total_vehicles_, 4);
}

bool ResDBIntersectionApp::rollbackDiscoveryComplete()
{
    std::set<int> visible;
    std::set<int> certed;
    {
        std::lock_guard<std::mutex> lk(certs_mutex_);
        for (const auto& kv : local_vehicle_states_) {
            int rid = extractReplicaId(kv.first);
            if (shouldIncludeInRollbackMembership(rid)) visible.insert(rid);
        }
        for (const auto& kv : collected_certs_) {
            int rid = extractReplicaId(kv.first);
            if (shouldIncludeInRollbackMembership(rid)) {
                visible.insert(rid);
                certed.insert(rid);
            }
        }
    }
    return (int)certed.size() >= minRollbackMembershipSize() &&
        certed.size() >= visible.size();
}

void ResDBIntersectionApp::maybeFinishRollbackDiscovery(const char* reason)
{
    if (!cancel_pending_ || rollback_discovery_ready_) return;
    if (!rollbackDiscoveryComplete()) return;
    rollback_discovery_ready_ = true;
    if (rollback_discovery_timer_ && rollback_discovery_timer_->isScheduled())
        cancelEvent(rollback_discovery_timer_);
    std::cout << "[ROLLBACK-DISCOVERY] r" << replicaId_
              << " complete reason=" << (reason ? reason : "certs")
              << " |M_candidate|=" << rollbackMembershipCandidates().size()
              << " cancelled_epoch=" << cancelled_epoch_
              << " new_epoch=" << rollback_new_epoch_ << "\n";
    trySubmitRollbackProposal(reason ? reason : "discovery-complete");
}

std::vector<int> ResDBIntersectionApp::rollbackMembershipCandidates()
{
    std::vector<int> candidates;
    {
        std::lock_guard<std::mutex> lk(certs_mutex_);
        for (const auto& kv : collected_certs_) {
            int rid = extractReplicaId(kv.first);
            if (shouldIncludeInRollbackMembership(rid)) candidates.push_back(rid);
        }
        for (const auto& kv : local_vehicle_states_) {
            int rid = extractReplicaId(kv.first);
            if (shouldIncludeInRollbackMembership(rid)) candidates.push_back(rid);
        }
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    return candidates;
}

int ResDBIntersectionApp::chooseRollbackProposer()
{
    std::vector<int> candidates = rollbackMembershipCandidates();
    if ((int)candidates.size() < minRollbackMembershipSize()) return -1;
    if (candidates.empty()) return -1;
    int idx = rollback_rotation_index_ % static_cast<int>(candidates.size());
    return candidates[idx];
}

bool ResDBIntersectionApp::shouldIncludeInRollbackMembership(int replicaId)
{
    if (replicaId < 0) return false;
    if (replicaId == replicaId_)
        return rollback_local_recallable_ &&
            current_phase_ != ConsensusPhase::DEPARTED &&
            !is_departed_;
    return true;
}

void ResDBIntersectionApp::trySubmitRollbackProposal(const char* reason)
{
    if (!cancel_pending_ || rollback_propose_submitted_) return;
    if (!rollback_discovery_ready_) {
        std::cout << "[ROLLBACK-PROPOSE] r" << replicaId_
                  << " wait reason=" << (reason ? reason : "try")
                  << " discovery_ready=0"
                  << " |M_candidate|=" << rollbackMembershipCandidates().size()
                  << " need>=" << minRollbackMembershipSize() << "\n";
        return;
    }
    int proposer = chooseRollbackProposer();
    if (proposer != replicaId_) {
        std::vector<int> candidates = rollbackMembershipCandidates();
        std::cout << "[ROLLBACK-PROPOSE] r" << replicaId_
                  << " skip reason=" << (reason ? reason : "try")
                  << " proposer=r" << proposer
                  << " |M_candidate|=" << candidates.size()
                  << " need>=" << minRollbackMembershipSize() << "\n";
        return;
    }
    proposeRollback();
    if (!rollback_vc_timer_) rollback_vc_timer_ = new cMessage("rollbackVcTimer");
    if (rollback_vc_timer_->isScheduled()) cancelEvent(rollback_vc_timer_);
    scheduleAt(simTime() + rollback_vc_timeout_sec_, rollback_vc_timer_);
}

void ResDBIntersectionApp::proposeRollback()
{
    if (!cancel_pending_ || rollback_propose_submitted_) return;

    std::set<int> included;
    std::vector<ResdbVehicleEntry> entries;
    {
        std::lock_guard<std::mutex> lk(certs_mutex_);
        for (const auto& kv : collected_certs_) {
            int rid = extractReplicaId(kv.first);
            if (!shouldIncludeInRollbackMembership(rid)) continue;
            ResdbVehicleEntry e{};
            e.replica_id = rid;
            e.is_ambulance = kv.second.isAmbulance ? 1 : 0;
            e.cyber_status = 1;
            e.lane = laneCode(kv.second.lane);
            e.direction = directionCode(kv.second.direction);
            e.position_in_lane = static_cast<uint8_t>(std::min(kv.second.positionInLane, 255));
            e.sim_time_us = (uint64_t)simTime().inUnit(SIMTIME_US);
            auto it = local_vehicle_states_.find(kv.first);
            if (it != local_vehicle_states_.end() && it->second.arrival_time_us != 0)
                e.sim_time_us = it->second.arrival_time_us;
            entries.push_back(e);
            included.insert(rid);
        }

        for (const auto& kv : local_vehicle_states_) {
            int rid = extractReplicaId(kv.first);
            if (included.count(rid) || !shouldIncludeInRollbackMembership(rid)) continue;
            const VehicleState& vs = kv.second;
            ResdbVehicleEntry e{};
            e.replica_id = rid;
            e.sim_time_us = UINT64_MAX;
            e.is_ambulance = 0;
            e.lane = laneCode(vs.lane);
            e.direction = 0;
            e.position_in_lane = static_cast<uint8_t>(std::min(vs.positionInLane, 255));
            e.cyber_status = 0;
            entries.push_back(e);
            included.insert(rid);
        }
    }

    if (entries.empty()) {
        std::cout << "[ROLLBACK-PROPOSE] r" << replicaId_
                  << " abort: empty membership M\n";
        return;
    }
    if ((int)entries.size() < minRollbackMembershipSize()) {
        std::cout << "[ROLLBACK-PROPOSE] r" << replicaId_
                  << " wait: |M|=" << entries.size()
                  << " need>=" << minRollbackMembershipSize()
                  << " cancelled_epoch=" << cancelled_epoch_
                  << " new_epoch=" << rollback_new_epoch_ << "\n";
        return;
    }

    ResdbRollbackHdr rhdr{};
    rhdr.new_epoch = rollback_new_epoch_;
    rhdr.cancelled_epoch = cancelled_epoch_;
    rhdr.reason = static_cast<uint8_t>(rollback_reason_);
    rhdr.justification_len = static_cast<uint32_t>(rollback_justification_.size());

    ResdbProposeHdr phdr{};
    phdr.epoch = rollback_new_epoch_;
    phdr.leader_id = replicaId_;
    phdr.propose_sim_time_us = (uint64_t)simTime().inUnit(SIMTIME_US);
    phdr.n_vehicles = static_cast<uint32_t>(entries.size());

    size_t total = sizeof(ResdbRollbackHdr) + rollback_justification_.size() +
        sizeof(ResdbProposeHdr) + entries.size() * sizeof(ResdbVehicleEntry);
    std::vector<uint8_t> buf(total);
    uint8_t* p = buf.data();
    std::memcpy(p, &rhdr, sizeof(rhdr)); p += sizeof(rhdr);
    if (!rollback_justification_.empty()) {
        std::memcpy(p, rollback_justification_.data(), rollback_justification_.size());
        p += rollback_justification_.size();
    }
    std::memcpy(p, &phdr, sizeof(phdr)); p += sizeof(phdr);
    for (const auto& e : entries) {
        std::memcpy(p, &e, sizeof(e)); p += sizeof(e);
    }

    rollback_propose_submitted_ = true;
    propose_submitted_ = true;
    propose_time_ = simTime();
    int rc = ResdbOmnetTriggerConsensus(resdb_server_handle_, buf.data(),
                                        static_cast<uint32_t>(buf.size()));
    std::cout << "[ROLLBACK-PROPOSE] r" << replicaId_
              << " rc=" << rc
              << " cancelled_epoch=" << cancelled_epoch_
              << " new_epoch=" << rollback_new_epoch_
              << " |M|=" << entries.size()
              << " TODO=resdb_dynamic_N_reconfiguration_pending\n";
}

bool ResDBIntersectionApp::maybeTriggerEmergencyRollbackFromCert(const ArrivalCert& cert)
{
    if (!enableRollback_ || !cert.isAmbulance) return false;
    int rid = extractReplicaId(cert.carId);
    if (has_committed_order_) {
        if (committed_order_vehicle_ids_.count(rid)) return false;
    } else if (rid >= 0 && rid < total_vehicles_) {
        return false;
    }
    const uint32_t cancelledEpoch = has_committed_order_ ? last_committed_epoch_ : current_epoch_;
    std::string ref = "amb:" + cert.carId + ":" + std::to_string(cert.epoch);
    rollback_cancel_initiated_ = true;
    std::cout << "[ROLLBACK-TRIGGER] r" << replicaId_
              << " emergency cert for unscheduled " << cert.carId
              << " cancelled_epoch=" << cancelledEpoch
              << " committed=" << (has_committed_order_ ? 1 : 0) << "\n";
    sendCancelEcho(cancelledEpoch, CANCEL_EMERGENCY, ref);
    return true;
}

void ResDBIntersectionApp::maybeTriggerCrashRollback(const std::string& reasonRef)
{
    if (!enableRollback_ || !has_committed_order_) return;
    std::string ref = cleanRef(reasonRef);
    std::cout << "[ROLLBACK-TRIGGER] r" << replicaId_
              << " crash ref=" << ref
              << " committed_epoch=" << last_committed_epoch_ << "\n";
    sendCancelEcho(last_committed_epoch_, CANCEL_CRASH, ref);
}

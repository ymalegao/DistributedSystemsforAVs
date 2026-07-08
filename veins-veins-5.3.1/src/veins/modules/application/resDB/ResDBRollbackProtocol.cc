#include "veins/modules/application/resDB/ResDBIntersectionApp.h"
#include "veins/modules/application/resDB/ResDBUtil.h"
#include "veins/modules/application/resDB/ResdbV2VWire.h"
#include "veins/modules/application/resDB/messages/BFTMessage_m.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>

using namespace veins;
using namespace veins::resdb_app_util;

namespace {

int anchoredFaults(int configuredF, int totalVehicles)
{
    return configuredF >= 0 ? configuredF : (totalVehicles - 1) / 3;
}

constexpr int kMinPerEpochRollbackVoteN = 4;

int bftQuorumSize(int n, int f)
{
    if (n <= 0 || f < 0 || f > (n - 1) / 3) return -1;
    return std::max((n + f + 2) / 2, 1);
}

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
    int f = anchoredFaults(tolerated_faults_, total_vehicles_);
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
        if (existing.echoingReplicaId == echo.echoingReplicaId) {
            std::cout << "[CANCEL-ECHO-DUP] r" << replicaId_
                      << " signer=r" << echo.echoingReplicaId
                      << " key=" << key
                      << " count=" << echoes.size() << "\n";
            return;
        }
    }
    echoes.push_back(echo);
    int threshold = anchoredFaults(tolerated_faults_, total_vehicles_) + 1;
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
        std::cout << "[CANCEL-CERT-GATE] r" << replicaId_
                  << " action=broadcast key=" << key
                  << " count=" << echoes.size()
                  << " threshold=" << threshold
                  << " seen=0\n";
        broadcastCancelCert(cert);
    } else if ((int)echoes.size() >= threshold) {
        std::cout << "[CANCEL-CERT-GATE] r" << replicaId_
                  << " action=skip key=" << key
                  << " count=" << echoes.size()
                  << " threshold=" << threshold
                  << " seen=" << (cancel_cert_seen_.count(key) ? 1 : 0)
                  << "\n";
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
    std::string key = cancelReasonKey(cert.cancelledEpoch, cert.reason, cert.reasonRef);
    if (!validateCancelCert(cert)) {
        std::cout << "[CANCEL-CERT-GATE] r" << replicaId_
                  << " action=invalid key=" << key
                  << " echoes=" << cert.echoes.size() << "\n";
        return;
    }
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
    if (firstSeen || !cancel_consensus_pending_)
        handleValidCancelJustification(cert.cancelledEpoch, cert.reason, cert.reasonRef, payload);
}

bool ResDBIntersectionApp::isRecallable()
{
    if (current_phase_ == ConsensusPhase::DEPARTED || is_departed_) return false;
    bool inOrPastConflict = isInOrPastConflictBox();
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
    bool alreadyStopped = speed <= 0.1;
    const bool waitingForPriorBatch =
        my_batch_index_ > 0 &&
        !preceding_batch_cars_.empty() &&
        clearance_poll_msg_ &&
        clearance_poll_msg_->isScheduled();
    bool recallable = (!inOrPastConflict && waitingForPriorBatch) ||
        (!inOrPastConflict &&
         (alreadyStopped || dist > brakingDistance + processing_latency_margin_));
    std::cout << "[HALT-LOCAL] r" << replicaId_
              << " recallable=" << (recallable ? 1 : 0)
              << " dist=" << dist
              << " speed=" << speed
              << " stopped=" << (alreadyStopped ? 1 : 0)
              << " waiting_prior_batch=" << (waitingForPriorBatch ? 1 : 0)
              << " batch=" << my_batch_index_
              << " in_or_past_conflict=" << (inOrPastConflict ? 1 : 0)
              << " brake_dist=" << brakingDistance
              << " margin=" << processing_latency_margin_ << "\n";
    return recallable;
}

void ResDBIntersectionApp::handleValidCancelJustification(
    uint32_t cancelledEpoch, CancelReason reason, const std::string& reasonRef,
    const std::vector<uint8_t>& justification)
{
    if (!enableRollback_) {
        std::cout << "[CANCEL-JUSTIFY-GATE] r" << replicaId_
                  << " action=skip reason=rollback-disabled"
                  << " cancelled_epoch=" << cancelledEpoch << "\n";
        return;
    }
    if (cancel_consensus_pending_ || cancel_pending_) {
        if (cancelled_epoch_ == cancelledEpoch) {
            std::cout << "[CANCEL-JUSTIFY-GATE] r" << replicaId_
                      << " action=skip reason=already-pending"
                      << " cancelled_epoch=" << cancelledEpoch
                      << " cancel_consensus_pending=" << (cancel_consensus_pending_ ? 1 : 0)
                      << " cancel_pending=" << (cancel_pending_ ? 1 : 0)
                      << " propose_submitted=" << (cancel_propose_submitted_ ? 1 : 0)
                      << " rotation_index=" << rollback_rotation_index_
                      << " proposer=r" << chooseCancelProposer()
                      << "\n";
            return;
        }
    }
    if (isEpochTombstoned(cancelledEpoch)) {
        std::cout << "[CANCEL-JUSTIFY-GATE] r" << replicaId_
                  << " action=skip reason=tombstoned"
                  << " cancelled_epoch=" << cancelledEpoch << "\n";
        return;
    }

    const uint32_t epoch = cancelledEpoch;
    const bool recallable = isRecallable();
    rollback_local_recallable_ = recallable;
    if (recallable && current_phase_ != ConsensusPhase::DEPARTED) {
        if (resume_msg_ && resume_msg_->isScheduled()) cancelEvent(resume_msg_);
        if (clearance_poll_msg_ && clearance_poll_msg_->isScheduled()) cancelEvent(clearance_poll_msg_);
        stopVehicle();
    }
    cancelled_epoch_ = epoch;
    rollback_reason_ = reason;
    rollback_reason_ref_ = reasonRef;
    cancel_cert_bytes_ = justification;
    cancel_consensus_pending_ = true;
    cancel_propose_submitted_ = false;
    rollback_rotation_index_ = 0;
    rollback_cancel_initiated_ = true;
    stopGossip();

    std::cout << "[HALT-LOCAL] r" << replicaId_
              << " valid_cancel epoch=" << epoch
              << " reason=" << static_cast<int>(reason)
              << " recallable=" << (recallable ? 1 : 0)
              << " ref=" << reasonRef
              << " cert_bytes=" << cancel_cert_bytes_.size()
              << " rotation_index=" << rollback_rotation_index_
              << " proposer=r" << chooseCancelProposer()
              << " |E|=" << cancelElectorateCandidates().size()
              << "\n";
    trySubmitCancelProposal("cert-validated");
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
    rollback_expected_membership_size_ = 0;
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
              << " expectedN=" << minRollbackMembershipSize()
              << " reason=" << static_cast<int>(reason) << "\n";
    if (rollback_local_recallable_) {
        broadcastArrivalAnnouncement();
        if (!broadcastArrivalAnnouncement_timer_)
            broadcastArrivalAnnouncement_timer_ =
                new cMessage("resdbBroadcastArrivalAnnouncement");
        if (broadcastArrivalAnnouncement_timer_->isScheduled())
            cancelEvent(broadcastArrivalAnnouncement_timer_);
        scheduleAt(simTime() + broadcast_arrival_announcement_interval_,
                   broadcastArrivalAnnouncement_timer_);
        std::cout << "[ROLLBACK-DISCOVERY] r" << replicaId_
                  << " reannounce timer armed interval="
                  << broadcast_arrival_announcement_interval_
                  << " cancelled_epoch=" << cancelled_epoch_
                  << " new_epoch=" << rollback_new_epoch_ << "\n";
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

int ResDBIntersectionApp::minRollbackVoteN() const
{
    if (isRollbackPerEpochMode()) return kMinPerEpochRollbackVoteN;
    const int f = anchoredFaults(tolerated_faults_, total_vehicles_);
    return std::max(1, 3 * f + 1);
}

int ResDBIntersectionApp::minRollbackMembershipSize() const
{
    if (rollback_expected_membership_size_ > 0)
        return std::max(minRollbackVoteN(), rollback_expected_membership_size_);
    return minRollbackVoteN();
}

bool ResDBIntersectionApp::isRollbackPerEpochMode() const
{
    return rollback_fault_mode_per_epoch_;
}

std::vector<int> ResDBIntersectionApp::cancelElectorateCandidates() const
{
    std::vector<int> electors(committed_order_vehicle_ids_.begin(),
                              committed_order_vehicle_ids_.end());
    std::sort(electors.begin(), electors.end());
    return electors;
}

bool ResDBIntersectionApp::isEpochTombstoned(uint32_t epoch) const
{
    if (tombstoned_epochs_.count(epoch)) return true;
    auto it = committed_cancels_.find(epoch);
    return it != committed_cancels_.end();
}

bool ResDBIntersectionApp::hasCommittedCancel(uint32_t epoch) const
{
    return committed_cancels_.count(epoch) > 0;
}

int ResDBIntersectionApp::chooseCancelProposer()
{
    std::vector<int> electors = cancelElectorateCandidates();
    if (electors.empty()) return -1;
    int idx = rollback_rotation_index_ % static_cast<int>(electors.size());
    return electors[idx];
}

void ResDBIntersectionApp::trySubmitCancelProposal(const char* reason)
{
    std::vector<int> electors = cancelElectorateCandidates();
    int proposer = chooseCancelProposer();
    if (!cancel_consensus_pending_ || cancel_propose_submitted_) {
        std::cout << "[CANCEL-PROPOSE-GATE] r" << replicaId_
                  << " action=skip reason=" << (reason ? reason : "try")
                  << " gate="
                  << (!cancel_consensus_pending_ ? "not-pending" : "already-submitted")
                  << " pending=" << (cancel_consensus_pending_ ? 1 : 0)
                  << " submitted=" << (cancel_propose_submitted_ ? 1 : 0)
                  << " rotation_index=" << rollback_rotation_index_
                  << " proposer=r" << proposer
                  << " |E|=" << electors.size()
                  << " cert_bytes=" << cancel_cert_bytes_.size()
                  << " t=" << simTime() << "\n";
        return;
    }
    std::cout << "[CANCEL-PROPOSER-CHECK] r" << replicaId_
              << " reason=" << (reason ? reason : "try")
              << " rotation_index=" << rollback_rotation_index_
              << " proposer=r" << proposer
              << " self_is_proposer=" << (proposer == replicaId_ ? 1 : 0)
              << " pending=" << (cancel_consensus_pending_ ? 1 : 0)
              << " submitted=" << (cancel_propose_submitted_ ? 1 : 0)
              << " |E|=" << electors.size()
              << " cert_bytes=" << cancel_cert_bytes_.size()
              << " t=" << simTime() << "\n";
    if (proposer != replicaId_) {
        std::cout << "[CANCEL-PROPOSE] r" << replicaId_
                  << " skip reason=" << (reason ? reason : "try")
                  << " proposer=r" << proposer
                  << " rotation_index=" << rollback_rotation_index_
                  << " |E|=" << electors.size()
                  << " cert_bytes=" << cancel_cert_bytes_.size()
                  << " t=" << simTime() << "\n";
        return;
    }
    proposeCancel();
    if (!cancel_vc_timer_) cancel_vc_timer_ = new cMessage("cancelVcTimer");
    if (cancel_vc_timer_->isScheduled()) cancelEvent(cancel_vc_timer_);
    scheduleAt(simTime() + rollback_vc_timeout_sec_, cancel_vc_timer_);
}

void ResDBIntersectionApp::proposeCancel()
{
    if (!cancel_consensus_pending_ || cancel_propose_submitted_) {
        std::cout << "[CANCEL-PROPOSE-GATE] r" << replicaId_
                  << " action=abort-propose gate="
                  << (!cancel_consensus_pending_ ? "not-pending" : "already-submitted")
                  << " pending=" << (cancel_consensus_pending_ ? 1 : 0)
                  << " submitted=" << (cancel_propose_submitted_ ? 1 : 0)
                  << " rotation_index=" << rollback_rotation_index_
                  << " proposer=r" << chooseCancelProposer()
                  << " |E|=" << cancelElectorateCandidates().size()
                  << "\n";
        return;
    }
    if (cancel_cert_bytes_.empty()) {
        std::cout << "[CANCEL-PROPOSE-GATE] r" << replicaId_
                  << " action=abort-propose gate=missing-cert"
                  << " cancelled_epoch=" << cancelled_epoch_
                  << " rotation_index=" << rollback_rotation_index_
                  << " proposer=r" << chooseCancelProposer()
                  << " |E|=" << cancelElectorateCandidates().size()
                  << "\n";
        return;
    }

    std::vector<int> electors = cancelElectorateCandidates();
    if (electors.empty()) {
        std::cout << "[CANCEL-PROPOSE] r" << replicaId_
                  << " abort: empty electorate E\n";
        return;
    }
    const int fAnchored = anchoredFaults(tolerated_faults_, total_vehicles_);
    const int minAvailable = 3 * fAnchored + 1;
    const int cancelQuorum = bftQuorumSize(static_cast<int>(electors.size()), fAnchored);
    if ((int)electors.size() < minAvailable || cancelQuorum < 0) {
        std::cout << "[CANCEL-UNAVAILABLE] r" << replicaId_
                  << " |E|=" << electors.size()
                  << " f_anchored=" << fAnchored
                  << " need>=" << minAvailable
                  << " cancelled_epoch=" << cancelled_epoch_ << "\n";
        return;
    }

    ResdbCancelHdr chdr{};
    chdr.cancelled_epoch = cancelled_epoch_;
    chdr.reason = static_cast<uint8_t>(rollback_reason_);
    chdr.justification_len = static_cast<uint32_t>(cancel_cert_bytes_.size());

    const int32_t leader_id = replicaId_;
    const uint32_t n_electors = static_cast<uint32_t>(electors.size());
    size_t total = sizeof(ResdbCancelHdr) + cancel_cert_bytes_.size() +
        sizeof(int32_t) + sizeof(uint32_t) + electors.size() * sizeof(int32_t);
    std::vector<uint8_t> buf(total);
    uint8_t* p = buf.data();
    std::memcpy(p, &chdr, sizeof(chdr)); p += sizeof(chdr);
    std::memcpy(p, cancel_cert_bytes_.data(), cancel_cert_bytes_.size());
    p += cancel_cert_bytes_.size();
    std::memcpy(p, &leader_id, sizeof(leader_id)); p += sizeof(leader_id);
    std::memcpy(p, &n_electors, sizeof(n_electors)); p += sizeof(n_electors);
    for (int rid : electors) {
        int32_t eid = rid;
        std::memcpy(p, &eid, sizeof(eid)); p += sizeof(eid);
    }

    cancel_propose_submitted_ = true;
    propose_submitted_ = true;
    propose_time_ = simTime();
    int rc = ResdbOmnetTriggerConsensus(resdb_server_handle_, buf.data(),
                                        static_cast<uint32_t>(buf.size()));
    std::cout << "[CANCEL-QUORUM] r" << replicaId_
              << " |E|=" << electors.size()
              << " f_anchored=" << fAnchored
              << " quorum=" << cancelQuorum
              << " proposer=r" << replicaId_
              << " cancelled_epoch=" << cancelled_epoch_ << "\n";
    std::cout << "[CANCEL-PROPOSE] r" << replicaId_
              << " rc=" << rc
              << " cancelled_epoch=" << cancelled_epoch_
              << " |E|=" << electors.size()
              << " cert_bytes=" << cancel_cert_bytes_.size()
              << " payload_bytes=" << buf.size()
              << " rotation_index=" << rollback_rotation_index_
              << " t=" << simTime() << "\n";
}

std::vector<uint8_t> ResDBIntersectionApp::buildCancelCommitRef(uint32_t cancelledEpoch) const
{
    auto it = committed_cancels_.find(cancelledEpoch);
    if (it == committed_cancels_.end()) return {};
    ResdbCancelCommitRef ref{};
    ref.cancelled_epoch = cancelledEpoch;
    ref.cancel_seq = it->second.cancel_seq;
    std::memcpy(ref.payload_digest, it->second.payload_digest, 32);
    ref.proof_len = static_cast<uint32_t>(it->second.attestation_bytes.size());
    std::vector<uint8_t> out(sizeof(ref) + ref.proof_len);
    std::memcpy(out.data(), &ref, sizeof(ref));
    if (ref.proof_len > 0) {
        std::memcpy(out.data() + sizeof(ref), it->second.attestation_bytes.data(),
                    ref.proof_len);
    }
    return out;
}

void ResDBIntersectionApp::broadcastCancelCommitAttestation(const ResdbCancelDecisionHdr& dh)
{
    if (!gossip_enabled_ || !ec_private_key_) return;
    std::vector<uint8_t> attestation(sizeof(dh));
    std::memcpy(attestation.data(), &dh, sizeof(dh));
    triggerCancelCommitGossip(dh.cancelled_epoch, attestation);
}

void ResDBIntersectionApp::triggerCancelCommitGossip(
    uint32_t cancelledEpoch, const std::vector<uint8_t>& attestation)
{
    if (attestation.empty()) return;
    if (cancel_gossip_timer_ && cancel_gossip_timer_->isScheduled())
        cancelEvent(cancel_gossip_timer_);
    if (!cancel_gossip_timer_)
        cancel_gossip_timer_ = new cMessage("resdbCancelCommitGossip");
    cancel_gossip_epoch_ = cancelledEpoch;
    cancel_gossip_bytes_ = attestation;
    cancel_gossip_retry_count_ = 0;

    auto inner = resdb_gossip::serialize(cancelledEpoch, attestation);
    auto signed_payload = resdbwire::packSignedPacket(
        ec_private_key_, ec_pub_key_, inner.data(), (uint32_t)inner.size());
    if (!signed_payload.empty()) {
        sendBFTMessage(-1, signed_payload, kCancelCommitGossipType);
        std::cout << "[CANCEL-GOSSIP-SEND] r" << replicaId_
                  << " cancelled_epoch=" << cancelledEpoch << "\n";
    }
    cancel_gossip_retry_count_++;
    if (cancel_cert_retry_max_ <= 0 ||
            cancel_gossip_retry_count_ < cancel_cert_retry_max_) {
        scheduleAt(simTime() + cancel_cert_retry_interval_sec_,
                   cancel_gossip_timer_);
    }
}

void ResDBIntersectionApp::handleCancelCommitDecision(const std::vector<uint8_t>& dec)
{
    if (dec.size() < sizeof(ResdbCancelDecisionHdr)) return;
    ResdbCancelDecisionHdr dh{};
    std::memcpy(&dh, dec.data(), sizeof(dh));
    if (dh.magic != RESDB_CANCEL_DECISION_MAGIC) return;

    tombstoned_epochs_.insert(dh.cancelled_epoch);
    CommittedCancelInfo info;
    info.cancel_seq = dh.cancel_seq;
    std::memcpy(info.payload_digest, dh.payload_digest, 32);
    info.attestation_bytes = dec;
    committed_cancels_[dh.cancelled_epoch] = info;

    const int fAnchored = anchoredFaults(tolerated_faults_, total_vehicles_);
    const int cancelQuorum = bftQuorumSize(
        static_cast<int>(cancelElectorateCandidates().size()), fAnchored);
    std::cout << "[CANCEL-COMMIT] r" << replicaId_
              << " cancelled_epoch=" << dh.cancelled_epoch
              << " seq=" << dh.cancel_seq
              << " quorum=" << cancelQuorum
              << " source=commit\n";
    std::cout << "[TOMBSTONE] r" << replicaId_
              << " epoch=" << dh.cancelled_epoch << " source=cancel-commit\n";

    cancel_consensus_pending_ = false;
    cancel_propose_submitted_ = false;
    rollback_cancel_initiated_ = false;
    stopCancelCertRetries();
    if (cancel_vc_timer_) {
        if (cancel_vc_timer_->isScheduled()) cancelEvent(cancel_vc_timer_);
        delete cancel_vc_timer_;
        cancel_vc_timer_ = nullptr;
    }

    rollback_justification_ = buildCancelCommitRef(dh.cancelled_epoch);
    broadcastCancelCommitAttestation(dh);
    beginRollbackDiscovery(dh.cancelled_epoch, rollback_reason_,
                           rollback_reason_ref_, rollback_justification_);
}

void ResDBIntersectionApp::handleCancelCommitGossip(BFTMessage* bft)
{
    int plen = bft->getPayloadArraySize();
    if (plen <= 0) return;
    std::vector<uint8_t> buf((size_t)plen);
    for (int i = 0; i < plen; ++i) buf[i] = bft->getPayload(i);

    resdbwire::SignedPacketView view;
    if (!resdbwire::unpackSignedPacket(buf.data(), (uint32_t)buf.size(), &view)) return;
    if (!CryptoAuth::instance().verifyBytes(view.pubKey, view.resdbBytes, view.resdbLen,
                                            view.sig, view.sigLen)) {
        return;
    }

    uint32_t cancelled_epoch = 0;
    std::vector<uint8_t> attestation;
    if (!resdb_gossip::parse(view.resdbBytes, view.resdbLen,
                             cancelled_epoch, attestation)) {
        return;
    }
    if (attestation.size() < sizeof(ResdbCancelDecisionHdr)) return;
    ResdbCancelDecisionHdr dh{};
    std::memcpy(&dh, attestation.data(), sizeof(dh));
    if (dh.magic != RESDB_CANCEL_DECISION_MAGIC ||
            dh.cancelled_epoch != cancelled_epoch) {
        return;
    }
    if (isEpochTombstoned(cancelled_epoch)) return;

    int threshold = anchoredFaults(tolerated_faults_, total_vehicles_) + 1;
    bool reached = cancel_gossip_acc_.add(
        bft->getFromReplicaId(), cancelled_epoch, attestation, threshold);
    if (!reached) return;

    tombstoned_epochs_.insert(cancelled_epoch);
    CommittedCancelInfo info;
    info.cancel_seq = dh.cancel_seq;
    std::memcpy(info.payload_digest, dh.payload_digest, 32);
    info.attestation_bytes = attestation;
    info.gossip_adopted = true;
    committed_cancels_[cancelled_epoch] = info;
    std::cout << "[CANCEL-COMMIT] r" << replicaId_
              << " cancelled_epoch=" << cancelled_epoch
              << " seq=" << dh.cancel_seq
              << " source=gossip-adopted\n";
    std::cout << "[TOMBSTONE] r" << replicaId_
              << " epoch=" << cancelled_epoch << " source=gossip-adopted\n";

    rollback_justification_ = buildCancelCommitRef(cancelled_epoch);
    cancel_consensus_pending_ = false;
    cancel_propose_submitted_ = false;
    triggerCancelCommitGossip(cancelled_epoch, attestation);

    const CancelReason reason = static_cast<CancelReason>(dh.reason);
    const std::string ref = rollback_reason_ref_.empty()
        ? "gossip-adopted"
        : rollback_reason_ref_;
    beginRollbackDiscovery(cancelled_epoch, reason, ref, rollback_justification_);

    if (resdb_server_handle_) {
        const int sync_rc = ResdbOmnetAdvanceExecutorAfterGossipCancel(
            resdb_server_handle_, cancelled_epoch);
        if (sync_rc != 0) {
            std::cout << "[EXECUTOR-GOSSIP-SYNC] r" << replicaId_
                      << " advance failed rc=" << sync_rc
                      << " cancelled_epoch=" << cancelled_epoch << "\n";
        }
    }
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
    const int expectedN = minRollbackMembershipSize();
    return (int)certed.size() >= expectedN &&
        (int)visible.size() >= expectedN &&
        certed.size() >= visible.size();
}

void ResDBIntersectionApp::logRollbackDiscoveryDiagnostics(const char* reason) const
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
    std::vector<int> missing;
    for (int rid : visible) {
        if (!certed.count(rid)) missing.push_back(rid);
    }
    std::sort(missing.begin(), missing.end());
    const int quiet = static_cast<int>(visible.size() - certed.size());
    std::cout << "[ROLLBACK-DISCOVERY] r" << replicaId_
              << " snapshot reason=" << (reason ? reason : "snapshot")
              << " mode=" << (isRollbackPerEpochMode() ? "per_epoch" : "anchored")
              << " signed=" << certed.size()
              << " quiet=" << quiet
              << " voteN=" << certed.size()
              << " sceneN=" << visible.size()
              << " expectedN=" << minRollbackMembershipSize()
              << " missingCerts=";
    for (size_t i = 0; i < missing.size(); ++i) {
        if (i) std::cout << ",";
        std::cout << "r" << missing[i];
    }
    std::cout << " cancelled_epoch=" << cancelled_epoch_
              << " new_epoch=" << rollback_new_epoch_ << "\n";
}

void ResDBIntersectionApp::maybeFinishRollbackDiscovery(const char* reason)
{
    if (!cancel_pending_ || rollback_discovery_ready_) return;
    if (!rollbackDiscoveryComplete()) return;
    rollback_discovery_ready_ = true;
    if (rollback_discovery_timer_ && rollback_discovery_timer_->isScheduled())
        cancelEvent(rollback_discovery_timer_);
    logRollbackDiscoveryDiagnostics(reason ? reason : "discovery-complete");
    trySubmitRollbackProposal(reason ? reason : "discovery-complete");
}

std::vector<int> ResDBIntersectionApp::rollbackCertedCandidates() const
{
    std::vector<int> candidates;
    {
        std::lock_guard<std::mutex> lk(certs_mutex_);
        for (const auto& kv : collected_certs_) {
            int rid = extractReplicaId(kv.first);
            if (shouldIncludeInRollbackMembership(rid)) candidates.push_back(rid);
        }
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    return candidates;
}

std::vector<int> ResDBIntersectionApp::rollbackMembershipCandidates() const
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

int ResDBIntersectionApp::designatedRollbackUnavailableReporter() const
{
    auto electors = cancelElectorateCandidates();
    if (!electors.empty()) return electors.front();
    return 0;
}

int ResDBIntersectionApp::chooseRollbackProposer()
{
    std::vector<int> candidates = rollbackCertedCandidates();
    if ((int)candidates.size() < minRollbackMembershipSize()) return -1;
    if (candidates.empty()) return -1;
    int idx = rollback_rotation_index_ % static_cast<int>(candidates.size());
    return candidates[idx];
}

bool ResDBIntersectionApp::shouldIncludeInRollbackMembership(int replicaId) const
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
                  << " voteN=" << rollbackCertedCandidates().size()
                  << " need>=" << minRollbackMembershipSize() << "\n";
        return;
    }
    if (reason && std::string(reason) == "discovery-timeout")
        logRollbackDiscoveryDiagnostics(reason);

    const int voteN = static_cast<int>(rollbackCertedCandidates().size());
    const int expectedN = minRollbackMembershipSize();
    int proposer = chooseRollbackProposer();
    if (proposer == -1) {
        if (replicaId_ == designatedRollbackUnavailableReporter()) {
            std::cout << "[ROLLBACK-UNAVAILABLE] r" << replicaId_
                      << " mode="
                      << (isRollbackPerEpochMode() ? "per_epoch" : "anchored")
                      << " reason=membership-too-small"
                      << " voteN=" << voteN
                      << " need>=" << expectedN
                      << " cancelled_epoch=" << cancelled_epoch_
                      << " new_epoch=" << rollback_new_epoch_ << "\n";
        } else {
            std::cout << "[ROLLBACK-PROPOSE] r" << replicaId_
                      << " skip reason=" << (reason ? reason : "try")
                      << " proposer=r-1"
                      << " voteN=" << voteN
                      << " need>=" << expectedN
                      << " reporter=r"
                      << designatedRollbackUnavailableReporter() << "\n";
        }
        return;
    }
    if (proposer != replicaId_) {
        std::cout << "[ROLLBACK-PROPOSE] r" << replicaId_
                  << " skip reason=" << (reason ? reason : "try")
                  << " proposer=r" << proposer
                  << " voteN=" << voteN
                  << " sceneN=" << rollbackMembershipCandidates().size()
                  << " need>=" << expectedN << "\n";
        return;
    }
    proposeAll();
    if (!rollback_vc_timer_) rollback_vc_timer_ = new cMessage("rollbackVcTimer");
    if (rollback_vc_timer_->isScheduled()) cancelEvent(rollback_vc_timer_);
    scheduleAt(simTime() + rollback_vc_timeout_sec_, rollback_vc_timer_);
}

bool ResDBIntersectionApp::maybeTriggerEmergencyRollbackFromCert(const ArrivalCert& cert)
{
    if (!enableRollback_ || !cert.isAmbulance) return false;
    int rid = extractReplicaId(cert.carId);
    if (!has_committed_order_) return false;
    if (committed_order_vehicle_ids_.count(rid)) return false;
    const uint32_t cancelledEpoch = last_committed_epoch_;
    std::string ref = "amb:" + cert.carId + ":" + std::to_string(cancelledEpoch);
    rollback_cancel_initiated_ = true;
    std::cout << "[CANCEL-WITNESS] r" << replicaId_
              << " source=arrival_cert car=" << cert.carId
              << " cancelled_epoch=" << cancelledEpoch
              << " ref=" << ref << "\n";
    sendCancelEcho(cancelledEpoch, CANCEL_EMERGENCY, ref);
    return true;
}

bool ResDBIntersectionApp::maybeTriggerEmergencyRollbackFromAnnouncement(
    const ArrivalAnnouncement& ann)
{
    if (!enableRollback_ || !ann.isAmbulance) return false;
    if (!has_committed_order_) return false;
    int rid = extractReplicaId(ann.carId);
    if (committed_order_vehicle_ids_.count(rid)) return false;
    const uint32_t cancelledEpoch = last_committed_epoch_;
    std::string ref = "amb:" + ann.carId + ":" + std::to_string(cancelledEpoch);
    rollback_cancel_initiated_ = true;
    std::cout << "[CANCEL-WITNESS] r" << replicaId_
              << " source=arrival_announce car=" << ann.carId
              << " cancelled_epoch=" << cancelledEpoch
              << " ref=" << ref << "\n";
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

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

// CancelReason and WitnessKind share the same 0/1 values (CANCEL_CRASH,
// CANCEL_EMERGENCY) by construction, so cancelReasonKey/cancelSignPayload
// below produce byte-identical strings to the pre-refactor inline versions.
WitnessKind ResDBIntersectionApp::toWitnessKind(CancelReason r)
{
    return r == CANCEL_CRASH ? WitnessKind::CANCEL_CRASH : WitnessKind::CANCEL_EMERGENCY;
}

WitnessEcho ResDBIntersectionApp::toWitnessEcho(const CancelEcho& e)
{
    WitnessEcho we;
    we.signerId = e.echoingReplicaId;
    std::memcpy(we.pubKey, e.signerPubKey, CRYPTO_PUBKEY_BYTES);
    std::memcpy(we.sig, e.signature, CRYPTO_SIG_MAX_BYTES);
    we.sigLen = e.signatureLen;
    return we;
}

CancelEcho ResDBIntersectionApp::toCancelEcho(
    const WitnessEcho& we, uint32_t epoch, CancelReason reason, const std::string& reasonRef)
{
    CancelEcho e;
    e.echoingReplicaId = we.signerId;
    e.cancelledEpoch = epoch;
    e.reason = reason;
    e.reasonRef = reasonRef;
    std::memcpy(e.signerPubKey, we.pubKey, CRYPTO_PUBKEY_BYTES);
    std::memcpy(e.signature, we.sig, CRYPTO_SIG_MAX_BYTES);
    e.signatureLen = we.sigLen;
    return e;
}

std::string ResDBIntersectionApp::cancelReasonKey(
    uint32_t epoch, CancelReason reason, const std::string& reasonRef) const
{
    return WitnessStatement{epoch, toWitnessKind(reason), reasonRef}.collectorKey();
}

std::string ResDBIntersectionApp::cancelSignPayload(
    uint32_t cancelledEpoch, CancelReason reason, const std::string& reasonRef,
    int echoingReplicaId) const
{
    return WitnessStatement{cancelledEpoch, toWitnessKind(reason), reasonRef}
        .signPayload(echoingReplicaId);
}

// Canonical BLOCKED-incident ref, carried as the CANCEL_CRASH reasonRef. The
// single formatter/parser pair below is the only place this string is built
// or split — protocol checks consume BlockedIncident, never scattered parsing.
std::string ResDBIntersectionApp::formatBlockedBatchRef(
    uint32_t cancelledEpoch, uint32_t executingBatch)
{
    return "blocked_batch:" + std::to_string(cancelledEpoch) + ":" +
        std::to_string(executingBatch);
}

bool ResDBIntersectionApp::parseBlockedBatchRef(const std::string& ref, BlockedIncident& out)
{
    auto parts = splitStr(ref, ':');
    if (parts.size() != 3 || parts[0] != "blocked_batch") return false;
    try {
        out.cancelledEpoch = static_cast<uint32_t>(std::stoul(parts[1]));
        out.executingBatch = static_cast<uint32_t>(std::stoul(parts[2]));
    } catch (...) {
        return false;
    }
    return true;
}

void ResDBIntersectionApp::registerBlockedIncidentIfCrash(const CancelCert& cert)
{
    if (cert.reason != CANCEL_CRASH) return;
    BlockedIncident inc;
    if (!parseBlockedBatchRef(cert.reasonRef, inc)) return;
    if (incidentRegistry_.count(inc)) return;
    incidentRegistry_.emplace(inc, IncidentRecord{IncidentState::BLOCKING, serializeCancelCert(cert)});
    std::cout << "[INCIDENT-REGISTER] r" << ctx_.replicaId_
              << " state=BLOCKING epoch=" << inc.cancelledEpoch
              << " batch=" << inc.executingBatch << "\n";
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

CancelEcho
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

CancelCert
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

    WitnessStatement stmt{cert.cancelledEpoch, toWitnessKind(cert.reason), cert.reasonRef};
    std::vector<WitnessEcho> echoes;
    echoes.reserve(cert.echoes.size());
    for (const auto& echo : cert.echoes) {
        if (echo.cancelledEpoch != cert.cancelledEpoch ||
                echo.reason != cert.reason ||
                echo.reasonRef != cert.reasonRef ||
                echo.signatureLen == 0) {
            continue;
        }
        echoes.push_back(toWitnessEcho(echo));
    }

    const int f = anchoredFaults(ctx_.tolerated_faults_, ctx_.total_vehicles_);
    // Signers must belong to the 16-member committed view (M_e); the incident
    // subject (e.g. an emergency-triggering ambulance not yet in M_e) is never
    // checked here — only each echo's signerId.
    std::lock_guard<std::mutex> lk(committed_view_mutex_);
    WitnessCertificateValidator validator(f, &ctx_.committed_order_vehicle_ids_);
    return validator.validate(stmt, echoes);
}

void ResDBIntersectionApp::sendCancelEcho(
    uint32_t cancelledEpoch, CancelReason reason, const std::string& reasonRef)
{
    if (!ctx_.enableRollback_ || !ctx_.ec_private_key_) return;
    const std::string cleanedRef = cleanRef(reasonRef);
    WitnessStatement stmt{cancelledEpoch, toWitnessKind(reason), cleanedRef};
    const std::string key = stmt.collectorKey();
    if (!rollback_.cancel_echo_sent_.insert(key).second) return;
    if (cancel_state_ == CancelState::INACTIVE) {
        cancel_state_ = CancelState::WITNESSING;
        std::cout << "[CANCEL-WITNESSING] r" << ctx_.replicaId_
                  << " cancelled_epoch=" << cancelledEpoch
                  << " ref=" << cleanedRef
                  << " t=" << simTime() << "\n";
    }

    CancelEcho echo;
    echo.echoingReplicaId = ctx_.replicaId_;
    echo.cancelledEpoch = cancelledEpoch;
    echo.reason = reason;
    echo.reasonRef = cleanedRef;
    std::memcpy(echo.signerPubKey, ctx_.ec_pub_key_, CRYPTO_PUBKEY_BYTES);

    const std::string toSign = stmt.signPayload(echo.echoingReplicaId);
    if (!CryptoAuth::instance().signBytes(
            ctx_.ec_private_key_, reinterpret_cast<const uint8_t*>(toSign.c_str()),
            toSign.size(), echo.signature, echo.signatureLen)) {
        return;
    }

    sendBFTMessage(-1, serializeCancelEcho(echo), kCancelEchoType);
    std::cout << "[CANCEL-ECHO] r" << ctx_.replicaId_
              << " epoch=" << cancelledEpoch
              << " reason=" << static_cast<int>(reason)
              << " ref=" << echo.reasonRef << "\n";

    rollback_.cancel_echo_collector_.add(stmt, toWitnessEcho(echo));
}

void ResDBIntersectionApp::handleCancelEcho(BFTMessage* msg)
{
    if (!ctx_.enableRollback_) return;
    CancelEcho echo = deserializeCancelEcho(msg);
    if (echo.echoingReplicaId < 0 || echo.signatureLen == 0) return;
    if (echo.reason != CANCEL_CRASH && echo.reason != CANCEL_EMERGENCY) return;
    WitnessStatement stmt{echo.cancelledEpoch, toWitnessKind(echo.reason), echo.reasonRef};
    const std::string toSign = stmt.signPayload(echo.echoingReplicaId);
    if (!CryptoAuth::instance().verifyBytes(
            echo.signerPubKey,
            reinterpret_cast<const uint8_t*>(toSign.c_str()), toSign.size(),
            echo.signature, echo.signatureLen)) {
        std::cout << "[CANCEL-ECHO] r" << ctx_.replicaId_
                  << " dropped invalid echo from r" << echo.echoingReplicaId << "\n";
        return;
    }

    const std::string key = stmt.collectorKey();
    if (!rollback_.cancel_echo_collector_.add(stmt, toWitnessEcho(echo))) {
        const auto* bucket = rollback_.cancel_echo_collector_.get(key);
        std::cout << "[CANCEL-ECHO-DUP] r" << ctx_.replicaId_
                  << " signer=r" << echo.echoingReplicaId
                  << " key=" << key
                  << " count=" << (bucket ? bucket->size() : 0) << "\n";
        return;
    }
    const auto* bucket = rollback_.cancel_echo_collector_.get(key);
    const size_t count = bucket ? bucket->size() : 0;
    const int threshold = anchoredFaults(ctx_.tolerated_faults_, ctx_.total_vehicles_) + 1;
    std::cout << "[CANCEL-ECHO] r" << ctx_.replicaId_
              << " recv signer=r" << echo.echoingReplicaId
              << " key=" << key
              << " count=" << count << "/" << threshold << "\n";

    if ((int)count >= threshold && !rollback_.cancel_cert_seen_.count(key)) {
        CancelCert cert;
        cert.cancelledEpoch = echo.cancelledEpoch;
        cert.reason = echo.reason;
        cert.reasonRef = echo.reasonRef;
        for (const auto& we : *bucket)
            cert.echoes.push_back(toCancelEcho(we, echo.cancelledEpoch, echo.reason, echo.reasonRef));
        std::cout << "[CANCEL-CERT-GATE] r" << ctx_.replicaId_
                  << " action=broadcast key=" << key
                  << " count=" << count
                  << " threshold=" << threshold
                  << " seen=0\n";
        broadcastCancelCert(cert);
    } else if ((int)count >= threshold) {
        std::cout << "[CANCEL-CERT-GATE] r" << ctx_.replicaId_
                  << " action=skip key=" << key
                  << " count=" << count
                  << " threshold=" << threshold
                  << " seen=" << (rollback_.cancel_cert_seen_.count(key) ? 1 : 0)
                  << "\n";
    }
}

// Fast-start exponential backoff (spec §11.1/§11.2): min(base*factor^k, cap)
// plus the existing broadcast jitter. Saturating — pow() on a bounded attempt
// count against a capped result can't overflow into anything unreasonable.
double ResDBIntersectionApp::backoffDelaySec(double baseSec, double capSec, int attempt) const
{
    double delay = baseSec * std::pow(evidence_retry_factor_, static_cast<double>(std::max(attempt, 0)));
    if (delay > capSec) delay = capSec;
    const double jmin = par("broadcastJitterMin").doubleValue();
    const double jmax = par("broadcastJitterMax").doubleValue();
    return delay + ((jmax > jmin) ? uniform(jmin, jmax) : 0.0);
}

bool ResDBIntersectionApp::cancelCertPropagationConfirmed(const std::string& key) const
{
    const int f = anchoredFaults(ctx_.tolerated_faults_, ctx_.total_vehicles_);
    auto it = rollback_.cancel_cert_carriers_.find(key);
    return it != rollback_.cancel_cert_carriers_.end() && (int)it->second.size() >= f + 1;
}

void ResDBIntersectionApp::scheduleNextCancelCertRetry()
{
    if (!ctx_.enableRollback_ || cancel_cert_pending_retries_.echoes.empty()) return;
    const std::string key = cancelReasonKey(cancel_cert_pending_retries_.cancelledEpoch,
                                            cancel_cert_pending_retries_.reason,
                                            cancel_cert_pending_retries_.reasonRef);
    if (cancelCertPropagationConfirmed(key)) {
        std::cout << "[CANCEL-CERT-STOP] r" << ctx_.replicaId_
                  << " key=" << key << " reason=propagation-confirmed\n";
        stopCancelCertRetries();
        return;
    }
    if (!cancel_cert_retry_timer_)
        cancel_cert_retry_timer_ = new cMessage("resdbCancelCertRetry");
    const double delay = backoffDelaySec(evidence_retry_base_sec_, evidence_retry_cap_sec_,
                                         cancel_cert_retry_count_);
    scheduleAt(simTime() + delay, cancel_cert_retry_timer_);
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

const char* ResDBIntersectionApp::cancelStateName() const
{
    switch (cancel_state_) {
    case CancelState::INACTIVE: return "INACTIVE";
    case CancelState::WITNESSING: return "WITNESSING";
    case CancelState::DRAINING: return "DRAINING";
    case CancelState::CONSENSUS: return "CONSENSUS";
    case CancelState::COMMITTED: return "COMMITTED";
    }
    return "UNKNOWN";
}

void ResDBIntersectionApp::beginCancelDrain(const char* reason)
{
    if (!cancel_consensus_pending_ || cancel_state_ == CancelState::COMMITTED)
        return;
    if (cancel_state_ == CancelState::DRAINING ||
            cancel_state_ == CancelState::CONSENSUS)
        return;

    cancel_state_ = CancelState::DRAINING;
    cancel_active_batch_ = perceivedActiveBatch();
    cancel_leader_candidates_ =
        cancelLeaderCandidatesForBatch(cancel_active_batch_);
    cancel_primary_ = cancel_leader_candidates_.empty()
        ? -1
        : cancel_leader_candidates_.front();

    // A validated f+1 CANCEL certificate closes witnessing for this event.
    // Keep bounded CANCEL_CERT retries active so replicas that missed the
    // first broadcast can enter this state too. Existing staggered ANN/ECHO
    // frames may still reach the NIC; the drain horizon below lets them finish
    // without admitting more discovery traffic.
    if (broadcastArrivalAnnouncement_timer_) {
        if (broadcastArrivalAnnouncement_timer_->isScheduled())
            cancelEvent(broadcastArrivalAnnouncement_timer_);
        delete broadcastArrivalAnnouncement_timer_;
        broadcastArrivalAnnouncement_timer_ = nullptr;
    }

    const double slot = std::max(
        par("viewAgreementSlotSec").doubleValue(),
        par("arrivalSlotSec").doubleValue());
    const double jitter = std::max(
        par("viewJitterMax").doubleValue(),
        par("broadcastJitterMax").doubleValue());
    const simtime_t drainFor = SimTime(
        std::max(0.05, (std::max(ctx_.total_vehicles_ - 1, 0) * slot) + jitter + 0.025));

    if (!cancel_drain_timer_) cancel_drain_timer_ = new cMessage("cancelDrainTimer");
    if (cancel_drain_timer_->isScheduled()) cancelEvent(cancel_drain_timer_);
    scheduleAt(simTime() + drainFor, cancel_drain_timer_);

    std::cout << "[CANCEL-DRAIN] r" << ctx_.replicaId_
              << " state=" << cancelStateName()
              << " reason=" << (reason ? reason : "valid-cert")
              << " active_batch=" << cancel_active_batch_
              << " leader_candidates=";
    for (size_t i = 0; i < cancel_leader_candidates_.size(); ++i) {
        if (i) std::cout << ",";
        std::cout << "r" << cancel_leader_candidates_[i];
    }
    std::cout << " cancel_primary=r" << cancel_primary_
              << " drain_for=" << drainFor
              << " deadline=" << simTime() + drainFor
              << " t=" << simTime() << "\n";
}

void ResDBIntersectionApp::finishCancelDrain()
{
    if (!cancel_consensus_pending_ || cancel_state_ != CancelState::DRAINING)
        return;
    cancel_state_ = CancelState::CONSENSUS;
    std::cout << "[CANCEL-CONSENSUS] r" << ctx_.replicaId_
              << " active_batch=" << cancel_active_batch_
              << " proposer=r" << cancel_primary_
              << " t=" << simTime() << "\n";
    trySubmitCancelProposal("cancel-drain-complete");
}

void ResDBIntersectionApp::broadcastCancelCert(const CancelCert& cert)
{
    std::string key = cancelReasonKey(cert.cancelledEpoch, cert.reason, cert.reasonRef);
    if (!validateCancelCert(cert)) {
        std::cout << "[CANCEL-CERT-GATE] r" << ctx_.replicaId_
                  << " action=invalid key=" << key
                  << " echoes=" << cert.echoes.size() << "\n";
        return;
    }
    registerBlockedIncidentIfCrash(cert);
    rollback_.cancel_cert_seen_.insert(key);
    std::vector<uint8_t> payload = serializeCancelCert(cert);
    sendBFTMessage(-1, payload, kCancelCertType);
    std::cout << "[CANCEL-CERT] r" << ctx_.replicaId_
              << " broadcast key=" << key
              << " echoes=" << cert.echoes.size() << "\n";
    cancel_cert_pending_retries_ = cert;
    cancel_cert_retry_count_ = 0;
    scheduleNextCancelCertRetry();
    handleValidCancelJustification(cert.cancelledEpoch, cert.reason, cert.reasonRef, payload);
}

void ResDBIntersectionApp::handleCancelCert(BFTMessage* msg)
{
    if (!ctx_.enableRollback_) return;
    CancelCert cert = deserializeCancelCert(msg);
    if (cert.reasonRef.empty()) return;
    if (!validateCancelCert(cert)) {
        std::cout << "[CANCEL-CERT] r" << ctx_.replicaId_
                  << " dropped invalid cert from r" << msg->getFromReplicaId() << "\n";
        return;
    }
    registerBlockedIncidentIfCrash(cert);

    std::string key = cancelReasonKey(cert.cancelledEpoch, cert.reason, cert.reasonRef);
    std::vector<uint8_t> payload = serializeCancelCert(cert);
    // Track distinct carriers regardless of firstSeen/relayed state, so my
    // own broadcastCancelCert retry loop can observe propagation even after
    // I've already seen/relayed this cert myself.
    rollback_.cancel_cert_carriers_[key].insert(msg->getFromReplicaId());
    const bool firstSeen = rollback_.cancel_cert_seen_.insert(key).second;
    if (rollback_.cancel_cert_relayed_.insert(key).second) {
        sendBFTMessage(-1, payload, kCancelCertType);
        std::cout << "[CANCEL-RELAY] r" << ctx_.replicaId_
                  << " key=" << key << " from=r" << msg->getFromReplicaId() << "\n";
    }
    if (firstSeen || !cancel_consensus_pending_)
        handleValidCancelJustification(cert.cancelledEpoch, cert.reason, cert.reasonRef, payload);
}

// ── Clear protocol (Types 15, 16) ────────────────────────────────────────────
// Structurally identical f+1 physical-evidence certificate to BLOCKED (§9.2),
// reusing the same shared WitnessCertificateValidator/WitnessEchoCollector
// machinery. Types 15/16 use thin serializers of their own rather than
// reusing the type-12/13 CANCEL_ECHO/CANCEL_CERT wire format.

WitnessEcho ResDBIntersectionApp::toWitnessEcho(const ClearEcho& e)
{
    WitnessEcho we;
    we.signerId = e.echoingReplicaId;
    std::memcpy(we.pubKey, e.signerPubKey, CRYPTO_PUBKEY_BYTES);
    std::memcpy(we.sig, e.signature, CRYPTO_SIG_MAX_BYTES);
    we.sigLen = e.signatureLen;
    return we;
}

ClearEcho ResDBIntersectionApp::toClearEcho(
    const WitnessEcho& we, uint32_t cancelledEpoch, uint32_t executingBatch)
{
    ClearEcho e;
    e.echoingReplicaId = we.signerId;
    e.cancelledEpoch = cancelledEpoch;
    e.executingBatch = executingBatch;
    std::memcpy(e.signerPubKey, we.pubKey, CRYPTO_PUBKEY_BYTES);
    std::memcpy(e.signature, we.sig, CRYPTO_SIG_MAX_BYTES);
    e.signatureLen = we.sigLen;
    return e;
}

std::vector<uint8_t> ResDBIntersectionApp::serializeClearEcho(const ClearEcho& echo) const
{
    std::vector<uint8_t> pubVec(echo.signerPubKey,
                                echo.signerPubKey + CRYPTO_PUBKEY_BYTES);
    std::vector<uint8_t> sigVec(echo.signature,
                                echo.signature + echo.signatureLen);
    std::stringstream ss;
    ss << "CLEAR_ECHO|"
       << echo.cancelledEpoch << "|"
       << echo.executingBatch << "|"
       << echo.echoingReplicaId << "|"
       << toHex(pubVec) << "," << toHex(sigVec);
    std::string s = ss.str();
    return std::vector<uint8_t>(s.begin(), s.end());
}

ClearEcho
ResDBIntersectionApp::deserializeClearEcho(BFTMessage* msg) const
{
    std::vector<uint8_t> payload = payloadBytes(msg);
    std::string s(payload.begin(), payload.end());
    auto parts = splitStr(s, '|');
    ClearEcho echo;
    std::memset(echo.signerPubKey, 0, CRYPTO_PUBKEY_BYTES);
    std::memset(echo.signature, 0, CRYPTO_SIG_MAX_BYTES);
    if (parts.size() >= 5 && parts[0] == "CLEAR_ECHO") {
        echo.cancelledEpoch = static_cast<uint32_t>(std::stoul(parts[1]));
        echo.executingBatch = static_cast<uint32_t>(std::stoul(parts[2]));
        echo.echoingReplicaId = std::stoi(parts[3]);
        size_t comma = parts[4].find(',');
        if (comma != std::string::npos) {
            auto pubVec = fromHex(parts[4].substr(0, comma));
            auto sigVec = fromHex(parts[4].substr(comma + 1));
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

std::vector<uint8_t> ResDBIntersectionApp::serializeClearCert(const ClearCert& cert) const
{
    std::stringstream ss;
    ss << "CLEAR_CERT|"
       << cert.cancelledEpoch << "|"
       << cert.executingBatch;
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

ClearCert
ResDBIntersectionApp::deserializeClearCert(BFTMessage* msg) const
{
    std::vector<uint8_t> payload = payloadBytes(msg);
    return deserializeClearCert(payload.data(), (uint32_t)payload.size());
}

ClearCert
ResDBIntersectionApp::deserializeClearCert(const uint8_t* data, uint32_t len) const
{
    std::string s(reinterpret_cast<const char*>(data), len);
    auto parts = splitStr(s, '|');
    ClearCert cert;
    if (parts.size() < 3 || parts[0] != "CLEAR_CERT") return cert;
    cert.cancelledEpoch = static_cast<uint32_t>(std::stoul(parts[1]));
    cert.executingBatch = static_cast<uint32_t>(std::stoul(parts[2]));
    for (size_t i = 3; i < parts.size(); ++i) {
        size_t colon = parts[i].find(':');
        if (colon == std::string::npos) continue;
        size_t comma = parts[i].find(',', colon + 1);
        if (comma == std::string::npos) continue;
        ClearEcho echo;
        echo.cancelledEpoch = cert.cancelledEpoch;
        echo.executingBatch = cert.executingBatch;
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

bool ResDBIntersectionApp::validateClearCert(const ClearCert& cert) const
{
    WitnessStatement stmt{cert.cancelledEpoch, WitnessKind::CLEAR,
        formatBlockedBatchRef(cert.cancelledEpoch, cert.executingBatch)};
    std::vector<WitnessEcho> echoes;
    echoes.reserve(cert.echoes.size());
    for (const auto& echo : cert.echoes) {
        if (echo.cancelledEpoch != cert.cancelledEpoch ||
                echo.executingBatch != cert.executingBatch ||
                echo.signatureLen == 0) {
            continue;
        }
        echoes.push_back(toWitnessEcho(echo));
    }

    const int f = anchoredFaults(ctx_.tolerated_faults_, ctx_.total_vehicles_);
    // Same identity rules as BLOCKED: signers must belong to the committed
    // view M_e; the incident subject is never checked here.
    std::lock_guard<std::mutex> lk(committed_view_mutex_);
    WitnessCertificateValidator validator(f, &ctx_.committed_order_vehicle_ids_);
    return validator.validate(stmt, echoes);
}

/*static*/ int ResDBIntersectionApp::clearEvidenceCallback(
    void* ctx, uint32_t cancelledEpoch, const uint8_t* certBytes, uint32_t certLen)
{
    auto* app = static_cast<ResDBIntersectionApp*>(ctx);
    if (!app || !certBytes || certLen == 0) return 0;
    ClearCert cert = app->deserializeClearCert(certBytes, certLen);
    // The bridge passes cancelledEpoch opaquely (it never parses cert_bytes
    // itself, per spec §5.1) — cross-check it here against the embedded
    // statement so a trailer for the wrong epoch cannot be replayed in.
    if (cert.cancelledEpoch != cancelledEpoch) return 0;
    if (!app->enable_recovery_clear_evidence_gate_) {
        std::cout << "[CLEAR-EVIDENCE-GATE] r" << app->ctx_.replicaId_
                  << " action=bypass"
                  << " cancelled_epoch=" << cancelledEpoch
                  << " batch=" << cert.executingBatch
                  << " echoes=" << cert.echoes.size() << "\n";
        return 1;
    }
    if (cert.echoes.empty()) return 0;
    return app->validateClearCert(cert) ? 1 : 0;
}

void ResDBIntersectionApp::sendClearEcho(uint32_t cancelledEpoch, uint32_t executingBatch)
{
    if (!ctx_.enableRollback_ || !ctx_.ec_private_key_) return;
    WitnessStatement stmt{cancelledEpoch, WitnessKind::CLEAR,
        formatBlockedBatchRef(cancelledEpoch, executingBatch)};
    const std::string key = stmt.collectorKey();
    const BlockedIncident incident{cancelledEpoch, executingBatch};
    auto incidentIt = incidentRegistry_.find(incident);
    if ((incidentIt != incidentRegistry_.end() &&
            incidentIt->second.state == IncidentState::CLEARED) ||
            rollback_.clear_cert_seen_.count(key) ||
            rollback_.clear_cert_candidate_keys_.count(key)) {
        std::cout << "[CLEAR-ECHO-HUSH] r" << ctx_.replicaId_
                  << " key=" << key << " reason=already-resolved\n";
        return;
    }
    if (!rollback_.clear_echo_sent_.insert(key).second) return;

    ClearEcho echo;
    echo.echoingReplicaId = ctx_.replicaId_;
    echo.cancelledEpoch = cancelledEpoch;
    echo.executingBatch = executingBatch;
    std::memcpy(echo.signerPubKey, ctx_.ec_pub_key_, CRYPTO_PUBKEY_BYTES);

    const std::string toSign = stmt.signPayload(echo.echoingReplicaId);
    if (!CryptoAuth::instance().signBytes(
            ctx_.ec_private_key_, reinterpret_cast<const uint8_t*>(toSign.c_str()),
            toSign.size(), echo.signature, echo.signatureLen)) {
        return;
    }

    sendBFTMessage(-1, serializeClearEcho(echo), kClearEchoType);
    std::cout << "[CLEAR-ECHO] r" << ctx_.replicaId_
              << " epoch=" << cancelledEpoch
              << " batch=" << executingBatch << "\n";

    rollback_.clear_echo_collector_.add(stmt, toWitnessEcho(echo));
}

void ResDBIntersectionApp::handleClearEcho(BFTMessage* msg)
{
    if (!ctx_.enableRollback_) return;
    ClearEcho echo = deserializeClearEcho(msg);
    if (echo.echoingReplicaId < 0 || echo.signatureLen == 0) return;
    WitnessStatement stmt{echo.cancelledEpoch, WitnessKind::CLEAR,
        formatBlockedBatchRef(echo.cancelledEpoch, echo.executingBatch)};
    const std::string key = stmt.collectorKey();
    const BlockedIncident incident{echo.cancelledEpoch, echo.executingBatch};
    auto incidentIt = incidentRegistry_.find(incident);
    if ((incidentIt != incidentRegistry_.end() &&
            incidentIt->second.state == IncidentState::CLEARED) ||
            rollback_.clear_cert_seen_.count(key) ||
            rollback_.clear_cert_candidate_keys_.count(key)) {
        std::cout << "[CLEAR-ECHO-HUSH] r" << ctx_.replicaId_
                  << " key=" << key
                  << " signer=r" << echo.echoingReplicaId
                  << " reason=cert-known-or-pending\n";
        return;
    }
    const std::string toSign = stmt.signPayload(echo.echoingReplicaId);
    if (!WitnessKeyRegistry::instance().matches(
            echo.echoingReplicaId, echo.signerPubKey)) {
        std::cout << "[CLEAR-ECHO] r" << ctx_.replicaId_
                  << " dropped key-mismatched echo from r"
                  << echo.echoingReplicaId << "\n";
        return;
    }
    if (!CryptoAuth::instance().verifyBytes(
            echo.signerPubKey,
            reinterpret_cast<const uint8_t*>(toSign.c_str()), toSign.size(),
            echo.signature, echo.signatureLen)) {
        std::cout << "[CLEAR-ECHO] r" << ctx_.replicaId_
                  << " dropped invalid echo from r" << echo.echoingReplicaId << "\n";
        return;
    }

    if (!rollback_.clear_echo_collector_.add(stmt, toWitnessEcho(echo))) {
        return;
    }
    const auto* bucket = rollback_.clear_echo_collector_.get(key);
    const size_t count = bucket ? bucket->size() : 0;
    const int threshold = anchoredFaults(ctx_.tolerated_faults_, ctx_.total_vehicles_) + 1;
    std::cout << "[CLEAR-ECHO] r" << ctx_.replicaId_
              << " recv signer=r" << echo.echoingReplicaId
              << " key=" << key
              << " count=" << count << "/" << threshold << "\n";

    if ((int)count >= threshold && !rollback_.clear_cert_seen_.count(key)) {
        ClearCert cert;
        cert.cancelledEpoch = echo.cancelledEpoch;
        cert.executingBatch = echo.executingBatch;
        for (const auto& we : *bucket)
            cert.echoes.push_back(toClearEcho(we, echo.cancelledEpoch, echo.executingBatch));
        scheduleClearCertCandidate(cert);
    }
}

std::string ResDBIntersectionApp::clearSemanticKey(
    uint32_t cancelledEpoch, uint32_t executingBatch) const
{
    return "CLEAR:" + std::to_string(cancelledEpoch) + ":" +
        std::to_string(executingBatch);
}

std::vector<int> ResDBIntersectionApp::clearPropagationMembers() const
{
    std::vector<int> members;
    for (const auto& kv : ctx_.collected_certs_) {
        const int rid = extractReplicaId(kv.first);
        if (rid >= 0 && shouldIncludeInRollbackMembership(rid))
            members.push_back(rid);
    }
    std::sort(members.begin(), members.end());
    members.erase(std::unique(members.begin(), members.end()), members.end());

    // CLEAR normally starts only after post-CANCEL discovery completes. Keep
    // a conservative fallback for a partially reconstructed view so a valid
    // carrier is not rejected solely because one local ARRIVAL_CERT was lost.
    if (members.empty()) {
        for (int rid = 0; rid < ctx_.total_vehicles_; ++rid) {
            if (shouldIncludeInRollbackMembership(rid)) members.push_back(rid);
        }
    }
    return members;
}

int ResDBIntersectionApp::clearPropagationRank() const
{
    std::vector<int> members = clearPropagationMembers();
    if (members.empty()) return -1;
    const int primary = CertPrimary();
    if (primary >= 0) {
        auto it = std::find(members.begin(), members.end(), primary);
        if (it != members.end()) {
            members.erase(it);
            members.insert(members.begin(), primary);
        }
    }
    auto self = std::find(members.begin(), members.end(), ctx_.replicaId_);
    return self == members.end()
        ? -1
        : static_cast<int>(std::distance(members.begin(), self));
}

bool ResDBIntersectionApp::clearCarrierIsActiveMember(int replicaId) const
{
    const std::vector<int> members = clearPropagationMembers();
    return std::find(members.begin(), members.end(), replicaId) != members.end();
}

int ResDBIntersectionApp::clearPropagationThreshold() const
{
    return anchoredFaults(ctx_.tolerated_faults_, ctx_.total_vehicles_) + 1;
}

bool ResDBIntersectionApp::clearPropagationConfirmed(const std::string& key) const
{
    return clear_propagation_tracker_.confirmed(
        key, clearPropagationThreshold());
}

void ResDBIntersectionApp::scheduleClearCertCandidate(const ClearCert& cert)
{
    if (!ctx_.enableRollback_ || cert.echoes.empty()) return;
    const std::string key =
        clearSemanticKey(cert.cancelledEpoch, cert.executingBatch);
    const std::string collectorKey = WitnessStatement{
        cert.cancelledEpoch, WitnessKind::CLEAR,
        formatBlockedBatchRef(cert.cancelledEpoch, cert.executingBatch)
    }.collectorKey();
    if (rollback_.clear_cert_seen_.count(collectorKey) ||
            rollback_.clear_cert_candidate_keys_.count(collectorKey)) {
        return;
    }
    const int rank = clearPropagationRank();
    if (rank < 0) {
        std::cout << "[CLEAR-CERT-CANDIDATE-CANCEL] r" << ctx_.replicaId_
                  << " key=" << key << " reason=not-active-member\n";
        return;
    }

    rollback_.clear_cert_candidate_keys_.insert(collectorKey);
    clear_cert_candidate_ = cert;
    rollback_.clear_cert_candidate_key_ = key;
    rollback_.clear_cert_candidate_rank_ = rank;
    if (!clear_cert_candidate_timer_)
        clear_cert_candidate_timer_ = new cMessage("resdbClearCertCandidate");
    if (clear_cert_candidate_timer_->isScheduled())
        cancelEvent(clear_cert_candidate_timer_);
    const simtime_t delay = SimTime(
        rank * std::max(0.0, clear_cert_candidate_slot_sec_));
    scheduleAt(simTime() + delay, clear_cert_candidate_timer_);
    std::cout << "[CLEAR-CERT-CANDIDATE] r" << ctx_.replicaId_
              << " key=" << key
              << " rank=" << rank
              << " fire_at=" << simTime() + delay << "\n";
}

void ResDBIntersectionApp::cancelClearCertCandidate(const char* reason)
{
    const bool active = !clear_cert_candidate_.echoes.empty();
    if (clear_cert_candidate_timer_ &&
            clear_cert_candidate_timer_->isScheduled()) {
        cancelEvent(clear_cert_candidate_timer_);
    }
    if (active) {
        std::cout << "[CLEAR-CERT-CANDIDATE-CANCEL] r" << ctx_.replicaId_
                  << " key=" << rollback_.clear_cert_candidate_key_
                  << " rank=" << rollback_.clear_cert_candidate_rank_
                  << " reason=" << (reason ? reason : "cancelled") << "\n";
    }
    clear_cert_candidate_ = ClearCert{};
    rollback_.clear_cert_candidate_key_.clear();
    rollback_.clear_cert_candidate_rank_ = -1;
}

void ResDBIntersectionApp::sendClearCertCarrier(
    const ClearCert& cert, const char* marker)
{
    const std::vector<uint8_t> raw = serializeClearCert(cert);
    auto signedPayload = resdbwire::packSignedPacket(
        ctx_.ec_private_key_, ctx_.ec_pub_key_, raw.data(), static_cast<uint32_t>(raw.size()));
    if (signedPayload.empty()) return;
    sendBFTMessage(-1, signedPayload, kClearCertType);
    std::cout << "[" << (marker ? marker : "CLEAR-CERT") << "] r" << ctx_.replicaId_
              << " key=" << clearSemanticKey(
                     cert.cancelledEpoch, cert.executingBatch)
              << " echoes=" << cert.echoes.size() << "\n";
}

void ResDBIntersectionApp::scheduleClearCertRelay(
    const ClearCert& cert, const std::string& key)
{
    if (clearPropagationConfirmed(key) || ctx_.order_applied_ ||
            ctx_.propose_submitted_) {
        std::cout << "[CLEAR-PROPAGATION-STOP] r" << ctx_.replicaId_
                  << " key=" << key
                  << " count=" << clear_propagation_tracker_.count(key)
                  << "/" << clearPropagationThreshold()
                  << " reason=" << (ctx_.order_applied_ ? "order-applied" :
                      ctx_.propose_submitted_ ? "order-proposed" :
                      "propagation-confirmed") << "\n";
        return;
    }
    if (!rollback_.clear_cert_relayed_.insert(key).second) return;
    const int rank = clearPropagationRank();
    if (rank < 0) return;
    clear_cert_pending_relay_ = cert;
    clear_cert_pending_relay_key_ = key;
    if (!clear_cert_relay_timer_)
        clear_cert_relay_timer_ = new cMessage("resdbClearCertRelay");
    if (clear_cert_relay_timer_->isScheduled())
        cancelEvent(clear_cert_relay_timer_);
    const simtime_t delay = SimTime(
        (rank + 1) * std::max(0.0, clear_cert_candidate_slot_sec_));
    scheduleAt(simTime() + delay, clear_cert_relay_timer_);
    std::cout << "[CLEAR-CERT-CANDIDATE] r" << ctx_.replicaId_
              << " key=" << key
              << " rank=" << rank
              << " role=relay"
              << " fire_at=" << simTime() + delay << "\n";
}

void ResDBIntersectionApp::cancelClearCertRelay(const char* reason)
{
    const bool active = !clear_cert_pending_relay_.echoes.empty();
    if (clear_cert_relay_timer_ && clear_cert_relay_timer_->isScheduled())
        cancelEvent(clear_cert_relay_timer_);
    if (active) {
        std::cout << "[CLEAR-PROPAGATION-STOP] r" << ctx_.replicaId_
                  << " key=" << clear_cert_pending_relay_key_
                  << " count=" << clear_propagation_tracker_.count(
                         clear_cert_pending_relay_key_)
                  << "/" << clearPropagationThreshold()
                  << " reason=" << (reason ? reason : "cancelled") << "\n";
    }
    clear_cert_pending_relay_ = ClearCert{};
    clear_cert_pending_relay_key_.clear();
}

void ResDBIntersectionApp::scheduleNextClearCertRetry()
{
    if (!ctx_.enableRollback_ || clear_cert_pending_retries_.echoes.empty()) return;
    if (!clear_cert_retry_timer_)
        clear_cert_retry_timer_ = new cMessage("resdbClearCertRetry");
    const double delay = backoffDelaySec(evidence_retry_base_sec_, evidence_retry_cap_sec_,
                                         clear_cert_retry_count_);
    scheduleAt(simTime() + delay, clear_cert_retry_timer_);
}

void ResDBIntersectionApp::stopClearCertRetries()
{
    if (clear_cert_retry_timer_) {
        if (clear_cert_retry_timer_->isScheduled()) cancelEvent(clear_cert_retry_timer_);
        delete clear_cert_retry_timer_;
        clear_cert_retry_timer_ = nullptr;
    }
    clear_cert_pending_retries_.echoes.clear();
    clear_cert_retry_count_ = 0;
}

bool ResDBIntersectionApp::hasBlockingIncidentForEpoch(uint32_t cancelledEpoch) const
{
    for (const auto& kv : incidentRegistry_) {
        if (kv.first.cancelledEpoch != cancelledEpoch) continue;
        if (kv.second.state != IncidentState::CLEARED) return true;
    }
    return false;
}

void ResDBIntersectionApp::onIncidentCleared(
    const BlockedIncident& incident, const std::vector<uint8_t>& clearCertBytes)
{
    auto it = incidentRegistry_.find(incident);
    if (it == incidentRegistry_.end() || it->second.state == IncidentState::CLEARED)
        return;
    it->second.state = IncidentState::CLEARED;
    it->second.clearCertBytes = clearCertBytes;
    std::cout << "[INCIDENT-REGISTER] r" << ctx_.replicaId_
              << " state=CLEARED epoch=" << incident.cancelledEpoch
              << " batch=" << incident.executingBatch << "\n";
    // Stop retrying my own CLEAR_CERT broadcast the moment the incident is
    // known CLEARED, rather than waiting for the next scheduled retry tick
    // to notice — same "go quiet as soon as your job is done" principle as
    // the CANCEL-commit gossip fix below.
    stopClearCertRetries();
    // Spec §8.4: valid CLEAR always supersedes WAIT — cancel the local
    // deferral immediately. Future WAIT for this incident is rejected
    // naturally by handleWaitHeartbeat's BLOCKING check now that the
    // incident is CLEARED, so no extra "ignore future WAIT" flag is needed.
    cancelClearCertCandidate("incident-cleared");
    stopWait("clear-cert");
    if (ctx_.cancel_pending_ && cancelled_epoch_ == incident.cancelledEpoch)
        evaluateOrderReadiness("clear-cert");
}

void ResDBIntersectionApp::broadcastClearCert(const ClearCert& cert)
{
    if (!validateClearCert(cert)) {
        std::cout << "[CLEAR-CERT-GATE] r" << ctx_.replicaId_
                  << " action=invalid epoch=" << cert.cancelledEpoch
                  << " batch=" << cert.executingBatch
                  << " echoes=" << cert.echoes.size() << "\n";
        return;
    }
    WitnessStatement stmt{cert.cancelledEpoch, WitnessKind::CLEAR,
        formatBlockedBatchRef(cert.cancelledEpoch, cert.executingBatch)};
    const std::string key = stmt.collectorKey();
    rollback_.clear_cert_seen_.insert(key);
    std::vector<uint8_t> payload = serializeClearCert(cert);
    const std::string semanticKey =
        clearSemanticKey(cert.cancelledEpoch, cert.executingBatch);
    clear_propagation_tracker_.observeAuthenticated(semanticKey, ctx_.replicaId_);
    std::cout << "[CLEAR-CARRIER] r" << ctx_.replicaId_
              << " key=" << semanticKey
              << " carrier=r" << ctx_.replicaId_
              << " count=" << clear_propagation_tracker_.count(semanticKey)
              << "/" << clearPropagationThreshold()
              << " source=local-broadcast\n";
    sendClearCertCarrier(cert, "CLEAR-CARRIER-SEND");
    std::cout << "[CLEAR-CERT] r" << ctx_.replicaId_
              << " broadcast key=" << semanticKey
              << " epoch=" << cert.cancelledEpoch
              << " batch=" << cert.executingBatch
              << " echoes=" << cert.echoes.size()
              << " t=" << simTime() << "\n";
    onIncidentCleared(BlockedIncident{cert.cancelledEpoch, cert.executingBatch}, payload);
}

void ResDBIntersectionApp::handleClearCert(BFTMessage* msg)
{
    if (!ctx_.enableRollback_) return;
    std::vector<uint8_t> payload = payloadBytes(msg);
    resdbwire::SignedPacketView signedView;
    if (!resdbwire::unpackSignedPacket(
            payload.data(), static_cast<uint32_t>(payload.size()), &signedView)) {
        std::cout << "[CLEAR-CERT] r" << ctx_.replicaId_
                  << " dropped unsigned carrier from r"
                  << msg->getFromReplicaId() << "\n";
        return;
    }
    const int carrier = msg->getFromReplicaId();
    if (!clearCarrierIsActiveMember(carrier) ||
            !WitnessKeyRegistry::instance().matches(carrier, signedView.pubKey) ||
            !CryptoAuth::instance().verifyBytes(
                signedView.pubKey, signedView.resdbBytes, signedView.resdbLen,
                signedView.sig, signedView.sigLen)) {
        std::cout << "[CLEAR-CERT] r" << ctx_.replicaId_
                  << " dropped unauthenticated carrier r" << carrier << "\n";
        return;
    }
    ClearCert cert = deserializeClearCert(
        signedView.resdbBytes, signedView.resdbLen);
    if (cert.echoes.empty()) return;
    if (!validateClearCert(cert)) {
        std::cout << "[CLEAR-CERT] r" << ctx_.replicaId_
                  << " dropped invalid cert from r" << msg->getFromReplicaId() << "\n";
        return;
    }
    WitnessStatement stmt{cert.cancelledEpoch, WitnessKind::CLEAR,
        formatBlockedBatchRef(cert.cancelledEpoch, cert.executingBatch)};
    const std::string collectorKey = stmt.collectorKey();
    const std::string semanticKey =
        clearSemanticKey(cert.cancelledEpoch, cert.executingBatch);
    const bool newCarrier =
        clear_propagation_tracker_.observeAuthenticated(semanticKey, carrier);
    if (newCarrier) {
        std::cout << "[CLEAR-CARRIER] r" << ctx_.replicaId_
                  << " key=" << semanticKey
                  << " carrier=r" << carrier
                  << " count=" << clear_propagation_tracker_.count(semanticKey)
                  << "/" << clearPropagationThreshold() << "\n";
    }
    const bool firstSeen = rollback_.clear_cert_seen_.insert(collectorKey).second;
    cancelClearCertCandidate(firstSeen ? "valid-cert-received" :
        "additional-carrier-received");
    std::vector<uint8_t> rawCert(
        signedView.resdbBytes, signedView.resdbBytes + signedView.resdbLen);
    onIncidentCleared(
        BlockedIncident{cert.cancelledEpoch, cert.executingBatch}, rawCert);

    if (clearPropagationConfirmed(semanticKey)) {
        cancelClearCertRelay("propagation-confirmed");
        return;
    }
    scheduleClearCertRelay(cert, semanticKey);
}

// ── WAIT advisory heartbeat (Type 17) ────────────────────────────────────────
// Spec §8: one signed message from the ordinary next-epoch certificate
// primary while the incident is BLOCKING, purely to delay local
// leader-suspicion timers. Not PBFT, not an f+1 certificate, no quorum.

bool ResDBIntersectionApp::waitConditionsHold(BlockedIncident* outIncident) const
{
    if (!ctx_.enableRollback_ || !ctx_.cancel_pending_) return false;
    if (ctx_.discovery_.state != DiscoveryState::COMPLETE || ctx_.discovery_.epoch != rollback_new_epoch_)
        return false;
    // Spec invariant 9: one executing batch maps to one blocked incident per
    // cancelled epoch — so at most one match is expected here.
    for (const auto& kv : incidentRegistry_) {
        if (kv.first.cancelledEpoch != cancelled_epoch_) continue;
        if (kv.second.state != IncidentState::BLOCKING) continue;
        if (outIncident) *outIncident = kv.first;
        return true;
    }
    return false;
}

void ResDBIntersectionApp::sendWaitHeartbeat(const BlockedIncident& incident)
{
    if (!ctx_.ec_private_key_) return;
    WaitHeartbeatPayload p{};
    p.magic = kWaitHeartbeatMagic;
    p.version = 1;
    p.cancelledEpoch = incident.cancelledEpoch;
    p.executingBatch = incident.executingBatch;
    p.leaderId = ctx_.replicaId_;
    p.heartbeatIndex = rollback_.wait_leader_heartbeat_index_++;
    p.sentAtSimUs = static_cast<uint64_t>(simTime().inUnit(SIMTIME_US));
    p.validUntilSimUs = static_cast<uint64_t>(
        (simTime() + wait_heartbeat_max_deferral_sec_).inUnit(SIMTIME_US));

    std::vector<uint8_t> raw(sizeof(p));
    std::memcpy(raw.data(), &p, sizeof(p));
    auto signed_payload = resdbwire::packSignedPacket(
        ctx_.ec_private_key_, ctx_.ec_pub_key_, raw.data(), (uint32_t)raw.size());
    if (signed_payload.empty()) return;

    sendBFTMessage(-1, signed_payload, kWaitHeartbeatType);
    std::cout << "[WAIT-SEND] r" << ctx_.replicaId_
              << " epoch=" << incident.cancelledEpoch
              << " batch=" << incident.executingBatch
              << " index=" << p.heartbeatIndex
              << " valid_until_us=" << p.validUntilSimUs
              << " t=" << simTime() << "\n";
}

void ResDBIntersectionApp::maybeSendWaitHeartbeat(const char* reason)
{
    BlockedIncident incident;
    const bool conditionsHold = waitConditionsHold(&incident);
    const bool amLeader = conditionsHold && currentOrderPrimary() == ctx_.replicaId_;

    if (!amLeader) {
        if (wait_leader_send_timer_ && wait_leader_send_timer_->isScheduled())
            cancelEvent(wait_leader_send_timer_);
        rollback_.wait_leader_active_ = false;
        return;
    }

    // rollback_.wait_leader_active_ (not timer->isScheduled()) marks a fresh
    // activation: a self-message that just fired reports isScheduled()==
    // false until it is rescheduled below, so using the timer's scheduled
    // state here would (and did) misdetect every periodic tick as a fresh
    // start and reset the counter back to 0 every time. Reset the
    // per-incident heartbeat counter only on genuine (re)activation (spec:
    // indices increase monotonically per incident, not globally).
    if (!rollback_.wait_leader_active_) {
        rollback_.wait_leader_heartbeat_index_ = 0;
        rollback_.wait_leader_active_ = true;
    }

    sendWaitHeartbeat(incident);

    if (!wait_leader_send_timer_) wait_leader_send_timer_ = new cMessage("resdbWaitLeaderSend");
    if (wait_leader_send_timer_->isScheduled()) cancelEvent(wait_leader_send_timer_);
    scheduleAt(simTime() + wait_heartbeat_interval_sec_, wait_leader_send_timer_);
}

void ResDBIntersectionApp::handleWaitHeartbeat(BFTMessage* msg)
{
    if (!ctx_.enableRollback_) return;
    std::vector<uint8_t> payload = payloadBytes(msg);
    resdbwire::SignedPacketView view;
    if (!resdbwire::unpackSignedPacket(payload.data(), (uint32_t)payload.size(), &view)) return;
    if (view.resdbLen != sizeof(WaitHeartbeatPayload)) return;
    WaitHeartbeatPayload p{};
    std::memcpy(&p, view.resdbBytes, sizeof(p));
    if (p.magic != kWaitHeartbeatMagic || p.version != 1) return;

    // Check 1: signature valid and bound to the claimed leaderId via the key
    // registry (not just "signed by someone").
    if (!WitnessKeyRegistry::instance().matches(p.leaderId, view.pubKey)) {
        std::cout << "[WAIT-REJECT] r" << ctx_.replicaId_
                  << " reason=key-mismatch claimed_leader=" << p.leaderId << "\n";
        return;
    }
    if (!CryptoAuth::instance().verifyBytes(view.pubKey, view.resdbBytes, view.resdbLen,
                                            view.sig, view.sigLen)) {
        std::cout << "[WAIT-REJECT] r" << ctx_.replicaId_ << " reason=bad-signature\n";
        return;
    }
    // Check 2: CANCEL for cancelledEpoch committed/adopted.
    if (!isEpochTombstoned(p.cancelledEpoch)) {
        std::cout << "[WAIT-REJECT] r" << ctx_.replicaId_
                  << " reason=cancel-not-committed epoch=" << p.cancelledEpoch << "\n";
        return;
    }
    // Check 3/4: a matching incident that is locally still BLOCKING (not
    // CLEARED — CLEAR always supersedes a late WAIT for the same incident).
    const BlockedIncident incident{p.cancelledEpoch, p.executingBatch};
    auto incIt = incidentRegistry_.find(incident);
    if (incIt == incidentRegistry_.end() || incIt->second.state != IncidentState::BLOCKING) {
        std::cout << "[WAIT-REJECT] r" << ctx_.replicaId_
                  << " reason=no-matching-blocking-incident"
                  << " epoch=" << p.cancelledEpoch << " batch=" << p.executingBatch << "\n";
        return;
    }
    // Check 5: no ORDER(cancelledEpoch+1) committed/applied.
    if (ctx_.order_applied_ && ctx_.current_epoch_ == p.cancelledEpoch + 1) {
        std::cout << "[WAIT-REJECT] r" << ctx_.replicaId_ << " reason=order-already-applied\n";
        return;
    }
    // Check 6: sender must equal my own locally-computed ordinary
    // CertPrimary() for the completed epoch-(e+1) discovery view.
    if (ctx_.discovery_.state != DiscoveryState::COMPLETE || ctx_.discovery_.epoch != p.cancelledEpoch + 1) {
        std::cout << "[WAIT-REJECT] r" << ctx_.replicaId_ << " reason=discovery-not-complete\n";
        return;
    }
    const int expectedLeader = currentOrderPrimary();
    if (p.leaderId != expectedLeader) {
        std::cout << "[WAIT-REJECT] r" << ctx_.replicaId_
                  << " reason=wrong-leader claimed=" << p.leaderId
                  << " expected=" << expectedLeader << "\n";
        return;
    }
    // Check 7: strictly increasing heartbeat index from this leader/incident.
    const bool sameLeaderIncident = wait_follower_state_.active &&
        wait_follower_state_.cancelledEpoch == p.cancelledEpoch &&
        wait_follower_state_.executingBatch == p.executingBatch &&
        wait_follower_state_.leaderId == p.leaderId;
    if (sameLeaderIncident && p.heartbeatIndex <= wait_follower_state_.lastHeartbeatIndex) {
        std::cout << "[WAIT-REJECT] r" << ctx_.replicaId_
                  << " reason=stale-index index=" << p.heartbeatIndex
                  << " last=" << wait_follower_state_.lastHeartbeatIndex << "\n";
        return;
    }
    // Check 8: sentAtSimUs within the configured clock-skew allowance (a
    // heartbeat claiming to be from the future is rejected).
    const simtime_t sentAt = SimTime(static_cast<int64_t>(p.sentAtSimUs), SIMTIME_US);
    if (sentAt > simTime() + wait_clock_skew_sec_) {
        std::cout << "[WAIT-REJECT] r" << ctx_.replicaId_ << " reason=future-timestamp\n";
        return;
    }
    // Check 9/10: valid, bounded, non-expired lease.
    const simtime_t validUntil = SimTime(static_cast<int64_t>(p.validUntilSimUs), SIMTIME_US);
    if (validUntil <= sentAt) {
        std::cout << "[WAIT-REJECT] r" << ctx_.replicaId_ << " reason=bad-lease-window\n";
        return;
    }
    if ((validUntil - sentAt).dbl() > wait_heartbeat_max_deferral_sec_ + 1e-6) {
        std::cout << "[WAIT-REJECT] r" << ctx_.replicaId_ << " reason=excessive-deferral\n";
        return;
    }
    if (validUntil <= simTime()) {
        std::cout << "[WAIT-REJECT] r" << ctx_.replicaId_ << " reason=already-expired\n";
        return;
    }

    // Accepted — reschedule the local recovery-leader suspicion deferral.
    wait_follower_state_.cancelledEpoch = p.cancelledEpoch;
    wait_follower_state_.executingBatch = p.executingBatch;
    wait_follower_state_.leaderId = p.leaderId;
    wait_follower_state_.lastHeartbeatIndex = p.heartbeatIndex;
    wait_follower_state_.validUntil = validUntil;
    wait_follower_state_.active = true;

    if (!wait_follower_expiry_timer_)
        wait_follower_expiry_timer_ = new cMessage("resdbWaitFollowerExpiry");
    if (wait_follower_expiry_timer_->isScheduled()) cancelEvent(wait_follower_expiry_timer_);
    scheduleAt(validUntil, wait_follower_expiry_timer_);

    std::cout << "[WAIT-ACCEPT] r" << ctx_.replicaId_
              << " leader=r" << p.leaderId
              << " epoch=" << p.cancelledEpoch
              << " batch=" << p.executingBatch
              << " index=" << p.heartbeatIndex
              << " valid_until=" << validUntil << "\n";
}

void ResDBIntersectionApp::stopWait(const char* reason)
{
    if (wait_leader_send_timer_ && wait_leader_send_timer_->isScheduled())
        cancelEvent(wait_leader_send_timer_);
    if (wait_follower_expiry_timer_ && wait_follower_expiry_timer_->isScheduled())
        cancelEvent(wait_follower_expiry_timer_);
    if (wait_follower_state_.active || rollback_.wait_leader_active_) {
        std::cout << "[WAIT-STOP] r" << ctx_.replicaId_
                  << " reason=" << (reason ? reason : "")
                  << " t=" << simTime() << "\n";
    }
    wait_follower_state_ = WaitHeartbeatState{};
    rollback_.wait_leader_active_ = false;
}

bool ResDBIntersectionApp::isRecallable()
{
    if (ctx_.current_phase_ == ConsensusPhase::DEPARTED || ctx_.is_departed_) return false;
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
        preceding_batch_poll_msg_ &&
        preceding_batch_poll_msg_->isScheduled();
    bool recallable = (!inOrPastConflict && waitingForPriorBatch) ||
        (!inOrPastConflict &&
         (alreadyStopped || dist > brakingDistance + processing_latency_margin_));
    std::cout << "[HALT-LOCAL] r" << ctx_.replicaId_
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
    if (!ctx_.enableRollback_) {
        std::cout << "[CANCEL-JUSTIFY-GATE] r" << ctx_.replicaId_
                  << " action=skip reason=rollback-disabled"
                  << " cancelled_epoch=" << cancelledEpoch << "\n";
        return;
    }
    if (cancel_consensus_pending_ || ctx_.cancel_pending_) {
        if (cancelled_epoch_ == cancelledEpoch) {
            std::cout << "[CANCEL-JUSTIFY-GATE] r" << ctx_.replicaId_
                      << " action=skip reason=already-pending"
                      << " cancelled_epoch=" << cancelledEpoch
                      << " cancel_consensus_pending=" << (cancel_consensus_pending_ ? 1 : 0)
                      << " cancel_pending=" << (ctx_.cancel_pending_ ? 1 : 0)
                      << " propose_submitted=" << (cancel_propose_submitted_ ? 1 : 0)
                      << " rotation_index=" << cancel_rotation_index_
                      << " proposer=r" << chooseCancelProposer()
                      << "\n";
            return;
        }
    }
    if (isEpochTombstoned(cancelledEpoch)) {
        std::cout << "[CANCEL-JUSTIFY-GATE] r" << ctx_.replicaId_
                  << " action=skip reason=tombstoned"
                  << " cancelled_epoch=" << cancelledEpoch << "\n";
        return;
    }

    const uint32_t epoch = cancelledEpoch;
    const bool recallable = isRecallable();
    rollback_local_recallable_ = recallable;
    if (recallable && ctx_.current_phase_ != ConsensusPhase::DEPARTED) {
        if (resume_msg_ && resume_msg_->isScheduled()) cancelEvent(resume_msg_);
        if (preceding_batch_poll_msg_ && preceding_batch_poll_msg_->isScheduled()) cancelEvent(preceding_batch_poll_msg_);
        stopVehicle();
    }
    cancelled_epoch_ = epoch;
    rollback_.rollback_reason_ = reason;
    rollback_.rollback_reason_ref_ = reasonRef;
    cancel_cert_bytes_ = justification;
    cancel_consensus_pending_ = true;
    cancel_propose_submitted_ = false;
    cancel_rotation_index_ = 0;
    rollback_.cancel_leader_attack_logged_ = false;
    cancel_active_batch_ = -1;
    cancel_primary_ = -1;
    cancel_leader_candidates_.clear();
    rollback_cancel_initiated_ = true;
    stopGossip();

    std::cout << "[HALT-LOCAL] r" << ctx_.replicaId_
              << " valid_cancel epoch=" << epoch
              << " reason=" << static_cast<int>(reason)
              << " recallable=" << (recallable ? 1 : 0)
              << " ref=" << reasonRef
              << " cert_bytes=" << cancel_cert_bytes_.size()
              << " rotation_index=" << cancel_rotation_index_
              << " proposer=r" << chooseCancelProposer()
              << " |E|=" << cancelElectorateCandidates().size()
              << "\n";
    beginCancelDrain("cert-validated");
}

void ResDBIntersectionApp::beginPostCancelDiscovery(
    uint32_t cancelledEpoch, CancelReason reason, const std::string& reasonRef,
    const std::vector<uint8_t>& justification)
{
    if (ctx_.cancel_pending_ && cancelled_epoch_ == cancelledEpoch) return;

    // A replica can learn the committed CANCEL exclusively through commit
    // gossip without ever handling the pre-consensus CANCEL justification.
    // In that path rollback_local_recallable_ is still its default false,
    // which incorrectly excludes a late-arriving local vehicle from the new
    // epoch's discovery membership.  Refresh the physical eligibility at the
    // common post-commit transition so direct committers and gossip adopters
    // enter rollback discovery with identical local state.
    rollback_local_recallable_ = isRecallable();
    if (rollback_local_recallable_ &&
            ctx_.current_phase_ != ConsensusPhase::DEPARTED) {
        if (resume_msg_ && resume_msg_->isScheduled()) cancelEvent(resume_msg_);
        if (preceding_batch_poll_msg_ && preceding_batch_poll_msg_->isScheduled())
            cancelEvent(preceding_batch_poll_msg_);
        stopVehicle();
    }

    ctx_.cancel_pending_ = true;
    cancelled_epoch_ = cancelledEpoch;
    rollback_new_epoch_ = cancelledEpoch + 1;
    rollback_.rollback_reason_ = reason;
    rollback_.rollback_reason_ref_ = reasonRef;
    rollback_.rollback_justification_ = justification;
    rollback_.rollback_expected_membership_size_ = 0;
    cancel_rotation_index_ = 0;
    resetOrderCandidate("rollback-begin");
    rollback_.fabricated_clearance_attack_logged_ = false;
    fabricated_clearance_attack_active_ = false;

    ctx_.current_epoch_ = rollback_new_epoch_;
    ctx_.propose_submitted_ = false;
    ctx_.order_applied_ = false;
    cert_broadcast_ = false;
    stopCertBroadcastRetries();
    // Drop any leftover epoch-0 discovery frames still waiting on the stagger
    // queue before epoch-1 discovery starts filling it again.
    cancelPendingDiscoveryTxs("rollback-begin");

    {
        std::lock_guard<std::mutex> lk(certs_mutex_);
        ctx_.collected_certs_.clear();
        local_vehicle_states_.clear();
        observed_intent_cars_.clear();
    }
    my_received_echoes_.clear();
    arrival_announcements_received_.clear();
    echoed_cars_.clear();
    announcement_relay_tracker_.reset();
    cert_relay_tracker_.reset();
    pending_relays_.clear();
    cancelClearCertCandidate("rollback-begin");
    cancelClearCertRelay("rollback-begin");
    rollback_.clear_cert_candidate_keys_.clear();
    clear_propagation_tracker_.reset();

    startDiscoveryRound("cancel-committed");

    std::cout << "[ROLLBACK-BEGIN] r" << ctx_.replicaId_
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
        std::cout << "[DISCOVERY-BEGIN] r" << ctx_.replicaId_
                  << " reannounce timer armed interval="
                  << broadcast_arrival_announcement_interval_
                  << " cancelled_epoch=" << cancelled_epoch_
                  << " new_epoch=" << rollback_new_epoch_ << "\n";
    } else {
        std::cout << "[DISCOVERY-BEGIN] r" << ctx_.replicaId_
                  << " local vehicle non-recallable; not announcing into M"
                  << " cancelled_epoch=" << cancelled_epoch_
                  << " new_epoch=" << rollback_new_epoch_ << "\n";
    }
    armDiscoveryTimers("cancel-committed");

    // Re-arm the batch-clearance poll: handleValidCancelJustification()
    // cancels it (via stopVehicle()'s cleanup) while CANCEL is being
    // witnessed, with nothing scheduled to replace it. CLEAR's empty-box
    // dwell scan rides this same tick (see handleSelfMsg), so without this
    // it would never run again after CANCEL commits and CLEAR could never
    // be perceived. The batch-resume logic further down that same handler
    // stays safely inert throughout — it reschedules-and-returns without
    // calling resumeVehicle() for as long as ctx_.cancel_pending_ is true.
    //
    // All 14 replicas call this within the same tick (driven by the same
    // CANCEL-commit event), unlike crash-dwell whose per-replica timers
    // picked up natural jitter from staggered ARRIVAL_ANNOUNCE traffic. Left
    // unstaggered, every replica's dwell timer starts at the same instant,
    // so all 14 cross clearDwellSec together and broadcast CLEAR_ECHO/
    // CLEAR_CERT in the same ~100ms window — right as the proposer's
    // resulting ORDER(1) PRE_PREPARE/PREPARE/COMMIT round is also starting.
    // sendBFTMessage's own ctx_.replicaId_*broadcastSlotSec+jitter stagger (see
    // ResDBTransport.cc) only spreads each individual send by ~70ms across
    // 14 replicas — it can't fix two whole O(N^2) rounds landing in the same
    // window. Stagger the poll's first tick by ctx_.replicaId_ * one poll period
    // (same pattern, coarser slot) so dwell timers — and everything that
    // cascades from them — spread out instead of firing in lockstep.
    if (ctx_.enableRollback_) {
        if (!preceding_batch_poll_msg_)
            preceding_batch_poll_msg_ = new cMessage("resdbClearancePoll");
        if (!preceding_batch_poll_msg_->isScheduled()) {
            const double jmin = par("broadcastJitterMin").doubleValue();
            const double jmax = par("broadcastJitterMax").doubleValue();
            const double stagger = ctx_.replicaId_ * preceding_batch_poll_period_sec_ +
                ((jmax > jmin) ? uniform(jmin, jmax) : 0.0);
            scheduleAt(simTime() + preceding_batch_poll_period_sec_ + stagger,
                      preceding_batch_poll_msg_);
        }
    }
}

int ResDBIntersectionApp::minRollbackVoteN() const
{
    if (isRollbackPerEpochMode()) return kMinPerEpochRollbackVoteN;
    const int f = anchoredFaults(ctx_.tolerated_faults_, ctx_.total_vehicles_);
    return std::max(1, 3 * f + 1);
}

int ResDBIntersectionApp::minRollbackMembershipSize() const
{
    if (rollback_.rollback_expected_membership_size_ > 0)
        return std::max(minRollbackVoteN(), rollback_.rollback_expected_membership_size_);
    return minRollbackVoteN();
}

bool ResDBIntersectionApp::isRollbackPerEpochMode() const
{
    return rollback_fault_mode_per_epoch_;
}

std::vector<int> ResDBIntersectionApp::cancelElectorateCandidates() const
{
    std::vector<int> electors;
    {
        std::lock_guard<std::mutex> lk(committed_view_mutex_);
        electors.assign(ctx_.committed_order_vehicle_ids_.begin(),
                        ctx_.committed_order_vehicle_ids_.end());
    }
    // Static units are permanent CANCEL voters — they took part in epoch e's PBFT,
    // so they belong in the CANCEL forced-view electorate exactly like the cars.
    for (int uid : staticUnitReplicaIds()) electors.push_back(uid);
    std::sort(electors.begin(), electors.end());
    electors.erase(std::unique(electors.begin(), electors.end()), electors.end());
    return electors;
}

int ResDBIntersectionApp::perceivedActiveBatch() const
{
    for (size_t batch = 0; batch < committed_order_batches_.size(); ++batch) {
        for (int rid : committed_order_batches_[batch]) {
            if (!vehicleHasClearedIntersectionTraCI("veh" + std::to_string(rid)))
                return static_cast<int>(batch);
        }
    }
    return -1;
}

std::vector<int> ResDBIntersectionApp::cancelLeaderCandidatesForBatch(
    int activeBatch) const
{
    std::vector<int> candidates;
    if (activeBatch < 0) return candidates;

    for (size_t batch = static_cast<size_t>(activeBatch + 1);
            batch < committed_order_batches_.size(); ++batch) {
        std::vector<int> waiting;
        for (int rid : committed_order_batches_[batch]) {
            if (!vehicleHasClearedIntersectionTraCI("veh" + std::to_string(rid)))
                waiting.push_back(rid);
        }
        if (!waiting.empty()) {
            std::sort(waiting.begin(), waiting.end());
            return waiting;
        }
    }
    return candidates;
}

bool ResDBIntersectionApp::isEpochTombstoned(uint32_t epoch) const
{
    if (tombstoned_epochs_.count(epoch)) return true;
    auto it = rollback_.committed_cancels_.find(epoch);
    return it != rollback_.committed_cancels_.end();
}

bool ResDBIntersectionApp::hasCommittedCancel(uint32_t epoch) const
{
    return rollback_.committed_cancels_.count(epoch) > 0;
}

int ResDBIntersectionApp::chooseCancelProposer()
{
    if (cancel_leader_candidates_.empty()) return -1;
    int idx = cancel_rotation_index_ %
        static_cast<int>(cancel_leader_candidates_.size());
    cancel_primary_ = cancel_leader_candidates_[idx];
    return cancel_primary_;
}

void ResDBIntersectionApp::trySubmitCancelProposal(const char* reason)
{
    std::vector<int> electors = cancelElectorateCandidates();
    int proposer = chooseCancelProposer();
    if (cancel_state_ != CancelState::CONSENSUS) {
        std::cout << "[CANCEL-PROPOSE-GATE] r" << ctx_.replicaId_
                  << " action=skip reason=" << (reason ? reason : "try")
                  << " gate=state state=" << cancelStateName()
                  << " proposer=r" << proposer
                  << " t=" << simTime() << "\n";
        return;
    }
    if (!cancel_consensus_pending_ || cancel_propose_submitted_) {
        std::cout << "[CANCEL-PROPOSE-GATE] r" << ctx_.replicaId_
                  << " action=skip reason=" << (reason ? reason : "try")
                  << " gate="
                  << (!cancel_consensus_pending_ ? "not-pending" : "already-submitted")
                  << " pending=" << (cancel_consensus_pending_ ? 1 : 0)
                  << " submitted=" << (cancel_propose_submitted_ ? 1 : 0)
                  << " rotation_index=" << cancel_rotation_index_
                  << " proposer=r" << proposer
                  << " |E|=" << electors.size()
                  << " cert_bytes=" << cancel_cert_bytes_.size()
                  << " t=" << simTime() << "\n";
        return;
    }

    std::cout << "[CANCEL-PROPOSER-CHECK] r" << ctx_.replicaId_
              << " reason=" << (reason ? reason : "try")
              << " rotation_index=" << cancel_rotation_index_
              << " proposer=r" << proposer
              << " self_is_proposer=" << (proposer == ctx_.replicaId_ ? 1 : 0)
              << " pending=" << (cancel_consensus_pending_ ? 1 : 0)
              << " submitted=" << (cancel_propose_submitted_ ? 1 : 0)
              << " |E|=" << electors.size()
              << " cert_bytes=" << cancel_cert_bytes_.size()
              << " t=" << simTime() << "\n";

    // Every replica that has accepted the same authenticated CANCEL_CERT owns
    // the proposer lease. Previously only the selected proposer armed this
    // timer, so a proposer that suppressed proposeCancel() also suppressed the
    // only mechanism capable of replacing it. Do not reschedule an existing
    // lease: repeated certs/relays must not extend a Byzantine leader forever.
    if (enable_cancel_leader_failover_) {
        if (!cancel_vc_timer_) cancel_vc_timer_ = new cMessage("cancelVcTimer");
        if (!cancel_vc_timer_->isScheduled()) {
            scheduleAt(simTime() + rollback_vc_timeout_sec_, cancel_vc_timer_);
            std::cout << "[CANCEL-LEADER-LEASE] r" << ctx_.replicaId_
                      << " action=arm"
                      << " rotation_index=" << cancel_rotation_index_
                      << " proposer=r" << proposer
                      << " deadline=" << simTime() + rollback_vc_timeout_sec_
                      << " reason=" << (reason ? reason : "try")
                      << "\n";
        }
    }
    if (proposer != ctx_.replicaId_) {
        std::cout << "[CANCEL-PROPOSE] r" << ctx_.replicaId_
                  << " skip reason=" << (reason ? reason : "try")
                  << " proposer=r" << proposer
                  << " rotation_index=" << cancel_rotation_index_
                  << " |E|=" << electors.size()
                  << " cert_bytes=" << cancel_cert_bytes_.size()
                  << " t=" << simTime() << "\n";
        return;
    }
    if (inject_suppress_initial_cancel_leader_ &&
            cancel_rotation_index_ == 0) {
        if (!rollback_.cancel_leader_attack_logged_) {
            rollback_.cancel_leader_attack_logged_ = true;
            std::cout << "[BYZANTINE-CANCEL-SUPPRESS] r" << ctx_.replicaId_
                      << " cancelled_epoch=" << cancelled_epoch_
                      << " rotation_index=0"
                      << " failover=" << (enable_cancel_leader_failover_ ? 1 : 0)
                      << " attack_time=" << simTime()
                      << " action=suppress-proposal\n";
        }
        return;
    }
    if (!rollback_local_recallable_) {
        std::cout << "[CANCEL-PROPOSE-GATE] r" << ctx_.replicaId_
                  << " action=skip reason=local-non-recallable"
                  << " active_batch=" << perceivedActiveBatch()
                  << " proposer=r" << proposer << "\n";
        return;
    }
    proposeCancel();
}

void ResDBIntersectionApp::proposeCancel()
{
    if (!cancel_consensus_pending_ || cancel_propose_submitted_) {
        std::cout << "[CANCEL-PROPOSE-GATE] r" << ctx_.replicaId_
                  << " action=abort-propose gate="
                  << (!cancel_consensus_pending_ ? "not-pending" : "already-submitted")
                  << " pending=" << (cancel_consensus_pending_ ? 1 : 0)
                  << " submitted=" << (cancel_propose_submitted_ ? 1 : 0)
                  << " rotation_index=" << cancel_rotation_index_
                  << " proposer=r" << chooseCancelProposer()
                  << " |E|=" << cancelElectorateCandidates().size()
                  << "\n";
        return;
    }
    if (cancel_cert_bytes_.empty()) {
        std::cout << "[CANCEL-PROPOSE-GATE] r" << ctx_.replicaId_
                  << " action=abort-propose gate=missing-cert"
                  << " cancelled_epoch=" << cancelled_epoch_
                  << " rotation_index=" << cancel_rotation_index_
                  << " proposer=r" << chooseCancelProposer()
                  << " |E|=" << cancelElectorateCandidates().size()
                  << "\n";
        return;
    }

    std::vector<int> electors = cancelElectorateCandidates();
    if (electors.empty()) {
        std::cout << "[CANCEL-PROPOSE] r" << ctx_.replicaId_
                  << " abort: empty electorate E\n";
        return;
    }
    // Spec §4/§7.3: CANCEL's quorum is a fixed property of the frozen
    // committed view (N=ctx_.total_vehicles_, f=fAnchored) — e.g. N=16/f=5/
    // quorum=11 — not something recomputed from however many electors
    // happen to be locally visible right now. Up to f members (the
    // crashed/wrecked ones) are expected to be permanently silent, so
    // gating readiness on "all N electors present" (the old 3f+1 check)
    // made CANCEL structurally unable to ever propose once any member
    // was lost — no proposer, rotated or not, could satisfy it.
    const int fAnchored = anchoredFaults(ctx_.tolerated_faults_, ctx_.total_vehicles_);
    const int cancelQuorum = bftQuorumSize(ctx_.total_vehicles_, fAnchored);
    if ((int)electors.size() < cancelQuorum || cancelQuorum < 0) {
        std::cout << "[CANCEL-UNAVAILABLE] r" << ctx_.replicaId_
                  << " |E|=" << electors.size()
                  << " f_anchored=" << fAnchored
                  << " need>=" << cancelQuorum
                  << " cancelled_epoch=" << cancelled_epoch_ << "\n";
        return;
    }

    ResdbCancelHdr chdr{};
    chdr.cancelled_epoch = cancelled_epoch_;
    chdr.reason = static_cast<uint8_t>(rollback_.rollback_reason_);
    chdr.justification_len = static_cast<uint32_t>(cancel_cert_bytes_.size());

    const int32_t leader_id = ctx_.replicaId_;
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
    ctx_.propose_submitted_ = true;
    propose_time_ = simTime();
    int rc = ResdbOmnetTriggerConsensus(ctx_.resdb_server_handle_, buf.data(),
                                        static_cast<uint32_t>(buf.size()));
    std::cout << "[CANCEL-QUORUM] r" << ctx_.replicaId_
              << " |E|=" << electors.size()
              << " f_anchored=" << fAnchored
              << " quorum=" << cancelQuorum
              << " proposer=r" << ctx_.replicaId_
              << " cancelled_epoch=" << cancelled_epoch_ << "\n";
    std::cout << "[CANCEL-PROPOSE] r" << ctx_.replicaId_
              << " rc=" << rc
              << " cancelled_epoch=" << cancelled_epoch_
              << " |E|=" << electors.size()
              << " cert_bytes=" << cancel_cert_bytes_.size()
              << " payload_bytes=" << buf.size()
              << " rotation_index=" << cancel_rotation_index_
              << " t=" << simTime() << "\n";
}

std::vector<uint8_t> ResDBIntersectionApp::buildCancelCommitRef(uint32_t cancelledEpoch) const
{
    auto it = rollback_.committed_cancels_.find(cancelledEpoch);
    if (it == rollback_.committed_cancels_.end()) return {};
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
    if (!ctx_.gossip_enabled_ || !ctx_.ec_private_key_) return;
    std::vector<uint8_t> attestation(sizeof(dh));
    std::memcpy(attestation.data(), &dh, sizeof(dh));
    triggerCancelCommitGossip(dh.cancelled_epoch, attestation);
}

bool ResDBIntersectionApp::cancelGossipPropagationConfirmed() const
{
    if (cancel_gossip_bytes_.empty()) return false;
    const int threshold = anchoredFaults(ctx_.tolerated_faults_, ctx_.total_vehicles_) + 1;
    return rollback_.cancel_gossip_acc_.count(cancel_gossip_epoch_, cancel_gossip_bytes_) >= threshold;
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
        ctx_.ec_private_key_, ctx_.ec_pub_key_, inner.data(), (uint32_t)inner.size());
    if (!signed_payload.empty()) {
        sendBFTMessage(-1, signed_payload, kCancelCommitGossipType);
        std::cout << "[CANCEL-GOSSIP-SEND] r" << ctx_.replicaId_
                  << " cancelled_epoch=" << cancelledEpoch << "\n";
    }
    if (cancelGossipPropagationConfirmed()) {
        std::cout << "[CANCEL-GOSSIP-STOP] r" << ctx_.replicaId_
                  << " cancelled_epoch=" << cancelledEpoch
                  << " reason=propagation-confirmed"
                  << " seen=" << rollback_.cancel_gossip_acc_.count(cancelledEpoch, attestation) << "\n";
    } else if (cancel_cert_retry_max_ <= 0 ||
            cancel_gossip_retry_count_ < cancel_cert_retry_max_) {
        // Slower capped backoff than evidence retry (spec §11.3): 0.25s,
        // 0.5s, 1.0s, 2.0s, cap 4.0s.
        const double delay = backoffDelaySec(cancel_gossip_retry_base_sec_,
                                             cancel_gossip_retry_cap_sec_,
                                             cancel_gossip_retry_count_);
        scheduleAt(simTime() + delay, cancel_gossip_timer_);
    }
    cancel_gossip_retry_count_++;
}

void ResDBIntersectionApp::handleCancelCommitDecision(const std::vector<uint8_t>& dec)
{
    if (dec.size() < sizeof(ResdbCancelDecisionHdr)) return;
    ResdbCancelDecisionHdr dh{};
    std::memcpy(&dh, dec.data(), sizeof(dh));
    if (dh.magic != RESDB_CANCEL_DECISION_MAGIC) return;
    clearConsensusRetries("cancel-committed");

    tombstoned_epochs_.insert(dh.cancelled_epoch);
    CommittedCancelInfo info;
    info.cancel_seq = dh.cancel_seq;
    std::memcpy(info.payload_digest, dh.payload_digest, 32);
    info.attestation_bytes = dec;
    rollback_.committed_cancels_[dh.cancelled_epoch] = info;

    const int fAnchored = anchoredFaults(ctx_.tolerated_faults_, ctx_.total_vehicles_);
    const int cancelQuorum = bftQuorumSize(ctx_.total_vehicles_, fAnchored);
    std::cout << "[CANCEL-COMMIT] r" << ctx_.replicaId_
              << " cancelled_epoch=" << dh.cancelled_epoch
              << " seq=" << dh.cancel_seq
              << " quorum=" << cancelQuorum
              << " source=commit"
              << " t=" << simTime() << "\n";
    std::cout << "[TOMBSTONE] r" << ctx_.replicaId_
              << " epoch=" << dh.cancelled_epoch << " source=cancel-commit\n";

    cancel_consensus_pending_ = false;
    cancel_propose_submitted_ = false;
    cancel_state_ = CancelState::COMMITTED;
    rollback_cancel_initiated_ = false;
    stopCancelCertRetries();
    if (cancel_vc_timer_) {
        if (cancel_vc_timer_->isScheduled()) cancelEvent(cancel_vc_timer_);
        delete cancel_vc_timer_;
        cancel_vc_timer_ = nullptr;
    }
    if (cancel_drain_timer_) {
        if (cancel_drain_timer_->isScheduled()) cancelEvent(cancel_drain_timer_);
        delete cancel_drain_timer_;
        cancel_drain_timer_ = nullptr;
    }

    rollback_.rollback_justification_ = buildCancelCommitRef(dh.cancelled_epoch);
    broadcastCancelCommitAttestation(dh);
    beginPostCancelDiscovery(dh.cancelled_epoch, rollback_.rollback_reason_,
                           rollback_.rollback_reason_ref_, rollback_.rollback_justification_);
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
    // Count this vote even if I've already tombstoned the epoch myself —
    // cancelGossipPropagationConfirmed() needs rollback_.cancel_gossip_acc_ to keep
    // growing after my own adoption/commit, not freeze at whatever it was
    // the instant I tombstoned (which is almost immediately, right before I
    // start my own retry loop — if counting stopped here, the "enough peers
    // already have it" check could never fire).
    const int threshold = anchoredFaults(ctx_.tolerated_faults_, ctx_.total_vehicles_) + 1;
    const bool reached = rollback_.cancel_gossip_acc_.add(
        bft->getFromReplicaId(), cancelled_epoch, attestation, threshold);

    if (isEpochTombstoned(cancelled_epoch)) return;
    if (!reached) return;

    tombstoned_epochs_.insert(cancelled_epoch);
    CommittedCancelInfo info;
    info.cancel_seq = dh.cancel_seq;
    std::memcpy(info.payload_digest, dh.payload_digest, 32);
    info.attestation_bytes = attestation;
    info.gossip_adopted = true;
    rollback_.committed_cancels_[cancelled_epoch] = info;
    std::cout << "[CANCEL-COMMIT] r" << ctx_.replicaId_
              << " cancelled_epoch=" << cancelled_epoch
              << " seq=" << dh.cancel_seq
              << " source=gossip-adopted\n";
    std::cout << "[TOMBSTONE] r" << ctx_.replicaId_
              << " epoch=" << cancelled_epoch << " source=gossip-adopted\n";

    rollback_.rollback_justification_ = buildCancelCommitRef(cancelled_epoch);
    cancel_consensus_pending_ = false;
    cancel_propose_submitted_ = false;
    cancel_state_ = CancelState::COMMITTED;
    clearConsensusRetries("cancel-gossip-adopted");
    if (cancel_drain_timer_) {
        if (cancel_drain_timer_->isScheduled()) cancelEvent(cancel_drain_timer_);
        delete cancel_drain_timer_;
        cancel_drain_timer_ = nullptr;
    }
    triggerCancelCommitGossip(cancelled_epoch, attestation);

    const CancelReason reason = static_cast<CancelReason>(dh.reason);
    const std::string ref = rollback_.rollback_reason_ref_.empty()
        ? "gossip-adopted"
        : rollback_.rollback_reason_ref_;
    beginPostCancelDiscovery(cancelled_epoch, reason, ref, rollback_.rollback_justification_);

    if (ctx_.resdb_server_handle_) {
        const int sync_rc = ResdbOmnetAdvanceExecutorAfterGossipCancel(
            ctx_.resdb_server_handle_, cancelled_epoch);
        if (sync_rc != 0) {
            std::cout << "[EXECUTOR-GOSSIP-SYNC] r" << ctx_.replicaId_
                      << " advance failed rc=" << sync_rc
                      << " cancelled_epoch=" << cancelled_epoch << "\n";
        }
    }
}

void ResDBIntersectionApp::logDiscoveryDiagnostics(const char* reason) const
{
    std::set<int> visible;
    std::set<int> certed;
    {
        std::lock_guard<std::mutex> lk(certs_mutex_);
        for (const auto& carId : observed_intent_cars_) {
            int rid = extractReplicaId(carId);
            if (rid >= 0 && (!ctx_.cancel_pending_ || shouldIncludeInRollbackMembership(rid)))
                visible.insert(rid);
        }
        for (const auto& kv : ctx_.collected_certs_) {
            int rid = extractReplicaId(kv.first);
            if (visible.count(rid)) {
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
    std::cout << "[DISCOVERY-VIEW] r" << ctx_.replicaId_
              << " snapshot reason=" << (reason ? reason : "snapshot")
              << " epoch=" << ctx_.discovery_.epoch
              << " state=" << discoveryStateName()
              << " signed=" << certed.size()
              << " quiet=" << quiet
              << " intents=" << visible.size()
              << " missingCerts=";
    for (size_t i = 0; i < missing.size(); ++i) {
        if (i) std::cout << ",";
        std::cout << "r" << missing[i];
    }
    std::cout << " deadline="
              << (ctx_.discovery_.closeReason == DiscoveryCloseReason::DEADLINE ? 1 : 0)
              << " t=" << simTime() << "\n";
}

std::vector<int> ResDBIntersectionApp::rollbackCertedCandidates() const
{
    std::vector<int> candidates;
    {
        std::lock_guard<std::mutex> lk(certs_mutex_);
        for (const auto& kv : ctx_.collected_certs_) {
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
        for (const auto& kv : ctx_.collected_certs_) {
            int rid = extractReplicaId(kv.first);
            if (shouldIncludeInRollbackMembership(rid)) candidates.push_back(rid);
        }
        for (const auto& kv : local_vehicle_states_) {
            int rid = extractReplicaId(kv.first);
            if (shouldIncludeInRollbackMembership(rid)) candidates.push_back(rid);
        }
    }
    // Static units are permanent recovery-membership voters (they hold no cert/intent,
    // so the cert/state loops above never surface them — add them explicitly).
    for (int uid : staticUnitReplicaIds()) candidates.push_back(uid);
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

bool ResDBIntersectionApp::shouldIncludeInRollbackMembership(int replicaId) const
{
    if (replicaId < 0) return false;
    // Static units are always present (never depart, never crash), so they are
    // permanent rollback members — including when a unit evaluates itself.
    if (isStaticUnitReplica(replicaId)) return true;
    if (replicaId == ctx_.replicaId_)
        return rollback_local_recallable_ &&
            ctx_.current_phase_ != ConsensusPhase::DEPARTED &&
            !ctx_.is_departed_;
    return true;
}

std::shared_ptr<const OrderCandidate>
ResDBIntersectionApp::buildOrderCandidate() const
{
    auto candidate = std::make_shared<OrderCandidate>();
    candidate->epoch = ctx_.current_epoch_;
    candidate->recovery = ctx_.cancel_pending_ && ctx_.current_epoch_ == rollback_new_epoch_;
    candidate->cancelledEpoch = candidate->recovery ? cancelled_epoch_ : 0;
    candidate->rollbackReason = rollback_.rollback_reason_;
    candidate->cancelJustification = candidate->recovery
        ? rollback_.rollback_justification_ : std::vector<uint8_t>{};

    {
        std::lock_guard<std::mutex> lk(certs_mutex_);
        candidate->certs = ctx_.collected_certs_;
        candidate->vehicleStates = local_vehicle_states_;
        candidate->observedIntents = observed_intent_cars_;
    }

    for (const auto& kv : candidate->certs) {
        const int rid = extractReplicaId(kv.first);
        // An external/late ambulance may be carried as a proposal entry, but
        // it is not an epoch-e voter until the recovery ORDER installs the
        // newly discovered epoch-(e+1) membership.  This mirrors CertPrimary()
        // and prevents a late r16/r17 from minting a one-member ORDER(0).
        const bool eligible = candidate->recovery
            ? shouldIncludeInRollbackMembership(rid)
            : (rid >= 0 && rid < ctx_.total_vehicles_);
        if (eligible) candidate->voterIds.push_back(rid);
    }
    std::sort(candidate->voterIds.begin(), candidate->voterIds.end());
    candidate->voterIds.erase(
        std::unique(candidate->voterIds.begin(), candidate->voterIds.end()),
        candidate->voterIds.end());
    if (!candidate->voterIds.empty())
        candidate->initialPrimary = candidate->voterIds.front();

    if (candidate->recovery) {
        for (const auto& kv : incidentRegistry_) {
            if (kv.first.cancelledEpoch != cancelled_epoch_ ||
                    kv.second.state != IncidentState::CLEARED ||
                    kv.second.clearCertBytes.empty()) continue;
            candidate->clearCerts.push_back(kv.second.clearCertBytes);
        }
    }
    return candidate;
}

void ResDBIntersectionApp::resetOrderCandidate(const char* reason)
{
    if (order_candidate_) {
        std::cout << "[ORDER-CANDIDATE-RESET] r" << ctx_.replicaId_
                  << " epoch=" << order_candidate_->epoch
                  << " reason=" << (reason ? reason : "reset") << "\n";
    }
    order_candidate_.reset();
    order_vc_requested_ = false;
    order_vc_authoritative_ = false;
}

int ResDBIntersectionApp::currentOrderPrimary() const
{
    if (order_vc_authoritative_ && ctx_.resdb_server_handle_)
        return ResdbOmnetGetPrimary(ctx_.resdb_server_handle_);
    if (order_candidate_) return order_candidate_->initialPrimary;
    return CertPrimary();
}

void ResDBIntersectionApp::armOrderSuspicionTimer(const char* reason)
{
    if (ctx_.order_applied_ || ctx_.propose_submitted_ ||
            ctx_.current_phase_ == ConsensusPhase::DEPARTED ||
            (ctx_.cancel_pending_ && hasBlockingIncidentForEpoch(cancelled_epoch_))) return;
    if (!vc_trigger_msg_) vc_trigger_msg_ = new cMessage("vc_trigger");
    if (vc_trigger_msg_->isScheduled()) return;
    scheduleAt(simTime() + pbft_vc_timeout_sec_, vc_trigger_msg_);
    std::cout << "[ORDER-READINESS] r" << ctx_.replicaId_
              << " epoch=" << ctx_.current_epoch_
              << " result=follower-arm-vc"
              << " primary=r" << currentOrderPrimary()
              << " trigger=" << (reason ? reason : "ready")
              << " deadline=" << simTime() + pbft_vc_timeout_sec_ << "\n";
}

void ResDBIntersectionApp::evaluateOrderReadiness(const char* reason)
{
    const char* trigger = reason ? reason : "evaluate";
    auto logBlocked = [&](const char* why) {
        std::cout << "[ORDER-READINESS] r" << ctx_.replicaId_
                  << " epoch=" << ctx_.current_epoch_
                  << " result=blocked reason=" << why
                  << " trigger=" << trigger
                  << " discovery=" << discoveryStateName()
                  << " cancel_pending=" << (ctx_.cancel_pending_ ? 1 : 0)
                  << " cancel_consensus=" << (cancel_consensus_pending_ ? 1 : 0)
                  << "\n";
    };

    if (ctx_.order_applied_ || ctx_.current_phase_ == ConsensusPhase::DEPARTED) return;
    if (cancel_consensus_pending_) {
        logBlocked("cancel-consensus-pending");
        return;
    }
    if (rollback_cancel_initiated_) {
        logBlocked("cancel-witness-pending");
        return;
    }
    if (ctx_.discovery_.state != DiscoveryState::COMPLETE ||
            ctx_.discovery_.epoch != ctx_.current_epoch_) {
        logBlocked("discovery-not-complete");
        return;
    }

    const bool recovery = ctx_.cancel_pending_ && ctx_.current_epoch_ == rollback_new_epoch_;
    if (ctx_.cancel_pending_ && !recovery) {
        logBlocked("recovery-epoch-mismatch");
        return;
    }
    if (recovery && !hasCommittedCancel(cancelled_epoch_)) {
        logBlocked("cancel-not-committed");
        return;
    }

    if (recovery && hasBlockingIncidentForEpoch(cancelled_epoch_)) {
        const int attackPrimary = CertPrimary();
        if (inject_fabricated_clearance_leader_ &&
                !rollback_.fabricated_clearance_attack_logged_ &&
                attackPrimary == ctx_.replicaId_) {
            rollback_.fabricated_clearance_attack_logged_ = true;
            fabricated_clearance_attack_active_ = true;
            order_candidate_ = buildOrderCandidate();
            std::cout << "[BYZANTINE-FABRICATED-CLEARANCE] r" << ctx_.replicaId_
                      << " cancelled_epoch=" << cancelled_epoch_
                      << " new_epoch=" << rollback_new_epoch_
                      << " evidence_gate="
                      << (enable_recovery_clear_evidence_gate_ ? 1 : 0)
                      << " attack_time=" << simTime()
                      << " action=submit-invalid-clear\n";
            proposeAll();
            fabricated_clearance_attack_active_ = false;
            ctx_.propose_submitted_ = false;
            order_candidate_.reset();
        }
        logBlocked("incident-blocking");
        maybeSendWaitHeartbeat(trigger);
        return;
    }

    if (!order_candidate_ || order_candidate_->epoch != ctx_.current_epoch_) {
        order_candidate_ = buildOrderCandidate();
        std::cout << "[ORDER-CANDIDATE] r" << ctx_.replicaId_
                  << " epoch=" << ctx_.current_epoch_
                  << " recovery=" << (recovery ? 1 : 0)
                  << " voters=" << order_candidate_->voterIds.size()
                  << " scene=" << order_candidate_->vehicleStates.size()
                  << " clear_certs=" << order_candidate_->clearCerts.size()
                  << " initial_primary=r" << order_candidate_->initialPrimary
                  << " trigger=" << trigger << "\n";
    }

    if (recovery && static_cast<int>(order_candidate_->voterIds.size()) <
            minRollbackMembershipSize()) {
        logBlocked("membership-too-small");
        if (ctx_.replicaId_ == designatedRollbackUnavailableReporter())
            logDiscoveryDiagnostics(trigger);
        return;
    }
    const int primary = currentOrderPrimary();
    if (primary < 0) {
        logBlocked("no-primary");
        return;
    }

    stopWait("order-ready");
    std::cout << "[ORDER-READINESS] r" << ctx_.replicaId_
              << " epoch=" << ctx_.current_epoch_
              << " result=ready"
              << " recovery=" << (recovery ? 1 : 0)
              << " voters=" << order_candidate_->voterIds.size()
              << " primary=r" << primary
              << " authority=" << (order_vc_authoritative_ ? "pbft-vc" : "cert")
              << " trigger=" << trigger << "\n";
    if (primary == ctx_.replicaId_) {
        if (vc_trigger_msg_) {
            if (vc_trigger_msg_->isScheduled()) cancelEvent(vc_trigger_msg_);
            delete vc_trigger_msg_;
            vc_trigger_msg_ = nullptr;
        }
        proposeAll();
    } else {
        armOrderSuspicionTimer(trigger);
    }
}

bool ResDBIntersectionApp::maybeTriggerEmergencyRollbackFromCert(const ArrivalCert& cert)
{
    if (!ctx_.enableRollback_ || !cert.isAmbulance) return false;
    int rid = extractReplicaId(cert.carId);
    if (!ctx_.has_committed_order_) return false;
    {
        std::lock_guard<std::mutex> lk(committed_view_mutex_);
        if (ctx_.committed_order_vehicle_ids_.count(rid)) return false;
    }
    const uint32_t cancelledEpoch = ctx_.last_committed_epoch_;
    // The ambulance remains visible while ORDER(e+1) discovery is running,
    // so its announcement/certificate can be observed repeatedly after
    // CANCEL(e) has already committed.  Do not re-open the pre-CANCEL
    // liveness gate for an incident that is already tombstoned.  Previously
    // rollback_cancel_initiated_ was set before sendCancelEcho() rejected the
    // stale echo, leaving direct CANCEL committers permanently blocked while
    // gossip-only adopters happened to proceed.
    if (hasCommittedCancel(cancelledEpoch) ||
            isEpochTombstoned(cancelledEpoch) ||
            (ctx_.cancel_pending_ && cancelled_epoch_ == cancelledEpoch)) {
        std::cout << "[CANCEL-WITNESS-HUSH] r" << ctx_.replicaId_
                  << " source=arrival_cert car=" << cert.carId
                  << " cancelled_epoch=" << cancelledEpoch
                  << " reason=cancel-already-committed\n";
        return false;
    }
    std::string ref = "amb:" + cert.carId + ":" + std::to_string(cancelledEpoch);
    rollback_cancel_initiated_ = true;
    std::cout << "[CANCEL-WITNESS] r" << ctx_.replicaId_
              << " source=arrival_cert car=" << cert.carId
              << " cancelled_epoch=" << cancelledEpoch
              << " ref=" << ref << "\n";
    sendCancelEcho(cancelledEpoch, CANCEL_EMERGENCY, ref);
    return true;
}

bool ResDBIntersectionApp::maybeTriggerEmergencyRollbackFromAnnouncement(
    const ArrivalAnnouncement& ann)
{
    if (!ctx_.enableRollback_ || !ann.isAmbulance) return false;
    if (!ctx_.has_committed_order_) return false;
    int rid = extractReplicaId(ann.carId);
    {
        std::lock_guard<std::mutex> lk(committed_view_mutex_);
        if (ctx_.committed_order_vehicle_ids_.count(rid)) return false;
    }
    const uint32_t cancelledEpoch = ctx_.last_committed_epoch_;
    if (hasCommittedCancel(cancelledEpoch) ||
            isEpochTombstoned(cancelledEpoch) ||
            (ctx_.cancel_pending_ && cancelled_epoch_ == cancelledEpoch)) {
        std::cout << "[CANCEL-WITNESS-HUSH] r" << ctx_.replicaId_
                  << " source=arrival_announce car=" << ann.carId
                  << " cancelled_epoch=" << cancelledEpoch
                  << " reason=cancel-already-committed\n";
        return false;
    }
    std::string ref = "amb:" + ann.carId + ":" + std::to_string(cancelledEpoch);
    rollback_cancel_initiated_ = true;
    std::cout << "[CANCEL-WITNESS] r" << ctx_.replicaId_
              << " source=arrival_announce car=" << ann.carId
              << " cancelled_epoch=" << cancelledEpoch
              << " ref=" << ref << "\n";
    sendCancelEcho(cancelledEpoch, CANCEL_EMERGENCY, ref);
    return true;
}

void ResDBIntersectionApp::maybeTriggerCrashRollback(const std::string& reasonRef)
{
    if (!ctx_.enableRollback_ || !ctx_.has_committed_order_) return;
    std::string ref = cleanRef(reasonRef);
    std::cout << "[ROLLBACK-TRIGGER] r" << ctx_.replicaId_
              << " crash ref=" << ref
              << " committed_epoch=" << ctx_.last_committed_epoch_ << "\n";
    sendCancelEcho(ctx_.last_committed_epoch_, CANCEL_CRASH, ref);
}

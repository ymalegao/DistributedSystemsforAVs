#include "veins/modules/application/resDB/ResDBIntersectionApp.h"
#include "veins/modules/application/resDB/ResDBUtil.h"
#include "veins/modules/application/resDB/ResdbV2VWire.h"
#include "veins/modules/application/resDB/messages/BFTMessage_m.h"

#include <cstdio>
#include <cstdint>
#include <deque>
#include <iostream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <openssl/evp.h>

using namespace veins;
using namespace veins::resdb_app_util;

namespace {

std::string sha256Hex(const uint8_t* data, uint32_t len)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digestLen = 0;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return "";
    bool ok = EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1 &&
              EVP_DigestUpdate(ctx, data, len) == 1 &&
              EVP_DigestFinal_ex(ctx, digest, &digestLen) == 1;
    EVP_MD_CTX_free(ctx);
    if (!ok) return "";

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < digestLen; ++i)
        oss << std::setw(2) << static_cast<unsigned int>(digest[i]);
    return oss.str();
}

} // namespace

ResDBIntersectionApp::LoggingTransport::LoggingTransport(int rid)
    : rid_(rid)
{
}

void ResDBIntersectionApp::LoggingTransport::sendTo(int to, const uint8_t*, uint32_t len)
{
    fprintf(stderr, "[ResDB-TRANSPORT r%d] unicast -> r%d  %u bytes\n", rid_, to, len);
}

void ResDBIntersectionApp::LoggingTransport::broadcast(const uint8_t*, uint32_t len)
{
    fprintf(stderr, "[ResDB-TRANSPORT r%d] broadcast  %u bytes\n", rid_, len);
}

void ResDBIntersectionApp::registerTransport()
{
    ResdbOmnetTransportCallbacks cbs;
    cbs.send_to   = IV2VTransport::c_send_to;
    cbs.broadcast = IV2VTransport::c_broadcast;
    cbs.ctx       = transport_.get();
    ResdbOmnetSetTransport(resdb_server_handle_, &cbs);
}

// ── VeinsTransport ────────────────────────────────────────────────────────────

void ResDBIntersectionApp::VeinsTransport::sendTo(int toReplica,
                                                   const uint8_t* data, uint32_t len)
{ app_->enqueueOutbound(toReplica, data, len); }

void ResDBIntersectionApp::VeinsTransport::broadcast(const uint8_t* data, uint32_t len)
{ app_->enqueueOutbound(-1, data, len); }

void ResDBIntersectionApp::enqueueOutbound(int toReplicaId,
                                            const uint8_t* data, uint32_t len)
{
    if (!data || len == 0) return;
    if (current_phase_ == ConsensusPhase::DEPARTED || is_departed_) return;
    if (crashCommsDisabled_) {
        std::cout << "[CRASH-TX-DROP] r" << replicaId_
                  << " path=enqueueOutbound to=" << toReplicaId
                  << " len=" << len << " t=" << simTime() << "\n";
        return;
    }
    std::vector<uint8_t> bytes(data, data + len);
    std::lock_guard<std::mutex> lk(outbound_mutex_);
    // Dedup: the bridge's per-recipient SendMessage calls broadcast() N-1 times
    // with identical bytes.  Keep only the first; subsequent are dropped here so
    // one PHY transmission covers all receivers.
    // NOTE: this compares against the ENTIRE undrained queue, not just packets
    // from the same logical broadcast call — if drainOutboundQueue() falls
    // behind (queue backs up) and two DIFFERENT logical sends happen to
    // produce identical bytes, the second is silently dropped forever with
    // no log. [OUTBOUND-DEDUP-DROP] below exists to catch that case.
    for (const auto& pkt : outbound_queue_) {
        if (pkt.resdbBytes == bytes) {
            std::cout << "[OUTBOUND-DEDUP-DROP] r" << replicaId_
                      << " to=" << toReplicaId
                      << " len=" << len
                      << " queue_depth=" << outbound_queue_.size()
                      << " t=" << simTime() << "\n";
            return;
        }
    }
    PendingOutboundPacket pkt;
    pkt.toReplicaId = toReplicaId;  // -1 from broadcast path, specific from send_to path
    pkt.resdbBytes  = std::move(bytes);
    outbound_queue_.push_back(std::move(pkt));
    std::cout << "[OUTBOUND-ENQUEUE] r" << replicaId_
              << " to=" << toReplicaId
              << " len=" << len
              << " queue_depth=" << outbound_queue_.size()
              << " t=" << simTime() << "\n";
}

// drainOutboundQueue: signs and sends queued PBFT (Type-8) packets over the radio.
void ResDBIntersectionApp::drainOutboundQueue()
{
    std::deque<PendingOutboundPacket> local;
    {
        std::lock_guard<std::mutex> lk(outbound_mutex_);
        if (outbound_queue_.empty()) return;
        local.swap(outbound_queue_);
    }

    if (crashCommsDisabled_) {
        std::cout << "[CRASH-TX-DROP] r" << replicaId_
                  << " path=drainOutboundQueue dropped=" << local.size()
                  << " t=" << simTime() << "\n";
        return;
    }

    for (auto& pkt : local) {
        if (pkt.resdbBytes.empty() || !ec_private_key_) continue;

        std::vector<uint8_t> signed_payload = resdbwire::packSignedPacket(
            ec_private_key_, ec_pub_key_,
            pkt.resdbBytes.data(), (uint32_t)pkt.resdbBytes.size());
        if (signed_payload.empty()) continue;

        sentMessages_++;
        sentPayloadBytes_ += signed_payload.size();

        BFTMessage* bft = new BFTMessage();
        bft->setFromReplicaId(replicaId_);
        bft->setToReplicaId(pkt.toReplicaId);
        bft->setMessageType(kResdbConsensusMsgType);
        bft->setSequenceNum(sequenceNumber_++);
        bft->setTimestamp(simTime());
        bft->setPayloadArraySize(signed_payload.size());
        for (size_t i = 0; i < signed_payload.size(); ++i)
            bft->setPayload(i, signed_payload[i]);
        bft->setPayloadLength((int)signed_payload.size());
        bft->setRecipientAddress(LAddress::L2BROADCAST());
        bft->setChannelNumber((int)veins::Channel::cch);
        bft->addBitLength(par("headerLength"));
        bft->addBitLength((int)(signed_payload.size() * 8));

        // Per-replica stagger + jitter — same knobs as cert-protocol broadcasts.
        // replica i transmits at: i * broadcastSlotSec + uniform(jitterMin, jitterMax)
        // This prevents simultaneous PREPARE/COMMIT from all replicas colliding at
        // the MAC layer.
        double slot  = par("broadcastSlotSec").doubleValue();
        double jmin  = par("broadcastJitterMin").doubleValue();
        double jmax  = par("broadcastJitterMax").doubleValue();
        double delay = replicaId_ * slot + ((jmax > jmin) ? uniform(jmin, jmax) : 0.0);
        ResdbPacketRequestInfo inner = {};
        ResdbOmnetGetPacketRequestInfo(pkt.resdbBytes.data(),
                                       (uint32_t)pkt.resdbBytes.size(),
                                       &inner);
        rememberConsensusRetry(pkt.resdbBytes, inner);

        // air_t = when sendDelayedDown actually hands the frame to the NIC.
        // TYPE8-DRAIN t= is enqueue/schedule time; without air_t, TYPE11 relays
        // (which take a second replicaId_*slot delay inside sendBFTMessage) look
        // like they fire earlier than they do and hide PREPARE/relay collisions.
        const simtime_t air_t = simTime() + delay;
        std::cout << "[TYPE8-DRAIN] r" << replicaId_ << " to=" << pkt.toReplicaId
                  << " resdbLen=" << pkt.resdbBytes.size()
                  << " signedLen=" << signed_payload.size()
                  << " inner=" << ResdbOmnetRequestTypeName(inner.type)
                  << "(" << inner.type << ")"
                  << " view=" << inner.current_view
                  << " seq=" << inner.seq
                  << " sender=" << inner.sender_id
                  << " primary=" << inner.primary_id
                  << " proxy=" << inner.proxy_id
                  << " executed=" << inner.current_executed_seq
                  << " dataLen=" << inner.data_len
                  << " hashLen=" << inner.hash_len
                  << " parseOk=" << inner.parse_ok
                  << " delay=" << delay
                  << " air_t=" << air_t
                  << " t=" << simTime() << "\n";
        sendDelayedDown(bft, delay);
    }
}

bool ResDBIntersectionApp::ConsensusRetryManager::remember(
    const std::vector<uint8_t>& bytes, const ResdbPacketRequestInfo& info)
{
    ConsensusRetryKey key;
    key.view = info.current_view;
    key.seq = info.seq;
    std::memcpy(key.requestHash.data(), info.request_hash_digest,
                key.requestHash.size());

    PhaseMap& phases = instances_[key];
    if (info.type == 5) {
        phases.erase(3);
        phases.erase(4);
    }
    if (phases.count(info.type)) return false;

    ConsensusRetryPacket packet;
    packet.resdbBytes = bytes;
    packet.type = info.type;
    phases.emplace(info.type, std::move(packet));
    return true;
}

size_t ResDBIntersectionApp::ConsensusRetryManager::size() const
{
    size_t count = 0;
    for (const auto& instance : instances_) count += instance.second.size();
    return count;
}

void ResDBIntersectionApp::rememberConsensusRetry(
    const std::vector<uint8_t>& bytes, const ResdbPacketRequestInfo& info)
{
    if (!info.parse_ok || bytes.empty() || consensus_retry_max_ == 0) return;
    if (info.type < 3 || info.type > 5) return;
    if (info.sender_id != replicaId_ + 1) return;

    if (consensus_retry_manager_.remember(bytes, info)) {
        std::cout << "[PBFT-RETRY-ARM] r" << replicaId_
                  << " phase=" << ResdbOmnetRequestTypeName(info.type)
                  << " view=" << info.current_view
                  << " seq=" << info.seq
                  << " interval=" << consensus_retry_interval_sec_
                  << " max=" << consensus_retry_max_
                  << " t=" << simTime() << "\n";
    }
    if (!consensus_retry_manager_.empty() &&
            !consensus_retry_timer_->isScheduled()) {
        scheduleAt(simTime() + consensus_retry_interval_sec_, consensus_retry_timer_);
    }
}

void ResDBIntersectionApp::sendConsensusBytes(
    const std::vector<uint8_t>& bytes, int toReplicaId,
    const char* source, int retryAttempt)
{
    if (bytes.empty() || !ec_private_key_ ||
            current_phase_ == ConsensusPhase::DEPARTED) return;
    std::vector<uint8_t> signedPayload = resdbwire::packSignedPacket(
        ec_private_key_, ec_pub_key_, bytes.data(), (uint32_t)bytes.size());
    if (signedPayload.empty()) return;

    BFTMessage* bft = new BFTMessage();
    bft->setFromReplicaId(replicaId_);
    bft->setToReplicaId(toReplicaId);
    bft->setMessageType(kResdbConsensusMsgType);
    bft->setSequenceNum(sequenceNumber_++);
    bft->setTimestamp(simTime());
    bft->setPayloadArraySize(signedPayload.size());
    for (size_t i = 0; i < signedPayload.size(); ++i) bft->setPayload(i, signedPayload[i]);
    bft->setPayloadLength((int)signedPayload.size());
    bft->setRecipientAddress(LAddress::L2BROADCAST());
    bft->setChannelNumber((int)veins::Channel::cch);
    bft->addBitLength(par("headerLength"));
    bft->addBitLength((int)(signedPayload.size() * 8));

    const double slot = par("broadcastSlotSec").doubleValue();
    const double jmin = par("broadcastJitterMin").doubleValue();
    const double jmax = par("broadcastJitterMax").doubleValue();
    const double delay = replicaId_ * slot +
        ((jmax > jmin) ? uniform(jmin, jmax) : 0.0);
    ResdbPacketRequestInfo info = {};
    ResdbOmnetGetPacketRequestInfo(bytes.data(), (uint32_t)bytes.size(), &info);
    sentMessages_++;
    sentPayloadBytes_ += signedPayload.size();
    std::cout << "[PBFT-RETRY] r" << replicaId_
              << " source=" << (source ? source : "timer")
              << " phase=" << ResdbOmnetRequestTypeName(info.type)
              << " view=" << info.current_view
              << " seq=" << info.seq
              << " attempt=" << retryAttempt
              << " air_t=" << simTime() + delay
              << " t=" << simTime() << "\n";
    sendDelayedDown(bft, delay);
}

void ResDBIntersectionApp::retryConsensusPackets()
{
    if (current_phase_ == ConsensusPhase::DEPARTED ||
            consensus_retry_manager_.empty())
        return;
    std::vector<ConsensusRetryPacket> sends;
    auto& instances = consensus_retry_manager_.instances();
    for (auto instanceIt = instances.begin(); instanceIt != instances.end();) {
        auto& phases = instanceIt->second;
        for (auto phaseIt = phases.begin(); phaseIt != phases.end();) {
            ConsensusRetryPacket& packet = phaseIt->second;
            const int progressVoteType = packet.type == 3 ? 4 :
                (packet.type == 4 ? 5 : -1);
            int progressCount = 0;
            int forcedQuorum = -1;
            const int rc = progressVoteType > 0
                ? ResdbOmnetGetVerifiedVoteProgress(
                    resdb_server_handle_, packet.resdbBytes.data(),
                    static_cast<uint32_t>(packet.resdbBytes.size()),
                    progressVoteType, &progressCount, &forcedQuorum)
                : -1;
            const int quorum = forcedQuorum > 0
                ? forcedQuorum : configured_consensus_quorum_;
            if (rc == 0 && quorum > 0 && progressCount >= quorum) {
                std::cout << "[PBFT-RETRY-STOP] r" << replicaId_
                          << " reason="
                          << (progressVoteType == 4
                                  ? "prepare-certificate"
                                  : "commit-certificate")
                          << " phase=" << ResdbOmnetRequestTypeName(packet.type)
                          << " view=" << instanceIt->first.view
                          << " seq=" << instanceIt->first.seq
                          << " votes=" << progressCount
                          << " quorum=" << quorum
                          << " t=" << simTime() << "\n";
                phaseIt = phases.erase(phaseIt);
                continue;
            }
            if (consensus_retry_max_ > 0 &&
                    packet.attempts >= consensus_retry_max_) {
                std::cout << "[PBFT-RETRY-STOP] r" << replicaId_
                          << " reason=max phase="
                          << ResdbOmnetRequestTypeName(packet.type)
                          << " view=" << instanceIt->first.view
                          << " seq=" << instanceIt->first.seq << "\n";
                phaseIt = phases.erase(phaseIt);
                continue;
            }
            packet.attempts++;
            sends.push_back(packet);
            ++phaseIt;
        }
        if (phases.empty()) instanceIt = instances.erase(instanceIt);
        else ++instanceIt;
    }
    for (const auto& retry : sends)
        sendConsensusBytes(retry.resdbBytes, -1, "bounded", retry.attempts);
    if (!consensus_retry_manager_.empty() &&
            !consensus_retry_timer_->isScheduled()) {
        scheduleAt(simTime() + consensus_retry_interval_sec_, consensus_retry_timer_);
    }
}

void ResDBIntersectionApp::clearConsensusRetries(const char* reason)
{
    if (!consensus_retry_manager_.empty()) {
        std::cout << "[PBFT-RETRY-STOP] r" << replicaId_
                  << " reason=" << (reason ? reason : "decision")
                  << " entries=" << consensus_retry_manager_.size()
                  << " t=" << simTime() << "\n";
    }
    consensus_retry_manager_.clear();
    if (consensus_retry_timer_->isScheduled()) cancelEvent(consensus_retry_timer_);
}

// ── sendBFTMessage: cert-protocol radio send (no PBFT crypto) ─────────────────

bool ResDBIntersectionApp::isDiscoveryAirMsgType(int msgType) const
{
    return msgType == kArrivalAnnounceType
        || msgType == kArrivalAnnounceGossipType
        || msgType == kArrivalEchoType
        || msgType == kArrivalCertType;
}

bool ResDBIntersectionApp::discoveryAcceptsNewTx(int msgType, bool localCert,
                                                  bool witnessTraffic) const
{
    if (current_phase_ == ConsensusPhase::DEPARTED) return false;
    if (cancel_state_ == CancelState::DRAINING ||
            cancel_state_ == CancelState::CONSENSUS) return false;
    // A replica with no active round can still attest to a late vehicle's
    // physical announcement. This does not reopen or mutate its own view.
    if (witnessTraffic && msgType == kArrivalEchoType) return true;
    if (order_applied_ || propose_submitted_) return false;
    if (discovery_.state == DiscoveryState::COLLECTING) return true;
    return discovery_.state == DiscoveryState::DRAINING_CERTS &&
        msgType == kArrivalCertType && localCert && !discovery_.localCertAired();
}

void ResDBIntersectionApp::sendBFTMessageNow(int toReplicaId,
                                              const std::vector<uint8_t>& payload,
                                              int msgType)
{
    if (payload.empty()) return;
    if (crashCommsDisabled_) {
        std::cout << "[CRASH-TX-DROP] r" << replicaId_
                  << " path=sendBFTMessageNow type=" << msgType
                  << " to=" << toReplicaId << " t=" << simTime() << "\n";
        return;
    }
    sentMessages_++;
    sentPayloadBytes_ += payload.size();
    BFTMessage* bft = new BFTMessage();
    bft->setFromReplicaId(replicaId_);
    bft->setToReplicaId(toReplicaId);
    bft->setMessageType(msgType);
    bft->setSequenceNum(sequenceNumber_++);
    bft->setTimestamp(simTime());
    bft->setPayloadArraySize(payload.size());
    for (size_t i = 0; i < payload.size(); ++i)
        bft->setPayload(i, payload[i]);
    bft->setPayloadLength((int)payload.size());
    bft->setRecipientAddress(LAddress::L2BROADCAST());
    bft->setChannelNumber((int)veins::Channel::cch);
    bft->addBitLength(par("headerLength"));
    bft->addBitLength((int)(payload.size() * 8));
    sendDown(bft);
}

void ResDBIntersectionApp::enqueueDiscoveryTx(int toReplicaId,
                                                const std::vector<uint8_t>& payload,
                                                int msgType,
                                                simtime_t fireTime,
                                                bool localCert,
                                                bool witnessTraffic)
{
    if (crashCommsDisabled_) {
        std::cout << "[CRASH-TX-DROP] r" << replicaId_
                  << " path=enqueueDiscoveryTx type=" << msgType
                  << " to=" << toReplicaId << " t=" << simTime() << "\n";
        return;
    }
    PendingDiscoveryTx pending;
    pending.toReplicaId = toReplicaId;
    pending.msgType = msgType;
    pending.payload = payload;
    pending.fireTime = fireTime;
    pending.epoch = current_epoch_;
    pending.localCert = localCert;
    pending.witnessTraffic = witnessTraffic;
    pending_discovery_txs_.push_back(std::move(pending));
    scheduleDiscoveryTxFlush();
}

void ResDBIntersectionApp::scheduleDiscoveryTxFlush()
{
    if (pending_discovery_txs_.empty()) return;
    simtime_t next = pending_discovery_txs_.front().fireTime;
    for (const auto& p : pending_discovery_txs_) {
        if (p.fireTime < next) next = p.fireTime;
    }
    if (!discovery_tx_flush_timer_) {
        discovery_tx_flush_timer_ = new cMessage("resdbDiscoveryTxFlush");
    }
    if (discovery_tx_flush_timer_->isScheduled()) {
        if (discovery_tx_flush_timer_->getArrivalTime() <= next) return;
        cancelEvent(discovery_tx_flush_timer_);
    }
    scheduleAt(next, discovery_tx_flush_timer_);
}

void ResDBIntersectionApp::flushDueDiscoveryTxs()
{
    simtime_t now = simTime();
    std::vector<PendingDiscoveryTx> due;
    for (auto it = pending_discovery_txs_.begin(); it != pending_discovery_txs_.end();) {
        if (it->fireTime <= now) {
            due.push_back(std::move(*it));
            it = pending_discovery_txs_.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto& pending : due) {
        const bool witnessAllowed = pending.witnessTraffic &&
            pending.msgType == kArrivalEchoType &&
            current_phase_ != ConsensusPhase::DEPARTED;
        if (!witnessAllowed && (pending.epoch != discovery_.epoch ||
                discovery_.state == DiscoveryState::INACTIVE ||
                discovery_.state == DiscoveryState::COMPLETE ||
                (discovery_.state == DiscoveryState::DRAINING_CERTS &&
                 pending.msgType != kArrivalCertType))) {
            continue;
        }
        std::cout << "[AIR-TX] r" << replicaId_
                  << " msgType=" << pending.msgType
                  << " delay=0"
                  << " air_t=" << now
                  << " t=" << now << "\n";
        sendBFTMessageNow(pending.toReplicaId, pending.payload, pending.msgType);
        if (pending.msgType == kArrivalCertType && pending.localCert) {
            discovery_.localCert = LocalCertState::AIRED;
            std::cout << "[DISCOVERY-CERT-AIRED] r" << replicaId_
                      << " epoch=" << pending.epoch
                      << " t=" << simTime() << "\n";
            if (discovery_.state == DiscoveryState::DRAINING_CERTS)
                stopCertBroadcastRetries();
        }
    }
    scheduleDiscoveryTxFlush();
    maybeCompleteDiscoveryDrain("cert-queue-drained");
}

void ResDBIntersectionApp::cancelPendingDiscoveryTxs(const char* reason)
{
    const size_t n = pending_discovery_txs_.size();
    if (n == 0 && !(discovery_tx_flush_timer_ && discovery_tx_flush_timer_->isScheduled()))
        return;
    pending_discovery_txs_.clear();
    if (discovery_tx_flush_timer_ && discovery_tx_flush_timer_->isScheduled())
        cancelEvent(discovery_tx_flush_timer_);
    if (n > 0) {
        std::cout << "[DISCOVERY-TX-CANCEL] r" << replicaId_
                  << " dropped=" << n
                  << " reason=" << (reason ? reason : "unspecified")
                  << " propose_submitted=" << (propose_submitted_ ? 1 : 0)
                  << " order_applied=" << (order_applied_ ? 1 : 0)
                  << " discovery_state=" << discoveryStateName()
                  << " t=" << simTime() << "\n";
    }
}

void ResDBIntersectionApp::discardPendingDiscoveryNonCerts(const char* reason)
{
    size_t dropped = 0;
    for (auto it = pending_discovery_txs_.begin(); it != pending_discovery_txs_.end();) {
        if (it->msgType != kArrivalCertType) {
            it = pending_discovery_txs_.erase(it);
            ++dropped;
        } else {
            ++it;
        }
    }
    if (discovery_tx_flush_timer_ && discovery_tx_flush_timer_->isScheduled())
        cancelEvent(discovery_tx_flush_timer_);
    scheduleDiscoveryTxFlush();
    if (dropped > 0) {
        std::cout << "[DISCOVERY-TX-DISCARD] r" << replicaId_
                  << " dropped=" << dropped
                  << " kept_certs=" << pending_discovery_txs_.size()
                  << " reason=" << (reason ? reason : "view-closed")
                  << " t=" << simTime() << "\n";
    }
}

bool ResDBIntersectionApp::hasPendingDiscoveryCerts(uint32_t epoch) const
{
    for (const auto& pending : pending_discovery_txs_) {
        if (pending.epoch == epoch && pending.msgType == kArrivalCertType) return true;
    }
    return false;
}

void ResDBIntersectionApp::sendBFTMessage(int toReplicaId,
                                           const std::vector<uint8_t>& payload,
                                           int msgType,
                                           bool localCert,
                                           bool witnessTraffic)
{
    if (payload.empty()) return;
    if (crashCommsDisabled_) {
        std::cout << "[CRASH-TX-DROP] r" << replicaId_
                  << " path=sendBFTMessage type=" << msgType
                  << " to=" << toReplicaId << " t=" << simTime() << "\n";
        return;
    }

    double delaySec = 0;
    if (msgType == kArrivalEchoType) {
        delaySec = replicaId_ * par("viewAgreementSlotSec").doubleValue()
            + uniform(par("viewJitterMin").doubleValue(), par("viewJitterMax").doubleValue());
    } else if (msgType == kArrivalCertType) {
        delaySec = replicaId_ * par("arrivalSlotSec").doubleValue()
            + uniform(par("viewJitterMin").doubleValue(), par("viewJitterMax").doubleValue());
    } else if (msgType == kArrivalAnnounceType || msgType == kArrivalAnnounceGossipType) {
        delaySec = replicaId_ * par("arrivalSlotSec").doubleValue()
            + uniform(par("broadcastJitterMin").doubleValue(), par("broadcastJitterMax").doubleValue());
    } else {
        delaySec = replicaId_ * par("broadcastSlotSec").doubleValue()
            + uniform(par("broadcastJitterMin").doubleValue(), par("broadcastJitterMax").doubleValue());
    }

    if (debug_cert_protocol_) {
        const char* kind = (msgType == kArrivalAnnounceType)       ? "ANN"
                           : (msgType == kArrivalAnnounceGossipType) ? "ANN-GOSSIP"
                           : (msgType == kArrivalEchoType)         ? "ECHO"
                           : (msgType == kArrivalCertType)         ? "CERT"
                           : "?";
        std::cout << "[CERT-DEBUG] sendBFT r" << replicaId_ << " kind=" << kind
                  << " type=" << msgType << " toReplicaId=" << toReplicaId
                  << " payloadBytes=" << payload.size() << " scheduleDelay=" << delaySec
                  << "s t=" << simTime() << "\n";
    }

    // Discovery frames remain cancellable while waiting for their radio slot.
    if (isDiscoveryAirMsgType(msgType)) {
        if (!discoveryAcceptsNewTx(msgType, localCert, witnessTraffic)) {
            std::cout << "[DISCOVERY-TX-DROP] r" << replicaId_
                      << " msgType=" << msgType
                      << " reason=state-blocked"
                      << " discovery_state=" << discoveryStateName()
                      << " t=" << simTime() << "\n";
            return;
        }
        const simtime_t air_t = simTime() + delaySec;
        std::cout << "[DISCOVERY-TX-SCHED] r" << replicaId_
                  << " msgType=" << msgType
                  << " delay=" << delaySec
                  << " air_t=" << air_t
                  << " t=" << simTime() << "\n";
        enqueueDiscoveryTx(toReplicaId, payload, msgType, air_t, localCert,
                           witnessTraffic);
        return;
    }

    if (msgType == kResdbConsensusRelayType) {
        std::cout << "[AIR-TX] r" << replicaId_
                  << " msgType=" << msgType
                  << " delay=" << delaySec
                  << " air_t=" << (simTime() + delaySec)
                  << " t=" << simTime() << "\n";
    }

    // Non-discovery (TYPE11 relay, gossip, cancel cert, …): keep prior path.
    sentMessages_++;
    sentPayloadBytes_ += payload.size();
    BFTMessage* bft = new BFTMessage();
    bft->setFromReplicaId(replicaId_);
    bft->setToReplicaId(toReplicaId);
    bft->setMessageType(msgType);
    bft->setSequenceNum(sequenceNumber_++);
    bft->setTimestamp(simTime());
    bft->setPayloadArraySize(payload.size());
    for (size_t i = 0; i < payload.size(); ++i)
        bft->setPayload(i, payload[i]);
    bft->setPayloadLength((int)payload.size());
    bft->setRecipientAddress(LAddress::L2BROADCAST());
    bft->setChannelNumber((int)veins::Channel::cch);
    bft->addBitLength(par("headerLength"));
    bft->addBitLength((int)(payload.size() * 8));
    sendDelayedDown(bft, delaySec);
}

void ResDBIntersectionApp::handleResdbConsensusMessage(BFTMessage* bft)
{
    if (bft->getToReplicaId() != -1 && bft->getToReplicaId() != replicaId_) return;

    int plen = bft->getPayloadArraySize();
    if (plen <= 0) return;
    std::vector<uint8_t> buf((size_t)plen);
    for (int i = 0; i < plen; ++i) buf[i] = bft->getPayload(i);

    resdbwire::SignedPacketView view;
    if (!resdbwire::unpackSignedPacket(buf.data(), (uint32_t)buf.size(), &view)) return;
    if (view.resdbLen == 0) return;

    if (!CryptoAuth::instance().verifyBytes(view.pubKey, view.resdbBytes, view.resdbLen,
                                            view.sig, view.sigLen)) {
        std::cout << "[TYPE8-RECV] r" << replicaId_ << " dropped forged packet from "
                  << bft->getFromReplicaId() << "\n";
        return;
    }
    ResdbPacketRequestInfo inner = {};
    ResdbOmnetGetPacketRequestInfo(view.resdbBytes, view.resdbLen, &inner);
    std::cout << "[TYPE8-RECV] r" << replicaId_ << " from=" << bft->getFromReplicaId()
              << " resdbLen=" << view.resdbLen
              << " inner=" << ResdbOmnetRequestTypeName(inner.type)
              << "(" << inner.type << ")"
              << " view=" << inner.current_view
              << " seq=" << inner.seq
              << " sender=" << inner.sender_id
              << " primary=" << inner.primary_id
              << " proxy=" << inner.proxy_id
              << " executed=" << inner.current_executed_seq
              << " dataLen=" << inner.data_len
              << " hashLen=" << inner.hash_len
              << " parseOk=" << inner.parse_ok
              << " t=" << simTime() << "\n";
    int primary = ResdbOmnetGetPrimary(resdb_server_handle_);
    if (primary >= 0 && bft->getFromReplicaId() == primary)
        stopCertBroadcastRetries();

    ResdbOmnetDeliverPacket(resdb_server_handle_, bft->getFromReplicaId(),
                            view.resdbBytes, view.resdbLen);
    maybeRelayResdbConsensusBytes(view.resdbBytes, view.resdbLen, inner, "type8");
}

bool ResDBIntersectionApp::isConsensusRelayEligible(const ResdbPacketRequestInfo& info) const
{
    if (!info.parse_ok) return false;
    switch (info.type) {
    case 3:  // TYPE_PRE_PREPARE
    case 4:  // TYPE_PREPARE
    case 5:  // TYPE_COMMIT
        return true;
    default:
        return false;
    }
}

std::string ResDBIntersectionApp::consensusRelayKey(
    const uint8_t* data, uint32_t len, const ResdbPacketRequestInfo& info) const
{
    return std::to_string(info.type) + ":" +
           std::to_string(info.current_view) + ":" +
           std::to_string(info.seq) + ":" +
           std::to_string(info.sender_id) + ":" +
           sha256Hex(data, len);
}

void ResDBIntersectionApp::maybeRelayResdbConsensusBytes(
    const uint8_t* data, uint32_t len, const ResdbPacketRequestInfo& info,
    const char* source)
{
    // NOTE: relay eligibility must track "still present in the sim"
    // (current_phase_ == DEPARTED), not "my own order_applied_" — a replica
    // whose ORDER(e) already applied must keep relaying CANCEL(e)/ORDER(e+1)
    // consensus traffic for everyone else, or rollback consensus can never
    // get enough votes relayed to out-of-range replicas. See [RELAY-GATE] log.
    if (!gossip_enabled_ || current_phase_ == ConsensusPhase::DEPARTED ||
        !data || len == 0) {
        if (data && len > 0 && isConsensusRelayEligible(info)) {
            std::cout << "[RELAY-GATE-DROP] r" << replicaId_
                      << " reason=" << (!gossip_enabled_ ? "gossip-disabled" :
                                        current_phase_ == ConsensusPhase::DEPARTED ?
                                            "departed" : "no-data")
                      << " source=" << (source ? source : "?")
                      << " inner=" << ResdbOmnetRequestTypeName(info.type)
                      << " seq=" << info.seq
                      << " order_applied=" << (order_applied_ ? 1 : 0)
                      << " t=" << simTime() << "\n";
        }
        return;
    }
    if (!isConsensusRelayEligible(info)) return;

    std::string key = consensusRelayKey(data, len, info);
    if (!consensus_relay_seen_.insert(key).second) {
        std::cout << "[TYPE11-DROP] r" << replicaId_
                  << " reason=duplicate"
                  << " source=" << (source ? source : "?")
                  << " inner=" << ResdbOmnetRequestTypeName(info.type)
                  << " view=" << info.current_view
                  << " seq=" << info.seq
                  << " sender=" << info.sender_id
                  << " hash=" << key.substr(key.rfind(':') + 1)
                  << " t=" << simTime() << "\n";
        return;
    }

    // Immediate relay (deduped). A prior suppress-on-overhear delay queue was
    // removed: at N≈10 simultaneous TYPE8 hearers it could not beat the ~3ms
    // one-hop floor, and discovery-TX cancel is what unblocked ORDER(1).
    std::vector<uint8_t> raw(data, data + len);
    auto inner = resdb_gossip::serializeConsensusRelay(current_epoch_, raw);
    auto signed_payload = resdbwire::packSignedPacket(
        ec_private_key_, ec_pub_key_, inner.data(), (uint32_t)inner.size());
    if (signed_payload.empty()) return;

    sendBFTMessage(-1, signed_payload, kResdbConsensusRelayType);
    std::cout << "[TYPE11-SEND] r" << replicaId_
              << " source=" << (source ? source : "?")
              << " inner=" << ResdbOmnetRequestTypeName(info.type)
              << " view=" << info.current_view
              << " seq=" << info.seq
              << " sender=" << info.sender_id
              << " hash=" << key.substr(key.rfind(':') + 1)
              << " bytes=" << raw.size()
              << " order_applied=" << (order_applied_ ? 1 : 0)
              << " current_epoch=" << current_epoch_
              << " t=" << simTime() << "\n";
}

void ResDBIntersectionApp::handleResdbConsensusRelay(BFTMessage* bft)
{
    int plen = bft->getPayloadArraySize();
    if (plen <= 0) return;
    std::vector<uint8_t> buf((size_t)plen);
    for (int i = 0; i < plen; ++i) buf[i] = bft->getPayload(i);

    resdbwire::SignedPacketView view;
    if (!resdbwire::unpackSignedPacket(buf.data(), (uint32_t)buf.size(), &view)) return;
    if (view.resdbLen == 0) return;

    if (!CryptoAuth::instance().verifyBytes(view.pubKey, view.resdbBytes, view.resdbLen,
                                            view.sig, view.sigLen)) {
        std::cout << "[TYPE11-RECV] r" << replicaId_
                  << " dropped forged relay from " << bft->getFromReplicaId()
                  << "\n";
        return;
    }

    uint32_t epoch = 0;
    std::vector<uint8_t> raw;
    if (!resdb_gossip::parseConsensusRelay(view.resdbBytes, view.resdbLen,
                                           epoch, raw)) return;
    // NOTE: staleness must be "a newer epoch has already started" (epoch <
    // current_epoch_), not "an order for this epoch already committed" —
    // CANCEL(e)/WAIT(e) relay traffic is tagged with epoch==e, same as the
    // ORDER(e) that just committed, and still needs to be relayed until
    // current_epoch_ actually advances in beginPostCancelDiscovery(). The old
    // has_committed_order_ check blackholed exactly that traffic. See
    // [RELAY-GATE] log.
    if ((int)epoch < (int)current_epoch_) {
        std::cout << "[RELAY-GATE-DROP] r" << replicaId_
                  << " reason=stale-epoch carrier=" << bft->getFromReplicaId()
                  << " relay_epoch=" << epoch
                  << " current_epoch=" << current_epoch_
                  << " has_committed_order=" << (has_committed_order_ ? 1 : 0)
                  << " last_committed_epoch=" << last_committed_epoch_
                  << " t=" << simTime() << "\n";
        return;
    }

    ResdbPacketRequestInfo info = {};
    ResdbOmnetGetPacketRequestInfo(raw.data(), (uint32_t)raw.size(), &info);
    std::string key = consensusRelayKey(raw.data(), (uint32_t)raw.size(), info);

    if (!isConsensusRelayEligible(info)) {
        std::cout << "[TYPE11-DROP] r" << replicaId_
                  << " reason=ineligible carrier=" << bft->getFromReplicaId()
                  << " inner=" << ResdbOmnetRequestTypeName(info.type)
                  << " view=" << info.current_view
                  << " seq=" << info.seq
                  << " sender=" << info.sender_id
                  << " parseOk=" << info.parse_ok
                  << " t=" << simTime() << "\n";
        return;
    }

    if (consensus_relay_seen_.count(key) != 0) {
        std::cout << "[TYPE11-DROP] r" << replicaId_
                  << " reason=duplicate"
                  << " carrier=" << bft->getFromReplicaId()
                  << " inner=" << ResdbOmnetRequestTypeName(info.type)
                  << " view=" << info.current_view
                  << " seq=" << info.seq
                  << " sender=" << info.sender_id
                  << " hash=" << key.substr(key.rfind(':') + 1)
                  << " t=" << simTime() << "\n";
        return;
    }

    std::cout << "[TYPE11-RECV] r" << replicaId_
              << " carrier=" << bft->getFromReplicaId()
              << " inner=" << ResdbOmnetRequestTypeName(info.type)
              << " view=" << info.current_view
              << " seq=" << info.seq
              << " sender=" << info.sender_id
              << " hash=" << key.substr(key.rfind(':') + 1)
              << " bytes=" << raw.size()
              << " has_committed_order=" << (has_committed_order_ ? 1 : 0)
              << " relay_epoch=" << epoch
              << " current_epoch=" << current_epoch_
              << " t=" << simTime() << "\n";

    int original_from = info.sender_id > 0 ? info.sender_id - 1 : bft->getFromReplicaId();
    ResdbOmnetDeliverPacket(resdb_server_handle_, original_from,
                            raw.data(), (uint32_t)raw.size());
    maybeRelayResdbConsensusBytes(raw.data(), (uint32_t)raw.size(), info, "type11");
}

// ── onWSM ─────────────────────────────────────────────────────────────────────

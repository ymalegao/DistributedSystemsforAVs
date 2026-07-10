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

// ── sendBFTMessage: cert-protocol radio send (no PBFT crypto) ─────────────────

bool ResDBIntersectionApp::isDiscoveryAirMsgType(int msgType) const
{
    return msgType == kArrivalAnnounceType
        || msgType == kArrivalAnnounceGossipType
        || msgType == kArrivalEchoType
        || msgType == kArrivalCertType;
}

bool ResDBIntersectionApp::shouldQuiesceDiscoveryAir() const
{
    // Same quiesce used by CERT-RELAY-STOP / ANN-SEND-STOP / cert retries.
    // Epoch 0: propose_submitted_ / pbft_observed_ (ORDER PRE_PREPARE seen).
    // Epoch 1: also stop once rollback discovery membership is closed — otherwise
    // long-stagger CERT relays keep landing on the ORDER(e+1) PRE_PREPARE.
    return order_applied_ || propose_submitted_ || pbft_observed_
        || current_phase_ == ConsensusPhase::DEPARTED
        || (cancel_pending_ && rollback_discovery_ready_);
}

void ResDBIntersectionApp::sendBFTMessageNow(int toReplicaId,
                                              const std::vector<uint8_t>& payload,
                                              int msgType)
{
    if (payload.empty()) return;
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
                                                simtime_t fireTime)
{
    PendingDiscoveryTx pending;
    pending.toReplicaId = toReplicaId;
    pending.msgType = msgType;
    pending.payload = payload;
    pending.fireTime = fireTime;
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
    if (shouldQuiesceDiscoveryAir()) {
        cancelPendingDiscoveryTxs("flush-quiesce");
        return;
    }
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
        if (shouldQuiesceDiscoveryAir()) {
            cancelPendingDiscoveryTxs("flush-mid-quiesce");
            return;
        }
        std::cout << "[AIR-TX] r" << replicaId_
                  << " msgType=" << pending.msgType
                  << " delay=0"
                  << " air_t=" << now
                  << " t=" << now << "\n";
        sendBFTMessageNow(pending.toReplicaId, pending.payload, pending.msgType);
    }
    scheduleDiscoveryTxFlush();
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
                  << " pbft_observed=" << (pbft_observed_ ? 1 : 0)
                  << " propose_submitted=" << (propose_submitted_ ? 1 : 0)
                  << " order_applied=" << (order_applied_ ? 1 : 0)
                  << " t=" << simTime() << "\n";
    }
}

void ResDBIntersectionApp::sendBFTMessage(int toReplicaId,
                                           const std::vector<uint8_t>& payload,
                                           int msgType)
{
    if (payload.empty()) return;

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

    // Discovery frames: hold in a cancellable queue (same idea as TYPE11 suppress).
    // sendDelayedDown is not cancellable — those 0.3–0.4s CERT staggers were still
    // hitting the air after pbft_observed_ flipped and colliding with ORDER PRE_PREPARE.
    if (isDiscoveryAirMsgType(msgType)) {
        if (shouldQuiesceDiscoveryAir()) {
            std::cout << "[DISCOVERY-TX-DROP] r" << replicaId_
                      << " msgType=" << msgType
                      << " reason=already-quiesced"
                      << " pbft_observed=" << (pbft_observed_ ? 1 : 0)
                      << " discovery_ready=" << (rollback_discovery_ready_ ? 1 : 0)
                      << " t=" << simTime() << "\n";
            return;
        }
        const simtime_t air_t = simTime() + delaySec;
        std::cout << "[DISCOVERY-TX-SCHED] r" << replicaId_
                  << " msgType=" << msgType
                  << " delay=" << delaySec
                  << " air_t=" << air_t
                  << " t=" << simTime() << "\n";
        enqueueDiscoveryTx(toReplicaId, payload, msgType, air_t);
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
    // CANCEL never has a discovery phase (it votes over the static provisioned set —
    // nobody needs to announce/certify for it), so it can never mean "my discovery
    // concluded" for anyone. Only ORDER concludes a discovery process. Ledger
    // convention: ORDER(e) = seq 2e+1 (odd), CANCEL(e) = seq 2e+2 (even).
    if (inner.type == 3 /* TYPE_PRE_PREPARE */ && (inner.seq % 2) == 1 /* ORDER, not CANCEL */) {
        pbft_observed_ = true;
        cancelPendingDiscoveryTxs("type8-order-preprepare");
        std::cout << "[PBFT-OBSERVED] r" << replicaId_ << " source=type8"
                  << " seq=" << inner.seq << " view=" << inner.current_view
                  << " t=" << simTime() << "\n";
    }
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
    // current_epoch_ actually advances in beginRollbackDiscovery(). The old
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
    // See handleResdbConsensusMessage() for why CANCEL (even seq) is excluded here:
    // it has no discovery phase, so it can never mean "my discovery concluded."
    if (info.type == 3 /* TYPE_PRE_PREPARE */ && (info.seq % 2) == 1 /* ORDER, not CANCEL */) {
        pbft_observed_ = true;
        cancelPendingDiscoveryTxs("type11-order-preprepare");
        std::cout << "[PBFT-OBSERVED] r" << replicaId_ << " source=type11"
                  << " seq=" << info.seq << " relay_epoch=" << epoch
                  << " t=" << simTime() << "\n";
    }
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

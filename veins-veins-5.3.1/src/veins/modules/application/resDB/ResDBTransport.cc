#include "veins/modules/application/resDB/ResDBIntersectionApp.h"
#include "veins/modules/application/resDB/ResDBUtil.h"
#include "veins/modules/application/resDB/ResdbV2VWire.h"
#include "veins/modules/bftsmart/BFTMessage_m.h"

#include <cstdio>
#include <deque>
#include <iostream>
#include <mutex>
#include <utility>
#include <vector>

using namespace veins;
using namespace veins::resdb_app_util;

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
    std::vector<uint8_t> bytes(data, data + len);
    std::lock_guard<std::mutex> lk(outbound_mutex_);
    // Dedup: the bridge's per-recipient SendMessage calls broadcast() N-1 times
    // with identical bytes.  Keep only the first; subsequent are dropped here so
    // one PHY transmission covers all receivers.
    for (const auto& pkt : outbound_queue_) {
        if (pkt.resdbBytes == bytes) return;
    }
    PendingOutboundPacket pkt;
    pkt.toReplicaId = toReplicaId;  // -1 from broadcast path, specific from send_to path
    pkt.resdbBytes  = std::move(bytes);
    outbound_queue_.push_back(std::move(pkt));
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
        std::cout << "[TYPE8-DRAIN] r" << replicaId_ << " to=" << pkt.toReplicaId
                  << " resdbLen=" << pkt.resdbBytes.size()
                  << " signedLen=" << signed_payload.size()
                  << " delay=" << delay << " t=" << simTime() << "\n";
        sendDelayedDown(bft, delay);
    }
}

// ── sendBFTMessage: cert-protocol radio send (no PBFT crypto) ─────────────────

void ResDBIntersectionApp::sendBFTMessage(int toReplicaId,
                                           const std::vector<uint8_t>& payload,
                                           int msgType)
{
    if (payload.empty()) return;
    sentMessages_++;
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
    double delaySec = 0;
    if (msgType == kArrivalEchoType) {
        delaySec = replicaId_ * par("viewAgreementSlotSec").doubleValue()
            + uniform(par("viewJitterMin").doubleValue(), par("viewJitterMax").doubleValue());
    } else if (msgType == kArrivalCertType) {
        delaySec = replicaId_ * par("arrivalSlotSec").doubleValue() + uniform(par("viewJitterMin").doubleValue(), par("viewJitterMax").doubleValue());
        // delaySec = uniform(par("viewJitterMin").doubleValue(), par("viewJitterMax").doubleValue());
    } else if (msgType == kArrivalAnnounceType) {
        delaySec = replicaId_ * par("arrivalSlotSec").doubleValue()
            + uniform(par("broadcastJitterMin").doubleValue(), par("broadcastJitterMax").doubleValue());
    } else {
        delaySec = replicaId_ * par("broadcastSlotSec").doubleValue()
            + uniform(par("broadcastJitterMin").doubleValue(), par("broadcastJitterMax").doubleValue());
    }

    if (debug_cert_protocol_) {
        const char* kind = (msgType == kArrivalAnnounceType)   ? "ANN"
                           : (msgType == kArrivalEchoType)     ? "ECHO"
                           : (msgType == kArrivalCertType)     ? "CERT"
                           : "?";
        std::cout << "[CERT-DEBUG] sendBFT r" << replicaId_ << " kind=" << kind
                  << " type=" << msgType << " toReplicaId=" << toReplicaId
                  << " payloadBytes=" << payload.size() << " scheduleDelay=" << delaySec
                  << "s t=" << simTime() << "\n";
    }
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
    std::cout << "[TYPE8-RECV] r" << replicaId_ << " from=" << bft->getFromReplicaId()
              << " resdbLen=" << view.resdbLen << " t=" << simTime() << "\n";
    int primary = ResdbOmnetGetPrimary(resdb_server_handle_);
    if (primary >= 0 && bft->getFromReplicaId() == primary)
        stopCertBroadcastRetries();

    ResdbOmnetDeliverPacket(resdb_server_handle_, bft->getFromReplicaId(),
                            view.resdbBytes, view.resdbLen);
}

// ── onWSM ─────────────────────────────────────────────────────────────────────

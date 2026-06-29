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

ResDBIntersectionApp::ArrivalCert
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

// ── tryStartCertCollectionTimer (V2V parity: deadline starts at primary stop-zone entry) ──

int ResDBIntersectionApp::countStaticCollectedCerts() const
{
    int count = 0;
    for (const auto& kv : collected_certs_) {
        const int rid = extractReplicaId(kv.first);
        if (rid >= 0 && rid < total_vehicles_)
            ++count;
    }
    return count;
}

int ResDBIntersectionApp::CertPrimary() const
{
    int primary = -1;
    for (const auto& kv : collected_certs_) {
        const int rid = extractReplicaId(kv.first);
        if (rid >= 0 && rid < total_vehicles_ &&
                (primary < 0 || rid < primary)) {
            primary = rid;
        }
    }
    return primary;
}

void ResDBIntersectionApp::tryStartCertCollectionTimer(bool rearm)
{
    if (!resdb_server_handle_ || propose_submitted_) return;
    int certPrimary = CertPrimary();
    if (certPrimary < 0) {
        std::cout << "[CERT-PRIMARY] r" << replicaId_
                  << " no static cert primary yet; keep gossiping\n";
        return;
    }
    if (replicaId_ != certPrimary) return;
    {
        std::lock_guard<std::mutex> lk(certs_mutex_);
        if (countStaticCollectedCerts() >= total_vehicles_) return;
    }
    if (!entered_stop_zone_) return;
    if (!rearm && cert_collection_started_) return;
    if (propose_timeout_msg_ && propose_timeout_msg_->isScheduled()) {
        if (!rearm) return;
        cancelEvent(propose_timeout_msg_);
    }

    if (!propose_timeout_msg_)
        propose_timeout_msg_ = new cMessage("resdbProposeTimeout");
    scheduleAt(simTime() + cert_collection_timeout_, propose_timeout_msg_);
    cert_collection_started_ = true;
    cert_collection_start_time_ = simTime();
    std::cout << "[METRICS " << replicaId_ << "] Cert_Collection_Start: " << cert_collection_start_time_ << "\n";
    std::cout << "[ResDB r" << replicaId_
              << "] CertPrimary: cert-collection deadline at stop line"
              << " cert_primary=" << certPrimary
              << " (timeout="
              << cert_collection_timeout_ << "s rearm=" << (rearm ? 1 : 0) << ")\n";
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
    if (current_phase_ == ConsensusPhase::DEPARTED || order_applied_) return;
    if (!mobility) return;

    TraCICommandInterface* traci = mobility->getCommandInterface();
    if (!traci) return;

    std::string myCarId = "veh" + std::to_string(replicaId_);
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

    // ECDSA P-256 self-signed claim.
    if (ec_private_key_) {
        std::string toSign = ann.carId + ":" + ann.laneId + ":" +
                             std::to_string(ann.positionInLane) + ":" +
                             std::to_string(ann.claimedArrivalTime) + ":" +
                             std::to_string(ann.epoch);
        uint8_t sigOut[CRYPTO_SIG_MAX_BYTES]; uint8_t sigLen = 0;
        if (CryptoAuth::instance().signBytes(ec_private_key_,
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
        physically_observed_cars_.insert(myCarId);
    }

    if (is_byzantine_ && byzantine_type_ == BYZANTINE_EQUIVOCATOR) {
        int n = total_vehicles_;
        for (int peerId = 0; peerId < n; ++peerId) {
            if (peerId == replicaId_) continue;
            ArrivalAnnouncement annByz = ann;
            annByz.direction = (peerId < n / 2) ? DIR_LEFT : DIR_RIGHT;
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
    if (propose_submitted_ || order_applied_) return;
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
    if (propose_submitted_ || order_applied_) return;

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
    std::vector<uint8_t> announceBytes = payloadBytes(msg);
    ArrivalAnnouncement ann = deserializeArrivalAnnouncement(msg);
    if (ann.carId.empty()) return;
    std::cout << "[ANN-RECV] Replica " << replicaId_ << " received ARRIVAL_ANNOUNCE from "
              << ann.carId << " frameFrom=" << msg->getFromReplicaId()
              << " via=" << (viaGossip ? "gossip" : "direct");
    if (viaGossip) std::cout << " carrier=" << carrierReplicaId;
    std::cout << " at t=" << simTime() << "\n";

    // Dedup: only re-echo if we VERIFIED and echoed this car before (not FALSE_LANE).
    // echoed_cars_ is populated only after verifyCarPosition passes, so FALSE_LANE
    // cars stored in local_vehicle_states_ are never re-echoed here.
    if (local_vehicle_states_.count(ann.carId)) {
        // Re-echo if we verified this car before but haven't received its cert yet.
        // Use !collected_certs_.count(ann.carId), NOT !cert_broadcast_: the latter
        // is this replica's own flag and would incorrectly suppress re-echoes after
        // this replica assembles its own cert.
        if (echoed_cars_.count(ann.carId)
                && !collected_certs_.count(ann.carId)
                && !propose_submitted_ && !order_applied_) {
            pending_relays_[std::make_pair((uint32_t)ann.epoch, ann.carId)] = {
                ann.carId, (uint32_t)ann.epoch, announceBytes, simTime().dbl(), 0};
            sendArrivalEcho(ann);
        }
        return;
    }

    // Verify lane via TraCI.
    VerificationResult result = verifyCarPosition(ann.carId, ann.laneId, ann.positionInLane, 1e9);

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
        physically_observed_cars_.insert(ann.carId);
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
    arrival_announcements_received_.insert(ann.carId);
    physically_observed_cars_.insert(ann.carId);
    echoed_cars_.insert(ann.carId);  // record that we actually echoed this car
    pending_relays_[std::make_pair((uint32_t)ann.epoch, ann.carId)] = {
        ann.carId, (uint32_t)ann.epoch, announceBytes, simTime().dbl(), 0};

    sendArrivalEcho(ann);
    gossipArrivalAnnouncement(ann, announceBytes);

    std::cout << "[ANN-RECV] Replica " << replicaId_ << " stored VehicleState for "
              << ann.carId << " via=" << (viaGossip ? "gossip" : "direct")
              << " (" << physically_observed_cars_.size()
              << "/" << total_vehicles_ << " observed)\n";

    if (physically_observed_cars_.size() == total_vehicles_) {
        current_phase_ = ConsensusPhase::COLLECTING_CERTS;
        // Do NOT cancel the re-announce timer here: this car may still need witnesses to re-echo it.
        // The timer handler stops once cert_broadcast_ is true.
        std::cout << "[ANN-RECV] Replica " << replicaId_ << " all vehicles observed, phase=COLLECTING_CERTS\n";
    }
}

// ── sendArrivalEcho ───────────────────────────────────────────────────────────

void ResDBIntersectionApp::sendArrivalEcho(const ArrivalAnnouncement& ann)
{
    if (!ec_private_key_) return;
    std::string toSign = ann.carId + ":" + ann.lane + ":" +
        std::to_string(ann.positionInLane) + ":" +
        (ann.direction == DIR_LEFT ? "L" : ann.direction == DIR_RIGHT ? "R" : "S") +
        ":" + (ann.isAmbulance ? "1" : "0") +
        ":" + std::to_string(replicaId_);

    ArrivalEcho echo;
    echo.echoingReplicaId = replicaId_;
    echo.targetCarId      = ann.carId;
    echo.lane             = ann.lane;
    echo.positionInLane   = ann.positionInLane;
    echo.direction        = ann.direction;
    echo.isAmbulance      = ann.isAmbulance;
    echo.epoch            = ann.epoch;
    std::memcpy(echo.signerPubKey, ec_pub_key_, CRYPTO_PUBKEY_BYTES);
    std::memset(echo.signature, 0, CRYPTO_SIG_MAX_BYTES);
    echo.signatureLen = 0;

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

    int targetId = extractReplicaId(ann.carId);
    sendBFTMessage(-1, serializeArrivalEcho(echo), kArrivalEchoType);
    std::cout << "[ECHO-SEND] Replica " << replicaId_ << " → " << ann.carId
              << " ARRIVAL_ECHO sigLen=" << (int)echo.signatureLen << "\n";
    if (debug_cert_protocol_) {
        std::cout << "[CERT-DEBUG] sendArrivalEcho r" << replicaId_ << " targetReplicaId=-1"
                  << " signPayload=\"" << toSign << "\"\n";
    }
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

    auto& echoes = my_received_echoes_[myCarId];
    for (const auto& e : echoes)
        if (e.echoingReplicaId == echo.echoingReplicaId) {
            if (debug_cert_protocol_)
                std::cout << "[CERT-DEBUG] handleArrivalEcho r" << replicaId_
                          << " dedup echo from r" << echo.echoingReplicaId << "\n";
            return;  // dedup
        }

    echoes.push_back(echo);
    std::cout << "[ECHO-RECV] Replica " << replicaId_ << " received echo from "
              << echo.echoingReplicaId << " (" << echoes.size() << " so far)\n";

    int f        = (total_vehicles_ - 1) / 3;
    int required = f + 1;
    if (debug_cert_protocol_)
        std::cout << "[CERT-DEBUG] handleArrivalEcho r" << replicaId_
                  << " progress " << echoes.size() << "/" << required
                  << " (f=" << f << ")\n";
    if ((int)echoes.size() >= required) {
        cert_broadcast_ = true;
        ArrivalCert cert;
        cert.carId = myCarId;
        if (local_vehicle_states_.count(myCarId)) {
            const VehicleState& sv = local_vehicle_states_.at(myCarId);
            cert.lane           = sv.lane;
            cert.positionInLane = sv.positionInLane;
            cert.direction      = sv.direction;
            cert.isAmbulance    = sv.isAmbulance;
        }
        cert.epoch  = (int)current_epoch_;
        cert.echoes = echoes;
        std::cout << "[CERT-ASSEMBLE] Replica " << replicaId_ << " assembled ARRIVAL_CERT with "
                  << cert.echoes.size() << " echoes — broadcasting\n";
        broadcastArrivalCert(cert);
    }
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

void ResDBIntersectionApp::broadcastArrivalCert(const ArrivalCert& cert)
{
    sendBFTMessage(-1, serializeArrivalCert(cert), kArrivalCertType);
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
    bool shouldPropose = false;
    if (!collected_certs_.count(cert.carId)) {
        int staticCerts = 0;
        int allCerts = 0;
        int certPrimary = -1;
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
        // If cert-primary and in stop zone and all static certs now collected, propose immediately.
        if (replicaId_ == certPrimary
                && entered_stop_zone_ && !propose_submitted_) {
            shouldPropose = !emergencyCancelStarted && (staticCerts >= total_vehicles_);
        }
    }
    if (shouldPropose) {
        if (propose_timeout_msg_) {
            cancelEvent(propose_timeout_msg_);
            delete propose_timeout_msg_;
            propose_timeout_msg_ = nullptr;
        }
        proposeAll();
    }
    trySubmitRollbackProposal("self-cert");
    maybeFinishRollbackDiscovery("self-cert");
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
        physically_observed_cars_.insert(cert.carId);
    }

    // Epidemic relay: each node forwards each validated cert once.
    // cert already carries f+1 signatures so recipients can verify without
    // re-accumulating votes.  cert_relay_tracker_ deduplicates per carId.
    if (cert_relay_tracker_.tryRelay(cert.carId)) {
        sendBFTMessage(-1, serializeArrivalCert(cert), kArrivalCertType);
        std::cout << "[CERT-RELAY] r" << replicaId_ << " relayed cert for "
                  << cert.carId << " t=" << simTime() << "\n";
    }

    const bool emergencyCancelStarted = maybeTriggerEmergencyRollbackFromCert(cert);

    // Cert-primary: if in stop zone and all static certs collected, propose.
    // Emergency late certs take the cancel path first; the old normal order
    // should not race the rollback trigger.
    if (replicaId_ == certPrimary && entered_stop_zone_ && !propose_submitted_) {
        if (!emergencyCancelStarted && staticCerts >= total_vehicles_) {
            if (propose_timeout_msg_) {
                cancelEvent(propose_timeout_msg_);
                delete propose_timeout_msg_;
                propose_timeout_msg_ = nullptr;
            }
            proposeAll();
        }
    }
    trySubmitRollbackProposal("cert-stored");
    maybeFinishRollbackDiscovery("cert-stored");
}

// ── validateArrivalCert ───────────────────────────────────────────────────────

bool ResDBIntersectionApp::validateArrivalCert(const ArrivalCert& cert)
{
    int f = (total_vehicles_ - 1) / 3;
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

#include "veins/modules/application/resDB/ResDBIntersectionApp.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

#include "veins/modules/application/resDB/ResdbV2VWire.h"
#include "veins/modules/bftsmart/BFTMessage_m.h"

using namespace veins;

Define_Module(veins::ResDBIntersectionApp);

namespace {
constexpr int kResdbConsensusMsgType     = 8;
constexpr int kResdbStateAnnounceMsgType = 9;
// Type-9 payload is exactly sizeof(ResdbVehicleEntry) = 13 bytes.
}

// ── Destructor ────────────────────────────────────────────────────────────────

ResDBIntersectionApp::~ResDBIntersectionApp()
{
    if (smoke_test_msg_)      { cancelAndDelete(smoke_test_msg_);      smoke_test_msg_      = nullptr; }
    if (transport_poll_msg_)  { cancelAndDelete(transport_poll_msg_);  transport_poll_msg_  = nullptr; }
    if (time_tick_msg_)       { cancelAndDelete(time_tick_msg_);       time_tick_msg_       = nullptr; }
    if (state_announce_msg_)  { cancelAndDelete(state_announce_msg_);  state_announce_msg_  = nullptr; }
    if (propose_timeout_msg_) { cancelAndDelete(propose_timeout_msg_); propose_timeout_msg_ = nullptr; }

    if (resdb_server_handle_) {
        ResdbOmnetDestroyServer(resdb_server_handle_);
        resdb_server_handle_ = nullptr;
    }
    if (ec_private_key_) {
        EVP_PKEY_free(ec_private_key_);
        ec_private_key_ = nullptr;
    }
}

// ── initialize ────────────────────────────────────────────────────────────────

void ResDBIntersectionApp::initialize(int stage)
{
    DemoBaseApplLayer::initialize(stage);

    if (stage == 0) {
        replicaId_                = par("replicaId").intValue();
        useRadioTransport_        = par("useRadioTransport").boolValue();
        transport_poll_interval_  = par("transportPollInterval").doubleValue();
        enable_sim_time_provider_ = par("enableSimTimeProvider").boolValue();
        time_tick_interval_       = par("timeTickInterval").doubleValue();
        state_announce_interval_  = par("stateAnnounceIntervalSec").doubleValue();
        cert_collection_timeout_  = par("certCollectionTimeoutSec").doubleValue();

        intersection_x_   = par("intersectionX").doubleValue();
        intersection_y_   = par("intersectionY").doubleValue();
        stop_distance_    = par("stopDistance").doubleValue();
        total_vehicles_   = par("totalVehicles").intValue();
        cruise_speed_mps_ = par("cruiseSpeedMps").doubleValue();
        is_ambulance_     = par("isAmbulance").boolValue();

        config_file_      = par("resdbConfigFile").stdstringValue();
        private_key_file_ = par("resdbPrivateKeyFile").stdstringValue();
        cert_file_        = par("resdbCertFile").stdstringValue();
        log_dir_          = par("resdbLogDir").stdstringValue();

        std::string crypto_dir = par("resdbCryptoDir").stdstringValue();
        if (!crypto_dir.empty() && config_file_.empty()) {
            int node_id = replicaId_ + 1;
            config_file_      = crypto_dir + "/server.config";
            private_key_file_ = crypto_dir + "/node" + std::to_string(node_id) + ".key.pri";
            cert_file_        = crypto_dir + "/cert_"  + std::to_string(node_id) + ".cert";
            log_dir_          = crypto_dir + "/logs";
        }

        ec_private_key_ = CryptoAuth::instance().generateKeyPair(ec_pub_key_);

        if (useRadioTransport_) {
            transport_ = std::make_unique<VeinsTransport>(this);
            transport_poll_msg_ = new cMessage("resdbTransportPoll");
            scheduleAt(simTime() + transport_poll_interval_, transport_poll_msg_);
        } else {
            transport_ = std::make_unique<LoggingTransport>(replicaId_);
        }

        if (config_file_.empty()) {
            resdb_server_handle_ = ResdbOmnetCreateNullHandle();
        } else {
            resdb_server_handle_ = ResdbOmnetCreateKvServer(
                &config_file_[0],
                private_key_file_.empty() ? nullptr : &private_key_file_[0],
                cert_file_.empty()        ? nullptr : &cert_file_[0],
                log_dir_.empty()          ? nullptr : &log_dir_[0]);
            if (!resdb_server_handle_) {
                resdb_server_handle_ = ResdbOmnetCreateNullHandle();
            }
        }

        registerTransport();

        ResdbOmnetSetOrderCallback(resdb_server_handle_,
                                   &ResDBIntersectionApp::onOrderDecided, this);

        if (enable_sim_time_provider_) {
            time_tick_msg_ = new cMessage("resdbTimeTick");
            scheduleAt(simTime() + time_tick_interval_, time_tick_msg_);
            ResdbOmnetUpdateSimTimeUs(resdb_server_handle_, simTime().inUnit(SIMTIME_US));
        }

        ResdbOmnetRunServer(resdb_server_handle_);
    }

    if (stage == 1) {
        if (par("smokeTestBroadcast").boolValue()) {
            smoke_test_msg_ = new cMessage("resdbSmokeTest");
            scheduleAt(simTime() + 0.05, smoke_test_msg_);
        }
        state_announce_msg_ = new cMessage("resdbStateAnnounce");
        scheduleAt(simTime() + state_announce_interval_, state_announce_msg_);
    }
}

// ── handleSelfMsg ─────────────────────────────────────────────────────────────

void ResDBIntersectionApp::handleSelfMsg(cMessage* msg)
{
    if (msg == smoke_test_msg_) {
        smoke_test_msg_ = nullptr;
        const uint8_t probe[] = {'R','E','S','D','B','T','S','T'};
        ResdbOmnetTestBroadcast(resdb_server_handle_, probe, sizeof(probe));
        delete msg;
        return;
    }

    if (msg == transport_poll_msg_) {
        drainOutboundQueue();
        processOrders();
        scheduleAt(simTime() + transport_poll_interval_, transport_poll_msg_);
        return;
    }

    if (msg == time_tick_msg_) {
        if (enable_sim_time_provider_)
            ResdbOmnetUpdateSimTimeUs(resdb_server_handle_, simTime().inUnit(SIMTIME_US));
        scheduleAt(simTime() + time_tick_interval_, time_tick_msg_);
        return;
    }

    if (msg == state_announce_msg_) {
        broadcastStateAnnounce();
        scheduleAt(simTime() + state_announce_interval_, state_announce_msg_);
        return;
    }

    if (msg == propose_timeout_msg_) {
        propose_timeout_msg_ = nullptr;
        if (!propose_submitted_) {
            EV_WARN << "[ResDB r" << replicaId_
                    << "] propose timeout — submitting with available states\n";
            proposeAll();
        }
        delete msg;
        return;
    }

    DemoBaseApplLayer::handleSelfMsg(msg);
}

// ── handlePositionUpdate ──────────────────────────────────────────────────────

void ResDBIntersectionApp::handlePositionUpdate(cObject* obj)
{
    DemoBaseApplLayer::handlePositionUpdate(obj);
    if (order_applied_) return;

    double dist = getDistanceToIntersection();
    double zone = stop_distance_ * (total_vehicles_ / 2.0);

    if (dist < zone && !entered_stop_zone_) {
        entered_stop_zone_ = true;
        stop_time_ = simTime();
        std::cout << "[METRICS " << replicaId_ << "] Arrival_Time: " << stop_time_ << "\n";
        std::cout << "[METRICS " << replicaId_ << "] Stop_Time: " << stop_time_ << "\n";
        std::cout << "[METRICS " << replicaId_ << "] Cert_Collection_Start: " << simTime() << "\n";

        stopVehicle();

        int primary = ResdbOmnetGetPrimary(resdb_server_handle_);
        if (replicaId_ == primary && !propose_submitted_) {
            int n;
            { std::lock_guard<std::mutex> lk(states_mutex_); n = (int)collected_states_.size(); }
            if (n >= total_vehicles_) {
                proposeAll();
            } else {
                propose_timeout_msg_ = new cMessage("resdbProposeTimeout");
                scheduleAt(simTime() + cert_collection_timeout_, propose_timeout_msg_);
            }
        }
    }
}

// ── finish ────────────────────────────────────────────────────────────────────

void ResDBIntersectionApp::finish()
{
    if (resdb_server_handle_) {
        ResdbOmnetStopServer(resdb_server_handle_);
        ResdbOmnetDestroyServer(resdb_server_handle_);
        resdb_server_handle_ = nullptr;
    }
    DemoBaseApplLayer::finish();
}

// ── registerTransport ─────────────────────────────────────────────────────────

void ResDBIntersectionApp::registerTransport()
{
    ResdbOmnetTransportCallbacks cbs;
    cbs.send_to   = IV2VTransport::c_send_to;
    cbs.broadcast = IV2VTransport::c_broadcast;
    cbs.ctx       = transport_.get();
    ResdbOmnetSetTransport(resdb_server_handle_, &cbs);
}

// ── Step-3: VeinsTransport ────────────────────────────────────────────────────

void ResDBIntersectionApp::VeinsTransport::sendTo(int toReplica,
                                                   const uint8_t* data, uint32_t len)
{ app_->enqueueOutbound(toReplica, data, len); }

void ResDBIntersectionApp::VeinsTransport::broadcast(const uint8_t* data, uint32_t len)
{ app_->enqueueOutbound(-1, data, len); }

void ResDBIntersectionApp::enqueueOutbound(int toReplicaId,
                                            const uint8_t* data, uint32_t len)
{
    if (!data || len == 0) return;
    PendingOutboundPacket pkt;
    pkt.toReplicaId = toReplicaId;
    pkt.resdbBytes.assign(data, data + len);
    std::lock_guard<std::mutex> lk(outbound_mutex_);
    outbound_queue_.push_back(std::move(pkt));
}

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

        double jmin = par("resdbBroadcastJitterMin").doubleValue();
        double jmax = par("resdbBroadcastJitterMax").doubleValue();
        sendDelayedDown(bft, (jmax > jmin) ? uniform(jmin, jmax) : 0);
    }
}

// ── Step-3: inbound (onWSM) ───────────────────────────────────────────────────

void ResDBIntersectionApp::onWSM(BaseFrame1609_4* wsm)
{
    auto* bft = dynamic_cast<BFTMessage*>(wsm);
    if (!bft) return;

    int msgType = bft->getMessageType();

    // ── Type 9: VehicleState struct broadcast ─────────────────────────────────
    if (msgType == kResdbStateAnnounceMsgType) {
        if (bft->getFromReplicaId() == replicaId_) return;

        int plen = bft->getPayloadArraySize();
        if (plen != (int)sizeof(ResdbVehicleEntry)) return;

        ResdbVehicleEntry entry;
        for (int i = 0; i < plen; ++i)
            reinterpret_cast<uint8_t*>(&entry)[i] = bft->getPayload(i);

        {
            std::lock_guard<std::mutex> lk(states_mutex_);
            collected_states_[entry.replica_id] = entry;
        }

        // If we're the primary, in the stop zone, and now have all states → propose.
        int primary = ResdbOmnetGetPrimary(resdb_server_handle_);
        if (replicaId_ == primary && entered_stop_zone_ && !propose_submitted_) {
            int n;
            { std::lock_guard<std::mutex> lk(states_mutex_); n = (int)collected_states_.size(); }
            if (n >= total_vehicles_) {
                if (propose_timeout_msg_) {
                    cancelEvent(propose_timeout_msg_);
                    delete propose_timeout_msg_;
                    propose_timeout_msg_ = nullptr;
                }
                proposeAll();
            }
        }
        return;
    }

    // ── Type 8: ResDB PBFT consensus bytes ────────────────────────────────────
    if (msgType != kResdbConsensusMsgType) return;
    if (bft->getFromReplicaId() == replicaId_) return;
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
        EV_WARN << "[ResDB r" << replicaId_ << "] dropped forged packet\n";
        return;
    }

    ResdbOmnetDeliverPacket(resdb_server_handle_, bft->getFromReplicaId(),
                            view.resdbBytes, view.resdbLen);
}

// ── Step 5: broadcastStateAnnounce ───────────────────────────────────────────

void ResDBIntersectionApp::broadcastStateAnnounce()
{
    if (!mobility) return;

    ResdbVehicleEntry entry;
    entry.replica_id   = replicaId_;
    entry.sim_time_us  = (uint64_t)simTime().inUnit(SIMTIME_US);
    entry.is_ambulance = is_ambulance_ ? 1 : 0;

    // Store own state.
    {
        std::lock_guard<std::mutex> lk(states_mutex_);
        collected_states_[replicaId_] = entry;
    }

    // Broadcast the 13-byte struct as BFTMessage type 9 (no crypto needed).
    const uint8_t* raw = reinterpret_cast<const uint8_t*>(&entry);
    constexpr int plen = (int)sizeof(ResdbVehicleEntry);

    BFTMessage* bft = new BFTMessage();
    bft->setFromReplicaId(replicaId_);
    bft->setToReplicaId(-1);
    bft->setMessageType(kResdbStateAnnounceMsgType);
    bft->setSequenceNum(sequenceNumber_++);
    bft->setTimestamp(simTime());
    bft->setPayloadArraySize(plen);
    for (int i = 0; i < plen; ++i) bft->setPayload(i, raw[i]);
    bft->setPayloadLength(plen);
    bft->setRecipientAddress(LAddress::L2BROADCAST());
    bft->setChannelNumber((int)veins::Channel::cch);
    bft->addBitLength(par("headerLength"));
    bft->addBitLength(plen * 8);
    sendDown(bft);
}

// ── Step 5: proposeAll ────────────────────────────────────────────────────────

void ResDBIntersectionApp::proposeAll()
{
    if (propose_submitted_) return;
    propose_submitted_ = true;
    propose_time_ = simTime();
    std::cout << "[METRICS " << replicaId_ << "] ProposeAll_Submit_Time: " << propose_time_ << "\n";

    // Snapshot collected states.
    std::vector<ResdbVehicleEntry> entries;
    {
        std::lock_guard<std::mutex> lk(states_mutex_);
        for (auto& kv : collected_states_) entries.push_back(kv.second);
    }

    // Pack: ResdbProposeHdr + entries.
    uint32_t n = (uint32_t)entries.size();
    size_t total = sizeof(ResdbProposeHdr) + n * sizeof(ResdbVehicleEntry);
    std::vector<uint8_t> buf(total);
    uint8_t* p = buf.data();

    ResdbProposeHdr hdr;
    hdr.epoch               = current_epoch_;
    hdr.leader_id           = replicaId_;
    hdr.propose_sim_time_us = (uint64_t)simTime().inUnit(SIMTIME_US);
    hdr.n_vehicles          = n;
    std::memcpy(p, &hdr, sizeof(hdr));
    p += sizeof(hdr);

    for (const auto& e : entries) {
        std::memcpy(p, &e, sizeof(e));
        p += sizeof(e);
    }

    int rc = ResdbOmnetTriggerConsensus(resdb_server_handle_, buf.data(), (uint32_t)buf.size());
    EV_INFO << "[ResDB r" << replicaId_ << "] TriggerConsensus rc=" << rc
            << " vehicles=" << n << "\n";
}

// ── Step 5: onOrderDecided (ResDB worker thread) ──────────────────────────────

/*static*/ void ResDBIntersectionApp::onOrderDecided(void* ctx,
                                                      const uint8_t* bytes, uint32_t len)
{
    auto* app = static_cast<ResDBIntersectionApp*>(ctx);
    if (!bytes || len == 0) return;
    std::vector<uint8_t> copy(bytes, bytes + len);
    std::lock_guard<std::mutex> lk(app->orders_mutex_);
    app->pending_orders_.push_back(std::move(copy));
}

// ── Step 5: processOrders (simulation thread) ─────────────────────────────────

void ResDBIntersectionApp::processOrders()
{
    std::deque<std::vector<uint8_t>> local;
    {
        std::lock_guard<std::mutex> lk(orders_mutex_);
        if (pending_orders_.empty()) return;
        local.swap(pending_orders_);
    }

    for (const auto& dec : local) {
        if (order_applied_) break;
        if (dec.size() < sizeof(ResdbOrderHdr)) continue;

        ResdbOrderHdr ohdr;
        std::memcpy(&ohdr, dec.data(), sizeof(ohdr));
        if (dec.size() < sizeof(ResdbOrderHdr) + ohdr.n_vehicles * sizeof(int32_t)) continue;

        const int32_t* order = reinterpret_cast<const int32_t*>(
            dec.data() + sizeof(ResdbOrderHdr));

        std::cout << "[METRICS " << replicaId_ << "] Order_Decided_Time: " << simTime() << "\n";

        int position = -1;
        for (uint32_t i = 0; i < ohdr.n_vehicles; ++i) {
            if (order[i] == replicaId_) { position = (int)i; break; }
        }
        if (position < 0) continue;

        order_applied_ = true;
        resumeVehicle(position);
    }
}

// ── Step 5: TraCI helpers ─────────────────────────────────────────────────────

void ResDBIntersectionApp::stopVehicle()
{
    if (!traciVehicle) return;
    try {
        traciVehicle->setSpeedMode(0);
        traciVehicle->setSpeed(0);
    } catch (...) {}
}

void ResDBIntersectionApp::resumeVehicle(int position_in_order)
{
    if (!traciVehicle) return;
    double delay = position_in_order * par("safetyGapS").doubleValue();
    std::cout << "[METRICS " << replicaId_ << "] Resume_Time: "
              << (simTime() + delay) << "\n";
    try {
        traciVehicle->setSpeedMode(31);
        traciVehicle->setSpeed(cruise_speed_mps_);
    } catch (...) {}
}

double ResDBIntersectionApp::getDistanceToIntersection() const
{
    if (!mobility) return 1e9;
    auto pos = mobility->getPositionAt(simTime());
    double dx = pos.x - intersection_x_;
    double dy = pos.y - intersection_y_;
    return std::sqrt(dx * dx + dy * dy);
}

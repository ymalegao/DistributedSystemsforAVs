//
// V2VArrivalProtocol.cc
// Extracted arrival and VIEW protocol helpers for V2VProxyModule
//

#include "veins/modules/bftsmart/V2VProxyModule.h"
#include "veins/modules/bftsmart/crypto/CryptoAuth.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <openssl/evp.h>
#include <sstream>



using namespace veins;

// XXHash32 has been replaced by ECDSA P-256 (CryptoAuth) for all signing use
// cases in this file (witness echoes + self-signed arrival claims), per the
// IEEE 1609.2 / ETSI TS 103 097 V2X security profile.
static std::map<int, std::map<int, double>> resetToViewEndByEpochAndReplica;
static std::set<int> printedResetToViewEndAvgEpochs;




static std::string dirToStr(V2VProxyModule::Direction d) {
    switch (d) {
        case V2VProxyModule::DIR_LEFT:     return "L";
        case V2VProxyModule::DIR_RIGHT:    return "R";
        default:                           return "S";
    }
}

static V2VProxyModule::Direction strToDir(const std::string& s) {
    if (s == "L") return V2VProxyModule::DIR_LEFT;
    if (s == "R") return V2VProxyModule::DIR_RIGHT;
    return V2VProxyModule::DIR_STRAIGHT;
}

// Hex encode/decode helpers for ambulance cert bytes
static std::string toHex(const std::vector<uint8_t>& v) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(v.size() * 2);
    for (uint8_t b : v) { out += digits[b >> 4]; out += digits[b & 0xf]; }
    return out;
}

static std::vector<uint8_t> fromHex(const std::string& s) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        out.push_back((uint8_t)std::stoi(s.substr(i, 2), nullptr, 16));
    }
    return out;
}

// Stable short key identifier used by PROPOSE_ALL cert compression:
// keyId = first 8 bytes of SHA-256(pubKey), rendered as 16 hex chars.
static std::string shortPubKeyIdHex(const uint8_t* pubKey, size_t len) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digestLen = 0;
    EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
    if (!mdctx) return "";
    bool ok = (EVP_DigestInit_ex(mdctx, EVP_sha256(), nullptr) == 1)
           && (EVP_DigestUpdate(mdctx, pubKey, len) == 1)
           && (EVP_DigestFinal_ex(mdctx, digest, &digestLen) == 1);
    EVP_MD_CTX_free(mdctx);
    if (!ok || digestLen < 8) return "";

    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(16);
    for (size_t i = 0; i < 8; ++i) {
        uint8_t b = digest[i];
        out += digits[b >> 4];
        out += digits[b & 0x0f];
    }
    return out;
}

V2VProxyModule::VerificationResult V2VProxyModule::verifyCarPosition(const std::string& carId,
    const std::string& claimedLane,
    double claimedPosition,
    double tolerance) {
    // mobility comes from DemoBaseApplLayer (veins) - this node's TraCIMobility
    if (!mobility) {
        return {false, "NO_TRACI"};
    }
    TraCICommandInterface* traci = mobility->getCommandInterface();
    if (!traci) {
        return {false, "NO_TRACI"};
    }
    // Check vehicle exists (vehicle(id) returns by value, no null)
    std::list<std::string> ids = traci->getVehicleIds();
    if (std::find(ids.begin(), ids.end(), carId) == ids.end()) {
        return {false, "NO_VEHICLE"};
    }
    // vehicle(carId) returns Vehicle by value (TraCICommandInterface::Vehicle)
    TraCICommandInterface::Vehicle targetVeh = traci->vehicle(carId);
    std::string actualLane = targetVeh.getLaneId();
    double actualPosition = targetVeh.getLanePosition();

    if (actualLane != claimedLane) {
        std::cout << "[VERIFY] Replica " << replicaId << " WRONG_LANE: actualLane=" << actualLane << " claimedLane=" << claimedLane << "\n";
        return {false, "WRONG_LANE", actualLane, actualPosition};
    }
    if (std::abs(actualPosition - claimedPosition) > tolerance) {
        std::cout << "[VERIFY] Replica " << replicaId << " WRONG_POSITION: actualPosition=" << actualPosition << " claimedPosition=" << claimedPosition << " tolerance=" << tolerance << "\n";
        return {false, "WRONG_POSITION", actualLane, actualPosition};
    }
    std::cout << "[VERIFY] Replica " << replicaId << " OK: actualLane=" << actualLane << " actualPosition=" << actualPosition << " claimedLane=" << claimedLane << " claimedPosition=" << claimedPosition << " tolerance=" << tolerance << "\n";
    return {true, "OK", actualLane, actualPosition};
}

// TraCI Vehicle::getLeader(distance) returns (leaderId, distanceToLeader).
// If leaderId is empty, there is no vehicle ahead within that distance -> car is at front of lane.

std::vector<uint8_t> V2VProxyModule::signArrivalClaim(const ArrivalAnnouncement& announcement) {
    // ECDSA P-256 self-signed claim (replaces legacy XXHash32 MAC). Aligns with the
    // IEEE 1609.2 / ETSI TS 103 097 V2X security profile already used by the
    // ambulance Emergency_CA cert path.
    std::string data = announcement.carId + ":" + announcement.laneId + ":" +
                       std::to_string(announcement.positionInLane) + ":" +
                       std::to_string(announcement.claimedArrivalTime) + ":" +
                       std::to_string(announcement.epoch);
    if (!replicaPrivateKey) {
        std::cerr << "[CRYPTO] Replica " << replicaId
                  << " signArrivalClaim called before replica keypair was initialized" << "\n";
        return {};
    }
    EVP_PKEY* pk = static_cast<EVP_PKEY*>(replicaPrivateKey);
    uint8_t sigOut[CRYPTO_SIG_MAX_BYTES];
    uint8_t sigLen = 0;
    if (!CryptoAuth::instance().signBytes(pk,
            reinterpret_cast<const uint8_t*>(data.c_str()), data.size(),
            sigOut, sigLen)) {
        std::cerr << "[CRYPTO] Replica " << replicaId
                  << " signArrivalClaim: ECDSA sign failed" << "\n";
        return {};
    }
    return std::vector<uint8_t>(sigOut, sigOut + sigLen);
}

void V2VProxyModule::attachAmbulanceCryptoToAnnouncement(ArrivalAnnouncement& ann)
{
    ann.isAmbulance = false;
    ann.ambulanceCertBytes.clear();
    ann.ambulanceSigBytes.clear();
    if (!moduleIsAmbulance)
        return;

    ann.isAmbulance = true;
    ann.ambulanceCertBytes = myAmbulanceCertBytes;
    if (!ambulancePrivateKey || myAmbulanceCertBytes.size() != sizeof(VehicleCert)) {
        std::cerr << "[AMBULANCE] Replica " << replicaId << " attach failed: key="
                  << (ambulancePrivateKey ? "ok" : "null") << " certBytes="
                  << myAmbulanceCertBytes.size() << " expected " << sizeof(VehicleCert) << "\n";
        return;
    }

    const std::string ambPayload = ann.carId + ":" + ann.lane + ":"
                                 + std::to_string(ann.positionInLane) + ":"
                                 + dirToStr(ann.direction) + ":AMBULANCE";
    uint8_t sigOut[CRYPTO_SIG_MAX_BYTES];
    uint8_t sigLen = 0;
    EVP_PKEY* pk = static_cast<EVP_PKEY*>(ambulancePrivateKey);
    if (!CryptoAuth::instance().signBytes(pk,
            reinterpret_cast<const uint8_t*>(ambPayload.c_str()), ambPayload.size(),
            sigOut, sigLen)) {
        std::cerr << "[AMBULANCE] Replica " << replicaId << " signBytes failed for payload=" << ambPayload
                  << "\n";
        return;
    }
    ann.ambulanceSigBytes.assign(sigOut, sigOut + sigLen);
}

std::vector<uint8_t> V2VProxyModule::generateByzantinePayload(
    ByzantineType type, const ArrivalAnnouncement& honest, int targetReplicaId)
{
    switch (type) {

        case BYZANTINE_FALSE_LANE: {
            // Claim a completely wrong lane so verifyCarPosition() rejects this car
            // and no honest node will witness it — it will be excluded from ORDER.
            ArrivalAnnouncement corrupted = honest;
            corrupted.laneId = "BYZANTINE_FAKE_LANE";
            corrupted.signature = signArrivalClaim(corrupted); // internally consistent but wrong
            std::cout << "[BYZANTINE] Replica " << replicaId
                      << " FALSE_LANE: claiming laneId=" << corrupted.laneId
                      << " (real=" << honest.laneId << ")" << "\n";
            return serializeArrivalAnnouncement(corrupted);
        }

        case BYZANTINE_INVALID_SIG: {
            // Real position data but a garbage 4-byte signature.
            // The Java QC assembler will reject this car's self-signed claim.
            ArrivalAnnouncement corrupted = honest;
            corrupted.signature = std::vector<uint8_t>(4, 0xDE); // 0xDE 0xDE 0xDE 0xDE
            std::cout << "[BYZANTINE] Replica " << replicaId
                      << " INVALID_SIG: broadcasting corrupt signature" << "\n";
            return serializeArrivalAnnouncement(corrupted);
        }

        case BYZANTINE_EQUIVOCATOR: {
            // Split direction: targetReplicaId >= 0 → first half (dir='L'),
            //                  targetReplicaId < 0  → second half (dir='R').
            ArrivalAnnouncement corrupted = honest;
            if (targetReplicaId >= 0) {
                corrupted.direction = DIR_LEFT;
                std::cout << "[BYZANTINE] Replica " << replicaId
                          << " EQUIVOCATOR: sending dir=L to peer " << targetReplicaId << "\n";
            } else {
                corrupted.direction = DIR_RIGHT;
                std::cout << "[BYZANTINE] Replica " << replicaId
                          << " EQUIVOCATOR: sending dir=R to second-half peer\n";
            }
            corrupted.signature = signArrivalClaim(corrupted);
            return serializeArrivalAnnouncement(corrupted);
        }

        default: // BYZANTINE_HONEST — should not reach here
            return serializeArrivalAnnouncement(honest);
    }
}

void V2VProxyModule::broadcastArrivalAnnouncement() {
   
    // ZOMBIE FILTER: Departed cars don't broadcast arrival announcements
    if (zombieFilter()) return;
    std::string myCarId = "veh" + std::to_string(replicaId);
    ArrivalAnnouncement announcement;
    announcement.carId = myCarId;
    // Get lane info from TraCI Vehicle interface
    if (!mobility) {
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: No mobility for broadcastArrivalAnnouncement" << "\n";
        return;
    }
    TraCICommandInterface* traci = mobility->getCommandInterface();
    if (!traci) {
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: No TraCI interface" << "\n";
        return;
    }
    TraCICommandInterface::Vehicle myVeh = traci->vehicle(myCarId);
    announcement.laneId = myVeh.getLaneId();
    // Derive cardinal lane from TraCI lane ID (first character: 'n','s','e','w' prefix)
    // Convention: lane IDs start with the approach direction letter in upper-case
    {
        std::string lid = announcement.laneId;
        if (!lid.empty()) {
            char c = std::toupper(lid[0]);
            announcement.lane = (c == 'N' || c == 'S' || c == 'E' || c == 'W')
                                 ? std::string(1, c) : "N"; // default fallback
        } else { announcement.lane = "N"; }
    }
    // positionInLane: integer rank (1 = front). Derive from TraCI position and lane queue.
    // For now, use lane queue rank if available; fallback to 1.
    {
        int rank = 1;
        if (!laneQueue.empty()) {
            auto it = std::find(laneQueue.begin(), laneQueue.end(), myCarId);
            if (it != laneQueue.end()) rank = (int)(it - laneQueue.begin()) + 1;
        }
        announcement.positionInLane = rank;
    }
    // Direction: read from omnetpp.ini parameter "intendedDirection" (S/L/R), default S
    {
        std::string dirParam = par("intendedDirection").stdstringValue();
        announcement.direction = strToDir(dirParam);
    }
    attachAmbulanceCryptoToAnnouncement(announcement);
    announcement.claimedArrivalTime = simTime().dbl();
    announcement.epoch = currentEpoch;
    announcement.signature = signArrivalClaim(announcement);

    // CRITICAL: Serialize then deserialize to ensure we store the same precision as witnesses will see!
    std::vector<uint8_t> payload = serializeArrivalAnnouncement(announcement);

    // Create a mock BFTMessage for deserialization
    BFTMessage* mockMsg = new BFTMessage();
    mockMsg->setPayloadArraySize(payload.size());
    for (size_t i = 0; i < payload.size(); i++) {
        mockMsg->setPayload(i, payload[i]);
    }
    ArrivalAnnouncement canonicalAnn = deserializeArrivalAnnouncement(mockMsg);
    delete mockMsg;

    // Store the canonical (serialized/deserialized) version
    // Self-store in localVehicleStates and physicallyObservedCars (I know I am physically here)
    {
        VehicleState selfVS;
        selfVS.vehicleId    = canonicalAnn.carId;
        selfVS.lane         = canonicalAnn.lane;
        selfVS.positionInLane = canonicalAnn.positionInLane;
        selfVS.direction    = canonicalAnn.direction;
        selfVS.isAmbulance  = canonicalAnn.isAmbulance;
        localVehicleStates[myCarId]  = selfVS;
        arrivalAnnouncementsReceived.insert(myCarId);
        physicallyObservedCars.insert(myCarId);
    }

    if (isByzantine && byzantineType != BYZANTINE_HONEST) {
        if (byzantineType == BYZANTINE_EQUIVOCATOR) {
            // Collect peers sorted by vehicle ID (MAC order), then split in half:
            // first half receives direction 'L', second half receives direction 'R'.
            std::set<std::string> targetsSet = establishedView.empty()
                ? std::set<std::string>() : establishedView;
            if (targetsSet.empty()) {
                std::lock_guard<std::mutex> lock(registryMutex);
                for (const auto& kv : replicaProxyMap) {
                    if (kv.first != replicaId)
                        targetsSet.insert("veh" + std::to_string(kv.first));
                }
            }
            std::vector<std::string> targets(targetsSet.begin(), targetsSet.end());
            std::sort(targets.begin(), targets.end());
            size_t half = targets.size() / 2;

            // Build both direction variants: vehId|lane|pos|L|isAmb and vehId|lane|pos|R|isAmb
            ArrivalAnnouncement annL = canonicalAnn;
            annL.direction = DIR_LEFT;
            annL.signature = signArrivalClaim(annL);
            std::vector<uint8_t> payloadL = serializeArrivalAnnouncement(annL);

            ArrivalAnnouncement annR = canonicalAnn;
            annR.direction = DIR_RIGHT;
            annR.signature = signArrivalClaim(annR);
            std::vector<uint8_t> payloadR = serializeArrivalAnnouncement(annR);

            for (size_t i = 0; i < targets.size(); ++i) {
                int peerId = extractReplicaIdFromCarId(targets[i]);
                if (peerId < 0 || peerId == replicaId) continue;
                const std::vector<uint8_t>& pkt = (i < half) ? payloadL : payloadR;
                sendBFTMessage(replicaId, peerId, pkt, 1);
            }
            std::cout << "[BYZANTINE] Replica " << replicaId
                      << " EQUIVOCATOR: sent dir=L to first " << half
                      << " peer(s), dir=R to remaining " << (targets.size() - half) << " peer(s)\n";
        } else {
            // FALSE_LANE or INVALID_SIG: single broadcast with corrupted payload
            std::vector<uint8_t> byzantinePayload =
                generateByzantinePayload(byzantineType, canonicalAnn, -1);
            sendBFTMessage(replicaId, -1, byzantinePayload, 1);
        }
    } else {
        sendBFTMessage(replicaId, -1, payload, 1);  // Broadcast, type=1
    }
    std::cout << "[ANN-BROADCAST] Replica " << replicaId << " (" << myCarId << ") broadcast arrival announcement at t=" << simTime() << "\n";
}


void V2VProxyModule::handleArrivalAnnouncement(BFTMessage* bftMsg) {
    ArrivalAnnouncement ann = deserializeArrivalAnnouncement(bftMsg);

    // Stage 11: if intersection is locked (EXECUTING in progress), buffer new arrivals
    // that are not already part of the pendingBatches schedule.
    if (intersectionLocked) {
        bool inSchedule = false;
        for (const auto& batch : pendingBatches) {
            for (const auto& carId : batch) {
                if (carId == ann.carId) { inSchedule = true; break; }
            }
            if (inSchedule) break;
        }
        if (!inSchedule) {
            // New car arrived while intersection is locked
            if (std::find(bufferedNewArrivals.begin(), bufferedNewArrivals.end(), ann.carId)
                    == bufferedNewArrivals.end()) {
                bufferedNewArrivals.push_back(ann.carId);
                std::cout << "[ANN-RECV] Replica " << replicaId
                          << " BUFFERED new arrival " << ann.carId
                          << " (intersectionLocked, pending preemption)\n";
            }
            checkPreemptionConditions();
            return;
        }
    }

    // Dedup: skip if we already have a VehicleState for this car
    if (localVehicleStates.count(ann.carId)) {
        std::cout << "[ANN-RECV] Replica " << replicaId << " DEDUP: already have VehicleState for " << ann.carId << "\n";
        return;
    }

    // In the new protocol positionInLane is a rank (1=front), not a lane-position in metres.
    // verifyCarPosition checks lane membership + numeric distance; pass tolerance=1e9 to
    // effectively skip the distance check and only verify that the car is in the claimed lane.
    VerificationResult result = verifyCarPosition(ann.carId, ann.laneId, ann.positionInLane, 1e9);

    // ---- If the lane claim is wrong, the car is still physically present ----
    // Add it to physicallyObservedCars with its TraCI-verified actual lane, but
    // do NOT echo — no honest replica will sign a false lane claim, so the car
    // will accumulate zero valid echoes and be marked QUIET in the proposal.
    if (!result.isValid) {
        if (result.actualLaneId.empty()) {
            // Car not found via TraCI at all (NO_VEHICLE / NO_TRACI) — truly absent.
            std::cout << "[ANN-RECV] Replica " << replicaId << " ABSENT (not in TraCI): " << ann.carId << "\n";
            return;
        }
        // Car IS physically present but is lying about its lane.
        // Derive cardinal direction from the actual TraCI lane ID.
        std::string actualCardinal;
        {
            char c = result.actualLaneId.empty() ? 'N' : std::toupper(result.actualLaneId[0]);
            actualCardinal = (c == 'N' || c == 'S' || c == 'E' || c == 'W')
                             ? std::string(1, c) : "N";
        }
        std::cout << "[ANN-RECV] Replica " << replicaId << " FALSE_LANE from " << ann.carId
                  << ": claimed=" << ann.lane << " actual=" << actualCardinal
                  << " — recording as QUIET (no echo sent)\n";
        if (!localVehicleStates.count(ann.carId)) {
            VehicleState vs;
            vs.vehicleId      = ann.carId;
            vs.lane           = actualCardinal;
            vs.positionInLane = ann.positionInLane;  // rank unchanged
            vs.direction      = ann.direction;        // direction claim kept (can't verify)
            vs.isAmbulance    = false;                // don't trust ambulance claim from liar
            localVehicleStates[ann.carId] = vs;
            arrivalAnnouncementsReceived.insert(ann.carId);
            physicallyObservedCars.insert(ann.carId);
        }
        // Fall through to timer-start logic below (no echo).
        size_t n = physicallyObservedCars.size();
        if (!certCollectionStarted && !proposeAllSubmitted && amITheLeader(physicallyObservedCars)) {
            certCollectionStarted = true;
            certCollectionStartTime = simTime();
            std::cout << "[V2VProxy " << replicaId << "] Leader: started cert collection timer on first observation"
                      << " (have " << n << "/" << BATCH_SIZE << " observed, timeout="
                      << certCollectionTimeoutSec << "s)\n";
            if (certCollectionTimeoutTimer && !certCollectionTimeoutTimer->isScheduled())
                scheduleAt(simTime() + certCollectionTimeoutSec, certCollectionTimeoutTimer);
        }
        return;
    }

    // ---- Ambulance certificate verification ----
    bool effectiveIsAmbulance = ann.isAmbulance;
    if (ann.isAmbulance && ann.ambulanceCertBytes.size() == sizeof(VehicleCert)) {
        VehicleCert cert;
        std::memcpy(&cert, ann.ambulanceCertBytes.data(), sizeof(VehicleCert));
        std::string role = CryptoAuth::instance().verifyCert(cert);
        if (role != "ambulance") {
            std::cerr << "[ANN-RECV] Replica " << replicaId
                      << " DOWNGRADE: cert role='" << role << "' for " << ann.carId << "\n";
            effectiveIsAmbulance = false;
        } else if (!ann.ambulanceSigBytes.empty() &&
                   ann.ambulanceSigBytes.size() <= CRYPTO_SIG_MAX_BYTES) {
            // Verify self-signature over "vehicleId:lane:posInLane:direction:AMBULANCE"
            std::string payload = ann.carId + ":" + ann.lane + ":"
                                + std::to_string(ann.positionInLane) + ":"
                                + dirToStr(ann.direction) + ":AMBULANCE";
            bool sigOk = CryptoAuth::instance().verifyBytes(
                cert.publicKey,
                reinterpret_cast<const uint8_t*>(payload.c_str()), payload.size(),
                ann.ambulanceSigBytes.data(),
                static_cast<uint8_t>(ann.ambulanceSigBytes.size()));
            if (!sigOk) {
                std::cerr << "[ANN-RECV] Replica " << replicaId
                          << " DOWNGRADE: ambulance sig invalid for " << ann.carId << "\n";
                effectiveIsAmbulance = false;
            }
        } else {
            effectiveIsAmbulance = false; // cert present but no sig to verify
        }
    } else if (ann.isAmbulance && !ann.ambulanceCertBytes.empty()) {
        // Cert was provided but wrong size → downgrade (malformed cert)
        std::cerr << "[ANN-RECV] Replica " << replicaId
                  << " DOWNGRADE: ambulance cert wrong size (" << ann.ambulanceCertBytes.size()
                  << " vs " << sizeof(VehicleCert) << ") for " << ann.carId << "\n";
        effectiveIsAmbulance = false;
    }
    // If ambulanceCertBytes is empty, trust ann.isAmbulance as-is (no-cert testing)

    // ---- Build and store VehicleState ----
    VehicleState vs;
    vs.vehicleId      = ann.carId;
    vs.lane           = ann.lane;
    vs.positionInLane = ann.positionInLane;
    vs.direction      = ann.direction;
    vs.isAmbulance    = effectiveIsAmbulance;

    localVehicleStates[ann.carId] = vs;
    arrivalAnnouncementsReceived.insert(ann.carId);

    // Physical check passed — add to observed set and send echo back to announcing car
    physicallyObservedCars.insert(ann.carId);
    sendArrivalEcho(ann);

    size_t n        = physicallyObservedCars.size();
    size_t expected = BATCH_SIZE;
    std::cout << "[ANN-RECV] Replica " << replicaId << " stored VehicleState for " << ann.carId
              << " (have " << n << "/" << expected << " physically observed)\n";

    // ---- Leader: start cert collection timer on FIRST observation ----
    // We always expect BATCH_SIZE certs. A Byzantine car may never pass the physical
    // check (so physicallyObservedCars stays < BATCH_SIZE), but the timer must still
    // fire so that missing cars are marked QUIET rather than stalling the protocol.
    if (!certCollectionStarted && !proposeAllSubmitted && amITheLeader(physicallyObservedCars)) {
        certCollectionStarted = true;
        certCollectionStartTime = simTime();
        std::cout << "[V2VProxy " << replicaId << "] Leader: started cert collection timer on first observation"
                  << " (have " << n << "/" << BATCH_SIZE << " observed, timeout="
                  << certCollectionTimeoutSec << "s)\n";
        if (certCollectionTimeoutTimer && !certCollectionTimeoutTimer->isScheduled()) {
            scheduleAt(simTime() + certCollectionTimeoutSec, certCollectionTimeoutTimer);
        }
    }
}

// ============================================================================
// ARRIVAL_ECHO / ARRIVAL_CERT SERIALIZATION
// ============================================================================

// Wire format for ARRIVAL_ECHO (all text, pipe-delimited):
//   echoingReplicaId|targetCarId|lane|positionInLane|direction|isAmbulance|epoch|signerPubKeyHex,sigHex
std::vector<uint8_t> V2VProxyModule::serializeArrivalEcho(const ArrivalEcho& echo) {
    std::vector<uint8_t> pubVec(echo.signerPubKey, echo.signerPubKey + CRYPTO_PUBKEY_BYTES);
    std::vector<uint8_t> sigVec(echo.signature, echo.signature + echo.signatureLen);
    std::stringstream ss;
    ss << echo.echoingReplicaId << "|"
       << echo.targetCarId     << "|"
       << echo.lane            << "|"
       << echo.positionInLane  << "|"
       << dirToStr(echo.direction) << "|"
       << (echo.isAmbulance ? "1" : "0") << "|"
       << echo.epoch           << "|"
       << toHex(pubVec) << "," << toHex(sigVec);
    std::string s = ss.str();
    return std::vector<uint8_t>(s.begin(), s.end());
}

V2VProxyModule::ArrivalEcho V2VProxyModule::deserializeArrivalEcho(BFTMessage* bftMsg) {
    std::vector<uint8_t> payload(bftMsg->getPayloadArraySize());
    for (size_t i = 0; i < payload.size(); i++) payload[i] = bftMsg->getPayload(i);
    std::string s(payload.begin(), payload.end());
    std::vector<std::string> parts = split(s, '|');
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
        // parts[7] = "<pubKeyHex>,<sigHex>"
        const std::string& sigField = parts[7];
        size_t comma = sigField.find(',');
        if (comma != std::string::npos) {
            std::vector<uint8_t> pubVec = fromHex(sigField.substr(0, comma));
            std::vector<uint8_t> sigVec = fromHex(sigField.substr(comma + 1));
            if (pubVec.size() == CRYPTO_PUBKEY_BYTES) {
                std::memcpy(echo.signerPubKey, pubVec.data(), CRYPTO_PUBKEY_BYTES);
            }
            if (sigVec.size() <= CRYPTO_SIG_MAX_BYTES) {
                std::memcpy(echo.signature, sigVec.data(), sigVec.size());
                echo.signatureLen = static_cast<uint8_t>(sigVec.size());
            }
        }
    }
    return echo;
}

// Wire format for ARRIVAL_CERT (all text, pipe-delimited):
//   carId|lane|positionInLane|direction|isAmbulance|epoch
//        |replicaId1:pubKeyHex1,sigHex1|replicaId2:pubKeyHex2,sigHex2|...
std::vector<uint8_t> V2VProxyModule::serializeArrivalCert(const ArrivalCert& cert) {
    std::stringstream ss;
    ss << cert.carId           << "|"
       << cert.lane            << "|"
       << cert.positionInLane  << "|"
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

V2VProxyModule::ArrivalCert V2VProxyModule::deserializeArrivalCert(BFTMessage* bftMsg) {
    std::vector<uint8_t> payload(bftMsg->getPayloadArraySize());
    for (size_t i = 0; i < payload.size(); i++) payload[i] = bftMsg->getPayload(i);
    std::string s(payload.begin(), payload.end());
    std::vector<std::string> parts = split(s, '|');
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
        // Right side of ':' is "<pubKeyHex>,<sigHex>"
        std::string sigField = parts[i].substr(colon + 1);
        size_t comma = sigField.find(',');
        if (comma != std::string::npos) {
            std::vector<uint8_t> pubVec = fromHex(sigField.substr(0, comma));
            std::vector<uint8_t> sigVec = fromHex(sigField.substr(comma + 1));
            if (pubVec.size() == CRYPTO_PUBKEY_BYTES) {
                std::memcpy(echo.signerPubKey, pubVec.data(), CRYPTO_PUBKEY_BYTES);
            }
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



// ============================================================================
// READYQC SERIALIZATION (Phase 2)
// ============================================================================

std::vector<uint8_t> V2VProxyModule::serializeArrivalAnnouncement(const ArrivalAnnouncement& ann) {
    // New format: carId|laneId|lane|posInLane|direction|isAmbulance|time|epoch
    //             |certHex|sigHex|selfSigLen|selfSig
    // Fields 0-7 are text; certHex/sigHex are empty strings when !isAmbulance.
    std::stringstream ss;
    ss << std::setprecision(17);
    ss << ann.carId           << "|"
       << ann.laneId          << "|"
       << ann.lane            << "|"
       << ann.positionInLane  << "|"
       << dirToStr(ann.direction) << "|"
       << (ann.isAmbulance ? "1" : "0") << "|"
       << ann.claimedArrivalTime << "|"
       << ann.epoch           << "|"
       << toHex(ann.ambulanceCertBytes) << "|"
       << toHex(ann.ambulanceSigBytes)  << "|"
       << ann.signature.size() << "|";

    std::string header = ss.str();
    std::vector<uint8_t> result(header.begin(), header.end());
    result.insert(result.end(), ann.signature.begin(), ann.signature.end());
    return result;
}

V2VProxyModule::ArrivalAnnouncement V2VProxyModule::deserializeArrivalAnnouncement(BFTMessage* bftMsg) {
    std::vector<uint8_t> payload(bftMsg->getPayloadArraySize());
    for (size_t i = 0; i < payload.size(); i++) payload[i] = bftMsg->getPayload(i);

    std::string s(payload.begin(), payload.end());
    std::vector<std::string> parts = split(s, '|');

    ArrivalAnnouncement ann;
    // Minimum fields: 0..10 text + binary self-sig
    if (parts.size() >= 11) {
        ann.carId             = parts[0];
        ann.laneId            = parts[1];
        ann.lane              = parts[2];
        ann.positionInLane    = std::stoi(parts[3]);
        ann.direction         = strToDir(parts[4]);
        ann.isAmbulance       = (parts[5] == "1");
        ann.claimedArrivalTime= std::stod(parts[6]);
        ann.epoch             = std::stoi(parts[7]);
        ann.ambulanceCertBytes= fromHex(parts[8]);
        ann.ambulanceSigBytes = fromHex(parts[9]);

        int siglen = std::stoi(parts[10]);
        // Locate 11th '|' to find the binary self-sig offset
        size_t p = s.find('|');
        for (int k = 1; k < 11 && p != std::string::npos; ++k) p = s.find('|', p + 1);
        size_t offset = (p != std::string::npos) ? p + 1 : s.size();
        if (offset < payload.size() && offset + (size_t)siglen <= payload.size()) {
            ann.signature.assign(payload.begin() + offset, payload.begin() + offset + siglen);
        }
    }
    return ann;
}

std::set<std::string> V2VProxyModule::getVisibleVehicles(double maxRange) {
    
    // ZOMBIE FILTER: Departed cars don't see anyone
    if (isDeparted) {
        std::set<std::string> visible;
        visible.insert("veh" + std::to_string(replicaId));  // Only themselves
        return visible;
    }
    
    std::set<std::string> visible;
    
    if (!mobility) {
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: No mobility for getVisibleVehicles" << "\n";
        return visible;
    }
    
    TraCICommandInterface* traci = mobility->getCommandInterface();
    if (!traci) {
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: No TraCI for getVisibleVehicles" << "\n";
        return visible;
    }
    
    std::string myCarId = "veh" + std::to_string(replicaId);
    visible.insert(myCarId);
    
    // Get all vehicles in simulation
    std::list<std::string> allIds = traci->getVehicleIds();
    
    // Get my vehicle's position
    Coord myPos = mobility->getPositionAt(simTime());
    
    // Use OMNeT++ module hierarchy to find other vehicles
    cModule* network = getSimulation()->getSystemModule();
    if (!network) return visible;

    // Iterate through all vehicles in the network
    for (cModule::SubmoduleIterator it(network); !it.end(); ++it) {
        cModule* veh = *it;
        if (!veh) continue;

        // Find the V2VProxyModule in this vehicle (usually under "appl")
        cModule* applModule = veh->getSubmodule("appl");
        if (!applModule) continue;

        V2VProxyModule* otherProxy = dynamic_cast<V2VProxyModule*>(applModule);
        if (!otherProxy || otherProxy == this) {
           continue;
        }  // skip myself

        // Get the other vehicle's mobility
        if (!otherProxy->mobility) continue;

        if (otherProxy->isDeparted) continue;

        // Get other vehicle's position
        Coord otherPos = otherProxy->mobility->getPositionAt(simTime());

        // Calculate Euclidean distance
        double distance = myPos.distance(otherPos);

        // Filter by maxRange
        if (distance <= maxRange) {
            std::string otherId = "veh" + std::to_string(otherProxy->replicaId);
            visible.insert(otherId);
        }
    }
    
    return visible;
}

// Build the canonical semicolon-pipe vehicleStates string from the local localVehicleStates map.
// 5-field format (no certs): "veh0|N|1|S|0;veh1|S|1|L|0;veh2|W|2|R|1"
// 6-field format (certs provided): "veh0|N|1|S|0|SIGNED;veh1|S|1|L|0|QUIET;..."
//   A vehicle is SIGNED iff its carId appears as a key in the certs map.
//   All others are QUIET (physically present but could not produce a valid ARRIVAL_CERT).
std::string V2VProxyModule::buildVehicleStatesStr(const std::map<std::string, ArrivalCert>* certs) const {
    std::vector<std::pair<std::string, VehicleState>> sorted(localVehicleStates.begin(), localVehicleStates.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    std::string result;
    for (const auto& kv : sorted) {
        const VehicleState& vs = kv.second;
        if (!result.empty()) result += ";";
        result += vs.vehicleId + "|" + vs.lane + "|"
               + std::to_string(vs.positionInLane) + "|"
               + dirToStr(vs.direction) + "|"
               + (vs.isAmbulance ? "1" : "0");
        if (certs != nullptr) {
            bool isSigned = (certs->count(vs.vehicleId) > 0);
            result += "|";
            result += (isSigned ? "SIGNED" : "QUIET");
        }
    }
    return result;
}

// Use the AGREED view (not live visibility) so all replicas elect the same leader.

// ============================================================================
// ARRIVAL_ECHO / ARRIVAL_CERT HANDLERS
// ============================================================================

void V2VProxyModule::sendArrivalEcho(const ArrivalAnnouncement& ann) {
    if (zombieFilter()) return;
    std::string myCarId = "veh" + std::to_string(replicaId);

    // ECDSA P-256 sign over UTF-8(targetCarId:lane:pos:dir:isAmb:echoingReplicaId).
    // Embed the signer's compressed pubkey so any peer (C++ or Java) can verify
    // self-containedly (IEEE 1609.2 V2X model).
    std::string toSign = ann.carId + ":" + ann.lane + ":"
        + std::to_string(ann.positionInLane) + ":"
        + (ann.direction == DIR_LEFT ? "L" : ann.direction == DIR_RIGHT ? "R" : "S")
        + ":" + (ann.isAmbulance ? "1" : "0")
        + ":" + std::to_string(replicaId);

    ArrivalEcho echo;
    echo.echoingReplicaId = replicaId;
    echo.targetCarId      = ann.carId;
    echo.lane             = ann.lane;
    echo.positionInLane   = ann.positionInLane;
    echo.direction        = ann.direction;
    echo.isAmbulance      = ann.isAmbulance;
    echo.epoch            = ann.epoch;
    std::memcpy(echo.signerPubKey, myReplicaPubKey, CRYPTO_PUBKEY_BYTES);
    std::memset(echo.signature, 0, CRYPTO_SIG_MAX_BYTES);
    echo.signatureLen = 0;

    if (!replicaPrivateKey) {
        std::cerr << "[ECHO-SEND] Replica " << replicaId
                  << " has no replica keypair; aborting echo for " << ann.carId << "\n";
        return;
    }
    EVP_PKEY* pk = static_cast<EVP_PKEY*>(replicaPrivateKey);
    if (!CryptoAuth::instance().signBytes(pk,
            reinterpret_cast<const uint8_t*>(toSign.c_str()), toSign.size(),
            echo.signature, echo.signatureLen)) {
        std::cerr << "[ECHO-SEND] Replica " << replicaId
                  << " ECDSA signBytes failed for echo of " << ann.carId << "\n";
        return;
    }

    std::vector<uint8_t> payload = serializeArrivalEcho(echo);
    int targetReplicaId = extractReplicaIdFromCarId(ann.carId);
    sendBFTMessage(replicaId, targetReplicaId, payload, 4);  // msgType=4 ARRIVAL_ECHO (unicast)
    std::cout << "[ECHO-SEND] Replica " << replicaId << " -> " << ann.carId
              << " ARRIVAL_ECHO ECDSA sigLen=" << (int)echo.signatureLen << "\n";
}

void V2VProxyModule::handleArrivalEcho(BFTMessage* bftMsg) {
    if (zombieFilter()) return;
    ArrivalEcho echo = deserializeArrivalEcho(bftMsg);
    std::string myCarId = "veh" + std::to_string(replicaId);

    // Only process echoes for MY own announcement
    if (echo.targetCarId != myCarId) return;
    if (certBroadcast) return;  // already sent my cert

    auto& echoes = myReceivedEchoes[myCarId];

    // Dedup: one echo per echoingReplicaId
    for (const auto& e : echoes) {
        if (e.echoingReplicaId == echo.echoingReplicaId) return;
    }

    echoes.push_back(echo);
    std::cout << "[ECHO-RECV] Replica " << replicaId << " received echo from replica "
              << echo.echoingReplicaId << " (" << echoes.size() << " so far)\n";

    // Check if we have f+1 echoes. Use BATCH_SIZE-based f.
    int f = (BATCH_SIZE - 1) / 3;
    int required = f + 1;

    if ((int)echoes.size() >= required) {
        certBroadcast = true;
        // Assemble ARRIVAL_CERT from my own localVehicleStates entry
        ArrivalCert cert;
        cert.carId = myCarId;
        if (localVehicleStates.count(myCarId)) {
            const VehicleState& selfVS = localVehicleStates.at(myCarId);
            cert.lane           = selfVS.lane;
            cert.positionInLane = selfVS.positionInLane;
            cert.direction      = selfVS.direction;
            cert.isAmbulance    = selfVS.isAmbulance;
        }
        cert.epoch  = currentEpoch;
        cert.echoes = echoes;
        std::cout << "[CERT-ASSEMBLE] Replica " << replicaId << " assembled ARRIVAL_CERT with "
                  << cert.echoes.size() << " echoes — broadcasting\n";
        broadcastArrivalCert(cert);
    }
}

void V2VProxyModule::broadcastArrivalCert(const ArrivalCert& cert) {
    if (zombieFilter()) return;
    std::vector<uint8_t> payload = serializeArrivalCert(cert);
    sendBFTMessage(replicaId, -1, payload, 5);  // msgType=5 ARRIVAL_CERT (broadcast)
    std::cout << "[CERT-BROADCAST] Replica " << replicaId << " broadcast ARRIVAL_CERT for "
              << cert.carId << "\n";

    // OMNeT++ modules do NOT receive their own channel broadcasts.
    // Self-store so we (the sender) also have our cert in collectedCerts.
    if (!collectedCerts.count(cert.carId)) {
        collectedCerts[cert.carId] = cert;
        std::cout << "[CERT-STORED-SELF] Replica " << replicaId << " self-stored ARRIVAL_CERT for "
                  << cert.carId << " (" << collectedCerts.size() << "/" << BATCH_SIZE << " certs)\n";
        // If we are the leader and this was the last missing cert, submit immediately.
        if (amITheLeader(physicallyObservedCars) && certCollectionStarted && !proposeAllSubmitted) {
            if (collectedCerts.size() >= (size_t)BATCH_SIZE) {
                if (certCollectionTimeoutTimer && certCollectionTimeoutTimer->isScheduled())
                    cancelEvent(certCollectionTimeoutTimer);
                proposeAllSubmitted = true;
                std::cout << "[V2VProxy " << replicaId << "] ===== ALL CERTS COLLECTED (via self-store): SUBMITTING TO BFT =====\n";
                submitProposeAllToBFT();
            }
        }
    }
}

void V2VProxyModule::handleArrivalCert(BFTMessage* bftMsg) {
    if (zombieFilter()) return;
    ArrivalCert cert = deserializeArrivalCert(bftMsg);

    if (!validateArrivalCert(cert)) {
        std::cout << "[CERT-INVALID] Replica " << replicaId << " INVALID ARRIVAL_CERT from "
                  << cert.carId << " — dropping\n";
        return;
    }

    if (collectedCerts.count(cert.carId)) return;  // dedup
    collectedCerts[cert.carId] = cert;
    std::cout << "[CERT-STORED] Replica " << replicaId << " stored ARRIVAL_CERT for "
              << cert.carId << " (" << collectedCerts.size() << "/" << BATCH_SIZE << " certs)\n";

    // Leader: check if all BATCH_SIZE cars now have certs (Byzantine cars will be
    // absent and marked QUIET when the timer fires — never use physicallyObservedCars
    // as the expected count, since a liar may have failed the physical check).
    if (amITheLeader(physicallyObservedCars) && certCollectionStarted && !proposeAllSubmitted) {
        if (collectedCerts.size() >= (size_t)BATCH_SIZE) {
            if (certCollectionTimeoutTimer && certCollectionTimeoutTimer->isScheduled()) {
                cancelEvent(certCollectionTimeoutTimer);
            }
            proposeAllSubmitted = true;
            std::cout << "[V2VProxy " << replicaId << "] ===== ALL CERTS COLLECTED: SUBMITTING TO BFT =====\n";
            submitProposeAllToBFT();
        }
    }
}

bool V2VProxyModule::validateArrivalCert(const ArrivalCert& cert) {
    int f = (BATCH_SIZE - 1) / 3;
    int required = f + 1;
    if ((int)cert.echoes.size() < required) {
        std::cout << "[CERT-VALIDATE] " << cert.carId << " has " << cert.echoes.size()
                  << " echoes, need " << required << "\n";
        return false;
    }

    std::set<int> seenEchoers;
    int validCount = 0;
    for (const auto& echo : cert.echoes) {
        if (!seenEchoers.insert(echo.echoingReplicaId).second) continue;  // dedup
        if (echo.signatureLen == 0) continue;  // missing/short-circuit signature
        // Verify ECDSA P-256 signature using the signer's embedded pubkey.
        std::string dirStr = (cert.direction == DIR_LEFT ? "L" :
                              cert.direction == DIR_RIGHT ? "R" : "S");
        std::string toSign = cert.carId + ":" + cert.lane + ":"
            + std::to_string(cert.positionInLane) + ":"
            + dirStr + ":"
            + (cert.isAmbulance ? "1" : "0") + ":"
            + std::to_string(echo.echoingReplicaId);
        bool ok = CryptoAuth::instance().verifyBytes(
            echo.signerPubKey,
            reinterpret_cast<const uint8_t*>(toSign.c_str()), toSign.size(),
            echo.signature, echo.signatureLen);
        if (ok) validCount++;
    }

    if (validCount < required) {
        std::cout << "[CERT-VALIDATE] " << cert.carId << " only " << validCount
                  << " valid sigs (need " << required << ")\n";
        return false;
    }
    return true;
}

int V2VProxyModule::getCurrentViewLeader(const std::set<std::string>& agreedView) {
    if (agreedView.empty()) return -1;
    
    int minId = INT_MAX;
    
    for (const std::string& veh : agreedView) {
        try {
            int currentId = std::stoi(veh.substr(3));
            minId = std::min(minId, currentId);
        } catch (const std::exception& e) {
            EV_ERROR << "[VIEW] Failed to parse vehicle ID from: " << veh << "\n";
        }
    }

    return minId;
}

bool V2VProxyModule::amITheLeader(const std::set<std::string>& agreedView) {
    return (getCurrentViewLeader(agreedView) == replicaId);
}

std::set<std::string> V2VProxyModule::getCertSnapshotKeys() const {
    std::set<std::string> keys;
    for (const auto& kv : collectedCerts)
        keys.insert(kv.first);
    return keys;
}

// Factored out of submitProposeAllToBFT(): builds the "<vsStr>:<perCarCerts>"
// half of the PROPOSE_ALL wire format from the CURRENT collectedCerts snapshot.
// The Java layer tacks on the "PROPOSE_ALL:<proposerId>:" prefix and the
// trailing ":<orderBagStr>" (from OrderScheduler.buildProposal) itself.
std::string V2VProxyModule::buildFreshProposePayload() const {
    std::string vsStr = buildVehicleStatesStr(&collectedCerts);

    // Per-car cert string with key-cache compression:
    //   full: carId~r1,pubKeyHex1,sigHex1|...
    //   ref:  carId~r2,@keyIdHex,sigHex2|...
    // where keyIdHex = first 8 bytes of SHA-256(pubKey). This avoids repeating
    // the same signer pubkey across many echoes in one PROPOSE_ALL payload.
    std::string perCarCertsStr;
    std::map<std::string, std::string> keyIdToPubHex;
    bool firstCar = true;
    for (const auto& kv : collectedCerts) {
        if (!firstCar) perCarCertsStr += ";";
        firstCar = false;
        perCarCertsStr += kv.first + "~";  // carId~
        bool firstSig = true;
        for (const auto& echo : kv.second.echoes) {
            if (!firstSig) perCarCertsStr += "|";
            firstSig = false;
            std::vector<uint8_t> pubVec(echo.signerPubKey,
                                        echo.signerPubKey + CRYPTO_PUBKEY_BYTES);
            std::vector<uint8_t> sigVec(echo.signature,
                                        echo.signature + echo.signatureLen);
            std::string pubHex = toHex(pubVec);
            std::string keyId = shortPubKeyIdHex(echo.signerPubKey, CRYPTO_PUBKEY_BYTES);
            bool emitReference = false;
            if (!keyId.empty()) {
                auto it = keyIdToPubHex.find(keyId);
                if (it == keyIdToPubHex.end()) {
                    keyIdToPubHex[keyId] = pubHex;
                } else if (it->second == pubHex) {
                    emitReference = true;
                } else {
                    // Defensive collision handling: fall back to full pubkey encoding.
                    emitReference = false;
                }
            }
            perCarCertsStr += std::to_string(echo.echoingReplicaId)
                            + "," + (emitReference ? ("@" + keyId) : pubHex)
                            + "," + toHex(sigVec);
        }
    }

    return vsStr + ":" + perCarCertsStr;
}

void V2VProxyModule::submitProposeAllToBFT() {
    proposeAllSubmitTime = simTime();
    realOrderConsensusStart = std::chrono::high_resolution_clock::now();
    std::cout << "[V2VProxy " << replicaId << "] Submitting PROPOSE_ALL to BFT-SMaRt (per-car cert protocol)...\n";
    std::cout << "[METRICS " << replicaId << "] ProposeAll_Submit_Time: " << proposeAllSubmitTime << "\n";

    // Wire format: "PROPOSE_ALL:<proposerId>:<vehicleStatesStr>:<perCarCerts>"
    // vehicleStatesStr: "veh0|N|1|S|0|SIGNED;veh1|S|1|L|0|QUIET;..."
    // perCarCerts:      "veh0~r1,pubKeyHex1,sigHex1|r2,@keyIdHex,sigHex2;veh1~..."
    //   (carId ~ signerReplicaId,(compressedP256PubKeyHex|@keyIdHex),DERECDSASigHex | ... )
    // Java leader verifies SHA256withECDSA over (carId:lane:pos:dir:isAmb:replicaId)
    // for every echo, then appends the schedule before invoking ordered consensus.

    std::string request = "PROPOSE_ALL:" + std::to_string(replicaId)
                        + ":" + buildFreshProposePayload();

    std::cout << "[V2VProxy " << replicaId << "] BFT PROPOSE_ALL: " << request << "\n";

    if (!triggerJoinViaJNI(request)) {
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: Failed to submit PROPOSE_ALL to BFT — storing for retry when Java ready\n";
        pendingProposeAllRequest = request;  // will be retried by checkJavaReadyTimer
    }
}


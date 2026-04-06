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

int32_t computeXXHash32(const std::string& str);
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
        return {false, "WRONG_LANE"};
    }
    if (std::abs(actualPosition - claimedPosition) > tolerance) {
        return {false, "WRONG_POSITION"};
    }
    return {true, "OK"};
}

// TraCI Vehicle::getLeader(distance) returns (leaderId, distanceToLeader).
// If leaderId is empty, there is no vehicle ahead within that distance -> car is at front of lane.

std::vector<uint8_t> V2VProxyModule::signArrivalClaim(const ArrivalAnnouncement& announcement) {
    std::string data = announcement.carId + ":" + announcement.laneId + ":" +
                       std::to_string(announcement.positionInLane) + ":" +
                       std::to_string(announcement.claimedArrivalTime) + ":" +
                       std::to_string(announcement.epoch);
    int32_t hash = computeXXHash32(data);
    std::vector<uint8_t> sig(sizeof(int32_t));
    std::memcpy(sig.data(), &hash, sizeof(int32_t));
    return sig;
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
            // Send epoch N+1 to even replica IDs, honest epoch N to odd replica IDs.
            // From different receivers' perspectives this car is "in different rounds",
            // which violates BFT-SMaRt's agreement property.
            ArrivalAnnouncement corrupted = honest;
            if (targetReplicaId >= 0 && targetReplicaId % 2 == 0) {
                corrupted.epoch = honest.epoch + 1;
                std::cout << "[BYZANTINE] Replica " << replicaId
                          << " EQUIVOCATOR: sending epoch=" << corrupted.epoch
                          << " to peer " << targetReplicaId
                          << " (honest epoch=" << honest.epoch << ")" << "\n";
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
    // Self-store in viewState so handleArrivalAnnouncement's threshold check includes us
    {
        VehicleState selfVS;
        selfVS.vehicleId    = canonicalAnn.carId;
        selfVS.lane         = canonicalAnn.lane;
        selfVS.positionInLane = canonicalAnn.positionInLane;
        selfVS.direction    = canonicalAnn.direction;
        selfVS.isAmbulance  = canonicalAnn.isAmbulance;
        viewState[myCarId]  = selfVS;
        arrivalAnnouncementsReceived.insert(myCarId);
    }

    if (isByzantine && byzantineType != BYZANTINE_HONEST) {
        if (byzantineType == BYZANTINE_EQUIVOCATOR) {
            // Unicast different payloads to each peer so they see different epochs.
            // We iterate the established view; fall back to replicaProxyMap if not yet set.
            std::set<std::string> targets = establishedView.empty()
                ? std::set<std::string>() : establishedView;
            if (targets.empty()) {
                std::lock_guard<std::mutex> lock(registryMutex);
                for (const auto& kv : replicaProxyMap) {
                    if (kv.first != replicaId)
                        targets.insert("veh" + std::to_string(kv.first));
                }
            }
            for (const auto& peerStr : targets) {
                int peerId = extractReplicaIdFromCarId(peerStr);
                if (peerId < 0 || peerId == replicaId) continue;
                std::vector<uint8_t> byzantinePayload =
                    generateByzantinePayload(byzantineType, canonicalAnn, peerId);
                sendBFTMessage(replicaId, peerId, byzantinePayload, 1);
            }
            std::cout << "[BYZANTINE] Replica " << replicaId
                      << " EQUIVOCATOR: sent " << targets.size() << " divergent unicasts\n";
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
    if (viewState.count(ann.carId)) {
        std::cout << "[ANN-RECV] Replica " << replicaId << " DEDUP: already have VehicleState for " << ann.carId << "\n";
        return;
    }

    // In the new protocol positionInLane is a rank (1=front), not a lane-position in metres.
    // verifyCarPosition checks lane membership + numeric distance; pass tolerance=1e9 to
    // effectively skip the distance check and only verify that the car is in the claimed lane.
    VerificationResult result = verifyCarPosition(ann.carId, ann.laneId, ann.positionInLane, 1e9);
    if (!result.isValid) {
        std::cout << "[ANN-RECV] Replica " << replicaId << " INVALID announcement from " << ann.carId << ": " << result.reason << "\n";
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

    viewState[ann.carId] = vs;
    arrivalAnnouncementsReceived.insert(ann.carId);

    size_t n        = viewState.size();
    size_t expected = BATCH_SIZE;
    std::cout << "[ANN-RECV] Replica " << replicaId << " stored VehicleState for " << ann.carId
              << " (have " << n << "/" << expected << " VehicleStates)\n";

    // ---- When all VehicleStates collected: build and broadcast VIEW_PROPOSAL ----
    if (n == expected && currentPhase == PROPOSING_VIEW && !viewEstablished) {
        std::cout << "[ANN-RECV] Replica " << replicaId
                  << " has all " << expected << " VehicleStates — initiating view proposal\n";
        initiateViewProposal();
    }
}

std::vector<uint8_t> V2VProxyModule::serializeViewProposal(const ViewProposal& proposal) {
    // Format: proposerId|vehicleStatesStr|timestamp|siglen|sig
    // vehicleStatesStr: "veh0|N|1|S|0;veh1|S|1|L|0" (semicolon between cars, pipe within)
    std::stringstream ss;
    ss << proposal.proposerReplicaId << "|"
       << proposal.vehicleStatesStr  << "|"
       << proposal.proposalTimestamp << "|"
       << proposal.signature.size() << "|";

    std::string header = ss.str();
    std::vector<uint8_t> result(header.begin(), header.end());
    result.insert(result.end(), proposal.signature.begin(), proposal.signature.end());
    return result;
}

V2VProxyModule::ViewProposal V2VProxyModule::deserializeViewProposal(BFTMessage* bftMsg) {
    std::vector<uint8_t> payload(bftMsg->getPayloadArraySize());
    for (size_t i = 0; i < payload.size(); i++) {
        payload[i] = bftMsg->getPayload(i);
    }

    // Format: proposerId|vehicleStatesStr|timestamp|siglen|sig
    // vehicleStatesStr contains '|' within each car record, so we can't just split('|').
    // Parse by finding the first '|' (proposerId), then scan for the ';'-delimited vehicleStatesStr
    // region, then the remaining fields.
    // Easier: locate field boundaries by counting: field[0] ends at 1st '|', field[3] (siglen)
    // is between the 3rd-from-last and 2nd-from-last '|' before the binary blob.
    // Safest: the text header ends at the 4th '|' that belongs to our fixed fields (proposerId,
    // vehicleStatesStr, timestamp, siglen). But vehicleStatesStr itself contains '|'.
    // Solution: store vehicleStatesStr as field[1] by using a different delimiter.
    // -- The vehicleStatesStr ends at the semicolon sequence; since the outer delimiter is '|'
    //    and the inner car-field delimiter is also '|', we use a different approach:
    //    After proposerId, the vehicleStatesStr ends at the LAST ';'-terminated segment before
    //    the timestamp float. This is ambiguous. Instead: at serialization we ensure
    //    vehicleStatesStr uses ';' between cars and ',' within records to avoid '|' conflicts.
    //    BUT for now vehicleStatesStr uses '|' within records (veh0|N|1|S|0;...).
    //    Simple parse: split on '|', then fields[1..5N] are the vehicleStates records interleaved
    //    with ';' as car separators. The final three '|'-delimited tokens are: timestamp, siglen, sig.
    //
    // We adopt the simplest correct approach: scan the raw string for the pattern
    // "proposerId|<vsStr>|<double>|<int>|<blob>" by:
    //   1. Find first '|' → proposerId
    //   2. Find last occurrence of "|\d+|" pattern before the blob → that's "|siglen|"
    //   3. Everything between step 1 and the previous '|' before siglen is vsStr+"|"+timestamp

    std::string s(payload.begin(), payload.end());
    ViewProposal proposal;

    // Step 1: extract proposerId
    size_t p1 = s.find('|');
    if (p1 == std::string::npos) return proposal;
    proposal.proposerReplicaId = std::stoi(s.substr(0, p1));

    // The rest: "<vsStr>|<timestamp>|<siglen>|<blob>"
    // We know siglen is a small non-negative integer. Scan backwards from payload end.
    // The binary blob starts right after the 4th '|' from right in the text part.
    // Walk backward: find siglen first (it's the last text field before the blob).
    // Strategy: scan for last '|<digits>|' near the end of the text area.
    // We find: last '|' that is followed only by digits until another '|' or end.
    size_t p_siglen = std::string::npos;
    size_t p_ts     = std::string::npos;

    // Walk backwards from the end to find |siglen|
    for (size_t i = s.size(); i > p1 + 1; ) {
        --i;
        if (s[i] != '|') continue;
        // Check if s[i+1..next_pipe] is all digits
        size_t j = i + 1;
        while (j < s.size() && s[j] != '|' && (s[j] >= '0' && s[j] <= '9')) ++j;
        if (j < s.size() && s[j] == '|' && j > i + 1) {
            // Candidate: s[i+1..j-1] is the siglen, s[j] is the pipe before the blob
            p_siglen = i;  // position of '|' before siglen
            // Now find the '|' before timestamp (one more '|' backwards)
            for (size_t k = p_siglen; k > p1 + 1; ) {
                --k;
                if (s[k] == '|') { p_ts = k; break; }
            }
            break;
        }
    }

    if (p_siglen == std::string::npos || p_ts == std::string::npos) return proposal;

    // vehicleStatesStr is between p1+1 and p_ts
    proposal.vehicleStatesStr = s.substr(p1 + 1, p_ts - p1 - 1);
    proposal.proposalTimestamp = std::stod(s.substr(p_ts + 1, p_siglen - p_ts - 1));
    int siglen = std::stoi(s.substr(p_siglen + 1));  // from siglen field start (digits only)

    // Actual siglen field ends at the pipe after it
    size_t blob_start_pipe = s.find('|', p_siglen + 1);
    size_t offset = (blob_start_pipe != std::string::npos) ? blob_start_pipe + 1 : s.size();

    if (offset < payload.size() && offset + (size_t)siglen <= payload.size()) {
        proposal.signature.assign(payload.begin() + offset, payload.begin() + offset + siglen);
    }

    // Also populate observedCars from vehicleStatesStr (carId is first pipe-field of each record)
    if (!proposal.vehicleStatesStr.empty()) {
        std::vector<std::string> recs = split(proposal.vehicleStatesStr, ';');
        for (const auto& rec : recs) {
            size_t pp = rec.find('|');
            if (pp != std::string::npos) {
                proposal.observedCars.insert(rec.substr(0, pp));
            } else if (!rec.empty()) {
                proposal.observedCars.insert(rec);
            }
        }
    }

    return proposal;
}

std::vector<uint8_t> V2VProxyModule::serializeViewAgreement(const ViewAgreement& agreement) {
    // Format: agreingReplicaId|carList|siglen|sig
    std::stringstream ss;
    ss << agreement.agreingReplicaId << "|";
    
    // Sort cars for deterministic ordering
    std::vector<std::string> sortedCars(agreement.agreedView.begin(), agreement.agreedView.end());
    std::sort(sortedCars.begin(), sortedCars.end());
    
    for (size_t i = 0; i < sortedCars.size(); i++) {
        if (i > 0) ss << ",";
        ss << sortedCars[i];
    }
    
    ss << "|" << agreement.signature.size() << "|";
    
    std::string header = ss.str();
    std::vector<uint8_t> result(header.begin(), header.end());
    result.insert(result.end(), agreement.signature.begin(), agreement.signature.end());
    
    return result;
}

V2VProxyModule::ViewAgreement V2VProxyModule::deserializeViewAgreement(BFTMessage* bftMsg) {
    std::vector<uint8_t> payload(bftMsg->getPayloadArraySize());
    for (size_t i = 0; i < payload.size(); i++) {
        payload[i] = bftMsg->getPayload(i);
    }
    
    std::string s(payload.begin(), payload.end());
    std::vector<std::string> parts = split(s, '|');
    
    ViewAgreement agreement;
    if (parts.size() >= 3) {
        agreement.agreingReplicaId = std::stoi(parts[0]);
        
        // Parse comma-separated car list
        if (!parts[1].empty()) {
            std::vector<std::string> cars = split(parts[1], ',');
            agreement.agreedView.insert(cars.begin(), cars.end());
        }
        
        int siglen = std::stoi(parts[2]);
        // Find offset of raw bytes by locating the 3rd '|' scanning forward
        // through the text header only.  find_last_of('|') is wrong because
        // the binary signature bytes can contain 0x7C ('|'), causing it to
        // land inside the payload and produce an empty or corrupt signature.
        size_t p1 = s.find('|');
        size_t p2 = (p1 != std::string::npos) ? s.find('|', p1 + 1) : std::string::npos;
        size_t p3 = (p2 != std::string::npos) ? s.find('|', p2 + 1) : std::string::npos;
        size_t offset = (p3 != std::string::npos) ? p3 + 1 : s.size();

        if (offset < payload.size() && offset + (size_t)siglen <= payload.size()) {
            agreement.signature.assign(payload.begin() + offset, payload.begin() + offset + siglen);
        }
    }

    return agreement;
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

// Build the canonical semicolon-pipe vehicleStates string from the local viewState map.
// Format: "veh0|N|1|S|0;veh1|S|1|L|0;veh2|W|2|R|1"  (sorted by vehicleId for determinism)
std::string V2VProxyModule::buildVehicleStatesStr() const {
    std::vector<std::pair<std::string, VehicleState>> sorted(viewState.begin(), viewState.end());
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
    }
    return result;
}

std::vector<uint8_t> V2VProxyModule::signViewProposal(const std::set<std::string>& viewSet) {
    // NEW: sign the vehicleStatesStr (not just carId list) to cover VehicleState data.
    // Input: vehicleStatesStr + ":" + signerReplicaId
    // Falls back to carId-list signing when viewState is empty (e.g. early VIEW_PROPOSAL from old code).
    std::string toSign;
    std::string vsStr = buildVehicleStatesStr();
    if (!vsStr.empty()) {
        toSign = vsStr + ":" + std::to_string(replicaId);
    } else {
        // Fallback: legacy carId-list format
        for (const std::string& carId : viewSet) {
            if (!toSign.empty()) toSign += ",";
            toSign += carId;
        }
        toSign += ":" + std::to_string(replicaId);
    }

    int32_t hash = computeXXHash32(toSign);
    std::vector<uint8_t> sig(sizeof(int32_t));
    std::memcpy(sig.data(), &hash, sizeof(int32_t));

    std::cout << "[VIEW_SIGN] Replica " << replicaId << " signed vehicleStates hash=" << hash << "\n";
    return sig;
}


void V2VProxyModule::initiateViewProposal() {
    // Called when all BATCH_SIZE ARRIVAL_ANNOUNCEs are collected (or IDLE → PROPOSING_VIEW at arrival).
    if (currentPhase != PROPOSING_VIEW) {
        std::cout << "[V2VProxy " << replicaId << "] Cannot initiate view proposal - phase="
                  << currentPhase << " (expected PROPOSING_VIEW)\n";
        return;
    }

    std::cout << "[V2VProxy " << replicaId << "] ===== PHASE 1b: BROADCASTING VIEW_PROPOSAL =====\n";

    // Build the vehicleStates string from the collected viewState map
    std::string vsStr = buildVehicleStatesStr();

    // Collect car IDs for observedCars field
    std::set<std::string> carIds;
    for (const auto& kv : viewState) carIds.insert(kv.first);

    std::cout << "[V2VProxy " << replicaId << "] vehicleStatesStr: " << vsStr << "\n";

    myViewProposal.proposerReplicaId = replicaId;
    myViewProposal.vehicleStatesStr  = vsStr;
    myViewProposal.observedCars      = carIds;
    myViewProposal.proposalTimestamp = simTime().dbl();
    myViewProposal.signature         = signViewProposal(carIds);  // signs vsStr + replicaId

    broadcastViewProposal();
    currentPhase = VIEW_AGREEMENT;
}

void V2VProxyModule::broadcastViewProposal() {
     // ZOMBIE FILTER: Departed cars don't broadcast views
    if (zombieFilter()) return;
    
    
    
    std::cout << "[V2VProxy " << replicaId << "] Broadcasting view proposal via V2V..." << "\n";
    
    std::vector<uint8_t> payload = serializeViewProposal(myViewProposal);
    sendBFTMessage(replicaId, -1, payload, 4);  // messageType=4 (VIEW_PROPOSAL)
    
    std::cout << "[V2VProxy " << replicaId << "] Broadcasted view with " 
              << myViewProposal.observedCars.size() << " cars" << "\n";
}

void V2VProxyModule::handleViewProposal(BFTMessage* bftMsg) {
    
    // ZOMBIE FILTER: Departed cars don't accept view proposals
    if (zombieFilter()) return;
    
    ViewProposal proposal = deserializeViewProposal(bftMsg);
    
    std::cout << "[V2VProxy " << replicaId << "] Received view proposal from replica " 
              << proposal.proposerReplicaId << "\n";
    std::cout << "[V2VProxy " << replicaId << "]   Their view: {";
    for (const auto& car : proposal.observedCars) {
        std::cout << car << " ";
    }
    std::cout << "}" << "\n";
    
    // Build my vehicleStates string and compare to the proposal
    std::string myVsStr = buildVehicleStatesStr();

    std::cout << "[V2VProxy " << replicaId << "]   My vehicleStatesStr: " << myVsStr << "\n";
    std::cout << "[V2VProxy " << replicaId << "]   Proposal vehicleStatesStr: " << proposal.vehicleStatesStr << "\n";

    if (!proposal.vehicleStatesStr.empty() && proposal.vehicleStatesStr == myVsStr) {
        std::cout << "[V2VProxy " << replicaId << "] AGREEMENT: vehicleStates match! Sending V2V signature...\n";

        // Must match Java verifyViewSignature: XXHash32(vehicleStatesStr + ":" + signingReplicaId)
        std::string toSign = proposal.vehicleStatesStr + ":" + std::to_string(replicaId);
        int32_t hash = computeXXHash32(toSign);
        std::vector<uint8_t> sig(sizeof(int32_t));
        std::memcpy(sig.data(), &hash, sizeof(int32_t));

        ViewAgreement agreement;
        agreement.agreingReplicaId = replicaId;
        agreement.agreedView       = proposal.observedCars;
        agreement.signature        = sig;

        std::vector<uint8_t> payload = serializeViewAgreement(agreement);
        sendBFTMessage(replicaId, proposal.proposerReplicaId, payload, 5);
    } else {
        std::cout << "[V2VProxy " << replicaId << "] DISAGREEMENT: vehicleStates don't match (have "
                  << viewState.size() << "/" << BATCH_SIZE << " VehicleStates). Not signing.\n";
    }
}

// Use the AGREED view (not live visibility) so all replicas elect the same leader.
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

void V2VProxyModule::handleViewAgreement(BFTMessage* bftMsg) {
    ViewAgreement agreement = deserializeViewAgreement(bftMsg);
    
    // 1. Get a reference to the specific vote list for this view
    auto& votes = viewVotes[agreement.agreedView];

    // 2. Check if this specific replica has already voted for this view
    bool alreadyVoted = false;
    for (const auto& existingVote : votes) {
        if (existingVote.agreingReplicaId == agreement.agreingReplicaId) {
            alreadyVoted = true;
            break; 
        }
    }

    // 3. Only proceed if this is a new, unique voter
    if (alreadyVoted) {
        std::cout << "[V2VProxy " << replicaId << "] IGNORING duplicate V2V agreement from replica " 
                  << agreement.agreingReplicaId << "\n";
        return;
    }

    // 4. Validate signature BEFORE counting — a vote with an empty/malformed
    //    signature will be skipped during serialization, so counting it here
    //    would make voteCount drift above the actual number of sigs Java receives.
    if (agreement.signature.size() < 4) {
        std::cout << "[V2VProxy " << replicaId << "] IGNORING vote from replica "
                  << agreement.agreingReplicaId << " — signature.size()="
                  << agreement.signature.size() << " (need >=4)" << "\n";
        return;
    }

    // 5. Record the unique, valid vote
    votes.push_back(agreement);

    int voteCount = votes.size();
    std::cout << "[V2VProxy " << replicaId << "] Received NEW valid V2V agreement from " 
              << agreement.agreingReplicaId << ". Valid votes: " << voteCount << "\n";
    
    // 5. Check if we have f+1 V2V agreements on this view
    // std::set<std::string> myView = getVisibleVehicles(300.0);
    int viewSize = agreement.agreedView.size();
    int f = (viewSize - 1) / 3;

    int required = f + 1;
    std::cout << "[V2VProxy " << replicaId << "] NEW agreement from " 
              << agreement.agreingReplicaId << ". Bucket for this exact view has: " 
              << voteCount << "/" << required << " votes. (View Size: " << viewSize << ")" << "\n";
    
    if (voteCount >= required && !viewEstablished) {
        viewSignatureCollectionEndTime = simTime();
        viewEstablished = true;

        std::cout << "[V2VProxy " << replicaId << "] ===== PHASE 1c: SUBMITTING TO BFT CONSENSUS =====" << "\n";
        std::cout << "[V2VProxy " << replicaId << "] Collected f+1=" << required 
                  << " V2V signatures for view: {";
        for (const auto& car : agreement.agreedView) {
            std::cout << car << " ";
        }
        std::cout << "}" << "\n";

       //submitViewToBFTConsensus(agreement.agreedView, votes);

        // Use the AGREED view (not live visibility) to elect the leader deterministically.
        // // All replicas with the same agreedView will compute the same leader.
        if (amITheLeader(agreement.agreedView)) {
            std::cout << "[V2VProxy " << replicaId << "] ===== PHASE 1c: LEADER SUBMITTING TO BFT =====" << "\n";
            submitViewToBFTConsensus(agreement.agreedView, votes);
        } else {
            std::cout << "[V2VProxy " << replicaId << "] I am a follower. Waiting for BFT delivery via Java callback." << "\n";
        }
    }
}

void V2VProxyModule::submitViewToBFTConsensus(const std::set<std::string>& view,
                                                const std::vector<ViewAgreement>& v2vSigs) {
    viewConsensusStartTime = simTime();
    realViewConsensusStart = std::chrono::high_resolution_clock::now();
    std::cout << "[V2VProxy " << replicaId << "] Submitting view to BFT-SMaRt consensus...\n";
    std::cout << "[METRICS " << replicaId << "] View_Consensus_Start: " << viewConsensusStartTime << "\n";

    // New wire format: "VIEW_PROPOSE:<proposerId>:<vehicleStatesStr>:<viewSignatures>"
    // vehicleStatesStr: "veh0|N|1|S|0;veh1|S|1|L|0;..."
    // viewSignatures:   "replicaId,XXHash32Decimal|replicaId,XXHash32Decimal|..."
    //   XXHash32 input: vehicleStatesStr + ":" + signingReplicaId (matches Java verifyViewSignature)

    std::string vsStr = buildVehicleStatesStr();

    std::stringstream ss;
    ss << "VIEW_PROPOSE:" << replicaId << ":" << vsStr << ":";

    // Append collected VIEW_AGREEMENT signatures
    bool firstSig = true;
    for (const ViewAgreement& sig : v2vSigs) {
        if (sig.signature.size() >= 4) {
            if (!firstSig) ss << "|";
            int32_t hashValue;
            std::memcpy(&hashValue, sig.signature.data(), sizeof(int32_t));
            ss << sig.agreingReplicaId << "," << hashValue;
            firstSig = false;
        } else {
            std::cerr << "[V2VProxy " << replicaId << "] WARNING: skipping sig from replica "
                      << sig.agreingReplicaId << " (size=" << sig.signature.size() << ")\n";
        }
    }

    std::string request = ss.str();
    std::cout << "[V2VProxy " << replicaId << "] BFT VIEW_PROPOSE: " << request << "\n";

    if (!triggerJoinViaJNI(request)) {
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: Failed to submit view to BFT — storing for retry when Java ready\n";
        pendingViewProposalRequest = request;  // will be retried by checkJavaReadyTimer
    }
}

void V2VProxyModule::onViewAgreed(const std::set<std::string>& agreedView) {
    std::lock_guard<std::mutex> lock(jniMutex);

    // Idempotency guard: phase2 must only be triggered once.
    // The leader can receive this callback TWICE:
    //   1. From appExecuteBatch (delivery thread, fires for ALL replicas), and
    //   2. From sendConsensusRequest's proxy reply (only the invokeOrdered caller).
    // Followers only receive it once from appExecuteBatch.
    if (viewEstablished) {
        std::cout << "[V2VProxy " << replicaId << "] onViewAgreed called again - view already established, ignoring." << "\n";
        return;
    }
    
    // Mark the end of View consensus
    viewConsensusEndTime = simTime();
    simtime_t viewConsensusDuration = viewConsensusEndTime - viewConsensusStartTime;
    realViewConsensusEnd = std::chrono::high_resolution_clock::now();
    auto realViewConsensusDuration = std::chrono::duration_cast<std::chrono::milliseconds>(realViewConsensusEnd - realViewConsensusStart);
    std::cout << "[METRICS " << replicaId << "] View_Consensus_Duration: " << realViewConsensusDuration.count() << "ms" << "\n";
    std::cout << "[METRICS " << replicaId << "] View_Consensus_End: " << viewConsensusEndTime << "\n";
    std::cout << "[METRICS " << replicaId << "] View_Consensus_Latency: " << viewConsensusDuration.dbl() << " seconds" << "\n";
    if (lastRoundResetTime >= 0 && lastRoundResetEpoch == currentEpoch) {
        const double resetToViewEndSec = (viewConsensusEndTime - lastRoundResetTime).dbl();
        std::cout << "[ROUND-DIAG] Replica " << replicaId
                  << " epoch=" << currentEpoch
                  << " resetToViewEnd=" << resetToViewEndSec
                  << " seconds (resetAt=" << lastRoundResetTime
                  << ", viewEnd=" << viewConsensusEndTime << ")" << "\n";
        resetToViewEndByEpochAndReplica[currentEpoch][replicaId] = resetToViewEndSec;
        const auto& epochResetToView = resetToViewEndByEpochAndReplica[currentEpoch];
        if (epochResetToView.size() >= 4 && printedResetToViewEndAvgEpochs.count(currentEpoch) == 0) {
            double sumDelay = 0.0;
            for (const auto& kv : epochResetToView) {
                sumDelay += kv.second;
            }
            printedResetToViewEndAvgEpochs.insert(currentEpoch);
            std::cout << "[ROUND-METRICS] Epoch " << currentEpoch
                      << " Avg_ResetToViewEnd_4Cars: " << (sumDelay / epochResetToView.size())
                      << " seconds (replicasCounted=" << epochResetToView.size() << ")" << "\n";
        }
    }
    
    // Within-epoch H1 fix: once VIEW is decided, any remaining VIEW WRITE/ACCEPT retransmissions
    // are stale and should not spill into the ORDER broadcast window.
    // // Clear this replica's reliability-layer unacked queue now (before ORDER starts).
    // if (jvm && javaReady) {
    //     JNIEnv* env;
    //     jvm->AttachCurrentThread((void**)&env, nullptr);
    //     jclass cls = env->FindClass("bftsmart/communication/V2V/ReliableV2VMessaging");
    //     if (cls) {
    //         jmethodID m = env->GetStaticMethodID(cls, "clearUnackedForReplica", "(I)V");
    //         if (m) env->CallStaticVoidMethod(cls, m, (jint)replicaId);
    //         if (env->ExceptionCheck()) env->ExceptionClear();
    //     }
    // }

    establishedView = agreedView;
    viewEstablished = true;
    // NEW PROTOCOL: No Phase 2. Java leader internally submits ORDER_PROPOSE after VIEW consensus.
    // C++ waits for notifyOrderDecided JNI callback to start batch execution.
    std::cout << "[V2VProxy " << replicaId << "] VIEW CONSENSUS COMPLETE at t=" << simTime()
              << " — waiting for Java ORDER_PROPOSE consensus\n";
}

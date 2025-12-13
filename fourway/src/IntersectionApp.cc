#include "veins/modules/application/traci/TraCIDemo11p.h"
#include "veins/modules/mobility/traci/TraCIMobility.h"
#include "veins/modules/mobility/traci/TraCICommandInterface.h"
#include "veins/base/utils/Coord.h"
#include "veins/modules/application/traci/TraCIDemo11pMessage_m.h"
#include <omnetpp/platdep/sockets.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <cmath>
#include <string>
#include <sstream>
#include <cstring>

using namespace veins;
using namespace omnetpp;

class IntersectionApp : public TraCIDemo11p {
  protected:
    int gatewaySocket = -1;
    std::string gatewayHost;
    int gatewayPort;
    std::string carId;
    std::string direction;
    double stopDistance;
    double intersectionX = 0.0;
    double intersectionY = 0.0;

    // BFT-Smart variables
    bool connectedToGateway = false;
    bool waitingForDecision = false;
    bool decisionReceived = false;
    bool hasRequested = false;
    bool isStopped = false;

    // Decision variables
    std::string qcId;
    std::string orderPosition;
    std::string fullOrder;

    // Messages
    cMessage* checkSocketMsg = nullptr;
    cMessage* resumeEvt = nullptr;
    cMessage* sendReqEvt = nullptr; // Add this
    cMessage* resumeMsg = nullptr;

    char recvBuffer[4096];
    std::string recvLineBuffer;

public:
virtual ~IntersectionApp();

protected:
    virtual void initialize(int stage) override;
    virtual void handleMessage(cMessage* msg) override;
    virtual void handlePositionUpdate(cObject* obj) override;
    virtual void onWSM(veins::BaseFrame1609_4* wsm) override;
    virtual void finish() override;

    void connectToGateway();
    void disconnectFromGateway();
    void sendRequestCross();
    void checkSocketData();
    void parseDecision(const std::string& line);
    void processDecision();

    double getDistanceToIntersection();
    bool isApproachingIntersection();
    void stopVehicle();
    void resumeVehicle();
};

Define_Module(IntersectionApp);

IntersectionApp::~IntersectionApp() {
    cancelAndDelete(checkSocketMsg);
    cancelAndDelete(resumeEvt);
    cancelAndDelete(resumeMsg);
    disconnectFromGateway();
}

void IntersectionApp::initialize(int stage) {
    // 1. Initialize Base Class (This sets up 'mobility' pointer)
    TraCIDemo11p::initialize(stage);

    if (stage == 0) {
        gatewayHost = par("gatewayHost").stringValue();
        gatewayPort = par("gatewayPort");
        carId = par("carId").stringValue();
        direction = par("direction").stringValue();
        stopDistance = par("stopDistance").doubleValue();
        intersectionX = par("intersectionX").doubleValue();
        intersectionY = par("intersectionY").doubleValue();

        checkSocketMsg = new cMessage("checkSocketMsg");
        resumeEvt = new cMessage("resumeAfterDecision");
        resumeMsg = new cMessage("resumeVehicle");
        sendReqEvt = new cMessage("sendRequestEvent");
        EV_INFO << "IntersectionApp initialized: carId=" << carId 
        << ", gateway=" << gatewayHost << ":" << gatewayPort << endl;
    }
    // Don't schedule anything in stage 1 - wait for first position update
}

void IntersectionApp::handleMessage(cMessage* msg) {
    if (msg == checkSocketMsg) {
        // First time: connect to gateway
        if (!connectedToGateway) {
            connectToGateway();
            if (connectedToGateway) {
                scheduleAt(simTime() + 0.1, checkSocketMsg);
            } else {
                scheduleAt(simTime() + 1.0, checkSocketMsg);
            }
        } else {
            // Already connected, just check for data
            checkSocketData();
            scheduleAt(simTime() + 0.1, checkSocketMsg);
        }
    }
    else if (msg == resumeMsg) {  // CHANGED: handle resumeMsg for scheduled resume
        EV_INFO << "Car " << carId << " resuming after scheduled delay\n";
        resumeVehicle();
    }
    else if (msg == resumeEvt) {  // KEEP: for immediate processing
        processDecision();
    }
    else if (msg == sendReqEvt) {
        sendRequestCross();
    }
    else {
        TraCIDemo11p::handleMessage(msg);
    }
}
void IntersectionApp::handlePositionUpdate(cObject* obj) {
    TraCIDemo11p::handlePositionUpdate(obj);

    // Safety checks
    if (!checkSocketMsg || !mobility) return;

    // First position update - connect to gateway
    // Note: All cars connect to gateways to act as proxies/forwarders
    if (!connectedToGateway && !checkSocketMsg->isScheduled()) {
        EV_INFO << "First position update - connecting to gateway\n";
        connectToGateway();
        if (connectedToGateway) {
            scheduleAt(simTime() + 0.1, checkSocketMsg);
        } else {
            // Retry connection later
            scheduleAt(simTime() + 1.0, checkSocketMsg);
        }
    }

    // Safely check distance - ANY car can broadcast a request when approaching
    // No need to be connected to gateway for broadcasting
    if (isApproachingIntersection() && !hasRequested && !waitingForDecision) {
        EV_INFO << "Car " << carId << " approaching intersection, broadcasting REQUEST via V2V\n";
        stopVehicle();
        double randomDelay = uniform(0.01, 0.5);
        scheduleAt(simTime() + randomDelay, sendReqEvt);
        hasRequested = true;
        waitingForDecision = true;
    }
}

double IntersectionApp::getDistanceToIntersection() {
    // FIX 1: Use inherited 'mobility' pointer directly
    // Do NOT use getSubmodule("veinsmobility") -> It returns NULL because the module is named "mobility"
    if (!mobility) {
        return 1e10; 
    }
    
    Coord position = mobility->getPositionAt(simTime());
    double dx = intersectionX - position.x;
    double dy = intersectionY - position.y;
    return std::sqrt(dx*dx + dy*dy);
}

bool IntersectionApp::isApproachingIntersection() {
    double distance = getDistanceToIntersection();
    return distance < stopDistance && distance > 0;
}

void IntersectionApp::stopVehicle() {
    // FIX 2: Use mobility->getVehicleCommandInterface() instead of traciVehicle
    if (!isStopped && mobility && mobility->getVehicleCommandInterface()) {
        mobility->getVehicleCommandInterface()->setSpeed(0);
        isStopped = true;
        EV_INFO << "Car " << carId << " stopped at intersection\n";
    }
}

void IntersectionApp::resumeVehicle() {
    // FIX 3: Use mobility->getVehicleCommandInterface()
    if (isStopped && mobility && mobility->getVehicleCommandInterface()) {
        mobility->getVehicleCommandInterface()->setSpeed(-1); // Release control to SUMO
        isStopped = false;
        EV_INFO << "Car " << carId << " resumed movement\n";
    }
}

// ... (Rest of your socket functions remain the same) ...

void IntersectionApp::connectToGateway(){
    if (connectedToGateway) return;

    gatewaySocket = socket(AF_INET, SOCK_STREAM, 0);
    if (gatewaySocket < 0){
        EV_ERROR << "Failed to create socket\n"; 
        return;
    }

    int flags = fcntl(gatewaySocket, F_GETFL, 0);
    fcntl(gatewaySocket, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(gatewayPort);

    if (inet_pton(AF_INET, gatewayHost.c_str(), &serverAddr.sin_addr) <= 0){
        EV_ERROR << "Invalid gateway host: " << gatewayHost << "\n";
        close(gatewaySocket);
        gatewaySocket = -1;
        return;
    }

    int result = connect(gatewaySocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr));
    if (result < 0 && errno != EINPROGRESS){
        EV_ERROR << "Failed to connect to gateway: " << strerror(errno) << "\n";
        close(gatewaySocket);
        gatewaySocket = -1;
        return;
    }

    connectedToGateway = true;
    EV_INFO << "Connected to gateway: " << gatewayHost << ":" << gatewayPort << "\n";
}

void IntersectionApp::disconnectFromGateway(){
    if (gatewaySocket >= 0){
        close(gatewaySocket);
        gatewaySocket = -1;
    }
    connectedToGateway = false;
}

void IntersectionApp::sendRequestCross(){
    // 1. Prepare the payload string
    // Format: "REQ <carId> <direction> <timestamp>"
    std::ostringstream request;
    request << "REQ " << carId << " " << direction << " " << simTime().dbl();
    std::string requestStr = request.str();

    // 2. Create the Radio Packet (V2V Message)
    veins::TraCIDemo11pMessage* wsm = new veins::TraCIDemo11pMessage("RequestCross");
    populateWSM(wsm);
    wsm->setDemoData(requestStr.c_str());
    
    
    // 3. Broadcast it to the air!
    sendDown(wsm);
    
    EV_INFO << "Broadcasting request via V2V: " << requestStr << endl;
}

void IntersectionApp::checkSocketData(){
    if (!connectedToGateway || gatewaySocket < 0 ) return;
    
    ssize_t n = ::recv(gatewaySocket, recvBuffer, sizeof(recvBuffer) - 1, MSG_DONTWAIT);

    if (n > 0 ){
        recvBuffer[n] = '\0';
        recvLineBuffer += std::string(recvBuffer, n);

        size_t pos;
        while ((pos = recvLineBuffer.find('\n')) != std::string::npos){
            std::string line = recvLineBuffer.substr(0, pos);
            recvLineBuffer.erase(0, pos + 1);
            if (!line.empty()){
                // Check if this is a DECISION message
                if (line.find("DECISION ") == 0) {
                    // 1. Process it locally (So I know if I can go)
                    parseDecision(line);
                    
                    // 2. RE-BROADCAST it to the network (V2V)
                    // This ensures the car that requested (Car 1) hears the answer
                    // even if it's not connected to this specific Gateway.
                    
                    // Strip "DECISION " prefix and replace with "DEC " to save bytes/differentiate
                    std::string v2vMsg = "DEC " + line.substr(9);
                    
                    veins::TraCIDemo11pMessage* wsm = new veins::TraCIDemo11pMessage("DecisionBroadcast");
                    populateWSM(wsm);
                    wsm->setDemoData(v2vMsg.c_str());
                    sendDown(wsm);
                    
                    EV_INFO << "Re-broadcasting BFT Decision via V2V: " << v2vMsg << endl;
                } else {
                    parseDecision(line);
                }
            }
        }
    }
    else if (n == 0){
        EV_WARN << "Connection closed by gateway\n";
        disconnectFromGateway();
    }
    else if (errno != EAGAIN && errno != EWOULDBLOCK){
        EV_ERROR << "Error reading from gateway: " << strerror(errno) << "\n";
    }
}

void IntersectionApp::parseDecision(const std::string& line){
    if (line.find("DECISION ") == 0){
        std::istringstream iss(line);
        std::string command;
        iss >> command >> qcId >> orderPosition >> fullOrder;

        // Only mark as received if this decision is for ME
        if (!fullOrder.empty() && fullOrder.find(carId) != std::string::npos) {
            decisionReceived = true;
            waitingForDecision = false;
            EV_INFO << "Received MY decision: " << fullOrder << "\n";
        } else {
            EV_INFO << "Received decision for another car: " << fullOrder << " (ignoring)\n";
        }
        
        processDecision();
    }
}

void IntersectionApp::processDecision(){
    EV_INFO << "Processing decision: " << fullOrder << "\n";  // CHANGED: decision -> fullOrder
    
    // Parse format: "CAR_ID:GO:DELAY" or "CAR_ID:WAIT"
    size_t firstColon = fullOrder.find(':');  // CHANGED: decision -> fullOrder
    size_t secondColon = fullOrder.find(':', firstColon + 1);  // CHANGED
    
    if (firstColon == std::string::npos) return;
    
    std::string carInDecision = fullOrder.substr(0, firstColon);  // CHANGED
    std::string command = fullOrder.substr(firstColon + 1,  // CHANGED
                                         (secondColon != std::string::npos) 
                                         ? secondColon - firstColon - 1 
                                         : std::string::npos);
    
    // Extract delay if present (format: CAR_ID:GO:DELAY)
    double delaySeconds = 0.0;
    if (secondColon != std::string::npos) {
        std::string delayStr = fullOrder.substr(secondColon + 1);  // CHANGED
        delaySeconds = std::stod(delayStr);
    }
    
    if (carInDecision == carId && command == "GO") {
        double safeDelay = (delaySeconds < 0.1) ? 0.1 : delaySeconds;

        EV_INFO << "Car " << carId << " will resume in " << safeDelay << "s\n";
        
        // Ensure we don't schedule duplicates
        if (resumeMsg->isScheduled()) {
            cancelEvent(resumeMsg);
        }
        scheduleAt(simTime() + safeDelay, resumeMsg);
    } else if (carInDecision == carId && command == "WAIT") {
        EV_INFO << "Car " << carId << " must continue waiting\n";
    }
}

void IntersectionApp::onWSM(veins::BaseFrame1609_4* frame) {
    // Cast to TraCIDemo11pMessage
    veins::TraCIDemo11pMessage* wsm = dynamic_cast<veins::TraCIDemo11pMessage*>(frame);
    if (!wsm) {
        // Not a TraCIDemo11pMessage, let parent handle it
        TraCIDemo11p::onWSM(frame);
        return;
    }

    // 1. Get the data
    std::string msg = wsm->getDemoData();
    EV_INFO << "Received V2V Message: " << msg << endl;

    // 2. Check Type
    if (msg.find("REQ ") == 0) {
        // --- PROXY LOGIC ---
        // I heard a neighbor request crossing.
        // I must forward this to my local BFT Gateway so it can process it.
        
        // Extract the carId from the message: "REQ <carId> ..."
        std::istringstream iss(msg);
        std::string reqPrefix, requestingCarId;
        iss >> reqPrefix >> requestingCarId;
        
        // Don't forward my own requests (avoid double submission)
        if (requestingCarId == carId) {
            EV_INFO << "Ignoring my own V2V request (not forwarding to gateway)\n";
            return;
        }
        
        if (connectedToGateway && gatewaySocket >= 0) {
            // Transform to the format Gateway expects: "REQUEST_CROSS ..."
            // Note: My code sent "REQ", Gateway expects "REQUEST_CROSS"
            // Let's just fix the string before sending.
            std::string forwardedMsg = "REQUEST_CROSS" + msg.substr(3) + "\n"; // replace REQ with REQUEST_CROSS
            
            ::send(gatewaySocket, forwardedMsg.c_str(), forwardedMsg.length(), 0);
            EV_INFO << "Forwarded neighbor request (" << requestingCarId << ") to Gateway via TCP: " << forwardedMsg;
        } else {
            EV_WARN << "Cannot forward request from " << requestingCarId << " - not connected to gateway yet\n";
        }
    }
    else if (msg.find("DEC ") == 0) {
        // --- DECISION LOGIC ---
        // Format: "DEC <qcId> <orderPosition> <full_order_string>"
        // Example: "DEC 123 0 CAR_0:GO:0"
        
        // IMPORTANT: Only process if we're still waiting for a decision
        // With V2V multi-proxy, we might receive multiple conflicting decisions
        // We accept the FIRST decision and ignore subsequent ones
        if (!waitingForDecision || decisionReceived) {
            EV_INFO << "Ignoring duplicate V2V decision (already processed)\n";
            return;
        }
        
        std::string payload = msg.substr(4); // Remove "DEC "
        
        // Parse the decision (format: <qcId> <orderPosition> <carId>:GO:<delay>)
        std::istringstream iss(payload);
        std::string receivedQcId, receivedOrderPosition, receivedFullOrder;
        iss >> receivedQcId >> receivedOrderPosition >> receivedFullOrder;
        
        // Check if this decision is for me
        if (!receivedFullOrder.empty() && receivedFullOrder.find(carId) != std::string::npos) {
            qcId = receivedQcId;
            orderPosition = receivedOrderPosition;
            fullOrder = receivedFullOrder;
            
            decisionReceived = true;
            waitingForDecision = false;
            
            EV_INFO << "Received V2V Decision (FIRST): " << fullOrder << endl;
            processDecision();
        } else {
            EV_INFO << "Received V2V Decision for another car: " << receivedFullOrder << endl;
        }
    }
}

void IntersectionApp::finish(){
    TraCIDemo11p::finish();
    disconnectFromGateway();
}
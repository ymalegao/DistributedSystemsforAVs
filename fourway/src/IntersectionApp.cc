#include "veins/modules/application/traci/TraCIDemo11p.h"
#include "veins/modules/mobility/traci/TraCIMobility.h"
#include "veins/modules/mobility/traci/TraCICommandInterface.h"
#include "veins/base/utils/Coord.h"
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

    char recvBuffer[4096];
    std::string recvLineBuffer;

  public:
    virtual ~IntersectionApp();

  protected:
    virtual void initialize(int stage) override;
    virtual void handleMessage(cMessage* msg) override;
    virtual void handlePositionUpdate(cObject* obj) override;
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
                scheduleAt(simTime() + 0.1, checkSocketMsg); // Then check regularly
            } else {
                // Retry connection later if it failed
                scheduleAt(simTime() + 1.0, checkSocketMsg);
            }
        } else {
            // Already connected, just check for data
            checkSocketData();
            scheduleAt(simTime() + 0.1, checkSocketMsg);
        }
    }
    else if (msg == resumeEvt) {
        processDecision();
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
    
    if (!connectedToGateway) return;

    // Safely check distance
    if (isApproachingIntersection() && !hasRequested && !waitingForDecision) {
        EV_INFO << "Car " << carId << " approaching intersection, sending REQUEST_CROSS\n";
        stopVehicle();
        sendRequestCross();
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
    if (!connectedToGateway || gatewaySocket < 0 ){
        EV_ERROR << "Cannot send request: not connected to gateway\n";
        return;
    }

    double arrivalTime = simTime().dbl();
    std::ostringstream request;
    request << "REQUEST_CROSS " << carId << " " << direction << " " << arrivalTime << "\n";
    std::string requestStr = request.str();
    
    ssize_t sent = ::send(gatewaySocket, requestStr.c_str(), requestStr.length(), 0);
    if (sent < 0){
        EV_ERROR << "Failed to send request: " << strerror(errno) << "\n";
        return;
    }
    EV_INFO << "Request sent successfully\n";
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
                parseDecision(line);
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
        
        decisionReceived = true;
        waitingForDecision = false;
        processDecision();
    }
}

void IntersectionApp::processDecision(){
    if (!decisionReceived) return;

    bool canGo = false; 
    std::string searchPattern = carId + ":GO";
    if (fullOrder.find(searchPattern) != std::string::npos){
        canGo = true;
    }

    if (canGo){
        EV_INFO << "Car " << carId << " can go\n";
        resumeVehicle();
    }
    else{
        EV_INFO << "Car " << carId << " must wait\n";
        stopVehicle();
        scheduleAt(simTime() + 1.0, resumeEvt); // Check again in 1s, or wait for next message
    }
    decisionReceived = false;
}

void IntersectionApp::finish(){
    TraCIDemo11p::finish();
    disconnectFromGateway();
}
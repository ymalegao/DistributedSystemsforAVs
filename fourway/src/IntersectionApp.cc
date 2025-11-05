#include "veins/modules/application/traci/TraCIDemo11p.h"
#include "veins/modules/mobility/traci/TraCIMobility.h"
#include "veins/modules/mobility/traci/TraCICommandInterface.h"

using namespace veins;

class IntersectionApp : public TraCIDemo11p {
  protected:
    bool didInitialStop = false;
    cMessage* resumeEvt = nullptr;

    virtual void initialize(int stage) override {
        TraCIDemo11p::initialize(stage);
        if (stage == 0) {
            resumeEvt = new cMessage("resumeAfterSpawn");
        }
    }

    virtual void handleMessage(cMessage* msg) override {
        if (msg == resumeEvt) {
            EV_INFO << "resuming after 3s initial wait\n";
            // -1 = let SUMO control speed again
            traciVehicle->setSpeed(-1);
        } else {
            TraCIDemo11p::handleMessage(msg);
        }
    }

    virtual void handlePositionUpdate(cObject* obj) override {
        TraCIDemo11p::handlePositionUpdate(obj);

        // first time we ever get a position -> do the 3s stop
        if (!didInitialStop) {
            EV_INFO << "first position, stopping for 3s\n";
            traciVehicle->setSpeed(0.0);
            didInitialStop = true;
            scheduleAt(simTime() + 3, resumeEvt);
        }
    }

    virtual ~IntersectionApp() {
        cancelAndDelete(resumeEvt);
    }
};

Define_Module(IntersectionApp);

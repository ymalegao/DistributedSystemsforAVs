#include "TraCIDemo11p.h"
#include "veins/modules/mobility/traci/TraCIMobility.h"
#include "veins/modules/mobility/traci/TraCICommandInterface.h"

using namespace veins;

class IntersectionApp : public TraCIDemo11p {
  protected:
    bool hasStoppedOnce = false;

    virtual void handlePositionUpdate(cObject* obj) override {
        // always call parent so CAMs etc still work
        TraCIDemo11p::handlePositionUpdate(obj);

        // current position from SUMO
        Coord pos = mobility->getPositionAt(simTime());

        // your intersection center (rough from SUMO net)
        // you can read it from osm.net.xml: x="103.22" y="84.03"
        // OMNeT++ uses meters, same as SUMO after import
        Coord intersection(103.22, 84.03);

        // distance to intersection
        double dist = pos.distance(intersection);

        // say: if I'm within 10 m of the center, stop
        if (!hasStoppedOnce && dist < 10.0) {
            // get the TraCI vehicle handle
            traciVehicle->setSpeed(0.0);      // hard stop
            // or: traciVehicle->setStop("10553209#0_0", 3.0, 0, 1);  // lane, pos, duration
            hasStoppedOnce = true;
        }
    }
};

Define_Module(IntersectionApp);

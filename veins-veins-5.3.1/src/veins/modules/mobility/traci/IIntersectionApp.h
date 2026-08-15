#pragma once

#include <omnetpp.h>

// ── Vehicle-lifecycle hooks the scenario manager needs from an application ────
//
// TraCIScenarioManager observes things in SUMO that an intersection application
// must react to (a vehicle departing the junction, a crash being injected into
// it). It previously reached those out by including ResDBIntersectionApp.h and
// dynamic_cast-ing to the concrete class, which made core mobility code depend
// on the application layer while the application layer already depended on
// core -- a dependency cycle that pinned the app inside the Veins tree.
//
// The consumer owns the interface: core mobility declares what it needs, and
// the application implements it. Adding a hook means adding a method here and
// implementing it in the app; core never learns the concrete type.
//
// Implemented by: ResDBIntersectionApp.
//
// Found via getSubmodulesOfType<IIntersectionApp>() / dynamic_cast from
// cModule*, which is a valid cross-cast because implementers derive from both
// cSimpleModule and this interface.

namespace veins {

class IIntersectionApp {
public:
    virtual ~IIntersectionApp() = default;

    // The vehicle has left the intersection, at the given simulation time.
    virtual void recordIntersectionDeparture(omnetpp::simtime_t when) = 0;

    // The vehicle has been crashed by injection; it must stop transmitting.
    // reason is a short tag for logging, e.g. "crash-inject".
    virtual void disableCrashComms(const char* reason) = 0;
};

} // namespace veins

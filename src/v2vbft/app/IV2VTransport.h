#pragma once

#include <cstdint>

// ── Abstract transport interface (Step 2) ────────────────────────────────────
//
// Concrete subclasses:
//   Step 2 → LoggingTransport  (prints to stderr — proves callback path)
//   Step 3 → VeinsTransport    (calls DemoBaseApplLayer::sendDown())
//
// The two static methods (c_send_to / c_broadcast) are C-compatible adaptors
// that can be placed directly into a ResdbOmnetTransportCallbacks struct.
// Thread-safety: callbacks may arrive from ResDB's worker threads in Step 3+;
// concrete implementations must be thread-safe from that point on.

namespace veins {

class IV2VTransport {
public:
    virtual ~IV2VTransport() = default;

    // Unicast a serialized ResDB packet to a specific replica (0-based id).
    virtual void sendTo(int toReplica, const uint8_t* data, uint32_t len) = 0;

    // Broadcast a serialized ResDB packet to every known replica.
    virtual void broadcast(const uint8_t* data, uint32_t len) = 0;

    // ── C-bridge adapters ────────────────────────────────────────────────────
    // Pass these as the function-pointer fields in ResdbOmnetTransportCallbacks,
    // with 'this' as the ctx pointer.

    static void c_send_to(void* ctx, int to_replica,
                          const uint8_t* data, uint32_t len)
    {
        static_cast<IV2VTransport*>(ctx)->sendTo(to_replica, data, len);
    }

    static void c_broadcast(void* ctx, const uint8_t* data, uint32_t len)
    {
        static_cast<IV2VTransport*>(ctx)->broadcast(data, len);
    }
};

} // namespace veins

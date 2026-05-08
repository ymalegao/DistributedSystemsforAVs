#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Step 1: lifecycle ─────────────────────────────────────────────────────── */

int   ResdbOmnetBridgeVersion();
void* ResdbOmnetCreateKvServer(char* config_file, char* private_key_file,
                               char* cert_file, char* logging_dir);
int   ResdbOmnetRunServer(void* server_handle);
int   ResdbOmnetStopServer(void* server_handle);
void  ResdbOmnetDestroyServer(void* server_handle);

/* ── Step 2: abstract transport interface ──────────────────────────────────── */

/* C-compatible callback table.  OMNeT++ fills this and passes it through
 * ResdbOmnetSetTransport().  ResDB calls these when it wants to send a packet.
 * ctx is the opaque pointer supplied by the caller (typically "this").
 * Thread-safety: callbacks may be invoked from ResDB's internal threads. */
typedef struct ResdbOmnetTransportCallbacks {
    /* Unicast to a specific replica (0-based replica id). */
    void (*send_to)(void* ctx, int to_replica, const uint8_t* data, uint32_t len);
    /* Broadcast to all replicas. */
    void (*broadcast)(void* ctx, const uint8_t* data, uint32_t len);
    /* Opaque context forwarded verbatim to every callback. */
    void* ctx;
} ResdbOmnetTransportCallbacks;

/* Creates a lightweight "null" server handle that has no real ResDB server
 * behind it.  Safe to call with empty/missing config files.  Useful for
 * Step-2 transport smoke-testing before real keys are generated.
 * The returned handle supports SetTransport and TestBroadcast but NOT
 * RunServer (returns -1). Destroy with ResdbOmnetDestroyServer() as usual. */
void* ResdbOmnetCreateNullHandle();

/* Register the transport with the server handle.
 * Must be called after ResdbOmnetCreateKvServer (or CreateNullHandle) and
 * before ResdbOmnetRunServer.
 * Returns 0 on success, -1 if handle or cbs is null. */
int ResdbOmnetSetTransport(void* server_handle, ResdbOmnetTransportCallbacks* cbs);

/* Step-2 smoke-test probe: fires the registered broadcast callback with
 * synthetic data so the logging transport can be exercised without needing
 * real ResDB consensus to run.  Returns 0 if callback was invoked, -1 otherwise. */
int ResdbOmnetTestBroadcast(void* server_handle,
                            const uint8_t* data, uint32_t len);

/* ── Step 3: inbound delivery (socketless simulation) ─────────────────────── */

/* Provide a pointer to the OMNeT++ channel/transport object if needed by the
 * bridge implementation. Currently optional; stored verbatim for future use. */
int ResdbOmnetSetChannel(void* server_handle, void* channel_ptr);

/* Inject a received V2V packet (raw ResDB network bytes) into the ResDB server.
 * from_replica is provided for convenience by the simulation layer; ResDB's
 * own message formats generally embed sender_id in their payloads.
 * Returns 0 on success, -1 on invalid input/handle. */
int ResdbOmnetDeliverPacket(void* server_handle, int from_replica,
                            const uint8_t* data, uint32_t len);

/* ── Step 4: time virtualization ──────────────────────────────────────────── */

/* Update the ResDB sim-time provider (microseconds). This must be called by
 * the simulation thread (OMNeT++) whenever simTime advances, so that ResDB
 * worker threads never consult wall clock. */
int ResdbOmnetUpdateSimTimeUs(void* server_handle, int64_t now_us);

/* ── Step 5: wire format (shared between Veins and ResDB bridge) ───────────── */

/* One vehicle's arrival state.  13 bytes, no padding.
 * Appears in both the ProposeAll payload and the OrderDecision reply. */
#pragma pack(push, 1)
typedef struct ResdbVehicleEntry {
    int32_t  replica_id;     /* 4 bytes */
    uint64_t sim_time_us;    /* 8 bytes — simTime() at stop-zone entry */
    uint8_t  is_ambulance;   /* 1 byte  — non-zero means emergency priority */
} ResdbVehicleEntry;         /* 13 bytes total */

/* Header of the payload passed to ResdbOmnetTriggerConsensus:
 *   [0..3]   uint32_t epoch
 *   [4..7]   int32_t  leader_id
 *   [8..15]  uint64_t propose_sim_time_us
 *   [16..19] uint32_t n_vehicles
 *   [20..]   n_vehicles × ResdbVehicleEntry
 */
typedef struct ResdbProposeHdr {
    uint32_t epoch;
    int32_t  leader_id;
    uint64_t propose_sim_time_us;
    uint32_t n_vehicles;
} ResdbProposeHdr;            /* 20 bytes */

/* Header of the bytes delivered via ResdbOrderDecidedFn:
 *   [0..3]  uint32_t epoch
 *   [4..7]  uint32_t n_vehicles
 *   [8..]   n_vehicles × int32_t  (crossing order, first = crosses first)
 */
typedef struct ResdbOrderHdr {
    uint32_t epoch;
    uint32_t n_vehicles;
} ResdbOrderHdr;              /* 8 bytes */
#pragma pack(pop)

/* ── Step 5: smart contract & application logic ────────────────────────────── */

/* Submit a serialized ProposeAllPayload as a TYPE_CLIENT_REQUEST.
 * Only the primary replica should call this; followers receive the PRE_PREPARE
 * from the primary over radio and do not need to call this themselves.
 * Returns 0 on success, -1 on invalid handle/data. */
int ResdbOmnetTriggerConsensus(void* server_handle,
                               const uint8_t* payload, uint32_t len);

/* Callback invoked from a ResDB worker thread after PBFT commits and the
 * IntersectionExecutor builds an OrderDecision.
 * decision_bytes is a serialized OrderDecision proto.
 * Must be async-safe: enqueue and return quickly. */
typedef void (*ResdbOrderDecidedFn)(void* ctx,
                                    const uint8_t* decision_bytes, uint32_t len);

/* Register the order-decided callback.  Must be called before RunServer.
 * Returns 0 on success, -1 if handle is null. */
int ResdbOmnetSetOrderCallback(void* server_handle,
                               ResdbOrderDecidedFn cb, void* ctx);

/* Return the 0-based primary replica ID from PBFT SystemInfo.
 * Returns 0 if the handle is null or the consensus is not yet started. */
int ResdbOmnetGetPrimary(void* server_handle);

/* Remove a departed replica from the active set (epoch reset).
 * Returns 0 on success, -1 if handle is null. */
int ResdbOmnetRemoveReplica(void* server_handle, int replica_id);

#ifdef __cplusplus
}  // extern "C"
#endif

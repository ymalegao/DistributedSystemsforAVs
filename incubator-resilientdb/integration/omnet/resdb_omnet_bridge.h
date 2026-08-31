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
/* Stops the process-wide Stats monitor thread (glog "monitor" spam). Safe to
 * call multiple times (e.g. from each replica's finish()). Call after
 * ResdbOmnetStopServer when tearing down so opp_run can exit cleanly. */
void  ResdbOmnetStopGlobalStats(void);

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

/* Return the local collector's distinct, verified votes matching the packet's
 * (view, seq, request hash). vote_type uses Request::Type (4=PREPARE, 5=COMMIT).
 * quorum_out is the exact forced-view quorum when available, otherwise -1. */
int ResdbOmnetGetVerifiedVoteProgress(void* server_handle,
                                      const uint8_t* packet_data,
                                      uint32_t packet_len,
                                      int vote_type,
                                      int* count_out,
                                      int* quorum_out);

/* ── Step 4: time virtualization ──────────────────────────────────────────── */

/* Update the ResDB sim-time provider (microseconds). This must be called by
 * the simulation thread (OMNeT++) whenever simTime advances, so that ResDB
 * worker threads never consult wall clock. */
int ResdbOmnetUpdateSimTimeUs(void* server_handle, int64_t now_us);

/* ── Step 5: wire format (shared between Veins and ResDB bridge) ───────────── */

/* One vehicle's certified scheduling state. 22 bytes, no padding.
 * Appears in the ProposeAll payload (Veins → ResDB bridge).
 * lane:     0=N, 1=S, 2=E, 3=W
 * direction: 0=Straight, 1=Left, 2=Right, 3=Unknown (signed singleton)
 * position_in_lane: 1=front, 2=second, …
 * cyber_status: 0=QUIET (no f+1 echoes), 1=SIGNED */
#pragma pack(push, 1)
typedef struct ResdbVehicleEntry {
    int32_t  replica_id;        /* 4 bytes */
    uint64_t sim_time_us;       /* 8 bytes — simTime() at stop-zone entry; UINT64_MAX = QUIET */
    uint8_t  is_ambulance;      /* 1 byte  — non-zero means emergency priority */
    uint8_t  lane;              /* 1 byte  — 0=N,1=S,2=E,3=W */
    uint8_t  direction;         /* 1 byte  — 0=Straight,1=Left,2=Right,3=Unknown */
    uint8_t  position_in_lane;  /* 1 byte  — 1=front,2=second,… */
    uint8_t  cyber_status;      /* 1 byte  — 0=QUIET,1=SIGNED */
    uint8_t  physical_lane_index; /* 1 byte — 0=outer/T,1=inner/L */
    int32_t  lateral_claim_cm;    /* 4 bytes — signed lane-normal claim */
} ResdbVehicleEntry;            /* 22 bytes total */

/* Header of the payload passed to ResdbOmnetTriggerConsensus:
 *   [0..3]   uint32_t epoch
 *   [4..7]   int32_t  leader_id
 *   [8..15]  uint64_t propose_sim_time_us
 *   [16..19] uint32_t n_vehicles
 *   [20..]   n_vehicles × ResdbVehicleEntry (22 bytes each)
 */
typedef struct ResdbProposeHdr {
    uint32_t epoch;
    int32_t  leader_id;
    uint64_t propose_sim_time_us;
    uint32_t n_vehicles;
} ResdbProposeHdr;            /* 20 bytes */

/* Optional evidence trailer appended after ResdbVehicleEntry[] in a
 * crash-recovery ORDER proposal (spec §12) — NOT a new consensus mode, just
 * extra bytes on the ordinary ResdbProposeHdr proposal:
 *   ResdbProposeHdr
 *   ResdbVehicleEntry[ResdbProposeHdr.n_vehicles]
 *   ResdbOrderEvidenceHdr                                    (optional)
 *   repeated { uint32_t cert_len; uint8_t cert[cert_len]; }  (n_clear_certs times)
 * Absent for epoch 0 and for Scenario-15 emergency-rollback ORDER, which
 * never require CLEAR evidence. */
#define RESDB_ORDER_EVIDENCE_MAGIC 0x4F455631u
typedef struct ResdbOrderEvidenceHdr {
    uint32_t magic;              /* RESDB_ORDER_EVIDENCE_MAGIC */
    uint16_t version;
    uint16_t n_clear_certs;
} ResdbOrderEvidenceHdr;      /* 8 bytes */

/* Rollback proposal wrapper.  The bytes after this header are:
 *   justification[justification_len]
 *   ResdbProposeHdr
 *   ResdbVehicleEntry[ResdbProposeHdr.n_vehicles]
 *
 * reason: 0=CRASH, 1=EMERGENCY.
 * Rollback proposals carry a dynamic membership M; the bridge installs a
 * forced PBFT view over M with f lowered to what M can safely support. */
typedef struct ResdbRollbackHdr {
    uint32_t new_epoch;
    uint32_t cancelled_epoch;
    uint8_t  reason;
    uint8_t  _pad[3];
    uint32_t justification_len;
} ResdbRollbackHdr;           /* 16 bytes */

/* Phase-2 cancel proposal (no vehicle entries).  Bytes after this header:
 *   justification[justification_len]   // serialized CANCEL_CERT
 *   int32_t  leader_id                 // deterministic cancel proposer in E
 *   uint32_t n_electors
 *   int32_t  electors[n_electors]      // epoch-e electorate E
 */
typedef struct ResdbCancelHdr {
    uint32_t cancelled_epoch;
    uint8_t  reason;
    uint8_t  _pad[3];
    uint32_t justification_len;
} ResdbCancelHdr;             /* 12 bytes */

/* Executor output for a committed CANCEL(e) decision (not an order schedule).
 * magic distinguishes this from ResdbOrderHdr in the app callback. */
#define RESDB_CANCEL_DECISION_MAGIC 0xCACEC0DEu
typedef struct ResdbCancelDecisionHdr {
    uint32_t magic;              /* RESDB_CANCEL_DECISION_MAGIC */
    uint32_t cancelled_epoch;
    uint8_t  reason;
    uint8_t  _pad[3];
    uint64_t cancel_seq;
    uint8_t  payload_digest[32];
} ResdbCancelDecisionHdr;     /* 48 bytes */

/* Phase-3 rollback justification: reference to committed CANCEL(e). */
typedef struct ResdbCancelCommitRef {
    uint32_t cancelled_epoch;
    uint64_t cancel_seq;
    uint8_t  payload_digest[32];
    uint32_t proof_len;
} ResdbCancelCommitRef;       /* 44 bytes */

/* Per-vehicle batch assignment in the OrderDecision reply.
 *   replica_id  — which vehicle
 *   batch_index — 0-based; vehicles with same index cross simultaneously
 */
typedef struct ResdbVehicleDecision {
    int32_t  replica_id;
    uint32_t batch_index;
} ResdbVehicleDecision;       /* 8 bytes */

/* Header of the bytes delivered via ResdbOrderDecidedFn:
 *   [0..3]   uint32_t epoch
 *   [4..7]   uint32_t n_vehicles
 *   [8..11]  uint32_t n_batches
 *   [12..]   n_vehicles × ResdbVehicleDecision (8 bytes each)
 */
typedef struct ResdbOrderHdr {
    uint32_t epoch;
    uint32_t n_vehicles;
    uint32_t n_batches;
} ResdbOrderHdr;              /* 12 bytes */
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

/* Scenario 16: validate a CLEAR evidence certificate carried in a
 * crash-recovery ORDER's evidence trailer. Called from a ResDB worker thread
 * during PBFT PreVerify — must be thread-safe and read-only (spec §5.1's
 * "one thread-safe, read-only evidence-validation callback"; the bridge must
 * not implement a second, weaker validator of its own, so cert_bytes is
 * opaque here — only the Veins-side callback parses it).
 * cancelled_epoch is the epoch that was cancelled (proposal epoch - 1); the
 * callback is responsible for checking cert_bytes actually matches it.
 * Returns nonzero if cert_bytes is a valid CLEAR certificate for
 * cancelled_epoch under the replica's own committed view, zero otherwise. */
typedef int (*ResdbClearEvidenceFn)(void* ctx,
                                    uint32_t cancelled_epoch,
                                    const uint8_t* cert_bytes,
                                    uint32_t cert_len);

/* Register the CLEAR evidence-validation callback.  Must be called before
 * RunServer.  Returns 0 on success, -1 if handle is null. */
int ResdbOmnetSetClearEvidenceCallback(void* server_handle,
                                       ResdbClearEvidenceFn fn, void* ctx);

/* Notification for a recovery ORDER rejected before PBFT installs/prepares
 * the request.  It runs on a ResDB worker thread; the application must only
 * enqueue/atomically record the event and handle it on the simulation thread.
 * reason=1 denotes missing/invalid CLEAR evidence. */
typedef void (*ResdbRecoveryRejectFn)(void* ctx, uint32_t epoch,
                                      int32_t leader_id, int32_t reason);
int ResdbOmnetSetRecoveryRejectCallback(void* server_handle,
                                        ResdbRecoveryRejectFn fn, void* ctx);

/* Return the 0-based primary replica ID from PBFT SystemInfo.
 * Returns 0 if the handle is null or the consensus is not yet started. */
int ResdbOmnetGetPrimary(void* server_handle);

/* Set PBFT's primary from the application-level cert-primary election.
 * primary_omnet is a 0-based replica ID. Returns 0 on success, -1 on error. */
int ResdbOmnetSetPrimaryFromCert(void* server_handle, int primary_omnet);

/* After gossip-adopting CANCEL(e), advance the PBFT executor past the
 * ORDER(e)/CANCEL(e) prefix so ORDER(e+1) can execute on late spawns. */
int ResdbOmnetAdvanceExecutorAfterGossipCancel(void* server_handle,
                                               uint32_t cancelled_epoch);

/* Diagnostic metadata decoded from a serialized ResDBMessage/Request packet. */
typedef struct ResdbPacketRequestInfo {
    int      parse_ok;
    int32_t  type;
    uint64_t current_view;
    uint64_t seq;
    int32_t  sender_id;
    int32_t  primary_id;
    int64_t  proxy_id;
    uint64_t current_executed_seq;
    uint32_t data_len;
    uint32_t hash_len;
    uint8_t  request_hash_digest[32];
} ResdbPacketRequestInfo;

/* Decode a serialized ResDBMessage/Request packet carried by Type 8.
 * Returns the inner Request::Type numeric value, or -1 if parsing fails. */
int ResdbOmnetGetPacketRequestType(const uint8_t* data, uint32_t len);

/* Decode detailed Request fields from a Type 8 packet for logging. */
int ResdbOmnetGetPacketRequestInfo(const uint8_t* data, uint32_t len,
                                   ResdbPacketRequestInfo* info);

/* Human-readable name for ResdbOmnetGetPacketRequestType() values. */
const char* ResdbOmnetRequestTypeName(int request_type);

/* Remove a departed replica from the active set (epoch reset).
 * Returns 0 on success, -1 if handle is null. */
int ResdbOmnetRemoveReplica(void* server_handle, int replica_id);

/* ── Step 4 (M4): view-change support ─────────────────────────────────────── */

/* Configure the PBFT complaint-timer duration (microseconds of sim-time).
 * Must be called after ResdbOmnetCreateKvServer and before RunServer.
 * Default is 3,000,000 µs (3 s). Returns 0 on success, -1 on bad args. */
int ResdbOmnetSetVcTimeoutUs(void* server_handle, int64_t timeout_us);

/* Force a view-change from the OMNeT++ simulation thread.
 * Injects a minimal-deadline complaint into the ViewChangeManager's monitoring
 * queue so the next sim-time tick fires VC immediately.
 * Call when a follower's application-level timer expires and the primary has
 * been silent.  Safe to call multiple times (idempotent per view).
 * Returns 0 on success, -1 if handle/consensus is null. */
int ResdbOmnetForceViewChange(void* server_handle);

/* Make this node's PBFT communicator drop all outbound messages (PRE_PREPARE,
 * PREPARE, COMMIT, VIEW_CHANGE, NEW_VIEW).  Used by BYZANTINE_SILENT_PRIMARY
 * so that followers' complaint timers actually fire and ResDB's built-in
 * view-change runs.  Call after ResdbOmnetRunServer.
 * Returns 0 on success, -1 if handle/consensus is null. */
int ResdbOmnetSetPbftSilent(void* server_handle, int silent);

/* Mark this local replica as inactive for future epochs. The static
 * server.config remains unchanged; this only stops local OMNeT/PBFT
 * participation so a departed vehicle cannot act as a ghost replica. */
int ResdbOmnetMarkReplicaInactive(void* server_handle,
                                  int replica_id,
                                  uint32_t min_epoch);

/* Configure the intended tolerated f for fixed-physical-N experiments.
 * All static replicas remain consensus members; only quorum is overridden to
 * 2f+1 for normal proposals. */
int ResdbOmnetSetToleratedFaults(void* server_handle, int tolerated_f);

/* Rollback Phase-3 fault mode: 1 = per_epoch (default), 0 = anchored. */
int ResdbOmnetSetRollbackFaultMode(void* server_handle, int per_epoch_mode);

/* ── Step 5 (M4): cert-omission guard (Java OrderRequestVerifier Check 7) ──── */

/* Per-cert entry returned by ResdbCertSnapshotFn.
 * Carries the replica ID and the cert-attested vehicle state fields so the
 * pre-verify can check both QUIET suppression (Check 9) and state-field
 * tampering (Check 10) in a single callback invocation. */
#pragma pack(push, 1)
typedef struct ResdbCertEntry {
    int32_t replica_id;
    uint8_t lane;             /* 0=N,1=S,2=E,3=W — same encoding as ResdbVehicleEntry */
    uint8_t position_in_lane; /* 1=front, 2=second, … */
    uint8_t direction;        /* 0=Straight,1=Left,2=Right,3=Unknown */
    uint8_t is_ambulance;     /* 0 or 1 */
    uint8_t physical_lane_index; /* 0=outer/T,1=inner/L */
    int32_t lateral_claim_cm;    /* signed lane-normal declaration */
} ResdbCertEntry;             /* 13 bytes */
#pragma pack(pop)

/* Callback filled in by the OMNeT++ app to give the bridge a snapshot of the
 * local C++ collectedCerts with full cert-attested state fields.
 * Called from a ResDB worker thread during PRE_PREPARE verification — must be
 * thread-safe.
 * entries_out: caller-supplied buffer of ResdbCertEntry.
 * count_inout: capacity on entry, actual count written on exit. */
typedef void (*ResdbCertSnapshotFn)(void* ctx,
                                    ResdbCertEntry* entries_out,
                                    uint32_t*       count_inout);

/* Register the cert-snapshot provider.  Call after CreateKvServer, before RunServer.
 * Returns 0 on success, -1 if handle is null. */
int ResdbOmnetSetCertSnapshotFn(void* server_handle,
                                ResdbCertSnapshotFn fn, void* ctx);

#ifdef __cplusplus
}  // extern "C"
#endif

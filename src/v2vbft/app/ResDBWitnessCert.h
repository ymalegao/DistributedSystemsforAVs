#pragma once
// ResDBWitnessCert.h — shared f+1 witness-certificate machinery.
//
// Generalizes the "collect f+1 distinct signed echoes for a statement, verify
// against a trusted key, assemble+retry a certificate" pattern that the CANCEL
// pipeline (type-12/13) already implements inline. CANCEL (including the
// Scenario-16 BLOCKED reuse of CANCEL_CRASH) is refactored onto this; a future
// CLEAR certificate can reuse the same classes behind a thin wire adapter.
//
// No OMNeT++ dependency here and no wire format — CANCEL/CLEAR own their own
// byte-exact serializers and translate to/from WitnessStatement/WitnessEcho at
// the boundary.

#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "v2vbft/crypto/CryptoAuth.h"

// ---- Trusted identity binding (replicaId -> pubkey), process-global singleton ----
// Mirrors CryptoAuth::instance()'s lifetime model. Each module registers its own
// key once at init; a witness signature only counts once the embedded key matches
// the registry entry for the claimed signer id.
class WitnessKeyRegistry {
public:
    static WitnessKeyRegistry& instance();

    // First write for a given replicaId wins. Re-registering the identical key is
    // a no-op success. Registering a different key for an already-bound id fails
    // and logs [WITNESS-KEY-CONFLICT].
    bool registerKey(int replicaId, const uint8_t pubKey[CRYPTO_PUBKEY_BYTES]);
    bool matches(int replicaId, const uint8_t pubKey[CRYPTO_PUBKEY_BYTES]) const;
    bool known(int replicaId) const;

    // Clears all bindings. Must be called once at the start of each simulation
    // run (the registry is a process-global singleton, but keys are regenerated
    // fresh per run) — see plan Risk R1.
    void resetForNewRun();

    WitnessKeyRegistry(const WitnessKeyRegistry&) = delete;
    WitnessKeyRegistry& operator=(const WitnessKeyRegistry&) = delete;

private:
    WitnessKeyRegistry() = default;

    mutable std::mutex mtx_;
    std::map<int, std::array<uint8_t, CRYPTO_PUBKEY_BYTES>> keys_;
};

// ---- Generic witness statement / echo model ----

enum class WitnessKind {
    CANCEL_CRASH,
    CANCEL_EMERGENCY,
    CLEAR, // reserved for a later increment; unused today
};

struct WitnessStatement {
    uint32_t epoch = 0;
    WitnessKind kind = WitnessKind::CANCEL_CRASH;
    std::string subjectRef;

    // Statement identity used to bucket echoes together: "epoch:kindInt:subjectRef".
    std::string collectorKey() const;
    // Bytes a signer actually signs for this statement: collectorKey() + ":" + signerId.
    std::string signPayload(int signerId) const;
};

struct WitnessEcho {
    int signerId = -1;
    uint8_t pubKey[CRYPTO_PUBKEY_BYTES] = {};
    uint8_t sig[CRYPTO_SIG_MAX_BYTES] = {};
    uint8_t sigLen = 0;
};

// ---- Pure validation: threshold, distinct signers, membership, key, signature ----
class WitnessCertificateValidator {
public:
    // f: fault tolerance for this statement's committed view (threshold = f+1).
    // committedView: signer ids eligible to count toward the threshold; the
    // incident/statement *subject* is never checked against this set, only
    // each echo's signerId.
    WitnessCertificateValidator(int f, const std::set<int>* committedView);

    bool validate(const WitnessStatement& stmt,
                  const std::vector<WitnessEcho>& echoes,
                  int* outValidDistinct = nullptr) const;

private:
    int f_;
    const std::set<int>* committedView_;
};

// ---- Dedup-by-signer echo collection, keyed by statement collectorKey() ----
class WitnessEchoCollector {
public:
    // Returns true iff this signer was newly added for this statement (progress).
    bool add(const WitnessStatement& stmt, const WitnessEcho& echo);
    const std::vector<WitnessEcho>* get(const std::string& collectorKey) const;
    bool reachedThreshold(const std::string& collectorKey, int threshold) const;

private:
    std::map<std::string, std::vector<WitnessEcho>> buckets_;
    std::map<std::string, std::set<int>> signers_;
};

// ---- Retry data/policy only; the owning module keeps the cMessage/scheduleAt ----
class WitnessRetryManager {
public:
    struct Entry {
        std::vector<uint8_t> certBytes;
        int attempts = 0;
        bool active = false;
    };

    void arm(const std::string& key, std::vector<uint8_t> certBytes);
    void resetBackoff(const std::string& key);
    void stop(const std::string& key);

    // Fixed interval this increment (exponential backoff deferred). Returns a
    // negative value when the caller should stop retrying (not armed/active).
    double nextDelaySec(const std::string& key, double fixedIntervalSec);
    const Entry* entry(const std::string& key) const;

private:
    std::map<std::string, Entry> entries_;
};

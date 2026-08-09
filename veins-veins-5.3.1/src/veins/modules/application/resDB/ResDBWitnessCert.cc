#include "veins/modules/application/resDB/ResDBWitnessCert.h"

#include <cstring>
#include <iostream>

// ---- WitnessKeyRegistry ----

WitnessKeyRegistry& WitnessKeyRegistry::instance()
{
    static WitnessKeyRegistry inst;
    return inst;
}

bool WitnessKeyRegistry::registerKey(int replicaId, const uint8_t pubKey[CRYPTO_PUBKEY_BYTES])
{
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = keys_.find(replicaId);
    if (it == keys_.end()) {
        std::array<uint8_t, CRYPTO_PUBKEY_BYTES> arr;
        std::memcpy(arr.data(), pubKey, CRYPTO_PUBKEY_BYTES);
        keys_.emplace(replicaId, arr);
        return true;
    }
    if (std::memcmp(it->second.data(), pubKey, CRYPTO_PUBKEY_BYTES) == 0) return true;
    std::cout << "[WITNESS-KEY-CONFLICT] replicaId=" << replicaId << "\n";
    return false;
}

bool WitnessKeyRegistry::matches(int replicaId, const uint8_t pubKey[CRYPTO_PUBKEY_BYTES]) const
{
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = keys_.find(replicaId);
    if (it == keys_.end()) return false;
    return std::memcmp(it->second.data(), pubKey, CRYPTO_PUBKEY_BYTES) == 0;
}

bool WitnessKeyRegistry::copyKey(int replicaId, uint8_t out[CRYPTO_PUBKEY_BYTES]) const
{
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = keys_.find(replicaId);
    if (it == keys_.end() || !out) return false;
    std::memcpy(out, it->second.data(), CRYPTO_PUBKEY_BYTES);
    return true;
}

bool WitnessKeyRegistry::known(int replicaId) const
{
    std::lock_guard<std::mutex> lk(mtx_);
    return keys_.count(replicaId) != 0;
}

void WitnessKeyRegistry::resetForNewRun()
{
    std::lock_guard<std::mutex> lk(mtx_);
    keys_.clear();
}

// ---- WitnessStatement ----

std::string WitnessStatement::collectorKey() const
{
    return std::to_string(epoch) + ":" + std::to_string(static_cast<int>(kind)) + ":" + subjectRef;
}

std::string WitnessStatement::signPayload(int signerId) const
{
    return collectorKey() + ":" + std::to_string(signerId);
}

// ---- WitnessCertificateValidator ----

WitnessCertificateValidator::WitnessCertificateValidator(int f, const std::set<int>* committedView)
    : f_(f), committedView_(committedView)
{
}

bool WitnessCertificateValidator::validate(const WitnessStatement& stmt,
                                            const std::vector<WitnessEcho>& echoes,
                                            int* outValidDistinct) const
{
    const int required = f_ + 1;
    if (outValidDistinct) *outValidDistinct = 0;
    if ((int)echoes.size() < required) return false;

    std::set<int> seen;
    int valid = 0;
    for (const auto& echo : echoes) {
        if (echo.signerId < 0 || echo.sigLen == 0) continue;
        if (committedView_ && !committedView_->count(echo.signerId)) continue;
        if (!seen.insert(echo.signerId).second) continue;
        if (!WitnessKeyRegistry::instance().matches(echo.signerId, echo.pubKey)) continue;
        const std::string toSign = stmt.signPayload(echo.signerId);
        if (CryptoAuth::instance().verifyBytes(
                echo.pubKey,
                reinterpret_cast<const uint8_t*>(toSign.c_str()), toSign.size(),
                echo.sig, echo.sigLen)) {
            valid++;
        }
    }
    if (outValidDistinct) *outValidDistinct = valid;
    return valid >= required;
}

// ---- WitnessEchoCollector ----

bool WitnessEchoCollector::add(const WitnessStatement& stmt, const WitnessEcho& echo)
{
    const std::string key = stmt.collectorKey();
    if (!signers_[key].insert(echo.signerId).second) return false;
    buckets_[key].push_back(echo);
    return true;
}

const std::vector<WitnessEcho>* WitnessEchoCollector::get(const std::string& collectorKey) const
{
    auto it = buckets_.find(collectorKey);
    return it == buckets_.end() ? nullptr : &it->second;
}

bool WitnessEchoCollector::reachedThreshold(const std::string& collectorKey, int threshold) const
{
    auto it = buckets_.find(collectorKey);
    return it != buckets_.end() && (int)it->second.size() >= threshold;
}

// ---- WitnessRetryManager ----

void WitnessRetryManager::arm(const std::string& key, std::vector<uint8_t> certBytes)
{
    Entry& e = entries_[key];
    e.certBytes = std::move(certBytes);
    e.attempts = 0;
    e.active = true;
}

void WitnessRetryManager::resetBackoff(const std::string& key)
{
    auto it = entries_.find(key);
    if (it != entries_.end()) it->second.attempts = 0;
}

void WitnessRetryManager::stop(const std::string& key)
{
    auto it = entries_.find(key);
    if (it != entries_.end()) it->second.active = false;
}

double WitnessRetryManager::nextDelaySec(const std::string& key, double fixedIntervalSec)
{
    auto it = entries_.find(key);
    if (it == entries_.end() || !it->second.active) return -1.0;
    it->second.attempts++;
    return fixedIntervalSec;
}

const WitnessRetryManager::Entry* WitnessRetryManager::entry(const std::string& key) const
{
    auto it = entries_.find(key);
    return it == entries_.end() ? nullptr : &it->second;
}

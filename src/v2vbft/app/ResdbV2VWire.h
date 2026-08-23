#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "v2vbft/crypto/CryptoAuth.h"

namespace veins {
namespace resdbwire {

// Payload layout:
//   [33B sender_pub_key_compressed]
//   [1B  sig_len]
//   [sig_len bytes DER ECDSA signature over raw_resdb_bytes]
//   [raw_resdb_bytes ...]
struct SignedPacketView {
    uint8_t pubKey[CRYPTO_PUBKEY_BYTES] = {};
    uint8_t sigLen = 0;
    const uint8_t* sig = nullptr;
    const uint8_t* resdbBytes = nullptr;
    uint32_t resdbLen = 0;
};

inline bool unpackSignedPacket(const uint8_t* data, uint32_t len, SignedPacketView* out)
{
    if (out == nullptr || data == nullptr) {
        return false;
    }
    if (len < static_cast<uint32_t>(CRYPTO_PUBKEY_BYTES + 1)) {
        return false;
    }

    std::memcpy(out->pubKey, data, CRYPTO_PUBKEY_BYTES);
    out->sigLen = data[CRYPTO_PUBKEY_BYTES];
    if (out->sigLen == 0 || out->sigLen > CRYPTO_SIG_MAX_BYTES) {
        return false;
    }

    uint32_t headerLen = static_cast<uint32_t>(CRYPTO_PUBKEY_BYTES + 1 + out->sigLen);
    if (len < headerLen) {
        return false;
    }

    out->sig = data + CRYPTO_PUBKEY_BYTES + 1;
    out->resdbBytes = data + headerLen;
    out->resdbLen = len - headerLen;
    return true;
}

inline std::vector<uint8_t> packSignedPacket(EVP_PKEY* privKey,
                                             const uint8_t pubKey[CRYPTO_PUBKEY_BYTES],
                                             const uint8_t* resdbBytes, uint32_t resdbLen)
{
    std::vector<uint8_t> out;
    if (privKey == nullptr || pubKey == nullptr || resdbBytes == nullptr || resdbLen == 0) {
        return out;
    }

    uint8_t sig[CRYPTO_SIG_MAX_BYTES] = {};
    uint8_t sigLen = 0;
    if (!CryptoAuth::instance().signBytes(privKey, resdbBytes, resdbLen, sig, sigLen)) {
        return out;
    }

    out.reserve(CRYPTO_PUBKEY_BYTES + 1 + sigLen + resdbLen);
    out.insert(out.end(), pubKey, pubKey + CRYPTO_PUBKEY_BYTES);
    out.push_back(sigLen);
    out.insert(out.end(), sig, sig + sigLen);
    out.insert(out.end(), resdbBytes, resdbBytes + resdbLen);
    return out;
}

}  // namespace resdbwire
}  // namespace veins


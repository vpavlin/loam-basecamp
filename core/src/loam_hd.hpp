// loam_hd.hpp — deterministic, unlinkable identities from one root (loam-keycard ADR 0001).
//
// C++ twin of loam-keycard/src/hd.ts, byte-for-byte: BIP39 seed (PBKDF2-HMAC-SHA512, 2048 rounds,
// salt "mnemonic"+passphrase) → BIP32 master ("Bitcoin seed") → HARDENED children only:
//
//   main identity          m/43'/60'/1581'/0'/0'
//   (app, context)         m/43'/60'/1581'/A'/C0'/C1'/C2'/C3'
//     A  = idx31(sha256("loam-app:" + appId))[0]
//     C* = idx31(sha256("loam-ctx:" + appId + ":" + contextId))[0..3]
//   idx31 = first 16 bytes as four big-endian uint32, each & 0x7FFFFFFF.
//
// Address = "0x" + hex(sha256(compressed pub))[24:64] (the Loam address used by every app).
// Self-contained (OpenSSL + std) so it can be unit-tested without Qt. Shared test vectors:
// core/test/hd_vectors.txt (generated from loam-keycard test/vectors/hd.json).
#pragma once
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/obj_mac.h>
#include <openssl/sha.h>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace loamhd {

using Bytes = std::vector<unsigned char>;

inline std::string hex(const unsigned char* b, size_t n) {
    static const char* h = "0123456789abcdef"; std::string s; s.reserve(n * 2);
    for (size_t i = 0; i < n; i++) { s += h[b[i] >> 4]; s += h[b[i] & 15]; } return s;
}
inline std::string hex(const Bytes& b) { return hex(b.data(), b.size()); }

inline std::array<uint32_t, 4> idx31(const std::string& seed) {
    unsigned char h[32]; SHA256((const unsigned char*)seed.data(), seed.size(), h);
    std::array<uint32_t, 4> r{};
    for (int k = 0; k < 4; k++) {
        const unsigned char* p = h + 4 * k;
        r[k] = ((uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3])) & 0x7FFFFFFFu;
    }
    return r;
}

/** Hardened indices below m/43'/60'/<1581'|1582'>/ — empty appId means the main identity. */
inline std::vector<uint32_t> subPath(const std::string& appId, const std::string& contextId) {
    if (appId.empty()) return {0, 0};
    std::vector<uint32_t> out{idx31("loam-app:" + appId)[0]};
    for (uint32_t i : idx31("loam-ctx:" + appId + ":" + contextId)) out.push_back(i);
    return out;
}
inline std::string pathString(uint32_t purposeLeaf, const std::vector<uint32_t>& sub) {
    std::string s = "m/43'/60'/" + std::to_string(purposeLeaf) + "'";
    for (uint32_t i : sub) s += "/" + std::to_string(i) + "'";
    return s;
}
inline std::string keyPathFor(const std::string& appId, const std::string& contextId) { return pathString(1581, subPath(appId, contextId)); }
inline std::string signPathFor(const std::string& appId, const std::string& contextId) { return pathString(1582, subPath(appId, contextId)); }

/** BIP39 seed from a (normalised, ASCII English) phrase and optional passphrase. */
inline Bytes seedFromMnemonic(const std::string& mnemonic, const std::string& passphrase = "") {
    Bytes seed(64);
    const std::string salt = "mnemonic" + passphrase;
    PKCS5_PBKDF2_HMAC(mnemonic.data(), (int)mnemonic.size(), (const unsigned char*)salt.data(), (int)salt.size(),
                      2048, EVP_sha512(), 64, seed.data());
    return seed;
}

struct ExtKey { Bytes key, chain; bool ok = false; };

inline ExtKey masterFromSeed(const Bytes& seed) {
    unsigned char I[64]; unsigned int n = 0;
    static const char* k = "Bitcoin seed";
    HMAC(EVP_sha512(), k, 12, seed.data(), seed.size(), I, &n);
    ExtKey m; m.key.assign(I, I + 32); m.chain.assign(I + 32, I + 64); m.ok = (n == 64);
    return m;
}

/** Hardened child: I = HMAC-SHA512(chain, 0x00 ‖ k ‖ ser32(i | 2^31)); k' = (IL + k) mod n. */
inline ExtKey hardenedChild(const ExtKey& parent, uint32_t index) {
    ExtKey c; if (!parent.ok) return c;
    const uint32_t i = index | 0x80000000u;
    unsigned char data[37]; data[0] = 0;
    for (int b = 0; b < 32; b++) data[1 + b] = parent.key[b];
    data[33] = (unsigned char)(i >> 24); data[34] = (unsigned char)(i >> 16); data[35] = (unsigned char)(i >> 8); data[36] = (unsigned char)i;
    unsigned char I[64]; unsigned int n = 0;
    HMAC(EVP_sha512(), parent.chain.data(), (int)parent.chain.size(), data, sizeof data, I, &n);
    if (n != 64) return c;
    EC_GROUP* grp = EC_GROUP_new_by_curve_name(NID_secp256k1);
    BN_CTX* ctx = BN_CTX_new();
    BIGNUM *order = BN_new(), *il = BN_bin2bn(I, 32, nullptr), *kp = BN_bin2bn(parent.key.data(), 32, nullptr), *sum = BN_new();
    EC_GROUP_get_order(grp, order, ctx);
    // BIP32: an IL ≥ n or a zero child key is invalid (probability ~2^-127); we report failure.
    if (BN_cmp(il, order) < 0 && BN_mod_add(sum, il, kp, order, ctx) == 1 && !BN_is_zero(sum)) {
        c.key.assign(32, 0);
        BN_bn2binpad(sum, c.key.data(), 32);
        c.chain.assign(I + 32, I + 64);
        c.ok = true;
    }
    BN_free(order); BN_free(il); BN_free(kp); BN_free(sum); BN_CTX_free(ctx); EC_GROUP_free(grp);
    return c;
}

struct Derived { std::string path, pubHex, address; Bytes priv; bool ok = false; };

/** Compressed public key + Loam address for a private key. */
inline Derived describe(const Bytes& priv) {
    Derived d; if (priv.size() != 32) return d;
    EC_GROUP* grp = EC_GROUP_new_by_curve_name(NID_secp256k1);
    BN_CTX* ctx = BN_CTX_new(); BIGNUM* k = BN_bin2bn(priv.data(), 32, nullptr);
    EC_POINT* P = EC_POINT_new(grp);
    if (EC_POINT_mul(grp, P, k, nullptr, nullptr, ctx) == 1) {
        Bytes pub(33);
        if (EC_POINT_point2oct(grp, P, POINT_CONVERSION_COMPRESSED, pub.data(), 33, ctx) == 33) {
            unsigned char h[32]; SHA256(pub.data(), 33, h);
            d.priv = priv; d.pubHex = hex(pub); d.address = "0x" + hex(h, 32).substr(24, 40); d.ok = true;
        }
    }
    EC_POINT_free(P); BN_free(k); BN_CTX_free(ctx); EC_GROUP_free(grp);
    return d;
}

/** Derive the identity for (appId, contextId) — or the main identity when appId is empty. */
inline Derived derive(const Bytes& seed, const std::string& appId, const std::string& contextId) {
    ExtKey k = masterFromSeed(seed);
    for (uint32_t i : {43u, 60u, 1581u}) k = hardenedChild(k, i);
    for (uint32_t i : subPath(appId, contextId)) k = hardenedChild(k, i);
    if (!k.ok) return Derived{};
    Derived d = describe(k.key);
    d.path = keyPathFor(appId, contextId);
    return d;
}

} // namespace loamhd

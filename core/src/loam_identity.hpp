// loam_identity.hpp — the loam identity SERVICE (loam ADR 0004). Key custody (device / named-soft;
// Keycard delegated to Alisher's keycard module — see loam_core_impl), per-container binding, a default, and
// signDigest(). Apps consume this over module IPC and never hold a private key; the identity UI stays
// in each app's view. secp256k1 via OpenSSL — the SAME address derivation as the mobile registry
// (address = "0x" + sha256(compressed-pubkey)[24:64]) so ONE key is one identity across phone+desktop.
#pragma once
#include <openssl/ec.h>
#include <openssl/obj_mac.h>
#include <openssl/ecdsa.h>
#include <openssl/sha.h>
#include <openssl/bn.h>
#include <openssl/rand.h>
#include <nlohmann/json.hpp>
#include <QString>
#include <QDir>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <cstdlib>

namespace loamid {

using Bytes = std::vector<unsigned char>;
using json = nlohmann::json;

inline Bytes sha256b(const Bytes& in) { Bytes o(32); SHA256(in.data(), in.size(), o.data()); return o; }
inline Bytes strBytes(const std::string& s) { return Bytes(s.begin(), s.end()); }
inline std::string toHexS(const unsigned char* b, size_t n) {
    static const char* h = "0123456789abcdef"; std::string s; s.reserve(n * 2);
    for (size_t i = 0; i < n; i++) { s += h[b[i] >> 4]; s += h[b[i] & 15]; } return s;
}
inline Bytes fromHexB(const std::string& s) {
    Bytes o; o.reserve(s.size() / 2);
    auto v = [](char c) -> int { if (c >= '0' && c <= '9') return c - '0'; if (c >= 'a' && c <= 'f') return c - 'a' + 10; if (c >= 'A' && c <= 'F') return c - 'A' + 10; return 0; };
    for (size_t i = 0; i + 1 < s.size(); i += 2) o.push_back((unsigned char)((v(s[i]) << 4) | v(s[i + 1]))); return o;
}

struct SignId { Bytes priv, pub; std::string pubHex, address; bool ok = false; };

// priv(32B) → compressed pub(33B) + address. Mirrors scala_identity.hpp identityFromPriv byte-for-byte.
inline SignId identityFromPriv(const Bytes& priv) {
    SignId id; if (priv.size() != 32) return id;
    EC_KEY* key = EC_KEY_new_by_curve_name(NID_secp256k1);
    BIGNUM* bn = BN_bin2bn(priv.data(), 32, nullptr);
    if (key && bn && EC_KEY_set_private_key(key, bn) == 1) {
        const EC_GROUP* grp = EC_KEY_get0_group(key);
        BN_CTX* ctx = BN_CTX_new();
        EC_POINT* pub = EC_POINT_new(grp);
        if (EC_POINT_mul(grp, pub, bn, nullptr, nullptr, ctx) == 1) {
            Bytes pubc(33);
            size_t n = EC_POINT_point2oct(grp, pub, POINT_CONVERSION_COMPRESSED, pubc.data(), 33, ctx);
            if (n == 33) {
                id.priv = priv; id.pub = pubc; id.pubHex = toHexS(pubc.data(), 33);
                Bytes h = sha256b(pubc);
                id.address = "0x" + toHexS(h.data(), 32).substr(24, 40);
                id.ok = true;
            }
        }
        EC_POINT_free(pub); BN_CTX_free(ctx);
    }
    if (bn) BN_free(bn); if (key) EC_KEY_free(key);
    return id;
}
inline SignId generateIdentity() {
    Bytes priv(32);
    for (int i = 0; i < 16; i++) { if (RAND_bytes(priv.data(), 32) == 1) { SignId id = identityFromPriv(priv); if (id.ok) return id; } }
    return SignId{};
}
// Sign a RAW 32B digest → 64B compact r‖s, LOW-S. (Apps compute their own canonical digest.)
inline Bytes ecdsaSignLowS(const Bytes& priv, const Bytes& digest32) {
    Bytes out; EC_KEY* key = EC_KEY_new_by_curve_name(NID_secp256k1);
    BIGNUM* bn = BN_bin2bn(priv.data(), (int)priv.size(), nullptr);
    if (key && bn && EC_KEY_set_private_key(key, bn) == 1) {
        ECDSA_SIG* sig = ECDSA_do_sign(digest32.data(), (int)digest32.size(), key);
        if (sig) {
            const BIGNUM *r, *s; ECDSA_SIG_get0(sig, &r, &s);
            const EC_GROUP* grp = EC_KEY_get0_group(key);
            BIGNUM* order = BN_new(); BIGNUM* halford = BN_new(); BN_CTX* ctx = BN_CTX_new();
            EC_GROUP_get_order(grp, order, ctx); BN_rshift1(halford, order);
            BIGNUM* sLow = BN_dup(s);
            if (BN_cmp(s, halford) > 0) BN_sub(sLow, order, s); // low-S
            out.assign(64, 0);
            BN_bn2binpad(r, out.data(), 32); BN_bn2binpad(sLow, out.data() + 32, 32);
            BN_free(sLow); BN_free(order); BN_free(halford); BN_CTX_free(ctx); ECDSA_SIG_free(sig);
        }
    }
    if (bn) BN_free(bn); if (key) EC_KEY_free(key);
    return out;
}

// Recover the compressed pubkey (+address) from a 65-byte recoverable ECDSA sig (R‖S‖V) over a
// 32-byte digest — Q = r⁻¹(s·R − e·G). Used at KEYCARD ENROL: loam requestSign's an enrol digest,
// the card returns R‖S‖V, and we recover the card's public address WITHOUT the private key ever
// leaving the card. V tolerates the Ethereum 27/28 offset. Mirrors scala's address derivation.
inline SignId ecdsaRecover(const Bytes& digest32, const Bytes& sig65) {
    SignId id;
    if (digest32.size() != 32 || sig65.size() != 65) return id;
    int v = sig65[64]; if (v >= 27) v -= 27; if (v < 0 || v > 3) return id;
    EC_GROUP* grp = EC_GROUP_new_by_curve_name(NID_secp256k1);
    BN_CTX* ctx = BN_CTX_new();
    BIGNUM* r = BN_bin2bn(sig65.data(), 32, nullptr);
    BIGNUM* s = BN_bin2bn(sig65.data() + 32, 32, nullptr);
    BIGNUM* e = BN_bin2bn(digest32.data(), 32, nullptr);
    BIGNUM* order = BN_new(); EC_GROUP_get_order(grp, order, ctx);
    BIGNUM* x = BN_new(); BN_copy(x, r);
    if (v & 2) BN_add(x, x, order);                 // x = r (+ n when recovery id ≥ 2)
    EC_POINT* R = EC_POINT_new(grp);
    Bytes enc(33); enc[0] = (unsigned char)(0x02 | (v & 1)); BN_bn2binpad(x, enc.data() + 1, 32);
    if (r && s && e && EC_POINT_oct2point(grp, R, enc.data(), 33, ctx) == 1) {
        BIGNUM* rinv = BN_mod_inverse(nullptr, r, order, ctx);
        BIGNUM* sr = BN_new(); BN_mod_mul(sr, s, rinv, order, ctx);          // s/r
        BIGNUM* er = BN_new(); BN_mod_mul(er, e, rinv, order, ctx);          // e/r
        BIGNUM* erNeg = BN_new(); BN_mod_sub(erNeg, order, er, order, ctx);  // −e/r
        EC_POINT* Q = EC_POINT_new(grp);
        if (EC_POINT_mul(grp, Q, erNeg, R, sr, ctx) == 1) {                  // Q = (−e/r)·G + (s/r)·R
            Bytes pubc(33);
            if (EC_POINT_point2oct(grp, Q, POINT_CONVERSION_COMPRESSED, pubc.data(), 33, ctx) == 33) {
                id.pub = pubc; id.pubHex = toHexS(pubc.data(), 33);
                Bytes h = sha256b(pubc);
                id.address = "0x" + toHexS(h.data(), 32).substr(24, 40);
                id.ok = true;
            }
        }
        EC_POINT_free(Q); if (rinv) BN_free(rinv); BN_free(sr); BN_free(er); BN_free(erNeg);
    }
    EC_POINT_free(R); if (r) BN_free(r); if (s) BN_free(s); if (e) BN_free(e);
    BN_free(order); BN_free(x); BN_CTX_free(ctx); EC_GROUP_free(grp);
    return id;
}

// Extract an EC public key embedded as an UNCOMPRESSED point (0x04‖X32‖Y32) anywhere inside a blob,
// used when the keycard module returns the card's FULL sign response (BER-TLV) rather than a bare
// 65-byte recoverable sig — the full response carries the card's public key directly (the same key
// signWithPathFullResponse exposes), so we take it verbatim instead of recovering it from R‖S‖V.
// Only a point that actually lies on secp256k1 is accepted, so a stray 0x04 byte can't false-match.
inline SignId pubFromUncompressedBlob(const Bytes& blob) {
    SignId id;
    if (blob.size() < 65) return id;
    EC_GROUP* grp = EC_GROUP_new_by_curve_name(NID_secp256k1);
    BN_CTX* ctx = BN_CTX_new();
    EC_POINT* P = EC_POINT_new(grp);
    for (size_t i = 0; i + 65 <= blob.size(); i++) {
        if (blob[i] != 0x04) continue;
        if (EC_POINT_oct2point(grp, P, blob.data() + i, 65, ctx) != 1) continue;  // on-curve check
        Bytes pubc(33);
        if (EC_POINT_point2oct(grp, P, POINT_CONVERSION_COMPRESSED, pubc.data(), 33, ctx) == 33) {
            id.pub = pubc; id.pubHex = toHexS(pubc.data(), 33);
            Bytes h = sha256b(pubc);
            id.address = "0x" + toHexS(h.data(), 32).substr(24, 40);
            id.ok = true;
            break;
        }
    }
    EC_POINT_free(P); BN_CTX_free(ctx); EC_GROUP_free(grp);
    return id;
}

// Normalise whatever the keycard module hands back to the 64-byte LOW-S r‖s that scala/loam verify
// expect. The module may return a bare 64B r‖s, a 65B R‖S‖V (drop V), a DER-encoded ECDSA sig, or
// the card's full BER-TLV sign response with a DER sig embedded in it. We read r,s robustly:
//   - exactly 64/65 bytes → treat as raw r‖s (compact / recoverable) — the common, known forms;
//   - otherwise → scan for a DER SEQUENCE (0x30…) and parse it with OpenSSL, so a DER or full-
//     response blob yields the SAME r,s a naive first-64-bytes read would have mangled.
// Dropping V is safe — the event stores e.pub, so verification never needs recovery. Flipping
// s→n−s stays a valid signature (canonical low-S). r/s are left-padded to 32 (Keycard emits them
// minimal-length, so a short r or s must not shift the compact layout).
inline Bytes compact64LowS(const Bytes& sig) {
    if (sig.size() < 64) return {};
    EC_GROUP* grp = EC_GROUP_new_by_curve_name(NID_secp256k1); BN_CTX* ctx = BN_CTX_new();
    BIGNUM* order = BN_new(); EC_GROUP_get_order(grp, order, ctx);
    BIGNUM* half = BN_new(); BN_rshift1(half, order);

    BIGNUM* rb = nullptr; BIGNUM* sb = nullptr;
    ECDSA_SIG* der = nullptr;
    if (sig.size() != 64 && sig.size() != 65) {          // not a known compact form → look for DER
        for (size_t i = 0; i + 8 <= sig.size(); i++) {
            if (sig[i] != 0x30) continue;
            const unsigned char* p = sig.data() + i;
            ECDSA_SIG* cand = d2i_ECDSA_SIG(nullptr, &p, (long)(sig.size() - i));
            if (!cand) continue;
            const BIGNUM *cr, *cs; ECDSA_SIG_get0(cand, &cr, &cs);
            if (cr && cs && !BN_is_zero(cr) && !BN_is_zero(cs)
                && BN_cmp(cr, order) < 0 && BN_cmp(cs, order) < 0) { der = cand; break; }
            ECDSA_SIG_free(cand);
        }
    }
    if (der) { const BIGNUM *cr, *cs; ECDSA_SIG_get0(der, &cr, &cs); rb = BN_dup(cr); sb = BN_dup(cs); }
    else { rb = BN_bin2bn(sig.data(), 32, nullptr); sb = BN_bin2bn(sig.data() + 32, 32, nullptr); }

    if (BN_cmp(sb, half) > 0) BN_sub(sb, order, sb);        // low-S
    Bytes out(64, 0);
    BN_bn2binpad(rb, out.data(), 32);                       // r (left-padded)
    BN_bn2binpad(sb, out.data() + 32, 32);                  // s (low, left-padded)
    if (der) ECDSA_SIG_free(der);
    BN_free(rb); BN_free(sb); BN_free(half); BN_free(order); BN_CTX_free(ctx); EC_GROUP_free(grp);
    return out;
}

// ── the persistent store: device + soft keys, bindings, default ──────────────
class IdentityStore {
public:
    void load() {
        m_path = (dataDir() + "/identities.json").toStdString();
        std::ifstream f(m_path); if (!f) { ensureDevice(); return; }
        std::stringstream ss; ss << f.rdbuf();
        try { m_j = json::parse(ss.str()); } catch (...) { m_j = json::object(); }
        ensureDevice();
    }
    // meta json {id,kind,label,address,pubHex[,domain]} for one identity id, or null.
    json metaFor(const std::string& id) {
        // Keycard identities hold NO private key (the card does) — resolve them from the
        // keycard array by public material. signDigest for these delegates to the keycard module.
        for (auto& kc : m_j["keycard"])
            if (kc["id"] == id)
                return json{{"id", id}, {"kind", "keycard"}, {"label", kc.value("label", id)},
                            {"address", kc.value("address", "")}, {"pubHex", kc.value("pubHex", "")},
                            {"domain", kc.value("domain", "")}};
        SignId k = keyFor(id); if (!k.ok) return nullptr;
        std::string kind = id == "device" ? "device" : "soft";
        std::string label = id == "device" ? "This device" : labelOfSoft(id);
        return json{{"id", id}, {"kind", kind}, {"label", label}, {"address", k.address}, {"pubHex", k.pubHex}};
    }
    json listIdentities() {
        json arr = json::array();
        arr.push_back(metaFor("device"));
        for (auto& s : m_j["soft"]) arr.push_back(metaFor(s["id"].get<std::string>()));
        for (auto& kc : m_j["keycard"]) arr.push_back(metaFor(kc["id"].get<std::string>()));
        return arr;
    }
    // Record an enrolled keycard identity (public material only — the private key stays on the card).
    // domain is the Keycard sign domain (e.g. "scala") whose m/43'/60'/1582'/… key this address is;
    // it MUST match the mobile domainToSignPath domain so one physical card is one identity across
    // phone + desktop. Returns the meta (or an {error}).
    json addKeycardIdentity(const std::string& label, const std::string& domain,
                            const std::string& address, const std::string& pubHex) {
        if (address.empty() || pubHex.empty()) return json{{"error", "missing address/pubHex"}};
        // de-dupe by address: re-enrolling the same card just relabels it.
        for (auto& kc : m_j["keycard"])
            if (kc["address"] == address) { kc["label"] = label; kc["domain"] = domain; save(); return metaFor(kc["id"].get<std::string>()); }
        std::string id = "keycard-" + toHexS(sha256b(strBytes(pubHex)).data(), 6);
        m_j["keycard"].push_back(json{{"id", id}, {"label", label}, {"domain", domain},
                                      {"address", address}, {"pubHex", pubHex}});
        save(); return metaFor(id);
    }
    void removeKeycardIdentity(const std::string& id) {
        json keep = json::array();
        for (auto& kc : m_j["keycard"]) if (kc["id"] != id) keep.push_back(kc);
        m_j["keycard"] = keep;
        for (auto it = m_j["bindings"].begin(); it != m_j["bindings"].end();)
            if (it.value() == id) it = m_j["bindings"].erase(it); else ++it;
        save();
    }
    json addSoftIdentity(const std::string& label) {
        SignId k = generateIdentity(); if (!k.ok) return json{{"error", "keygen failed"}};
        std::string id = "soft-" + toHexS(sha256b(strBytes(k.pubHex)).data(), 6);
        m_j["soft"].push_back(json{{"id", id}, {"label", label}, {"priv", toHexS(k.priv.data(), 32)}});
        save(); return metaFor(id);
    }
    void renameSoftIdentity(const std::string& id, const std::string& label) {
        for (auto& s : m_j["soft"]) if (s["id"] == id) { s["label"] = label; save(); return; }
    }
    void removeSoftIdentity(const std::string& id) {
        json keep = json::array();
        for (auto& s : m_j["soft"]) if (s["id"] != id) keep.push_back(s);
        m_j["soft"] = keep;
        // drop bindings that pointed at it (they fall back to default)
        for (auto it = m_j["bindings"].begin(); it != m_j["bindings"].end();)
            if (it.value() == id) it = m_j["bindings"].erase(it); else ++it;
        save();
    }
    std::string getDefaultIdentityId() {
        if (m_j.contains("default")) { std::string d = m_j["default"].get<std::string>(); if (exists(d)) return d; }
        return "device";
    }
    void setDefaultIdentityId(const std::string& id) { if (exists(id)) { m_j["default"] = id; save(); } }
    std::string bindingFor(const std::string& c) {
        if (m_j["bindings"].contains(c)) return m_j["bindings"][c].get<std::string>(); return "";
    }
    void bindContainer(const std::string& c, const std::string& id) { m_j["bindings"][c] = id; save(); }
    std::string identityIdForContainer(const std::string& c) {
        std::string b = bindingFor(c); if (!b.empty() && exists(b)) return b; return getDefaultIdentityId();
    }
    json identityForContainer(const std::string& c) { return metaFor(identityIdForContainer(c)); }

    // Sign a hex digest with the container's identity → {sig,pub,address} or {error}.
    json signDigest(const std::string& containerId, const std::string& digestHex) {
        std::string id = identityIdForContainer(containerId);
        // Keycard identity → the private key is on the card; loam_core_impl delegates this to the
        // keycard module (requestSign at 1582' + poll). Signal that here so a direct call is safe.
        for (auto& kc : m_j["keycard"])
            if (kc["id"] == id) return json{{"error", "keycard-delegated"}, {"kind", "keycard"},
                                            {"domain", kc.value("domain", "")}, {"pubHex", kc.value("pubHex", "")},
                                            {"address", kc.value("address", "")}};
        SignId k = keyFor(id);
        if (!k.ok) return json{{"error", "identity key unavailable"}};
        Bytes digest = fromHexB(digestHex);
        if (digest.size() != 32) return json{{"error", "digest must be 32 bytes hex"}};
        Bytes sig = ecdsaSignLowS(k.priv, digest);
        if (sig.size() != 64) return json{{"error", "sign failed"}};
        return json{{"sig", toHexS(sig.data(), 64)}, {"pub", k.pubHex}, {"address", k.address}};
    }

private:
    static QString dataDir() {
        const char* env = std::getenv("LOAM_CORE_DATA");
        QString d = env ? QString::fromUtf8(env)
                        : (QString::fromUtf8(std::getenv("HOME") ? std::getenv("HOME") : "/tmp") + "/.loam-core");
        QDir().mkpath(d); return d;
    }
    void save() { std::ofstream f(m_path); if (f) f << m_j.dump(2); }
    void ensureDevice() {
        if (!m_j.contains("soft")) m_j["soft"] = json::array();
        if (!m_j.contains("keycard")) m_j["keycard"] = json::array();
        if (!m_j.contains("bindings")) m_j["bindings"] = json::object();
        if (!m_j.contains("device") || !m_j["device"].contains("priv")) {
            SignId k = generateIdentity();
            if (k.ok) { m_j["device"] = json{{"priv", toHexS(k.priv.data(), 32)}}; save(); }
        }
    }
    bool exists(const std::string& id) {
        if (id == "device") return true;
        for (auto& s : m_j["soft"]) if (s["id"] == id) return true;
        for (auto& kc : m_j["keycard"]) if (kc["id"] == id) return true;
        return false;
    }
    std::string labelOfSoft(const std::string& id) {
        for (auto& s : m_j["soft"]) if (s["id"] == id) return s.value("label", id);
        return id;
    }
    SignId keyFor(const std::string& id) {
        std::string privHex;
        if (id == "device") privHex = m_j.value("device", json::object()).value("priv", "");
        else for (auto& s : m_j["soft"]) if (s["id"] == id) { privHex = s.value("priv", ""); break; }
        if (privHex.empty()) return SignId{};
        return identityFromPriv(fromHexB(privHex));
    }
    std::string m_path;
    json m_j = json::object();
};

} // namespace loamid

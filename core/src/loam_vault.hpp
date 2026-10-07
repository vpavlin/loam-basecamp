// loam_vault.hpp — the desktop root vault for ADR 0001 identities.
//
// The 12-word recovery phrase is kept encrypted on disk in an Ethereum keystore v3-style envelope
// (the format Logos' keystore also uses): scrypt KDF → AES-128-CTR, MAC = keccak256(dk[16:32] ‖ ct).
// Only the phrase is stored; every identity is re-derived from it (loam_hd.hpp). Self-contained
// (OpenSSL + nlohmann/json + segment_compat's keccak) so it unit-tests without Qt.
#pragma once
#include "loam_bip39_words.hpp"
#include "segment_compat.hpp"
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <cstring>
#include <string>
#include <vector>

namespace loamhd {

using json = nlohmann::json;

inline std::string hexs(const unsigned char* b, size_t n) {
    static const char* h = "0123456789abcdef"; std::string s; s.reserve(n * 2);
    for (size_t i = 0; i < n; i++) { s += h[b[i] >> 4]; s += h[b[i] & 15]; } return s;
}
inline std::vector<unsigned char> unhex(const std::string& s) {
    std::vector<unsigned char> o;
    auto v = [](char c) -> int { if (c >= '0' && c <= '9') return c - '0'; if (c >= 'a' && c <= 'f') return c - 'a' + 10; if (c >= 'A' && c <= 'F') return c - 'A' + 10; return -1; };
    if (s.size() % 2) return o;
    for (size_t i = 0; i < s.size(); i += 2) { int a = v(s[i]), b = v(s[i + 1]); if (a < 0 || b < 0) return {}; o.push_back((unsigned char)(a * 16 + b)); }
    return o;
}

// ---- BIP39: generate / normalise / validate (English) ----

inline int wordIndex(const std::string& w) {
    for (int i = 0; i < 2048; i++) if (w == kBip39English[i]) return i;
    return -1;
}

/** Lower-case, single-spaced phrase. */
inline std::string normalizeMnemonic(const std::string& in) {
    std::string out, cur;
    auto flush = [&] { if (!cur.empty()) { if (!out.empty()) out += ' '; out += cur; cur.clear(); } };
    for (char c : in) { if (c == ' ' || c == '\t' || c == '\n' || c == '\r') flush(); else cur += (char)std::tolower((unsigned char)c); }
    flush();
    return out;
}

/** Valid = 12/15/18/21/24 known words with a correct checksum. */
inline bool validMnemonic(const std::string& phrase) {
    std::vector<int> idx; std::string w;
    std::string p = normalizeMnemonic(phrase) + " ";
    for (char c : p) { if (c == ' ') { if (!w.empty()) { int i = wordIndex(w); if (i < 0) return false; idx.push_back(i); w.clear(); } } else w += c; }
    if (idx.size() % 3 || idx.size() < 12 || idx.size() > 24) return false;
    const size_t bits = idx.size() * 11, csBits = bits / 33, entBits = bits - csBits;
    std::vector<unsigned char> b((bits + 7) / 8, 0);
    for (size_t k = 0; k < idx.size(); k++)
        for (int j = 0; j < 11; j++)
            if (idx[k] & (1 << (10 - j))) { size_t pos = k * 11 + j; b[pos / 8] |= (unsigned char)(0x80 >> (pos % 8)); }
    unsigned char h[32]; SHA256(b.data(), entBits / 8, h);
    for (size_t j = 0; j < csBits; j++) {
        size_t pos = entBits + j;
        bool want = h[j / 8] & (0x80 >> (j % 8)), got = b[pos / 8] & (0x80 >> (pos % 8));
        if (want != got) return false;
    }
    return true;
}

/** A new 12-word phrase from 128 bits of OS randomness. Empty on RNG failure. */
inline std::string newMnemonic() {
    unsigned char ent[17] = {0};
    if (RAND_bytes(ent, 16) != 1) return "";
    unsigned char h[32]; SHA256(ent, 16, h);
    ent[16] = h[0];   // 4 checksum bits live in the top nibble
    std::string out;
    for (int k = 0; k < 12; k++) {
        int v = 0;
        for (int j = 0; j < 11; j++) { int pos = k * 11 + j; if (ent[pos / 8] & (0x80 >> (pos % 8))) v |= 1 << (10 - j); }
        if (k) out += ' ';
        out += kBip39English[v];
    }
    return out;
}

// ---- keystore v3-style envelope ----

struct VaultParams { uint64_t n = 1u << 16; uint32_t r = 8, p = 1; };   // 64 MiB, ~0.3 s

inline bool scryptKey(const std::string& password, const std::vector<unsigned char>& salt, const VaultParams& vp, unsigned char dk[32]) {
    return EVP_PBE_scrypt(password.data(), password.size(), salt.data(), salt.size(), vp.n, vp.r, vp.p,
                          256ull * 1024 * 1024, dk, 32) == 1;
}

inline bool aes128ctr(const unsigned char key[16], const unsigned char iv[16], const std::vector<unsigned char>& in, std::vector<unsigned char>& out) {
    EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new(); if (!c) return false;
    out.assign(in.size(), 0); int n1 = 0, n2 = 0;
    bool ok = EVP_EncryptInit_ex(c, EVP_aes_128_ctr(), nullptr, key, iv) == 1 &&
              EVP_EncryptUpdate(c, out.data(), &n1, in.data(), (int)in.size()) == 1 &&
              EVP_EncryptFinal_ex(c, out.data() + n1, &n2) == 1;
    EVP_CIPHER_CTX_free(c); return ok;
}

inline std::string macOf(const unsigned char dk[32], const std::vector<unsigned char>& ct) {
    std::string m((const char*)dk + 16, 16); m.append((const char*)ct.data(), ct.size());
    std::string h = loam::segcompat::keccak256(m);
    return hexs((const unsigned char*)h.data(), h.size());
}

/** Encrypt a phrase under a password → the vault JSON (never contains the phrase in clear). */
inline json sealMnemonic(const std::string& mnemonic, const std::string& password, VaultParams vp = {}) {
    std::vector<unsigned char> salt(32), iv(16);
    if (RAND_bytes(salt.data(), 32) != 1 || RAND_bytes(iv.data(), 16) != 1) return nullptr;
    unsigned char dk[32];
    if (!scryptKey(password, salt, vp, dk)) return nullptr;
    std::vector<unsigned char> pt(mnemonic.begin(), mnemonic.end()), ct;
    if (!aes128ctr(dk, iv.data(), pt, ct)) return nullptr;
    json j = {
        {"version", 1}, {"kind", "loam-root"}, {"content", "bip39-mnemonic"},
        {"crypto", {{"cipher", "aes-128-ctr"}, {"cipherparams", {{"iv", hexs(iv.data(), 16)}}},
                    {"ciphertext", hexs(ct.data(), ct.size())},
                    {"kdf", "scrypt"}, {"kdfparams", {{"n", vp.n}, {"r", vp.r}, {"p", vp.p}, {"dklen", 32}, {"salt", hexs(salt.data(), 32)}}},
                    {"mac", macOf(dk, ct)}}}};
    OPENSSL_cleanse(dk, 32);
    return j;
}

/** Decrypt a vault. Returns the phrase, or "" with `err` set ("wrong password", "corrupt vault"). */
inline std::string openMnemonic(const json& v, const std::string& password, std::string& err) {
    try {
        const json& c = v.at("crypto");
        VaultParams vp{c.at("kdfparams").at("n").get<uint64_t>(), c.at("kdfparams").at("r").get<uint32_t>(), c.at("kdfparams").at("p").get<uint32_t>()};
        auto salt = unhex(c.at("kdfparams").at("salt").get<std::string>());
        auto iv = unhex(c.at("cipherparams").at("iv").get<std::string>());
        auto ct = unhex(c.at("ciphertext").get<std::string>());
        if (salt.empty() || iv.size() != 16 || ct.empty()) { err = "corrupt vault"; return ""; }
        unsigned char dk[32];
        if (!scryptKey(password, salt, vp, dk)) { err = "corrupt vault"; return ""; }
        if (macOf(dk, ct) != c.at("mac").get<std::string>()) { OPENSSL_cleanse(dk, 32); err = "wrong password"; return ""; }
        std::vector<unsigned char> pt;
        bool ok = aes128ctr(dk, iv.data(), ct, pt);
        OPENSSL_cleanse(dk, 32);
        if (!ok) { err = "corrupt vault"; return ""; }
        return std::string(pt.begin(), pt.end());
    } catch (...) { err = "corrupt vault"; return ""; }
}

} // namespace loamhd

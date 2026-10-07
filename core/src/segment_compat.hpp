// segment_compat.hpp — read reliable-channel content from BOTH delivery library generations.
//
// liblogosdelivery v0.39 (upstream delivery_module 0.3.x) wraps every reliable-channel payload in a
// LIP-243 `SegmentMessage` protobuf — even a payload that fits one segment — while keeping the wire
// marker "RELIABLE-CHANNEL-API/1". v0.38.1 (our fork 0.1.4, and the phones) carries the app bytes in
// SDS field 5 unwrapped. Nothing on the wire says which one you got, so we recognise the wrapper:
//
//   SegmentMessage (proto3, nim-segmentation 0593ef7c segment_message_pb.nim):
//     f1 originalPayloadHash (bytes, keccak256 of the payload, 32 B)   f2 originalPayloadLength (varint)
//     f3 index (varint)   f4 dataSegmentCount (varint)   f5 paritySegmentCount (optional varint)
//     f6 isParity (bool)  f7 payload (bytes)
//   proto3 omits defaults: a single segment has NO f3 (index 0) and NO f6 (not parity).
//
// unwrapSingleSegment() returns the inner payload only for a COMPLETE single-segment message whose
// keccak256 matches f1 — so app bytes that merely look like a protobuf are never mangled (our apps put
// base64 text there, which can't start with the 0x0A tag anyway). Multi-segment sets (> 100 KiB by
// default) need reassembly; isSegmentWrapped() lets the caller notice them instead of misreading them.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>

namespace loam::segcompat {

// ---- Keccak-256 (the original Keccak padding 0x01, as Ethereum/nimcrypto use — NOT SHA3-256) ----
namespace detail {
inline uint64_t rotl(uint64_t x, int s) { return s ? (x << s) | (x >> (64 - s)) : x; }
inline void keccakf(uint64_t st[25]) {
    static const uint64_t RC[24] = {
        0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL, 0x8000000080008000ULL,
        0x000000000000808bULL, 0x0000000080000001ULL, 0x8000000080008081ULL, 0x8000000000008009ULL,
        0x000000000000008aULL, 0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
        0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL, 0x8000000000008003ULL,
        0x8000000000008002ULL, 0x8000000000000080ULL, 0x000000000000800aULL, 0x800000008000000aULL,
        0x8000000080008081ULL, 0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL};
    static const int R[25] = {0, 1, 62, 28, 27, 36, 44, 6, 55, 20, 3, 10, 43,
                              25, 39, 41, 45, 15, 21, 8, 18, 2, 61, 56, 14};
    for (int round = 0; round < 24; ++round) {
        uint64_t C[5], D[5], B[25];
        for (int x = 0; x < 5; ++x) C[x] = st[x] ^ st[x + 5] ^ st[x + 10] ^ st[x + 15] ^ st[x + 20];
        for (int x = 0; x < 5; ++x) D[x] = C[(x + 4) % 5] ^ rotl(C[(x + 1) % 5], 1);
        for (int i = 0; i < 25; ++i) st[i] ^= D[i % 5];
        for (int x = 0; x < 5; ++x)
            for (int y = 0; y < 5; ++y) B[y + 5 * ((2 * x + 3 * y) % 5)] = rotl(st[x + 5 * y], R[x + 5 * y]);
        for (int x = 0; x < 5; ++x)
            for (int y = 0; y < 5; ++y)
                st[x + 5 * y] = B[x + 5 * y] ^ (~B[(x + 1) % 5 + 5 * y] & B[(x + 2) % 5 + 5 * y]);
        st[0] ^= RC[round];
    }
}
}  // namespace detail

inline std::string keccak256(const std::string& in) {
    const size_t rate = 136;
    uint64_t st[25] = {0};
    auto absorb = [&](const unsigned char* blk) {
        for (size_t i = 0; i < rate / 8; ++i) {
            uint64_t w = 0;
            for (int b = 0; b < 8; ++b) w |= (uint64_t)blk[i * 8 + b] << (8 * b);
            st[i] ^= w;
        }
        detail::keccakf(st);
    };
    const unsigned char* p = (const unsigned char*)in.data();
    size_t n = in.size();
    while (n >= rate) { absorb(p); p += rate; n -= rate; }
    unsigned char last[136] = {0};
    std::memcpy(last, p, n);
    last[n] ^= 0x01;
    last[rate - 1] ^= 0x80;
    absorb(last);
    std::string out(32, '\0');
    for (int i = 0; i < 32; ++i) out[i] = (char)((st[i / 8] >> (8 * (i % 8))) & 0xff);
    return out;
}

// ---- SegmentMessage recognition ----
struct Segment {
    bool ok = false;                 // parsed cleanly, only known fields, f1 is 32 bytes, f4 >= 1
    std::string hash, payload;       // f1, f7
    uint64_t length = 0, index = 0, dataCount = 0, parityCount = 0;
    bool isParity = false;
};

inline Segment parseSegment(const std::string& b) {
    Segment s;
    if (b.empty() || (unsigned char)b[0] != 0x0A) return s;   // must start with f1 (bytes)
    size_t i = 0, n = b.size();
    auto varint = [&](uint64_t& out) -> bool {
        uint64_t r = 0; int sh = 0;
        while (i < n) {
            unsigned char x = (unsigned char)b[i++];
            r |= (uint64_t)(x & 0x7f) << sh;
            if (!(x & 0x80)) { out = r; return true; }
            sh += 7; if (sh > 63) return false;
        }
        return false;
    };
    bool haveHash = false;
    while (i < n) {
        uint64_t tag; if (!varint(tag)) return s;
        const int f = (int)(tag >> 3), wt = (int)(tag & 7);
        if (wt == 2 && (f == 1 || f == 7)) {
            uint64_t ln; if (!varint(ln) || i + ln > n) return s;
            (f == 1 ? s.hash : s.payload) = b.substr(i, (size_t)ln);
            if (f == 1) haveHash = true;
            i += (size_t)ln;
        } else if (wt == 0 && f >= 2 && f <= 6) {
            uint64_t v; if (!varint(v)) return s;
            if (f == 2) s.length = v; else if (f == 3) s.index = v; else if (f == 4) s.dataCount = v;
            else if (f == 5) s.parityCount = v; else s.isParity = v != 0;
        } else {
            return s;   // unknown field or wire type: not a SegmentMessage
        }
    }
    s.ok = haveHash && s.hash.size() == 32 && s.dataCount >= 1;
    return s;
}

// True when `b` is any LIP-243 segment (single or part of a set) — the caller can then tell a
// multi-segment set apart from app bytes instead of passing protobuf through as if it were content.
inline bool isSegmentWrapped(const std::string& b) { return parseSegment(b).ok; }

// The inner payload if `b` is a complete, hash-verified single segment; otherwise `b` unchanged.
inline std::string unwrapSingleSegment(const std::string& b) {
    const Segment s = parseSegment(b);
    if (!s.ok || s.dataCount != 1 || s.index != 0 || s.isParity || s.parityCount != 0) return b;
    if (s.length != s.payload.size()) return b;
    if (keccak256(s.payload) != s.hash) return b;
    return s.payload;
}

}  // namespace loam::segcompat

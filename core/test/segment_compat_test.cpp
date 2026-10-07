// Tests for segment_compat.hpp. Golden vectors are upstream's (nim-segmentation 0593ef7c
// tests/test_wire_vectors.nim); single-segment frames are built exactly as performSegmentation does.
//   g++ -std=c++17 -I core/src core/test/segment_compat_test.cpp -o /tmp/seg_test && /tmp/seg_test
#include "segment_compat.hpp"
#include <cstdio>
#include <string>
using namespace loam::segcompat;

static int fails = 0;
#define CHECK(c) do { if (c) printf("  ok   %s\n", #c); else { printf("  FAIL %s\n", #c); ++fails; } } while (0)

static std::string hex2(const std::string& h) {
    std::string o; for (size_t i = 0; i + 1 < h.size(); i += 2) o.push_back((char)std::stoi(h.substr(i, 2), nullptr, 16)); return o;
}
static std::string tohex(const std::string& s) {
    static const char* d = "0123456789abcdef"; std::string o;
    for (unsigned char c : s) { o.push_back(d[c >> 4]); o.push_back(d[c & 15]); } return o;
}
static void varint(std::string& o, uint64_t v) { while (v >= 0x80) { o.push_back((char)(v | 0x80)); v >>= 7; } o.push_back((char)v); }
// What v0.39 puts on the wire for a payload that fits one segment (proto3: index/isParity omitted).
static std::string wrapSingle(const std::string& p) {
    std::string o;
    o.push_back(0x0A); varint(o, 32); o += keccak256(p);
    if (!p.empty()) { o.push_back(0x10); varint(o, p.size()); }
    o.push_back(0x20); varint(o, 1);
    if (!p.empty()) { o.push_back(0x3A); varint(o, p.size()); o += p; }
    return o;
}

int main() {
    printf("keccak256 (Ethereum/nimcrypto variant):\n");
    CHECK(tohex(keccak256("")) == "c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470");
    CHECK(tohex(keccak256("abc")) == "4e03657aea45a94fc7d47ba826c8d667c0d1e6e33a64a036ec44f58fa12d6c45");
    std::string big(300, 'x');   // > one 136-byte block
    CHECK(keccak256(big).size() == 32 && keccak256(big) != keccak256(big + "y"));

    const std::string H = "000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F";
    printf("upstream golden vectors:\n");
    const std::string dataSeg = hex2("0A20" + H + "10F403" "1801" "2003" "3A03AABBCC");
    const std::string paritySeg = hex2("0A20" + H + "10F403" "2003" "2801" "3001" "3A021122");
    const std::string minimal = hex2("0A20" + H + "2001");
    Segment d = parseSegment(dataSeg);
    CHECK(d.ok && d.index == 1 && d.dataCount == 3 && d.length == 500 && d.payload == hex2("AABBCC"));
    Segment p = parseSegment(paritySeg);
    CHECK(p.ok && p.isParity && p.parityCount == 1 && p.payload == hex2("1122"));
    CHECK(parseSegment(minimal).ok);
    CHECK(isSegmentWrapped(dataSeg) && isSegmentWrapped(paritySeg));
    CHECK(unwrapSingleSegment(dataSeg) == dataSeg);       // part of a 3-segment set: never "unwrapped"
    CHECK(unwrapSingleSegment(paritySeg) == paritySeg);   // parity
    CHECK(unwrapSingleSegment(minimal) == minimal);       // hash doesn't match the (empty) payload

    printf("single segment (what a v0.39 peer sends for a normal message):\n");
    const std::string app = "eyJ2IjoxLCJ0eXBlIjoiRVZFTlQifQ==";   // our apps' base64 text
    const std::string w = wrapSingle(app);
    CHECK(isSegmentWrapped(w));
    CHECK(unwrapSingleSegment(w) == app);
    const std::string bin = std::string("\x0a\x00\xff\x10 binary\x00", 11);
    CHECK(unwrapSingleSegment(wrapSingle(bin)) == bin);
    CHECK(unwrapSingleSegment(wrapSingle("")) == "");
    std::string tampered = w; tampered.back() ^= 1;
    CHECK(unwrapSingleSegment(tampered) == tampered);     // hash mismatch → left alone

    printf("old-format content (v0.38.1 / phones) passes through untouched:\n");
    CHECK(unwrapSingleSegment(app) == app);
    CHECK(!isSegmentWrapped(app));
    CHECK(unwrapSingleSegment("") == "");
    const std::string looksLikeProto = hex2("0A0548454C4C4F");   // f1 = "HELLO" (5 bytes, not 32)
    CHECK(!isSegmentWrapped(looksLikeProto) && unwrapSingleSegment(looksLikeProto) == looksLikeProto);
    const std::string truncated = w.substr(0, w.size() - 3);
    CHECK(unwrapSingleSegment(truncated) == truncated);

    printf(fails ? "\nSEGMENT COMPAT: %d FAILED\n" : "\nSEGMENT COMPAT GREEN\n", fails);
    return fails ? 1 : 0;
}

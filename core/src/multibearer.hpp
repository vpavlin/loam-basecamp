// multibearer.hpp — the transport core of loam_core: a set of BEARERS (delivery today,
// ble_mesh / lora tomorrow) behind one fan-out + dedup. The C++ desktop mirror of
// loam-transport's bearer.ts (MultiBearer + WakuBearer/BleMeshBearer). See ADR 0015.
//
// A bearer moves OPAQUE sealed bytes on content topics; it knows nothing about crypto.
// The facade (loam_core_impl) seals nothing either — apps seal, call sendSealed(), and
// open on receive. MultiBearer fans each write to every enabled bearer and funnels all
// receives into ONE dedup'd stream (dedup by frameId = sha256(topic‖0x00‖bytes)[:16]),
// so the same sealed bytes arriving over Waku AND BLE collapse to a single delivery.
//
// IBearer is std::string-only (no LogosMap) so MultiBearer stays generated-type-free;
// LogosMap appears ONLY inside DeliveryBearer<LogosMap>, whose delivery calls arrive as
// an `Ops` struct built in the .cpp (the generated delivery_module type can't be named
// in a header — same reason as scala's logos_transport.hpp).
#pragma once
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <cstdio>
#include <cstdint>
#include <map>
#include <set>
#include <deque>
#include <algorithm>
#include <openssl/sha.h>

namespace loam {

// ── base64 (RFC 4648, no newlines) ──────────────────────────────────────────
namespace b64 {
inline const char *tbl() { return "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"; }
inline std::string encode(const std::string &in) {
    std::string out; out.reserve(((in.size() + 2) / 3) * 4);
    int val = 0, bits = -6; const unsigned char *p = (const unsigned char *)in.data();
    for (size_t i = 0; i < in.size(); ++i) { val = (val << 8) + p[i]; bits += 8;
        while (bits >= 0) { out.push_back(tbl()[(val >> bits) & 0x3F]); bits -= 6; } }
    if (bits > -6) out.push_back(tbl()[((val << 8) >> (bits + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}
// Accepts standard AND URL-safe base64 (delivery >= 0.3.0 hands payloads as {"_bytes": <url-safe>}).
// The old decoder stopped at the first '-' or '_' and silently returned a TRUNCATED frame.
inline std::string decode(const std::string &in) {
    std::vector<int> T(256, -1); for (int i = 0; i < 64; i++) T[(unsigned char)tbl()[i]] = i;
    T[(unsigned char)'-'] = 62; T[(unsigned char)'_'] = 63;
    std::string out; int val = 0, bits = -8;
    for (unsigned char c : in) { if (c == '=' || T[c] == -1) break;
        val = (val << 6) + T[c]; bits += 6;
        if (bits >= 0) { out.push_back(char((val >> bits) & 0xFF)); bits -= 8; } }
    return out;
}
} // namespace b64

// frameId = first 16 bytes of sha256(topic‖0x00‖bytes), hex. Bearer-independent, so the
// same sealed bytes over any pipe dedup to one. Matches bearer.ts's frame identity.
inline std::string frameId(const std::string &topic, const std::string &bytes) {
    std::string buf; buf.reserve(topic.size() + 1 + bytes.size());
    buf.append(topic); buf.push_back('\0'); buf.append(bytes);
    unsigned char d[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char *)buf.data(), buf.size(), d);
    static const char *hex = "0123456789abcdef";
    std::string out; out.reserve(32);
    for (int i = 0; i < 16; ++i) { out.push_back(hex[d[i] >> 4]); out.push_back(hex[d[i] & 0xF]); }
    return out;
}

// Where a frame's time comes from. `ts` is the NETWORK message timestamp in nanoseconds: the
// WakuMessage timestamp the publisher set, which RLN binds into the proof and relays reject when it is
// more than ~20 s off their clock (logos-delivery rln_evm.nim MaxClockGapSeconds). 0 = the bearer has
// none (BLE mesh, or delivery's channel event, which stamps local receive time), so apps must not
// treat it as verified. `source` is delivery's "live" | "history" (store), "" when unknown.
struct FrameMeta {
    int64_t ts = 0;
    std::string source;
    std::string bearer;
};
// A frame handed up from a bearer: the topic, the sender's id (if the bearer knows it),
// the ONCE-decoded sealed bytes (app peels the last layer), and its meta.
using FrameCb = std::function<void(const std::string &topic, const std::string &senderId,
                                   const std::string &sealedBytes, const FrameMeta &meta)>;
// What MultiBearer hands the facade: the frame, its meta, its frameId, and whether this is a
// REDELIVERY (the same frame seen again with an earlier network timestamp).
using ReceivedCb = std::function<void(const std::string &topic, const std::string &senderId,
                                      const std::string &sealedBytes, const FrameMeta &meta,
                                      const std::string &frameId, bool redelivery)>;

// ── Bearer interface (std::string only) ─────────────────────────────────────
class IBearer {
public:
    virtual ~IBearer() = default;
    virtual const std::string &name() const = 0;
    bool enabled() const { return m_enabled; }
    void setEnabled(bool on) { m_enabled = on; }
    int priority() const { return m_priority; }
    void setPriority(int p) { m_priority = p; }
    // MultiBearer sets this; the bearer calls it for every received frame.
    FrameCb onFrame;
    virtual void start() = 0;
    virtual void join(const std::string &topic) = 0;
    virtual void send(const std::string &topic, const std::string &sealedBytes) = 0;
    virtual bool ready() const = 0;
    // Set a peer count from an external poll (delivery); no-op for bearers that don't track it.
    virtual void setPeers(long) {}
    // Re-establish the node's connections when it has silently gone peerless (no built-in discovery
    // re-dials on its own). Default no-op; the delivery bearer stop→start→rejoins.
    virtual void reconnect() {}
    // A JSON object: {"name","enabled","priority","ready","peers","rx","tx", …}
    virtual std::string metricsJson() const = 0;
protected:
    bool m_enabled = true;
    int m_priority = 0;
    uint64_t m_rx = 0, m_tx = 0;
};

// ── MultiBearer: fan-out + dedup ────────────────────────────────────────────
class MultiBearer {
public:
    // The facade sets this; MultiBearer calls it once per NEW frame (post-dedup), and once more
    // whenever a duplicate brings an EARLIER network timestamp than the one already delivered.
    ReceivedCb onReceived;
    // Overall reachable-peer count; the facade fills it from the delivery metrics poll.
    long overallPeers = -1;

    void add(std::unique_ptr<IBearer> b) {
        b->onFrame = [this](const std::string &topic, const std::string &sender,
                            const std::string &bytes, const FrameMeta &meta) { ingest(topic, sender, bytes, meta); };
        m_bearers.push_back(std::move(b));
    }
    void startAll()                       { for (auto &b : ordered()) b->start(); }
    void joinAll(const std::string &t)    { for (auto &b : ordered()) if (b->enabled()) b->join(t); }

    // Fan out to every ENABLED bearer, in priority order. Fan-out-to-all is the resilient
    // default; priority only tunes the order (and lets a UI express preference/cost).
    void sendSealed(const std::string &topic, const std::string &sealedBytes) {
        for (auto *b : ordered()) if (b->enabled()) b->send(topic, sealedBytes);
    }

    IBearer *byName(const std::string &n) {
        for (auto &b : m_bearers) if (b->name() == n) return b.get();
        return nullptr;
    }
    // Priority order as a name list (for a UI to render / reorder).
    std::vector<IBearer *> ordered() {
        std::vector<IBearer *> v; for (auto &b : m_bearers) v.push_back(b.get());
        std::stable_sort(v.begin(), v.end(), [](IBearer *a, IBearer *c) { return a->priority() < c->priority(); });
        return v;
    }
    void setPriorityOrder(const std::vector<std::string> &order) {
        for (size_t i = 0; i < order.size(); ++i) if (auto *b = byName(order[i])) b->setPriority((int)i);
    }

    // {"connected":bool,"peers":N,"bearers":[ {…}, … ]}
    std::string metricsJson() {
        std::string out = "{\"bearers\":[";
        bool first = true; bool anyReady = false;
        for (auto *b : ordered()) {
            if (!first) out += ",";
            out += b->metricsJson(); first = false;
            anyReady = anyReady || b->ready();
        }
        out += "]";
        out += ",\"peers\":" + std::to_string(overallPeers);
        out += ",\"connected\":" + std::string(anyReady ? "true" : "false") + "}";
        return out;
    }

private:
    // Dedup by frameId, keeping the EARLIEST network timestamp seen. The first copy is delivered at
    // once; a later copy is delivered again only if it carries an earlier network ts than the one
    // delivered (or the first had none) — e.g. a catch-up re-publish reached us before the store's
    // copy of the original. Apps dedup events themselves and keep the earliest ts.
    void ingest(const std::string &topic, const std::string &sender,
                const std::string &sealedBytes, const FrameMeta &meta) {
        const std::string id = frameId(topic, sealedBytes);
        auto it = m_seen.find(id);
        if (it != m_seen.end()) {
            const int64_t had = it->second;
            if (meta.ts <= 0 || (had > 0 && meta.ts >= had)) return;   // no earlier network time → drop
            it->second = meta.ts;
            if (onReceived) onReceived(topic, sender, sealedBytes, meta, id, true);
            return;
        }
        if (m_seenOrder.size() >= kSeenMax) {              // bounded seen-set
            m_seen.erase(m_seenOrder.front()); m_seenOrder.pop_front();
        }
        m_seen.emplace(id, meta.ts);
        m_seenOrder.push_back(id);
        if (onReceived) onReceived(topic, sender, sealedBytes, meta, id, false);
    }
    static constexpr size_t kSeenMax = 8192;
    std::vector<std::unique_ptr<IBearer>> m_bearers;
    std::map<std::string, int64_t> m_seen;   // frameId → earliest network ts delivered (0 = none)
    std::deque<std::string> m_seenOrder;
};

} // namespace loam

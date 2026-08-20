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
inline std::string decode(const std::string &in) {
    std::vector<int> T(256, -1); for (int i = 0; i < 64; i++) T[(unsigned char)tbl()[i]] = i;
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

// A frame handed up from a bearer: the topic, the sender's id (if the bearer knows it),
// the ONCE-decoded sealed bytes (app peels the last layer), and a bearer-local timestamp.
using FrameCb = std::function<void(const std::string &topic, const std::string &senderId,
                                   const std::string &sealedBytes, int64_t ts)>;

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
    // The facade sets this; MultiBearer calls it once per NEW frame (post-dedup).
    FrameCb onReceived;
    // Overall reachable-peer count; the facade fills it from the delivery metrics poll.
    long overallPeers = -1;

    void add(std::unique_ptr<IBearer> b) {
        b->onFrame = [this](const std::string &topic, const std::string &sender,
                            const std::string &bytes, int64_t ts) { ingest(topic, sender, bytes, ts); };
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
    void ingest(const std::string &topic, const std::string &sender,
                const std::string &sealedBytes, int64_t ts) {
        const std::string id = frameId(topic, sealedBytes);
        if (!m_seen.insert(id).second) return;            // duplicate across bearers → drop
        if (m_seenOrder.size() >= kSeenMax) {              // bounded seen-set
            m_seen.erase(m_seenOrder.front()); m_seenOrder.pop_front();
        }
        m_seenOrder.push_back(id);
        if (onReceived) onReceived(topic, sender, sealedBytes, ts);
    }
    static constexpr size_t kSeenMax = 8192;
    std::vector<std::unique_ptr<IBearer>> m_bearers;
    std::set<std::string> m_seen;
    std::deque<std::string> m_seenOrder;
};

} // namespace loam

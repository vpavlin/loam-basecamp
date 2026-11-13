// delivery_transport.hpp — Loam's wire on Logos Delivery, for any Basecamp core.
//
// Header-only. loam_core uses it for its delivery bearer, and an app core can vendor it to talk to
// delivery_module directly with the same wire behaviour, so it interoperates with Loam phones and
// loam_core desktops (WhisperBox's "official" flavour). One source of truth for:
//
//   - the network config: logos.test without RLN (delivery_module >= 0.3.0 turns RLN on for the
//     preset NAME "logos.test", so we spell out its parameters instead), `nodeConfig()`;
//   - the receive decoding: every payload shape delivery hands out ({"_bytes"} URL-safe base64,
//     byte arrays, strings), the SDS frame (f4 topic, f5 content, f7 sender), LIP-243
//     SegmentMessage unwrapping, `decodeReceived()`; and the send encoding, `encodeSend()`;
//   - the node lifecycle: register receive handlers before createNode, wait for nodeStarted,
//     re-subscribe after ready (cpp-sdk before logos-cpp-sdk #134), joins requested early,
//     ADOPT a node another module already created, peerless reconnect.
//
// Templated on the JSON type (`LogosMap` = nlohmann::json in Basecamp cores). The delivery_module
// calls arrive as `Ops` std::functions the core builds from its generated client, because the
// generated type can't be named in a shared header.
//
// Threading: no locks inside. Call every method, and deliver every Ops callback, on one thread (or
// under the caller's lock), as loam_core does under its module mutex.
//
// Bump LOAM_DELIVERY_WIRE_VERSION when the bytes on the wire or their decoding change; the shared
// vectors (core/test/delivery_vectors.json, also run by loam-transport) must change with it.
#pragma once
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "segment_compat.hpp"

#define LOAM_DELIVERY_WIRE_VERSION 1

namespace loam::delivery {

// ── base64: encode standard; decode standard AND URL-safe, padded or not ──────────────────────
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
    int T[256];
    for (int &t : T) t = -1;
    for (int i = 0; i < 64; i++) T[(unsigned char)tbl()[i]] = i;
    T[(unsigned char)'-'] = 62; T[(unsigned char)'_'] = 63;
    std::string out; int val = 0, bits = -8;
    for (unsigned char c : in) { if (c == '=' || T[c] == -1) break;
        val = (val << 6) + T[c]; bits += 6;
        if (bits >= 0) { out.push_back(char((val >> bits) & 0xFF)); bits -= 8; } }
    return out;
}
} // namespace b64

// One length-delimited (wire type 2) field of a protobuf buffer; "" if absent or malformed.
inline std::string protoField(const std::string &b, int want) {
    size_t i = 0, n = b.size();
    auto rv = [&](uint64_t &out) -> bool {
        uint64_t r = 0; int s = 0;
        while (i < n) { unsigned char x = (unsigned char)b[i++]; r |= (uint64_t)(x & 0x7f) << s;
            if (!(x & 0x80)) { out = r; return true; } s += 7; if (s > 63) return false; }
        return false;
    };
    while (i < n) {
        uint64_t tag; if (!rv(tag)) break;
        int f = (int)(tag >> 3), wt = (int)(tag & 7);
        if (wt == 2) {
            uint64_t ln;
            if (!rv(ln) || i + ln > n) break;
            if (f == want) return b.substr(i, (size_t)ln);
            i += (size_t)ln;
        }
        else if (wt == 0) { uint64_t v; if (!rv(v)) break; }
        else if (wt == 5) { if (i + 4 > n) break; i += 4; }
        else if (wt == 1) { if (i + 8 > n) break; i += 8; }
        else break;
    }
    return std::string();
}

// ── network config ─────────────────────────────────────────────────────────────────────────
// The delivery_module createNode config for an app's settings `app` (a JSON object, may be empty):
// the flat node options, with
//   - the layered shape ({preset, messagingOverrides:{dash-case}}) flattened (delivery switches to
//     the flat parser as soon as any other top-level key is present),
//   - mode "Core" and random ports unless set,
//   - for the logos.test network (no preset, or "logos.test"): with rln=false, preset "" plus
//     logos.test's own parameters (cluster 2, 8 shards, the fleet entry nodes), which is RLN off;
//     with rln=true, the preset name, which needs the RLN modules and a funded membership.
// Keys that are not node options must be removed by the caller first.
template <class J>
J nodeConfig(J app, bool rln = false, const std::string &defaultMode = "Core") {
    J j = app.is_object() ? app : J::object();
    if (j.contains("messagingOverrides") && j["messagingOverrides"].is_object()) {
        for (auto it = j["messagingOverrides"].begin(); it != j["messagingOverrides"].end(); ++it) {
            std::string k, key = it.key();
            for (size_t i = 0; i < key.size(); ++i) {
                if (key[i] == '-' && i + 1 < key.size()) k.push_back((char)std::toupper((unsigned char)key[++i]));
                else k.push_back(key[i]);
            }
            if (!j.contains(k)) j[k] = it.value();
        }
        j.erase("messagingOverrides");
    }
    if (!j.contains("mode")) j["mode"] = defaultMode;
    // The flat config defaults to FIXED tcp 60000 / discv5 udp 9000: a second node on the machine
    // would fail START_NODE with "Address already in use".
    if (!j.contains("tcpPort")) j["tcpPort"] = 0;
    if (!j.contains("discv5UdpPort")) j["discv5UdpPort"] = 0;
    const bool logosTest = !j.contains("preset") ||
        (j["preset"].is_string() && j["preset"].template get<std::string>() == "logos.test");
    if (logosTest && rln) {
        j["preset"] = "logos.test";
    } else if (logosTest) {
        // Mirror of logos-delivery v0.39 networks_config.nim LogosTestConf (RLN off there).
        j["preset"] = "";
        if (!j.contains("clusterId")) j["clusterId"] = 2;
        if (!j.contains("numShardsInNetwork")) j["numShardsInNetwork"] = 8;
        if (!j.contains("maxMessageSize")) j["maxMessageSize"] = "150KiB";
        if (!j.contains("discv5Discovery")) j["discv5Discovery"] = true;
        if (!j.contains("enableKadDiscovery")) j["enableKadDiscovery"] = true;
        if (!j.contains("reliabilityEnabled")) j["reliabilityEnabled"] = true;
        if (!j.contains("entryNodes")) j["entryNodes"] = J::array({
            "/dns4/node-01.do-ams3.logos.test.status.im/tcp/30303/p2p/16Uiu2HAmQ9X2xDfPG3uL77V9piYDhjq14JhKCtcmNYsTMKNqrKCj",
            "/dns4/node-02.do-ams3.logos.test.status.im/tcp/30303/p2p/16Uiu2HAmB8NYprrfQrgWVzsJtYWkfjsXbmJEGNMG6othXsQ53BwG",
            "/dns4/node-01.gc-us-central1-a.logos.test.status.im/tcp/30303/p2p/16Uiu2HAmF8WtwGPmeGHgYAX2277jHgy5cW9F7zsB8EqUjBZQAZQ3",
            "/dns4/node-02.gc-us-central1-a.logos.test.status.im/tcp/30303/p2p/16Uiu2HAmUuXhUW9bdJpzN1kfDziFiUZo4bszTk66cvr7uuyCHXR7",
            "/dns4/node-01.ac-cn-hongkong-c.logos.test.status.im/tcp/30303/p2p/16Uiu2HAmL3oU95jh1BZHozn3uNhx8HEneirgr8M1jEAapzXGDqRF",
            "/dns4/node-02.ac-cn-hongkong-c.logos.test.status.im/tcp/30303/p2p/16Uiu2HAm28CoBZjpyxsanC8tQpbvZ7bZJnVYuB1EgFzb571qpWsV"});
    }
    return j;
}

// ── receive decoding ───────────────────────────────────────────────────────────────────────
struct Frame {
    std::string topic;   // the content topic (from the SDS frame when there is one)
    std::string sender;  // the SDS sender id, or the event's sender ("" for plain relay)
    std::string bytes;   // what the app sent: the sealed bytes, exactly
    int64_t ts = 0;
};

enum class Decoded {
    Frame,      // `out` holds a frame
    Empty,      // nothing usable (foreign or undecodable traffic): drop it
    Segmented,  // one part of a multi-segment SDS message (> 100 KiB): not reassembled yet, dropped
};

// A delivery payload as text: a string, a byte array, or {"_bytes": base64}.
template <class J>
std::string payloadText(const J &v) {
    if (v.is_string()) return v.template get<std::string>();
    if (v.is_array()) { std::string s; s.reserve(v.size());
        for (const auto &c : v) if (c.is_number_integer()) s.push_back((char)c.template get<int>());
        return s; }
    if (v.is_object() && v.contains("_bytes") && v["_bytes"].is_string())
        return b64::decode(v["_bytes"].template get<std::string>());
    return std::string();
}

// Decode one received message. `topicArg`/`senderArg` are what the event said (messageReceived:
// the content topic and ""; channelMessageReceived: the channel id and the sender), `payload` the
// event's payload argument untouched.
//
// The wire, both directions: the app's sealed bytes are base64-encoded (the inner layer) and sent
// as the payload; delivery adds its own layers. Received, the payload is either
//   - plain relay (the default): the inner base64 text → Frame::bytes = the sealed bytes; or
//   - an SDS Message protobuf (f4 channelId == content topic, f5 content, f7 original sender), the
//     content possibly inside a single-segment LIP-243 SegmentMessage (delivery >= 0.3.0)
//     → Frame::bytes = the content as is (the inner base64 text; channel apps peel it).
template <class J>
Decoded decodeReceived(const std::string &topicArg, const std::string &senderArg, const J &payload,
                       int64_t ts, Frame &out) {
    std::string enc = payloadText(payload);
    if (enc.empty() && payload.is_object() && payload.contains("payload")) enc = payloadText(payload["payload"]);
    if (enc.empty()) return Decoded::Empty;
    std::string bytes, topic = topicArg, sender = senderArg;
    const std::string f4 = protoField(enc, 4), f5 = protoField(enc, 5);
    if (!f5.empty() && !f4.empty() && f4.rfind("/", 0) == 0) {
        topic = f4;   // authoritative: the event's own arguments differ between the two event shapes
        bytes = loam::segcompat::unwrapSingleSegment(f5);
        if (loam::segcompat::isSegmentWrapped(bytes)) return Decoded::Segmented;
        const std::string f7 = protoField(enc, 7);
        if (!f7.empty()) sender = f7;
        // SDS content is handed on RAW (still the inner base64 text): loam_core always did this and
        // apps that use channels peel the base64 themselves (single or double). Wire version 1.
    } else {
        // Plain relay: the payload is the inner base64 text. Peel it to the sealed bytes.
        bytes = b64::decode(enc);
    }
    if (bytes.empty()) return Decoded::Empty;
    out.topic = topic; out.sender = sender; out.bytes = std::move(bytes); out.ts = ts;
    return Decoded::Frame;
}

// The payload to send for `bytes`: repr 1 = a byte array of the inner base64 text (what every
// delivery_module so far accepts), repr 2 = the text as a JSON string.
template <class J>
J encodeSend(const std::string &bytes, int repr = 1) {
    const std::string enc = b64::encode(bytes);
    if (repr == 2) return J(enc);
    J a = J::array();
    for (unsigned char c : enc) a.push_back((unsigned)c);
    return a;
}

// ── the transport ──────────────────────────────────────────────────────────────────────────
template <class J>
class DeliveryTransport {
public:
    using Cb = std::function<void(bool ok, const std::string &err)>;
    // An event as delivery_module emits it: (topic or channel id, sender or "", payload, timestamp).
    using RecvCb = std::function<void(const std::string &topic, const std::string &sender, const J &payload, int64_t ts)>;

    // The delivery_module calls, built by the core from its generated client (async variants).
    // Required: createNode, start, subscribe, sendRaw, onMessage. The rest are optional.
    struct Ops {
        std::function<void(const std::string &cfgJson, Cb)> createNode;
        std::function<void(Cb)> start;
        std::function<void(Cb)> stop;                                          // reconnect
        std::function<void(const std::string &contentTopic, Cb)> subscribe;
        std::function<void(const std::string &contentTopic, const J &payload, Cb)> sendRaw;
        // Subscribe to messageReceived; the core maps the event's arguments to RecvCb(contentTopic,
        // "", payload, ts). Use the RAW event payload, not the generated client's bytes conversion
        // (it zeroes payloads that aren't {"_bytes"}). May be called more than once (re-subscribe).
        std::function<void(RecvCb)> onMessage;
        // delivery >= 0.3.0: start() only dispatches; nodeStarted(success, message) is the outcome.
        std::function<void(std::function<void(bool ok, const std::string &msg)>)> onNodeStarted;
        // getNodeInfo("Metrics") text, for peer counts (tick).
        std::function<void(std::function<void(bool ok, const std::string &metrics)>)> metrics;
        // getConnectionStatus: "Disconnected" until the node runs and has peers, then e.g. "Connected"
        // / "PartiallyConnected". How an adopted node is seen to run (metrics answer on a node that
        // was created but never started).
        std::function<void(std::function<void(bool ok, const std::string &status)>)> connectionStatus;
        // SDS Reliable Channels (opt-in, Config::useChannels).
        std::function<void(const std::string &channelId, const std::string &contentTopic, const std::string &senderId, Cb)> channelCreate;
        std::function<void(const std::string &channelId, const J &payload, Cb)> channelSend;
        std::function<void(RecvCb)> onChannelMessage;   // RecvCb(channelId, senderId, payload, ts)
    };

    struct Config {
        // The createNode config: usually nodeConfig(appSettings, rln).dump().
        std::string nodeConfigJson;
        // RLN expected on the node (only used for the warning when adopting).
        bool rln = false;
        // SDS channels: off by default (plain relay; apps repair gaps themselves). Per-topic override.
        bool useChannels = false;
        std::function<bool(const std::string &topic)> channelFor;   // true: this topic uses a channel
        std::string senderId;                                      // SDS sender when senderFor is unset
        std::function<std::string(const std::string &topic)> senderFor;
        // Subscribe the receive handlers again once the node is ready: needed while delivery_module's
        // cpp-sdk predates logos-cpp-sdk #134 (the first subscription, made before the delivery host
        // listens, fails silently and nothing is ever received). Harmless otherwise.
        bool resubscribeAfterReady = true;
        // Peerless watchdog (tick): after this many seconds at 0 peers, stop/start the node to re-dial
        // the entry nodes (peer exchange can't recover from 0). Twice as long before the first connect,
        // three times as long on an adopted node. 0 = off.
        int peerlessSeconds = 30;
        int reconnectCooldownSeconds = 45;
        // Adopting a node another module created: if it isn't running (getConnectionStatus) after
        // this long, start it.
        int adoptStartAfterSeconds = 30;
        // Hand on only frames for topics joined through this transport (other apps share the node).
        // loam_core passes everything on, as it always did; apps filter.
        bool onlyJoinedTopics = true;
        // Register Ops::onChannelMessage even when useChannels is off (loam_core always did).
        bool alwaysListenChannels = false;
        // tick() polls Ops::metrics for the peer count. Off when the core polls metrics itself and
        // reports peers with notePeers() (loam_core); the adopt probe still uses Ops::metrics.
        bool pollMetrics = true;
    };

    // Callbacks the core sets.
    std::function<void(const Frame &)> onFrame;            // every message on a joined topic
    std::function<void()> onReady;                         // node up (also after a reconnect)
    std::function<void(const std::string &)> onError;      // node failed to start
    std::function<void(const std::string &)> onWarning;    // adopted node, RLN doubts, segmented drops

    DeliveryTransport(Ops ops, Config cfg) : m_ops(std::move(ops)), m_cfg(std::move(cfg)) {}

    // Create (or adopt) and start the node. Returns at once; onReady / onError tell the outcome.
    void start() {
        if (m_ready || m_starting) return;
        m_starting = true;
        m_handle = [this](const std::string &t, const std::string &s, const J &p, int64_t ts) { receive(t, s, p, ts); };
        subscribeReceive();                          // before createNode: the position a GUI host receives with
        if (m_ops.onNodeStarted && !m_startSubscribed) {
            m_startSubscribed = true;
            m_ops.onNodeStarted([this](bool ok, const std::string &msg) { onStarted(ok, msg); });
        }
        if (!m_ops.createNode) { m_starting = false; fail("delivery_module has no createNode"); return; }
        m_ops.createNode(m_cfg.nodeConfigJson, [this](bool ok, const std::string &err) {
            if (!ok && isAlreadyInitialized(err)) { adopt(); return; }
            if (!ok) { m_starting = false; fail("createNode: " + err); return; }
            startNode();
        });
    }

    // Listen on a content topic. Before the node is up the topic is remembered and joined later.
    void join(const std::string &topic) {
        m_topics.insert(topic);
        if (m_ready) doJoin(topic);
    }

    // Send the app's (sealed) bytes on a topic. Dropped while the node isn't up: Loam apps repair
    // gaps with their own catch-up, so a send is never queued here.
    bool send(const std::string &topic, const std::string &bytes) {
        if (!m_ready) return false;
        auto attempt = [&](int repr) -> bool {
            try {
                J p = encodeSend<J>(bytes, repr);
                if (channel(topic)) { if (!m_ops.channelSend) return false; m_ops.channelSend(topic, p, noop()); }
                else { if (!m_ops.sendRaw) return false; m_ops.sendRaw(topic, p, noop()); }
                return true;
            } catch (...) { return false; }
        };
        bool sent = false;
        if (m_repr && attempt(m_repr)) sent = true;
        else if (attempt(1)) { m_repr = 1; sent = true; }
        else if (attempt(2)) { m_repr = 2; sent = true; }
        if (sent) ++m_tx;
        else std::fprintf(stderr, "loam delivery: send failed, no payload shape works\n");
        return sent;
    }

    // Call every few seconds (loam_core: 3 s). Polls metrics for the peer count (when Ops::metrics
    // is set), runs the peerless watchdog and the adopt fallback.
    void tick(long long nowMs) {
        if (m_adopting && !m_ready) {
            if (m_adoptAtMs == 0) m_adoptAtMs = nowMs;
            probeAdopted();
            if (!m_adoptStarted && nowMs - m_adoptAtMs > (long long)m_cfg.adoptStartAfterSeconds * 1000) {
                m_adoptStarted = true;
                warn("the adopted delivery node isn't running after " + std::to_string(m_cfg.adoptStartAfterSeconds) + " s: starting it");
                startNode();
            }
            return;
        }
        if (!m_ready || !m_cfg.pollMetrics || !m_ops.metrics || m_metricsInFlight) return;
        m_metricsInFlight = true;
        m_ops.metrics([this, nowMs](bool ok, const std::string &text) {
            m_metricsInFlight = false;
            if (ok) notePeers(peersFromMetrics(text), nowMs);
        });
    }

    // Tell the watchdog the current peer count (loam_core polls metrics itself and calls this).
    void notePeers(long peers, long long nowMs) {
        if (peers < 0) return;
        m_peers = peers;
        if (peers > 0) { m_everConnected = true; m_zeroSinceMs = 0; return; }
        if (m_cfg.peerlessSeconds <= 0) return;
        if (m_zeroSinceMs == 0) { m_zeroSinceMs = nowMs; return; }
        // Also when it never had peers: a node started while offline (or while the entry nodes were
        // down) never dials them again on delivery < v0.40, and an Edge node has no connectivity loop
        // at all. The first connect gets twice the time. An ADOPTED node waits three times as long, so
        // its owner (if it runs this watchdog too) restarts it first and the two don't take turns.
        long long wait = (long long)m_cfg.peerlessSeconds * 1000;
        if (!m_everConnected) wait *= 2;
        if (m_adopted) wait *= 3;
        if (nowMs - m_zeroSinceMs >= wait &&
            (m_lastReconnectMs == 0 || nowMs - m_lastReconnectMs > (long long)m_cfg.reconnectCooldownSeconds * 1000)) {
            m_lastReconnectMs = nowMs; m_zeroSinceMs = 0;
            std::fprintf(stderr, "loam delivery: peerless %llds%s, re-dialing the entry nodes\n", wait / 1000,
                         m_adopted ? " (adopted node: restarting it for every module on it)" : "");
            reconnect();
        }
    }

    // Stop and start the node and join every topic again (re-dials the entry nodes).
    void reconnect() {
        // A restart that never reported back (the node was hung) blocks a new one for 60 s only.
        const long long now = steadyMs();
        if (!m_everReady || (m_reconnecting && now - m_reconnectAtMs < 60000)) return;
        m_reconnecting = true; m_reconnectAtMs = now; m_ready = false;
        auto again = [this]() { startNode(); };
        if (m_ops.stop) m_ops.stop([again](bool, const std::string &) { again(); });
        else again();
    }

    bool ready() const { return m_ready; }
    bool adopted() const { return m_adopted; }
    long peers() const { return m_peers; }
    // "starting" | "adopting" | "ready" | "reconnecting" | "error: …"
    std::string status() const {
        if (!m_error.empty() && !m_ready) return "error: " + m_error;
        if (m_ready) return "ready";
        if (m_reconnecting) return "reconnecting";
        if (m_adopting) return "adopting";
        return "starting";
    }
    std::string metricsJson() const {
        std::ostringstream o;
        o << "{\"ready\":" << (m_ready ? "true" : "false") << ",\"adopted\":" << (m_adopted ? "true" : "false")
          << ",\"peers\":" << m_peers << ",\"rx\":" << m_rx << ",\"tx\":" << m_tx
          << ",\"rxSegmented\":" << m_rxSegmented << ",\"wire\":" << LOAM_DELIVERY_WIRE_VERSION << "}";
        return o.str();
    }
    uint64_t rx() const { return m_rx; }
    uint64_t tx() const { return m_tx; }
    uint64_t rxSegmented() const { return m_rxSegmented; }

    static long peersFromMetrics(const std::string &text) {
        std::istringstream ms(text); std::string ln;
        while (std::getline(ms, ln)) {
            if (!ln.empty() && ln.back() == '\r') ln.pop_back();
            if (ln.rfind("libp2p_peers ", 0) == 0) {
                auto sp = ln.rfind(' ');
                try { return (long)std::stod(ln.substr(sp + 1)); } catch (...) { return -1; }
            }
        }
        return -1;
    }
    static bool isAlreadyInitialized(const std::string &err) {
        std::string e; for (char c : err) e.push_back((char)std::tolower((unsigned char)c));
        return e.find("already initialized") != std::string::npos || e.find("already initialised") != std::string::npos;
    }

private:
    static Cb noop() { return [](bool, const std::string &) {}; }
    static long long steadyMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    void fail(const std::string &msg) {
        m_error = msg;
        std::fprintf(stderr, "loam delivery: %s\n", msg.c_str()); std::fflush(stderr);
        if (onError) onError(msg);
    }
    void warn(const std::string &msg) {
        std::fprintf(stderr, "loam delivery: %s\n", msg.c_str()); std::fflush(stderr);
        if (onWarning) onWarning(msg);
    }
    void subscribeReceive() {
        if (m_ops.onMessage) m_ops.onMessage(m_handle);
        if (m_ops.onChannelMessage && (m_cfg.useChannels || m_cfg.alwaysListenChannels)) m_ops.onChannelMessage(m_handle);
    }
    // Another module created the node first (one node per Basecamp). Use it: its config is theirs.
    void adopt() {
        m_adopted = true; m_adopting = true; m_adoptStarted = false; m_adoptAtMs = 0;
        std::string w = "adopting the delivery node another module created; its network config is that module's";
        if (!m_cfg.rln) w += ". If it was created with the RLN-on \"logos.test\" preset and there is no membership, sends will fail"
                             " (set LOGOS_DELIVERY_RLN_PRESETS={\"logos.test\":{\"enabled\":false}} on dev machines)";
        warn(w);
        // It may already run (no nodeStarted will come): tick() probes with metrics. It may be started
        // by its owner soon (nodeStarted arrives). Or nobody starts it: tick() starts it after a while.
        m_awaitStart = true;
        probeAdopted();
    }
    // Is the adopted node running? Its connection status says so once it has peers.
    void probeAdopted() {
        if (!m_ops.connectionStatus || m_probeInFlight) return;
        m_probeInFlight = true;
        m_ops.connectionStatus([this](bool ok, const std::string &status) {
            m_probeInFlight = false;
            if (ok && isRunning(status) && m_adopting && !m_ready) becomeReady();
        });
    }
public:
    static bool isRunning(const std::string &connectionStatus) {
        std::string s; for (char c : connectionStatus) s.push_back((char)std::tolower((unsigned char)c));
        return s.find("connected") != std::string::npos && s.find("disconnected") == std::string::npos;
    }
private:
    void startNode() {
        if (!m_ops.start) { m_starting = false; fail("delivery_module has no start"); return; }
        m_awaitStart = (bool)m_ops.onNodeStarted;    // set BEFORE dispatch: the event may beat the reply
        m_ops.start([this](bool ok, const std::string &err) {
            if (!ok) {
                // A reconnect's start may only have timed out and still complete: keep waiting for nodeStarted.
                if (m_reconnecting && m_ops.onNodeStarted) { std::fprintf(stderr, "loam delivery: reconnect start: %s\n", err.c_str()); return; }
                // Adopting: the owner may have started it after all. Keep probing.
                if (m_adopting) { m_awaitStart = true; warn("starting the adopted node: " + err + " (still waiting for it)"); return; }
                m_awaitStart = false; m_starting = false; m_reconnecting = false;
                fail("start: " + err); return;
            }
            if (m_ops.onNodeStarted) return;          // the outcome comes as nodeStarted
            becomeReady();
        });
    }
    void onStarted(bool ok, const std::string &msg) {
        if (!m_awaitStart) return;
        m_awaitStart = false;
        if (!ok) { m_starting = false; m_reconnecting = false; fail("node failed to start: " + msg); return; }
        becomeReady();
    }
    void becomeReady() {
        const bool wasReady = m_everReady;
        m_ready = true; m_starting = false; m_reconnecting = false; m_adopting = false; m_everReady = true;
        m_awaitStart = false; m_error.clear();
        if (m_cfg.resubscribeAfterReady && !wasReady) subscribeReceive();
        for (const auto &t : m_topics) doJoin(t);
        if (onReady) onReady();
    }
    bool channel(const std::string &topic) const {
        return m_cfg.useChannels && (!m_cfg.channelFor || m_cfg.channelFor(topic));
    }
    void doJoin(const std::string &topic) {
        if (m_ops.subscribe) m_ops.subscribe(topic, noop());
        if (channel(topic) && m_ops.channelCreate)
            m_ops.channelCreate(topic, topic, m_cfg.senderFor ? m_cfg.senderFor(topic) : m_cfg.senderId, noop());
    }
    void receive(const std::string &topicArg, const std::string &senderArg, const J &payload, int64_t ts) {
        Frame f;
        switch (decodeReceived(topicArg, senderArg, payload, ts, f)) {
        case Decoded::Frame:
            if (m_cfg.onlyJoinedTopics && !m_topics.count(f.topic)) return;   // another app's topic on the shared node
            ++m_rx;
            if (onFrame) onFrame(f);
            return;
        case Decoded::Segmented:
            ++m_rxSegmented;
            if (m_rxSegmented == 1) warn("dropping multi-segment SDS messages (> 100 KiB): reassembly isn't supported");
            return;
        case Decoded::Empty:
            return;
        }
    }

    Ops m_ops; Config m_cfg;
    RecvCb m_handle;
    std::set<std::string> m_topics;
    bool m_ready = false, m_starting = false, m_reconnecting = false, m_everReady = false;
    bool m_awaitStart = false, m_startSubscribed = false;
    bool m_adopted = false, m_adopting = false, m_adoptStarted = false, m_probeInFlight = false;
    long long m_adoptAtMs = 0;
    bool m_metricsInFlight = false, m_everConnected = false;
    long long m_zeroSinceMs = 0, m_lastReconnectMs = 0, m_reconnectAtMs = 0;
    long m_peers = -1;
    int m_repr = 0;
    uint64_t m_rx = 0, m_tx = 0, m_rxSegmented = 0;
    std::string m_error;
};

} // namespace loam::delivery

// delivery_bearer.hpp — the delivery_module bearer for loam_core's MultiBearer.
// Wraps the host's delivery_module (SDS Reliable Channels over an embedded Logos node)
// as an IBearer. Logic is lifted from the proven scala/kym logos_transport.hpp:
// double-base64 channel framing, the array/string payload-representation probe, register
// receive handlers BEFORE createNode, one-startup-at-a-time. Templated ONLY on LogosMap
// (nlohmann::json, a complete SDK header); the generated delivery_module type arrives via
// the `Ops` std::functions built in the .cpp. See ADR 0015 + basecamp/README.md.
#pragma once
#include <set>
#include "multibearer.hpp"
#include <string>
#include <vector>
#include <functional>

namespace loam {

template <class LogosMap>
class DeliveryBearer : public IBearer {
public:
    using Cb = std::function<void(bool ok, const std::string &err)>;
    // topic/channelId, senderId (empty for raw relay), the raw payload, a timestamp.
    using RecvCb = std::function<void(const std::string &topic, const std::string &senderId,
                                      const LogosMap &payload, int64_t ts)>;
    // Thin wrappers around modules().delivery_module.*, built in loam_core_impl.cpp where
    // the generated type is complete. Adapt StdLogosResult → Cb(ok,err) there.
    struct Ops {
        std::function<void(const std::string &cfgJson, Cb)> createNode;
        std::function<void(Cb)> start;
        std::function<void(Cb)> stop;   // delivery_module.stop — used by reconnect() to re-dial
        std::function<void(const std::string &contentTopic, Cb)> subscribe;
        std::function<void(const std::string &channelId, const std::string &contentTopic,
                           const std::string &senderId, Cb)> channelCreate;
        std::function<void(const std::string &channelId, const LogosMap &payload, Cb)> channelSend;
        std::function<void(const std::string &contentTopic, const LogosMap &payload, Cb)> sendRaw;
        std::function<void(RecvCb)> onMessage;         // raw relay receive
        std::function<void(RecvCb)> onChannelMessage;  // SDS channel receive
    };
    struct Config {
        std::string deviceId;                // SDS senderId
        bool useChannels = true;             // SDS Reliable Channels (interops with mobile)
        bool hubMode = false;                // headless: delay createNode so handler IPC lands first
        // The FULL delivery createNode config (WakuNodeConf) as JSON, forwarded verbatim to
        // delivery.createNode — so the consuming app's shard/cluster/entryNodes/preset/mode all
        // pass through unchanged. loam_core fills defaults (mode, preset) before setting it.
        std::string nodeCfgJson;
    };
    using Delay = std::function<void(int ms, std::function<void()>)>;

    DeliveryBearer(Ops ops, Config cfg, Delay delay = {})
        : m_ops(std::move(ops)), m_cfg(std::move(cfg)), m_delay(std::move(delay)) { m_priority = 0; }

    const std::string &name() const override { static const std::string n = "delivery"; return n; }
    bool ready() const override { return m_nodeReady; }
    void setPeers(long p) override { m_peers = p; }
    // Fired once when the node finishes createNode+start (the readiness signal loam_core
    // turns into a statusChanged("Connected") event for apps — start() itself returns early).
    std::function<void()> onReady;

    void start() override {
        if (m_nodeReady || m_starting || m_reconnecting) return;
        m_starting = true;

        auto toWire = [](const LogosMap &v) -> std::string {
            if (v.is_string()) return v.template get<std::string>();
            if (v.is_array()) { std::string s; s.reserve(v.size());
                for (const auto &c : v) if (c.is_number_integer()) s.push_back((char)c.template get<int>());
                return s; }
            if (v.is_object() && v.contains("_bytes") && v["_bytes"].is_string())
                return b64::decode(v["_bytes"].template get<std::string>());
            return std::string();
        };
        auto handle = [this, toWire](const std::string &topic, const std::string &sender,
                                     const LogosMap &payload, int64_t ts) {
            std::string enc = toWire(payload);
            if (enc.empty() && payload.is_object() && payload.contains("payload")) enc = toWire(payload["payload"]);
            if (enc.empty()) return;
            std::string sealed, snd = sender, tpc = topic;
            // Both delivery receive events (message_received AND channel_message_received) hand us the
            // whole SDS Message protobuf as the payload (with the no-op Encrypt provider, the WakuMessage
            // payload == the SDS proto verbatim). SDS layout (source: logos-delivery sds/protobuf.nim):
            //   f1 messageId (keccak hex), f4 channelId (== our content topic), f5 content (the RAW app
            //   bytes — NOT base64), f7 original senderId.
            // We reconstruct EVERYTHING from the frame itself, because the event's own topic/sender args
            // are unreliable across the two event shapes (message_received puts the messageHash where
            // channel_message_received puts the channelId). So: derive the content topic from f4, the
            // payload from f5 (forwarded RAW — qaku_core peels the base64 depth, trying single AND
            // double; b64-decoding f5 here is the bug that silently dropped every frame), the sender
            // from f7. A frame is "ours" when f5 is present and f4 looks like a Waku content topic.
            const std::string f4 = sdsField(enc, 4);
            const std::string f5 = sdsField(enc, 5);
            const bool isSds = !f5.empty() && !f4.empty() && f4.rfind("/", 0) == 0;
            if (isSds) {
                tpc = f4;                                      // authoritative content topic
                sealed = f5;                                   // RAW field-5 content; app peels b64
                const std::string s7 = sdsField(enc, 7);
                if (!s7.empty()) snd = s7;
            } else {
                sealed = b64::decode(enc);                     // a non-SDS / already-peeled payload
            }
            // Nothing usable? Genuine foreign/undecodable relay noise — drop it. Forwarding empty
            // frames both delivers garbage AND floods `received` (a QRO event storm that wedges the hub).
            if (sealed.empty()) return;
            ++m_rx;
            if (onFrame) onFrame(tpc, snd, sealed, ts);
        };
        m_handle = handle;   // stored so we can RE-subscribe once the delivery host is listening (below)
        // Register receive handlers BEFORE createNode (the position a GUI host receives with).
        if (m_ops.onMessage)        m_ops.onMessage(handle);
        if (m_ops.onChannelMessage) m_ops.onChannelMessage(handle);

        // The node config is forwarded verbatim (loam_core already merged defaults).
        const std::string cfgStr = m_cfg.nodeCfgJson.empty()
            ? std::string("{\"mode\":\"Core\",\"preset\":\"logos.test\"}") : m_cfg.nodeCfgJson;

        auto startNode = [this, cfgStr]() {
            if (!m_ops.createNode) { m_starting = false; return; }
            m_ops.createNode(cfgStr, [this](bool ok, const std::string &err) {
                if (!ok) { m_starting = false; fprintf(stderr, "loam delivery createNode err: %s\n", err.c_str()); return; }
                if (!m_ops.start) { m_starting = false; return; }
                m_ops.start([this](bool ok2, const std::string &err2) {
                    if (!ok2) { m_starting = false; fprintf(stderr, "loam delivery start err: %s\n", err2.c_str()); return; }
                    m_nodeReady = true; m_starting = false;
                    // RE-SUBSCRIBE receive handlers now that the delivery host is LISTENING.
                    // The generated event-subscription (cpp-sdk pre-#134 9d508292e) does a blocking
                    // ensureReplica() at the FIRST subscribe — which runs before the delivery host
                    // calls listen() — and fails PERMANENTLY + silently, so `messageReceived` never
                    // delivers and rx stays 0. Subscribing again here, after the node is up, actually
                    // lands the callback. Remove once we're on a module-builder that carries #134.
                    if (m_ops.onMessage)        m_ops.onMessage(m_handle);
                    if (m_ops.onChannelMessage) m_ops.onChannelMessage(m_handle);
                    for (const auto &t : m_pendingTopics) doJoin(t);   // (re)join topics requested before ready
                    m_pendingTopics.clear();
                    if (onReady) onReady();                            // → loam_core emits "Connected"
                });
            });
        };
        if (m_cfg.hubMode && m_delay) m_delay(1500, startNode); else startNode();
    }

    void join(const std::string &topic) override {
        m_allTopics.insert(topic);   // remembered so reconnect() can re-join after a node restart
        if (!m_nodeReady) { m_pendingTopics.push_back(topic); return; }
        doJoin(topic);
    }

    // The node lost all peers and won't re-dial itself (no discovery). Restart it and re-join every
    // topic: stop → start (re-dials entryNodes) → doJoin all. Receive handlers were registered on the
    // module (not the node), so they survive; SDS channels are rebuilt by the re-channelCreate in doJoin.
    void reconnect() override {
        if (!m_nodeReady || m_reconnecting) return;
        m_reconnecting = true;
        fprintf(stderr, "loam delivery: node peerless — reconnecting (stop→start→rejoin %zu topics)\n", m_allTopics.size());
        fflush(stderr);
        auto doStart = [this]() {
            if (!m_ops.start) { m_reconnecting = false; return; }
            m_ops.start([this](bool ok, const std::string &err) {
                m_reconnecting = false;
                if (!ok) { fprintf(stderr, "loam reconnect start err: %s\n", err.c_str()); return; }
                m_nodeReady = true;
                for (const auto &t : m_allTopics) doJoin(t);
                if (onReady) onReady();
            });
        };
        m_nodeReady = false;
        if (m_ops.stop) m_ops.stop([doStart](bool, const std::string &) { doStart(); });
        else doStart();
    }

    void send(const std::string &topic, const std::string &sealedBytes) override {
        if (!m_nodeReady) return;
        const std::string enc = b64::encode(sealedBytes);   // inner layer; delivery adds the outer
        auto attempt = [&](int repr) -> bool {
            try {
                LogosMap p = (repr == 1) ? bytesPayload(enc) : LogosMap(enc);
                if (m_cfg.useChannels) { if (m_ops.channelSend) m_ops.channelSend(topic, p, noop()); }
                else                   { if (m_ops.sendRaw)     m_ops.sendRaw(topic, p, noop()); }
                return true;
            } catch (...) { return false; }
        };
        bool sent = false;
        if (m_sendRepr == 1 || m_sendRepr == 2) { if (attempt(m_sendRepr)) sent = true; else m_sendRepr = 0; }
        if (!sent && attempt(1)) { m_sendRepr = 1; sent = true; }
        else if (!sent && attempt(2)) { m_sendRepr = 2; sent = true; }
        if (sent) ++m_tx;
        else fprintf(stderr, "loam delivery send: no working payload representation\n");
    }

    std::string metricsJson() const override {
        std::string o = "{\"name\":\"delivery\",\"enabled\":";
        o += m_enabled ? "true" : "false";
        o += ",\"priority\":" + std::to_string(m_priority);
        o += ",\"ready\":" + std::string(m_nodeReady ? "true" : "false");
        o += ",\"peers\":" + std::to_string(m_peers);
        o += ",\"rx\":" + std::to_string((long long)m_rx);
        o += ",\"tx\":" + std::to_string((long long)m_tx) + "}";
        return o;
    }

private:
    static Cb noop() { return [](bool, const std::string &) {}; }
    // Extract one length-delimited (wire-type 2) field's bytes from a protobuf buffer.
    // A minimal, tolerant varint walk over unknown fields — used to peel the SDS Message
    // wrapper (see handle()): returns "" if the field is absent or the buffer is malformed.
    static std::string sdsField(const std::string &b, int want) {
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
            if (wt == 2) { uint64_t ln; if (!rv(ln)) break; if (i + ln > n) break;
                if (f == want) return b.substr(i, (size_t)ln); i += (size_t)ln; }
            else if (wt == 0) { uint64_t v; if (!rv(v)) break; }
            else if (wt == 5) { if (i + 4 > n) break; i += 4; }
            else if (wt == 1) { if (i + 8 > n) break; i += 8; }
            else break;
        }
        return std::string();
    }
    static LogosMap bytesPayload(const std::string &s) {
        LogosMap a = LogosMap::array();
        for (unsigned char c : s) a.push_back((unsigned)c);
        return a;
    }
    void doJoin(const std::string &topic) {
        if (m_cfg.useChannels) {
            if (m_ops.subscribe)     m_ops.subscribe(topic, noop());                       // recv service gate
            if (m_ops.channelCreate) m_ops.channelCreate(topic, topic, m_cfg.deviceId, noop());
        } else {
            if (m_ops.subscribe)     m_ops.subscribe(topic, noop());
        }
    }

    Ops m_ops; Config m_cfg; Delay m_delay;
    bool m_nodeReady = false, m_starting = false, m_reconnecting = false;
    int m_sendRepr = 0;      // 0 unprobed, 1 byte array, 2 string
    long m_peers = -1;
    std::vector<std::string> m_pendingTopics;
    std::set<std::string> m_allTopics;   // every topic ever joined — re-joined on reconnect()
    RecvCb m_handle;   // the receive callback; re-subscribed after the node is ready (cpp-sdk pre-#134 workaround)
};

} // namespace loam

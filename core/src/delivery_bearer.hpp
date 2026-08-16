// delivery_bearer.hpp — the delivery_module bearer for loam_core's MultiBearer.
// Wraps the host's delivery_module (SDS Reliable Channels over an embedded Logos node)
// as an IBearer. Logic is lifted from the proven scala/kym logos_transport.hpp:
// double-base64 channel framing, the array/string payload-representation probe, register
// receive handlers BEFORE createNode, one-startup-at-a-time. Templated ONLY on LogosMap
// (nlohmann::json, a complete SDK header); the generated delivery_module type arrives via
// the `Ops` std::functions built in the .cpp. See ADR 0015 + basecamp/README.md.
#pragma once
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
        if (m_nodeReady || m_starting) return;
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
            const std::string sealed = b64::decode(enc);   // one decode; the app peels the last layer
            // Raw relay (onMessage) also fires for the shard's SDS-framed relay traffic — a string that
            // isn't our base64 payload, so b64::decode yields nothing. Drop it: forwarding empty frames
            // both delivers garbage AND floods `received` (a QRO event storm that wedges the headless hub).
            if (sealed.empty()) return;
            ++m_rx;
            if (onFrame) onFrame(topic, sender, sealed, ts);
        };
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
                    for (const auto &t : m_pendingTopics) doJoin(t);   // (re)join topics requested before ready
                    m_pendingTopics.clear();
                    if (onReady) onReady();                            // → loam_core emits "Connected"
                });
            });
        };
        if (m_cfg.hubMode && m_delay) m_delay(1500, startNode); else startNode();
    }

    void join(const std::string &topic) override {
        if (!m_nodeReady) { m_pendingTopics.push_back(topic); return; }
        doJoin(topic);
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
    bool m_nodeReady = false, m_starting = false;
    int m_sendRepr = 0;      // 0 unprobed, 1 byte array, 2 string
    long m_peers = -1;
    std::vector<std::string> m_pendingTopics;
};

} // namespace loam

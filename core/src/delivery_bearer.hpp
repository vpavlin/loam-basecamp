// delivery_bearer.hpp — the delivery_module bearer for loam_core's MultiBearer: a thin IBearer
// adapter over loam::delivery::DeliveryTransport (include/loam/delivery_transport.hpp), which holds
// the wire, the network config and the node lifecycle. Apps that talk to delivery_module directly
// vendor the same header, so their wire is identical. See ADR 0015.
#pragma once
#include <string>
#include <functional>
#include "../include/loam/delivery_transport.hpp"
#include "multibearer.hpp"

namespace loam {

template <class LogosMap>
class DeliveryBearer : public IBearer {
public:
    using T = loam::delivery::DeliveryTransport<LogosMap>;
    using Cb = typename T::Cb;
    using RecvCb = typename T::RecvCb;
    using Ops = typename T::Ops;
    using Config = typename T::Config;

    DeliveryBearer(Ops ops, Config cfg) : m_t(std::move(ops), std::move(cfg)) {
        m_priority = 0;
        m_t.onFrame = [this](const loam::delivery::Frame &f) {
            ++m_rx;
            if (onFrame) onFrame(f.topic, f.sender, f.bytes, f.ts);
        };
        m_t.onReady = [this] { if (onReady) onReady(); };
        m_t.onError = [this](const std::string &e) { if (onError) onError(e); };
        m_t.onWarning = [this](const std::string &w) { if (onWarning) onWarning(w); };
    }

    // Fired when the node is up (also after a reconnect); loam_core turns it into "Connected".
    std::function<void()> onReady;
    std::function<void(const std::string &)> onError;
    std::function<void(const std::string &)> onWarning;

    const std::string &name() const override { static const std::string n = "delivery"; return n; }
    bool ready() const override { return m_t.ready(); }
    void setPeers(long p) override { m_peers = p; }
    void start() override { m_t.start(); }
    void join(const std::string &topic) override { m_t.join(topic); }
    void reconnect() override { m_t.reconnect(); }
    void send(const std::string &topic, const std::string &sealedBytes) override {
        if (m_t.send(topic, sealedBytes)) ++m_tx;
    }
    // loam_core polls metrics itself (with its stall detection) and reports peers here.
    void notePeers(long peers, long long nowMs) { m_peers = peers; m_t.notePeers(peers, nowMs); }
    void tick(long long nowMs) { m_t.tick(nowMs); }
    bool adopted() const { return m_t.adopted(); }

    std::string metricsJson() const override {
        std::string o = "{\"name\":\"delivery\",\"enabled\":";
        o += m_enabled ? "true" : "false";
        o += ",\"priority\":" + std::to_string(m_priority);
        o += ",\"ready\":" + std::string(m_t.ready() ? "true" : "false");
        o += ",\"adopted\":" + std::string(m_t.adopted() ? "true" : "false");
        o += ",\"peers\":" + std::to_string(m_peers);
        o += ",\"rx\":" + std::to_string((long long)m_rx);
        o += ",\"tx\":" + std::to_string((long long)m_tx);
        o += ",\"rxSegmented\":" + std::to_string((long long)m_t.rxSegmented()) + "}";
        return o;
    }

private:
    T m_t;
    long m_peers = -1;
};

} // namespace loam

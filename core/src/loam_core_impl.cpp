// loam_core_impl.cpp — the facade wiring. Builds the delivery bearer's Ops from
// modules().delivery_module.* (where the generated type is complete), funnels receives
// through the MultiBearer dedup into the `received` event, and polls delivery metrics.
#include "loam_core_impl.h"
#include "logos_sdk.h"          // umbrella: complete LogosModules + LogosMap (nlohmann::json)
#include "delivery_bearer.hpp"  // DeliveryBearer<LogosMap> (needs LogosMap → after logos_sdk.h)
#include "ble_module_bearer.hpp"// BleModuleBearer — fronts the ble_mesh module (Ops bound below)
#include "logos_result.h"      // StdLogosResult {success, value, error}
#include <QTimer>
#include <sstream>

using DB = loam::DeliveryBearer<LogosMap>;

LoamCoreImpl::~LoamCoreImpl() {
    if (m_metricsTimer) { m_metricsTimer->stop(); m_metricsTimer->deleteLater(); m_metricsTimer = nullptr; }
}

void LoamCoreImpl::setStatus(const std::string& s) { m_status = s; statusChanged(s); }

void LoamCoreImpl::onContextReady() {
    // Receive path: MultiBearer dedups, then we emit `received` (b64 for IPC safety).
    m_bearers.onReceived = [this](const std::string& topic, const std::string& sender,
                                  const std::string& sealed, int64_t ts) {
        const std::string b64 = loam::b64::encode(sealed);
        received(topic, sender, b64, ts);          // the QRO event (for GUI/app cores)
        // ALSO tee into a drainable ring, so a headless collector can pull frames via
        // recentReceived() even where the event stream isn't observable (loam-telemetry).
        std::lock_guard<std::recursive_mutex> lk(m_mtx);
        LogosMap e; e["topic"] = topic; e["sender"] = sender; e["payloadB64"] = b64; e["ts"] = ts;
        m_rxLog.push_back(e.dump());
        while (m_rxLog.size() > 256) m_rxLog.pop_front();
    };
    // Peer-count poll on this module's thread (getNodeInfo is async; never blocks).
    m_metricsTimer = new QTimer();
    QObject::connect(m_metricsTimer, &QTimer::timeout, m_metricsTimer, [this] { refreshMetrics(); });
    m_metricsTimer->start(3000);
    setStatus("Ready");
}

// Build the delivery bearer once, from the (optional) cfg overrides + senderId/mode.
void LoamCoreImpl::ensureBearers(const std::string& cfgJson) {
    if (m_built) return;
    DB::Ops ops;
    ops.createNode = [this](const std::string& cfg, DB::Cb cb) {
        modules().delivery_module.createNodeAsync(cfg, [cb](StdLogosResult r) { cb(r.success, r.error); });
    };
    ops.start = [this](DB::Cb cb) {
        modules().delivery_module.startAsync([cb](StdLogosResult r) { cb(r.success, r.error); });
    };
    ops.subscribe = [this](const std::string& t, DB::Cb cb) {
        modules().delivery_module.subscribeAsync(t, [cb](StdLogosResult r) { cb(r.success, r.error); });
    };
    ops.channelCreate = [this](const std::string& id, const std::string& ct, const std::string& sender, DB::Cb cb) {
        modules().delivery_module.channelCreateAsync(id, ct, sender, [cb](StdLogosResult r) { cb(r.success, r.error); });
    };
    ops.channelSend = [this](const std::string& id, const LogosMap& p, DB::Cb cb) {
        modules().delivery_module.channelSendAsync(id, p, [cb](StdLogosResult r) { cb(r.success, r.error); });
    };
    ops.sendRaw = [this](const std::string& t, const LogosMap& p, DB::Cb cb) {
        modules().delivery_module.sendAsync(t, p, [cb](StdLogosResult r) { cb(r.success, r.error); });
    };
    ops.onMessage = [this](DB::RecvCb rcb) {
        modules().delivery_module.onMessageReceived(
            [rcb](const std::string&, const std::string& topic, const LogosMap& payload, int64_t ts) {
                rcb(topic, "", payload, ts);
            });
    };
    ops.onChannelMessage = [this](DB::RecvCb rcb) {
        modules().delivery_module.onChannelMessageReceived(
            [rcb](const std::string& channelId, const std::string& sender, const LogosMap& payload, int64_t ts) {
                rcb(channelId, sender, payload, ts);
            });
    };

    DB::Config cfg;
    cfg.deviceId = m_senderId;
    // Parse the app's cfg. loam-only keys (useChannels, hubMode) are pulled OUT into Config;
    // everything else is the delivery node config (WakuNodeConf) forwarded verbatim to
    // createNode — so the app's shard/cluster/entryNodes/preset/mode all pass through.
    LogosMap j = cfgJson.empty() ? LogosMap::object() : LogosMap::parse(cfgJson, nullptr, false);
    if (!j.is_object()) j = LogosMap::object();
    if (j.contains("hubMode")     && j["hubMode"].is_boolean())     cfg.hubMode     = j["hubMode"].get<bool>();
    // useChannels: true → SDS Reliable Channels (mobile parity, default); false → raw relay
    // (shipping kym's current wire). Keeps loam_core a drop-in — no silent mode flip.
    if (j.contains("useChannels") && j["useChannels"].is_boolean()) cfg.useChannels = j["useChannels"].get<bool>();
    j.erase("hubMode"); j.erase("useChannels");                    // not delivery-node keys
    if (!j.contains("mode"))   j["mode"]   = m_mode;               // default node mode
    if (!j.contains("preset")) j["preset"] = "logos.test";        // cluster-2 (ADR 0008)
    cfg.nodeCfgJson = j.dump();

    auto db = std::make_unique<DB>(ops, cfg);
    // Turn the bearer's readiness into a statusChanged("Connected") event: start() returns
    // early (async node bringup), so apps learn the node is up by subscribing to statusChanged,
    // not from the start() callback.
    db->onReady = [this] { setStatus("Connected"); };
    m_delivery = db.get();
    m_bearers.add(std::move(db));

    // Second bearer: the ble_mesh module (flood-gossip over Bluetooth). The MultiBearer fans each
    // write to it too and dedups its frames against the Waku copy by frameId. 0 peers until the
    // ble_mesh Qt Bluetooth radio (Phase 3), so it's inert but present in metrics/control today.
    loam::BleModuleBearer::Ops bops;
    bops.start = [this] { modules().ble_mesh.startAsync([](std::string) {}); };
    bops.flood = [this](const std::string& topic, const std::string& payloadB64) {
      modules().ble_mesh.floodAsync(topic, payloadB64, [](std::string) {});
    };
    bops.onFrame = [this](loam::BleModuleBearer::FrameRecvCb cb) {
      modules().ble_mesh.onFrameReceived(
        [cb](const std::string& topic, const std::string& sender, const std::string& payloadB64, int64_t ts) {
          cb(topic, sender, payloadB64, ts);
        });
    };
    m_bearers.add(std::make_unique<loam::BleModuleBearer>(bops));

    m_built = true;
}

std::string LoamCoreImpl::start(std::string cfgJson) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    ensureBearers(cfgJson);
    m_started = true;
    setStatus("Connecting...");
    m_bearers.startAll();
    return "";
}

std::string LoamCoreImpl::stop() {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    m_started = false;
    setStatus("Stopped");
    return "";   // delivery node teardown is a Phase-2 concern; bearers idle when unused
}

std::string LoamCoreImpl::setSenderId(std::string id) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (!id.empty()) m_senderId = id;
    return "";   // call before start(); the senderId is baked into the reliable channel
}

std::string LoamCoreImpl::join(std::string topic) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (topic.empty()) return "empty topic";
    ensureBearers("");
    m_bearers.joinAll(topic);
    return "";
}

std::string LoamCoreImpl::sendSealed(std::string topic, std::string sealedB64) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (topic.empty()) return "empty topic";
    m_bearers.sendSealed(topic, loam::b64::decode(sealedB64));
    return "";
}

std::string LoamCoreImpl::setBearerEnabled(std::string name, std::string on) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    auto* b = m_bearers.byName(name);
    if (!b) return "unknown bearer: " + name;
    b->setEnabled(on == "1" || on == "true");
    if (m_built) metricsChanged(m_bearers.metricsJson());
    return "";
}

std::string LoamCoreImpl::setBearerPriority(std::string orderCsv) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    std::vector<std::string> order; std::string cur; std::istringstream ss(orderCsv);
    while (std::getline(ss, cur, ',')) {
        size_t a = cur.find_first_not_of(" \t"), z = cur.find_last_not_of(" \t");
        if (a != std::string::npos) order.push_back(cur.substr(a, z - a + 1));
    }
    m_bearers.setPriorityOrder(order);
    if (m_built) metricsChanged(m_bearers.metricsJson());
    return "";
}

std::string LoamCoreImpl::forceMesh(std::string on) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    m_forceMesh = (on == "1" || on == "true");
    return "";   // no-op until the ble_mesh bearer exists (Phase 4); stored for then
}

std::string LoamCoreImpl::setNodeMode(std::string mode) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    m_mode = (mode == "Edge") ? "Edge" : "Core";
    return m_built ? "applied on next start" : "";
}

std::string LoamCoreImpl::metricsJson() {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (!m_built) return "{\"bearers\":[],\"peers\":-1,\"connected\":false}";
    return m_bearers.metricsJson();
}

// Drain the receive ring: returns [{topic,sender,payloadB64,ts}, …] and clears it. A headless
// collector polls this (a plain `call`, unlike the `received` event) — e.g. loam-telemetry capture.
std::string LoamCoreImpl::recentReceived() {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    LogosMap arr = LogosMap::array();
    for (const auto& s : m_rxLog) { try { arr.push_back(LogosMap::parse(s)); } catch (...) {} }
    m_rxLog.clear();
    return arr.dump();
}

std::string LoamCoreImpl::status() {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    return m_status;
}

void LoamCoreImpl::refreshMetrics() {
    if (!m_built || !m_delivery || !m_delivery->ready()) return;
    modules().delivery_module.getNodeInfoAsync("Metrics", [this](StdLogosResult r) {
        std::lock_guard<std::recursive_mutex> lk(m_mtx);
        if (!r.success) return;
        const std::string metrics = r.value.is_string() ? r.value.get<std::string>() : r.value.dump();
        long peers = -1;
        std::istringstream ms(metrics); std::string ln;
        while (std::getline(ms, ln)) {
            if (!ln.empty() && ln.back() == '\r') ln.pop_back();
            if (ln.rfind("libp2p_peers ", 0) == 0) {
                auto sp = ln.rfind(' ');
                if (sp != std::string::npos) try { peers = (long)std::stod(ln.substr(sp + 1)); } catch (...) {}
            }
        }
        if (peers >= 0) { m_delivery->setPeers(peers); m_bearers.overallPeers = peers; }
        metricsChanged(m_bearers.metricsJson());
    });
}

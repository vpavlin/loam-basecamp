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
#include "loam_identity.hpp"    // loam ADR 0004 identity service (crypto + key/binding store)

using DB = loam::DeliveryBearer<LogosMap>;

LoamCoreImpl::~LoamCoreImpl() {
    if (m_metricsTimer) { m_metricsTimer->stop(); m_metricsTimer->deleteLater(); m_metricsTimer = nullptr; }
    delete m_idStore; m_idStore = nullptr;
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

// ── identity service (loam ADR 0004) ─────────────────────────────────────────
// Apps sign through loam; keys never leave. All JSON in/out (universal authoring). The identity UI
// lives in each app's view and drives these over module IPC.
loamid::IdentityStore* LoamCoreImpl::idStore() {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (!m_idStore) { m_idStore = new loamid::IdentityStore(); m_idStore->load(); }
    return m_idStore;
}
std::string LoamCoreImpl::listIdentities() { std::lock_guard<std::recursive_mutex> lk(m_mtx); return idStore()->listIdentities().dump(); }
std::string LoamCoreImpl::addSoftIdentity(std::string label) { std::lock_guard<std::recursive_mutex> lk(m_mtx); return idStore()->addSoftIdentity(label).dump(); }
std::string LoamCoreImpl::renameSoftIdentity(std::string id, std::string label) { std::lock_guard<std::recursive_mutex> lk(m_mtx); idStore()->renameSoftIdentity(id, label); return "{\"ok\":true}"; }
std::string LoamCoreImpl::removeSoftIdentity(std::string id) { std::lock_guard<std::recursive_mutex> lk(m_mtx); idStore()->removeSoftIdentity(id); return "{\"ok\":true}"; }
std::string LoamCoreImpl::getDefaultIdentityId() { std::lock_guard<std::recursive_mutex> lk(m_mtx); return nlohmann::json(idStore()->getDefaultIdentityId()).dump(); }
std::string LoamCoreImpl::setDefaultIdentityId(std::string id) { std::lock_guard<std::recursive_mutex> lk(m_mtx); idStore()->setDefaultIdentityId(id); return "{\"ok\":true}"; }
std::string LoamCoreImpl::bindingFor(std::string containerId) { std::lock_guard<std::recursive_mutex> lk(m_mtx); std::string b = idStore()->bindingFor(containerId); return b.empty() ? std::string("null") : nlohmann::json(b).dump(); }
std::string LoamCoreImpl::bindContainer(std::string containerId, std::string identityId) { std::lock_guard<std::recursive_mutex> lk(m_mtx); idStore()->bindContainer(containerId, identityId); return "{\"ok\":true}"; }
std::string LoamCoreImpl::identityForContainer(std::string containerId) { std::lock_guard<std::recursive_mutex> lk(m_mtx); return idStore()->identityForContainer(containerId).dump(); }
std::string LoamCoreImpl::signDigest(std::string containerId, std::string digestHex) { std::lock_guard<std::recursive_mutex> lk(m_mtx); return idStore()->signDigest(containerId, digestHex).dump(); }
std::string LoamCoreImpl::removeKeycardIdentity(std::string id) { std::lock_guard<std::recursive_mutex> lk(m_mtx); idStore()->removeKeycardIdentity(id); return "{\"ok\":true}"; }

// ── keycard identity delegation (scala ADR 0016) ──────────────────────────────
// A keycard identity's private key lives on the card; loam delegates signing to Alisher's keycard
// module (a declared dependency): requestSign at the domain's 1582' non-exportable path (== mobile's
// domainToSignPath) → keycard-ui shows the approval panel (PIN + tap) → checkSignStatus returns a
// 65-byte R‖S‖V once (one-read-and-drop). We NEVER block: requestSignAsync + a self-rescheduling
// checkSignStatusAsync poll, result delivered on the keycardSignResult event (like refreshMetrics).
struct KcPending {
    std::string mode;      // "sign" | "enroll"
    std::string ref;       // caller-chosen correlation id (echoed on keycardSignResult)
    std::string domain;    // Keycard sign domain (e.g. "scala")
    std::string digestHex; // 32-byte hex payload actually signed
    std::string pubHex;    // (sign) expected card pubkey — guards against a wrong card
    std::string address;   // (sign) expected card address
    std::string label;     // (enroll) label for the new identity
    std::vector<unsigned char> enrollDigest;  // (enroll) raw digest, for pubkey recovery
    std::string signId;    // filled once requestSign returns
    int polls = 0;
};

void LoamCoreImpl::startKeycardSign(std::shared_ptr<KcPending> p) {
    nlohmann::json args{{"domain", p->domain}, {"payloadHash", p->digestHex},
                        {"caller", "loam"}, {"scheme", "ecdsa"}};
    modules().keycard.requestSignAsync(args.dump(), [this, p](std::string res) {
        nlohmann::json r = nlohmann::json::parse(res, nullptr, false);
        if (!r.is_object() || !r.contains("signId")) {
            keycardSignResult(p->ref, nlohmann::json{{"error", "keycard requestSign failed"}, {"detail", res}}.dump());
            return;
        }
        p->signId = r["signId"].get<std::string>();
        pollKeycard(p);
    });
}

void LoamCoreImpl::pollKeycard(std::shared_ptr<KcPending> p) {
    modules().keycard.checkSignStatusAsync(p->signId, [this, p](std::string res) {
        nlohmann::json r = nlohmann::json::parse(res, nullptr, false);
        const std::string status = r.is_object() ? r.value("status", std::string()) : std::string();
        if (status == "complete" && r.contains("signature")) { finalizeKeycard(p, r["signature"].get<std::string>()); return; }
        if (status == "failed" || (r.is_object() && r.contains("error"))) {
            // {error:"Sign request not found"} (expired) or an on-card failure — stop.
            keycardSignResult(p->ref, nlohmann::json{{"error", r.value("error", status.empty() ? std::string("keycard sign failed") : status)}}.dump());
            return;
        }
        // still pending (incl. wrong-PIN retries, which stay pending) — poll again, with a cap.
        if (++p->polls > 180) { keycardSignResult(p->ref, nlohmann::json{{"error", "keycard sign timeout"}}.dump()); return; }
        QTimer::singleShot(1000, [this, p] { pollKeycard(p); });
    });
}

void LoamCoreImpl::finalizeKeycard(std::shared_ptr<KcPending> p, const std::string& sigHex) {
    std::vector<unsigned char> sig = loamid::fromHexB(sigHex);
    std::vector<unsigned char> compact = loamid::compact64LowS(sig);   // 65B R‖S‖V → 64B low-S r‖s
    if (compact.size() != 64) { keycardSignResult(p->ref, nlohmann::json{{"error", "keycard bad signature"}}.dump()); return; }

    if (p->mode == "enroll") {
        // Two ways the module can hand us the card's public key:
        //  1) it returned the FULL card sign response (BER-TLV, >65B) → the pubkey is embedded as an
        //     uncompressed point; take it verbatim (no recovery math, no recovery-id ambiguity).
        //  2) it returned a bare 65B R‖S‖V recoverable sig → recover the pubkey via the recovery id.
        loamid::SignId rec;
        if (sig.size() > 65) rec = loamid::pubFromUncompressedBlob(sig);   // full-response path
        if (!rec.ok) rec = loamid::ecdsaRecover(p->enrollDigest, sig);     // recoverable-sig path
        if (!rec.ok) {
            // Surface the exact bytes so a format mismatch (64B r‖s with no recovery id, DER, …) is
            // diagnosable from one tap instead of guessing. The signed payload is a public enrol
            // digest, so the signature is not sensitive.
            keycardSignResult(p->ref, nlohmann::json{{"error", "keycard pubkey recovery failed"},
                {"siglen", (int)sig.size()}, {"sighex", loamid::toHexS(sig.data(), sig.size())}}.dump());
            return;
        }
        nlohmann::json meta; { std::lock_guard<std::recursive_mutex> lk(m_mtx); meta = idStore()->addKeycardIdentity(p->label, p->domain, rec.address, rec.pubHex); }
        keycardSignResult(p->ref, meta.dump());
        return;
    }
    // sign: guard that the signing card IS the enrolled identity (compare its pubkey), then return.
    // The card's pubkey comes either embedded in a full response (>65B) or by recovering from a
    // 65B R‖S‖V; either way, a mismatch means the wrong card tapped.
    if (!p->pubHex.empty()) {
        loamid::SignId rec;
        if (sig.size() > 65) rec = loamid::pubFromUncompressedBlob(sig);
        if (!rec.ok && sig.size() == 65) rec = loamid::ecdsaRecover(loamid::fromHexB(p->digestHex), sig);
        if (rec.ok && rec.pubHex != p->pubHex) {
            keycardSignResult(p->ref, nlohmann::json{{"error", "wrong card — signed by " + rec.address + ", expected " + p->address}}.dump());
            return;
        }
    }
    keycardSignResult(p->ref, nlohmann::json{{"sig", loamid::toHexS(compact.data(), 64)}, {"pub", p->pubHex}, {"address", p->address}}.dump());
}

std::string LoamCoreImpl::enrollKeycard(std::string label, std::string domain, std::string ref) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (domain.empty()) return "{\"error\":\"domain required\"}";
    auto p = std::make_shared<KcPending>();
    p->mode = "enroll"; p->ref = ref; p->domain = domain; p->label = label.empty() ? domain : label;
    p->enrollDigest = loamid::sha256b(loamid::strBytes("loam-keycard-enroll-v1:" + domain));
    p->digestHex = loamid::toHexS(p->enrollDigest.data(), 32);
    startKeycardSign(p);
    return nlohmann::json{{"status", "pending"}, {"ref", ref}}.dump();
}

std::string LoamCoreImpl::keycardSign(std::string containerId, std::string digestHex, std::string ref) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    nlohmann::json meta = idStore()->identityForContainer(containerId);
    if (!meta.is_object() || meta.value("kind", std::string()) != "keycard")
        return "{\"error\":\"container is not bound to a keycard identity\"}";
    if (digestHex.size() != 64) return "{\"error\":\"digest must be 32 bytes hex\"}";
    auto p = std::make_shared<KcPending>();
    p->mode = "sign"; p->ref = ref; p->domain = meta.value("domain", std::string());
    p->digestHex = digestHex; p->pubHex = meta.value("pubHex", std::string());
    p->address = meta.value("address", std::string());
    startKeycardSign(p);
    return nlohmann::json{{"status", "pending"}, {"ref", ref}}.dump();
}

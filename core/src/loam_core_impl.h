#pragma once
#include <string>
#include <memory>
#include <mutex>
#include <deque>

class QTimer;
struct KcPending;                            // a pending keycard sign/enrol op — defined in the .cpp
namespace loamid { class IdentityStore; }   // loam ADR 0004 identity service — defined in loam_identity.hpp (.cpp only)
#include "logos_module_context.h"   // LogosModuleContext base + logos_events: + modules()
#include "multibearer.hpp"          // IBearer + MultiBearer (std::string only, no LogosMap)
// NOTE: the generated modules() type + LogosMap live in the umbrella "logos_sdk.h", which
// is included only in the .cpp (it can't appear in this generator-read header). The delivery
// bearer is therefore held as the non-templated IBearer* base and built in the .cpp.

/**
 * LoamCoreImpl — the desktop Loam transport FACADE as a Logos CORE module (ADR 0015).
 *
 * Apps depend ONLY on loam_core and program against this one stable, bearer-agnostic
 * API: start()/join()/sendSealed() to move OPAQUE sealed bytes, `received` to consume
 * them. Internally it holds a MultiBearer that fans each write to every bearer and
 * dedups receives by frameId — so a write arriving over Waku AND BLE folds once. Today
 * the only bearer is `delivery` (delivery_module); ble_mesh / lora slot in behind the
 * same facade with ZERO app edits.
 *
 * It also exposes a control + metrics surface (setBearerEnabled/priority, forceMesh,
 * setNodeMode, metricsJson) that the loam_ui QML view drives — generalising the Android
 * Loam app's controls to N bearers.
 *
 * Universal authoring: public methods are the API (JSON-serializable std::string), no
 * Q_OBJECT/QML; the plugin glue is generated from this header. Keep method declaration
 * lines free of trailing // comments — the glue generator skips any method with one.
 */
class LoamCoreImpl : public LogosModuleContext {
public:
    ~LoamCoreImpl() override;

    // --- transport API (apps call these) ---
    std::string start(std::string cfgJson);
    std::string stop();
    std::string setSenderId(std::string id);
    std::string join(std::string topic);
    std::string sendSealed(std::string topic, std::string sealedB64);

    // --- control API (loam_ui drives these) ---
    std::string setBearerEnabled(std::string name, std::string on);
    std::string setBearerPriority(std::string orderCsv);
    std::string forceMesh(std::string on);
    std::string setNodeMode(std::string mode);

    // --- identity API (loam ADR 0004) — apps sign through loam; the identity UI stays in each app.
    // Keys never leave loam. Returns are JSON strings (a meta = {id,kind,label,address,pubHex}).
    std::string listIdentities();
    std::string addSoftIdentity(std::string label);
    std::string renameSoftIdentity(std::string id, std::string label);
    std::string removeSoftIdentity(std::string id);
    std::string getDefaultIdentityId();
    std::string setDefaultIdentityId(std::string id);
    std::string bindingFor(std::string containerId);
    std::string bindContainer(std::string containerId, std::string identityId);
    std::string identityForContainer(std::string containerId);
    // Sign a 32-byte hex digest with the container's bound identity → {sig,pub,address} | {error}.
    // For a device/soft identity this returns the signature synchronously; for a KEYCARD identity it
    // returns {status:"keycard",...} and the app must instead call keycardSign (async, card tap).
    std::string signDigest(std::string containerId, std::string digestHex);

    // --- keycard identity (loam ADR 0004 + scala ADR 0016) — async, card tap + PIN via keycard-ui.
    // Enrol a physical Keycard as a loam identity: signs an enrol digest at the domain's 1582' path,
    // recovers the card's public address (key never leaves the card), and records it. `domain` MUST
    // match the mobile sign domain (e.g. "scala") so one card is one identity across phone + desktop.
    // Returns {status:"pending",ref} immediately; the result arrives on keycardSignResult(ref, …).
    std::string enrollKeycard(std::string label, std::string domain, std::string ref);
    // Sign a 32-byte hex digest with a keycard-bound container's card (requestSign at 1582' + poll).
    // Returns {status:"pending",ref}; the {sig,pub,address}|{error} arrives on keycardSignResult(ref).
    std::string keycardSign(std::string containerId, std::string digestHex, std::string ref);
    std::string removeKeycardIdentity(std::string id);

    // --- metrics API (loam_ui polls these) ---
    std::string metricsJson();
    std::string status();
    // Pull-based receive log: drains + returns recently received frames as a JSON array of
    // {topic, sender, payloadB64, ts}. Complements the `received` EVENT for headless collectors
    // (the hub can `call` this even where it can't stream the event) — e.g. loam-telemetry capture.
    std::string recentReceived();

protected:
    void onContextReady() override;

logos_events:
    // A received frame, once-decoded and re-base64'd for IPC safety. App cores subscribe
    // via modules().loam_core.onReceived(...) and open the bytes with their own key.
    void received(const std::string& topic, const std::string& senderId,
                  const std::string& payloadB64, int64_t ts);
    void metricsChanged(const std::string& metricsJson);
    void statusChanged(const std::string& status);
    // Result of an async enrollKeycard / keycardSign, matched by the caller-chosen `ref`.
    // resultJson = {sig,pub,address} | (enrol) {id,kind,label,address,pubHex,domain} | {error}.
    void keycardSignResult(const std::string& ref, const std::string& resultJson);

private:
    void ensureBearers(const std::string& cfgJson);   // build the delivery bearer (once)
    loamid::IdentityStore* idStore();                 // lazy-init the identity service on first use
    void setStatus(const std::string& s);
    void refreshMetrics();         // async getNodeInfo("Metrics") → peer count → metricsChanged
    void startKeycardSign(std::shared_ptr<KcPending> p);   // requestSignAsync → begin polling
    void pollKeycard(std::shared_ptr<KcPending> p);        // checkSignStatusAsync, self-reschedule
    void finalizeKeycard(std::shared_ptr<KcPending> p, const std::string& sigHex);

    loam::MultiBearer m_bearers;
    loam::IBearer* m_delivery = nullptr;   // the delivery bearer, owned by m_bearers
    loamid::IdentityStore* m_idStore = nullptr; // loam ADR 0004 identity service (lazy-init; new/delete in .cpp)
    bool m_built = false, m_started = false, m_forceMesh = false;
    std::string m_senderId = "loam-core";
    std::string m_mode = "Core";     // delivery bearer node mode (Core|Edge)
    std::string m_status = "Starting...";
    std::recursive_mutex m_mtx;
    QTimer* m_metricsTimer = nullptr;
    std::deque<std::string> m_rxLog;   // recent received frames (JSON strings); drained by recentReceived()
};

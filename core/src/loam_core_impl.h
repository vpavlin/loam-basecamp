#pragma once
#include <string>
#include <memory>
#include <mutex>

class QTimer;
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

    // --- metrics API (loam_ui polls these) ---
    std::string metricsJson();
    std::string status();

protected:
    void onContextReady() override;

logos_events:
    // A received frame, once-decoded and re-base64'd for IPC safety. App cores subscribe
    // via modules().loam_core.onReceived(...) and open the bytes with their own key.
    void received(const std::string& topic, const std::string& senderId,
                  const std::string& payloadB64, int64_t ts);
    void metricsChanged(const std::string& metricsJson);
    void statusChanged(const std::string& status);

private:
    void ensureBearers(const std::string& cfgJson);   // build the delivery bearer (once)
    void setStatus(const std::string& s);
    void refreshMetrics();         // async getNodeInfo("Metrics") → peer count → metricsChanged

    loam::MultiBearer m_bearers;
    loam::IBearer* m_delivery = nullptr;   // the delivery bearer, owned by m_bearers
    bool m_built = false, m_started = false, m_forceMesh = false;
    std::string m_senderId = "loam-core";
    std::string m_mode = "Core";     // delivery bearer node mode (Core|Edge)
    std::string m_status = "Starting...";
    std::recursive_mutex m_mtx;
    QTimer* m_metricsTimer = nullptr;
};

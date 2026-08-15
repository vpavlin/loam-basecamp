// ble_module_bearer.hpp — a loam_core bearer that fronts the ble_mesh MODULE (flood-gossip over
// Bluetooth). Like DeliveryBearer, the ble_mesh calls arrive as an `Ops` struct built in the .cpp
// (the generated modules().ble_mesh type can't be named in a header). loam_core's MultiBearer fans
// each sealed write to this bearer too; a frame over Waku and the same over BLE dedup by frameId
// (identical hash — mesh.hpp's frameId == loam_core's == bearer.ts's). See ADR 0015.
#pragma once
#include "multibearer.hpp"
#include <string>
#include <functional>

namespace loam {

class BleModuleBearer : public IBearer {
public:
  // A frame off the mesh: topic, senderId (empty), payload as base64-of-raw-sealed, ts.
  using FrameRecvCb = std::function<void(const std::string& topic, const std::string& senderId,
                                         const std::string& payloadB64, int64_t ts)>;
  struct Ops {
    std::function<void()> start;                                                  // ble_mesh.start (fire-and-forget)
    std::function<void(const std::string& topic, const std::string& payloadB64)> flood;   // ble_mesh.flood
    std::function<void(FrameRecvCb)> onFrame;                                     // subscribe frameReceived
  };
  explicit BleModuleBearer(Ops ops) : m_ops(std::move(ops)) { m_priority = 1; }   // fanned after delivery by default

  const std::string& name() const override { static const std::string n = "ble"; return n; }
  bool ready() const override { return m_started; }
  void setPeers(long p) override { m_peers = p; }

  void start() override {
    if (m_started) return;
    if (m_ops.onFrame) m_ops.onFrame([this](const std::string& topic, const std::string& sender,
                                            const std::string& payloadB64, int64_t ts) {
      // ble_mesh hands us base64-of-raw-sealed; decode to raw and funnel up so the MultiBearer
      // dedups it against the Waku copy by the SAME frameId(topic, rawSealed).
      ++m_rx;
      if (onFrame) onFrame(topic, sender, b64::decode(payloadB64), ts);
    });
    if (m_ops.start) m_ops.start();
    m_started = true;   // stub radio starts instantly; a real radio reports peers via setPeers()
  }
  void stop() { m_started = false; }
  void join(const std::string&) override { /* the mesh floods every topic — no per-topic join */ }
  void send(const std::string& topic, const std::string& sealedBytes) override {
    if (m_ops.flood) { m_ops.flood(topic, b64::encode(sealedBytes)); ++m_tx; }
  }
  std::string metricsJson() const override {
    std::string o = "{\"name\":\"ble\",\"enabled\":";
    o += m_enabled ? "true" : "false";
    o += ",\"priority\":" + std::to_string(m_priority);
    o += ",\"ready\":" + std::string(m_started ? "true" : "false");
    o += ",\"peers\":" + std::to_string(m_peers);
    o += ",\"rx\":" + std::to_string((long long)m_rx);
    o += ",\"tx\":" + std::to_string((long long)m_tx) + "}";
    return o;
  }

private:
  Ops m_ops;
  bool m_started = false;
  long m_peers = 0;
};

} // namespace loam

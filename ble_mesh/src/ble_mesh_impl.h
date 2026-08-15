#pragma once
#include <string>
#include <memory>
#include <mutex>

#include "logos_module_context.h"   // LogosModuleContext + logos_events:
#include "mesh.hpp"                  // portable gossip + frame codec (ADR 0012)
#include "stub_radio.hpp"           // no-op radio until the Qt Bluetooth radio (Phase 3)

/**
 * BleMeshImpl — the BLE offline-mesh bearer as a standalone, reusable Logos CORE module
 * (ADR 0015). It owns the portable flood-gossip (mesh.hpp) over a MeshRadio and exposes a
 * bearer surface: flood sealed bytes onto the mesh, receive frames off it, report peers.
 *
 * loam_core (and any other app) depends on ble_mesh and adds it to its MultiBearer — a write
 * flooded here and the same write over Waku dedup by frameId (identical hash on both sides),
 * so BLE needs no ordering/reliability of its own. It is wire-compatible with the Android Loam
 * mesh + loam-transport bearer.ts (same frame format + frameId).
 *
 * TODAY the radio is a no-op stub (0 peers) so the module builds + loads without hardware; the
 * Qt Bluetooth radio (QLowEnergyController peripheral+central) lands in Phase 3 and drops in
 * with a one-line swap. Universal authoring: public methods are the API (JSON-serializable
 * std::string), no Q_OBJECT; keep declaration lines free of trailing // comments.
 */
class BleMeshImpl : public LogosModuleContext {
public:
  ~BleMeshImpl() override;

  // --- bearer API ---
  std::string start();
  std::string stop();
  std::string flood(std::string topic, std::string payloadB64);
  std::string reachablePeers();
  std::string status();

protected:
  void onContextReady() override;

logos_events:
  // A frame received off the mesh, once-decoded and re-base64'd for IPC. senderId is empty (the
  // mesh carries opaque sealed bytes; the app learns the author from the sealed content).
  void frameReceived(const std::string& topic, const std::string& senderId,
                     const std::string& payloadB64, int64_t ts);

private:
  std::unique_ptr<loam::mesh::MeshRadio> m_radio;
  std::unique_ptr<loam::mesh::BleMeshBearer> m_bearer;
  bool m_started = false;
  std::string m_status = "idle";
  std::recursive_mutex m_mtx;
};

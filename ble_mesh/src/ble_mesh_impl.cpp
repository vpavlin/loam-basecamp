// ble_mesh_impl.cpp — the module wiring around the portable gossip (mesh.hpp).
#include "ble_mesh_impl.h"
#include "logos_sdk.h"   // umbrella (kept for parity with other modules; ble_mesh has no deps)
#ifdef LOAM_HAS_QTBLUETOOTH
#include "mesh_radio_qt.hpp"   // the real Qt Bluetooth radio (Phase 3), built when qtconnectivity is present
#include <openssl/rand.h>
#include <cstdlib>
#endif

using namespace loam::mesh;

// A stable per-process node id for the mesh (ADR 0014): env LOAM_NODE_ID, else random hex.
static std::string loamNodeId() {
#ifdef LOAM_HAS_QTBLUETOOTH
  if (const char* e = std::getenv("LOAM_NODE_ID")) if (*e) return e;
  unsigned char b[8]; if (RAND_bytes(b, 8) != 1) for (int i = 0; i < 8; ++i) b[i] = (unsigned char)i;
  static const char* hx = "0123456789abcdef"; std::string s;
  for (int i = 0; i < 8; ++i) { s.push_back(hx[b[i] >> 4]); s.push_back(hx[b[i] & 15]); }
  return s;
#else
  return "stub";
#endif
}

BleMeshImpl::~BleMeshImpl() { if (m_bearer) m_bearer->stop(); }

void BleMeshImpl::onContextReady() {
  std::lock_guard<std::recursive_mutex> lk(m_mtx);
  // Build the gossip over the radio: the real Qt Bluetooth radio when built with qtconnectivity,
  // else the no-op stub (0 peers). The gossip layer (mesh.hpp) is identical either way.
#ifdef LOAM_HAS_QTBLUETOOTH
  const std::string nid = loamNodeId();
  m_radio = std::make_unique<QtBleRadio>(nid);
  m_status = "ready (Qt Bluetooth radio — node " + nid + ")";
#else
  m_radio = std::make_unique<StubRadio>();
  m_status = "ready (stub radio — 0 peers; build with qtconnectivity for the Qt Bluetooth radio)";
#endif
  m_bearer = std::make_unique<BleMeshBearer>(m_radio.get());
  // Deliver received frames up: re-base64 the once-decoded sealed bytes for IPC safety. (When the
  // Qt radio delivers on its own thread in Phase 3, marshal this emit onto the module thread —
  // Qt Remote Objects drops cross-thread signal emits; same rule as loam_core.)
  m_bearer->onReceive([this](const Frame& f) {
    frameReceived(f.topic, std::string(), b64::encode(f.payload), 0);
  });
}

std::string BleMeshImpl::start() {
  std::lock_guard<std::recursive_mutex> lk(m_mtx);
  if (!m_bearer) return "not ready";
  if (!m_started) { m_bearer->start(); m_started = true; m_status = "started"; }
  return "";
}

std::string BleMeshImpl::stop() {
  std::lock_guard<std::recursive_mutex> lk(m_mtx);
  if (m_bearer && m_started) { m_bearer->stop(); m_started = false; m_status = "stopped"; }
  return "";
}

std::string BleMeshImpl::flood(std::string topic, std::string payloadB64) {
  std::lock_guard<std::recursive_mutex> lk(m_mtx);
  if (!m_bearer) return "not ready";
  if (topic.empty()) return "empty topic";
  m_bearer->send(topic, b64::decode(payloadB64));   // flood the sealed bytes to all neighbours
  return "";
}

std::string BleMeshImpl::reachablePeers() {
  std::lock_guard<std::recursive_mutex> lk(m_mtx);
  return std::to_string(m_bearer ? m_bearer->reachablePeers() : 0);
}

std::string BleMeshImpl::status() {
  std::lock_guard<std::recursive_mutex> lk(m_mtx);
  return m_status;
}

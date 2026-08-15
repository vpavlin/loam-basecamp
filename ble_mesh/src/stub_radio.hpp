// stub_radio.hpp — a no-op MeshRadio so the ble_mesh module builds + loads WITHOUT hardware.
// It has zero peers and drops sends, so the mesh is inert (reachablePeers()==0) until the real
// Qt Bluetooth radio (ble_mesh Phase 3, needs a BLE device) replaces it. Swapping it in is a
// one-line change in ble_mesh_impl — the gossip layer (mesh.hpp) is unchanged.
#pragma once
#include "mesh.hpp"

namespace loam { namespace mesh {

class StubRadio : public MeshRadio {
public:
  void start() override {}
  void stop() override {}
  std::vector<std::string> peers() override { return {}; }
  void sendTo(const std::string&, const Bytes&) override {}
  void onReceiveFrom(std::function<void(const std::string&, const Bytes&)> cb) override { rx_ = std::move(cb); }
private:
  std::function<void(const std::string&, const Bytes&)> rx_;
};

}} // namespace loam::mesh

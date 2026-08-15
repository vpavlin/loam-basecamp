// mesh_radio_qt.hpp — Phase-3 SKELETON for the desktop BLE radio (ADR 0015). NOT yet in the build
// (not in CMakeLists) and NOT hardware-verified — it's a concrete starting point for an on-device
// session. Implements MeshRadio with Qt Bluetooth (QLowEnergyController, peripheral + central) so a
// desktop meshes with the Android Loam phones. Wire-compat contract it MUST match (see README):
//
//   Service UUID  10a11052-0000-4c6f-616d-6d6573680001   ("Loammesh"; app-agnostic, one mesh)
//   Char UUID     10a11052-0000-4c6f-616d-6d6573680002   (WRITE_NO_RESPONSE | NOTIFY)
//   CCCD          00002902-0000-1000-8000-00805f9b34fb
//   MTU           request 512, handle negotiated-down
//   Type byte     0x41 'A' announce (payload = STABLE NODE ID), 0x46 'F' fragment   (ADR 0014)
//   Fragment      one at a time (next after prior write completes); reassemble per (addr,msgId),
//                 30s timeout; chunk cap = mtu - 3(ATT) - <frag header>
//   Identity      peers/routing/dedup keyed by NODE ID, not the rotating BLE address (ADR 0014)
//   Roles         advertise the service AND scan-filter+dial it; the LOWER node id dials (tiebreak)
//
// To wire in: add this + mesh_radio_qt.cpp to CMakeLists SOURCES, add `Qt6::Bluetooth` to
// FIND_PACKAGES/LINK_LIBRARIES and `qt6.qtconnectivity` to metadata.json nix packages, and swap
// `std::make_unique<StubRadio>()` -> `std::make_unique<QtBleRadio>(nodeId)` in ble_mesh_impl.cpp.
// The gossip layer (mesh.hpp) is UNCHANGED — this is a pure radio driver.
#pragma once
#if 0   // ← flip to 1 (and satisfy the build wiring above) when implementing on hardware
#include "mesh.hpp"
#include <QtBluetooth/QLowEnergyController>
#include <QtBluetooth/QBluetoothDeviceDiscoveryAgent>
#include <string>
#include <map>

namespace loam { namespace mesh {

class QtBleRadio : public MeshRadio {
public:
  explicit QtBleRadio(std::string nodeId) : nodeId_(std::move(nodeId)) {}

  // ── MeshRadio ──────────────────────────────────────────────────────────────
  void start() override {
    // 1. PERIPHERAL: QLowEnergyController::createPeripheral(); publish a GATT service with the
    //    Loammesh SERVICE_UUID + a CHAR_UUID characteristic (WriteNoResponse|Notify) + CCCD;
    //    startAdvertising() the service uuid. On a central's write -> onFragment(peerAddr, value).
    // 2. CENTRAL: QBluetoothDeviceDiscoveryAgent scan filtered to SERVICE_UUID; for each device
    //    where OUR node id < theirs (tiebreak), createCentral(), connectToDevice(), discover the
    //    service+char, request MTU 512, enable notifications (write the CCCD). On notify ->
    //    onFragment(peerAddr, value).
    // 3. On a fresh link, exchange an ANNOUNCE (type 0x41, payload = nodeId_) so both sides map
    //    addr<->nodeId (ADR 0014). peers() reports NODE IDs, not addresses.
  }
  void stop() override { /* stop advertising + discovery, disconnect all controllers */ }

  std::vector<std::string> peers() override {
    std::vector<std::string> out; for (auto& kv : addrToNode_) out.push_back(kv.second); return out;
  }

  // Fragment `bytes` to the negotiated MTU for `peerId`, prefix each with the 0x46 fragment
  // header [type, msgId hi, msgId lo, idx, count], and write them ONE AT A TIME (each after the
  // previous QLowEnergyCharacteristic::write completes — the Android radio serializes writes).
  void sendTo(const std::string& peerId, const Bytes& bytes) override {
    // resolve peerId(node id) -> its addr/controller; fragment; enqueue; drain sequentially.
    (void)peerId; (void)bytes;
  }
  void onReceiveFrom(std::function<void(const std::string&, const Bytes&)> cb) override { rx_ = std::move(cb); }

private:
  // Reassembly: buffer fragments per (nodeId,msgId); on the final fragment, deliver the whole
  // message up via rx_(nodeId, reassembled). ANNOUNCE frames (0x41) update addrToNode_ instead.
  void onFragment(const std::string& fromAddr, const Bytes& value) { (void)fromAddr; (void)value; }

  std::string nodeId_;
  std::map<std::string, std::string> addrToNode_;   // BLE addr -> stable node id (ADR 0014)
  std::function<void(const std::string&, const Bytes&)> rx_;
};

}} // namespace loam::mesh
#endif

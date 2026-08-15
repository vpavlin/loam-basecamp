// mesh_radio_qt.hpp — the REAL desktop BLE radio (ADR 0015 Phase 3): implements MeshRadio with
// Qt Bluetooth (QLowEnergyController peripheral + central), wire-compatible with the Android Loam
// mesh (LoamMeshModule.kt). Builds against Qt6 Bluetooth (qtconnectivity). HARDWARE-UNVERIFIED —
// desktop BLE peripheral/advertising via BlueZ is historically flaky (ADR 0015 risks); prove
// laptop↔laptop then laptop↔phone. The gossip layer (mesh.hpp) is unchanged; this is a pure driver.
//
// Wire contract (must match LoamMeshModule.kt):
//   Service 10a11052-0000-4c6f-616d-6d6573680001, Char 10a11052-0000-4c6f-616d-6d6573680002
//   (WriteNoResponse|Notify), CCCD 2902, MTU 512. Type byte 0x41='A' announce (payload = node id),
//   0x46='F' fragment [type, msgId hi, msgId lo, idx, count, chunk…]; reassemble per (nodeId,msgId).
//   Peers/routing/dedup keyed by NODE ID (ADR 0014), not the BLE address. Lower node id dials.
#pragma once
#include "mesh.hpp"
#include <QObject>
#include <QByteArray>
#include <QBluetoothUuid>
#include <QBluetoothDeviceInfo>
#include <QLowEnergyController>
#include <QLowEnergyService>
#include <QLowEnergyServiceData>
#include <QLowEnergyAdvertisingData>
#include <QLowEnergyAdvertisingParameters>
#include <QBluetoothDeviceDiscoveryAgent>
#include <map>
#include <deque>
#include <memory>
#include <functional>

namespace loam { namespace mesh {

class QtBleRadio : public QObject, public MeshRadio {
  Q_OBJECT
public:
  explicit QtBleRadio(std::string nodeId, QObject* parent = nullptr);
  ~QtBleRadio() override;

  // ── MeshRadio ──────────────────────────────────────────────────────────────
  void start() override;
  void stop() override;
  std::vector<std::string> peers() override;                       // connected NODE ids
  void sendTo(const std::string& peerId, const Bytes& bytes) override;
  void onReceiveFrom(std::function<void(const std::string&, const Bytes&)> cb) override { m_rx = std::move(cb); }

  static QBluetoothUuid serviceUuid();
  static QBluetoothUuid charUuid();

private:
  // GATT / protocol constants (match LoamMeshModule.kt)
  static constexpr int  DEFAULT_MTU = 512;
  static constexpr char T_ANNOUNCE  = 0x41;   // 'A'
  static constexpr char T_FRAG      = 0x46;   // 'F'

  // A link to one neighbour (central-role connection we dialed, or a central connected to us).
  struct Link {
    QString addr;                              // BLE address (transient; identity is the node id)
    std::string nodeId;                        // stable id learned via ANNOUNCE (ADR 0014)
    QLowEnergyController* ctrl = nullptr;       // set for links WE dialed (central role)
    QLowEnergyService* service = nullptr;       // remote service (central role)
    QLowEnergyCharacteristic ch;                // remote char (central role)
    int mtu = 23;                               // negotiated ATT MTU
    bool ready = false;                         // service discovered + notifications on
    std::deque<QByteArray> outQ;                // pending fragments (write one at a time)
    bool writing = false;
  };

  // peripheral (advertise + GATT server) — centrals write/subscribe to us
  void startPeripheral();
  void onCentralWrote(const QByteArray& value, const QString& fromAddr);
  // central (scan + dial) — we write/subscribe to peripherals
  void startCentral();
  void dial(const QBluetoothDeviceInfo& info);
  void onServiceStateChanged(std::shared_ptr<Link> link, QLowEnergyService::ServiceState st);
  // protocol
  void announceTo(std::shared_ptr<Link> link);
  void enqueueFragments(std::shared_ptr<Link> link, const Bytes& bytes);
  void drain(std::shared_ptr<Link> link);
  void onIncoming(const QString& fromAddr, const QByteArray& value);   // announce or fragment
  std::shared_ptr<Link> linkForNode(const std::string& nodeId);

  std::string m_nodeId;
  std::function<void(const std::string&, const Bytes&)> m_rx;
  bool m_started = false;
  uint16_t m_msgId = 0;

  // peripheral role
  std::unique_ptr<QLowEnergyController> m_peripheral;
  QLowEnergyService* m_localService = nullptr;

  // central role
  std::unique_ptr<QBluetoothDeviceDiscoveryAgent> m_agent;
  std::map<QString, std::shared_ptr<Link>> m_links;    // addr -> link
  std::map<std::string, QString> m_nodeToAddr;         // node id -> addr

  // reassembly, keyed by "nodeIdOrAddr/msgId"
  struct Reasm { int count = 0; std::map<int, QByteArray> parts; };
  std::map<std::string, Reasm> m_reasm;
};

}} // namespace loam::mesh

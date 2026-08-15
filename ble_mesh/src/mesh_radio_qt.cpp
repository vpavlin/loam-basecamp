// mesh_radio_qt.cpp — Qt Bluetooth implementation of the Loam mesh radio (ADR 0015 Phase 3).
// HARDWARE-UNVERIFIED. See mesh_radio_qt.hpp for the wire contract.
#include "mesh_radio_qt.hpp"
#include <QLowEnergyCharacteristicData>
#include <QLowEnergyDescriptorData>
#include <QLowEnergyConnectionParameters>
#include <QBluetoothLocalDevice>
#include <QLowEnergyAdvertisingData>
#include <QtEndian>
#include <QTimer>
#include <cstdio>

namespace loam { namespace mesh {

QBluetoothUuid QtBleRadio::serviceUuid() { return QBluetoothUuid(QString("10a11052-0000-4c6f-616d-6d6573680001")); }
QBluetoothUuid QtBleRadio::charUuid()    { return QBluetoothUuid(QString("10a11052-0000-4c6f-616d-6d6573680002")); }

QtBleRadio::QtBleRadio(std::string nodeId, QObject* parent) : QObject(parent), m_nodeId(std::move(nodeId)) {}
QtBleRadio::~QtBleRadio() { stop(); }

void QtBleRadio::start() { if (m_started) return; m_started = true; startPeripheral(); startCentral(); }

void QtBleRadio::stop() {
  if (!m_started) return;
  m_started = false;
  if (m_agent) m_agent->stop();
  if (m_peripheral) m_peripheral->stopAdvertising();
  for (auto& kv : m_links) if (kv.second->ctrl) kv.second->ctrl->disconnectFromDevice();
  m_links.clear(); m_nodeToAddr.clear();
}

std::vector<std::string> QtBleRadio::peers() {
  std::vector<std::string> out;
  for (auto& kv : m_links) if (kv.second->ready && !kv.second->nodeId.empty()) out.push_back(kv.second->nodeId);
  return out;
}

// ── peripheral: advertise the service + a Write/Notify characteristic ─────────
void QtBleRadio::startPeripheral() {
  m_peripheral.reset(QLowEnergyController::createPeripheral(this));

  QLowEnergyDescriptorData cccd(QBluetoothUuid::DescriptorType::ClientCharacteristicConfiguration, QByteArray(2, 0));
  QLowEnergyCharacteristicData ch;
  ch.setUuid(charUuid());
  ch.setProperties(QLowEnergyCharacteristic::Write | QLowEnergyCharacteristic::WriteNoResponse | QLowEnergyCharacteristic::Notify);
  ch.addDescriptor(cccd);
  ch.setValueLength(0, DEFAULT_MTU);

  QLowEnergyServiceData svc;
  svc.setType(QLowEnergyServiceData::ServiceTypePrimary);
  svc.setUuid(serviceUuid());
  svc.addCharacteristic(ch);

  auto addAndAdvertise = [this, svc]() {
    m_localService = m_peripheral->addService(svc);
    if (!m_localService) { fprintf(stderr, "[loammesh] addService failed (peripheral role unsupported?)\n"); return; }
    QObject::connect(m_localService, &QLowEnergyService::characteristicChanged, this,
                     [this](const QLowEnergyCharacteristic&, const QByteArray& v) { onCentralWrote(v, QString()); });
    QLowEnergyAdvertisingData adv;
    adv.setLocalName("loam");
    adv.setServices({ serviceUuid() });
    QLowEnergyAdvertisingParameters params;
    m_peripheral->startAdvertising(params, adv, adv);
  };
  // A central connecting stops advertising; re-add the service + re-advertise on disconnect so we
  // stay discoverable to the next neighbour.
  QObject::connect(m_peripheral.get(), &QLowEnergyController::disconnected, this, addAndAdvertise);
  addAndAdvertise();
}

// A central wrote to our characteristic. Without a per-central handle here we key reassembly by
// the fragment's own contents; announces still map identity. (BlueZ peripheral gives limited peer
// info to characteristicChanged — a known desktop limitation to refine on hardware.)
void QtBleRadio::onCentralWrote(const QByteArray& value, const QString& fromAddr) { onIncoming(fromAddr, value); }

// ── central: scan for the service + dial (lower address dials) ────────────────
void QtBleRadio::startCentral() {
  m_agent.reset(new QBluetoothDeviceDiscoveryAgent(this));
  m_agent->setLowEnergyDiscoveryTimeout(0);   // continuous
  QObject::connect(m_agent.get(), &QBluetoothDeviceDiscoveryAgent::deviceDiscovered, this,
    [this](const QBluetoothDeviceInfo& info) {
      if (!info.serviceUuids().contains(serviceUuid())) return;
      const QString addr = info.address().toString();
      if (addr.isEmpty() || m_links.count(addr)) return;
      // tiebreak: only the LOWER local address dials, so two peers don't double-link.
      const QString mine = QBluetoothLocalDevice().address().toString();
      if (!mine.isEmpty() && mine >= addr) return;
      dial(info);
    });
  m_agent->start(QBluetoothDeviceDiscoveryAgent::LowEnergyMethod);
}

void QtBleRadio::dial(const QBluetoothDeviceInfo& info) {
  auto link = std::make_shared<Link>();
  link->addr = info.address().toString();
  link->ctrl = QLowEnergyController::createCentral(info, this);
  m_links[link->addr] = link;

  QObject::connect(link->ctrl, &QLowEnergyController::connected, this, [this, link]() { link->ctrl->discoverServices(); });
  QObject::connect(link->ctrl, &QLowEnergyController::mtuChanged, this, [link](int m) { link->mtu = m; });
  QObject::connect(link->ctrl, &QLowEnergyController::discoveryFinished, this, [this, link]() {
    link->service = link->ctrl->createServiceObject(serviceUuid(), this);
    if (!link->service) { fprintf(stderr, "[loammesh] service not found on %s\n", link->addr.toUtf8().constData()); return; }
    QObject::connect(link->service, &QLowEnergyService::stateChanged, this,
                     [this, link](QLowEnergyService::ServiceState st) { onServiceStateChanged(link, st); });
    QObject::connect(link->service, &QLowEnergyService::characteristicChanged, this,
                     [this, link](const QLowEnergyCharacteristic&, const QByteArray& v) { onIncoming(link->addr, v); });
    QObject::connect(link->service, &QLowEnergyService::characteristicWritten, this,
                     [this, link](const QLowEnergyCharacteristic&, const QByteArray&) { link->writing = false; drain(link); });
    link->service->discoverDetails();
  });
  auto cleanup = [this, link]() { if (link->service) link->service->deleteLater(); m_links.erase(link->addr);
    if (!link->nodeId.empty()) m_nodeToAddr.erase(link->nodeId); };
  QObject::connect(link->ctrl, &QLowEnergyController::disconnected, this, cleanup);
  QObject::connect(link->ctrl, &QLowEnergyController::errorOccurred, this, [cleanup](QLowEnergyController::Error) { cleanup(); });
  link->ctrl->connectToDevice();
}

void QtBleRadio::onServiceStateChanged(std::shared_ptr<Link> link, QLowEnergyService::ServiceState st) {
  if (st != QLowEnergyService::RemoteServiceDiscovered) return;
  link->ch = link->service->characteristic(charUuid());
  if (!link->ch.isValid()) { fprintf(stderr, "[loammesh] char not found\n"); return; }
  // enable notifications (write CCCD = 0x0100)
  auto cccd = link->ch.descriptor(QBluetoothUuid::DescriptorType::ClientCharacteristicConfiguration);
  if (cccd.isValid()) link->service->writeDescriptor(cccd, QByteArray::fromHex("0100"));
  link->mtu = link->ctrl->mtu();
  link->ready = true;
  announceTo(link);          // exchange stable node ids (ADR 0014)
}

// ── protocol: announce + fragmentation + reassembly ───────────────────────────
void QtBleRadio::announceTo(std::shared_ptr<Link> link) {
  QByteArray a; a.append(T_ANNOUNCE); a.append(QByteArray::fromStdString(m_nodeId));
  if (link->service && link->ch.isValid())
    link->service->writeCharacteristic(link->ch, a, QLowEnergyService::WriteWithoutResponse);
}

void QtBleRadio::enqueueFragments(std::shared_ptr<Link> link, const Bytes& bytes) {
  const int cap = std::max(20, link->mtu - 3 /*ATT*/ - 5 /*frag header*/);
  const int total = (int)((bytes.size() + cap - 1) / cap);
  const uint16_t id = m_msgId++;
  for (int i = 0; i < total; ++i) {
    QByteArray f;
    f.append(T_FRAG);
    f.append(char((id >> 8) & 0xff)); f.append(char(id & 0xff));
    f.append(char(i & 0xff)); f.append(char(total & 0xff));
    f.append(QByteArray::fromStdString(bytes.substr((size_t)i * cap, (size_t)cap)));
    link->outQ.push_back(f);
  }
  drain(link);
}

// Write fragments ONE AT A TIME (next after the prior write completes) — the Android radio
// serializes writes; back-to-back writes drop on many stacks.
void QtBleRadio::drain(std::shared_ptr<Link> link) {
  if (link->writing || link->outQ.empty() || !link->ready || !link->service || !link->ch.isValid()) return;
  QByteArray f = link->outQ.front(); link->outQ.pop_front();
  link->writing = true;
  link->service->writeCharacteristic(link->ch, f, QLowEnergyService::WriteWithoutResponse);
  // WriteWithoutResponse has no characteristicWritten on some backends; nudge the queue.
  QTimer::singleShot(15, this, [this, link]() { link->writing = false; drain(link); });
}

void QtBleRadio::sendTo(const std::string& peerId, const Bytes& bytes) {
  auto link = linkForNode(peerId);
  if (link) enqueueFragments(link, bytes);
}

std::shared_ptr<QtBleRadio::Link> QtBleRadio::linkForNode(const std::string& nodeId) {
  auto it = m_nodeToAddr.find(nodeId);
  if (it == m_nodeToAddr.end()) return nullptr;
  auto lit = m_links.find(it->second);
  return lit == m_links.end() ? nullptr : lit->second;
}

void QtBleRadio::onIncoming(const QString& fromAddr, const QByteArray& value) {
  if (value.isEmpty()) return;
  const char type = value.at(0);
  if (type == T_ANNOUNCE) {
    std::string nodeId = value.mid(1).toStdString();
    if (!fromAddr.isEmpty()) {
      auto lit = m_links.find(fromAddr);
      if (lit != m_links.end()) { lit->second->nodeId = nodeId; m_nodeToAddr[nodeId] = fromAddr; announceTo(lit->second); }
    }
    return;
  }
  if (type == T_FRAG && value.size() >= 5) {
    const uint16_t id = ((uint8_t)value[1] << 8) | (uint8_t)value[2];
    const int idx = (uint8_t)value[3], count = (uint8_t)value[4];
    const std::string key = fromAddr.toStdString() + "/" + std::to_string(id);
    auto& r = m_reasm[key]; r.count = count; r.parts[idx] = value.mid(5);
    if ((int)r.parts.size() == count) {
      QByteArray whole; for (auto& p : r.parts) whole.append(p.second);
      m_reasm.erase(key);
      std::string node = fromAddr.toStdString();
      auto lit = m_links.find(fromAddr);
      if (lit != m_links.end() && !lit->second->nodeId.empty()) node = lit->second->nodeId;
      if (m_rx) m_rx(node, whole.toStdString());
    }
  }
}

}} // namespace loam::mesh

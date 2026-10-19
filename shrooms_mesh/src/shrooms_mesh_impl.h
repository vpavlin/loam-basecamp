#pragma once
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "logos_module_context.h"   // LogosModuleContext + logos_events:
#include "mesh_wire.hpp"

class QTcpServer;
class QTcpSocket;
class QTimer;

/**
 * ShroomsMeshImpl — a Loam bearer over the Shrooms mesh, as a standalone Logos CORE module, beside
 * ble_mesh. It moves opaque sealed frames between your own devices: every machine on the mesh
 * listens on TCP 7392 at its mesh (overlay) addresses only, and a flood sends the frame to every
 * online mesh peer that answers there. Peers come from the Shrooms daemon's status socket
 * (or the shrooms-agent's /v1/peers), refreshed every minute.
 *
 * The mesh is WireGuard-authenticated, and frames are sealed by the app (a calendar's key), so a
 * peer that isn't a member just can't open them. No ordering, store or retries here: the app's own
 * catch-up (Scala's RBSR) repairs what a peer missed.
 *
 * loam_core adds it to its MultiBearer with useMesh:true; with useDelivery:false it is the only
 * bearer and no Logos Delivery node exists on the machine. Universal authoring: public methods
 * are the API (std::string), no Q_OBJECT; keep declaration lines free of trailing // comments.
 */
class ShroomsMeshImpl : public LogosModuleContext {
public:
  ~ShroomsMeshImpl() override;

  // --- bearer API (same shape as ble_mesh) ---
  std::string start();
  std::string stop();
  std::string flood(std::string topic, std::string payloadB64);
  std::string reachablePeers();
  std::string status();
  // --- diagnostics: {listening:[addr], peers:[{name,mesh,addr,connected}], rx, tx, dropped}
  std::string peers();
  // Re-read the peer list now (otherwise every minute).
  std::string refresh();

protected:
  void onContextReady() override;

logos_events:
  // A frame received from a mesh peer, re-base64'd for IPC. senderId is empty: the author is in the
  // sealed content, as on every Loam bearer.
  void frameReceived(const std::string& topic, const std::string& senderId,
                     const std::string& payloadB64, int64_t ts);

private:
  struct Out { QTcpSocket* sock = nullptr; std::string name, mesh; bool connected = false; };
  void listen(const std::vector<std::string>& addrs);
  void applyPeers(const shrooms_mesh::PeerList& pl);
  void fetchPeers();
  void fetchFrom(size_t idx);
  void accept(QTcpServer* srv);
  void received(const shrooms_mesh::Frame& f);
  bool inMesh(const std::string& addr) const;

  bool m_started = false;
  std::string m_status = "idle";
  std::vector<QTcpServer*> m_servers;
  std::map<std::string, Out> m_out;                 // peer mesh address -> outgoing connection
  std::vector<std::string> m_prefixes;              // accept inbound only from these
  std::set<std::string> m_seen; std::deque<std::string> m_seenOrder;   // recent frameIds (a peer on two meshes sends twice)
  QTimer* m_timer = nullptr;
  bool m_fetching = false;
  uint64_t m_rx = 0, m_tx = 0, m_dropped = 0;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

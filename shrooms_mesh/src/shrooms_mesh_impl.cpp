// shrooms_mesh_impl.cpp — the sockets around mesh_wire.hpp.
#include "shrooms_mesh_impl.h"

#include <QByteArray>
#include <QHostAddress>
#include <QLocalSocket>
#include <QNetworkInterface>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>

#include "logos_sdk.h"   // umbrella (parity with other modules; shrooms_mesh has no deps)

using shrooms_mesh::Frame;
using shrooms_mesh::PeerList;

namespace {

// base64 (standard alphabet out; standard or URL-safe in), as loam_core and ble_mesh use for IPC.
const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
std::string b64encode(const std::string& in) {
    std::string out; int val = 0, bits = -6;
    for (unsigned char c : in) { val = (val << 8) + c; bits += 8; while (bits >= 0) { out.push_back(kB64[(val >> bits) & 0x3F]); bits -= 6; } }
    if (bits > -6) out.push_back(kB64[((val << 8) >> (bits + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}
std::string b64decode(const std::string& in) {
    int T[256]; for (int& t : T) t = -1;
    for (int i = 0; i < 64; i++) T[(unsigned char)kB64[i]] = i;
    T[(unsigned char)'-'] = 62; T[(unsigned char)'_'] = 63;
    std::string out; int val = 0, bits = -8;
    for (unsigned char c : in) { if (c == '=' || T[c] == -1) break; val = (val << 6) + T[c]; bits += 6; if (bits >= 0) { out.push_back(char((val >> bits) & 0xFF)); bits -= 8; } }
    return out;
}

// Where the peer list comes from, in order: the Shrooms daemon's status, then the agent's peers.
struct Source { std::string socket, path; };
std::vector<Source> sources() {
    std::vector<Source> s;
    auto env = [](const char* k) { const char* v = std::getenv(k); return std::string(v ? v : ""); };
    if (!env("SHROOMS_SOCKET").empty()) s.push_back({env("SHROOMS_SOCKET"), "/status"});
    s.push_back({"/run/shrooms/shrooms.sock", "/status"});
    if (!env("SHROOMS_AGENT_SOCKET").empty()) s.push_back({env("SHROOMS_AGENT_SOCKET"), "/v1/peers"});
    if (!env("XDG_RUNTIME_DIR").empty()) s.push_back({env("XDG_RUNTIME_DIR") + "/shrooms-agent/agent.sock", "/v1/peers"});
    return s;
}

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// "fdb0:9afc:a5ef::/48" style prefix test.
bool inPrefix(const QHostAddress& a, const std::string& prefix) {
    const auto p = QHostAddress::parseSubnet(QString::fromStdString(prefix));
    return !p.first.isNull() && a.isInSubnet(p);
}

} // namespace

ShroomsMeshImpl::~ShroomsMeshImpl() {
    m_alive.reset();
    for (auto& [addr, o] : m_out) if (o.sock) { o.sock->abort(); o.sock->deleteLater(); }
    for (auto* s : m_servers) { s->close(); s->deleteLater(); }
    if (m_timer) { m_timer->stop(); m_timer->deleteLater(); }
}

void ShroomsMeshImpl::onContextReady() {
    m_status = "ready";
}

std::string ShroomsMeshImpl::start() {
    if (m_started) return "";
    m_started = true;
    m_status = "starting: reading the mesh peers";
    m_timer = new QTimer();
    QObject::connect(m_timer, &QTimer::timeout, m_timer, [this] { fetchPeers(); });
    m_timer->start(60 * 1000);
    fetchPeers();
    return "";
}

std::string ShroomsMeshImpl::stop() {
    if (!m_started) return "";
    m_started = false;
    if (m_timer) { m_timer->stop(); m_timer->deleteLater(); m_timer = nullptr; }
    for (auto& [addr, o] : m_out) if (o.sock) { o.sock->abort(); o.sock->deleteLater(); }
    m_out.clear();
    for (auto* s : m_servers) { s->close(); s->deleteLater(); }
    m_servers.clear();
    m_status = "stopped";
    return "";
}

std::string ShroomsMeshImpl::refresh() {
    if (!m_started) return "not started";
    fetchPeers();
    return "";
}

// ── peer list ─────────────────────────────────────────────────────────────────

void ShroomsMeshImpl::fetchPeers() {
    if (m_fetching) return;
    m_fetching = true;
    fetchFrom(0);
}

// One HTTP/1.0 GET over a unix socket (the server closes when done), then parse; on any failure
// try the next source.
void ShroomsMeshImpl::fetchFrom(size_t idx) {
    const auto srcs = sources();
    if (idx >= srcs.size()) {
        m_fetching = false;
        if (m_servers.empty()) m_status = "no mesh peers: the Shrooms daemon's status socket isn't reachable";
        return;
    }
    auto* sock = new QLocalSocket();
    auto buf = std::make_shared<QByteArray>();
    auto done = std::make_shared<bool>(false);
    std::weak_ptr<bool> alive = m_alive;
    auto finish = [this, alive, sock, buf, done, idx]() {
        if (*done) return;
        *done = true;
        sock->deleteLater();
        if (alive.expired()) return;
        const int split = buf->indexOf("\r\n\r\n");
        const bool ok = buf->startsWith("HTTP/1.") && buf->mid(9, 3) == "200" && split > 0;
        nlohmann::json j = ok ? nlohmann::json::parse(buf->mid(split + 4).toStdString(), nullptr, false) : nlohmann::json();
        PeerList pl = shrooms_mesh::parsePeers(j);
        if (!ok || j.is_discarded() || (pl.peers.empty() && pl.self.empty())) { fetchFrom(idx + 1); return; }
        m_fetching = false;
        applyPeers(pl);
    };
    QObject::connect(sock, &QLocalSocket::readyRead, sock, [sock, buf] { buf->append(sock->readAll()); });
    QObject::connect(sock, &QLocalSocket::disconnected, sock, [sock, buf, finish] { buf->append(sock->readAll()); finish(); });
    QObject::connect(sock, &QLocalSocket::errorOccurred, sock, [finish](QLocalSocket::LocalSocketError e) {
        if (e != QLocalSocket::PeerClosedError) finish();
    });
    QObject::connect(sock, &QLocalSocket::connected, sock, [sock, path = srcs[idx].path] {
        sock->write(QByteArray::fromStdString("GET " + path + " HTTP/1.0\r\nHost: unix\r\n\r\n"));
    });
    QTimer::singleShot(5000, sock, [sock, finish] { sock->abort(); finish(); });
    sock->connectToServer(QString::fromStdString(srcs[idx].socket));
}

bool ShroomsMeshImpl::inMesh(const std::string& addr) const {
    const QHostAddress a(QString::fromStdString(addr));
    for (const auto& p : m_prefixes) if (inPrefix(a, p)) return true;
    return false;
}

void ShroomsMeshImpl::applyPeers(const PeerList& plIn) {
    PeerList pl = plIn;
    // The agent's /v1/peers has no prefixes or own addresses: take each peer's /48, and our own
    // addresses from the interfaces inside those prefixes.
    if (pl.prefixes.empty())
        for (const auto& p : pl.peers) {
            QHostAddress a(QString::fromStdString(p.addr));
            if (a.protocol() != QAbstractSocket::IPv6Protocol) continue;
            Q_IPV6ADDR b = a.toIPv6Address();
            for (int i = 6; i < 16; ++i) b[i] = 0;
            const std::string pre = QHostAddress(b).toString().toStdString() + "/48";
            if (std::find(pl.prefixes.begin(), pl.prefixes.end(), pre) == pl.prefixes.end()) pl.prefixes.push_back(pre);
        }
    m_prefixes = pl.prefixes;
    if (pl.self.empty())
        for (const auto& a : QNetworkInterface::allAddresses())
            if (a.protocol() == QAbstractSocket::IPv6Protocol)
                for (const auto& p : m_prefixes) if (inPrefix(a, p)) { pl.self.push_back(a.toString().toStdString()); break; }
    listen(pl.self);

    // Outgoing connections: one per online peer address; drop the ones that left.
    std::set<std::string> want;
    for (const auto& p : pl.peers) if (inMesh(p.addr)) want.insert(p.addr);
    for (auto it = m_out.begin(); it != m_out.end();) {
        if (!want.count(it->first)) { if (it->second.sock) { it->second.sock->abort(); it->second.sock->deleteLater(); } it = m_out.erase(it); }
        else ++it;
    }
    for (const auto& p : pl.peers) {
        if (!want.count(p.addr)) continue;
        Out& o = m_out[p.addr];
        o.name = p.name; o.mesh = p.mesh;
        if (o.sock && o.sock->state() != QAbstractSocket::UnconnectedState) continue;   // connected or connecting
        if (!o.sock) {
            o.sock = new QTcpSocket();
            const std::string addr = p.addr;
            QObject::connect(o.sock, &QTcpSocket::connected, o.sock, [this, addr] { if (m_out.count(addr)) m_out[addr].connected = true; });
            QObject::connect(o.sock, &QTcpSocket::disconnected, o.sock, [this, addr] { if (m_out.count(addr)) m_out[addr].connected = false; });
            QObject::connect(o.sock, &QTcpSocket::errorOccurred, o.sock, [this, addr](QAbstractSocket::SocketError) {
                if (m_out.count(addr)) m_out[addr].connected = false;   // closed port, peer gone: retried on the next refresh
            });
            // We only send on outgoing connections; anything a peer writes back is ignored.
            QObject::connect(o.sock, &QTcpSocket::readyRead, o.sock, [s = o.sock] { s->readAll(); });
        }
        o.sock->connectToHost(QHostAddress(QString::fromStdString(p.addr)), shrooms_mesh::kPort);
    }
    m_status = "running: listening on " + std::to_string(m_servers.size()) + " mesh address(es), "
             + std::to_string(want.size()) + " online peer(s)";
}

// Listen on each of our mesh addresses (never on all interfaces). Already-bound ones stay.
void ShroomsMeshImpl::listen(const std::vector<std::string>& addrs) {
    for (const auto& a : addrs) {
        bool have = false;
        for (auto* s : m_servers) if (s->serverAddress().toString().toStdString() == a) have = true;
        if (have) continue;
        auto* srv = new QTcpServer();
        if (!srv->listen(QHostAddress(QString::fromStdString(a)), shrooms_mesh::kPort)) {
            m_status = "can't listen on [" + a + "]:" + std::to_string(shrooms_mesh::kPort) + ": " + srv->errorString().toStdString();
            srv->deleteLater();
            continue;
        }
        QObject::connect(srv, &QTcpServer::newConnection, srv, [this, srv] { accept(srv); });
        m_servers.push_back(srv);
    }
}

// ── receive ───────────────────────────────────────────────────────────────────

void ShroomsMeshImpl::accept(QTcpServer* srv) {
    while (QTcpSocket* c = srv->nextPendingConnection()) {
        QHostAddress from = c->peerAddress();
        bool ok = false;
        for (const auto& p : m_prefixes) if (inPrefix(from, p)) ok = true;
        if (!ok) { c->abort(); c->deleteLater(); continue; }   // only the mesh's own addresses
        auto reader = std::make_shared<shrooms_mesh::Reader>();
        QObject::connect(c, &QTcpSocket::readyRead, c, [this, c, reader] {
            const QByteArray data = c->readAll();
            std::vector<Frame> frames;
            if (!reader->feed(data.constData(), (size_t)data.size(), frames)) { ++m_dropped; c->abort(); return; }
            for (const auto& f : frames) received(f);
        });
        QObject::connect(c, &QTcpSocket::disconnected, c, [c] { c->deleteLater(); });
    }
}

void ShroomsMeshImpl::received(const Frame& f) {
    const std::string id = shrooms_mesh::frameId(f.topic, f.payload);
    if (!m_seen.insert(id).second) return;   // the same frame via a second mesh
    m_seenOrder.push_back(id);
    while (m_seenOrder.size() > 4096) { m_seen.erase(m_seenOrder.front()); m_seenOrder.pop_front(); }
    ++m_rx;
    frameReceived(f.topic, std::string(), b64encode(f.payload), nowMs());
}

// ── send ──────────────────────────────────────────────────────────────────────

std::string ShroomsMeshImpl::flood(std::string topic, std::string payloadB64) {
    if (topic.empty() || topic.size() > shrooms_mesh::kMaxTopic) return "bad topic";
    const std::string payload = b64decode(payloadB64);
    if (payload.size() + topic.size() + 3 > shrooms_mesh::kMaxFrame) return "frame too large";
    const QByteArray frame = QByteArray::fromStdString(shrooms_mesh::encode(topic, payload));
    size_t sent = 0;
    for (auto& [addr, o] : m_out) {
        if (!o.connected || !o.sock) continue;
        if (o.sock->bytesToWrite() > 16 * 1024 * 1024) { ++m_dropped; continue; }   // a stuck peer: drop, catch-up repairs
        o.sock->write(frame);
        ++sent;
    }
    if (sent) ++m_tx;
    return "";
}

// ── status ────────────────────────────────────────────────────────────────────

std::string ShroomsMeshImpl::reachablePeers() {
    size_t up = 0;
    for (const auto& [a, o] : m_out) if (o.connected) ++up;
    return std::to_string(up);
}

std::string ShroomsMeshImpl::status() { return m_status; }

std::string ShroomsMeshImpl::peers() {
    nlohmann::json j = {{"listening", nlohmann::json::array()}, {"peers", nlohmann::json::array()},
                        {"rx", m_rx}, {"tx", m_tx}, {"dropped", m_dropped}, {"status", m_status}};
    for (auto* s : m_servers) j["listening"].push_back(s->serverAddress().toString().toStdString());
    for (const auto& [a, o] : m_out) j["peers"].push_back({{"name", o.name}, {"mesh", o.mesh}, {"addr", a}, {"connected", o.connected}});
    return j.dump();
}

// mesh.hpp — portable BLE mesh gossip (ADR 0012), ported from loam-transport's bearer.ts so the
// two implementations stay wire-compatible (a desktop and a phone mesh together). Phone-free:
// the flood-gossip (seen-set, hop-TTL, carry-forward) runs over an abstract MeshRadio. The real
// BLE radio (Qt Bluetooth, ble_mesh Phase 3) implements MeshRadio on the device; MockRadio
// implements it in tests — no hardware, same logic. See ADR 0015 §"Code reuse & parity".
#pragma once
#include <string>
#include <vector>
#include <functional>
#include <set>
#include <deque>
#include <memory>
#include <cstdint>
#include <openssl/sha.h>

namespace loam { namespace mesh {

using Bytes = std::string;   // opaque bytes as a binary-safe byte-string; this layer never inspects them

// base64 (RFC 4648) — payloads cross the module IPC boundary as base64-in-JSON (text-safe).
namespace b64 {
inline const char* tbl() { return "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"; }
inline std::string encode(const std::string& in) {
  std::string out; out.reserve(((in.size() + 2) / 3) * 4);
  int val = 0, bits = -6; const unsigned char* p = (const unsigned char*)in.data();
  for (size_t i = 0; i < in.size(); ++i) { val = (val << 8) + p[i]; bits += 8;
    while (bits >= 0) { out.push_back(tbl()[(val >> bits) & 0x3F]); bits -= 6; } }
  if (bits > -6) out.push_back(tbl()[((val << 8) >> (bits + 8)) & 0x3F]);
  while (out.size() % 4) out.push_back('=');
  return out;
}
inline std::string decode(const std::string& in) {
  std::vector<int> T(256, -1); for (int i = 0; i < 64; i++) T[(unsigned char)tbl()[i]] = i;
  std::string out; int val = 0, bits = -8;
  for (unsigned char c : in) { if (c == '=' || T[c] == -1) break;
    val = (val << 6) + T[c]; bits += 6;
    if (bits >= 0) { out.push_back(char((val >> bits) & 0xFF)); bits -= 8; } }
  return out;
}
} // namespace b64

struct Frame { std::string id; std::string topic; int hop; Bytes payload; };

// Content-hash frame id = hex(sha256(topic ‖ 0x00 ‖ payload)[:16]) — BYTE-IDENTICAL to bearer.ts
// AND loam_core's frameId, so the same sealed bytes over BLE and over Waku dedup to ONE frame with
// no shared id generator. id excludes `hop` (which changes on forward) so a forward can't forge it.
inline std::string frameId(const std::string& topic, const Bytes& payload) {
  std::string buf; buf.reserve(topic.size() + 1 + payload.size());
  buf.append(topic); buf.push_back('\0'); buf.append(payload);
  unsigned char d[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(buf.data()), buf.size(), d);
  static const char* hx = "0123456789abcdef";
  std::string s; s.reserve(32);
  for (int i = 0; i < 16; ++i) { s.push_back(hx[d[i] >> 4]); s.push_back(hx[d[i] & 15]); }
  return s;
}
inline Frame makeFrame(const std::string& topic, const Bytes& payload, int hop = 6) {
  return Frame{ frameId(topic, payload), topic, hop, payload };
}

// wire: [ ver(1) | hop(1) | topicLen(2 BE) | topic utf8 | payload ]. The id is NOT on the wire —
// it's recomputed from (topic‖payload) on receive. (topic travels as its raw bytes; kym/qaku
// content topics are ASCII, matching bearer.ts's utf8 length for the ASCII range.)
static const uint8_t WIRE_VER = 1;
inline Bytes encodeFrame(const Frame& f) {
  Bytes out; out.reserve(4 + f.topic.size() + f.payload.size());
  out.push_back(static_cast<char>(WIRE_VER));
  out.push_back(static_cast<char>(f.hop & 0xff));
  out.push_back(static_cast<char>((f.topic.size() >> 8) & 0xff));
  out.push_back(static_cast<char>(f.topic.size() & 0xff));
  out.append(f.topic);
  out.append(f.payload);
  return out;
}
inline bool decodeFrame(const Bytes& b, Frame& out) {
  if (b.size() < 4 || static_cast<uint8_t>(b[0]) != WIRE_VER) return false;
  int hop = static_cast<uint8_t>(b[1]);
  int tlen = (static_cast<uint8_t>(b[2]) << 8) | static_cast<uint8_t>(b[3]);
  if (static_cast<int>(b.size()) < 4 + tlen) return false;
  std::string topic = b.substr(4, tlen);
  Bytes payload = b.substr(4 + tlen);
  out = Frame{ frameId(topic, payload), topic, hop, payload };
  return true;
}

// A bounded, insertion-ordered set of recently-seen frame ids (loop/flood kill + cross-bearer dedup).
class SeenSet {
  std::set<std::string> ids_; std::deque<std::string> order_; size_t cap_;
public:
  explicit SeenSet(size_t cap = 4096) : cap_(cap) {}
  bool has(const std::string& id) const { return ids_.count(id) > 0; }
  void add(const std::string& id) {
    if (ids_.count(id)) return;
    ids_.insert(id); order_.push_back(id);
    if (order_.size() > cap_) { ids_.erase(order_.front()); order_.pop_front(); }
  }
};

// The dumb link layer the mesh drives. Real BLE (Qt Bluetooth) or MockRadio (tests) implements it.
struct MeshRadio {
  virtual ~MeshRadio() = default;
  virtual void start() = 0;
  virtual void stop() = 0;
  virtual std::vector<std::string> peers() = 0;                               // connected neighbour ids
  virtual void sendTo(const std::string& peerId, const Bytes& bytes) = 0;     // radio owns MTU fragmentation
  virtual void onReceiveFrom(std::function<void(const std::string&, const Bytes&)> cb) = 0;
};

// Flood-gossip bearer: send = flood a frame to all neighbours; receive = deliver locally ONCE,
// then carry-forward to the OTHER neighbours at hop-1 until TTL runs out or the seen-set kills it.
// Deliberately dumb — convergence is loam-sync's job, not the mesh's.
class BleMeshBearer {
  MeshRadio* radio_; SeenSet seen_; int ttl_;
  std::function<void(const Frame&)> rx_ = [](const Frame&) {};
  void broadcastExcept(const std::string* except, const Frame& f) {
    Bytes bytes = encodeFrame(f);
    for (const auto& p : radio_->peers()) if (!except || p != *except) radio_->sendTo(p, bytes);
  }
  void onRadio(const std::string& from, const Bytes& bytes) {
    Frame f; if (!decodeFrame(bytes, f)) return;         // malformed
    if (seen_.has(f.id)) return;                         // loop / already delivered
    seen_.add(f.id);
    try { rx_(f); } catch (...) { /* never let a consumer kill the mesh */ }
    if (f.hop > 1) { f.hop -= 1; broadcastExcept(&from, f); }   // store-carry-forward
  }
public:
  explicit BleMeshBearer(MeshRadio* r, int ttl = 6, size_t seenCap = 4096)
    : radio_(r), seen_(seenCap), ttl_(ttl) {}
  const char* name() const { return "ble"; }
  void start() { radio_->onReceiveFrom([this](const std::string& p, const Bytes& b) { onRadio(p, b); }); radio_->start(); }
  void stop() { radio_->stop(); }
  int reachablePeers() { return static_cast<int>(radio_->peers().size()); }
  void onReceive(std::function<void(const Frame&)> cb) { rx_ = std::move(cb); }

  // Originate a local write onto the mesh: flood to every neighbour at full TTL (deduped so we
  // don't re-flood something we already originated/saw).
  void send(const std::string& topic, const Bytes& payload) {
    Frame f = makeFrame(topic, payload, ttl_);
    if (seen_.has(f.id)) return;
    seen_.add(f.id);
    broadcastExcept(nullptr, f);
  }
};

}} // namespace loam::mesh

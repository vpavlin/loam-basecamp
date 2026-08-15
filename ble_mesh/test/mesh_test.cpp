// mesh_test.cpp — unit tests for the portable BLE mesh (mesh.hpp), mirroring loam-transport's
// bearer.ts suite: frame codec round-trip, frameId (deterministic + hop-independent + parity
// vectors), flood, cross-bearer dedup, store-carry-forward, TTL exhaustion, N-node convergence.
// No hardware — an in-memory MockRadio/MockMesh plays the radio. Build:
//   g++ -std=c++17 -I src test/mesh_test.cpp -lcrypto -o /tmp/mesh_test && /tmp/mesh_test
#include "mesh.hpp"
#include <map>
#include <iostream>
#include <cassert>
using namespace loam::mesh;

static int failures = 0, checks = 0;
#define CHECK(cond, msg) do { ++checks; if (!(cond)) { ++failures; \
  std::cerr << "FAIL: " << msg << "  (" #cond ")\n"; } } while (0)

static std::string hexOf(const std::string& b) {
  static const char* hx = "0123456789abcdef"; std::string s;
  for (unsigned char c : b) { s.push_back(hx[c >> 4]); s.push_back(hx[c & 15]); } return s;
}

// ── in-memory mesh: MockRadio delivers synchronously to connected neighbours ──
struct MockMesh;
struct MockRadio : MeshRadio {
  std::string id; MockMesh* mesh = nullptr;
  std::function<void(const std::string&, const Bytes&)> cb;
  void start() override {} void stop() override {}
  std::vector<std::string> peers() override;
  void sendTo(const std::string& peer, const Bytes& bytes) override;
  void onReceiveFrom(std::function<void(const std::string&, const Bytes&)> c) override { cb = std::move(c); }
};
struct MockMesh {
  std::map<std::string, MockRadio*> radios;
  std::map<std::string, std::vector<std::string>> adj;
  void connect(const std::string& a, const std::string& b) { adj[a].push_back(b); adj[b].push_back(a); }
  void deliver(const std::string& from, const std::string& to, const Bytes& bytes) {
    auto it = radios.find(to); if (it != radios.end() && it->second->cb) it->second->cb(from, bytes);
  }
};
std::vector<std::string> MockRadio::peers() { return mesh->adj[id]; }
void MockRadio::sendTo(const std::string& peer, const Bytes& bytes) { mesh->deliver(id, peer, bytes); }

// a node = radio + bearer + a log of frames delivered UP (to the sync layer)
struct Node {
  MockRadio radio; std::unique_ptr<BleMeshBearer> bearer; std::vector<Frame> got;
  Node(MockMesh* m, const std::string& id, int ttl = 6) {
    radio.id = id; radio.mesh = m; m->radios[id] = &radio;
    bearer = std::make_unique<BleMeshBearer>(&radio, ttl);
    bearer->onReceive([this](const Frame& f) { got.push_back(f); });
    bearer->start();
  }
};

int main() {
  // 1. frame codec round-trip
  {
    Frame f = makeFrame("/kym/1/abc/proto", std::string("\x01\x02\x00\xff sealed", 12), 6);
    Frame g; CHECK(decodeFrame(encodeFrame(f), g), "decode(encode) ok");
    CHECK(g.topic == f.topic, "topic round-trips");
    CHECK(g.payload == f.payload, "payload round-trips (binary-safe)");
    CHECK(g.hop == f.hop, "hop round-trips");
    CHECK(g.id == f.id, "id round-trips");
  }
  // 2. frameId: deterministic, hop-independent, collision-free on distinct payloads
  {
    std::string t = "/kym/1/x/proto";
    CHECK(frameId(t, "hello") == frameId(t, "hello"), "frameId deterministic");
    CHECK(frameId(t, "hello") != frameId(t, "world"), "frameId distinguishes payloads");
    CHECK(frameId(t, "hello") != frameId("/other", "hello"), "frameId distinguishes topics");
    CHECK(makeFrame(t, "p", 6).id == makeFrame(t, "p", 1).id, "id independent of hop");
  }
  // 3. malformed / version guard
  {
    Frame g; CHECK(!decodeFrame(std::string("\x00\x06\x00\x01t", 5), g), "wrong ver rejected");
    CHECK(!decodeFrame("ab", g), "too-short rejected");
    Frame g2; CHECK(!decodeFrame(std::string("\x01\x06\x00\xff x", 6), g2), "truncated topic rejected");
  }
  // 4. flood: A -> B (directly connected)
  {
    MockMesh m; Node A(&m, "A"), B(&m, "B"); m.connect("A", "B");
    A.bearer->send("/t", "msg1");
    CHECK(B.got.size() == 1, "B receives A's flood");
    CHECK(B.got.size() && B.got[0].payload == "msg1", "B gets the payload");
    CHECK(A.got.empty(), "A does not deliver its own origination up");
  }
  // 5. dedup: a frame arriving twice delivers up ONCE
  {
    MockMesh m; Node A(&m, "A"), B(&m, "B"), C(&m, "C");
    m.connect("A", "B"); m.connect("A", "C"); m.connect("B", "C"); // triangle → B hears it from A and via C
    A.bearer->send("/t", "dup");
    CHECK(B.got.size() == 1, "B delivers duplicate-flooded frame exactly once");
    CHECK(C.got.size() == 1, "C delivers exactly once");
  }
  // 6. carry-forward: line A—B—C, A floods, C gets it via B with hop decremented
  {
    MockMesh m; Node A(&m, "A"), B(&m, "B"), C(&m, "C");
    m.connect("A", "B"); m.connect("B", "C");        // A not directly connected to C
    A.bearer->send("/t", "relayed");
    CHECK(B.got.size() == 1, "B (middle) delivers up");
    CHECK(C.got.size() == 1, "C receives via B's carry-forward");
    // A sends at ttl=6; B receives hop=6, forwards hop=5; C receives hop=5.
    CHECK(C.got.size() && C.got[0].hop == 5, "hop decremented once on the A->B->C relay");
  }
  // 7. TTL exhaustion: ttl=1 does not travel past the first hop
  {
    MockMesh m; Node A(&m, "A", 1), B(&m, "B", 1), C(&m, "C", 1);
    m.connect("A", "B"); m.connect("B", "C");
    A.bearer->send("/t", "short");
    CHECK(B.got.size() == 1, "B (1 hop) receives");
    CHECK(C.got.empty(), "C does NOT receive (ttl=1 exhausted at B, no carry-forward)");
  }
  // 8. N-node convergence: a 6-node line, A floods, everyone downstream within TTL gets it once
  {
    MockMesh m; std::vector<std::unique_ptr<Node>> ns;
    for (int i = 0; i < 6; ++i) ns.push_back(std::make_unique<Node>(&m, std::string(1, 'A' + i), 6));
    for (int i = 0; i + 1 < 6; ++i) m.connect(std::string(1, 'A' + i), std::string(1, 'A' + i + 1));
    ns[0]->bearer->send("/t", "wave");
    for (int i = 1; i < 6; ++i) CHECK(ns[i]->got.size() == 1, "node " + std::to_string(i) + " converged once");
  }

  // 9. GOLDEN VECTORS — cross-check these against loam-transport bearer.ts frameId()/encodeFrame()
  //    for the same inputs to confirm desktop<->phone wire parity (ADR 0015 §parity).
  {
    std::cout << "-- parity vectors (must equal bearer.ts for the same inputs) --\n";
    std::cout << "frameId(\"/kym/1/x/proto\",\"hello\") = " << frameId("/kym/1/x/proto", "hello") << "\n";
    std::cout << "frameId(\"t\",\"\")                    = " << frameId("t", "") << "\n";
    Frame f{ "", "t", 6, "hi" };
    std::cout << "encodeFrame({topic:t,hop:6,payload:hi}) = " << hexOf(encodeFrame(f)) << "\n";
    // encode is [01 06 0001 '74' 'hi'] = 0106000174 6869
    CHECK(hexOf(encodeFrame(f)) == "01060001746869", "encodeFrame golden bytes");
  }

  std::cout << (failures ? "\n" : "") << checks - failures << "/" << checks << " checks passed"
            << (failures ? "  <<< FAILURES >>>\n" : "  — ALL PASS\n");
  return failures ? 1 : 0;
}

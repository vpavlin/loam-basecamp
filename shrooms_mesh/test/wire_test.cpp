// Unit test for src/mesh_wire.hpp: frame codec (incl. split reads and garbage) and peer-list parsing.
#include "mesh_wire.hpp"
#include <cstdio>
using namespace shrooms_mesh;
static int pass = 0, fail = 0;
#define CHECK(c, m) do { if (c) ++pass; else { ++fail; std::printf("FAIL %d %s\n", __LINE__, m); } } while (0)

int main() {
    // round trip, two frames in one buffer, byte-by-byte feeding
    std::string a = encode("/scala/1/cal/json", std::string("\x00\x01sealed\xff", 9)), b = encode("t", "");
    { Reader r; std::vector<Frame> out; CHECK(r.feed((a + b).data(), a.size() + b.size(), out), "feed ok");
      CHECK(out.size() == 2 && out[0].topic == "/scala/1/cal/json" && out[0].payload == std::string("\x00\x01sealed\xff", 9) && out[1].topic == "t" && out[1].payload.empty(), "two frames"); }
    { Reader r; std::vector<Frame> out; std::string all = a + b;
      for (char c : all) CHECK(r.feed(&c, 1, out), "byte feed");
      CHECK(out.size() == 2 && r.buffered() == 0, "byte by byte"); }
    // a 5 MB payload passes, length over the cap fails, garbage fails and stays failed
    { std::string big(5 * 1024 * 1024, 'x'); std::string f = encode("t", big); Reader r; std::vector<Frame> out;
      CHECK(r.feed(f.data(), f.size(), out) && out.size() == 1 && out[0].payload.size() == big.size(), "5 MB frame"); }
    { std::string f = "\x7f\xff\xff\xff"; Reader r; std::vector<Frame> out; CHECK(!r.feed(f.data(), f.size(), out), "oversize length"); }
    { std::string f = "GET / HTTP/1.1\r\n\r\n"; Reader r; std::vector<Frame> out; CHECK(!r.feed(f.data(), f.size(), out) && r.failed(), "http garbage"); CHECK(!r.feed(a.data(), a.size(), out), "stays failed"); }
    { std::string f = a; f[4] = 2; Reader r; std::vector<Frame> out; CHECK(!r.feed(f.data(), f.size(), out), "bad version"); }
    { std::string f = a; f[5] = 0x7f; Reader r; std::vector<Frame> out; CHECK(!r.feed(f.data(), f.size(), out), "topic longer than frame"); }
    { std::string f = encode("", "x"); Reader r; std::vector<Frame> out; CHECK(!r.feed(f.data(), f.size(), out), "empty topic"); }
    // frameId = sha256(topic|0|bytes)[:16] hex, the MultiBearer's
    CHECK(frameId("t", "x").size() == 32 && frameId("t", "x") != frameId("t", "y") && frameId("ta", "") != frameId("t", "a"), "frameId");

    // peers: daemon status shape
    auto st = nlohmann::json::parse(R"({"overlay":"fdb0::1","prefix":"fdb0::/48",
      "meshes":[{"overlay":"fdb0::1","prefix":"fdb0::/48"},{"overlay":"fd11::1","prefix":"fd11::/48"}],
      "peers":[{"name":"atlas","mesh":"office","overlay":"fdb0::2","online":true},
               {"name":"duet","mesh":"office","overlay":"fdb0::3","online":false},
               {"name":"atlas","mesh":"home","overlay":"fd11::2","online":true},
               {"name":"me?","mesh":"office","overlay":"fdb0::1","online":true},
               {"name":"dup","mesh":"office","overlay":"fdb0::2","online":true},
               {"name":7,"overlay":5}, "junk"]})");
    PeerList pl = parsePeers(st);
    CHECK(pl.self.size() == 2 && pl.self[0] == "fdb0::1" && pl.self[1] == "fd11::1", "self addrs");
    CHECK(pl.prefixes.size() >= 2, "prefixes");
    CHECK(pl.peers.size() == 2 && pl.peers[0].addr == "fdb0::2" && pl.peers[1].addr == "fd11::2", "online peers, no self/dup/offline/junk");
    // agent /v1/peers shape: no online flag, no self
    PeerList ag = parsePeers(nlohmann::json::parse(R"({"peers":[{"name":"atlas","mesh":"office","overlay":"fdb0::2"}]})"));
    CHECK(ag.peers.size() == 1 && ag.self.empty(), "agent shape");
    CHECK(parsePeers(nlohmann::json::parse("[1,2]")).peers.empty(), "not an object");
    std::printf("%d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}

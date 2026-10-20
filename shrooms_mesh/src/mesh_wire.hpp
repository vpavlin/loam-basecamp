// mesh_wire.hpp — the shrooms_mesh wire format and peer-list parsing, Qt-free so a test can
// include it. The module (shrooms_mesh_impl.cpp) does the sockets.
//
// A connection carries frames, each:
//   u32 big-endian  length L (of what follows; 3 <= L <= kMaxFrame)
//   u8              version (1)
//   u16 big-endian  topic length T (1 <= T <= kMaxTopic)
//   T bytes         topic
//   L-3-T bytes     payload: the app's sealed bytes, opaque here
// Anything else (a bad version, a length out of range) means the peer isn't speaking this
// protocol: the reader reports an error and the module drops the connection.
#pragma once
#include <nlohmann/json.hpp>
#include <openssl/sha.h>

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace shrooms_mesh {

constexpr uint16_t kPort = 7392;                    // "loam" among the mesh's well-known ports
constexpr uint8_t kVersion = 1;
constexpr uint32_t kMaxFrame = 8u * 1024 * 1024;    // a catch-up reply is tens of KB; 8 MB is a hard ceiling
constexpr uint16_t kMaxTopic = 1024;

struct Frame {
    std::string topic, payload;
};

inline std::string encode(const std::string& topic, const std::string& payload) {
    const uint32_t len = 3 + (uint32_t)topic.size() + (uint32_t)payload.size();
    std::string out;
    out.reserve(4 + len);
    out.push_back(char(len >> 24)); out.push_back(char(len >> 16)); out.push_back(char(len >> 8)); out.push_back(char(len));
    out.push_back(char(kVersion));
    out.push_back(char(topic.size() >> 8)); out.push_back(char(topic.size()));
    out += topic;
    out += payload;
    return out;
}

// Incremental reader for one connection: feed() whatever arrived, take whole frames out.
class Reader {
public:
    // Returns false (and stays failed) on a protocol violation.
    bool feed(const char* data, size_t n, std::vector<Frame>& out) {
        if (m_failed) return false;
        m_buf.append(data, n);
        size_t pos = 0;
        while (m_buf.size() - pos >= 4) {
            const auto* p = reinterpret_cast<const unsigned char*>(m_buf.data() + pos);
            const uint32_t len = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
            if (len < 3 || len > kMaxFrame) { m_failed = true; return false; }
            if (m_buf.size() - pos - 4 < len) break;             // wait for the rest
            const auto* b = p + 4;
            const uint16_t tlen = uint16_t((b[1] << 8) | b[2]);
            if (b[0] != kVersion || tlen == 0 || tlen > kMaxTopic || 3u + tlen > len) { m_failed = true; return false; }
            Frame f;
            f.topic.assign(reinterpret_cast<const char*>(b + 3), tlen);
            f.payload.assign(reinterpret_cast<const char*>(b + 3 + tlen), len - 3 - tlen);
            out.push_back(std::move(f));
            pos += 4 + len;
        }
        m_buf.erase(0, pos);
        return true;
    }
    bool failed() const { return m_failed; }
    size_t buffered() const { return m_buf.size(); }
private:
    std::string m_buf;
    bool m_failed = false;
};

// The same frame identity loam_core's MultiBearer uses: sha256(topic ‖ 0x00 ‖ bytes)[:16], hex.
inline std::string frameId(const std::string& topic, const std::string& bytes) {
    std::string buf = topic;
    buf.push_back('\0');
    buf += bytes;
    unsigned char d[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(buf.data()), buf.size(), d);
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (int i = 0; i < 16; ++i) { out.push_back(hex[d[i] >> 4]); out.push_back(hex[d[i] & 15]); }
    return out;
}

struct Peer {
    std::string name, mesh, addr;   // addr: the peer's mesh (overlay) IPv6
};

struct PeerList {
    std::vector<Peer> peers;              // online peers, one per (mesh, address); never ourselves
    std::vector<std::string> self;        // our own mesh addresses (one per mesh) — what we listen on
    std::vector<std::string> prefixes;    // the meshes' prefixes, e.g. "fdb0:9afc:a5ef::/48"
};

// From the Shrooms daemon's GET /status ({meshes:[{overlay,prefix}], overlay, prefix,
// peers:[{name,mesh,overlay,online}]}) or the agent's GET /v1/peers ({peers:[{name,mesh,overlay}]},
// no "online": all listed). Typed reads only: this is another program's output.
inline PeerList parsePeers(const nlohmann::json& j) {
    PeerList out;
    if (!j.is_object()) return out;
    auto str = [](const nlohmann::json& o, const char* k) {
        return (o.is_object() && o.contains(k) && o[k].is_string()) ? o[k].get<std::string>() : std::string();
    };
    std::set<std::string> self;
    if (j.contains("meshes") && j["meshes"].is_array())
        for (const auto& m : j["meshes"]) {
            const std::string a = str(m, "overlay"), p = str(m, "prefix");
            if (!a.empty() && self.insert(a).second) out.self.push_back(a);
            if (!p.empty()) out.prefixes.push_back(p);
        }
    if (const std::string a = str(j, "overlay"); !a.empty() && self.insert(a).second) out.self.push_back(a);
    if (const std::string p = str(j, "prefix"); !p.empty()) out.prefixes.push_back(p);
    std::set<std::string> seen;
    if (j.contains("peers") && j["peers"].is_array())
        for (const auto& p : j["peers"]) {
            if (!p.is_object()) continue;
            if (p.contains("online") && p["online"].is_boolean() && !p["online"].get<bool>()) continue;
            Peer x{str(p, "name"), str(p, "mesh"), str(p, "overlay")};
            if (x.addr.empty() || self.count(x.addr) || !seen.insert(x.addr).second) continue;
            out.peers.push_back(x);
        }
    return out;
}

} // namespace shrooms_mesh

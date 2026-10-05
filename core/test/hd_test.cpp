// loam_hd.hpp must reproduce loam-keycard's TypeScript derivation bit-for-bit (ADR 0001).
//   g++ -std=c++17 -I core/src core/test/hd_test.cpp -lcrypto -o /tmp/hd_test && /tmp/hd_test core/test/hd_vectors.txt
// hd_vectors.txt: line 1 = mnemonic; then "name|appId|contextId|path|signPath|privHex|pubHex|address"
// (generated from loam-keycard test/vectors/hd.json; empty appId = the main identity).
#include "loam_hd.hpp"
#include <cstdio>
#include <fstream>
#include <sstream>

static std::vector<std::string> split(const std::string& s, char d) {
    std::vector<std::string> out; std::stringstream ss(s); std::string x;
    while (std::getline(ss, x, d)) out.push_back(x);
    if (!s.empty() && s.back() == d) out.push_back("");
    return out;
}

int main(int argc, char** argv) {
    int fails = 0;
    auto check = [&](bool c, const std::string& what) { printf("  %s %s\n", c ? "ok  " : "FAIL", what.c_str()); if (!c) ++fails; };

    // Official BIP39 vector (TREZOR passphrase) and BIP32 vector 1 (m/0' private key).
    const std::string abandon = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";
    check(loamhd::hex(loamhd::seedFromMnemonic(abandon, "TREZOR")) ==
          "c55257c360c07c72029aebc1b53c05ed0362ada38ead3e3e9efa3708e53495531f09a6987599d18264c1e1c92f2cf141630c7a3c4ab7c81b2f001698e7463b04",
          "BIP39 official vector");
    {
        loamhd::Bytes seed = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
        loamhd::ExtKey m0 = loamhd::hardenedChild(loamhd::masterFromSeed(seed), 0);
        check(m0.ok && loamhd::hex(m0.key) == "edb2e14f9ee77d26dd93b4ecede8d16ed408ce149b6cd80b0715a2d911a0afea",
              "BIP32 vector 1, m/0' private key");
    }

    std::ifstream f(argc > 1 ? argv[1] : "core/test/hd_vectors.txt");
    std::string mnemonic; std::getline(f, mnemonic);
    const loamhd::Bytes seed = loamhd::seedFromMnemonic(mnemonic);
    std::string line; int n = 0;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        auto v = split(line, '|');
        if (v.size() != 8) { check(false, "malformed vector line: " + line); continue; }
        const std::string &name = v[0], &app = v[1], &ctx = v[2];
        loamhd::Derived d = loamhd::derive(seed, app, ctx);
        check(d.ok && d.path == v[3], name + " path");
        check(loamhd::signPathFor(app, ctx) == v[4], name + " card sign path");
        check(loamhd::hex(d.priv) == v[5], name + " private key");
        check(d.pubHex == v[6], name + " public key");
        check(d.address == v[7], name + " address");
        ++n;
    }
    check(n >= 5, "read the shared vectors");
    printf(fails ? "HD: %d FAILED\n" : "HD GREEN\n", fails);
    return fails ? 1 : 0;
}

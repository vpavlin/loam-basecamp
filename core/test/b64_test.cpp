// loam::b64::decode must accept standard and URL-safe base64 (delivery >= 0.3.0 sends {"_bytes": url-safe}).
//   g++ -std=c++17 -I core/src core/test/b64_test.cpp -lcrypto -o /tmp/b64_test && /tmp/b64_test
#include "multibearer.hpp"
#include <cstdio>
int main() {
    int fails = 0;
    auto check = [&](bool c, const char* what) { printf("  %s %s\n", c ? "ok  " : "FAIL", what); if (!c) ++fails; };
    const std::string bytes = std::string("\xfb\xff\xbf\x10\x3e\x3f\x00\x01", 8);
    const std::string std64 = loam::b64::encode(bytes);
    std::string url = std64; for (auto& c : url) { if (c == '+') c = '-'; else if (c == '/') c = '_'; }
    check(std64.find_first_of("+/") != std::string::npos, "fixture exercises + and /");
    check(loam::b64::decode(std64) == bytes, "standard alphabet round-trips");
    check(loam::b64::decode(url) == bytes, "URL-safe alphabet decodes to the same bytes");
    std::string unpadded = url; while (!unpadded.empty() && unpadded.back() == '=') unpadded.pop_back();
    check(loam::b64::decode(unpadded) == bytes, "unpadded URL-safe decodes");
    printf(fails ? "B64: %d FAILED\n" : "B64 GREEN\n", fails);
    return fails ? 1 : 0;
}

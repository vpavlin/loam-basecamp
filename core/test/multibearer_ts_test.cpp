// multibearer_ts_test — frame dedup keeps the EARLIEST network timestamp: the first copy is delivered,
// a duplicate is delivered again only when it brings an earlier network ts (or the first had none).
//   g++ -std=c++17 -I../src multibearer_ts_test.cpp -lcrypto -o /tmp/mbt && /tmp/mbt
#include "multibearer.hpp"
#include <cassert>
#include <cstdio>

struct Fake : loam::IBearer {
    std::string n = "fake";
    const std::string &name() const override { return n; }
    void start() override {}
    void join(const std::string &) override {}
    void send(const std::string &, const std::string &) override {}
    bool ready() const override { return true; }
    std::string metricsJson() const override { return "{}"; }
    void rx(const std::string &topic, const std::string &bytes, int64_t ts, const char *src = "") {
        loam::FrameMeta m; m.ts = ts; m.source = src; m.bearer = n; onFrame(topic, "s", bytes, m);
    }
};

int main() {
    loam::MultiBearer mb;
    auto f = std::make_unique<Fake>(); Fake *b = f.get(); mb.add(std::move(f));
    struct D { int64_t ts; bool re; std::string src, id; };
    std::vector<D> got;
    mb.onReceived = [&](const std::string &, const std::string &, const std::string &, const loam::FrameMeta &m,
                        const std::string &id, bool re) { got.push_back({m.ts, re, m.source, id}); };

    b->rx("/t", "A", 2000, "live");          // first copy: delivered
    b->rx("/t", "A", 2000, "live");          // same ts: dropped
    b->rx("/t", "A", 3000, "live");          // later ts (a re-publish): dropped
    b->rx("/t", "A", 1000, "history");       // EARLIER ts from the store: delivered again
    b->rx("/t", "A", 0);                     // no network ts (BLE / channel event): dropped
    assert(got.size() == 2);
    assert(got[0].ts == 2000 && !got[0].re && got[0].src == "live");
    assert(got[1].ts == 1000 && got[1].re && got[1].src == "history" && got[1].id == got[0].id);

    b->rx("/t", "B", 0);                     // first copy has no network ts (BLE): delivered
    b->rx("/t", "B", 5000, "live");          // a network ts arrives later: delivered again
    b->rx("/t", "B", 6000, "live");          // later: dropped
    assert(got.size() == 4 && got[2].ts == 0 && !got[2].re && got[3].ts == 5000 && got[3].re);

    b->rx("/u", "A", 9000);                  // same bytes on another topic = another frame
    assert(got.size() == 5 && !got[4].re);
    std::puts("multibearer_ts_test: OK");
}

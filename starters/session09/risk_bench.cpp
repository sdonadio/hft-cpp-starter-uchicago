// risk_bench.cpp — Session 9 lab (labs/session09.md), optional Part D, D6: what does YOUR risk gate cost?
//
//   make risk-bench        (= g++ -std=c++17 -O2 -pthread -Iinclude -Itests ...)
//
// Times three gates on the same order stream (64 symbols, buys and sells,
// ~1 in 16 orders refused by the collar):
//   none   — the loop with no checks at all (the floor)
//   naive  — what people write first: std::string symbol, std::unordered_map,
//            a std::mutex, a double-precision collar
//   yours  — include/risk_gate.hpp (check + on_done)
// A laptop clock can't time one ~5 ns call, so each sample is a batch of 64
// calls divided by 64 (the same trick as the Session 7 lab's lock_tail.cpp). The tail
// percentiles are therefore per-batch averages: a single slow call is diluted
// 64x. Say so when you report them.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "risk_gate.hpp"
#include "bench.hpp"

namespace {

constexpr int kBatch = 64;
constexpr int kSamples = 200'000;            // 12.8 M calls per gate

struct NaiveGate {                           // deliberately ordinary code
    struct State { double ref = 0; long pos = 0, open_buy = 0, open_sell = 0; };
    std::unordered_map<std::string, State> by_sym;
    std::mutex mu;
    bool check(const std::string& sym, bool buy, int qty, double px) {
        std::lock_guard<std::mutex> lock(mu);
        State& s = by_sym[sym];
        if (qty <= 0 || qty > 500) return false;
        if (s.ref <= 0 || std::fabs(px - s.ref) / s.ref > 0.08) return false;
        long worst = buy ? s.pos + s.open_buy + qty : s.pos - s.open_sell - qty;
        if (std::labs(worst) > 1200) return false;
        (buy ? s.open_buy : s.open_sell) += qty;
        return true;
    }
    void done(const std::string& sym, bool buy, int qty) {
        std::lock_guard<std::mutex> lock(mu);
        State& s = by_sym[sym];
        (buy ? s.open_buy : s.open_sell) -= qty;
    }
};

struct Stats { double p50, p99, p999, max; };
Stats summarize(std::vector<double>& v) {
    std::sort(v.begin(), v.end());
    auto at = [&](double q) { return v[std::min(v.size() - 1, size_t(q * double(v.size())))]; };
    return {at(0.50), at(0.99), at(0.999), v.back()};
}

template <class F>
Stats run(F&& one_call) {
    std::vector<double> ns;
    ns.reserve(kSamples);
    for (int i = 0; i < kSamples / 10; ++i) one_call(i);          // warm up
    for (int i = 0; i < kSamples; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        for (int k = 0; k < kBatch; ++k) one_call(i * kBatch + k);
        auto t1 = std::chrono::steady_clock::now();
        ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() / kBatch);
    }
    return summarize(ns);
}

void print(const char* name, Stats s) {
    std::printf("%-6s p50 %6.2f  p99 %6.2f  p99.9 %7.2f  max %9.2f  ns/check\n",
                name, s.p50, s.p99, s.p999, s.max);
}

}  // namespace

int main() {
    // The order stream, built once: no allocation inside the timed loops.
    struct Pre { uint32_t sym; bool buy; int64_t px; };
    std::vector<Pre> stream(4096);
    std::vector<std::string> names(RiskGate::kMaxSymbols);
    for (uint32_t s = 0; s < RiskGate::kMaxSymbols; ++s) names[s] = "SYM" + std::to_string(s);
    for (size_t i = 0; i < stream.size(); ++i) {
        uint32_t sym = uint32_t((i * 37) % RiskGate::kMaxSymbols);
        int64_t ref = 10'000 + sym;
        int64_t px = (i % 16 == 0) ? ref * 2 : ref - 5 + int64_t(i % 11);   // 1 in 16 outside the collar
        stream[i] = {sym, (i & 1) != 0, px};
    }
    const size_t mask = stream.size() - 1;

    long sink = 0;
    Stats none = run([&](int i) {
        const Pre& p = stream[size_t(i) & mask];
        sink += p.px;
        doNotOptimize(sink);
    });

    NaiveGate naive;
    for (uint32_t s = 0; s < RiskGate::kMaxSymbols; ++s) naive.by_sym[names[s]].ref = (10'000 + s) / 100.0;
    Stats nv = run([&](int i) {
        const Pre& p = stream[size_t(i) & mask];
        const std::string& sym = names[p.sym];
        bool ok = naive.check(sym, p.buy, 100, double(p.px) / 100.0);
        if (ok) naive.done(sym, p.buy, 100);
        doNotOptimize(ok);
    });

    RiskLimits lim;
    lim.msgs_per_sec = 1e12; lim.burst = 1e12; lim.dup_window_ns = 0;   // measure the checks, not the throttle
    RiskGate gate(lim);
    for (uint32_t s = 0; s < RiskGate::kMaxSymbols; ++s) gate.set_reference(s, 10'000 + s);
    uint64_t ts = 1;
    Stats yours = run([&](int i) {
        const Pre& p = stream[size_t(i) & mask];
        Order o{p.sym, p.buy ? Side::Buy : Side::Sell, 100, p.px, ts++};
        Risk r = gate.check(o);
        if (r == Risk::Ok) gate.on_done(o.sym, o.side, o.qty);
        doNotOptimize(r);
    });

    print("none", none);
    print("naive", nv);
    print("yours", yours);
    std::printf("(batches of %d calls; %d samples each)\n", kBatch, kSamples);
    return 0;
}

// bench_alloc.cpp — Session 6 lab: new/delete vs YOUR Pool (include/pool.hpp).
// Build:  make bench-alloc      (or: g++ -std=c++17 -O2 -Iinclude -Itests \
//                                  starters/session06/bench_alloc.cpp -o /tmp/bench_alloc)
// Prints the mean ns per alloc+free pair AND the per-op tail (p50 / p99 / p99.9 / max)
// for the same workload: a live set of 256 orders, one alloc + one free per step,
// in a scrambled order so the heap cannot just hand back the last block.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <new>
#include <vector>
#include "pool.hpp"
#include "bench.hpp"

struct Order { std::uint64_t id; double px; std::uint32_t qty; char side; char pad[43]; };  // 64 B

// The tail is measured in BATCHES of 16 steps: a laptop clock cannot time one
// 2 ns operation (macOS steady_clock and the arm64 timer both tick in ~42 ns
// steps, even on an M4 whose cntfrq_el0 reports 1 GHz), but it can time 16.
static constexpr int BATCH = 16;

static constexpr int LIVE = 256, STEPS = 2'000'000, SAMPLES = 100'000;

template <class Alloc, class Free>
static void run(const char* name, Alloc&& alloc, Free&& release) {
    std::vector<Order*> live(LIVE, nullptr);
    std::uint64_t x = 88172645463325252ull;                 // xorshift: no rand() on the path
    auto next = [&] { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x; };
    for (auto& p : live) p = alloc();
    auto step = [&] {                                       // free one random slot, refill it
        Order*& p = live[next() & (LIVE - 1)];
        release(p);
        p = alloc();
        p->id = x; doNotOptimize(p);
    };
    for (int i = 0; i < STEPS / 10; ++i) step();            // warm up
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < STEPS; ++i) step();
    auto t1 = std::chrono::steady_clock::now();
    double mean = std::chrono::duration<double, std::nano>(t1 - t0).count() / STEPS;

    std::vector<double> ns(SAMPLES);                        // per-batch time / BATCH
    for (int i = 0; i < SAMPLES; ++i) {
        auto a = std::chrono::steady_clock::now();
        for (int j = 0; j < BATCH; ++j) step();
        auto b = std::chrono::steady_clock::now();
        ns[i] = std::chrono::duration<double, std::nano>(b - a).count() / BATCH;
    }
    std::sort(ns.begin(), ns.end());
    auto pct = [&](double q) { return ns[(std::size_t)(q * (SAMPLES - 1))]; };
    std::printf("%-11s mean %6.2f ns | per-pair, batches of %d: p50 %6.2f  p99 %6.2f  p99.9 %7.2f  max %8.1f ns\n",
                name, mean, BATCH, pct(0.50), pct(0.99), pct(0.999), ns.back());
    for (auto p : live) release(p);
}

int main() {
    Pool pool(sizeof(Order), LIVE + 1);
    if (!pool.alloc()) { std::puts("include/pool.hpp returns nullptr - implement it first (make pool)."); return 1; }
    run("new/delete", [] { return new Order{}; }, [](Order* p) { delete p; });
    run("Pool", [&] { return new (pool.alloc()) Order{}; },
                [&](Order* p) { p->~Order(); pool.free(p); });
    std::puts("(batch columns include one clock read per 16 pairs - compare the rows to each other."
              " The max is the number to explain.)");
}

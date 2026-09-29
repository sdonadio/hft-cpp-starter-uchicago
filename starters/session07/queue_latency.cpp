// queue_latency.cpp — Session 7 lab, step 7. One-way latency through a queue:
// the producer stamps each item and pushes it at a steady rate; the consumer
// pops it and records (now - stamp). Two queues: std::mutex + std::deque, and
// YOUR lock-free SPSCRing (include/spsc_ring.hpp).
//   g++ -std=c++17 -O2 -pthread -Iinclude starters/session07/queue_latency.cpp -o /tmp/qlat && /tmp/qlat
// Latencies are cross-thread steady_clock deltas: on macOS that clock ticks in
// ~42 ns steps, so read p50 as "under ~100 ns" and look hard at the tail.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include "spsc_ring.hpp"

static constexpr int N = 400'000;
static inline std::uint64_t now() {
    return (std::uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
static void pace(std::uint64_t ns) { auto t = now() + ns; while (now() < t) {} }

struct MutexQueue {                                  // the obvious, correct version
    std::mutex m; std::deque<std::uint64_t> q;
    bool push(std::uint64_t v) { std::lock_guard<std::mutex> g(m); q.push_back(v); return true; }
    bool pop(std::uint64_t& v) { std::lock_guard<std::mutex> g(m);
                                 if (q.empty()) return false; v = q.front(); q.pop_front(); return true; }
};

template <class Q>
static void run(const char* name, Q& q) {
    std::vector<std::uint32_t> lat(N);
    std::thread consumer([&] {
        for (int i = 0; i < N; ++i) {
            std::uint64_t ts;
            while (!q.pop(ts)) {}
            lat[i] = (std::uint32_t)(now() - ts);
        }
    });
    for (int i = 0; i < N; ++i) {                    // ~one item every 250 ns
        while (!q.push(now())) {}
        pace(250);
    }
    consumer.join();
    std::sort(lat.begin(), lat.end());
    auto pct = [&](double p) { return lat[(std::size_t)(p * (N - 1))]; };
    std::printf("%-10s p50 %6u  p99 %7u  p99.9 %8u  max %9u  ns\n",
                name, pct(.5), pct(.99), pct(.999), lat.back());
}

int main() {
    SPSCRing probe(8);
    if (!probe.push(1)) { std::puts("include/spsc_ring.hpp is still the stub - make spsc first."); return 1; }
    MutexQueue mq; run("mutex+deque", mq);
    SPSCRing r(1 << 12); run("SPSCRing", r);
}

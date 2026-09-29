// lock_tail.cpp — Session 7 lab, step 4. Same work, three ways, under contention:
// a std::mutex, a std::atomic fetch_add, and no sharing at all. Prints the mean
// AND the tail of the time per increment, measured in batches of 64 (a laptop
// clock cannot time one 5 ns operation).
//   g++ -std=c++17 -O2 -pthread starters/session07/lock_tail.cpp -o /tmp/lock_tail && /tmp/lock_tail
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

static constexpr int THREADS = 4, BATCHES = 50'000, BATCH = 64;

template <class Op>
static void run(const char* name, Op op) {
    std::vector<std::vector<double>> per(THREADS, std::vector<double>(BATCHES));
    std::atomic<int> go{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < THREADS; ++t)
        ts.emplace_back([&, t] {
            go.fetch_add(1); while (go.load() < THREADS) {}          // start together
            for (int b = 0; b < BATCHES; ++b) {
                auto a = std::chrono::steady_clock::now();
                for (int i = 0; i < BATCH; ++i) op(t);
                auto z = std::chrono::steady_clock::now();
                per[t][b] = std::chrono::duration<double, std::nano>(z - a).count() / BATCH;
            }
        });
    for (auto& th : ts) th.join();
    std::vector<double> all;
    for (auto& v : per) all.insert(all.end(), v.begin(), v.end());
    std::sort(all.begin(), all.end());
    double sum = 0; for (double x : all) sum += x;
    auto pct = [&](double q) { return all[(std::size_t)(q * (all.size() - 1))]; };
    std::printf("%-8s mean %7.1f  p50 %7.1f  p99 %8.1f  p99.9 %9.1f  max %10.1f  ns/op\n",
                name, sum / all.size(), pct(.5), pct(.99), pct(.999), all.back());
}

int main() {
    std::mutex m; long locked = 0;
    std::atomic<long> atom{0};
    struct alignas(64) Local { long v = 0; };
    std::vector<Local> local(THREADS);
    std::printf("%d threads contending on one counter, %d x %d increments each\n",
                THREADS, BATCHES, BATCH);
    run("mutex",  [&](int)   { std::lock_guard<std::mutex> g(m); ++locked; });
    run("atomic", [&](int)   { atom.fetch_add(1, std::memory_order_relaxed); });
    run("private",[&](int t) { local[t].v++; asm volatile("" ::: "memory"); });
    std::printf("totals: mutex %ld  atomic %ld\n", locked, atom.load());
}

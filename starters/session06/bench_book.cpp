// bench_book.cpp — Session 6 lab: YOUR flat Book / SymMap (include/order_book.hpp)
// vs the std:: containers you would reach for first.
// Build:  make bench-book
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <unordered_map>
#include "order_book.hpp"
#include "bench.hpp"

int main() {
    // same 2,000 resting orders on both books: prices 100.00 .. 100.49, both sides
    Book flat;
    std::map<int, std::uint32_t> bids, asks;                    // tick -> qty
    struct Rec { char side; int tick; std::uint32_t qty; };
    std::unordered_map<std::uint64_t, Rec> ids;                 // same id index Book keeps
    for (std::uint64_t i = 0; i < 2000; ++i) {
        double px = 100.0 + ((i * 7) % 50) * 0.01;
        std::uint32_t q = 1 + (i % 9);
        char side = (i & 1) ? 'B' : 'S';
        flat.add(i, side, px, q);
        int t = (int)(px / 0.01 + 0.5);
        (side == 'B' ? bids : asks)[t] += q;
        ids[i] = {side, t, q};
    }
    if (flat.best_bid() == 0.0) { std::puts("include/order_book.hpp is still the stub - make book first."); return 1; }

    double f = ns_per_op([&] { double x = flat.best_bid() + flat.best_ask(); doNotOptimize(x); }, 5'000'000);
    double m = ns_per_op([&] { double x = (bids.rbegin()->first + asks.begin()->first) * 0.01;
                               doNotOptimize(x); }, 5'000'000);
    // add + cancel churn at the touch (the write path)
    std::uint64_t id = 1'000'000;
    double fa = ns_per_op([&] { flat.add(id, 'B', 100.20, 1); flat.cancel(id); ++id; }, 1'000'000);
    std::uint64_t id2 = 1'000'000;
    double ma = ns_per_op([&] {                                 // same work: level + id index
        bids[10020] += 1; ids[id2] = {'B', 10020, 1};
        auto it = ids.find(id2); auto lv = bids.find(it->second.tick);
        if ((lv->second -= it->second.qty) == 0) bids.erase(lv);
        ids.erase(it); ++id2; }, 1'000'000);

    SymMap sm; std::unordered_map<std::string, std::uint64_t> um;
    const char* syms[] = {"AAPL","MSFT","NVDA","TSLA","AMZN","GOOGL","META","NFLX"};
    for (std::uint64_t i = 0; i < 8; ++i) { sm.put(syms[i], i); um[syms[i]] = i; }
    int k = 0;
    double s1 = ns_per_op([&] { auto v = sm.get(syms[(k++) & 7]); doNotOptimize(v); }, 5'000'000);
    double s2 = ns_per_op([&] { auto v = um.find(syms[(k++) & 7])->second; doNotOptimize(v); }, 5'000'000);

    std::printf("BBO read     flat %6.2f ns   std::map %6.2f ns\n", f, m);
    std::printf("add+cancel   flat %6.2f ns   std::map %6.2f ns  (both keep an unordered_map id index)\n", fa, ma);
    std::printf("symbol->id   SymMap %6.2f ns   unordered_map<string> %6.2f ns (builds a std::string per call)\n", s1, s2);
}

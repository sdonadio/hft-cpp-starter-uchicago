// bench_containers.cpp — Session 6 lab, Part B (step B5): which container answers
// which order-book question, and what does each answer cost?
//
//   Q1  look up an order by id      -> cancel(id)
//   Q2  look up a level by price    -> add(order)
//   Q3  know the best prices        -> best_bid() / best_ask()
//
// Four books, same workload, same fixed-seed RNG:
//   1. vector  : one std::vector<Order>, unsorted. Q1 and Q3 are linear scans,
//                erase in the middle shifts the tail (O(n)), add is push_back.
//   2. list    : one std::list per side, kept sorted best-first. Q3 is front(),
//                insert/erase are O(1) once you hold the iterator, but FINDING the
//                spot (Q2) or the order (Q1) is a pointer-chasing scan.
//   3. map     : std::map<tick, Level> per side (Q2 find O(log n), Q3 begin()/rbegin(),
//                one node allocation per new level) + std::unordered_map<id, Rec>
//                holding the level's map iterator (Q1 O(1) average, chained nodes).
//   4. flat    : a price-indexed array (a band against a base tick) with cached best
//                slots + an open-addressing id map (power-of-two mask, linear probing,
//                tombstones, in-place rebuild). No allocation after construction.
//
// For N = 64, 1,024 and 16,384 resting orders it prints ns/op for add, cancel-by-id
// and best_bid. Each round cancels a batch of random live orders, adds as many fresh
// ones (so the book stays near N), and reads best_bid once per op; each batch is timed
// with steady_clock and the clock-read overhead is subtracted. Standard library only.
//
// Build & run (about 5 s):
//   c++ -std=c++20 -O2 starters/session06/bench_containers.cpp -o /tmp/bench_containers && /tmp/bench_containers
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <random>
#include <unordered_map>
#include <vector>

using Tick = std::int32_t;   // prices are integer ticks ($0.01)
using Id   = std::uint64_t;
using Qty  = std::uint32_t;

struct Order { Id id; Tick px; Qty qty; char side; };   // side 'B' = bid, 'S' = ask

constexpr Tick MID   = 10000;   // $100.00
constexpr int  DEPTH = 100;     // the workload rests orders 1..100 ticks from MID

// ── 1. The naive vector book ─────────────────────────────────────────────────
struct VecBook {
    std::vector<Order> v;                         // arrival order, no index at all
    explicit VecBook(int) {}
    void add(const Order& o) { v.push_back(o); }  // amortized O(1); a reallocation now and then
    void cancel(Id id) {                          // Q1: linear scan, then O(n) shift
        auto it = std::find_if(v.begin(), v.end(), [id](const Order& o) { return o.id == id; });
        if (it != v.end()) v.erase(it);
    }
    Tick best_bid() const {                       // Q3: scan everything
        Tick b = 0;
        for (const Order& o : v) if (o.side == 'B' && o.px > b) b = o.px;
        return b;
    }
    Tick best_ask() const {
        Tick a = 0;
        for (const Order& o : v) if (o.side == 'S' && (a == 0 || o.px < a)) a = o.px;
        return a;
    }
};

// ── 2. std::list, sorted best-first, iterators stable ────────────────────────
struct ListBook {
    std::list<Order> bids, asks;                  // equal prices keep arrival (time) order
    explicit ListBook(int) {}
    std::list<Order>::iterator add(const Order& o) {   // Q2: walk to the price, then O(1) insert
        auto& l = o.side == 'B' ? bids : asks;
        auto it = l.begin();
        if (o.side == 'B') while (it != l.end() && it->px >= o.px) ++it;
        else               while (it != l.end() && it->px <= o.px) ++it;
        return l.insert(it, o);                   // the returned iterator stays valid until erased
    }
    void cancel(Id id) {                          // Q1: pointer-chasing scan, then O(1) erase
        for (auto* l : {&bids, &asks})
            for (auto it = l->begin(); it != l->end(); ++it)
                if (it->id == id) { l->erase(it); return; }
    }
    Tick best_bid() const { return bids.empty() ? 0 : bids.front().px; }
    Tick best_ask() const { return asks.empty() ? 0 : asks.front().px; }
};

// ── 3. std::map<price, Level> + std::unordered_map<id, Rec> ──────────────────
struct MapBook {
    struct Level { std::uint64_t qty = 0; std::uint32_t n = 0; };
    using Side = std::map<Tick, Level>;           // ascending: bids' best is rbegin(), asks' is begin()
    struct Rec { char side; Qty qty; Side::iterator lvl; };   // map iterators are stable
    Side bids, asks;
    std::unordered_map<Id, Rec> ids;
    explicit MapBook(int n) { ids.reserve(static_cast<std::size_t>(2 * n)); }  // no rehash spike in the loop
    void add(const Order& o) {                    // Q2: O(log n) find, a node allocation per NEW level
        Side& s = o.side == 'B' ? bids : asks;
        auto it = s.try_emplace(o.px).first;
        it->second.qty += o.qty;
        ++it->second.n;
        ids.emplace(o.id, Rec{o.side, o.qty, it});
    }
    void cancel(Id id) {                          // Q1: O(1) average hash lookup, no scan
        auto f = ids.find(id);
        if (f == ids.end()) return;
        Rec r = f->second;
        ids.erase(f);
        r.lvl->second.qty -= r.qty;
        if (--r.lvl->second.n == 0) (r.side == 'B' ? bids : asks).erase(r.lvl);
    }
    Tick best_bid() const { return bids.empty() ? 0 : bids.rbegin()->first; }   // Q3
    Tick best_ask() const { return asks.empty() ? 0 : asks.begin()->first; }
};

// ── 4. Flat price-indexed band + open-addressing id map ──────────────────────
struct IdMap {                                    // id -> Rec; linear probing, tombstones
    struct Rec  { Qty qty; std::int16_t slot; char side; std::uint8_t st; };   // slot < 4,096 fits in 16 bits
    struct Cell { Id key; Rec r; };               // 16 bytes; r.st: 0 empty, 1 live, 2 tombstone
    static_assert(sizeof(Cell) == 16, "four cells per 64-byte line");
    std::vector<Cell> t, spare;                   // both sized once; rebuild swaps them
    std::size_t mask = 0, used = 0;               // used = live + tombstones

    explicit IdMap(int n) {
        std::size_t c = 16;
        while (c < 8 * static_cast<std::size_t>(n)) c <<= 1;  // power of two: mask, not %
        t.assign(c, Cell{});
        spare.assign(c, Cell{});
        mask = c - 1;
    }
    static std::size_t h(Id k) {                  // murmur3 finalizer: spreads sequential ids
        k ^= k >> 33; k *= 0xff51afd7ed558ccdULL; k ^= k >> 33;
        return static_cast<std::size_t>(k);
    }
    void insert_fresh(Id k, const Rec& r) {       // no tombstones present (used after rebuild)
        std::size_t i = h(k) & mask;
        while (t[i].r.st != 0) i = (i + 1) & mask;
        t[i] = Cell{k, r}; t[i].r.st = 1;
        ++used;
    }
    void rebuild() {                              // drop tombstones: every ~3N puts, O(cap), no allocation
        std::swap(t, spare);
        std::fill(t.begin(), t.end(), Cell{});
        used = 0;
        for (const Cell& c : spare) if (c.r.st == 1) insert_fresh(c.key, c.r);
    }
    void put(Id k, const Rec& r) {                // ids are unique in this workload
        if (2 * (used + 1) > t.size()) rebuild();  // live + tombstones <= 1/2: short probes
        std::size_t i = h(k) & mask, tomb = SIZE_MAX;
        for (;; i = (i + 1) & mask) {
            Cell& c = t[i];
            if (c.r.st == 0) {
                Cell& dst = tomb == SIZE_MAX ? c : t[tomb];      // reuse the first tombstone
                if (tomb == SIZE_MAX) ++used;
                dst = Cell{k, r};
                dst.r.st = 1;
                return;
            }
            if (c.r.st == 2 && tomb == SIZE_MAX) tomb = i;
            else if (c.r.st == 1 && c.key == k) { c.r = r; c.r.st = 1; return; }
        }
    }
    bool take(Id k, Rec& out) {                   // find + erase (leaves a tombstone)
        for (std::size_t i = h(k) & mask;; i = (i + 1) & mask) {
            Cell& c = t[i];
            if (c.r.st == 0) return false;
            if (c.r.st == 1 && c.key == k) { out = c.r; c.r.st = 2; return true; }
        }
    }
};

struct FlatBook {
    static constexpr int NLEV = 4096;             // a band of 4,096 ticks around MID
    std::array<std::uint64_t, NLEV> bidq{}, askq{};
    Tick base = MID - NLEV / 2;                   // absolute tick of slot 0
    int bb = -1, ba = -1;                         // cached best slots (-1 = side empty)
    IdMap ids;
    explicit FlatBook(int n) : ids(n) {}
    static std::size_t u(int i) { return static_cast<std::size_t>(i); }
    void add(const Order& o) {                    // Q2: price -> slot is a subtraction
        const int i = o.px - base;
        if (i < 0 || i >= NLEV) return;           // outside the band: refuse, don't scribble
        ids.put(o.id, IdMap::Rec{o.qty, static_cast<std::int16_t>(i), o.side, 0});
        if (o.side == 'B') { bidq[u(i)] += o.qty; if (i > bb) bb = i; }
        else               { askq[u(i)] += o.qty; if (ba < 0 || i < ba) ba = i; }
    }
    void cancel(Id id) {                          // Q1: one probe sequence; scan only if the touch empties
        IdMap::Rec r{};
        if (!ids.take(id, r)) return;
        if (r.side == 'B') {
            if ((bidq[u(r.slot)] -= r.qty) == 0 && r.slot == bb)
                while (bb >= 0 && bidq[u(bb)] == 0) --bb;
        } else {
            if ((askq[u(r.slot)] -= r.qty) == 0 && r.slot == ba) {
                while (ba < NLEV && askq[u(ba)] == 0) ++ba;
                if (ba >= NLEV) ba = -1;
            }
        }
    }
    Tick best_bid() const { return bb < 0 ? 0 : bb + base; }   // Q3: one load
    Tick best_ask() const { return ba < 0 ? 0 : ba + base; }
};

// ── Driver ────────────────────────────────────────────────────────────────────
using Clock = std::chrono::steady_clock;
static volatile std::uint64_t g_sink;             // results go here so nothing is optimized away

static double clock_overhead_ns() {               // cost of one back-to-back now() pair
    const int R = 200000;
    auto a = Clock::now();
    for (int i = 0; i < R; ++i) { auto x = Clock::now(); auto y = Clock::now(); g_sink = g_sink + static_cast<std::uint64_t>((y - x).count()); }
    auto b = Clock::now();
    return std::chrono::duration<double, std::nano>(b - a).count() / R / 2.0;   // ~one now()
}

struct Row { double add, cancel, best; };

struct Gen {                                      // fixed seed, prices on the tick grid in a band
    std::mt19937_64 rng;
    explicit Gen(std::uint64_t seed) : rng(seed) {}
    Order make(Id id) {
        const char side = (rng() & 1) ? 'B' : 'S';
        const int off = 1 + static_cast<int>(rng() % DEPTH);
        const Qty q = 1 + static_cast<Qty>(rng() % 9);
        return Order{id, side == 'B' ? MID - off : MID + off, q, side};
    }
};

template <class Book>
static Row run(int N, double clk) {
    Gen g(12345u + static_cast<unsigned>(N));
    auto owner = std::make_unique<Book>(N);
    Book* volatile bp = owner.get();              // re-read every call: best_bid() can't be hoisted
    std::vector<Id> live;
    live.reserve(static_cast<std::size_t>(N));
    Id next = 1;
    for (int i = 0; i < N; ++i) { Order o = g.make(next++); bp->add(o); live.push_back(o.id); }

    const int B = std::clamp(N / 4, 16, 256);     // cancels and adds per timed batch
    constexpr int READS = 256;                    // best_bid reads per timed batch
    std::vector<Id> victims(static_cast<std::size_t>(B));
    std::vector<Order> fresh(static_cast<std::size_t>(B));
    std::uint64_t sink = 0;
    double tc = 0, ta = 0, tb = 0;
    long rounds = 0;

    auto round = [&](bool keep) {
        for (int k = 0; k < B; ++k) {             // untimed: pick victims, draw fresh orders
            std::size_t j = static_cast<std::size_t>(g.rng() % live.size());
            victims[static_cast<std::size_t>(k)] = live[j];
            live[j] = live.back();
            live.pop_back();
            fresh[static_cast<std::size_t>(k)] = g.make(next++);
        }
        auto t0 = Clock::now();
        for (int k = 0; k < B; ++k) bp->cancel(victims[static_cast<std::size_t>(k)]);
        auto t1 = Clock::now();
        for (int k = 0; k < B; ++k) bp->add(fresh[static_cast<std::size_t>(k)]);
        auto t2 = Clock::now();
        for (int k = 0; k < READS; ++k) sink += static_cast<std::uint64_t>(bp->best_bid());
        auto t3 = Clock::now();
        for (int k = 0; k < B; ++k) live.push_back(fresh[static_cast<std::size_t>(k)].id);
        if (keep) {
            tc += std::chrono::duration<double, std::nano>(t1 - t0).count() - clk;
            ta += std::chrono::duration<double, std::nano>(t2 - t1).count() - clk;
            tb += (std::chrono::duration<double, std::nano>(t3 - t2).count() - clk) * B / READS;
            ++rounds;
        }
    };

    const auto warm_end = Clock::now() + std::chrono::milliseconds(60);   // warm-up: caches, allocator, branch predictors
    while (Clock::now() < warm_end) round(false);
    const auto end = Clock::now() + std::chrono::milliseconds(300);       // measure for ~0.3 s, at least 20 rounds
    while (rounds < 20 || Clock::now() < end) round(true);

    g_sink = g_sink + sink;
    const double ops = static_cast<double>(rounds) * B;
    return Row{std::max(0.0, ta / ops), std::max(0.0, tc / ops), std::max(0.0, tb / ops)};
}

static bool cross_check() {                       // all four books must agree on the touch
    const int N = 1000;
    Gen g(777);
    VecBook v(N); ListBook l(N); MapBook m(N); FlatBook f(N);
    std::vector<Id> live;
    Id next = 1;
    for (int step = 0; step < 20000; ++step) {
        if (!live.empty() && (live.size() >= static_cast<std::size_t>(N) || (g.rng() & 1))) {
            std::size_t j = static_cast<std::size_t>(g.rng() % live.size());
            Id id = live[j]; live[j] = live.back(); live.pop_back();
            v.cancel(id); l.cancel(id); m.cancel(id); f.cancel(id);
        } else {
            Order o = g.make(next++);
            v.add(o); l.add(o); m.add(o); f.add(o);
            live.push_back(o.id);
        }
        const Tick b = v.best_bid(), a = v.best_ask();
        if (l.best_bid() != b || m.best_bid() != b || f.best_bid() != b ||
            l.best_ask() != a || m.best_ask() != a || f.best_ask() != a) {
            std::printf("MISMATCH at step %d\n", step);
            return false;
        }
    }
    return true;
}

int main() {
    if (!cross_check()) return 1;
    std::puts("cross-check: vector, list, map and flat books agree on best bid/ask over 20,000 random ops");
    const double clk = clock_overhead_ns();
    std::printf("steady_clock::now() overhead %.1f ns (subtracted per batch)\n", clk);

    struct Spec { const char* name; Row (*fn)(int, double); };
    const Spec specs[] = {
        {"vector (unsorted, scans)",         &run<VecBook>},
        {"list (sorted, scan to find)",      &run<ListBook>},
        {"map<px,Level> + unordered_map<id>", &run<MapBook>},
        {"flat band + open-addressing ids",  &run<FlatBook>},
    };
    for (int N : {64, 1024, 16384}) {
        std::printf("\nN = %d resting orders (batches of %d cancels, %d adds, 256 best_bid reads)\n", N,
                    std::clamp(N / 4, 16, 256), std::clamp(N / 4, 16, 256));
        std::printf("  %-36s %10s %12s %10s   ns/op\n", "structure", "add", "cancel(id)", "best_bid");
        for (const Spec& s : specs) {
            Row r = s.fn(N, clk);
            std::printf("  %-36s %10.1f %12.1f %10.1f\n", s.name, r.add, r.cancel, r.best);
        }
    }
    return g_sink == 42 ? 2 : 0;                  // never true in practice; keeps g_sink observable
}

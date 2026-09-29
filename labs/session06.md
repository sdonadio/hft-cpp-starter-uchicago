# Session 6 Lab — Memory Pools & the Order Book

**Format:** in class, guided, in breakout pairs (~45 min), plus a take-home tail.
**Repo:** this starter. **Deck:** Session 6, *Memory Pools & the Order Book*.

## Goal
Build the two headers the hot path lives on and measure them:

1. **`include/pool.hpp`**: a fixed-size object pool. One slab allocated once,
   an intrusive free-list, O(1) `alloc()` / `free()`. You construct objects in
   it with placement `new` and destroy them with an explicit destructor call.
2. **`include/order_book.hpp`**: a flat, price-indexed `Book` with O(1)
   `best_bid()` / `best_ask()`, indexed against a movable **band**, plus an
   open-addressing `SymMap` (symbol → id).

Then you measure both against the standard library: `make bench-alloc` and `make bench-book`.

This lab covers the autograded part of **HW 6 — A memory pool & a fast order
book** (10 pts, due **Thu Nov 12, 10:59 pm CT**). It is also the base for
**Project Phase 2 — The Local Order Book**, due **this Thursday, Nov 5,
10:59 pm CT**.

| Part | What | Where | In class? |
|---|---|---|---|
| A | The pool | `include/pool.hpp`, `make pool` | yes |
| B | The book + symbol map | `include/order_book.hpp`, `make book` | yes |
| C | Measure both | `make bench-alloc`, `make bench-book` | yes, if time |
| D | Arena, HW 6 driver, ring + Welford, Phase 2 wiring | your files | take-home |

## Setup
```bash
cd hft-cpp-starter-uchicago
make pool      # builds tests/pool_test.cpp against include/pool.hpp: RED on the stub
make book      # builds tests/book_test.cpp against include/order_book.hpp: RED on the stub
```
Keep the header you edit in one pane and the test file in the other. **The test
file is the contract** (read it, don't edit it). All the code below compiles
with the Makefile's `-std=c++17 -O2` and with `-std=c++20`.

---

## Part A — The pool (`include/pool.hpp`)

The contract, from `tests/pool_test.cpp`:
```cpp
struct Pool { Pool(std::size_t obj_size, std::size_t capacity);
              void* alloc(); void free(void*); };
```
| test | what it demands |
|---|---|
| `pool_basic` | `capacity` allocations, all non-null, **distinct** and **non-overlapping**. It writes a tag into every slot and reads them all back |
| `pool_full` | the pool fills with real slots first, and **only then** does `alloc()` return `nullptr` |
| `pool_reuse` | with the pool full, `free(a)` then `alloc()` must return **exactly `a`** |
| `pool_pattern` | 100k fill/drain rounds, nothing leaked, no slot handed out twice |

Then it prints `METRIC|pool_ns_per_op`.

> **Don't trust the metric yet.** The stub's `alloc()` returns `nullptr`, and
> timing "allocate nothing" gives ~0.4 ns/op, which is *faster* than a correct
> pool. The test prints a `NOTE|` line saying so. The number only means
> something once all four tests pass.

### A1. Why not just `new`?
`new`/`malloc` run a general-purpose heap: size classes, per-thread caches,
locking, and now and then a trip to the kernel for more pages. On average that's
about 10–20 ns, but once in a while it takes a microsecond, and those rare calls
become your p99.9 (you'll measure it in Part C). A pool gives up generality to
get the same cost every time. Every object is the same size, so allocating is
just popping a pointer.

### A2. One slab, owned once
```cpp
#pragma once
#include <cstddef>
#include <cstdint>
#include <new>

struct Pool {
    Pool(std::size_t obj_size, std::size_t capacity)
        : slot_(round_up(obj_size < sizeof(void*) ? sizeof(void*) : obj_size)),
          cap_(capacity) {
        buf_  = static_cast<std::uint8_t*>(::operator new(slot_ * cap_));
        head_ = nullptr;
        for (std::size_t i = cap_; i-- > 0; )      // thread every slot, back to front
            free(buf_ + i * slot_);
    }
    ~Pool() { ::operator delete(buf_); }
    Pool(const Pool&) = delete;                     // one owner of the slab (Session 3)
    Pool& operator=(const Pool&) = delete;
    // alloc() and free() go here: A3, A4
private:
    static std::size_t round_up(std::size_t n) {    // keep every slot max-aligned
        constexpr std::size_t A = alignof(std::max_align_t);
        return (n + A - 1) & ~(A - 1);
    }
    std::size_t   slot_, cap_;
    std::uint8_t* buf_;
    void*         head_;                            // top of the free-list
};
```
- **Why a slot is at least `sizeof(void*)`:** a *free* slot stores the address
  of the next free slot inside itself. This is an intrusive free-list, and it
  costs zero extra memory.
- **Why `round_up`:** `::operator new` returns memory aligned for any type, but
  slot *i* starts at `buf_ + i * slot_`. With `obj_size = 12` the second slot
  would sit at offset 12, which is misaligned for a `double`. Rounding
  `slot_` up to `alignof(std::max_align_t)` keeps every slot aligned. (The
  tests use `obj_size = 8`, so CI won't catch it if you skip this. Your
  `Order` will.)
- **Why the copy is deleted:** two `Pool` copies would `delete` the same slab
  twice. That's the Rule of Five from Session 3.

### A3. `alloc()`: pop, O(1)
```cpp
    void* alloc() {
        if (!head_) return nullptr;                 // exhausted: the tests rely on this
        void* p = head_;
        head_ = *static_cast<void**>(head_);        // head = head->next
        return p;
    }
```

### A4. `free()`: push, O(1)
```cpp
    void free(void* p) {
        if (!p) return;
        *static_cast<void**>(p) = head_;            // p->next = head
        head_ = p;
    }
```
The constructor builds the initial list by calling `free()` on every slot, back
to front, so the first `alloc()` returns slot 0.

### A5. Placement `new` + explicit destructor
The pool gives you raw bytes. To make a live object you construct it in place.
Because *you* started its lifetime, *you* end it. `delete` would be wrong here,
since it would hand the heap memory the heap never gave you.
```cpp
void* m = pool.alloc();
if (!m) return;                                      // never placement-new a null pointer
Order* o = new (m) Order{id, px, qty};               // construct in the slot
// ... use o ...
o->~Order();                                         // end its life
pool.free(o);                                        // give the slot back
```
Say it out loud: **placement `new` to construct, `~T()` to destroy, `free` to
reclaim.** If you forget the `free`, `alloc()` returns `nullptr` after
`capacity` ticks and your bot goes quiet without crashing.

### A6. Run it
```bash
make pool
```
```text
RESULT|pool_basic|pass|4 distinct, writable, non-overlapping slots
RESULT|pool_full|pass|3 distinct slots, then alloc past capacity -> null
RESULT|pool_reuse|pass|freed slot reused
RESULT|pool_pattern|pass|100k fill/drain rounds OK
METRIC|pool_ns_per_op|0.54
```
That metric is from an Apple M4 laptop (Apple clang 21, `-O2`). Expect well
under 1 ns/op, and don't chase the third digit: an alloc+free pair is a couple
of loads and stores, so it's in timer noise. If you see several ns, you're
doing more work than a free-list pop. If you see exactly 0, the benchmark's sink
is missing.

---

## Part B — The book (`include/order_book.hpp`)

The contract, from the stub and `tests/book_test.cpp`:
```cpp
struct Book {
    void   add(uint64_t id, char side, double px, uint32_t qty); // 'B' = bid, 'S' = ask
    void   cancel(uint64_t id);
    double best_bid() const;   // O(1); 0.0 if the side is empty
    double best_ask() const;   // O(1); 0.0 if the side is empty
};
struct SymMap {                // get() returns (uint64_t)-1 if absent
    void     put(const char* sym, uint64_t id);
    uint64_t get(const char* sym) const;
};
```

### B1. Why not `std::map<double, Level>`?
A tree gives O(log n) add and cancel, and every node is a separate heap
allocation. Walking to the best price chases pointers across memory. But prices
aren't arbitrary doubles: they sit on a **tick grid** ($0.01 here). That lets a
price become an **array index**, so the common operations are O(1) and every
access goes to one contiguous array.

### B2. Ticks and a price-indexed **band** (read this before you write code)
```cpp
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <unordered_map>

static constexpr double TICK = 0.01;
static constexpr int    NLEV = 1 << 16;          // 65,536 contiguous slots = a BAND

static inline int px_to_tick(double px) {
    return static_cast<int>(px / TICK + 0.5);    // round to the nearest tick
}
```
`NLEV` one-cent slots cover **$655.35**. If you index *absolutely* (slot = tick,
so slot 0 = $0.00), your book stops at $655.35. The arena lists **NFLX near
$720** (`plugins/securities/defaults.py`). A bid at $719.98 is tick 71,998,
which is 6,462 slots past the end of `bid_qty`. `bid_qty` and `ask_qty` are
adjacent members, so that write lands in `ask_qty` and **adds quantity to the
ask side at $64.62**.

It won't crash. `-fsanitize=address` won't report it either, because ASan
checks the boundaries *between* allocations, not the boundaries between two
members of one object. And the autograder only uses prices near $100, so CI
stays green. You end up with a book that shows size nobody quoted, and a bot
that crosses a spread that doesn't exist.

The fix is **not** "make the array bigger": an absolute array wide enough for a
$5,000 stock is megabytes of empty slots, and it throws away the small working
set you built this structure for. Instead, index against a **base**:
`slot = tick - base_tick_`. Centre the band on the first price you see, and
bounds-check both ends on every write.

### B3. The level arrays
```cpp
struct Book {
    std::array<uint32_t, NLEV> bid_qty{};   // resting qty per bid slot
    std::array<uint32_t, NLEV> ask_qty{};   // resting qty per ask slot
    int  base_tick_ = 0;                    // absolute tick of slot 0
    bool based_     = false;                // has the band been placed yet?
    int best_bid_slot = -1;                 // highest bid slot with qty > 0
    int best_ask_slot = -1;                 // lowest  ask slot with qty > 0

    int slot_of(int tick) {                 // the first price centres the band
        if (!based_) { base_tick_ = std::max(0, tick - NLEV / 2); based_ = true; }
        return tick - base_tick_;
    }
    double px_of(int slot) const { return (slot + base_tick_) * TICK; }

    struct Rec { char side; int slot; uint32_t qty; };
    std::unordered_map<uint64_t, Rec> orders_;   // id -> what cancel must undo
    // add / cancel / best_bid / best_ask: B4-B6
};
```
The best slots are tracked explicitly, so reading the touch (the hot path)
never scans. Add and cancel pay a little to keep them fresh. `slot_of` isn't
`const` because it may place the band. Only the write path can move it.

### B4. `add`: update qty, nudge the touch
```cpp
    void add(uint64_t id, char side, double px, uint32_t qty) {
        int i = slot_of(px_to_tick(px));
        if (i < 0 || i >= NLEV) return;          // outside the band: refuse, don't scribble
        orders_[id] = {side, i, qty};
        if (side == 'B') {
            bid_qty[i] += qty;
            if (i > best_bid_slot) best_bid_slot = i;
        } else {
            ask_qty[i] += qty;
            if (best_ask_slot < 0 || i < best_ask_slot) best_ask_slot = i;
        }
    }
```

### B5. `best_bid` / `best_ask`: pure O(1) reads
```cpp
    double best_bid() const { return best_bid_slot < 0 ? 0.0 : px_of(best_bid_slot); }
    double best_ask() const { return best_ask_slot < 0 ? 0.0 : px_of(best_ask_slot); }
```

### B6. `cancel`: the only place we scan
```cpp
    void cancel(uint64_t id) {
        auto it = orders_.find(id);
        if (it == orders_.end()) return;
        Rec r = it->second; orders_.erase(it);
        if (r.side == 'B') {
            bid_qty[r.slot] -= r.qty;
            if (r.slot == best_bid_slot && bid_qty[r.slot] == 0)
                while (best_bid_slot >= 0 && bid_qty[best_bid_slot] == 0) --best_bid_slot;
        } else {
            ask_qty[r.slot] -= r.qty;
            if (r.slot == best_ask_slot && ask_qty[r.slot] == 0) {
                while (best_ask_slot < NLEV && ask_qty[best_ask_slot] == 0) ++best_ask_slot;
                if (best_ask_slot >= NLEV) best_ask_slot = -1;   // side is empty
            }
        }
    }
```
Both scans are bounded on both ends. `--best_bid_slot` stops at `-1`, and the
ask scan resets to the `-1` sentinel if it runs off the end. The scan only runs
when the **touch** empties, and it walks adjacent slots. The common case,
cancelling away from the top, is a single subtraction.

### B7. `SymMap`: open addressing, no per-lookup allocation
```cpp
struct SymMap {
    static constexpr int CAP = 1 << 12;             // power of two: mask, not %
    struct Slot { char key[16]{}; uint64_t id = (uint64_t)-1; bool used = false; };
    std::array<Slot, CAP> t_{};

    static uint64_t hash(const char* s) {           // FNV-1a
        uint64_t h = 1469598103934665603ull;
        for (; *s; ++s) { h ^= (uint8_t)*s; h *= 1099511628211ull; }
        return h;
    }
    static bool keq(const char* a, const char* b) {
        for (int i = 0; i < 16; ++i) { if (a[i] != b[i]) return false; if (!a[i]) return true; }
        return true;
    }
    void put(const char* sym, uint64_t id) {
        int i = hash(sym) & (CAP - 1);
        for (;; i = (i + 1) & (CAP - 1)) {          // linear probe
            if (!t_[i].used || keq(t_[i].key, sym)) {
                std::snprintf(t_[i].key, 16, "%s", sym);
                t_[i].id = id; t_[i].used = true; return;
            }
        }
    }
    uint64_t get(const char* sym) const {
        int i = hash(sym) & (CAP - 1);
        for (;; i = (i + 1) & (CAP - 1)) {
            if (!t_[i].used)         return (uint64_t)-1;   // empty slot: absent
            if (keq(t_[i].key, sym)) return t_[i].id;
        }
    }
};
```
The table is one contiguous array, it probes with a mask instead of `%`, and a
lookup allocates nothing. Compare that with `std::unordered_map<std::string, …>`,
which builds and hashes a `std::string` on every call. (Keys are at most 15
characters plus the terminator. The test uses `"BRK.B"`.)

### B8. Run it
```bash
make book
```
```text
RESULT|book_bbo|pass|best bid/ask + cancel correct
RESULT|symmap|pass|put/get/overwrite/absent correct
METRIC|book_bbo_ns_per_op|0.55
METRIC|symmap_get_ns_per_op|2.65
```
(Apple M4, `-O2`.) Then `make test` should show `book_bbo` 12 pts and `symmap`
8 pts on top of the pool's 25.

---

## Part C — Measure both (`starters/session06/`)

These two drivers use **your** headers, so they refuse to run on the stubs.

```bash
make bench-alloc     # new/delete vs your Pool: 256 live orders, random free + re-alloc
make bench-book      # your Book / SymMap vs std::map / std::unordered_map
```
One run on an Apple M4 (Apple clang 21, `-O2`):
```text
new/delete  mean  11.36 ns | per-pair, batches of 16: p50 10.44  p99 15.62  p99.9 18.25  max  632.8 ns
Pool        mean   1.37 ns | per-pair, batches of 16: p50  2.56  p99  2.62  p99.9  2.62  max    7.8 ns

BBO read     flat   0.51 ns   std::map   0.92 ns
add+cancel   flat  19.19 ns   std::map  37.48 ns  (both keep an unordered_map id index)
symbol->id   SymMap   2.93 ns   unordered_map<string>  10.70 ns (builds a std::string per call)
```
How to read it, and what to write down:
- The tail is timed in **batches of 16**. On a laptop the clock ticks in ~42 ns
  steps (macOS `steady_clock`, and the arm64 timer even on an M4), so you can't
  time one 2 ns operation, but you can time 16 of them.
- Across seven runs: new/delete had a mean of 11–21 ns and a max of 0.5–0.9 µs
  **every** run. The pool had a mean of 1.4–1.6 ns and a max of 8–10 ns, except
  in about half the runs, where a timer interrupt landed in a batch (~0.4 µs).
  The max measures your OS as well as your code. Say so in your note.
- `add+cancel` is only 1.4–2× better. Look at why: the reference `Book` keeps
  its id → order index in a `std::unordered_map`, and that index is now the
  bottleneck. Replacing it (open addressing, pooled nodes) is a Phase 2
  improvement, not a HW 6 requirement.

Your numbers will differ. That's the point: **report yours, with your machine
and compiler.**

---

## Part D — Take-home tail

### D1. An arena (bump) allocator
Sketch it in a scratch file. It's in the deck, but it isn't graded:
```cpp
#include <cstddef>

class Arena {                                    // bump / monotonic allocator
    std::byte*  base_;                           // max-aligned slab you own
    std::size_t cap_, off_ = 0;
public:
    Arena(std::byte* buf, std::size_t n) : base_(buf), cap_(n) {}
    void* alloc(std::size_t n, std::size_t align) {      // align is a power of two
        std::size_t p = (off_ + align - 1) & ~(align - 1);
        if (p + n > cap_) return nullptr;
        off_ = p + n;
        return base_ + p;
    }
    void reset() { off_ = 0; }                   // end of tick: everything freed, O(1)
};
```
Give it an `alignas(64) std::byte buf[64 * 1024]` that you keep as a member or
a `static`, not a local. Time `alloc(sizeof(Order), alignof(Order))` against
your pool. The arena should win: it's one add, one mask and one compare. When is
it the wrong tool? When objects don't all die at the same time, or when they
need their destructors run.

### D2. The HW 6 driver (2 of the 10 points)
One small `main` that exercises placement `new` through your pool **and** the
band test. Put it anywhere, e.g. `scratch/hw6_driver.cpp` (the folder is
git-ignored, so commit it somewhere else if you want it graded):
```cpp
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <new>
#include "pool.hpp"
#include "order_book.hpp"

struct Order { std::uint64_t id; double px; std::uint32_t qty; };

int main() {
    Pool pool(sizeof(Order), 2);
    void* m = pool.alloc();
    Order* o = new (m) Order{1, 719.98, 10};       // construct in the slot
    assert(o->qty == 10);
    o->~Order();                                   // explicit destructor
    pool.free(o);
    assert(pool.alloc() == m);                     // the slot came back

    static Book b;                                 // 512 KB: static, not a local
    b.add(1, 'B', 719.98, 10);                     // NFLX-like prices: the band centres here
    b.add(2, 'S', 720.02, 10);
    b.add(3, 'B', 100.00, 10);                     // outside the band: must be REFUSED
    std::printf("BBO %.2f / %.2f\n", b.best_bid(), b.best_ask());
    assert(b.best_bid() > 719.97 && b.best_bid() < 719.99);
    assert(b.best_ask() > 720.01 && b.best_ask() < 720.03);
}
```
```bash
g++ -std=c++17 -O2 -Iinclude scratch/hw6_driver.cpp -o /tmp/hw6 && /tmp/hw6
# BBO 719.98 / 720.02
```
Then answer in one line: what will your Phase 2 book do *instead of refusing*
when the market walks out of the band? (Re-base the band and re-key the resting
levels.)

### D3. The ring buffer and Welford, checked by hand
Type in the deck's `Ring<T, N>` (with its `static_assert`) and the `Online`
struct. Feed `Online` the values 1, 2, 3, 4, 5. You should get mean 3 and sample
variance 2.5. Check that by hand before you trust it on prices. Then try
`Ring<double, 100>` and read the error the `static_assert` gives you.

### D4. Wire it into your bot (Project Phase 2, due Thu Nov 5)
In your bot's `on_book`:
1. Map the symbol to a small int **once** with `SymMap`. After that, every
   access is `st_[k]`, a fixed array of per-symbol state.
2. Keep the touch as integer ticks, and keep **your own resting orders** in
   pooled slots with the `queue_ahead` / `level_qty` that `on_ack` and
   `on_queue` report.
3. Find every hidden allocation on the path (`std::string` temporaries, map
   inserts, vector growth), remove it or move it off the path, and re-measure
   against your Phase 1 numbers with
   `scripts/latency_replay.py --latest --cmd './hft/cpp_client/build/hft_bot --replay'`.

### D5. Optional: the sliding-window counter
`include/rolling_counter.hpp` / `make rolling` implements the event counter from
the deck's appendix (A3). It's ungraded this term. Do it if you want practice
with amortized O(1) and the unsigned-underflow trap.

## Checkpoint
- `make pool`: four `pass` lines. `make book`: `book_bbo` and `symmap` both pass.
- Two measured ratios from Part C, with your machine stated, plus **one sentence
  on why the pool's number is stable** and not just small.
- You can say in one sentence why the flat book reads the touch in O(1), and
  where the only scan happens (cancelling the last order at the touch).
- You can say why NFLX at $720 would corrupt an absolute-indexed book while CI
  stays green.

## Links
- Edit: `include/pool.hpp`, `include/order_book.hpp`
- Contracts (read-only): `tests/pool_test.cpp`, `tests/book_test.cpp`, `tests/bench.hpp`
- Measurements: `starters/session06/bench_alloc.cpp`, `starters/session06/bench_book.cpp`
- Reference CLOB (price-time priority, queue position): `shared/orderbook.py` in the arena repo
- Client hooks (`on_book`, `on_ack`, `on_queue`): `hft/cpp_client/include/hft_bot.hpp`, `docs/HFT_CPP_CLIENT.md`
- Build: `make pool`, `make book`, `make bench-alloc`, `make bench-book`, full grade `make test`

# Session 4 Lab — Polymorphism & Smart Ownership

**FINM 32700 · Session 4 · Mon Oct 19** · in class, in pairs (~40 min) + a take-home tail
**Repo:** your copy of the starter · **Deck:** Session 4 — *Object-Oriented C++ II: Polymorphism & Smart Pointers*
**Feeds:** HW 4 — *Polymorphism & smart ownership* (due Thu Oct 29, 10:59 pm CT) ·
Project Phase 1 — *The Fast Hot Path* (due Mon Oct 26, 10:59 pm CT)
**Next session:** the midterm (Mon Oct 26, remote) covers Sessions 1–4 — steps 1–3 are exam practice.

## Goal

Call derived code through a base and see what the compiler still decides
statically; leak 800 KB through a missing virtual destructor; look at the vptr;
**measure** a virtual call against five alternatives; then own objects with
`unique_ptr`, price `shared_ptr` (it is the copy, not the deref), break a
`shared_ptr` cycle with `weak_ptr`, and finally **count the heap allocations your
own `on_book` makes** — the number Phase 1 wants at zero.

| Step | What | Minutes |
|---|---|---|
| 0 | Setup: `scratch/s4/`, the two build recipes | 2 |
| 1 | An interface, a factory, `vector<unique_ptr<IStrategy>>` — and a broken `override` | 8 |
| 2 | Delete through a base without a virtual destructor | 5 |
| 3 | The object model: `sizeof`, the vptr, virtual calls in constructors, the assembly | 7 |
| 4 | `make dispatch`: what a virtual call costs, next to five alternatives | 10 |
| 5 | `unique_ptr` vs `shared_ptr`: sizes, allocation counts, custom deleters | 8 |
| 6 | The `shared_ptr` tax, contention, and a `weak_ptr` cycle | take-home |
| 7 | Count the allocations in your `on_book` (`tick_alloc.hpp` + the replay tape) | take-home |
| 8 | Refactor your bot: an `ISignal` owned by `unique_ptr`, chosen at startup | take-home (HW 4) |

**Checkpoint at 2:50:** post in the Zoom chat (a) the error line from step 1's
broken `override`, (b) your full step-4 table, and (c) your step-5
`allocations:` line — plus your machine and compiler.

## 0. Setup (2 min)

```bash
mkdir -p scratch/s4                    # scratch/ is git-ignored
# CORRECTNESS (steps 1-3, 5): -O0 -g, sanitizers on where the step says so
clang++ -std=c++20 -O0 -g -Wall -Wextra scratch/s4/FILE.cpp -o /tmp/FILE
# TIMING (steps 4, 6): -O2, or you measured nothing
clang++ -std=c++20 -O2 -Wall -Wextra -Itests scratch/s4/FILE.cpp -o /tmp/FILE
```

`g++` works the same. Every output below was produced on an Apple M4 with Apple
clang 21; your timings will differ, your *orderings* should not.

## 1. An interface, a factory, and a broken `override` (8 min)

`scratch/s4/s4_poly.cpp`:

```cpp
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

struct Book { double bid, ask, mid, microprice, obi; };

struct IStrategy {                                   // an interface: a contract
    virtual double signal(const Book& b) const = 0;  // pure virtual
    virtual const char* name() const = 0;
    virtual ~IStrategy() = default;                  // we delete through the base
};

struct Momentum final : IStrategy {
    double signal(const Book& b) const override { return 0.5 * b.obi; }
    const char* name() const override { return "momentum"; }
};

struct MeanRevert final : IStrategy {
    explicit MeanRevert(double k) : k_(k) {}
    double signal(const Book& b) const override { return -k_ * (b.microprice - b.mid); }
    const char* name() const override { return "meanrevert"; }
    ~MeanRevert() override { std::puts("~MeanRevert"); }
private:
    double k_;
};

// The factory: the ONLY place that knows the concrete types.
// Ownership leaves through the return type.
std::unique_ptr<IStrategy> make_strategy(const std::string& kind) {
    if (kind == "momentum")   return std::make_unique<Momentum>();
    if (kind == "meanrevert") return std::make_unique<MeanRevert>(40.0);
    return nullptr;                                  // unknown: caller must check
}

int main() {
    std::vector<std::unique_ptr<IStrategy>> strats;  // owns every strategy
    for (const char* k : {"momentum", "meanrevert", "typo"})
        if (auto s = make_strategy(k)) strats.push_back(std::move(s));
        else std::printf("unknown strategy '%s' rejected at startup\n", k);

    const Book b{100.00, 100.02, 100.01, 100.015, 0.40};
    for (const auto& s : strats)                     // borrow: const unique_ptr&
        std::printf("%-10s signal=%+.3f\n", s->name(), s->signal(b));
    std::printf("%zu strategies\n", strats.size());
}                                                    // vector dies -> each strategy dies
```

```bash
clang++ -std=c++20 -O2 -Wall -Wextra scratch/s4/s4_poly.cpp -o /tmp/s4_poly && /tmp/s4_poly
```

```text
unknown strategy 'typo' rejected at startup
momentum   signal=+0.200
meanrevert signal=-0.200
2 strategies
~MeanRevert
```

Read it against the deck: the **factory** is the only function that names a
concrete type; the `vector` **owns**; the loop **borrows** through
`const unique_ptr&`; and `~MeanRevert` runs when the vector dies — through
`IStrategy`'s virtual destructor.

**Now break it on purpose.** In `Momentum`, delete the `const` from
`signal(...) const override`. Compile:

```text
error: non-virtual member function marked 'override' hides virtual member function
error: allocating an object of abstract class type 'Momentum'
```

Without `override` the first line vanishes — you would have silently written a
*new* function, and the second error would be your only clue (and only because
the base is abstract). Put the `const` back.

## 2. Delete through a base without a virtual destructor (5 min)

`scratch/s4/s4_dtor.cpp`:

```cpp
#include <cstdio>
#include <vector>

struct Base {
    virtual void on_tick() {}
    ~Base() { std::puts("~Base"); }                  // BUG: not virtual
};
struct Recorder : Base {
    std::vector<double> ticks = std::vector<double>(100'000);   // 800 KB
    ~Recorder() { std::puts("~Recorder"); }
};

int main() {
    Base* p = new Recorder;
    delete p;                                        // UB: ~Recorder never runs
}
```

```bash
clang++ -std=c++20 -O0 -g -Wall scratch/s4/s4_dtor.cpp -o /tmp/s4_dtor && /tmp/s4_dtor
```

Clang warns (`delete called on non-final 'Base' that has virtual functions but
non-virtual destructor [-Wdelete-non-abstract-non-virtual-dtor]`), builds anyway,
and the program prints only:

```text
~Base
```

`~Recorder` never ran, so the vector's 800 KB buffer is never freed. Prove it:

- **macOS:** `leaks --atExit -- /tmp/s4_dtor` → `1 leak for 802816 total leaked bytes.`
  (ignore the "not debuggable" preamble; build at `-O0`, since `leaks` is
  conservative and can miss a leak at `-O1`+).
- **Linux:** add `-fsanitize=address` and run; LeakSanitizer (on by default on
  Linux) reports the buffer as a direct leak. On macOS, ASan has no leak
  detection and exits 0 — that is the platform, not your fix.

**Fix:** `virtual ~Base() { std::puts("~Base"); }`. Re-run: `~Recorder`, then
`~Base`, and `leaks` reports 0 leaks. One word, 800 KB.

## 3. The object model (7 min)

`scratch/s4/s4_model.cpp`:

```cpp
#include <cstdio>

struct P { int a; };                          // no virtuals
struct V { int a; virtual ~V() = default; };  // one virtual => one hidden vptr
static_assert(sizeof(P) == 4);
static_assert(sizeof(V) == 16);               // 8 (vptr) + 4 (a) + 4 (padding)
static_assert(alignof(V) == 8);

struct Base {
    Base()          { hello(); }              // virtual call in a constructor
    virtual ~Base() { hello(); }              // ... and in a destructor
    virtual void hello() const { std::puts("Base::hello"); }
    void greet() const { hello(); }           // an ordinary virtual call
};
struct Derived : Base {
    void hello() const override { std::puts("Derived::hello"); }
};

int main() {
    Derived d;       // prints Base::hello — during ~Base()/Base() the object IS a Base
    d.greet();       // prints Derived::hello
}                    // prints Base::hello again from ~Base
```

```bash
clang++ -std=c++20 -O2 -Wall -Wextra scratch/s4/s4_model.cpp -o /tmp/s4_model && /tmp/s4_model
```

```text
Base::hello
Derived::hello
Base::hello
```

The `static_assert`s passing *is* the first half of the output: one virtual
function added 8 bytes of vptr and 4 of padding. The three lines are the second
half: while `Base()` runs, the object's vptr points at `Base`'s table — the
`Derived` part does not exist yet — and `~Base()` sees the same thing on the way out.

Now look at a virtual call. `scratch/s4/asm_demo.cpp`:

```cpp
struct Calc { virtual long f(long) const = 0; virtual ~Calc() = default; };
long call_virtual(const Calc& c, long x) { return c.f(x); }

template <class D> struct Base { long f(long x) const { return static_cast<const D*>(this)->f_impl(x); } };
struct Twice : Base<Twice> { long f_impl(long x) const { return 2 * x; } };
long call_crtp(const Twice& t, long x) { return t.f(x); }

struct TwiceV final : Calc { long f(long x) const override { return 2 * x; } };
long call_final(const TwiceV& t, long x) { return t.f(x); }
```

```bash
clang++ -std=c++20 -O2 -S scratch/s4/asm_demo.cpp -o - | grep -A5 -E '^_*_Z[0-9]+call_'
```

On arm64 (Apple Silicon), `call_virtual` is `ldr x8, [x0]` (load the vptr),
`ldr x2, [x8]` (load the slot), `br x2` (indirect jump). `call_crtp` and
`call_final` are both `lsl x0, x1, #1` + `ret` — the call is gone, the body
is inlined. On x86-64 you will see `movq (%rdi), %rax` / `movq (%rax), %rax` /
`jmpq *%rax` against `leaq (%rsi,%rsi), %rax`. (`call_crtp` is Session 5's
preview: a template, no `virtual` anywhere.)

## 4. What a virtual call costs — `make dispatch` (10 min)

`starters/session04/dispatch_bench.cpp` times one tiny call (`2*x`, `3*x`,
`x+7` or `x^5`) twenty million times per mechanism, best of five, over 4,096
objects in three orders: **mono** (all the same type), **mixed** (four types,
random order), **sorted** (the same four types, grouped). Read the file first —
the objects are owned by `std::vector<std::unique_ptr<Signal>>`, exactly the
pattern from step 1.

```bash
make dispatch
```

Apple M4, Apple clang 21:

```text
dispatch mechanism                          mono     mixed    sorted   (ns per call)
direct (exact final type, inlined)          0.24         -         -
virtual call through Signal*                0.76      4.84      0.78
std::variant + std::visit                   0.76      4.87      0.77
enum tag + switch                           0.39      1.17      0.55
function pointer                            0.76      4.19      0.77
std::function                               1.02      4.58      1.02
sizeof(Twice)=8 (vptr only)  sizeof(VTwice)=1  sizeof(AnySignal)=8  sizeof(std::function<long(long)>)=32
```

Run it twice; the second decimal wobbles between builds and runs (the direct
row reads anywhere from 0.23 to 0.38 here), the pattern does not. Answer in
your notes — these are HW 4 questions:

1. How much does a **predicted** virtual call cost over the direct call? A
   **mispredicted** one? Convert the mixed number to cycles for your CPU.
2. Why is **sorted** almost exactly **mono**, with the same objects?
3. Why is `std::variant` no faster than `virtual` in the mixed column — and
   what does it still buy you (look at the `sizeof` line and think about where
   the objects live)?
4. Why can the **switch** go branch-free here, and why would that stop being
   true if each case were 50 lines long?

## 5. `unique_ptr` vs `shared_ptr`: sizes, allocations, deleters (8 min)

`scratch/s4/s4_owner.cpp` replaces the global `operator new` to count every
heap allocation in the program:

```cpp
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>

// Count every heap allocation in the program (replaces the global operator new).
static long g_allocs = 0;
void* operator new(std::size_t n) {
    ++g_allocs;
    if (void* p = std::malloc(n)) return p;
    throw std::bad_alloc{};
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

struct Quote { double px; int qty; };

// Custom deleters: what does each cost in SIZE?
struct FileCloser { void operator()(std::FILE* f) const { if (f) std::fclose(f); } };
using File   = std::unique_ptr<std::FILE, FileCloser>;              // stateless deleter
using FileFp = std::unique_ptr<std::FILE, int (*)(std::FILE*)>;     // function pointer
static_assert(sizeof(std::unique_ptr<Quote>) == sizeof(Quote*));    // zero overhead
static_assert(sizeof(File)   == sizeof(std::FILE*));                // still zero
static_assert(sizeof(FileFp) == 2 * sizeof(void*));                 // stores the pointer
static_assert(sizeof(std::shared_ptr<Quote>) == 2 * sizeof(void*)); // object + control block

int main() {
    long a0 = g_allocs;
    auto s1 = std::make_shared<Quote>(Quote{100.0, 5});             // object + count, ONE block
    long a1 = g_allocs;
    std::shared_ptr<Quote> s2(new Quote{100.0, 5});                 // object, THEN control block
    long a2 = g_allocs;
    auto u = std::make_unique<Quote>(Quote{100.0, 5});
    long a3 = g_allocs;
    std::printf("allocations: make_shared=%ld  shared_ptr(new)=%ld  make_unique=%ld\n",
                a1 - a0, a2 - a1, a3 - a2);

    {
        auto c = s1;                                                // copy: count 1 -> 2
        std::printf("use_count after copy = %ld\n", s1.use_count());
    }                                                               // c dies: 2 -> 1
    std::printf("use_count after scope = %ld\n", s1.use_count());

    File f(std::fopen("/dev/null", "w"));                           // RAII for a C handle
    std::fputs("closed by FileCloser at scope exit\n", f.get());
    std::printf("sizeof: unique_ptr=%zu File=%zu FileFp=%zu shared_ptr=%zu\n",
                sizeof(u), sizeof(File), sizeof(FileFp), sizeof(s1));
}
```

```bash
clang++ -std=c++20 -O2 -Wall -Wextra scratch/s4/s4_owner.cpp -o /tmp/s4_owner && /tmp/s4_owner
```

```text
allocations: make_shared=1  shared_ptr(new)=2  make_unique=1
use_count after copy = 2
use_count after scope = 1
sizeof: unique_ptr=8 File=8 FileFp=16 shared_ptr=16
```

`make_shared` puts the object and its control block in **one** allocation;
`shared_ptr(new T)` needs two. A stateless deleter (`FileCloser`) costs zero
bytes; a function-pointer deleter doubles the handle. Every `static_assert`
passing is part of the result.

## 6. The `shared_ptr` tax, contention, and a cycle (take-home)

**6a. It is the copy, not the deref.** `scratch/s4/s4_sharedcost.cpp`:

```cpp
#include <cstdio>
#include <memory>
#include "bench.hpp"                                 // -Itests: ns_per_op, doNotOptimize

struct Quote { double px; int qty; };

// noinline: a real call boundary, like a function in another .cpp file
[[gnu::noinline]] double by_value(std::shared_ptr<Quote> q)      { return q->px; }
[[gnu::noinline]] double by_cref(const std::shared_ptr<Quote>& q) { return q->px; }
[[gnu::noinline]] double by_ref(const Quote& q)                   { return q.px; }

int main() {
    auto up = std::make_unique<Quote>(Quote{100.0, 5});
    auto sp = std::make_shared<Quote>(Quote{100.0, 5});
    constexpr long N = 50'000'000;

    double u_get  = ns_per_op([&] { Quote* r = up.get(); doNotOptimize(r); }, N);
    double s_get  = ns_per_op([&] { Quote* r = sp.get(); doNotOptimize(r); }, N);
    double s_copy = ns_per_op([&] { auto c = sp; doNotOptimize(c); }, N);
    double f_val  = ns_per_op([&] { double x = by_value(sp); doNotOptimize(x); }, N);
    double f_cref = ns_per_op([&] { double x = by_cref(sp);  doNotOptimize(x); }, N);
    double f_ref  = ns_per_op([&] { double x = by_ref(*sp);  doNotOptimize(x); }, N);

    std::printf("unique_ptr get          %5.2f ns\n", u_get);
    std::printf("shared_ptr get          %5.2f ns\n", s_get);
    std::printf("shared_ptr copy+destroy %5.2f ns\n", s_copy);
    std::printf("call f(shared_ptr)      %5.2f ns   (a copy per call)\n", f_val);
    std::printf("call f(const shared_ptr&) %3.2f ns\n", f_cref);
    std::printf("call f(const Quote&)    %5.2f ns\n", f_ref);
}
```

```bash
clang++ -std=c++20 -O2 -Wall -Wextra -Itests scratch/s4/s4_sharedcost.cpp -o /tmp/s4_sc && /tmp/s4_sc
```

```text
unique_ptr get           0.23 ns
shared_ptr get           0.23 ns
shared_ptr copy+destroy  3.20 ns
call f(shared_ptr)       3.23 ns   (a copy per call)
call f(const shared_ptr&) 0.69 ns
call f(const Quote&)     0.68 ns
```

(The first line sometimes reads ~0.47 on the first run — warm-up; `get()` on
either pointer is the same single load.)

**6b. Now share it between threads.** `scratch/s4/s4_contended.cpp`:

```cpp
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>
#include "bench.hpp"
struct Quote { double px; int qty; };
int main() {
    auto sp = std::make_shared<Quote>(Quote{100.0, 5});
    constexpr long N = 20'000'000;
    for (int T : {1, 2, 4}) {
        std::vector<std::thread> th;
        auto t0 = std::chrono::steady_clock::now();
        for (int t = 0; t < T; ++t)
            th.emplace_back([&] { for (long i = 0; i < N; ++i) { auto c = sp; doNotOptimize(c); } });
        for (auto& x : th) x.join();
        auto t1 = std::chrono::steady_clock::now();
        std::printf("%d thread(s) copying ONE shared_ptr: %.2f ns per copy (per thread)\n", T,
                    std::chrono::duration<double, std::nano>(t1 - t0).count() / N);
    }
}
```

```bash
clang++ -std=c++20 -O2 -pthread -Itests scratch/s4/s4_contended.cpp -o /tmp/s4_ct && /tmp/s4_ct
```

```text
1 thread(s) copying ONE shared_ptr: 3.25 ns per copy (per thread)
2 thread(s) copying ONE shared_ptr: 17.60 ns per copy (per thread)
4 thread(s) copying ONE shared_ptr: 64.52 ns per copy (per thread)
```

Same instruction, twenty times slower: every core is fighting for the one cache
line that holds the reference count. You will meet this line-bouncing again in
Session 7.

**6c. A cycle, and the fix.** `scratch/s4/s4_cycle.cpp`:

```cpp
#include <cstdio>
#include <memory>

struct Strategy;
struct Gateway {                                   // routes orders for a strategy
    std::shared_ptr<Strategy> owner;               // owning back-link: a CYCLE
    ~Gateway() { std::puts("~Gateway"); }
};
struct Strategy {
    std::shared_ptr<Gateway> gw;
    ~Strategy() { std::puts("~Strategy"); }
};

struct Gateway2;
struct Strategy2 {
    std::shared_ptr<Gateway2> gw;
    ~Strategy2() { std::puts("~Strategy2"); }
};
struct Gateway2 {
    std::weak_ptr<Strategy2> owner;                // observes, does not own
    void route() const {
        if (auto s = owner.lock()) std::puts("route: strategy alive");
        else                       std::puts("route: strategy gone");
    }
    ~Gateway2() { std::puts("~Gateway2"); }
};

int main() {
    {
        auto s = std::make_shared<Strategy>();
        s->gw = std::make_shared<Gateway>();
        s->gw->owner = s;                          // count(s) = 2
    }                                              // count(s) = 1 forever: nothing printed
    std::puts("-- cycle leaked; now the weak_ptr version --");
    std::shared_ptr<Gateway2> keep;
    {
        auto s = std::make_shared<Strategy2>();
        s->gw = std::make_shared<Gateway2>();
        s->gw->owner = s;                          // weak: count(s) stays 1
        s->gw->route();
        keep = s->gw;
    }                                              // ~Strategy2 runs here
    keep->route();
}
```

```bash
clang++ -std=c++20 -O0 -g -Wall scratch/s4/s4_cycle.cpp -o /tmp/s4_cycle && /tmp/s4_cycle
leaks --atExit -- /tmp/s4_cycle | grep "leaks for"      # macOS
```

```text
-- cycle leaked; now the weak_ptr version --
route: strategy alive
~Strategy2
route: strategy gone
~Gateway2
Process NNNNN: 2 leaks for 96 total leaked bytes.
```

The first block prints **nothing** — neither destructor ever runs, and `leaks`
finds both objects. The `weak_ptr` version destroys the strategy at the closing
brace, and `lock()` tells the gateway it is gone.

## 7. Count the allocations in your `on_book` (take-home)

The starter ships `hft/cpp_client/include/tick_alloc.hpp`: a counting
`operator new` plus two RAII scopes. `TickScope` counts everything allocated
while `on_book` runs; `SendScope` moves an order send into its own counter.
Make **four edits** to `hft/cpp_client/src/main.cpp`:

```cpp
#include "hft_bot.hpp"
#include "tick_alloc.hpp"   // Session 4 lab: include in this ONE file only
```

```cpp
    void on_book(const std::string& symbol, double bid, double ask, double mid,
                 double microprice, double obi) override {
        tickalloc::TickScope tick;                       // count this call
        // ... the example strategy, unchanged ...
```

```cpp
            tickalloc::SendScope send;
            buy_limit(symbol, kClip, ask);               // cross to buy
        // ... and the same line before sell_limit(...)
```

```cpp
        bot.run_replay(std::cin, std::cout);
        tickalloc::report(std::cerr);
```

Build, make a replay tape (the harness's own synthetic snapshots, written to a
file), and run the bot over it directly — the harness discards the bot's stderr,
so we bypass it:

```bash
cmake --build hft/cpp_client/build
python3 -c "import sys,json; sys.path.insert(0,'scripts'); import latency_replay as L; [print(json.dumps(s,separators=(',',':'))) for s in L.synthetic_snapshots(2000)]" > /tmp/tape.jsonl
hft/cpp_client/build/hft_bot --replay < /tmp/tape.jsonl 2> /tmp/bot.err > /dev/null
grep -a tickalloc /tmp/bot.err
```

The stock example bot, measured:

```text
[tickalloc] on_book calls: 2000 | allocations in your code: 10 (0.005 per tick) | order sends: 1997 | allocations inside sends: 95857 (48.0005 per send)
```

Read it carefully:

- **Your code: 10 allocations in 2,000 ticks** — all on the first tick of each
  of the three symbols, when `last_mid_[symbol]` and `position_[symbol]` insert
  into their `unordered_map<std::string, …>`. That is warm-up, not steady state,
  but a new symbol mid-session would pay it on the hot path. (Pre-populate the
  maps at startup to make it zero.)
- **The send path: ~48 allocations per order** — the client builds each order as
  an `nlohmann::json` object, serialises it to a `std::string`, and logs the
  latency. Every one of those is a `malloc` that is usually fast and
  occasionally very slow. Session 5 builds an allocation-free encoder; Phase 1
  wants the steady-state number at zero.

`tick_alloc.hpp` is harmless in a live run (it forwards to `malloc`), so you can
leave it in while you work on Phase 1.

## 8. Refactor: an `ISignal` owned by the bot (take-home — this is HW 4's bot task)

Replace the `SpreadCaptureBot` class in `src/main.cpp` with the version below
(keep everything from the "Plumbing" comment down), change `SpreadCaptureBot
bot(cfg);` to `MyBot bot(cfg);`, and add `#include <cstdlib>`, `<memory>` and
`<stdexcept>` at the top:

```cpp
// ── Session 4, step 8: the signal is an interface, owned by the bot ─────────
struct ISignal {
    virtual double value(double mid, double microprice, double obi) const = 0;
    virtual ~ISignal() = default;
};
struct ObiSignal final : ISignal {            // book imbalance, in [-1, 1]
    double value(double, double, double obi) const override { return obi; }
};
struct MicropriceSignal final : ISignal {     // microprice above mid => up-pressure
    explicit MicropriceSignal(double scale) : scale_(scale) {}
    double value(double mid, double mp, double) const override { return scale_ * (mp - mid); }
private:
    double scale_;
};
std::unique_ptr<ISignal> make_signal(const char* name) {
    const std::string n = name ? name : "obi";          // default when SIGNAL is unset
    if (n == "obi")        return std::make_unique<ObiSignal>();
    if (n == "microprice") return std::make_unique<MicropriceSignal>(100.0);
    return nullptr;
}

class MyBot : public arena::HFTBot {
public:
    explicit MyBot(arena::ClientConfig cfg)
        : HFTBot(std::move(cfg)),
          signal_(make_signal(std::getenv("SIGNAL"))) {  // chosen ONCE
        if (!signal_) throw std::runtime_error("unknown SIGNAL (use obi or microprice)");
    }

private:
    static constexpr double kEnter  = 0.10;   // act only on a clear signal
    static constexpr int    kMaxPos = 5;      // inventory cap

    std::unique_ptr<ISignal> signal_;         // owned by the bot, built at startup

    void on_book(const std::string& symbol, double bid, double ask, double mid,
                 double microprice, double obi) override {
        tickalloc::TickScope tick;                              // step 7 counter
        if (bid <= 0.0 || ask <= 0.0) return;
        const double s   = signal_->value(mid, microprice, obi); // borrow: one virtual call
        const int    pos = position_[symbol];                    // first sight inserts once
        if (s > kEnter && pos < kMaxPos) {
            tickalloc::SendScope send;
            buy_limit(symbol, 1, bid);                           // join the bid
        } else if (s < -kEnter && pos > -kMaxPos) {
            tickalloc::SendScope send;
            sell_limit(symbol, 1, ask);                          // join the ask
        }
    }

    void on_fill(const std::string& side, const std::string& symbol,
                 int quantity, double price, bool maker) override {
        if (side.empty()) return;
        position_[symbol] += (side == "buy") ? quantity : -quantity;
        (void)price; (void)maker;
    }

    void on_session(const std::string& event, const std::string& message) override {
        std::cerr << "[session] " << event << " — " << message << "\n";
        if (event == "SESSION_CLOSED")
            for (auto& [sym, qty] : position_) {
                if (qty > 0)      sell_market(sym, qty);
                else if (qty < 0) buy_market(sym, -qty);
            }
    }

    std::unordered_map<std::string, int> position_;   // symbol -> net shares
};
```

```bash
cmake --build hft/cpp_client/build
for S in obi microprice; do
  SIGNAL=$S hft/cpp_client/build/hft_bot --replay < /tmp/tape.jsonl 2> /tmp/bot.err > /dev/null
  echo "SIGNAL=$S"; grep -a tickalloc /tmp/bot.err
done
SIGNAL=bogus hft/cpp_client/build/hft_bot --replay < /tmp/tape.jsonl > /dev/null   # must fail fast
```

Measured:

```text
SIGNAL=obi
[tickalloc] on_book calls: 2000 | allocations in your code: 5 (0.0025 per tick) | order sends: 0 | allocations inside sends: 0 (0 per send)
SIGNAL=microprice
[tickalloc] on_book calls: 2000 | allocations in your code: 5 (0.0025 per tick) | order sends: 2000 | allocations inside sends: 96001 (48.0005 per send)
libc++abi: terminating due to uncaught exception of type std::runtime_error: unknown SIGNAL (use obi or microprice)
```

The synthetic tape's imbalance never clears `kEnter`, so `obi` sends nothing — a
reminder that the tape is for latency, not for P&L. Five allocations, all first-sight inserts into `position_` (three nodes plus
the bucket array growing); zero per tick after that. The bogus signal aborts in
the constructor — before any socket opens, which is exactly where you want a
configuration error. Then run the latency harness to be sure nothing regressed:

```bash
python3 scripts/latency_replay.py --self-test --cmd "hft/cpp_client/build/hft_bot --replay"
```

## Your turn — HW 4 (*Polymorphism & smart ownership*, 10 pts, due Thu Oct 29, 10:59 pm CT)

1. **(3 pts)** An abstract `IStrategy` (pure virtuals + virtual destructor), two
   `final` strategies with `override` on every override, built by a factory into
   a `vector<unique_ptr<IStrategy>>` (step 1). Show it clean under
   `leaks --atExit` (macOS) or `-fsanitize=address` (Linux), and show the
   step-2 leak disappearing when you add `virtual`.
2. **(3 pts)** Your step-4 table with CPU and compiler, and a paragraph that
   answers the four questions under it.
3. **(2 pts)** Your step-6a numbers, and the step-6c cycle fixed with `weak_ptr`
   (paste the output that shows the destructor now runs).
4. **(2 pts)** Your bot from step 8 (or your own design with the same shape:
   a signal owned by `unique_ptr`, chosen once at startup), plus the
   `tick_alloc` line showing zero steady-state allocations in your strategy code.

## Checkpoint

- Step 1: two strategies print, `~MeanRevert` runs; removing `const` gives the
  `override` error.
- Step 2: `~Base` alone, then 802,816 leaked bytes; after the fix, `~Recorder ~Base` and 0 leaks.
- Step 3: the three `hello` lines, and you can point at the two `ldr` + `br` (or two `movq` + `jmpq *`).
- Step 4: your mixed column is several times your mono column for `virtual`,
  `variant`, function pointers and `std::function`.
- Step 5: `make_shared=1  shared_ptr(new)=2  make_unique=1`.
- Step 7/8: allocations in your code are a one-off warm-up count, not per tick.

## Links
Session 4 deck · HW 4 · Project Phase 1 (`project/README.md`) · Next: Session 5
lab (`labs/session05.md`) adds a CRTP row to `dispatch_bench.cpp`.

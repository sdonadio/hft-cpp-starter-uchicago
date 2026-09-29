# Session 5 Lab — Templates & CRTP

**FINM 32700 · Session 5 · Mon Oct 26** · 15 min in class (after the midterm) + a take-home tail
**Repo:** your copy of the starter · **Deck:** Session 5 — *Templates, Compile-Time & CRTP*
**Feeds:** HW 5 — *Templates & CRTP* (due Thu Nov 5, 10:59 pm CT) ·
Project Phase 1 — *The Fast Hot Path* (due **tonight**, Mon Oct 26, 10:59 pm CT)

## Goal

Write templates and watch the compiler do the work: a class template whose
capacity is part of its type; specializations; packs and folds; the `overload{}`
visitor; SFINAE next to a concept; `consteval` fees and a compile-time table;
and a CRTP strategy that erases Session 4's virtual call — **measured** in the
same `dispatch_bench` you ran last week. Every file is a complete program.

| Step | What | When |
|---|---|---|
| 0 | Setup: `scratch/s5/`, one build recipe | in class (1 min) |
| 1 | `max_of`, `Ring<T, N>` and two deliberate compile errors | in class (7 min) |
| 2 | `Wire<T>`: primary, full and partial specialization | in class (7 min) |
| 3 | Packs, folds, `if constexpr`, the `overload{}` visitor | take-home |
| 4 | SFINAE vs a concept: the same function, two error messages | take-home |
| 5 | `constexpr`, `consteval`, `static_assert`: fees and a tick table | take-home |
| 6 | CRTP strategies + a CRTP row in `dispatch_bench` + the assembly | take-home |
| 7 | A policy-based `Quoter`; add a third policy axis | take-home |
| 8 | (Phase 1) the allocation-free order encoder | take-home |

**Checkpoint (8:55 pm):** paste the two compile errors from step 1 in the chat.

## 0. Setup

```bash
mkdir -p scratch/s5
clang++ -std=c++20 -O2 -Wall -Wextra -Itests scratch/s5/FILE.cpp -o /tmp/FILE && /tmp/FILE
```

C++20 is required tonight (concepts, `consteval`, aggregate CTAD). `g++` 11+
works the same. Outputs below: Apple M4, Apple clang 21.

## 1–2. Templates, a class template, specializations (in class, 14 min)

`scratch/s5/s5_templates.cpp`:

```cpp
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>

// 1. A function template: a recipe the compiler stamps out per type.
template <class T>
T max_of(T a, T b) { return a < b ? b : a; }

// 2. A class template with a NON-TYPE parameter: capacity is part of the type.
template <class T, std::size_t N>
class Ring {
    static_assert(N > 0 && (N & (N - 1)) == 0, "N must be a power of two");
    std::array<T, N> buf_{};          // inline storage: no heap, ever
    std::size_t head_ = 0;            // total pushes so far
public:
    void push(const T& x) { buf_[head_++ & (N - 1)] = x; }     // overwrite oldest
    std::size_t size() const { return head_ < N ? head_ : N; }
    const T& back() const { return buf_[(head_ - 1) & (N - 1)]; }
    T sum() const {
        T s{};                        // value-initialised: 0, 0.0, Price{0}, ...
        for (std::size_t i = 0; i < size(); ++i) s += buf_[i];
        return s;
    }
};

// 3. Full specialization: a hand-tuned version for ONE exact type.
struct Price { std::int64_t ticks; };
template <class T> struct Wire          { static const char* fmt() { return "generic"; } };
template <>        struct Wire<Price>   { static const char* fmt() { return "fixed-point"; } };
template <class T> struct Wire<T*>      { static const char* fmt() { return "pointer"; } };  // partial

int main() {
    std::printf("max_of(3,4)=%d  max_of(1.5,2.5)=%.1f\n", max_of(3, 4), max_of(1.5, 2.5));
    // max_of(3, 4.5);                // error: T deduced as int AND double
    std::printf("max_of<double>(3,4.5)=%.1f\n", max_of<double>(3, 4.5));   // explicit T

    Ring<double, 8> mids;
    for (int i = 0; i < 10; ++i) mids.push(100.0 + i * 0.01);   // 10 pushes into 8 slots
    std::printf("size=%zu back=%.2f sum=%.2f\n", mids.size(), mids.back(), mids.sum());
    // Ring<int, 6> bad;              // error: static_assert — N must be a power of two

    std::printf("Wire<int>=%s Wire<Price>=%s Wire<int*>=%s\n",
                Wire<int>::fmt(), Wire<Price>::fmt(), Wire<int*>::fmt());
    static_assert(sizeof(Ring<double, 8>) == 8 * sizeof(double) + sizeof(std::size_t));
}
```

```text
max_of(3,4)=4  max_of(1.5,2.5)=2.5
max_of<double>(3,4.5)=4.5
size=8 back=100.09 sum=800.44
Wire<int>=generic Wire<Price>=fixed-point Wire<int*>=pointer
```

**Step 1 — break it twice.** Uncomment `max_of(3, 4.5);`:

```text
error: no matching function for call to 'max_of'
note: candidate template ignored: deduced conflicting types for parameter 'T' ('int' vs. 'double')
```

Put it back and uncomment `Ring<int, 6> bad;`:

```text
error: static assertion failed due to requirement '(6UL & (6UL - 1)) == 0': N must be a power of two
```

Ten pushes into eight slots keep the last eight (`100.02 … 100.09`, sum
`800.44`): the ring overwrites, it never grows, and it never touches the heap —
`sizeof(Ring<double, 8>)` is exactly 8 doubles plus one counter.

**Step 2 — specializations.** Explain in one line each why `Wire<int>`,
`Wire<Price>` and `Wire<int*>` pick the version they do. Then predict and check
`Wire<Price*>::fmt()` (partial beats primary: `pointer`), and add a partial
specialization `template <class T, std::size_t N> struct Wire<Ring<T, N>>` that
returns `"ring"`.

## 3–4. Packs, folds, `if constexpr`, `overload{}`, concepts (take-home)

`scratch/s5/s5_variadic.cpp`:

```cpp
#include <concepts>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <variant>

// 1. A pack and two folds.
template <class... Ts>
constexpr auto sum(Ts... xs) { return (xs + ...); }            // unary right fold
template <class... Ts>
constexpr double avg(Ts... xs) { return sum(xs...) / double(sizeof...(xs)); }
static_assert(sum(1, 2, 3, 4) == 10);                          // evaluated by the compiler

// 2. if constexpr: one body, a different path per type, dead arms discarded.
struct Buf { char data[64]; std::size_t n = 0; };
template <class T>
void put(Buf& b, T v) {
    if constexpr (std::is_same_v<T, bool>) {
        b.data[b.n++] = v ? 1 : 0;                             // 1 byte
    } else if constexpr (std::is_integral_v<T>) {
        std::memcpy(b.data + b.n, &v, sizeof v); b.n += sizeof v;
    } else if constexpr (std::is_floating_point_v<T>) {
        long long ticks = static_cast<long long>(v * 100.0 + 0.5);   // price -> ticks
        std::memcpy(b.data + b.n, &ticks, sizeof ticks); b.n += sizeof ticks;
    } else {
        static_assert(!sizeof(T), "put(): unsupported field type");
    }
}
template <class... Fs>
void encode_all(Buf& b, const Fs&... fs) { (put(b, fs), ...); }  // fold over the comma

// 3. The overload{} helper: a variadic class that inherits every lambda's operator().
template <class... Fs> struct overload : Fs... { using Fs::operator()...; };
template <class... Fs> overload(Fs...) -> overload<Fs...>;     // deduction guide

struct BookUpdate { double mid; };
struct Fill       { int qty; };
struct Cancel     { unsigned long id; };
using Msg = std::variant<BookUpdate, Fill, Cancel>;

const char* route(const Msg& m) {
    return std::visit(overload{
        [](const BookUpdate&) { return "book"; },
        [](const Fill&)       { return "fill"; },
        [](const Cancel&)     { return "cancel"; },          // delete me: compile error
    }, m);
}

// 4. Constraining a template — the old way (SFINAE) and the C++20 way (concept).
template <class T, std::enable_if_t<std::is_arithmetic_v<T>, int> = 0>
T twice_sfinae(T x) { return x + x; }

template <class T>
concept Arithmetic = std::is_arithmetic_v<T>;
template <Arithmetic T>
T twice(T x) { return x + x; }

int main() {
    std::printf("sum=%d avg=%.2f\n", sum(1, 2, 3, 4), avg(1.0, 2.0, 4.5));
    Buf b;
    encode_all(b, 42, 100.25, true, std::int64_t{7});
    std::printf("encoded %zu bytes (int 4 + price 8 + bool 1 + int64 8)\n", b.n);
    Msg msgs[] = {BookUpdate{100.01}, Fill{5}, Cancel{17}};
    for (const auto& m : msgs) std::printf("%s ", route(m));
    std::printf("\ntwice_sfinae(21)=%d twice(1.25)=%.2f\n", twice_sfinae(21), twice(1.25));
    // twice("no");            // error: constraints not satisfied — 'Arithmetic'
    // twice_sfinae("no");     // error: candidate ignored, enable_if requirement not met
}
```

```text
sum=10 avg=2.50
encoded 21 bytes (int 4 + price 8 + bool 1 + int64 8)
book fill cancel 
twice_sfinae(21)=42 twice(1.25)=2.50
```

Step 3 exercises:

- In `route`, delete the `Cancel` lambda. Clang: ``static assertion failed due
  to requirement 'is_invocable_v<overload<…>, const Cancel &>': `std::visit`
  requires the visitor to be exhaustive``. That is a production bug caught at
  build time. Put it back.
- Call `encode_all(b, 42, "AAPL")`: the `if constexpr` chain reaches its final
  `else` — `static assertion failed … put(): unsupported field type`.
- Add a `Heartbeat {}` alternative to `Msg` and its lambda.

Step 4: uncomment `twice("no");`, then (separately) `twice_sfinae("no");`:

```text
twice("no"):        note: candidate template ignored: constraints not satisfied [with T = const char *]
                    note: because 'const char *' does not satisfy 'Arithmetic'
twice_sfinae("no"): note: candidate template ignored: requirement 'std::is_arithmetic_v<const char *>' was not satisfied
```

Modern clang explains both; the concept version names the *requirement*, and
you can reuse `Arithmetic` everywhere. Write one more concept, `Strategy`, that
requires `s.signal(b)` to return something convertible to `double` (deck slide
13), and `static_assert` that your step-6 `Momentum` satisfies it.

## 5. `constexpr`, `consteval`, `static_assert` (take-home)

`scratch/s5/s5_compile_time.cpp`:

```cpp
#include <array>
#include <cstdint>
#include <cstdio>

// 1. constexpr: CAN run at compile time. A table built by the compiler.
struct TickTable {
    std::array<std::int64_t, 256> px{};           // prices in 1/10000 dollars
    constexpr TickTable() {
        for (int i = 0; i < 256; ++i) px[i] = 1'000'000 + i * 100;   // $100.00 + i cents
    }
};
constexpr TickTable kTicks{};                    // built entirely at compile time
static_assert(kTicks.px[50] == 1'005'000);       // $100.50, proven before the program runs

// 2. consteval: MUST run at compile time. A runtime argument is a compile error.
consteval std::int64_t bps(double rate) { return static_cast<std::int64_t>(rate * 10'000 + 0.5); }
constexpr std::int64_t kTakerBps = bps(0.0015);  // 15 bps
constexpr std::int64_t kMakerBps = bps(0.0010);  // 10 bps rebate
static_assert(kTakerBps == 15 && kMakerBps == 10);

// 3. static_assert on layout: a violated assumption fails the BUILD, not the market.
struct Level { std::int64_t px; std::int32_t qty; std::int32_t orders; };
static_assert(sizeof(Level) == 16 && 64 % sizeof(Level) == 0, "4 levels per cache line");

std::int64_t price_at(int i) { return kTicks.px[i]; }   // run time: one load from .rodata

int main(int argc, char**) {
    std::printf("px[50]=%lld taker=%lld bps maker=%lld bps\n",
                (long long)price_at(50), (long long)kTakerBps, (long long)kMakerBps);
    double r = 0.0015 * argc;
    (void)r;
    // auto bad = bps(r);        // error: call to consteval function is not a constant expression
}
```

```text
px[50]=1005000 taker=15 bps maker=10 bps
```

Uncomment `auto bad = bps(r);`:

```text
error: call to consteval function 'bps' is not a constant expression
```

Now make `Level` 24 bytes (add an `int64_t` field) and watch the layout
`static_assert` fail the build. Then check where the table lives:
`clang++ -std=c++20 -O2 -S scratch/s5/s5_compile_time.cpp -o - | grep -c kTicks`
— it is data in the binary, not code that runs at startup.

## 6. CRTP — and the number (take-home)

`scratch/s5/s5_crtp.cpp` (also holds step 7's `Quoter`):

```cpp
#include <cstdio>

struct Book { double bid, ask, mid, microprice, obi; };

// 1. CRTP: the base knows the derived type, so the call is bound at compile time.
template <class Derived>
struct Strategy {
    double signal(const Book& b) const {
        return static_cast<const Derived*>(this)->signal_impl(b);   // no vtable
    }
    int side(const Book& b) const {                                 // shared logic, written once
        double s = signal(b);
        return s > 0.1 ? +1 : (s < -0.1 ? -1 : 0);
    }
};
struct Momentum : Strategy<Momentum> {
    double signal_impl(const Book& b) const { return 0.5 * b.obi; }
};
struct MeanRevert : Strategy<MeanRevert> {
    double signal_impl(const Book& b) const { return -40.0 * (b.microprice - b.mid); }
};

// No common base pointer: generic code takes the CRTP base by template.
template <class S>
int decide(const Strategy<S>& s, const Book& b) { return s.side(b); }

// 2. Policy-based design: behaviour as template parameters.
struct TakerFees { static constexpr double rate = +0.0015; };   // pay to cross
struct MakerFees { static constexpr double rate = -0.0010; };   // rebate for resting
struct NoSkew    { static constexpr double skew(int)     { return 0.0; } };
struct LinSkew   { static constexpr double skew(int pos) { return -0.001 * pos; } };

template <class FeePolicy, class SkewPolicy>
struct Quoter {
    // edge per share after fees, for a quote at px, holding pos shares
    static constexpr double edge(double half_spread, double px, int pos) {
        return half_spread - FeePolicy::rate * px + SkewPolicy::skew(pos);
    }
};
using Aggressive = Quoter<TakerFees, NoSkew>;
using Passive    = Quoter<MakerFees, LinSkew>;
static_assert(Passive::edge(0.01, 100.0, 0) > Aggressive::edge(0.01, 100.0, 0));

int main() {
    const Book b{100.00, 100.02, 100.01, 100.015, 0.40};
    Momentum m; MeanRevert r;
    std::printf("momentum side=%+d  meanrevert side=%+d\n", decide(m, b), decide(r, b));
    std::printf("edge/share at $100, half-spread 1c: aggressive=%+.4f passive=%+.4f\n",
                Aggressive::edge(0.01, 100.0, 0), Passive::edge(0.01, 100.0, 0));
}
```

```text
momentum side=+1  meanrevert side=-1
edge/share at $100, half-spread 1c: aggressive=-0.1400 passive=+0.1100
```

Now fill Session 4's TODO in `starters/session04/dispatch_bench.cpp`. Above the
`// ── harness` line add:

```cpp
template <class D> struct CSignal {
    long f(long x) const { return static_cast<const D*>(this)->f_impl(x); }
};
struct CTwice : CSignal<CTwice> { long f_impl(long x) const { return 2 * x; } };
```

and at the TODO, inside `main`:

```cpp
    CTwice ct;
    measure("CRTP (static, type known)", [&](const Set&, std::size_t, long x) { return ct.f(x); });
```

```bash
make dispatch
```

Measured (Apple M4, clang 21, the new row plus two for reference):

```text
direct (exact final type, inlined)          0.38         -         -
virtual call through Signal*                0.70      4.32      0.73
CRTP (static, type known)                   0.23      0.24      0.24
```

CRTP is the direct call (the direct row wobbles 0.23–0.38 between builds —
code alignment, not dispatch). Its "mixed" and "sorted" columns only repeat the
mono number, because a CRTP call cannot *see* the run-time pattern: the type is
fixed at compile time. Say in your write-up what you would do if the type
really arrived at run time (a `std::variant` of CRTP types, or group by type).

Evidence from the compiler: the Session 4 lab's `asm_demo.cpp` already has
`call_crtp`; `-O2 -S` shows `lsl x0, x1, #1` / `ret` (x86: `leaq (%rsi,%rsi), %rax`)
against `ldr`/`ldr`/`br` for `call_virtual`. Rebuild at `-O0` and the `bl`/`call`
reappears: "zero-cost" is the optimiser's work, not a property of the syntax.

## 7. Policy-based design (take-home)

`Quoter<FeePolicy, SkewPolicy>` in `s5_crtp.cpp` already has two axes. At $100
the taker fee is $0.15 a share against a $0.01 half-spread; the `static_assert`
proves at build time that posting beats crossing. Add a third axis,
`SizePolicy` (e.g. `FixedSize<1>` vs `InvSpreadSize`), with a `static constexpr
int qty(double spread)`, instantiate two configurations, and `static_assert`
one property of each.

## 8. (Phase 1) An allocation-free order encoder (take-home)

Session 4 step 7 measured ~48 allocations per order in the stock send path.
`scratch/s5/s5_codec.cpp` — an `if constexpr` field writer, a variadic `encode`
folded over fields, `std::to_chars` into a reused buffer, and a counting
`operator new` to prove it:

```cpp
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <new>
#include <string_view>
#include <type_traits>

static long g_allocs = 0;                              // count heap allocations
void* operator new(std::size_t n) { ++g_allocs; if (void* p = std::malloc(n)) return p; throw std::bad_alloc{}; }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

struct Buf {                                           // reused, fixed capacity
    char d[256]; std::size_t n = 0;
    void raw(std::string_view s) { std::memcpy(d + n, s.data(), s.size()); n += s.size(); }
};
template <class T>
void put(Buf& b, std::string_view key, const T& v) {
    b.raw(b.n > 1 ? ",\"" : "\""); b.raw(key); b.raw("\":");
    if constexpr (std::is_same_v<T, bool>) b.raw(v ? "true" : "false");
    else if constexpr (std::is_arithmetic_v<T>)            // int, double: no alloc
        b.n = std::to_chars(b.d + b.n, std::end(b.d), v).ptr - b.d;
    else { b.raw("\""); b.raw(v); b.raw("\""); }            // string-like
}
template <class T> struct F { std::string_view key; T val; };   // one field
template <class... Ts>
void encode(Buf& b, const F<Ts>&... fs) {
    b.n = 0; b.raw("{");
    (put(b, fs.key, fs.val), ...);                     // fold: one put per field
    b.raw("}");
}
int main() {
    Buf b;
    long a0 = g_allocs;
    encode(b, F{"type", "place_order"}, F{"symbol", "NVDA"}, F{"side", "buy"},
              F{"order_type", "limit"}, F{"price", 100.25}, F{"quantity", 5});
    long a1 = g_allocs;
    std::printf("%.*s\nallocations: %ld\n", int(b.n), b.d, a1 - a0);
}
```

```text
{"type":"place_order","symbol":"NVDA","side":"buy","order_type":"limit","price":100.25,"quantity":5}
allocations: 0
```

Timed on the M4 at `-O2` for the same six-field order: this encoder ~56 ns and
0 allocations, `nlohmann::json` build + `dump()` ~1,261 ns and 41 allocations.
Wiring it into the client's send path is Phase 1 work (the client currently
builds orders in `ArenaClient::place_limit`); keep the before/after
`tick_alloc` lines.

## Your turn — HW 5 (*Templates & CRTP*, 10 pts, due Thu Nov 5, 10:59 pm CT)

1. **(3 pts)** A class template with a non-type parameter and inline storage
   (`Ring<T, N>` or your own), a `static_assert` on `N`, used with two element
   types, and constrained by a concept.
2. **(2 pts)** A variadic utility with a fold **and** an `if constexpr`
   per-type path (the step-8 encoder qualifies), with a test.
3. **(3 pts)** A CRTP strategy base next to its virtual twin: your
   `dispatch_bench` CRTP row with CPU + compiler, and the `-O2 -S` lines that
   show the indirect branch is gone.
4. **(2 pts)** A `consteval`/`constexpr` table guarded by `static_assert`, and
   the policy-based `Quoter` with its extra axis.

## Checkpoint

- Step 1: both compile errors reproduced and explained.
- Step 3: deleting a visitor lambda breaks the build.
- Step 6: your CRTP row matches your direct row, not your virtual row.
- Step 8: `allocations: 0`.

## Links
Session 5 deck · HW 5 · Session 4 lab (`labs/session04.md`, `dispatch_bench.cpp`) ·
Next: Session 6 lab (`labs/session06.md`).

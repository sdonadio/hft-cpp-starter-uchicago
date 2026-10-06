# Session 2 Lab — Pointers & the Cost of Memory · Classes, Encapsulation & Inheritance

**FINM 32700 · Session 2 · Mon Oct 5** · in class, in pairs (Part A ~45 min, Part B ~45 min) + a take-home tail
**Repo:** your copy of the starter · **Deck:** Session 2 — *Pointers & the Cost of Memory* and *Object-Oriented C++ I: Encapsulation & Inheritance*
**Feeds:** HW 2 — *Pointers, classes, the Rule of Five & RAII* (due Thu Oct 22, 10:59 pm CT) ·
Project Phase 0 — *Connect & Baseline* (due Mon Oct 12, 10:59 pm CT)

## Goal

Two halves of one story. **Part A (steps 0–6)**: every claim in the deck about
memory, measured on **your** machine — an honest micro-benchmark (and a dishonest
one), what `const` and array decay actually do, stack vs heap, contiguous vs
pointer-chasing, row-major vs column-major, the classic memory bugs caught by a
tool, and false sharing removed with `alignas(64)`. **Part B (steps 7–13)**:
those same bugs, fixed by types — write a class that cannot be built invalid;
watch construction and destruction happen in order; build `PxBuf` — a class that
owns a price buffer — correctly by hand, then remove its copy operations and
watch ASan catch the double free; add the two moves and **measure** what
`noexcept` buys when a `vector` grows; write an RAII guard that survives an
exception; shrink a struct by reordering it; and see slicing and name hiding
happen. You leave with four timings, one sanitizer report, the bugs-fixed-by-types
classes HW 2 grows from, and the harness HW 2 is graded on.

| Step | What | Minutes |
|---|---|---|
| 0 | Setup: read `tests/bench.hpp`, make `scratch/` | 3 |
| **A** | **Pointers & the cost of memory** | |
| 1 | An honest micro-benchmark — and a dishonest one | 5 |
| 2 | Pointers, `const` and decay: read the compiler errors | 5 |
| 3 | Stack vs heap — sink the pointer | 5 |
| 4 | Contiguous vs pointer chase; row- vs column-major | 9 |
| 5 | Memory bugs under ASan / `leaks` / LSan | 10 |
| 6 | False sharing and `alignas(64)` | 8 |
| **B** | **Classes, the Rule of Five & inheritance** | |
| 7 | `Order`: an invariant, `const`, private data | 6 |
| 8 | Construction & destruction order | 5 |
| 9 | `PxBuf`: Rule of Three — and the double free | 10 |
| 10 | Rule of Five: `noexcept` and `vector` growth | 8 |
| 11 | `ScopedTimer`: RAII on every exit path | 5 |
| 12 | Layout: `sizeof`, padding, `static_assert` | 4 |
| 13 | Inheritance: slicing, name hiding, static binding | 5 |
| 14 | *Take-home:* build `starters/hw02` as shipped | — |

**Checkpoint at the end of Part A:** post in the Zoom chat (a) your stack/heap ratio, (b) your
list/vector ratio, (c) your shared/padded ratio, and (d) the first `ERROR:` or
`leak` line from a sanitizer — plus your machine (e.g. "M2 Air, Apple clang 17").
**Checkpoint at the end of Part B:** post (a) your step-9 ASan `ERROR:` line
from the broken build, (b) your two step-10 lines (copies/moves + ms), and (c) your
step-13 output.

# Part A — Pointers & the Cost of Memory

## 0. Setup (3 min)

Two build recipes in Part A. Keep them in two terminal tabs and never mix them up:

```bash
# TIMING runs (steps 1, 3, 4, 6): optimised, or you measured nothing
clang++ -std=c++20 -O2 -Wall -Itests scratch/FILE.cpp -o /tmp/FILE

# CORRECTNESS runs (step 5): -O0 -g so the tools can see everything
clang++ -std=c++20 -O0 -g -fsanitize=address scratch/FILE.cpp -o /tmp/FILE
```

`g++` works the same on Linux (add `-pthread` for step 6 on either). We reuse
the grader's own timing helper — read it now, it is 20 lines that matter:

```bash
mkdir -p scratch              # git-ignored: nothing here is committed
sed -n '13,18p;69,76p' tests/bench.hpp
```

The two pieces you need: `ns_per_op(fn, iters)` runs `iters/10` warm-up calls,
then times `iters` calls with `std::chrono::steady_clock` and divides; and
`doNotOptimize(v)` is an empty `asm` statement that pretends to read `v`, so
`-O2` cannot delete the work that produced it.


## 1. An honest micro-benchmark — and a dishonest one (5 min)

```cpp
// scratch/s1.cpp
#include "bench.hpp"     // build with -Itests
#include <cstdio>

int main() {
    long n = 50'000'000;
    double x = 1.0;
    double honest = ns_per_op([&]{ x = x * 1.0000001 + 1.0; doNotOptimize(x); }, n);
    double y = 1.0;
    double lie    = ns_per_op([&]{ y = y * 1.0000001 + 1.0; }, n);   // no sink
    std::printf("honest: %.3f ns/op   no sink: %.3f ns/op\n", honest, lie);
    auto clk = ns_per_op([]{ auto t = std::chrono::steady_clock::now(); doNotOptimize(t); }, 10'000'000);
    std::printf("steady_clock::now(): %.1f ns\n", clk);
}
```

```bash
clang++ -std=c++20 -O2 -Wall -Itests scratch/s1.cpp -o /tmp/s1 && /tmp/s1
```

Measured on an Apple M4, Apple clang 21, `-O2` (yours will differ). **Run every
benchmark tonight at least twice** — the first run on a busy laptop is often
noisy (we saw a 15× stack/heap ratio once, then 40–53× three times in a row):

```text
honest: 1.233 ns/op   no sink: 0.000 ns/op
steady_clock::now(): 12.7 ns
```

`0.000 ns/op` is not speed, it is the optimiser deleting a loop whose result
nobody reads. **If a number is faster than an L1 hit (~1 ns), it is not a
number.** Note the clock: reading `steady_clock` costs ~13 ns here, which is why
you time a *batch* of tiny operations and divide, never one at a time.

## 2. Pointers, `const` and decay — read the compiler errors (5 min)

```cpp
// scratch/s2.cpp
#include <cstdio>
#include <span>

void by_ptr(const int* p)            { std::printf("by_ptr  sizeof(p)=%zu\n", sizeof(p)); }
void by_span(std::span<const int> v) { std::printf("by_span size()=%zu\n", v.size()); }
void swap_ptr(int* a, int* b)        { int t = *a; *a = *b; *b = t; }

int main() {
    int a[5] = {10, 20, 30, 40, 50};
    std::printf("main    sizeof(a)=%zu  elements=%zu\n", sizeof(a), sizeof(a) / sizeof(a[0]));
    by_ptr(a);                 // decays: the length is gone
    by_span(a);                // pointer + length travel together

    int x = 1, y = 2;
    const int* p1 = &x;        // pointer to const int
    int* const p2 = &x;        // const pointer to int
    // *p1 = 5;                // (A) uncomment: what does the compiler say?
    // p2 = &y;                // (B) uncomment: and now?
    p1 = &y;                   // fine: p1 itself may move
    *p2 = 7;                   // fine: write through p2
    swap_ptr(&x, &y);
    std::printf("x=%d y=%d *p1=%d\n", x, y, *p1);
    std::printf("a+1 - a = %td elements, %td bytes\n", (a + 1) - a,
                reinterpret_cast<char*>(a + 1) - reinterpret_cast<char*>(a));
}
```

```bash
clang++ -std=c++20 -O2 -Wall scratch/s2.cpp -o /tmp/s2 && /tmp/s2
```

```text
main    sizeof(a)=20  elements=5
by_ptr  sizeof(p)=8
by_span size()=5
x=2 y=7 *p1=7
a+1 - a = 1 elements, 4 bytes
```

Before running, predict the `x=… y=…` line out loud with your partner (hint:
`*p2 = 7` wrote into `x`, then the swap). Then uncomment **(A)** and rebuild —
clang says `read-only variable is not assignable`; put it back, uncomment
**(B)** — `cannot assign to variable 'p2' with const-qualified type 'int *const'`.
Read `const int*` and `int* const` right to left until those two messages are
obvious.

## 3. Stack vs heap — sink the pointer (5 min)

```cpp
// scratch/s2.cpp
#include "bench.hpp"
#include <cstdio>

int main() {
    auto stack_ns = ns_per_op([]{
        int buf[64];
        buf[0] = 7; doNotOptimize(buf);      // sink the ARRAY
    }, 20'000'000);
    auto heap_ns = ns_per_op([]{
        int* p = new int[64];
        p[0] = 7; doNotOptimize(p);          // sink the POINTER
        delete[] p;
    }, 20'000'000);
    auto wrong_ns = ns_per_op([]{
        int* p = new int[64];
        p[0] = 7; doNotOptimize(p[0]);       // WRONG sink: the int, not the allocation
        delete[] p;
    }, 20'000'000);
    std::printf("stack=%.2f ns  heap=%.2f ns  (%.1fx)   heap, wrong sink=%.2f ns\n",
                stack_ns, heap_ns, heap_ns / stack_ns, wrong_ns);
}
```

```bash
clang++ -std=c++20 -O2 -Wall -Itests scratch/s2.cpp -o /tmp/s2 && /tmp/s2
clang++ -std=c++20 -O0 -Itests scratch/s2.cpp -o /tmp/s3_O0 && /tmp/s3_O0   # once, to see the lie
```

Measured on an Apple M4:

```text
-O2:  stack=0.25 ns  heap=12.32 ns  (49.2x)   heap, wrong sink=0.23 ns
-O0:  stack=2.50 ns  heap=13.59 ns  (5.4x)    heap, wrong sink=13.33 ns
```

Three lessons in one table. **The heap is ~50× the stack** for a small buffer.
**The wrong sink lies:** `doNotOptimize(p[0])` only keeps an `int` alive, and
C++14 lets the compiler elide a `new`/`delete` pair whose storage never escapes —
clang does, and the "heap" costs less than an L1 hit. Sink the thing whose cost
you are measuring: for an allocator, the pointer. **`-O0` hides the effect:** a
50× gap shrinks to 5× because the debug overhead swamps it. Report `-O2`, always.

## 4. Contiguous vs pointer chase; row- vs column-major (9 min)

```cpp
// scratch/s3.cpp
#include "bench.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <numeric>
#include <random>
#include <vector>

struct Node { int v; Node* next; };

int main() {
    // --- (a) the same 1M ints: a vector, a list in allocation order, a shuffled list
    const int M = 1'000'000;
    std::vector<int> arr(M);
    std::iota(arr.begin(), arr.end(), 0);
    auto seq = ns_per_op([&]{ long s = 0; for (int v : arr) s += v; doNotOptimize(s); }, 200);

    Node* head = nullptr;
    for (int i = 0; i < M; ++i) head = new Node{i, head};        // one new per node
    auto chase = ns_per_op([&]{ long s = 0; for (Node* p = head; p; p = p->next) s += p->v;
                                doNotOptimize(s); }, 200);

    std::vector<Node*> nodes(M);
    for (int i = 0; i < M; ++i) nodes[i] = new Node{i, nullptr};
    std::shuffle(nodes.begin(), nodes.end(), std::mt19937{1});
    for (int i = 0; i + 1 < M; ++i) nodes[i]->next = nodes[i + 1];
    Node* shead = nodes[0];
    auto shuf = ns_per_op([&]{ long s = 0; for (Node* p = shead; p; p = p->next) s += p->v;
                               doNotOptimize(s); }, 50);
    std::printf("vector %.0f us   list %.0f us   shuffled list %.0f us   (%.0fx / %.0fx)\n",
                seq / 1e3, chase / 1e3, shuf / 1e3, chase / seq, shuf / seq);

    // --- (b) one 4096x4096 matrix of doubles (128 MB): row- vs column-major
    const int N = 4096;
    std::vector<double> m(static_cast<std::size_t>(N) * N, 1.0);
    using clk = std::chrono::steady_clock;
    double s = 0;
    auto t0 = clk::now();
    for (int r = 0; r < N; ++r) for (int c = 0; c < N; ++c) s += m[static_cast<std::size_t>(r) * N + c];
    auto t1 = clk::now();
    for (int c = 0; c < N; ++c) for (int r = 0; r < N; ++r) s += m[static_cast<std::size_t>(r) * N + c];
    auto t2 = clk::now();
    doNotOptimize(s);
    auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    std::printf("row-major %.1f ms   column-major %.1f ms   (%.1fx)\n",
                ms(t0, t1), ms(t1, t2), ms(t1, t2) / ms(t0, t1));

    for (Node* p = head; p;) { Node* n = p->next; delete p; p = n; }
    for (Node* p : nodes) delete p;
}
```

```bash
clang++ -std=c++20 -O2 -Wall -Itests scratch/s3.cpp -o /tmp/s3 && /tmp/s3
```

Measured on an Apple M4:

```text
vector 35 us   list 750 us   shuffled list 10560 us   (21x / 299x)
row-major 9.6 ms   column-major 48.1 ms   (5.0x)
```

Same number of additions in every row. The **allocation-order list** is ~20×
slower even though `new` handed back nodes that are mostly neighbours: every hop
is a *dependent* load (you cannot start loading node *k+1* until node *k* has
arrived), and each 16-byte node carries 4 useful bytes. **Shuffle** the links and
every hop is a likely miss the prefetcher cannot guess: ~300×. The column-major
walk strides 32 KB between reads, so it uses 8 bytes of every cache line it
fetches. This is why an order book is a flat array (Session 5), not a `std::map`
of nodes.

## 5. Memory bugs under a sanitizer (10 min)

Three tiny programs, each with one classic bug. Build them **with the
correctness recipe** (`-O0 -g`).

```cpp
// scratch/leak.cpp
#include <cstdio>
struct Order { int id; double px; };
void handle(double px) {
    Order* o = new Order{1, px};
    if (px > 1000.0) return;      // BUG: early return leaks o
    std::printf("sent %d @ %.2f\n", o->id, o->px);
    delete o;
}
int main() { handle(2000.0); handle(100.0); }
```

```cpp
// scratch/uaf.cpp
#include <cstdio>
int main() {
    int* p = new int(7);
    delete p;
    *p = 8;                       // use-after-free
    std::printf("%d\n", *p);
}
```

```cpp
// scratch/df.cpp
int main() {
    int* q = new int(7);
    delete q;
    delete q;                     // double free
}
```

**Use-after-free and double free — ASan, any platform:**

```bash
clang++ -std=c++20 -O0 -g -fsanitize=address scratch/uaf.cpp -o /tmp/uaf && /tmp/uaf
clang++ -std=c++20 -O0 -g -fsanitize=address scratch/df.cpp  -o /tmp/df  && /tmp/df
```

```text
==74481==ERROR: AddressSanitizer: heap-use-after-free on address 0x6020000000f0 ...
WRITE of size 4 at 0x6020000000f0 thread T0
    #0 0x000102038900 in main uaf.cpp:6
...
==74488==ERROR: AddressSanitizer: attempting double-free on 0x6020000000f0 in thread T0:
    #1 0x000104fc4654 in main df.cpp:5
```

Both abort (exit 134 on macOS) and point at the exact line (line numbers count
the `// scratch/…` comment as line 1). Scroll down in the
report: ASan also shows where the block was **freed** and where it was
**allocated** — three stack traces for one bug.

**The leak — the tool depends on your platform.** ASan on macOS has **no**
LeakSanitizer.

*macOS — use `leaks`:*

```bash
clang++ -std=c++20 -O0 -g scratch/leak.cpp -o /tmp/leak    # no sanitizer
leaks --atExit -- /tmp/leak
```

```text
sent 1 @ 100.00
...
Process 74499: 1 leak for 32 total leaked bytes.
STACK OF 1 INSTANCE OF 'ROOT LEAK: <malloc in handle(double)>':
3   leak   0x100f444b0 handle(double) + 24  leak.cpp:5
```

`leaks` also prints `Process NNNNN is not debuggable…` and a crash-report-style
header: ignore both. 32 bytes, not 16, because `malloc` rounds up to its size
class. `leaks` is *conservative* — a stale copy of the pointer in a register can
hide the leak, which is one more reason to hunt at `-O0`. Trust a positive
report, never a clean one.

*Linux (or any Linux container) — ASan includes LSan:*

```bash
g++ -std=c++20 -O0 -g -fsanitize=address scratch/leak.cpp -o /tmp/leak && /tmp/leak
```

Expected shape (exit code 1):

```text
sent 1 @ 100.00
==31==ERROR: LeakSanitizer: detected memory leaks
Direct leak of 16 byte(s) in 1 object(s) allocated from:
    #0 0x... in operator new(unsigned long)
    #1 0x... in handle(double) scratch/leak.cpp:5
```

> **The trap, so you don't lose twenty minutes to it:** on macOS the ASan build
> of `leak.cpp` exits **0 and reports nothing**, and
> `ASAN_OPTIONS=detect_leaks=1 /tmp/leak` aborts with
> `AddressSanitizer: detect_leaks is not supported on this platform.`
> That is not your bug. On a Mac: `leaks --atExit`, or a Linux container.

Fixing these three for good takes a destructor — Part B, step 9.

## 6. False sharing and `alignas(64)` (8 min)

```cpp
// scratch/s5.cpp
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

struct Shared { std::atomic<long> a{0}, b{0}; };                // one line: ping-pong
struct Padded { alignas(64) std::atomic<long> a{0};
                alignas(64) std::atomic<long> b{0}; };          // one line each

template <class S> double race() {
    S s;
    auto t0 = std::chrono::steady_clock::now();
    std::thread ta([&]{ for (long i = 0; i < 50'000'000; ++i) s.a.fetch_add(1, std::memory_order_relaxed); });
    std::thread tb([&]{ for (long i = 0; i < 50'000'000; ++i) s.b.fetch_add(1, std::memory_order_relaxed); });
    ta.join(); tb.join();
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

int main() {
    static_assert(sizeof(Shared) == 16 && sizeof(Padded) == 128);
    for (int k = 0; k < 3; ++k)
        std::printf("shared=%.1f ms  padded(alignas 64)=%.1f ms\n", race<Shared>(), race<Padded>());
}
```

```bash
clang++ -std=c++20 -O2 -Wall -pthread scratch/s5.cpp -o /tmp/s5 && /tmp/s5
```

Measured on an Apple M4: `shared≈312–400 ms  padded≈80 ms` — about 4×, for
identical atomic work. The two counters are different variables, but a cache
line is the unit of coherence: each write invalidates the other core's copy.
(Apple M-series reports a 128-byte line, `sysctl hw.cachelinesize`, yet 64-byte
padding already removed the effect here — measure, don't assume.) Session 6's
SPSC ring puts `alignas(64)` on its head and tail for exactly this reason.


---

# Part B — Classes, the Rule of Five & Inheritance

Part A's bugs, fixed by types. The build recipes are the same two as in step 0:
correctness runs (steps 7–9, 11–13) use `-O0 -g -fsanitize=address`; the one timing
run (step 10) uses `-O2`. ASan works on macOS for everything in Part B (we need
no leak detection), so Mac and Linux users run identical commands.

## 7. `Order`: an invariant, `const`, private data (6 min)

```cpp
// scratch/order.cpp
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <stdexcept>

class Order {
 public:
    Order(std::uint64_t id, double px, std::uint32_t qty, char side)
        : id_(id), px_(px), qty_(qty), side_(side) {
        if (px <= 0 || qty == 0 || (side != 'B' && side != 'S'))
            throw std::invalid_argument("bad order");
    }
    std::uint64_t id()   const { return id_; }
    double        px()   const { return px_; }
    std::uint32_t qty()  const { return qty_; }
    char          side() const { return side_; }
    void fill(std::uint32_t q) { qty_ -= std::min(q, qty_); }   // never wraps below 0
 private:
    std::uint64_t id_;
    double        px_;
    std::uint32_t qty_;
    char          side_;
};

void show(const Order& o) {                        // borrows read-only
    std::printf("%c %u @ %.2f\n", o.side(), o.qty(), o.px());
    // o.fill(1);                                  // (B) uncomment: why can't you?
}

int main() {
    Order o{1, 101.5, 200, 'B'};
    o.fill(50);
    show(o);
    o.fill(1000);                                  // over-fill: clamps to 0
    show(o);
    try {
        Order bad{2, 101.5, 0, 'B'};               // qty 0: cannot exist
        show(bad);
    } catch (const std::invalid_argument& e) {
        std::printf("rejected: %s\n", e.what());
    }
    // o.qty_ = 0;                                 // (A) uncomment: what does clang say?
}
```

```bash
clang++ -std=c++20 -O0 -g -Wall -fsanitize=address scratch/order.cpp -o /tmp/order && /tmp/order
```

```text
B 150 @ 101.50
B 0 @ 101.50
rejected: bad order
```

Uncomment **(A)**: `'qty_' is a private member of 'Order'`. Put it back; uncomment
**(B)**: `'this' argument to member function 'fill' has type 'const Order', but
function is not marked const`. Those two errors are the whole point of the
class: outsiders can't break the invariant, and a `const&` borrower can't
modify what it borrowed. Notice `bad` was never shown — a half-built object never
escapes a throwing constructor.

## 8. Construction & destruction order (5 min)

**Predict the output in chat before you run it.**

```cpp
// scratch/order_of.cpp
#include <cstdio>

struct Tag {
    const char* n;
    explicit Tag(const char* s) : n(s) { std::printf("+%s ", n); }
    ~Tag() { std::printf("-%s ", n); }
};
struct Venue : Tag { Venue() : Tag("Venue") {} };

struct Quoter : Venue {
    Tag book{"book"};                          // members, in declaration order
    Tag risk{"risk"};
    Quoter()  { std::printf("+Quoter "); }     // body LAST
    ~Quoter() { std::printf("-Quoter "); }     // body FIRST
};

int main() {
    {
        Quoter q;
        std::printf("| ");
    }
    std::printf("\n");
    {
        Tag a{"a"};
        Tag b{"b"};
    }
    std::printf("\n");
}
```

```bash
clang++ -std=c++20 -O0 -g -Wall -fsanitize=address scratch/order_of.cpp -o /tmp/order_of && /tmp/order_of
```

```text
+Venue +book +risk +Quoter | -Quoter -risk -book -Venue
+a +b -b -a
```

Base, then members in declaration order, then the body; destruction is the exact
reverse. Now swap the two member lines (`risk` before `book`) and predict again.
Then try writing the constructor as `Quoter() : risk{"risk"}, book{"book"} {}` —
clang warns (`-Wreorder-ctor`) and the order **still** follows the declarations.

## 9. `PxBuf`: the Rule of Three — and the double free (10 min)

This class owns a raw `new[]`. HW 2 grows from this file — keep it.

```cpp
// scratch/pxbuf.cpp
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <vector>

class PxBuf {                                   // owns a raw new[]
 public:
    explicit PxBuf(std::size_t n) : n_(n), p_(new double[n]()) {}
    ~PxBuf() { delete[] p_; }                   // 1. destructor
#ifndef BROKEN
    PxBuf(const PxBuf& o)                       // 2. copy ctor: DEEP copy
        : n_(o.n_), p_(new double[o.n_]) {
        std::copy(o.p_, o.p_ + n_, p_);
    }
    PxBuf& operator=(const PxBuf& o) {          // 3. copy assignment
        if (this == &o) return *this;           //    self-assignment is safe
        double* tmp = new double[o.n_];         //    allocate BEFORE freeing:
        std::copy(o.p_, o.p_ + o.n_, tmp);      //    if new throws, *this is intact
        delete[] p_;
        p_ = tmp;
        n_ = o.n_;
        return *this;
    }
#endif
    double&     operator[](std::size_t i)       { return p_[i]; }
    double      operator[](std::size_t i) const { return p_[i]; }
    std::size_t size() const { return n_; }
 private:
    std::size_t n_;
    double*     p_;
};

int main() {
    PxBuf a(3);
    a[0] = 101.5; a[1] = 101.6; a[2] = 101.7;
    PxBuf b = a;                                // copy ctor
    b[0] = 99.0;                                // must NOT change a
    PxBuf c(1);
    c = a;                                      // copy assignment
    c = c;                                      // self-assignment
    std::vector<PxBuf> v;
    for (int i = 0; i < 5; ++i) v.push_back(a); // reallocates: copies PxBufs around
    std::printf("a[0]=%.1f b[0]=%.1f c[2]=%.1f v.size()=%zu v[4][1]=%.1f\n",
                a[0], b[0], c[2], v.size(), v[4][1]);
}
```

```bash
clang++ -std=c++20 -O0 -g -Wall -fsanitize=address scratch/pxbuf.cpp -o /tmp/pxbuf && /tmp/pxbuf
```

```text
a[0]=101.5 b[0]=99.0 c[2]=101.7 v.size()=5 v[4][1]=101.6
```

(Clang may warn `explicitly assigning value of variable of type 'PxBuf' to
itself` on `c = c;` — that's the self-assignment test, on purpose.) ASan
prints nothing: every buffer freed exactly once.

**Now break it.** Build with `-DBROKEN`, which removes the copy constructor and
copy assignment but keeps the destructor — so the compiler writes its own
member-by-member copies:

```bash
clang++ -std=c++20 -O0 -g -Wall -fsanitize=address -DBROKEN scratch/pxbuf.cpp -o /tmp/pxbuf_bad && /tmp/pxbuf_bad
```

```text
==NNNNN==ERROR: AddressSanitizer: attempting double-free on 0x... in thread T0:
    ...
    #1 0x... in PxBuf::~PxBuf() pxbuf.cpp:...
```

Two objects, one buffer, two `delete[]`s — slide 12. (Build the broken version at
`-O2` and it may appear to "work": correctness runs go at `-O0`.) Say out loud
which line of `main` created the first shared pointer.

## 10. Rule of Five: `noexcept` and `vector` growth (8 min)

`PxBuf` still has no move operations, so every `vector` reallocation deep-copies.
Here is the same buffer with counters and a switch for `noexcept`:

```cpp
// scratch/growth.cpp
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <vector>

static long copies = 0, moves = 0;

template <bool NX>
struct Buf {                                                  // PxBuf + counters
    explicit Buf(std::size_t n) : n_(n), p_(new double[n]()) {}
    ~Buf() { delete[] p_; }
    Buf(const Buf& o) : n_(o.n_), p_(new double[o.n_]) {
        std::copy(o.p_, o.p_ + n_, p_); ++copies;
    }
    Buf& operator=(const Buf& o) {                            // copy-and-swap
        Buf t(o); std::swap(n_, t.n_); std::swap(p_, t.p_); return *this;
    }
    Buf(Buf&& o) noexcept(NX) : n_(o.n_), p_(o.p_) {          // steal, then blank
        o.p_ = nullptr; o.n_ = 0; ++moves;
    }
    Buf& operator=(Buf&& o) noexcept(NX) {
        std::swap(n_, o.n_); std::swap(p_, o.p_); return *this;
    }
    std::size_t n_;
    double*     p_;
};

template <bool NX>
void run(const char* name) {
    copies = moves = 0;
    auto t0 = std::chrono::steady_clock::now();
    std::vector<Buf<NX>> v;                                   // no reserve()
    for (int i = 0; i < 10'000; ++i) v.emplace_back(512);     // 4 KB each
    auto t1 = std::chrono::steady_clock::now();
    std::printf("%-20s copies=%6ld moves=%6ld  %6.2f ms\n", name, copies, moves,
                std::chrono::duration<double, std::milli>(t1 - t0).count());
}

int main() {
    for (int k = 0; k < 2; ++k) {
        run<false>("move NOT noexcept");
        run<true>("move noexcept");
    }
}
```

```bash
clang++ -std=c++20 -O2 -Wall scratch/growth.cpp -o /tmp/growth && /tmp/growth
```

Measured on an Apple M4 (first pair noisier than the second):

```text
move NOT noexcept    copies= 16383 moves=     0   22.76 ms
move noexcept        copies=     0 moves= 16383    4.21 ms
move NOT noexcept    copies= 16383 moves=     0   10.44 ms
move noexcept        copies=     0 moves= 16383    2.88 ms
```

16,383 = 1 + 2 + 4 + … + 8192: every element is relocated at every doubling.
Without `noexcept`, `vector` keeps its strong exception guarantee by **copying**
(a 4 KB allocation + memcpy each time); with it, it moves (16 bytes). Then add
`v.reserve(10'000);` before the loop and run again: zero copies, zero moves —
the hot-path answer.

**Now finish your own `PxBuf`:** add a `noexcept` move constructor and move
assignment to `scratch/pxbuf.cpp` (steal the pointer, null the source — slide
15), and re-run step 9's ASan build. That is the Rule of Five, and it is HW 2's
core.

## 11. `ScopedTimer`: RAII on every exit path (5 min)

```cpp
// scratch/raii.cpp
#include <chrono>
#include <cstdio>
#include <mutex>
#include <stdexcept>

struct ScopedTimer {
    const char* name;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    explicit ScopedTimer(const char* n) : name(n) {}
    ScopedTimer(const ScopedTimer&) = delete;              // one owner of the stamp
    ScopedTimer& operator=(const ScopedTimer&) = delete;
    ~ScopedTimer() {
        auto ns = std::chrono::duration<double, std::nano>(
                      std::chrono::steady_clock::now() - t0).count();
        std::printf("[%s] done after %.0f ns\n", name, ns);
    }
};

std::mutex m;
double pos = 0;

void on_fill(double q) {
    ScopedTimer t{"on_fill"};                 // stamped on EVERY exit
    std::lock_guard<std::mutex> g(m);         // unlocked on EVERY exit
    if (q == 0) return;                       // early return
    if (q < 0) throw std::runtime_error("negative fill");
    pos += q;
}

int main() {
    on_fill(10);
    on_fill(0);
    try { on_fill(-5); } catch (const std::exception& e) { std::printf("caught: %s\n", e.what()); }
    std::printf("pos=%.0f, mutex free again: %s\n", pos, m.try_lock() ? "yes" : "no");
    m.unlock();
}
```

```bash
clang++ -std=c++20 -O0 -g -Wall -fsanitize=address scratch/raii.cpp -o /tmp/raii && /tmp/raii
```

```text
[on_fill] done after 375 ns
[on_fill] done after 125 ns
[on_fill] done after 46333 ns
caught: negative fill
pos=10, mutex free again: yes
```

(Your ns will differ: this is an ASan `-O0` build, meaningless as timing — the
lines are there to show the destructor ran.) Three exits — normal, early return, exception — and the
timer printed and the mutex unlocked on all three. Note the timer line appears
**before** `caught:`: the destructor runs during stack unwinding, before the
`catch` block. (The deck's version has no constructor and is built as an
aggregate; declaring the deleted copies makes it a non-aggregate, so this one
needs the one-line constructor.)

## 12. Layout: `sizeof`, padding, `static_assert` (4 min)

```cpp
// scratch/layout.cpp
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <type_traits>

struct Naive {                 // declared the way people think
    char          side;
    std::uint64_t id;
    double        px;
    std::uint32_t qty;
    char          symbol[8];
};
struct Order {                 // widest members first
    std::uint64_t id;
    double        px;
    char          symbol[8];
    std::uint32_t qty;
    char          side;
};
static_assert(sizeof(Order) == 32, "layout drift");
static_assert(std::is_trivially_copyable_v<Order>);

int main() {
    std::printf("Naive: sizeof=%zu  side@%zu id@%zu px@%zu qty@%zu symbol@%zu\n",
                sizeof(Naive), offsetof(Naive, side), offsetof(Naive, id),
                offsetof(Naive, px), offsetof(Naive, qty), offsetof(Naive, symbol));
    std::printf("Order: sizeof=%zu  id@%zu px@%zu symbol@%zu qty@%zu side@%zu  alignof=%zu\n",
                sizeof(Order), offsetof(Order, id), offsetof(Order, px),
                offsetof(Order, symbol), offsetof(Order, qty), offsetof(Order, side),
                alignof(Order));
}
```

```bash
clang++ -std=c++20 -O0 -g -Wall -fsanitize=address scratch/layout.cpp -o /tmp/layout && /tmp/layout
```

```text
Naive: sizeof=40  side@0 id@8 px@16 qty@24 symbol@28
Order: sizeof=32  id@0 px@8 symbol@16 qty@24 side@28  alignof=8
```

Same five fields, 8 bytes smaller: two `Order`s per 64-byte cache line. Now add
`#include <string>` and a `std::string venue;` member to `Order` and rebuild —
both `static_assert`s fire. That is what they are for.

## 13. Inheritance: slicing, name hiding, static binding (5 min)

**Predict all three lines before running.**

```cpp
// scratch/inherit.cpp
#include <cstdint>
#include <cstdio>
#include <vector>

struct Order       { std::uint64_t id; double px; std::uint32_t qty; };
struct PeggedOrder : Order { double offset; };

struct Venue {
    const char* send(double)                { return "Venue::send(px)"; }
    const char* send(double, std::uint32_t) { return "Venue::send(px, qty)"; }
    const char* name()                      { return "Venue"; }
};
struct Paper : Venue {
    const char* send(double)                { return "Paper::send(px)"; }   // hides BOTH
    const char* name()                      { return "Paper"; }             // hides, no virtual
};

const char* who(Venue& v) { return v.name(); }   // bound at compile time

int main() {
    PeggedOrder pg{{7, 101.5, 100}, -0.01};
    Order o = pg;                                // sliced
    std::vector<Order> book{pg};                 // sliced again
    std::printf("sizeof: Order=%zu PeggedOrder=%zu  o.px=%.1f book[0].id=%llu\n",
                sizeof(Order), sizeof(PeggedOrder), o.px,
                static_cast<unsigned long long>(book[0].id));

    Paper p;
    std::printf("%s | %s\n", p.send(101.5), p.Venue::send(101.5, 10));
    // std::printf("%s\n", p.send(101.5, 10));  // (C) uncomment: why an error?

    std::printf("who(p) = %s\n", who(p));        // Venue or Paper?
}
```

```bash
clang++ -std=c++20 -O0 -g -Wall -fsanitize=address scratch/inherit.cpp -o /tmp/inherit && /tmp/inherit
```

```text
sizeof: Order=24 PeggedOrder=32  o.px=101.5 book[0].id=7
Paper::send(px) | Venue::send(px, qty)
who(p) = Venue
```

Line 1: `o` and `book[0]` are `Order`s — `offset` is gone, silently. Line 2:
`Paper::send` hides **both** base overloads; uncomment **(C)** and clang says
`too many arguments to function call` — add `using Venue::send;` inside `Paper`
and (C) compiles. Line 3: without `virtual`, `who()` calls `Venue::name` because
`v`'s static type is `Venue&`. Session 3 adds one keyword and prices it.


## 14. Take-home: build `starters/hw02` as shipped

```bash
cd starters/hw02
make            # -O2 build + run: prints four tables, all WRONG / 0 — that's your red signal
make debug      # -O0, once, for one sentence in your write-up
```

Read the harness at the top of `hw2.cpp` before you write anything: it batches
calls, warms up, reports p50 / p99 / p99.9 over samples, and calls the two
`sum_*` functions **through a volatile function pointer** — otherwise clang
sees both sides of the call and deletes the 648-byte copy you are trying to
measure (the "cost of a copy" slide). Then read `starters/hw02/README.md`:
it is the rubric.


## Your turn — HW 2 (*Pointers, classes, the Rule of Five & RAII*, 10 pts, due Thu Oct 22, 10:59 pm CT)

HW 2 is one homework in two parts; each task starts from a file above. HW 2's class
is called `PriceBuffer`; it is `PxBuf` from step 9. **Part B rules:** Session 2 material only —
no `virtual`, no smart pointers. The starter for the whole homework is `starters/hw02`
(its README is the rubric).

**Part A — pointers and the cost of a copy (5 pts)**

1. Fill the four TODO blocks in `starters/hw02/hw2.cpp` (swaps, the two sums,
   the two traversals). Table 1 must say `OK` three times, including the
   self-swap `swap_ptr(&a, &a)`.
2. Sweep step 4's vector sum over sizes that fit in L1 (e.g. 16 KB), L2 (1 MB)
   and DRAM (256 MB), and report ns per element for each. Mark where each level
   "falls off".
3. Report every number as p50/p99/p99.9 with warm-up, from `-O2`, with your
   machine, compiler and flags. A benchmark without those is not a result.

**Part B — classes, the Rule of Five and RAII (5 pts)**

4. **(Rule of Three, 1.5 pts — from `pxbuf.cpp`)** Rename to `PriceBuffer`, add
   `const`-correct accessors and an invariant in the constructor, and keep it
   ASan-clean: deep copy, self-assignment safe, no leak, no double free.
5. **(Rule of Five, 1.5 pts — from `growth.cpp`)** `noexcept` moves that leave the
   source empty. Count copies and moves for 1,000 `push_back`s without
   `reserve()`, with and without `noexcept`, and explain the difference. Then
   time copying vs moving a 1 MB buffer (p50/p99, `-O2`, machine stated).
6. **(RAII guard, 1 pt — from `raii.cpp`)** A non-copyable `ScopedTimer` that
   writes the elapsed nanoseconds into a caller-supplied slot, on normal exit and
   when an exception leaves the scope.
7. **(Inheritance, no virtual, 1 pt — from `order_of.cpp`, `layout.cpp`,
   `inherit.cpp`)** `Instrument` with `Equity` and `Future` derived from it; print
   the construction/destruction order for a `Future` holding a `PriceBuffer`
   member, `sizeof` each type and explain the padding, show slicing, and say which
   derived classes follow the Rule of Zero and why that is enough.
8. **(Not graded, midterm practice)** Rewrite `PxBuf` with a member
   `std::vector<double>` and say which of the five special members you could
   delete (answer: all of them — the Rule of Zero).

## Checkpoint

Part A
- `s1`: you can explain the `0.000 ns/op` line.
- `s3`: stack/heap ratio posted, and you can say why the wrong sink reads ~0.2 ns.
- `s4`: list/vector and column/row ratios posted.
- `s5`: one ASan `ERROR:` line and one leak report (`leaks` or LSan) pasted,
  platform stated.
- `s6`: shared/padded ratio posted.
- `starters/hw02` builds and prints its four tables.

Part B
- `order`: three lines, and you've read errors (A) and (B).
- `order_of`: you predicted `+Venue +book +risk +Quoter | -Quoter -risk -book -Venue`.
- `pxbuf`: clean under ASan; `pxbuf_bad`: `attempting double-free`.
- `growth`: 16,383 copies vs 16,383 moves, and your two times.
- `raii`: the timer fired three times; the mutex is free.
- `layout`: 40 vs 32.
- `inherit`: you predicted `who(p) = Venue`.

## Links

Session 2 deck · HW 2 (Canvas, due Thu Oct 22) · `starters/hw02/README.md` ·
`tests/bench.hpp` · Project Phase 0 (due Mon Oct 12) · next: Session 3 — polymorphism & smart pointers —
`python3 scripts/latency_replay.py --self-test --cmd "hft/cpp_client/build/hft_bot --replay"`

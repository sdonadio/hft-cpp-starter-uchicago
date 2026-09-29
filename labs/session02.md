# Session 2 Lab — Pointers & the Cost of Memory

**FINM 32700 · Session 2 · Mon Oct 5** · in class, in pairs (~45 min) + a short take-home tail
**Repo:** your copy of the starter · **Deck:** Session 2 — *Pointers & the Cost of Memory*
**Feeds:** HW 2 — *Pointers, references & the cost of a copy* (due Thu Oct 15, 10:59 pm CT) ·
Project Phase 0 — *Connect & Baseline* (due Mon Oct 12, 10:59 pm CT)

## Goal

Every claim in tonight's deck, measured on **your** machine: an honest
micro-benchmark (and a dishonest one), what `const` and array decay actually do,
stack vs heap, contiguous vs pointer-chasing, row-major vs column-major, the
classic memory bugs caught by a tool, and false sharing removed with
`alignas(64)`. You leave with four timings, one sanitizer report, and the
harness HW 2 is graded on.

| Step | What | Minutes |
|---|---|---|
| 0 | Setup: read `tests/bench.hpp`, make `scratch/` | 3 |
| 1 | An honest micro-benchmark — and a dishonest one | 5 |
| 2 | Pointers, `const` and decay: read the compiler errors | 5 |
| 3 | Stack vs heap — sink the pointer | 5 |
| 4 | Contiguous vs pointer chase; row- vs column-major | 9 |
| 5 | Memory bugs under ASan / `leaks` / LSan | 10 |
| 6 | False sharing and `alignas(64)` | 8 |
| 7 | *Take-home:* build `starters/hw02` as shipped | — |

**Checkpoint at 2:45:** post in the Zoom chat (a) your stack/heap ratio, (b) your
list/vector ratio, (c) your shared/padded ratio, and (d) the first `ERROR:` or
`leak` line from a sanitizer — plus your machine (e.g. "M2 Air, Apple clang 17").

## 0. Setup (3 min)

Two build recipes tonight. Keep them in two terminal tabs and never mix them up:

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
// scratch/s3.cpp
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
clang++ -std=c++20 -O2 -Wall -Itests scratch/s3.cpp -o /tmp/s3 && /tmp/s3
clang++ -std=c++20 -O0 -Itests scratch/s3.cpp -o /tmp/s3_O0 && /tmp/s3_O0   # once, to see the lie
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
// scratch/s4.cpp
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
clang++ -std=c++20 -O2 -Wall -Itests scratch/s4.cpp -o /tmp/s4 && /tmp/s4
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
fetches. This is why an order book is a flat array (Session 6), not a `std::map`
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

Fixing these three for good takes a destructor — Session 3.

## 6. False sharing and `alignas(64)` (8 min)

```cpp
// scratch/s6.cpp
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
clang++ -std=c++20 -O2 -Wall -pthread scratch/s6.cpp -o /tmp/s6 && /tmp/s6
```

Measured on an Apple M4: `shared≈312–400 ms  padded≈80 ms` — about 4×, for
identical atomic work. The two counters are different variables, but a cache
line is the unit of coherence: each write invalidates the other core's copy.
(Apple M-series reports a 128-byte line, `sysctl hw.cachelinesize`, yet 64-byte
padding already removed the effect here — measure, don't assume.) Session 7's
SPSC ring puts `alignas(64)` on its head and tail for exactly this reason.

## 7. Take-home: build `starters/hw02` as shipped

```bash
cd starters/hw02
make            # -O2 build + run: prints four tables, all WRONG / 0 — that's your red signal
make debug      # -O0, once, for one sentence in your write-up
```

Read the harness at the top of `hw2.cpp` before you write anything: it batches
calls, warms up, reports p50 / p99 / p99.9 over samples, and calls the two
`sum_*` functions **through a volatile function pointer** — otherwise clang
sees both sides of the call and deletes the 648-byte copy you are trying to
measure (tonight's "cost of a copy" slide). Then read `starters/hw02/README.md`:
it is the rubric.

## Your turn — the on-ramp to HW 2

1. Fill the four TODO blocks in `starters/hw02/hw2.cpp` (swaps, the two sums,
   the two traversals). Table 1 must say `OK` three times, including the
   self-swap `swap_ptr(&a, &a)`.
2. Sweep step 4's vector sum over sizes that fit in L1 (e.g. 16 KB), L2 (1 MB)
   and DRAM (256 MB), and report ns per element for each. Mark where each level
   "falls off".
3. Report every number as p50/p99/p99.9 with warm-up, from `-O2`, with your
   machine, compiler and flags. A benchmark without those is not a result.

## Checkpoint

- `s1`: you can explain the `0.000 ns/op` line.
- `s3`: stack/heap ratio posted, and you can say why the wrong sink reads ~0.2 ns.
- `s4`: list/vector and column/row ratios posted.
- `s5`: one ASan `ERROR:` line and one leak report (`leaks` or LSan) pasted,
  platform stated.
- `s6`: shared/padded ratio posted.
- `starters/hw02` builds and prints its four tables.

## Links

Session 2 deck · HW 2 (Canvas, due Thu Oct 15) · `starters/hw02/README.md` ·
`tests/bench.hpp` · Project Phase 0 (due Mon Oct 12) —
`python3 scripts/latency_replay.py --self-test --cmd "hft/cpp_client/build/hft_bot --replay"`

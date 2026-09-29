# Session 3 Lab — Classes, the Rule of Five & Inheritance

**FINM 32700 · Session 3 · Mon Oct 12** · in class, in pairs (~45 min) + a take-home tail
**Repo:** your copy of the starter · **Deck:** Session 3 — *Object-Oriented C++ I: Encapsulation & Inheritance*
**Feeds:** HW 3 — *Classes, the Rule of Five & RAII* (due Thu Oct 22, 10:59 pm CT) ·
Project Phase 1 — *The Fast Hot Path* (due Mon Oct 26, 10:59 pm CT)
**Also tonight:** Project Phase 0 is due at **10:59 pm CT**.

## Goal

Write a class that cannot be built invalid; watch construction and destruction
happen in order; build `PxBuf` — a class that owns a price buffer — correctly by
hand, then remove its copy operations and watch ASan catch the double free; add
the two moves and **measure** what `noexcept` buys when a `vector` grows; write an
RAII guard that survives an exception; shrink a struct by reordering it; and see
slicing and name hiding happen. Last week's bugs, fixed by types.

| Step | What | Minutes |
|---|---|---|
| 0 | Setup: `scratch/`, the two build recipes | 2 |
| 1 | `Order`: an invariant, `const`, private data | 6 |
| 2 | Construction & destruction order | 5 |
| 3 | `PxBuf`: Rule of Three — and the double free | 10 |
| 4 | Rule of Five: `noexcept` and `vector` growth | 8 |
| 5 | `ScopedTimer`: RAII on every exit path | 5 |
| 6 | Layout: `sizeof`, padding, `static_assert` | 4 |
| 7 | Inheritance: slicing, name hiding, static binding | 5 |

**Checkpoint at 2:45:** post in the Zoom chat (a) your step-3 ASan `ERROR:` line
from the broken build, (b) your two step-4 lines (copies/moves + ms), and (c) your
step-7 output — plus your machine.

## 0. Setup (2 min)

```bash
mkdir -p scratch                       # git-ignored
# CORRECTNESS (steps 1-3, 5-7): -O0 -g plus AddressSanitizer
clang++ -std=c++20 -O0 -g -Wall -fsanitize=address scratch/FILE.cpp -o /tmp/FILE
# TIMING (step 4 only): -O2
clang++ -std=c++20 -O2 -Wall scratch/FILE.cpp -o /tmp/FILE
```

`g++` behaves the same here. ASan works on macOS for everything tonight (we
need no leak detection), so Mac and Linux users run identical commands.

## 1. `Order`: an invariant, `const`, private data (6 min)

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

## 2. Construction & destruction order (5 min)

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

## 3. `PxBuf`: the Rule of Three — and the double free (10 min)

This class owns a raw `new[]`. HW 3 grows from this file — keep it.

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

## 4. Rule of Five: `noexcept` and `vector` growth (8 min)

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
15), and re-run step 3's ASan build. That is the Rule of Five, and it is HW 3's
core.

## 5. `ScopedTimer`: RAII on every exit path (5 min)

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

## 6. Layout: `sizeof`, padding, `static_assert` (4 min)

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

## 7. Inheritance: slicing, name hiding, static binding (5 min)

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
`v`'s static type is `Venue&`. Session 4 adds one keyword and prices it.

## Your turn — the on-ramp to HW 3

HW 3 is four tasks, and each starts from a file above. HW 3 calls the class
`PriceBuffer`; it is tonight's `PxBuf`. **HW 3 rules:** Session 3 material only —
no `virtual`, no smart pointers.

1. **(Rule of Three, 3 pts — from `pxbuf.cpp`)** Rename to `PriceBuffer`, add
   `const`-correct accessors and an invariant in the constructor, and keep it
   ASan-clean: deep copy, self-assignment safe, no leak, no double free.
2. **(Rule of Five, 3 pts — from `growth.cpp`)** `noexcept` moves that leave the
   source empty. Count copies and moves for 1,000 `push_back`s without
   `reserve()`, with and without `noexcept`, and explain the difference. Then
   time copying vs moving a 1 MB buffer (p50/p99, `-O2`, machine stated).
3. **(RAII guard, 2 pts — from `raii.cpp`)** A non-copyable `ScopedTimer` that
   writes the elapsed nanoseconds into a caller-supplied slot, on normal exit and
   when an exception leaves the scope.
4. **(Inheritance, no virtual, 2 pts — from `order_of.cpp`, `layout.cpp`,
   `inherit.cpp`)** `Instrument` with `Equity` and `Future` derived from it; print
   the construction/destruction order for a `Future` holding a `PriceBuffer`
   member, `sizeof` each type and explain the padding, show slicing, and say which
   derived classes follow the Rule of Zero and why that is enough.
5. **(Not graded, midterm practice)** Rewrite `PxBuf` with a member
   `std::vector<double>` and say which of the five special members you could
   delete (answer: all of them — the Rule of Zero).

## Checkpoint

- `order`: three lines, and you've read errors (A) and (B).
- `order_of`: you predicted `+Venue +book +risk +Quoter | -Quoter -risk -book -Venue`.
- `pxbuf`: clean under ASan; `pxbuf_bad`: `attempting double-free`.
- `growth`: 16,383 copies vs 16,383 moves, and your two times.
- `raii`: the timer fired three times; the mutex is free.
- `layout`: 40 vs 32.
- `inherit`: you predicted `who(p) = Venue`.

## Links

Session 3 deck · HW 3 (Canvas, due Thu Oct 22) · Project Phase 1 (due Mon Oct 26) ·
Session 2 lab step 5 (the sanitizers) · next: Session 4 — polymorphism & smart pointers

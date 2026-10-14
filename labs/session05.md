# Session 5 — Midterm (no lab tonight)

**FINM 32700 · Session 5 · Mon Oct 26** · the midterm, remote, **2 h 20 min**, on Canvas.
There is no lecture and no lab tonight.

> **Looking for the memory pools & order-book lab?** It is now
> [`labs/session06.md`](session06.md) (Session 6, Mon Nov 2). The file comments in
> `include/pool.hpp`, `include/order_book.hpp`, `include/rolling_counter.hpp`,
> `tests/pool_test.cpp`, `tests/book_test.cpp`, `tests/rolling_counter_test.cpp` and the
> `Makefile` still say "Session 5" / `labs/session05.md`: they mean `labs/session06.md`.
> Its Part C drivers stay in `starters/session05/` (the Makefile builds them from there).

## Tonight
- **Midterm:** remote, 2 h 20 min, on Canvas. Scope: **Sessions 1–4, including templates &
  CRTP**.
- **Project Phase 1 — *The Fast Hot Path*** is due **tonight, Mon Oct 26, 10:59 pm CT**.
  Submit what works.
- **HW 4 — *Templates & CRTP*** is still due **Thu Nov 5, 10:59 pm CT** (unchanged).
- **Next session:** Mon Nov 2, Session 6, memory pools & the order book →
  [`labs/session06.md`](session06.md). Project Phase 2 (*The Local Order Book*) is due
  Thu Nov 5 and builds on it.

## What to revise
Re-run the measured steps of each lab and be able to explain the numbers, not just quote them.

**Session 1 — The HFT landscape, market microstructure & LOB metrics** ([`labs/session01.md`](session01.md))
- the limit order book: bids, asks, the touch, spread, depth, price-time priority, maker
  vs taker fees
- the book metrics: mid, spread, microprice, order-book imbalance, and what each one says
- tick-to-trade latency, and why you report p50 / p99 / p99.9 rather than the mean

**Session 2 — Pointers & the cost of memory · Classes, encapsulation & inheritance** ([`labs/session02.md`](session02.md))
- pointers vs references, `const`, array decay, the cost of a copy, pass by value vs by `const&`
- stack vs heap; the memory hierarchy: cache lines, the prefetcher, pointer chasing, false sharing
- RAII, the Rule of Three / Five (and of Zero), `noexcept` moves and `vector` growth,
  ASan-clean code
- encapsulation (invariants behind `private`), construction/destruction order, layout and
  padding, inheritance: slicing, name hiding, static binding

**Session 3 — Polymorphism & smart ownership** ([`labs/session03.md`](session03.md))
- interfaces, `virtual`, `override`, `final`, virtual destructors; the object model
  (vptr, `sizeof`), what a virtual call costs (`make dispatch`) and when `final` removes it
- `unique_ptr` vs `shared_ptr` vs `weak_ptr`: ownership, sizes and allocations, the
  reference-count cost under contention, breaking a cycle
- counting the heap allocations on your own hot path

**Session 4 — Templates, compile-time & CRTP** ([`labs/session04.md`](session04.md))
- function and class templates, full and partial specialization, packs and folds
- SFINAE vs concepts (`requires`), and reading the two error messages
- `constexpr` / `consteval` / `if constexpr` / `static_assert`, `std::variant` + `std::visit`
- CRTP: static polymorphism, and why the CRTP row of `dispatch_bench.cpp` matches the
  direct call rather than the virtual one; policy-based design

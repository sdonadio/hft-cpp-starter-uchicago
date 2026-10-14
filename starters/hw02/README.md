# HW 2 — Pointers, classes, the Rule of Five & RAII

**10 points · team work (your 2026Autumn project group, one submission per group) · due Thu Oct 22, 2026, 10:59 pm CT · submit on Canvas**

Two parts, one submission. **Part A** (5 pts) is the pointers-and-cost-of-a-copy harness in this folder (`hw2.cpp`). **Part B** (5 pts) is the classes / Rule of Five / RAII work from the Session 2 lab, Part B (`labs/session02.md`, steps 7–13): you write those classes in your own files (e.g. `price_buffer.cpp`, `raii.cpp`, `instrument.cpp`) next to `hw2.cpp` — see "Part B" below.

Part A. Session 2 said: *a reference is an alias, a pointer is an address, a copy costs
bytes, and a cache miss costs ~100 ns.* This homework makes you prove all four
with a clock instead of taking my word for it.

---

## Build and run — do this first, before you write anything

```bash
cd starters/hw02
make                 # == g++ -std=c++17 -O2 -Wall hw2.cpp -o hw2 && ./hw2
```

It compiles as shipped and prints all four tables. The numbers are wrong
(`WRONG`, `0.000`, `1.00x`) because the four functions are empty. That is your
red signal. Your job is to turn them green.

> Note what the shipped, empty version prints for Table 2: **the same time for
> by-value and by-const-ref**. An empty function has nothing to copy, so there
> is nothing to measure. Remember that shape — an identical p50 on both rows is
> exactly what an *elided* benchmark looks like too, and it is the most common
> way this homework loses points.

You will also want, once, for your write-up:

```bash
make debug           # -O0. Compare the numbers. Then never report them.
```

---

## What to fill in

Four TODO blocks in `hw2.cpp`. **The signatures are fixed — do not change them.**

| TODO | Function | Points |
|------|----------|--------|
| 1a / 1b | `void swap_ref(int&, int&)` and `void swap_ptr(int*, int*)` | 1 |
| 2a / 2b | `double sum_by_value(Big)` and `double sum_by_cref(const Big&)` | 1.5 |
| 3a / 3b | `long sum_vector(const std::vector<int>&)` and `long sum_list(const Node*)` | 1.5 |
| 4 | nothing to code — report the build, the method and your machine | 1 |

Everything else (the percentile harness, the data setup, the shuffled list, the
four tables) is written for you. Read the harness — you are expected to be able
to explain what it does.

In `sum_by_value` and `sum_by_cref` the **bodies must be identical**. The only
difference between them is how the argument arrives; if you also change the
arithmetic you are no longer measuring the copy.

---

## What to report

A short `README.md` or PDF alongside your `hw2.cpp`. It needs:

### 1. Your machine and build (1 pt lives here)

State all of it — a benchmark without a machine is not a result:

- CPU (model, core count), RAM, OS
- Compiler and exact version (`g++ --version`)
- The exact flags you used: `-std=c++17 -O2 -Wall`
- That you used `std::chrono::steady_clock`, that you warmed up, and how many
  samples you took
- Anything you did about noise (closed other apps, laptop on power, etc.)

### 2. The four tables

Paste the program's own output, or retype it. Table 2 and Table 3 must show
**p50, p99 and p99.9** — a mean alone is not a distribution and loses a point.

### 3. Explanation A — `swap_ref` vs `swap_ptr` (part of the 1 pt)

A short paragraph. Cover, in your own words:

- What the callee actually receives in each case, and what it costs
- Why `swap_ptr` needs `*` and `swap_ref` does not
- Why swapping the *pointers* inside `swap_ptr` does nothing to the caller
- `nullptr` is expressible for the pointer version and not for the reference
  version — say what that means for a caller, and whether you added a null
  check (either choice is fine; say which and why)
- Which one you would use in an HFT hot path and why

### 4. Explanation B — contiguous vs pointer chasing (part of the 1.5 pts)

Same amount of arithmetic, same number of `int`s summed, very different time.
Explain *why*, in terms of the Session 2 memory hierarchy:

- cache lines: how many `int`s ride along in one 64-byte line vs how many
  useful bytes you get from a 16-byte `Node`
- the hardware prefetcher: why it helps the vector and cannot help the chase
- the dependent-load chain: each `p->next` must complete before the next hop
  can even start, so misses serialise instead of overlapping
- where this shows up in our course: it is the reason the order book (Session 6, HW 5) is
  a flat array and not a `map` of nodes

Also say something about the **copy** result: is the by-value overhead roughly
what you would predict from `sizeof(Big)` and your machine's memory bandwidth,
or not? Order-of-magnitude reasoning is enough.

---

## Submission checklist (this is the rubric)

**Part A — pointers and the cost of a copy (5 pts)**

- [ ] **(1)** `swap_ref` and `swap_ptr` both print `OK` in Table 1, and the
      difference between references and pointers is explained in prose.
- [ ] **(1.5)** `sum_by_value` vs `sum_by_cref` benchmarked, both correctness-checked
      as equal and non-zero, with the p50/p99/p99.9 table and the ratio.
- [ ] **(1.5)** Contiguous vs pointer-chasing traversal of the **same** 1,048,576
      elements, measured, with the cache explanation.
- [ ] **(1)** Every timing from an `-O2` (or `-O3`) build, with warm-up,
      percentiles reported, and your machine + compiler + flags stated.

**Part B — classes, the Rule of Five & RAII (5 pts)** — Session 2 material only:
no `virtual`, no smart pointers. Each task starts from a lab file
(`labs/session02.md`, steps 9–13); keep every program ASan-clean (`-O0 -g
-fsanitize=address`), and time with `-O2`.

- [ ] **(1.5) Rule of Three — `PriceBuffer`** (from the lab's `pxbuf.cpp`): rename
      to `PriceBuffer`; `const`-correct accessors and an invariant in the
      constructor; deep copy, self-assignment safe, no leak, no double free.
- [ ] **(1.5) Rule of Five** (from `growth.cpp`): `noexcept` moves that leave the
      source empty. Count copies and moves for 1,000 `push_back`s without
      `reserve()`, with and without `noexcept`, and explain the difference. Then
      time copying vs moving a 1 MB buffer (p50/p99, `-O2`, machine stated).
- [ ] **(1) RAII guard** (from `raii.cpp`): a non-copyable `ScopedTimer` that writes
      the elapsed nanoseconds into a caller-supplied slot, on normal exit and when
      an exception leaves the scope.
- [ ] **(1) Inheritance, no `virtual`** (from `order_of.cpp`, `layout.cpp`,
      `inherit.cpp`): `Instrument` with `Equity` and `Future` derived from it; print
      the construction/destruction order for a `Future` holding a `PriceBuffer`
      member, `sizeof` each type and explain the padding, show slicing, and say
      which derived classes follow the Rule of Zero and why that is enough.
- [ ] *Not graded (midterm practice):* rewrite `PxBuf` with a member
      `std::vector<double>` and say which of the five special members you could
      delete (the Rule of Zero).

**Submission**

- [ ] `hw2.cpp`, your Part B source files and the write-up attached to Canvas (`.cpp` + `.md`/`.pdf`,
      or a `.zip`, or a link to your repo — a repo link still needs the write-up).

**Not graded:** false sharing. We do it in the Session 2 lab (`labs/session02.md`, Part A step 6); it is not on this
task list.

---

## Traps that cost points every year

1. **No `doNotOptimize`.** `-O2` deletes a result nobody uses, and you report
   0 ns or two identical rows. The harness sinks every result for you — do not
   remove it, and if you write extra benchmarks of your own, sink those too.
2. **Swapping the pointers, not the pointees.** `int* t = a; a = b; b = t;`
   inside `swap_ptr` reorders two local copies of two addresses. The caller
   never sees it. Table 1 will say `WRONG`.
3. **Reporting `-O0` numbers.** A debug build hides the effect you were asked to
   measure behind its own overhead. `make debug` exists so you can see this
   happen once, on purpose, and write a sentence about it.
4. **A mean instead of percentiles.** One number is a lie; the tail is the
   grade in this course.
5. **Different bodies in the two `sum_*` functions.** Then the gap is your
   arithmetic, not the copy.
6. **"The list is slower because pointers are slow."** Pointers are not slow —
   a dereference is one instruction. *Missing the cache* is slow. Say that.

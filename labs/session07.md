# Session 7 Lab — Concurrency: From Atomics to Lock-Free

**Format:** in class, guided, in breakout pairs (~45 min), plus a take-home tail.
**Repo:** this starter. **Deck:** Session 7, *Concurrency — From Atomics to Lock-Free*.

## Goal
Watch a data race happen, fix it correctly, and then build the lock-free queue
your bot will use to hand ticks from the socket thread to the strategy thread:

1. Reproduce a **data race**, let **ThreadSanitizer** name it, and fix it with
   `std::atomic`.
2. Publish data between threads with **release/acquire**, break it on purpose,
   and see why a mutex is correct but slow in the tail.
3. Build **`include/spsc_ring.hpp`**, a single-producer/single-consumer
   lock-free ring buffer, and prove it with the grader's stress and TSan runs.
4. Build **`include/shm_ring.hpp`**, the same ring in shared memory, and move
   data between two **processes**.

The work maps onto three deliverables:
- **HW 7 — An SPSC lock-free ring buffer** (10 pts, due **Thu Nov 19, 10:59 pm CT**) is Part C.
- **Project Phase 3 — Threading to Accelerate** (due **this Thursday, Nov 12, 10:59 pm CT**) wires Part C into your bot.
- **Project Phase 4 — Multi-Process System with Shared-Memory Lock-Free IPC** (due **Thu Nov 19, 10:59 pm CT**) builds on Part D.

| Part | What | Where | In class? |
|---|---|---|---|
| A | Race, TSan, atomic fix, the tail of a lock | `starters/session07/race.cpp`, `lock_tail.cpp` | yes |
| B | Release/acquire, and a real reordering | `starters/session07/handoff.cpp`, `litmus_sb.cpp` | yes |
| C | The SPSC ring | `include/spsc_ring.hpp`, `make spsc` / `spsc-conc` / `spsc-tsan` | yes |
| D | Across processes | `include/shm_ring.hpp`, `make shm`, `starters/session07/shm_pipeline.cpp` | start in class |
| E | Latency vs a mutex queue, Phase 3/4 wiring, a stretch optimization | your bot | take-home |

## Setup, and the one thing to check first
```bash
cd hft-cpp-starter-uchicago
mkdir -p /tmp/s7
# the flags this lab uses (they match tests/run_ci.py):
#   normal: -std=c++17 -O2 -pthread
#   TSan:   -std=c++17 -O1 -g -pthread -fsanitize=thread
```
**Check that ThreadSanitizer works on your machine before anything else.** On
Linux, `g++` and `clang++` both ship it. On macOS, Apple clang supports it
(`g++` there is Apple clang in disguise). On Windows, use WSL2. If it won't link,
pair with someone whose machine works. Every breakout room needs at least one
working TSan.
```bash
g++ -std=c++17 -O1 -g -pthread -fsanitize=thread starters/session07/race.cpp -o /tmp/s7/race_tsan \
  && /tmp/s7/race_tsan 2>&1 | head -3
# WARNING: ThreadSanitizer: data race (pid=...)
```
All `-pthread` commands below compile with `-std=c++17` and with `-std=c++20`.

---

## Part A — The race, and what a lock costs

### A1. Reproduce the race
`starters/session07/race.cpp` has two threads each doing `++counter` a million
times on a plain `long`.
```bash
g++ -std=c++17 -O0 -pthread starters/session07/race.cpp -o /tmp/s7/race
/tmp/s7/race; /tmp/s7/race; /tmp/s7/race
```
On an Apple M4 this gave `1002524`, `1019813` and `998937`, never 2,000,000,
and a different number every run. `++` is a load, an add and a store. The two
threads interleave, and updates get lost.

Now build the **same file at `-O2`**:
```bash
g++ -std=c++17 -O2 -pthread starters/session07/race.cpp -o /tmp/s7/race_o2 && /tmp/s7/race_o2
# counter = 2000000 (expected 2000000)      <- on the M4, every run
```
The optimizer assumed there's no race, which it's allowed to do because a race
is UB, and turned each loop into a single `counter += 1000000`. It's still a
race. You just got lucky. **That's what undefined behaviour means:** the
program's meaning isn't "a wrong number", it's whatever the compiler did with
your code.

### A2. Let ThreadSanitizer name it
```bash
/tmp/s7/race_tsan 2>&1 | head -12
```
TSan prints `WARNING: ThreadSanitizer: data race` with **both** stacks, the
write and the conflicting access, both inside `work()`. The grader greps for
the string `ThreadSanitizer`, so on your ring any report means the TSan check
fails.

### A3. The wrong fix: `volatile`
```bash
g++ -std=c++17 -O2 -pthread -DVOLATILE starters/session07/race.cpp -o /tmp/s7/race_v && /tmp/s7/race_v
# counter = 1004432 (expected 2000000)
g++ -std=c++17 -O1 -g -pthread -fsanitize=thread -DVOLATILE starters/session07/race.cpp \
    -o /tmp/s7/race_vt && /tmp/s7/race_vt 2>&1 | grep -m1 WARNING
# WARNING: ThreadSanitizer: data race
```
`volatile` stops the compiler from folding the loop, so now you *see* the lost
updates, even at `-O2`. It makes no access atomic and creates no happens-before
edge. `volatile` is for memory-mapped hardware, not for threads.

### A4. The right fix: `std::atomic`
```bash
g++ -std=c++17 -O2 -pthread -DFIX_ATOMIC starters/session07/race.cpp -o /tmp/s7/fixed && /tmp/s7/fixed
g++ -std=c++17 -O1 -g -pthread -fsanitize=thread -DFIX_ATOMIC starters/session07/race.cpp \
    -o /tmp/s7/fixed_tsan && /tmp/s7/fixed_tsan
# counter = 2000000 (expected 2000000)   -- both builds, every run, no TSan warning
```
Why `memory_order_relaxed` is enough here: you only need each increment to be
**atomic**. No *other* data is being published along with the counter, so you
don't need any ordering. Keep that distinction in mind for Part B.

### A5. What a lock costs, in the tail
`lock_tail.cpp` runs 4 threads doing the same increment three ways: under a
`std::mutex`, as an atomic `fetch_add`, and on a private per-thread counter.
It reports ns per increment, timed in batches of 64 (a laptop clock can't time
a single 5 ns operation).
```bash
g++ -std=c++17 -O2 -pthread starters/session07/lock_tail.cpp -o /tmp/s7/lock_tail && /tmp/s7/lock_tail
```
One run on an Apple M4:
```text
mutex    mean    51.3  p50     4.6  p99    382.2  p99.9     749.3  max     3429.0  ns/op
atomic   mean    36.6  p50    37.1  p99     63.8  p99.9     220.0  max      593.1  ns/op
private  mean     0.7  p50     0.7  p99      1.3  p99.9       2.0  max        3.9  ns/op
```
Read it. **The mutex wins at the median** (4.6 vs 37 ns), because the thread
holding the lock runs a burst of uncontended increments. It **loses at p99.9**
(3–13× worse over five runs: 0.75–1.6 µs against 0.12–0.26 µs), because the
losing threads sleep in the kernel and wake when the scheduler decides. The max
is noisy for both: one run hit 77 µs for the mutex, another 7 µs for the atomic. The
atomic is slow because four cores are fighting over one cache line, but its
tail is bounded. The real answer is `private`: **don't share**. The SPSC ring
is the closest you can get while still sharing: each index has exactly one
writer. Write one sentence tying this to how the arena grades you (p99.9, not
the mean).

---

## Part B — Ordering: publish data safely

### B1. Release/acquire handoff
`starters/session07/handoff.cpp` has a producer that writes a plain `Quote`
and then does `ready.store(true, release)`. The consumer spins on
`ready.load(acquire)` and then reads the quote.
```bash
g++ -std=c++17 -O2 -pthread starters/session07/handoff.cpp -o /tmp/s7/handoff && /tmp/s7/handoff
g++ -std=c++17 -O1 -g -pthread -fsanitize=thread starters/session07/handoff.cpp \
    -o /tmp/s7/handoff_tsan && /tmp/s7/handoff_tsan
# seq 42  100.01 / 100.03     -- and no TSan warning
```
- **release**: no earlier write in this thread may move *after* this store.
- **acquire**: no later read in this thread may move *before* this load.
- A release store that is read by an acquire load **synchronizes-with** it.
  That makes the payload write *happen-before* the payload read.

### B2. Break it on purpose
```bash
g++ -std=c++17 -O1 -g -pthread -fsanitize=thread -DBROKEN starters/session07/handoff.cpp \
    -o /tmp/s7/handoff_broken && /tmp/s7/handoff_broken 2>&1 | grep -m1 WARNING
# WARNING: ThreadSanitizer: data race
```
`-DBROKEN` makes both operations `relaxed`. It probably still *prints* the right
quote on your laptop. Write a two-line comment explaining why it's unsound
anyway: there's no synchronizes-with edge, so the plain reads of `q` race with
the plain writes, and nothing stops the compiler or CPU from making the flag
visible before the payload.

### B3. A reordering you can count
```bash
for d in "" -DACQ_REL -DSEQ_CST; do
  g++ -std=c++17 -O2 -pthread $d starters/session07/litmus_sb.cpp -o /tmp/s7/sb && /tmp/s7/sb
done
```
Thread A does `x = 1; r1 = y;` and thread B does `y = 1; r2 = x;`. Can both
threads read 0? On an Apple M4:
```text
r1 == 0 && r2 == 0 in 199966 of 200000 trials (relaxed)
r1 == 0 && r2 == 0 in 0 of 200000 trials (release/acquire)
r1 == 0 && r2 == 0 in 0 of 200000 trials (seq_cst)
```
The C++ model **allows** both-zero under relaxed *and* under release/acquire
(the store buffer lets each core's load run ahead of its own store). Only
`seq_cst` forbids it. The M4 happened to show 0 for release/acquire, because
the Arm instructions clang emits are stronger than C++ requires. **Not seeing
an allowed outcome proves nothing.** Reason from the model, not from one run on
one CPU.

---

## Part C — The SPSC ring (`include/spsc_ring.hpp`, HW 7)

```bash
make spsc        # tests/spsc_correctness.cpp against your header: RED on the stub
```
The contract, fixed by the grader (read `tests/spsc_correctness.cpp`):
`SPSCRing(capacity_pow2)`, `push(v) -> bool` (false when full),
`pop(out) -> bool` (false when empty), `empty()`, `full()`, with
`std::uint64_t` items.

> **Naming convention, used by the deck, this lab and the grader:** the
> **producer** writes `tail_` and the **consumer** writes `head_`. Size is
> `tail_ - head_`. Pick this convention and stick to it.

### C1. Why SPSC needs no CAS
Exactly one thread pushes and exactly one thread pops, so **each index has
exactly one writer**. No two threads ever race to update the same index, so
plain atomic loads and stores with the right ordering are enough. There's no
retry loop, so the ring is wait-free, and because indices are never reused
there's no ABA problem.

### C2. Layout: power-of-two capacity, monotonic counters, padded indices
```cpp
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

struct SPSCRing {
    explicit SPSCRing(std::size_t capacity_pow2)
        : cap_(capacity_pow2), mask_(capacity_pow2 - 1), buf_(capacity_pow2) {}
    // push / pop / empty / full: C3-C5
private:
    alignas(64) std::atomic<std::size_t> tail_{0};   // producer writes, consumer reads
    alignas(64) std::atomic<std::size_t> head_{0};   // consumer writes, producer reads
    alignas(64) const std::size_t cap_, mask_;       // read-only after construction
    std::vector<std::uint64_t> buf_;
};
```
- **`& mask_`, not `% cap_`:** `%` is an integer division, 20–40 cycles, on
  every push and pop. `&` takes one cycle, but it's only correct when the
  capacity is a power of two.
- **Monotonic counters:** `tail_` and `head_` only ever increase, and you mask
  them when you use them. Empty is `head == tail` and full is
  `tail - head == cap`. Unsigned wraparound is well defined and the difference
  stays correct, so you get all `cap` slots with none wasted.
- **`alignas(64)` on each index:** if `tail_` and `head_` share a 64-byte line,
  every store by one core invalidates the other core's copy. The two cores
  ping-pong one line, which is correct but silently slow. The third
  `alignas(64)` keeps the read-only fields off both hot lines.

### C3. `push()`: the producer
```cpp
    bool push(std::uint64_t v) {
        const std::size_t t = tail_.load(std::memory_order_relaxed);   // mine: nobody else writes it
        const std::size_t h = head_.load(std::memory_order_acquire);   // theirs: see the slots they freed
        if (t - h == cap_) return false;                               // full: back-pressure, not a stall
        buf_[t & mask_] = v;                                           // (1) write the payload...
        tail_.store(t + 1, std::memory_order_release);                 // (2) ...then publish it
        return true;
    }
```
The **release** store on `tail_` guarantees that the payload write (1) is
visible to any thread that acquires the new `tail_`. That one pairing is the
whole correctness argument.

### C4. `pop()`: the consumer. Write it yourself.
It's the exact mirror of `push`:
- your own index is `head_`, so load it `relaxed`;
- the other side's index is `tail_`, so load it `acquire` (this pairs with the
  producer's release);
- if they're equal the ring is empty, so return `false`;
- read `buf_[h & mask_]` into `out`, **then** `release`-store `h + 1` into `head_`,
  which hands the slot back to the producer.

<details><summary>Stuck after 10 minutes? Reveal <code>pop()</code>.</summary>

```cpp
    bool pop(std::uint64_t& out) {
        const std::size_t h = head_.load(std::memory_order_relaxed);   // mine
        const std::size_t t = tail_.load(std::memory_order_acquire);   // pairs with push's release
        if (h == t) return false;                                      // empty
        out = buf_[h & mask_];                                         // read the payload...
        head_.store(h + 1, std::memory_order_release);                 // ...then free the slot
        return true;
    }
```
</details>

### C5. `empty()` / `full()`
```cpp
    bool empty() const {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }
    bool full() const {
        return tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire) == cap_;
    }
```
From another thread these are only snapshots. Use them for tests and metrics,
never to decide whether to call `push`. Just call `push` and check what it
returns.

### C6. Green, then prove it
```bash
make spsc        # basic, empty_pop, full, fifo, wrap: all pass
make spsc-conc   # 2M items across two threads: RESULT|concurrent|pass + THROUGHPUT
make spsc-tsan   # the same driver under -fsanitize=thread, 200k items: NO "ThreadSanitizer" lines
```
On an Apple M4, `make spsc-conc` measured 23–36 M items/s in most runs, and
91 M and 179 M in two of 18. Where the OS puts the two threads matters, and a laptop doesn't
let you pin them. If TSan reports a race, you almost certainly weakened an
`acquire` or `release` to `relaxed`. Fix the pairing, not the symptom.

`make test` then scores all seven checks: `basic, empty_pop, full, fifo, wrap,
concurrent, tsan`.

---

## Part D — Across processes (`include/shm_ring.hpp`, Phase 4)

The same algorithm, with one extra rule: the ring must mean the same thing in
two address spaces. So it contains **no pointers** (store indices), no
`std::string` or `std::vector` (they own heap memory in one process), and only
**lock-free** atomics (a hidden lock would live in one process's memory).

### D1. Fill in the POD ring
```cpp
#pragma once
#include <atomic>
#include <cstdint>

struct ShmRing {
    static constexpr uint32_t CAPACITY = 1024;               // power of two
    static_assert((CAPACITY & (CAPACITY - 1)) == 0, "CAPACITY must be a power of two");
    static_assert(std::atomic<uint32_t>::is_always_lock_free,
                  "a lock-based atomic cannot be shared across processes");

    void init() {                                            // creator, once, before the other side attaches
        head.store(0, std::memory_order_relaxed);
        tail.store(0, std::memory_order_relaxed);
    }
    bool push(uint64_t v) {                                  // producer process
        uint32_t t = tail.load(std::memory_order_relaxed);
        if (t - head.load(std::memory_order_acquire) == CAPACITY) return false;
        buf[t & (CAPACITY - 1)] = v;
        tail.store(t + 1, std::memory_order_release);
        return true;
    }
    bool pop(uint64_t& out);                                 // consumer process: C4's mirror, you write it

    alignas(64) std::atomic<uint32_t> head;                  // consumer writes
    alignas(64) std::atomic<uint32_t> tail;                  // producer writes
    alignas(64) uint64_t buf[CAPACITY];                      // inline data, no pointers
};
```
Define `pop` in the header, either inline in the struct or as an `inline`
out-of-class definition. It's C4 with `head`, `tail` and `CAPACITY`. The members
stay public and in this order, because that's the layout the stub declares.

### D2. The grader's test: one mapping, `fork()`
```bash
make shm
# RESULT|shm_spsc|pass|moved 500000 items across processes, FIFO
# METRIC|shm_ops_per_sec|...
```
`tests/shm_ring_test.cpp` maps an anonymous `MAP_SHARED` region, calls `init()`,
and then `fork()`s. The child produces and the parent consumes, both through
your ring. On the M4 it measured 51–71 M ops/s.

### D3. Two separate programs: `shm_pipeline.cpp`
Phase 4 needs two *unrelated* processes, so the region needs a name:
`shm_open` + `ftruncate` + `mmap`.
```bash
g++ -std=c++17 -O2 -Iinclude starters/session07/shm_pipeline.cpp -o /tmp/s7/shm_pipe
/tmp/s7/shm_pipe strategy &        # creates "/finm32700_ring", init()s it, consumes
/tmp/s7/shm_pipe feed              # attaches to the same object, produces 1,000,000 ticks
# strategy: ring ready, waiting for the feed...
# feed: done
# strategy: 1000000 ticks in order across processes, 22.1 M/s   (22-100 M/s run to run on an M4)
```
Read `map_ring()`. The creator calls `ftruncate` and `init()` **once**, before
the feed attaches. The feed maps without `O_CREAT` and never calls `init()`.
(macOS: names are at most 31 characters. Older glibc may need `-lrt`.)

---

## Part E — Take-home tail

### E1. Latency: a mutex queue vs your ring
```bash
g++ -std=c++17 -O2 -pthread -Iinclude starters/session07/queue_latency.cpp -o /tmp/s7/qlat && /tmp/s7/qlat
```
The producer stamps an item every ~250 ns, and the consumer records one-way
latency. Here's one run on the M4:
```text
mutex+deque p50    209  p99    5584  p99.9    10625  max    65541  ns
SPSCRing    p50     83  p99     125  p99.9      667  max     8792  ns
```
Over 13 runs in two sittings, the SPSC p50 was 83–125 ns (83 is two steps of
the laptop clock, so the ring is faster than the clock can resolve), against
0.2–1.3 µs for the mutex queue. At p99.9 it was 0.2–40 µs against 9–220 µs. A
laptop with no core pinning adds scheduler noise to both, run to run, but the
gap survives it. Now change the ring size in `main()` to `SPSCRing r(16);` and
re-run. What happens to the tail, and why?

### E2. Wire it into your bot (Project Phase 3, due Thu Nov 12)
- **Producer:** `on_book` runs on the client's receive thread. Copy the tick
  into a small POD (symbol *id*, not a `std::string`, plus bid/ask/microprice/obi
  and a receive timestamp), `push` it, and return. If `push` fails, **count the
  drop** in a relaxed atomic and move on. Never spin on the socket thread.
- **Consumer:** a `std::jthread` running your strategy loop. It drains the
  ring, keeps only the latest tick per symbol (coalescing), and then decides.
  The deck's Session-7 code slide shows the shape. Your HW 7 ring holds
  `uint64_t`; for the bot, template the same algorithm on `T` (Session 5).
- **Latency:** the client's order helpers only time orders sent *inside*
  `on_book`. Orders sent from the strategy thread must record
  `now - tick.recv` themselves (`record_latency` is a protected member of
  `ArenaClient`).
- **Replay:** keep `--replay` working. `scripts/latency_replay.py` expects one
  output line per input line, so in replay mode, process each tick
  synchronously.
- **Audit:** the client's book cache and latency histogram take a `std::mutex`
  (`hft/cpp_client/include/arena_client.hpp`). List every lock on your
  tick-to-order path in your Phase 3 write-up.

### E3. Plan Phase 4 (due Thu Nov 19)
Split the bot into a **feed/gateway process** (socket, decode, push) and a
**strategy process** (pop, decide, send), connected by your `ShmRing`.
`shm_pipeline.cpp` is the skeleton. Decide who creates and `init()`s the
region, what happens if the strategy restarts while the feed is running, and
what a tick looks like when it has to fit in `uint64_t` (or change the payload
to a fixed-size POD struct).

### E4. Stretch: cache the other side's index
Each `push` reads `head_`, a line the consumer keeps writing, so every call
transfers a cache line. Keep a plain `head_cache_` member next to `tail_`
(written only by the producer), and only re-read `head_` with acquire when the
ring *looks* full. Mirror it on the consumer side with `tail_cache_`. The deck's
appendix A2 has the code. Re-measure with `make spsc-conc` and E1, and stay
TSan-clean.

## Checkpoint
- A TSan `data race` report captured (A2), and the same program TSan-clean
  after the atomic fix (A4).
- One line each: what release guarantees, what acquire guarantees, and why the
  ring needs both.
- `make spsc`, `make spsc-conc`, `make spsc-tsan`: seven checks green and zero
  `ThreadSanitizer` lines.
- `make shm` passes. `shm_pipeline` moves 1,000,000 ticks in order between two
  processes.
- One sentence: **which two lines make the ring correct?** (The producer's
  release store of `tail_` and the consumer's acquire load of it, plus the
  mirror pair on `head_`.)

## Links
- Edit: `include/spsc_ring.hpp`, `include/shm_ring.hpp`
- Contracts (read-only): `tests/spsc_correctness.cpp`, `tests/spsc_concurrency.cpp`, `tests/shm_ring_test.cpp`
- How CI runs TSan (flags, and the grep for `ThreadSanitizer`): `tests/run_ci.py`
- Lab programs: `starters/session07/race.cpp`, `handoff.cpp`, `litmus_sb.cpp`, `lock_tail.cpp`, `queue_latency.cpp`, `shm_pipeline.cpp`
- Project: `project/README.md` (Phases 3 and 4), `docs/HFT_CPP_CLIENT.md`
- Build: `make spsc`, `make spsc-conc`, `make spsc-tsan`, `make shm`, full grade `make test`

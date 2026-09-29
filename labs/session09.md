# Session 9 Lab — Profile, Fix, Race

**FINM 32700 · Session 9 · Mon Nov 30** · in class, in pairs (~30 min) + the tournament + a take-home tail
**Repo:** your copy of the starter · **Deck:** Session 9 — *The Tail & the Tournament*
**Feeds:** Project Phase 6 — *Profile & Kill the Tail* (due **Fri Dec 4, 10:59 pm CT**) ·
HW 9 — *Cross-venue stale-quote detector* (due **Thu Dec 10, 10:59 pm CT**) ·
Project Phase 7 — *The Tournament* (due **Fri Dec 11, 10:59 pm CT**)

## Goal

Find a latency tail with a profiler, fix it without changing what the program
computes, and prove it with a before/after table. Then start the HW 9 detector
on a test table, and walk into the tournament with a build that works from a fresh
clone. You leave with one before/after table, one clean sanitizer run, a detector
that passes its first cases, and a pre-flight checklist done.

| Part | What | When |
|---|---|---|
| A0 | Build and run `tail.cpp` three times | in class, 3 min |
| A1 | Form a hypothesis from the code | 4 min |
| A2 | Profile it (Linux `perf` / macOS fallback) | 8 min |
| A3 | Fix it in a copy: `tail_fixed.cpp` | 10 min |
| A4 | Sanitizers on the fixed copy | 3 min |
| A5 | PGO + LTO on `kernel.cpp` (if time) | 2 min |
| B | HW 9: the stale-quote detector | take-home (start in class if you finish A) |
| C | Tournament pre-flight | before Part 3 of the session |

**Checkpoint at 1:18:** post in the Zoom chat your `tail` vs `tail_fixed` line (p50,
p99.9, max, and the `sink`, which must match) plus your machine.

## A0. Build and run the tail (3 min)

```bash
sed -n '1,14p' starters/hw14/tail.cpp          # the brief: p50/p99/p99.9/max in ns
g++ -O2 -std=c++17 starters/hw14/tail.cpp -o /tmp/tail
for i in 1 2 3; do /tmp/tail; done
```

The flags are `-O2` on purpose: tonight we profile the *algorithm*, not a missing
optimizer. Measured on an Apple M4 (Apple clang):

```
p50=417ns  p99=792ns  p99.9=1333ns  max=31667ns  (sink=200998581.510)
p50=417ns  p99=541ns  p99.9=584ns  max=15666ns  (sink=200998581.510)
p50=417ns  p99=500ns  p99.9=584ns  max=37917ns  (sink=200998581.510)
```

`p50` doesn't move; `p99.9` and `max` jump between runs. That variance is the tell.
On Linux with glibc's allocator the p99.9/p50 ratio is usually much larger than on
macOS — **report what your machine prints, and say which machine it is.**

## A1. A hypothesis from the code (4 min)

Open `signal()`:

```cpp
std::vector<double> scratch;                 // (a) a fresh vector EVERY tick
for (std::size_t k = lo; k <= i; ++k) scratch.push_back(prices[k]);
double s = 0.0;
for (double v : scratch) s += v;             // (b) O(window) re-sum every tick
return s / scratch.size();
```

- **(a)** `malloc`/`free` plus `push_back` growth on the hot path. Usually the
  allocator's fast path; occasionally a real refill. That is a tail.
- **(b)** ~257 adds per tick when a rolling sum needs one add and one subtract: a
  constant-factor drag on *every* tick, i.e. the median.

Note the window: `lo = i - window` and the loop is `k <= i` **inclusive**, so once warm
it averages **`window + 1` = 257 prices**, growing 1, 2, 3, ... at the start. Your fix
must reproduce that exactly.

## A2. Profile before you fix (8 min)

**Linux:**

```bash
perf stat -d /tmp/tail                              # IPC, cache and branch misses
perf record -g /tmp/tail && perf report --stdio | head -40
perf script | stackcollapse-perf.pl | flamegraph.pl > /tmp/tail.svg
```

Look for allocator frames (`operator new`, `malloc`, `_int_malloc`, `free`, sometimes
`mmap`) under a function that "just averages numbers." `flamegraph.pl` and
`stackcollapse-perf.pl` come from `github.com/brendangregg/FlameGraph`; put both on
your `PATH`. `flamegraph.pl` is the script; the `.svg` is what comes out.

**macOS (no `perf`):** Instruments → *Time Profiler* and *Allocations* on `/tmp/tail`
(`xcrun xctrace record --template 'Time Profiler' --launch -- /tmp/tail` from the
command line), or count calls from the code: 2,000,000 ticks, one vector per tick,
several reallocations per vector as it grows to 257. Either way, **attribute the time to
the allocator and the re-sum before you change a line.**

## A3. Fix it in a copy (10 min)

```bash
cp starters/hw14/tail.cpp starters/hw14/tail_fixed.cpp
```

In `tail_fixed.cpp`, replace `signal()` with a ring that is allocated **once** and a
running sum:

```cpp
struct RollingMean {
    std::vector<double> buf;          // sized ONCE, in the constructor
    std::size_t cap, count = 0, head = 0;
    double sum = 0.0;
    explicit RollingMean(std::size_t n) : buf(n, 0.0), cap(n) {}
    double push(double px) {
        if (count == cap) sum -= buf[head];   // evict the oldest
        else              ++count;
        sum += px;                             // admit the newest
        buf[head] = px;
        head = (head + 1 == cap) ? 0 : head + 1;
        return sum / static_cast<double>(count);
    }
};
```

In `main`, build it **before** the timed loop and call it inside:

```cpp
RollingMean roll(WINDOW + 1);                 // 257: matches the inclusive window
// ... inside the timed loop, replacing the signal() call:
sink += roll.push(prices[i]);
```

```bash
g++ -O2 -std=c++17 starters/hw14/tail_fixed.cpp -o /tmp/tail_fixed
for i in 1 2 3; do /tmp/tail_fixed; done
```

Measured on the same M4:

```
p50=0ns  p99=42ns  p99.9=42ns  max=9250ns  (sink=200998581.510)
```

- **`sink` is identical** to the last printed digit. If yours differs, you changed the
  math. The usual cause is `RollingMean roll(WINDOW)` (256 prices, not 257).
- **`0ns` and `42ns` are clock ticks, not measurements**: macOS `steady_clock` advances
  in ~42 ns steps here, so the whole tick now fits under one step. On Linux you'll see
  real single-digit to tens of ns.
- **`max` is still microseconds** even though every tick now does identical O(1) work.
  That's the OS (preemption, interrupts), not your code — the deck's jitter slide.
  Report it; don't "fix" it with flags.

The before/after table for Phase 6: p50, p99, p99.9 and max for both, three runs each,
the same `sink`, and one line naming the cause.

## A4. Sanitizers on the fixed copy (3 min)

A hand-written ring is exactly where an off-by-one hides:

```bash
g++ -O1 -g -fsanitize=address,undefined -std=c++17 starters/hw14/tail_fixed.cpp -o /tmp/tail_asan
/tmp/tail_asan; echo "exit=$?"
```

Clean means the normal output line and `exit=0` (slower, that's expected). Break it on
purpose — `head = head + 1;` without the wrap — and read the `heap-buffer-overflow`
report once. For your threaded pipeline, TSan is a **separate** build:
`-fsanitize=thread`. Never ship a sanitizer build.

## A5. PGO and LTO on the frozen kernel (2 min, if time)

`starters/hw13/kernel.cpp` is still **do-not-modify**. Two builds for GCC:

```bash
g++ -O3 -march=native -fprofile-generate starters/hw13/kernel.cpp -o /tmp/gen && /tmp/gen
g++ -O3 -march=native -fprofile-use      starters/hw13/kernel.cpp -o /tmp/pgo && /tmp/pgo
```

For clang / Apple clang the profile goes through `llvm-profdata`:

```bash
clang++ -O3 -march=native -fprofile-instr-generate starters/hw13/kernel.cpp -o /tmp/gen
LLVM_PROFILE_FILE=/tmp/k.profraw /tmp/gen
xcrun llvm-profdata merge -o /tmp/k.profdata /tmp/k.profraw     # Linux: llvm-profdata
clang++ -O3 -march=native -fprofile-instr-use=/tmp/k.profdata starters/hw13/kernel.cpp -o /tmp/pgo && /tmp/pgo
g++ -O3 -march=native -flto starters/hw13/kernel.cpp -o /tmp/lto && /tmp/lto
```

M4: `-O3` 149 ms → PGO 137 ms → LTO 139 ms, **same checksum** each time. One file, so
LTO has little to work with; on your bot (many `.cpp` files) it has more. For the bot,
one CMake build directory per flag set, `--fresh` (CMake ≥ 3.24), or the cache ignores
your `-DCMAKE_CXX_FLAGS`.

## B. HW 9 — the cross-venue stale-quote detector (take-home)

`starters/session09/stale_quote.cpp` is the harness: the contract in the header
comment, a `detect_stale()` stub, and a ten-row test table.

```bash
g++ -std=c++20 -O2 starters/session09/stale_quote.cpp -o /tmp/stale && /tmp/stale
# ... 4/10 cases pass    <- the stub never fires, so only the "no order" rows pass
```

The contract, in one breath: a cross exists when one venue's bid is above the other's
ask; the **older** of the two crossing quotes is the stale one; buy the stale ask or sell
the stale bid **on the stale venue**; fire only if the cross survives **two taker
fees** (`price × fee_bps × 1e-4` per leg); size is the stale quote's displayed size,
**clipped so |position| never exceeds the limit**. No allocation — this runs inside
`on_book`.

Two rows are the deck's arithmetic: the 1-cent cross at 30 bps **must not fire**
(net −0.59 a share); the $0.96 cross at 30 bps must. When all ten pass, write
`README.md` with the part that carries the marks:

1. **What latency would you need for this to be real?** Compare how long a stale quote
   survives with your measured tick-to-trade (Part A, Phase 6).
2. **What makes the signal false?** Adverse selection: the "stale" venue may be right and
   the fresh one about to reverse; the stale quote may be a trap sized to be hit; your
   timestamps may be the stale ones.
3. **Stretch:** a resting order on the stale venue when the other venue moves — hold or
   reprice? Use the queue-aware rule from the deck.

HW 9 is hand-graded: source plus README. Add your own cases to the table.

## C. Tournament pre-flight (before Part 3)

From a **fresh clone** of your repo, not your working copy:

```bash
make client                    # hft/cpp_client -> hft/cpp_client/build/hft_bot (TLS on)
make run                       # connects with TEAM_ID / ARENA_TOKEN from .env
```

Then confirm, in order:

- [ ] Your team appears on the dashboard's LATENCY tab once the round opens.
- [ ] Your bot counts its own messages per tick and stays under **6** (the finale
      scenario's `order_quota`); rejects in your log mean you're over.
- [ ] Any arb threshold includes the **30 bps** taker fee on *both* legs.
- [ ] Nothing in `on_book` allocates, logs or blocks (Part A, applied to the bot).
- [ ] Your cross-venue plan, if any, is **two processes** with one `EXCHANGE_URL` each
      and a shared touch cache (your Phase 4 ring). `EXCHANGE_URLS` is Python-only.
- [ ] Offline fallback works: if you can't connect, the replay gives a legitimate
      Phase 7 number:
      ```bash
      python3 scripts/latency_replay.py --latest --cmd "hft/cpp_client/build/hft_bot --replay"
      ```

The finale scenario is `hft/scenarios/week10.json` in the arena repo: `order_quota` 6,
`position_limit` 1200, `SHORT_LOCATE_CAP` 600, opening auction 15 ticks, closing 12,
`LULD_BAND_PCT` 0.08, AAPL earnings at tick 250, the FPGA ("Gateway Silicon") IPO
listing at 550, a market-wide econ print at 850, NVDA earnings at 1150. The shop is
closed (`purchase_window: false`): nobody buys colocation.

## Checkpoint

- [ ] Before/after table: `tail` vs `tail_fixed`, three runs each, **same `sink`**
- [ ] A profile (perf, a flame graph, or Instruments) that points at the allocator
- [ ] `tail_fixed` clean under `-fsanitize=address,undefined`
- [ ] You can explain why `max` stayed in microseconds after the fix
- [ ] `/tmp/stale` passes 10/10 (by Dec 10) and the README answers both questions
- [ ] Fresh-clone build connects, or the replay runs

## Files

- Profile and fix: `starters/hw14/tail.cpp` → your `starters/hw14/tail_fixed.cpp`
- Frozen kernel (flags only): `starters/hw13/kernel.cpp`
- HW 9 harness: `starters/session09/stale_quote.cpp`
- Client, `make client` / `make run`, replay: `docs/HFT_CPP_CLIENT.md`, `scripts/latency_replay.py`
- Phase 6 and 7 checklist: `project/README.md`

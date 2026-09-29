# Session 8 Lab — Parse, Serialize, Vectorize

**FINM 32700 · Session 8 · Mon Nov 16** · in class, in pairs (~45 min) + a take-home tail
**Repo:** your copy of the starter · **Deck:** Session 8 — *The Wire & the Machine*
**Feeds:** HW 8 — *Fast FIX parser + uint64→text* (due **Tue Dec 1, 10:59 pm CT** — ten
days out is Thanksgiving) · Project Phase 5 — *Wire & Hardware Tuning* (due **Tue Dec 1,
10:59 pm CT**)

## Goal

Build the two hot-path codecs HW 8 grades — a **single-pass, zero-allocation FIX
parser** and a **`uint64_t` → decimal text** routine that beats `std::to_string` —
then run your parser behind a **non-blocking socket** that frames a FIX byte stream
by BodyLength and rejects a bad checksum. Last, look at the machine: the same
`kernel.cpp` at `-O0` and `-O3 -march=native`, and the compiler's own report of
whether it vectorized. You leave with two green tests, two ns/op numbers, one
framed stream, and one vectorization report.

| Step | What | Minutes |
|---|---|---|
| 0 | Setup: both targets RED | 2 |
| 1 | The FIX parser (HW 8, part 1) | 15 |
| 2 | `u64toa` (HW 8, part 2) | 10 |
| 3 | Frame a FIX stream over a non-blocking socket | 10 |
| 4 | The machine: `-O0` vs `-O3 -march=native`, the vectorization report | 8 |
| 5 | *Take-home:* two-digit `u64toa`, AVX2 by hand, your bot on the replay | — |

**Checkpoint at 2:40:** post in the Zoom chat (a) your `fix_parse_ns_per_op`, (b) your
`u64toa_ns_per_op` next to `std_to_string_ns_per_op`, (c) the last line of `/tmp/frame`,
and (d) your `-O0`/`-O3` `time_ms` pair — plus your machine (e.g. "M4 Pro, Apple clang 17").

## 0. Setup (2 min)

From the repo root:

```bash
make fix       # builds tests/fix_test.cpp against include/fix_parser.hpp
make u64toa    # builds tests/u64toa_test.cpp against include/u64toa.hpp
```

Both print `RESULT|...|fail` right now — the headers are stubs. Read the two contracts
(`tests/fix_test.cpp`, `tests/u64toa_test.cpp`) before you write a line: the grader
checks exactly those fields, edge cases and fuzz values.

## 1. The FIX parser — HW 8, part 1 (15 min)

The test message (SOH shown as `|`):

```
8=FIX.4.2 | 9=45 | 35=D | 11=ORD123 | 55=AAPL | 54=1 | 38=100 | 44=185.50 | 10=179 |
```

`9=45` is the real BodyLength and `10=179` the real CheckSum of those bytes — step 3
verifies both. The struct in `include/fix_parser.hpp` tells you the design:
`clordid` is a **pointer + length into the caller's buffer** (a view, nothing copied),
`symbol` a small fixed `char[16]`.

**The mental model: one forward scan.** Read an integer tag, expect `=`, take the value
up to the next SOH, `switch` on the tag, repeat. No `std::string`, no `std::map`, no
allocation, no going back. Fill in `parse_new_order`:

```cpp
#pragma once
#include <cstdint>
#include <cstring>
#include <cstdlib>
// (keep the NewOrder struct exactly as shipped)

inline bool parse_new_order(const char* buf, int len, NewOrder& out) {
    const char SOH = '\x01';
    const char* p   = buf;
    const char* end = buf + len;
    bool id = false, sym = false, side = false, qty = false, px = false;

    while (p < end) {
        int tag = 0;                                   // tag: digits up to '='
        while (p < end && *p != '=') {
            if (*p < '0' || *p > '9') return false;    // malformed tag
            tag = tag * 10 + (*p - '0');
            ++p;
        }
        if (p >= end) return false;                    // no '=' -> malformed
        ++p;                                           // consume '='

        const char* v = p;                             // value: bytes up to SOH
        while (p < end && *p != SOH) ++p;
        const int vlen = static_cast<int>(p - v);
        if (p < end) ++p;                              // consume SOH

        switch (tag) {
            case 11: out.clordid = v; out.clordid_len = vlen; id = true; break;
            case 55: {
                int n = vlen < 15 ? vlen : 15;
                std::memcpy(out.symbol, v, n);
                out.symbol[n] = '\0';
                sym = true;
                break;
            }
            case 54: if (vlen >= 1) { out.side = v[0]; side = true; } break;
            case 38: {
                uint32_t q = 0;
                for (int i = 0; i < vlen; ++i) {
                    if (v[i] < '0' || v[i] > '9') return false;
                    q = q * 10 + static_cast<uint32_t>(v[i] - '0');
                }
                out.qty = q; qty = true;
                break;
            }
            case 44: out.price = std::strtod(v, nullptr); px = true; break;  // stops at SOH
            default: break;                            // 8, 9, 35, 10, ...: skip
        }
    }
    return id && sym && side && qty && px;             // every required field seen
}
```

```bash
make fix
# RESULT|fix_parse|pass|symbol/side/qty/price/clordid parsed
# METRIC|fix_parse_ns_per_op|<n>
```

Measured on an Apple M4 (Apple clang, `-O2`): **~45 ns/op**. If you see hundreds,
something allocates or copies. Two things to be able to say out loud: why `clordid`
is a view (copying it is an allocation-shaped cost for bytes you may never read), and
why `strtod` is safe here (it stops at the first non-numeric byte — the SOH — and does
not allocate).

## 2. `u64toa` — HW 8, part 2 (10 min)

`int u64toa(uint64_t v, char* out)` writes the digits and returns the count. Start
with the classic reverse-then-flip:

```cpp
#pragma once
#include <cstdint>

inline int u64toa(std::uint64_t v, char* out) {
    char tmp[20];                      // UINT64_MAX = 18446744073709551615: 20 digits
    int n = 0;
    do {
        tmp[n++] = static_cast<char>('0' + v % 10);
        v /= 10;
    } while (v);                       // do/while: v == 0 still prints "0"
    for (int i = 0; i < n; ++i)        // flip into the caller's buffer, MSD first
        out[i] = tmp[n - 1 - i];
    return n;
}
```

```bash
make u64toa
# RESULT|u64toa_edges|pass|...   RESULT|u64toa_fuzz|pass|...
# METRIC|u64toa_ns_per_op|<n>    METRIC|std_to_string_ns_per_op|<m>
```

**Read the two numbers honestly.** On an Apple M4 with libc++ this simple version
measured **9–12 ns/op against ~7.5 ns for `std::to_string`** — it *loses*. Modern
`std::to_string` uses the small-string buffer (no heap for 15 digits) and a fast
`to_chars`. Correct is green; *beating the library* is the HW 8 point, and it needs
step 5's two-digit table (measured 5–6 ns/op on the same machine). On Linux/libstdc++
your ratio will differ — report yours.

> The benchmark hides `x` from the optimizer (`opaque(x)`) on purpose. If a
> hand-rolled benchmark of yours ever prints ~0.25 ns/op, the compiler computed the
> answer at compile time and you timed an empty loop.

## 3. Frame a FIX stream over a non-blocking socket (10 min)

A TCP socket delivers **bytes, not messages**. `starters/session08/frame_demo.cpp`
pushes four NewOrderSingles through an `AF_UNIX` stream socketpair in awkward chunks —
1 byte, 7 bytes, half a message, two at once. The reader is the deck's shape: the fd
is `O_NONBLOCK`, `poll()` says "readable", `on_readable()` drains until `EAGAIN` into
**one reusable buffer**, cuts frames from **BodyLength** (`8=FIX.4.2|9=NN|` + NN bytes +
`10=XXX|`), verifies the **CheckSum**, and hands complete frames to **your** parser.

```bash
g++ -std=c++20 -O2 -Iinclude starters/session08/frame_demo.cpp -o /tmp/frame && /tmp/frame
```

Expected tail (identical on macOS and Linux):

```
  frame 1: 65 bytes, ORD1 BUY  100 @ 185.50
  frame 2: 64 bytes, ORD2 SELL 25 @ 182.25
wrote  115 bytes (256/256)
  frame 3: 64 bytes, CHECKSUM MISMATCH -> dropped
  frame 4: 63 bytes, ORD4 SELL 5 @ 251.10
frames=4 parsed=3 bad_checksum=1 recv_calls=6 partial_waits=5 leftover=0
```

Frame 3 was tampered with (`38=10` → `38=90`) *after* its checksum was computed.
`partial_waits=5` is the number of times the reader had half a message and correctly
**waited** instead of parsing garbage. If you still see `parse FAILED`, step 1 isn't done.

Read `frame_at()` and `on_readable()` and answer in one line each: (a) why the reader
never calls `recv` once per message, (b) what `memmove` keeps, (c) why `kMaxBody` exists.

## 4. The machine: `-O0` vs `-O3 -march=native` (8 min)

`starters/hw13/kernel.cpp` is a float reduction with a data-dependent branch over a
64K array, 3,000 times. **Do not edit it** — the command line is your only lever.

```bash
g++ -O0 starters/hw13/kernel.cpp -o /tmp/slow && /tmp/slow
g++ -O3 -march=native starters/hw13/kernel.cpp -o /tmp/fast && /tmp/fast
```

Same `checksum=`, far smaller `time_ms`. Measured on an Apple M4: **934 ms → 148 ms
(6.3×)** — and `-O2` alone gives the same 149 ms. Now ask the compiler *why*:

```bash
# clang / Apple clang:
g++ -O3 -march=native -Rpass=loop-vectorize -Rpass-missed=loop-vectorize \
    -c starters/hw13/kernel.cpp -o /dev/null
# GCC:
g++ -O3 -march=native -fopt-info-vec-all -c starters/hw13/kernel.cpp -o /dev/null
```

On clang (arm64 *and* x86-64 AVX2) the hot loop reports **`loop not vectorized`**. The
6× is inlining, registers and scheduling — not SIMD. The reason is the accumulator:
`acc += ...` in order is a floating-point sum, and **FP addition is not associative**,
so splitting it across 8 lanes would change the answer. Prove it:

```bash
g++ -O3 -march=native -ffast-math starters/hw13/kernel.cpp -o /tmp/ffm && /tmp/ffm
```

M4: **87 ms, and the checksum moves** (151299320.088155 → 151299381.089299). The
compiler vectorized as soon as you gave it permission to reassociate. One line in your
notes: would you ship `-ffast-math` in a pricing kernel? (A faster build that prints a
different checksum changed the math; it does not count.)

## 5. Take-home

1. **Beat `std::to_string` (HW 8).** Halve the divisions with a two-digit table:
   ```cpp
   static constexpr char D2[201] =
       "00010203040506070809101112131415161718192021222324"
       "25262728293031323334353637383940414243444546474849"
       "50515253545556575859606162636465666768697071727374"
       "75767778798081828384858687888990919293949596979899";
   // while (v >= 100): r = v % 100, v /= 100, copy D2[2*r], D2[2*r+1] (fill from the right)
   // then the last one or two digits; copy the result into out; return the length.
   ```
   Re-run `make u64toa` three times and report the median of each metric.
2. **AVX2 by hand, once.** The deck's `weighted_sum` intrinsic loop, in
   `scratch/simd_demo.cpp` with a `main` that sums 128 ones against weights `0..127`
   (expect `8128.0`). x86-64: `g++ -std=c++20 -O2 -mavx2 -mfma scratch/simd_demo.cpp -o /tmp/simd`.
   Apple Silicon has no AVX: `clang++ -std=c++20 -O2 -arch x86_64 -mavx2 -mfma ...`
   builds an x86 binary that runs under Rosetta — correct output, meaningless timings.
3. **Your bot (Project Phase 5).** Replace `on_book`'s path to best bid/ask with the
   deck's targeted extract (`"bids":[[`, `"asks":[[`, `"mid_price":` — levels are
   `[price, qty]`, index 0 is the touch), stamp outbound quantities with your `u64toa`,
   and measure before/after on **the same tape**:
   ```bash
   make client        # or: cmake -S hft/cpp_client -B hft/cpp_client/build && cmake --build hft/cpp_client/build
   python3 scripts/latency_replay.py --latest --cmd "hft/cpp_client/build/hft_bot --replay"
   ```
   (`--latest` takes the newest `sessions/session_*.jsonl`; or pass a tape path
   positionally.) The reference client measured p50 33.4 µs / p99.9 72.9 µs on an M4
   over 4,057 snapshots — your baseline is whatever *your* machine prints first.
4. **Linux only:** pin the bot's hot thread (`pthread_setaffinity_np` or `taskset -c 3`)
   and re-run the replay three times. Report the p99.9 spread with and without. macOS has
   no CPU-affinity API: skip the pin, keep the measurement, say so in the Phase 5 write-up.

## Checkpoint

- [ ] `make fix` → `RESULT|fix_parse|pass`, and you know your ns/op
- [ ] `make u64toa` → edges and fuzz `pass`; `u64toa_ns_per_op` below `std_to_string_ns_per_op`
      (after the take-home table if needed)
- [ ] `clordid` is a view into the caller's buffer; malformed input returns `false`
- [ ] `/tmp/frame` ends `frames=4 parsed=3 bad_checksum=1 ... leftover=0`
- [ ] You can explain why `kernel.cpp` did **not** vectorize at `-O3` and what
      `-ffast-math` changed
- [ ] `make test` scores the rows *FIX: parse NewOrder*, *u64toa: edge cases*,
      *u64toa: fuzz vs std*

## Files

- Edit: `include/fix_parser.hpp`, `include/u64toa.hpp` (HW 8)
- Contracts (read-only): `tests/fix_test.cpp`, `tests/u64toa_test.cpp`, `tests/bench.hpp`
- Demo: `starters/session08/frame_demo.cpp` · frozen kernel: `starters/hw13/kernel.cpp`
- Grader: `make test` (`tests/run_ci.py`) · client + replay: `docs/HFT_CPP_CLIENT.md`
- The arena wire (JSON over WebSocket, not FIX): `shared/messages.py` in the arena repo

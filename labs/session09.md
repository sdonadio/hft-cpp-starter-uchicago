# Session 9 Lab — A Pre-Trade Risk Gate

**FINM 32700 · Session 9 · Mon Nov 30** · in class, in breakout pairs (~45 min) + a take-home tail
**Repo:** your copy of the starter · **Deck:** Session 9 — *Pre-Trade Risk & Controls*
**Feeds:** HW 9 — *A pre-trade risk gate* (10 pts, team work, due **Thu Dec 10, 10:59 pm CT**)

## Goal

Build `include/risk_gate.hpp`: the last piece of code every order passes before it
is serialized and sent. It answers one question per order, *may this go out?*, and
returns a reason code when the answer is no. It must:

1. refuse fat-finger orders (size, notional) and prices outside a **collar** around
   a reference price;
2. track **position and open orders** per symbol, and refuse anything whose
   *worst case* would break the position limit;
3. refuse orders that would **cross your own** resting orders, and **duplicates**;
4. meter order **and** cancel messages with a **token bucket**;
5. hold a **kill switch** (`std::atomic<bool>`) that any thread can pull, that
   blocks new orders but never cancels, and that trips itself on the conditions
   below;
6. do all of it in **O(1)**, with **no allocation, no locks and no clock reads**,
   in a few nanoseconds, measured.

You leave with seven green CI rows, a measured ns/check, and a plan for wiring the
gate into your bot.

| Step | What | Minutes |
|---|---|---|
| 0 | `make risk`: seven red rows; read the contract | 2 |
| 1 | Size, notional, sanity, collar | 8 |
| 2 | Position with open orders: `check`, `on_fill`, `on_done` | 10 |
| 3 | Self-cross and duplicate | 7 |
| 4 | Token bucket, `check_cancel` | 8 |
| 5 | Kill switch: atomic flag, auto-trips | 5 |
| 6 | `make risk-bench`: your ns/check vs a naive gate | 5 |
| — | Your turn: wire it in, fuzz it, measure it in the bot | take-home (HW 9) |

**Checkpoint at 2:33:** post in the Zoom chat your seven CI rows (how many green) and
the `yours` line from `make risk-bench`, plus your machine.

## Setup

```bash
cd hft-cpp-starter-uchicago
git pull                       # include/risk_gate.hpp, tests/risk_gate_test.cpp,
                               # starters/session09/risk_bench.cpp, make risk / risk-bench
make risk
```
The flags are the CI's: `g++ -std=c++17 -O2 -pthread -Iinclude -Itests`. Everything
below also compiles with `-std=c++20`. The stub compiles and fails every row:

```
RESULT|risk_size|fail|501 shares: got Ok, want MaxQty
RESULT|risk_collar|fail|no reference yet: got Ok, want NoReference
RESULT|risk_position|fail|open buy after accept: got 0, want 600
RESULT|risk_selfcross|fail|buy at our own ask: got Ok, want SelfCross
RESULT|risk_throttle|fail|a burst of 10 at one instant: got 10, want 6
RESULT|risk_dup|fail|same order 1 ms later: got Ok, want Duplicate
RESULT|risk_kill|fail|kill() sets killed()
METRIC|risk_ns_per_check|0.25
```

## Step 0 — Read the contract (2 min)

Open `include/risk_gate.hpp`. The types are given and fixed by the autograder:

```cpp
enum class Side : uint8_t { Buy = 0, Sell = 1 };
enum class Risk : uint8_t { Ok = 0, Killed, BadOrder, MaxQty, MaxNotional,
                            NoReference, PriceCollar, Position, SelfCross,
                            Duplicate, Throttled };
struct Order { uint32_t sym; Side side; int32_t qty; int64_t px; uint64_t ts_ns; };
```

Three decisions are already made for you. Know why before you code:

- **Prices are integer ticks** (1 tick = $0.01). Limits compare by multiplying,
  never by dividing, so there's no rounding and no floating point in a limit.
- **Time comes in with the order** (`ts_ns`). The gate never calls a clock: the caller
  stamps the tick once, and tests can drive time exactly.
- **`check()` is check-and-reserve.** `Ok` means *you will send this now*, so the
  gate counts the order as open right away. A refused order changes nothing.

**The order of the enum is the order of the checks.** The first failure wins. The
kill switch is first because it is the one that must never be skipped.

## Step 1 — Size, notional, sanity, collar (8 min)

Keep the limits in a member (`RiskLimits lim_;`) and give each symbol one cache line:

```cpp
struct alignas(64) Sym {
    int64_t  ref = 0, max_bid = 0, min_ask = 0, last_px = 0;
    uint64_t last_ts = 0;
    int32_t  pos = 0, open_buy = 0, open_sell = 0, last_qty = 0;
    Side     last_side = Side::Buy;
};
Sym s_[kMaxSymbols]{};
```

`set_reference(sym, px)` stores `px` in `s_[sym].ref` (ignore `sym >= kMaxSymbols`).
Then the top of `check()`:

```cpp
if (o.qty <= 0 || o.px <= 0 || o.sym >= kMaxSymbols)  return Risk::BadOrder;
if (o.qty > lim_.max_qty)                             return Risk::MaxQty;
if (int64_t(o.qty) * o.px > lim_.max_notional)        return Risk::MaxNotional;
Sym& s = s_[o.sym];
if (s.ref <= 0)                                       return Risk::NoReference;
if (std::llabs(o.px - s.ref) * 10000 > int64_t(lim_.collar_bps) * s.ref)
                                                      return Risk::PriceCollar;
```

Two traps:
- `o.qty * o.px` in `int32_t` overflows for 500 shares at $50,000 = 5,000,000 ticks.
  Widen **before** multiplying.
- **The edge is allowed.** With a reference of 10,000 and 800 bps, 10,800 passes and
  10,801 doesn't. Use `>`, not `>=`.

Run `make risk`: `risk_size` and `risk_collar` go green. (Once you add Step 3's
self-cross check, `risk_collar` also relies on Step 2's `on_done`: it rests a buy,
releases it, then sends a sell below it.)

## Step 2 — Position with open orders (10 min)

The venue counts every resting order as if it will fill (`exchange/server.py`,
the `POSITION_LIMIT` check: team position + same-side open + this order). Your gate
must count the same way, or it lets through exactly the orders the venue refuses.

```cpp
const bool buy = o.side == Side::Buy;
const int64_t worst = buy ? int64_t(s.pos) + s.open_buy  + o.qty
                          : int64_t(s.pos) - s.open_sell - o.qty;
if (std::llabs(worst) > lim_.max_position)            return Risk::Position;
```

When every check has passed, **reserve**: add `o.qty` to `open_buy` or `open_sell`.

**Write `on_fill` and `on_done` yourself.** Both release open quantity on their side,
never below zero (a cancel can race a fill, so you may be told twice). `on_fill`
also moves the position. The test walks through:
buy 600 rests → buy 500 refused (600 + 500 > 1000) → buy 400 accepted (exactly
1000) → 600 fill → buy 1 refused (600 + 400 + 1) → the 400 is cancelled → buy 400
accepted.

One more rule, for Step 5: if a fill takes `|pos|` past `max_position`, something sent
an order your gate never approved. **Fail closed:** call `kill()`.

## Step 3 — Self-cross and duplicate (7 min)

**Self-trade prevention.** The arena cancels *your resting order* when you trade with
yourself (team-scoped STP in `shared/orderbook.py`). You lose the queue spot, and
the venue still counts the message. Refuse it in the gate instead.

You don't track individual orders, so keep a **conservative bound** per symbol: the
highest price among your resting bids (`max_bid`) and the lowest among your resting
asks (`min_ask`), valid only while that side has open quantity. On an accepted
buy, `max_bid = (open_buy == 0) ? px : max(max_bid, px)` *before* you add the qty;
mirror it for sells. Then:

- a **buy** with `open_sell > 0 && px >= min_ask` → `SelfCross`
- a **sell** with `open_buy > 0 && px <= max_bid` → `SelfCross`

When a side's open quantity returns to zero, its bound stops mattering. The bound
may refuse a legal order (your best ask was cancelled but an older one is still
there). It never lets a self-cross through. Say that trade-off in your README.

**Duplicate.** Remember the last *accepted* order per symbol (side, qty, px, ts). A
new order with the same side, qty and px less than `dup_window_ns` later is a
`Duplicate`. `dup_window_ns == 0` turns the check off. Different qty, other side,
other symbol, or 50 ms later: all fine.

## Step 4 — The token bucket (8 min)

The arena meters order **and** cancel messages: `order_quota` per 1-second tick with
a 3× burst (`exchange/limits.py`), and over quota the venue answers `RATE_LIMITED`.
Your bucket runs in front of it, in nanoseconds:

- `tokens_` starts at `burst` (in the constructor);
- on each call, refill `(ts - last) * 1e-9 * msgs_per_sec`, cap at `burst`, and
  remember `ts` (only when it moved forward);
- a message needs one whole token.

**Write `take_token(uint64_t ts)` yourself** (the deck's slide 12 shows the shape).
Put it **last** in `check()`: an order refused for any other reason must not spend a
token. `check_cancel(ts)` is just `take_token(ts) ? Ok : Throttled`.

The test fires 10 orders at one instant (6 pass), checks a cancel is throttled,
waits 100 ms (0.6 token: refused) and 200 ms (1.2: accepted), then sends a
collar-violating order 5 s later and checks that it **did not** use a token.

In your bot, set `msgs_per_sec` **below** the venue's quota (say 80%), and leave
room for the cancels the kill switch will need.

## Step 5 — The kill switch (5 min)

```cpp
std::atomic<bool> killed_{false};
void kill()          { killed_.store(true, std::memory_order_relaxed); }
bool killed() const  { return killed_.load(std::memory_order_relaxed); }
```

Why `relaxed` is enough: the flag publishes no other data (Session 6). The hot
thread only needs to *eventually* see `true`, and on x86 and ARM the load is a plain
load of a line nobody else is writing.

- First line of `check()`: `if (killed_.load(std::memory_order_relaxed)) return Risk::Killed;`
- `check_cancel()` **ignores** the flag. Cancel-all is what the switch is for.
- `on_venue_reject()` counts; the `max_venue_rejects`-th one since the last `reset()`
  calls `kill()`. `0` disables it.
- `reset()` re-arms: clears the flag **and** the reject count. In a real system only a
  person calls it.

The test also pulls the switch from a second `std::thread` and checks that the next
`check()` returns `Killed`. (The test joins that thread first, so it can't catch a
plain `bool`. Your bot can: a console or watchdog thread writing the flag while
`on_book` reads it is a data race, which is undefined behaviour.)

```bash
make risk          # seven pass rows
g++ -std=c++17 -O1 -g -pthread -fsanitize=thread -Iinclude -Itests \
    tests/risk_gate_test.cpp -o /tmp/risk_tsan && /tmp/risk_tsan | grep -c pass   # 7, no warnings
```

## Step 6 — What does it cost? (5 min)

```bash
make risk-bench
```
`starters/session09/risk_bench.cpp` times three gates on one fixed stream of orders
(64 symbols, about 1 in 16 outside the collar): **none** (the loop alone), **naive**
(`std::string` symbol, `std::unordered_map`, a `std::mutex`, a `double` collar), and
**yours**. A laptop clock can't time one 5 ns call, so each sample is a batch of 64
calls divided by 64. The reference gate on an Apple M4 (Apple clang 21, `-O2`), three
runs:

```
none   p50   0.64  p99   0.66  p99.9    0.66  max     28.64  ns/check
naive  p50  25.39  p99  27.34  p99.9   32.56  max    242.84  ns/check
yours  p50   5.20  p99   5.86  p99.9    6.52  max     41.02  ns/check
```
Across runs, `yours` was 4.6–5.2 ns at p50 and 6.5 ns at p99.9; `naive` was 25–26 ns
and 33–48 ns. The stub prints `0.00` for `yours` because it does nothing: a number
that small means "you measured nothing", not "fast".

Read it: the naive gate is about 5× slower and still only ~30 ns, because the mutex
is **uncontended**. Put a monitor thread on that mutex and its tail becomes the
scheduler's (Session 6, Part A5). And because each sample averages 64 calls, one
slow call is diluted 64×. Say so when you report a p99.9.

## Your turn (take-home, HW 9)

1. **Wire it in.** In `hft/cpp_client/src/main.cpp`, give your bot a `RiskGate` and
   send every order through `check()` and every cancel through `check_cancel()`. Let
   CMake find the header:
   ```cmake
   target_include_directories(hft_bot PRIVATE
     ${CMAKE_CURRENT_SOURCE_DIR}/include
     ${CMAKE_CURRENT_SOURCE_DIR}/../../include)      # include/risk_gate.hpp
   ```
   Convert once per tick: dollars → ticks (`std::llround(px * 100)`), symbol →
   id (your Session 5 `SymMap`), `steady_clock` → `ts_ns`. Feed the state: `on_fill`
   → `gate.on_fill`; `on_error` → `gate.on_venue_reject()`; `SESSION_CLOSED` →
   `kill()`, then cancel what rests. The deck's slide 25 has the skeleton.
2. **Release on reject.** `on_error` carries no order id. Keep a small FIFO of
   *sent, not yet acked* orders per symbol; `on_ack` pops one, `on_error` pops one and
   calls `on_done`. Otherwise every venue reject leaks open quantity, and the gate
   slowly locks you out of the symbol. Write down what you chose.
3. **Fuzz it.** A property test (the deck's slide 23): a million random orders,
   random fills, and after each step `|position| <= max_position`. Then pull the
   switch at a random step and assert nothing is accepted after it.
4. **Measure it in the bot.** Replay the same tape with the gate on and off:
   ```bash
   python3 scripts/latency_replay.py --latest --cmd "hft/cpp_client/build/hft_bot --replay"
   ```
   Report p50/p99.9 tick-to-trade for both, on the same machine.
5. **Stretch (no extra points).** A gross-notional limit across symbols (keep the
   sum incremental: update it on accept, fill and done, marked at the reference);
   a runaway ratio (orders vs fills per second); a loss limit that pulls the switch.

## Checkpoint

- [ ] `make risk`: seven `pass` rows, and `make test` shows 85/85 on the seven risk lines
- [ ] TSan build of `tests/risk_gate_test.cpp`: seven passes, no `ThreadSanitizer` line
- [ ] `make risk-bench`: your `yours` line posted, with the machine
- [ ] One sentence: why does the token bucket go **last** in `check()`?
      (A refused order must not spend a message.)
- [ ] One sentence: why may `check_cancel()` never look at the kill flag?
      (Cancel-all is the kill switch's job.)

## Links

- Edit: `include/risk_gate.hpp`
- Contract (read-only): `tests/risk_gate_test.cpp`; how CI grades it: `tests/run_ci.py`
- Benchmark: `starters/session09/risk_bench.cpp` (`make risk-bench`)
- The venue's checks, for comparison: `exchange/server.py` (order validation),
  `exchange/limits.py` (token bucket), `exchange/circuit_breaker.py` (halts), and the
  finale's limits in `hft/scenarios/week10.json` (AlgoArena repo)
- Client: `docs/HFT_CPP_CLIENT.md`, `hft/cpp_client/include/hft_bot.hpp`
- Build: `make risk`, `make risk-bench`, full grade `make test`

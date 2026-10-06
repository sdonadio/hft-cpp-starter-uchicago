#pragma once
#include <atomic>
#include <cstdint>
#include <cstdlib>
// HW 9 (Session 9) — a client-side pre-trade risk gate. Walk-through: labs/session09.md.
// Every order your bot sends goes through check() first; Ok means "send it now".
// One hot thread owns the gate. The ONLY thing other threads may touch is the
// kill switch (kill() / killed()), so that member is a std::atomic<bool>.
//
// Rules for the hot path: O(1) per call, no allocation, no locks, no clock
// reads (time arrives in Order::ts_ns), per-symbol state in a flat array.
// Prices are integer ticks (1 tick = $0.01): no floating point in the limits.

enum class Side : uint8_t { Buy = 0, Sell = 1 };

// Why an order was refused. check() runs the tests in THIS order and returns
// the first failure, so the order of the enum is part of the contract.
enum class Risk : uint8_t {
    Ok = 0,
    Killed,       // kill switch is on: no new orders (cancels still go through)
    BadOrder,     // qty <= 0, px <= 0, or sym >= kMaxSymbols
    MaxQty,       // qty > max_qty                                  (fat finger, shares)
    MaxNotional,  // int64(qty) * px > max_notional                 (fat finger, money)
    NoReference,  // no reference price set for this symbol yet (ref <= 0)
    PriceCollar,  // |px - ref| * 10000 > collar_bps * ref          (edge = allowed)
    Position,     // worst case: |pos + open_buy + qty| (buy) or
                  //             |pos - open_sell - qty| (sell) > max_position
    SelfCross,    // buy with px >= our lowest resting ask, or sell with
                  // px <= our highest resting bid, while that side has open qty
    Duplicate,    // same side, qty and px as the LAST ACCEPTED order on this
                  // symbol, and ts_ns - its ts_ns < dup_window_ns (0 = off)
    Throttled     // token bucket empty (refill msgs_per_sec, capacity burst)
};

struct Order {
    uint32_t sym;     // symbol id, < RiskGate::kMaxSymbols
    Side     side;
    int32_t  qty;     // shares
    int64_t  px;      // limit price in ticks
    uint64_t ts_ns;   // caller's clock, non-decreasing
};

struct RiskLimits {
    int32_t  max_qty           = 500;         // the venue's MAX_ORDER_SIZE
    int64_t  max_notional      = 5'000'000;   // ticks x shares = $50,000
    int32_t  collar_bps        = 800;         // 8 %: the finale's LULD band
    int32_t  max_position      = 1200;        // the finale's position_limit
    double   msgs_per_sec      = 6.0;         // the finale's order_quota (1 tick = 1 s)
    double   burst             = 6.0;         // bucket capacity; it starts FULL
    uint64_t dup_window_ns     = 50'000'000;  // 50 ms; 0 disables
    int32_t  max_venue_rejects = 5;           // venue rejects before the switch trips; 0 disables
};

class RiskGate {
public:
    static constexpr uint32_t kMaxSymbols = 64;

    explicit RiskGate(const RiskLimits& lim) { (void)lim;
        // TODO(student): keep the limits; the token bucket starts full (burst tokens).
    }

    // The latest reference price (e.g. the mid, in ticks) for a symbol.
    void set_reference(uint32_t sym, int64_t ref_px) { (void)sym; (void)ref_px; /* TODO(student) */ }

    // Check-and-reserve. If every check passes: add qty to this side's open
    // quantity, update the self-cross bound, remember the order for the
    // duplicate test, spend ONE token, return Ok. A refused order changes
    // nothing and spends no token.
    Risk check(const Order& o) { (void)o;
        return Risk::Ok;   // TODO(student): the ten checks above, in order
    }

    // A cancel message: never blocked by the kill switch (the switch NEEDS
    // cancels), but it still spends a token — the venue meters cancels too.
    Risk check_cancel(uint64_t ts_ns) { (void)ts_ns;
        return Risk::Ok;   // TODO(student)
    }

    // One of OUR orders filled qty shares: move the position, release open qty
    // (never below 0). A position beyond max_position means an order the gate
    // never approved: trip the kill switch (fail closed).
    void on_fill(uint32_t sym, Side side, int32_t qty) { (void)sym; (void)side; (void)qty; /* TODO(student) */ }

    // qty of one of our orders will never fill (cancelled, rejected, expired):
    // release it from the open quantity (never below 0).
    void on_done(uint32_t sym, Side side, int32_t qty) { (void)sym; (void)side; (void)qty; /* TODO(student) */ }

    // The venue refused something (RATE_LIMITED, PRICE_BAND, POSITION_LIMIT, ...).
    // The max_venue_rejects-th one since the last reset() trips the switch.
    void on_venue_reject() { /* TODO(student) */ }

    void kill()  { /* TODO(student): may be called from ANY thread */ }
    void reset() { /* TODO(student): re-arm: clear the flag and the venue-reject count */ }
    bool killed() const { return false; /* TODO(student) */ }

    int32_t position(uint32_t sym) const { (void)sym; return 0; /* TODO(student) */ }
    int32_t open_qty(uint32_t sym, Side side) const { (void)sym; (void)side; return 0; /* TODO(student) */ }

private:
    // TODO(student): a flat array of per-symbol state (one cache line each),
    // the token bucket, the venue-reject count, and std::atomic<bool> killed_.
};

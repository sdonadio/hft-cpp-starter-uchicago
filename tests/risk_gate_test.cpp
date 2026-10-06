// risk_gate_test.cpp — HW 9 (Session 9). Contract: include/risk_gate.hpp defines
//   enum class Side : uint8_t { Buy, Sell };
//   enum class Risk : uint8_t { Ok, Killed, BadOrder, MaxQty, MaxNotional,
//                               NoReference, PriceCollar, Position, SelfCross,
//                               Duplicate, Throttled };      // checked in THIS order
//   struct Order { uint32_t sym; Side side; int32_t qty; int64_t px; uint64_t ts_ns; };
//   struct RiskLimits { max_qty, max_notional, collar_bps, max_position,
//                       msgs_per_sec, burst, dup_window_ns, max_venue_rejects };
//   class RiskGate {
//     static constexpr uint32_t kMaxSymbols = 64;
//     explicit RiskGate(const RiskLimits&);
//     void set_reference(uint32_t sym, int64_t ref_px);
//     Risk check(const Order&);            // check-and-reserve: Ok => it will be sent
//     Risk check_cancel(uint64_t ts_ns);   // never blocked by the kill switch; throttled
//     void on_fill(uint32_t sym, Side, int32_t qty);
//     void on_done(uint32_t sym, Side, int32_t qty);   // cancelled / rejected / expired
//     void on_venue_reject();
//     void kill(); void reset(); bool killed() const;
//     int32_t position(uint32_t sym) const; int32_t open_qty(uint32_t sym, Side) const;
//   };
// Prices are integer ticks (1 tick = $0.01). Time comes from Order::ts_ns: the
// gate never reads a clock itself.
#include <atomic>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include "risk_gate.hpp"
#include "bench.hpp"

static void R(const char* k, bool ok, const std::string& m) {
    std::cout << "RESULT|" << k << "|" << (ok ? "pass" : "fail") << "|" << m << "\n";
}

static const char* name(Risk r) {
    switch (r) {
        case Risk::Ok: return "Ok"; case Risk::Killed: return "Killed";
        case Risk::BadOrder: return "BadOrder"; case Risk::MaxQty: return "MaxQty";
        case Risk::MaxNotional: return "MaxNotional"; case Risk::NoReference: return "NoReference";
        case Risk::PriceCollar: return "PriceCollar"; case Risk::Position: return "Position";
        case Risk::SelfCross: return "SelfCross"; case Risk::Duplicate: return "Duplicate";
        case Risk::Throttled: return "Throttled";
    }
    return "?";
}

// A test case accumulates the first mismatch it sees.
struct Case {
    bool ok = true; std::string why;
    void expect(Risk got, Risk want, const char* what) {
        if (ok && got != want) { ok = false; why = std::string(what) + ": got " + name(got) + ", want " + name(want); }
    }
    void expect_eq(long long got, long long want, const char* what) {
        if (ok && got != want) { ok = false; why = std::string(what) + ": got " + std::to_string(got) + ", want " + std::to_string(want); }
    }
    void expect_true(bool b, const char* what) { if (ok && !b) { ok = false; why = what; } }
};

// Everything off except what a case turns on.
static RiskLimits loose() {
    RiskLimits l;
    l.max_qty = 1'000'000; l.max_notional = 1'000'000'000'000LL; l.collar_bps = 10'000;
    l.max_position = 1'000'000; l.msgs_per_sec = 1e9; l.burst = 1e9;
    l.dup_window_ns = 0; l.max_venue_rejects = 0;
    return l;
}
static Order O(uint32_t sym, Side sd, int32_t q, int64_t px, uint64_t ts = 0) { return Order{sym, sd, q, px, ts}; }
static const Side B = Side::Buy, S = Side::Sell;

int main() {
    // ── size & notional (fat finger) ─────────────────────────────────────────
    {
        Case c; RiskLimits l = loose(); l.max_qty = 500; l.max_notional = 5'000'000;
        RiskGate g(l); g.set_reference(0, 10'000);                    // $100.00
        c.expect(g.check(O(0, B, 100, 10'000)), Risk::Ok,          "100 @ $100");
        c.expect(g.check(O(0, B, 500, 10'000)), Risk::Ok,          "500 @ $100 (= both limits)");
        c.expect(g.check(O(0, B, 501, 10'000)), Risk::MaxQty,      "501 shares");
        c.expect(g.check(O(0, B, 0, 10'000)),   Risk::BadOrder,    "qty 0");
        c.expect(g.check(O(0, S, -5, 10'000)),  Risk::BadOrder,    "qty -5");
        c.expect(g.check(O(0, B, 10, 0)),       Risk::BadOrder,    "px 0");
        c.expect(g.check(O(64, B, 10, 10'000)), Risk::BadOrder,    "sym 64 (out of range)");
        g.set_reference(1, 20'000);
        c.expect(g.check(O(1, B, 300, 20'000)), Risk::MaxNotional, "300 @ $200 = $60,000");
        c.expect(g.check(O(1, B, 250, 20'000)), Risk::Ok,          "250 @ $200 = $50,000");
        R("risk_size", c.ok, c.ok ? "size, notional and sanity checks" : c.why);
    }
    // ── price collar vs reference ────────────────────────────────────────────
    {
        Case c; RiskLimits l = loose(); l.collar_bps = 800;          // 8 %
        RiskGate g(l);
        c.expect(g.check(O(3, B, 10, 10'000)), Risk::NoReference, "no reference yet");
        g.set_reference(3, 10'000);
        c.expect(g.check(O(3, B, 10, 10'800)), Risk::Ok,          "+8.00 % (edge)");
        g.on_done(3, B, 10);                                          // so the sell can't self-cross
        c.expect(g.check(O(3, S, 10, 9'200)),  Risk::Ok,          "-8.00 % (edge)");
        c.expect(g.check(O(3, B, 10, 10'801)), Risk::PriceCollar, "+8.01 %");
        c.expect(g.check(O(3, S, 10, 9'199)),  Risk::PriceCollar, "-8.01 %");
        c.expect(g.check(O(3, B, 10, 100'000)), Risk::PriceCollar, "fat-finger 10x price");
        g.set_reference(3, 20'000);                                   // the reference moves
        c.expect(g.check(O(3, B, 10, 10'000)), Risk::PriceCollar, "old price after ref moved");
        R("risk_collar", c.ok, c.ok ? "collar and missing-reference checks" : c.why);
    }
    // ── position, counting resting orders ────────────────────────────────────
    {
        Case c; RiskLimits l = loose(); l.max_position = 1000;
        RiskGate g(l); g.set_reference(5, 5'000);
        c.expect(g.check(O(5, B, 600, 4'990)), Risk::Ok,       "buy 600 rests");
        c.expect_eq(g.open_qty(5, B), 600, "open buy after accept");
        c.expect(g.check(O(5, B, 500, 4'980)), Risk::Position, "600 open + 500 > 1000");
        c.expect(g.check(O(5, B, 400, 4'980)), Risk::Ok,       "600 open + 400 = 1000");
        g.on_fill(5, B, 600);
        c.expect_eq(g.position(5), 600, "position after buy fill");
        c.expect_eq(g.open_qty(5, B), 400, "open buy after fill");
        c.expect(g.check(O(5, B, 1, 4'970)), Risk::Position,   "600 pos + 400 open + 1");
        g.on_done(5, B, 400);                                         // cancelled
        c.expect_eq(g.open_qty(5, B), 0, "open buy after cancel");
        c.expect(g.check(O(5, B, 400, 4'970)), Risk::Ok,       "600 pos + 400 after cancel");
        g.on_done(5, B, 400);
        // the short side: 600 long, sell 1600 -> -1000 is fine, -1001 is not
        c.expect(g.check(O(5, S, 1601, 5'010)), Risk::Position, "sell 1601 -> -1001");
        c.expect(g.check(O(5, S, 1600, 5'010)), Risk::Ok,       "sell 1600 -> -1000");
        g.on_fill(5, S, 1600);
        c.expect_eq(g.position(5), -1000, "position after sell fill");
        c.expect(g.check(O(5, B, 1000, 4'990)), Risk::Ok,       "buying back is allowed");
        c.expect(g.check(O(6, B, 10, 1)), Risk::NoReference,    "symbols are independent");
        R("risk_position", c.ok, c.ok ? "worst-case position incl. resting orders" : c.why);
    }
    // ── self-trade prevention ────────────────────────────────────────────────
    {
        Case c; RiskGate g(loose()); g.set_reference(7, 10'000);
        c.expect(g.check(O(7, S, 10, 10'010)), Risk::Ok,        "rest an ask at 100.10");
        c.expect(g.check(O(7, B, 10, 10'010)), Risk::SelfCross, "buy at our own ask");
        c.expect(g.check(O(7, B, 10, 10'050)), Risk::SelfCross, "buy through our own ask");
        c.expect(g.check(O(7, B, 10, 10'000)), Risk::Ok,        "bid below our ask");
        c.expect(g.check(O(7, S, 10, 10'000)), Risk::SelfCross, "sell at our own bid");
        g.on_done(7, S, 10);                                          // ask cancelled
        c.expect(g.check(O(7, B, 10, 10'020)), Risk::Ok,        "ask gone: buy 100.20 is fine");
        R("risk_selfcross", c.ok, c.ok ? "never crosses its own resting orders" : c.why);
    }
    // ── throttle: token bucket on order AND cancel messages ──────────────────
    {
        Case c; RiskLimits l = loose(); l.msgs_per_sec = 6; l.burst = 6;
        RiskGate g(l); g.set_reference(9, 10'000);
        const uint64_t t0 = 1'000'000'000;
        int accepted = 0;
        for (int i = 0; i < 10; ++i) accepted += g.check(O(9, B, 1, 9'900 + i, t0)) == Risk::Ok;
        c.expect_eq(accepted, 6, "a burst of 10 at one instant");
        c.expect(g.check_cancel(t0), Risk::Throttled, "cancel with an empty bucket");
        c.expect(g.check(O(9, B, 1, 9'950, t0 + 100'000'000)), Risk::Throttled, "+100 ms: 0.6 token");
        c.expect(g.check(O(9, B, 1, 9'951, t0 + 200'000'000)), Risk::Ok,        "+200 ms: 1.2 tokens");
        c.expect(g.check(O(9, B, 1, 30'000, t0 + 5'000'000'000ULL)), Risk::PriceCollar,
                 "a refused order");
        accepted = 0;                                                  // +5 s: full again (capped at 6)
        for (int i = 0; i < 10; ++i) accepted += g.check(O(9, B, 1, 9'960 + i, t0 + 5'000'000'000ULL)) == Risk::Ok;
        c.expect_eq(accepted, 6, "refill caps at burst; refused orders spend no token");
        c.expect(g.check_cancel(t0 + 6'000'000'000ULL), Risk::Ok, "cancel after refill");
        R("risk_throttle", c.ok, c.ok ? "token bucket on orders and cancels" : c.why);
    }
    // ── duplicate / runaway-loop detection ───────────────────────────────────
    {
        Case c; RiskLimits l = loose(); l.dup_window_ns = 50'000'000;   // 50 ms
        RiskGate g(l); g.set_reference(2, 10'000);
        const uint64_t t0 = 7'000'000'000ULL;
        c.expect(g.check(O(2, B, 10, 9'990, t0)), Risk::Ok,                       "first");
        c.expect(g.check(O(2, B, 10, 9'990, t0 + 1'000'000)), Risk::Duplicate,   "same order 1 ms later");
        c.expect(g.check(O(2, B, 11, 9'990, t0 + 2'000'000)), Risk::Ok,          "different qty");
        c.expect(g.check(O(2, B, 11, 9'990, t0 + 60'000'000)), Risk::Ok,         "same order 58 ms later");
        c.expect(g.check(O(2, S, 11, 10'010, t0 + 61'000'000)), Risk::Ok,        "other side");
        c.expect(g.check(O(4, S, 11, 10'010, t0 + 61'000'000)), Risk::NoReference, "per symbol");
        R("risk_dup", c.ok, c.ok ? "duplicate orders inside the window are refused" : c.why);
    }
    // ── kill switch ──────────────────────────────────────────────────────────
    {
        Case c; RiskLimits l = loose(); l.max_venue_rejects = 3; l.max_position = 100;
        RiskGate g(l); g.set_reference(1, 10'000);
        c.expect_true(!g.killed(), "starts armed, not killed");
        g.kill();
        c.expect_true(g.killed(), "kill() sets killed()");
        c.expect(g.check(O(1, B, 1, 10'000)), Risk::Killed, "new order while killed");
        c.expect(g.check(O(1, B, 0, 10'000)), Risk::Killed, "Killed is checked first");
        c.expect(g.check_cancel(1), Risk::Ok,               "cancels still allowed while killed");
        g.reset();
        c.expect(g.check(O(1, B, 1, 10'000)), Risk::Ok,     "after reset");
        g.on_venue_reject(); g.on_venue_reject();
        c.expect_true(!g.killed(), "2 venue rejects < 3");
        g.on_venue_reject();
        c.expect_true(g.killed(), "3rd venue reject trips the switch");
        g.reset();
        c.expect_true(!g.killed(), "reset clears the reject count");
        g.on_venue_reject();
        c.expect_true(!g.killed(), "count restarted after reset");
        g.on_fill(1, B, 150);                 // a fill the gate never approved
        c.expect_true(g.killed(), "a fill past max_position trips the switch");
        g.reset();
        // another thread pulls the switch; the hot thread must see it
        std::atomic<bool> go{false};
        std::thread t([&] { while (!go.load()) {} g.kill(); });
        go.store(true); t.join();
        c.expect(g.check(O(1, S, 1, 10'000)), Risk::Killed, "kill() from another thread");
        R("risk_kill", c.ok, c.ok ? "kill switch: manual, auto-trip, cross-thread" : c.why);
    }
    // ── the metric: ns per accepted check (+ its release) ────────────────────
    {
        RiskLimits l = loose(); l.max_position = 1000; l.collar_bps = 800; l.max_qty = 500;
        RiskGate g(l);
        for (uint32_t s = 0; s < RiskGate::kMaxSymbols; ++s) g.set_reference(s, 10'000 + s);
        uint64_t t = 1; uint32_t sym = 0;
        double ns = ns_per_op([&] {
            Order o{sym, (t & 1) ? Side::Buy : Side::Sell, 100, 10'000 + int64_t(sym), t};
            Risk r = g.check(o); doNotOptimize(r);
            g.on_done(o.sym, o.side, o.qty);
            sym = (sym + 1) & (RiskGate::kMaxSymbols - 1); ++t;
        }, 5'000'000);
        std::cout << "METRIC|risk_ns_per_check|" << ns << "\n";
    }
    return 0;
}

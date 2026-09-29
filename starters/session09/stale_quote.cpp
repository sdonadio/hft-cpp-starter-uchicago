// stale_quote.cpp — HW 9 "Cross-venue stale-quote detector" (Session 9).
//
// Two venues quote the same symbol. When their tops of book CROSS (one venue's
// bid above the other's ask), one of the two quotes is stale: the one that was
// updated LONGER AGO has not caught up with the news the other already shows.
// Your detector finds that stale quote and emits the ONE order that captures
// it, on the stale venue, if and only if the edge survives fees and the fill
// keeps your position inside the limit.
//
// Contract (keep the signatures; the table below is the hand-grader's start):
//   * A cross exists if v[0].bid > v[1].ask or v[1].bid > v[0].ask.
//   * The stale venue is the one of the crossing pair with the OLDER ts_ns.
//     - stale venue's ASK is below the other bid  -> BUY  on the stale venue at its ask
//     - stale venue's BID is above the other ask  -> SELL on the stale venue at its bid
//   * Edge per share = |other venue's touch - stale price| - 2 * taker fee
//     (fee = price * fee_bps * 1e-4 per leg; you pay to enter AND to exit).
//     Fire only if edge > 0.
//   * qty = the stale quote's displayed size, clipped so that
//     |position + signed qty| <= limit. qty <= 0 -> no order.
//   * No allocation, no I/O: this runs inside on_book.
//
// Build:  g++ -std=c++20 -O2 starters/session09/stale_quote.cpp -o /tmp/stale && /tmp/stale
// Then:   write README.md — what latency this needs to be real, and what makes the
//         signal FALSE (adverse selection). That discussion carries the marks.

#include <cstdint>
#include <cstdio>

struct Top {
    double   bid = 0, ask = 0;      // 0 = that side is empty
    int      bid_sz = 0, ask_sz = 0;
    uint64_t ts_ns = 0;             // when this venue's touch last changed
};

struct Order {
    int    venue = -1;              // 0 or 1; -1 = no order
    int    side  = 0;               // +1 buy, -1 sell
    int    qty   = 0;
    double px    = 0;
};

// TODO(HW 9): implement the contract above.
Order detect_stale(const Top v[2], double fee_bps, int position, int limit) {
    (void)v; (void)fee_bps; (void)position; (void)limit;
    return {};                      // placeholder: never fires
}

// ── test table ───────────────────────────────────────────────────────────────
struct Case {
    const char* name;
    Top a, b;
    double fee_bps; int position, limit;
    Order want;
};

int main() {
    const Case cases[] = {
        {"no cross",                    {100.00, 100.02, 100, 100, 10}, {100.01, 100.03, 100, 100, 20}, 0, 0, 1000, {}},
        {"B ask stale, no fees",        {100.05, 100.07, 300, 300, 20}, {100.02, 100.04, 200, 200, 10}, 0, 0, 1000, {1, +1, 200, 100.04}},
        {"same cross, 30 bps kills it", {100.05, 100.07, 300, 300, 20}, {100.02, 100.04, 200, 200, 10}, 30, 0, 1000, {}},
        {"big cross survives 30 bps",   {101.00, 101.02, 300, 300, 20}, {100.02, 100.04, 200, 200, 10}, 30, 0, 1000, {1, +1, 200, 100.04}},
        {"A bid stale -> sell A",       {101.00, 101.02, 150, 300, 10}, {100.02, 100.04, 200, 200, 20}, 30, 0, 1000, {0, -1, 150, 101.00}},
        {"clipped by the limit",        {101.00, 101.02, 300, 300, 20}, {100.02, 100.04, 200, 200, 10}, 30, 950, 1000, {1, +1, 50, 100.04}},
        {"at the limit: no order",      {101.00, 101.02, 300, 300, 20}, {100.02, 100.04, 200, 200, 10}, 30, 1000, 1000, {}},
        {"short side has room",         {101.00, 101.02, 150, 300, 10}, {100.02, 100.04, 200, 200, 20}, 30, -900, 1000, {0, -1, 100, 101.00}},
        {"one-sided books still cross", {101.00, 0.0, 300, 0, 20},      {0.0, 100.04, 0, 200, 10},      0, 0, 1000, {1, +1, 200, 100.04}},
        {"one venue empty: nothing",    {0.0, 0.0, 0, 0, 20},           {100.02, 100.04, 200, 200, 10}, 0, 0, 1000, {}},
    };
    int pass = 0, n = 0;
    for (const Case& c : cases) {
        ++n;
        const Top v[2] = {c.a, c.b};
        Order got = detect_stale(v, c.fee_bps, c.position, c.limit);
        bool ok = got.venue == c.want.venue && got.side == c.want.side &&
                  got.qty == c.want.qty &&
                  (c.want.venue < 0 || (got.px > c.want.px - 1e-9 && got.px < c.want.px + 1e-9));
        pass += ok;
        std::printf("%-30s %s  got venue=%d side=%+d qty=%d px=%.2f\n", c.name,
                    ok ? "PASS" : "FAIL", got.venue, got.side, got.qty, got.px);
    }
    std::printf("%d/%d cases pass\n", pass, n);
    return pass == n ? 0 : 1;
}

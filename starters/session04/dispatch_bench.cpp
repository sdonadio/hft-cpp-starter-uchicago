// dispatch_bench.cpp — Session 4 lab: what does a virtual call really cost?
//
// Build (from the repo root):
//   c++ -std=c++20 -O2 -Itests starters/session04/dispatch_bench.cpp -o /tmp/dispatch && /tmp/dispatch
//   (or: make dispatch)
//
// Every case does the same arithmetic — one tiny "signal" per call — and only
// the DISPATCH mechanism changes. Two access patterns:
//   * mono  : every object is the same concrete type (the branch predictor
//             learns the single target after a few calls)
//   * mixed : four concrete types in a random order (the indirect branch now
//             has four targets and mispredicts often)
//   * sorted: the SAME four types, grouped by type (long runs of one target —
//             what you get if you batch work by type)
//
// Numbers are machine-specific: report yours with the CPU and compiler.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <random>
#include <variant>
#include <vector>
#include <chrono>

#include "bench.hpp"   // doNotOptimize()

// ── 1. The classic runtime-polymorphic interface ────────────────────────────
struct Signal {
    virtual long f(long x) const = 0;
    virtual ~Signal() = default;
};
struct Twice  final : Signal { long f(long x) const override { return 2 * x; } };
struct Thrice final : Signal { long f(long x) const override { return 3 * x; } };
struct Plus7  final : Signal { long f(long x) const override { return x + 7; } };
struct Xor5   final : Signal { long f(long x) const override { return x ^ 5; } };

// ── 2. The same four behaviours as plain value types (no base, no vptr) ─────
struct VTwice  { long f(long x) const { return 2 * x; } };
struct VThrice { long f(long x) const { return 3 * x; } };
struct VPlus7  { long f(long x) const { return x + 7; } };
struct VXor5   { long f(long x) const { return x ^ 5; } };
using AnySignal = std::variant<VTwice, VThrice, VPlus7, VXor5>;

// ── 3. A tag + switch, and plain function pointers ──────────────────────────
enum class Kind : std::uint8_t { Twice, Thrice, Plus7, Xor5 };
inline long by_switch(Kind k, long x) {
    switch (k) {
        case Kind::Twice:  return 2 * x;
        case Kind::Thrice: return 3 * x;
        case Kind::Plus7:  return x + 7;
        case Kind::Xor5:   return x ^ 5;
    }
    return 0;
}
long fp_twice(long x)  { return 2 * x; }
long fp_thrice(long x) { return 3 * x; }
long fp_plus7(long x)  { return x + 7; }
long fp_xor5(long x)   { return x ^ 5; }
using Fn = long (*)(long);

// ── harness ─────────────────────────────────────────────────────────────────
constexpr std::size_t N     = 4096;           // objects (fits in L1/L2)
constexpr long        ITERS = 20'000'000;     // calls per timed run
constexpr int         REPS  = 5;              // report the fastest run

template <class Body>
double time_ns(Body&& body) {
    double best = 1e30;
    for (int r = 0; r < REPS; ++r) {
        auto t0 = std::chrono::steady_clock::now();
        long s = body();
        auto t1 = std::chrono::steady_clock::now();
        doNotOptimize(s);
        double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / ITERS;
        if (ns < best) best = ns;
    }
    return best;
}

int main() {
    std::mt19937 rng(12345);
    std::vector<int> mixed(N), mono(N, 0);
    for (auto& k : mixed) k = static_cast<int>(rng() % 4);
    std::vector<int> sorted = mixed;
    std::sort(sorted.begin(), sorted.end());

    // Owning storage for the polymorphic objects: unique_ptr<Base>.
    auto make = [](int k) -> std::unique_ptr<Signal> {
        switch (k) {
            case 0:  return std::make_unique<Twice>();
            case 1:  return std::make_unique<Thrice>();
            case 2:  return std::make_unique<Plus7>();
            default: return std::make_unique<Xor5>();
        }
    };
    // One set of containers per access pattern (mono / mixed / sorted).
    struct Set {
        std::vector<std::unique_ptr<Signal>> own;   // owns the objects
        std::vector<const Signal*> ptr;              // non-owning views
        std::vector<AnySignal> var;
        std::vector<Kind> kind;
        std::vector<Fn> fn;
        std::vector<std::function<long(long)>> sfn;
    };
    constexpr std::array<Fn, 4> fns{fp_twice, fp_thrice, fp_plus7, fp_xor5};
    auto to_var = [](int k) -> AnySignal {
        switch (k) { case 0: return VTwice{}; case 1: return VThrice{};
                     case 2: return VPlus7{}; default: return VXor5{}; }
    };
    auto fill = [&](const std::vector<int>& kinds) {
        Set st;
        for (int k : kinds) {
            st.own.push_back(make(k));
            st.ptr.push_back(st.own.back().get());
            st.var.push_back(to_var(k));
            st.kind.push_back(static_cast<Kind>(k));
            st.fn.push_back(fns[k]);
            st.sfn.emplace_back(fns[k]);
        }
        return st;
    };
    const Set sets[3] = {fill(mono), fill(mixed), fill(sorted)};

    Twice exact;   // concrete, final type: the compiler knows the target

    struct Row { const char* name; double ns[3]; };
    std::vector<Row> rows;
    auto measure = [&](const char* name, auto one_call) {
        Row r{name, {0, 0, 0}};
        for (int p = 0; p < 3; ++p) {
            const Set& st = sets[p];
            r.ns[p] = time_ns([&] {
                long s = 0;
                for (long i = 0; i < ITERS; ++i) { s += one_call(st, i & (N - 1), i); doNotOptimize(s); }
                return s;
            });
        }
        rows.push_back(r);
    };

    measure("direct (exact final type, inlined)",
            [&](const Set&, std::size_t, long x) { return exact.f(x); });
    measure("virtual call through Signal*",
            [](const Set& st, std::size_t j, long x) { return st.ptr[j]->f(x); });
    measure("std::variant + std::visit",
            [](const Set& st, std::size_t j, long x) {
                return std::visit([x](const auto& g) { return g.f(x); }, st.var[j]); });
    measure("enum tag + switch",
            [](const Set& st, std::size_t j, long x) { return by_switch(st.kind[j], x); });
    measure("function pointer",
            [](const Set& st, std::size_t j, long x) { return st.fn[j](x); });
    measure("std::function",
            [](const Set& st, std::size_t j, long x) { return st.sfn[j](x); });

    // TODO(Session 5 lab): add a CRTP row here — Strategy<Derived> calling
    // static_cast<const Derived*>(this)->f_impl(x) — and compare it with the
    // first two rows. (Which pattern can CRTP serve: mono, mixed, or both?)

    std::printf("%-38s %9s %9s %9s   (ns per call)\n", "dispatch mechanism", "mono", "mixed", "sorted");
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const Row& r = rows[i];
        if (i == 0)   // the direct call ignores the pattern: one number is enough
            std::printf("%-38s %9.2f %9s %9s\n", r.name, r.ns[0], "-", "-");
        else
            std::printf("%-38s %9.2f %9.2f %9.2f\n", r.name, r.ns[0], r.ns[1], r.ns[2]);
    }
    std::printf("sizeof(Twice)=%zu (vptr only)  sizeof(VTwice)=%zu  sizeof(AnySignal)=%zu  "
                "sizeof(std::function<long(long)>)=%zu\n",
                sizeof(Twice), sizeof(VTwice), sizeof(AnySignal), sizeof(std::function<long(long)>));
    return 0;
}

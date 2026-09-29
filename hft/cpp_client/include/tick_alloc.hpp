// ─────────────────────────────────────────────────────────────────────────────
// tick_alloc.hpp — count heap allocations on your hot path (Session 4 lab).
//
// Replaces the global operator new with a counting version, and gives you two
// RAII scopes:
//
//   tickalloc::TickScope — put one at the top of on_book(): every allocation
//                          made while on_book() runs is charged to YOUR code.
//   tickalloc::SendScope — wrap an order send in one: allocations inside the
//                          client's send path are counted separately.
//
//   tickalloc::report(std::cerr) prints the totals.
//
// Include this header in EXACTLY ONE .cpp file (src/main.cpp): it defines the
// replacement operator new/delete, and a program may define them only once.
// The counters are plain (not atomic) and only count while a scope is open on
// the calling thread — on_book() runs on the receive thread.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstdio>
#include <cstdlib>
#include <new>
#include <ostream>

namespace tickalloc {

inline thread_local int mode = 0;          // 0 = not counting, 1 = on_book, 2 = send
inline long ticks = 0, tick_allocs = 0, sends = 0, send_allocs = 0;

struct TickScope {                          // RAII: the flag cannot be left on
    TickScope()  { mode = 1; ++ticks; }
    ~TickScope() { mode = 0; }
    TickScope(const TickScope&) = delete;
    TickScope& operator=(const TickScope&) = delete;
};

struct SendScope {                          // nests inside a TickScope
    int saved = mode;
    SendScope()  { saved = mode; mode = 2; ++sends; }
    ~SendScope() { mode = saved; }
    SendScope(const SendScope&) = delete;
    SendScope& operator=(const SendScope&) = delete;
};

inline void report(std::ostream& os) {
    os << "[tickalloc] on_book calls: " << ticks
       << " | allocations in your code: " << tick_allocs
       << " (" << (ticks ? double(tick_allocs) / double(ticks) : 0.0) << " per tick)"
       << " | order sends: " << sends
       << " | allocations inside sends: " << send_allocs
       << " (" << (sends ? double(send_allocs) / double(sends) : 0.0) << " per send)\n";
}

}  // namespace tickalloc

// Replacement global allocation functions (count, then defer to malloc/free).
void* operator new(std::size_t n) {
    if (tickalloc::mode == 1) ++tickalloc::tick_allocs;
    else if (tickalloc::mode == 2) ++tickalloc::send_allocs;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

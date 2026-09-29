// race.cpp — Session 7 lab, step 1. Two threads, one plain counter: a data race.
//   g++ -std=c++17 -O0 -pthread starters/session07/race.cpp -o /tmp/race      (see lost updates)
//   g++ -std=c++17 -O2 -pthread starters/session07/race.cpp -o /tmp/race_o2   (may LOOK right)
//   g++ -std=c++17 -O1 -g -pthread -fsanitize=thread starters/session07/race.cpp -o /tmp/race_tsan
// Build with -DVOLATILE to try the classic wrong fix (volatile long: still a race),
// or -DFIX_ATOMIC to use std::atomic<long> + fetch_add(relaxed) instead.
#include <cstdio>
#include <thread>
#ifdef FIX_ATOMIC
#include <atomic>
std::atomic<long> counter{0};
static void work() { for (int i = 0; i < 1'000'000; ++i) counter.fetch_add(1, std::memory_order_relaxed); }
#elif defined(VOLATILE)
volatile long counter = 0;                          // NOT a fix: no atomicity, no ordering
static void work() { for (int i = 0; i < 1'000'000; ++i) counter = counter + 1; }
#else
long counter = 0;                                   // shared, unsynchronized -> UB
static void work() { for (int i = 0; i < 1'000'000; ++i) ++counter; }
#endif

int main() {
    std::thread a(work), b(work);
    a.join(); b.join();
    std::printf("counter = %ld (expected 2000000)\n", (long)counter);
}

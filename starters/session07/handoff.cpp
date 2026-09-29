// handoff.cpp — Session 7 lab, step 3. Publish a payload with release/acquire.
//   g++ -std=c++17 -O2 -pthread starters/session07/handoff.cpp -o /tmp/handoff && /tmp/handoff
//   g++ -std=c++17 -O1 -g -pthread -fsanitize=thread starters/session07/handoff.cpp -o /tmp/handoff_tsan
// Rebuild with -DBROKEN (both orders become relaxed) and run the TSan build again.
#include <atomic>
#include <cstdio>
#include <thread>

#ifdef BROKEN
constexpr auto PUB = std::memory_order_relaxed, SUB = std::memory_order_relaxed;
#else
constexpr auto PUB = std::memory_order_release, SUB = std::memory_order_acquire;
#endif

struct Quote { double bid, ask; long seq; };        // plain, non-atomic payload
Quote q;
std::atomic<bool> ready{false};

int main() {
    std::thread producer([] {
        q = {100.01, 100.03, 42};                   // (1) write the payload
        ready.store(true, PUB);                     // (2) publish it
    });
    std::thread consumer([] {
        while (!ready.load(SUB)) {}                 // (3) wait for the flag
        std::printf("seq %ld  %.2f / %.2f\n", q.seq, q.bid, q.ask);   // (4) read it
    });
    producer.join(); consumer.join();
}

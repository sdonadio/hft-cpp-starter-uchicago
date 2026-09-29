// litmus_sb.cpp — Session 7 lab, step 3b. The store-buffering litmus test:
//   thread A: x = 1; r1 = y;        thread B: y = 1; r2 = x;
// Can BOTH threads read 0? The C++ model says: with relaxed OR release/acquire
// atomics, yes; with seq_cst on all four operations, never.
// Measured on an Apple M4 (clang -O2, 200,000 trials): relaxed ~199,800 both-zero;
// release/acquire 0 (Arm's stlr/ldar pair happens to be stronger than C++ requires -
// NOT observing an allowed outcome proves nothing); seq_cst 0.
// Build with -DACQ_REL for release stores + acquire loads, -DSEQ_CST for seq_cst.
//   g++ -std=c++17 -O2 -pthread starters/session07/litmus_sb.cpp -o /tmp/sb && /tmp/sb
//   g++ -std=c++17 -O2 -pthread -DSEQ_CST starters/session07/litmus_sb.cpp -o /tmp/sb_sc && /tmp/sb_sc
#include <atomic>
#include <cstdio>
#include <thread>

#if defined(SEQ_CST)
constexpr auto ST = std::memory_order_seq_cst, LD = std::memory_order_seq_cst;
#define NAME "seq_cst"
#elif defined(ACQ_REL)                               // release stores, acquire loads
constexpr auto ST = std::memory_order_release, LD = std::memory_order_acquire;
#define NAME "release/acquire"
#else
constexpr auto ST = std::memory_order_relaxed, LD = std::memory_order_relaxed;
#define NAME "relaxed"
#endif

std::atomic<int> x{0}, y{0};
std::atomic<int> start{0}, done{0};                 // trial handshake (acq/rel)
int r1, r2;

int main() {
    const int TRIALS = 200000;
    int both0 = 0;
    auto side = [](std::atomic<int>& mine, std::atomic<int>& other, int& r) {
        for (int i = 1; i <= TRIALS; ++i) {
            while (start.load(std::memory_order_acquire) != i) {}   // wait for trial i
            mine.store(1, ST);
            r = other.load(LD);
            done.fetch_add(1, std::memory_order_acq_rel);
        }
    };
    std::thread a(side, std::ref(x), std::ref(y), std::ref(r1));
    std::thread b(side, std::ref(y), std::ref(x), std::ref(r2));
    for (int i = 1; i <= TRIALS; ++i) {
        x.store(0); y.store(0); done.store(0);
        start.store(i, std::memory_order_release);                  // go
        while (done.load(std::memory_order_acquire) != 2) {}
        if (r1 == 0 && r2 == 0) ++both0;
    }
    a.join(); b.join();
    std::printf("r1 == 0 && r2 == 0 in %d of %d trials (%s)\n", both0, TRIALS, NAME);
}

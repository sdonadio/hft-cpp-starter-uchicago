// shm_pipeline.cpp — Session 7 lab, step 9 / Project Phase 4 starting point.
// Two SEPARATE processes share YOUR ShmRing (include/shm_ring.hpp) through a
// named POSIX shared-memory object. Run the strategy first, then the feed:
//   g++ -std=c++17 -O2 -Iinclude starters/session07/shm_pipeline.cpp -o /tmp/shm_pipe
//   /tmp/shm_pipe strategy &      # creates + init()s the region, then consumes
//   /tmp/shm_pipe feed            # maps the SAME region, produces 1,000,000 ticks
// (Linux may need -lrt on older glibc.)
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include "shm_ring.hpp"

static const char* NAME = "/finm32700_ring";         // macOS: <= 31 chars, leading '/'
static constexpr std::uint64_t N = 1'000'000;

static ShmRing* map_ring(bool create) {
    int fd = shm_open(NAME, create ? (O_CREAT | O_RDWR) : O_RDWR, 0600);
    if (fd < 0) { std::perror("shm_open"); return nullptr; }
    if (create && ftruncate(fd, sizeof(ShmRing)) != 0) { std::perror("ftruncate"); return nullptr; }
    void* p = mmap(nullptr, sizeof(ShmRing), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);                                        // the mapping keeps the object alive
    return p == MAP_FAILED ? nullptr : static_cast<ShmRing*>(p);
}

int main(int argc, char** argv) {
    if (argc < 2) { std::puts("usage: shm_pipe strategy | feed"); return 2; }
    if (!std::strcmp(argv[1], "strategy")) {
        shm_unlink(NAME);                             // start from a fresh object
        ShmRing* r = map_ring(true);
        if (!r) return 1;
        r->init();                                    // ONCE, before the feed attaches
        std::puts("strategy: ring ready, waiting for the feed...");
        std::uint64_t expect = 0, v = 0;
        auto t0 = std::chrono::steady_clock::now();
        bool started = false;
        while (expect < N) {
            if (!r->pop(v)) continue;
            if (!started) { t0 = std::chrono::steady_clock::now(); started = true; }
            if (v != expect) { std::printf("strategy: got %llu, expected %llu - lost/reordered\n",
                                           (unsigned long long)v, (unsigned long long)expect); return 1; }
            ++expect;
        }
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("strategy: %llu ticks in order across processes, %.1f M/s\n",
                    (unsigned long long)N, N / s / 1e6);
        munmap(r, sizeof(ShmRing));
        shm_unlink(NAME);
        return 0;
    }
    ShmRing* r = nullptr;                             // feed: attach to an existing region
    for (int i = 0; i < 200 && !(r = map_ring(false)); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!r) return 1;
    for (std::uint64_t i = 0; i < N; ++i) while (!r->push(i)) {}
    std::puts("feed: done");
    munmap(r, sizeof(ShmRing));
    return 0;
}

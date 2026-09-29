# Course Project — Build a Competitive HFT System in C++

You evolve one C++ trading bot across the semester into a tournament-ready
low-latency system in the **AlgoArena** arena. Each phase builds on that block's
lectures and is measured by the arena's own instrumentation. Full specs +
due dates are on Canvas; this is the working checklist.

Get and build the arena C++ client first: see [`../docs/HFT_CPP_CLIENT.md`](../docs/HFT_CPP_CLIENT.md).

| Phase | Sessions | You build | Graded on |
|------:|-------|-----------|-----------|
| 0 | 1–2 | Connect the client; react in `on_book`; place/cancel | on the LATENCY board + baseline p50/p99/p99.9 |
| 1 | 3–5 | No-alloc hot path; compile-time codec (variant/CRTP) | tick-to-trade vs your baseline |
| 2 | 6 | Cache-friendly local book; track `queue_ahead` | hot-path latency + book correctness |
| 3 | 7 | Threaded pipeline + **`include/spsc_ring.hpp`** | throughput + p99.9 + ThreadSanitizer |
| 4 | 7 | Multi-process gateway/strategy + **`include/shm_ring.hpp`** | cross-process IPC latency + resilience |
| 5 | 8 | SIMD/prefetch/LTO; colocation trade-off | end-to-end p50/p99.9 |
| 6 | 9 | Profile & kill the tail | measured p99.9 reduction |
| 7 | 9 | A real strategy in the tournament scenario | composite (latency + queue + fill + markout) |

## Deliverable per phase

Submit on Canvas: a link to your repo at a tagged commit **+** a short report
with the measured numbers. Phases 3 and 4 are **autograded here** — implement
`include/spsc_ring.hpp` and `include/shm_ring.hpp` and `make test` (or CI) scores them.

## The interfaces the autograder expects

- `include/spsc_ring.hpp` — `SPSCRing(capacity_pow2)` · `push`/`pop`/`empty`/`full`
- `include/shm_ring.hpp` — POD (no pointers), `CAPACITY`, `init`/`push`/`pop`;
  the grader `mmap`s a shared region, **forks**, and runs producer/consumer through it.

Keep the signatures as given — CI compiles the course tests against them.

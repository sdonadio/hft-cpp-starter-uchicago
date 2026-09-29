# HFT starter — local autograder + per-challenge quick builds.
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -pthread -Iinclude -Itests

.PHONY: test clean pool book fix u64toa rolling spsc dispatch spsc-conc spsc-tsan shm bench-alloc bench-book client register run

test:                      ## run the full autograder (grades what you've implemented)
	python3 tests/run_ci.py

# quick single-challenge builds while you iterate:
pool:    ; $(CXX) $(CXXFLAGS) tests/pool_test.cpp    -o /tmp/pool    && /tmp/pool
book:    ; $(CXX) $(CXXFLAGS) tests/book_test.cpp    -o /tmp/book    && /tmp/book
fix:     ; $(CXX) $(CXXFLAGS) tests/fix_test.cpp     -o /tmp/fix     && /tmp/fix
u64toa:  ; $(CXX) $(CXXFLAGS) tests/u64toa_test.cpp  -o /tmp/u64toa  && /tmp/u64toa
rolling: ; $(CXX) $(CXXFLAGS) tests/rolling_counter_test.cpp -o /tmp/rolling && /tmp/rolling
spsc:    ; $(CXX) $(CXXFLAGS) tests/spsc_correctness.cpp -o /tmp/spsc && /tmp/spsc

# Session 4 lab: the dispatch-cost benchmark (C++20, -O2)
dispatch: ; $(CXX) -std=c++20 -O2 -Itests starters/session04/dispatch_bench.cpp -o /tmp/dispatch && /tmp/dispatch
spsc-conc: ; $(CXX) $(CXXFLAGS) tests/spsc_concurrency.cpp -o /tmp/spsc_conc && /tmp/spsc_conc
spsc-tsan: ; $(CXX) -std=c++17 -O1 -g -pthread -fsanitize=thread -Iinclude tests/spsc_concurrency.cpp -o /tmp/spsc_tsan && SPSC_N=200000 /tmp/spsc_tsan
shm:     ; $(CXX) $(CXXFLAGS) tests/shm_ring_test.cpp -o /tmp/shm && /tmp/shm

# Session 6 lab measurements (use YOUR include/pool.hpp and include/order_book.hpp):
bench-alloc: ; $(CXX) $(CXXFLAGS) starters/session06/bench_alloc.cpp -o /tmp/bench_alloc && /tmp/bench_alloc
bench-book:  ; $(CXX) $(CXXFLAGS) starters/session06/bench_book.cpp  -o /tmp/bench_book  && /tmp/bench_book

clean:   ; rm -f /tmp/pool /tmp/book /tmp/fix /tmp/u64toa /tmp/rolling /tmp/spsc /tmp/spsc_conc /tmp/spsc_tsan /tmp/shm /tmp/bench_alloc /tmp/bench_book /tmp/dispatch report.json

# ── Arena: build the C++ client, register your team, run your bot ─────────────
ARENA ?= https://algoarena-uc.duckdns.org
client:                    ## build hft/cpp_client with TLS -> hft/cpp_client/build/hft_bot
	cmake -S hft/cpp_client -B hft/cpp_client/build -DHFT_USE_TLS=ON && cmake --build hft/cpp_client/build
register:                  ## make register CODE=<class code> NAME="Your Team"  (writes .env)
	python3 scripts/register.py --arena $(ARENA) --code $(CODE) --name "$(NAME)"
run:                       ## start your bot with the credentials in .env
	./run_bot.sh

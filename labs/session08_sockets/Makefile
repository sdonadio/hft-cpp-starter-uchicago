# Session 8 warm-up: TCP and UDP multicast moving-average client/server.
# POSIX sockets only, no dependencies. Linux and macOS.
CXX      ?= c++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -pthread

BINS = tcp_price_server tcp_ma_client udp_price_publisher udp_ma_client

all: $(BINS)

%: %.cpp common.hpp
	$(CXX) $(CXXFLAGS) $< -o $@

clean:
	rm -f $(BINS)

.PHONY: all clean

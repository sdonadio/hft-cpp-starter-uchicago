// common.hpp: shared by the four Session 8 warm-up programs.
// Header-only, POSIX + the C++20 standard library, nothing else.
#pragma once

#include <cstddef>
#include <cstdint>
#include <random>
#include <string_view>
#include <vector>

namespace s8 {

// "--name value" on the command line, or the fallback when the flag is absent.
inline const char* arg(int argc, char** argv, std::string_view name, const char* fallback) {
  for (int i = 1; i + 1 < argc; ++i)
    if (name == argv[i]) return argv[i + 1];
  return fallback;
}

// A deterministic random walk in integer cents, so both servers send the same prices
// on every machine. std::mt19937's output is fixed by the standard; the distributions
// are not, which is why we take a plain modulo instead of uniform_int_distribution.
class PriceWalk {
 public:
  explicit PriceWalk(std::uint32_t seed = 42, std::int64_t start_cents = 10000)
      : rng_(seed), cents_(start_cents) {}
  std::int64_t next() {
    cents_ += static_cast<std::int64_t>(rng_() % 11) - 5;  // move -5 .. +5 cents
    if (cents_ < 1) cents_ = 1;
    return cents_;
  }

 private:
  std::mt19937 rng_;
  std::int64_t cents_;
};

// N-point simple moving average in O(1) per update: a ring of the last N prices and a
// running sum. Add the new price, subtract the one that falls out of the window.
class MovingAverage {
 public:
  explicit MovingAverage(std::size_t n) : ring_(n, 0.0) {}
  double push(double x) {
    if (count_ == ring_.size()) sum_ -= ring_[head_];  // oldest leaves the window
    else ++count_;
    ring_[head_] = x;
    sum_ += x;
    head_ = (head_ + 1) % ring_.size();
    return sum_ / static_cast<double>(count_);
  }
  bool full() const { return count_ == ring_.size(); }
  std::size_t count() const { return count_; }

 private:
  std::vector<double> ring_;
  std::size_t head_ = 0, count_ = 0;
  double sum_ = 0.0;
};

// The UDP packet: 16 bytes, both fields big-endian (network byte order).
//   bytes 0..7   sequence number: starts at 1, +1 per packet, dropped packets included
//   bytes 8..15  price in integer cents
inline constexpr std::size_t kPacketSize = 16;

inline void put_be64(unsigned char* p, std::uint64_t v) {
  for (int i = 7; i >= 0; --i) { p[i] = static_cast<unsigned char>(v & 0xff); v >>= 8; }
}
inline std::uint64_t get_be64(const unsigned char* p) {
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
  return v;
}

}  // namespace s8

// udp_price_publisher.cpp: publish prices to a multicast group, one datagram each,
// with a sequence number. Nobody connects to a publisher; it never learns who listens.
//
//   ./udp_price_publisher [--group 239.255.0.1] [--port 30001] [--iface 127.0.0.1]
//                         [--count 50] [--interval-ms 100] [--drop 0.0] [--seed 7]
//
// --drop P skips each send with probability P (0..1) but still uses up its sequence
// number, exactly like a packet lost on the path. --count 0 publishes forever.
#include "common.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>

int main(int argc, char** argv) {
  const char* group = s8::arg(argc, argv, "--group", "239.255.0.1");
  const int port = std::atoi(s8::arg(argc, argv, "--port", "30001"));
  const char* iface = s8::arg(argc, argv, "--iface", "127.0.0.1");
  const long count = std::atol(s8::arg(argc, argv, "--count", "50"));
  const int interval_ms = std::atoi(s8::arg(argc, argv, "--interval-ms", "100"));
  const double drop = std::atof(s8::arg(argc, argv, "--drop", "0"));
  const auto seed = static_cast<std::uint32_t>(std::atol(s8::arg(argc, argv, "--seed", "7")));

  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) { std::perror("socket"); return 1; }

  unsigned char ttl = 1;   // stay on the local network: routers will not forward it
  unsigned char loop = 1;  // also deliver to receivers on this same host
  ::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
  ::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop);
  in_addr out{};
  if (::inet_pton(AF_INET, iface, &out) != 1) { std::fprintf(stderr, "bad --iface\n"); return 1; }
  if (::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &out, sizeof out) < 0) {  // which NIC sends
    std::perror("IP_MULTICAST_IF");
    return 1;
  }

  sockaddr_in dst{};
  dst.sin_family = AF_INET;
  dst.sin_port = htons(static_cast<std::uint16_t>(port));
  if (::inet_pton(AF_INET, group, &dst.sin_addr) != 1) { std::fprintf(stderr, "bad --group\n"); return 1; }

  std::printf("[pub] group %s:%d via %s, one price every %d ms, drop %.0f%%\n",
              group, port, iface, interval_ms, drop * 100.0);
  std::fflush(stdout);

  s8::PriceWalk walk;  // same prices as the TCP server
  std::mt19937 rng(seed);
  std::uniform_real_distribution<double> u01(0.0, 1.0);
  long dropped = 0;
  for (std::uint64_t seq = 1; count == 0 || seq <= static_cast<std::uint64_t>(count); ++seq) {
    const std::int64_t cents = walk.next();
    const double px = static_cast<double>(cents) / 100.0;
    if (drop > 0.0 && u01(rng) < drop) {
      ++dropped;
      std::printf("[pub] seq %3llu price %7.2f   DROPPED (simulated loss)\n",
                  static_cast<unsigned long long>(seq), px);
    } else {
      unsigned char pkt[s8::kPacketSize];
      s8::put_be64(pkt, seq);
      s8::put_be64(pkt + 8, static_cast<std::uint64_t>(cents));
      const ssize_t n = ::sendto(fd, pkt, sizeof pkt, 0, reinterpret_cast<sockaddr*>(&dst), sizeof dst);
      if (n != static_cast<ssize_t>(sizeof pkt)) { std::perror("sendto"); return 1; }
      std::printf("[pub] seq %3llu price %7.2f\n", static_cast<unsigned long long>(seq), px);
    }
    std::fflush(stdout);
    std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
  }
  std::printf("[pub] done, %ld dropped on purpose\n", dropped);
  ::close(fd);
  return 0;
}

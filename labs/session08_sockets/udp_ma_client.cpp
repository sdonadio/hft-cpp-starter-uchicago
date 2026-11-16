// udp_ma_client.cpp: join a multicast group, print an N-point SMA, detect gaps.
//
//   ./udp_ma_client [--group 239.255.0.1] [--port 30001] [--iface 127.0.0.1]
//                   [--window 5] [--idle-ms 3000]
//
// UDP has no "connection closed": the client stops after --idle-ms with no data.
#include "common.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
  const char* group = s8::arg(argc, argv, "--group", "239.255.0.1");
  const int port = std::atoi(s8::arg(argc, argv, "--port", "30001"));
  const char* iface = s8::arg(argc, argv, "--iface", "127.0.0.1");
  const int window = std::atoi(s8::arg(argc, argv, "--window", "5"));
  const int idle_ms = std::atoi(s8::arg(argc, argv, "--idle-ms", "3000"));
  if (window < 1) { std::fprintf(stderr, "--window must be at least 1\n"); return 1; }

  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) { std::perror("socket"); return 1; }

  // Several clients on one host must bind the same port. Linux: SO_REUSEADDR is enough
  // for multicast. macOS / BSD: SO_REUSEPORT is also needed. (On Linux, SO_REUSEPORT
  // means load balancing for unicast, which is not what we want here.)
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#if defined(SO_REUSEPORT) && !defined(__linux__)
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<std::uint16_t>(port));
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) { std::perror("bind"); return 1; }

  // Join the group on one interface: the kernel sends an IGMP join, the switch starts
  // copying the group's packets to us. The publisher never finds out.
  ip_mreq m{};
  if (::inet_pton(AF_INET, group, &m.imr_multiaddr) != 1 || ::inet_pton(AF_INET, iface, &m.imr_interface) != 1) {
    std::fprintf(stderr, "bad --group or --iface\n");
    return 1;
  }
  if (::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof m) < 0) {
    std::perror("IP_ADD_MEMBERSHIP");
    return 1;
  }

  timeval tv{};
  tv.tv_sec = idle_ms / 1000;
  tv.tv_usec = (idle_ms % 1000) * 1000;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  std::printf("[client] joined %s:%d on %s, window N=%d\n", group, port, iface, window);
  std::fflush(stdout);

  s8::MovingAverage ma(static_cast<std::size_t>(window));
  std::uint64_t expected = 0;  // 0 = not synced yet: the first packet sets it
  long received = 0, gaps = 0, late = 0;
  unsigned long long missing = 0;
  double last = 0.0;
  for (;;) {
    unsigned char buf[1500];
    const ssize_t n = ::recvfrom(fd, buf, sizeof buf, 0, nullptr, nullptr);  // exactly one datagram
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      std::printf("[client] no data for %d ms: publisher finished or gone\n", idle_ms);
      break;
    }
    if (n < 0) { std::perror("recvfrom"); break; }
    if (n != static_cast<ssize_t>(s8::kPacketSize)) {
      std::printf("[client] ignored a %zd-byte datagram\n", n);
      continue;
    }
    const std::uint64_t seq = s8::get_be64(buf);
    const double px = static_cast<double>(static_cast<std::int64_t>(s8::get_be64(buf + 8))) / 100.0;

    if (expected == 0) {
      std::printf("[client] first packet is seq %llu: synced\n", static_cast<unsigned long long>(seq));
      expected = seq;
    }
    if (seq < expected) {  // late or duplicate (an A/B feed sends everything twice)
      ++late;
      std::printf("[client] seq %llu is late or a duplicate: ignored\n", static_cast<unsigned long long>(seq));
      continue;
    }
    if (seq > expected) {  // the only evidence of loss is the jump
      ++gaps;
      missing += seq - expected;
      std::printf("[client] GAP: expected seq %llu, got %llu (%llu missing)\n",
                  static_cast<unsigned long long>(expected), static_cast<unsigned long long>(seq),
                  static_cast<unsigned long long>(seq - expected));
    }
    expected = seq + 1;
    ++received;
    last = ma.push(px);
    if (ma.full())
      std::printf("seq %3llu price %7.2f   sma%d %9.4f\n", static_cast<unsigned long long>(seq), px, window, last);
    else
      std::printf("seq %3llu price %7.2f   sma%d warming up (%zu/%d)\n",
                  static_cast<unsigned long long>(seq), px, window, ma.count(), window);
    std::fflush(stdout);
  }
  std::printf("[client] received %ld, gaps %ld, missing %llu, late %ld, last sma%d = %.4f\n",
              received, gaps, missing, late, window, last);
  ::setsockopt(fd, IPPROTO_IP, IP_DROP_MEMBERSHIP, &m, sizeof m);
  ::close(fd);
  return 0;
}

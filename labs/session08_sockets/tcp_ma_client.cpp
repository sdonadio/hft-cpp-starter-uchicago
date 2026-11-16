// tcp_ma_client.cpp: connect, frame the byte stream into lines, print an N-point SMA.
//
//   ./tcp_ma_client [--host 127.0.0.1] [--port 5555] [--window 5] [--show-recv 1]
//
// --show-recv 1 prints the size of every recv(), so you can see that one recv() is
// not one message.
#include "common.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
  const char* host = s8::arg(argc, argv, "--host", "127.0.0.1");
  const int port = std::atoi(s8::arg(argc, argv, "--port", "5555"));
  const int window = std::atoi(s8::arg(argc, argv, "--window", "5"));
  const bool show_recv = std::atoi(s8::arg(argc, argv, "--show-recv", "0")) != 0;
  if (window < 1) { std::fprintf(stderr, "--window must be at least 1\n"); return 1; }

  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) { std::perror("socket"); return 1; }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<std::uint16_t>(port));
  if (::inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
    std::fprintf(stderr, "--host must be a dotted IPv4 address, e.g. 127.0.0.1\n");
    return 1;
  }
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {  // SYN, SYN-ACK, ACK
    std::perror("connect");
    std::fprintf(stderr, "is tcp_price_server running on %s:%d?\n", host, port);
    return 1;
  }
  std::printf("[client] connected to %s:%d, window N=%d\n", host, port, window);

  s8::MovingAverage ma(static_cast<std::size_t>(window));
  std::string buf;  // bytes received but not yet framed into a full line
  char tmp[4096];
  long msgs = 0;
  double last = 0.0;
  for (;;) {
    ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) { std::perror("recv"); break; }
    if (n == 0) { std::printf("[client] server closed the connection\n"); break; }
    if (show_recv) std::printf("[client] recv() returned %zd bytes\n", n);
    buf.append(tmp, static_cast<std::size_t>(n));

    // Frame: every complete line is one price. Keep the unfinished tail for next time.
    std::size_t start = 0, nl;
    while ((nl = buf.find('\n', start)) != std::string::npos) {
      char* end = nullptr;
      const double px = std::strtod(buf.c_str() + start, &end);  // stops at the '\n'
      const bool good = end == buf.c_str() + nl;
      start = nl + 1;
      if (!good) { std::fprintf(stderr, "[client] bad line skipped\n"); continue; }
      last = ma.push(px);
      ++msgs;
      if (ma.full())
        std::printf("#%-3ld price %7.2f   sma%d %9.4f\n", msgs, px, window, last);
      else
        std::printf("#%-3ld price %7.2f   sma%d warming up (%zu/%d)\n", msgs, px, window, ma.count(), window);
    }
    buf.erase(0, start);
  }
  if (!buf.empty()) std::printf("[client] %zu bytes of an unfinished line left over\n", buf.size());
  std::printf("[client] %ld prices, last sma%d = %.4f\n", msgs, window, last);
  ::close(fd);
  return 0;
}

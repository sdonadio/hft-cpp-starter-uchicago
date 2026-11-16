// tcp_price_server.cpp: accept one client at a time and send it a stream of prices.
// Framing: one price per line, "100.12\n". The README explains newline vs length prefix.
//
//   ./tcp_price_server [--port 5555] [--count 50] [--interval-ms 100] [--split 1]
//
// --split 1 sends every line in two pieces, 20 ms apart, to show that TCP does not
// keep your message boundaries: the client must frame the byte stream itself.
#include "common.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <thread>

// send() may write fewer bytes than asked: loop until the whole buffer is out.
static bool send_all(int fd, const char* p, std::size_t n) {
  while (n > 0) {
    ssize_t k = ::send(fd, p, n, 0);
    if (k < 0 && errno == EINTR) continue;
    if (k <= 0) return false;  // the client went away (EPIPE, ECONNRESET)
    p += k;
    n -= static_cast<std::size_t>(k);
  }
  return true;
}

int main(int argc, char** argv) {
  const int port = std::atoi(s8::arg(argc, argv, "--port", "5555"));
  const int count = std::atoi(s8::arg(argc, argv, "--count", "50"));
  const int interval_ms = std::atoi(s8::arg(argc, argv, "--interval-ms", "100"));
  const bool split = std::atoi(s8::arg(argc, argv, "--split", "0")) != 0;
  std::signal(SIGPIPE, SIG_IGN);  // a vanished client must give EPIPE, not kill the server

  int ls = ::socket(AF_INET, SOCK_STREAM, 0);
  if (ls < 0) { std::perror("socket"); return 1; }
  int one = 1;
  // Restart right away on the same port instead of waiting out TIME_WAIT.
  ::setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<std::uint16_t>(port));
  if (::bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) { std::perror("bind"); return 1; }
  if (::listen(ls, 8) < 0) { std::perror("listen"); return 1; }
  std::printf("[server] listening on port %d: %d prices per client, one every %d ms%s\n",
              port, count, interval_ms, split ? ", each line split in two sends" : "");
  std::fflush(stdout);

  for (;;) {
    sockaddr_in peer{};
    socklen_t plen = sizeof peer;
    int c = ::accept(ls, reinterpret_cast<sockaddr*>(&peer), &plen);  // a NEW socket per client
    if (c < 0) {
      if (errno == EINTR) continue;
      std::perror("accept");
      return 1;
    }
    char ip[INET_ADDRSTRLEN];
    ::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);
    std::printf("[server] client %s:%d connected\n", ip, ntohs(peer.sin_port));
    std::fflush(stdout);

    // Small messages: send each one now instead of letting Nagle wait to batch them.
    ::setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    s8::PriceWalk walk;  // same seed: every client gets the same stream
    int sent = 0;
    bool ok = true;
    for (; sent < count && ok; ++sent) {
      const std::int64_t cents = walk.next();
      char line[32];
      const int n = std::snprintf(line, sizeof line, "%lld.%02lld\n",
                                  static_cast<long long>(cents / 100), static_cast<long long>(cents % 100));
      if (split) {
        const int half = n / 2;
        ok = send_all(c, line, static_cast<std::size_t>(half));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        ok = ok && send_all(c, line + half, static_cast<std::size_t>(n - half));
      } else {
        ok = send_all(c, line, static_cast<std::size_t>(n));
      }
      if (ok) std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }
    if (!ok) --sent;  // the last line did not make it out
    std::printf("[server] %s after %d prices; waiting for the next client\n",
                ok ? "done" : "client disconnected", sent);
    std::fflush(stdout);
    ::close(c);
  }
}

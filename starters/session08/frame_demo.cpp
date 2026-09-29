// frame_demo.cpp — Session 8 lab, step 3: frame a FIX byte STREAM.
//
// A TCP socket hands you bytes, not messages. This demo pushes four FIX
// NewOrderSingle messages through a non-blocking AF_UNIX stream socketpair in
// deliberately awkward chunks (1 byte, 7 bytes, half a message, two at once),
// and a poll()-driven reader:
//   * drains the socket until EAGAIN into ONE reusable buffer,
//   * frames each message from BodyLength (tag 9): 8=...|9=NN| + NN bytes + "10=XXX|",
//   * verifies the CheckSum (tag 10) and drops a message that fails it,
//   * hands complete, verified frames to YOUR parse_new_order (include/fix_parser.hpp).
//
// Portable: poll() + O_NONBLOCK work on Linux and macOS (epoll/kqueue are the
// production versions of the same readiness loop).
//
// Build:  g++ -std=c++20 -O2 -Iinclude starters/session08/frame_demo.cpp -o /tmp/frame
// Run:    /tmp/frame
// Until parse_new_order is implemented, frames are found but "parse FAILED".

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "fix_parser.hpp"

// ── helpers ──────────────────────────────────────────────────────────────────
// Build a correct FIX message from body fields (setup code, NOT the hot path).
static std::string make_fix(const std::vector<std::string>& fields) {
    std::string body;
    for (const auto& f : fields) { body += f; body += '\x01'; }
    std::string msg = "8=FIX.4.2\x01" "9=" + std::to_string(body.size()) + "\x01" + body;
    unsigned sum = 0;
    for (unsigned char c : msg) sum += c;
    char tail[8];
    std::snprintf(tail, sizeof tail, "10=%03u\x01", sum % 256);
    return msg + tail;
}

// FIX CheckSum: byte sum of everything before "10=", mod 256, as 3 digits.
static bool checksum_ok(const char* msg, const char* tail) {
    unsigned sum = 0;
    for (const char* p = msg; p < tail; ++p) sum += static_cast<unsigned char>(*p);
    unsigned want = unsigned(tail[3] - '0') * 100 + unsigned(tail[4] - '0') * 10
                  + unsigned(tail[5] - '0');
    return (sum & 0xFF) == want;
}

// ── the reader ───────────────────────────────────────────────────────────────
struct Stats { int frames = 0, parsed = 0, bad_checksum = 0, reads = 0, partial = 0; };

struct FixConn {
    int    fd = -1;
    char   buf[1 << 16];          // allocated once (a member), reused every wake-up
    size_t len = 0;
    Stats  st;

    static constexpr size_t kMaxBody = 4096;   // never trust a length blindly

    // Try to cut ONE complete message starting at buf+off. Returns its total
    // length, 0 if it is still partial, or SIZE_MAX if the header is malformed.
    size_t frame_at(size_t off) const {
        const char*  p = buf + off;
        const size_t n = len - off;
        static constexpr char kBegin[] = "8=FIX.4.2\x01" "9=";
        const size_t kb = sizeof kBegin - 1;               // 12 bytes
        if (n < kb) return 0;                              // header not here yet
        if (std::memcmp(p, kBegin, kb) != 0) return SIZE_MAX;
        size_t i = kb, body = 0;
        while (i < n && p[i] >= '0' && p[i] <= '9') body = body * 10 + size_t(p[i++] - '0');
        if (i == n) return 0;                              // digits still arriving
        if (p[i] != '\x01' || body == 0 || body > kMaxBody) return SIZE_MAX;
        const size_t total = i + 1 + body + 7;             // + "10=XXX" + SOH
        return n < total ? 0 : total;
    }

    void dispatch(const char* m, size_t total) {
        ++st.frames;
        const char* tail = m + total - 7;                  // points at "10="
        if (std::memcmp(tail, "10=", 3) != 0 || !checksum_ok(m, tail)) {
            ++st.bad_checksum;
            std::printf("  frame %d: %zu bytes, CHECKSUM MISMATCH -> dropped\n", st.frames, total);
            return;
        }
        NewOrder o{};
        if (parse_new_order(m, int(total), o)) {
            ++st.parsed;
            std::printf("  frame %d: %zu bytes, %.*s %s %u @ %.2f\n", st.frames, total,
                        o.clordid_len, o.clordid, o.side == '1' ? "BUY " : "SELL",
                        o.qty, o.price);
        } else {
            std::printf("  frame %d: %zu bytes, checksum ok, parse FAILED "
                        "(implement include/fix_parser.hpp)\n", st.frames, total);
        }
    }

    // Called when poll() says readable. false = peer closed or error.
    bool on_readable() {
        for (;;) {
            ssize_t r = ::recv(fd, buf + len, sizeof buf - len, 0);
            if (r > 0) {
                ++st.reads;
                len += size_t(r);
                size_t off = 0;
                for (;;) {
                    size_t t = frame_at(off);
                    if (t == SIZE_MAX) { std::printf("  malformed header: resetting\n"); off = len; break; }
                    if (t == 0) { if (off < len) ++st.partial; break; }
                    dispatch(buf + off, t);
                    off += t;
                }
                std::memmove(buf, buf + off, len - off);   // keep only the partial tail
                len -= off;
                continue;
            }
            if (r == 0) return false;                               // peer closed
            if (errno == EAGAIN || errno == EWOULDBLOCK) return true; // drained
            if (errno != EINTR) { std::perror("recv"); return false; }
        }
    }
};

int main() {
    // Four messages; the third gets a corrupted byte AFTER its checksum was computed.
    std::string stream;
    stream += make_fix({"35=D", "11=ORD1", "55=AAPL", "54=1", "38=100", "44=185.50"});
    stream += make_fix({"35=D", "11=ORD2", "55=NVDA", "54=2", "38=25", "44=182.25"});
    std::string bad = make_fix({"35=D", "11=ORD3", "55=MSFT", "54=1", "38=10", "44=410.00"});
    bad[bad.find("38=10") + 3] = '9';                       // 38=10 -> 38=90: tampered
    stream += bad;
    stream += make_fix({"35=D", "11=ORD4", "55=TSLA", "54=2", "38=5", "44=251.10"});

    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { std::perror("socketpair"); return 1; }
    ::fcntl(sv[0], F_SETFL, ::fcntl(sv[0], F_GETFL) | O_NONBLOCK);   // the reader never blocks

    static FixConn conn;                                    // 64 KiB buffer: not on the stack
    conn.fd = sv[0];

    // Writer: awkward chunk sizes, so frames split across reads and share reads.
    const size_t chunks[] = {1, 7, 40, 3, 90, 200, 11, 64, 1000};
    size_t sent = 0;
    for (size_t c : chunks) {
        if (sent >= stream.size()) break;
        size_t n = std::min(c, stream.size() - sent);
        if (::write(sv[1], stream.data() + sent, n) != ssize_t(n)) { std::perror("write"); return 1; }
        sent += n;
        std::printf("wrote %4zu bytes (%zu/%zu)\n", n, sent, stream.size());

        pollfd pfd{sv[0], POLLIN, 0};
        if (::poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN))  // readiness, not a blocking read
            conn.on_readable();
    }
    ::close(sv[1]);
    pollfd pfd{sv[0], POLLIN, 100};
    ::poll(&pfd, 1, 100);
    conn.on_readable();                                     // drain, then see EOF
    ::close(sv[0]);

    std::printf("frames=%d parsed=%d bad_checksum=%d recv_calls=%d partial_waits=%d leftover=%zu\n",
                conn.st.frames, conn.st.parsed, conn.st.bad_checksum, conn.st.reads,
                conn.st.partial, conn.len);
    return 0;
}

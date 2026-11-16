# Session 8 warm-up: a moving average over TCP, then over UDP multicast

A server sends prices. A client computes a running N-point simple moving average (SMA)
and prints it. You build it twice: once over a TCP connection, once over UDP multicast
with sequence numbers and gap detection. Same prices both times (same random walk,
same seed), so the two clients print the same averages until a packet goes missing.

POSIX sockets and the C++20 standard library only. Builds on Linux and macOS.

| File | What it does |
|---|---|
| `common.hpp` | argument helper, the price walk, the SMA ring, big-endian helpers |
| `tcp_price_server.cpp` | listens on a port, sends each client 50 prices, one per line |
| `tcp_ma_client.cpp` | connects, frames the byte stream into lines, prints the SMA |
| `udp_price_publisher.cpp` | sends 16-byte datagrams (seq, price) to a multicast group, `--drop P` |
| `udp_ma_client.cpp` | joins the group, prints the SMA, reports gaps |

## Build

```bash
cd labs/session08_sockets
make            # four binaries, -std=c++20 -O2 -Wall -Wextra
```

## Part A: TCP

Two terminals.

```bash
./tcp_price_server                 # terminal 1: listens on 5555
./tcp_ma_client --window 5         # terminal 2
```

Expected (first lines and the end):

```
[client] connected to 127.0.0.1:5555, window N=5
#1   price  100.01   sma5 warming up (1/5)
#2   price  100.03   sma5 warming up (2/5)
#3   price  100.07   sma5 warming up (3/5)
#4   price  100.05   sma5 warming up (4/5)
#5   price  100.02   sma5  100.0360
#6   price  100.07   sma5  100.0480
#7   price  100.05   sma5  100.0520
...
[client] server closed the connection
[client] 50 prices, last sma5 = 100.0040
```

**Framing: why a newline and not a length prefix.** TCP is a byte stream. Your 50
`send()` calls do not arrive as 50 `recv()` results: one `recv()` can return half a
line or three lines. So every TCP protocol needs framing. Two common choices:

- **Delimiter** (here, `'\n'`). Good for text: you can read the stream with `nc 127.0.0.1 5555`.
  The delimiter must never appear inside a message, and the reader scans every byte.
- **Length prefix** (for example a 2-byte big-endian length, then the body). Good for binary
  messages, where any byte value can appear. The reader knows how many bytes to wait for
  without scanning. Most binary exchange protocols do this (SoupBinTCP, SBE framing).

A price as text is short and never contains `'\n'`, so a newline is the simplest correct choice.

Things to try:

1. `./tcp_ma_client --show-recv 1` prints every `recv()` size. Then restart the server with
   `--split 1` (each line goes out in two pieces, 20 ms apart): `recv()` now returns 3 or 4
   bytes, and the client still prints one price per line because it frames the stream.
2. `./tcp_price_server --interval-ms 0 --count 2000` with `--show-recv 1`: now several lines
   arrive in one `recv()` (12, 14, 18 bytes...).
3. Kill the server (Ctrl-C) mid-stream: `recv()` returns 0 and the client prints
   `server closed the connection`. Kill the client instead: the server's `send()` fails
   (EPIPE) and it prints `client disconnected after N prices`.
4. Start two clients at once. The second one waits: this server handles one client at a
   time, and the second connection sits in the `listen()` backlog until the first is done.
   One thread serving many sockets needs `poll`/`epoll`/`kqueue` (end of the page).
5. `nc 127.0.0.1 5555` shows the raw stream.

## Part B: UDP multicast

Start the client(s) first, then the publisher.

```bash
./udp_ma_client --window 5                 # terminal 2 (and 3, for two clients)
./udp_price_publisher                      # terminal 1: 50 datagrams to 239.255.0.1:30001
./udp_price_publisher --drop 0.2           # lose about 1 packet in 5, on purpose
```

The packet is 16 bytes, big-endian: 8 bytes of sequence number, 8 bytes of price in cents.
A dropped packet still uses up its sequence number, exactly like a packet lost on the path,
so the client sees the jump:

```
[client] joined 239.255.0.1:30001 on 127.0.0.1, window N=5
[client] first packet is seq 1: synced
seq   1 price  100.01   sma5 warming up (1/5)
...
seq   6 price  100.07   sma5  100.0480
[client] GAP: expected seq 7, got 8 (1 missing)
seq   8 price  100.00   sma5  100.0420
...
[client] no data for 3000 ms: publisher finished or gone
[client] received 10, gaps 2, missing 2, late 0, last sma5 = 100.0080
```

(That is `./udp_price_publisher --count 12 --drop 0.2`: with the default `--seed 7`, seq 7 and 9 are dropped.
With the default 50 prices the same seed drops 7, 9, 23, 29, 30, 38, 39 and 40.)

Things to try:

1. Two or three clients at once, with different `--window` values. One `sendto()` per
   price, and every client gets a copy. The publisher never knows how many listen.
2. `--drop 0.2`: every client reports the same gaps. The SMA keeps going: each price is
   absolute, so a missing one only makes the average skip a point. A feed of increments
   ("add 300 at 100.02") would be corrupted by a gap; that is why real feeds need snapshots.
3. `--count 0` publishes forever. Start a client late: its first packet is not seq 1, and
   it syncs on whatever arrives first. Kill the publisher: UDP sends no "closed"; the
   client gives up after `--idle-ms` (3 s) of silence.

## Portability notes

- **Loopback.** Both programs default to `--iface 127.0.0.1`, with `IP_MULTICAST_LOOP` on
  and TTL 1, so everything stays on your machine. This works out of the box on macOS.
  On Linux, if the client receives nothing, the loopback device may not have the
  MULTICAST flag: run `sudo ip link set lo multicast on`, or pass your machine's LAN
  address to both sides (`--iface 192.168.1.23`).
- **Group address.** `239.0.0.0/8` is the "administratively scoped" range: meant for
  private use, never routed on the internet.
- **Two clients on one port.** Every client binds port 30001. On Linux `SO_REUSEADDR`
  is enough for multicast. On macOS and BSD you also need `SO_REUSEPORT` (the client sets
  it there). Do not add `SO_REUSEPORT` on Linux for this: there it load-balances unicast
  datagrams across sockets.
- **Ports.** If 5555 or 30001 is taken, pass `--port` to both sides.

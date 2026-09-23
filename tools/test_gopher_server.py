#!/usr/bin/env python3
"""Instrumented gopher test server for isolating R-8.

Runs on this Mac (same LAN as the MEGA65), so we control exactly how the
response is segmented and can observe how much the client actually
ACKs -- which a packet capture would also show, but tcpdump needs root
here and this is more directly targeted anyway.

Key trick: a deliberately small SO_SNDBUF plus a non-blocking socket
means the kernel stops accepting our writes once unacknowledged data
fills the send buffer + the peer's advertised window. So "the point at
which send() stalls" is a direct read on "how much the MEGA65 actually
acknowledged" -- no root required.

Every line is exactly 64 bytes and numbered, so the truncation point in
the client's display maps to an exact byte offset.

Usage:
  test_gopher_server.py [total_lines] [chunk_lines] [delay_s] [port]

  total_lines  how many 64-byte lines to send   (default 88 = 5632 bytes,
                                                 ~= floodgap's 5639)
  chunk_lines  lines per write() burst          (default 88 = one burst)
  delay_s      pause between bursts             (default 0.0)
"""
import errno
import select
import socket
import sys
import time

PORT_DEFAULT = 7070
LINE_LEN = 64


def make_line(n):
    text = "LINE {:04d} ".format(n).ljust(47)[:47]
    line = "i{}\t\terror.host\t1\r\n".format(text).encode()
    assert len(line) == LINE_LEN, len(line)
    return line


def main():
    total_lines = int(sys.argv[1]) if len(sys.argv) > 1 else 88
    chunk_lines = int(sys.argv[2]) if len(sys.argv) > 2 else 88
    delay = float(sys.argv[3]) if len(sys.argv) > 3 else 0.0
    port = int(sys.argv[4]) if len(sys.argv) > 4 else PORT_DEFAULT

    payload = b"".join(make_line(i) for i in range(1, total_lines + 1)) + b".\r\n"

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", port))
    srv.listen(1)
    print("[srv] listening on 0.0.0.0:{}  total={}B ({} lines) chunk={} lines delay={}s".format(
        port, len(payload), total_lines, chunk_lines, delay), flush=True)

    conn, addr = srv.accept()
    t0 = time.time()
    print("[srv] {:6.2f}s connection from {}".format(0.0, addr), flush=True)

    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    # Small send buffer: makes back-pressure from the peer observable fast.
    conn.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4096)

    conn.settimeout(10)
    try:
        sel = conn.recv(256)
        print("[srv] {:6.2f}s selector={!r}".format(time.time() - t0, sel), flush=True)
    except Exception as e:
        print("[srv] {:6.2f}s selector recv failed: {}".format(time.time() - t0, e), flush=True)

    conn.setblocking(False)
    sent = 0
    chunk_bytes = chunk_lines * LINE_LEN
    stalled_reported = False

    while sent < len(payload):
        target = min(sent + chunk_bytes, len(payload))
        while sent < target:
            try:
                r, w, x = select.select([], [conn], [], 2.0)
                if not w:
                    if not stalled_reported:
                        print("[srv] {:6.2f}s *** SEND STALLED at {} bytes "
                              "(peer not ACKing / window closed)".format(time.time() - t0, sent), flush=True)
                        stalled_reported = True
                    continue
                n = conn.send(payload[sent:target])
                if n > 0:
                    sent += n
                    print("[srv] {:6.2f}s sent -> cumulative {} bytes".format(time.time() - t0, sent), flush=True)
                    stalled_reported = False
            except socket.error as e:
                if e.errno in (errno.EAGAIN, errno.EWOULDBLOCK):
                    continue
                print("[srv] {:6.2f}s SOCKET ERROR after {} bytes: {}".format(
                    time.time() - t0, sent, e), flush=True)
                conn.close()
                srv.close()
                return
        if delay:
            time.sleep(delay)

    print("[srv] {:6.2f}s all {} bytes handed to kernel".format(time.time() - t0, sent), flush=True)

    # Hold the connection open and watch for errors/close, so we can tell
    # "server finished cleanly" apart from "client stopped responding".
    deadline = time.time() + 12
    while time.time() < deadline:
        try:
            r, w, x = select.select([conn], [], [conn], 1.0)
            if r:
                d = conn.recv(256)
                if not d:
                    print("[srv] {:6.2f}s peer closed (clean FIN)".format(time.time() - t0), flush=True)
                    break
                print("[srv] {:6.2f}s peer sent {!r}".format(time.time() - t0, d), flush=True)
            if x:
                print("[srv] {:6.2f}s socket exception".format(time.time() - t0), flush=True)
                break
        except socket.error as e:
            print("[srv] {:6.2f}s error while waiting: {}".format(time.time() - t0, e), flush=True)
            break

    print("[srv] {:6.2f}s closing".format(time.time() - t0), flush=True)
    conn.close()
    srv.close()


if __name__ == "__main__":
    main()

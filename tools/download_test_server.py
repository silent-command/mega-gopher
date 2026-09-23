#!/usr/bin/env python3
"""Serves a type-9 binary so the client's download path can be verified
byte-for-byte.

The payload is deterministic (a counter, not random) so the file written to
the .d81 can be checked against a regenerated copy without transferring a
reference file separately.

Usage: download_test_server.py [port] [size_bytes]
"""
import socket
import sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 7071
SIZE = int(sys.argv[2]) if len(sys.argv) > 2 else 32768
def lan_ip():
    """The address to advertise in menu lines.

    socket.gethostbyname(gethostname()) is unreliable here -- it raised
    gaierror mid-session when mDNS was unhappy, killing the server at
    startup. Opening a UDP socket toward a routable address reveals the
    interface the kernel would use without sending anything.
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.0.2.1", 9))  # TEST-NET-1: never actually routed
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


HOST_ADV = sys.argv[3] if len(sys.argv) > 3 else lan_ip()


def payload(n):
    # byte i = (i*7 + (i>>8)) & 0xff -- varies within and across pages, and
    # contains 0x00, 0x0a, 0x0d and 0x2e so the binary path is proven not to
    # be doing any line or terminator handling.
    return bytes(((i * 7 + (i >> 8)) & 0xff) for i in range(n))


def menu():
    lines = [
        b"iBinary download test\t\terror.host\t1\r\n",
        ("9Binary %d bytes\t/bin\t%s\t%d\r\n" % (SIZE, HOST_ADV, PORT)).encode(),
        ("9Binary 1KB\t/small\t%s\t%d\r\n" % (HOST_ADV, PORT)).encode(),
        b".\r\n",
    ]
    return b"".join(lines)


def main():
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", PORT))
    s.listen(8)
    print("download test server on %s:%d  (%d-byte payload)" % (HOST_ADV, PORT, SIZE))
    sys.stdout.flush()
    while True:
        c, addr = s.accept()
        try:
            sel = c.recv(512).split(b"\r\n")[0].split(b"\t")[0].decode("latin1")
            print("  %s -> %r" % (addr[0], sel))
            sys.stdout.flush()
            if sel == "/bin":
                c.sendall(payload(SIZE))
            elif sel == "/small":
                c.sendall(payload(1024))
            else:
                c.sendall(menu())
        except OSError:
            pass
        finally:
            c.close()


if __name__ == "__main__":
    main()

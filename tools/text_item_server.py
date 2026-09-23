#!/usr/bin/env python3
"""Minimal two-selector gopher server for exercising the text viewer.

The client's type-0 path (fetch_text) is the last thing that needs
verifying on hardware, and public servers keep rate-limiting us mid-test
(R-8c). This serves everything locally and stays up across connections,
which the single-shot test_gopher_server.py does not:

  selector ""      -> a menu containing one type-0 [TXT] item
  selector "/text" -> a plain text file terminated by a lone "."

Usage: text_item_server.py [port]        (default 7070)
"""
import os
import pathlib
import socket
import sys
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 7070
HOST_IP = "192.168.1.232"

MENU = (
    "iA menu with a text item below\t\terror.host\t1\r\n"
    "iTHIS INFO LINE IS DELIBERATELY MUCH LONGER THAN EIGHTY COLUMNS SO THAT CLIPPING CAN BE CHECKED ON A REAL SCREEN\t\terror.host\t1\r\n"
    "i\t\terror.host\t1\r\n"
    "0Sample text file\t/text\t{ip}\t{port}\r\n"
    "iEnd of menu\t\terror.host\t1\r\n"
    ".\r\n"
).format(ip=HOST_IP, port=PORT).encode()

# Serves a byte-exact capture of a real gopherpedia article, so the client
# sees precisely what it sees over the internet -- the local fixtures I
# invented did not reproduce the reported corruption, so replay the real
# thing instead of guessing at what makes it different.
_ARTICLE = (pathlib.Path(__file__).parent / "fixtures"
            / "long-article.txt").read_bytes()
TEXT = _ARTICLE if _ARTICLE.rstrip().endswith(b".") else _ARTICLE + b".\r\n"

def main():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", PORT))
    srv.listen(4)
    print("[srv] listening on 0.0.0.0:{}  menu={}B text={}B".format(
        PORT, len(MENU), len(TEXT)), flush=True)

    while True:
        conn, addr = srv.accept()
        conn.settimeout(10)
        try:
            sel = b""
            while not sel.endswith(b"\n"):
                chunk = conn.recv(64)
                if not chunk:
                    break
                sel += chunk
            selector = sel.strip().decode(errors="replace")
            body = TEXT if "text" in selector else MENU
            print("[srv] {} selector={!r} -> {} bytes".format(
                time.strftime("%H:%M:%S"), selector, len(body)), flush=True)
            conn.sendall(body)
        except Exception as e:
            print("[srv] error: {}".format(e), flush=True)
        finally:
            conn.close()


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Hostile gopher server for testing the MEGA65 client's edge cases.

Serves a menu at "/" linking to one case per selector, so the whole suite
can be walked from the client itself. Each case targets a specific limit
or malformed-input path in the parser rather than the network layer --
tools/test_gopher_server.py already covers segmentation and windowing.

Client limits these are aimed at (src/gopher_items.h, src/gopher_text.h,
src/gopher.c):
  MAX_ITEMS 200, ITEM_DISPLAY_LEN 71, ITEM_SELECTOR_LEN 47,
  ITEM_HOST_LEN 31, MAX_TEXT_LINES_ATTIC 4000, TEXT_LINE_LEN 79,
  linebuf[160]

Usage: adversarial_server.py [port]   (default 7070)
"""
import socket
import sys
import threading
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 7070
HOST_ADV = "192.168.1.232"

CASES = []


def case(sel, title):
    def deco(fn):
        CASES.append((sel, title, fn))
        return fn
    return deco


def menu_line(t, display, sel, host=HOST_ADV, port=PORT):
    return "{}{}\t{}\t{}\t{}\r\n".format(t, display, sel, host, port).encode()


# --- empty / truncated responses ---------------------------------------

@case("/empty", "Empty response, clean close")
def c_empty(c):
    pass


@case("/noterm", "Menu with no terminating dot")
def c_noterm(c):
    for i in range(5):
        c.sendall(menu_line("i", "line {} of a menu that never ends".format(i), ""))
    # deliberately no "."


@case("/halfline", "Closes mid-line")
def c_halfline(c):
    c.sendall(b"1A complete line\t/empty\t" + HOST_ADV.encode() + b"\t70\r\n")
    c.sendall(b"1This line stops abr")


@case("/justdot", "Only the terminator")
def c_justdot(c):
    c.sendall(b".\r\n")


# --- malformed structure -----------------------------------------------

@case("/notabs", "Lines with no tabs at all")
def c_notabs(c):
    for i in range(6):
        c.sendall("iThis line has no tab separators at all {}\r\n".format(i).encode())
    c.sendall(b".\r\n")


@case("/emptyfields", "Empty selector/host/port fields")
def c_emptyfields(c):
    c.sendall(b"1Empty selector and host\t\t\t\r\n")
    c.sendall(b"1Missing port\t/empty\t" + HOST_ADV.encode() + b"\t\r\n")
    c.sendall(b"1Nonnumeric port\t/empty\t" + HOST_ADV.encode() + b"\tabc\r\n")
    c.sendall(b".\r\n")


@case("/barelf", "Bare LF line endings, no CR")
def c_barelf(c):
    for i in range(5):
        c.sendall("iBare LF line {}\t\t\t\n".format(i).encode())
    c.sendall(b".\n")


@case("/badtypes", "Unsupported and unknown item types")
def c_badtypes(c):
    for t in "9gIhsMc;+2":
        c.sendall(menu_line(t, "Item of type '{}'".format(t), "/empty"))
    c.sendall(menu_line("1", "A normal menu item (should still work)", "/small"))
    c.sendall(b".\r\n")


# --- oversize fields ----------------------------------------------------

@case("/longdisplay", "Display text far over ITEM_DISPLAY_LEN (71)")
def c_longdisplay(c):
    c.sendall(menu_line("1", "D" * 300, "/small"))
    c.sendall(menu_line("i", "X" * 500, ""))
    c.sendall(b".\r\n")


@case("/longselector", "Selector far over ITEM_SELECTOR_LEN (47)")
def c_longselector(c):
    c.sendall(menu_line("1", "Selector is 300 chars", "/" + "s" * 300))
    c.sendall(b".\r\n")


@case("/longhost", "Host far over ITEM_HOST_LEN (31)")
def c_longhost(c):
    c.sendall("1Host is 200 chars\t/empty\t{}\t70\r\n".format("h" * 200).encode())
    c.sendall(b".\r\n")


@case("/hugeline", "Single line of 4000 bytes, over linebuf[160]")
def c_hugeline(c):
    c.sendall(b"i" + b"H" * 4000 + b"\t\t\t\r\n")
    c.sendall(menu_line("1", "Survived the huge line?", "/small"))
    c.sendall(b".\r\n")


@case("/manyitems", "300 items, over MAX_ITEMS (200)")
def c_manyitems(c):
    for i in range(300):
        c.sendall(menu_line("1", "Item number {:03d}".format(i), "/small"))
    c.sendall(b".\r\n")


# --- text files ---------------------------------------------------------

@case("/small", "Small text file")
def c_small(c):
    c.sendall(b"A short text file.\r\nSecond line.\r\nThird line.\r\n.\r\n")


@case("/bigtext", "5000-line text file, over MAX_TEXT_LINES_ATTIC (4000)")
def c_bigtext(c):
    for i in range(5000):
        c.sendall("Line {:05d} of a very long document.\r\n".format(i).encode())
    c.sendall(b".\r\n")


@case("/widetext", "Text lines of 400 chars, over TEXT_LINE_LEN (79)")
def c_widetext(c):
    for i in range(20):
        c.sendall(("W{:02d} ".format(i) + "x" * 400 + "\r\n").encode())
    c.sendall(b".\r\n")


@case("/utf8", "UTF-8 multibyte and control characters")
def c_utf8(c):
    c.sendall("Accents: aeiou -> áéíóú\r\n".encode())
    c.sendall("CJK: 你好世界\r\n".encode())
    c.sendall("Emoji: \U0001f600\U0001f680\r\n".encode())
    c.sendall("Box: ─│┌┐└┘\r\n".encode())
    c.sendall(b"Control: bell\x07 tab\x09 vt\x0b nul\x00 done\r\n")
    c.sendall(b".\r\n")


# --- timing -------------------------------------------------------------

@case("/slowdrip", "One byte every 0.5s for 30s")
def c_slowdrip(c):
    msg = b"This text arrives one byte at a time to test the idle timeout.\r\n"
    for ch in msg:
        c.sendall(bytes([ch]))
        time.sleep(0.5)
    c.sendall(b".\r\n")


@case("/stall", "Accepts, sends nothing, holds open 60s")
def c_stall(c):
    time.sleep(60)


@case("/latemenu", "10s of silence, then a normal menu")
def c_latemenu(c):
    time.sleep(10)
    c.sendall(menu_line("1", "Arrived after a long silence", "/small"))
    c.sendall(b".\r\n")


# --- root menu ----------------------------------------------------------

def root(c):
    c.sendall(menu_line("i", "MEGA65 gopher client - adversarial test suite", ""))
    c.sendall(menu_line("i", "", ""))
    for sel, title, fn in CASES:
        t = "0" if sel in ("/small", "/bigtext", "/widetext", "/utf8",
                           "/slowdrip", "/hugeline") else "1"
        c.sendall(menu_line(t, "{}  [{}]".format(title, sel), sel))
    c.sendall(b".\r\n")


HANDLERS = {sel: fn for sel, title, fn in CASES}


def serve(c, addr):
    try:
        c.settimeout(20)
        req = b""
        while not req.endswith(b"\n") and len(req) < 512:
            chunk = c.recv(1)
            if not chunk:
                break
            req += chunk
        sel = req.decode("latin-1").strip()
        sel = sel.split("\t")[0]
        print("  {} -> {!r}".format(addr[0], sel), flush=True)
        handler = HANDLERS.get(sel)
        if sel in ("", "/"):
            root(c)
        elif handler:
            handler(c)
        else:
            c.sendall("3Unknown selector: {}\t\terror.host\t1\r\n".format(sel).encode())
            c.sendall(b".\r\n")
    except Exception as e:
        print("  error: {}".format(e), flush=True)
    finally:
        try:
            c.close()
        except Exception:
            pass


def main():
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", PORT))
    s.listen(8)
    print("adversarial gopher server on port {} ({} cases)".format(PORT, len(CASES)),
          flush=True)
    for sel, title, fn in CASES:
        print("   {:16s} {}".format(sel, title), flush=True)
    while True:
        c, addr = s.accept()
        threading.Thread(target=serve, args=(c, addr), daemon=True).start()


if __name__ == "__main__":
    main()

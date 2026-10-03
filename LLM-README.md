# MEGA65 Gopher Client

A native gopher client (RFC 1436) for the [MEGA65](https://mega65.org),
written in C for llvm-mos, running in native MEGA65 mode on
[mega-net](../mega-net), a TCP/IP stack written for the MEGA65 alongside
this client.

![version 0.3.2](https://img.shields.io/badge/version-0.3.2-blue)

## What it does

- Browses gopher menus over the MEGA65's built-in 45E100 Ethernet, with
  DHCP and DNS
- Q, typed at the start screen's prompt or pressed in a menu, leaves for BASIC's READY prompt with the disk still mounted, so `RUN "GOPHER"` starts it again
- 80-column display, keyboard navigation, paginated menus and text
- Search, and a back-stack with instant redraw from cached menus
- Saves articles to a mounted `.d81` as CBM SEQ files
- Downloads binary items (types 9 and 5) and images (`g`, `I`) to disk,
  choosing PRG or SEQ
- Bookmarks, persisted on the program disk

## Running

Copy `bin/GOPHER.D81` to your SD card, then from BASIC 65:

```
MOUNT "GOPHER.D81"
RUN "GOPHER"
```

The disk carries two files: `GOPHER`, the client, and `MEGANET`, the
network stack, which the client loads into bank 4 at startup. They ship
together; neither is useful without the other.

## Keys

On the start screen, type the number of a bookmark and press RETURN to
open it, `1` to type a gopher address, `D` and a number to delete a
bookmark, or `Q` or RUN/STOP to leave for BASIC. The same keys mean the
same in the FTP and SSH clients: RUN/STOP backs out one level at a time
and quits from the top, HELP is the way to the start screen, plain
letters act on the content.

In a menu:

| Key | Does |
|---|---|
| cursor up/down | move the selection; the page turns at the edge |
| RETURN | open the selected item: a menu, a text file to read, or a download |
| G | get the selected item to disk without viewing it: a text, a binary or an image |
| HOME | the start page; there, the start screen |
| RUN/STOP | back to the previous menu; from the first, the start screen |
| HELP | the start screen, from anywhere |
| A | go to a gopher address typed in |
| MEGA+M, or M | bookmark this page, or remove the bookmark if it is one |
| MEGA+F, or F | next text color |
| MEGA+B, or B | next background and border color (the first press: black) |

In a text file:

| Key | Does |
|---|---|
| cursor up/down | scroll a line |
| RETURN | next page |
| HOME | back to the top |
| G | get: save the file to disk |
| RUN/STOP | back to the menu |
| HELP | the start screen |

## Building

Needs [llvm-mos](https://llvm-mos.org) (`mos-mega65-clang`), `c1541` from
VICE, Python 3, and two sibling checkouts next to this one:

| Path | What for |
|---|---|
| `../mega-net` | the TCP/IP stack: its image ships on the disk as `MEGANET`, and its `meganet.h` is the client's network interface |
| `../mega65-libc` | the C library (conio, DMA copies, memory access) |

```
./build-native.sh
```

The script builds mega-net first if its image is missing, compiles the
client, and writes `bin/GOPHER.D81`. It is a shell script, so it runs on
macOS and Linux; mega-net's own build is Python and runs anywhere.

## How the client uses the stack

mega-net is a banked binary with a jump-table ABI: the client copies a
92-byte trampoline to `$1600` (the page the stack reserves), loads the
image into bank 4, and calls entries through `meganet.h` — DHCP, DNS, one
TCP connection, all polled from the client's own loop with every wait
bounded by the frame counter. `src/gopher_net.c` is the whole of the
client's side of that; `src/gopher_boot.c` does the loading. The stack
keeps its socket buffers in bank 5 and its interrupt vectors at the top
of bank 4; the client's memory map in `src/gopher_scratch.h` stays clear
of both. mega-net's `docs/ABI.md` is the reference.

## Documentation

`REQUIREMENTS.md` is the design record and a numbered log of every
hardware finding and dead end: driving the F011 floppy controller
directly, the VIC-IV 80-column setup, several llvm-mos code generation
issues, and the port from the stack this client started on to mega-net
(entries 2.49 onward). `PLATFORM-NOTES.md` is the distilled version. If
you are writing anything for the MEGA65 that touches disks, the network
or the screen, those two files are the parts of this repository most
likely to save you time. `attic/` holds material from earlier stages,
including the patches and bug report for the stack this client no longer
uses, kept for the record.

## Status

Works, against public gopher servers and against the adversarial test
servers in `tools/`. Saving to a real floppy drive is deliberately
refused (REQUIREMENTS.md 2.48). Verified on MEGA65 hardware; nothing
here has been run in an emulator.

## License

0BSD, see `LICENSE`. mega-net is a separate project under the same
license; mega65-libc is under its own.

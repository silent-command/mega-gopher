# mega-gopher

A gopher browser for the MEGA65 on mega-net: menus, text files with a
pager, downloads to disk through attic RAM, bookmarks in `GOPHER.CFG`,
UTF-8 folded to the machine's glyphs. Native mode, llvm-mos, 80x25.
Version in `src/gopher.c` (`GOPHER_VERSION`). 0BSD. Releases carry
`bin/GOPHER.D81`.

This is the oldest of the three clients: the platform modules the
others share (screen, F011, CBM DOS, boot, exit, the font copy) began
here and were copied out as `m65_*` into `../mega-ftp/src/platform/`
and `../mega-ssh/src/platform/`. A fix to one of them belongs in all three.

## Read first

1. This file.
2. `../mega-net/docs/PLATFORM.md`: the machine, the family's memory
   map, the traps, the tools, the test driver.
3. `PLATFORM-NOTES.md` here: the longer notes this project wrote as it
   went, including the dead ends in section 9. Do not re-derive those.
4. `REQUIREMENTS.md`: decisions in section 2, the findings numbered
   2.x and R-x (this project's numbering predates the others').

## Commands

```
sh build-native.sh        the client and bin/GOPHER.D81 (mega65-libc into build/libc, mega-net from ../mega-net)
python3 tools/test_gopher_server.py     a local menu and text fixture; public servers rate-limit, use this
python3 tools/adversarial_server.py     the cases that broke earlier stacks
python3 tools/download_test_server.py   large items for the download path
```

There is no host suite here; the client is tested on the machine
against the fixture servers. The build script is a shell script; if it
is ever touched for another host, it becomes a `build.py` like the
other three.

On the machine: the disks live in `net-tools` on the card and **nothing
mounts from a subdirectory** (2026-09-23), so start it from the
Freezer's disk-image browser, which does walk directories. With the
driver, `boot_prg` stages a copy at the root for the run; call
`unstage_d81 gopher.d81` when finished:

```
source ../mega-net/tools/m65lib.sh
put_d81 bin/GOPHER.D81 GOPHER.D81
boot_prg gopher.d81 gopher 'bookmark'    the start screen
type_line '3'; wait_for 'floodgap' 60    bookmark 3 opens a menu
type_keys '~C'                           RUN/STOP: back a menu, then the start screen, then BASIC (or q there)
wait_for 'tem 15 of' 60                  the page is drawn only when the item counter shows; keys typed before are flushed
```

## What is mine in memory

Bank 1 `$12000-$19D3F`: the parsed items, 200 of 160 bytes (2.51; they
sat inside mega-net's socket buffers until 2026-09-06). Bank 1
`$11000-$117FF`: the font copy. Attic RAM: the menu cache at
`$8060000`, history at `$80A0000`, downloads at `$8100000`; attic RAM
is optional hardware, probed by readback at start. The screen at
`$0800`. Everything else the program touches is in its own `$2001-$D000`
or in `PLATFORM.md`'s table.

## Rules

- Every wait is bounded on `$D7FA`; poll mega-net from every loop; the
  peer closing is not the end of the data: drain, then close.
- Keep text ASCII until draw time; decode UTF-8 before storage so one
  character is one byte; sanitise every byte from the network.
- Build strings with explicit running lengths, never `strcat` in a
  loop, and never a static write position: both miscompiled here
  (`gopher_config.c` says how).
- Downloads are buffered whole in attic RAM and written to disk after,
  never streamed to the F011 while the stack pumps.
- The exit is a software reset from the stub at `$1FB0` with the hot
  registers on, the font pointer restored in full, and the bank byte
  once more inside the stub (`gopher_exit.S`, 0.3.2, ssh 5.25).
- A screenshot proves screen memory; for the picture, the PNG.
- Record every hardware finding in `REQUIREMENTS.md`, numbered.
- Commits are local until the user asks for a push or a release. A
  release bumps `GOPHER_VERSION`, rebuilds, tags `vX.Y.Z`, and attaches
  `GOPHER.D81` with a short note ending in how to run it.

## Not to reopen

Screen codes written directly, not conio's output (it crashes and
costs 765 bytes). The KERNAL's file writes (dead end, section 9). Raw
SD sector I/O (dead end). One socket at a time. The trampoline page at
`$1600`. TERM handling is the SSH client's; this client draws text.

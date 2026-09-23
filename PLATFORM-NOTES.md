# MEGA65 + llvm-mos: platform notes

Hard-won notes from building a native-mode MEGA65 network application in C.
Everything here was measured on real hardware, not inferred from
documentation — several entries exist because the obvious assumption was
wrong and cost hours.

Extracted from the gopher client's REQUIREMENTS.md, which keeps the
project-specific design decisions and the full narrative of how each of
these was found.

---

## 1. Toolchain

**Use llvm-mos.** CC65 cannot produce a native-mode MEGA65 program: it has
no MEGA65 target and mega65-libc ships no linker config for one. A CC65
build runs in C64 mode (`$0801` load address, C64 BASIC stub), i.e. it
requires `GO64`.

- `mos-mega65-clang` produces a native PRG at load address **`$2001`**.
- `mega65-libc` builds for it directly — its `CMakeLists.txt` already sets
  `LLVM_MOS_PLATFORM mega65`:
  ```sh
  cmake -DCMAKE_PREFIX_PATH=$HOME/llvm-mos -B build -S <mega65-libc>
  cmake --build build
  ```
- Calypsi also has a real MEGA65 target (`--target MEGA65 --core 45gs02`,
  plus `mega65-banked.scm` for banking and `--string-literals` for
  encoding control). It was not chosen because mega65-libc has no Calypsi
  support, but it is the stronger option if you need real 45GS02 codegen
  or >64KB banking.

**CC65's charmap is a trap worth knowing about** even if you never use it:
it silently rewrites string literals into PETSCII at compile time. That
single behaviour caused four separate bugs in this project. llvm-mos does
no such rewriting — literals stay plain ASCII, which is simpler but means
*you* must translate to screen codes.

---

## 2. The runtime environment llvm-mos hands you

`unmap-basic.S` (`.init.010`) runs **before `main`**:

```asm
sei                    ; interrupts OFF for the whole program
ldx #$3e → $01         ; BASIC ROM unmapped, KERNAL still mapped
ldx #$44 → $d030       ; MEGA65 ROM banking
```

Consequences that bite:

- **Interrupts are masked for your entire program.** Anything needing them
  (KERNAL disk I/O, the KERNAL keyboard scan) will not work as-is.
- **`cli` alone crashes the machine**, because the interrupt handler runs
  with BASIC unmapped.
- **`$d030` carries VIC-III/IV configuration as well as ROM banking.**
  Changing it to reach the KERNAL disturbs the display.

The teardown (`.fini.990`) restores `$01 = $3f`, `$d030 = $64`, `cli` —
**those are the values to use** if you need a KERNAL-friendly environment
mid-program. They are not guesses; they are what the runtime itself
considers safe to hand back.

**Returning from `main()` exits cleanly to BASIC**: `__after_main` jumps to
the fini chain, which restores the above and `rts`es to whatever ran the
program. (Caveat: in this project BASIC's *editor* was left unusable
afterwards — every typed line came back as `break`. Cause not identified;
clearing `$91`, `$c6`, `$d610` and `$dc00` did not help.)

---

## 3. Memory map — what is actually usable

Measured by write-then-readback probes, which is the only reliable way:

| region | status |
| --- | --- |
| `$2001`–`$D000` | program + soft stack (stack at `$D000`, grows down) |
| `$0800` | **screen** in native mode (80×25 = 2000 bytes) |
| `$0334`–`$03FF` | free for a program that owns the machine; **under BASIC 65 it is the KERNAL's far-call code** (mega-net REQUIREMENTS.md 5.10) |
| `$1600`–`$16FF` | mega-net's trampoline, reserved |
| bank 1 `$10000` | free (BASIC 65 keeps its variables there when it is running) |
| **banks 2–3 `$20000`–`$3FFFF`** | **NOT usable — this is the 128KB C65 ROM.** A probe writing every 1KB had 126 of 128 samples fail to stick |
| bank 4 `$42000`–`$4BFFF` | mega-net's image; `$4FFF0`–`$4FFFF` its interrupt vectors during a call |
| bank 5 `$50000`–`$59FFF` | mega-net's socket buffers; the rest of bank 5 is free |
| banks 6+ | do not exist (384KB machine) |
| **attic RAM `$8000000`** | **works for CPU access and one-off DMA** — 128 samples, zero mismatches. Optional hardware, so probe before relying on it. **DMA reads shortly after DMA writes nearby return stale data** (mega-net 5.13), so it is not a place for buffers that DMA fills and drains |

Attic RAM is the answer when you need real space for data you fill once
and read later; this client downloads into it. Probe it at startup by
readback and fall back gracefully.

---

## 4. DMA hazards

**A zero-length DMA copies 65536 bytes.** `lcopy`/`lfill` pass the count
straight to DMAgic with no zero check:

```c
void lcopy(uint32_t src, uint32_t dst, size_t count) { ... dmalist.count = count; ... }
```

So any `lcopy`/`lfill` whose length is caller-controlled needs a zero
guard. This is not hypothetical — it destroyed 64KB of memory and hung the
machine in this project, and the same hazard is **unguarded inside
mega65-libc's own `cputsxy()`**, which does
`lcopy(s, screen + offset, strlen(s))`. Drawing an empty string is enough
to trigger it.

Symptom to recognise: screen fills with recognisable data from elsewhere in
memory, machine unresponsive.

---

## 5. Display and text

- **Do not use `cputsxy()`** — it crashed under llvm-mos here. Use
  `gotoxy()` then `cputs()`.
- **Do not use `sprintf("%s")`** — crashes. A three-argument `"%u-%u of %u"`
  also failed while a two-argument one worked. Hand-roll number formatting;
  it also drops ~3.4KB of printf from the binary.
- **Screen codes are not ASCII.** `A`–`Z` map to `$41`–`$5A`, but `a`–`z`
  map to `1`–`26`. Translate once, at draw time, and keep text as plain
  ASCII internally — translating early creates a "literals are magic,
  network data is not" split that causes persistent bugs.
- **Sanitise bytes from the network** before drawing. Anything outside
  printable ASCII should be replaced, and UTF-8 should be decoded *before*
  storage so one character is one byte and length limits stay honest.
- **Colours**: `$d020` (border) and `$d021` (background) are whatever the
  ROM left — inherit them rather than imposing. `$0286` holds the ROM's
  current text colour. conio hard-defaults to white with no getter.
- `clrscr()` does not reliably home the cursor — call `gotoxy(0, 0)`.

---

## 6. Networking with mega-net

The client talks to [mega-net](../mega-net), a stack written for the
MEGA65 and for exactly this kind of program: a banked image at `$42000`
with a jump-table ABI, called through a 92-byte trampoline at `$1600`
that maps bank 4 over `$2000`–`$BFFF` for the duration of a call and
puts the caller's map and interrupt flag back afterwards. Nothing blocks;
the client polls from its own loop, paced on the `$D7FA` frame counter.
`src/gopher_net.c` is the client's whole use of it; mega-net's
`docs/ABI.md` is the reference, and its REQUIREMENTS.md the record of how
each part was proven on hardware.

**What the client learned on its first stack, and the new one was
designed around:**

- **"Connection closed" ≠ "all data received."** Bytes already in the
  receive ring must still be drained; breaking out on the close event
  silently truncates responses. mega-net flags EOF separately from the
  state and keeps the ring readable after the peer closes.
- **Close before connecting again.** Servers close their end after
  responding; the previous connection sits in `CLOSE_WAIT` until the
  client closes too. The client aborts the old connection before opening
  the next.
- **Send a request as one segment.** Byte-by-byte sends got the client
  RST by real servers. mega-net sends whatever is queued as one segment.
- **Advertise the window by its right edge.** A size-based window update
  stalled transfers at exactly 1,024 bytes; mega-net's is edge-based, and
  the 5,635-byte case that used to stall is a test it passes.
- **Cache DNS.** Browsing stays within one host; the client keeps four
  entries and only invalidates on a failed connection.

**Two things the stack, not the client, has to get right, both found
while the client was being ported:** the MEGA65 I/O personality must be
asserted on every entry, because the client's screen and disk code select
their own; and the DMA list address registers must be restored after
every job, because BASIC 65 triggers its own jobs by writing `$D700`
alone. Both are the stack's job now (mega-net 5.9, 5.10).

---

## 7. Debugging techniques that worked

**Border-colour bisection.** Set `$d020` to a distinct value at each stage;
whatever colour the screen ends on tells you how far execution got. This
needs no working screen, no serial link, and no functioning output path —
it found every hard bug in this project, including ones where the display
itself was the casualty.

```c
#define BORDER (*(volatile unsigned char *)0xd020)
BORDER = 1;  /* reached here */
```

**Read device state directly.** `lpeek()` at `$40000 + <offset from
64tass -l labels>` reads the stack's internal variables live, with no need to
modify or instrument it.

**Sample before the call that clears the evidence.** `ETH_STATUS_POLL`
clears `TCP_EVENT_FLAG` when it reports it — reading `ETH_RX_TCP_FLAGS`
*before* pumping is what caught the server's RST.

**Use byte-exact captures, not invented fixtures.** Three hand-written test
files failed to reproduce a corruption bug; a `nc`-captured real server
response reproduced it immediately, because it contained blank lines the
invented ones did not. Keep captures in the repo.

**Bisect by disabling halves.** Store-but-don't-draw, draw-but-don't-store,
etc. Note the trap: disabling "drawing" also disabled the read path, which
produced a wrong conclusion until the split was done properly.

---

**Measure static memory from the linker map, never by reasoning.** Pass
`-Wl,-Map=out.map` and read `__bss_end`; the gap to the stack at `$d000`
is your real headroom:

```sh
grep ' __bss_end' out.map     # free = 0xd000 - that address
```

A constant that sets a struct field's width is spent once per instance of
that struct, everywhere. Widening one such constant by 33 bytes cost 1,717
bytes, not the 660 predicted from the obvious array — the other users of
the struct were missed. Diff the map before and after; the number is free
to obtain and the estimate is not trustworthy.

When headroom does get tight, the cheapest lever is usually the depth of
whatever array dominates (here, a navigation history), not the field width
that forced the growth.

---

**ROM banking collides with any program larger than ~32KB.** A native PRG
loads at `$2001`, so a 41KB program occupies `$2001-$c0f1` with its stack
below `$d000`. Both banking controls then overlap it:

| Setting | Maps ROM over | What is actually there |
|---|---|---|
| `$01 = $3f` | `$a000-$bfff` | your code |
| `$d030 = $64` | `$c000-$cfff` | your statics and your stack |

Neither is survivable while executing from those addresses. Worse, the
KERNAL **IRQ handler** needs that layout too, so a bare `cli` hangs a
large program instantly. That rules out KERNAL disk I/O, which needs
interrupts: with them masked, `OPEN` waits forever on a jiffy-driven
timeout. Measured on hardware in both configurations, and in a 2.7KB build
where the conflict does not exist.

Note llvm-mos already runs with `$01 = $3e` — LORAM 0, HIRAM 1, CHAREN 1 —
so the **KERNAL is already mapped at `$e000` and I/O at `$d000`**. Anything
reached through the `$ffxx` jump table needs no banking change at all; the
banking in most example code is cargo-culted from C64 practice.

**Interrupts need the C65 ROM at `$c000`.** A bare `cli` hangs with
`$d030 = $44` and survives with `$d030 = $64` -- the KERNAL IRQ handler
lives partly in that ROM. The `$0314` vector is not the problem; it points
at `$f9ec`, inside KERNAL ROM, which `$01 = $3e` already maps.

So KERNAL calls are possible from a large program, in an assembly-only
"window": `$d030 = $64`, `cli`, call, `sei`, restore. Inside the window
`$c000-$cfff` is hidden -- including the soft stack -- so the window must
touch no C variables and make no C calls. Stage arguments and data in low
memory first.

`.bss` straddles `$c000`, so a `static` buffer cannot be relied on to land
below it (one landed at `$ca6e`). Pin the staging area to a fixed address
instead. **`$1400-$15ff` was verified free**: a pattern survived DHCP,
a menu fetch and an article load untouched. The 80x25 screen is at
`$0800-$0fcf`; `$d060` reads as zero and is useless once `conioinit()`
disables the VIC-IV hot registers, so find the screen by looking for its
text in memory instead.

**Enabling interrupts reactivates the KERNAL screen editor**, which will
take the display back and can relocate it over your own buffers -- the
screen and the staging buffer ended up holding identical garbage. The fix
is a minimal `$0314` handler: advance the jiffy clock (`$a0-$a2`),
acknowledge CIA1 (`$dc0d`) and the VIC (`$d019`), pull Y/X/A and `RTI`.
24 bytes. Disk timeouts need the clock, not the editor. Save and restore
the original vector around it. With that in place `OPEN` returns
`ST = $00` and the display survives.

**F011 status registers** (chipset reference, and note they only appear at
`$d080` in C65/VIC-III/IV I/O mode):

```
$d082  DB7 BUSY  DB6 DRQ  DB5 EQ  DB4 RNF  DB3 CRC  DB2 LOST  DB1 PROT  DB0 TK0
$d083  DB7 RDREQ DB6 WTREQ DB5 RUN DB4 WGATE DB3 DISKIN DB2 INDEX DB1 IRQ DB0 DSKCHG
```

DISKIN is media sense, PROT is write protect.

**`m65 --memsave` does NOT read these registers correctly.** Measured with
a disk mounted: the running program reads `$d083 = $08` (DISKIN set) while
`--memsave` on the same address reports `$00`. The F011 registers only
appear at `$d080` in C65/VIC-III/IV I/O mode and the debug interface does
not see the same mapping. `--memsave` is reliable for RAM; for I/O
registers, have the program record the value into RAM and read that
instead. Getting this wrong cost a full debugging cycle chasing a
"no disk" fault on a machine that had a disk mounted.

**Check llvm-mos's zero-page range before calling any KERNAL routine.**
The link map reports the span; on this project it was `$22-$90`, and `$90`
is the KERNAL's `ST` I/O status byte. `$9a` (output device) and `$b7-$bf`
(filename, device, secondary, name pointer) sit just above. If the
compiler has allocated over those, C code and KERNAL I/O will corrupt each
other -- and the symptom is not a clean failure but a stall partway
through, varying between runs.

**`m65` halts the CPU to read memory.** Every `--memsave` and
`--screenshot` taken *during* a timing-sensitive operation is itself an
intervention. Worth ruling out before trusting any observation made mid-
operation -- though on this project it turned out not to be the cause.

**The 45E100 asserts interrupts nothing services.** `$d6e1` bit 7 (RXQEN)
and bit 6 (TXQEN) enable its RX/TX interrupts. After `ETH_INIT`, any `cli`
in a program that has not installed an ethernet-aware handler drops into a
storm: the controller re-asserts immediately and the CPU never leaves the
handler. The symptom is not a clean hang but severe starvation -- work
creeps forward a little, by a different amount each run. Clear those two
bits before enabling interrupts, read-modify-write (the low bits of
`$d6e1` are the RX queue advance controls and a blind store loses frames).

**F011 direct access works, and avoids every KERNAL problem above** -- no
interrupts, no banking, no ROM mapping. Verified by reading a mounted
`.d81`'s header sector and getting a valid 1581 header back. The chipset
reference's read sequence is correct and applies to SD-card disk images as
well as physical disks.

**But SPINUP never completes on a virtualised drive.** The documented
sequence says to write `$60` to `$d080`, `$20` (SPINUP) to `$d081`, and
wait for BUSY to clear. With a mounted image there is no motor: BUSY stays
set and `$d082` reads `$b0` (BUSY|EQ|RNF) -- which on real hardware means
failure. Do not gate reads on it, and do not test RNF until *after* the
transfer, or every read is rejected.

CBM-to-physical mapping for a D81: track = CBM track - 1, side = sector /
20, physical sector = (sector % 20) / 2 + 1, and the CBM 256-byte block is
one half of the 512-byte physical sector (`sector % 2`).

**Loading with `m65 -r` without a preceding reset leaves F011 unusable.**
Reads return garbage from the previous run's state. But a reset drops any
mounted disk image and restores the auto-mount -- so testing disk code
against a specific image means changing the machine's auto-mount, not
mounting by hand between runs.

**Drive select lives in `$d080` alongside motor and side**, and any
transfer routine that writes that register will clobber a drive selection
made outside it. Keep the selection inside the driver.

**F011 writes work.** Pre-fill the controller buffer through `$d087`,
open the SD write gate (`$57` to `$d680`, good for ~1ms, and `$4d` is the
separate master-boot-record gate that should never be used), then command
`$84` (`$80` write + `$04` write-precompensation). Verified by writing a
pattern, re-reading the sector from the medium, and confirming both the
pattern and that the neighbouring 256-byte half was untouched.

Because a CBM 256-byte block is half a 512-byte physical sector, writing
one block is always read-modify-write.

**Fixed-address buffers in low RAM beat `.bss` arrays.** Below a native
PRG's `$2001` load address there is usable RAM: `$1400-$15ff`,
`$1a00-$1bff` and `$1c00-$1fff` were each verified free by writing a
pattern and confirming it survived a full run. Moving thirteen buffers
there did not just free `.bss` -- constant addresses also generate much
better code, and `.text` fell by over 5KB.

Two traps when doing this. Do NOT alias buffers onto a `const` array (such
as an embedded payload that is dead after boot): writing through a `const`
object is undefined, LTO folds the reads back to the original contents,
and you get the payload painted on your screen. And converting an array to
a pointer silently changes `sizeof()` to 2 -- every bounded copy using it
truncates. Keep explicit length constants next to the addresses.

**Use the memory-mapped sector buffer, not `$d087`.** The DATA register
walks an internal pointer into the F011's 512-byte buffer, and nothing
resets that pointer before a write -- so a write issued after a read
begins part-way through and stores the data displaced. Map the buffer at
`$ffd6c00-$ffd6dff` instead (clear BUFSEL, bit 7 of `$d689`, so the floppy
buffer is visible rather than the SD card's) and DMA in and out of it.
Faster, shorter, and free of ordering hazards.

**Diagnosing a dead network:** sample `$d6e0` repeatedly. DRXD (bit 2)
reads the ethernet RX bits currently on the wire and DRXDV (bit 3) whether
they are valid, so on a live link those change constantly. Frozen across
tens of thousands of reads means no signal is arriving at all -- a cable
or switch problem, not a driver one. This separated a genuine link failure
from a suspected code regression in seconds.

**CBM DOS does not check for media at OPEN.** With no disk, `OPEN` returns
`ST = $00` and the first `CHROUT` that forces a sector write blocks
forever, beyond the reach of Run/Stop. Check for media yourself first.

**`$ba` (186) holds the device the program was loaded from.** Zero page,
so no banking games needed to read it — the traditional way to find "which
drive did I come from".

---

## 8. Tooling

- **`m65 -F`** reset, **`-4`** C64 mode, **`-@ file@addr`** inject,
  **`-r prg`** load+run, **`--screenshot=f.png`**, **`-t 'text~M'`** type
  (lowercase; `~M` Return, `~D`/`~U` cursor, `~C` Stop, `~H` Home).
- **`mega65_ftp`** pushes files to the SD card and mounts `.d81` images
  (`put`, `get`, `mount`, `del`, `rename`, `dir`). It does **not** build on
  arm64 Macs out of the box — the Makefile passes `-mno-sse3` and
  GNU-ld-style static flags. Compile it by hand without those.
- **`put` over an existing file silently does nothing.** It prints
  `Uploaded 0 bytes.` and exits successfully; the file on the card keeps
  its old contents *and its old timestamp*. There is no error. `del` the
  target first, then `put`. This burned a full debugging cycle: every
  "re-upload and retest" was testing a disk image from hours earlier, and
  the failure looked like a bug in the code being tested. **Always confirm
  with `dir` and check the timestamp**, never trust that a `put` landed.
- **A mounted `.d81` does not survive a reset** — you must re-mount. Worse,
  the hypervisor resolves a mount to sectors *once*: if you `del` and
  re-`put` the image, the machine keeps serving the old sectors under the
  same filename. A `dir` on the host and a directory read on the MEGA65 can
  therefore disagree completely. Re-mount by name after every upload.
- **Reliable native-mode test cycle** for a program that reads from the
  mounted disk at runtime (as this client now does, for `ETHBIN`):

      mega65_ftp -F -l PORT -c "del X.D81" -c "put X.D81 X.D81" -c "dir X*"
      m65 -l PORT -F -T 'mount "x.d81"'    # reset to native mode, re-mount
      m65 -l PORT -r prog.prg              # NO -F: a reset drops the mount

  The `-F` belongs on the mount step, not the run step. Reversing those two
  is what produces a program running against the wrong disk.
- **`m65 -t`/`-T` drop uppercase letters.** `-T 'MOUNT "SAVETEST.D81"'`
  arrives as `".81"` — punctuation and digits survive, letters do not, and
  BASIC answers `?SYNTAX ERROR`. Type in **lowercase**; BASIC 65 accepts it
  and echoes uppercase. Also allow a second or two after a reset before
  typing, or the first characters are swallowed by the boot sequence.
- **`m65 -r` after `m65 -@` injections** left the machine unable to start
  the program in native mode. Embedding payloads in the binary avoids this
  entirely (`tools/gen_payload.py` turns a binary into a C array).
- **Always `m65 -F` before `mega65_ftp`.** `mega65_ftp` switches the machine
  to C64 mode and installs a fast SD routine over low memory. Doing that
  while a native-mode program is running corrupts its display and usually
  wedges the machine: the `put` then produces no output at all and the
  upload silently does not happen. Reset to BASIC first, every time.
- **Reading the CPU registers.** `m65` has no register dump, but the serial
  monitor protocol does: send `r` and it returns PC, A/X/Y/Z, SP, MAP and
  the instruction at the PC. On macOS the port must be opened at 2000000
  baud with the `IOSSIOSPEED` ioctl (`0x80045402`) -- plain `termios` and
  `stty` both reject that rate with EINVAL. Sampling the PC repeatedly
  localises a hang to a few bytes, which `llvm-objdump -d --start-address`
  then maps to a function. This is the tool to reach for once
  instrumentation has eliminated the obvious candidates; it found a hang in
  `say_net_error` that no amount of inference had.
- **`m65 -S` (ASCII screenshot) lies about glyphs.** It maps screen codes
  back to ASCII, so a character stored as the wrong screen code still reads
  back as the character you intended. It reported a correct `@` and a
  correct `[DIR]` while the display showed a horizontal line and `+DIR|`.
  For any question about what is actually on screen, capture the PNG
  (`m65 -S<file>.png`) and look at it.
- **A spinning program is recoverable over serial; a dead monitor is not.**
  If `m65 -S` still returns a screenshot, the serial monitor is alive and
  `m65 -F` will reset the machine even though the program is wedged --
  verified against a client stuck in a network loop. Reach for `-F` before
  asking anyone to touch the hardware. Only when the monitor itself stops
  answering (typically after `mega65_ftp` has run over a live native-mode
  program) is a physical power-cycle actually required.
- **Detect a wedged machine early**: run one `m65` call with a short
  timeout, try `m65 -F`, and if that does not answer either, **ask the user
  to power-cycle**. Do not escalate timeouts
  or sit polling for screenshots that will never arrive — a serial timeout
  cannot recover a wedged machine, and the person at the keyboard can reset
  it in seconds.

---

## 9. Dead ends — do not re-derive these

- **Writing files via KERNAL/CBM DOS.** The sequence works in a standalone
  probe (verified by writing a file and reading the `.d81` back), but
  fails inside a real application, and `$d030` switching fights the
  80-column display. Three attempts, two hard crashes.
- **Raw SD sector I/O** (`mega65_sdcard_readsector`). Reports success but
  returns data that is not the MBR — no `55 aa` signature. Needs an
  initialisation sequence beyond `mega65_sdcard_open()`, and may conflict
  with the hypervisor holding the card for a mounted image.
- **mega65-libc's fileio is read-only** — `open`, `read512`, `close`,
  `closeall`, `gethyppoversion`. There is no write path. It reads the SD
  card's FAT, *not* the contents of a mounted `.d81`.
- **`fat32.c` exists in mega65-libc but exposes no API** (no `fat32.h`).
- **Public gopher servers rate-limit.** Both floodgap and gopherpedia
  refused connections after repeated hits during development. Run a local
  fixture server for iteration.

#!/bin/sh
# Build the gopher client as a native MEGA65 program (FR-0a) using
# llvm-mos, and the .d81 that carries it together with the mega-net
# network stack (FR-0b).
#
# Prerequisites:
#   - llvm-mos SDK in ~/llvm-mos (provides mos-mega65-clang)
#   - mega65-libc built for llvm-mos (see LIBC_BUILD below)
#   - the mega-net repository as a sibling (../mega-net), or MEGANET=path
set -e

ROOT=$(cd "$(dirname "$0")" && pwd)
LIBC_SRC="$ROOT/../mega65-libc"
LIBC_BUILD="${LIBC_BUILD:-$ROOT/build/libc}"
CLANG="$HOME/llvm-mos/bin/mos-mega65-clang"

# 1. mega65-libc for the llvm-mos mega65 platform (its CMakeLists already
#    sets LLVM_MOS_PLATFORM=mega65).
if [ ! -f "$LIBC_BUILD/src/libmega65libc.a" ]; then
  echo "building mega65-libc for llvm-mos..."
  cmake -DCMAKE_PREFIX_PATH="$HOME/llvm-mos" -B "$LIBC_BUILD" -S "$LIBC_SRC"
  cmake --build "$LIBC_BUILD"
fi

# 2. The network stack: mega-net, built in its own repository. Its image
#    ships on the .d81 as MEGANET; only the 101-byte trampoline is compiled
#    in, from the array mega-net's build generates.
MEGANET="${MEGANET:-$ROOT/../mega-net}"
if [ ! -f "$MEGANET/build/m65/meganet.bin" ] || [ ! -f "$MEGANET/build/gen/meganet_tramp.c" ]; then
  echo "building mega-net..."
  (cd "$MEGANET" && python3 build.py abi >/dev/null)
fi

# 3. The client itself.
echo "building gopher.prg (native MEGA65)..."
"$CLANG" -Os -I "$LIBC_SRC/include" -I "$MEGANET/src/abi" -I "$MEGANET/build/gen" -Wl,-Map="$ROOT/bin/gopher.map" \
  -o "$ROOT/bin/gopher.prg" \
  "$ROOT/src/gopher.c" "$ROOT/src/gopher_net.c" "$ROOT/src/gopher_screen.c" \
  "$ROOT/src/gopher_items.c" "$ROOT/src/gopher_boot.c" "$ROOT/src/gopher_text.c" "$ROOT/src/gopher_config.c" \
  "$ROOT/src/gopher_f011.c" "$ROOT/src/gopher_cbmdos.c" "$ROOT/src/m65_font.c" "$ROOT/src/gopher_exit.S" "$MEGANET/build/gen/meganet_tramp.c" \
  "$LIBC_BUILD/src/libmega65libc.a"

echo "built: bin/gopher.prg ($(wc -c < "$ROOT/bin/gopher.prg") bytes)"
echo "load address: $(xxd -l 2 -e -g 2 "$ROOT/bin/gopher.prg" | awk '{print "$"$2}')"

# Build the distribution disk. It carries two files: the client, and the
# mega-net image the client reads into bank 4 at boot. They must ship
# together -- a GOPHER without its MEGANET cannot bring up the network.
echo "building bin/GOPHER.D81..."
cp "$MEGANET/build/m65/meganet.bin" "$ROOT/bin/meganet"
rm -f "$ROOT/bin/GOPHER.D81"
c1541 -format "gopher,gc" d81 "$ROOT/bin/GOPHER.D81" \
  -write "$ROOT/bin/gopher.prg" gopher \
  -write "$ROOT/bin/meganet" meganet >/dev/null
c1541 -attach "$ROOT/bin/GOPHER.D81" -dir

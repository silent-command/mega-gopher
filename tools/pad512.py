#!/usr/bin/env python3
"""Pad a binary up to a whole number of 512-byte sectors.

The MEGA65 hyppo fileio read512() call works in whole sectors, and a
file whose length is not a multiple of 512 can return a short count or 0
on the final partial sector -- which made the 70-byte trampoline fail to
load at all. Padding sidesteps it; the trailing bytes are never read.
"""
import sys, pathlib
src, dst = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
data = src.read_bytes()
pad = (-len(data)) % 512
dst.write_bytes(data + b"\x00" * pad)
print(f"{src.name}: {len(data)} -> {len(data)+pad} bytes ({pad} padding)")

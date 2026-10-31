#!/usr/bin/env python3
"""Round-trip tests for tools/assets/pvr.py using synthetic textures (no game data)."""
import os, struct, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools", "assets"))
import pvr

def chunk(pf, df, w, h, data): return b"PVRT" + struct.pack("<IBBHHH", 8 + len(data), pf, df, 0, w, h) + data
def same(rgba, w, h, f): return all(tuple(rgba[(y*w+x)*4:(y*w+x)*4+4]) == f(x, y) for y in range(h) for x in range(w))

w = h = 8; fails = 0
px = [(x * 4 + y) & 0x7FFF | 0x8000 for y in range(h) for x in range(w)]
tw = [0] * (w * h)
for y in range(h):
    for x in range(w): tw[pvr.tw(x, y)] = px[y * w + x]
ref = lambda x, y: pvr.px16(px[y * w + x], pvr.ARGB1555)
cases = [
    ("twiddled", chunk(pvr.ARGB1555, pvr.TW, w, h, struct.pack("<%dH" % (w*h), *tw)), ref),
    ("rect", chunk(pvr.ARGB1555, pvr.RECT, w, h, struct.pack("<%dH" % (w*h), *px)), ref),
    ("twiddled_mip", chunk(pvr.ARGB1555, pvr.TW_MM, w, h, b"\0" * 6 + b"\0" * 8 + b"\0" * 32 + struct.pack("<%dH" % (w*h), *tw)), ref),
]
cb = b"".join(struct.pack("<4H", *(0x8000 | k,) * 4) for k in range(256))
idx = bytearray(16)
for y in range(4):
    for x in range(4): idx[pvr.tw(x, y)] = y * 4 + x
vqref = lambda x, y: pvr.px16(0x8000 | ((y // 2) * 4 + x // 2), pvr.RGB565)
cases.append(("vq", chunk(pvr.RGB565, pvr.VQ, w, h, cb + bytes(idx)), vqref))
cases.append(("vq_mip", chunk(pvr.RGB565, pvr.VQ_MM, w, h, cb + b"\0" * pvr.vq_mip_bytes(w) + bytes(idx)), vqref))
for name, data, f in cases:
    W, H, rgba = pvr.decode(data)
    ok = same(rgba, W, H, f); fails += not ok
    print("%-14s %s" % (name, "ok" if ok else "FAIL"))
sys.exit(1 if fails else 0)

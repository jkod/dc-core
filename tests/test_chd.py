#!/usr/bin/env python3
"""tools/disc's pure-Python CHD reader against the same disc as a .gdi (see test_chd.c):
    test_chd.py DIR      (DIR from make_gd_fixture.py + chdman createcd; skipped without disc.chd)"""
import os, sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools", "disc"))
from gdi import GDI  # noqa: E402


def main():
    d = sys.argv[1]
    chd = os.path.join(d, "disc.chd")
    if not os.path.exists(chd):
        print("chd (python): no disc.chd (no chdman), skipped"); return 0
    a, b = GDI(os.path.join(d, "disc.gdi")), GDI(chd)
    fails = 0
    ta = [(t.num, t.lba, t.type, t.sectors - t.offset) for t in a.tracks]
    tb = [(t.num, t.lba, t.type, t.sectors - t.offset) for t in b.tracks]
    if ta != tb:
        print("FAIL tracks differ:\n  gdi %s\n  chd %s" % (ta, tb)); fails += 1
    if a.gd != b.gd or a.header() != b.header():
        print("FAIL disc type or IP.BIN differs"); fails += 1
    bad = sum(a.read_sector(t.lba + k) != b.read_sector(t.lba + k)
              for t in a.tracks if t.is_data for k in range(t.sectors - t.offset))
    if bad:
        print("FAIL %d data sectors differ" % bad); fails += 1
    fa = {p: (l, s) for p, l, s, isdir in a.walk() if not isdir}
    fb = {p: (l, s) for p, l, s, isdir in b.walk() if not isdir}
    if fa != fb or any(a.read(l, s) != b.read(l, s) for l, s in fa.values()):
        print("FAIL files differ"); fails += 1
    print("chd (python): %s" % ("FAILED" if fails else "all ok"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())

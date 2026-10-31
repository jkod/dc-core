#!/usr/bin/env python3
"""dc-core GD-ROM image reader: .gdi + track files -> ISO9660 file tree of the high-density area.

    gdi.py check   game.gdi                 # validate tracks, print IP.BIN header
    gdi.py list    game.gdi                 # list files (path, LBA, size)
    gdi.py extract game.gdi OUTDIR [names]  # extract all files (or only the given names)

Handles 2352- and 2048-byte sectors and file data spread over several data tracks
(e.g. track03 + track05). Reads only the player's own image; nothing is bundled.
"""
import os, struct, sys

HD_START = 45000


class Track:
    def __init__(self, num, lba, ttype, sector, path, offset):
        self.num, self.lba, self.type, self.sector, self.path, self.offset = num, lba, ttype, sector, path, offset
        self.exists = os.path.isfile(path)
        self.sectors = (os.path.getsize(path) // sector) if self.exists else 0
        self.f = None

    @property
    def is_data(self): return self.type == 4


class MissingTrack(Exception):
    pass


class GDI:
    def __init__(self, path):
        self.path = path
        d = os.path.dirname(os.path.abspath(path))
        lines = [l.strip() for l in open(path, encoding="latin-1") if l.strip()]
        n = int(lines[0])
        self.tracks = []
        for line in lines[1:1 + n]:
            # filenames may be quoted and contain spaces
            if '"' in line:
                pre, name, post = line.split('"')[0], line.split('"')[1], line.split('"')[2]
                p = pre.split(); offset = int(post.split()[0]) if post.split() else 0
            else:
                p = line.split(); name = p[4]; offset = int(p[5]) if len(p) > 5 else 0
            self.tracks.append(Track(int(p[0]), int(p[1]), int(p[2]), int(p[3]), os.path.join(d, name), offset))
        # sector counts for missing files: infer from the next track's LBA
        for i, t in enumerate(self.tracks):
            if not t.exists and i + 1 < len(self.tracks):
                t.sectors = self.tracks[i + 1].lba - t.lba - 150 if self.tracks[i + 1].lba > t.lba else 0

    def missing(self):
        return [t for t in self.tracks if not t.exists]

    def track_for(self, lba):
        best = None
        for t in self.tracks:
            if t.lba <= lba and (best is None or t.lba > best.lba):
                best = t
        return best

    def read_sector(self, lba):
        t = self.track_for(lba)
        if t is None:
            raise ValueError("LBA %d before first track" % lba)
        if not t.exists:
            raise MissingTrack("LBA %d is in track %02d (%s), which is missing"
                               % (lba, t.num, os.path.basename(t.path)))
        if t.f is None:
            t.f = open(t.path, "rb")
        idx = lba - t.lba
        if idx >= t.sectors:
            raise ValueError("LBA %d past end of track %02d" % (lba, t.num))
        t.f.seek(idx * t.sector)
        raw = t.f.read(t.sector)
        if t.sector == 2352:
            return raw[16:16 + 2048]     # MODE1: 12 sync + 4 header
        return raw[:2048]

    def read(self, lba, size):
        out = bytearray()
        n = (size + 2047) // 2048
        for k in range(n):
            out += self.read_sector(lba + k)
        return bytes(out[:size])

    # ---- IP.BIN / ISO9660 ----
    def ip_bin(self):
        return self.read(HD_START, 0x8000)

    def header(self):
        ip = self.read_sector(HD_START)
        f = lambda a, b: ip[a:b].decode("latin-1").strip()
        return {"hardware": f(0x00, 0x10), "maker": f(0x10, 0x20), "device": f(0x20, 0x30),
                "area": f(0x30, 0x38), "product": f(0x40, 0x4A), "version": f(0x4A, 0x50),
                "date": f(0x50, 0x60), "boot": f(0x60, 0x70), "company": f(0x70, 0x80), "title": f(0x80, 0x100)}

    def pvd(self):
        p = self.read_sector(HD_START + 16)
        if p[1:6] != b"CD001":
            raise ValueError("no ISO9660 primary volume descriptor at LBA %d" % (HD_START + 16))
        return p

    def walk(self):
        """Yield (path, lba, size, is_dir) for every entry."""
        root = self.pvd()[156:156 + 34]
        lba, size = struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0]
        yield from self._walk_dir(lba, size, "")

    def _walk_dir(self, lba, size, prefix):
        d = self.read(lba, size)
        p = 0
        while p < len(d):
            ln = d[p]
            if ln == 0:
                p = (p // 2048 + 1) * 2048; continue
            e = d[p:p + ln]
            elba, esz, flags = struct.unpack_from("<I", e, 2)[0], struct.unpack_from("<I", e, 10)[0], e[25]
            name = e[33:33 + e[32]]
            p += ln
            if name in (b"\x00", b"\x01"):
                continue
            name = name.decode("latin-1").split(";")[0]
            path = prefix + name
            if flags & 2:
                yield path + "/", elba, esz, True
                yield from self._walk_dir(elba, esz, path + "/")
            else:
                yield path, elba, esz, False


def cmd_check(g):
    ok = True
    for t in g.tracks:
        kind = "data " if t.is_data else "audio"
        state = "ok" if t.exists else "MISSING"
        if t.exists and os.path.getsize(t.path) % t.sector:
            state = "TRUNCATED (size is not a whole number of %d-byte sectors)" % t.sector
            ok = False
        print("track %02d  LBA %6d  %s  %4d  %-14s %s" % (t.num, t.lba, kind, t.sector, os.path.basename(t.path), state))
        ok &= t.exists
    try:
        h = g.header()
        print("\n%s  %s  v%s  %s  boot=%s  area=%s" % (h["product"], h["title"], h["version"], h["date"], h["boot"], h["area"]))
    except MissingTrack as e:
        print("\ncannot read IP.BIN:", e); return 1
    # can every file be read?
    bad = []
    for path, lba, size, isdir in g.walk():
        if isdir or size == 0:
            continue
        last = lba + (size - 1) // 2048
        t = g.track_for(last)
        t0 = g.track_for(lba)
        if not t0.exists or not t.exists or last >= t.lba + t.sectors:
            bad.append((path, t0.num))
    if bad:
        tr = sorted({b[1] for b in bad})
        print("\n%d files lie in missing or cut-off track(s) %s, e.g. %s" % (len(bad), ", ".join("%02d" % x for x in tr),
                                                                ", ".join(b[0] for b in bad[:5])))
        ok = False
    print("\nimage is COMPLETE" if ok else "\nimage is INCOMPLETE")
    return 0 if ok else 1


def main():
    if len(sys.argv) < 3:
        print(__doc__); return 2
    cmd, g = sys.argv[1], GDI(sys.argv[2])
    if cmd == "check":
        return cmd_check(g)
    if cmd == "list":
        for path, lba, size, isdir in g.walk():
            print("%-40s %8d %10d%s" % (path, lba, size, " <dir>" if isdir else ""))
        return 0
    if cmd == "extract":
        out = sys.argv[3]; only = {n.upper() for n in sys.argv[4:]}
        n = 0
        for path, lba, size, isdir in g.walk():
            if isdir:
                continue
            if only and path.upper() not in only and os.path.basename(path).upper() not in only:
                continue
            dst = os.path.join(out, path)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "wb") as f:
                left, cur = size, lba
                while left > 0:
                    chunk = min(left, 2048 * 512)
                    f.write(g.read(cur, chunk)); cur += chunk // 2048; left -= chunk
            n += 1
        print("extracted %d files to %s" % (n, out))
        return 0
    print(__doc__); return 2


if __name__ == "__main__":
    sys.exit(main())

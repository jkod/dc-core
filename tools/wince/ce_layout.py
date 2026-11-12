#!/usr/bin/env python3
"""Windows CE games: lay out the game's PE modules (EXE + DLLs) at fixed addresses for static recompilation.

    ce_layout.py MODDIR --exe GAME.EXE --dlls A.DLL B.DLL ... --out OUTDIR

A Windows CE game is an EXE plus DLLs that Windows CE loads into one process at addresses it picks
at run time. A static recompiler needs every instruction at a known address, so this tool picks
them up front: the EXE at its own image base (CE EXEs have no relocations), the DLLs packed after
it, each relocated to its slot. dc-core's loader (wince/ce_loader.c) relocates the same files from
the player's disc to the same addresses at run time, so the recompiled code and the data agree.

Writes to OUTDIR:
  image.bin    all modules, relocated, from the lowest address up (input for sh4recomp.py --base)
  base.txt     that address as P1 (0x80000000 | va: dc-core folds code addresses onto P1)
  funcs.txt    function starts (and ends, from .pdata: exact on SH-4 CE binaries), exports, entry points
  ce_layout.h  the table for the loader: name, address, image size, file hash

Only the standard library is used (no pefile), so setup scripts run on any Python 3.
"""
import argparse, os, struct, sys

ALIGN = 0x10000


def fnv1a(b):
    h = 1469598103934665603
    for x in b:
        h = ((h ^ x) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


class PE:
    def __init__(self, path):
        self.path = path
        self.name = os.path.basename(path).upper()
        d = self.raw = open(path, "rb").read()
        if d[:2] != b"MZ":
            raise ValueError("%s: not a PE file" % path)
        o = struct.unpack_from("<I", d, 0x3C)[0]
        if d[o:o + 4] != b"PE\0\0":
            raise ValueError("%s: not a PE file" % path)
        self.machine, nsec, _, _, _, optsz, self.chars = struct.unpack_from("<HHIIIHH", d, o + 4)
        if self.machine not in (0x1A6, 0x1A8):          # SH-4 (0x1A6 SH4, 0x1A8 SH5 never seen)
            raise ValueError("%s: machine %04X is not SH-4" % (path, self.machine))
        opt = o + 24
        self.entry = struct.unpack_from("<I", d, opt + 16)[0]
        self.image_base = struct.unpack_from("<I", d, opt + 28)[0]
        self.size = struct.unpack_from("<I", d, opt + 56)[0]
        self.hdr_size = struct.unpack_from("<I", d, opt + 60)[0]
        ndir = struct.unpack_from("<I", d, opt + 92)[0]
        self.dirs = [struct.unpack_from("<II", d, opt + 96 + 8 * i) for i in range(min(ndir, 16))]
        self.secs = []
        so = opt + optsz
        for i in range(nsec):
            name, vsize, va, rsize, rptr = struct.unpack_from("<8sIIII", d, so + 40 * i)
            self.secs.append((name.rstrip(b"\0").decode("latin-1"), va, vsize, rsize, rptr))

    def dir(self, i):
        return self.dirs[i] if i < len(self.dirs) else (0, 0)

    def image(self):
        """the file mapped as the CE loader maps it (headers + sections at their RVAs)"""
        img = bytearray(self.size)
        img[:self.hdr_size] = self.raw[:self.hdr_size]
        for _, va, vsize, rsize, rptr in self.secs:
            n = min(rsize, vsize) if vsize else rsize
            img[va:va + n] = self.raw[rptr:rptr + n]
        return img

    def relocate(self, img, new_base):
        delta = (new_base - self.image_base) & 0xFFFFFFFF
        rva, size = self.dir(5)
        if not delta:
            return
        if not size:
            raise ValueError("%s has no relocations and can't move from %08X" % (self.name, self.image_base))
        p, end = rva, rva + size
        while p < end:
            page, bsize = struct.unpack_from("<II", img, p)
            if bsize < 8:
                break
            for k in range((bsize - 8) // 2):
                e = struct.unpack_from("<H", img, p + 8 + 2 * k)[0]
                t, off = e >> 12, e & 0xFFF
                if t == 0:
                    continue
                if t != 3:
                    raise ValueError("%s: relocation type %d not supported" % (self.name, t))
                a = page + off
                v = struct.unpack_from("<I", img, a)[0]
                struct.pack_into("<I", img, a, (v + delta) & 0xFFFFFFFF)
            p += bsize

    def pdata_funcs(self):
        """function starts (RVAs) from .pdata: 8-byte entries {start VA, prolog:8 len:22 flags:2}"""
        rva, size = self.dir(3)
        img = self.image()
        out = []
        for k in range(size // 8):
            start, info = struct.unpack_from("<II", img, rva + 8 * k)
            if start:
                out.append((start - self.image_base, ((info >> 8) & 0x3FFFFF) * 2))
        return out

    def exports(self):
        rva, size = self.dir(0)
        if not size:
            return []
        img = self.image()
        _, _, _, _, _, nfun, nnames, afun, anames, aords = struct.unpack_from("<IIHHIIIIII", img, rva)
        out = []
        for i in range(nfun):
            f = struct.unpack_from("<I", img, afun + 4 * i)[0]
            if f and not (rva <= f < rva + size):            # skip forwarders
                out.append(f)
        return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("moddir")
    ap.add_argument("--exe", required=True)
    ap.add_argument("--dlls", nargs="*", default=[])
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    def find(n):
        for f in os.listdir(args.moddir):
            if f.upper() == n.upper():
                return os.path.join(args.moddir, f)
        sys.exit("%s not found in %s" % (n, args.moddir))

    exe = PE(find(args.exe))
    mods = [(exe, exe.image_base)]
    at = (exe.image_base + exe.size + ALIGN - 1) & ~(ALIGN - 1)
    for n in args.dlls:
        m = PE(find(n))
        mods.append((m, at))
        at = (at + m.size + ALIGN - 1) & ~(ALIGN - 1)
    lo = exe.image_base
    img = bytearray(at - lo)
    funcs = set()
    ends = {}
    rows = []
    for m, base in mods:
        mi = m.image()
        m.relocate(mi, base)
        img[base - lo:base - lo + len(mi)] = mi
        for rva, n in m.pdata_funcs():
            funcs.add(base + rva)
            if n:
                ends[base + rva] = max(ends.get(base + rva, 0), base + rva + n)
        for rva in m.exports():
            funcs.add(base + rva)
        # code without .pdata (import thunks, hand-written helpers): each stretch of .text between the
        # functions .pdata lists starts a function of its own
        known = sorted((base + r, base + r + n) for r, n in m.pdata_funcs())
        for name, va, vsize, _, _ in m.secs:
            if name != ".text":
                continue
            gap, end = base + va, base + va + vsize
            for s0, e0 in known + [(end, end)]:
                if s0 > gap and gap < end and any(mi[gap - base + k] for k in range(0, min(8, s0 - gap))):
                    funcs.add(gap)
                    ends[gap] = min(s0, end)
                gap = max(gap, e0)
        if m.entry:
            funcs.add(base + m.entry)
        rows.append((m.name, base, m.size, fnv1a(m.raw), len(m.raw)))
    open(os.path.join(args.out, "image.bin"), "wb").write(img)
    open(os.path.join(args.out, "base.txt"), "w").write("0x%08X\n" % (0x80000000 | lo))
    open(os.path.join(args.out, "funcs.txt"), "w").write("".join(
        "%08x %08x\n" % (0x80000000 | f, 0x80000000 | ends[f]) if f in ends else "%08x\n" % (0x80000000 | f)
        for f in sorted(x for x in funcs if x % 2 == 0)))
    h = ["/* generated by dc-core tools/wince/ce_layout.py: where each module of the game lives */",
         "#define CE_LAYOUT_END 0x%08Xu" % at,
         "static const struct { const char *name; unsigned base, size; unsigned long long hash; unsigned file_size; }"
         " ce_layout[] = {"]
    for name, base, size, hsh, fs in rows:
        h.append('  { "%s", 0x%08Xu, 0x%Xu, 0x%016Xull, %u },' % (name, base, size, hsh, fs))
    h.append("};")
    open(os.path.join(args.out, "ce_layout.h"), "w").write("\n".join(h) + "\n")
    print("%d modules at %08X..%08X, %d function starts" % (len(mods), lo, at, len(funcs)))


if __name__ == "__main__":
    main()

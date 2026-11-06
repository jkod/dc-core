#!/usr/bin/env python3
"""Turn a Switch profile.txt (the on-screen menu's PROFILE item) into function names.

    python3 tools/prof/nxprof.py build/switch/revolt.elf profile.txt [--top 40]

Reads the .elf's own symbol table (no devkitPro tools needed). Recompiled game functions are f_* (and
fs_*, their fallback copies); everything else is dc-core, SDL, Mesa or the system."""
import argparse, bisect, collections, struct, sys


def elf_funcs(path):
    d = open(path, "rb").read()
    if d[:4] != b"\x7fELF" or d[4] != 2:
        sys.exit("%s: not a 64-bit ELF" % path)
    shoff, = struct.unpack_from("<Q", d, 0x28)
    shentsize, shnum = struct.unpack_from("<HH", d, 0x3A)
    secs = [struct.unpack_from("<IIQQQQIIQQ", d, shoff + i * shentsize) for i in range(shnum)]
    out = []
    for name, typ, flags, addr, off, size, link, info, align, entsize in secs:
        if typ != 2:                                  # SHT_SYMTAB
            continue
        stroff = secs[link][4]
        for k in range(size // 24):
            st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from("<IBBHQQ", d, off + k * 24)
            if (st_info & 15) != 2 or not st_value:  # STT_FUNC
                continue
            e = d.index(b"\0", stroff + st_name)
            out.append((st_value, st_size, d[stroff + st_name:e].decode("latin-1")))
    if not out:
        sys.exit("%s: no symbol table (stripped?)" % path)
    out.sort()
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("profile")
    ap.add_argument("--top", type=int, default=40)
    a = ap.parse_args()
    funcs = elf_funcs(a.elf)
    addrs = [f[0] for f in funcs]
    anchor = [f[0] for f in funcs if f[2] == "dc_prof_anchor"]
    if not anchor:
        sys.exit("dc_prof_anchor isn't in %s: is it the build that made the profile?" % a.elf)
    hits, groups, n = collections.Counter(), collections.Counter(), 0
    for line in open(a.profile):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        pc = anchor[0] + int(line, 16) if not line.startswith("-") else anchor[0] - int(line[1:], 16)
        i = bisect.bisect_right(addrs, pc) - 1
        name = "(outside the program: system / kernel)"
        if i >= 0 and pc < funcs[i][0] + max(funcs[i][1], 1) + 0x1000:
            name = funcs[i][2]
        n += 1
        hits[name] += 1
        if name.startswith(("f_", "fs_")):
            groups["game code (f_ / fs_)"] += 1
        elif name.startswith(("arm7_", "dsp_", "aica", "dc_aica", "sample_")):
            groups["sound chip"] += 1
        elif name.startswith(("dc_", "sh4_", "ta_", "pvr", "new_", "gd")):
            groups["dc-core runtime"] += 1
        elif name.startswith("("):
            groups[name] += 1
        else:
            groups["libraries (libc, SDL, Mesa, libnx)"] += 1
    print("%d samples\n" % n)
    for g, v in groups.most_common():
        print("%5.1f%%  %s" % (100.0 * v / n, g))
    print("\ntop functions:")
    for name, v in hits.most_common(a.top):
        print("%5.1f%%  %s" % (100.0 * v / n, name))


if __name__ == "__main__":
    main()

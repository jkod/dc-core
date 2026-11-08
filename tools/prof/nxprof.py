#!/usr/bin/env python3
"""Turn a Switch profile.txt (the on-screen menu's PROFILE item) into function names.

    python3 tools/prof/nxprof.py build/switch/revolt.elf profile.txt [--top 40]
    python3 tools/prof/nxprof.py build/switch/revolt.elf --crash 0123_0100690020fbe000.log
      (an Atmosphere crash report: every "revolt + 0x..." address as a function name)

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


def load_base(path):
    d = open(path, "rb").read()
    phoff, = struct.unpack_from("<Q", d, 0x20)
    phentsize, phnum = struct.unpack_from("<HH", d, 0x36)
    lo = None
    for i in range(phnum):
        typ, flags, off, vaddr = struct.unpack_from("<IIQQ", d, phoff + i * phentsize)
        if typ == 1 and (lo is None or vaddr < lo):
            lo = vaddr
    return lo or 0


def crash(elf, funcs, addrs, log):
    import re
    base = load_base(elf)
    seen = set()
    for line in open(log, errors="replace"):
        m = re.search(r"\(\w+ \+ 0x([0-9a-fA-F]+)\)", line)
        if not m:
            if re.match(r"\s*(Thread ID|Threads\[|Crashed Thread|Exception Info|Type:|Address:)", line):
                print(line.rstrip())
            continue
        pc = base + int(m.group(1), 16)
        i = bisect.bisect_right(addrs, pc) - 1
        name = funcs[i][2] if i >= 0 else "?"
        off = pc - funcs[i][0] if i >= 0 else 0
        print("%s   -> %s+0x%x" % (line.rstrip(), name, off))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("profile", nargs="?")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--crash", help="an Atmosphere crash report to put function names on")
    a = ap.parse_args()
    funcs = elf_funcs(a.elf)
    addrs = [f[0] for f in funcs]
    if a.crash:
        crash(a.elf, funcs, addrs, a.crash)
        return
    if not a.profile:
        sys.exit("give a profile.txt, or --crash LOG")
    anchor = [f[0] for f in funcs if f[2] == "dc_prof_anchor"]
    if not anchor:
        sys.exit("dc_prof_anchor isn't in %s: is it the build that made the profile?" % a.elf)
    hits, groups, n = collections.Counter(), collections.Counter(), 0
    chains = collections.Counter()

    def name_of(off):
        pc = anchor[0] + off
        i = bisect.bisect_right(addrs, pc) - 1
        if i >= 0 and pc < funcs[i][0] + max(funcs[i][1], 1) + 0x1000:
            return funcs[i][2]
        return "(outside the program: system / kernel)"

    def num(t):
        v = int(t[1:], 16) if t.startswith("-") else int(t, 16)
        if t.startswith("-"):
            v = -v
        return v - (1 << 64) if v >= 1 << 63 else v

    # waiting / system-call leaves: what called them (newer profiles carry lr and 4 or 10 stack frames)
    ours = ("dc_", "f_", "fs_", "pvr", "osd_", "aica", "present", "batch_", "draw_", "probe_", "edge_", "texture_for", "load_rgb", "on_vblank", "ready_task")
    plumbing = ("svc", "mutex", "rmutex", "condvar", "__libnx", "_lock", "__retarget_lock", "__lock", "barrier", "semaphore")
    for line in open(a.profile):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        cols = line.split()
        name = name_of(num(cols[0]))
        n += 1
        hits[name] += 1
        if name.startswith("svc") and len(cols) > 1:
            path, frm = [], None
            for t in cols[1:]:
                v = num(t)
                if v == 0:
                    continue
                c = name_of(v)
                if c.startswith(plumbing) or (path and path[-1] == c):
                    continue
                if len(path) < 3:
                    path.append(c)
                elif c.startswith(ours):              # deeper profiles: the first of our own code that led there
                    frm = c
                    break
            chain = name + "  <-  " + "  <-  ".join(path or ["?"])
            if frm and not any(p.startswith(ours) for p in path):
                chain += "  ...  " + frm
            chains[chain] += 1
    for name, v in hits.items():
        if name.startswith(("f_", "fs_")):
            groups["game code (f_ / fs_)"] += v
        elif name.startswith(("arm7_", "dsp_", "aica", "dc_aica", "sample_")):
            groups["sound chip"] += v
        elif name.startswith(("dc_", "sh4_", "ta_", "pvr", "new_", "gd")):
            groups["dc-core runtime"] += v
        elif name.startswith("("):
            groups[name] += v
        else:
            groups["libraries (libc, SDL, Mesa, libnx)"] += v
    print("%d samples\n" % n)
    for g, v in groups.most_common():
        print("%5.1f%%  %s" % (100.0 * v / n, g))
    if chains:
        print("\nsystem calls and who made them (leaf <- callers):")
        for c, v in chains.most_common(15):
            print("%5.1f%%  %s" % (100.0 * v / n, c))
    print("\ntop functions:")
    for name, v in hits.most_common(a.top):
        print("%5.1f%%  %s" % (100.0 * v / n, name))


if __name__ == "__main__":
    main()

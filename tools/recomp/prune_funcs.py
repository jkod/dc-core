#!/usr/bin/env python3
"""Drop function starts that are really data, judged from the generated C of a scan run.

    sh4recomp.py GAME.BIN --out gen --write-funcs scanned.txt
    prune_funcs.py gen scanned.txt funcs.txt [limit]

A start is dropped when its first instruction is invalid or more than `limit` (default 0.35) of
its instructions are. Literal pools inside real functions stay well below that. Use the result
with --no-discover (CMake: NO_DISCOVER): discovery would scan the data again. Jumps into what
was dropped still work through the interpreter, and show up as "interpreting" in the log.
"""
import glob, re, sys


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    gen, funcs_in, funcs_out = sys.argv[1:4]
    limit = float(sys.argv[4]) if len(sys.argv) > 4 else 0.35
    bad = set()
    for f in glob.glob(gen + "/recomp_*.c"):
        cur = None
        for line in open(f):
            m = re.match(r"uint32_t f_([0-9A-F]{8})\(sh4ctx", line)
            if m:
                cur = int(m.group(1), 16); n = ill = 0; first = None
                continue
            if cur is None:
                continue
            if line.startswith("L_"):
                n += 1
                continue
            bad_insn = "dc_trap" in line and '"illegal' in line
            if n and first is None:
                first = bad_insn
            ill += bad_insn
            if line.startswith("local_jump:"):
                if first or (n and ill / n > limit):
                    bad.add(cur)
                cur = None
    starts = [l for l in open(funcs_in).read().split() if l.strip()]
    keep = [l for l in starts if int(l, 16) not in bad]
    open(funcs_out, "w").write("\n".join(keep) + "\n")
    print("dropped %d of %d function starts" % (len(starts) - len(keep), len(starts)))


if __name__ == "__main__":
    main()

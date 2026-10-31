#!/usr/bin/env python3
"""Unit tests for the SH-4 -> C recompiler: hand-assembled snippets, translated, compiled, run.

    python3 tests/test_recomp.py
"""
import os, struct, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = 0x8C010000

# Each test: (name, {offset: [halfwords]}, setup C, expected C condition)
TESTS = []

def t(name, prog, setup, check):
    TESTS.append((name, prog, setup, check))

# 1. unsigned 32/32 division: div0u + 32 x (rotcl r0; div1 r1,r0) + rotcl r0
#    dividend r2, divisor r1, remainder r0 (starts 0): div0u + 32 x (rotcl r2; div1 r1,r0) + rotcl r2
div = [0x0019] + [0x4224, 0x3014] * 32 + [0x4224, 0x000B, 0x0009]
t("div1_unsigned", {0: div}, "c.r[2] = 1000000007u; c.r[1] = 12345u;", "c.r[2] == 1000000007u / 12345u")
t("div1_unsigned_big", {0: div}, "c.r[2] = 0xFFFFFFF0u; c.r[1] = 7u;", "c.r[2] == 0xFFFFFFF0u / 7u")

# 2. loop with dt / bf: sum 10..1
t("dt_loop", {0: [0xE10A, 0xE000, 0x301C, 0x4110, 0x8BFC, 0x000B, 0x0009]}, "", "c.r[0] == 55")

# 3. bsr + rts with delay slots, pr save/restore
t("bsr_rts_slots", {0: [0x4F22, 0xB005, 0xE405, 0x4F26, 0x000B, 0x7001, 0x0009, 0x0009,
                       0x6043, 0x000B, 0x300C]},
  "", "c.r[0] == 11")

# 4. FPU: (1+1)^2 + 7 -> ftrc -> 11
t("fpu_basic", {0: [0xF09D, 0xF19D, 0xF010, 0xF002, 0xE107, 0x415A, 0xF22D, 0xF020,
                   0xF03D, 0x005A, 0x000B, 0x0009]}, "", "c.r[0] == 11")

# 5. braf jump inside a function
t("braf_local", {0: [0xE102, 0x0123, 0x0009, 0xE063, 0xE007, 0x000B, 0x0009]}, "", "c.r[0] == 7")

# 6. bt/s: condition sampled before the slot changes T
#   cmp/eq r0,r0 (3000) ; bt/s +2 (8D01) ; clrt (0008) ; mov #1,r1 (E101) ; mov #2,r2 (E202) ; rts ; nop
t("bts_cond_before_slot", {0: [0x3000, 0x8D01, 0x0008, 0xE101, 0xE202, 0x000B, 0x0009]},
  "c.r[1] = 0;", "c.r[1] == 0 && c.r[2] == 2 && c.t == 0")

# 7. jsr through a register loaded from a literal pool, delay slot modifies the register
#   0: mov.l @(lit),r1 (D1xx) ; 2: jsr @r1 (410B) ; 4: mov #0,r1 (E100) ; 6: rts ; 8: nop
#   literal at 0xC = BASE+0x20 ; at 0x20: mov #42,r0 ; rts ; nop
#   D1xx: disp = (0xC - ((0+4)&~3))/4 = 2 -> D102
t("jsr_reg_read_before_slot", {0: [0xD102, 0x410B, 0xE100, 0x000B, 0x0009, 0x0009,
                                   (BASE + 0x20) & 0xFFFF, (BASE + 0x20) >> 16],
                               0x20: [0xE02A, 0x000B, 0x0009]},
  "", "c.r[0] == 42")

# 8. shad / shld both directions
#   mov #-4,r1 (E1FC); shad r1,r2 (412C... shad Rm,Rn = 0100 nnnn mmmm 1100 -> n=2,m=1: 421C)
#   mov #-4,r3? reuse r1: shld r1,r4 -> 441D ; rts ; nop
t("shad_shld", {0: [0xE1FC, 0x421C, 0x441D, 0x000B, 0x0009]},
  "c.r[2] = 0x80000000u; c.r[4] = 0x80000000u;", "c.r[2] == 0xF8000000u && c.r[4] == 0x08000000u")

# 9. fmov with SZ=1 copies a 64-bit pair; fschg toggles
#   fschg (F3FD) ; fmov @r4,dr0 (F048: fmov @Rm,FRn n=0 m=4) ; fmov dr0,@r5 (F50A: fmov FRm,@Rn n=5 m=0) ; fschg ; rts ; nop
t("fmov_sz_pair", {0: [0xF3FD, 0xF048, 0xF50A, 0xF3FD, 0x000B, 0x0009]},
  "c.r[4] = 0x8C100000u; c.r[5] = 0x8C100010u; dc_wr32(0x8C100000u, 0x11111111u); dc_wr32(0x8C100004u, 0x22222222u);",
  "dc_rd32(0x8C100010u) == 0x11111111u && dc_rd32(0x8C100014u) == 0x22222222u && !(c.fpscr & FPSCR_SZ)")

# 10. addc / subc 64-bit add and subtract
#   clrt; addc r2,r0 (302E); addc r3,r1 (313E); rts; nop
t("addc64", {0: [0x0008, 0x302E, 0x313E, 0x000B, 0x0009]},
  "c.r[0]=0xFFFFFFFFu; c.r[1]=1; c.r[2]=1; c.r[3]=2;", "c.r[0]==0 && c.r[1]==4")
t("subc64", {0: [0x0008, 0x302A, 0x313A, 0x000B, 0x0009]},
  "c.r[0]=0; c.r[1]=4; c.r[2]=1; c.r[3]=2;", "c.r[0]==0xFFFFFFFFu && c.r[1]==1")

# 11. signed division via div0s/div1 (Hitachi sdiv pattern for 32/16 is complex; check div0s flags)
t("div0s_flags", {0: [0x2107, 0x000B, 0x0009]},  # div0s r0,r1 -> 0010 nnnn mmmm 0111 n=1,m=0
  "c.r[0]=0x80000000u; c.r[1]=1;", "c.q==0 && c.m==1 && c.t==1")

# 12. ftrv with an identity*2 matrix in XF
#   frchg (FBFD) -> fill fr0..15 as matrix -> frchg -> ftrv xmtrx,fv4 (F5FD: nn=01 -> n=0101? FV4 => nn=01 -> 1111 0101 1111 1101)
t("ftrv", {0: [0xF5FD, 0x000B, 0x0009]},
  "for (int i=0;i<16;i++) c.fr.f[16+i] = (i%5==0) ? 2.0f : 0.0f; c.fr.f[4]=1; c.fr.f[5]=2; c.fr.f[6]=3; c.fr.f[7]=4;",
  "c.fr.f[4]==2 && c.fr.f[5]==4 && c.fr.f[6]==6 && c.fr.f[7]==8")

# 13. regression: bf/s whose target is outside the function must not return when not taken
#   0: tst r4,r4 (2448) ; 2: bf/s +0x10 -> 0x18 (8F0A... target = 2+4+d*2 = 0x18 -> d=9 -> 8F09)
#   4: mov #1,r1 (E101) ; 6: mov #5,r0 (E005) ; 8: rts ; A: nop      [taken path lands in fn at 0x18]
#   0x18: mov #9,r0 (E009) ; rts ; nop
t("cbr_out_of_function_not_taken", {0: [0x2448, 0x8F09, 0xE101, 0xE005, 0x000B, 0x0009],
                                    0x18: [0xE009, 0x000B, 0x0009]},
  "c.r[4] = 0;", "c.r[0] == 5 && c.r[1] == 1")
t("cbr_out_of_function_taken", {0: [0x2448, 0x8F09, 0xE101, 0xE005, 0x000B, 0x0009],
                                0x18: [0xE009, 0x000B, 0x0009]},
  "c.r[4] = 3;", "c.r[0] == 9 && c.r[1] == 1")

# 14. fmac is fused (one rounding): fr0*fr1 + fr2 where the unfused result differs
#   fmac fr0,fr1,fr2 = 1111 nnnn mmmm 1110 -> n=2, m=1 : F21E
t("fmac_fused", {0: [0xF21E, 0x000B, 0x0009]},
  "c.fr.f[0] = 1.0f + 0x1p-12f; c.fr.f[1] = 1.0f + 0x1p-12f; c.fr.f[2] = -1.0f;",
  "c.fr.f[2] == fmaf(1.0f + 0x1p-12f, 1.0f + 0x1p-12f, -1.0f) && c.fr.f[2] != (float)((1.0f + 0x1p-12f) * (1.0f + 0x1p-12f)) - 1.0f")

# 15. vblank interrupt: guest polls a counter that only its VBR+0x600 handler increments.
#   main:  mov.l @r4,r0 ; cmp/eq #2,r0 ; bf main ; rts ; nop
#   0x600: mov.l @r4,r0 ; add #1,r0 ; mov.l r0,@r4 ; mov.l @(lit),r1 ; mov #8,r2 ; mov.l r2,@r1 (ack ISTNRM) ; rte ; nop
#   handler runs on register bank 1, so main's r1 must survive
t("vblank_irq_bank_rte", {0: [0x6042, 0x8802, 0x8BFC, 0x000B, 0x0009],
                          0x600: [0x6042, 0x7001, 0x2402, 0xD103, 0xE208, 0x2122, 0x002B, 0x0009, 0x0009, 0x0009,
                                  0x6900, 0xA05F]},
  "c.vbr = 0x8C010000u; sh4_set_sr(&c, 0x40000000u); dc_wr32(0xA05F6930u, 8); dc_wr32(0x8C100000u, 0);"
  "c.r[4] = c.rbank[4] = 0x8C100000u; c.r[1] = 0x1234;",
  "dc_rd32(0x8C100000u) == 2 && c.r[1] == 0x1234 && !(sh4_get_sr(&c) & SR_BL) && (dc_rd32(0xA05F6900u) & 8) == 0")

# 16. a loop split across two functions (A -> B -> A ...) must not grow the C stack
#   A@0: dt r1 ; bt 8 ; bra B ; nop ; 8: rts ; nop        B@0x10: add #1,r0 ; bra A ; nop
t("cross_function_loop_1M", {0: [0x4110, 0x8901, 0xA004, 0x0009, 0x000B, 0x0009],
                             0x10: [0x7001, 0xAFF5, 0x0009]},
  "c.r[1] = 1000000; c.r[0] = 0;", "c.r[0] == 999999")

# 17. code copied to RAM at runtime (like exception vectors) runs as the recompiled original
t("runtime_copied_code_alias", {0: [0x0009, 0x000B, 0x0009], 0x20: [0xE02A, 0x7001, 0x000B, 0x0009]},
  "memcpy(dc_ram + 0xF000, dc_ram + 0x10020, 32); dc_alias_verbose = 0;",
  "(dc_call(&c, 0x8C00F000u), c.r[0] == 43)")


def build_bin(prog):
    size = max(off + 2 * len(w) for off, w in prog.items())
    size = (size + 0x40) & ~3
    b = bytearray(size)
    for off, words in prog.items():
        for k, w in enumerate(words):
            struct.pack_into("<H", b, off + 2 * k, w)
    return bytes(b)


def main():
    tmp = tempfile.mkdtemp(prefix="dccore_test_")
    ok = 0
    for name, prog, setup, check in TESTS:
        d = os.path.join(tmp, name); os.makedirs(d)
        open(os.path.join(d, "t.bin"), "wb").write(build_bin(prog))
        starts = sorted(BASE + off for off in prog)
        open(os.path.join(d, "funcs.txt"), "w").write("\n".join("%08x" % s for s in starts))
        subprocess.check_call([sys.executable, os.path.join(ROOT, "tools/recomp/sh4recomp.py"),
                               os.path.join(d, "t.bin"), "--funcs", os.path.join(d, "funcs.txt"),
                               "--out", os.path.join(d, "gen")], stdout=subprocess.DEVNULL)
        main_c = """
#include "dccore/runtime.h"
#include <stdio.h>
#include <string.h>
extern int dc_alias_verbose;
int main(void) {
  sh4ctx c; dc_init(&c);
  if (dc_load_binary("%s", 0x8C010000u) < 0) return 2;
  %s
  c.pr = 0; dc_call(&c, 0x8C010000u);
  if (%s) return 0;
  printf("r0=%%08X r1=%%08X r2=%%08X t=%%u\\n", c.r[0], c.r[1], c.r[2], c.t);
  return 1;
}
""" % (os.path.join(d, "t.bin"), setup, check)
        open(os.path.join(d, "main.c"), "w").write(main_c)
        srcs = [os.path.join(d, "main.c")] + [os.path.join(ROOT, "runtime/src", f) for f in os.listdir(os.path.join(ROOT, "runtime/src"))] + \
               [os.path.join(d, "gen", f) for f in os.listdir(os.path.join(d, "gen"))]
        exe = os.path.join(d, "t")
        subprocess.check_call(["cc", "-O1", "-w", "-I" + os.path.join(ROOT, "runtime/include"), "-o", exe] + srcs + ["-lm"])
        r = subprocess.run([exe], capture_output=True, text=True)
        status = "ok" if r.returncode == 0 else "FAIL " + r.stdout.strip() + r.stderr.strip()
        print("%-28s %s" % (name, status))
        ok += r.returncode == 0
    print("%d/%d passed" % (ok, len(TESTS)))
    sys.exit(0 if ok == len(TESTS) else 1)


if __name__ == "__main__":
    main()

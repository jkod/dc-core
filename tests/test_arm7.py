#!/usr/bin/env python3
"""ARM7 interpreter tests: programs assembled with keystone, run on runtime/src/arm7.c, results
checked against values computed here. Needs: pip install keystone-engine"""
import os, subprocess, sys, tempfile
from keystone import Ks, KS_ARCH_ARM, KS_MODE_ARM

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ks = Ks(KS_ARCH_ARM, KS_MODE_ARM)
M = 0xFFFFFFFF
TESTS = []

def t(name, src, checks, setup=""):
    TESTS.append((name, src, checks, setup))

# programs end with "str r0,[r13...]"-free halt: write anything to 0xFFFC via r12
HALT = "ldr r12, =0xFFFC; str r0, [r12]; b ."

t("add_sub_flags", """
    mov r0, #0xFFFFFFFF
    adds r1, r0, #1          @ r1=0, C=1 Z=1
    adc r2, r0, #1           @ r2 = 0xFFFFFFFF + 1 + 1 = 1
    mov r3, #5
    subs r4, r3, #7          @ -2, C=0 (borrow)
    sbc r5, r3, #1           @ 5 - 1 - 1 = 3
    rsc r6, r3, #10          @ C from subs? (after sbc, flags still from subs: C=0) -> 10 - 5 - 1 = 4
    mov r7, #0x80000000
    subs r8, r7, #1          @ signed overflow: V=1
    mrs r9, cpsr
""" + HALT, {"r1": 0, "r2": 1, "r4": (-2) & M, "r5": 3, "r6": 4, "r8": 0x7FFFFFFF, "r9_flags": 0x30000000})

t("shifts", """
    mov r0, #0x80000001
    movs r1, r0, lsl #1      @ 2, C=1
    mrs r2, cpsr
    movs r3, r0, lsr #1      @ 0x40000000, C=1
    mov r4, r0, asr #4       @ 0xF8000000
    mov r5, r0, ror #4       @ 0x18000000
    mov r6, #3
    mov r7, r0, lsl r6       @ 8
    mov r8, #33
    movs r9, r0, lsl r8      @ 0, C=0
    mov r10, r0, rrx         @ C was 0 -> 0x40000000
""" + HALT, {"r1": 2, "r2_flags": 0x20000000, "r3": 0x40000000, "r4": 0xF8000000, "r5": 0x18000000,
             "r7": 8, "r9": 0, "r10": 0x40000000})

t("load_store_modes", """
    ldr r1, =0x1000
    ldr r0, =0x11223344
    str r0, [r1]
    ldrb r2, [r1, #1]         @ 0x33
    strb r2, [r1, #4]!        @ r1 = 0x1004
    ldr r3, [r1], #-4         @ word at 0x1004 = 0x33, r1 back to 0x1000
    mov r4, #2
    ldr r5, [r1, r4, lsl #1]  @ word at 0x1004
    ldr r6, [r1, #1]          @ unaligned: rotated 0x44112233
    mov r7, r1
""" + HALT, {"r2": 0x33, "r3": 0x33, "r5": 0x33, "r6": 0x44112233, "r7": 0x1000})

t("ldm_stm_call", """
    ldr sp, =0x2000
    mov r0, #3
    mov r1, #4
    bl func
    mov r2, r0
    b done
func:
    stmfd sp!, {r4, r5, lr}
    mov r4, r0
    mov r5, r1
    mul r0, r4, r5
    ldmfd sp!, {r4, r5, pc}
done:
    mov r3, sp
""" + HALT, {"r2": 12, "r3": 0x2000})

t("multiplies", """
    ldr r0, =0x12345678
    ldr r1, =0x9ABCDEF0
    mul r2, r0, r1
    mov r3, #7
    mla r4, r0, r3, r1
    umull r5, r6, r0, r1
    smull r7, r8, r0, r1
""" + HALT, {"r2": (0x12345678 * 0x9ABCDEF0) & M, "r4": (0x12345678 * 7 + 0x9ABCDEF0) & M,
             "r5": (0x12345678 * 0x9ABCDEF0) & M, "r6": (0x12345678 * 0x9ABCDEF0) >> 32,
             "r7": (0x12345678 * (0x9ABCDEF0 - (1 << 32))) & M, "r8": ((0x12345678 * (0x9ABCDEF0 - (1 << 32))) >> 32) & M})

t("halfwords", """
    ldr r1, =0x1000
    ldr r0, =0x8001F0FF
    str r0, [r1]
    ldrh r2, [r1]            @ 0xF0FF
    ldrsh r3, [r1]           @ 0xFFFFF0FF
    ldrsb r4, [r1, #1]       @ 0xFFFFFFF0
    ldr r5, =0x1234
    strh r5, [r1, #2]
    ldr r6, [r1]             @ 0x1234F0FF
""" + HALT, {"r2": 0xF0FF, "r3": 0xFFFFF0FF, "r4": 0xFFFFFFF0, "r6": 0x1234F0FF})

# FIQ: vector 0x1C branches to handler that bumps r8_fiq and a memory counter, returns with subs pc,lr,#4
t("fiq_banked", """
    b start
    b .
    b .
    b .
    b .
    b .
    b .
    b fiq
fiq:
    add r8, r8, #1           @ FIQ-banked r8
    ldr r9, =0x1100
    ldr r10, [r9]
    add r10, r10, #1
    str r10, [r9]
    ldr r11, =0xFFF0         @ ack: test bus clears the FIQ line on this write
    str r10, [r11]
    subs pc, lr, #4
start:
    mov r8, #100             @ SVC/user r8
    mrs r0, cpsr
    bic r0, r0, #0x40        @ enable FIQ
    msr cpsr_c, r0
    ldr r9, =0x1100
loop:
    ldr r1, [r9]
    cmp r1, #3
    blt loop
    mov r2, r8               @ must still be 100
""" + HALT, {"r1": 3, "r2": 100}, setup="fiq_every = 50;")

t("swi_and_modes", """
    b start
    b .
    b swi
    b .
    b .
    b .
    b .
    b .
swi:
    mov r5, #0x55
    movs pc, lr
start:
    mrs r0, cpsr
    and r6, r0, #0x1F        @ starts in SVC (0x13)
    ldr sp, =0x3000
    msr cpsr_c, #0xD0        @ user mode, IRQ/FIQ off
    ldr sp, =0x2800          @ user sp
    swi #0
    mov r7, sp               @ back in user mode with user sp
    mrs r8, cpsr
    and r8, r8, #0x1F
""" + HALT, {"r5": 0x55, "r6": 0x13, "r7": 0x2800, "r8": 0x10})

t("loop_sum_primes", """
    mov r0, #0               @ count primes < 200
    mov r1, #2
outer:
    mov r2, #2
inner:
    mul r3, r2, r2
    cmp r3, r1
    bgt isprime
    mov r4, r1
mod:
    cmp r4, r2
    subge r4, r4, r2
    bge mod
    cmp r4, #0
    beq notprime
    add r2, r2, #1
    b inner
isprime:
    add r0, r0, #1
notprime:
    add r1, r1, #1
    cmp r1, #200
    blt outer
""" + HALT, {"r0": 46})

MAIN = r'''
#include "dccore/arm7.h"
#include <stdio.h>
#include <string.h>
static uint8_t mem[0x10000];
static int halted, fiq_every, cyc;
uint32_t arm7_read32(uint32_t a) { uint32_t v; memcpy(&v, mem + (a & 0xFFFC), 4); return v; }
uint16_t arm7_read16(uint32_t a) { uint16_t v; memcpy(&v, mem + (a & 0xFFFE), 2); return v; }
uint8_t  arm7_read8(uint32_t a) { return mem[a & 0xFFFF]; }
void arm7_write32(uint32_t a, uint32_t v) { if ((a & 0xFFFF) == 0xFFFC) halted = 1; if ((a & 0xFFFF) == 0xFFF0) arm7.fiq_line = 0; memcpy(mem + (a & 0xFFFC), &v, 4); }
void arm7_write16(uint32_t a, uint16_t v) { memcpy(mem + (a & 0xFFFE), &v, 2); }
void arm7_write8(uint32_t a, uint8_t v) { mem[a & 0xFFFF] = v; }
static const unsigned char prog[] = { %s };
int main(void) {
    memcpy(mem, prog, sizeof prog);
    arm7_reset();
    %s
    for (int i = 0; i < 200000 && !halted; i++) { arm7_run(1); cyc++; if (fiq_every && cyc %% fiq_every == 0) arm7.fiq_line = 1; }
    if (!halted) { printf("TIMEOUT pc=%%08X\n", arm7.r[15]); return 1; }
    for (int i = 0; i < 16; i++) printf("r%%d=%%08X\n", i, arm7.r[i]);
    return 0;
}
'''

def main():
    tmp = tempfile.mkdtemp(prefix="arm7_")
    ok = 0
    for name, src, checks, setup in TESTS:
        code, _ = ks.asm(src, 0)
        c = MAIN % (",".join(str(b) for b in code), setup)
        cf = os.path.join(tmp, name + ".c"); exe = os.path.join(tmp, name)
        open(cf, "w").write(c)
        subprocess.check_call(["cc", "-O1", "-w", "-I" + os.path.join(ROOT, "runtime/include"), cf,
                               os.path.join(ROOT, "runtime/src/arm7.c"), "-o", exe])
        out = subprocess.run([exe], capture_output=True, text=True).stdout
        regs = {}
        for line in out.split():
            if "=" in line and line.startswith("r"):
                k, v = line.split("="); regs[k] = int(v, 16)
        bad = []
        for k, v in checks.items():
            if k.endswith("_flags"):
                r = regs.get(k[:-6]); got = (r or 0) & 0xF0000000
            else:
                got = regs.get(k)
            if got != v: bad.append("%s=%s want %08X" % (k, "%08X" % got if got is not None else "?", v))
        if "TIMEOUT" in out: bad.append(out.strip())
        print("%-22s %s" % (name, "ok" if not bad else "FAIL " + "; ".join(bad)))
        ok += not bad
    print("%d/%d passed" % (ok, len(TESTS)))
    sys.exit(0 if ok == len(TESTS) else 1)

if __name__ == "__main__":
    main()

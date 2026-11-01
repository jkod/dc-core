/* dc-core: ARM7 interpreter for the AICA's sound CPU (ARMv4, ARM state).
 *
 * The sound driver a game uploads to sound RAM runs here. Memory accesses go through
 * arm7_bus (sound RAM + AICA registers, see aica.c). Only FIQ is wired on the AICA, but IRQ is
 * modelled too. Thumb is not supported (the AICA's ARM7DI has none).
 */
#include "dccore/arm7.h"
#include <string.h>
static inline int popcount16(uint32_t v) { int n = 0; for (v &= 0xFFFF; v; v &= v - 1) n++; return n; }

enum { M_USR = 0x10, M_FIQ = 0x11, M_IRQ = 0x12, M_SVC = 0x13, M_ABT = 0x17, M_UND = 0x1B, M_SYS = 0x1F };
#define F_N (1u << 31)
#define F_Z (1u << 30)
#define F_C (1u << 29)
#define F_V (1u << 28)
#define F_I (1u << 7)
#define F_F (1u << 6)

arm7_state arm7;

/* banked registers: index 0 usr/sys, 1 fiq, 2 irq, 3 svc, 4 abt, 5 und */
static int bank_of(uint32_t mode) {
    switch (mode & 0x1F) {
    case M_FIQ: return 1; case M_IRQ: return 2; case M_SVC: return 3; case M_ABT: return 4; case M_UND: return 5;
    default: return 0;
    }
}

static void switch_mode(uint32_t newmode) {
    arm7_state *s = &arm7;
    int ob = bank_of(s->cpsr), nb = bank_of(newmode);
    if (ob == nb) { s->cpsr = (s->cpsr & ~0x1Fu) | (newmode & 0x1F); return; }
    /* save current r8-r14 */
    if (ob == 1) { for (int i = 0; i < 5; i++) s->fiq_r8[i] = s->r[8 + i]; }
    else { for (int i = 0; i < 5; i++) s->usr_r8[i] = s->r[8 + i]; }
    s->bank_r13[ob] = s->r[13]; s->bank_r14[ob] = s->r[14];
    /* load new */
    if (nb == 1) { for (int i = 0; i < 5; i++) s->r[8 + i] = s->fiq_r8[i]; }
    else { for (int i = 0; i < 5; i++) s->r[8 + i] = s->usr_r8[i]; }
    s->r[13] = s->bank_r13[nb]; s->r[14] = s->bank_r14[nb];
    s->cpsr = (s->cpsr & ~0x1Fu) | (newmode & 0x1F);
}

static uint32_t *spsr_ptr(void) { int b = bank_of(arm7.cpsr); return b ? &arm7.spsr[b] : NULL; }

static void set_cpsr(uint32_t v) {
    if ((v & 0x1F) != (arm7.cpsr & 0x1F)) switch_mode(v & 0x1F);
    arm7.cpsr = v;
}

void arm7_reset(void) {
    memset(&arm7, 0, sizeof arm7);
    arm7.cpsr = M_SVC | F_I | F_F;
    arm7.r[15] = 0;
}

static void exception(uint32_t vector, uint32_t mode, uint32_t ret, int disable_fiq) {
    uint32_t old = arm7.cpsr;
    switch_mode(mode);
    arm7.spsr[bank_of(mode)] = old;
    arm7.r[14] = ret;
    arm7.cpsr |= F_I; if (disable_fiq) arm7.cpsr |= F_F;
    arm7.r[15] = vector;
}

static inline int cond_ok(uint32_t cond) {
    uint32_t f = arm7.cpsr;
    int n = !!(f & F_N), z = !!(f & F_Z), c = !!(f & F_C), v = !!(f & F_V);
    switch (cond) {
    case 0: return z; case 1: return !z; case 2: return c; case 3: return !c;
    case 4: return n; case 5: return !n; case 6: return v; case 7: return !v;
    case 8: return c && !z; case 9: return !c || z; case 10: return n == v; case 11: return n != v;
    case 12: return !z && n == v; case 13: return z || n != v; case 14: return 1;
    default: return 0;
    }
}

/* register read with the ARM pipeline: r15 reads as instruction + 8 (+12 for register-specified shifts) */
static inline uint32_t reg(int n, uint32_t pc8) { return n == 15 ? pc8 : arm7.r[n]; }

static uint32_t shift_op(uint32_t v, int type, uint32_t amt, int imm, int *carry) {
    int c = *carry;
    switch (type) {
    case 0: /* LSL */
        if (amt == 0) break;
        if (amt < 32) { c = (v >> (32 - amt)) & 1; v <<= amt; } else if (amt == 32) { c = v & 1; v = 0; } else { c = 0; v = 0; }
        break;
    case 1: /* LSR */
        if (amt == 0 && imm) amt = 32;
        if (amt == 0) break;
        if (amt < 32) { c = (v >> (amt - 1)) & 1; v >>= amt; } else if (amt == 32) { c = v >> 31; v = 0; } else { c = 0; v = 0; }
        break;
    case 2: /* ASR */
        if (amt == 0 && imm) amt = 32;
        if (amt == 0) break;
        if (amt < 32) { c = ((int32_t)v >> (amt - 1)) & 1; v = (uint32_t)((int32_t)v >> amt); }
        else { c = v >> 31; v = c ? 0xFFFFFFFFu : 0; }
        break;
    case 3: /* ROR / RRX */
        if (amt == 0 && imm) { int nc = v & 1; v = (v >> 1) | ((uint32_t)c << 31); c = nc; break; }
        if (amt == 0) break;
        amt &= 31;
        if (amt == 0) { c = v >> 31; break; }
        c = (v >> (amt - 1)) & 1; v = (v >> amt) | (v << (32 - amt));
        break;
    }
    *carry = c;
    return v;
}

static inline void set_nz(uint32_t r) { arm7.cpsr = (arm7.cpsr & ~(F_N | F_Z)) | (r & F_N) | (r ? 0 : F_Z); }

static void data_processing(uint32_t op, uint32_t pc8) {
    int opc = (op >> 21) & 15, s = (op >> 20) & 1, rn = (op >> 16) & 15, rd = (op >> 12) & 15;
    int carry = !!(arm7.cpsr & F_C);
    uint32_t op2;
    if (op & (1u << 25)) {
        uint32_t imm = op & 0xFF, rot = ((op >> 8) & 15) * 2;
        op2 = rot ? (imm >> rot) | (imm << (32 - rot)) : imm;
        if (rot) carry = op2 >> 31;
    } else {
        int rm = op & 15, type = (op >> 5) & 3;
        if (op & (1u << 4)) {                       /* shift by register */
            uint32_t amt = arm7.r[(op >> 8) & 15] & 0xFF;
            op2 = shift_op(rm == 15 ? pc8 + 4 : arm7.r[rm], type, amt, 0, &carry);
            pc8 += 4;                               /* Rn == r15 also reads +12 */
        } else op2 = shift_op(reg(rm, pc8), type, (op >> 7) & 31, 1, &carry);
    }
    uint32_t a = reg(rn, pc8), r = 0;
    int c_out = carry, v_out = !!(arm7.cpsr & F_V), write = 1, cin = !!(arm7.cpsr & F_C);
    uint64_t w;
    switch (opc) {
    case 0x0: r = a & op2; break;                                            /* AND */
    case 0x1: r = a ^ op2; break;                                            /* EOR */
    case 0x2: r = a - op2; c_out = a >= op2; v_out = ((a ^ op2) & (a ^ r)) >> 31; break;          /* SUB */
    case 0x3: r = op2 - a; c_out = op2 >= a; v_out = ((op2 ^ a) & (op2 ^ r)) >> 31; break;        /* RSB */
    case 0x4: w = (uint64_t)a + op2; r = (uint32_t)w; c_out = (int)(w >> 32); v_out = (~(a ^ op2) & (a ^ r)) >> 31; break; /* ADD */
    case 0x5: w = (uint64_t)a + op2 + cin; r = (uint32_t)w; c_out = (int)(w >> 32); v_out = (~(a ^ op2) & (a ^ r)) >> 31; break; /* ADC */
    case 0x6: w = (uint64_t)a - op2 - !cin; r = (uint32_t)w; c_out = ((uint64_t)a >= (uint64_t)op2 + !cin); v_out = ((a ^ op2) & (a ^ r)) >> 31; break; /* SBC */
    case 0x7: w = (uint64_t)op2 - a - !cin; r = (uint32_t)w; c_out = ((uint64_t)op2 >= (uint64_t)a + !cin); v_out = ((op2 ^ a) & (op2 ^ r)) >> 31; break; /* RSC */
    case 0x8: r = a & op2; write = 0; break;                                 /* TST */
    case 0x9: r = a ^ op2; write = 0; break;                                 /* TEQ */
    case 0xA: r = a - op2; c_out = a >= op2; v_out = ((a ^ op2) & (a ^ r)) >> 31; write = 0; break;  /* CMP */
    case 0xB: w = (uint64_t)a + op2; r = (uint32_t)w; c_out = (int)(w >> 32); v_out = (~(a ^ op2) & (a ^ r)) >> 31; write = 0; break; /* CMN */
    case 0xC: r = a | op2; break;                                            /* ORR */
    case 0xD: r = op2; break;                                                /* MOV */
    case 0xE: r = a & ~op2; break;                                           /* BIC */
    case 0xF: r = ~op2; break;                                               /* MVN */
    }
    if (write) arm7.r[rd] = r;
    if (s) {
        if (rd == 15 && write) { uint32_t *sp = spsr_ptr(); if (sp) set_cpsr(*sp); }   /* MOVS pc, lr etc. */
        else {
            set_nz(r);
            arm7.cpsr = (arm7.cpsr & ~(F_C | F_V)) | (c_out ? F_C : 0) | (v_out ? F_V : 0);
        }
    }
    if (rd == 15 && write) arm7.r[15] &= ~3u;
}

static void block_transfer(uint32_t op, uint32_t pc8) {
    int p = (op >> 24) & 1, u = (op >> 23) & 1, s = (op >> 22) & 1, w = (op >> 21) & 1, l = (op >> 20) & 1;
    int rn = (op >> 16) & 15;
    uint32_t list = op & 0xFFFF;
    int n = popcount16(list);
    uint32_t base = arm7.r[rn];
    uint32_t addr = u ? base : base - 4u * n;
    if (u ? p : !p) addr += 4;
    uint32_t newbase = u ? base + 4u * n : base - 4u * n;
    int user_bank = s && !(l && (list & 0x8000));
    uint32_t oldmode = arm7.cpsr & 0x1F;
    if (user_bank) switch_mode(M_USR);
    if (l) {
        if (w) arm7.r[rn] = newbase;
        for (int i = 0; i < 16; i++) if (list & (1u << i)) { arm7.r[i] = arm7_read32(addr); addr += 4; }
        if (list & 0x8000) {
            arm7.r[15] &= ~3u;
            if (s) { uint32_t *sp = spsr_ptr(); if (sp) set_cpsr(*sp); }
        }
    } else {
        int first = 1;
        for (int i = 0; i < 16; i++) if (list & (1u << i)) {
            uint32_t v = i == 15 ? pc8 + 4 : arm7.r[i];
            if (i == rn && !first && w) v = newbase;
            arm7_write32(addr, v); addr += 4; first = 0;
        }
        if (w) arm7.r[rn] = newbase;
    }
    if (user_bank) switch_mode(oldmode);
}

static void single_transfer(uint32_t op, uint32_t pc8) {
    int i = (op >> 25) & 1, p = (op >> 24) & 1, u = (op >> 23) & 1, b = (op >> 22) & 1, w = (op >> 21) & 1, l = (op >> 20) & 1;
    int rn = (op >> 16) & 15, rd = (op >> 12) & 15;
    uint32_t off;
    if (i) { int c = 0; off = shift_op(arm7.r[op & 15], (op >> 5) & 3, (op >> 7) & 31, 1, &c); }
    else off = op & 0xFFF;
    uint32_t base = reg(rn, pc8);
    uint32_t addr = p ? (u ? base + off : base - off) : base;
    if (l) {
        uint32_t v;
        if (b) v = arm7_read8(addr);
        else { v = arm7_read32(addr & ~3u); uint32_t rot = (addr & 3) * 8; if (rot) v = (v >> rot) | (v << (32 - rot)); }
        if (!p) addr = u ? base + off : base - off;
        if ((!p || w) && rn != rd) arm7.r[rn] = addr;
        arm7.r[rd] = v;
        if (rd == 15) arm7.r[15] &= ~3u;
    } else {
        uint32_t v = rd == 15 ? pc8 + 4 : arm7.r[rd];
        if (b) arm7_write8(addr, (uint8_t)v); else arm7_write32(addr & ~3u, v);
        if (!p) addr = u ? base + off : base - off;
        if (!p || w) arm7.r[rn] = addr;
    }
}

static void halfword_transfer(uint32_t op, uint32_t pc8) {
    int p = (op >> 24) & 1, u = (op >> 23) & 1, imm = (op >> 22) & 1, w = (op >> 21) & 1, l = (op >> 20) & 1;
    int rn = (op >> 16) & 15, rd = (op >> 12) & 15, sh = (op >> 5) & 3;
    uint32_t off = imm ? (((op >> 4) & 0xF0) | (op & 0xF)) : arm7.r[op & 15];
    uint32_t base = reg(rn, pc8);
    uint32_t addr = p ? (u ? base + off : base - off) : base;
    if (l) {
        uint32_t v;
        if (sh == 1) v = arm7_read16(addr & ~1u);
        else if (sh == 2) v = (uint32_t)(int8_t)arm7_read8(addr);
        else v = (uint32_t)(int16_t)arm7_read16(addr & ~1u);
        if (!p) addr = u ? base + off : base - off;
        if ((!p || w) && rn != rd) arm7.r[rn] = addr;
        arm7.r[rd] = v;
    } else {
        arm7_write16(addr & ~1u, (uint16_t)(rd == 15 ? pc8 + 4 : arm7.r[rd]));
        if (!p) addr = u ? base + off : base - off;
        if (!p || w) arm7.r[rn] = addr;
    }
}

static int step(void) {
    uint32_t pc = arm7.r[15];
    uint32_t op = arm7_read32(pc);
    uint32_t pc8 = pc + 8;
    arm7.r[15] = pc + 4;
    if (!cond_ok(op >> 28)) return 1;

    uint32_t top = (op >> 25) & 7;
    if (top == 5) {                                                   /* B / BL */
        int32_t off = (int32_t)(op << 8) >> 6;
        if (op & (1u << 24)) arm7.r[14] = pc + 4;
        arm7.r[15] = pc8 + (uint32_t)off;
        return 3;
    }
    if (top == 4) { block_transfer(op, pc8); return 2 + popcount16(op & 0xFFFF); }
    if (top == 2 || top == 3) {
        if (top == 3 && (op & 0x10)) { exception(0x04, M_UND, pc + 4, 0); return 3; }   /* undefined */
        single_transfer(op, pc8); return 3;
    }
    if (top == 7) {
        if ((op >> 24) & 1) { exception(0x08, M_SVC, pc + 4, 0); return 3; }        /* SWI */
        exception(0x04, M_UND, pc + 4, 0); return 3;                                  /* coprocessor */
    }
    if (top == 6) { exception(0x04, M_UND, pc + 4, 0); return 3; }
    /* top 0 / 1: data processing, multiplies, swaps, PSR transfers, halfwords, BX */
    if ((op & 0x0FFFFFF0u) == 0x012FFF10u) {                          /* BX (ARMv4T): treat as MOV pc, rm */
        arm7.r[15] = arm7.r[op & 15] & ~3u; return 3;
    }
    if ((op & 0x0FC000F0u) == 0x00000090u) {                          /* MUL / MLA */
        int rd = (op >> 16) & 15, rn = (op >> 12) & 15, rs = (op >> 8) & 15, rm = op & 15;
        uint32_t r = arm7.r[rm] * arm7.r[rs];
        if (op & (1u << 21)) r += arm7.r[rn];
        arm7.r[rd] = r;
        if (op & (1u << 20)) set_nz(r);
        return 4;
    }
    if ((op & 0x0F8000F0u) == 0x00800090u) {                          /* UMULL / UMLAL / SMULL / SMLAL */
        int hi = (op >> 16) & 15, lo = (op >> 12) & 15, rs = (op >> 8) & 15, rm = op & 15;
        uint64_t r = (op & (1u << 22)) ? (uint64_t)((int64_t)(int32_t)arm7.r[rm] * (int32_t)arm7.r[rs])
                                       : (uint64_t)arm7.r[rm] * arm7.r[rs];
        if (op & (1u << 21)) r += ((uint64_t)arm7.r[hi] << 32) | arm7.r[lo];
        arm7.r[lo] = (uint32_t)r; arm7.r[hi] = (uint32_t)(r >> 32);
        if (op & (1u << 20)) arm7.cpsr = (arm7.cpsr & ~(F_N | F_Z)) | ((uint32_t)(r >> 32) & F_N) | (r ? 0 : F_Z);
        return 5;
    }
    if ((op & 0x0FB00FF0u) == 0x01000090u) {                          /* SWP / SWPB */
        int rn = (op >> 16) & 15, rd = (op >> 12) & 15, rm = op & 15;
        uint32_t a = arm7.r[rn];
        if (op & (1u << 22)) { uint32_t t = arm7_read8(a); arm7_write8(a, (uint8_t)arm7.r[rm]); arm7.r[rd] = t; }
        else { uint32_t t = arm7_read32(a & ~3u); uint32_t rot = (a & 3) * 8; if (rot) t = (t >> rot) | (t << (32 - rot)); arm7_write32(a & ~3u, arm7.r[rm]); arm7.r[rd] = t; }
        return 4;
    }
    if ((op & 0x0E000090u) == 0x00000090u && (op & 0x60)) { halfword_transfer(op, pc8); return 3; }
    if ((op & 0x0FBF0FFFu) == 0x010F0000u) {                          /* MRS */
        int rd = (op >> 12) & 15;
        if (op & (1u << 22)) { uint32_t *sp = spsr_ptr(); arm7.r[rd] = sp ? *sp : arm7.cpsr; }
        else arm7.r[rd] = arm7.cpsr;
        return 1;
    }
    if ((op & 0x0DB0F000u) == 0x0120F000u) {                          /* MSR */
        uint32_t v;
        if (op & (1u << 25)) { uint32_t imm = op & 0xFF, rot = ((op >> 8) & 15) * 2; v = rot ? (imm >> rot) | (imm << (32 - rot)) : imm; }
        else v = arm7.r[op & 15];
        uint32_t mask = 0;
        if (op & (1u << 19)) mask |= 0xFF000000u;
        if (op & (1u << 16)) mask |= 0x000000FFu;
        if (op & (1u << 22)) { uint32_t *sp = spsr_ptr(); if (sp) *sp = (*sp & ~mask) | (v & mask); }
        else {
            if ((arm7.cpsr & 0x1F) == M_USR) mask &= 0xFF000000u;
            set_cpsr((arm7.cpsr & ~mask) | (v & mask));
        }
        return 1;
    }
    data_processing(op, pc8);
    return 1;
}

/* Idle loops: a backward jump to the same place with every register unchanged and no memory
 * changed since the last time spins on something only an interrupt, a timer or the SH-4 can
 * change, none of which happen inside this slice: give up the rest of it. */
unsigned arm7_writes;                  /* counted by the bus (aica.c) */
static struct { uint32_t pc, r[15], cpsr; unsigned writes; int valid; } idle;
int arm7_run(int cycles) {
    int done = 0;
    idle.valid = 0;
    while (done < cycles) {
        if (arm7.fiq_line && !(arm7.cpsr & F_F)) exception(0x1C, M_FIQ, arm7.r[15] + 4, 1);
        else if (arm7.irq_line && !(arm7.cpsr & F_I)) exception(0x18, M_IRQ, arm7.r[15] + 4, 0);
        uint32_t pc = arm7.r[15];
        done += step();
        uint32_t to = arm7.r[15];
        if (to < pc && pc - to <= 0x1000) {                    /* backward branch (a loop) */
            if (idle.valid && idle.pc == to && idle.writes == arm7_writes && idle.cpsr == arm7.cpsr &&
                !memcmp(idle.r, arm7.r, sizeof idle.r))
                return cycles;                                  /* spinning: done for this slice */
            idle.pc = to; idle.cpsr = arm7.cpsr; idle.writes = arm7_writes;
            memcpy(idle.r, arm7.r, sizeof idle.r); idle.valid = 1;
        }
    }
    return done;
}

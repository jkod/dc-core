/* dc-core: SH-4 interpreter for code that has no recompiled translation.
 *
 * dc_call() falls back to it when an address has no translation: code a game loads or builds at
 * run time (overlays, copied routines the alias finder cannot match) and anything the function
 * list missed outside the executable. Every case below is the run-time form of what
 * tools/recomp/sh4recomp.py emits for the same opcode, delay slots and calls included, so the two
 * agree instruction for instruction (tests run every recompiler test through both).
 *
 * Control flow follows the recompiled code's contract: calls (bsr/jsr/bsrf) nest through dc_call
 * and continue after the delay slot; rts/rte return 0; a jump to an address that has a translation
 * is returned to the dc_call trampoline, other jumps keep interpreting. */
#include "dccore/sh4.h"
#include "dccore/runtime.h"
#include <stdio.h>
#include <stdlib.h>

int dc_interp_enabled = 1;
int dc_interp_all;                         /* interpret everything except HLE (DC_INTERP_ALL=1) */

void dc_interp_illegal(sh4ctx *c, uint32_t pc);   /* dispatch.c: miss handler or abort */

#define R(n) c->r[n]
#define FR(n) c->fr.f[n]
#define FU(n) c->fr.u[n]
#define PR_ (c->fpscr & FPSCR_PR)
#define SZ_ (c->fpscr & FPSCR_SZ)
static inline int32_t s8(uint32_t v) { return (int8_t)v; }
static inline int32_t s12(uint32_t v) { return (v & 0x800) ? (int32_t)v - 0x1000 : (int32_t)v; }

/* FPU group (top nibble F). 1 = executed, -1 = illegal */
static int fpu(sh4ctx *c, uint32_t n, uint32_t m, uint32_t lo) {
    switch (lo) {
    case 0x0: if (PR_) sh4_set_dr(c, n & 0xE, sh4_get_dr(c, n & 0xE) + sh4_get_dr(c, m & 0xE)); else FR(n) += FR(m); return 1;
    case 0x1: if (PR_) sh4_set_dr(c, n & 0xE, sh4_get_dr(c, n & 0xE) - sh4_get_dr(c, m & 0xE)); else FR(n) -= FR(m); return 1;
    case 0x2: if (PR_) sh4_set_dr(c, n & 0xE, sh4_get_dr(c, n & 0xE) * sh4_get_dr(c, m & 0xE)); else FR(n) *= FR(m); return 1;
    case 0x3: if (PR_) sh4_set_dr(c, n & 0xE, sh4_get_dr(c, n & 0xE) / sh4_get_dr(c, m & 0xE)); else FR(n) /= FR(m); return 1;
    case 0x4: c->t = PR_ ? sh4_get_dr(c, n & 0xE) == sh4_get_dr(c, m & 0xE) : FR(n) == FR(m); return 1;
    case 0x5: c->t = PR_ ? sh4_get_dr(c, n & 0xE) > sh4_get_dr(c, m & 0xE) : FR(n) > FR(m); return 1;
    case 0x6: case 0x8: case 0x9: {                              /* fmov @(r0,rm) / @rm / @rm+ */
        uint32_t a = lo == 0x6 ? R(m) + R(0) : R(m);
        if (SZ_) { int d = sh4_xd((int)n); c->fr.u[d] = dc_rd32(a); c->fr.u[d + 1] = dc_rd32(a + 4); }
        else FU(n) = dc_rd32(a);
        if (lo == 0x9) R(m) += SZ_ ? 8 : 4;
        return 1;
    }
    case 0x7: case 0xA: case 0xB: {                              /* fmov -> @(r0,rn) / @rn / @-rn */
        if (lo == 0xB) R(n) -= SZ_ ? 8 : 4;
        uint32_t a = lo == 0x7 ? R(n) + R(0) : R(n);
        if (SZ_) { int s = sh4_xd((int)m); dc_wr32(a, c->fr.u[s]); dc_wr32(a + 4, c->fr.u[s + 1]); }
        else dc_wr32(a, FU(m));
        return 1;
    }
    case 0xC:
        if (SZ_) { int d = sh4_xd((int)n), s = sh4_xd((int)m); uint32_t x0 = c->fr.u[s], x1 = c->fr.u[s + 1]; c->fr.u[d] = x0; c->fr.u[d + 1] = x1; }
        else FU(n) = FU(m);
        return 1;
    case 0xE: FR(n) = fmaf(FR(0), FR(m), FR(n)); return 1;
    case 0xD:
        switch (m) {
        case 0x0: FU(n) = c->fpul; return 1;
        case 0x1: c->fpul = FU(n); return 1;
        case 0x2: if (PR_) sh4_set_dr(c, n & 0xE, (double)(int32_t)c->fpul); else FR(n) = (float)(int32_t)c->fpul; return 1;
        case 0x3: c->fpul = PR_ ? sh4_ftrc_d(sh4_get_dr(c, n & 0xE)) : sh4_ftrc(FR(n)); return 1;
        case 0x4: if (PR_) c->fr.u[n & 0xE] ^= 0x80000000u; else FR(n) = -FR(n); return 1;
        case 0x5: if (PR_) c->fr.u[n & 0xE] &= 0x7FFFFFFFu; else FR(n) = fabsf(FR(n)); return 1;
        case 0x6: if (PR_) sh4_set_dr(c, n & 0xE, sqrt(sh4_get_dr(c, n & 0xE))); else FR(n) = sqrtf(FR(n)); return 1;
        case 0x7: FR(n) = 1.0f / sqrtf(FR(n)); return 1;
        case 0x8: FU(n) = 0x00000000u; return 1;
        case 0x9: FU(n) = 0x3F800000u; return 1;
        case 0xA: { float f; memcpy(&f, &c->fpul, 4); sh4_set_dr(c, n & 0xE, (double)f); return 1; }
        case 0xB: { float f = (float)sh4_get_dr(c, n & 0xE); memcpy(&c->fpul, &f, 4); return 1; }
        case 0xE: sh4_fipr(c, n & 0xC, (n & 3) << 2); return 1;
        case 0xF:
            if ((n & 1) == 0) { sh4_fsca(c, (int)n); return 1; }
            if ((n & 3) == 1) { sh4_ftrv(c, n & 0xC); return 1; }
            if (n == 0xB) { sh4_set_fpscr(c, c->fpscr ^ FPSCR_FR); return 1; }
            if (n == 0x3) { c->fpscr ^= FPSCR_SZ; return 1; }
            if (n == 0x7) { c->fpscr ^= FPSCR_PR; return 1; }
            return -1;
        }
        return -1;
    }
    return -1;
}

/* One non-branch instruction at pc. 1 = executed, 0 = a branch (not executed), -1 = illegal. */
static int step(sh4ctx *c, uint32_t pc, uint32_t op) {
    uint32_t n = (op >> 8) & 0xF, m = (op >> 4) & 0xF, lo = op & 0xF, lb = op & 0xFF;
    switch (op >> 12) {
    case 0x0:
        switch (lo) {
        case 0x2:
            if (m & 8) { R(n) = c->rbank[m & 7]; return 1; }
            switch (m) { case 0: R(n) = sh4_get_sr(c); return 1; case 1: R(n) = c->gbr; return 1; case 2: R(n) = c->vbr; return 1;
                         case 3: R(n) = c->ssr; return 1; case 4: R(n) = c->spc; return 1; }
            return -1;
        case 0x3:
            if (m == 0 || m == 2) return 0;                      /* bsrf / braf */
            if (m == 8) { dc_hw_pref(R(n)); return 1; }
            if (m == 9 || m == 0xA || m == 0xB) return 1;        /* cache ops */
            if (m == 0xC) { dc_wr32(R(n), R(0)); return 1; }     /* movca.l */
            return -1;
        case 0x4: dc_wr8(R(n) + R(0), (uint8_t)R(m)); return 1;
        case 0x5: dc_wr16(R(n) + R(0), (uint16_t)R(m)); return 1;
        case 0x6: dc_wr32(R(n) + R(0), R(m)); return 1;
        case 0x7: c->macl = R(n) * R(m); return 1;
        case 0x8:
            if (n != 0) return -1;
            switch (m) { case 0: c->t = 0; return 1; case 1: c->t = 1; return 1; case 2: c->mach = c->macl = 0; return 1;
                         case 4: c->s = 0; return 1; case 5: c->s = 1; return 1; }
            return -1;
        case 0x9:
            if (op == 0x0009) return 1;
            if (op == 0x0019) { c->m = c->q = c->t = 0; return 1; }
            if (m == 2) { R(n) = c->t; return 1; }
            return -1;
        case 0xA:
            switch (m) { case 0: R(n) = c->mach; return 1; case 1: R(n) = c->macl; return 1; case 2: R(n) = c->pr; return 1;
                         case 3: R(n) = c->sgr; return 1; case 5: R(n) = c->fpul; return 1; case 6: R(n) = c->fpscr; return 1;
                         case 0xF: R(n) = c->dbr; return 1; }
            return -1;
        case 0xB:
            if (op == 0x000B || op == 0x002B) return 0;          /* rts / rte */
            if (op == 0x001B) { dc_trap(c, pc, "sleep"); return 1; }
            return -1;
        case 0xC: R(n) = (uint32_t)(int8_t)dc_rd8(R(m) + R(0)); return 1;
        case 0xD: R(n) = (uint32_t)(int16_t)dc_rd16(R(m) + R(0)); return 1;
        case 0xE: R(n) = dc_rd32(R(m) + R(0)); return 1;
        case 0xF: sh4_macl(c, (int)n, (int)m); return 1;
        }
        return -1;
    case 0x1: dc_wr32(R(n) + lo * 4, R(m)); return 1;
    case 0x2:
        switch (lo) {
        case 0x0: dc_wr8(R(n), (uint8_t)R(m)); return 1;
        case 0x1: dc_wr16(R(n), (uint16_t)R(m)); return 1;
        case 0x2: dc_wr32(R(n), R(m)); return 1;
        case 0x4: { uint32_t v = R(m); R(n) -= 1; dc_wr8(R(n), (uint8_t)v); return 1; }
        case 0x5: { uint32_t v = R(m); R(n) -= 2; dc_wr16(R(n), (uint16_t)v); return 1; }
        case 0x6: { uint32_t v = R(m); R(n) -= 4; dc_wr32(R(n), v); return 1; }
        case 0x7: c->q = R(n) >> 31; c->m = R(m) >> 31; c->t = c->q ^ c->m; return 1;
        case 0x8: c->t = (R(n) & R(m)) == 0; return 1;
        case 0x9: R(n) &= R(m); return 1;
        case 0xA: R(n) ^= R(m); return 1;
        case 0xB: R(n) |= R(m); return 1;
        case 0xC: { uint32_t x = R(n) ^ R(m); c->t = !(x & 0xFF) || !(x & 0xFF00) || !(x & 0xFF0000) || !(x & 0xFF000000u); return 1; }
        case 0xD: R(n) = (R(n) >> 16) | (R(m) << 16); return 1;
        case 0xE: c->macl = (uint32_t)(uint16_t)R(n) * (uint16_t)R(m); return 1;
        case 0xF: c->macl = (uint32_t)((int32_t)(int16_t)R(n) * (int16_t)R(m)); return 1;
        }
        return -1;
    case 0x3:
        switch (lo) {
        case 0x0: c->t = R(n) == R(m); return 1;
        case 0x2: c->t = R(n) >= R(m); return 1;
        case 0x3: c->t = (int32_t)R(n) >= (int32_t)R(m); return 1;
        case 0x4: sh4_div1(c, (int)n, (int)m); return 1;
        case 0x5: { uint64_t p = (uint64_t)R(n) * R(m); c->mach = (uint32_t)(p >> 32); c->macl = (uint32_t)p; return 1; }
        case 0x6: c->t = R(n) > R(m); return 1;
        case 0x7: c->t = (int32_t)R(n) > (int32_t)R(m); return 1;
        case 0x8: R(n) -= R(m); return 1;
        case 0xA: { uint32_t a = R(n), b = R(m), r = a - b - c->t; c->t = (a < b) || (a - b < c->t); R(n) = r; return 1; }
        case 0xB: { int32_t a = (int32_t)R(n), b = (int32_t)R(m); int32_t r = (int32_t)((uint32_t)a - (uint32_t)b); c->t = ((a ^ b) & (a ^ r)) >> 31 & 1; R(n) = (uint32_t)r; return 1; }
        case 0xC: R(n) += R(m); return 1;
        case 0xD: { int64_t p = (int64_t)(int32_t)R(n) * (int32_t)R(m); c->mach = (uint32_t)((uint64_t)p >> 32); c->macl = (uint32_t)p; return 1; }
        case 0xE: { uint32_t a = R(n), b = R(m), r = a + b + c->t; c->t = (r < a) || (r == a && c->t); R(n) = r; return 1; }
        case 0xF: { int32_t a = (int32_t)R(n), b = (int32_t)R(m); int32_t r = (int32_t)((uint32_t)a + (uint32_t)b); c->t = (~(a ^ b) & (a ^ r)) >> 31 & 1; R(n) = (uint32_t)r; return 1; }
        }
        return -1;
    case 0x4:
        if (lo == 0xC) { int32_t s = (int32_t)R(m); uint32_t v = R(n); if (s >= 0) R(n) = v << (s & 31); else if ((s & 31) == 0) R(n) = (uint32_t)((int32_t)v >> 31); else R(n) = (uint32_t)((int32_t)v >> ((-s) & 31)); return 1; }
        if (lo == 0xD) { int32_t s = (int32_t)R(m); uint32_t v = R(n); if (s >= 0) R(n) = v << (s & 31); else if ((s & 31) == 0) R(n) = 0; else R(n) = v >> ((-s) & 31); return 1; }
        if (lo == 0xF) { sh4_macw(c, (int)n, (int)m); return 1; }
        if (lb == 0x0B || lb == 0x2B) return 0;                  /* jsr / jmp */
        if ((lb & 0x8F) == 0x83) { R(n) -= 4; dc_wr32(R(n), c->rbank[m & 7]); return 1; }
        if ((lb & 0x8F) == 0x87) { c->rbank[m & 7] = dc_rd32(R(n)); R(n) += 4; return 1; }
        if ((lb & 0x8F) == 0x8E) { c->rbank[m & 7] = R(n); return 1; }
#define PUSH(src) { R(n) -= 4; dc_wr32(R(n), src); return 1; }
#define POP(dst) { dst = dc_rd32(R(n)); R(n) += 4; return 1; }
        switch (lb) {
        case 0x00: c->t = R(n) >> 31; R(n) <<= 1; return 1;
        case 0x01: c->t = R(n) & 1; R(n) >>= 1; return 1;
        case 0x02: PUSH(c->mach) case 0x12: PUSH(c->macl) case 0x22: PUSH(c->pr)
        case 0x03: PUSH(sh4_get_sr(c)) case 0x13: PUSH(c->gbr) case 0x23: PUSH(c->vbr)
        case 0x32: PUSH(c->sgr) case 0x33: PUSH(c->ssr) case 0x43: PUSH(c->spc)
        case 0x52: PUSH(c->fpul) case 0x62: PUSH(c->fpscr) case 0xF2: PUSH(c->dbr)
        case 0x04: c->t = R(n) >> 31; R(n) = (R(n) << 1) | c->t; return 1;
        case 0x05: c->t = R(n) & 1; R(n) = (R(n) >> 1) | (c->t << 31); return 1;
        case 0x06: POP(c->mach) case 0x16: POP(c->macl) case 0x26: POP(c->pr)
        case 0x07: sh4_set_sr(c, dc_rd32(R(n))); R(n) += 4; return 1;
        case 0x17: POP(c->gbr) case 0x27: POP(c->vbr) case 0x37: POP(c->ssr) case 0x47: POP(c->spc)
        case 0x56: POP(c->fpul) case 0xF6: POP(c->dbr)
        case 0x66: sh4_set_fpscr(c, dc_rd32(R(n))); R(n) += 4; return 1;
        case 0x08: R(n) <<= 2; return 1;  case 0x09: R(n) >>= 2; return 1;
        case 0x18: R(n) <<= 8; return 1;  case 0x19: R(n) >>= 8; return 1;
        case 0x28: R(n) <<= 16; return 1; case 0x29: R(n) >>= 16; return 1;
        case 0x0A: c->mach = R(n); return 1; case 0x1A: c->macl = R(n); return 1; case 0x2A: c->pr = R(n); return 1;
        case 0x5A: c->fpul = R(n); return 1; case 0x6A: sh4_set_fpscr(c, R(n)); return 1; case 0xFA: c->dbr = R(n); return 1;
        case 0x0E: sh4_set_sr(c, R(n)); return 1; case 0x1E: c->gbr = R(n); return 1; case 0x2E: c->vbr = R(n); return 1;
        case 0x3E: c->ssr = R(n); return 1; case 0x4E: c->spc = R(n); return 1;
        case 0x10: R(n) -= 1; c->t = R(n) == 0; return 1;
        case 0x11: c->t = (int32_t)R(n) >= 0; return 1;
        case 0x15: c->t = (int32_t)R(n) > 0; return 1;
        case 0x1B: { uint8_t v = dc_rd8(R(n)); c->t = v == 0; dc_wr8(R(n), v | 0x80); return 1; }
        case 0x20: c->t = R(n) >> 31; R(n) <<= 1; return 1;
        case 0x21: c->t = R(n) & 1; R(n) = (uint32_t)((int32_t)R(n) >> 1); return 1;
        case 0x24: { uint32_t o = c->t; c->t = R(n) >> 31; R(n) = (R(n) << 1) | o; return 1; }
        case 0x25: { uint32_t o = c->t; c->t = R(n) & 1; R(n) = (R(n) >> 1) | (o << 31); return 1; }
        }
#undef PUSH
#undef POP
        return -1;
    case 0x5: R(n) = dc_rd32(R(m) + lo * 4); return 1;
    case 0x6:
        if ((lo == 4 || lo == 5 || lo == 6) && n == m) {         /* mov.x @Rm+,Rn, n == m: the load wins */
            R(n) = lo == 4 ? (uint32_t)(int8_t)dc_rd8(R(n)) : lo == 5 ? (uint32_t)(int16_t)dc_rd16(R(n)) : dc_rd32(R(n));
            return 1;
        }
        switch (lo) {
        case 0x0: R(n) = (uint32_t)(int8_t)dc_rd8(R(m)); return 1;
        case 0x1: R(n) = (uint32_t)(int16_t)dc_rd16(R(m)); return 1;
        case 0x2: R(n) = dc_rd32(R(m)); return 1;
        case 0x3: R(n) = R(m); return 1;
        case 0x4: { uint32_t a = R(m); R(m) += 1; R(n) = (uint32_t)(int8_t)dc_rd8(a); return 1; }
        case 0x5: { uint32_t a = R(m); R(m) += 2; R(n) = (uint32_t)(int16_t)dc_rd16(a); return 1; }
        case 0x6: { uint32_t a = R(m); R(m) += 4; R(n) = dc_rd32(a); return 1; }
        case 0x7: R(n) = ~R(m); return 1;
        case 0x8: R(n) = (R(m) & 0xFFFF0000u) | ((R(m) & 0xFF) << 8) | ((R(m) >> 8) & 0xFF); return 1;
        case 0x9: R(n) = (R(m) >> 16) | (R(m) << 16); return 1;
        case 0xA: { uint32_t t0 = 0 - R(m); uint32_t r = t0 - c->t; c->t = (0 < t0) || (t0 < r); R(n) = r; return 1; }
        case 0xB: R(n) = 0 - R(m); return 1;
        case 0xC: R(n) = R(m) & 0xFF; return 1;
        case 0xD: R(n) = R(m) & 0xFFFF; return 1;
        case 0xE: R(n) = (uint32_t)(int8_t)R(m); return 1;
        case 0xF: R(n) = (uint32_t)(int16_t)R(m); return 1;
        }
        return -1;
    case 0x7: R(n) += (uint32_t)s8(lb); return 1;
    case 0x8:
        switch (n) {
        case 0x0: dc_wr8(R(m) + lo, (uint8_t)R(0)); return 1;
        case 0x1: dc_wr16(R(m) + lo * 2, (uint16_t)R(0)); return 1;
        case 0x4: R(0) = (uint32_t)(int8_t)dc_rd8(R(m) + lo); return 1;
        case 0x5: R(0) = (uint32_t)(int16_t)dc_rd16(R(m) + lo * 2); return 1;
        case 0x8: c->t = R(0) == (uint32_t)s8(lb); return 1;
        case 0x9: case 0xB: case 0xD: case 0xF: return 0;        /* bt / bf / bt/s / bf/s */
        }
        return -1;
    case 0x9: R(n) = (uint32_t)(int16_t)dc_rd16(pc + 4 + lb * 2); return 1;
    case 0xA: case 0xB: return 0;                                /* bra / bsr */
    case 0xC: {
        uint32_t g = c->gbr;
        switch (n) {
        case 0x0: dc_wr8(g + lb, (uint8_t)R(0)); return 1;
        case 0x1: dc_wr16(g + lb * 2, (uint16_t)R(0)); return 1;
        case 0x2: dc_wr32(g + lb * 4, R(0)); return 1;
        case 0x3: dc_trap(c, pc, "trapa"); return 1;
        case 0x4: R(0) = (uint32_t)(int8_t)dc_rd8(g + lb); return 1;
        case 0x5: R(0) = (uint32_t)(int16_t)dc_rd16(g + lb * 2); return 1;
        case 0x6: R(0) = dc_rd32(g + lb * 4); return 1;
        case 0x7: R(0) = ((pc + 4) & ~3u) + lb * 4; return 1;
        case 0x8: c->t = (R(0) & lb) == 0; return 1;
        case 0x9: R(0) &= lb; return 1;
        case 0xA: R(0) ^= lb; return 1;
        case 0xB: R(0) |= lb; return 1;
        case 0xC: c->t = (dc_rd8(R(0) + g) & lb) == 0; return 1;
        case 0xD: { uint32_t a = R(0) + g; dc_wr8(a, dc_rd8(a) & lb); return 1; }
        case 0xE: { uint32_t a = R(0) + g; dc_wr8(a, dc_rd8(a) ^ lb); return 1; }
        case 0xF: { uint32_t a = R(0) + g; dc_wr8(a, dc_rd8(a) | lb); return 1; }
        }
        return -1;
    }
    case 0xD: R(n) = dc_rd32(((pc + 4) & ~3u) + lb * 4); return 1;
    case 0xE: R(n) = (uint32_t)s8(lb); return 1;
    case 0xF: return fpu(c, n, m, lo);
    }
    return -1;
}

/* the delay slot of the branch at pc */
static void slot(sh4ctx *c, uint32_t pc) {
    uint32_t sp = pc + 2;
    int k = step(c, sp, dc_rd16(sp));
    if (k == 0) dc_trap(c, sp, "branch in delay slot");
    else if (k < 0) dc_trap(c, sp, "illegal instruction in delay slot");
}

/* a jump: hand translated targets back to the trampoline, keep interpreting the rest */
static int leave_to(uint32_t t) { return !dc_interp_all && dc_lookup(t) != NULL; }

uint32_t dc_interp(sh4ctx *c, uint32_t pc) {
    for (;;) {
        uint32_t op = dc_rd16(pc);
        int k = step(c, pc, op);
        if (k > 0) { pc += 2; continue; }
        if (k < 0) { dc_interp_illegal(c, pc); return 0; }
        uint32_t top = op >> 12, n = (op >> 8) & 0xF, lb = op & 0xFF, t;
        if (top == 0x8) {                                        /* conditional branches */
            int cc = (n == 0x9 || n == 0xD) ? (int)c->t : !c->t;
            t = pc + 4 + (uint32_t)(s8(lb) * 2);
            uint32_t next = pc + 2;
            if (n == 0xD || n == 0xF) { slot(c, pc); next = pc + 4; }
            if (!cc) { pc = next; continue; }
        } else if (top == 0xA) { t = pc + 4 + (uint32_t)(s12(op & 0xFFF) * 2); slot(c, pc); }
        else if (top == 0x0 && (op & 0xFF) == 0x23) { t = R(n) + pc + 4; slot(c, pc); }          /* braf */
        else if (top == 0x4 && lb == 0x2B) { t = R(n); slot(c, pc); }                             /* jmp */
        else if (op == 0x000B) { slot(c, pc); return 0; }                                         /* rts */
        else if (op == 0x002B) { sh4_set_sr(c, c->ssr); slot(c, pc); return 0; }                  /* rte */
        else {                                                   /* calls: bsr / jsr / bsrf */
            if (top == 0xB) t = pc + 4 + (uint32_t)(s12(op & 0xFFF) * 2);
            else if (top == 0x4) t = R(n);
            else t = R(n) + pc + 4;
            uint32_t ret = pc + 4;
            c->pr = ret;
            slot(c, pc);
            if (t == dc_coro_setjmp_addr) {                      /* the game's setjmp (dccore/coro.h) */
                jmp_buf *jb = dc_coro_setjmp_slot(c);
                dc_call(c, t);
                dc_coro_setjmp_done(c, jb);
                /* resumed by a guest longjmp: this frame's later calls changed every local, but the
                 * guest restore left pr = the return address of this setjmp call */
                if (jb && DC_SETJMP(*jb)) { pc = c->pr; continue; }
            } else dc_call(c, t);
            pc = ret;
            continue;
        }
        /* jumps */
        if (t <= pc) DC_TICK(c, 16);                             /* loop: let time pass / interrupts in */
        if (leave_to(t)) return t;
        pc = t;
    }
}

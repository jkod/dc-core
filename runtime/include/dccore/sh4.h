/* dc-core: SH-4 CPU state and helpers used by statically recompiled Dreamcast code.
 *
 * Recompiled functions have the signature `void f_XXXXXXXX(sh4ctx *c)` and keep the
 * original register-level semantics: arguments in r4..r7 / fr4..fr11, result in r0 / fr0,
 * stack in r15, return address in pr. Memory is reached only through dc_rd* / dc_wr*.
 */
#ifndef DCCORE_SH4_H
#define DCCORE_SH4_H

#include <stdint.h>
#include <string.h>
#include <math.h>
#include <setjmp.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sh4ctx {
    uint32_t r[16];
    uint32_t rbank[8];          /* the other R0..R7 bank (only touched by ldc/stc Rn_BANK) */
    uint32_t t, s, q, m;        /* SR flag bits kept apart for speed */
    uint32_t sr_rest;           /* SR without T/S/Q/M */
    uint32_t gbr, vbr, ssr, spc, sgr, dbr;
    uint32_t mach, macl, pr, fpul, fpscr;
    union { float f[32]; uint32_t u[32]; } fr;   /* [0..15] current FR bank, [16..31] XF bank */
    uint64_t cycles;            /* rough cycle counter, drives the hardware model */
    uint32_t entry;             /* address a function is being entered at (set by dc_call) */
} sh4ctx;

#define FPSCR_PR (1u << 19)
#define FPSCR_SZ (1u << 20)
#define FPSCR_FR (1u << 21)

/* ---------------- memory ---------------- */
/* Main RAM: 16 MB at 0x0C000000 (P0/U0), mirrored in P1 0x8C.., P2 0xAC.., P3 0xCC... */
extern uint8_t *dc_ram;

uint8_t  dc_hw_rd8 (uint32_t a);
uint16_t dc_hw_rd16(uint32_t a);
uint32_t dc_hw_rd32(uint32_t a);
void     dc_hw_wr8 (uint32_t a, uint8_t v);
void     dc_hw_wr16(uint32_t a, uint16_t v);
void     dc_hw_wr32(uint32_t a, uint32_t v);
void     dc_hw_pref(uint32_t a);          /* store-queue flush etc. */

#define DC_IS_RAM(a) (((a) & 0x1C000000u) == 0x0C000000u)
/* DC_RAM_32MB (CMake DCCORE_RAM_32MB, Windows CE games): 32 MB of RAM at 0x0C000000, the user
 * address space's whole size, so that every user address has a physical one (0x0C000000 + va) for
 * the drivers' DMA. Otherwise the console's 16 MB, mirrored at 0x0D000000. */
#ifdef DC_RAM_32MB
#define DC_RAM_SIZE 0x02000000u
#else
#define DC_RAM_SIZE 0x01000000u
#endif
#define DC_RAM_OFS(a) ((a) & (DC_RAM_SIZE - 1u))

/* User address space (Windows CE games, see dccore/wince.h): when dc_uram is set, the 32 MB of
 * virtual addresses 0x00000000..0x01FFFFFF -- and their P1 fold 0x80000000..0x81FFFFFF, which is
 * how recompiled code addresses its own instructions -- are a flat 32 MB block of host memory.
 * Off (NULL) for ordinary Dreamcast games; the test sits after the RAM test, so it costs them
 * nothing on RAM accesses. */
extern uint8_t *dc_uram;
#define DC_URAM_SIZE 0x02000000u
#define DC_IS_URAM(a) (((a) & 0x7E000000u) == 0 && dc_uram)
#define DC_URAM_OFS(a) ((a) & 0x01FFFFFFu)

#ifdef DC_WATCH
extern uint32_t dc_rwatch_lo, dc_rwatch_hi;
void dc_rwatch_hit(uint32_t a, int size);
#define DC_RWATCHED(a, n) do { uint32_t o_ = DC_RAM_OFS(a); if (o_ + (n) > dc_rwatch_lo && o_ < dc_rwatch_hi) dc_rwatch_hit(a, n); } while (0)
#else
#define DC_RWATCHED(a, n) ((void)0)
#endif
/* Memory accesses: the RAM test inline, everything else (hardware registers, a Windows CE game's user
 * space unless DC_RAM_32MB) through one out-of-line dc_mem_rd* / dc_mem_wr* per size. Inlining is the
 * compiler's choice: forcing it (always_inline) made GCC need more than 7 GB for one of Sega Rally 2's
 * generated files and Visual C++ builds much slower, and gained nothing on the Switch (-O2 inlines
 * these anyway); only -O1 PC builds ran faster, which don't need it. */
#define DC_FORCE_INLINE static inline
#if defined(_MSC_VER)
#define DC_LIKELY(x) (x)
#else
#define DC_LIKELY(x) __builtin_expect(!!(x), 1)
#endif
uint8_t  dc_mem_rd8 (uint32_t a); uint16_t dc_mem_rd16(uint32_t a); uint32_t dc_mem_rd32(uint32_t a);
void dc_mem_wr8(uint32_t a, uint8_t v); void dc_mem_wr16(uint32_t a, uint16_t v); void dc_mem_wr32(uint32_t a, uint32_t v);
#ifdef DC_RAM_32MB
/* Windows CE games live in the user space: keep that test inline too */
#define DC_URAM_INLINE_RD(a, T) if (DC_IS_URAM(a)) { T v; memcpy(&v, dc_uram + DC_URAM_OFS(a), sizeof(T)); return v; }
#define DC_URAM_INLINE_WR(a, v) if (DC_IS_URAM(a)) { memcpy(dc_uram + DC_URAM_OFS(a), &v, sizeof(v)); return; }
#else
#define DC_URAM_INLINE_RD(a, T)
#define DC_URAM_INLINE_WR(a, v)
#endif
DC_FORCE_INLINE uint8_t  dc_rd8 (uint32_t a) { if (DC_LIKELY(DC_IS_RAM(a))) { DC_RWATCHED(a, 1); return dc_ram[DC_RAM_OFS(a)]; } DC_URAM_INLINE_RD(a, uint8_t) return dc_mem_rd8(a); }
DC_FORCE_INLINE uint16_t dc_rd16(uint32_t a) { if (DC_LIKELY(DC_IS_RAM(a))) { uint16_t v; DC_RWATCHED(a, 2); memcpy(&v, dc_ram + DC_RAM_OFS(a), 2); return v; } DC_URAM_INLINE_RD(a, uint16_t) return dc_mem_rd16(a); }
DC_FORCE_INLINE uint32_t dc_rd32(uint32_t a) { if (DC_LIKELY(DC_IS_RAM(a))) { uint32_t v; DC_RWATCHED(a, 4); memcpy(&v, dc_ram + DC_RAM_OFS(a), 4); return v; } DC_URAM_INLINE_RD(a, uint32_t) return dc_mem_rd32(a); }
#ifdef DC_WATCH
/* debug builds (-DDC_WATCH): every RAM write inside [dc_watch_lo, dc_watch_hi) is reported */
extern uint32_t dc_watch_lo, dc_watch_hi;
void dc_watch_hit(uint32_t a, uint32_t v, int size);
#define DC_WATCHED(a, v, n) do { uint32_t o_ = DC_RAM_OFS(a); if (o_ + (n) > dc_watch_lo && o_ < dc_watch_hi) dc_watch_hit(a, v, n); } while (0)
#else
#define DC_WATCHED(a, v, n) ((void)0)
#endif
DC_FORCE_INLINE void dc_wr8 (uint32_t a, uint8_t v)  { if (DC_LIKELY(DC_IS_RAM(a))) { DC_WATCHED(a, v, 1); dc_ram[DC_RAM_OFS(a)] = v; return; } DC_URAM_INLINE_WR(a, v) dc_mem_wr8(a, v); }
DC_FORCE_INLINE void dc_wr16(uint32_t a, uint16_t v) { if (DC_LIKELY(DC_IS_RAM(a))) { DC_WATCHED(a, v, 2); memcpy(dc_ram + DC_RAM_OFS(a), &v, 2); return; } DC_URAM_INLINE_WR(a, v) dc_mem_wr16(a, v); }
DC_FORCE_INLINE void dc_wr32(uint32_t a, uint32_t v) { if (DC_LIKELY(DC_IS_RAM(a))) { DC_WATCHED(a, v, 4); memcpy(dc_ram + DC_RAM_OFS(a), &v, 4); return; } DC_URAM_INLINE_WR(a, v) dc_mem_wr32(a, v); }

DC_FORCE_INLINE float dc_rdf(uint32_t a) { uint32_t u = dc_rd32(a); float f; memcpy(&f, &u, 4); return f; }
DC_FORCE_INLINE void  dc_wrf(uint32_t a, float f) { uint32_t u; memcpy(&u, &f, 4); dc_wr32(a, u); }

/* ---------------- dispatch ---------------- */
/* A recompiled (or HLE) function: returns 0 when the guest returns, or the next address to run
 * for a tail jump. HLE replacements normally return 0. */
typedef uint32_t (*sh4fn)(sh4ctx *c);
/* Call guest code at `addr` and run until it returns. */
void dc_call(sh4ctx *c, uint32_t addr);
/* a call to a fixed address (bsr, or jsr through a register just loaded with a constant): each call site
 * keeps the function it resolved to, so the dispatcher's lookup, address folding and alias scan run once
 * per site instead of on every call. dc_dispatch_gen changes whenever what an address resolves to can
 * change (an HLE hook, an alias, another executable, a call hook), and the site resolves again. */
typedef struct { uint32_t gen, entry; sh4fn fn; } dc_site;
extern uint32_t dc_dispatch_gen;
int  dc_site_resolve(dc_site *s, uint32_t addr);
void dc_call_ticked(sh4ctx *c, uint32_t addr);      /* dc_call with the first call's time already charged */
/* Called for things the recompiler cannot do statically (trapa, sleep, rte, unknown opcodes). */
void dc_trap(sh4ctx *c, uint32_t pc, const char *what);
uint32_t dc_mid_entry(sh4ctx *c, uint32_t pc);   /* entered where sparse labels left no label */

/* Calls to a game's setjmp (recompiler --setjmp, runtime in coro.c): after the guest routine
 * has saved its registers, a host setjmp in this frame lets the guest longjmp come back here. */
#if defined(__APPLE__)
#define DC_SETJMP(b) _setjmp(b)
#define DC_LONGJMP(b, v) _longjmp(b, v)
#else
#define DC_SETJMP(b) setjmp(b)
#define DC_LONGJMP(b, v) longjmp(b, v)
#endif
extern uint32_t dc_coro_setjmp_addr;
jmp_buf *dc_coro_setjmp_slot(sh4ctx *c);              /* before the call: slot for buffer r4 */
void dc_coro_setjmp_done(sh4ctx *c, jmp_buf *slot);   /* after it: remember r15/pr/coroutine */
/* the target may be any mirror of the setjmp address (Propeller Arena calls it through P0) */
#define DC_CALL_SETJMP(c, t) do { uint32_t sjt_ = (t); \
    if (((sjt_ & 0x1FFFFFFFu) | 0x80000000u) == dc_coro_setjmp_addr) { jmp_buf *sjb_ = dc_coro_setjmp_slot(c); dc_call(c, sjt_); \
        dc_coro_setjmp_done(c, sjb_); if (sjb_) (void)DC_SETJMP(*sjb_); } \
    else dc_call(c, sjt_); } while (0)

/* ---------------- SR / FPSCR ---------------- */
static inline uint32_t sh4_get_sr(const sh4ctx *c) {
    return c->sr_rest | (c->t & 1) | ((c->s & 1) << 1) | ((c->q & 1) << 8) | ((c->m & 1) << 9);
}
#define SR_MD (1u << 30)
#define SR_RB (1u << 29)
#define SR_BL (1u << 28)
static inline void sh4_set_sr(sh4ctx *c, uint32_t v) {
    /* R0..R7 bank 1 is active when MD and RB are both set: swap on a change */
    int was = (c->sr_rest & (SR_MD | SR_RB)) == (SR_MD | SR_RB), now = (v & (SR_MD | SR_RB)) == (SR_MD | SR_RB);
    if (was != now)
        for (int i = 0; i < 8; i++) { uint32_t t_ = c->r[i]; c->r[i] = c->rbank[i]; c->rbank[i] = t_; }
    c->t = v & 1; c->s = (v >> 1) & 1; c->q = (v >> 8) & 1; c->m = (v >> 9) & 1;
    c->sr_rest = v & ~0x303u;
}

/* ---------------- time / events ----------------
 * c->cycles is a rough SH-4 cycle count (200 MHz) advanced at every call and backward branch.
 * When it passes dc_next_event the hardware model runs (video timing, timers) and may take an
 * interrupt by calling the game's handler at VBR+0x600. */
extern uint64_t dc_next_event;
void dc_events(struct sh4ctx *c);
extern uint32_t dc_loop_cost;      /* cycles charged per backward branch (default 16); dc_call_cost per call */
#define DC_TICK(c, n) do { (c)->cycles += (n); if ((c)->cycles >= dc_next_event) dc_events(c); } while (0)
/* a poll loop (the recompiler's idle_loop: it only loads, tests and branches back) can only end when
 * something outside it changes what it reads, so each pass lets time jump up to dc_idle_step cycles
 * ahead (default 20000, 0.1 ms), never past the next hardware event: waiting for the next frame no
 * longer runs the loop hundreds of thousands of times. DC_IDLE_STEP=0 turns it off. */
extern uint32_t dc_idle_step;
extern uint64_t dc_idle_skipped;
#define DC_IDLE(c) do { uint64_t t_ = (c)->cycles + dc_idle_step; if (t_ > dc_next_event) t_ = dc_next_event; \
    if (t_ > (c)->cycles) { dc_idle_skipped += t_ - (c)->cycles; (c)->cycles = t_; } DC_TICK(c, dc_loop_cost); } while (0)
extern uint32_t dc_call_cost;
/* save states (reset.h): a save waits for the next call from recompiled code, where the callee's address
 * is an exact place to go on from and every register is in the context */
extern volatile int dc_state_req;
void dc_state_point(sh4ctx *c, uint32_t addr);
static inline void dc_call_site(sh4ctx *c, uint32_t addr, dc_site *s) {
    if (dc_state_req) dc_state_point(c, addr);
    DC_TICK(c, dc_call_cost);
    if (s->gen != dc_dispatch_gen && !dc_site_resolve(s, addr)) { dc_call_ticked(c, addr); return; }
    c->entry = s->entry;
    uint32_t n = s->fn(c);
    if (n) dc_call(c, n);
}
static inline void sh4_swap_fbank(sh4ctx *c) {
    uint32_t tmp[16];
    memcpy(tmp, &c->fr.u[0], 64); memcpy(&c->fr.u[0], &c->fr.u[16], 64); memcpy(&c->fr.u[16], tmp, 64);
}
static inline void sh4_set_fpscr(sh4ctx *c, uint32_t v) {
    if ((v ^ c->fpscr) & FPSCR_FR) sh4_swap_fbank(c);
    c->fpscr = v & 0x003FFFFFu;
}

/* ---------------- double-precision register pairs ---------------- */
/* DRn = FR[n] (high word) : FR[n+1] (low word) */
static inline double sh4_get_dr(const sh4ctx *c, int n) {
    uint64_t v = ((uint64_t)c->fr.u[n] << 32) | c->fr.u[n + 1]; double d; memcpy(&d, &v, 8); return d;
}
static inline void sh4_set_dr(sh4ctx *c, int n, double d) {
    uint64_t v; memcpy(&v, &d, 8); c->fr.u[n] = (uint32_t)(v >> 32); c->fr.u[n + 1] = (uint32_t)v;
}
/* 64-bit fmov register index: even -> DRn (bank 0), odd -> XDn-1 (bank 1) */
static inline int sh4_xd(int n) { return (n & 1) ? 16 + (n & 0xE) : n; }

/* ---------------- ALU helpers ---------------- */
static inline void sh4_div1(sh4ctx *c, int n, int m) {
    uint32_t old_q = c->q, tmp0, tmp1, tmp2 = c->r[m];
    c->q = (c->r[n] >> 31) & 1;
    c->r[n] = (c->r[n] << 1) | (c->t & 1);
    tmp0 = c->r[n];
    if (old_q == c->m) { c->r[n] -= tmp2; tmp1 = c->r[n] > tmp0; }
    else               { c->r[n] += tmp2; tmp1 = c->r[n] < tmp0; }
    /* Q update: the four-way table in the SH-4 manual collapses to this */
    c->q = (tmp1 ^ c->q ^ c->m) & 1;
    c->t = (c->q == c->m);
}

static inline uint32_t sh4_ftrc(float f) {
    if (f != f) return 0x80000000u;
    if (f >= 2147483648.0f) return 0x7FFFFFFFu;
    if (f < -2147483648.0f) return 0x80000000u;
    return (uint32_t)(int32_t)f;
}
static inline uint32_t sh4_ftrc_d(double f) {
    if (f != f) return 0x80000000u;
    if (f >= 2147483648.0) return 0x7FFFFFFFu;
    if (f < -2147483648.0) return 0x80000000u;
    return (uint32_t)(int32_t)f;
}

/* fsca: FPUL low 16 bits = fraction of a full turn. The hardware reads a sine table; a port that
 * needs bit-exact results sets dc_fsca_table to 0x8000 floats = sin over the first half turn
 * (index i -> i * pi / 32768), the second half is the negated first half. Default: sin/cos. */
extern const float *dc_fsca_table;
static inline float sh4_fsin(uint32_t a) {
    a &= 0xFFFF;
    if (dc_fsca_table) return a < 0x8000 ? dc_fsca_table[a] : -dc_fsca_table[a - 0x8000];
    return (float)sin((double)a * (3.14159265358979323846 / 32768.0));
}
static inline void sh4_fsca(sh4ctx *c, int n) {
    c->fr.f[n] = sh4_fsin(c->fpul); c->fr.f[n + 1] = sh4_fsin(c->fpul + 0x4000);
}
/* fipr / ftrv: accumulated in double and rounded once, like the hardware's wide adder (and Flycast) */
static inline void sh4_fipr(sh4ctx *c, int n, int m) {
    float *f = c->fr.f;
    f[n + 3] = (float)((double)f[n] * f[m] + (double)f[n + 1] * f[m + 1] + (double)f[n + 2] * f[m + 2] + (double)f[n + 3] * f[m + 3]);
}
static inline void sh4_ftrv(sh4ctx *c, int n) {
    float *f = c->fr.f, *x = &c->fr.f[16];
    double v0 = f[n], v1 = f[n + 1], v2 = f[n + 2], v3 = f[n + 3];
    for (int i = 0; i < 4; i++)
        f[n + i] = (float)(x[i] * v0 + x[4 + i] * v1 + x[8 + i] * v2 + x[12 + i] * v3);
}

static inline void sh4_macl(sh4ctx *c, int n, int m) {
    int64_t a = (int64_t)(int32_t)dc_rd32(c->r[n]); c->r[n] += 4;
    int64_t b = (int64_t)(int32_t)dc_rd32(c->r[m]); c->r[m] += 4;
    int64_t acc = (int64_t)(((uint64_t)c->mach << 32) | c->macl) + a * b;
    if (c->s) { if (acc > 0x00007FFFFFFFFFFFLL) acc = 0x00007FFFFFFFFFFFLL; if (acc < -0x0000800000000000LL) acc = -0x0000800000000000LL; }
    c->mach = (uint32_t)((uint64_t)acc >> 32); c->macl = (uint32_t)acc;
}
static inline void sh4_macw(sh4ctx *c, int n, int m) {
    int32_t a = (int16_t)dc_rd16(c->r[n]); c->r[n] += 2;
    int32_t b = (int16_t)dc_rd16(c->r[m]); c->r[m] += 2;
    if (c->s) {
        int64_t acc = (int64_t)(int32_t)c->macl + (int64_t)a * b;
        if (acc > 0x7FFFFFFF) { acc = 0x7FFFFFFF; c->mach |= 1; } else if (acc < -0x7FFFFFFFLL - 1) { acc = -0x7FFFFFFFLL - 1; c->mach |= 1; }
        c->macl = (uint32_t)acc;
    } else {
        int64_t acc = (int64_t)(((uint64_t)c->mach << 32) | c->macl) + (int64_t)a * b;
        c->mach = (uint32_t)((uint64_t)acc >> 32); c->macl = (uint32_t)acc;
    }
}

#ifdef __cplusplus
}
#endif
#endif

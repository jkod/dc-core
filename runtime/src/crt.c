/* Native versions of a game's C library copy routines (memmove, memcpy, memset). Recompiled, they copy
 * a word or a byte per guest instruction through the memory helpers; natively it's one host memmove.
 * A port registers the routines it has found in its executable (dc_crt_register), with the recompiled
 * original as the fallback for anything outside main RAM (video memory, registers, sound RAM).
 * DC_CRT=0 leaves the recompiled routines in place. */
#include <stdlib.h>
#include <string.h>
#include "dccore/sh4.h"
#include "dccore/runtime.h"

static uint32_t crt_addr[DC_CRT_COUNT];
static sh4fn crt_orig[DC_CRT_COUNT];

static int in_ram(uint32_t a, uint32_t n) {
    return n == 0 || (DC_IS_RAM(a) && (uint64_t)DC_RAM_OFS(a) + n <= DC_RAM_SIZE && DC_IS_RAM(a + n - 1));
}
static uint32_t run_orig(sh4ctx *c, int k) {
    c->entry = crt_addr[k];
    uint32_t nx = crt_orig[k](c);
    if (nx) dc_call(c, nx);
    return 0;
}
static uint32_t crt_move(sh4ctx *c, int k) {
    uint32_t d = c->r[4], s = c->r[5], n = c->r[6];
    if (!in_ram(d, n) || !in_ram(s, n)) return run_orig(c, k);
    if (n) memmove(dc_ram + DC_RAM_OFS(d), dc_ram + DC_RAM_OFS(s), n);
    c->r[0] = d;
    DC_TICK(c, n / 4);
    return 0;
}
static uint32_t crt_memmove(sh4ctx *c) { return crt_move(c, DC_CRT_MEMMOVE); }
static uint32_t crt_memcpy(sh4ctx *c) { return crt_move(c, DC_CRT_MEMCPY); }
static uint32_t crt_memset(sh4ctx *c) {
    uint32_t d = c->r[4], v = c->r[5], n = c->r[6];
    if (!in_ram(d, n)) return run_orig(c, DC_CRT_MEMSET);
    if (n) memset(dc_ram + DC_RAM_OFS(d), (int)(v & 0xFF), n);
    c->r[0] = d;
    DC_TICK(c, n / 4);
    return 0;
}

void dc_crt_register(int which, uint32_t addr, sh4fn orig) {
    static const sh4fn fns[DC_CRT_COUNT] = { crt_memmove, crt_memcpy, crt_memset };
    const char *e = getenv("DC_CRT");
    if ((e && *e == '0') || which < 0 || which >= DC_CRT_COUNT || !orig) return;
    crt_addr[which] = (addr & 0x1FFFFFFFu) | 0x80000000u;
    crt_orig[which] = orig;
    dc_hle_register(addr, fns[which]);
}

/* the game is in its idle loop (a port's hook says so): let time jump ahead, as DC_IDLE does */
void dc_idle(sh4ctx *c) {
    uint64_t t = c->cycles + dc_idle_step;
    if (t > dc_next_event) t = dc_next_event;
    if (t > c->cycles) { dc_idle_skipped += t - c->cycles; c->cycles = t; }
    DC_TICK(c, 0);
}

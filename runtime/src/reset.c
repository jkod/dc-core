/* Resetting the game in place (reset.h): a copy of the emulated machine taken right before the game
 * starts, put back on a reset. */
#include <stdlib.h>
#include <string.h>
#include "dccore/reset.h"
#include "dccore/sh4.h"

static uint8_t *copy;          /* every region, back to back */
static size_t used, cap;
static int restoring;
static sh4ctx ctx0;
static uint8_t *ram0;

static void save(void *p, size_t n) {
    if (used + n > cap) {
        size_t c = cap ? cap * 2 : (size_t)16 << 20;
        while (c < used + n) c *= 2;
        uint8_t *q = realloc(copy, c);
        if (!q) { cap = 0; return; }
        copy = q; cap = c;
    }
    memcpy(copy + used, p, n); used += n;
}
static void load(void *p, size_t n) { memcpy(p, copy + used, n); used += n; }

static void all(dc_state_fn f) { dc_hw_state(f); dc_ta_state(f); dc_aica_state(f); dc_arm7_state(f); dc_bios_state(f); }

int dc_reset_snapshot(sh4ctx *c) {
    used = 0;
    all(save);
    if (!cap) return -1;
    if (!ram0) ram0 = malloc(DC_RAM_SIZE);
    if (!ram0) return -1;
    memcpy(ram0, dc_ram, DC_RAM_SIZE);
    ctx0 = *c;
    return 0;
}

void dc_reset_restore(sh4ctx *c) {
    if (!ram0 || restoring) return;
    restoring = 1;
    used = 0;
    all(load);
    memcpy(dc_ram, ram0, DC_RAM_SIZE);
    *c = ctx0;
    dc_aica_drop_output();
    dc_hw_invalidate_caches();
    dc_dispatch_gen++;                 /* call sites resolve again (code in RAM is the original again) */
    restoring = 0;
}

/* Resetting the game in place (reset.h): a copy of the emulated machine taken right before the game
 * starts, put back on a reset. */
#include <stdlib.h>
#include <string.h>
#include "dccore/reset.h"
#include "dccore/sh4.h"
#include "dccore/net.h"

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

static void all(dc_state_fn f) { dc_hw_state(f); dc_ta_state(f); dc_aica_state(f); dc_arm7_state(f); dc_bios_state(f); dc_bba_state(f); dc_modem_state(f); }

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

static void after_restore(void) {
    dc_aica_drop_output();
    dc_hw_invalidate_caches();
    dc_dispatch_gen++;                 /* call sites resolve again (the code in RAM may differ) */
}

void dc_reset_restore(sh4ctx *c) {
    if (!ram0 || restoring) return;
    restoring = 1;
    used = 0;
    all(load);
    memcpy(dc_ram, ram0, DC_RAM_SIZE);
    *c = ctx0;
    after_restore();
    restoring = 0;
}

/* ------------------------------------------------------------------ save states */
#include <stdio.h>
void (*dc_state_hook)(sh4ctx *c, uint32_t addr);
volatile int dc_state_req;
int dc_state_block;
void dc_state_point(sh4ctx *c, uint32_t addr) {
    if (dc_state_block) return;                     /* stays requested: the next call site */
    dc_state_req = 0;
    if (dc_state_hook) dc_state_hook(c, addr);
}

typedef struct {
    char magic[4];                     /* "DCST" */
    uint32_t version, regions, ram, ctx, pc;
    char tag[64];
} state_head;
static size_t counted;
static void count(void *p, size_t n) { (void)p; counted += n; }
static FILE *sf;
static int sf_err;
static void fsave(void *p, size_t n) { if (fwrite(p, 1, n, sf) != n) sf_err = 1; }
static uint8_t *src_at;
static void bload(void *p, size_t n) { memcpy(p, src_at, n); src_at += n; }

int dc_state_save(sh4ctx *c, uint32_t pc, const char *path, const char *tag) {
    state_head h; memset(&h, 0, sizeof h);
    memcpy(h.magic, "DCST", 4); h.version = 1; h.ram = DC_RAM_SIZE; h.ctx = sizeof *c; h.pc = pc;
    counted = 0; all(count); h.regions = (uint32_t)counted;
    snprintf(h.tag, sizeof h.tag, "%s", tag ? tag : "");
    sf = fopen(path, "wb");
    if (!sf) return -1;
    sf_err = 0;
    fsave(&h, sizeof h); all(fsave); fsave(dc_ram, DC_RAM_SIZE); fsave(c, sizeof *c);
    if (fclose(sf)) sf_err = 1;
    sf = NULL;
    return sf_err ? -1 : 0;
}

void *dc_state_load(const char *path, const char *tag) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    state_head h;
    counted = 0; all(count);
    if (fread(&h, sizeof h, 1, f) != 1 || memcmp(h.magic, "DCST", 4) || h.version != 1 || h.regions != counted ||
        h.ram != DC_RAM_SIZE || h.ctx != sizeof(sh4ctx) || strncmp(h.tag, tag ? tag : "", sizeof h.tag)) { fclose(f); return NULL; }
    size_t n = sizeof h + (size_t)h.regions + h.ram + h.ctx;
    uint8_t *b = malloc(n);
    if (!b) { fclose(f); return NULL; }
    memcpy(b, &h, sizeof h);
    int ok = fread(b + sizeof h, 1, n - sizeof h, f) == n - sizeof h;
    fclose(f);
    if (!ok) { free(b); return NULL; }
    return b;
}

uint32_t dc_state_apply(sh4ctx *c, void *state) {
    uint8_t *b = state; state_head h; memcpy(&h, b, sizeof h);
    src_at = b + sizeof h;
    all(bload);
    memcpy(dc_ram, src_at, DC_RAM_SIZE); src_at += DC_RAM_SIZE;
    memcpy(c, src_at, sizeof *c);
    free(b);
    after_restore();
    return h.pc;
}


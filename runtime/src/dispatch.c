/* dc-core: call dispatch for recompiled code, plus HLE overrides and a hardware stub. */
#include "dccore/sh4.h"
#include "dccore/runtime.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct { uint32_t addr, end; sh4fn fn; } dc_fn_entry;
extern const dc_fn_entry dc_fn_table[];
extern const size_t dc_fn_count;

uint8_t *dc_ram;
uint32_t dc_call_cost = 40;   /* SH-4 cycles charged per call/jump through the dispatcher */
sh4ctx *dc_current_ctx;
const float *dc_fsca_table;

#define MAX_HLE 256
static struct { uint32_t addr; sh4fn fn; } hle[MAX_HLE];
static int hle_n;
static dc_miss_fn miss_handler;
static dc_trap_fn trap_handler;
static dc_call_hook call_hook;

void dc_hle_register(uint32_t addr, sh4fn fn) {
    addr = (addr & 0x1FFFFFFFu) | 0x80000000u;
    for (int i = 0; i < hle_n; i++) if (hle[i].addr == addr) { hle[i].fn = fn; return; }
    if (hle_n < MAX_HLE) { hle[hle_n].addr = addr; hle[hle_n].fn = fn; hle_n++; }
}
void dc_set_miss_handler(dc_miss_fn f) { miss_handler = f; }
void dc_set_trap_handler(dc_trap_fn f) { trap_handler = f; }
void dc_set_call_hook(dc_call_hook f) { call_hook = f; }

/* ---- runtime-copied code: RAM ranges that hold a copy of code from the executable ---- */
#define MAX_ALIAS 64
static struct { uint32_t dst, src, len; } alias[MAX_ALIAS];
static int alias_n;
static uint32_t img_base, img_end;
int dc_alias_verbose = 1;

void dc_alias_code(uint32_t dst, uint32_t src, uint32_t len) {
    dst = (dst & 0x1FFFFFFFu) | 0x80000000u; src = (src & 0x1FFFFFFFu) | 0x80000000u;
    if (alias_n < MAX_ALIAS) { alias[alias_n].dst = dst; alias[alias_n].src = src; alias[alias_n].len = len; alias_n++; }
}

static sh4fn lookup_table(uint32_t addr);

/* addr is outside the executable: find the bytes there in the loaded image */
static uint32_t auto_alias(uint32_t addr) {
    uint32_t o = DC_RAM_OFS(addr);
    if (!img_end || !DC_IS_RAM(addr) || o + 32 > 0x1000000) return 0;
    const uint8_t *pat = dc_ram + o, *ib = dc_ram + DC_RAM_OFS(img_base);
    size_t n = img_end - img_base;
    static const uint8_t zero[32];
    if (!memcmp(pat, zero, 32)) return 0;
    uint32_t hit = 0; int hits = 0;
    for (size_t i = 0; i + 32 <= n; i += 2)
        if (ib[i] == pat[0] && !memcmp(ib + i, pat, 32)) { hit = img_base + (uint32_t)i; if (++hits > 1) break; }
    if (hits != 1) return 0;
    uint32_t len = 32;
    while (len < 0x4000 && o + len < 0x1000000 && hit - img_base + len < n && dc_ram[o + len] == ib[hit - img_base + len]) len++;
    dc_alias_code(addr, hit, len & ~1u);
    if (dc_alias_verbose) fprintf(stderr, "dc-core: code at %08X is a copy of %08X (%u bytes)\n", addr, hit, len);
    return hit;
}

sh4fn dc_lookup(uint32_t addr) {
    addr = (addr & 0x1FFFFFFFu) | 0x80000000u;   /* fold P0/P2 mirrors onto P1 */
    for (int i = 0; i < hle_n; i++) if (hle[i].addr == addr) return hle[i].fn;
    for (int i = 0; i < alias_n; i++)
        if (addr - alias[i].dst < alias[i].len) return lookup_table(alias[i].src + (addr - alias[i].dst));
    return lookup_table(addr);
}

static sh4fn lookup_table(uint32_t addr) {
    size_t lo = 0, hi = dc_fn_count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (dc_fn_table[mid].addr < addr) lo = mid + 1; else hi = mid;
    }
    if (lo < dc_fn_count && dc_fn_table[lo].addr == addr) return dc_fn_table[lo].fn;
    /* inside a function: it is entered there through its entry switch */
    if (lo > 0 && addr < dc_fn_table[lo - 1].end && !(addr & 1)) return dc_fn_table[lo - 1].fn;
    return NULL;
}

void dc_call(sh4ctx *c, uint32_t addr) {
    while (addr) {
        DC_TICK(c, dc_call_cost);
        if (call_hook) call_hook(c, addr);
        sh4fn f = dc_lookup(addr);
        if (!f && img_end && ((addr & 0x1FFFFFFFu) | 0x80000000u) - img_base >= img_end - img_base) {
            uint32_t src = auto_alias((addr & 0x1FFFFFFFu) | 0x80000000u);
            if (src) { f = dc_lookup(addr); if (!f) addr = src; }   /* report the image address */
        }
        if (!f) {
            if (miss_handler) { miss_handler(c, addr); return; }
            fprintf(stderr, "dc-core: no recompiled function at %08X (pr=%08X)\n", addr, c->pr);
            abort();
        }
        c->entry = (addr & 0x1FFFFFFFu) | 0x80000000u;
        for (int i = 0; i < alias_n; i++)       /* copied code runs as its original */
            if (c->entry - alias[i].dst < alias[i].len) { c->entry = alias[i].src + (c->entry - alias[i].dst); break; }
        addr = f(c);
    }
}

void dc_trap(sh4ctx *c, uint32_t pc, const char *what) {
    if (trap_handler) { trap_handler(c, pc, what); return; }
    fprintf(stderr, "dc-core: trap '%s' at %08X\n", what, pc);
}

/* ---- convenience ---- */
int dc_load_binary(const char *path, uint32_t addr) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(dc_ram + DC_RAM_OFS(addr), 1, 0x1000000 - DC_RAM_OFS(addr), f);
    fclose(f);
    img_base = (addr & 0x1FFFFFFFu) | 0x80000000u; img_end = img_base + (uint32_t)n;
    return (int)n;
}

/* an executable already in memory (e.g. read from the disc image) */
int dc_load_memory(const uint8_t *data, uint32_t len, uint32_t addr) {
    uint32_t o = DC_RAM_OFS(addr);
    if (o + len > 0x1000000) return -1;
    memcpy(dc_ram + o, data, len);
    img_base = (addr & 0x1FFFFFFFu) | 0x80000000u; img_end = img_base + len;
    return (int)len;
}

void dc_init(sh4ctx *c) {
    if (!dc_ram) dc_ram = (uint8_t *)calloc(1, 0x1000000);
    memset(c, 0, sizeof *c);
    dc_current_ctx = c;
    c->r[15] = 0x8C00F400u;             /* where the boot ROM leaves the stack (games keep data at the top of RAM) */
    c->sr_rest = 0x700000F0u;           /* MD=1 RB=1 BL=1 IMASK=15 */
    c->fpscr = 0x00040001u;             /* DN=1, RM=nearest, PR=0 SZ=0 FR=0 */
    c->vbr = 0x8C00F400u;
}

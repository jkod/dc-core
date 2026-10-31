/* dc-core: call dispatch for recompiled code, plus HLE overrides and a hardware stub. */
#include "dccore/sh4.h"
#include "dccore/runtime.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct { uint32_t addr; sh4fn fn; } dc_fn_entry;
extern const dc_fn_entry dc_fn_table[];
extern const size_t dc_fn_count;

uint8_t *dc_ram;
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

sh4fn dc_lookup(uint32_t addr) {
    addr = (addr & 0x1FFFFFFFu) | 0x80000000u;   /* fold P0/P2 mirrors onto P1 */
    for (int i = 0; i < hle_n; i++) if (hle[i].addr == addr) return hle[i].fn;
    size_t lo = 0, hi = dc_fn_count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (dc_fn_table[mid].addr < addr) lo = mid + 1; else hi = mid;
    }
    if (lo < dc_fn_count && dc_fn_table[lo].addr == addr) return dc_fn_table[lo].fn;
    return NULL;
}

void dc_call(sh4ctx *c, uint32_t addr) {
    sh4fn f = dc_lookup(addr);
    if (call_hook) call_hook(c, addr);
    if (f) { f(c); return; }
    if (miss_handler) { miss_handler(c, addr); return; }
    fprintf(stderr, "dc-core: no recompiled function at %08X (pr=%08X)\n", addr, c->pr);
    abort();
}

void dc_trap(sh4ctx *c, uint32_t pc, const char *what) {
    if (trap_handler) { trap_handler(c, pc, what); return; }
    fprintf(stderr, "dc-core: trap '%s' at %08X\n", what, pc);
}

/* ---- default hardware: a flat 64 KB scratch for the SH-4 on-chip RAM (0x7C000000),
 * everything else reads 0 and is logged once per address page. Platform layers
 * replace these with real PowerVR/AICA/Maple handling via dc_set_hw(). */
static dc_hw_ops hw;
static uint8_t oc_ram[0x2000];

void dc_set_hw(const dc_hw_ops *ops) { if (ops) hw = *ops; else memset(&hw, 0, sizeof hw); }

static int is_ocram(uint32_t a) { return (a & 0xFC000000u) == 0x7C000000u; }
static uint32_t warned[64]; static int nwarned;
static void warn(const char *k, uint32_t a) {
    uint32_t page = a & 0xFFFFF000u;
    for (int i = 0; i < nwarned; i++) if (warned[i] == page) return;
    if (nwarned < 64) warned[nwarned++] = page;
    fprintf(stderr, "dc-core: unhandled %s %08X\n", k, a);
}

#define HW_RD(bits, T) \
    T dc_hw_rd##bits(uint32_t a) { \
        if (is_ocram(a)) { T v; memcpy(&v, oc_ram + (a & 0x1FFF), sizeof v); return v; } \
        if (hw.rd##bits) return hw.rd##bits(a); warn("read" #bits, a); return 0; }
#define HW_WR(bits, T) \
    void dc_hw_wr##bits(uint32_t a, T v) { \
        if (is_ocram(a)) { memcpy(oc_ram + (a & 0x1FFF), &v, sizeof v); return; } \
        if (hw.wr##bits) { hw.wr##bits(a, v); return; } warn("write" #bits, a); }
HW_RD(8, uint8_t) HW_RD(16, uint16_t) HW_RD(32, uint32_t)
HW_WR(8, uint8_t) HW_WR(16, uint16_t) HW_WR(32, uint32_t)

void dc_hw_pref(uint32_t a) { if (hw.pref) hw.pref(a); }

/* ---- convenience ---- */
int dc_load_binary(const char *path, uint32_t addr) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(dc_ram + DC_RAM_OFS(addr), 1, 0x1000000 - DC_RAM_OFS(addr), f);
    fclose(f);
    return (int)n;
}

void dc_init(sh4ctx *c) {
    if (!dc_ram) dc_ram = (uint8_t *)calloc(1, 0x1000000);
    memset(c, 0, sizeof *c);
    c->r[15] = 0x8D000000u - 0x10;      /* top of RAM, like the boot ROM leaves it */
    c->sr_rest = 0x700000F0u;           /* MD=1 RB=1 BL=1 IMASK=15 */
    c->fpscr = 0x00040001u;             /* DN=1, RM=nearest, PR=0 SZ=0 FR=0 */
    c->vbr = 0x8C00F400u;
}

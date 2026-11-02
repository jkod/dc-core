/* dc-core boot probe: run a recompiled 1ST_READ.BIN with HLE BIOS + the player's disc and report
 * how far it gets (syscalls, unhandled hardware, last functions called).
 *
 *   cc -O1 -Iruntime/include tools/probe/boot_probe.c runtime/src/*.c gen/librecomp.a -lm -o boot_probe
 *   ./boot_probe game.cue|game.gdi 1ST_READ.BIN [seconds] [region 0/1/2]
 */
#include "dccore/runtime.h"
#include "dccore/coro.h"
#include "dccore/pvr.h"
#include "dccore/maple.h"
#include "dccore/disc.h"
#include "dccore/bios.h"
#include "dccore/hw.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>

static sh4ctx C;
static unsigned long calls;
static uint32_t last[16]; static int li;
#define NU 65536
static uint32_t seen[NU]; static unsigned nseen;

/* DC_LOGFN=addr:reg[,addr:reg...]: print the C string in that register whenever addr is called
 * (a game's own debug/error printer) */
static struct { uint32_t a; int reg; } logfn[8]; static int nlogfn;
static void print_guest_str(uint32_t p) {
    char b[160]; int i = 0;
    if (!DC_IS_RAM(p)) { printf("(%08X)", p); return; }
    for (; i < 159; i++) { char ch = (char)dc_rd8(p + i); if (!ch) break; b[i] = (ch >= 32 && ch < 127) ? ch : '.'; }
    b[i] = 0; printf("\"%s\"", b);
}
static uint64_t cur_frame;          /* vblanks so far */
static uint32_t trace_from; static int trace_left, trace_armed;
static void hook(sh4ctx *c, uint32_t a) {
    calls++; last[li++ & 15] = a;
    if (trace_from && ((a & 0x1FFFFFFFu) | 0x80000000u) == trace_from && !trace_armed) trace_armed = 1;
    static uint32_t dump_at_call; static int dumped_call;
    if (!dump_at_call && getenv("DC_DUMP_AT_CALL")) dump_at_call = (uint32_t)strtoul(getenv("DC_DUMP_AT_CALL"), 0, 16) | 0x80000000u;
    if (dump_at_call && !dumped_call && ((a & 0x1FFFFFFFu) | 0x80000000u) == dump_at_call) {
        dumped_call = 1; FILE *f = fopen("ramdump_at_call.bin", "wb"); if (f) { fwrite(dc_ram, 1, 0x1000000, f); fclose(f); }
        printf("RAM dumped at first call of %08X (%lu calls)\n", dump_at_call, calls);
    }
    if (trace_armed && trace_left > 0) {
        trace_left--;
        printf("trace %08X r4=%08X r5=%08X r6=%08X r0=%08X pr=%08X\n", a, c->r[4], c->r[5], c->r[6], c->r[0], c->pr);
    }
    for (int i = 0; i < nlogfn; i++) if (logfn[i].a == ((a & 0x1FFFFFFFu) | 0x80000000u)) {
        printf("log %08X r4=%08X r5=%08X r6=%08X f=%llu: ", a, c->r[4], c->r[5], c->r[6], (unsigned long long)cur_frame);
        print_guest_str(c->r[logfn[i].reg]); printf(" (pr=%08X)\n", c->pr);
    }
    uint32_t h = (a * 2654435761u) % NU;
    while (seen[h] && seen[h] != a) h = (h + 1) % NU;
    if (!seen[h] && nseen < NU - 1) { seen[h] = a; nseen++; }
}
static void report(const char *why) {
    printf("%s after %lu calls, %u distinct functions\nlast calls:", why, calls, nseen);
    for (int i = 0; i < 16; i++) printf(" %08X", last[(li + i) & 15]);
    printf("\nr0..r7: "); for (int i = 0; i < 8; i++) printf("%08X ", C.r[i]);
    printf("pr=%08X sr=%08X\nr8..r15: ", C.pr, sh4_get_sr(&C));
    for (int i = 8; i < 16; i++) printf("%08X ", C.r[i]);
    printf("\n");
    FILE *f = fopen("ramdump_full.bin", "wb"); if (f) { fwrite(dc_ram, 1, 0x1000000, f); fclose(f); }
    fflush(stdout);
}
static void miss(sh4ctx *c, uint32_t a) {
    char b[64]; snprintf(b, sizeof b, "MISS %08X", a); report(b);
    /* code outside the executable (copied to RAM at runtime): dump the 64 KB around it */
    uint32_t base = (a & 0x00FFFFFFu) & ~0xFFFFu;
    snprintf(b, sizeof b, "ramdump_%08X.bin", 0x8C000000u | base);
    FILE *f = fopen(b, "wb"); if (f) { fwrite(dc_ram + base, 1, 0x10000, f); fclose(f); printf("dumped %s\n", b); }
    exit(1);
}
#ifdef DC_WATCH
uint32_t dc_watch_lo = 0xFFFFFFFFu, dc_watch_hi = 0, dc_rwatch_lo = 0xFFFFFFFFu, dc_rwatch_hi = 0;
void dc_rwatch_hit(uint32_t a, int size) {
    static uint32_t seen[64]; static int ns;
    uint32_t fn = last[(li - 1) & 15];
    for (int i = 0; i < ns; i++) if (seen[i] == fn) return;
    if (ns < 64) seen[ns++] = fn;
    printf("rwatch: rd%d %08X in %08X pr=%08X (%lu calls)\n", size * 8, a, fn, C.pr, calls);
}
static int nwatch;
void dc_watch_hit(uint32_t a, uint32_t v, int size) {
    if (nwatch++ > 200) return;
    printf("watch: wr%d %08X = %08X in %08X pr=%08X r4..r7 %08X %08X %08X %08X (%lu calls) f=%llu ISTNRM=%08X ISTEXT=%08X INTEVT=%03X\n", size * 8, a, v,
           last[(li - 1) & 15], C.pr, C.r[4], C.r[5], C.r[6], C.r[7], calls, (unsigned long long)cur_frame,
           dc_sb_reg(0x005F6900u), dc_sb_reg(0x005F6904u), dc_rd32(0xFF000028u));
}
#endif
static void unhandled(const char *k, uint32_t a) {
    static int n; static uint32_t seen[64]; static int ns;
    uint32_t key = last[(li - 1) & 15];
    for (int i = 0; i < ns; i++) if (seen[i] == key) return;
    if (ns < 64) seen[ns++] = key;
    if (n++ < 64) printf("unhandled %s %08X in %08X (called from pr=%08X, %lu calls)\n", k, a, key, C.pr, calls);
}
static void on_read(uint32_t lba, uint32_t n, uint32_t dst) {
    static int done;
    const char *at = getenv("DC_DUMP_AT_LBA");
    if (done || !at || (uint32_t)strtoul(at, 0, 10) != lba) return;
    done = 1;
    FILE *f = fopen("ramdump_at_lba.bin", "wb"); if (f) { fwrite(dc_ram, 1, 0x1000000, f); fclose(f); }
    printf("RAM dumped before reading LBA %u (frame %llu, %lu calls)\n", lba, (unsigned long long)dc_frame_count(), calls);
}
/* hardware read statistics (DC_HWSTATS=1): the most-polled hardware addresses */
#define HS 4096
static struct { uint32_t a; unsigned long n; uint32_t fn; } hs[HS];
static void hw_read(uint32_t a, int size) {
    uint32_t h = (a * 2654435761u) % HS;
    for (int k = 0; k < 16; k++, h = (h + 1) % HS)
        if (hs[h].a == a || !hs[h].n) { hs[h].a = a; hs[h].n++; hs[h].fn = last[(li - 1) & 15]; return; }
}
static void hw_report(void) {
    fflush(stdout);
    for (int top = 0; top < 25; top++) {
        int b = -1; for (int i = 0; i < HS; i++) if (hs[i].n && (b < 0 || hs[i].n > hs[b].n)) b = i;
        if (b < 0 || !hs[b].n) break;
        printf("hwread %08X x%lu (last in %08X)\n", hs[b].a, hs[b].n, hs[b].fn); hs[b].n = 0;
    }
    fflush(stdout);
}
/* sound-side trace (DC_AICATRACE=1): AICA register writes and G2 DMA transfers */
static unsigned long aram_writes, aica_reg_writes;
static void aica_wr(uint32_t a, uint32_t v) {
    if (a >= 0x00800000u) { aram_writes++; return; }
    if (aica_reg_writes++ < 80) printf("aica reg wr %08X = %08X (in %08X, %lu calls, %lu sound-RAM writes so far)\n", a, v, last[(li - 1) & 15], calls, aram_writes);
}
static void sb_wr(uint32_t a, uint32_t v) {
    if ((a & 0xFFFF) >= 0x7800 && (a & 0xFFFF) < 0x7880 && (a & 0x1F) == 0x18 && (v & 1)) {
        uint32_t b = (a & 0x1FFFFFFFu) - 0x18;
        printf("G2 DMA ch%u: g2=%08X sh4=%08X len=%u dir=%u (in %08X)\n", ((a & 0xFFFF) - 0x7818) / 0x20,
               dc_sb_reg(b), dc_sb_reg(b + 4), dc_sb_reg(b + 8) & 0x7FFFFFFF, dc_sb_reg(b + 12), last[(li - 1) & 15]);
    }
}
/* DC_DUMP_FRAMES=a,b,c: dump RAM at these vblank counts (ramdump_f<N>.bin) */
static uint64_t renders;
static void on_render_count(void) { renders++; }

/* ---- DC_SHOT=f1,f2,...: software-render every frame, save the displayed image at these vblanks
 * (shot_fNNNNN.png). Uncompressed PNG (stored deflate) to stay dependency-free. */
static uint32_t crc_tab[256];
static uint32_t crc(uint32_t c, const uint8_t *p, size_t n) {
    if (!crc_tab[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t k = i; for (int j = 0; j < 8; j++) k = k & 1 ? 0xEDB88320u ^ (k >> 1) : k >> 1; crc_tab[i] = k; }
    c = ~c; while (n--) c = crc_tab[(c ^ *p++) & 255] ^ (c >> 8); return ~c;
}
static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void chunk(FILE *f, const char *t, const uint8_t *d, uint32_t n) {
    uint8_t h[8]; be32(h, n); memcpy(h + 4, t, 4); fwrite(h, 1, 8, f); if (n) fwrite(d, 1, n, f);
    uint32_t c = crc(crc(0, (const uint8_t *)t, 4), d, n); uint8_t cb[4]; be32(cb, c); fwrite(cb, 1, 4, f);
}
static int write_png(const char *path, const uint8_t *rgb, int w, int h) {
    FILE *f = fopen(path, "wb"); if (!f) return -1;
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ih[13]; be32(ih, w); be32(ih + 4, h); ih[8] = 8; ih[9] = 2; ih[10] = ih[11] = ih[12] = 0; chunk(f, "IHDR", ih, 13);
    size_t raw_n = (size_t)h * (w * 3 + 1), blocks = (raw_n + 65534) / 65535;
    uint8_t *raw = malloc(raw_n), *z = malloc(2 + raw_n + blocks * 5 + 4);
    for (int y = 0; y < h; y++) { raw[y * (w * 3 + 1)] = 0; memcpy(raw + y * (w * 3 + 1) + 1, rgb + (size_t)y * w * 3, (size_t)w * 3); }
    size_t o = 0; z[o++] = 0x78; z[o++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_n; i += 65535) {
        size_t n = raw_n - i < 65535 ? raw_n - i : 65535;
        z[o++] = i + n >= raw_n; z[o++] = n & 255; z[o++] = n >> 8; z[o++] = ~n & 255; z[o++] = (~n >> 8) & 255;
        memcpy(z + o, raw + i, n); o += n;
    }
    for (size_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    be32(z + o, b << 16 | a); o += 4;
    chunk(f, "IDAT", z, (uint32_t)o); chunk(f, "IEND", NULL, 0);
    fclose(f); free(raw); free(z); return 0;
}
static void shot_check(uint64_t f) {
    const char *p = getenv("DC_SHOT");
    while (p && *p) {
        unsigned long v = strtoul(p, (char **)&p, 10);
        if (v == f) {
            static uint8_t rgb[1280 * 1024 * 3]; int w, h;
            int on = dc_pvr_display(rgb, 1280, 1024, &w, &h);
            char b[64]; snprintf(b, sizeof b, "shot_f%05lu.png", v);
            write_png(b, rgb, w, h);
            fprintf(stderr, "shot %s %dx%d%s\n", b, w, h, on ? " (display off)" : "");
        }
        if (*p == ',') p++; else break;
    }
}
static int rendered;
static void on_render_sw(void) { renders++; dc_pvr_sw_render(); rendered++; }
static void on_vblank(uint64_t f) {
    cur_frame = f;
    if (getenv("DC_SHOT")) shot_check(f);
    if (getenv("DC_PVRDBG") && (uint64_t)atoll(getenv("DC_PVRDBG")) == f) dc_pvr_sw_debug = 1;
    if (getenv("DC_HIGHLIGHT")) {   /* DC_HIGHLIGHT=frame: classes 1..4 on the following 4 renders */
        uint64_t h0 = (uint64_t)atoll(getenv("DC_HIGHLIGHT"));
        dc_pvr_sw_highlight = f >= h0 && f < h0 + 4 ? (int)(f - h0 + 1) : 0;
    }
    if (getenv("DC_TRACE_FRAME")) {   /* DC_TRACE_FRAME=frame:count: trace calls from that vblank on */
        unsigned fr = 0, n = 200; sscanf(getenv("DC_TRACE_FRAME"), "%u:%u", &fr, &n);
        if (f == fr) { trace_armed = 1; trace_left = (int)n; }
    }
    if (getenv("DC_FPS") && f % 500 == 0) { static uint64_t last; static uint32_t lt; uint32_t t0 = dc_rd32(0xFFD8000Cu);
        printf("fps f=%llu renders/vblank=%.2f TCR0=%04X TOCR=%02X tmu0 ticks/vblank=%.1f\n", (unsigned long long)f, (renders - last) / 500.0,
               dc_rd16(0xFFD80010u), dc_rd8(0xFFD80000u), (double)(uint32_t)(lt - t0) / 500.0); last = renders; lt = t0; }
    const char *p = getenv("DC_DUMP_FRAMES");
    while (p && *p) {
        unsigned long v = strtoul(p, (char **)&p, 10);
        if (v == f) { char b[64]; snprintf(b, sizeof b, "ramdump_f%05lu.bin", v); FILE *o = fopen(b, "wb"); if (o) { fwrite(dc_ram, 1, 0x1000000, o); fclose(o); } }
        if (*p == ',') p++; else break;
    }
}
static void trap(sh4ctx *c, uint32_t pc, const char *w) { printf("TRAP %s @%08X\n", w, pc); }
static void alrm(int s) { report("timeout"); if (getenv("DC_HWSTATS")) hw_report(); _exit(0); }

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s disc.cue|disc.gdi 1ST_READ.BIN [seconds]\n", argv[0]); return 2; }
    dc_init(&C);
    if (dc_disc_open(argv[1])) { fprintf(stderr, "cannot open disc %s\n", argv[1]); return 2; }
    if (dc_load_binary(argv[2], 0x8C010000u) < 0) { fprintf(stderr, "cannot read %s\n", argv[2]); return 2; }
    dc_bios_trace = 1;
    if (getenv("DC_BIOS")) {   /* directory with dc_boot.bin / dc_flash.bin */
        char b[1024], fl[1024];
        snprintf(b, sizeof b, "%s/dc_boot.bin", getenv("DC_BIOS")); snprintf(fl, sizeof fl, "%s/dc_flash.bin", getenv("DC_BIOS"));
        printf("BIOS: %s\n", dc_bios_load(b, fl) ? "load failed" : "loaded");
    }
    if (dc_bios_install(&C, argc > 4 ? argv[4][0] : '2')) fprintf(stderr, "warning: IP.BIN not loaded\n");
    dc_unhandled_hook = unhandled; dc_hw_verbose = 0;
    dc_bios_read_hook = on_read;
    if (getenv("DC_CORO")) {   /* the game's setjmp,longjmp (recompile with --setjmp too) */
        unsigned sj = 0, lj = 0; sscanf(getenv("DC_CORO"), "%x,%x", &sj, &lj);
        dc_coro_install(sj, lj, 0); dc_coro_trace = getenv("DC_CORO_TRACE") != NULL;
    }
    if (getenv("DC_SWITCH")) {   /* task-switch entry points: addr,addr,... */
        const char *p = getenv("DC_SWITCH");
        while (*p) { dc_coro_add_switch((uint32_t)strtoul(p, (char **)&p, 16)); while (*p == ',') p++; }
        dc_coro_trace = getenv("DC_CORO_TRACE") != NULL;
    }
    if (getenv("DC_HWSTATS")) dc_hw_read_hook = hw_read;
    dc_hw.on_vblank = on_vblank;
    if (getenv("DC_CABLE")) dc_cable = atoi(getenv("DC_CABLE"));
    if (getenv("DC_GD_DMA_IRQ")) dc_gd_dma_irq = 1;   /* Tennis 2K2 */
    if (!getenv("DC_NOPAD")) dc_maple_install();   /* a standard controller on port A */
    if (getenv("DC_CALLCOST")) dc_call_cost = (uint32_t)atoi(getenv("DC_CALLCOST"));
    if (getenv("DC_FPS")) dc_hw.on_render = on_render_count;
    if (getenv("DC_SHOT")) { dc_pvr_sw_install(); dc_hw.on_render = on_render_sw; }   /* frame count for logs; DC_DUMP_FRAMES dumps */
    if (getenv("DC_TRACE_FROM")) { unsigned a, n = 200; sscanf(getenv("DC_TRACE_FROM"), "%x:%u", &a, &n); trace_from = a | 0x80000000u; trace_left = (int)n; }
    if (getenv("DC_LOGFN")) {
        const char *p = getenv("DC_LOGFN");
        while (*p && nlogfn < 8) { unsigned a, r; if (sscanf(p, "%x:%u", &a, &r) != 2) break; logfn[nlogfn].a = a | 0x80000000u; logfn[nlogfn].reg = (int)r; nlogfn++; p = strchr(p, ','); if (!p) break; p++; }
    }
    if (getenv("DC_AICATRACE")) { dc_hw.on_aica_write = aica_wr; dc_hw.on_sb_write = sb_wr; }
#ifdef DC_WATCH
    if (getenv("DC_WATCH")) { unsigned lo, len = 4; sscanf(getenv("DC_WATCH"), "%x:%x", &lo, &len); dc_watch_lo = lo & 0xFFFFFF; dc_watch_hi = dc_watch_lo + len; }
    if (getenv("DC_RWATCH")) { unsigned lo, len = 4; sscanf(getenv("DC_RWATCH"), "%x:%x", &lo, &len); dc_rwatch_lo = lo & 0xFFFFFF; dc_rwatch_hi = dc_rwatch_lo + len; }
#endif
    dc_set_call_hook(hook); dc_set_miss_handler(miss); dc_set_trap_handler(trap);
    signal(SIGALRM, alrm); alarm(argc > 3 ? atoi(argv[3]) : 10);
    C.pr = 0xDEAD0000u;
    dc_call(&C, 0x8C010000u);
    report("returned");
    return 0;
}

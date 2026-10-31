/* dc-core boot probe: run a recompiled 1ST_READ.BIN with HLE BIOS + the player's disc and report
 * how far it gets (syscalls, unhandled hardware, last functions called).
 *
 *   cc -O1 -Iruntime/include tools/probe/boot_probe.c runtime/src/*.c gen/librecomp.a -lm -o boot_probe
 *   ./boot_probe game.cue|game.gdi 1ST_READ.BIN [seconds] [region 0/1/2]
 */
#include "dccore/runtime.h"
#include "dccore/disc.h"
#include "dccore/bios.h"
#include "dccore/hw.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>

static sh4ctx C;
static unsigned long calls;
static uint32_t last[16]; static int li;
#define NU 65536
static uint32_t seen[NU]; static unsigned nseen;

static void hook(sh4ctx *c, uint32_t a) {
    calls++; last[li++ & 15] = a;
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
uint32_t dc_watch_lo = 0xFFFFFFFFu, dc_watch_hi = 0;
static int nwatch;
void dc_watch_hit(uint32_t a, uint32_t v, int size) {
    if (nwatch++ > 200) return;
    printf("watch: wr%d %08X = %08X in %08X pr=%08X r4..r7 %08X %08X %08X %08X (%lu calls)\n", size * 8, a, v,
           last[(li - 1) & 15], C.pr, C.r[4], C.r[5], C.r[6], C.r[7], calls);
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
static void trap(sh4ctx *c, uint32_t pc, const char *w) { printf("TRAP %s @%08X\n", w, pc); }
static void alrm(int s) { report("timeout"); _exit(0); }

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
#ifdef DC_WATCH
    if (getenv("DC_WATCH")) { unsigned lo, len = 4; sscanf(getenv("DC_WATCH"), "%x:%x", &lo, &len); dc_watch_lo = lo & 0xFFFFFF; dc_watch_hi = dc_watch_lo + len; }
#endif
    dc_set_call_hook(hook); dc_set_miss_handler(miss); dc_set_trap_handler(trap);
    signal(SIGALRM, alrm); alarm(argc > 3 ? atoi(argv[3]) : 10);
    C.pr = 0xDEAD0000u;
    dc_call(&C, 0x8C010000u);
    report("returned");
    return 0;
}

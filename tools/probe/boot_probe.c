/* dc-core boot probe: run a recompiled 1ST_READ.BIN with HLE BIOS + the player's disc and report
 * how far it gets (syscalls, unhandled hardware, last functions called).
 *
 *   cc -O1 -Iruntime/include tools/probe/boot_probe.c runtime/src/*.c gen/librecomp.a -lm -o boot_probe
 *   ./boot_probe game.cue|game.gdi 1ST_READ.BIN [seconds] [region 0/1/2]
 */
#include "dccore/runtime.h"
#include "dccore/disc.h"
#include "dccore/bios.h"
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
    printf("pr=%08X sr=%08X\n", C.pr, sh4_get_sr(&C));
    fflush(stdout);
}
static void miss(sh4ctx *c, uint32_t a) { char b[64]; snprintf(b, sizeof b, "MISS %08X", a); report(b); exit(1); }
static void trap(sh4ctx *c, uint32_t pc, const char *w) { printf("TRAP %s @%08X\n", w, pc); }
static void alrm(int s) { report("timeout"); _exit(0); }

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s disc.cue|disc.gdi 1ST_READ.BIN [seconds]\n", argv[0]); return 2; }
    dc_init(&C);
    if (dc_disc_open(argv[1])) { fprintf(stderr, "cannot open disc %s\n", argv[1]); return 2; }
    if (dc_load_binary(argv[2], 0x8C010000u) < 0) { fprintf(stderr, "cannot read %s\n", argv[2]); return 2; }
    dc_bios_trace = 1;
    if (dc_bios_install(&C, argc > 4 ? argv[4][0] : '2')) fprintf(stderr, "warning: IP.BIN not loaded\n");
    dc_set_call_hook(hook); dc_set_miss_handler(miss); dc_set_trap_handler(trap);
    signal(SIGALRM, alrm); alarm(argc > 3 ? atoi(argv[3]) : 10);
    C.pr = 0xDEAD0000u;
    dc_call(&C, 0x8C010000u);
    report("returned");
    return 0;
}

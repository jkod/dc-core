/* CD audio: GD-ROM PLAY through the HLE syscall streams the audio track's raw sectors;
 * GETSCD reports "playing". cc -Iruntime/include tests/test_cdda.c runtime/src/*.c -lm */
#include "dccore/runtime.h"
#include "dccore/bios.h"
#include "dccore/disc.h"
#include <stdio.h>
#include <string.h>
#include <stddef.h>

typedef struct { uint32_t addr, end; sh4fn fn; } dc_fn_entry;   /* no recompiled code */
const dc_fn_entry dc_fn_table[1] = {{0, 0, 0}};
const size_t dc_fn_count = 0;

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static uint32_t gd(sh4ctx *c, uint32_t func, uint32_t cmd, const uint32_t p[4]) {
    for (int i = 0; i < 4; i++) dc_wr32(0x8C100000u + i * 4, p ? p[i] : 0);
    c->r[7] = func; c->r[4] = cmd; c->r[5] = 0x8C100000u; c->pr = 0;
    dc_call(c, dc_rd32(0x8C0000BCu));
    return c->r[0];
}

int main(void) {
    static uint8_t data[2048 * 4], audio[2352 * 3];
    for (int s = 0; s < 3 * 588; s++) {                     /* left = frame number, right = -frame */
        int16_t l = (int16_t)s, r = (int16_t)-s;
        memcpy(audio + s * 4, &l, 2); memcpy(audio + s * 4 + 2, &r, 2);
    }
    FILE *f = fopen("cddatest_t1.bin", "wb"); fwrite(data, 1, sizeof data, f); fclose(f);
    f = fopen("cddatest_t2.raw", "wb"); fwrite(audio, 1, sizeof audio, f); fclose(f);
    f = fopen("cddatest_t3.bin", "wb"); fwrite(data, 1, sizeof data, f); fclose(f);
    f = fopen("cddatest.gdi", "w");
    fprintf(f, "3\n1 0 4 2048 cddatest_t1.bin 0\n2 600 0 2352 cddatest_t2.raw 0\n3 45000 4 2048 cddatest_t3.bin 0\n"); fclose(f);

    sh4ctx c; dc_init(&c);
    CHECK(dc_disc_open("cddatest.gdi") == 0);
    dc_bios_install(&c, '1');                                /* no IP.BIN on this disc: syscalls only */
    int16_t l, r;
    CHECK(dc_cdda_frame(&l, &r) == 0);                       /* silent until PLAY */
    const uint32_t play[4] = { 2, 2, 0, 0 };                 /* track 2 to 2, no repeat */
    gd(&c, 0, 20, play);
    int ok = 1, n = 0;
    while (dc_cdda_frame(&l, &r)) { if (l != (int16_t)n || r != (int16_t)-n) ok = 0; n++; }
    CHECK(ok && n == 3 * 588);
    const uint32_t loop[4] = { 2, 2, 15, 0 };                /* forever */
    gd(&c, 0, 20, loop);
    for (int i = 0; i < 3 * 588 + 5; i++) dc_cdda_frame(&l, &r);
    CHECK(l == 4 && r == -4);                                /* wrapped to the start */
    const uint32_t scd[4] = { 1, 14, 0x8C101000u, 0 };
    gd(&c, 0, 34, scd);
    CHECK(dc_rd8(0x8C101001u) == 0x11 && dc_rd8(0x8C101005u) == 2);   /* playing, track 2 */
    gd(&c, 0, 22, NULL);                                     /* PAUSE */
    CHECK(dc_cdda_frame(&l, &r) == 0);
    remove("cddatest_t1.bin"); remove("cddatest_t2.raw"); remove("cddatest_t3.bin"); remove("cddatest.gdi");
    printf(fails ? "%d cdda checks failed\n" : "cdda tests passed\n", fails);
    return fails != 0;
}

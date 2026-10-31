/* Frontend smoke test, headless: SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy.
 * cc -Iruntime/include -Ifrontend tests/test_front.c frontend/dccore_front.c runtime/src/*.c $(sdl2-config --cflags --libs) -lm */
#include "dccore/runtime.h"
#include "dccore/hw.h"
#include "dccore/pvr.h"
#include "dccore_front.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef struct { uint32_t addr, end; sh4fn fn; } dc_fn_entry;
const dc_fn_entry dc_fn_table[1] = {{0, 0, 0}};
const size_t dc_fn_count = 0;

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
static void pvr(uint32_t ofs, uint32_t v) { dc_wr32(0xA05F8000u + ofs, v); }
static void ta(const uint32_t w[8]) { for (int i = 0; i < 8; i++) dc_wr32(0x10000000u + i * 4, w[i]); }
static uint32_t fu(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

int main(void) {
    sh4ctx c; dc_init(&c);
    dc_front_config cfg = { .title = "dc-core test", .scale = 2, .no_pacing = 1 };
    if (dc_front_open(&cfg)) { printf("FAIL: frontend did not open\n"); return 1; }

    pvr(0x068, 63u << 16); pvr(0x06C, 63u << 16);           /* 64x64 scene */
    pvr(0x020, 0); pvr(0x08C, (0x1000 / 4) << 3); pvr(0x088, fu(0.0f));
    uint32_t bgp[] = { 0, 0, 0, fu(0), fu(0), fu(1), 0xFF0000FFu };
    for (int i = 0; i < 7; i++) dc_wr32(0xA5000000u + 0x1000 + i * 4, bgp[i]);
    pvr(0x128, 0); pvr(0x144, 0x80000000u);
    uint32_t hdr[8] = { (4u << 29) | (1u << 1), (7u << 29), (1u << 29) | (2u << 22), 0, 0, 0, 0, 0 };
    uint32_t v0[8] = { 7u << 29, fu(4), fu(4), fu(1), 0, 0, 0xFFFF0000u, 0 }, v1[8] = { 7u << 29, fu(40), fu(4), fu(1), 0, 0, 0xFFFF0000u, 0 };
    uint32_t v2[8] = { (7u << 29) | (1u << 28), fu(4), fu(40), fu(1), 0, 0, 0xFFFF0000u, 0 }, eol[8] = {0};
    ta(hdr); ta(v0); ta(v1); ta(v2); ta(eol);
    pvr(0x014, 1);                                         /* STARTRENDER -> GL at 2x */
    dc_hw.on_vblank(1);                                    /* present, audio, input */
    dc_hw.on_vblank(2);

    static uint8_t rgb[128 * 128 * 3]; int w, h;
    dc_pvr_gl_read(rgb, &w, &h);
    CHECK(w == 128 && h == 128, "render target %dx%d, want 128x128 (scale 2)", w, h);
    const uint8_t *in = rgb + (20 * w + 20) * 3, *out = rgb + (100 * w + 100) * 3;
    CHECK(in[0] == 255 && in[1] == 0 && in[2] == 0, "triangle %02X%02X%02X", in[0], in[1], in[2]);
    CHECK(out[0] == 0 && out[2] == 255, "background %02X%02X%02X", out[0], out[1], out[2]);
    CHECK(dc_front_frames() == 2, "presented %lu frames", dc_front_frames());
    dc_front_close();
    printf(fails ? "%d frontend checks failed\n" : "frontend tests passed\n", fails);
    return fails != 0;
}

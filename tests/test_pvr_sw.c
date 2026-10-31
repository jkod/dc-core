/* Software PowerVR2 renderer: a frame built through the real register/TA path, checked pixel by
 * pixel. cc -Iruntime/include tests/test_pvr_sw.c runtime/src/*.c -lm */
#include "dccore/runtime.h"
#include "dccore/hw.h"
#include "dccore/pvr.h"
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
static void on_render(void) { int n = dc_pvr_sw_render(); if (getenv("PVRDBG")) printf("rendered %d polys\n", n); }

static uint32_t fb_px(int x, int y) {          /* ARGB8888 framebuffer at 0x200000, stride 64 px */
    uint32_t v = 0;
    for (int k = 0; k < 4; k++) v |= (uint32_t)dc_rd8(0xA5000000u + 0x200000u + (uint32_t)(y * 64 + x) * 4 + k) << (8 * k);
    return v & 0xFFFFFF;
}

int main(void) {
    sh4ctx c; dc_init(&c);
    dc_pvr_sw_install();
    dc_hw.on_render = on_render;

    /* 64x64 target, ARGB8888 at 0x200000 (32-bit area), stride 64*4 bytes = 32 64-bit units */
    pvr(0x068, 63u << 16); pvr(0x06C, 63u << 16);
    pvr(0x048, 6); pvr(0x04C, 32); pvr(0x060, 0x200000);
    /* background polygon at parameter offset 0x1000: ISP, TSP, TCW, 3 x (x, y, z, colour) */
    pvr(0x020, 0); pvr(0x08C, (0x1000 / 4) << 3); pvr(0x088, fu(0.0f));
    uint32_t bgp[] = { 0, 0, 0, fu(0), fu(0), fu(1), 0xFF0000FFu };
    for (int i = 0; i < 7; i++) dc_wr32(0xA5000000u + 0x1000 + i * 4, bgp[i]);

    pvr(0x128, 0); pvr(0x144, 0x80000000u);       /* TA_ISP_BASE, TA_LIST_INIT */

    /* opaque Gouraud triangle, packed colour, depth "always" */
    uint32_t hdr[8] = { (4u << 29) | (0u << 24) | (1u << 1), (7u << 29) | (1u << 23), (1u << 29), 0, 0, 0, 0, 0 };
    ta(hdr);
    uint32_t v0[8] = { 7u << 29, fu(4), fu(4), fu(1), 0, 0, 0xFFFF0000u, 0 };
    uint32_t v1[8] = { 7u << 29, fu(40), fu(4), fu(1), 0, 0, 0xFFFF0000u, 0 };
    uint32_t v2[8] = { (7u << 29) | (1u << 28), fu(4), fu(40), fu(1), 0, 0, 0xFFFF0000u, 0 };
    ta(v0); ta(v1); ta(v2);

    /* textured sprite: 8x8 twiddled RGB565 texture at 0x100000, texel (u,v) -> red = u*4, green = v*8 */
    for (int v = 0; v < 8; v++) for (int u = 0; u < 8; u++) {
        uint32_t i = 0, s = 0;
        for (uint32_t b = 1; b < 8; b <<= 1, s += 2) { if (v & b) i |= 1u << s; if (u & b) i |= 2u << s; }
        uint16_t px = (uint16_t)((u * 4) << 11 | (v * 8) << 5);
        dc_wr8(0xA4000000u + 0x100000u + i * 2, px & 255); dc_wr8(0xA4000000u + 0x100000u + i * 2 + 1, px >> 8);
    }
    uint32_t sh[8] = { (5u << 29) | (0u << 24) | (1u << 3), (7u << 29) | (1u << 25), (1u << 29) | (0u << 6), (1u << 27) | (0x100000u >> 3), 0xFFFFFFFFu, 0, 0, 0 };
    ta(sh);
    /* sprite quad 48..56 x 48..56; uv 0..1 */
    uint32_t sv[16] = { (7u << 29) | (1u << 28), fu(48), fu(48), fu(1), fu(56), fu(48), fu(1), fu(56), fu(56), fu(1), fu(48), fu(56), 0,
                        fu(0) >> 16 << 16 | fu(0) >> 16, fu(1.0f) & 0xFFFF0000u, (fu(1.0f) & 0xFFFF0000u) | fu(1.0f) >> 16 };
    ta(sv); ta(sv + 8);
    uint32_t eol[8] = {0};
    ta(eol);

    pvr(0x014, 1);                                  /* STARTRENDER */
    if (getenv("PVRDBG")) printf("raw %08X %08X %08X\n", dc_rd32(0xA5200000u), dc_rd32(0xA5200000u + (8 * 64 + 8) * 4), dc_rd32(0xA5200000u + (4 * 64 + 60) * 4));

    CHECK(fb_px(8, 8) == 0xFF0000, "triangle pixel %06X", fb_px(8, 8));
    CHECK(fb_px(60, 4) == 0x0000FF, "background pixel %06X", fb_px(60, 4));
    CHECK(fb_px(39, 39) == 0x0000FF, "outside triangle %06X", fb_px(39, 39));
    /* sprite pixel (48+5.5, 48+2.5) -> texel (5, 2): red 20<<3|.. , green 16<<2|.. */
    uint32_t t = fb_px(53, 50), exp_r = (20 << 3) | (20 >> 2), exp_g = (16 << 2) | (16 >> 4);
    CHECK(((t >> 16) & 255) == exp_r && ((t >> 8) & 255) == exp_g, "texel %06X want %02X%02X00", t, exp_r, exp_g);

    /* display read-out: 32-bit framebuffer, 64 words per line */
    pvr(0x044, 1 | (3 << 2)); pvr(0x050, 0x200000); pvr(0x05C, 63u | (63u << 10) | (1u << 20));
    static uint8_t rgb[64 * 64 * 3]; int w, h;
    CHECK(dc_pvr_display(rgb, 64, 64, &w, &h) == 0 && w == 64 && h == 64, "display %dx%d", w, h);
    CHECK(rgb[(8 * 64 + 8) * 3] == 255 && rgb[(8 * 64 + 8) * 3 + 2] == 0, "display pixel");

    printf(fails ? "%d pvr_sw checks failed\n" : "pvr_sw tests passed\n", fails);
    return fails != 0;
}

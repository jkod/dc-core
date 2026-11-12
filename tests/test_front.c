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
    dc_front_config cfg = { .title = "dc-core test", .scale = 2, .no_pacing = 1, .no_vmu = 1 };
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

    static uint8_t rgb[256 * 128 * 3]; int w, h;
    dc_pvr_gl_read(rgb, &w, &h);
    CHECK(w == 128 && h == 128, "render target %dx%d, want 128x128 (scale 2)", w, h);
    const uint8_t *in = rgb + (20 * w + 20) * 3, *out = rgb + (100 * w + 100) * 3;
    CHECK(in[0] == 255 && in[1] == 0 && in[2] == 0, "triangle %02X%02X%02X", in[0], in[1], in[2]);
    CHECK(out[0] == 0 && out[2] == 255, "background %02X%02X%02X", out[0], out[1], out[2]);
    CHECK(dc_front_frames() == 2, "presented %lu frames", dc_front_frames());

    /* widescreen: 16:9 target, a triangle left of the 4:3 picture lands in the left band */
    dc_pvr_gl_widescreen = 1;
    dc_pvr_gl_aspect = 16.f / 9.f;                         /* the frontend keeps the window's shape here (4:3 in this test) */
    { uint32_t w0[8] = { 7u << 29, fu(-20), fu(4), fu(1), 0, 0, 0xFF00FF00u, 0 }, w1[8] = { 7u << 29, fu(-2), fu(4), fu(1), 0, 0, 0xFF00FF00u, 0 };
      uint32_t w2[8] = { (7u << 29) | (1u << 28), fu(-20), fu(40), fu(1), 0, 0, 0xFF00FF00u, 0 };
      pvr(0x128, 0); pvr(0x144, 0x80000000u);              /* new list */
      ta(hdr); ta(w0); ta(w1); ta(w2); ta(v0); ta(v1); ta(v2); ta(eol); }
    pvr(0x014, 1);
    dc_pvr_gl_read(rgb, &w, &h);
    CHECK(w == 170 && h == 128, "widescreen target %dx%d, want 170x128 (whole-pixel margins of 21)", w, h);
    const uint8_t *side = rgb + (20 * w + 6) * 3, *mid = rgb + (20 * w + 21 + 20) * 3, *edge = rgb + (100 * w + 5) * 3;
    CHECK(side[1] == 255 && side[0] == 0, "left band %02X%02X%02X, want the green triangle", side[0], side[1], side[2]);
    CHECK(mid[0] == 255 && mid[1] == 0, "centre %02X%02X%02X, want the red triangle", mid[0], mid[1], mid[2]);
    CHECK(edge[2] == 255, "band background %02X%02X%02X", edge[0], edge[1], edge[2]);

    /* widescreen HUD: a 2D (1/w >= 999) triangle in the left third moves to the left edge when the
     * frame also has a 3D scene */
    dc_pvr_gl_hud_z = 999.f;
    pvr(0x128, 0); pvr(0x144, 0x80000000u);
    ta(hdr);
    for (int i = 0; i < 60; i++) {                          /* 3D: tiny triangles at the bottom right, varying 1/w; */
        /* the first one reaches past the 4:3 picture (a race scene fills the width; menus do not) */
        uint32_t a0[8] = { 7u << 29, fu(60), fu(60), fu(1.0f + i * 0.01f), 0, 0, 0xFFFFFFFFu, 0 }, a1[8] = { 7u << 29, fu(i ? 61 : 70), fu(60), fu(1.5f), 0, 0, 0xFFFFFFFFu, 0 };
        uint32_t a2[8] = { (7u << 29) | (1u << 28), fu(60), fu(61), fu(2.0f), 0, 0, 0xFFFFFFFFu, 0 };
        ta(a0); ta(a1); ta(a2);
    }
    { uint32_t h0[8] = { 7u << 29, fu(4), fu(4), fu(1000), 0, 0, 0xFF00FF00u, 0 }, h1[8] = { 7u << 29, fu(16), fu(4), fu(1000), 0, 0, 0xFF00FF00u, 0 };
      uint32_t h2[8] = { (7u << 29) | (1u << 28), fu(4), fu(16), fu(1000), 0, 0, 0xFF00FF00u, 0 };
      ta(h0); ta(h1); ta(h2); }
    ta(eol);
    pvr(0x014, 1);
    dc_pvr_gl_read(rgb, &w, &h);
    const uint8_t *moved = rgb + (12 * w + 12) * 3, *old = rgb + (12 * w + 40) * 3;   /* shifted: x 8..28, 4:3 position: 29..49 (2x) */
    CHECK(moved[1] == 255 && moved[0] == 0, "HUD at the left edge %02X%02X%02X", moved[0], moved[1], moved[2]);
    CHECK(old[2] == 255, "HUD left its 4:3 position %02X%02X%02X", old[0], old[1], old[2]);
    dc_pvr_gl_hud_z = 0;
    dc_pvr_gl_widescreen = 0;
    /* mipmaps (also switched on for an already uploaded texture): a 64x64 one-texel checkerboard on a
     * 4x4 quad averages to grey */
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
        uint16_t px = ((x ^ y) & 1) ? 0xFFFF : 0x0000;                         /* RGB565, scan order */
        dc_wr8(0xA4000000u + 0x100000u + (uint32_t)(y * 64 + x) * 2, px & 255); dc_wr8(0xA4000000u + 0x100000u + (uint32_t)(y * 64 + x) * 2 + 1, px >> 8);
    }
    int grey[2];
    for (int m = 0; m < 2; m++) {
        dc_pvr_gl_mipmaps = m;
        pvr(0x128, 0); pvr(0x144, 0x80000000u);
        uint32_t th[8] = { (4u << 29) | (1u << 3), (7u << 29) | (1u << 25), (1u << 29) | (1u << 13) | (3u << 3) | 3u, (1u << 27) | (1u << 26) | (0x100000u >> 3) | (uint32_t)m * 0, 0, 0, 0, 0 };
        ta(th);
        uint32_t q0[8] = { 7u << 29, fu(30), fu(30), fu(1), fu(0.f), fu(0.f), 0xFFFFFFFFu, 0 }, q1[8] = { 7u << 29, fu(34), fu(30), fu(1), fu(1.f), fu(0.f), 0xFFFFFFFFu, 0 };
        uint32_t q2[8] = { 7u << 29, fu(30), fu(34), fu(1), fu(0.f), fu(1.f), 0xFFFFFFFFu, 0 }, q3[8] = { (7u << 29) | (1u << 28), fu(34), fu(34), fu(1), fu(1.f), fu(1.f), 0xFFFFFFFFu, 0 };
        ta(q0); ta(q1); ta(q2); ta(q3); ta(eol);
        pvr(0x014, 1);
        dc_pvr_gl_read(rgb, &w, &h);
        grey[m] = rgb[(64 * w + 64) * 3];
    }
    CHECK(grey[1] > 80 && grey[1] < 175, "mipmapped checkerboard %d, want grey", grey[1]);
    dc_pvr_gl_mipmaps = 0;

    /* modifier volumes, in both depth modes and across the switches between them: a red quad (Shadow bit)
     * under one volume triangle in front of it (cheap shadow: half as bright) and one behind it (no
     * effect). With depth from the vertices the depth buffer has no stencil until the first frame with a
     * volume (pvr_gl.c zv_stencil): the first pass is that frame. */
    for (int pass = 0; pass < 3; pass++) {
        dc_pvr_gl_fast_depth = pass != 1;
        pvr(0x074, 0x100 | 128);                                               /* FPU_SHAD_SCALE: cheap shadow, 128/256 */
        pvr(0x128, 0); pvr(0x144, 0x80000000u);
        uint32_t qh[8] = { (4u << 29) | (0u << 24) | (1u << 7), (7u << 29), (1u << 29) | (2u << 22), 0, 0, 0, 0, 0 };
        uint32_t q0[8] = { 7u << 29, fu(0), fu(0), fu(1), 0, 0, 0xFFFF0000u, 0 }, q1[8] = { 7u << 29, fu(64), fu(0), fu(1), 0, 0, 0xFFFF0000u, 0 };
        uint32_t q2[8] = { 7u << 29, fu(0), fu(64), fu(1), 0, 0, 0xFFFF0000u, 0 }, q3[8] = { (7u << 29) | (1u << 28), fu(64), fu(64), fu(1), 0, 0, 0xFFFF0000u, 0 };
        ta(qh); ta(q0); ta(q1); ta(q2); ta(q3); ta(eol);
        uint32_t mh[8] = { (4u << 29) | (1u << 24), 1u << 29, 0, 0, 0, 0, 0, 0 };   /* opaque volumes: "inside last polygon", one triangle each */
        uint32_t mfront[16] = { 7u << 29, fu(2), fu(2), fu(2), fu(30), fu(2), fu(2), fu(2), fu(30), fu(2), 0, 0, 0, 0, 0, 0 };
        uint32_t mback[16] = { 7u << 29, fu(40), fu(40), fu(0.5f), fu(62), fu(40), fu(0.5f), fu(40), fu(62), fu(0.5f), 0, 0, 0, 0, 0, 0 };
        ta(mh); ta(mfront); ta(mfront + 8); ta(mh); ta(mback); ta(mback + 8); ta(eol);
        pvr(0x014, 1);
        dc_pvr_gl_read(rgb, &w, &h);
        const uint8_t *in_ = rgb + (10 * w + 10) * 3, *behind = rgb + (100 * w + 100) * 3, *beside = rgb + (10 * w + 100) * 3;
        const char *mode = pass == 1 ? "depth by logarithm" : pass ? "depth from the vertices again" : "depth from the vertices";
        CHECK(in_[0] >= 127 && in_[0] <= 129 && in_[1] == 0 && in_[2] == 0, "%s: shadowed pixel %02X%02X%02X, want 800000", mode, in_[0], in_[1], in_[2]);
        CHECK(behind[0] == 255 && behind[1] == 0, "%s: volume behind the surface %02X%02X%02X", mode, behind[0], behind[1], behind[2]);
        CHECK(beside[0] == 255 && beside[1] == 0, "%s: outside the volume %02X%02X%02X", mode, beside[0], beside[1], beside[2]);
    }
    pvr(0x074, 0);
    dc_pvr_gl_fast_depth = 1;
    dc_front_close();
    printf(fails ? "%d frontend checks failed\n" : "frontend tests passed\n", fails);
    return fails != 0;
}

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

/* A 256 x 128 scene of 16-pixel cells, one polygon each, between them in every render state the
 * renderer builds its programs for: textured or not, the four shading instructions, vertex alpha,
 * texture alpha ignored, offset colour, Gouraud or flat, the four fog modes; opaque, translucent
 * (some drawn in two passes: dc_pvr_gl_topaque), punch-through, two-volume polygons half inside a
 * volume, and flat HUD pieces (bicubic, clamped to their part of the texture). */
#define TEX4444 0x110000u
static void cell(int col, int row, int list, uint32_t st, float z0, float dz) {
    int tex = st & 1, shade = (st >> 1) & 3, use_alpha = (st >> 3) & 1, ign_ta = (st >> 4) & 1, offset = (st >> 5) & 1, gouraud = (st >> 6) & 1,
        fog = (st >> 7) & 3, vol = (st >> 9) & 1, blend = (st >> 10) & 1, point = (st >> 11) & 1, sub = (st >> 12) & 1;
    uint32_t pcw = (4u << 29) | ((uint32_t)list << 24) | ((uint32_t)vol << 6) | ((uint32_t)tex << 3) | ((uint32_t)offset << 2) | ((uint32_t)gouraud << 1);
    uint32_t isp = ((blend ? 4u : 6u) << 29);
    uint32_t tsp = ((blend ? 4u : 1u) << 29) | ((blend ? 5u : 0u) << 26) | ((uint32_t)fog << 22) | ((uint32_t)use_alpha << 20) | ((uint32_t)ign_ta << 19)
                 | ((point ? 0u : 1u) << 13) | ((uint32_t)shade << 6) | (3u << 3) | 3u;
    uint32_t tcw = (2u << 27) | (1u << 26) | (TEX4444 >> 3);
    uint32_t h[8] = { pcw, isp, tsp, tex ? tcw : 0, vol ? tsp ^ (1u << 20) : 0, 0, 0, 0 };
    ta(h);
    static const uint32_t base[4] = { 0xFFE04020u, 0x8020C0F0u, 0xC0F0F040u, 0x4080FF80u }, spec[4] = { 0x40101030u, 0x80302010u, 0xC0002040u, 0xFF403000u };
    for (int k = 0; k < 4; k++) {
        float x = (float)(col * 16 + (k & 1) * 15), y = (float)(row * 16 + (k >> 1) * 15), z = z0 + dz * (float)k;
        float u = sub ? 0.25f + 0.5f * (float)(k & 1) : -0.25f + 1.5f * (float)(k & 1), v = sub ? 0.125f + 0.75f * (float)(k >> 1) : 1.25f * (float)(k >> 1);
        uint32_t c = base[(k + col) & 3], o = spec[(k + row) & 3];
        uint32_t w[8] = { (7u << 29) | (k == 3 ? 1u << 28 : 0), fu(x), fu(y), fu(z), tex ? fu(u) : 0, tex ? fu(v) : 0, c, o };
        if (vol) { w[4] = c; w[5] = base[(k + col + 2) & 3] ^ 0x00FFFFFFu; w[6] = w[7] = 0; }   /* two volumes, untextured: both sets' colours */
        ta(w);
    }
}
/* a translucent quad (blended by alpha, depth test "greater", no fog, point-sampled), 1/w zl at its left
 * edge and zr at its right; textured (decal) when tcw is not 0, else in the colour argb */
static void tquad(float x0, float y0, float x1, float y1, float zl, float zr, uint32_t tcw, uint32_t argb) {
    uint32_t h[8] = { (4u << 29) | (2u << 24) | (tcw ? 1u << 3 : 0), 4u << 29, (4u << 29) | (5u << 26) | (2u << 22) | (1u << 20) | (3u << 3) | 3u, tcw, 0, 0, 0, 0 };
    ta(h);
    for (int k = 0; k < 4; k++) {
        uint32_t w[8] = { (7u << 29) | (k == 3 ? 1u << 28 : 0), fu(k & 1 ? x1 : x0), fu(k >> 1 ? y1 : y0), fu(k & 1 ? zr : zl),
                          tcw ? fu((float)(k & 1)) : 0, tcw ? fu((float)(k >> 1)) : 0, argb, 0 };
        ta(w);
    }
}
static void state_scene(void) {
    uint32_t eol[8] = {0};
    for (int row = 0; row < 4; row++) for (int col = 0; col < 16; col++)                 /* opaque: 64 of the 512 states */
        cell(col, row, 0, (uint32_t)((row * 16 + col) * 37 + 11) & 511u, 1.0f + 0.01f * (float)col, 0.25f);
    for (int col = 0; col < 16; col++) cell(col, 4, 0, (uint32_t)(col & 1) << 6 | (uint32_t)((col >> 1) & 3) << 7 | (1u << 3) | (1u << 9), 1.0f, 0.125f);   /* two volumes */
    for (int col = 0; col < 16; col++)                                                  /* HUD: flat, 1/w 1000 */
        cell(col, 7, 0, 1u | (uint32_t)(col & 3) << 1 | (1u << 3) | (uint32_t)((col >> 2) & 1) << 11 | (uint32_t)((col >> 3) & 1) << 12 | (uint32_t)(col % 3 == 0) << 4, 1000.f, 0.f);
    ta(eol);
    {   /* an opaque volume in front of the left half of the two-volume row */
        uint32_t mh[8] = { (4u << 29) | (1u << 24), 1u << 29, 0, 0, 0, 0, 0, 0 };
        uint32_t tri[16] = { 7u << 29, fu(0), fu(60), fu(8), fu(250), fu(60), fu(8), fu(0), fu(84), fu(8), 0, 0, 0, 0, 0, 0 };
        ta(mh); ta(tri); ta(tri + 8); ta(eol);
    }
    for (int col = 0; col < 16; col++)                                                  /* translucent: blended, half of them in two passes */
        cell(col, 5, 2, 1u | (uint32_t)(col & 3) << 1 | (1u << 3) | (uint32_t)((col >> 2) & 1) << 5 | (1u << 6) | (uint32_t)((col >> 3) & 1) * (2u << 7) | (1u << 10), 1.0f, col & 8 ? 0.f : 0.25f);
    ta(eol);
    for (int col = 0; col < 16; col++)                                                  /* punch-through */
        cell(col, 6, 4, 1u | (uint32_t)(col & 3) << 1 | (1u << 3) | (uint32_t)((col >> 2) & 1) << 6 | (uint32_t)((col >> 3) & 1) << 7 | (2u << 7) * (uint32_t)!(col & 8), 1.0f, 0.25f);
    ta(eol);
}

int main(void) {
    sh4ctx c; dc_init(&c);
    dc_front_config cfg = { .title = "dc-core test", .scale = 2, .no_pacing = 1, .no_vmu = 1 };
    dc_pvr_gl_shader_list = "";                             /* no list of shader variants from an earlier run, and none left behind */
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

    /* Translucent polygons the sort by depth gets wrong, and the opaque fragments first (dc_pvr_gl_topaque).
     * Three rows, each a deep quad (1/w 1 at its left, 3 at its right: a railing along a deck) and a quad at
     * 1/w 2 over all of it: sorted by their farthest points the deep one is drawn first, so the other covers it
     * also on the right, where the deep one is the nearer. Row 1: a cut-out texture (opaque red in its upper
     * half) under a half see-through green; row 2: the same with a texture that fades (opaque in a quarter,
     * half see-through in the rest); row 3: solid red under solid green. */
    {
#define TEXCUT 0x120000u
#define TEXFADE 0x124000u
        for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
            uint16_t cut = y < 32 ? 0xFC00 : 0x7C00, fade = y < 16 ? 0xFF00 : 0x8F00;      /* ARGB1555, ARGB4444: red */
            dc_wr8(0xA4000000u + TEXCUT + (uint32_t)(y * 64 + x) * 2, cut & 255); dc_wr8(0xA4000000u + TEXCUT + (uint32_t)(y * 64 + x) * 2 + 1, cut >> 8);
            dc_wr8(0xA4000000u + TEXFADE + (uint32_t)(y * 64 + x) * 2, fade & 255); dc_wr8(0xA4000000u + TEXFADE + (uint32_t)(y * 64 + x) * 2 + 1, fade >> 8);
        }
        for (int pass = 0; pass < 6; pass++) {
            int mode = pass % 3, zv = pass < 3;
            unsigned long c0[5], c1[5];
            dc_pvr_gl_topaque = mode; dc_pvr_gl_fast_depth = zv;
            pvr(0x128, 0); pvr(0x144, 0x80000000u);
            ta(eol);                                                           /* nothing opaque */
            tquad(8, 4, 56, 20, 1.f, 3.f, (0u << 27) | (1u << 26) | (TEXCUT >> 3), 0xFFFFFFFFu);
            tquad(8, 4, 56, 20, 2.f, 2.01f, 0, 0x8000FF00u);
            tquad(8, 24, 56, 40, 1.f, 3.f, (2u << 27) | (1u << 26) | (TEXFADE >> 3), 0xFFFFFFFFu);
            tquad(8, 24, 56, 40, 2.f, 2.01f, 0, 0x8000FF00u);
            tquad(8, 44, 56, 60, 1.f, 3.f, 0, 0xFFFF0000u);
            tquad(8, 44, 56, 60, 2.f, 2.01f, 0, 0xFF00FF00u);
            ta(eol);
            dc_pvr_gl_counts(c0);
            pvr(0x014, 1);
            dc_pvr_gl_counts(c1);
            dc_pvr_gl_read(rgb, &w, &h);
            /* at 2x: the right part (the deep quad nearer) at x 96, the left part at 32; rows at y 12 (in the
             * cut-out's and the fading texture's opaque part: 12 and 52), 36 (the cut-out's clear part), 104 */
            const uint8_t *cut_r = rgb + (12 * w + 96) * 3, *cut_l = rgb + (12 * w + 32) * 3, *clear_r = rgb + (36 * w + 96) * 3;
            const uint8_t *fade_r = rgb + (52 * w + 96) * 3, *solid_r = rgb + (104 * w + 96) * 3, *solid_l = rgb + (104 * w + 32) * 3;
            const char *dm = zv ? "depth from the vertices" : "depth by logarithm";
#define IS_RED(p) ((p)[0] == 255 && (p)[1] == 0 && (p)[2] == 0)
#define IS_GREEN(p) ((p)[0] == 0 && (p)[1] == 255 && (p)[2] == 0)
#define IS_MIX(p) ((p)[0] >= 120 && (p)[0] <= 135 && (p)[1] >= 120 && (p)[1] <= 135 && (p)[2] == 0)      /* half green over red */
            CHECK(w == 128 && h == 128, "sorting scene %dx%d, want 128x128", w, h);
            CHECK(IS_MIX(cut_l), "%s, opaque first %d: a cut-out behind see-through green %02X%02X%02X, want green over red", dm, mode, cut_l[0], cut_l[1], cut_l[2]);
            CHECK(clear_r[0] < 8 && clear_r[1] >= 120 && clear_r[1] <= 135, "%s, opaque first %d: the cut-out's clear half %02X%02X%02X, want green over the background", dm, mode, clear_r[0], clear_r[1], clear_r[2]);
            CHECK(IS_GREEN(solid_l), "%s, opaque first %d: solid red behind solid green %02X%02X%02X", dm, mode, solid_l[0], solid_l[1], solid_l[2]);
            if (mode == 0) {                                                   /* sorted only: the deep quads are covered */
                CHECK(IS_MIX(cut_r) && IS_MIX(fade_r) && IS_GREEN(solid_r), "%s, sorted only: %02X%02X%02X %02X%02X%02X %02X%02X%02X, want the nearer deep quads covered",
                      dm, cut_r[0], cut_r[1], cut_r[2], fade_r[0], fade_r[1], fade_r[2], solid_r[0], solid_r[1], solid_r[2]);
            } else {
                CHECK(IS_RED(cut_r), "%s, opaque first %d: a cut-out before see-through green %02X%02X%02X, want red", dm, mode, cut_r[0], cut_r[1], cut_r[2]);
                CHECK(IS_RED(solid_r), "%s, opaque first %d: solid red before solid green %02X%02X%02X, want red", dm, mode, solid_r[0], solid_r[1], solid_r[2]);
                /* a texture that fades: its opaque part first with 1, all of it sorted (covered as before) with 2 */
                CHECK(mode == 1 ? IS_RED(fade_r) : IS_MIX(fade_r), "%s, opaque first %d: a fading texture's opaque part before see-through green %02X%02X%02X, want %s",
                      dm, mode, fade_r[0], fade_r[1], fade_r[2], mode == 1 ? "red" : "green over red");
            }
            /* what is drawn: sorted, the four untextured quads follow each other in one state (3 changes of
             * state with the two textured ones). In two passes a polygon is drawn twice only where it has both
             * kinds of fragment (the two textured quads with 1, the cut-out alone with 2): the solid pair goes
             * out once, in pass 1, and the see-through pair once, in pass 2 */
            unsigned long want = mode == 0 ? 3 : mode == 1 ? 6 : 5;
            CHECK(c1[2] - c0[2] == want, "%s, opaque first %d: %lu state changes, want %lu", dm, mode, c1[2] - c0[2], want);
        }
        dc_pvr_gl_topaque = 0; dc_pvr_gl_fast_depth = 1;

        /* See-through sheets (dc_pvr_gl_tsheets). A half see-through green sheet over the right half, deep, and a
         * solid red quad a fifth nearer than its plane that also reaches further left, where that plane is
         * further off: sorted by their farthest points the red one is drawn first and the sheet covers it. With
         * the option the sheet goes first. In the second row a solid blue quad in the sheet's own plane was
         * submitted between them: the sheet does not move past it (layers on one surface keep their order). */
        for (int on = 0; on < 2; on++) {
            dc_pvr_gl_tsheets = on;
            pvr(0x128, 0); pvr(0x144, 0x80000000u);
            ta(eol);
            tquad(0, 4, 56, 28, 1.2f * 0.5f, 1.2f * 4.f, 0, 0xFFFF0000u);              /* red, before the plane 1/w = 0.5 + x / 16 */
            tquad(30, 4, 56, 28, 0.5f + 30.f / 16.f, 4.f, 0, 0x8000FF00u);             /* the sheet */
            tquad(0, 36, 56, 60, 1.2f * 0.5f, 1.2f * 4.f, 0, 0xFFFF0000u);
            tquad(20, 36, 56, 60, 0.5f + 20.f / 16.f, 4.f, 0, 0xFF0000FFu);            /* blue, in the plane */
            tquad(30, 36, 56, 60, 0.5f + 30.f / 16.f, 4.f, 0, 0x8000FF00u);
            ta(eol);
            pvr(0x014, 1);
            dc_pvr_gl_read(rgb, &w, &h);
            const uint8_t *free_ = rgb + (32 * w + 88) * 3, *held = rgb + (96 * w + 88) * 3, *left = rgb + (32 * w + 30) * 3;
            CHECK(IS_RED(left), "sheets %d: beside the sheet %02X%02X%02X, want red", on, left[0], left[1], left[2]);
            CHECK(on ? IS_RED(free_) : IS_MIX(free_), "sheets %d: red before a see-through sheet %02X%02X%02X, want %s", on, free_[0], free_[1], free_[2],
                  on ? "red (the sheet first)" : "green over red (sorted by farthest point)");
            CHECK(held[0] == 0 && held[1] >= 120 && held[1] <= 135 && held[2] >= 120 && held[2] <= 135,
                  "sheets %d: a sheet after a layer in its plane %02X%02X%02X, want green over blue", on, held[0], held[1], held[2]);
        }
        dc_pvr_gl_tsheets = 0;
    }

    /* One program for every render state against a program per state (pvr_gl.c sv_uber, dc_pvr_gl_variants):
     * the same picture to the bit, in both depth modes and without anti-aliasing (punch-through then cuts
     * by itself, not by coverage), and no changes of program where there is one. */
    {
        for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {            /* ARGB4444, scan order: alpha runs across */
            uint16_t px = (uint16_t)(((x >> 2) << 12) | ((y >> 2) << 8) | (((x ^ y) & 15) << 4) | (15 - (x >> 2)));
            dc_wr8(0xA4000000u + TEX4444 + (uint32_t)(y * 64 + x) * 2, px & 255); dc_wr8(0xA4000000u + TEX4444 + (uint32_t)(y * 64 + x) * 2 + 1, px >> 8);
        }
        pvr(0x068, 255u << 16); pvr(0x06C, 127u << 16);                        /* 256 x 128 */
        pvr(0x0B0, 0x00406080u); pvr(0x0B4, 0x00804020u); pvr(0x0B8, 0x8007u); /* fog colours; density 128 */
        for (int i = 0; i < 128; i++) pvr(0x200 + (uint32_t)i * 4, (uint32_t)((255 - 2 * i) << 8 | (254 - 2 * i)));
        pvr(0x11C, 0x60);                                                      /* punch-through alpha reference */
        dc_pvr_gl_hud_z = 500.f; dc_pvr_gl_topaque = 1;
        static uint8_t pic[2][512 * 256 * 3];
        int aa = dc_pvr_gl_msaa;
        for (int pass = 0; pass < 3; pass++) {
            unsigned long progs[2] = { 0, 0 }, states[2] = { 0, 0 };
            int zv = pass != 1;
            dc_pvr_gl_fast_depth = zv; dc_pvr_gl_msaa = pass == 2 ? 0 : aa;
            for (int m = 0; m < 2; m++) {
                unsigned long c0[5], c1[5];
                dc_pvr_gl_variants = !m;
                pvr(0x128, 0); pvr(0x144, 0x80000000u);
                state_scene();
                dc_pvr_gl_counts(c0);
                pvr(0x014, 1);
                dc_pvr_gl_counts(c1);
                progs[m] = c1[3] - c0[3]; states[m] = c1[2] - c0[2];
                w = h = 0;
                dc_pvr_gl_read(pic[m], &w, &h);
                CHECK(w == 512 && h == 256, "state scene %dx%d, want 512x256", w, h);
            }
            const char *mode = pass == 2 ? "no anti-aliasing" : zv ? "depth from the vertices" : "depth by logarithm";
            long diff = 0, drawn = 0, first = -1;
            for (long i = 0; i < 512L * 256; i++) {
                const uint8_t *a = pic[0] + i * 3, *b = pic[1] + i * 3;
                if (a[0] != b[0] || a[1] != b[1] || a[2] != b[2]) { diff++; if (first < 0) first = i; }
                if (!(a[0] == 0 && a[1] == 0 && a[2] == 255)) drawn++;
            }
            CHECK(drawn > 60000, "%s: the state scene drew %ld pixels, want most of 131072", mode, drawn);
            CHECK(diff == 0, "%s: one program and a program per state differ in %ld pixels, first at %ld,%ld: %02X%02X%02X against %02X%02X%02X", mode, diff,
                  first % 512, first / 512, pic[1][first * 3], pic[1][first * 3 + 1], pic[1][first * 3 + 2], pic[0][first * 3], pic[0][first * 3 + 1], pic[0][first * 3 + 2]);
            CHECK(states[0] == states[1] && states[0] >= 100, "%s: %lu and %lu state changes, want the same and 100 or more", mode, states[0], states[1]);
            CHECK(progs[0] >= 40, "%s: a program per state changed program %lu times, want 40 or more (the scene's states)", mode, progs[0]);
            CHECK(progs[1] <= 4, "%s: one program for every state changed program %lu times", mode, progs[1]);
        }
        dc_pvr_gl_variants = -1; dc_pvr_gl_hud_z = 0; dc_pvr_gl_topaque = 0; dc_pvr_gl_fast_depth = 1; dc_pvr_gl_msaa = aa;

        /* The anti-aliasing buffers keep the size of the largest picture while pictures of that size still
         * come (pvr_gl.c ensure_ms), and are made again for what is drawn once 300 frames went without one. */
        char d[256];
        pvr(0x128, 0); pvr(0x144, 0x80000000u); state_scene(); pvr(0x014, 1);
        dc_pvr_gl_describe(d, sizeof d);
        CHECK(strstr(d, "512x256, ") == d && strstr(d, "anti-aliasing in 512x256"), "after a 256x128 scene: \"%s\"", d);
        pvr(0x068, 63u << 16); pvr(0x06C, 63u << 16);
        for (int i = 0; i < 302; i++) {
            pvr(0x128, 0); pvr(0x144, 0x80000000u);
            ta(hdr); ta(v0); ta(v1); ta(v2); ta(eol);
            pvr(0x014, 1);
            if (i == 298 || i == 301) {
                dc_pvr_gl_describe(d, sizeof d);
                CHECK(strstr(d, "128x128, ") == d && strstr(d, i == 298 ? "anti-aliasing in 512x256" : "anti-aliasing in 128x128"),
                      "%d frames of 64x64 after it: \"%s\"", i + 1, d);
            }
        }
    }
    dc_front_close();
    printf(fails ? "%d frontend checks failed\n" : "frontend tests passed\n", fails);
    return fails != 0;
}

/* dc-core: PowerVR2 (CLX2) software renderer.
 *
 * Collects the polygons the game sends through the Tile Accelerator during a frame and, at
 * STARTRENDER, draws them into the framebuffer in VRAM the way the PVR core would: background
 * plane, opaque list (depth tested), punch-through (alpha tested), translucent (sorted or in
 * submission order), with the PVR's texture formats, shading instructions and blending. The video
 * output side (dc_pvr_display) reads back whatever framebuffer the display registers point at.
 *
 * It is the reference the GPU backends are checked against, and makes frames visible without a
 * window (tools/probe DC_SHOT). Not covered yet: modifier volumes (shadows), fog, mip-mapping
 * beyond the top level, bump maps.
 */
#include "dccore/pvr.h"
#include "dccore/hw.h"
#include "dccore/ta.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define REG(o) dc_pvr_reg(o)
int dc_pvr_sw_debug;          /* non-zero: describe the next rendered frame on stderr */
int dc_pvr_sw_highlight;      /* debug: paint one texture class magenta (1 VQ+mip, 2 non-square twiddled, 3 ARGB4444 VQ, 4 other) */
static int highlighted(uint32_t tcw, uint32_t tsp) {
    int vq = (tcw >> 30) & 1, mip = tcw >> 31, fmt = (tcw >> 27) & 7, sq = ((tsp >> 3) & 7) == (tsp & 7), lin = (tcw >> 26) & 1;
    switch (dc_pvr_sw_highlight) {
    case 1: return vq && mip;
    case 2: return !vq && !lin && !sq;
    case 3: return vq && fmt == 2;
    case 4: return !(vq && mip) && !(!vq && !lin && !sq) && !(vq && fmt == 2);
    default: return 0;
    }
}

/* ------------------------------------------------------------------ VRAM views */
static uint32_t vram32_ofs(uint32_t o) {           /* 32-bit area offset -> storage offset */
    o &= 0x7FFFFF;
    uint32_t bank = (o >> 22) & 1;
    return ((o & 3) | ((o & 0x3FFFFC) << 1) | (bank << 2)) & 0x7FFFFF;
}
static uint32_t rd32_area32(uint32_t o) {
    uint32_t v = 0;
    for (int k = 0; k < 4; k++) v |= (uint32_t)dc_vram[vram32_ofs(o + k)] << (8 * k);
    return v;
}
static void wr8_area32(uint32_t o, uint8_t v) {
    uint32_t so = vram32_ofs(o);
    if ((o & 0xFFF) == 0 || (o & 3) == 0) dc_vram_gen[so >> 12]++;    /* framebuffer writes invalidate textures there */
    dc_vram[so] = v;
}
static uint16_t tex16(uint32_t o) { o &= 0x7FFFFE; return (uint16_t)(dc_vram[o] | dc_vram[o + 1] << 8); }
static uint8_t tex8(uint32_t o) { return dc_vram[o & 0x7FFFFF]; }

/* ------------------------------------------------------------------ display list */
typedef struct { float x, y, z, u, v; float c[4], o[4]; float u1, v1, c1[4], o1[4]; } vtx;   /* colours r,g,b,a 0..1; *1 = second volume */
typedef struct {
    uint32_t isp, tsp, tcw, tsp1, tcw1;
    int list, first, n, sprite;
    int twovol, shadow;                            /* PCW Volume (two parameter sets) / Shadow bits */
} poly;
typedef struct { float x[3], y[3], z[3]; int inst, list; } modtri;   /* modifier volume triangle */

/* The TA fills one context while the core renders another: contexts are keyed by TA_ISP_BASE at
 * list init and STARTRENDER draws the one whose base equals PARAM_BASE. */
typedef struct { uint32_t base; int used; vtx *vt; int nvt, cap_vt; poly *pl; int npl, cap_pl; modtri *mt; int nmt, cap_mt; unsigned age; } ctx_t;
#define NCTX 4
static ctx_t ctxs[NCTX];
static ctx_t *ta_ctx = &ctxs[0];
static unsigned ctx_clock;
#define vt (ta_ctx->vt)
#define nvt (ta_ctx->nvt)
#define cap_vt (ta_ctx->cap_vt)
#define pl (ta_ctx->pl)
#define npl (ta_ctx->npl)
#define cap_pl (ta_ctx->cap_pl)
#define mt (ta_ctx->mt)
#define nmt (ta_ctx->nmt)
#define cap_mt (ta_ctx->cap_mt)
static void list_init(void) {
    uint32_t base = REG(0x128) & 0xFFFFFF;
    ctx_t *c = NULL, *old = &ctxs[0];
    for (int i = 0; i < NCTX; i++) { if (ctxs[i].used && ctxs[i].base == base) c = &ctxs[i]; if (ctxs[i].age < old->age) old = &ctxs[i]; }
    if (!c) c = old;
    c->base = base; c->used = 1; c->age = ++ctx_clock;
    ta_ctx = c;
    npl = 0; nvt = 0; nmt = 0;
}
static struct {
    uint32_t pcw, isp, tsp, tcw, tsp1, tcw1;
    float face[4], face_ofs[4], face1[4];
    int mod_inst;                                  /* modifier volume instruction of the last header */
    int open;                                      /* index of the strip being filled, -1 none */
    int sprite;                                    /* the last header was a sprite header */
} cur = { .open = -1 };

static vtx *new_vtx(void) {
    if (nvt == cap_vt) { cap_vt = cap_vt ? cap_vt * 2 : 4096; vt = realloc(vt, cap_vt * sizeof *vt); }
    memset(&vt[nvt], 0, sizeof *vt);
    return &vt[nvt++];
}
static poly *new_poly(int list, int sprite) {
    if (npl == cap_pl) { cap_pl = cap_pl ? cap_pl * 2 : 1024; pl = realloc(pl, cap_pl * sizeof *pl); }
    poly *p = &pl[npl++];
    p->isp = cur.isp; p->tsp = cur.tsp; p->tcw = cur.tcw; p->tsp1 = cur.tsp1; p->tcw1 = cur.tcw1;
    p->list = list; p->first = nvt; p->n = 0; p->sprite = sprite;
    p->twovol = !sprite && ((cur.pcw >> 6) & 1); p->shadow = (cur.pcw >> 7) & 1;
    return p;
}

static float f32(uint32_t w) { float f; memcpy(&f, &w, 4); return f; }
static void unpack(uint32_t argb, float *c) {
    c[0] = ((argb >> 16) & 255) / 255.f; c[1] = ((argb >> 8) & 255) / 255.f;
    c[2] = (argb & 255) / 255.f; c[3] = (argb >> 24) / 255.f;
}
static void float_col(const uint32_t *w, float *c) {      /* A R G B floats */
    c[3] = f32(w[0]); c[0] = f32(w[1]); c[1] = f32(w[2]); c[2] = f32(w[3]);
    for (int i = 0; i < 4; i++) c[i] = c[i] < 0 ? 0 : c[i] > 1 ? 1 : c[i];
}
static void intensity(const float *face, float i, float *c) {
    i = i < 0 ? 0 : i > 1 ? 1 : i;
    c[0] = face[0] * i; c[1] = face[1] * i; c[2] = face[2] * i; c[3] = face[3];
}
static float uv16(uint32_t w, int hi) { return f32(hi ? (w & 0xFFFF0000u) : (w << 16)); }

/* the TA takes Texture / Offset / Gouraud / 16-bit UV from the parameter control word and puts them
 * into the ISP/TSP instruction word (bits 25..22); what the game wrote there is ignored */
static uint32_t isp_from_pcw(uint32_t isp, uint32_t pcw) { return (isp & ~(0xFu << 22)) | ((pcw & 0xF) << 22); }

static void sink(int kind, int list, const uint32_t *w, int words) {
    switch (kind) {
    case -1: list_init(); cur.open = -1; cur.pcw = 0; cur.sprite = 0; return;
    case 4:                                           /* polygon header (or modifier volume) */
        cur.open = -1;
        cur.sprite = 0;
        if (list == 1 || list == 3) { cur.pcw = 0; cur.mod_inst = (int)(w[1] >> 29); return; }
        cur.pcw = w[0]; cur.isp = isp_from_pcw(w[1], w[0]); cur.tsp = w[2]; cur.tcw = w[3];
        {
            int col = (w[0] >> 4) & 3, vol = (w[0] >> 6) & 1, ofs = (w[0] >> 2) & 1;
            if (vol) { cur.tsp1 = w[4]; cur.tcw1 = w[5]; } else { cur.tsp1 = cur.tsp; cur.tcw1 = cur.tcw; }
            if (col == 2) {
                if (words == 16 && vol) { float_col(w + 8, cur.face); float_col(w + 12, cur.face1); memset(cur.face_ofs, 0, sizeof cur.face_ofs); }
                else if (words == 16 && ofs) { float_col(w + 8, cur.face); float_col(w + 12, cur.face_ofs); }
                else float_col(w + 4, cur.face);
                if (!vol) memcpy(cur.face1, cur.face, sizeof cur.face1);
            }
        }
        return;
    case 5:                                           /* sprite header */
        cur.open = -1;
        cur.pcw = w[0]; cur.sprite = 1;
        cur.isp = isp_from_pcw(w[1], w[0]) & ~(1u << 23); cur.tsp = cur.tsp1 = w[2]; cur.tcw = cur.tcw1 = w[3];   /* sprites are flat */
        unpack(w[4], cur.face); unpack(w[5], cur.face_ofs);
        return;
    case 7: {                                         /* vertex */
        if (list == 1 || list == 3) {                 /* modifier volume triangle: x,y,z x 3 */
            if (words < 16) return;
            if (nmt == cap_mt) { cap_mt = cap_mt ? cap_mt * 2 : 512; mt = realloc(mt, cap_mt * sizeof *mt); }
            modtri *m = &mt[nmt++];
            for (int k = 0; k < 3; k++) { m->x[k] = f32(w[1 + 3 * k]); m->y[k] = f32(w[2 + 3 * k]); m->z[k] = f32(w[3 + 3 * k]); }
            m->inst = cur.mod_inst; m->list = list;
            return;
        }
        if (!cur.pcw) return;
        if (cur.sprite) {                             /* sprite: one quad per parameter */
            poly *p = new_poly(list, 1);
            int tex = (cur.isp >> 25) & 1;
            float x[4] = { f32(w[1]), f32(w[4]), f32(w[7]), f32(w[10]) }, y[4] = { f32(w[2]), f32(w[5]), f32(w[8]), f32(w[11]) };
            float z[4] = { f32(w[3]), f32(w[6]), f32(w[9]), 0 };
            /* D lies on the plane of A, B, C */
            float ux = x[1] - x[0], uy = y[1] - y[0], uz = z[1] - z[0], vx = x[2] - x[0], vy = y[2] - y[0], vz = z[2] - z[0];
            float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
            z[3] = nz != 0 ? z[0] - (nx * (x[3] - x[0]) + ny * (y[3] - y[0])) / nz : z[0];
            float u[4] = {0}, v[4] = {0};
            if (tex && words == 16) {
                u[0] = uv16(w[13], 1); v[0] = uv16(w[13], 0); u[1] = uv16(w[14], 1); v[1] = uv16(w[14], 0);
                u[2] = uv16(w[15], 1); v[2] = uv16(w[15], 0); u[3] = u[0] + u[2] - u[1]; v[3] = v[0] + v[2] - v[1];
            }
            for (int i = 0; i < 4; i++) {
                vtx *t = new_vtx(); t->x = x[i]; t->y = y[i]; t->z = z[i]; t->u = u[i]; t->v = v[i];
                memcpy(t->c, cur.face, sizeof t->c); memcpy(t->o, cur.face_ofs, sizeof t->o);
            }
            p->n = 4;
            return;
        }
        if (cur.open < 0) { new_poly(list, 0); cur.open = npl - 1; }
        vtx *t = new_vtx();
        t->x = f32(w[1]); t->y = f32(w[2]); t->z = f32(w[3]);
        uint32_t pcw = cur.pcw;
        int tex = (pcw >> 3) & 1, col = (pcw >> 4) & 3, vol = (pcw >> 6) & 1, u16 = pcw & 1;
        if (!tex) {
            if (!vol) {
                if (col == 0) unpack(w[6], t->c);
                else if (col == 1) float_col(w + 4, t->c);
                else intensity(cur.face, f32(w[6]), t->c);
            } else {
                if (col == 0) { unpack(w[4], t->c); unpack(w[5], t->c1); }
                else { intensity(cur.face, f32(w[4]), t->c); intensity(cur.face1, f32(w[5]), t->c1); }
            }
        } else {
            if (u16) { t->u = uv16(w[4], 1); t->v = uv16(w[4], 0); } else { t->u = f32(w[4]); t->v = f32(w[5]); }
            if (!vol) {
                if (col == 0) { unpack(w[6], t->c); unpack(w[7], t->o); }
                else if (col == 1) { if (words == 16) { float_col(w + 8, t->c); float_col(w + 12, t->o); } }
                else { intensity(cur.face, f32(w[6]), t->c); intensity(cur.face_ofs, f32(w[7]), t->o); }
            } else if (words == 16) {                 /* two volumes: set 1 in the second half */
                if (u16) { t->u1 = uv16(w[8], 1); t->v1 = uv16(w[8], 0); } else { t->u1 = f32(w[8]); t->v1 = f32(w[9]); }
                if (col == 0) { unpack(w[6], t->c); unpack(w[7], t->o); unpack(w[10], t->c1); unpack(w[11], t->o1); }
                else { intensity(cur.face, f32(w[6]), t->c); intensity(cur.face_ofs, f32(w[7]), t->o);
                       intensity(cur.face1, f32(w[10]), t->c1); intensity(cur.face_ofs, f32(w[11]), t->o1); }
            }
        }
        if (!vol) { t->u1 = t->u; t->v1 = t->v; memcpy(t->c1, t->c, sizeof t->c1); memcpy(t->o1, t->o, sizeof t->o1); }
        pl[cur.open].n++;
        if ((w[0] >> 28) & 1) cur.open = -1;          /* end of strip */
        return;
    }
    default:
        return;
    }
}

/* ------------------------------------------------------------------ textures */
typedef struct { uint32_t tcw, tsp_size, pal_gen, gen, start, end; int w, h; uint32_t *px; uint64_t id; } tex_t;
#define TEX_CACHE 512
static tex_t tcache[TEX_CACHE];
static int ntc;

static uint32_t argb1555(uint16_t p) { uint32_t a = (p & 0x8000) ? 255 : 0, r = (p >> 10) & 31, g = (p >> 5) & 31, b = p & 31;
    return a << 24 | (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2); }
static uint32_t rgb565(uint16_t p) { uint32_t r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
    return 0xFF000000u | (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2); }
static uint32_t argb4444(uint16_t p) { uint32_t a = (p >> 12) & 15, r = (p >> 8) & 15, g = (p >> 4) & 15, b = p & 15;
    return (a * 17) << 24 | (r * 17) << 16 | (g * 17) << 8 | b * 17; }
static uint32_t conv16(int fmt, uint16_t p) { return fmt == 1 ? rgb565(p) : fmt == 2 ? argb4444(p) : argb1555(p); }
static uint32_t palette(uint32_t i) {
    uint32_t e = REG(0x1000 + (i & 1023) * 4), mode = REG(0x108) & 3;
    return mode == 0 ? argb1555((uint16_t)e) : mode == 1 ? rgb565((uint16_t)e) : mode == 2 ? argb4444((uint16_t)e) : e;
}
static uint32_t yuv_px(int y, int u, int v) {
    float r = y + 1.375f * (v - 128), g = y - 0.34375f * (u - 128) - 0.6875f * (v - 128), b = y + 1.71875f * (u - 128);
    int R = r < 0 ? 0 : r > 255 ? 255 : (int)r, G = g < 0 ? 0 : g > 255 ? 255 : (int)g, B = b < 0 ? 0 : b > 255 ? 255 : (int)b;
    return 0xFF000000u | R << 16 | G << 8 | B;
}
/* twiddled (Morton) index: square part interleaved (v in bit 0), rest of the longer side above */
static uint32_t twiddle(uint32_t u, uint32_t v, uint32_t w, uint32_t h) {
    uint32_t m = w < h ? w : h, r = 0, s = 0;
    for (uint32_t b = 1; b < m; b <<= 1, s += 2) { if (v & b) r |= 1u << s; if (u & b) r |= 2u << s; }
    uint32_t lm = 0; while ((1u << lm) < m) lm++;
    return r | (w > h ? (u >> lm) : (v >> lm)) << (2 * lm);
}
static const uint32_t mip16[11] = {0x3, 0x4, 0x8, 0x18, 0x58, 0x158, 0x558, 0x1558, 0x5558, 0x15558, 0x55558};  /* texels */
static const uint32_t mipvq[11] = {0x0, 0x1, 0x2, 0x6, 0x16, 0x56, 0x156, 0x556, 0x1556, 0x5556, 0x15556};      /* bytes */

/* sum of the write generations of the VRAM pages a texture occupies */
static uint32_t page_gen(uint32_t start, uint32_t end) {
    uint32_t g = 0;
    for (uint32_t p = start >> 12; p <= ((end - 1) >> 12) && p < 0x800; p++) g += dc_vram_gen[p] * (p + 1);
    return g;
}
static uint64_t tex_ids;
static int tc_next;
static tex_t *get_tex(uint32_t tcw, uint32_t tsp) {
    uint32_t sz = tsp & 0x3F;
    int palfmt = ((tcw >> 27) & 7) == 5 || ((tcw >> 27) & 7) == 6;
    for (int i = 0; i < ntc; i++) {
        tex_t *c = &tcache[i];
        if (c->tcw != tcw || c->tsp_size != sz) continue;
        if (page_gen(c->start, c->end) == c->gen && (!palfmt || c->pal_gen == dc_pal_gen)) return c;
        free(c->px); *c = tcache[--ntc];                     /* stale: decode again */
        break;
    }
    tex_t *t;
    if (ntc < TEX_CACHE) t = &tcache[ntc++];
    else { t = &tcache[tc_next++ % TEX_CACHE]; free(t->px); }
    int w = 8 << ((tsp >> 3) & 7), h = 8 << (tsp & 7);
    int fmt = (tcw >> 27) & 7, mip = (tcw >> 31) & 1, vq = (tcw >> 30) & 1;
    int linear = (tcw >> 26) & 1, stride = (tcw >> 25) & 1;
    uint32_t addr = (tcw & 0x1FFFFF) << 3;
    if (fmt == 5 || fmt == 6) linear = 0, stride = 0;       /* palette formats are always twiddled */
    int pitch = w;                                           /* texels per row in memory */
    if (linear && stride) { pitch = (int)(REG(0x0E4) & 31) * 32; if (pitch <= 0) pitch = w; }
    if (mip) h = w;                                          /* mip-mapped textures are square */
    t->tcw = tcw; t->tsp_size = sz; t->pal_gen = dc_pal_gen; t->w = w; t->h = h; t->id = ++tex_ids;
    t->px = malloc((size_t)w * h * 4);
    int lw = 0; while ((1 << lw) < w) lw++;
    uint32_t cb = addr;                                      /* VQ codebook: always at the texture address */
    if (mip) {
        if (vq) addr += mipvq[lw];
        else if (fmt == 5) addr += mip16[lw] / 2;             /* 4 bpp */
        else if (fmt == 6) addr += mip16[lw];                 /* 8 bpp */
        else addr += mip16[lw] * 2;
    }
    {   /* the bytes this texture reads, for change detection */
        uint32_t bytes = vq ? 2048 + (uint32_t)(w / 2) * (h / 2) : fmt == 5 ? (uint32_t)w * h / 2 : fmt == 6 ? (uint32_t)w * h
                       : (uint32_t)pitch * h * 2;
        t->start = cb & 0x7FFFFF; t->end = ((addr & 0x7FFFFF) + bytes + (vq ? 0 : 0)) ;
        if (vq) t->end = (addr & 0x7FFFFF) + 2048 + (uint32_t)(w / 2) * (h / 2);
        if (t->end > 0x800000) t->end = 0x800000;
        if (t->end <= t->start) t->end = t->start + 1;
        t->gen = page_gen(t->start, t->end);
    }
    uint32_t pal_base = fmt == 5 ? ((tcw >> 21) & 63) << 4 : ((tcw >> 25) & 3) << 8;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t c;
            if (vq) {                                       /* 2x2 blocks of 16-bit texels from a 256-entry codebook */
                uint32_t idx = tex8(addr + 2048 + twiddle(x >> 1, y >> 1, w >> 1, h >> 1));
                uint32_t sub = ((x & 1) << 1) | (y & 1);
                uint16_t p = tex16(cb + idx * 8 + sub * 2);
                c = fmt == 3 ? 0xFF808080u : conv16(fmt, p);
            } else if (fmt == 5) {
                uint32_t i = twiddle(x, y, w, h);
                uint8_t b = tex8(addr + i / 2);
                c = palette(pal_base + ((i & 1) ? b >> 4 : b & 15));
            } else if (fmt == 6) {
                c = palette(pal_base + tex8(addr + twiddle(x, y, w, h)));
            } else if (fmt == 3) {                          /* YUV422: pairs of texels share U/V */
                uint32_t i0 = linear ? (uint32_t)(y * pitch + (x & ~1)) : twiddle(x & ~1, y, w, h);
                uint32_t i1 = linear ? i0 + 1 : twiddle(x | 1, y, w, h);
                uint16_t p0 = tex16(addr + i0 * 2), p1 = tex16(addr + i1 * 2);
                int U = p0 & 255, V = p1 & 255, Y = (x & 1) ? p1 >> 8 : p0 >> 8;
                c = yuv_px(Y, U, V);
            } else {
                uint32_t i = linear ? (uint32_t)(y * pitch + x) : twiddle(x, y, w, h);
                c = conv16(fmt == 4 ? 1 : fmt == 7 ? 0 : fmt, tex16(addr + i * 2));
            }
            t->px[y * w + x] = c;
        }
    return t;
}

static int wrap(int i, int n, int clamp, int flip) {
    if (clamp) return i < 0 ? 0 : i >= n ? n - 1 : i;
    if (flip) { int p = i & (2 * n - 1); return p < n ? p : 2 * n - 1 - p; }
    return i & (n - 1);
}
static void sample(tex_t *t, uint32_t tsp, float u, float v, float *out) {
    int cu = (tsp >> 16) & 1, cv = (tsp >> 15) & 1, fu = (tsp >> 18) & 1, fv = (tsp >> 17) & 1;
    int filter = (tsp >> 13) & 3;
    float x = u * t->w - (filter ? 0.5f : 0), y = v * t->h - (filter ? 0.5f : 0);
    if (!filter) {
        uint32_t c = t->px[wrap((int)floorf(y), t->h, cv, fv) * t->w + wrap((int)floorf(x), t->w, cu, fu)];
        out[0] = ((c >> 16) & 255) / 255.f; out[1] = ((c >> 8) & 255) / 255.f; out[2] = (c & 255) / 255.f; out[3] = (c >> 24) / 255.f;
        return;
    }
    int x0 = (int)floorf(x), y0 = (int)floorf(y); float ax = x - x0, ay = y - y0;
    int X0 = wrap(x0, t->w, cu, fu), X1 = wrap(x0 + 1, t->w, cu, fu), Y0 = wrap(y0, t->h, cv, fv), Y1 = wrap(y0 + 1, t->h, cv, fv);
    uint32_t c00 = t->px[Y0 * t->w + X0], c10 = t->px[Y0 * t->w + X1], c01 = t->px[Y1 * t->w + X0], c11 = t->px[Y1 * t->w + X1];
    for (int k = 0; k < 4; k++) {
        int sh = k == 3 ? 24 : 16 - 8 * k;
        float a = ((c00 >> sh) & 255) * (1 - ax) + ((c10 >> sh) & 255) * ax;
        float b = ((c01 >> sh) & 255) * (1 - ax) + ((c11 >> sh) & 255) * ax;
        out[k] = (a * (1 - ay) + b * ay) / 255.f;
    }
}

/* ------------------------------------------------------------------ rasteriser */
static int RW, RH;                    /* render target size */
static float *cbuf;                   /* RGBA */
static float *zbuf;
static int cap_px;

static int depth_pass(int mode, float z, float old) {
    switch (mode) {
    case 0: return 0;
    case 1: return z < old;
    case 2: return z == old;
    case 3: return z <= old;
    case 4: return z > old;
    case 5: return z != old;
    case 6: return z >= old;
    default: return 1;
    }
}
static void blend_factor(int ins, const float *src, const float *dst, int is_src, float *f) {
    const float *other = is_src ? dst : src;
    switch (ins) {
    case 0: f[0] = f[1] = f[2] = f[3] = 0; break;
    case 1: f[0] = f[1] = f[2] = f[3] = 1; break;
    case 2: for (int i = 0; i < 4; i++) f[i] = other[i]; break;
    case 3: for (int i = 0; i < 4; i++) f[i] = 1 - other[i]; break;
    case 4: f[0] = f[1] = f[2] = f[3] = src[3]; break;
    case 5: f[0] = f[1] = f[2] = f[3] = 1 - src[3]; break;
    case 6: f[0] = f[1] = f[2] = f[3] = dst[3]; break;
    default: f[0] = f[1] = f[2] = f[3] = 1 - dst[3]; break;
    }
}

static int hscale;                     /* SCALER_CTL horizontal: scene is twice as wide */

/* fog table: FOG_DENSITY scales 1/w; the exponent and top mantissa bits of the result pick one of
 * 128 entries, the next mantissa bits blend its two halves (as the PVR does) */
static float fog_table(float invw) {
    uint32_t den = REG(0x0B4);
    float mant = ((den >> 8) & 255) / 128.f;
    int ex = (int8_t)(den & 255);
    float fw = mant * ldexpf(1.f, ex) * invw;
    if (fw < 1) fw = 1; if (fw > 255.999985f) fw = 255.999985f;
    uint32_t b; memcpy(&b, &fw, 4);
    uint32_t e = (b >> 23) & 255, m = b & 0x7FFFFF;
    uint32_t idx = (((e + 1) & 7) << 4) | ((m >> 19) & 15);
    uint32_t bl = (m >> 11) & 255, inv = 255 ^ bl;
    uint32_t ent = REG(0x200 + idx * 4);
    uint32_t a = ((ent & 255) * bl + ((ent >> 8) & 255) * inv) >> 8;
    return a / 255.f;
}
static void unpack_rgb(uint32_t v, float *c) { c[0] = ((v >> 16) & 255) / 255.f; c[1] = ((v >> 8) & 255) / 255.f; c[2] = (v & 255) / 255.f; }

/* colour of one pixel of polygon p (parameter set `set`) at perspective weights pa, pb, pc */
static void shade(const poly *p, int set, const vtx *a, const vtx *b, const vtx *c, float pa, float pb, float pc, float z, float *out) {
    uint32_t tsp = set ? p->tsp1 : p->tsp, tcw = set ? p->tcw1 : p->tcw;
    int tex = (p->isp >> 25) & 1, ofs = (p->isp >> 24) & 1, gouraud = (p->isp >> 23) & 1;
    int use_alpha = (tsp >> 20) & 1, ign_ta = (tsp >> 19) & 1, shade_i = (tsp >> 6) & 3, fogm = (tsp >> 22) & 3;
    const float *ca = set ? a->c1 : a->c, *cb = set ? b->c1 : b->c, *cc = set ? c->c1 : c->c;
    const float *oa = set ? a->o1 : a->o, *ob = set ? b->o1 : b->o, *oc = set ? c->o1 : c->o;
    float col[4], off[4];
    for (int k = 0; k < 4; k++) {
        col[k] = gouraud ? pa * ca[k] + pb * cb[k] + pc * cc[k] : cc[k];
        off[k] = gouraud ? pa * oa[k] + pb * ob[k] + pc * oc[k] : oc[k];
    }
    if (!use_alpha) col[3] = 1;
    if (tex) {
        tex_t *t = get_tex(tcw, tsp);
        float tc[4];
        float u = set ? pa * a->u1 + pb * b->u1 + pc * c->u1 : pa * a->u + pb * b->u + pc * c->u;
        float v = set ? pa * a->v1 + pb * b->v1 + pc * c->v1 : pa * a->v + pb * b->v + pc * c->v;
        sample(t, tsp, u, v, tc);
        if (ign_ta) tc[3] = 1;
        switch (shade_i) {
        case 0: for (int k = 0; k < 3; k++) out[k] = tc[k]; out[3] = tc[3]; break;
        case 1: for (int k = 0; k < 3; k++) out[k] = tc[k] * col[k]; out[3] = tc[3]; break;
        case 2: for (int k = 0; k < 3; k++) out[k] = tc[k] * tc[3] + col[k] * (1 - tc[3]); out[3] = col[3]; break;
        default: for (int k = 0; k < 3; k++) out[k] = tc[k] * col[k]; out[3] = tc[3] * col[3]; break;
        }
        if (dc_pvr_sw_highlight && highlighted(tcw, tsp)) { out[0] = 1; out[1] = 0; out[2] = 1; }
    } else memcpy(out, col, 4 * sizeof *out);
    if (ofs) for (int k = 0; k < 3; k++) { out[k] += off[k]; if (out[k] > 1) out[k] = 1; }
    if (fogm != 2) {                                  /* 0 table, 1 vertex (offset alpha), 3 table mode 2 */
        float fc[3], f;
        if (fogm == 1) { unpack_rgb(REG(0x0AC), fc); f = off[3]; }
        else { unpack_rgb(REG(0x0B0), fc); f = fog_table(z); }
        if (fogm == 3) { for (int k = 0; k < 3; k++) out[k] = fc[k]; out[3] = f; }
        else for (int k = 0; k < 3; k++) out[k] = out[k] * (1 - f) + fc[k] * f;
    }
}

/* per-pixel record of what produced each opaque / punch-through pixel, for modifier volumes */
typedef struct { int pid, va, vb, vc; float pa, pb; } dpix;
static dpix *dbuf;
static int cur_pid;

static void draw_tri(const poly *p, const vtx *a, const vtx *b, const vtx *c, int flip) {
    float ax = a->x, ay = a->y, bx = b->x, by = b->y, cx = c->x, cy = c->y;
    float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (area == 0) return;
    int cull = (p->isp >> 27) & 3;
    float sarea = flip ? -area : area;
    if (cull == 1 && fabsf(area) < f32(REG(0x078))) return;
    if (cull == 2 && sarea < 0) return;
    if (cull == 3 && sarea > 0) return;
    int list = p->list;
    int zmode = (p->isp >> 29) & 7, zwrite = !((p->isp >> 26) & 1);
    if (list == 2) zwrite = 0;
    if (list == 4 && zmode == 0) zmode = 6;
    uint32_t tsp = p->tsp;
    int src_i = (tsp >> 29) & 7, dst_i = (tsp >> 26) & 7;
    int x0 = (int)floorf(fminf(ax, fminf(bx, cx))), x1 = (int)ceilf(fmaxf(ax, fmaxf(bx, cx)));
    int y0 = (int)floorf(fminf(ay, fminf(by, cy))), y1 = (int)ceilf(fmaxf(ay, fmaxf(by, cy)));
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > RW) x1 = RW; if (y1 > RH) y1 = RH;
    float inv = 1.f / area;
    float pt_ref = (REG(0x11C) & 255) / 255.f;
    int ia = (int)(a - vt), ib = (int)(b - vt), ic = (int)(c - vt);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float w0 = ((bx - px) * (cy - py) - (by - py) * (cx - px)) * inv;
            float w1 = ((cx - px) * (ay - py) - (cy - py) * (ax - px)) * inv;
            float w2 = 1 - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            float z = w0 * a->z + w1 * b->z + w2 * c->z;
            int i = y * RW + x;
            if (!depth_pass(zmode, z, zbuf[i])) continue;
            /* perspective-correct weights (z = 1/w) */
            float pa = w0 * a->z, pb = w1 * b->z, pc = w2 * c->z, ps = pa + pb + pc;
            if (ps != 0) { pa /= ps; pb /= ps; pc /= ps; } else { pa = w0; pb = w1; pc = w2; }
            float px_[4];
            shade(p, 0, a, b, c, pa, pb, pc, z, px_);
            if (list == 4 && px_[3] < pt_ref) continue;
            float *d = cbuf + i * 4;
            if (list == 2) {
                float fs[4], fd[4];
                blend_factor(src_i, px_, d, 1, fs); blend_factor(dst_i, px_, d, 0, fd);
                for (int k = 0; k < 4; k++) { float v = px_[k] * fs[k] + d[k] * fd[k]; d[k] = v > 1 ? 1 : v; }
            } else {
                memcpy(d, px_, sizeof px_);
                dpix *dp = &dbuf[i]; dp->pid = cur_pid; dp->va = ia; dp->vb = ib; dp->vc = ic; dp->pa = pa; dp->pb = pb;
            }
            if (zwrite) zbuf[i] = z;
        }
}

/* modifier volumes (opaque list): even/odd count of volume triangles in front of the surface at
 * each pixel; a volume ends with a triangle whose instruction is 1 (inside last) or 2 (outside).
 * Affected pixels get the shadow factor (cheap shadow mode) or their polygon's second set. */
static uint8_t *mvbuf, *mvacc;
static void mod_tri(const modtri *m) {
    float ax = m->x[0], ay = m->y[0], bx = m->x[1], by = m->y[1], cx = m->x[2], cy = m->y[2];
    float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (area == 0) return;
    int x0 = (int)floorf(fminf(ax, fminf(bx, cx))), x1 = (int)ceilf(fmaxf(ax, fmaxf(bx, cx)));
    int y0 = (int)floorf(fminf(ay, fminf(by, cy))), y1 = (int)ceilf(fmaxf(ay, fmaxf(by, cy)));
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > RW) x1 = RW; if (y1 > RH) y1 = RH;
    float inv = 1.f / area;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float w0 = ((bx - px) * (cy - py) - (by - py) * (cx - px)) * inv;
            float w1 = ((cx - px) * (ay - py) - (cy - py) * (ax - px)) * inv;
            float w2 = 1 - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            float z = w0 * m->z[0] + w1 * m->z[1] + w2 * m->z[2];
            int i = y * RW + x;
            if (z >= zbuf[i]) mvbuf[i] ^= 1;          /* volume surface between the eye and the pixel */
        }
}
static void modifier_volumes(void) {
    int n = RW * RH, any = 0;
    memset(mvbuf, 0, (size_t)n); memset(mvacc, 0, (size_t)n);
    for (int k = 0; k < nmt; k++) {
        const modtri *m = &mt[k];
        if (m->list != 1) continue;
        mod_tri(m);
        if (m->inst == 1 || m->inst == 2) {           /* last triangle of a volume */
            for (int i = 0; i < n; i++) { if ((mvbuf[i] & 1) == (m->inst == 1)) mvacc[i] = 1; }
            memset(mvbuf, 0, (size_t)n); any = 1;
        }
    }
    if (!any) return;
    uint32_t shad = REG(0x074);
    int cheap = (shad >> 8) & 1;
    float factor = (shad & 255) / 256.f;
    for (int i = 0; i < n; i++) {
        if (!mvacc[i] || dbuf[i].pid < 0) continue;
        const poly *p = &pl[dbuf[i].pid];
        float *d = cbuf + i * 4;
        if (cheap) { if (p->shadow) for (int k = 0; k < 3; k++) d[k] *= factor; }
        else if (p->twovol) {
            const dpix *dp = &dbuf[i];
            float pc = 1 - dp->pa - dp->pb, out[4];
            const vtx *a = &vt[dp->va], *b = &vt[dp->vb], *c = &vt[dp->vc];
            shade(p, 1, a, b, c, dp->pa, dp->pb, pc, zbuf[i], out);
            memcpy(d, out, sizeof out);
        }
    }
}

static void draw_poly(const poly *p) {
    const vtx *v = vt + p->first;
    if (p->sprite) { draw_tri(p, &v[0], &v[1], &v[2], 0); draw_tri(p, &v[0], &v[2], &v[3], 0); return; }
    for (int i = 0; i + 2 < p->n; i++) draw_tri(p, &v[i], &v[i + 1], &v[i + 2], i & 1);
}

typedef struct { int idx; float key; } sort_e;
static int cmp_sort(const void *a, const void *b) {
    float x = ((const sort_e *)a)->key, y = ((const sort_e *)b)->key;
    return x < y ? -1 : x > y;
}

/* background plane: ISP_BACKGND_T points at a 3-vertex polygon in parameter memory */
static void background(float *col, float *depth) {
    uint32_t t = REG(0x08C), base = (REG(0x020) & 0xF00000) + ((t >> 3) & 0x1FFFFF) * 4;
    uint32_t skip = (t >> 24) & 7, isp = rd32_area32(base);
    int texd = (isp >> 25) & 1;
    uint32_t vbase = base + 12;                       /* after ISP, TSP, TCW */
    uint32_t cw = rd32_area32(vbase + (3 + (texd ? 2 : 0)) * 4);
    (void)skip;
    unpack(cw, col); col[3] = 1;
    *depth = f32(REG(0x088));
}

int dc_pvr_sw_render(void) {
    ctx_t *saved = ta_ctx, *rc = NULL;
    uint32_t pbase = REG(0x020) & 0xFFFFFF;
    for (int i = 0; i < NCTX; i++) if (ctxs[i].used && ctxs[i].base == pbase) rc = &ctxs[i];
    if (!rc) rc = saved;                         /* no match: the list being built */
    ta_ctx = rc;
    /* render target: FB_X_CLIP / FB_Y_CLIP maxima, doubled horizontally when the scaler halves it */
    uint32_t xc = REG(0x068), yc = REG(0x06C), sc = REG(0x0F4);
    hscale = (sc >> 16) & 1;
    int W = (int)((xc >> 16) & 0x7FF) + 1, H = (int)((yc >> 16) & 0x3FF) + 1;
    if (W < 16 || W > 1280) W = 640;
    if (H < 16 || H > 1024) H = 480;
    RW = W * (hscale ? 2 : 1); RH = H;
    if (RW * RH > cap_px) {
        cap_px = RW * RH; cbuf = realloc(cbuf, (size_t)cap_px * 16); zbuf = realloc(zbuf, (size_t)cap_px * 4);
        dbuf = realloc(dbuf, (size_t)cap_px * sizeof *dbuf); mvbuf = realloc(mvbuf, (size_t)cap_px); mvacc = realloc(mvacc, (size_t)cap_px);
    }
    float bg[4], bz;
    background(bg, &bz);
    if (dc_pvr_sw_debug) {
        fprintf(stderr, "pvr: %dx%d bg %.2f %.2f %.2f z %g, %d polys\n", RW, RH, bg[0], bg[1], bg[2], bz, npl);
        for (int i = 0; i < npl; i++) { const vtx *v = vt + pl[i].first;
            uint32_t ta = (pl[i].tcw & 0x1FFFFF) << 3;
            fprintf(stderr, " poly list %d n %d sprite %d isp %08X tsp %08X tcw %08X v0 %.1f,%.1f,%.2f uv %.3f,%.3f c %.2f %.2f %.2f %.2f tex@%06X: %02X%02X %02X%02X %02X%02X\n", pl[i].list, pl[i].n, pl[i].sprite,
                    pl[i].isp, pl[i].tsp, pl[i].tcw, v->x, v->y, v->z, v->u, v->v, v->c[0], v->c[1], v->c[2], v->c[3], ta,
                    dc_vram[ta + 1], dc_vram[ta], dc_vram[ta + 3], dc_vram[ta + 2], dc_vram[ta + 0x401], dc_vram[ta + 0x400]); }
        dc_pvr_sw_debug = 0;
    }
    for (int i = 0; i < RW * RH; i++) { memcpy(cbuf + i * 4, bg, 16); zbuf[i] = bz; dbuf[i].pid = -1; }
    for (int i = 0; i < npl; i++) if (pl[i].list == 0) { cur_pid = i; draw_poly(&pl[i]); }
    for (int i = 0; i < npl; i++) if (pl[i].list == 4) { cur_pid = i; draw_poly(&pl[i]); }
    modifier_volumes();
    /* translucent: ISP_FEED_CFG bit 0 = presort (submission order), else sort back to front */
    int presort = REG(0x098) & 1, nt = 0;
    sort_e *s = malloc(sizeof *s * (npl ? npl : 1));
    for (int i = 0; i < npl; i++) if (pl[i].list == 2) {
        float zmin = 1e30f;
        for (int k = 0; k < pl[i].n; k++) if (vt[pl[i].first + k].z < zmin) zmin = vt[pl[i].first + k].z;
        s[nt].idx = i; s[nt].key = presort ? (float)nt : zmin; nt++;
    }
    if (!presort) qsort(s, nt, sizeof *s, cmp_sort);
    for (int i = 0; i < nt; i++) draw_poly(&pl[s[i].idx]);
    free(s);
    /* write the framebuffer: FB_W_SOF1, FB_W_LINESTRIDE (64-bit units), FB_W_CTRL packing */
    uint32_t sof = REG(0x060) & 0x7FFFFF, stride = (REG(0x04C) & 0x1FF) * 8, ctrl = REG(0x048), mode = ctrl & 7;
    uint32_t athr = (ctrl >> 8) & 255;
    int bpp = mode <= 3 ? 2 : mode == 4 ? 3 : 4;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            float c[4];
            if (hscale) { const float *p0 = cbuf + (y * RW + 2 * x) * 4, *p1 = p0 + 4; for (int k = 0; k < 4; k++) c[k] = (p0[k] + p1[k]) * 0.5f; }
            else memcpy(c, cbuf + (y * RW + x) * 4, 16);
            uint32_t r = (uint32_t)(c[0] * 255 + .5f), g = (uint32_t)(c[1] * 255 + .5f), b = (uint32_t)(c[2] * 255 + .5f), a = (uint32_t)(c[3] * 255 + .5f);
            uint32_t v;
            switch (mode) {
            case 0: v = (r >> 3) << 10 | (g >> 3) << 5 | b >> 3 | 0x8000; break;
            case 1: v = (r >> 3) << 11 | (g >> 2) << 5 | b >> 3; break;
            case 2: v = (a >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | b >> 4; break;
            case 3: v = (a >= athr ? 0x8000u : 0) | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3; break;
            case 4: v = r << 16 | g << 8 | b; break;
            case 5: v = r << 16 | g << 8 | b; break;
            default: v = a << 24 | r << 16 | g << 8 | b; break;
            }
            uint32_t o = sof + y * stride + x * bpp;
            for (int k = 0; k < bpp; k++) wr8_area32(o + k, (uint8_t)(v >> (8 * k)));
        }
    int n = npl;
    ta_ctx = saved;
    return n;
}

void dc_pvr_sw_install(void) { dc_ta_set_sink(sink); }

/* ------------------------------------------------------------------ video output */
int dc_pvr_display(uint8_t *rgb, int maxw, int maxh, int *w, int *h) {
    uint32_t ctrl = REG(0x044), size = REG(0x05C), s1 = REG(0x050) & 0x7FFFFF, s2 = REG(0x054) & 0x7FFFFF;
    int depth = (ctrl >> 2) & 3, bpp = depth == 0 || depth == 1 ? 2 : depth == 2 ? 3 : 4;
    int words = (int)(size & 0x3FF) + 1, lines = (int)((size >> 10) & 0x3FF) + 1, mod = (int)((size >> 20) & 0x3FF);
    int W = words * 4 / bpp, interlace = (REG(0x0D0) >> 4) & 1;
    int H = interlace ? lines * 2 : lines;
    uint32_t pitch = (uint32_t)(words + mod - 1) * 4;
    if (W > maxw) W = maxw;
    if (H > maxh) H = maxh;
    for (int y = 0; y < H; y++) {
        uint32_t base = interlace ? ((y & 1) ? s2 : s1) + (uint32_t)(y >> 1) * pitch : s1 + (uint32_t)y * pitch;
        for (int x = 0; x < W; x++) {
            uint32_t o = base + (uint32_t)x * bpp, v = 0;
            for (int k = 0; k < bpp; k++) v |= (uint32_t)dc_vram[vram32_ofs(o + k)] << (8 * k);
            uint8_t *d = rgb + (y * W + x) * 3;
            if (depth == 0) { d[0] = ((v >> 10) & 31) << 3; d[1] = ((v >> 5) & 31) << 3; d[2] = (v & 31) << 3; }
            else if (depth == 1) { d[0] = ((v >> 11) & 31) << 3; d[1] = ((v >> 5) & 63) << 2; d[2] = (v & 31) << 3; }
            else { d[0] = (v >> 16) & 255; d[1] = (v >> 8) & 255; d[2] = v & 255; }
        }
    }
    *w = W; *h = H;
    return (ctrl & 1) ? 0 : -1;
}

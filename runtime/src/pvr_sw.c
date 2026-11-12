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
#include "pvr_int.h"
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
    if ((o & 0xFFF) == 0 || (o & 3) == 0) { dc_vram_gen[so >> 12]++; dc_vram32_gen[(o & 0x7FFFFF) >> 12]++; }   /* framebuffer writes invalidate textures there */
    dc_vram[so] = v;
}
static uint16_t tex16(uint32_t o) { o &= dc_vram_tex_mask & ~1u; return (uint16_t)(dc_vram[o] | dc_vram[o + 1] << 8); }
static uint8_t tex8(uint32_t o) { return dc_vram[o & dc_vram_tex_mask]; }

/* ------------------------------------------------------------------ display list */
typedef pvr_vtx vtx;
typedef pvr_poly poly;
typedef pvr_modtri modtri;

/* The TA fills one context while the core renders another: contexts are keyed by TA_ISP_BASE at
 * list init and STARTRENDER draws the one whose base equals PARAM_BASE. */
/* What a header leaves for the vertices after it (the TA's state while a list comes in). */
typedef struct {
    uint32_t pcw, isp, tsp, tcw, tsp1, tcw1;
    float face[4], face_ofs[4], face1[4];
    int mod_inst, mod_cull;                        /* modifier volume instruction and culling mode of the last header */
    int open;                                      /* index of the strip being filled, -1 none */
    int sprite;                                    /* the last header was a sprite header */
    int ux0, uy0, ux1, uy1;                        /* last user tile clip rectangle (scene pixels) */
} pstate;
/* The TA's input is kept as it comes (raw: records of a word -- kind, list + 1 << 8, words << 16 -- and
 * the parameter's 8 or 16 words) and only turned into the lists a renderer draws (vt, pl, mt) when one
 * asks for them (ctx_parse). The game's thread then pays a copy of 32 bytes for a vertex, where the
 * colours' conversion and the rest of the parsing was a tenth of its time in a race, and a renderer on
 * its own thread parses its copy there (pvr_snap_take / pvr_snap_lists). */
typedef struct {
    uint32_t base; int used; vtx *vt; int nvt, cap_vt; poly *pl; int npl, cap_pl; modtri *mt; int nmt, cap_mt; unsigned age;
    uint32_t *raw; int nraw, cap_raw, parsed;      /* in words; parsed: how far vt, pl and mt have it */
    pstate ps, ps0;                                /* the parser's state at parsed, and at the start of raw */
    uint32_t *tex; int ntex, cap_tex;              /* tcw, tsp of the textured polygons, as they came (pairs) */
} ctx_t;
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
/* the collecting side's own view of the last header (in the order the TA is sent things, across
 * contexts): whether its polygons are textured and with what, and the face colours, which an
 * "intensity" header may take over from the one before it -- also from the list before */
static struct { uint32_t pcw, tcw, tsp, tcw1, tsp1; int tex, two, noted; float face[4], face_ofs[4], face1[4]; } col;
static void list_init(void) {
    uint32_t base = REG(0x128) & 0xFFFFFF;
    ctx_t *c = NULL, *old = &ctxs[0];
    for (int i = 0; i < NCTX; i++) { if (ctxs[i].used && ctxs[i].base == base) c = &ctxs[i]; if (ctxs[i].age < old->age) old = &ctxs[i]; }
    if (!c) c = old;
    c->base = base; c->used = 1; c->age = ++ctx_clock;
    ta_ctx = c;
    npl = 0; nvt = 0; nmt = 0;
    c->nraw = c->parsed = 0; c->ntex = 0;
    memset(&c->ps, 0, sizeof c->ps);
    c->ps.open = -1; c->ps.ux1 = c->ps.uy1 = 4096;
    memcpy(c->ps.face, col.face, sizeof col.face); memcpy(c->ps.face_ofs, col.face_ofs, sizeof col.face_ofs); memcpy(c->ps.face1, col.face1, sizeof col.face1);
    c->ps0 = c->ps;
    col.pcw = 0; col.noted = 1;
}

static float f32(uint32_t w) { float f; memcpy(&f, &w, 4); return f; }
/* a packed colour's channels as floats: n / 255 from a table (the same values; four divisions for each
 * colour of each vertex were a tenth of what a vertex cost here) */
static float u8f[256];
static void u8f_init(void) { for (int i = 0; i < 256; i++) u8f[i] = (float)i / 255.f; }
static void unpack(uint32_t argb, float *c) {
    c[0] = u8f[(argb >> 16) & 255]; c[1] = u8f[(argb >> 8) & 255];
    c[2] = u8f[argb & 255]; c[3] = u8f[argb >> 24];
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

/* ---- the parser: one context's records into its lists. It touches nothing but the context it is
 * given, so the copy a renderer on another thread took can be parsed there. */
#undef vt
#undef nvt
#undef cap_vt
#undef pl
#undef npl
#undef cap_pl
#undef mt
#undef nmt
#undef cap_mt
#define vt (pc->vt)
#define nvt (pc->nvt)
#define cap_vt (pc->cap_vt)
#define pl (pc->pl)
#define npl (pc->npl)
#define cap_pl (pc->cap_pl)
#define mt (pc->mt)
#define nmt (pc->nmt)
#define cap_mt (pc->cap_mt)
#define cur (pc->ps)
static vtx *new_vtx(ctx_t *pc) {
    if (nvt == cap_vt) { cap_vt = cap_vt ? cap_vt * 2 : 4096; vt = realloc(vt, cap_vt * sizeof *vt); }
    memset(&vt[nvt], 0, sizeof *vt);
    return &vt[nvt++];
}
static poly *new_poly(ctx_t *pc, int list, int sprite) {
    if (npl == cap_pl) { cap_pl = cap_pl ? cap_pl * 2 : 1024; pl = realloc(pl, cap_pl * sizeof *pl); }
    poly *p = &pl[npl++];
    p->isp = cur.isp; p->tsp = cur.tsp; p->tcw = cur.tcw; p->tsp1 = cur.tsp1; p->tcw1 = cur.tcw1;
    p->list = list; p->first = nvt; p->n = 0; p->sprite = sprite;
    p->twovol = !sprite && ((cur.pcw >> 6) & 1); p->shadow = (cur.pcw >> 7) & 1;
    p->uclip = (int)((cur.pcw >> 16) & 3); if (p->uclip == 1) p->uclip = 0;
    p->ux0 = cur.ux0; p->uy0 = cur.uy0; p->ux1 = cur.ux1; p->uy1 = cur.uy1;
    return p;
}

static void parse_rec(ctx_t *pc, int kind, int list, const uint32_t *w, int words) {
    switch (kind) {
    case 1:                                           /* user tile clip: tile (32x32) min/max, inclusive */
        cur.ux0 = (int)(w[4] & 63) * 32; cur.uy0 = (int)(w[5] & 31) * 32;
        cur.ux1 = ((int)(w[6] & 63) + 1) * 32; cur.uy1 = ((int)(w[7] & 31) + 1) * 32;
        return;
    case 4:                                           /* polygon header (or modifier volume) */
        cur.open = -1;
        cur.sprite = 0;
        if (list == 1 || list == 3) { cur.pcw = 0; cur.mod_inst = (int)(w[1] >> 29); cur.mod_cull = (int)(w[1] >> 27) & 3; return; }
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
            m->inst = cur.mod_inst; m->list = list; m->cull = cur.mod_cull;
            return;
        }
        if (!cur.pcw) return;
        if (cur.sprite) {                             /* sprite: one quad per parameter */
            poly *p = new_poly(pc, list, 1);
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
                vtx *t = new_vtx(pc); t->x = x[i]; t->y = y[i]; t->z = z[i]; t->u = u[i]; t->v = v[i];
                memcpy(t->c, cur.face, sizeof t->c); memcpy(t->o, cur.face_ofs, sizeof t->o);
            }
            p->n = 4;
            return;
        }
        if (cur.open < 0) { new_poly(pc, list, 0); cur.open = npl - 1; }
        vtx *t = new_vtx(pc);
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
static void ctx_lists(ctx_t *pc, pvr_frame *f) { f->verts = vt; f->nverts = nvt; f->polys = pl; f->npolys = npl; f->mods = mt; f->nmods = nmt; }
static void ctx_restart(ctx_t *pc) { nvt = 0; npl = 0; nmt = 0; }
#undef cur
#undef vt
#undef nvt
#undef cap_vt
#undef pl
#undef npl
#undef cap_pl
#undef mt
#undef nmt
#undef cap_mt
#define vt (ta_ctx->vt)
#define nvt (ta_ctx->nvt)
#define cap_vt (ta_ctx->cap_vt)
#define pl (ta_ctx->pl)
#define npl (ta_ctx->npl)
#define cap_pl (ta_ctx->cap_pl)
#define mt (ta_ctx->mt)
#define nmt (ta_ctx->nmt)
#define cap_mt (ta_ctx->cap_mt)
static void ctx_parse(ctx_t *pc) {
    while (pc->parsed < pc->nraw) {
        const uint32_t *r = pc->raw + pc->parsed;
        int words = (int)(r[0] >> 16);
        parse_rec(pc, (int)(r[0] & 0xFF), (int)((r[0] >> 8) & 0xFF) - 1, r + 1, words);
        pc->parsed += 1 + words;
    }
}

/* ---- the TA's sink: on the game's thread, as the game sends its lists */
static void tex_note(ctx_t *c, uint32_t tcw, uint32_t tsp) {
    if (c->ntex && c->tex[c->ntex * 2 - 2] == tcw && c->tex[c->ntex * 2 - 1] == tsp) return;   /* as the one before */
    if (c->ntex == c->cap_tex) { c->cap_tex = c->cap_tex ? c->cap_tex * 2 : 256; c->tex = realloc(c->tex, (size_t)c->cap_tex * 8); }
    c->tex[c->ntex * 2] = tcw; c->tex[c->ntex * 2 + 1] = tsp; c->ntex++;
}
static void sink(int kind, int list, const uint32_t *w, int words) {
    if (kind == -1) { list_init(); return; }           /* TA_LIST_INIT */
    if (kind != 7 && kind != 4 && kind != 5 && kind != 1) return;
    ctx_t *c = ta_ctx;
    if (c->nraw + 17 > c->cap_raw) { c->cap_raw = c->cap_raw ? c->cap_raw * 2 : 1 << 16; c->raw = realloc(c->raw, (size_t)c->cap_raw * 4); }
    uint32_t *r = c->raw + c->nraw;
    r[0] = (uint32_t)kind | ((uint32_t)(list + 1) & 0xFF) << 8 | (uint32_t)words << 16;
    if (words == 8) memcpy(r + 1, w, 32); else memcpy(r + 1, w, (size_t)words * 4);
    c->nraw += 1 + words;
    if (kind == 7) {                                   /* the first vertex after a header: its polygons exist */
        if (!col.noted && col.pcw && list != 1 && list != 3) {
            col.noted = 1;
            if (col.tex) { tex_note(c, col.tcw, col.tsp); if (col.two) tex_note(c, col.tcw1, col.tsp1); }
        }
        return;
    }
    if (kind == 4) {                                   /* as parse_rec reads a header, for what col keeps */
        if (list == 1 || list == 3) { col.pcw = 0; return; }
        int colt = (w[0] >> 4) & 3, vol = (w[0] >> 6) & 1, ofs = (w[0] >> 2) & 1;
        col.pcw = w[0]; col.tex = (int)((isp_from_pcw(w[1], w[0]) >> 25) & 1); col.tsp = w[2]; col.tcw = w[3];
        col.two = vol; if (vol) { col.tsp1 = w[4]; col.tcw1 = w[5]; }
        if (colt == 2) {
            if (words == 16 && vol) { float_col(w + 8, col.face); float_col(w + 12, col.face1); memset(col.face_ofs, 0, sizeof col.face_ofs); }
            else if (words == 16 && ofs) { float_col(w + 8, col.face); float_col(w + 12, col.face_ofs); }
            else float_col(w + 4, col.face);
            if (!vol) memcpy(col.face1, col.face, sizeof col.face1);
        }
        col.noted = 0;
    } else if (kind == 5) {
        col.pcw = w[0]; col.tex = (int)((isp_from_pcw(w[1], w[0]) >> 25) & 1); col.tsp = w[2]; col.tcw = w[3]; col.two = 0;
        unpack(w[4], col.face); unpack(w[5], col.face_ofs);
        col.noted = 0;
    }
}

/* ------------------------------------------------------------------ textures */
typedef struct { uint32_t tcw, tsp_size, pal_gen, pal_hash, gen, start, end; int w, h; uint32_t *px; uint64_t id; } tex_t;
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
/* the palette entries a paletted texture uses (16 for 4 bpp, 256 for 8 bpp) and the palette format,
 * hashed: palette RAM writes bump one global generation, and games that rewrite palettes every frame
 * (Soulcalibur) would otherwise make every paletted texture decode and upload again each frame */
static uint32_t pal_hash(uint32_t tcw) {
    int fmt = (tcw >> 27) & 7;
    uint32_t base = fmt == 5 ? ((tcw >> 21) & 63) << 4 : ((tcw >> 25) & 3) << 8, n = fmt == 5 ? 16 : 256;
    uint32_t h = 2166136261u ^ (REG(0x108) & 3);
    for (uint32_t i = 0; i < n; i++) { h ^= REG(0x1000 + ((base + i) & 1023) * 4); h *= 16777619u; }
    return h;
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
    for (uint32_t p = start >> 12; p <= ((end - 1) >> 12) && p <= (dc_vram_tex_mask >> 12); p++) g += dc_vram_gen[p] * (p + 1);
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
        if (page_gen(c->start, c->end) == c->gen) {
            if (!palfmt || c->pal_gen == dc_pal_gen) return c;
            if (pal_hash(tcw) == c->pal_hash) { c->pal_gen = dc_pal_gen; return c; }   /* its own entries unchanged */
        }
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
    int vq_linear = vq && linear && !mip;
    int pitch = w;                                           /* texels per row in memory */
    if (linear && stride) { pitch = (int)(REG(0x0E4) & 31) * 32; if (pitch <= 0) pitch = w; }
    if (mip) h = w;                                          /* mip-mapped textures are square */
    t->tcw = tcw; t->tsp_size = sz; t->pal_gen = dc_pal_gen; t->pal_hash = palfmt ? pal_hash(tcw) : 0; t->w = w; t->h = h; t->id = ++tex_ids;
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
        t->start = cb & dc_vram_tex_mask; t->end = ((addr & dc_vram_tex_mask) + bytes + (vq ? 0 : 0)) ;
        if (vq) t->end = (addr & dc_vram_tex_mask) + 2048 + (uint32_t)(w / 2) * (h / 2);
        if (t->end > dc_vram_tex_mask + 1) t->end = dc_vram_tex_mask + 1;
        if (t->end <= t->start) t->end = t->start + 1;
        t->gen = page_gen(t->start, t->end);
    }
    uint32_t pal_base = fmt == 5 ? ((tcw >> 21) & 63) << 4 : ((tcw >> 25) & 3) << 8;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t c;
            if (vq) {                                       /* 16-bit texels from a 256-entry codebook of 4 */
                /* twiddled: an index per 2x2 block; non-twiddled (scan order 1, which Rush Rush Rally
                 * Reloaded uses): row by row, each codebook entry 4 texels of one row */
                uint32_t idx, sub;
                if (vq_linear) { idx = tex8(addr + 2048 + (uint32_t)(y * (pitch >> 2) + (x >> 2))); sub = x & 3; }
                else { idx = tex8(addr + 2048 + twiddle(x >> 1, y >> 1, w >> 1, h >> 1)); sub = ((x & 1) << 1) | (y & 1); }
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
    uint32_t den = REG(0x0B8);                       /* FOG_DENSITY */
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
        if (fogm == 1) { unpack_rgb(REG(0x0B4), fc); f = off[3]; }
        else { unpack_rgb(REG(0x0B0), fc); f = fog_table(z); }
        if (fogm == 3) { for (int k = 0; k < 3; k++) out[k] = fc[k]; out[3] = f; }
        else for (int k = 0; k < 3; k++) out[k] = out[k] * (1 - f) + fc[k] * f;
    }
}

/* per-pixel record of what produced each opaque / punch-through pixel, for modifier volumes */
typedef struct { int pid, va, vb, vc; float pa, pb; } dpix;
static dpix *dbuf;
static int cur_pid;

/* Is the pixel centre (px, py) inside triangle abc? Barycentric weights out (inv = 1/area).
 * Top-left fill rule: a centre exactly on an edge belongs to the triangle only for top or left
 * edges, so shared edges are drawn once (as GPUs do). */
static int top_left(float dx, float dy, float area) {
    if (area < 0) { dx = -dx; dy = -dy; }
    return dy < 0 || (dy == 0 && dx > 0);
}
static int inside(float ax, float ay, float bx, float by, float cx, float cy, float px, float py, float inv,
                  float *w0, float *w1, float *w2) {
    float e0 = (bx - px) * (cy - py) - (by - py) * (cx - px);   /* edge b -> c */
    float e1 = (cx - px) * (ay - py) - (cy - py) * (ax - px);   /* edge c -> a */
    float e2 = (ax - px) * (by - py) - (ay - py) * (bx - px);   /* edge a -> b */
    float area = 1.f / inv;
    if (area < 0) { e0 = -e0; e1 = -e1; e2 = -e2; }
    if (e0 < 0 || e1 < 0 || e2 < 0) return 0;
    if (e0 == 0 && !top_left(cx - bx, cy - by, area)) return 0;
    if (e1 == 0 && !top_left(ax - cx, ay - cy, area)) return 0;
    if (e2 == 0 && !top_left(bx - ax, by - ay, area)) return 0;
    float ai = fabsf(inv);
    *w0 = e0 * ai; *w1 = e1 * ai; *w2 = e2 * ai;
    return 1;
}

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
    if (p->uclip == 2) {                                   /* user tile clip: only inside the rectangle */
        if (x0 < p->ux0) x0 = p->ux0; if (y0 < p->uy0) y0 = p->uy0;
        if (x1 > p->ux1) x1 = p->ux1; if (y1 > p->uy1) y1 = p->uy1;
    }
    float inv = 1.f / area;
    float pt_ref = (REG(0x11C) & 255) / 255.f;
    int ia = (int)(a - vt), ib = (int)(b - vt), ic = (int)(c - vt);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            if (p->uclip == 3 && x >= p->ux0 && x < p->ux1 && y >= p->uy0 && y < p->uy1) continue;
            float px = x + 0.5f, py = y + 0.5f, w0, w1, w2;
            if (!inside(ax, ay, bx, by, cx, cy, px, py, inv, &w0, &w1, &w2)) continue;
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
    /* culling as for polygons: a volume can be a flat, double-sided shape of which only one side counts
     * (Daytona USA 2001's car and scenery shadows) */
    if (m->cull == 1 && fabsf(area) < f32(REG(0x078))) return;
    if (m->cull == 2 && area < 0) return;
    if (m->cull == 3 && area > 0) return;
    int x0 = (int)floorf(fminf(ax, fminf(bx, cx))), x1 = (int)ceilf(fmaxf(ax, fmaxf(bx, cx)));
    int y0 = (int)floorf(fminf(ay, fminf(by, cy))), y1 = (int)ceilf(fmaxf(ay, fmaxf(by, cy)));
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > RW) x1 = RW; if (y1 > RH) y1 = RH;
    float inv = 1.f / area;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float px = x + 0.5f, py = y + 0.5f, w0, w1, w2;
            if (!inside(ax, ay, bx, by, cx, cy, px, py, inv, &w0, &w1, &w2)) continue;
            float z = w0 * m->z[0] + w1 * m->z[1] + w2 * m->z[2];
            int i = y * RW + x;
            if (z >= zbuf[i]) mvbuf[i] ^= 1;          /* volume surface between the eye and the pixel */
        }
}
static int mv_dbg;                              /* the frame dc_pvr_sw_debug describes: volume statistics */
static void modifier_volumes(void) {
    int n = RW * RH, any = 0, nvol = 0, insts[4] = {0};
    memset(mvbuf, 0, (size_t)n); memset(mvacc, 0, (size_t)n);
    for (int k = 0; k < nmt; k++) {
        const modtri *m = &mt[k];
        if (m->list != 1) continue;
        insts[m->inst & 3]++;
        mod_tri(m);
        if (m->inst == 1 || m->inst == 2) {           /* last triangle of a volume */
            for (int i = 0; i < n; i++) { if ((mvbuf[i] & 1) == (m->inst == 1)) mvacc[i] = 1; }
            memset(mvbuf, 0, (size_t)n); any = 1; nvol++;
        }
    }
    if (mv_dbg) {
        int in = 0, nopoly = 0, sh = 0, unsh = 0;
        for (int i = 0; i < n; i++) if (mvacc[i]) { in++; if (dbuf[i].pid < 0) nopoly++; else if (pl[dbuf[i].pid].shadow) sh++; else unsh++; }
        fprintf(stderr, "pvr: %d volumes (triangle instructions %d/%d/%d/%d), %d pixels inside: %d on shadow-flag polygons, %d on others, %d with no opaque polygon\n",
                nvol, insts[0], insts[1], insts[2], insts[3], in, sh, unsh, nopoly);
        int k0 = 0;
        for (int k = 0; k < nmt; k++) {
            const modtri *m = &mt[k];
            if (m->list != 1 || !(m->inst == 1 || m->inst == 2)) continue;
            float bx0 = 1e30f, bx1 = -1e30f, by0 = 1e30f, by1 = -1e30f, bz0 = 1e30f, bz1 = -1e30f;
            for (int j = k0; j <= k; j++) for (int q = 0; q < 3; q++) {
                const modtri *t = &mt[j]; if (t->list != 1) continue;
                if (t->x[q] < bx0) bx0 = t->x[q]; if (t->x[q] > bx1) bx1 = t->x[q]; if (t->y[q] < by0) by0 = t->y[q]; if (t->y[q] > by1) by1 = t->y[q];
                if (t->z[q] < bz0) bz0 = t->z[q]; if (t->z[q] > bz1) bz1 = t->z[q]; }
            int cx = (int)((bx0 + bx1) / 2), cy = (int)((by0 + by1) / 2);
            float zb = cx >= 0 && cx < RW && cy >= 0 && cy < RH ? zbuf[cy * RW + cx] : -1;
            fprintf(stderr, "pvr: volume %d tris x %.1f..%.1f y %.1f..%.1f z %g..%g, surface z %g at its centre\n", k - k0 + 1, bx0, bx1, by0, by1, bz0, bz1, zb);
            if (getenv("DC_MVDUMP") && bx1 > 200 && bx0 < 440 && by1 > 300)
                for (int gy = 0; gy < 4; gy++) for (int gx = 0; gx < 6; gx++) {
                    float px = bx0 + (bx1 - bx0) * (gx + 0.5f) / 6, py = by0 + (by1 - by0) * (gy + 0.5f) / 4;
                    int ix = (int)px, iy = (int)py; if (ix < 0 || iy < 0 || ix >= RW || iy >= RH) continue;
                    int i = iy * RW + ix, cross = 0; char zs[200]; int zl = 0; zs[0] = 0;
                    for (int j = k0; j <= k; j++) { const modtri *t = &mt[j]; float w0, w1, w2;
                        float area = (t->x[1] - t->x[0]) * (t->y[2] - t->y[0]) - (t->y[1] - t->y[0]) * (t->x[2] - t->x[0]); if (area == 0) continue;
                        if (!inside(t->x[0], t->y[0], t->x[1], t->y[1], t->x[2], t->y[2], ix + .5f, iy + .5f, 1.f / area, &w0, &w1, &w2)) continue;
                        float z = w0 * t->z[0] + w1 * t->z[1] + w2 * t->z[2]; cross += z >= zbuf[i];
                        if (zl < 180) zl += snprintf(zs + zl, sizeof zs - zl, " %.4f", z); }
                    int pid = dbuf[i].pid;
                    fprintf(stderr, "   px %d,%d surface z %.4f list %d shadow %d: faces%s -> %d in front\n", ix, iy, zbuf[i], pid >= 0 ? pl[pid].list : -1, pid >= 0 ? pl[pid].shadow : -1, zs, cross);
                }
            k0 = k + 1;
        }
        mv_dbg = 0;
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
    /* equal depth keeps submission order: qsort is not stable everywhere (MSVC's is not), and
     * text drawn at one depth then lost letters at random */
    const sort_e *p = a, *q = b;
    if (p->key != q->key) return p->key < q->key ? -1 : 1;
    return p->idx < q->idx ? -1 : p->idx > q->idx;
}

/* background plane: ISP_BACKGND_T points at a 3-vertex polygon in parameter memory */
static void background(float *col, float *depth) {
    uint32_t t = REG(0x08C), base = (REG(0x020) & 0xF00000) + ((t >> 3) & 0x1FFFFF) * 4;
    uint32_t skip = (t >> 24) & 7, isp = rd32_area32(base);
    int texd = (isp >> 25) & 1;
    uint32_t vbase = base + 12;                       /* after ISP, TSP, TCW */
    uint32_t cw = rd32_area32(vbase + (3 + (texd ? 2 : 0)) * 4);
    (void)skip;
    if (u8f[255] == 0) u8f_init();
    unpack(cw, col); col[3] = 1;
    *depth = f32(REG(0x088));
}

static ctx_t *render_ctx(void) {
    uint32_t pbase = REG(0x020) & 0xFFFFFF;
    ctx_t *rc = NULL;
    for (int i = 0; i < NCTX; i++) if (ctxs[i].used && ctxs[i].base == pbase) rc = &ctxs[i];
    return rc ? rc : ta_ctx;                     /* no match: the list being built */
}
static void frame_params(pvr_frame *f);
void pvr_get_frame(pvr_frame *f) {
    ctx_t *rc = render_ctx();
    ctx_parse(rc);
    ctx_lists(rc, f);
    frame_params(f);
}
/* for a renderer on another thread (pvr_int.h) */
static ctx_t snap;
void pvr_snap_take(pvr_frame *f, const uint32_t **tex, int *ntex) {
    ctx_t *rc = render_ctx();
    if (rc->nraw > snap.cap_raw) { snap.cap_raw = rc->nraw + rc->nraw / 2 + 1024; snap.raw = realloc(snap.raw, (size_t)snap.cap_raw * 4); }
    if (rc->nraw) memcpy(snap.raw, rc->raw, (size_t)rc->nraw * 4);
    snap.nraw = rc->nraw; snap.parsed = 0; ctx_restart(&snap); snap.ps = rc->ps0;
    *tex = rc->tex; *ntex = rc->ntex;
    f->verts = NULL; f->polys = NULL; f->mods = NULL; f->nverts = f->npolys = f->nmods = 0;
    frame_params(f);
}
void pvr_snap_lists(pvr_frame *f) {
    ctx_parse(&snap);
    ctx_lists(&snap, f);
}
static void frame_params(pvr_frame *f) {
    uint32_t xc = REG(0x068), yc = REG(0x06C);
    f->hscale = (REG(0x0F4) >> 16) & 1;
    f->clip_x0 = (int)(xc & 0x7FF); f->clip_x1 = (int)((xc >> 16) & 0x7FF);
    f->clip_y0 = (int)(yc & 0x3FF); f->clip_y1 = (int)((yc >> 16) & 0x3FF);
    if (f->clip_x1 < 15 || f->clip_x1 >= 1280) { f->clip_x0 = 0; f->clip_x1 = 639; }
    if (f->clip_y1 < 15 || f->clip_y1 >= 1024) { f->clip_y0 = 0; f->clip_y1 = 479; }
    if (f->clip_x0 > f->clip_x1) f->clip_x0 = 0;
    if (f->clip_y0 > f->clip_y1) f->clip_y0 = 0;
    /* the picture is the display (FB_R_SIZE), which can be larger than the clip: Rush 2049 shows
     * 640x470 and draws lines 4..443 only */
    uint32_t ctrl = REG(0x044), size = REG(0x05C);
    int depth = (ctrl >> 2) & 3, bpp = depth == 0 || depth == 1 ? 2 : depth == 2 ? 3 : 4;
    int dw = ((int)(size & 0x3FF) + 1) * 4 / bpp, dh = (int)((size >> 10) & 0x3FF) + 1;
    if ((REG(0x0D0) >> 4) & 1) dh *= 2;                       /* interlaced: both fields */
    if (dw < 256 || dw > 1280 || dh < 200 || dh > 1024) dw = dh = 0;
    f->width = dw > f->clip_x1 + 1 ? dw : f->clip_x1 + 1;
    f->height = dh > f->clip_y1 + 1 ? dh : f->clip_y1 + 1;
    background(f->bg, &f->bgz);
}
void pvr_get_texture(uint32_t tcw, uint32_t tsp, pvr_texinfo *ti) {
    tex_t *t = get_tex(tcw, tsp);
    ti->w = t->w; ti->h = t->h; ti->px = t->px; ti->id = t->id;
}
float pvr_fog_alpha(float invw) { return fog_table(invw); }

/* the area the region array (REGION_BASE) lists tiles for, in pixels: what the PVR actually renders.
 * Entries are 5 words, or 6 with FPU_PARAM_CFG bit 21; the control word holds the tile's x (bits 2-7),
 * y (8-13) and "last region" (31). 0 when there is no usable array. */
void pvr_region_extent(int *w, int *h) {
    uint32_t a = REG(0x02C) & 0x7FFFFC, words = (REG(0x07C) >> 21) & 1 ? 6 : 5;
    int mx = -1, my = -1;
    for (int i = 0; i < 4096; i++, a += words * 4) {
        uint32_t o = vram32_ofs(a), c = (uint32_t)dc_vram[o] | (uint32_t)dc_vram[o + 1] << 8 | (uint32_t)dc_vram[o + 2] << 16 | (uint32_t)dc_vram[o + 3] << 24;
        int tx = (c >> 2) & 63, ty = (c >> 8) & 63;
        if (tx > mx) mx = tx; if (ty > my) my = ty;
        if (c >> 31) break;
    }
    *w = (mx + 1) * 32; *h = (my + 1) * 32;
}

/* a pixel in the framebuffer write format (FB_W_CTRL mode, alpha threshold for 1555) */
static uint32_t fb_pack(uint32_t mode, uint32_t athr, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    switch (mode) {
    case 0: return (r >> 3) << 10 | (g >> 3) << 5 | b >> 3 | 0x8000;
    case 1: return (r >> 3) << 11 | (g >> 2) << 5 | b >> 3;
    case 2: return (a >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | b >> 4;
    case 3: return (a >= athr ? 0x8000u : 0) | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3;
    case 4: case 5: return r << 16 | g << 8 | b;
    default: return a << 24 | r << 16 | g << 8 | b;
    }
}

/* where a render writes: FB_W_SOF1, FB_W_LINESTRIDE (64-bit units), FB_W_CTRL packing; the size
 * written (WO x HO, from the clip of W x H) */
typedef struct { uint32_t sof, stride, mode, athr; int bpp, tex, WO, HO; } fb_target;
static fb_target fb_target_of(int W, int H) {
    fb_target t;
    t.sof = REG(0x060) & 0x7FFFFF; t.stride = (REG(0x04C) & 0x1FF) * 8;
    uint32_t ctrl = REG(0x048);
    t.mode = ctrl & 7; t.athr = (ctrl >> 8) & 255;
    t.bpp = t.mode <= 3 ? 2 : t.mode == 4 ? 3 : 4; t.tex = (REG(0x060) >> 24) & 1;
    t.WO = W; t.HO = H;                                     /* no wider than a line (a 1024 clip into a 640-pixel texture) */
    if (t.stride && t.WO * t.bpp > (int)t.stride) t.WO = (int)t.stride / t.bpp;
    if (t.tex) {                                            /* a texture: only the tiles the region array lists, so the */
        int rw, rh; pvr_region_extent(&rw, &rh);                /* lines past them (other textures) stay as they are */
        if (rw > 0 && rw < t.WO) t.WO = rw;
        if (rh > 0 && rh < t.HO) t.HO = rh;
    }
    return t;
}
static void fb_put(const fb_target *t, int x, int y, uint32_t v) {
    uint32_t o = t->sof + (uint32_t)y * t->stride + (uint32_t)x * (uint32_t)t->bpp;
    if (t->tex) { o &= 0x7FFFFF; for (int k = 0; k < t->bpp; k++) dc_vram[(o + k) & 0x7FFFFF] = (uint8_t)(v >> (8 * k)); }
    else for (int k = 0; k < t->bpp; k++) wr8_area32(o + k, (uint8_t)(v >> (8 * k)));
}
static void fb_done(const fb_target *t) {
    if (t->tex)                                            /* the texture cache sees the new contents */
        for (uint32_t pg = t->sof >> 12; pg <= ((t->sof + (uint32_t)t->HO * t->stride) >> 12) && pg < 0x800; pg++) dc_vram_gen[pg]++;
}

/* A render whose clip the game set to 0..0 (FB_X_CLIP = FB_Y_CLIP = 0) writes at most one pixel: the
 * framebuffer keeps its picture. Soulcalibur renders its VS screen like this while it loads the stage's
 * textures over the VS screen's, so the picture stays frozen for the wipe that follows. Clip registers
 * the game never wrote (power-on 0) still mean the whole screen. */
extern int dc_pvr_clip_written;
int dc_pvr_clip_empty(void) {
    return dc_pvr_clip_written && (REG(0x068) & 0x07FF07FFu) == 0 && (REG(0x06C) & 0x03FF03FFu) == 0;
}

/* the render's size: FB_X_CLIP / FB_Y_CLIP maxima */
void dc_pvr_target_size(int *w, int *h) {
    uint32_t xc = REG(0x068), yc = REG(0x06C);
    int W = (int)((xc >> 16) & 0x7FF) + 1, H = (int)((yc >> 16) & 0x3FF) + 1;
    if (W < 16 || W > 1280) W = 640;
    if (H < 16 || H > 1024) H = 480;
    *w = W; *h = H;
}

/* store a picture drawn elsewhere (GL) as this render's result: rgba is w x h, top row first */
void dc_pvr_sw_store(const uint8_t *rgba, int w, int h) {
    fb_target t = fb_target_of(w, h);
    for (int y = 0; y < t.HO && y < h; y++)
        for (int x = 0; x < t.WO && x < w; x++) {
            const uint8_t *p = rgba + ((size_t)y * w + x) * 4;
            fb_put(&t, x, y, fb_pack(t.mode, t.athr, p[0], p[1], p[2], p[3]));
        }
    fb_done(&t);
}

int dc_pvr_sw_render(void) {
    ctx_t *saved = ta_ctx, *rc = render_ctx();
    ctx_parse(rc);
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
        { int n3 = 0, sh[5] = {0}, tv[5] = {0}; for (int k = 0; k < nmt; k++) n3 += mt[k].list == 3;
        for (int k = 0; k < npl; k++) { if (pl[k].shadow) sh[pl[k].list & 7 & 3]++; if (pl[k].twovol) tv[pl[k].list & 3]++; }
        fprintf(stderr, "pvr: FPU_SHAD_SCALE %08X, shadow-flag polys by list %d/%d/%d/%d, two-volume %d/%d/%d/%d (0/1|4/2/3)\n", REG(0x074), sh[0], sh[1], sh[2], sh[3], tv[0], tv[1], tv[2], tv[3]);
        fprintf(stderr, "pvr: %dx%d bg %.2f %.2f %.2f z %g, %d polys, %d volume triangles (%d translucent)\n", RW, RH, bg[0], bg[1], bg[2], bz, npl, nmt, n3); }
        for (int i = 0; i < npl; i++) { const vtx *v = vt + pl[i].first;
            uint32_t ta = (pl[i].tcw & 0x1FFFFF) << 3;
            float bx0 = 1e30f, bx1 = -1e30f, by0 = 1e30f, by1 = -1e30f, bz0 = 1e30f, bz1 = -1e30f;
            for (int k = 0; k < pl[i].n; k++) {
                if (v[k].x < bx0) bx0 = v[k].x; if (v[k].x > bx1) bx1 = v[k].x; if (v[k].y < by0) by0 = v[k].y; if (v[k].y > by1) by1 = v[k].y;
                if (v[k].z < bz0) bz0 = v[k].z; if (v[k].z > bz1) bz1 = v[k].z; }
            fprintf(stderr, " box x %.1f..%.1f y %.1f..%.1f z %g..%g\n", bx0, bx1, by0, by1, bz0, bz1);
            fprintf(stderr, " poly list %d n %d sprite %d isp %08X tsp %08X tcw %08X v0 %.1f,%.1f,%.2f uv %.3f,%.3f c %.2f %.2f %.2f %.2f tex@%06X: %02X%02X %02X%02X %02X%02X\n", pl[i].list, pl[i].n, pl[i].sprite,
                    pl[i].isp, pl[i].tsp, pl[i].tcw, v->x, v->y, v->z, v->u, v->v, v->c[0], v->c[1], v->c[2], v->c[3], ta,
                    dc_vram[ta + 1], dc_vram[ta], dc_vram[ta + 3], dc_vram[ta + 2], dc_vram[ta + 0x401], dc_vram[ta + 0x400]); }
        dc_pvr_sw_debug = 0; mv_dbg = 1;
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
    /* write the framebuffer. FB_W_SOF1 bit 24: render to texture, written in the 64-bit (texture)
     * layout, not the 32-bit framebuffer one (Tennis 2K2 captures frames this way for its crossfades) */
    fb_target t = fb_target_of(W, H);
    /* a framebuffer gets the clip rectangle only (FB_X_CLIP / FB_Y_CLIP minima too): a band render keeps
     * the picture above and left of it, as on the console; a texture target is written whole */
    int x0 = t.tex ? 0 : (int)(xc & 0x7FF), y0 = t.tex ? 0 : (int)(yc & 0x3FF);
    if (x0 >= t.WO) x0 = 0;
    if (y0 >= t.HO) y0 = 0;
    for (int y = y0; y < t.HO; y++)
        for (int x = x0; x < t.WO; x++) {
            float c[4];
            if (hscale) { const float *p0 = cbuf + (y * RW + 2 * x) * 4, *p1 = p0 + 4; for (int k = 0; k < 4; k++) c[k] = (p0[k] + p1[k]) * 0.5f; }
            else memcpy(c, cbuf + (y * RW + x) * 4, 16);
            fb_put(&t, x, y, fb_pack(t.mode, t.athr, (uint32_t)(c[0] * 255 + .5f), (uint32_t)(c[1] * 255 + .5f), (uint32_t)(c[2] * 255 + .5f), (uint32_t)(c[3] * 255 + .5f)));
        }
    fb_done(&t);
    int n = npl;
    ta_ctx = saved;
    return n;
}

void dc_pvr_sw_install(void) {
    u8f_init();
    for (int i = 0; i < NCTX; i++) if (!ctxs[i].used && !ctxs[i].nraw) {   /* a context never initialised: no strip open */
        ctxs[i].ps.open = -1; ctxs[i].ps.ux1 = ctxs[i].ps.uy1 = 4096; ctxs[i].ps0 = ctxs[i].ps;
    }
    dc_ta_set_sink(sink);
}

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

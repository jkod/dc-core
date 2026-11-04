/* dc-core texture packs: the name a texture goes by, saving and loading PNGs, and extracting every
 * texture from the disc without playing.
 *
 * A texture is named by a hash of its decoded pixels (0xAARRGGBB) and its size: <hash>_<w>x<h>.png.
 * The renderer dumps and replaces textures by that name (pvr_gl.c); the extractor here decodes the
 * disc's textures with the renderer's own decoder (pvr_get_texture), so its names are the same ones
 * the game's uploads get. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "dccore/disc.h"
#include "dccore/hw.h"
#include "dccore/texpack.h"
#include "pvr_int.h"

#define STBI_ONLY_PNG
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "../third_party/stb_image.h"
#include "../third_party/stb_image_write.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#ifdef _WIN32
#include <direct.h>
#define tex_mkdir(d) _mkdir(d)
#else
#define tex_mkdir(d) mkdir(d, 0755)
#endif

uint64_t dc_tex_hash(const uint32_t *px, int w, int h) {
    uint64_t x = 0x9E3779B97F4A7C15ull ^ ((uint64_t)(uint32_t)w << 32 | (uint32_t)h);
    size_t n = (size_t)w * (size_t)h;
    for (size_t i = 0; i < n; i++) {
        x ^= px[i]; x *= 0xFF51AFD7ED558CCDull; x ^= x >> 32;
    }
    x ^= x >> 33; x *= 0xC4CEB9FE1A85EC53ull; x ^= x >> 33;
    return x;
}

static void tex_path(char *out, size_t n, const char *dir, uint64_t h, int w, int hh) {
    snprintf(out, n, "%s/%016llx_%dx%d.png", dir, (unsigned long long)h, w, hh);
}

int dc_tex_save(const char *dir, uint64_t h, const uint32_t *px, int w, int hh) {
    static char made[1024];
    if (strcmp(made, dir)) { tex_mkdir(dir); snprintf(made, sizeof made, "%s", dir); }
    char path[1100]; tex_path(path, sizeof path, dir, h, w, hh);
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return 0; }
    size_t n = (size_t)w * (size_t)hh;
    uint8_t *rgba = malloc(n * 4);
    if (!rgba) return -1;
    for (size_t i = 0; i < n; i++) {
        uint32_t p = px[i];
        rgba[i * 4] = (uint8_t)(p >> 16); rgba[i * 4 + 1] = (uint8_t)(p >> 8); rgba[i * 4 + 2] = (uint8_t)p; rgba[i * 4 + 3] = (uint8_t)(p >> 24);
    }
    int ok = stbi_write_png(path, w, hh, 4, rgba, w * 4);
    free(rgba);
    if (!ok) { static int warned; if (!warned++) fprintf(stderr, "dc-core: cannot write %s\n", path); return -1; }
    return 1;
}

uint8_t *dc_tex_load(const char *dir, uint64_t h, int w, int hh, int *rw, int *rh) {
    char path[1100]; tex_path(path, sizeof path, dir, h, w, hh);
    int comp = 0;
    return stbi_load(path, rw, rh, &comp, 4);
}
void dc_tex_free(uint8_t *px) { stbi_image_free(px); }

/* ------------------------------------------------------------------ extraction */
struct dc_tex_extract {
    const char *dir, *file;
    unsigned long saved, seen, skipped;
    uint64_t *hashes; size_t nh, caph;      /* open addressing, 0 = empty */
};

static int seen_before(dc_tex_extract *x, uint64_t h) {
    if (!h) h = 1;
    if (x->nh * 2 >= x->caph) {
        size_t nc = x->caph ? x->caph * 2 : 4096;
        uint64_t *t = calloc(nc, sizeof *t);
        if (!t) return 1;
        for (size_t i = 0; i < x->caph; i++) if (x->hashes[i])
            for (size_t j = x->hashes[i] & (nc - 1); ; j = (j + 1) & (nc - 1)) if (!t[j]) { t[j] = x->hashes[i]; break; }
        free(x->hashes); x->hashes = t; x->caph = nc;
    }
    for (size_t j = h & (x->caph - 1); ; j = (j + 1) & (x->caph - 1)) {
        if (x->hashes[j] == h) return 1;
        if (!x->hashes[j]) { x->hashes[j] = h; x->nh++; return 0; }
    }
}

int dc_tex_extract_argb(dc_tex_extract *x, const uint32_t *px, int w, int h) {
    if (w <= 0 || h <= 0) return -1;
    uint64_t hs = dc_tex_hash(px, w, h);
    x->seen++;
    if (seen_before(x, hs)) return 0;
    int r = dc_tex_save(x->dir, hs, px, w, h);
    if (r > 0) x->saved++;
    return r;
}

void dc_tex_extract_skip(dc_tex_extract *x) { x->skipped++; }
const char *dc_tex_extract_file(const dc_tex_extract *x) { return x->file; }

/* the decoder reads textures from VRAM: put the data there, at an address of our own */
#define XADDR 0x200000u
static void vram_put(uint32_t addr, const uint8_t *src, size_t n) {
    for (size_t i = 0; i < n; i++) dc_vram[(addr + i) & dc_vram_tex_mask] = src[i];
    for (uint32_t pg = addr >> 12; pg <= (uint32_t)((addr + n) >> 12) && pg < (dc_vram_tex_mask + 1) >> 12; pg++) dc_vram_gen[pg]++;
}
static int log2i(int v) { int l = 0; while ((1 << l) < v) l++; return (1 << l) == v ? l : -1; }

int dc_tex_extract_pvr(dc_tex_extract *x, const void *data, size_t len, uint32_t tcw, int w, int h) {
    int lw = log2i(w), lh = log2i(h);
    int fmt = (tcw >> 27) & 7, mip = (tcw >> 31) & 1, vq = (tcw >> 30) & 1;
    if (lw < 3 || lh < 3 || lw > 10 || lh > 10 || (mip && w != h)) { x->skipped++; return -1; }
    if (fmt == 5 || fmt == 6) { x->skipped++; return -1; }   /* palettes: set by the game while it runs */
    const uint8_t *d = data;
    size_t top = vq ? (size_t)w * h / 4 : (size_t)w * h * 2;
    if (len < top + (vq ? 8 : 0)) { x->skipped++; return -1; }
    /* where the decoder looks for the top level: after the smaller levels, which it skips by a table;
     * the data has them in front, however it packs them, so place the top level where it is looked for */
    static const uint32_t mip16[11] = {0x3, 0x4, 0x8, 0x18, 0x58, 0x158, 0x558, 0x1558, 0x5558, 0x15558, 0x55558};
    static const uint32_t mipvq[11] = {0x0, 0x1, 0x2, 0x6, 0x16, 0x56, 0x156, 0x556, 0x1556, 0x5556, 0x15556};
    if (vq) {
        size_t rest = len - top, cb = rest;                    /* codebook, then the smaller levels' indices */
        if (mip) { size_t small = mipvq[lw]; cb = rest > small ? rest - small : 0; }
        if (cb > 2048) cb = 2048;
        vram_put(XADDR, d, cb);                              /* entries 0.. (small VQ: only the ones used) */
        vram_put(XADDR + 2048 + (mip ? mipvq[lw] : 0), d + len - top, top);
    } else {
        uint32_t at = XADDR + (mip ? mip16[lw] * 2 : 0);
        vram_put(at, d + len - top, top);
    }
    uint32_t tsp = (uint32_t)(lw - 3) << 3 | (uint32_t)(lh - 3);
    pvr_texinfo ti;
    pvr_get_texture((tcw & ~0x1FFFFFu) | XADDR >> 3, tsp, &ti);
    return dc_tex_extract_argb(x, ti.px, ti.w, ti.h);
}

/* PVRT chunks (Sega's texture format, with or without a GBIX before it) anywhere in a file */
static void scan_pvrt(dc_tex_extract *x, const uint8_t *d, size_t n) {
    for (size_t i = 0; i + 16 <= n; i++) {
        if (d[i] != 'P' || memcmp(d + i, "PVRT", 4)) continue;
        uint32_t clen = d[i + 4] | d[i + 5] << 8 | d[i + 6] << 16 | (uint32_t)d[i + 7] << 24;
        int pf = d[i + 8], df = d[i + 9], w = d[i + 12] | d[i + 13] << 8, h = d[i + 14] | d[i + 15] << 8;
        if (clen < 8 || w < 8 || h < 8) continue;
        size_t len = clen - 8;
        if (len > n - i - 16) len = n - i - 16;
        if (pf > 4) { x->skipped++; continue; }
        uint32_t tcw = (uint32_t)pf << 27;
        switch (df) {
        case 0x01: case 0x0D: break;                             /* twiddled (square / rectangle) */
        case 0x02: case 0x12: tcw |= 1u << 31; break;            /* twiddled, mipmaps */
        case 0x03: case 0x10: tcw |= 1u << 30; break;            /* VQ (small VQ: a shorter codebook) */
        case 0x04: case 0x11: tcw |= 3u << 30; break;            /* VQ, mipmaps */
        case 0x09: tcw |= 1u << 26; break;                       /* rectangle (scan order) */
        default: x->skipped++; continue;                         /* palettes, stride */
        }
        if (dc_tex_extract_pvr(x, d + i + 16, len, tcw, w, h) >= 0) i += 15 + len;
    }
}

typedef struct { dc_tex_extract *x; dc_tex_unpacker game; void *user; unsigned files; } walk_ctx;
static int walk_file(const char *path, uint32_t lba, uint32_t size, void *u) {
    walk_ctx *c = u;
    if (!size) return 0;
    uint8_t *d = malloc(((size_t)size + 2047) / 2048 * 2048);
    if (!d) return 0;
    if (!dc_disc_read(lba, (size + 2047) / 2048, d)) {
        c->x->file = path;
        unsigned long before = c->x->seen;
        scan_pvrt(c->x, d, size);
        if (c->game) c->game(c->x, path, d, size, c->user);
        if (c->x->seen != before) c->files++;
    }
    free(d);
    return 0;
}

long dc_tex_extract_disc(const char *dir, dc_tex_unpacker game, void *user) {
    dc_tex_extract x; memset(&x, 0, sizeof x);
    x.dir = dir;
    walk_ctx c = { &x, game, user, 0 };
    if (dc_disc_walk(walk_file, &c) < 0) { fprintf(stderr, "dc-core: no file system on the disc\n"); return -1; }
    fprintf(stderr, "dc-core: %lu textures from %u files, %lu new in %s (%lu skipped: paletted or unsupported)\n",
            x.seen, c.files, x.saved, dir, x.skipped);
    free(x.hashes);
    return (long)x.saved;
}

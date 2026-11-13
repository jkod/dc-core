/* dc-core internal: the frame the TA collected, shared by the PowerVR backends (pvr_sw.c, pvr_gl.c). */
#ifndef DCCORE_PVR_INT_H
#define DCCORE_PVR_INT_H
#include <stdint.h>

typedef struct {
    float x, y, z, u, v;                /* screen x/y, z = 1/w */
    float c[4], o[4];                   /* base / offset colour r, g, b, a (0..1) */
    float u1, v1, c1[4], o1[4];         /* second parameter set (two-volume polygons) */
} pvr_vtx;
typedef struct {
    uint32_t isp, tsp, tcw, tsp1, tcw1;
    int list, first, n, sprite;         /* list: 0 opaque, 2 translucent, 4 punch-through */
    int twovol, shadow;                 /* PCW Volume / Shadow bits */
    int uclip;                          /* user tile clip: 0 off, 2 draw inside, 3 draw outside the rectangle */
    int ux0, uy0, ux1, uy1;             /* that rectangle in scene pixels, exclusive end */
} pvr_poly;
typedef struct { float x[3], y[3], z[3]; int inst, list, cull; } pvr_modtri;   /* cull: the volume header's ISP culling mode */

typedef struct {
    const pvr_vtx *verts; int nverts;
    const pvr_poly *polys; int npolys;
    const pvr_modtri *mods; int nmods;
    int width, height;                  /* output picture: the display size (FB_R_SIZE), at least the clip */
    int clip_x0, clip_y0, clip_x1, clip_y1;   /* drawn area, inclusive (FB_X_CLIP / FB_Y_CLIP) */
    int hscale;                         /* scene coordinates are 2x wide (SCALER_CTL) */
    float bg[4], bgz;                   /* background plane colour and depth */
} pvr_frame;
void pvr_region_extent(int *w, int *h);   /* pixels the region array lists tiles for (0 if none) */

/* the context STARTRENDER draws (matched by PARAM_BASE), plus render size and background */
void pvr_get_frame(pvr_frame *f);
/* The same for a renderer on its own thread, in two steps. On the game's thread, at STARTRENDER:
 * the sizes and background, and a copy of that context as the TA was sent it (not yet its lists);
 * tex: tcw, tsp pairs of its textured polygons in the order they came (valid until the TA is next
 * sent a list). Then on the renderer's thread: the copy's lists into f. */
void pvr_snap_take(pvr_frame *f, const uint32_t **tex, int *ntex);
void pvr_snap_lists(pvr_frame *f);
/* decoded texture (0xAARRGGBB texels), cached until its VRAM pages or the palette change;
 * id changes whenever the pixels do. soft: its alpha is a gradient (of the texels that are not clear,
 * more lie between clear and opaque than are opaque: smoke, a wake, tinted glass), not a cut-out
 * (a railing, a figure on a card, whose only in-between texels are its edges) */
typedef struct { int w, h; const uint32_t *px; uint64_t id; int soft; } pvr_texinfo;
void pvr_get_texture(uint32_t tcw, uint32_t tsp, pvr_texinfo *t);
float pvr_fog_alpha(float invw);       /* FOG_TABLE lookup for a 1/w value */
extern unsigned long pvr_sink_vertices; /* vertices that came through the TA's sink, not straight from the fast code (sh4.h dc_fpref) */
#endif

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
typedef struct { float x[3], y[3], z[3]; int inst, list; } pvr_modtri;

typedef struct {
    const pvr_vtx *verts; int nverts;
    const pvr_poly *polys; int npolys;
    const pvr_modtri *mods; int nmods;
    int width, height;                  /* output picture: the display size (FB_R_SIZE), at least the clip */
    int clip_x0, clip_y0, clip_x1, clip_y1;   /* drawn area, inclusive (FB_X_CLIP / FB_Y_CLIP) */
    int hscale;                         /* scene coordinates are 2x wide (SCALER_CTL) */
    float bg[4], bgz;                   /* background plane colour and depth */
} pvr_frame;

/* the context STARTRENDER draws (matched by PARAM_BASE), plus render size and background */
void pvr_get_frame(pvr_frame *f);
/* decoded texture (0xAARRGGBB texels), cached until its VRAM pages or the palette change;
 * id changes whenever the pixels do */
typedef struct { int w, h; const uint32_t *px; uint64_t id; } pvr_texinfo;
void pvr_get_texture(uint32_t tcw, uint32_t tsp, pvr_texinfo *t);
float pvr_fog_alpha(float invw);       /* FOG_TABLE lookup for a 1/w value */
#endif

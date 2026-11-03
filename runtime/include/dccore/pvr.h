#ifndef DCCORE_PVR_H
#define DCCORE_PVR_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Software PowerVR2 renderer (pvr_sw.c). dc_pvr_sw_install() subscribes to the TA; call
 * dc_pvr_sw_render() at STARTRENDER (it draws into the framebuffer in VRAM and returns the number
 * of polygons drawn). dc_pvr_display() converts the framebuffer the video output shows into RGB8
 * (returns -1 when the display is off). */
void dc_pvr_sw_install(void);
extern int dc_pvr_sw_debug;
extern int dc_pvr_sw_highlight;   /* debug: paint a texture class magenta */   /* set: the next render lists its polygons on stderr */
int  dc_pvr_sw_render(void);
int  dc_pvr_display(uint8_t *rgb, int maxw, int maxh, int *w, int *h);

/* OpenGL 3.3 core backend (pvr_gl.c), needs a current context. getproc: SDL_GL_GetProcAddress,
 * eglGetProcAddress, ... dc_pvr_gl_render(scale) draws the frame at scale x the console
 * resolution into an offscreen framebuffer; dc_pvr_gl_present() puts it on the current window
 * (stretch = 0 keeps 4:3); dc_pvr_gl_read() reads it back as RGB8, top row first. Uses the
 * software renderer's TA collection: call dc_pvr_sw_install() too. */
int  dc_pvr_gl_init(void *(*getproc)(const char *name));
void dc_pvr_gl_render(int scale);
void dc_pvr_gl_present(int win_w, int win_h, int stretch);
void dc_pvr_gl_read(uint8_t *rgb, int *w, int *h);
void dc_pvr_gl_read_window(uint8_t *rgb, int win_w, int win_h);   /* the presented window (before swapping), RGB8 */
extern int dc_pvr_gl_fill_lines;    /* show only the lines the game draws, filling the window height (default 1) */
extern int dc_pvr_gl_side_fill;     /* widescreen: fill empty side bars with the picture's edge colour (default 1) */
extern int dc_pvr_gl_letterbox_4_3;  /* widescreen: frames with cut-scene letterbox bars are shown at 4:3, black sides (default 1);
                                      * 0: shown wide like the rest, the bars stretched across the full width */
extern int dc_pvr_gl_tile_clamp;     /* big screen-parallel textured pieces with UVs in 0..1 sample clamped to their UV range: no seams between 2D background tiles when scaled up (default 1) */
extern int dc_pvr_gl_keep_outside;    /* a render of under 3/4 of the lines into a buffer rendered whole before keeps that picture outside its clip, as on the console (default 1) */
extern int dc_pvr_gl_letterbox_zoom;  /* widescreen: with dc_pvr_gl_letterbox_4_3, the picture between the bars is cropped to the window's shape and fills it (default 0) */
extern int dc_pvr_gl_hud_before_shade;  /* widescreen: polygons drawn before a full-screen pause shade are HUD even when
                                         * they lie above it (they keep moving); only those after it are the menu (default 0) */
extern int dc_pvr_gl_show_4_3;       /* widescreen: set by a port for screens framed for 4:3 (e.g. a 2D title picture over a 3D
                                      * scene): drawn at 4:3 with black sides, the HUD left in place (default 0) */
void dc_pvr_gl_load_rgb(const uint8_t *rgb, int w, int h);   /* show a CPU-drawn framebuffer instead */
/* 1 = render 16:9: the 4:3 picture in the middle, the sides filled with geometry the game draws
 * beyond the screen edges (games usually cull it, so a per-game camera patch gives the real thing) */
extern int dc_pvr_gl_widescreen;
extern float dc_pvr_gl_aspect;        /* widescreen: the window's width / height, so the picture fills it (0 = 16:9) */
/* enhancements beyond the console (off by default): mipmaps for every bilinear-filtered texture,
 * and anisotropic filtering up to this many samples where the driver supports it */
extern int dc_pvr_gl_mipmaps;
extern float dc_pvr_gl_anisotropy;
/* widescreen HUD: polygons with every vertex at the same 1/w, at least this, are treated as 2D
 * overlay, and those in the left / right third of the 4:3 picture move to the edges (0 = off; a
 * per-game value: Rush 2049 draws its HUD at 1/w 1.0 (panels) and 1000+ (sprites), so 1.0) */
extern float dc_pvr_gl_hud_z;
/* widescreen HUD: flat polygons at or above this 1/w are not HUD and never move (0 = no limit). For
 * markers that follow the 3D scene but are drawn flat in front of it (Tennis 2K2's "1P" / "COM" at
 * 1/w 8 and 50, its HUD at 1.0 - 3.5) */
extern float dc_pvr_gl_hud_zmax;
/* widescreen HUD: only polygons with every vertex at exactly the same 1/w are HUD (no tolerance, no small
 * 3D models). For games whose HUD is exactly flat while 3D models share its depth range (Jet Set Radio's
 * title logo at 1/w 1.0 - 1.17, its HUD at 1.11 - 1.14 and 6.67) (default 0) */
extern int dc_pvr_gl_hud_exact;
/* widescreen: frames without a 3D scene reaching past the 4:3 picture (2D menus) are cut to the 4:3
 * picture, so the side bars replace what the game draws beyond its edges (default 0) */
extern int dc_pvr_gl_clip_2d;
/* widescreen: 2D backdrops drawn exactly across the 4:3 picture (menu gradients and bands, sky cards)
 * continue to the screen edges by repeating their edge column (default 0) */
extern int dc_pvr_gl_extend_backdrops;
/* render to texture (FB_W_SOF1 bit 24) in widescreen: also draw the pass at full width, for
 * full-screen quads that show that texture later (call at STARTRENDER, with dc_pvr_sw_render) */
void dc_pvr_gl_capture(int scale);
/* the same, and the picture also goes to the texture in VRAM (read back from GL, instead of a software
 * render): 0 when GL cannot (not widescreen / not ready), then use dc_pvr_sw_render + dc_pvr_gl_capture */
int  dc_pvr_gl_capture_to_vram(int scale);
/* translucent polygons: their fully opaque fragments drawn first with depth, so they hide what is
 * behind them whatever the polygon order (default 1; 0 = sorted per polygon only) */
extern int dc_pvr_gl_tsort2;
extern int dc_pvr_gl_split_panel_x;   /* widescreen: split-screen views clipped to [0, x) beside a side panel become full width (0 = off) */
extern int dc_pvr_gl_hud_move;        /* widescreen: HUD pieces move out to the screen edges (default 1) */
extern int dc_pvr_gl_hud_sharp;       /* HUD textures (flat polygons at >= dc_pvr_gl_hud_z) get a bicubic filter (default 1) */
#ifdef __cplusplus
}
#endif
#endif

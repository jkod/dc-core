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
/* the game set the render clip to 0..0: the render writes nothing visible, the framebuffer keeps its picture */
int dc_pvr_clip_empty(void);
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
/* on the game's thread, before a dc_pvr_gl_render on another: takes the frame (TA lists, registers,
 * textures) so that render reads nothing of the console. 0: not this frame, render the usual way. */
int  dc_pvr_gl_prepare(void);
void dc_pvr_gl_present(int win_w, int win_h, int stretch);
void dc_pvr_gl_read(uint8_t *rgb, int *w, int *h);
void dc_pvr_gl_read_window(uint8_t *rgb, int win_w, int win_h);   /* the presented window (before swapping), RGB8 */
extern int dc_pvr_gl_scanlines;     /* CRT-style scanlines over each console line: 0 off (default), 1 light, 2 medium, 3 strong */
/* an RGBA image (top row first) over the presented window at x, y (window pixels, top-left), w x h
 * scaled to dw x dh without smoothing, alpha-blended: the frontend's on-screen menu */
void dc_pvr_gl_overlay(const uint8_t *rgba, int w, int h, int x, int y, int dw, int dh, int win_w, int win_h);
extern int dc_pvr_gl_fill_lines;    /* show only the lines the game draws, filling the window height (default 1) */
extern int dc_pvr_gl_side_fill;     /* widescreen: fill empty side bars with the picture's edge colour (default 1) */
extern int dc_pvr_gl_letterbox_4_3;  /* widescreen: frames with cut-scene letterbox bars are shown at 4:3, black sides (default 1);
                                      * 0: shown wide like the rest, the bars stretched across the full width */
extern int dc_pvr_gl_tile_clamp;     /* big screen-parallel textured pieces with UVs in 0..1 sample clamped to their UV range: no seams between 2D background tiles when scaled up (default 1) */
extern int dc_pvr_gl_camera_plane;    /* pieces with corners on the camera plane held over from the last frame or dropped (Soulcalibur's opening); default 0: drawn as they come */
extern int dc_pvr_gl_keep_outside;    /* a render of under 3/4 of the lines into a buffer rendered whole before keeps that picture outside its clip, as on the console (default 1) */
extern int dc_pvr_gl_letterbox_zoom;  /* widescreen: with dc_pvr_gl_letterbox_4_3, the picture between the bars is cropped to the window's shape and fills it (default 0) */
extern int dc_pvr_gl_shade_translucent; /* widescreen: a full-screen pause shade can be any colour of translucent quad, textured or
                                         * not (Daytona USA 2001's is a white-modulated texture at 60% alpha); default 0: untextured
                                         * or black only */
extern int dc_pvr_gl_hud_panel_rows;   /* widescreen: HUD within the rows of a wide centred HUD panel (40%+ of the width)
                                          * stays beside it instead of moving to the edge (Tennis 2K2's match results:
                                          * names and 1P / COM by the score box). Default 0 */
extern int dc_pvr_gl_shade_any_z;      /* widescreen: a dark full-screen shade made of tiles counts at any depth, not
                                          * only at HUD depth: what is drawn over it stays centred with it (Tennis
                                          * 2K2's match results: names and 1P / COM beside the box). Default 0 */
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
extern int dc_pvr_gl_msaa;              /* multisample anti-aliasing: samples a pixel (0/1 off; clamped to the GPU's max) */
extern int dc_pvr_gl_fast_depth;        /* 1 (default): depth from the vertices into a float buffer where the driver can, so the
                                           GPU skips hidden pixels; 0: the logarithm per pixel (pvr_gl.c: zv_on) */
int dc_pvr_gl_fast_depth_ok(void);       /* the driver can (GL 4.5 / ARB_clip_control, and it passed the test) */
extern const char *dc_pvr_gl_shader_list; /* file listing the shader variants a game has used: built at start-up next time */
extern int dc_pvr_gl_half_step;         /* render half a step under the scale asked for (2 -> 1.5: 960x720) */
extern float dc_pvr_gl_sharpen;          /* 0 off .. 1: contrast-adaptive sharpening when presenting */
extern int dc_pvr_gl_gpu_bars;           /* side bars decided on the GPU, so the CPU never waits for it (default on; 0: read back) */
extern int dc_pvr_gl_plain_scaling;      /* 1: plain linear scaling to the window, no supersampled downscale */
/* texture packs: textures are named <hash>_<w>x<h>.png by a hash of their decoded pixels. With
 * dc_pvr_gl_tex_dump set to a folder, every texture the game uploads is saved there once; with
 * dc_pvr_gl_tex_pack set, a PNG of that name in that folder (any size: upscaled, redrawn) is used
 * instead. Counters: textures written / distinct textures replaced so far. */
extern const char *dc_pvr_gl_tex_dump;
extern const char *dc_pvr_gl_tex_pack;
extern unsigned long dc_pvr_gl_tex_dumped, dc_pvr_gl_tex_replaced;
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
 * continue to the screen edges by repeating their edge column; flat cards wider than the picture (to
 * half a picture past each edge) continue with their texture mirrored at their edges; a backdrop made
 * of a mesh of small flat untextured quads at one depth (a glow) continues in its edge colours
 * (default 0) */
extern int dc_pvr_gl_extend_backdrops;
/* with it: a picture made of opaque, flat, screen-aligned tiles continues past the 4:3 edges as its
 * mirror image (each tile within a side width of an edge is drawn again reflected about it; default 0,
 * menus whose tiles should keep side bars leave it off); a wall of pattern tiles (24+ quads at one
 * depth across the picture, each showing the same whole texture, turning or not) has its outer
 * columns repeated outwards a cell at a time */
/* with dc_pvr_gl_extend_backdrops: a textured card spanning the picture (two corners on each side)
 * continues as its mirror image instead of its edge column repeated (default 0) */
extern int dc_pvr_gl_extend_mirror;
extern int dc_pvr_gl_extend_tiles;
/* with it: only tiles at least this many pixels wide (default 0: all); a port can set it per field, e.g.
 * to mirror a picture of small tiles on one screen but not on menus built the same way */
extern float dc_pvr_gl_extend_tiles_min;
/* render to texture (FB_W_SOF1 bit 24) in widescreen: also draw the pass at full width, for
 * full-screen quads that show that texture later (call at STARTRENDER, with dc_pvr_sw_render) */
void dc_pvr_gl_capture(int scale);
/* the same, and the picture also goes to the texture in VRAM (read back from GL, instead of a software
 * render): 0 when GL cannot (not widescreen / not ready), then use dc_pvr_sw_render + dc_pvr_gl_capture */
int  dc_pvr_gl_capture_to_vram(int scale);
/* translucent polygons (auto-sorted): deep, uniformly see-through ones (a fence along a court side)
 * are sorted per triangle, not as a whole, so they no longer cover smaller ones in front of parts of
 * them (default 1; 0 = sorted per polygon only) */
extern int dc_pvr_gl_tsort2;
/* translucent polygons (auto-sorted): their fully opaque fragments drawn first with depth, the rest sorted
 * after (default 0). Daytona USA 2001's car decals over the coplanar layers of the paint need it. Flat polygons (2D
 * screens) keep their order */
extern int dc_pvr_gl_topaque;
extern int dc_pvr_gl_split_panel_x;   /* widescreen: split-screen views clipped to [0, x) beside a side panel become full width (0 = off) */
extern int dc_pvr_gl_hud_sprites;       /* widescreen: only sprite polygons are HUD (Daytona USA 2001: its HUD is all sprites, while
                                         * the "ROLLING START!!" banner's strips at the HUD's depth, now and then exactly flat, are not) */
/* widescreen: a game's fixed HUD places, in the console's pixels (640 x 480), with the side each moves to
 * (-1 left edge, 1 right edge, 0 stays). A HUD piece whose centre lies in a zone moves with it and joins
 * no group, so a message that appears next to it cannot pull it back (Daytona USA 2001's lap counter and
 * "TIME EXTENSION!!"), and a piece that zooms in moves the same way all the time (its position number).
 * Pieces outside every zone are grouped as before. A game sets dc_pvr_gl_hud_nzones to 0 on screens
 * where the zones do not apply. */
typedef struct { float x0, y0, x1, y1; int side;
    /* > 0: pieces in the zone stretch instead of moving: its outer end goes out to the screen edge, its
     * inner end stays, x in between scales; pieces within this many pixels of either end move rigidly
     * with it (a fighting game's health bar reaching from the edge to the timer, its caps kept) */
    int stretch;
    /* != 0: pieces in the zone move by this fraction of the extra width on each side instead of by side
     * (-1 .. 1; a row of panels spreads evenly over the width: five panels at -0.8, -0.4, 0, 0.4, 0.8,
     * Propeller Arena's teams) */
    float move; } dc_hud_zone;
extern const dc_hud_zone *dc_pvr_gl_hud_zones;
extern int dc_pvr_gl_hud_nzones;
/* widescreen: a port's callback naming HUD pieces (flat or 3D, at or above dc_pvr_gl_hud_z) that go to a screen edge
 * on every frame, menus and shaded screens included (not in 4:3 letterboxed frames): given the piece's box in
 * 640 x 480 picture units, it returns -1 (left edge), 1 (right edge) or 0 (as usual). Sega Rally 2: its logo. */
extern int (*dc_pvr_gl_hud_pin)(float x0, float y0, float x1, float y1);
extern int dc_pvr_gl_hud_move;        /* widescreen: HUD pieces move out to the screen edges (default 1) */
/* widescreen HUD: a group of touching pieces that spans most of the width moves piece by piece instead of
 * staying centred: a fighting game's health bars joined by the timer between them (Dead or Alive 2) (default 0) */
extern int dc_pvr_gl_hud_split_wide;
/* widescreen HUD: a flat bar drawn across the whole 4:3 width (reaching both edges, at most a quarter of the
 * height) is stretched out to the screen edges instead of staying 4:3 wide; what is drawn on it moves as
 * usual (Propeller Arena's pause menu "Return to game" strip) (default 0) */
extern int dc_pvr_gl_hud_bars;
extern int dc_pvr_gl_hud_sharp;       /* HUD textures (flat polygons at >= dc_pvr_gl_hud_z) get a bicubic filter (default 1) */
#ifdef __cplusplus
}
#endif
#endif

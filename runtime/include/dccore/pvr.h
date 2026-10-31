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
#ifdef __cplusplus
}
#endif
#endif

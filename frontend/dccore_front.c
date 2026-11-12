/* dc-core desktop frontend: SDL2 window + OpenGL 3.3, audio, input, real-time pacing.
 * See dccore_front.h. */
#include "dccore_front.h"
#include "dccore/runtime.h"
#include "dccore/hw.h"
#include "dccore/pvr.h"
#include "dccore/aica.h"
#include "dccore/disc.h"
#include "dccore/maple.h"
#define SDL_MAIN_HANDLED
#include <SDL.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32) && !defined(__SWITCH__)
#include <unistd.h>
#endif
#ifndef _WIN32
#include <sys/stat.h>
#endif
#include <time.h>

extern sh4ctx *dc_current_ctx;         /* dispatch.c */

/* game speed (F5 slower, F6 faster, F7 normal, or the controls bar / menu): emulated time runs at this
 * rate against the wall clock. Sound is muted away from 1x (it would only stutter or skip). */
static const float speed_steps[] = { 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f };
#define SPEED_N ((int)(sizeof speed_steps / sizeof speed_steps[0]))
#define SPEED_1X 3
static int speed_i = SPEED_1X;
static float game_speed(void) { return speed_steps[speed_i]; }
static int frame_skip(void) { float v = game_speed(); return v > 1.f ? (int)(v + 0.99f) : 1; }   /* draw every nth frame when faster */

static SDL_Window *win;
static SDL_GLContext glc;
static SDL_AudioDeviceID audio;
#ifdef __SWITCH__
#define DC_NX_AUDIO 1
/* sound straight to audout from a thread of ours (dccore_nxthread.c): SDL's audio thread crashed when it ended */
int dc_nx_audio_open(void (*cb)(void *, uint8_t *, int), void *user);
void dc_nx_audio_pause(int on); void dc_nx_audio_lock(void); void dc_nx_audio_unlock(void); void dc_nx_audio_close(void);
static void audio_pause(int on) { dc_nx_audio_pause(on); }
static void audio_lock(int on) { if (on) dc_nx_audio_lock(); else dc_nx_audio_unlock(); }
static void audio_close(void) { dc_nx_audio_close(); }
#else
#define DC_NX_AUDIO 0
static void audio_pause(int on) { SDL_PauseAudioDevice(audio, on); }
static void audio_lock(int on) { if (on) SDL_LockAudioDevice(audio); else SDL_UnlockAudioDevice(audio); }
static void audio_close(void) { SDL_CloseAudioDevice(audio); }
#endif
static SDL_GameController *pads[4];     /* gamepad k drives Dreamcast port k (port A also has the keyboard) */
static void pad_open(int device_index) {
    if (!SDL_IsGameController(device_index)) return;
    SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(device_index);
    for (int k = 0; k < 4; k++)
        if (pads[k] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[k])) == id) return;   /* already open */
    /* port: an XInput pad's player number (its ring light: player 1 -> port A) when that port is free or
     * held by a pad without one (virtual or unnumbered devices), otherwise the first free port */
    int want = SDL_JoystickGetDevicePlayerIndex(device_index), slot = -1;
    if (want >= 0 && want < 4) {
        if (!pads[want]) slot = want;
        else if (SDL_GameControllerGetPlayerIndex(pads[want]) < 0) {
            for (int k = 0; k < 4; k++) if (!pads[k]) {     /* move the unnumbered pad to a free port */
                pads[k] = pads[want]; pads[want] = NULL; dc_pad[k].connected = 1;
                fprintf(stderr, "controller %s: moved to port %c\n", SDL_GameControllerName(pads[k]), 'A' + k);
                break;
            }
            if (!pads[want]) slot = want;
        }
    }
    for (int k = 0; k < 4 && slot < 0; k++) if (!pads[k]) slot = k;
    if (slot < 0) { fprintf(stderr, "controller %s: no free port\n", SDL_GameControllerNameForIndex(device_index)); return; }
    pads[slot] = SDL_GameControllerOpen(device_index);
    if (pads[slot]) {
        fprintf(stderr, "controller %s (player %d): port %c\n", SDL_GameControllerName(pads[slot]), want + 1, 'A' + slot);
        dc_pad[slot].connected = 1;
    } else fprintf(stderr, "controller %d: %s\n", device_index, SDL_GetError());
}
static void pad_close(SDL_JoystickID id) {
    for (int k = 0; k < 4; k++)
        if (pads[k] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[k])) == id) {
            SDL_GameControllerClose(pads[k]); pads[k] = NULL;
            fprintf(stderr, "controller %d disconnected\n", k + 1);
        }
}
#ifdef __SWITCH__
static SDL_Joystick *joy;
/* the CPU clock, from the on-screen menu (CPU CLOCK) and kept in osd.cfg: 0 = the system's (dccore_nxthread.c) */
int dc_nx_set_cpu_mhz(int mhz);
static void set_cpu_clock(int mhz) {
    int r = dc_nx_set_cpu_mhz(mhz);
    if (r == -1) fprintf(stderr, "CPU clock: Horizon OC's profile decides\n");
    else if (r < 0) fprintf(stderr, "CPU clock: can't be set here (%d)\n", r);
    else fprintf(stderr, "CPU clock %d MHz\n", r);
}
int dc_nx_set_gpu_mhz(int mhz);
void dc_nx_gpu_check(void);
static void set_gpu_clock(int mhz) {
    int r = dc_nx_set_gpu_mhz(mhz);
    if (r == -1) fprintf(stderr, "GPU clock: Horizon OC's profile decides\n");
    else if (r < 0) fprintf(stderr, "GPU clock: can't be set here (%d)\n", r);
    else fprintf(stderr, "GPU clock %d MHz\n", r);
}
#else
/* a joystick SDL has no gamepad mapping for: buttons 0-3 = A B X Y, 7 or 9 = Start, axes 0/1 = stick,
 * axes 2/5 or 4/5 = triggers, hat 0 = d-pad (the usual XInput/DirectInput layout); it drives the
 * first port without a gamepad */
static SDL_Joystick *raw;
static void raw_open(int device_index) {
    if (raw) return;
    raw = SDL_JoystickOpen(device_index);
    if (raw) fprintf(stderr, "generic joystick %s: %d axes, %d buttons, %d hats\n", SDL_JoystickName(raw),
                     SDL_JoystickNumAxes(raw), SDL_JoystickNumButtons(raw), SDL_JoystickNumHats(raw));
}
static void raw_close(SDL_JoystickID id) {
    if (raw && SDL_JoystickInstanceID(raw) == id) { SDL_JoystickClose(raw); raw = NULL; fprintf(stderr, "generic joystick removed\n"); }
}
static void read_raw(uint16_t *b, int *jx, int *jy, int *lt, int *rt) {
    if (!raw) return;
    static const uint16_t bm[4] = { DC_BTN_A, DC_BTN_B, DC_BTN_X, DC_BTN_Y };
    int nb = SDL_JoystickNumButtons(raw), na = SDL_JoystickNumAxes(raw);
    for (int i = 0; i < 4 && i < nb; i++) if (SDL_JoystickGetButton(raw, i)) *b |= bm[i];
    if ((nb > 7 && SDL_JoystickGetButton(raw, 7)) || (nb > 9 && SDL_JoystickGetButton(raw, 9))) *b |= DC_BTN_START;
    if (SDL_JoystickNumHats(raw) > 0) {
        Uint8 h = SDL_JoystickGetHat(raw, 0);
        if (h & SDL_HAT_UP) *b |= DC_BTN_UP; if (h & SDL_HAT_DOWN) *b |= DC_BTN_DOWN;
        if (h & SDL_HAT_LEFT) *b |= DC_BTN_LEFT; if (h & SDL_HAT_RIGHT) *b |= DC_BTN_RIGHT;
    }
    if (na >= 2) {
        Sint16 ax = SDL_JoystickGetAxis(raw, 0), ay = SDL_JoystickGetAxis(raw, 1);
        if (ax < -4000 || ax > 4000) *jx = (ax + 32768) >> 8;
        if (ay < -4000 || ay > 4000) *jy = (ay + 32768) >> 8;
    }
    if (na >= 6) {                                      /* triggers rest at -32768 */
        int tl = (SDL_JoystickGetAxis(raw, 4) + 32768) >> 8, tr = (SDL_JoystickGetAxis(raw, 5) + 32768) >> 8;
        if (tl > *lt) *lt = tl > 255 ? 255 : tl;
        if (tr > *rt) *rt = tr > 255 ? 255 : tr;
    }
}
#endif
static dc_front_config cfg;
static unsigned long presented, renders, last_render_frame;
static unsigned long drawn;             /* frames really drawn: renders also counts the ones skipped (FRAME RATE 30, fast-forward) */
/* FRAME RATE for games without a 30 / 60 choice of their own (dc_front_fps60 < 0): at 30 a 60 fps game is
 * drawn every other frame (it still runs at 60): steady where the Switch can't draw each one. A game that
 * draws at 30 anyway is not touched. 30 by default on the Switch. */
#ifdef __SWITCH__
static int osd_half = 1;
#else
static int osd_half = 0;
#endif
static unsigned long last_drawn_vblank;
static int need_present = 1;
static uint64_t vblanks;
static Uint64 t0_count; static uint64_t t0_cycles; static int paced;

static void *getproc(const char *n) { return SDL_GL_GetProcAddress(n); }

/* The PVR's render targets and the VRAM write count over each when it was drawn: a displayed
 * framebuffer that is one of them and unchanged since is the picture already shown (the OpenGL
 * renderer does not write VRAM), so the framebuffer fallback below must not replace it. */
static struct { uint32_t addr, gen; int used; } rtarget[4];
static uint32_t fb_gen(uint32_t a) {                       /* a: 32-bit-area offset; 640x480 at 32 bpp, at most */
    uint32_t g = 0;
    for (uint32_t p = a >> 12; p <= ((a + 640u * 480u * 4u - 1) >> 12) && p < 0x800; p++) g += dc_vram32_gen[p] * (p | 1);
    return g;
}
static void note_render_target(void) {
    uint32_t a = dc_pvr_reg(0x060) & 0x7FFFFFu;
    int k = 0;
    while (k < 3 && rtarget[k].addr != a) k++;
    memmove(rtarget + 1, rtarget, (size_t)k * sizeof *rtarget);
    rtarget[0].addr = a; rtarget[0].gen = fb_gen(a); rtarget[0].used = 1;
}
static int shows_render(void) {
    uint32_t d = dc_pvr_reg(0x050) & 0x7FFFFFu;
    for (int k = 0; k < 4; k++) if (rtarget[k].used && rtarget[k].addr == d) return rtarget[k].gen == fb_gen(d);
    return 0;
}

/* where the time goes (Show FPS, --stats): rendering, presenting (with vsync: waiting for the display),
 * pacing sleeps; the sound chip in dc_prof_aica; the rest is the game's own code */
static Uint64 prof_render, prof_present, prof_idle;
/* the render thread (rt_*, below present_window) */
static int rt_on;
static volatile uint32_t rt_handle;                         /* Switch: the render thread's kernel handle (profiler) */
static void rt_wait_idle(void);
static void rt_call(void (*fn)(void *), void *arg);
static void rt_render(int scale);
static void job_capture(void *u) { (void)u; if (!dc_pvr_gl_capture_to_vram(cfg.scale)) { dc_pvr_sw_render(); dc_pvr_gl_capture(cfg.scale); } }
static void job_sw_render(void *u);
static uint64_t prof_clock(void) { return SDL_GetPerformanceCounter(); }
static void on_render(void) {
    /* clip 0..0 (Soulcalibur's VS screen while the stage loads): nothing is drawn, the picture stays */
    if (dc_pvr_clip_empty()) { renders++; last_render_frame = (unsigned long)vblanks; return; }
    /* render to texture (FB_W_SOF1 bit 24): the frame goes into VRAM as a texture the game draws later
     * (Tennis 2K2's crossfades), not to the screen; the software renderer writes it there */
    if ((dc_pvr_reg(0x060) >> 24) & 1) {
        if (rt_on) rt_call(job_capture, NULL);              /* VRAM is read back at once: the game waits */
        else job_capture(NULL);
        return;
    }
    static long skip = -1;                                 /* debugging: DC_RENDER_EVERY=n draws every nth frame */
    if (skip < 0) { const char *e = getenv("DC_RENDER_EVERY"); skip = e ? atol(e) : 1; if (skip < 1) skip = 1; }
    static long from = -2;                                 /* DC_RENDER_FROM=field: draw every frame from then on */
    if (from == -2) { const char *e = getenv("DC_RENDER_FROM"); from = e ? atol(e) : -1; }
    if (skip > 1 && (from < 0 || (long)presented < from) && renders % (unsigned long)skip) { renders++; last_render_frame = (unsigned long)vblanks; return; }
    if (frame_skip() > 1 && renders % (unsigned long)frame_skip()) { renders++; last_render_frame = (unsigned long)vblanks; return; }
    if (dc_front_fast && renders % 8) { renders++; last_render_frame = (unsigned long)vblanks; return; }   /* fast-forward: draw every 8th frame */
    if (osd_half && dc_front_fps60 < 0 && (unsigned long)vblanks - last_drawn_vblank < 2) { renders++; last_render_frame = (unsigned long)vblanks; return; }
    last_drawn_vblank = (unsigned long)vblanks;
    static int sw = -1;                                    /* DC_SW_RENDER=1: software renderer (diagnosing GPU issues) */
    if (sw < 0) { const char *e = getenv("DC_SW_RENDER"); sw = e && *e == '1'; }
    if (sw) {
        if (rt_on) rt_call(job_sw_render, NULL); else job_sw_render(NULL);
    } else if (rt_on) {
        rt_render(cfg.scale);                               /* drawn and presented on the render thread */
        note_render_target();
        renders++; drawn++; last_render_frame = (unsigned long)vblanks;
        return;
    } else { Uint64 t = SDL_GetPerformanceCounter(); dc_pvr_gl_render(cfg.scale); prof_render += SDL_GetPerformanceCounter() - t; }
    note_render_target();
    need_present = 1;
    renders++; drawn++; last_render_frame = (unsigned long)vblanks;
}

/* ------------------------------------------------------------------ input */
static int pause_held, pause_req;   /* the pause button down this poll; a pause asked for (P / Pause, the button) */
static uint8_t axis8(Sint16 v) { int x = (v + 32768) >> 8; return (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x); }
static void read_pad(SDL_GameController *pad, uint16_t *b, int *jx, int *jy, int *lt, int *rt) {
    Sint16 ax = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX), ay = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
    if (ax < -4000 || ax > 4000) *jx = axis8(ax);
    if (ay < -4000 || ay > 4000) *jy = axis8(ay);
    int tl = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT), tr = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    if (tl / 128 > *lt) *lt = tl / 128 > 255 ? 255 : tl / 128;
    if (tr / 128 > *rt) *rt = tr / 128 > 255 ? 255 : tr / 128;
    static const struct { SDL_GameControllerButton s; uint16_t d; } map[] = {
        { SDL_CONTROLLER_BUTTON_A, DC_BTN_A }, { SDL_CONTROLLER_BUTTON_B, DC_BTN_B }, { SDL_CONTROLLER_BUTTON_X, DC_BTN_X },
        { SDL_CONTROLLER_BUTTON_Y, DC_BTN_Y }, { SDL_CONTROLLER_BUTTON_START, DC_BTN_START },
        { SDL_CONTROLLER_BUTTON_DPAD_UP, DC_BTN_UP }, { SDL_CONTROLLER_BUTTON_DPAD_DOWN, DC_BTN_DOWN },
        { SDL_CONTROLLER_BUTTON_DPAD_LEFT, DC_BTN_LEFT }, { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, DC_BTN_RIGHT } };
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++) if (SDL_GameControllerGetButton(pad, map[i].s)) *b |= map[i].d;
    /* View / Select / -: Start too (the Dreamcast pad has one), or the pause button (cfg.pause_button) */
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_BACK)) { if (cfg.pause_button) pause_held = 1; else *b |= DC_BTN_START; }
}
static void read_input(void) {
    static int pause_was;
    pause_held = 0;
    for (int p = 1; p < 4; p++) {                          /* ports B-D: gamepads 2-4 */
        uint16_t pb = 0; int px = 128, py = 128, pl = 0, pr = 0;
        if (pads[p]) read_pad(pads[p], &pb, &px, &py, &pl, &pr);
        dc_pad[p].buttons = pb; dc_pad[p].jx = (uint8_t)px; dc_pad[p].jy = (uint8_t)py; dc_pad[p].lt = (uint8_t)pl; dc_pad[p].rt = (uint8_t)pr;
    }
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    uint16_t b = 0; int jx = 128, jy = 128, lt = 0, rt = 0;
    int dp = !cfg.arrows_stick_only;                       /* arrows also press the d-pad */
    if (k[SDL_SCANCODE_UP]) { if (dp) b |= DC_BTN_UP; jy = 0; }
    if (k[SDL_SCANCODE_DOWN]) { if (dp) b |= DC_BTN_DOWN; jy = 255; }
    if (k[SDL_SCANCODE_LEFT]) { if (dp) b |= DC_BTN_LEFT; jx = 0; }
    if (k[SDL_SCANCODE_RIGHT]) { if (dp) b |= DC_BTN_RIGHT; jx = 255; }
    if (k[SDL_SCANCODE_I]) b |= DC_BTN_UP;
    if (k[SDL_SCANCODE_K]) b |= DC_BTN_DOWN;
    if (k[SDL_SCANCODE_J]) b |= DC_BTN_LEFT;
    if (k[SDL_SCANCODE_L]) b |= DC_BTN_RIGHT;
    if (k[SDL_SCANCODE_Z] || k[SDL_SCANCODE_SPACE]) b |= DC_BTN_A;
    if (k[SDL_SCANCODE_X] || k[SDL_SCANCODE_LCTRL]) b |= DC_BTN_B;
    if (k[SDL_SCANCODE_C] || k[SDL_SCANCODE_LSHIFT]) b |= DC_BTN_X;
    if (k[SDL_SCANCODE_V] || k[SDL_SCANCODE_LALT]) b |= DC_BTN_Y;
    if (k[SDL_SCANCODE_RETURN]) b |= DC_BTN_START;
    if (k[SDL_SCANCODE_A]) lt = 255;
    if (k[SDL_SCANCODE_S]) rt = 255;
    if (pads[0]) read_pad(pads[0], &b, &jx, &jy, &lt, &rt);
#ifndef __SWITCH__
    else read_raw(&b, &jx, &jy, &lt, &rt);
    {   /* first input from any pad, logged once: shows whether the controller reaches the game */
        static int seen;
        if (!seen && (pads[0] || raw) && (b & ~0u) ) { seen = 1; fprintf(stderr, "input: first button press from the controller on port A\n"); }
    }
#endif
#ifdef __SWITCH__
    /* Joy-Cons / Pro Controller as SDL joystick 0, buttons in libnx order. Mapped by position:
     * B (bottom) = A, A (right) = B, Y (left) = X, X (top) = Y; ZL/ZR = triggers; + = Start;
     * + and - together quit. */
    if (joy) {
        enum { NA, NB, NX, NY, NLS, NRS, NL, NR, NZL, NZR, NPLUS, NMINUS, NLEFT, NUP, NRIGHT, NDOWN };
        #define JB(i) SDL_JoystickGetButton(joy, i)
        if (JB(NB)) b |= DC_BTN_A; if (JB(NA)) b |= DC_BTN_B; if (JB(NY)) b |= DC_BTN_X; if (JB(NX)) b |= DC_BTN_Y;
        if (JB(NPLUS)) b |= DC_BTN_START;
        if (cfg.pause_button && JB(NMINUS) && !JB(NPLUS)) pause_held = 1;
        if (JB(NUP)) b |= DC_BTN_UP; if (JB(NDOWN)) b |= DC_BTN_DOWN; if (JB(NLEFT)) b |= DC_BTN_LEFT; if (JB(NRIGHT)) b |= DC_BTN_RIGHT;
        if (JB(NZL)) lt = 255; if (JB(NZR)) rt = 255;
        Sint16 ax = SDL_JoystickGetAxis(joy, 0), ay = SDL_JoystickGetAxis(joy, 1);
        if (ax < -4000 || ax > 4000) jx = axis8(ax);
        if (ay < -4000 || ay > 4000) jy = axis8(ay);
        if (JB(NPLUS) && JB(NMINUS)) { dc_front_close(); exit(0); }
        #undef JB
    }
#endif
    if (pause_held && !pause_was) pause_req = 1;
    pause_was = pause_held;
    /* only the focused window plays: a controller can be read by every program at once, so with two
     * ports open both would follow it (the pads are polled with background events allowed, so a
     * console window that kept focus doesn't make the pad look dead; one click on the game fixes that) */
#ifndef __SWITCH__
    {
        static int bg = -1;
        if (bg < 0) { const char *e = getenv("DC_BACKGROUND_INPUT"); bg = cfg.background_input || (e && *e && *e != '0'); }
        if (!bg && win && !(SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS)) {
            b = 0; jx = jy = 128; lt = rt = 0;
            for (int p = 1; p < 4; p++) { dc_pad[p].buttons = 0; dc_pad[p].jx = dc_pad[p].jy = 128; dc_pad[p].lt = dc_pad[p].rt = 0; }
        }
    }
#endif
    /* debugging: DC_PRESS="[P/]field:buttons[:fields],..." holds Dreamcast buttons (DC_BTN_* mask, hex)
     * from that presented field on, for `fields` fields (default 10), on port P (A-D, default A; a port
     * named here counts as connected); extra bits 16 L trigger, 17 R trigger, 18 stick left, 19 stick right,
     * 20 stick up, 21 stick down */
    {
        static const char *press = (const char *)1;
        if (press == (const char *)1) {
            press = getenv("DC_PRESS");
            for (const char *q = press; q && *q; q++)
                if (*q >= 'A' && *q <= 'D' && q[1] == '/') dc_pad[*q - 'A'].connected = 1;
        }
        for (const char *q = press; q && *q; ) {
            char *e; int port = 0;
            if (*q >= 'A' && *q <= 'D' && q[1] == '/') { port = *q - 'A'; q += 2; }
            unsigned long f = strtoul(q, &e, 0), m = 0, d = 10; q = e;
            if (*q == ':') { m = strtoul(q + 1, &e, 16); q = e; }
            if (*q == ':') { d = strtoul(q + 1, &e, 0); q = e; }
            if (presented >= f && presented < f + d) {
                if (port == 0) {
                    b |= (uint16_t)m;
                    if (m & (1ul << 16)) lt = 255;
                    if (m & (1ul << 17)) rt = 255;
                    if (m & (1ul << 18)) jx = 0;
                    if (m & (1ul << 19)) jx = 255;
                    if (m & (1ul << 20)) jy = 0;
                    if (m & (1ul << 21)) jy = 255;
                } else {
                    dc_pad_state *ps = &dc_pad[port];
                    ps->buttons |= (uint16_t)m;
                    if (m & (1ul << 16)) ps->lt = 255;
                    if (m & (1ul << 17)) ps->rt = 255;
                    if (m & (1ul << 18)) ps->jx = 0;
                    if (m & (1ul << 19)) ps->jx = 255;
                    if (m & (1ul << 20)) ps->jy = 0;
                    if (m & (1ul << 21)) ps->jy = 255;
                }
            }
            while (*q && *q != ',') q++;
            if (*q == ',') q++;
        }
    }
    dc_pad[0].buttons = b; dc_pad[0].jx = (uint8_t)jx; dc_pad[0].jy = (uint8_t)jy; dc_pad[0].lt = (uint8_t)lt; dc_pad[0].rt = (uint8_t)rt;
}

static Uint64 start_count; static uint64_t start_cycles;
static void print_stats(void) {
    double wall = (double)(SDL_GetPerformanceCounter() - start_count) / (double)SDL_GetPerformanceFrequency();
    double emu = dc_current_ctx ? (double)(dc_current_ctx->cycles - start_cycles) / 200e6 : 0;
    double f = (double)SDL_GetPerformanceFrequency(), draw = (double)prof_render / f, pres = (double)prof_present / f;
    fprintf(stderr, "dc-core: %lu fields, %lu renders, %.1f s emulated in %.1f s (%.0f%% speed); drawing %.1f s, presenting %.1f s%s\n",
            presented, renders, emu, wall, wall > 0 ? emu / wall * 100 : 0, draw, pres, rt_on ? " (on the render thread)" : "");
}
/* debugging: DC_SHOTS_WINDOW=1 makes screenshots show the window as presented, not the rendered picture */
static uint8_t win_rgb[1920 * 1080 * 3 * 4]; static int win_rgb_w, win_rgb_h;
static int shot_window(void) { static int v = -1; if (v < 0) { const char *e = getenv("DC_SHOTS_WINDOW"); v = e && *e == '1'; } return v; }
static void toast(const char *msg);
static void job_load_rgb(void *u) { struct { uint8_t *p; int w, h; } *a = u; dc_pvr_gl_load_rgb(a->p, a->w, a->h); }
static void job_read(void *u) { struct { uint8_t *p; int *w, *h; } *a = u; dc_pvr_gl_read(a->p, a->w, a->h); }
static int screenshot_to(const char *path) {
    int w = 0, h = 0;
    static uint8_t *rgb; rgb = realloc(rgb, 1920 * 4 * 1440 * 4 * 3);
    if (shot_window() && win_rgb_w) { w = win_rgb_w; h = win_rgb_h; size_t n = (size_t)w * (size_t)h * 3; memcpy(rgb, win_rgb, n < sizeof win_rgb ? n : sizeof win_rgb); }
    else if (rt_on) { struct { uint8_t *p; int *w, *h; } a = { rgb, &w, &h }; rt_call(job_read, &a); }
    else dc_pvr_gl_read(rgb, &w, &h);
    if (w <= 0) return 0;
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(rgb, w, h, 24, w * 3, SDL_PIXELFORMAT_RGB24);
    int ok = s && SDL_SaveBMP(s, path) == 0;
    if (s) SDL_FreeSurface(s);
    fprintf(stderr, ok ? "saved %s\n" : "could not save %s\n", path);
    return ok;
}
/* F12 (or SHOT on the controls bar): the picture as rendered, to a "screenshots" folder next to the
 * game, named <game>_<date>_<time>_<n>.bmp. Held (or BURST on the bar), one every 100 ms. */
static int burst_on;                                       /* BURST on the controls bar */
static Uint32 shot_held_since;                             /* F12 held since (0: not held) */
static void screenshot(void) {
    static char dir[1024];
    if (!dir[0]) {
        char *base = SDL_GetBasePath();
        snprintf(dir, sizeof dir, "%sscreenshots", base ? base : "");
        if (base) SDL_free(base);
#ifdef _WIN32
        CreateDirectoryA(dir, NULL);
#else
        mkdir(dir, 0755);
#endif
    }
    static int n; time_t t = time(NULL); struct tm *tm = localtime(&t);
    char path[1200];
    snprintf(path, sizeof path, "%s/%s_%04d%02d%02d_%02d%02d%02d_%03d.bmp", dir, cfg.save_name ? cfg.save_name : "dc-core",
             tm ? tm->tm_year + 1900 : 0, tm ? tm->tm_mon + 1 : 0, tm ? tm->tm_mday : 0, tm ? tm->tm_hour : 0, tm ? tm->tm_min : 0, tm ? tm->tm_sec : 0, n++ % 1000);
    toast(screenshot_to(path) ? (burst_on || shot_held_since ? "BURST CAPTURE" : "SCREENSHOT SAVED") : "SCREENSHOT FAILED");
}
static void set_speed(int i) {
    if (i < 0) i = 0;
    if (i >= SPEED_N) i = SPEED_N - 1;
    speed_i = i;
    char m[32]; snprintf(m, sizeof m, "SPEED %gX", (double)game_speed()); toast(m);
}
static int bar_click(int x, int y);

int dc_front_fps60 = -1;
int dc_front_refresh_ok;
#ifdef __SWITCH__
/* libnx threads on the Switch (dccore_nxthread.c): an SDL thread crashed in SDL's clean-up when it ended */
void *dc_nx_thread_start(int (*fn)(void *), void *arg, int core);
void *dc_nx_thread_start_prio(int (*fn)(void *), void *arg, int core, int prio);
void dc_nx_thread_join(void *t);
void dc_nx_set_core(int core);
void dc_nx_clear_tls(void);
uint32_t dc_nx_cur_handle(void);
int dc_nx_egl_grab(void);
int dc_nx_egl_current(int on);
void dc_nx_egl_swap(void);
void dc_nx_exit_thread(void);
uint32_t dc_nx_thread_handle(void *t);
static void *aica_thr;
/* the render thread's GL context: EGL straight on the Switch (see dccore_nxthread.c), SDL elsewhere */
#define RT_CURRENT(on) dc_nx_egl_current(on)
#else
static SDL_Thread *aica_thr;                               /* the sound chip's thread (below) */
#define RT_CURRENT(on) (SDL_GL_MakeCurrent(win, (on) ? glc : NULL) == 0)
#endif
static Uint64 prof_wait;                                    /* the game waiting for it */
static int osd_open, osd_req;                              /* the on-screen menu (dccore_osd.inc.c) */
static void events(void) {
    SDL_Event e;
#ifndef __SWITCH__
    /* the mouse pointer hides after 3 s without moving over the window and comes back when it moves */
    static Uint32 moved; static int shown = 1;
    if (!moved) moved = SDL_GetTicks() | 1;
#endif
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) { dc_front_close(); exit(0); }
#ifndef __SWITCH__
        if (e.type == SDL_MOUSEMOTION) { moved = SDL_GetTicks() | 1; if (!shown) { SDL_ShowCursor(SDL_ENABLE); shown = 1; } }
#endif
        if (e.type == SDL_WINDOWEVENT) need_present = 1;
        if (e.type == SDL_KEYDOWN && !e.key.repeat) {
            if (e.key.keysym.sym == SDLK_ESCAPE) { if (osd_open) osd_req = 1; else { dc_front_close(); exit(0); } }
            if (e.key.keysym.sym == SDLK_F1) osd_req = 1;   /* the on-screen menu */
            if ((e.key.keysym.sym == SDLK_p || e.key.keysym.sym == SDLK_PAUSE) && !e.key.repeat) pause_req = 1;
            if (e.key.keysym.sym == SDLK_F11) {
                cfg.fullscreen = !cfg.fullscreen;
                SDL_SetWindowFullscreen(win, cfg.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
            }
            if (e.key.keysym.sym == SDLK_F12) { screenshot(); shot_held_since = SDL_GetTicks() | 1; }
            if (e.key.keysym.sym == SDLK_F2) { extern int front_state_key(int load); front_state_key(0); }   /* save state */
            if (e.key.keysym.sym == SDLK_F4) { extern int front_state_key(int load); front_state_key(1); }   /* load state */
            if (e.key.keysym.sym == SDLK_F5) set_speed(speed_i - 1);
            if (e.key.keysym.sym == SDLK_F6) set_speed(speed_i + 1);
            if (e.key.keysym.sym == SDLK_F7) set_speed(SPEED_1X);
            if (e.key.keysym.sym == SDLK_F9) { extern int osd_bar; osd_bar = !osd_bar; need_present = 1; }
        }
        if (e.type == SDL_KEYUP && e.key.keysym.sym == SDLK_F12) shot_held_since = 0;
        if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) bar_click(e.button.x, e.button.y);
#ifndef __SWITCH__
        if (e.type == SDL_JOYDEVICEADDED) {
            fprintf(stderr, "joystick added: %s%s\n", SDL_JoystickNameForIndex(e.jdevice.which),
                    SDL_IsGameController(e.jdevice.which) ? " (gamepad)" : " (not a known gamepad: opened as a generic one)");
            if (!SDL_IsGameController(e.jdevice.which)) raw_open(e.jdevice.which);
        }
        if (e.type == SDL_JOYDEVICEREMOVED) raw_close((SDL_JoystickID)e.jdevice.which);
#endif
        if (e.type == SDL_CONTROLLERBUTTONDOWN) {          /* the first presses, for diagnosing pads */
            static int logged;
            if (logged++ < 40) {
                int port = -1;
                for (int k = 0; k < 4; k++) if (pads[k] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[k])) == e.cbutton.which) port = k;
                fprintf(stderr, "pad button %s (port %c)\n", SDL_GameControllerGetStringForButton((SDL_GameControllerButton)e.cbutton.button), port >= 0 ? 'A' + port : '?');
            }
        }
        if (e.type == SDL_CONTROLLERDEVICEADDED) pad_open(e.cdevice.which);                 /* device index */
        if (e.type == SDL_CONTROLLERDEVICEREMOVED) pad_close((SDL_JoystickID)e.cdevice.which); /* instance id */
    }
#ifndef __SWITCH__
    if (shown && SDL_GetTicks() - moved > 3000) { SDL_ShowCursor(SDL_DISABLE); shown = 0; }
#endif
    read_input();
}

/* debugging: DC_WAV=file.wav records everything the AICA outputs (44.1 kHz stereo) */
static FILE *wav; static uint32_t wav_frames;
static void wav_close(void) {
    if (!wav) return;
    uint32_t data = wav_frames * 4, riff = 36 + data;
    fseek(wav, 4, SEEK_SET); fwrite(&riff, 4, 1, wav);
    fseek(wav, 40, SEEK_SET); fwrite(&data, 4, 1, wav);
    fclose(wav); wav = NULL;
}
static FILE *wav_file(void) {
    static int tried;
    if (tried) return wav;
    tried = 1;
    const char *p = getenv("DC_WAV");
    if (!p || !(wav = fopen(p, "wb"))) return NULL;
    static const uint8_t hdr[44] = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ', 16,0,0,0, 1,0, 2,0,
                                     0x44,0xAC,0,0, 0x10,0xB1,0x02,0, 4,0, 16,0, 'd','a','t','a', 0,0,0,0 };
    fwrite(hdr, 1, 44, wav);
    atexit(wav_close);
    return wav;
}

int dc_front_fast;

static void osd_overlays(int ww, int wh);
static int aica_audio_read(int16_t *dst, int frames);
static void rt_present(void);
static void present_gl(void);
static void present_window(void) {
    if (rt_on) {                                            /* on the render thread, when it is free */
        need_present = 0;
        /* while the game draws, each frame is presented with its drawing (Show FPS and toasts with it): a
         * present of its own between two frames waits for a vsync of the thread's, which held a 60 fps
         * game to 30. Only when no frame came for a few vblanks (menu, pause, a still picture). */
        if (!osd_open && (unsigned long)vblanks - last_render_frame < 4) return;
        rt_present(); return;
    }
    present_gl();
}
static void present_gl(void) {
    Uint64 pt = SDL_GetPerformanceCounter();
    int dw, dh; SDL_GL_GetDrawableSize(win, &dw, &dh);
    /* the window's shape, also in 4:3: the widescreen target uses it (the next frame fills the window),
     * and a port can keep its culling widened to it in 4:3 too, so switching to widescreen in the menu
     * has the scenery at the sides at once */
    if (dh > 0) dc_pvr_gl_aspect = (float)dw / (float)dh;
    dc_pvr_gl_present(dw, dh, 0);
    osd_overlays(dw, dh);
    if (shot_window() && dw * dh <= 1920 * 1080 * 4) { dc_pvr_gl_read_window(win_rgb, dw, dh); win_rgb_w = dw; win_rgb_h = dh; }
#ifdef __SWITCH__
    if (rt_on) dc_nx_egl_swap(); else
#endif
    SDL_GL_SwapWindow(win);
    need_present = 0;
    prof_present += SDL_GetPerformanceCounter() - pt;
}

/* ------------------------------------------------------------------ render thread
 * The frame is drawn and presented on its own thread (and core) while the game goes on with the next
 * one, as the PowerVR draws beside the SH-4 on the console. At STARTRENDER the game's thread waits for
 * the previous frame to be done, takes what the drawing reads of the console (dc_pvr_gl_prepare) and
 * hands it over. The thread owns the OpenGL context; anything else that draws or reads back (render to
 * a texture, the software renderer, CPU-drawn pictures, screenshots) runs there with the game waiting
 * (rt_call). Presenting without a new frame (menu, toasts, Show FPS) is asked for and done when the
 * thread is free. Default on the Switch; DC_RENDER_THREAD=1 / 0 elsewhere / to turn it off. */
static SDL_Thread *rt_thr;
static SDL_mutex *rt_mx; static SDL_cond *rt_cv, *rt_done_cv;
static int rt_job, rt_busy, rt_quit, rt_want_present, rt_scale;   /* under rt_mx; job: 1 render, 2 call */
static void (*rt_fn)(void *); static void *rt_arg;
static int rt_main(void *u) {
    (void)u;
#ifdef __SWITCH__
    dc_nx_clear_tls();                                      /* before any SDL call: see there */
    rt_handle = dc_nx_cur_handle();
    dc_nx_set_core(2);                                      /* game on 0, sound on 1 */
#endif
    RT_CURRENT(1);
    SDL_LockMutex(rt_mx);
    for (;;) {
        while (!rt_job && !rt_want_present && !rt_quit) SDL_CondWait(rt_cv, rt_mx);
        if (rt_quit) break;
        int job = rt_job; rt_busy = 1;
        if (job == 1 || !job) rt_want_present = 0;
        SDL_UnlockMutex(rt_mx);
        if (job == 1) {
            Uint64 t = SDL_GetPerformanceCounter();
            dc_pvr_gl_render(rt_scale);
            prof_render += SDL_GetPerformanceCounter() - t;
            /* drawn: the frame taken at prepare is free, and the game may hand over the next one while this
             * one waits for the screen (the swap was a quarter of the game thread's time, spent waiting) */
            SDL_LockMutex(rt_mx); rt_job = 0; SDL_CondBroadcast(rt_done_cv); SDL_UnlockMutex(rt_mx);
            present_gl();
        } else if (job == 2) rt_fn(rt_arg);
        else present_gl();
        SDL_LockMutex(rt_mx);
        if (job == 2) rt_job = 0;
        rt_busy = 0;
        SDL_CondBroadcast(rt_done_cv);
    }
    SDL_UnlockMutex(rt_mx);
    RT_CURRENT(0);
#ifdef __SWITCH__
    dc_nx_exit_thread();                                    /* SDL's thread clean-up crashes on the Switch */
#endif
    return 0;
}
static void rt_wait_idle(void) {
    if (!rt_on) return;
    SDL_LockMutex(rt_mx);
    while (rt_job || rt_busy) SDL_CondWait(rt_done_cv, rt_mx);
    SDL_UnlockMutex(rt_mx);
}
static void rt_wait_drawn(void) {                          /* no frame waiting to be drawn or being drawn */
    SDL_LockMutex(rt_mx);
    while (rt_job) SDL_CondWait(rt_done_cv, rt_mx);
    SDL_UnlockMutex(rt_mx);
}
static void rt_post(int job) {                             /* no job waiting (rt_wait_idle / rt_wait_drawn) */
    SDL_LockMutex(rt_mx); rt_job = job; SDL_CondSignal(rt_cv); SDL_UnlockMutex(rt_mx);
}
static void rt_call(void (*fn)(void *), void *arg) {
    rt_wait_idle();
    rt_fn = fn; rt_arg = arg;
    rt_post(2);
    rt_wait_idle();
}
static void job_render_sync(void *u) { (void)u; Uint64 t = SDL_GetPerformanceCounter(); dc_pvr_gl_render(rt_scale); prof_render += SDL_GetPerformanceCounter() - t; present_gl(); }
static void rt_render(int scale) {
    rt_wait_drawn();                                        /* the last frame drawn (maybe not yet on screen): its snapshot is free */
    rt_scale = scale;
    if (dc_pvr_gl_prepare()) rt_post(1);                    /* the game goes on meanwhile */
    else rt_call(job_render_sync, NULL);                    /* too much to take: drawn reading the console, the game waits */
}
static void rt_present(void) {
    SDL_LockMutex(rt_mx);
    rt_want_present = 1;
    SDL_CondSignal(rt_cv);
    SDL_UnlockMutex(rt_mx);
}
static void rt_start(void) {
    const char *e = getenv("DC_RENDER_THREAD");
#ifdef __SWITCH__
    int on = !(e && *e == '0');
#else
    int on = e && *e == '1';
#endif
    if (!on || rt_thr) return;
    if (!rt_mx) { rt_mx = SDL_CreateMutex(); rt_cv = SDL_CreateCond(); rt_done_cv = SDL_CreateCond(); }
    if (!rt_mx || !rt_cv || !rt_done_cv) return;
    rt_job = rt_busy = rt_quit = rt_want_present = 0;
#ifdef __SWITCH__
    if (!dc_nx_egl_grab()) { fprintf(stderr, "render thread: no EGL context to hand over\n"); return; }
#endif
    RT_CURRENT(0);                                          /* the context moves to the thread */
    /* SDL keeps the current GL context in thread-local storage (pthread keys); 2 MB of stack for Mesa */
    rt_thr = SDL_CreateThreadWithStackSize(rt_main, "render", 0x200000, NULL);
    if (!rt_thr) { RT_CURRENT(1); return; }
    rt_on = 1;
    fprintf(stderr, "drawing on its own thread\n");
}
static void rt_stop(void) {
    if (!rt_thr) return;
    rt_wait_idle();
    SDL_LockMutex(rt_mx); rt_quit = 1; SDL_CondBroadcast(rt_cv); SDL_UnlockMutex(rt_mx);
    SDL_WaitThread(rt_thr, NULL);
    rt_thr = NULL; rt_on = 0;
    RT_CURRENT(1);
}
static uint8_t sw_rgb[1280 * 1024 * 3];
static void job_sw_render(void *u) {
    (void)u; int w, h;
    dc_pvr_sw_render();
    if (dc_pvr_display(sw_rgb, 1280, 1024, &w, &h) == 0) dc_pvr_gl_load_rgb(sw_rgb, w, h);
}

/* the game pauses and goes quiet while its window is not focused (another window clicked, minimised,
 * alt-tabbed out of fullscreen), and goes on where it was when the window gets focus back. Only after
 * the window has had focus once, so a console window that keeps focus at start doesn't hold the game.
 * Off with run_in_background / background_input / DC_RUN_IN_BACKGROUND=1, and for scripted and
 * unpaced runs (--frames, --no-pacing, DC_PRESS, DC_SHOTS), which have no one to focus the window. */
/* Sound to the device: the game's thread puts samples into this ring and SDL's audio thread takes them
 * in its callback. Neither waits for the other: on the Switch, SDL_QueueAudio / SDL_GetQueuedAudioSize
 * took the device's lock, which SDL's audio thread holds while it feeds the system's buffers, and the
 * game's thread waited there for up to 40% of each frame. One writer, one reader, two indices. */
#define OUT_RING 16384                                      /* stereo frames, 0.37 s */
static int16_t out_ring[OUT_RING * 2];
static SDL_atomic_t out_w, out_r;
static uint32_t out_fill(void) { return (uint32_t)SDL_AtomicGet(&out_w) - (uint32_t)SDL_AtomicGet(&out_r); }
static void out_push(const int16_t *src, int frames) {
    uint32_t w = (uint32_t)SDL_AtomicGet(&out_w), r = (uint32_t)SDL_AtomicGet(&out_r);
    for (int i = 0; i < frames && w - r < OUT_RING; i++, w++) { out_ring[(w % OUT_RING) * 2] = src[i * 2]; out_ring[(w % OUT_RING) * 2 + 1] = src[i * 2 + 1]; }
    SDL_AtomicSet(&out_w, (int)w);
}
static void SDLCALL out_callback(void *u, Uint8 *stream, int len) {
    (void)u;
    int16_t *dst = (int16_t *)stream; int frames = len / 4, n = 0;
    uint32_t r = (uint32_t)SDL_AtomicGet(&out_r), w = (uint32_t)SDL_AtomicGet(&out_w);
    for (; n < frames && r != w; n++, r++) { dst[n * 2] = out_ring[(r % OUT_RING) * 2]; dst[n * 2 + 1] = out_ring[(r % OUT_RING) * 2 + 1]; }
    if (n < frames) memset(dst + n * 2, 0, (size_t)(frames - n) * 4);
    SDL_AtomicSet(&out_r, (int)r);
}
/* forget what's queued (pause, reset): the reader's index, so under the device's lock (rare) */
static void out_clear(void) {
    if (audio) audio_lock(1);
    SDL_AtomicSet(&out_r, SDL_AtomicGet(&out_w));
    if (audio) audio_lock(0);
}

static void pause_while_unfocused(void) {
#ifndef __SWITCH__
    static int off = -1, had_focus;
    if (off < 0) {
        const char *e = getenv("DC_RUN_IN_BACKGROUND"), *b = getenv("DC_BACKGROUND_INPUT");
        off = cfg.run_in_background || cfg.background_input || (e && *e && *e != '0') || (b && *b && *b != '0') ||
              cfg.exit_after || cfg.no_pacing || getenv("DC_PRESS") || getenv("DC_SHOTS");
    }
    if (off || !win || dc_front_fast) return;
    if (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS) { had_focus = 1; return; }
    if (!had_focus) return;
    fprintf(stderr, "dc-core: paused (window not focused)\n");
    if (audio) { audio_pause(1); out_clear(); }
    while (!(SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS)) {
        SDL_WaitEventTimeout(NULL, 100);
        events();                                           /* quit, F11, window moved / uncovered */
        if (need_present) present_window();
    }
    if (audio) audio_pause(0);
    paced = 0;                                              /* pacing starts over: no catching up on the pause */
    fprintf(stderr, "dc-core: resumed\n");
#endif
}

#include "dccore_osd.inc.c"

/* Jump Pack effects to the host's gamepads, once per field: sent when the level changes, refreshed
 * while it lasts (SDL stops a rumble after the duration it was given) */
static void rumble_tick(void) {
    static float last[4]; static int age[4];
    for (int k = 0; k < 4; k++) {
        float lo, hi;
        int on = dc_rumble_level(k, &lo, &hi);
        float m = osd_rumble_scale(k);                     /* the player's strength from the menu */
        lo = lo * m > 1 ? 1 : lo * m; hi = hi * m > 1 ? 1 : hi * m;
        if (m == 0) on = 0;
        float q = on ? lo + hi : 0;
        if (q == last[k] && !(on && ++age[k] >= 20)) continue;
        last[k] = q; age[k] = 0;
        Uint16 a = (Uint16)(lo * 65535.0f), b = (Uint16)(hi * 65535.0f);
#ifdef __SWITCH__
        if (k == 0 && joy) SDL_JoystickRumble(joy, a, b, on ? 500 : 0);
#else
        if (pads[k]) SDL_GameControllerRumble(pads[k], a, b, on ? 500 : 0);
#endif
    }
}

/* ------------------------------------------------------------------ per field */
static void on_vblank(uint64_t f) {
    vblanks = f;
#ifdef __SWITCH__
    if (!(f & 127)) dc_nx_gpu_check();                      /* the system put its own GPU clock back (undocked, woken) */
#endif
    if (!cfg.no_rumble) rumble_tick();
    if (vblanks - last_render_frame > 3 && !shows_render()) {   /* nothing drawn by the PVR: show the framebuffer */
        static uint8_t rgb[1280 * 1024 * 3]; int w, h;
        if (dc_pvr_display(rgb, 1280, 1024, &w, &h) == 0) {
            /* uninitialised VRAM: a framebuffer that is black but for a streak of noise a few lines high
             * across the width (Looney Tunes Space Race between its logo movie and the game) shows black */
            int lit_rows = 0, x0 = w, x1 = -1;
            for (int y = 0; y < h && lit_rows <= 8; y++) {
                int lit = 0;
                for (int x = 0; x < w; x++) { const uint8_t *p = rgb + ((size_t)y * w + x) * 3; if (p[0] + p[1] + p[2] > 48) { lit = 1; if (x < x0) x0 = x; if (x > x1) x1 = x; } }
                lit_rows += lit;
            }
            if (lit_rows > 0 && lit_rows <= 8 && (x1 - x0) * 4 >= w * 3) memset(rgb, 0, (size_t)w * h * 3);
            if (rt_on) { struct { uint8_t *p; int w, h; } a = { rgb, w, h }; rt_call(job_load_rgb, &a); }
            else dc_pvr_gl_load_rgb(rgb, w, h);
            need_present = 1;
        }
    }
    {   /* burst capture: F12 held for more than 0.3 s, or BURST on the controls bar: a picture every 100 ms */
        static Uint32 last;
        Uint32 now = SDL_GetTicks();
        if ((burst_on || (shot_held_since && now - shot_held_since > 300)) && now - last >= 100) { last = now; screenshot(); }
    }
#ifdef __SWITCH__
    prof_poll();
#endif
    if (toast_live()) need_present = 1;
    if (need_present && (!dc_front_fast || presented % 8 == 0) && presented % (unsigned long)frame_skip() == 0) present_window();   /* only when the picture changed (or the window) */
    presented++;
    { static long dbg = -2; if (dbg == -2) { const char *e = getenv("DC_PVRDBG"); dbg = e ? atol(e) : -1; }
      if (dbg >= 0 && presented == (unsigned long)dbg) dc_pvr_sw_debug = 1; }   /* debugging: describe that frame's polygons (with DC_SW_RENDER=1) */
    if (cfg.stats && presented % 300 == 0) print_stats();
    {   /* debugging: DC_SHOTS="every[:dir]" saves the picture every `every` fields as dir/fNNNNN.bmp;
         * DC_SHOTS_FROM=N starts at field N (with DC_RENDER_FROM, a long run pictures only the part that matters) */
        static long every = -1, from; static char dir[512];
        if (every < 0) {
            const char *e = getenv("DC_SHOTS"), *f = getenv("DC_SHOTS_FROM"); char *end; every = 0;
            if (e) { every = strtol(e, &end, 0); snprintf(dir, sizeof dir, "%s", *end == ':' ? end + 1 : "."); }
            from = f ? atol(f) : 0;
        }
        if (every > 0 && (long)presented >= from && presented % (unsigned long)every == 0) {
            char path[640]; snprintf(path, sizeof path, "%s/f%05lu.bmp", dir, presented); screenshot_to(path);
        }
    }
    if (cfg.exit_after && presented >= cfg.exit_after) {
        if (cfg.shot_path) screenshot_to(cfg.shot_path);
        if (cfg.stats) print_stats();
        dc_front_close(); exit(0);
    }
    { static long reset_at = -2;                           /* DC_RESET_AT=frame: Reset there (tests) */
      if (reset_at == -2) { const char *e = getenv("DC_RESET_AT"); reset_at = e ? atol(e) : -1; }
      if (reset_at >= 0 && (long)presented >= reset_at) { reset_at = -1; front_reset_now(); } }
    { static long save_at = -2, load_at = -2;              /* DC_STATE_SAVE_AT / DC_STATE_LOAD_AT=frame (tests) */
      if (save_at == -2) { const char *e = getenv("DC_STATE_SAVE_AT"); save_at = e ? atol(e) : -1; e = getenv("DC_STATE_LOAD_AT"); load_at = e ? atol(e) : -1; }
      if (save_at >= 0 && (long)presented >= save_at) { save_at = -1; front_state_save(); }
      if (load_at >= 0 && (long)presented >= load_at) { load_at = -1; front_load_now(); } }
    if (wav_file() || audio) {
        static int16_t buf[4096 * 2];
        int n;
        while ((n = aica_audio_read(buf, 4096)) > 0) {
            if (wav) { fwrite(buf, 4, (size_t)n, wav); wav_frames += (uint32_t)n; }
            if (audio && speed_i == SPEED_1X && out_fill() < 44100 / 5) out_push(buf, n);   /* keep < 0.2 s queued */
        }
    }
    events();
    pause_while_unfocused();
    osd_count_frame();
    if (!dc_front_fast) { int by = osd_combo(); if (osd_req || by) osd_run(by ? by - 1 : 0); }
    if (pause_req) pause_run();
    /* pacing: emulated SH-4 time (200 MHz) against the wall clock */
    if (dc_front_fast) { paced = 0; return; }              /* fast-forward: no pacing, resynchronised after */
    if (!cfg.no_pacing && dc_current_ctx) {
        Uint64 freq = SDL_GetPerformanceFrequency(), now = SDL_GetPerformanceCounter(), idle0 = now;
        if (!paced) { t0_count = now; t0_cycles = dc_current_ctx->cycles; paced = 1; return; }
        static int last_speed = SPEED_1X;
        if (speed_i != last_speed) { last_speed = speed_i; t0_count = now; t0_cycles = dc_current_ctx->cycles; return; }   /* a new speed: pacing starts over */
        double emu = (double)(dc_current_ctx->cycles - t0_cycles) / 200e6 / (double)game_speed(), real = (double)(now - t0_count) / (double)freq;
        /* with vsync the swap already waits for the display; sleep only when more than a field ahead
         * (a display faster than 60 Hz), so a 60 Hz display isn't made to skip a refresh now and then */
        double ahead = cfg.vsync ? 0.017 : 0.0;
        /* with sound (and no vsync) the sound card's clock paces the game: the queue is held near 30 ms,
         * so it never runs dry (stutter) or overflows (dropped blocks), whatever the drift between the
         * wall clock, SDL_Delay's granularity and the card. The wall clock only bounds it: never more
         * than 50 ms ahead of real time. DC_PACE=clock: wall clock only (as before). */
        static int by_clock = -1;
        if (by_clock < 0) { const char *e = getenv("DC_PACE"); by_clock = e && !strcmp(e, "clock"); }
        if (speed_i != SPEED_1X) ahead = 0.0;             /* away from 1x the wall clock paces (sound is muted) */
        if (audio && !cfg.vsync && !by_clock && speed_i == SPEED_1X) {
            /* the queue swings between the target and the target plus a field (a field's samples arrive
             * at once) and drains while the next field is emulated; 30 ms above a 512-frame device buffer
             * keeps sound about 50 ms behind the picture instead of 100. DC_AUDIO_MS sets the target. */
            static Uint32 target;
            if (!target) { const char *e = getenv("DC_AUDIO_MS"); int ms = e ? atoi(e) : 30; if (ms < 10) ms = 10; if (ms > 200) ms = 200;
                           target = (Uint32)(44100 * 4 * ms / 1000); }
            while (out_fill() * 4 > target && emu > real - 0.05) {
                SDL_Delay(1);
                now = SDL_GetPerformanceCounter(); real = (double)(now - t0_count) / (double)freq;
            }
            if (real - emu > 0.25 || emu - real > 0.05) { t0_count = now; t0_cycles = dc_current_ctx->cycles; }
        }
        else if (emu > real + ahead) SDL_Delay((Uint32)((emu - real - ahead) * 1000.0));
        else if (real - emu > 0.25) { t0_count = now; t0_cycles = dc_current_ctx->cycles; }   /* too slow: don't try to catch up */
        prof_idle += SDL_GetPerformanceCounter() - idle0;
    }
}

/* ------------------------------------------------------------------ the sound chip on its own thread
 * Its ARM7 and 64 channels run ~44,100 times a second; on the Switch that was about a third of a frame's
 * time on the game's core. The thread runs the chip up to where the game is (aica.h: advance), and each
 * game access to the chip waits for it to catch up there first (enter / leave), so the game sees the same
 * chip it did inline. On by default on the Switch (its own core), DC_AICA_THREAD=1 / 0 elsewhere / to
 * turn it off. */
static SDL_mutex *aica_mx, *disc_mx;
/* Two wake-ups, each only sent when someone waits for it: on the Switch every signal is a system
 * call (libnx's condvarWakeOne doesn't check for waiters) and the game used to send one each time it
 * moved time on -- 37% of the game thread in a profile. The sound thread is woken when the game is a
 * chunk (64 samples) ahead of where it went to sleep, or when the game needs it (enter); it sleeps at
 * most 2 ms, so a wake-up lost between the two threads' unlocked checks only delays it that long. */
static SDL_cond *aica_cv, *aica_done_cv;
static volatile uint64_t aica_target, aica_slept_at;
static SDL_atomic_t aica_sleeping;
static SDL_atomic_t aica_want;                           /* the game is waiting for aica_mx */
static int aica_game_waits;                                 /* under aica_mx */
static volatile int aica_stop;
static const uint64_t aica_chunk = 64ull * 200000000ull / 44100ull;   /* 64 samples a turn, then the game may get in */
static int aica_thread(void *u) {
    (void)u;
    SDL_LockMutex(aica_mx);
    while (!aica_stop) {
        uint64_t t = aica_target, at = dc_aica_clock();
        if (at > t) {
            aica_slept_at = at;
            SDL_AtomicSet(&aica_sleeping, 1);
            if (aica_target <= at && !aica_stop) SDL_CondWaitTimeout(aica_cv, aica_mx, 2);
            SDL_AtomicSet(&aica_sleeping, 0);
            continue;
        }
        dc_aica_advance(t - at > aica_chunk ? at + aica_chunk : t);
        if (aica_game_waits) SDL_CondSignal(aica_done_cv);
        /* the game wants in (aica_async_enter): step aside until it has the lock. Unlocking and
         * locking again at once kept it out -- the Switch's mutex isn't fair, and the game thread sat
         * in svcArbitrateLock for 42% of a profile */
        SDL_UnlockMutex(aica_mx);
        while (SDL_AtomicGet(&aica_want) && !aica_stop) SDL_Delay(0);
        SDL_LockMutex(aica_mx);
    }
    SDL_UnlockMutex(aica_mx);
    return 0;
}
static void aica_async_advance(uint64_t now) {
    if (now <= aica_target) return;
    aica_target = now;
    if (SDL_AtomicGet(&aica_sleeping) && now - aica_slept_at >= aica_chunk) {
        SDL_AtomicSet(&aica_sleeping, 0);                   /* one signal per sleep */
        SDL_CondSignal(aica_cv);
    }
}
static void aica_async_enter(uint64_t now) {
    Uint64 t = SDL_GetPerformanceCounter();
    SDL_AtomicIncRef(&aica_want);
    SDL_LockMutex(aica_mx);
    SDL_AtomicDecRef(&aica_want);
    if (now > aica_target) aica_target = now;
    uint64_t need = now > dc_aica_async.slack ? now - dc_aica_async.slack : 0;   /* see dc_aica_async_t */
    if (dc_aica_clock() <= need) {
        aica_game_waits = 1;
        while (dc_aica_clock() <= need) { SDL_CondSignal(aica_cv); SDL_CondWait(aica_done_cv, aica_mx); }
        aica_game_waits = 0;
    }
    prof_wait += SDL_GetPerformanceCounter() - t;
}
static void aica_async_leave(void) { dc_aica_apply_irq(); SDL_UnlockMutex(aica_mx); }
static void disc_lock(int on) { if (on) SDL_LockMutex(disc_mx); else SDL_UnlockMutex(disc_mx); }
static void aica_thread_start(void) {
    const char *e = getenv("DC_AICA_THREAD");
#ifdef __SWITCH__
    int on = !(e && *e == '0');
#else
    int on = e && *e == '1';
#endif
    if (!on || aica_thr) return;
    if (!aica_mx) { aica_mx = SDL_CreateMutex(); disc_mx = SDL_CreateMutex(); aica_cv = SDL_CreateCond(); aica_done_cv = SDL_CreateCond(); }
    if (!aica_mx || !disc_mx || !aica_cv || !aica_done_cv) return;
    aica_stop = 0; aica_target = 0; aica_slept_at = 0; aica_game_waits = 0;   /* again after a reset */
    SDL_AtomicSet(&aica_sleeping, 0); SDL_AtomicSet(&aica_want, 0);
    dc_disc_lock = disc_lock;
    dc_aica_async.advance = aica_async_advance; dc_aica_async.enter = aica_async_enter; dc_aica_async.leave = aica_async_leave;
    { const char *sl = getenv("DC_AICA_SLACK");            /* samples; 0: exact */
      dc_aica_async.slack = (sl ? (uint64_t)atoi(sl) : 192ull) * 200000000ull / 44100ull; }
#ifdef __SWITCH__
    aica_thr = dc_nx_thread_start(aica_thread, NULL, 1);   /* core 1; the game's thread is on core 0 */
#else
    aica_thr = SDL_CreateThread(aica_thread, "aica", NULL);
#endif
    if (!aica_thr) { memset(&dc_aica_async, 0, sizeof dc_aica_async); dc_disc_lock = NULL; return; }
    fprintf(stderr, "sound chip on its own thread\n");
}
static void aica_thread_stop(void) {
    if (!aica_thr) return;
    SDL_LockMutex(aica_mx); aica_stop = 1; SDL_CondBroadcast(aica_cv); SDL_CondBroadcast(aica_done_cv); SDL_UnlockMutex(aica_mx);
#ifdef __SWITCH__
    dc_nx_thread_join(aica_thr); aica_thr = NULL;
#else
    SDL_WaitThread(aica_thr, NULL); aica_thr = NULL;
#endif
    memset(&dc_aica_async, 0, sizeof dc_aica_async); dc_disc_lock = NULL;
}
/* the chip's sample ring has one writer and one reader and needs no lock (aica.c) */
static int aica_audio_read(int16_t *dst, int frames) { return dc_aica_audio_read(dst, frames); }

static const unsigned char *app_icon;
int dc_front_open_icon(const dc_front_config *c, const unsigned char *icon_rgba) { app_icon = icon_rgba; return dc_front_open(c); }

int dc_front_open(const dc_front_config *c) {
    cfg = *c;
    {   /* started again by Reset (a port without dc_front_run): the window as it was (osd_restart) */
        const char *e = getenv("DC_RESTART_STATE"); int fs, sc, ws, w, h;
        if (e && sscanf(e, "%d,%d,%d,%d,%d", &fs, &sc, &ws, &w, &h) == 5) {
            cfg.fullscreen = fs; if (sc > 0) cfg.scale = sc; cfg.widescreen = ws;
            if (w > 0 && h > 0) { cfg.width = w; cfg.height = h; }
        }
    }
#ifdef _WIN32
    /* messages go to <save_name>.log next to the exe (DC_LOG=console keeps them in the console) */
    { const char *lg = getenv("DC_LOG"); if (!(lg && !strcmp(lg, "console"))) {
        char *base = SDL_GetBasePath(), path[1024];
        snprintf(path, sizeof path, "%s%s.log", base ? base : "", cfg.save_name ? cfg.save_name : "dc-core");
        if (base) SDL_free(base);
        fprintf(stderr, "log: %s\n", path);
        if (freopen(path, "w", stderr)) setvbuf(stderr, NULL, _IONBF, 0);
    } }
#endif
    if (cfg.scale <= 0) cfg.scale = 2;
#ifdef __SWITCH__
    cfg.width = 1280; cfg.height = 720; cfg.fullscreen = 1;   /* the console's screen / TV output */
#endif
    if (!cfg.width) cfg.width = (cfg.widescreen ? 854 : 640) * cfg.scale > 1600 ? 1600 : (cfg.widescreen ? 854 : 640) * cfg.scale;
    if (!cfg.height) cfg.height = cfg.widescreen ? cfg.width * 9 / 16 : cfg.width * 3 / 4;
    dc_pvr_gl_widescreen = cfg.widescreen;
    SDL_SetMainReady();                                    /* ports use a plain main(), not SDL_main */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");   /* pads work while a console window has focus */
    { SDL_version v; SDL_GetVersion(&v); fprintf(stderr, "SDL %d.%d.%d\n", v.major, v.minor, v.patch); }
    { const char *ig = getenv("SDL_GAMECONTROLLER_IGNORE_DEVICES"); if (ig) fprintf(stderr, "note: SDL_GAMECONTROLLER_IGNORE_DEVICES=%s\n", ig); }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER | (cfg.no_audio || DC_NX_AUDIO ? 0 : SDL_INIT_AUDIO)) != 0) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError()); return -1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3); SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    { int fl = 0; const char *e = getenv("DC_GL_DEBUG");
#ifdef __APPLE__
      fl |= SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG;
#endif
      if (e && *e == '1') fl |= SDL_GL_CONTEXT_DEBUG_FLAG;
      if (fl) SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, fl); }
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24); SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    win = SDL_CreateWindow(cfg.title ? cfg.title : "dc-core", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, cfg.width, cfg.height,
                           SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | (cfg.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (!win) { fprintf(stderr, "SDL window: %s\n", SDL_GetError()); return -1; }
#ifndef __SWITCH__
    if (app_icon) {                                       /* the game's VMU icon (window, taskbar, Dock), x4 unsmoothed */
        static uint8_t big[128 * 128 * 4];
        for (int y = 0; y < 128; y++) for (int x = 0; x < 128; x++) memcpy(big + (y * 128 + x) * 4, app_icon + ((y / 4) * 32 + x / 4) * 4, 4);
        SDL_Surface *ic = SDL_CreateRGBSurfaceWithFormatFrom(big, 128, 128, 32, 128 * 4, SDL_PIXELFORMAT_RGBA32);
        if (ic) { SDL_SetWindowIcon(win, ic); SDL_FreeSurface(ic); }
    }
#endif
    glc = SDL_GL_CreateContext(win);
    if (!glc) { fprintf(stderr, "OpenGL 3.3: %s\n", SDL_GetError()); return -1; }
    if (cfg.vsync) {                                       /* adaptive (late frames don't wait) where offered */
        if (SDL_GL_SetSwapInterval(-1) != 0 && SDL_GL_SetSwapInterval(1) != 0)
            { fprintf(stderr, "vsync not available: %s\n", SDL_GetError()); cfg.vsync = 0; }
        else fprintf(stderr, "vsync on (%s)\n", SDL_GL_GetSwapInterval() < 0 ? "adaptive" : "every refresh");
    } else SDL_GL_SetSwapInterval(0);
    if (!dc_pvr_gl_shader_list) {                          /* the shader variants the game has used: built at start-up */
        static char list[1024]; char *dir = SDL_GetPrefPath("dc-core", cfg.save_name ? cfg.save_name : "default");
        if (dir) { snprintf(list, sizeof list, "%sshaders.txt", dir); dc_pvr_gl_shader_list = list; SDL_free(dir); }
    }
    if (dc_pvr_gl_init(getproc)) return -1;
    dc_pvr_sw_install();                                   /* TA collection (shared by the backends) */
    rt_start();
    dc_maple_install();
    if (!cfg.no_vmu) {                                     /* VMU in controller A's first slot */
        char path[1024]; char *dir = NULL;
        if (cfg.vmu_path) snprintf(path, sizeof path, "%s", cfg.vmu_path);
        else if ((dir = SDL_GetPrefPath("dc-core", cfg.save_name ? cfg.save_name : "default")))
            snprintf(path, sizeof path, "%svmu_a1.bin", dir);
        else path[0] = 0;
        SDL_free(dir);
        if (!path[0] || dc_vmu_attach(0, path)) fprintf(stderr, "VMU: cannot use %s (continuing without saves)\n", path[0] ? path : "app data");
        else fprintf(stderr, "VMU: %s\n", path);
    }
    { const char *e = getenv("DC_NO_RUMBLE"); if (e && *e == '1') cfg.no_rumble = 1; }
    for (int k = 0; k < 4; k++) dc_rumble_attach(k, !cfg.no_rumble);   /* a Jump Pack in every controller */
    if (!dc_found_funcs_path) {                            /* functions only the interpreter ran: for the next build */
        static char found[1024]; char *dir = SDL_GetPrefPath("dc-core", cfg.save_name ? cfg.save_name : "default");
        if (dir) { snprintf(found, sizeof found, "%sfound_funcs.txt", dir); dc_found_funcs_path = found; SDL_free(dir); }
    }
    {   /* the display's refresh rate bounds REFRESH RATE: more fields than it shows would be thrown away
         * (and with vsync each would wait for a refresh, slowing the game down) */
        SDL_DisplayMode dm;
        if (SDL_GetCurrentDisplayMode(SDL_GetWindowDisplayIndex(win), &dm) == 0 && dm.refresh_rate > 0) display_hz = dm.refresh_rate;
    }
    osd_load();
    dc_prof_clock = prof_clock;
#ifdef __SWITCH__
    dc_aica_arm_throttle = 1;                              /* the sound chip's ARM: see aica.c */
    dc_aica_arm_cycles = 128;                              /* half the ARM's cycles: the sound thread was the limit */
#endif
    aica_thread_start();
    if (!cfg.no_audio) {
        SDL_AudioSpec want = {0}, have;
        want.freq = 44100; want.format = AUDIO_S16SYS; want.channels = 2; want.callback = out_callback;
#ifdef __SWITCH__
        want.samples = 1024;
#else
        want.samples = 512;                                /* 11.6 ms: little latency on top of the queue */
#endif
#ifdef __SWITCH__
        (void)have;
        audio = dc_nx_audio_open((void (*)(void *, uint8_t *, int))out_callback, NULL) ? 1 : 0;
        if (!audio) fprintf(stderr, "audio: audout didn't start (continuing without sound)\n");
#else
        audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
#endif
        if (audio) audio_pause(0);
        else if (!DC_NX_AUDIO) fprintf(stderr, "audio: %s (continuing without sound)\n", SDL_GetError());
    }
#ifdef __SWITCH__
    if (SDL_NumJoysticks() > 0) joy = SDL_JoystickOpen(0);
#else
    fprintf(stderr, "joysticks: %d\n", SDL_NumJoysticks());
    for (int i = 0; i < SDL_NumJoysticks(); i++)          /* what the system reports, for diagnosing input */
        fprintf(stderr, "joystick %d: %s%s, player %d\n", i, SDL_JoystickNameForIndex(i),
                SDL_IsGameController(i) ? " (gamepad)" : " (not a known gamepad: ignored)", SDL_JoystickGetDevicePlayerIndex(i) + 1);
    for (int i = 0; i < SDL_NumJoysticks(); i++) pad_open(i);
#endif
    dc_hw.on_render = on_render;
    dc_hw.on_vblank = on_vblank;
    start_count = SDL_GetPerformanceCounter(); start_cycles = dc_current_ctx ? dc_current_ctx->cycles : 0;
    return 0;
}

void dc_front_close(void) {
    rt_stop();
    aica_thread_stop();
    wav_close();
    for (int k = 0; k < 4; k++) if (pads[k]) SDL_GameControllerClose(pads[k]);
#ifndef __SWITCH__
    if (raw) { SDL_JoystickClose(raw); raw = NULL; }
#endif
#ifdef __SWITCH__
    if (joy) SDL_JoystickClose(joy);
    joy = NULL;
    set_cpu_clock(0);
    set_gpu_clock(0);
#endif
    /* the on-screen menu (and the focus pause) leave the device paused; on the Switch, closing a paused
     * device crashed SDL's audio thread (Reset / Quit from the menu): empty it and let it run first */
#ifdef __SWITCH__
    prof_stop();                                           /* the profiler's thread, if it ran */
#endif
    if (audio) { out_clear(); audio_pause(0); audio_close(); audio = 0; }
    if (glc) SDL_GL_DeleteContext(glc);
    if (win) SDL_DestroyWindow(win);
    memset(pads, 0, sizeof pads); audio = 0; glc = NULL; win = NULL;
    SDL_Quit();
}

unsigned long dc_front_frames(void) { return presented; }

/* ------------------------------------------------------------------ reset in place (dc_front_run) */
#include "dccore/reset.h"
#include "dccore/coro.h"
void (*dc_front_on_reset)(struct sh4ctx *c);
static jmp_buf reset_jb, load_jb;
static uint32_t run_pc;                                     /* where dc_front_run goes on */
static void *load_buf;                                      /* a checked save state on its way in */
int state_slot = 1;                                         /* 1..9: the menu's slot (dccore_osd.inc.c) */
static void state_path(char *out, size_t n, int slot) {
    snprintf(out, n, "%s", osd_cfg_path[0] ? osd_cfg_path : "osd.cfg");
    char *s1 = strrchr(out, '/'), *s2 = strrchr(out, '\\'), *sl = s1 > s2 ? s1 : s2;
    snprintf(sl ? sl + 1 : out, n - (size_t)(sl ? sl + 1 - out : 0), "state%d.dcs", slot);
}
static void toast_slot(const char *fmt) { static char m[40]; snprintf(m, sizeof m, fmt, state_slot); toast(m); }
/* the save itself, at the next call from recompiled code (dc_state_req) */
static void state_save_here(sh4ctx *c, uint32_t addr) {
    char path[1100]; state_path(path, sizeof path, state_slot);
    if (aica_thr) { SDL_AtomicIncRef(&aica_want); SDL_LockMutex(aica_mx); SDL_AtomicDecRef(&aica_want); }
    int r = dc_state_save(c, addr, path, cfg.title);
    if (aica_thr) SDL_UnlockMutex(aica_mx);
    fprintf(stderr, "dc-core: state %d %s (%s)\n", state_slot, r ? "NOT saved" : "saved", path);
    toast_slot(r ? "STATE %d NOT SAVED" : "STATE %d SAVED");
    need_present = 1;
}
static int front_state_save(void) {
    if (!reset_ready) return 0;
    dc_state_hook = state_save_here; dc_state_req = 1;
    return 1;
}
static int reset_ready;                                     /* dc_front_run took its copy */
void dc_front_run(struct sh4ctx *c, uint32_t entry) {
    /* the copy is taken holding the sound chip's lock: the sound thread touches the chip only under it
     * (stopping the thread instead crashed the Switch: a thread that ends crashes there) */
    if (aica_thr) { SDL_AtomicIncRef(&aica_want); SDL_LockMutex(aica_mx); SDL_AtomicDecRef(&aica_want); }
    reset_ready = dc_reset_snapshot(c) == 0;
    if (aica_thr) SDL_UnlockMutex(aica_mx);
    if (!reset_ready) fprintf(stderr, "dc-core: no memory for the reset copy: Reset starts the program again\n");
    if (DC_SETJMP(reset_jb)) {                              /* front_reset_now: back here, on the main stack */
        if (aica_thr) { SDL_AtomicIncRef(&aica_want); SDL_LockMutex(aica_mx); SDL_AtomicDecRef(&aica_want); }
        dc_reset_restore(c);                                /* under the sound chip's lock, the thread keeps running */
        aica_target = 0; aica_slept_at = 0;                 /* the chip's clock is back at the copy's */
        if (aica_thr) SDL_UnlockMutex(aica_mx);
        if (dc_front_on_reset) dc_front_on_reset(c);
        out_clear();
        paced = 0;
        fprintf(stderr, "dc-core: reset (the game starts again)\n");
    }
    run_pc = entry;
    c->pr = 0;
    if (DC_SETJMP(load_jb)) {                               /* front_load_now: a save state, on the main stack */
        if (aica_thr) { SDL_AtomicIncRef(&aica_want); SDL_LockMutex(aica_mx); SDL_AtomicDecRef(&aica_want); }
        run_pc = dc_state_apply(c, load_buf); load_buf = NULL;
        aica_target = 0; aica_slept_at = 0;
        if (aica_thr) SDL_UnlockMutex(aica_mx);
        if (dc_front_on_reset) dc_front_on_reset(c);
        out_clear();
        paced = 0;
        fprintf(stderr, "dc-core: state %d loaded (going on at %08X)\n", state_slot, run_pc);
        toast_slot("STATE %d LOADED");
    }
    /* the game never returns. After a load it goes on at the call the state was saved at, with none of its
     * callers on the host stack: whenever a function returns here, go on at its return address (pr) */
    for (;;) {
        dc_call(c, run_pc);
        run_pc = c->pr;
        if (!run_pc) { fprintf(stderr, "dc-core: the game returned\n"); exit(1); }
    }
}
/* from the menu (on the game's thread, anywhere in its code): leave it for dc_front_run's jmp_buf */
static int front_load_now(void) {
    if (!reset_ready) return 0;
    char path[1100]; state_path(path, sizeof path, state_slot);
    void *b = dc_state_load(path, cfg.title);
    if (!b) { toast_slot("NO STATE %d"); fprintf(stderr, "dc-core: no state %d for this game (%s)\n", state_slot, path); return 1; }
    load_buf = b;
    if (osd_open) { osd_open = 0; while (!osd_all_released()) osd_frame(); }   /* from the menu: the game doesn't see the press */
    need_present = 1;
    if (audio) audio_pause(0);
    rt_wait_idle();                                         /* the drawing done with the frame it took */
    dc_coro_leave(&load_jb);
    return 1;                                               /* not reached */
}
/* F2 / F4: save / load the menu's slot. A load from here (the event loop, on the game's thread) is the
 * same jump as the menu's */
int front_state_key(int load) { return load ? front_load_now() : front_state_save(); }
static int front_reset_now(void) {
    if (!reset_ready) return 0;
    osd_open = 0;
    while (!osd_all_released()) osd_frame();               /* the game doesn't see the press */
    need_present = 1;
    if (audio) audio_pause(0);
    rt_wait_idle();                                         /* the drawing done with the frame it took */
    dc_coro_leave(&reset_jb);
    return 1;                                               /* not reached */
}

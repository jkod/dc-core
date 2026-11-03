/* dc-core desktop frontend: SDL2 window + OpenGL 3.3, audio, input, real-time pacing.
 * See dccore_front.h. */
#include "dccore_front.h"
#include "dccore/runtime.h"
#include "dccore/hw.h"
#include "dccore/pvr.h"
#include "dccore/aica.h"
#include "dccore/maple.h"
#define SDL_MAIN_HANDLED
#include <SDL.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern sh4ctx *dc_current_ctx;         /* dispatch.c */

static SDL_Window *win;
static SDL_GLContext glc;
static SDL_AudioDeviceID audio;
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

static void on_render(void) {
    /* render to texture (FB_W_SOF1 bit 24): the frame goes into VRAM as a texture the game draws later
     * (Tennis 2K2's crossfades), not to the screen; the software renderer writes it there */
    if ((dc_pvr_reg(0x060) >> 24) & 1) { dc_pvr_sw_render(); dc_pvr_gl_capture(cfg.scale); return; }
    static long skip = -1;                                 /* debugging: DC_RENDER_EVERY=n draws every nth frame */
    if (skip < 0) { const char *e = getenv("DC_RENDER_EVERY"); skip = e ? atol(e) : 1; if (skip < 1) skip = 1; }
    static long from = -2;                                 /* DC_RENDER_FROM=field: draw every frame from then on */
    if (from == -2) { const char *e = getenv("DC_RENDER_FROM"); from = e ? atol(e) : -1; }
    if (skip > 1 && (from < 0 || (long)presented < from) && renders % (unsigned long)skip) { renders++; last_render_frame = (unsigned long)vblanks; return; }
    if (dc_front_fast && renders % 8) { renders++; last_render_frame = (unsigned long)vblanks; return; }   /* fast-forward: draw every 8th frame */
    static int sw = -1;                                    /* DC_SW_RENDER=1: software renderer (diagnosing GPU issues) */
    if (sw < 0) { const char *e = getenv("DC_SW_RENDER"); sw = e && *e == '1'; }
    if (sw) {
        static uint8_t rgb[1280 * 1024 * 3]; int w, h;
        dc_pvr_sw_render();
        if (dc_pvr_display(rgb, 1280, 1024, &w, &h) == 0) dc_pvr_gl_load_rgb(rgb, w, h);
    } else dc_pvr_gl_render(cfg.scale);
    note_render_target();
    need_present = 1;
    renders++; last_render_frame = (unsigned long)vblanks;
}

/* ------------------------------------------------------------------ input */
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
        { SDL_CONTROLLER_BUTTON_BACK, DC_BTN_START },      /* View / Select / - as Start too (the Dreamcast pad has one) */
        { SDL_CONTROLLER_BUTTON_DPAD_UP, DC_BTN_UP }, { SDL_CONTROLLER_BUTTON_DPAD_DOWN, DC_BTN_DOWN },
        { SDL_CONTROLLER_BUTTON_DPAD_LEFT, DC_BTN_LEFT }, { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, DC_BTN_RIGHT } };
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++) if (SDL_GameControllerGetButton(pad, map[i].s)) *b |= map[i].d;
}
static void read_input(void) {
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
        if (JB(NUP)) b |= DC_BTN_UP; if (JB(NDOWN)) b |= DC_BTN_DOWN; if (JB(NLEFT)) b |= DC_BTN_LEFT; if (JB(NRIGHT)) b |= DC_BTN_RIGHT;
        if (JB(NZL)) lt = 255; if (JB(NZR)) rt = 255;
        Sint16 ax = SDL_JoystickGetAxis(joy, 0), ay = SDL_JoystickGetAxis(joy, 1);
        if (ax < -4000 || ax > 4000) jx = axis8(ax);
        if (ay < -4000 || ay > 4000) jy = axis8(ay);
        if (JB(NPLUS) && JB(NMINUS)) { dc_front_close(); exit(0); }
        #undef JB
    }
#endif
    /* debugging: DC_PRESS="[P/]field:buttons[:fields],..." holds Dreamcast buttons (DC_BTN_* mask, hex)
     * from that presented field on, for `fields` fields (default 10), on port P (A-D, default A; a port
     * named here counts as connected); extra bits 16 L trigger, 17 R trigger, 18 stick left, 19 stick right */
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
                } else {
                    dc_pad_state *ps = &dc_pad[port];
                    ps->buttons |= (uint16_t)m;
                    if (m & (1ul << 16)) ps->lt = 255;
                    if (m & (1ul << 17)) ps->rt = 255;
                    if (m & (1ul << 18)) ps->jx = 0;
                    if (m & (1ul << 19)) ps->jx = 255;
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
    fprintf(stderr, "dc-core: %lu fields, %lu renders, %.1f s emulated in %.1f s (%.0f%% speed)\n",
            presented, renders, emu, wall, wall > 0 ? emu / wall * 100 : 0);
}
static void screenshot_to(const char *path) {
    int w = 0, h = 0;
    static uint8_t *rgb; rgb = realloc(rgb, 1920 * 4 * 1440 * 4 * 3);
    dc_pvr_gl_read(rgb, &w, &h);
    if (w <= 0) return;
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(rgb, w, h, 24, w * 3, SDL_PIXELFORMAT_RGB24);
    char name[64]; snprintf(name, sizeof name, "screenshot_%04lu.bmp", presented);
    if (!path) path = name;
    if (s) { SDL_SaveBMP(s, path); SDL_FreeSurface(s); fprintf(stderr, "saved %s\n", path); }
}
static void screenshot(void) { screenshot_to(NULL); }

static void events(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) { dc_front_close(); exit(0); }
        if (e.type == SDL_WINDOWEVENT) need_present = 1;
        if (e.type == SDL_KEYDOWN && !e.key.repeat) {
            if (e.key.keysym.sym == SDLK_ESCAPE) { dc_front_close(); exit(0); }
            if (e.key.keysym.sym == SDLK_F11) {
                cfg.fullscreen = !cfg.fullscreen;
                SDL_SetWindowFullscreen(win, cfg.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
            }
            if (e.key.keysym.sym == SDLK_F12) screenshot();
        }
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

/* ------------------------------------------------------------------ per field */
static void on_vblank(uint64_t f) {
    vblanks = f;
    if (vblanks - last_render_frame > 3 && !shows_render()) {   /* nothing drawn by the PVR: show the framebuffer */
        static uint8_t rgb[1280 * 1024 * 3]; int w, h;
        if (dc_pvr_display(rgb, 1280, 1024, &w, &h) == 0) { dc_pvr_gl_load_rgb(rgb, w, h); need_present = 1; }
    }
    if (need_present && (!dc_front_fast || presented % 8 == 0)) {   /* only when the picture changed (or the window) */
        int dw, dh; SDL_GL_GetDrawableSize(win, &dw, &dh);
        if (cfg.widescreen && dh > 0) dc_pvr_gl_aspect = (float)dw / (float)dh;   /* the next frame fills the window */
        dc_pvr_gl_present(dw, dh, 0);
        SDL_GL_SwapWindow(win);
        need_present = 0;
    }
    presented++;
    { static long dbg = -2; if (dbg == -2) { const char *e = getenv("DC_PVRDBG"); dbg = e ? atol(e) : -1; }
      if (dbg >= 0 && presented == (unsigned long)dbg) dc_pvr_sw_debug = 1; }   /* debugging: describe that frame's polygons (with DC_SW_RENDER=1) */
    if (cfg.stats && presented % 300 == 0) print_stats();
    {   /* debugging: DC_SHOTS="every[:dir]" saves the picture every `every` fields as dir/fNNNNN.bmp */
        static long every = -1; static char dir[512];
        if (every < 0) {
            const char *e = getenv("DC_SHOTS"); char *end; every = 0;
            if (e) { every = strtol(e, &end, 0); snprintf(dir, sizeof dir, "%s", *end == ':' ? end + 1 : "."); }
        }
        if (every > 0 && presented % (unsigned long)every == 0) {
            char path[640]; snprintf(path, sizeof path, "%s/f%05lu.bmp", dir, presented); screenshot_to(path);
        }
    }
    if (cfg.exit_after && presented >= cfg.exit_after) {
        if (cfg.shot_path) screenshot_to(cfg.shot_path);
        if (cfg.stats) print_stats();
        dc_front_close(); exit(0);
    }
    if (wav_file() || audio) {
        static int16_t buf[4096 * 2];
        int n;
        while ((n = dc_aica_audio_read(buf, 4096)) > 0) {
            if (wav) { fwrite(buf, 4, (size_t)n, wav); wav_frames += (uint32_t)n; }
            if (audio && SDL_GetQueuedAudioSize(audio) < 44100 * 4 / 5) SDL_QueueAudio(audio, buf, (Uint32)n * 4);   /* keep < 0.2 s queued */
        }
    }
    events();
    /* pacing: emulated SH-4 time (200 MHz) against the wall clock */
    if (dc_front_fast) { paced = 0; return; }              /* fast-forward: no pacing, resynchronised after */
    if (!cfg.no_pacing && dc_current_ctx) {
        Uint64 freq = SDL_GetPerformanceFrequency(), now = SDL_GetPerformanceCounter();
        if (!paced) { t0_count = now; t0_cycles = dc_current_ctx->cycles; paced = 1; return; }
        double emu = (double)(dc_current_ctx->cycles - t0_cycles) / 200e6, real = (double)(now - t0_count) / (double)freq;
        /* with vsync the swap already waits for the display; sleep only when more than a field ahead
         * (a display faster than 60 Hz), so a 60 Hz display isn't made to skip a refresh now and then */
        double ahead = cfg.vsync ? 0.017 : 0.0;
        if (emu > real + ahead) SDL_Delay((Uint32)((emu - real - ahead) * 1000.0));
        else if (real - emu > 0.25) { t0_count = now; t0_cycles = dc_current_ctx->cycles; }   /* too slow: don't try to catch up */
    }
}

int dc_front_open(const dc_front_config *c) {
    cfg = *c;
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
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER | (cfg.no_audio ? 0 : SDL_INIT_AUDIO)) != 0) {
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
    glc = SDL_GL_CreateContext(win);
    if (!glc) { fprintf(stderr, "OpenGL 3.3: %s\n", SDL_GetError()); return -1; }
    if (cfg.vsync) {                                       /* adaptive (late frames don't wait) where offered */
        if (SDL_GL_SetSwapInterval(-1) != 0 && SDL_GL_SetSwapInterval(1) != 0)
            { fprintf(stderr, "vsync not available: %s\n", SDL_GetError()); cfg.vsync = 0; }
        else fprintf(stderr, "vsync on (%s)\n", SDL_GL_GetSwapInterval() < 0 ? "adaptive" : "every refresh");
    } else SDL_GL_SetSwapInterval(0);
    if (dc_pvr_gl_init(getproc)) return -1;
    dc_pvr_sw_install();                                   /* TA collection (shared by the backends) */
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
    if (!cfg.no_audio) {
        SDL_AudioSpec want = {0}, have;
        want.freq = 44100; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
        audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (audio) SDL_PauseAudioDevice(audio, 0);
        else fprintf(stderr, "audio: %s (continuing without sound)\n", SDL_GetError());
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
    wav_close();
    for (int k = 0; k < 4; k++) if (pads[k]) SDL_GameControllerClose(pads[k]);
#ifndef __SWITCH__
    if (raw) { SDL_JoystickClose(raw); raw = NULL; }
#endif
#ifdef __SWITCH__
    if (joy) SDL_JoystickClose(joy);
    joy = NULL;
#endif
    if (audio) SDL_CloseAudioDevice(audio);
    if (glc) SDL_GL_DeleteContext(glc);
    if (win) SDL_DestroyWindow(win);
    memset(pads, 0, sizeof pads); audio = 0; glc = NULL; win = NULL;
    SDL_Quit();
}

unsigned long dc_front_frames(void) { return presented; }

/* dc-core desktop frontend: SDL2 window + OpenGL 3.3, audio, input, real-time pacing.
 * See dccore_front.h. */
#include "dccore_front.h"
#include "dccore/runtime.h"
#include "dccore/hw.h"
#include "dccore/pvr.h"
#include "dccore/aica.h"
#include "dccore/maple.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern sh4ctx *dc_current_ctx;         /* dispatch.c */

static SDL_Window *win;
static SDL_GLContext glc;
static SDL_AudioDeviceID audio;
static SDL_GameController *pad;
static dc_front_config cfg;
static unsigned long presented, renders, last_render_frame;
static uint64_t vblanks;
static Uint64 t0_count; static uint64_t t0_cycles; static int paced;

static void *getproc(const char *n) { return SDL_GL_GetProcAddress(n); }

static void on_render(void) {
    dc_pvr_gl_render(cfg.scale);
    renders++; last_render_frame = (unsigned long)vblanks;
}

/* ------------------------------------------------------------------ input */
static uint8_t axis8(Sint16 v) { int x = (v + 32768) >> 8; return (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x); }
static void read_input(void) {
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    uint16_t b = 0; int jx = 128, jy = 128, lt = 0, rt = 0;
    if (k[SDL_SCANCODE_UP]) { b |= DC_BTN_UP; jy = 0; }
    if (k[SDL_SCANCODE_DOWN]) { b |= DC_BTN_DOWN; jy = 255; }
    if (k[SDL_SCANCODE_LEFT]) { b |= DC_BTN_LEFT; jx = 0; }
    if (k[SDL_SCANCODE_RIGHT]) { b |= DC_BTN_RIGHT; jx = 255; }
    if (k[SDL_SCANCODE_Z] || k[SDL_SCANCODE_SPACE]) b |= DC_BTN_A;
    if (k[SDL_SCANCODE_X] || k[SDL_SCANCODE_LCTRL]) b |= DC_BTN_B;
    if (k[SDL_SCANCODE_C] || k[SDL_SCANCODE_LSHIFT]) b |= DC_BTN_X;
    if (k[SDL_SCANCODE_V] || k[SDL_SCANCODE_LALT]) b |= DC_BTN_Y;
    if (k[SDL_SCANCODE_RETURN]) b |= DC_BTN_START;
    if (k[SDL_SCANCODE_A]) lt = 255;
    if (k[SDL_SCANCODE_S]) rt = 255;
    if (pad) {
        Sint16 ax = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX), ay = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
        if (ax < -4000 || ax > 4000) jx = axis8(ax);
        if (ay < -4000 || ay > 4000) jy = axis8(ay);
        int tl = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT), tr = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        if (tl / 128 > lt) lt = tl / 128 > 255 ? 255 : tl / 128;
        if (tr / 128 > rt) rt = tr / 128 > 255 ? 255 : tr / 128;
        static const struct { SDL_GameControllerButton s; uint16_t d; } map[] = {
            { SDL_CONTROLLER_BUTTON_A, DC_BTN_A }, { SDL_CONTROLLER_BUTTON_B, DC_BTN_B }, { SDL_CONTROLLER_BUTTON_X, DC_BTN_X },
            { SDL_CONTROLLER_BUTTON_Y, DC_BTN_Y }, { SDL_CONTROLLER_BUTTON_START, DC_BTN_START },
            { SDL_CONTROLLER_BUTTON_DPAD_UP, DC_BTN_UP }, { SDL_CONTROLLER_BUTTON_DPAD_DOWN, DC_BTN_DOWN },
            { SDL_CONTROLLER_BUTTON_DPAD_LEFT, DC_BTN_LEFT }, { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, DC_BTN_RIGHT } };
        for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++) if (SDL_GameControllerGetButton(pad, map[i].s)) b |= map[i].d;
    }
    dc_pad[0].buttons = b; dc_pad[0].jx = (uint8_t)jx; dc_pad[0].jy = (uint8_t)jy; dc_pad[0].lt = (uint8_t)lt; dc_pad[0].rt = (uint8_t)rt;
}

static void screenshot(void) {
    int w = 0, h = 0;
    static uint8_t *rgb; rgb = realloc(rgb, 1920 * 4 * 1440 * 4 * 3);
    dc_pvr_gl_read(rgb, &w, &h);
    if (w <= 0) return;
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(rgb, w, h, 24, w * 3, SDL_PIXELFORMAT_RGB24);
    char name[64]; snprintf(name, sizeof name, "screenshot_%04lu.bmp", presented);
    if (s) { SDL_SaveBMP(s, name); SDL_FreeSurface(s); fprintf(stderr, "saved %s\n", name); }
}

static void events(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) { dc_front_close(); exit(0); }
        if (e.type == SDL_KEYDOWN && !e.key.repeat) {
            if (e.key.keysym.sym == SDLK_ESCAPE) { dc_front_close(); exit(0); }
            if (e.key.keysym.sym == SDLK_F11) {
                cfg.fullscreen = !cfg.fullscreen;
                SDL_SetWindowFullscreen(win, cfg.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
            }
            if (e.key.keysym.sym == SDLK_F12) screenshot();
        }
        if (e.type == SDL_CONTROLLERDEVICEADDED && !pad) pad = SDL_GameControllerOpen(e.cdevice.which);
        if (e.type == SDL_CONTROLLERDEVICEREMOVED && pad) { SDL_GameControllerClose(pad); pad = NULL; }
    }
    read_input();
}

/* ------------------------------------------------------------------ per field */
static void on_vblank(uint64_t f) {
    vblanks = f;
    if (vblanks - last_render_frame > 3) {                 /* nothing drawn by the PVR: show the framebuffer */
        static uint8_t rgb[1280 * 1024 * 3]; int w, h;
        if (dc_pvr_display(rgb, 1280, 1024, &w, &h) == 0) dc_pvr_gl_load_rgb(rgb, w, h);
    }
    int dw, dh; SDL_GL_GetDrawableSize(win, &dw, &dh);
    dc_pvr_gl_present(dw, dh, 0);
    SDL_GL_SwapWindow(win);
    presented++;
    if (audio) {
        static int16_t buf[4096 * 2];
        int n;
        while ((n = dc_aica_audio_read(buf, 4096)) > 0)
            if (SDL_GetQueuedAudioSize(audio) < 44100 * 4 / 5) SDL_QueueAudio(audio, buf, (Uint32)n * 4);   /* keep < 0.2 s queued */
    }
    events();
    /* pacing: emulated SH-4 time (200 MHz) against the wall clock */
    if (!cfg.no_pacing && dc_current_ctx) {
        Uint64 freq = SDL_GetPerformanceFrequency(), now = SDL_GetPerformanceCounter();
        if (!paced) { t0_count = now; t0_cycles = dc_current_ctx->cycles; paced = 1; return; }
        double emu = (double)(dc_current_ctx->cycles - t0_cycles) / 200e6, real = (double)(now - t0_count) / (double)freq;
        if (emu > real) SDL_Delay((Uint32)((emu - real) * 1000.0));
        else if (real - emu > 0.25) { t0_count = now; t0_cycles = dc_current_ctx->cycles; }   /* too slow: don't try to catch up */
    }
}

int dc_front_open(const dc_front_config *c) {
    cfg = *c;
    if (cfg.scale <= 0) cfg.scale = 2;
    if (!cfg.width) cfg.width = 640 * cfg.scale > 1600 ? 1600 : 640 * cfg.scale;
    if (!cfg.height) cfg.height = cfg.width * 3 / 4;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | (cfg.no_audio ? 0 : SDL_INIT_AUDIO)) != 0) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError()); return -1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3); SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#ifdef __APPLE__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24); SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    win = SDL_CreateWindow(cfg.title ? cfg.title : "dc-core", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, cfg.width, cfg.height,
                           SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | (cfg.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (!win) { fprintf(stderr, "SDL window: %s\n", SDL_GetError()); return -1; }
    glc = SDL_GL_CreateContext(win);
    if (!glc) { fprintf(stderr, "OpenGL 3.3: %s\n", SDL_GetError()); return -1; }
    SDL_GL_SetSwapInterval(0);
    if (dc_pvr_gl_init(getproc)) return -1;
    dc_pvr_sw_install();                                   /* TA collection (shared by the backends) */
    dc_maple_install();
    if (!cfg.no_audio) {
        SDL_AudioSpec want = {0}, have;
        want.freq = 44100; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
        audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (audio) SDL_PauseAudioDevice(audio, 0);
        else fprintf(stderr, "audio: %s (continuing without sound)\n", SDL_GetError());
    }
    for (int i = 0; i < SDL_NumJoysticks() && !pad; i++) if (SDL_IsGameController(i)) pad = SDL_GameControllerOpen(i);
    dc_hw.on_render = on_render;
    dc_hw.on_vblank = on_vblank;
    return 0;
}

void dc_front_close(void) {
    if (pad) SDL_GameControllerClose(pad);
    if (audio) SDL_CloseAudioDevice(audio);
    if (glc) SDL_GL_DeleteContext(glc);
    if (win) SDL_DestroyWindow(win);
    pad = NULL; audio = 0; glc = NULL; win = NULL;
    SDL_Quit();
}

unsigned long dc_front_frames(void) { return presented; }

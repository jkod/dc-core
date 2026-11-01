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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern sh4ctx *dc_current_ctx;         /* dispatch.c */

static SDL_Window *win;
static SDL_GLContext glc;
static SDL_AudioDeviceID audio;
static SDL_GameController *pad;
#ifdef __SWITCH__
static SDL_Joystick *joy;
#endif
static dc_front_config cfg;
static unsigned long presented, renders, last_render_frame;
static uint64_t vblanks;
static Uint64 t0_count; static uint64_t t0_cycles; static int paced;

static void *getproc(const char *n) { return SDL_GL_GetProcAddress(n); }

static void on_render(void) {
    static long skip = -1;                                 /* debugging: DC_RENDER_EVERY=n draws every nth frame */
    if (skip < 0) { const char *e = getenv("DC_RENDER_EVERY"); skip = e ? atol(e) : 1; if (skip < 1) skip = 1; }
    if (skip > 1 && renders % (unsigned long)skip) { renders++; last_render_frame = (unsigned long)vblanks; return; }
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
    /* debugging: DC_PRESS="field:buttons[:fields],..." holds Dreamcast buttons (DC_BTN_* mask, hex)
     * from that presented field on, for `fields` fields (default 10); extra bits 16 L trigger,
     * 17 R trigger, 18 stick left, 19 stick right */
    {
        static const char *press = (const char *)1;
        if (press == (const char *)1) press = getenv("DC_PRESS");
        for (const char *q = press; q && *q; ) {
            char *e;
            unsigned long f = strtoul(q, &e, 0), m = 0, d = 10; q = e;
            if (*q == ':') { m = strtoul(q + 1, &e, 16); q = e; }
            if (*q == ':') { d = strtoul(q + 1, &e, 0); q = e; }
            if (presented >= f && presented < f + d) {
                b |= (uint16_t)m;
                if (m & (1ul << 16)) lt = 255;
                if (m & (1ul << 17)) rt = 255;
                if (m & (1ul << 18)) jx = 0;
                if (m & (1ul << 19)) jx = 255;
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
#ifdef __SWITCH__
    cfg.width = 1280; cfg.height = 720; cfg.fullscreen = 1;   /* the console's screen / TV output */
#endif
    if (!cfg.width) cfg.width = (cfg.widescreen ? 854 : 640) * cfg.scale > 1600 ? 1600 : (cfg.widescreen ? 854 : 640) * cfg.scale;
    if (!cfg.height) cfg.height = cfg.widescreen ? cfg.width * 9 / 16 : cfg.width * 3 / 4;
    dc_pvr_gl_widescreen = cfg.widescreen;
    SDL_SetMainReady();                                    /* ports use a plain main(), not SDL_main */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER | (cfg.no_audio ? 0 : SDL_INIT_AUDIO)) != 0) {
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
    for (int i = 0; i < SDL_NumJoysticks() && !pad; i++) if (SDL_IsGameController(i)) pad = SDL_GameControllerOpen(i);
#endif
    dc_hw.on_render = on_render;
    dc_hw.on_vblank = on_vblank;
    start_count = SDL_GetPerformanceCounter(); start_cycles = dc_current_ctx ? dc_current_ctx->cycles : 0;
    return 0;
}

void dc_front_close(void) {
    wav_close();
    if (pad) SDL_GameControllerClose(pad);
#ifdef __SWITCH__
    if (joy) SDL_JoystickClose(joy);
    joy = NULL;
#endif
    if (audio) SDL_CloseAudioDevice(audio);
    if (glc) SDL_GL_DeleteContext(glc);
    if (win) SDL_DestroyWindow(win);
    pad = NULL; audio = 0; glc = NULL; win = NULL;
    SDL_Quit();
}

unsigned long dc_front_frames(void) { return presented; }

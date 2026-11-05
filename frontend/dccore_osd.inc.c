/* dc-core on-screen menu (included by dccore_front.c). Open it with L trigger + R trigger + d-pad right
 * + B held together on any controller (F1 on the keyboard); the game pauses and goes quiet while it is
 * open. Up / down pick, A (or left / right) changes, B, Start or Esc closes. Resume, Reset game,
 * Resolution (the render scale) and Aspect (widescreen / 4:3) for this session, Scanlines, Show FPS,
 * Rumble (for the controller that opened the menu: off, low, normal, strong; a short buzz shows the
 * new strength), Fullscreen, Quit. Scanlines, Show FPS and each port's rumble are kept in osd.cfg
 * next to the VMU. */

/* an original 5x7 font */
static const struct { char c; uint8_t r[7]; } osd_font[] = {
    { ' ', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } },
    { '!', { 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04 } },
    { '%', { 0x19, 0x19, 0x02, 0x04, 0x08, 0x13, 0x13 } },
    { '\'', { 0x04, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00 } },
    { '(', { 0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02 } },
    { ')', { 0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08 } },
    { '+', { 0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00 } },
    { ',', { 0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08 } },
    { '-', { 0x00, 0x00, 0x00, 0x0E, 0x00, 0x00, 0x00 } },
    { '.', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C } },
    { '/', { 0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10 } },
    { '0', { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E } },
    { '1', { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E } },
    { '2', { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F } },
    { '3', { 0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E } },
    { '4', { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 } },
    { '5', { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E } },
    { '6', { 0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E } },
    { '7', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 } },
    { '8', { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E } },
    { '9', { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E } },
    { ':', { 0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00 } },
    { '<', { 0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02 } },
    { '>', { 0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08 } },
    { '?', { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04 } },
    { 'A', { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
    { 'B', { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E } },
    { 'C', { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E } },
    { 'D', { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E } },
    { 'E', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F } },
    { 'F', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 } },
    { 'G', { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F } },
    { 'H', { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
    { 'I', { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E } },
    { 'J', { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C } },
    { 'K', { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 } },
    { 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F } },
    { 'M', { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 } },
    { 'N', { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 } },
    { 'O', { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
    { 'P', { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 } },
    { 'Q', { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D } },
    { 'R', { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 } },
    { 'S', { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E } },
    { 'T', { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
    { 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
    { 'V', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 } },
    { 'W', { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A } },
    { 'X', { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 } },
    { 'Y', { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 } },
    { 'Z', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F } },
};
#define OSD_W 200
#define OSD_ITEMS 10
static int osd_scan, osd_fps, osd_boost;
static int osd_rumble[4] = { 2, 2, 2, 2 };   /* per port: 0 off, 1 low, 2 normal, 3 strong */
static int osd_port;                         /* the port whose controller opened the menu (keyboard: A) */
static float osd_rumble_scale(int port) {
    static const float k[4] = { 0.0f, 0.5f, 1.0f, 1.6f };
    return port >= 0 && port < 4 ? k[osd_rumble[port] & 3] : 1.0f;
}
static char osd_cfg_path[1024];
static uint8_t osd_px[OSD_W * 192 * 4];
static int osd_h;
static float fps_now;
static char fps_detail[48];

static void osd_load(void) {
    char *dir = SDL_GetPrefPath("dc-core", cfg.save_name ? cfg.save_name : "default");
    if (!dir) return;
    snprintf(osd_cfg_path, sizeof osd_cfg_path, "%sosd.cfg", dir);
    SDL_free(dir);
    FILE *f = fopen(osd_cfg_path, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "scanlines=", 10)) osd_scan = atoi(line + 10) & 3;
        if (!strncmp(line, "fps=", 4)) osd_fps = atoi(line + 4) != 0;
        if (!strncmp(line, "boost=", 6)) osd_boost = atoi(line + 6) != 0;
        if (!strncmp(line, "framerate=", 10) && dc_front_fps60 >= 0) dc_front_fps60 = atoi(line + 10) >= 60;
        if (!strncmp(line, "rumble_", 7) && line[7] >= 'a' && line[7] <= 'd' && line[8] == '=') osd_rumble[line[7] - 'a'] = atoi(line + 9) & 3;
    }
    fclose(f);
    dc_pvr_gl_scanlines = osd_scan;
#ifdef __SWITCH__
    if (osd_boost) set_boost(1);
#endif
}
static void osd_save(void) {
    if (!osd_cfg_path[0]) return;
    FILE *f = fopen(osd_cfg_path, "w");
    if (!f) return;
    fprintf(f, "scanlines=%d\nfps=%d\nboost=%d\n", osd_scan, osd_fps, osd_boost);
    if (dc_front_fps60 >= 0) fprintf(f, "framerate=%d\n", dc_front_fps60 ? 60 : 30);
    for (int p = 0; p < 4; p++) fprintf(f, "rumble_%c=%d\n", 'a' + p, osd_rumble[p]);
    fclose(f);
}

static void osd_fill(uint8_t *px, int w, int x0, int y0, int x1, int y1, uint32_t rgba) {
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) {
        uint8_t *p = px + (y * w + x) * 4;
        p[0] = (uint8_t)(rgba >> 24); p[1] = (uint8_t)(rgba >> 16); p[2] = (uint8_t)(rgba >> 8); p[3] = (uint8_t)rgba;
    }
}
static int osd_text(uint8_t *px, int w, int h, int x, int y, const char *s, uint32_t rgba) {
    for (; *s; s++, x += 6) {
        char c = (*s >= 'a' && *s <= 'z') ? (char)(*s - 32) : *s;
        const uint8_t *g = NULL;
        for (unsigned i = 0; i < sizeof osd_font / sizeof osd_font[0]; i++) if (osd_font[i].c == c) { g = osd_font[i].r; break; }
        if (!g) continue;
        for (int r = 0; r < 7; r++) for (int b = 0; b < 5; b++)
            if (g[r] & (0x10 >> b) && x + b < w && y + r < h) osd_fill(px, w, x + b, y + r, x + b + 1, y + r + 1, rgba);
    }
    return x;
}

static const char *osd_label(int i, char *buf, size_t n) {
    static const char *scan[4] = { "OFF", "LIGHT", "MEDIUM", "STRONG" };
    static const char *rum[4] = { "OFF", "LOW", "NORMAL", "STRONG" };
    switch (i) {
    case 0: return "RESUME";
    case 1: return "RESET GAME";
    case 2: snprintf(buf, n, "RESOLUTION    < %dX %dP >", cfg.scale, 480 * cfg.scale); return buf;
    case 3: snprintf(buf, n, "ASPECT        < %s >", cfg.widescreen ? "WIDE" : "4:3"); return buf;
    case 4: snprintf(buf, n, "SCANLINES     < %s >", scan[osd_scan]); return buf;
    case 5: snprintf(buf, n, "SHOW FPS      < %s >", osd_fps ? "ON" : "OFF"); return buf;
    case 6:
        if (cfg.no_rumble) return "";
        snprintf(buf, n, "RUMBLE PAD %c  < %s >", 'A' + osd_port, rum[osd_rumble[osd_port] & 3]); return buf;
#ifdef __SWITCH__
    case 7: snprintf(buf, n, "CPU BOOST     < %s >", osd_boost ? "ON" : "OFF"); return buf;
#else
    case 7: snprintf(buf, n, "FULLSCREEN    < %s >", cfg.fullscreen ? "ON" : "OFF"); return buf;
#endif
    case 8:                                               /* only for a port that offers both */
        if (dc_front_fps60 < 0) return "";
        snprintf(buf, n, "FRAME RATE    < %s >", dc_front_fps60 ? "60" : "30"); return buf;
    default: return "QUIT";
    }
}
static void osd_paint(int sel) {
    const int line = 12, top = 22;
    osd_h = top + OSD_ITEMS * line + 22;
    osd_fill(osd_px, OSD_W, 0, 0, OSD_W, osd_h, 0x141A24F0u);
    osd_fill(osd_px, OSD_W, 0, 0, OSD_W, 1, 0x5A9CFFFFu); osd_fill(osd_px, OSD_W, 0, osd_h - 1, OSD_W, osd_h, 0x5A9CFFFFu);
    osd_fill(osd_px, OSD_W, 0, 0, 1, osd_h, 0x5A9CFFFFu); osd_fill(osd_px, OSD_W, OSD_W - 1, 0, OSD_W, osd_h, 0x5A9CFFFFu);
    const char *title = cfg.title ? cfg.title : "DC-CORE";
    osd_text(osd_px, OSD_W, osd_h, (OSD_W - (int)strlen(title) * 6) / 2, 8, title, 0x9CC8FFFFu);
    for (int i = 0; i < OSD_ITEMS; i++) {
        char buf[64]; const char *s = osd_label(i, buf, sizeof buf);
        if (!*s) continue;
        int y = top + i * line;
        if (i == sel) osd_fill(osd_px, OSD_W, 6, y - 2, OSD_W - 6, y + 9, 0x2F5FB4FFu);
        osd_text(osd_px, OSD_W, osd_h, 14, y, s, i == sel ? 0xFFFFFFFFu : 0xB4BCC8FFu);
    }
    osd_text(osd_px, OSD_W, osd_h, 14, osd_h - 13, "A SELECT   B BACK", 0x7884A0FFu);
}
/* drawn over the presented picture, before the swap */
static void osd_overlays(int ww, int wh) {
    if (osd_open) {
        static const uint8_t dim[4] = { 0, 0, 0, 120 };
        dc_pvr_gl_overlay(dim, 1, 1, 0, 0, ww, wh, ww, wh);
        int k = wh * 6 / 10 / osd_h; if (k * OSD_W > ww * 9 / 10) k = ww * 9 / 10 / OSD_W; if (k < 1) k = 1;
        dc_pvr_gl_overlay(osd_px, OSD_W, osd_h, (ww - OSD_W * k) / 2, (wh - osd_h * k) / 2, OSD_W * k, osd_h * k, ww, wh);
    }
    if (osd_fps) {                                        /* frames a second, and where the time goes */
        enum { FW = 140, FH = 20 };
        static uint8_t px[FW * FH * 4];
        char s[16]; snprintf(s, sizeof s, "%.0f FPS", fps_now);
        memset(px, 0, sizeof px);
        int w1 = (int)strlen(s) * 6 + 3, w2 = (int)strlen(fps_detail) * 6 + 3, w = w1 > w2 ? w1 : w2;
        osd_fill(px, FW, 0, 0, w < FW ? w : FW, FH, 0x000000A0u);
        osd_text(px, FW, FH, 2, 2, s, 0x7CFF7CFFu);
        osd_text(px, FW, FH, 2, 11, fps_detail, 0xC8D2DCFFu);
        int k = wh / 360; if (k < 2) k = 2;
        dc_pvr_gl_overlay(px, FW, FH, 8, 8, FW * k, FH * k, ww, wh);
    }
}
/* rendered frames a second, for Show FPS, and where each frame's time goes (ms): G the game's code,
 * S the sound chip, R rendering, P presenting (with vsync, waiting for the display), I pacing sleeps */

static void osd_count_frame(void) {
    static Uint64 t0, a0, r0t, p0, i0; static unsigned long r0;
    Uint64 now = SDL_GetPerformanceCounter();
    if (!t0) { t0 = now; r0 = renders; a0 = dc_prof_aica; r0t = prof_render; p0 = prof_present; i0 = prof_idle; return; }
    double f = (double)SDL_GetPerformanceFrequency(), dt = (double)(now - t0) / f;
    if (dt < 0.5) return;
    unsigned long fr = renders - r0;
    fps_now = (float)(fr / dt);
    double per = fr ? 1000.0 / f / (double)fr : 0;
    double a = (double)(dc_prof_aica - a0) * per, r = (double)(prof_render - r0t) * per, p = (double)(prof_present - p0) * per,
           i = (double)(prof_idle - i0) * per, tot = fr ? dt * 1000.0 / (double)fr : 0, g = tot - a - r - p - i;
    snprintf(fps_detail, sizeof fps_detail, "G%.0f S%.0f R%.0f P%.0f I%.0f", g < 0 ? 0 : g, a, r, p, i);
    if (cfg.stats) fprintf(stderr, "dc-core: %.1f fps, ms a frame: game %.1f, sound %.1f, render %.1f, present %.1f, idle %.1f\n",
                           fps_now, g < 0 ? 0 : g, a, r, p, i);
    t0 = now; r0 = renders; a0 = dc_prof_aica; r0t = prof_render; p0 = prof_present; i0 = prof_idle;
    if (osd_fps) need_present = 1;
}

static void osd_restart(void) {
    fprintf(stderr, "dc-core: reset (starting the game again)\n");
    fflush(NULL);
#if defined(_WIN32)
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(NULL, exe, MAX_PATH);
    size_t n = wcslen(GetCommandLineW()) + 1;
    wchar_t *cl = malloc(n * sizeof *cl); memcpy(cl, GetCommandLineW(), n * sizeof *cl);
    dc_front_close();
    STARTUPINFOW si; PROCESS_INFORMATION pi; memset(&si, 0, sizeof si); si.cb = sizeof si;
    if (CreateProcessW(exe, cl, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    exit(0);
#elif defined(__SWITCH__)
    extern int __system_argc; extern char **__system_argv;
    extern unsigned int envSetNextLoad(const char *path, const char *argv);
    static char args[2048]; args[0] = 0;
    for (int i = 0; i < __system_argc; i++) {
        size_t l = strlen(args);
        snprintf(args + l, sizeof args - l, i ? " \"%s\"" : "\"%s\"", __system_argv[i]);
    }
    dc_front_close();
    if (__system_argc > 0) envSetNextLoad(__system_argv[0], args);   /* the homebrew loader starts it again */
    exit(0);
#else
    static char exe[4096], buf[65536]; static char *av[256]; int ac = 0;
#ifdef __APPLE__
    extern int *_NSGetArgc(void); extern char ***_NSGetArgv(void); extern int _NSGetExecutablePath(char *, uint32_t *);
    uint32_t sz = sizeof exe; _NSGetExecutablePath(exe, &sz);
    for (int i = 0; i < *_NSGetArgc() && ac < 255; i++) av[ac++] = (*_NSGetArgv())[i];
#else
    ssize_t l = readlink("/proc/self/exe", exe, sizeof exe - 1); if (l > 0) exe[l] = 0;
    FILE *f = fopen("/proc/self/cmdline", "rb");
    size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0; if (f) fclose(f);
    for (size_t i = 0; i < n && ac < 255; i += strlen(buf + i) + 1) av[ac++] = buf + i;
#endif
    av[ac] = NULL;
    dc_front_close();
    execv(exe, av);
    perror("reset");
    exit(1);
#endif
}

static int osd_combo(void) {                  /* the menu combo held: that port + 1 */
    for (int p = 0; p < 4; p++) {
        const dc_pad_state *s = &dc_pad[p];
        if (s->lt >= 160 && s->rt >= 160 && (s->buttons & (DC_BTN_RIGHT | DC_BTN_B)) == (DC_BTN_RIGHT | DC_BTN_B)) return p + 1;
    }
    return 0;
}
static void osd_rumble_pad(int port, float level, Uint32 ms) {   /* rumble straight on a port's gamepad */
    Uint16 v = (Uint16)((level > 1 ? 1 : level) * 65535.0f);
#ifdef __SWITCH__
    if (port == 0 && joy) SDL_JoystickRumble(joy, v, v, ms);
#else
    if (port >= 0 && port < 4 && pads[port]) SDL_GameControllerRumble(pads[port], v, v, ms);
#endif
}
static uint16_t osd_buttons(void) {          /* every pad, the stick as the d-pad */
    uint16_t b = 0;
    for (int p = 0; p < 4; p++) {
        b |= dc_pad[p].buttons;
        if (dc_pad[p].jy < 64) b |= DC_BTN_UP;
        if (dc_pad[p].jy > 192) b |= DC_BTN_DOWN;
        if (dc_pad[p].jx < 64) b |= DC_BTN_LEFT;
        if (dc_pad[p].jx > 192) b |= DC_BTN_RIGHT;
    }
    return b;
}
static int osd_all_released(void) {
    for (int p = 0; p < 4; p++) if (dc_pad[p].buttons || dc_pad[p].lt > 40 || dc_pad[p].rt > 40) return 0;
    return !(osd_buttons() & (DC_BTN_UP | DC_BTN_DOWN | DC_BTN_LEFT | DC_BTN_RIGHT));
}
static void osd_frame(void) { events(); need_present = 1; present_window(); SDL_Delay(8); }

static void osd_run(int port) {
    osd_req = 0;
    osd_port = port >= 0 && port < 4 ? port : 0;
    for (int p = 0; p < 4; p++) osd_rumble_pad(p, 0, 0);  /* the game is paused: so is its rumble */
    if (audio) SDL_PauseAudioDevice(audio, 1);
    fprintf(stderr, "dc-core: menu\n");
    int sel = 0;
    if (cfg.scale < 1) cfg.scale = 2;
    osd_open = 1; osd_paint(sel);
    while (!osd_all_released() && osd_open) osd_frame();   /* the buttons that opened it don't count */
    uint16_t prev = 0;
    Uint32 rep_t = 0;
    while (osd_open) {
        osd_frame();
        if (osd_req) { osd_req = 0; break; }               /* F1 / Esc again */
        uint16_t b = osd_buttons(), down = (uint16_t)(b & ~prev);
        /* held up / down repeats */
        if (b & (DC_BTN_UP | DC_BTN_DOWN)) { if (!(prev & (DC_BTN_UP | DC_BTN_DOWN))) rep_t = SDL_GetTicks() + 350; else if (SDL_GetTicks() > rep_t) { down |= b & (DC_BTN_UP | DC_BTN_DOWN); rep_t = SDL_GetTicks() + 90; } }
        prev = b;
        int step = 0;
        if (down & DC_BTN_UP) { do sel = (sel + OSD_ITEMS - 1) % OSD_ITEMS; while (!*osd_label(sel, (char[64]){0}, 64)); }
        if (down & DC_BTN_DOWN) { do sel = (sel + 1) % OSD_ITEMS; while (!*osd_label(sel, (char[64]){0}, 64)); }
        if (down & DC_BTN_LEFT) step = -1;
        if (down & (DC_BTN_RIGHT | DC_BTN_A)) step = 1;
        if (down & (DC_BTN_B | DC_BTN_START)) break;
        if (step) {
            if (sel == 0 && (down & DC_BTN_A)) break;
            if (sel == 1 && (down & DC_BTN_A)) osd_restart();
            if (sel == 2) {                                 /* the next frame renders at the new size */
#ifdef __SWITCH__
                const int max_scale = 3;
#else
                const int max_scale = 6;
#endif
                cfg.scale += step; if (cfg.scale < 1) cfg.scale = max_scale; if (cfg.scale > max_scale) cfg.scale = 1;
            }
            if (sel == 3) {
                cfg.widescreen = !cfg.widescreen; dc_pvr_gl_widescreen = cfg.widescreen;
            }
            if (sel == 4) { osd_scan = (osd_scan + 4 + step) % 4; dc_pvr_gl_scanlines = osd_scan; osd_save(); }
            if (sel == 5) { osd_fps = !osd_fps; osd_save(); }
            if (sel == 6) {                                 /* this controller's rumble, felt at once */
                osd_rumble[osd_port] = (osd_rumble[osd_port] + 4 + step) % 4; osd_save();
                osd_rumble_pad(osd_port, 0.6f * osd_rumble_scale(osd_port), 250);
            }
#ifdef __SWITCH__
            if (sel == 7) { osd_boost = !osd_boost; set_boost(osd_boost); osd_save(); }
#else
            if (sel == 7) { cfg.fullscreen = !cfg.fullscreen; SDL_SetWindowFullscreen(win, cfg.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0); }
#endif
            if (sel == 8 && dc_front_fps60 >= 0) { dc_front_fps60 = !dc_front_fps60; osd_save(); }
            if (sel == 9 && (down & DC_BTN_A)) { dc_front_close(); exit(0); }
        }
        osd_paint(sel);
    }
    osd_open = 0;
    while (!osd_all_released()) osd_frame();               /* the game doesn't see the closing press */
    need_present = 1;
    if (audio) SDL_PauseAudioDevice(audio, 0);
    paced = 0;
}

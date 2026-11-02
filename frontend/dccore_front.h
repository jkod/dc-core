#ifndef DCCORE_FRONT_H
#define DCCORE_FRONT_H
/* dc-core desktop frontend (SDL2 + OpenGL 3.3): window, rendering, audio, input, pacing.
 *
 * A port opens it once before starting the game's code; from then on the hardware model's hooks
 * drive it: frames are drawn at STARTRENDER (pvr_gl.c, `scale` x the console resolution), shown
 * at vblank, audio from the AICA is queued, the keyboard / first game controller become the
 * Dreamcast pad on port A, and emulated time is paced to real time. Closing the window exits.
 *
 * Keys: arrows = stick + d-pad, Z/X/C/V (or Space/Ctrl/Shift/Alt) = A/B/X/Y, Return = Start,
 * A / S = left / right trigger, F11 = fullscreen, F12 = screenshot, Esc = quit.
 */
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const char *title;
    int scale;              /* render resolution multiplier (1 = 640x480), 0 = 2 */
    int width, height;      /* window size, 0 = 4:3 at scale */
    int fullscreen;
    int widescreen;         /* 16:9 (dc_pvr_gl_widescreen) */
    int no_pacing;          /* run as fast as possible (benchmarks) */
    int arrows_stick_only;  /* keyboard arrows move only the analog stick (d-pad on I/J/K/L): for games
                               where the d-pad does something else while driving (Rush: camera) */
    int no_audio;
    unsigned long exit_after;   /* quit after this many presented fields (0 = run until closed) */
    const char *shot_path;      /* with exit_after: save the last rendered frame here (.bmp) */
    const char *save_name;      /* VMU folder name under the user's app data (default "dc-core") */
    const char *vmu_path;       /* or an explicit VMU image file (.bin, 128 KB) */
    int no_vmu;
    int stats;                  /* print fields / renders / emulated vs wall time on exit and every 5 s */
} dc_front_config;
int  dc_front_open(const dc_front_config *cfg);   /* 0 = ok */
void dc_front_close(void);
/* frames presented so far / whether the window was closed (for tests and headless runs) */
unsigned long dc_front_frames(void);
#ifdef __cplusplus
}
#endif
#endif

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
    int vsync;                  /* wait for the display's refresh when showing a frame (no tearing; the
                                   game then runs at the display's rate if that is below the console's) */
    int background_input;       /* read controllers and keys while the window is not focused (default:
                                   only the focused window gets input, so two ports running side by side
                                   don't both follow one pad); DC_BACKGROUND_INPUT=1 does the same */
    int run_in_background;      /* keep running and playing sound while the window is not focused (default:
                                   the game pauses and mutes until it gets focus back; background_input
                                   implies this); DC_RUN_IN_BACKGROUND=1 does the same */
} dc_front_config;
int  dc_front_open(const dc_front_config *cfg);   /* 0 = ok */
void dc_front_close(void);
/* frames presented so far / whether the window was closed (for tests and headless runs) */
unsigned long dc_front_frames(void);
/* non-zero: fast-forward (no pacing to the wall clock, every 8th field shown). A port sets it while
 * the game shows a loading screen that waits on fields rather than on the disc. */
extern int dc_front_fast;
#ifdef __cplusplus
}
#endif
#endif

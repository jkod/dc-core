/* Headless libretro runner for a reference emulator core (e.g. Flycast): loads a disc, runs
 * frames with no video/audio/input, and dumps main RAM so dc-core runs can be compared with it.
 *
 *   cc -O2 tools/reference/lr_ref.c -ldl -o lr_ref
 *   lr_ref core.so game.cue SYSTEM_DIR OUTDIR frames dump_every
 *     SYSTEM_DIR must contain dc/dc_boot.bin (+ dc/dc_flash.bin)
 *     writes OUTDIR/ram_<frame>.bin (16 MB) every dump_every frames
 */
#include <dlfcn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* minimal libretro.h subset */
#define RETRO_ENVIRONMENT_GET_CAN_DUPE 3
#define RETRO_ENVIRONMENT_SET_PIXEL_FORMAT 10
#define RETRO_ENVIRONMENT_SET_HW_RENDER 14
#define RETRO_ENVIRONMENT_GET_VARIABLE 15
#define RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE 17
#define RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY 9
#define RETRO_ENVIRONMENT_GET_LOG_INTERFACE 27
#define RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY 31
#define RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION 52
#define RETRO_MEMORY_SYSTEM_RAM 2
struct retro_game_info { const char *path; const void *data; size_t size; const char *meta; };
struct retro_variable { const char *key; const char *value; };
typedef void (*retro_log_printf_t)(int level, const char *fmt, ...);
struct retro_log_callback { retro_log_printf_t log; };

static const char *sysdir, *savedir;
static int verbose;
static void logf_(int level, const char *fmt, ...) {
    if (!verbose && level < 2) return;
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
}
static const char *opt(const char *key) {
    /* keep the reference close to hardware and fast to run headless */
    if (!strcmp(key, "reicast_region") || !strcmp(key, "flycast_region")) return "Europe";
    if (!strcmp(key, "reicast_boot_to_bios") || !strcmp(key, "flycast_boot_to_bios")) return "disabled";
    if (!strcmp(key, "reicast_hle_bios") || !strcmp(key, "flycast_hle_bios")) return "disabled";
    if (!strcmp(key, "reicast_threaded_rendering") || !strcmp(key, "flycast_threaded_rendering")) return "disabled";
    return getenv(key);   /* override any option from the environment */
}
static bool env(unsigned cmd, void *data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: *(const char **)data = sysdir; return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *(const char **)data = savedir; return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: ((struct retro_log_callback *)data)->log = logf_; return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: { struct retro_variable *v = data; v->value = opt(v->key); return v->value != NULL; }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: *(bool *)data = false; return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool *)data = true; return true;
    case RETRO_ENVIRONMENT_SET_HW_RENDER: return getenv("LR_HW") != NULL;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION: *(unsigned *)data = 0; return true;
    default: return false;
    }
}
static void video(const void *d, unsigned w, unsigned h, size_t p) { (void)d; (void)w; (void)h; (void)p; }
static void audio(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t audio_batch(const int16_t *d, size_t n) { (void)d; return n; }
static void input_poll(void) {}
static int16_t input_state(unsigned port, unsigned dev, unsigned idx, unsigned id) { (void)port; (void)dev; (void)idx; (void)id; return 0; }

typedef void (*fn_env)(bool (*)(unsigned, void *));
typedef void (*fn_video)(void (*)(const void *, unsigned, unsigned, size_t));
typedef void (*fn_audio)(void (*)(int16_t, int16_t));
typedef void (*fn_audio_batch)(size_t (*)(const int16_t *, size_t));
typedef void (*fn_poll)(void (*)(void));
typedef void (*fn_state)(int16_t (*)(unsigned, unsigned, unsigned, unsigned));
typedef void (*fn_void)(void);
typedef bool (*fn_load)(const struct retro_game_info *);
typedef void *(*fn_mem)(unsigned);
typedef size_t (*fn_memsize)(unsigned);
#define SYM(T, n) T n = (T)dlsym(core, #n); if (!n) { fprintf(stderr, "missing %s\n", #n); return 2; }

int main(int argc, char **argv) {
    if (argc < 7) { fprintf(stderr, "usage: %s core.so game.cue sysdir outdir frames dump_every\n", argv[0]); return 2; }
    verbose = getenv("LR_VERBOSE") != NULL;
    void *core = dlopen(argv[1], RTLD_NOW);
    if (!core) { fprintf(stderr, "%s\n", dlerror()); return 2; }
    sysdir = argv[3]; savedir = argv[4];
    SYM(fn_env, retro_set_environment); SYM(fn_video, retro_set_video_refresh);
    SYM(fn_audio, retro_set_audio_sample); SYM(fn_audio_batch, retro_set_audio_sample_batch);
    SYM(fn_poll, retro_set_input_poll); SYM(fn_state, retro_set_input_state);
    SYM(fn_void, retro_init); SYM(fn_load, retro_load_game); SYM(fn_void, retro_run);
    SYM(fn_mem, retro_get_memory_data); SYM(fn_memsize, retro_get_memory_size);
    retro_set_environment(env); retro_set_video_refresh(video); retro_set_audio_sample(audio);
    retro_set_audio_sample_batch(audio_batch); retro_set_input_poll(input_poll); retro_set_input_state(input_state);
    retro_init();
    struct retro_game_info gi = { argv[2], NULL, 0, NULL };
    if (!retro_load_game(&gi)) { fprintf(stderr, "load_game failed\n"); return 1; }
    uint8_t *ram = retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
    size_t rs = retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
    fprintf(stderr, "running, ram=%p size=%zu\n", (void *)ram, rs);
    int frames = atoi(argv[5]), every = atoi(argv[6]);
    for (int f = 1; f <= frames; f++) {
        retro_run();
        if (ram && every && f % every == 0) {
            char p[1024]; snprintf(p, sizeof p, "%s/ram_%05d.bin", argv[4], f);
            FILE *o = fopen(p, "wb"); if (o) { fwrite(ram, 1, rs, o); fclose(o); }
            fprintf(stderr, "frame %d dumped\n", f);
        }
    }
    return 0;
}

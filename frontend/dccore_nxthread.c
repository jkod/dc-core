/* Switch: dc-core's own threads (the sound chip's, the profiler's) are libnx threads, not SDL ones. A thread
 * SDL made crashed in SDL's thread clean-up when it ended, and they have to end: on Reset and Quit the
 * homebrew loader takes over the same process, so anything still running would run unloaded code. */
#include <switch.h>
#include <stdlib.h>

typedef struct { Thread t; int (*fn)(void *); void *arg; } nx_thread;

/* The thread ends with svcExitThread right away: returning into libnx (threadExit and the C library's
 * per-thread clean-up) crashed on a garbage pointer at Quit, with SDL's threads and libnx's alike. The
 * handle still signals, so dc_nx_thread_join works; the thread's small C-library block is not freed. */
static void entry(void *p) { nx_thread *x = p; x->fn(x->arg); svcExitThread(); }

void *dc_nx_thread_start(int (*fn)(void *), void *arg, int core) {
    nx_thread *x = calloc(1, sizeof *x);
    if (!x) return NULL;
    x->fn = fn; x->arg = arg;
    if (R_FAILED(threadCreate(&x->t, entry, x, NULL, 0x40000, 0x2C, core))) { free(x); return NULL; }
    if (R_FAILED(threadStart(&x->t))) { threadClose(&x->t); free(x); return NULL; }
    return x;
}

uint32_t dc_nx_thread_handle(void *p) { return p ? ((nx_thread *)p)->t.handle : 0; }

void dc_nx_thread_join(void *p) {
    nx_thread *x = p;
    if (!x) return;
    threadWaitForExit(&x->t);
    threadClose(&x->t);
    free(x);
}

/* ------------------------------------------------------------------ sound out (Switch)
 * SDL's audio thread crashed when it ended (closing the device at Quit / Reset), so on the Switch sound
 * goes straight to audout from a thread of ours: 48 kHz stereo, three 1024-frame buffers, the game's
 * 44.1 kHz resampled linearly. cb fills 16-bit stereo frames at 44.1 kHz like an SDL callback. */
#include <string.h>
#include <malloc.h>
#define NXA_FRAMES 1024
#define NXA_BUFS 3
static AudioOutBuffer nxa_buf[NXA_BUFS];
static int16_t *nxa_mem[NXA_BUFS];
static void (*nxa_cb)(void *, uint8_t *, int);
static void *nxa_user, *nxa_thr;
static volatile int nxa_quit, nxa_paused;
static Mutex nxa_mx;                                   /* held while cb runs (dc_nx_audio_lock) */
static int16_t nxa_src[2][2];                          /* the two 44.1 kHz frames being interpolated */
static double nxa_pos = 1.0;                           /* where between them (1.0: fetch the next one) */

static void nxa_fill(int16_t *dst) {
    static int16_t in[NXA_FRAMES * 2]; static int n_in, at_in;
    mutexLock(&nxa_mx);
    for (int i = 0; i < NXA_FRAMES; i++) {
        while (nxa_pos >= 1.0) {
            nxa_pos -= 1.0;
            nxa_src[0][0] = nxa_src[1][0]; nxa_src[0][1] = nxa_src[1][1];
            if (at_in >= n_in) {                       /* ask for the next block at 44.1 kHz */
                n_in = 941; at_in = 0;
                if (nxa_paused || !nxa_cb) memset(in, 0, sizeof in);
                else nxa_cb(nxa_user, (uint8_t *)in, n_in * 4);
            }
            nxa_src[1][0] = in[at_in * 2]; nxa_src[1][1] = in[at_in * 2 + 1]; at_in++;
        }
        for (int c = 0; c < 2; c++) dst[i * 2 + c] = (int16_t)(nxa_src[0][c] + (nxa_src[1][c] - nxa_src[0][c]) * nxa_pos);
        nxa_pos += 44100.0 / 48000.0;
    }
    mutexUnlock(&nxa_mx);
}

static int nxa_main(void *u) {
    (void)u;
    for (int b = 0; b < NXA_BUFS; b++) { nxa_fill(nxa_mem[b]); audoutAppendAudioOutBuffer(&nxa_buf[b]); }
    while (!nxa_quit) {
        AudioOutBuffer *done = NULL; u32 n = 0;
        if (R_FAILED(audoutWaitPlayFinish(&done, &n, 100000000ull)) || !done || !n) continue;   /* 0.1 s */
        nxa_fill(done->buffer);
        audoutAppendAudioOutBuffer(done);
    }
    return 0;
}

int dc_nx_audio_open(void (*cb)(void *, uint8_t *, int), void *user) {
    mutexInit(&nxa_mx);
    if (R_FAILED(audoutInitialize())) return 0;
    if (R_FAILED(audoutStartAudioOut())) { audoutExit(); return 0; }
    size_t sz = NXA_FRAMES * 4;                        /* 4096: a multiple of 0x1000 as audout wants */
    for (int b = 0; b < NXA_BUFS; b++) {
        nxa_mem[b] = memalign(0x1000, sz);
        if (!nxa_mem[b]) return 0;
        memset(nxa_mem[b], 0, sz);
        nxa_buf[b].next = NULL; nxa_buf[b].buffer = nxa_mem[b]; nxa_buf[b].buffer_size = sz;
        nxa_buf[b].data_size = sz; nxa_buf[b].data_offset = 0;
    }
    nxa_cb = cb; nxa_user = user; nxa_quit = 0; nxa_paused = 0;
    nxa_thr = dc_nx_thread_start(nxa_main, NULL, 2);
    return nxa_thr != NULL;
}
void dc_nx_audio_pause(int on) { nxa_paused = on; }
void dc_nx_audio_lock(void) { mutexLock(&nxa_mx); }
void dc_nx_audio_unlock(void) { mutexUnlock(&nxa_mx); }
void dc_nx_audio_close(void) {
    if (!nxa_thr) return;
    nxa_quit = 1;
    dc_nx_thread_join(nxa_thr); nxa_thr = NULL;
    audoutStopAudioOut();
    audoutExit();
    for (int b = 0; b < NXA_BUFS; b++) { free(nxa_mem[b]); nxa_mem[b] = NULL; }
}

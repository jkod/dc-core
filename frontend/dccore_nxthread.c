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

void dc_nx_thread_join(void *p) {
    nx_thread *x = p;
    if (!x) return;
    threadWaitForExit(&x->t);
    threadClose(&x->t);
    free(x);
}

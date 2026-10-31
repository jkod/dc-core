/* dc-core: guest setjmp/longjmp and cooperative threads on host coroutines (see coro.h). */
#if defined(__APPLE__) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 600      /* ucontext on macOS */
#endif
#include "dccore/coro.h"
#include "dccore/runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <ucontext.h>

extern sh4ctx *dc_current_ctx;    /* dispatch.c */
uint32_t dc_coro_setjmp_addr;   /* 0 = feature off: DC_CALL_SETJMP is a plain call */
int dc_coro_trace;

typedef struct coro {
    ucontext_t uc;
    void *stack;
    struct slot *pending;        /* set by whoever switches here: longjmp to this slot on resume */
    int refs;                    /* live slots owned by this coroutine */
    uint32_t sp_key;             /* suspended in a stack switch: resumed when a task with this r15 is loaded */
    int sp_waiting;
    int started, is_main;
} coro;

typedef struct slot {
    jmp_buf jb;                  /* first: the address of the slot is the jmp_buf* handed out */
    uint32_t buf, r15, pr;       /* guest buffer and the state its longjmp returns to */
    coro *owner;
    int used;
} slot;

#define MAX_SLOTS 256
#define MAX_COROS 64
static slot slots[MAX_SLOTS];
static coro coros[MAX_COROS];
static int ncoros;
static coro main_coro = { .is_main = 1, .started = 1 };
static coro *cur = &main_coro;
static sh4fn orig_longjmp;
static uint32_t longjmp_addr;
static size_t stack_bytes = 1u << 20;
static uint32_t start_pc;        /* where a new coroutine starts */

int dc_coro_count(void) { return ncoros; }

static slot *find_slot(uint32_t buf) {
    for (int i = 0; i < MAX_SLOTS; i++) if (slots[i].used && slots[i].buf == buf) return &slots[i];
    return NULL;
}
static void drop_slot(slot *s) { if (s->used) { s->used = 0; s->owner->refs--; } }

jmp_buf *dc_coro_setjmp_slot(sh4ctx *c) {
    uint32_t buf = (c->r[4] & 0x1FFFFFFFu) | 0x80000000u;
    slot *s = find_slot(buf);
    if (s) drop_slot(s);
    else for (int i = 0; i < MAX_SLOTS && !s; i++) if (!slots[i].used) s = &slots[i];
    if (!s) { fprintf(stderr, "dc-core: out of setjmp slots\n"); return NULL; }
    s->buf = buf;
    return &s->jb;
}

void dc_coro_setjmp_done(sh4ctx *c, jmp_buf *jb) {
    if (!jb) return;
    slot *s = (slot *)jb;
    s->r15 = c->r[15]; s->pr = c->pr; s->owner = cur; s->used = 1; cur->refs++;
}

static void switch_to(coro *to) {
    coro *from = cur;
    cur = to;
    swapcontext(&from->uc, &to->uc);
    /* resumed: someone longjmp'd into one of our setjmps */
    cur = from;
    slot *s = from->pending; from->pending = NULL;
    if (s) DC_LONGJMP(s->jb, 1);
}

/* a thread started by longjmp into a hand-made context: the restore ended with rts, so run from pr;
 * a guest function returning goes on at its pr, like rts */
static void coro_main(void) {
    sh4ctx *c = dc_current_ctx;
    uint32_t pc = start_pc;
    for (;;) { dc_call(c, pc); pc = c->pr; }
}

static coro *new_coro(void) {
    coro *k = NULL;
    for (int i = 0; i < ncoros && !k; i++)              /* reuse one nothing can resume any more */
        if (coros[i].refs == 0 && !coros[i].sp_waiting && &coros[i] != cur) k = &coros[i];
    if (!k) {
        if (ncoros == MAX_COROS) { fprintf(stderr, "dc-core: out of coroutines\n"); abort(); }
        k = &coros[ncoros++];
        k->stack = malloc(stack_bytes);
    }
    getcontext(&k->uc);
    k->uc.uc_stack.ss_sp = k->stack; k->uc.uc_stack.ss_size = stack_bytes; k->uc.uc_link = NULL;
    makecontext(&k->uc, coro_main, 0);
    k->pending = NULL; k->refs = 0; k->started = 1; k->sp_waiting = 0;
    return k;
}

static uint32_t h_longjmp(sh4ctx *c) {
    uint32_t buf = (c->r[4] & 0x1FFFFFFFu) | 0x80000000u;
    /* the game's own restore: registers, r15, pr, r0 = val (ends with rts) */
    c->entry = longjmp_addr;
    uint32_t next = orig_longjmp(c);
    if (next) dc_call(c, next);
    slot *s = find_slot(buf);
    if (s && s->r15 == c->r[15] && s->pr == c->pr) {
        if (dc_coro_trace) fprintf(stderr, "coro: longjmp %08X -> setjmp of %s%ld (pr %08X)\n", buf,
                                   s->owner->is_main ? "main" : "coro ", s->owner->is_main ? 0L : (long)(s->owner - coros), s->pr);
        if (s->owner == cur) DC_LONGJMP(s->jb, 1);
        s->owner->pending = s;
        switch_to(s->owner);
        return 0;   /* not reached: switch_to longjmps when we are resumed */
    }
    /* a context the game made itself (new thread, or a thread sent to its exit routine) */
    if (s) drop_slot(s);
    start_pc = c->pr;
    coro *k = new_coro();
    if (dc_coro_trace) fprintf(stderr, "coro: longjmp %08X starts coro %ld at %08X (r15 %08X)\n",
                               buf, (long)(k - coros), c->pr, c->r[15]);
    switch_to(k);
    return 0;
}

void dc_coro_install(uint32_t setjmp_addr, uint32_t lj_addr, size_t host_stack) {
    setjmp_addr = (setjmp_addr & 0x1FFFFFFFu) | 0x80000000u;
    lj_addr = (lj_addr & 0x1FFFFFFFu) | 0x80000000u;
    if (host_stack) stack_bytes = host_stack;
    orig_longjmp = dc_lookup(lj_addr);
    if (!orig_longjmp) { fprintf(stderr, "dc-core: no recompiled longjmp at %08X\n", lj_addr); return; }
    longjmp_addr = lj_addr;
    dc_coro_setjmp_addr = setjmp_addr;
    dc_hle_register(lj_addr, h_longjmp);
}

/* ---- task switches that swap stacks and end with rte (dc_coro_add_switch) ----
 * The routine saves the running task's registers on its stack, stores that stack pointer, loads
 * another task's and returns into it with rte (PC = spc). A task calls it like a function, so it
 * is resumed with the r15 it called with; that r15 identifies its suspended host coroutine. */
#define MAX_SW 8
static struct { uint32_t addr; sh4fn orig; } sw[MAX_SW];
static int nsw;

/* The switch routine's own frame need not be balanced to the byte (pushes on one entry path,
 * pops on another), so a task is matched by the nearest waiting stack within 256 bytes; different
 * tasks' stacks are kilobytes apart. */
static coro *waiting_on(uint32_t r15) {
    coro *best = NULL; uint32_t bd = 257;
    for (int i = -1; i < ncoros; i++) {
        coro *k = i < 0 ? &main_coro : &coros[i];
        if (!k->sp_waiting) continue;
        uint32_t d = k->sp_key > r15 ? k->sp_key - r15 : r15 - k->sp_key;
        if (d < bd) { bd = d; best = k; }
    }
    return best;
}

static uint32_t h_switch(sh4ctx *c) {
    uint32_t at = c->entry, r15 = c->r[15], ret = c->pr;
    sh4fn orig = NULL;
    for (int i = 0; i < nsw; i++) if (sw[i].addr == at) orig = sw[i].orig;
    uint32_t next = orig(c);                 /* the game's switch; its rte returns here */
    if (next) dc_call(c, next);
    uint32_t d = c->r[15] > r15 ? c->r[15] - r15 : r15 - c->r[15];
    if (d <= 256) return 0;    /* the task picked itself (also when preempted: the stack differs a little) */
    (void)ret;
    coro *to = waiting_on(c->r[15]);
    cur->sp_key = r15; cur->sp_waiting = 1;
    if (!to) {
        start_pc = c->spc;
        to = new_coro();
        if (dc_coro_trace) fprintf(stderr, "coro: switch %08X starts coro %ld at %08X (r15 %08X)\n",
                                   at, (long)(to - coros), c->spc, c->r[15]);
    } else if (dc_coro_trace) fprintf(stderr, "coro: switch %08X resumes %s%ld (key %08X, r15 %08X pc %08X)\n",
                                      at, to->is_main ? "main" : "coro", to->is_main ? 0L : (long)(to - coros), to->sp_key, c->r[15], c->spc);
    if (dc_coro_trace) fprintf(stderr, "coro:   suspended %s%ld under r15 %08X\n", cur->is_main ? "main" : "coro",
                               cur->is_main ? 0L : (long)(cur - coros), r15);
    to->sp_waiting = 0;
    switch_to(to);
    /* resumed: the task that switched to us has loaded our registers */
    return 0;
}

void dc_coro_add_switch(uint32_t addr) {
    addr = (addr & 0x1FFFFFFFu) | 0x80000000u;
    sh4fn f = dc_lookup(addr);
    if (!f || nsw == MAX_SW) { fprintf(stderr, "dc-core: cannot hook task switch at %08X\n", addr); return; }
    sw[nsw].addr = addr; sw[nsw].orig = f; nsw++;
    dc_hle_register(addr, h_switch);
}

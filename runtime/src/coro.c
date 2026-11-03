/* dc-core: guest setjmp/longjmp and cooperative threads on host coroutines (see coro.h). */
/* Host context switching, one of:
 *   Windows            fibers
 *   AArch64 / x86-64   a small register switch in assembly (Switch / libnx has no ucontext); used
 *                      where ucontext is missing, or everywhere with -DDCCORE_CORO_ASM
 *   other POSIX        ucontext (Linux, macOS) */
#if defined(_WIN32)
#define CORO_FIBER 1
#elif defined(DCCORE_CORO_ASM) || defined(__SWITCH__) || (defined(__aarch64__) && !defined(__linux__) && !defined(__APPLE__))
#define CORO_ASM 1
#else
#define CORO_UCONTEXT 1
#if defined(__APPLE__) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 600      /* ucontext on macOS */
#endif
#if !defined(__APPLE__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#endif
#include "dccore/coro.h"
#include "dccore/runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#if CORO_FIBER
#include <windows.h>
#elif CORO_UCONTEXT
#include <ucontext.h>
#endif

/* ---- host contexts ---- */
#if CORO_FIBER
typedef struct { LPVOID fiber; } hctx;
static void hctx_switch(hctx *from, hctx *to) {
    if (!from->fiber) from->fiber = GetCurrentFiber();
    SwitchToFiber(to->fiber);
}
static VOID CALLBACK fiber_entry(LPVOID p) { ((void (*)(void))p)(); }
static void hctx_make(hctx *k, void **stack, size_t bytes, void (*fn)(void)) {
    (void)stack;
    if (k->fiber) DeleteFiber(k->fiber);
    k->fiber = CreateFiber(bytes, fiber_entry, (LPVOID)fn);
    if (!k->fiber) { fprintf(stderr, "dc-core: CreateFiber failed\n"); abort(); }
}
static void hctx_main_init(hctx *m) {
    if (!m->fiber) {
        m->fiber = ConvertThreadToFiber(NULL);
        if (!m->fiber) m->fiber = GetCurrentFiber();       /* already a fiber */
    }
}
#elif CORO_ASM
typedef struct { void *sp; } hctx;
void dccore_ctx_switch(void **save_sp, void *new_sp);      /* below */
#if defined(__APPLE__) || defined(_WIN32)
#define CORO_SYM "_dccore_ctx_switch"
#else
#define CORO_SYM "dccore_ctx_switch"
#endif
#if defined(__aarch64__)
/* callee-saved: x19-x30, d8-d15 (176 bytes, 16-aligned) */
__asm__(".text\n.p2align 2\n.globl " CORO_SYM "\n" CORO_SYM ":\n"
        "sub sp, sp, #176\n"
        "stp x19, x20, [sp, #0]\n stp x21, x22, [sp, #16]\n stp x23, x24, [sp, #32]\n"
        "stp x25, x26, [sp, #48]\n stp x27, x28, [sp, #64]\n stp x29, x30, [sp, #80]\n"
        "stp d8, d9, [sp, #96]\n stp d10, d11, [sp, #112]\n stp d12, d13, [sp, #128]\n stp d14, d15, [sp, #144]\n"
        "mov x9, sp\n str x9, [x0]\n mov sp, x1\n"
        "ldp x19, x20, [sp, #0]\n ldp x21, x22, [sp, #16]\n ldp x23, x24, [sp, #32]\n"
        "ldp x25, x26, [sp, #48]\n ldp x27, x28, [sp, #64]\n ldp x29, x30, [sp, #80]\n"
        "ldp d8, d9, [sp, #96]\n ldp d10, d11, [sp, #112]\n ldp d12, d13, [sp, #128]\n ldp d14, d15, [sp, #144]\n"
        "add sp, sp, #176\n ret\n");
static void *frame_init(char *top, void (*fn)(void)) {
    uint64_t *f = (uint64_t *)(top - 176);
    for (int i = 0; i < 22; i++) f[i] = 0;
    f[11] = (uint64_t)(uintptr_t)fn;                       /* x30: "return" into the entry */
    return f;
}
#elif defined(__x86_64__) && !defined(_WIN32)
/* System V: rbx, rbp, r12-r15 */
__asm__(".text\n.globl " CORO_SYM "\n" CORO_SYM ":\n"
        "pushq %rbp\n pushq %rbx\n pushq %r12\n pushq %r13\n pushq %r14\n pushq %r15\n"
        "movq %rsp, (%rdi)\n movq %rsi, %rsp\n"
        "popq %r15\n popq %r14\n popq %r13\n popq %r12\n popq %rbx\n popq %rbp\n ret\n");
static void *frame_init(char *top, void (*fn)(void)) {
    uint64_t *f = (uint64_t *)(top - 64);                  /* 6 registers, return address, pad */
    for (int i = 0; i < 8; i++) f[i] = 0;
    f[6] = (uint64_t)(uintptr_t)fn;                        /* rsp = 8 mod 16 at entry, like after a call */
    return f;
}
#else
#error "dc-core: no coroutine backend for this CPU (add one to coro.c)"
#endif
static void hctx_switch(hctx *from, hctx *to) { dccore_ctx_switch(&from->sp, to->sp); }
static void hctx_make(hctx *k, void **stack, size_t bytes, void (*fn)(void)) {
    if (!*stack) *stack = malloc(bytes);
    if (!*stack) { fprintf(stderr, "dc-core: out of memory for a coroutine stack\n"); abort(); }
    k->sp = frame_init((char *)(((uintptr_t)*stack + bytes) & ~(uintptr_t)15), fn);
}
static void hctx_main_init(hctx *m) { (void)m; }
#else
typedef struct { ucontext_t uc; } hctx;
static void hctx_switch(hctx *from, hctx *to) { swapcontext(&from->uc, &to->uc); }
static void hctx_make(hctx *k, void **stack, size_t bytes, void (*fn)(void)) {
    if (!*stack) *stack = malloc(bytes);
    if (!*stack) { fprintf(stderr, "dc-core: out of memory for a coroutine stack\n"); abort(); }
    getcontext(&k->uc);
    k->uc.uc_stack.ss_sp = *stack; k->uc.uc_stack.ss_size = bytes; k->uc.uc_link = NULL;
    makecontext(&k->uc, fn, 0);
}
static void hctx_main_init(hctx *m) { (void)m; }
#endif

extern sh4ctx *dc_current_ctx;    /* dispatch.c */
uint32_t dc_coro_setjmp_addr;   /* 0 = feature off: DC_CALL_SETJMP is a plain call */
int dc_coro_trace;

typedef struct coro {
    hctx uc;
    void *stack;
    struct slot *pending;        /* set by whoever switches here: longjmp to this slot on resume */
    int refs;                    /* live slots owned by this coroutine */
    uint32_t sp_key;             /* suspended in a stack switch: resumed when a task with this r15 is loaded */
    int sp_waiting;
    uint32_t resume_pc;          /* stack switch (rts / jump): where the task that resumed us said to go on (0 = rts) */
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

static jmp_buf *leave_jb;             /* dc_coro_reset: where the main stack continues */
static uint32_t lj_hooked;            /* the longjmp address dc_coro_install hooked */
static void switch_to(coro *to) {
    coro *from = cur;
    cur = to;
    hctx_switch(&from->uc, &to->uc);
    cur = from;
    if (leave_jb && from == &main_coro) { jmp_buf *j = leave_jb; leave_jb = NULL; DC_LONGJMP(*j, 1); }
    /* resumed: someone longjmp'd into one of our setjmps */
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
    }
    hctx_main_init(&main_coro.uc);
    hctx_make(&k->uc, &k->stack, stack_bytes, coro_main);
    k->pending = NULL; k->refs = 0; k->started = 1; k->sp_waiting = 0; k->resume_pc = 0;
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
    lj_hooked = lj_addr;
}

/* ---- task switches that swap stacks and end with rte (dc_coro_add_switch) ----
 * The routine saves the running task's registers on its stack, stores that stack pointer, loads
 * another task's and returns into it with rte (PC = spc). A task calls it like a function, so it
 * is resumed with the r15 it called with; that r15 identifies its suspended host coroutine. */
#define MAX_SW 8
static struct { uint32_t addr; sh4fn orig; int kind; } sw[MAX_SW];   /* kind: 0 rte, 1 stack switch, 2 stack start */
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

static void add_switch(uint32_t addr, int kind, sh4fn handler) {
    addr = (addr & 0x1FFFFFFFu) | 0x80000000u;
    sh4fn f = dc_lookup(addr);
    if (!f || nsw == MAX_SW) { fprintf(stderr, "dc-core: cannot hook task switch at %08X\n", addr); return; }
    sw[nsw].addr = addr; sw[nsw].orig = f; sw[nsw].kind = kind; nsw++;
    dc_hle_register(addr, handler);
}
void dc_coro_add_switch(uint32_t addr) { add_switch(addr, 0, h_switch); }

/* ---- task switches that swap stacks and go on with rts or a jump (dc_coro_add_stack_switch) ----
 * Like a scheduler that saves its registers, stores its r15, loads a task's r15 and returns into the
 * task with rts, while the task's yield saves the task, loads the scheduler's r15 back and jumps into
 * the scheduler's loop (Dead or Alive 2's task system). The routine's own code does the switch; then
 * the host switches to the coroutine waiting on the new r15 (or starts one at the continuation for a
 * new task) and tells it where to go on: the routine's jump target, or 0 for rts to the restored pr. */
static uint32_t h_stack_switch(sh4ctx *c) {
    uint32_t at = c->entry, r15 = c->r[15];
    sh4fn orig = NULL; int start = 0;
    for (int i = 0; i < nsw; i++) if (sw[i].addr == at) { orig = sw[i].orig; start = sw[i].kind == 2; }
    uint32_t next = orig(c);                 /* stacks swapped; next: a jump on the other stack (0: rts to pr) */
    uint32_t d = c->r[15] > r15 ? c->r[15] - r15 : r15 - c->r[15];
    if (d <= 256) return next;               /* stayed on this stack */
    coro *to = NULL;
    if (start) {                             /* a new task on that stack: whatever waited there has ended */
        for (int i = -1; i < ncoros; i++) {
            coro *k = i < 0 ? &main_coro : &coros[i];
            if (k == cur || !k->sp_waiting) continue;
            uint32_t e = k->sp_key > c->r[15] ? k->sp_key - c->r[15] : c->r[15] - k->sp_key;
            if (e <= 256 && !k->is_main) k->sp_waiting = 0;
        }
    } else to = waiting_on(c->r[15]);
    cur->sp_key = r15; cur->sp_waiting = 1;
    if (!to) {
        start_pc = next ? next : c->pr;
        to = new_coro();
        if (dc_coro_trace) fprintf(stderr, "coro: stack switch %08X starts coro %ld at %08X (r15 %08X)\n",
                                   at, (long)(to - coros), start_pc, c->r[15]);
    } else {
        to->resume_pc = next;
        if (dc_coro_trace) fprintf(stderr, "coro: stack switch %08X resumes %s%ld (key %08X, r15 %08X, at %08X)\n",
                                   at, to->is_main ? "main" : "coro", to->is_main ? 0L : (long)(to - coros), to->sp_key,
                                   c->r[15], next ? next : c->pr);
    }
    to->sp_waiting = 0;
    switch_to(to);
    /* resumed: the routine that switched back loaded our registers and said where to go on */
    uint32_t pc = cur->resume_pc; cur->resume_pc = 0;
    return pc;
}
void dc_coro_add_stack_switch(uint32_t addr, int starts_task) { add_switch(addr, starts_task ? 2 : 1, h_stack_switch); }

/* ---- leaving a whole executable (dc_coro_reset) */
void dc_coro_reset(jmp_buf *jb) {
    for (int i = 0; i < nsw; i++) dc_hle_register(sw[i].addr, NULL);
    nsw = 0;
    if (lj_hooked) dc_hle_register(lj_hooked, NULL);
    lj_hooked = 0; longjmp_addr = 0; orig_longjmp = NULL; dc_coro_setjmp_addr = 0;
    for (int i = 0; i < MAX_SLOTS; i++) slots[i].used = 0;
    for (int i = 0; i < ncoros; i++) { coros[i].refs = 0; coros[i].sp_waiting = 0; coros[i].pending = NULL; }
    main_coro.refs = 0; main_coro.sp_waiting = 0; main_coro.pending = NULL;
    if (cur == &main_coro) DC_LONGJMP(*jb, 1);
    leave_jb = jb;                       /* on a coroutine: the main stack takes the jump (fibers can't share one) */
    coro *from = cur;
    cur = &main_coro;
    hctx_switch(&from->uc, &main_coro.uc);
    abort();                             /* not reached */
}

/* ---- plain fibers (dc_fiber_*, coro.h): host stacks for an HLE operating system's threads */
struct dc_fiber { hctx uc; void *stack; size_t bytes; void (*fn)(void *); void *arg; };
static dc_fiber fiber_main;
static dc_fiber *fiber_cur;
static void fiber_start(void) {
    dc_fiber *f = fiber_cur;
    f->fn(f->arg);
    fprintf(stderr, "dc-core: a fiber's function returned\n");
    abort();
}
dc_fiber *dc_fiber_current(void) {
    if (!fiber_cur) { hctx_main_init(&fiber_main.uc); fiber_cur = &fiber_main; }
    return fiber_cur;
}
dc_fiber *dc_fiber_new(void (*fn)(void *), void *arg, size_t stack_bytes) {
    dc_fiber_current();
    dc_fiber *f = (dc_fiber *)calloc(1, sizeof *f);
    if (!f) { fprintf(stderr, "dc-core: out of memory for a fiber\n"); abort(); }
    f->fn = fn; f->arg = arg; f->bytes = stack_bytes ? stack_bytes : (size_t)2 << 20;
    hctx_make(&f->uc, &f->stack, f->bytes, fiber_start);
    return f;
}
void dc_fiber_reset(dc_fiber *f, void (*fn)(void *), void *arg) {
    if (f == dc_fiber_current() || f == &fiber_main) { fprintf(stderr, "dc-core: dc_fiber_reset on the running fiber\n"); abort(); }
    f->fn = fn; f->arg = arg;
    hctx_make(&f->uc, &f->stack, f->bytes, fiber_start);
}
void dc_fiber_switch(dc_fiber *to) {
    dc_fiber *from = dc_fiber_current();
    if (from == to) return;
    fiber_cur = to;
    hctx_switch(&from->uc, &to->uc);
}

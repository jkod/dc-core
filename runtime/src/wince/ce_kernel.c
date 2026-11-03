/* dc-core Windows CE runtime: memory, kernel objects, threads and the scheduler, interrupts. */
#include "ce_int.h"
#include "dccore/hw.h"
#include <stdarg.h>

int ce_trace;
sh4ctx *ce_ctx;

void ce_trace_api(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[ce %6u t%u] ", ce_milliseconds(), ce_cur ? ce_cur->id : 0);
    vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}
void ce_unimpl(const char *what) {
    static char seen[64][48]; static int n;
    for (int i = 0; i < n; i++) if (!strcmp(seen[i], what)) return;
    if (n < 64) snprintf(seen[n++], sizeof seen[0], "%s", what);
    fprintf(stderr, "[ce] not implemented: %s (pr=%08X)\n", what, ce_ctx ? ce_ctx->pr : 0);
}

/* ------------------------------------------------------------------ user memory */
uint8_t *ce_ptr(uint32_t va) {
    if (DC_IS_URAM(va)) return dc_uram + DC_URAM_OFS(va);
    if (DC_IS_RAM(va)) return dc_ram + DC_RAM_OFS(va);
    return NULL;
}
void ce_rd_block(void *dst, uint32_t va, uint32_t n) {
    uint8_t *p = ce_ptr(va);
    if (p && ce_ptr(va + n - 1)) { memcpy(dst, p, n); return; }
    for (uint32_t i = 0; i < n; i++) ((uint8_t *)dst)[i] = dc_rd8(va + i);
}
void ce_wr_block(uint32_t va, const void *src, uint32_t n) {
    uint8_t *p = ce_ptr(va);
    if (p && ce_ptr(va + n - 1)) { memcpy(p, src, n); return; }
    for (uint32_t i = 0; i < n; i++) dc_wr8(va + i, ((const uint8_t *)src)[i]);
}
int ce_wstr(uint32_t va, char *out, int n) {
    int i = 0;
    if (!va) { if (n) out[0] = 0; return 0; }
    for (; i < n - 1; i++) {
        uint16_t ch = dc_rd16(va + (uint32_t)i * 2);
        if (!ch) break;
        out[i] = ch < 256 ? (char)ch : '?';
    }
    out[i] = 0;
    return i;
}
int ce_wstrw(uint32_t va, uint16_t *out, int n) {
    int i = 0;
    for (; va && i < n - 1; i++) { out[i] = dc_rd16(va + (uint32_t)i * 2); if (!out[i]) break; }
    out[i] = 0;
    return i;
}
uint32_t ce_wstrlen(uint32_t va) { uint32_t n = 0; while (dc_rd16(va + n * 2)) n++; return n; }
void ce_put_wstr(uint32_t va, const char *s, int max) {
    int i = 0;
    for (; s[i] && i < max - 1; i++) dc_wr16(va + (uint32_t)i * 2, (uint8_t)s[i]);
    if (max > 0) dc_wr16(va + (uint32_t)i * 2, 0);
}

/* ------------------------------------------------------------------ heap: first fit over free extents,
 * sizes of live blocks in a hash table (host side, so the game can't corrupt them) */
static struct ext { uint32_t at, len; } *fr; static int nfr, capfr;
typedef struct { uint32_t va, size; } live_ent;
static live_ent *live; static uint32_t caplive, nlive;
static uint32_t heap_lo, heap_hi;

static void live_grow(void);
static uint32_t lh(uint32_t va) { return (va >> 4) * 2654435761u; }
static void live_put(uint32_t va, uint32_t size) {
    if ((nlive + 1) * 2 > caplive) live_grow();
    for (uint32_t i = lh(va) & (caplive - 1);; i = (i + 1) & (caplive - 1))
        if (!live[i].va) { live[i].va = va; live[i].size = size; nlive++; return; }
}
static void live_grow(void) {
    uint32_t oc = caplive; live_ent *ol = live;
    caplive = caplive ? caplive * 2 : 4096; nlive = 0;
    live = (live_ent *)calloc(caplive, sizeof *live);
    for (uint32_t i = 0; i < oc; i++) if (ol[i].va) live_put(ol[i].va, ol[i].size);
    free(ol);
}
static uint32_t live_take(uint32_t va, int remove) {
    if (!caplive) return 0;
    for (uint32_t i = lh(va) & (caplive - 1);; i = (i + 1) & (caplive - 1)) {
        if (!live[i].va) return 0;
        if (live[i].va == va) {
            uint32_t s = live[i].size;
            if (remove) {            /* backward-shift delete */
                uint32_t j = i;
                live[i].va = 0; nlive--;
                for (;;) {
                    j = (j + 1) & (caplive - 1);
                    if (!live[j].va) break;
                    uint32_t k = lh(live[j].va) & (caplive - 1);
                    if ((j > i && (k <= i || k > j)) || (j < i && (k <= i && k > j))) { live[i] = live[j]; live[j].va = 0; i = j; }
                }
            }
            return s;
        }
    }
}
void ce_heap_init(uint32_t lo, uint32_t hi) {
    heap_lo = lo; heap_hi = hi;
    capfr = 256; fr = malloc(sizeof *fr * (size_t)capfr); nfr = 1; fr[0].at = lo; fr[0].len = hi - lo;
}
void ce_heap_reserve(uint32_t lo, uint32_t hi) {
    for (int i = 0; i < nfr; i++) {
        uint32_t a = fr[i].at, e = fr[i].at + fr[i].len;
        if (hi <= a || lo >= e) continue;
        if (nfr + 1 >= capfr) { capfr *= 2; fr = realloc(fr, sizeof *fr * (size_t)capfr); }
        memmove(&fr[i + 1], &fr[i], sizeof *fr * (size_t)(nfr - i)); nfr++;
        fr[i].at = a; fr[i].len = lo > a ? lo - a : 0;
        fr[i + 1].at = hi < e ? hi : e; fr[i + 1].len = hi < e ? e - hi : 0;
        return;
    }
}
uint32_t ce_alloc(uint32_t size, uint32_t align) {
    if (!size) size = 1;
    size = (size + CE_HEAP_ALIGN - 1) & ~(CE_HEAP_ALIGN - 1);
    if (align < CE_HEAP_ALIGN) align = CE_HEAP_ALIGN;
    for (int i = 0; i < nfr; i++) {
        uint32_t a = (fr[i].at + align - 1) & ~(align - 1), pad = a - fr[i].at;
        if (fr[i].len < pad || fr[i].len - pad < size) continue;
        uint32_t rest_at = a + size, rest = fr[i].len - pad - size;
        if (pad) {
            fr[i].len = pad;
            if (rest) {
                if (nfr == capfr) { capfr *= 2; fr = realloc(fr, sizeof *fr * (size_t)capfr); }
                memmove(&fr[i + 2], &fr[i + 1], sizeof *fr * (size_t)(nfr - i - 1)); nfr++;
                fr[i + 1].at = rest_at; fr[i + 1].len = rest;
            }
        } else if (rest) { fr[i].at = rest_at; fr[i].len = rest; }
        else { memmove(&fr[i], &fr[i + 1], sizeof *fr * (size_t)(nfr - i - 1)); nfr--; }
        live_put(a, size);
        memset(dc_uram + DC_URAM_OFS(a), 0, size);
        return a;
    }
    fprintf(stderr, "[ce] out of memory: %u bytes\n", size);
    return 0;
}
uint32_t ce_alloc_size(uint32_t va) { return live_take(va, 0); }
void ce_free(uint32_t va) {
    uint32_t size = live_take(va, 1);
    if (!size) { if (va) TRACE("free of unknown block %08X", va); return; }
    int lo = 0, hi = nfr;
    while (lo < hi) { int m = (lo + hi) / 2; if (fr[m].at < va) lo = m + 1; else hi = m; }
    int i = lo;     /* insert before fr[i] */
    int prev = i > 0 && fr[i - 1].at + fr[i - 1].len == va, next = i < nfr && va + size == fr[i].at;
    if (prev && next) { fr[i - 1].len += size + fr[i].len; memmove(&fr[i], &fr[i + 1], sizeof *fr * (size_t)(nfr - i - 1)); nfr--; }
    else if (prev) fr[i - 1].len += size;
    else if (next) { fr[i].at = va; fr[i].len += size; }
    else {
        if (nfr == capfr) { capfr *= 2; fr = realloc(fr, sizeof *fr * (size_t)capfr); }
        memmove(&fr[i + 1], &fr[i], sizeof *fr * (size_t)(nfr - i)); nfr++;
        fr[i].at = va; fr[i].len = size;
    }
}
uint32_t ce_realloc(uint32_t va, uint32_t size) {
    if (!va) return ce_alloc(size, 0);
    uint32_t old = ce_alloc_size(va);
    uint32_t n = ce_alloc(size, 0);
    if (n && old) memcpy(dc_uram + DC_URAM_OFS(n), dc_uram + DC_URAM_OFS(va), old < size ? old : size);
    ce_free(va);
    return n;
}

/* ------------------------------------------------------------------ handles */
#define MAXH 4096
static ce_obj objs[MAXH];
#define H_OF(i) (0x00001000u + (uint32_t)(i) * 4u)
uint32_t ce_handle_new(int type) {
    for (int i = 1; i < MAXH; i++) if (objs[i].type == OB_FREE) {
        memset(&objs[i], 0, sizeof objs[i]); objs[i].type = type; objs[i].refs = 1;
        return H_OF(i);
    }
    fprintf(stderr, "[ce] out of handles\n");
    return 0;
}
ce_obj *ce_handle(uint32_t h) {
    if (h < 0x1000u || (h & 3)) return NULL;
    uint32_t i = (h - 0x1000u) / 4;
    return i < MAXH && objs[i].type != OB_FREE ? &objs[i] : NULL;
}
void ce_handle_close(uint32_t h) {
    ce_obj *o = ce_handle(h);
    if (o && --o->refs <= 0) {
        if (o->type == OB_THREAD && o->thread && o->thread->state != TH_DEAD) { o->refs = 1; return; }   /* stays until it ends */
        free(o->data); o->type = OB_FREE;
    }
}

/* ------------------------------------------------------------------ threads */
#define MAXT 128
static ce_thread *threads[MAXT];
static int nthreads;
ce_thread *ce_cur;
static uint32_t next_tid = 0x10;
uint64_t ce_now(void) { return ce_ctx->cycles; }
uint32_t ce_milliseconds(void) { return ce_ctx ? (uint32_t)(ce_ctx->cycles / CE_CYCLES_PER_MS) : 0; }

static void kdata_update(void) {
    /* PUserKData: lpvTls at +0, SYSHANDLE_OFFSET 4: [SH_WIN32, SH_CURTHREAD, SH_CURPROC] */
    dc_wr32(CE_KDATA + 0x004 + 1 * 4, ce_cur ? ce_cur->handle : 0);
    dc_wr32(CE_KDATA + 0x004 + 2 * 4, 0x00000042u);
    if (ce_cur) dc_wr32(CE_KDATA, 0x00005000u + (ce_cur->id & 0x3F) * 0);   /* TLS through TlsGetValue only */
}

static void save_regs(ce_thread *t) { uint64_t cy = ce_ctx->cycles; t->regs = *ce_ctx; t->regs.cycles = cy; }
static void load_regs(ce_thread *t) { uint64_t cy = ce_ctx->cycles; *ce_ctx = t->regs; ce_ctx->cycles = cy; }

static void switch_to(ce_thread *t) {
    ce_thread *from = ce_cur;
    if (from == t) { t->state = TH_RUNNING; return; }
    if (ce_trace > 1) TRACE("switch t%u -> t%u", from ? from->id : 0, t->id);
    if (from) save_regs(from);
    load_regs(t);
    ce_cur = t; t->state = TH_RUNNING; t->last_run = ce_now();
    kdata_update();
    dc_fiber_switch(t->fiber);
    /* back: whoever switched to us loaded our registers */
}

static void thread_main(void *arg) {
    ce_thread *t = (ce_thread *)arg;
    dc_call(ce_ctx, t->start);
    /* returning from the thread function = ExitThread(r0) */
    extern void ce_exit_thread(uint32_t code);
    ce_exit_thread(ce_ctx->r[0]);
}

ce_thread *ce_thread_create(uint32_t start, uint32_t param, uint32_t stack, int suspended) {
    if (nthreads == MAXT) { fprintf(stderr, "[ce] too many threads\n"); return NULL; }
    ce_thread *t = calloc(1, sizeof *t);
    if (!stack) stack = 0x10000;
    if (stack < 0x10000) stack = 0x10000;
    t->stack_lo = ce_alloc(stack, 16); t->stack_hi = t->stack_lo + stack;
    t->id = next_tid++; t->start = start; t->param = param;
    t->prio = 3;                    /* THREAD_PRIORITY_NORMAL */
    t->handle = ce_handle_new(OB_THREAD);
    ce_handle(t->handle)->thread = t;
    ce_handle(t->handle)->refs = 2;                     /* the caller's handle + the thread itself */
    memset(&t->regs, 0, sizeof t->regs);
    t->regs.r[4] = param; t->regs.r[15] = t->stack_hi - 32;
    t->regs.sr_rest = 0; t->regs.fpscr = 0x00040001u; t->regs.vbr = CE_VBR;
    t->regs.pr = CE_THREAD_EXIT;
    t->fiber = dc_fiber_new(thread_main, t, 0);
    t->state = suspended ? TH_SUSPENDED : TH_READY; t->suspend = suspended ? 1 : 0;
    t->wake_at = ~0ull;
    threads[nthreads++] = t;
    TRACE("CreateThread %08X(%08X) -> t%u handle %08X%s", start, param, t->id, t->handle, suspended ? " suspended" : "");
    return t;
}

/* a waiting thread's wait: satisfiable now? acquire = take ownership / reset auto events */
static int obj_ready(ce_obj *o, ce_thread *t) {
    if (!o) return 0;
    switch (o->type) {
    case OB_EVENT: case OB_THREAD: case OB_PROCESS: return o->signaled;
    case OB_MUTEX: return !o->owner || o->owner == t;
    case OB_SEM: return o->count > 0;
    default: return 1;
    }
}
static void obj_acquire(ce_obj *o, ce_thread *t) {
    if (o->type == OB_EVENT && !o->manual) o->signaled = 0;
    else if (o->type == OB_MUTEX) { o->owner = t; o->count++; }
    else if (o->type == OB_SEM) o->count--;
}
static int wait_check(ce_thread *t) {
    if (t->nwait == 0) return t->wake_at <= ce_now();
    if (t->wait_all) {
        for (int i = 0; i < t->nwait; i++) if (!obj_ready(ce_handle(t->waits[i]), t)) goto timeout;
        for (int i = 0; i < t->nwait; i++) obj_acquire(ce_handle(t->waits[i]), t);
        t->wait_result = WAIT_OBJECT_0; return 1;
    }
    for (int i = 0; i < t->nwait; i++) {
        ce_obj *o = ce_handle(t->waits[i]);
        if (!o) { t->wait_result = WAIT_FAILED; return 1; }
        if (obj_ready(o, t)) { obj_acquire(o, t); t->wait_result = WAIT_OBJECT_0 + (uint32_t)i; return 1; }
    }
timeout:
    if (t->wake_at <= ce_now()) { t->wait_result = WAIT_TIMEOUT; return 1; }
    return 0;
}

static int runnable(ce_thread *t) {
    if (t->state == TH_READY || t->state == TH_RUNNING) return 1;
    if (t->state == TH_WAITING || t->state == TH_SLEEPING) {
        if (wait_check(t)) { t->state = TH_READY; return 1; }
    }
    return 0;
}

static ce_thread *pick(void) {
    ce_thread *best = NULL;
    for (int i = 0; i < nthreads; i++) {
        ce_thread *t = threads[i];
        if (!runnable(t)) continue;
        if (!best || t->prio < best->prio || (t->prio == best->prio && t->last_run < best->last_run)) best = t;
    }
    return best;
}

static int in_idle, process_exit;
static uint32_t exit_code;
static dc_fiber *main_fiber;

/* nothing can run: let time pass (the hardware model raises the interrupts that wake drivers) */
static void idle(void) {
    uint64_t wake = ~0ull;
    for (int i = 0; i < nthreads; i++) {
        ce_thread *t = threads[i];
        if ((t->state == TH_WAITING || t->state == TH_SLEEPING) && t->wake_at < wake) wake = t->wake_at;
    }
    uint64_t to = wake < dc_next_event ? wake : dc_next_event;
    if (to == ~0ull) to = ce_now() + CE_CYCLES_PER_MS;
    if (to <= ce_now()) to = ce_now() + 2000;         /* the hardware has an event now: still let time pass */
    ce_ctx->cycles = to;
    in_idle = 1;
    dc_events(ce_ctx);
    in_idle = 0;
}

/* run whatever should run now; returns when the calling thread runs again */
static void schedule(void) {
    for (;;) {
        if (process_exit) { dc_fiber_switch(main_fiber); }
        ce_thread *t = pick();
        if (t) { switch_to(t); return; }
        int alive = 0;
        for (int i = 0; i < nthreads; i++) if (threads[i]->state != TH_DEAD) alive = 1;
        if (!alive) { process_exit = 1; continue; }
        idle();
    }
}

uint32_t ce_wait(uint32_t *handles, int n, int all, uint32_t ms) {
    ce_thread *t = ce_cur;
    if (n > CE_MAXWAIT) n = CE_MAXWAIT;
    memcpy(t->waits, handles, sizeof *handles * (size_t)n); t->nwait = n; t->wait_all = all;
    t->wake_at = ms == INFINITE ? ~0ull : ce_now() + (uint64_t)ms * CE_CYCLES_PER_MS;
    if (wait_check(t)) { t->nwait = 0; return t->wait_result; }
    if (ms == 0) { t->nwait = 0; return WAIT_TIMEOUT; }
    t->state = TH_WAITING;
    schedule();
    t->nwait = 0;
    return t->wait_result;
}
void ce_sleep(uint32_t ms) {
    ce_thread *t = ce_cur;
    t->nwait = 0;
    t->wake_at = ce_now() + (uint64_t)ms * CE_CYCLES_PER_MS;
    t->state = ms ? TH_SLEEPING : TH_READY;
    t->last_run = ce_now();              /* Sleep(0): behind the other threads of its priority */
    schedule();
}
void ce_signal(uint32_t h) {
    ce_obj *o = ce_handle(h);
    if (o) o->signaled = 1;
}
/* after something became ready: a thread of higher priority runs now (CE is preemptive) */
void ce_reschedule(void) {
    ce_thread *t = pick();
    if (t && t != ce_cur && t->prio < ce_cur->prio) { ce_cur->state = TH_READY; schedule(); }
}

void ce_exit_thread(uint32_t code) {
    ce_thread *t = ce_cur;
    TRACE("ExitThread t%u code %u", t->id, code);
    t->state = TH_DEAD; t->exit_code = code;
    ce_obj *o = ce_handle(t->handle);
    if (o) { o->signaled = 1; ce_handle_close(t->handle); }
    if (t->id == 0x10) { process_exit = 1; exit_code = code; }    /* the main thread ends the process */
    schedule();
    abort();   /* not reached */
}
void ce_exit_process(uint32_t code) {
    process_exit = 1; exit_code = code;
    schedule();
}
ce_thread *ce_thread_of(uint32_t h) {
    ce_obj *o = ce_handle(h);
    if (o && o->type == OB_THREAD) return o->thread;
    if (h == 0xFFFFFFFEu) return ce_cur;   /* GetCurrentThread() pseudo handle */
    for (int i = 0; i < nthreads; i++) if (threads[i]->handle == h || threads[i]->id == h) return threads[i];
    return NULL;
}
void ce_thread_resume(ce_thread *t) {
    if (t->suspend > 0 && --t->suspend == 0 && t->state == TH_SUSPENDED) t->state = TH_READY;
}

/* ------------------------------------------------------------------ interrupts (the OEM layer).
 * Drivers wait on an event that InterruptInitialize ties to a SYSINTR; InterruptDone re-enables the
 * source. The Dreamcast OAL's SYSINTRs (from the table in the console's NK.EXE): SYSINTR_FIRMWARE (16)
 * + n is Holly source group n, given as status register (0 ISTNRM, 1 ISTEXT, 2 ISTERR) and bits. */
static const struct { int bank; uint32_t bits; } holly_src[] = {
    { 0, 0x00080FFFu },    /* 16 PowerVR: render done, vblank, hblank, TA lists, PVR DMA, ch2 DMA */
    { 2, 0x000000FFu },    /* 17 PowerVR errors */
    { 0, 0x00003000u },    /* 18 Maple DMA end, Maple vblank over */
    { 2, 0x00000F00u },    /* 19 Maple errors */
    { 1, 0x00000001u },    /* 20 GD-ROM */
    { 0, 0x00004000u },    /* 21 GD DMA end */
    { 2, 0x00007000u },    /* 22 GD errors */
    { 1, 0x00000002u },    /* 23 AICA */
    { 0, 0x00008000u },    /* 24 AICA DMA end */
    { 2, 0x00888000u },    /* 25 G2 errors */
    { 1, 0x00000004u },    /* 26 modem */
    { 1, 0x00000008u },    /* 27 expansion */
};
#define CE_SYSINTR_FIRST 16u
#define MAXI 32
static struct { uint32_t sysintr, event, seen; int enabled; } irqs[MAXI];
static int nirqs;
static uint32_t sysintr_mask(uint32_t s, int *bank) {
    if (s < CE_SYSINTR_FIRST || s >= CE_SYSINTR_FIRST + sizeof holly_src / sizeof *holly_src) return 0;
    *bank = holly_src[s - CE_SYSINTR_FIRST].bank;
    return holly_src[s - CE_SYSINTR_FIRST].bits;
}
static void iml_update(void) {
    uint32_t m[3] = { 0, 0, 0 };
    for (int i = 0; i < nirqs; i++) if (irqs[i].enabled) {
        int b; uint32_t bits = sysintr_mask(irqs[i].sysintr, &b);
        if (bits) m[b] |= bits;
    }
    dc_wr32(0xA05F6930u, m[0]); dc_wr32(0xA05F6934u, m[1]); dc_wr32(0xA05F6938u, m[2]);   /* IML6NRM/EXT/ERR */
}
int ce_irq_attach(uint32_t sysintr, uint32_t event) {
    int i;
    for (i = 0; i < nirqs; i++) if (irqs[i].sysintr == sysintr) break;
    if (i == nirqs) { if (nirqs == MAXI) return 0; nirqs++; }
    irqs[i].sysintr = sysintr; irqs[i].event = event; irqs[i].enabled = 1;
    int b; if (!sysintr_mask(sysintr, &b)) TRACE("InterruptInitialize: unknown SYSINTR %u", sysintr);
    iml_update();
    return 1;
}
void ce_irq_done(uint32_t sysintr) {
    for (int i = 0; i < nirqs; i++) if (irqs[i].sysintr == sysintr) irqs[i].enabled = 1;
    iml_update();
}
uint32_t ce_irq_take(uint32_t sysintr) {
    for (int i = 0; i < nirqs; i++) if (irqs[i].sysintr == sysintr) { uint32_t v = irqs[i].seen; irqs[i].seen = 0; return v; }
    return 0;
}
void ce_irq_disable(uint32_t sysintr) {
    for (int i = 0; i < nirqs; i++) if (irqs[i].sysintr == sysintr) irqs[i].enabled = 0;
    iml_update();
}
static int need_resched;
static uint32_t h_vector(sh4ctx *c) {
    uint32_t evt = dc_rd32(0xFF000028u);          /* INTEVT */
    if (evt >= 0x400 && evt <= 0x440) {           /* TMU0-2 underflow: no driver here uses them; acknowledge */
        uint32_t tcr = 0xFFD80010u + (evt - 0x400) / 0x20 * 12;
        dc_wr16(tcr, (uint16_t)(dc_rd16(tcr) & ~0x0120u));    /* UNF off, UNIE off */
        TRACE("TMU interrupt %03X: disabled", evt);
        sh4_set_sr(c, c->ssr);
        return 0;
    }
    if (evt >= 0x640) {                           /* DMAC: the drivers poll the channels */
        TRACE("DMAC interrupt %03X", evt);
        for (int ch = 0; ch < 4; ch++) { uint32_t chcr = 0xFFA0000Cu + (uint32_t)ch * 0x10; dc_wr32(chcr, dc_rd32(chcr) & ~4u); }
        sh4_set_sr(c, c->ssr);
        return 0;
    }
    uint32_t n = dc_rd32(0xA05F6900u), e = dc_rd32(0xA05F6904u), r = dc_rd32(0xA05F6908u);
    uint32_t pend[3] = { n, e, r };
    int any = 0;
    for (int i = 0; i < nirqs; i++) {
        int b; uint32_t bit = sysintr_mask(irqs[i].sysintr, &b);
        if (irqs[i].enabled && bit && (pend[b] & bit)) {
            /* like the OAL: remember which sources fired (KernelIoControl 0x10000 hands them to the
             * driver), acknowledge them, and mask the SYSINTR until InterruptDone */
            irqs[i].seen |= pend[b] & bit;
            if (b != 1) dc_wr32(b == 0 ? 0xA05F6900u : 0xA05F6908u, pend[b] & bit);
            irqs[i].enabled = 0;
            ce_signal(irqs[i].event);
            any = 1;
        }
    }
    iml_update();
    /* rte */
    sh4_set_sr(c, c->ssr);
    if (any && !in_idle && ce_cur) {
        ce_thread *t = pick();
        if (t && t != ce_cur && t->prio < ce_cur->prio) need_resched = 1;
    }
    /* time slice: another thread of the same priority has waited long enough */
    if (!in_idle && ce_cur && ce_now() - ce_cur->last_run > 100 * CE_CYCLES_PER_MS) need_resched = 1;
    if (need_resched) c->spc = CE_RESCHED;    /* runs after the interrupt, as the interrupted thread */
    return 0;
}
static uint32_t h_resched(sh4ctx *c) {
    (void)c;
    need_resched = 0;
    if (ce_cur) { ce_cur->state = TH_READY; ce_cur->last_run = ce_now(); schedule(); }
    return 0;
}
static uint32_t h_thread_exit(sh4ctx *c) { ce_exit_thread(c->r[0]); return 0; }
static uint32_t h_exception(sh4ctx *c) {
    fprintf(stderr, "[ce] CPU exception (EXPEVT %08X) at %08X\n", dc_rd32(0xFF000024u), c->spc);
    sh4_set_sr(c, c->ssr);
    return 0;
}

void ce_irq_init(void) {
    dc_hle_register(CE_VBR + 0x600, h_vector);
    dc_hle_register(CE_VBR + 0x100, h_exception);
    dc_hle_register(CE_VBR + 0x400, h_exception);
    dc_hle_register(CE_THREAD_EXIT, h_thread_exit);
    dc_hle_register(CE_RESCHED, h_resched);
}

/* debugging: the threads and what they wait for (DC_CE_DUMP=1, at exit) */
void ce_dump_threads(void) {
    static const char *st[] = { "ready", "running", "waiting", "sleeping", "suspended", "dead" };
    for (int i = 0; i < nthreads; i++) {
        ce_thread *t = threads[i];
        fprintf(stderr, "[ce] t%u prio %d %s start %08X pr %08X r15 %08X", t->id, t->prio, st[t->state], t->start, t->regs.pr, t->regs.r[15]);
        for (int k = 0; k < t->nwait; k++) {
            ce_obj *o = ce_handle(t->waits[k]);
            fprintf(stderr, " wait %08X(%d %s sig %d)", t->waits[k], o ? o->type : -1, o ? o->name : "", o ? o->signaled : 0);
        }
        fputc('\n', stderr);
    }
}

/* ------------------------------------------------------------------ process start */
int ce_run_main(sh4ctx *c, uint32_t entry) {
    main_fiber = dc_fiber_current();
    if (getenv("DC_CE_DUMP")) atexit(ce_dump_threads);
    ce_thread_create(entry, 0, 0x40000, 0);
    (void)c;
    schedule();
    /* back on the main fiber: the process ended */
    return (int)exit_code;
}

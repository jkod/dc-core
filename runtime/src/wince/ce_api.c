/* dc-core Windows CE runtime: native versions of COREDLL's kernel calls (threads, synchronisation,
 * memory, modules, files, time, the OEM layer). Everything else in COREDLL -- the C runtime, wsprintf,
 * string and 64-bit helpers -- is the disc's own code, recompiled. */
#include "ce_int.h"
#include "dccore/hw.h"
#include "dccore/disc.h"
#include "dccore/bios.h"
#include <stdarg.h>
#include <ctype.h>

#define RET(v) do { c->r[0] = (uint32_t)(v); return 0; } while (0)
#define FN(name) static uint32_t k_##name(sh4ctx *c)

static void set_error(uint32_t e) { if (ce_cur) ce_cur->last_error = e; }
static uint32_t wdm_proc(const char *name);
static uint32_t cdrom_ioctl(sh4ctx *c);

/* ------------------------------------------------------------------ calling guest code */
uint32_t ce_call(uint32_t fn, int n, ...) {
    sh4ctx *c = ce_ctx;
    uint32_t a[12] = { 0 }, sp = c->r[15], pr = c->pr, args[4] = { c->r[4], c->r[5], c->r[6], c->r[7] };
    va_list ap; va_start(ap, n);
    for (int i = 0; i < n && i < 12; i++) a[i] = va_arg(ap, uint32_t);
    va_end(ap);
    uint32_t frame = 16 + (n > 4 ? (uint32_t)(n - 4) * 4 : 0);
    c->r[15] = (sp - frame - 8) & ~7u;
    for (int i = 4; i < n; i++) dc_wr32(c->r[15] + 16 + (uint32_t)(i - 4) * 4, a[i]);
    for (int i = 0; i < 4; i++) c->r[4 + i] = a[i];
    dc_call(c, fn);
    uint32_t r = c->r[0];
    /* the native caller keeps reading its own arguments (ARG) afterwards */
    c->r[15] = sp; c->pr = pr;
    for (int i = 0; i < 4; i++) c->r[4 + i] = args[i];
    return r;
}

/* ------------------------------------------------------------------ threads */
FN(CreateThread) {        /* (sec, stack, start, param, flags, &tid) */
    uint32_t flags = ARG(4), tidp = ARG(5);
    ce_thread *t = ce_thread_create(ARG(2), ARG(3), (flags & 0x10000) ? ARG(1) : 0, (flags & 4) != 0);   /* CREATE_SUSPENDED */
    if (!t) RET(0);
    if (tidp) dc_wr32(tidp, t->id);
    t->prio = ce_cur ? ce_cur->prio : 3;
    RET(t->handle);
}
FN(ExitThread) { ce_exit_thread(ARG(0)); RET(0); }
FN(TerminateThread) {
    ce_thread *t = ce_thread_of(ARG(0));
    if (!t) RET(0);
    if (t == ce_cur) ce_exit_thread(ARG(1));
    TRACE("TerminateThread t%u", t->id);
    t->state = TH_DEAD; t->exit_code = ARG(1);
    ce_obj *o = ce_handle(t->handle); if (o) o->signaled = 1;
    RET(1);
}
FN(ResumeThread) { ce_thread *t = ce_thread_of(ARG(0)); if (!t) RET(0xFFFFFFFFu); uint32_t p = (uint32_t)t->suspend; ce_thread_resume(t); ce_reschedule(); RET(p); }
FN(SuspendThread) { ce_thread *t = ce_thread_of(ARG(0)); if (!t) RET(0xFFFFFFFFu); uint32_t p = (uint32_t)t->suspend++; if (t != ce_cur) t->state = TH_SUSPENDED; RET(p); }
FN(SetThreadPriority) {
    ce_thread *t = ce_thread_of(ARG(0));
    if (!t) RET(0);
    TRACE("SetThreadPriority t%u %d", t->id, (int)ARG(1));
    t->prio = (int)ARG(1);
    ce_reschedule();
    RET(1);
}
FN(GetThreadPriority) { ce_thread *t = ce_thread_of(ARG(0)); RET(t ? (uint32_t)t->prio : 0x7FFFFFFFu); }
FN(GetExitCodeThread) {
    ce_thread *t = ce_thread_of(ARG(0));
    ce_obj *o = ce_handle(ARG(0));
    if (!t && o && o->type == OB_THREAD) { dc_wr32(ARG(1), (uint32_t)o->count); RET(1); }   /* ended, slot reused */
    if (!t) RET(0);
    dc_wr32(ARG(1), t->state == TH_DEAD ? t->exit_code : 259u /* STILL_ACTIVE */);
    RET(1);
}
FN(Sleep) { ce_sleep(ARG(0)); RET(0); }
FN(SleepTillTick) { ce_sleep(1); RET(0); }
FN(WaitForSingleObject) {
    uint32_t h = ARG(0), r = ce_wait(&h, 1, 0, ARG(1));
    if (r == WAIT_FAILED) TRACE("WaitForSingleObject %08X: not a handle (pr %08X)", h, c->pr);
    RET(r);
}
FN(WaitForMultipleObjects) {
    uint32_t n = ARG(0), hs[CE_MAXWAIT];
    if (n > CE_MAXWAIT) n = CE_MAXWAIT;
    for (uint32_t i = 0; i < n; i++) hs[i] = dc_rd32(ARG(1) + i * 4);
    RET(ce_wait(hs, (int)n, (int)ARG(2), ARG(3)));
}
FN(GetCurrentThreadId) { RET(ce_cur->id); }

/* ------------------------------------------------------------------ events, mutexes, semaphores */
static uint32_t named(int type, uint32_t namep) {
    char nm[64]; ce_wstr(namep, nm, sizeof nm);
    if (nm[0]) {
        for (uint32_t h = 0x1000; h < 0x1000 + 4096 * 4; h += 4) {
            ce_obj *o = ce_handle(h);
            if (o && o->type == type && !strcmp(o->name, nm)) { o->refs++; set_error(183); return h; }   /* ERROR_ALREADY_EXISTS */
        }
    }
    uint32_t h = ce_handle_new(type);
    if (h) snprintf(ce_handle(h)->name, sizeof ce_handle(h)->name, "%s", nm);
    set_error(0);
    return h;
}
FN(CreateEventW) {        /* (sec, manual, initial, name) */
    uint32_t h = named(OB_EVENT, ARG(3));
    ce_obj *o = ce_handle(h);
    if (o && o->refs == 1) { o->manual = ARG(1) != 0; o->signaled = ARG(2) != 0; }
    TRACE("CreateEvent %s%s -> %08X", o && o->name[0] ? o->name : "", ARG(1) ? " manual" : "", h);
    RET(h);
}
FN(EventModify) {         /* (h, EVENT_PULSE 1 / RESET 2 / SET 3) */
    ce_obj *o = ce_handle(ARG(0));
    if (!o || o->type != OB_EVENT) RET(0);
    switch (ARG(1)) {
    case 3: o->signaled = 1; ce_reschedule(); break;
    case 2: o->signaled = 0; break;
    case 1: o->signaled = 1; ce_reschedule(); o->signaled = 0; break;
    }
    RET(1);
}
FN(CreateMutexW) {
    uint32_t h = named(OB_MUTEX, ARG(2));
    ce_obj *o = ce_handle(h);
    if (o && o->refs == 1 && ARG(1)) { o->owner = ce_cur; o->count = 1; }
    RET(h);
}
FN(ReleaseMutex) {
    ce_obj *o = ce_handle(ARG(0));
    if (!o || o->type != OB_MUTEX || o->owner != ce_cur) RET(0);
    if (--o->count <= 0) { o->owner = NULL; o->count = 0; ce_reschedule(); }
    RET(1);
}
FN(CreateSemaphoreW) {    /* (sec, initial, max, name) */
    uint32_t h = named(OB_SEM, ARG(3));
    ce_obj *o = ce_handle(h);
    if (o && o->refs == 1) { o->count = (int)ARG(1); o->max = (int)ARG(2); }
    RET(h);
}
FN(ReleaseSemaphore) {
    ce_obj *o = ce_handle(ARG(0));
    if (!o || o->type != OB_SEM) RET(0);
    if (ARG(2)) dc_wr32(ARG(2), (uint32_t)o->count);
    o->count += (int)ARG(1); if (o->count > o->max) o->count = o->max;
    ce_reschedule();
    RET(1);
}
FN(CloseHandle) {
    uint32_t h = ARG(0);
    ce_obj *o = ce_handle(h);
    if (!o) RET(0);
    if (o->type == OB_FILE && o->max == 1 && o->refs == 1) { uint32_t f = wdm_proc("CloseWDMDevice"), fo = o->lba; if (f) ce_call(f, 1, fo);
      if (dc_rd32(fo + 24) & 2) { ce_handle_close(dc_rd32(fo + 48)); ce_handle_close(dc_rd32(fo + 52)); }
      ce_call(wdm_proc("ObDereferenceObject"), 1, fo); }
    ce_handle_close(h);
    RET(1);
}

/* CRITICAL_SECTION { LockCount, OwnerThread, hCrit, needtrap (our waiter count), dwReserved } */
FN(InitializeCriticalSection) {
    uint32_t cs = ARG(0);
    dc_wr32(cs, 0); dc_wr32(cs + 4, 0); dc_wr32(cs + 12, 0); dc_wr32(cs + 16, 0);
    uint32_t h = ce_handle_new(OB_EVENT);
    dc_wr32(cs + 8, h);
    RET(0);
}
FN(DeleteCriticalSection) { ce_handle_close(dc_rd32(ARG(0) + 8)); dc_wr32(ARG(0) + 8, 0); RET(0); }
FN(EnterCriticalSection) {
    uint32_t cs = ARG(0), me = ce_cur->handle;
    for (;;) {
        uint32_t owner = dc_rd32(cs + 4);
        if (!owner || owner == me || !ce_handle(owner) || (ce_thread_of(owner) && ce_thread_of(owner)->state == TH_DEAD)) break;
        uint32_t ev = dc_rd32(cs + 8);
        if (!ce_handle(ev)) { ev = ce_handle_new(OB_EVENT); dc_wr32(cs + 8, ev); }
        dc_wr32(cs + 12, dc_rd32(cs + 12) + 1);
        ce_wait(&ev, 1, 0, INFINITE);
        dc_wr32(cs + 12, dc_rd32(cs + 12) - 1);
    }
    dc_wr32(cs + 4, me);
    dc_wr32(cs, dc_rd32(cs) + 1);
    RET(0);
}
FN(TryEnterCriticalSection) {
    uint32_t cs = ARG(0), me = ce_cur->handle, owner = dc_rd32(cs + 4);
    if (owner && owner != me) RET(0);
    dc_wr32(cs + 4, me); dc_wr32(cs, dc_rd32(cs) + 1);
    RET(1);
}
FN(LeaveCriticalSection) {
    uint32_t cs = ARG(0);
    if (dc_rd32(cs + 4) != ce_cur->handle) RET(0);
    uint32_t n = dc_rd32(cs) - 1;
    dc_wr32(cs, n);
    if (!n) {
        dc_wr32(cs + 4, 0);
        if (dc_rd32(cs + 12)) { ce_signal(dc_rd32(cs + 8)); ce_reschedule(); }
    }
    RET(0);
}
FN(InterlockedIncrement) { uint32_t v = dc_rd32(ARG(0)) + 1; dc_wr32(ARG(0), v); RET(v); }
FN(InterlockedDecrement) { uint32_t v = dc_rd32(ARG(0)) - 1; dc_wr32(ARG(0), v); RET(v); }
FN(InterlockedExchange) { uint32_t v = dc_rd32(ARG(0)); dc_wr32(ARG(0), ARG(1)); RET(v); }
FN(InterlockedTestExchange) { uint32_t v = dc_rd32(ARG(0)); if (v == ARG(1)) dc_wr32(ARG(0), ARG(2)); RET(v); }
FN(InterlockedCompareExchange) { uint32_t v = dc_rd32(ARG(0)); if (v == ARG(2)) dc_wr32(ARG(0), ARG(1)); RET(v); }
FN(InterlockedExchangeAdd) { uint32_t v = dc_rd32(ARG(0)); dc_wr32(ARG(0), v + ARG(1)); RET(v); }

/* TLS: TlsCall(TLS_FUNCALLOC 0 / TLS_FUNCFREE 1, slot) */
static uint32_t tls_used = 0xF;     /* the first slots belong to the system */
FN(TlsCall) {
    if (ARG(0) == 0) {
        for (uint32_t i = 4; i < CE_TLS && i < 32; i++) if (!(tls_used & (1u << i))) { tls_used |= 1u << i; RET(i); }
        RET(0xFFFFFFFFu);
    }
    if (ARG(1) < 32) tls_used &= ~(1u << ARG(1));
    RET(1);
}
FN(TlsGetValue) { set_error(0); RET(ARG(0) < CE_TLS ? ce_cur->tls[ARG(0)] : 0); }
FN(TlsSetValue) { if (ARG(0) >= CE_TLS) RET(0); ce_cur->tls[ARG(0)] = ARG(1); RET(1); }
FN(GetLastError) { RET(ce_cur ? ce_cur->last_error : 0); }
FN(SetLastError) { set_error(ARG(0)); RET(0); }

/* ------------------------------------------------------------------ time */
FN(GetTickCount) { RET(ce_milliseconds()); }
FN(QueryPerformanceCounter) { uint64_t v = ce_now(); dc_wr32(ARG(0), (uint32_t)v); dc_wr32(ARG(0) + 4, (uint32_t)(v >> 32)); RET(1); }
FN(QueryPerformanceFrequency) { dc_wr32(ARG(0), 200000000u); dc_wr32(ARG(0) + 4, 0); RET(1); }
static void systemtime(uint32_t p) {
    /* a fixed date plus the time the game has run */
    uint32_t ms = ce_milliseconds(), s = ms / 1000;
    uint16_t st[8] = { 2000, 1, 6, 1, (uint16_t)(12 + s / 3600 % 12), (uint16_t)(s / 60 % 60), (uint16_t)(s % 60), (uint16_t)(ms % 1000) };
    for (int i = 0; i < 8; i++) dc_wr16(p + (uint32_t)i * 2, st[i]);
}
FN(GetLocalTime) { systemtime(ARG(0)); RET(0); }
FN(GetSystemTime) { systemtime(ARG(0)); RET(0); }
static uint32_t rnd = 12345;
FN(Random) { rnd = rnd * 1103515245u + 12345u; RET(rnd >> 1); }

/* ------------------------------------------------------------------ memory */
#define LMEM_ZEROINIT 0x40u
FN(LocalAlloc) { RET(ce_alloc(ARG(1), 0)); }
FN(LocalFree) { ce_free(ARG(0)); RET(0); }
FN(LocalReAlloc) {            /* (p, size, flags): LMEM_MOVEABLE 2 */
    uint32_t p = ARG(0), sz = ARG(1), old = ce_alloc_size(p);
    if (sz <= old) RET(p);
    if (!(ARG(2) & 2)) RET(0);
    RET(ce_realloc(p, sz));
}
FN(LocalSize) { RET(ce_alloc_size(ARG(0))); }
FN(GetProcessHeap) { RET(0x00000F00u); }
FN(HeapCreate) { RET(0x00000F04u); }
FN(HeapDestroy) { RET(1); }
FN(HeapAlloc) { RET(ce_alloc(ARG(2), 0)); }
FN(HeapFree) { ce_free(ARG(2)); RET(1); }
FN(HeapReAlloc) { RET(ce_realloc(ARG(2), ARG(3))); }
FN(HeapSize) { RET(ce_alloc_size(ARG(2))); }
FN(malloc) { RET(ce_alloc(ARG(0), 0)); }
FN(calloc) { RET(ce_alloc(ARG(0) * ARG(1), 0)); }
FN(free) { ce_free(ARG(0)); RET(0); }
FN(realloc) { RET(ce_realloc(ARG(0), ARG(1))); }
FN(memcpy) { uint32_t d = ARG(0), s = ARG(1), n = ARG(2); uint8_t *pd = ce_ptr(d), *ps = ce_ptr(s);
    if (n && pd && ps && ce_ptr(d + n - 1) && ce_ptr(s + n - 1)) memmove(pd, ps, n);
    else for (uint32_t i = 0; i < n; i++) dc_wr8(d + i, dc_rd8(s + i));
    RET(d); }
FN(memcmp) { uint32_t a = ARG(0), b = ARG(1), n = ARG(2);
    for (uint32_t i = 0; i < n; i++) { int d = (int)dc_rd8(a + i) - (int)dc_rd8(b + i); if (d) RET((uint32_t)d); }
    RET(0); }
/* case-insensitive compares: COREDLL's own need the locale tables its start-up would set */
static uint32_t lw(uint32_t ch) { return ch >= 'A' && ch <= 'Z' ? ch + 32 : (ch >= 0xC0 && ch <= 0xDE && ch != 0xD7 ? ch + 32 : ch); }
static uint32_t icmp(uint32_t a, uint32_t b, uint32_t n, int wide) {
    for (uint32_t i = 0; i < n; i++) {
        uint32_t x = wide ? dc_rd16(a + i * 2) : dc_rd8(a + i), y = wide ? dc_rd16(b + i * 2) : dc_rd8(b + i);
        int d = (int)lw(x) - (int)lw(y);
        if (d) return (uint32_t)d;
        if (!x) return 0;
    }
    return 0;
}
FN(_wcsicmp) { RET(icmp(ARG(0), ARG(1), 0xFFFFFFFFu, 1)); }
FN(_wcsnicmp) { RET(icmp(ARG(0), ARG(1), ARG(2), 1)); }
FN(_stricmp) { RET(icmp(ARG(0), ARG(1), 0xFFFFFFFFu, 0)); }
FN(_strnicmp) { RET(icmp(ARG(0), ARG(1), ARG(2), 0)); }
FN(lstrcmpiW) { RET(icmp(ARG(0), ARG(1), 0xFFFFFFFFu, 1)); }
FN(towlower) { RET(lw(ARG(0) & 0xFFFF)); }
FN(towupper) { uint32_t ch = ARG(0) & 0xFFFF; RET(lw(ch) == ch && ((ch >= 'a' && ch <= 'z') || (ch >= 0xE0 && ch <= 0xFE && ch != 0xF7)) ? ch - 32 : ch); }
FN(memset) { uint32_t d = ARG(0), v = ARG(1), n = ARG(2); uint8_t *pd = ce_ptr(d);
    if (n && pd && ce_ptr(d + n - 1)) memset(pd, (int)v, n); else for (uint32_t i = 0; i < n; i++) dc_wr8(d + i, (uint8_t)v);
    RET(d); }

/* VirtualAlloc: committed memory comes from the heap (page aligned). A reservation with PAGE_NOACCESS is
 * what drivers make before VirtualCopy, so it gets an address in the window area instead. */
static uint32_t win_next = CE_MAPS_LO;
static struct { uint32_t va, size, backing; } wins[256]; static int nwins;
FN(VirtualAlloc) {            /* (addr, size, type, prot) */
    uint32_t addr = ARG(0), size = (ARG(1) + 0xFFF) & ~0xFFFu, type = ARG(2), prot = ARG(3);
    if (!addr) {
        if ((type & 0x2000) && !(type & 0x1000) && prot == 1 && nwins < 256) {      /* MEM_RESERVE, PAGE_NOACCESS */
            uint32_t va = win_next; win_next += (size + 0xFFFF) & ~0xFFFFu;
            wins[nwins].va = va; wins[nwins].size = size; wins[nwins].backing = 0; nwins++;
            TRACE("VirtualAlloc reserve %X -> %08X", size, va);
            RET(va);
        }
        uint32_t p = ce_alloc(size, 0x1000);
        TRACE("VirtualAlloc %X type %X -> %08X", size, type, p);
        RET(p);
    }
    for (int i = 0; i < nwins; i++) if (addr - wins[i].va < wins[i].size) {
        if (type & 0x1000) {                                      /* commit inside a reservation: back it */
            if (!wins[i].backing) { wins[i].backing = ce_alloc(wins[i].size, 0x1000); dc_hw_map(wins[i].va, wins[i].backing, wins[i].size); }
        }
        RET(addr);
    }
    RET(addr);                /* inside an allocation of ours: already committed */
}
FN(VirtualFree) {
    uint32_t addr = ARG(0);
    for (int i = 0; i < nwins; i++) if (wins[i].va == addr) {
        if (ARG(2) & 0x8000) { dc_hw_map(addr, 0, 0); if (wins[i].backing) ce_free(wins[i].backing); wins[i] = wins[--nwins]; }
        RET(1);
    }
    if (ARG(2) & 0x8000) ce_free(addr);     /* MEM_RELEASE */
    RET(1);
}
/* Drivers' memory the CE ROM reserves at a fixed address (config.bib's DRIVGLOB and DMA areas):
 * DDHAL maps "physical" 0x1FD80000, fills it through the store queues (which land at RAM offset
 * 0xD80000) and DMAs from it. It is RAM offset 0xD80000.., kept out of the heap (ce_init). */
static uint32_t drvmem_phys(uint32_t pa) {
    pa &= 0x1FFFFFFFu;
    if (pa - CE_DRVMEM_PHYS < CE_DRVMEM_LEN) return 0x0C000000u + (pa & 0x00FFFFFFu);
    return pa;
}
static uint32_t dma_src(uint32_t src) { return drvmem_phys(src); }
/* physical 0x1Fxxxxxx is the SH-4's own registers (P4 0xFFxxxxxx); the rest through P2 */
static uint32_t phys_to_va(uint32_t pa) {
    pa = drvmem_phys(pa);
    return (pa & 0x1F000000u) == 0x1F000000u ? 0xE0000000u | pa : 0xA0000000u | pa;
}
FN(VirtualCopy) {             /* (dst, src, size, prot): PAGE_PHYSICAL 0x400 -> src is a physical address >> 8 */
    uint32_t dst = ARG(0), src = ARG(1), size = ARG(2), prot = ARG(3);
    uint32_t to = (prot & 0x400) ? phys_to_va(src << 8) : src;
    TRACE("VirtualCopy %08X <- %08X (%X bytes, prot %X)", dst, to, size, prot);
    size = ((dst & 0xFFFu) + size + 0xFFFu) & ~0xFFFu;     /* whole pages, like the MMU (AICA's 0x702C00 sits in the page of 0x702000) */
    if (dst >= CE_MAPS_LO && dst < CE_MAPS_HI) { dc_hw_map(dst & ~0xFFFu, to & ~0xFFFu, size); RET(1); }
    ce_unimpl("VirtualCopy into committed memory");
    RET(0);
}
FN(LockPages) { if (ARG(3)) { uint32_t n = (ARG(1) + 0xFFF) / 0x1000; for (uint32_t i = 0; i < n; i++) dc_wr32(ARG(3) + i * 4, ARG(0) + i * 0x1000); } RET(1); }
FN(UnlockPages) { RET(1); }
FN(MapPtrToProcess) { RET(ARG(0)); }
FN(MapPtrUnsecure) { RET(ARG(0)); }
FN(SetProcPermissions) { RET(0xFFFFFFFFu); }
FN(GetCurrentPermissions) { RET(0xFFFFFFFFu); }
FN(SetKMode) { RET(1); }
FN(GetCallerProcess) { RET(0x00000042u); }
FN(GetCurrentProcessId) { RET(0x00000042u); }
FN(GetOwnerProcess) { RET(0x00000042u); }
FN(IsBadCodePtr) { RET(0); }
FN(IsBadReadPtr) { RET(0); }
FN(IsBadWritePtr) { RET(0); }
FN(GlobalMemoryStatus) {
    uint32_t p = ARG(0);
    dc_wr32(p + 4, 50); dc_wr32(p + 8, 16u << 20); dc_wr32(p + 12, 8u << 20); dc_wr32(p + 16, 0); dc_wr32(p + 20, 0);
    dc_wr32(p + 24, 32u << 20); dc_wr32(p + 28, 16u << 20);
    RET(0);
}

/* ------------------------------------------------------------------ modules */
FN(LoadLibraryW) { char n[128]; ce_wstr(ARG(0), n, sizeof n); RET(ce_load_library(n)); }
FN(LoadLibraryExW) { char n[128]; ce_wstr(ARG(0), n, sizeof n); RET(ce_load_library(n)); }
FN(LoadDriver) { char n[128]; ce_wstr(ARG(0), n, sizeof n); RET(ce_load_library(n)); }
FN(FreeLibrary) { RET(ce_free_library(ARG(0))); }
FN(GetProcAddressW) {
    uint32_t p = ARG(1);
    if (p < 0x10000) RET(ce_get_proc(ARG(0), NULL, p));
    char n[128]; ce_wstr(p, n, sizeof n);
    uint32_t a = ce_get_proc(ARG(0), n, 0);
    TRACE("GetProcAddress %08X %s -> %08X", ARG(0), n, a);
    RET(a);
}
FN(GetModuleHandleW) {
    if (!ARG(0)) RET(0x00010000u);
    char n[128]; ce_wstr(ARG(0), n, sizeof n);
    ce_module *m = ce_module_find(n);
    RET(m && m->refs ? m->base : 0);
}
FN(GetModuleFileNameW) {
    ce_module *m = ARG(0) ? ce_module_at(ARG(0)) : ce_module_find("RALLY2.EXE");
    if (!ARG(0)) m = ce_module_at(0x00010000u);
    char path[96]; snprintf(path, sizeof path, "\\CD-ROM\\%s", m ? m->name : "GAME.EXE");
    ce_put_wstr(ARG(1), path, (int)ARG(2));
    RET(strlen(path));
}
FN(DisableThreadLibraryCalls) { RET(1); }

/* resources: string tables (LoadStringW); other kinds through FindResource */
static uint32_t find_resource(uint32_t hmod, uint32_t type, uint32_t id, uint32_t *size) {
    ce_module *m = ce_module_at(hmod ? hmod : 0x10000u);
    if (!m) return 0;
    uint32_t base = m->base, opt = dc_rd32(base + 0x3C) + 24;
    uint32_t rrva = dc_rd32(base + opt + 96 + 2 * 8);
    if (!rrva) return 0;
    uint32_t root = base + rrva, ids[3] = { type, id, 0 }, dir = root;
    for (int lvl = 0; lvl < 3; lvl++) {
        uint32_t nn = dc_rd16(dir + 12), ni = dc_rd16(dir + 14), e = dir + 16, found = 0;
        for (uint32_t k = 0; k < nn + ni; k++, e += 8) {
            uint32_t name = dc_rd32(e), off = dc_rd32(e + 4);
            if (lvl == 2 || (!(name & 0x80000000u) && name == ids[lvl])) {
                if (off & 0x80000000u) { dir = root + (off & 0x7FFFFFFFu); found = 1; break; }
                uint32_t data = root + off;
                *size = dc_rd32(data + 4);
                return base + dc_rd32(data);
            }
        }
        if (!found) return 0;
    }
    return 0;
}
FN(LoadStringW) {             /* (hinst, id, buf, max) */
    uint32_t id = ARG(1), size, buf = ARG(2), max = ARG(3);
    uint32_t p = find_resource(ARG(0), 6, id / 16 + 1, &size);
    if (!p) { if (max) dc_wr16(buf, 0); RET(0); }
    for (uint32_t i = 0; i < id % 16; i++) p += 2 + dc_rd16(p) * 2u;
    uint32_t len = dc_rd16(p);
    if (!max) { dc_wr32(buf, p + 2); RET(len); }       /* max 0: pointer to the resource */
    if (len > max - 1) len = max - 1;
    for (uint32_t i = 0; i < len; i++) dc_wr16(buf + i * 2, dc_rd16(p + 2 + i * 2));
    dc_wr16(buf + len * 2, 0);
    RET(len);
}

/* ------------------------------------------------------------------ files: the disc is \CD-ROM */
static int disc_path(uint32_t wpath, char *out, size_t n) {
    char p[260]; ce_wstr(wpath, p, sizeof p);
    const char *s = p;
    if (!strncasecmp(s, "\\CD-ROM", 7)) s += 7;
    else if (!strncasecmp(s, "\\Windows", 8)) s += 8;
    while (*s == '\\') s++;
    size_t i = 0;
    for (; *s && i < n - 1; s++) out[i++] = *s == '\\' ? '/' : *s;
    out[i] = 0;
    return 0;
}
/* "\Device\X": a WDM driver loaded in this process by WDMLIB (the device manager's job on CE) */
static uint32_t wdm_proc(const char *name) {
    uint32_t h = ce_load_library("WDMLIB.DLL");
    return h ? ce_get_proc(h, name, 0) : 0;
}
#define CE_WDM_FILE 128u
static uint32_t new_event(int manual, int set) { uint32_t h = ce_handle_new(OB_EVENT); ce_obj *o = ce_handle(h); o->manual = manual; o->signaled = set; return h; }
static uint32_t open_device(uint32_t wname, const char *name, uint32_t flags) {
    uint32_t open = wdm_proc("OpenWDMDevice");
    if (!open) return INVALID_HANDLE;
    /* OpenWDMDevice(&file, name) -> BOOL fills in a file object (the device first) that the other calls
     * take. It is an object manager object (WDMLIB references it around requests), made with
     * ObCreateObject and holding one reference, zeroed, as the handle's data. */
    uint32_t tmp = ce_alloc(4, 4), file = 0;
    if (ce_call(wdm_proc("ObCreateObject"), 5, CE_WDM_FILE, 0, 0, tmp, 0) == 0) file = dc_rd32(tmp);
    ce_free(tmp);
    if (!file) return INVALID_HANDLE;
    for (uint32_t i = 0; i < CE_WDM_FILE; i += 4) dc_wr32(file + i, 0);
    ce_call(wdm_proc("ObReferenceObjectByPointer"), 1, file);
    uint32_t r = ce_call(open, 2, file, wname + 8 * 2), dev = dc_rd32(file);   /* the name in the "Device" directory */
    TRACE("OpenWDMDevice %s -> %u, device %08X", name, r, dev);
    if (!r || !dev) ce_call(wdm_proc("ObDereferenceObject"), 1, file);
    else if (!(flags & 0x40000000u)) {                     /* no FILE_FLAG_OVERLAPPED: synchronous file object, */
        dc_wr32(file + 24, 2);                             /* whose calls wait for the request's completion event */
        dc_wr32(file + 48, new_event(0, 0));               /* (+48) under a lock (+52) */
        dc_wr32(file + 52, new_event(0, 1));
    }
    if ((!r || !dev) && !strcasecmp(name, "\\Device\\CDROM0")) {
        /* the GD-ROM: the disc is read through the files API here, so its driver isn't loaded; its
         * controls (drive settings) have nothing to do */
        uint32_t h = ce_handle_new(OB_FILE);
        ce_obj *o = ce_handle(h);
        o->max = 2; snprintf(o->name, sizeof o->name, "%s", name);
        return h;
    }
    if (!r || !dev) { set_error(2); return INVALID_HANDLE; }
    uint32_t h = ce_handle_new(OB_FILE);
    ce_obj *o = ce_handle(h);
    o->max = 1; o->lba = file;
    snprintf(o->name, sizeof o->name, "%s", name);
    return h;
}
FN(CreateFileW) {             /* (name, access, share, sec, disposition, flags, template) */
    { char full[64]; ce_wstr(ARG(0), full, sizeof full);
      if (!strncasecmp(full, "\\Device\\", 8)) RET(open_device(ARG(0), full, ARG(5))); }
    char p[64]; disc_path(ARG(0), p, sizeof p);
    uint32_t lba, size;
    if (!p[0] || dc_disc_find(p, &lba, &size)) {
        char full[260]; ce_wstr(ARG(0), full, sizeof full);
        TRACE("CreateFile %s: not found", full);
        set_error(2); RET(INVALID_HANDLE);
    }
    uint32_t h = ce_handle_new(OB_FILE);
    ce_obj *o = ce_handle(h);
    o->lba = lba; o->size = size; o->pos = 0;
    snprintf(o->name, sizeof o->name, "%s", p);
    TRACE("CreateFile %s -> %08X (%u bytes)", p, h, size);
    RET(h);
}
FN(ReadFile) {                /* (h, buf, n, &read, ovl) */
    ce_obj *o = ce_handle(ARG(0));
    if (!o || o->type != OB_FILE) RET(0);
    uint32_t buf = ARG(1), n = ARG(2);
    if (o->pos >= o->size) n = 0;
    else if (n > o->size - o->pos) n = o->size - o->pos;
    static uint8_t tmp[64 * 2048];
    uint32_t done = 0;
    while (done < n) {
        uint32_t at = o->pos + done, sec = at / 2048, off = at % 2048, cnt = (off + n - done + 2047) / 2048;
        if (cnt > 64) cnt = 64;
        if (dc_disc_read(o->lba + sec, cnt, tmp)) break;
        uint32_t take = cnt * 2048 - off; if (take > n - done) take = n - done;
        ce_wr_block(buf + done, tmp + off, take);
        done += take;
    }
    o->pos += done;
    if (ARG(3)) dc_wr32(ARG(3), done);
    RET(1);
}
FN(WriteFile) { if (ARG(3)) dc_wr32(ARG(3), 0); set_error(5); RET(0); }
FN(SetFilePointer) {          /* (h, dist, &high, method) */
    ce_obj *o = ce_handle(ARG(0));
    if (!o || o->type != OB_FILE) RET(0xFFFFFFFFu);
    int32_t d = (int32_t)ARG(1);
    uint32_t m = ARG(3);
    int64_t np = (m == 0 ? 0 : m == 1 ? (int64_t)o->pos : (int64_t)o->size) + d;
    if (np < 0) np = 0;
    o->pos = (uint32_t)np;
    if (ARG(2)) dc_wr32(ARG(2), 0);
    RET(o->pos);
}
FN(GetFileSize) { ce_obj *o = ce_handle(ARG(0)); if (ARG(1)) dc_wr32(ARG(1), 0); RET(o && o->type == OB_FILE ? o->size : 0xFFFFFFFFu); }
FN(GetFileAttributesW) {
    char p[260]; disc_path(ARG(0), p, sizeof p);
    uint32_t lba, size;
    if (!p[0]) RET(0x10);
    if (dc_disc_find(p, &lba, &size)) RET(0xFFFFFFFFu);
    RET(0x01);                /* read only */
}
/* the GD-ROM drive's audio controls (NT CD-ROM IOCTLs) on dc-core's CD audio player */
static uint32_t msf_lba(uint32_t m, uint32_t s, uint32_t f) { return (m * 60 + s) * 75 + f - 150; }
static void lba_msf(uint32_t lba, uint32_t p) { uint32_t a = lba + 150; dc_wr8(p, 0); dc_wr8(p + 1, (uint8_t)(a / 4500)); dc_wr8(p + 2, (uint8_t)(a / 75 % 60)); dc_wr8(p + 3, (uint8_t)(a % 75)); }
static uint32_t cdrom_ioctl(sh4ctx *c) {
    uint32_t code = ARG(1), in = ARG(2), out = ARG(4), outsz = ARG(5), retp = ARG(6), ret = 0;
    TRACE("CD-ROM ioctl %08X in %08X/%u out %08X/%u ovl %08X", code, in, ARG(3), out, outsz, ARG(7));
    switch (code) {
    case 0x00024000u: {           /* READ_TOC: CDROM_TOC { Length[2] BE, First, Last, TRACK_DATA[100] (8 bytes) } */
        int n = dc_disc_track_count();
        if (outsz < 4u + 8u * (uint32_t)(n + 1)) break;
        uint32_t len = 2 + 8u * (uint32_t)(n + 1);
        dc_wr8(out, (uint8_t)(len >> 8)); dc_wr8(out + 1, (uint8_t)len); dc_wr8(out + 2, 1); dc_wr8(out + 3, (uint8_t)n);
        uint32_t end = 0;
        for (int i = 0; i <= n; i++) {
            uint32_t lba = 0, sec = 0, e = out + 4 + 8u * (uint32_t)i; int data = 0;
            if (i < n) dc_disc_track_info(i, &lba, &data, &sec); else lba = end;
            if (lba + sec > end) end = lba + sec;
            dc_wr8(e, 0); dc_wr8(e + 1, (uint8_t)((data ? 4 : 0) << 4 | 1)); dc_wr8(e + 2, (uint8_t)(i < n ? i + 1 : 0xAA)); dc_wr8(e + 3, 0);
            lba_msf(lba, e + 4);
        }
        ret = 4 + 8u * (uint32_t)(n + 1);
        break;
    }
    case 0x00024018u:             /* PLAY_AUDIO_MSF { M S F (start), M S F (end) } */
        TRACE("CD audio play %u:%u:%u .. %u:%u:%u", dc_rd8(in), dc_rd8(in + 1), dc_rd8(in + 2), dc_rd8(in + 3), dc_rd8(in + 4), dc_rd8(in + 5));
        dc_cdda_play(msf_lba(dc_rd8(in), dc_rd8(in + 1), dc_rd8(in + 2)), msf_lba(dc_rd8(in + 3), dc_rd8(in + 4), dc_rd8(in + 5)), 0);
        break;
    case 0x00024004u: break;      /* SEEK_AUDIO_MSF */
    case 0x00024008u: dc_cdda_stop(); break;
    case 0x0002400Cu: dc_cdda_pause(1); break;
    case 0x00024010u: dc_cdda_pause(0); break;
    case 0x0002402Cu: {           /* READ_Q_CHANNEL: SUB_Q_CURRENT_POSITION */
        uint32_t lba; int st = dc_cdda_status(&lba);
        if (outsz < 16) break;
        for (uint32_t i = 0; i < 16; i++) dc_wr8(out + i, 0);
        dc_wr8(out + 1, st == 1 ? 0x11 : st == 2 ? 0x12 : 0x13);
        dc_wr8(out + 3, 12); dc_wr8(out + 4, 1); dc_wr8(out + 5, 0x01);
        int n = dc_disc_track_count();
        for (int i = 0; i < n; i++) { uint32_t tl, ts; int d; dc_disc_track_info(i, &tl, &d, &ts); if (lba >= tl && lba < tl + ts) dc_wr8(out + 6, (uint8_t)(i + 1)); }
        dc_wr8(out + 7, 1); lba_msf(lba, out + 8);
        ret = 16;
        break;
    }
    case 0x00024014u: case 0x00024028u: ret = 0; break;   /* GET/SET_VOLUME */
    default:
        TRACE("DeviceIoControl CD-ROM code %08X", code);
    }
    if (retp) dc_wr32(retp, ret);
    if (ARG(7)) {                 /* OVERLAPPED { Internal, InternalHigh, Offset, OffsetHigh, hEvent }: done now */
        dc_wr32(ARG(7), 0); dc_wr32(ARG(7) + 4, ret);
        if (dc_rd32(ARG(7) + 16)) ce_signal(dc_rd32(ARG(7) + 16));
    }
    return 1;
}
FN(DeviceIoControl) {         /* (h, code, in, insize, out, outsize, &ret, ovl) */
    ce_obj *o = ce_handle(ARG(0));
    if (o && o->type == OB_FILE && o->max == 1) {
        uint32_t ctl = wdm_proc("ControlWDMDevice");
        uint32_t r = ce_call(ctl, 8, o->lba, ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), ARG(6), ARG(7));
        if (ce_trace > 1) TRACE("DeviceIoControl %s code %08X -> status %08X", o->name, ARG(1), r);
        if (r == 0) RET(1);                                /* ControlWDMDevice returns an NTSTATUS */
        set_error(r == 0x103u ? 997u : 31u);               /* ERROR_IO_PENDING, ERROR_GEN_FAILURE */
        RET(0);
    }
    if (o && o->type == OB_FILE && o->max == 2) RET(cdrom_ioctl(c));
    TRACE("DeviceIoControl %08X (%s) code %08X", ARG(0), o ? o->name : "?", ARG(1));
    ce_unimpl("DeviceIoControl");
    RET(0);
}
FN(CreateDirectoryW) { RET(0); }
FN(DeleteFileW) { RET(0); }

/* ------------------------------------------------------------------ the OEM layer */
FN(InterruptInitialize) {     /* (sysintr, event, data, size) */
    TRACE("InterruptInitialize SYSINTR %u event %08X", ARG(0), ARG(1));
    RET(ce_irq_attach(ARG(0), ARG(1)));
}
FN(InterruptDone) { ce_irq_done(ARG(0)); RET(0); }
FN(InterruptDisable) { ce_irq_disable(ARG(0)); RET(0); }
static uint32_t phys_next = 0x0C100000u;      /* physical RAM: nothing else uses it without a CE kernel */
FN(KernelIoControl) {         /* (code, in, insize, out, outsize, &ret) */
    uint32_t code = ARG(0), in = ARG(1), insz = ARG(2), out = ARG(3), outsz = ARG(4), retp = ARG(5);
    (void)phys_next;
    switch (code) {
    case 0x00010000u:          /* the Holly status bits behind a SYSINTR's last interrupt (graphics thread) */
        if (out && outsz >= 4 && in) {
            uint32_t bits = ce_irq_take(dc_rd32(in));
            dc_wr32(out, bits);
            if (ce_trace > 2) TRACE("interrupt bits SYSINTR %u: %08X", dc_rd32(in), bits);
        }
        if (retp) dc_wr32(retp, 4);
        RET(1);
    case 0x00030000u:          /* (sysintr, on): interrupt option, nothing to do */
        RET(1);
    }
    TRACE("KernelIoControl %08X in %08X/%u (%08X) out %08X/%u", code, in, insz, insz >= 4 ? dc_rd32(in) : 0, out, outsz);
    ce_unimpl("KernelIoControl");
    RET(0);
}
FN(SetStoreQueueBase) {      /* the store queue area 0xE0000000.. writes to this physical address on */
    TRACE("SetStoreQueueBase %08X", ARG(0));
    dc_sq_base = 0xA0000000u | (ARG(0) & 0x1FFFFFFFu);
    /* DDHAL addresses its buffers in the drivers' reserved RAM as this value + (physical - 0x0C000000):
     * that VA range is the reserved RAM */
    dc_hw_map(0xE0000000u + (CE_DRVMEM_PHYS - 0x0C000000u), 0xAC000000u | (CE_DRVMEM_PHYS & 0x00FFFFFFu), CE_DRVMEM_LEN);
    RET(0xE0000000u);           /* the store queue area's address, which now writes there */
}
/* another process's (here: the drivers') use of an event, which it ends with CloseHandle: WDMLIB's
 * IoCompleteRequest closes the request's event after setting it */
FN(AddEventAccess) { ce_obj *o = ce_handle(ARG(0)); if (!o || o->type != OB_EVENT) RET(0); o->refs++; RET(1); }
/* ---- the kernel's WDM HAL API set ("WDMH"), which WDMLIB's Hal and Mm functions call. User memory
 * below 16 MB is the console's RAM (dc_ram aliases it), so its physical address is 0x0C000000 + va. */
#define CE_WDMH_SET 0x20u
#define CE_WDMH     (CE_MAGIC + 0x2000u)
static uint32_t va_phys(uint32_t va) { return va < DC_RAM_SIZE ? 0x0C000000u + va : DC_IS_RAM(va) ? (va & 0x1FFFFFFFu) : 0; }
/* the kernel side of HalAllocateCommonBuffer: physical memory, which WDMLIB then maps with VirtualCopy */
static uint32_t wdmh_alloc(uint32_t len) {
    uint32_t va = ce_alloc((len + 0xFFF) & ~0xFFFu, 0x1000);
    if (va >= DC_RAM_SIZE) { ce_free(va); TRACE("common buffer above physical RAM"); return 0; }
    TRACE("HalAllocateCommonBuffer %X -> phys %08X", len, va_phys(va));
    return va_phys(va);
}
static void wdmh_free(sh4ctx *c) {          /* which argument is the physical address isn't known: any that is */
    for (int i = 0; i < 4; i++) {
        uint32_t a = ARG(i);
        if (a >= 0x0C000000u && a < 0x0C000000u + DC_RAM_SIZE && ce_alloc_size(a - 0x0C000000u)) { ce_free(a - 0x0C000000u); return; }
    }
}
static uint32_t h_wdmh(sh4ctx *c) {
    uint32_t m = (c->entry - (0x80000000u | (CE_WDMH & 0x1FFFFFFFu))) / 2;
    switch (m) {
    case 2: {                                              /* HalAllocateCommonBuffer(adapter, len, &phys, cached) -> phys, */
        uint32_t phys = wdmh_alloc(ARG(1));                /* also stored in the caller's PHYSICAL_ADDRESS (WDMLIB doesn't) */
        if (ARG(2)) { dc_wr32(ARG(2), phys); dc_wr32(ARG(2) + 4, 0); }
        RET(phys);
    }
    case 3: wdmh_free(c); RET(0);                          /* HalFreeCommonBuffer */
    case 4: case 6: RET(0);                                /* Hal{Get,Set}BusDataByOffset */
    case 5:                                                /* HalMapInterrupt(irq, ?, &sysintr): the OAL's numbers are SYSINTRs */
        TRACE("HalMapInterrupt %u", ARG(0));
        if (ARG(2)) dc_wr32(ARG(2), ARG(0));
        RET(0);
    case 7: case 8: {                                      /* MmBuildMdlForNonPagedPool(mdl) / MmProbeAndLockPages(mdl, mode, op) */
        uint32_t mdl = ARG(0), start = dc_rd32(mdl + 16), count = dc_rd32(mdl + 20), ofs = dc_rd32(mdl + 24);
        uint32_t pages = (ofs + count + 0xFFF) >> 12;
        for (uint32_t i = 0; i < pages; i++) dc_wr32(mdl + 28 + i * 4, va_phys(start + i * 0x1000) >> 12);
        if (m == 7) dc_wr32(mdl + 12, start + ofs);
        RET(0);
    }
    case 9: RET(0);                                        /* MmUnlockPages */
    case 10: case 11: {        /* buffers kept by id across driver loads: 10 (id, va, phys) records, 11 (id, &va, &phys) finds */
        static struct { uint32_t id, va, phys; } ids[16]; static int nids;
        int k = 0;
        for (; k < nids && ids[k].id != ARG(0); k++) ;
        if (m == 10) { if (k == nids && nids < 16) nids++; if (k < 16) { ids[k].id = ARG(0); ids[k].va = ARG(1); ids[k].phys = ARG(2); } RET(0); }
        dc_wr32(ARG(1), k < nids ? ids[k].va : 0); dc_wr32(ARG(2), k < nids ? ids[k].phys : 0);
        RET(0);
    }
    }
    char what[48]; snprintf(what, sizeof what, "WDMH method %u", m);
    ce_unimpl(what);
    RET(0);
}
FN(QueryAPISetID) {           /* (char *name) */
    char n[32]; int i = 0;
    for (; i < 31 && dc_rd8(ARG(0) + (uint32_t)i); i++) n[i] = (char)dc_rd8(ARG(0) + (uint32_t)i);
    n[i] = 0;
    TRACE("QueryAPISetID %s", n);
    if (!strcmp(n, "WDMH")) RET(CE_WDMH_SET);
    RET(0xFFFFFFFFu);
}
FN(GetAPIAddress) {           /* (set, method) */
    if (ARG(0) == CE_WDMH_SET) RET(CE_WDMH + ARG(1) * 2);
    RET(0);
}
static void debug_print(uint32_t ws) {
    char s[512]; int n = ce_wstr(ws, s, sizeof s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = 0;
    fprintf(stderr, "[ce dbg] %s\n", s);
}
FN(OutputDebugStringW) { debug_print(ARG(0)); RET(0); }
FN(NKDbgPrintfW) {
    /* format with the recompiled wvsprintfW is not reachable from here: print the format string */
    debug_print(ARG(0));
    RET(0);
}
FN(ExitProcess) { ce_exit_process(ARG(0)); RET(0); }
FN(TerminateProcess) { ce_exit_process(ARG(1)); RET(1); }
FN(SetSystemPowerState) { RET(0); }
FN(SystemParametersInfoW) { RET(0); }
FN(GetSystemInfo) {
    uint32_t p = ARG(0);
    dc_wr32(p, 0); dc_wr32(p + 4, 4096); dc_wr32(p + 8, 0x10000); dc_wr32(p + 12, 0x7FFFFFFF);
    dc_wr32(p + 16, 1); dc_wr32(p + 20, 1); dc_wr32(p + 24, 10003 /* PROCESSOR_HITACHI_SH4 */); dc_wr32(p + 28, 0x10000);
    RET(0);
}

/* COREDLL's own calls into the kernel for anything not replaced above */
static uint32_t h_trap(sh4ctx *c) {
    uint32_t off = (0x9FFFC801u - (c->entry | 1u)) / 2;
    char what[64]; snprintf(what, sizeof what, "kernel call: API set %u method %u", off >> 8, off & 0xFF);
    ce_unimpl(what);
    c->r[0] = 0;
    return 0;
}

const ce_override ce_kernel_overrides[] = {
#define K(n) { "COREDLL.DLL", #n, k_##n }
    K(CreateThread), K(ExitThread), K(TerminateThread), K(ResumeThread), K(SuspendThread), K(SetThreadPriority),
    K(GetThreadPriority), K(GetExitCodeThread), K(Sleep), K(SleepTillTick), K(WaitForSingleObject), K(WaitForMultipleObjects),
    K(GetCurrentThreadId),
    K(CreateEventW), K(EventModify), K(CreateMutexW), K(ReleaseMutex), K(CreateSemaphoreW), K(ReleaseSemaphore), K(CloseHandle),
    K(InitializeCriticalSection), K(DeleteCriticalSection), K(EnterCriticalSection), K(TryEnterCriticalSection),
    K(LeaveCriticalSection), K(InterlockedIncrement), K(InterlockedDecrement), K(InterlockedExchange),
    K(InterlockedTestExchange), K(InterlockedCompareExchange), K(InterlockedExchangeAdd),
    K(TlsCall), K(TlsGetValue), K(TlsSetValue), K(GetLastError), K(SetLastError),
    K(GetTickCount), K(QueryPerformanceCounter), K(QueryPerformanceFrequency), K(GetLocalTime), K(GetSystemTime), K(Random),
    K(LocalAlloc), K(LocalFree), K(LocalReAlloc), K(LocalSize), K(GetProcessHeap), K(HeapCreate), K(HeapDestroy),
    K(HeapAlloc), K(HeapFree), K(HeapReAlloc), K(HeapSize), K(malloc), K(calloc), K(free), K(realloc), K(memcpy), K(memset), K(memcmp), K(_wcsicmp), K(_wcsnicmp), K(_stricmp), K(_strnicmp), K(lstrcmpiW),
    K(towlower), K(towupper),
    { "COREDLL.DLL", "memmove", k_memcpy }, { "COREDLL.DLL", "??2@YAPAXI@Z", k_malloc }, { "COREDLL.DLL", "??3@YAXPAX@Z", k_free },
    K(VirtualAlloc), K(VirtualFree), K(VirtualCopy), K(LockPages), K(UnlockPages), K(MapPtrToProcess), K(MapPtrUnsecure),
    K(SetProcPermissions), K(GetCurrentPermissions), K(SetKMode), K(GetCallerProcess), K(GetCurrentProcessId), K(GetOwnerProcess),
    K(IsBadCodePtr), K(IsBadReadPtr), K(IsBadWritePtr), K(GlobalMemoryStatus),
    K(LoadLibraryW), K(LoadLibraryExW), K(LoadDriver), K(FreeLibrary), K(GetProcAddressW), K(GetModuleHandleW),
    K(GetModuleFileNameW), K(DisableThreadLibraryCalls), K(LoadStringW),
    K(CreateFileW), K(ReadFile), K(WriteFile), K(SetFilePointer), K(GetFileSize), K(GetFileAttributesW), K(DeviceIoControl),
    K(CreateDirectoryW), K(DeleteFileW),
    K(InterruptInitialize), K(InterruptDone), K(InterruptDisable), K(KernelIoControl), K(SetStoreQueueBase),
    K(AddEventAccess), K(QueryAPISetID), K(GetAPIAddress), K(OutputDebugStringW), K(NKDbgPrintfW),
    K(ExitProcess), K(TerminateProcess), K(SetSystemPowerState), K(SystemParametersInfoW), K(GetSystemInfo),
#undef K
    { NULL, NULL, NULL }
};

void ce_overrides_init(void) {
    const ce_override *lists[] = { ce_kernel_overrides, ce_gwes_overrides, ce_misc_overrides };
    for (size_t l = 0; l < sizeof lists / sizeof *lists; l++)
        for (const ce_override *o = lists[l]; o->name; o++) {
            ce_module *m = ce_module_find(o->dll);
            uint32_t a = m ? ce_get_proc(m->base, o->name, 0) : 0;
            if (a) dc_hle_register(a, o->fn);
            else if (m && ce_trace) TRACE("override %s!%s: no such export", o->dll, o->name);
        }
    dc_hle_register_range(CE_TRAPS_LO, CE_TRAPS_HI, h_trap);
    dc_hle_register_range(CE_WDMH, CE_WDMH + 0x40, h_wdmh);
    dc_dma_src_hook = dma_src;
}

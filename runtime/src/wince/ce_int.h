/* dc-core Windows CE runtime: shared declarations (see dccore/wince.h) */
#ifndef CE_INT_H
#define CE_INT_H
#include "dccore/wince.h"
#include "dccore/runtime.h"
#include "dccore/coro.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifdef _MSC_VER
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#else
#include <strings.h>
#endif

/* ---- the user address space ---- */
#define CE_KDATA        0x00005800u    /* PUserKData on SH (kfuncs.h) */
#define CE_HEAP_ALIGN   32u
#define CE_MAGIC        0x9F800000u    /* runtime entry points: VBR, thread exit, reschedule */
#define CE_VBR          (CE_MAGIC + 0x0000u)
#define CE_THREAD_EXIT  (CE_MAGIC + 0x1000u)
#define CE_RESCHED      (CE_MAGIC + 0x1002u)
#define CE_PROC_START   (CE_MAGIC + 0x1004u)
#define CE_TRAPS_LO     0xFFFF0000u    /* COREDLL's kernel calls: FIRST_METHOD 0xFFFFC801 - (set << 8 | method) * 2 */
#define CE_TRAPS_HI     0xFFFFC802u
#define CE_DRVMEM_PHYS  0x1FD80000u    /* drivers' reserved RAM (DDHAL), = RAM offset 0xD80000 */
#define CE_DRVMEM_LEN   0x00100000u
#define CE_MAPS_LO      0x7C000000u    /* VirtualAlloc reservations that VirtualCopy maps (dc_hw_map windows) */
#define CE_MAPS_HI      0x7F000000u

static inline uint32_t ce_arg(sh4ctx *c, int i) { return i < 4 ? c->r[4 + i] : dc_rd32(c->r[15] + 16 + (uint32_t)(i - 4) * 4); }
#define ARG(i) ce_arg(c, i)

uint8_t *ce_ptr(uint32_t va);                       /* host pointer to user memory, or NULL */
void     ce_rd_block(void *dst, uint32_t va, uint32_t n);
void     ce_wr_block(uint32_t va, const void *src, uint32_t n);
int      ce_wstr(uint32_t va, char *out, int n);    /* guest UTF-16 -> host (Latin-1, '?' else); length */
int      ce_wstrw(uint32_t va, uint16_t *out, int n);
uint32_t ce_wstrlen(uint32_t va);
void     ce_put_wstr(uint32_t va, const char *s, int max);

/* ---- heap (user memory) ---- */
void     ce_heap_init(uint32_t lo, uint32_t hi);
void     ce_heap_reserve(uint32_t lo, uint32_t hi);     /* take a range out of the heap (before any allocation) */
uint32_t ce_alloc(uint32_t size, uint32_t align);
void     ce_free(uint32_t va);
uint32_t ce_alloc_size(uint32_t va);
uint32_t ce_realloc(uint32_t va, uint32_t size);

/* ---- kernel objects ---- */
enum { OB_FREE, OB_EVENT, OB_MUTEX, OB_SEM, OB_THREAD, OB_FILE, OB_PROCESS, OB_FIND, OB_REG, OB_OTHER };
typedef struct ce_thread ce_thread;
typedef struct ce_obj {
    int type, refs;
    int signaled, manual;          /* events; threads/processes: signaled when ended */
    int count, max;                /* semaphores; mutex recursion */
    ce_thread *owner;              /* mutexes */
    ce_thread *thread;             /* OB_THREAD */
    uint32_t lba, size, pos;       /* files on the disc */
    void *data;                    /* other kinds */
    char name[260];                /* object name, file path, registry key path */
} ce_obj;
uint32_t ce_handle_new(int type);
ce_obj  *ce_handle(uint32_t h);
void     ce_handle_close(uint32_t h);

/* ---- threads ---- */
enum { TH_READY, TH_RUNNING, TH_WAITING, TH_SLEEPING, TH_SUSPENDED, TH_DEAD };
#define CE_MAXWAIT 64
#define CE_TLS 64
typedef struct ce_msg { uint32_t hwnd, msg, wparam, lparam; } ce_msg;
struct ce_thread {
    sh4ctx regs;
    dc_fiber *fiber;
    uint32_t id, handle, start, param, stack_lo, stack_hi;
    int prio, state, suspend;
    uint64_t wake_at;              /* cycles; ~0 = no timeout */
    uint32_t waits[CE_MAXWAIT]; int nwait, wait_all;
    uint32_t wait_result;
    uint64_t last_run;
    uint32_t exit_code;
    uint32_t tls[CE_TLS];
    uint32_t last_error;
    ce_msg q[64]; int qh, qn;      /* posted messages (GWES) */
};
extern ce_thread *ce_cur;
extern sh4ctx *ce_ctx;
ce_thread *ce_thread_create(uint32_t start, uint32_t param, uint32_t stack, int suspended);
uint32_t ce_wait(uint32_t *handles, int n, int all, uint32_t ms);   /* WAIT_OBJECT_0 + i, WAIT_TIMEOUT */
void     ce_sleep(uint32_t ms);
void     ce_signal(uint32_t h);                    /* SetEvent */
void     ce_reschedule(void);                      /* let a higher-priority ready thread run */
uint64_t ce_now(void);                              /* cycles */
#define CE_CYCLES_PER_MS 200000ull

/* ---- the OEM layer: interrupts ---- */
void     ce_irq_init(void);
int      ce_irq_attach(uint32_t sysintr, uint32_t event);
void     ce_irq_done(uint32_t sysintr);
void     ce_irq_disable(uint32_t sysintr);
uint32_t ce_irq_take(uint32_t sysintr);            /* status bits that fired since the last call */

/* ---- modules ---- */
typedef struct ce_module {
    char name[64];
    uint32_t base, size, entry;
    int refs, is_exe, native;      /* native: COREDLL, whose start-up the runtime does itself */
    uint8_t *pristine;             /* relocated image, copied in at every first load */
    const ce_module_desc *desc;
} ce_module;
ce_module *ce_module_find(const char *name);
uint32_t ce_load_library(const char *name);           /* HMODULE (= base), 0 if missing */
int      ce_free_library(uint32_t hmod);
uint32_t ce_get_proc(uint32_t hmod, const char *name, uint32_t ordinal);
ce_module *ce_module_at(uint32_t va);

/* ---- native overrides of DLL exports (COREDLL's kernel and window-manager calls, CoCreateInstance) */
typedef uint32_t (*ce_fn)(sh4ctx *c);
typedef struct { const char *dll, *name; ce_fn fn; } ce_override;
void     ce_overrides_init(void);
extern const ce_override ce_kernel_overrides[], ce_gwes_overrides[], ce_misc_overrides[];
ce_thread *ce_thread_of(uint32_t h);
void     ce_thread_resume(ce_thread *t);
void     ce_exit_thread(uint32_t code);
void     ce_exit_process(uint32_t code);
/* calling guest code from native code (window procedures, DllMain, COM methods) */
uint32_t ce_call(uint32_t fn, int n, ...);

/* ---- registry, files ---- */
void     ce_reg_init(void);
void     ce_trace_api(const char *fmt, ...);
#define TRACE(...) do { if (ce_trace) ce_trace_api(__VA_ARGS__); } while (0)
void     ce_unimpl(const char *what);

/* Win32 constants */
#define WAIT_OBJECT_0   0u
#define WAIT_TIMEOUT    258u
#define WAIT_FAILED     0xFFFFFFFFu
#define INFINITE        0xFFFFFFFFu
#define INVALID_HANDLE  0xFFFFFFFFu
#endif

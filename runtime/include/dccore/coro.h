#ifndef DCCORE_CORO_H
#define DCCORE_CORO_H
/* dc-core: a game's own setjmp/longjmp and the cooperative threads built on them.
 *
 * Many Dreamcast games switch between threads by saving registers into a buffer (setjmp) and
 * loading another buffer's registers, stack pointer and return address (longjmp). Recompiled
 * code runs on the host stack, so a guest longjmp has to come back to the host frame that made
 * the matching guest setjmp -- possibly on another stack:
 *
 *  - the recompiler (--setjmp ADDR) wraps every call to the game's setjmp in DC_CALL_SETJMP, which
 *    also takes a host setjmp in the calling function's frame, keyed by the guest buffer;
 *  - dc_coro_install() replaces the game's longjmp: it runs the game's own register restore, then
 *      * resumes the host frame of the matching setjmp (unwinding in this host stack, or
 *        switching to the coroutine that owns it), or
 *      * starts a new coroutine for a buffer no setjmp of ours filled (a thread being started:
 *        the game built its context by hand, with a new stack and entry point).
 * Guest registers live in the one sh4ctx; coroutines only give each guest thread a host stack.
 */
#include "dccore/sh4.h"
#ifdef __cplusplus
extern "C" {
#endif

/* setjmp_addr / longjmp_addr: the game's routines (setjmp returns 0 in r0, longjmp(buf=r4, val=r5)
 * returns val, or 1 for 0, at the setjmp). host_stack: bytes per coroutine (0: 1 MB). */
void dc_coro_install(uint32_t setjmp_addr, uint32_t longjmp_addr, size_t host_stack);
/* A routine that switches tasks by swapping stacks and ending with rte into the other task (a
 * middleware/OS task switch). Each entry point is hooked separately. */
void dc_coro_add_switch(uint32_t addr);
/* Leave every guest thread and task switch of the running executable (another one is about to
 * start, e.g. the game after an intro executable): continue at `jb`, a host setjmp taken on the
 * program's main stack before the guest code ran. The hooks of dc_coro_install / dc_coro_add_switch
 * are removed; install the next executable's afterwards. Does not return. */
void dc_coro_reset(jmp_buf *jb);
extern int dc_coro_trace;           /* non-zero: log switches to stderr */
int dc_coro_count(void);            /* coroutines created so far (tests/diagnostics) */

#ifdef __cplusplus
}
#endif
#endif

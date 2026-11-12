#ifndef DCCORE_RUNTIME_H
#define DCCORE_RUNTIME_H
#include "dccore/sh4.h"
#include <stddef.h>
#if defined(__GNUC__) && defined(__ELF__)
#pragma GCC visibility push(hidden)             /* see dccore/sh4.h */
#endif
#ifdef __cplusplus
extern "C" {
#endif

typedef void (*dc_miss_fn)(sh4ctx *c, uint32_t addr);   /* unknown address: handle it, or abort */
typedef void (*dc_trap_fn)(sh4ctx *c, uint32_t pc, const char *what);
typedef void (*dc_call_hook)(sh4ctx *c, uint32_t addr);

typedef struct dc_hw_ops {
    uint8_t  (*rd8)(uint32_t);  uint16_t (*rd16)(uint32_t);  uint32_t (*rd32)(uint32_t);
    void (*wr8)(uint32_t, uint8_t); void (*wr16)(uint32_t, uint16_t); void (*wr32)(uint32_t, uint32_t);
    void (*pref)(uint32_t);
} dc_hw_ops;

void  dc_init(sh4ctx *c);                         /* allocates 16 MB RAM, sets boot-like state */
int   dc_load_binary(const char *path, uint32_t addr);
int   dc_load_memory(const uint8_t *data, uint32_t len, uint32_t addr);   /* call after dc_init */
/* Programs made of several executables loaded at the same address one after another (a launcher, an
 * intro, the game): recompile the others with sh4recomp.py --prefix NAME (CMake: PREFIX NAME), then
 * switch with dc_use_code(NAME_dc_fn_table, NAME_dc_fn_count) after loading one. Declare them as
 *   extern const char NAME_dc_fn_table[]; extern const size_t NAME_dc_fn_count;
 * dc_use_code(dc_fn_table, dc_fn_count) goes back to the unprefixed one. */
void  dc_use_code(const void *table, size_t count);
sh4fn dc_lookup(uint32_t addr);
/* the recompiled function at addr itself, past any dc_hle_register override of it (an override that
 * wraps the original: set c->entry = addr, call it, then dc_call what it returns), NULL if none */
sh4fn dc_lookup_recompiled(uint32_t addr);
void  dc_hle_register(uint32_t addr, sh4fn fn);   /* replace a guest function with native code */
/* every address in [lo, hi) runs fn, which tells them apart by c->entry (P1 fold of the address) */
void  dc_hle_register_range(uint32_t lo, uint32_t hi, sh4fn fn);
/* RAM range [dst, dst+len) holds a copy of executable code at src (found automatically for code
 * a game copies at runtime, e.g. exception vectors; call this to declare one up front). */
void  dc_alias_code(uint32_t dst, uint32_t src, uint32_t len);
void  dc_set_miss_handler(dc_miss_fn f);
void  dc_set_trap_handler(dc_trap_fn f);
void  dc_set_call_hook(dc_call_hook f);          /* sees every dc_call (tracing/profiling) */
void  dc_set_hw(const dc_hw_ops *ops);
extern uint32_t dc_call_cost;           /* SH-4 cycles charged per call (default 40) */
extern uint32_t dc_loop_cost;           /* ... per backward branch (default 16); lower both to give a game more CPU time */
/* Interpreter fallback: an address with no recompiled code is interpreted instead of reaching the
 * miss handler (which then only sees jumps into data). dc_interp_all interprets everything but
 * HLE functions (env DC_INTERP_ALL=1, for checking the recompiler); DC_INTERP=0 turns it off.
 * The notify callback sees each interpreted address once. */
extern int dc_interp_enabled, dc_interp_all;
void  dc_set_interp_notify(dc_miss_fn f);                   /* cycles charged per dispatched call (default 40) */
/* Found functions: each address in the loaded executable that had to be interpreted (no recompiled
 * function covers it) is appended to this file once, as a hex line; give the file to
 * dccore_add_recomp(... FOUND file) and the next build recompiles those functions too. NULL (the
 * default) records nothing; env DC_FOUND_FUNCS sets it; the frontend puts it next to the saves.
 * A program that switches executables (dc_use_code) points it at one file per executable. */
extern const char *dc_found_funcs_path;

/* native copy routines for a game's C library (runtime/src/crt.c): memmove / memcpy (r4 dst, r5 src,
 * r6 bytes) and memset (r4 dst, r5 byte, r6 bytes), all returning dst in r0. orig is the recompiled
 * routine, used when a range isn't all main RAM. */
enum { DC_CRT_MEMMOVE, DC_CRT_MEMCPY, DC_CRT_MEMSET, DC_CRT_COUNT };
void dc_crt_register(int which, uint32_t addr, sh4fn orig);
/* from a port's hook on the game's idle loop (it found nothing to do): time jumps ahead, as in DC_IDLE */
void dc_idle(sh4ctx *c);

#ifdef __cplusplus
}
#endif
#if defined(__GNUC__) && defined(__ELF__)
#pragma GCC visibility pop
#endif
#endif

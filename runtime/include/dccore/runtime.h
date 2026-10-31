#ifndef DCCORE_RUNTIME_H
#define DCCORE_RUNTIME_H
#include "dccore/sh4.h"
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
sh4fn dc_lookup(uint32_t addr);
void  dc_hle_register(uint32_t addr, sh4fn fn);   /* replace a guest function with native code */
/* RAM range [dst, dst+len) holds a copy of executable code at src (found automatically for code
 * a game copies at runtime, e.g. exception vectors; call this to declare one up front). */
void  dc_alias_code(uint32_t dst, uint32_t src, uint32_t len);
void  dc_set_miss_handler(dc_miss_fn f);
void  dc_set_trap_handler(dc_trap_fn f);
void  dc_set_call_hook(dc_call_hook f);          /* sees every dc_call (tracing/profiling) */
void  dc_set_hw(const dc_hw_ops *ops);
extern uint32_t dc_call_cost;                   /* cycles charged per dispatched call (default 40) */

#ifdef __cplusplus
}
#endif
#endif

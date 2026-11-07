#ifndef DCCORE_RESET_H
#define DCCORE_RESET_H
/* Resetting the game in place (the frontend's Reset): the emulated machine's state is copied once,
 * right before the game starts (dc_reset_snapshot), and put back on a reset (dc_reset_restore). Each
 * runtime module lists its machine state through dc_state_fn; configuration (renderer options, hooks a
 * port registered, the disc, VMU and flash saves) is not part of it and stays as it is. */
#include <stddef.h>
#include "sh4.h"
typedef void (*dc_state_fn)(void *p, size_t n);
void dc_hw_state(dc_state_fn f);       /* hw.c: registers, timers, events, video memory */
void dc_ta_state(dc_state_fn f);       /* ta.c */
void dc_aica_state(dc_state_fn f);     /* aica.c: sound RAM, registers, channels */
void dc_aica_drop_output(void);        /* aica.c: forget samples not read yet */
void dc_arm7_state(dc_state_fn f);     /* arm7.c */
void dc_bios_state(dc_state_fn f);     /* bios_hle.c: GD-ROM requests, CD audio */
void dc_hw_invalidate_caches(void);    /* hw.c: every video-memory page and the palette count as rewritten */
int  dc_reset_snapshot(sh4ctx *c);     /* 0 = ok */
void dc_reset_restore(sh4ctx *c);
#endif

#ifndef DCCORE_TA_H
#define DCCORE_TA_H
#include <stdint.h>
#if defined(__GNUC__) && defined(__ELF__)
#pragma GCC visibility push(hidden)             /* see dccore/sh4.h */
#endif
#ifdef __cplusplus
extern "C" {
#endif
/* kind: parameter type (0 end of list, 1 user clip, 2 object list set, 4 polygon / modifier
 * volume, 5 sprite, 7 vertex); list: 0 opaque, 1 opaque modifier, 2 translucent, 3 translucent
 * modifier, 4 punch-through; words: 8 or 16 (64-byte parameters). kind -1: TA_LIST_INIT. */
typedef void (*dc_ta_sink)(int kind, int list, const uint32_t *param, int words);
void dc_ta_set_sink(dc_ta_sink s);
void dc_ta_reset(void);                       /* TA_LIST_INIT */
void dc_ta_write32(const uint32_t blk[8]);    /* one 32-byte block */
void dc_ta_write(const uint8_t *p, uint32_t len);
/* sh4.h dc_fpref: no more vertices straight from the fast code until the list's code allows them again.
 * For whatever changes where a store queue is sent or what the TA takes its next 32 bytes for. */
void dc_ta_fast_off(void);
uint32_t dc_sq_ta_tag(void);                  /* hw.c: a >> 23 of the queue addresses that go to the TA's FIFO, unobserved (0: none) */
#ifdef __cplusplus
}
#endif
#if defined(__GNUC__) && defined(__ELF__)
#pragma GCC visibility pop
#endif
#endif

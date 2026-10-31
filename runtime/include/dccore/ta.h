#ifndef DCCORE_TA_H
#define DCCORE_TA_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* kind: parameter type (0 end of list, 1 user clip, 2 object list set, 4 polygon / modifier
 * volume, 5 sprite, 7 vertex); list: 0 opaque, 1 opaque modifier, 2 translucent, 3 translucent
 * modifier, 4 punch-through; words: 8 or 16 (64-byte parameters). */
typedef void (*dc_ta_sink)(int kind, int list, const uint32_t *param, int words);
void dc_ta_set_sink(dc_ta_sink s);
void dc_ta_reset(void);                       /* TA_LIST_INIT */
void dc_ta_write32(const uint32_t blk[8]);    /* one 32-byte block */
void dc_ta_write(const uint8_t *p, uint32_t len);
#ifdef __cplusplus
}
#endif
#endif

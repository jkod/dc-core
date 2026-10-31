#ifndef DCCORE_ARM7_H
#define DCCORE_ARM7_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct arm7_state {
    uint32_t r[16];
    uint32_t cpsr;
    uint32_t spsr[6];                 /* per bank: 0 unused, 1 fiq, 2 irq, 3 svc, 4 abt, 5 und */
    uint32_t usr_r8[5], fiq_r8[5];    /* r8..r12 of the non-FIQ modes / FIQ mode */
    uint32_t bank_r13[6], bank_r14[6];
    int fiq_line, irq_line;
} arm7_state;
extern arm7_state arm7;

void arm7_reset(void);
int  arm7_run(int cycles);            /* returns cycles executed */

/* bus, provided by aica.c (or a test) */
uint32_t arm7_read32(uint32_t a);
uint16_t arm7_read16(uint32_t a);
uint8_t  arm7_read8(uint32_t a);
void arm7_write32(uint32_t a, uint32_t v);
void arm7_write16(uint32_t a, uint16_t v);
void arm7_write8(uint32_t a, uint8_t v);
#ifdef __cplusplus
}
#endif
#endif

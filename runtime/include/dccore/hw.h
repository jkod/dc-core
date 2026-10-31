#ifndef DCCORE_HW_H
#define DCCORE_HW_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Observers for the hardware model; a renderer / sound / input backend fills these in. */
typedef struct dc_hw_hooks {
    void (*on_vblank)(uint64_t frame);
    void (*on_irq)(uint32_t intevt);
    void (*on_pvr_write)(uint32_t ofs, uint32_t v);        /* ofs from 0x005F8000 */
    void (*on_sb_write)(uint32_t addr, uint32_t v);        /* 0x005F6800.. */
    void (*on_aica_write)(uint32_t addr, uint32_t v);
    void (*on_ta_write)(uint32_t addr, uint32_t v);        /* direct 32-bit writes to the TA FIFO */
    int  (*on_sq)(uint32_t dst, const uint32_t data[8]);   /* store-queue burst; return 1 if consumed */
} dc_hw_hooks;
extern dc_hw_hooks dc_hw;
extern uint8_t dc_vram[0x800000];
extern int dc_hw_verbose;

void     dc_raise(uint32_t istnrm_bits);      /* raise Holly normal interrupts (e.g. render done) */
uint64_t dc_frame_count(void);
uint32_t dc_pvr_reg(uint32_t ofs);
uint32_t dc_sb_reg(uint32_t addr);
#ifdef __cplusplus
}
#endif
#endif

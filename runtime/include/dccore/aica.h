#ifndef DCCORE_AICA_H
#define DCCORE_AICA_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
extern uint8_t dc_aram[0x200000];                    /* sound RAM */
uint32_t dc_aica_read(uint32_t ofs, int size);       /* ofs: 0x0000..0xFFFF in the register space */
void     dc_aica_write(uint32_t ofs, uint32_t v, int size);
void     dc_aica_advance(uint64_t sh4_cycles);       /* run samples (and the ARM) up to this time */
uint64_t dc_aica_next(void);                         /* when it next needs to run */
int      dc_aica_arm_running(void);
int      dc_aica_dsp_active(void);                    /* the effects DSP has a program loaded */
/* mixed output, 44100 Hz interleaved stereo int16: copy up to `frames` frames, returns how many */
int      dc_aica_audio_read(int16_t *dst, int frames);
void     dc_set_ext_irq(uint32_t istext_bit, int on);   /* hw.c: Holly external interrupt line */
#ifdef __cplusplus
}
#endif
#endif

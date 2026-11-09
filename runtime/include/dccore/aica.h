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
/* profiling: with dc_prof_clock set (the frontend sets it for Show FPS / --stats), the time spent in the
 * sound chip (its ARM7 and the channels), in dc_prof_clock units */
extern uint64_t (*dc_prof_clock)(void);
extern uint64_t dc_prof_aica, dc_prof_aica_arm, dc_prof_aica_dsp;   /* all, its ARM7, its effects DSP */
/* the sound chip on its own thread (the frontend runs it, see dccore_front.c): with these set, the event
 * loop only tells the thread how far it may run (advance), and each SH-4 access to the chip's registers
 * or a read of its RAM is bracketed by enter (the thread catches up to `now` and pauses) and leave. The
 * chip's interrupt line to the SH-4 is then delivered on this thread by dc_aica_apply_irq. All NULL (the
 * default): everything runs inline on the game's thread. */
/* slack: SH-4 cycles the chip may lag the game when it touches the chip. The thread always runs a bit
 * behind (it is woken a chunk at a time); catching it up to the exact cycle on every access made the
 * game wait for it (8% of the Switch's race frame). Within the slack, reads of sound RAM don't even lock:
 * they see the chip as it is, a few milliseconds back. 0: exact. */
typedef struct { void (*advance)(uint64_t now); void (*enter)(uint64_t now); void (*leave)(void); uint64_t slack; } dc_aica_async_t;
extern dc_aica_async_t dc_aica_async;
extern int dc_aica_arm_cycles;                   /* the ARM's cycles per sample (256; 128 on the Switch); DC_ARM_CYCLES overrides */
extern int dc_aica_arm_idle_div;                 /* its share while throttled: 1 / this (8); DC_ARM_IDLE_DIV overrides */
extern int dc_aica_arm_throttle;                 /* slow the ARM down while nothing reaches it (aica.c); DC_ARM_THROTTLE overrides */
extern volatile unsigned dc_aica_ext_events;     /* the SH-4 wrote the chip's registers or its RAM (the ARM's idle throttle wakes) */
uint64_t dc_aica_clock(void);                      /* SH-4 cycle count the sound chip has run to */
void     dc_aica_apply_irq(void);
uint64_t dc_aica_next(void);                         /* when it next needs to run */
int      dc_aica_arm_running(void);
/* run n samples (and the ARM) now, outside the SH-4's time: a boot step that lets the sound CPU's
 * program initialise itself (an HLE operating system's sound set-up) */
void     dc_aica_run_samples(uint32_t n);
int      dc_aica_dsp_active(void);                    /* the effects DSP has a program loaded */
/* mixed output, 44100 Hz interleaved stereo int16: copy up to `frames` frames, returns how many */
int      dc_aica_audio_read(int16_t *dst, int frames);
void     dc_set_ext_irq(uint32_t istext_bit, int on);   /* hw.c: Holly external interrupt line */
#ifdef __cplusplus
}
#endif
#endif

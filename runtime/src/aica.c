/* dc-core: AICA sound chip model.
 *
 * Sound RAM (2 MB), the register file, the ARM7 that runs the game's sound driver, timers A/B/C,
 * the sample-interval tick, both interrupt controllers (SCI -> ARM FIQ, MCI -> SH-4 via Holly
 * ISTEXT bit 1) and channel playback positions (key on/off, loops, CA/LP/EG of the monitored
 * channel). Everything advances in 44.1 kHz samples driven by the SH-4 cycle clock.
 * Mixing/audio output hooks onto the same channel state (dc_aica_render, TODO).
 */
#include "dccore/aica.h"
#include "dccore/arm7.h"
#include "dccore/hw.h"
#include <string.h>

#define CPU_HZ 200000000ull
#define SAMPLE_HZ 44100ull
#define ARM_CYCLES_PER_SAMPLE 256

uint8_t dc_aram[0x200000];
static uint32_t regs[0x4000];                  /* 0x0000..0xFFFF as 32-bit words (low 16 bits used) */
#define R(o) regs[((o) & 0xFFFC) >> 2]

static uint64_t sample_clock;                  /* SH-4 cycle of the next sample */
static int arm_running;
static int tim_sub[3];                         /* sample prescaler counters for timers A/B/C */

typedef struct { int playing; double pos; int looped; } chan_t;
static chan_t ch[64];

/* ---------------------------------------------------------------- interrupts */
static void update_irqs(void) {
    uint32_t sci = R(0x28A0) & R(0x289C) & 0x7FF;
    arm7.fiq_line = sci != 0;
    uint32_t mci = R(0x28B8) & R(0x28B4) & 0x7FF;
    dc_set_ext_irq(1u << 1, mci != 0);                       /* Holly ISTEXT bit 1: AICA */
}
static void raise(uint32_t bit) { R(0x28A0) |= bit; R(0x28B8) |= bit; update_irqs(); }

/* L register: level of the lowest-numbered pending enabled interrupt */
static uint32_t level_reg(void) {
    uint32_t p = R(0x28A0) & R(0x289C) & 0x7FF;
    for (int i = 0; i < 11; i++) if (p & (1u << i)) {
        int b = i > 7 ? 7 : i;
        return ((R(0x28A8) >> b) & 1) | (((R(0x28AC) >> b) & 1) << 1) | (((R(0x28B0) >> b) & 1) << 2);
    }
    return 0;
}

/* ---------------------------------------------------------------- channels */
static uint32_t creg(int c, int r) { return R(c * 0x80 + r * 4) & 0xFFFF; }

static void key_execute(void) {
    for (int c = 0; c < 64; c++) {
        int kyonb = (creg(c, 0) >> 14) & 1;
        if (kyonb && !ch[c].playing) { ch[c].playing = 1; ch[c].pos = 0; ch[c].looped = 0; }
        else if (!kyonb && ch[c].playing) ch[c].playing = 0;
    }
}

static void advance_channels(void) {
    for (int c = 0; c < 64; c++) {
        if (!ch[c].playing) continue;
        uint32_t pitch = creg(c, 6);
        int oct = (pitch >> 11) & 15; if (oct & 8) oct -= 16;
        double step = (1.0 + (pitch & 0x3FF) / 1024.0) * (oct >= 0 ? (double)(1 << oct) : 1.0 / (1 << -oct));
        ch[c].pos += step;
        uint32_t lsa = creg(c, 2), lea = creg(c, 3);
        int loop = (creg(c, 0) >> 9) & 1;
        if (lea && ch[c].pos >= lea) {
            ch[c].looped = 1;
            if (loop && lea > lsa) ch[c].pos = lsa + (ch[c].pos - lea);
            else { ch[c].playing = 0; ch[c].pos = lea; }
        }
    }
}

/* ---------------------------------------------------------------- one sample */
static void sample_tick(void) {
    if (arm_running) arm7_run(ARM_CYCLES_PER_SAMPLE);
    advance_channels();
    for (int t = 0; t < 3; t++) {
        uint32_t reg = R(0x2890 + t * 4), pre = (reg >> 8) & 7;
        if (++tim_sub[t] >= (1 << pre)) {
            tim_sub[t] = 0;
            uint32_t cnt = (reg + 1) & 0xFF;
            R(0x2890 + t * 4) = (reg & 0x700) | cnt;
            if (cnt == 0) raise(1u << (6 + t));
        }
    }
    raise(1u << 10);                                          /* one-sample interval */
}

void dc_aica_advance(uint64_t now) {
    const uint64_t per = CPU_HZ / SAMPLE_HZ;
    if (now < sample_clock) return;
    uint64_t n = (now - sample_clock) / per + 1;
    if (n > 4410) { sample_clock = now - 4410 * per; n = 4410; }   /* never fall more than 0.1 s behind */
    while (n--) { sample_tick(); sample_clock += per; }
}
uint64_t dc_aica_next(void) { return sample_clock + 64 * (CPU_HZ / SAMPLE_HZ); }

/* ---------------------------------------------------------------- registers */
uint32_t dc_aica_read(uint32_t ofs, int size) {
    ofs &= 0xFFFF;
    uint32_t v;
    switch (ofs & 0xFFFC) {
    case 0x2810: {                                            /* LP / SGC / EG of the monitored channel */
        int c = (R(0x280C) >> 8) & 63;
        v = (ch[c].looped ? 0x8000u : 0) | (ch[c].playing ? 0 : 0x1FFF);
        ch[c].looped = 0;
        break;
    }
    case 0x2814: { int c = (R(0x280C) >> 8) & 63; v = (uint32_t)ch[c].pos & 0xFFFF; break; }   /* CA */
    case 0x2D00: v = level_reg(); break;                      /* L */
    default: v = R(ofs);
    }
    if (size == 4) return v;
    if (size == 2) return (v >> ((ofs & 2) * 8)) & 0xFFFF;
    return (v >> ((ofs & 3) * 8)) & 0xFF;
}

void dc_aica_write(uint32_t ofs, uint32_t v, int size) {
    ofs &= 0xFFFF;
    uint32_t w = ofs & 0xFFFC, old = R(w), nv;
    if (size == 4) nv = v;
    else if (size == 2) { int sh = (ofs & 2) * 8; nv = (old & ~(0xFFFFu << sh)) | ((v & 0xFFFF) << sh); }
    else { int sh = (ofs & 3) * 8; nv = (old & ~(0xFFu << sh)) | ((v & 0xFF) << sh); }
    switch (w) {
    case 0x28A0: if (nv & 0x20) { R(0x28A0) |= 0x20; update_irqs(); } return;                  /* SCIPD bit 5: SH-4 -> ARM */
    case 0x28A4: R(0x28A0) &= ~nv; update_irqs(); return;                                        /* SCIRE */
    case 0x28B8: if (nv & 0x20) { R(0x28B8) |= 0x20; update_irqs(); } return;                    /* MCIPD: ARM -> SH-4 */
    case 0x28BC: R(0x28B8) &= ~nv; update_irqs(); return;                                        /* MCIRE */
    case 0x2D04: if (nv & 1) update_irqs(); return;                                              /* M: RP re-evaluates */
    case 0x2C00:                                                                                  /* ARMRST */
        R(w) = nv;
        if (!(nv & 1) && !arm_running) { arm7_reset(); arm_running = 1; }
        else if (nv & 1) arm_running = 0;
        return;
    }
    R(w) = nv;
    if (w < 0x2000 && (w & 0x7C) == 0 && (nv & 0x8000)) {     /* KYONEX in any channel's reg 0 */
        R(w) = nv & ~0x8000u;
        key_execute();
    }
    if (w == 0x289C || w == 0x28B4) update_irqs();
}

/* ---------------------------------------------------------------- ARM bus */
static inline int is_reg(uint32_t a) { return (a & 0x00F00000u) == 0x00800000u; }
uint32_t arm7_read32(uint32_t a) { if (is_reg(a)) return dc_aica_read(a, 4); uint32_t v; memcpy(&v, dc_aram + (a & 0x1FFFFC), 4); return v; }
uint16_t arm7_read16(uint32_t a) { if (is_reg(a)) return (uint16_t)dc_aica_read(a, 2); uint16_t v; memcpy(&v, dc_aram + (a & 0x1FFFFE), 2); return v; }
uint8_t  arm7_read8(uint32_t a)  { if (is_reg(a)) return (uint8_t)dc_aica_read(a, 1); return dc_aram[a & 0x1FFFFF]; }
void arm7_write32(uint32_t a, uint32_t v) { if (is_reg(a)) { dc_aica_write(a, v, 4); return; } memcpy(dc_aram + (a & 0x1FFFFC), &v, 4); }
void arm7_write16(uint32_t a, uint16_t v) { if (is_reg(a)) { dc_aica_write(a, v, 2); return; } memcpy(dc_aram + (a & 0x1FFFFE), &v, 2); }
void arm7_write8(uint32_t a, uint8_t v)   { if (is_reg(a)) { dc_aica_write(a, v, 1); return; } dc_aram[a & 0x1FFFFF] = v; }

int dc_aica_arm_running(void) { return arm_running; }

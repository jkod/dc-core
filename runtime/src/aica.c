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
#include <math.h>

#define CPU_HZ 200000000ull
#define SAMPLE_HZ 44100ull
#define ARM_CYCLES_PER_SAMPLE 256

uint8_t dc_aram[0x200000];
static uint32_t regs[0x4000];                  /* 0x0000..0xFFFF as 32-bit words (low 16 bits used) */
#define R(o) regs[((o) & 0xFFFC) >> 2]

static uint64_t sample_clock;                  /* SH-4 cycle of the next sample */
static int arm_running;
static int tim_sub[3];                         /* sample prescaler counters for timers A/B/C */

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


/* Channel registers (16 bits each, 4 bytes apart, 0x80 per channel):
 *  0 KYONEX KYONB SSCTL LPCTL PCMS(8:7) SA[22:16]   1 SA[15:0]   2 LSA   3 LEA
 *  4 D2R(15:11) D1R(10:6) AR(4:0)   5 LPSLNK KRS(13:10) DL(9:5) RR(4:0)   6 OCT(14:11) FNS(9:0)
 *  9 DISDL(11:8) DIPAN(4:0)   10 TL(15:8)
 * Envelope attenuation is 10 bits (0 loudest, 0x3FF silent); EG readback is 13 bits. */
enum { EG_ATTACK, EG_DECAY1, EG_DECAY2, EG_RELEASE };
typedef struct {
    int playing, looped;
    double pos;                       /* sample position (fractional) */
    int eg_state; double att;         /* envelope */
    /* ADPCM */
    int32_t adpcm_sig, adpcm_step; uint32_t adpcm_at;   /* decoded up to this sample */
    int32_t loop_sig, loop_step; int loop_saved;
    int16_t cur;                      /* last decoded sample */
} chan_t;
static chan_t ch[64];

/* full-scale envelope times in ms per effective rate (0..63), after MAME's AICA */
static const double ar_ms[64] = { 1e9, 1e9, 8100, 6900, 6000, 4800, 4000, 3400, 3000, 2400, 2000, 1700, 1500, 1200, 1000, 860,
    760, 600, 500, 430, 380, 300, 250, 220, 190, 150, 130, 110, 95, 76, 63, 55, 47, 38, 31, 27, 24, 19, 15, 13, 12, 9.4, 7.9, 6.8,
    6.0, 4.7, 3.8, 3.4, 3.0, 2.4, 2.0, 1.8, 1.6, 1.3, 1.1, 0.93, 0.85, 0.65, 0.53, 0.44, 0.40, 0.35, 0, 0 };
static const double dr_ms[64] = { 1e9, 1e9, 118200, 101300, 88600, 70900, 59100, 50700, 44300, 35500, 29600, 25300, 22200, 17700,
    14800, 12700, 11100, 8900, 7400, 6300, 5500, 4400, 3700, 3200, 2800, 2200, 1800, 1600, 1400, 1100, 920, 790, 690, 550, 460,
    390, 340, 270, 230, 200, 170, 140, 110, 98, 85, 68, 57, 49, 43, 34, 28, 25, 22, 18, 14, 12, 11, 8.5, 7.1, 6.1, 5.4, 4.3, 3.6, 3.1 };

static uint32_t creg(int c, int r) { return R(c * 0x80 + r * 4) & 0xFFFF; }

static int eff_rate(int c, int r) {
    if (r == 0) return 0;
    uint32_t pitch = creg(c, 6), krs = (creg(c, 5) >> 10) & 15;
    int oct = (pitch >> 11) & 15; if (oct & 8) oct -= 16;
    int rate = 2 * r;
    if (krs != 15) rate += (int)krs + oct * 2 + (int)((pitch >> 9) & 1);
    return rate < 0 ? 0 : rate > 63 ? 63 : rate;
}
/* attenuation change per sample for a full 0x3FF sweep in `ms` */
static double eg_step(double ms) { return ms <= 0 ? 1024.0 : 1024.0 / (ms * 44.1); }

static void key_on(int c) {
    chan_t *h = &ch[c];
    h->playing = 1; h->pos = 0; h->looped = 0;
    h->eg_state = EG_ATTACK; h->att = 0x3FF;
    if (eff_rate(c, creg(c, 4) & 31) >= 62) h->att = 0;     /* instant attack */
    h->adpcm_sig = 0; h->adpcm_step = 127; h->adpcm_at = 0; h->loop_saved = 0; h->cur = 0;
}
static void key_execute(void) {
    for (int c = 0; c < 64; c++) {
        int kyonb = (creg(c, 0) >> 14) & 1;
        if (kyonb && !ch[c].playing) key_on(c);
        else if (!kyonb && ch[c].playing && ch[c].eg_state != EG_RELEASE) ch[c].eg_state = EG_RELEASE;
    }
}

static void envelope(int c) {
    chan_t *h = &ch[c];
    uint32_t r4 = creg(c, 4), r5 = creg(c, 5);
    switch (h->eg_state) {
    case EG_ATTACK:
        h->att -= eg_step(ar_ms[eff_rate(c, r4 & 31)]);
        if (h->att <= 0) { h->att = 0; h->eg_state = EG_DECAY1; }
        break;
    case EG_DECAY1: {
        double dl = (double)(((r5 >> 5) & 31) << 5);               /* decay level, in attenuation units */
        h->att += eg_step(dr_ms[eff_rate(c, (r4 >> 6) & 31)]);
        if (h->att >= dl) { h->att = dl; h->eg_state = EG_DECAY2; }
        break;
    }
    case EG_DECAY2:
        h->att += eg_step(dr_ms[eff_rate(c, (r4 >> 11) & 31)]);
        if (h->att > 0x3FF) h->att = 0x3FF;
        break;
    default:
        h->att += eg_step(dr_ms[eff_rate(c, r5 & 31)]);
        if (h->att >= 0x3FF) { h->att = 0x3FF; h->playing = 0; }
        break;
    }
}

/* Yamaha 4-bit ADPCM */
static const int adpcm_scale[8] = { 0x0E6, 0x0E6, 0x0E6, 0x0E6, 0x133, 0x199, 0x200, 0x266 };
static void adpcm_one(chan_t *h, int nib) {
    int delta = (h->adpcm_step * ((nib & 7) * 2 + 1)) >> 3;
    if (nib & 8) delta = -delta;
    h->adpcm_sig += delta;
    if (h->adpcm_sig > 32767) h->adpcm_sig = 32767; if (h->adpcm_sig < -32768) h->adpcm_sig = -32768;
    h->adpcm_step = (h->adpcm_step * adpcm_scale[nib & 7]) >> 8;
    if (h->adpcm_step < 127) h->adpcm_step = 127; if (h->adpcm_step > 24576) h->adpcm_step = 24576;
}

static int16_t fetch(int c) {
    chan_t *h = &ch[c];
    uint32_t r0 = creg(c, 0), sa = ((r0 & 0x7F) << 16) | creg(c, 1), fmt = (r0 >> 7) & 3;
    uint32_t i = (uint32_t)h->pos;
    if (fmt == 0) { int16_t v; memcpy(&v, dc_aram + ((sa + i * 2) & 0x1FFFFE), 2); return v; }
    if (fmt == 1) return (int16_t)((int8_t)dc_aram[(sa + i) & 0x1FFFFF] << 8);
    /* ADPCM: decode forward up to sample i (state restored at the loop start) */
    uint32_t lsa = creg(c, 2);
    if (i < h->adpcm_at) {                                     /* jumped back: loop */
        if (h->loop_saved && i >= lsa) { h->adpcm_sig = h->loop_sig; h->adpcm_step = h->loop_step; h->adpcm_at = lsa; }
        else { h->adpcm_sig = 0; h->adpcm_step = 127; h->adpcm_at = 0; }
    }
    while (h->adpcm_at <= i) {
        if (h->adpcm_at == lsa && !h->loop_saved) { h->loop_sig = h->adpcm_sig; h->loop_step = h->adpcm_step; h->loop_saved = 1; }
        uint8_t b = dc_aram[(sa + (h->adpcm_at >> 1)) & 0x1FFFFF];
        adpcm_one(h, (h->adpcm_at & 1) ? b >> 4 : b & 15);
        h->adpcm_at++;
    }
    return (int16_t)h->adpcm_sig;
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

/* ---------------------------------------------------------------- mixing */
#define RING 16384                                   /* stereo frames */
static int16_t ring[RING * 2];
static uint32_t ring_w, ring_r;
/* all level terms are multiples of 3/32 dB: envelope 1 step, TL 4, send level / pan 32 */
static float gain_tab[4096];
static float gain(int units) { return units >= 4096 ? 0.f : gain_tab[units]; }
static void mix_sample(void) {
    if (gain_tab[0] == 0) for (int i = 0; i < 4096; i++) gain_tab[i] = powf(10.f, -(i * 3.f / 32.f) / 20.f);
    float l = 0, r = 0;
    for (int c = 0; c < 64; c++) {
        chan_t *h = &ch[c];
        if (!h->playing) continue;
        h->cur = fetch(c);
        envelope(c);
        uint32_t r9 = creg(c, 9), disdl = (r9 >> 8) & 15, dipan = r9 & 31, tl = creg(c, 10) >> 8;
        if (!disdl) continue;
        int units = (int)h->att + 4 * (int)tl + 32 * (int)(15 - disdl);
        float g = gain(units) * (h->cur / 32768.f), gl = g, gr = g;
        int pan = (int)(dipan & 15);
        if (pan == 15) { if (dipan & 16) gr = 0; else gl = 0; }
        else if (dipan & 16) gl *= gain(32 * pan); else gr *= gain(32 * pan);
        l += gl; r += gr;
    }
    float mv = gain(32 * (int)(15 - (R(0x2800) & 15))) * 32767.f * 0.5f;   /* headroom for many voices */
    l *= mv; r *= mv;
    int il = l > 32767 ? 32767 : l < -32768 ? -32768 : (int)l, ir = r > 32767 ? 32767 : r < -32768 ? -32768 : (int)r;
    if (ring_w - ring_r >= RING) ring_r++;                       /* nobody listening: drop the oldest */
    ring[(ring_w % RING) * 2] = (int16_t)il; ring[(ring_w % RING) * 2 + 1] = (int16_t)ir; ring_w++;
}
int dc_aica_audio_read(int16_t *dst, int frames) {
    int n = 0;
    while (n < frames && ring_r != ring_w) { dst[n * 2] = ring[(ring_r % RING) * 2]; dst[n * 2 + 1] = ring[(ring_r % RING) * 2 + 1]; ring_r++; n++; }
    return n;
}

/* ---------------------------------------------------------------- one sample */
static void sample_tick(void) {
    if (arm_running) arm7_run(ARM_CYCLES_PER_SAMPLE);
    mix_sample();
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
        v = (ch[c].looped ? 0x8000u : 0) | (ch[c].playing ? (uint32_t)ch[c].eg_state << 13 | ((uint32_t)ch[c].att << 3) : 0x1FFF);
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

/* dc-core: AICA sound chip model.
 *
 * Sound RAM (2 MB), the register file, the ARM7 that runs the game's sound driver, timers A/B/C,
 * the sample-interval tick, both interrupt controllers (SCI -> ARM FIQ, MCI -> SH-4 via Holly
 * ISTEXT bit 1) and channel playback positions (key on/off, loops, CA/LP/EG of the monitored
 * channel). Everything advances in 44.1 kHz samples driven by the SH-4 cycle clock.
 * Mixing/audio output hooks onto the same channel state (dc_aica_render, TODO).
 */
#include <stdlib.h>
#include "dccore/aica.h"
#include "dccore/bios.h"
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
dc_aica_async_t dc_aica_async;
volatile unsigned dc_aica_ext_events;
static unsigned arm_wakes, arm_regwrites;
static volatile int irq_want = -1;
static void update_irqs(void) {
    uint32_t sci = R(0x28A0) & R(0x289C) & 0x7FF;
    if (sci && !arm7.fiq_line) arm_wakes++;                   /* an interrupt for the ARM: full speed again */
    arm7.fiq_line = sci != 0;
    uint32_t mci = R(0x28B8) & R(0x28B4) & 0x7FF;
    if (dc_aica_async.advance) irq_want = mci != 0;           /* delivered on the game's thread */
    else dc_set_ext_irq(1u << 1, mci != 0);                  /* Holly ISTEXT bit 1: AICA */
}
void dc_aica_apply_irq(void) { int w = irq_want; if (w >= 0) dc_set_ext_irq(1u << 1, w); }
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
 *  9 DISDL(11:8) DIPAN(4:0)   10 TL(15:8) LPOFF(5) Q(4:0)   11..15 FLV0..FLV4 (12:0)
 *  16 FAR(12:8) FD1R(4:0)   17 FD2R(12:8) FRR(4:0)
 * Envelope attenuation is 10 bits (0 loudest, 0x3FF silent); EG readback is 13 bits. */
enum { EG_ATTACK, EG_DECAY1, EG_DECAY2, EG_RELEASE };
typedef struct {
    int playing, looped;
    int keyed;                        /* keyed on and not keyed off since (a sample that ran out stays keyed) */
    double pos;                       /* sample position (fractional) */
    int eg_state; double att;         /* envelope */
    /* ADPCM */
    int32_t adpcm_sig, adpcm_step; uint32_t adpcm_at;   /* decoded up to this sample */
    int32_t adpcm_prev;               /* the sample before adpcm_sig (for interpolation) */
    int32_t loop_sig, loop_step; int loop_saved;
    int16_t cur;                      /* last decoded sample */
    /* low-pass filter: its own envelope (13-bit cutoff, FLV0 -> FLV1 -> FLV2 -> FLV3, release to FLV4) */
    int feg_state; double feg;
    float lp_low, lp_band;
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
    h->playing = 1; h->keyed = 1; h->pos = 0; h->looped = 0;
    h->eg_state = EG_ATTACK; h->att = 0x3FF;
    if (eff_rate(c, creg(c, 4) & 31) >= 62) h->att = 0;     /* instant attack */
    h->adpcm_sig = 0; h->adpcm_step = 127; h->adpcm_at = 0; h->loop_saved = 0; h->cur = 0; h->adpcm_prev = 0;
    h->feg_state = EG_ATTACK; h->feg = creg(c, 11) & 0x1FFF; h->lp_low = h->lp_band = 0;
}
static void key_execute(void) {
    for (int c = 0; c < 64; c++) {
        int kyonb = (creg(c, 0) >> 14) & 1;
        /* Key on only a slot that is keyed off (stopped or releasing), as the AICA does: a one-shot
         * sample that ran out keeps its slot keyed, and games leave KYONB set on it (Soulcalibur), so
         * restarting every finished KYONB slot replayed old effects with each new sound. */
        if (kyonb && !ch[c].keyed) key_on(c);
        else if (!kyonb && ch[c].keyed) { ch[c].keyed = 0; if (ch[c].playing) ch[c].eg_state = ch[c].feg_state = EG_RELEASE; }
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

/* The voice's low-pass filter (on unless LPOFF): a 2-pole resonant low-pass whose cutoff follows
 * its own envelope. The 13-bit cutoff is a float, exponent (12:9) and mantissa (8:0 with a hidden
 * bit), so each exponent step is an octave; 0x1FFF is wide open. The envelope moves from FLV0 to
 * FLV1 (FAR), FLV2 (FD1R), FLV3 (FD2R) and on key off to FLV4 (FRR), at the amplitude envelope's
 * decay speeds over a range 8 times as long. Q (0..31) is the resonance, 0.75 dB a step from -3 dB.
 * Done here as a state-variable filter. */
static float filter(int c, float x) {
    chan_t *h = &ch[c];
    uint32_t r10 = creg(c, 10);
    if (r10 & 0x20) return x;                                  /* LPOFF */
    static const int target_reg[4] = { 12, 13, 14, 15 };
    uint32_t r16 = creg(c, 16), r17 = creg(c, 17);
    int rate = h->feg_state == EG_ATTACK ? (int)((r16 >> 8) & 31) : h->feg_state == EG_DECAY1 ? (int)(r16 & 31)
             : h->feg_state == EG_DECAY2 ? (int)((r17 >> 8) & 31) : (int)(r17 & 31);
    double target = creg(c, target_reg[h->feg_state]) & 0x1FFF, d = eg_step(dr_ms[eff_rate(c, rate)]);
    if (h->feg < target) { h->feg += d; if (h->feg >= target) h->feg = target; }
    else if (h->feg > target) { h->feg -= d; if (h->feg <= target) h->feg = target; }
    if (h->feg == target && (h->feg_state == EG_ATTACK || h->feg_state == EG_DECAY1)) h->feg_state++;
    uint32_t fv = (uint32_t)h->feg, e = fv >> 9, m = (fv & 0x1FF) | 0x200;
    float fn = (float)m * (float)(1u << e) / 33554432.f;      /* cutoff, 1 = half the sample rate */
    if (fn >= 0.98f) { h->lp_low = x; h->lp_band = 0; return x; }
    static float damp[32];
    if (damp[0] == 0) for (int q = 0; q < 32; q++) damp[q] = 1.f / powf(10.f, (0.75f * q - 3.f) / 20.f);
    float F = 2.f * sinf(3.14159265f * fn * 0.5f), dq = damp[r10 & 31];
    if (F > 1.9f - dq) F = 1.9f - dq;                           /* keep the filter stable near the top */
    if (F < 0.f) F = 0.f;
    h->lp_low += F * h->lp_band;
    float hp = x - h->lp_low - dq * h->lp_band;
    h->lp_band += F * hp;
    return h->lp_low;
}

/* Yamaha 4-bit ADPCM */
static const int adpcm_scale[8] = { 0x0E6, 0x0E6, 0x0E6, 0x0E6, 0x133, 0x199, 0x200, 0x266 };
static void adpcm_one(chan_t *h, int nib) {
    int delta = (h->adpcm_step * ((nib & 7) * 2 + 1)) >> 3;
    if (delta > 32767) delta = 32767;
    if (nib & 8) delta = -delta;
    h->adpcm_sig += delta;
    if (h->adpcm_sig > 32767) h->adpcm_sig = 32767; if (h->adpcm_sig < -32768) h->adpcm_sig = -32768;
    h->adpcm_step = (h->adpcm_step * adpcm_scale[nib & 7]) >> 8;
    if (h->adpcm_step < 127) h->adpcm_step = 127; if (h->adpcm_step > 24576) h->adpcm_step = 24576;
}

/* ADPCM: decode forward so that adpcm_sig is sample t and adpcm_prev sample t-1. When the position
 * jumps back to the loop start, PCMS 2 restores the decoder state it had there; PCMS 3 ("long
 * stream", a ring buffer the program keeps refilling) carries on with the state it has */
static void adpcm_to(chan_t *h, uint32_t sa, uint32_t lsa, uint32_t t, int stream) {
    if (h->adpcm_at == t + 1) return;                          /* already there (pitch < 1) */
    if (t < h->adpcm_at && stream && t >= lsa) {               /* looped: keep decoding from the loop start */
        h->adpcm_at = lsa;
        while (h->adpcm_at < t) {
            uint8_t b = dc_aram[(sa + (h->adpcm_at >> 1)) & 0x1FFFFF];
            h->adpcm_prev = h->adpcm_sig; adpcm_one(h, (h->adpcm_at & 1) ? b >> 4 : b & 15); h->adpcm_at++;
        }
    } else if (t < h->adpcm_at) {                              /* jumped back: loop */
        if (h->loop_saved && t >= lsa) { h->adpcm_sig = h->loop_sig; h->adpcm_step = h->loop_step; h->adpcm_at = lsa; }
        else { h->adpcm_sig = 0; h->adpcm_step = 127; h->adpcm_at = 0; }
        h->adpcm_prev = h->adpcm_sig;
    }
    while (h->adpcm_at <= t) {
        if (h->adpcm_at == lsa && !h->loop_saved) { h->loop_sig = h->adpcm_sig; h->loop_step = h->adpcm_step; h->loop_saved = 1; }
        uint8_t b = dc_aram[(sa + (h->adpcm_at >> 1)) & 0x1FFFFF];
        h->adpcm_prev = h->adpcm_sig;
        adpcm_one(h, (h->adpcm_at & 1) ? b >> 4 : b & 15);
        h->adpcm_at++;
    }
}

/* the channel's output at its fractional position: linear interpolation between neighbouring
 * samples, as the AICA does */
static int16_t fetch(int c) {
    chan_t *h = &ch[c];
    uint32_t r0 = creg(c, 0), sa = ((r0 & 0x7F) << 16) | creg(c, 1), fmt = (r0 >> 7) & 3;
    uint32_t i = (uint32_t)h->pos;
    double fr = h->pos - (double)i;
    int32_t s0, s1;
    if (fmt == 0) {
        int16_t a, b; memcpy(&a, dc_aram + ((sa + i * 2) & 0x1FFFFE), 2); memcpy(&b, dc_aram + ((sa + i * 2 + 2) & 0x1FFFFE), 2);
        s0 = a; s1 = b;
    } else if (fmt == 1) {
        s0 = (int8_t)dc_aram[(sa + i) & 0x1FFFFF] << 8; s1 = (int8_t)dc_aram[(sa + i + 1) & 0x1FFFFF] << 8;
    } else {
        /* the last sample before the loop end: its successor is the loop start, not the nibble past
         * the end, so decode only up to it (decoding past the end put a stray nibble into a long
         * stream's state at every wrap: a DC offset that drifted the music up and down) */
        uint32_t lea = creg(c, 3); if (fmt == 3) lea = (lea + 3) & ~3u;
        int last = lea && i + 1 >= lea;
        adpcm_to(h, sa, creg(c, 2), last ? i : i + 1, fmt == 3);
        s0 = last ? h->adpcm_sig : h->adpcm_prev; s1 = h->adpcm_sig;
    }
    return (int16_t)(s0 + (int32_t)((double)(s1 - s0) * fr));
}

static void advance_channels(void) {
    for (int c = 0; c < 64; c++) {
        if (!ch[c].playing) continue;
        uint32_t pitch = creg(c, 6);
        int oct = (pitch >> 11) & 15; if (oct & 8) oct -= 16;
        double step = (1.0 + (pitch & 0x3FF) / 1024.0) * (oct >= 0 ? (double)(1 << oct) : 1.0 / (1 << -oct));
        ch[c].pos += step;
        uint32_t lsa = creg(c, 2), lea = creg(c, 3);
        if (((creg(c, 0) >> 7) & 3) == 3) lea = (lea + 3) & ~3u;   /* an ADPCM long stream ends on a 4-sample boundary */
        int loop = (creg(c, 0) >> 9) & 1;
        /* LPSLNK: the attack ends when playback reaches the loop start */
        if (((creg(c, 5) >> 14) & 1) && ch[c].eg_state == EG_ATTACK && ch[c].pos >= lsa) ch[c].eg_state = EG_DECAY1;
        if (lea && ch[c].pos >= lea) {
            ch[c].looped = 1;
            if (loop && lea > lsa) ch[c].pos = lsa + (ch[c].pos - lea);
            else { ch[c].playing = 0; ch[c].pos = lea; }
        }
    }
}

/* ---------------------------------------------------------------- effects DSP
 * 128-step microprogram run once per sample (reverb, echo, chorus). Channels send to the 16 MIXS
 * inputs (ISEL / IMXL), the program works on 24-bit values with a ring buffer in sound RAM
 * (RBP / RBL), and its 16 EFREG outputs are mixed back with EFSDL / EFPAN. The AICA's own
 * instruction layout, not the Saturn SCSP's: each step uses its own COEF (COEF[step]), 64 MADRS,
 * and a memory read reaches MEMS two steps later.
 *   COEF 0x3000 (128 x 13 bit), MADRS 0x3200 (64), MPRO 0x3400 (128 x 64 bit), EFSDL/EFPAN 0x2000 */
static struct {
    int32_t temp[128], mems[32], mixs[16], efreg[16];
    int32_t acc, shifted, x, y, b, inputs, memval[4], frc, yreg, adrs;
    uint32_t dec;
    int last_step;                                  /* -1 = program changed, recount */
} dsp = { .last_step = -1 };

static uint16_t dsp_pack(int32_t val) {
    uint32_t sign = (val >> 23) & 1, temp = (uint32_t)(val ^ (val << 1)) & 0xFFFFFF;
    int e = 0;
    while (e < 12 && !(temp & 0x800000)) { temp <<= 1; e++; }
    if (e < 12) val = (val << e) & 0x3FFFFF; else val <<= 11;
    val >>= 11;
    return (uint16_t)(sign << 15 | (uint32_t)e << 11 | ((uint32_t)val & 0x7FF));
}
static int32_t dsp_unpack(uint16_t v) {
    int sign = (v >> 15) & 1, e = (v >> 11) & 15;
    int32_t u = (int32_t)(v & 0x7FF) << 11;
    if (e > 11) { e = 11; u |= sign << 22; } else u |= (sign ^ 1) << 22;
    u |= sign << 23;
    u = (int32_t)((uint32_t)u << 8) >> 8;
    return u >> e;
}
static inline int32_t sx24(int32_t v) { return (int32_t)((uint32_t)v << 8) >> 8; }
static inline int32_t clamp24(int32_t v) { return v < -0x800000 ? -0x800000 : v > 0x7FFFFF ? 0x7FFFFF : v; }
#define MPRO(st, k) (R(0x3400 + (st) * 16 + (k) * 4) & 0xFFFF)

static void dsp_step(void) {
    if (dsp.last_step < 0) {
        dsp.last_step = 0;
        for (int st = 127; st >= 0; st--)
            if (MPRO(st, 0) | MPRO(st, 1) | MPRO(st, 2) | MPRO(st, 3)) { dsp.last_step = st + 1; break; }
    }
    memset(dsp.efreg, 0, sizeof dsp.efreg);
    if (!dsp.last_step) { memset(dsp.mixs, 0, sizeof dsp.mixs); return; }
    uint32_t common = R(0x2804) & 0xFFFF, rbp = common & 0xFFF, rbl = 0x2000u << ((common >> 13) & 3);
    uint16_t *ram = (uint16_t *)dc_aram;
    for (int st = 0; st < dsp.last_step; st++) {
        uint32_t i0 = MPRO(st, 0), i1 = MPRO(st, 1), i2 = MPRO(st, 2), i3 = MPRO(st, 3);
        if (!(i0 | i1 | i2 | i3)) {                         /* empty step: the multiplier keeps running */
            dsp.x = sx24(dsp.temp[dsp.dec & 0x7F]); dsp.y = dsp.frc;
            int32_t y13 = (int32_t)((uint32_t)dsp.y << 19) >> 19;
            dsp.acc = (int32_t)(((int64_t)dsp.x * y13) >> 12) + dsp.x;
            continue;
        }
        uint32_t TRA = (i0 >> 9) & 0x7F, TWT = (i0 >> 8) & 1, TWA = (i0 >> 1) & 0x7F;
        uint32_t XSEL = (i1 >> 15) & 1, YSEL = (i1 >> 13) & 3, IRA = (i1 >> 7) & 0x3F, IWT = (i1 >> 6) & 1, IWA = (i1 >> 1) & 0x1F;
        uint32_t TABLE = (i2 >> 15) & 1, MWT = (i2 >> 14) & 1, MRD = (i2 >> 13) & 1, EWT = (i2 >> 12) & 1, EWA = (i2 >> 8) & 15;
        uint32_t ADRL = (i2 >> 7) & 1, FRCL = (i2 >> 6) & 1, SHIFT = (i2 >> 4) & 3, YRL = (i2 >> 3) & 1;
        uint32_t NEGB = (i2 >> 2) & 1, ZERO = (i2 >> 1) & 1, BSEL = i2 & 1;
        uint32_t MASA = (i3 >> 9) & 0x3F, ADREB = (i3 >> 8) & 1, NXADR = (i3 >> 7) & 1;

        if (IRA <= 0x1F) dsp.inputs = dsp.mems[IRA];
        else if (IRA <= 0x2F) dsp.inputs = dsp.mixs[IRA - 0x20] << 4;          /* 20 -> 24 bit */
        else dsp.inputs = 0;                                                    /* EXTS (CD audio): none */
        dsp.inputs = sx24(dsp.inputs);
        if (IWT) dsp.mems[IWA] = dsp.memval[st & 3];                            /* read two steps ago */

        if (ZERO) dsp.b = 0;
        else {
            dsp.b = BSEL ? dsp.acc : sx24(dsp.temp[(TRA + dsp.dec) & 0x7F]);
            if (NEGB) dsp.b = -dsp.b;
        }
        dsp.x = XSEL ? dsp.inputs : sx24(dsp.temp[(TRA + dsp.dec) & 0x7F]);
        if (YSEL == 0) dsp.y = dsp.frc;
        else if (YSEL == 1) dsp.y = (int32_t)(int16_t)(R(0x3000 + st * 4) & 0xFFFF) >> 3;
        else if (YSEL == 2) dsp.y = dsp.yreg >> 11;
        else dsp.y = (dsp.yreg >> 4) & 0x0FFF;
        if (YRL) dsp.yreg = dsp.inputs;

        if (SHIFT == 0) dsp.shifted = clamp24(dsp.acc);
        else if (SHIFT == 1) dsp.shifted = clamp24(dsp.acc * 2);
        else if (SHIFT == 2) dsp.shifted = sx24(dsp.acc * 2);
        else dsp.shifted = sx24(dsp.acc);

        int32_t y13 = (int32_t)((uint32_t)dsp.y << 19) >> 19;
        dsp.acc = (int32_t)(((int64_t)dsp.x * y13) >> 12) + dsp.b;

        if (TWT) dsp.temp[(TWA + dsp.dec) & 0x7F] = dsp.shifted;
        if (FRCL) dsp.frc = SHIFT == 3 ? dsp.shifted & 0xFFF : (dsp.shifted >> 11) & 0x1FFF;

        if ((MRD || MWT) && (st & 1)) {                     /* memory access on odd steps */
            uint32_t addr = R(0x3200 + MASA * 4) & 0xFFFF;
            if (ADREB) addr += (uint32_t)dsp.adrs & 0xFFF;
            if (NXADR) addr++;
            if (!TABLE) addr = (addr + dsp.dec) & (rbl - 1); else addr &= 0xFFFF;
            addr = (addr + (rbp << 10)) & 0xFFFFF;          /* 16-bit words in 2 MB */
            if (MRD) dsp.memval[(st + 2) & 3] = dsp_unpack(ram[addr]);
            if (MWT) ram[addr] = dsp_pack(dsp.shifted);
        }
        if (ADRL) dsp.adrs = SHIFT == 3 ? (dsp.shifted >> 12) & 0xFFF : dsp.inputs >> 16;
        if (EWT) dsp.efreg[EWA] = dsp.shifted >> 8;
    }
    dsp.dec--;
    memset(dsp.mixs, 0, sizeof dsp.mixs);
}
int dc_aica_dsp_active(void) { return dsp.last_step > 0; }

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
        {
            float y = filter(c, (float)fetch(c));
            h->cur = (int16_t)(y > 32767.f ? 32767.f : y < -32768.f ? -32768.f : y);
        }
        envelope(c);
        uint32_t r9 = creg(c, 9), disdl = (r9 >> 8) & 15, dipan = r9 & 31, tl = creg(c, 10) >> 8;
        uint32_t r8 = creg(c, 8), imxl = (r8 >> 4) & 15, isel = r8 & 15;
        if (imxl) {                                                  /* effect send */
            float sg = gain((int)h->att + 4 * (int)tl + 32 * (int)(15 - imxl));
            dsp.mixs[isel] += (int32_t)(h->cur * sg) << 4;           /* 20-bit input */
        }
        if (!disdl) continue;
        int units = (int)h->att + 4 * (int)tl + 32 * (int)(15 - disdl);
        float g = gain(units) * (h->cur / 32768.f), gl = g, gr = g;
        int pan = (int)(dipan & 15);
        /* DIPAN/EFPAN: 0x1n turns the right side down by 3 dB per n, 0x0n the left; n = 15 mutes it */
        if (dipan & 16) gr *= pan == 15 ? 0.f : gain(32 * pan); else gl *= pan == 15 ? 0.f : gain(32 * pan);
        l += gl; r += gr;
    }
    dsp_step();
    for (int i = 0; i < 16; i++) {                                   /* effect returns */
        if (!dsp.efreg[i]) continue;
        uint32_t e = R(0x2000 + i * 4) & 0xFFFF, efsdl = (e >> 8) & 15, efpan = e & 31;
        if (!efsdl) continue;
        float g = gain(32 * (int)(15 - efsdl)) * (float)(int16_t)dsp.efreg[i] / 32768.f, gl = g, gr = g;
        int pan = (int)(efpan & 15);
        /* DIPAN/EFPAN: 0x1n turns the right side down by 3 dB per n, 0x0n the left; n = 15 mutes it */
        if (efpan & 16) gr *= pan == 15 ? 0.f : gain(32 * pan); else gl *= pan == 15 ? 0.f : gain(32 * pan);
        l += gl; r += gr;
    }
    {                                                                /* CD audio: external inputs 16 (L), 17 (R) */
        int16_t cl, cr;
        if (dc_cdda_frame(&cl, &cr))
            for (int i = 0; i < 2; i++) {
                uint32_t e = R(0x2000 + (16 + i) * 4) & 0xFFFF, efsdl = (e >> 8) & 15, efpan = e & 31;
                if (!efsdl) continue;
                float g = gain(32 * (int)(15 - efsdl)) * (float)(i ? cr : cl) / 32768.f, gl = g, gr = g;
                int pan = (int)(efpan & 15);
                /* DIPAN/EFPAN: 0x1n turns the right side down by 3 dB per n, 0x0n the left; n = 15 mutes it */
                if (efpan & 16) gr *= pan == 15 ? 0.f : gain(32 * pan); else gl *= pan == 15 ? 0.f : gain(32 * pan);
                l += gl; r += gr;
            }
    }
    float mv = gain(32 * (int)(15 - (R(0x2800) & 15))) * 32767.f * 0.5f;   /* headroom: no voice filter yet, so bright voices run hotter than on the console */
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
/* The ARM's idle throttle. Some sound drivers never idle in a way arm7.c's loop check can see: Re-Volt's
 * main loop keeps scanning its request tables and running delay loops, with a pass counter in RAM that
 * changes every time, so the ARM ran flat out (most of the sound chip's time). After ARM_QUIET samples in
 * which nothing reached it (no write to the chip or its RAM from the SH-4, no interrupt for it) and it
 * wrote no chip register, it gets 1/ARM_IDLE_DIV of its cycles until something does. Its replies to the
 * SH-4 still come at once (an SH-4 write wakes it); its own delay loops in that time run slower. In a
 * Re-Volt race it was throttled 75-95% of the time and the run took 14% less CPU. On when
 * dc_aica_arm_throttle is set (the frontend does on the Switch); DC_ARM_THROTTLE=1 / 0 overrides. */
int dc_aica_arm_throttle;
#define ARM_QUIET    64
#define ARM_IDLE_DIV 8
static void arm_slice(void) {
    static int off = -1;
    static unsigned last, quiet;
    if (off < 0) { const char *e = getenv("DC_ARM_THROTTLE"); off = e ? *e == '0' : !dc_aica_arm_throttle; }
    unsigned act = arm_wakes + arm_regwrites + dc_aica_ext_events;
    if (off || act != last) { last = act; quiet = 0; }
    else if (quiet < ARM_QUIET + 1) quiet++;
    arm7_run(quiet > ARM_QUIET ? ARM_CYCLES_PER_SAMPLE / ARM_IDLE_DIV : ARM_CYCLES_PER_SAMPLE);
}
static void sample_tick(void) {
    if (arm_running) arm_slice();
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

uint64_t (*dc_prof_clock)(void);
uint64_t dc_prof_aica;
void dc_aica_advance(uint64_t now) {
    const uint64_t per = CPU_HZ / SAMPLE_HZ;
    if (now < sample_clock) return;
    uint64_t t0 = dc_prof_clock ? dc_prof_clock() : 0;
    uint64_t n = (now - sample_clock) / per + 1;
    if (n > 4410) { sample_clock = now - 4410 * per; n = 4410; }   /* never fall more than 0.1 s behind */
    while (n--) { sample_tick(); sample_clock += per; }
    if (dc_prof_clock) dc_prof_aica += dc_prof_clock() - t0;
}
void dc_aica_run_samples(uint32_t n) { while (n--) sample_tick(); }
uint64_t dc_aica_clock(void) { return sample_clock; }
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
    if (w >= 0x3400 && w < 0x3C00) dsp.last_step = -1;      /* DSP program changed */
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
extern unsigned arm7_writes;          /* memory writes by the ARM (idle-loop detection, arm7.c) */
/* arm7_writes counts register writes and RAM writes that change something */
void arm7_write32(uint32_t a, uint32_t v) {
    if (is_reg(a)) { arm7_writes++; arm_regwrites++; dc_aica_write(a, v, 4); return; }
    uint8_t *p = dc_aram + (a & 0x1FFFFC); if (memcmp(p, &v, 4)) { arm7_writes++; memcpy(p, &v, 4); }
}
void arm7_write16(uint32_t a, uint16_t v) {
    if (is_reg(a)) { arm7_writes++; arm_regwrites++; dc_aica_write(a, v, 2); return; }
    uint8_t *p = dc_aram + (a & 0x1FFFFE); if (memcmp(p, &v, 2)) { arm7_writes++; memcpy(p, &v, 2); }
}
void arm7_write8(uint32_t a, uint8_t v) {
    if (is_reg(a)) { arm7_writes++; arm_regwrites++; dc_aica_write(a, v, 1); return; }
    if (dc_aram[a & 0x1FFFFF] != v) { arm7_writes++; dc_aram[a & 0x1FFFFF] = v; }
}

int dc_aica_arm_running(void) { return arm_running; }

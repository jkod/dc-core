/* dc-core: Dreamcast hardware model for recompiled code.
 *
 * Holly (PowerVR2 + system bus) registers, video timing (SPG) with vblank interrupts, the
 * SH-4 on-chip peripherals games touch (INTC, TMU, store queues, cache control), VRAM and
 * sound RAM. Interrupts are delivered by calling the game's own handler at VBR+0x600 from
 * dc_events(), which runs whenever the cycle counter passes the next scheduled event.
 *
 * Not yet: rendering (TA/ISP/TSP), AICA/ARM sound, Maple, G1/GD DMA. Writes to those land in
 * register storage and can be observed through dc_hw_hooks.
 */
#include "dccore/runtime.h"
#include "dccore/hw.h"
#include <stdio.h>
#include <string.h>

#define CPU_HZ 200000000ull
#define PERIPH_HZ (CPU_HZ / 4)      /* Pφ = 50 MHz feeds the TMU */

uint64_t dc_next_event = ~0ull;
dc_hw_hooks dc_hw;                   /* optional observers (renderer, sound, input) */

static dc_hw_ops user_ops;
void dc_set_hw(const dc_hw_ops *ops) { if (ops) user_ops = *ops; else memset(&user_ops, 0, sizeof user_ops); }

/* ---------------------------------------------------------------- storage */
static uint32_t sb[0x800];           /* 0x005F6800..0x005F7FFF, word index */
static uint32_t pvr[0x800];          /* 0x005F8000..0x005F9FFF */
static uint32_t aica[0x4000];        /* 0x00700000..0x0070FFFF */
static uint8_t  aram[0x200000];      /* 0x00800000..0x009FFFFF sound RAM */
uint8_t dc_vram[0x800000];           /* 8 MB, 64-bit layout */
static uint32_t p4[32][64];          /* SH-4 on-chip registers, by module */
static uint32_t sq[2][8];            /* store queues */
static uint8_t  ocram[0x2000];

#define SBR(o)  sb[((o) - 0x6800) >> 2]
#define PVR(o)  pvr[(o) >> 2]

/* Holly interrupt status/mask (offsets from 0x005F6800 block base 0x5F6800) */
#define ISTNRM  0x6900
#define ISTEXT  0x6904
#define ISTERR  0x6908
#define IML2NRM 0x6910
#define IML4NRM 0x6920
#define IML6NRM 0x6930

/* p4 module index for an 0xFFxxxxxx address */
static uint32_t *p4reg(uint32_t a) { return &p4[(a >> 19) & 31][(a & 0xFC) >> 2]; }
#define INTEVT (*p4reg(0xFF000028u))
#define QACR0  (*p4reg(0xFF000038u))
#define QACR1  (*p4reg(0xFF00003Cu))
#define IPRA   (*p4reg(0xFFD00004u))
#define TSTR   (*(uint8_t *)p4reg(0xFFD80004u))

/* ---------------------------------------------------------------- video timing */
static uint64_t field_cycles(void) {
    uint32_t lines = (PVR(0x0D8) >> 16 & 0x3FF) + 1;
    return CPU_HZ / (lines > 600 ? 50 : 60);
}
static uint32_t total_lines(void) { uint32_t l = (PVR(0x0D8) >> 16 & 0x3FF) + 1; return l > 1 ? l : 525; }
static uint64_t frame_count;
static uint32_t cur_line(uint64_t cyc) { uint64_t f = field_cycles(); return (uint32_t)((cyc % f) * total_lines() / f); }

static uint32_t spg_status(uint64_t cyc) {
    uint32_t line = cur_line(cyc);
    uint32_t vb = PVR(0x0DC), vbstart = vb & 0x3FF, vbend = (vb >> 16) & 0x3FF;
    int blank = vbstart > vbend ? (line >= vbstart || line < vbend) : (line >= vbstart && line < vbend);
    uint32_t field = (uint32_t)((cyc / field_cycles()) & 1);
    return line | (field << 10) | ((uint32_t)blank << 11) | ((uint32_t)(line < 3) << 13);
}

/* ---------------------------------------------------------------- TMU */
static struct { uint64_t start; uint32_t cnt0; int running; } tmu[3];
static uint32_t *tmu_reg(int ch, int r) { return p4reg(0xFFD80008u + ch * 12 + r * 4); }  /* r: 0 TCOR 1 TCNT 2 TCR */
static uint64_t tmu_div(int ch) { static const int d[8] = {4, 16, 64, 256, 1024, 1024, 1024, 1024}; return (uint64_t)d[*tmu_reg(ch, 2) & 7] * (CPU_HZ / PERIPH_HZ); }

static void tmu_sync(int ch, uint64_t now) {
    if (!tmu[ch].running) return;
    uint64_t ticks = (now - tmu[ch].start) / tmu_div(ch);
    uint32_t cor = *tmu_reg(ch, 0), cnt = tmu[ch].cnt0;
    if (ticks > cnt) {                    /* one or more underflows */
        uint64_t rest = ticks - cnt - 1, per = (uint64_t)cor + 1;
        cnt = cor - (uint32_t)(per ? rest % per : 0);
        *tmu_reg(ch, 2) |= 0x100;         /* UNF */
    } else cnt -= (uint32_t)ticks;
    *tmu_reg(ch, 1) = cnt;
    tmu[ch].cnt0 = cnt; tmu[ch].start += ticks * tmu_div(ch);
}
static uint64_t tmu_next_underflow(int ch, uint64_t now) {
    if (!tmu[ch].running || !(*tmu_reg(ch, 2) & 0x20)) return ~0ull;     /* UNIE off */
    return tmu[ch].start + ((uint64_t)tmu[ch].cnt0 + 1) * tmu_div(ch);
}

/* ---------------------------------------------------------------- interrupts */
static int irq_depth;
static void take_irq(sh4ctx *c, uint32_t code) {
    uint32_t sr = sh4_get_sr(c);
    c->ssr = sr; c->sgr = c->r[15];
    INTEVT = code;
    sh4_set_sr(c, sr | SR_MD | SR_RB | SR_BL);
    irq_depth++;
    if (dc_hw.on_irq) dc_hw.on_irq(code);
    uint32_t saved_pr = c->pr;
    dc_call(c, c->vbr + 0x600);
    c->pr = saved_pr;
    irq_depth--;
    /* if the handler did not rte (unusual), restore SR ourselves */
    if (sh4_get_sr(c) & SR_BL) sh4_set_sr(c, c->ssr);
}

static int holly_level(void) {
    uint32_t n = SBR(ISTNRM), e = SBR(ISTEXT), r = SBR(ISTERR);
    if ((n & SBR(IML6NRM)) | (e & SBR(IML6NRM + 4)) | (r & SBR(IML6NRM + 8))) return 6;
    if ((n & SBR(IML4NRM)) | (e & SBR(IML4NRM + 4)) | (r & SBR(IML4NRM + 8))) return 4;
    if ((n & SBR(IML2NRM)) | (e & SBR(IML2NRM + 4)) | (r & SBR(IML2NRM + 8))) return 2;
    return 0;
}

void dc_raise(uint32_t istnrm_bits) { SBR(ISTNRM) |= istnrm_bits; dc_next_event = 0; }

static void check_irqs(sh4ctx *c) {
    if (irq_depth) return;
    uint32_t sr = sh4_get_sr(c);
    if (sr & SR_BL) return;
    int imask = (sr >> 4) & 15;
    /* TMU underflow interrupts (priority from IPRA) */
    for (int ch = 0; ch < 3; ch++) {
        uint32_t tcr = *tmu_reg(ch, 2);
        int pri = (IPRA >> (12 - ch * 4)) & 15;
        if ((tcr & 0x120) == 0x120 && pri > imask) { take_irq(c, 0x400 + ch * 0x20); return; }
    }
    int lvl = holly_level();
    if (lvl && lvl > imask) take_irq(c, 0x200 + (15 - lvl) * 0x20);
}

static uint64_t last_vbin_field = ~0ull, last_vbout_field = ~0ull;

void dc_events(sh4ctx *c) {
    uint64_t now = c->cycles, f = field_cycles(), field = now / f;
    uint32_t lines = total_lines();
    uint32_t vbin = PVR(0x0CC) & 0x3FF, vbout = (PVR(0x0CC) >> 16) & 0x3FF;
    if (vbin >= lines) vbin = lines - 1;
    if (vbout >= lines) vbout = 0;
    uint64_t at_in = field * f + (uint64_t)vbin * f / lines, at_out = field * f + (uint64_t)vbout * f / lines;
    if (now >= at_in && last_vbin_field != field) {
        last_vbin_field = field; SBR(ISTNRM) |= 1u << 3; frame_count++;
        if (dc_hw.on_vblank) dc_hw.on_vblank(frame_count);
    }
    if (now >= at_out && last_vbout_field != field) { last_vbout_field = field; SBR(ISTNRM) |= 1u << 4; }
    for (int ch = 0; ch < 3; ch++) tmu_sync(ch, now);
    check_irqs(c);
    /* next event: the next vblank in/out edge or TMU underflow */
    uint64_t nx = (last_vbin_field == field) ? (field + 1) * f + (uint64_t)vbin * f / lines : at_in;
    uint64_t no = (last_vbout_field == field) ? (field + 1) * f + (uint64_t)vbout * f / lines : at_out;
    if (no < nx) nx = no;
    for (int ch = 0; ch < 3; ch++) { uint64_t t = tmu_next_underflow(ch, now); if (t < nx) nx = t; }
    /* while an interrupt is still pending (masked), look again soon */
    if (holly_level() || ((*tmu_reg(0, 2) | *tmu_reg(1, 2) | *tmu_reg(2, 2)) & 0x100)) { uint64_t t = now + 2000; if (t < nx) nx = t; }
    dc_next_event = nx;
}

/* ---------------------------------------------------------------- VRAM 32-bit view */
static uint32_t vram32(uint32_t o) {
    o &= 0x7FFFFC;
    uint32_t bank = (o >> 22) & 1;                       /* the two 4 MB banks interleave per 32-bit word */
    uint32_t rv = (o & 3) | ((o & 0x3FFFFC) << 1) | (bank << 2);
    return rv & 0x7FFFFF;
}

/* ---------------------------------------------------------------- store queues */
static void sq_flush(uint32_t a) {
    int n = (a >> 5) & 1;
    uint32_t qacr = n ? QACR1 : QACR0;
    uint32_t dst = ((qacr & 0x1C) << 24) | (a & 0x03FFFFE0);
    if (dc_hw.on_sq && dc_hw.on_sq(dst, sq[n])) return;          /* TA FIFO etc. */
    for (int i = 0; i < 8; i++) dc_wr32(dst + i * 4, sq[n][i]);
}
void dc_hw_pref(uint32_t a) { if ((a >> 26) == 0x38) sq_flush(a); }   /* 0xE0000000..0xE3FFFFFF */

/* ---------------------------------------------------------------- access */
int dc_hw_verbose = 1;
extern sh4ctx *dc_current_ctx;
static int warned_n; static uint32_t warned[256];
static void warn(const char *k, uint32_t a) {
    uint32_t page = a & 0xFFFFFFF0u;
    for (int i = 0; i < warned_n; i++) if (warned[i] == page) return;
    if (warned_n < 256) warned[warned_n++] = page;
    if (dc_hw_verbose) fprintf(stderr, "dc-core: unhandled %s %08X\n", k, a);
}
static uint32_t rd(uint32_t a, int size) {
    if ((a >> 26) == 0x38) { uint32_t v = sq[(a >> 5) & 1][(a >> 2) & 7]; return v; }
    if ((a & 0xFC000000u) == 0x7C000000u) { uint32_t v = 0; memcpy(&v, ocram + (a & 0x1FFF), size); return v; }
    if (a >= 0xF0000000u && a < 0xF8000000u) return 0;                 /* cache arrays */
    if (a >= 0xFF000000u) {
        uint64_t now = dc_current_ctx ? dc_current_ctx->cycles : 0;
        if ((a & 0xFFFFFF00u) == 0xFFD80000u) for (int ch = 0; ch < 3; ch++) tmu_sync(ch, now);
        uint32_t v = *p4reg(a);
        return size == 4 ? v : size == 2 ? (v >> ((a & 2) * 8)) & 0xFFFF : (v >> ((a & 3) * 8)) & 0xFF;
    }
    uint32_t m = a & 0x1FFFFFFFu;
    if (m >= 0x005F6800u && m < 0x005F8000u) {
        uint32_t v = SBR(m & 0xFFFC);
        return size == 4 ? v : size == 2 ? (v >> ((m & 2) * 8)) & 0xFFFF : (v >> ((m & 3) * 8)) & 0xFF;
    }
    if (m >= 0x005F8000u && m < 0x005FA000u) {
        uint32_t o = m - 0x005F8000u, v;
        if (o == 0x000) v = 0x17FD11DBu;                                  /* ID */
        else if (o == 0x004) v = 0x11;                                    /* REVISION */
        else if (o == 0x10C) v = spg_status(dc_current_ctx ? dc_current_ctx->cycles : 0);
        else v = PVR(o & 0x1FFC);
        return v;
    }
    if (m >= 0x00700000u && m < 0x00710000u) { uint32_t v = aica[(m & 0xFFFC) >> 2]; return size == 4 ? v : size == 2 ? (v >> ((m & 2) * 8)) & 0xFFFF : (v >> ((m & 3) * 8)) & 0xFF; }
    if (m >= 0x00800000u && m < 0x00A00000u) { uint32_t v = 0; memcpy(&v, aram + (m & 0x1FFFFF), size); return v; }
    if ((m >> 24) == 0x04 || (m >> 24) == 0x06) { uint32_t v = 0; memcpy(&v, dc_vram + (m & 0x7FFFFF), size); return v; }
    if ((m >> 24) == 0x05 || (m >> 24) == 0x07) { uint32_t v = 0; memcpy(&v, dc_vram + vram32(m), size); return v; }
    warn(size == 4 ? "read32" : size == 2 ? "read16" : "read8", a);
    return 0;
}

static void wr(uint32_t a, uint32_t v, int size) {
    if ((a >> 26) == 0x38) { sq[(a >> 5) & 1][(a >> 2) & 7] = v; return; }
    if ((a & 0xFC000000u) == 0x7C000000u) { memcpy(ocram + (a & 0x1FFF), &v, size); return; }
    if (a >= 0xF0000000u && a < 0xF8000000u) return;
    if (a >= 0xFF000000u) {
        uint64_t now = dc_current_ctx ? dc_current_ctx->cycles : 0;
        uint32_t *r = p4reg(a);
        if ((a & 0xFFFFFF00u) == 0xFFD80000u) {
            for (int ch = 0; ch < 3; ch++) tmu_sync(ch, now);
            if (a == 0xFFD80004u) {                                          /* TSTR */
                for (int ch = 0; ch < 3; ch++) {
                    int on = (v >> ch) & 1;
                    if (on && !tmu[ch].running) { tmu[ch].start = now; tmu[ch].cnt0 = *tmu_reg(ch, 1); }
                    tmu[ch].running = on;
                }
            }
            if (size == 4) *r = v; else if (size == 2) { int sh = (a & 2) * 8; *r = (*r & ~(0xFFFFu << sh)) | ((v & 0xFFFF) << sh); }
            else { int sh = (a & 3) * 8; *r = (*r & ~(0xFFu << sh)) | ((v & 0xFF) << sh); }
            for (int ch = 0; ch < 3; ch++) if (a == 0xFFD8000Cu + ch * 12u) tmu[ch].cnt0 = *r, tmu[ch].start = now;
            dc_next_event = 0;
            return;
        }
        if (size == 4) *r = v; else if (size == 2) { int sh = (a & 2) * 8; *r = (*r & ~(0xFFFFu << sh)) | ((v & 0xFFFF) << sh); }
        else { int sh = (a & 3) * 8; *r = (*r & ~(0xFFu << sh)) | ((v & 0xFF) << sh); }
        return;
    }
    uint32_t m = a & 0x1FFFFFFFu;
    if (m >= 0x005F6800u && m < 0x005F8000u) {
        uint32_t o = m & 0xFFFC;
        if (o == ISTNRM || o == ISTERR) { SBR(o) &= ~v; dc_next_event = 0; return; }   /* write 1 to clear */
        if (o >= IML2NRM && o <= IML6NRM + 8) dc_next_event = 0;
        SBR(o) = v;
        if (dc_hw.on_sb_write) dc_hw.on_sb_write(o + 0x005F0000u, v);
        return;
    }
    if (m >= 0x005F8000u && m < 0x005FA000u) {
        uint32_t o = m - 0x005F8000u;
        PVR(o & 0x1FFC) = v;
        if (dc_hw.on_pvr_write) dc_hw.on_pvr_write(o, v);
        if (o == 0x0CC || o == 0x0D8 || o == 0x0DC) dc_next_event = 0;
        return;
    }
    if (m >= 0x00700000u && m < 0x00710000u) { aica[(m & 0xFFFC) >> 2] = v; if (dc_hw.on_aica_write) dc_hw.on_aica_write(m, v); return; }
    if (m >= 0x00800000u && m < 0x00A00000u) { memcpy(aram + (m & 0x1FFFFF), &v, size); return; }
    if ((m >> 24) == 0x04 || (m >> 24) == 0x06) { memcpy(dc_vram + (m & 0x7FFFFF), &v, size); return; }
    if ((m >> 24) == 0x05 || (m >> 24) == 0x07) { memcpy(dc_vram + vram32(m), &v, size); return; }
    if ((m >> 26) == 0x04) { if (dc_hw.on_ta_write) dc_hw.on_ta_write(m, v); return; }      /* 0x10000000.. TA FIFO */
    warn(size == 4 ? "write32" : size == 2 ? "write16" : "write8", a);
}

uint8_t  dc_hw_rd8 (uint32_t a) { return user_ops.rd8 ? user_ops.rd8(a) : (uint8_t)rd(a, 1); }
uint16_t dc_hw_rd16(uint32_t a) { return user_ops.rd16 ? user_ops.rd16(a) : (uint16_t)rd(a, 2); }
uint32_t dc_hw_rd32(uint32_t a) { return user_ops.rd32 ? user_ops.rd32(a) : rd(a, 4); }
void dc_hw_wr8 (uint32_t a, uint8_t v)  { if (user_ops.wr8) user_ops.wr8(a, v); else wr(a, v, 1); }
void dc_hw_wr16(uint32_t a, uint16_t v) { if (user_ops.wr16) user_ops.wr16(a, v); else wr(a, v, 2); }
void dc_hw_wr32(uint32_t a, uint32_t v) { if (user_ops.wr32) user_ops.wr32(a, v); else wr(a, v, 4); }

uint64_t dc_frame_count(void) { return frame_count; }
uint32_t dc_pvr_reg(uint32_t ofs) { return PVR(ofs & 0x1FFC); }
uint32_t dc_sb_reg(uint32_t addr) { return SBR(addr & 0xFFFC); }

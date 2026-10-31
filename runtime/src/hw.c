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
#include "dccore/ta.h"
#include "dccore/bios.h"
#include "dccore/aica.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define CPU_HZ 200000000ull
#define PERIPH_HZ (CPU_HZ / 4)      /* Pφ = 50 MHz feeds the TMU */

uint64_t dc_next_event = ~0ull;
dc_hw_hooks dc_hw;                   /* optional observers (renderer, sound, input) */
extern sh4ctx *dc_current_ctx;

static dc_hw_ops user_ops;
void dc_set_hw(const dc_hw_ops *ops) { if (ops) user_ops = *ops; else memset(&user_ops, 0, sizeof user_ops); }

/* ---------------------------------------------------------------- storage */
static uint32_t sb[0x800];           /* 0x005F6800..0x005F7FFF, word index */
static uint32_t pvr[0x800];          /* 0x005F8000..0x005F9FFF */
#define aram dc_aram                 /* 0x00800000..0x009FFFFF sound RAM (aica.c) */
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
static void dmac_check(void);
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

/* ---------------------------------------------------------------- delayed completions */
#define MAXQ 32
static struct { uint64_t at; uint32_t bits; } evq[MAXQ];
static int evn;
static uint64_t now_cycles(void) { return dc_current_ctx ? dc_current_ctx->cycles : 0; }
/* raise ISTNRM bits after `delay` cycles (hardware never completes a transfer instantly, and
 * games set their "busy" flags after starting one) */
static void schedule_irq(uint32_t bits, uint64_t delay) {
    if (evn < MAXQ) { evq[evn].at = now_cycles() + delay; evq[evn].bits = bits; evn++; }
    else SBR(ISTNRM) |= bits;
    dc_next_event = 0;
}

/* ---------------------------------------------------------------- port A (PDTRA) */
/* Bits 8-9 report the AV cable: 0 VGA, 2 RGB (SCART), 3 composite/S-video. Games pick their video
 * mode from it (a PAL game runs 50 Hz on a TV cable, 480p/60 Hz on VGA). The low bits answer the
 * boot-time handshake like the hardware does (as Flycast models it). */
int dc_cable = 0;
static uint32_t port_a(void) {
    uint32_t pctra = *p4reg(0xFF80002Cu) & 0xF, pdtra = *p4reg(0xFF800030u) & 0xF, v;
    v = (pctra == 0x8 || pctra == 0xB) ? 3 : 0;
    if (pctra == 0xB && pdtra == 2) v = 0;
    else if (pctra == 0xC && pdtra == 2) v = 3;
    return v | ((uint32_t)(dc_cable & 3) << 8);
}

/* ---------------------------------------------------------------- interrupts */
static int irq_depth;
#define DC_IRQ_RET 0xFFFFFFF0u
static void take_irq(sh4ctx *c, uint32_t code) {
    uint32_t sr = sh4_get_sr(c);
    c->ssr = sr; c->sgr = c->r[15];
    INTEVT = code;
    sh4_set_sr(c, sr | SR_MD | SR_RB | SR_BL);
    irq_depth++;
    if (dc_hw.on_irq) dc_hw.on_irq(code);
    uint32_t saved_pr = c->pr;
    /* spc = "back to the interrupted code". A handler that wants the interrupt to return
     * elsewhere (an OS doing a preemptive task switch patches its saved frame) leaves another
     * address in spc at rte: that guest code runs first; it gets back here when whatever it
     * resumes returns to the marker. */
    c->spc = DC_IRQ_RET;
    dc_call(c, c->vbr + 0x600);
    c->pr = saved_pr;
    irq_depth--;
    /* if the handler did not rte (unusual), restore SR ourselves */
    if (sh4_get_sr(c) & SR_BL) sh4_set_sr(c, c->ssr);
    /* the interrupt is over; redirected code is ordinary code of the interrupted task (it may
     * switch to another task, which must see interrupts enabled) */
    for (int n = 0; c->spc != DC_IRQ_RET && n < 4; n++) {
        uint32_t to = c->spc;
        c->spc = DC_IRQ_RET;
        dc_call(c, to);
    }
}

static int holly_level(void) {
    uint32_t n = SBR(ISTNRM), e = SBR(ISTEXT), r = SBR(ISTERR);
    if ((n & SBR(IML6NRM)) | (e & SBR(IML6NRM + 4)) | (r & SBR(IML6NRM + 8))) return 6;
    if ((n & SBR(IML4NRM)) | (e & SBR(IML4NRM + 4)) | (r & SBR(IML4NRM + 8))) return 4;
    if ((n & SBR(IML2NRM)) | (e & SBR(IML2NRM + 4)) | (r & SBR(IML2NRM + 8))) return 2;
    return 0;
}

void dc_raise(uint32_t istnrm_bits) { SBR(ISTNRM) |= istnrm_bits; dc_next_event = 0; }
void dc_set_ext_irq(uint32_t bit, int on) {                    /* ISTEXT is level-driven by the devices */
    uint32_t o = SBR(ISTEXT);
    SBR(ISTEXT) = on ? (o | bit) : (o & ~bit);
    if (SBR(ISTEXT) != o) dc_next_event = 0;
}

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
    int dpri = (*p4reg(0xFFD0000Cu) >> 8) & 15;                 /* IPRC: DMAC */
    for (int ch = 0; ch < 4; ch++) {
        uint32_t chcr = *p4reg(0xFFA0000Cu + ch * 0x10);
        if ((chcr & 6) == 6 && ch != 2 && dpri > imask) { take_irq(c, 0x640 + ch * 0x20); return; }
    }
    int lvl = holly_level();
    if (lvl && lvl > imask) take_irq(c, 0x200 + (15 - lvl) * 0x20);
}

static uint64_t last_vbin_field = ~0ull, last_vbout_field = ~0ull;

/* SPG_HBLANK_INT (0x5F80C8): bits 0-9 line_comp_val, 12-13 mode: 0 = once per field at that line,
 * 1 = every line_comp_val lines, 2 = every line. Raises ISTNRM bit 5; returns when the next one is
 * due. Lines are counted from the start of the field. */
static uint64_t hb_last_line = ~0ull;      /* absolute line number (field * lines + line) last raised */
static int hblank_line_matches(uint32_t line) {
    uint32_t r = PVR(0x0C8), comp = r & 0x3FF, mode = (r >> 12) & 3;
    if (mode == 0) return line == comp;
    if (mode == 1) return comp ? line % comp == 0 : 0;
    return mode == 2;
}
static uint64_t hblank_events(uint64_t now, uint64_t f, uint32_t lines) {
    uint32_t r = PVR(0x0C8), mode = (r >> 12) & 3;
    if (mode == 3) return ~0ull;
    uint64_t abs_now = now / f * lines + (now % f) * lines / f;          /* current absolute line */
    if (hb_last_line == ~0ull || abs_now - hb_last_line > lines) hb_last_line = abs_now ? abs_now - 1 : 0;
    for (uint64_t l = hb_last_line + 1; l <= abs_now; l++)
        if (hblank_line_matches((uint32_t)(l % lines))) { SBR(ISTNRM) |= 1u << 5; }
    hb_last_line = abs_now;
    for (uint64_t l = abs_now + 1; l <= abs_now + lines; l++)            /* next matching line start */
        if (hblank_line_matches((uint32_t)(l % lines))) return (l / lines) * f + (l % lines) * f / lines;
    return ~0ull;
}

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
    uint64_t hb_next = hblank_events(now, f, lines);
    for (int ch = 0; ch < 3; ch++) tmu_sync(ch, now);
    for (int i = 0; i < evn; )
        if (evq[i].at <= now) { SBR(ISTNRM) |= evq[i].bits; evq[i] = evq[--evn]; } else i++;
    dc_aica_advance(now);
    check_irqs(c);
    /* next event: the next vblank in/out edge or TMU underflow */
    uint64_t nx = (last_vbin_field == field) ? (field + 1) * f + (uint64_t)vbin * f / lines : at_in;
    uint64_t no = (last_vbout_field == field) ? (field + 1) * f + (uint64_t)vbout * f / lines : at_out;
    if (no < nx) nx = no;
    if (hb_next < nx) nx = hb_next;
    for (int ch = 0; ch < 3; ch++) { uint64_t t = tmu_next_underflow(ch, now); if (t < nx) nx = t; }
    for (int i = 0; i < evn; i++) if (evq[i].at < nx) nx = evq[i].at;
    if (dc_aica_arm_running() || (SBR(ISTEXT) & 2)) { uint64_t t = dc_aica_next(); if (t < nx) nx = t; }
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

/* ---------------------------------------------------------------- YUV converter
 * Input: macroblocks of YUV420 (384 bytes: U 8x8, V 8x8, Y 4x 8x8) or YUV422 (512 bytes), written
 * to 0x10800000..; output: UYVY 4:2:2 texture at TA_YUV_TEX_BASE, (U+1)*16 x (V+1)*16 pixels.
 * When the configured number of macroblocks has arrived, ISTNRM bit 6 is raised. */
static struct { uint8_t buf[512]; uint32_t fill, mb; } yuv;
static void yuv_macroblock(void) {
    uint32_t ctrl = PVR(0x14C), w_mb = (ctrl & 0x3F) + 1, h_mb = ((ctrl >> 8) & 0x3F) + 1;
    int is422 = (ctrl >> 24) & 1;
    uint32_t mx = yuv.mb % w_mb, my = yuv.mb / w_mb, pitch = w_mb * 16 * 2;
    uint32_t base = PVR(0x148) & 0x7FFFF8;
    const uint8_t *b = yuv.buf;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x += 2) {
            uint8_t U, V, Y0, Y1;
            if (!is422) {
                U = b[(y / 2) * 8 + x / 2]; V = b[64 + (y / 2) * 8 + x / 2];
                int blk = (y / 8) * 2 + (x / 8);                     /* Y blocks: TL, TR, BL, BR */
                const uint8_t *yb = b + 128 + blk * 64;
                Y0 = yb[(y % 8) * 8 + (x % 8)]; Y1 = yb[(y % 8) * 8 + (x % 8) + 1];
            } else {
                U = b[y * 8 + x / 2]; V = b[128 + y * 8 + x / 2];
                int blk = x / 8; const uint8_t *yb = b + 256 + blk * 128;
                Y0 = yb[y * 8 + (x % 8)]; Y1 = yb[y * 8 + (x % 8) + 1];
            }
            uint32_t o = base + (my * 16 + y) * pitch + (mx * 16 + x) * 2;
            uint8_t px[4] = {U, Y0, V, Y1};
            for (int k = 0; k < 4; k++) dc_vram[vram32((o + k) & 0x7FFFFF) | ((o + k) & 3)] = px[k];
        }
    if (++yuv.mb >= w_mb * h_mb) { yuv.mb = 0; dc_raise(1u << 6); }
    PVR(0x150) = yuv.mb;                                                 /* TA_YUV_TEX_CNT */
}
static void yuv_write(const uint8_t *p, uint32_t len) {
    uint32_t need = ((PVR(0x14C) >> 24) & 1) ? 512 : 384;
    while (len) {
        uint32_t n = need - yuv.fill; if (n > len) n = len;
        memcpy(yuv.buf + yuv.fill, p, n); yuv.fill += n; p += n; len -= n;
        if (yuv.fill == need) { yuv.fill = 0; yuv_macroblock(); }
    }
}

/* ---------------------------------------------------------------- TA FIFO / texture path */
/* m = address & 0x1FFFFFFF in 0x10000000..0x13FFFFFF */
static void ta_area_write(uint32_t m, const uint8_t *p, uint32_t len) {
    uint32_t sub = m & 0x03FFFFFFu;
    if (sub < 0x00800000u || (sub >= 0x02000000u && sub < 0x02800000u)) { dc_ta_write(p, len); return; }
    if (sub < 0x01000000u || (sub >= 0x02800000u && sub < 0x03000000u)) { yuv_write(p, len); return; }
    int lm32 = (sub >= 0x03000000u) ? (SBR(0x6888) & 1) : (SBR(0x6884) & 1);           /* LMMODE1 / LMMODE0 */
    for (uint32_t i = 0; i < len; i += 4) {
        uint32_t o = (sub + i) & 0x7FFFFF;
        if (lm32) memcpy(dc_vram + vram32(o), p + i, 4); else memcpy(dc_vram + o, p + i, 4);
    }
}

/* ---------------------------------------------------------------- store queues */
static void sq_flush(uint32_t a) {
    int n = (a >> 5) & 1;
    uint32_t qacr = n ? QACR1 : QACR0;
    uint32_t dst = ((qacr & 0x1C) << 24) | (a & 0x03FFFFE0);
    if (dc_hw.on_sq && dc_hw.on_sq(dst, sq[n])) return;
    uint32_t m = dst & 0x1FFFFFFFu;
    if (m >= 0x10000000u && m < 0x14000000u) { ta_area_write(m, (const uint8_t *)sq[n], 32); return; }
    for (int i = 0; i < 8; i++) dc_wr32(dst + i * 4, sq[n][i]);
}
void dc_hw_pref(uint32_t a) { if ((a >> 26) == 0x38) sq_flush(a); }   /* 0xE0000000..0xE3FFFFFF */

/* ---------------------------------------------------------------- access */
int dc_hw_verbose = 1;
extern sh4ctx *dc_current_ctx;
static int warned_n; static uint32_t warned[256];
void (*dc_unhandled_hook)(const char *kind, uint32_t addr);
static void warn(const char *k, uint32_t a) {
    if (dc_unhandled_hook) dc_unhandled_hook(k, a);
    uint32_t page = a & 0xFFFFFFF0u;
    for (int i = 0; i < warned_n; i++) if (warned[i] == page) return;
    if (warned_n < 256) warned[warned_n++] = page;
    if (dc_hw_verbose) fprintf(stderr, "dc-core: unhandled %s %08X\n", k, a);
}
/* ---------------------------------------------------------------- SH-4 DMAC (auto-request) */
int dc_dmac_log = 1;
static void dmac_check(void) {
    uint32_t dmaor = *p4reg(0xFFA00040u);
    if (!(dmaor & 1) || (dmaor & 6)) return;                    /* DME off, or NMI/address error */
    for (int ch = 0; ch < 4; ch++) {
        uint32_t base = 0xFFA00000u + ch * 0x10;
        uint32_t chcr = *p4reg(base + 0x0C);
        if (!(chcr & 1) || (chcr & 2)) continue;                 /* DE off or TE already set */
        if (((chcr >> 8) & 15) != 4) continue;                   /* only auto-request; CH2 is driven by SB_C2DST */
        static const int tsz[8] = {8, 1, 2, 4, 32, 4, 4, 4};
        int ts = tsz[(chcr >> 4) & 7], sm = (chcr >> 12) & 3, dm = (chcr >> 14) & 3;
        uint32_t sar = *p4reg(base), dar = *p4reg(base + 4), n = *p4reg(base + 8) & 0xFFFFFF;
        if (!n) n = 0x1000000;
        if (dc_dmac_log) { fprintf(stderr, "dc-core: DMAC ch%d %08X -> %08X, %u x %d bytes\n", ch, sar, dar, n, ts); }
        for (uint32_t i = 0; i < n; i++) {
            for (int b = 0; b < ts; b += 4 > ts ? ts : 4) {
                if (ts == 1) dc_wr8(dar + b, dc_rd8(sar + b));
                else if (ts == 2) dc_wr16(dar + b, dc_rd16(sar + b));
                else dc_wr32(dar + b, dc_rd32(sar + b));
            }
            if (sm == 1) sar += ts; else if (sm == 2) sar -= ts;
            if (dm == 1) dar += ts; else if (dm == 2) dar -= ts;
        }
        *p4reg(base) = sar; *p4reg(base + 4) = dar; *p4reg(base + 8) = 0;
        *p4reg(base + 0x0C) = chcr | 2;                          /* TE */
        if (chcr & 4) dc_next_event = 0;                         /* IE: DMTE interrupt, see check_irqs */
    }
}

/* ---------------------------------------------------------------- DMA engines */
static void copy_words(uint32_t dst, uint32_t src, uint32_t len) {
    for (uint32_t i = 0; i < len; i += 4) dc_wr32(dst + i, dc_rd32(src + i));
}

static void maple_dma(void) {
    uint32_t p = (SBR(0x6C04) & 0x1FFFFFE0u) | 0x80000000u;
    for (int guard = 0; guard < 64; guard++) {
        uint32_t hdr = dc_rd32(p), recv = dc_rd32(p + 4);
        int words = hdr & 0xFF, port = (hdr >> 16) & 3, pattern = (hdr >> 8) & 7;
        if (pattern == 0) {
            uint32_t frame = dc_rd32(p + 8);
            int handled = dc_hw.on_maple && dc_hw.on_maple(port, frame, p + 12, words, recv | 0x80000000u);
            if (!handled) dc_wr32(recv | 0x80000000u, 0xFFFFFFFFu);        /* no device on this port */
            p += 12 + (uint32_t)words * 4;
        } else p += 8;
        if (hdr >> 31) break;
    }
    SBR(0x6C18) = 0;
    schedule_irq(1u << 12, 20000);                                       /* maple DMA end */
}

static void sb_dma(uint32_t o, uint32_t v) {
    if (o == 0x6808 && (v & 1)) {                                        /* CH2 DMA -> TA / texture memory */
        uint32_t src = *p4reg(0xFFA00020u), len = SBR(0x6804), dst = SBR(0x6800);
        uint32_t so = DC_RAM_OFS(src);
        if (DC_IS_RAM(src) && so + len <= 0x1000000u) {
            uint32_t m = dst & 0x1FFFFFFFu;
            if (m >= 0x10000000u && m < 0x14000000u) ta_area_write(m, dc_ram + so, len);
            else copy_words(dst, src, len);
        }
        SBR(0x6800) = dst + len; SBR(0x6804) = 0; SBR(0x6808) = 0;
        *p4reg(0xFFA00028u) = 0; *p4reg(0xFFA0002Cu) |= 2;               /* DMATCR2 = 0, CHCR2.TE */
        schedule_irq(1u << 19, 2000 + len / 4);
        return;
    }
    if (o >= 0x7800 && o < 0x7880 && (o & 0x1F) == 0x18 && (v & 1)) {   /* G2 DMA (AICA, ext1, ext2, dev) */
        uint32_t b = o - 0x18, k = (b - 0x7800) / 0x20;
        uint32_t g2 = SBR(b), sh = SBR(b + 4), len = SBR(b + 8) & 0x7FFFFFFFu, dir = SBR(b + 0xC);
        if (dir) copy_words(sh, g2, len); else copy_words(g2, sh, len);
        SBR(b + 0x18) = 0;
        schedule_irq(1u << (15 + k), 2000 + len / 2);
        return;
    }
    if (o == 0x7C18 && (v & 1)) {                                        /* PVR DMA */
        uint32_t pv = SBR(0x7C00), sh = SBR(0x7C04), len = SBR(0x7C08), dir = SBR(0x7C0C);
        if (dir) copy_words(sh, pv, len); else copy_words(pv, sh, len);
        SBR(0x7C18) = 0;
        schedule_irq(1u << 11, 2000 + len / 4);
        return;
    }
    if (o == 0x6C18 && (v & 1) && (SBR(0x6C14) & 1)) maple_dma();        /* Maple DMA */
}

void (*dc_hw_read_hook)(uint32_t addr, int size);
static uint32_t rd(uint32_t a, int size) {
    if (dc_hw_read_hook) dc_hw_read_hook(a, size);
    if ((a >> 26) == 0x38) { uint32_t v = sq[(a >> 5) & 1][(a >> 2) & 7]; return v; }
    if ((a & 0xFC000000u) == 0x7C000000u) { uint32_t v = 0; memcpy(&v, ocram + (a & 0x1FFF), size); return v; }
    if (a >= 0xF0000000u && a < 0xF8000000u) return 0;                 /* cache arrays */
    if (a >= 0xFF000000u) {
        uint64_t now = dc_current_ctx ? dc_current_ctx->cycles : 0;
        if ((a & 0xFFFFFF00u) == 0xFFD80000u) for (int ch = 0; ch < 3; ch++) tmu_sync(ch, now);
        uint32_t v = *p4reg(a);
        if ((a & 0xFFFFFFFCu) == 0xFF800030u) v = port_a();
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
    if (m >= 0x00710000u && m < 0x00710010u) {                           /* AICA RTC: seconds since 1950 */
        uint32_t t = (uint32_t)time(NULL) + 631152000u;                  /* 1950-01-01 -> 1970-01-01 */
        return (m & 4) ? (t & 0xFFFF) : (t >> 16);
    }
    if (m >= 0x00700000u && m < 0x00710000u) { dc_aica_advance(now_cycles()); return dc_aica_read(m, size); }
    if (m >= 0x00800000u && m < 0x00A00000u) { dc_aica_advance(now_cycles()); uint32_t v = 0; memcpy(&v, aram + (m & 0x1FFFFF), size); return v; }
    if ((m >> 24) == 0x04 || (m >> 24) == 0x06) { uint32_t v = 0; memcpy(&v, dc_vram + (m & 0x7FFFFF), size); return v; }
    if ((m >> 24) == 0x05 || (m >> 24) == 0x07) { uint32_t v = 0; memcpy(&v, dc_vram + vram32(m), size); return v; }
    if (m < 0x00200000u && dc_bios_rom) { uint32_t v = 0; memcpy(&v, dc_bios_rom + m, size); return v; }   /* boot ROM */
    if (m >= 0x00200000u && m < 0x00220000u) { uint32_t v = 0; memcpy(&v, dc_flash + (m & 0x1FFFF), size); return v; }
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
        if ((a & 0xFFFFFF00u) == 0xFFA00000u) dmac_check();
        return;
    }
    uint32_t m = a & 0x1FFFFFFFu;
    if (m >= 0x005F6800u && m < 0x005F8000u) {
        uint32_t o = m & 0xFFFC;
        if (o == ISTNRM || o == ISTERR) { SBR(o) &= ~v; dc_next_event = 0; return; }   /* write 1 to clear */
        if (o >= IML2NRM && o <= IML6NRM + 8) dc_next_event = 0;
        SBR(o) = v;
        if (dc_hw.on_sb_write) dc_hw.on_sb_write(o + 0x005F0000u, v);
        sb_dma(o, v);
        return;
    }
    if (m >= 0x005F8000u && m < 0x005FA000u) {
        uint32_t o = m - 0x005F8000u;
        PVR(o & 0x1FFC) = v;
        if (dc_hw.on_pvr_write) dc_hw.on_pvr_write(o, v);
        if (o == 0x0CC || o == 0x0D8 || o == 0x0DC) dc_next_event = 0;
        if (o == 0x144 && (v >> 31)) dc_ta_reset();                      /* TA_LIST_INIT */
        if (o == 0x148) { yuv.mb = 0; yuv.fill = 0; PVR(0x150) = 0; }   /* TA_YUV_TEX_BASE: new frame */
        if (o == 0x014) {                                                 /* STARTRENDER */
            if (dc_hw.on_render) dc_hw.on_render();
            schedule_irq(7u, 200000);                                     /* render done (video, ISP, TSP) */
        }
        return;
    }
    if (m >= 0x00700000u && m < 0x00710000u) { dc_aica_advance(now_cycles()); dc_aica_write(m, v, size); if (dc_hw.on_aica_write) dc_hw.on_aica_write(m, v); dc_next_event = 0; return; }
    if (m >= 0x00800000u && m < 0x00A00000u) { memcpy(aram + (m & 0x1FFFFF), &v, size); if (dc_hw.on_aica_write) dc_hw.on_aica_write(m, v); return; }
    if ((m >> 24) == 0x04 || (m >> 24) == 0x06) { memcpy(dc_vram + (m & 0x7FFFFF), &v, size); return; }
    if ((m >> 24) == 0x05 || (m >> 24) == 0x07) { memcpy(dc_vram + vram32(m), &v, size); return; }
    if ((m >> 26) == 0x04) {                                                                  /* 0x10000000.. TA FIFO */
        static uint32_t blk[8]; static int nb;
        if (dc_hw.on_ta_write) dc_hw.on_ta_write(m, v);
        blk[nb++] = v;
        if (nb == 8) { nb = 0; ta_area_write(m & ~31u, (const uint8_t *)blk, 32); }
        return;
    }
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

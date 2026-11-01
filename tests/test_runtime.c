/* dc-core runtime tests (no game code): TA parser, DMA engines, maple, delayed interrupts.
 *   cc -Iruntime/include tests/test_runtime.c runtime/src/*.c -lm -o test_runtime && ./test_runtime */
#include "dccore/runtime.h"
#include "dccore/hw.h"
#include "dccore/ta.h"
#include "dccore/aica.h"
#include "dccore/maple.h"
#include "dccore/bios.h"
#include <stdio.h>
#include <string.h>

typedef struct { uint32_t addr, end; sh4fn fn; } dc_fn_entry;
const dc_fn_entry dc_fn_table[1] = {{0, 0, 0}};
const size_t dc_fn_count = 0;

static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); fails++; } } while (0)
static int nparams, nvtx, last_list;
static void sink(int kind, int list, const uint32_t *p, int words) { nparams++; if (kind == 7) nvtx++; last_list = list; }
static void run(sh4ctx *c, uint64_t cycles) { c->cycles += cycles; dc_events(c); }

static void blk(uint32_t pcw) { uint32_t b[8] = {pcw}; dc_ta_write32(b); }

int main(void) {
    sh4ctx c; dc_init(&c); dc_hw_verbose = 0;
    dc_ta_set_sink(sink);
    uint32_t IST = 0xA05F6900u;

    /* 1. opaque list: packed-colour polygon, 3 vertices, end of list -> ISTNRM bit 7 */
    dc_ta_reset();
    blk(0x80000000u | (0u << 24));                 /* polygon, list 0 */
    blk(0xE0000000u); blk(0xE0000000u); blk(0xF0000000u);
    CHECK(!(dc_rd32(IST) & (1u << 7)));
    blk(0x00000000u);
    CHECK(dc_rd32(IST) & (1u << 7));
    CHECK(nvtx == 3 && last_list == 0);
    dc_wr32(IST, 1u << 7); CHECK(!(dc_rd32(IST) & (1u << 7)));

    /* 2. translucent, textured float colour -> 64-byte vertices: an all-zero second half must not end the list */
    nvtx = 0;
    blk(0x80000000u | (2u << 24) | (1u << 3) | (1u << 4));
    blk(0xE0000000u); blk(0x00000000u);           /* vertex, second half looks like "end of list" */
    blk(0xF0000000u); blk(0x00000000u);
    CHECK(!(dc_rd32(IST) & (1u << 9)));
    blk(0x00000000u);                               /* real end of list */
    CHECK(dc_rd32(IST) & (1u << 9));
    CHECK(nvtx == 2);

    /* 3. CH2 DMA of a polygon list into the TA FIFO: list-end + DMA-end (delayed) interrupts */
    dc_wr32(IST, 0xFFFFFFFFu); dc_ta_reset(); nvtx = 0;
    uint32_t src = 0x8C200000u;
    uint32_t words[] = {0x80000000u | (4u << 24), 0xE0000000u, 0xF0000000u, 0};
    for (int i = 0; i < 4; i++) { memset(dc_ram + 0x200000 + i * 32, 0, 32); memcpy(dc_ram + 0x200000 + i * 32, &words[i], 4); }
    dc_wr32(0xFFA00020u, src);                       /* SAR2 */
    dc_wr32(0xA05F6800u, 0x10000000u); dc_wr32(0xA05F6804u, 128); dc_wr32(0xA05F6808u, 1);
    CHECK(nvtx == 2 && (dc_rd32(IST) & (1u << 21)));  /* punch-through list end */
    CHECK(!(dc_rd32(IST) & (1u << 19)));             /* DMA end not yet */
    run(&c, 100000);
    CHECK(dc_rd32(IST) & (1u << 19));
    CHECK(dc_rd32(0xA05F6808u) == 0);

    /* 4. maple: two-port command list, no devices -> 0xFFFFFFFF replies + maple DMA end */
    uint32_t list = 0x8C300000u, r0 = 0x8C310000u, r1 = 0x8C310100u;
    dc_wr32(list + 0, (0u << 16) | 0); dc_wr32(list + 4, r0); dc_wr32(list + 8, 0x01200001u);
    dc_wr32(list + 12, 0x80000000u | (1u << 16) | 0); dc_wr32(list + 16, r1); dc_wr32(list + 20, 0x01600001u);
    dc_wr32(r0, 0); dc_wr32(r1, 0);
    dc_wr32(0xA05F6C04u, list); dc_wr32(0xA05F6C14u, 1); dc_wr32(0xA05F6C18u, 1);
    CHECK(dc_rd32(r0) == 0xFFFFFFFFu && dc_rd32(r1) == 0xFFFFFFFFu);
    run(&c, 100000);
    CHECK(dc_rd32(IST) & (1u << 12));

    /* 5. G2 DMA to sound RAM */
    for (int i = 0; i < 16; i++) dc_wr32(0x8C400000u + i * 4, 0x11110000u + i);
    dc_wr32(0xA05F7800u, 0xA0800000u); dc_wr32(0xA05F7804u, 0x8C400000u); dc_wr32(0xA05F7808u, 64);
    dc_wr32(0xA05F780Cu, 0); dc_wr32(0xA05F7814u, 1); dc_wr32(0xA05F7818u, 1);
    CHECK(dc_rd32(0xA080003Cu) == 0x1111000Fu);
    run(&c, 100000);
    CHECK(dc_rd32(IST) & (1u << 15));

    /* 6. AICA: upload an ARM program, release ARMRST; it writes a mailbox in sound RAM and raises
     *    MCIPD bit 5 (ARM -> SH-4), which must reach Holly ISTEXT bit 1. Timer A must then count. */
    static const uint8_t armprog[] = { 0x20,0x00,0x9F,0xE5,0x42,0x10,0xA0,0xE3,0x00,0x10,0x80,0xE5,0x18,0x20,0x9F,0xE5,0x20,0x30,0xA0,0xE3,0x00,0x30,0x82,0xE5,0x10,0x40,0x9F,0xE5,0x00,0x50,0xA0,0xE3,0x00,0x50,0x84,0xE5,0xFE,0xFF,0xFF,0xEA,0x00,0x10,0x00,0x00,0xB8,0x28,0x80,0x00,0x90,0x28,0x80,0x00 };
    dc_wr32(0xA0702C00u, 1);
    for (unsigned i = 0; i < sizeof armprog; i += 4) { uint32_t w; memcpy(&w, armprog + i, 4); dc_wr32(0xA0800000u + i, w); }
    dc_wr32(0xA0801000u, 0);
    dc_wr32(0xA07028B4u, 0x20);                       /* MCIEB: CPU interrupt */
    dc_wr32(0xA0702C00u, 0);                          /* release the ARM */
    run(&c, 2000000);                                 /* 10 ms */
    CHECK(dc_rd32(0xA0801000u) == 0x42);
    CHECK(dc_rd32(0xA07028B8u) & 0x20);
    CHECK(dc_rd32(0xA05F6904u) & 2);                  /* ISTEXT: AICA */
    dc_wr32(0xA07028BCu, 0x20);                       /* MCIRE */
    run(&c, 10000);
    CHECK(!(dc_rd32(0xA05F6904u) & 2));
    CHECK((dc_rd32(0xA0702890u) & 0xFF) != 0);        /* timer A counted */

    /* 7. YUV converter: one 16x16 YUV420 macroblock (U=0x40, V=0xC0, Y=0x80) -> UYVY in VRAM + ISTNRM bit 6 */
    dc_wr32(IST, 0xFFFFFFFFu);
    dc_wr32(0xA05F814Cu, 0);                          /* 1x1 macroblocks, YUV420 */
    dc_wr32(0xA05F8148u, 0x00100000u);                /* texture base */
    uint32_t mbw[96];
    for (int i = 0; i < 96; i++) mbw[i] = i < 16 ? 0x40404040u : i < 32 ? 0xC0C0C0C0u : 0x80808080u;
    for (int i = 0; i < 96; i += 8) { uint32_t b8[8]; memcpy(b8, mbw + i, 32); dc_wr32(0xFFA00020u, 0); (void)b8; }
    memcpy(dc_ram + 0x500000, mbw, 384);
    dc_wr32(0xFFA00020u, 0x8C500000u); dc_wr32(0xA05F6800u, 0x10800000u); dc_wr32(0xA05F6804u, 384); dc_wr32(0xA05F6808u, 1);
    CHECK(dc_rd32(IST) & (1u << 6));
    CHECK(dc_rd32(0xA4100000u) == 0x80C08040u);       /* U Y V Y, in texture memory (64-bit area) */

    /* 8. maple controller: device info + condition on port A, nothing on port B */
    dc_maple_install();
    dc_pad[0].buttons = DC_BTN_START | DC_BTN_A; dc_pad[0].rt = 200; dc_pad[0].jx = 10;
    uint32_t ml = 0x8C320000u, ra = 0x8C330000u, rb = 0x8C330200u, rc = 0x8C330400u;
    dc_wr32(ml + 0, (0u << 16) | 0); dc_wr32(ml + 4, ra); dc_wr32(ml + 8, 0x00002001u);              /* DEVINFO to A */
    dc_wr32(ml + 12, (0u << 16) | 1); dc_wr32(ml + 16, rb); dc_wr32(ml + 20, 0x01002009u);           /* GETCOND to A */
    dc_wr32(ml + 24, 0x01000000u);
    dc_wr32(ml + 28, 0x80000000u | (1u << 16) | 0); dc_wr32(ml + 32, rc); dc_wr32(ml + 36, 0x00006001u);   /* DEVINFO to B */
    dc_wr32(0xA05F6C04u, ml); dc_wr32(0xA05F6C18u, 1);
    CHECK((dc_rd32(ra) & 0xFF) == 5 && (dc_rd32(ra) >> 24) == 28 && dc_rd32(ra + 4) == 0x01000000u);
    CHECK(dc_rd8(ra + 4 + 18) == 'D' && ((dc_rd32(ra) >> 16) & 0xFF) == 0x20);
    CHECK((dc_rd32(rb) & 0xFF) == 8 && (dc_rd32(rb) >> 24) == 3 && dc_rd32(rb + 4) == 0x01000000u);
    CHECK((dc_rd32(rb + 8) & 0xFFFF) == (uint16_t)~(DC_BTN_START | DC_BTN_A) && ((dc_rd32(rb + 8) >> 16) & 0xFF) == 200);
    CHECK((dc_rd32(rb + 12) & 0xFF) == 10 && ((dc_rd32(rb + 12) >> 8) & 0xFF) == 128);
    CHECK(dc_rd32(rc) == 0xFFFFFFFFu);

    /* 8b. VMU in slot 1 of port A: new formatted card, media info, block write + read, file */
    {
        const char *vp = "dctest_vmu.bin"; remove(vp);
        CHECK(dc_vmu_attach(0, vp) == 0);
        CHECK(dc_vmu_data(0)[255 * 512] == 0x55 && dc_vmu_data(0)[254 * 512 + 241 * 2] == 0xFA);
        uint32_t loc = 0u | 1u << 8 | 0u << 16 | 5u << 24;                         /* partition 0, phase 1, block 5 */
        uint32_t r1 = 0x8C330800u, r2 = 0x8C330A00u, r3 = 0x8C330C00u, r4 = 0x8C331000u, r5 = 0x8C331400u, q = ml;
        dc_wr32(q, 0); dc_wr32(q + 4, r1); dc_wr32(q + 8, 0x00002001u); q += 12;                       /* DEVINFO main */
        dc_wr32(q, 0x700); dc_wr32(q + 4, 0x700); dc_wr32(q + 8, 0x700); q += 12;                     /* 3 NOPs: one word each (Rush 2049) */
        dc_wr32(q, 0); dc_wr32(q + 4, r5); dc_wr32(q + 8, 0x00000101u); q += 12;                       /* DEVINFO VMU */
        dc_wr32(q, 1); dc_wr32(q + 4, r2); dc_wr32(q + 8, 0x0100010Au); dc_wr32(q + 12, 0x02000000u); q += 16;   /* GETMINFO */
        dc_wr32(q, 34); dc_wr32(q + 4, r3); dc_wr32(q + 8, 0x2200010Cu); dc_wr32(q + 12, 0x02000000u); dc_wr32(q + 16, loc);
        for (int i = 0; i < 32; i++) dc_wr32(q + 20 + i * 4, 0x11223300u + (uint32_t)i);
        q += 20 + 128;                                                                                  /* BWRITE 128 bytes */
        dc_wr32(q, 0x80000000u | 2); dc_wr32(q + 4, r4); dc_wr32(q + 8, 0x0200010Bu); dc_wr32(q + 12, 0x02000000u); dc_wr32(q + 16, loc & ~0xFF00u);
        dc_wr32(0xA05F6C04u, ml); dc_wr32(0xA05F6C18u, 1);
        CHECK(((dc_rd32(r1) >> 16) & 0xFF) == 0x21);                   /* controller reports the VMU */
        CHECK((dc_rd32(r5) & 0xFF) == 5 && dc_rd32(r5 + 4) == 0x0E000000u);   /* VMU found after the NOPs */
        CHECK(dc_rd32(r5 + 8) == 0x403F7E7Eu && dc_rd32(r5 + 16) == 0x00410F00u);   /* clock ... storage data */
        CHECK((dc_rd32(r2) & 0xFF) == 8 && (dc_rd32(r2 + 12) >> 16) == 254 && (dc_rd32(r2 + 24) & 0xFFFF) == 200);
        CHECK((dc_rd32(r3) & 0xFF) == 7 && ((dc_rd32(r3) >> 16) & 0xFF) == 0x01);
        CHECK((dc_rd32(r4) & 0xFF) == 8 && (dc_rd32(r4) >> 24) == 130 && dc_rd32(r4 + 8) == (loc & ~0xFF00u));
        CHECK(dc_rd32(r4 + 12 + 128) == 0x11223300u && dc_rd32(r4 + 12 + 128 + 31 * 4) == 0x1122331Fu && dc_rd32(r4 + 12) == 0);
        FILE *vf = fopen(vp, "rb"); uint8_t blk[512] = {0};
        CHECK(vf && fseek(vf, 5 * 512, SEEK_SET) == 0 && fread(blk, 1, 512, vf) == 512);
        if (vf) fclose(vf);
        CHECK(blk[128] == 0x00 && blk[129] == 0x33 && blk[131] == 0x11 && blk[132] == 0x01);
        dc_vmu_attach(0, NULL); remove(vp);
    }

    /* 9. AICA mixer: looping PCM16 square wave on channel 0 */
    for (int i = 0; i < 200; i++) { int16_t v = i < 100 ? 16000 : -16000; dc_wr16(0xA0810000u + i * 2, (uint16_t)v); }
    #define CH0(r, v) dc_wr32(0xA0700000u + (r) * 4, (v))
    dc_wr32(0xA0702800u, 15);                       /* MVOL */
    CH0(1, 0x0000); CH0(2, 0); CH0(3, 200);          /* SA low, LSA, LEA */
    CH0(4, 31); CH0(5, 0x3C00 | 0);                  /* AR 31; KRS 15 (off), DL 0, RR 0 */
    CH0(6, 0);                                       /* pitch 1.0 */
    CH0(9, 15 << 8);                                 /* DISDL 15, centre */
    CH0(10, 0);                                      /* TL 0 */
    { int16_t junk[2048]; while (dc_aica_audio_read(junk, 1024) > 0) {} }
    CH0(0, 0xC000 | (1 << 9) | (0 << 7) | 0x01);    /* KYONEX KYONB, loop, PCM16, SA 0x10000 */
    uint64_t t0 = c.cycles;
    for (int k = 1; k <= 20; k++) dc_aica_advance(t0 + (uint64_t)k * 200000000ull / 44100 * 50);
    static int16_t au[4096 * 2];
    int got = dc_aica_audio_read(au, 4096);
    CHECK(got >= 900);
    int maxv = 0, flips = 0;
    for (int i = 10; i < got; i++) { if (au[i * 2] > maxv) maxv = au[i * 2]; if ((au[i * 2] > 0) != (au[i * 2 - 2] > 0)) flips++; }
    CHECK(maxv > 7000 && maxv < 9000);               /* 16000 at half scale */
    CHECK(flips >= got / 100 - 3 && flips <= got / 100 + 3);   /* a sign change every 100 samples */
    CHECK(au[200 * 2] == au[200 * 2 + 1]);           /* centred */
    /* key off with a fast release: silence */
    CH0(5, 0x3C00 | 31); CH0(0, 0x8000 | (1 << 9) | 0x01);
    for (int k = 21; k <= 40; k++) dc_aica_advance(t0 + (uint64_t)k * 200000000ull / 44100 * 50);
    got = dc_aica_audio_read(au, 4096);
    CHECK(got > 100 && au[(got - 1) * 2] == 0);
    /* ADPCM channel: non-silent and bounded */
    for (int i = 0; i < 256; i++) dc_wr8(0xA0820000u + i, (uint8_t)(i & 1 ? 0x77 : 0x07));
    CH0(1, 0x0000); CH0(3, 400); CH0(5, 0x3C00); CH0(0, 0xC000 | (1 << 9) | (2 << 7) | 0x02);
    for (int k = 41; k <= 50; k++) dc_aica_advance(t0 + (uint64_t)k * 200000000ull / 44100 * 50);
    got = dc_aica_audio_read(au, 4096);
    maxv = 0; for (int i = 0; i < got; i++) { int v = au[i * 2] < 0 ? -au[i * 2] : au[i * 2]; if (v > maxv) maxv = v; }
    CHECK(maxv > 500 && maxv <= 16384);

    /* 10. effects DSP: channel -> MIXS0 -> 100-sample delay in the ring buffer -> EFREG0 */
    {
        CH0(5, 0x3C00 | 31); CH0(0, 0x8000 | 0x01);                       /* ADPCM voice off */
        for (int k = 51; k <= 60; k++) dc_aica_advance(t0 + (uint64_t)k * 200000000ull / 44100 * 50);
        { int16_t junk[2048]; while (dc_aica_audio_read(junk, 1024) > 0) {} }
        #define AR(o, v) dc_wr32(0xA0700000u + (o), (v))
        #define STEP(st, w0, w1, w2, w3) do { AR(0x3400 + (st) * 16, w0); AR(0x3404 + (st) * 16, w1); AR(0x3408 + (st) * 16, w2); AR(0x340C + (st) * 16, w3); } while (0)
        AR(0x3000, 0x0FFFu << 3);                                         /* COEF0 ~ 1.0 */
        AR(0x3200, 0); AR(0x3204, 100);                                   /* MADRS0 = 0, MADRS1 = 100 */
        AR(0x2804, 0x100);                                                /* ring buffer at 0x80000, 8K words */
        STEP(0, 0, 1u << 15 | 1u << 13 | 0x20u << 7, 1u << 1, 0);                                   /* ACC = MIXS0 * COEF0 */
        STEP(1, 0, 0, 1u << 14, 1u << 15 | 0u << 2);                                                 /* write ring[dec] (NOFL) */
        STEP(3, 0, 0, 1u << 13, 1u << 15 | 1u << 2);                                                 /* MEMVAL = ring[dec+100] */
        STEP(4, 0, 1u << 15 | 1u << 13 | 0u << 7 | 1u << 6 | 0u << 1, 1u << 1, 0);                   /* MEMS0 = MEMVAL; ACC = it * COEF0 */
        STEP(5, 0, 0, 1u << 12 | 0u << 8, 0);                                                         /* EFREG0 = ACC */
        AR(0x2000, 15u << 8);                                             /* EFSDL 15, centre */
        #define CH1(r, v) dc_wr32(0xA0700080u + (r) * 4, (v))
        CH1(1, 0x0000); CH1(2, 0); CH1(3, 200); CH1(4, 31); CH1(5, 0x3C00); CH1(6, 0);
        CH1(8, 15u << 4 | 0);                                             /* IMXL 15 -> MIXS0 */
        CH1(9, 0); CH1(10, 0);                                            /* DISDL 0: dry signal muted */
        uint64_t t1 = c.cycles + 100000000ull;
        dc_aica_advance(t1);
        { int16_t junk[2048]; while (dc_aica_audio_read(junk, 1024) > 0) {} }
        CH1(0, 0xC000 | (1 << 9) | 0x01);                                 /* key on: PCM16 square, period 200 */
        for (int k = 1; k <= 20; k++) dc_aica_advance(t1 + (uint64_t)k * 200000000ull / 44100 * 50);
        got = dc_aica_audio_read(au, 4096);
        int first = -1; maxv = 0;
        for (int i = 0; i < got; i++) { int v = au[i * 2] < 0 ? -au[i * 2] : au[i * 2]; if (v > 100 && first < 0) first = i; if (v > maxv) maxv = v; }
        CHECK(got > 500 && first >= 99 && first <= 102);                  /* silent until the delay elapses */
        CHECK(maxv > 7000 && maxv < 9000);                                /* full level through the DSP */
        CHECK(dc_aica_dsp_active());
        CH1(5, 0x3C00 | 31); CH1(0, 0x8000 | 0x01);
    }

    /* 10b. ADPCM at half pitch: each decoded sample twice (and no re-decode from the loop start) */
    {
        for (int i = 0; i < 0x4000; i++) dc_wr8(0xA0840000u + i, (uint8_t)(i * 37 + (i >> 3)));
        #define CH2(r, v) dc_wr32(0xA0700100u + (r) * 4, (v))
        CH2(1, 0x0000); CH2(2, 0x1000); CH2(3, 0x7000); CH2(4, 31); CH2(5, 0x3C00); CH2(6, 0xF800); CH2(8, 0); CH2(9, 15 << 8); CH2(10, 0);
        uint64_t t2 = c.cycles + 300000000ull;
        dc_aica_advance(t2);
        { int16_t junk[2048]; while (dc_aica_audio_read(junk, 1024) > 0) {} }
        CH2(0, 0xC000 | (1 << 9) | (2 << 7) | 0x04);          /* ADPCM at 0x40000, loop */
        for (int k = 1; k <= 10; k++) dc_aica_advance(t2 + (uint64_t)k * 200000000ull / 44100 * 50);
        got = dc_aica_audio_read(au, 4096);
        int pairs = 0, nonzero = 0;
        for (int i = 20; i + 1 < got; i += 2) { if (au[i * 2] == au[i * 2 + 2]) pairs++; if (au[i * 2]) nonzero++; }
        CHECK(got > 400 && nonzero > got / 8 && pairs > got / 2 - 20);
        CH2(5, 0x3C00 | 31); CH2(0, 0x8000 | 0x04);
    }
    /* 11. boot ROM: its system area is copied to RAM (games checksum it), vectors stay HLE */
    {
        static uint8_t rom[0x200000];
        for (int i = 0; i < 0x8000; i++) rom[i] = (uint8_t)(i * 13 + 7);
        FILE *rf = fopen("dctest_boot.bin", "wb"); fwrite(rom, 1, sizeof rom, rf); fclose(rf);
        CHECK(dc_bios_load("dctest_boot.bin", NULL) == 0);
        dc_bios_install(&c, 'E');
        CHECK(dc_ram[0x10F0] == rom[0x10F0] && dc_ram[0x3200] == rom[0x3200] && dc_ram[0x7FFF] == rom[0x7FFF]);
        CHECK(dc_rd32(0x8C0000BCu) == 0x8C0010F0u);          /* GD-ROM vector still the HLE entry */
        remove("dctest_boot.bin");
    }
    printf(fails ? "%d runtime test(s) FAILED\n" : "runtime tests passed\n", fails);
    return fails != 0;
}

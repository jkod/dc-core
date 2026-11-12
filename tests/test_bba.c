/* The Broadband Adapter (dccore/net.h), driven the way a game's network library drives it: find the
 * bridge, read the address from the EEPROM, start the chip, send through the four descriptors,
 * receive into the ring (wrap, CAPR, full ring holding frames back), interrupts and the flash's
 * network settings. */
#include "dccore/runtime.h"
#include "dccore/hw.h"
#include "dccore/bios.h"
#include "dccore/net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint32_t addr, end; sh4fn fn; } dc_fn_entry;
const dc_fn_entry dc_fn_table[1] = {{0, 0, 0}};
const size_t dc_fn_count = 0;

static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); fails++; } } while (0)

#define BASE 0xA1000000u
#define RTL(o) (BASE + 0x1700u + (o))
#define SRAM 0xA1840000u

static uint8_t sent[8][1600]; static int sent_len[8], nsent;
static void link_send(const uint8_t *f, int n) { if (nsent < 8) { memcpy(sent[nsent], f, (size_t)n); sent_len[nsent] = n; } nsent++; }
static uint8_t inq[64][1600]; static int inq_len[64], inq_head, inq_tail;
static int link_recv(uint8_t *buf, int max) {
    if (inq_head == inq_tail) return 0;
    int n = inq_len[inq_head % 64]; if (n > max) n = max;
    memcpy(buf, inq[inq_head % 64], (size_t)n); inq_head++;
    return n;
}
static void queue_frame(const uint8_t *dst, uint16_t type, int len, uint8_t fill) {
    uint8_t *f = inq[inq_tail % 64];
    memcpy(f, dst, 6); memset(f + 6, 0x22, 6); f[12] = (uint8_t)(type >> 8); f[13] = (uint8_t)type;
    for (int i = 14; i < len; i++) f[i] = (uint8_t)(fill + i);
    inq_len[inq_tail % 64] = len; inq_tail++;
    dc_net_rx_waiting = 1;
}

static uint16_t eeprom_read(int addr) {               /* 93C46 over 9346CR, as drivers bit-bang it */
    #define EE(cs, sk, di) dc_wr8(RTL(0x50), (uint8_t)(0x80 | (cs) << 3 | (sk) << 2 | (di) << 1))
    uint32_t cmd = 0x6u << 6 | (uint32_t)addr;         /* start bit, READ (10), 6-bit address */
    EE(1, 0, 0);
    for (int i = 8; i >= 0; i--) { int bit = cmd >> i & 1; EE(1, 0, bit); EE(1, 1, bit); }
    uint16_t v = 0;
    for (int i = 0; i < 16; i++) { EE(1, 0, 0); EE(1, 1, 0); v = (uint16_t)(v << 1 | (dc_rd8(RTL(0x50)) & 1)); }
    EE(0, 0, 0); dc_wr8(RTL(0x50), 0);
    return v;
}

int main(void) {
    sh4ctx c; dc_init(&c); dc_hw_verbose = 0;
    dc_net.send = link_send; dc_net.recv = link_recv;

    /* without the adapter the expansion port is empty */
    CHECK(dc_rd32(BASE + 0x1400) != 0x53504147u);

    dc_bba_enabled = 1;
    memcpy(dc_bba_mac, "\x00\xD0\xF1\x12\x34\x56", 6);
    dc_bba_reset();
    char id[17] = {0};
    for (int i = 0; i < 16; i += 4) { uint32_t w = dc_rd32(BASE + 0x1400 + i); memcpy(id + i, &w, 4); }
    CHECK(!strcmp(id, "GAPSPCI_BRIDGE_2"));
    CHECK(dc_rd32(BASE + 0x141C) == 0x41474553u);         /* "SEGA" */
    dc_wr32(BASE + 0x141C, 0x55AAFF00u);                  /* and the scratch test after it */
    CHECK(dc_rd32(BASE + 0x141C) == 0x55AAFF00u);
    dc_wr32(BASE + 0x1418, 0x5A14A501u);                 /* bridge reset, then wait for bit 0 */
    CHECK(dc_rd32(BASE + 0x1418) & 1);
    dc_wr32(BASE + 0x1414, 1);                            /* interrupts on */
    CHECK(dc_rd16(BASE + 0x1600) == 0x10EC && dc_rd16(BASE + 0x1602) == 0x8139);
    dc_wr32(BASE + 0x1614, 0x01000000u);                  /* memory BAR */
    CHECK(dc_rd32(BASE + 0x1614) == 0x01000000u);
    dc_wr32(BASE + 0x1610, 0xFFFFFFFFu);                  /* sizing the I/O BAR */
    CHECK(dc_rd32(BASE + 0x1610) == 0xFFFFFF01u);

    /* chip reset, address from the registers and from the EEPROM */
    dc_wr8(RTL(0x37), 0x10);
    CHECK((dc_rd8(RTL(0x37)) & 0x10) == 0);
    CHECK(dc_rd8(RTL(0x37)) & 1);                         /* receive buffer empty */
    CHECK(dc_rd32(RTL(0x00)) == 0xF1D000u + (0x12u << 24) && dc_rd16(RTL(4)) == 0x5634);
    CHECK(eeprom_read(0) == 0x8129);
    CHECK(eeprom_read(7) == 0xD000 && eeprom_read(8) == 0x12F1 && eeprom_read(9) == 0x5634);
    CHECK(dc_rd16(RTL(0x64)) & 4);                        /* PHY: link up */
    CHECK((dc_rd8(RTL(0x58)) & 4) == 0);                  /* media status: link up */

    /* receive ring 8 KB at SRAM 0, transmit buffers after it; accept broadcast + own address */
    dc_wr32(RTL(0x30), 0x01840000u);
    dc_wr32(RTL(0x44), 0x0000000Au);                      /* AB | APM, 8 KB, no WRAP */
    dc_wr16(RTL(0x3C), 0x0005);                           /* ROK | TOK */
    dc_wr8(RTL(0x37), 0x0C);                              /* RE | TE */

    /* send: frame in SRAM, address, then size into TSD0 (OWN clear) */
    uint8_t frame[60] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0xD0, 0xF1, 0x12, 0x34, 0x56, 0x08, 0x06};
    for (int i = 0; i < 60; i += 4) { uint32_t w; memcpy(&w, frame + i, 4); dc_wr32(SRAM + 0x4000 + i, w); }
    for (int d = 0; d < 4; d++) {
        dc_wr32(RTL(0x20 + d * 4), 0x01844000u);
        dc_wr32(RTL(0x10 + d * 4), 60);
        CHECK(dc_rd32(RTL(0x10 + d * 4)) & (1u << 15));   /* TOK */
        CHECK(dc_rd32(RTL(0x10 + d * 4)) & (1u << 13));   /* OWN: the host's again */
    }
    CHECK(nsent == 4 && sent_len[0] == 60 && !memcmp(sent[0], frame, 60));
    /* through the DMA window: with offset 0x4FF6, window byte 10 is SRAM 0x5000 */
    dc_wr32(BASE + 0x142C, 0x01844FF6u);
    for (int i = 0; i < 64; i += 4) dc_wr32(0xA1848000u + i, 0xEEEEEEEEu);
    for (int i = 0; i < 60; i += 4) { uint32_t w; memcpy(&w, frame + i, 4); dc_wr32(0xA1848000u + 8 + i, w); }
    dc_wr32(RTL(0x20), 0x01845000u);                      /* window byte 10 (the frame's byte 2) = SRAM 0x5000 */
    dc_wr32(RTL(0x10), 58);
    CHECK(nsent == 5 && sent_len[4] == 58 && !memcmp(sent[4], frame + 2, 58));
    nsent = 4;
    CHECK(dc_rd16(RTL(0x3E)) & 4);
    CHECK(dc_rd32(0xA05F6904u) & 8);                      /* Holly ISTEXT bit 3 */
    dc_wr16(RTL(0x3E), 4);                                /* acknowledge */
    CHECK(!(dc_rd16(RTL(0x3E)) & 4));
    CHECK(!(dc_rd32(0xA05F6904u) & 8));
    dc_wr8(RTL(0x37), 0x08);                              /* TE off: a descriptor is not sent */
    dc_wr32(RTL(0x10), 60);
    CHECK(nsent == 4);
    dc_wr8(RTL(0x37), 0x0C);

    /* receive: one to us, one broadcast, one to someone else (filtered), a short one (padded) */
    static const uint8_t me[6] = {0x00, 0xD0, 0xF1, 0x12, 0x34, 0x56}, other[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    static const uint8_t bc[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    queue_frame(me, 0x0800, 100, 1);
    queue_frame(bc, 0x0806, 42, 2);
    queue_frame(other, 0x0800, 100, 3);
    dc_bba_poll();
    CHECK(dc_rd16(RTL(0x3E)) & 1);                        /* ROK */
    CHECK(!(dc_rd8(RTL(0x37)) & 1));
    uint32_t st = dc_rd32(SRAM);
    CHECK((st & 0xFFFF) == (1u | 1u << 14));              /* ROK, physical address match */
    CHECK((st >> 16) == 104);                             /* length with the CRC */
    CHECK(dc_rd8(SRAM + 4) == 0x00 && dc_rd8(SRAM + 4 + 14) == (uint8_t)(1 + 14));
    uint32_t next = (4 + 104 + 3) & ~3u;
    st = dc_rd32(SRAM + next);
    CHECK((st & 0xFFFF) == (1u | 1u << 13) && (st >> 16) == 64);   /* broadcast, padded to 60 */
    uint32_t end = (next + 4 + 64 + 3) & ~3u;
    CHECK(dc_rd16(RTL(0x3A)) == end);                     /* CBR: nothing more (the third filtered) */
    dc_wr16(RTL(0x38), (uint16_t)(end - 16));             /* read everything */
    CHECK(dc_rd8(RTL(0x37)) & 1);
    CHECK(dc_bba_rx_frames == 2);

    /* a full ring holds frames back until the driver reads */
    int before = (int)dc_bba_rx_frames, queued = 0;
    for (int i = 0; i < 12; i++, queued++) queue_frame(me, 0x0800, 1000, (uint8_t)i);
    dc_bba_poll();
    int got = (int)(dc_bba_rx_frames - (uint64_t)before);
    CHECK(got >= 6 && got < queued);                      /* 8 KB: about 7 of 1 KB */
    uint32_t rd = end;
    for (int k = 0; k < 40 && dc_bba_rx_frames - (uint64_t)before < (uint64_t)queued; k++) {
        /* the driver reads one frame and moves CAPR on, with the ring wrapping at 8 KB */
        uint32_t h = dc_rd32(SRAM + (rd % 8192));
        CHECK((h & 1) && (h >> 16) == 1004);
        rd = ((rd + 4 + (h >> 16) + 3) & ~3u) % 8192;
        dc_wr16(RTL(0x38), (uint16_t)((rd - 16) & 0xFFFF));
        dc_bba_poll();
    }
    CHECK(dc_bba_rx_frames - (uint64_t)before == (uint64_t)queued);

    /* the network settings in the flash's user partition: the INET block and the adapter's */
    dc_bios_install(&c, '1');
    uint8_t *u = dc_flash + 0x1C000;
    int found_inet = 0, found_bba = 0, found_sys = 0;
    for (int blkn = 1; blkn < 255; blkn++) {
        const uint8_t *bk = u + blkn * 64;
        uint16_t bid = (uint16_t)(bk[0] | bk[1] << 8);
        if (bid == 0x80 && !memcmp(bk + 2, "PWBrowser", 9)) found_inet = 1;
        if (bid == 0xE0 && !memcmp(bk + 2, "SEGA", 4)) found_bba++;
        if (bid == 0x05) found_sys = 1;
    }
    CHECK(found_inet && found_bba == 1 && found_sys);
    CHECK((u[0x4000 - 0x40] & 0x80) == 0);                /* block 1 marked in use */

    /* a player's own flash, set up but never for the internet: the network blocks are added */
    {
        static uint8_t fl[0x20000];
        memcpy(fl, dc_flash, sizeof fl);                  /* the factory partition as above */
        uint8_t *up = fl + 0x1C000;
        memset(up + 0x40, 0xFF, 0x4000 - 0x40);           /* only block 1: system settings, a marker */
        memcpy(up + 0x40, dc_flash + 0x1C040, 64);
        up[0x40 + 30] = 0x5A;
        up[0x4000 - 0x40] = 0x7F;
        FILE *f = fopen("test_bba_flash/dc_flash.bin", "wb");
        if (!f) { (void)!system("mkdir test_bba_flash"); f = fopen("test_bba_flash/dc_flash.bin", "wb"); }
        CHECK(f != NULL);
        if (f) { fwrite(fl, 1, sizeof fl, f); fclose(f); }
        CHECK(dc_bios_load_dir("test_bba_flash") == 0);
        CHECK(dc_bios_load_dir("no_such_folder") == -1);
        dc_bios_install(&c, '1');
        int inet = 0, sys = 0, bbab = 0;
        for (int blkn = 1; blkn < 255; blkn++) {
            const uint8_t *bk = dc_flash + 0x1C000 + blkn * 64;
            if (dc_flash[0x1C000 + 0x4000 - 0x40 + (blkn - 1) / 8] & (0x80 >> ((blkn - 1) % 8))) continue;
            uint16_t bid = (uint16_t)(bk[0] | bk[1] << 8);
            if (bid == 0x05 && bk[30] == 0x5A && blkn == 1) sys = 1;
            if (bid == 0x80) inet = blkn;
            if (bid == 0xE0) bbab = 1;
        }
        CHECK(sys && inet == 2 && bbab);
    }

    printf(fails ? "bba: %d FAILED\n" : "bba: all passed\n", fails);
    return fails != 0;
}

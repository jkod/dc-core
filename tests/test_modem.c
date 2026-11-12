/* The modem (dccore/net.h, runtime/src/modem.c), driven the way a game's modem library drives it:
 * ID, reset and self tests, DSP RAM, dialling, the far end answering, the handshake, bytes both
 * ways at the line's speed, carrier loss and hanging up. */
#include "dccore/runtime.h"
#include "dccore/hw.h"
#include "dccore/bios.h"
#include "dccore/net.h"
#include <stdio.h>
#include <string.h>

typedef struct { uint32_t addr, end; sh4fn fn; } dc_fn_entry;
const dc_fn_entry dc_fn_table[1] = {{0, 0, 0}};
const size_t dc_fn_count = 0;

static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); fails++; } } while (0)

#define REG(n) (0xA0600400u + (n) * 4u)
static sh4ctx c;
static uint8_t rd(int n) { return (uint8_t)dc_rd8(REG(n)); }
static void wr(int n, uint8_t v) { dc_wr8(REG(n), v); }
static void run_ms(int ms) { for (int i = 0; i < ms * 10; i++) { c.cycles += 20000; dc_events(&c); } }

static uint8_t sent[256]; static int nsent, opened, hung;
static void link_send(const uint8_t *p, int n) { while (n-- > 0 && nsent < 256) sent[nsent++] = *p++; }
static const uint8_t *in; static int in_n;
static int link_recv(uint8_t *buf, int max) { int n = in_n < max ? in_n : max; memcpy(buf, in, (size_t)n); in += n; in_n -= n; return n; }
static void link_open(void) { opened++; }
static void link_hangup(void) { hung++; }

static void dsp_write(unsigned a, uint8_t v) {
    wr(0x18, v); wr(0x1C, (uint8_t)a); wr(0x1D, (uint8_t)(0x80 | 0x20 | (a >> 8)));
}
static uint8_t dsp_read(unsigned a) {
    wr(0x1C, (uint8_t)a); wr(0x1D, (uint8_t)(0x80 | (a >> 8)));
    return rd(0x18);
}

int main(void) {
    dc_init(&c); dc_hw_verbose = 0;
    dc_net.send = link_send; dc_net.recv = link_recv; dc_net.open = link_open; dc_net.hangup = link_hangup;

    CHECK(dc_rd32(0xA0600000u) == 0);                     /* no modem: reads as nothing */
    dc_modem_enabled = 1;
    dc_modem_reset();
    CHECK(dc_rd32(0xA0600000u) == 2 && dc_rd32(0xA0600004u) == 0x10);   /* USA, Rockwell 33.6k */

    /* reset, then the self tests */
    dc_wr8(0xA0600480u, 0);
    dc_wr8(0xA0600480u, 1);
    CHECK(rd(0x12) == 0x30 && rd(0x13) == 0x37);          /* controller test: part number */
    run_ms(60);
    CHECK(rd(0x13) == 0x37 && rd(0x14) == 0x09 && rd(0x15) == 0x0A);   /* DSP test: ROM checksum */
    run_ms(60);
    CHECK(rd(0x12) == 0x76);                              /* ready: CONF V.32 bis */
    CHECK(rd(0x0B) & 0xE0);                               /* dial tone */
    CHECK(rd(0x1E) & 0x08);                               /* TDBE */

    /* DSP RAM: power-on contents, a write and the read back */
    CHECK(dsp_read(0x32C) == 0xCB);
    dsp_write(0x400, 0x5A);
    CHECK(dsp_read(0x400) == 0x5A);

    /* V.34 33.6k, dial, wait for the answer */
    wr(0x12, 0xCE);
    wr(0x1F, 0x01);                                       /* NEWC: take the configuration */
    CHECK(!(rd(0x1F) & 1));
    wr(0x09, 0x20);                                       /* DTMF, not DATA yet */
    for (const char *d = "5551234"; *d; d++) { wr(0x10, (uint8_t)*d); run_ms(5); }
    run_ms(300);
    CHECK(!(rd(0x0F) & 0x08));                            /* still ringing */
    run_ms(1000);
    CHECK(rd(0x0F) & 0x08);                               /* RI: answered */
    CHECK(rd(0x0B) & 0x10);                               /* answer tone */
    wr(0x08, 0x81);                                       /* asynchronous, RTS */
    wr(0x04, 0x10);                                       /* FIFOs on */
    wr(0x09, 0x05);                                       /* DATA + DTR: handshake */
    run_ms(5);
    CHECK(!(rd(0x0F) & 0x08));
    CHECK(opened == 0);
    run_ms(600);
    CHECK(opened == 1);
    uint8_t f = rd(0x0F);
    CHECK((f & 0x10) && (f & 0x40) && (f & 0x80) && (f & 0x20));   /* DSR, FED, RLSD, CTS */
    CHECK((rd(0x0E) & 0x1F) == 0x0E + 2);                 /* SPEED: 33600 */

    /* bytes out */
    const char *lcp = "\x7E\xFF\x7D\x23\xC0\x21";
    for (int i = 0; i < 6; i++) wr(0x10, (uint8_t)lcp[i]);
    CHECK(nsent == 6 && !memcmp(sent, lcp, 6));
    /* bytes in, at 33.6k: 4200 a second, so 42 in 10 ms */
    static uint8_t data[200];
    for (int i = 0; i < 200; i++) data[i] = (uint8_t)i;
    in = data; in_n = 200; dc_net_rx_waiting = 1;
    int got = 0, ok = 1;
    for (int t = 0; t < 10 * 10; t++) {
        c.cycles += 20000; dc_events(&c);
        while (rd(0x0C) & 2) { uint8_t b = rd(0x00); if (b != (uint8_t)got) ok = 0; got++; }   /* RXFNE */
    }
    CHECK(ok);
    CHECK(got >= 35 && got <= 50);
    for (int t = 0; t < 60 * 10; t++) {
        c.cycles += 20000; dc_events(&c);
        while (rd(0x0C) & 2) { uint8_t b = rd(0x00); if (b != (uint8_t)got) ok = 0; got++; }
    }
    CHECK(ok && got == 200);
    /* a slow reader: the FIFO holds 16, RDBF at the trigger level (12), nothing lost */
    in = data; in_n = 40; dc_net_rx_waiting = 1; got = 0;
    run_ms(20);
    CHECK(rd(0x1E) & 1);
    for (int t = 0; t < 30 * 10; t++) {
        c.cycles += 20000; dc_events(&c);
        if (t % 50 == 0) while (rd(0x0C) & 2) { uint8_t b = rd(0x00); if (b != (uint8_t)got) ok = 0; got++; }
    }
    while (rd(0x0C) & 2) { uint8_t b = rd(0x00); if (b != (uint8_t)got) ok = 0; got++; }
    CHECK(ok && got == 40);

    /* the far end goes away: carrier drops */
    dc_net_carrier_lost = 1;
    run_ms(1);
    CHECK(!(rd(0x0F) & 0x80));
    /* call again and hang up with DTR */
    wr(0x09, 0x20);
    for (const char *d = "1"; *d; d++) wr(0x10, (uint8_t)*d);
    run_ms(1300);
    wr(0x09, 0x05);
    run_ms(600);
    CHECK(opened == 2 && (rd(0x0F) & 0x80));
    wr(0x09, 0x04);                                       /* DTR off */
    CHECK(hung == 1 && !(rd(0x0F) & 0x80));

    printf(fails ? "modem: %d FAILED\n" : "modem: all passed\n", fails);
    return fails != 0;
}

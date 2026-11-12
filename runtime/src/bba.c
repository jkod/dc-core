/* The Broadband Adapter (dccore/net.h): GAPSPCI bridge, RTL8139C and 32 KB of SRAM.
 *
 * Expansion-port layout (offsets from 0x01000000):
 *   0x1400  GAPS bridge: "GAPSPCI_BRIDGE_2" at +0x00, control registers after it ("SEGA" in the
 *           scratch word at +0x1C until written)
 *   0x1600  the RTL8139's PCI configuration space
 *   0x1700  the RTL8139's registers (its memory BAR, which drivers set to 0x01000000)
 *   0x840000  the SRAM, 32 KB: the chip's receive ring and transmit buffers live here, so its "DMA"
 *             addresses are SRAM offsets
 *   0x848000  the bridge's DMA window onto the same SRAM, starting at the offset in bridge register
 *             +0x2C: drivers copy a frame from RAM with G2 DMA (32-byte aligned) through it, setting
 *             the offset so the frame's first byte lands at its transmit buffer
 *
 * The chip is modelled at the level a driver sees it, following the RTL8139C datasheet: the
 * register file, the four transmit descriptors, the receive ring with its packet headers and the
 * CAPR quirk, the 93C46 serial EEPROM and a PHY whose link is always up at 100 Mb/s full duplex.
 * Transmission completes at once; received frames go into the ring when it has room, so a slow
 * game holds them in the link's queue instead of losing them. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dccore/net.h"
#include "dccore/aica.h"       /* dc_set_ext_irq */
#include "dccore/reset.h"      /* dc_state_fn */

dc_net_link dc_net;
volatile int dc_net_rx_waiting;
int dc_bba_enabled;
uint8_t dc_bba_mac[6];
int dc_bba_trace;
uint64_t dc_bba_tx_frames, dc_bba_rx_frames, dc_bba_rx_dropped;

#define TR(...) do { if (dc_bba_trace) fprintf(stderr, __VA_ARGS__); } while (0)

/* RTL8139 registers */
enum {
    IDR0 = 0x00, MAR0 = 0x08, TSD0 = 0x10, TSAD0 = 0x20, RBSTART = 0x30, CR = 0x37, CAPR = 0x38,
    CBR = 0x3A, IMR = 0x3C, ISR = 0x3E, TCR = 0x40, RCR = 0x44, TCTR = 0x48, MPC = 0x4C,
    CR9346 = 0x50, CONFIG0 = 0x51, CONFIG1 = 0x52, MSR = 0x58, CONFIG3 = 0x59, CONFIG4 = 0x5A,
    MULINT = 0x5C, RERID = 0x5E, TSAD = 0x60, BMCR = 0x62, BMSR = 0x64, ANAR = 0x66, ANLPAR = 0x68,
    ANER = 0x6A, CSCR = 0x74, HLTCLK = 0x5B, CONFIG5 = 0xD8,
};
enum { CR_BUFE = 1, CR_TE = 4, CR_RE = 8, CR_RST = 0x10 };
enum { INT_ROK = 1, INT_TOK = 4, INT_RXOVW = 0x10 };
enum { TSD_OWN = 1u << 13, TSD_TOK = 1u << 15 };
enum { RCR_AAP = 1, RCR_APM = 2, RCR_AM = 4, RCR_AB = 8, RCR_WRAP = 0x80 };
enum { RX_ROK = 1, RX_BAR = 1 << 13, RX_PAM = 1 << 14, RX_MAR = 1 << 15 };

#define SRAM_SIZE 0x8000u
#define MAX_FRAME 1514

static struct {
    uint8_t gaps[0x100], pci[0x100], reg[0x100], sram[SRAM_SIZE];
    uint32_t rx_size;                      /* receive ring length (8, 16, 32 or 64 KB, + 16) */
    uint32_t rx_read;                      /* where the driver will read next (CAPR + 16) */
    uint32_t rx_write;                     /* where the next frame goes (CBR) */
    uint16_t eeprom[64];
    int ee_cs, ee_sk, ee_do, ee_bits, ee_state;   /* 0 idle, 1 command, 2 data out */
    uint32_t ee_in, ee_out;
    int irq;
} b;

int dc_bba_take(const uint8_t *f, int n);

static uint16_t rd16(int o) { return (uint16_t)(b.reg[o] | b.reg[o + 1] << 8); }
static uint32_t rd32(int o) { return rd16(o) | (uint32_t)rd16(o + 2) << 16; }
static void put16(int o, uint32_t v) { b.reg[o] = (uint8_t)v; b.reg[o + 1] = (uint8_t)(v >> 8); }
static void put32(int o, uint32_t v) { put16(o, v); put16(o + 2, v >> 16); }

static void update_irq(void) {
    int on = (rd16(ISR) & rd16(IMR)) && (b.gaps[0x14] & 1);
    if (on != b.irq) TR("bba: interrupt %s (ISR %04X IMR %04X)\n", on ? "on" : "off", rd16(ISR), rd16(IMR));
    b.irq = on;
    dc_set_ext_irq(1u << 3, on);                 /* Holly ISTEXT bit 3: expansion device */
}

static void chip_reset(void) {
    memset(b.reg, 0, sizeof b.reg);
    memcpy(b.reg + IDR0, b.eeprom + 7, 6);       /* the address loads from the EEPROM */
    for (int i = 0; i < 4; i++) put32(TSD0 + i * 4, TSD_OWN);
    b.reg[CR] = CR_BUFE;
    put32(TCR, 0x74800000u);                     /* hardware version: RTL8139C */
    b.reg[CONFIG1] = 0x0C;
    b.reg[MSR] = 0x10;                           /* 100 Mb/s, link up (LINKB = 0) */
    put16(BMCR, 0x2100);                         /* 100 Mb/s, full duplex */
    put16(BMSR, 0x782D);                         /* capabilities, link up, autonegotiation complete */
    put16(ANAR, 0x01E1); put16(ANLPAR, 0x45E1); put16(ANER, 1);
    put16(CSCR, 0x0400);
    b.reg[HLTCLK] = 'R';
    b.rx_size = 8192 + 16; b.rx_read = 0; b.rx_write = 0;
    update_irq();
}

void dc_bba_reset(void) {
    memset(&b, 0, sizeof b);
    if (!dc_bba_mac[0] && !dc_bba_mac[1] && !dc_bba_mac[2]) {   /* Sega's OUI, as on a real adapter */
        unsigned r = (unsigned)time(NULL) ^ (unsigned)clock() * 2654435761u ^ (unsigned)(uintptr_t)&b;
        r ^= r >> 13; r *= 0x5BD1E995u; r ^= r >> 15;
        dc_bba_mac[0] = 0x00; dc_bba_mac[1] = 0xD0; dc_bba_mac[2] = 0xF1;
        dc_bba_mac[3] = (uint8_t)r; dc_bba_mac[4] = (uint8_t)(r >> 8); dc_bba_mac[5] = (uint8_t)(r >> 16);
    }
    memcpy(b.gaps, "GAPSPCI_BRIDGE_2", 16);
    memcpy(b.gaps + 0x1C, "SEGA", 4);            /* a scratch word, "SEGA" at power-on: drivers check both */
    /* PCI configuration: Realtek 8139, network controller, an I/O and a memory BAR, INTA */
    static const uint8_t pci[0x40] = {
        0xEC, 0x10, 0x39, 0x81, 0x00, 0x00, 0x90, 0x02, 0x10, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xEC, 0x10, 0x39, 0x81,
        0, 0, 0, 0, 0x50, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x01, 0x20, 0x40 };
    memcpy(b.pci, pci, sizeof pci);
    b.eeprom[0] = 0x8129; b.eeprom[1] = 0x10EC; b.eeprom[2] = 0x8139;
    for (int i = 0; i < 3; i++) b.eeprom[7 + i] = (uint16_t)(dc_bba_mac[i * 2] | dc_bba_mac[i * 2 + 1] << 8);
    chip_reset();
}

void dc_bba_state(dc_state_fn f) { f(&b, sizeof b); }

/* ---------------------------------------------------------------- 93C46 EEPROM (bit-banged) */
static void eeprom_wires(uint8_t v) {
    int cs = v >> 3 & 1, sk = v >> 2 & 1, di = v >> 1 & 1;
    if (!cs) { b.ee_cs = 0; b.ee_state = 0; b.ee_do = 0; return; }
    if (!b.ee_cs) { b.ee_state = 0; b.ee_bits = 0; b.ee_in = 0; }
    b.ee_cs = 1;
    if (sk && !b.ee_sk) {                             /* rising clock */
        if (b.ee_state == 0) { if (di) { b.ee_state = 1; b.ee_bits = 0; b.ee_in = 0; } }   /* start bit */
        else if (b.ee_state == 1) {                   /* opcode (2) + address (6) */
            b.ee_in = b.ee_in << 1 | (uint32_t)di;
            if (++b.ee_bits == 8) {
                if ((b.ee_in >> 6) == 2) {            /* READ */
                    b.ee_out = b.eeprom[b.ee_in & 63]; b.ee_state = 2; b.ee_bits = 0; b.ee_do = 0;   /* dummy 0 */
                } else b.ee_state = 3;                /* writes and the rest: ignored */
            }
        } else if (b.ee_state == 2) {
            b.ee_do = b.ee_out >> 15 & 1; b.ee_out = (b.ee_out << 1) & 0xFFFF;
            if (++b.ee_bits == 16) { b.ee_bits = 0; b.ee_out = b.eeprom[(++b.ee_in) & 63]; }   /* sequential read */
        }
    }
    b.ee_sk = sk;
}

/* ---------------------------------------------------------------- transmit */
static void transmit(int d) {
    uint32_t tsd = rd32(TSD0 + d * 4), len = tsd & 0x1FFF, at = rd32(TSAD0 + d * 4) & (SRAM_SIZE - 1);
    if (!(b.reg[CR] & CR_TE) || (tsd & TSD_OWN)) return;
    uint8_t frame[2048];
    if (len > sizeof frame) len = sizeof frame;
    for (uint32_t i = 0; i < len; i++) frame[i] = b.sram[(at + i) & (SRAM_SIZE - 1)];
    TR("bba: send %u bytes to %02X:%02X:%02X:%02X:%02X:%02X type %02X%02X\n", len, frame[0], frame[1],
       frame[2], frame[3], frame[4], frame[5], frame[12], frame[13]);
    if (dc_bba_trace) { for (uint32_t i = 0; i < len && i < 96; i++) fprintf(stderr, "%02X%s", frame[i], i % 32 == 31 ? "\n" : " "); fprintf(stderr, "\n"); }
    if (len >= 14) {
        dc_bba_tx_frames++;
        if ((rd32(TCR) >> 17 & 3) == 3) {          /* loopback: straight back into the ring */
            dc_bba_take(frame, (int)len);
        } else if (dc_net.send) dc_net.send(frame, (int)len);
    }
    put32(TSD0 + d * 4, tsd | TSD_OWN | TSD_TOK);
    put16(ISR, rd16(ISR) | INT_TOK);
    update_irq();
}

/* ---------------------------------------------------------------- receive */
static uint32_t rx_free(void) {                    /* bytes the chip may write before the read pointer */
    if (b.rx_write == b.rx_read) return b.rx_size - 16;
    return (b.rx_read + b.rx_size - b.rx_write) % b.rx_size;
}
static void ring_put(const uint8_t *p, uint32_t n) {
    uint32_t base = rd32(RBSTART);
    for (uint32_t i = 0; i < n; i++) {
        b.sram[(base + b.rx_write) & (SRAM_SIZE - 1)] = p[i];
        b.rx_write++;
        /* WRAP set: the frame continues past the end (the driver allocated the extra 1.5 KB) */
        if (b.rx_write >= b.rx_size && !(rd32(RCR) & RCR_WRAP)) b.rx_write = 0;
    }
}
static uint32_t crc32_eth(const uint8_t *p, int n) {
    uint32_t c = 0xFFFFFFFFu;
    for (int i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1))); }
    return ~c;
}

/* one frame into the ring: 1 taken (or filtered out), 0 no room yet */
int dc_bba_take(const uint8_t *f, int n) {
    if (!(b.reg[CR] & CR_RE) || b.reg[HLTCLK] == 'H' || n < 14 || n > MAX_FRAME) return 1;
    uint32_t rcr = rd32(RCR), st = RX_ROK;
    static const uint8_t bc[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    if (!memcmp(f, bc, 6)) { if (!(rcr & (RCR_AB | RCR_AAP))) return 1; st |= RX_BAR; }
    else if (f[0] & 1) {
        uint32_t h = crc32_eth(f, 6) >> 26;
        if (!(rcr & RCR_AAP) && !((rcr & RCR_AM) && (b.reg[MAR0 + (h >> 3)] >> (h & 7) & 1))) return 1;
        st |= RX_MAR;
    } else if (!memcmp(f, b.reg + IDR0, 6)) { if (!(rcr & (RCR_APM | RCR_AAP))) return 1; st |= RX_PAM; }
    else if (!(rcr & RCR_AAP)) return 1;
    uint8_t buf[MAX_FRAME + 64];
    int len = n < 60 ? 60 : n;                       /* short frames padded, as on the wire */
    memset(buf, 0, (size_t)len);
    memcpy(buf + 4, f, (size_t)n);
    uint32_t crc = crc32_eth(buf + 4, len);
    buf[0] = (uint8_t)st; buf[1] = (uint8_t)(st >> 8);
    buf[2] = (uint8_t)(len + 4); buf[3] = (uint8_t)((len + 4) >> 8);
    memcpy(buf + 4 + len, &crc, 4);                  /* little-endian, like the chip */
    uint32_t total = (uint32_t)(4 + len + 4 + 3) & ~3u;
    if (total + 16 > rx_free()) return 0;
    ring_put(buf, (uint32_t)(4 + len + 4));
    b.rx_write = ((b.rx_write + 3) & ~3u) % b.rx_size;
    put16(CBR, b.rx_write);
    b.reg[CR] &= (uint8_t)~CR_BUFE;
    put16(ISR, rd16(ISR) | INT_ROK);
    dc_bba_rx_frames++;
    TR("bba: received %d bytes from %02X:%02X:%02X:%02X:%02X:%02X type %02X%02X\n", n, f[6], f[7], f[8],
       f[9], f[10], f[11], f[12], f[13]);
    update_irq();
    return 1;
}

void dc_bba_poll(void) {
    static uint8_t held[MAX_FRAME + 2];
    static int held_len;
    if (!dc_bba_enabled) return;
    for (int i = 0; i < 64; i++) {
        if (!held_len) {
            if (!dc_net_rx_waiting || !dc_net.recv) return;
            held_len = dc_net.recv(held, MAX_FRAME);
            if (held_len <= 0) { held_len = 0; dc_net_rx_waiting = 0; return; }
        }
        if (!dc_bba_take(held, held_len)) {           /* the ring is full: try again later */
            if (!(b.reg[CR] & CR_RE)) held_len = 0;
            return;
        }
        held_len = 0;
    }
}

/* ---------------------------------------------------------------- registers */
static uint8_t reg_byte(int o) {
    switch (o) {
    case CR: return (uint8_t)((b.reg[CR] & ~CR_BUFE) | (b.rx_read == b.rx_write ? CR_BUFE : 0));
    case CR9346: return (uint8_t)((b.reg[CR9346] & ~1) | ((b.reg[CR9346] >> 6) == 2 && b.ee_cs ? b.ee_do : 0));
    case TSAD: case TSAD + 1: {                     /* transmit status of all descriptors */
        uint16_t s = 0;
        for (int d = 0; d < 4; d++) {
            uint32_t t = rd32(TSD0 + d * 4);
            if (t & TSD_OWN) s |= (uint16_t)(1u << d);
            if (t & TSD_TOK) s |= (uint16_t)(0x1000u << d);
        }
        return (uint8_t)(o == TSAD ? s : s >> 8);
    }
    case RERID: return 0x10;
    }
    return b.reg[o];
}

static void after_write(int o, int size) {
    int send = 0;
    for (int i = 0; i < size; i++) {
        int r = o + i;
        if (r == CR) {
            if (b.reg[CR] & CR_RST) chip_reset();
            if (!(b.reg[CR] & CR_RE)) { b.rx_read = b.rx_write = 0; put16(CBR, 0); }
        } else if (r == CR9346) {
            uint8_t m = b.reg[CR9346] >> 6;
            if (m == 2) eeprom_wires(b.reg[CR9346]);
            else if (m == 1) b.reg[CR9346] = 0;     /* auto-load: done at once */
        } else if (r == RCR + 1) {
            uint32_t n = 8192u << (rd32(RCR) >> 11 & 3);
            if (b.rx_size != n + 16) { b.rx_size = n + 16; b.rx_read = b.rx_write = 0; put16(CBR, 0); }
        } else if (r == CAPR + 1) {
            b.rx_read = (uint32_t)(rd16(CAPR) + 16) % b.rx_size;   /* room again: the next poll fills it */
        } else if (r == MPC + 3) put32(MPC, 0);
        else if (r == IMR + 1) update_irq();
        else if (r >= TSD0 && r < TSD0 + 16) send |= 1 << ((r - TSD0) >> 2);   /* OWN cleared: send it */
    }
    for (int d = 0; d < 4; d++) if (send >> d & 1) transmit(d);
}

static uint32_t rtl_read(int o, int size) {
    uint32_t v = 0;
    for (int i = 0; i < size; i++) v |= (uint32_t)reg_byte((o + i) & 0xFF) << (8 * i);
    return v;
}

static void rtl_write(int o, uint32_t v, int size) {
    TR("bba: rtl[%02X] = %0*X\n", o, size * 2, v);
    for (int i = 0; i < size; i++) {
        int r = (o + i) & 0xFF;
        uint8_t x = (uint8_t)(v >> (8 * i));
        if (r == ISR || r == ISR + 1) { b.reg[r] &= (uint8_t)~x; continue; }   /* write 1 to clear */
        if (r == TSAD || r == TSAD + 1 || r == RERID || r == MSR || r == BMSR || r == BMSR + 1) continue;
        if (r == CBR || r == CBR + 1) continue;
        if (r >= TSD0 && r < TSD0 + 16 && (r & 3) == 1) x &= (uint8_t)~0xC0;   /* TOK, TUN: the chip's */
        if ((r == CONFIG0 || r == CONFIG1 || r == CONFIG3 || r == CONFIG4) && (b.reg[CR9346] >> 6) != 3) continue;
        b.reg[r] = x;
    }
    if (o == ISR || o == ISR + 1) update_irq();
    after_write(o, size);
}

/* ---------------------------------------------------------------- the expansion port */
static uint32_t sram_at(uint32_t m) {
    uint32_t o = m & 0xFFFF;
    if (o & 0x8000) o += b.gaps[0x2C] | (uint32_t)b.gaps[0x2D] << 8;   /* the DMA window */
    return o & (SRAM_SIZE - 1);
}
static uint32_t sram_rd(uint32_t m, int size) {
    uint32_t v = 0;
    for (int i = 0; i < size; i++) v |= (uint32_t)b.sram[sram_at(m + (uint32_t)i)] << (8 * i);
    return v;
}
static void sram_wr(uint32_t m, uint32_t v, int size) {
    for (int i = 0; i < size; i++) b.sram[sram_at(m + (uint32_t)i)] = (uint8_t)(v >> (8 * i));
}

uint32_t dc_bba_read(uint32_t a, int size) {
    uint32_t m = a & 0x00FFFFFFu, v = 0;
    if (m >= 0x840000u && m < 0x850000u) return sram_rd(m, size);
    if (m >= 0x1400 && m < 0x1500) {
        if ((m & 0xFC) == 0x18) return 1;          /* bridge reset: done */
        memcpy(&v, b.gaps + (m & 0xFF), (size_t)size);
    } else if (m >= 0x1600 && m < 0x1700) memcpy(&v, b.pci + (m & 0xFF), (size_t)size);
    else if (m >= 0x1700 && m < 0x1800) v = rtl_read((int)(m & 0xFF), size);
    else v = 0xFFFFFFFFu;
    if (dc_bba_trace && !(m >= 0x1700 && m < 0x1800 && (m & 0xFF) == ISR)) TR("bba: read%d %06X = %X\n", size * 8, m, v);
    return size == 4 ? v : size == 2 ? v & 0xFFFF : v & 0xFF;
}

void dc_bba_write(uint32_t a, uint32_t v, int size) {
    uint32_t m = a & 0x00FFFFFFu;
    if (m >= 0x840000u && m < 0x850000u) { sram_wr(m, v, size); return; }
    if (m >= 0x1400 && m < 0x1500) {
        TR("bba: gaps[%02X] = %X\n", m & 0xFF, v);
        if ((m & 0xFF) >= 0x10) memcpy(b.gaps + (m & 0xFF), &v, (size_t)size);   /* the ID string stays */
        if ((m & 0xFC) == 0x14) update_irq();
        if ((m & 0xFC) == 0x18 && (v & 1) && v != 1) { /* 0x5A14A501: reset the PCI side */ chip_reset(); }
    } else if (m >= 0x1600 && m < 0x1700) {
        uint32_t o = m & 0xFF;
        TR("bba: pci[%02X] = %X\n", o, v);
        for (int i = 0; i < size; i++) {             /* command, latency, BARs, interrupt line */
            uint32_t r = o + (uint32_t)i;
            if (r == 4 || r == 5 || r == 0x0D || (r >= 0x10 && r < 0x18) || r == 0x3C) b.pci[r] = (uint8_t)(v >> (8 * i));
        }
        b.pci[0x10] = 1;                             /* both BARs 256 bytes: the I/O one */
        b.pci[0x14] = 0;                             /* and the memory one */
    } else if (m >= 0x1700 && m < 0x1800) rtl_write((int)(m & 0xFF), v, size);
    else TR("bba: write%d %06X = %X (nothing there)\n", size * 8, m, v);
}

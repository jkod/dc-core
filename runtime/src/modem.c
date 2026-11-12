/* The Dreamcast's 33.6k modem (dccore/net.h): a Rockwell data pump behind Sega's glue, at 0x00600000.
 *
 *   0x600000-0x6003FF  ID: words alternating country (2 = USA) and maker/type (0x10 = Rockwell, 33.6k)
 *   0x600400 + 4n      register n (0x00-0x1F, one byte each): the data pump's interface registers
 *   0x600480           reset: 0 holds the chip in reset, non-zero releases it into its self tests
 * Interrupts on Holly ISTEXT bit 2.
 *
 * Modelled at the level of the registers a game's modem library drives, as the emulators do: the
 * reset and self-test results, the DSP RAM reached through MEADDL/MEADDH/MEDA (with its power-on
 * contents), dialling (digits written to TBUFFER), the remote answering, the handshake and the
 * connection at the speed the game configured. Once connected, bytes the game writes go to
 * dc_net.send and bytes from dc_net.recv come in at the line's speed: the game's PPP, carried to
 * DCNet's dial-up access (dc_net.open when the line comes up, dc_net.hangup when the game drops
 * DTR or resets the chip).
 *
 * In synchronous mode the console's modem library does the error correction in software, so the
 * far end's side is here: the line carries a bit stream, and characters travel in it with start
 * and stop bits (V.14). A game that sends V.42's detection pattern (ODP) gets the answer (ADP);
 * one that then opens a V.42 link (LAPM, HDLC frames) gets a peer for that; otherwise, after the
 * detection time (750 ms), the call goes on as plain V.14. V.8bis is not modelled. */
#include <stdio.h>
#include <string.h>
#include "dccore/net.h"
#include "dccore/aica.h"       /* dc_set_ext_irq */
#include "dccore/sh4.h"
extern sh4ctx *dc_current_ctx;

int dc_modem_enabled;
volatile int dc_net_carrier_lost;
int dc_modem_country = 2;      /* 0 reserved, 1 Japan, 2 USA */
int dc_modem_trace;
#define TR(...) do { if (dc_modem_trace) fprintf(stderr, __VA_ARGS__); } while (0)

#define CPU_HZ 200000000ull
#define MS(n) (CPU_HZ / 1000 * (n))

/* register bits */
enum {
    R01_RXHF = 0x02, R01_TXHF = 0x04, R02_RTSDE = 0x08, R02_TDE = 0x40, R03_RLSDE = 0x10,
    R04_FIFOEN = 0x10, R05_CEQ = 0x08, R07_RDLE = 0x80, R08_RTS = 0x01, R08_ASYN = 0x80,
    R09_DTR = 0x01, R09_DATA = 0x04, R09_DTMF = 0x20, R0B_ATV25 = 0x10, R0B_TONES = 0xE0,
    R0C_RXFNE = 0x02, R0D_TXFNF = 0x02, R0F_RTSDT = 0x02, R0F_RI = 0x08, R0F_DSR = 0x10,
    R0F_CTS = 0x20, R0F_FED = 0x40, R0F_RLSD = 0x80, R15_AUTO = 0x08, R15_HWRWK = 0x10,
    R15_RDWK = 0x20, R1A_SFRES = 0x80, R1D_MEMW = 0x20, R1D_MEACC = 0x80, R1E_RDBF = 0x01,
    R1E_RDBIE = 0x04, R1E_TDBE = 0x08, R1E_TDBIE = 0x20, R1E_RDBIA = 0x40, R1E_TDBIA = 0x80,
    R1F_NEWC = 0x01, R1F_NCIE = 0x04, R1F_NEWS = 0x08, R1F_NSIE = 0x10, R1F_NCIA = 0x40, R1F_NSIA = 0x80,
};
enum { TBUFFER = 0x10, CONF = 0x12, ABCODE = 0x14, SECRXB = 0x16, SECTXB = 0x17, MEDAL = 0x18, MEDAM = 0x19 };

/* bits the host may write, per register (the rest are status the chip sets) */
static const uint8_t wmask[0x21] = {
    0xFF, 0xE0, 0xEF, 0xF3, 0x95, 0x3A, 0x7F, 0xEB, 0xFF, 0xFF, 0x08, 0x03, 0x01, 0x00, 0x20, 0x03,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xF7, 0x00, 0xFF, 0xBF, 0x24, 0x1D, 0xFF };
/* where in DSP RAM each status register's NEWS mask lives (0: none) */
static const uint16_t news_mask_at[0x21] = {
    [0x01] = 0x247, [0x0A] = 0x246, [0x0B] = 0x245, [0x0C] = 0x244, [0x0D] = 0x243, [0x0E] = 0x242,
    [0x0F] = 0x241, [0x12] = 0x089, [0x14] = 0x38A, [0x16] = 0x370, [0x17] = 0x371, [0x1A] = 0x27D, [0x1B] = 0x27C };
/* the DSP RAM's non-zero bytes after power-on */
static const struct { uint16_t at; uint8_t n, v[10]; } dsp_por[] = {
    {0x09F, 1, {0x97}}, {0x0B3, 1, {0x01}}, {0x133, 1, {0x30}}, {0x209, 1, {0x20}},
    {0x218, 5, {0xDD, 0xAD, 0x08, 0x07, 0x99}}, {0x21E, 2, {0x23, 0xA0}}, {0x22C, 1, {0x56}},
    {0x285, 1, {0x20}}, {0x287, 1, {0x20}}, {0x28B, 1, {0x10}}, {0x28E, 1, {0x10}}, {0x290, 2, {0x32, 0x2C}},
    {0x29B, 4, {0xC0, 0x19, 0x85, 0x20}}, {0x2DA, 1, {0x01}}, {0x2E4, 2, {0x0D, 0x0E}},
    {0x304, 6, {0xC0, 0x28, 0x40, 0x03, 0x84, 0xCC}},
    {0x30B, 10, {0x76, 0x84, 0xB1, 0x14, 0x02, 0x08, 0x08, 0xA4, 0xA4, 0xA0}}, {0x32C, 1, {0xCB}},
    {0x3B0, 9, {0x20, 0x1A, 0x12, 0x0C, 0x0A, 0x09, 0x05, 0x03, 0x02}}, {0x3BA, 5, {0x90, 0x28, 0x10, 0x04, 0x01}},
    {0x3DB, 1, {0x40}}, {0x409, 1, {0xCE}}, {0x6A2, 2, {0xC0, 0x05}}, {0x701, 2, {0x01, 0x01}},
    {0xAA1, 1, {0xCF}}, {0xAA6, 2, {0x02, 0xDF}}, {0xAAD, 1, {0xCF}}, {0xC94, 3, {0x1B, 0x2D, 0x14}},
    {0xE96, 1, {0x14}}, {0xF20, 2, {0x7F, 0xF0}},
};
static const unsigned rx_trigger[4] = {1, 4, 8, 12};

enum { CHIP_OFF, CHIP_RESET, CHIP_TEST_CTL, CHIP_TEST_DSP, CHIP_TEST_END, CHIP_NORMAL };
enum { LINE_IDLE, LINE_DIALING, LINE_RINGING, LINE_HANDSHAKE, LINE_TRAINING, LINE_UP };

static struct {
    uint8_t r[0x21];
    uint8_t dsp[0x1000];
    int chip, line;
    uint64_t next_at;            /* the chip's next timed step (cycles), 0 = none */
    uint64_t next_prev;          /* when the line's last byte time was due */
    uint64_t dial_at;            /* when dialling went quiet */
    uint64_t cycles_per_byte;
    float tx_fill, tx_per_rx;    /* bytes in the transmit FIFO; speed ratio of the two directions */
    uint8_t rxq[16]; int rx_n;
    int word_write, download; uint8_t download_crc, saved_1b;
    /* synchronous mode: the far end's V.14 / V.42 side */
    int sync_phase;              /* SYNC_* */
    uint64_t sync_since;         /* when the line came up (V.42's detection timer) */
    uint8_t enc[96]; int enc_n;  /* bytes for the game's receiver, bit stream */
    uint32_t enc_bits; int enc_nbits;
    int dec_state, dec_bits, dec_ones, dec_max_ones; uint8_t dec_byte;
    int odp_count; uint8_t odp_last;
} m;
enum { SYNC_NONE, SYNC_DETECT, SYNC_V14, SYNC_LAPM };
#define SYNC_DETECT_TIME MS(750)

static uint64_t now(void) { return dc_current_ctx ? dc_current_ctx->cycles : 0; }

/* ---------------------------------------------------------------- V.14: characters in a bit stream */
static void enc_bit(int bit) {                   /* bits go out LSB first */
    m.enc_bits |= (uint32_t)(bit & 1) << m.enc_nbits;
    if (++m.enc_nbits == 8) {
        if (m.enc_n < (int)sizeof m.enc) m.enc[m.enc_n++] = (uint8_t)m.enc_bits;
        m.enc_bits = 0; m.enc_nbits = 0;
    }
}
static void enc_char(uint8_t c, int stop_bits) {
    enc_bit(0);
    for (int i = 0; i < 8; i++) enc_bit(c >> i & 1);
    for (int i = 0; i < stop_bits; i++) enc_bit(1);
}
static void enc_idle(void) {                     /* mark: finish the byte, or a whole byte of ones */
    if (m.enc_nbits) while (m.enc_nbits) enc_bit(1);
    else for (int i = 0; i < 8; i++) enc_bit(1);
}
static uint8_t enc_take(void) {
    uint8_t v = m.enc[0];
    memmove(m.enc, m.enc + 1, (size_t)--m.enc_n);
    return v;
}
/* the game's bit stream back into characters: each with the run of ones (stop bits) before it */
static int dec_byte(uint8_t v, uint8_t *out, int *ones) {
    int n = 0;
    for (int i = 0; i < 8; i++, v >>= 1) {
        int bit = v & 1;
        if (m.dec_state == 0) {                      /* idle: ones, until a start bit */
            if (bit) m.dec_ones++;
            else { m.dec_state = 1; m.dec_bits = 0; m.dec_max_ones = m.dec_ones; }
        } else if (m.dec_state == 1) {               /* data bits */
            m.dec_byte = (uint8_t)(m.dec_byte >> 1 | bit << 7);
            if (++m.dec_bits == 8) { ones[n] = m.dec_max_ones; out[n++] = m.dec_byte; m.dec_state = 2; }
        } else {                                     /* the stop bit */
            if (bit) { m.dec_state = 0; m.dec_ones = 1; }
            else { m.dec_state = 1; m.dec_bits = 0; m.dec_max_ones = 0; }   /* missing: the next start */
        }
    }
    return n;
}

void dc_modem_lapm_reset(void);
int dc_modem_lapm_write(uint8_t v);             /* the game's LAPM bit stream (bytes) */
int dc_modem_lapm_read(void);                   /* the far end's: a byte, always */

/* a byte the game's transmitter sent, once the line is up */
static void line_tx(uint8_t v) {
    if (m.r[0x08] & R08_ASYN) { if (dc_net.send) dc_net.send(&v, 1); return; }
    if (m.sync_phase == SYNC_LAPM) { dc_modem_lapm_write(v); return; }
    if (m.sync_phase != SYNC_V14 && now() - m.sync_since >= SYNC_DETECT_TIME) {
        m.sync_phase = SYNC_V14;
        TR("modem: V.14 (no V.42)\n");
    }
    uint8_t c[2]; int ones[2], n = dec_byte(v, c, ones);
    for (int i = 0; i < n; i++) {
        if (m.sync_phase == SYNC_V14) { if (dc_net.send) dc_net.send(&c[i], 1); continue; }
        /* V.42 detection: ODP is DC1 with alternating parity, each after 8 or 16 stop bits */
        if ((c[i] == 0x11 || c[i] == 0x91) && (ones[i] == 9 || ones[i] == 17) && (m.sync_phase == SYNC_NONE || c[i] != m.odp_last)) {
            if (m.sync_phase == SYNC_NONE) { m.sync_phase = SYNC_DETECT; m.odp_count = 1; }
            else if (++m.odp_count == 4) {           /* answer: ADP, "EC" after 8 stop bits */
                m.odp_count = 0;
                enc_char('E', 9); enc_char('C', 9);
                TR("modem: V.42 detection pattern, answered\n");
            }
        } else if (m.sync_phase == SYNC_DETECT) {
            m.odp_count = 0;
            if (c[i] == 0x7E) { m.sync_phase = SYNC_LAPM; dc_modem_lapm_reset(); TR("modem: V.42 LAPM\n"); }
        }
        m.odp_last = c[i];
    }
}

/* the next byte for the game's receiver (-1: none) */
static int line_rx(void) {
    uint8_t c;
    if (m.r[0x08] & R08_ASYN) {
        if (dc_net_rx_waiting && dc_net.recv) { if (dc_net.recv(&c, 1) == 1) return c; dc_net_rx_waiting = 0; }
        return -1;
    }
    if (m.sync_phase == SYNC_LAPM) return dc_modem_lapm_read();
    if (m.sync_phase == SYNC_V14)                    /* what the far end sent, as characters */
        while (m.enc_n < (int)sizeof m.enc - 12 && dc_net_rx_waiting && dc_net.recv) {
            if (dc_net.recv(&c, 1) != 1) { dc_net_rx_waiting = 0; break; }
            enc_char(c, 1);
        }
    if (!m.enc_n) enc_idle();
    return enc_take();
}
static void after(uint64_t cycles) { m.next_at = now() + cycles; dc_next_event = 0; }

static void update_irq(void) {
    uint8_t *e = &m.r[0x1E], *f = &m.r[0x1F];
    *e = (uint8_t)((*e & ~(R1E_RDBIA | R1E_TDBIA)) | ((*e & R1E_RDBIE) && (*e & R1E_RDBF) ? R1E_RDBIA : 0)
                   | ((*e & R1E_TDBIE) && (*e & R1E_TDBE) ? R1E_TDBIA : 0));
    dc_set_ext_irq(1u << 2, (*f & (R1F_NCIA | R1F_NSIA)) || (*e & (R1E_RDBIA | R1E_TDBIA)));
}

/* a status bit changes: with NEWS interrupts on, a change the DSP's mask selects raises NSIA */
static void status(int reg, uint8_t bit, int on) {
    uint8_t old = m.r[reg], v = on ? (uint8_t)(old | bit) : (uint8_t)(old & ~bit);
    if (v == old) return;
    m.r[reg] = v;
    if ((m.r[0x1F] & R1F_NSIE) && news_mask_at[reg]) {
        uint8_t mask = m.dsp[news_mask_at[reg]];
        if ((old & mask) != (v & mask)) m.r[0x1F] |= R1F_NSIA;
    }
}

static void rx_status(void) {
    int n = m.rx_n;
    if (n) m.r[0x00] = m.rxq[0];
    m.r[0x1E] = (uint8_t)((m.r[0x1E] & ~R1E_RDBF) | (n && (unsigned)n >= rx_trigger[m.dsp[0x32C] >> 6 & 3] ? R1E_RDBF : 0));
    if (m.r[0x04] & R04_FIFOEN) status(0x0C, R0C_RXFNE, n > 0);
    status(0x01, R01_RXHF, n >= 8);
}

static void put16(int hi, int lo, unsigned v) { m.r[hi] = (uint8_t)(v >> 8); m.r[lo] = (uint8_t)v; }

static void normal_defaults(void) {
    memset(m.r, 0, sizeof m.r);
    memset(m.dsp, 0, sizeof m.dsp);
    for (unsigned i = 0; i < sizeof dsp_por / sizeof *dsp_por; i++) memcpy(m.dsp + dsp_por[i].at, dsp_por[i].v, dsp_por[i].n);
    m.r[0x05] = R05_CEQ; m.r[CONF] = 0x76;               /* V.32 bis 14400 */
    m.r[0x09] = R09_DATA | R09_DTMF; m.r[0x15] = R15_HWRWK | R15_RDWK; m.r[0x07] = R07_RDLE;
    m.r[0x03] = R03_RLSDE; m.r[0x02] = R02_TDE; m.r[0x13] = 0x09; m.r[0x1E] = R1E_TDBE;
    m.line = LINE_IDLE; m.dial_at = 0; m.tx_fill = 0; m.rx_n = 0;
}

static void hangup(void) {
    if (m.line == LINE_UP || m.line == LINE_TRAINING) { TR("modem: hang up\n"); if (dc_net.hangup) dc_net.hangup(); }
    m.line = LINE_IDLE;
}

static void chip_reset(uint32_t v) {
    if (v == 0) { hangup(); memset(m.r, 0, sizeof m.r); m.chip = CHIP_RESET; m.next_at = 0; TR("modem: reset\n"); return; }
    if (m.chip == CHIP_RESET) {                          /* released: controller self test results */
        memset(m.r, 0, sizeof m.r);
        m.r[0x1E] = R1E_TDBE;
        put16(0x1D, 0x1C, 0xEA3C); put16(0x1B, 0x1A, 0x5536); put16(0x19, 0x18, 0x5F4C);
        put16(0x17, 0x16, 0x3835); put16(0x15, 0x14, 0x0801); put16(0x13, 0x12, 0x3730); put16(0x11, 0x00, 0x4241);
        m.chip = CHIP_TEST_DSP;
        after(MS(50));
    }
    m.r[0x20] = (uint8_t)v;
}

/* the connection's speed from CONF: 0 = a configuration that does not connect */
static int set_speed(void) {
    static const int v90[] = {0, 28800, 29333, 30667, 32000, 33333, 34667, 36000, 37333, 38667, 40000, 41333,
                              42667, 44000, 45333, 46667, 48000, 49333, 50667, 52000, 53333, 54667, 56000};
    static const int v34[] = {0, 2400, 4800, 7200, 9600, 12000, 14400, 16800, 19200, 21600, 24000, 26400, 28800, 31200, 33600};
    static const struct { uint8_t conf; int tx, rx; } fixed[] = {
        {0x31, 14400, 0}, {0x32, 12000, 0}, {0x34, 9600, 0}, {0x38, 7200, 0}, {0x76, 14400, 0}, {0x72, 12000, 0},
        {0x74, 9600, 0}, {0x75, 9600, 0}, {0x78, 7200, 0}, {0x71, 4800, 0}, {0xB1, 14400, 0}, {0xB2, 12000, 0},
        {0xB4, 9600, 0}, {0xB8, 7200, 0}, {0x14, 9600, 0}, {0x12, 7200, 0}, {0x11, 4800, 0}, {0x02, 4800, 0},
        {0x01, 2400, 0}, {0x84, 2400, 0}, {0x82, 1200, 0}, {0x52, 1200, 0}, {0x51, 600, 0}, {0xA0, 300, 0},
        {0xA8, 300, 0}, {0x23, 4800, 0}, {0x62, 1200, 0}, {0x60, 300, 0}, {0xA4, 1200, 75}, {0xA1, 75, 1200} };
    uint8_t c = m.r[CONF];
    int tx = 0, rx = 0, code = 0;
    if (c >= 0xE0 && c <= 0xF6) {                        /* V.90 */
        rx = v90[c - 0xE0];
        if (rx) { m.dsp[0x2E4] = (uint8_t)(c - 0xE0); m.dsp[0x2E5] = 0x0D; tx = 33600; code = 0x10; }
    } else if (c >= 0x90 && c <= 0x9D) {                 /* K56flex */
        if (c != 0x90) { rx = 30000 + (c - 0x90) * 2000; tx = 33600; code = 0x10; }
    } else if (c >= 0xC0 && c <= 0xCE) {                 /* V.34 */
        tx = v34[c - 0xC0];
        if (tx) code = c - 0xC0 + 2;
        m.dsp[0x2E4] = m.dsp[0x2E5] = (uint8_t)(c - 0xC0);
    } else
        for (unsigned i = 0; i < sizeof fixed / sizeof *fixed; i++) if (fixed[i].conf == c) { tx = fixed[i].tx; rx = fixed[i].rx; }
    if (!rx) rx = tx;
    if (tx && !code) {
        static const int sp[][2] = {{14400, 7}, {12000, 6}, {9600, 5}, {7200, 8}, {4800, 4}, {2400, 3}, {1200, 2}, {600, 1}};
        for (unsigned i = 0; i < 8; i++) if (sp[i][0] == tx) code = sp[i][1];
    }
    m.r[0x0E] = (uint8_t)((m.r[0x0E] & ~0x1F) | (tx ? code : 0));
    m.cycles_per_byte = CPU_HZ * 8 / (uint64_t)(tx ? (rx ? rx : tx) : 300);
    m.tx_per_rx = tx ? (float)tx / (float)rx : 1.f;
    TR("modem: connected, %d / %d bit/s (CONF %02X)\n", rx, tx, c);
    return tx != 0;
}

/* the chip's timed steps */
static void step(void) {
    uint64_t was = m.next_at;
    m.next_at = 0;
    m.next_prev = was;
    switch (m.chip) {
    case CHIP_TEST_DSP:
        m.r[0x1E] |= R1E_TDBE;
        put16(0x1B, 0x1A, 0xF083); put16(0x19, 0x18, 0x46EE); put16(0x17, 0x16, 0x00FA);
        put16(0x15, 0x14, 0x0A09); put16(0x13, 0x12, 0x3730); put16(0x11, 0x00, 0x2041);
        m.chip = CHIP_TEST_END; after(MS(50));
        return;
    case CHIP_TEST_END:
        m.chip = CHIP_NORMAL; normal_defaults(); TR("modem: ready\n");
        update_irq();
        return;
    case CHIP_NORMAL: break;
    default: return;
    }
    m.r[0x1F] &= (uint8_t)~R1F_NEWC;
    switch (m.line) {
    case LINE_DIALING:                                   /* a second after the last digit, the far end rings */
        if (m.dial_at && now() - m.dial_at >= CPU_HZ) { m.line = LINE_RINGING; after(MS(100)); }
        else { m.dial_at = now(); m.r[0x1E] |= R1E_TDBE; after(MS(1000)); }
        break;
    case LINE_RINGING:                                   /* answered: answer tone */
        m.dial_at = 0;
        m.r[0x1F] |= R1F_NEWS;
        if (!(m.r[0x09] & R09_DATA)) { status(0x0F, R0F_RI, 1); status(0x0B, R0B_ATV25, 1); }
        TR("modem: answered\n");
        break;
    case LINE_HANDSHAKE:
        if (m.r[CONF] == 0xAA) m.dsp[0x302] |= 1 << 3;  /* V.8 automode: ANSam detected */
        m.r[0x1F] |= R1F_NEWS;
        status(0x0F, R0F_RI, 0); status(0x0B, R0B_ATV25, 0);
        m.line = LINE_TRAINING; after(MS(500));
        break;
    case LINE_TRAINING: {
        if (m.r[0x03] & R03_RLSDE) status(0x0F, R0F_RLSD, 1);
        if (m.r[CONF] == 0xAA) {                         /* V.8 automode: the best the game allows */
            m.dsp[0x302] |= 1 << 4;
            m.dsp[0x302] = (uint8_t)((m.dsp[0x302] & 0x1F) | (m.dsp[0x304] & 0xE0));
            m.dsp[0x301] |= 1 << 4; m.dsp[0x303] |= 0xE1; m.dsp[0x2E3] = 5;
            if ((m.dsp[0x6A3] & 1) && (m.dsp[0x6A2] & 0x20)) {
                m.r[CONF] = (uint8_t)(0xE0 | (m.dsp[0x6A2] & 0x1F));                   /* V.90 */
                m.dsp[0x6C1] = 0xFF; m.dsp[0x6C2] = 0xFF; m.dsp[0x6C3] = 0x3F;
            } else m.r[CONF] = m.dsp[0x309];
            m.dsp[0x239] = 12;
            if ((m.r[0x1F] & R1F_NSIE) && (m.dsp[news_mask_at[CONF]] & 0x80)) m.r[0x1F] |= R1F_NSIA;
        }
        int ok = set_speed();
        if ((m.r[0x1F] & R1F_NSIE) && (m.dsp[news_mask_at[0x0E]] & 0x1F)) m.r[0x1F] |= R1F_NSIA;
        if (m.r[0x15] & R15_AUTO) { m.r[0x09] |= R09_DATA; m.r[0x15] &= (uint8_t)~R15_AUTO; }
        if (!ok) { m.r[ABCODE] = 1; m.line = LINE_IDLE; TR("modem: no connection (CONF %02X)\n", m.r[CONF]); }
        else {
            m.r[ABCODE] = 0;
            status(0x0F, R0F_DSR, 1);
            if (m.r[0x02] & R02_RTSDE) status(0x0F, R0F_RTSDT, 1);
            status(0x0F, R0F_FED, 1);                    /* energy on the line: games wait for it */
            m.sync_phase = SYNC_NONE; m.sync_since = now(); m.enc_n = 0; m.enc_nbits = 0; m.enc_bits = 0;
            m.dec_state = 0; m.dec_ones = 0; m.odp_count = 0;
            TR("modem: %s\n", m.r[0x08] & R08_ASYN ? "asynchronous" : "synchronous");
            dc_net_carrier_lost = 0;
            if (dc_net.open) dc_net.open();
            m.line = LINE_UP;
        }
        if ((m.r[0x1F] & R1F_NSIE) && m.dsp[news_mask_at[ABCODE]]) m.r[0x1F] |= R1F_NSIA;
        m.r[0x1F] |= R1F_NEWS;
        m.dsp[0x208] = 0xFF; m.dsp[0x209] = 0xBF;        /* the far modem: every V.34 rate */
        after(CPU_HZ / 1000000 * 238);
        m.next_prev = m.next_at;
        break;
    }
    case LINE_UP:                                        /* one byte time: the FIFOs move */
        if (dc_net_carrier_lost) {                       /* the far end is gone */
            dc_net_carrier_lost = 0;
            status(0x0F, R0F_RLSD, 0); status(0x0F, R0F_FED, 0); status(0x0F, R0F_DSR, 0); status(0x0F, R0F_CTS, 0);
            m.r[0x1F] |= R1F_NEWS; m.line = LINE_IDLE;
            TR("modem: carrier lost\n");
            break;
        }
        m.tx_fill = m.tx_fill > m.tx_per_rx ? m.tx_fill - m.tx_per_rx : 0;
        if (m.tx_fill == 0) m.r[0x1E] |= R1E_TDBE; else m.r[0x1E] &= (uint8_t)~R1E_TDBE;
        if (m.rx_n < 16) { int c = line_rx(); if (c >= 0) { m.rxq[m.rx_n++] = (uint8_t)c; rx_status(); } }
        {   /* on the line's clock, not from whenever the game looked last */
            uint64_t due = m.next_prev + m.cycles_per_byte, t = now();
            m.next_at = due > t ? due : t + 1; m.next_prev = m.next_at; dc_next_event = 0;
        }
        break;
    }
    update_irq();
}

void dc_modem_events(uint64_t t, uint64_t *next) {
    if (m.next_at && t >= m.next_at) step();
    if (m.next_at && m.next_at < *next) *next = m.next_at;
}

static void handshake_if_ready(void) {
    if ((m.r[0x09] & R09_DTR) && ((m.r[0x09] & R09_DATA) || (m.r[0x15] & R15_AUTO)) && m.line == LINE_RINGING) {
        TR("modem: DTR, handshake\n");
        m.line = LINE_HANDSHAKE; after(MS(1));
    }
}

static void write_reg(int reg, uint8_t v) {
    uint8_t old = m.r[reg];
    m.r[reg] = (uint8_t)((old & ~wmask[reg]) | (v & wmask[reg]));
    switch (reg) {
    case 0x02:
        if (m.r[0x02] & R02_RTSDE && m.line == LINE_UP) m.r[0x0F] |= R0F_RTSDT; else m.r[0x0F] &= (uint8_t)~R0F_RTSDT;
        break;
    case 0x09:
        handshake_if_ready();
        if (m.line == LINE_UP) {
            if (m.r[0x09] & R09_DTR) status(0x0F, R0F_DSR, 1);
            else { status(0x0F, R0F_DSR, 0); status(0x0F, R0F_RLSD, 0); hangup(); }
        }
        break;
    case TBUFFER:
        if (m.download) m.download_crc = (uint8_t)((m.download_crc << 1) + (m.download_crc >> 7) + v);
        else if (m.line == LINE_IDLE || m.line == LINE_DIALING) {   /* dialling: the digits go out as tones */
            if (m.line == LINE_IDLE) { TR("modem: dialling\n"); m.line = LINE_DIALING; m.r[SECRXB] = 0; }
            after(MS(100));
        } else if (m.line == LINE_UP && (m.r[0x08] & R08_RTS)) {
            line_tx(v);
            m.r[0x1E] &= (uint8_t)~R1E_TDBE;
            m.tx_fill += 1.f;
            if (m.r[0x04] & R04_FIFOEN) { status(0x0D, R0D_TXFNF, m.tx_fill <= 15.f); status(0x01, R01_TXHF, m.tx_fill >= 8.f); }
            else { status(0x0D, R0D_TXFNF, 0); status(0x01, R01_TXHF, 1); }
        }
        break;
    case ABCODE:                                         /* a firmware module download to the DSP */
        if (v == 0x4F || v == 0x5F) { m.saved_1b = m.r[0x1B]; m.r[0x1B] = v; m.download = 1; m.download_crc = 0; }
        else if (v == 0 && m.download) { m.r[SECTXB] = 0xFF; m.r[SECRXB] = m.download_crc; m.r[0x1B] = m.saved_1b; m.download = 0; }
        break;
    case 0x15: handshake_if_ready(); break;
    case MEDAM: m.word_write = 1; break;
    case 0x1D:
        if ((m.r[0x1D] & R1D_MEMW) && !(old & R1D_MEMW)) m.word_write = 0;
        if (m.r[0x1D] & R1D_MEACC) {                    /* DSP RAM access */
            unsigned a = (m.r[0x1C] | (m.r[0x1D] & 0x0F) << 8) & 0xFFF, d = m.r[MEDAL] | m.r[MEDAM] << 8;
            m.r[0x1D] &= (uint8_t)~R1D_MEACC;
            m.r[0x1F] |= R1F_NEWS;
            if (m.r[0x1D] & R1D_MEMW) {
                m.dsp[a] = (uint8_t)d;
                if (m.word_write) m.dsp[(a + 1) & 0xFFF] = (uint8_t)(d >> 8);
                if ((a == 0x26B || a == 0x26C) && (d & 0xFF)) m.dsp[0x26F] = 1;
            } else { m.r[MEDAL] = m.dsp[a]; m.r[MEDAM] = m.dsp[(a + 1) & 0xFFF]; }
        }
        break;
    case 0x1F:
        if (!(m.r[0x1F] & R1F_NCIE)) m.r[0x1F] &= (uint8_t)~R1F_NCIA;
        if (m.r[0x1F] & R1F_NEWC) {                     /* new configuration */
            if (m.r[0x1A] & R1A_SFRES) {                 /* soft reset */
                m.r[0x1A] &= (uint8_t)~R1A_SFRES;
                chip_reset(0); chip_reset(1);
                m.r[0x1F] |= R1F_NEWC; m.r[0x20] = 0;
            } else {
                m.r[0x1F] &= (uint8_t)~R1F_NEWC;
                if (m.r[0x1F] & R1F_NCIE) m.r[0x1F] |= R1F_NCIA;
            }
        }
        if (!(old & R1F_NEWS)) m.r[0x1F] &= (uint8_t)~R1F_NEWS;   /* the host can only clear NEWS */
        if (!(m.r[0x1F] & R1F_NEWS)) m.r[0x1F] &= (uint8_t)~R1F_NSIA;
        break;
    }
    update_irq();
}

uint32_t dc_modem_read(uint32_t a, int size) {
    unsigned reg = (a & 0x7FF) >> 2;
    (void)size;
    if (reg < 0x100) return reg & 1 ? 0x10u : (uint32_t)dc_modem_country;
    reg -= 0x100;
    if (reg > 0x20) return 0;
    switch (m.chip) {
    case CHIP_NORMAL: {
        int up = m.line == LINE_UP, idle = m.line == LINE_IDLE;
        status(0x0F, R0F_CTS, (m.r[0x08] & R08_RTS) && up);
        status(0x0B, 0x80, idle); status(0x0B, 0x40, idle); status(0x0B, 0x20, idle);   /* dial tone */
        if ((m.r[0x04] & R04_FIFOEN) || m.download) { status(0x0D, R0D_TXFNF, m.tx_fill <= 15.f); status(0x01, R01_TXHF, m.tx_fill >= 8.f); }
        else { status(0x0D, R0D_TXFNF, m.tx_fill == 0); status(0x01, R01_TXHF, m.tx_fill >= 1.f); }
        uint8_t v = m.r[reg];
        if (reg == 0 && m.rx_n) {                        /* RBUFFER: the next byte moves up */
            memmove(m.rxq, m.rxq + 1, (size_t)--m.rx_n);
            rx_status(); update_irq();
        }
        return v;
    }
    case CHIP_TEST_DSP: case CHIP_TEST_END: return reg == TBUFFER ? 0 : m.r[reg];
    default: return 0;
    }
}

void dc_modem_write(uint32_t a, uint32_t v, int size) {
    unsigned reg = (a & 0x7FF) >> 2;
    (void)size;
    if (reg < 0x100) return;
    reg -= 0x100;
    if (dc_modem_trace && reg != TBUFFER) TR("modem: reg %02X = %02X\n", reg, v & 0xFF);
    if (reg == 0x20) chip_reset(v & 0xFF);
    else if (reg < 0x20 && m.chip == CHIP_NORMAL) write_reg((int)reg, (uint8_t)v);
}

void dc_modem_reset(void) { memset(&m, 0, sizeof m); }
void dc_modem_state(void (*f)(void *p, size_t n)) { f(&m, sizeof m); }

/* ---------------------------------------------------------------- V.42 LAPM (not yet: plain V.14) */
void dc_modem_lapm_reset(void) { m.sync_phase = SYNC_V14; fprintf(stderr, "modem: V.42 LAPM is not emulated yet\n"); }
int dc_modem_lapm_write(uint8_t v) { (void)v; return 0; }
int dc_modem_lapm_read(void) { return 0xFF; }

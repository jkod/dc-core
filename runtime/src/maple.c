/* dc-core: Maple bus devices -- the standard Dreamcast controller, the VMU (memory card in the
 * controller's first slot, saved to a 128 KB file in the usual .bin dump layout) and the Jump Pack
 * (Puru Puru Pack: rumble, in the second slot; dc_rumble_level turns its effects into a level a
 * frontend hands to the host's gamepad).
 *
 * Maple DMA (hw.c) hands every command frame to on_maple(). Frame word: command (bits 0-7),
 * recipient (8-15: port << 6 | 0x20 main unit, low bits = sub-units), sender (16-23), length in
 * words (24-31). The reply goes to the receive address with the same layout.
 */
#include "dccore/maple.h"
#include "dccore/hw.h"
#include "dccore/runtime.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

dc_pad_state dc_pad[4] = { { .connected = 1, .jx = 128, .jy = 128 },
                           { .jx = 128, .jy = 128 }, { .jx = 128, .jy = 128 }, { .jx = 128, .jy = 128 } };

#define FUNC_VIBRATION  0x00010000u
#define FUNC_CONTROLLER 0x01000000u
#define FUNC_STORAGE    0x02000000u
#define FUNC_LCD        0x04000000u
#define FUNC_CLOCK      0x08000000u
enum { CMD_DEVINFO = 1, CMD_ALLINFO = 2, CMD_RESET = 3, CMD_SHUTDOWN = 4,
       RE_DEVINFO = 5, RE_ALLINFO = 6, RE_ACK = 7, RE_DATA = 8, CMD_GETCOND = 9,
       CMD_GETMINFO = 10, CMD_BREAD = 11, CMD_BWRITE = 12, CMD_BSYNC = 13, CMD_SETCOND = 14,
       RE_UNKNOWN = 0xFE, RE_BADFUNC = 0xFD, RE_FILEERR = 0xFB };

/* ------------------------------------------------------------------ VMU */
#define VMU_SIZE (256 * 512)
static struct { uint8_t *data; char path[1024]; } vmu[4];

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint8_t bcd(int v) { return (uint8_t)((v / 10) << 4 | v % 10); }

void dc_vmu_format(uint8_t *d) {
    memset(d, 0, VMU_SIZE);
    uint8_t *root = d + 255 * 512, *fat = d + 254 * 512;
    memset(root, 0x55, 16);                                   /* "formatted" */
    time_t t = time(NULL); struct tm *tm = localtime(&t);
    if (tm) {
        int y = tm->tm_year + 1900;
        root[0x30] = bcd(y / 100); root[0x31] = bcd(y % 100); root[0x32] = bcd(tm->tm_mon + 1);
        root[0x33] = bcd(tm->tm_mday); root[0x34] = bcd(tm->tm_hour); root[0x35] = bcd(tm->tm_min);
        root[0x36] = bcd(tm->tm_sec); root[0x37] = bcd((tm->tm_wday + 6) % 7);
    }
    put16(root + 0x40, 255); put16(root + 0x42, 0); put16(root + 0x44, 255);
    put16(root + 0x46, 254); put16(root + 0x48, 1);           /* FAT: block 254, 1 block */
    put16(root + 0x4A, 253); put16(root + 0x4C, 13);          /* directory: 253 down to 241 */
    put16(root + 0x4E, 0);                                    /* icon */
    put16(root + 0x50, 200); put16(root + 0x52, 31);          /* 200 user blocks */
    for (int b = 0; b < 256; b++) put16(fat + b * 2, 0xFFFC); /* free */
    for (int b = 242; b <= 253; b++) put16(fat + b * 2, (uint16_t)(b - 1));
    put16(fat + 241 * 2, 0xFFFA); put16(fat + 254 * 2, 0xFFFA); put16(fat + 255 * 2, 0xFFFA);
}

int dc_vmu_attach(int port, const char *path) {
    if (port < 0 || port > 3) return -1;
    if (!path) { free(vmu[port].data); vmu[port].data = NULL; return 0; }
    uint8_t *d = malloc(VMU_SIZE);
    if (!d) return -1;
    FILE *f = fopen(path, "rb");
    int ok = f && fread(d, 1, VMU_SIZE, f) == VMU_SIZE;
    if (f) fclose(f);
    if (!ok) {
        if (f) { fprintf(stderr, "dc-core: %s is not a 128 KB VMU image\n", path); free(d); return -1; }
        dc_vmu_format(d);                                      /* new card */
        if (!(f = fopen(path, "wb"))) { free(d); return -1; }
        fwrite(d, 1, VMU_SIZE, f); fclose(f);
    }
    free(vmu[port].data);
    vmu[port].data = d;
    snprintf(vmu[port].path, sizeof vmu[port].path, "%s", path);
    return 0;
}
const uint8_t *dc_vmu_data(int port) { return port >= 0 && port < 4 ? vmu[port].data : NULL; }

static void vmu_save(int port, uint32_t ofs, uint32_t len) {
    FILE *f = fopen(vmu[port].path, "r+b");
    if (!f) f = fopen(vmu[port].path, "wb");
    if (!f) { fprintf(stderr, "dc-core: cannot write %s\n", vmu[port].path); return; }
    if (fseek(f, (long)ofs, SEEK_SET) == 0) fwrite(vmu[port].data + ofs, 1, len, f);
    fclose(f);
}

static void reply(uint32_t recv, int cmd, uint32_t to, uint32_t from, const uint32_t *w, int n);
static void put_str(uint8_t *d, const char *s, int n);

/* location word: bytes partition, phase, block (big-endian 16) */
static int vmu_cmd(int port, int cmd, uint32_t send, int words, uint32_t recv, uint32_t src) {
    uint32_t me = (uint32_t)port << 6 | 0x01;
    uint8_t *d = vmu[port].data;
    uint32_t func = words > 0 ? dc_rd32(send) : 0;
    switch (cmd) {
    case CMD_DEVINFO: case CMD_ALLINFO: {
        uint8_t b[192];
        memset(b, 0, sizeof b);
        uint32_t f[4] = { FUNC_STORAGE | FUNC_LCD | FUNC_CLOCK, 0x403F7E7Eu, 0x00100500u, 0x00410F00u };   /* clock, LCD, storage (1 read / write per block) */
        memcpy(b, f, 16);
        b[16] = 0xFF; b[17] = 0;
        put_str(b + 18, "Visual Memory", 30);
        put_str(b + 48, "Produced By or Under License From SEGA ENTERPRISES,LTD.", 60);
        b[108] = 0x7C; b[110] = 0x82;                         /* 12.4 / 13.0 mA */
        int n = cmd == CMD_DEVINFO ? 28 : 48;
        if (cmd == CMD_ALLINFO) put_str(b + 112, "Version 1.005,1999/04/15,315-6208-03,SEGA Visual Memory System BIOS", 80);
        uint32_t w[48]; memcpy(w, b, (size_t)n * 4);
        reply(recv, cmd == CMD_DEVINFO ? RE_DEVINFO : RE_ALLINFO, src, me, w, n);
        return 1;
    }
    case CMD_GETMINFO: {
        if (func == FUNC_LCD) {                               /* 48x32 dots - 1, one colour, no contrast levels */
            uint32_t w[2] = { FUNC_LCD, 47u | 31u << 8 | 0x10u << 16 | 2u << 24 };
            reply(recv, RE_DATA, src, me, w, 2);
            return 1;
        }
        if (func != FUNC_STORAGE) { reply(recv, RE_BADFUNC, src, me, NULL, 0); return 1; }
        uint32_t w[7] = { FUNC_STORAGE, 255u | 0u << 16, 255u | 254u << 16, 1u | 253u << 16, 13u | 0u << 16, 200u | 31u << 16, 0 };
        reply(recv, RE_DATA, src, me, w, 7);
        return 1;
    }
    case CMD_BREAD: {
        uint32_t loc = dc_rd32(send + 4), block = (loc >> 8 & 0xFF00) | loc >> 24;
        if (func != FUNC_STORAGE || block > 255) { reply(recv, func == FUNC_STORAGE ? RE_FILEERR : RE_BADFUNC, src, me, NULL, 0); return 1; }
        uint32_t w[2 + 128];
        w[0] = FUNC_STORAGE; w[1] = loc;
        memcpy(w + 2, d + block * 512, 512);
        reply(recv, RE_DATA, src, me, w, 130);
        return 1;
    }
    case CMD_BWRITE: {
        if (func == FUNC_LCD || func == FUNC_CLOCK) { reply(recv, RE_ACK, src, me, NULL, 0); return 1; }   /* screen / clock: ignored */
        uint32_t loc = dc_rd32(send + 4), block = (loc >> 8 & 0xFF00) | loc >> 24, phase = loc >> 8 & 0xFF;
        uint32_t len = words > 2 ? (uint32_t)(words - 2) * 4 : 0, ofs = block * 512 + phase * 128;
        if (func != FUNC_STORAGE || block > 255 || ofs + len > VMU_SIZE) { reply(recv, func == FUNC_STORAGE ? RE_FILEERR : RE_BADFUNC, src, me, NULL, 0); return 1; }
        for (uint32_t i = 0; i < len / 4; i++) { uint32_t v = dc_rd32(send + 8 + i * 4); memcpy(d + ofs + i * 4, &v, 4); }
        vmu_save(port, ofs, len);
        reply(recv, RE_ACK, src, me, NULL, 0);
        return 1;
    }
    case CMD_GETCOND:
        if (func == FUNC_CLOCK) {
            time_t t = time(NULL); struct tm *tm = localtime(&t);
            int y = tm ? tm->tm_year + 1900 : 2000;
            uint32_t w[3] = { FUNC_CLOCK,
                (uint32_t)(y >> 8) | (uint32_t)(y & 255) << 8 | (uint32_t)(tm ? tm->tm_mon + 1 : 1) << 16 | (uint32_t)(tm ? tm->tm_mday : 1) << 24,
                (uint32_t)(tm ? tm->tm_hour : 0) | (uint32_t)(tm ? tm->tm_min : 0) << 8 | (uint32_t)(tm ? tm->tm_sec : 0) << 16 | (uint32_t)(tm ? (tm->tm_wday + 6) % 7 : 0) << 24 };
            reply(recv, RE_DATA, src, me, w, 3);
        } else reply(recv, RE_BADFUNC, src, me, NULL, 0);
        return 1;
    case CMD_BSYNC: case CMD_SETCOND: case CMD_RESET: case CMD_SHUTDOWN:
        reply(recv, RE_ACK, src, me, NULL, 0);
        return 1;
    default:
        reply(recv, RE_UNKNOWN, src, me, NULL, 0);
        return 1;
    }
}

/* ------------------------------------------------------------------ Jump Pack (vibration)
 * A condition word (SET_CONDITION, function 0x100 of the bus's function bits), byte by byte from
 * the low end: mode (bits 4-7 the vibration source, 1 = the motor; bit 0 continuous), power
 * (bits 4-6 the "+" power, bits 0-2 the "-" power, 1..7; bit 3 set with a + power: rising to the
 * maximum; bit 7 with a - power: falling to nothing; no power at all stops), pulse frequency
 * (7..59: (value + 1) / 2 Hz, 4-30 Hz), and cycles (with a rise or fall, it lasts value + 1
 * pulses). Continuous vibration runs until the game stops it (here at most 5 s, so a lost stop
 * can't leave a pad shaking). Sources: the DreamcastWiki jump pack page, KallistiOS purupuru.h. */
static struct {
    int attached, active;
    float start, end, hz;      /* level 0..1 from start to end over `ramp` fields, kept until `total` */
    uint32_t ramp, total, done, cond, ast;
} rb[4];

int dc_rumble_attach(int port, int on) {
    if (port < 0 || port > 3) return -1;
    memset(&rb[port], 0, sizeof rb[port]);
    rb[port].attached = on != 0;
    return 0;
}

static uint32_t fields(float secs) { return secs * 60.0f < 1.0f ? 1u : (uint32_t)(secs * 60.0f + 0.5f); }

static void rumble_set(int port, uint32_t w) {
    int src = (int)(w >> 4 & 15), cont = (int)(w & 1);
    int up = (int)(w >> 12 & 7), down = (int)(w >> 8 & 7), rise = (int)(w >> 11 & 1), fall = (int)(w >> 15 & 1);
    uint32_t freq = w >> 16 & 255, cycles = w >> 24;
    rb[port].cond = w;
    if (!src || (!up && !down)) { rb[port].active = 0; return; }      /* stop (or no motor named) */
    float hz = (freq >= 7 && freq <= 59) ? (float)(freq + 1) / 2.0f : 15.0f;
    float p = (float)(up ? up : down) / 7.0f;
    rb[port].hz = hz; rb[port].start = p; rb[port].done = 0;
    rb[port].end = (rise && up) ? 1.0f : (fall && down) ? 0.0f : p;
    rb[port].ramp = fields((float)(cycles + 1) / hz);                  /* one pulse when steady */
    rb[port].total = rb[port].ramp;
    if (cont && rb[port].end > 0 && rb[port].total < 300) rb[port].total = 300;   /* continuous: until stopped, 5 s at most */
    rb[port].active = 1;
}

int dc_rumble_level(int port, float *low, float *high) {
    float l = 0, k = 0;
    if (port >= 0 && port < 4 && rb[port].active) {
        float t = (float)rb[port].done / (float)rb[port].ramp;
        if (t > 1) t = 1;
        l = rb[port].start + (rb[port].end - rb[port].start) * t;
        if (++rb[port].done >= rb[port].total) rb[port].active = 0;
        /* a slow pulse is the big (low-frequency) motor's job; the faster it pulses, the more of the small one */
        k = (rb[port].hz - 4.0f) / 26.0f;
        if (k < 0) k = 0;
        if (k > 1) k = 1;
    }
    if (low) *low = l;
    if (high) *high = l * k;
    return l > 0;
}

static int rumble_cmd(int port, int cmd, uint32_t send, int words, uint32_t recv, uint32_t src) {
    uint32_t me = (uint32_t)port << 6 | 0x02;
    uint32_t func = words > 0 ? dc_rd32(send) : 0;
    switch (cmd) {
    case CMD_DEVINFO: case CMD_ALLINFO: {
        uint8_t b[192];
        memset(b, 0, sizeof b);
        uint32_t f[4] = { FUNC_VIBRATION, 0x00000101u, 0, 0 };   /* 1 vibration source, 1 at a time */
        memcpy(b, f, 16);
        b[16] = 0xFF; b[17] = 0;
        put_str(b + 18, "Puru Puru Pack", 30);
        put_str(b + 48, "Produced By or Under License From SEGA ENTERPRISES,LTD.", 60);
        b[108] = 0xC8; b[110] = 0x40; b[111] = 0x06;               /* 20.0 / 160.0 mA */
        int n = cmd == CMD_DEVINFO ? 28 : 48;
        if (cmd == CMD_ALLINFO) put_str(b + 112, "Version 1.000,1998/11/10,315-6211-AH   ,Vibration Motor:1 , Fm:4 - 30Hz ,Pow:7", 80);
        uint32_t w[48]; memcpy(w, b, (size_t)n * 4);
        reply(recv, cmd == CMD_DEVINFO ? RE_DEVINFO : RE_ALLINFO, src, me, w, n);
        return 1;
    }
    case CMD_SETCOND:
        if (func != FUNC_VIBRATION) { reply(recv, RE_BADFUNC, src, me, NULL, 0); return 1; }
        if (words > 1) rumble_set(port, dc_rd32(send + 4));
        reply(recv, RE_ACK, src, me, NULL, 0);
        return 1;
    case CMD_GETCOND: case CMD_GETMINFO: {
        if (func != FUNC_VIBRATION) { reply(recv, RE_BADFUNC, src, me, NULL, 0); return 1; }
        uint32_t w[2] = { FUNC_VIBRATION, cmd == CMD_GETCOND ? rb[port].cond : 0x00000101u };
        reply(recv, RE_DATA, src, me, w, 2);
        return 1;
    }
    case CMD_BREAD: {          /* the auto-stop setting: kept and read back */
        uint32_t w[3] = { FUNC_VIBRATION, words > 1 ? dc_rd32(send + 4) : 0, rb[port].ast };
        reply(recv, func == FUNC_VIBRATION ? RE_DATA : RE_BADFUNC, src, me, w, func == FUNC_VIBRATION ? 3 : 0);
        return 1;
    }
    case CMD_BWRITE:
        if (func == FUNC_VIBRATION && words > 2) rb[port].ast = dc_rd32(send + 8);
        reply(recv, func == FUNC_VIBRATION ? RE_ACK : RE_BADFUNC, src, me, NULL, 0);
        return 1;
    case CMD_RESET: case CMD_SHUTDOWN:
        rb[port].active = 0;
        reply(recv, RE_ACK, src, me, NULL, 0);
        return 1;
    default:
        reply(recv, RE_UNKNOWN, src, me, NULL, 0);
        return 1;
    }
}

static void put_str(uint8_t *d, const char *s, int n) {
    int l = (int)strlen(s);
    for (int i = 0; i < n; i++) d[i] = i < l ? (uint8_t)s[i] : ' ';
}

/* reply frame header + payload words */
static void reply(uint32_t recv, int cmd, uint32_t to, uint32_t from, const uint32_t *w, int n) {
    dc_wr32(recv, (uint32_t)cmd | to << 8 | from << 16 | (uint32_t)n << 24);
    for (int i = 0; i < n; i++) dc_wr32(recv + 4 + i * 4, w[i]);
}

static int on_maple(int port, uint32_t frame, uint32_t send, int words, uint32_t recv) {
    int cmd = frame & 255;
    uint32_t dest = (frame >> 8) & 255, src = (frame >> 16) & 255;
    static int trace = -1;                                      /* DC_MAPLE_TRACE=1: commands (polls only when they change) */
    if (trace < 0) trace = getenv("DC_MAPLE_TRACE") != NULL;
    if (trace && (cmd != CMD_GETCOND || (dest & 0x1F)))
        fprintf(stderr, "maple: port %d cmd %d to %02X from %02X words %d func %08X loc %08X\n", port, cmd, dest, src, words,
                words ? dc_rd32(send) : 0, words > 1 ? dc_rd32(send + 4) : 0);
    if (port < 0 || port > 3 || !dc_pad[port].connected) return 0;
    if ((dest & 0x3F) == 0x01) return vmu[port].data ? vmu_cmd(port, cmd, send, words, recv, src) : 0;
    if ((dest & 0x3F) == 0x02) return rb[port].attached ? rumble_cmd(port, cmd, send, words, recv, src) : 0;
    if ((dest & 0x3F) != 0x20) return 0;                       /* other sub-units: none */
    uint32_t me = (uint32_t)port << 6 | 0x20 | (vmu[port].data ? 0x01 : 0) | (rb[port].attached ? 0x02 : 0);   /* + attached sub-units */
    switch (cmd) {
    case CMD_DEVINFO: case CMD_ALLINFO: {
        uint8_t b[112 + 80];
        memset(b, 0, sizeof b);
        uint32_t f[4] = { FUNC_CONTROLLER, 0xFE060F00u, 0, 0 };   /* function, buttons/axes present */
        memcpy(b, f, 16);
        b[16] = 0xFF;                                             /* area: all regions */
        b[17] = 0;                                                /* connector direction */
        put_str(b + 18, "Dreamcast Controller", 30);
        put_str(b + 48, "Produced By or Under License From SEGA ENTERPRISES,LTD.", 60);
        b[108] = 0xAE; b[109] = 0x01;                              /* standby 43.0 mA */
        b[110] = 0xF4; b[111] = 0x01;                              /* max 50.0 mA */
        int n = cmd == CMD_DEVINFO ? 28 : 48;
        if (cmd == CMD_ALLINFO) put_str(b + 112, "Version 1.010,1998/09/28,315-6211-AB   ,Analog Module : The 4th Edition.5/8  +DF", 80);
        uint32_t w[48];
        memcpy(w, b, (size_t)n * 4);
        reply(recv, cmd == CMD_DEVINFO ? RE_DEVINFO : RE_ALLINFO, src, me, w, n);
        return 1;
    }
    case CMD_GETCOND: {
        uint32_t func = dc_rd32(send);
        if (func != FUNC_CONTROLLER) { reply(recv, RE_BADFUNC, src, me, NULL, 0); return 1; }
        const dc_pad_state *p = &dc_pad[port];
        uint16_t btn = (uint16_t)~p->buttons;                    /* active low */
        uint32_t w[3];
        w[0] = FUNC_CONTROLLER;
        w[1] = (uint32_t)btn | (uint32_t)p->rt << 16 | (uint32_t)p->lt << 24;
        w[2] = (uint32_t)p->jx | (uint32_t)p->jy << 8 | 0x80u << 16 | 0x80u << 24;   /* second stick centred */
        reply(recv, RE_DATA, src, me, w, 3);
        if (trace) { static uint32_t last; if (w[1] != last) { fprintf(stderr, "maple: pad %d %08X %08X\n", port, w[1], w[2]); last = w[1]; } }
        return 1;
    }
    case CMD_RESET: case CMD_SHUTDOWN:
        reply(recv, RE_ACK, src, me, NULL, 0);
        return 1;
    default:
        reply(recv, RE_UNKNOWN, src, me, NULL, 0);
        return 1;
    }
}

void dc_maple_install(void) { dc_hw.on_maple = on_maple; }

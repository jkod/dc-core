/* dc-core: high-level replacements for the Dreamcast boot ROM services.
 *
 * Games reach the BIOS through vectors in low RAM (0x8C0000B0.. 0x8C0000E0). dc_bios_install()
 * points those vectors at addresses that dispatch to native code and fills the RAM areas the
 * boot ROM normally leaves behind (IP.BIN at 0x8C008000, machine ID, ...).
 */
#include "dccore/hw.h"
#include "dccore/runtime.h"
#include "dccore/disc.h"
#include "dccore/bios.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define V_SYSINFO 0x8C0000B0u
#define V_ROMFONT 0x8C0000B4u
#define V_FLASH   0x8C0000B8u
#define V_GDROM   0x8C0000BCu
#define V_GDROM2  0x8C0000C0u
#define V_MISC    0x8C0000E0u
#define HLE_BASE  0x8C00E000u   /* fake entry points: never executed as code */
#define GD_ENTRY  0x8C0010F0u   /* where the real boot ROM puts the GD-ROM syscall: some games
                                   (Rush 2049) call it directly as a disc check */

int dc_bios_trace;
void (*dc_bios_read_hook)(uint32_t lba, uint32_t count, uint32_t dst);
#define TR(...) do { if (dc_bios_trace) fprintf(stderr, __VA_ARGS__); } while (0)

/* ---------------------------------------------------------------- flash */
uint8_t dc_flash[0x20000];          /* shared with hw.c (readable at 0x00200000) */
#define flash dc_flash
uint8_t *dc_bios_rom;                /* 2 MB boot ROM if the player supplied one (0x00000000) */
static int flash_loaded;

static int load_file(const char *path, uint8_t *dst, size_t n) {
    FILE *f = fopen(path, "rb"); if (!f) return -1;
    size_t r = fread(dst, 1, n, f); fclose(f);
    return r == n ? 0 : -1;
}
int dc_bios_load(const char *boot, const char *flashp) {
    int ok = 0;
    if (boot) {
        if (!dc_bios_rom) dc_bios_rom = (uint8_t *)malloc(0x200000);
        if (load_file(boot, dc_bios_rom, 0x200000)) { free(dc_bios_rom); dc_bios_rom = NULL; ok = -1; }
    }
    if (flashp) { if (load_file(flashp, flash, sizeof flash)) ok = -1; else flash_loaded = 1; }
    return ok;
}

/* User partition (0x1C000, 256 blocks of 64 bytes) as a console leaves it once its clock has been
 * set: block 0 the "KATANA_FLASH____" header, the last block the allocation bitmap (bit 7 of byte 0
 * = block 1; a cleared bit = in use). A data block is a 16-bit ID, data, and a CRC-16 (CCITT,
 * inverted) of its first 62 bytes. The one block here is the system settings (ID 5): clock,
 * language, stereo, auto-start. Bomberman Online's flash manager waits for it on a black screen. */
static uint16_t flash_crc(const uint8_t *p, int n) {
    uint32_t crc = 0xFFFF;
    for (int i = 0; i < n; i++) {
        crc ^= (uint32_t)p[i] << 8;
        for (int b = 0; b < 8; b++) crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
    }
    return (uint16_t)~crc;
}
static void flash_user_init(void) {
    uint8_t *u = flash + 0x1C000, *b = u + 0x40;
    memset(u, 0xFF, 0x4000);
    memcpy(u, "KATANA_FLASH____", 16);
    u[16] = 2; u[17] = 0;                                 /* partition 2 */
    uint32_t t = 2240611200u;                             /* 2021-01-01 00:00, in seconds since 1950 */
    b[0] = 0x05; b[1] = 0x00;                             /* block ID: system settings */
    b[2] = t & 0xFF; b[3] = (t >> 8) & 0xFF; b[4] = (t >> 16) & 0xFF; b[5] = t >> 24;
    b[7] = 1;                                             /* language: English */
    b[8] = 0;                                             /* stereo */
    b[9] = 1;                                             /* auto-start discs */
    uint16_t crc = flash_crc(b, 62);
    b[62] = crc & 0xFF; b[63] = crc >> 8;
    u[0x4000 - 0x40] = 0x7F;                              /* bitmap: block 1 in use */
}

static void flash_init(char region) {
    memset(flash, 0xFF, sizeof flash);
    /* factory partition 0x1A000 (mirrored at 0x1A0A0): "00" region language broadcast, then
     * "Dreamcast  "; region/broadcast are set by set_region() */
    for (int m = 0; m < 2; m++) {
        uint8_t *p = flash + 0x1A000 + m * 0xA0;
        memcpy(p, "00011Dreamcast  ", 16);
    }
    (void)region;
    memset(flash + 0x1A056, 0x00, 8);  /* machine id */
    flash_user_init();
}

static const uint32_t part_ofs[5] = {0x1A000, 0x18000, 0x1C000, 0x10000, 0x00000};
static const uint32_t part_len[5] = {0x02000, 0x02000, 0x04000, 0x08000, 0x10000};

/* ---------------------------------------------------------------- GD-ROM */
#define MAX_REQ 16
static struct { int cmd; uint32_t p[4]; int status; } req[MAX_REQ];
static uint32_t next_req = 1;
static uint32_t last_lba;
/* DMA reads raise the GD-DMA end interrupt (ISTNRM bit 14). Off by default: it is raised at once,
 * which Tennis 2K2's file system needs and Rush 2049's track loader does not survive (it stops
 * drawing for good after the car select). */
int dc_gd_dma_irq;
/* get_cmd_stat as on the console: a finished request reports COMPLETED (2) once, after that its
 * slot is free and reads NO_ACTIVE (0). Off by default (always 2); Dolphin Blue's GD init waits
 * for 0. Not part of save states (a restored game only ever polls requests it makes afterwards). */
int dc_gd_stat_free;
static uint8_t req_reported[MAX_REQ];

/* Stream reads (DMAREAD_STREAM 28, PIOREAD_STREAM 37 and their _EX forms 38/39): the command only
 * opens the stream (p[0] FAD, p[1] sectors); the game then pulls it in pieces of any size with
 * REQ_DMA_TRANS / REQ_PIO_TRANS (dst, bytes) and polls CHECK_*_TRANS: 1 while that piece is still
 * moving, 0 once it arrived, with the stream's bytes left in *size. get_cmd_stat says 3 (STREAMING)
 * until all were taken; READ_ABORT ends it early. Pieces are copied at once (no transfer time). */
static struct { uint32_t id, lba, total, done; } stream;
static int is_stream_cmd(int cmd) { return cmd == 28 || cmd == 37 || cmd == 38 || cmd == 39; }
static int stream_copy(uint32_t dst, uint32_t n) {
    if (!stream.id || !DC_IS_RAM(dst) || DC_RAM_OFS(dst) + n > DC_RAM_SIZE) return -1;
    if (n > stream.total - stream.done) n = stream.total - stream.done;
    static uint8_t sec[2048];
    while (n) {
        uint32_t s0 = stream.done / 2048, o = stream.done % 2048, k = 2048 - o < n ? 2048 - o : n;
        if (dc_disc_read(stream.lba + s0, 1, sec)) return -1;
        memcpy(dc_ram + DC_RAM_OFS(dst), sec + o, k);
        dst += k; n -= k; stream.done += k;
    }
    last_lba = stream.lba + (stream.done + 2047) / 2048;
    return 0;
}

/* ---- CD audio: image LBAs [cur, end), `repeat` more times (15 = forever) from `start` */
static struct { int playing, paused; uint32_t start, cur, end; int repeat; uint8_t buf[2352]; int pos; } cdda;
static void cdda_play(uint32_t start, uint32_t end, int repeat) {
    cdda.start = cdda.cur = start; cdda.end = end; cdda.repeat = repeat & 15;
    cdda.playing = end > start; cdda.paused = 0; cdda.pos = 588;
    TR("bios: cdda play lba %u..%u repeat %d\n", start, end, cdda.repeat);
}
/* for ports that drive CD audio themselves (Windows CE's CD-ROM device) */
void dc_cdda_play(uint32_t start, uint32_t end, int repeat) { cdda_play(start, end, repeat); }
void dc_cdda_pause(int on) { cdda.paused = on; }
void dc_cdda_stop(void) { cdda.playing = 0; }
int  dc_cdda_status(uint32_t *lba) { if (lba) *lba = cdda.cur; return cdda.playing ? (cdda.paused ? 2 : 1) : 0; }
int dc_cdda_frame(int16_t *l, int16_t *r) {
    if (!cdda.playing || cdda.paused) return 0;
    if (cdda.pos >= 588) {
        if (cdda.cur >= cdda.end) {
            if (!cdda.repeat) { cdda.playing = 0; return 0; }
            if (cdda.repeat != 15) cdda.repeat--;
            cdda.cur = cdda.start;
        }
        if (dc_disc_read_raw(cdda.cur, 1, cdda.buf)) { cdda.playing = 0; return 0; }
        cdda.cur++; cdda.pos = 0;
    }
    const uint8_t *b = cdda.buf + cdda.pos++ * 4;
    *l = (int16_t)(b[0] | b[1] << 8); *r = (int16_t)(b[2] | b[3] << 8);
    return 1;
}
/* track number (1-based) -> image LBA of its start / end */
static int track_span(uint32_t t, uint32_t *start, uint32_t *end) {
    uint32_t lba, secs; int data;
    if (t < 1 || dc_disc_track_info((int)t - 1, &lba, &data, &secs)) return -1;
    if (start) *start = lba;
    if (end) *end = lba + secs;
    return 0;
}

static uint32_t toc_entry(uint32_t lba, int is_data) { return ((is_data ? 4u : 0u) << 28) | (1u << 24) | (lba + 150); }

static int gd_exec(int cmd, uint32_t *p) {
    switch (cmd) {
    case 34:                                 /* GETSCD(format, size, buffer): CD audio status (Q subcode) */
        if (p[0] <= 2) {
            uint32_t buf = p[2], n = p[1] ? p[1] : 14, fad = (cdda.playing ? cdda.cur : last_lba) + 150, rel = fad;
            int track = 1;
            for (int i = 0; i < dc_disc_track_count(); i++) {
                uint32_t lba; dc_disc_track_info(i, &lba, NULL, NULL);
                if (lba + 150 <= fad) { track = i + 1; rel = fad - (lba + 150); }
            }
            uint8_t b[14] = { 0, (uint8_t)(cdda.playing ? (cdda.paused ? 0x12 : 0x11) : 0x15), 0, 14,
                              0x01, (uint8_t)track, 1, (uint8_t)(rel >> 16), (uint8_t)(rel >> 8), (uint8_t)rel, 0,
                              (uint8_t)(fad >> 16), (uint8_t)(fad >> 8), (uint8_t)fad };
            if (DC_IS_RAM(buf)) for (uint32_t i = 0; i < n && i < 14; i++) dc_wr8(buf + i, b[i]);
            return 0;
        }
        /* fall through - a read on discs that use 34 that way */
    case 16: case 17: case 32: {             /* PIOREAD / DMAREAD (+ stream variants) */
        uint32_t lba = p[0] - 150, n = p[1], dst = p[2];
        TR("bios: gd read lba %u x%u -> %08X\n", lba, n, dst);
        if (dc_bios_read_hook) dc_bios_read_hook(lba, n, dst);
        if (!DC_IS_RAM(dst)) return -1;
        if (DC_RAM_OFS(dst) + n * 2048 > DC_RAM_SIZE) return -1;
        last_lba = lba + n;
        if (dc_disc_read(lba, n, dc_ram + DC_RAM_OFS(dst))) return -1;
        if (dc_gd_dma_irq && (cmd == 17 || cmd == 34)) dc_raise(1u << 14);   /* GD-DMA end interrupt (opt-in) */
        return 0;
    }
    case 18: case 19: {                      /* GETTOC / GETTOC2: p[0] session area, p[1] buffer */
        uint32_t buf = p[1], area = p[0];
        for (int i = 0; i < 102; i++) dc_wr32(buf + i * 4, 0xFFFFFFFFu);
        int first = 0, last = 0, n = dc_disc_track_count();
        for (int i = 0; i < n; i++) {
            uint32_t lba, secs; int data; dc_disc_track_info(i, &lba, &data, &secs);
            int hd = !dc_disc_is_cd() && lba >= 45000;   /* a CD has only the one area */
            if ((area == 1) != hd) continue;
            dc_wr32(buf + i * 4, toc_entry(lba, data));
            if (!first) first = i + 1;
            last = i + 1;
            dc_wr32(buf + 101 * 4, toc_entry(lba + secs, data));
        }
        dc_wr32(buf + 99 * 4, (uint32_t)first << 16);
        dc_wr32(buf + 100 * 4, (uint32_t)last << 16);
        TR("bios: gd toc area %u: tracks %d..%d\n", area, first, last);
        return 0;
    }
    case 20: {                               /* PLAY tracks p[0]..p[1], p[2] repeats */
        uint32_t a, b;
        if (track_span(p[0], &a, NULL) || track_span(p[1] ? p[1] : p[0], NULL, &b)) return 0;
        cdda_play(a, b, (int)p[2]);
        return 0;
    }
    case 21:                                 /* PLAY2: FAD p[0]..p[1] (FAD = LBA + 150), p[2] repeats */
        cdda_play(p[0] - 150, p[1] - 150, (int)p[2]);
        return 0;
    case 22: cdda.paused = 1; return 0;      /* PAUSE */
    case 23: cdda.paused = 0; return 0;      /* RELEASE: resume */
    case 27: case 24: cdda.playing = 0; return 0;   /* SEEK (stops audio), INIT */
    case 40:
        return 0;
    default:
        TR("bios: gd cmd %d (ignored)\n", cmd);
        return 0;
    }
}

/* ---------------------------------------------------------------- handlers */
static uint32_t h_sysinfo(sh4ctx *c) {
    TR("bios: sysinfo r7=%u\n", c->r[7]);
    switch (c->r[7]) {
    case 0:                                          /* INIT: machine ID + factory region bytes */
        memcpy(dc_ram + 0x68, flash + 0x1A056, 8);
        memcpy(dc_ram + 0x70, flash + 0x1A000, 5);
        c->r[0] = 0; break;
    case 2: c->r[0] = (uint32_t)-1; break;           /* icon: none */
    case 3: c->r[0] = 0x8C000068u; break;            /* pointer to machine id */
    default: c->r[0] = 0;
    }
    return 0;
}

static uint32_t h_romfont(sh4ctx *c) {
    TR("bios: romfont r1=%u\n", c->r[1]);
    switch (c->r[1]) {
    case 0: c->r[0] = dc_bios_rom ? 0xA0100020u : 0x8CFF0000u; break;  /* ROM font (blank without a BIOS) */
    default: c->r[0] = 0;
    }
    return 0;
}

static uint32_t h_flash(sh4ctx *c) {
    uint32_t f = c->r[7];
    TR("bios: flash r7=%u r4=%08X r5=%08X r6=%08X\n", f, c->r[4], c->r[5], c->r[6]);
    if (f == 0) {                                     /* info(part, out[2]) */
        uint32_t part = c->r[4];
        if (part > 4) { c->r[0] = (uint32_t)-1; return 0; }
        dc_wr32(c->r[5], part_ofs[part]); dc_wr32(c->r[5] + 4, part_len[part]); c->r[0] = 0;
    } else if (f == 1) {                              /* read(ofs, dst, len) */
        uint32_t o = c->r[4], d = c->r[5], n = c->r[6];
        for (uint32_t i = 0; i < n; i++) dc_wr8(d + i, o + i < sizeof flash ? flash[o + i] : 0xFF);
        c->r[0] = 0;
    } else if (f == 2) {                              /* write(ofs, src, len): AND, like real flash */
        uint32_t o = c->r[4], s = c->r[5], n = c->r[6];
        for (uint32_t i = 0; i < n && o + i < sizeof flash; i++) flash[o + i] &= dc_rd8(s + i);
        c->r[0] = n;
    } else if (f == 3) {                              /* delete(ofs): erase the partition */
        for (int p = 0; p < 5; p++) if (c->r[4] == part_ofs[p]) memset(flash + part_ofs[p], 0xFF, part_len[p]);
        c->r[0] = 0;
    } else c->r[0] = (uint32_t)-1;
    return 0;
}

static uint32_t h_gdrom(sh4ctx *c) {
    uint32_t f = c->r[7];
    switch (f) {
    case 0: {                                         /* req_cmd(cmd, params) -> id */
        uint32_t id = next_req++; int k = id % MAX_REQ;
        req[k].cmd = (int)c->r[4];
        for (int i = 0; i < 4; i++) req[k].p[i] = c->r[5] ? dc_rd32(c->r[5] + i * 4) : 0;
        TR("bios: gd req cmd %d (%08X %08X %08X %08X) -> %u\n", req[k].cmd, req[k].p[0], req[k].p[1], req[k].p[2], req[k].p[3], id);
        req[k].status = gd_exec(req[k].cmd, req[k].p);
        req_reported[k] = 0;
        if (is_stream_cmd(req[k].cmd)) {
            stream.id = id; stream.lba = req[k].p[0] - 150; stream.total = req[k].p[1] * 2048u; stream.done = 0;
        }
        c->r[0] = id;
        break;
    }
    case 1: {                                         /* get_cmd_stat(id, status[4]) */
        int k = c->r[4] % MAX_REQ;
        if (c->r[5]) {
            dc_wr32(c->r[5], req[k].status ? 1 : 0);  /* error code */
            int rd = req[k].cmd == 16 || req[k].cmd == 17 || req[k].cmd == 32 || (req[k].cmd == 34 && req[k].p[0] > 2);
            dc_wr32(c->r[5] + 4, 0);
            dc_wr32(c->r[5] + 8, rd && !req[k].status ? req[k].p[1] * 2048u : 0);   /* bytes transferred */
            dc_wr32(c->r[5] + 12, 0);
        }
        c->r[0] = req[k].status ? (uint32_t)-1 : 2;   /* 2 = completed */
        if (is_stream_cmd(req[k].cmd)) {
            if (stream.id == c->r[4] && stream.done < stream.total) {
                if (c->r[5]) dc_wr32(c->r[5] + 8, stream.done);
                c->r[0] = 3;                          /* streaming */
                break;
            }
            if (stream.id != c->r[4] && req_reported[k]) { c->r[0] = 0; break; }   /* aborted */
        }
        if (dc_gd_stat_free && !req[k].status && req_reported[k]++) c->r[0] = 0;   /* no active request */
        break;
    }
    case 4:                                           /* get_drv_stat(status[2]) */
        if (c->r[4]) {                                /* standby / playing / paused */
            dc_wr32(c->r[4], cdda.playing ? (cdda.paused ? 1 : 3) : 2);
            dc_wr32(c->r[4] + 4, dc_disc_is_cd() ? 0x20 : 0x80);   /* disc type: CD-ROM XA / GD-ROM */
        }
        c->r[0] = 0;
        break;
    case 10:                                          /* change_data_type(params[4]) */
        c->r[0] = 0;
        break;
    case 5:                                           /* dma_end: the game's GD-DMA end interrupt handler */
        TR("bios: gd dma_end\n");
        dc_wr32(0xA05F6900u, 1u << 14);              /* acknowledge it (ISTNRM bit 14, write 1 to clear) */
        c->r[0] = 0;
        break;
    case 6: case 11: {                                /* req_dma_trans / req_pio_trans(id, {dst, bytes}) */
        uint32_t dst = c->r[5] ? dc_rd32(c->r[5]) : 0, n = c->r[5] ? dc_rd32(c->r[5] + 4) : 0;
        TR("bios: gd stream %s %u bytes -> %08X (%u/%u)\n", f == 6 ? "dma" : "pio", n, dst, stream.done, stream.total);
        c->r[0] = stream.id == c->r[4] && !stream_copy(dst, n) ? 0 : (uint32_t)-1;
        if (f == 6 && !c->r[0] && dc_gd_dma_irq) dc_raise(1u << 14);
        break;
    }
    case 7: case 12: {                                /* check_dma_trans / check_pio_trans(id, &bytes_left) */
        uint32_t left = stream.id == c->r[4] ? stream.total - stream.done : 0;
        if (c->r[5]) dc_wr32(c->r[5], left);
        c->r[0] = 0;                                  /* the piece is done (1: still moving) */
        break;
    }
    case 8:                                           /* abort: ends a stream */
        if (stream.id) { int k = stream.id % MAX_REQ; req_reported[k] = 1; stream.id = 0; }
        c->r[0] = 0;
        break;
    case 2: case 3: case 9:                           /* exec_server, init, reset */
        c->r[0] = 0;
        break;
    default:
        TR("bios: gd func %u\n", f);
        c->r[0] = 0;
    }
    return 0;
}

static uint32_t h_misc(sh4ctx *c) {
    TR("bios: misc r4=%d\n", (int)c->r[4]);
    /* KOS uses misc(1) to return to the BIOS menu, but Katana games call it during normal
     * start-up and carry on, so it is treated as a no-op */
    c->r[0] = 0;
    return 0;
}

/* The console's region has to match the disc: games read it back through SYSINFO (PAL games
 * check for a European, 50 Hz machine). Like other emulators, a supplied flash keeps its ID and
 * settings but gets the disc's region and broadcast standard. */
static void set_region(char region) {
    char bc = region == '2' ? '1' : '0';                     /* EU: PAL, JP/US: NTSC */
    for (int m = 0; m < 2; m++) {
        flash[0x1A002 + m * 0xA0] = (uint8_t)region;
        flash[0x1A004 + m * 0xA0] = (uint8_t)bc;
    }
}

int dc_bios_install(sh4ctx *c, char region) {
    (void)c;
    if (!flash_loaded) flash_init(region);
    set_region(region);
    /* the boot ROM leaves its system code in RAM (0x8C000100..0x8C008000, copied from the start of
     * the ROM). Some games checksum it: Rush 2049's menu re-enters every frame when it doesn't
     * match. The syscalls themselves stay HLE (by address), this is only the data games see. */
    if (dc_bios_rom) memcpy(dc_ram + 0x100, dc_bios_rom + 0x100, 0x7F00);
    struct { uint32_t vec; sh4fn fn; } v[] = {
        {V_SYSINFO, h_sysinfo}, {V_ROMFONT, h_romfont}, {V_FLASH, h_flash},
        {V_GDROM, h_gdrom}, {V_GDROM2, h_gdrom}, {V_MISC, h_misc},
    };
    for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) {
        uint32_t entry = v[i].vec == V_GDROM ? GD_ENTRY : HLE_BASE + i * 0x10;
        dc_wr32(v[i].vec, entry);
        dc_hle_register(entry, v[i].fn);
    }
    /* IP.BIN (first 0x8000 bytes of the boot area) where the boot ROM leaves it */
    if (dc_disc_track_count() && dc_disc_read(dc_disc_boot_lba(), 16, dc_ram + 0x8000) == 0)
        return 0;
    return -1;
}

#include "dccore/reset.h"
void dc_bios_state(dc_state_fn f) { f(req, sizeof req); f(&next_req, sizeof next_req); f(&last_lba, sizeof last_lba); f(&cdda, sizeof cdda); }

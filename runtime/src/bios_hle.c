/* dc-core: high-level replacements for the Dreamcast boot ROM services.
 *
 * Games reach the BIOS through vectors in low RAM (0x8C0000B0.. 0x8C0000E0). dc_bios_install()
 * points those vectors at addresses that dispatch to native code and fills the RAM areas the
 * boot ROM normally leaves behind (IP.BIN at 0x8C008000, machine ID, ...).
 */
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

static void flash_init(char region) {
    memset(flash, 0xFF, sizeof flash);
    /* factory partition 0x1A000: region / broadcast / language, then "Dreamcast  " */
    uint8_t *p = flash + 0x1A000;
    p[0] = (uint8_t)region; p[1] = (uint8_t)region; p[2] = '0'; p[3] = '0'; p[4] = '0';
    memcpy(p + 5, "Dreamcast  ", 11);
    memset(flash + 0x1A056, 0x00, 8);  /* machine id */
    memset(flash + 0x1C000, 0xFF, 0x4000); /* user/system blocks: blank (no saved settings) */
}

static const uint32_t part_ofs[5] = {0x1A000, 0x18000, 0x1C000, 0x10000, 0x00000};
static const uint32_t part_len[5] = {0x02000, 0x02000, 0x04000, 0x08000, 0x10000};

/* ---------------------------------------------------------------- GD-ROM */
#define MAX_REQ 16
static struct { int cmd; uint32_t p[4]; int status; } req[MAX_REQ];
static uint32_t next_req = 1;
static uint32_t last_lba;

static uint32_t toc_entry(uint32_t lba, int is_data) { return ((is_data ? 4u : 0u) << 28) | (1u << 24) | (lba + 150); }

static int gd_exec(int cmd, uint32_t *p) {
    switch (cmd) {
    case 16: case 17: case 32: case 34: {   /* PIOREAD / DMAREAD (+ stream variants) */
        uint32_t lba = p[0] - 150, n = p[1], dst = p[2];
        TR("bios: gd read lba %u x%u -> %08X\n", lba, n, dst);
        if (!DC_IS_RAM(dst)) return -1;
        if (DC_RAM_OFS(dst) + n * 2048 > 0x1000000) return -1;
        last_lba = lba + n;
        return dc_disc_read(lba, n, dc_ram + DC_RAM_OFS(dst)) ? -1 : 0;
    }
    case 18: case 19: {                      /* GETTOC / GETTOC2: p[0] session area, p[1] buffer */
        uint32_t buf = p[1], area = p[0];
        for (int i = 0; i < 102; i++) dc_wr32(buf + i * 4, 0xFFFFFFFFu);
        int first = 0, last = 0, n = dc_disc_track_count();
        for (int i = 0; i < n; i++) {
            uint32_t lba, secs; int data; dc_disc_track_info(i, &lba, &data, &secs);
            int hd = lba >= 45000;
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
    case 24: case 27: case 22: case 23: case 20: case 21: case 40:   /* INIT, SEEK, PAUSE, RELEASE, PLAY... */
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
    case 0: memcpy(dc_ram + 0x68, flash + 0x1A056, 8); c->r[0] = 0; break;
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
        c->r[0] = id;
        break;
    }
    case 1: {                                         /* get_cmd_stat(id, status[4]) */
        int k = c->r[4] % MAX_REQ;
        if (c->r[5]) {
            dc_wr32(c->r[5], req[k].status ? 1 : 0);  /* error code */
            dc_wr32(c->r[5] + 4, 0); dc_wr32(c->r[5] + 8, 0); dc_wr32(c->r[5] + 12, 0);
        }
        c->r[0] = req[k].status ? (uint32_t)-1 : 2;   /* 2 = completed */
        break;
    }
    case 4:                                           /* get_drv_stat(status[2]) */
        if (c->r[4]) { dc_wr32(c->r[4], 2); dc_wr32(c->r[4] + 4, 0x80); }  /* standby, GD-ROM */
        c->r[0] = 0;
        break;
    case 10:                                          /* change_data_type(params[4]) */
        c->r[0] = 0;
        break;
    case 2: case 3: case 5: case 8: case 9:           /* exec_server, init, dma_end, abort, reset */
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

int dc_bios_install(sh4ctx *c, char region) {
    (void)c;
    if (!flash_loaded) flash_init(region);
    struct { uint32_t vec; sh4fn fn; } v[] = {
        {V_SYSINFO, h_sysinfo}, {V_ROMFONT, h_romfont}, {V_FLASH, h_flash},
        {V_GDROM, h_gdrom}, {V_GDROM2, h_gdrom}, {V_MISC, h_misc},
    };
    for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) {
        uint32_t entry = v[i].vec == V_GDROM ? GD_ENTRY : HLE_BASE + i * 0x10;
        dc_wr32(v[i].vec, entry);
        dc_hle_register(entry, v[i].fn);
    }
    /* IP.BIN (first 0x8000 bytes of the HD area) where the boot ROM leaves it */
    if (dc_disc_track_count() && dc_disc_read(45000, 16, dc_ram + 0x8000) == 0)
        return 0;
    return -1;
}

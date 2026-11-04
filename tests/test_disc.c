/* Disc image reader: a synthetic GDI with an ISO9660 file system in the high-density area, and a
 * two-session MODE2 CD-ROM (MIL-CD / CD-R layout) with a scrambled boot file.
 * cc -Iruntime/include tests/test_disc.c runtime/src/disc.c -o test_disc */
#include "dccore/disc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void le32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static int dirent(uint8_t *p, uint32_t lba, uint32_t size, int dir, const char *name) {
    size_t n = strlen(name); int len = 33 + (int)n + ((n & 1) ? 0 : 1);
    memset(p, 0, (size_t)len); p[0] = (uint8_t)len; le32(p + 2, lba); le32(p + 10, size); p[25] = dir ? 2 : 0; p[32] = (uint8_t)n;
    memcpy(p + 33, name, n);
    return len;
}

/* scramble.c's scramble (what a CD's boot file goes through), to check dc_descramble against */
static void scramble(const uint8_t *in, uint8_t *out, uint32_t size) {
    static int idx[(2048 * 1024) / 32];
    uint32_t seed = size & 0xFFFF, pos = 0, left = size; uint8_t *o = out;
    for (uint32_t chunk = 2048 * 1024; chunk >= 32; chunk >>= 1)
        while (left >= chunk) {
            int n = (int)(chunk / 32);
            for (int i = 0; i < n; i++) idx[i] = i;
            for (int i = n - 1; i >= 0; i--) {
                seed = (seed * 2109 + 9273) & 0x7FFF;
                int x = (int)((((seed + 0xC000) & 0xFFFF) * (uint32_t)i) >> 16);
                int t = idx[i]; idx[i] = idx[x]; idx[x] = t;
                memcpy(o, in + pos + 32 * idx[i], 32); o += 32;
            }
            pos += chunk; left -= chunk;
        }
    memcpy(o, in + pos, left);
}

/* 2352-byte MODE2 XA form 1 sector: sync, header (mode byte 2), 8-byte subheader, 2048 bytes of data */
static void mode2_sector(uint8_t *raw, const uint8_t *data) {
    memset(raw, 0, 2352); memset(raw + 1, 0xFF, 10); raw[15] = 2; memcpy(raw + 24, data, 2048);
}

static void cd_tests(void) {
    /* session 1: track 1 at LBA 0, 40 sectors: the file system's files (absolute LBAs);
     * session 2: track 2 at LBA 40 + 11400: IP.BIN, PVD at +16, root directory at +17 */
    enum { N1 = 40, N2 = 20, BOOT = N1 + 11400, FSIZE = 3 * 2048 + 1000 };
    static uint8_t sec[2048], raw[2352], plain[FSIZE], scr[FSIZE], buf[FSIZE + 2048];
    for (int i = 0; i < FSIZE; i++) plain[i] = (uint8_t)(i * 13 + (i >> 8));
    scramble(plain, scr, FSIZE);
    FILE *f = fopen("cdtest_t1.bin", "wb");
    for (int s = 0; s < N1; s++) {
        memset(sec, 0, sizeof sec);
        if (s >= 10 && s < 14) memcpy(sec, scr + (s - 10) * 2048, s == 13 ? FSIZE - 3 * 2048 : 2048);   /* 1ST_READ.BIN at LBA 10 */
        mode2_sector(raw, sec); fwrite(raw, 1, 2352, f);
    }
    fclose(f);
    f = fopen("cdtest_t2.bin", "wb");
    for (int s = 0; s < N2; s++) {
        memset(sec, 0, sizeof sec);
        if (s == 0) { memcpy(sec, "SEGA SEGAKATANA ", 16); memcpy(sec + 0x40, "T0000     ", 10); memcpy(sec + 0x60, "1ST_READ.BIN    ", 16); }
        if (s == 16) { sec[0] = 1; memcpy(sec + 1, "CD001", 5); dirent(sec + 156, BOOT + 17, 2048, 1, "\x00"); }
        if (s == 17) { int o = 0; o += dirent(sec + o, BOOT + 17, 2048, 1, "\x00"); o += dirent(sec + o, BOOT + 17, 2048, 1, "\x01");
                       o += dirent(sec + o, 10, FSIZE, 0, "1ST_READ.BIN;1"); }
        mode2_sector(raw, sec); fwrite(raw, 1, 2352, f);
    }
    fclose(f);
    f = fopen("cdtest.cue", "w");
    fprintf(f, "REM SESSION 01\nFILE \"cdtest_t1.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n"
               "REM SESSION 02\nFILE \"cdtest_t2.bin\" BINARY\n  TRACK 02 MODE2/2352\n    INDEX 01 00:00:00\n");
    fclose(f);
    CHECK(dc_disc_open("cdtest.cue") == 0);
    CHECK(dc_disc_is_cd());
    CHECK(dc_disc_boot_lba() == BOOT);
    uint32_t lba = 0, size = 0;
    CHECK(dc_disc_find("1ST_READ.BIN", &lba, &size) == 0 && lba == 10 && size == FSIZE);
    char prod[11];
    CHECK(dc_disc_ip_product(prod) == 0 && !strcmp(prod, "T0000"));
    CHECK(dc_disc_load_file("1ST_READ.BIN", buf, sizeof buf) == FSIZE && !memcmp(buf, scr, FSIZE));
    CHECK(dc_disc_load_boot(buf, sizeof buf) == FSIZE && !memcmp(buf, plain, FSIZE));
    dc_disc_close();
    remove("cdtest_t1.bin"); remove("cdtest_t2.bin"); remove("cdtest.cue");
    /* descrambling: sizes with 2 MB chunks and odd tails */
    static uint8_t a[2 * 1024 * 1024 + 70000 + 7], b[sizeof a];
    for (uint32_t i = 0; i < sizeof a; i++) a[i] = (uint8_t)(i * 2654435761u >> 24);
    scramble(a, b, sizeof a);
    CHECK(memcmp(a, b, sizeof a) != 0);
    dc_descramble(b, sizeof a);
    CHECK(!memcmp(a, b, sizeof a));
}

int main(void) {
    char path[256];                   /* files in the current directory, removed at the end */
    /* track 3 (HD area, LBA 45000): sector 16 PVD, 17 root dir, 18 DATA dir, 19.. files */
    enum { NSEC = 24 };
    static uint8_t img[NSEC * 2048];
    uint8_t *pvd = img + 16 * 2048; pvd[0] = 1; memcpy(pvd + 1, "CD001", 5);
    dirent(pvd + 156, 45017, 2048, 1, "\x00");
    uint8_t *root = img + 17 * 2048; int o = 0;
    o += dirent(root + o, 45017, 2048, 1, "\x00"); o += dirent(root + o, 45017, 2048, 1, "\x01");
    o += dirent(root + o, 45019, 5000, 0, "1ST_READ.BIN;1");
    o += dirent(root + o, 45018, 2048, 1, "DATA");
    uint8_t *data = img + 18 * 2048; o = 0;
    o += dirent(data + o, 45018, 2048, 1, "\x00"); o += dirent(data + o, 45017, 2048, 1, "\x01");
    o += dirent(data + o, 45022, 100, 0, "TRACK1.LZS;1");
    for (int i = 0; i < 5000; i++) img[19 * 2048 + i] = (uint8_t)(i * 7);
    for (int i = 0; i < 100; i++) img[22 * 2048 + i] = (uint8_t)(0xA0 + i);
    snprintf(path, sizeof path, "dctest_track03.bin"); FILE *f = fopen(path, "wb"); fwrite(img, 1, sizeof img, f); fclose(f);
    static uint8_t blank[2048 * 4];
    snprintf(path, sizeof path, "dctest_track01.bin"); f = fopen(path, "wb"); fwrite(blank, 1, sizeof blank, f); fclose(f);
    snprintf(path, sizeof path, "dctest_disc.gdi"); f = fopen(path, "w");
    fprintf(f, "2\n1 0 4 2048 dctest_track01.bin 0\n2 45000 4 2048 dctest_track03.bin 0\n"); fclose(f);

    CHECK(dc_disc_open(path) == 0);
    uint32_t lba, size;
    CHECK(dc_disc_find("1ST_READ.BIN", &lba, &size) == 0 && lba == 45019 && size == 5000);
    CHECK(dc_disc_find("1st_read.bin", &lba, &size) == 0);
    CHECK(dc_disc_find("DATA/TRACK1.LZS", &lba, &size) == 0 && lba == 45022 && size == 100);
    CHECK(dc_disc_find("NOPE.BIN", &lba, &size) != 0);
    CHECK(dc_disc_find("1ST_READ.BIN/X", &lba, &size) != 0);
    static uint8_t buf[8192];
    CHECK(dc_disc_load_file("1ST_READ.BIN", buf, sizeof buf) == 5000 && buf[4999] == (uint8_t)(4999 * 7) && buf[100] == (uint8_t)700);
    CHECK(dc_disc_load_file("DATA/TRACK1.LZS", buf, sizeof buf) == 100 && buf[0] == 0xA0 && buf[99] == (uint8_t)(0xA0 + 99));
    CHECK(dc_disc_load_file("1ST_READ.BIN", buf, 100) == -1);       /* does not fit */
    CHECK(!dc_disc_is_cd() && dc_disc_boot_lba() == 45000);
    remove("dctest_track01.bin"); remove("dctest_track03.bin"); remove("dctest_disc.gdi");
    cd_tests();
    printf(fails ? "%d disc checks failed\n" : "disc tests passed\n", fails);
    return fails != 0;
}

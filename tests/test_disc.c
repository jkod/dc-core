/* Disc image reader: a synthetic GDI with an ISO9660 file system in the high-density area.
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
    remove("dctest_track01.bin"); remove("dctest_track03.bin"); remove("dctest_disc.gdi");
    printf(fails ? "%d disc checks failed\n" : "disc tests passed\n", fails);
    return fails != 0;
}

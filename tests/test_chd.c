/* CHD images (DCCORE_CHD): the synthetic GD-ROM from make_gd_fixture.py, read as its .gdi and as the
 * .chd chdman made of it, must give the same tracks, sectors, audio and files.
 *
 *   python3 tests/make_gd_fixture.py DIR && chdman createcd -i DIR/disc.gdi -o DIR/disc.chd
 *   test_chd DIR
 *
 * CI makes DIR when chdman is installed; without it the test is skipped. */
#include "dccore/disc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

typedef struct { uint32_t lba, sectors; int data; } tinfo;

static int tracks(tinfo *t) {
    int n = dc_disc_track_count();
    for (int i = 0; i < n; i++) dc_disc_track_info(i, &t[i].lba, &t[i].data, &t[i].sectors);
    return n;
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: test_chd DIR\n"); return 2; }
    char gdi[1024], chd[1024];
    snprintf(gdi, sizeof gdi, "%s/disc.gdi", argv[1]);
    snprintf(chd, sizeof chd, "%s/disc.chd", argv[1]);
    FILE *f = fopen(chd, "rb");
    if (!f) { printf("chd: %s missing (no chdman), skipped\n", chd); return 0; }
    fclose(f);

    tinfo a[99], b[99];
    CHECK(dc_disc_open(gdi) == 0);
    int na = tracks(a);
    uint32_t boot_a = dc_disc_boot_lba();
    CHECK(dc_disc_open(chd) == 0);
    int nb = tracks(b);
    CHECK(na == 5 && nb == na);
    CHECK(!dc_disc_is_cd() && dc_disc_boot_lba() == boot_a && boot_a == 45000);
    for (int i = 0; i < na && i < nb; i++) {
        if (a[i].lba != b[i].lba || a[i].sectors != b[i].sectors || a[i].data != b[i].data)
            printf("track %d: gdi lba %u n %u data %d | chd lba %u n %u data %d\n", i + 1,
                   a[i].lba, a[i].sectors, a[i].data, b[i].lba, b[i].sectors, b[i].data);
        CHECK(a[i].lba == b[i].lba && a[i].sectors == b[i].sectors && a[i].data == b[i].data);
    }

    /* every sector of every track: user data for data tracks, raw samples for audio */
    static uint8_t x[2352], y[2352];
    int diff = 0;
    for (int i = 0; i < na; i++)
        for (uint32_t k = 0; k < a[i].sectors; k++) {
            uint32_t lba = a[i].lba + k;
            int ra, rb;
            if (a[i].data) {
                dc_disc_open(gdi); ra = dc_disc_read(lba, 1, x);
                dc_disc_open(chd); rb = dc_disc_read(lba, 1, y);
                if (ra || rb || memcmp(x, y, 2048)) diff++;
            } else {
                dc_disc_open(gdi); ra = dc_disc_read_raw(lba, 1, x);
                dc_disc_open(chd); rb = dc_disc_read_raw(lba, 1, y);
                if (ra || rb || memcmp(x, y, 2352)) diff++;
            }
        }
    if (diff) printf("%d sectors differ\n", diff);
    CHECK(diff == 0);

    /* files, one of them in track 5 (after the audio track and its gap) */
    char prod[11];
    CHECK(dc_disc_open(chd) == 0);
    CHECK(dc_disc_ip_product(prod) == 0 && !strcmp(prod, "T-00000"));
    static uint8_t fa[400000], fb[400000];
    const char *names[] = { "BIG.BIN", "SMALL.BIN" };
    for (int i = 0; i < 2; i++) {
        dc_disc_open(gdi); int la = dc_disc_load_file(names[i], fa, sizeof fa);
        dc_disc_open(chd); int lb = dc_disc_load_file(names[i], fb, sizeof fb);
        CHECK(la > 0 && la == lb && !memcmp(fa, fb, (size_t)la));
    }
    dc_disc_close();
    printf("chd: %s\n", fails ? "FAILED" : "all ok");
    return fails != 0;
}

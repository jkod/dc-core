/* dc-core: read the player's GD-ROM image (.gdi or Redump-style .cue) by LBA. */
#include "dccore/disc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_TRACKS 99

typedef struct {
    int num, type;          /* type 4 = data, 0 = audio */
    uint32_t lba;           /* image LBA of INDEX 01 (no +150) */
    int sector;             /* 2352 or 2048 */
    uint32_t skip;          /* sectors to skip at the start of the file (pregap) */
    uint32_t sectors;       /* sectors in the file */
    char path[1024];
    FILE *f;
} track_t;

static track_t trk[MAX_TRACKS];
static int ntrk;

static void dirname_of(const char *p, char *out, size_t n) {
    const char *s1 = strrchr(p, '/'), *s2 = strrchr(p, '\\');
    const char *s = s1 > s2 ? s1 : s2;
    if (!s) { snprintf(out, n, "."); return; }
    size_t l = (size_t)(s - p); if (l >= n) l = n - 1;
    memcpy(out, p, l); out[l] = 0;
}

static uint32_t file_sectors(const char *path, int sector) {
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); long long n = ftell(f); fclose(f);
    return (uint32_t)(n / sector);
}

static int load_gdi(const char *path, const char *dir) {
    FILE *f = fopen(path, "r"); if (!f) return -1;
    char line[1400]; int n = 0;
    if (!fgets(line, sizeof line, f)) { fclose(f); return -1; }
    n = atoi(line);
    for (int i = 0; i < n && fgets(line, sizeof line, f); i++) {
        track_t *t = &trk[ntrk];
        char name[1024] = {0};
        unsigned lba; int type, sec, num;
        char *q = strchr(line, '"');
        if (q) {
            if (sscanf(line, "%d %u %d %d", &num, &lba, &type, &sec) != 4) continue;
            char *e = strchr(q + 1, '"'); if (!e) continue;
            memcpy(name, q + 1, (size_t)(e - q - 1));
        } else if (sscanf(line, "%d %u %d %d %1023s", &num, &lba, &type, &sec, name) != 5) continue;
        t->num = num; t->lba = lba; t->type = type; t->sector = sec; t->skip = 0;
        snprintf(t->path, sizeof t->path, "%s/%s", dir, name);
        t->sectors = file_sectors(t->path, sec);
        ntrk++;
    }
    fclose(f);
    return ntrk ? 0 : -1;
}

static uint32_t msf(const char *s) { int m, se, fr; if (sscanf(s, "%d:%d:%d", &m, &se, &fr) != 3) return 0; return (uint32_t)((m * 60 + se) * 75 + fr); }

static int load_cue(const char *path, const char *dir) {
    FILE *f = fopen(path, "r"); if (!f) return -1;
    char line[1400], cur[1024] = {0};
    uint32_t file_lba = 0; track_t *t = NULL;
    while (fgets(line, sizeof line, f)) {
        char *p = line; while (isspace((unsigned char)*p)) p++;
        if (!strncmp(p, "REM HIGH-DENSITY", 16)) file_lba = 45000;
        else if (!strncmp(p, "FILE", 4)) {
            char *q = strchr(p, '"'), *e = q ? strchr(q + 1, '"') : NULL;
            if (q && e) { memcpy(cur, q + 1, (size_t)(e - q - 1)); cur[e - q - 1] = 0; }
        } else if (!strncmp(p, "TRACK", 5) && ntrk < MAX_TRACKS) {
            t = &trk[ntrk++]; memset(t, 0, sizeof *t);
            char mode[32] = {0}; sscanf(p, "TRACK %d %31s", &t->num, mode);
            t->type = strncmp(mode, "AUDIO", 5) ? 4 : 0;
            t->sector = strstr(mode, "2048") ? 2048 : 2352;
            snprintf(t->path, sizeof t->path, "%s/%s", dir, cur);
            t->sectors = file_sectors(t->path, t->sector);
            t->lba = file_lba;
        } else if (!strncmp(p, "INDEX 01", 8) && t) {
            t->skip = msf(p + 9);
            t->lba = file_lba + t->skip;
            file_lba += t->sectors;
        }
    }
    fclose(f);
    return ntrk ? 0 : -1;
}

int dc_disc_open(const char *path) {
    dc_disc_close();
    char dir[1024]; dirname_of(path, dir, sizeof dir);
    size_t l = strlen(path);
    int r = (l > 4 && (!strcmp(path + l - 4, ".cue") || !strcmp(path + l - 4, ".CUE"))) ? load_cue(path, dir) : load_gdi(path, dir);
    if (r) return r;
    for (int i = 0; i < ntrk; i++) if (!trk[i].sectors) { fprintf(stderr, "dc-core: track %d missing (%s)\n", trk[i].num, trk[i].path); return -2; }
    return 0;
}

void dc_disc_close(void) {
    for (int i = 0; i < ntrk; i++) if (trk[i].f) fclose(trk[i].f);
    memset(trk, 0, sizeof trk); ntrk = 0;
}

int dc_disc_track_count(void) { return ntrk; }
int dc_disc_track_info(int i, uint32_t *lba, int *is_data, uint32_t *sectors) {
    if (i < 0 || i >= ntrk) return -1;
    if (lba) *lba = trk[i].lba;
    if (is_data) *is_data = trk[i].type == 4;
    if (sectors) *sectors = trk[i].sectors - trk[i].skip;
    return 0;
}

int dc_disc_read(uint32_t lba, uint32_t count, uint8_t *dst) {
    for (uint32_t k = 0; k < count; k++, lba++, dst += 2048) {
        track_t *t = NULL;
        for (int i = 0; i < ntrk; i++) if (trk[i].lba <= lba && (!t || trk[i].lba > t->lba)) t = &trk[i];
        if (!t || lba - t->lba >= t->sectors - t->skip) return -1;
        if (!t->f && !(t->f = fopen(t->path, "rb"))) return -1;
        long long off = (long long)(lba - t->lba + t->skip) * t->sector + (t->sector == 2352 ? 16 : 0);
#ifdef _WIN32
        _fseeki64(t->f, off, SEEK_SET);
#else
        fseeko(t->f, (off_t)off, SEEK_SET);
#endif
        if (fread(dst, 1, 2048, t->f) != 2048) return -1;
    }
    return 0;
}

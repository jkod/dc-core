/* dc-core: read the player's disc image (.gdi, Redump-style .cue, or .chd) by LBA: GD-ROMs, and CD-ROMs
 * (MIL-CD / CD-R releases: MODE2 XA sectors, a second session that holds the boot area).
 * .chd needs DCCORE_CHD (libchdr; the CMake build turns it on). */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L      /* fseeko */
#endif
#define _FILE_OFFSET_BITS 64
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
    char path[2200];
    FILE *f;
    int chd;                /* in the CHD: frames from chd_frame on (2448 bytes each: sector + subcode) */
    uint32_t chd_frame;
    uint8_t *ra; uint32_t ra_first, ra_n;   /* read-ahead window (track_sector_unlocked) */
} track_t;

static track_t trk[MAX_TRACKS];
static int ntrk;
static int is_gd;               /* a GD-ROM (.gdi, or a cue with a high-density area); otherwise a CD-ROM */

/* ---------------------------------------------------------------- CHD (libchdr) */
#ifdef DCCORE_CHD
#include <libchdr/chd.h>
#define CHD_FRAME 2448              /* 2352 bytes of sector + 96 of subcode */
static chd_file *chd;
static uint8_t *chd_hunk;           /* the hunk last read, and its number */
static uint32_t chd_hunk_no = 0xFFFFFFFFu, chd_frames_per_hunk;

/* Tracks from the CHD's metadata. A GD-ROM CHD ('CHGD') made by chdman from a .gdi stores each
 * track padded out to where the next one starts, so a track's LBA is the sum of the frames before
 * it (and the high-density area starts at 45000 whatever the low-density tracks add up to). The
 * pregap: stored in the CHD when its type starts with 'V' (the track's data follows it), otherwise
 * only counted. Each track's frames start on a multiple of 4 in the CHD. */
static int load_chd(const char *path) {
    if (chd_open(path, CHD_OPEN_READ, NULL, &chd) != CHDERR_NONE) { chd = NULL; return -1; }
    const chd_header *h = chd_get_header(chd);
    if (!h || h->hunkbytes % CHD_FRAME) { dc_disc_close(); return -1; }
    chd_frames_per_hunk = h->hunkbytes / CHD_FRAME;
    chd_hunk = (uint8_t *)malloc(h->hunkbytes);
    if (!chd_hunk) { dc_disc_close(); return -1; }
    static const uint32_t tags[] = { GDROM_TRACK_METADATA_TAG, CDROM_TRACK_METADATA2_TAG, CDROM_TRACK_METADATA_TAG };
    uint32_t lba = 0, frame = 0;
    for (int k = 0; k < 3 && !ntrk; k++)
        for (uint32_t i = 0; ntrk < MAX_TRACKS; i++) {
            char m[256] = {0}, type[32] = {0}, sub[32] = {0}, pgtype[32] = {0}, pgsub[32] = {0};
            uint32_t len = 0;
            if (chd_get_metadata(chd, tags[k], i, m, sizeof m - 1, &len, NULL, NULL) != CHDERR_NONE) break;
            int num = 0, frames = 0, pad = 0, pregap = 0, postgap = 0;
            if (tags[k] == GDROM_TRACK_METADATA_TAG)
                sscanf(m, "TRACK:%d TYPE:%31s SUBTYPE:%31s FRAMES:%d PAD:%d PREGAP:%d PGTYPE:%31s PGSUB:%31s POSTGAP:%d",
                       &num, type, sub, &frames, &pad, &pregap, pgtype, pgsub, &postgap);
            else if (tags[k] == CDROM_TRACK_METADATA2_TAG)
                sscanf(m, "TRACK:%d TYPE:%31s SUBTYPE:%31s FRAMES:%d PREGAP:%d PGTYPE:%31s PGSUB:%31s POSTGAP:%d",
                       &num, type, sub, &frames, &pregap, pgtype, pgsub, &postgap);
            else
                sscanf(m, "TRACK:%d TYPE:%31s SUBTYPE:%31s FRAMES:%d", &num, type, sub, &frames);
            if (tags[k] == GDROM_TRACK_METADATA_TAG) is_gd = 1;
            if (is_gd && num == 3 && lba < 45000) lba = 45000;
            int stored = pgtype[0] == 'V' ? pregap : 0;          /* pregap frames held in the CHD */
            track_t *t = &trk[ntrk++]; memset(t, 0, sizeof *t);
            t->num = num; t->type = strcmp(type, "AUDIO") ? 4 : 0;
            t->sector = (!strcmp(type, "MODE1") || !strcmp(type, "MODE2_FORM1")) ? 2048 : 2352;
            t->chd = 1;
            t->lba = lba + (uint32_t)pregap;
            t->chd_frame = frame + (uint32_t)stored;
            t->sectors = (uint32_t)(frames - stored - pad);        /* the track's own sectors */
            snprintf(t->path, sizeof t->path, "%s#%d", path, num);
            lba += (uint32_t)(pregap - stored + frames + postgap);
            frame += ((uint32_t)frames + 3) & ~3u;
        }
    return ntrk ? 0 : -1;
}

/* frame `f` of the CHD: 2352 bytes of sector (the subcode after it is left out) */
static const uint8_t *chd_frame_at(uint32_t f) {
    uint32_t hn = f / chd_frames_per_hunk;
    if (hn != chd_hunk_no) {
        if (chd_read(chd, hn, chd_hunk) != CHDERR_NONE) { chd_hunk_no = 0xFFFFFFFFu; return NULL; }
        chd_hunk_no = hn;
    }
    return chd_hunk + (f % chd_frames_per_hunk) * CHD_FRAME;
}
#endif

/* sector `k` of track `t` as stored: t->sector bytes (audio in CHDs is big-endian: swapped here) */
void (*dc_disc_lock)(int on);          /* set when the disc is also read from another thread (CD audio) */
static int track_sector_unlocked(track_t *t, uint32_t k, uint8_t *dst);
static int track_sector(track_t *t, uint32_t k, uint8_t *dst) {
    if (!dc_disc_lock) return track_sector_unlocked(t, k, dst);
    dc_disc_lock(1);
    int r = track_sector_unlocked(t, k, dst);
    dc_disc_lock(0);
    return r;
}
static int track_sector_unlocked(track_t *t, uint32_t k, uint8_t *dst) {
#ifdef DCCORE_CHD
    if (t->chd) {
        const uint8_t *p = chd_frame_at(t->chd_frame + k);
        if (!p) return -1;
        if (t->type == 0) for (int i = 0; i < 2352; i += 2) { dst[i] = p[i + 1]; dst[i + 1] = p[i]; }
        else memcpy(dst, p, (size_t)t->sector);
        return 0;
    }
#endif
    if (!t->f && !(t->f = fopen(t->path, "rb"))) return -1;
    /* read ahead RA_SECTORS at a time: one seek and read per sector was a request to the SD card for each
     * of CD audio's 75 sectors a second on the Switch (6.5% of the sound thread) */
    enum { RA_SECTORS = 32 };
    if (!t->ra) t->ra = malloc((size_t)RA_SECTORS * 2352);
    if (t->ra && k >= t->ra_first && k < t->ra_first + t->ra_n) {
        memcpy(dst, t->ra + (size_t)(k - t->ra_first) * t->sector, (size_t)t->sector);
        return 0;
    }
    long long off = (long long)(k + t->skip) * t->sector;
#ifdef _WIN32
    _fseeki64(t->f, off, SEEK_SET);
#else
    fseeko(t->f, (off_t)off, SEEK_SET);
#endif
    if (!t->ra) return fread(dst, 1, (size_t)t->sector, t->f) == (size_t)t->sector ? 0 : -1;
    size_t got = fread(t->ra, (size_t)t->sector, RA_SECTORS, t->f);
    t->ra_first = k; t->ra_n = (uint32_t)got;
    if (!got) return -1;
    memcpy(dst, t->ra, (size_t)t->sector);
    return 0;
}

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
        if (!strncmp(p, "REM HIGH-DENSITY", 16)) { file_lba = 45000; is_gd = 1; }
        /* a CD's next session starts after the lead-out (6750), the next lead-in (4500) and the
         * first track's pregap (150) */
        else if (!strncmp(p, "REM SESSION", 11) && atoi(p + 11) > 1 && !is_gd) file_lba += 11400;
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

static int has_ext(const char *path, const char *ext) {
    size_t l = strlen(path), e = strlen(ext);
    if (l <= e) return 0;
    for (size_t i = 0; i < e; i++) if (tolower((unsigned char)path[l - e + i]) != ext[i]) return 0;
    return 1;
}

int dc_disc_open(const char *path) {
    dc_disc_close();
    char dir[1024]; dirname_of(path, dir, sizeof dir);
    if (has_ext(path, ".chd")) {
#ifdef DCCORE_CHD
        return load_chd(path);
#else
        fprintf(stderr, "dc-core: built without CHD support (DCCORE_CHD)\n");
        return -1;
#endif
    }
    int cue = has_ext(path, ".cue");
    is_gd = !cue;
    int r = cue ? load_cue(path, dir) : load_gdi(path, dir);
    if (r) return r;
    for (int i = 0; i < ntrk; i++) if (!trk[i].sectors) { fprintf(stderr, "dc-core: track %d missing (%s)\n", trk[i].num, trk[i].path); return -2; }
    return 0;
}

void dc_disc_close(void) {
    for (int i = 0; i < ntrk; i++) { if (trk[i].f) fclose(trk[i].f); free(trk[i].ra); }
    memset(trk, 0, sizeof trk); ntrk = 0; is_gd = 0;
#ifdef DCCORE_CHD
    if (chd) chd_close(chd);
    chd = NULL; free(chd_hunk); chd_hunk = NULL; chd_hunk_no = 0xFFFFFFFFu;
#endif
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
        if (t->sector == 2352) {
            /* user data after 12 sync + 4 header bytes; MODE2 (XA form 1) has an 8-byte subheader too */
            uint8_t raw[2352];
            if (track_sector(t, lba - t->lba, raw)) return -1;
            memcpy(dst, raw + (raw[15] == 2 ? 24 : 16), 2048);
        } else if (track_sector(t, lba - t->lba, dst)) return -1;
    }
    return 0;
}

/* where IP.BIN and the file system start: the high-density area on a GD-ROM, the last session's
 * first data track on a CD */
uint32_t dc_disc_boot_lba(void) {
    uint32_t best = 0; int found = 0;
    if (is_gd) return 45000;
    for (int i = 0; i < ntrk; i++) {
        if (trk[i].type == 4 && (!found || trk[i].lba > best)) { best = trk[i].lba; found = 1; }
    }
    return best;
}
int dc_disc_is_cd(void) { return ntrk && !is_gd; }

/* The boot ROM descrambles the boot file of a CD-ROM (Marcus Comstedt's scramble.c, reversed): from
 * 2 MB chunks down to 32 bytes, each chunk's 32-byte slices are stored in a pseudo-random order;
 * the bytes after the last full slice stay in place. */
void dc_descramble(uint8_t *p, uint32_t size) {
    uint32_t seed = size & 0xFFFF;
    uint8_t *out = (uint8_t *)malloc(size ? size : 1);
    static int idx[(2048 * 1024) / 32];
    if (!out) return;
    uint32_t pos = 0, left = size;
    for (uint32_t chunk = 2048 * 1024; chunk >= 32; chunk >>= 1)
        while (left >= chunk) {
            int n = (int)(chunk / 32);
            for (int i = 0; i < n; i++) idx[i] = i;
            const uint8_t *src = p + pos;
            for (int i = n - 1; i >= 0; i--) {
                seed = (seed * 2109 + 9273) & 0x7FFF;
                int x = (int)((((seed + 0xC000) & 0xFFFF) * (uint32_t)i) >> 16);
                int tmp = idx[i]; idx[i] = idx[x]; idx[x] = tmp;
                memcpy(out + pos + 32 * idx[i], src, 32); src += 32;
            }
            pos += chunk; left -= chunk;
        }
    memcpy(out + pos, p + pos, left);
    memcpy(p, out, size);
    free(out);
}

/* raw 2352-byte sectors (CD audio: 588 stereo 16-bit little-endian frames each) */
int dc_disc_read_raw(uint32_t lba, uint32_t count, uint8_t *dst) {
    for (uint32_t k = 0; k < count; k++, lba++, dst += 2352) {
        track_t *t = NULL;
        for (int i = 0; i < ntrk; i++) if (trk[i].lba <= lba && (!t || trk[i].lba > t->lba)) t = &trk[i];
        if (!t || t->sector != 2352 || lba - t->lba >= t->sectors - t->skip) return -1;
        if (track_sector(t, lba - t->lba, dst)) return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------- ISO9660 lookup (boot area) */
static int find_in_dir(uint32_t lba, uint32_t size, const char *name, size_t nlen, uint32_t *elba, uint32_t *esize, int *is_dir) {
    uint8_t *d = (uint8_t *)malloc(size + 2048);
    if (!d || dc_disc_read(lba, (size + 2047) / 2048, d)) { free(d); return -1; }
    for (uint32_t p = 0; p < size; ) {
        uint8_t ln = d[p];
        if (!ln) { p = (p / 2048 + 1) * 2048; continue; }
        const uint8_t *e = d + p;
        size_t fl = e[32];
        const char *fn = (const char *)e + 33;
        size_t n = 0; while (n < fl && fn[n] != ';') n++;
        if (n == nlen) {
            size_t k = 0;
            while (k < n && toupper((unsigned char)fn[k]) == toupper((unsigned char)name[k])) k++;
            if (k == n) {
                *elba = e[2] | e[3] << 8 | e[4] << 16 | (uint32_t)e[5] << 24;
                *esize = e[10] | e[11] << 8 | e[12] << 16 | (uint32_t)e[13] << 24;
                *is_dir = (e[25] & 2) != 0;
                free(d); return 0;
            }
        }
        p += ln;
    }
    free(d);
    return -1;
}

int dc_disc_find(const char *path, uint32_t *lba, uint32_t *size) {
    uint8_t pvd[2048];
    if (dc_disc_read(dc_disc_boot_lba() + 16, 1, pvd) || memcmp(pvd + 1, "CD001", 5)) return -1;
    const uint8_t *r = pvd + 156;
    uint32_t dl = r[2] | r[3] << 8 | r[4] << 16 | (uint32_t)r[5] << 24, ds = r[10] | r[11] << 8 | r[12] << 16 | (uint32_t)r[13] << 24;
    while (*path == '/') path++;
    for (;;) {
        const char *slash = strchr(path, '/');
        size_t n = slash ? (size_t)(slash - path) : strlen(path);
        int is_dir = 0;
        if (find_in_dir(dl, ds, path, n, &dl, &ds, &is_dir)) return -1;
        if (!slash) { *lba = dl; *size = ds; return 0; }
        if (!is_dir) return -1;
        path = slash + 1;
    }
}

/* every file under directory (lba, size), depth first; path holds the directory's path */
static int walk_dir(uint32_t lba, uint32_t size, char *path, size_t plen, int depth,
                    int (*cb)(const char *, uint32_t, uint32_t, void *), void *user) {
    if (depth > 16) return 0;
    uint8_t *d = (uint8_t *)malloc(size + 2048);
    if (!d || dc_disc_read(lba, (size + 2047) / 2048, d)) { free(d); return -1; }
    int r = 0;
    for (uint32_t p = 0; p < size && !r; ) {
        uint8_t ln = d[p];
        if (!ln) { p = (p / 2048 + 1) * 2048; continue; }
        const uint8_t *e = d + p;
        p += ln;
        size_t fl = e[32];
        const char *fn = (const char *)e + 33;
        if (fl == 1 && (fn[0] == 0 || fn[0] == 1)) continue;       /* . and .. */
        size_t n = 0; while (n < fl && fn[n] != ';') n++;
        if (plen + n + 2 > 1024) continue;
        memcpy(path + plen, fn, n); path[plen + n] = 0;
        uint32_t el = e[2] | e[3] << 8 | e[4] << 16 | (uint32_t)e[5] << 24, es = e[10] | e[11] << 8 | e[12] << 16 | (uint32_t)e[13] << 24;
        if (e[25] & 2) { path[plen + n] = '/'; path[plen + n + 1] = 0; r = walk_dir(el, es, path, plen + n + 1, depth + 1, cb, user); }
        else r = cb(path, el, es, user);
        path[plen] = 0;
    }
    free(d);
    return r < 0 ? 0 : r;
}

int dc_disc_walk(int (*cb)(const char *path, uint32_t lba, uint32_t size, void *user), void *user) {
    uint8_t pvd[2048];
    if (dc_disc_read(dc_disc_boot_lba() + 16, 1, pvd) || memcmp(pvd + 1, "CD001", 5)) return -1;
    const uint8_t *r = pvd + 156;
    uint32_t dl = r[2] | r[3] << 8 | r[4] << 16 | (uint32_t)r[5] << 24, ds = r[10] | r[11] << 8 | r[12] << 16 | (uint32_t)r[13] << 24;
    char path[1024] = "";
    return walk_dir(dl, ds, path, 0, 0, cb, user);
}

int dc_disc_load_file(const char *path, uint8_t *dst, uint32_t max) {
    uint32_t lba, size;
    if (dc_disc_find(path, &lba, &size) || size > max) return -1;
    uint8_t *tmp = (uint8_t *)malloc(((size + 2047) / 2048) * 2048);
    if (!tmp || dc_disc_read(lba, (size + 2047) / 2048, tmp)) { free(tmp); return -1; }
    memcpy(dst, tmp, size); free(tmp);
    return (int)size;
}

int dc_disc_ip_product(char out[11]) {
    uint8_t ip[2048];
    if (dc_disc_read(dc_disc_boot_lba(), 1, ip)) return -1;
    int n = 0;
    for (int i = 0x40; i < 0x4A && ip[i] != ' '; i++) out[n++] = (char)ip[i];
    out[n] = 0;
    return 0;
}

/* the boot file named in IP.BIN (usually 1ST_READ.BIN), descrambled on a CD-ROM like the boot ROM does */
int dc_disc_load_boot(uint8_t *dst, uint32_t max) {
    uint8_t ip[2048];
    if (dc_disc_read(dc_disc_boot_lba(), 1, ip)) return -1;
    char name[17]; int n = 0;
    for (int i = 0x60; i < 0x70 && ip[i] > ' '; i++) name[n++] = (char)ip[i];
    name[n] = 0;
    int r = dc_disc_load_file(n ? name : "1ST_READ.BIN", dst, max);
    if (r > 0 && dc_disc_is_cd()) dc_descramble(dst, (uint32_t)r);
    return r;
}

#ifndef DCCORE_DISC_H
#define DCCORE_DISC_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Open the player's image: .gdi or Redump-style .cue (GD-ROM, or a CD-ROM: MODE1/MODE2 sectors, several
 * sessions). 0 = ok, -2 = a track file is missing. */
int  dc_disc_open(const char *path);
void dc_disc_close(void);
/* Read `count` 2048-byte user-data sectors starting at image LBA `lba` (no +150 offset). */
int  dc_disc_read(uint32_t lba, uint32_t count, uint8_t *dst);
/* Read raw 2352-byte sectors (CD audio tracks) starting at image LBA `lba`. */
int  dc_disc_read_raw(uint32_t lba, uint32_t count, uint8_t *dst);
int  dc_disc_track_count(void);
int  dc_disc_track_info(int i, uint32_t *lba, int *is_data, uint32_t *sectors);
/* LBA of IP.BIN and the file system: 45000 (high-density area) on a GD-ROM, the last session's data
 * track on a CD-ROM */
uint32_t dc_disc_boot_lba(void);
int  dc_disc_is_cd(void);
/* ISO9660 file in the boot area ("1ST_READ.BIN", "DATA/X.BIN"; case-insensitive) */
int  dc_disc_find(const char *path, uint32_t *lba, uint32_t *size);
/* Call cb for every file in the high-density area ("DIR/NAME.EXT", its LBA and size), depth first;
 * a non-zero return from cb stops the walk and is returned. -1: no ISO9660 file system. */
int  dc_disc_walk(int (*cb)(const char *path, uint32_t lba, uint32_t size, void *user), void *user);
int  dc_disc_load_file(const char *path, uint8_t *dst, uint32_t max);   /* bytes read, -1 on error */
int  dc_disc_ip_product(char out[11]);
/* the boot file named in IP.BIN, descrambled on a CD-ROM (as the boot ROM does): bytes, -1 on error */
int  dc_disc_load_boot(uint8_t *dst, uint32_t max);
void dc_descramble(uint8_t *p, uint32_t size);    /* product number from IP.BIN, e.g. "T-9709D-50" */
#ifdef __cplusplus
}
#endif
#endif

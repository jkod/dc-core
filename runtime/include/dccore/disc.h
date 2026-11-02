#ifndef DCCORE_DISC_H
#define DCCORE_DISC_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Open the player's image: .gdi or Redump-style .cue. 0 = ok, -2 = a track file is missing. */
int  dc_disc_open(const char *path);
void dc_disc_close(void);
/* Read `count` 2048-byte user-data sectors starting at image LBA `lba` (no +150 offset). */
int  dc_disc_read(uint32_t lba, uint32_t count, uint8_t *dst);
/* Read raw 2352-byte sectors (CD audio tracks) starting at image LBA `lba`. */
int  dc_disc_read_raw(uint32_t lba, uint32_t count, uint8_t *dst);
int  dc_disc_track_count(void);
int  dc_disc_track_info(int i, uint32_t *lba, int *is_data, uint32_t *sectors);
/* ISO9660 file in the high-density area ("1ST_READ.BIN", "DATA/X.BIN"; case-insensitive) */
int  dc_disc_find(const char *path, uint32_t *lba, uint32_t *size);
int  dc_disc_load_file(const char *path, uint8_t *dst, uint32_t max);   /* bytes read, -1 on error */
int  dc_disc_ip_product(char out[11]);    /* product number from IP.BIN, e.g. "T-9709D-50" */
#ifdef __cplusplus
}
#endif
#endif

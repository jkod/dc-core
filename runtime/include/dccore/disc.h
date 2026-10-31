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
int  dc_disc_track_count(void);
int  dc_disc_track_info(int i, uint32_t *lba, int *is_data, uint32_t *sectors);
#ifdef __cplusplus
}
#endif
#endif

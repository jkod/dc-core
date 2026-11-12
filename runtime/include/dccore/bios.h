#ifndef DCCORE_BIOS_H
#define DCCORE_BIOS_H
#include "dccore/sh4.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Install native replacements for the boot ROM syscalls (sysinfo, ROM font, flash, GD-ROM,
 * misc) and load IP.BIN from the open disc to 0x8C008000. region: '0' JP, '1' US, '2' EU. */
int dc_bios_install(sh4ctx *c, char region);
extern int dc_bios_trace;
extern int dc_gd_dma_irq;          /* 1: GD DMA reads raise the GD-DMA end interrupt (Tennis 2K2) */
extern int dc_gd_stat_free;        /* 1: get_cmd_stat reports COMPLETED once, then NO_ACTIVE (Dolphin Blue) */
/* CD audio (GD commands PLAY / PLAY2 / PAUSE / RELEASE / SEEK / STOP): the next stereo frame at
 * 44.1 kHz, 0 when nothing plays. The AICA mixes it as its external inputs (EFSDL/EFPAN 16, 17). */
int dc_cdda_frame(int16_t *l, int16_t *r);
/* CD audio for a port's own drive emulation: play image LBAs [start, end) (repeat 15 = forever),
 * pause, stop; status 0 stopped, 1 playing, 2 paused (current LBA in *lba) */
void dc_cdda_play(uint32_t start, uint32_t end, int repeat);
void dc_cdda_pause(int on);
void dc_cdda_stop(void);
int  dc_cdda_status(uint32_t *lba);
extern void (*dc_bios_read_hook)(uint32_t lba, uint32_t count, uint32_t dst);   /* before each GD read */
/* Optional: the player's own boot ROM (dc_boot.bin, 2 MB) and flash (dc_flash.bin, 128 KB).
 * The ROM is mapped at 0x00000000 (reads through NULL pointers see it, like on hardware) and
 * provides the built-in font; the flash replaces the synthetic one. Call before dc_bios_install. */
int dc_bios_load(const char *boot_rom, const char *flash);
/* the same from a folder: the boot ROM as dc_boot.bin (or dc_bios.bin, boot.bin, bios.bin) and the
 * flash as dc_flash.bin (or dc_nvmem.bin, flash.bin); either may be missing. -1: neither there.
 * A flash without network settings gets them for the session (online games need them). */
int dc_bios_load_dir(const char *dir);
extern uint8_t *dc_bios_rom;
extern uint8_t dc_flash[0x20000];   /* non-zero: log every syscall to stderr */
#ifdef __cplusplus
}
#endif
#endif

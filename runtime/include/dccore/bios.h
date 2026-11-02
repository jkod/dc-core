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
extern void (*dc_bios_read_hook)(uint32_t lba, uint32_t count, uint32_t dst);   /* before each GD read */
/* Optional: the player's own boot ROM (dc_boot.bin, 2 MB) and flash (dc_flash.bin, 128 KB).
 * The ROM is mapped at 0x00000000 (reads through NULL pointers see it, like on hardware) and
 * provides the built-in font; the flash replaces the synthetic one. Call before dc_bios_install. */
int dc_bios_load(const char *boot_rom, const char *flash);
extern uint8_t *dc_bios_rom;
extern uint8_t dc_flash[0x20000];   /* non-zero: log every syscall to stderr */
#ifdef __cplusplus
}
#endif
#endif

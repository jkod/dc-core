#ifndef DCCORE_BIOS_H
#define DCCORE_BIOS_H
#include "dccore/sh4.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Install native replacements for the boot ROM syscalls (sysinfo, ROM font, flash, GD-ROM,
 * misc) and load IP.BIN from the open disc to 0x8C008000. region: '0' JP, '1' US, '2' EU. */
int dc_bios_install(sh4ctx *c, char region);
extern int dc_bios_trace;   /* non-zero: log every syscall to stderr */
#ifdef __cplusplus
}
#endif
#endif

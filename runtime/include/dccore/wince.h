/* dc-core: Windows CE games (the Dreamcast titles that boot 0WINCEOS.BIN instead of 1ST_READ.BIN).
 *
 * Such a game is a CE program: an EXE plus DLLs (its own, Sega's game framework, DirectDraw/D3D/
 * DirectSound and the drivers under them), all user-mode SH-4 code that reaches the system only through
 * COREDLL. dc-core runs it without the CE kernel:
 *
 *  - tools/wince/ce_layout.py puts every module at a fixed address and lists its functions (.pdata), so
 *    sh4recomp.py can recompile them all into one table;
 *  - the loader here maps the same modules from the player's disc into the user address space
 *    (dc_uram, 32 MB at 0x00000000) at those addresses and links their imports;
 *  - COREDLL (and OLE32) are native: threads on fibers with CE's priority scheduling, events, mutexes,
 *    critical sections, heaps and virtual memory, files on the disc, the registry, a minimal window
 *    manager, and the OEM layer the drivers need (VirtualCopy windows onto the hardware, interrupts as
 *    events, store queues). The drivers themselves (DDHAL, SNDCORE, MAPLE) run recompiled against
 *    dc-core's hardware model, so the PowerVR renderer, AICA and Maple work as for any other game.
 */
#ifndef DCCORE_WINCE_H
#define DCCORE_WINCE_H
#include "dccore/sh4.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct ce_module_desc {
    const char *name;            /* file name on the disc, upper case ("RALLY2.EXE") */
    unsigned base, size;         /* where ce_layout.py put it, image size */
    unsigned long long hash;     /* FNV-1a of the file the code was recompiled from */
    unsigned file_size;
} ce_module_desc;

/* Map the modules (the first is the EXE) from the open disc. 0 = ok; -1 a file is missing,
 * -2 a file differs from the one the build was made from (*bad = its name). */
int  ce_init(sh4ctx *c, const ce_module_desc *mods, int n, const char **bad);
/* Run the EXE's entry point as the main thread. Returns when the game exits (ExitProcess or the
 * last thread ends); the result is the exit code. */
int  ce_run(sh4ctx *c, const char *cmdline);   /* command line (ASCII) */

/* registry values a port provides before ce_run (the console's ROM image has its own registry) */
void ce_reg_set_dword(const char *key, const char *name, uint32_t v);   /* "HKLM\\Drivers\\X", "Irq" */
void ce_reg_set_string(const char *key, const char *name, const char *v);

/* a function to run at process start, before the EXE (the work of CE's own system processes; e.g.
 * WDMLIB!InitWDM loads the WDM drivers listed under HKLM\WDMDrivers\BuiltIn, like wdevice.exe) */
void ce_add_startup(const char *dll, const char *fn);

/* load the AICA's ARM7 program from the disc into sound RAM and start it (CE: armcom.drv, which the
 * ROM loads at boot; SNDCORE expects it running). After ce_init. */
int  ce_load_arm_firmware(const char *name);

extern int ce_trace;            /* 1: log API calls (DC_CE_TRACE=1), 2: also every switch between threads */
uint32_t ce_milliseconds(void); /* the guest's GetTickCount */

#ifdef __cplusplus
}
#endif
#endif

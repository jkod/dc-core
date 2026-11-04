#ifndef DCCORE_HW_H
#define DCCORE_HW_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Observers for the hardware model; a renderer / sound / input backend fills these in. */
typedef struct dc_hw_hooks {
    void (*on_vblank)(uint64_t frame);
    void (*on_irq)(uint32_t intevt);
    void (*on_pvr_write)(uint32_t ofs, uint32_t v);        /* ofs from 0x005F8000 */
    void (*on_sb_write)(uint32_t addr, uint32_t v);        /* 0x005F6800.. */
    void (*on_aica_write)(uint32_t addr, uint32_t v);      /* AICA registers and sound RAM */
    void (*on_ta_write)(uint32_t addr, uint32_t v);        /* direct 32-bit writes to the TA FIFO */
    int  (*on_sq)(uint32_t dst, const uint32_t data[8]);   /* store-queue burst; return 1 if consumed */
    void (*on_render)(void);                               /* STARTRENDER: draw the lists received so far */
    /* Maple bus: a frame for `port`; write the reply to recv (P1 address) and return 1, or return 0
     * for "no device". send = address of the frame's data words. */
    int  (*on_maple)(int port, uint32_t frame, uint32_t send, int words, uint32_t recv);
} dc_hw_hooks;
extern dc_hw_hooks dc_hw;
/* Video RAM in the PVR's 64-bit (texture) layout: the console's 8 MB, then 8 MB of extra texture memory
 * that only a port can fill (dc_vram_ext_write) and only texture fetches see, after dc_vram_ext_enable(). */
#define DC_VRAM_SIZE     0x800000u
#define DC_VRAM_EXT_SIZE 0x800000u
extern uint8_t dc_vram[DC_VRAM_SIZE + DC_VRAM_EXT_SIZE];
extern uint32_t dc_vram_gen[(DC_VRAM_SIZE + DC_VRAM_EXT_SIZE) >> 12];   /* per 4 KB VRAM page, bumped on writes */
/* Extra texture memory (a port's own textures beyond the console's 8 MB, e.g. assets a game never fitted):
 * texture addresses (TCW bits 0..20, 16 MB) are no longer folded into 8 MB, so 0x800000.. reaches the
 * extra area. Off by default: on the console the top bit is ignored and some games may leave it set.
 * dc_vram_ext_write: ofs is a texture address (>= DC_VRAM_SIZE), data in the 64-bit (texture) layout. */
extern uint32_t dc_vram_tex_mask;     /* DC_VRAM_SIZE - 1, or the 16 MB mask with the extension */
void dc_vram_ext_enable(void);
int  dc_vram_ext_write(uint32_t ofs, const void *data, uint32_t len);
extern uint32_t dc_vram32_gen[0x800]; /* per 4 KB of the 32-bit area (framebuffer addresses), bumped on writes through it */
extern uint32_t dc_pal_gen;           /* bumped on palette writes */
extern int dc_hw_verbose;
extern int dc_dmac_log;               /* log SH-4 DMAC auto-request transfers to stderr (default 1) */
extern int dc_cable;             /* AV cable the game sees: 0 VGA (default), 2 RGB, 3 composite */
extern void (*dc_hw_read_hook)(uint32_t addr, int size);              /* every hardware read */
extern void (*dc_unhandled_hook)(const char *kind, uint32_t addr);   /* every unhandled access */

/* An address window: [va, va+len) reads and writes [to, to+len) (RAM, hardware or user memory). For
 * an operating system's virtual mappings of hardware (Windows CE VirtualCopy). va must be outside RAM
 * and the user address space. len 0 removes the window at va. */
void     dc_hw_map(uint32_t va, uint32_t to, uint32_t len);
/* Store queues mapped through the MMU (Windows CE's SetStoreQueueBase): non-zero = the address a flush
 * of 0xE0000000 + offset writes is dc_sq_base + offset, instead of the QACR registers' area */
extern uint32_t dc_sq_base;
/* CH2 DMA source address translation (an OS's physical memory that the port keeps elsewhere in RAM) */
extern uint32_t (*dc_dma_src_hook)(uint32_t src);
void     dc_raise(uint32_t istnrm_bits);      /* raise Holly normal interrupts (e.g. render done) */
uint64_t dc_frame_count(void);
uint32_t dc_pvr_reg(uint32_t ofs);
uint32_t dc_sb_reg(uint32_t addr);
#ifdef __cplusplus
}
#endif
#endif

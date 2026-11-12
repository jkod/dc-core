/* dc-core: the Dreamcast's network hardware.
 *
 * The Broadband Adapter (HIT-0400) sits in the expansion port at 0x01000000: a "GAPSPCI" bridge
 * from the G2 bus to PCI, a Realtek RTL8139C on that PCI bus and 32 KB of SRAM the chip moves
 * frames through. Off by default (the port reads as empty, as on a console without one). With
 * dc_bba_enabled set before the game probes it, the game finds the adapter and drives it like the
 * real one; the frames it sends go to dc_net_link.send and the frames the link has received are
 * taken from dc_net_link.recv, on the emulation thread, as the receive ring has room.
 *
 * The runtime does no networking itself: the frontend (dccore_front.h, dc_front_net_*) connects
 * the link to a DCNet server, or anything else that carries Ethernet frames. */
#ifndef DCCORE_NET_H
#define DCCORE_NET_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* a frame the game sent (destination MAC first, no CRC). Must not block. */
    void (*send)(const uint8_t *frame, int len);
    /* the next received frame into buf (up to max bytes): its length, or 0 when none is waiting.
     * Called only while dc_net_rx_waiting is set. */
    int (*recv)(uint8_t *buf, int max);
} dc_net_link;
extern dc_net_link dc_net;
/* set (from any thread) when the link has frames for the game; the runtime looks at it about
 * once a millisecond of emulated time and clears it when the link has none left */
extern volatile int dc_net_rx_waiting;

extern int dc_bba_enabled;          /* the Broadband Adapter is plugged in */
extern uint8_t dc_bba_mac[6];       /* its address (default 00:D0:F1 Sega + 3 random bytes at reset) */
extern int dc_bba_trace;            /* print register accesses and frames to stderr */

void dc_bba_reset(void);            /* power-on state (the HLE boot calls it) */
/* the hardware model's side (hw.c): accesses to 0x01000000-0x01FFFFFF while enabled */
uint32_t dc_bba_read(uint32_t addr, int size);
void dc_bba_write(uint32_t addr, uint32_t v, int size);
void dc_bba_poll(void);            /* move waiting frames into the receive ring */
void dc_bba_state(void (*f)(void *p, size_t n));   /* reset.h: the adapter's state in a snapshot */
/* counters for a run report */
extern uint64_t dc_bba_tx_frames, dc_bba_rx_frames, dc_bba_rx_dropped;

#ifdef __cplusplus
}
#endif
#endif

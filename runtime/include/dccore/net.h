/* dc-core: the Dreamcast's network hardware: the Broadband Adapter or the modem (one at a time).
 *
 * The Broadband Adapter (HIT-0400) sits in the expansion port at 0x01000000: a "GAPSPCI" bridge
 * from the G2 bus to PCI, a Realtek RTL8139C on that PCI bus and 32 KB of SRAM the chip moves
 * frames through. Off by default (the port reads as empty, as on a console without one). With
 * dc_bba_enabled set before the game probes it, the game finds the adapter and drives it like the
 * real one; the frames it sends go to dc_net_link.send and the frames the link has received are
 * taken from dc_net_link.recv, on the emulation thread, as the receive ring has room.
 *
 * The modem (runtime/src/modem.c) is the 33.6k Rockwell modem every console has at 0x00600000.
 * With dc_modem_enabled, the game dials, the far end answers and, once the line is up, the bytes
 * the game sends (its PPP) go to dc_net_link.send and the link's bytes come back at the line's
 * speed. dc_net_link.open is called when the line comes up and dc_net_link.hangup when it drops.
 *
 * The runtime does no networking itself: the frontend (dccore_front.h, dc_front_net_*) connects
 * the link to a DCNet server, or anything else that carries Ethernet frames or a PPP stream. */
#ifndef DCCORE_NET_H
#define DCCORE_NET_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* adapter: a frame the game sent (destination MAC first, no CRC); modem: bytes. Must not block. */
    void (*send)(const uint8_t *frame, int len);
    /* adapter: the next received frame into buf (up to max bytes): its length, or 0 when none is
     * waiting; modem: up to max bytes of the stream. Called only while dc_net_rx_waiting is set. */
    int (*recv)(uint8_t *buf, int max);
    void (*open)(void);             /* modem: the line is up (connect now) */
    void (*hangup)(void);           /* modem: the line dropped (a later call starts a new session) */
} dc_net_link;
extern dc_net_link dc_net;
/* set (from any thread) when the link has frames for the game; the runtime looks at it about
 * once a millisecond of emulated time and clears it when the link has none left */
extern volatile int dc_net_rx_waiting;
/* set by the link when the modem's connection is gone: the modem drops its carrier */
extern volatile int dc_net_carrier_lost;

extern int dc_bba_enabled;          /* the Broadband Adapter is plugged in */
extern uint8_t dc_bba_mac[6];       /* its address (default 00:D0:F1 Sega + 3 random bytes at reset) */
extern int dc_bba_trace;            /* print register accesses and frames to stderr */

void dc_bba_reset(void);            /* power-on state (the HLE boot calls it) */
/* the hardware model's side (hw.c): accesses to 0x01000000-0x01FFFFFF while enabled */
uint32_t dc_bba_read(uint32_t addr, int size);
void dc_bba_write(uint32_t addr, uint32_t v, int size);
void dc_bba_poll(void);            /* move waiting frames into the receive ring */
void dc_bba_state(void (*f)(void *p, size_t n));   /* reset.h: the adapter's state in a snapshot */
extern int dc_modem_enabled;        /* the modem answers (else its registers read as 0, as before) */
extern int dc_modem_country;        /* its ID's country: 1 Japan, 2 USA (default) */
extern int dc_modem_trace;          /* print register writes and line events to stderr */
void dc_modem_reset(void);          /* power-on state (the HLE boot calls it) */
uint32_t dc_modem_read(uint32_t addr, int size);    /* hw.c: 0x00600000-0x006007FF while enabled */
void dc_modem_write(uint32_t addr, uint32_t v, int size);
void dc_modem_events(uint64_t now, uint64_t *next); /* hw.c: the modem's timed steps */
void dc_modem_state(void (*f)(void *p, size_t n));
/* counters for a run report */
extern uint64_t dc_bba_tx_frames, dc_bba_rx_frames, dc_bba_rx_dropped;

#ifdef __cplusplus
}
#endif
#endif

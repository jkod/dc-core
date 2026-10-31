/* dc-core: Maple bus devices -- the standard Dreamcast controller.
 *
 * Maple DMA (hw.c) hands every command frame to on_maple(). Frame word: command (bits 0-7),
 * recipient (8-15: port << 6 | 0x20 main unit, low bits = sub-units), sender (16-23), length in
 * words (24-31). The reply goes to the receive address with the same layout.
 */
#include "dccore/maple.h"
#include "dccore/hw.h"
#include "dccore/runtime.h"
#include <string.h>

dc_pad_state dc_pad[4] = { { .connected = 1, .jx = 128, .jy = 128 },
                           { .jx = 128, .jy = 128 }, { .jx = 128, .jy = 128 }, { .jx = 128, .jy = 128 } };

#define FUNC_CONTROLLER 0x01000000u
enum { CMD_DEVINFO = 1, CMD_ALLINFO = 2, CMD_RESET = 3, CMD_SHUTDOWN = 4,
       RE_DEVINFO = 5, RE_ALLINFO = 6, RE_ACK = 7, RE_DATA = 8, CMD_GETCOND = 9,
       RE_UNKNOWN = 0xFE, RE_BADFUNC = 0xFD };

static void put_str(uint8_t *d, const char *s, int n) {
    int l = (int)strlen(s);
    for (int i = 0; i < n; i++) d[i] = i < l ? (uint8_t)s[i] : ' ';
}

/* reply frame header + payload words */
static void reply(uint32_t recv, int cmd, uint32_t to, uint32_t from, const uint32_t *w, int n) {
    dc_wr32(recv, (uint32_t)cmd | to << 8 | from << 16 | (uint32_t)n << 24);
    for (int i = 0; i < n; i++) dc_wr32(recv + 4 + i * 4, w[i]);
}

static int on_maple(int port, uint32_t frame, uint32_t send, int words, uint32_t recv) {
    (void)words;
    int cmd = frame & 255;
    uint32_t dest = (frame >> 8) & 255, src = (frame >> 16) & 255;
    if (port < 0 || port > 3 || !dc_pad[port].connected) return 0;
    if ((dest & 0x3F) != 0x20) return 0;                       /* sub-unit (VMU, rumble): none */
    uint32_t me = (uint32_t)port << 6 | 0x20;
    switch (cmd) {
    case CMD_DEVINFO: case CMD_ALLINFO: {
        uint8_t b[112 + 80];
        memset(b, 0, sizeof b);
        uint32_t f[4] = { FUNC_CONTROLLER, 0xFE060F00u, 0, 0 };   /* function, buttons/axes present */
        memcpy(b, f, 16);
        b[16] = 0xFF;                                             /* area: all regions */
        b[17] = 0;                                                /* connector direction */
        put_str(b + 18, "Dreamcast Controller", 30);
        put_str(b + 48, "Produced By or Under License From SEGA ENTERPRISES,LTD.", 60);
        b[108] = 0xAE; b[109] = 0x01;                              /* standby 43.0 mA */
        b[110] = 0xF4; b[111] = 0x01;                              /* max 50.0 mA */
        int n = cmd == CMD_DEVINFO ? 28 : 48;
        if (cmd == CMD_ALLINFO) put_str(b + 112, "Version 1.010,1998/09/28,315-6211-AB   ,Analog Module : The 4th Edition.5/8  +DF", 80);
        uint32_t w[48];
        memcpy(w, b, (size_t)n * 4);
        reply(recv, cmd == CMD_DEVINFO ? RE_DEVINFO : RE_ALLINFO, src, me, w, n);
        return 1;
    }
    case CMD_GETCOND: {
        uint32_t func = dc_rd32(send);
        if (func != FUNC_CONTROLLER) { reply(recv, RE_BADFUNC, src, me, NULL, 0); return 1; }
        const dc_pad_state *p = &dc_pad[port];
        uint16_t btn = (uint16_t)~p->buttons;                    /* active low */
        uint32_t w[3];
        w[0] = FUNC_CONTROLLER;
        w[1] = (uint32_t)btn | (uint32_t)p->rt << 16 | (uint32_t)p->lt << 24;
        w[2] = (uint32_t)p->jx | (uint32_t)p->jy << 8 | 0x80u << 16 | 0x80u << 24;   /* second stick centred */
        reply(recv, RE_DATA, src, me, w, 3);
        return 1;
    }
    case CMD_RESET: case CMD_SHUTDOWN:
        reply(recv, RE_ACK, src, me, NULL, 0);
        return 1;
    default:
        reply(recv, RE_UNKNOWN, src, me, NULL, 0);
        return 1;
    }
}

void dc_maple_install(void) { dc_hw.on_maple = on_maple; }

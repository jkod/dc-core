#ifndef DCCORE_MAPLE_H
#define DCCORE_MAPLE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Maple bus devices: a standard controller on each connected port (maple.c). A frontend fills
 * dc_pad[] from the host's gamepad/keyboard; the game reads it with GET_CONDITION. */
enum {
    DC_BTN_C = 1 << 0, DC_BTN_B = 1 << 1, DC_BTN_A = 1 << 2, DC_BTN_START = 1 << 3,
    DC_BTN_UP = 1 << 4, DC_BTN_DOWN = 1 << 5, DC_BTN_LEFT = 1 << 6, DC_BTN_RIGHT = 1 << 7,
    DC_BTN_Z = 1 << 8, DC_BTN_Y = 1 << 9, DC_BTN_X = 1 << 10, DC_BTN_D = 1 << 11,
};
typedef struct {
    int connected;
    uint16_t buttons;          /* DC_BTN_* pressed = 1 (the bus reports them active-low) */
    uint8_t lt, rt;            /* triggers 0 (released) .. 255 */
    uint8_t jx, jy;            /* stick, 128 = centre */
} dc_pad_state;
extern dc_pad_state dc_pad[4];
void dc_maple_install(void);   /* sets dc_hw.on_maple; port A connected by default */
/* VMU in the first slot of controller `port`, stored in a 128 KB file (the common .bin dump
 * layout, interchangeable with other emulators). A missing file becomes a freshly formatted
 * card; writes go straight to the file. path NULL removes the card. 0 = ok. */
int dc_vmu_attach(int port, const char *path);
const uint8_t *dc_vmu_data(int port);
void dc_vmu_format(uint8_t *data);       /* 128 KB blank, formatted card */
/* Jump Pack (Puru Puru Pack) in the second slot of controller `port`: games see it and send rumble
 * effects. Call dc_rumble_level once per field (vblank): the vibration now, 0..1, for the host's
 * low-frequency (big) and high-frequency (small) motors; it returns non-zero while vibrating. */
int dc_rumble_attach(int port, int on);
int dc_rumble_level(int port, float *low, float *high);
#ifdef __cplusplus
}
#endif
#endif

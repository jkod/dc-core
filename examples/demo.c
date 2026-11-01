/* dc-core frontend demo: a spinning triangle drawn through the PowerVR2 model (TA list ->
 * renderer -> window), no game code. Buttons change the background: A red, B green, X blue,
 * Y white; the stick moves the triangle. Esc (desktop) or + and - (Switch) quits.
 * A quick end-to-end check of the frontend on a new platform. */
#include "dccore/runtime.h"
#include "dccore/hw.h"
#include "dccore/maple.h"
#include "dccore_front.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#define SDL_MAIN_HANDLED          /* plain main() on every platform */
#include <SDL.h>

/* no recompiled game: empty function table */
typedef struct { uint32_t addr, end; sh4fn fn; } dc_fn_entry;
const dc_fn_entry dc_fn_table[1] = {{0, 0, 0}};
const size_t dc_fn_count = 0;

static void pvr(uint32_t ofs, uint32_t v) { dc_wr32(0xA05F8000u + ofs, v); }
static void ta(const uint32_t w[8]) { for (int i = 0; i < 8; i++) dc_wr32(0x10000000u + i * 4, w[i]); }
static uint32_t fu(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

int main(int argc, char **argv) {
    static sh4ctx c;
    dc_init(&c);
    dc_front_config cfg = { .title = "dc-core demo", .scale = 2, .no_vmu = 1, .no_audio = 1 };
    for (int i = 1; i + 1 < argc; i++) {                     /* headless checks: --frames N --shot F.bmp */
        if (!strcmp(argv[i], "--frames")) cfg.exit_after = (unsigned long)atol(argv[++i]);
        else if (!strcmp(argv[i], "--shot")) cfg.shot_path = argv[++i];
    }
    if (dc_front_open(&cfg)) return 1;
    pvr(0x068, 639u << 16); pvr(0x06C, 479u << 16);           /* 640x480 */
    pvr(0x020, 0); pvr(0x08C, (0x1000 / 4) << 3); pvr(0x088, fu(0.0f));
    float px = 320, py = 240;
    for (uint64_t frame = 1;; frame++) {
        const dc_pad_state *p = &dc_pad[0];
        uint32_t bg = 0xFF203040u;
        if (p->buttons & DC_BTN_A) bg = 0xFFC02020u;
        if (p->buttons & DC_BTN_B) bg = 0xFF20C020u;
        if (p->buttons & DC_BTN_X) bg = 0xFF2020C0u;
        if (p->buttons & DC_BTN_Y) bg = 0xFFC0C0C0u;
        px += (p->jx - 128) / 16.f; py += (p->jy - 128) / 16.f;
        if (px < 0) px = 0; if (px > 640) px = 640; if (py < 0) py = 0; if (py > 480) py = 480;
        uint32_t bgp[] = { 0, 0, 0, fu(0), fu(0), fu(1), bg };
        for (int i = 0; i < 7; i++) dc_wr32(0xA5000000u + 0x1000 + i * 4, bgp[i]);
        pvr(0x128, 0); pvr(0x144, 0x80000000u);              /* new display list */
        uint32_t hdr[8] = { (4u << 29) | (1u << 1), (7u << 29), (1u << 29) | (2u << 22), 0, 0, 0, 0, 0 };
        ta(hdr);
        float a = (float)frame * 0.03f;
        static const uint32_t col[3] = { 0xFFFF4040u, 0xFF40FF40u, 0xFF4040FFu };
        for (int k = 0; k < 3; k++) {
            float ang = a + (float)k * 2.0943951f;
            uint32_t v[8] = { (7u << 29) | (k == 2 ? 1u << 28 : 0), fu(px + 150 * cosf(ang)), fu(py + 150 * sinf(ang)), fu(1), 0, 0, col[k], 0 };
            ta(v);
        }
        uint32_t eol[8] = { 0 };
        ta(eol);
        pvr(0x014, 1);                                         /* STARTRENDER */
        dc_hw.on_vblank(frame);                                /* present, input */
        SDL_Delay(16);
    }
}

/* dc-core runtime tests (no game code): TA parser, DMA engines, maple, delayed interrupts.
 *   cc -Iruntime/include tests/test_runtime.c runtime/src/*.c -lm -o test_runtime && ./test_runtime */
#include "dccore/runtime.h"
#include "dccore/hw.h"
#include "dccore/ta.h"
#include "dccore/aica.h"
#include <stdio.h>
#include <string.h>

typedef struct { uint32_t addr, end; sh4fn fn; } dc_fn_entry;
const dc_fn_entry dc_fn_table[1] = {{0, 0, 0}};
const size_t dc_fn_count = 0;

static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); fails++; } } while (0)
static int nparams, nvtx, last_list;
static void sink(int kind, int list, const uint32_t *p, int words) { nparams++; if (kind == 7) nvtx++; last_list = list; }
static void run(sh4ctx *c, uint64_t cycles) { c->cycles += cycles; dc_events(c); }

static void blk(uint32_t pcw) { uint32_t b[8] = {pcw}; dc_ta_write32(b); }

int main(void) {
    sh4ctx c; dc_init(&c); dc_hw_verbose = 0;
    dc_ta_set_sink(sink);
    uint32_t IST = 0xA05F6900u;

    /* 1. opaque list: packed-colour polygon, 3 vertices, end of list -> ISTNRM bit 7 */
    dc_ta_reset();
    blk(0x80000000u | (0u << 24));                 /* polygon, list 0 */
    blk(0xE0000000u); blk(0xE0000000u); blk(0xF0000000u);
    CHECK(!(dc_rd32(IST) & (1u << 7)));
    blk(0x00000000u);
    CHECK(dc_rd32(IST) & (1u << 7));
    CHECK(nvtx == 3 && last_list == 0);
    dc_wr32(IST, 1u << 7); CHECK(!(dc_rd32(IST) & (1u << 7)));

    /* 2. translucent, textured float colour -> 64-byte vertices: an all-zero second half must not end the list */
    nvtx = 0;
    blk(0x80000000u | (2u << 24) | (1u << 3) | (1u << 4));
    blk(0xE0000000u); blk(0x00000000u);           /* vertex, second half looks like "end of list" */
    blk(0xF0000000u); blk(0x00000000u);
    CHECK(!(dc_rd32(IST) & (1u << 9)));
    blk(0x00000000u);                               /* real end of list */
    CHECK(dc_rd32(IST) & (1u << 9));
    CHECK(nvtx == 2);

    /* 3. CH2 DMA of a polygon list into the TA FIFO: list-end + DMA-end (delayed) interrupts */
    dc_wr32(IST, 0xFFFFFFFFu); dc_ta_reset(); nvtx = 0;
    uint32_t src = 0x8C200000u;
    uint32_t words[] = {0x80000000u | (4u << 24), 0xE0000000u, 0xF0000000u, 0};
    for (int i = 0; i < 4; i++) { memset(dc_ram + 0x200000 + i * 32, 0, 32); memcpy(dc_ram + 0x200000 + i * 32, &words[i], 4); }
    dc_wr32(0xFFA00020u, src);                       /* SAR2 */
    dc_wr32(0xA05F6800u, 0x10000000u); dc_wr32(0xA05F6804u, 128); dc_wr32(0xA05F6808u, 1);
    CHECK(nvtx == 2 && (dc_rd32(IST) & (1u << 21)));  /* punch-through list end */
    CHECK(!(dc_rd32(IST) & (1u << 19)));             /* DMA end not yet */
    run(&c, 100000);
    CHECK(dc_rd32(IST) & (1u << 19));
    CHECK(dc_rd32(0xA05F6808u) == 0);

    /* 4. maple: two-port command list, no devices -> 0xFFFFFFFF replies + maple DMA end */
    uint32_t list = 0x8C300000u, r0 = 0x8C310000u, r1 = 0x8C310100u;
    dc_wr32(list + 0, (0u << 16) | 0); dc_wr32(list + 4, r0); dc_wr32(list + 8, 0x01200001u);
    dc_wr32(list + 12, 0x80000000u | (1u << 16) | 0); dc_wr32(list + 16, r1); dc_wr32(list + 20, 0x01600001u);
    dc_wr32(r0, 0); dc_wr32(r1, 0);
    dc_wr32(0xA05F6C04u, list); dc_wr32(0xA05F6C14u, 1); dc_wr32(0xA05F6C18u, 1);
    CHECK(dc_rd32(r0) == 0xFFFFFFFFu && dc_rd32(r1) == 0xFFFFFFFFu);
    run(&c, 100000);
    CHECK(dc_rd32(IST) & (1u << 12));

    /* 5. G2 DMA to sound RAM */
    for (int i = 0; i < 16; i++) dc_wr32(0x8C400000u + i * 4, 0x11110000u + i);
    dc_wr32(0xA05F7800u, 0xA0800000u); dc_wr32(0xA05F7804u, 0x8C400000u); dc_wr32(0xA05F7808u, 64);
    dc_wr32(0xA05F780Cu, 0); dc_wr32(0xA05F7814u, 1); dc_wr32(0xA05F7818u, 1);
    CHECK(dc_rd32(0xA080003Cu) == 0x1111000Fu);
    run(&c, 100000);
    CHECK(dc_rd32(IST) & (1u << 15));

    /* 6. AICA: upload an ARM program, release ARMRST; it writes a mailbox in sound RAM and raises
     *    MCIPD bit 5 (ARM -> SH-4), which must reach Holly ISTEXT bit 1. Timer A must then count. */
    static const uint8_t armprog[] = { 0x20,0x00,0x9F,0xE5,0x42,0x10,0xA0,0xE3,0x00,0x10,0x80,0xE5,0x18,0x20,0x9F,0xE5,0x20,0x30,0xA0,0xE3,0x00,0x30,0x82,0xE5,0x10,0x40,0x9F,0xE5,0x00,0x50,0xA0,0xE3,0x00,0x50,0x84,0xE5,0xFE,0xFF,0xFF,0xEA,0x00,0x10,0x00,0x00,0xB8,0x28,0x80,0x00,0x90,0x28,0x80,0x00 };
    dc_wr32(0xA0702C00u, 1);
    for (unsigned i = 0; i < sizeof armprog; i += 4) { uint32_t w; memcpy(&w, armprog + i, 4); dc_wr32(0xA0800000u + i, w); }
    dc_wr32(0xA0801000u, 0);
    dc_wr32(0xA07028B4u, 0x20);                       /* MCIEB: CPU interrupt */
    dc_wr32(0xA0702C00u, 0);                          /* release the ARM */
    run(&c, 2000000);                                 /* 10 ms */
    CHECK(dc_rd32(0xA0801000u) == 0x42);
    CHECK(dc_rd32(0xA07028B8u) & 0x20);
    CHECK(dc_rd32(0xA05F6904u) & 2);                  /* ISTEXT: AICA */
    dc_wr32(0xA07028BCu, 0x20);                       /* MCIRE */
    run(&c, 10000);
    CHECK(!(dc_rd32(0xA05F6904u) & 2));
    CHECK((dc_rd32(0xA0702890u) & 0xFF) != 0);        /* timer A counted */

    printf(fails ? "%d runtime test(s) FAILED\n" : "runtime tests passed\n", fails);
    return fails != 0;
}

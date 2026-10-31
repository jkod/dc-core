/* dc-core: Tile Accelerator input parser.
 *
 * The game streams 32-byte blocks to the TA FIFO (store queues or CH2 DMA). This parser walks
 * the parameter stream, keeps the polygon/vertex state a renderer needs and raises the
 * "list end" interrupts the game waits for. A renderer subscribes with dc_ta_set_sink(); each
 * parameter is passed on with its decoded size and the list it belongs to.
 */
#include "dccore/runtime.h"
#include "dccore/hw.h"
#include "dccore/ta.h"
#include <string.h>

/* list types (global param bits 26..24) -> ISTNRM "end of transfer" bits */
static const uint32_t list_end_bit[5] = {1u << 7, 1u << 8, 1u << 9, 1u << 10, 1u << 21};

static struct {
    int list;            /* current list type, -1 = none open */
    int vtx64;           /* vertex parameters are 64 bytes */
    int pending;         /* second half of a 64-byte parameter expected */
    uint32_t buf[16];
} ta;

static dc_ta_sink sink;
void dc_ta_set_sink(dc_ta_sink s) { sink = s; }

void dc_ta_reset(void) { ta.list = -1; ta.pending = 0; ta.vtx64 = 0; if (sink) sink(-1, -1, NULL, 0); }   /* -1: TA_LIST_INIT */

/* polygon global parameter size and vertex size from the parameter control word */
static void poly_format(uint32_t pcw, int *hdr64, int *vtx64) {
    int tex = (pcw >> 3) & 1, col = (pcw >> 4) & 3, vol = (pcw >> 6) & 1, uv16 = pcw & 1, ofs = (pcw >> 2) & 1;
    *hdr64 = (col == 2 && (vol || ofs));                        /* polygon types 2 and 4 */
    if (!tex) *vtx64 = 0;
    else if (!vol) *vtx64 = (col == 1);                         /* textured float colour: types 5/6 */
    else *vtx64 = 1;                                            /* two volumes, textured: 11..14 */
    (void)uv16;
}

static void emit(int kind, const uint32_t *p, int words) { if (sink) sink(kind, ta.list, p, words); }

void dc_ta_write32(const uint32_t blk[8]) {
    if (ta.pending) {                       /* second half of a 64-byte parameter */
        memcpy(ta.buf + 8, blk, 32);
        ta.pending = 0;
        emit((int)(ta.buf[0] >> 29), ta.buf, 16);
        return;
    }
    uint32_t pcw = blk[0];
    int type = (int)(pcw >> 29);
    switch (type) {
    case 0:                                 /* end of list */
        if (ta.list >= 0 && ta.list < 5) dc_raise(list_end_bit[ta.list]);
        emit(0, blk, 8);
        ta.list = -1;
        return;
    case 1: case 2:                          /* user tile clip, object list set */
        emit(type, blk, 8);
        return;
    case 4: case 5: {                        /* polygon / modifier volume header, sprite header */
        if (ta.list < 0) ta.list = (int)((pcw >> 24) & 7);
        int hdr64 = 0, v64 = 0;
        if (type == 5) { v64 = 1; }                                   /* sprite vertices are 64 bytes */
        else if (ta.list == 1 || ta.list == 3) { v64 = 1; }           /* modifier volume triangles */
        else poly_format(pcw, &hdr64, &v64);
        ta.vtx64 = v64;
        if (hdr64) { memcpy(ta.buf, blk, 32); ta.pending = 1; return; }
        emit(type, blk, 8);
        return;
    }
    case 7:                                  /* vertex */
        if (ta.vtx64) { memcpy(ta.buf, blk, 32); ta.pending = 1; return; }
        emit(7, blk, 8);
        return;
    default:
        emit(type, blk, 8);
        return;
    }
}

/* any byte stream (DMA) -> 32-byte blocks */
void dc_ta_write(const uint8_t *p, uint32_t len) {
    uint32_t blk[8];
    for (uint32_t o = 0; o + 32 <= len; o += 32) { memcpy(blk, p + o, 32); dc_ta_write32(blk); }
}

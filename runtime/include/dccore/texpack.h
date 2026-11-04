#ifndef DCCORE_TEXPACK_H
#define DCCORE_TEXPACK_H
/* dc-core texture packs.
 *
 * Textures are named by a hash of their decoded pixels (0xAARRGGBB) and size: <hash>_<w>x<h>.png.
 * The renderer dumps (dc_pvr_gl_tex_dump) and replaces (dc_pvr_gl_tex_pack) textures by that name.
 *
 * Extraction writes the disc's textures under those same names without playing:
 * dc_tex_extract_disc() walks every file on the (open) disc, decodes any PVRT texture it finds (Sega's
 * standard format, alone or inside archives) with the renderer's decoder, and hands each file to the
 * port's unpacker for the game's own containers. The unpacker passes what it finds on:
 *   dc_tex_extract_pvr   raw PVR texture data, described by a texture control word (format bits 27-29,
 *                        scan order 26, VQ 30, mipmaps 31; the address bits are ignored) and its size;
 *                        mipmapped data holds the smaller levels first, as the hardware wants it
 *   dc_tex_extract_argb  pixels already decoded (0xAARRGGBB, row by row), exactly as the game's uploads
 *                        decode, or the names will not match
 * Paletted textures are skipped: their colours are set by the game while it runs; dump those by
 * playing (--dump-textures). */
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct dc_tex_extract dc_tex_extract;
typedef void (*dc_tex_unpacker)(dc_tex_extract *x, const char *path, const uint8_t *data, size_t len, void *user);

long dc_tex_extract_disc(const char *dir, dc_tex_unpacker game, void *user);   /* new PNGs written, -1 on error */
int  dc_tex_extract_pvr(dc_tex_extract *x, const void *data, size_t len, uint32_t tcw, int w, int h);
int  dc_tex_extract_argb(dc_tex_extract *x, const uint32_t *argb, int w, int h);
void dc_tex_extract_skip(dc_tex_extract *x);                /* count one the unpacker cannot decode */
const char *dc_tex_extract_file(const dc_tex_extract *x);   /* the file being unpacked */

/* the naming and PNG files themselves */
uint64_t dc_tex_hash(const uint32_t *argb, int w, int h);
int      dc_tex_save(const char *dir, uint64_t hash, const uint32_t *argb, int w, int h);   /* 1 new, 0 already there, -1 */
uint8_t *dc_tex_load(const char *dir, uint64_t hash, int w, int h, int *rw, int *rh);       /* RGBA bytes or NULL */
void     dc_tex_free(uint8_t *rgba);
#ifdef __cplusplus
}
#endif
#endif

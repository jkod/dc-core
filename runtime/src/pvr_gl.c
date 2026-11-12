/* dc-core: PowerVR2 frames on OpenGL 3.3 core.
 *
 * Draws the polygons the TA collected (pvr_int.h) at any multiple of the console resolution into
 * an offscreen framebuffer; the frontend presents it. Matches the software renderer (pvr_sw.c,
 * the reference, checked by tests/test_pvr_gl.c):
 *  - x/y in PVR screen units, z = 1/w: varyings are perspective-correct through gl_Position.w,
 *    depth is log2(1/w) mapped to [0,1] in the fragment shader, so the PVR's depth compare modes
 *    ("greater" = closer) map onto GL's directly;
 *  - shading instructions, offset colour, fog (table / vertex / table mode 2) and the
 *    punch-through alpha test in the fragment shader; textures come decoded from pvr_sw.c;
 *  - translucent polygons sorted back to front unless the game presorts;
 *  - modifier volumes with the stencil buffer: bit 1 marks surfaces that volumes affect, bit 0
 *    counts volume triangles in front (parity), bit 2 accumulates "inside"; then cheap shadow
 *    multiplies, or two-volume polygons are redrawn with their second parameter set.
 * GL functions are loaded through the getproc the caller passes (SDL, EGL, ...).
 */
#include "dccore/pvr.h"
#include "dccore/hw.h"
#include "pvr_int.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <sys/stat.h>   /* first: newlib (Switch) #defines quad, used as a name below */
#include "dccore/texpack.h"

/* Drawing on another thread (dc_pvr_gl_prepare): what a frame reads of the console -- the TA's lists,
 * the PowerVR registers, decoded textures -- is taken on the game's thread at STARTRENDER, and the
 * drawing reads only that, while the game goes on. */
static uint32_t snap_reg[0x2000 / 4]; static int snap_on;
static inline uint32_t preg(uint32_t o) { return snap_on ? snap_reg[(o & 0x1FFC) >> 2] : (dc_pvr_reg)(o); }
#define dc_pvr_reg(o) preg(o)
enum { TMAP = 2048 };                    /* (tcw, tsp size) -> the texture, decoded at prepare */
static struct { uint32_t tcw, tsz, stamp; pvr_texinfo ti; } tmap[TMAP];
static uint32_t tmap_stamp; static int tmap_on, tmap_n, tmap_miss;
static unsigned tmap_slot(uint32_t tcw, uint32_t tsz) { uint32_t h = tcw * 2654435761u ^ tsz * 40503u; return (h ^ (h >> 15)) & (TMAP - 1); }
static int tmap_add(uint32_t tcw, uint32_t tsp) {
    uint32_t tsz = tsp & 0x3F;
    for (unsigned i = tmap_slot(tcw, tsz), k = 0; k < TMAP; k++, i = (i + 1) & (TMAP - 1)) {
        if (tmap[i].stamp != tmap_stamp) {
            tmap[i].stamp = tmap_stamp; tmap[i].tcw = tcw; tmap[i].tsz = tsz;
            pvr_get_texture(tcw, tsp, &tmap[i].ti);
            return ++tmap_n;
        }
        if (tmap[i].tcw == tcw && tmap[i].tsz == tsz) return tmap_n;
    }
    return 1 << 30;
}
static void tex_get(uint32_t tcw, uint32_t tsp, pvr_texinfo *ti) {
    if (tmap_on) {
        uint32_t tsz = tsp & 0x3F;
        for (unsigned i = tmap_slot(tcw, tsz), k = 0; k < TMAP; k++, i = (i + 1) & (TMAP - 1)) {
            if (tmap[i].stamp != tmap_stamp) break;
            if (tmap[i].tcw == tcw && tmap[i].tsz == tsz) { *ti = tmap[i].ti; return; }
        }
        tmap_miss++;                                         /* not seen at prepare: decoded here after all */
    }
    pvr_get_texture(tcw, tsp, ti);
}
static pvr_frame snap_f;

/* ------------------------------------------------------------------ minimal GL binding */
typedef unsigned int GLenum, GLuint, GLbitfield; typedef int GLint, GLsizei; typedef float GLfloat;
typedef unsigned char GLboolean, GLubyte; typedef char GLchar; typedef ptrdiff_t GLsizeiptr, GLintptr; typedef double GLdouble;
#define GL_FALSE 0
#define GL_TRUE 1
#define GL_TRIANGLES 0x0004
#define GL_NEVER 0x0200
#define GL_ALWAYS 0x0207
#define GL_EQUAL 0x0202
#define GL_GEQUAL 0x0206
#define GL_ZERO 0
#define GL_ONE 1
#define GL_SRC_COLOR 0x0300
#define GL_ONE_MINUS_SRC_COLOR 0x0301
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_DST_ALPHA 0x0304
#define GL_ONE_MINUS_DST_ALPHA 0x0305
#define GL_DST_COLOR 0x0306
#define GL_ONE_MINUS_DST_COLOR 0x0307
#define GL_CONSTANT_COLOR 0x8001
#define GL_DEPTH_TEST 0x0B71
#define GL_STENCIL_TEST 0x0B90
#define GL_BLEND 0x0BE2
#define GL_SCISSOR_TEST 0x0C11
#define GL_CULL_FACE 0x0B44
#define GL_KEEP 0x1E00
#define GL_REPLACE 0x1E01
#define GL_INVERT 0x150A
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_TEXTURE_MAX_ANISOTROPY 0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY 0x84FF
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_REPEAT 0x2901
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_MIRRORED_REPEAT 0x8370
#define GL_RGBA 0x1908
#define GL_RGBA8 0x8058
#define GL_RGB 0x1907
#define GL_BGRA 0x80E1
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FLOAT 0x1406
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#define GL_DEPTH24_STENCIL8 0x88F0
#define GL_DEPTH32F_STENCIL8 0x8CAD
#define GL_DEPTH_COMPONENT32F 0x8CAC
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_LOWER_LEFT 0x8CA1
#define GL_NEGATIVE_ONE_TO_ONE 0x935E
#define GL_ZERO_TO_ONE 0x935F
#define GL_MAX_SAMPLES 0x8D57
#define GL_SAMPLE_ALPHA_TO_COVERAGE 0x809E
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_DEPTH_BUFFER_BIT 0x0100
#define GL_STENCIL_BUFFER_BIT 0x0400
#define GL_ARRAY_BUFFER 0x8892
#define GL_STREAM_DRAW 0x88E0
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_RG_INTEGER 0x8228
#define GL_RG8UI 0x8238
#define GL_TEXTURE1 0x84C1

#define GLFUNCS \
    F(void, glGenFramebuffers, (GLsizei, GLuint *)) F(void, glBindFramebuffer, (GLenum, GLuint)) \
    F(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    F(void, glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint)) F(void, glGenRenderbuffers, (GLsizei, GLuint *)) \
    F(void, glBindRenderbuffer, (GLenum, GLuint)) F(void, glRenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei)) \
    F(void, glRenderbufferStorageMultisample, (GLenum, GLsizei, GLenum, GLsizei, GLsizei)) F(void, glGetIntegerv, (GLenum, GLint *)) \
    F(GLenum, glCheckFramebufferStatus, (GLenum)) F(void, glDeleteFramebuffers, (GLsizei, const GLuint *)) \
    F(void, glDeleteRenderbuffers, (GLsizei, const GLuint *)) F(void, glGenTextures, (GLsizei, GLuint *)) \
    F(void, glDeleteTextures, (GLsizei, const GLuint *)) F(void, glBindTexture, (GLenum, GLuint)) \
    F(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    F(void, glTexParameteri, (GLenum, GLenum, GLint)) F(void, glActiveTexture, (GLenum)) \
    F(void, glViewport, (GLint, GLint, GLsizei, GLsizei)) F(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
    F(void, glClearDepth, (GLdouble)) F(void, glClearStencil, (GLint)) F(void, glClear, (GLbitfield)) \
    F(void, glScissor, (GLint, GLint, GLsizei, GLsizei)) \
    F(void, glGenerateMipmap, (GLenum)) F(void, glTexParameterf, (GLenum, GLenum, GLfloat)) F(void, glGetFloatv, (GLenum, GLfloat *)) \
    F(void, glMultiDrawArrays, (GLenum, const GLint *, const GLsizei *, GLsizei)) \
    F(void, glTexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
    F(void, glEnable, (GLenum)) F(void, glDisable, (GLenum)) F(void, glDepthFunc, (GLenum)) \
    F(void, glDepthMask, (GLboolean)) F(void, glColorMask, (GLboolean, GLboolean, GLboolean, GLboolean)) \
    F(void, glStencilFunc, (GLenum, GLint, GLuint)) F(void, glStencilOp, (GLenum, GLenum, GLenum)) \
    F(void, glStencilMask, (GLuint)) F(void, glBlendFunc, (GLenum, GLenum)) \
    F(void, glBlendColor, (GLfloat, GLfloat, GLfloat, GLfloat)) F(GLuint, glCreateShader, (GLenum)) \
    F(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) F(void, glCompileShader, (GLuint)) \
    F(void, glGetShaderiv, (GLuint, GLenum, GLint *)) F(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    F(GLuint, glCreateProgram, (void)) F(void, glAttachShader, (GLuint, GLuint)) F(void, glLinkProgram, (GLuint)) \
    F(void, glGetProgramiv, (GLuint, GLenum, GLint *)) F(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    F(void, glUseProgram, (GLuint)) F(GLint, glGetUniformLocation, (GLuint, const GLchar *)) \
    F(void, glUniform1i, (GLint, GLint)) F(void, glUniform1f, (GLint, GLfloat)) F(void, glUniform2f, (GLint, GLfloat, GLfloat)) \
    F(void, glUniform3f, (GLint, GLfloat, GLfloat, GLfloat)) F(void, glUniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat)) \
    F(void, glUniform2fv, (GLint, GLsizei, const GLfloat *)) F(void, glGenVertexArrays, (GLsizei, GLuint *)) \
    F(void, glBindVertexArray, (GLuint)) F(void, glGenBuffers, (GLsizei, GLuint *)) F(void, glBindBuffer, (GLenum, GLuint)) \
    F(void, glBufferData, (GLenum, GLsizeiptr, const void *, GLenum)) \
    F(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
    F(void, glEnableVertexAttribArray, (GLuint)) F(void, glDrawArrays, (GLenum, GLint, GLsizei)) \
    F(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)) \
    F(void, glBlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)) \
    F(void, glPixelStorei, (GLenum, GLint)) F(GLenum, glGetError, (void)) \
    F(void, glDeleteProgram, (GLuint)) F(void, glDeleteShader, (GLuint))
#define F(ret, name, args) static ret (*p_##name) args;
GLFUNCS
#undef F

/* DC_GL_DEBUG=1: the driver's own messages (and the GL in use) go to dc_gl_debug.txt in the
 * current folder, for reports from machines we cannot test on */
#ifdef _WIN32
#define GLAPIENTRY_ __stdcall
#else
#define GLAPIENTRY_
#endif
typedef void (GLAPIENTRY_ *gl_debug_cb)(GLenum, GLenum, GLuint, GLenum, GLsizei, const GLchar *, const void *);
static FILE *dbg_log; static int dbg_count;
static void GLAPIENTRY_ on_gl_debug(GLenum src, GLenum type, GLuint id, GLenum sev, GLsizei len, const GLchar *msg, const void *user) {
    (void)len; (void)user;
    if (!dbg_log || dbg_count >= 400) return;
    fprintf(dbg_log, "[src %04X type %04X id %u sev %04X] %s\n", src, type, id, sev, msg);
    fflush(dbg_log);
    if (++dbg_count == 400) fprintf(dbg_log, "(more messages suppressed)\n");
}
static void (*p_getteximage)(GLenum, GLint, GLenum, GLenum, void *);
static int dbg_tex_checked, dbg_tex_bad;
/* debug: read the texture just uploaded back from the GPU and compare (first 300 uploads) */
static void verify_upload(const pvr_texinfo *ti, uint32_t tcw, uint32_t tsp) {
    if (!p_getteximage || dbg_tex_checked >= 300) return;
    size_t n = (size_t)ti->w * ti->h;
    uint32_t *back = malloc(n * 4);
    if (!back) return;
    memset(back, 0xCD, n * 4);
    p_getteximage(0x0DE1 /* TEXTURE_2D */, 0, 0x80E1 /* BGRA */, 0x1401 /* UNSIGNED_BYTE */, back);
    size_t bad = 0, first = 0;
    for (size_t i = 0; i < n; i++) if (back[i] != ti->px[i]) { if (!bad) first = i; bad++; }
    dbg_tex_checked++;
    if (bad) {
        dbg_tex_bad++;
        fprintf(dbg_log, "texture %dx%d tcw %08X tsp %08X: %zu of %zu texels differ, first at (%d,%d): sent %08X got %08X\n",
                ti->w, ti->h, tcw, tsp, bad, n, (int)(first % ti->w), (int)(first / ti->w), ti->px[first], back[first]);
    }
    if (dbg_tex_checked == 300 || (bad && dbg_tex_bad <= 3) || dbg_tex_checked % 50 == 0)
        fprintf(dbg_log, "textures checked %d, differing %d\n", dbg_tex_checked, dbg_tex_bad);
    fflush(dbg_log);
    free(back);
}
static void *(*p_getproc_saved)(const char *);
static void gl_debug_init(void *(*getproc)(const char *)) {
    p_getproc_saved = getproc;
    { extern int dc_pvr_gl_gpu_bars; const char *a = getenv("DC_GPU_BARS"); if (a) dc_pvr_gl_gpu_bars = *a == '1'; }
    const char *e = getenv("DC_GL_DEBUG");
    if (!e || *e != '1') return;
    dbg_log = fopen("dc_gl_debug.txt", "w");
    if (!dbg_log) return;
    const GLubyte *(*getstr)(GLenum) = (const GLubyte *(*)(GLenum))getproc("glGetString");
    p_getteximage = (void (*)(GLenum, GLint, GLenum, GLenum, void *))getproc("glGetTexImage");
    fprintf(dbg_log, "dc-core GL diagnostics v3; texture read-back %s\n", p_getteximage ? "available" : "NOT available");
    if (getstr) fprintf(dbg_log, "GL_VENDOR %s\nGL_RENDERER %s\nGL_VERSION %s\nGLSL %s\n", getstr(0x1F00), getstr(0x1F01), getstr(0x1F02), getstr(0x8B8C));
    void (*cb)(gl_debug_cb, const void *) = (void (*)(gl_debug_cb, const void *))getproc("glDebugMessageCallback");
    void (*en)(GLenum) = (void (*)(GLenum))getproc("glEnable");
    if (cb && en) { en(0x92E0 /* DEBUG_OUTPUT */); en(0x8242 /* DEBUG_OUTPUT_SYNCHRONOUS */); cb(on_gl_debug, NULL); fprintf(dbg_log, "debug output on\n"); }
    else fprintf(dbg_log, "no glDebugMessageCallback\n");
    fflush(dbg_log);
}

/* ------------------------------------------------------------------ shaders */
/* Perspective is done here, not by GL: positions go out with w = 1 and every interpolated value
 * is sent premultiplied by 1/w (noperspective), then divided by the interpolated 1/w per pixel.
 * Drivers then interpolate plainly in screen space, clipping included; GL's own perspective
 * correction with w = 1/(1/w) gave wrong texture coordinates on NVIDIA whenever 1/w was far from 1. */
/* One program per render state in use (a variant, sv_*): what the PVR's instruction words select -- textured,
 * the shading mode, fog, punch-through, ... -- is fixed when the program is built (#if on the names
 * below) where one program tested it all per pixel through uniforms. Per pixel is where the Switch's
 * GPU spends a frame (a few thousand triangles, but every pixel several times over), and a variant
 * runs a fraction of the instructions. A game uses a few dozen of them; each is built when first
 * needed, and the ones a game has used are built at start-up next time (dc_pvr_gl_shader_list). */
static const char *vs_body =
"layout(location=0) in vec3 a_pos;\n"            /* PVR x, y, 1/w */
"layout(location=1) in vec4 a_uv;\n"             /* u v (set 0), u v (set 1) */
"layout(location=2) in vec4 a_c;  layout(location=3) in vec4 a_o;\n"
"layout(location=4) in vec4 a_c1; layout(location=5) in vec4 a_o1;\n"
"uniform vec2 u_scale; uniform float u_offx;\n"
"noperspective out vec2 v_uvz; noperspective out vec4 v_cz; noperspective out vec4 v_oz; noperspective out float v_invw;\n"
"flat out vec4 f_c; flat out vec4 f_o;\n"
"invariant gl_Position;\n"                       /* the same in every variant: a polygon's passes meet at equal depth */
"void main() {\n"
"#if QUAD\n"
"  gl_Position = vec4(a_pos.xy, 0.0, 1.0); v_invw = 1.0; v_uvz = vec2(0.0); v_cz = vec4(0.0); v_oz = vec4(0.0); f_c = vec4(0.0); f_o = vec4(0.0);\n"
"#else\n"
"  float z = max(a_pos.z, 1e-30);\n"
"  vec2 ndc = vec2((a_pos.x + u_offx) * u_scale.x - 1.0, 1.0 - a_pos.y * u_scale.y);\n"
"#if ZV\n"                                       /* depth from the vertices: see zv_on */
"  gl_Position = vec4(ndc, min(z * 5.421010862427522e-20, 1.0), 1.0);\n"
"#else\n"
"  gl_Position = vec4(ndc, 0.0, 1.0);\n"
"#endif\n"
"#if SET\n"
"  vec2 uv = a_uv.zw; vec4 c = a_c1, o = a_o1;\n"
"#else\n"
"  vec2 uv = a_uv.xy; vec4 c = a_c, o = a_o;\n"
"#endif\n"
"  v_uvz = uv * z; v_cz = c * z; v_oz = o * z; v_invw = z; f_c = c; f_o = o;\n"
"#endif\n"
"}\n";

static const char *fs_body =
"noperspective in vec2 v_uvz; noperspective in vec4 v_cz; noperspective in vec4 v_oz; noperspective in float v_invw;\n"
"flat in vec4 f_c; flat in vec4 f_o;\n"
"uniform sampler2D u_tex; uniform usampler2D u_fogtab;\n"
"uniform vec4 u_quad_col;\n"
"uniform float u_pt_ref; uniform vec3 u_fog_ram, u_fog_vert; uniform float u_fog_den;\n"
"uniform vec4 u_uvbox;\n"
"out vec4 o_col;\n"
"#if !QUAD\n"
"#if TEX && (HUD & 1)\n"
/* HUD art (low resolution, magnified several times): Catmull-Rom bicubic instead of bilinear keeps
 * digits and outlines crisp; 9 bilinear taps, clamped (the kernel overshoots at hard edges) */
"vec4 tex_cr(vec2 uv) {\n"
"  vec2 ts = vec2(textureSize(u_tex, 0)), p = uv * ts - 0.5, i = floor(p), f = p - i;\n"
"  vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);\n"
"  vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);\n"
"  vec2 w12 = w1 + w2, o12 = w2 / w12;\n"
"  vec2 t0 = (i - 0.5) / ts, t3 = (i + 2.5) / ts, t12 = (i + 0.5 + o12) / ts;\n"
"#if HUD & 2\n"
"  { vec2 lo = u_uvbox.xy + 0.5 / ts, hi = max(u_uvbox.zw - 0.5 / ts, lo); t0 = clamp(t0, lo, hi); t3 = clamp(t3, lo, hi); t12 = clamp(t12, lo, hi); }\n"
"#endif\n"
"  vec4 r = vec4(0.0);\n"
"  r += textureLod(u_tex, vec2(t0.x, t0.y), 0.0) * w0.x * w0.y;\n"
"  r += textureLod(u_tex, vec2(t12.x, t0.y), 0.0) * w12.x * w0.y;\n"
"  r += textureLod(u_tex, vec2(t3.x, t0.y), 0.0) * w3.x * w0.y;\n"
"  r += textureLod(u_tex, vec2(t0.x, t12.y), 0.0) * w0.x * w12.y;\n"
"  r += textureLod(u_tex, vec2(t12.x, t12.y), 0.0) * w12.x * w12.y;\n"
"  r += textureLod(u_tex, vec2(t3.x, t12.y), 0.0) * w3.x * w12.y;\n"
"  r += textureLod(u_tex, vec2(t0.x, t3.y), 0.0) * w0.x * w3.y;\n"
"  r += textureLod(u_tex, vec2(t12.x, t3.y), 0.0) * w12.x * w3.y;\n"
"  r += textureLod(u_tex, vec2(t3.x, t3.y), 0.0) * w3.x * w3.y;\n"
"  r = clamp(r, 0.0, 1.0);\n"
/* plus a light unsharp mask against the half-resolution mip (a 2x2 texel average) */
"  vec4 blur = textureLod(u_tex, uv, 1.0);\n"
"  return clamp(r + 1.0 * (r - blur), 0.0, 1.0);\n"
"}\n"
"#endif\n"
"#if FOG == 0 || FOG == 3\n"
/* the fog table (FOG_TABLE, 128 entries of two 8-bit values) is a 128x1 texture: as a uniform array
 * its 2 KB went to the GPU again with every change of state */
"float fog_alpha(float invw) {\n"
"  float fw = clamp(u_fog_den * invw, 1.0, 255.999985);\n"
"  uint b = floatBitsToUint(fw); uint e = (b >> 23) & 255u; uint m = b & 0x7FFFFFu;\n"
"  int idx = int((((e + 1u) & 7u) << 4) | ((m >> 19) & 15u));\n"
"  float bl = float((m >> 11) & 255u);\n"
"  vec2 ent = vec2(texelFetch(u_fogtab, ivec2(idx, 0), 0).rg);\n"
"  return floor((ent.x * bl + ent.y * (255.0 - bl)) / 256.0) / 255.0;\n"
"}\n"
"#endif\n"
"#endif\n"
"void main() {\n"
"#if QUAD\n"
"  o_col = u_quad_col;\n"
"#if !ZV\n"
"  gl_FragDepth = 0.0;\n"
"#endif\n"
"#else\n"
"  float iw = max(v_invw, 1e-30);\n"
"  vec2 uv = v_uvz / iw;\n"
"#if GOURAUD\n"
"  vec4 col = v_cz / iw, off = v_oz / iw;\n"
"#else\n"
"  vec4 col = f_c, off = f_o;\n"
"#endif\n"
"#if !USE_ALPHA\n"
"  col.a = 1.0;\n"
"#endif\n"
"  vec4 px;\n"
"#if TEX\n"
"#if HUD & 2\n"
"  { vec2 h = 0.5 / vec2(textureSize(u_tex, 0)), lo = u_uvbox.xy + h, hi = max(u_uvbox.zw - h, lo); uv = clamp(uv, lo, hi); }\n"
"#endif\n"
"#if HUD & 1\n"
"  vec4 t = tex_cr(uv);\n"
"#else\n"
"  vec4 t = texture(u_tex, uv);\n"
"#endif\n"
"#if IGN_TA\n"
"  t.a = 1.0;\n"
"#endif\n"
"#if SHADE == 0\n"
"  px = t;\n"
"#elif SHADE == 1\n"
"  px = vec4(t.rgb * col.rgb, t.a);\n"
"#elif SHADE == 2\n"
"  px = vec4(t.rgb * t.a + col.rgb * (1.0 - t.a), col.a);\n"
"#else\n"
"  px = t * col;\n"
"#endif\n"
"#else\n"
"  px = col;\n"
"#endif\n"
"#if OFFSET\n"
"  px.rgb = min(px.rgb + off.rgb, vec3(1.0));\n"
"#endif\n"
"#if FOG == 0\n"
"  px.rgb = mix(px.rgb, u_fog_ram, fog_alpha(iw));\n"
"#elif FOG == 1\n"
"  px.rgb = mix(px.rgb, u_fog_vert, off.a);\n"
"#elif FOG == 3\n"
"  px = vec4(u_fog_ram, fog_alpha(iw));\n"
"#endif\n"
"#if PT == 2\n"   /* MSAA: punch-through edges by alpha to coverage, a ramp one pixel wide across the cut-off */
"  { float c = clamp((px.a * 255.0 + 0.5 - u_pt_ref * 255.0) / max(fwidth(px.a) * 255.0, 1.0) + 0.5, 0.0, 1.0);\n"
"    if (c <= 0.0) discard;\n"
"    px.a = c; }\n"
"#elif PT == 1\n"
"  if (px.a * 255.0 + 0.5 < u_pt_ref * 255.0) discard;\n"
"#endif\n"
"#if TPASS == 1\n"
"  if (px.a < 0.996) discard;\n"
"#elif TPASS == 2\n"
"  if (px.a >= 0.996) discard;\n"   /* in 8-bit steps like the PVR: colours divided by the interpolated 1/w can land a hair below 1.0 (NVIDIA), which dropped whole letters of Rush's text */
"#endif\n"
"  o_col = px;\n"
"#if !ZV\n"
"  gl_FragDepth = clamp((log2(iw) + 64.0) / 128.0, 0.0, 1.0);\n"
"#endif\n"
"#endif\n"
"}\n";
static float depth_of(float invw) { float d = (log2f(fmaxf(invw, 1e-30f)) + 64.f) / 128.f; return d < 0 ? 0 : d > 1 ? 1 : d; }
/* Depth. The PVR compares 1/w, a float of any magnitude (a HUD at 2e6 over a scene below 1000). In a
 * 24-bit depth buffer that takes a logarithm, and since 1/w, not its logarithm, is what runs linearly
 * across a triangle, the logarithm has to be taken per pixel (gl_FragDepth). A program that writes its
 * own depth is run for every pixel of every triangle, hidden or not, and Dreamcast games send their
 * opaque polygons in any order (the PVR only textured what stayed visible), so on the Switch most of
 * the GPU's work went into pixels that were then thrown away.
 * zv_on: 1/w itself goes out as the vertex's depth, scaled by 2^-64 into 0..1 (exact, and the range
 * the logarithm covered), into a float depth buffer that keeps a float's precision at any magnitude.
 * That needs depth 0..1 left as it is (glClipControl: GL's -1..1 to 0..1 mapping would round small
 * values away). The GPU then tests depth before it runs the program, and skips what is hidden.
 * dc_pvr_gl_fast_depth = 0 (DC_GL_FAST_DEPTH=0) keeps the logarithm; so does a driver without
 * glClipControl (GL 4.5 / ARB_clip_control: macOS). */
int dc_pvr_gl_fast_depth = 1;
static int zv_on;                       /* this is how depth goes now */
/* The stencil is only for modifier volumes (shadows). Next to a float depth it is a plane of its own
 * (64 bits a pixel where depth alone is 32), on a GPU that is short of memory bandwidth: a game gets
 * it with its first frame that has volumes (zv_stencil, kept from then on), and plain 32-bit depth
 * until then. zv_plain: the driver makes a framebuffer with depth alone (tested with the rest). */
static int zv_stencil, zv_plain;
static GLenum depth_format(void) { return !zv_on ? GL_DEPTH24_STENCIL8 : zv_stencil || !zv_plain ? GL_DEPTH32F_STENCIL8 : GL_DEPTH_COMPONENT32F; }
static GLenum depth_attachment(GLenum fmt) { return fmt == GL_DEPTH_COMPONENT32F ? GL_DEPTH_ATTACHMENT : GL_DEPTH_STENCIL_ATTACHMENT; }
static GLenum fbo_dfmt, ms_dfmt;        /* what fbo's and ms_fbo's depth buffers are */
static int zv_ok;                       /* the driver can */
static void (*p_glClipControl)(GLenum, GLenum);
static float depth_clear(float invw) {
    if (!zv_on) return depth_of(invw);
    float d = fmaxf(invw, 1e-30f) * 5.421010862427522e-20f;   /* 2^-64 */
    return d > 1 ? 1 : d;
}

/* ------------------------------------------------------------------ state */
typedef struct { float x, y, z, u, v, u1, v1, c[4], o[4], c1[4], o1[4]; } gv;
static GLuint vao, vbo, fbo, color_tex, depth_rb;
static int fb_w, fb_h, ready;
/* a variant's key: the state its program is built for */
#define SV_TEX 1u                       /* textured */
#define SV_SHADE(n) ((uint32_t)(n) << 1)   /* 2 bits: texture/shading instruction */
#define SV_USE_ALPHA (1u << 3)
#define SV_IGN_TA (1u << 4)
#define SV_OFFSET (1u << 5)
#define SV_GOURAUD (1u << 6)
#define SV_FOG(n) ((uint32_t)(n) << 7)  /* 2 bits: fog mode (2 = none) */
#define SV_PT(n) ((uint32_t)(n) << 9)   /* 2 bits: punch-through (1), by alpha to coverage (2) */
#define SV_TPASS(n) ((uint32_t)(n) << 11) /* 2 bits: two-pass translucent polygons (see two_pass_poly) */
#define SV_HUD(n) ((uint32_t)(n) << 13) /* 2 bits: bicubic (1), sampling clamped to u_uvbox (2) */
#define SV_SET (1u << 15)               /* the vertices' second set (two-volume polygons inside a volume) */
#define SV_QUAD (1u << 16)              /* a quad of one colour in clip space: draw_quad */
#define SV_ZV (1u << 17)                /* depth from the vertices: zv_on */
#define SV_PLAIN (SV_FOG(2))            /* untextured, no fog: modifier volumes (their colour is masked) */
typedef struct { uint32_t key; GLuint prog; GLint scale, offx, quad_col, pt_ref, fog_ram, fog_vert, fog_den, uvbox; unsigned stamp; } svar;
#define NSV 256
static svar sv_tab[NSV]; static int sv_n;
static svar *sv_cur;                    /* the variant in use (NULL: another program may be bound) */
static unsigned sv_stamp = 1;           /* FU's generation: a variant takes the values when it is first used with them */
static struct { float sx, sy, offx, pt_ref, fog_ram[3], fog_vert[3], fog_den; } FU;   /* the frame's uniforms */
static GLuint fog_tex; static uint8_t fog_last[256]; static int fog_valid;
const char *dc_pvr_gl_shader_list;      /* file of the variants a game has used: built at start-up next time */
static gv *verts; static int nverts, cap_verts;
typedef struct { int first, count; } span;
static span *spans; static int cap_spans;

/* GL textures for decoded PVR textures, by decode id */
#define GLTEX 1024
static struct { uint64_t id; GLuint name; unsigned last; int mips; } gtex[GLTEX];
static unsigned frame_no;

static GLuint compile(GLenum type, const char *src) {
    GLuint s = p_glCreateShader(type);
    p_glShaderSource(s, 1, &src, NULL); p_glCompileShader(s);
    GLint ok = 0; p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[2048]; p_glGetShaderInfoLog(s, sizeof log, NULL, log); fprintf(stderr, "pvr_gl: shader: %s\n", log); }
    return s;
}

int dc_pvr_gl_mipmaps;                 /* enhancement: mipmaps for every texture */
int dc_pvr_gl_msaa;                    /* enhancement: multisample anti-aliasing, samples per pixel (0/1: off) */
float dc_pvr_gl_anisotropy;            /* enhancement: anisotropic filtering (clamped to the driver's max) */
static float max_aniso;
static void bars_init(void);

/* ------------------------------------------------------------------ variants */
static int sv_listed;                   /* building the list's variants: don't write them back */
static svar *sv_get(uint32_t key) {
    for (int i = 0; i < sv_n; i++) if (sv_tab[i].key == key) return &sv_tab[i];
    if (sv_n == NSV) {                  /* never in practice: start over */
        for (int i = 0; i < sv_n; i++) p_glDeleteProgram(sv_tab[i].prog);
        sv_n = 0; sv_cur = NULL;
    }
    char def[512];
    snprintf(def, sizeof def, "#version 330 core\n#define TEX %d\n#define SHADE %d\n#define USE_ALPHA %d\n#define IGN_TA %d\n#define OFFSET %d\n"
             "#define GOURAUD %d\n#define FOG %d\n#define PT %d\n#define TPASS %d\n#define HUD %d\n#define SET %d\n#define QUAD %d\n#define ZV %d\n",
             (int)(key & 1), (int)(key >> 1 & 3), (int)(key >> 3 & 1), (int)(key >> 4 & 1), (int)(key >> 5 & 1), (int)(key >> 6 & 1),
             (int)(key >> 7 & 3), (int)(key >> 9 & 3), (int)(key >> 11 & 3), (int)(key >> 13 & 3), (int)(key >> 15 & 1), (int)(key >> 16 & 1), (int)(key >> 17 & 1));
    GLuint sh[2], pr = p_glCreateProgram();
    for (int k = 0; k < 2; k++) {
        const char *src[2] = { def, k ? fs_body : vs_body };
        sh[k] = p_glCreateShader(k ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);
        p_glShaderSource(sh[k], 2, src, NULL); p_glCompileShader(sh[k]);
        GLint ok = 0; p_glGetShaderiv(sh[k], GL_COMPILE_STATUS, &ok);
        if (!ok) { char log[2048]; p_glGetShaderInfoLog(sh[k], sizeof log, NULL, log); fprintf(stderr, "pvr_gl: shader %05X: %s\n", key, log); }
        p_glAttachShader(pr, sh[k]);
    }
    p_glLinkProgram(pr);
    p_glDeleteShader(sh[0]); p_glDeleteShader(sh[1]);
    GLint ok = 0; p_glGetProgramiv(pr, GL_LINK_STATUS, &ok);
    if (!ok) { char log[2048]; p_glGetProgramInfoLog(pr, sizeof log, NULL, log); fprintf(stderr, "pvr_gl: link %05X: %s\n", key, log); p_glDeleteProgram(pr); return NULL; }
    svar *v = &sv_tab[sv_n++];
    memset(v, 0, sizeof *v);
    v->key = key; v->prog = pr;
    v->scale = p_glGetUniformLocation(pr, "u_scale"); v->offx = p_glGetUniformLocation(pr, "u_offx"); v->quad_col = p_glGetUniformLocation(pr, "u_quad_col");
    v->pt_ref = p_glGetUniformLocation(pr, "u_pt_ref"); v->fog_ram = p_glGetUniformLocation(pr, "u_fog_ram"); v->fog_vert = p_glGetUniformLocation(pr, "u_fog_vert");
    v->fog_den = p_glGetUniformLocation(pr, "u_fog_den"); v->uvbox = p_glGetUniformLocation(pr, "u_uvbox");
    p_glUseProgram(pr); sv_cur = NULL;                     /* the samplers' units, once */
    { GLint t = p_glGetUniformLocation(pr, "u_tex"), ft = p_glGetUniformLocation(pr, "u_fogtab");
      if (t >= 0) p_glUniform1i(t, 0);
      if (ft >= 0) p_glUniform1i(ft, 1); }
    if (!sv_listed && dc_pvr_gl_shader_list) {             /* for the next start */
        FILE *f = fopen(dc_pvr_gl_shader_list, "a");
        if (f) { fprintf(f, "%05X\n", key); fclose(f); }
    }
    return v;
}
static void sv_use(uint32_t key) {
    if (key & SV_QUAD) key = SV_QUAD;
    else if (!(key & SV_TEX)) key &= ~(SV_SHADE(3) | SV_IGN_TA | SV_HUD(3));    /* what only textured polygons have */
    if (zv_on) key |= SV_ZV;
    svar *v = sv_cur && sv_cur->key == key ? sv_cur : sv_get(key);
    if (!v) return;
    if (v != sv_cur) { p_glUseProgram(v->prog); sv_cur = v; }
    if (v->stamp != sv_stamp) {
        v->stamp = sv_stamp;
        if (v->scale >= 0) p_glUniform2f(v->scale, FU.sx, FU.sy);
        if (v->offx >= 0) p_glUniform1f(v->offx, FU.offx);
        if (v->pt_ref >= 0) p_glUniform1f(v->pt_ref, FU.pt_ref);
        if (v->fog_ram >= 0) p_glUniform3f(v->fog_ram, FU.fog_ram[0], FU.fog_ram[1], FU.fog_ram[2]);
        if (v->fog_vert >= 0) p_glUniform3f(v->fog_vert, FU.fog_vert[0], FU.fog_vert[1], FU.fog_vert[2]);
        if (v->fog_den >= 0) p_glUniform1f(v->fog_den, FU.fog_den);
    }
}
/* the variants a game used before, from dc_pvr_gl_shader_list: built now, not in the middle of play */
static void sv_load_list(void) {
    if (!dc_pvr_gl_shader_list) return;
    FILE *f = fopen(dc_pvr_gl_shader_list, "r");
    if (!f) return;
    char line[64]; int n = 0;
    sv_listed = 1;
    while (fgets(line, sizeof line, f) && n < NSV / 2) {
        uint32_t key = (uint32_t)strtoul(line, NULL, 16);
        if (!key || key >= (1u << 18) || !(key & SV_ZV) != !zv_on) continue;
        if (sv_get(key)) n++;
    }
    sv_listed = 0;
    fclose(f);
    sv_cur = NULL;
}

int dc_pvr_gl_init(void *(*getproc)(const char *)) {
    int missing = 0;
#define F(ret, name, args) p_##name = (ret (*) args)getproc(#name); if (!p_##name) { fprintf(stderr, "pvr_gl: missing %s\n", #name); missing++; }
    GLFUNCS
#undef F
    if (missing) return -1;
    gl_debug_init(getproc);
    bars_init();
    p_glGenTextures(1, &fog_tex); p_glActiveTexture(GL_TEXTURE1); p_glBindTexture(GL_TEXTURE_2D, fog_tex);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8UI, 128, 1, 0, GL_RG_INTEGER, GL_UNSIGNED_BYTE, fog_last);
    p_glActiveTexture(GL_TEXTURE0);
    p_glGenVertexArrays(1, &vao); p_glBindVertexArray(vao);
    p_glGenBuffers(1, &vbo); p_glBindBuffer(GL_ARRAY_BUFFER, vbo);
    p_glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(gv), (void *)offsetof(gv, x));
    p_glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(gv), (void *)offsetof(gv, u));
    p_glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(gv), (void *)offsetof(gv, c));
    p_glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(gv), (void *)offsetof(gv, o));
    p_glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, sizeof(gv), (void *)offsetof(gv, c1));
    p_glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, sizeof(gv), (void *)offsetof(gv, o1));
    for (int i = 0; i < 6; i++) p_glEnableVertexAttribArray((GLuint)i);
    { GLfloat a = 0; p_glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &a); while (p_glGetError()) {} max_aniso = a; }
    sv_listed = 1;
    if (!sv_get(SV_TEX | SV_SHADE(3) | SV_USE_ALPHA | SV_GOURAUD | SV_FOG(2))) return -1;   /* the shaders build here at all */
    sv_listed = 0;
    {   /* zv_on's needs: glClipControl (GL 4.5 or ARB_clip_control) */
        const unsigned char *(*gsi)(GLenum, GLuint) = (const unsigned char *(*)(GLenum, GLuint))getproc("glGetStringi");
        GLint n = 0, maj = 0, min = 0;
        p_glGetIntegerv(0x821B /* MAJOR_VERSION */, &maj); p_glGetIntegerv(0x821C /* MINOR_VERSION */, &min); p_glGetIntegerv(0x821D /* NUM_EXTENSIONS */, &n);
        int has = maj > 4 || (maj == 4 && min >= 5);
        for (int i = 0; gsi && !has && i < n; i++) { const char *e = (const char *)gsi(0x1F03 /* EXTENSIONS */, (GLuint)i); if (e && !strcmp(e, "GL_ARB_clip_control")) has = 1; }
        p_glClipControl = has ? (void (*)(GLenum, GLenum))getproc("glClipControl") : NULL;
        zv_ok = p_glClipControl != NULL;
        while (p_glGetError()) {}
        const char *e = getenv("DC_GL_FAST_DEPTH"); if (e) dc_pvr_gl_fast_depth = atoi(e) != 0;
    }
    ready = 1;
    return 0;
}

int dc_pvr_gl_widescreen;
float dc_pvr_gl_aspect;                /* window width / height (the frontend sets it, 4:3 or not); the widescreen target's shape (0 = 16:9) */
float dc_pvr_gl_hud_z;
float dc_pvr_gl_hud_zmax;
int dc_pvr_gl_hud_exact;
int dc_pvr_gl_clip_2d;
static float disp_aspect = 4.f / 3.f;
static float last_ww, last_rw, last_rh;   /* the last render's target width and picture size, console units */

/* the last picture rendered into each framebuffer address (double buffering: two), so a later render
 * that only draws a band of lines into one of them keeps what is there outside it, as the console
 * does (Looney Tunes Space Race's loading bar over the picture before a race) */
#define NKEEP 4
static struct { uint32_t tgt; int w, h, fbw, fbh; float ww; GLuint fbo, tex; unsigned frame; } keep[NKEEP];
int dc_pvr_gl_keep_outside = 1;
static int fb_scale = 2;                                   /* the render's scale over the console's lines, in halves */
int dc_pvr_gl_half_step;                                   /* render half a step lower than asked: 2 gives 1.5 (720 lines) */
/* MSAA: the frame's polygons are drawn into ms_fbo (the same size, dc_pvr_gl_msaa samples a pixel) and
 * resolved into fbo once they are all drawn; everything after (side bars, readbacks, keep, present)
 * works on fbo as without it. Not while capturing a render-to-texture pass (its own buffers). */
static GLuint ms_fbo, ms_col, ms_depth; static int ms_w, ms_h, ms_n, ms_n_said, ms_max = -1;
static int ms_a2c;                              /* this frame: punch-through by alpha to coverage */
static int ms_want(void) {
    if (dc_pvr_gl_msaa < 2) return 0;
    if (ms_max < 0) { GLint m = 0; p_glGetIntegerv(GL_MAX_SAMPLES, &m); while (p_glGetError()) {} ms_max = m; }
    int n = dc_pvr_gl_msaa < ms_max ? dc_pvr_gl_msaa : ms_max;
    return n >= 2 ? n : 0;
}
static int ensure_ms(void) {                     /* 1: draw into ms_fbo this frame */
    int n = ms_want();
    /* kept at the largest picture seen: games that switch sizes (Soulcalibur) remade it again and again */
    GLenum df = depth_format();
    if (n && ms_fbo && ms_n == n && ms_w >= fb_w && ms_h >= fb_h && ms_dfmt == df) return 1;
    int want_w = fb_w > ms_w ? fb_w : ms_w, want_h = fb_h > ms_h ? fb_h : ms_h;
    if (!n || ms_n != n) { want_w = fb_w; want_h = fb_h; }
    if (ms_fbo) { p_glDeleteFramebuffers(1, &ms_fbo); p_glDeleteRenderbuffers(1, &ms_col); p_glDeleteRenderbuffers(1, &ms_depth); ms_fbo = 0; }
    ms_n = 0;
    if (!n) return 0;
    p_glGenRenderbuffers(1, &ms_col); p_glBindRenderbuffer(GL_RENDERBUFFER, ms_col);
    p_glRenderbufferStorageMultisample(GL_RENDERBUFFER, n, GL_RGBA8, want_w, want_h);
    p_glGenRenderbuffers(1, &ms_depth); p_glBindRenderbuffer(GL_RENDERBUFFER, ms_depth);
    p_glRenderbufferStorageMultisample(GL_RENDERBUFFER, n, df, want_w, want_h);
    p_glGenFramebuffers(1, &ms_fbo); p_glBindFramebuffer(GL_FRAMEBUFFER, ms_fbo);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, ms_col);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, depth_attachment(df), GL_RENDERBUFFER, ms_depth);
    int ok = p_glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    while (p_glGetError()) {}
    if (!ok) {
        fprintf(stderr, "pvr_gl: no %dx anti-aliasing here (framebuffer incomplete)\n", n);
        p_glDeleteFramebuffers(1, &ms_fbo); p_glDeleteRenderbuffers(1, &ms_col); p_glDeleteRenderbuffers(1, &ms_depth); ms_fbo = 0;
        ms_max = n - 1;                           /* try fewer next time */
        return 0;
    }
    if (ms_n_said != n) fprintf(stderr, "pvr_gl: %dx anti-aliasing\n", n);
    ms_n = ms_n_said = n; ms_w = want_w; ms_h = want_h; ms_dfmt = df;
    return 1;
}
static void ensure_fbo(int w, int h) {
    GLenum df = depth_format();
    if (fbo && w == fb_w && h == fb_h) {
        if (fbo_dfmt == df) return;
        /* another depth (the first volumes bring the stencil; FAST DEPTH was switched): the colour
         * stays, a picture made in bands keeps the bands it has and a capture its picture */
        p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
        p_glBindRenderbuffer(GL_RENDERBUFFER, depth_rb);
        p_glRenderbufferStorage(GL_RENDERBUFFER, df, w, h);
        p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, depth_attachment(df), GL_RENDERBUFFER, depth_rb);
        if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) fprintf(stderr, "pvr_gl: framebuffer incomplete\n");
        fbo_dfmt = df;
        return;
    }
    if (fbo) { p_glDeleteFramebuffers(1, &fbo); p_glDeleteTextures(1, &color_tex); p_glDeleteRenderbuffers(1, &depth_rb); }
    p_glGenTextures(1, &color_tex); p_glBindTexture(GL_TEXTURE_2D, color_tex);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    p_glGenRenderbuffers(1, &depth_rb); p_glBindRenderbuffer(GL_RENDERBUFFER, depth_rb);
    p_glRenderbufferStorage(GL_RENDERBUFFER, df, w, h);
    p_glGenFramebuffers(1, &fbo); p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_tex, 0);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, depth_attachment(df), GL_RENDERBUFFER, depth_rb);
    if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) fprintf(stderr, "pvr_gl: framebuffer incomplete\n");
    fb_w = w; fb_h = h; fbo_dfmt = df;
}

/* ------------------------------------------------------------------ texture packs
 * Textures are named by a hash of their decoded pixels and size: <hash>_<w>x<h>.png. With a dump
 * folder set, every texture the game uploads is saved there once; with a pack folder set, a PNG of
 * that name there (any size, e.g. upscaled) is uploaded in its place. */
const char *dc_pvr_gl_tex_dump;
const char *dc_pvr_gl_tex_pack;
unsigned long dc_pvr_gl_tex_dumped, dc_pvr_gl_tex_replaced;

/* hashes seen: bit 0 dumped, bit 1 looked up in the pack, bit 2 the pack has it */
#define TEXSEEN (1u << 16)
static struct { uint64_t h; unsigned flags; } tex_seen[TEXSEEN];
static unsigned *tex_flags(uint64_t h) {
    for (unsigned i = (unsigned)h & (TEXSEEN - 1), n = 0; n < TEXSEEN; i = (i + 1) & (TEXSEEN - 1), n++) {
        if (tex_seen[i].h == h) return &tex_seen[i].flags;
        if (!tex_seen[i].h) { tex_seen[i].h = h; tex_seen[i].flags = 0; return &tex_seen[i].flags; }
    }
    static unsigned spare; spare = 3; return &spare;   /* full: neither dump nor replace */
}
static void tex_dump(const pvr_texinfo *ti, uint64_t h) {
    if (dc_tex_save(dc_pvr_gl_tex_dump, h, ti->px, ti->w, ti->h) > 0) dc_pvr_gl_tex_dumped++;
}
/* a replacement for this texture from the pack (RGBA bytes, free with dc_tex_free), or NULL */
static uint8_t *tex_replacement(const pvr_texinfo *ti, uint64_t h, unsigned *flags, int *w, int *hh) {
    if ((*flags & 2) && !(*flags & 4)) return NULL;
    uint8_t *px = dc_tex_load(dc_pvr_gl_tex_pack, h, ti->w, ti->h, w, hh);
    *flags |= 2;
    if (px) { *flags |= 4; if (!(*flags & 8)) { dc_pvr_gl_tex_replaced++; *flags |= 8; } }
    return px;
}

static GLuint texture_for(uint32_t tcw, uint32_t tsp) {
    pvr_texinfo ti;
    tex_get(tcw, tsp, &ti);
    int slot = -1, oldest = 0;
    for (int i = 0; i < GLTEX; i++) {
        if (gtex[i].name && gtex[i].id == ti.id) {
            gtex[i].last = frame_no;
            if (dc_pvr_gl_mipmaps && !gtex[i].mips) {             /* mipmaps switched on after the upload */
                p_glBindTexture(GL_TEXTURE_2D, gtex[i].name); p_glGenerateMipmap(GL_TEXTURE_2D); gtex[i].mips = 1;
            }
            return gtex[i].name;
        }
        if (!gtex[i].name && slot < 0) slot = i;
        if (gtex[i].last < gtex[oldest].last) oldest = i;
    }
    if (slot < 0) { slot = oldest; p_glDeleteTextures(1, &gtex[slot].name); gtex[slot].name = 0; }
    GLuint n; p_glGenTextures(1, &n); p_glBindTexture(GL_TEXTURE_2D, n);
    p_glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    uint8_t *rep = NULL; int rw = 0, rh = 0;
    static const char *pack_seen; static int pack_ok;
    if (dc_pvr_gl_tex_pack != pack_seen) {             /* only when the folder is there */
        struct stat st; pack_seen = dc_pvr_gl_tex_pack;
        pack_ok = pack_seen && stat(pack_seen, &st) == 0;
        if (pack_ok) fprintf(stderr, "pvr_gl: texture pack %s\n", pack_seen);
    }
    if (dc_pvr_gl_tex_dump || pack_ok) {
        uint64_t h = dc_tex_hash(ti.px, ti.w, ti.h);
        unsigned *fl = tex_flags(h);
        if (dc_pvr_gl_tex_dump && !(*fl & 1)) { *fl |= 1; tex_dump(&ti, h); }
        if (pack_ok) rep = tex_replacement(&ti, h, fl, &rw, &rh);
    }
    if (rep) { p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, rw, rh, 0, GL_RGBA, GL_UNSIGNED_BYTE, rep); dc_tex_free(rep); }
    else p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ti.w, ti.h, 0, GL_BGRA, GL_UNSIGNED_BYTE, ti.px);
    if (dc_pvr_gl_mipmaps) p_glGenerateMipmap(GL_TEXTURE_2D);
    gtex[slot].mips = dc_pvr_gl_mipmaps;
    gtex[slot].id = ti.id; gtex[slot].name = n; gtex[slot].last = frame_no;
    if (dbg_log) { static int ups; if (++ups <= 5 || ups % 500 == 0) { fprintf(dbg_log, "upload %d: %dx%d tcw %08X tsp %08X\n", ups, ti.w, ti.h, tcw, tsp); fflush(dbg_log); }
                   verify_upload(&ti, tcw, tsp); }
    return n;
}

/* ------------------------------------------------------------------ geometry */
static float push_dx;                   /* x shift for the polygon being pushed (HUD in widescreen) */
static float push_wide;                 /* > 0: a full-screen quad, its edges pushed out this far */
static float push_rw;
/* widescreen render to texture (dc_pvr_gl_capture): the pass is also drawn with GL at the screen's
 * full width; a full-screen quad that later shows that texture draws the wide capture instead, its
 * texture coordinates taken from where each corner lands on the screen */
#define NCAP 4
static struct { GLuint fbo, tex, depth; GLenum dfmt; int w, h; uint32_t addr; float ww, rw, rh; unsigned long frame; int flat2d; } cap[NCAP + 1];
static int pass_2d;                     /* the capture pass just drawn was a 2D screen */   /* [NCAP]: the previous contents of the one being drawn */
static int capturing;                   /* drawing a render-to-texture pass */
static int cap_cur = -1;                /* while capturing: the slot being drawn */
static int cap_uv = -1;                 /* >= 0: the polygon being pushed shows capture cap_uv */
/* where the quad's screen position lands in the capture (console pixels = texels): xc = cap_cx0 + (x - cap_qx0) * cap_sx */
static float cap_qx0, cap_qy0, cap_cx0, cap_cy0, cap_sx, cap_sy;
static unsigned char *poly_cap;         /* per polygon: capture + 1, or 0 */
/* how many translucent polygons this frame came before at exactly this depth (tdepth_reset per frame) */
static struct { uint32_t key; int n; } tdepth[4096];
static uint32_t tdepth_gen = 1, tdepth_used[4096];
static void tdepth_reset(void) { tdepth_gen++; }
static int tdepth_run(float z) {
    uint32_t k; memcpy(&k, &z, 4);
    for (uint32_t h = (k * 2654435761u) >> 20, n = 0; n < 4096; n++, h = (h + 1) & 4095) {
        if (tdepth_used[h] != tdepth_gen) { tdepth_used[h] = tdepth_gen; tdepth[h].key = k; tdepth[h].n = 1; return 0; }
        if (tdepth[h].key == k) return tdepth[h].n++;
    }
    return 0;
}
static float push_zk = 1.f;            /* translucent polygons at one depth: a later one a hair nearer (see below) */
/* a HUD zone that stretches (dc_hud_zone.stretch): x in [s_a, s_b] goes to [s_a2, s_b2], the ends
 * (within the zone's margin) move rigidly with them */
static int push_str; static float s_a, s_b, s_m, s_a2, s_b2;
static float stretch_x(float x) {
    if (x <= s_a + s_m) return x + (s_a2 - s_a);
    if (x >= s_b - s_m) return x + (s_b2 - s_b);
    float a = s_a + s_m, b = s_b - s_m, a2 = s_a2 + s_m, b2 = s_b2 - s_m;
    return a2 + (x - a) * (b2 - a2) / (b - a);
}
static void push(const pvr_vtx *v) {
    if (nverts == cap_verts) { cap_verts = cap_verts ? cap_verts * 2 : 8192; verts = realloc(verts, cap_verts * sizeof *verts); }
    gv *g = &verts[nverts++];
    g->x = push_str ? stretch_x(v->x) : v->x + push_dx;
    if (push_wide > 0) { if (v->x <= 1.f) g->x -= push_wide; else if (v->x >= push_rw - 1.f) g->x += push_wide; } g->y = v->y; g->z = v->z * push_zk; g->u = v->u; g->v = v->v; g->u1 = v->u1; g->v1 = v->v1;
    memcpy(g->c, v->c, 16); memcpy(g->o, v->o, 16); memcpy(g->c1, v->c1, 16); memcpy(g->o1, v->o1, 16);
    if (cap_uv >= 0) {                  /* the capture spans [-(ww - rw) / 2, rw + (ww - rw) / 2] x [0, rh], rows up */
        /* by the quad's own texture coordinates: a capture drawn smaller than the screen and stretched over
         * it (Dead or Alive 2's transitions: 512 x 256 texels), or a quad zoomed past the screen */
        float xc = cap_cx0 + (g->x - cap_qx0) * cap_sx;
        float yc = cap_cy0 + (g->y - cap_qy0) * cap_sy;
        g->u = (xc + (cap[cap_uv].ww - cap[cap_uv].rw) * 0.5f) / cap[cap_uv].ww;
        g->v = 1.f - yc / cap[cap_uv].rh;
    }
}
/* Triangles reaching outside the render target are clipped to it here, not by the GPU. Games send
 * polygons that cross the camera plane with 1/w clamped and screen positions far off screen (Jet Set
 * Radio's road and crosswalk near the camera: 1/w 1.0 at x 38000, y 7700). The GPU's own clipping of
 * those gave the interpolated 1/w a wrong slope (26% too steep on llvmpipe), so the road won the depth
 * test against the crosswalk painted on it and the crosswalk's near stripes vanished in some frames.
 * Clipping here is exact: x, y, 1/w and every value premultiplied by 1/w are linear in screen space,
 * which is how the shaders interpolate them. Bounds are in console units (the target spans
 * [-(WW - RW) / 2, RW + (WW - RW) / 2] x [0, RH]), one unit of margin. */
static float tgt_x0 = -1e30f, tgt_y0 = -1e30f, tgt_x1 = 1e30f, tgt_y1 = 1e30f;
static pvr_vtx clip_lerp(const pvr_vtx *a, const pvr_vtx *b, float t) {
    pvr_vtx r;
    float za = a->z * (1.f - t), zb = b->z * t, z = za + zb;
    float ia = z > 0 ? za / z : 1.f - t, ib = z > 0 ? zb / z : t;     /* weights of the attributes themselves */
    r.x = a->x + (b->x - a->x) * t; r.y = a->y + (b->y - a->y) * t; r.z = z;
    r.u = a->u * ia + b->u * ib; r.v = a->v * ia + b->v * ib; r.u1 = a->u1 * ia + b->u1 * ib; r.v1 = a->v1 * ia + b->v1 * ib;
    for (int k = 0; k < 4; k++) {
        r.c[k] = a->c[k] * ia + b->c[k] * ib; r.o[k] = a->o[k] * ia + b->o[k] * ib;
        r.c1[k] = a->c1[k] * ia + b->c1[k] * ib; r.o1[k] = a->o1[k] * ia + b->o1[k] * ib;
    }
    return r;
}
/* Sutherland-Hodgman against one edge: keeps x (axis 0) or y (axis 1) >= lim, or <= lim with below */
static int clip_edge(const pvr_vtx *in, int n, pvr_vtx *out, int axis, float lim, int below) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        const pvr_vtx *a = &in[i], *b = &in[(i + 1) % n];
        float da = (axis ? a->y : a->x) - lim, db = (axis ? b->y : b->x) - lim;
        if (below) { da = -da; db = -db; }
        if (da >= 0) out[m++] = *a;
        if ((da >= 0) != (db >= 0)) out[m++] = clip_lerp(a, b, da / (da - db));
    }
    return m;
}
/* dc_pvr_gl_extend_backdrops: a 2D backdrop drawn exactly across the 4:3 picture (every vertex inside it,
 * two or more on each side edge: a menu's gradient or streak band, a sky card behind a 3D scene) gets
 * strips out to the widescreen edges that repeat its edge column (same UV and colours along each edge),
 * so the picture continues instead of ending in black; its own 4:3 part is unchanged. */
int dc_pvr_gl_extend_backdrops;
/* dc_pvr_gl_extend_tiles: also pictures made of tiles, continued mirrored past the 4:3 edges (see
 * mirror_tile) */
int dc_pvr_gl_extend_tiles;
float dc_pvr_gl_extend_tiles_min;   /* only tiles at least this wide (a port can keep small-tile menus as they are) */
static int cmp_vy(const void *a, const void *b) { float d = (*(const pvr_vtx *const *)a)->y - (*(const pvr_vtx *const *)b)->y; return d < 0 ? -1 : d > 0; }
/* a flat card a little wider than the picture (Soulcalibur's opening: a sky quad from x -15 to 648 at
 * one depth): strips from its side edges out to the widescreen edges showing the card's texture
 * mirrored at that edge (its own screen-to-texture mapping, reflected), rather than its edge column
 * repeated */
static int overhang_backdrop(const pvr_poly *p, const pvr_vtx *v, float rw, float ext) {
    if (p->n < 3 || p->n > 8) return 0;
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    for (int k = 0; k < p->n; k++) {
        if (v[k].x < -0.5f * rw || v[k].x > 1.5f * rw) return 0;   /* far past the picture: 3D (the opening's sky
                                                                       * card pans out to x -100..695) */
        if (fabsf(v[k].z - v[0].z) > 1e-6f * fabsf(v[0].z) + 1e-9f) return 0;   /* not flat: perspective */
        if (v[k].x < x0) x0 = v[k].x; if (v[k].x > x1) x1 = v[k].x;
        if (v[k].y < y0) y0 = v[k].y; if (v[k].y > y1) y1 = v[k].y;
    }
    if (x0 > 1.f || x1 < rw - 2.5f || y1 - y0 < 0.6f * rw) return 0;   /* must cover the picture */
    /* screen -> (u, v): the plane through the three vertices spanning the largest area */
    int ba = 0, bb = 1, bc = 2; float best = 0;
    for (int a = 0; a < p->n; a++) for (int b = a + 1; b < p->n; b++) for (int c = b + 1; c < p->n; c++) {
        float ar = fabsf((v[b].x - v[a].x) * (v[c].y - v[a].y) - (v[b].y - v[a].y) * (v[c].x - v[a].x));
        if (ar > best) { best = ar; ba = a; bb = b; bc = c; }
    }
    if (best < 1.f) return 0;
    const pvr_vtx *A = &v[ba], *B = &v[bb], *C = &v[bc];
    float d = (B->x - A->x) * (C->y - A->y) - (B->y - A->y) * (C->x - A->x);
    float ux = ((B->u - A->u) * (C->y - A->y) - (C->u - A->u) * (B->y - A->y)) / d;
    float uy = ((C->u - A->u) * (B->x - A->x) - (B->u - A->u) * (C->x - A->x)) / d;
    float vx = ((B->v - A->v) * (C->y - A->y) - (C->v - A->v) * (B->y - A->y)) / d;
    float vy = ((C->v - A->v) * (B->x - A->x) - (B->v - A->v) * (C->x - A->x)) / d;
    for (int k = 0; k < p->n; k++)   /* the UVs must follow that plane (a plain textured card) */
        if (fabsf(A->u + ux * (v[k].x - A->x) + uy * (v[k].y - A->y) - v[k].u) > 1e-3f ||
            fabsf(A->v + vx * (v[k].x - A->x) + vy * (v[k].y - A->y) - v[k].v) > 1e-3f) return 0;
    for (int sd = 0; sd < 2; sd++) {
        const pvr_vtx *e[8]; int ne = 0;
        for (int k = 0; k < p->n; k++)
            if (sd ? v[k].x >= x1 - 1.f : v[k].x <= x0 + 1.f) e[ne++] = &v[k];
        if (ne < 2) continue;
        qsort(e, (size_t)ne, sizeof e[0], cmp_vy);
        for (int k = 0; k + 1 < ne; k++) {
            const pvr_vtx *a = e[k], *b = e[k + 1];
            if (b->y - a->y < 0.5f) continue;
            pvr_vtx a2 = *a, b2 = *b;
            a2.x = b2.x = sd ? rw + ext : -ext;
            a2.u = A->u + ux * (a2.x - A->x) + uy * (a2.y - A->y); a2.v = A->v + vx * (a2.x - A->x) + vy * (a2.y - A->y);
            b2.u = A->u + ux * (b2.x - A->x) + uy * (b2.y - A->y); b2.v = A->v + vx * (b2.x - A->x) + vy * (b2.y - A->y);
            /* mirrored at the card's edge: seamless whatever the texture holds past it (a clamped
             * texture would smear its edge column into streaks) */
            a2.u = 2.f * a->u - a2.u; a2.v = 2.f * a->v - a2.v; b2.u = 2.f * b->u - b2.u; b2.v = 2.f * b->v - b2.v;
            push(a); push(b); push(&a2); push(&a2); push(b); push(&b2);
        }
    }
    return 1;
}
/* dc_pvr_gl_extend_tiles: a picture made of opaque, flat, screen-aligned tiles (Soulcalibur's intro
 * screen: 10 x 8 tiles of 64 pixels; the opening's 320-pixel glyph wall; its main menu) continues past
 * the 4:3 edges as its own mirror image: each tile within a side width of an edge is drawn again,
 * reflected about that edge and cut to the side band, with UVs from the tile's own screen-to-texture
 * mapping, so the strip meets the picture without a seam. */
static void mirror_tile(const pvr_poly *p, const pvr_vtx *v, float rw, float ext) {
    if (p->list != 0 || p->n != 4) return;
    float x0 = v[0].x, x1 = v[0].x, y0 = v[0].y, y1 = v[0].y;
    for (int k = 1; k < 4; k++) {
        if (fabsf(v[k].z - v[0].z) > 1e-6f * fabsf(v[0].z) + 1e-9f) return;       /* flat */
        if (v[k].x < x0) x0 = v[k].x; if (v[k].x > x1) x1 = v[k].x;
        if (v[k].y < y0) y0 = v[k].y; if (v[k].y > y1) y1 = v[k].y;
    }
    if (x0 < -1.f || x1 > rw + 1.f || x1 - x0 < 8.f || y1 - y0 < 8.f || x1 - x0 < dc_pvr_gl_extend_tiles_min) return;
    for (int k = 0; k < 4; k++)                                                    /* a screen-aligned rectangle */
        if ((fabsf(v[k].x - x0) > 0.5f && fabsf(v[k].x - x1) > 0.5f) || (fabsf(v[k].y - y0) > 0.5f && fabsf(v[k].y - y1) > 0.5f)) return;
    int left = x0 < ext, right = x1 > rw - 1.f - ext;
    if (!left && !right) return;
    /* screen -> (u, v) through three corners */
    const pvr_vtx *A = &v[0], *B = &v[1], *C = &v[2];
    float d = (B->x - A->x) * (C->y - A->y) - (B->y - A->y) * (C->x - A->x);
    if (fabsf(d) < 1.f) { C = &v[3]; d = (B->x - A->x) * (C->y - A->y) - (B->y - A->y) * (C->x - A->x); }
    if (fabsf(d) < 1.f) return;
    float ux = ((B->u - A->u) * (C->y - A->y) - (C->u - A->u) * (B->y - A->y)) / d;
    float uy = ((C->u - A->u) * (B->x - A->x) - (B->u - A->u) * (C->x - A->x)) / d;
    float vx = ((B->v - A->v) * (C->y - A->y) - (C->v - A->v) * (B->y - A->y)) / d;
    float vy = ((C->v - A->v) * (B->x - A->x) - (B->v - A->v) * (C->x - A->x)) / d;
    for (int k = 0; k < 4; k++)
        if (fabsf(A->u + ux * (v[k].x - A->x) + uy * (v[k].y - A->y) - v[k].u) > 1e-3f ||
            fabsf(A->v + vx * (v[k].x - A->x) + vy * (v[k].y - A->y) - v[k].v) > 1e-3f) return;
    for (int sd = 0; sd < 2; sd++) {
        if (sd ? !right : !left) continue;
        float edge = rw - 1.f;                              /* the picture's right edge: 640 in a 641-wide frame */
        float m = sd ? 2.f * edge : 0.f;                    /* mirror: x' = m - x */
        float a = m - x1, b = m - x0;                       /* the reflected tile */
        if (sd) { if (a < edge) a = edge; if (b > rw + ext) b = rw + ext; }
        else { if (a < -ext) a = -ext; if (b > 0.f) b = 0.f; }
        if (b - a < 0.01f) continue;
        pvr_vtx q[4];
        const float qx[4] = { a, b, b, a }, qy[4] = { y0, y0, y1, y1 };
        for (int k = 0; k < 4; k++) {
            q[k] = v[0];
            float ox = m - qx[k];                           /* where this point's texel sits in the tile */
            q[k].x = qx[k]; q[k].y = qy[k];
            q[k].u = A->u + ux * (ox - A->x) + uy * (qy[k] - A->y);
            q[k].v = A->v + vx * (ox - A->x) + vy * (qy[k] - A->y);
        }
        push(&q[0]); push(&q[1]); push(&q[2]); push(&q[0]); push(&q[2]); push(&q[3]);
    }
}
/* dc_pvr_gl_extend_backdrops: a backdrop drawn as a mesh of small, flat, untextured, opaque rectangles
 * at one depth (Soulcalibur's logo screen: a 58 x 58 grid of shaded quads making a glow, x -23..651,
 * zooming) continues past its edges in its edge colours: each rectangle on the mesh's outer edge gets
 * a strip out to the widescreen edge (and to the top / bottom of the picture when the mesh stops short
 * of them), corners filled with the corner colour. mesh_z: the depths of this frame's meshes. */
enum { MESH_MAX = 4 };
static struct { float z, x0, x1, y0, y1; int n; } mesh[MESH_MAX]; static int nmesh;
static int mesh_rect(const pvr_poly *p, const pvr_vtx *v, float *x0, float *x1, float *y0, float *y1) {
    if (p->list != 0 || p->n != 4 || ((p->isp >> 25) & 1)) return 0;   /* opaque, untextured quads */
    *x0 = *x1 = v[0].x; *y0 = *y1 = v[0].y;
    for (int k = 1; k < 4; k++) {
        if (v[k].z != v[0].z) return 0;
        if (v[k].x < *x0) *x0 = v[k].x; if (v[k].x > *x1) *x1 = v[k].x;
        if (v[k].y < *y0) *y0 = v[k].y; if (v[k].y > *y1) *y1 = v[k].y;
    }
    for (int k = 0; k < 4; k++)
        if ((fabsf(v[k].x - *x0) > .5f && fabsf(v[k].x - *x1) > .5f) || (fabsf(v[k].y - *y0) > .5f && fabsf(v[k].y - *y1) > .5f)) return 0;
    return *x1 - *x0 > 0.01f && *y1 - *y0 > 0.01f;
}
static void find_meshes(const pvr_frame *f, float rw, float rh) {
    enum { NC = 8 };
    struct { float z, x0, x1, y0, y1, big; int n; } c[NC]; int nc = 0;
    nmesh = 0;
    for (int i = 0; i < f->npolys; i++) {
        const pvr_poly *p = &f->polys[i]; const pvr_vtx *v = f->verts + p->first;
        float x0, x1, y0, y1;
        if (!mesh_rect(p, v, &x0, &x1, &y0, &y1)) continue;
        int j = 0; while (j < nc && c[j].z != v[0].z) j++;
        if (j == nc) { if (nc == NC) continue; nc++; c[j].z = v[0].z; c[j].x0 = x0; c[j].x1 = x1; c[j].y0 = y0; c[j].y1 = y1; c[j].n = 0; c[j].big = 0; }
        if (x0 < c[j].x0) c[j].x0 = x0; if (x1 > c[j].x1) c[j].x1 = x1;
        if (y0 < c[j].y0) c[j].y0 = y0; if (y1 > c[j].y1) c[j].y1 = y1;
        if (x1 - x0 > rw / 8.f || y1 - y0 > rh / 8.f) c[j].big = 1;
        c[j].n++;
    }
    for (int j = 0; j < nc && nmesh < MESH_MAX; j++)
        if (c[j].n >= 100 && !c[j].big && c[j].x1 - c[j].x0 > rw * 0.5f && c[j].y1 - c[j].y0 > rh * 0.5f &&
            c[j].x0 > -0.5f * rw && c[j].x1 < 1.5f * rw && c[j].x0 < rw * 0.25f && c[j].x1 > rw * 0.75f) {
            mesh[nmesh].z = c[j].z; mesh[nmesh].x0 = c[j].x0; mesh[nmesh].x1 = c[j].x1;
            mesh[nmesh].y0 = c[j].y0; mesh[nmesh].y1 = c[j].y1; mesh[nmesh].n = c[j].n; nmesh++;
        }
}
static void mesh_strip(const pvr_vtx *src, float x0, float y0, float x1, float y1, const float *c00, const float *c10, const float *c11, const float *c01) {
    pvr_vtx q[4];
    const float qx[4] = { x0, x1, x1, x0 }, qy[4] = { y0, y0, y1, y1 };
    const float *qc[4] = { c00, c10, c11, c01 };
    for (int k = 0; k < 4; k++) { q[k] = *src; q[k].x = qx[k]; q[k].y = qy[k]; memcpy(q[k].c, qc[k], 16); memcpy(q[k].o, src->o, 16); }
    push(&q[0]); push(&q[1]); push(&q[2]); push(&q[0]); push(&q[2]); push(&q[3]);
}
static int mesh_edge(const pvr_poly *p, const pvr_vtx *v, float rw, float rh, float ext) {
    if (!nmesh) return 0;
    float x0, x1, y0, y1;
    if (!mesh_rect(p, v, &x0, &x1, &y0, &y1)) return 0;
    int j = 0; while (j < nmesh && mesh[j].z != v[0].z) j++;
    if (j == nmesh) return 0;
    /* the corners' colours */
    const float *cTL = v[0].c, *cTR = v[0].c, *cBR = v[0].c, *cBL = v[0].c;
    for (int k = 0; k < 4; k++) {
        int r = fabsf(v[k].x - x1) <= .5f, b = fabsf(v[k].y - y1) <= .5f;
        if (!r && !b) cTL = v[k].c; else if (r && !b) cTR = v[k].c; else if (r && b) cBR = v[k].c; else cBL = v[k].c;
    }
    int L = fabsf(x0 - mesh[j].x0) < .5f, R = fabsf(x1 - mesh[j].x1) < .5f;
    int T = fabsf(y0 - mesh[j].y0) < .5f && mesh[j].y0 > 0.5f, B = fabsf(y1 - mesh[j].y1) < .5f && mesh[j].y1 < rh - 0.5f;
    float lx = -ext, rx = rw + ext, ty = -1.f, by = rh + 1.f;
    if (L && mesh[j].x0 > lx) mesh_strip(&v[0], lx, y0, x0, y1, cTL, cTL, cBL, cBL);
    if (R && mesh[j].x1 < rx) mesh_strip(&v[0], x1, y0, rx, y1, cTR, cTR, cBR, cBR);
    if (T) mesh_strip(&v[0], x0, ty, x1, y0, cTL, cTR, cTR, cTL);
    if (B) mesh_strip(&v[0], x0, y1, x1, by, cBL, cBR, cBR, cBL);
    if (T && L) mesh_strip(&v[0], lx, ty, x0, y0, cTL, cTL, cTL, cTL);
    if (T && R) mesh_strip(&v[0], x1, ty, rx, y0, cTR, cTR, cTR, cTR);
    if (B && L) mesh_strip(&v[0], lx, y1, x0, by, cBL, cBL, cBL, cBL);
    if (B && R) mesh_strip(&v[0], x1, y1, rx, by, cBR, cBR, cBR, cBR);
    return 1;
}
/* dc_pvr_gl_extend_tiles: a wall of pattern tiles (Soulcalibur's opening, Xianghua's scenes: 10 x 8
 * tiles of 64 pixels at one depth, each showing the same whole texture and flipping over in a ripple)
 * continues past the 4:3 edges: each tile in the outer column on a side is repeated outwards a cell
 * at a time, so the pattern fills the screen (also while the tiles turn over from one pattern to the
 * next: tiles of both textures at the depth). Grids: this frame's walls (depth, extent, cell). */
enum { GRID_MAX = 4 };
static struct { float z, x0, x1, cell, u0, u1, w0, w1; } grid[GRID_MAX]; static int ngrid;
static int grid_quad(const pvr_poly *p, const pvr_vtx *v, float *x0, float *x1, float *u0, float *u1, float *w0, float *w1) {
    if (p->list != 0 || p->n != 4 || !((p->isp >> 25) & 1)) return 0;
    *x0 = *x1 = v[0].x; *u0 = *u1 = v[0].u; *w0 = *w1 = v[0].v;
    for (int k = 1; k < 4; k++) {
        if (v[k].z != v[0].z) return 0;
        if (v[k].x < *x0) *x0 = v[k].x; if (v[k].x > *x1) *x1 = v[k].x;
        if (v[k].u < *u0) *u0 = v[k].u; if (v[k].u > *u1) *u1 = v[k].u;
        if (v[k].v < *w0) *w0 = v[k].v; if (v[k].v > *w1) *w1 = v[k].v;
    }
    return 1;
}
static void find_grids(const pvr_frame *f, float rw) {
    enum { NC = 8 };
    struct { float z, x0, x1, cell, u0, u1, w0, w1; int n, odd; } c[NC]; int nc = 0;
    ngrid = 0;
    for (int i = 0; i < f->npolys; i++) {
        const pvr_poly *p = &f->polys[i]; const pvr_vtx *v = f->verts + p->first;
        float x0, x1, u0, u1, w0, w1;
        if (!grid_quad(p, v, &x0, &x1, &u0, &u1, &w0, &w1)) continue;
        int j = 0; while (j < nc && c[j].z != v[0].z) j++;
        if (j == nc) { if (nc == NC) continue; nc++; c[j].z = v[0].z; c[j].x0 = x0; c[j].x1 = x1; c[j].cell = 0;
                       c[j].u0 = u0; c[j].u1 = u1; c[j].w0 = w0; c[j].w1 = w1; c[j].n = 0; c[j].odd = 0; }
        c[j].n++;
        /* only the tiles showing the whole texture count (a stray piece at the depth doesn't) */
        if (fabsf(u0 - c[j].u0) > 1e-3f || fabsf(u1 - c[j].u1) > 1e-3f || fabsf(w0 - c[j].w0) > 1e-3f || fabsf(w1 - c[j].w1) > 1e-3f) { c[j].odd++; continue; }
        if (x0 < c[j].x0) c[j].x0 = x0; if (x1 > c[j].x1) c[j].x1 = x1;
        if (x1 - x0 > c[j].cell) c[j].cell = x1 - x0;
    }
    for (int j = 0; j < nc && ngrid < GRID_MAX; j++)
        if (c[j].odd * 10 <= c[j].n && c[j].n >= 24 && c[j].cell >= 16.f && c[j].cell <= rw / 4.f && c[j].u1 - c[j].u0 > 0.5f && c[j].w1 - c[j].w0 > 0.5f &&
            c[j].x0 >= -1.f && c[j].x0 <= c[j].cell * 0.5f && c[j].x1 <= rw + 1.f && c[j].x1 >= rw - 1.f - c[j].cell * 0.5f) {
            grid[ngrid].z = c[j].z; grid[ngrid].x0 = c[j].x0; grid[ngrid].x1 = c[j].x1; grid[ngrid].cell = c[j].cell;
            grid[ngrid].u0 = c[j].u0; grid[ngrid].u1 = c[j].u1; grid[ngrid].w0 = c[j].w0; grid[ngrid].w1 = c[j].w1; ngrid++;
        }
}
static int grid_member(const pvr_poly *p, const pvr_vtx *v) {
    if (!ngrid) return 0;
    float x0, x1, u0, u1, w0, w1;
    if (!grid_quad(p, v, &x0, &x1, &u0, &u1, &w0, &w1)) return 0;
    for (int j = 0; j < ngrid; j++) if (grid[j].z == v[0].z) return 1;   /* a stray piece at the depth isn't stretched either */
    return 0;
}
static int grid_repeat(const pvr_poly *p, const pvr_vtx *v, float rw, float ext) {
    if (!ngrid) return 0;
    float x0, x1, u0, u1, w0, w1;
    if (!grid_quad(p, v, &x0, &x1, &u0, &u1, &w0, &w1)) return 0;
    int j = 0; while (j < ngrid && grid[j].z != v[0].z) j++;
    if (j == ngrid) return 0;
    if (fabsf(u0 - grid[j].u0) > 1e-3f || fabsf(u1 - grid[j].u1) > 1e-3f || fabsf(w0 - grid[j].w0) > 1e-3f || fabsf(w1 - grid[j].w1) > 1e-3f) return 1;
    float cx = (x0 + x1) * 0.5f, cell = grid[j].cell;
    int sd = cx < grid[j].x0 + cell ? -1 : cx > grid[j].x1 - cell ? 1 : 0;
    if (!sd) return 1;
    for (int k = 1; k <= 8; k++) {
        float dx = (float)(sd * k) * cell;
        if (sd < 0 ? x1 + dx <= -ext : x0 + dx >= rw + ext) break;
        pvr_vtx q[4];
        for (int m = 0; m < 4; m++) { q[m] = v[m]; q[m].x += dx; }
        push(&q[0]); push(&q[1]); push(&q[2]); push(&q[0]); push(&q[2]); push(&q[3]);
    }
    return 1;
}
/* dc_pvr_gl_extend_backdrops: a far, tilted sky card (Soulcalibur's second opening: the sunset sky over
 * the sand flight, a perspective quad that ends just past the 4:3 edge as the camera rolls) continues
 * past its edges as its own mirror image, in its own plane: 1/w, u/w and v/w are linear in screen
 * space, and a texture coordinate reflected about the edge's (u' = 2 u_edge - u) stays linear once
 * multiplied by 1/w, so the strip beyond each edge of constant u or v is drawn with reflected UVs and
 * the card's perspective. Only far cards (1/w under 0.005) among the frame's first pieces that cover a
 * quarter of the picture; the strip stops where 1/w falls to 60% of the card's nearest value. */
typedef struct { float x, y; } sk_pt;
static int sk_clip(const sk_pt *in, int n, sk_pt *out, float A, float B, float C) {   /* keeps A x + B y + C >= 0 */
    int m = 0;
    for (int i = 0; i < n; i++) {
        const sk_pt *a = &in[i], *b = &in[(i + 1) % n];
        float da = A * a->x + B * a->y + C, db = A * b->x + B * b->y + C;
        if (da >= 0) out[m++] = *a;
        if ((da >= 0) != (db >= 0) && m < 15) { float t = da / (da - db); out[m].x = a->x + (b->x - a->x) * t; out[m].y = a->y + (b->y - a->y) * t; m++; }
    }
    return m;
}
static int sky_card(const pvr_poly *p, const pvr_vtx *v, float rw, float rh) {
    if (p->list != 0 || p->n != 4 || p->sprite || !((p->isp >> 25) & 1)) return 0;
    float zmin = 1e30f, zmax = 0;
    for (int k = 0; k < 4; k++) {
        if (!isfinite(v[k].x) || !isfinite(v[k].y) || !(v[k].z > 0) || v[k].z >= 0.005f) return 0;
        if (v[k].z < zmin) zmin = v[k].z; if (v[k].z > zmax) zmax = v[k].z;
    }
    if (zmax - zmin < 1e-6f * zmax) return 0;                    /* flat: overhang_backdrop's */
    const pvr_vtx *o[4] = { &v[0], &v[1], &v[3], &v[2] };       /* the outline of a strip: 0 1 3 2 */
    float area = 0;
    for (int k = 0; k < 4; k++) area += o[k]->x * o[(k + 1) & 3]->y - o[(k + 1) & 3]->x * o[k]->y;
    return fabsf(area) * 0.5f >= rw * rh * 0.25f;
}
static int sky_cards;                                            /* such cards among the frame's first pieces */
static void sky_mirror(const pvr_poly *p, const pvr_vtx *v, int index, float rw, float rh, float ext) {
    /* one card alone (a sky made of several adjacent cards would get mirrored strips over its neighbours) */
    if (index > 16 || sky_cards != 1 || !sky_card(p, v, rw, rh)) return;
    float zmin = 1e30f;
    for (int k = 0; k < 4; k++) if (v[k].z < zmin) zmin = v[k].z;
    const pvr_vtx *o[4] = { &v[0], &v[1], &v[3], &v[2] };
    /* z, u z, v z = a + b x + c y per triangle of the strip (0 1 2 and 1 2 3: the game's UVs follow
     * each triangle, not one projective map of the quad); the corners must lie in one plane */
    float fit[2][3][3], cfit[2][8][3];                           /* and the colours, base and offset */
    for (int t = 0; t < 2; t++) {
        const pvr_vtx *A = &v[t], *B = &v[t + 1], *C = &v[t + 2];
        float d = (B->x - A->x) * (C->y - A->y) - (B->y - A->y) * (C->x - A->x);
        if (fabsf(d) < 1.f) return;
        float qa[3] = { A->z, A->u * A->z, A->v * A->z }, qb[3] = { B->z, B->u * B->z, B->v * B->z }, qc[3] = { C->z, C->u * C->z, C->v * C->z };
        for (int j = 0; j < 3; j++) {
            float gx = ((qb[j] - qa[j]) * (C->y - A->y) - (qc[j] - qa[j]) * (B->y - A->y)) / d;
            float gy = ((qc[j] - qa[j]) * (B->x - A->x) - (qb[j] - qa[j]) * (C->x - A->x)) / d;
            fit[t][j][1] = gx; fit[t][j][2] = gy; fit[t][j][0] = qa[j] - gx * A->x - gy * A->y;
        }
        for (int j = 0; j < 8; j++) {
            float ca = j < 4 ? A->c[j] : A->o[j - 4], cb = j < 4 ? B->c[j] : B->o[j - 4], cc = j < 4 ? C->c[j] : C->o[j - 4];
            float gx = ((cb - ca) * (C->y - A->y) - (cc - ca) * (B->y - A->y)) / d;
            float gy = ((cc - ca) * (B->x - A->x) - (cb - ca) * (C->x - A->x)) / d;
            cfit[t][j][1] = gx; cfit[t][j][2] = gy; cfit[t][j][0] = ca - gx * A->x - gy * A->y;
        }
    }
    {   const pvr_vtx *D = &v[3];
        float zd = fit[0][0][0] + fit[0][0][1] * D->x + fit[0][0][2] * D->y;
        if (fabsf(zd - D->z) > 0.02f * D->z) return;             /* not one plane */
    }
    float cx = 0, cy = 0; for (int k = 0; k < 4; k++) { cx += o[k]->x * .25f; cy += o[k]->y * .25f; }
    static const int edge_tri[4] = { 0, 1, 1, 0 };               /* outline edges 0-1, 1-3, 3-2, 2-0 */
    for (int e = 0; e < 4; e++) {
        const pvr_vtx *a = o[e], *b = o[(e + 1) & 3];
        const float *fz = fit[edge_tri[e]][0], *fu = fit[edge_tri[e]][1], *fv = fit[edge_tri[e]][2];
        int byu = fabsf(a->u - b->u) < 1e-3f, byv = fabsf(a->v - b->v) < 1e-3f;
        if (byu == byv) continue;                                /* reflect about u = const or v = const */
        float ev = byu ? a->u : a->v;
        /* the side of the edge away from the card */
        float LA = -(b->y - a->y), LB = b->x - a->x, LC = -(LA * a->x + LB * a->y);
        if (LA * cx + LB * cy + LC > 0) { LA = -LA; LB = -LB; LC = -LC; }
        sk_pt r0[16] = { { -ext, -1.f }, { rw + ext, -1.f }, { rw + ext, rh + 1.f }, { -ext, rh + 1.f } }, r1[16], r2[16];
        int n = sk_clip(r0, 4, r1, LA, LB, LC);
        if (n < 3) continue;
        n = sk_clip(r1, n, r2, fz[1], fz[2], fz[0] - 0.6f * zmin);    /* stay in front of the horizon */
        if (n < 3) continue;
        pvr_vtx q[16];
        for (int k = 0; k < n; k++) {
            q[k] = v[0];
            q[k].x = r2[k].x; q[k].y = r2[k].y;
            float z = fz[0] + fz[1] * q[k].x + fz[2] * q[k].y;
            q[k].z = z;
            float u = (fu[0] + fu[1] * q[k].x + fu[2] * q[k].y) / z, w = (fv[0] + fv[1] * q[k].x + fv[2] * q[k].y) / z;
            if (byu) u = 2.f * ev - u; else w = 2.f * ev - w;
            q[k].u = u; q[k].v = w;
            {   /* colours at the mirrored point: the edge's foot plus the distance back into the card */
                float t2 = LA * q[k].x + LB * q[k].y + LC, nn = LA * LA + LB * LB;
                float mx = q[k].x - 2.f * LA * t2 / nn, my = q[k].y - 2.f * LB * t2 / nn;
                const float (*cf)[3] = cfit[edge_tri[e]];
                for (int j = 0; j < 8; j++) {
                    float c = cf[j][0] + cf[j][1] * mx + cf[j][2] * my;
                    c = c < 0 ? 0 : c > 1 ? 1 : c;
                    if (j < 4) q[k].c[j] = c; else q[k].o[j - 4] = c;
                }
            }
            /* no deeper than the edge: the card's fog (by depth) would darken the far strip */
            float ze = a->z < b->z ? a->z : b->z;
            if (q[k].z < ze) q[k].z = ze;
        }
        for (int k = 1; k + 1 < n; k++) { push(&q[0]); push(&q[k]); push(&q[k + 1]); }
    }
}
int dc_pvr_gl_extend_mirror;
static void extend_backdrop(const pvr_poly *p, const pvr_vtx *v, float rw, float ext) {
    if (p->n < 3 || p->n > 64) return;
    const pvr_vtx *e[2][64]; int ne[2] = { 0, 0 };
    for (int k = 0; k < p->n; k++) {
        if (v[k].x < -1.f || v[k].x > rw + 1.f) { overhang_backdrop(p, v, rw, ext); return; }   /* else 3D, not a backdrop */
        if (v[k].x <= 1.f) e[0][ne[0]++] = &v[k];
        else if (v[k].x >= rw - 2.5f) e[1][ne[1]++] = &v[k];   /* 639 in a 641-wide picture (Soulcalibur's save screen) */
    }
    /* both sides: a picture made of tiles (each touching one side) would get stray streaks; its
     * frame keeps the blurred edge-colour bars instead */
    if (ne[0] < 2 || ne[1] < 2) {
        if (dc_pvr_gl_extend_tiles) mirror_tile(p, v, rw, ext);
        return;
    }
    /* dc_pvr_gl_extend_mirror: a textured card with two corners on each side (a letterboxed shot's sky)
     * continues as its mirror image rather than its edge column repeated (streaks): the strip's outer
     * corners take what the card shows as far inside from that edge (perspective-correct) */
    int mir = dc_pvr_gl_extend_mirror && p->n == 4 && ne[0] == 2 && ne[1] == 2 && ((p->isp >> 25) & 1) && ext < rw;
    if (mir) for (int sd = 0; sd < 2; sd++) {
        qsort(e[0], 2, sizeof e[0][0], cmp_vy); qsort(e[1], 2, sizeof e[1][0], cmp_vy);
        pvr_vtx o[2];
        for (int k = 0; k < 2; k++) {
            const pvr_vtx *in = e[sd][k], *far = e[!sd][k];
            float t = ext / fabsf(far->x - in->x);
            if (t > 1.f) t = 1.f;
            float zi = in->z, zf = far->z, z = zi + (zf - zi) * t;
            o[k] = *in;
            o[k].x = sd ? rw + ext : -ext;
            o[k].y = in->y + (far->y - in->y) * t;
            o[k].z = zi;                                       /* depth (and fog) of the edge */
            if (z > 0) {
                o[k].u = (in->u * zi + (far->u * zf - in->u * zi) * t) / z;
                o[k].v = (in->v * zi + (far->v * zf - in->v * zi) * t) / z;
            }
            for (int j = 0; j < 4; j++) { o[k].c[j] = in->c[j] + (far->c[j] - in->c[j]) * t; o[k].o[j] = in->o[j] + (far->o[j] - in->o[j]) * t; }
            o[k].y = in->y;
        }
        const pvr_vtx *a = e[sd][0], *b = e[sd][1];
        push(a); push(b); push(&o[0]); push(&o[0]); push(b); push(&o[1]);
    }
    else for (int sd = 0; sd < 2; sd++) {
        qsort(e[sd], (size_t)ne[sd], sizeof e[sd][0], cmp_vy);
        for (int k = 0; k + 1 < ne[sd]; k++) {
            const pvr_vtx *a = e[sd][k], *b = e[sd][k + 1];
            if (b->y - a->y < 0.5f) continue;
            pvr_vtx a2 = *a, b2 = *b;
            a2.x = sd ? rw + ext : -ext; b2.x = a2.x;
            push(a); push(b); push(&a2); push(&a2); push(b); push(&b2);
        }
    }
}

/* triangle with the PVR's culling (sign of the screen area; odd strip triangles are flipped) */
/* dc_pvr_gl_camera_plane (a port opts in; Soulcalibur): pieces with corners on the camera plane are
 * held over from the last frame (small ones) or drop out (their triangles). Off, they are drawn as
 * they come, as before: Sega Rally 2's widened view has ground pieces beside the camera like that,
 * and dropping them left holes of background colour at the widescreen edges. */
int dc_pvr_gl_camera_plane;
static void tri(const pvr_poly *p, const pvr_vtx *a, const pvr_vtx *b, const pvr_vtx *c, int flip, float cull_small) {
    /* a corner on the camera plane (1/w clamped to 1e30, or at infinity) next to ordinary ones: the
     * triangle would be drawn as a flat smear of one texel in front of everything (the opening's sand
     * floor under the camera), so it drops out */
    if (dc_pvr_gl_camera_plane) {
        int on = (a->z >= 1e29f || !isfinite(a->x + a->y + a->z)) + (b->z >= 1e29f || !isfinite(b->x + b->y + b->z)) + (c->z >= 1e29f || !isfinite(c->x + c->y + c->z));
        if (on && on < 3) return;
    }
    float area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
    if (area == 0) return;
    int cull = (p->isp >> 27) & 3;
    float sarea = flip ? -area : area;
    if (cull == 1 && fabsf(area) < cull_small) return;
    if (cull == 2 && sarea < 0) return;
    if (cull == 3 && sarea > 0) return;
    /* the bounds where the triangle lands after push() shifts it (HUD and split-screen moves); full-screen
     * quads stretched to the edges or showing a capture are placed by their corners: left alone */
    float x0 = tgt_x0 - push_dx, x1 = tgt_x1 - push_dx;
    if (push_wide > 0 || cap_uv >= 0 ||
        (a->x >= x0 && a->x <= x1 && b->x >= x0 && b->x <= x1 && c->x >= x0 && c->x <= x1 &&
         a->y >= tgt_y0 && a->y <= tgt_y1 && b->y >= tgt_y0 && b->y <= tgt_y1 && c->y >= tgt_y0 && c->y <= tgt_y1)) {
        push(a); push(b); push(c); return;
    }
    pvr_vtx p0[9], p1[9]; int n;
    p0[0] = *a; p0[1] = *b; p0[2] = *c;
    if ((n = clip_edge(p0, 3, p1, 0, x0, 0)) < 3) return;
    if ((n = clip_edge(p1, n, p0, 0, x1, 1)) < 3) return;
    if ((n = clip_edge(p0, n, p1, 1, tgt_y0, 0)) < 3) return;
    if ((n = clip_edge(p1, n, p0, 1, tgt_y1, 1)) < 3) return;
    for (int k = 1; k + 1 < n; k++) { push(&p0[0]); push(&p0[k]); push(&p0[k + 1]); }   /* same winding */
}

static unsigned char *poly_hud; static const pvr_poly *poly_base;
/* per polygon: bit 0 (bit 1 for the second parameter set) when its UVs stay within one copy of the
 * texture (0..1): it samples clamped to the texture's edge, so bilinear filtering at a border doesn't
 * read the opposite side (a seam where two textures meet: Space Race's start banner) */
static unsigned char *poly_edge;
/* translucent polygons, opaque parts first (dc_pvr_gl_topaque, auto-sort only): the PVR sorts translucent
 * fragments per pixel, the GL path per polygon. A car's decals and the coplanar layers over its paint
 * (Daytona USA 2001) come out in pieces when whole polygons, or their triangles, are ordered by depth.
 * Fragments a normal alpha blend draws fully opaque come out the same in any order, so pass 1 draws just
 * those, writing depth; pass 2 draws the rest sorted, behind what pass 1 drew hidden. */
int dc_pvr_gl_topaque;
static int tpass;                         /* 0 off, 1 the opaque fragments, 2 the rest */
static const pvr_frame *tp_frame;         /* the frame being drawn */
static int two_pass_poly(const pvr_poly *p) {
    int zmode = (p->isp >> 29) & 7;
    if (!(p->list == 2 && ((p->tsp >> 29) & 7) == 4 && ((p->tsp >> 26) & 7) == 5 && (zmode == 4 || zmode == 6))) return 0;
    if (!tp_frame) return 0;
    /* flat polygons (one depth: 2D screens, menus) keep their drawing order: layers of a menu at one depth
     * rely on it, and pass 1's depth writes would hide later layers (Daytona's menus lost their panels) */
    const pvr_vtx *v = tp_frame->verts + p->first;
    for (int j = 1; j < p->n; j++) if (v[j].z != v[0].z) return 1;
    return 0;
}
static void set_poly_state(const pvr_poly *p, int set, int hud) {
    uint32_t tsp = set ? p->tsp1 : p->tsp, tcw = set ? p->tcw1 : p->tcw;
    int tex = (p->isp >> 25) & 1;
    int tp = tpass && two_pass_poly(p) ? tpass : 0;
    sv_use((tex ? SV_TEX : 0) | SV_SHADE((tsp >> 6) & 3) | (((tsp >> 20) & 1) ? SV_USE_ALPHA : 0) | (((tsp >> 19) & 1) ? SV_IGN_TA : 0)
           | (((p->isp >> 24) & 1) ? SV_OFFSET : 0) | (((p->isp >> 23) & 1) ? SV_GOURAUD : 0) | SV_FOG((tsp >> 22) & 3)
           | SV_PT(p->list == 4 ? (ms_a2c ? 2 : 1) : 0) | SV_TPASS(tp) | SV_HUD(hud & 3) | (set ? SV_SET : 0));
    if (ms_a2c) { if (p->list == 4) p_glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE); else p_glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE); }
    if (tex) {
        int ci = poly_cap && poly_base ? poly_cap[p - poly_base] : 0;
        p_glActiveTexture(GL_TEXTURE0); p_glBindTexture(GL_TEXTURE_2D, ci ? cap[ci - 1].tex : texture_for(tcw, tsp));
        if (ci) {
            p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        } else {
        int filt = ((tsp >> 13) & 3) ? GL_LINEAR : GL_NEAREST;
        int minf = filt == GL_LINEAR && dc_pvr_gl_mipmaps ? GL_LINEAR_MIPMAP_LINEAR : filt;   /* point-sampled stays sharp */
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minf); p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt);
        if (max_aniso >= 1.f)
            p_glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY,
                              minf == GL_LINEAR_MIPMAP_LINEAR && dc_pvr_gl_anisotropy > 1.f ? (dc_pvr_gl_anisotropy < max_aniso ? dc_pvr_gl_anisotropy : max_aniso) : 1.f);
        int cu = (tsp >> 16) & 1, cv = (tsp >> 15) & 1, fu = (tsp >> 18) & 1, fv = (tsp >> 17) & 1;
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, cu ? GL_CLAMP_TO_EDGE : fu ? GL_MIRRORED_REPEAT : GL_REPEAT);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, cv ? GL_CLAMP_TO_EDGE : fv ? GL_MIRRORED_REPEAT : GL_REPEAT);
        if (poly_edge && poly_base && ((poly_edge[p - poly_base] >> set) & 1)) {
            p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        }
    }
    static const GLenum depth_fn[8] = { GL_NEVER, 0x0201, GL_EQUAL, 0x0203, 0x0204, 0x0205, GL_GEQUAL, GL_ALWAYS };
    int zmode = (p->isp >> 29) & 7;
    if (p->list == 4 && zmode == 0) zmode = 6;
    p_glDepthFunc(depth_fn[zmode]);
    p_glDepthMask(p->list != 2 && !((p->isp >> 26) & 1));
    if (tp == 1) p_glDepthMask(1);
}
static GLenum blend_src(int i) { static const GLenum t[8] = { GL_ZERO, GL_ONE, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA }; return t[i & 7]; }
static GLenum blend_dst(int i) { static const GLenum t[8] = { GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA }; return t[i & 7]; }

static int quad_first;                 /* full-screen quad in the vertex buffer */
static void draw_quad(float r, float g, float b, float a) {
    uint32_t back = sv_cur ? sv_cur->key : SV_PLAIN;
    sv_use(SV_QUAD);
    if (sv_cur && sv_cur->quad_col >= 0) p_glUniform4f(sv_cur->quad_col, r, g, b, a);
    p_glDrawArrays(GL_TRIANGLES, quad_first, 6);
    sv_use(back & ~SV_ZV);                                  /* what was in use: the caller goes on drawing with it */
}

typedef struct { int idx; float key; } sort_e;
int dc_pvr_gl_tsort2 = 1;
typedef struct { int first, count, poly; float key; } tri_e;
static int cmp_tri(const void *a, const void *b) {     /* by key, then submission (vertex) order */
    const tri_e *p = a, *q = b;
    if (p->key != q->key) return p->key < q->key ? -1 : 1;
    return p->first < q->first ? -1 : p->first > q->first;
}
static int cmp_sort(const void *a, const void *b) {
    /* equal depth keeps submission order: qsort is not stable everywhere (MSVC's is not), and
     * text drawn at one depth then lost letters at random */
    const sort_e *p = a, *q = b;
    if (p->key != q->key) return p->key < q->key ? -1 : 1;
    return p->idx < q->idx ? -1 : p->idx > q->idx;
}

/* Draw batching: consecutive polygons with the same render state go out in one call (menus can
 * have thousands of small polygons, and per-call overhead dominates). Draw order is unchanged. */
typedef struct { uint32_t isp, tsp, tcw; int list, set, mark, blend, uclip, ux0, uy0, ux1, uy1, hud; float ub[4]; int cap, edge; } bkey;
/* HUD polygons (flat, at or above dc_pvr_gl_hud_z) get the sharper texture filter */
int dc_pvr_gl_hud_sharp = 1;
int dc_pvr_gl_hud_move = 1;
int (*dc_pvr_gl_hud_pin)(float x0, float y0, float x1, float y1);
int dc_pvr_gl_hud_split_wide;
static int is_hud(const pvr_poly *p) { return poly_hud && poly_base ? poly_hud[p - poly_base] : 0; }
static float (*poly_uvbox)[4];          /* per polygon: its UV range (2D overlay pieces that sample clamped to it) */
/* scissor: the frame's clip rectangle, narrowed per polygon by the PVR's user tile clip (split screen) */
static int sc_x, sc_y, sc_w, sc_h, sc_set[4];
static float sc_off, sc_k, sc_kx; static int sc_scale, sc_rw;
static void scissor(int x, int y, int w, int h) {
    if (w < 0) w = 0; if (h < 0) h = 0;
    if (sc_set[0] == x && sc_set[1] == y && sc_set[2] == w && sc_set[3] == h) return;
    sc_set[0] = x; sc_set[1] = y; sc_set[2] = w; sc_set[3] = h;
    p_glScissor(x, y, w, h);
}
/* split-screen widening (dc_pvr_gl_split_panel_x): views clipped to [0, panel_x) beside a side panel
 * are shifted to centre and widened to the whole width; the panel's backdrop goes, its contents
 * (map, timer) move to the right edge */
int dc_pvr_gl_split_panel_x;
static int split_wide;                 /* this frame has such views */
static int split_view(const pvr_poly *p) {
    return split_wide && p->uclip == 2 && p->ux0 <= 0 && p->ux1 == dc_pvr_gl_split_panel_x;
}
static void scissor_for(const pvr_poly *p) {
    if (p->uclip != 2) { scissor(sc_x, sc_y, sc_w, sc_h); return; }
    /* a rectangle at the picture's left or right edge reaches the edge of the (widescreen) target */
    int x0 = p->ux0 <= 0 ? 0 : (int)floorf((sc_off + (float)p->ux0 * sc_kx) * sc_k);
    int x1 = p->ux1 >= sc_rw || split_view(p) ? 1 << 20 : (int)ceilf((sc_off + (float)p->ux1 * sc_kx) * sc_k);
    int y0 = fb_h - p->uy1 * sc_scale / 2, y1 = fb_h - p->uy0 * sc_scale / 2;     /* GL rows go up */
    if (x0 < sc_x) x0 = sc_x; if (x1 > sc_x + sc_w) x1 = sc_x + sc_w;
    if (y0 < sc_y) y0 = sc_y; if (y1 > sc_y + sc_h) y1 = sc_y + sc_h;
    scissor(x0, y0, x1 - x0, y1 - y0);
}
static struct { bkey key; int valid; GLint *first; GLsizei *count; int n, cap; } B;
/* DC_GL_STATS=1: what a frame costs to draw, averaged and printed every 300 frames (polygons, vertices
 * sent, state changes, draw calls) */
static struct { int on; unsigned long frames, polys, verts, states, draws, ranges, mods, tav, sunk; } gst = { -1, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static void batch_flush(void) {
    if (B.n && gst.on > 0) { gst.draws++; gst.ranges += (unsigned long)B.n; }
    if (B.n == 1) p_glDrawArrays(GL_TRIANGLES, B.first[0], B.count[0]);
    else if (B.n > 1) p_glMultiDrawArrays(GL_TRIANGLES, B.first, B.count, B.n);
    B.n = 0;
}
static void batch_reset(void) { batch_flush(); B.valid = 0; scissor(sc_x, sc_y, sc_w, sc_h); }
/* mark: stencil reference (-1 = leave the stencil state alone); blend: set the blend function */
static void batch_add(const pvr_poly *p, int set, int mark, int blend, int first, int count) {
    if (!count) return;
    bkey k = { p->isp, set ? p->tsp1 : p->tsp, set ? p->tcw1 : p->tcw, p->list, set, mark, blend,
               p->uclip, p->uclip ? p->ux0 : 0, p->uclip ? p->uy0 : 0, p->uclip ? p->ux1 : 0, p->uclip ? p->uy1 : 0,
               ((p->tsp >> 13) & 3) ? is_hud(p) : (is_hud(p) & 2), {0, 0, 0, 0}, poly_cap && poly_base ? poly_cap[p - poly_base] : 0,
               poly_edge && poly_base ? (poly_edge[p - poly_base] >> set) & 1 : 0 }; /* bicubic for bilinear-filtered HUD textures only */
    if (k.hud & 2) memcpy(k.ub, poly_uvbox[p - poly_base], sizeof k.ub);
    if (!B.valid || memcmp(&k, &B.key, sizeof k)) {
        batch_flush();
        if (gst.on > 0) gst.states++;
        if (mark >= 0) p_glStencilFunc(GL_ALWAYS, mark, 0xFF);
        set_poly_state(p, set, k.hud);
        scissor_for(p);
        if ((k.hud & 2) && sv_cur && sv_cur->uvbox >= 0) p_glUniform4f(sv_cur->uvbox, k.ub[0], k.ub[1], k.ub[2], k.ub[3]);
        if (set) { p_glDepthFunc(GL_EQUAL); p_glDepthMask(0); }
        if (blend) p_glBlendFunc(blend_src((p->tsp >> 29) & 7), blend_dst((p->tsp >> 26) & 7));
        B.key = k; B.valid = 1;
    }
    static int nobatch = -1;                       /* DC_GL_NOBATCH=1: one draw call per polygon (diagnosis) */
    if (nobatch < 0) { const char *e = getenv("DC_GL_NOBATCH"); nobatch = e && *e == '1'; }
    if (nobatch) { p_glDrawArrays(GL_TRIANGLES, first, count); return; }
    if (B.n && B.first[B.n - 1] + B.count[B.n - 1] == first) { B.count[B.n - 1] += count; return; }   /* contiguous */
    if (B.n == B.cap) { B.cap = B.cap ? B.cap * 2 : 256; B.first = realloc(B.first, B.cap * sizeof *B.first); B.count = realloc(B.count, B.cap * sizeof *B.count); }
    B.first[B.n] = first; B.count[B.n] = count; B.n++;
}



/* ------------------------------------------------------------------ widescreen side bars
 * Frames that draw nothing beyond the 4:3 picture (2D menus) get side bars in the picture's own
 * edge colour: each edge strip is copied into a small texture, mipmapped on the GPU, and the bar
 * shows its blurred, darkened average (vertical gradient kept). No readback. */
int dc_pvr_gl_side_fill = 1;
int dc_pvr_gl_letterbox_4_3 = 1;
int dc_pvr_gl_tile_clamp = 1;
int dc_pvr_gl_letterbox_zoom;
int dc_pvr_gl_show_4_3;
static GLuint bar_prog, edge_tex[2], edge_fbo[2];
static GLint bar_x0, bar_x1, bar_tex, bar_flat, bar_col;
static GLint bar_gpu, bar_side, bar_dec;
static const char *bar_vs =
"#version 330 core\n"
"layout(location=0) in vec3 a_pos;\n"
"uniform float u_x0, u_x1; out float v_y;\n"
"void main() { float t = (a_pos.x + 1.0) * 0.5; gl_Position = vec4(mix(u_x0, u_x1, t), a_pos.y, 0.0, 1.0); v_y = (a_pos.y + 1.0) * 0.5; }\n";
static const char *bar_fs =
"#version 330 core\n"
"in float v_y; uniform sampler2D u_tex, u_dec; uniform int u_flat, u_gpu, u_side; uniform vec3 u_col; out vec4 o_col;\n"
"void main() {\n"
"  if (u_gpu != 0) {\n"                                /* gpu_bars_decide's verdict for this side */
"    vec4 d = texelFetch(u_dec, ivec2(u_side, 0), 0);\n"
"    if (d.r < 0.5) discard;\n"
"    if (d.g > 0.5) { o_col = vec4(texelFetch(u_tex, ivec2(0), 0).rgb, 1.0); return; }\n"
"  }\n"
/* an edge of one flat colour (the white of Tecmo's logo screen, a plain backdrop) continues as
 * exactly that colour, so the bars don't show */
"  if (u_flat != 0) { o_col = vec4(u_col, 1.0); return; }\n"

"  vec3 c = textureLod(u_tex, vec2(0.5, v_y), 3.0).rgb;\n"
"  float l = max(c.r, max(c.g, c.b));\n"
/* near-black pictures (the logo and legal screens: black with some text near the edge) keep black
 * bars instead of a faint grey smear; others, dark blue panels included, get the blurred, darkened
 * edge colour */
"  o_col = vec4(c * 0.75 * smoothstep(0.06, 0.16, l), 1.0);\n"
"}\n";
#define EDGE_W 16
#define EDGE_H 64
static void bars_init(void) {
    bar_prog = p_glCreateProgram();
    p_glAttachShader(bar_prog, compile(GL_VERTEX_SHADER, bar_vs));
    p_glAttachShader(bar_prog, compile(GL_FRAGMENT_SHADER, bar_fs));
    p_glLinkProgram(bar_prog);
    GLint ok = 0; p_glGetProgramiv(bar_prog, GL_LINK_STATUS, &ok);
    if (!ok) { bar_prog = 0; return; }
    bar_x0 = p_glGetUniformLocation(bar_prog, "u_x0"); bar_x1 = p_glGetUniformLocation(bar_prog, "u_x1");
    bar_tex = p_glGetUniformLocation(bar_prog, "u_tex");
    bar_flat = p_glGetUniformLocation(bar_prog, "u_flat"); bar_col = p_glGetUniformLocation(bar_prog, "u_col");
    bar_gpu = p_glGetUniformLocation(bar_prog, "u_gpu"); bar_side = p_glGetUniformLocation(bar_prog, "u_side"); bar_dec = p_glGetUniformLocation(bar_prog, "u_dec");
    for (int i = 0; i < 2; i++) {
        p_glGenTextures(1, &edge_tex[i]); p_glBindTexture(GL_TEXTURE_2D, edge_tex[i]);
        p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, EDGE_W, EDGE_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR); p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        p_glGenerateMipmap(GL_TEXTURE_2D);
        p_glGenFramebuffers(1, &edge_fbo[i]); p_glBindFramebuffer(GL_FRAMEBUFFER, edge_fbo[i]);
        p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, edge_tex[i], 0);
    }
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
/* side = 0 left, 1 right; picture columns [px0, px1) of the FBO, rows [y0, y1) */
/* the picture's edge strips, blurred, into edge_tex (what the bars show) */
static int edges_ok(int px0, int px1, int y0, int y1) { return bar_prog && px0 > 0 && y1 > y0 && px1 < fb_w && px1 - px0 >= 8; }
static void edge_blit(int px0, int px1, int y0, int y1) {
    int strip = (px1 - px0) / 40 > 2 ? (px1 - px0) / 40 : 2;
    for (int i = 0; i < 2; i++) {
        int sx = i ? px1 - 1 - strip : px0 + 1;         /* inside the picture, past its (often dark) edge pixel */
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo); p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, edge_fbo[i]);
        p_glBlitFramebuffer(sx, y0, sx + strip, y1, 0, 0, EDGE_W, EDGE_H, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        p_glBindTexture(GL_TEXTURE_2D, edge_tex[i]); p_glGenerateMipmap(GL_TEXTURE_2D);
    }
}
/* one flat colour down the whole edge (every texel within 3% of the first)? */
static int edge_is_flat(const uint8_t *px, float col[3]) {
    int same = 1;
    for (int k = 1; k < EDGE_W * EDGE_H && same; k++)
        for (int ch = 0; ch < 3; ch++) if (abs((int)px[k * 4 + ch] - (int)px[ch]) > 8) { same = 0; break; }
    for (int ch = 0; ch < 3; ch++) col[ch] = px[ch] / 255.f;
    return same;
}
static int column_empty(const uint8_t *col, int n) {      /* under 5% of the column drawn */
    int lit = 0;
    for (int y = 0; y < n; y++) if (col[y * 4] + col[y * 4 + 1] + col[y * 4 + 2] > 24) lit++;
    return lit * 20 < n;
}
/* side bars decided on the GPU. Reading the checks back (glReadPixels) makes the CPU wait until the
 * GPU has finished the frame, so the two stop working side by side; on the Switch that cost menus and the
 * title screen a third of the frame (and pixel-pack buffers didn't help: Mesa waits there too). With
 * dc_pvr_gl_gpu_bars the columns just outside the picture are copied into col_tex, a 2-texel pass
 * (dec_prog) makes the same checks into dec_tex, and the bars' shader reads its side's verdict:
 * the same bars, this very frame, with nothing coming back to the CPU. */
int dc_pvr_gl_gpu_bars = 1;
static GLuint col_tex, col_fbo, dec_tex, dec_fbo, dec_prog; static int col_h, gpu_bars_failed;
static GLint dec_n, dec_e0, dec_e1, dec_c;
static const char *dec_fs =
"#version 330 core\n"
"uniform sampler2D u_e0, u_e1, u_c; uniform int u_n; out vec4 o_col;\n"
"void main() {\n"
"  int sd = int(gl_FragCoord.x);\n"
"  int lit = 0;\n"                                     /* the column past the picture: under 5% drawn is empty */
"  for (int y = 0; y < u_n; y++) { vec3 c = floor(texelFetch(u_c, ivec2(sd, y), 0).rgb * 255.0 + 0.5); if (c.r + c.g + c.b > 24.0) lit++; }\n"
"  float fill = lit * 20 < u_n ? 1.0 : 0.0;\n"
"  float fl = 1.0;\n"                                /* the picture's edge one flat colour (within 8/255)? */
"  vec3 c0 = floor((sd == 0 ? texelFetch(u_e0, ivec2(0), 0) : texelFetch(u_e1, ivec2(0), 0)).rgb * 255.0 + 0.5);\n"
"  for (int k = 1; k < " "1024" " && fl > 0.5; k++) {\n"
"    ivec2 q = ivec2(k % 16, k / 16);\n"
"    vec3 c = floor((sd == 0 ? texelFetch(u_e0, q, 0) : texelFetch(u_e1, q, 0)).rgb * 255.0 + 0.5);\n"
"    if (any(greaterThan(abs(c - c0), vec3(8.0)))) fl = 0.0;\n"
"  }\n"
"  o_col = vec4(fill, fl, 0.0, 1.0);\n"
"}\n";
static const char *dec_vs =
"#version 330 core\n"
"layout(location=0) in vec3 a_pos;\n"
"void main() { gl_Position = vec4(a_pos.xy, 0.0, 1.0); }\n";
static int gpu_bars_init(int h) {
    if (gpu_bars_failed) return 0;
    if (!dec_prog) {
        dec_prog = p_glCreateProgram();
        p_glAttachShader(dec_prog, compile(GL_VERTEX_SHADER, dec_vs)); p_glAttachShader(dec_prog, compile(GL_FRAGMENT_SHADER, dec_fs));
        p_glLinkProgram(dec_prog);
        GLint ok = 0; p_glGetProgramiv(dec_prog, GL_LINK_STATUS, &ok);
        if (!ok) { dec_prog = 0; gpu_bars_failed = 1; fprintf(stderr, "pvr_gl: side bars decided on the CPU (shader)\n"); return 0; }
        dec_n = p_glGetUniformLocation(dec_prog, "u_n"); dec_c = p_glGetUniformLocation(dec_prog, "u_c");
        dec_e0 = p_glGetUniformLocation(dec_prog, "u_e0"); dec_e1 = p_glGetUniformLocation(dec_prog, "u_e1");
        p_glGenTextures(1, &dec_tex); p_glBindTexture(GL_TEXTURE_2D, dec_tex);
        p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        p_glGenFramebuffers(1, &dec_fbo); p_glBindFramebuffer(GL_FRAMEBUFFER, dec_fbo);
        p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dec_tex, 0);
    }
    if (h > col_h) {
        if (!col_tex) { p_glGenTextures(1, &col_tex); p_glGenFramebuffers(1, &col_fbo); }
        p_glBindTexture(GL_TEXTURE_2D, col_tex);
        p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        p_glBindFramebuffer(GL_FRAMEBUFFER, col_fbo);
        p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, col_tex, 0);
        col_h = h;
    }
    return 1;
}
/* the checks, into dec_tex: (fill, flat) for the left and right side */
static void gpu_bars_decide(int cx0, int cx1, int y0, int y1) {
    int n = y1 - y0;
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo); p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, col_fbo);
    p_glBlitFramebuffer(cx0, y0, cx0 + 1, y1, 0, 0, 1, n, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    p_glBlitFramebuffer(cx1, y0, cx1 + 1, y1, 1, 0, 2, n, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    p_glBindFramebuffer(GL_FRAMEBUFFER, dec_fbo);
    p_glViewport(0, 0, 2, 1);
    p_glDisable(GL_SCISSOR_TEST); p_glDisable(GL_DEPTH_TEST); p_glDisable(GL_BLEND); p_glDisable(GL_STENCIL_TEST);
    p_glUseProgram(dec_prog);
    p_glActiveTexture((GL_TEXTURE0 + 1)); p_glBindTexture(GL_TEXTURE_2D, edge_tex[0]);
    p_glActiveTexture((GL_TEXTURE0 + 2)); p_glBindTexture(GL_TEXTURE_2D, edge_tex[1]);
    p_glActiveTexture((GL_TEXTURE0 + 3)); p_glBindTexture(GL_TEXTURE_2D, col_tex);
    p_glActiveTexture(GL_TEXTURE0);
    p_glUniform1i(dec_e0, 1); p_glUniform1i(dec_e1, 2); p_glUniform1i(dec_c, 3); p_glUniform1i(dec_n, n);
    p_glBindVertexArray(vao);
    p_glDrawArrays(GL_TRIANGLES, quad_first, 6);
    sv_cur = NULL;
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    p_glViewport(0, 0, fb_w, fb_h);
}
/* side = 0 left, 1 right; picture columns [px0, px1) of the FBO, rows [y0, y1). gpu: edge_tex already
 * holds this frame's edges and dec_tex the verdicts (both sides drawn, each keeps or drops itself) */
static void draw_bars(int px0, int px1, int y0, int y1, int left, int right, int gpu) {
    if (!edges_ok(px0, px1, y0, y1)) return;
    int flat[2] = { 0, 0 }; float col[2][3] = { { 0 } };
    if (!gpu) {
        edge_blit(px0, px1, y0, y1);
        for (int i = 0; i < 2; i++) {
            static uint8_t px[EDGE_W * EDGE_H * 4];
            p_glBindFramebuffer(GL_READ_FRAMEBUFFER, edge_fbo[i]); p_glPixelStorei(GL_PACK_ALIGNMENT, 1);
            p_glReadPixels(0, 0, EDGE_W, EDGE_H, GL_RGBA, GL_UNSIGNED_BYTE, px);
            flat[i] = edge_is_flat(px, col[i]);
        }
    }
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    p_glViewport(0, 0, fb_w, fb_h);
    p_glEnable(GL_SCISSOR_TEST); p_glScissor(0, y0, fb_w, y1 - y0);
    p_glDisable(GL_DEPTH_TEST); p_glDisable(GL_BLEND); p_glDisable(GL_STENCIL_TEST);
    p_glUseProgram(bar_prog);
    p_glUniform1i(bar_gpu, gpu); p_glUniform1i(bar_dec, 4);
    if (gpu) { p_glActiveTexture((GL_TEXTURE0 + 4)); p_glBindTexture(GL_TEXTURE_2D, dec_tex); }
    p_glUniform1i(bar_tex, 0); p_glActiveTexture(GL_TEXTURE0);
    p_glBindVertexArray(vao);
    for (int i = 0; i < 2; i++) {
        if (!(i ? right : left)) continue;
        float a = i ? (float)(px1 - 1) : 0.f, b = i ? (float)fb_w : (float)(px0 + 1);   /* cover the edge pixel */
        p_glUniform1f(bar_x0, a / fb_w * 2.f - 1.f); p_glUniform1f(bar_x1, b / fb_w * 2.f - 1.f);
        p_glUniform1i(bar_flat, flat[i]); p_glUniform3f(bar_col, col[i][0], col[i][1], col[i][2]); p_glUniform1i(bar_side, i);
        p_glBindTexture(GL_TEXTURE_2D, edge_tex[i]);
        p_glDrawArrays(GL_TRIANGLES, quad_first, 6);
    }
    sv_cur = NULL;
    p_glDisable(GL_SCISSOR_TEST);
}
/* vertical centring: the share of the picture height to move it down by (the drawn lines are
 * centred in the display; Rush shows 470 lines and draws 4..443) */
static float pic_offy;
/* dc_pvr_gl_fill_lines: show only the lines the game draws, filling the window height (no black bands
 * above and below; Rush draws 440 of its 470 lines, so the picture is ~7% taller). Rows of the FBO
 * (GL rows go up) holding those lines; 0/0 = the whole picture. */
int dc_pvr_gl_fill_lines = 1;
static int pic_y0, pic_y1;
/* a letterboxed cut-scene zoomed to fill the window: the FBO rectangle between the bars (0 = none) */
static int zoom_x0, zoom_x1, zoom_y0, zoom_y1;
/* debug (DC_GL_DEBUG): draw a known 4x4 texture through the real shader and read the result back,
 * at several 1/w depths and filter settings, and log what the driver produced */
static void gl_selftest(void) {
    static int done;
    if (done || !dbg_log || !fbo) return;
    done = 1;
    void (*getiv)(GLenum, GLint *) = NULL;
    uint32_t tx[16];
    for (int i = 0; i < 16; i++) tx[i] = 0xFF000000u | (uint32_t)(i * 16 + 8) << 16 | (uint32_t)(255 - i * 16) << 8 | (uint32_t)((i & 3) * 80);
    GLuint t; p_glGenTextures(1, &t); p_glActiveTexture(GL_TEXTURE0); p_glBindTexture(GL_TEXTURE_2D, t);
    p_glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_BGRA, GL_UNSIGNED_BYTE, tx);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo); p_glViewport(0, 0, fb_w, fb_h);
    p_glDisable(GL_SCISSOR_TEST); p_glDisable(GL_DEPTH_TEST); p_glDisable(GL_STENCIL_TEST); p_glDisable(GL_BLEND); p_glDisable(GL_CULL_FACE);
    p_glColorMask(1, 1, 1, 1);
    FU.sx = 2.f / 640.f; FU.sy = 2.f / 480.f; FU.offx = 0; sv_stamp++; sv_cur = NULL;
    sv_use(SV_TEX | SV_SHADE(0) | SV_USE_ALPHA | SV_GOURAUD | SV_FOG(2));
    static const float zs[4] = { 1.f, 0.01f, 1000.f, -1.f };   /* -1: per-vertex 1/w 0.5 .. 2 (perspective) */
    const char *names[4] = { "1/w=1", "1/w=0.01", "1/w=1000", "perspective" };
    uint8_t *px = malloc((size_t)fb_w * fb_h * 4);
    for (int c = 0; c < 4; c++) {
        gv q[6]; memset(q, 0, sizeof q);
        float X[4] = { 0, 64, 0, 64 }, Y[4] = { 0, 0, 64, 64 }, Uu[4] = { 0, 1, 0, 1 }, Vv[4] = { 0, 0, 1, 1 };
        int idx[6] = { 0, 1, 2, 1, 3, 2 };
        for (int k = 0; k < 6; k++) {
            int j = idx[k]; gv *g = &q[k];
            g->x = X[j]; g->y = Y[j]; g->z = zs[c] > 0 ? zs[c] : (j < 2 ? 0.5f : 2.f);
            g->u = Uu[j]; g->v = Vv[j]; g->u1 = g->u; g->v1 = g->v;
            for (int m = 0; m < 4; m++) { g->c[m] = g->c1[m] = 1.f; }
        }
        p_glClearColor(1, 0, 1, 1); p_glClear(GL_COLOR_BUFFER_BIT);
        p_glBindVertexArray(vao); p_glBindBuffer(GL_ARRAY_BUFFER, vbo);
        p_glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STREAM_DRAW);
        p_glDrawArrays(GL_TRIANGLES, 0, 6);
        p_glPixelStorei(GL_PACK_ALIGNMENT, 1);
        p_glReadPixels(0, 0, fb_w, fb_h, GL_RGBA, GL_UNSIGNED_BYTE, px);
        int bad = 0;
        fprintf(dbg_log, "selftest %s:", names[c]);
        for (int ty = 0; ty < 4; ty++) for (int txi = 0; txi < 4; txi++) {
            /* centre of texel (txi, ty): PVR x = 8 + 16 txi, y = 8 + 16 ty (perspective: just check order) */
            int sx = (int)((8 + 16 * txi) * fb_w / 640.f), sy = fb_h - 1 - (int)((8 + 16 * ty) * fb_h / 480.f);
            const uint8_t *p = px + ((size_t)sy * fb_w + sx) * 4;
            uint32_t e = tx[ty * 4 + txi], got = (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
            int ok = abs((int)((e >> 16) & 255) - p[0]) <= 2 && abs((int)((e >> 8) & 255) - p[1]) <= 2 && abs((int)(e & 255) - p[2]) <= 2;
            if (c == 3) ok = 1;
            if (!ok) bad++;
            fprintf(dbg_log, " %06X%s", got, ok ? "" : (e & 0xFFFFFF) == 0 ? "!" : "*");
        }
        fprintf(dbg_log, "  -> %s\n", bad ? "WRONG" : "ok");
    }
    /* state the driver reports */
    getiv = (void (*)(GLenum, GLint *))p_getproc_saved("glGetIntegerv");
    if (getiv) {
        GLint v[6] = {0};
        getiv(0x8069 /* TEXTURE_BINDING_2D */, &v[0]); getiv(0x84E0 /* ACTIVE_TEXTURE */, &v[1]);
        getiv(0x8B8D /* CURRENT_PROGRAM */, &v[2]); getiv(0x8919 /* SAMPLER_BINDING */, &v[3]);
        fprintf(dbg_log, "state: texture %d (made %u) active unit %X program %d (ours %u) sampler %d\n", v[0], t, v[1], v[2], sv_cur ? sv_cur->prog : 0, v[3]);
    }
    fflush(dbg_log);
    free(px);
    p_glDeleteTextures(1, &t);
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
}


/* widescreen HUD grouping (see dc_pvr_gl_render): side[i] = -1 / +1 for HUD polygons whose group
 * centre lies in the left / right third of the 4:3 picture, 0 otherwise */
typedef struct { float x0, y0, x1, y1; int poly, parent; } hud_box;
static int hud_find(hud_box *b, int i) { while (b[i].parent != i) { b[i].parent = b[b[i].parent].parent; i = b[i].parent; } return i; }
static float hud_group_zmax = 1e30f;    /* polygons at or above this depth are left out (a pause menu) */
static int hud_group_imin;              /* ... when drawn at or after this polygon (dc_pvr_gl_hud_before_shade) */
int dc_pvr_gl_hud_before_shade;
const dc_hud_zone *dc_pvr_gl_hud_zones;
int dc_pvr_gl_hud_sprites;
int dc_pvr_gl_hud_nzones;
int dc_pvr_gl_shade_any_z;
int dc_pvr_gl_hud_panel_rows;
int dc_pvr_gl_hud_bars;
#define HUD_BAR 127                     /* side[i]: a full-width HUD bar, stretched to the screen edges */
#define HUD_MOVE0 64                    /* side[i] 64 + zone: a zone moving by a fraction (dc_hud_zone.move) */
#define HUD_MOVE_N 60
static void hud_group(const pvr_frame *f, int RW, signed char *side) {
    static hud_box *b; static int cap;
    int n = 0;
    for (int i = 0; i < f->npolys; i++) {
        const pvr_poly *p = &f->polys[i];
        const pvr_vtx *v = f->verts + p->first;
        if (p->n < 3) continue;
        int flat = 1, near = 1; float x0 = v[0].x, x1 = v[0].x, y0 = v[0].y, y1 = v[0].y;
        for (int k = 0; k < p->n; k++) {
            /* nearly flat (within 2% in 1/w) counts as flat: Space Race's map shadow tilts a little */
            if (dc_pvr_gl_hud_exact ? v[k].z != v[0].z : fabsf(v[k].z - v[0].z) > 0.02f * v[0].z) flat = 0;
            if (v[k].z < dc_pvr_gl_hud_z || (v[k].z >= hud_group_zmax && i >= hud_group_imin) || (dc_pvr_gl_hud_zmax > 0 && v[k].z >= dc_pvr_gl_hud_zmax)) near = 0;
            if (v[k].x < x0) x0 = v[k].x; if (v[k].x > x1) x1 = v[k].x;
            if (v[k].y < y0) y0 = v[k].y; if (v[k].y > y1) y1 = v[k].y;
        }
        if (!near) continue;
        if (dc_pvr_gl_hud_sprites && !p->sprite) continue;
        /* the game's fixed HUD places: a piece centred in one moves with it, and joins no group */
        int z_ = 0;
        for (; z_ < dc_pvr_gl_hud_nzones; z_++) {
            const dc_hud_zone *hz = &dc_pvr_gl_hud_zones[z_];
            float zs = RW / 640.f;
            float cx = (x0 + x1) * .5f, cy = (y0 + y1) * .5f;
            if (cx >= hz->x0 * zs && cx <= hz->x1 * zs && cy >= hz->y0 * zs && cy <= hz->y1 * zs) {
                side[i] = (signed char)hz->side;
                if (hz->move != 0.f && z_ < HUD_MOVE_N) { side[i] = (signed char)(HUD_MOVE0 + z_); break; }   /* moves by a fraction */
                if (hz->stretch > 0 && hz->side && z_ < 100) side[i] = (signed char)(hz->side * (8 + z_));   /* |side| >= 8: stretches */
                break;
            }
        }
        if (z_ < dc_pvr_gl_hud_nzones) continue;
        /* small 3D HUD models (Rush's spinning coins) count; up to a fifth of the width: Space Race's
         * commentator frame grows past 15% as it slides out, and must keep moving with the portrait */
        if (!flat && (dc_pvr_gl_hud_exact || x1 - x0 > RW * 0.2f)) continue;
        /* a flat bar across the whole 4:3 width, at most a quarter of the height (Propeller Arena's "Return
         * to game" strip): its ends go out to the screen edges (dc_pvr_gl_hud_bars) */
        if (dc_pvr_gl_hud_bars && flat && x0 <= 1.f && x1 >= RW - 1.f && y1 - y0 <= f->height * 0.25f) { side[i] = HUD_BAR; continue; }
        if (x1 - x0 > RW * 0.9f) continue;                    /* full-width bars and fades stay */
        if (n == cap) { cap = cap ? cap * 2 : 1024; b = realloc(b, (size_t)cap * sizeof *b); }
        b[n].x0 = x0; b[n].y0 = y0; b[n].x1 = x1; b[n].y1 = y1; b[n].poly = i; b[n].parent = n; n++;
    }
    /* in the console's pixels: anything within 6 joins; in the same row (vertical overlap, pieces of
     * a similar height) within 32, so a menu line's labels and values move or stay together (Rush's
     * "PLAYERS 1 2 3 4"), but a tall HUD piece beside a centred line of text does not pull that line
     * aside (Re-Volt's position arrow next to "PRESS START BUTTON TO CONTINUE") */
    const float gap = 6.f, row_gap = 32.f;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++) {
            float hi = b[i].y1 - b[i].y0, hj = b[j].y1 - b[j].y0;
            int same_row = b[i].y0 < b[j].y1 && b[j].y0 < b[i].y1 && hi < hj * 2.5f && hj < hi * 2.5f;
            float gx = same_row ? row_gap : gap;
            if (b[i].x0 > b[j].x1 + gx || b[j].x0 > b[i].x1 + gx) continue;
            if (b[i].y0 > b[j].y1 + gap || b[j].y0 > b[i].y1 + gap) continue;
            int a = hud_find(b, i), c = hud_find(b, j);
            if (a != c) b[c].parent = a;
        }
    /* group extents */
    float *gx0 = malloc(sizeof(float) * (size_t)(n ? n : 1) * 2), *gx1 = gx0 + (n ? n : 1);
    for (int i = 0; i < n; i++) { gx0[i] = 1e30f; gx1[i] = -1e30f; }
    for (int i = 0; i < n; i++) { int r = hud_find(b, i); if (b[i].x0 < gx0[r]) gx0[r] = b[i].x0; if (b[i].x1 > gx1[r]) gx1[r] = b[i].x1; }
    /* dc_pvr_gl_hud_split_wide: in a group that spans most of the screen, pieces that touch directly
     * (within a pixel: a bar and its end caps, a portrait and its frame) form the units that move */
    int *sub = NULL; float *sx0 = NULL, *sx1 = NULL;
    if (dc_pvr_gl_hud_split_wide && n) {
        sub = malloc(sizeof(int) * (size_t)n); sx0 = malloc(sizeof(float) * (size_t)n * 2); sx1 = sx0 + n;
        for (int i = 0; i < n; i++) sub[i] = i;
        for (int i = 0; i < n; i++) {
            int r = hud_find(b, i);
            if (gx1[r] - gx0[r] <= RW * 0.6f) continue;
            for (int j = i + 1; j < n; j++) {
                if (hud_find(b, j) != r) continue;
                if (b[i].x0 > b[j].x1 + 1.f || b[j].x0 > b[i].x1 + 1.f || b[i].y0 > b[j].y1 + 1.f || b[j].y0 > b[i].y1 + 1.f) continue;
                int a = i, c = j;
                while (sub[a] != a) a = sub[a];
                while (sub[c] != c) c = sub[c];
                if (a != c) sub[c] = a;
            }
        }
        for (int i = 0; i < n; i++) { sx0[i] = 1e30f; sx1[i] = -1e30f; }
        for (int i = 0; i < n; i++) {
            int a = i; while (sub[a] != a) a = sub[a];
            sub[i] = a;
            if (b[i].x0 < sx0[a]) sx0[a] = b[i].x0; if (b[i].x1 > sx1[a]) sx1[a] = b[i].x1;
        }
    }
    for (int i = 0; i < n; i++) {
        int r = hud_find(b, i);
        float cx = (gx0[r] + gx1[r]) * 0.5f;
        if (gx1[r] - gx0[r] > RW * 0.6f) {                   /* spans most of the screen: centred layout */
            if (!sub) continue;
            cx = (sx0[sub[i]] + sx1[sub[i]]) * 0.5f;          /* or by the pieces it touches directly */
        }
        side[b[i].poly] = cx < RW / 3.f ? -1 : cx > RW * 2.f / 3.f ? 1 : 0;
    }
    free(sub); free(sx0);
    free(gx0);
}

/* widescreen: a cut-scene's letterbox bar (an untextured or black-textured, flat, opaque, near-black polygon across the
 * whole 4:3 width, along the top or bottom edge, up to a third of the height). A frame with one is
 * shown at 4:3 with black sides: the game frames such scenes (and culls them) for 4:3, and its bars
 * stop at the 4:3 edges (Looney Tunes Space Race's intro). */
/* could be a screen shade: untextured, or a texture modulated by black vertex colours (Re-Volt's pause
 * shade is a textured quad drawn black at half alpha) */
int dc_pvr_gl_shade_translucent;
static int shade_poly(const pvr_poly *p, const pvr_vtx *v) {
    if (!((p->isp >> 25) & 1)) return 1;
    if (dc_pvr_gl_shade_translucent && p->list == 2) {      /* any colour, see pvr.h */
        int t = 1; for (int k = 0; k < p->n; k++) if (v[k].c[3] > .95f) t = 0;
        if (t) return 1;
    }
    if (!(((p->tsp >> 6) & 3) & 1)) return 0;                /* modulate (or modulate alpha) */
    for (int k = 0; k < p->n; k++) if (v[k].c[0] > .05f || v[k].c[1] > .05f || v[k].c[2] > .05f) return 0;
    return 1;
}

/* a textured polygon that shows black: the texels its UVs cover are all near-black, its offset colour adds
 * nothing, and its shading keeps them black (Propeller Arena draws its cut-scene bars as sprites of a
 * black corner of a texture, white vertex colour) */
static int black_textured(const pvr_poly *p, const pvr_vtx *v) {
    if (p->tcw1 && p->twovol) return 0;
    for (int k = 0; k < p->n; k++)
        if (((p->isp >> 24) & 1) && (v[k].o[0] > 0.06f || v[k].o[1] > 0.06f || v[k].o[2] > 0.06f)) return 0;
    pvr_texinfo ti; tex_get(p->tcw, p->tsp, &ti);
    if (!ti.px || ti.w <= 0 || ti.h <= 0) return 0;
    float u0 = v[0].u, u1 = v[0].u, w0 = v[0].v, w1 = v[0].v;
    for (int k = 1; k < p->n; k++) {
        if (v[k].u < u0) u0 = v[k].u; if (v[k].u > u1) u1 = v[k].u;
        if (v[k].v < w0) w0 = v[k].v; if (v[k].v > w1) w1 = v[k].v;
    }
    if (u0 < 0.f || w0 < 0.f || u1 > 1.f || w1 > 1.f) return 0;    /* wrapping: not worth following */
    int tx0 = (int)(u0 * ti.w), tx1 = (int)(u1 * ti.w), ty0 = (int)(w0 * ti.h), ty1 = (int)(w1 * ti.h);
    if (tx1 >= ti.w) tx1 = ti.w - 1; if (ty1 >= ti.h) ty1 = ti.h - 1;
    if (tx0 > tx1) tx0 = tx1; if (ty0 > ty1) ty0 = ty1;
    for (int y = ty0; y <= ty1; y++)
        for (int x = tx0; x <= tx1; x++) {
            uint32_t c = ti.px[y * ti.w + x];
            if (((c >> 16) & 255) > 15 || ((c >> 8) & 255) > 15 || (c & 255) > 15) return 0;
        }
    return 1;
}

static int letterbox_bar(const pvr_poly *p, const pvr_vtx *v, int RW, int H) {
    if (p->n < 3 || p->list != 0) return 0;
    int textured = (p->isp >> 25) & 1;
    float x0 = v[0].x, x1 = v[0].x, y0 = v[0].y, y1 = v[0].y;
    for (int k = 0; k < p->n; k++) {
        /* corners at infinity (a piece of scenery on the camera plane: Soulcalibur's win poses) look
         * flat and wider than the screen, and switched single frames to 4:3 with black sides */
        if (!isfinite(v[k].x) || !isfinite(v[k].y) || !isfinite(v[k].z)) return 0;
        if (v[k].z != v[0].z) return 0;
        if (!textured && (v[k].c[0] > 0.06f || v[k].c[1] > 0.06f || v[k].c[2] > 0.06f)) return 0;
        if (v[k].x < x0) x0 = v[k].x; if (v[k].x > x1) x1 = v[k].x;
        if (v[k].y < y0) y0 = v[k].y; if (v[k].y > y1) y1 = v[k].y;
    }
    if (x0 > 1.f || x1 < RW - 1.f || y1 - y0 < 4.f || y1 - y0 > H / 3.f) return 0;
    if (!(y0 <= 1.f || y1 >= H - 1.f)) return 0;
    return !textured || black_textured(p, v);
}

/* widescreen, bars stretched (dc_pvr_gl_letterbox_4_3 off): also a translucent dark band along the top or
 * bottom edge across the 4:3 width, which a game fades in and out with its bars (Propeller Arena's attract
 * intro) */
static int letterbox_band(const pvr_poly *p, const pvr_vtx *v, int RW, int H) {
    if (letterbox_bar(p, v, RW, H)) return 1;
    if (p->n < 3 || p->list != 2) return 0;
    int textured = (p->isp >> 25) & 1;
    float x0 = v[0].x, x1 = v[0].x, y0 = v[0].y, y1 = v[0].y;
    for (int k = 0; k < p->n; k++) {
        if (!isfinite(v[k].x) || !isfinite(v[k].y) || !isfinite(v[k].z)) return 0;
        if (v[k].z != v[0].z) return 0;
        if (!textured && (v[k].c[0] > 0.06f || v[k].c[1] > 0.06f || v[k].c[2] > 0.06f)) return 0;
        if (v[k].x < x0) x0 = v[k].x; if (v[k].x > x1) x1 = v[k].x;
        if (v[k].y < y0) y0 = v[k].y; if (v[k].y > y1) y1 = v[k].y;
    }
    if (x0 > 1.f || x1 < RW - 1.f || y1 - y0 < 4.f || y1 - y0 > H / 3.f) return 0;
    if (!(y0 <= 1.f || y1 >= H - 1.f)) return 0;
    return !textured || black_textured(p, v);
}

/* a full-screen card: an opaque textured quad, nearly flat, that covers the whole 4:3 picture with
 * a 3D scene behind it (20+ polygons wholly farther away). In 4:3 the scene is hidden; in widescreen
 * it would show past the card's edges (Looney Tunes Space Race's title over its studio set), so
 * such a frame counts as 2D for dc_pvr_gl_clip_2d. */
static int backdrop_card(const pvr_frame *f, int RW) {
    for (int i = 0; i < f->npolys; i++) {
        const pvr_poly *p = &f->polys[i]; const pvr_vtx *v = f->verts + p->first;
        if (p->n < 3 || p->n > 4 || (p->list != 0 && p->list != 4) || !((p->isp >> 25) & 1)) continue;
        float x0 = v[0].x, x1 = v[0].x, y0 = v[0].y, y1 = v[0].y, z0 = v[0].z, z1 = v[0].z;
        for (int k = 1; k < p->n; k++) {
            if (v[k].x < x0) x0 = v[k].x; if (v[k].x > x1) x1 = v[k].x; if (v[k].y < y0) y0 = v[k].y; if (v[k].y > y1) y1 = v[k].y;
            if (v[k].z < z0) z0 = v[k].z; if (v[k].z > z1) z1 = v[k].z;
        }
        if (x0 > 1.f || x1 < RW - 1.f || y0 > 1.f || y1 < f->height - 1.f || z1 - z0 > 0.1f * z1) continue;
        /* a card is about the picture's size; a 3D polygon right in front of the camera that happens to cover
         * it reaches far past (Propeller Arena's cockpit close-ups: the fuselage, flat at that distance) */
        if (x0 < -0.1f * RW || x1 > 1.1f * RW || y0 < -0.1f * f->height || y1 > 1.1f * f->height) continue;
        int behind = 0;
        for (int j = 0; j < f->npolys && behind < 20; j++) {
            const pvr_poly *q = &f->polys[j]; const pvr_vtx *w = f->verts + q->first;
            if (q->n < 3) continue;
            float zm = w[0].z; for (int k = 1; k < q->n; k++) if (w[k].z > zm) zm = w[k].z;
            if (zm < z0) behind++;
        }
        if (behind >= 20) return 1;
    }
    return 0;
}

/* on the game's thread: take what the next dc_pvr_gl_render reads of the console (see snap_on), so it
 * can run on another thread while the game goes on. 0: not possible this frame (too many textures to
 * keep decoded at once); render the usual way then. */
int dc_pvr_gl_prepare(void) {
    snap_on = tmap_on = 0;
    const uint32_t *tex; int ntex;
    pvr_snap_take(&snap_f, &tex, &ntex);                   /* the lists are parsed where they are drawn */
    for (uint32_t i = 0; i < 0x2000 / 4; i++) snap_reg[i] = (dc_pvr_reg)(i * 4);
    tmap_stamp++; tmap_n = 0; tmap_miss = 0;
    if (!tmap_stamp) { memset(tmap, 0, sizeof tmap); tmap_stamp = 1; }
    for (int i = 0; i < ntex; i++)                         /* the decoded textures stay in pvr_sw's cache (512) */
        if (tmap_add(tex[i * 2], tex[i * 2 + 1]) > 384) return 0;
    snap_on = tmap_on = 1;
    return 1;
}
static void render_frame(int scale);
void dc_pvr_gl_render(int scale) {
    render_frame(scale);
    snap_on = tmap_on = 0;
}
/* depth from the vertices or by the logarithm (zv_on), as dc_pvr_gl_fast_depth asks and the driver can:
 * the depth buffers are made again in the other format, and the variants of that kind are used */
/* Tried once before it is used: four quads over each other in a small float-depth target, at 1/w as a
 * scene and a HUD have them, the second of each pair behind the first. A driver that maps depth to
 * 0..1 its own way after all, or keeps fewer bits than a float, lets the ones behind through. */
static int zv_selftest(void) {
    GLuint tf = 0, tc = 0, td = 0; int ok = 0;
    p_glGenTextures(1, &tc); p_glBindTexture(GL_TEXTURE_2D, tc);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    p_glGenRenderbuffers(1, &td); p_glBindRenderbuffer(GL_RENDERBUFFER, td);
    p_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH32F_STENCIL8, 8, 8);
    p_glGenFramebuffers(1, &tf); p_glBindFramebuffer(GL_FRAMEBUFFER, tf);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tc, 0);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, td);
    if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        static const float zs[4] = { 2e-3f, 1.5e-3f, 1e6f, 5e5f };
        static const float cols[4][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { 1, 1, 1 } };
        static const float X[6] = { 0, 8, 0, 8, 8, 0 }, Y[6] = { 0, 0, 8, 0, 8, 8 };
        gv q[24]; memset(q, 0, sizeof q);
        for (int k = 0; k < 24; k++) {
            q[k].x = X[k % 6]; q[k].y = Y[k % 6]; q[k].z = zs[k / 6];
            q[k].c[0] = cols[k / 6][0]; q[k].c[1] = cols[k / 6][1]; q[k].c[2] = cols[k / 6][2]; q[k].c[3] = 1;
        }
        p_glBindVertexArray(vao); p_glBindBuffer(GL_ARRAY_BUFFER, vbo);
        p_glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STREAM_DRAW);
        p_glViewport(0, 0, 8, 8);
        p_glDisable(GL_SCISSOR_TEST); p_glDisable(GL_STENCIL_TEST); p_glDisable(GL_BLEND); p_glDisable(GL_CULL_FACE);
        p_glEnable(GL_DEPTH_TEST); p_glDepthFunc(GL_GEQUAL); p_glDepthMask(1); p_glColorMask(1, 1, 1, 1);
        FU.sx = 2.f / 8.f; FU.sy = 2.f / 8.f; FU.offx = 0; sv_stamp++; sv_cur = NULL;
        sv_use(SV_USE_ALPHA | SV_FOG(2));
        p_glClearColor(0, 0, 0, 1); p_glClearDepth(depth_clear(1e-3f)); p_glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        uint8_t px[2][4] = { { 0 }, { 0 } };
        p_glPixelStorei(GL_PACK_ALIGNMENT, 1);
        p_glDrawArrays(GL_TRIANGLES, 0, 12); p_glReadPixels(4, 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px[0]);
        p_glDrawArrays(GL_TRIANGLES, 12, 12); p_glReadPixels(4, 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px[1]);
        ok = px[0][0] > 200 && px[0][1] < 50 && px[0][2] < 50 && px[1][2] > 200 && px[1][0] < 50 && px[1][1] < 50;
        if (!ok) fprintf(stderr, "pvr_gl: depth test from the vertices: got %d %d %d, %d %d %d\n", px[0][0], px[0][1], px[0][2], px[1][0], px[1][1], px[1][2]);
    }
    /* and with depth alone, as games without modifier volumes get it (see zv_stencil) */
    while (p_glGetError()) {}
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
    p_glBindRenderbuffer(GL_RENDERBUFFER, td); p_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT32F, 8, 8);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, td);
    zv_plain = !p_glGetError() && p_glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    while (p_glGetError()) {}
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    p_glDeleteFramebuffers(1, &tf); p_glDeleteTextures(1, &tc); p_glDeleteRenderbuffers(1, &td);
    sv_cur = NULL;
    return ok;
}
int dc_pvr_gl_fast_depth_ok(void) { return zv_ok; }
static void zv_apply(void) {
    int want = dc_pvr_gl_fast_depth && zv_ok;
    static int listed, tested;
    if (want && !tested) {
        tested = 1;
        zv_on = 1; p_glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
        int ok = zv_selftest();
        zv_on = 0; p_glClipControl(GL_LOWER_LEFT, GL_NEGATIVE_ONE_TO_ONE);
        if (!ok) { zv_ok = 0; want = 0; fprintf(stderr, "pvr_gl: this driver's depth from the vertices fails its test: depth by logarithm per pixel\n"); }
    }
    if (want != zv_on) {
        zv_on = want;
        p_glClipControl(GL_LOWER_LEFT, zv_on ? GL_ZERO_TO_ONE : GL_NEGATIVE_ONE_TO_ONE);
        sv_cur = NULL;                           /* the buffers follow: ensure_fbo and ensure_ms look at their depth's format */
        fprintf(stderr, "pvr_gl: depth %s\n", zv_on ? "from the vertices (float buffer, tested before shading)" : "by logarithm per pixel");
        listed = 0;
    }
    if (!listed) { listed = 1; sv_load_list(); }
}
static void render_frame(int scale) {
    if (!ready) return;
    if (scale < 1) scale = 1;
    zv_apply();
    /* from here on in halves (SC): 1.5 times is 960x720, the Switch's own screen */
    scale = scale * 2 - (dc_pvr_gl_half_step && scale > 1 && !capturing);
    #define SC(x) ((x) * scale / 2)
    frame_no++;
    pvr_frame f;
    if (snap_on) { f = snap_f; pvr_snap_lists(&f); } else pvr_get_frame(&f);
    tp_frame = &f;
    /* Several renders into the same framebuffer, each clipped to its own band of lines (FB_Y_CLIP), make
     * up one picture (Looney Tunes Space Race's split screen: top view, bottom view, top strip): a
     * render whose band does not overlap the previous one's, same target, same displayed buffer, keeps
     * what is already drawn outside its band, and the picture's size (the display size can come out a
     * line or two different from one band to the next). */
    int band, restore = -1;
    uint32_t cur_tgt = dc_pvr_reg(0x060) & 0x7FFFFFu;
    {
        static uint32_t prev_tgt = 0xFFFFFFFFu, prev_disp; static int prev_x0, prev_x1, prev_y0, prev_y1, prev_w, prev_h;
        uint32_t tgt = dc_pvr_reg(0x060) & 0x7FFFFFu, disp = dc_pvr_reg(0x050) & 0x7FFFFFu;
        /* bands of lines, or quarters (Re-Volt's 4-player race: four 320x240 renders): rectangles apart */
        band = !capturing && tgt == prev_tgt && disp == prev_disp && (f.clip_y0 > prev_y1 || f.clip_y1 < prev_y0 || f.clip_x0 > prev_x1 || f.clip_x1 < prev_x0);
        if (band) { f.width = prev_w; f.height = prev_h; }
        /* a partial render (under 3/4 of the lines) into a buffer drawn whole before: that picture stays
         * outside its clip */
        if (!capturing && !band && dc_pvr_gl_keep_outside && f.clip_y1 >= f.clip_y0 && (f.clip_y1 - f.clip_y0 + 1) * 4 < f.height * 3)
            for (int k = 0; k < NKEEP; k++)
                if (keep[k].tex && keep[k].tgt == tgt) { restore = k; band = 1; f.width = keep[k].w; f.height = keep[k].h; break; }
        if (!capturing) { prev_tgt = tgt; prev_disp = disp; prev_x0 = f.clip_x0; prev_x1 = f.clip_x1; prev_y0 = f.clip_y0; prev_y1 = f.clip_y1; prev_w = f.width; prev_h = f.height; }
    }
    if (capturing) {                    /* a texture target: no wider than its lines (a 1024 clip into 640) */
        uint32_t stride = (dc_pvr_reg(0x04C) & 0x1FF) * 8, mode = dc_pvr_reg(0x048) & 7;
        int bpp = mode <= 3 ? 2 : mode == 4 ? 3 : 4;
        if (stride && f.width * bpp > (int)stride) f.width = (int)stride / bpp;
        int rw, rh; pvr_region_extent(&rw, &rh);                 /* only the tiles the region array lists */
        if (rw > 0 && rw < f.width) f.width = rw;
        if (rh > 0 && rh < f.height) f.height = rh;
        if (f.clip_x1 >= f.width) f.clip_x1 = f.width - 1;
        if (f.clip_y1 >= f.height) f.clip_y1 = f.height - 1;
    }
    int RW = f.width * (f.hscale ? 2 : 1), RH = f.height;
    last_rw = (float)RW; last_rh = (float)RH;
    /* Corners at infinity. A game's projection can send corners that land on the camera plane as
     * x, y = +-inf with 1/w = FLT_MAX (Soulcalibur's opening: the spinning sunset sky quad and the
     * rings round the camera, on single frames). Their triangles fell out in clipping and the picture
     * flashed to the plain background for that frame. Such a polygon is drawn as it was in the last
     * frame when that frame had the same piece (texture, mode, corner count) at the same place in its
     * list; the camera moves little in one frame. */
    if (!capturing && dc_pvr_gl_camera_plane) {
        #define KEEP_POLYS 64
        #define KEEP_VERTS 128
        static struct { uint32_t tcw, isp; int n; pvr_vtx v[KEEP_VERTS]; } *keep_p;
        static int nkeep;
        if (!keep_p) keep_p = calloc(KEEP_POLYS, sizeof *keep_p);
        int np = f.npolys < KEEP_POLYS ? f.npolys : KEEP_POLYS;
        for (int i = 0; keep_p && i < np; i++) {
            const pvr_poly *p = &f.polys[i]; pvr_vtx *v = (pvr_vtx *)(f.verts + p->first);
            /* only small pieces (a quad, a ring segment): a long strip (the sand floor of the opening's
             * sunset flight, 80 corners running from the horizon to under the camera) moves too much
             * between frames for its last shape to stand in; its triangles with a corner at infinity
             * just drop out */
            if (p->n > 8) continue;
            int bad = 0;
            for (int k = 0; k < p->n && !bad; k++)
                if (!isfinite(v[k].x) || !isfinite(v[k].y) || !isfinite(v[k].z) || v[k].z >= 1e30f) bad = 1;
            if (bad) {
                if (i < nkeep && keep_p[i].tcw == p->tcw && keep_p[i].isp == p->isp && keep_p[i].n == p->n)
                    memcpy(v, keep_p[i].v, (size_t)p->n * sizeof *v);
                continue;                                         /* the kept copy stays for the next frame */
            }
            keep_p[i].tcw = p->tcw; keep_p[i].isp = p->isp; keep_p[i].n = p->n;
            memcpy(keep_p[i].v, v, (size_t)p->n * sizeof *v);
        }
        nkeep = np;
    }
    /* a polygon with no finite corner at all is never drawn; it is moved to one point far off screen
     * so the frame's tests (letterbox bars, shades, backdrops) don't take it for something across the
     * whole screen */
    for (int i = 0; i < f.npolys; i++) {
        const pvr_poly *p = &f.polys[i]; pvr_vtx *v = (pvr_vtx *)(f.verts + p->first);
        int fin = 0;
        for (int k = 0; k < p->n && !fin; k++) fin = isfinite(v[k].x) && isfinite(v[k].y) && isfinite(v[k].z) && v[k].z < 1e30f;
        if (fin) continue;
        for (int k = 0; k < p->n; k++) { v[k].x = -1e4f; v[k].y = -1e4f; v[k].z = 1e-6f; }
    }
    {   static int cap_hud;
        if (f.npolys > cap_hud) { cap_hud = f.npolys + 1024; poly_hud = realloc(poly_hud, (size_t)cap_hud); poly_uvbox = realloc(poly_uvbox, (size_t)cap_hud * sizeof *poly_uvbox); poly_cap = realloc(poly_cap, (size_t)cap_hud); poly_edge = realloc(poly_edge, (size_t)cap_hud); }
        poly_base = f.polys;
        for (int i = 0; i < f.npolys; i++) {
            const pvr_poly *p = &f.polys[i]; const pvr_vtx *v = f.verts + p->first;
            int flat = dc_pvr_gl_hud_z > 0 && p->n >= 3 && ((p->isp >> 25) & 1);
            for (int k = 0; k < p->n && flat; k++) if (v[k].z != v[0].z || v[k].z < dc_pvr_gl_hud_z) flat = 0;
            /* a 2D overlay piece that maps part of one copy of its texture (UVs within 0..1) samples
             * clamped to its own UV range: at 1:1 on the console every pixel lands on a texel centre,
             * but scaled up, bilinear filtering at the piece's edge also reads the texel next to it.
             * Art cut into pieces (Bomberman Online's PAUSE logo: two halves stacked in one texture)
             * otherwise gets a dark seam where the pieces meet */
            /* the same for a big screen-parallel piece at any depth: 2D backgrounds cut into tiles
             * (Looney Tunes Space Race's menus) otherwise show thin lines where the tiles meet */
            int tile = 0;
            if (!flat && dc_pvr_gl_tile_clamp && p->n >= 3 && ((p->isp >> 25) & 1) && ((p->tsp >> 13) & 3)) {
                float x0 = v[0].x, x1 = v[0].x, y0 = v[0].y, y1 = v[0].y; tile = 1;
                for (int k = 1; k < p->n; k++) {
                    if (v[k].z != v[0].z) { tile = 0; break; }
                    if (v[k].x < x0) x0 = v[k].x; if (v[k].x > x1) x1 = v[k].x; if (v[k].y < y0) y0 = v[k].y; if (v[k].y > y1) y1 = v[k].y;
                }
                if (tile && (x1 - x0 < 64.f || y1 - y0 < 64.f)) tile = 0;
            }
            int one = flat || tile;
            float *ub = poly_uvbox[i]; ub[0] = ub[1] = 1e30f; ub[2] = ub[3] = -1e30f;
            for (int k = 0; k < p->n && one; k++) {
                if (v[k].u < 0.f || v[k].u > 1.f || v[k].v < 0.f || v[k].v > 1.f) one = 0;
                if (v[k].u < ub[0]) ub[0] = v[k].u; if (v[k].v < ub[1]) ub[1] = v[k].v;
                if (v[k].u > ub[2]) ub[2] = v[k].u; if (v[k].v > ub[3]) ub[3] = v[k].v;
            }
            poly_hud[i] = (unsigned char)((flat && dc_pvr_gl_hud_sharp ? 1 : 0) | (one ? 2 : 0));
            int e0 = dc_pvr_gl_tile_clamp && p->n >= 3 && ((p->isp >> 25) & 1), e1 = e0 && p->twovol;
            for (int k = 0; k < p->n && (e0 || e1); k++) {
                if (v[k].u < -0.001f || v[k].u > 1.001f || v[k].v < -0.001f || v[k].v > 1.001f) e0 = 0;
                if (v[k].u1 < -0.001f || v[k].u1 > 1.001f || v[k].v1 < -0.001f || v[k].v1 > 1.001f) e1 = 0;
            }
            poly_edge[i] = (unsigned char)(e0 | (e1 << 1));
        }
    }
    /* widescreen: a target as wide as the window (16:9 until the frontend reports its shape) with the
     * console's 4:3 picture in the middle; the sides show what the game draws beyond the screen edges.
     * WW is the target width in console x units: the shown lines (only the drawn ones with
     * dc_pvr_gl_fill_lines) fill the window height at the console's 4:3 pixel shape. */
    /* full-screen quads showing a texture captured at full width (dc_pvr_gl_capture) */
    for (int i = 0; i < f.npolys; i++) {
        const pvr_poly *p = &f.polys[i]; const pvr_vtx *v = f.verts + p->first;
        poly_cap[i] = 0;
        if (!dc_pvr_gl_widescreen || !((p->isp >> 25) & 1) || p->n < 3) continue;
        uint32_t addr = (p->tcw & 0x1FFFFF) << 3; int c = 0;
        while (c < NCAP && !(cap[c].tex && cap[c].addr == addr)) c++;
            if (c == NCAP) continue;
        if (capturing && c == cap_cur) { if (!cap[NCAP].tex) continue; c = NCAP; }   /* feedback: the last frame (replay trails) */
        float x0 = v[0].x, x1 = v[0].x, y0 = v[0].y, y1 = v[0].y; int flat = 1;
        for (int k = 1; k < p->n; k++) {
            if (v[k].x < x0) x0 = v[k].x; if (v[k].x > x1) x1 = v[k].x;
            if (v[k].y < y0) y0 = v[k].y; if (v[k].y > y1) y1 = v[k].y;
            if (fabsf(v[k].z - v[0].z) > fabsf(v[0].z) * 1e-4f) flat = 0;   /* flat, to rounding (a quad placed in 3D) */
        }
        /* the full picture: of a render to texture, its clipped area (Dead or Alive 2 draws its trails into the
         * top 512 x 256 of a 512 x 478 pass, the last capture fed back over the new one) */
        float fx1 = (float)RW, fy1 = (float)RH;
        if (capturing && f.clip_x1 > f.clip_x0 && f.clip_x1 + 1 < f.width) fx1 = (float)(f.clip_x1 + 1) * (float)RW / (float)f.width;
        if (capturing && f.clip_y1 > f.clip_y0 && f.clip_y1 + 1 < (int)RH) fy1 = (float)(f.clip_y1 + 1);
        if (flat && x0 <= 1.f && x1 >= fx1 - 1.f && y0 <= 1.f && y1 >= fy1 - 1.f && x1 > x0 && y1 > y0 &&
            cap[c].rw >= RW * 0.5f && cap[c].rh >= RH * 0.5f)
            { poly_cap[i] = (unsigned char)(c + 1); poly_hud[i] = 0; }   /* plain bilinear over the whole capture */
    }
    float aspect = 4.f / 3.f, WW = (float)RW;
    if (dc_pvr_gl_widescreen) {
        aspect = dc_pvr_gl_aspect > 0 ? dc_pvr_gl_aspect : 16.f / 9.f;
        if (aspect < 4.f / 3.f) aspect = 4.f / 3.f;
        if (aspect > 4.f) aspect = 4.f;
        int lines = f.height;
        /* at least 3/4 of the picture: a render of one band (a split-screen view, half the height) is
         * part of a larger picture, not the picture with unused lines (Re-Volt's 2-player race) */
        if (dc_pvr_gl_fill_lines && f.clip_y1 > f.clip_y0 && f.clip_y1 < f.height && (f.clip_y1 - f.clip_y0 + 1) * 4 >= f.height * 3)
            lines = f.clip_y1 - f.clip_y0 + 1;
        WW = aspect * (float)lines * (float)RW / (4.f / 3.f * (float)f.height);
        if (WW < (float)RW) WW = (float)RW;
    }
    /* the 4:3 picture starts on a whole pixel: margins of half a pixel put the console's pixel edges
     * (where 2D background tiles meet) through pixel centres, where both tiles may draw the pixel: a
     * thin line of doubled translucency (Sega Rally 2's title at 1707 x 960) */
    int pic_w = SC(f.width), fb_w = pic_w + 2 * (int)((WW / (float)RW - 1.f) * (float)pic_w * 0.5f + 0.5f);
    if (WW > (float)RW) WW = (float)RW * (float)fb_w / (float)pic_w;
    if (!zv_stencil) for (int i = 0; i < f.nmods; i++) if (f.mods[i].list == 1) { zv_stencil = 1; break; }
    ensure_fbo(fb_w, SC(f.height));
    fb_scale = scale;
    gl_selftest();
    disp_aspect = aspect;
    last_ww = WW;

    /* geometry: every polygon's triangles, then volume triangles, then a full-screen quad */
    nverts = 0;
    tdepth_reset();
    if (f.npolys + 1 > cap_spans) { cap_spans = f.npolys + 1024; spans = realloc(spans, cap_spans * sizeof *spans); }
    float cull_small = 0; { uint32_t cv = dc_pvr_reg(0x078); memcpy(&cull_small, &cv, 4); }
    /* widescreen HUD: 2D overlay polygons (every vertex at the same 1/w, >= dc_pvr_gl_hud_z) in the left or
     * right third of the 4:3 picture move out to the edges of the 16:9 one; only in frames that
     * also draw a 3D scene, so full-screen 2D menus keep their layout */
    float hud_dx = 0;
    if (dc_pvr_gl_widescreen && dc_pvr_gl_hud_z > 0 && dc_pvr_gl_hud_move) {
        /* a race-like frame: a 3D scene (50+ polygons with varying depth) that reaches past the
         * 4:3 picture into the sides; menus with 3D objects (track previews, emblems) keep their
         * 2D layout */
        int n3d = 0, wide3d = 0;
        for (int i = 0; i < f.npolys && !(n3d >= 50 && wide3d); i++) {
            const pvr_vtx *v = f.verts + f.polys[i].first;
            if (f.polys[i].n >= 3 && v[0].z < dc_pvr_gl_hud_z && (v[0].z != v[1].z || v[0].z != v[2].z)) {
                n3d++;
                for (int k = 0; k < f.polys[i].n && !wide3d; k++) if (v[k].x < -2.f || v[k].x > RW + 2.f) wide3d = 1;
            }
        }
        /* split screen (user tile clip on part of the picture): the HUD belongs to its view, it stays. A
         * view rendered as its own band of lines (see band above) is the whole picture of that render:
         * its HUD moves out like a single player's */
        int split = 0;
        for (int i = 0; i < f.npolys && !split; i++)
            if (f.polys[i].uclip == 2) {
                /* the rectangle's part inside this render's lines (one that misses them draws nothing) */
                int y0 = f.polys[i].uy0 > f.clip_y0 ? f.polys[i].uy0 : f.clip_y0, y1 = f.polys[i].uy1 < f.clip_y1 + 1 ? f.polys[i].uy1 : f.clip_y1 + 1;
                if (y1 > y0 && (y1 - y0 < (f.clip_y1 - f.clip_y0 + 1) * 3 / 4 || f.polys[i].ux1 - f.polys[i].ux0 < f.width * 3 / 4)) split = 1;
            }
        /* a letterboxed scene (a cut-scene shown wide): its text is subtitles, laid out for the picture,
         * not HUD. Jet Set Radio types its lines out: a short line sat in the left third and was pushed to
         * the edge, then jumped back once it grew past the middle */
        int bars = 0;
        for (int i = 0; i < f.npolys && !bars; i++) bars = letterbox_bar(&f.polys[i], f.verts + f.polys[i].first, RW, f.height);
        if (n3d >= 50 && wide3d && !split && !bars) hud_dx = (WW - (float)RW) * 0.5f;
    }
    /* HUD pieces are grouped first (pieces that touch or nearly touch: the letters of a line, the
     * parts of a panel), then each group moves as a whole by where its centre is, so centred text
     * stays together */
    /* a full-screen shade over the HUD (Rush's pause) and what is drawn above it (the menu) belong to
     * the whole screen: they stay centred, the shade covers the full width, and the HUD below keeps
     * its widescreen place */
    float ovl_z = 1e30f;
    if (dc_pvr_gl_widescreen && dc_pvr_gl_hud_z > 0)
        for (int i = 0; i < f.npolys; i++) {
            const pvr_poly *p = &f.polys[i]; const pvr_vtx *v = f.verts + p->first;
            if (p->n < 3 || !shade_poly(p, v) || v[0].z < dc_pvr_gl_hud_z) continue;
            float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f; int flat = 1;
            for (int k = 0; k < p->n; k++) {
                if (v[k].x < x0) x0 = v[k].x; if (v[k].x > x1) x1 = v[k].x;
                if (v[k].y < y0) y0 = v[k].y; if (v[k].y > y1) y1 = v[k].y;
                if (v[k].z != v[0].z) flat = 0;
            }
            if (flat && x0 <= 1.f && x1 >= RW - 1.f && y1 - y0 > f.height * 0.8f && v[0].z < ovl_z) ovl_z = v[0].z;
        }
    /* the same shade drawn as tiles (Tennis 2K2's pause: four quarter-screen quads): large flat
     * rectangles at one depth that together cover the whole picture once */
    if (dc_pvr_gl_widescreen && dc_pvr_gl_hud_z > 0) {
        enum { NT = 16 };
        struct { float z, x0, x1, y0, y1, area; int n; } t[NT]; int nt = 0;
        const float big = (float)RW * (float)f.height / 20.f;
        for (int i = 0; i < f.npolys; i++) {
            const pvr_poly *p = &f.polys[i]; const pvr_vtx *v = f.verts + p->first;
            if (p->n != 4 || !shade_poly(p, v) || v[0].z >= ovl_z) continue;
            if (v[0].z < dc_pvr_gl_hud_z) {         /* deeper: only a dark shade, and only when the game asks */
                if (!dc_pvr_gl_shade_any_z) continue;
                int dark = 1; for (int k = 0; k < 4; k++) if (v[k].c[0] > .3f || v[k].c[1] > .3f || v[k].c[2] > .3f) dark = 0;
                if (!dark) continue;
            }
            float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f; int rect = 1;
            for (int k = 0; k < 4; k++) {
                if (v[k].x < x0) x0 = v[k].x; if (v[k].x > x1) x1 = v[k].x;
                if (v[k].y < y0) y0 = v[k].y; if (v[k].y > y1) y1 = v[k].y;
                if (v[k].z != v[0].z) rect = 0;
            }
            for (int k = 0; k < 4 && rect; k++)            /* axis-aligned: every corner on the box */
                if ((fabsf(v[k].x - x0) > .5f && fabsf(v[k].x - x1) > .5f) || (fabsf(v[k].y - y0) > .5f && fabsf(v[k].y - y1) > .5f)) rect = 0;
            float a = (x1 - x0) * (y1 - y0);
            if (!rect || a < big || x0 < -1.f || x1 > RW + 1.f) continue;
            int j = 0; while (j < nt && t[j].z != v[0].z) j++;
            if (j == nt) { if (nt == NT) continue; nt++; t[j].z = v[0].z; t[j].x0 = x0; t[j].x1 = x1; t[j].y0 = y0; t[j].y1 = y1; t[j].area = 0; t[j].n = 0; }
            if (x0 < t[j].x0) t[j].x0 = x0; if (x1 > t[j].x1) t[j].x1 = x1;
            if (y0 < t[j].y0) t[j].y0 = y0; if (y1 > t[j].y1) t[j].y1 = y1;
            t[j].area += a; t[j].n++;
        }
        for (int j = 0; j < nt; j++) {
            float box = (t[j].x1 - t[j].x0) * (t[j].y1 - t[j].y0);
            if (t[j].n >= 2 && t[j].x0 <= 1.f && t[j].x1 >= RW - 1.f && t[j].y1 - t[j].y0 > f.height * 0.8f
                && t[j].area > box * 0.98f && t[j].area < box * 1.02f && t[j].z < ovl_z) ovl_z = t[j].z;
        }
    }
    /* dc_pvr_gl_hud_before_shade: what the game draws before the shade is HUD even above its depth
     * (Re-Volt draws its race HUD above the pause shade); only what comes after it is the menu */
    int ovl_i = 0;
    if (ovl_z < 1e29f && dc_pvr_gl_hud_before_shade) {
        ovl_i = f.npolys;
        for (int i = 0; i < f.npolys; i++) {
            const pvr_poly *p = &f.polys[i]; const pvr_vtx *v = f.verts + p->first;
            if (p->n >= 3 && v[0].z == ovl_z && shade_poly(p, v)) { ovl_i = i; break; }
        }
    }
    static signed char *hud_side; static int cap_side;
    if (hud_dx > 0) {
        if (f.npolys > cap_side) { cap_side = f.npolys + 1024; hud_side = realloc(hud_side, (size_t)cap_side); }
        memset(hud_side, 0, (size_t)f.npolys);
        hud_group_zmax = ovl_z; hud_group_imin = ovl_i;
        hud_group(&f, RW, hud_side);
        if (dc_pvr_gl_hud_panel_rows) {
            /* HUD beside a wide centred HUD panel, within its rows, belongs to it (Tennis 2K2's match
             * results: the names and 1P / COM left and right of the score box) */
            /* panels: centred HUD (side 0, flat, inside the 4:3 picture), pieces overlapping in rows
             * merged; one spanning 40%+ of the width marks its rows */
            enum { NPAN = 32 };
            float px0[NPAN], px1[NPAN], py0[NPAN], py1[NPAN]; int nq = 0;
            for (int i = 0; i < f.npolys; i++) {
                const pvr_poly *p = &f.polys[i]; const pvr_vtx *v = f.verts + p->first;
                if (p->n < 3 || hud_side[i]) continue;      /* any depth: the panel may lie below the HUD depth */
                float a0 = v[0].x, a1 = v[0].x, b0 = v[0].y, b1 = v[0].y; int flat = 1;
                for (int k = 1; k < p->n; k++) { if (v[k].x < a0) a0 = v[k].x; if (v[k].x > a1) a1 = v[k].x; if (v[k].y < b0) b0 = v[k].y; if (v[k].y > b1) b1 = v[k].y; if (v[k].z != v[0].z) flat = 0; }
                if (!flat || a1 - a0 > RW * 0.9f || a0 < -1.f || a1 > RW + 1.f || b1 - b0 < 4.f) continue;
                int j = 0;
                while (j < nq && !(b0 < py1[j] && py0[j] < b1 && a0 <= px1[j] + 8.f && px0[j] <= a1 + 8.f)) j++;
                if (j == nq) { if (nq == NPAN) continue; nq++; px0[j] = a0; px1[j] = a1; py0[j] = b0; py1[j] = b1; continue; }
                if (a0 < px0[j]) px0[j] = a0; if (a1 > px1[j]) px1[j] = a1; if (b0 < py0[j]) py0[j] = b0; if (b1 > py1[j]) py1[j] = b1;
            }
            float pr0[NPAN], pr1[NPAN]; int np = 0;
            for (int j = 0; j < nq; j++)
                if (px1[j] - px0[j] >= RW * 0.4f && px1[j] - px0[j] <= RW * 0.9f && fabsf((px0[j] + px1[j]) * 0.5f - RW * 0.5f) < RW * 0.1f && py1[j] - py0[j] >= 16.f)
                    { pr0[np] = py0[j]; pr1[np] = py1[j]; np++; }
            for (int i = 0; i < f.npolys && np; i++) {
                if (!hud_side[i]) continue;
                const pvr_vtx *v = f.verts + f.polys[i].first;
                float b0 = v[0].y, b1 = v[0].y;
                for (int k = 1; k < f.polys[i].n; k++) { if (v[k].y < b0) b0 = v[k].y; if (v[k].y > b1) b1 = v[k].y; }
                for (int j = 0; j < np; j++) if (b0 >= pr0[j] - 1.f && b1 <= pr1[j] + 1.f) { hud_side[i] = 0; break; }
            }
        }
        hud_group_zmax = 1e30f; hud_group_imin = 0;
    }
    int lbox = dc_pvr_gl_widescreen && dc_pvr_gl_show_4_3;
    if (dc_pvr_gl_widescreen && dc_pvr_gl_letterbox_4_3 && (f.clip_y1 - f.clip_y0 + 1) * 4 >= f.height * 3)   /* not in a band */
        for (int i = 0; i < f.npolys && !lbox; i++) lbox = letterbox_bar(&f.polys[i], f.verts + f.polys[i].first, RW, f.height);
    /* dc_pvr_gl_letterbox_zoom: the picture between the bars is cropped to the window's shape and fills it */
    float lb_top = 0.f, lb_bot = (float)f.height;
    if (lbox && !dc_pvr_gl_show_4_3 && dc_pvr_gl_letterbox_zoom)
        for (int i = 0; i < f.npolys; i++) {
            const pvr_vtx *v = f.verts + f.polys[i].first;
            if (!letterbox_bar(&f.polys[i], v, RW, f.height)) continue;
            float y0 = v[0].y, y1 = v[0].y;
            for (int k = 1; k < f.polys[i].n; k++) { if (v[k].y < y0) y0 = v[k].y; if (v[k].y > y1) y1 = v[k].y; }
            if (y0 <= 1.f && y1 > lb_top) lb_top = y1;
            if (y1 >= f.height - 1.f && y0 < lb_bot) lb_bot = y0;
        }
    if (lb_top > 0.f && lb_bot == (float)f.height) lb_bot = (float)f.height - lb_top;   /* one bar seen: the same below */
    if (lb_bot == (float)f.height && lb_top == 0.f) lb_bot = lb_top = -1.f;            /* no zoom */
    float cx0 = 1e30f, cx1 = -1e30f;                        /* x extent of the textured picture (side bars) */
    split_wide = 0;
    if (dc_pvr_gl_widescreen && dc_pvr_gl_split_panel_x > 0 && dc_pvr_gl_split_panel_x < f.width)
        for (int i = 0; i < f.npolys && !split_wide; i++) {
            const pvr_poly *p = &f.polys[i];
            if (p->uclip == 2 && p->ux0 <= 0 && p->ux1 == dc_pvr_gl_split_panel_x && p->uy1 - p->uy0 < f.height * 3 / 4) split_wide = 1;
        }
    float view_dx = (float)(RW - dc_pvr_gl_split_panel_x * RW / f.width) * 0.5f, panel_dx = (WW - (float)RW) * 0.5f;
    float panel_x = (float)(dc_pvr_gl_split_panel_x * RW / f.width);
    /* each view's HUD moves out like the single-player one: left third to the left edge, right third
     * to the right edge (with the small flat icons next to it, Rush's coins); the panel's map and
     * timer then sit just left of the right-hand HUD */
    float side_dx = view_dx + panel_dx;                      /* from the view's edge to the screen's */
    if (split_wide && dc_pvr_gl_hud_z > 0 && dc_pvr_gl_hud_move) {
        if (f.npolys > cap_side) { cap_side = f.npolys + 1024; hud_side = realloc(hud_side, (size_t)cap_side); }
        memset(hud_side, 0, (size_t)f.npolys);
        hud_group_zmax = ovl_z;
        hud_group(&f, (int)panel_x, hud_side);
        hud_group_zmax = 1e30f;
        /* boxes of the right-moving HUD pieces, and the small flat polygons next to them */
        float r0 = 1e30f, p1 = -1e30f;
        for (int pass = 0; pass < 2; pass++)
            for (int i = 0; i < f.npolys; i++) {
                const pvr_poly *p = &f.polys[i]; const pvr_vtx *v = f.verts + p->first;
                float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f, z0 = 1e30f; int flat = 1;
                for (int k = 0; k < p->n; k++) {
                    if (v[k].x < x0) x0 = v[k].x; if (v[k].x > x1) x1 = v[k].x;
                    if (v[k].y < y0) y0 = v[k].y; if (v[k].y > y1) y1 = v[k].y;
                    if (v[k].z != v[0].z) flat = 0;
                    if (v[k].z < z0) z0 = v[k].z;
                }
                /* small icons: flat, or small 3D models right at the camera (Rush's battle score
                 * dial around the number) */
                float icon = flat ? 40.f : z0 >= 0.5f ? 64.f : 0.f;
                if (x0 >= panel_x + 2.f) { if (pass && !split_view(p) && flat && x1 <= (float)RW + 2.f && x1 > p1) p1 = x1; continue; }   /* the panel's contents */
                if (pass == 0) { if (hud_side[i] > 0 && x0 < r0) r0 = x0; continue; }
                if (!hud_side[i] && x1 - x0 < icon && y1 - y0 < icon && x0 > panel_x * 0.75f) {
                    for (int j = 0; j < f.npolys; j++) {        /* within 24 px of a right-moving piece */
                        if (hud_side[j] != 1) continue;
                        const pvr_vtx *w = f.verts + f.polys[j].first;
                        float a0 = w[0].x, a1 = w[0].x, b0 = w[0].y, b1 = w[0].y;
                        for (int k = 1; k < f.polys[j].n; k++) { if (w[k].x < a0) a0 = w[k].x; if (w[k].x > a1) a1 = w[k].x; if (w[k].y < b0) b0 = w[k].y; if (w[k].y > b1) b1 = w[k].y; }
                        if (x0 < a1 + 24.f && a0 < x1 + 24.f && y0 < b1 + 4.f && b0 < y1 + 4.f) { hud_side[i] = 2; break; }   /* 2: icon, moves, not counted */
                    }
                }
            }
        if (r0 < 1e29f && p1 > -1e29f) panel_dx = r0 + view_dx + side_dx - 8.f - p1;   /* map right edge 8 px left of the HUD */
    }
    /* the render target in console units (see tri) */
    { float o = (WW - (float)RW) * 0.5f + 1.f; tgt_x0 = -o; tgt_x1 = (float)RW + o; tgt_y0 = -1.f; tgt_y1 = (float)RH + 1.f; }
    /* a fade, flash or shade for the whole screen: flat colour (or black-modulated) rectangles at one
     * depth that together cover the 4:3 picture once, one quad or tiles, at any depth (Tennis 2K2's
     * white flash behind the SPT logo, the quarter tiles shading the match results). In widescreen
     * their outer edges move out to the full width. */
    float scr_z[4]; int nscr = 0;
    if (dc_pvr_gl_widescreen && WW > (float)RW) {
        enum { NT = 16 };
        struct { float z, x0, x1, y0, y1, area; int n; } t[NT]; int nt = 0;
        for (int i = 0; i < f.npolys; i++) {
            const pvr_poly *p = &f.polys[i]; const pvr_vtx *v = f.verts + p->first;
            if (p->n != 4 || !shade_poly(p, v)) continue;
            float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f; int rect = 1;
            for (int k = 0; k < 4; k++) {
                if (v[k].x < x0) x0 = v[k].x; if (v[k].x > x1) x1 = v[k].x;
                if (v[k].y < y0) y0 = v[k].y; if (v[k].y > y1) y1 = v[k].y;
                if (v[k].z != v[0].z) rect = 0;
            }
            for (int k = 0; k < 4 && rect; k++)
                if ((fabsf(v[k].x - x0) > .5f && fabsf(v[k].x - x1) > .5f) || (fabsf(v[k].y - y0) > .5f && fabsf(v[k].y - y1) > .5f)) rect = 0;
            float a = (x1 - x0) * (y1 - y0);
            if (!rect || a < (float)RW * (float)RH / 20.f || x0 < -1.f || x1 > RW + 1.f) continue;
            int j = 0; while (j < nt && t[j].z != v[0].z) j++;
            if (j == nt) { if (nt == NT) continue; nt++; t[j].z = v[0].z; t[j].x0 = x0; t[j].x1 = x1; t[j].y0 = y0; t[j].y1 = y1; t[j].area = 0; t[j].n = 0; }
            if (x0 < t[j].x0) t[j].x0 = x0; if (x1 > t[j].x1) t[j].x1 = x1;
            if (y0 < t[j].y0) t[j].y0 = y0; if (y1 > t[j].y1) t[j].y1 = y1;
            t[j].area += a; t[j].n++;
        }
        for (int j = 0; j < nt && nscr < 4; j++) {
            float box = (t[j].x1 - t[j].x0) * (t[j].y1 - t[j].y0);
            if (t[j].x0 <= 1.f && t[j].x1 >= RW - 1.f && t[j].y0 <= 1.f && t[j].y1 >= RH - 1.f
                && t[j].area > box * 0.98f && t[j].area < box * 1.02f) scr_z[nscr++] = t[j].z;
        }
    }
    nmesh = 0;
    ngrid = 0;
    if (dc_pvr_gl_extend_backdrops && dc_pvr_gl_widescreen && WW > (float)RW + 1.f) {
        find_meshes(&f, (float)RW, (float)f.height);
        sky_cards = 0;
        for (int i = 0; i < f.npolys && i <= 16; i++) sky_cards += sky_card(&f.polys[i], f.verts + f.polys[i].first, (float)RW, (float)f.height);
        if (dc_pvr_gl_extend_tiles) find_grids(&f, (float)RW);
    }
    for (int i = 0; i < f.npolys; i++) {
        const pvr_poly *p = &f.polys[i];
        const pvr_vtx *v = f.verts + p->first;
        push_dx = 0; push_str = 0;
        if (hud_dx > 0 && hud_side && hud_side[i] == HUD_BAR) { push_wide = hud_dx; push_rw = (float)RW; }
        else if (hud_dx > 0 && hud_side && hud_side[i] >= HUD_MOVE0 && hud_side[i] < HUD_MOVE0 + HUD_MOVE_N)
            push_dx = dc_pvr_gl_hud_zones[hud_side[i] - HUD_MOVE0].move * hud_dx;
        else if (hud_dx > 0 && hud_side && (hud_side[i] >= 8 || hud_side[i] <= -8)) {   /* a stretching zone: outer end out, inner end stays */
            const dc_hud_zone *hz = &dc_pvr_gl_hud_zones[abs(hud_side[i]) - 8];
            float zs = RW / 640.f;
            s_a = hz->x0 * zs; s_b = hz->x1 * zs; s_m = hz->stretch * zs;
            s_a2 = hz->side < 0 ? s_a - hud_dx : s_a; s_b2 = hz->side < 0 ? s_b : s_b + hud_dx;
            push_str = 1;
        }
        else if (hud_dx > 0 && hud_side) {
            push_dx = hud_side[i] * hud_dx;
            if (ovl_z < 1e29f && v[0].z >= ovl_z && i >= ovl_i) {   /* pause shade and menu */
                int flat = 1; for (int k = 1; k < p->n; k++) if (v[k].z != v[0].z) flat = 0;
                /* at the shade's depth only what reaches the picture's edges widens (the shade, or its tiles):
                 * a menu box drawn at that depth stays as it is (Daytona USA 2001's pause menu) */
                float mnx = v[0].x, mxx = v[0].x; for (int k = 1; k < p->n; k++) { if (v[k].x < mnx) mnx = v[k].x; if (v[k].x > mxx) mxx = v[k].x; }
                /* ... and ends there: a tile already reaching past the picture (Soulcalibur's opening scrolls
                 * translucent paper panes past the 4:3 edges at the shade's depth) is not stretched */
                if (flat) { push_dx = 0; if (v[0].z == ovl_z && (mnx <= 1.f || mxx >= RW - 1.f) && mnx >= -1.f && mxx <= RW + 1.f) { push_wide = WW; push_rw = (float)RW; } }
            }
        }
        if (split_wide) {
            float mnx = 1e30f, mxx = -1e30f, mny = 1e30f, mxy = -1e30f; int flat = 1;
            for (int k = 0; k < p->n; k++) {
                if (v[k].x < mnx) mnx = v[k].x; if (v[k].x > mxx) mxx = v[k].x; if (v[k].y < mny) mny = v[k].y; if (v[k].y > mxy) mxy = v[k].y;
                if (v[k].z != v[0].z) flat = 0;
            }
            int moving = hud_side && dc_pvr_gl_hud_move && dc_pvr_gl_hud_z > 0;
            int out = moving ? (hud_side[i] < 0 ? -1 : hud_side[i] > 0 ? 1 : 0) : 0;
            if (split_view(p)) push_dx = view_dx + out * side_dx;
            else if (mnx >= panel_x - 1.f && flat && p->list == 0 && mxy - mny > f.height * 0.6f) { spans[i].first = nverts; spans[i].count = 0; continue; }   /* the panel backdrop */
            else if (flat && !((p->isp >> 25) & 1) && mxy - mny <= 4.f && mxx - mnx > RW * 0.9f) { spans[i].first = nverts; spans[i].count = 0; continue; }   /* the divider line */
            else if (flat && v[0].z >= ovl_z) { push_dx = 0; if (v[0].z == ovl_z) { push_wide = WW; push_rw = (float)RW; } }   /* pause shade and menu */
            else if (mnx >= panel_x + 2.f) push_dx = panel_dx;      /* map, timer (the views' coin icons start just left of it) */
            else if (flat && v[0].z >= dc_pvr_gl_hud_z && dc_pvr_gl_hud_z > 0) push_dx = view_dx + out * side_dx;   /* the views' HUD (drawn unclipped) */
        }
        /* a port's pinned pieces (dc_pvr_gl_hud_pin): to a screen edge on every widescreen frame */
        if (dc_pvr_gl_hud_pin && dc_pvr_gl_widescreen && !lbox && WW > (float)RW + 1.f && p->n >= 3 && v[0].z >= dc_pvr_gl_hud_z) {
            float x0 = v[0].x, x1 = v[0].x, y0 = v[0].y, y1 = v[0].y; int flat = 1;
            for (int k = 1; k < p->n; k++) {
                if (v[k].x < x0) x0 = v[k].x; if (v[k].x > x1) x1 = v[k].x; if (v[k].y < y0) y0 = v[k].y; if (v[k].y > y1) y1 = v[k].y;
                if (v[k].z != v[0].z) flat = 0;
            }
            float k640 = 640.f / (float)RW, k480 = 480.f / (float)f.height;
            (void)flat;                                       /* flat or a small 3D model (a spinning logo) */
            int side = dc_pvr_gl_hud_pin(x0 * k640, y0 * k480, x1 * k640, y1 * k480);
            if (side) push_dx = (float)side * (WW - (float)RW) * 0.5f;
        }
        if (nscr && shade_poly(p, v) && p->n >= 3 && !split_wide && !grid_member(p, v)) {   /* a pattern wall's tiles are repeated instead */
            int flat = 1; for (int k = 1; k < p->n; k++) if (v[k].z != v[0].z) flat = 0;
            for (int j = 0; j < nscr && flat; j++) if (scr_z[j] == v[0].z) { push_dx = 0; push_wide = WW; push_rw = (float)RW; break; }
        }
        if (poly_cap[i]) {
            push_dx = 0; push_wide = WW; push_rw = (float)RW; cap_uv = poly_cap[i] - 1;
            const pvr_vtx *q = f.verts + f.polys[i].first;
            float tw = (float)(8 << ((f.polys[i].tsp >> 3) & 7)), th = (float)(8 << (f.polys[i].tsp & 7));
            float qx0 = q[0].x, qx1 = q[0].x, qy0 = q[0].y, qy1 = q[0].y, u0 = q[0].u, u1 = q[0].u, w0 = q[0].v, w1 = q[0].v;
            for (int k = 1; k < f.polys[i].n; k++) {
                if (q[k].x < qx0) qx0 = q[k].x; if (q[k].x > qx1) qx1 = q[k].x;
                if (q[k].y < qy0) qy0 = q[k].y; if (q[k].y > qy1) qy1 = q[k].y;
                if (q[k].u < u0) u0 = q[k].u; if (q[k].u > u1) u1 = q[k].u;
                if (q[k].v < w0) w0 = q[k].v; if (q[k].v > w1) w1 = q[k].v;
            }
            cap_qx0 = qx0; cap_qy0 = qy0; cap_cx0 = u0 * tw; cap_cy0 = w0 * th;
            cap_sx = (u1 - u0) * tw / (qx1 - qx0); cap_sy = (w1 - w0) * th / (qy1 - qy0);
        }
        /* letterboxed scenes shown wide (dc_pvr_gl_letterbox_4_3 off): the game's bars stop at the 4:3
         * edges, so they are stretched across the full width (Jet Set Radio's tutorial and cut-scenes) */
        else if (dc_pvr_gl_widescreen && !dc_pvr_gl_letterbox_4_3 && WW > (float)RW + 1.f && letterbox_band(p, v, RW, f.height))
            { push_dx = 0; push_wide = WW; push_rw = (float)RW; }
        /* translucent polygons at exactly the same depth: on the PVR the later one is on top (text on its menu's
         * selection bar, a pause title drawn after the shade at its depth: Daytona USA 2001). Each such
         * polygon is set a hair nearer than the ones before it at its depth, so the sort keeps them in order */
        push_zk = 1.f;
        if (p->list == 2) {
            float zmin = v[0].z; for (int k = 1; k < p->n; k++) if (v[k].z < zmin) zmin = v[k].z;
            int run = tdepth_run(zmin);
            if (run > 0) push_zk = 1.f + (float)(run < 255 ? run : 255) * (1.f / 16384.f);
        }
        spans[i].first = nverts;
        if (p->sprite) { tri(p, &v[0], &v[1], &v[2], 0, cull_small); tri(p, &v[0], &v[2], &v[3], 0, cull_small); }
        else for (int k = 0; k + 2 < p->n; k++) tri(p, &v[k], &v[k + 1], &v[k + 2], k & 1, cull_small);
        if (dc_pvr_gl_extend_backdrops && dc_pvr_gl_widescreen && WW > (float)RW + 1.f && push_dx == 0 && push_wide == 0 && cap_uv < 0
            && !mesh_edge(p, v, (float)RW, (float)f.height, (WW - (float)RW) * 0.5f + 2.f)
            && !(dc_pvr_gl_extend_tiles && grid_repeat(p, v, (float)RW, (WW - (float)RW) * 0.5f + 2.f)))
        {
            extend_backdrop(p, v, (float)RW, (WW - (float)RW) * 0.5f + 2.f);
            sky_mirror(p, v, i, (float)RW, (float)f.height, (WW - (float)RW) * 0.5f + 2.f);
        }
        spans[i].count = nverts - spans[i].first;
        push_wide = 0; cap_uv = -1; push_zk = 1.f; push_str = 0;
        if ((p->isp >> 25) & 1) for (int k = 0; k < p->n; k++) { if (v[k].x < cx0) cx0 = v[k].x; if (v[k].x > cx1) cx1 = v[k].x; }
    }
    push_dx = 0;
    int mod_first = nverts;
    for (int i = 0; i < f.nmods; i++) {
        pvr_vtx t[3]; memset(t, 0, sizeof t);
        const pvr_modtri *m = &f.mods[i];
        for (int k = 0; k < 3; k++) { t[k].x = m->x[k]; t[k].y = m->y[k]; t[k].z = m->z[k]; }
        /* the header's culling mode, as in the software renderer: a culled triangle is drawn degenerate */
        float area = (m->x[1] - m->x[0]) * (m->y[2] - m->y[0]) - (m->y[1] - m->y[0]) * (m->x[2] - m->x[0]);
        if ((m->cull == 1 && fabsf(area) < cull_small) || (m->cull == 2 && area < 0) || (m->cull == 3 && area > 0)) t[1] = t[2] = t[0];
        push(&t[0]); push(&t[1]); push(&t[2]);
    }
    quad_first = nverts;
    { pvr_vtx q[4]; memset(q, 0, sizeof q);
      float qx[4] = { -1, 1, -1, 1 }, qy[4] = { -1, -1, 1, 1 };
      for (int k = 0; k < 4; k++) { q[k].x = qx[k]; q[k].y = qy[k]; q[k].z = 1; }
      push(&q[0]); push(&q[1]); push(&q[2]); push(&q[1]); push(&q[3]); push(&q[2]); }
    p_glBindVertexArray(vao);
    p_glBindBuffer(GL_ARRAY_BUFFER, vbo);
    p_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)nverts * (GLsizeiptr)sizeof(gv), verts, GL_STREAM_DRAW);
    if (gst.on < 0) { const char *e = getenv("DC_GL_STATS"); gst.on = e && *e == '1'; }
    if (gst.on > 0) {
        gst.polys += (unsigned long)f.npolys; gst.verts += (unsigned long)nverts; gst.mods += (unsigned long)f.nmods; gst.tav += (unsigned long)f.nverts;
        if (++gst.frames % 300 == 0) {
            fprintf(stderr, "pvr_gl: a frame: %lu polygons, %lu vertices, %lu state changes, %lu draw calls (%lu ranges), %lu volume triangles, %d shader variants\n",
                    gst.polys / 300, gst.verts / 300, gst.states / 300, gst.draws / 300, gst.ranges / 300, gst.mods / 300, sv_n);
            /* over the 300 frames (all the game sent, drawn or not): how many came through the sink */
            fprintf(stderr, "pvr_gl: the game's vertices in the frames drawn: %lu a frame; through the TA's sink in that time: %lu a frame\n",
                    gst.tav / 300, (pvr_sink_vertices - gst.sunk) / 300);
            gst.polys = gst.verts = gst.states = gst.draws = gst.ranges = gst.mods = gst.tav = 0; gst.sunk = pvr_sink_vertices;
        }
    }

    int ms = !capturing && ensure_ms();
    ms_a2c = ms;
    GLuint tgt_fbo = ms ? ms_fbo : fbo;                   /* see ms_fbo */
    p_glBindFramebuffer(GL_FRAMEBUFFER, tgt_fbo);
    p_glViewport(0, 0, fb_w, fb_h);
    /* the frame's uniforms: each variant takes them when it is first used (sv_use) */
    FU.sx = 2.f / WW; FU.sy = 2.f / RH;
    FU.offx = (WW - (float)RW) * 0.5f;
    FU.pt_ref = (dc_pvr_reg(0x11C) & 255) / 255.f;
    { uint32_t fr = dc_pvr_reg(0x0B0), fv = dc_pvr_reg(0x0B4), den = dc_pvr_reg(0x0B8);   /* FOG_COL_RAM, FOG_COL_VERT, FOG_DENSITY */
      FU.fog_ram[0] = ((fr >> 16) & 255) / 255.f; FU.fog_ram[1] = ((fr >> 8) & 255) / 255.f; FU.fog_ram[2] = (fr & 255) / 255.f;
      FU.fog_vert[0] = ((fv >> 16) & 255) / 255.f; FU.fog_vert[1] = ((fv >> 8) & 255) / 255.f; FU.fog_vert[2] = (fv & 255) / 255.f;
      FU.fog_den = ((den >> 8) & 255) / 128.f * ldexpf(1.f, (int8_t)(den & 255));
      uint8_t tab[256];                                    /* FOG_TABLE: a texture on unit 1, sent when it changes */
      for (int i = 0; i < 128; i++) { uint32_t e = dc_pvr_reg(0x200 + i * 4); tab[i * 2] = (uint8_t)(e & 255); tab[i * 2 + 1] = (uint8_t)((e >> 8) & 255); }
      p_glActiveTexture(GL_TEXTURE1); p_glBindTexture(GL_TEXTURE_2D, fog_tex);
      if (!fog_valid || memcmp(tab, fog_last, sizeof tab)) {
          p_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
          p_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 128, 1, GL_RG_INTEGER, GL_UNSIGNED_BYTE, tab);
          memcpy(fog_last, tab, sizeof tab); fog_valid = 1;
      }
      p_glActiveTexture(GL_TEXTURE0); }
    sv_stamp++; sv_cur = NULL;

    p_glDisable(GL_CULL_FACE);
    p_glColorMask(1, 1, 1, 1); p_glDepthMask(1); p_glStencilMask(0xFF);
    /* outside the clip rectangle: black (not for a band, see above); inside: background plane, and all
     * drawing scissored to it */
    {
        if (restore >= 0 && keep[restore].fbw == fb_w && keep[restore].fbh == fb_h) {   /* see keep */
            p_glDisable(GL_SCISSOR_TEST);
            p_glBindFramebuffer(GL_READ_FRAMEBUFFER, keep[restore].fbo); p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, tgt_fbo);
            p_glBlitFramebuffer(0, 0, fb_w, fb_h, 0, 0, fb_w, fb_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            p_glBindFramebuffer(GL_FRAMEBUFFER, tgt_fbo);
        } else if (ms && band) {                          /* a band over what is there: the samples start from it */
            p_glDisable(GL_SCISSOR_TEST);
            p_glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo); p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, ms_fbo);
            p_glBlitFramebuffer(0, 0, fb_w, fb_h, 0, 0, fb_w, fb_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            p_glBindFramebuffer(GL_FRAMEBUFFER, ms_fbo);
        } else if (!band || restore >= 0) {
            p_glDisable(GL_SCISSOR_TEST);
            p_glClearColor(0, 0, 0, 1);
            p_glClear(GL_COLOR_BUFFER_BIT);
        }
    }
    { int sx0 = dc_pvr_gl_widescreen ? 0 : (int)((f.clip_x0 * (float)fb_w) / f.width), sx1 = dc_pvr_gl_widescreen ? fb_w : (int)(((f.clip_x1 + 1) * (float)fb_w) / f.width);
      if (lbox) { float o = (WW - (float)RW) * 0.5f, k = (float)fb_w / WW; sx0 = (int)ceilf(o * k); sx1 = (int)floorf((o + (float)RW) * k); }
      zoom_x0 = zoom_x1 = zoom_y0 = zoom_y1 = 0;
      if (lbox && lb_bot > lb_top && lb_bot - lb_top >= f.height / 3.f) {
          zoom_x0 = sx0; zoom_x1 = sx1;
          zoom_y0 = fb_h - (int)floorf(lb_bot * (float)scale * 0.5f); zoom_y1 = fb_h - (int)ceilf(lb_top * (float)scale * 0.5f);
      }
      else if (dc_pvr_gl_widescreen && band) {             /* a quarter: its side of the screen, out to that edge */
        float o = (WW - (float)RW) * 0.5f, k = (float)fb_w / WW, kx = (float)RW / (float)f.width;
        if (f.clip_x0 > 0) sx0 = (int)floorf((o + (float)f.clip_x0 * kx) * k + 0.5f);
        if (f.clip_x1 + 1 < f.width) sx1 = (int)floorf((o + (float)(f.clip_x1 + 1) * kx) * k + 0.5f);
      }
      int sy0 = SC(f.clip_y0), sy1 = SC(f.clip_y1 + 1);
      p_glEnable(GL_SCISSOR_TEST);
      sc_x = sx0; sc_y = fb_h - sy1; sc_w = sx1 - sx0; sc_h = sy1 - sy0; sc_set[2] = -1;
      sc_off = (WW - (float)RW) * 0.5f; sc_k = (float)fb_w / WW; sc_kx = (float)RW / (float)f.width; sc_scale = scale; sc_rw = f.width;
      scissor(sc_x, sc_y, sc_w, sc_h); }
    p_glClearColor(f.bg[0], f.bg[1], f.bg[2], 1.f);
    p_glClearDepth(depth_clear(f.bgz)); p_glClearStencil(0);
    p_glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    p_glEnable(GL_DEPTH_TEST);
    p_glDisable(GL_BLEND);

    /* opaque, then punch-through; stencil bit 1 = surface modifier volumes act on */
    uint32_t shad = dc_pvr_reg(0x074);
    int cheap = (shad >> 8) & 1, has_mods = 0;
    for (int i = 0; i < f.nmods; i++) if (f.mods[i].list == 1) has_mods = 1;
    /* without volumes nothing reads the stencil: no test, and no writes of a mark nobody looks at */
    if (has_mods) { p_glEnable(GL_STENCIL_TEST); p_glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE); p_glStencilMask(2); }
    else p_glDisable(GL_STENCIL_TEST);
    for (int pass = 0; pass < 2; pass++) {
        int list = pass ? 4 : 0;
        for (int i = 0; i < f.npolys; i++) {
            const pvr_poly *p = &f.polys[i];
            if (p->list != list || !spans[i].count) continue;
            int mark = has_mods && (cheap ? p->shadow : p->twovol);
            batch_add(p, 0, has_mods ? (mark ? 2 : 0) : -1, 0, spans[i].first, spans[i].count);
        }
    }
    batch_reset();

    /* modifier volumes */
    if (has_mods) {
        p_glColorMask(0, 0, 0, 0); p_glDepthMask(0);
        sv_use(SV_PLAIN);
        int any = 0;
        /* the parity bit changes only under the volume's triangles: resolving and clearing it is limited
         * to their bounding box. Soulcalibur's shadows are hundreds of one-triangle volumes per frame, and
         * two full-screen passes for each of them left the GPU fill-bound in fights. */
        float bx0 = 1e30f, bx1 = -1e30f, by0 = 1e30f, by1 = -1e30f, kx = (float)fb_w / WW, ky = (float)fb_h / (float)RH;
        /* a volume's triangles are drawn with one call (the parity flip does not depend on their order):
         * Dead or Alive 2's shadows are hundreds of volume triangles per frame, and a call (and five state
         * changes) for each one made fights CPU-bound in the Windows GL driver */
        int run = -1, parity_state = 0;
        for (int i = 0; i <= f.nmods; i++) {
            const pvr_modtri *m = i < f.nmods ? &f.mods[i] : NULL;
            if (m && m->list == 1 && run < 0) run = i;
            int closes = m && m->list == 1 && (m->inst == 1 || m->inst == 2);
            /* draw the pending run when this triangle ends a volume, a triangle of another list breaks it, or at the end */
            if (run >= 0 && (closes || !m || m->list != 1)) {
                int end = closes ? i + 1 : i;
                if (!parity_state) {
                    p_glEnable(GL_DEPTH_TEST); p_glDepthFunc(GL_GEQUAL);
                    p_glStencilMask(1); p_glStencilFunc(GL_ALWAYS, 0, 0xFF); p_glStencilOp(GL_KEEP, GL_KEEP, GL_INVERT);
                    parity_state = 1;
                }
                p_glDrawArrays(GL_TRIANGLES, mod_first + run * 3, (end - run) * 3);
                run = -1;
            }
            if (!m || m->list != 1) continue;
            for (int k = 0; k < 3; k++) {
                if (m->x[k] < bx0) bx0 = m->x[k]; if (m->x[k] > bx1) bx1 = m->x[k];
                if (m->y[k] < by0) by0 = m->y[k]; if (m->y[k] > by1) by1 = m->y[k];
            }
            if (closes) {                                  /* volume closed: parity -> bit 2, clear parity */
                parity_state = 0;
                {   /* GL pixels of the box (rows go up), a pixel of margin, inside the render's own scissor */
                    float fx0 = (bx0 + sc_off) * kx - 1.f, fx1 = (bx1 + sc_off) * kx + 2.f;
                    float fy0 = (float)fb_h - by1 * ky - 1.f, fy1 = (float)fb_h - by0 * ky + 2.f;
                    int x0 = fx0 < (float)sc_x ? sc_x : (int)fx0, x1 = fx1 > (float)(sc_x + sc_w) ? sc_x + sc_w : (int)fx1;
                    int y0 = fy0 < (float)sc_y ? sc_y : (int)fy0, y1 = fy1 > (float)(sc_y + sc_h) ? sc_y + sc_h : (int)fy1;
                    scissor(x0, y0, x1 - x0, y1 - y0);
                    bx0 = by0 = 1e30f; bx1 = by1 = -1e30f;
                }
                p_glDisable(GL_DEPTH_TEST);
                p_glStencilMask(4); p_glStencilFunc(GL_EQUAL, 4 | (m->inst == 1 ? 1 : 0), 1); p_glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
                draw_quad(0, 0, 0, 0);
                p_glStencilMask(1); p_glStencilFunc(GL_ALWAYS, 0, 0xFF); p_glStencilOp(GL_ZERO, GL_ZERO, GL_ZERO);
                draw_quad(0, 0, 0, 0);
                scissor(sc_x, sc_y, sc_w, sc_h);           /* the next volume's triangles: the whole render */
                any = 1;
            }
        }
        p_glColorMask(1, 1, 1, 1);
        p_glStencilMask(0); p_glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        p_glStencilFunc(GL_EQUAL, 6, 6);                   /* affected surface inside a volume */
        if (any && cheap) {
            float k = (shad & 255) / 256.f;
            p_glDisable(GL_DEPTH_TEST);
            p_glEnable(GL_BLEND); p_glBlendFunc(GL_ZERO, GL_CONSTANT_COLOR); p_glBlendColor(k, k, k, 1);
            draw_quad(1, 1, 1, 1);
            p_glDisable(GL_BLEND);
        } else if (any) {
            p_glEnable(GL_DEPTH_TEST);
            for (int i = 0; i < f.npolys; i++) {
                const pvr_poly *p = &f.polys[i];
                if (!p->twovol || (p->list != 0 && p->list != 4) || !spans[i].count) continue;
                batch_add(p, 1, -1, 0, spans[i].first, spans[i].count);
            }
            batch_reset();
        }
    }
    p_glDisable(GL_STENCIL_TEST);
    p_glEnable(GL_DEPTH_TEST);

    /* translucent: presorted (ISP_FEED_CFG bit 0) or back to front by nearest 1/w */
    int presort = dc_pvr_reg(0x098) & 1, nt = 0;
    sort_e *s = malloc(sizeof *s * (f.npolys ? f.npolys : 1));
    for (int i = 0; i < f.npolys; i++) if (f.polys[i].list == 2) {
        float zmin = 1e30f;
        for (int k = 0; k < f.polys[i].n; k++) if (f.verts[f.polys[i].first + k].z < zmin) zmin = f.verts[f.polys[i].first + k].z;
        s[nt].idx = i; s[nt].key = presort ? (float)nt : zmin; nt++;
    }
    if (!presort) qsort(s, nt, sizeof *s, cmp_sort);
    p_glEnable(GL_BLEND);
    if (!presort && dc_pvr_gl_topaque) {
        tpass = 1;
        for (int k = 0; k < nt; k++) {
            const pvr_poly *p = &f.polys[s[k].idx];
            if (!two_pass_poly(p)) continue;
            batch_add(p, 0, -1, 1, spans[s[k].idx].first, spans[s[k].idx].count);
        }
        batch_reset();
        tpass = 2;
    }
    if (!presort && dc_pvr_gl_tsort2) {
        /* the PVR sorts translucent fragments per pixel; sorting whole polygons (a strip can be a
         * fence along a whole side of the court) put a large one in front of smaller ones before it
         * (Tennis 2K2's fence over the boards in Casablanca). Polygons spanning a deep range are sorted
         * per triangle instead, each by its own farthest point; the rest stay whole (a strip at one depth
         * layering over itself, Tennis 2K2's ship's wake, keeps its order); equal keys keep submission order. */
        int ntri = 0;
        for (int k = 0; k < nt; k++) ntri += spans[s[k].idx].count / 3;
        tri_e *t = malloc(sizeof *t * (ntri ? ntri : 1)); int m = 0;
        for (int k = 0; k < nt; k++) {
            int first = spans[s[k].idx].first, cnt = spans[s[k].idx].count / 3;
            if (!cnt) continue;
            const pvr_poly *p = &f.polys[s[k].idx]; const pvr_vtx *v = f.verts + p->first;
            float z0 = v[0].z, z1 = v[0].z, amin = v[0].c[3];
            for (int j = 1; j < p->n; j++) { if (v[j].z < z0) z0 = v[j].z; if (v[j].z > z1) z1 = v[j].z; if (v[j].c[3] < amin) amin = v[j].c[3]; }
            /* split: deep, of one see-through-ness throughout (texture alpha ignored, vertex alpha
             * solid: a fence or net drawn see-through as a whole) and not reaching the camera. Layers
             * that blend by their texture's alpha (a ship's wake fading out over the sea) or a sea or
             * sky sheet keep the order they had */
            if (cnt <= 2 || z1 < z0 * 1.5f || z1 > 1.f || amin < 0.99f || !((p->tsp >> 19) & 1)) {   /* one unit, as before */
                t[m].first = first; t[m].count = cnt * 3; t[m].poly = s[k].idx; t[m].key = s[k].key; m++;
                continue;
            }
            for (int j = 0; j < cnt; j++, m++) {        /* deep (a fence along a court side): per triangle */
                const gv *g = &verts[first + 3 * j];
                float zm = g[0].z; if (g[1].z < zm) zm = g[1].z; if (g[2].z < zm) zm = g[2].z;
                t[m].first = first + 3 * j; t[m].count = 3; t[m].poly = s[k].idx; t[m].key = zm;
            }
        }
        ntri = m;
        qsort(t, ntri, sizeof *t, cmp_tri);
        for (int j = 0; j < ntri; j++) batch_add(&f.polys[t[j].poly], 0, -1, 1, t[j].first, t[j].count);
        free(t);
    } else
    for (int k = 0; k < nt; k++) {
        const pvr_poly *p = &f.polys[s[k].idx];
        batch_add(p, 0, -1, 1, spans[s[k].idx].first, spans[s[k].idx].count);
    }
    batch_reset();
    tpass = 0;
    free(s);
    p_glDisable(GL_BLEND);
    p_glDisable(GL_SCISSOR_TEST);
    p_glDepthMask(1);
    if (ms_a2c) { p_glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE); ms_a2c = 0; }
    if (ms) {                                             /* resolve: the rest works on fbo */
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, ms_fbo); p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
        p_glBlitFramebuffer(0, 0, fb_w, fb_h, 0, 0, fb_w, fb_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    }
    if (dc_pvr_gl_widescreen && dc_pvr_gl_clip_2d && !lbox && WW > (float)RW + 1.f) {
        /* a 2D frame (no 3D scene of 50+ polygons reaching past the 4:3 picture): what the game draws
         * beyond the 4:3 edges (a background's overhang, a ticker running on) is cleared, and the side
         * fill below gives it bars like any menu */
        int n3d = 0, wide3d = 0;
        for (int i = 0; i < f.npolys && !(n3d >= 50 && wide3d); i++) {
            const pvr_vtx *v = f.verts + f.polys[i].first;
            if (f.polys[i].n < 3 || (dc_pvr_gl_hud_z > 0 && v[0].z >= dc_pvr_gl_hud_z) || (v[0].z == v[1].z && v[0].z == v[2].z)) continue;
            n3d++;
            for (int k = 0; k < f.polys[i].n && !wide3d; k++) if (v[k].x < -2.f || v[k].x > RW + 2.f) wide3d = 1;
        }
        /* a full-width capture of a 3D scene on screen (a held replay frame) keeps its sides; a capture
         * of a 2D screen (Tennis 2K2's player card, faded in through a capture) is 2D like it */
        int shows_cap = 0;
        for (int i = 0; i < f.npolys && !shows_cap; i++) shows_cap = poly_cap[i] != 0 && !cap[poly_cap[i] - 1].flat2d;
        int is2d = !(n3d >= 50 && wide3d) || backdrop_card(&f, RW);
        if (capturing) pass_2d = is2d;
        if (!shows_cap && is2d) {
            float off = (WW - (float)RW) * 0.5f, k = (float)fb_w / WW;
            int l = (int)floorf(off * k), r = (int)ceilf((off + (float)RW) * k);
            p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            p_glEnable(GL_SCISSOR_TEST); p_glClearColor(0, 0, 0, 1);
            /* only this render's lines: a band render (see above) leaves the others alone */
            if (l > 0) { p_glScissor(0, sc_y, l, sc_h); p_glClear(GL_COLOR_BUFFER_BIT); }
            if (r < fb_w) { p_glScissor(r, sc_y, fb_w - r, sc_h); p_glClear(GL_COLOR_BUFFER_BIT); }
            p_glDisable(GL_SCISSOR_TEST);
        }
    }
    /* not in races (their HUD is out there), nor over a pattern wall or a mesh backdrop carried on past the
     * picture (black where its tiles turn edge-on is the wall's own) */
    if (dc_pvr_gl_widescreen && dc_pvr_gl_side_fill && hud_dx == 0 && !lbox && !ngrid && !nmesh) {
        /* bars start where the picture's textured content ends when that is just short of the edge
         * (Rush's title image stops 3 pixels short of the right edge, which left a black line); content
         * that ends further in is a picture on an untextured background (Bomberman Online's logos on
         * the grey background plane), whose bars start at the edge */
        if (cx0 > cx1 || cx0 > 16.f) cx0 = 0;
        if (cx0 > cx1 || cx1 < (float)RW - 16.f) cx1 = (float)RW;
        if (cx0 < 0) cx0 = 0;
        if (cx1 > (float)RW) cx1 = (float)RW;
        float off = (WW - (float)RW) * 0.5f, k = (float)fb_w / WW;
        int px0 = (int)ceilf((off + cx0) * k), px1 = (int)floorf((off + cx1) * k);
        /* a side band the game drew into (a 3D scene past the picture) keeps it; an empty (black) one gets
         * a bar: menus, the title, the panel beside Rush's split-screen views. One pixel column each,
         * just outside the picture. */
        int y0 = fb_h - SC(f.clip_y1 + 1), y1 = fb_h - SC(f.clip_y0), fill[2] = { 0, 0 };
        int gpu = dc_pvr_gl_gpu_bars && edges_ok(px0, px1, y0, y1) && px0 > 4 && px1 < fb_w - 4 && gpu_bars_init(y1 - y0);
        if (gpu) {                                           /* see dc_pvr_gl_gpu_bars */
            edge_blit(px0, px1, y0, y1);
            gpu_bars_decide(px0 - 3, px1 + 2, y0, y1);
            fill[0] = fill[1] = 1;
        } else if (y1 > y0 && px0 > 4 && px1 < fb_w - 4) {
            static uint8_t *col; static int cap_col;
            if (y1 - y0 > cap_col) { cap_col = y1 - y0; col = realloc(col, (size_t)cap_col * 4); }
            p_glBindFramebuffer(GL_FRAMEBUFFER, fbo); p_glPixelStorei(GL_PACK_ALIGNMENT, 1);
            for (int sd = 0; sd < 2; sd++) {
                p_glReadPixels(sd ? px1 + 2 : px0 - 3, y0, 1, y1 - y0, GL_RGBA, GL_UNSIGNED_BYTE, col);
                fill[sd] = column_empty(col, y1 - y0);
            }
        }
        if (fill[0] || fill[1]) draw_bars(px0, px1, y0, y1, fill[0], fill[1], gpu);
    }
    pic_offy = ((float)(f.height - 1 - f.clip_y1) - (float)f.clip_y0) * 0.5f / (float)f.height;
    pic_y0 = fb_h - SC(f.clip_y1 + 1); pic_y1 = fb_h - SC(f.clip_y0);
    /* odd clip or one band (a split-screen view, a loading screen's progress bar): the whole picture
     * where it is, not centred on the band (Looney Tunes Space Race's loading bar near the bottom) */
    if (pic_y0 < 0 || pic_y1 > fb_h || (pic_y1 - pic_y0) * 4 < fb_h * 3) { pic_y0 = pic_y1 = 0; pic_offy = 0; }
    /* dc_pvr_gl_letterbox_zoom for a movie: one textured quad over the whole picture whose video has
     * black bars of its own (Looney Tunes Space Race's intro movie) is zoomed like a letterboxed scene.
     * Found from the rendered rows; while the picture is all black the last bars stay. */
    {
        static int mv_y0, mv_y1;                            /* FBO rows of the video between its bars */
        int movie = 0;
        if (!capturing && dc_pvr_gl_widescreen && dc_pvr_gl_letterbox_zoom && !lbox && !band) {
            for (int i = 0; i < f.npolys; i++) {
                const pvr_poly *p = &f.polys[i]; const pvr_vtx *v = f.verts + p->first;
                if (p->n < 3 || !((p->isp >> 25) & 1)) continue;
                float x0 = v[0].x, x1 = v[0].x, y0 = v[0].y, y1 = v[0].y;
                for (int k = 1; k < p->n; k++) { if (v[k].x < x0) x0 = v[k].x; if (v[k].x > x1) x1 = v[k].x; if (v[k].y < y0) y0 = v[k].y; if (v[k].y > y1) y1 = v[k].y; }
                if (x0 <= 1.f && x1 >= RW - 1.f && y0 <= 1.f && y1 >= f.height - 1.f && movie == 0) movie = 1;
                else { movie = -1; break; }               /* anything else textured: not a movie */
            }
        }
        if (movie == 1) {
            float o = (WW - (float)RW) * 0.5f, k = (float)fb_w / WW;
            int px0 = (int)ceilf(o * k), px1 = (int)floorf((o + (float)RW) * k);
            static uint8_t *col; static int cap_col;
            if (fb_h * 4 > cap_col) { cap_col = fb_h * 4; col = realloc(col, (size_t)cap_col); }
            p_glBindFramebuffer(GL_FRAMEBUFFER, fbo); p_glPixelStorei(GL_PACK_ALIGNMENT, 1);
            int top = fb_h, bot = -1;                       /* GL rows: lit rows' range over three columns */
            for (int c = 1; c <= 3; c++) {
                p_glReadPixels(px0 + (px1 - px0) * c / 4, 0, 1, fb_h, GL_RGBA, GL_UNSIGNED_BYTE, col);
                for (int y = 0; y < fb_h; y++) if (col[y * 4] + col[y * 4 + 1] + col[y * 4 + 2] > 30) { if (y < top) top = y; if (y > bot) bot = y; }
            }
            if (bot >= top) {
                if (top >= fb_h / 16 && fb_h - 1 - bot >= fb_h / 16 && (bot - top) * 3 >= fb_h) { mv_y0 = top; mv_y1 = bot + 1; }
                else mv_y0 = mv_y1 = 0;                     /* fills the picture: no bars */
            }
            if (mv_y1 > mv_y0) { zoom_x0 = px0; zoom_x1 = px1; zoom_y0 = mv_y0; zoom_y1 = mv_y1; }
        } else if (!capturing) mv_y0 = mv_y1 = 0;
    }
    if (!capturing && dc_pvr_gl_keep_outside) {            /* see keep: this buffer's picture now */
        int k = 0, old = 0;
        while (k < NKEEP && !(keep[k].tex && keep[k].tgt == cur_tgt)) { if (keep[k].frame < keep[old].frame) old = k; k++; }
        if (k == NKEEP) k = old;
        if (keep[k].fbw != fb_w || keep[k].fbh != fb_h || !keep[k].tex) {
            if (keep[k].tex) { p_glDeleteFramebuffers(1, &keep[k].fbo); p_glDeleteTextures(1, &keep[k].tex); }
            p_glGenTextures(1, &keep[k].tex); p_glBindTexture(GL_TEXTURE_2D, keep[k].tex);
            p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, fb_w, fb_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            p_glGenFramebuffers(1, &keep[k].fbo); p_glBindFramebuffer(GL_FRAMEBUFFER, keep[k].fbo);
            p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, keep[k].tex, 0);
            keep[k].fbw = fb_w; keep[k].fbh = fb_h;
        }
        p_glDisable(GL_SCISSOR_TEST);
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo); p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, keep[k].fbo);
        p_glBlitFramebuffer(0, 0, fb_w, fb_h, 0, 0, fb_w, fb_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        keep[k].tgt = cur_tgt; keep[k].w = f.width; keep[k].h = f.height; keep[k].frame = frame_no;
    }
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

/* the window as presented (call before swapping), RGB8 top row first, */
void dc_pvr_gl_read_window(uint8_t *rgb, int win_w, int win_h) {
    uint8_t *tmp = malloc((size_t)win_w * win_h * 4);
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0); p_glPixelStorei(GL_PACK_ALIGNMENT, 1);
    p_glReadPixels(0, 0, win_w, win_h, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
    for (int y = 0; y < win_h; y++)
        for (int x = 0; x < win_w; x++) memcpy(rgb + ((size_t)y * win_w + x) * 3, tmp + ((size_t)(win_h - 1 - y) * win_w + x) * 4, 3);
    free(tmp);
}

void dc_pvr_gl_read(uint8_t *rgb, int *w, int *h) {
    *w = fb_w; *h = fb_h;
    if (!fbo) return;
    uint8_t *tmp = malloc((size_t)fb_w * fb_h * 4);
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    p_glPixelStorei(GL_PACK_ALIGNMENT, 1);
    p_glReadPixels(0, 0, fb_w, fb_h, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    for (int y = 0; y < fb_h; y++)                         /* GL rows are bottom-up */
        for (int x = 0; x < fb_w; x++) memcpy(rgb + ((size_t)y * fb_w + x) * 3, tmp + ((size_t)(fb_h - 1 - y) * fb_w + x) * 4, 3);
    free(tmp);
}

/* render to texture in widescreen: the pass (its lists are the current ones) is drawn with GL into a
 * capture at the screen's full width; full-screen quads that show that texture later draw the
 * capture instead, so a crossfade over a captured frame (Tennis 2K2's replays) covers the 16:9
 * picture. The texture in VRAM (dc_pvr_sw_render) stays as it is for everything else. */
void dc_pvr_sw_store(const uint8_t *rgba, int w, int h);
void dc_pvr_target_size(int *w, int *h);
static int capture_vram;                 /* dc_pvr_gl_capture_to_vram: also store the picture in VRAM */
static void capture_readback(int c);
int dc_pvr_gl_capture_to_vram(int scale) {
    if (!ready || !dc_pvr_gl_widescreen) return 0;
    capture_vram = 1;
    dc_pvr_gl_capture(scale);
    capture_vram = 0;
    return 1;
}
void dc_pvr_gl_capture(int scale) {
    if (!ready || !dc_pvr_gl_widescreen) return;
    uint32_t addr = dc_pvr_reg(0x060) & 0x7FFFFF;
    int c = 0, old = 0;
    while (c < NCAP && !(cap[c].tex && cap[c].addr == addr)) { if (cap[c].frame < cap[old].frame) old = c; c++; }
    if (c == NCAP) c = old;
    /* swap the screen's target for the capture's, draw, swap back (with what the present uses) */
    GLuint sf = fbo, st = color_tex, sd = depth_rb; GLenum sdf = fbo_dfmt; int sw = fb_w, sh = fb_h;
    float sa = disp_aspect, so = pic_offy, sww = last_ww, srw = last_rw, srh = last_rh; int sy0 = pic_y0, sy1 = pic_y1, zx0 = zoom_x0, zx1 = zoom_x1, zy0 = zoom_y0, zy1 = zoom_y1;
    if (cap[c].tex) {                   /* keep what is there now: the pass may draw it again (a trail) */
        if (cap[NCAP].w != cap[c].w || cap[NCAP].h != cap[c].h || !cap[NCAP].tex) {
            if (cap[NCAP].tex) { p_glDeleteFramebuffers(1, &cap[NCAP].fbo); p_glDeleteTextures(1, &cap[NCAP].tex); }
            p_glGenTextures(1, &cap[NCAP].tex); p_glBindTexture(GL_TEXTURE_2D, cap[NCAP].tex);
            p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, cap[c].w, cap[c].h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            p_glGenFramebuffers(1, &cap[NCAP].fbo); p_glBindFramebuffer(GL_FRAMEBUFFER, cap[NCAP].fbo);
            p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, cap[NCAP].tex, 0);
            cap[NCAP].w = cap[c].w; cap[NCAP].h = cap[c].h;
        }
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, cap[c].fbo); p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, cap[NCAP].fbo);
        p_glDisable(GL_SCISSOR_TEST);
        p_glBlitFramebuffer(0, 0, cap[c].w, cap[c].h, 0, 0, cap[c].w, cap[c].h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        cap[NCAP].addr = addr; cap[NCAP].ww = cap[c].ww; cap[NCAP].rw = cap[c].rw; cap[NCAP].rh = cap[c].rh;
    } else if (cap[NCAP].tex) cap[NCAP].addr = 0xFFFFFFFFu;
    fbo = cap[c].fbo; color_tex = cap[c].tex; depth_rb = cap[c].depth; fbo_dfmt = cap[c].dfmt; fb_w = cap[c].w; fb_h = cap[c].h;
    capturing = 1; cap_cur = c; pass_2d = 0;
    dc_pvr_gl_render(scale);
    capturing = 0; cap_cur = -1;
    {   /* the texture's alpha: RGB565 / 555 / 888 targets have none (opaque); ARGB4444 / 1555 / 8888 keep the drawn one */
        uint32_t mode = dc_pvr_reg(0x048) & 7;
        if (mode == 1 || mode == 4 || mode == 5) {
            p_glBindFramebuffer(GL_FRAMEBUFFER, fbo); p_glDisable(GL_SCISSOR_TEST);
            p_glColorMask(0, 0, 0, 1); p_glClearColor(0, 0, 0, 1); p_glClear(GL_COLOR_BUFFER_BIT); p_glColorMask(1, 1, 1, 1);
        }
    }
    cap[c].fbo = fbo; cap[c].tex = color_tex; cap[c].depth = depth_rb; cap[c].dfmt = fbo_dfmt; cap[c].w = fb_w; cap[c].h = fb_h;
    cap[c].addr = addr; cap[c].ww = last_ww; cap[c].rw = last_rw; cap[c].rh = last_rh; cap[c].frame = frame_no;
    cap[c].flat2d = pass_2d;
    if (capture_vram) capture_readback(c);
    fbo = sf; color_tex = st; depth_rb = sd; fbo_dfmt = sdf; fb_w = sw; fb_h = sh;
    disp_aspect = sa; pic_offy = so; pic_y0 = sy0; pic_y1 = sy1; zoom_x0 = zx0; zoom_x1 = zx1; zoom_y0 = zy0; zoom_y1 = zy1; last_ww = sww; last_rw = srw; last_rh = srh;
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void present_scanlines(int x, int y, int w, int h, int lines, int win_w, int win_h);
/* the picture into the window. Scaled down by more than 1.5x (a high render resolution in a smaller window)
 * each window pixel averages the render pixels it covers (supersampling), where a plain linear blit would
 * pick 2x2 of them and skip the rest: shimmering edges and sparkling textures. Rects in GL pixels. */
static GLuint ds_prog, ds_vao; static GLint ds_rect, ds_src, ds_foot, ds_n, ds_texloc, ds_nb, ds_sharp; static int ds_failed;
static const char *ds_vs =
"#version 330 core\n"
"layout(location=0) in vec2 a_pos; uniform vec4 u_rect; out vec2 v_uv;\n"
"void main() { v_uv = a_pos; gl_Position = vec4(mix(u_rect.xy, u_rect.zw, a_pos), 0.0, 1.0); }\n";
static const char *ds_fs =
"#version 330 core\n"
"in vec2 v_uv; uniform sampler2D u_tex; uniform vec4 u_src; uniform vec2 u_foot, u_nb; uniform int u_n; uniform float u_sharp; out vec4 o_col;\n"
"vec3 avg(vec2 c) {\n"
"  vec3 s = vec3(0.0);\n"
"  for (int j = 0; j < u_n; j++) for (int i = 0; i < u_n; i++)\n"     /* bilinear taps, 2x2 texels each */
"    s += texture(u_tex, c + u_foot * ((vec2(float(i), float(j)) + 0.5) / float(u_n) - 0.5)).rgb;\n"
"  return s / float(u_n * u_n);\n"
"}\n"
"void main() {\n"
"  vec2 c = mix(u_src.xy, u_src.zw, v_uv);\n"
"  vec3 m = avg(c);\n"
"  if (u_sharp > 0.0) {\n"    /* contrast-adaptive sharpening (after AMD's CAS): less where the contrast is already high */
"    vec3 a = texture(u_tex, c + vec2(0.0, u_nb.y)).rgb, b = texture(u_tex, c - vec2(u_nb.x, 0.0)).rgb;\n"
"    vec3 d = texture(u_tex, c + vec2(u_nb.x, 0.0)).rgb, e = texture(u_tex, c - vec2(0.0, u_nb.y)).rgb;\n"
"    vec3 mn = min(min(min(a, b), min(d, e)), m), mx = max(max(max(a, b), max(d, e)), m);\n"
"    vec3 amp = sqrt(clamp(min(mn, 1.0 - mx) / max(mx, vec3(1e-4)), 0.0, 1.0));\n"
"    vec3 w = -amp / mix(8.0, 5.0, u_sharp);\n"
"    m = clamp((m + (a + b + d + e) * w) / (1.0 + 4.0 * w), 0.0, 1.0);\n"
"  }\n"
"  o_col = vec4(m, 1.0);\n"
"}\n";
static int ds_init(void) {
    if (ds_prog) return 1;
    if (ds_failed) return 0;
    ds_prog = p_glCreateProgram();
    p_glAttachShader(ds_prog, compile(GL_VERTEX_SHADER, ds_vs)); p_glAttachShader(ds_prog, compile(GL_FRAGMENT_SHADER, ds_fs));
    p_glLinkProgram(ds_prog);
    GLint ok = 0; p_glGetProgramiv(ds_prog, GL_LINK_STATUS, &ok);
    if (!ok) { ds_prog = 0; ds_failed = 1; return 0; }
    ds_rect = p_glGetUniformLocation(ds_prog, "u_rect"); ds_src = p_glGetUniformLocation(ds_prog, "u_src");
    ds_foot = p_glGetUniformLocation(ds_prog, "u_foot"); ds_n = p_glGetUniformLocation(ds_prog, "u_n");
    ds_texloc = p_glGetUniformLocation(ds_prog, "u_tex");
    ds_nb = p_glGetUniformLocation(ds_prog, "u_nb"); ds_sharp = p_glGetUniformLocation(ds_prog, "u_sharp");
    static const float q[12] = { 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 0, 1 };
    GLuint vb; p_glGenVertexArrays(1, &ds_vao); p_glBindVertexArray(ds_vao);
    p_glGenBuffers(1, &vb); p_glBindBuffer(GL_ARRAY_BUFFER, vb);
    p_glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STREAM_DRAW);
    p_glEnableVertexAttribArray(0); p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void *)0);
    p_glBindVertexArray(vao);
    return 1;
}
int dc_pvr_gl_plain_scaling;           /* 1: always the plain linear blit (comparisons) */
float dc_pvr_gl_sharpen;               /* 0: off; up to 1: contrast-adaptive sharpening of the presented picture */
static void present_blit(int x0, int y0, int x1, int y1, int dx0, int dy0, int dx1, int dy1, int win_w, int win_h) {
    float rx = (float)(x1 - x0) / (float)(dx1 - dx0), ry = (float)(y1 - y0) / (float)(dy1 - dy0);
    float r = rx > ry ? rx : ry;
    if ((r <= 1.5f && dc_pvr_gl_sharpen <= 0.f) || dc_pvr_gl_plain_scaling || !ds_init()) {
        p_glBlitFramebuffer(x0, y0, x1, y1, dx0, dy0, dx1, dy1, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        return;
    }
    int n = r <= 1.5f ? 1 : (int)ceilf(r * 0.5f); if (n > 8) n = 8;
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    p_glViewport(0, 0, win_w, win_h);
    p_glDisable(GL_DEPTH_TEST); p_glDisable(GL_STENCIL_TEST); p_glDisable(GL_SCISSOR_TEST); p_glDisable(GL_CULL_FACE); p_glDisable(GL_BLEND);
    p_glColorMask(1, 1, 1, 1);
    p_glUseProgram(ds_prog);
    p_glActiveTexture(GL_TEXTURE0); p_glBindTexture(GL_TEXTURE_2D, color_tex);
    p_glUniform1i(ds_texloc, 0); p_glUniform1i(ds_n, n);
    p_glUniform4f(ds_rect, dx0 * 2.f / win_w - 1, dy0 * 2.f / win_h - 1, dx1 * 2.f / win_w - 1, dy1 * 2.f / win_h - 1);
    p_glUniform4f(ds_src, (float)x0 / fb_w, (float)y0 / fb_h, (float)x1 / fb_w, (float)y1 / fb_h);
    p_glUniform2f(ds_foot, r <= 1.5f ? 0.f : rx / fb_w, r <= 1.5f ? 0.f : ry / fb_h);
    p_glUniform2f(ds_nb, (rx > 1.f ? rx : 1.f) / fb_w, (ry > 1.f ? ry : 1.f) / fb_h);   /* a window pixel, at least a render pixel */
    p_glUniform1f(ds_sharp, dc_pvr_gl_sharpen > 1.f ? 1.f : dc_pvr_gl_sharpen);
    p_glBindVertexArray(ds_vao);
    p_glDrawArrays(GL_TRIANGLES, 0, 6);
    p_glBindVertexArray(vao); sv_cur = NULL;
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo); p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
}
void dc_pvr_gl_present(int win_w, int win_h, int stretch) {
    if (!fbo) return;
    int dw = win_w, dh = win_h, dx = 0, dy = 0;
    if (!stretch) {                                        /* keep 4:3 (16:9 in widescreen), letterbox */
        if (win_w > win_h * disp_aspect) { dw = (int)(win_h * disp_aspect + 0.5f); dx = (win_w - dw) / 2; }
        else { dh = (int)(win_w / disp_aspect + 0.5f); dy = (win_h - dh) / 2; }
    }
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    p_glViewport(0, 0, win_w, win_h);
    p_glClearColor(0, 0, 0, 1); p_glClear(GL_COLOR_BUFFER_BIT);
    if (zoom_x1 > zoom_x0 && zoom_y1 > zoom_y0) {          /* a letterboxed cut-scene, cropped to fill the window */
        float px = disp_aspect * (float)fb_h / (float)fb_w;    /* a target pixel's shape */
        float sw = (float)(zoom_x1 - zoom_x0) * px, shh = (float)(zoom_y1 - zoom_y0), want = (float)dw / (float)dh;
        int x0 = zoom_x0, x1 = zoom_x1, y0 = zoom_y0, y1 = zoom_y1;
        if (sw / shh > want) { int c = (int)(((float)(x1 - x0) - want * shh / px) * 0.5f); x0 += c; x1 -= c; }
        else { int c = (int)((shh - sw / want) * 0.5f); y0 += c; y1 -= c; }
        present_blit(x0, y0, x1, y1, dx, dy, dx + dw, dy + dh, win_w, win_h);
    } else if (dc_pvr_gl_fill_lines && pic_y1 > pic_y0) {         /* the drawn lines fill the height */
        present_blit(0, pic_y0, fb_w, pic_y1, dx, dy, dx + dw, dy + dh, win_w, win_h);
        if (dc_pvr_gl_scanlines) present_scanlines(dx, dy, dw, dh, (pic_y1 - pic_y0) * 2 / (fb_scale > 0 ? fb_scale : 2), win_w, win_h);
    } else {
        int sh = (int)(pic_offy * (float)dh + (pic_offy >= 0 ? 0.5f : -0.5f));   /* centre the drawn lines (GL rows go up) */
        present_blit(0, 0, fb_w, fb_h, dx, dy - sh, dx + dw, dy + dh - sh, win_w, win_h);
        if (dc_pvr_gl_scanlines) present_scanlines(dx, dy - sh, dw, dh, fb_h * 2 / (fb_scale > 0 ? fb_scale : 2), win_w, win_h);
    }
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

/* ------------------------------------------------------------------ on top of the presented picture
 * Scanlines: each console line darkens towards its edges, like a CRT's beam (dc_pvr_gl_scanlines 1-3:
 * light, medium, strong). Overlays: an RGBA image drawn over the window, for the frontend's menu. */
int dc_pvr_gl_scanlines;
static GLuint ov_prog, ov_vao, ov_vbo, ov_tex, sl_prog;
static GLint ov_rect, ov_texloc, sl_rect, sl_y0, sl_h, sl_lines, sl_k;
static const char *ov_vs =
"#version 330 core\n"
"layout(location=0) in vec2 a_pos; uniform vec4 u_rect; out vec2 v_uv;\n"
"void main() { v_uv = vec2(a_pos.x, 1.0 - a_pos.y); gl_Position = vec4(mix(u_rect.xy, u_rect.zw, a_pos), 0.0, 1.0); }\n";
static const char *ov_fs =
"#version 330 core\n"
"in vec2 v_uv; uniform sampler2D u_tex; out vec4 o_col;\n"
"void main() { o_col = texture(u_tex, v_uv); }\n";
static const char *sl_fs =
"#version 330 core\n"
"uniform float u_y0, u_h, u_lines, u_k; out vec4 o_col;\n"
"void main() {\n"
"  float t = fract((gl_FragCoord.y - u_y0) / u_h * u_lines);\n"
"  float d = 1.0 - u_k * (1.0 - sin(3.14159265 * t));\n"   /* 1 in a line's middle, 1 - k at its edges */
"  o_col = vec4(d, d, d, 1.0);\n"
"}\n";
static int ov_init(void) {
    if (ov_prog) return 1;
    GLuint vs = compile(GL_VERTEX_SHADER, ov_vs);
    ov_prog = p_glCreateProgram();
    p_glAttachShader(ov_prog, vs); p_glAttachShader(ov_prog, compile(GL_FRAGMENT_SHADER, ov_fs)); p_glLinkProgram(ov_prog);
    sl_prog = p_glCreateProgram();
    p_glAttachShader(sl_prog, vs); p_glAttachShader(sl_prog, compile(GL_FRAGMENT_SHADER, sl_fs)); p_glLinkProgram(sl_prog);
    GLint ok1 = 0, ok2 = 0; p_glGetProgramiv(ov_prog, GL_LINK_STATUS, &ok1); p_glGetProgramiv(sl_prog, GL_LINK_STATUS, &ok2);
    if (!ok1 || !ok2) { ov_prog = 0; return 0; }
    ov_rect = p_glGetUniformLocation(ov_prog, "u_rect"); ov_texloc = p_glGetUniformLocation(ov_prog, "u_tex");
    sl_rect = p_glGetUniformLocation(sl_prog, "u_rect"); sl_y0 = p_glGetUniformLocation(sl_prog, "u_y0");
    sl_h = p_glGetUniformLocation(sl_prog, "u_h"); sl_lines = p_glGetUniformLocation(sl_prog, "u_lines"); sl_k = p_glGetUniformLocation(sl_prog, "u_k");
    static const float q[12] = { 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 0, 1 };
    p_glGenVertexArrays(1, &ov_vao); p_glBindVertexArray(ov_vao);
    p_glGenBuffers(1, &ov_vbo); p_glBindBuffer(GL_ARRAY_BUFFER, ov_vbo);
    p_glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STREAM_DRAW);
    p_glEnableVertexAttribArray(0); p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void *)0);
    p_glGenTextures(1, &ov_tex);
    return 1;
}
static void ov_begin(int win_w, int win_h) {
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    p_glViewport(0, 0, win_w, win_h);
    p_glDisable(GL_DEPTH_TEST); p_glDisable(GL_STENCIL_TEST); p_glDisable(GL_SCISSOR_TEST); p_glDisable(GL_CULL_FACE);
    p_glEnable(GL_BLEND);
    p_glBindVertexArray(ov_vao);
}
static void present_scanlines(int x, int y, int w, int h, int lines, int win_w, int win_h) {
    int vp[4] = { 0, 0, win_w, win_h };
    if (lines <= 0 || h < lines * 2 || !ov_init()) return;        /* under 2 window pixels a line: nothing to show */
    static const float k[4] = { 0, 0.25f, 0.45f, 0.65f };
    int lvl = dc_pvr_gl_scanlines < 0 ? 0 : dc_pvr_gl_scanlines > 3 ? 3 : dc_pvr_gl_scanlines;
    ov_begin(vp[2], vp[3]);
    p_glBlendFunc(GL_DST_COLOR, GL_ZERO);                          /* multiply */
    p_glUseProgram(sl_prog);
    p_glUniform4f(sl_rect, x * 2.f / vp[2] - 1, y * 2.f / vp[3] - 1, (x + w) * 2.f / vp[2] - 1, (y + h) * 2.f / vp[3] - 1);
    p_glUniform1f(sl_y0, (float)y); p_glUniform1f(sl_h, (float)h); p_glUniform1f(sl_lines, (float)lines); p_glUniform1f(sl_k, k[lvl]);
    p_glDrawArrays(GL_TRIANGLES, 0, 6);
    p_glDisable(GL_BLEND); sv_cur = NULL; p_glBindVertexArray(vao);
}
void dc_pvr_gl_overlay(const uint8_t *rgba, int w, int h, int x, int y, int dw, int dh, int win_w, int win_h) {
    if (!ready || !rgba || w <= 0 || h <= 0 || !ov_init()) return;
    ov_begin(win_w, win_h);
    p_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    p_glActiveTexture(GL_TEXTURE0); p_glBindTexture(GL_TEXTURE_2D, ov_tex);
    /* the same picture as last time (Show FPS changes twice a second): no upload. Uploading it every
     * frame cost the Switch a new GPU buffer each time, ~2% of a race frame in the profile */
    static uint8_t *ov_last; static int ov_w, ov_h;
    size_t bytes = (size_t)w * h * 4;
    if (!ov_last || ov_w != w || ov_h != h || memcmp(ov_last, rgba, bytes)) {
        p_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        uint8_t *n = realloc(ov_last, bytes);
        if (n) { ov_last = n; memcpy(ov_last, rgba, bytes); ov_w = w; ov_h = h; } else { free(ov_last); ov_last = NULL; }
    }
    p_glUseProgram(ov_prog);
    p_glUniform1i(ov_texloc, 0);
    /* x, y: top-left in window pixels (top row 0) */
    float x0 = x * 2.f / win_w - 1, x1 = (x + dw) * 2.f / win_w - 1, y1 = 1 - y * 2.f / win_h, y0 = 1 - (y + dh) * 2.f / win_h;
    p_glUniform4f(ov_rect, x0, y0, x1, y1);
    p_glDrawArrays(GL_TRIANGLES, 0, 6);
    p_glDisable(GL_BLEND); sv_cur = NULL; p_glBindVertexArray(vao);
}

/* show an RGB8 image (top row first) instead of a rendered frame: screens the CPU drew directly
 * into the framebuffer (dc_pvr_display) */
void dc_pvr_gl_load_rgb(const uint8_t *rgb, int w, int h) {
    if (!ready || w <= 0 || h <= 0) return;
    if (!fbo) ensure_fbo(w, h);
    disp_aspect = 4.f / 3.f;                               /* a CPU-drawn screen: always 4:3 */
    pic_offy = 0; pic_y0 = pic_y1 = 0; zoom_x0 = zoom_x1 = zoom_y0 = zoom_y1 = 0;
    uint8_t *tmp = malloc((size_t)w * h * 3);
    for (int y = 0; y < h; y++) memcpy(tmp + (size_t)(h - 1 - y) * w * 3, rgb + (size_t)y * w * 3, (size_t)w * 3);
    if (w == fb_w && h == fb_h) {                          /* same size: straight into the picture */
        p_glBindTexture(GL_TEXTURE_2D, color_tex);
        p_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        p_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, tmp);
    } else {                                               /* scaled into it, keeping the render size */
        static GLuint up_tex, up_fbo; static int up_w, up_h;
        if (!up_tex) { p_glGenTextures(1, &up_tex); p_glGenFramebuffers(1, &up_fbo); }
        p_glBindTexture(GL_TEXTURE_2D, up_tex);
        p_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        if (w != up_w || h != up_h) {
            p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, tmp);
            p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            p_glBindFramebuffer(GL_FRAMEBUFFER, up_fbo);
            p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, up_tex, 0);
            up_w = w; up_h = h;
        } else p_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, tmp);
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, up_fbo);
        p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
        p_glDisable(GL_SCISSOR_TEST);
        p_glBlitFramebuffer(0, 0, w, h, 0, 0, fb_w, fb_h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    free(tmp);
}

/* the texture the game asked for, from the capture instead of the software renderer: the 4:3 part,
 * scaled down on the GPU to the render's size (FB_X/Y_CLIP), read back and stored in the target's
 * format. A software render of the pass took 50-200 ms (crossfades ran at a few frames a second). */
static void capture_readback(int c) {
    static GLuint small_fbo, small_tex; static int sw, sh;
    static uint8_t *px, *rows; static size_t cap_px;
    int W, H; dc_pvr_target_size(&W, &H);
    if (cap[c].ww <= 0 || cap[c].rh <= 0) return;
    if (!small_fbo || sw != W || sh != H) {
        if (small_fbo) { p_glDeleteFramebuffers(1, &small_fbo); p_glDeleteTextures(1, &small_tex); }
        p_glGenTextures(1, &small_tex); p_glBindTexture(GL_TEXTURE_2D, small_tex);
        p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        p_glGenFramebuffers(1, &small_fbo); p_glBindFramebuffer(GL_FRAMEBUFFER, small_fbo);
        p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, small_tex, 0);
        sw = W; sh = H;
    }
    if ((size_t)W * H * 4 > cap_px) { cap_px = (size_t)W * H * 4; px = realloc(px, cap_px); rows = realloc(rows, cap_px); }
    /* the capture is cap.ww native pixels wide (the 4:3 picture in the middle) and cap.rh high */
    float fx = (float)cap[c].w / cap[c].ww, fy = (float)cap[c].h / cap[c].rh;
    int x0 = (int)((cap[c].ww - cap[c].rw) * 0.5f * fx + 0.5f), x1 = x0 + (int)(W * fx + 0.5f);
    int y1 = cap[c].h, y0 = y1 - (int)(H * fy + 0.5f);
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, cap[c].fbo); p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, small_fbo);
    p_glDisable(GL_SCISSOR_TEST);
    p_glBlitFramebuffer(x0, y0, x1, y1, 0, 0, W, H, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    p_glBindFramebuffer(GL_FRAMEBUFFER, small_fbo);
    p_glPixelStorei(GL_PACK_ALIGNMENT, 1);
    p_glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    for (int y = 0; y < H; y++) memcpy(rows + (size_t)y * W * 4, px + (size_t)(H - 1 - y) * W * 4, (size_t)W * 4);   /* GL rows are bottom-up */
    dc_pvr_sw_store(rows, W, H);
}

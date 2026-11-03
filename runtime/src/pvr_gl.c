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

/* ------------------------------------------------------------------ minimal GL binding */
typedef unsigned int GLenum, GLuint, GLbitfield; typedef int GLint, GLsizei; typedef float GLfloat;
typedef unsigned char GLboolean, GLubyte; typedef char GLchar; typedef ptrdiff_t GLsizeiptr; typedef double GLdouble;
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

#define GLFUNCS \
    F(void, glGenFramebuffers, (GLsizei, GLuint *)) F(void, glBindFramebuffer, (GLenum, GLuint)) \
    F(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    F(void, glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint)) F(void, glGenRenderbuffers, (GLsizei, GLuint *)) \
    F(void, glBindRenderbuffer, (GLenum, GLuint)) F(void, glRenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei)) \
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
    F(void, glPixelStorei, (GLenum, GLint)) F(GLenum, glGetError, (void))
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
static const char *vs_src =
"#version 330 core\n"
"layout(location=0) in vec3 a_pos;\n"            /* PVR x, y, 1/w */
"layout(location=1) in vec4 a_uv;\n"             /* u v (set 0), u v (set 1) */
"layout(location=2) in vec4 a_c;  layout(location=3) in vec4 a_o;\n"
"layout(location=4) in vec4 a_c1; layout(location=5) in vec4 a_o1;\n"
"uniform vec2 u_scale; uniform float u_offx; uniform int u_set; uniform int u_quad;\n"
"noperspective out vec2 v_uvz; noperspective out vec4 v_cz; noperspective out vec4 v_oz; noperspective out float v_invw;\n"
"flat out vec4 f_c; flat out vec4 f_o;\n"
"void main() {\n"
"  if (u_quad != 0) { gl_Position = vec4(a_pos.xy, 0.0, 1.0); v_invw = 1.0; v_uvz = vec2(0.0); v_cz = vec4(0.0); v_oz = vec4(0.0); f_c = vec4(0.0); f_o = vec4(0.0); return; }\n"
"  float z = max(a_pos.z, 1e-30);\n"
"  vec2 ndc = vec2((a_pos.x + u_offx) * u_scale.x - 1.0, 1.0 - a_pos.y * u_scale.y);\n"
"  gl_Position = vec4(ndc, 0.0, 1.0);\n"
"  vec2 uv = u_set != 0 ? a_uv.zw : a_uv.xy;\n"
"  vec4 c = u_set != 0 ? a_c1 : a_c, o = u_set != 0 ? a_o1 : a_o;\n"
"  v_uvz = uv * z; v_cz = c * z; v_oz = o * z; v_invw = z; f_c = c; f_o = o;\n"
"}\n";

static const char *fs_src =
"#version 330 core\n"
"noperspective in vec2 v_uvz; noperspective in vec4 v_cz; noperspective in vec4 v_oz; noperspective in float v_invw;\n"
"flat in vec4 f_c; flat in vec4 f_o;\n"
"uniform sampler2D u_tex;\n"
"uniform int u_quad; uniform vec4 u_quad_col;\n"
"uniform int u_textured, u_shade, u_use_alpha, u_ign_ta, u_offset, u_gouraud, u_fog, u_pt;\n"
"uniform float u_pt_ref; uniform vec3 u_fog_ram, u_fog_vert; uniform float u_fog_den; uniform vec2 u_fog_tab[128];\n"
"uniform int u_hud; uniform vec4 u_uvbox;\n"
"out vec4 o_col;\n"
/* HUD art (low resolution, magnified several times): Catmull-Rom bicubic instead of bilinear keeps
 * digits and outlines crisp; 9 bilinear taps, clamped (the kernel overshoots at hard edges) */
"vec4 tex_cr(vec2 uv, bool edge) {\n"
"  vec2 ts = vec2(textureSize(u_tex, 0)), p = uv * ts - 0.5, i = floor(p), f = p - i;\n"
"  vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);\n"
"  vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);\n"
"  vec2 w12 = w1 + w2, o12 = w2 / w12;\n"
"  vec2 t0 = (i - 0.5) / ts, t3 = (i + 2.5) / ts, t12 = (i + 0.5 + o12) / ts;\n"
"  if (edge) { vec2 lo = u_uvbox.xy + 0.5 / ts, hi = max(u_uvbox.zw - 0.5 / ts, lo); t0 = clamp(t0, lo, hi); t3 = clamp(t3, lo, hi); t12 = clamp(t12, lo, hi); }\n"
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
"float fog_alpha(float invw) {\n"
"  float fw = clamp(u_fog_den * invw, 1.0, 255.999985);\n"
"  uint b = floatBitsToUint(fw); uint e = (b >> 23) & 255u; uint m = b & 0x7FFFFFu;\n"
"  int idx = int((((e + 1u) & 7u) << 4) | ((m >> 19) & 15u));\n"
"  float bl = float((m >> 11) & 255u);\n"
"  vec2 ent = u_fog_tab[idx];\n"
"  return floor((ent.x * bl + ent.y * (255.0 - bl)) / 256.0) / 255.0;\n"
"}\n"
"void main() {\n"
"  if (u_quad != 0) { o_col = u_quad_col; gl_FragDepth = 0.0; return; }\n"
"  float iw = max(v_invw, 1e-30);\n"
"  vec2 uv = v_uvz / iw;\n"
"  vec4 col = u_gouraud != 0 ? v_cz / iw : f_c, off = u_gouraud != 0 ? v_oz / iw : f_o;\n"
"  if (u_use_alpha == 0) col.a = 1.0;\n"
"  vec4 px;\n"
"  if (u_textured != 0) {\n"
"    if ((u_hud & 2) != 0) { vec2 h = 0.5 / vec2(textureSize(u_tex, 0)), lo = u_uvbox.xy + h, hi = max(u_uvbox.zw - h, lo); uv = clamp(uv, lo, hi); }\n"
"    vec4 t = (u_hud & 1) != 0 ? tex_cr(uv, (u_hud & 2) != 0) : texture(u_tex, uv);\n"
"    if (u_ign_ta != 0) t.a = 1.0;\n"
"    if (u_shade == 0) px = t;\n"
"    else if (u_shade == 1) px = vec4(t.rgb * col.rgb, t.a);\n"
"    else if (u_shade == 2) px = vec4(t.rgb * t.a + col.rgb * (1.0 - t.a), col.a);\n"
"    else px = t * col;\n"
"  } else px = col;\n"
"  if (u_offset != 0) px.rgb = min(px.rgb + off.rgb, vec3(1.0));\n"
"  if (u_fog != 2) {\n"
"    float f; vec3 fc;\n"
"    if (u_fog == 1) { fc = u_fog_vert; f = off.a; } else { fc = u_fog_ram; f = fog_alpha(iw); }\n"
"    if (u_fog == 3) px = vec4(fc, f); else px.rgb = mix(px.rgb, fc, f);\n"
"  }\n"
"  if (u_pt != 0 && px.a * 255.0 + 0.5 < u_pt_ref * 255.0) discard;\n"   /* in 8-bit steps like the PVR: colours divided by the interpolated 1/w can land a hair below 1.0 (NVIDIA), which dropped whole letters of Rush's text */
"  o_col = px;\n"
"  gl_FragDepth = clamp((log2(iw) + 64.0) / 128.0, 0.0, 1.0);\n"
"}\n";

static float depth_of(float invw) { float d = (log2f(fmaxf(invw, 1e-30f)) + 64.f) / 128.f; return d < 0 ? 0 : d > 1 ? 1 : d; }

/* ------------------------------------------------------------------ state */
typedef struct { float x, y, z, u, v, u1, v1, c[4], o[4], c1[4], o1[4]; } gv;
static GLuint prog, vao, vbo, fbo, color_tex, depth_rb;
static int fb_w, fb_h, ready;
static struct { GLint scale, offx, set, quad, quad_col, tex, textured, shade, use_alpha, ign_ta, offset, gouraud, fog, pt, pt_ref,
                fog_ram, fog_vert, fog_den, fog_tab, hud, uvbox; } U;
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
float dc_pvr_gl_anisotropy;            /* enhancement: anisotropic filtering (clamped to the driver's max) */
static float max_aniso;
static void bars_init(void);

int dc_pvr_gl_init(void *(*getproc)(const char *)) {
    int missing = 0;
#define F(ret, name, args) p_##name = (ret (*) args)getproc(#name); if (!p_##name) { fprintf(stderr, "pvr_gl: missing %s\n", #name); missing++; }
    GLFUNCS
#undef F
    if (missing) return -1;
    gl_debug_init(getproc);
    prog = p_glCreateProgram();
    p_glAttachShader(prog, compile(GL_VERTEX_SHADER, vs_src));
    p_glAttachShader(prog, compile(GL_FRAGMENT_SHADER, fs_src));
    p_glLinkProgram(prog);
    GLint ok = 0; p_glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) { char log[2048]; p_glGetProgramInfoLog(prog, sizeof log, NULL, log); fprintf(stderr, "pvr_gl: link: %s\n", log); return -1; }
    bars_init();
#define UL(f, n) U.f = p_glGetUniformLocation(prog, n)
    UL(scale, "u_scale"); UL(offx, "u_offx"); UL(set, "u_set"); UL(quad, "u_quad"); UL(quad_col, "u_quad_col"); UL(tex, "u_tex");
    UL(textured, "u_textured"); UL(shade, "u_shade"); UL(use_alpha, "u_use_alpha"); UL(ign_ta, "u_ign_ta"); UL(offset, "u_offset");
    UL(gouraud, "u_gouraud"); UL(fog, "u_fog"); UL(pt, "u_pt"); UL(pt_ref, "u_pt_ref"); UL(hud, "u_hud"); UL(uvbox, "u_uvbox"); UL(fog_ram, "u_fog_ram");
    UL(fog_vert, "u_fog_vert"); UL(fog_den, "u_fog_den"); UL(fog_tab, "u_fog_tab");
#undef UL
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
    ready = 1;
    return 0;
}

int dc_pvr_gl_widescreen;
float dc_pvr_gl_aspect;                /* window width / height for the widescreen target (0 = 16:9) */
float dc_pvr_gl_hud_z;
float dc_pvr_gl_hud_zmax;
int dc_pvr_gl_clip_2d;
static float disp_aspect = 4.f / 3.f;
static float last_ww, last_rw, last_rh;   /* the last render's target width and picture size, console units */

/* the last picture rendered into each framebuffer address (double buffering: two), so a later render
 * that only draws a band of lines into one of them keeps what is there outside it, as the console
 * does (Looney Tunes Space Race's loading bar over the picture before a race) */
#define NKEEP 4
static struct { uint32_t tgt; int w, h, fbw, fbh; float ww; GLuint fbo, tex; unsigned frame; } keep[NKEEP];
int dc_pvr_gl_keep_outside = 1;
static void ensure_fbo(int w, int h) {
    if (fbo && w == fb_w && h == fb_h) return;
    if (fbo) { p_glDeleteFramebuffers(1, &fbo); p_glDeleteTextures(1, &color_tex); p_glDeleteRenderbuffers(1, &depth_rb); }
    p_glGenTextures(1, &color_tex); p_glBindTexture(GL_TEXTURE_2D, color_tex);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    p_glGenRenderbuffers(1, &depth_rb); p_glBindRenderbuffer(GL_RENDERBUFFER, depth_rb);
    p_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    p_glGenFramebuffers(1, &fbo); p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_tex, 0);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth_rb);
    if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) fprintf(stderr, "pvr_gl: framebuffer incomplete\n");
    fb_w = w; fb_h = h;
}

static GLuint texture_for(uint32_t tcw, uint32_t tsp) {
    pvr_texinfo ti;
    pvr_get_texture(tcw, tsp, &ti);
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
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ti.w, ti.h, 0, GL_BGRA, GL_UNSIGNED_BYTE, ti.px);
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
static struct { GLuint fbo, tex, depth; int w, h; uint32_t addr; float ww, rw, rh; unsigned long frame; } cap[NCAP + 1];   /* [NCAP]: the previous contents of the one being drawn */
static int capturing;                   /* drawing a render-to-texture pass */
static int cap_cur = -1;                /* while capturing: the slot being drawn */
static int cap_uv = -1;                 /* >= 0: the polygon being pushed shows capture cap_uv */
static unsigned char *poly_cap;         /* per polygon: capture + 1, or 0 */
static void push(const pvr_vtx *v) {
    if (nverts == cap_verts) { cap_verts = cap_verts ? cap_verts * 2 : 8192; verts = realloc(verts, cap_verts * sizeof *verts); }
    gv *g = &verts[nverts++];
    g->x = v->x + push_dx;
    if (push_wide > 0) { if (v->x <= 1.f) g->x -= push_wide; else if (v->x >= push_rw - 1.f) g->x += push_wide; } g->y = v->y; g->z = v->z; g->u = v->u; g->v = v->v; g->u1 = v->u1; g->v1 = v->v1;
    memcpy(g->c, v->c, 16); memcpy(g->o, v->o, 16); memcpy(g->c1, v->c1, 16); memcpy(g->o1, v->o1, 16);
    if (cap_uv >= 0) {                  /* the capture spans [-(ww - rw) / 2, rw + (ww - rw) / 2] x [0, rh], rows up */
        g->u = (g->x + (cap[cap_uv].ww - cap[cap_uv].rw) * 0.5f) / cap[cap_uv].ww;
        g->v = 1.f - g->y / cap[cap_uv].rh;
    }
}
/* triangle with the PVR's culling (sign of the screen area; odd strip triangles are flipped) */
static void tri(const pvr_poly *p, const pvr_vtx *a, const pvr_vtx *b, const pvr_vtx *c, int flip, float cull_small) {
    float area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
    if (area == 0) return;
    int cull = (p->isp >> 27) & 3;
    float sarea = flip ? -area : area;
    if (cull == 1 && fabsf(area) < cull_small) return;
    if (cull == 2 && sarea < 0) return;
    if (cull == 3 && sarea > 0) return;
    push(a); push(b); push(c);
}

static unsigned char *poly_hud; static const pvr_poly *poly_base;
static void set_poly_state(const pvr_poly *p, int set) {
    uint32_t tsp = set ? p->tsp1 : p->tsp, tcw = set ? p->tcw1 : p->tcw;
    int tex = (p->isp >> 25) & 1;
    p_glUniform1i(U.set, set);
    p_glUniform1i(U.textured, tex); p_glUniform1i(U.offset, (p->isp >> 24) & 1); p_glUniform1i(U.gouraud, (p->isp >> 23) & 1);
    p_glUniform1i(U.shade, (tsp >> 6) & 3); p_glUniform1i(U.use_alpha, (tsp >> 20) & 1); p_glUniform1i(U.ign_ta, (tsp >> 19) & 1);
    p_glUniform1i(U.fog, (tsp >> 22) & 3); p_glUniform1i(U.pt, p->list == 4);
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
        }
    }
    static const GLenum depth_fn[8] = { GL_NEVER, 0x0201, GL_EQUAL, 0x0203, 0x0204, 0x0205, GL_GEQUAL, GL_ALWAYS };
    int zmode = (p->isp >> 29) & 7;
    if (p->list == 4 && zmode == 0) zmode = 6;
    p_glDepthFunc(depth_fn[zmode]);
    p_glDepthMask(p->list != 2 && !((p->isp >> 26) & 1));
}
static GLenum blend_src(int i) { static const GLenum t[8] = { GL_ZERO, GL_ONE, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA }; return t[i & 7]; }
static GLenum blend_dst(int i) { static const GLenum t[8] = { GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA }; return t[i & 7]; }

static int quad_first;                 /* full-screen quad in the vertex buffer */
static void draw_quad(float r, float g, float b, float a) {
    p_glUniform1i(U.quad, 1); p_glUniform4f(U.quad_col, r, g, b, a);
    p_glDrawArrays(GL_TRIANGLES, quad_first, 6);
    p_glUniform1i(U.quad, 0);
}

typedef struct { int idx; float key; } sort_e;
static int cmp_sort(const void *a, const void *b) {
    /* equal depth keeps submission order: qsort is not stable everywhere (MSVC's is not), and
     * text drawn at one depth then lost letters at random */
    const sort_e *p = a, *q = b;
    if (p->key != q->key) return p->key < q->key ? -1 : 1;
    return p->idx < q->idx ? -1 : p->idx > q->idx;
}

/* Draw batching: consecutive polygons with the same render state go out in one call (menus can
 * have thousands of small polygons, and per-call overhead dominates). Draw order is unchanged. */
typedef struct { uint32_t isp, tsp, tcw; int list, set, mark, blend, uclip, ux0, uy0, ux1, uy1, hud; float ub[4]; int cap; } bkey;
/* HUD polygons (flat, at or above dc_pvr_gl_hud_z) get the sharper texture filter */
int dc_pvr_gl_hud_sharp = 1;
int dc_pvr_gl_hud_move = 1;
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
    int y0 = fb_h - p->uy1 * sc_scale, y1 = fb_h - p->uy0 * sc_scale;     /* GL rows go up */
    if (x0 < sc_x) x0 = sc_x; if (x1 > sc_x + sc_w) x1 = sc_x + sc_w;
    if (y0 < sc_y) y0 = sc_y; if (y1 > sc_y + sc_h) y1 = sc_y + sc_h;
    scissor(x0, y0, x1 - x0, y1 - y0);
}
static struct { bkey key; int valid; GLint *first; GLsizei *count; int n, cap; } B;
static void batch_flush(void) {
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
               ((p->tsp >> 13) & 3) ? is_hud(p) : (is_hud(p) & 2), {0, 0, 0, 0}, poly_cap && poly_base ? poly_cap[p - poly_base] : 0 }; /* bicubic for bilinear-filtered HUD textures only */
    if (k.hud & 2) memcpy(k.ub, poly_uvbox[p - poly_base], sizeof k.ub);
    if (!B.valid || memcmp(&k, &B.key, sizeof k)) {
        batch_flush();
        if (mark >= 0) p_glStencilFunc(GL_ALWAYS, mark, 0xFF);
        set_poly_state(p, set);
        scissor_for(p);
        p_glUniform1i(U.hud, k.hud);
        if (k.hud & 2) p_glUniform4f(U.uvbox, k.ub[0], k.ub[1], k.ub[2], k.ub[3]);
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
static GLint bar_x0, bar_x1, bar_tex;
static const char *bar_vs =
"#version 330 core\n"
"layout(location=0) in vec3 a_pos;\n"
"uniform float u_x0, u_x1; out float v_y;\n"
"void main() { float t = (a_pos.x + 1.0) * 0.5; gl_Position = vec4(mix(u_x0, u_x1, t), a_pos.y, 0.0, 1.0); v_y = (a_pos.y + 1.0) * 0.5; }\n";
static const char *bar_fs =
"#version 330 core\n"
"in float v_y; uniform sampler2D u_tex; out vec4 o_col;\n"
"void main() {\n"

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
static void draw_bars(int px0, int px1, int y0, int y1, int left, int right) {
    if (!bar_prog || px0 <= 0 || y1 <= y0) return;
    if (px1 >= fb_w || px1 - px0 < 8) return;
    int strip = (px1 - px0) / 40 > 2 ? (px1 - px0) / 40 : 2;
    for (int i = 0; i < 2; i++) {
        int sx = i ? px1 - 1 - strip : px0 + 1;         /* inside the picture, past its (often dark) edge pixel */
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo); p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, edge_fbo[i]);
        p_glBlitFramebuffer(sx, y0, sx + strip, y1, 0, 0, EDGE_W, EDGE_H, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        p_glBindTexture(GL_TEXTURE_2D, edge_tex[i]); p_glGenerateMipmap(GL_TEXTURE_2D);
    }
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    p_glViewport(0, 0, fb_w, fb_h);
    p_glEnable(GL_SCISSOR_TEST); p_glScissor(0, y0, fb_w, y1 - y0);
    p_glDisable(GL_DEPTH_TEST); p_glDisable(GL_BLEND); p_glDisable(GL_STENCIL_TEST);
    p_glUseProgram(bar_prog);
    p_glUniform1i(bar_tex, 0); p_glActiveTexture(GL_TEXTURE0);
    p_glBindVertexArray(vao);
    for (int i = 0; i < 2; i++) {
        if (!(i ? right : left)) continue;
        float a = i ? (float)(px1 - 1) : 0.f, b = i ? (float)fb_w : (float)(px0 + 1);   /* cover the edge pixel */
        p_glUniform1f(bar_x0, a / fb_w * 2.f - 1.f); p_glUniform1f(bar_x1, b / fb_w * 2.f - 1.f);
        p_glBindTexture(GL_TEXTURE_2D, edge_tex[i]);
        p_glDrawArrays(GL_TRIANGLES, quad_first, 6);
    }
    p_glUseProgram(prog);
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
    p_glUseProgram(prog);
    p_glUniform2f(U.scale, 2.f / 640.f, 2.f / 480.f); p_glUniform1f(U.offx, 0);
    p_glUniform1i(U.tex, 0); p_glUniform1i(U.quad, 0); p_glUniform1i(U.set, 0);
    p_glUniform1i(U.textured, 1); p_glUniform1i(U.shade, 0); p_glUniform1i(U.use_alpha, 1); p_glUniform1i(U.ign_ta, 0);
    p_glUniform1i(U.offset, 0); p_glUniform1i(U.gouraud, 1); p_glUniform1i(U.fog, 2); p_glUniform1i(U.pt, 0);
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
        fprintf(dbg_log, "state: texture %d (made %u) active unit %X program %d (ours %u) sampler %d\n", v[0], t, v[1], v[2], prog, v[3]);
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
            if (fabsf(v[k].z - v[0].z) > 0.02f * v[0].z) flat = 0;
            if (v[k].z < dc_pvr_gl_hud_z || (v[k].z >= hud_group_zmax && i >= hud_group_imin) || (dc_pvr_gl_hud_zmax > 0 && v[k].z >= dc_pvr_gl_hud_zmax)) near = 0;
            if (v[k].x < x0) x0 = v[k].x; if (v[k].x > x1) x1 = v[k].x;
            if (v[k].y < y0) y0 = v[k].y; if (v[k].y > y1) y1 = v[k].y;
        }
        if (!near) continue;
        /* small 3D HUD models (Rush's spinning coins) count; up to a fifth of the width: Space Race's
         * commentator frame grows past 15% as it slides out, and must keep moving with the portrait */
        if (!flat && x1 - x0 > RW * 0.2f) continue;
        if (x1 - x0 > RW * 0.9f) continue;                    /* full-width bars and fades stay */
        if (n == cap) { cap = cap ? cap * 2 : 1024; b = realloc(b, (size_t)cap * sizeof *b); }
        b[n].x0 = x0; b[n].y0 = y0; b[n].x1 = x1; b[n].y1 = y1; b[n].poly = i; b[n].parent = n; n++;
    }
    /* in the console's pixels: anything within 6 joins; in the same row (vertical overlap) within
     * 32, so a menu line's labels and values move or stay together (Rush's "PLAYERS 1 2 3 4") */
    const float gap = 6.f, row_gap = 32.f;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++) {
            int same_row = b[i].y0 < b[j].y1 && b[j].y0 < b[i].y1;
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
    for (int i = 0; i < n; i++) {
        int r = hud_find(b, i);
        float cx = (gx0[r] + gx1[r]) * 0.5f;
        if (gx1[r] - gx0[r] > RW * 0.6f) continue;           /* spans most of the screen: centred layout */
        side[b[i].poly] = cx < RW / 3.f ? -1 : cx > RW * 2.f / 3.f ? 1 : 0;
    }
    free(gx0);
}

/* widescreen: a cut-scene's letterbox bar (an untextured, flat, opaque, near-black polygon across the
 * whole 4:3 width, along the top or bottom edge, up to a third of the height). A frame with one is
 * shown at 4:3 with black sides: the game frames such scenes (and culls them) for 4:3, and its bars
 * stop at the 4:3 edges (Looney Tunes Space Race's intro). */
/* could be a screen shade: untextured, or a texture modulated by black vertex colours (Re-Volt's pause
 * shade is a textured quad drawn black at half alpha) */
static int shade_poly(const pvr_poly *p, const pvr_vtx *v) {
    if (!((p->isp >> 25) & 1)) return 1;
    if (!(((p->tsp >> 6) & 3) & 1)) return 0;                /* modulate (or modulate alpha) */
    for (int k = 0; k < p->n; k++) if (v[k].c[0] > .05f || v[k].c[1] > .05f || v[k].c[2] > .05f) return 0;
    return 1;
}

static int letterbox_bar(const pvr_poly *p, const pvr_vtx *v, int RW, int H) {
    if (p->n < 3 || p->list != 0 || ((p->isp >> 25) & 1)) return 0;
    float x0 = v[0].x, x1 = v[0].x, y0 = v[0].y, y1 = v[0].y;
    for (int k = 0; k < p->n; k++) {
        if (v[k].z != v[0].z || v[k].c[0] > 0.06f || v[k].c[1] > 0.06f || v[k].c[2] > 0.06f) return 0;
        if (v[k].x < x0) x0 = v[k].x; if (v[k].x > x1) x1 = v[k].x;
        if (v[k].y < y0) y0 = v[k].y; if (v[k].y > y1) y1 = v[k].y;
    }
    if (x0 > 1.f || x1 < RW - 1.f || y1 - y0 < 4.f || y1 - y0 > H / 3.f) return 0;
    return y0 <= 1.f || y1 >= H - 1.f;
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

void dc_pvr_gl_render(int scale) {
    if (!ready) return;
    if (scale < 1) scale = 1;
    frame_no++;
    pvr_frame f;
    pvr_get_frame(&f);
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
    {   static int cap_hud;
        if (f.npolys > cap_hud) { cap_hud = f.npolys + 1024; poly_hud = realloc(poly_hud, (size_t)cap_hud); poly_uvbox = realloc(poly_uvbox, (size_t)cap_hud * sizeof *poly_uvbox); poly_cap = realloc(poly_cap, (size_t)cap_hud); }
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
            if (v[k].z != v[0].z) flat = 0;
        }
        if (flat && x0 <= 1.f && x1 >= RW - 1.f && y0 <= 1.f && y1 >= RH - 1.f && cap[c].rw == (float)RW && cap[c].rh >= (float)RH)
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
    ensure_fbo((int)(WW / (float)RW * (float)(f.width * scale) + 0.5f), f.height * scale);
    gl_selftest();
    disp_aspect = aspect;
    last_ww = WW;

    /* geometry: every polygon's triangles, then volume triangles, then a full-screen quad */
    nverts = 0;
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
        if (n3d >= 50 && wide3d && !split) hud_dx = (WW - (float)RW) * 0.5f;
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
            if (p->n != 4 || !shade_poly(p, v) || v[0].z < dc_pvr_gl_hud_z || v[0].z >= ovl_z) continue;
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
    for (int i = 0; i < f.npolys; i++) {
        const pvr_poly *p = &f.polys[i];
        const pvr_vtx *v = f.verts + p->first;
        push_dx = 0;
        if (hud_dx > 0 && hud_side) {
            push_dx = hud_side[i] * hud_dx;
            if (ovl_z < 1e29f && v[0].z >= ovl_z && i >= ovl_i) {   /* pause shade and menu */
                int flat = 1; for (int k = 1; k < p->n; k++) if (v[k].z != v[0].z) flat = 0;
                if (flat) { push_dx = 0; if (v[0].z == ovl_z) { push_wide = WW; push_rw = (float)RW; } }
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
        if (poly_cap[i]) { push_dx = 0; push_wide = WW; push_rw = (float)RW; cap_uv = poly_cap[i] - 1; }
        spans[i].first = nverts;
        if (p->sprite) { tri(p, &v[0], &v[1], &v[2], 0, cull_small); tri(p, &v[0], &v[2], &v[3], 0, cull_small); }
        else for (int k = 0; k + 2 < p->n; k++) tri(p, &v[k], &v[k + 1], &v[k + 2], k & 1, cull_small);
        spans[i].count = nverts - spans[i].first;
        push_wide = 0; cap_uv = -1;
        if ((p->isp >> 25) & 1) for (int k = 0; k < p->n; k++) { if (v[k].x < cx0) cx0 = v[k].x; if (v[k].x > cx1) cx1 = v[k].x; }
    }
    push_dx = 0;
    int mod_first = nverts;
    for (int i = 0; i < f.nmods; i++) {
        pvr_vtx t[3]; memset(t, 0, sizeof t);
        for (int k = 0; k < 3; k++) { t[k].x = f.mods[i].x[k]; t[k].y = f.mods[i].y[k]; t[k].z = f.mods[i].z[k]; }
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

    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    p_glViewport(0, 0, fb_w, fb_h);
    p_glUseProgram(prog);
    p_glUniform2f(U.scale, 2.f / WW, 2.f / RH);
    p_glUniform1f(U.offx, (WW - (float)RW) * 0.5f);
    p_glUniform1i(U.tex, 0); p_glUniform1i(U.quad, 0); p_glUniform1i(U.hud, 0);
    p_glUniform1f(U.pt_ref, (dc_pvr_reg(0x11C) & 255) / 255.f);
    { uint32_t fr = dc_pvr_reg(0x0B0), fv = dc_pvr_reg(0x0AC), den = dc_pvr_reg(0x0B4);
      p_glUniform3f(U.fog_ram, ((fr >> 16) & 255) / 255.f, ((fr >> 8) & 255) / 255.f, (fr & 255) / 255.f);
      p_glUniform3f(U.fog_vert, ((fv >> 16) & 255) / 255.f, ((fv >> 8) & 255) / 255.f, (fv & 255) / 255.f);
      p_glUniform1f(U.fog_den, ((den >> 8) & 255) / 128.f * ldexpf(1.f, (int8_t)(den & 255)));
      float tab[256];
      for (int i = 0; i < 128; i++) { uint32_t e = dc_pvr_reg(0x200 + i * 4); tab[i * 2] = (float)(e & 255); tab[i * 2 + 1] = (float)((e >> 8) & 255); }
      p_glUniform2fv(U.fog_tab, 128, tab); }

    p_glDisable(GL_CULL_FACE);
    p_glColorMask(1, 1, 1, 1); p_glDepthMask(1); p_glStencilMask(0xFF);
    /* outside the clip rectangle: black (not for a band, see above); inside: background plane, and all
     * drawing scissored to it */
    {
        if (restore >= 0 && keep[restore].fbw == fb_w && keep[restore].fbh == fb_h) {   /* see keep */
            p_glDisable(GL_SCISSOR_TEST);
            p_glBindFramebuffer(GL_READ_FRAMEBUFFER, keep[restore].fbo); p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
            p_glBlitFramebuffer(0, 0, fb_w, fb_h, 0, 0, fb_w, fb_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
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
          zoom_y0 = fb_h - (int)floorf(lb_bot * (float)scale); zoom_y1 = fb_h - (int)ceilf(lb_top * (float)scale);
      }
      else if (dc_pvr_gl_widescreen && band) {             /* a quarter: its side of the screen, out to that edge */
        float o = (WW - (float)RW) * 0.5f, k = (float)fb_w / WW, kx = (float)RW / (float)f.width;
        if (f.clip_x0 > 0) sx0 = (int)floorf((o + (float)f.clip_x0 * kx) * k + 0.5f);
        if (f.clip_x1 + 1 < f.width) sx1 = (int)floorf((o + (float)(f.clip_x1 + 1) * kx) * k + 0.5f);
      }
      int sy0 = f.clip_y0 * scale, sy1 = (f.clip_y1 + 1) * scale;
      p_glEnable(GL_SCISSOR_TEST);
      sc_x = sx0; sc_y = fb_h - sy1; sc_w = sx1 - sx0; sc_h = sy1 - sy0; sc_set[2] = -1;
      sc_off = (WW - (float)RW) * 0.5f; sc_k = (float)fb_w / WW; sc_kx = (float)RW / (float)f.width; sc_scale = scale; sc_rw = f.width;
      scissor(sc_x, sc_y, sc_w, sc_h); }
    p_glClearColor(f.bg[0], f.bg[1], f.bg[2], 1.f);
    p_glClearDepth(depth_of(f.bgz)); p_glClearStencil(0);
    p_glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    p_glEnable(GL_DEPTH_TEST);
    p_glDisable(GL_BLEND);

    /* opaque, then punch-through; stencil bit 1 = surface modifier volumes act on */
    uint32_t shad = dc_pvr_reg(0x074);
    int cheap = (shad >> 8) & 1, has_mods = 0;
    for (int i = 0; i < f.nmods; i++) if (f.mods[i].list == 1) has_mods = 1;
    p_glEnable(GL_STENCIL_TEST);
    p_glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE); p_glStencilMask(2);
    for (int pass = 0; pass < 2; pass++) {
        int list = pass ? 4 : 0;
        for (int i = 0; i < f.npolys; i++) {
            const pvr_poly *p = &f.polys[i];
            if (p->list != list || !spans[i].count) continue;
            int mark = has_mods && (cheap ? p->shadow : p->twovol);
            batch_add(p, 0, mark ? 2 : 0, 0, spans[i].first, spans[i].count);
        }
    }
    batch_reset();

    /* modifier volumes */
    if (has_mods) {
        p_glColorMask(0, 0, 0, 0); p_glDepthMask(0);
        p_glUniform1i(U.textured, 0); p_glUniform1i(U.fog, 2); p_glUniform1i(U.pt, 0); p_glUniform1i(U.set, 0);
        int any = 0;
        for (int i = 0; i < f.nmods; i++) {
            const pvr_modtri *m = &f.mods[i];
            if (m->list != 1) continue;
            p_glEnable(GL_DEPTH_TEST); p_glDepthFunc(GL_GEQUAL);
            p_glStencilMask(1); p_glStencilFunc(GL_ALWAYS, 0, 0xFF); p_glStencilOp(GL_KEEP, GL_KEEP, GL_INVERT);
            p_glDrawArrays(GL_TRIANGLES, mod_first + i * 3, 3);
            if (m->inst == 1 || m->inst == 2) {            /* volume closed: parity -> bit 2, clear parity */
                p_glDisable(GL_DEPTH_TEST);
                p_glStencilMask(4); p_glStencilFunc(GL_EQUAL, 4 | (m->inst == 1 ? 1 : 0), 1); p_glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
                draw_quad(0, 0, 0, 0);
                p_glStencilMask(1); p_glStencilFunc(GL_ALWAYS, 0, 0xFF); p_glStencilOp(GL_ZERO, GL_ZERO, GL_ZERO);
                draw_quad(0, 0, 0, 0);
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
    for (int k = 0; k < nt; k++) {
        const pvr_poly *p = &f.polys[s[k].idx];
        batch_add(p, 0, -1, 1, spans[s[k].idx].first, spans[s[k].idx].count);
    }
    batch_reset();
    free(s);
    p_glDisable(GL_BLEND);
    p_glDisable(GL_SCISSOR_TEST);
    p_glDepthMask(1);
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
        int shows_cap = 0;                                  /* a full-width capture on screen (a held replay frame) */
        for (int i = 0; i < f.npolys && !shows_cap; i++) shows_cap = poly_cap[i] != 0;
        if (!shows_cap && (!(n3d >= 50 && wide3d) || backdrop_card(&f, RW))) {
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
    if (dc_pvr_gl_widescreen && dc_pvr_gl_side_fill && hud_dx == 0 && !lbox) {   /* not in races (their HUD is out there) */
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
        int y0 = fb_h - (f.clip_y1 + 1) * scale, y1 = fb_h - f.clip_y0 * scale, fill[2] = { 0, 0 };
        if (y1 > y0 && px0 > 4 && px1 < fb_w - 4) {
            static uint8_t *col; static int cap_col;
            if (y1 - y0 > cap_col) { cap_col = y1 - y0; col = realloc(col, (size_t)cap_col * 4); }
            p_glBindFramebuffer(GL_FRAMEBUFFER, fbo); p_glPixelStorei(GL_PACK_ALIGNMENT, 1);
            for (int sd = 0; sd < 2; sd++) {
                p_glReadPixels(sd ? px1 + 2 : px0 - 3, y0, 1, y1 - y0, GL_RGBA, GL_UNSIGNED_BYTE, col);
                int lit = 0;
                for (int y = 0; y < y1 - y0; y++) if (col[y * 4] + col[y * 4 + 1] + col[y * 4 + 2] > 24) lit++;
                fill[sd] = lit * 20 < y1 - y0;                       /* under 5% of the column drawn */
            }
        }
        if (fill[0] || fill[1]) draw_bars(px0, px1, y0, y1, fill[0], fill[1]);
    }
    pic_offy = ((float)(f.height - 1 - f.clip_y1) - (float)f.clip_y0) * 0.5f / (float)f.height;
    pic_y0 = fb_h - (f.clip_y1 + 1) * scale; pic_y1 = fb_h - f.clip_y0 * scale;
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
    GLuint sf = fbo, st = color_tex, sd = depth_rb; int sw = fb_w, sh = fb_h;
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
    fbo = cap[c].fbo; color_tex = cap[c].tex; depth_rb = cap[c].depth; fb_w = cap[c].w; fb_h = cap[c].h;
    capturing = 1; cap_cur = c;
    dc_pvr_gl_render(scale);
    capturing = 0; cap_cur = -1;
    {   /* the texture's alpha: RGB565 / 555 / 888 targets have none (opaque); ARGB4444 / 1555 / 8888 keep the drawn one */
        uint32_t mode = dc_pvr_reg(0x048) & 7;
        if (mode == 1 || mode == 4 || mode == 5) {
            p_glBindFramebuffer(GL_FRAMEBUFFER, fbo); p_glDisable(GL_SCISSOR_TEST);
            p_glColorMask(0, 0, 0, 1); p_glClearColor(0, 0, 0, 1); p_glClear(GL_COLOR_BUFFER_BIT); p_glColorMask(1, 1, 1, 1);
        }
    }
    cap[c].fbo = fbo; cap[c].tex = color_tex; cap[c].depth = depth_rb; cap[c].w = fb_w; cap[c].h = fb_h;
    cap[c].addr = addr; cap[c].ww = last_ww; cap[c].rw = last_rw; cap[c].rh = last_rh; cap[c].frame = frame_no;
    if (capture_vram) capture_readback(c);
    fbo = sf; color_tex = st; depth_rb = sd; fb_w = sw; fb_h = sh;
    disp_aspect = sa; pic_offy = so; pic_y0 = sy0; pic_y1 = sy1; zoom_x0 = zx0; zoom_x1 = zx1; zoom_y0 = zy0; zoom_y1 = zy1; last_ww = sww; last_rw = srw; last_rh = srh;
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
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
        p_glBlitFramebuffer(x0, y0, x1, y1, dx, dy, dx + dw, dy + dh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    } else if (dc_pvr_gl_fill_lines && pic_y1 > pic_y0) {         /* the drawn lines fill the height */
        p_glBlitFramebuffer(0, pic_y0, fb_w, pic_y1, dx, dy, dx + dw, dy + dh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    } else {
        int sh = (int)(pic_offy * (float)dh + (pic_offy >= 0 ? 0.5f : -0.5f));   /* centre the drawn lines (GL rows go up) */
        p_glBlitFramebuffer(0, 0, fb_w, fb_h, dx, dy - sh, dx + dw, dy + dh - sh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
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

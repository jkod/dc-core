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
typedef unsigned char GLboolean; typedef char GLchar; typedef ptrdiff_t GLsizeiptr; typedef double GLdouble;
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

/* ------------------------------------------------------------------ shaders */
static const char *vs_src =
"#version 330 core\n"
"layout(location=0) in vec3 a_pos;\n"            /* PVR x, y, 1/w */
"layout(location=1) in vec4 a_uv;\n"             /* u v (set 0), u v (set 1) */
"layout(location=2) in vec4 a_c;  layout(location=3) in vec4 a_o;\n"
"layout(location=4) in vec4 a_c1; layout(location=5) in vec4 a_o1;\n"
"uniform vec2 u_scale; uniform float u_offx; uniform int u_set; uniform int u_quad;\n"
"out vec2 v_uv; out vec4 v_c; out vec4 v_o; flat out vec4 f_c; flat out vec4 f_o; noperspective out float v_invw;\n"
"void main() {\n"
"  if (u_quad != 0) { gl_Position = vec4(a_pos.xy, 0.0, 1.0); v_invw = 1.0; return; }\n"
"  float z = max(a_pos.z, 1e-30); float w = 1.0 / z;\n"
"  vec2 ndc = vec2((a_pos.x + u_offx) * u_scale.x - 1.0, 1.0 - a_pos.y * u_scale.y);\n"
"  gl_Position = vec4(ndc * w, 0.0, w);\n"
"  v_uv = u_set != 0 ? a_uv.zw : a_uv.xy;\n"
"  v_c = u_set != 0 ? a_c1 : a_c; v_o = u_set != 0 ? a_o1 : a_o; f_c = v_c; f_o = v_o;\n"
"  v_invw = z;\n"
"}\n";

static const char *fs_src =
"#version 330 core\n"
"in vec2 v_uv; in vec4 v_c; in vec4 v_o; flat in vec4 f_c; flat in vec4 f_o; noperspective in float v_invw;\n"
"uniform sampler2D u_tex;\n"
"uniform int u_quad; uniform vec4 u_quad_col;\n"
"uniform int u_textured, u_shade, u_use_alpha, u_ign_ta, u_offset, u_gouraud, u_fog, u_pt;\n"
"uniform float u_pt_ref; uniform vec3 u_fog_ram, u_fog_vert; uniform float u_fog_den; uniform vec2 u_fog_tab[128];\n"
"out vec4 o_col;\n"
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
"  vec4 col = u_gouraud != 0 ? v_c : f_c, off = u_gouraud != 0 ? v_o : f_o;\n"
"  if (u_use_alpha == 0) col.a = 1.0;\n"
"  vec4 px;\n"
"  if (u_textured != 0) {\n"
"    vec4 t = texture(u_tex, v_uv);\n"
"    if (u_ign_ta != 0) t.a = 1.0;\n"
"    if (u_shade == 0) px = t;\n"
"    else if (u_shade == 1) px = vec4(t.rgb * col.rgb, t.a);\n"
"    else if (u_shade == 2) px = vec4(t.rgb * t.a + col.rgb * (1.0 - t.a), col.a);\n"
"    else px = t * col;\n"
"  } else px = col;\n"
"  if (u_offset != 0) px.rgb = min(px.rgb + off.rgb, vec3(1.0));\n"
"  if (u_fog != 2) {\n"
"    float f; vec3 fc;\n"
"    if (u_fog == 1) { fc = u_fog_vert; f = off.a; } else { fc = u_fog_ram; f = fog_alpha(v_invw); }\n"
"    if (u_fog == 3) px = vec4(fc, f); else px.rgb = mix(px.rgb, fc, f);\n"
"  }\n"
"  if (u_pt != 0 && px.a < u_pt_ref) discard;\n"
"  o_col = px;\n"
"  gl_FragDepth = clamp((log2(max(v_invw, 1e-30)) + 64.0) / 128.0, 0.0, 1.0);\n"
"}\n";

static float depth_of(float invw) { float d = (log2f(fmaxf(invw, 1e-30f)) + 64.f) / 128.f; return d < 0 ? 0 : d > 1 ? 1 : d; }

/* ------------------------------------------------------------------ state */
typedef struct { float x, y, z, u, v, u1, v1, c[4], o[4], c1[4], o1[4]; } gv;
static GLuint prog, vao, vbo, fbo, color_tex, depth_rb;
static int fb_w, fb_h, ready;
static struct { GLint scale, offx, set, quad, quad_col, tex, textured, shade, use_alpha, ign_ta, offset, gouraud, fog, pt, pt_ref,
                fog_ram, fog_vert, fog_den, fog_tab; } U;
static gv *verts; static int nverts, cap_verts;
typedef struct { int first, count; } span;
static span *spans; static int cap_spans;

/* GL textures for decoded PVR textures, by decode id */
#define GLTEX 1024
static struct { uint64_t id; GLuint name; unsigned last; } gtex[GLTEX];
static unsigned frame_no;

static GLuint compile(GLenum type, const char *src) {
    GLuint s = p_glCreateShader(type);
    p_glShaderSource(s, 1, &src, NULL); p_glCompileShader(s);
    GLint ok = 0; p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[2048]; p_glGetShaderInfoLog(s, sizeof log, NULL, log); fprintf(stderr, "pvr_gl: shader: %s\n", log); }
    return s;
}

int dc_pvr_gl_init(void *(*getproc)(const char *)) {
    int missing = 0;
#define F(ret, name, args) p_##name = (ret (*) args)getproc(#name); if (!p_##name) { fprintf(stderr, "pvr_gl: missing %s\n", #name); missing++; }
    GLFUNCS
#undef F
    if (missing) return -1;
    prog = p_glCreateProgram();
    p_glAttachShader(prog, compile(GL_VERTEX_SHADER, vs_src));
    p_glAttachShader(prog, compile(GL_FRAGMENT_SHADER, fs_src));
    p_glLinkProgram(prog);
    GLint ok = 0; p_glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) { char log[2048]; p_glGetProgramInfoLog(prog, sizeof log, NULL, log); fprintf(stderr, "pvr_gl: link: %s\n", log); return -1; }
#define UL(f, n) U.f = p_glGetUniformLocation(prog, n)
    UL(scale, "u_scale"); UL(offx, "u_offx"); UL(set, "u_set"); UL(quad, "u_quad"); UL(quad_col, "u_quad_col"); UL(tex, "u_tex");
    UL(textured, "u_textured"); UL(shade, "u_shade"); UL(use_alpha, "u_use_alpha"); UL(ign_ta, "u_ign_ta"); UL(offset, "u_offset");
    UL(gouraud, "u_gouraud"); UL(fog, "u_fog"); UL(pt, "u_pt"); UL(pt_ref, "u_pt_ref"); UL(fog_ram, "u_fog_ram");
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
    ready = 1;
    return 0;
}

int dc_pvr_gl_widescreen;
float dc_pvr_gl_hud_z;
static float disp_aspect = 4.f / 3.f;

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
        if (gtex[i].name && gtex[i].id == ti.id) { gtex[i].last = frame_no; return gtex[i].name; }
        if (!gtex[i].name && slot < 0) slot = i;
        if (gtex[i].last < gtex[oldest].last) oldest = i;
    }
    if (slot < 0) { slot = oldest; p_glDeleteTextures(1, &gtex[slot].name); gtex[slot].name = 0; }
    GLuint n; p_glGenTextures(1, &n); p_glBindTexture(GL_TEXTURE_2D, n);
    p_glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ti.w, ti.h, 0, GL_BGRA, GL_UNSIGNED_BYTE, ti.px);
    gtex[slot].id = ti.id; gtex[slot].name = n; gtex[slot].last = frame_no;
    return n;
}

/* ------------------------------------------------------------------ geometry */
static float push_dx;                   /* x shift for the polygon being pushed (HUD in widescreen) */
static void push(const pvr_vtx *v) {
    if (nverts == cap_verts) { cap_verts = cap_verts ? cap_verts * 2 : 8192; verts = realloc(verts, cap_verts * sizeof *verts); }
    gv *g = &verts[nverts++];
    g->x = v->x + push_dx; g->y = v->y; g->z = v->z; g->u = v->u; g->v = v->v; g->u1 = v->u1; g->v1 = v->v1;
    memcpy(g->c, v->c, 16); memcpy(g->o, v->o, 16); memcpy(g->c1, v->c1, 16); memcpy(g->o1, v->o1, 16);
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

static void set_poly_state(const pvr_poly *p, int set) {
    uint32_t tsp = set ? p->tsp1 : p->tsp, tcw = set ? p->tcw1 : p->tcw;
    int tex = (p->isp >> 25) & 1;
    p_glUniform1i(U.set, set);
    p_glUniform1i(U.textured, tex); p_glUniform1i(U.offset, (p->isp >> 24) & 1); p_glUniform1i(U.gouraud, (p->isp >> 23) & 1);
    p_glUniform1i(U.shade, (tsp >> 6) & 3); p_glUniform1i(U.use_alpha, (tsp >> 20) & 1); p_glUniform1i(U.ign_ta, (tsp >> 19) & 1);
    p_glUniform1i(U.fog, (tsp >> 22) & 3); p_glUniform1i(U.pt, p->list == 4);
    if (tex) {
        p_glActiveTexture(GL_TEXTURE0); p_glBindTexture(GL_TEXTURE_2D, texture_for(tcw, tsp));
        int filt = ((tsp >> 13) & 3) ? GL_LINEAR : GL_NEAREST;
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt); p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt);
        int cu = (tsp >> 16) & 1, cv = (tsp >> 15) & 1, fu = (tsp >> 18) & 1, fv = (tsp >> 17) & 1;
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, cu ? GL_CLAMP_TO_EDGE : fu ? GL_MIRRORED_REPEAT : GL_REPEAT);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, cv ? GL_CLAMP_TO_EDGE : fv ? GL_MIRRORED_REPEAT : GL_REPEAT);
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
    float x = ((const sort_e *)a)->key, y = ((const sort_e *)b)->key;
    return x < y ? -1 : x > y;
}

/* Draw batching: consecutive polygons with the same render state go out in one call (menus can
 * have thousands of small polygons, and per-call overhead dominates). Draw order is unchanged. */
typedef struct { uint32_t isp, tsp, tcw; int list, set, mark, blend; } bkey;
static struct { bkey key; int valid; GLint *first; GLsizei *count; int n, cap; } B;
static void batch_flush(void) {
    if (B.n == 1) p_glDrawArrays(GL_TRIANGLES, B.first[0], B.count[0]);
    else if (B.n > 1) p_glMultiDrawArrays(GL_TRIANGLES, B.first, B.count, B.n);
    B.n = 0;
}
static void batch_reset(void) { batch_flush(); B.valid = 0; }
/* mark: stencil reference (-1 = leave the stencil state alone); blend: set the blend function */
static void batch_add(const pvr_poly *p, int set, int mark, int blend, int first, int count) {
    if (!count) return;
    bkey k = { p->isp, set ? p->tsp1 : p->tsp, set ? p->tcw1 : p->tcw, p->list, set, mark, blend };
    if (!B.valid || memcmp(&k, &B.key, sizeof k)) {
        batch_flush();
        if (mark >= 0) p_glStencilFunc(GL_ALWAYS, mark, 0xFF);
        set_poly_state(p, set);
        if (set) { p_glDepthFunc(GL_EQUAL); p_glDepthMask(0); }
        if (blend) p_glBlendFunc(blend_src((p->tsp >> 29) & 7), blend_dst((p->tsp >> 26) & 7));
        B.key = k; B.valid = 1;
    }
    if (B.n && B.first[B.n - 1] + B.count[B.n - 1] == first) { B.count[B.n - 1] += count; return; }   /* contiguous */
    if (B.n == B.cap) { B.cap = B.cap ? B.cap * 2 : 256; B.first = realloc(B.first, B.cap * sizeof *B.first); B.count = realloc(B.count, B.cap * sizeof *B.count); }
    B.first[B.n] = first; B.count[B.n] = count; B.n++;
}

void dc_pvr_gl_render(int scale) {
    if (!ready) return;
    if (scale < 1) scale = 1;
    frame_no++;
    pvr_frame f;
    pvr_get_frame(&f);
    int RW = f.width * (f.hscale ? 2 : 1), RH = f.height;
    /* widescreen: a 16:9 target with the console's 4:3 picture in the middle; the sides show what
     * the game draws beyond the screen edges */
    float WW = dc_pvr_gl_widescreen ? RW * 4.f / 3.f : (float)RW;
    ensure_fbo(dc_pvr_gl_widescreen ? (f.width * scale * 4 + 2) / 3 : f.width * scale, f.height * scale);
    disp_aspect = dc_pvr_gl_widescreen ? 16.f / 9.f : 4.f / 3.f;

    /* geometry: every polygon's triangles, then volume triangles, then a full-screen quad */
    nverts = 0;
    if (f.npolys + 1 > cap_spans) { cap_spans = f.npolys + 1024; spans = realloc(spans, cap_spans * sizeof *spans); }
    float cull_small = 0; { uint32_t cv = dc_pvr_reg(0x078); memcpy(&cull_small, &cv, 4); }
    /* widescreen HUD: 2D overlay polygons (every vertex at 1/w >= dc_pvr_gl_hud_z) in the left or
     * right third of the 4:3 picture move out to the edges of the 16:9 one; only in frames that
     * also draw a 3D scene, so full-screen 2D menus keep their layout */
    float hud_dx = 0;
    if (dc_pvr_gl_widescreen && dc_pvr_gl_hud_z > 0) {
        int n3d = 0;
        for (int i = 0; i < f.npolys && n3d < 50; i++) {
            const pvr_vtx *v = f.verts + f.polys[i].first;
            if (f.polys[i].n >= 3 && v[0].z < dc_pvr_gl_hud_z && (v[0].z != v[1].z || v[0].z != v[2].z)) n3d++;
        }
        if (n3d >= 50) hud_dx = (WW - (float)RW) * 0.5f;
    }
    for (int i = 0; i < f.npolys; i++) {
        const pvr_poly *p = &f.polys[i];
        const pvr_vtx *v = f.verts + p->first;
        push_dx = 0;
        if (hud_dx > 0) {
            float zmin = 1e30f, cx = 0;
            for (int k = 0; k < p->n; k++) { if (v[k].z < zmin) zmin = v[k].z; cx += v[k].x; }
            cx /= (float)p->n;
            if (zmin >= dc_pvr_gl_hud_z) push_dx = cx < RW / 3.f ? -hud_dx : cx > RW * 2.f / 3.f ? hud_dx : 0;
        }
        spans[i].first = nverts;
        if (p->sprite) { tri(p, &v[0], &v[1], &v[2], 0, cull_small); tri(p, &v[0], &v[2], &v[3], 0, cull_small); }
        else for (int k = 0; k + 2 < p->n; k++) tri(p, &v[k], &v[k + 1], &v[k + 2], k & 1, cull_small);
        spans[i].count = nverts - spans[i].first;
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
    p_glUniform1i(U.tex, 0); p_glUniform1i(U.quad, 0);
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
    /* outside the clip rectangle: black; inside: background plane, and all drawing scissored to it */
    p_glDisable(GL_SCISSOR_TEST);
    p_glClearColor(0, 0, 0, 1);
    p_glClear(GL_COLOR_BUFFER_BIT);
    { int sx0 = dc_pvr_gl_widescreen ? 0 : (int)((f.clip_x0 * (float)fb_w) / f.width), sx1 = dc_pvr_gl_widescreen ? fb_w : (int)(((f.clip_x1 + 1) * (float)fb_w) / f.width);
      int sy0 = f.clip_y0 * scale, sy1 = (f.clip_y1 + 1) * scale;
      p_glEnable(GL_SCISSOR_TEST);
      p_glScissor(sx0, fb_h - sy1, sx1 - sx0, sy1 - sy0); }
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
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
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
    p_glBlitFramebuffer(0, 0, fb_w, fb_h, dx, dy, dx + dw, dy + dh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

/* show an RGB8 image (top row first) instead of a rendered frame: screens the CPU drew directly
 * into the framebuffer (dc_pvr_display) */
void dc_pvr_gl_load_rgb(const uint8_t *rgb, int w, int h) {
    if (!ready || w <= 0 || h <= 0) return;
    if (!fbo) ensure_fbo(w, h);
    disp_aspect = 4.f / 3.f;                               /* a CPU-drawn screen: always 4:3 */
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

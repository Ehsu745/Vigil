/* gpu.c: the black hole drawn on the GPU. blackhole-simulation's fragment shader with Vigil's patches (bh_shader.h,
 * written from web/blackhole-mock.html by bh_shader.py), run by OpenGL 3.3 (opengl32 and WGL: part of Windows) into an
 * offscreen picture, read back for the shared renderer to lay into the core's picture (BH_GPU_IMG in render.c), which
 * adds what is ours: the roll, glow, the fade before the window's edge, the dark under it. Everything happens on
 * the panel's main thread (the context is made current there once). If anything is missing, gpu_init says no and the
 * CPU engine (render.c, bh2_*) draws it as before. */
#include <windows.h>
#include <GL/gl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "gpu.h"
#include "bh_shader.h"

typedef char GLchar_;
typedef ptrdiff_t GLsizeiptr_;
#define GL_FRAMEBUFFER_ 0x8D40
#define GL_COLOR_ATTACHMENT0_ 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE_ 0x8CD5
#define GL_FRAGMENT_SHADER_ 0x8B30
#define GL_VERTEX_SHADER_ 0x8B31
#define GL_COMPILE_STATUS_ 0x8B81
#define GL_LINK_STATUS_ 0x8B82
#define GL_INFO_LOG_LENGTH_ 0x8B84
#define GL_ARRAY_BUFFER_ 0x8892
#define GL_STATIC_DRAW_ 0x88E4
#define GL_TEXTURE0_ 0x84C0
#define GL_RGBA8_ 0x8058
#define GL_CLAMP_TO_EDGE_ 0x812F
#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x0001

#define GLF(ret, name, args) typedef ret (APIENTRY *name##_t) args; static name##_t p##name;
GLF(GLuint, glCreateShader, (GLenum))
GLF(void, glShaderSource, (GLuint, GLsizei, const GLchar_ *const *, const GLint *))
GLF(void, glCompileShader, (GLuint))
GLF(void, glGetShaderiv, (GLuint, GLenum, GLint *))
GLF(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar_ *))
GLF(GLuint, glCreateProgram, (void))
GLF(void, glAttachShader, (GLuint, GLuint))
GLF(void, glBindAttribLocation, (GLuint, GLuint, const GLchar_ *))
GLF(void, glLinkProgram, (GLuint))
GLF(void, glGetProgramiv, (GLuint, GLenum, GLint *))
GLF(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar_ *))
GLF(void, glUseProgram, (GLuint))
GLF(GLint, glGetUniformLocation, (GLuint, const GLchar_ *))
GLF(void, glUniform1f, (GLint, GLfloat))
GLF(void, glUniform1i, (GLint, GLint))
GLF(void, glUniform2f, (GLint, GLfloat, GLfloat))
GLF(void, glUniform3f, (GLint, GLfloat, GLfloat, GLfloat))
GLF(void, glUniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat))
GLF(void, glUniform4fv, (GLint, GLsizei, const GLfloat *))
GLF(void, glUniform2fv, (GLint, GLsizei, const GLfloat *))
GLF(void, glGenFramebuffers, (GLsizei, GLuint *))
GLF(void, glBindFramebuffer, (GLenum, GLuint))
GLF(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))
GLF(GLenum, glCheckFramebufferStatus, (GLenum))
GLF(void, glGenVertexArrays, (GLsizei, GLuint *))
GLF(void, glBindVertexArray, (GLuint))
GLF(void, glGenBuffers, (GLsizei, GLuint *))
GLF(void, glBindBuffer, (GLenum, GLuint))
GLF(void, glBufferData, (GLenum, GLsizeiptr_, const void *, GLenum))
GLF(void, glEnableVertexAttribArray, (GLuint))
GLF(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *))
GLF(void, glActiveTexture, (GLenum))
typedef HGLRC (WINAPI *wglCreateContextAttribsARB_t)(HDC, HGLRC, const int *);

static int g_state; /* 0 not tried, 1 ready, -1 not available */
static char g_why[256];
static HWND g_glw;
static HDC g_gldc;
static HGLRC g_glrc;
static GLuint g_prog, g_fb, g_tex, g_vao;
static int g_tex_n;
static unsigned char *g_px;
static GLint U_(const char *n) { return pglGetUniformLocation(g_prog, n); }
static int fail(const char *why) { snprintf(g_why, sizeof g_why, "%s", why); g_state = -1; return 0; }
static void *gl_proc(const char *name) {
    void *p = (void *)wglGetProcAddress(name);
    if (p == NULL || p == (void *)1 || p == (void *)2 || p == (void *)3 || p == (void *)-1) return NULL;
    return p;
}
static GLuint shader(GLenum type, const char *src) {
    GLuint s = pglCreateShader(type);
    pglShaderSource(s, 1, &src, NULL);
    pglCompileShader(s);
    GLint ok = 0; pglGetShaderiv(s, GL_COMPILE_STATUS_, &ok);
    if (!ok) { char log[200] = ""; pglGetShaderInfoLog(s, sizeof log, NULL, log); snprintf(g_why, sizeof g_why, "shader: %s", log); return 0; }
    return s;
}
const char *gpu_why(void) { return g_why; }
int gpu_init(void) {
    if (g_state) return g_state > 0;
    WNDCLASSW wc; memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandle(NULL); wc.lpszClassName = L"VigilGL"; wc.style = CS_OWNDC;
    RegisterClassW(&wc);
    g_glw = CreateWindowExW(0, L"VigilGL", L"", WS_POPUP, 0, 0, 8, 8, NULL, NULL, wc.hInstance, NULL); /* (never shown) */
    if (!g_glw) return fail("no window");
    g_gldc = GetDC(g_glw);
    PIXELFORMATDESCRIPTOR pfd; memset(&pfd, 0, sizeof pfd);
    pfd.nSize = sizeof pfd; pfd.nVersion = 1; pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER; pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 32;
    int pf = ChoosePixelFormat(g_gldc, &pfd);
    if (!pf || !SetPixelFormat(g_gldc, pf, &pfd)) return fail("no OpenGL pixel format");
    HGLRC old = wglCreateContext(g_gldc);
    if (!old || !wglMakeCurrent(g_gldc, old)) return fail("no OpenGL context");
    wglCreateContextAttribsARB_t mk = (wglCreateContextAttribsARB_t)gl_proc("wglCreateContextAttribsARB");
    int attr[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, 3, WGL_CONTEXT_MINOR_VERSION_ARB, 3, WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0};
    g_glrc = mk ? mk(g_gldc, NULL, attr) : NULL;
    wglMakeCurrent(NULL, NULL); wglDeleteContext(old);
    if (!g_glrc || !wglMakeCurrent(g_gldc, g_glrc)) return fail("no OpenGL 3.3 (an old driver, Remote Desktop or a VM?)");
#define LOAD(name) if (!(p##name = (name##_t)gl_proc(#name))) return fail("missing " #name);
    LOAD(glCreateShader) LOAD(glShaderSource) LOAD(glCompileShader) LOAD(glGetShaderiv) LOAD(glGetShaderInfoLog) LOAD(glCreateProgram)
    LOAD(glAttachShader) LOAD(glBindAttribLocation) LOAD(glLinkProgram) LOAD(glGetProgramiv) LOAD(glGetProgramInfoLog) LOAD(glUseProgram)
    LOAD(glGetUniformLocation) LOAD(glUniform1f) LOAD(glUniform1i) LOAD(glUniform2f) LOAD(glUniform3f) LOAD(glUniform4f) LOAD(glUniform4fv) LOAD(glUniform2fv)
    LOAD(glGenFramebuffers) LOAD(glBindFramebuffer) LOAD(glFramebufferTexture2D) LOAD(glCheckFramebufferStatus)
    LOAD(glGenVertexArrays) LOAD(glBindVertexArray) LOAD(glGenBuffers) LOAD(glBindBuffer) LOAD(glBufferData)
    LOAD(glEnableVertexAttribArray) LOAD(glVertexAttribPointer) LOAD(glActiveTexture)
#undef LOAD
    GLuint vs = shader(GL_VERTEX_SHADER_, "#version 330 core\nlayout(location = 0) in vec2 position;\nvoid main() { gl_Position = vec4(position, 0.0, 1.0); }\n");
    GLuint fs = vs ? shader(GL_FRAGMENT_SHADER_, BH_FRAG) : 0;
    if (!vs || !fs) { g_state = -1; return 0; }
    g_prog = pglCreateProgram();
    pglAttachShader(g_prog, vs); pglAttachShader(g_prog, fs); pglBindAttribLocation(g_prog, 0, "position"); pglLinkProgram(g_prog);
    GLint ok = 0; pglGetProgramiv(g_prog, GL_LINK_STATUS_, &ok);
    if (!ok) { char log[200] = ""; pglGetProgramInfoLog(g_prog, sizeof log, NULL, log); snprintf(g_why, sizeof g_why, "link: %s", log); g_state = -1; return 0; }
    /* a quad over the whole picture, and its noise textures (random bytes, as blackhole-simulation makes them) */
    GLuint vb; static const float Q[8] = {-1, -1, 1, -1, -1, 1, 1, 1};
    pglGenVertexArrays(1, &g_vao); pglBindVertexArray(g_vao);
    pglGenBuffers(1, &vb); pglBindBuffer(GL_ARRAY_BUFFER_, vb); pglBufferData(GL_ARRAY_BUFFER_, sizeof Q, Q, GL_STATIC_DRAW_);
    pglEnableVertexAttribArray(0); pglVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
    unsigned char *d = malloc(256 * 256 * 4);
    unsigned st = 0x9E3779B9u;
    for (int u = 0; u < 2; u++) {
        for (int i = 0; i < 256 * 256 * 4; i++) { st ^= st << 13; st ^= st >> 17; st ^= st << 5; d[i] = (unsigned char)(st >> 9); }
        GLuint t; glGenTextures(1, &t);
        pglActiveTexture(GL_TEXTURE0_ + 1 + u); glBindTexture(GL_TEXTURE_2D, t);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8_, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, d);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, u ? GL_NEAREST : GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, u ? GL_NEAREST : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    }
    free(d);
    pglGenFramebuffers(1, &g_fb); glGenTextures(1, &g_tex);
    g_state = 1;
    return 1;
}
const unsigned char *gpu_hole(const Session *s, int n, float E, double now, int mini) {
    if (!gpu_init() || n < 8 || n > 2048) return NULL;
    if (wglGetCurrentContext() != g_glrc) wglMakeCurrent(g_gldc, g_glrc);
    if (n != g_tex_n) { /* its picture: an n x n texture to draw into */
        pglActiveTexture(GL_TEXTURE0_); glBindTexture(GL_TEXTURE_2D, g_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8_, n, n, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        pglBindFramebuffer(GL_FRAMEBUFFER_, g_fb); pglFramebufferTexture2D(GL_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_TEXTURE_2D, g_tex, 0);
        if (pglCheckFramebufferStatus(GL_FRAMEBUFFER_) != GL_FRAMEBUFFER_COMPLETE_) { fail("no offscreen picture"); return NULL; }
        free(g_px); g_px = malloc((size_t)n * n * 4);
        if (!g_px) return NULL;
        g_tex_n = n;
    }
    const BhTune *T = &BH_TUNE;
    const HoleRS *H = &s->mo.rs.hole;
    float ax[3]; bh_hole_axis(s, now, ax);
    float inc = acosf(fmaxf(-1, fminf(1, ax[2]))), my = fmaxf(0.005f, fminf(0.995f, 1 - inc / 3.14159265f)); /* (their camera: 0.5 + elevation / 180) */
    int act = H->act;
    float at = act ? (float)(now - H->act_at) : 0, fo = act ? fmaxf(0, fminf(1, (float)(H->act_end - now) / 0.9f)) : 0;
    fo = fo * fo * (3 - 2 * fo);
    float spot = 0, flare = 0;
    if (act == HA_SPOT) spot = fminf(1, at / 0.6f) * fmaxf(0, fminf(1, (HOLE_ACT_DUR[HA_SPOT] - at) / 3.5f)) * fo;
    if (act == HA_FLARE && at > 0) flare = fminf(1, at / 0.3f) * fo; /* (the bubble and plasmoids keep their own time: u_flare_t) */
    pglBindFramebuffer(GL_FRAMEBUFFER_, g_fb);
    glViewport(0, 0, n, n);
    pglUseProgram(g_prog);
    pglBindVertexArray(g_vao);
    pglUniform2f(U_("u_resolution"), (float)n, (float)n); pglUniform1f(U_("u_time"), (float)fmod(now, 10000.0));
    pglUniform1f(U_("u_mass"), 1); pglUniform1f(U_("u_spin"), T->b2_spin); pglUniform1f(U_("u_zoom"), T->b2_dist);
    pglUniform1f(U_("u_focal"), T->b2_dist * 0.5f / E);
    pglUniform1f(U_("u_disk_size"), mini ? fminf(T->b2_disk, E * 0.85f) : T->b2_disk); pglUniform1f(U_("u_disk_scale_height"), mini ? 0.1f : T->b2_height); pglUniform1f(U_("u_disk_temp"), T->b2_temp);
    pglUniform1f(U_("u_disk_density"), T->b2_dens); pglUniform1f(U_("u_lensing_strength"), T->b2_lens); pglUniform1f(U_("u_frame_dragging_strength"), 2);
    pglUniform1i(U_("u_maxRaySteps"), (int)T->b2_steps);
    pglUniform1f(U_("u_show_redshift"), 0); pglUniform1f(U_("u_show_kerr_shadow"), 0); pglUniform1f(U_("u_debug"), 0); pglUniform1f(U_("u_shadowCount"), 0);
    pglUniform3f(U_("u_camPos"), 0, 0, 0); pglUniform4f(U_("u_camQuat"), 0, 0, 0, 1);
    pglUniform2f(U_("u_mouse"), 0.5f, my); /* (their turn: 0.5 is none, so the gas and the hand agree on left and right) */
    { float rho = atan2f(ax[0], ax[1]); /* its roll (the wheel only turns its gas, never mirrors it) */
      pglUniform1f(U_("u_roll_c"), cosf(rho)); pglUniform1f(U_("u_roll_s"), sinf(rho)); pglUniform1f(U_("u_flip"), 1); }
    pglUniform3f(U_("u_sky"), ax[0], ax[1], ax[2]); /* (its axis on the screen: the stars stay put, drawn out the way it is turned) */
    pglUniform1i(U_("u_noiseTex"), 1); pglUniform1i(U_("u_blueNoiseTex"), 2);
    { float echo = act == HA_FLARE ? flare * expf(-(at - BH_FLARE_SNAP - 0.5f) * (at - BH_FLARE_SNAP - 0.5f) / 0.18f) : 0; /* (the flare's light, lensed round, lights the photon ring a moment later) */
      pglUniform1f(U_("u_ring_gain"), T->ring_a * (1 + 3 * echo)); } pglUniform1f(U_("u_star_gain"), !mini && (HOLE_ACTS >> HA_LENS & 1) ? T->star_a : 0); pglUniform1f(U_("u_star_r"), 0.95f);
    pglUniform1f(U_("u_expo"), bh_hole_expo(s, now)); pglUniform1f(U_("u_disk_time"), H->flow * T->b2_speed); pglUniform1f(U_("u_nebula"), 0);
    pglUniform1f(U_("u_heat"), H->tde.heat);
    { /* the actions' gas (bh_gas_points: the view's frame), into the shader's own: the roll undone, then the camera's tilt */
        static float P[64][4], Q[64][2];
        int gn = mini ? 0 : bh_gas_points(s, now, P, Q, 64);
        float rho = atan2f(ax[0], ax[1]), cr = cosf(rho), sr = sinf(rho), f = 1;
        float th = (my - 0.5f) * 3.14159265f, ct = cosf(th), st = sinf(th), lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
        for (int i = 0; i < gn; i++) {
            float X = P[i][0], Y = P[i][1], Z = P[i][2], Xr = (X * cr - Y * sr) * f, Yr = X * sr + Y * cr;
            P[i][0] = Xr; P[i][1] = Yr * ct + Z * st; P[i][2] = Yr * st - Z * ct;
            for (int q = 0; q < 3; q++) { lo[q] = fminf(lo[q], P[i][q]); hi[q] = fmaxf(hi[q], P[i][q]); }
        }
        float cx = (lo[0] + hi[0]) / 2, cy = (lo[1] + hi[1]) / 2, cz = (lo[2] + hi[2]) / 2, rb = gn ? 0.5f * sqrtf((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) + (hi[2] - lo[2]) * (hi[2] - lo[2])) + 3 : 0;
        pglUniform1i(U_("u_gn"), gn); pglUniform4f(U_("u_gb"), cx, cy, cz, rb);
        if (gn) { pglUniform4fv(U_("u_gp"), gn, &P[0][0]); pglUniform2fv(U_("u_gq"), gn, &Q[0][0]); }
    }
    { float ang, sr; bh_spot_state(s, now, &ang, &sr); /* (spiralling in: render.c) */
      pglUniform1f(U_("u_spot"), spot); pglUniform1f(U_("u_spot_a"), ang); pglUniform1f(U_("u_spot_t"), at); pglUniform1f(U_("u_spot_r"), sr); }
    pglUniform1f(U_("u_flare"), flare); pglUniform1f(U_("u_flare_a"), H->act_p * 3.14159265f); pglUniform1f(U_("u_flare_t"), at);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, n, n, GL_RGBA, GL_UNSIGNED_BYTE, g_px);
    if (glGetError() != GL_NO_ERROR) { fail("drawing failed"); return NULL; }
    return g_px;
}

/* Software renderer. Everything is added into a float RGB canvas (light adds up, like a screen blend)
 * and clamped at the end. Glow is computed from distance, so there is no blur pass.
 * Sizes are given for a 600-pixel tile and scaled by k = tile / 600. */
#include "vigil.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAM 7.0f

typedef struct { int x0, y0, x1, y1; } Clip; /* half-open */

void canvas_resize(Canvas *c, int w, int h) {
    if (w == c->w && h == c->h && c->p) return;
    free(c->p);
    c->w = w; c->h = h;
    c->p = malloc(sizeof(float) * 3 * (size_t)w * (size_t)h);
}

/* the light is laid over the terminal's background colour (a screen blend), so the picture has no black box around it */
void canvas_bytes(const Canvas *c, uint8_t *rgb, Col bg) {
    size_t n = (size_t)c->w * (size_t)c->h * 3;
    float b[3] = {bg.r, bg.g, bg.b};
    for (size_t i = 0; i < n; i++) {
        float p = c->p[i] > 1 ? 1 : c->p[i], v = 1 - (1 - b[i % 3]) * (1 - p);
        rgb[i] = (uint8_t)(v <= 0 ? 0 : v >= 1 ? 255 : v * 255.0f + 0.5f);
    }
}

/* For a see-through window: alpha is the brightest channel, so dark pixels let what is behind show and the
 * glow lies over it. The colour is at most alpha, which is what "premultiplied" asks for. */
void canvas_rgba(const Canvas *c, uint8_t *rgba) {
    size_t n = (size_t)c->w * (size_t)c->h;
    for (size_t i = 0; i < n; i++) {
        float r = c->p[i * 3], g = c->p[i * 3 + 1], b = c->p[i * 3 + 2];
        r = r < 0 ? 0 : r > 1 ? 1 : r; g = g < 0 ? 0 : g > 1 ? 1 : g; b = b < 0 ? 0 : b > 1 ? 1 : b;
        float a = r > g ? (r > b ? r : b) : (g > b ? g : b);
        rgba[i * 4] = (uint8_t)(r * 255.0f + 0.5f); rgba[i * 4 + 1] = (uint8_t)(g * 255.0f + 0.5f);
        rgba[i * 4 + 2] = (uint8_t)(b * 255.0f + 0.5f); rgba[i * 4 + 3] = (uint8_t)(a * 255.0f + 0.5f);
    }
}

static inline void add(Canvas *c, int x, int y, Col col, float a) {
    float *p = c->p + 3 * ((size_t)y * (size_t)c->w + (size_t)x);
    p[0] += col.r * a; p[1] += col.g * a; p[2] += col.b * a;
}

static inline float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static inline Col mix(Col a, Col b, float t) { return (Col){a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }
static const Col WHITE = {1, 1, 1};

/* a thin line with a halo: `line` at full strength within half a pixel, then `halo` falling off to `pad` */
static void glow_line(Canvas *c, Clip cl, float x0, float y0, float x1, float y1, Col line, float a_line, Col halo, float a_halo, float k) {
    float hw = k > 1 ? 0.5f * k : 0.5f, pad = 11.0f * (k > 0.4f ? k : 0.4f);
    int bx0 = (int)floorf(fminf(x0, x1) - pad), bx1 = (int)ceilf(fmaxf(x0, x1) + pad) + 1;
    int by0 = (int)floorf(fminf(y0, y1) - pad), by1 = (int)ceilf(fmaxf(y0, y1) + pad) + 1;
    if (bx0 < cl.x0) bx0 = cl.x0;
    if (by0 < cl.y0) by0 = cl.y0;
    if (bx1 > cl.x1) bx1 = cl.x1;
    if (by1 > cl.y1) by1 = cl.y1;
    float dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy;
    if (len2 < 1e-6f) len2 = 1e-6f;
    for (int y = by0; y < by1; y++) for (int x = bx0; x < bx1; x++) {
        float px = x + 0.5f - x0, py = y + 0.5f - y0;
        float t = clamp01((px * dx + py * dy) / len2);
        float ex = px - dx * t, ey = py - dy * t, d2 = ex * ex + ey * ey;
        if (d2 >= pad * pad) continue;
        float d = sqrtf(d2), f = 1.0f - d / pad, f2 = f * f, f8 = f2 * f2 * f2 * f2;
        float core = clamp01(hw + 0.5f - d);
        float *p = c->p + 3 * ((size_t)y * (size_t)c->w + (size_t)x);
        float h = (0.20f * f8 + 0.05f * f2) * a_halo, l = core * a_line;
        p[0] += line.r * l + halo.r * h; p[1] += line.g * l + halo.g * h; p[2] += line.b * l + halo.b * h;
    }
}

/* a small round light: `in` at the centre, `out` at `mid` of the radius, nothing at the edge */
static void blob(Canvas *c, Clip cl, float cx, float cy, float rad, Col in, float a_in, Col out, float a_out, float mid) {
    int bx0 = (int)floorf(cx - rad), bx1 = (int)ceilf(cx + rad) + 1, by0 = (int)floorf(cy - rad), by1 = (int)ceilf(cy + rad) + 1;
    if (bx0 < cl.x0) bx0 = cl.x0;
    if (by0 < cl.y0) by0 = cl.y0;
    if (bx1 > cl.x1) bx1 = cl.x1;
    if (by1 > cl.y1) by1 = cl.y1;
    for (int y = by0; y < by1; y++) for (int x = bx0; x < bx1; x++) {
        float dx = x + 0.5f - cx, dy = y + 0.5f - cy, u = sqrtf(dx * dx + dy * dy) / rad;
        if (u >= 1) continue;
        if (u < mid) { float t = u / mid; add(c, x, y, mix(in, out, t), a_in + (a_out - a_in) * t); }
        else add(c, x, y, out, a_out * (1 - (u - mid) / (1 - mid)));
    }
}

static void disc(Canvas *c, Clip cl, float cx, float cy, float rad, Col col, float a) {
    int bx0 = (int)floorf(cx - rad - 1), bx1 = (int)ceilf(cx + rad + 1) + 1, by0 = (int)floorf(cy - rad - 1), by1 = (int)ceilf(cy + rad + 1) + 1;
    if (bx0 < cl.x0) bx0 = cl.x0;
    if (by0 < cl.y0) by0 = cl.y0;
    if (bx1 > cl.x1) bx1 = cl.x1;
    if (by1 > cl.y1) by1 = cl.y1;
    for (int y = by0; y < by1; y++) for (int x = bx0; x < bx1; x++) {
        float dx = x + 0.5f - cx, dy = y + 0.5f - cy, cov = clamp01(rad + 0.5f - sqrtf(dx * dx + dy * dy));
        if (cov > 0) add(c, x, y, col, a * cov);
    }
}

/* faulty-lamp flicker: mostly on with a little jitter, broken up by dropouts and bursts of stutter */
static double hash1(double n) { double x = sin(n * 127.1) * 43758.5453; return x - floor(x); }
static float glitch_val(double t) {
    double seg = hash1(floor(t * 3)), fast = hash1(floor(t * 24) + 91);
    if (seg < 0.22) return (float)(0.12 + 0.25 * fast);
    if (seg < 0.40) return fast < 0.5 ? 0.25f : 1.05f;
    return (float)(0.92 + 0.12 * fast);
}

/* the core: a wide halo, a white-hot body, and a solid inner disc out to 2/3 of the core radius */
static void draw_core(Canvas *c, Clip cl, float ox, float oy, float rc, const Look *L, float inten) {
    float hR = rc * L->hr, bR = rc * 1.5f, sR = rc * 2.0f / 3.0f, lit = inten > 1 ? 1 : inten;
    float solid = 0.08f + 0.5f * (1 - lit); /* a brightness floor: brighter than the shell when dim, nearly nothing when bright */
    int bx0 = (int)floorf(ox - hR), bx1 = (int)ceilf(ox + hR) + 1, by0 = (int)floorf(oy - hR), by1 = (int)ceilf(oy + hR) + 1;
    if (bx0 < cl.x0) bx0 = cl.x0;
    if (by0 < cl.y0) by0 = cl.y0;
    if (bx1 > cl.x1) bx1 = cl.x1;
    if (by1 > cl.y1) by1 = cl.y1;
    for (int y = by0; y < by1; y++) for (int x = bx0; x < bx1; x++) {
        float dx = x + 0.5f - ox, dy = y + 0.5f - oy, r = sqrtf(dx * dx + dy * dy);
        if (r >= hR) continue;
        float u = r / hR, a;
        if (u < 0.14f) a = 0.5f + (0.22f - 0.5f) * u / 0.14f;
        else if (u < 0.4f) a = 0.22f + (0.06f - 0.22f) * (u - 0.14f) / 0.26f;
        else a = 0.06f * (1 - (u - 0.4f) / 0.6f);
        add(c, x, y, L->halo, a * inten);
        if (r < bR) {
            float w = r / bR;
            if (w < 0.55f) { float t = w / 0.55f; add(c, x, y, mix(WHITE, L->col, t), lit + (0.85f * lit - lit) * t); }
            else if (w < 0.72f) { float t = (w - 0.55f) / 0.17f; add(c, x, y, mix(L->col, L->halo, t), 0.85f * lit + (0.3f * inten - 0.85f * lit) * t); }
            else add(c, x, y, L->halo, 0.3f * inten * (1 - (w - 0.72f) / 0.28f));
        }
        float cov = clamp01(sR + 0.5f - r);
        if (cov > 0) add(c, x, y, L->col, solid * cov);
    }
}

static void ring(Canvas *c, Clip cl, float ox, float oy, float rad, float width, float a) {
    int bx0 = (int)floorf(ox - rad - width), bx1 = (int)ceilf(ox + rad + width) + 1, by0 = (int)floorf(oy - rad - width), by1 = (int)ceilf(oy + rad + width) + 1;
    if (bx0 < cl.x0) bx0 = cl.x0;
    if (by0 < cl.y0) by0 = cl.y0;
    if (bx1 > cl.x1) bx1 = cl.x1;
    if (by1 > cl.y1) by1 = cl.y1;
    for (int y = by0; y < by1; y++) for (int x = bx0; x < bx1; x++) {
        float dx = x + 0.5f - ox, dy = y + 0.5f - oy, cov = clamp01(width * 0.5f + 0.5f - fabsf(sqrtf(dx * dx + dy * dy) - rad));
        if (cov > 0) add(c, x, y, WHITE, a * cov);
    }
}

/* ---- a 3 x 5 pixel font for the labels ------------------------------------------------------- */
static const char *glyph(char ch) {
    switch (toupper((unsigned char)ch)) {
    case '0': return "111101101101111"; case '1': return "010110010010111"; case '2': return "111001111100111";
    case '3': return "111001111001111"; case '4': return "101101111001001"; case '5': return "111100111001111";
    case '6': return "111100111101111"; case '7': return "111001001001001"; case '8': return "111101111101111";
    case '9': return "111101111001111"; case 'A': return "010101111101101"; case 'B': return "110101110101110";
    case 'C': return "111100100100111"; case 'D': return "110101101101110"; case 'E': return "111100110100111";
    case 'F': return "111100110100100"; case 'G': return "111100101101111"; case 'H': return "101101111101101";
    case 'I': return "111010010010111"; case 'J': return "001001001101111"; case 'K': return "101101110101101";
    case 'L': return "100100100100111"; case 'M': return "101111111101101"; case 'N': return "110101101101101";
    case 'O': return "111101101101111"; case 'P': return "111101111100100"; case 'Q': return "111101101111001";
    case 'R': return "111101110101101"; case 'S': return "011100010001110"; case 'T': return "111010010010010";
    case 'U': return "101101101101111"; case 'V': return "101101101101010"; case 'W': return "101101111111101";
    case 'X': return "101101010101101"; case 'Y': return "101101010010010"; case 'Z': return "111001010100111";
    case '-': return "000000111000000"; case '_': return "000000000000111"; case '.': return "000000000000010";
    case '/': return "001001010100100"; case ':': return "000010000010000"; case ' ': return "000000000000000";
    default: return "111001010000010";
    }
}

static void text(Canvas *c, Clip cl, float cx, int y, const char *s, int fs, Col col, float a) {
    int n = (int)strlen(s), maxn = (cl.x1 - cl.x0) / (4 * fs) - 1;
    if (maxn < 1) return;
    if (n > maxn) n = maxn;
    int x = (int)(cx - (n * 4 - 1) * fs / 2.0f);
    for (int i = 0; i < n; i++, x += 4 * fs) {
        const char *g = glyph(s[i]);
        for (int gy = 0; gy < 5; gy++) for (int gx = 0; gx < 3; gx++) {
            if (g[gy * 3 + gx] != '1') continue;
            for (int yy = 0; yy < fs; yy++) for (int xx = 0; xx < fs; xx++) {
                int X = x + gx * fs + xx, Y = y + gy * fs + yy;
                if (X >= cl.x0 && X < cl.x1 && Y >= cl.y0 && Y < cl.y1) add(c, X, Y, col, a);
            }
        }
    }
}

/* ---- the shell, as the motion system has moved it --------------------------------------------- */
typedef struct { float M[9], T[9], ox, oy, unit, tall, st, ux, uy; } Proj;
static void project(const Proj *P, const float *p, float *out) {
    float w[3];
    m3_app(w, P->M, p);
    float pp = CAM / (CAM - w[2]);
    float dx = w[0] * P->unit * pp * (1 - 0.5f * P->tall), dy = w[1] * P->unit * pp * (1 + P->tall);
    float par = (dx * P->ux + dy * P->uy) * (1 + P->st), per = (-dx * P->uy + dy * P->ux) * (1 - P->st * 0.5f);
    out[0] = P->ox + par * P->ux - per * P->uy; out[1] = P->oy + par * P->uy + per * P->ux; out[2] = w[2];
}
static int moved(const Motion *mo, int t) { return fabsf(mo->off[t]) > 0.002f || mo->ang[t] != 0; }
static int same_tri(const Motion *mo, int a, int b) {
    if (mo->ang[a] != mo->ang[b]) return 0;
    if (fabsf(mo->off[a]) < 0.002f && fabsf(mo->off[b]) < 0.002f) return 1;
    return mo->grp[a] == mo->grp[b] && fabsf(mo->off[a] - mo->off[b]) < 0.002f;
}
typedef struct { int n; float ang[4], R[4][9]; } RotCache; /* a frame has only a few distinct layer turns */
static const float *layer_rot(RotCache *rc, const Motion *mo, float a) {
    for (int i = 0; i < rc->n; i++) if (rc->ang[i] == a) return rc->R[i];
    int i = rc->n < 4 ? rc->n++ : 3;
    rc->ang[i] = a; m3_rot(rc->R[i], mo->lax, a);
    return rc->R[i];
}
/* where node v is drawn as part of triangle t: slid out with its patch, and turned with its layer */
static void at_tri(const Proj *P, const Motion *mo, const Shape *sh, float base[][3], int v, int t, RotCache *rc, float *out) {
    float p[3];
    const float *n = mo->gn[mo->grp[t]];
    float o = mo->off[t] * CIRCUM;
    p[0] = base[v][0] + n[0] * o; p[1] = base[v][1] + n[1] * o; p[2] = base[v][2] + n[2] * o;
    if (mo->ang[t] != 0) m3_app(p, layer_rot(rc, mo, mo->ang[t]), p);
    (void)sh;
    project(P, p, out);
}
static void draw_edge(Canvas *c, Clip cl, const float *a, const float *b, const Look *L, float edge, float thin, float halo, float gg, float k) {
    float depth = 0.3f + 0.7f * ((a[2] + b[2]) * 0.5f + CIRCUM) / (2 * CIRCUM); /* far edges are fainter */
    float al = depth * edge * (1 + gg);
    glow_line(c, cl, a[0], a[1], b[0], b[1], mix(L->line, WHITE, gg * 0.6f), thin * al, L->eh, halo * al, k);
}
static const float (*icosa(void))[3] { /* the 12 corners of an icosahedron on the shell's sphere */
    static float v[12][3];
    static int made;
    if (!made) {
        const float P = 1.6180339887f, l = sqrtf(1 + P * P) / CIRCUM;
        int n = 0;
        for (int a = -1; a <= 1; a += 2) for (int b = -1; b <= 1; b += 2) {
            v[n][0] = 0; v[n][1] = a / l; v[n][2] = b * P / l; n++;
            v[n][0] = a / l; v[n][1] = b * P / l; v[n][2] = 0; n++;
            v[n][0] = b * P / l; v[n][1] = 0; v[n][2] = a / l; n++;
        }
        made = 1;
    }
    return (const float (*)[3])v;
}

static void draw_session(Canvas *c, Clip cl, const Session *s, const Shape *sh, double now, float glow, float core_frac, int label) {
    double t = now - s->created + s->phase; /* small numbers keep sin() precise */
    Look lit_look = s->cur; /* pointed at: a brighter shell */
    lit_look.edge *= 1 + HOVER.bright * 0.6f * s->hover;
    lit_look.edge *= 1 + CARRY.lift * s->carry + 0.5f * s->shake; /* carried: lit up, more so when shaken */
    float wb = s->w_bright > 0 ? s->w_bright : 1, ws = s->w_scale > 0 ? s->w_scale : 1; /* a whim (0 = never ticked) */
    lit_look.edge *= 0.4f + 0.6f * wb;
    lit_look.hr *= 1 + lit_look.lh * s->level * 0.15f; /* the glow grows with the microphone level */
    float flash = 0, thick = 1;
    if (s->unread && !s->overlay) { /* an unread reply: a slow, soft flash, as if waiting for leave to report */
        flash = TOUCH.unread_amp * (0.5f - 0.5f * (float)cos(t * TOUCH.unread_hz * 6.283185));
        lit_look.edge *= 1 + flash * 0.5f;
    }
    float vf = (float)(1 - (now - s->vol_at) / TOUCH.vol_for); /* after a scroll the shell shows the volume */
    if (vf > 0) { thick = 1 + vf * (0.3f + 1.7f * s->vol - 1); lit_look.edge *= 1 + vf * (0.4f + 0.9f * s->vol - 1); }
    if (s->aura_a > 0.01f) { /* single-core mode: the core takes on this session's colour, quietly; the shell stays */
        lit_look.halo = mix(lit_look.halo, s->aura, s->aura_a);
        lit_look.col = mix(lit_look.col, s->aura, s->aura_a * 0.45f);
    }
    const Look *L = &lit_look;
    int tw = cl.x1 - cl.x0, th = cl.y1 - cl.y0;
    int fs = tw / 120 < 1 ? 1 : tw / 120, label_h = fs * 16;
    if (!label || th - label_h < 12) label_h = 0;
    float m = fminf((float)tw, (float)(th - label_h));
    float unit = m * SHELL_FRAC / CIRCUM * ws, k = m / 600.0f;
    float ox = cl.x0 + tw * 0.5f + s->w_off_x * CIRCUM * unit, oy = cl.y0 + (th - label_h) * 0.5f + s->w_off_y * CIRCUM * unit;

    /* the core */
    float gl = fminf(1, L->glitch + s->shake * s->shake * CARRY.shake_glitch + (motion_crazy(s, now) ? 0.9f : 0));
    float breathe = (1 + L->depth * (float)sin(t * L->hz * 6.283185)) * (1 - gl) + glitch_val(t) * gl;
    float flick = 1 + 0.035f * (float)sin(t * 17.3) + 0.025f * (float)sin(t * 29.1 + 1.7);
    float burst = (float)(1 - (now - s->burst_at) / 1.4);
    if (burst < 0) burst = 0;
    float voice = (1 - L->lc) + L->lc * (0.35f + 1.3f * s->level); /* speaking: brightness follows the voice */
    float inten = L->core * breathe * flick * glow * (1 + HOVER.bright * s->hover) * (1 + flash) * voice * (1 + CARRY.lift * s->carry) * wb + burst * 0.8f;
    float rc = core_frac * INRADIUS * unit;
    float gx = ox + (s->look_x * HOVER.gaze + s->jig_x) * CIRCUM * unit, gy = oy + (s->look_y * HOVER.gaze + s->jig_y) * CIRCUM * unit; /* the core looks at the cursor, and sloshes when carried */
    draw_core(c, cl, gx, gy, rc, L, inten);
    if (burst > 0) ring(c, cl, ox, oy, rc + (1 - burst) * m * 0.6f, (1 + burst * 3) * (k > 0.5f ? k : 0.5f) * 2, burst * 0.6f);
    float wave = (float)(1 - (now - s->ring_at) / TOUCH.ring_for); /* double click: one shockwave ring, no flare */
    if (wave > 0 && wave <= 1) ring(c, cl, ox, oy, rc + (1 - wave) * m * 0.55f, (1 + wave * 2) * (k > 0.5f ? k : 0.5f) * 2, wave * 0.55f);

    /* the shell: the snap turns (mo.q), the drift (ax, ay), then turned toward the cursor or into the motion */
    const Motion *mo = &s->mo;
    Proj P;
    {
        float E[9], Ty[9], Tx[9], Y[3] = {0, 1, 0}, X[3] = {1, 0, 0};
        float ty = HOVER.tilt * s->look_x + s->lean_x + s->w_lean_x, tx = HOVER.tilt * s->look_y + s->lean_y + s->w_lean_y;
        m3_euler(E, s->ax, s->ay);
        m3_rot(Ty, Y, ty);
        m3_rot(Tx, X, -tx);
        m3_mul(P.T, Tx, Ty);
        m3_mul(P.M, E, mo->q);
        m3_mul(P.M, P.T, P.M);
    }
    float lm = hypotf(s->lean_x, s->lean_y); /* stretched along the motion when carried */
    P.st = lm / CARRY.lean_max * CARRY.stretch;
    P.ux = lm > 1e-4f ? s->lean_x / lm : 1; P.uy = lm > 1e-4f ? s->lean_y / lm : 0;
    P.ox = ox; P.oy = oy; P.unit = unit * (mo->scale > 0 ? mo->scale : 1); P.tall = s->w_tall;
    /* each node, pushed out or in by a spike */
    float base[MAX_V][3], q[MAX_V][3];
    for (int i = 0; i < sh->nv; i++) {
        float r = 1 + mo->push[i];
        base[i][0] = sh->v[i][0] * r; base[i][1] = sh->v[i][1] * r; base[i][2] = sh->v[i][2] * r;
        project(&P, base[i], q[i]);
    }
    int crazy = motion_crazy(s, now), any = 0;
    for (int t = 0; t < sh->nt && !any; t++) any = moved(mo, t);
    float strobe = crazy ? (sinf((float)t * 90.0f) > 0 ? 0.45f : 1.0f) : 1.0f; /* going crazy: the threads stutter */
    float edge = L->edge * strobe, thin = 0.9f * (sh->df > 0.6f ? sh->df : 0.6f) * thick;
    RotCache rc_ = {0};
    for (int i = 0; i < sh->ne; i++) {
        int a = sh->e[i][0], b = sh->e[i][1], t1 = sh->et[i][0], t2 = sh->et[i][1];
        float gg = (mo->glow[a] + mo->glow[b]) * 0.5f, pa[3], pb[3];
        if (!any || t1 < 0) { draw_edge(c, cl, q[a], q[b], L, edge, thin, glow * sh->df, gg, k); continue; }
        /* an edge between two triangles that moved apart is drawn on each of them */
        at_tri(&P, mo, sh, base, a, t1, &rc_, pa); at_tri(&P, mo, sh, base, b, t1, &rc_, pb);
        draw_edge(c, cl, pa, pb, L, edge, thin, glow * sh->df, gg, k);
        if (t2 >= 0 && !same_tri(mo, t1, t2)) {
            at_tri(&P, mo, sh, base, a, t2, &rc_, pa); at_tri(&P, mo, sh, base, b, t2, &rc_, pb);
            draw_edge(c, cl, pa, pb, L, edge, thin, glow * sh->df, gg, k);
        }
    }
    for (int i = 0; i < sh->nv; i++) { /* the nodes: one dot for each place a node is drawn */
        int seen[MAX_DEG], ns = 0, nt = any ? sh->nvt[i] : 0;
        for (int j = 0; j <= nt; j++) {
            float p[3];
            if (!nt) { if (j) break; memcpy(p, q[i], sizeof p); }
            else {
                if (j == nt) break;
                int tt = sh->vt[i][j], dup = 0;
                for (int u = 0; u < ns; u++) if (same_tri(mo, seen[u], tt)) dup = 1;
                if (dup) continue;
                seen[ns++] = tt;
                at_tri(&P, mo, sh, base, i, tt, &rc_, p);
            }
            float d = 0.3f + 0.7f * (p[2] + CIRCUM) / (2 * CIRCUM), gg = mo->glow[i];
            if (k > 0.3f) blob(c, cl, p[0], p[1], 5 * k * (1 + gg), L->eh, (0.3f * d * edge * glow * sh->df) * (1 + 2 * gg), L->eh, 0.1f * d * edge * glow * sh->df, 0.5f);
            disc(c, cl, p[0], p[1], (k * 1.5f > 0.6f ? k * 1.5f : 0.6f) * (1 + gg), mix(L->line, WHITE, gg), clamp01(0.95f * d * edge * (k > 0.3f ? 1 : 0.5f) + gg));
        }
    }
    if (mo->inner > 0.01f) { /* the inner shell: a small icosahedron turning against the outer one */
        float R[9], M2[9], ax[3] = {0.28f, 0.94f, 0.19f}, pts[12][3];
        m3_rot(R, ax, mo->inner_ang);
        m3_mul(M2, P.T, R);
        Proj PI = P; memcpy(PI.M, M2, sizeof M2);
        const float (*iv)[3] = icosa();
        for (int i = 0; i < 12; i++) { float v[3] = {iv[i][0] * 0.42f, iv[i][1] * 0.42f, iv[i][2] * 0.42f}; project(&PI, v, pts[i]); }
        for (int i = 0; i < 12; i++) for (int j = i + 1; j < 12; j++) {
            float dx = iv[i][0] - iv[j][0], dy = iv[i][1] - iv[j][1], dz = iv[i][2] - iv[j][2];
            if (dx * dx + dy * dy + dz * dz > 3.9f) continue; /* only the 30 edges (edge length² is 3.3 on this sphere, the next distance 8.7) */
            glow_line(c, cl, pts[i][0], pts[i][1], pts[j][0], pts[j][1], L->eh, 0.5f * mo->inner * edge, L->eh, 0.4f * mo->inner * glow * edge, k);
        }
    }
    if (mo->scan_a > 0.01f) { /* the scan plane, seen edge-on: a flat ellipse at its height */
        float h = mo->scan_h, rx = sqrtf(fmaxf(0, 1.04f - h * h)) * CIRCUM * P.unit, ry = rx * 0.14f, cy = oy + h * CIRCUM * P.unit;
        for (int i = 0; i < 40; i++) {
            float a0 = (float)i / 40 * 6.2831853f, a1 = (float)(i + 1) / 40 * 6.2831853f;
            glow_line(c, cl, ox + rx * cosf(a0), cy + ry * sinf(a0), ox + rx * cosf(a1), cy + ry * sinf(a1), L->eh, 0.35f * mo->scan_a, L->eh, 0.15f * mo->scan_a * glow, k);
        }
    }
    /* pulses running along the edges */
    if (L->flow > 0.03f) {
        float pr = 9 * (k > 0.25f ? k : 0.25f);
        for (int i = 0; i < NPULSE; i++) {
            const Pulse *p = &s->p[i];
            if (p->e >= sh->ne) continue;
            const float *a = q[sh->e[p->e][p->dir > 0 ? 0 : 1]], *b = q[sh->e[p->e][p->dir > 0 ? 1 : 0]];
            blob(c, cl, a[0] + (b[0] - a[0]) * p->t, a[1] + (b[1] - a[1]) * p->t, pr, WHITE, 0.95f * L->flow, L->halo, 0.4f * L->flow * glow, 0.3f);
        }
    }

    /* the label: folder name, then state (and tool) */
    if (label_h) {
        char line[80];
        int st = session_shown(s, now), y = cl.y1 - label_h + fs * 2;
        text(c, cl, ox, y, s->name[0] ? s->name : "CLAUDE", fs, WHITE, 0.8f);
        if (st == ST_WORK && s->tool[0]) snprintf(line, sizeof line, "%s %s", STATE_NAME[st], s->tool);
        else snprintf(line, sizeof line, "%s", STATE_NAME[st]);
        text(c, cl, ox, y + fs * 7, line, fs, L->eh, 0.85f);
    }
}

void render_room(Canvas *c, const Room *r, const Shape *sh, double now, float glow, float core_frac) {
    memset(c->p, 0, sizeof(float) * 3 * (size_t)c->w * (size_t)c->h);
    int n = r->n;
    if (n < 1) return;
    /* the grid whose tiles come out largest */
    int best = 1;
    float best_side = 0;
    for (int cols = 1; cols <= n; cols++) {
        int rows = (n + cols - 1) / cols;
        float side = fminf((float)c->w / cols, (float)c->h / rows);
        if (side > best_side) { best_side = side; best = cols; }
    }
    int cols = best, rows = (n + cols - 1) / cols;
    for (int i = 0; i < n; i++) {
        int gx = i % cols, gy = i / cols, in_row = gy == rows - 1 ? n - gy * cols : cols;
        float tw = (float)c->w / cols, th = (float)c->h / rows, off = (cols - in_row) * tw * 0.5f; /* a short last row is centred */
        Clip cl = {(int)(off + gx * tw), (int)(gy * th), (int)(off + (gx + 1) * tw), (int)((gy + 1) * th)};
        draw_session(c, cl, &r->s[i], sh, now, glow, core_frac, 1);
    }
}

/* one session filling the whole canvas, without the drawn label (the list layout writes real text instead) */
void render_one(Canvas *c, const Session *s, const Shape *sh, double now, float glow, float core_frac) {
    memset(c->p, 0, sizeof(float) * 3 * (size_t)c->w * (size_t)c->h);
    draw_session(c, (Clip){0, 0, c->w, c->h}, s, sh, now, glow, core_frac, 0);
}

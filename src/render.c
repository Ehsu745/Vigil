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
 * glow lies over it. The colour is at most alpha, which is what "premultiplied" asks for.
 * The faint outer glow spans only a few of the 256 steps, so plain rounding draws it as rings; a 4×4 ordered
 * dither (the same offset for all three channels, so alpha stays the brightest of them) turns the steps into
 * a smooth fall-off. */
void canvas_rgba(const Canvas *c, uint8_t *rgba) {
    static const float BAYER[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
    for (int y = 0; y < c->h; y++) {
        const float *p = c->p + (size_t)y * c->w * 3;
        uint8_t *o = rgba + (size_t)y * c->w * 4;
        const float *row = BAYER + (y & 3) * 4;
        for (int x = 0; x < c->w; x++, p += 3, o += 4) {
            float d = (row[x & 3] + 0.5f) / 16.0f, v[3];
            for (int k = 0; k < 3; k++) {
                float q = p[k] * 255.0f + d;
                v[k] = q < 0 ? 0 : q > 255 ? 255 : q;
            }
            uint8_t r = (uint8_t)v[0], g = (uint8_t)v[1], b = (uint8_t)v[2];
            o[0] = r; o[1] = g; o[2] = b;
            o[3] = r > g ? (r > b ? r : b) : (g > b ? g : b);
        }
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

static void ring_col(Canvas *c, Clip cl, float ox, float oy, float rad, float width, Col col, float a) {
    int bx0 = (int)floorf(ox - rad - width), bx1 = (int)ceilf(ox + rad + width) + 1, by0 = (int)floorf(oy - rad - width), by1 = (int)ceilf(oy + rad + width) + 1;
    if (bx0 < cl.x0) bx0 = cl.x0;
    if (by0 < cl.y0) by0 = cl.y0;
    if (bx1 > cl.x1) bx1 = cl.x1;
    if (by1 > cl.y1) by1 = cl.y1;
    for (int y = by0; y < by1; y++) for (int x = bx0; x < bx1; x++) {
        float dx = x + 0.5f - ox, dy = y + 0.5f - oy, cov = clamp01(width * 0.5f + 0.5f - fabsf(sqrtf(dx * dx + dy * dy) - rad));
        if (cov > 0) add(c, x, y, col, a * cov);
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
/* a point already in view space (sparks): only the pointer tilt and the perspective are left */
static void project_view(const Proj *P, const float *p, float *out) {
    Proj Q = *P;
    memcpy(Q.M, P->T, sizeof Q.M);
    project(&Q, p, out);
}
static float alpha_of(const Motion *mo, int t) { return mo->ta_on ? mo->talpha[t] : 1; }
static int carried_off(const Motion *mo, int t) { return mo->tt_on && (mo->tt[t][0] != 0 || mo->tt[t][1] != 0 || mo->tt[t][2] != 0); }
static int moved(const Motion *mo, int t) {
    return fabsf(mo->off[t]) > 0.002f || mo->ang[t] != 0 || mo->tmi[t] || carried_off(mo, t) || alpha_of(mo, t) != 1 || (mo->sharp_on && mo->sharp_k[t] > 0.001f);
}
static int same_tri(const Motion *mo, int a, int b) {
    if (mo->ang[a] != mo->ang[b] || mo->tmi[a] != mo->tmi[b] || alpha_of(mo, a) != alpha_of(mo, b)) return 0;
    if (mo->tt_on && (mo->tt[a][0] != mo->tt[b][0] || mo->tt[a][1] != mo->tt[b][1] || mo->tt[a][2] != mo->tt[b][2])) return 0;
    if (mo->sharp_on && (mo->sharp_k[a] > 0.001f || mo->sharp_k[b] > 0.001f)) return 0;
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
/* where node v is drawn as part of triangle t: a blade's tip drawn out, slid out with its patch, turned with
 * its layer or by its own matrix, then carried off whole */
static void at_tri(const Proj *P, const Motion *mo, const Shape *sh, float base[][3], int v, int t, RotCache *rc, float *out) {
    float p[3], bv[3] = {base[v][0], base[v][1], base[v][2]};
    if (mo->sharp_on && mo->sharp_k[t] > 0.001f && mo->sharp_v[t] >= 0) {
        float k = mo->sharp_k[t], c0[3], ax[3], d[3];
        const int *tv = sh->t[t];
        for (int i = 0; i < 3; i++) c0[i] = (base[tv[0]][i] + base[tv[1]][i] + base[tv[2]][i]) / 3;
        for (int i = 0; i < 3; i++) { ax[i] = base[mo->sharp_v[t]][i] - c0[i]; d[i] = bv[i] - c0[i]; }
        float l = sqrtf(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
        if (l > 1e-6f) {
            for (int i = 0; i < 3; i++) ax[i] /= l;
            float al = d[0] * ax[0] + d[1] * ax[1] + d[2] * ax[2], fa = v == mo->sharp_v[t] ? 1 + 0.75f * k : 1, fp = 1 - 0.3f * k;
            for (int i = 0; i < 3; i++) bv[i] = c0[i] + ax[i] * al * fa + (d[i] - ax[i] * al) * fp;
        }
    }
    const float *n = mo->gn[mo->grp[t]];
    float o = mo->off[t] * CIRCUM;
    p[0] = bv[0] + n[0] * o; p[1] = bv[1] + n[1] * o; p[2] = bv[2] + n[2] * o;
    if (mo->tmi[t]) m3_app(p, mo->tmx[mo->tmi[t] - 1], p);
    else if (mo->ang[t] != 0) m3_app(p, layer_rot(rc, mo, mo->ang[t]), p);
    if (mo->tt_on) { p[0] += mo->tt[t][0]; p[1] += mo->tt[t][1]; p[2] += mo->tt[t][2]; }
    project(P, p, out);
}

/* ---- sparks, light trails, the black hole ------------------------------------------------------ */
static void draw_sparks(Canvas *c, Clip cl, const Proj *P, const Motion *mo, const Look *L, float ks) {
    for (int i = 0; i < MAX_SPARK; i++) {
        const Spark *q = &mo->spark[i];
        if (q->life <= 0) continue;
        float a = clamp01(q->life / q->max), age = 1 - a, h[3];
        project_view(P, q->p, h);
        Col c0 = q->own ? L->halo : q->col, col = c0;
        if (q->hot) col = age < 0.18f ? mix((Col){1, 0.98f, 0.925f}, c0, age / 0.18f) : mix(c0, (Col){0.43f, 0.11f, 0.03f}, clamp01((age - 0.18f) / 0.82f) * 0.75f);
        if (q->kind == SK_STREAK) { /* a short motion-blurred line with a white-hot head */
            float sp = sqrtf(q->v[0] * q->v[0] + q->v[1] * q->v[1] + q->v[2] * q->v[2]) / CIRCUM, tl = fminf(0.09f, 0.025f + sp * 0.022f), tp[3], mp[3];
            float tail[3] = {q->p[0] - q->v[0] * tl, q->p[1] - q->v[1] * tl, q->p[2] - q->v[2] * tl};
            project_view(P, tail, tp);
            mp[0] = (tp[0] + h[0]) / 2; mp[1] = (tp[1] + h[1]) / 2;
            float kk = ks * q->size * 0.35f;
            glow_line(c, cl, tp[0], tp[1], mp[0], mp[1], col, 0.3f * a, col, 0.25f * a, kk);
            glow_line(c, cl, mp[0], mp[1], h[0], h[1], col, a, col, 0.6f * a, kk);
            disc(c, cl, h[0], h[1], ks * q->size * (0.35f + 0.45f * a), mix(col, WHITE, 0.65f), 0.9f * a);
        } else if (q->kind == SK_FLARE) { /* a flash that swells and fades */
            float u = clamp01(age), fr = q->size * ks * (0.5f + 2.2f * (1 - (1 - u) * (1 - u) * (1 - u)));
            blob(c, cl, h[0], h[1], fr, WHITE, a * a, col, 0.65f * a * a, 0.22f);
        } else { /* an ember flickers, a mote drifts */
            float fl = q->kind == SK_EMBER ? 0.5f + 0.5f * fabsf(sinf(q->life * 23 + q->seed)) : 0.75f + 0.25f * sinf(q->life * 3 + q->seed);
            float mr = q->size * ks * (q->kind == SK_MOTE ? 3.2f : 2.2f);
            blob(c, cl, h[0], h[1], mr, mix(col, WHITE, 0.5f), a * fl, col, 0.5f * a * fl, 0.3f);
        }
    }
}
static void draw_trail(Canvas *c, Clip cl, const Proj *P, const Motion *mo, float k) {
    int n = mo->ntrail;
    if (!mo->trail_on || n < 2) return;
    float pts[MAX_TRAIL][3];
    for (int i = 0; i < n; i++) { float p[3] = {mo->trail[i][0] * CIRCUM, mo->trail[i][1] * CIRCUM, mo->trail[i][2] * CIRCUM}; project(P, p, pts[i]); }
    Col tc = mo->trail_col;
    for (int i = 1; i < n; i++) { /* a tapered glowing ribbon, brightest at the head */
        float f = (float)i / (n - 1);
        glow_line(c, cl, pts[i - 1][0], pts[i - 1][1], pts[i][0], pts[i][1], mix(tc, WHITE, f * 0.5f), 0.9f * f * f, tc, 0.8f * f, k * (0.6f + 0.9f * f));
    }
    blob(c, cl, pts[n - 1][0], pts[n - 1][1], 14 * k, WHITE, 0.95f, tc, 0.55f, 0.25f);
}
/* the void, its dust disc, nebula light and a few stars, drawn over the collapsed shell */
static void draw_hole(Canvas *c, Clip cl, const Motion *mo, float ox, float oy, float R, float ks, double t) {
    float hb = mo->hole, rh = R * 0.3f * (1 + 0.25f * hb), tilt = 0.36f, rotA = -0.42f, ca = cosf(rotA), sa = sinf(rotA);
    const Col VIOLET = {0.56f, 0.32f, 1}, MAGENTA = {1, 0.3f, 0.82f}, PALE = {0.86f, 0.8f, 1}, DEEP = {0.28f, 0.16f, 0.7f};
    for (int i = 0; i < 3; i++) { /* nebula light, slowly drifting round */
        float a = (float)t * 0.05f + i * 2.1f;
        Col col = i == 1 ? MAGENTA : i == 2 ? DEEP : VIOLET;
        blob(c, cl, ox + cosf(a) * rh * 0.8f, oy + sinf(a) * rh * 0.6f, rh * (1.9f + 0.3f * i), col, 0.13f * hb, col, 0.12f * hb, 0.02f); /* (kept inside the tile) */
    }
    for (int front = 0; front < 2; front++) {
        if (front) { /* the void (light taken away: see-through on the desktop), then its violet haze */
            float vr = rh * 1.3f;
            int bx0 = (int)fmaxf(cl.x0, floorf(ox - rh * 1.6f)), bx1 = (int)fminf(cl.x1, ceilf(ox + rh * 1.6f) + 1);
            int by0 = (int)fmaxf(cl.y0, floorf(oy - rh * 1.6f)), by1 = (int)fminf(cl.y1, ceilf(oy + rh * 1.6f) + 1);
            for (int y = by0; y < by1; y++) for (int x = bx0; x < bx1; x++) {
                float dx = x + 0.5f - ox, dy = y + 0.5f - oy, r = sqrtf(dx * dx + dy * dy), u = r / vr;
                float *p = c->p + 3 * ((size_t)y * (size_t)c->w + (size_t)x);
                if (u < 1) {
                    float a = u < 0.62f ? hb : u < 0.85f ? hb * (1 - 0.4f * (u - 0.62f) / 0.23f) : 0.6f * hb * (1 - (u - 0.85f) / 0.15f);
                    p[0] *= 1 - a; p[1] *= 1 - a; p[2] *= 1 - a;
                }
                float hu = (r - rh * 0.8f) / (rh * 0.8f); /* 0 at 0.8 rh, 1 at 1.6 rh */
                if (hu > 0 && hu < 1) { float ha = 0.14f * hb * (hu < 0.35f ? hu / 0.35f : (1 - hu) / 0.65f); p[0] += VIOLET.r * ha; p[1] += VIOLET.g * ha; p[2] += VIOLET.b * ha; }
            }
        }
        for (int i = 0; i < NDUST; i++) { /* the far side of the disc behind the void, the near side in front */
            const Dust *d = &mo->rs.hole.dust[i];
            float sn = sinf(d->a);
            if ((sn > 0) != front) continue;
            float x = cosf(d->a) * d->r * rh, y = sn * d->r * rh * tilt;
            float al = hb * 0.8f * clamp01((3.2f - d->r) / 1.2f) * (front ? 0.9f : 0.55f);
            Col col = d->r < 1.6f ? mix(PALE, VIOLET, clamp01((d->r - 1.15f) / 0.45f)) : mix(VIOLET, MAGENTA, clamp01((d->r - 1.6f) / 1.2f));
            disc(c, cl, ox + x * ca - y * sa, oy + x * sa + y * ca, d->s * ks * (front ? 1.3f : 1), col, al);
        }
    }
    for (int i = 0; i < NSTAR; i++) { /* a few faint stars, holding still */
        const float *st = mo->rs.hole.star[i];
        float x = st[0] * R, y = st[1] * R;
        if (sqrtf(x * x + y * y) < rh * 2) continue;
        disc(c, cl, ox + x, oy + y, st[2] * 0.8f * ks, PALE, hb * (0.4f + 0.1f * sinf((float)t * 0.4f + st[3])) * 0.6f);
    }
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
    float mb = s->mo.bright > 0 ? s->mo.bright : 1; /* a routine's own brightness (a forge blow, a beacon facing you) */
    lit_look.edge *= 0.4f + 0.6f * mb;
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
    float inten = L->core * breathe * flick * glow * (1 + HOVER.bright * s->hover) * (1 + flash) * voice * (1 + CARRY.lift * s->carry) * wb * mb * (1 - 0.85f * s->mo.hole) + burst * 0.8f;
    float rc = core_frac * INRADIUS * unit;
    float gx = ox + (s->look_x * HOVER.gaze + s->jig_x) * CIRCUM * unit, gy = oy + (s->look_y * HOVER.gaze + s->jig_y) * CIRCUM * unit; /* the core looks at the cursor, and sloshes when carried */
    draw_core(c, cl, gx, gy, rc, L, inten);
    if (burst > 0) ring(c, cl, ox, oy, rc + (1 - burst) * m * 0.6f, (1 + burst * 3) * (k > 0.5f ? k : 0.5f) * 2, burst * 0.6f);
    float wave = (float)(1 - (now - s->ring_at) / TOUCH.ring_for); /* double click: one shockwave ring, no flare */
    if (wave > 0 && wave <= 1) ring(c, cl, ox, oy, rc + (1 - wave) * m * 0.55f, (1 + wave * 2) * (k > 0.5f ? k : 0.5f) * 2, wave * 0.55f);
    for (int i = 0; i < MAX_RING; i++) { /* the pulses a routine sends out (a click, a blow, a beacon facing you): thin, */
        const MRing *r = &s->mo.ring[i];  /* from the shell's edge outward, fading as they go, so none stays round the core like a frame */
        if (r->a <= 0) continue;
        float u = clamp01((float)(now - r->at) / 1.1f), e = 1 - (1 - u) * (1 - u) * (1 - u), sr = CIRCUM * unit * s->mo.scale;
        float rad = sr * 1.02f + e * fmaxf(0, m * 0.5f - sr * 1.02f);
        ring_col(c, cl, ox, oy, rad, (0.6f + 0.8f * (1 - u)) * (k > 0.5f ? k : 0.5f) * 2, L->halo, (1 - u) * (1 - u) * 0.45f * r->a);
    }

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
        if (mo->squash > 0) { /* pressed flat along sq_ax, the waist bulging (Compressor) */
            const float *a = mo->sq_ax;
            float al = base[i][0] * a[0] + base[i][1] * a[1] + base[i][2] * a[2];
            for (int j = 0; j < 3; j++) { float along = a[j] * al; base[i][j] = (base[i][j] - along) * (1 + mo->squash * 0.55f) + along * (1 - mo->squash); }
        }
        project(&P, base[i], q[i]);
    }
    int crazy = motion_crazy(s, now), any = 0;
    for (int t = 0; t < sh->nt && !any; t++) any = moved(mo, t);
    float strobe = crazy ? (sinf((float)t * 90.0f) > 0 ? 0.45f : 1.0f) : 1.0f; /* going crazy: the threads stutter */
    float edge = L->edge * strobe, thin = 0.9f * (sh->df > 0.6f ? sh->df : 0.6f) * thick;
    float Rpx = CIRCUM * P.unit, ks = Rpx / 156.0f; /* the mock's pixel sizes, for a shell of this size */
    draw_trail(c, cl, &P, mo, k);
    draw_sparks(c, cl, &P, mo, L, ks);
    RotCache rc_ = {0};
    for (int i = 0; i < sh->ne; i++) {
        int a = sh->e[i][0], b = sh->e[i][1], t1 = sh->et[i][0], t2 = sh->et[i][1];
        float gg = (mo->glow[a] + mo->glow[b]) * 0.5f, pa[3], pb[3];
        if (!any || t1 < 0) { draw_edge(c, cl, q[a], q[b], L, edge, thin, glow * sh->df, gg, k); continue; }
        /* an edge between two triangles that moved apart is drawn on each of them */
        float a1 = alpha_of(mo, t1);
        if (a1 > 0.01f) {
            at_tri(&P, mo, sh, base, a, t1, &rc_, pa); at_tri(&P, mo, sh, base, b, t1, &rc_, pb);
            draw_edge(c, cl, pa, pb, L, edge * a1, thin, glow * sh->df, gg, k);
        }
        if (t2 >= 0 && !same_tri(mo, t1, t2) && alpha_of(mo, t2) > 0.01f) {
            at_tri(&P, mo, sh, base, a, t2, &rc_, pa); at_tri(&P, mo, sh, base, b, t2, &rc_, pb);
            draw_edge(c, cl, pa, pb, L, edge * alpha_of(mo, t2), thin, glow * sh->df, gg, k);
        }
    }
    for (int i = 0; i < sh->nv; i++) { /* the nodes: one dot for each place a node is drawn */
        int seen[MAX_DEG], ns = 0, nt = any ? sh->nvt[i] : 0;
        for (int j = 0; j <= nt; j++) {
            float p[3], al = 1;
            if (!nt) { if (j) break; memcpy(p, q[i], sizeof p); }
            else {
                if (j == nt) break;
                int tt = sh->vt[i][j], dup = 0;
                for (int u = 0; u < ns; u++) if (same_tri(mo, seen[u], tt)) dup = 1;
                if (dup) continue;
                seen[ns++] = tt;
                at_tri(&P, mo, sh, base, i, tt, &rc_, p);
                al = alpha_of(mo, tt);
                if (al < 0.01f) continue;
            }
            float d = 0.3f + 0.7f * (p[2] + CIRCUM) / (2 * CIRCUM), gg = mo->glow[i];
            if (k > 0.3f) blob(c, cl, p[0], p[1], 5 * k * (1 + gg), L->eh, (0.3f * d * edge * glow * sh->df) * (1 + 2 * gg) * al, L->eh, 0.1f * d * edge * glow * sh->df * al, 0.5f);
            disc(c, cl, p[0], p[1], (k * 1.5f > 0.6f ? k * 1.5f : 0.6f) * (1 + gg), mix(L->line, WHITE, gg), clamp01(0.95f * d * edge * (k > 0.3f ? 1 : 0.5f) + gg) * al);
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
    if (mo->hole > 0.01f) draw_hole(c, cl, mo, ox, oy, Rpx, ks, t);
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

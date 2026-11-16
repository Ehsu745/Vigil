/* Software renderer. Everything is added into a float RGB canvas (light adds up, like a screen blend)
 * and clamped at the end. Glow is computed from distance, so there is no blur pass.
 * Sizes are given for a 600-pixel tile and scaled by k = tile / 600. */
#include <stdatomic.h>
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
    free(c->p); free(c->dk);
    c->w = w; c->h = h;
    c->p = calloc(3 * (size_t)w * (size_t)h, sizeof(float));
    c->dk = calloc((size_t)w * (size_t)h, sizeof(float));
    c->bx0 = c->by0 = 0; c->bx1 = w; c->by1 = h; /* all new: whoever shows it takes all of it once */
}
static void canvas_clear(Canvas *c) {
    memset(c->p, 0, sizeof(float) * 3 * (size_t)c->w * (size_t)c->h);
    if (c->dk) memset(c->dk, 0, sizeof(float) * (size_t)c->w * (size_t)c->h);
    c->bx0 = c->by0 = c->bx1 = c->by1 = 0;
}
/* only what was drawn since the last clear (the rest is still clear) */
static void canvas_clear_box(Canvas *c) {
    if (c->bx1 > c->bx0 && c->by1 > c->by0)
        for (int y = c->by0; y < c->by1; y++) {
            memset(c->p + 3 * ((size_t)y * c->w + c->bx0), 0, sizeof(float) * 3 * (size_t)(c->bx1 - c->bx0));
            if (c->dk) memset(c->dk + (size_t)y * c->w + c->bx0, 0, sizeof(float) * (size_t)(c->bx1 - c->bx0));
        }
    c->bx0 = c->by0 = c->bx1 = c->by1 = 0;
}
/* a primitive is about to draw into x0..x1, y0..y1 (already clipped): the canvas's drawn box grows to take it in */
static inline void touch(Canvas *c, int x0, int y0, int x1, int y1) {
    if (x1 <= x0 || y1 <= y0) return;
    if (c->bx1 <= c->bx0 || c->by1 <= c->by0) { c->bx0 = x0; c->by0 = y0; c->bx1 = x1; c->by1 = y1; return; }
    if (x0 < c->bx0) c->bx0 = x0;
    if (y0 < c->by0) c->by0 = y0;
    if (x1 > c->bx1) c->bx1 = x1;
    if (y1 > c->by1) c->by1 = y1;
}

/* the light is laid over the terminal's background colour (a screen blend), so the picture has no black box around it */
void canvas_bytes(const Canvas *c, uint8_t *rgb, Col bg) {
    size_t n = (size_t)c->w * (size_t)c->h * 3;
    float b[3] = {bg.r, bg.g, bg.b};
    for (size_t i = 0; i < n; i++) {
        float p = c->p[i] > 1 ? 1 : c->p[i], d = c->dk ? c->dk[i / 3] : 0, sc = i % 3 == 0 ? SHADE_COL.r : i % 3 == 1 ? SHADE_COL.g : SHADE_COL.b;
        float v = 1 - (1 - (b[i % 3] * (1 - d) + sc * d)) * (1 - p); /* the shade lies over the background first */
        rgb[i] = (uint8_t)(v <= 0 ? 0 : v >= 1 ? 255 : v * 255.0f + 0.5f);
    }
}

/* For a see-through window: alpha is the brightest channel, so dark pixels let what is behind show and the
 * glow lies over it. The colour is at most alpha, which is what "premultiplied" asks for.
 * The faint outer glow spans only a few of the 256 steps, so plain rounding draws it as rings; a 4×4 ordered
 * dither (the same offset for all three channels, so alpha stays the brightest of them) turns the steps into
 * a smooth fall-off. */
void canvas_rgba(const Canvas *c, uint8_t *rgba) { canvas_rgba_box(c, rgba, 0, 0, c->w, c->h); }
void canvas_rgba_box(const Canvas *c, uint8_t *rgba, int x0, int y0, int x1, int y1) {
    static const float BAYER[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > c->w) x1 = c->w;
    if (y1 > c->h) y1 = c->h;
    for (int y = y0; y < y1; y++) {
        const float *p = c->p + ((size_t)y * c->w + x0) * 3;
        uint8_t *o = rgba + ((size_t)y * c->w + x0) * 4;
        const float *row = BAYER + (y & 3) * 4;
        const float *dk = c->dk ? c->dk + (size_t)y * c->w : NULL;
        for (int x = x0; x < x1; x++, p += 3, o += 4) {
            if (p[0] <= 0 && p[1] <= 0 && p[2] <= 0 && (!dk || dk[x] <= 0)) { o[0] = o[1] = o[2] = o[3] = 0; continue; } /* nothing here (most of READY's layer): no dither, no shade */
            float d = (row[x & 3] + 0.5f) / 16.0f, v[3];
            for (int k = 0; k < 3; k++) {
                float q = p[k] * 255.0f + d;
                v[k] = q < 0 ? 0 : q > 255 ? 255 : q;
            }
            uint8_t r = (uint8_t)v[0], g = (uint8_t)v[1], b = (uint8_t)v[2];
            o[0] = r; o[1] = g; o[2] = b;
            o[3] = r > g ? (r > b ? r : b) : (g > b ? g : b);
            if (c->dk && c->dk[(size_t)y * c->w + x] > 0) { /* shade: a dark layer (SHADE_COL) under the light, so the light lies on it over a light desktop */
                float dk = c->dk[(size_t)y * c->w + x], add = (255 - o[3]) * (dk > 1 ? 1 : dk), a = o[3] + add;
                float sc[3] = {SHADE_COL.r, SHADE_COL.g, SHADE_COL.b};
                for (int k = 0; k < 3; k++) { float v = o[k] + sc[k] * add; o[k] = (uint8_t)(v > 255 ? 255 : v); }
                o[3] = (uint8_t)(a > 255 ? 255 : a);
            }
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

/* the halo's fall-off, (0.20 f^8 + 0.05 f^2) with f = 1 - d / pad, by u = d^2 / pad^2 (no square root per pixel) */
#define GLOW_N 1024
static float GLOW_LUT[GLOW_N + 2];
static void glow_lut(void) {
    if (GLOW_LUT[0] > 0) return;
    for (int i = 0; i <= GLOW_N + 1; i++) {
        float u = (float)i / GLOW_N, f = u < 1 ? 1.0f - sqrtf(u) : 0, f2 = f * f, f8 = f2 * f2 * f2 * f2;
        GLOW_LUT[i] = 0.20f * f8 + 0.05f * f2;
    }
}

/* a thin line with a halo: `line` at full strength within half a pixel, then `halo` falling off to `pad`. Each row
 * walks only its stretch of the capsule round the line (not the whole bounding box: READY's bold lines are mostly
 * halo, and that box was most of a frame's time). Alongside the line the distance needs no root; round its ends the
 * halo comes from GLOW_LUT, worked out exactly only near the line, where the table's steps would show */
static void glow_line(Canvas *c, Clip cl, float x0, float y0, float x1, float y1, Col line, float a_line, Col halo, float a_halo, float k) {
    float hw = k > 1 ? 0.5f * k : 0.5f, pad = 11.0f * (k > 0.4f ? k : 0.4f);
    int bx0 = (int)floorf(fminf(x0, x1) - pad), bx1 = (int)ceilf(fmaxf(x0, x1) + pad) + 1;
    int by0 = (int)floorf(fminf(y0, y1) - pad), by1 = (int)ceilf(fmaxf(y0, y1) + pad) + 1;
    if (bx0 < cl.x0) bx0 = cl.x0;
    if (by0 < cl.y0) by0 = cl.y0;
    if (bx1 > cl.x1) bx1 = cl.x1;
    if (by1 > cl.y1) by1 = cl.y1;
    touch(c, bx0, by0, bx1, by1);
    glow_lut();
    float dx = x1 - x0, dy = y1 - y0, rl2 = dx * dx + dy * dy, len2 = rl2 < 1e-6f ? 1e-6f : rl2, len = sqrtf(rl2);
    int band = rl2 > 1e-6f; /* (a line of no length is only its round ends) */
    float pad2 = pad * pad, ipad2 = GLOW_N / pad2, ipad = 1 / pad, ilen2 = 1 / len2, ilen = band ? 1 / len : 0, cr = hw + 0.5f, near2 = fmaxf(cr * cr, pad2 / 64);
    for (int y = by0; y < by1; y++) {
        float py = y + 0.5f - y0, lo = 1e30f, hi = -1e30f; /* the row's stretch of the capsule, in px (x + 0.5 - x0) */
        if (fabsf(py) < pad) { float r = sqrtf(pad2 - py * py); lo = fminf(lo, -r); hi = fmaxf(hi, r); }
        if (fabsf(py - dy) < pad) { float r = sqrtf(pad2 - (py - dy) * (py - dy)); lo = fminf(lo, dx - r); hi = fmaxf(hi, dx + r); }
        if (band) { /* the band: 0 <= px dx + py dy <= len^2 and |px dy - py dx| < pad len */
            float a = -1e30f, b = 1e30f;
            if (fabsf(dx) > 1e-6f) { float u0 = -py * dy / dx, u1 = (rl2 - py * dy) / dx; a = fmaxf(a, fminf(u0, u1)); b = fminf(b, fmaxf(u0, u1)); }
            else if (py * dy < 0 || py * dy > rl2) b = -1e30f;
            if (fabsf(dy) > 1e-6f) { float u0 = (py * dx - pad * len) / dy, u1 = (py * dx + pad * len) / dy; a = fmaxf(a, fminf(u0, u1)); b = fminf(b, fmaxf(u0, u1)); }
            else if (fabsf(py * dx) >= pad * len) b = -1e30f;
            if (a < b) { lo = fminf(lo, a); hi = fmaxf(hi, b); }
        }
        if (lo > hi) continue;
        int xa = (int)floorf(lo + x0 - 0.5f), xb = (int)ceilf(hi + x0 - 0.5f) + 1; /* (a pixel to spare each side: the test below decides) */
        if (xa < bx0) xa = bx0;
        if (xb > bx1) xb = bx1;
        float *p = c->p + 3 * ((size_t)y * (size_t)c->w + (size_t)xa);
        for (int x = xa; x < xb; x++, p += 3) {
            float px = x + 0.5f - x0, h, l = 0;
            float t = (px * dx + py * dy) * ilen2;
            if (band && t > 0 && t < 1) { /* alongside the line: the distance is plain (no root), and so is the fall-off */
                float d = fabsf(px * dy - py * dx) * ilen;
                if (d >= pad) continue;
                float f = 1.0f - d * ipad, f2 = f * f, f4 = f2 * f2;
                h = 0.20f * f4 * f4 + 0.05f * f2;
                if (d < cr) l = clamp01(cr - d) * a_line;
            } else { /* round an end */
                float ex = t <= 0 ? px : px - dx, ey = t <= 0 ? py : py - dy, d2 = ex * ex + ey * ey;
                if (d2 >= pad2) continue;
                if (d2 < near2) { /* near the line: exact */
                    float d = sqrtf(d2), f = 1.0f - d / pad, f2 = f * f, f8 = f2 * f2 * f2 * f2;
                    h = 0.20f * f8 + 0.05f * f2;
                    l = clamp01(cr - d) * a_line;
                } else {
                    float u = d2 * ipad2;
                    int i = (int)u;
                    h = GLOW_LUT[i] + (GLOW_LUT[i + 1] - GLOW_LUT[i]) * (u - i);
                }
            }
            h *= a_halo;
            p[0] += line.r * l + halo.r * h; p[1] += line.g * l + halo.g * h; p[2] += line.b * l + halo.b * h;
        }
    }
}

/* a small round light: `in` at the centre, `out` at `mid` of the radius, nothing at the edge. Each row walks only its
 * chord of the circle; a big one (a hit's flash: hundreds of pixels across) reads its light from a table by (r / rad)²
 * built for it, so no root per pixel, worked out exactly only near the centre where the table's steps would show */
#define BLOB_N 512
static void blob(Canvas *c, Clip cl, float cx, float cy, float rad, Col in, float a_in, Col out, float a_out, float mid) {
    if (!(rad > 0)) return;
    int bx0 = (int)floorf(cx - rad), bx1 = (int)ceilf(cx + rad) + 1, by0 = (int)floorf(cy - rad), by1 = (int)ceilf(cy + rad) + 1;
    if (bx0 < cl.x0) bx0 = cl.x0;
    if (by0 < cl.y0) by0 = cl.y0;
    if (bx1 > cl.x1) bx1 = cl.x1;
    if (by1 > cl.y1) by1 = cl.y1;
    touch(c, bx0, by0, bx1, by1);
    if (bx0 >= bx1 || by0 >= by1) return;
    float ir2 = 1 / (rad * rad), rad2 = rad * rad, tab[BLOB_N + 2][3];
    int big = rad > 24;
    if (big) /* the light (colour × strength) at (r / rad)² = i / BLOB_N */
        for (int i = 0; i <= BLOB_N + 1; i++) {
            float u = i < BLOB_N ? sqrtf((float)i / BLOB_N) : 1;
            Col col; float a;
            if (u < mid) { float t = u / mid; col = mix(in, out, t); a = a_in + (a_out - a_in) * t; }
            else { col = out; a = a_out * (1 - (u - mid) / (1 - mid)); }
            tab[i][0] = col.r * a; tab[i][1] = col.g * a; tab[i][2] = col.b * a;
        }
    for (int y = by0; y < by1; y++) {
        float dy = y + 0.5f - cy, h2 = rad2 - dy * dy;
        if (h2 <= 0) continue;
        float hw = sqrtf(h2);
        int xa = (int)floorf(cx - hw - 0.5f), xb = (int)ceilf(cx + hw - 0.5f) + 1; /* (a pixel to spare: the test below decides) */
        if (xa < bx0) xa = bx0;
        if (xb > bx1) xb = bx1;
        for (int x = xa; x < xb; x++) {
            float dx = x + 0.5f - cx, u2 = (dx * dx + dy * dy) * ir2;
            if (u2 >= 1) continue;
            if (big && u2 >= 1.0f / 256) {
                float q = u2 * BLOB_N, w;
                int i = (int)q;
                w = q - i;
                float *p = c->p + 3 * ((size_t)y * (size_t)c->w + (size_t)x);
                p[0] += tab[i][0] + (tab[i + 1][0] - tab[i][0]) * w; p[1] += tab[i][1] + (tab[i + 1][1] - tab[i][1]) * w; p[2] += tab[i][2] + (tab[i + 1][2] - tab[i][2]) * w;
                continue;
            }
            float u = sqrtf(u2);
            if (u < mid) { float t = u / mid; add(c, x, y, mix(in, out, t), a_in + (a_out - a_in) * t); }
            else add(c, x, y, out, a_out * (1 - (u - mid) / (1 - mid)));
        }
    }
}

static void disc(Canvas *c, Clip cl, float cx, float cy, float rad, Col col, float a) {
    int bx0 = (int)floorf(cx - rad - 1), bx1 = (int)ceilf(cx + rad + 1) + 1, by0 = (int)floorf(cy - rad - 1), by1 = (int)ceilf(cy + rad + 1) + 1;
    if (bx0 < cl.x0) bx0 = cl.x0;
    if (by0 < cl.y0) by0 = cl.y0;
    if (bx1 > cl.x1) bx1 = cl.x1;
    if (by1 > cl.y1) by1 = cl.y1;
    touch(c, bx0, by0, bx1, by1);
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
/* light round the shell, so its outline is not a hard edge: rising from nothing at half the shell's radius R to its full
 * at R, then easing to nothing at rmax (inside the picture). Smooth everywhere, so it shows no ring of its own. Painted
 * by draw_core in the same pass as the core's glow (they cover nearly the same pixels). */
typedef struct { float ox, oy, r2max, in2; Col col; float lut[512]; } Aura;
enum { AURA_N = 512 };
static void aura_make(Aura *au, float ox, float oy, float R, float rmax, Col col, float a) {
    au->ox = ox; au->oy = oy; au->col = col;
    if (a <= 0.001f || rmax <= R) { au->r2max = au->in2 = 0; return; }
    au->r2max = rmax * rmax; au->in2 = 0.25f * R * R;
    for (int i = 0; i < AURA_N; i++) { /* the curve against (r / rmax)² */
        float r = rmax * sqrtf((float)i / (AURA_N - 1)), v;
        if (r <= 0.5f * R) v = 0;
        else if (r < R) { float t = (r - 0.5f * R) / (0.5f * R); v = t * t * (3 - 2 * t); }
        else { float t = (r - R) / (rmax - R), w = 1 - t; v = w * w * w * (w * (w * 6 - 15) + 10); } /* smootherstep down */
        au->lut[i] = a * v;
    }
}

/* the core's glow against (r / its radius)², one smooth curve close to the old 0.5 / 0.22 at 0.14 / 0.06 at 0.4, reaching
 * nothing at the edge with no kink (straight pieces left faint rings where they met); a table, since it is read for
 * every pixel of a large disc */
enum { HALO_N = 1024 };
static float HALO_LUT[HALO_N];
static void halo_lut(void) {
    if (HALO_LUT[0] > 0) return;
    for (int i = 0; i < HALO_N; i++) {
        float u = sqrtf((float)i / (HALO_N - 1)), e = 1 - u * u * u * u;
        HALO_LUT[i] = 0.5f * expf(-5.6f * u) * e * e;
    }
}
static void draw_core(Canvas *c, Clip cl, float ox, float oy, float rc, const Look *L, float inten, const Aura *au) {
    halo_lut();
    float hR = rc * L->hr, bR = rc * 1.5f, sR = rc * 2.0f / 3.0f, lit = inten > 1 ? 1 : inten, hR2 = hR * hR;
    float solid = 0.08f + 0.5f * (1 - lit); /* a brightness floor: brighter than the shell when dim, nearly nothing when bright */
    float ar = sqrtf(au->r2max); /* the aura round the shell, in the same pass */
    int bx0 = (int)floorf(fminf(ox - hR, au->ox - ar)), bx1 = (int)ceilf(fmaxf(ox + hR, au->ox + ar)) + 1;
    int by0 = (int)floorf(fminf(oy - hR, au->oy - ar)), by1 = (int)ceilf(fmaxf(oy + hR, au->oy + ar)) + 1;
    if (bx0 < cl.x0) bx0 = cl.x0;
    if (by0 < cl.y0) by0 = cl.y0;
    if (bx1 > cl.x1) bx1 = cl.x1;
    if (by1 > cl.y1) by1 = cl.y1;
    touch(c, bx0, by0, bx1, by1);
    for (int y = by0; y < by1; y++) for (int x = bx0; x < bx1; x++) {
        float ax = x + 0.5f - au->ox, ay = y + 0.5f - au->oy, a2 = ax * ax + ay * ay;
        if (a2 < au->r2max && a2 > au->in2) add(c, x, y, au->col, au->lut[(int)(a2 / au->r2max * (AURA_N - 1))]);
        float dx = x + 0.5f - ox, dy = y + 0.5f - oy, d2 = dx * dx + dy * dy;
        if (d2 >= hR2) continue;
        float r = sqrtf(d2);
        float u = r / hR, a = HALO_LUT[(int)(u * u * (HALO_N - 1))];
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
    touch(c, bx0, by0, bx1, by1);
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
    touch(c, bx0, by0, bx1, by1);
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
                if (X >= cl.x0 && X < cl.x1 && Y >= cl.y0 && Y < cl.y1) { touch(c, X, Y, X + 1, Y + 1); add(c, X, Y, col, a); }
            }
        }
    }
}

/* ---- the shell, as the motion system has moved it --------------------------------------------- */
typedef struct { float M[9], T[9], ox, oy, unit, tall, st, ux, uy, cam; } Proj;
static void project(const Proj *P, const float *p, float *out) {
    float w[3];
    m3_app(w, P->M, p);
    float pp = P->cam / (P->cam - w[2]);
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
int RENDER_PART; /* see vigil.h */
static const float I3_R[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
/* (per thread: bands of one picture may be drawn at the same time, see render_band) */
#if defined(__wasm__)
#define VIGIL_TLS
#else
#define VIGIL_TLS _Thread_local
#endif
static VIGIL_TLS Proj g_stage; /* the stage's own frame, for READY's loose pieces and sparks: the core window's centre, the core's
                      * plain size, none of its offset, size, stretch or tilt (the Bestiary's projFree) */
Col SHADE_COL;   /* see vigil.h */
static VIGIL_TLS int g_part;  /* RENDER_PART for the session being drawn: 0 when it has no pieces out */
static float alpha_of(const Motion *mo, int t) {
    float a = mo->ta_on ? mo->talpha[t] : 1;
    if (g_part && (g_part == 2) != (mo->loose[t] != 0)) return 0; /* the core's window leaves its loose pieces to the layer, and the layer draws only them */
    return a;
}
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
    if (mo->loose[t] && mo->tmi[t]) { /* READY's piece: the shell's shape (a spike's cone stays a cone) or, once loose, its own */
        const int *tv = sh->t[t];        /* plain one; drawn out into a dart, turned into view space about its centre, at its place on the stage */
        const float (*src)[3] = mo->loose[t] == 2 ? (const float (*)[3])sh->v : (const float (*)[3])base;
        float c0[3], d[3];
        for (int i = 0; i < 3; i++) { c0[i] = (sh->v[tv[0]][i] + sh->v[tv[1]][i] + sh->v[tv[2]][i]) / 3; d[i] = src[v][i] - c0[i]; }
        if (mo->sharp_on && mo->sharp_k[t] > 0.001f && mo->sharp_v[t] >= 0) {
            float k = mo->sharp_k[t], ax[3];
            for (int i = 0; i < 3; i++) ax[i] = sh->v[mo->sharp_v[t]][i] - c0[i];
            float l = sqrtf(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
            if (l > 1e-6f) {
                for (int i = 0; i < 3; i++) ax[i] /= l;
                float al = d[0] * ax[0] + d[1] * ax[1] + d[2] * ax[2], fa = v == mo->sharp_v[t] ? 1 + 0.75f * k : 1, fp = 1 - 0.3f * k;
                for (int i = 0; i < 3; i++) d[i] = ax[i] * al * fa + (d[i] - ax[i] * al) * fp;
            }
        }
        m3_app(p, mo->tmx[mo->tmi[t] - 1], d);
        for (int i = 0; i < 3; i++) p[i] += mo->tt[t][i];
        project(&g_stage, p, out);
        return;
    }
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

/* ---- sparks, light trails ------------------------------------------------------ */
static void draw_sparks(Canvas *c, Clip cl, const Proj *P, const Motion *mo, const Look *L, float ks) {
    for (int i = 0; i < MAX_SPARK; i++) {
        const Spark *q = &mo->spark[i];
        if (q->life <= 0) continue;
        float a = clamp01(q->life / q->max), age = 1 - a, h[3];
        const Proj *Pq = q->stage ? &g_stage : P;
        if (q->stage) project(Pq, q->p, h); else project_view(P, q->p, h);
        Col c0 = q->own ? L->halo : q->col, col = c0;
        if (q->hot) col = age < 0.18f ? mix((Col){1, 0.98f, 0.925f}, c0, age / 0.18f) : mix(c0, (Col){0.43f, 0.11f, 0.03f}, clamp01((age - 0.18f) / 0.82f) * 0.75f);
        if (q->kind == SK_STREAK) { /* a short motion-blurred line with a white-hot head */
            float sp = sqrtf(q->v[0] * q->v[0] + q->v[1] * q->v[1] + q->v[2] * q->v[2]) / CIRCUM, tl = fminf(0.09f, 0.025f + sp * 0.022f), tp[3], mp[3];
            float tail[3] = {q->p[0] - q->v[0] * tl, q->p[1] - q->v[1] * tl, q->p[2] - q->v[2] * tl};
            if (q->stage) project(Pq, tail, tp); else project_view(P, tail, tp);
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
/* ---- the black hole ------------------------------------------------------------------------------
 * Traced the way light really bends round a non-spinning (Schwarzschild) hole, in units of its mass M. A ray from
 * your eye with impact parameter b stays in one plane; how far it is from the hole after sweeping an angle phi in
 * that plane, 1/u, follows u'' = 3u^2 - u (u(0) = 0, u'(0) = 1/b). That is worked out once for a range of b
 * (bh_lut), so each pixel only looks up where its ray crosses the disc's plane: the first crossing is the disc as
 * you see it directly (its far side bent up over the shadow), the second the image of its underside (the thin arc
 * under the shadow, and the halo), the third the photon ring. Rays with b < 3 sqrt 3 fall in: the shadow. The disc
 * (Shakura-Sunyaev-like, from the last stable orbit at 6 M) is thin and patchy, turns at Kepler's rate, and is
 * shifted by its speed toward or away from you (Doppler: brighter and whiter on the side coming at you) and by
 * gravity; a ray that leaves the hole gives the sky behind it, bent (the Lensing action's star). */
#define BH_NB 1024        /* impact parameters, 0 .. BH_BMAX */
#define BH_NP 448         /* sweep angles, 0 .. BH_PMAX */
#define BH_BMAX 30.0f
#define BH_PMAX 9.6f      /* a little past 3 pi: three crossings */
#define BH_BC 5.19615242f /* the critical impact parameter, 3 sqrt 3: the shadow's edge */
#define BH_RIN 6.0f       /* the last stable orbit */
static float g_bh_u[BH_NB][BH_NP], g_bh_end[BH_NB]; /* u along each ray; where it ends (falls in, or leaves) */
static unsigned char g_bh_cap[BH_NB];
static float g_bh_lane[512];                         /* the disc's lanes against r (0 .. 32) */
static volatile int g_bh_ready;
static float bh_hash(int x, int y) { unsigned h = (unsigned)x * 374761393u + (unsigned)y * 668265263u; h = (h ^ (h >> 13)) * 1274126177u; return (float)((h ^ (h >> 16)) & 0xffffff) / 16777215.0f; }
static void bh_lut(void) {
    if (g_bh_ready) return;
    for (int i = 0; i < BH_NB; i++) {
        double b = (i + 0.5) * BH_BMAX / BH_NB, u = 0, du = 1 / b, ph = 0;
        const double h = BH_PMAX / BH_NP / 8;
        int j = 0, end = 0;
        g_bh_u[i][0] = 0;
        for (j = 1; j < BH_NP && !end; j++) {
            for (int k = 0; k < 8; k++) { /* RK4 */
                double a1 = du, b1 = 3 * u * u - u, a2 = du + b1 * h / 2, u2 = u + a1 * h / 2, b2 = 3 * u2 * u2 - u2;
                double u3 = u + a2 * h / 2, a3 = du + b2 * h / 2, b3 = 3 * u3 * u3 - u3, u4 = u + a3 * h, a4 = du + b3 * h, b4 = 3 * u4 * u4 - u4;
                u += h / 6 * (a1 + 2 * a2 + 2 * a3 + a4); du += h / 6 * (b1 + 2 * b2 + 2 * b3 + b4); ph += h;
                if (u > 0.5) { end = 1; g_bh_cap[i] = 1; g_bh_end[i] = (float)(ph - (u - 0.5) / fmax(du, 1e-6)); break; } /* through the horizon */
                if (u < 0) { end = 1; g_bh_end[i] = (float)(ph + u / fmin(du, -1e-6)); break; }                          /* away again */
            }
            g_bh_u[i][j] = (float)(u < 0 ? 0 : u > 0.5 ? 0.5 : u);
        }
        for (; j < BH_NP; j++) g_bh_u[i][j] = g_bh_cap[i] ? 0.5f : 0;
        if (!end) { g_bh_cap[i] = 1; g_bh_end[i] = BH_PMAX; } /* (still circling: as good as fallen in) */
    }
    for (int i = 0; i < 512; i++) { /* lanes: noise in r over four octaves */
        float r = i * 32.0f / 512, v = 0, amp = 0.5f, f = 1.1f;
        for (int o = 0; o < 4; o++, amp *= 0.6f, f *= 2.3f) {
            float x = r * f; int x0 = (int)floorf(x); float w = x - x0; w = w * w * (3 - 2 * w);
            v += amp * (bh_hash(x0, o * 7 + 1) * (1 - w) + bh_hash(x0 + 1, o * 7 + 1) * w);
        }
        g_bh_lane[i] = v / 1.2f;
    }
    g_bh_ready = 1;
}
/* u (1/r) of the ray with impact b after sweeping phi; 0 if it has left, 0.5 if it has fallen in */
static float bh_u(float b, float ph, int *fate) {
    if (b >= BH_BMAX - 0.03f) { /* far out: hardly bent */
        float s = sinf(ph);
        *fate = ph >= 3.14159265f + 4 / b ? 1 : 0;
        return *fate ? 0 : fmaxf(0, s) / b * (1 + 2 / b);
    }
    float fb = b * BH_NB / BH_BMAX - 0.5f; if (fb < 0) fb = 0;
    int i = (int)fb; float wb = fb - i; if (i >= BH_NB - 1) { i = BH_NB - 2; wb = 1; }
    int n = wb < 0.5f ? i : i + 1; /* the nearer ray decides whether it got this far */
    if (ph >= g_bh_end[n]) { *fate = g_bh_cap[n] ? 2 : 1; return g_bh_cap[n] ? 0.5f : 0; }
    *fate = 0;
    if (ph >= g_bh_end[i] || ph >= g_bh_end[i + 1]) wb = n == i ? 0 : 1;
    float fp = ph * BH_NP / BH_PMAX; int j = (int)fp; float wp = fp - j; if (j >= BH_NP - 1) { j = BH_NP - 2; wp = 1; }
    float a = g_bh_u[i][j] + (g_bh_u[i][j + 1] - g_bh_u[i][j]) * wp, c = g_bh_u[i + 1][j] + (g_bh_u[i + 1][j + 1] - g_bh_u[i + 1][j]) * wp;
    return a + (c - a) * wb;
}
/* where a point at P (M units, x right, y up, z toward you) shows: image 0 is the direct one, image 1 the one bent
 * round the far side of the hole (on the other side of it, close to the shadow). 0 if it has none */
static int bh_image(const float *P, int img, float *X, float *Y) {
    float r = sqrtf(P[0] * P[0] + P[1] * P[1] + P[2] * P[2]);
    if (r < 2.2f) return 0;
    float q = sqrtf(P[0] * P[0] + P[1] * P[1]) + 1e-6f, g = atan2f(q, P[2]), dx = P[0] / q, dy = P[1] / q, ph = img ? 6.2831853f - g : g, ut = 1 / r;
    float lo = img ? BH_BC : 0, hi = img ? BH_BC + 3 : BH_BMAX + 40;
    int fate;
    for (int it = 0; it < 22; it++) { /* u falls as b grows (for the rays that get this far) */
        float mid = 0.5f * (lo + hi), u = bh_u(mid, ph, &fate);
        if (fate == 2 || (fate == 0 && u > ut)) lo = mid; else hi = mid;
    }
    float b = 0.5f * (lo + hi), u = bh_u(b, ph, &fate);
    if (fate || fabsf(u - ut) > 0.25f * ut) return 0; /* (a little loose: a stream through it never drops a point) */
    if (img) { dx = -dx; dy = -dy; }
    *X = dx * b; *Y = dy * b;
    return 1;
}
static float bh_noise(float x, float y, int py) { /* value noise, periodic in y over py cells */
    int x0 = (int)floorf(x), y0 = (int)floorf(y); float wx = x - x0, wy = y - y0;
    wx = wx * wx * (3 - 2 * wx); wy = wy * wy * (3 - 2 * wy);
    int ya = ((y0 % py) + py) % py, yb = (ya + 1) % py;
    float a = bh_hash(x0, ya), b = bh_hash(x0 + 1, ya), c = bh_hash(x0, yb), d = bh_hash(x0 + 1, yb);
    return (a + (b - a) * wx) + ((c + (d - c) * wx) - (a + (b - a) * wx)) * wy;
}
/* the colour of the disc's light by how hot it looks: ember, orange, gold, warm white, and a blue-white for the
 * hottest light coming at you */
static Col bh_temp(float T) {
    static const float K[6][4] = {{0, 0.22f, 0.03f, 0}, {0.35f, 0.85f, 0.24f, 0.02f}, {0.6f, 1, 0.45f, 0.08f}, {0.85f, 1, 0.64f, 0.24f}, {1.1f, 1, 0.84f, 0.56f}, {1.45f, 0.93f, 0.94f, 1}};
    if (T <= 0) return (Col){K[0][1], K[0][2], K[0][3]};
    for (int i = 1; i < 6; i++) if (T < K[i][0]) { float w = (T - K[i - 1][0]) / (K[i][0] - K[i - 1][0]); return (Col){K[i - 1][1] + (K[i][1] - K[i - 1][1]) * w, K[i - 1][2] + (K[i][2] - K[i - 1][2]) * w, K[i - 1][3] + (K[i][3] - K[i - 1][3]) * w}; }
    return (Col){K[5][1], K[5][2], K[5][3]};
}
typedef struct {
    float n[3], ea[3], ec[3];  /* the disc's axis, and its plane's two directions (ea toward you) */
    float rout, T, bright, heat;  /* outer edge (M), its turn (s, Kepler-scaled), how bright, the torn star's heat */
    int act; float au, at, p, q, fo; /* the action playing, how far through (0..1, and seconds), its leans; fo: 1, down to 0 as it fades out */
    float sky, sky_off[2], ring_c[2], half[2], half_k, sky_lim[2]; /* half: the side of the shadow its ring shows on (half_k: how much, 0 edge-on) */ /* the stars behind it: how bright, how far the sky has drifted (radians); the ring's source, off the line */
    float side_lit[2], lit;      /* your side, pointed at */
    float dir, hot, pxm, zmax, room; /* zmax: the picture's edge from the middle, M (room: in pixels); which way the disc turns (-1..1, the wheel can reverse it), how hot it runs, M a pixel */
} BHView;
BhTune BH_TUNE = {1, 1, 0.38f, 1.7f, 1, 1.02f, 0.62f, 3, 0.5f, 0.19f, 0.4f, 1, 8, 0, 9500,
                  1, 0.5f, 0.7f, 9500, 4, 0.2f, 24, 128, 160, 30, 60, 1, 1};
#define BH_DO BH_TUNE.lens_d /* how far you are from the hole (M): sets the Einstein ring's size */
static float bh_spot(const BHView *V, float r, float az, float delay) { /* the Hot spot action: a clump on the inner disc */
    float t = V->at - delay;
    if (t < 0) return 0;
    float rs = 7.0f, w = BH_TS / powf(rs, 1.5f), a0 = V->p * 3.14159265f + w * t;
    float da = az - a0; da -= 6.2831853f * floorf((da + 3.14159265f) / 6.2831853f);
    float life = clamp01(t / 0.6f) * clamp01((HOLE_ACT_DUR[HA_SPOT] - t) / 3.5f) * V->fo, sa = 0.3f + 0.08f * t, sr = 0.75f + 0.04f * t;
    if (da > 0) da *= 0.6f; /* a tail drawn out behind it */
    return 16 * life * expf(-(r - rs) * (r - rs) / (sr * sr) - da * da / (sa * sa)) / (1 + 0.15f * t);
}
/* the disc at radius r, azimuth az (from ea toward ec): its light (before the shift) and how much it hides behind it */
static float bh_disc(const BHView *V, float r, float az, int img, float *alpha) {
    if (r < BH_RIN * 0.86f || r > V->rout) { *alpha = 0; return 0; }
    float fr = r * 16; int li = (int)fr; float lane = li < 511 ? g_bh_lane[li] + (g_bh_lane[li + 1] - g_bh_lane[li]) * (fr - li) : g_bh_lane[511];
    /* clumps sheared by the turn: two layers, each started afresh in turn (when it is unseen) so they never wind up too tight */
    const float P = 5; /* (short: the inner gas outruns the outer, and a long cycle winds the clouds into fine streaks) */
    float om = 1 / (r * sqrtf(r)), clump = 0, x6 = BH_RIN / r, q6 = sqrtf(sqrtf(x6));
    for (int k = 0; k < 2; k++) {
        float tl = fmodf(V->T + k * P * 0.5f, P), wk = 1 - fabsf(2 * tl / P - 1), a = az - om * BH_TS * tl, x = a * (28 / 6.2831853f);
        float nz = 0.6f * bh_noise(r * 0.6f + k * 37, x * (18.0f / 28), 18) + 0.4f * bh_noise(r * 1.5f + k * 91, x * (40.0f / 28), 40); /* (broad clouds of gas, then finer ones) */
        clump += wk * nz;
    }
    float cl = BH_TUNE.cloud, tex = 0.38f + 0.18f * lane * (1 + 4 * (1 - cl)) + 1.15f * cl * clump * clump * (0.85f + 0.3f * lane); /* (soft turbulent gas, sheared into swirls; only a hint of lanes) */
    float prof = x6 * q6 * (1 + 1.1f * expf(-(r - 6.6f) * (r - 6.6f) / 2.5f)); /* thin-disc light, brightest just out from the inner edge */
    float edge = clamp01((r - BH_RIN * 0.86f) / (BH_RIN * 0.16f)) * clamp01((V->rout - r) / (V->rout * 0.35f));
    float heat = 1 + V->heat * 2.5f * expf(-(r - 7) * (r - 7) / 30);
    float e = prof * tex * edge * heat;
    if (V->act == HA_FLARE) { /* the flux eruption's flash: a patch of the inner disc blazes, then shears out along it;
                                * the bent images see it later (light echo) */
        float t = V->at - 0.35f * img;
        if (t > 0) {
            float a0 = V->p * 3.14159265f + BH_TS / (6.8f * sqrtf(6.8f)) * t, da = az - a0;
            da -= 6.2831853f * floorf((da + 3.14159265f) / 6.2831853f);
            float amp = (t < 0.18f ? t / 0.18f : expf(-(t - 0.18f) * 1.2f)) * V->fo, sa = 0.25f + 0.45f * t, sr = 0.8f + 0.25f * t;
            e += 22 * amp * expf(-(r - 6.8f) * (r - 6.8f) / (sr * sr) - da * da / (sa * sa)) * edge;
            e *= 1 + 0.5f * amp * expf(-(r - 8) * (r - 8) / 20); /* the inner disc lit up round it */
        }
    }
    if (V->act == HA_SPOT) e += bh_spot(V, r, az, 0.12f * img) * edge;
    *alpha = clamp01((0.5f + 0.55f * clump + 0.2f * lane) * edge) * 0.95f;
    return e;
}
static inline float q6v(float r) { float x = BH_RIN / r, q = sqrtf(x); return q * sqrtf(q); } /* (6 / r)^0.75 */
/* one ray: its light (out); 1 if it falls in (what is behind the hole is hidden, whatever disc lies in front) */
/* a blackbody's colour at temperature T (K), linear (Tanner Helland's fit, as blackhole-simulation uses it) */
static void bh_blackbody(float T, float *c) {
    float t = fmaxf(T, 1) / 100, r, g, b;
    if (t <= 66) { r = 255; g = 99.4708025861f * logf(t) - 161.1195681661f; b = t <= 19 ? 0 : 138.5177312231f * logf(t - 10) - 305.0447927307f; }
    else { r = 329.698727446f * powf(t - 60, -0.1332047592f); g = 288.1221695283f * powf(t - 60, -0.0755148492f); b = 255; }
    c[0] = powf(clamp01(r / 255), 2.2f); c[1] = powf(clamp01(g / 255), 2.2f); c[2] = powf(clamp01(b / 255), 2.2f);
}
/* the disc's light at radius r, azimuth az, for image k: its colour into col (times its strength), how opaque into al */
static void bh_shade(const BHView *V, float r, float az, int k, float Lz, float dx, float dy, float *col, float *al) {
    float e = bh_disc(V, r, az, k, al);
    col[0] = col[1] = col[2] = 0;
    if (*al <= 0) return;
    float gg = sqrtf(fmaxf(0.02f, 1 - 3 / r)), gd = 1 / fmaxf(0.3f, 1 - BH_TUNE.doppler * V->dir * Lz / (r * sqrtf(r))); /* gravity's and the turn's shift */
    if (BH_TUNE.site_col > 0.5f) { /* as blackhole-simulation colours it (MIT, steeltroops-ai): Novikov-Thorne temperature, a blackbody, beaming d^3.5 */
        float ir = fminf(1, BH_RIN / r), Tk = BH_TUNE.site_temp * powf(ir, 0.75f) * powf(fmaxf(0, 1 - sqrtf(ir)), 0.25f) * gd * gg, bb[3];
        bh_blackbody(Tk, bb);
        float I = e * powf(gd, 3.5f) * V->bright * (k ? 1.15f : 1) * 3;
        col[0] = I * bb[0]; col[1] = I * bb[1]; col[2] = I * bb[2];
        return;
    }
    float I = e * powf(gd, BH_TUNE.beam) * gg * gg * V->bright * (k ? 1.15f : 1) * (1 + V->lit * fmaxf(0, dx * V->side_lit[0] + dy * V->side_lit[1])), T = BH_TUNE.temp * q6v(r) * gd * gg * V->hot * (1 + 0.05f * fminf(e, 8));
    Col cc = bh_temp(T);
    col[0] = I * cc.r; col[1] = I * cc.g; col[2] = I * cc.b;
}
/* only the disc as this ray sees it (all its crossings, front to back): for the reflection inside the shadow */
static void bh_disc_seen(const BHView *V, float X, float Y, float *out) {
    float b = sqrtf(X * X + Y * Y) + 1e-5f, dx = X / b, dy = Y / b, A = V->n[2], B = dx * V->n[0] + dy * V->n[1];
    float ph0 = atan2f(A, -B); if (ph0 < 0) ph0 += 3.14159265f;
    float Lz = Y * V->n[0] - X * V->n[1], tr = 1;
    int fate = 0;
    out[0] = out[1] = out[2] = 0;
    for (int k = 0; k < 3; k++) {
        float ph = ph0 + k * 3.14159265f, u = bh_u(b, ph, &fate);
        if (fate) break;
        float r = 1 / fmaxf(u, 1e-4f);
        if (r > V->rout) continue;
        float sp = sinf(ph), cp = cosf(ph), P[3] = {sp * dx, sp * dy, cp};
        float az = atan2f(P[0] * V->ec[0] + P[1] * V->ec[1] + P[2] * V->ec[2], P[0] * V->ea[0] + P[1] * V->ea[1] + P[2] * V->ea[2]), col[3], al;
        bh_shade(V, r, az, k, Lz, dx, dy, col, &al);
        if (al <= 0) continue;
        out[0] += tr * al * col[0]; out[1] += tr * al * col[1]; out[2] += tr * al * col[2];
        tr *= 1 - al;
        if (tr < 0.02f) return;
    }
}
/* one ray: its light (out); 1 if it falls in (what is behind the hole is hidden, whatever disc lies in front). The
 * disc's crossings front to back, the photon ring at the shadow's edge, and with Lensing the sky behind it and, inside
 * the shadow, the disc reflected */
static float bh_ray(const BHView *V, float X, float Y, float *out, float *trans) {
    float b = sqrtf(X * X + Y * Y) + 1e-5f, dx = X / b, dy = Y / b, A = V->n[2], B = dx * V->n[0] + dy * V->n[1];
    float ph0 = atan2f(A, -B); if (ph0 < 0) ph0 += 3.14159265f;
    float Lz = Y * V->n[0] - X * V->n[1]; /* the ray's angular momentum about the disc's axis (Doppler) */
    float tr = 1; out[0] = out[1] = out[2] = 0;
    int fate = 0;
    { /* the photon ring: light that circled the hole before getting out, a bold white ring hugging the shadow, thicker and
       * brighter on the side coming toward you (Doppler); hidden only where the disc lies in front of it */
        float side = V->dir * Lz / b, w = fmaxf(0.09f, 0.9f * V->pxm) * BH_TUNE.ring_w * (1 + 0.7f * fmaxf(0, side)), off = b - BH_BC * 1.012f;
        if (off * off < 16 * w * w) {
            float L = BH_TUNE.ring_a * (1.5f + 1.2f * side) * expf(-off * off / (w * w)) * fminf(1, 0.14f / w) * tr * V->bright / 1.5f;
            if (L > 0) { out[0] += L * 0.95f; out[1] += L * 0.95f; out[2] += L; }
        }
    }
    for (int k = 0; k < 3; k++) {
        float ph = ph0 + k * 3.14159265f, u = bh_u(b, ph, &fate);
        if (fate) break;
        float r = 1 / fmaxf(u, 1e-4f);
        if (r > V->rout) continue;
        float sp = sinf(ph), cp = cosf(ph), P[3] = {sp * dx, sp * dy, cp};
        float az = atan2f(P[0] * V->ec[0] + P[1] * V->ec[1] + P[2] * V->ec[2], P[0] * V->ea[0] + P[1] * V->ea[1] + P[2] * V->ea[2]), col[3], al;
        bh_shade(V, r, az, k, Lz, dx, dy, col, &al);
        if (al <= 0) continue;
        out[0] += tr * al * col[0]; out[1] += tr * al * col[1]; out[2] += tr * al * col[2];
        tr *= 1 - al;
        if (tr < 0.02f) { *trans = 0; return b < BH_BC; }
    }
    *trans = tr;
    if (fate == 2 || (fate == 0 && b < BH_BC)) { /* fallen in: dark; but with Lensing, its circle is not flat black: */
        if (V->sky > 0.003f) {
            /* a faint warm haze of scattered disc light */
            float rho = b / BH_BC, h = 0.05f * (0.35f + 0.65f * rho * rho);
            out[0] += tr * V->sky * h; out[1] += tr * V->sky * h * 0.5f; out[2] += tr * V->sky * h * 0.2f;
            /* the disc seen again inside it, as light that went further round shows it: turned over (through the middle)
             * and squeezed in toward the ring, dim, fading toward the centre; it changes with every view, as the disc does */
            float bm = BH_BC * 1.03f + (BH_BC - b) * BH_TUNE.refl_squeeze, rl[3];
            bh_disc_seen(V, -dx * bm, -dy * bm, rl);
            float wgt = BH_TUNE.refl_a * tr * V->sky * powf(rho, 0.8f);
            out[0] += wgt * rl[0]; out[1] += wgt * rl[1]; out[2] += wgt * rl[2];
        }
        return 1;
    }
    if (V->sky > 0.003f) { /* the sky behind it, bent (lens equation, you BH_DO away): its stars laid out round the hole and
                            * drawn out along the circles round it into short arcs, as the hole's pull smears them, longer
                            * further out; doubled inside the Einstein ring; turning with the view. To the drawn box's edge,
                            * fading out before it */
        float rl = fminf(V->sky_lim[0], V->sky_lim[1]) * 0.96f, fade = clamp01((rl - b) / (rl * 0.45f)); fade = fade * fade * (3 - 2 * fade); /* (a soft round fade, well inside the window: its edge never shows) */
        float dfl = (fate == 1 ? g_bh_end[(int)fminf(BH_NB - 1, b * BH_NB / BH_BMAX)] : 3.14159265f + 4 / b) - 3.14159265f;
        if (b >= BH_BMAX - 0.03f) dfl = 4 / b;
        /* where this ray's star lies on the sky behind (fixed: turning the hole does not turn its sky); each star a set size
         * on the sky, about a pixel where the light is hardly bent, so near the hole, where the bending magnifies it, it is
         * drawn out into an arc round it by the bending itself */
        float be = b / BH_DO - dfl, sx = dx * be, sy = dy * be;
        float px1 = V->pxm / BH_DO, CS = 10 * px1, sig = 0.85f * px1, lit = 0; /* (a pixel far out, on the sky; a cell) */
        int ix0 = (int)floorf(sx / CS - 0.5f), iy0 = (int)floorf(sy / CS - 0.5f);
        for (int j = 0; j < 2; j++) for (int i = 0; i < 2; i++) {
            int ix = ix0 + i, iy = iy0 + j;
            float h = bh_hash(ix * 7 + 3, iy * 13 + 5);
            if (h > BH_TUNE.star_n) continue;
            float cx = (ix + 0.1f + 0.8f * bh_hash(ix, iy + 101)) * CS, cy = (iy + 0.1f + 0.8f * bh_hash(ix + 57, iy)) * CS;
            float d2 = ((sx - cx) * (sx - cx) + (sy - cy) * (sy - cy)) / (sig * sig);
            if (d2 < 12) lit += BH_TUNE.star_a * (0.45f + 1.6f * (h / BH_TUNE.star_n) * (h / BH_TUNE.star_n)) * expf(-d2);
        }
        if (lit > 0) { float a = tr * V->sky * fade * lit; out[0] += a * 0.88f; out[1] += a * 0.93f; out[2] += a; }
    }
    return 0;
}
static float bh_env(float u, float in, float out) { return u <= 0 || u >= 1 ? 0 : fminf(1, fminf(u / in, (1 - u) / out)); }
/* a soft glowing stream through the points p (Catmull-Rom, drawn as close-set soft dots, so it is smooth at any size):
 * ok[i] whether point i shows, a[i] how bright, col[i] its colour; w its half-width in pixels; a gap longer than gap breaks it */
static void bh_ribbon(Canvas *c, Clip cl, float (*p)[2], const int *ok, const float *a, const Col *col, int n, float w, float gap) {
    for (int i = 0; i + 1 < n; i++) {
        if (!ok[i] || !ok[i + 1]) continue;
        float L = hypotf(p[i + 1][0] - p[i][0], p[i + 1][1] - p[i][1]);
        if (L > gap) continue;
        const float *p0 = p[i > 0 && ok[i - 1] ? i - 1 : i], *p1 = p[i], *p2 = p[i + 1], *p3 = p[i + 2 < n && ok[i + 2] ? i + 2 : i + 1];
        int m = (int)fminf(160, ceilf(L / (0.35f * w)) + 1);
        for (int q = (i == 0 || !ok[i - 1]) ? 0 : 1; q <= m; q++) {
            float u = (float)q / m, u2 = u * u, u3 = u2 * u;
            float X = 0.5f * (2 * p1[0] + (-p0[0] + p2[0]) * u + (2 * p0[0] - 5 * p1[0] + 4 * p2[0] - p3[0]) * u2 + (-p0[0] + 3 * p1[0] - 3 * p2[0] + p3[0]) * u3);
            float Y = 0.5f * (2 * p1[1] + (-p0[1] + p2[1]) * u + (2 * p0[1] - 5 * p1[1] + 4 * p2[1] - p3[1]) * u2 + (-p0[1] + 3 * p1[1] - 3 * p2[1] + p3[1]) * u3);
            float aa = (a[i] + (a[i + 1] - a[i]) * u) * fminf(1, L / m / (0.35f * w)) * 0.3f;
            Col cc = mix(col[i], col[i + 1], u);
            blob(c, cl, X, Y, w * 2.2f, mix(cc, WHITE, 0.5f), aa, cc, aa * 0.35f, 0.3f);
        }
    }
}
/* a point (disc frame: x toward you, y across, z up its axis) on screen: both its images, in pixels; how many */
/* the black hole's disc axis as it is drawn now: as the hand and the wheel left it (HoleRS.nv; at rest about 15 degrees
 * above edge-on), with its own slow sway, and the precession */
void bh_hole_axis(const Session *s, double now, float *out) {
    const HoleRS *H = &s->mo.rs.hole;
    int act = H->act;
    float at = act ? (float)(now - H->act_at) : 0, au = act ? clamp01(at / HOLE_ACT_DUR[act]) : 0;
    float fo = act ? clamp01((float)(H->act_end - now) / 0.9f) : 0; fo = fo * fo * (3 - 2 * fo);
    struct { float p, q, fo; } V_ = {H->act_p, H->act_q, fo}, *Vp = &V_;
    float n3[3];
    { /* the disc's axis: as the hand and the wheel left it (HoleRS.nv; at rest about 6 degrees above edge-on), with a slow
       * sway, and the precession tipping it toward you */
        float nv[3] = {H->nv[0], H->nv[1], H->nv[2]};
        if (nv[0] * nv[0] + nv[1] * nv[1] + nv[2] * nv[2] < 0.25f) { nv[0] = -0.06f * 0.966f; nv[1] = 0.966f; nv[2] = 0.26f; }
        float ph = H->tilt_ph, sw = 0.14f * sinf(ph * 0.29f), sr = 0.1f * sinf(ph * 0.2f + 1); /* (its axis never quite still: it tilts and rolls, HOLE_TILT fast) */
        float c1 = cosf(sw), s1 = sinf(sw), y = nv[1] * c1 - nv[2] * s1, z = nv[1] * s1 + nv[2] * c1; /* (about x: toward you) */
        nv[1] = y; nv[2] = z;
        float c2 = cosf(sr), s2 = sinf(sr), x = nv[0] * c2 + nv[1] * s2; y = -nv[0] * s2 + nv[1] * c2; nv[0] = x; nv[1] = y; /* (about the view: a roll) */
        float l = sqrtf(nv[0] * nv[0] + nv[1] * nv[1] + nv[2] * nv[2]);
        for (int q = 0; q < 3; q++) nv[q] /= l;
        if (act == HA_PRECESS) { /* Lense-Thirring precession: the dragged disc wobbles like a top, its axis sweeping two turns
                                  * of a cone that opens up and closes again, so it shows its face from every side in turn */
            float w = sinf(3.14159265f * au); w = w * w * Vp->fo;
            float cone = (0.62f + 0.15f * Vp->q) * w, ph = Vp->p * 3.14159265f + 6.2831853f * 2 * au * au * (3 - 2 * au) * (Vp->q < 0 ? -1 : 1);
            float e1[3] = {1, 0, 0}, d = nv[0], e2[3];
            for (int q = 0; q < 3; q++) e1[q] -= d * nv[q];
            float l1 = sqrtf(e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2]) + 1e-5f;
            for (int q = 0; q < 3; q++) e1[q] /= l1;
            e2[0] = nv[1] * e1[2] - nv[2] * e1[1]; e2[1] = nv[2] * e1[0] - nv[0] * e1[2]; e2[2] = nv[0] * e1[1] - nv[1] * e1[0];
            for (int q = 0; q < 3; q++) nv[q] = cosf(cone) * nv[q] + sinf(cone) * (cosf(ph) * e1[q] + sinf(ph) * e2[q]);
        }
        for (int q = 0; q < 3; q++) n3[q] = nv[q];
        if (fabsf(n3[2]) > 0.999f) n3[1] += 0.002f; /* (face on: the plane's own directions still defined) */
    }
    memcpy(out, n3, sizeof n3);
}
/* ---- the black hole after blackhole-simulation ------------------------------------------------------------------
 * Its look is blackhole-simulation's (https://github.com/steeltroops-ai/blackhole-simulation, MIT License, Copyright (c)
 * 2026 Mayank): its rays are traced the way its fragment shader traces them (the Kerr acceleration of its metric chunk,
 * its step rule, its frame dragging, its disc sampling and crossing count, its photon-ring and ergosphere terms, its
 * starfield), and its disc shaded as its disc chunk does (noise turning at each radius's own rate, Doppler beaming
 * delta^3.5, a blackbody at the Novikov-Thorne temperature), then its ACES tone curve. That costs far too much to do
 * every frame here, so the tracing is done once per view (the picture depends on the disc's tilt only: its turn moves
 * the gas, a roll turns the picture) into a map every B2_STEP degrees, built a little at a time and kept (B2_KEEP);
 * each frame only shades the map: every ray's disc samples, their gas noise, their light. Inclinations past 90 are the
 * mirror of those under it (the hole and its disc are the same above and below). */
#define B2_STEP 2.5f
#define B2_NI 37      /* 0 .. 90 degrees */
#define B2_KEEP 8
#define B2_SMAX 24    /* disc samples kept per ray (the nearest first: those behind barely show through) */
#define B2_RA 120     /* the ring, traced finely: angles round it */
#define B2_RB 128     /* and radii, over B2_RLO .. B2_RHI (M) */
#define B2_RLO 2.0f
#define B2_RHI 9.0f
typedef struct { short x, y, z; unsigned short om; float w; unsigned char r, g, b, pad; } B2S; /* a sample: where (M / 512), its turn (1 / 8192), weight, colour */
typedef struct {
    int ready, row, n, idx;         /* built to row; its size (n x n); its tilt (x B2_STEP degrees) */
    unsigned long used;             /* when last drawn (to keep the latest) */
    unsigned *off; unsigned char *cnt, *hit, *star; float *ring, *ergo; short *dir; /* dir: where each ray leaves to (x 32767) */
    float *prof; int prow; /* the ring's profile (B2_RA x B2_RB), traced to angle prow */
    B2S *s; size_t ns, cap;
} B2Map;
static B2Map g_b2[B2_NI];
/* rough maps for every tilt (B2_CN square, the ring's profile every B2_CPS-th radius): built first, so a turn or a drag
 * shows the tilt it is at straight away and never the nearest finished one (it jumped); the full map follows once it rests */
#define B2_CN 48
#define B2_CPS 2
static B2Map g_b2c[B2_NI];
static int g_b2_last = -1, g_b2_still; /* the tilt asked for last, and for how many calls in a row */
static float g_b2_key[12];          /* the tunables the maps were built with: changed, they are built again */
static unsigned long g_b2_clock;
static int g_b2_want = -1; /* the map asked for last */
static unsigned char g_b2_tex[256 * 256]; /* blackhole-simulation's noise texture: random bytes (its hash) */
static int g_b2_tex_ok;
int BH_GPU_ON, BH_LIGHT;
static float b2_res(void) { return BH_TUNE.b2_res; } /* (the map's size: the same in light, so switching never rebuilds the maps and shows the old engine meanwhile) */
static float b2_key_now(float *k) {
    const BhTune *t = &BH_TUNE;
    float v[12] = {t->b2_spin, t->b2_lens, t->b2_temp, t->b2_height, t->b2_disk, t->b2_steps, b2_res(), t->b2_ext, t->b2_dist, 0, 0, 0};
    memcpy(k, v, sizeof v);
    return 0;
}
static void b2_free(B2Map *m) { free(m->dir); free(m->prof); free(m->off); free(m->cnt); free(m->hit); free(m->star); free(m->ring); free(m->ergo); free(m->s); memset(m, 0, sizeof *m); }
static float b2_hash3(float x, float y, float z) { /* its hash: the noise texture at p.xy + p.z * 37 (whole numbers: one texel) */
    int u = (int)floorf(x + z * 37) & 255, v = (int)floorf(y + z * 37) & 255;
    return g_b2_tex[v * 256 + u] / 255.0f;
}
static float b2_noise(float x, float y, float z) { /* its 3D value noise (its hash: the texel at x + 37 z, y + 37 z) */
    float fx0 = floorf(x), fy0 = floorf(y), fz0 = floorf(z), fx = x - fx0, fy = y - fy0, fz = z - fz0;
    int ix = (int)fx0, iy = (int)fy0, iz = (int)fz0, o0 = iz * 37, o1 = o0 + 37;
    fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy); fz = fz * fz * (3 - 2 * fz);
    const unsigned char *T = g_b2_tex;
    int u0 = (ix + o0) & 255, u1 = (ix + 1 + o0) & 255, v0 = ((iy + o0) & 255) << 8, v1 = ((iy + 1 + o0) & 255) << 8;
    int U0 = (ix + o1) & 255, U1 = (ix + 1 + o1) & 255, V0 = ((iy + o1) & 255) << 8, V1 = ((iy + 1 + o1) & 255) << 8;
    float a = T[v0 + u0] + (T[v0 + u1] - T[v0 + u0]) * fx, b = T[v1 + u0] + (T[v1 + u1] - T[v1 + u0]) * fx;
    float c = T[V0 + U0] + (T[V0 + U1] - T[V0 + U0]) * fx, d = T[V1 + U0] + (T[V1 + U1] - T[V1 + U0]) * fx;
    float y0 = a + (b - a) * fy, y1 = c + (d - c) * fy;
    return (y0 + (y1 - y0) * fz) * (1 / 255.0f);
}
static float b2_smooth(float e0, float e1, float x) { float t = clamp01((x - e0) / (e1 - e0)); return t * t * (3 - 2 * t); }
static void b2_kerr(const float *p, const float *v, float a, float *acc, float *omega) { /* its kerr_geodesic_accel, M = 1, spin along y */
    float a2 = a * a, rho2 = p[0] * p[0] + p[1] * p[1] + p[2] * p[2], diff = rho2 - a2, disc = diff * diff + 4 * a2 * p[1] * p[1];
    float r2 = 0.5f * (diff + sqrtf(fmaxf(0, disc))), rk = sqrtf(fmaxf(1e-8f, r2));
    float sigma = r2 + a2 * (p[1] * p[1] / fmaxf(1e-8f, r2));
    float L[3] = {p[1] * v[2] - p[2] * v[1], p[2] * v[0] - p[0] * v[2], p[0] * v[1] - p[1] * v[0]};
    float Ly = L[1], Lye = Ly - a, L2 = Lye * Lye + (L[0] * L[0] + L[1] * L[1] + L[2] * L[2] - Ly * Ly);
    float ri = 1 / rk, r2i = ri * ri, r4i = r2i * r2i, sr = r2 / fmaxf(1e-8f, sigma), pl = sqrtf(rho2) + 1e-8f;
    float f = -(r2i * sr + 3 * fmaxf(0, L2) * r4i * sr) / pl;
    float r3 = rk * r2 + a2 * rk, drag = 2 * a / fmaxf(1e-8f, r3);
    acc[0] = p[0] * f + drag * v[2]; acc[1] = p[1] * f; acc[2] = p[2] * f - drag * v[0]; /* (+ cross((0,1,0), v) * drag) */
    *omega = drag;
}
static void b2_blackbody(float T, float *c) { /* its blackbody (Tanner Helland's fit, made linear) */
    float t = fmaxf(T, 1) / 100, r, g, b;
    if (t <= 66) { r = 255; g = 99.4708025861f * logf(t) - 161.1195681661f; b = t <= 19 ? 0 : 138.5177312231f * logf(t - 10) - 305.0447927307f; }
    else { r = 329.698727446f * powf(t - 60, -0.1332047592f); g = 288.1221695283f * powf(t - 60, -0.0755148492f); b = 255; }
    c[0] = powf(clamp01(r / 255), 2.2f); c[1] = powf(clamp01(g / 255), 2.2f); c[2] = powf(clamp01(b / 255), 2.2f);
}
static float g_b2_bb[512][3]; /* the blackbody by temperature, 500 K to 500000 K on a log scale */
static int g_b2_bb_ok;
static void b2_bb_lut(float T, float *c) {
    if (!g_b2_bb_ok) { for (int i = 0; i < 512; i++) b2_blackbody(500 * powf(1000, i / 511.0f), g_b2_bb[i]); g_b2_bb_ok = 1; }
    float f = logf(fmaxf(500, T) / 500) / logf(1000) * 511;
    int i = (int)f; float w = f - i;
    if (i >= 511) { i = 510; w = 1; }
    for (int q = 0; q < 3; q++) c[q] = g_b2_bb[i][q] + (g_b2_bb[i + 1][q] - g_b2_bb[i][q]) * w;
}
static void b2_starcol(float bv, float *c) {
    float t = fminf(2, fmaxf(-0.4f, bv));
    if (t < 0) { c[0] = 0.6f; c[1] = 0.7f; c[2] = 1; } else if (t < 0.3f) { c[0] = 0.85f; c[1] = 0.88f; c[2] = 1; }
    else if (t < 0.6f) { c[0] = 1; c[1] = 0.96f; c[2] = 0.9f; } else if (t < 1) { c[0] = 1; c[1] = 0.85f; c[2] = 0.6f; } else { c[0] = 1; c[1] = 0.6f; c[2] = 0.4f; }
}
/* one ray of map m (inclination inc, radians) through (u, v), M on the hole's plane: its shader's loop */
static float b2_ray(B2Map *m, size_t pix, float inc, float u, float v) { /* (m NULL: only its ring) */
    const BhTune *T = &BH_TUNE;
    const float a = T->b2_spin, lens = T->b2_lens, D = T->b2_dist, MINS = 0.01f, MAXS = 1.2f;
    float rh = 1 + sqrtf(fmaxf(0, 1 - a * a));
    float as = fminf(0.9999f, fmaxf(-0.9999f, a)), th = (2.0f / 3) * acosf(fminf(1, fmaxf(-1, -as))), rph = 2 * (1 + cosf(th));
    float s_ = fabsf(as), z1 = 1 + cbrtf(1 - s_ * s_) * (cbrtf(1 + s_) + cbrtf(1 - s_)), z2 = sqrtf(3 * s_ * s_ + z1 * z1);
    float isco = 3 + z2 - (a >= 0 ? 1 : -1) * sqrtf((3 - z1) * (3 + z1 + 2 * z2));
    float ci = cosf(inc), si = sinf(inc), ro[3] = {0, D * ci, -D * si}, fw[3] = {0, -ci, si}, up[3] = {0, si, ci}, rt[3] = {1, 0, 0};
    float p[3], vv[3];
    for (int q = 0; q < 3; q++) { p[q] = ro[q]; vv[q] = fw[q] + (u * rt[q] + v * up[q]) / D; }
    { float l = sqrtf(vv[0] * vv[0] + vv[1] * vv[1] + vv[2] * vv[2]); for (int q = 0; q < 3; q++) vv[q] /= l; }
    for (int q = 0; q < 3; q++) p[q] += vv[q] * (0.5f * MINS + fmaxf(0, D - 40)); /* (its blue-noise start, at its mean; then straight in to 40 M: out there the light is hardly bent) */
    float cr[3] = {ro[1] * vv[2] - ro[2] * vv[1], ro[2] * vv[0] - ro[0] * vv[2], ro[0] * vv[1] - ro[1] * vv[0]};
    int hit = sqrtf(cr[0] * cr[0] + cr[1] * cr[1] + cr[2] * cr[2]) < rh * 0.9f, cross = 0, steps = (int)T->b2_steps, ns = 0;
    float prevY = p[1], alpha_hi = 0, hgt = fminf(T->b2_height, 0.45f), outer = fmaxf(T->b2_disk, isco * 1.1f);
    size_t first = m ? m->ns : 0;
    for (int i = 0; i < steps && !hit; i++) {
        float pp[3] = {p[0], p[1], p[2]}, r = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        if (r < rh * 1.15f) { hit = 1; break; }
        if (r > 32 && r > outer + 4 && p[0] * vv[0] + p[1] * vv[1] + p[2] * vv[2] > 0) break; /* (on its way out past the disc: as good as gone) */
        float df = 1 + r * 0.05f, dt = fminf(fmaxf((r - rh) * 0.1f * df, MINS), MAXS * df);
        if (r > 30) { dt = fmaxf(dt, MINS + (r - 30) * 0.08f); dt = fminf(dt, MAXS * 2.5f); }
        dt = fminf(dt, MINS + fabsf(r - rph) * 0.15f);
        float hr = b2_smooth(0.2f, 0, fabsf(p[1])), cdt = dt * (1 - hr * 0.7f), acc[3], acc2[3], om, om2;
        b2_kerr(p, vv, a, acc, &om);
        for (int q = 0; q < 3; q++) acc[q] *= lens;
        { float an = om * cdt, c = 1 - 0.5f * an * an, s = an * (1 - an * an / 6), x = vv[0] * c - vv[2] * s, z = vv[0] * s + vv[2] * c; vv[0] = x; vv[2] = z; } /* (its ZAMO turn: a small angle) */
        for (int q = 0; q < 3; q++) p[q] += vv[q] * cdt + 0.5f * acc[q] * cdt * cdt;
        float rn = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        b2_kerr(p, vv, a, acc2, &om2);
        for (int q = 0; q < 3; q++) vv[q] += 0.5f * (acc[q] + acc2[q] * lens) * cdt;
        { float l = sqrtf(vv[0] * vv[0] + vv[1] * vv[1] + vv[2] * vv[2]); for (int q = 0; q < 3; q++) vv[q] /= l; }
        if (prevY * p[1] < 0 && rn < rph * 2 && rn > rh) cross = cross < 3 ? cross + 1 : 3;
        prevY = p[1];
        /* its sample_accretion_disk, the parts that do not change with time */
        int crossed = pp[1] * p[1] < 0;
        float sP[3] = {p[0], p[1], p[2]};
        if (crossed) { float t = fabsf(pp[1]) / fmaxf(1e-4f, fabsf(pp[1]) + fabsf(p[1])); for (int q = 0; q < 3; q++) sP[q] = pp[q] + (p[q] - pp[q]) * t; }
        float sR = sqrtf(sP[0] * sP[0] + sP[1] * sP[1] + sP[2] * sP[2]), dH = sR * hgt;
        if (m && ns < B2_SMAX && alpha_hi < 3 && (fabsf(sP[1]) < dH || crossed) && sR > isco && sR < outer) {
            float sg = a + 1e-8f >= 0 ? 1.0f : -1.0f, Om = sg / (sR * sqrtf(sR) + a);
            float hf = expf(-fabsf(sP[1]) / fmaxf(0.001f, dH * 0.25f)), rf = b2_smooth(outer, isco, sR), w = hf * rf * 0.12f * cdt;
            if (hf * rf * 0.75f > 0.001f) {
                float g_tt = -(1 - 2 / sR), g_tp = -2 * a / sR, g_pp = sR * sR + a * a + 2 * a * a / sR;
                float ut = 1 / sqrtf(fmaxf(1e-6f, -(g_tt + 2 * Om * g_tp + Om * Om * g_pp))), Lp = p[2] * vv[0] - p[0] * vv[2];
                float delta = 1 / fmaxf(0.01f, ut * (1 - Om * Lp)), beam = fmaxf(0.01f, delta * delta * delta * sqrtf(delta));
                float ir = clamp01(isco / sR), nt = fmaxf(0, 1 - sqrtf(ir)), Tk = T->b2_temp * sqrtf(ir * sqrtf(ir)) * sqrtf(sqrtf(nt)) * delta, col[3];
                b2_bb_lut(Tk, col);
                float mx = fmaxf(col[0] * beam, fmaxf(col[1] * beam, col[2] * beam));
                if (mx > 1e-6f) {
                    if (m->ns >= m->cap) { m->cap = m->cap ? m->cap * 2 : (size_t)m->n * m->n * 2 + 256; m->s = realloc(m->s, m->cap * sizeof *m->s); }
                    B2S *o = &m->s[m->ns++];
                    o->x = (short)lrintf(fmaxf(-63, fminf(63, sP[0])) * 512); o->y = (short)lrintf(fmaxf(-63, fminf(63, sP[1])) * 512); o->z = (short)lrintf(fmaxf(-63, fminf(63, sP[2])) * 512);
                    o->om = (unsigned short)lrintf(fminf(65535, fabsf(Om) * 8192)); o->w = w * mx * (Om < 0 ? -1 : 1);
                    o->r = (unsigned char)lrintf(col[0] * beam / mx * 255); o->g = (unsigned char)lrintf(col[1] * beam / mx * 255); o->b = (unsigned char)lrintf(col[2] * beam / mx * 255);
                    ns++; alpha_hi += w * 4 * 0.75f;
                }
            }
        }
    }
    float rf = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]), dist = fabsf(rf - rph), ring = 0, ergo = 0;
    if (!hit) {
        ring = expf(-dist * 40) * 1.8f * lens;
        if (cross > 0) ring += expf(-dist * (60 + cross * 30.0f)) * expf(-(float)cross) * 1.2f * lens;
        if (fabsf(a) > 0.1f) { float ct = p[1] / fmaxf(rf, 0.001f), re = 1 + sqrtf(fmaxf(0, 1 - a * a * ct * ct)); ergo = expf(-fabsf(rf - re) * 20) * 0.35f * fabsf(a); }
    }
    if (!m) return ring;
    m->off[pix] = (unsigned)first; m->cnt[pix] = (unsigned char)ns; m->hit[pix] = (unsigned char)hit;
    m->ring[pix] = ring; m->ergo[pix] = ergo;
    for (int q = 0; q < 3; q++) m->dir[pix * 3 + q] = hit ? 0 : (short)lrintf(vv[q] * 32767);
    unsigned char *st = m->star + pix * 3;
    st[0] = st[1] = st[2] = 0;
    if (!hit) { /* its starfield, in the direction the ray leaves in (bright ones rare, small ones more) */
        float sc[3] = {0, 0, 0}, c[3];
        float cx = floorf(vv[0] * 200), cy = floorf(vv[1] * 200), cz = floorf(vv[2] * 200), h = b2_hash3(cx, cy, cz);
        if (h > 0.998f) { b2_starcol(b2_hash3(cx + 127.1f, cy + 127.1f, cz + 127.1f) * 2.4f - 0.4f, c); for (int q = 0; q < 3; q++) sc[q] += c[q] * powf(h, 10) * 2; }
        cx = floorf(vv[0] * 500); cy = floorf(vv[1] * 500); cz = floorf(vv[2] * 500); h = b2_hash3(cx, cy, cz);
        if (h > 0.996f) { b2_starcol(b2_hash3(cx + 217.3f, cy + 217.3f, cz + 217.3f) * 2.4f - 0.4f, c); for (int q = 0; q < 3; q++) sc[q] += c[q] * powf(h, 20) * 1.5f; }
        for (int q = 0; q < 3; q++) st[q] = (unsigned char)lrintf(fminf(1, sc[q] / 2.5f) * 255);
    }
    return ring;
}
/* build (a little more of) map m for tilt idx at size n: about `budget` rays; the ring's profile every pstep-th radius */
static void b2_build(B2Map *m, int idx, int n, int budget, int pstep) {
    if (m->ready) return;
    if (!m->off) {
        size_t np = (size_t)n * n;
        m->n = n; m->row = 0; m->idx = idx; m->used = g_b2_clock;
        m->off = malloc(np * sizeof *m->off); m->cnt = malloc(np); m->hit = malloc(np); m->star = malloc(np * 3);
        m->ring = malloc(np * sizeof *m->ring); m->ergo = malloc(np * sizeof *m->ergo); m->dir = malloc(np * 3 * sizeof *m->dir);
        m->prof = malloc((size_t)B2_RA * B2_RB * sizeof *m->prof); m->prow = 0;
        if (!m->off || !m->cnt || !m->hit || !m->star || !m->ring || !m->ergo || !m->dir || !m->prof) { b2_free(m); return; }
    }
    float E = BH_TUNE.b2_ext, incr = idx * B2_STEP / 57.29578f;
    int rows = (int)fmaxf(1, (float)budget / n);
    for (int k = 0; k < rows && m->row < n; k++, m->row++) {
        float v = E - (m->row + 0.5f) * 2 * E / n;
        for (int i = 0; i < n; i++) b2_ray(m, (size_t)m->row * n + i, incr, -E + (i + 0.5f) * 2 * E / n, v);
    }
    if (m->row < n) return;
    int angs = (int)fmaxf(1, (float)budget * pstep / B2_RB); /* then the ring's profile: rays along each angle */
    for (int k = 0; k < angs && m->prow < B2_RA; k++, m->prow++) {
        float th = (m->prow + 0.5f) * 6.2831853f / B2_RA, cu = cosf(th), sv = sinf(th), *P = m->prof + m->prow * B2_RB;
        for (int i = 0; i < B2_RB; i += pstep) {
            float b = B2_RLO + (i + 0.5f * pstep) * (B2_RHI - B2_RLO) / B2_RB, val = b2_ray(NULL, 0, incr, b * cu, b * sv);
            for (int q = i; q < i + pstep && q < B2_RB; q++) P[q] = val;
        }
    }
    if (m->prow >= B2_RA) { /* done: each bin takes its neighbours' brightest (the ring is thinner than a bin: never missed between two) */
        static float row[B2_RB];
        for (int a = 0; a < B2_RA; a++) {
            float *P = m->prof + a * B2_RB;
            for (int i = 0; i < B2_RB; i++) row[i] = fmaxf(P[i], fmaxf(i ? P[i - 1] : 0, i + 1 < B2_RB ? P[i + 1] : 0));
            memcpy(P, row, sizeof row);
        }
        m->ready = 1;
    }
}
/* the maps: built for the view a hole shows now (and kept), a little each call */
BhGpuImg BH_GPU_IMG;
void bh2_tick(const float *axis) {
    if (BH_TUNE.b2_engine < 0.5f || BH_GPU_ON) return;
    if (!g_b2_tex_ok) { unsigned s = 0x2545F491u; for (int i = 0; i < 256 * 256; i++) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; g_b2_tex[i] = (unsigned char)(s >> 7); } g_b2_tex_ok = 1; }
    float key[12]; b2_key_now(key);
    if (memcmp(key, g_b2_key, sizeof key)) { for (int i = 0; i < B2_NI; i++) { b2_free(&g_b2[i]); b2_free(&g_b2c[i]); } memcpy(g_b2_key, key, sizeof key); }
    float inc = acosf(fmaxf(-1, fminf(1, axis[2]))) * 57.29578f;
    if (inc > 90) inc = 180 - inc;
    int want = (int)lrintf(inc / B2_STEP);
    if (want < 0) want = 0;
    if (want >= B2_NI) want = B2_NI - 1;
    g_b2_want = want;
    if (want == g_b2_last) g_b2_still++; else { g_b2_last = want; g_b2_still = 0; }
    B2Map *c = &g_b2c[want];
    if (!c->ready) { b2_build(c, want, B2_CN, 3000, B2_CPS); return; } /* the rough one, at once (a tick or two) */
    B2Map *m = &g_b2[want];
    m->used = ++g_b2_clock;
    if (!m->ready && g_b2_still >= 8) { /* resting on this tilt: its full map, a little each call */
        int n = (int)b2_res();
        if (n < 32) n = 32;
        if (n > 512) n = 512;
        if (!m->off) { /* new: room for it, the least used one going if too many are kept */
            int kept = 0, lru = -1;
            for (int i = 0; i < B2_NI; i++) if (g_b2[i].off && &g_b2[i] != m) { kept++; if (lru < 0 || g_b2[i].used < g_b2[lru].used) lru = i; }
            if (kept >= B2_KEEP && lru >= 0) b2_free(&g_b2[lru]);
        }
        b2_build(m, want, n, 360, 1);
        return;
    }
    for (int d = 1; d < B2_NI; d++) for (int sg = -1; sg <= 1; sg += 2) { /* else the rough maps for the other tilts, the nearest first */
        int i = want + sg * d;
        if (i < 0 || i >= B2_NI || g_b2c[i].ready) continue;
        b2_build(&g_b2c[i], i, B2_CN, 360, B2_CPS);
        return;
    }
}
/* how far the map asked for last is built, 0..100 (100: ready; -1: none asked for) */
int bh2_progress(void) {
    if (g_b2_want < 0) return -1;
    const B2Map *m = &g_b2[g_b2_want];
    if (m->ready) return 100;
    if (!m->n) return 0;
    return (int)((m->row + (float)m->prow / B2_RA * m->n * 0.4f) * 100 / (m->n * 1.4f));
}
/* the best map ready for this axis: the one asked for, else the nearest built; 0 if none */
static B2Map *b2_pick(const float *axis, int *mirror) {
    float inc = acosf(fmaxf(-1, fminf(1, axis[2]))) * 57.29578f;
    *mirror = inc > 90;
    if (inc > 90) inc = 180 - inc;
    float want = inc / B2_STEP;
    int wi = (int)lrintf(want);
    if (wi < 0) wi = 0;
    if (wi >= B2_NI) wi = B2_NI - 1;
    B2Map *best = g_b2[wi].ready ? &g_b2[wi] : g_b2c[wi].ready ? &g_b2c[wi] : NULL; float bd = 1e9f;
    if (!best) for (int i = 0; i < B2_NI; i++) {
        if (g_b2[i].ready && fabsf(i - want) < bd) { bd = fabsf(i - want); best = &g_b2[i]; }
        if (g_b2c[i].ready && fabsf(i - want) < bd - 0.01f) { bd = fabsf(i - want); best = &g_b2c[i]; }
    }
    if (best) best->used = ++g_b2_clock;
    return best;
}
/* map pixel j shaded now: its light (rgb, tone-mapped) and how much of the hole's shadow (and dense disc) is there */
typedef struct { const B2Map *m; float *col, *tr; unsigned char *done; float spin, dens, expo, E, star_r; const BHView *V; } B2Shade;
static void b2_shade_px(B2Shade *S, size_t j) {
    if (S->done[j]) return;
    S->done[j] = 1;
    const B2Map *m = S->m;
    const BHView *V = S->V;
    float acc[3] = {0, 0, 0}, al = 0, heat = V->heat;
    for (int i = 0; i < m->cnt[j]; i++) {
        const B2S *q = &m->s[m->off[j] + i];
        float x = q->x / 512.0f, y = q->y / 512.0f, z = q->z / 512.0f, om = q->om / 8192.0f * (q->w < 0 ? -1 : 1), w = fabsf(q->w);
        float ang = om * S->spin, c = cosf(ang), s = sinf(ang), nx = (x * c - z * s) * 0.75f, nz = (x * s + z * c) * 0.75f, ny = y * 0.75f;
        float turb = b2_noise(nx, ny, nz) * 0.5f + b2_noise(nx * 2.5f, ny * 2.5f, nz * 2.5f) * 0.25f, d = turb * w * S->dens;
        float boost = 1;
        if (V->act == HA_SPOT || V->act == HA_FLARE || heat > 0.01f) { /* our actions, on its gas: a hot spot, a flare, a torn star's heat */
            float r = sqrtf(x * x + z * z);
            if (V->act == HA_FLARE) { float t = V->at - BH_FLARE_SNAP; /* (as the GPU's: the disc warms as its loop rises, then flashes as it snaps, the flash running outward) */
                if (t > 0) { float fr = 5 + 6 * t, fw = 0.8f + 0.6f * t; boost += 3 * V->fo * expf(-t * 1.1f) * (expf(-(r - fr) * (r - fr) / (fw * fw)) + expf(-t * 3) * expf(-(r - 5) * (r - 5) / 6)); }
                else boost += 0.6f * V->fo * clamp01(1 + t / BH_FLARE_SNAP) * expf(-(r - 6.5f) * (r - 6.5f) / 4); }
            boost += heat * 2 * expf(-(r - 7) * (r - 7) / 30);
        }
        float col = d * (1 - al) * boost;
        acc[0] += col * q->r / 255.0f; acc[1] += col * q->g / 255.0f; acc[2] += col * q->b / 255.0f;
        al += d;
        if (al > 0.99f) break;
    }
    float uu = -S->E + ((j % m->n) + 0.5f) * 2 * S->E / m->n, vv = S->E - ((j / m->n) + 0.5f) * 2 * S->E / m->n, bb = sqrtf(uu * uu + vv * vv);
    float sf = clamp01((S->star_r - bb) / (S->star_r * 0.45f)); sf = sf * sf * (3 - 2 * sf); /* (the stars fade out well inside the window) */
    float tr = fmaxf(0, 1 - al), eg = m->ergo[j] * tr;
    (void)sf;
    float o[3] = {acc[0] + eg * 0.3f, acc[1] + eg * 0.35f, acc[2] + eg * 0.9f};
    float *out = S->col + j * 4;
    for (int q = 0; q < 3; q++) { /* its ACES curve and gamma */
        float v = o[q] * S->expo, a = (v * (2.51f * v + 0.03f)) / (v * (2.43f * v + 0.59f) + 0.14f);
        out[q] = powf(clamp01(a), 0.4545f);
    }
    out[3] = m->hit[j] ? 1.0f : clamp01(al);
    S->tr[j] = tr;
}
/* how bright a black hole's light is now: pointed at, carried, its gas pushed round by the wheel; dim when you are away,
 * a flare when you are back */
float bh_hole_expo(const Session *s, double now) {
    const HoleRS *H = &s->mo.rs.hole;
    float wake = (float)(now - s->woke_at), fl = wake >= 0 && wake < 3 ? 1.6f * expf(-wake * 1.6f) * fminf(1, wake * 6) : 0;
    return BH_TUNE.b2_expo * (1 + 0.25f * s->hover + 0.4f * s->carry) * (1 + 0.05f * fminf(fabsf(H->flow_v), 12)) * (1 - 0.7f * s->quiet) * (1 + fl);
}
/* the black hole's scale, pixels per M, for a shell R pixels in radius: small as it opens, then once the shell is
 * swallowed (grow) the shell's own size is BH_SHELL_M */
static float bh_kpx(const Motion *mo, float R) {
    const HoleRS *H = &mo->rs.hole;
    float g0 = clamp01(H->grow), gr = g0 * g0 * (3 - 2 * g0), rs0 = R * 0.17f * (1 + 0.25f * mo->hole);
    return (rs0 + (R * BH_BC / BH_SHELL_M - rs0) * gr) / BH_BC;
}
/* how far from the middle of a px-square picture of s its edge is, M (as draw_session lays it out, no label) */
float bh_hole_E(const Session *s, int px) {
    float m = (float)px, ws = s->w_scale > 0 ? s->w_scale : 1;
    if (s->pad > 1) m /= s->pad;
    float R = m * SHELL_FRAC * ws * (s->mo.scale > 0 ? s->mo.scale : 1);
    return px * 0.5f / fmaxf(1e-3f, bh_kpx(&s->mo, R));
}
static void draw_hole(Canvas *c, Clip cl, const Session *s, const Motion *mo, float ox, float oy, float R, float ks, double t, double now, float room) {
    bh_lut();
    (void)ks;
    const HoleRS *H = &mo->rs.hole;
    /* the shadow's radius in pixels: small as it opens, then once the shell is swallowed (grow) the whole picture grows
     * until the disc reaches the tile's edge */
    float hb = mo->hole, k = bh_kpx(mo, R); /* pixels per M */
    /* Backdrop (Settings > Look, as the cores' band): a soft dark sky under all of its light, from the blurred light, so
     * the disc, the ring, a torn star and the lensed stars read on a light app; never per star (each a grey speck) */
    const float bdk = s->backdrop > 0.01f ? fminf(1, 0.95f * s->backdrop * BACKDROP_A) : 0;
    BHView V = {0};
    V.rout = fminf(17, room * 0.85f / k); /* the disc as far as the picture has room for, fading before its edge */
    int act = H->act;
    float at = act ? (float)(now - H->act_at) : 0, au = act ? clamp01(at / HOLE_ACT_DUR[act]) : 0;
    V.act = act; V.at = at; V.au = au; V.p = H->act_p; V.q = H->act_q;
    V.fo = act ? clamp01((float)(H->act_end - now) / 0.9f) : 0; V.fo = V.fo * V.fo * (3 - 2 * V.fo); /* (ending, or cut short: it fades) */
    float tf = (float)t, flick = 1 + 0.05f * sinf(tf * 1.3f) + 0.03f * sinf(tf * 3.7f + 1);
    V.bright = 1.5f * flick * (1 + 0.25f * s->hover + 0.4f * s->carry);
    V.T = H->flow; V.heat = H->tde.heat; V.pxm = 1 / k;
    { float sp = 1 + 1.2f * s->hover + 0.8f * s->carry + H->flow_v; V.dir = tanhf(sp * 1.5f); V.hot = 1 + 0.02f * fminf(fabsf(H->flow_v), 10); } /* pushed round backward: the other side comes at you */
    { float lk = sqrtf(s->look_x * s->look_x + s->look_y * s->look_y) + 1e-3f; V.side_lit[0] = s->look_x / lk; V.side_lit[1] = -s->look_y / lk; V.lit = 0.7f * s->hover * clamp01(lk * 1.5f); } /* pointed at: brighter on your side */
    bh_hole_axis(s, now, V.n);
    { float l; for (int q = 0; q < 3; q++) V.ea[q] = (q == 2) - V.n[2] * V.n[q]; l = sqrtf(V.ea[0] * V.ea[0] + V.ea[1] * V.ea[1] + V.ea[2] * V.ea[2]); for (int q = 0; q < 3; q++) V.ea[q] /= l; }
    V.ec[0] = V.n[1] * V.ea[2] - V.n[2] * V.ea[1]; V.ec[1] = V.n[2] * V.ea[0] - V.n[0] * V.ea[2]; V.ec[2] = V.n[0] * V.ea[1] - V.n[1] * V.ea[0];
    /* the sky behind it (Lensing, a switch: HOLE_ACTS bit HA_LENS): it drifts slowly, and turns as you turn the hole (the
     * view goes round it, so what lies behind changes) */
    V.sky = (HOLE_ACTS >> HA_LENS & 1) ? hb : 0;
    { float hl = sqrtf(V.n[0] * V.n[0] + V.n[1] * V.n[1]) + 1e-4f, sg = V.n[2] >= 0 ? 1.0f : -1.0f; V.half[0] = sg * V.n[0] / hl; V.half[1] = sg * V.n[1] / hl; V.half_k = clamp01(fabsf(V.n[2]) * 7 + 0.55f); }
    { float tx = atan2f(V.n[0], V.n[1]) + 0.06f * V.n[0], ty = asinf(fmaxf(-1, fminf(1, V.n[2]))) - 0.26f; V.ring_c[0] = 0.35f * tx; V.ring_c[1] = 0.35f * ty; } /* (a little off the line as it rests; more as you turn it) */
    V.sky_off[0] = 0.03f * tf + 1.2f * atan2f(V.n[0], V.n[1]); V.sky_off[1] = 0.006f * tf + 0.6f * asinf(fmaxf(-1, fminf(1, V.n[2]))); /* (the sky turns round it, and drifts in and out, as the view goes round) */
    /* its reach on screen, and a coarse picture of it for the glow round the light and the dark under it */
    float ex = fminf(room, (V.rout + 1.5f) * k + 0.2f * R), ey = fminf(room, fmaxf(BH_BC * 1.9f, sqrtf(4.6f * V.rout) + 2.5f) * k + 0.2f * R); /* (room for the glow) */
    { float sy = sqrtf(fmaxf(0, 1 - V.n[1] * V.n[1])), sx = sqrtf(fmaxf(0, 1 - V.n[0] * V.n[0])); /* the disc's outline, however it is turned */
      ey = fminf(room, fmaxf(ey, (V.rout * sy + 2.5f) * k + 0.2f * R)); ex = fminf(room, fmaxf(ex, (V.rout * sx + 2.5f) * k + 0.2f * R)); }
    if (HOLE_ACTS >> HA_LENS & 1) ey = fminf(room, fmaxf(ey, ex * 0.9f)); /* (room round it for the stars, a round field) */
    V.sky_lim[0] = ex / k; V.sky_lim[1] = ey / k; /* (the stars keep to the box as it rests, whatever an action needs) */
    if (act == HA_FEED || act == HA_LENS || act == HA_FLARE) ex = ey = room; /* (out to the picture's edge) */
    V.zmax = room / k; V.room = room;
    int b2_mir = 0, gpu = BH_GPU_IMG.rgba && BH_GPU_IMG.s == s && BH_GPU_IMG.n > 1;
    B2Map *bm = !gpu && BH_TUNE.b2_engine > 0.5f ? b2_pick(V.n, &b2_mir) : NULL;
    if (bm || gpu) ex = ey = room; /* (its disc is wider and thicker: the whole picture, faded before its edge) */
    int bx0 = (int)fmaxf(cl.x0, floorf(ox - ex)), bx1 = (int)fminf(cl.x1, ceilf(ox + ex) + 1);
    int by0 = (int)fmaxf(cl.y0, floorf(oy - ey)), by1 = (int)fminf(cl.y1, ceilf(oy + ey) + 1);
    if (bx0 >= bx1 || by0 >= by1) return;
    touch(c, bx0, by0, bx1, by1);
    if (bm || gpu) { /* traced after blackhole-simulation (its map for this tilt, shaded now; or the GPU's picture) */
        int n = gpu ? BH_GPU_IMG.n : bm->n;
        float E = gpu ? BH_GPU_IMG.E : BH_TUNE.b2_ext, rho = atan2f(V.n[0], V.n[1]), cr = cosf(rho), sr = sinf(rho), fu = 1, /* (the wheel only turns its gas, never mirrors it) */ fv = b2_mir ? -1.0f : 1.0f;
        if (gpu) { fv = 1; fu = 1; cr = 1; sr = 0; } /* (its camera goes below the disc itself; its picture comes turned) */
        size_t nb = gpu ? 1 : (size_t)n * n;
        B2Shade S = {bm, NULL, NULL, NULL, -V.T * 1.2f * BH_TUNE.b2_speed, /* (its gas the way the GPU's goes: the way its Doppler boost says) */ BH_TUNE.b2_dens, bh_hole_expo(s, now), E, fminf(V.sky_lim[0], V.sky_lim[1]) * 0.96f, &V};
        /* On the CPU the map is shaded once a picture, shared by all its bands (the threads drawing it a band of rows
         * each): the first to get here sets it up, and every band takes rows of it to shade until none are left, then
         * waits for the rest. (Each band shading the whole picture's part of the map again was most of the CPU's black
         * hole.) */
        static atomic_flag b2c_lock = ATOMIC_FLAG_INIT;
        static atomic_int b2c_next, b2c_rows_done;
        static float *b2c_col, *b2c_tr, b2c_key[8]; static unsigned char *b2c_done; static size_t b2c_nb; static const void *b2c_s, *b2c_bm; static int b2c_i0, b2c_i1;
        if (gpu) {
            S.col = malloc(nb * 4 * sizeof(float)); S.tr = malloc(nb * sizeof(float)); S.done = calloc(nb, 1);
            if (!S.col || !S.tr || !S.done) { free(S.col); free(S.tr); free(S.done); return; }
        } else {
            while (atomic_flag_test_and_set(&b2c_lock)) {}
            if (nb != b2c_nb) {
                free(b2c_col); free(b2c_tr); free(b2c_done);
                b2c_col = malloc(nb * 4 * sizeof(float)); b2c_tr = malloc(nb * sizeof(float)); b2c_done = calloc(nb, 1);
                b2c_nb = b2c_col && b2c_tr && b2c_done ? nb : 0; b2c_s = NULL;
                if (!b2c_nb) { atomic_flag_clear(&b2c_lock); return; }
            }
            S.col = b2c_col; S.tr = b2c_tr; S.done = b2c_done;
        }
        /* the screen (M, y up) to the map: turned by the axis's roll, mirrored for a view from below or a disc turned back */
#define B2_AT(X, Y, OUT) do { \
            float u_ = ((X) * cr - (Y) * sr) * fu, v_ = ((X) * sr + (Y) * cr) * fv, fi_ = (u_ + E) / (2 * E) * n - 0.5f, fj_ = (E - v_) / (2 * E) * n - 0.5f; \
            int i_ = (int)floorf(fi_), j_ = (int)floorf(fj_); float wi_ = fi_ - i_, wj_ = fj_ - j_; \
            (OUT)[0] = (OUT)[1] = (OUT)[2] = (OUT)[3] = 0; \
            if (gpu) { fj_ = (v_ + E) / (2 * E) * n - 0.5f; j_ = (int)floorf(fj_); wj_ = fj_ - j_; } /* (its picture: the bottom row first) */ \
            for (int q_ = 0; q_ < 4; q_++) { int ii_ = i_ + (q_ & 1), jj_ = j_ + (q_ >> 1); \
                if (ii_ < 0 || jj_ < 0 || ii_ >= n || jj_ >= n) continue; \
                size_t k_ = (size_t)jj_ * n + ii_; \
                float ww_ = ((q_ & 1) ? wi_ : 1 - wi_) * ((q_ >> 1) ? wj_ : 1 - wj_); \
                if (gpu) { const unsigned char *g_ = BH_GPU_IMG.rgba + k_ * 4; for (int c_ = 0; c_ < 4; c_++) (OUT)[c_] += g_[c_] * (1 / 255.0f) * ww_; continue; } \
                b2_shade_px(&S, k_); \
                for (int c_ = 0; c_ < 4; c_++) (OUT)[c_] += S.col[k_ * 4 + c_] * ww_; } \
        } while (0)
        enum { GM = 44 };
        float gl[GM * GM], tmp[GM * GM];
        float gx0 = ox - ex, gy0 = oy - ey, gs = fmaxf(2 * ex, 2 * ey) / (GM - 1);
        int gw = (int)(2 * ex / gs) + 1, gh = (int)(2 * ey / gs) + 1; if (gw > GM) gw = GM; if (gh > GM) gh = GM;
        if (!gpu) {
            float key[8] = {(float)fmod(now, 1000.0), ox, oy, k, R, room, ex, (float)b2_mir};
            if (b2c_s != s || b2c_bm != bm || memcmp(key, b2c_key, sizeof key)) { /* the first band of a new picture: its part of the map to shade */
                memset(S.done, 0, nb);
                float rM = 1.42f * fmaxf(ex, ey) / k + 1;
                int i0 = (int)floorf((E - rM) / (2 * E) * n) - 2, i1 = (int)ceilf((E + rM) / (2 * E) * n) + 2;
                b2c_i0 = i0 < 0 ? 0 : i0; b2c_i1 = i1 > n ? n : i1;
                atomic_store(&b2c_rows_done, 0); atomic_store(&b2c_next, b2c_i0);
                memcpy(b2c_key, key, sizeof key); b2c_s = s; b2c_bm = bm;
            }
            int i0 = b2c_i0, i1 = b2c_i1;
            atomic_flag_clear(&b2c_lock);
            for (;;) { int j = atomic_fetch_add(&b2c_next, 1); if (j >= i1) break; for (int i = i0; i < i1; i++) b2_shade_px(&S, (size_t)j * n + i); atomic_fetch_add(&b2c_rows_done, 1); }
            while (atomic_load(&b2c_rows_done) < i1 - i0) {} /* (the last rows, others' still going: short) */
        }
        {
            for (int j = 0; j < gh; j++) for (int i = 0; i < gw; i++) { float L4[4]; B2_AT((gx0 + i * gs - ox) / k, -(gy0 + j * gs - oy) / k, L4); gl[j * GM + i] = 0.3f * L4[0] + 0.55f * L4[1] + 0.15f * L4[2]; }
            int br = (int)fmaxf(1, BH_TUNE.bloom_r * R / gs);
            for (int pass = 0; pass < 2; pass++) {
                for (int j = 0; j < gh; j++) for (int i = 0; i < gw; i++) { float a = 0; int m2 = 0; for (int d = -br; d <= br; d++) { int ii = i + d; if (ii >= 0 && ii < gw) { a += gl[j * GM + ii]; m2++; } } tmp[j * GM + i] = a / m2; }
                for (int j = 0; j < gh; j++) for (int i = 0; i < gw; i++) { float a = 0; int m2 = 0; for (int d = -br; d <= br; d++) { int jj = j + d; if (jj >= 0 && jj < gh) { a += tmp[jj * GM + i]; m2++; } } gl[j * GM + i] = a / m2; }
            }
        }
        if (gpu && fabsf(k * 2 * E / n - 1) < 0.003f) { /* the GPU's picture at the core's own scale: pixel for pixel */
            float rr0 = room * 0.88f;
            for (int y = by0; y < by1; y++) {
                int j = (int)lrintf(oy + n * 0.5f - 1 - y);
                if (j < 0 || j >= n) continue;
                const unsigned char *row = BH_GPU_IMG.rgba + (size_t)j * n * 4;
                float dy_ = y + 0.5f - oy, fj = (y + 0.5f - gy0) / gs; int jg = (int)fj; float wj = fj - jg;
                int ioff = (int)lrintf(n * 0.5f - ox);
                for (int x = bx0; x < bx1; x++) {
                    int i = x + ioff;
                    if (i < 0 || i >= n) continue;
                    const unsigned char *q = row + (size_t)i * 4;
                    float fi = (x + 0.5f - gx0) / gs; int ig = (int)fi; float wi = fi - ig, g = 0;
                    if (ig >= 0 && jg >= 0 && ig < gw - 1 && jg < gh - 1) g = (gl[jg * GM + ig] * (1 - wi) + gl[jg * GM + ig + 1] * wi) * (1 - wj) + (gl[(jg + 1) * GM + ig] * (1 - wi) + gl[(jg + 1) * GM + ig + 1] * wi) * wj;
                    if (!(q[0] | q[1] | q[2] | q[3]) && g < 0.002f) continue;
                    float dx_ = x + 0.5f - ox, d2 = dx_ * dx_ + dy_ * dy_, ef = d2 < rr0 * rr0 ? 1 : clamp01((room - 1 - sqrtf(d2)) / (room * 0.12f));
                    float *p = c->p + 3 * ((size_t)y * (size_t)c->w + (size_t)x), sh = q[3] * (1 / 255.0f);
                    if (sh > 0) { float a = sh * hb; p[0] *= 1 - a; p[1] *= 1 - a; p[2] *= 1 - a; }
                    float gb = BH_TUNE.bloom * g * ef * (1 - 0.95f * sh), lf = ef * hb * (1 / 255.0f);
                    p[0] += q[0] * lf + gb * hb; p[1] += q[1] * lf + gb * 0.55f * hb; p[2] += q[2] * lf + gb * 0.22f * hb;
                    if (c->dk) { /* dark under the shadow and the gas only: under the glow it is a brown haze on a light desktop, under a star a grey speck */
                        float lum = (0.3f * q[0] + 0.55f * q[1] + 0.15f * q[2]) * (1 / 255.0f), pl = clamp01((g - 0.05f) * 4), d = fmaxf(fmaxf(sh, clamp01(lum * 1.5f) * clamp01(sh * 6)) * 0.92f, bdk * pl * (3 - 2 * pl)) * hb * ef, *dk = c->dk + (size_t)y * c->w + x; if (d > *dk) *dk = d; }
                }
            }
        } else
        for (int y = by0; y < by1; y++) for (int x = bx0; x < bx1; x++) {
            float X = (x + 0.5f - ox) / k, Y = -(y + 0.5f - oy) / k, L4[4];
            if (fabsf(X) > E || fabsf(Y) > E) continue;
            B2_AT(X, Y, L4);
            float sh = L4[3];
            if (L4[0] + L4[1] + L4[2] < 0.002f && sh < 0.002f && (gpu || V.sky <= 0 || X * X + Y * Y > S.star_r * S.star_r)) {
                /* (nothing here but maybe the glow: the cheap way) */
                float fi = (x + 0.5f - gx0) / gs, fj = (y + 0.5f - gy0) / gs; int i = (int)fi, j = (int)fj; float wi = fi - i, wj = fj - j, g = 0;
                if (i >= 0 && j >= 0 && i < gw - 1 && j < gh - 1) g = (gl[j * GM + i] * (1 - wi) + gl[j * GM + i + 1] * wi) * (1 - wj) + (gl[(j + 1) * GM + i] * (1 - wi) + gl[(j + 1) * GM + i + 1] * wi) * wj;
                if (g < 0.002f) continue;
                float dx_ = x + 0.5f - ox, dy_ = y + 0.5f - oy, ef = clamp01((room - 1 - sqrtf(dx_ * dx_ + dy_ * dy_)) / (room * 0.12f)), gb = BH_TUNE.bloom * g * ef * hb;
                float *p = c->p + 3 * ((size_t)y * (size_t)c->w + (size_t)x);
                p[0] += gb; p[1] += gb * 0.55f; p[2] += gb * 0.22f;
                continue;
            }
            if (!gpu) { /* the ring, crisp: its profile at this exact point, behind whatever disc lies in front; and the stars where
               * this ray leaves to, behind it too */
                float u_ = (X * cr - Y * sr) * fu, v_ = (X * sr + Y * cr) * fv, bb = sqrtf(u_ * u_ + v_ * v_), ring = 0;
                float fb = (bb - B2_RLO) / (B2_RHI - B2_RLO) * B2_RB - 0.5f;
                int ib = (int)floorf(fb);
                if (ib >= 0 && ib < B2_RB - 1) { /* (only near the shadow: atan2 is not cheap) */
                    float th = atan2f(v_, u_);
                    if (th < 0) th += 6.2831853f;
                    float fa = th / 6.2831853f * B2_RA - 0.5f, wb = fb - ib;
                    int ia = (int)floorf(fa); float wa = fa - ia;
                    int a0 = (ia % B2_RA + B2_RA) % B2_RA, a1 = (a0 + 1) % B2_RA;
                    const float *P = bm->prof;
                    ring = (P[a0 * B2_RB + ib] * (1 - wb) + P[a0 * B2_RB + ib + 1] * wb) * (1 - wa) + (P[a1 * B2_RB + ib] * (1 - wb) + P[a1 * B2_RB + ib + 1] * wb) * wa;
                }
                float fi_ = (u_ + E) / (2 * E) * n - 0.5f, fj_ = (E - v_) / (2 * E) * n - 0.5f;
                int i_ = (int)lrintf(fi_), j_ = (int)lrintf(fj_);
                float trn = 1, sky = 0, scol[3] = {0, 0, 0};
                if (i_ >= 0 && j_ >= 0 && i_ < n && j_ < n) {
                    size_t kk = (size_t)j_ * n + i_; b2_shade_px(&S, kk); trn = S.tr[kk];
                    if (V.sky > 0 && !BH_LIGHT && !bm->hit[kk]) { /* its starfield, at the exact direction (interpolated between the map's rays) */
                        float d3[3] = {0, 0, 0}; int i0 = (int)floorf(fi_), j0 = (int)floorf(fj_); float wi = fi_ - i0, wj = fj_ - j0, ws = 0;
                        for (int q = 0; q < 4; q++) { int ii = i0 + (q & 1), jj = j0 + (q >> 1); if (ii < 0 || jj < 0 || ii >= n || jj >= n || bm->hit[(size_t)jj * n + ii]) continue;
                            float w = ((q & 1) ? wi : 1 - wi) * ((q >> 1) ? wj : 1 - wj); const short *dd = bm->dir + ((size_t)jj * n + ii) * 3; for (int c3 = 0; c3 < 3; c3++) d3[c3] += dd[c3] / 32767.0f * w; ws += w; }
                        if (ws > 0.01f) { /* fixed on the screen: the direction turned into the map's camera frame, then the
                                           * roll undone; in pixels at the hole's full size, so they never move or grow with it */
                            float inc = (float)bm->idx * B2_STEP / 57.29578f, ci = cosf(inc), si = sinf(inc), c[3];
                            for (int c3 = 0; c3 < 3; c3++) d3[c3] /= ws;
                            float cx = d3[0], cy = d3[1] * si + d3[2] * ci, cz = -d3[1] * ci + d3[2] * si; /* (onto right, up, forward) */
                            if (cz > 0.05f) {
                                float mu = cx / cz * BH_TUNE.b2_dist * fu, mv = cy / cz * BH_TUNE.b2_dist * fv; /* (M on the hole's plane, undone of the flips) */
                                float sX = mu * cr + mv * sr, sY = -mu * sr + mv * cr, kf = R / BH_SHELL_M, qx = sX * kf, qy = -sY * kf;
                                float wl = b2_smooth(18, 7, bb); /* (bent near the hole only: further out the stars hold still) */
                                qx = (x + 0.5f - ox) + (qx - (x + 0.5f - ox)) * wl; qy = (y + 0.5f - oy) + (qy - (y + 0.5f - oy)) * wl;
                                for (int L = 0; L < 2; L++) {
                                    float cs = L ? 23 : 9, sg = L ? 0.8f : 0.55f, gx = floorf(qx / cs), gy = floorf(qy / cs);
                                    for (int jj = -1; jj <= 1; jj++) for (int ii = -1; ii <= 1; ii++) {
                                        int ux = (int)gx + ii, uy = (int)gy + jj;
                                        float h = bh_hash(ux * 3 + 11 + L * 7, uy * 5 + 3);
                                        if (h < (L ? 0.93f : 0.88f)) continue;
                                        float px_ = (ux + 0.15f + 0.7f * bh_hash(ux, uy + 31)) * cs, py_ = (uy + 0.15f + 0.7f * bh_hash(ux + 47, uy)) * cs;
                                        float d2 = ((qx - px_) * (qx - px_) + (qy - py_) * (qy - py_)) / (sg * sg);
                                        if (d2 > 9) continue;
                                        b2_starcol(bh_hash(ux + 61, uy + 61) * 2.4f - 0.4f, c);
                                        for (int c3 = 0; c3 < 3; c3++) scol[c3] += c[c3] * (L ? 1.6f : 0.7f) * (0.4f + h) * expf(-d2);
                                    }
                                }
                            }
                            float rs_ = sqrtf(X * X + Y * Y) * k / room, sf = clamp01((0.95f - rs_) / 0.35f); sky = sf * sf * (3 - 2 * sf) * BH_TUNE.star_a; /* (by the window, not the hole) */
                        }
                    }
                }
                float add = ring * BH_TUNE.ring_a * trn * S.expo;
                for (int q = 0; q < 3; q++) { float v = scol[q] * sky * trn + add; L4[q] = 1 - (1 - L4[q]) * (1 - clamp01(v)); } /* (white light laid over: the ring, a star) */
            }
            float fi = (x + 0.5f - gx0) / gs, fj = (y + 0.5f - gy0) / gs; int i = (int)fi, j = (int)fj; float wi = fi - i, wj = fj - j, g = 0;
            if (i >= 0 && j >= 0 && i < gw - 1 && j < gh - 1) g = (gl[j * GM + i] * (1 - wi) + gl[j * GM + i + 1] * wi) * (1 - wj) + (gl[(j + 1) * GM + i] * (1 - wi) + gl[(j + 1) * GM + i + 1] * wi) * wj;
            float ef = clamp01((room - 1 - hypotf(x + 0.5f - ox, y + 0.5f - oy)) / (room * 0.12f)); /* (nothing reaches the window's edge) */
            float gb = BH_TUNE.bloom * g * ef * (1 - 0.95f * clamp01(sh)), a_ = clamp01(sh) * hb; /* (no glow in the shadow) */
            float lum = 0.3f * L4[0] + 0.55f * L4[1] + 0.15f * L4[2], pl = clamp01((g - 0.05f) * 4), d_ = fmaxf(fmaxf(clamp01(sh), clamp01(lum * 1.5f) * clamp01(sh * 6)) * 0.92f, bdk * pl * (3 - 2 * pl)) * hb * ef; /* (as the GPU's: no dark under the glow or the stars) */
            float *p = c->p + 3 * ((size_t)y * (size_t)c->w + (size_t)x);
            if (sh > 0) { p[0] *= 1 - a_; p[1] *= 1 - a_; p[2] *= 1 - a_; }
            p[0] += (L4[0] * ef + gb) * hb; p[1] += (L4[1] * ef + gb * 0.55f) * hb; p[2] += (L4[2] * ef + gb * 0.22f) * hb;
            if (c->dk) { float *dk = c->dk + (size_t)y * c->w + x; if (d_ > *dk) *dk = d_; }
        }
#undef B2_AT
        if (gpu) { free(S.col); free(S.tr); free(S.done); }
    } else {
    /* the glow: the light on a coarse grid over the whole reach (not just this band's rows, so bands join up), blurred */
        enum { GM = 44 };
        float gl[GM * GM], tmp[GM * GM];
        float gx0 = ox - ex, gy0 = oy - ey, gs = fmaxf(2 * ex, 2 * ey) / (GM - 1);
        int gw = (int)(2 * ex / gs) + 1, gh = (int)(2 * ey / gs) + 1; if (gw > GM) gw = GM; if (gh > GM) gh = GM;
        float sky_keep = V.sky; V.sky = 0; /* (the glow is the disc's: no stars in it) */
        for (int j = 0; j < gh; j++) for (int i = 0; i < gw; i++) {
            float L[3], X = (gx0 + i * gs - ox) / k, Y = -(gy0 + j * gs - oy) / k;
            float tr; bh_ray(&V, X, Y, L, &tr);
            gl[j * GM + i] = fminf(2.5f, 0.3f * L[0] + 0.55f * L[1] + 0.15f * L[2]);
        }
        V.sky = sky_keep;
        int br = (int)fmaxf(1, BH_TUNE.bloom_r * R / gs); /* box blur, twice each way */
        for (int pass = 0; pass < 2; pass++) {
            for (int j = 0; j < gh; j++) for (int i = 0; i < gw; i++) { float a = 0; int n = 0; for (int d = -br; d <= br; d++) { int ii = i + d; if (ii >= 0 && ii < gw) { a += gl[j * GM + ii]; n++; } } tmp[j * GM + i] = a / n; }
            for (int j = 0; j < gh; j++) for (int i = 0; i < gw; i++) { float a = 0; int n = 0; for (int d = -br; d <= br; d++) { int jj = j + d; if (jj >= 0 && jj < gh) { a += tmp[jj * GM + i]; n++; } } gl[j * GM + i] = a / n; }
        }
        const float bloom = BH_TUNE.bloom;
        for (int y = by0; y < by1; y++) for (int x = bx0; x < bx1; x++) {
            float px = x + 0.5f - ox, py = y + 0.5f - oy, X = px / k, Y = -py / k, b = sqrtf(X * X + Y * Y), L[3], sh, tr = 0;
            if (fabsf(b - BH_BC) * k < 1.6f || (b > BH_BC && b < BH_BC + 0.7f)) { /* the shadow's edge and the thin rings: four rays */
                float a[3] = {0, 0, 0}; sh = 0;
                for (int q = 0; q < 4; q++) { float l[3], t1; sh += bh_ray(&V, (px + ((q & 1) - 0.5f) * 0.5f) / k, -(py + ((q >> 1) - 0.5f) * 0.5f) / k, l, &t1); a[0] += l[0]; a[1] += l[1]; a[2] += l[2]; tr += t1; }
                L[0] = a[0] / 4; L[1] = a[1] / 4; L[2] = a[2] / 4; sh /= 4; tr /= 4;
            } else sh = bh_ray(&V, X, Y, L, &tr);
            float fi = (x + 0.5f - gx0) / gs, fj = (y + 0.5f - gy0) / gs; int i = (int)fi, j = (int)fj; float wi = fi - i, wj = fj - j, g = 0;
            if (i >= 0 && j >= 0 && i < gw - 1 && j < gh - 1) g = (gl[j * GM + i] * (1 - wi) + gl[j * GM + i + 1] * wi) * (1 - wj) + (gl[(j + 1) * GM + i] * (1 - wi) + gl[(j + 1) * GM + i + 1] * wi) * wj;
            float *p = c->p + 3 * ((size_t)y * (size_t)c->w + (size_t)x);
            if (sh > 0) { float a = sh * hb; p[0] *= 1 - a; p[1] *= 1 - a; p[2] *= 1 - a; }
            float gb = bloom * g * (1 - 0.55f * sh), o[3] = {L[0] + gb, L[1] + gb * 0.5f, L[2] + gb * 0.16f}; /* (the glow lies over the shadow's edge, fainter) */
            for (int q = 0; q < 3; q++) p[q] += (1 - expf(-o[q] * 1.15f)) * hb; /* (a soft knee: the hottest light goes white, never flat) */
            if (c->dk) { /* the dark sky under it, so it reads on a light desktop too: solid in the shadow, soft round the light */
                float lum = 0.3f * L[0] + 0.55f * L[1] + 0.15f * L[2], d = fmaxf(fmaxf(sh, clamp01((BH_BC * 1.4f - b) / (BH_BC * 0.3f))), clamp01(g * 3.2f + lum * 1.5f)) * hb * 0.92f, *dk = c->dk + (size_t)y * c->w + x;
                if (d > *dk) *dk = d;
            }
        }
    }
    /* the actions drawn over the traced picture: points placed where their light really shows (both images; the bent one,
     * near the shadow on the far side, fainter and thinner) */
    float wpx = fmaxf(1.1f, 0.5f * k); /* a stream's half-width in pixels */
    /* a stream of gas, soft and clumpy: a wide faint glow and a bright core through the same points, fading toward
     * the picture's edge so nothing is cut */
#define EDGE_FADE(X, Y) clamp01((V.room - 2 - hypotf((X) - ox, (Y) - oy)) / (V.room * 0.22f))
    if (gpu) return; /* (the GPU drew the actions' gas itself: bh_gas_points) */
    { /* the actions' gas, the same points the GPU traces (bh_gas_points: the torn star and its stream, the flare's loop
       * and blob, gas pulled off a held press), and the hot spot spiralling in (bh_spot_state), each where its light
       * really shows: its direct image, and its bent one near the shadow on the far side, fainter and thinner */
        enum { GN = 80 };
        float P[GN][4], Q[GN][2];
        int gn = bh_gas_points(s, now, P, Q, 64);
        if (act == HA_SPOT) { /* (the GPU draws it in its shader; here it is a clump and its smeared tail) */
            float ang, rs, at2 = at, dur = HOLE_ACT_DUR[HA_SPOT], nn3[3], ea[3], ec[3], l;
            float sf = fminf(1, at2 / 0.6f) * clamp01((dur - at2) / 3.5f) * V.fo * hb;
            bh_spot_state(s, now, &ang, &rs);
            bh_hole_axis(s, now, nn3);
            if (fabsf(nn3[2]) > 0.999f) nn3[1] += 0.002f;
            for (int q = 0; q < 3; q++) ea[q] = (q == 2) - nn3[2] * nn3[q];
            l = sqrtf(ea[0] * ea[0] + ea[1] * ea[1] + ea[2] * ea[2]); for (int q = 0; q < 3; q++) ea[q] /= l;
            ec[0] = nn3[1] * ea[2] - nn3[2] * ea[1]; ec[1] = nn3[2] * ea[0] - nn3[0] * ea[2]; ec[2] = nn3[0] * ea[1] - nn3[1] * ea[0];
            float smear = 0.3f + 0.2f * at2;
            for (int i = 0; i < 9 && gn < GN; i++) {
                float u = i / 8.0f, a = ang - smear * u * u;
                for (int q = 0; q < 3; q++) P[gn][q] = rs * (cosf(a) * ea[q] + sinf(a) * ec[q]);
                P[gn][3] = sf * (i ? 2.2f * (1 - 0.8f * u) : 4); Q[gn][0] = 0.15f + 0.6f * u; Q[gn][1] = (i ? 1 : -1) * (0.45f + 0.2f * u);
                gn++;
            }
        }
        float sp[2][GN][2], al[2][GN], ag[2][GN]; int ok[2][GN]; Col cc[GN], cg[GN];
        for (int a0 = 0; a0 < gn;) {
            int a1 = a0 + 1;
            while (a1 < gn && Q[a1][1] >= 0) a1++; /* (one stream: up to where the next starts) */
            int m = a1 - a0;
            float ext = 0, rad = 0;
            for (int i = a0; i < a1; i++) { ext = fmaxf(ext, fabsf(P[i][0] - P[a0][0]) + fabsf(P[i][1] - P[a0][1]) + fabsf(P[i][2] - P[a0][2])); rad = fmaxf(rad, fabsf(Q[i][1])); }
            for (int i = 0; i < m; i++) {
                const float *pp = P[a0 + i];
                float tp = Q[a0 + i][0];
                cc[i] = mix((Col){0.72f, 0.84f, 1}, (Col){1, 0.5f, 0.16f}, tp); cg[i] = mix((Col){0.5f, 0.62f, 1}, (Col){1, 0.42f, 0.14f}, tp);
                for (int g = 0; g < 2; g++) {
                    float X, Y; ok[g][i] = bh_image(pp, g, &X, &Y);
                    if (ok[g][i]) { sp[g][i][0] = ox + X * k; sp[g][i][1] = oy - Y * k; }
                    al[g][i] = ok[g][i] ? fminf(1.2f, pp[3] * 0.55f) * (g ? 0.3f : 1) * EDGE_FADE(sp[g][i][0], sp[g][i][1]) : 0; ag[g][i] = al[g][i] * 0.35f;
                }
            }
            for (int g = 0; g < 2; g++) {
                if (ext < 0.05f) { /* (points on one spot: a ball of gas, a star, a flash) */
                    if (!ok[g][0]) continue;
                    float e = fminf(1.5f, P[a0][3] * 0.3f) * (g ? 0.35f : 1) * EDGE_FADE(sp[g][0][0], sp[g][0][1]), sz = rad * k * (g ? 0.6f : 1);
                    blob(c, cl, sp[g][0][0], sp[g][0][1], sz * 5, cg[0], 0.25f * e, cg[0], 0.08f * e, 0.15f);
                    blob(c, cl, sp[g][0][0], sp[g][0][1], sz * 1.4f, WHITE, 1.0f * e, cc[0], 0.7f * e, 0.45f);
                    continue;
                }
                float w = fmaxf(wpx * 0.6f, rad * k * 0.9f);
                bh_ribbon(c, cl, sp[g], ok[g], ag[g], cg, m, w * (g ? 1.6f : 3), R * 1.1f);
                bh_ribbon(c, cl, sp[g], ok[g], al[g], cc, m, w * (g ? 0.5f : 0.9f), R * 1.1f);
            }
            a0 = a1;
        }
    }
}
/* The actions' gas for the GPU to trace (win/gpu.c): points in the view's frame (x right, y up, z toward you; M), each
 * with how bright (P[i][3]), how hot (Q[i][0], 0: a star's blue-white .. 1: glowing orange) and how thick (Q[i][1], M;
 * negative: a new stream starts here). The torn star and its debris, the flare's loop, gas pulled off a held press:
 * the same paths the CPU draws as ribbons, now as gas the GPU bends round the hole and hides behind it. How many */
/* two points of a stream not to join with a straight tube: it would pass close by the hole (a chord across its shadow,
 * drawn in front of it as a line: a stream swinging round behind it is split there instead, and the hole hides it), or
 * be very long (a straight stick across the picture) */
static int gas_cuts(const float *a, const float *b) {
    float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, l2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    if (l2 > 14.0f * 14.0f) return 1;
    float t = l2 > 1e-8f ? clamp01(-(a[0] * d[0] + a[1] * d[1] + a[2] * d[2]) / l2) : 0, c[3] = {a[0] + d[0] * t, a[1] + d[1] * t, a[2] + d[2] * t};
    return c[0] * c[0] + c[1] * c[1] + c[2] * c[2] < 4.2f * 4.2f;
}
/* the hot spot spiralling in under gravity: from 8.5 M toward the disc's inner edge, faster as it goes (Kepler: its turn ~
 * r^-1.5; once round in 5 s at 7 M), the way the gas goes. Its angle (the disc's frame) and radius now */
void bh_spot_state(const Session *s, double now, float *ang, float *r) {
    const HoleRS *H = &s->mo.rs.hole;
    float at = H->act == HA_SPOT ? (float)(now - H->act_at) : 0, dur = HOLE_ACT_DUR[HA_SPOT], a = H->act_p * 3.14159265f, sg = BH_TUNE.b2_spin < 0 ? -1.0f : 1.0f;
    for (int k = 0; k < 100 && at > 0; k++) { float tk = at * (k + 0.5f) / 100, u = fminf(1, tk / dur), rk = 8.5f - 3.3f * u * u; a += sg * 6.2831853f / 5 * powf(7 / rk, 1.5f) * at / 100; }
    float u = fminf(1, at / dur);
    *ang = a; *r = 8.5f - 3.3f * u * u;
}
int bh_gas_points(const Session *s, double now, float (*P)[4], float (*Q)[2], int max) {
    const HoleRS *H = &s->mo.rs.hole;
    float nn[3], ea[3], ec[3], l;
    bh_hole_axis(s, now, nn);
    if (fabsf(nn[2]) > 0.999f) nn[1] += 0.002f;
    for (int q = 0; q < 3; q++) ea[q] = (q == 2) - nn[2] * nn[q];
    l = sqrtf(ea[0] * ea[0] + ea[1] * ea[1] + ea[2] * ea[2]); for (int q = 0; q < 3; q++) ea[q] /= l;
    ec[0] = nn[1] * ea[2] - nn[2] * ea[1]; ec[1] = nn[2] * ea[0] - nn[0] * ea[2]; ec[2] = nn[0] * ea[1] - nn[1] * ea[0];
    int act = H->act, n = 0, brk = 1;
    float at = act ? (float)(now - H->act_at) : 0, au = act ? clamp01(at / HOLE_ACT_DUR[act]) : 0, fo = act ? clamp01((float)(H->act_end - now) / 0.9f) : 0, hb = s->mo.hole;
    fo = fo * fo * (3 - 2 * fo);
#define GAS_DISC(D, BR, TP, RAD) do { if (n < max) { const float *d_ = (D); for (int q_ = 0; q_ < 3; q_++) P[n][q_] = d_[0] * ea[q_] + d_[1] * ec[q_] + d_[2] * nn[q_]; \
        if (!brk && n && gas_cuts(P[n - 1], P[n])) { brk = 1; } \
        P[n][3] = (BR); Q[n][0] = (TP); Q[n][1] = brk ? -(RAD) : (RAD); brk = 0; n++; } } while (0)
#define GAS_VIEW(D, BR, TP, RAD) do { if (n < max) { for (int q_ = 0; q_ < 3; q_++) P[n][q_] = (D)[q_]; \
        if (!brk && n && gas_cuts(P[n - 1], P[n])) { brk = 1; } \
        P[n][3] = (BR); Q[n][0] = (TP); Q[n][1] = brk ? -(RAD) : (RAD); brk = 0; n++; } } while (0)
    if (act == HA_FEED) {
        const TdeRS *T = &H->tde;
        float fade = bh_env(au, 0.04f, 0.2f) * hb * fo;
        { /* the star: a bright ball while it holds together; its gas, in order along it (each piece's orbital energy once
           * it is past its closest point; before that, how far in toward the hole), a ball still, then a stream as it tears */
            float st[3]; for (int q = 0; q < 3; q++) st[q] = T->s[q] + T->s[3 + q] * T->acc;
            float cr = sqrtf(st[0] * st[0] + st[1] * st[1] + st[2] * st[2]) + 1e-4f, in = fade * clamp01((23 - cr) / 4); /* (it comes in out of the dark) */
            float torn = clamp01((T->spread - 1.0f) / 2.5f);
            if (torn < 1) { GAS_DISC(st, in * 5 * (1 - torn), 0.05f, 0.75f); GAS_DISC(st, in * 5 * (1 - torn), 0.05f, 0.75f); brk = 1; }
            int ord[TDE_N], m = 0; float key[TDE_N];
            for (int i = 0; i < T->n; i++) {
                if (!T->on[i]) continue;
                float kk = T->split ? T->q[i] : -((T->p[i][0] - T->s[0]) * T->s[0] + (T->p[i][1] - T->s[1]) * T->s[1] + (T->p[i][2] - T->s[2]) * T->s[2]) / cr;
                int at2 = m++;
                while (at2 > 0 && key[at2 - 1] > kk) { key[at2] = key[at2 - 1]; ord[at2] = ord[at2 - 1]; at2--; }
                key[at2] = kk; ord[at2] = i;
            }
            for (int o = 0; o < m && n < max - 3; o++) {
                int i = ord[o];
                float q3[3]; for (int j = 0; j < 3; j++) q3[j] = T->p[i][j] + T->v[i][j] * T->acc;
                float r = sqrtf(q3[0] * q3[0] + q3[1] * q3[1] + q3[2] * q3[2]), sh = clamp01(T->hot[i] / 2);
                float life = clamp01((r - 5.5f) / 3) * clamp01((30 - r) / 8); /* (in close it has joined the disc, which flares with it) */
                GAS_DISC(q3, in * life * (0.3f + 0.7f * torn) * (0.45f + 1.6f * sh), 0.08f + 0.1f * torn + 0.75f * sh, 0.5f - 0.26f * torn + 0.25f * sh);
            }
            brk = 1;
            if (T->shock_amp > 0.05f) { /* the shock where the streams cross: a bright, swelling blob */
                float a = fade * fminf(1.2f, T->shock_amp * 0.7f), rad = 0.4f + 0.22f * T->shock_amp;
                GAS_DISC(T->shock, a, 0.75f, rad); GAS_DISC(T->shock, a, 0.75f, rad);
                brk = 1;
            }
        }
        brk = 1;
    }
    if (act == HA_FLARE) { /* a magnetic loop (as the Sun's, and the flares seen off Sgr A*): an arch of hot gas rises off the inner
                            * disc between two footpoints that ride round with it, twisting as it grows; at SNAP s its field
                            * reconnects: a flash at its top, the top flung outward and upward as a blob that cools from white
                            * to orange, the legs falling back into the disc (the shader flashes the disc then: bh_flare_snap) */
        const float SNAP = BH_FLARE_SNAP, R0 = 6.5f, SEP = 0.6f;
        float om = 1.2f * BH_TUNE.b2_speed / (powf(R0, 1.5f) + BH_TUNE.b2_spin), ph0 = H->act_p * 3.14159265f + om * at, f = hb * fo;
        float rise = clamp01(at / SNAP), hgt = 0.4f + 4.6f * rise * rise * (3 - 2 * rise), lit = 0.5f + 0.9f * rise;
        float ts = at - SNAP, fall = ts > 0 ? clamp01(ts / 0.9f) : 0;
        for (int i = 0; i < 17; i++) {
            float u = i / 16.0f, arc = sinf(3.14159265f * u);
            if (ts > 0 && u > 0.3f && u < 0.7f) { brk = 1; continue; } /* (snapped: its top is gone) */
            float az = ph0 + (u - 0.5f) * SEP * (1 + 0.3f * rise), tw = 0.35f * sinf(u * 9 + at * 4) * arc * rise;
            float rr = R0 + 1.3f * arc * rise + tw, h = hgt * arc * (1 - fall);
            float d[3] = {rr * cosf(az) - tw * sinf(az), rr * sinf(az) + tw * cosf(az), h};
            GAS_DISC(d, f * lit * (1 - fall) * (0.6f + 0.6f * arc), 0.5f + 0.25f * (1 - arc), 0.28f + 0.1f * arc);
        }
        brk = 1;
        if (ts > 0) { /* the flash where it snapped, then the blob flung out */
            float azt = H->act_p * 3.14159265f + om * SNAP, rt = R0 + 1.3f, ht = hgt; /* (where it snapped: the blob leaves from there) */
            float fl = expf(-ts * 4.5f) * 4.5f;
            float top[3] = {rt * cosf(azt), rt * sinf(azt), ht};
            if (fl > 0.05f) { GAS_DISC(top, f * fl, 0.1f, 0.5f + 0.9f * ts); GAS_DISC(top, f * fl, 0.1f, 0.5f + 0.9f * ts); brk = 1; }
            /* the blob, flung off under the hole's gravity (Paczynski-Wiita, as the torn star): launched outward, upward
             * and along the way the gas turns, a little slower than it would take to get away, so it climbs, slows and
             * curves round; worked out again from the snap each frame (a few hundred small steps) */
            float rr0 = sqrtf(rt * rt + ht * ht), ve = 0.85f * sqrtf(2 / (rr0 - 2)), bp[3] = {top[0], top[1], top[2]}, bv[3];
            { float out[3] = {cosf(azt), sinf(azt), 0}, up[3] = {0, 0, 1}, fw[3] = {-sinf(azt), cosf(azt), 0}, l2 = 0;
              for (int q = 0; q < 3; q++) { bv[q] = 0.55f * out[q] + 0.45f * up[q] + 0.7f * fw[q]; l2 += bv[q] * bv[q]; }
              for (int q = 0; q < 3; q++) bv[q] *= ve / sqrtf(l2); }
            const float hstep = 0.4f;
            float trail[3][3], tt = ts * BH_TS;
            int steps = (int)(tt / hstep), mark[3] = {steps, (int)(fmaxf(0, ts - 0.08f) * BH_TS / hstep), (int)(fmaxf(0, ts - 0.16f) * BH_TS / hstep)};
            for (int k = 0; k < 3; k++) memcpy(trail[k], bp, sizeof bp);
            for (int st = 1; st <= steps && st < 600; st++) { /* velocity Verlet */
                float a1[3], a2[3], r1 = sqrtf(bp[0] * bp[0] + bp[1] * bp[1] + bp[2] * bp[2]) + 1e-4f, g1 = -1 / ((r1 - 2) * (r1 - 2) * r1);
                for (int q = 0; q < 3; q++) { a1[q] = bp[q] * g1; bv[q] += a1[q] * hstep / 2; bp[q] += bv[q] * hstep; }
                float r2 = sqrtf(bp[0] * bp[0] + bp[1] * bp[1] + bp[2] * bp[2]) + 1e-4f, g2 = -1 / ((r2 - 2) * (r2 - 2) * r2);
                for (int q = 0; q < 3; q++) { a2[q] = bp[q] * g2; bv[q] += a2[q] * hstep / 2; }
                for (int k = 0; k < 3; k++) if (st == mark[k]) memcpy(trail[k], bp, sizeof bp);
            }
            float cool = clamp01(ts / 2.2f), br = f * 2.6f * expf(-ts * 0.55f), rad = 0.45f + 0.25f * clamp01(ts / 1.5f);
            for (int k = 0; k < 3; k++) GAS_DISC(trail[k], br * (1 - 0.3f * k), 0.15f + 0.8f * cool, rad * (1 - 0.15f * k)); /* (the blob and its wake) */
            brk = 1;
        }
    }
    { /* gas pulled off a held press, oldest first */
        const HoldRS *Hp = &H->hold;
        for (int j = 0; j < HS_N && n < max; j += 2) {
            int i = (Hp->head + j) % HS_N;
            if (Hp->age[i] <= 0) { brk = 1; continue; }
            float q3[3]; for (int d = 0; d < 3; d++) q3[d] = Hp->p[i][d] + Hp->v[i][d] * Hp->acc;
            float r = sqrtf(q3[0] * q3[0] + q3[1] * q3[1] + q3[2] * q3[2]), hot = clamp01((10 - r) / 5.5f);
            float life = clamp01((r - 4.5f) / 2) * clamp01(Hp->age[i] / 0.15f) * clamp01((7 - Hp->age[i]) / 1.5f) * hb;
            GAS_VIEW(q3, life * (0.8f + 1.2f * hot), 1 - 0.6f * hot, 0.35f);
        }
    }
#undef GAS_DISC
#undef GAS_VIEW
    return n;
}
/* Settings > Look > Backdrop, over a light app: a thin soft dark band along each line and round each node that is drawn
 * (Canvas.dk), like the outline that keeps subtitles readable on any picture. It follows whatever shape the lines are
 * in (a piece drawn into a needle, a cone, the broken shell), which a round halo never did. r: its reach in pixels,
 * solid for its inner part, then a soft edge; strongest on the near side, none on the far side (its lines would lay a
 * dark mesh across the shell) */
float BACKDROP_W = 1, BACKDROP_A = 1;
int BACKDROP_CORE;
static void shade_seg(Canvas *c, Clip cl, const float *a, const float *b, float r, float amt) {
    float zf = clamp01(((a[2] + b[2]) * 0.5f / CIRCUM + 0.35f) / 0.45f); /* near: 1; past the middle toward the back: 0 */
    amt *= zf * zf * (3 - 2 * zf);
    if (!c->dk || amt <= 0.003f || r < 0.5f) return;
    int x0 = (int)floorf(fminf(a[0], b[0]) - r), x1 = (int)ceilf(fmaxf(a[0], b[0]) + r) + 1, y0 = (int)floorf(fminf(a[1], b[1]) - r), y1 = (int)ceilf(fmaxf(a[1], b[1]) + r) + 1;
    if (x0 < cl.x0) x0 = cl.x0;
    if (y0 < cl.y0) y0 = cl.y0;
    if (x1 > cl.x1) x1 = cl.x1;
    if (y1 > cl.y1) y1 = cl.y1;
    if (x1 <= x0 || y1 <= y0) return;
    touch(c, x0, y0, x1, y1);
    float dx = b[0] - a[0], dy = b[1] - a[1], l2 = dx * dx + dy * dy, ir = 1 / r;
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) {
        float px = x + 0.5f - a[0], py = y + 0.5f - a[1], u = l2 > 1e-6f ? clamp01((px * dx + py * dy) / l2) : 0;
        float ex = px - dx * u, ey = py - dy * u, d = sqrtf(ex * ex + ey * ey) * ir;
        if (d >= 1) continue;
        float e = clamp01((1 - d) / 0.8f), sd = e * e * (3 - 2 * e) * amt, *k = c->dk + (size_t)y * c->w + x;
        if (sd > *k) *k = sd;
    }
}

/* READY's marks at the pointer (Bestiary): the lock-on closing round it while pieces turn and fly, and the burst of each
 * blow, impact lines thrown out, a shock ring, a white flash. (cx, cy): the stage's centre; U: pixels per stage unit */
static void pointer_fx(Canvas *c, Clip cl, const Session *s, float cx, float cy, float U, double now) {
    const Motion *mo = &s->mo;
    float k = U / 120.0f;
    if (mo->lock_on > 0.01f && s->aim_on) {
        float lmin = fmaxf(18 * k, s->aim_hit * U + 4 * k); /* closing down round the whole mouse (aim_x / aim_y: its middle; aim_hit reaches past it) */
        float px = cx + s->aim_x * U, py = cy + s->aim_y * U, lr = lmin + 22 * k * powf(1 - mo->lock_on, 3), la = (float)fmod(now * 1.6, 6.2831853), al = 0.75f * mo->lock_on;
        Col red = {1, 0.47f, 0.43f};
        for (int i = 0; i < 4; i++) {
            float g0 = la + i * 1.5707963f;
            for (int j = 0; j < 4; j++) { float a0 = g0 - 0.32f + 0.16f * j, a1 = a0 + 0.16f; glow_line(c, cl, px + cosf(a0) * lr, py + sinf(a0) * lr, px + cosf(a1) * lr, py + sinf(a1) * lr, red, al, red, 0.4f * al, 1.6f * k); }
            glow_line(c, cl, px + cosf(g0) * lr, py + sinf(g0) * lr, px + cosf(g0) * (lr - 7 * k), py + sinf(g0) * (lr - 7 * k), red, al, red, 0.4f * al, 1.6f * k);
        }
    }
    for (int i = 0; i < 4; i++) {
        float u = (float)(now - mo->imp[i].at) / 0.3f;
        if (mo->imp[i].at <= 0 || u < 0 || u >= 1) continue;
        float a = 1 - u, sc = mo->imp[i].size / 40, px = cx + mo->imp[i].x * U, py = cy + mo->imp[i].y * U, o3 = 1 - (1 - u) * (1 - u) * (1 - u), d = atan2f(mo->imp[i].dy, mo->imp[i].dx);
        Col col = mo->imp[i].col;
        for (int j = 0; j < 8; j++) { /* impact lines thrown out */
            float g0 = j * 0.7853982f + d + 0.2f, r0 = (8 + 26 * o3) * k * sc, r1 = r0 + (10 + 14 * (1 - u)) * k * sc;
            glow_line(c, cl, px + cosf(g0) * r0, py + sinf(g0) * r0, px + cosf(g0) * r1, py + sinf(g0) * r1, col, 0.9f * a, col, 0.4f * a, 1.6f * k * (1 + sc * 0.5f));
        }
        if (mo->imp[i].ring > 0) ring_col(c, cl, px, py, (6 + 60 * mo->imp[i].ring * o3) * k, (1 + 3 * a) * k, col, 0.7f * a * mo->imp[i].ring); /* the shock ring */
        float fl = 1 - (float)(now - mo->imp[i].at) * 2.5f; /* the pointer flashes */
        if (fl > 0.01f) ring_col(c, cl, px, py, (8 + 22 * (1 - fl)) * k, 2 * k, WHITE, 0.6f * fl);
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
    lit_look.edge *= 1 + 1.5f * s->glow_boost; glow *= 1 + 0.8f * s->glow_boost; /* (winding up to be thrown) */
    float wb = s->w_bright > 0 ? s->w_bright : 1, ws = s->w_scale > 0 ? s->w_scale : 1; /* a whim (0 = never ticked) */
    lit_look.edge *= 0.4f + 0.6f * wb;
    float mb = s->mo.bright > 0 ? s->mo.bright : 1; /* a routine's own brightness (a forge blow, a beacon facing you) */
    lit_look.edge *= 0.4f + 0.6f * mb;
    lit_look.hr *= 1 + lit_look.lh * s->level * 0.15f; /* the glow grows with the microphone level */
    float flash = 0, thick = 1;
    if (s->unread && !s->overlay && session_shown(s, now) != ST_READY) { /* an unread reply: a slow, soft flash, as if waiting for leave to report (READY has its own ways) */
        flash = TOUCH.unread_amp * (0.5f - 0.5f * (float)cos(t * TOUCH.unread_hz * 6.283185));
        lit_look.edge *= 1 + flash * 0.5f;
    }
    float vf = (float)(1 - (now - s->vol_at) / TOUCH.vol_for); /* after a scroll the shell shows the volume */
    if (vf > 0) { thick = 1 + vf * (0.3f + 1.7f * s->vol - 1); lit_look.edge *= 1 + vf * (0.4f + 0.9f * s->vol - 1); }
    if (s->aura_a > 0.01f) { /* single-core mode: the core takes on this session's colour, quietly; the shell stays */
        lit_look.halo = mix(lit_look.halo, s->aura, s->aura_a);
        lit_look.col = mix(lit_look.col, s->aura, s->aura_a * 0.45f);
    }
    if (s->mo.rage > 0.01f) { /* READY winding up: toward NEEDS YOU's orange */
        const Look *W = &LOOKS[ST_WAIT];
        float r = fminf(1, s->mo.rage);
        lit_look.col = mix(lit_look.col, W->col, r); lit_look.halo = mix(lit_look.halo, W->halo, r); /* (all of it, core and glow too, as the Bestiary) */
        lit_look.line = mix(lit_look.line, W->line, r); lit_look.eh = mix(lit_look.eh, W->eh, r);
        lit_look.core += (W->core - lit_look.core) * r; lit_look.hr += (W->hr - lit_look.hr) * r;
    }
    g_part = s->mo.loose_on ? RENDER_PART : 0;
    if (RENDER_PART == 2 && !g_part) return; /* the layer: nothing of this one is out */
    const Look *L = &lit_look;
    int tw = cl.x1 - cl.x0, th = cl.y1 - cl.y0;
    int lay_y1 = cl.y1; /* (the label's place: cl may be cut to a band of rows below) */
    int fs = tw / 120 < 1 ? 1 : tw / 120, label_h = fs * 16;
    if (!label || th - label_h < 12) label_h = 0;
    float m = fminf((float)tw, (float)(th - label_h)), room = m * 0.5f - 1; /* room: from the middle to the picture's edge */
    if (s->pad > 1) m /= s->pad;
    float unit = m * SHELL_FRAC / CIRCUM * ws, k = m / 600.0f;
    /* READY as the Core Bestiary draws it: there the stage zooms out to 30% for READY while line widths, node dots and
     * sparks stay the stage's size (k = side / 520), so against the core they are 4.6 times what the other states use
     * (sparks 3.3 times): the core and the pieces it throws read as bold dots and lines on visible smoke */
    int ready_look = session_shown(s, now) == ST_READY;
    if (ready_look) k *= SHELL_FRAC * 600.0f / 47.0f;
    float ox = cl.x0 + tw * 0.5f + s->w_off_x * CIRCUM * unit + c->sx, oy = cl.y0 + (th - label_h) * 0.5f + s->w_off_y * CIRCUM * unit + c->sy;

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
    /* small cores (satellites, under ~150 px as shown): their many edges crowd into a few pixels and pile up into a
     * bright disc with a hard rim at the outline, so each edge and node is fainter the smaller the core */
    float shown_k = k / (c->ss > 1 ? c->ss : 1), crowd = shown_k < 0.25f ? 0.35f + 0.65f * shown_k / 0.25f : 1;
    int crazy = motion_crazy(s, now);
    float strobe = crazy ? (sinf((float)t * 90.0f) > 0 ? 0.45f : 1.0f) : 1.0f; /* going crazy: the threads stutter */
    Aura au;
    { /* to just inside the picture's edge, so nothing is cut off there either */
        float half = fminf((float)tw, (float)(th - label_h)) * 0.5f - 1, R = CIRCUM * unit * (s->mo.scale > 0 ? s->mo.scale : 1);
        aura_make(&au, ox, oy, R, fminf(R * 1.36f, half), L->eh, 0.07f * L->edge * strobe * crowd * glow);
    }
    if (g_part == 2) cl = (Clip){0, 0, c->w, c->h}; /* the layer: the core's place from the clip, its pieces anywhere */
    if (c->ry1 > c->ry0) { if (cl.y0 < c->ry0) cl.y0 = c->ry0; if (cl.y1 > c->ry1) cl.y1 = c->ry1; } /* a band of rows: every primitive keeps to cl */
    if (g_part != 2) draw_core(c, cl, gx, gy, rc, L, inten, &au);
    if (g_part == 2) burst = 0;
    if (burst > 0) ring(c, cl, ox, oy, rc + (1 - burst) * m * 0.6f, (1 + burst * 3) * (k > 0.5f ? k : 0.5f) * 2, burst * 0.6f);
    float wave = (float)(1 - (now - s->ring_at) / TOUCH.ring_for); /* double click: one shockwave ring, no flare */
    if (wave > 0 && wave <= 1 && g_part != 2) ring(c, cl, ox, oy, rc + (1 - wave) * m * 0.55f, (1 + wave * 2) * (k > 0.5f ? k : 0.5f) * 2, wave * 0.55f);
    for (int i = 0; i < MAX_RING; i++) { /* the pulses a routine sends out (a click, a blow, a beacon facing you): thin, */
        const MRing *r = &s->mo.ring[i];  /* from the shell's edge outward, fading as they go, so none stays round the core like a frame */
        if (r->a <= 0 || g_part == 2) continue;
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
    P.cam = mo->cam > 0 ? mo->cam : CAM; /* (READY: the Bestiary's gentler camera, Motion.cam) */
    g_stage = P; /* READY's loose pieces and sparks: see g_stage */
    memcpy(g_stage.M, I3_R, sizeof g_stage.M); memcpy(g_stage.T, I3_R, sizeof g_stage.T);
    g_stage.ox = ox - s->w_off_x * CIRCUM * unit; g_stage.oy = oy - s->w_off_y * CIRCUM * unit;
    g_stage.unit = unit / ws; g_stage.tall = 0; g_stage.st = 0; g_stage.ux = 1; g_stage.uy = 0;
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
    int any = 0;
    for (int t = 0; t < sh->nt && !any; t++) any = moved(mo, t);
    float edge = L->edge * strobe * crowd, thin = 0.9f * (sh->df > 0.6f ? sh->df : 0.6f) * thick;
    float Rpx = CIRCUM * P.unit, ks = Rpx / (ready_look ? 47.0f : 156.0f); /* the mock's pixel sizes, for a shell of this size (READY: see above) */
    float bd = s->backdrop > 0.01f ? 0.75f * s->backdrop * BACKDROP_A : 0, br = fmaxf(1.6f, 0.055f * Rpx) * BACKDROP_W; /* the backdrop's band along the lines (shade_seg): how dark, how far it reaches */
    if (bd > 0 && BACKDROP_CORE && g_part != 2) { float cc[3] = {ox, oy, CIRCUM}; shade_seg(c, cl, cc, cc, 0.55f * Rpx, 0.7f * bd); } /* and behind the bright centre, a small soft disc */
    if (g_part != 1) draw_trail(c, cl, &P, mo, k); /* (while pieces are out the layer draws it: it may reach far, as constellation's line does) */
    if (g_part != 1) draw_sparks(c, cl, &P, mo, L, ks); /* (while pieces are out the layer draws the sparks: they fly far) */
    RotCache rc_ = {0};
    /* READY's thrown pieces are drawn in the edge loop below with the rest, and get the backdrop's band the same way */
    for (int i = 0; i < sh->ne; i++) {
        int a = sh->e[i][0], b = sh->e[i][1], t1 = sh->et[i][0], t2 = sh->et[i][1];
        float gg = (mo->glow[a] + mo->glow[b]) * 0.5f, pa[3], pb[3];
        if (!any || t1 < 0) { if (g_part == 2) continue; if (bd > 0) shade_seg(c, cl, q[a], q[b], br, bd); draw_edge(c, cl, q[a], q[b], L, edge, thin, glow * sh->df, gg, k); continue; }
        /* an edge between two triangles that moved apart is drawn on each of them */
        float a1 = alpha_of(mo, t1);
        if (a1 > 0.01f) {
            at_tri(&P, mo, sh, base, a, t1, &rc_, pa); at_tri(&P, mo, sh, base, b, t1, &rc_, pb);
            if (bd > 0) shade_seg(c, cl, pa, pb, br, bd * a1);
            draw_edge(c, cl, pa, pb, L, edge * a1, thin, glow * sh->df, gg, k);
        }
        if (t2 >= 0 && !same_tri(mo, t1, t2) && alpha_of(mo, t2) > 0.01f) {
            at_tri(&P, mo, sh, base, a, t2, &rc_, pa); at_tri(&P, mo, sh, base, b, t2, &rc_, pb);
            if (bd > 0) shade_seg(c, cl, pa, pb, br, bd * alpha_of(mo, t2));
            draw_edge(c, cl, pa, pb, L, edge * alpha_of(mo, t2), thin, glow * sh->df, gg, k);
        }
    }
    for (int i = 0; i < sh->nv; i++) { /* the nodes: one dot for each place a node is drawn */
        int seen[MAX_DEG], ns = 0, nt = any ? sh->nvt[i] : 0;
        for (int j = 0; j <= nt; j++) {
            float p[3], al = 1;
            if (!nt) { if (j || g_part == 2) break; memcpy(p, q[i], sizeof p); }
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
            if (bd > 0) shade_seg(c, cl, p, p, br * 1.25f, bd * al); /* (a node: a soft dot of the band) */
            if (k > 0.3f) blob(c, cl, p[0], p[1], 5 * k * (1 + gg), L->eh, (0.3f * d * edge * glow * sh->df) * (1 + 2 * gg) * al, L->eh, 0.1f * d * edge * glow * sh->df * al, 0.5f);
            disc(c, cl, p[0], p[1], (k * 1.5f > 0.6f ? k * 1.5f : 0.6f) * (1 + gg), mix(L->line, WHITE, gg), clamp01(0.95f * d * edge * (k > 0.3f ? 1 : 0.5f) + gg) * al);
        }
    }
    if (g_part != 1) pointer_fx(c, cl, s, ox - s->w_off_x * CIRCUM * unit, oy - s->w_off_y * CIRCUM * unit, CIRCUM * unit / ws, now);
    if (g_part == 2) return; /* the layer: only the pieces and the sparks */
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
    if (mo->hole > 0.01f) draw_hole(c, cl, s, mo, ox, oy, Rpx, ks, t, now, room);
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
        int st = session_shown(s, now), y = lay_y1 - label_h + fs * 2;
        text(c, cl, ox, y, s->name[0] ? s->name : "CLAUDE", fs, WHITE, 0.8f);
        if (st == ST_WORK && s->tool[0]) snprintf(line, sizeof line, "%s %s", STATE_NAME[st], s->tool);
        else snprintf(line, sizeof line, "%s", STATE_NAME[st]);
        text(c, cl, ox, y + fs * 7, line, fs, L->eh, 0.85f);
    }
}

void render_room(Canvas *c, const Room *r, const Shape *sh, double now, float glow, float core_frac) {
    canvas_clear(c);
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

/* one session laid out in the box x0..x1, y0..y1 of the canvas (READY's layer: RENDER_PART 2 draws its pieces anywhere).
 * It clears only the canvas's drawn box, so draw nothing else into a canvas it is given. */
void render_part(Canvas *c, const Session *s, const Shape *sh, double now, float glow, float core_frac, int x0, int y0, int x1, int y1) {
    render_prep(c, 0); /* (the layer is mostly empty: clearing all of it each frame cost more than drawing) */
    draw_session(c, (Clip){x0, y0, x1, y1}, s, sh, now, glow, core_frac, 0);
}

void render_prep(Canvas *c, int whole) {
    glow_lut(); halo_lut(); icosa(); bh_lut(); /* (built here, before any bands share them) */
    if (whole) canvas_clear(c); else canvas_clear_box(c);
}
void render_band(Canvas *band, const Session *s, const Shape *sh, double now, float glow, float core_frac, int x0, int y0, int x1, int y1) {
    band->bx0 = band->by0 = band->bx1 = band->by1 = 0;
    if (band->ry1 > band->ry0) draw_session(band, (Clip){x0, y0, x1, y1}, s, sh, now, glow, core_frac, 0);
}
void canvas_box_merge(Canvas *c, const Canvas *band) { touch(c, band->bx0, band->by0, band->bx1, band->by1); }

/* one session filling the whole canvas, without the drawn label (the list layout writes real text instead) */
void render_one(Canvas *c, const Session *s, const Shape *sh, double now, float glow, float core_frac) {
    render_prep(c, 1);
    draw_session(c, (Clip){0, 0, c->w, c->h}, s, sh, now, glow, core_frac, 0);
}

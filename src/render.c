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

/* ---- sparks, light trails, the black hole ------------------------------------------------------ */
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
/* the black hole, drawn over the collapsed shell, as Gargantua: a black shadow ringed by a thin photon ring, a hot
 * accretion disc seen almost edge-on that crosses in front of it, and the disc's far side bent by the hole's gravity
 * into a glowing halo, wide over the top of the shadow and thin under it; the side turning toward you is brighter
 * (Doppler). Light, worked out per pixel and coloured by how bright it is (ember, orange, gold, white-hot); the dust
 * (rs.hole.dust, spiralling in) glints along the disc. */
static Col hole_col(float x) {
    const Col EMBER = {0.5f, 0.16f, 0.04f}, ORANGE = {1, 0.48f, 0.14f}, GOLD = {1, 0.78f, 0.45f}, HOT = {1, 0.97f, 0.9f};
    return x < 0.45f ? mix(EMBER, ORANGE, x / 0.45f) : x < 0.9f ? mix(ORANGE, GOLD, (x - 0.45f) / 0.45f) : mix(GOLD, HOT, clamp01((x - 0.9f) / 0.6f));
}
static float hole_env(float u) { return u <= 0 || u >= 1 ? 0 : u < 0.15f ? u / 0.15f : 1 - (u - 0.15f) / 0.85f; } /* quick in, slow out */
static void draw_hole(Canvas *c, Clip cl, const Session *s, const Motion *mo, float ox, float oy, float R, float ks, double t, double now) {
    /* rh0: the hole as it opens; once the shell is swallowed (grow) the whole picture grows with it until the disc's
     * reach is the tile's, its outer edge fading before the tile's edge */
    const HoleRS *H = &mo->rs.hole;
    float hb = mo->hole, g0 = clamp01(H->grow), gr = g0 * g0 * (3 - 2 * g0), rh0 = R * 0.3f * (1 + 0.25f * hb), rh = rh0 + (R * 0.85f / 1.3f - rh0) * gr;
    /* its own action now (motion.c picks it): how far through, and how strong */
    static const float DUR[HA_N] = {1, 2.6f, 4.5f, 2.2f, 2.8f, 6};
    int act = H->act;
    float au = act ? clamp01((float)(now - H->act_at) / DUR[act]) : 0, env = hole_env(au), sw = sinf(3.14159265f * au);
    float tf = (float)t, fl = H->flow, flick = 1 + 0.06f * sinf(tf * 1.3f) + 0.04f * sinf(tf * 3.7f + 1) + 0.03f * sinf(tf * 7.9f); /* accretion never steady */
    float boost = flick * (act == HA_FLARE ? 1 + 1.3f * env : 1) * (1 + 0.25f * s->hover + 0.4f * s->carry);
    /* the disc's lean: its own slow drift and the hand's drag (the shell's turn, ax / ay), and the precession */
    float tilt = fminf(0.3f, fmaxf(0.07f, 0.17f + 0.1f * sinf(s->ax * 0.7f))) + (act == HA_PRECESS ? 0.17f * sw * sw : 0), rot = -0.08f + 0.22f * sinf(s->ay * 0.5f) + (act == HA_PRECESS ? 0.14f * sw * H->act_p : 0);
    float lk = sqrtf(s->look_x * s->look_x + s->look_y * s->look_y) + 1e-3f, lx = s->look_x / lk, ly = s->look_y / lk, lit = s->hover * clamp01(lk * 1.5f); /* your side, pointed at */
    float rs = rh * 0.62f, cr = cosf(rot), sr = sinf(rot);
    float eout = fminf(4.4f, R * 1.25f / rs); /* the disc's outer edge, in shadow radii */
    float feed_flash = act == HA_FEED && au > 0.82f ? 1 - (au - 0.82f) / 0.18f : 0;
    float ripple_r = act == HA_RIPPLE ? 1.1f + 2.3f * au : -9; /* the gravity wave's front, in shadow radii */
    blob(c, cl, ox, oy, fminf(rs * 3.2f, R * 1.1f), (Col){1, 0.5f, 0.15f}, 0, (Col){0.5f, 0.16f, 0.04f}, 0.12f * hb * boost, 0.02f); /* a warm haze */
    blob(c, cl, ox - rs * 0.2f - s->move_x * rs * 0.25f, oy - rs * 0.9f - s->move_y * rs * 0.25f, rs * 1.6f, (Col){1, 0.85f, 0.6f}, 0, (Col){1, 0.6f, 0.25f}, 0.18f * hb * boost, 0.02f); /* the halo's bloom (it leans back from a carry) */
    float ex = rs * eout + 2, ey = rs * 2.1f + 2;
    int bx0 = (int)fmaxf(cl.x0, floorf(ox - ex)), bx1 = (int)fminf(cl.x1, ceilf(ox + ex) + 1);
    int by0 = (int)fmaxf(cl.y0, floorf(oy - ey)), by1 = (int)fminf(cl.y1, ceilf(oy + ey) + 1);
    touch(c, bx0, by0, bx1, by1);
    /* three hot spots riding the inner disc, each at its own orbital speed */
    float spot_e[3] = {1.6f, 2.05f, 2.6f}, spot_a[3];
    for (int k = 0; k < 3; k++) spot_a[k] = fl * 0.9f / powf(spot_e[k], 1.5f) + k * 2.1f;
    for (int y = by0; y < by1; y++) for (int x = bx0; x < bx1; x++) {
        float dx = x + 0.5f - ox, dy = y + 0.5f - oy, u = dx * cr + dy * sr, v = -dx * sr + dy * cr; /* u along the disc, v across it (down: near side) */
        float r = sqrtf(dx * dx + dy * dy), rc = r / rs, side = u / (r + 1e-3f);
        float shadow = rc < 0.95f ? 1 : rc < 1.03f ? (1.03f - rc) / 0.08f : 0;
        /* the disc: an ellipse seen almost edge-on, hottest at its inner edge; its lanes flow round, inner ones faster */
        float e = sqrtf(u * u + (v / tilt) * (v / tilt)) / rs, disc = 0;
        if (e > 1.35f && e < eout) {
            float in = clamp01((e - 1.35f) / 0.3f), out = clamp01((eout - e) / (eout * 0.25f));
            float ang = atan2f(v / tilt, u), om = fl / powf(e, 1.5f); /* how far this ring has turned (Kepler) */
            float wave = ripple_r > 0 ? 0.25f * (1 - au) * expf(-(e - ripple_r) * (e - ripple_r) * 6) * sinf((e - ripple_r) * 18) : 0;
            float streak = 0.84f + 0.1f * sinf(e * 23 + 3 * sinf(e * 5.1f + ang * 3 - om * 2.2f) + wave * 9) + 0.06f * sinf(e * 61 + ang * 7 - om * 3.5f);
            float spot = 0;
            for (int k = 0; k < 3; k++) {
                float da = ang - spot_a[k];
                da = da - 6.2831853f * floorf((da + 3.14159265f) / 6.2831853f);
                spot += expf(-(e - spot_e[k]) * (e - spot_e[k]) / 0.02f - da * da / 0.12f);
            }
            disc = 1.7f * in * out * powf(1.6f / fmaxf(e, 1.6f), 1.5f) * (streak + 0.9f * spot + wave) * (1.3f - 0.65f * u / (e * rs));
        }
        /* the far side lensed into a halo: wide over the top, thin under; light ripples slowly along it */
        float lens = 0;
        if (rc > 1.0f && rc < 2.0f) {
            float top = clamp01(-v / (r + 1e-3f) * 1.25f + 0.15f), reach = 1.16f + 0.8f * top * top;
            float w = rc < 1.07f ? (rc - 1) / 0.07f : clamp01((reach - rc) / (reach - 1.07f));
            lens = 1.5f * powf(w, 1.6f) * (0.25f + 0.75f * top) * (1.15f - 0.45f * side) * (0.88f + 0.08f * sinf(rc * 33 - fl * 0.35f) + 0.06f * sinf(atan2f(dy, dx) * 5 - fl * 0.7f)) * (1 + 0.6f * lit * fmaxf(0, (dx * lx + dy * ly) / (r + 1e-3f)));
        }
        float ring = (1.3f + 2.2f * feed_flash) * expf(-((rc - 1.035f) * (rc - 1.035f)) / (0.016f * 0.016f)) * (1.1f - 0.35f * side) * (1 + 1.2f * lit * fmaxf(0, (dx * lx + dy * ly) / (r + 1e-3f))); /* the photon ring, brighter on your side */
        float I = ((v > 0 ? disc : disc * (1 - shadow)) + lens) * boost + ring;
        float *p = c->p + 3 * ((size_t)y * (size_t)c->w + (size_t)x);
        if (shadow > 0) { float a = shadow * hb; p[0] *= 1 - a; p[1] *= 1 - a; p[2] *= 1 - a; } /* light taken away: see-through on the desktop */
        if (I <= 0.003f) continue;
        Col col = hole_col(I);
        float a = fminf(I, 1.5f) * 0.9f * hb;
        p[0] += col.r * a; p[1] += col.g * a; p[2] += col.b * a;
    }
    if (ripple_r > 0) { /* the gravity wave: a faint ring going out, gone before the tile's edge */
        float rr = ripple_r * rs, al = 0.35f * (1 - au) * clamp01((R * 1.2f - rr) / (R * 0.35f)) * hb;
        if (al > 0.003f) { float px = ox + rr, py = oy; for (int i = 1; i <= 64; i++) { float a = i * 6.2831853f / 64, X = ox + cosf(a) * rr, Y = oy + sinf(a) * rr * 0.92f; glow_line(c, cl, px, py, X, Y, hole_col(1.0f), al, hole_col(0.6f), al * 0.8f, ks * 0.8f); px = X; py = Y; } }
    }
    if (act == HA_JETS) { /* two beams out of the poles, across the disc: bright at the root, thinning out */
        float L = fminf(rs * 2.4f, R * 1.15f - rs) * clamp01(au / 0.3f), fade = au < 0.6f ? 1 : 1 - (au - 0.6f) / 0.4f, wob = 0.03f * sinf(tf * 9);
        const Col BLUE = {0.7f, 0.82f, 1}, PALEB = {0.92f, 0.96f, 1};
        for (int k = -1; k <= 1; k += 2) {
            float ax = -sinf(rot + wob) * k, ay = -cosf(rot + wob) * k; /* up (k = 1) and down */
            for (int sgm = 0; sgm < 3; sgm++) {
                float f0 = sgm / 3.0f, f1 = (sgm + 1) / 3.0f, a = fade * hb * (0.75f - 0.22f * sgm);
                glow_line(c, cl, ox + ax * (rs * 1.05f + L * f0), oy + ay * (rs * 1.05f + L * f0), ox + ax * (rs * 1.05f + L * f1), oy + ay * (rs * 1.05f + L * f1), PALEB, a, BLUE, a * 0.6f, ks * (1.1f - 0.25f * sgm));
            }
            blob(c, cl, ox + ax * rs * 1.1f, oy + ay * rs * 1.1f, rs * 0.3f, PALEB, 0.45f * fade * hb, BLUE, 0.2f * fade * hb, 0.2f);
        }
    }
    if (act == HA_FEED && au < 0.86f) { /* a star drawn in: spiralling down, pulled into a streak, gone into the shadow */
        float q = au / 0.86f, er = 3.6f - 2.55f * q * q, a1 = H->act_p * 3.14159265f + q * 9, a0 = a1 - (0.08f + 0.7f * q * q);
        float lift = -0.7f * rs * (1 - q) * (1 - q); /* it comes in from above the disc and settles onto it */
        float rr = er * rs, px0 = cosf(a0) * rr, py0 = sinf(a0) * rr * tilt + lift * 1.15f, px1 = cosf(a1) * rr, py1 = sinf(a1) * rr * tilt + lift;
        float X0 = ox + px0 * cr - py0 * sr, Y0 = oy + px0 * sr + py0 * cr, X1 = ox + px1 * cr - py1 * sr, Y1 = oy + px1 * sr + py1 * cr;
        int hidden = sinf(a1) < 0 && hypotf(X1 - ox, Y1 - oy) < rs * 1.05f;
        if (!hidden) {
            const Col STAR = {0.62f, 0.8f, 1};
            glow_line(c, cl, X0, Y0, X1, Y1, STAR, 0.9f * hb, STAR, 0.6f * hb, ks * (2.4f - 0.9f * q)); /* its tidal tail */
            blob(c, cl, X1, Y1, rs * (0.3f - 0.14f * q), WHITE, 0.95f * hb, STAR, 0.7f * hb, 0.22f);
        }
    }
    for (int i = 0; i < NDUST; i++) { /* the dust: glints riding the disc (behind the shadow on the far side) */
        const Dust *d = &H->dust[i];
        float sn = sinf(d->a), rr = rs * (1.5f + (d->r - 1.05f) * 1.25f);
        if (rr > rs * eout) continue;
        float px = cosf(d->a) * rr, py = sn * rr * tilt, X = ox + px * cr - py * sr, Y = oy + px * sr + py * cr;
        if (sn < 0 && hypotf(X - ox, Y - oy) < rs * 1.05f) continue;
        float al = hb * 0.35f * clamp01((3.2f - d->r) / 1.2f) * (1 - 0.5f * cosf(d->a)) * boost;
        disc(c, cl, X, Y, d->s * ks * 0.7f, hole_col(1.1f - 0.25f * d->r), al);
    }
    for (int i = 0; i < NSTAR; i++) { /* a few faint stars, holding still */
        const float *st = H->star[i];
        float x = st[0] * R, y = st[1] * R;
        if (sqrtf(x * x + y * y) < rh * 2) continue;
        disc(c, cl, ox + x, oy + y, st[2] * 0.8f * ks, (Col){1, 0.93f, 0.85f}, hb * (0.4f + 0.1f * sinf(tf * 0.4f + st[3])) * 0.55f);
    }
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
    float m = fminf((float)tw, (float)(th - label_h));
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
    if (mo->hole > 0.01f) draw_hole(c, cl, s, mo, ox, oy, Rpx, ks, t, now);
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
    glow_lut(); halo_lut(); icosa(); /* (built here, before any bands share them) */
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

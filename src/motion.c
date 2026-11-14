/* How the shell moves: the "Core Bestiary" design, worked out in an interactive mock first.
 *
 * At rest the shell is an exact geodesic sphere, a calm oracle. Every motion is geometric and never
 * overshoots: snap turns about symmetry axes that speed up and stop dead or ease in and out; irregular
 * patches of triangles that slide out along their normal and lock back (a new random cut each time, so
 * no pattern repeats); layers of triangles turning against the rest; pieces carried off whole; spikes,
 * where one node is pulled far out into a sharp pyramid or in toward the centre (out and in together make
 * the tension); a scan plane and nodes lighting in sequence; sparks, light trails and rings.
 *
 * Each state runs a routine (one of the rows of ROUTINES in session.c) picked from its row of MOTION_MAP
 * each time the state starts, and now and then an action (a whim) the map allows it. The routines are at
 * the bottom of this file, one function each, dispatched in run_routine.
 *
 * Patches and layers need triangles, so they only happen on the geodesic shells (geo2, geo3); on dodeca
 * and bucky the net stays rigid and only turns, spikes, light and sparks remain.
 *
 * Sizes differ from the mock: the shell fills 72% of its tile here, so lifts, flights and sparks are
 * smaller, and spikes are capped at 0.4 of a radius. */
#include "vigil.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TAU 6.28318531f
#define PI_F 3.14159265f
#define STEP5 (TAU / 5) /* a fifth of a turn about a 5-fold axis maps the shell onto itself */

/* ---- small helpers ---------------------------------------------------------------------------- */
static float mrand(Session *s) {
    uint32_t x = s->rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    s->rng = x ? x : 1;
    return (float)(s->rng >> 8) / 16777216.0f;
}
static float rnd(Session *s, float a, float b) { return a + (b - a) * mrand(s); }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float io3(float u) { if (u <= 0) return 0; if (u >= 1) return 1; return u < .5f ? 4 * u * u * u : 1 - powf(-2 * u + 2, 3) / 2; }
static float outx(float u) { return u <= 0 ? 0 : u >= 1 ? 1 : 1 - powf(2, -10 * u); }
static float out3(float u) { return u <= 0 ? 0 : u >= 1 ? 1 : 1 - (1 - u) * (1 - u) * (1 - u); }
static float in3(float u) { return u <= 0 ? 0 : u >= 1 ? 1 : u * u * u; }
static float bump(float u) { return u <= 0 || u >= 1 ? 0 : sinf(u * PI_F); }
static float smooth01(float u) { u = clampf(u, 0, 1); return u * u * (3 - 2 * u); }
static float env(float e, float up, float hd, float dn) { /* out fast, hold, back in */
    if (e < 0) return 0;
    if (e < up) return outx(e / up);
    if (e < up + hd) return 1;
    return fmaxf(0, 1 - in3((e - up - hd) / dn));
}
static float hold(float u, float in) { /* up, stay, down (as in session.c) */
    if (u <= 0 || u >= 1) return 0;
    float e = u < in ? u / in : u > 1 - in ? (1 - u) / in : 1;
    return e * e * (3 - 2 * e);
}
/* gathers speed, runs evenly, slows down: the share of the way covered at u, a ramp of a each way */
static float ramp(float u, float a) {
    if (u <= 0) return 0;
    if (u >= 1) return 1;
    float vm = 1 / (1 - a);
    return u < a ? vm * u * u / (2 * a) : u > 1 - a ? 1 - vm * (1 - u) * (1 - u) / (2 * a) : vm * (u - a / 2);
}
static float hashf(int n) { float x = sinf((float)n * 127.1f) * 43758.5453f; return x - floorf(x); }
static float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static float len3(const float *a) { return sqrtf(dot3(a, a)); }
static void norm3(float *v) { float l = sqrtf(dot3(v, v)); if (l > 1e-9f) { v[0] /= l; v[1] /= l; v[2] /= l; } }
static void cross3(float *r, const float *a, const float *b) {
    float x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    r[0] = x; r[1] = y; r[2] = z;
}
static void set3(float *v, float x, float y, float z) { v[0] = x; v[1] = y; v[2] = z; }
static void rand_dir(Session *s, float *v) { set3(v, rnd(s, -1, 1), rnd(s, -1, 1), rnd(s, -1, 1)); norm3(v); }

void m3_mul(float *r, const float *a, const float *b) {
    float t[9];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) t[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
    memcpy(r, t, sizeof t);
}
void m3_rot(float *r, const float *a, float g) {
    float c = cosf(g), s = sinf(g), t = 1 - c, x = a[0], y = a[1], z = a[2];
    float m[9] = {t * x * x + c, t * x * y - s * z, t * x * z + s * y, t * x * y + s * z, t * y * y + c, t * y * z - s * x, t * x * z - s * y, t * y * z + s * x, t * z * z + c};
    memcpy(r, m, sizeof m);
}
void m3_app(float *out, const float *m, const float *v) {
    float x = m[0] * v[0] + m[1] * v[1] + m[2] * v[2], y = m[3] * v[0] + m[4] * v[1] + m[5] * v[2], z = m[6] * v[0] + m[7] * v[1] + m[8] * v[2];
    out[0] = x; out[1] = y; out[2] = z;
}
void m3_euler(float *r, float ax, float ay) { /* as the renderer always did: about y, then about x */
    float cy = cosf(ay), sy = sinf(ay), cx = cosf(ax), sx = sinf(ax);
    float ry[9] = {cy, 0, sy, 0, 1, 0, -sy, 0, cy}, rx[9] = {1, 0, 0, 0, cx, -sx, 0, sx, cx};
    m3_mul(r, rx, ry);
}
static void m3_ortho(float *m) { /* keep a matrix that is updated every frame a clean rotation */
    float x[3] = {m[0], m[3], m[6]}, y[3] = {m[1], m[4], m[7]}, z[3];
    norm3(x);
    float d = dot3(x, y);
    for (int i = 0; i < 3; i++) y[i] -= d * x[i];
    norm3(y);
    cross3(z, x, y);
    for (int i = 0; i < 3; i++) { m[i * 3] = x[i]; m[i * 3 + 1] = y[i]; m[i * 3 + 2] = z[i]; }
}
static void m3_tr(float *r, const float *m) { float t[9] = {m[0], m[3], m[6], m[1], m[4], m[7], m[2], m[5], m[8]}; memcpy(r, t, sizeof t); }
static void m3_tapp(float *out, const float *m, const float *v) { float t[9]; m3_tr(t, m); m3_app(out, t, v); } /* the inverse of a rotation */
static void m3_scale(float *m, float k) { for (int i = 0; i < 9; i++) m[i] *= k; }
static const float I3[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
/* a unit vector square to a (for building a frame round an axis) */
static void square_to(float *out, const float *a) { float X[3] = {1, 0, 0}, Y[3] = {0, 1, 0}; cross3(out, a, fabsf(a[1]) < .9f ? Y : X); norm3(out); }

void motion_init(Session *s) {
    Motion *m = &s->mo;
    memset(m, 0, sizeof *m);
    m->q[0] = m->q[4] = m->q[8] = 1;
    m->scale = m->rate = m->bright = 1;
    m->routine = m->rt = m->want_rt = -1;
    m->crazy_until = m->tense_until = m->last_crazy = m->press_at = -1e9;
}

/* ---- the view: where things are as the viewer sees them (before the pointer tilt) -------------- */
static void view_matrix(const Session *s, float *M) { float E[9]; m3_euler(E, s->ax, s->ay); m3_mul(M, E, s->mo.q); }
static void view_dir(const Session *s, const float *M, const float *v, float *out) { (void)s; m3_app(out, M, v); norm3(out); }
/* a direction in view space, back into the space the snap turns act in (just before the drift) */
static void to_turn_space(const Session *s, const float *view, float *out) {
    float E[9]; m3_euler(E, s->ax, s->ay);
    out[0] = E[0] * view[0] + E[3] * view[1] + E[6] * view[2];
    out[1] = E[1] * view[0] + E[4] * view[1] + E[7] * view[2];
    out[2] = E[2] * view[0] + E[5] * view[1] + E[8] * view[2];
}
/* the model direction facing the viewer */
static void toward(const float *M, float *out) { set3(out, M[6], M[7], M[8]); norm3(out); }
static void unit_v(const Shape *sh, int v, float *out) { set3(out, sh->v[v][0] / CIRCUM, sh->v[v][1] / CIRCUM, sh->v[v][2] / CIRCUM); }

/* ---- snap turns ------------------------------------------------------------------------------- */
static void turn(Session *s, const float *axis, float angle, float dur, int kind, double now) {
    Motion *m = &s->mo;
    int slot = 0;
    for (int i = 0; i < MAX_TURN; i++) if (m->turn[i].dur <= 0) { slot = i; break; }
    Turn *t = &m->turn[slot];
    memcpy(t->axis, axis, sizeof t->axis);
    norm3(t->axis);
    t->angle = angle; t->dur = dur; t->at = now; t->kind = kind; t->done = 0;
}
static void turn_view(Session *s, const float *view_axis, float angle, float dur, double now) { float a[3]; to_turn_space(s, view_axis, a); turn(s, a, angle, dur, 0, now); }
static void turn_model(Session *s, const float *model_axis, float angle, float dur, double now) { float a[3]; m3_app(a, s->mo.q, model_axis); turn(s, a, angle, dur, 0, now); }
static float turn_ease(int kind, float u) {
    if (kind == 1) return u * u;                                               /* speeds up, stops dead */
    if (kind == 2) return u < .08f ? outx(u / .08f) : 1 - out3((u - .08f) / .92f); /* a jerk out and back */
    return io3(u);
}
static int turns_busy(const Motion *m) { for (int i = 0; i < MAX_TURN; i++) if (m->turn[i].dur > 0) return 1; return 0; }
static void run_turns(Session *s, double now) {
    Motion *m = &s->mo;
    for (int i = 0; i < MAX_TURN; i++) {
        Turn *t = &m->turn[i];
        if (t->dur <= 0) continue;
        float u = clampf((float)(now - t->at) / t->dur, 0, 1), want = t->angle * turn_ease(t->kind, u), R[9];
        m3_rot(R, t->axis, want - t->done);
        m3_mul(m->q, R, m->q);
        t->done = want;
        if (u >= 1) t->dur = 0;
    }
    m3_ortho(m->q);
}
static void jerk(Session *s, const float *view_axis, float angle, double now) { float a[3]; to_turn_space(s, view_axis, a); turn(s, a, angle, .7f, 2, now); }
/* turn vertex v so it points straight at the viewer */
static void align_to_viewer(Session *s, const Shape *sh, int v, float dur, double now) {
    float a[3], t[3], ax[3], Z[3] = {0, 0, 1};
    m3_app(a, s->mo.q, sh->v[v]); norm3(a);
    to_turn_space(s, Z, t);
    cross3(ax, a, t);
    float sn = sqrtf(dot3(ax, ax));
    if (sn < 1e-4f) return;
    turn(s, ax, atan2f(sn, dot3(a, t)), dur, 0, now);
}
static int corner_facing(const Session *s, const Shape *sh, const float *M) { /* the 5-fold vertex most toward the viewer */
    int best = 0; float bz = -9;
    for (int i = 0; i < sh->nv; i++) {
        if (sh->nadj[i] != 5) continue;
        float d[3]; view_dir(s, M, sh->v[i], d);
        if (d[2] > bz) { bz = d[2]; best = i; }
    }
    return best;
}
static int random_corner(Session *s, const Shape *sh) {
    for (int k = 0; k < 40; k++) { int v = (int)(mrand(s) * sh->nv) % sh->nv; if (sh->nadj[v] == 5) return v; }
    return 0;
}
static void random_axis5(Session *s, const Shape *sh, float *out) { unit_v(sh, random_corner(s, sh), out); } /* a 5-fold axis, model space */

/* ---- the cut: irregular patches by a random flood fill; about a quarter stay put, each has its own reach */
static void recut(Session *s, const Shape *sh, int lo, int hi) {
    Motion *m = &s->mo;
    int nt = sh->nt, order[MAX_T];
    if (!nt) { m->cut_for = 0; m->ng = 0; return; }
    for (int i = 0; i < nt; i++) order[i] = i;
    for (int i = nt - 1; i > 0; i--) { int j = (int)(mrand(s) * (i + 1)) % (i + 1), t = order[i]; order[i] = order[j]; order[j] = t; }
    memset(m->grp, 255, sizeof m->grp);
    int ng = 0;
    for (int k = 0; k < nt; k++) {
        int s0 = order[k];
        if (m->grp[s0] != 255) continue;
        int size = lo + (int)(mrand(s) * (hi - lo + 1)), g = ng++, mem[16], nm = 1;
        if (size > 16) size = 16;
        m->grp[s0] = (unsigned char)g; mem[0] = s0;
        while (nm < size) {
            int cand[48], nc = 0;
            for (int i = 0; i < nm; i++) for (int j = 0; j < 3; j++) { int n = sh->tnb[mem[i]][j]; if (n >= 0 && m->grp[n] == 255 && nc < 48) cand[nc++] = n; }
            if (!nc) break;
            int p = cand[(int)(mrand(s) * nc) % nc];
            m->grp[p] = (unsigned char)g; mem[nm++] = p;
        }
    }
    m->ng = ng;
    for (int g = 0; g < ng; g++) { m->gn[g][0] = m->gn[g][1] = m->gn[g][2] = 0; m->gw[g] = mrand(s) < .25f ? 0 : rnd(s, .45f, 1.35f); }
    for (int t = 0; t < nt; t++) for (int q = 0; q < 3; q++) m->gn[m->grp[t]][q] += sh->tn[t][q];
    for (int g = 0; g < ng; g++) norm3(m->gn[g]);
    m->cut_for = nt;
    m->cutid++;
}
/* a patch's reach for a lift: the same call never looks even, and fixed patches still count */
static float wof(const Motion *m, int g) { return fmaxf(.45f, m->gw[g] > 0 ? m->gw[g] : 1); }
static void glow_patch(Motion *m, const Shape *sh, int g, float a) {
    for (int t = 0; t < sh->nt; t++) if (m->grp[t] == g) for (int k = 0; k < 3; k++) m->glow[sh->t[t][k]] = fmaxf(m->glow[sh->t[t][k]], a);
}
/* patch steps from patch g0 to every patch (for a press that travels outward) */
static void patch_distances(Session *s, const Shape *sh, int g0) {
    Motion *m = &s->mo;
    int queue[MAX_T], qh = 0, qt = 0;
    memset(m->pdist, 255, sizeof m->pdist);
    m->pdist[g0] = 0; queue[qt++] = g0;
    while (qh < qt) {
        int g = queue[qh++];
        for (int t = 0; t < sh->nt; t++) {
            if (m->grp[t] != g) continue;
            for (int j = 0; j < 3; j++) {
                int n = sh->tnb[t][j];
                if (n < 0) continue;
                int h = m->grp[n];
                if (m->pdist[h] == 255) { m->pdist[h] = (unsigned char)(m->pdist[g] + 1); queue[qt++] = h; }
            }
        }
    }
}

/* ---- spikes ------------------------------------------------------------------------------------ */
static int spike_live(const Spike *p, double now) { return p->len != 0 && now - p->at < p->up + p->hd + p->dn; }
static void spike_at(Session *s, int v, float len, float hd, float up, float dn, double now) {
    Motion *m = &s->mo;
    int slot = 0; double oldest = 1e30;
    for (int i = 0; i < MAX_SPIKE; i++) {
        if (!spike_live(&m->sp[i], now)) { slot = i; break; }
        if (m->sp[i].at < oldest) { oldest = m->sp[i].at; slot = i; }
    }
    if (len > .4f) len = .4f; /* the shell fills 72% of its tile: further out would be cut off */
    m->sp[slot] = (Spike){v, len, up, hd, dn, now};
    m->glow[v] = fmaxf(m->glow[v], up > .1f ? .4f : 1);
}
/* a node away from the spikes already out, leaning toward `prefer` (a view direction) if given */
static int spike_random(Session *s, const Shape *sh, const float *M, float len, float hd, const float *prefer, float up, float dn, double now) {
    Motion *m = &s->mo;
    int best = 0; float score = -1e9f;
    for (int k = 0; k < 14; k++) {
        int v = (int)(mrand(s) * sh->nv) % sh->nv;
        float d = 2;
        for (int i = 0; i < MAX_SPIKE; i++) {
            if (!spike_live(&m->sp[i], now)) continue;
            const float *a = sh->v[v], *b = sh->v[m->sp[i].v];
            float dd = sqrtf((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2])) / CIRCUM;
            if (dd < d) d = dd;
        }
        float sc = fminf(d, 1.2f) * 3 + mrand(s);
        if (prefer) { float vd[3]; view_dir(s, M, sh->v[v], vd); sc += 3 * dot3(vd, prefer); }
        if (sc > score) { score = sc; best = v; }
    }
    spike_at(s, best, len, hd, up, dn, now);
    return best;
}
/* out and in together: a spike out, and a node two steps away pulled toward the centre */
static void spike_pair(Session *s, const Shape *sh, const float *M, float out, float in, float hd, const float *prefer, float up, float dn, double now) {
    int v = spike_random(s, sh, M, out, hd, prefer, up, dn, now), cand[36], nc = 0;
    for (int i = 0; i < sh->nadj[v]; i++) {
        int a = sh->vn[v][i];
        for (int j = 0; j < sh->nadj[a]; j++) {
            int w = sh->vn[a][j], skip = w == v;
            for (int k = 0; k < sh->nadj[v] && !skip; k++) if (sh->vn[v][k] == w) skip = 1;
            if (!skip && nc < 36) cand[nc++] = w;
        }
    }
    if (nc) spike_at(s, cand[(int)(mrand(s) * nc) % nc], -in, hd, up, dn, now);
}

/* ---- later: things a routine asks for a moment from now ------------------------------------------ */
enum { EV_PAIR = 1, EV_JERK, EV_FLASH, EV_STEP };
static void later(Session *s, double at, int kind) {
    Motion *m = &s->mo;
    for (int i = 0; i < MAX_EV; i++) if (!m->ev[i].kind) { m->ev[i] = (MEvent){at, kind}; return; }
}
static void flash(Motion *m, int nv, float a) { for (int i = 0; i < nv; i++) m->glow[i] = fmaxf(m->glow[i], a); }

/* ---- rings, sparks, light trails -------------------------------------------------------------------- */
static void ring(Motion *m, double now, float a) {
    int slot = 0; double oldest = 1e30;
    for (int i = 0; i < MAX_RING; i++) { if (m->ring[i].a <= 0) { slot = i; break; } if (m->ring[i].at < oldest) { oldest = m->ring[i].at; slot = i; } }
    m->ring[slot] = (MRing){now, a};
}
#define OWN ((Col){-1, 0, 0})
#define RGB255(r, g, b) ((Col){(r) / 255.0f, (g) / 255.0f, (b) / 255.0f})
/* A particle from model point p with model velocity v (both in shell radii); it lives `life` s, slows by `drag`,
 * falls by g (negative floats up). kind: a hot streak, a flickering ember, a flare that swells and fades, a soft
 * mote; hot: it cools from white through col to a dull red. col.r < 0: the core's own glow colour. */
static void spark(Session *s, const float *M, const float *p, const float *v, float life, float drag, float g, float size, int kind, int hot, Col col) {
    Motion *m = &s->mo;
    int slot = -1;
    for (int i = 0; i < MAX_SPARK; i++) if (m->spark[i].life <= 0) { slot = i; break; }
    if (slot < 0) return;
    Spark *q = &m->spark[slot];
    m3_app(q->p, M, p); m3_app(q->v, M, v);
    for (int i = 0; i < 3; i++) { q->p[i] *= CIRCUM; q->v[i] *= CIRCUM; }
    q->life = q->max = life; q->drag = drag; q->g = g * CIRCUM; q->size = size; q->kind = (unsigned char)kind; q->hot = (unsigned char)hot;
    q->own = col.r < 0; q->col = col; q->seed = mrand(s) * 99;
}
static void spark_at(Session *s, const float *M, const float *p, float life, float size, Col col) { float z[3] = {0, 0, 0}; spark(s, M, p, z, life, 0, 0, size, SK_FLARE, 0, col); }
static void trail_push(Motion *m, const float *p) {
    if (m->ntrail == MAX_TRAIL) { memmove(m->trail, m->trail + 1, sizeof(float) * 3 * (MAX_TRAIL - 1)); m->ntrail--; }
    memcpy(m->trail[m->ntrail++], p, sizeof(float) * 3);
}
static void trail_pop(Motion *m) { if (m->ntrail) { memmove(m->trail, m->trail + 1, sizeof(float) * 3 * (MAX_TRAIL - 1)); m->ntrail--; } }

/* ---- per-triangle matrices, carried offsets, see-through ---------------------------------------- */
static int tm_add(Motion *m, const float *R) { if (m->ntm >= MAX_T) return m->ntm; memcpy(m->tmx[m->ntm++], R, sizeof(float) * 9); return m->ntm; }
static void tt_set(Motion *m, int t, const float *v, float k) { m->tt_on = 1; for (int i = 0; i < 3; i++) m->tt[t][i] = v[i] * k * CIRCUM; }

/* Going crazy: first tension (a few nodes drawn in, the shell tightens), then a burst of out-and-in spike
 * pairs at nodes far apart, jerks about random axes, flicking patches and a glitching core. */
static void crazy(Session *s, const Shape *sh, const float *M, float dur, double now) {
    Motion *m = &s->mo;
    const float T0 = .4f;
    m->tense_until = now + T0; m->crazy_until = now + T0 + dur; m->last_crazy = now;
    recut(s, sh, 1, 6);
    int k0 = 4 + (int)(mrand(s) * 3);
    for (int i = 0; i < k0; i++) spike_random(s, sh, M, -rnd(s, .35f, .55f), T0 - .3f + rnd(s, 0, .1f), NULL, .3f, .14f, now);
    later(s, now + T0, EV_FLASH);
    int n = 7 + (int)(mrand(s) * 5);
    for (int i = 0; i < n; i++) later(s, now + T0 + mrand(s) * dur * .8f, EV_PAIR);
    for (int i = 0; i < 3; i++) later(s, now + T0 + mrand(s) * dur * .7f, EV_JERK);
}

/* ---- the routines ----------------------------------------------------------------------------------
 * A routine is a function of one RCtx, listed with its name and state in ROUTINES at the bottom. It reads the
 * session and the frame (c->s, c->m, c->sh, c->M the view, c->e the seconds since it started), keeps its own
 * working state in its member of c->m->rs (zero when it starts), and each frame asks for patch lifts, spin, size
 * and brightness in c->w and sets the per-frame effects in Motion (tmi/tmx, tt, talpha, squash, trail,
 * sparks, rings, glow) for the renderer. */
typedef struct { float grp[MAX_T]; float push[MAX_V]; float scale, rate, bright; } Want;
typedef struct RCtx {
    Session *s;
    Motion *m;
    const Shape *sh;
    const float *M;   /* the view: model to view (before the pointer tilt) */
    Want *w;
    int shown;        /* the state shown now */
    double now, tt;   /* tt: the session's own time, for steady loops */
    float dt, e;      /* e: seconds since the routine started */
} Ctx;
/* a routine's spin given in rad/s (as the mock gives it), as the share of the shown state's own spin */
static float absrate(int shown, float r) { float base = LOOKS[shown].rot; return base > .02f ? r / base : 0; }
static void set_rate(Ctx *c, float r) { c->w->rate = absrate(c->shown, r); }

/* IDLE: an exact sphere; now and then a line of light climbs it; pointed at, calm out-and-in pairs on its side */
static void r_oracle(Ctx *c) {
    Session *s = c->s; Motion *m = c->m; OracleRS *o = &m->rs.oracle;
    float u = (float)fmod(c->tt, 7.0) / 1.4f;
    if (u < 1) { m->scan_h = 1.15f - 2.3f * io3(u); m->scan_a = .5f; }
    if (s->hover > .3f) {
        float cl = hypotf(s->look_x_to, s->look_y_to);
        if (cl > .15f || (o->stir[0] == 0 && o->stir[1] == 0 && o->stir[2] == 0)) {
            float a = cl > .15f ? atan2f(s->look_y_to, s->look_x_to) : mrand(s) * TAU;
            set3(o->stir, cosf(a), sinf(a), .3f); norm3(o->stir);
        }
        if (c->now > o->next) { o->next = c->now + rnd(s, .22f, .45f); spike_pair(s, c->sh, c->M, rnd(s, .22f, .34f), rnd(s, .2f, .3f), rnd(s, .3f, .6f), o->stir, rnd(s, .3f, .45f), rnd(s, .45f, .7f), c->now); }
    }
}
static void r_drift(Ctx *c) { set_rate(c, .1f); }

/* THINK's two layers as matrices: layer 1 lifted (by scaling the whole layer out, so its pieces keep their
 * gaps, and each piece set a little apart) and turned, layer 0 turned where it is */
static void layer_mats(Ctx *c, float a0, float a1, float lift) {
    Motion *m = c->m; ThinkRS *T = &m->rs.think;
    float R0[9], Rr[9], R1[9];
    m3_rot(R0, T->ax, a0); m3_rot(Rr, T->ax, a1);
    memcpy(R1, Rr, sizeof R1); m3_scale(R1, 1 + .1f * lift);
    int i0 = a0 != 0 ? tm_add(m, R0) : 0, i1 = tm_add(m, R1);
    for (int t = 0; t < c->sh->nt; t++) {
        int g = m->grp[t];
        if (T->layer[g] == 1) { float n[3]; m3_app(n, Rr, m->gn[g]); m->tmi[t] = (unsigned char)i1; tt_set(m, t, n, T->gap[g] * lift); }
        else m->tmi[t] = (unsigned char)i0;
    }
}
static void think_new_round(Ctx *c) {
    Motion *m = c->m; ThinkRS *T = &m->rs.think;
    recut(c->s, c->sh, 1, 6);
    for (int g = 0; g < m->ng; g++) { T->layer[g] = (signed char)(mrand(c->s) < .5f ? 0 : 1); T->gap[g] = rnd(c->s, .03f, .11f); } /* every patch in a layer: nothing stays behind to be crossed */
    random_axis5(c->s, c->sh, T->ax);
    T->at = c->now; T->flags = 1;
}

/* Puzzle and Smooth puzzle: the tempo of solving a cube, __.__.__. : two turns back to back (one layer, then the
 * other the opposite way), a halt, and again; each layer's turns add up to zero. Puzzle speeds up and stops
 * dead; Smooth eases each turn in and out and starts the second of a pair halfway through the first. */
static void think(Ctx *c, int smooth) {
    Motion *m = c->m; ThinkRS *T = &m->rs.think;
    static const int SEQ[6][2] = {{0, 1}, {1, -1}, {0, 1}, {1, -1}, {0, -2}, {1, 2}};
    const float LIFT = .4f, FOLD = .35f, REST = .7f, HALT = .34f;
    if (!c->sh->nt) return;
    if (!T->flags || c->now - T->at > T->end + FOLD + REST) { /* a new round: new cut, new axis, new layers */
        think_new_round(c);
        float t0 = LIFT + .15f;
        for (int i = 0; i < 6; i++) {
            int mult = abs(SEQ[i][1]) * (smooth ? 2 : 1);
            float d = smooth ? (mult > 2 ? .72f : .5f) : (mult > 1 ? .36f : .26f);
            T->seq_d[i] = d; T->seq_s[i] = t0;
            t0 += i % 2 ? d + HALT * (smooth ? 1.4f : 1) : smooth ? d * .5f : d;
        }
        T->end = t0; /* the fold starts here; flags bits 1..6: which turns have landed */
    }
    float e = (float)(c->now - T->at);
    float lift = e < LIFT ? outx(e / LIFT) : e < T->end ? 1 : fmaxf(0, 1 - in3((e - T->end) / FOLD));
    float ang[2] = {0, 0};
    for (int i = 0; i < 6; i++) {
        float u = clampf((e - T->seq_s[i]) / T->seq_d[i], 0, 1), mult = (float)SEQ[i][1] * (smooth ? 2 : 1);
        ang[SEQ[i][0]] += mult * STEP5 * (smooth ? (1 - cosf(PI_F * u)) / 2 : u * u);
        if (u >= 1 && !(T->flags & (2 << i))) { /* a soft blink as a turn lands */
            T->flags |= 2 << i;
            for (int t = 0; t < c->sh->nt; t++) if (T->layer[m->grp[t]] == SEQ[i][0]) for (int k = 0; k < 3; k++) m->glow[c->sh->t[t][k]] = fmaxf(m->glow[c->sh->t[t][k]], smooth ? .35f : .7f);
        }
    }
    layer_mats(c, ang[0], ang[1], lift);
    m->inner = .3f; m->inner_rate = -.3f;
    c->w->rate = .15f;
}
static void r_puzzle(Ctx *c) { think(c, 0); }
static void r_smooth(Ctx *c) { think(c, 1); }

/* Long spin: one lifted layer at a time runs three quick full turns, then the other runs them back, while the
 * whole shell turns slowly underneath */
static void r_long(Ctx *c) {
    Motion *m = c->m; ThinkRS *T = &m->rs.think;
    const float LIFT = .4f, SPIN = 1.45f, GAP = .12f, FOLD = .35f, REST = .8f;
    if (!c->sh->nt) return;
    if (!T->flags || c->now - T->at > T->end + FOLD + REST) {
        think_new_round(c);
        T->dir = mrand(c->s) < .5f ? 1 : -1;
        T->seq_s[0] = LIFT + .1f; T->seq_s[1] = T->seq_s[0] + SPIN + GAP;
        T->end = T->seq_s[1] + SPIN + .1f;
    }
    float e = (float)(c->now - T->at);
    float lift = e < LIFT ? outx(e / LIFT) : e < T->end ? 1 : fmaxf(0, 1 - in3((e - T->end) / FOLD));
    float a0 = TAU * 3 * ramp((e - T->seq_s[0]) / SPIN, .28f) * T->dir, a1 = -TAU * 3 * ramp((e - T->seq_s[1]) / SPIN, .28f) * T->dir;
    layer_mats(c, a0, a1, lift);
    m->inner = .3f; m->inner_rate = .6f;
    set_rate(c, .14f);
}

/* Blades: five single triangles leave the shell and become long thin blades; they hover in a fan behind the core,
 * sweep across the front in a V like a skein of geese, hover in a fan on the far side, sweep back the other way and
 * go home. Each blade is its triangle mapped by its own matrix onto the blade's shape and place, worked out in view
 * space (shell radii, x right, y down, z toward you), so nothing is carried from frame to frame but the core's spin. */
#define BL_OUT .6f     /* leaving the shell, or going home: each blade's own time */
#define BL_STAG .07f   /* how far behind the one before it each blade starts */
#define BL_SWEEP .75f  /* one sweep across the front */
#define BL_PAUSE .9f   /* the hover between the sweeps */
#define BL_T1 (BL_OUT + 4 * BL_STAG + .25f)
#define BL_T2 (BL_T1 + BL_SWEEP + BL_PAUSE)
#define BL_TH (BL_T2 + BL_SWEEP + .2f)
#define BL_END (BL_TH + BL_OUT + 4 * BL_STAG)
static const float BL_LEN = .5f;  /* a blade's length, shell radii (of the core at its usual size) */
static const float BL_SHRINK = .22f; /* the core draws in by this much while its blades are out, to give them room */
/* a blade's corners in its own frame (along it to the tip, across it), times its length: the tip, a corner almost
 * under it on a long straight edge, and the other a little out to the side and up, a thin sheared dagger */
static const float BL_SHAPE[3][2] = {{.66f, 0}, {-.34f, -.03f}, {-.25f, .13f}};

static void frame_cols(float *F, const float *a, const float *b, const float *c) { for (int r = 0; r < 3; r++) { F[r * 3] = a[r]; F[r * 3 + 1] = b[r]; F[r * 3 + 2] = c[r]; } }
static void m3_to_q(const float *m, float *q) { /* a rotation as a unit quaternion (w, x, y, z) */
    float tr = m[0] + m[4] + m[8];
    if (tr > 0) { float s = sqrtf(tr + 1) * 2; q[0] = s / 4; q[1] = (m[7] - m[5]) / s; q[2] = (m[2] - m[6]) / s; q[3] = (m[3] - m[1]) / s; }
    else if (m[0] > m[4] && m[0] > m[8]) { float s = sqrtf(1 + m[0] - m[4] - m[8]) * 2; q[0] = (m[7] - m[5]) / s; q[1] = s / 4; q[2] = (m[1] + m[3]) / s; q[3] = (m[2] + m[6]) / s; }
    else if (m[4] > m[8]) { float s = sqrtf(1 + m[4] - m[0] - m[8]) * 2; q[0] = (m[2] - m[6]) / s; q[1] = (m[1] + m[3]) / s; q[2] = s / 4; q[3] = (m[5] + m[7]) / s; }
    else { float s = sqrtf(1 + m[8] - m[0] - m[4]) * 2; q[0] = (m[3] - m[1]) / s; q[1] = (m[2] + m[6]) / s; q[2] = (m[5] + m[7]) / s; q[3] = s / 4; }
}
static void q_to_m3(const float *q, float *m) {
    float w = q[0], x = q[1], y = q[2], z = q[3];
    float r[9] = {1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y),
                  2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x),
                  2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)};
    memcpy(m, r, sizeof r);
}
static void m3_slerp(float *out, const float *A, const float *B, float u) { /* the turn from A to B, part u of the way */
    float a[4], b[4], q[4];
    m3_to_q(A, a); m3_to_q(B, b);
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (d < 0) { for (int i = 0; i < 4; i++) b[i] = -b[i]; d = -d; }
    float ka = 1 - u, kb = u;
    if (d < .9995f) { float th = acosf(fminf(1, d)), sn = sinf(th); ka = sinf((1 - u) * th) / sn; kb = sinf(u * th) / sn; }
    float l = 0;
    for (int i = 0; i < 4; i++) { q[i] = a[i] * ka + b[i] * kb; l += q[i] * q[i]; }
    l = sqrtf(l);
    for (int i = 0; i < 4; i++) q[i] /= l;
    q_to_m3(q, out);
}
static float wrap_pi(float a) { return atan2f(sinf(a), cosf(a)); }
/* blade i hovering in the fan behind the core, on the right (side > 0) or the left: they fan out from straight up to
 * low on that side, each pointing away from the core */
static void blade_fan(int i, float side, float clock, float *p, float *th) {
    float deg = 95 - i * 18.75f, a = (side > 0 ? deg : 180 - deg) * PI_F / 180, r = 1.0f + .025f * sinf(clock * 2.2f + i * 1.3f);
    set3(p, r * cosf(a), -r * sinf(a), -.55f);
    *th = a;
}
/* blade i in the V, part u of a sweep in direction dir (+1: left to right); vel: the lead's speed (per second) */
static void blade_vee(int i, float dir, float u, float *p, float *th, float *lead, float *vel) {
    const float RX = 1.0f, Y0 = -.35f, YD = .3f, DROP = .7f, Z0 = -.55f, ZD = 1.55f, GB = .3f, GS = .15f; /* DROP: the line slopes down across */
    float q = u - .75f * sinf(TAU * u) / TAU, dq = 1 - .75f * cosf(TAU * u); /* slow behind, fastest across the front */
    float L[3] = {-dir * RX * cosf(PI_F * q), Y0 + YD * sinf(PI_F * q) + DROP * (q - .5f), Z0 + ZD * sinf(PI_F * q)};
    float D[3] = {dir * RX * PI_F * sinf(PI_F * q), YD * PI_F * cosf(PI_F * q) + DROP, ZD * PI_F * cosf(PI_F * q)}, H[3];
    if (lead) memcpy(lead, L, sizeof L);
    if (vel) for (int k = 0; k < 3; k++) vel[k] = D[k] * dq / BL_SWEEP;
    memcpy(H, D, sizeof H); norm3(H);
    int k = i - 2; /* the lead in the middle; the others trail back on either wing */
    for (int q2 = 0; q2 < 3; q2++) p[q2] = L[q2] - H[q2] * GB * abs(k);
    p[1] += k * GS;
    *th = atan2f(-D[1], fabsf(D[0]) > 1e-4f ? D[0] : dir * 1e-4f);
}
static void blades_new_round(Ctx *c) {
    Session *s = c->s; Motion *m = c->m; const Shape *sh = c->sh; BladesRS *B = &m->rs.blades;
    recut(s, sh, 1, 1); /* every triangle its own patch */
    for (int g = 0; g < m->ng; g++) m->gw[g] = 1;
    int pick[MAX_BLADE], n = 1, want = sh->nt < MAX_BLADE ? sh->nt : MAX_BLADE;
    pick[0] = (int)(mrand(s) * sh->nt) % sh->nt;
    while (n < want) { /* each next one the farthest from those picked */
        int best = -1; float bd = -1;
        for (int t = 0; t < sh->nt; t++) {
            int used = 0; float d = 9;
            for (int k = 0; k < n; k++) { if (pick[k] == t) used = 1; d = fminf(d, 1 - dot3(sh->tn[t], sh->tn[pick[k]])); }
            if (used) continue;
            d += mrand(s) * .02f;
            if (d > bd) { bd = d; best = t; }
        }
        pick[n++] = best;
    }
    for (int i = 0; i < n; i++) {
        Blade *b = &B->blade[i];
        int t = pick[i], j = (int)(mrand(s) * 3) % 3;
        b->t = t; b->tip = sh->t[t][j]; b->a = sh->t[t][(j + 1) % 3]; b->b = sh->t[t][(j + 2) % 3];
    }
    B->n = n;
    B->dir = mrand(s) < .5f ? 1 : -1;
    set3(B->core_v, 0, 0, 0);
    B->at = c->now;
}
/* blade b's triangle: its centre, and its own frame (to the tip, across, out) with the tip's and a corner's places in
 * it; then the matrix that turns the triangle into the blade (in that frame) */
static void blade_model(const Shape *sh, const Blade *b, float *cen, float *Fm, float *S2) {
    float en[3], et[3], eb[3], vt[3], va[3], vb[3];
    for (int q = 0; q < 3; q++) cen[q] = (sh->v[b->tip][q] + sh->v[b->a][q] + sh->v[b->b][q]) / 3;
    for (int q = 0; q < 3; q++) { vt[q] = sh->v[b->tip][q] - cen[q]; va[q] = sh->v[b->a][q] - cen[q]; vb[q] = sh->v[b->b][q] - cen[q]; }
    memcpy(en, sh->tn[b->t], sizeof en); norm3(en);
    float k = dot3(vt, en);
    for (int q = 0; q < 3; q++) et[q] = vt[q] - en[q] * k;
    norm3(et); cross3(eb, en, et);
    frame_cols(Fm, et, eb, en);
    float ot[2] = {dot3(vt, et), dot3(vt, eb)}, oa[2] = {dot3(va, et), dot3(va, eb)}, ob[2] = {dot3(vb, et), dot3(vb, eb)};
    if (oa[1] > ob[1]) memcpy(oa, ob, sizeof oa); /* the corner on the straight edge: the one on the -across side */
    float cx = (BL_SHAPE[0][0] + BL_SHAPE[1][0] + BL_SHAPE[2][0]) / 3, cy = (BL_SHAPE[0][1] + BL_SHAPE[1][1] + BL_SHAPE[2][1]) / 3, Lm = BL_LEN * CIRCUM;
    float tt[2] = {(BL_SHAPE[0][0] - cx) * Lm, (BL_SHAPE[0][1] - cy) * Lm}, ta[2] = {(BL_SHAPE[1][0] - cx) * Lm, (BL_SHAPE[1][1] - cy) * Lm};
    float det = ot[0] * oa[1] - oa[0] * ot[1];
    if (fabsf(det) < 1e-9f) { S2[0] = S2[3] = 1; S2[1] = S2[2] = 0; return; }
    /* S [ot oa] = [tt ta]: S = [tt ta] [ot oa]^-1 */
    float i00 = oa[1] / det, i01 = -oa[0] / det, i10 = -ot[1] / det, i11 = ot[0] / det;
    S2[0] = tt[0] * i00 + ta[0] * i10; S2[1] = tt[0] * i01 + ta[0] * i11;
    S2[2] = tt[1] * i00 + ta[1] * i10; S2[3] = tt[1] * i01 + ta[1] * i11;
}
static void r_blades(Ctx *c) {
    Session *s = c->s; Motion *m = c->m; const Shape *sh = c->sh; const float *M = c->M; BladesRS *B = &m->rs.blades;
    const float CORE = .18f;
    c->w->rate = 0; m->inner = .35f; m->inner_rate = -.5f;
    if (!sh->nt) return;
    if (!B->n || c->now - B->at > BL_END + .6f) blades_new_round(c);
    float e = (float)(c->now - B->at), Mt[9], dtb = fminf(.05f, fmaxf(0, c->dt)), clock = (float)(c->now - s->created), want[3] = {0, 0, 0};
    float d1 = (float)B->dir;
    m3_tr(Mt, M);
    int has_want = 0;
    /* the core draws in while the blades are out; they are placed and sized for the core at its usual size */
    float drawn = e < BL_T1 ? io3(e / BL_OUT) : e >= BL_TH ? 1 - io3((e - BL_TH) / (BL_OUT + 4 * BL_STAG)) : 1;
    c->w->scale *= 1 - BL_SHRINK * drawn;
    float sc = m->scale > .2f ? m->scale : 1;
    for (int i = 0; i < B->n; i++) {
        Blade *b = &B->blade[i];
        float cen[3], Fm[9], S2[4], P[3], th, k = 1, out = 0; /* k: how far it has become a blade (0: at home in the shell) */
        int smash = 0;
        blade_model(sh, b, cen, Fm, S2);
        if (e < BL_T1 || e >= BL_TH) { /* leaving the shell into the fan on the side the first sweep starts from, or home from there */
            float u = e < BL_T1 ? clampf((e - i * BL_STAG) / BL_OUT, 0, 1) : clampf((e - BL_TH - (4 - i) * BL_STAG) / BL_OUT, 0, 1);
            k = e < BL_T1 ? io3(u) : 1 - io3(u);
            out = bump(u);
            blade_fan(i, -d1, clock, P, &th);
        } else if (e < BL_T1 + BL_SWEEP || (e >= BL_T2 && e < BL_T2 + BL_SWEEP)) { /* a sweep: out of the fan into the V, across, into the fan on the far side */
            int second = e >= BL_T2;
            float dir = second ? -d1 : d1, u = (e - (second ? BL_T2 : BL_T1)) / BL_SWEEP, fp[3], fth, vp[3], vth, lead[3], vel[3];
            float w = smooth01(u / .22f) * smooth01((1 - u) / .22f);
            blade_fan(i, u < .5f ? -dir : dir, clock, fp, &fth);
            blade_vee(i, dir, u, vp, &vth, lead, vel);
            for (int q = 0; q < 3; q++) P[q] = fp[q] + (vp[q] - fp[q]) * w;
            th = fth + wrap_pi(vth - fth) * w;
            smash = w > .5f && P[2] > -.2f;
            if (i == 2) { /* the core turns with the lead */
                float ll = dot3(lead, lead), om[3];
                cross3(om, lead, vel);
                for (int q = 0; q < 3; q++) want[q] = om[q] / fmaxf(ll, .2f) * CORE * w;
                has_want = 1;
            }
            if (P[2] > 0) for (int v = 0; v < 3; v++) { int nv = sh->t[b->t][v]; m->glow[nv] = fmaxf(m->glow[nv], .25f * w); }
        } else blade_fan(i, (e < BL_T2 ? d1 : -d1), clock, P, &th); /* hovering in the fan */
        B->pos[i][0] = P[0]; B->pos[i][1] = P[1]; B->pos[i][2] = P[2];
        B->smash[i] = (unsigned char)smash;
        if (k <= 0) continue; /* at home: drawn as part of the shell */
        for (int q = 0; q < 3; q++) P[q] /= sc;
        for (int v = 0; v < 3; v++) { int nv = sh->t[b->t][v]; m->glow[nv] = fmaxf(m->glow[nv], .3f * k); }
        /* the blade's frame in view space: along it to the tip, across, and its face toward you */
        float T[3] = {cosf(th), -sinf(th), 0}, N[3] = {0, 0, 1}, Bx[3], Fv[9], Rb[9], Rk[9], FmT[9], S3[9], Lin[9], X[9], tmp[9];
        cross3(Bx, N, T);
        frame_cols(Fv, T, Bx, N);
        m3_tr(FmT, Fm);
        m3_mul(Rb, Fv, FmT);
        if (k < 1) {
            float hp[3], hn[3];
            m3_slerp(Rk, M, Rb, k);
            m3_app(hp, M, cen); m3_app(hn, M, sh->tn[b->t]); norm3(hn);
            for (int q = 0; q < 3; q++) P[q] = hp[q] / CIRCUM + (P[q] - hp[q] / CIRCUM) * k + hn[q] * .3f * out; /* up off the shell first */
        } else memcpy(Rk, Rb, sizeof Rk);
        float g = 1 + (1 / sc - 1) * k, S[4] = {(1 + (S2[0] - 1) * k) * g, S2[1] * k * g, S2[2] * k * g, (1 + (S2[3] - 1) * k) * g};
        float S3v[9] = {S[0], S[1], 0, S[2], S[3], 0, 0, 0, 1};
        memcpy(S3, S3v, sizeof S3);
        m3_mul(tmp, Fm, S3); m3_mul(tmp, tmp, FmT); m3_mul(Lin, Rk, tmp);
        m3_mul(X, Mt, Lin);
        float xc[3], pv[3], tv[3];
        m3_app(xc, X, cen); m3_app(pv, Mt, P);
        for (int q = 0; q < 3; q++) tv[q] = pv[q] - xc[q] / CIRCUM;
        m->tmi[b->t] = (unsigned char)tm_add(m, X);
        tt_set(m, b->t, tv, 1);
    }
    /* the core carries momentum: its spin chases what the sweep wants and coasts down after */
    float kk = 1 - expf(-dtb * (has_want ? 4 : 3.5f));
    for (int q = 0; q < 3; q++) B->core_v[q] += (want[q] - B->core_v[q]) * kk;
    float cw = len3(B->core_v);
    if (cw > 1e-4f) { float ax[3] = {B->core_v[0] / cw, B->core_v[1] / cw, B->core_v[2] / cw}, a[3], R[9]; to_turn_space(s, ax, a); m3_rot(R, a, cw * dtb); m3_mul(m->q, R, m->q); m3_ortho(m->q); }
}

/* Engine: a turbine belt round the middle (a layer turn about a 5-fold axis) and pistons on the caps */
static void r_engine(Ctx *c) {
    Session *s = c->s; Motion *m = c->m; const Shape *sh = c->sh; EngineRS *E = &m->rs.engine;
    if (!sh->nt) { c->w->rate = 1; return; }
    if (E->built != m->cutid) { /* (re)build for this cut: a belt round the middle, pistons on the caps in firing order */
        random_axis5(s, sh, m->lax);
        float u0[3], w0[3];
        square_to(u0, m->lax); cross3(w0, m->lax, u0);
        unsigned char on_belt_g[MAX_T]; memset(on_belt_g, 0, sizeof on_belt_g);
        for (int t = 0; t < sh->nt; t++) { E->belt[t] = fabsf(dot3(sh->tn[t], m->lax)) < .4f; if (E->belt[t]) on_belt_g[m->grp[t]] = 1; }
        float ang[MAX_T]; int n = 0;
        for (int g = 0; g < m->ng; g++) if (!on_belt_g[g] && m->gw[g] > 0) { ang[n] = atan2f(dot3(m->gn[g], w0), dot3(m->gn[g], u0)); E->order[n++] = (unsigned char)g; }
        for (int i = 1; i < n; i++) for (int j = i; j > 0 && ang[j] < ang[j - 1]; j--) { /* by angle round the axis */
            float ta = ang[j]; ang[j] = ang[j - 1]; ang[j - 1] = ta;
            unsigned char to = E->order[j]; E->order[j] = E->order[j - 1]; E->order[j - 1] = to;
        }
        E->n = n; E->built = m->cutid;
    }
    float belt = fmodf(c->e * 2.6f, TAU);
    for (int t = 0; t < sh->nt; t++) m->ang[t] = E->belt[t] ? belt : 0;
    m->ang_on = 1;
    float per = .07f, cyc = fmaxf(E->n * per, .5f);
    for (int i = 0; i < E->n; i++) {
        int g = E->order[i];
        float e = fmodf(fmodf(c->e - i * per, cyc) + cyc, cyc), near = 0;
        if (s->hover > .3f) { float d[3]; m3_app(d, c->M, m->gn[g]); near = fmaxf(0, 1 - hypotf(d[0] - s->look_x_to, d[1] - s->look_y_to)); }
        c->w->grp[g] += (.12f + .1f * near * s->hover) * env(e, .045f, .03f, .13f) / fmaxf(.45f, m->gw[g]);
    }
    m->inner = .6f; m->inner_rate = -2.4f;
    c->w->rate = .32f;
}

/* Gyro and Helix: a 5-fold axis, and the axis most nearly square to it */
static void axes_enter(Ctx *c) {
    AxesRS *A = &c->m->rs.axes;
    random_axis5(c->s, c->sh, A->ax);
    float best = 9;
    for (int v = 0; v < c->sh->nv; v++) { float u[3]; unit_v(c->sh, v, u); float d = fabsf(dot3(u, A->ax)); if (d < best) { best = d; memcpy(A->ax2, u, sizeof u); } }
}
static void r_gyro(Ctx *c) { /* two belts at right angles, like the rings of a gyroscope */
    Motion *m = c->m; AxesRS *A = &m->rs.axes;
    float R1[9], R2[9];
    m3_rot(R1, A->ax, c->e * 2.3f); m3_scale(R1, 1.06f);
    m3_rot(R2, A->ax2, -c->e * 1.7f); m3_scale(R2, 1.1f);
    int i1 = tm_add(m, R1), i2 = tm_add(m, R2);
    for (int t = 0; t < c->sh->nt; t++) { /* two belts at two heights: they cross without touching */
        if (fabsf(dot3(c->sh->tn[t], A->ax)) < .3f) m->tmi[t] = (unsigned char)i1;
        else if (fabsf(dot3(c->sh->tn[t], A->ax2)) < .3f) m->tmi[t] = (unsigned char)i2;
    }
    m->inner = .6f; m->inner_rate = -2.4f;
    set_rate(c, c->sh->nt ? .05f : .9f);
}
static void r_helix(Ctx *c) { /* the shell turns while a spiral of lifted patches climbs it */
    Motion *m = c->m; AxesRS *A = &m->rs.axes;
    if (!c->sh->nt) { c->w->rate = 1; return; }
    float spin = fmodf(c->e * 2.4f, TAU), u0[3], w0[3];
    memcpy(m->lax, A->ax, sizeof m->lax);
    for (int t = 0; t < c->sh->nt; t++) m->ang[t] = spin;
    m->ang_on = 1;
    square_to(u0, A->ax); cross3(w0, A->ax, u0);
    for (int g = 0; g < m->ng; g++) {
        float h = dot3(m->gn[g], A->ax), a = atan2f(dot3(m->gn[g], w0), dot3(m->gn[g], u0));
        float f = (a + c->e * 3) / TAU, target = (f - floorf(f)) * 2.4f - 1.2f, d = h - target; /* a spiral that climbs as it turns */
        c->w->grp[g] += .13f * expf(-d * d / .03f) / wof(m, g);
    }
    m->inner = .6f; m->inner_rate = -2.4f;
    c->w->rate = 0;
}

/* Typewriter: row by row the patches pop out like keys; at the end of a row, a ring and a carriage return */
static void r_typewriter(Ctx *c) {
    Motion *m = c->m; TypewriterRS *T = &m->rs.typewriter;
    if (!c->sh->nt) { c->w->rate = 1; return; }
    if (T->built != m->cutid) { /* rows by height along the axis, each ordered across */
        if (!T->built) random_axis5(c->s, c->sh, T->ax);
        float u0[3], w0[3], ang[MAX_T]; int n = 0;
        square_to(u0, T->ax); cross3(w0, T->ax, u0);
        for (int r = 0; r < 5; r++) {
            T->start[r] = n;
            for (int g = 0; g < m->ng; g++) {
                if (m->gw[g] <= 0 || (int)clampf(floorf((1 - dot3(m->gn[g], T->ax)) / 2 * 5), 0, 4) != r) continue;
                ang[n] = atan2f(dot3(m->gn[g], w0), dot3(m->gn[g], u0)); T->order[n++] = (unsigned char)g;
            }
            for (int i = T->start[r] + 1; i < n; i++) for (int j = i; j > T->start[r] && ang[j] < ang[j - 1]; j--) {
                float ta = ang[j]; ang[j] = ang[j - 1]; ang[j - 1] = ta;
                unsigned char to = T->order[j]; T->order[j] = T->order[j - 1]; T->order[j - 1] = to;
            }
        }
        T->start[5] = n; T->built = m->cutid;
    }
    const float per = .085f;
    float e = c->e - T->row_at;
    int r = T->row % 5, a = T->start[r], len = T->start[r + 1] - a;
    for (int i = 0; i < len; i++) { int g = T->order[a + i]; c->w->grp[g] += .14f * env(e - i * per, .04f, .03f, .12f) / wof(m, g); }
    if (e > len * per + .25f) { /* the end of the row: ding, carriage return */
        ring(m, c->now, .35f);
        turn_model(c->s, T->ax, -PI_F / 5, .28f, c->now);
        T->row++; T->row_at = c->e + .3f;
    }
    set_rate(c, .02f);
}

/* Forge: one blow at a time, with weight. 1 to 3 pieces on the front (never the ones that struck last) rise
 * slowly, gathering heat, quiver at the top, then come down hard, once: a flash, hot sparks, embers, a ring,
 * the shell recoils and a ripple runs out from the blow. Every third blow the work is turned. */
static int in_list(const int *a, int n, int v) { for (int i = 0; i < n; i++) if (a[i] == v) return 1; return 0; }
static void r_forge(Ctx *c) {
    Session *s = c->s; Motion *m = c->m; const Shape *sh = c->sh; const float *M = c->M; Want *w = c->w; ForgeRS *F = &m->rs.forge;
    const float P = 2.4f, RISE = 1.3f, HOLD = 1.65f, HIT = 1.72f;
    float u = fmodf(c->e, P);
    int k = (int)(c->e / P) + 1; /* (k 0 is "not started") */
    w->rate = 0;
    if (!m->ng) return;
    if (k != F->k) { /* a new blow: 1 to 3 pieces facing you, none of the last ones */
        F->k = k; F->hit = 0;
        float f[3]; toward(M, f);
        int ok[MAX_T], n = 0;
        for (int g = 0; g < m->ng; g++) if (dot3(m->gn[g], f) > .35f && !in_list(F->last, F->nlast, g)) ok[n++] = g;
        for (int i = n - 1; i > 0; i--) { int j = (int)(mrand(s) * (i + 1)) % (i + 1), t = ok[i]; ok[i] = ok[j]; ok[j] = t; }
        F->nham = n < 1 ? 0 : 1 + (int)(mrand(s) * 3) % 3;
        if (F->nham > n) F->nham = n;
        for (int i = 0; i < F->nham; i++) F->ham[i] = ok[i];
        if (!F->nham) { float bd = -2; for (int g = 0; g < m->ng; g++) if (m->gw[g] > 0 && dot3(m->gn[g], f) > bd) { bd = dot3(m->gn[g], f); F->ham[0] = g; } F->nham = 1; }
        F->nlast = F->nham;
        for (int i = 0; i < F->nham; i++) { F->last[i] = F->ham[i]; F->rise[i] = rnd(s, .85f, 1.15f); }
        if (k % 3 == 0) { float a[3] = {rnd(s, -.5f, .5f), 1, rnd(s, -.5f, .5f)}; norm3(a); turn_view(s, a, PI_F / 5, .5f, c->now); } /* the work turned to a fresh spot */
    }
    /* the hammer's height: a slow heavy rise, a quiver at the top, one hard strike just past flush, back */
    float quiver = u > RISE && u < HOLD ? .012f * sinf(u * 70) * (u - RISE) / (HOLD - RISE) : 0;
    float lift = u < RISE ? .5f * io3(u / RISE) : u < HOLD ? .5f + quiver : u < HIT ? .5f - .56f * in3((u - HOLD) / (HIT - HOLD)) : -.06f * (1 - out3(clampf((u - HIT) / .45f, 0, 1)));
    lift *= .6f;
    for (int t = 0; t < sh->nt; t++) for (int h = 0; h < F->nham; h++) if (m->grp[t] == F->ham[h]) tt_set(m, t, m->gn[F->ham[h]], lift * F->rise[h]); /* not smoothed: the strike stays sharp */
    float n[3] = {0, 0, 0};
    for (int h = 0; h < F->nham; h++) for (int q = 0; q < 3; q++) n[q] += m->gn[F->ham[h]][q];
    norm3(n);
    if (u < HIT) { float pw = clampf(u / HOLD, 0, 1); for (int h = 0; h < F->nham; h++) glow_patch(m, sh, F->ham[h], .15f + .75f * pw * pw); w->bright *= 1 + .5f * pw * pw; } /* power gathering in the hammer */
    if (u > .4f && u < HOLD && mrand(s) < 21 * c->dt) { /* heat drawn in */
        const float *g = m->gn[F->ham[(int)(mrand(s) * F->nham) % F->nham]];
        float r0 = 1.3f + mrand(s) * .15f, p[3] = {g[0] * r0, g[1] * r0, g[2] * r0}, v[3];
        for (int q = 0; q < 3; q++) v[q] = -(p[q] - g[q] * 1.1f) * 2;
        spark(s, M, p, v, .4f, 1, 0, .9f, SK_MOTE, 0, RGB255(255, 200, 140));
    }
    if (u >= HIT && !F->hit) {
        F->hit = 1; ring(m, c->now, .9f); flash(m, sh->nv, .35f);
        float nv[3], Z[3] = {0, 0, 1}, ax[3], p[3] = {n[0] * 1.03f, n[1] * 1.03f, n[2] * 1.03f};
        m3_app(nv, M, n); cross3(ax, nv, Z); norm3(ax);
        if (len3(ax) > .5f) jerk(s, ax, .07f, c->now);
        spark_at(s, M, p, .35f, 30 + 8.0f * F->nham, RGB255(255, 196, 130));
        for (int i = 0; i < 26 + 10 * F->nham; i++) {
            float d[3] = {n[0] + rnd(s, -1, 1), n[1] + rnd(s, -1, 1), n[2] + rnd(s, -1, 1)}, sp = rnd(s, .7f, 2.1f);
            norm3(d); for (int q = 0; q < 3; q++) d[q] *= sp;
            spark(s, M, p, d, rnd(s, .5f, 1.1f), 1.3f, 1.6f, rnd(s, 1, 2.3f), SK_STREAK, 1, RGB255(255, 150, 50));
        }
        for (int i = 0; i < 9; i++) {
            float d[3] = {n[0] + rnd(s, -.6f, .6f), n[1] + rnd(s, -.6f, .6f), n[2] + rnd(s, -.6f, .6f)}, sp = rnd(s, .2f, .5f);
            norm3(d); for (int q = 0; q < 3; q++) d[q] *= sp;
            spark(s, M, p, d, rnd(s, 1.3f, 2.4f), .8f, .4f, rnd(s, 1.4f, 2.6f), SK_EMBER, 1, RGB255(255, 140, 40));
        }
        for (int h = 0; h < F->nham; h++) glow_patch(m, sh, F->ham[h], 1);
    }
    float since = u - HIT;
    if (since > 0) { /* the shell recoils and a ripple runs out from the blow */
        const float *n0 = m->gn[F->ham[0]];
        for (int g = 0; g < m->ng; g++) {
            if (in_list(F->ham, F->nham, g)) continue;
            float d = acosf(clampf(dot3(m->gn[g], n0), -1, 1));
            w->grp[g] += .07f * bump((since - d * .22f) / .25f) * (1 - d / PI_F) / wof(m, g);
        }
        w->bright *= 1 + .9f * expf(-since * 8);
        w->scale *= 1 - .045f * expf(-since * 12);
    }
}

/* Loom: two crossing belts lift out as layers; then a shuttle runs along one, laying a light thread, and at the
 * end of each pass the other belt clicks a step round and the shuttle comes back the other way */
static void r_loom(Ctx *c) {
    Session *s = c->s; Motion *m = c->m; const Shape *sh = c->sh; LoomRS *L = &m->rs.loom;
    float a1[3] = {1, .2f, .1f}, a2[3] = {.1f, 1, .3f};
    norm3(a1); norm3(a2);
    set_rate(c, .08f);
    if (!sh->nt) return;
    float e = c->e, out = io3(clampf(e / .7f, 0, 1)), R2[9], S1[9];
    m3_rot(R2, a2, L->ang); m3_scale(R2, 1 + .07f * out);
    memcpy(S1, I3, sizeof S1); m3_scale(S1, 1 + .04f * out);
    int i2 = tm_add(m, R2), i1 = tm_add(m, S1);
    m->ta_on = 1;
    for (int t = 0; t < sh->nt; t++) {
        m->talpha[t] = 1;
        if (fabsf(dot3(sh->tn[t], a2)) < .32f) m->tmi[t] = (unsigned char)i2;
        else if (fabsf(dot3(sh->tn[t], a1)) < .32f) m->tmi[t] = (unsigned char)i1;
        else m->talpha[t] = 1 - .3f * out;
    }
    m->trail_col = RGB255(255, 214, 150);
    if (e < .9f) { if (e > .7f && !L->lit) { L->lit = 1; ring(m, c->now, .4f); } return; } /* layered out: now the loom starts */
    const float P = 1.15f;
    float wv = e - .9f, u = fmodf(wv, P) / P;
    int k = (int)(wv / P) + 1, dir = k % 2 ? 1 : -1;
    if (k != L->k) { L->k = k; L->from = L->ang; L->rang = 0; m->ntrail = 0; }
    L->ang = L->from + io3(clampf((u - .78f) / .22f, 0, 1)) * TAU / 15;
    float b1[3], b2[3], sp[3];
    square_to(b1, a1); cross3(b2, a1, b1);
    float run = u / .78f, sa = (dir > 0 ? io3(run) : 1 - io3(run)) * PI_F * 1.6f - .8f * PI_F;
    for (int q = 0; q < 3; q++) sp[q] = (b1[q] * cosf(sa) + b2[q] * sinf(sa)) * 1.12f;
    for (int i = 0; i < sh->nv; i++) {
        float d[3] = {sh->v[i][0] / CIRCUM - sp[0], sh->v[i][1] / CIRCUM - sp[1], sh->v[i][2] / CIRCUM - sp[2]}, l = len3(d);
        if (l < .5f) m->glow[i] = fmaxf(m->glow[i], (1 - l / .5f) * .9f);
    }
    if (u < .8f) trail_push(m, sp); else trail_pop(m); /* the thread it lays, fading as the pass ends */
    m->trail_on = 1;
    if (u < .78f && mrand(s) < 18 * c->dt) { float v[3] = {rnd(s, -.12f, .12f), rnd(s, -.12f, .12f), rnd(s, -.12f, .12f)}; spark(s, c->M, sp, v, rnd(s, .5f, .9f), 2, 0, 1, SK_MOTE, 0, RGB255(255, 220, 160)); }
    if (u > .8f && !L->rang) { L->rang = 1; ring(m, c->now, .35f); }
}

/* Conveyor: the belt round a 5-fold axis floats up as two rings, the upper one higher, and they take turns
 * stepping one slot (72°, where the net repeats exactly), the upper on, the lower back, each with a click; after
 * five rounds both are home, they settle in a wave and float up again */
static void conveyor_enter(Ctx *c) {
    ConveyorRS *C = &c->m->rs.conveyor;
    C->clicked = -1;
    random_axis5(c->s, c->sh, C->ax);
    float u0[3], w0[3];
    square_to(u0, C->ax); cross3(w0, C->ax, u0);
    for (int t = 0; t < c->sh->nt; t++) {
        float h = dot3(c->sh->tn[t], C->ax), a = atan2f(dot3(c->sh->tn[t], w0), dot3(c->sh->tn[t], u0)) / TAU + 1;
        C->ring[t] = fabsf(h) < .5f ? (h >= 0 ? 1 : 2) : 0; /* upper ring 1, lower ring 2 */
        C->ph[t] = a - floorf(a);
    }
}
static void r_conveyor(Ctx *c) {
    Session *s = c->s; Motion *m = c->m; const Shape *sh = c->sh; ConveyorRS *C = &m->rs.conveyor;
    c->w->rate = 0;
    if (!sh->nt) { c->w->rate = 1; return; }
    const float STEP = .6f, UP = .45f, RUN = 5 * 2 * STEP, CYC = UP + RUN + UP;
    float e = fmodf(c->e, CYC), run = clampf(e - UP, 0, RUN), within = fmodf(run, STEP) / STEP;
    int n = (int)(run / STEP), moving = run < RUN ? n % 2 : -1, done[2] = {(n + 1) / 2, n / 2};
    float ease = io3(clampf((within - .1f) / .75f, 0, 1)), R[2][9], H[2] = {.1f, .17f};
    for (int r = 0; r < 2; r++) m3_rot(R[r], C->ax, (r ? 1 : -1) * (done[r] + (moving == r ? ease : 0)) * STEP5);
    int step_no = (int)(c->e / CYC) * 100 + n;
    if (moving >= 0 && within > .85f && C->clicked != step_no) { C->clicked = step_no; ring(m, c->now, .25f); } /* a click as a ring lands one slot on */
    int ir[2] = {tm_add(m, R[0]), tm_add(m, R[1])};
    m->ta_on = 1;
    for (int t = 0; t < sh->nt; t++) {
        if (!C->ring[t]) { m->talpha[t] = .6f; continue; } /* the still caps, a little dimmer */
        int r = C->ring[t] - 1;
        float up = out3(clampf((e - C->ph[t] * .25f) / .2f, 0, 1)), down = in3(clampf((e - (CYC - UP) - C->ph[t] * .25f) / .2f, 0, 1));
        float n2[3]; m3_app(n2, R[r], sh->tn[t]);
        m->talpha[t] = 1;
        m->tmi[t] = (unsigned char)ir[r];
        tt_set(m, t, n2, H[r] * up * (1 - down));
        if (moving == r && mrand(s) < .24f * c->dt) {
            float p[3], v[3], cr[3]; cross3(cr, C->ax, sh->tn[t]); m3_app(v, R[r], cr);
            for (int q = 0; q < 3; q++) { p[q] = n2[q] * 1.1f; v[q] *= r ? .6f : -.6f; }
            spark(s, c->M, p, v, .5f, 2, 0, .9f, SK_MOTE, 0, RGB255(255, 214, 150));
        }
    }
}

/* Compressor: squeezed flat along one axis in three hard clicks, the waist bulging; then it vents in one stroke */
static void r_compressor(Ctx *c) {
    Session *s = c->s; Motion *m = c->m; CompressorRS *C = &m->rs.compressor;
    const float P = 2.2f;
    float u = fmodf(c->e, P);
    int k = (int)(c->e / P) + 1;
    if (C->k != k) { C->k = k; C->vented = 0; set3(m->sq_ax, rnd(s, -.4f, .4f), 1, rnd(s, -.4f, .4f)); norm3(m->sq_ax); }
    set_rate(c, .12f);
    float sq = 0;
    for (int i = 0; i < 3; i++) sq += .08f * out3(clampf((u - .2f - i * .38f) / .12f, 0, 1));
    float vent = u > 1.75f ? out3((u - 1.75f) / .15f) : 0;
    sq *= 1 - vent;
    if (vent > 0 && !C->vented) {
        C->vented = 1; ring(m, c->now, .9f);
        float z[3] = {0, 0, 0};
        spark_at(s, c->M, z, .35f, 36, RGB255(255, 190, 120));
        for (int i = 0; i < 24; i++) {
            float r[3], d[3], p[3]; rand_dir(s, r); cross3(d, m->sq_ax, r); norm3(d);
            float sp = rnd(s, .9f, 2);
            for (int q = 0; q < 3; q++) { p[q] = d[q] * 1.05f; d[q] *= sp; }
            spark(s, c->M, p, d, rnd(s, .3f, .6f), 2.5f, 0, rnd(s, 1, 1.8f), SK_STREAK, 1, RGB255(255, 170, 90));
        }
    }
    m->squash = sq;
    for (int g = 0; g < m->ng; g++) { float wv = 1 - fabsf(dot3(m->gn[g], m->sq_ax)); c->w->grp[g] += sq * 1.2f * wv * wv / wof(m, g); }
    c->w->bright *= 1 + sq * 3;
}

/* Swimmers: two fish of fine patches under the surface, leaving a wake; pointed at, they swim toward you */
static void swim_enter(Ctx *c) { c->m->rs.swim.fish[0].to = c->m->rs.swim.fish[1].to = -1; }
static void r_swim(Ctx *c) {
    Session *s = c->s; Motion *m = c->m; const Shape *sh = c->sh; const float *M = c->M; Fish *fish = m->rs.swim.fish;
    double now = c->now;
    float glide = .26f / (1 + 1.2f * s->hover);
    if (!sh->nt) return;
    if (fish[0].to < 0) { /* start them far apart */
        int a = (int)(mrand(s) * sh->nv) % sh->nv, b = a; float far = -1;
        for (int v = 0; v < sh->nv; v++) { float d = -dot3(sh->v[a], sh->v[v]); if (d > far) { far = d; b = v; } }
        int st[2] = {a, b};
        for (int f = 0; f < 2; f++) fish[f] = (Fish){st[f], sh->vn[st[f]][0], 0, {0}, now - f * .13, {0}};
    }
    float lift[MAX_T]; memset(lift, 0, sizeof lift);
    for (int f = 0; f < 2; f++) {
        Fish *F = &fish[f], *O = &fish[1 - f];
        if (now - F->at >= glide) {
            if (F->ntrail == 10) { memmove(F->trail, F->trail + 1, 9 * sizeof(int)); memmove(F->trail_at, F->trail_at + 1, 9 * sizeof(double)); F->ntrail--; }
            F->trail[F->ntrail] = F->from; F->trail_at[F->ntrail++] = now;
            float a[3], b[3], dir[3], want[3];
            view_dir(s, M, sh->v[F->from], a); view_dir(s, M, sh->v[F->to], b);
            for (int q = 0; q < 3; q++) dir[q] = b[q] - a[q];
            norm3(dir);
            want[0] = s->look_x_to - b[0]; want[1] = s->look_y_to - b[1]; want[2] = .9f - b[2]; norm3(want);
            int best = -1; float sc = -1e9f;
            for (int i = 0; i < sh->nadj[F->to]; i++) {
                int n = sh->vn[F->to][i], recent = n == F->from;
                for (int k = F->ntrail - 4 < 0 ? 0 : F->ntrail - 4; k < F->ntrail; k++) if (F->trail[k] == n) recent = 1;
                if (recent) continue;
                float cc[3], d[3]; view_dir(s, M, sh->v[n], cc);
                for (int q = 0; q < 3; q++) d[q] = cc[q] - b[q];
                norm3(d);
                const float *o = sh->v[O->to], *p = sh->v[n];
                float apart = fminf(sqrtf((o[0] - p[0]) * (o[0] - p[0]) + (o[1] - p[1]) * (o[1] - p[1]) + (o[2] - p[2]) * (o[2] - p[2])) / CIRCUM, 1.2f);
                float score = dot3(d, dir) + .7f * cc[2] + .5f * apart + (s->hover > .3f ? 1.6f * s->hover * dot3(d, want) : 0) + .45f * mrand(s);
                if (score > sc) { sc = score; best = n; }
            }
            if (best < 0) best = sh->vn[F->to][(int)(mrand(s) * sh->nadj[F->to]) % sh->nadj[F->to]];
            F->from = F->to; F->to = best; F->at = now;
        }
        float u = io3(clampf((float)(now - F->at) / glide, 0, 1));
        for (int k = 0; k < sh->nvt[F->to]; k++) { int g = m->grp[sh->vt[F->to][k]]; lift[g] = fmaxf(lift[g], .15f * u); }
        for (int k = 0; k < sh->nvt[F->from]; k++) { int g = m->grp[sh->vt[F->from][k]]; lift[g] = fmaxf(lift[g], .15f * (1 - u) + .1f * u); }
        for (int j = 0; j < F->ntrail; j++) {
            float a = .1f * expf(-(float)(now - F->trail_at[j]) * 2.4f);
            for (int k = 0; k < sh->nvt[F->trail[j]]; k++) { int g = m->grp[sh->vt[F->trail[j]][k]]; lift[g] = fmaxf(lift[g], a); }
        }
        m->glow[F->to] = fmaxf(m->glow[F->to], .45f * u);
    }
    for (int g = 0; g < m->ng; g++) if (lift[g] > 0) c->w->grp[g] += lift[g] / wof(m, g);
    c->w->rate = .8f;
}

/* Beacon: a lighthouse beam of lifted patches sweeps round; each time it faces you it flares and sends a ring */
static void r_beacon(Ctx *c) {
    Motion *m = c->m; BeaconRS *B = &m->rs.beacon;
    const float P = 1.6f;
    float ax[3] = {.1f, 1, .05f}, X[3] = {1, 0, 0}, b0[3], beam[3], R[9], f[3];
    norm3(ax); cross3(b0, ax, X); norm3(b0);
    m3_rot(R, ax, c->e / P * TAU); m3_app(beam, R, b0);
    toward(c->M, f);
    float face = fmaxf(0, dot3(beam, f)), fl = powf(face, 6);
    for (int g = 0; g < m->ng; g++) { float d = dot3(m->gn[g], beam); if (d > .55f) c->w->grp[g] += .16f * (d - .55f) / .45f * (1 + fl) / wof(m, g); }
    c->w->bright *= 1 + 1.1f * fl;
    int kk = (int)floorf(c->e / P + .25f) + 1;
    if (kk != B->last && face > .97f) { B->last = kk; ring(m, c->now, 1); }
    c->w->rate = 0;
}

/* Knock: knock, knock: two taps toward you (the front patches flatten and pop back), a ring each, a pause */
static void r_knock(Ctx *c) {
    Motion *m = c->m; KnockRS *K = &m->rs.knock;
    const float P = 2.1f;
    float u = fmodf(c->e, P), lunge = 0, f[3];
    int k = (int)(c->e / P) + 1;
    if (K->k != k) { K->k = k; K->hits = 0; }
    for (int i = 0; i < 2; i++) {
        float d = u - (.35f + i * .26f);
        if (d > 0 && d < .26f) lunge = fmaxf(lunge, d < .07f ? out3(d / .07f) : 1 - io3((d - .07f) / .19f));
        if (d > .07f && K->hits == i) { K->hits++; ring(m, c->now, .8f); }
    }
    c->w->scale *= 1 + .07f * lunge;
    toward(c->M, f);
    for (int g = 0; g < m->ng; g++) { float d = dot3(m->gn[g], f); if (d > .6f) c->w->grp[g] += (-.07f * lunge + .05f * bump((u - .5f) / .4f) * (d - .6f) / .4f) / wof(m, g); }
    for (int i = 0; i < c->sh->nv; i++) { float v[3]; unit_v(c->sh, i, v); if (dot3(v, f) > .85f) m->glow[i] = fmaxf(m->glow[i], lunge * .9f); }
    c->w->bright *= 1 + .4f * lunge;
    set_rate(c, .05f);
}

/* Unlock: charges, bursts open with spikes and a ring, turns a full circle, the patches lock back click by click */
static void r_unlock(Ctx *c) {
    Session *s = c->s; Motion *m = c->m; const Shape *sh = c->sh; UnlockRS *U = &m->rs.unlock;
    double now = c->now;
    if (!sh->nt) return;
    if (U->flags && now - U->at > 3.6) { U->flags = 0; recut(s, sh, 1, 6); memset(U->clicked, 0, sizeof U->clicked); } /* given to a longer state: again */
    if (!(U->flags & 1)) {
        U->flags = 1; U->at = now;
        float ax[3], u0[3], w0[3];
        rand_dir(s, ax); square_to(u0, ax); cross3(w0, ax, u0);
        float a[MAX_T]; int idx[MAX_T];
        for (int g = 0; g < m->ng; g++) { a[g] = atan2f(dot3(m->gn[g], w0), dot3(m->gn[g], u0)); idx[g] = g; }
        for (int i = 1; i < m->ng; i++) for (int j = i; j > 0 && a[idx[j]] < a[idx[j - 1]]; j--) { int t = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = t; }
        for (int i = 0; i < m->ng; i++) U->rank[idx[i]] = (float)i / (float)(m->ng > 1 ? m->ng : 1);
        for (int i = 0; i < 4; i++) spike_random(s, sh, c->M, -rnd(s, .3f, .45f), .12f, NULL, .22f, .1f, now); /* charging */
    }
    float e = (float)(now - U->at);
    if (e < .3f) { c->w->scale *= 1 - .06f * io3(e / .3f); c->w->rate = 0; }
    else c->w->rate = .15f;
    if (e >= .3f && !(U->flags & 2)) { /* burst */
        U->flags |= 2;
        ring(m, now, 1);
        flash(m, sh->nv, 1);
        for (int i = 0; i < 5; i++) spike_pair(s, sh, c->M, rnd(s, .28f, .4f), rnd(s, .25f, .4f), rnd(s, .2f, .4f), NULL, .07f, .16f, now);
        float ax[3]; rand_dir(s, ax);
        turn(s, ax, TAU, .9f, 0, now); /* a full turn while open */
    }
    for (int g = 0; g < m->ng; g++) {
        float close = 1.3f + U->rank[g] * .9f;
        float a = e < .3f ? 0 : e < .55f ? outx((e - .3f) / .25f) : e < close ? 1 : fmaxf(0, 1 - in3((e - close) / .08f));
        c->w->grp[g] += .2f * a / fmaxf(.45f, m->gw[g]);
        if (e >= close + .08f && !U->clicked[g]) { U->clicked[g] = 1; glow_patch(m, sh, g, .9f); } /* click: this patch is home */
    }
    if (e > 2.32f && !(U->flags & 4)) { U->flags |= 4; ring(m, now, .7f); flash(m, sh->nv, .8f); s->spin_y += 3; }
}

/* Bloom: anticipation, release, joy, a satisfying close. It curls in and gathers light; bursts open ring by ring
 * from the front, petals glinting as they part; turns once in full bloom, breathing, pollen rising; folds back
 * from the outside in, click by click; and seals with a flash, a ring and a happy spin. */
static void r_bloom(Ctx *c) {
    Session *s = c->s; Motion *m = c->m; const Shape *sh = c->sh; const float *M = c->M; Want *w = c->w; BloomRS *B = &m->rs.bloom;
    const float P = 4.8f;
    float u = fmodf(c->e, P);
    int k = (int)(c->e / P) + 1;
    if (B->k != k) { /* the flower faces you */
        B->k = k; B->flags = 0;
        float f[3]; toward(M, f);
        for (int g = 0; g < m->ng; g++) B->d[g] = acosf(clampf(dot3(m->gn[g], f), -1, 1)) / PI_F;
        memset(B->marks, 0, sizeof B->marks);
    }
    set_rate(c, .12f);
    if (u < .55f) { float a = io3(u / .55f); w->scale *= 1 - .07f * a; w->bright *= 1 + .9f * a; for (int g = 0; g < m->ng; g++) w->grp[g] -= .03f * a / wof(m, g); } /* curls in, gathers light */
    if (u >= .55f && !(B->flags & 1)) {
        B->flags |= 1; ring(m, c->now, .8f); flash(m, sh->nv, .5f);
        float z[3] = {0, 0, 0}, Z[3] = {0, 0, 1};
        spark_at(s, M, z, .4f, 34, RGB255(190, 255, 215));
        for (int i = 0; i < 3; i++) spike_pair(s, sh, M, rnd(s, .3f, .4f), rnd(s, .12f, .22f), .25f, Z, .07f, .16f, c->now);
    }
    for (int g = 0; g < m->ng; g++) {
        float d = B->d[g], open_at = .55f + d * .75f, close_at = 3.1f + (1 - d) * .75f;
        float open = outx(clampf((u - open_at) / .28f, 0, 1)), close = in3(clampf((u - close_at) / .12f, 0, 1));
        float breathe = u > 1.4f && u < 3.1f ? .04f * sinf((u - 1.4f) * 5 - d * 7) : 0;
        float petal = .07f + .21f * sinf(PI_F * fminf(1, d * 1.7f)); /* widest about 60° from the front, like petals opening toward you */
        w->grp[g] += (petal + breathe) * open * (1 - close) / wof(m, g);
        if (u >= open_at + .06f && !(B->marks[g] & 1)) { B->marks[g] |= 1; glow_patch(m, sh, g, .7f); } /* a glint as each petal parts */
        if (u >= close_at + .12f && !(B->marks[g] & 2)) { /* click */
            B->marks[g] |= 2; glow_patch(m, sh, g, 1);
            if (m->gw[g] > 0) { float p[3], v[3]; for (int q = 0; q < 3; q++) { p[q] = m->gn[g][q] * 1.05f; v[q] = m->gn[g][q] * .4f; } spark(s, M, p, v, .45f, 3, 0, 1.1f, SK_MOTE, 0, RGB255(200, 255, 220)); }
        }
    }
    if (u >= 1.4f && !(B->flags & 2)) { float Z[3] = {0, 0, 1}; B->flags |= 2; turn_view(s, Z, TAU, 1.6f, c->now); } /* a full turn in full bloom, facing you */
    if (u > 1 && u < 3.1f) {
        w->bright *= 1 + .25f * fmaxf(0, sinf((u - 1) * 6)); /* the heart beats */
        if (m->ng && mrand(s) < 36 * c->dt) { /* pollen */
            int g = (int)(mrand(s) * m->ng) % m->ng;
            if (B->d[g] > .35f) {
                float p[3], v[3], vm[3] = {rnd(s, -.1f, .1f), -.15f, rnd(s, -.1f, .1f)};
                for (int q = 0; q < 3; q++) p[q] = m->gn[g][q] * 1.3f;
                m3_tapp(v, M, vm); for (int q = 0; q < 3; q++) v[q] += m->gn[g][q] * .2f;
                spark(s, M, p, v, rnd(s, 1.2f, 2.2f), .6f, -.3f, rnd(s, 1, 2), SK_MOTE, 0, RGB255(190, 255, 215));
            }
        }
    }
    if (u >= 3.95f && !(B->flags & 4)) {
        B->flags |= 4; ring(m, c->now, 1); flash(m, sh->nv, .9f); s->spin_y += 4;
        float z[3] = {0, 0, 0};
        spark_at(s, M, z, .45f, 40, RGB255(190, 255, 215));
        for (int i = 0; i < 22; i++) { float d[3], p[3]; rand_dir(s, d); float sp = rnd(s, .8f, 1.6f); for (int q = 0; q < 3; q++) { p[q] = d[q] * 1.05f; d[q] *= sp; } spark(s, M, p, d, rnd(s, .4f, .8f), 2, 0, rnd(s, .9f, 1.6f), SK_STREAK, 1, RGB255(150, 255, 190)); }
    }
    if (B->flags & 4) w->bright *= 1 + 1.3f * expf(-(u - 3.95f) * 6);
}

/* Zip: tension, a run, a lock, a cheer. The patches stand open in a ripple, humming; a zipper head runs down a
 * spiral, faster and faster, snapping each patch shut; at the bottom a spike punches out and it locks with a 72°
 * snap and a ring; a light races back up the seam and sparks fountain from the top while the core beats twice. */
static void r_zip(Ctx *c) {
    Session *s = c->s; Motion *m = c->m; const Shape *sh = c->sh; const float *M = c->M; Want *w = c->w; ZipRS *Z = &m->rs.zip;
    const float P = 4.2f;
    float u = fmodf(c->e, P), *ax = Z->ax;
    int k = (int)(c->e / P) + 1;
    if (Z->k != k) {
        Z->k = k; Z->flags = 0; m->ntrail = 0;
        set3(ax, rnd(s, -.2f, .2f), 1, rnd(s, -.2f, .2f)); norm3(ax);
        for (int g = 0; g < m->ng; g++) Z->shut_at[g] = -1; /* when each patch was snapped shut */
    }
    set_rate(c, .08f);
    float z = powf(clampf((u - .5f) / 2, 0, 1), 1.6f), head = 1 - 2 * z, X[3] = {1, 0, 0}, b1[3], b2[3], hp[3];
    cross3(b1, ax, X); norm3(b1); cross3(b2, ax, b1);
    float ang = z * PI_F * 7, rr = sqrtf(fmaxf(0, 1 - head * head));
    for (int q = 0; q < 3; q++) hp[q] = ax[q] * head + b1[q] * cosf(ang) * rr + b2[q] * sinf(ang) * rr;
    for (int g = 0; g < m->ng; g++) {
        float h = dot3(m->gn[g], ax), ready = out3(clampf((u - (1 - h) * .2f) / .25f, 0, 1)); /* they stand open in a ripple from the top */
        if (Z->shut_at[g] < 0 && u > .5f && h > head + .05f) { /* the head passes: snap */
            Z->shut_at[g] = u;
            glow_patch(m, sh, g, 1);
            if (m->gw[g] > 0 && mrand(s) < .6f) {
                float tg[3], p[3], v[3]; cross3(tg, ax, m->gn[g]); norm3(tg);
                float sp = rnd(s, .4f, .9f);
                for (int q = 0; q < 3; q++) { p[q] = m->gn[g][q] * 1.04f; v[q] = tg[q] * sp + m->gn[g][q] * .25f; }
                spark(s, M, p, v, .3f, 3, 0, .9f, SK_STREAK, 1, RGB255(170, 255, 205));
            }
        }
        float shut = Z->shut_at[g] < 0 ? 0 : in3(clampf((u - Z->shut_at[g]) / .06f, 0, 1));
        w->grp[g] += (.12f + .012f * sinf(u * 40 + g)) * ready * (1 - shut) / wof(m, g); /* humming while open */
    }
    m->trail_col = RGB255(200, 255, 222);
    if (u > .5f && u < 2.5f) { float p[3] = {hp[0] * 1.03f, hp[1] * 1.03f, hp[2] * 1.03f}; trail_push(m, p); } else trail_pop(m);
    m->trail_on = 1;
    if (u > .5f && u < 2.5f) for (int i = 0; i < sh->nv; i++) {
        float d[3] = {sh->v[i][0] / CIRCUM - hp[0], sh->v[i][1] / CIRCUM - hp[1], sh->v[i][2] / CIRCUM - hp[2]}, l = len3(d);
        if (l < .4f) m->glow[i] = fmaxf(m->glow[i], 1 - l / .4f);
    }
    if (u >= 2.5f && !(Z->flags & 1)) { /* lock */
        Z->flags |= 1; ring(m, c->now, 1); flash(m, sh->nv, .7f);
        int bot = 0; float bd = 9;
        for (int i = 0; i < sh->nv; i++) { float v[3]; unit_v(sh, i, v); float d = dot3(v, ax); if (d < bd) { bd = d; bot = i; } }
        spike_at(s, bot, .4f, .12f, .07f, .16f, c->now);
        turn_model(s, ax, STEP5, .3f, c->now);
        float p[3] = {-ax[0] * 1.05f, -ax[1] * 1.05f, -ax[2] * 1.05f};
        spark_at(s, M, p, .4f, 36, RGB255(200, 255, 222));
    }
    if (u >= 2.8f && !(Z->flags & 2)) { /* the cheer: a fountain from the top */
        Z->flags |= 2;
        float p[3] = {ax[0] * 1.05f, ax[1] * 1.05f, ax[2] * 1.05f};
        for (int i = 0; i < 26; i++) {
            float v[3], sp = rnd(s, .9f, 1.6f);
            for (int q = 0; q < 3; q++) v[q] = ax[q] * sp + rnd(s, -.5f, .5f);
            spark(s, M, p, v, rnd(s, .6f, 1.1f), 1.2f, 1.5f, rnd(s, 1, 1.9f), SK_STREAK, 1, RGB255(150, 255, 195));
        }
        spark_at(s, M, p, .35f, 34, RGB255(210, 255, 230));
    }
    if (u > 2.6f && u < 3.3f) { /* the light races back up the seam */
        float hh = -1 + 2 * (u - 2.6f) / .7f;
        for (int i = 0; i < sh->nv; i++) { float v[3]; unit_v(sh, i, v); float d = fabsf(dot3(v, ax) - hh); if (d < .18f) m->glow[i] = fmaxf(m->glow[i], 1 - d / .18f); }
    }
    w->bright *= 1 + (u < .5f ? .06f * sinf(u * 30) : 0) + .6f * bump((u - 2.85f) / .25f) + .45f * bump((u - 3.2f) / .25f);
}

/* Dormant: drawn in a little and barely turning; the only sign of life a faint scan every 6 s */
static void r_dormant(Ctx *c) {
    c->w->scale = .93f + .07f * c->s->hover;
    float u = (float)fmod(c->tt, 6.0) / 2.2f;
    if (u < 1) { c->m->scan_h = -1.1f + 2.2f * io3(u); c->m->scan_a = .5f; }
}

/* Black hole: a big bang, then the fall. For 2.6 s the core gathers power (a quickening heartbeat, matter and
 * light drawn in, the shell squeezed and trembling, crackling, an implosion), then bursts: the patches are blown
 * apart, small ones fastest, each tumbling; at once the pull takes over and they fall back on a tightening spiral,
 * twisting harder and stretching thin, and are swallowed at the edge of the void with a violet flash. Then only
 * the void and its dust disc. Pointing at it wakes it: the pieces come home. */
static void hole_enter(Ctx *c) {
    Session *s = c->s; Motion *m = c->m; const Shape *sh = c->sh; HoleRS *H = &m->rs.hole;
    float SW[3] = {rnd(s, -.3f, .3f), 1, rnd(s, -.3f, .3f)}, X[3] = {1, 0, 0};
    norm3(SW);
    int cnt[MAX_T]; memset(cnt, 0, sizeof cnt);
    for (int t = 0; t < sh->nt; t++) {
        Piece *pc = &H->piece[m->grp[t]];
        for (int k = 0; k < 3; k++) for (int q = 0; q < 3; q++) pc->c[q] += sh->v[sh->t[t][k]][q] / CIRCUM / 3;
        cnt[m->grp[t]]++;
    }
    for (int g = 0; g < m->ng; g++) {
        Piece *pc = &H->piece[g];
        for (int q = 0; q < 3; q++) pc->c[q] /= cnt[g] ? cnt[g] : 1;
        memcpy(pc->n, pc->c, sizeof pc->n); norm3(pc->n);
        cross3(pc->tn, SW, pc->n);
        if (len3(pc->tn) > .05f) norm3(pc->tn); else { cross3(pc->tn, X, pc->n); norm3(pc->tn); }
        pc->r = len3(pc->c);
        pc->vr = rnd(s, 2, 3) * (1.18f - .06f * cnt[g]) * .55f; /* small ones fly fastest (sized so they stay in the tile) */
        pc->L = rnd(s, .25f, .7f); rand_dir(s, pc->ax);
        pc->spin = rnd(s, 2, 7) * (mrand(s) < .5f ? -1 : 1) / sqrtf((float)(cnt[g] ? cnt[g] : 1));
        pc->size = cnt[g];
    }
    for (int i = 0; i < NDUST; i++) H->dust[i] = (Dust){1.25f + powf(mrand(s), 1.6f) * 1.9f, mrand(s) * TAU, rnd(s, .6f, 1.6f)};
    for (int i = 0; i < NSTAR; i++) { H->star[i][0] = rnd(s, -1.6f, 1.6f); H->star[i][1] = rnd(s, -1.6f, 1.6f); H->star[i][2] = rnd(s, .6f, 1.4f); H->star[i][3] = rnd(s, 0, TAU); }
    H->moving = 1;
}
static void r_hole(Ctx *c) {
    Session *s = c->s; Motion *m = c->m; const Shape *sh = c->sh; const float *M = c->M; Want *w = c->w; HoleRS *H = &m->rs.hole;
    const float BANG = 2.6f;
    float dt = fminf(.05f, c->dt), e = c->e;
    H->wake += ((s->hover > .3f ? 1.0f : 0.0f) - H->wake) * (1 - expf(-dt * 4));
    for (int i = 0; i < NDUST; i++) { /* the dust disc turns, and slowly falls in */
        Dust *d = &H->dust[i];
        d->a += dt * 1.15f / powf(d->r, 1.5f); d->r -= dt * .05f / d->r;
        if (d->r < 1.05f) { d->r = 2.9f + mrand(s) * .3f; d->a = mrand(s) * TAU; }
    }
    if (e < BANG) { /* gathering power: a heartbeat that quickens, light and matter drawn in, the shell squeezed tighter */
        float u = e / BANG, uu = u * u, ph = TAU * BANG * (1.2f * u + 3.4f * uu);
        float beat = powf(fmaxf(0, sinf(ph)), 6), last = clampf((u - .93f) / .07f, 0, 1);
        w->scale *= (1 - .12f * io3(u) - .035f * beat * (.3f + u)) * (1 - .14f * in3(last));
        w->bright *= 1 + 3 * uu + .8f * beat * u;
        set_rate(c, .2f + 1.4f * uu);
        for (int i = 0; i < sh->nv; i++) w->push[i] = -(i % 3 ? .03f : .09f) * (.3f + .7f * u) * (1 + .6f * beat);
        float shk = .028f * uu * (1 + beat);
        for (int t = 0; t < sh->nt; t++) { float j[3] = {rnd(s, -1, 1), rnd(s, -1, 1), rnd(s, -1, 1)}; tt_set(m, t, j, shk); }
        for (int k = 0; k < 2; k++) if (mrand(s) < (6 + 70 * uu) * dt) { /* matter and light drawn in from far out */
            float d[3], p[3], v[3]; rand_dir(s, d);
            float r0 = rnd(s, 1.25f, 1.6f), sp = rnd(s, 1, 1.6f) * (1 + u);
            for (int q = 0; q < 3; q++) { p[q] = d[q] * r0; v[q] = -d[q] * sp; }
            spark(s, M, p, v, (r0 - .3f) / sp, 0, 0, rnd(s, .7f, 1.4f), SK_STREAK, 0, mrand(s) < .5f ? RGB255(205, 170, 255) : RGB255(255, 225, 250));
        }
        if (mrand(s) < (2 + 25 * uu) * dt) m->glow[(int)(mrand(s) * sh->nv) % sh->nv] = 1; /* crackling */
        if (beat > .95f && u > .3f && !H->beat_ring) { H->beat_ring = 1; ring(m, c->now, .25f + .4f * u); } else if (beat < .5f) H->beat_ring = 0;
        return;
    }
    if (!H->banged) { /* the bang */
        H->banged = 1; ring(m, c->now, 1); flash(m, sh->nv, 1);
        float z[3] = {0, 0, 0};
        spark_at(s, M, z, .7f, 48, RGB255(215, 185, 255)); /* (kept inside the tile: a bigger flare shows its square edge) */
        spark_at(s, M, z, .25f, 26, RGB255(255, 255, 255));
        for (int i = 0; i < 46; i++) { float d[3], p[3]; rand_dir(s, d); float sp = rnd(s, 1.5f, 3.3f); for (int q = 0; q < 3; q++) { p[q] = d[q] * .3f; d[q] *= sp; } spark(s, M, p, d, rnd(s, .35f, .8f), 2.2f, 0, rnd(s, 1, 2), SK_STREAK, 1, RGB255(200, 150, 255)); }
    }
    float eb = e - BANG, wake = H->wake;
    m->hole = clampf(eb / .45f, 0, 1) * (1 - wake);
    w->bright *= 1 + 2.5f * expf(-eb * 7);
    w->rate = 0;
    const float GM = 8;
    const int sub = 4;
    float h = dt / sub;
    int base = m->ntm, alive = 0;
    m->ta_on = 1;
    for (int g = 0; g < m->ng; g++) {
        Piece *pc = &H->piece[g];
        if (!pc->gone) for (int k = 0; k < sub; k++) {
            pc->vr += -GM / (pc->r * pc->r) * h; pc->r = fmaxf(.05f, pc->r + pc->vr * h);
            pc->phi += fminf(30, pc->L / (pc->r * pc->r)) * h; /* the spiral tightens as it falls */
            pc->tw += (pc->spin + (pc->vr < 0 ? 4 / fmaxf(.25f, pc->r) : 0)) * h; /* and the twist grows near the hole */
        }
        float pos[3], rd[3];
        for (int q = 0; q < 3; q++) pos[q] = (pc->n[q] * cosf(pc->phi) + pc->tn[q] * sinf(pc->phi)) * pc->r;
        memcpy(rd, pos, sizeof rd); norm3(rd);
        int falling = pc->vr < 0;
        float stretch = falling ? 3 * clampf((.85f - pc->r) / .6f, 0, 1) : 0, sc = falling ? .3f + .7f * clampf((pc->r - .15f) / .7f, 0, 1) : 1;
        float Sx[9], Rt[9], A[9];
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) Sx[i * 3 + j] = (i == j) + stretch * rd[i] * rd[j];
        m3_rot(Rt, pc->ax, pc->tw); m3_scale(Rt, sc);
        m3_mul(A, Sx, Rt);
        if (falling && pc->r < .16f && !pc->gone) { /* swallowed at the edge */
            pc->gone = 1;
            spark_at(s, M, pos, .35f, 8 + 4.0f * pc->size, RGB255(195, 145, 255));
            for (int k = 0; k < 4; k++) { float q2[3], v[3]; for (int q = 0; q < 3; q++) q2[q] = pos[q] + rnd(s, -.2f, .2f); float sp = rnd(s, 3, 5); for (int q = 0; q < 3; q++) v[q] = -q2[q] * sp; spark(s, M, q2, v, .2f, 0, 0, 1, SK_STREAK, 0, RGB255(215, 180, 255)); }
        }
        alive += !pc->gone;
        float off[3], ac[3];
        m3_app(ac, A, pc->c);
        for (int q = 0; q < 3; q++) off[q] = pos[q] - ac[q];
        float al = pc->gone ? 0 : falling ? clampf((pc->r - .14f) / .2f, 0, 1) : 1;
        if (wake > .001f) { for (int i = 0; i < 9; i++) A[i] = A[i] * (1 - wake) + I3[i] * wake; for (int q = 0; q < 3; q++) off[q] *= 1 - wake; al = al * (1 - wake) + wake; }
        tm_add(m, A);
        for (int t = 0; t < sh->nt; t++) if (m->grp[t] == g) { m->tmi[t] = (unsigned char)(base + g + 1); tt_set(m, t, off, 1); m->talpha[t] = al; }
    }
    /* all swallowed: the void grows to 0.85 of the shell (back again as it wakes) */
    float gt = !alive && wake < .5f ? 1.0f : 0.0f;
    H->grow += (gt - H->grow) * (1 - expf(-dt * (gt > H->grow ? 1.1f : 4)));
    H->moving = alive || wake > .01f || fabsf(gt - H->grow) > .01f; /* only the void and its dust left: motion_busy lets it rest */
}

/* the panel's own states */
static void r_broadcast(Ctx *c) { /* the patches rise and fall with the voice, each at its own rate */
    Motion *m = c->m;
    for (int g = 0; g < m->ng; g++) if (m->gw[g] > 0) c->w->grp[g] += .15f * c->s->level * fabsf(sinf(g * 1.7f + (float)c->tt * (6 + 5 * hashf(g)))) / m->gw[g];
}
static void r_receiver(Ctx *c) { /* the patches facing you open with the microphone */
    Motion *m = c->m;
    c->w->rate = 0;
    for (int g = 0; g < m->ng; g++) {
        float d[3]; m3_app(d, c->M, m->gn[g]);
        if (d[2] > .35f && m->gw[g] > 0) c->w->grp[g] += (.04f + .1f * c->s->level * (.6f + .4f * sinf((float)c->tt * (4 + 3 * hashf(g)) + g)) + .06f * c->s->hover) * (d[2] - .35f) / .65f / m->gw[g];
    }
}
static void r_decoding(Ctx *c) { /* 36° steps while a scan reads up and down; the row it reads flips open */
    Session *s = c->s; Motion *m = c->m; DecodingRS *D = &m->rs.decoding;
    c->w->rate = 0;
    if (c->now >= D->next) { float Y[3] = {0, 1, 0}; turn(s, Y, TAU / 10, .22f, 0, c->now); D->next = c->now + .6; }
    float h = s->hover > .5f ? clampf(s->look_y_to, -1.05f, 1.05f) : sinf((float)c->tt * 2.6f) * 1.05f;
    m->scan_h = h; m->scan_a = .8f;
    for (int g = 0; g < m->ng; g++) { float d[3]; m3_app(d, c->M, m->gn[g]); if (d[2] > 0 && m->gw[g] > 0) { float k = d[1] - h; c->w->grp[g] += .1f * expf(-k * k / .03f) / m->gw[g]; } }
}
static void locked_enter(Ctx *c) { align_to_viewer(c->s, c->sh, corner_facing(c->s, c->sh, c->M), .8f, c->now); }
static void r_locked(Ctx *c) { /* square to you, drawn in, still */
    Motion *m = c->m;
    c->w->rate = 0; c->w->scale = .8f + .12f * c->s->hover;
    for (int g = 0; g < m->ng; g++) if (m->gw[g] > 0) c->w->grp[g] += .05f * c->s->hover / m->gw[g];
}
static void r_crazy(Ctx *c) { /* fits of going crazy; pointed at, it jerks away */
    Session *s = c->s; Motion *m = c->m; CrazyRS *C = &m->rs.crazy;
    if (c->now - m->last_crazy > 2.6) crazy(s, c->sh, c->M, 1.3f, c->now);
    if (s->hover > .5f && !C->flinched) { float a[3] = {s->look_y_to, -s->look_x_to, 0}; norm3(a); C->flinched = 1; jerk(s, a, -.3f, c->now); }
    if (s->hover < .2f) C->flinched = 0;
}

/* every routine: name, the state it was made for, how long it keeps DONE on screen, fine patches, run, enter, what it does */
const Routine ROUTINES[RT_N] = {
    [RT_DRIFT] = {"Drift", -1, 0, 0, r_drift, NULL, "A slow, plain turn; the actions do the rest."},
    [RT_ORACLE] = {"Oracle", ST_IDLE, 0, 0, r_oracle, NULL, "Exact and calm; now and then a line of light climbs it. Pointed at, it stirs with calm spikes."},
    [RT_PUZZLE] = {"Puzzle", ST_THINK, 0, 0, r_puzzle, NULL, "One layer rises in separate pieces; both turn like a cube being solved: two quick turns, a halt, three times."},
    [RT_SMOOTH] = {"Smooth puzzle", ST_THINK, 0, 0, r_smooth, NULL, "The puzzle with eased turns that flow into each other; only the halt is still."},
    [RT_LONG] = {"Long spin", ST_THINK, 0, 0, r_long, NULL, "One layer spins three full turns, then the other spins back, while the shell turns slowly."},
    [RT_BLADES] = {"Blades", ST_THINK, 0, 0, r_blades, NULL, "Five triangles leave the shell as long thin blades, hover in a fan behind the core, sweep across the front in a V like a skein of geese, hover on the far side, sweep back and go home."},
    [RT_ENGINE] = {"Engine", ST_WORK, 0, 0, r_engine, NULL, "A turbine belt runs round the middle while the patches on the caps fire in order like pistons."},
    [RT_GYRO] = {"Gyro", ST_WORK, 0, 0, r_gyro, axes_enter, "Two belts at right angles run against each other like the rings of a gyroscope."},
    [RT_HELIX] = {"Helix", ST_WORK, 0, 0, r_helix, axes_enter, "The shell turns while a spiral of lifted patches climbs it, like a screw being driven."},
    [RT_TYPEWRITER] = {"Typewriter", ST_WORK, 0, 0, r_typewriter, NULL, "Row by row the patches pop out like keys; at the end of a row, a ring and a carriage return."},
    [RT_FORGE] = {"Forge", ST_WORK, 0, 0, r_forge, NULL, "One to three pieces rise slowly, glowing hotter, and come down hard: sparks, embers, a ring."},
    [RT_LOOM] = {"Loom", ST_WORK, 0, 0, r_loom, NULL, "Two belts cross like warp and weft; a bright shuttle runs along one, the other clicks a step."},
    [RT_CONVEYOR] = {"Conveyor", ST_WORK, 0, 0, r_conveyor, conveyor_enter, "The belt floats up as two rings that take turns stepping one slot on, each with a click."},
    [RT_COMPRESSOR] = {"Compressor", ST_WORK, 0, 0, r_compressor, NULL, "Squeezed flat in three hard clicks, then it vents in one clean stroke and a ring of exhaust."},
    [RT_SWIM] = {"Swimmers", ST_WAIT, 0, 1, r_swim, swim_enter, "Two fish of fine patches swim just under the surface, leaving a wake."},
    [RT_BEACON] = {"Beacon", ST_WAIT, 0, 0, r_beacon, NULL, "A lighthouse beam of lifted patches sweeps round; each time it faces you it flares."},
    [RT_KNOCK] = {"Knock", ST_WAIT, 0, 0, r_knock, NULL, "Knock, knock: it taps twice toward you, a ring each time, then a pause."},
    [RT_UNLOCK] = {"Unlock", ST_CHEER, 0, 0, r_unlock, NULL, "Charges, bursts open with spikes and a ring, turns a full circle, then locks back click by click."},
    [RT_BLOOM] = {"Bloom", ST_CHEER, 4.6f, 0, r_bloom, NULL, "Curls in, bursts open ring by ring like a flower, turns once in full bloom, folds back and seals."},
    [RT_ZIP] = {"Zip", ST_CHEER, 3.7f, 0, r_zip, NULL, "A zipper head runs down a spiral snapping each patch shut, locks with a snap, sparks fountain."},
    [RT_DORMANT] = {"Dormant", ST_SLEEP, 0, 0, r_dormant, NULL, "Drawn in slightly and barely turning; a faint scan every 6 s."},
    [RT_HOLE] = {"Black hole", ST_SLEEP, 0, 0, r_hole, hole_enter, "Gathers power, bursts apart and falls back into a void with a violet dust disc. Asleep only."},
    [RT_BROADCAST] = {"Broadcast", ST_SPEAK, 0, 0, r_broadcast, NULL, "The patches rise and fall with the voice, like the bars of an equaliser."},
    [RT_RECEIVER] = {"Receiver", ST_LISTEN, 0, 0, r_receiver, NULL, "Holds still; the patches facing you open with the microphone level."},
    [RT_DECODING] = {"Decoding", ST_TRANSCRIBE, 0, 0, r_decoding, NULL, "Steps 36 degrees at a time while a scan plane reads the rows."},
    [RT_LOCKED] = {"Locked", ST_STANDBY, 0, 0, r_locked, locked_enter, "Stops square to you, drawn in and still."},
    [RT_CRAZY] = {"Crazy", ST_ERROR, 0, 0, r_crazy, NULL, "Fits of going crazy: spikes out and in, jerks, flicking patches."},
};

static void routine_enter(Session *s, const Shape *sh, int st, double now) {
    Motion *m = &s->mo;
    /* which routine: one asked for (the Settings preview), else one of the state's at random (MOTION_MAP) */
    int rt = m->want_rt;
    m->want_rt = -1;
    if (rt < 0 || rt >= RT_N) {
        uint32_t ok = MOTION_MAP[st].rt;
        if (st != ST_SLEEP) ok &= ~(1u << RT_HOLE); /* the black hole is for sleep only */
        int n = 0;
        for (int i = 0; i < RT_N; i++) if (ok & (1u << i)) n++;
        rt = RT_DRIFT;
        if (n) {
            int pick = (int)(mrand(s) * n) % n;
            for (int i = 0; i < RT_N; i++) if ((ok & (1u << i)) && pick-- == 0) { rt = i; break; }
        }
    }
    m->routine = st; m->rt = rt; m->r_at = now;
    m->cheer_for = ROUTINES[rt].cheer;
    memset(&m->rs, 0, sizeof m->rs);
    m->ntrail = 0;
    recut(s, sh, 1, ROUTINES[rt].fine ? 2 : 6);
    if (ROUTINES[rt].enter) {
        float M[9]; view_matrix(s, M);
        Want w; memset(&w, 0, sizeof w);
        Ctx c = {s, m, sh, M, &w, st, now, now - s->created + s->phase, 0, 0};
        ROUTINES[rt].enter(&c);
    }
}

/* ---- the geometric actions (the soft ones are in session.c) ---------------------------------------
 * An action is a function of the frame (c), how far through it is (u, 0..1), how free it is to play (k: it fades
 * out when touched) and whether it has just started; it keeps its own state in c->m->ws. */
typedef void (*WhimFn)(Ctx *c, float u, float k, int start);
static void w_align(Ctx *c, float u, float k, int start) { (void)u; (void)k; if (start) align_to_viewer(c->s, c->sh, random_corner(c->s, c->sh), .55f, c->now); c->w->rate = 0; }
static void w_step(Ctx *c, float u, float k, int start) { /* two snap turns of 72°, like a combination lock */
    (void)u; (void)k;
    if (start) { int v = random_corner(c->s, c->sh); float a[3]; m3_app(a, c->m->q, c->sh->v[v]); turn(c->s, a, (mrand(c->s) < .5f ? -1 : 1) * STEP5, .38f, 0, c->now); later(c->s, c->now + .6, EV_STEP); memcpy(c->m->ws.step_ax, a, sizeof a); }
    c->w->rate = 0;
}
static void w_unfold(Ctx *c, float u, float k, int start) { /* a fresh cut, opened row by row from the top, closed in reverse */
    Motion *m = c->m; WhimState *W = &m->ws;
    (void)k;
    if (start) {
        recut(c->s, c->sh, 1, 6);
        float y[MAX_T];
        for (int g = 0; g < m->ng; g++) { float d[3]; m3_app(d, c->M, m->gn[g]); y[g] = d[1]; W->order[g] = (unsigned char)g; }
        for (int i = 1; i < m->ng; i++) for (int j = i; j > 0 && y[W->order[j]] < y[W->order[j - 1]]; j--) { unsigned char t = W->order[j]; W->order[j] = W->order[j - 1]; W->order[j - 1] = t; }
        W->n = m->ng;
    }
    for (int i = 0; i < W->n; i++) {
        int g = W->order[i]; float a = (float)i / W->n * .45f, b = 1 - (float)(W->n - i) / W->n * .45f;
        float v = u < a ? 0 : u < a + .08f ? outx((u - a) / .08f) : u < b ? 1 : u < b + .08f ? 1 - in3((u - b) / .08f) : 0;
        if (g < m->ng && m->gw[g] > 0) c->w->grp[g] += .15f * v / m->gw[g];
    }
}
static void w_scan(Ctx *c, float u, float k, int start) { (void)k; (void)start; c->m->scan_h = -1.1f + 2.2f * io3(u); c->m->scan_a = .7f; }
static void w_spike(Ctx *c, float u, float k, int start) {
    (void)u; (void)k;
    if (start) { spike_random(c->s, c->sh, c->M, rnd(c->s, .3f, .4f), rnd(c->s, .15f, .4f), NULL, .07f, .16f, c->now); if (mrand(c->s) < .5f) later(c->s, c->now + rnd(c->s, .15f, .45f), EV_PAIR); }
}
static void w_crazy(Ctx *c, float u, float k, int start) { (void)u; (void)k; if (start) crazy(c->s, c->sh, c->M, 1.4f, c->now); }
static void w_ripple(Ctx *c, float u, float k, int start) { /* a band of lifted patches runs over the sphere from one side to the other */
    Motion *m = c->m;
    if (start) rand_dir(c->s, m->ws.dir);
    float h = -1.25f + 2.5f * io3(u);
    for (int g = 0; g < m->ng; g++) {
        if (m->gw[g] <= 0) continue;
        float d = dot3(m->gn[g], m->ws.dir) - h;
        c->w->grp[g] += .11f * k * expf(-d * d / .04f) / m->gw[g];
    }
}
static void w_orbit(Ctx *c, float u, float k, int start) { /* one patch lifts off, flies once round a great circle and docks in its own place */
    Motion *m = c->m; WhimState *W = &m->ws;
    (void)k;
    if (start) {
        int ok[MAX_T], n = 0;
        for (int g = 0; g < m->ng; g++) if (m->gw[g] > 0) ok[n++] = g;
        W->g = n ? ok[(int)(mrand(c->s) * n) % n] : -1;
        if (W->g >= 0) { square_to(W->dir, m->gn[W->g]); W->v = mrand(c->s) < .5f ? 1 : -1; } /* square to the patch: a great circle through it */
    }
    if (W->g < 0 || W->g >= m->ng) return;
    /* up first, round only once it is fully out, down after it is home: it never cuts through its neighbours */
    float a = W->v * TAU * io3(clampf((u - .22f) / .56f, 0, 1)), lit = .35f * hold(u, .2f), R[9];
    c->w->grp[W->g] += .3f * hold(u, .18f) / m->gw[W->g];
    m3_rot(R, W->dir, a);
    int i = tm_add(m, R); /* its own matrix: the layer axis stays the routine's */
    for (int t = 0; t < c->sh->nt; t++) if (m->grp[t] == W->g) { m->tmi[t] = (unsigned char)i; for (int q = 0; q < 3; q++) m->glow[c->sh->t[t][q]] = fmaxf(m->glow[c->sh->t[t][q]], lit); }
}
static void w_petal(Ctx *c, float u, float k, int start) { /* the patches round the node facing you open ring by ring, like a flower, and close */
    Motion *m = c->m; WhimState *W = &m->ws; const Shape *sh = c->sh;
    if (start) { /* W->order: each patch's node steps from the node W->v */
        W->v = corner_facing(c->s, sh, c->M);
        unsigned char vd[MAX_V]; int queue[MAX_V], qh = 0, qt = 0;
        memset(vd, 255, sizeof vd);
        vd[W->v] = 0; queue[qt++] = W->v;
        while (qh < qt) { int v = queue[qh++]; for (int i = 0; i < sh->nadj[v]; i++) { int n = sh->vn[v][i]; if (vd[n] == 255) { vd[n] = (unsigned char)(vd[v] + 1); queue[qt++] = n; } } }
        memset(W->order, 255, sizeof W->order);
        for (int t = 0; t < sh->nt; t++) for (int q = 0; q < 3; q++) { int g = m->grp[t]; if (vd[sh->t[t][q]] < W->order[g]) W->order[g] = vd[sh->t[t][q]]; }
    }
    for (int g = 0; g < m->ng; g++) {
        int d = W->order[g];
        if (d > 2 || m->gw[g] <= 0) continue;
        float e = clampf((u - d * .08f) / .8f, 0, 1);
        c->w->grp[g] += (.16f - .04f * d) * k * hold(e, .3f) / m->gw[g];
    }
    if (W->v < sh->nv) m->glow[W->v] = fmaxf(m->glow[W->v], .6f * k * hold(u, .3f));
}
static void w_constellation(Ctx *c, float u, float k, int start) { /* a short chain of neighbouring nodes lights up one by one, then fades */
    Motion *m = c->m; WhimState *W = &m->ws; const Shape *sh = c->sh;
    if (start) {
        int v = (int)(mrand(c->s) * sh->nv) % sh->nv;
        W->path[0] = (unsigned char)v; W->n = 1;
        while (W->n < 6) {
            int nx[MAX_DEG], nn = 0;
            for (int i = 0; i < sh->nadj[v]; i++) {
                int cand = sh->vn[v][i], used = 0;
                for (int j = 0; j < W->n; j++) if (W->path[j] == cand) used = 1;
                if (!used) nx[nn++] = cand;
            }
            if (!nn) break;
            v = nx[(int)(mrand(c->s) * nn) % nn];
            W->path[W->n++] = (unsigned char)v;
        }
    }
    for (int i = 0; i < W->n && i < 6; i++) { float e = u - i * .1f; if (e > 0 && e < .55f && W->path[i] < sh->nv) m->glow[W->path[i]] = fmaxf(m->glow[W->path[i]], .95f * k); }
}
/* Shatter: the shell breaks into a few big shards (each a patch of neighbouring triangles) that burst apart, drift
 * and tumble, a couple of them flung well out, then fly back together and lock in place. The shards, their
 * directions, distances and spins come from a hash of the action's seed, so nothing needs keeping. While apart the
 * whole thing is drawn smaller, so even the far shards stay inside the tile. */
#define SHARDS 6
static float hash01(unsigned x) { x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16; return (x & 0xffffff) / 16777216.0f; }
static void w_shatter(Ctx *c, float u, float k, int start) {
    (void)k;
    Session *s = c->s; Motion *m = c->m; const Shape *sh = c->sh; const float *M = c->M;
    if (!sh->nt) return;
    if (start) { /* the blow: a ring, a flash of sparks */
        m->ws.g = (int)(mrand(s) * 1e6f);
        ring(m, c->now, .7f); flash(m, sh->nv, .9f);
        float z[3] = {0, 0, 0};
        spark_at(s, M, z, .5f, 32, RGB255(255, 236, 200));
    }
    const float BURST = .06f, BACK = .74f; /* fractions of the action */
    float far = u < BURST ? outx(u / BURST) : u < BACK ? 1 + .2f * (u - BURST) / (BACK - BURST) : 1.2f * (1 - io3((u - BACK) / (1 - BACK)));
    float spin = u < BACK ? u / BACK : 1 - io3((u - BACK) / (1 - BACK)); /* turned the most just before they come back */
    float apart = fminf(1, far), shrink = 1 - .35f * apart;                /* drawn smaller while apart */
    unsigned seed = (unsigned)m->ws.g;
    /* the shards: SHARDS seed directions, each triangle going with the nearest */
    float sd[SHARDS][3], sc[SHARDS][3] = {{0}}; int cnt[SHARDS] = {0}, of[MAX_T];
    for (int j = 0; j < SHARDS; j++) {
        float z = 2 * hash01(seed + 101u * j) - 1, a = TAU * hash01(seed + 101u * j + 7), r = sqrtf(fmaxf(0, 1 - z * z));
        set3(sd[j], r * cosf(a), r * sinf(a), z);
    }
    for (int t = 0; t < sh->nt; t++) {
        int best = 0; float bd = -9;
        for (int j = 0; j < SHARDS; j++) { float d = dot3(sh->tn[t], sd[j]); if (d > bd) { bd = d; best = j; } }
        of[t] = best; cnt[best]++;
        for (int v = 0; v < 3; v++) for (int q = 0; q < 3; q++) sc[best][q] += sh->v[sh->t[t][v]][q] / 3;
    }
    int ti[SHARDS] = {0}, nfar = 0;
    float tr[SHARDS][3];
    for (int j = 0; j < SHARDS; j++) {
        if (!cnt[j]) continue;
        float cen[3], dir[3], ax[3], R[9], rc[3];
        for (int q = 0; q < 3; q++) cen[q] = sc[j][q] / cnt[j];
        float h1 = hash01(seed + j * 4u + 1), h2 = hash01(seed + j * 4u + 2), h3 = hash01(seed + j * 4u + 3), h4 = hash01(seed * 7u + j);
        memcpy(dir, cen, sizeof dir); norm3(dir);
        float jit[3] = {h1 - .5f, h2 - .5f, h3 - .5f};
        for (int q = 0; q < 3; q++) dir[q] += .5f * jit[q];
        norm3(dir);
        set3(ax, h2 - .5f, h3 - .5f, h1 - .5f); norm3(ax);
        m3_rot(R, ax, (1.2f + 2.8f * h4) * (h1 < .5f ? -1 : 1) * spin);
        m3_scale(R, shrink);
        m3_app(rc, R, cen); /* turned about the shard's centre, which is drawn in a little: R p + (k c - R c) */
        int flung = h4 > .62f && nfar < 2; /* one or two go much further */
        nfar += flung;
        float dist = (flung ? .36f + .1f * h2 : .13f + .12f * h3) * far;
        for (int q = 0; q < 3; q++) tr[j][q] = dir[q] * dist + (cen[q] * shrink - rc[q]) / CIRCUM;
        ti[j] = tm_add(m, R);
    }
    for (int t = 0; t < sh->nt; t++) { m->tmi[t] = (unsigned char)ti[of[t]]; tt_set(m, t, tr[of[t]], 1); }
    c->w->rate = 0;
    c->w->bright *= 1 + .6f * expf(-u * 30);
}
static void w_tumble(Ctx *c, float u, float k, int start) { (void)u; (void)k; if (start) { float a[3]; rand_dir(c->s, a); turn(c->s, a, PI_F, 1.3f, 0, c->now); } c->w->rate = 0; }

/* the geometric actions, by W_ name (the soft ones have none: session.c plays them) */
static const WhimFn WHIM_RUN[W_N] = {
    [W_ALIGN] = w_align, [W_STEP] = w_step, [W_UNFOLD] = w_unfold, [W_SCAN] = w_scan, [W_SPIKE] = w_spike, [W_CRAZY] = w_crazy,
    [W_RIPPLE] = w_ripple, [W_ORBIT] = w_orbit, [W_PETAL] = w_petal, [W_CONSTELLATION] = w_constellation, [W_TUMBLE] = w_tumble,
    [W_SHATTER] = w_shatter,
};
static void whim(Ctx *c) {
    Session *s = c->s; Motion *m = c->m;
    if (s->whim <= W_NONE || s->whim >= W_N || !WHIM_RUN[s->whim]) return;
    if (!s->whim_forced && !(MOTION_MAP[c->shown].act & (1u << s->whim))) return; /* the state changed mid-action: its routine owns the shell now */
    int start = m->ws.started != s->whim_at;
    if (start) { memset(&m->ws, 0, sizeof m->ws); m->ws.started = s->whim_at; }
    WHIM_RUN[s->whim](c, clampf((float)(c->now - s->whim_at) / WHIMS[s->whim].dur, 0, 1), s->whim_k, start);
}

/* ---- every frame ------------------------------------------------------------------------------- */
void motion_tick(Session *s, const Shape *sh, int shown, double now, float dt) {
    Motion *m = &s->mo;
    if ((sh->nt && m->cut_for != sh->nt) || (!sh->nt && m->cut_for)) { /* a new shape (Settings > Shape): start clean */
        recut(s, sh, 1, 6);
        memset(m->sp, 0, sizeof m->sp); memset(m->push, 0, sizeof m->push); memset(m->off, 0, sizeof m->off); memset(m->ang, 0, sizeof m->ang);
        m->routine = -1;
    }
    if (!sh->nt) m->ng = 0;
    if (m->routine != shown) routine_enter(s, sh, shown, now);
    for (int i = 0; i < sh->nv; i++) m->glow[i] *= expf(-dt * 5);
    float M[9]; view_matrix(s, M);
    Want w; memset(&w, 0, sizeof w);
    w.scale = 1; w.rate = 1; w.bright = 1;
    m->ang_on = 0; m->scan_a = 0; m->inner = 0;
    /* what a routine sets afresh every frame */
    memset(m->tmi, 0, sizeof m->tmi);
    m->ntm = 0; m->tt_on = m->ta_on = m->trail_on = 0; m->squash = 0; m->hole = 0;
    memset(m->tt, 0, sizeof m->tt);

    /* particles and rings age */
    for (int i = 0; i < MAX_SPARK; i++) {
        Spark *q = &m->spark[i];
        if (q->life <= 0) continue;
        q->life -= dt;
        float k = expf(-dt * q->drag);
        for (int j = 0; j < 3; j++) { q->p[j] += q->v[j] * dt; q->v[j] *= k; }
        q->v[1] += q->g * dt;
    }
    for (int i = 0; i < MAX_RING; i++) if (m->ring[i].a > 0 && now - m->ring[i].at > 1.1) m->ring[i].a = 0;

    /* things asked for a moment ago */
    for (int i = 0; i < MAX_EV; i++) {
        MEvent *e = &m->ev[i];
        if (!e->kind || now < e->at) continue;
        if (e->kind == EV_PAIR) spike_pair(s, sh, M, rnd(s, .25f, .4f), rnd(s, .35f, .55f), rnd(s, .04f, .3f), NULL, .07f, .16f, now);
        else if (e->kind == EV_JERK) { float a[3]; rand_dir(s, a); turn(s, a, (mrand(s) < .5f ? -1 : 1) * rnd(s, .2f, .55f), .7f, 2, now); }
        else if (e->kind == EV_FLASH) flash(m, sh->nv, .7f);
        else if (e->kind == EV_STEP) turn(s, m->ws.step_ax, STEP5, .38f, 0, now);
        e->kind = 0;
    }

    /* the state's routine, then an action on top */
    Ctx c = {s, m, sh, M, &w, shown, now, now - s->created + s->phase, dt, (float)(now - m->r_at)};
    if (m->rt >= 0 && m->rt < RT_N) ROUTINES[m->rt].run(&c);
    whim(&c);

    /* the pointer: the patch under it lifts and lights (DONE), the nodes under it light (THINK) */
    if (s->hover > .05f && m->ng && (shown == ST_CHEER || shown == ST_THINK)) {
        float p[3] = {s->look_x_to, s->look_y_to, 0}, l = p[0] * p[0] + p[1] * p[1];
        p[2] = l < 1 ? sqrtf(1 - l) : 0;
        if (shown == ST_THINK) for (int v = 0; v < sh->nv; v++) { float d[3]; view_dir(s, M, sh->v[v], d); if (dot3(d, p) > .9f) m->glow[v] = fmaxf(m->glow[v], .55f * s->hover); }
        else { int best = -1; float bd = -2; for (int g = 0; g < m->ng; g++) { float d[3]; m3_app(d, M, m->gn[g]); if (m->gw[g] > 0 && dot3(d, p) > bd) { bd = dot3(d, p); best = g; } } if (best >= 0) w.grp[best] += .1f * s->hover / m->gw[best]; }
    }
    /* lock-on: pointed at for 2 s, it stops with a vertex toward you */
    m->lock += ((s->lock_to > .5f ? 1.0f : 0.0f) - m->lock) * (1 - expf(-dt * 6));
    if (s->lock_to > .5f && !m->locked) { m->locked = 1; if (shown != ST_WORK) align_to_viewer(s, sh, corner_facing(s, sh, M), .5f, now); }
    if (s->lock_to < .5f) m->locked = 0;
    if (m->locked && shown != ST_WORK) w.rate = 0;
    /* a rush at the core: the patches clamp shut, a spike toward where it came from, a jerk */
    if (s->defend_at != m->defend_seen && now - s->defend_at < .5) {
        m->defend_seen = s->defend_at;
        float ax[3] = {s->defend_y, -s->defend_x, 0}, pref[3] = {-s->defend_x, -s->defend_y, .4f};
        norm3(ax); norm3(pref);
        jerk(s, ax, .14f, now);
        spike_pair(s, sh, M, rnd(s, .3f, .4f), rnd(s, .25f, .4f), .12f, pref, .07f, .16f, now);
    }
    if (now - s->defend_at < .5) for (int g = 0; g < m->ng; g++) w.grp[g] = fminf(w.grp[g], 0) - .03f * (1 - (float)(now - s->defend_at) / .5f);
    /* a press: the patch under it goes in, and the press steps outward through the neighbours */
    if (s->press_at != m->press_seen && m->ng) {
        m->press_seen = s->press_at;
        float p[3] = {s->press_x, s->press_y, 0}, l = p[0] * p[0] + p[1] * p[1], bd = -2;
        p[2] = l < 1 ? sqrtf(1 - l) : 0;
        m->press_g = 0;
        for (int g = 0; g < m->ng; g++) { float d[3]; m3_app(d, M, m->gn[g]); if (dot3(d, p) > bd) { bd = dot3(d, p); m->press_g = g; } }
        patch_distances(s, sh, m->press_g);
        m->press_at = now;
    }
    if (now - m->press_at < 1.4) {
        float e = (float)(now - m->press_at);
        for (int g = 0; g < m->ng; g++) {
            int d = m->pdist[g];
            float u = (e - d * .09f) / .35f;
            if (d <= 4 && u > 0 && u < 1) w.grp[g] -= .07f * (1 - d * .18f) * (u < .25f ? outx(u / .25f) : 1 - io3((u - .25f) / .75f));
        }
    }
    /* carried: the patches on the trailing side open a little; shaken hard, it goes crazy */
    if (s->carried) {
        float vl = hypotf(s->move_x, s->move_y);
        if (vl > .3f) for (int g = 0; g < m->ng; g++) {
            float d[3]; m3_app(d, M, m->gn[g]);
            float back = -(d[0] * s->move_x + d[1] * s->move_y) / vl;
            if (back > .3f && m->gw[g] > 0) w.grp[g] = fmaxf(w.grp[g], fminf(.12f, vl * .012f) * (back - .3f) / m->gw[g]);
        }
    }
    if (s->shake > .92f && now - m->last_crazy > 2) { crazy(s, sh, M, 1.2f, now); s->shake = .5f; }
    /* going crazy: tension first, then flicking patches */
    if (now < m->tense_until) {
        float u = 1 - (float)(m->tense_until - now) / .4f;
        w.scale *= 1 - .07f * io3(u); w.rate = 0;
        for (int g = 0; g < m->ng; g++) w.grp[g] = -.02f * u;
    } else if (now < m->crazy_until)
        for (int g = 0; g < m->ng; g++) if (hashf((int)floor(now * 14) * 31 + g) > .82f && m->gw[g] > 0) w.grp[g] += .14f / m->gw[g];

    /* spikes: snap out (or rise calmly), hold, go back; a routine may have pushed the nodes already */
    for (int i = 0; i < MAX_SPIKE; i++) {
        Spike *p = &m->sp[i];
        if (!spike_live(p, now)) { p->len = 0; continue; }
        float e = (float)(now - p->at), a;
        if (p->up > .1f) a = e < p->up ? io3(e / p->up) : e < p->up + p->hd ? 1 : fmaxf(0, 1 - io3((e - p->up - p->hd) / p->dn));
        else a = env(e, p->up, p->hd, p->dn);
        float v = p->len * a;
        if (fabsf(v) > fabsf(w.push[p->v])) w.push[p->v] = v;
    }
    memcpy(m->push, w.push, sizeof m->push);
    for (int v = 0; v < sh->nv; v++) if (fabsf(m->push[v]) > .05f) m->glow[v] = fmaxf(m->glow[v], fminf(1, fabsf(m->push[v]) * 1.5f));

    /* the scan plane lights the nodes it passes */
    if (m->scan_a > 0) for (int v = 0; v < sh->nv; v++) { float d[3]; view_dir(s, M, sh->v[v], d); float k = d[1] - m->scan_h; m->glow[v] = fmaxf(m->glow[v], m->scan_a * expf(-k * k / .02f)); }

    /* everything follows its target on a critically damped curve, so nothing wobbles */
    float f = 1 - expf(-dt * 16);
    for (int t = 0; t < sh->nt; t++) { float a = m->ng ? w.grp[m->grp[t]] * m->gw[m->grp[t]] : 0; m->off[t] += (a - m->off[t]) * f; }
    if (!m->ang_on) for (int t = 0; t < sh->nt; t++) { /* layers not driven: settle on the nearest fifth of a turn (lax is a 5-fold axis), then rest */
        float home = roundf(m->ang[t] / STEP5) * STEP5;
        m->ang[t] += (home - m->ang[t]) * f;
        if (fabsf(m->ang[t] - home) < 1e-3f) m->ang[t] = 0;
    }
    m->scale += (w.scale - m->scale) * (1 - expf(-dt * 10));
    m->bright = w.bright;
    m->rate += ((turns_busy(m) ? 0 : w.rate) - m->rate) * (1 - expf(-dt * 4));
    m->inner_ang += m->inner_rate * dt;
    run_turns(s, now);
}

void motion_play(Session *s, int rt) {
    if (rt < 0 || rt >= RT_N) return;
    s->mo.want_rt = rt;
    s->mo.routine = -1; /* entered again on the next tick */
}

int motion_busy(const Session *s, double now) {
    const Motion *m = &s->mo;
    if (turns_busy(m) || m->inner > 0 || m->scan_a > 0 || m->lock > .01f || now < m->crazy_until || now - m->press_at < 1.4) return 1;
    int quiet_hole = m->rt == RT_HOLE && m->hole > .99f && !m->rs.hole.moving; /* only the void and its dust: session_rate gives it 10 fps */
    if (!quiet_hole && (m->ntm || m->tt_on || m->ta_on || m->bright != 1)) return 1;
    if (m->trail_on || m->squash > 0) return 1;
    for (int i = 0; i < MAX_SPIKE; i++) if (spike_live(&m->sp[i], now)) return 1;
    for (int i = 0; i < MAX_SPARK; i++) if (m->spark[i].life > 0) return 1;
    for (int i = 0; i < MAX_RING; i++) if (m->ring[i].a > 0) return 1;
    for (int t = 0; t < MAX_T; t++) if (fabsf(m->off[t]) > .002f || m->ang[t] != 0) return 1;
    return 0;
}

int motion_crazy(const Session *s, double now) { return now < s->mo.crazy_until && now >= s->mo.tense_until; }

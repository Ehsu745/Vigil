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
static float gauss(Session *s) { float u = fmaxf(1e-6f, mrand(s)), v = mrand(s); return sqrtf(-2 * logf(u)) * cosf(TAU * v); }
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
/* the rotation taking unit vector a onto unit vector b the shortest way */
static void turn_to(float *R, const float *a, const float *b) {
    float ax[3]; cross3(ax, a, b);
    float sn = len3(ax), cs = dot3(a, b);
    if (sn < 1e-6f) {
        if (cs > 0) { memcpy(R, I3, sizeof I3); return; }
        float X[3] = {1, 0, 0}, Y[3] = {0, 1, 0};
        cross3(ax, a, fabsf(a[0]) < .9f ? X : Y); norm3(ax);
        m3_rot(R, ax, PI_F);
        return;
    }
    for (int i = 0; i < 3; i++) ax[i] /= sn;
    m3_rot(R, ax, atan2f(sn, cs));
}
static void slerp_dir(float *out, const float *a, const float *b, float u) {
    float ax[3], R[9]; cross3(ax, a, b);
    float sn = len3(ax), cs = dot3(a, b);
    if (sn < 1e-6f) { float X[3] = {1, 0, 0}, Y[3] = {0, 1, 0}; cross3(ax, a, fabsf(a[0]) < .9f ? X : Y); norm3(ax); }
    else for (int i = 0; i < 3; i++) ax[i] /= sn;
    m3_rot(R, ax, atan2f(sn, cs) * u);
    m3_app(out, R, a);
}
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
    if (len > .4f && m->rt != RT_RESTLESS) len = .4f; /* the shell fills 72% of its tile: further out would be cut off (READY's spike cones are drawn on the layer) */
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
    q->own = col.r < 0; q->col = col; q->seed = mrand(s) * 99; q->stage = 0;
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
 * and brightness in c->w and sets the per-frame effects in Motion (tmi/tmx, tt, talpha, sharp, squash, trail,
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
/* READY's camera distance: the Bestiary's, which draws a point z shell radii toward you at 5 / (5 - .6 z) */
#define READY_CAM (5.0f * CIRCUM / .6f)
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

/* Blades: 15 to 20 single triangles, spread evenly, float out to a slowly turning ring behind the core; then two
 * pairs of smashes, each one full turn across the front (long, short), from the left or the right at random. The
 * flock is a bell curve in place and in timing: a comet band, thin at the head and tail. Each blade keeps its own
 * orientation from frame to frame and points along the smash like an arrow; the core swings with the flock. */
static float median(float *a, int n) {
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && a[j] < a[j - 1]; j--) { float t = a[j]; a[j] = a[j - 1]; a[j - 1] = t; }
    return a[n / 2];
}
static float blade_pair(Blade *b, float from, float start, int pair) {
    const float SW0 = .6f, SW1 = .2f;
    float s1 = fmaxf(start + b->lag, from);
    if (s1 > from && b->nseq < 9) b->seq[b->nseq++] = (BStep){1, 0, from, s1 - from}; /* waiting behind for its turn */
    if (b->nseq < 8) { b->seq[b->nseq++] = (BStep){2, (unsigned char)(pair * 2), s1, SW0}; b->seq[b->nseq++] = (BStep){2, (unsigned char)(pair * 2 + 1), s1 + SW0, SW1}; }
    return s1 + SW0 + SW1;
}
static void blades_new_round(Ctx *c) {
    Session *s = c->s; Motion *m = c->m; const Shape *sh = c->sh; BladesRS *B = &m->rs.blades;
    const float LIFT = .35f, SHUFFLE = .3f, HOME = .7f, FOLD = .3f;
    recut(s, sh, 1, 1); /* every triangle its own patch, all free to fly */
    for (int g = 0; g < m->ng; g++) m->gw[g] = 1;
    int nwant = 15 + (int)(mrand(s) * 6), ok[MAX_BLADE], n = 1;
    if (nwant > MAX_BLADE) nwant = MAX_BLADE;
    ok[0] = (int)(mrand(s) * m->ng) % m->ng;
    while (n < nwant && n < m->ng) { /* each next pick the one farthest from those picked */
        int best = -1; float bd = -1;
        for (int g = 0; g < m->ng; g++) {
            int used = 0; float d = 9;
            for (int k = 0; k < n; k++) { if (ok[k] == g) used = 1; d = fminf(d, 1 - dot3(m->gn[g], m->gn[ok[k]])); }
            if (used) continue;
            d += mrand(s) * .02f;
            if (d > bd) { bd = d; best = g; }
        }
        ok[n++] = best;
    }
    float zs[MAX_BLADE];
    for (int i = 0; i < n; i++) zs[i] = clampf(gauss(s), -2.2f, 2.2f);
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && zs[j] < zs[j - 1]; j--) { float t = zs[j]; zs[j] = zs[j - 1]; zs[j - 1] = t; }
    for (int i = 0; i < n; i++) {
        Blade *b = &B->blade[i];
        memset(b, 0, sizeof *b);
        float side = clampf(gauss(s), -2.2f, 2.2f), e2 = zs[i] + .4f * (side < 0 ? -1 : 1), en = expf(-e2 * e2 / 2);
        b->g = ok[i];
        for (int t = 0; t < sh->nt; t++) if (m->grp[t] == b->g) { b->t = t; break; }
        b->tip = sh->t[b->t][(int)(mrand(s) * 3) % 3];
        b->slot = (float)i / n * TAU; b->lag = .11f * zs[i]; b->side = .3f * side * en; b->gd = rnd(s, .3f, .9f);
        b->seq[b->nseq++] = (BStep){0, 0, LIFT, b->gd};
    }
    B->n = n;
    /* each pair starts from the left (about 120°) or the right (about 60°) at random, the second from the other side */
    for (int p = 0; p < 2; p++) {
        int left = mrand(s) < .5f;
        for (int k = 0; k < 2; k++) { float deg = ((left ^ k) ? 120 : 60) + rnd(s, -12, 12), a = deg * PI_F / 180; set3(B->dirs[p * 2 + k], -cosf(a), sinf(a), 0); }
    }
    float tmp[MAX_BLADE], ends1[MAX_BLADE], ends2[MAX_BLADE];
    for (int i = 0; i < n; i++) tmp[i] = B->blade[i].gd;
    float g1 = LIFT + median(tmp, n);
    for (int i = 0; i < n; i++) ends1[i] = blade_pair(&B->blade[i], LIFT + B->blade[i].gd, g1, 0);
    memcpy(tmp, ends1, sizeof(float) * n);
    float g2 = median(tmp, n) + SHUFFLE, home_at = 0;
    for (int i = 0; i < n; i++) { ends2[i] = blade_pair(&B->blade[i], ends1[i], g2, 1); home_at = fmaxf(home_at, ends2[i]); }
    B->lead = 0;
    for (int i = 0; i < n; i++) {
        Blade *b = &B->blade[i];
        if (home_at > ends2[i] && b->nseq < 9) b->seq[b->nseq++] = (BStep){1, 0, ends2[i], home_at - ends2[i]};
        if (b->nseq < 9) b->seq[b->nseq++] = (BStep){3, 0, home_at, HOME};
        if (fabsf(b->lag) < fabsf(B->blade[B->lead].lag)) B->lead = i; /* the middle of the flock: the core swings with it */
    }
    B->fold = home_at + HOME; B->end = B->fold + FOLD;
    set3(B->core_v, 0, 0, 0);
    B->at = c->now;
}
static void r_blades(Ctx *c) {
    Session *s = c->s; Motion *m = c->m; const Shape *sh = c->sh; const float *M = c->M; BladesRS *B = &m->rs.blades;
    const float LIFT = .35f, FOLD = .3f, CORE = .18f;
    c->w->rate = 0; m->inner = .35f; m->inner_rate = -.5f;
    if (!sh->nt) return;
    if (!B->n || c->now - B->at > B->end + .6f) blades_new_round(c);
    float e = (float)(c->now - B->at), Mt[9], F[3] = {0, 0, 1};
    m3_tr(Mt, M);
    float lift = e < LIFT ? outx(e / LIFT) : e < B->fold ? 1 : fmaxf(0, 1 - in3((e - B->fold) / FOLD));
    int has_want = 0;
    m->sharp_on = 1;
    float dtb = fminf(.05f, fmaxf(0, c->dt)), clock = (float)(c->now - s->created);
    for (int i = 0; i < B->n; i++) {
        Blade *b = &B->blade[i];
        float home[3], d[3], vel[3] = {0, 0, 0};
        m3_app(home, M, m->gn[b->g]); norm3(home);
        float ringa = b->slot + clock * .12f, rr = .62f * (1 + .04f * sinf(clock * 1.4f));
        float back1[3] = {cosf(ringa) * rr, sinf(ringa) * rr, -1}; norm3(back1);
        float loose = 0, home_u = 0; int has_vel = 0;
        const BStep *st = NULL;
        for (int k = 0; k < b->nseq; k++) if (e >= b->seq[k].s && e < b->seq[k].s + b->seq[k].d) { st = &b->seq[k]; break; }
        if (!st && b->nseq && e >= b->seq[b->nseq - 1].s) st = &b->seq[b->nseq - 1];
        memcpy(d, home, sizeof d);
        if (st) {
            float u = clampf((e - st->s) / st->d, 0, 1);
            if (st->k == 0) { slerp_dir(d, home, back1, io3(u)); loose = io3(u); }
            else if (st->k == 1) { memcpy(d, back1, sizeof d); loose = 1; }
            else if (st->k == 3) { slerp_dir(d, back1, home, io3(u)); loose = 1 - io3(u); home_u = smooth01((u - .55f) / .45f); } /* heading first; it settles into its own orientation only on the last stretch */
            else { /* one full turn about the axis square to the smash: slow behind, fastest across the front */
                const float *D = B->dirs[st->dir];
                float A[3], R[9], front = bump(u), a = TAU * (u - .75f * sinf(TAU * u) / TAU);
                cross3(A, F, D); norm3(A);
                if (i == B->lead) { float sp = TAU * (1 - .75f * cosf(TAU * u)) / st->d; for (int q = 0; q < 3; q++) B->want[q] = A[q] * CORE * sp; has_want = 1; }
                float tight = 1 - .88f * front, bk[3] = {cosf(ringa) * rr * tight, sinf(ringa) * rr * tight, -1};
                norm3(bk);
                m3_rot(R, A, a); m3_app(d, R, bk);
                loose = tight;
                float sd = tanf(b->side * front); /* the band: each blade off to the side by its own amount */
                for (int q = 0; q < 3; q++) d[q] += A[q] * sd;
                norm3(d);
                cross3(vel, A, d); norm3(vel); has_vel = 1;
            }
        }
        /* the blade's own orientation (W: model to view), carried along by small turns, never rebuilt as the
         * shortest turn from home (which flips when a blade passes the far side of the core) */
        float mv[3] = {0, 0, 0}; int has_mv = 0;
        if (!b->init || !st) { memcpy(b->W, M, sizeof b->W); memcpy(b->dp, home, sizeof b->dp); b->al = 0; b->init = 1; }
        else {
            float dd[3] = {d[0] - b->dp[0], d[1] - b->dp[1], d[2] - b->dp[2]}, R[9];
            if (dtb > 0 && len3(dd) / dtb > .25f) { memcpy(mv, dd, sizeof mv); has_mv = 1; }
            turn_to(R, b->dp, d); m3_mul(b->W, R, b->W); m3_ortho(b->W);
            memcpy(b->dp, d, sizeof b->dp);
        }
        float tipm[3], tipw[3], tipFree[3];
        unit_v(sh, b->tip, tipm);
        m3_app(tipw, b->W, tipm);
        #define TANG(out, p) do { float k_ = dot3(p, d); for (int q_ = 0; q_ < 3; q_++) out[q_] = p[q_] - d[q_] * k_; } while (0)
        TANG(tipFree, tipw); norm3(tipFree);
        float th;
        float facing = smooth01((dot3(home, d) + .92f) / .45f); /* the blend is unstable from almost opposite home: it fades in past that */
        if (home_u > 0 && facing > 0) { /* going home: blend exactly onto the orientation it left with */
            float R[9], RM[9], hw[3], ht[3], cr[3];
            turn_to(R, home, d); m3_mul(RM, R, M); m3_app(hw, RM, tipm);
            TANG(ht, hw); norm3(ht);
            cross3(cr, tipFree, ht);
            float raw = atan2f(dot3(cr, d), dot3(tipFree, ht));
            if (!b->has_ht) { b->ht = b->al + atan2f(sinf(raw - b->al), cosf(raw - b->al)); b->has_ht = 1; }
            else b->ht += atan2f(sinf(raw - b->ht), cosf(raw - b->ht));
            th = b->al + (b->ht - b->al) * home_u * facing;
            if (home_u >= 1) b->al = th;
        } else if (home_u > 0) {
            b->has_ht = 0;
            float whole = roundf(b->al / TAU) * TAU;
            b->al = whole + (b->al - whole) * expf(-dtb * 5);
            th = b->al;
        } else {
            b->has_ht = 0;
            /* all the blades point the same way: along the smash they are in or about to make */
            const BStep *sm = NULL;
            if (st && st->k != 3) { if (st->k == 2) sm = st; else for (int k = 0; k < b->nseq; k++) if (b->seq[k].k == 2 && b->seq[k].s >= st->s) { sm = &b->seq[k]; break; } }
            float hd[3]; int has_hd = 0;
            if (st && st->k == 2 && has_vel) { memcpy(hd, vel, sizeof hd); has_hd = 1; }
            else if (sm) {
                float A[3], v0[3]; cross3(A, F, B->dirs[sm->dir]); cross3(v0, A, d); norm3(v0);
                TANG(hd, v0);
                if (len3(hd) > .3f) { norm3(hd); has_hd = 1; }
            } else if (has_mv) { TANG(hd, mv); if (len3(hd) > 1e-5f) { norm3(hd); has_hd = 1; } }
            if (has_hd) { /* quickly, but never more than about 0.23 rad a frame, so re-aiming cannot jump */
                float cr[3]; cross3(cr, tipFree, hd);
                float raw = atan2f(dot3(cr, d), dot3(tipFree, hd)), tgt = b->al + atan2f(sinf(raw - b->al), cosf(raw - b->al)), was = b->al;
                b->al += clampf((tgt - b->al) * (1 - expf(-dtb * 25)), -14 * dtb, 14 * dtb);
                if (st && st->k == 2) b->al += (tgt - b->al) * smooth01((d[2] - .1f) / .4f); /* across the visible front, locked onto its line of flight */
                float most = 20 * dtb;
                b->al = was + clampf(b->al - was, -most, most);
            }
            th = b->al;
        }
        #undef TANG
        float R[9], X[9];
        m3_rot(R, d, th);
        m3_mul(X, R, b->W);
        m3_mul(X, Mt, X);
        m->tmi[b->t] = (unsigned char)tm_add(m, X);
        m->sharp_v[b->t] = (signed char)(b->tip < 128 ? b->tip : -1);
        m->sharp_k[b->t] = has_vel ? 1 - loose : 0; /* drawn out into a dart by its speed */
        float sheen = .22f * (1 - loose) / .88f;
        if (sheen > .02f) for (int k = 0; k < 3; k++) m->glow[sh->t[b->t][k]] = fmaxf(m->glow[sh->t[b->t][k]], sheen);
        c->w->grp[b->g] += (.18f + .14f * loose) * lift; /* a layer well out from the core, further when loose */
    }
    /* the core carries momentum: its spin chases what the smash wants and coasts down after */
    float k = 1 - expf(-dtb * (has_want ? 4 : 3.5f));
    for (int q = 0; q < 3; q++) B->core_v[q] += ((has_want ? B->want[q] : 0) - B->core_v[q]) * k;
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
 * twisting harder and stretching thin, and are swallowed at the edge of the void with a golden flash. Then only
 * the hole and its disc (drawn in render.c). Pointing at it wakes it: the pieces come home. */
static void hole_enter(Ctx *c) {
    Session *s = c->s; Motion *m = c->m; const Shape *sh = c->sh; HoleRS *H = &m->rs.hole;
    int cnt[MAX_T]; memset(cnt, 0, sizeof cnt);
    for (int t = 0; t < sh->nt; t++) {
        Piece *pc = &H->piece[m->grp[t]];
        for (int k = 0; k < 3; k++) for (int q = 0; q < 3; q++) pc->c[q] += sh->v[sh->t[t][k]][q] / CIRCUM / 3;
        cnt[m->grp[t]]++;
    }
    /* the disc's plane as render.c will draw it (about 6 degrees above it, y down here), in the view as it is now */
    memcpy(H->M0, c->M, sizeof H->M0);
    float el = .26f, roll = -.06f;
    H->dn[0] = sinf(roll) * cosf(el); H->dn[1] = -cosf(roll) * cosf(el); H->dn[2] = sinf(el);
    float X[3] = {1, 0, 0}, d = dot3(X, H->dn);
    for (int q = 0; q < 3; q++) H->d1[q] = X[q] - d * H->dn[q];
    norm3(H->d1); cross3(H->d2, H->dn, H->d1);
    for (int g = 0; g < m->ng; g++) { /* each piece: where it starts (in the view), the way it will be blown, its spin */
        Piece *pc = &H->piece[g];
        for (int q = 0; q < 3; q++) pc->c[q] /= cnt[g] ? cnt[g] : 1;
        float v[3]; m3_app(v, c->M, pc->c);
        pc->r = len3(v); memcpy(pc->n, v, sizeof pc->n); norm3(pc->n);
        cross3(pc->tn, H->dn, pc->n); /* (it will go round the way the disc turns) */
        if (len3(pc->tn) < .05f) memcpy(pc->tn, H->d1, sizeof pc->tn);
        norm3(pc->tn);
        pc->vr = rnd(s, 2.1f, 3.0f) * (1.15f - .05f * cnt[g]); /* small ones fly fastest */
        pc->L = rnd(s, .35f, .8f); pc->phi = 0;
        rand_dir(s, pc->ax); pc->spin = rnd(s, 2, 7) * (mrand(s) < .5f ? -1 : 1) / sqrtf((float)(cnt[g] ? cnt[g] : 1)); pc->tw = 0;
        pc->size = cnt[g]; pc->gone = 0;
    }
    for (int i = 0; i < NSTAR; i++) { H->star[i][0] = rnd(s, -1.6f, 1.6f); H->star[i][1] = rnd(s, -1.6f, 1.6f); H->star[i][2] = rnd(s, .6f, 1.4f); H->star[i][3] = rnd(s, 0, TAU); }
    H->moving = 1;
}
uint32_t HOLE_ACTS = HOLE_ACTS_DEF; /* all of them (Jets retired); HA_LENS is not played but a switch: the stars behind it bent round it, always */
const char *HOLE_ACT_NAME[HA_N] = {"", "Jets", "Tidal disruption", "Flare", "Lensing", "Precession", "Hot spot"};
const char *HOLE_ACT_ABOUT[HA_N] = {"",
    "(retired)",
    "A star falls in on a long orbit, is pulled into a thin stream as it swings past, half of it flung away and half wrapping round the hole and falling in; the disc flares.",
    "A loop of hot gas arches up off the inner disc, twisting as it grows, then snaps: a flash, the disc lit up, and its top flung out as a blob that cools as it goes; the photon ring echoes the flash a moment later.",
    "Always, while it is in use: the stars behind the hole bent round its shadow into arcs and doubled images inside its circle, shifting as you turn it. Off: a plain dark sky.",
    "Frame dragging: the disc wobbles like a top, its axis sweeping round a widening cone so it shows its face from every side, while the cores round it are whipped round faster and closer, as if being sucked in, then let go.",
    "A bright clump of hot gas spirals in toward the inner edge, faster as it falls, brighter as it comes toward you, smeared out behind it; its lensed image races round the photon ring."};
const float HOLE_ACT_DUR[HA_N] = {0, 6.5f, 10, 5.5f, 7, 9, 10};
void hole_play(Session *s, int a) {
    if (a <= HA_NONE || a >= HA_N || a == HA_LENS || (HA_RETIRED >> a & 1)) return; /* (Lensing is not played: it is always on, or off) */
    if (s->mo.rt != RT_HOLE || s->mo.hole < 0.5f) motion_play(s, RT_HOLE); /* not a black hole yet: it becomes one, then plays it */
    s->mo.hole_want = a;
}
/* A tidal disruption (HA_FEED), in units of the hole's mass M and its own time (BH_TS a second), in the disc's frame.
 * Gravity is Paczynski-Wiita's, -1 / (r - 2)^2, which has the real hole's last stable orbit (6) and lets orbits
 * precess, so the returning stream crosses itself. The star comes in on a near-parabolic orbit; while it nears the
 * hole it is drawn out along its path (TdeRS.hist); once past the closest point it breaks up: each piece along the
 * stretch keeps its own speed, spread so the inner, leading half is bound (it swings back round and falls in) and the
 * trailing half is flung away. Pieces that fall in heat the disc (heat: the flare it gives). */
float HOLE_TILT = 2.5f;
static int bits_n(uint32_t v) { int n = 0; for (; v; v &= v - 1) n++; return n; }
static void tde_acc(const float *p, float *a) {
    float r = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]) + 1e-4f, f = -1 / ((r - 2) * (r - 2) * r);
    for (int q = 0; q < 3; q++) a[q] = p[q] * f;
}
static void tde_step(float *p, float *v, float h) { /* velocity Verlet */
    float a[3]; tde_acc(p, a);
    for (int q = 0; q < 3; q++) { v[q] += a[q] * h / 2; p[q] += v[q] * h; }
    tde_acc(p, a);
    for (int q = 0; q < 3; q++) v[q] += a[q] * h / 2;
}
/* The star's own gravity (softened, as a gas ball's): strong enough to hold it together out where it starts, too weak
 * for the hole's tidal pull near its closest point (about 12 M in), where it is stretched and torn into a stream */
#define TDE_RS 0.7f
#define TDE_GM 5e-4f
static void tde_start(Session *s, HoleRS *H) {
    TdeRS *T = &H->tde;
    memset(T, 0, sizeof *T);
    /* In the disc's frame (x toward you, y to your right, z its axis; bh_gas_points draws it so). Its two legs swing out
     * about 120 degrees either side of its closest point, so with that point in front of the hole both legs lie behind
     * it, off to the sides, bent over the shadow as they go; and its orbit is tipped 35 to 42 degrees down out of the
     * disc's plane there, so the pass in front runs under the shadow, never across it (a pass in front in the disc's
     * plane, or a leg coming out at you, drew a flat line over the shadow). From the left it goes round the way the gas
     * goes (prograde: played on its own it always comes so, and its debris joins the disc's flow); a click on the right
     * (act_p > 0) mirrors it */
    const float r0 = 23, rp = 7.2f, L = rp * sqrtf(2 / (rp - 2));
    float f0 = acosf(2 * rp / r0 - 1) * 1.08f; /* (how far round from where it starts to its closest point) */
    float dir = H->act_p > 0 ? -1 : 1, psi = rnd(s, -.25f, .25f), th0 = psi - dir * f0;
    float inc = rnd(s, .61f, .73f), ax[3] = {-sinf(psi), cosf(psi), 0}, Rm[9];
    float v0 = sqrtf(2 / (r0 - 2)), vt = L / r0, vr = -sqrtf(fmaxf(0, v0 * v0 - vt * vt));
    float p[3] = {r0 * cosf(th0), r0 * sinf(th0), 0}, v[3] = {vr * cosf(th0) - dir * vt * sinf(th0), vr * sinf(th0) + dir * vt * cosf(th0), 0};
    m3_rot(Rm, ax, inc);
    m3_app(T->s, Rm, p); m3_app(T->s + 3, Rm, v);
    T->n = TDE_N; T->spread = TDE_RS;
    for (int i = 0; i < TDE_N; i++) { /* its gas: a ball, all moving with it */
        float u[3];
        do { for (int q = 0; q < 3; q++) u[q] = rnd(s, -1, 1); } while (u[0] * u[0] + u[1] * u[1] + u[2] * u[2] > 1);
        for (int q = 0; q < 3; q++) { T->p[i][q] = T->s[q] + TDE_RS * u[q]; T->v[i][q] = T->s[3 + q]; }
        T->on[i] = 1;
    }
}
static void tde_tick(HoleRS *H, float dt) {
    TdeRS *T = &H->tde;
    const float h = .1f, hh = h; /* a fixed step (M), the time left over carried on (and drawn ahead by it: render.c), so it never jerks */
    T->acc += dt * BH_TS;
    int n = (int)(T->acc / h);
    T->acc -= n * h;
    if (n > 40) n = 40;
    for (int k = 0; k < n; k++) {
        float c[3] = {0, 0, 0}, cv[3] = {0, 0, 0}, sp = 0; int m = 0;
        for (int i = 0; i < T->n; i++) if (T->on[i]) { for (int q = 0; q < 3; q++) { c[q] += T->p[i][q]; cv[q] += T->v[i][q]; } m++; }
        if (!m) break;
        for (int q = 0; q < 3; q++) { c[q] /= m; cv[q] /= m; }
        float rd0 = dot3(T->s, T->s + 3);
        memcpy(T->s, c, sizeof c); memcpy(T->s + 3, cv, sizeof cv);
        if (!T->split && rd0 < 0 && dot3(c, cv) >= 0) { /* its centre past its closest point: each piece's orbital energy, from now on its place along the stream */
            T->split = 1;
            float ch[3] = {c[0], c[1], c[2]}, vh[3] = {cv[0], cv[1], cv[2]}; norm3(ch); norm3(vh);
            for (int i = 0; i < T->n; i++) { /* the tidal spread in orbital energy (here made larger than a real star's, so the bound half
                                              * comes back round in sight): the side nearer the hole slower and bound, the far side faster, flung off */
                float off = (T->p[i][0] - c[0]) * ch[0] + (T->p[i][1] - c[1]) * ch[1] + (T->p[i][2] - c[2]) * ch[2];
                for (int q = 0; q < 3; q++) T->v[i][q] += vh[q] * 0.14f * off;
                float r = len3(T->p[i]); T->q[i] = 0.5f * dot3(T->v[i], T->v[i]) - 1 / (r - 2);
            }
        }
        for (int i = 0; i < T->n; i++) {
            if (!T->on[i]) continue;
            float d[3] = {T->p[i][0] - c[0], T->p[i][1] - c[1], T->p[i][2] - c[2]}, d2 = dot3(d, d), f = -TDE_GM * h / powf(d2 + TDE_RS * TDE_RS, 1.5f);
            for (int q = 0; q < 3; q++) T->v[i][q] += d[q] * f; /* its own gravity, toward its centre */
            if (d2 < 1.44f * TDE_RS * TDE_RS && dot3(c, c) > 16 * 16) { float w = 1 - expf(-h * 0.05f); for (int q = 0; q < 3; q++) T->v[i][q] += (cv[q] - T->v[i][q]) * w; } /* (inside it, the gas settles; not in close, where it is torn) */
            sp += d2;
        }
        T->spread = sqrtf(sp / m);
        {
            for (int i = 0; i < T->n; i++) {
                if (!T->on[i]) continue;
                float vr0 = T->p[i][0] * T->v[i][0] + T->p[i][1] * T->v[i][1] + T->p[i][2] * T->v[i][2];
                tde_step(T->p[i], T->v[i], hh);
                float r = sqrtf(T->p[i][0] * T->p[i][0] + T->p[i][1] * T->p[i][1] + T->p[i][2] * T->p[i][2]);
                float vr1 = T->p[i][0] * T->v[i][0] + T->p[i][1] * T->v[i][1] + T->p[i][2] * T->v[i][2];
                if (vr0 > 0 && vr1 <= 0) T->back[i] = 1; /* (at its far point: falling back in) */
                if (T->hot[i] > 0) { /* shocked: it loses its energy and circles in, forming a disc (circularization) */
                    float dd = expf(-hh * 0.04f * T->hot[i]), vr = vr1 / (r * r);
                    for (int q = 0; q < 3; q++) T->v[i][q] -= T->p[i][q] * vr * (1 - dd); /* (its fall slowed: the radial part damped) */
                    T->hot[i] *= expf(-hh * 0.01f);
                }
                if (r < 3.2f) { T->on[i] = 0; T->heat += (2.4f + 3 * T->hot[i]) / TDE_N; } /* fallen in: the disc flares */
                else if (r > 40) T->on[i] = 0;
            }
            /* the returning stream crashing into the incoming one (its path swung round by the hole's pull, apsidal
             * precession): a shock where they meet, the gas there heated */
            for (int i = 0; i < T->n; i++) {
                if (!T->on[i] || !T->back[i]) continue;
                for (int j = 0; j < T->n; j += 2) {
                    if (j == i || !T->on[j] || T->back[j]) continue;
                    float dx = T->p[i][0] - T->p[j][0], dy = T->p[i][1] - T->p[j][1], dz = T->p[i][2] - T->p[j][2];
                    if (dx * dx + dy * dy + dz * dz > 1.6f * 1.6f) continue;
                    float w = T->shock_amp < 0.05f ? 1.0f : 0.15f;
                    for (int q = 0; q < 3; q++) T->shock[q] += (0.5f * (T->p[i][q] + T->p[j][q]) - T->shock[q]) * w;
                    T->shock_amp = fminf(3, T->shock_amp + 0.35f);
                    T->hot[i] = fminf(4, T->hot[i] + 1.5f); T->hot[j] = fminf(4, T->hot[j] + 1);
                    T->heat = fminf(1.5f, T->heat + 0.02f);
                    break;
                }
            }
            T->shock_amp *= expf(-hh * 0.03f);
        }
    }
}
/* Held down on the hole: gas is pulled off the pointer (the screen frame, M units) and falls in on a spiral, round the
 * way the disc turns, onto the disc, which it heats (TdeRS.heat, the same flare as a torn star's) */
static void hold_tick(Session *s, HoleRS *H, float dt, double now, int on) {
    HoldRS *P = &H->hold;
    if (on && now >= P->next) {
        P->next = now + .035;
        float lx = s->press_x, ly = s->press_y; /* (the panel keeps it under the pointer while held) */
        float x = lx * BH_SHELL_M + rnd(s, -.25f, .25f), y = -ly * BH_SHELL_M + rnd(s, -.25f, .25f), z = rnd(s, -.4f, .4f), r = sqrtf(x * x + y * y + z * z) + 1e-3f;
        if (r > 7.5f) {
            int i = P->head; P->head = (P->head + 1) % HS_N;
            float t[3] = {z, 0, -x}, tl = sqrtf(t[0] * t[0] + t[2] * t[2]) + 1e-3f, sp = .86f * sqrtf(1 / (r - 2));
            P->p[i][0] = x; P->p[i][1] = y; P->p[i][2] = z;
            for (int q = 0; q < 3; q++) P->v[i][q] = t[q] / tl * sp - P->p[i][q] / r * .05f;
            P->age[i] = .001f;
        }
    }
    const float hh = .12f;
    P->acc += dt * BH_TS;
    int n = (int)(P->acc / hh);
    P->acc -= n * hh;
    if (n > 40) n = 40;
    for (int i = 0; i < HS_N; i++) {
        if (P->age[i] <= 0) continue;
        P->age[i] += dt;
        for (int k = 0; k < n; k++) {
            tde_step(P->p[i], P->v[i], hh);
            P->v[i][1] *= 1 - .02f * hh; /* (settling toward the disc's plane) */
        }
        float r = sqrtf(P->p[i][0] * P->p[i][0] + P->p[i][1] * P->p[i][1] + P->p[i][2] * P->p[i][2]);
        if (r < 4.5f || P->age[i] > 7) { P->age[i] = 0; H->tde.heat = fminf(1.5f, H->tde.heat + .012f); }
    }
}
static void r_hole(Ctx *c) {
    Session *s = c->s; Motion *m = c->m; const Shape *sh = c->sh; const float *M = c->M; Want *w = c->w; HoleRS *H = &m->rs.hole;
    float dt = fminf(.05f, c->dt), e = c->e;
    /* resting on it (lock-on) wakes it; a glance does not, nor does handling it: a press, a hold, a drag, a carry, the wheel */
    if (H->grow > .3f) { float d = expf(-dt * 2.2f); s->spin_x *= d; s->spin_y *= d; } /* (a throw or the wheel spins it a moment, not on and on) */
    int handled = s->pressing || s->held || s->carried || fabsf(s->spin_x) + fabsf(s->spin_y) > .6f;
    if (handled) H->touched = c->now;
    H->wake += ((s->lock_to > .5f && !handled && c->now - H->touched > 1 ? 1.0f : 0.0f) - H->wake) * (1 - expf(-dt * 4));
    { /* turned like a shell: a drag and the wheel (Session.spin_*, which also carries a flung drag on) turn its disc's axis,
       * a trackball in the view (x right, y up, z toward you); left alone a while, it eases back to lying almost edge-on */
        const float REST[3] = {-.06f * .966f, .966f, .26f}; /* (about 15 degrees above edge-on) */
        if (len3(H->nv) < .5f) memcpy(H->nv, REST, sizeof H->nv);
        float dax = s->ax - H->ax_seen, day = s->ay - H->ay_seen;
        H->ax_seen = s->ax; H->ay_seen = s->ay;
        float rx = 0, ry = 0;
        if (H->grow > .3f) {
            if (s->held && fabsf(dax) + fabsf(day) < 1) { rx = dax; ry = day; }
            else { rx = s->spin_x * dt; ry = s->spin_y * dt; }
        }
        if (fabsf(rx) + fabsf(ry) > 1e-6f) {
            float X[3] = {1, 0, 0}, Y[3] = {0, 1, 0}, R1[9], R2[9], t[3];
            m3_rot(R1, X, rx); m3_rot(R2, Y, ry);
            m3_app(t, R1, H->nv); m3_app(H->nv, R2, t); norm3(H->nv);
            H->flow_v += ry * 2.5f; /* (a sideways turn pushes the disc round too) */
        }
        /* home again once let alone: a critically damped spring whose pull eases in over a second, so it glides back
         * without a jolt at either end */
        float pull = H->act == HA_PRECESS ? 0 : clampf(((float)(c->now - H->touched) - .3f) / 1.0f, 0, 1), W = 3.2f;
        pull = pull * pull * (3 - 2 * pull);
        if (pull > 0) {
            for (int q = 0; q < 3; q++) { H->nvel[q] += (W * W * (REST[q] - H->nv[q]) * pull - 2 * W * H->nvel[q]) * dt; H->nv[q] += H->nvel[q] * dt; }
            norm3(H->nv);
        } else for (int q = 0; q < 3; q++) H->nvel[q] = 0;
        H->flow_v *= expf(-dt * 0.6f); /* (the wheel's push on the gas: a few seconds to ease off) */
    }
    { /* away: quiet (embers, slowed); back: it flares up */
        float was = s->quiet;
        s->quiet += (s->quiet_to - s->quiet) * (1 - expf(-dt * (s->quiet_to > s->quiet ? 0.5f : 2.5f)));
        if (was > 0.5f && s->quiet <= 0.5f) s->woke_at = c->now;
    }
    H->flow += dt * (1 + 1.2f * s->hover + 0.8f * s->carry) * (1 - 0.75f * s->quiet) + H->flow_v * dt; /* the disc turns faster while pointed at, carried or wheeled; slower when you are away */
    H->tilt_ph += dt * HOLE_TILT; /* (its own slow tilt and roll: a phase, so changing the speed never jumps it) */ /* the disc turns faster while pointed at or carried */
    /* The shell bursts: for BANG s it gathers (trembling, squeezed, brighter), then the shield explodes, its pieces blown
     * out every way, tumbling, and the core goes dark: a small black hole. Its pull takes over at once: the pieces slow,
     * fall back on tightening spirals that settle into the disc's plane, stretch thin near the hole and are swallowed at
     * its edge, and with each one it grows (grow: the share of the shell it has eaten). Worked out in the view as it
     * began (HoleRS.M0, dn, d1, d2), then turned back into the shell's own frame. */
    const float BANG = 0.9f, GM = 6;
    float wake = H->wake;
    if (e < BANG) {
        float u = e / BANG, uu = u * u;
        w->scale *= 1 - .1f * io3(u);
        w->bright *= 1 + 2.5f * uu;
        set_rate(c, .4f + 2 * uu);
        float shk = .03f * uu;
        for (int t = 0; t < sh->nt; t++) { float j[3] = {rnd(s, -1, 1), rnd(s, -1, 1), rnd(s, -1, 1)}; tt_set(m, t, j, shk); }
        if (mrand(s) < (4 + 40 * uu) * dt) m->glow[(int)(mrand(s) * sh->nv) % sh->nv] = 1; /* crackling */
        return;
    }
    if (!H->banged) { /* the shield explodes; the core is gone dark */
        H->banged = 1; ring(m, c->now, 1); flash(m, sh->nv, 1);
        float z[3] = {0, 0, 0};
        spark_at(s, M, z, .45f, 30, RGB255(255, 236, 214));
        for (int i = 0; i < 36; i++) { float d[3], p[3]; rand_dir(s, d); float sp = rnd(s, 1.5f, 3.2f); for (int q = 0; q < 3; q++) { p[q] = d[q] * .3f; d[q] *= sp; } spark(s, M, p, d, rnd(s, .3f, .7f), 2.2f, 0, rnd(s, 1, 2), SK_STREAK, 1, RGB255(255, 200, 150)); }
    }
    float eb = e - BANG;
    m->hole = clampf(eb / .25f, 0, 1) * (1 - wake);
    w->bright *= 1 + 2 * expf(-eb * 8);
    w->rate = 0;
    float g2 = clampf(H->grow, 0, 1), edge = .21f + (5.196f / BH_SHELL_M - .21f) * g2 * g2 * (3 - 2 * g2); /* the shadow's edge, as render.c draws it */
    const int sub = 4;
    float h = dt / sub, flat = clampf((eb - .3f) / 2.2f, 0, 1); /* the orbits settle into the disc's plane */
    flat = flat * flat * (3 - 2 * flat);
    int base = m->ntm, total = 0, eaten = 0;
    m->ta_on = 1;
    const float *n = H->dn;
    float Mt[9]; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) Mt[i * 3 + j] = M[j * 3 + i];
    for (int g = 0; g < m->ng; g++) {
        Piece *pc = &H->piece[g];
        total += pc->size;
        if (!pc->gone) for (int k = 0; k < sub; k++) {
            pc->vr += -GM / (pc->r * pc->r) * h - pc->vr * .25f * h; pc->r = fmaxf(.05f, pc->r + pc->vr * h);
            pc->phi += fminf(25, pc->L / (pc->r * pc->r)) * h; /* the spiral tightens as it falls */
            pc->tw += (pc->spin + (pc->vr < 0 ? 3 / fmaxf(.25f, pc->r) : 0)) * h;
        }
        float rd[3], P[3];
        for (int q = 0; q < 3; q++) rd[q] = pc->n[q] * cosf(pc->phi) + pc->tn[q] * sinf(pc->phi);
        float hh = dot3(rd, n);
        for (int q = 0; q < 3; q++) rd[q] -= hh * flat * n[q]; /* pressed toward the plane */
        norm3(rd);
        for (int q = 0; q < 3; q++) P[q] = rd[q] * pc->r;
        int falling = pc->vr < 0;
        float near = clampf((.9f - pc->r) / .6f, 0, 1), sx = falling ? 1 + 2.4f * near : 1, sc = falling ? .35f + .65f * clampf((pc->r - edge) / .6f, 0, 1) : 1;
        float al = pc->gone ? 0 : clampf((pc->r - edge + .02f) / .18f, 0, 1);
        if (falling && !pc->gone && pc->r < edge + .02f) { /* swallowed: a flicker at the edge, the hole a little bigger */
            pc->gone = 1; H->tde.heat = fminf(1.2f, H->tde.heat + .05f);
            float pl[3]; m3_app(pl, Mt, P); spark_at(s, M, pl, .25f, 5 + 2.0f * pc->size, RGB255(255, 210, 150));
        }
        if (pc->gone) eaten += pc->size;
        /* the piece: tumbling, then drawn out along its orbit and thinned across it as it nears the hole */
        float tg[3], Rt[9], St[9], Av[9], A[9], T1[9];
        cross3(tg, n, rd); norm3(tg);
        m3_rot(Rt, pc->ax, pc->tw);
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) St[i * 3 + j] = ((i == j) + (sx - 1) * tg[i] * tg[j] + (1 / sqrtf(sx) - 1) * rd[i] * rd[j]) * sc;
        m3_mul(Av, St, Rt);
        m3_mul(T1, Av, H->M0); m3_mul(A, Mt, T1);
        float Pl[3], ac[3], off[3];
        m3_app(Pl, Mt, P); m3_app(ac, A, pc->c);
        for (int q = 0; q < 3; q++) off[q] = Pl[q] - ac[q];
        if (wake > .001f) { for (int i = 0; i < 9; i++) A[i] = A[i] * (1 - wake) + I3[i] * wake; for (int q = 0; q < 3; q++) off[q] *= 1 - wake; al = al * (1 - wake) + wake; }
        tm_add(m, A);
        for (int t = 0; t < sh->nt; t++) if (m->grp[t] == g) { m->tmi[t] = (unsigned char)(base + g + 1); tt_set(m, t, off, 1); m->talpha[t] = al; }
    }
    /* it grows as it eats: to the share of the shell swallowed (eased), back again as it wakes */
    float gt = wake < .5f ? (total ? (float)eaten / total : 1) : 0;
    H->grow += (gt - H->grow) * (1 - expf(-dt * (gt > H->grow ? 2.5f : 4)));
    /* grown: now and then one of its own actions (feeding on a star, a flare, a slow precession, a hot spot) */
    /* its actions never start or stop at a stroke: each has its end (act_end), and anything that cuts in (waking, a
     * click, a rush, one asked for) brings that end to HOLE_OUT s away, so it fades out first; what cut in then starts */
    const float HOLE_OUT = .9f;
#define HOLE_START(A, P) do { H->act = (A); H->act_at = c->now; H->act_end = c->now + HOLE_ACT_DUR[A]; H->act_p = (P); H->act_q = rnd(s, -1, 1); H->act_new = 1; } while (0)
#define HOLE_WANT(A, P) do { if (H->act == HA_NONE) HOLE_START(A, P); else { H->pend = (A); H->pend_p = (P); if (H->act_end > c->now + HOLE_OUT) H->act_end = c->now + HOLE_OUT; } } while (0)
    if (s->press_at > H->press_seen + 1e-6) { H->press_seen = s->press_at; H->pdown = c->now; } /* pressed: a click or a hold? */
    if (H->pdown > 0 && s->held) H->pdown = -1; /* (it became a drag) */
    int ready = H->grow > .5f && wake < .1f;
    if (H->pdown != 0 && !s->pressing && !s->held) { /* let go: a quick click drops a star in from that side */
        if (H->pdown > 0 && c->now - H->pdown < .4 && ready && H->act != HA_FEED && H->pend != HA_FEED) HOLE_WANT(HA_FEED, s->press_x < 0 ? -.5f : .5f); /* (not again while one is falling in) */
        H->pdown = 0;
    }
    hold_tick(s, H, dt, c->now, H->pdown > 0 && s->pressing && c->now - H->pdown > .3 && H->grow > .9f && wake < .1f);
    if (ready && s->defend_at > H->defend_seen + 1e-6) { H->defend_seen = s->defend_at; if (H->act != HA_FLARE) HOLE_WANT(HA_FLARE, rnd(s, -1, 1)); } /* rushed at: it flares */
    if (m->hole_want && H->grow > .9f && wake < .1f) { HOLE_WANT(m->hole_want, m->hole_want == HA_FEED ? -.5f : rnd(s, -1, 1)); m->hole_want = 0; } /* asked for (the Settings preview) */
    if (!ready) { H->pend = 0; H->act_next = 0; if (H->act && H->act_end > c->now + HOLE_OUT) H->act_end = c->now + HOLE_OUT; } /* waking: what plays fades out */
    if (H->act != HA_NONE && c->now >= H->act_end) { /* over: what was waiting, or a while to rest */
        H->last_act = H->act; H->act = HA_NONE;
        if (H->pend && ready) { int a = H->pend; H->pend = 0; HOLE_START(a, H->pend_p); }
        else H->act_next = c->now + rnd(s, 6, 14);
    }
    if (H->act == HA_NONE && ready && H->grow > .95f) {
        if (!H->act_next) H->act_next = c->now + rnd(s, 4, 8);
        else if (c->now >= H->act_next) { /* one of those it may play (HOLE_ACTS), at random, never the same twice running */
            int on[HA_N], n = 0;
            for (int a = 1; a < HA_N; a++) if (HOLE_ACTS >> a & 1 && a != HA_LENS && !(HA_RETIRED >> a & 1) && (a != H->last_act || bits_n(HOLE_ACTS & ~(1u << HA_LENS) & ~HA_RETIRED) < 2)) on[n++] = a;
            if (n) { int a = on[(int)(mrand(s) * n) % n]; HOLE_START(a, a == HA_FEED ? -.5f : rnd(s, -1, 1)); } /* (a star on its own: from the left, with the gas) */
            else H->act_next = c->now + 5;
        }
    }
    if (H->act == HA_FEED && H->act_new) tde_start(s, H); /* (only as it begins: never started over while it plays) */
    H->act_new = 0;
#undef HOLE_START
#undef HOLE_WANT
    H->tde.heat *= expf(-dt / 2.2f);
    if (H->act == HA_FEED || (H->tde.n && c->now - H->act_at < 30)) tde_tick(H, dt);
    if (m->hole > 0.01f) { float ax3[3]; bh_hole_axis(s, c->now, ax3); bh2_tick(ax3); } /* (the traced map for this view, a little at a time) */
    H->moving = H->act != HA_NONE || H->grow < .99f || wake > .01f || H->tde.heat > .01f || H->hold.next > c->now - 3 || c->now - H->touched < 8; /* playing something: every refresh; else only its disc turns (panel_rate gives that 24 a second) */
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

/* ---- READY (panel): a reply nobody has seen yet. The core keeps IDLE's look and throws pieces of its own shell at the
 * pointer; each piece flies back and locks into its place again. A hit means "seen": the panel opens the chat box and
 * READY ends once the pieces are home. Ported from the Core Bestiary (web/bestiary.html, READY v7).
 * Pieces live on the stage (see RPiece): shell radii from the core window's centre at the core's usual size, y down,
 * so a piece far from the core never swings when the core hops, grows or turns. Each frame a piece is put where it
 * should be on screen through its triangle's own matrix and carried offset, worked out against the very turn the
 * renderer uses (the view, then the pointer's tilt). */
uint32_t READY_ATKS = (1u << RA_N) - 2; /* all of them */
const char *READY_ATK_NAME[RA_N] = {"", "Missiles", "Forge", "Slingshot", "Spike cones"};
const char *READY_ATK_ABOUT[RA_N] = {"",
    "Three pieces pop out and spin like thrown coins into missiles, then come at the pointer from three sides on smoke trails, aiming where you are going, bent by the core's pull. One you dodge whips round faster each time; it may crash into the core.",
    "One blow at a time, with weight: a clump of pieces lifts out, glowing hotter and turning after the pointer for a charge of random length, takes aim at the last moment and comes down hard on where you are, faster the further it goes, and shatters into its little triangles, which float and fly home. Three blows.",
    "One piece is flung into orbit, a light trail behind it, faster and wider; it tears free toward you and chases you for up to 4 s, aiming where you are going, whipping round faster after each dodge. Lead it into the core and it breaks a hole in the shell.",
    "Spikes grow on your side and the triangles round each tear off as a cone drawn into a needle; it aims, trembles with a glint and shoots point first; one you dodge flies on, still a needle, before it breaks. Three, one after another."};
void ready_play(Session *s, int a) {
    if (a <= RA_NONE || a >= RA_N) return;
    if (s->mo.rt != RT_RESTLESS) motion_play(s, RT_RESTLESS);
    s->mo.atk_want = a;
}

typedef struct { float Mf[9], T[9], wox, woy, sc; } RFrame; /* the full turn the renderer uses, its tilt alone, the core's place and size on the stage */
static void rframe(Ctx *c, RFrame *f) {
    const Session *s = c->s;
    float Ty[9], Tx[9], Y[3] = {0, 1, 0}, X[3] = {1, 0, 0};
    const Motion *m = c->m; /* (the offset and lean as drawn: smoothed while READY, see motion_tick) */
    float wlx = m->sm_on ? m->sm_lx : s->w_lean_x, wly = m->sm_on ? m->sm_ly : s->w_lean_y, wx = m->sm_on ? m->sm_x : s->w_off_x, wy = m->sm_on ? m->sm_y : s->w_off_y;
    float ty = HOVER.tilt * s->look_x + s->lean_x + wlx, tx = HOVER.tilt * s->look_y + s->lean_y + wly; /* as render.c tilts it */
    m3_rot(Ty, Y, ty); m3_rot(Tx, X, -tx); m3_mul(f->T, Tx, Ty);
    m3_mul(f->Mf, f->T, c->M);
    float ws = s->w_scale > 0 ? s->w_scale : 1;
    f->wox = wx * ws; f->woy = wy * ws; f->sc = ws * (m->scale > 0 ? m->scale : 1);
}
static void r_to_view(const RFrame *f, const float *r, float *v) { v[0] = (r[0] - f->wox) / f->sc * CIRCUM; v[1] = (r[1] - f->woy) / f->sc * CIRCUM; v[2] = r[2] * CIRCUM; }
static void r_from_view(const RFrame *f, const float *v, float *r) { r[0] = v[0] / CIRCUM * f->sc + f->wox; r[1] = v[1] / CIRCUM * f->sc + f->woy; r[2] = v[2] / CIRCUM; }
static void tri_centre(const Shape *sh, int ti, float *o) { for (int i = 0; i < 3; i++) o[i] = (sh->v[sh->t[ti][0]][i] + sh->v[sh->t[ti][1]][i] + sh->v[sh->t[ti][2]][i]) / 3; }
/* A piece knocked loose keeps the shell's turn from that moment (M0): placed between as_free and free_end, the helpers
 * below use M0 for the shell's turn, and r_place keeps the piece its own size whatever the core's size does (Bestiary's
 * asFree). Going home it blends from M0 into the live turn (home_turn) and docks exactly. */
static const float *R_FREE_M;
static int R_FREE_ON;
static void as_free(const float *M0) { R_FREE_ON = 1; R_FREE_M = M0; }
static float R_FLAT; /* 0..1, set after as_free: see flat_m */
static void free_end(void) { R_FREE_ON = 0; R_FREE_M = NULL; R_FLAT = 0; }
#define MOF(f) (R_FREE_M ? R_FREE_M : (f)->Mf)
/* How much bigger triangle ti must be to be the shell's mean size (by area): a geodesic shell's triangles differ a little */
static float tri_even(const Shape *sh, int ti) {
    static const Shape *made; static int made_nt; static float k[MAX_T];
    if (made != sh || made_nt != sh->nt) {
        float a[MAX_T], sum = 0;
        for (int t = 0; t < sh->nt; t++) {
            const float *A = sh->v[sh->t[t][0]], *B = sh->v[sh->t[t][1]], *C = sh->v[sh->t[t][2]];
            float u[3] = {B[0] - A[0], B[1] - A[1], B[2] - A[2]}, w[3] = {C[0] - A[0], C[1] - A[1], C[2] - A[2]}, x[3];
            cross3(x, u, w); a[t] = len3(x); sum += a[t];
        }
        for (int t = 0; t < sh->nt; t++) k[t] = a[t] > 0 ? sqrtf(sum / sh->nt / a[t]) : 1;
        made = sh; made_nt = sh->nt;
    }
    return ti >= 0 && ti < sh->nt ? k[ti] : 1;
}
/* A piece broken off by a hit keeps the shell's tilt of that moment, so one seen edge-on was a sliver and every one a
 * different size. R_FLAT of the way, it is turned to face you (its nearer side: the least turn) and sized to the shell's
 * mean triangle; it eases back as it goes home, so it docks exactly. F: that turn (view space, applied after the
 * shell's turn MOF and pre, a turn of its own before it, may be NULL); *gs: its size */
static void flat_m(const RFrame *f, const Shape *sh, int ti, const float *pre, float *F, float *gs) {
    if (R_FLAT <= 0) { memcpy(F, I3, sizeof I3); *gs = 1; return; }
    float n[3], to[3];
    m3_app(n, MOF(f), sh->tn[ti]);
    if (pre) { float t[3]; m3_app(t, pre, n); memcpy(n, t, sizeof n); }
    norm3(n);
    float z = n[2] >= 0 ? 1 : -1;
    for (int i = 0; i < 3; i++) to[i] = n[i] + ((i == 2 ? z : 0) - n[i]) * R_FLAT;
    if (len3(to) < 1e-4f) set3(to, 0, 0, z);
    norm3(to);
    turn_to(F, n, to);
    *gs = 1 + (tri_even(sh, ti) - 1) * R_FLAT;
}
static void home_turn(const RFrame *f, const float *M0, float u, float *out) { /* from M0 to the live turn, u of the way */
    if (u >= 1) { memcpy(out, f->Mf, sizeof(float) * 9); return; }
    for (int i = 0; i < 9; i++) out[i] = M0[i] + (f->Mf[i] - M0[i]) * u;
    m3_ortho(out);
}
static void r_home(Ctx *c, const RFrame *f, int ti, float *out) { float cm[3], v[3]; tri_centre(c->sh, ti, cm); m3_app(v, MOF(f), cm); r_from_view(f, v, out); }
static void r_tip_home(const RFrame *f, const Shape *sh, int tip, float *out) { float v[3]; m3_app(v, MOF(f), sh->v[tip]); r_from_view(f, v, out); }
static void r_normal(const RFrame *f, const Shape *sh, int ti, float *out) { m3_app(out, MOF(f), sh->tn[ti]); }
static void r_off(const RFrame *f, const Shape *sh, int ti, int tip, float *off) { float cm[3], d[3]; tri_centre(sh, ti, cm); for (int i = 0; i < 3; i++) d[i] = sh->v[tip][i] - cm[i]; m3_app(off, MOF(f), d); }
static void r_glow(Ctx *c, int ti, float a) { for (int i = 0; i < 3; i++) { int v = c->sh->t[ti][i]; c->m->glow[v] = fmaxf(c->m->glow[v], a); } }
/* triangle ti turned by R (view, may scale) with its centre at view point atv (about the core, as r_to_view). It is
 * handed to the renderer in the stage's frame (see Motion.loose): its centre on the stage and its shape turned into
 * view space, the core's size in it unless it is loose (as_free: its own size), so nothing the core does after this
 * (hop, lean, grow, turn) drags or bends it */
static void r_place(Ctx *c, const RFrame *f, int ti, const float *atv, const float *R0) {
    Motion *m = c->m;
    float st[3], B[9];
    r_from_view(f, atv, st);
    m3_mul(B, R0, MOF(f));
    if (!R_FREE_ON) m3_scale(B, f->sc);
    for (int i = 0; i < 3; i++) m->tt[ti][i] = st[i] * CIRCUM;
    m->tt_on = 1;
    m->tmi[ti] = (unsigned char)tm_add(m, B);
    m->loose[ti] = R_FREE_ON ? 2 : 1; m->loose_on = 1; /* (2: loose, its own plain shape; 1: still the shell's, spikes and all) */
}
/* a squeeze across axis ax (view) by a, longer along it, so a dart's point gets sharp */
static void sharpen_m(float *S, const float *ax, float a) {
    float w = 1 - .42f * a, h = 1 + .35f * a, d = h - w;
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) S[i * 3 + j] = (i == j ? w : 0) + d * ax[i] * ax[j];
}
static void r_sharp(Ctx *c, int ti, int tip, float k) { if (k <= 0) return; c->m->sharp_on = 1; c->m->sharp_v[ti] = (signed char)tip; c->m->sharp_k[ti] = k; }
/* how far a dart's tip is from its centre, on the stage */
static float r_dart_len(const RFrame *f, const Shape *sh, int ti, int tip, float k, float s, float slim) { float off[3], F[9], gs; r_off(f, sh, ti, tip, off); flat_m(f, sh, ti, NULL, F, &gs); return len3(off) / CIRCUM * (R_FREE_ON ? 1 : f->sc) * s * gs * (1 + .35f * slim) * (1 + .75f * k); } /* (a loose piece keeps its own size) */
/* a dart centred at cen, pointing along dir, drawn out by k, sized s, slimmed by slim */
static void r_dart(Ctx *c, const RFrame *f, int ti, int tip, const float *cen, const float *dir, float k, float s, float slim) {
    float off[3], o[3], d[3] = {dir[0], dir[1], dir[2]}, R[9], cv[3], F[9], gs;
    r_off(f, c->sh, ti, tip, off); flat_m(f, c->sh, ti, NULL, F, &gs);
    m3_app(o, F, off); norm3(o); norm3(d);
    turn_to(R, o, d); m3_scale(R, s * gs);
    if (slim > 0) { float S[9]; sharpen_m(S, d, slim); m3_mul(R, S, R); }
    m3_mul(R, R, F);
    r_to_view(f, cen, cv);
    r_place(c, f, ti, cv, R);
    r_sharp(c, ti, tip, k);
}
static void r_dart_tip(Ctx *c, const RFrame *f, int ti, int tip, const float *tp, const float *dir, float k, float s, float slim) { /* the same, by its tip */
    float d[3] = {dir[0], dir[1], dir[2]}, cen[3]; norm3(d);
    float l = r_dart_len(f, c->sh, ti, tip, k, s, slim);
    for (int i = 0; i < 3; i++) cen[i] = tp[i] - d[i] * l;
    r_dart(c, f, ti, tip, cen, d, k, s, slim);
}
/* a spark at a stage point, moving at a stage velocity (shell radii a second) */
static void r_spark(Ctx *c, const RFrame *f, const float *p, const float *v, float life, float drag, float g, float size, int kind, int hot, Col col) {
    (void)f;
    Motion *m = c->m;
    int slot = -1;
    for (int i = 0; i < MAX_SPARK; i++) if (m->spark[i].life <= 0) { slot = i; break; }
    if (slot < 0) return;
    spark(c->s, I3, p, v, life, drag, g, size, kind, hot, col);
    m->spark[slot].stage = 1; /* in the stage's frame, as the pieces: the core moving never drags it */
}
static int r_tip_toward(const RFrame *f, const Shape *sh, int ti, const float *dir) {
    float cm[3], cv[3]; tri_centre(sh, ti, cm); m3_app(cv, MOF(f), cm);
    int best = sh->t[ti][0]; float bd = -9;
    for (int i = 0; i < 3; i++) { int v = sh->t[ti][i]; float p[3], d[3]; m3_app(p, MOF(f), sh->v[v]); for (int j = 0; j < 3; j++) d[j] = p[j] - cv[j]; norm3(d); if (dot3(d, dir) > bd) { bd = dot3(d, dir); best = v; } }
    return best;
}
static float wrap_a(float a) { return atan2f(sinf(a), cosf(a)); }
/* n triangles on the side facing the pointer T, spread out: each next one as far as it can be from those taken
 * (Bestiary's pickFacing). ok (may be NULL) says which may be taken. Returns how many it found (into out). */
static int r_pick_facing(Ctx *c, const RFrame *f, const float *T, int n, int (*ok)(Ctx *, int), float back, float face, int *out) {
    const Shape *sh = c->sh;
    float d2[3] = {T[0] - f->wox, T[1] - f->woy, .45f};
    norm3(d2);
    int pool[MAX_T], np = 0, near[MAX_T], nn = 0, got = 0;
    float key[MAX_T];
    for (int ti = 0; ti < sh->nt; ti++) {
        float nv[3]; r_normal(f, sh, ti, nv);
        if (nv[2] <= back || (ok && !ok(c, ti))) continue;
        key[ti] = dot3(nv, d2);
        pool[np++] = ti;
        if (key[ti] > face) near[nn++] = ti;
    }
    if (nn >= n) { memcpy(pool, near, sizeof(int) * (size_t)nn); np = nn; }
    for (int i = 0; i < np; i++) for (int j = i + 1; j < np; j++) if (key[pool[j]] > key[pool[i]]) { int t = pool[i]; pool[i] = pool[j]; pool[j] = t; }
    if (!np) return 0;
    out[got++] = pool[0];
    while (got < n && got < np) {
        int best = -1; float bd = -1;
        for (int k = 0; k < np; k++) {
            int ti = pool[k], taken = 0;
            for (int j = 0; j < got; j++) if (out[j] == ti) taken = 1;
            if (taken) continue;
            float d = 9;
            for (int j = 0; j < got; j++) d = fminf(d, 1 - dot3(sh->tn[out[j]], sh->tn[ti]));
            d += .35f * (1 + key[ti]) * .1f + mrand(c->s) * .01f;
            if (d > bd) { bd = d; best = ti; }
        }
        if (best < 0) break;
        out[got++] = best;
    }
    return got;
}

/* after a hit: the piece flies on along its flight, slowing and tumbling, eases back from a dart into a plain
 * triangle, floats a moment, bobbing, and only then goes home. From the hit on it is loose (fr): it keeps the shell's
 * turn of that moment, so the core turning, leaning or hopping never swings it round (Bestiary's knockOn) */
#define R_COAST .45f
#define R_FLOAT .5f
#define R_SHAPE .15f
#define R_FLATTEN .3f /* how long a broken-off piece takes to turn to face you */
static void knock_on(Ctx *c, const RFrame *f, ReadyRS *R, RPiece *p, const float *vel) {
    float o[3] = {p->pos[0] - f->wox, p->pos[1] - f->woy, 0}, sp = hypotf(vel[0], vel[1]);
    norm3(o);
    if (sp < 1e-4f) sp = 1e-4f;
    float d[3] = {vel[0] / sp + .9f * o[0], vel[1] / sp + .9f * o[1], 0}; norm3(d); /* leaning away from the core */
    if (!p->fr) { p->fr = 1; memcpy(p->M0, f->Mf, sizeof p->M0); }
    p->st = RP_COAST; p->t0 = R->rt;
    for (int i = 0; i < 3; i++) p->vel[i] = d[i] * sp;
    p->ang = atan2f(p->dir[1], p->dir[0]); p->w = (mrand(c->s) < .5f ? 1 : -1) * fminf(9, 1.2f * sp);
    p->k0 = p->k; p->s0 = p->s > 0 ? p->s : 1; p->slim0 = p->slim; p->drag = rnd(c->s, 3.5f, 7); p->flat = 0;
}
static void let_go(RPiece *p, float t, float arc) { p->st = RP_BACK; p->t0 = t; memcpy(p->fpos, p->pos, sizeof p->fpos); memcpy(p->fdir, p->dir, sizeof p->fdir); p->fk = p->k; p->fs = p->s > 0 ? p->s : 1; p->farc = arc; }
static void coast_on(Ctx *c, const RFrame *f, ReadyRS *R, RPiece *p) {
    float e = R->rt - p->t0, dt = c->dt;
    if (p->st == RP_COAST) {
        float u = io3(clampf(e / R_SHAPE, 0, 1)), k = expf(-dt * p->drag);
        for (int i = 0; i < 2; i++) { p->vel[i] *= k; p->pos[i] += p->vel[i] * dt; }
        p->ang += p->w * dt; p->w *= expf(-dt * 2.5f);
        p->k = p->k0 * (1 - u); p->s = p->s0 + (1 - p->s0) * u; p->slim = p->slim0 * (1 - u);
        p->flat = io3(clampf(e / R_FLATTEN, 0, 1)); p->pos[2] *= expf(-dt * 10); /* turning to face you, level with the others */
        if (e >= R_COAST) { p->st = RP_FLOAT; p->t0 = R->rt; p->flat = 1; memcpy(p->base, p->pos, sizeof p->base); }
    } else {
        float v = clampf(e / R_FLOAT, 0, 1);
        p->ang += p->w * dt; p->w = (p->w < 0 ? -1 : 1) * fmaxf(.8f, fabsf(p->w) * expf(-dt * 2));
        p->pos[0] = p->base[0] + .015f * sinf(v * TAU); p->pos[1] = p->base[1] - .05f * sinf(v * PI_F); /* a little lift, a sway */
        if (v >= 1) { set3(p->dir, cosf(p->ang), sinf(p->ang), 0); let_go(p, R->rt, .6f); return; }
    }
    set3(p->dir, cosf(p->ang), sinf(p->ang), 0);
    if (p->fr) { as_free(p->M0); R_FLAT = p->flat; }
    r_dart_tip(c, f, p->ti, p->tip, p->pos, p->dir, p->k, p->s, p->slim);
    free_end();
    r_glow(c, p->ti, .35f + .4f * (p->k0 > 0 ? p->k / p->k0 : 0));
}
/* a triangle by its tip at stage point tp, pointing along dir, drawn out by k, sized s (Bestiary's placeTip) */
static void r_place_tip(Ctx *c, const RFrame *f, int ti, int tip, const float *tp, const float *dir, float k, float s) {
    float off[3], o[3], d[3] = {dir[0], dir[1], dir[2]}, R9[9], Ro[3], tv[3], cv[3], F[9], gs;
    r_off(f, c->sh, ti, tip, off); flat_m(f, c->sh, ti, NULL, F, &gs);
    m3_app(o, F, off); norm3(o); norm3(d);
    turn_to(R9, o, d); m3_scale(R9, s * gs); m3_mul(R9, R9, F);
    m3_app(Ro, R9, off);
    r_to_view(f, tp, tv);
    float ks = R_FREE_ON && f->sc > 0 ? 1 / f->sc : 1; /* (r_place keeps a loose piece its own size) */
    for (int i = 0; i < 3; i++) cv[i] = tv[i] - Ro[i] * ks * (1 + .75f * k);
    r_place(c, f, ti, cv, R9);
    r_sharp(c, ti, tip, k);
}
/* home again: from where it was let go back into its place, turning back to its own heading as it lands; a loose one
 * blends from its own turn into the shell's (Bestiary's tipHomeward) */
static void homeward(Ctx *c, const RFrame *f, ReadyRS *R, RPiece *p) {
    float u = clampf((R->rt - p->t0) / .8f, 0, 1);
    if (u >= 1) { if (!p->landed) { p->landed = 1; r_glow(c, p->ti, .9f); } p->st = RP_HOME; p->fr = 0; p->flat = 0; return; }
    float e = io3(u), home[3], off[3], fd[3] = {p->fdir[0], p->fdir[1], p->fdir[2]}, dir[3], pos[3];
    r_tip_home(f, c->sh, p->tip, home); /* where it docks, and the heading it docks with: the shell now */
    r_off(f, c->sh, p->ti, p->tip, off); norm3(off); norm3(fd);
    for (int i = 0; i < 3; i++) { pos[i] = p->fpos[i] + (home[i] - p->fpos[i]) * e; dir[i] = fd[i] + (off[i] - fd[i]) * e; }
    pos[1] -= .3f * sinf(e * PI_F) * p->farc;
    norm3(dir);
    memcpy(p->pos, pos, sizeof pos);
    float Mh[9];
    if (p->fr) { home_turn(f, p->M0, e, Mh); as_free(Mh); R_FLAT = p->flat * (1 - e); } /* (back to its own tilt and size as it docks) */
    r_place_tip(c, f, p->ti, p->tip, pos, dir, p->fk * (1 - e), 1 + (p->fs - 1) * (1 - e));
    free_end();
}
/* the core's feelings while it attacks, shared by every attack (Bestiary's feel / feelings): fixed curves, nothing overshoots */
static void feel(ReadyRS *R, int kind, const float *dir, float amt) {
    int k = 0;
    for (int i = 0; i < RF_MAX; i++) if (!R->fx[i].kind) { k = i; break; } else if (R->fx[i].at < R->fx[k].at) k = i; /* (full: the oldest goes) */
    R->fx[k].kind = kind; R->fx[k].at = R->t; R->fx[k].amt = amt;
    set3(R->fx[k].dir, dir ? dir[0] : 0, dir ? dir[1] : 0, dir ? dir[2] : 0);
}
static float pulse_c(float u) { return u <= 0 || u >= 1 ? 0 : u < .14f ? out3(u / .14f) : 1 - io3((u - .14f) / .86f); }
static void feelings(Ctx *c, ReadyRS *R) {
    Session *s = c->s;
    Motion *m = c->m;
    for (int i = 0; i < RF_MAX; i++) {
        RFeel *q = &R->fx[i];
        if (!q->kind) continue;
        float e = R->t - q->at;
        if (q->kind == RF_RECOIL) { /* the shell recoils, a ripple runs out from the blow (dir: the struck triangle's normal, model space) */
            for (int h = 0; h < m->ng; h++) { float d = acosf(clampf(dot3(m->gn[h], q->dir), -1, 1)); c->w->grp[h] += .09f * q->amt * bump((e - d * .22f) / .25f) * (1 - d / PI_F) / wof(m, h); }
            c->w->bright *= 1 + .9f * q->amt * expf(-e * 8); c->w->scale *= 1 - .045f * q->amt * expf(-e * 12);
            if (e >= 1) q->kind = 0;
        } else if (q->kind == RF_LUNGE) { /* the throw: it leans after what it threw and eases back */
            float p = pulse_c(e / .55f) * q->amt;
            s->w_off_x += q->dir[0] * .15f * p; s->w_off_y += q->dir[1] * .15f * p; c->w->scale *= 1 + .05f * p;
            s->w_lean_x += q->dir[0] * .25f * p; s->w_lean_y += q->dir[1] * .25f * p;
            if (e >= .55f) q->kind = 0;
        } else { /* a cheer: a small bright hop and a quick happy spin */
            float b = sinf(PI_F * clampf(e / .45f, 0, 1)) * q->amt;
            s->w_off_y -= .07f * b; c->w->scale *= 1 + .05f * b; c->w->bright *= 1 + .35f * b; c->w->rate += absrate(c->shown, 2 * b);
            if (e >= .45f) q->kind = 0;
        }
    }
}
/* winding up, and wanting you to look: the core runs to NEEDS YOU's orange and gets excited; it hops on the spot, a
 * little toward you, stretched in the air and squashed as it lands, faster as the charge builds; it gazes at the
 * pointer, spins up, and its patches ripple like waving hands (Bestiary's charge). Returns the colour's pull. */
static float r_charge(Ctx *c, float ch, const float *T) {
    if (!(ch > 0)) return 0;
    ch = fminf(1, ch);
    Session *s = c->s;
    float d[2] = {T[0], T[1]}, l = hypotf(d[0], d[1]);
    if (l > 1e-4f) { d[0] /= l; d[1] /= l; }
    float ph = fmodf((float)c->tt * (1.5f + 1.6f * ch), 1), air = ph < .6f ? bump(ph / .6f) : 0, land = ph >= .6f ? bump((ph - .6f) / .4f) : 0;
    s->w_off_y -= .11f * ch * air; s->w_off_x += d[0] * .05f * ch * air; s->w_off_y += d[1] * .03f * ch * air; s->w_tall += ch * (.07f * air - .09f * land);
    s->w_lx += d[0] * ch; s->w_ly += d[1] * ch; s->w_lean_x += d[0] * .2f * ch; s->w_lean_y += d[1] * .2f * ch;
    c->w->rate += absrate(c->shown, 1.3f * ch); c->w->bright *= 1 + .25f * ch * air;
    for (int g = 0; g < c->m->ng; g++) c->w->grp[g] += .05f * ch * fmaxf(0, sinf((float)c->tt * 9 - g * 1.7f));
    return ch;
}
/* a blow on the pointer (Bestiary's atkHit): a flash, a white flash, hot sparks along the blow, a burst of impact lines
 * and a shock ring at the pointer, and the core is pleased with itself. It counts as a hit (hit_at). No hit-stop: the
 * piece that struck flies on in real time anyway, so freezing the attack's clock only froze everything else in it (the
 * other missiles, the next cone, the core's own cheer) for a moment, which read as a stall. */
static void atk_hit(Ctx *c, const RFrame *f, ReadyRS *R, const float *T, const float *dir, Col col, float size, float ringa, int heavy) {
    float z[3] = {0, 0, 0}, l = hypotf(dir[0], dir[1]), d0[2] = {l > 0 ? dir[0] / l : 1, l > 0 ? dir[1] / l : 0};
    r_spark(c, f, T, z, heavy ? .42f : .3f, 0, 0, size, SK_FLARE, 0, col);
    r_spark(c, f, T, z, .1f, 0, 0, size * .45f, SK_FLARE, 0, RGB255(255, 255, 255));
    for (int i = 0, n = heavy ? 10 : 12; i < n; i++) {
        float d[3] = {d0[0] + rnd(c->s, -.9f, .9f), d0[1] + rnd(c->s, -.9f, .9f), rnd(c->s, -.3f, .3f)}; norm3(d);
        float v = rnd(c->s, 1.2f, 3);
        for (int k = 0; k < 3; k++) d[k] *= v;
        r_spark(c, f, T, d, rnd(c->s, .22f, .45f), 2.5f, 0, rnd(c->s, .9f, 1.5f), SK_STREAK, 1, col);
    }
    Motion *m = c->m;
    int k = 0;
    for (int i = 1; i < 4; i++) if (m->imp[i].at < m->imp[k].at) k = i;
    m->imp[k].at = c->now; m->imp[k].x = T[0]; m->imp[k].y = T[1]; m->imp[k].dx = d0[0]; m->imp[k].dy = d0[1];
    m->imp[k].size = size; m->imp[k].ring = ringa; m->imp[k].col = col;
    feel(R, RF_CHEER, NULL, clampf(size / 70, .35f, 1));
    m->hit_at = c->now;
}
/* the shell's shards (Bestiary's shardFan / shardMove / shardPlace): triangles blown out in a fan round the blow's
 * direction a0, leaning away from the core; each flies on, eases into a plain triangle, floats, and goes home in turn */
static void shard_fan(Ctx *c, const RFrame *f, ReadyRS *R, const int *tris, const float (*cs)[3], int n, const float *at, float a0, float wide, float lo, float hi, const float *ax, const float *d0, float s0) {
    float o[3] = {at[0] - f->wox, at[1] - f->woy, 0}; norm3(o);
    a0 = atan2f(sinf(a0) + .9f * o[1], cosf(a0) + .9f * o[0]); /* leaning away from the core */
    int ord[RS_MAX], order[RS_MAX];
    if (n > RS_MAX) n = RS_MAX;
    for (int i = 0; i < n; i++) ord[i] = order[i] = i;
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) { /* round the fan, in order of their angle from the blow */
        float ai = wrap_a(atan2f(cs[ord[i]][1] - at[1], cs[ord[i]][0] - at[0]) - a0), aj = wrap_a(atan2f(cs[ord[j]][1] - at[1], cs[ord[j]][0] - at[0]) - a0);
        if (aj < ai) { int t = ord[i]; ord[i] = ord[j]; ord[j] = t; }
    }
    for (int i = n - 1; i > 0; i--) { int j = (int)(mrand(c->s) * (i + 1)) % (i + 1), t = order[i]; order[i] = order[j]; order[j] = t; } /* who goes home first: anyone */
    for (int r = 0; r < n; r++) {
        if (R->nsh >= RS_MAX) break;
        int i = ord[r];
        RShard *q = &R->sh[R->nsh++];
        memset(q, 0, sizeof *q);
        float g = a0 + (n > 1 ? ((r + .5f + rnd(c->s, -.45f, .45f)) / n - .5f) * wide * rnd(c->s, .8f, 1.1f) : rnd(c->s, -.3f, .3f));
        float sp = rnd(c->s, lo * .45f, hi * 1.15f) * (mrand(c->s) < .2f ? .5f : 1);
        q->ti = tris[i]; q->i = order[r]; q->wait = rnd(c->s, 0, .22f); q->drag = rnd(c->s, 3.5f, 7); q->bob = rnd(c->s, .4f, 1.6f);
        memcpy(q->base, cs[i], sizeof q->base); set3(q->vel, cosf(g) * sp, sinf(g) * sp, 0);
        q->w = (mrand(c->s) < .5f ? 1 : -1) * rnd(c->s, 2.5f, 10); q->k0 = R->rt; q->s0 = s0; q->ph = rnd(c->s, 0, 6);
        if (ax && d0) { q->has_ax = 1; memcpy(q->ax, ax, sizeof q->ax); memcpy(q->d0, d0, sizeof q->d0); }
        memcpy(q->M0, f->Mf, sizeof q->M0);
    }
}
static int shard_busy(const ReadyRS *R, int ti) { for (int i = 0; i < R->nsh; i++) if (R->sh[i].ti == ti && !R->sh[i].home) return 1; return 0; }
static void shard_move(Ctx *c, RShard *q, float t) {
    float dt = c->dt;
    if (t - q->k0 < R_COAST) { float k = expf(-dt * q->drag); for (int i = 0; i < 2; i++) { q->vel[i] *= k; q->base[i] += q->vel[i] * dt; } }
    q->ang += q->w * dt; q->w = (q->w < 0 ? -1 : 1) * fmaxf(.7f, fabsf(q->w) * expf(-dt * 2.5f));
}
static void r_place_at(Ctx *c, const RFrame *f, int ti, const float *pos, const float *R9) { float cv[3]; r_to_view(f, pos, cv); r_place(c, f, ti, cv, R9); } /* its centre at stage point pos */
static int shard_place(Ctx *c, const RFrame *f, RShard *q, float t) { /* 1 once it is home */
    float k = t - q->k0, Z[3] = {0, 0, 1}, Rz[9], R9[9];
    if (k < R_COAST + R_FLOAT) {
        float su = io3(clampf(k / R_SHAPE, 0, 1)), s = 1 + (q->s0 - 1) * (1 - su), fl = clampf((k - R_COAST) / R_FLOAT, 0, 1);
        float fa = io3(clampf(k / R_FLATTEN, 0, 1)); /* turning to face you, level with the others, the mean size */
        float pos[3] = {q->base[0] + .012f * q->bob * sinf(fl * TAU + q->ph), q->base[1] - .05f * q->bob * sinf(fl * PI_F), q->base[2] * (1 - fa)}, B[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, F[9], gs;
        if (q->has_ax && su < 1) { float to[3]; for (int i = 0; i < 3; i++) to[i] = q->d0[i] + (q->ax[i] - q->d0[i]) * su; norm3(to); turn_to(B, q->ax, to); }
        as_free(q->M0); R_FLAT = fa;
        flat_m(f, c->sh, q->ti, B, F, &gs);
        m3_mul(B, F, B); m3_rot(Rz, Z, q->ang); m3_mul(R9, Rz, B); m3_scale(R9, s * gs);
        r_place_at(c, f, q->ti, pos, R9); free_end();
        r_glow(c, q->ti, .25f + .5f * fmaxf(0, 1 - k / R_COAST));
        return 0;
    }
    float v = io3(clampf((k - R_COAST - R_FLOAT - q->i * .05f - q->wait) / .55f, 0, 1)), hm[3];
    r_home(c, f, q->ti, hm);
    if (v >= 1) { if (!q->home) { q->home = 1; r_glow(c, q->ti, .9f); } return 1; }
    if (!q->ctl_set) { q->ctl_set = 1; memcpy(q->from, q->base, sizeof q->from); q->from[2] = 0; q->a0 = wrap_a(q->ang); for (int i = 0; i < 3; i++) q->ctl[i] = (q->from[i] + hm[i]) * .5f; q->ctl[1] -= .25f; }
    q->ang = q->a0 * (1 - v);
    float w0 = (1 - v) * (1 - v), w1 = 2 * v * (1 - v), w2 = v * v, pos[3], Mh[9], F[9], gs;
    for (int i = 0; i < 3; i++) pos[i] = q->from[i] * w0 + q->ctl[i] * w1 + hm[i] * w2;
    home_turn(f, q->M0, v, Mh); as_free(Mh); R_FLAT = 1 - v; /* back to its own tilt and size as it docks */
    flat_m(f, c->sh, q->ti, NULL, F, &gs);
    m3_rot(Rz, Z, q->ang); m3_mul(R9, Rz, F); m3_scale(R9, gs);
    r_place_at(c, f, q->ti, pos, R9); free_end();
    r_glow(c, q->ti, .25f);
    return 0;
}
/* a missile pulled into the core (Bestiary's coreCrash): a blast at I on the shell's edge (In: out from the core there),
 * and the shell breaks there, the missile and the 7 triangles nearest the blast blown out as shards */
static void core_crash(Ctx *c, const RFrame *f, ReadyRS *R, RPiece *p, const float *I, const float *In) {
    const Shape *sh = c->sh;
    int near[7], nn = 0;
    float best[7];
    for (int ti = 0; ti < sh->nt; ti++) {
        int busy = shard_busy(R, ti);
        for (int j = 0; j < R->n && !busy; j++) if (R->p[j].ti == ti && R->p[j].st != RP_HOME) busy = 1;
        float nv[3], hm[3];
        r_normal(f, sh, ti, nv);
        if (busy || nv[2] <= -.1f) continue;
        r_home(c, f, ti, hm);
        float d = hypotf(hm[0] - I[0], hm[1] - I[1]);
        int at = nn < 7 ? nn++ : (d < best[6] ? 6 : -1);
        if (at < 0) continue;
        near[at] = ti; best[at] = d;
        for (int j = at; j > 0 && best[j] < best[j - 1]; j--) { float t = best[j]; best[j] = best[j - 1]; best[j - 1] = t; int u = near[j]; near[j] = near[j - 1]; near[j - 1] = u; }
    }
    int tris[8]; float cs[8][3];
    tris[0] = p->ti; memcpy(cs[0], p->pos, sizeof cs[0]);
    for (int j = 0; j < nn; j++) { tris[j + 1] = near[j]; r_home(c, f, near[j], cs[j + 1]); }
    float a0 = atan2f(In[1], In[0]), z[3] = {0, 0, 0};
    r_spark(c, f, I, z, .4f, 0, 0, 76, SK_FLARE, 0, RGB255(255, 226, 190));
    r_spark(c, f, I, z, .12f, 0, 0, 34, SK_FLARE, 0, RGB255(255, 255, 255));
    for (int i = 0; i < 26; i++) { float g = a0 + rnd(c->s, -1.6f, 1.6f), v = rnd(c->s, 1.2f, 4), vel[3] = {cosf(g) * v, sinf(g) * v, rnd(c->s, -.6f, .6f)}; r_spark(c, f, I, vel, rnd(c->s, .3f, .8f), 2.2f, 1.2f, rnd(c->s, 1, 2), SK_STREAK, 1, RGB255(255, 196, 130)); }
    for (int i = 0; i < 10; i++) { float g = rnd(c->s, 0, TAU), v = rnd(c->s, .4f, 1.2f), vel[3] = {cosf(g) * v, sinf(g) * v, rnd(c->s, -.3f, .3f)}; r_spark(c, f, I, vel, rnd(c->s, .6f, 1.1f), 1.5f, -.2f, rnd(c->s, 1.2f, 2), SK_MOTE, 0, RGB255(150, 160, 185)); } /* smoke */
    flash(c->m, sh->nv, .35f); ring(c->m, c->now, .6f);
    { float ax[3] = {-In[1], In[0], 0}; jerk(c->s, ax, .08f, c->now); }
    feel(R, RF_RECOIL, sh->tn[nn ? near[0] : p->ti], 1);
    p->st = RP_BROKE;
    shard_fan(c, f, R, tris, (const float (*)[3])cs, nn + 1, I, a0, 3.4f, 6, 10, NULL, NULL, 1);
}

/* 1. missiles: three triangles on your side pop out, clear of the shell, and spin like thrown coins; as the spin
 * slows each one is drawn out long and slim into a missile, and the spin runs out with its nose on the pointer. A
 * beat, then it fires: it curls off to one side on a smoke trail and dives nose first. The core's pull bends every
 * flight; until it has been dodged once the core is solid to it, after that one pulled in crashes into the shell. */
static void missiles_start(Ctx *c, const RFrame *f, ReadyRS *R, const float *T) {
    const Shape *sh = c->sh;
    float dir[3] = {T[0] - f->wox, T[1] - f->woy, 0}; norm3(dir);
    int pool[MAX_T], n = 0;
    float side[MAX_T];
    for (int ti = 0; ti < sh->nt; ti++) { float nv[3]; r_normal(f, sh, ti, nv); float l = hypotf(nv[0], nv[1]); side[ti] = l > 1e-6f ? (nv[0] * dir[0] + nv[1] * dir[1]) / l : 0; if (nv[2] > -.2f && side[ti] > .35f) pool[n++] = ti; }
    if (n < 6) { /* the pointer right over the core: those facing it most */
        n = 0;
        for (int ti = 0; ti < sh->nt; ti++) pool[n++] = ti;
        for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) if (side[pool[j]] > side[pool[i]]) { int t = pool[i]; pool[i] = pool[j]; pool[j] = t; }
        if (n > 12) n = 12;
    }
    for (int i = n - 1; i > 0; i--) { int j = (int)(mrand(c->s) * (i + 1)) % (i + 1), t = pool[i]; pool[i] = pool[j]; pool[j] = t; }
    R->n = 0;
    for (int k = 0; k < n && R->n < 3; k++) {
        int ti = pool[k], ok = 1;
        for (int j = 0; j < R->n; j++) if (R->p[j].ti == ti || sh->tnb[R->p[j].ti][0] == ti || sh->tnb[R->p[j].ti][1] == ti || sh->tnb[R->p[j].ti][2] == ti) ok = 0;
        if (!ok) continue;
        RPiece *p = &R->p[R->n];
        memset(p, 0, sizeof *p);
        p->ti = ti; p->tip = r_tip_toward(f, sh, ti, dir); p->i = R->n; p->t0 = .1f + R->n * .2f; p->st = RP_MORPH; p->s = 1;
        p->spin = (mrand(c->s) < .5f ? 1 : -1) * rnd(c->s, 2.6f, 3.4f); p->dmin = 1e9f;
        R->n++;
    }
}
static void missiles_run(Ctx *c, const RFrame *f, ReadyRS *R, const float *T, float t, float dt) {
    const float MORPH = .8f, AIM = .12f, core[2] = {f->wox, f->woy};
    float T0[3] = {T[0], T[1], 0};
    { int morph = 0, live = 0; /* the lock-on round the pointer while any is turning into a missile or flying; the core winding up while any is turning */
      for (int ip = 0; ip < R->n; ip++) { if (R->p[ip].st == RP_MORPH) morph = 1; if (R->p[ip].st == RP_MORPH || R->p[ip].st == RP_FLY) live = 1; }
      if (live) c->m->lock_on = fmaxf(c->m->lock_on, clampf((t - .5f) / .6f, 0, 1));
      if (morph && R->n) R->charge = fmaxf(R->charge, .3f + .7f * io3(clampf(t / (R->p[0].t0 + MORPH), 0, 1)) * (R->p[0].st == RP_MORPH ? 1 : .7f)); }
    for (int ip = 0; ip < R->n; ip++) {
        RPiece *p = &R->p[ip];
        if (p->st == RP_MORPH) {
            float e = t - p->t0;
            if (e < 0) continue;
            float u = clampf(e / MORPH, 0, 1), nv[3], hm[3], aw[3] = {T0[0] - core[0], T0[1] - core[1], 0};
            r_normal(f, c->sh, p->ti, nv); r_home(c, f, p->ti, hm); norm3(aw);
            if (p->out[0] == 0 && p->out[1] == 0) {
                float sx[3];
                if (hypotf(nv[0], nv[1]) > .2f) set3(sx, nv[0], nv[1], 0);
                else set3(sx, -aw[1] + aw[0] * (p->i - 1), aw[0] + aw[1] * (p->i - 1), 0);
                norm3(sx);
                set3(p->out, sx[0] + aw[0] * .35f, sx[1] + aw[1] * .35f, 0); norm3(p->out);
            }
            float g = io3(clampf(e / .5f, 0, 1)), cen[3] = {hm[0] + p->out[0] * 1.25f * f->sc, hm[1] + p->out[1] * 1.25f * f->sc, hm[2] + .1f * g};
            cen[0] = hm[0] + (cen[0] - hm[0]) * g; cen[1] = hm[1] + (cen[1] - hm[1]) * g; /* well clear of the core */
            float aim[3] = {T0[0] - cen[0], T0[1] - cen[1], 0}; norm3(aim);
            float a0 = atan2f(aim[1], aim[0]), th[3], own[3];
            r_tip_home(f, c->sh, p->tip, th);
            for (int i = 0; i < 3; i++) own[i] = th[i] - hm[i];
            norm3(own);
            float spa = a0 + p->spin * TAU * (1 - out3(u)), sd[3] = {cosf(spa), sinf(spa), 0}, dir[3]; /* spinning, slowing, running out on the pointer */
            if (e < .2f) { float w = io3(e / .2f); for (int i = 0; i < 3; i++) dir[i] = own[i] + (sd[i] - own[i]) * w; norm3(dir); }
            else memcpy(dir, sd, sizeof dir);
            float k = io3(clampf((e - .25f) / .65f, 0, 1)), q = e > MORPH ? .01f * sinf(e * 90 + p->i) : 0;
            float cq[3] = {cen[0] + q, cen[1] - q, cen[2]};
            r_dart(c, f, p->ti, p->tip, cq, dir, .9f * k, 1, .7f * k);
            float l = r_dart_len(f, c->sh, p->ti, p->tip, .9f * k, 1, .7f * k);
            for (int i = 0; i < 3; i++) p->pos[i] = cq[i] + dir[i] * l;
            memcpy(p->dir, dir, sizeof p->dir); p->k = .9f * k; p->slim = .7f * k; p->s = 1;
            r_glow(c, p->ti, .4f + .5f * k);
            if (u < .85f && mrand(c->s) < .5f) { float v[3] = {-dir[1] * (p->spin > 0 ? .8f : -.8f), dir[0] * (p->spin > 0 ? .8f : -.8f), 0}; r_spark(c, f, p->pos, v, .2f, 3, 0, 1, SK_MOTE, 0, RGB255(215, 230, 255)); } /* glints off the spin */
            if (e >= MORPH + AIM) { /* fire: they curl off at their own angles, one wide out on the core's far side, one wide
                                    * the other way, the last (it fires last) almost straight: a pincer, so dodging one
                                    * moves you into another */
                p->st = RP_FLY; p->t0 = t;
                float w[3] = {-aim[1], aim[0], 0}, oc[3] = {cen[0] - core[0], cen[1] - core[1], 0}; norm3(oc);
                static const float CURL[3] = {1.35f, -1.35f, .3f};
                float cs = dot3(w, oc) > 0 ? 1 : -1, g = atan2f(aim[1], aim[0]) + CURL[p->i % 3] * cs, v[3] = {cosf(g), sinf(g), 0};
                for (int i = 0; i < 3; i++) p->vel[i] = v[i] * 5.5f;
                float fp[3] = {p->pos[0] - dir[0] * .35f, p->pos[1] - dir[1] * .35f, p->pos[2]}, z[3] = {0, 0, 0};
                r_spark(c, f, fp, z, .2f, 0, 0, 22, SK_FLARE, 0, RGB255(255, 236, 210));
                { float ld[3] = {-aim[0], -aim[1], 0}; feel(R, RF_LUNGE, ld, .4f); }
            }
        } else if (p->st == RP_FLY) {
            float fly = t - p->t0, to[2] = {T0[0] - p->pos[0], T0[1] - p->pos[1]}, d = hypotf(to[0], to[1]);
            float up = 1 + .15f * (float)(p->nmiss < 5 ? p->nmiss : 5); /* each time you dodge it, it comes back faster and turns harder */
            float sp = fminf(15 * up, hypotf(p->vel[0], p->vel[1]) + (fly < .12f ? 4 : 30) * dt), cur = atan2f(p->vel[1], p->vel[0]);
            float lead = fminf(.6f, d / fmaxf(sp, 1)), want = atan2f(to[1] + R->pv[1] * lead, to[0] + R->pv[0] * lead); /* where you will be when it gets there */
            float pr[2] = {p->pos[0] - core[0], p->pos[1] - core[1]}, hx = cosf(cur), hy = sinf(cur);
            float ahead = -(pr[0] * hx + pr[1] * hy), miss = fabsf(pr[0] * hy - pr[1] * hx);
            if (!p->dodged && ahead > 0 && ahead < d + .5f && miss < 1.35f) { /* the core in the way: round it, on the side it is already on */
                float side = (pr[0] * hy - pr[1] * hx) > 0 ? -1 : 1;
                want = cur + side * fminf(1.2f, (1.35f - miss) * 2.5f);
            }
            /* it turns no faster than this, so it can be dodged; past you it whips round in a tight turn and comes back
             * (p->whip), until it points at you again */
            if (p->whip && fabsf(wrap_a(want - cur)) < .3f) p->whip = 0;
            float steer = (p->whip ? 16 : fminf(9, 4 + 17 * fly)) * up * dt, na = cur + clampf(wrap_a(want - cur), -steer, steer);
            p->vel[0] = cosf(na) * sp; p->vel[1] = sinf(na) * sp;
            { float r = fmaxf(.9f, hypotf(pr[0], pr[1])), g = 40 / (r * r); /* the core's pull: it bends the flight and slings it round */
              p->vel[0] -= pr[0] / r * g * dt; p->vel[1] -= pr[1] / r * g * dt;
              float v = hypotf(p->vel[0], p->vel[1]), vm = 19 * up; if (v > vm) { p->vel[0] *= vm / v; p->vel[1] *= vm / v; } }
            float prev[3] = {p->pos[0], p->pos[1], p->pos[2]};
            p->pos[0] += p->vel[0] * dt; p->pos[1] += p->vel[1] * dt; p->pos[2] *= expf(-dt * 6);
            if (d < p->dmin) p->dmin = d; else if (d > p->dmin + .3f) { if (!p->whip) p->nmiss++; p->dodged = 1; p->whip = 1; p->dmin = d; } /* it came close, missed and flew on: dodged; round it comes */
            float rr = hypotf(p->pos[0] - core[0], p->pos[1] - core[1]), cr = f->sc;
            if (rr < cr && !p->dodged) { /* not dodged yet: the core is solid, it glances off */
                float u[2] = {(p->pos[0] - core[0]) / fmaxf(rr, 1e-4f), (p->pos[1] - core[1]) / fmaxf(rr, 1e-4f)}, vn = p->vel[0] * u[0] + p->vel[1] * u[1];
                p->pos[0] = core[0] + u[0] * cr * 1.02f; p->pos[1] = core[1] + u[1] * cr * 1.02f;
                if (vn < 0) { p->vel[0] -= 1.6f * vn * u[0]; p->vel[1] -= 1.6f * vn * u[1]; }
                rr = cr * 1.02f;
            }
            if (rr < cr) { /* dodged, then pulled into the core: it explodes there and breaks a hole in the shell */
                float lo = 0, hi = 1;
                for (int i = 0; i < 12; i++) { float u = (lo + hi) / 2; if (hypotf(prev[0] + (p->pos[0] - prev[0]) * u - core[0], prev[1] + (p->pos[1] - prev[1]) * u - core[1]) < cr) hi = u; else lo = u; }
                float I[3] = {prev[0] + (p->pos[0] - prev[0]) * lo, prev[1] + (p->pos[1] - prev[1]) * lo, 0}, In[3] = {I[0] - core[0], I[1] - core[1], 0};
                norm3(In);
                core_crash(c, f, R, p, I, In);
                continue;
            }
            set3(p->dir, p->vel[0], p->vel[1], 0); p->k = 1;
            if (fly > 4) { /* chased too long: it bursts, harmlessly, and goes home */
                float z[3] = {0, 0, 0};
                for (int i = 0; i < 12; i++) { float a = rnd(c->s, 0, TAU), v = rnd(c->s, .6f, 1.8f), vel[3] = {cosf(a) * v, sinf(a) * v, rnd(c->s, -.3f, .3f)}; r_spark(c, f, p->pos, vel, rnd(c->s, .3f, .6f), 2.5f, 0, rnd(c->s, 1, 1.6f), SK_MOTE, 0, RGB255(200, 210, 230)); }
                r_spark(c, f, p->pos, z, .18f, 0, 0, 16, SK_FLARE, 0, RGB255(220, 228, 245));
                float kv[3] = {p->vel[0] * .35f, p->vel[1] * .35f, 0};
                knock_on(c, f, R, p, kv);
                continue;
            }
            float sx = p->vel[0] * dt, sy = p->vel[1] * dt, sl = sx * sx + sy * sy, tx0 = T0[0] - prev[0], ty0 = T0[1] - prev[1];
            float sk = sl > 0 ? clampf((tx0 * sx + ty0 * sy) / sl, 0, 1) : 0;
            if (c->s->aim_on && hypotf(tx0 - sx * sk, ty0 - sy * sk) < fmaxf(.28f * f->sc, c->s->aim_hit)) { /* the nose on the pointer (along this frame's whole step): freeze, blast */
                r_dart_tip(c, f, p->ti, p->tip, T0, p->dir, 1, 1, p->slim);
                { float hd[2] = {p->vel[0] / sp, p->vel[1] / sp}; atk_hit(c, f, R, T0, hd, RGB255(255, 236, 214), 58, 1, 0); }
                for (int i = 0; i < 18; i++) { float a = na + PI_F + rnd(c->s, -1.4f, 1.4f), v = rnd(c->s, 1.2f, 3.4f), vel[3] = {cosf(a) * v, sinf(a) * v, rnd(c->s, -.5f, .5f)}; r_spark(c, f, T0, vel, rnd(c->s, .3f, .65f), 2.2f, 1.2f, rnd(c->s, 1, 1.9f), SK_STREAK, 1, RGB255(255, 190, 120)); }
                memcpy(p->pos, T0, sizeof T0);
                float g = na + rnd(c->s, -1.2f, 1.2f), v = rnd(c->s, 6.5f, 8), kv[3] = {cosf(g) * v, sinf(g) * v, 0};
                knock_on(c, f, R, p, kv);
                continue;
            }
            r_dart_tip(c, f, p->ti, p->tip, p->pos, p->dir, 1, 1, p->slim);
            { float v[3] = {rnd(c->s, -.12f, .12f), rnd(c->s, -.12f, .12f), 0}; r_spark(c, f, prev, v, rnd(c->s, .45f, .8f), 1.5f, -.2f, rnd(c->s, 1, 1.5f), SK_MOTE, 0, RGB255(160, 172, 200)); } /* smoke */
            { float v[3] = {-p->vel[0] * .1f, -p->vel[1] * .1f, 0}; r_spark(c, f, prev, v, .14f, 3, 0, 1.2f, SK_STREAK, 1, RGB255(255, 200, 140)); }
        } else if (p->st == RP_COAST || p->st == RP_FLOAT) coast_on(c, f, R, p);
        else if (p->st == RP_BACK) homeward(c, f, R, p);
    }
    for (int i = 0; i < R->nsh; i++) shard_move(c, &R->sh[i], R->rt); /* the broken shell and the missile's own pieces */
    for (int i = 0; i < R->nsh; i++) if (!R->sh[i].home) shard_place(c, f, &R->sh[i], R->rt);
}

/* the other attacks, one file each (Bestiary's ATTACK.<name>, ported): NAME_start(c, f, R, T) picks its pieces into
 * R->p / its own state into R->own; NAME_run(c, f, R, T, t, dt) runs it, t its clock.
 * Building with -DREADY_SOLO and one -DREADY_SOLO_<NAME> leaves the others out (stubs), to check one alone. */
#define READY_STUB(name) static void name##_start(Ctx *c, const RFrame *f, ReadyRS *R, const float *T) { (void)c; (void)f; (void)T; R->n = 0; } \
                         static void name##_run(Ctx *c, const RFrame *f, ReadyRS *R, const float *T, float t, float dt) { (void)c; (void)f; (void)R; (void)T; (void)t; (void)dt; }
#if !defined(READY_SOLO) || defined(READY_SOLO_FORGE)
#include "ready_forge.inc"
#else
READY_STUB(forge)
#endif
#if !defined(READY_SOLO) || defined(READY_SOLO_SLINGSHOT)
#include "ready_slingshot.inc"
#else
READY_STUB(slingshot)
#endif
#if !defined(READY_SOLO) || defined(READY_SOLO_SPIKES)
#include "ready_spikes.inc"
#else
READY_STUB(spikes)
#endif

/* Restless: READY's routine. IDLE's calm turn; every 1.2 to 2.6 s, while the pointer is on its screen, an attack */
static void r_restless(Ctx *c) {
    Session *s = c->s;
    Motion *m = c->m;
    ReadyRS *R = &m->rs.ready;
    RFrame f; rframe(c, &f);
    set_rate(c, .16f);
    float T[3] = {s->aim_x, s->aim_y, 0};
    if (R->next == 0) R->next = c->now + .8;
    if (!R->atk && c->sh->nt) {
        int a = m->atk_want;
        if (!a && s->aim_on && c->now >= R->next && (READY_ATKS & ((1u << RA_N) - 2))) { /* one it may throw, at random */
            int on[RA_N], n = 0;
            for (int k = 1; k < RA_N; k++) if (READY_ATKS >> k & 1) on[n++] = k;
            a = on[(int)(mrand(s) * n) % n];
        }
        m->atk_want = 0;
        if (a) {
            R->atk = a; R->t = R->rt = 0; R->has_last = 0; R->pv[0] = R->pv[1] = 0; R->nsh = 0;
            memset(R->fx, 0, sizeof R->fx); memset(&R->own, 0, sizeof R->own);
            switch (a) {
            case RA_MISSILES: missiles_start(c, &f, R, T); break;
            case RA_FORGE: forge_start(c, &f, R, T); break;
            case RA_SLINGSHOT: slingshot_start(c, &f, R, T); break;
            case RA_SPIKES: spikes_start(c, &f, R, T); break;
            }
        }
    }
    if (!R->atk) { m->rage *= expf(-c->dt * 3); return; }
    m->atk_busy = 1;
    float dt = c->dt;
    R->t += dt; R->rt += c->dt;
    if (c->dt > 0) { /* how the pointer is moving, smoothed */
        if (R->has_last) { float k = 1 - expf(-c->dt * 8); for (int i = 0; i < 2; i++) R->pv[i] += ((T[i] - R->last[i]) / c->dt - R->pv[i]) * k; }
        R->last[0] = T[0]; R->last[1] = T[1]; R->has_last = 1;
    }
    R->charge = 0;
    switch (R->atk) {
    case RA_MISSILES: missiles_run(c, &f, R, T, R->t, dt); break;
    case RA_FORGE: forge_run(c, &f, R, T, R->t, dt); break;
    case RA_SLINGSHOT: slingshot_run(c, &f, R, T, R->t, dt); break;
    case RA_SPIKES: spikes_run(c, &f, R, T, R->t, dt); break;
    }
    feelings(c, R);
    { float ch = r_charge(c, R->charge, T); /* the colour runs to NEEDS YOU's orange while it winds up */
      m->rage += (ch - m->rage) * (1 - expf(-c->dt * (ch > m->rage ? 5 : 3))); }
    int out = 0;
    for (int i = 0; i < R->n; i++) if (R->p[i].st != RP_HOME && R->p[i].st != RP_BROKE) out = 1;
    for (int i = 0; i < R->nsh; i++) if (!R->sh[i].home) out = 1;
    if (!out || R->rt > 14) { R->atk = 0; R->n = 0; R->next = c->now + rnd(s, 1.2f, 2.6f); }
}

/* every routine: name, the state it was made for, how long it keeps DONE on screen, fine patches, run, enter, what it does */
const Routine ROUTINES[RT_N] = {
    [RT_DRIFT] = {"Drift", -1, 0, 0, r_drift, NULL, "A slow, plain turn; the actions do the rest."},
    [RT_ORACLE] = {"Oracle", ST_IDLE, 0, 0, r_oracle, NULL, "Exact and calm; now and then a line of light climbs it. Pointed at, it stirs with calm spikes."},
    [RT_PUZZLE] = {"Puzzle", ST_THINK, 0, 0, r_puzzle, NULL, "One layer rises in separate pieces; both turn like a cube being solved: two quick turns, a halt, three times."},
    [RT_SMOOTH] = {"Smooth puzzle", ST_THINK, 0, 0, r_smooth, NULL, "The puzzle with eased turns that flow into each other; only the halt is still."},
    [RT_LONG] = {"Long spin", ST_THINK, 0, 0, r_long, NULL, "One layer spins three full turns, then the other spins back, while the shell turns slowly."},
    [RT_BLADES] = {"Blades", ST_THINK, 0, 0, r_blades, NULL, "Single triangles float out behind the core and smash across the front in pairs, like a volley of arrows."},
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
    [RT_DORMANT] = {"Dormant", -1, 0, 0, r_dormant, NULL, "Drawn in slightly and barely turning; a faint scan every 6 s. (Retired: never picked, not offered; asleep, it is the black hole.)"},
    [RT_HOLE] = {"Black hole", ST_SLEEP, 0, 0, r_hole, hole_enter, "Gathers power, bursts apart and falls back into a black hole: its disc and the lensed far side traced the way light bends round a real one; then jets, a star torn apart, flares, lensing, precession, hot spots. Asleep only."},
    [RT_BROADCAST] = {"Broadcast", ST_SPEAK, 0, 0, r_broadcast, NULL, "The patches rise and fall with the voice, like the bars of an equaliser."},
    [RT_RECEIVER] = {"Receiver", ST_LISTEN, 0, 0, r_receiver, NULL, "Holds still; the patches facing you open with the microphone level."},
    [RT_DECODING] = {"Decoding", ST_TRANSCRIBE, 0, 0, r_decoding, NULL, "Steps 36 degrees at a time while a scan plane reads the rows."},
    [RT_LOCKED] = {"Locked", ST_STANDBY, 0, 0, r_locked, locked_enter, "Stops square to you, drawn in and still."},
    [RT_CRAZY] = {"Crazy", ST_ERROR, 0, 0, r_crazy, NULL, "Fits of going crazy: spikes out and in, jerks, flicking patches."},
    [RT_RESTLESS] = {"Restless", ST_READY, 0, 0, r_restless, NULL, "Keeps calm, and every second or two throws pieces of its shell at the pointer until you point at it to read the reply. Its attacks are below."},
};

static void routine_enter(Session *s, const Shape *sh, int st, double now) {
    Motion *m = &s->mo;
    /* which routine: one asked for (the Settings preview), else one of the state's at random (MOTION_MAP) */
    int rt = m->want_rt;
    m->want_rt = -1;
    if (rt < 0 || rt >= RT_N) {
        uint32_t ok = MOTION_MAP[st].rt;
        if (st != ST_SLEEP) ok &= ~(1u << RT_HOLE); /* the black hole is for sleep only */
        ok &= ~(1u << RT_DORMANT); /* (retired: asleep, it is the black hole) */
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
static void w_tumble(Ctx *c, float u, float k, int start) { (void)u; (void)k; if (start) { float a[3]; rand_dir(c->s, a); turn(c->s, a, PI_F, 1.3f, 0, c->now); } c->w->rate = 0; }

/* the geometric actions, by W_ name (the soft ones have none: session.c plays them) */
static const WhimFn WHIM_RUN[W_N] = {
    [W_ALIGN] = w_align, [W_STEP] = w_step, [W_UNFOLD] = w_unfold, [W_SCAN] = w_scan, [W_SPIKE] = w_spike, [W_CRAZY] = w_crazy,
    [W_RIPPLE] = w_ripple, [W_ORBIT] = w_orbit, [W_PETAL] = w_petal, [W_CONSTELLATION] = w_constellation, [W_TUMBLE] = w_tumble,
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
/* ---- a state change: the old routine winds down first. What it had out on its last frame (triangles turned or carried
 * off, blades, see-through ones, the squash, the black hole, the inner shell, a scan) eases back home over CLOSE_T while
 * nothing new is asked, so lifted patches and spikes settle too; the old state's colour stays meanwhile. Then the new
 * state's colour comes in and its routine starts. Nothing out: it changes at once. */
#define CLOSE_T .5f
static int motion_out(const Motion *m, const Shape *sh) {
    if (m->ntm || m->tt_on || m->ta_on || m->sharp_on || m->squash > .01f || m->hole > .01f || m->inner > .01f || m->scan_a > .01f) return 1;
    for (int t = 0; t < sh->nt; t++) if (fabsf(m->off[t]) > .03f) return 1;
    for (int v = 0; v < sh->nv; v++) if (fabsf(m->push[v]) > .03f) return 1;
    return 0;
}
static void close_begin(Motion *m, double now) {
    m->closing = 1; m->close_at = now;
    memcpy(m->cl.tmi, m->tmi, sizeof m->cl.tmi); memcpy(m->cl.tmx, m->tmx, sizeof m->cl.tmx); m->cl.ntm = m->ntm;
    memcpy(m->cl.tt, m->tt, sizeof m->cl.tt); m->cl.tt_on = m->tt_on;
    memcpy(m->cl.talpha, m->talpha, sizeof m->cl.talpha); m->cl.ta_on = m->ta_on;
    memcpy(m->cl.sharp_k, m->sharp_k, sizeof m->cl.sharp_k); memcpy(m->cl.sharp_v, m->sharp_v, sizeof m->cl.sharp_v); m->cl.sharp_on = m->sharp_on;
    m->cl.squash = m->squash; memcpy(m->cl.sq_ax, m->sq_ax, sizeof m->cl.sq_ax);
    m->cl.hole = m->hole; m->cl.inner = m->inner; m->cl.scan_h = m->scan_h; m->cl.scan_a = m->scan_a;
}
/* R (may scale) taken u of the way from the identity: its turn part of the way about its own axis, its stretch eased */
static void m3_part(const float *A, float u, float *out) {
    float R[9], Rt[9], S[9], P[9];
    memcpy(R, A, sizeof R); m3_ortho(R);
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) Rt[i * 3 + j] = R[j * 3 + i];
    m3_mul(S, Rt, A); /* the stretch left once the turn is taken out */
    float cs = clampf((R[0] + R[4] + R[8] - 1) / 2, -1, 1), ang = acosf(cs), ax[3] = {R[7] - R[5], R[2] - R[6], R[3] - R[1]}, l = len3(ax);
    if (ang < 1e-4f) memcpy(P, I3, sizeof P);
    else {
        if (l < 1e-6f) { int k = R[0] >= R[4] && R[0] >= R[8] ? 0 : R[4] >= R[8] ? 1 : 2; set3(ax, R[k] + (k == 0), R[3 + k] + (k == 1), R[6 + k] + (k == 2)); norm3(ax); }
        else for (int i = 0; i < 3; i++) ax[i] /= l;
        m3_rot(P, ax, ang * u);
    }
    for (int i = 0; i < 9; i++) S[i] = I3[i] + (S[i] - I3[i]) * u;
    m3_mul(out, P, S);
}
/* one frame of winding down; 1 once it is all home */
static int close_step(Motion *m, const Shape *sh, double now) {
    float e = (float)(now - m->close_at), f = 1 - io3(clampf(e / CLOSE_T, 0, 1));
    if (f > 0) {
        if (m->cl.ntm) { memcpy(m->tmi, m->cl.tmi, sizeof m->tmi); m->ntm = m->cl.ntm; for (int i = 0; i < m->cl.ntm; i++) m3_part(m->cl.tmx[i], f, m->tmx[i]); }
        if (m->cl.tt_on) { m->tt_on = 1; for (int t = 0; t < sh->nt; t++) for (int k = 0; k < 3; k++) m->tt[t][k] = m->cl.tt[t][k] * f; }
        if (m->cl.ta_on) { m->ta_on = 1; for (int t = 0; t < sh->nt; t++) m->talpha[t] = 1 + (m->cl.talpha[t] - 1) * f; }
        if (m->cl.sharp_on) { m->sharp_on = 1; memcpy(m->sharp_v, m->cl.sharp_v, sizeof m->sharp_v); for (int t = 0; t < sh->nt; t++) m->sharp_k[t] = m->cl.sharp_k[t] * f; }
        m->squash = m->cl.squash * f; memcpy(m->sq_ax, m->cl.sq_ax, sizeof m->sq_ax);
        m->hole = m->cl.hole * f; m->inner = m->cl.inner * f; m->scan_h = m->cl.scan_h; m->scan_a = m->cl.scan_a * f;
        return 0;
    }
    if (e < 1.2f) { /* the patches and spikes it lifted, still settling (nothing drives them now) */
        for (int t = 0; t < sh->nt; t++) if (fabsf(m->off[t]) > .03f) return 0;
        for (int v = 0; v < sh->nv; v++) if (fabsf(m->push[v]) > .03f) return 0;
    }
    return 1;
}

void motion_tick(Session *s, const Shape *sh, int shown, double now, float dt) {
    Motion *m = &s->mo;
    if ((sh->nt && m->cut_for != sh->nt) || (!sh->nt && m->cut_for)) { /* a new shape (Settings > Shape): start clean */
        recut(s, sh, 1, 6);
        memset(m->sp, 0, sizeof m->sp); memset(m->push, 0, sizeof m->push); memset(m->off, 0, sizeof m->off); memset(m->ang, 0, sizeof m->ang);
        m->routine = -1;
    }
    if (!sh->nt) m->ng = 0;
    if (m->closing && m->routine == shown) m->closing = 0; /* back to the old state before it wound down: it carries on */
    if (m->routine != shown && !m->closing) {
        /* a state change: wind the old routine down first, if it has anything out (READY's loose pieces see to themselves) */
        if (m->routine >= 0 && m->rt >= 0 && !m->loose_on && motion_out(m, sh)) close_begin(m, now);
        else routine_enter(s, sh, shown, now);
    }
    for (int i = 0; i < sh->nv; i++) m->glow[i] *= expf(-dt * 5);
    float M[9]; view_matrix(s, M);
    Want w; memset(&w, 0, sizeof w);
    w.scale = 1; w.rate = 1; w.bright = 1;
    m->ang_on = 0; m->scan_a = 0; m->inner = 0;
    /* what a routine sets afresh every frame */
    memset(m->tmi, 0, sizeof m->tmi);
    m->ntm = 0; m->tt_on = m->ta_on = m->sharp_on = m->trail_on = 0; m->squash = 0; m->hole = 0;
    memset(m->tt, 0, sizeof m->tt); memset(m->sharp_k, 0, sizeof m->sharp_k);
    memset(m->loose, 0, sizeof m->loose); m->loose_on = m->atk_busy = 0;
    if (m->rt != RT_RESTLESS) m->rage *= expf(-dt * 3);
    m->lock_on = fmaxf(0, m->lock_on - dt * 2.5f);
    { float want = shown == ST_READY ? READY_CAM : 7; /* the camera: see Motion.cam */
      m->cam = m->cam > 0 ? m->cam + (want - m->cam) * (1 - expf(-dt * 4)) : want; }

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
    if (m->closing) { if (close_step(m, sh, now)) { m->closing = 0; routine_enter(s, sh, shown, now); } } /* nothing asked meanwhile */
    else {
        if (m->rt >= 0 && m->rt < RT_N) ROUTINES[m->rt].run(&c);
        whim(&c);
    }
    /* READY: the offset, lean and stretch the routine asks for, as the Bestiary draws them: the offset follows on a
     * critically damped spring, the lean and stretch ease, so a hop, a lunge or a cheer is soft and never shakes */
    { int on = m->rt == RT_RESTLESS && shown == ST_READY;
      if (on && !m->sm_on) { m->sm_x = s->w_off_x; m->sm_y = s->w_off_y; m->sm_vx = m->sm_vy = 0; m->sm_lx = s->w_lean_x; m->sm_ly = s->w_lean_y; m->sm_tall = s->w_tall; }
      if (on || m->sm_on) {
          int n = (int)ceilf(dt / .004f); float h = dt / (float)(n > 0 ? n : 1), K = 160, D = 2 * sqrtf(160);
          for (int i = 0; i < n; i++) {
              m->sm_vx += (K * (s->w_off_x - m->sm_x) - D * m->sm_vx) * h; m->sm_x += m->sm_vx * h;
              m->sm_vy += (K * (s->w_off_y - m->sm_y) - D * m->sm_vy) * h; m->sm_y += m->sm_vy * h;
          }
          float kl = 1 - expf(-dt * 10), kt = 1 - expf(-dt * 12);
          m->sm_lx += (s->w_lean_x - m->sm_lx) * kl; m->sm_ly += (s->w_lean_y - m->sm_ly) * kl; m->sm_tall += (s->w_tall - m->sm_tall) * kt;
          float gap = fabsf(s->w_off_x - m->sm_x) + fabsf(s->w_off_y - m->sm_y) + fabsf(m->sm_vx) + fabsf(m->sm_vy) + fabsf(s->w_lean_x - m->sm_lx) + fabsf(s->w_lean_y - m->sm_ly) + fabsf(s->w_tall - m->sm_tall);
          s->w_off_x = m->sm_x; s->w_off_y = m->sm_y; s->w_lean_x = m->sm_lx; s->w_lean_y = m->sm_ly; s->w_tall = m->sm_tall;
          m->sm_on = on || gap > 1e-3f; /* (after READY it settles first, then lets go) */
      } }

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
    m->rate += ((turns_busy(m) ? 0 : w.rate) - m->rate) * (shown == ST_READY ? 1 : 1 - expf(-dt * 4)); /* (READY: at once, so a cheer's quick spin shows, as in the Bestiary) */
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
    int quiet_hole = m->rt == RT_HOLE && m->hole > .99f && !m->rs.hole.moving; /* only its disc turning: panel_rate gives it 24 a second, session_rate 10 */
    if (!quiet_hole && (m->ntm || m->tt_on || m->ta_on || m->bright != 1)) return 1;
    if (m->trail_on || m->squash > 0) return 1;
    for (int i = 0; i < MAX_SPIKE; i++) if (spike_live(&m->sp[i], now)) return 1;
    for (int i = 0; i < MAX_SPARK; i++) if (m->spark[i].life > 0) return 1;
    for (int i = 0; i < MAX_RING; i++) if (m->ring[i].a > 0) return 1;
    for (int t = 0; t < MAX_T; t++) if (fabsf(m->off[t]) > .002f || m->ang[t] != 0) return 1;
    return 0;
}

int motion_crazy(const Session *s, double now) { return now < s->mo.crazy_until && now >= s->mo.tense_until; }

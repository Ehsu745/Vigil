/* How the shell moves: the "Core Bestiary" design, worked out in an interactive mock first.
 *
 * At rest the shell is an exact geodesic sphere, a calm oracle. Every motion is geometric and never
 * overshoots: snap turns about symmetry axes that speed up and stop dead or ease in and out; irregular
 * patches of triangles that slide out along their normal and lock back (a new random cut each time, so
 * no pattern repeats); layers of triangles turning against the rest; spikes, where one node is pulled far
 * out into a sharp pyramid or in toward the centre (out and in together make the tension); a scan plane
 * and nodes lighting in sequence. Each state runs its own routine (motion_tick, at the bottom).
 *
 * Patches and layers need triangles, so they only happen on the geodesic shells (geo2, geo3); on dodeca
 * and bucky the net stays rigid and only turns, spikes and light remain. */
#include "vigil.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TAU 6.28318531f
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
static float env(float e, float up, float hd, float dn) { /* out fast, hold, back in */
    if (e < 0) return 0;
    if (e < up) return outx(e / up);
    if (e < up + hd) return 1;
    return fmaxf(0, 1 - in3((e - up - hd) / dn));
}
static float hashf(int n) { float x = sinf((float)n * 127.1f) * 43758.5453f; return x - floorf(x); }
static float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void norm3(float *v) { float l = sqrtf(dot3(v, v)); if (l > 1e-9f) { v[0] /= l; v[1] /= l; v[2] /= l; } }
static void cross3(float *r, const float *a, const float *b) {
    float x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    r[0] = x; r[1] = y; r[2] = z;
}

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

void motion_init(Session *s) {
    Motion *m = &s->mo;
    memset(m, 0, sizeof *m);
    m->q[0] = m->q[4] = m->q[8] = 1;
    m->scale = m->rate = 1;
    m->routine = -1;
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

/* ---- the routines, one per state ---------------------------------------------------------------- */
typedef struct { float grp[MAX_T]; float push[MAX_V]; float scale, rate; } Want;

static void routine_enter(Session *s, const Shape *sh, int st, double now) {
    Motion *m = &s->mo;
    m->routine = st; m->r_at = now; m->r_next = now; m->r_flag = 0; m->norder = 0;
    memset(m->clicked, 0, sizeof m->clicked);
    m->fish[0].ntrail = m->fish[1].ntrail = 0;
    m->fish[0].to = m->fish[1].to = -1;
    if (st == ST_WAIT) recut(s, sh, 1, 2); /* the swimmers are made of fine patches */
    else recut(s, sh, 1, 6);
    if (st == ST_STANDBY) { float M[9]; view_matrix(s, M); align_to_viewer(s, sh, corner_facing(s, sh, M), .8f, now); }
}

static void think(Session *s, const Shape *sh, Want *w, double now) { /* lifted layers, turned like a cube being solved */
    Motion *m = &s->mo;
    static const int SEQ[6][2] = {{0, 1}, {1, -1}, {0, 1}, {1, -1}, {0, -2}, {1, 2}}; /* each layer's turns add up to zero */
    const float LIFT = .4f, FOLD = .35f, REST = .7f, HALT = .34f;
    if (!m->r_flag || now - m->r_at > m->r_end + REST) { /* a new round: new cut, new axis, new layers */
        recut(s, sh, 1, 6);
        for (int g = 0; g < m->ng; g++) m->layer[g] = (signed char)(m->gw[g] > 0 ? (mrand(s) < .5f ? 0 : 1) : -1);
        float t0 = LIFT + .15f;
        for (int i = 0; i < 6; i++) { m->seq_d[i] = abs(SEQ[i][1]) > 1 ? .36f : .26f; m->seq_s[i] = t0; t0 += m->seq_d[i] + (i % 2 ? HALT : 0); }
        m->r_end = t0; /* the fold starts here: the tempo of solving a cube, __.__.__. */
        int v = random_corner(s, sh);
        for (int q = 0; q < 3; q++) m->lax[q] = sh->v[v][q] / CIRCUM;
        m->r_at = now; m->r_flag = 1; /* bit 0: set up; bits 1..6: which turns have stopped */
    }
    float e = (float)(now - m->r_at);
    float lift = e < LIFT ? outx(e / LIFT) : e < m->r_end ? 1 : fmaxf(0, 1 - in3((e - m->r_end) / FOLD));
    float ang[2] = {0, 0};
    for (int i = 0; i < 6; i++) {
        float u = clampf((e - m->seq_s[i]) / m->seq_d[i], 0, 1);
        ang[SEQ[i][0]] += SEQ[i][1] * STEP5 * u * u; /* speeds up, then stops dead */
        if (u >= 1 && !(m->r_flag & (2 << i))) {
            m->r_flag |= 2 << i;
            for (int t = 0; t < sh->nt; t++) if (m->layer[m->grp[t]] == SEQ[i][0]) for (int k = 0; k < 3; k++) m->glow[sh->t[t][k]] = fmaxf(m->glow[sh->t[t][k]], .7f);
        }
    }
    for (int g = 0; g < m->ng; g++) if (m->layer[g] >= 0) w->grp[g] += (m->layer[g] ? .19f : .09f) * lift / m->gw[g];
    for (int t = 0; t < sh->nt; t++) { int L = m->layer[m->grp[t]]; m->ang[t] = L >= 0 ? ang[L] : 0; }
    m->ang_on = 1;
    m->inner = .3f; m->inner_rate = -.3f;
    w->rate = .15f;
}

static void work(Session *s, const Shape *sh, Want *w, double now) { /* a turbine belt and pistons */
    Motion *m = &s->mo;
    if (m->r_flag != m->cutid) { /* (re)build for this cut: a belt round the middle, pistons on the caps in firing order */
        int v = random_corner(s, sh);
        for (int q = 0; q < 3; q++) m->lax[q] = sh->v[v][q] / CIRCUM;
        float u0[3], w0[3], Y[3] = {0, 1, 0}, X[3] = {1, 0, 0};
        cross3(u0, m->lax, fabsf(m->lax[1]) < .9f ? Y : X); norm3(u0); cross3(w0, m->lax, u0);
        unsigned char on_belt_g[MAX_T]; memset(on_belt_g, 0, sizeof on_belt_g);
        for (int t = 0; t < sh->nt; t++) { m->belt[t] = fabsf(dot3(sh->tn[t], m->lax)) < .4f; if (m->belt[t]) on_belt_g[m->grp[t]] = 1; }
        float ang[MAX_T]; int n = 0;
        for (int g = 0; g < m->ng; g++) if (!on_belt_g[g] && m->gw[g] > 0) { ang[n] = atan2f(dot3(m->gn[g], w0), dot3(m->gn[g], u0)); m->order[n++] = (unsigned char)g; }
        for (int i = 1; i < n; i++) for (int j = i; j > 0 && ang[j] < ang[j - 1]; j--) { /* by angle round the axis */
            float ta = ang[j]; ang[j] = ang[j - 1]; ang[j - 1] = ta;
            unsigned char to = m->order[j]; m->order[j] = m->order[j - 1]; m->order[j - 1] = to;
        }
        m->norder = n; m->r_flag = m->cutid;
    }
    float belt = fmodf((float)(now - m->r_at) * 2.6f, TAU);
    for (int t = 0; t < sh->nt; t++) m->ang[t] = m->belt[t] ? belt : 0;
    m->ang_on = 1;
    float per = .07f, cyc = fmaxf(m->norder * per, .5f);
    float M[9]; view_matrix(s, M);
    for (int i = 0; i < m->norder; i++) {
        int g = m->order[i];
        float e = fmodf(fmodf((float)(now - m->r_at) - i * per, cyc) + cyc, cyc), near = 0;
        if (s->hover > .3f) { float d[3]; m3_app(d, M, m->gn[g]); near = fmaxf(0, 1 - hypotf(d[0] - s->look_x_to, d[1] - s->look_y_to)); }
        w->grp[g] += (.12f + .1f * near * s->hover) * env(e, .045f, .03f, .13f) / fmaxf(.45f, m->gw[g]);
    }
    m->inner = .6f; m->inner_rate = -2.4f;
    w->rate = .32f;
}

static void swim(Session *s, const Shape *sh, Want *w, double now) { /* two fish of fine patches under the surface */
    Motion *m = &s->mo;
    float M[9]; view_matrix(s, M);
    float glide = .26f / (1 + 1.2f * s->hover);
    if (m->fish[0].to < 0) { /* start them far apart */
        int a = (int)(mrand(s) * sh->nv) % sh->nv, b = a; float far = -1;
        for (int v = 0; v < sh->nv; v++) { float d = -dot3(sh->v[a], sh->v[v]); if (d > far) { far = d; b = v; } }
        int st[2] = {a, b};
        for (int f = 0; f < 2; f++) m->fish[f] = (Fish){st[f], sh->vn[st[f]][0], 0, {0}, now - f * .13, {0}};
    }
    float lift[MAX_T]; memset(lift, 0, sizeof lift);
    for (int f = 0; f < 2; f++) {
        Fish *F = &m->fish[f], *O = &m->fish[1 - f];
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
                float c[3], d[3]; view_dir(s, M, sh->v[n], c);
                for (int q = 0; q < 3; q++) d[q] = c[q] - b[q];
                norm3(d);
                const float *o = sh->v[O->to], *p = sh->v[n];
                float apart = fminf(sqrtf((o[0] - p[0]) * (o[0] - p[0]) + (o[1] - p[1]) * (o[1] - p[1]) + (o[2] - p[2]) * (o[2] - p[2])) / CIRCUM, 1.2f);
                float score = dot3(d, dir) + .7f * c[2] + .5f * apart + (s->hover > .3f ? 1.6f * s->hover * dot3(d, want) : 0) + .45f * mrand(s);
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
    for (int g = 0; g < m->ng; g++) if (lift[g] > 0) w->grp[g] += lift[g] / fmaxf(.45f, m->gw[g] > 0 ? m->gw[g] : 1);
    w->rate = .8f;
}

static void unlock(Session *s, const Shape *sh, Want *w, double now) { /* charge, burst, a full turn, click-lock */
    Motion *m = &s->mo;
    float M[9]; view_matrix(s, M);
    if (!(m->r_flag & 1)) {
        m->r_flag = 1;
        float ax[3] = {rnd(s, -1, 1), rnd(s, -1, 1), rnd(s, -1, 1)}, u0[3], w0[3], Y[3] = {0, 1, 0}, X[3] = {1, 0, 0};
        norm3(ax); cross3(u0, ax, fabsf(ax[1]) < .9f ? Y : X); norm3(u0); cross3(w0, ax, u0);
        float a[MAX_T]; int idx[MAX_T];
        for (int g = 0; g < m->ng; g++) { a[g] = atan2f(dot3(m->gn[g], w0), dot3(m->gn[g], u0)); idx[g] = g; }
        for (int i = 1; i < m->ng; i++) for (int j = i; j > 0 && a[idx[j]] < a[idx[j - 1]]; j--) { int t = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = t; }
        for (int i = 0; i < m->ng; i++) m->rank[idx[i]] = (float)i / (float)(m->ng > 1 ? m->ng : 1);
        for (int i = 0; i < 4; i++) spike_random(s, sh, M, -rnd(s, .3f, .45f), .12f, NULL, .22f, .1f, now); /* charging */
    }
    float e = (float)(now - m->r_at);
    if (e < .3f) { w->scale *= 1 - .06f * io3(e / .3f); w->rate = 0; }
    else w->rate = .15f;
    if (e >= .3f && !(m->r_flag & 2)) { /* burst */
        m->r_flag |= 2;
        s->ring_at = now;
        flash(m, sh->nv, 1);
        for (int i = 0; i < 5; i++) spike_pair(s, sh, M, rnd(s, .28f, .4f), rnd(s, .25f, .4f), rnd(s, .2f, .4f), NULL, .07f, .16f, now);
        float ax[3] = {rnd(s, -1, 1), rnd(s, -1, 1), rnd(s, -1, 1)};
        norm3(ax);
        turn(s, ax, TAU, .9f, 0, now); /* a full turn while open */
    }
    for (int g = 0; g < m->ng; g++) {
        float close = 1.3f + m->rank[g] * .9f;
        float a = e < .3f ? 0 : e < .55f ? outx((e - .3f) / .25f) : e < close ? 1 : fmaxf(0, 1 - in3((e - close) / .08f));
        w->grp[g] += .2f * a / fmaxf(.45f, m->gw[g]);
        if (e >= close + .08f && !m->clicked[g]) { /* click: this patch is home */
            m->clicked[g] = 1;
            for (int t = 0; t < sh->nt; t++) if (m->grp[t] == g) for (int k = 0; k < 3; k++) m->glow[sh->t[t][k]] = fmaxf(m->glow[sh->t[t][k]], .9f);
        }
    }
    if (e > 2.32f && !(m->r_flag & 4)) { m->r_flag |= 4; s->ring_at = now; flash(m, sh->nv, .8f); s->spin_y += 3; }
}

/* ---- the geometric whims (the soft ones are in session.c) ---------------------------------------- */
static void whim(Session *s, const Shape *sh, const float *M, Want *w, double now) {
    Motion *m = &s->mo;
    if (s->whim < W_ALIGN) return;
    int start = m->wstart != s->whim_at;
    float u = clampf((float)(now - s->whim_at) / WHIMS[s->whim].dur, 0, 1);
    if (start) m->wstart = s->whim_at;
    switch (s->whim) {
    case W_ALIGN: if (start) align_to_viewer(s, sh, random_corner(s, sh), .55f, now); w->rate = 0; break;
    case W_STEP:
        if (start) { int v = random_corner(s, sh); float a[3]; m3_app(a, m->q, sh->v[v]); turn(s, a, (mrand(s) < .5f ? -1 : 1) * STEP5, .38f, 0, now); later(s, now + .6, EV_STEP); memcpy(m->lax, a, sizeof a); }
        w->rate = 0;
        break;
    case W_UNFOLD: {
        if (start) { /* a fresh cut, opened row by row from the top */
            recut(s, sh, 1, 6);
            float y[MAX_T];
            for (int g = 0; g < m->ng; g++) { float d[3]; m3_app(d, M, m->gn[g]); y[g] = d[1]; m->order[g] = (unsigned char)g; }
            for (int i = 1; i < m->ng; i++) for (int j = i; j > 0 && y[m->order[j]] < y[m->order[j - 1]]; j--) { unsigned char t = m->order[j]; m->order[j] = m->order[j - 1]; m->order[j - 1] = t; }
            m->norder = m->ng;
        }
        int n = m->norder;
        for (int k = 0; k < n; k++) {
            int g = m->order[k]; float a = (float)k / n * .45f, b = 1 - (float)(n - k) / n * .45f;
            float v = u < a ? 0 : u < a + .08f ? outx((u - a) / .08f) : u < b ? 1 : u < b + .08f ? 1 - in3((u - b) / .08f) : 0;
            if (m->gw[g] > 0) w->grp[g] += .15f * v / m->gw[g];
        }
        break;
    }
    case W_SCAN: m->scan_h = -1.1f + 2.2f * io3(u); m->scan_a = .7f; break;
    case W_SPIKE:
        if (start) { spike_random(s, sh, M, rnd(s, .3f, .4f), rnd(s, .15f, .4f), NULL, .07f, .16f, now); if (mrand(s) < .5f) later(s, now + rnd(s, .15f, .45f), EV_PAIR); }
        break;
    case W_CRAZY: if (start) crazy(s, sh, M, 1.4f, now); break;
    }
}

/* ---- every frame ------------------------------------------------------------------------------- */
void motion_tick(Session *s, const Shape *sh, int shown, double now, float dt) {
    Motion *m = &s->mo;
    if (sh->nt && m->cut_for != sh->nt) recut(s, sh, 1, 6);
    if (!sh->nt) m->ng = 0;
    if (m->routine != shown) routine_enter(s, sh, shown, now);
    for (int i = 0; i < sh->nv; i++) m->glow[i] *= expf(-dt * 5);
    float M[9]; view_matrix(s, M);
    double tt = now - s->created + s->phase;
    Want w; memset(&w, 0, sizeof w);
    w.scale = 1; w.rate = 1;
    m->ang_on = 0; m->scan_a = 0; m->inner = 0;

    /* things asked for a moment ago */
    for (int i = 0; i < MAX_EV; i++) {
        MEvent *e = &m->ev[i];
        if (!e->kind || now < e->at) continue;
        if (e->kind == EV_PAIR) spike_pair(s, sh, M, rnd(s, .25f, .4f), rnd(s, .35f, .55f), rnd(s, .04f, .3f), NULL, .07f, .16f, now);
        else if (e->kind == EV_JERK) { float a[3] = {rnd(s, -1, 1), rnd(s, -1, 1), rnd(s, -1, 1)}; norm3(a); turn(s, a, (mrand(s) < .5f ? -1 : 1) * rnd(s, .2f, .55f), .7f, 2, now); }
        else if (e->kind == EV_FLASH) flash(m, sh->nv, .7f);
        else if (e->kind == EV_STEP) turn(s, m->lax, STEP5, .38f, 0, now);
        e->kind = 0;
    }

    /* the state's own routine */
    switch (shown) {
    case ST_IDLE: {
        float u = (float)fmod(tt, 7.0) / 1.4f; /* now and then a line of light climbs from pole to pole */
        if (u < 1) { m->scan_h = 1.15f - 2.3f * io3(u); m->scan_a = .5f; }
        if (s->hover > .3f) { /* stirred by the pointer: calm out-and-in pairs on its side */
            float cl = hypotf(s->look_x_to, s->look_y_to);
            if (cl > .15f || (m->stir[0] == 0 && m->stir[1] == 0 && m->stir[2] == 0)) {
                float a = cl > .15f ? atan2f(s->look_y_to, s->look_x_to) : mrand(s) * TAU;
                m->stir[0] = cosf(a); m->stir[1] = sinf(a); m->stir[2] = .3f; norm3(m->stir);
            }
            if (now > m->stir_next) { m->stir_next = now + rnd(s, .22f, .45f); spike_pair(s, sh, M, rnd(s, .22f, .34f), rnd(s, .2f, .3f), rnd(s, .3f, .6f), m->stir, rnd(s, .3f, .45f), rnd(s, .45f, .7f), now); }
        }
        break;
    }
    case ST_THINK: if (sh->nt) think(s, sh, &w, now); break;
    case ST_WORK: if (sh->nt) work(s, sh, &w, now); else w.rate = 1; break;
    case ST_WAIT: if (sh->nt) swim(s, sh, &w, now); break;
    case ST_CHEER: if (sh->nt) unlock(s, sh, &w, now); break;
    case ST_SLEEP: {
        w.scale = .93f + .07f * s->hover;
        float u = (float)fmod(tt, 6.0) / 2.2f; /* the only sign of life: a faint scan every 6 s */
        if (u < 1) { m->scan_h = -1.1f + 2.2f * io3(u); m->scan_a = .5f; }
        break;
    }
    case ST_SPEAK: /* the patches rise and fall with the voice, each at its own rate */
        for (int g = 0; g < m->ng; g++) if (m->gw[g] > 0) w.grp[g] += .15f * s->level * fabsf(sinf(g * 1.7f + (float)tt * (6 + 5 * hashf(g)))) / m->gw[g];
        break;
    case ST_LISTEN: /* the patches facing you open with the microphone */
        w.rate = 0;
        for (int g = 0; g < m->ng; g++) {
            float d[3]; m3_app(d, M, m->gn[g]);
            if (d[2] > .35f && m->gw[g] > 0) w.grp[g] += (.04f + .1f * s->level * (.6f + .4f * sinf((float)tt * (4 + 3 * hashf(g)) + g)) + .06f * s->hover) * (d[2] - .35f) / .65f / m->gw[g];
        }
        break;
    case ST_TRANSCRIBE: { /* 36° steps while a scan reads up and down; the row it reads flips open */
        w.rate = 0;
        if (now >= m->r_next) { float Y[3] = {0, 1, 0}; turn(s, Y, TAU / 10, .22f, 0, now); m->r_next = now + .6; }
        float h = s->hover > .5f ? clampf(s->look_y_to, -1.05f, 1.05f) : sinf((float)tt * 2.6f) * 1.05f;
        m->scan_h = h; m->scan_a = .8f;
        for (int g = 0; g < m->ng; g++) { float d[3]; m3_app(d, M, m->gn[g]); if (d[2] > 0 && m->gw[g] > 0) { float k = d[1] - h; w.grp[g] += .1f * expf(-k * k / .03f) / m->gw[g]; } }
        break;
    }
    case ST_STANDBY:
        w.rate = 0; w.scale = .8f + .12f * s->hover;
        for (int g = 0; g < m->ng; g++) if (m->gw[g] > 0) w.grp[g] += .05f * s->hover / m->gw[g];
        break;
    case ST_ERROR: /* fits of going crazy */
        if (now - m->last_crazy > 2.6) crazy(s, sh, M, 1.3f, now);
        if (s->hover > .5f && !m->flinched) { float a[3] = {s->look_y_to, -s->look_x_to, 0}; norm3(a); m->flinched = 1; jerk(s, a, -.3f, now); }
        if (s->hover < .2f) m->flinched = 0;
        break;
    }
    whim(s, sh, M, &w, now);

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

    /* spikes: snap out (or rise calmly), hold, go back */
    for (int i = 0; i < sh->nv; i++) w.push[i] = 0;
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
    if (!m->ang_on) for (int t = 0; t < sh->nt; t++) { /* layers not driven: settle on the nearest whole fifth of a turn, then rest */
        float home = roundf(m->ang[t] / STEP5) * STEP5;
        m->ang[t] += (home - m->ang[t]) * f;
        if (fabsf(m->ang[t] - home) < 1e-3f) m->ang[t] = 0;
    }
    m->scale += (w.scale - m->scale) * (1 - expf(-dt * 10));
    m->rate += ((turns_busy(m) ? 0 : w.rate) - m->rate) * (1 - expf(-dt * 4));
    m->inner_ang += m->inner_rate * dt;
    run_turns(s, now);
}

int motion_busy(const Session *s, double now) {
    const Motion *m = &s->mo;
    if (turns_busy(m) || m->inner > 0 || m->scan_a > 0 || m->lock > .01f || now < m->crazy_until || now - m->press_at < 1.4) return 1;
    for (int i = 0; i < MAX_SPIKE; i++) if (spike_live(&m->sp[i], now)) return 1;
    for (int t = 0; t < MAX_T; t++) if (fabsf(m->off[t]) > .002f || m->ang[t] != 0) return 1;
    return 0;
}

int motion_crazy(const Session *s, double now) { return now < s->mo.crazy_until && now >= s->mo.tense_until; }

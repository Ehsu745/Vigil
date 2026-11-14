/* 4. slingshot (from Orbit): one triangle lifts off and is flung into orbit round the core, faster and faster and
 * swinging wider, a light trail behind it, the core leaning against the pull. After about one round, going too fast
 * to hold, it tears free just as its swing points at you and flies at you, a dart, chasing you for up to 4 s and
 * turning no faster than it can, so it can be dodged; the core's pull bends it as it passes. If it catches you its tip
 * strikes: a freeze, a flash, a hard push along the flight. Then it flies on, slows, floats and goes home. Dodged,
 * it whips round in a tight turn and comes again, faster than a missile. Dodge it, then lead it into the core, and it
 * breaks a hole in the shell. (Bestiary's ATTACK.slingshot) */
#define SLINGSHOT_SPEED 32   /* its flight, stage units a second (a missile tops out at 15) */
enum { SLINGSHOT_LIFT = 1, SLINGSHOT_ORBIT, SLINGSHOT_FLY };
#define SLINGSHOT_TRAIL 20
typedef struct {
    int phase, ntr;                /* SLINGSHOT_*: its own steps before RP_FLY's chase; the trail's length */
    float ph, rev, t0, tf, spd;    /* the orbit's angle and how far it has gone round, when it started; when it tore free, its speed */
    float q[3];                    /* where it is on its orbit: from the core, in shell radii */
    float tr[SLINGSHOT_TRAIL][3];  /* the light trail, stage points, oldest first */
} SlingshotAtk;
_Static_assert(sizeof(SlingshotAtk) <= sizeof(((ReadyRS *)0)->own), "SlingshotAtk must fit in ReadyRS.own");

static void slingshot_push(SlingshotAtk *A, const float *p) {
    if (A->ntr == SLINGSHOT_TRAIL) { memmove(A->tr, A->tr + 1, sizeof A->tr[0] * (SLINGSHOT_TRAIL - 1)); A->ntr--; }
    memcpy(A->tr[A->ntr++], p, sizeof A->tr[0]);
}
static void slingshot_shift(SlingshotAtk *A) { if (A->ntr) { memmove(A->tr, A->tr + 1, sizeof A->tr[0] * (SLINGSHOT_TRAIL - 1)); A->ntr--; } }
/* the trail lives on the stage; Motion's trail is drawn over the shell, so each frame its points are carried into the
 * shell's frame of this very frame, where they land back on the same stage points */
static void slingshot_trail(Ctx *c, const RFrame *f, const SlingshotAtk *A) {
    Motion *m = c->m;
    if (A->ntr < 2) { m->ntrail = 0; return; }
    for (int i = 0; i < A->ntr; i++) { float v[3]; r_to_view(f, A->tr[i], v); m3_tapp(m->trail[i], f->Mf, v); for (int k = 0; k < 3; k++) m->trail[i][k] /= CIRCUM; }
    m->ntrail = A->ntr; m->trail_on = 1; m->trail_col = RGB255(190, 214, 255);
}
/* the stage point of its place on the orbit */
static void slingshot_at(const RFrame *f, const float *q, float *pos) { pos[0] = f->wox + q[0] * f->sc; pos[1] = f->woy + q[1] * f->sc; pos[2] = q[2]; }

static void slingshot_start(Ctx *c, const RFrame *f, ReadyRS *R, const float *T) {
    SlingshotAtk *A = (SlingshotAtk *)R->own.b;
    float dir[3] = {T[0] - f->wox, T[1] - f->woy, 0}; norm3(dir);
    int ti = 0;
    R->n = 0;
    if (!r_pick_facing(c, f, T, 1, NULL, .05f, .3f, &ti)) return;
    RPiece *p = &R->p[0];
    memset(p, 0, sizeof *p);
    p->ti = ti; p->tip = r_tip_toward(f, c->sh, ti, dir); p->st = RP_MORPH; p->s = 1; p->dmin = 1e9f;
    R->n = 1;
    A->phase = SLINGSHOT_LIFT; A->spd = SLINGSHOT_SPEED;
    c->m->ntrail = 0;
}
static void slingshot_run(Ctx *c, const RFrame *f, ReadyRS *R, const float *T, float t, float dt) {
    SlingshotAtk *A = (SlingshotAtk *)R->own.b;
    if (!R->n) return;
    RPiece *p = &R->p[0];
    Session *s = c->s;
    const float core[2] = {f->wox, f->woy}, sc = f->sc > 0 ? f->sc : 1;
    if (p->st == RP_MORPH && A->phase == SLINGSHOT_LIFT) { /* it lifts off its place, glowing */
        float v = out3(clampf(t / .3f, 0, 1)), nv[3], th[3], off[3];
        R->charge = fmaxf(R->charge, .35f * v);
        r_normal(f, c->sh, p->ti, nv); r_tip_home(f, c->sh, p->tip, th); r_off(f, c->sh, p->ti, p->tip, off);
        for (int i = 0; i < 2; i++) p->pos[i] = th[i] + nv[i] * .3f * v * sc;
        p->pos[2] = th[2] + nv[2] * .3f * v;
        memcpy(p->dir, off, sizeof p->dir); p->s = 1 + .3f * v; p->k = 0;
        r_place_tip(c, f, p->ti, p->tip, p->pos, p->dir, 0, p->s);
        r_glow(c, p->ti, .6f * v);
        if (t >= .3f) {
            A->phase = SLINGSHOT_ORBIT; A->t0 = t;
            set3(A->q, (p->pos[0] - core[0]) / sc, (p->pos[1] - core[1]) / sc, p->pos[2]);
            A->ph = atan2f(A->q[1], A->q[0]);
        }
    } else if (p->st == RP_MORPH && A->phase == SLINGSHOT_ORBIT) {
        float e = t - A->t0, c0 = fminf(1, .35f + e / 1.3f);
        R->charge = fmaxf(R->charge, c0);
        { float l = hypotf(A->q[0], A->q[1]); if (l > 1e-5f) { s->w_lean_x += A->q[0] / l * .35f * c0; s->w_lean_y += A->q[1] / l * .35f * c0; } } /* the core leaning against the pull */
        /* faster and faster round the core; the faster it goes the wider it swings, the pull barely holding it, until it tears free */
        float w = fminf(18, 12 + e * 8), grow = (w / 18) * (w / 18);
        float r = clampf(1.6f - .4f, 1.35f, 1.9f) * (.85f + .15f * fminf(1, e * 3)) * (1 + .35f * grow) + .05f * grow * grow * sinf(e * 31);
        A->ph += w * dt; A->rev += w * dt;
        float prev[3], q[3] = {cosf(A->ph) * r, sinf(A->ph) * r * .9f, .2f * sinf(A->ph)};
        slingshot_at(f, A->q, prev);
        memcpy(A->q, q, sizeof q);
        slingshot_at(f, A->q, p->pos);
        if (dt > 0 || len3(p->dir) < 1e-6f) { float d[3] = {(p->pos[0] - prev[0]) / sc, (p->pos[1] - prev[1]) / sc, p->pos[2] - prev[2]}; if (len3(d) > 1e-7f) memcpy(p->dir, d, sizeof d); }
        p->k = clampf(w / 16, .2f, .9f);
        r_place_tip(c, f, p->ti, p->tip, p->pos, p->dir, p->k, p->s);
        slingshot_push(A, p->pos);
        float toT[2] = {T[0] - p->pos[0], T[1] - p->pos[1]};
        float al = (p->dir[0] * toT[0] + p->dir[1] * toT[1]) / (hypotf(p->dir[0], p->dir[1]) * hypotf(toT[0], toT[1]) + 1e-6f);
        if ((A->rev > 1.5f * PI_F && al > .97f) || A->rev > 3.5f * PI_F) { /* about one round, then off as its swing points at you, at full speed */
            float ld[3] = {toT[0], toT[1], 0}; norm3(ld);
            feel(R, RF_LUNGE, ld, 1);
            p->st = RP_FLY; A->phase = SLINGSHOT_FLY; A->tf = t;
            float v[3] = {p->dir[0], p->dir[1], 0}; norm3(v);
            for (int i = 0; i < 3; i++) p->vel[i] = v[i] * SLINGSHOT_SPEED;
            ring(c->m, c->now, .4f);
            { float ax[3] = {-p->vel[1], p->vel[0], 0}; norm3(ax); jerk(s, ax, .06f, c->now); } /* (as core_crash turns the Bestiary's jerk axis) */
        }
    } else if (p->st == RP_FLY) {
        float prev[3] = {p->pos[0], p->pos[1], p->pos[2]};
        p->k = 1; /* it chases you, fast, but turns no faster than this, so it can be dodged */
        float up = 1 + .12f * (float)(p->nmiss < 5 ? p->nmiss : 5), SP = A->spd * up; /* each time you dodge it, faster and turning harder */
        { float cur = atan2f(p->vel[1], p->vel[0]), toT[2] = {T[0] - p->pos[0], T[1] - p->pos[1]};
          float lead = fminf(.5f, hypotf(toT[0], toT[1]) / SP), want = atan2f(toT[1] + R->pv[1] * lead, toT[0] + R->pv[0] * lead); /* where you will be when it gets there */
          if (p->whip && fabsf(wrap_a(want - cur)) < .3f) p->whip = 0; /* past you it whips round, tight, until it points at you again */
          float TURN = (p->whip ? 22 : 8) * (1 + .15f * (float)(p->nmiss < 5 ? p->nmiss : 5)), na = cur + clampf(wrap_a(want - cur), -TURN * dt, TURN * dt);
          p->vel[0] = cosf(na) * SP; p->vel[1] = sinf(na) * SP; p->vel[2] = 0; }
        { float pr[2] = {(p->pos[0] - core[0]) / sc, (p->pos[1] - core[1]) / sc}, r = fmaxf(1.05f, hypotf(pr[0], pr[1])), g = (p->dodged ? 90 : 70) / (r * r);
          /* the core's pull: lead it past the core and it swings round; once dodged it pulls harder */
          p->vel[0] -= pr[0] / r * g * dt; p->vel[1] -= pr[1] / r * g * dt;
          float v = hypotf(p->vel[0], p->vel[1]); if (v > 1e-6f) { p->vel[0] *= SP / v; p->vel[1] *= SP / v; }
          float dT = hypotf(T[0] - p->pos[0], T[1] - p->pos[1]);
          if (dT < p->dmin) p->dmin = dT; else if (dT > p->dmin + .3f) { if (!p->whip) p->nmiss++; p->dodged = 1; p->whip = 1; p->dmin = dT; } /* it came close, missed and flew on: you dodged it; round it comes */
          if (r < 1.2f && !p->dodged) { /* not dodged yet: the core is solid, it glances off */
              float u[2] = {pr[0] / r, pr[1] / r}, vn = p->vel[0] * u[0] + p->vel[1] * u[1];
              if (vn < 0) { p->vel[0] -= 2 * vn * u[0]; p->vel[1] -= 2 * vn * u[1]; float l = hypotf(p->vel[0], p->vel[1]); if (l > 1e-6f) { p->vel[0] *= SP / l; p->vel[1] *= SP / l; } }
          } }
        memcpy(p->dir, p->vel, sizeof p->dir);
        float next[3] = {p->pos[0] + p->vel[0] * dt, p->pos[1] + p->vel[1] * dt, p->pos[2] * expf(-dt * 8)};
        int past = t - A->tf > 4; /* chased for 4 s without a hit: it gives up */
        float hr = fmaxf(.28f * sc, s->aim_hit);
        float sx = next[0] - prev[0], sy = next[1] - prev[1], sl = sx * sx + sy * sy, tx0 = T[0] - prev[0], ty0 = T[1] - prev[1];
        float sk = sl > 0 ? clampf((tx0 * sx + ty0 * sy) / sl, 0, 1) : 0;
        if (p->dodged && hypotf(next[0] - core[0], next[1] - core[1]) < 1.1f * sc) { /* dodged, then into the core: it explodes and breaks a hole in the shell */
            float lo = 0, hi = 1;
            for (int i = 0; i < 12; i++) { float u = (lo + hi) / 2; if (hypotf(prev[0] + sx * u - core[0], prev[1] + sy * u - core[1]) < 1.1f * sc) hi = u; else lo = u; }
            float I[3] = {prev[0] + sx * lo, prev[1] + sy * lo, 0}, In[3] = {I[0] - core[0], I[1] - core[1], 0};
            norm3(In);
            memcpy(p->pos, I, sizeof I);
            core_crash(c, f, R, p, I, In);
        } else if (s->aim_on && hypotf(tx0 - sx * sk, ty0 - sy * sk) < hr) { /* its tip strikes: freeze, flash, a hard push along the flight */
            float T0[3] = {T[0], T[1], 0}, hd[2] = {p->vel[0], p->vel[1]};
            memcpy(p->pos, T0, sizeof T0);
            r_place_tip(c, f, p->ti, p->tip, p->pos, p->dir, 1, p->s);
            atk_hit(c, f, R, T0, hd, RGB255(240, 246, 255), 70, 1.2f, 0);
            float g = atan2f(p->vel[1], p->vel[0]) + rnd(s, -.6f, .6f), kv[3] = {cosf(g) * 7.5f, sinf(g) * 7.5f, 0};
            knock_on(c, f, R, p, kv);
        } else if (past) { /* never caught you: it sails on, slows, floats, goes home */
            memcpy(p->pos, next, sizeof next);
            r_place_tip(c, f, p->ti, p->tip, p->pos, p->dir, 1, p->s);
            float kv[3] = {p->vel[0] * .35f, p->vel[1] * .35f, 0};
            knock_on(c, f, R, p, kv);
        } else {
            memcpy(p->pos, next, sizeof next);
            r_place_tip(c, f, p->ti, p->tip, p->pos, p->dir, 1, p->s);
        }
        if (p->st == RP_FLY) slingshot_push(A, p->pos);
    } else if (p->st == RP_COAST || p->st == RP_FLOAT) { coast_on(c, f, R, p); slingshot_shift(A); }
    else if (p->st == RP_BROKE) slingshot_shift(A); /* its pieces are moved below */
    else if (p->st == RP_BACK) { homeward(c, f, R, p); slingshot_shift(A); }
    else slingshot_shift(A);
    for (int i = 0; i < R->nsh; i++) shard_move(c, &R->sh[i], R->rt); /* the broken shell and the slingshot's own pieces */
    for (int i = 0; i < R->nsh; i++) if (!R->sh[i].home) shard_place(c, f, &R->sh[i], R->rt);
    slingshot_trail(c, f, A);
}

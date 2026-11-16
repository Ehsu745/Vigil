/* 3. forge (the Forge routine, at the pointer): one blow at a time, with weight. Three times a random clump of three to
 * five neighbouring pieces on your side floats straight up out of its place, still at the angle it had on the shell,
 * and stops clear of the core; there it turns, a real turn in 3D, its broad side to you and its flat edge leading,
 * glowing hotter while heat is drawn into it, turning after you, for a charge of its own random length (so there is
 * no beat to learn); a moment before the end it takes aim at where you are now, and the blow shoots out to that spot,
 * face first, just past flush, the faster the further it has to go (from afar, all but at once): a flash, hot sparks, embers, a ring, the shell recoils and ripples, and
 * the clump shatters into its little triangles, which fly on round the blow, float and go home in turn. If you have moved by then it slams down on nothing, carries on with its swing across
 * the screen (at its own size), floats a moment and goes home. The last blow is the heaviest. */
#define FORGE_TRIS 5
#define FORGE_TURN .8f      /* risen and turned to you by then; the charge runs on from there */
#define FORGE_BLOW_MAX .13f /* the blow from close by; from far it is all but instant (forge_blow) */
/* how long the blow takes over d shell radii: the further, the faster, so distance is no help */
static float forge_blow(float d) { return clampf(FORGE_BLOW_MAX - .016f * (d - 1.5f), .045f, FORGE_BLOW_MAX); }
typedef struct {
    int tris[FORGE_TRIS], nt, b, face[2];
    float c0[3], mid[3], n[3], ft;                       /* model: the clump's centre, its flat edge's middle, its normal */
    int top_set, sdt_set, lock_set, ctl_state, hit, miss, done, from_set, broke, blow_set; /* ctl_state: 0 not chosen, 1 straight, 2 round the core; broke: shattered on a hit (its triangles are shards) */
    float hold, lead, blow;                              /* its own clock: the charge ends at hold, aim is taken lead before that, the blow takes blow */
    float top[3], sd[3], sdt[3], lock[3], ctl[3];        /* stage */
    float base[3], vel[3], t0, M0[9], R[9], ang, w, from[3], a0; /* after a miss: flying on, loose */
} ForgeBlock;
typedef struct { ForgeBlock bl[3]; int nb; } ForgeAtk;
_Static_assert(sizeof(ForgeAtk) <= sizeof(((ReadyRS *)0)->own), "forge: its state must fit in ReadyRS.own");

/* the turn R taken u of the way about its own axis: a true rotation all the way, never a squashed blend */
static void forge_part_turn(const float *R, float u, float *out) {
    float cs = clampf((R[0] + R[4] + R[8] - 1) / 2, -1, 1), ang = acosf(cs), ax[3] = {R[7] - R[5], R[2] - R[6], R[3] - R[1]};
    if (ang < 1e-4f) { memcpy(out, R, sizeof(float) * 9); return; }
    float l = len3(ax);
    if (l < 1e-6f) { /* a half turn: the axis is the largest column of R + I */
        int k = R[0] >= R[4] && R[0] >= R[8] ? 0 : R[4] >= R[8] ? 1 : 2;
        set3(ax, R[k] + (k == 0), R[3 + k] + (k == 1), R[6 + k] + (k == 2)); norm3(ax);
    } else for (int i = 0; i < 3; i++) ax[i] /= l;
    m3_rot(out, ax, ang * u);
}
/* the clump turned by R9 (view, may scale) with its model point anchor on stage point at (Bestiary's placeGroup) */
static void forge_place(Ctx *c, const RFrame *f, const ForgeBlock *B, const float *at, const float *R9) {
    float atv[3], ks = R_FREE_ON && f->sc > 0 ? 1 / f->sc : 1; /* (r_place keeps a loose piece its own size) */
    r_to_view(f, at, atv);
    R_ANC = atv; /* (one perspective for the whole clump: it stays one piece) */
    for (int j = 0; j < B->nt; j++) {
        float cm[3], d[3], dv[3], o[3], cv[3];
        tri_centre(c->sh, B->tris[j], cm);
        for (int i = 0; i < 3; i++) d[i] = cm[i] - B->mid[i];
        m3_app(dv, MOF(f), d); m3_app(o, R9, dv);
        for (int i = 0; i < 3; i++) cv[i] = atv[i] + o[i] * ks;
        r_place(c, f, B->tris[j], cv, R9);
    }
    R_ANC = NULL;
}
static void forge_glow(Ctx *c, const ForgeBlock *B, float a) { for (int j = 0; j < B->nt; j++) r_glow(c, B->tris[j], a); }
static int forge_used(const unsigned char *used, const Shape *sh, int ti) { return used[ti] || used[sh->tnb[ti][0]] || used[sh->tnb[ti][1]] || used[sh->tnb[ti][2]]; }
static int forge_in(const ForgeBlock *B, int ti) { for (int j = 0; j < B->nt; j++) if (B->tris[j] == ti) return 1; return 0; }

static void forge_start(Ctx *c, const RFrame *f, ReadyRS *R, const float *T) {
    (void)T;
    const Shape *sh = c->sh;
    ForgeAtk *A = (ForgeAtk *)R->own.b;
    unsigned char used[MAX_T] = {0};
    int pool[MAX_T];
    R->n = 0; A->nb = 0;
    for (int b = 0; b < 3; b++) { /* a random clump anywhere on the side you see */
        int np = 0;
        for (int ti = 0; ti < sh->nt; ti++) { float nv[3]; r_normal(f, sh, ti, nv); if (nv[2] > .05f && !forge_used(used, sh, ti)) pool[np++] = ti; }
        if (!np) break;
        ForgeBlock *B = &A->bl[A->nb];
        memset(B, 0, sizeof *B);
        int want = 3 + (int)(mrand(c->s) * 3) % 3;
        B->tris[B->nt++] = pool[(int)(mrand(c->s) * np) % np];
        while (B->nt < want) { /* grows into a random clump */
            int cand[FORGE_TRIS * 3], nc = 0;
            for (int j = 0; j < B->nt; j++) for (int k = 0; k < 3; k++) {
                int x = sh->tnb[B->tris[j]][k], dup = 0;
                for (int q = 0; q < nc; q++) if (cand[q] == x) dup = 1;
                if (!dup && !used[x] && !forge_in(B, x) && !forge_used(used, sh, x)) cand[nc++] = x;
            }
            if (!nc) break;
            B->tris[B->nt++] = cand[(int)(mrand(c->s) * nc) % nc];
        }
        for (int j = 0; j < B->nt; j++) used[B->tris[j]] = 1;
        float fl = -1;
        for (int j = 0; j < B->nt; j++) { /* its flat face: the longest edge on the clump's outline (an edge no other piece of it shares) */
            float cm[3]; tri_centre(sh, B->tris[j], cm);
            for (int i = 0; i < 3; i++) { B->c0[i] += cm[i] / B->nt; B->n[i] += sh->tn[B->tris[j]][i]; }
            for (int k = 0; k < 3; k++) {
                int a = sh->t[B->tris[j]][k], e = sh->t[B->tris[j]][(k + 1) % 3], cnt = 0;
                for (int q = 0; q < B->nt; q++) for (int r = 0; r < 3; r++) { int x = sh->t[B->tris[q]][r], y = sh->t[B->tris[q]][(r + 1) % 3]; if ((x == a && y == e) || (x == e && y == a)) cnt++; }
                if (cnt != 1) continue;
                float d[3] = {sh->v[a][0] - sh->v[e][0], sh->v[a][1] - sh->v[e][1], sh->v[a][2] - sh->v[e][2]}, l = len3(d);
                if (l > fl) { fl = l; B->face[0] = a < e ? a : e; B->face[1] = a < e ? e : a; }
            }
        }
        for (int i = 0; i < 3; i++) B->mid[i] = (sh->v[B->face[0]][i] + sh->v[B->face[1]][i]) * .5f;
        norm3(B->n);
        B->b = b;
        B->hold = FORGE_TURN + rnd(c->s, .15f, .95f); B->lead = rnd(c->s, .1f, .2f); /* a charge of its own length; aim taken late */
        B->ft = b ? A->bl[b - 1].ft + A->bl[b - 1].hold + FORGE_BLOW_MAX + .2f : .05f; /* the next rises as the last lands */
        RPiece *p = &R->p[R->n++]; /* one piece stands for the clump: home only once the clump is */
        memset(p, 0, sizeof *p);
        p->ti = B->tris[0]; p->i = b; p->st = RP_MORPH; p->s = 1;
        A->nb++;
    }
}

static void forge_run(Ctx *c, const RFrame *f, ReadyRS *R, const float *T, float t, float dt) {
    /* it floats up as it was, turns (a real turn in 3D), charges, strikes; OFF in the Bestiary (how far from you it stops) is unused there */
    const float RISE = .45f, TURN = FORGE_TURN, Z[3] = {0, 0, 1};
    ForgeAtk *A = (ForgeAtk *)R->own.b;
    const float core[2] = {f->wox, f->woy}, sc = f->sc;
    float T0[3] = {T[0], T[1], 0}, ch = 0;
    int last = A->nb - 1;
    for (int b = 0; b < A->nb; b++) {
        ForgeBlock *B = &A->bl[b];
        RPiece *p = &R->p[b];
        float e = t - B->ft;
        const float HOLD = B->hold, HIT = HOLD + (B->blow_set ? B->blow : FORGE_BLOW_MAX), BACK = HIT + .27f, HOME = BACK + .6f;
        if (B->broke) { p->st = RP_BROKE; continue; } /* its little triangles are moved after the loop */
        if (e < 0 || (e > HOME && !(B->miss && !B->done))) { if (e > HOME) p->st = RP_HOME; continue; }
        if (B->miss) { /* it missed: carried on by its blow, slowing and tumbling, it floats a moment, then goes home */
            float k = R->rt - B->t0, rdt = c->dt, pos[3], Rr[9], Rz[9], Mz[9];
            if (k < R_COAST) { float g = expf(-rdt * 2.5f); for (int i = 0; i < 3; i++) { B->vel[i] *= g; B->base[i] += B->vel[i] * rdt; } } /* flying on with the whole strength of its blow */
            B->base[2] *= expf(-rdt * 8); /* at the screen's depth, as it struck: its size stays its own (see the miss below) */
            B->ang += B->w * rdt; B->w *= expf(-rdt * 1.5f);
            memcpy(Mz, B->M0, sizeof Mz);
            if (k < R_COAST + R_FLOAT) {
                float fl = clampf((k - R_COAST) / R_FLOAT, 0, 1);
                set3(pos, B->base[0] + .01f * sinf(fl * 6.28f), B->base[1] - .05f * sinf(fl * PI_F), B->base[2]);
                m3_rot(Rz, Z, B->ang); m3_mul(Rr, Rz, B->R);
                p->st = k < R_COAST ? RP_COAST : RP_FLOAT;
            } else {
                float v = io3(clampf((k - R_COAST - R_FLOAT) / .7f, 0, 1)), hv[3], hm[3], Rq[9];
                if (v >= 1) { B->done = 1; forge_glow(c, B, .9f); p->st = RP_HOME; continue; }
                if (!B->from_set) { B->from_set = 1; memcpy(B->from, B->base, sizeof B->from); B->a0 = B->ang; }
                m3_app(hv, f->Mf, B->mid); r_from_view(f, hv, hm); /* its place on the shell now */
                m3_rot(Rz, Z, B->a0 * (1 - v)); m3_mul(Rq, Rz, B->R);
                float w0 = (1 - v) * (1 - v), w1 = 2 * v * (1 - v), w2 = v * v;
                for (int i = 0; i < 3; i++) { float ctl = (B->from[i] + hm[i]) * .5f - (i == 1 ? .3f : 0); pos[i] = B->from[i] * w0 + ctl * w1 + hm[i] * w2; }
                for (int i = 0; i < 9; i++) Rr[i] = Rq[i] + (I3[i] - Rq[i]) * v;
                m3_ortho(Rr); home_turn(f, B->M0, v, Mz);
                p->st = RP_BACK;
            }
            as_free(Mz); forge_place(c, f, B, pos, Rr); free_end();
            memcpy(p->pos, pos, sizeof p->pos);
            forge_glow(c, B, .3f);
            continue;
        }
        p->st = RP_FLY;
        float hv[3], h[3], nv[3];
        m3_app(hv, f->Mf, B->mid); r_from_view(f, hv, h);
        m3_app(nv, f->Mf, B->n); norm3(nv);
        if (!B->top_set) { /* it floats straight up out of its own place, about a shell radius, clear of the core */
            float tp[3] = {(h[0] - core[0]) / sc + nv[0] * .9f, (h[1] - core[1]) / sc + nv[1] * .9f, h[2] + nv[2] * .9f}, l = hypotf(tp[0], tp[1]);
            if (l < 1.6f) {
                float d[2] = {l > .05f ? tp[0] / l : (nv[0] != 0 ? nv[0] : 1), l > .05f ? tp[1] / l : nv[1]}, dn = hypotf(d[0], d[1]);
                if (dn <= 0) dn = 1;
                tp[0] = d[0] / dn * 1.6f; tp[1] = d[1] / dn * 1.6f;
            }
            set3(B->top, core[0] + tp[0] * sc, core[1] + tp[1] * sc, tp[2]);
            B->top_set = 1;
            set3(B->sd, T0[0] - B->top[0], T0[1] - B->top[1], 0); norm3(B->sd);
        }
        if (!B->lock_set && e >= RISE) { /* charging, it keeps turning after you */
            set3(B->sdt, T0[0] - B->top[0], T0[1] - B->top[1], 0);
            if (hypotf(B->sdt[0], B->sdt[1]) > 1e-4f) { norm3(B->sdt); B->sdt_set = 1; }
        }
        if (B->sdt_set) { float k = 1 - expf(-dt * (B->lock_set ? 25 : 9)); for (int i = 0; i < 3; i++) B->sd[i] += (B->sdt[i] - B->sd[i]) * k; norm3(B->sd); } /* stopped, it turns its face to you */
        const float *bd = B->sd;
        float side[3] = {-bd[1], bd[0], 0};
        /* turned with its broad side to you (the most of it you can see), its flat edge leading toward the blow */
        float om[3], o[3], ed[3], ev[3], on[3], R1[9], on1[3], Rz[9], Rt[9];
        for (int i = 0; i < 3; i++) { om[i] = B->mid[i] - B->c0[i]; ed[i] = c->sh->v[B->face[1]][i] - c->sh->v[B->face[0]][i]; }
        m3_app(o, f->Mf, om); m3_app(ev, f->Mf, ed); norm3(ev);
        { float d = dot3(o, ev); for (int i = 0; i < 3; i++) on[i] = o[i] - ev[i] * d; norm3(on); }
        turn_to(R1, nv, Z); m3_app(on1, R1, on);
        m3_rot(Rz, Z, atan2f(bd[1], bd[0]) - atan2f(on1[1], on1[0])); m3_mul(Rt, Rz, R1);
        if (e >= HOLD - B->lead && !B->lock_set) { /* a moment before the charge ends it takes aim at where you are now: the blow lands here, wherever you go */
            B->lock_set = 1; memcpy(B->lock, T0, sizeof B->lock);
            B->blow = forge_blow(hypotf(B->lock[0] - B->top[0], B->lock[1] - B->top[1]) / (sc > 0 ? sc : 1)); B->blow_set = 1;
            for (int i = 0; i < 3; i++) B->sdt[i] = B->lock[i] - B->top[i];
            norm3(B->sdt); B->sdt_set = 1;
        }
        const float *L = B->lock_set ? B->lock : T0;
        float at[3], turnw, pw;
        if (e < RISE) { float u = io3(e / RISE); for (int i = 0; i < 3; i++) at[i] = h[i] + (B->top[i] - h[i]) * u; turnw = 0; pw = e / HOLD; } /* floating up out of its place, still at the angle it had on the shell */
        else if (e < TURN) { memcpy(at, B->top, sizeof at); turnw = io3((e - RISE) / (TURN - RISE)); pw = e / HOLD; } /* then it turns, about one axis, its broad side to you */
        else if (e < HOLD) { float q = .014f * sinf(e * 70) * (e - TURN) / (HOLD - TURN) * sc; for (int i = 0; i < 3; i++) at[i] = B->top[i] + side[i] * q; turnw = 1; pw = e / HOLD; } /* quivering at the top */
        else if (e < HIT) { /* the blow: straight at the spot, or round the core if the core is in the way */
            float end[3], u = in3((e - HOLD) / (HIT - HOLD));
            for (int i = 0; i < 3; i++) end[i] = L[i] + bd[i] * .05f * sc;
            if (!B->ctl_state) {
                float ab[2] = {end[0] - B->top[0], end[1] - B->top[1]}, l2 = ab[0] * ab[0] + ab[1] * ab[1];
                if (l2 <= 0) l2 = 1e-9f;
                float su = clampf(((core[0] - B->top[0]) * ab[0] + (core[1] - B->top[1]) * ab[1]) / l2, 0, 1);
                float sgd = hypotf(core[0] - B->top[0] - ab[0] * su, core[1] - B->top[1] - ab[1] * su);
                B->ctl_state = 1;
                if (sgd < 1.3f * sc) {
                    float mid[2] = {(B->top[0] + end[0]) * .5f - core[0], (B->top[1] + end[1]) * .5f - core[1]}, ml = hypotf(mid[0], mid[1]), d[3];
                    if (ml > .05f * sc) set3(d, mid[0] / ml, mid[1] / ml, 0);
                    else { set3(d, -ab[1], ab[0], 0); norm3(d); }
                    set3(B->ctl, core[0] + d[0] * 2.2f * sc, core[1] + d[1] * 2.2f * sc, 0);
                    B->ctl_state = 2;
                }
            }
            if (B->ctl_state == 2) { float w0 = (1 - u) * (1 - u), w1 = 2 * u * (1 - u), w2 = u * u; for (int i = 0; i < 3; i++) at[i] = B->top[i] * w0 + B->ctl[i] * w1 + end[i] * w2; }
            else for (int i = 0; i < 3; i++) at[i] = B->top[i] + (end[i] - B->top[i]) * u;
            turnw = 1; pw = 1;
        }
        else if (e < BACK) { float k = .05f * sc * (1 - out3(clampf((e - HIT) / .12f, 0, 1))); for (int i = 0; i < 3; i++) at[i] = L[i] + bd[i] * k; turnw = 1; pw = 1 - (e - HIT) / (BACK - HIT) * .6f; } /* just past flush, and settled */
        else { /* sinking back home */
            float u = io3((e - BACK) / (HOME - BACK)), w0 = (1 - u) * (1 - u), w1 = 2 * u * (1 - u), w2 = u * u;
            for (int i = 0; i < 3; i++) { float ctl = (L[i] + h[i]) * .5f + nv[i] * .3f * (i < 2 ? sc : 1); at[i] = L[i] * w0 + ctl * w1 + h[i] * w2; }
            turnw = 1 - u; pw = .4f * (1 - u);
        }
        float Rp[9]; /* part of the way along the one turn, a true rotation */
        if (turnw >= 1) memcpy(Rp, Rt, sizeof Rp);
        else if (turnw <= 0) memcpy(Rp, I3, sizeof Rp);
        else forge_part_turn(Rt, turnw, Rp);
        forge_place(c, f, B, at, Rp);
        memcpy(p->pos, at, sizeof p->pos);
        forge_glow(c, B, .15f + .8f * pw * pw); /* power gathering in it */
        if (e < HIT) { ch = fmaxf(ch, .3f + .7f * clampf(e / HOLD, 0, 1)); c->w->bright *= 1 + .4f * pw * pw; }
        if (e > .25f && e < HOLD && mrand(c->s) < .45f) { /* heat drawn in */
            float d[3] = {rnd(c->s, -1, 1), rnd(c->s, -1, 1), rnd(c->s, -.3f, .3f)}; norm3(d);
            float sp[3] = {at[0] + d[0] * .45f * sc, at[1] + d[1] * .45f * sc, at[2] + d[2] * .45f}, v[3] = {-1.1f * d[0] * sc, -1.1f * d[1] * sc, -1.1f * d[2]};
            r_spark(c, f, sp, v, .38f, 1, 0, .9f, SK_MOTE, 0, RGB255(255, 200, 140));
        }
        if (e >= HIT && !B->hit) { /* the blow */
            B->hit = 1;
            int big = B->b == last;
            float back = atan2f(-bd[1], -bd[0]);
            int got = c->s->aim_on && hypotf(T0[0] - B->lock[0], T0[1] - B->lock[1]) < fmaxf(.28f * sc, c->s->aim_hit) + .1f * sc; /* only if you are still where it aimed */
            if (!got) { /* missed: it slams down on nothing; a dull thud, a little dust */
                for (int i = 0; i < 12; i++) { float a = back + rnd(c->s, -1.4f, 1.4f), v = rnd(c->s, .5f, 1.5f), vel[3] = {cosf(a) * v * sc, sinf(a) * v * sc, rnd(c->s, -.3f, .3f)}; r_spark(c, f, B->lock, vel, rnd(c->s, .4f, .8f), 2, .6f, rnd(c->s, 1, 1.8f), SK_MOTE, 0, RGB255(150, 140, 130)); }
                feel(R, RF_RECOIL, B->n, .3f);
                float sp = fminf(24, hypotf(B->top[0] - L[0], B->top[1] - L[1]) / (HIT - HOLD));
                B->miss = 1; memcpy(B->base, at, sizeof B->base);
                for (int i = 0; i < 2; i++) B->vel[i] = bd[i] * sp; /* (across the screen only: the blow came down from */
                B->vel[2] = 0;                                    /* above it, and carrying that on sank it far behind, shrunk) */
                B->t0 = R->rt; memcpy(B->M0, f->Mf, sizeof B->M0); memcpy(B->R, Rp, sizeof B->R);
                B->ang = 0; B->w = rnd(c->s, -3, 3);
                continue;
            }
            atk_hit(c, f, R, B->lock, bd, RGB255(255, 196, 130), big ? 80 : 60, big ? 1.4f : 1, 1);
            for (int i = 0, n = big ? 38 : 28; i < n; i++) { float a = back + rnd(c->s, -1.35f, 1.35f), v = rnd(c->s, 1.1f, 3.4f), vel[3] = {cosf(a) * v * sc, sinf(a) * v * sc, rnd(c->s, -.4f, .4f)}; r_spark(c, f, B->lock, vel, rnd(c->s, .5f, 1.1f), 1.3f, 2.6f, rnd(c->s, 1, 2.3f), SK_STREAK, 1, RGB255(255, 150, 50)); }
            for (int i = 0; i < 9; i++) { float a = back + rnd(c->s, -.9f, .9f), vx = rnd(c->s, .25f, .7f), vy = rnd(c->s, .25f, .7f), vel[3] = {cosf(a) * vx * sc, sinf(a) * vy * sc, 0}; r_spark(c, f, B->lock, vel, rnd(c->s, 1.3f, 2.4f), .8f, .5f, rnd(c->s, 1.4f, 2.6f), SK_EMBER, 1, RGB255(255, 140, 40)); }
            forge_glow(c, B, 1);
            flash(c->m, c->sh->nv, .3f);
            feel(R, RF_RECOIL, B->n, big ? 1 : .7f);
            { float ax[3]; cross3(ax, nv, Z); norm3(ax); jerk(c->s, ax, big ? .07f : .05f, c->now); }
            { /* it shatters: each little triangle flies on from where it was in the clump, in a fan round the blow, each
               * turned as the clump was (R1 lays the clump's normal toward you, the twist about the view is the shard's
               * first spin angle), then floats and goes home in turn */
                float atv[3], cs[FORGE_TRIS][3], R1[9], d0[3] = {0, 0, 1}, on1[3];
                r_to_view(f, at, atv);
                for (int j = 0; j < B->nt; j++) {
                    float cm[3], dm[3], dv[3], o[3], cv[3];
                    tri_centre(c->sh, B->tris[j], cm);
                    for (int i = 0; i < 3; i++) dm[i] = cm[i] - B->mid[i];
                    m3_app(dv, f->Mf, dm); m3_app(o, Rp, dv);
                    for (int i = 0; i < 3; i++) cv[i] = atv[i] + o[i];
                    r_from_view(f, cv, cs[j]);
                }
                int n0 = R->nsh;
                turn_to(R1, nv, Z); m3_app(on1, R1, on);
                float tw = atan2f(bd[1], bd[0]) - atan2f(on1[1], on1[0]);
                shard_fan(c, f, R, B->tris, (const float (*)[3])cs, B->nt, at, atan2f(bd[1], bd[0]), big ? 4 : 3.4f, big ? 7 : 6, big ? 11 : 10, nv, d0, 1);
                for (int i = n0; i < R->nsh; i++) R->sh[i].ang = tw;
                B->broke = 1; p->st = RP_BROKE;
            }
        }
    }
    for (int i = 0; i < R->nsh; i++) shard_move(c, &R->sh[i], R->rt);
    for (int i = 0; i < R->nsh; i++) if (!R->sh[i].home) shard_place(c, f, &R->sh[i], R->rt);
    R->charge = fmaxf(R->charge, ch);
}

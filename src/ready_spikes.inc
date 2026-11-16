/* 6. spike cones (Bestiary's ATTACK.spikes, from Spike): a spike grows out of a node picked at random on your side,
 * the triangles round it a sharp cone; it tears off, turns its point at the pointer, trembles, and shoots point
 * first: the apex strikes, freeze, a spray, a jab. Three, one after another; each breaks into its little triangles,
 * which scatter on, float and fly home as the spike draws in. One you dodge does not break where you were: it flies
 * on, still a needle, slowing, and breaks out there. */
#define SPIKES_LEN .5f   /* the spike's length (the cone is drawn on the layer from the start, so nothing cuts it off) */
#define SPIKES_MAXT 8
#define SPIKES_ON 1.3f   /* how long a dodged needle flies on, still a needle, before it breaks */
#define SPIKES_DRAG 1.1f /* how fast it slows meanwhile: little, it carries on with its strike */
#define SPIKES_KEEP 9.f  /* the most speed it carries on with (stage units a second): it stays in sight */
/* the cone's own shape: drawn into the arrowhead it is charged as, and kept so, dodged too, until it breaks (it floats on
 * point first, its flight across the screen) */
static void spikes_shape(float *S, const float *axisV, float shp, float tau) { (void)tau; sharpen_m(S, axisV, shp); }
typedef struct {
    int v, nt, tris[SPIKES_MAXT], grown, shot, hit, has_from, on;
    float t0, aim[3], from[3], pos[3], vel[2], ton, len0, fire; /* fire: when it shoots, its own (no beat to learn, as the forge's charge) */
} SpikesCone;
typedef struct { int n; SpikesCone c[3]; } SpikesAtk;
_Static_assert(sizeof(SpikesAtk) <= sizeof(((ReadyRS *)0)->own), "SpikesAtk must fit in ReadyRS.own");

/* A flying cone is drawn with the shell's own triangles round its node, the spike's tip and all: if the spike drew back
 * (another spike taking its slot, anything that ends it early) the cone in flight went flat, a fan of triangles round
 * one point. So while it is out (fired, not yet broken) its spike is held out at the length it was fired with. */
static void spikes_hold(Ctx *c, const SpikesCone *C) {
    Motion *m = c->m;
    Spike *sp = NULL;
    for (int k = 0; k < MAX_SPIKE && !sp; k++) if (m->sp[k].v == C->v && m->sp[k].len != 0) sp = &m->sp[k];
    if (!sp) { spike_at(c->s, C->v, C->len0, 1, .7f, .15f, c->now - .7f); for (int k = 0; k < MAX_SPIKE && !sp; k++) if (m->sp[k].v == C->v && m->sp[k].len != 0) sp = &m->sp[k]; }
    if (!sp) return;
    sp->len = C->len0; sp->up = .7f; sp->at = c->now - .7f; sp->hd = 1; sp->dn = .15f; /* (fully out, held a while longer from now) */
}
static void spikes_start(Ctx *c, const RFrame *f, ReadyRS *R, const float *T) {
    const Shape *sh = c->sh;
    SpikesAtk *A = (SpikesAtk *)R->own.b;
    float d2[3] = {T[0] - f->wox, T[1] - f->woy, .45f}, key[MAX_V];
    norm3(d2);
    int order[MAX_V], pool[MAX_V], np = 0, nodes[3], nn = 0;
    for (int v = 0; v < sh->nv; v++) { float p[3]; m3_app(p, f->Mf, sh->v[v]); norm3(p); key[v] = dot3(p, d2); order[v] = v; }
    for (int i = 0; i < sh->nv; i++) for (int j = i + 1; j < sh->nv; j++) if (key[order[j]] > key[order[i]]) { int t = order[i]; order[i] = order[j]; order[j] = t; }
    for (int i = 0; i < sh->nv; i++) if (key[order[i]] > .2f) pool[np++] = order[i]; /* anywhere on your side, at random */
    for (int i = np - 1; i > 0; i--) { int j = (int)(mrand(c->s) * (i + 1)) % (i + 1), t = pool[i]; pool[i] = pool[j]; pool[j] = t; }
    if (np < 3) { np = sh->nv < 8 ? sh->nv : 8; memcpy(pool, order, sizeof(int) * (size_t)np); }
    for (int gap = 3; gap >= 1; gap--)
        for (int k = 0; k < np && nn < 3; k++) {
            int v = pool[k], ok = 1;
            for (int j = 0; j < nn && ok; j++) {
                if (nodes[j] == v) { ok = 0; break; }
                /* how many edges apart (Bestiary's G.vdist), by a search out from v no deeper than gap */
                unsigned char dist[MAX_V]; int q[MAX_V], qh = 0, qt = 0, near = 0;
                memset(dist, 255, sizeof dist); dist[v] = 0; q[qt++] = v;
                while (qh < qt && !near) {
                    int x = q[qh++];
                    if (dist[x] + 1 >= gap) continue;
                    for (int a = 0; a < sh->nadj[x]; a++) { int y = sh->vn[x][a]; if (dist[y] == 255) { dist[y] = (unsigned char)(dist[x] + 1); if (y == nodes[j]) near = 1; q[qt++] = y; } }
                }
                if (near) ok = 0;
            }
            if (ok) nodes[nn++] = v;
        }
    A->n = nn; R->n = 0;
    for (int i = 0; i < nn; i++) {
        SpikesCone *C = &A->c[i];
        C->v = nodes[i]; C->t0 = i ? A->c[i - 1].t0 + rnd(c->s, .35f, .75f) : 0; C->nt = 0;
        C->fire = rnd(c->s, .6f, 1.4f); /* trembling, aimed, for a time of its own before it shoots */
        for (int ti = 0; ti < sh->nt && C->nt < SPIKES_MAXT; ti++) if (sh->t[ti][0] == C->v || sh->t[ti][1] == C->v || sh->t[ti][2] == C->v) C->tris[C->nt++] = ti;
        RPiece *p = &R->p[R->n++]; /* stands for the cone until it breaks, so the attack runs while it is out */
        memset(p, 0, sizeof *p);
        p->ti = C->tris[0]; p->i = i; p->st = RP_MORPH; p->s = 1;
    }
}

static void spikes_run(Ctx *c, const RFrame *f, ReadyRS *R, const float *T, float t, float dt) {
    SpikesAtk *A = (SpikesAtk *)R->own.b;
    const Shape *sh = c->sh;
    Motion *m = c->m;
    float T0[3] = {T[0], T[1], 0}, z[3] = {0, 0, 0};
    { float ch = 0; /* winding up while a cone tears off and aims */
      for (int i = 0; i < A->n; i++) { float e = t - A->c[i].t0, F = A->c[i].fire; if (e > 0 && e < F) ch = fmaxf(ch, .3f + .7f * io3(clampf((e - .2f) / (F - .2f), 0, 1))); }
      for (int i = 0; i < A->n; i++) if (A->c[i].on && !A->c[i].hit) ch = 1; /* (a dodged cone flies on still charged: its glow, not a cold white needle) */
      R->charge = fmaxf(R->charge, ch); }
    for (int i = 0; i < A->n; i++) {
        SpikesCone *C = &A->c[i];
        if (t >= C->t0 && !C->grown) { C->grown = 1; spike_at(c->s, C->v, SPIKES_LEN, .7f, .2f, .15f, c->now); }
        const float F = C->fire, FH = C->fire + .12f; /* it shoots at F, and strikes at FH */
        if (t - C->t0 >= F && !C->shot) { /* it shoots at where you are now */
            C->shot = 1;
            float d[3] = {T[0] - f->wox, T[1] - f->woy, 0}; norm3(d);
            feel(R, RF_LUNGE, d, .5f);
            memcpy(C->aim, T0, sizeof C->aim);
        }
        float e = t - C->t0;
        if (e < 0) continue;
        /* growing in place (the spike itself does it), already drawn as its own piece, so the core's tile never cuts it off */
        float pv = m->push[C->v], len = fmaxf(.02f, pv != 0 ? pv : SPIKES_LEN);
        if (e >= F) { if (C->len0 <= 0) C->len0 = len; len = C->len0; if (!C->hit) spikes_hold(c, C); } /* fired: its length kept, its spike held out (as the spike on the core draws back
                                                                            * in, a flying cone flattened with it into a squat hexagon) */
        float apexM[3], apexV[3], apexS[3], axisV[3], toT[3], goal[3], at[3], Rt[9];
        for (int k = 0; k < 3; k++) apexM[k] = sh->v[C->v][k] * (1 + len);
        m3_app(apexV, f->Mf, apexM); r_from_view(f, apexV, apexS);
        m3_app(axisV, f->Mf, sh->v[C->v]); norm3(axisV);
        if (C->shot) { const float *fr = C->has_from ? C->from : apexS; for (int k = 0; k < 3; k++) toT[k] = C->aim[k] - fr[k]; memcpy(goal, C->aim, sizeof goal); }
        else { for (int k = 0; k < 3; k++) toT[k] = T0[k] - apexS[k]; memcpy(goal, T0, sizeof goal); }
        norm3(toT);
        float fdir[3] = {toT[0], toT[1], toT[2]}; /* which way it points: its aim (which leans into or out of the screen, the
                                                  * point starting on the shell's front), or once it flies on, its flight across
                                                  * the screen, so it is seen side on (pointed into the screen it showed as a
                                                  * fan of lines from one point) */
        if (C->on) { float vl = hypotf(C->vel[0], C->vel[1]); if (vl > 1e-4f) set3(fdir, C->vel[0] / vl, C->vel[1] / vl, 0); else { fdir[2] = 0; norm3(fdir); } }
        float outV[3], outS[3]; /* the apex drawn .55 out along the spike: where it tears off to */
        if (e < .55f) {
            float v = io3(fmaxf(0, e - .2f) / .35f), to[3]; /* tears off, turning its point at you */
            for (int k = 0; k < 3; k++) to[k] = axisV[k] + (toT[k] - axisV[k]) * v;
            norm3(to); turn_to(Rt, axisV, to);
            for (int k = 0; k < 3; k++) outV[k] = apexV[k] + axisV[k] * .55f * v * CIRCUM;
            r_from_view(f, outV, at);
        } else if (e < F) { /* trembles, aimed */
            turn_to(Rt, axisV, toT);
            for (int k = 0; k < 3; k++) outV[k] = apexV[k] + axisV[k] * .55f * CIRCUM;
            r_from_view(f, outV, at);
            at[0] += .012f * sinf(e * 95); at[1] += .012f * cosf(e * 83);
        } else if (e < FH) { /* shoots, point first */
            turn_to(Rt, axisV, toT);
            if (!C->has_from) { for (int k = 0; k < 3; k++) outV[k] = apexV[k] + axisV[k] * .55f * CIRCUM; r_from_view(f, outV, outS); memcpy(C->from, outS, sizeof C->from); C->has_from = 1; }
            float u = in3((e - F) / .12f);
            for (int k = 0; k < 3; k++) at[k] = C->from[k] + (goal[k] - C->from[k]) * u;
        } else if (C->hit) continue; /* broken: its little triangles are moved after the loop */
        else if (C->on) { /* dodged: flying on along its flight, slowing */
            float k = expf(-dt * SPIKES_DRAG);
            for (int q = 0; q < 2; q++) { C->vel[q] *= k; C->pos[q] += C->vel[q] * dt; }
            turn_to(Rt, axisV, fdir); memcpy(at, C->pos, sizeof at);
        } else { turn_to(Rt, axisV, toT); memcpy(at, goal, sizeof at); }
        if (i < R->n) memcpy(R->p[i].pos, at, sizeof at); /* its stand-in piece rides on its point: READY's layer grows round it
                                                            * wherever it flies (left at the core, a dodged needle flew out of it) */
        float shp = e < .55f ? io3(fmaxf(0, e - .2f) / .35f) : 1; /* drawn into a needle as it tears off, and kept so until it breaks */
        float fly = C->on && !C->hit ? 1 : 0; /* dodged: in the shape it was charged in, floating on point first */
        { float S[9], Rg[9], atV[3], aM[3];
          spikes_shape(S, axisV, shp, fly > 0 ? e - C->ton : -1);
          m3_mul(Rg, Rt, S);
          r_to_view(f, at, atV); m3_app(aM, f->Mf, apexM);
          R_ANC = atV; /* (one perspective for the whole cone, about its point: it stays one piece) */
          for (int j = 0; j < C->nt; j++) { /* the cone as one piece: its apex on at */
              float cm[3], cv[3], d[3], Rd[3], tv[3];
              tri_centre(sh, C->tris[j], cm); m3_app(cv, f->Mf, cm);
              for (int k = 0; k < 3; k++) d[k] = cv[k] - aM[k];
              m3_app(Rd, Rg, d);
              for (int k = 0; k < 3; k++) tv[k] = atV[k] + Rd[k];
              r_place(c, f, C->tris[j], tv, Rg);
          }
          R_ANC = NULL; }
        if ((e > .4f && e < FH) || (fly > 0 && !C->hit)) { m->glow[C->v] = 1; if (mrand(c->s) < .5f) r_spark(c, f, at, z, .06f, 2, 0, 10, SK_FLARE, 0, RGB255(230, 240, 255)); } /* a glint on the point */
        if ((e >= F && e < FH) || (C->on && !C->hit && mrand(c->s) < .6f)) { float v[3] = {-toT[0], -toT[1], -toT[2]}; r_spark(c, f, at, v, .12f, 3, 0, 1.2f, SK_STREAK, 0, RGB255(225, 236, 255)); }
        int got = 0, brk = 0;
        if (!C->hit && e >= FH && !C->on) { /* you were still there, or you dodged */
            const float *fr = C->has_from ? C->from : apexS;
            float ab[2] = {goal[0] - fr[0], goal[1] - fr[1]}, l2 = ab[0] * ab[0] + ab[1] * ab[1];
            if (l2 < 1e-9f) l2 = 1e-9f;
            float su = clampf(((T0[0] - fr[0]) * ab[0] + (T0[1] - fr[1]) * ab[1]) / l2, 0, 1);
            got = c->s->aim_on && hypotf(T0[0] - fr[0] - ab[0] * su, T0[1] - fr[1] - ab[1] * su) < fmaxf(.28f * f->sc, c->s->aim_hit);
            if (got) { brk = 1; atk_hit(c, f, R, T0, toT, RGB255(235, 242, 255), 44, .8f, 0); }
            else { /* it keeps the speed it struck with (in3's end: 3 x the flight over its .12 s), up to a limit */
                float sp = fminf(3 * sqrtf(l2) / .12f, SPIKES_KEEP), dl = hypotf(toT[0], toT[1]);
                if (dl < 1e-4f) dl = 1e-4f;
                C->on = 1; C->ton = e; memcpy(C->pos, at, sizeof C->pos);
                /* its spike stays out until it breaks: the flying cone is drawn with the spike's own triangles, and as the
                 * spike drew back into the core it collapsed into a squat pyramid */
                for (int k = 0; k < MAX_SPIKE; k++) { Spike *sp = &m->sp[k]; if (sp->v == C->v && sp->len != 0 && c->now - sp->at < sp->up + sp->hd + sp->dn) sp->hd = (float)(c->now - sp->at) - sp->up + SPIKES_ON + .05f; }
                C->vel[0] = toT[0] / dl * sp; C->vel[1] = toT[1] / dl * sp;
            }
        } else if (!C->hit && C->on && e - C->ton >= SPIKES_ON) { /* spent: it breaks out there, and its spike draws back in */
            brk = 1;
            for (int k = 0; k < MAX_SPIKE; k++) { Spike *sp = &m->sp[k]; if (sp->v == C->v && sp->len != 0 && c->now - sp->at < sp->up + sp->hd + sp->dn) sp->hd = fmaxf(0, (float)(c->now - sp->at) - sp->up); }
        }
        if (brk) {
            C->hit = 1;
            for (int k = 0; k < MAX_SPIKE; k++) { Spike *sp = &m->sp[k]; if (sp->v == C->v && sp->len != 0 && c->now - sp->at < sp->up + sp->hd + sp->dn) sp->hd = fmaxf(0, (float)(c->now - sp->at) - sp->up); } /* broken (hit or spent): its spike draws back in */
            float rest = C->on ? hypotf(C->vel[0], C->vel[1]) : 0; /* what it still carries */
            float R1[9], S1[9], aM[3], atV[3], cs[SPIKES_MAXT][3]; /* it breaks: each little triangle flies on from where it was in the needle */
            turn_to(R1, axisV, fdir); spikes_shape(S1, axisV, shp, C->on ? e - C->ton : -1); m3_mul(R1, R1, S1); /* (the cone as drawn this frame, so nothing jumps) */
            m3_app(aM, f->Mf, apexM); r_to_view(f, at, atV);
            for (int j = 0; j < C->nt; j++) {
                float cm[3], cv[3], d[3], Rd[3], pv3[3];
                tri_centre(sh, C->tris[j], cm); m3_app(cv, f->Mf, cm);
                for (int k = 0; k < 3; k++) d[k] = cv[k] - aM[k];
                m3_app(Rd, R1, d);
                for (int k = 0; k < 3; k++) pv3[k] = atV[k] + Rd[k];
                r_from_view(f, pv3, cs[j]);
            }
            int n0 = R->nsh;
            shard_fan(c, f, R, C->tris, (const float (*)[3])cs, C->nt, at, atan2f(toT[1], toT[0]), got ? 4.4f : 2.4f, got ? 8 : 5, got ? 10 : 7, axisV, toT, 1);
            { /* A needle shot point first does not burst evenly round its point (a ring like that read as a perfect hexagon):
               * it carries on along its flight. Each piece keeps most of that, the ones nearer the point the most; it is
               * thrown off to its own side of the needle by a random amount, more on a hit (it splashes off you) than on
               * a miss, at its own speed, spin and drag. */
                float fw[2] = {toT[0], toT[1]}, fl = hypotf(fw[0], fw[1]);
                if (fl < 1e-4f) { fw[0] = 1; fw[1] = 0; } else { fw[0] /= fl; fw[1] /= fl; }
                float sd[2] = {-fw[1], fw[0]}, reach = 0, cen[2] = {0, 0};
                for (int q = n0; q < R->nsh; q++) { const RShard *Q = &R->sh[q]; reach = fmaxf(reach, hypotf(Q->base[0] - at[0], Q->base[1] - at[1])); cen[0] += Q->base[0]; cen[1] += Q->base[1]; }
                if (R->nsh > n0) { cen[0] /= (float)(R->nsh - n0); cen[1] /= (float)(R->nsh - n0); }
                if (reach < 1e-4f) reach = 1e-4f;
                for (int q = n0; q < R->nsh; q++) {
                    RShard *Q = &R->sh[q];
                    float ox = Q->base[0] - at[0], oy = Q->base[1] - at[1], lat = ox * sd[0] + oy * sd[1], near = 1 - hypotf(ox, oy) / reach; /* near: 1 at the point */
                    float side = lat > .02f ? 1 : lat < -.02f ? -1 : (mrand(c->s) < .5f ? -1 : 1);
                    if (got) { /* on you: a tight tangle that splashes off, bursting apart from the needle's middle */
                        float fwd = rnd(c->s, 4, 9) * (.55f + .65f * near), sp = 3.8f, out = side * rnd(c->s, 0, .6f * sp) + (rnd(c->s, -1, 1) + rnd(c->s, -1, 1)) * .5f * sp;
                        Q->vel[0] = fw[0] * fwd + sd[0] * out; Q->vel[1] = fw[1] * fwd + sd[1] * out; Q->vel[2] = 0;
                        float rx = Q->base[0] - cen[0], ry = Q->base[1] - cen[1], rl = hypotf(rx, ry), g = rnd(c->s, -.7f, .7f);
                        if (rl < 1e-3f) { float a = rnd(c->s, 0, TAU); rx = cosf(a); ry = sinf(a); } else { rx /= rl; ry /= rl; }
                        float bx = rx * cosf(g) - ry * sinf(g), by = rx * sinf(g) + ry * cosf(g), bs = rnd(c->s, 2, 3.5f);
                        Q->vel[0] += bx * bs; Q->vel[1] += by * bs;
                    } else { /* spent out there: it snaps into its little triangles, which fly apart along its flight, each
                              * on its own heading across a wide fan, at its own speed (no burst from its middle: that opened
                              * into the same hexagon), far enough that each reads as its own triangle */
                        float fwd = (rest * rnd(c->s, .4f, 1.1f) + rnd(c->s, 2.2f, 4.8f)) * (.7f + .4f * near), vr = side * rnd(c->s, .15f, 1.25f) + rnd(c->s, -.25f, .25f);
                        Q->vel[0] = (fw[0] * cosf(vr) - fw[1] * sinf(vr)) * fwd; Q->vel[1] = (fw[0] * sinf(vr) + fw[1] * cosf(vr)) * fwd; Q->vel[2] = 0;
                        Q->wait = rnd(c->s, 0, .12f);
                    }
                    Q->w = (mrand(c->s) < .5f ? 1 : -1) * rnd(c->s, got ? 1.5f : 3, got ? 9 : 12); Q->ang = rnd(c->s, got ? -.6f : -2.5f, got ? .6f : 2.5f);
                    Q->drag = got ? rnd(c->s, 2.8f, 6.5f) : rnd(c->s, 1.6f, 3); Q->bob = rnd(c->s, .3f, 1.7f); /* (spent: less drag, so they part) */
                }
            }
            for (int k = 0, n = got ? 10 : 5; k < n; k++) {
                float g = rnd(c->s, 0, TAU), v = rnd(c->s, .8f, 2.2f), vel[3] = {cosf(g) * v, sinf(g) * v, rnd(c->s, -.3f, .3f)};
                r_spark(c, f, at, vel, rnd(c->s, .25f, .5f), 2.5f, 0, rnd(c->s, .9f, 1.5f), SK_STREAK, got, got ? RGB255(225, 236, 255) : RGB255(170, 180, 200));
            }
            if (i < R->n) R->p[i].st = RP_BROKE;
        }
        for (int j = 0; j < C->nt; j++) r_glow(c, C->tris[j], e > .55f && (e < FH || C->on) ? .5f : .2f);
    }
    for (int i = 0; i < R->nsh; i++) shard_move(c, &R->sh[i], R->rt);
    for (int i = 0; i < R->nsh; i++) if (!R->sh[i].home) shard_place(c, f, &R->sh[i], R->rt);
}

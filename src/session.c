/* Session state machine and the per-state look. Events move a session between states; each frame the
 * drawn look eases toward the look of the state it is in. */
#include "dodeca.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define HOLD_WORK 0.6    /* a tool that finishes at once still shows as work for this long */
#define CHEER_FOR 2.5
#define SLEEP_AFTER 90.0
#define DROP_AFTER 1800.0 /* a session silent this long is forgotten */

const char *STATE_NAME[ST_N] = {"IDLE", "THINK", "WORK", "NEEDS YOU", "DONE", "SLEEP", "SPEAKING", "LISTENING", "TRANSCRIBING", "STANDBY", "ERROR"};

#define C(r, g, b) {(r) / 255.0f, (g) / 255.0f, (b) / 255.0f}
#define PURE C(255, 255, 255)
#define BLUE C(105, 160, 255)
#define PALE C(255, 238, 206)
#define AMBER C(255, 172, 64)
#define RED C(255, 58, 48)
#define NEARWHITE C(255, 246, 244)
#define PINK C(255, 150, 140)
#define TEAL C(80, 225, 205)
#define GREEN C(110, 255, 160)
#define VIOLET C(175, 135, 255)
#define INDIGO C(110, 105, 215)
#define ORANGE C(255, 120, 30)

/*                rot   core  hz    depth edge  flow  hr   glitch  core body  core glow  line   line halo  lc    lh
 * lc / lh: how much the speech or microphone level drives core brightness / glow size (panel states only) */
const Look LOOKS[ST_N] = {
    [ST_IDLE]  = {0.16f, 0.72f, 0.15f, 0.08f, 0.78f, 0.00f, 4.5f, 0, PURE, BLUE, PURE, BLUE, 0, 0},
    [ST_THINK] = {0.36f, 0.92f, 0.70f, 0.32f, 0.86f, 0.00f, 6.0f, 0, PALE, AMBER, AMBER, AMBER, 0, 0},
    [ST_WORK]  = {0.95f, 1.15f, 2.20f, 0.12f, 1.00f, 1.00f, 4.5f, 0, PURE, BLUE, PURE, BLUE, 0, 0},
    [ST_WAIT]  = {0.10f, 1.00f, 1.00f, 0.60f, 0.60f, 0.45f, 6.0f, 1, NEARWHITE, RED, PINK, RED, 0, 0},
    [ST_CHEER] = {1.60f, 1.60f, 0.00f, 0.00f, 1.20f, 1.00f, 4.5f, 0, PURE, BLUE, PURE, BLUE, 0, 0},
    [ST_SLEEP] = {0.04f, 0.26f, 0.08f, 0.30f, 0.30f, 0.00f, 4.5f, 0, PURE, BLUE, PURE, BLUE, 0, 0},
    /* the panel's own states: reporting by voice, push-to-talk, turning speech into text, you are away, sending failed */
    [ST_SPEAK]      = {0.12f, 0.90f, 0.00f, 0.00f, 0.85f, 0.00f, 6.0f, 0, PURE, TEAL, PURE, TEAL, 0.9f, 0.0f},
    [ST_LISTEN]     = {0.00f, 0.80f, 0.00f, 0.00f, 0.90f, 0.00f, 4.5f, 0, PURE, GREEN, GREEN, GREEN, 0.3f, 4.0f},
    [ST_TRANSCRIBE] = {0.20f, 0.90f, 1.60f, 0.25f, 0.90f, 2.60f, 5.0f, 0, PURE, VIOLET, VIOLET, VIOLET, 0, 0},
    [ST_STANDBY]    = {0.03f, 0.20f, 0.05f, 0.40f, 0.22f, 0.00f, 4.0f, 0, PURE, INDIGO, PURE, INDIGO, 0, 0},
    [ST_ERROR]      = {0.05f, 0.95f, 0.80f, 0.50f, 0.75f, 0.00f, 5.0f, 1, NEARWHITE, ORANGE, ORANGE, ORANGE, 0, 0}, /* a voice reply could not be sent */
};

/* pointed at: it brightens, slows down and turns a little toward the cursor, as if looking at you */
const Hover HOVER = {.bright = 0.35f, .slow = 0.7f, .tilt = 0.32f, .gaze = 0.10f, .ease = 7.0f};
const Touch TOUCH = {.unread_hz = 0.5f, .unread_amp = 0.45f, .ring_for = 1.0f, .vol_for = 1.2f, .coast = 1.2f, .aura = 0.6f};

/* single-core mode: who is who, by the colour of the core (the shell keeps the state colours) */
const Col HUES[NHUE] = {C(80, 200, 255), C(255, 120, 200), C(140, 235, 110), C(255, 190, 70),
                        C(170, 140, 255), C(70, 230, 200), C(255, 110, 90), C(230, 230, 120)};

static uint32_t hash_str(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

static float frand(uint32_t *st) { /* xorshift, 0..1 */
    uint32_t x = *st;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *st = x ? x : 1;
    return (float)(*st >> 8) / 16777216.0f;
}

void session_init(Session *s, const char *sid, double t, const Shape *sh) {
    memset(s, 0, sizeof *s);
    snprintf(s->sid, sizeof s->sid, "%s", sid);
    s->created = s->last = s->state_t = t;
    s->burst_at = s->ring_at = s->vol_at = -1e9;
    s->vol = 0.8f;
    s->state = ST_IDLE;
    s->cur = LOOKS[ST_IDLE];
    s->rng = hash_str(sid) | 1; /* seeded by the id: a session always moves the same way */
    s->ay = frand(&s->rng) * 6.283f;
    s->ax = 0.35f + frand(&s->rng) * 6.283f;
    s->phase = frand(&s->rng) * 100.0;
    for (int i = 0; i < NPULSE; i++) {
        s->p[i].e = sh && sh->ne ? (int)(frand(&s->rng) * sh->ne) % sh->ne : 0;
        s->p[i].dir = 1;
        s->p[i].t = frand(&s->rng);
        s->p[i].sp = 0.9f + frand(&s->rng) * 0.9f;
    }
}

static Session *find(Room *r, const char *sid) {
    for (int i = 0; i < r->n; i++) if (!strcmp(r->s[i].sid, sid)) return &r->s[i];
    return NULL;
}

static void drop(Room *r, Session *s) {
    int i = (int)(s - r->s);
    memmove(s, s + 1, sizeof(Session) * (size_t)(r->n - i - 1));
    r->n--;
}

static void set_state(Session *s, int st, double t) { s->state = st; s->state_t = t; }

void room_move(Room *r, int from, int to) {
    if (from < 0 || to < 0 || from >= r->n || to >= r->n || from == to) return;
    Session tmp = r->s[from];
    if (from < to) memmove(&r->s[from], &r->s[from + 1], sizeof(Session) * (size_t)(to - from));
    else memmove(&r->s[to + 1], &r->s[to], sizeof(Session) * (size_t)(from - to));
    r->s[to] = tmp;
}

void room_apply(Room *r, const Event *e) {
    Session *s = find(r, e->sid);
    const char *ev = e->ev;
    if (!strcmp(ev, "SessionEnd")) { if (s) drop(r, s); return; }
    /* Claude Code also notifies when a prompt has sat idle; that is not a request for approval */
    if (!strcmp(ev, "Notification") && !strcmp(e->ntype, "idle_prompt")) return;
    if (!s) {
        if (r->n >= MAX_SESS) return;
        s = &r->s[r->n++];
        session_init(s, e->sid, e->t, NULL);
    }
    s->last = e->t;
    if (e->cwd[0]) snprintf(s->name, sizeof s->name, "%s", e->cwd);
    if (!strcmp(ev, "SessionStart")) set_state(s, ST_IDLE, e->t);
    else if (!strcmp(ev, "UserPromptSubmit")) { set_state(s, ST_THINK, e->t); s->unread = 0; }
    else if (!strcmp(ev, "PreToolUse")) {
        snprintf(s->tool, sizeof s->tool, "%s", e->tool);
        set_state(s, ST_WORK, e->t);
        s->hold_work_until = e->t + HOLD_WORK;
    } else if (!strcmp(ev, "PostToolUse")) {
        set_state(s, ST_THINK, e->t);
        if (e->t + HOLD_WORK > s->hold_work_until) s->hold_work_until = e->t + HOLD_WORK;
    } else if (!strcmp(ev, "Notification")) set_state(s, ST_WAIT, e->t);
    else if (!strcmp(ev, "Stop")) { set_state(s, ST_CHEER, e->t); s->burst_at = e->t; if (e->msg) s->unread = 1; }
    else if (!strcmp(ev, "_idle")) set_state(s, ST_IDLE, e->t);   /* (the built-in demo) */
    else if (!strcmp(ev, "_sleep")) set_state(s, ST_SLEEP, e->t);
}

/* the state to draw, after the small delays that keep it calm */
int session_shown(const Session *s, double now) {
    if (s->overlay > 0 && s->overlay < ST_N) return s->overlay;
    if (s->state == ST_THINK && now < s->hold_work_until) return ST_WORK;
    if (s->state == ST_CHEER && now - s->state_t > CHEER_FOR) return ST_IDLE;
    if (s->state == ST_IDLE && now - s->last > SLEEP_AFTER) return ST_SLEEP;
    return s->state;
}

static void ease(float *a, float b, float k) { *a += (b - *a) * k; }
static void ease_col(Col *a, Col b, float k) { ease(&a->r, b.r, k); ease(&a->g, b.g, k); ease(&a->b, b.b, k); }

void room_tick(Room *r, const Shape *sh, double now, float dt) {
    for (int i = 0; i < r->n;) {
        if (now - r->s[i].last > DROP_AFTER) drop(r, &r->s[i]);
        else i++;
    }
    float k = 1.0f - expf(-dt * 4.0f);
    for (int i = 0; i < r->n; i++) {
        Session *s = &r->s[i];
        const Look *L = &LOOKS[session_shown(s, now)];
        Look *c = &s->cur;
        ease(&c->rot, L->rot, k); ease(&c->core, L->core, k); ease(&c->hz, L->hz, k); ease(&c->depth, L->depth, k);
        ease(&c->edge, L->edge, k); ease(&c->flow, L->flow, k); ease(&c->hr, L->hr, k); ease(&c->glitch, L->glitch, k);
        ease_col(&c->col, L->col, k); ease_col(&c->halo, L->halo, k);
        ease_col(&c->line, L->line, k); ease_col(&c->eh, L->eh, k);
        ease(&c->lc, L->lc, k); ease(&c->lh, L->lh, k);
        float kh = 1.0f - expf(-dt * HOVER.ease);
        ease(&s->hover, s->hover_to, kh);
        ease(&s->look_x, s->look_x_to * s->hover_to, kh);
        ease(&s->look_y, s->look_y_to * s->hover_to, kh);
        float spin = c->rot * (1 - HOVER.slow * s->hover), fade = expf(-dt * TOUCH.coast);
        if (!s->held) { /* while dragged the panel turns it; after that a fling fades back into its own spin */
            s->ay += (spin + s->spin_y) * dt;
            s->ax += (spin * 0.37f + s->spin_x) * dt;
            s->spin_x *= fade;
            s->spin_y *= fade;
        }
        if (c->flow > 0.03f) {
            for (int j = 0; j < NPULSE; j++) {
                Pulse *p = &s->p[j];
                if (p->e >= sh->ne) p->e = 0;
                p->t += p->sp * dt * (0.4f + c->flow);
                if (p->t < 1.0f) continue;
                /* at the end of its edge a pulse turns onto one of the other edges that meet there */
                int end = sh->e[p->e][p->dir > 0 ? 1 : 0], pick = (int)(frand(&s->rng) * sh->nadj[end]) % sh->nadj[end];
                if (sh->adj[end][pick] == p->e) pick = (pick + 1) % sh->nadj[end];
                p->e = sh->adj[end][pick];
                p->dir = sh->e[p->e][0] == end ? 1 : -1;
                p->t = 0;
            }
        }
    }
}

const char *STATE_TEXT[ST_N] = {"Idle", "Thinking", "Working", "Needs approval", "Done", "Sleeping",
                               "Speaking", "Listening", "Transcribing", "Standby", "Error"};

/* How often a session's picture is redrawn. A calm session moves slowly, so it needs far fewer frames.
 * A state change gets the full rate for a moment so the colours ease smoothly. */
double session_rate(const Session *s, double now, int fps) {
    static const double RATE[ST_N] = {[ST_IDLE] = 10, [ST_THINK] = 15, [ST_WORK] = 20, [ST_WAIT] = 20, [ST_CHEER] = 20, [ST_SLEEP] = 4,
                                      [ST_SPEAK] = 20, [ST_LISTEN] = 20, [ST_TRANSCRIBE] = 20, [ST_STANDBY] = 4, [ST_ERROR] = 20};
    int busy = s->hover_to > 0 || s->hover > 0.01f || s->held || fabsf(s->spin_x) + fabsf(s->spin_y) > 0.02f
               || now - s->ring_at < TOUCH.ring_for || now - s->vol_at < TOUCH.vol_for; /* the mouse is doing something */
    double r = now - s->state_t < 1.5 || busy ? 20 : RATE[session_shown(s, now)];
    if (s->unread && r < 12) r = 12; /* the flash of an unread reply */
    return r < fps ? r : fps;
}

/* ---- the built-in demo -------------------------------------------------------------------------- */
static const struct { const char *ev, *tool; double dur; } SCRIPT[] = {
    {"_idle", "", 2.5}, {"UserPromptSubmit", "", 2.0}, {"PreToolUse", "Read", 2.0}, {"PostToolUse", "", 1.2},
    {"PreToolUse", "Edit", 2.6}, {"PostToolUse", "", 1.0}, {"Notification", "", 3.2}, {"PreToolUse", "Bash", 2.4},
    {"Stop", "", 6.5}, {"_sleep", "", 3.5},
};
#define NSCRIPT ((int)(sizeof SCRIPT / sizeof SCRIPT[0]))
static const char *DEMO_NAME[] = {"radio-sim", "door-lock", "notes", "firmware", "antenna", "scratch"};

void demo_start(Demo *d, int n, double now) {
    memset(d, 0, sizeof *d);
    d->n = n < 1 ? 1 : n > 6 ? 6 : n;
    for (int i = 0; i < d->n; i++) d->next[i] = now + i * 3.7;
}

void demo_run(Demo *d, Room *room, double now) {
    for (int i = 0; i < d->n; i++) {
        while (now >= d->next[i]) {
            Event e;
            memset(&e, 0, sizeof e);
            e.t = d->next[i];
            snprintf(e.sid, sizeof e.sid, "demo-%d", i);
            snprintf(e.cwd, sizeof e.cwd, "%s", DEMO_NAME[i]);
            snprintf(e.ev, sizeof e.ev, "%s", SCRIPT[d->step[i] % NSCRIPT].ev);
            snprintf(e.tool, sizeof e.tool, "%s", SCRIPT[d->step[i] % NSCRIPT].tool);
            e.msg = !strcmp(e.ev, "Stop"); /* every demo session has a (made-up) reply */
            room_apply(room, &e);
            d->next[i] += SCRIPT[d->step[i] % NSCRIPT].dur;
            d->step[i]++;
        }
    }
}

/* Session state machine and the per-state look. Events move a session between states; each frame the
 * drawn look eases toward the look of the state it is in. */
#include "vigil.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define HOLD_WORK 0.6    /* a tool that finishes at once still shows as work for this long */
#define CHEER_FOR 2.5
#define SLEEP_AFTER 90.0
#define DROP_AFTER 1800.0 /* a session silent this long is forgotten */
#define SUB_KEEP 20.0     /* a finished subagent stays this long (unless SUB_UNTIL_SEEN) */
int SUB_UNTIL_SEEN;

const char *STATE_NAME[ST_N] = {"IDLE", "THINK", "WORK", "NEEDS YOU", "DONE", "SLEEP", "SPEAKING", "LISTENING", "TRANSCRIBING", "STANDBY", "ERROR", "READY"};

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
#define CYAN C(40, 200, 255)
#define ICE C(165, 235, 255)

/*                rot   core  hz    depth edge  flow  hr   glitch  core body  core glow  line   line halo  lc    lh
 * lc / lh: how much the speech or microphone level drives core brightness / glow size (panel states only) */
const Look LOOKS[ST_N] = {
    [ST_IDLE]  = {0.16f, 0.72f, 0.15f, 0.08f, 0.78f, 0.00f, 4.5f, 0, PURE, BLUE, PURE, BLUE, 0, 0},
    [ST_THINK] = {0.36f, 0.92f, 0.70f, 0.32f, 0.86f, 0.00f, 6.0f, 0, PALE, AMBER, AMBER, AMBER, 0, 0},
    [ST_WORK]  = {0.95f, 1.15f, 2.20f, 0.12f, 1.00f, 1.00f, 4.5f, 0, PURE, CYAN, ICE, CYAN, 0, 0}, /* electric cyan: told apart from IDLE's soft blue */
    [ST_WAIT]  = {0.10f, 1.00f, 1.00f, 0.60f, 0.60f, 0.45f, 6.0f, 1, NEARWHITE, ORANGE, ORANGE, ORANGE, 0, 0},
    [ST_CHEER] = {1.60f, 1.60f, 0.00f, 0.00f, 1.20f, 1.00f, 4.5f, 0, PURE, BLUE, PURE, BLUE, 0, 0},
    [ST_SLEEP] = {0.04f, 0.26f, 0.08f, 0.30f, 0.30f, 0.00f, 4.5f, 0, PURE, BLUE, PURE, BLUE, 0, 0},
    /* the panel's own states: reporting by voice, push-to-talk, turning speech into text, you are away, sending failed */
    [ST_SPEAK]      = {0.12f, 0.90f, 0.00f, 0.00f, 0.85f, 0.00f, 6.0f, 0, PURE, TEAL, PURE, TEAL, 0.9f, 0.0f},
    [ST_LISTEN]     = {0.00f, 0.80f, 0.00f, 0.00f, 0.90f, 0.00f, 4.5f, 0, PURE, GREEN, GREEN, GREEN, 0.3f, 4.0f},
    [ST_TRANSCRIBE] = {0.20f, 0.90f, 1.60f, 0.25f, 0.90f, 2.60f, 5.0f, 0, PURE, VIOLET, VIOLET, VIOLET, 0, 0},
    [ST_STANDBY]    = {0.03f, 0.20f, 0.05f, 0.40f, 0.22f, 0.00f, 4.0f, 0, PURE, INDIGO, PURE, INDIGO, 0, 0},
    [ST_ERROR]      = {0.05f, 0.95f, 0.80f, 0.50f, 0.75f, 0.00f, 5.0f, 1, NEARWHITE, RED, PINK, RED, 0, 0}, /* a voice reply could not be sent */
    [ST_READY]      = {0.16f, 0.72f, 0.15f, 0.08f, 0.78f, 0.00f, 4.5f, 0, PURE, BLUE, PURE, BLUE, 0, 0}, /* IDLE's look; it reddens as it winds up (Motion.rage) */
};

/* pointed at: it brightens, slows down and turns a little toward the cursor, as if looking at you */
const Hover HOVER = {.bright = 0.35f, .slow = 0.7f, .tilt = 0.32f, .gaze = 0.10f, .ease = 7.0f};
/* carried: it lights up, leans into the motion, the core sloshes behind, and a hard shake makes it dizzy */
/* damp = 2·√spring: critically damped, so the core follows the motion and settles without bouncing */
const Carry CARRY = {.lift = 0.35f, .ease = 10.0f, .lean = 0.035f, .lean_max = 0.55f, .stretch = 0.12f,
                     .spring = 170.0f, .damp = 26.0f, .lag = 0.20f, .jig_max = 0.42f,
                     .shake_gain = 0.0022f, .shake_fade = 1.4f, .shake_spin = 9.0f, .shake_glitch = 0.7f, .drop = 0.9f};
/* actions: name, seconds, weight while awake (IDLE, STANDBY), weight while asleep, what it does (the Settings window
 * shows it); a weight above 0 also puts it in that state's default map. The geometric ones are played by motion.c. */
const Whim WHIMS[W_N] = {
    [W_LOOK] = {"look", 2.6f, 3, 0, "Glances one way, then the other."},
    [W_PEEK] = {"peek", 2.2f, 2, 0, "Leans to one side to see past something."},
    [W_TWIRL] = {"twirl", 1.5f, 1, 0, "Spins round fast and coasts down."},
    [W_SHIVER] = {"shiver", 0.8f, 1, 0.6f, "A quick shudder."},
    [W_BLINK] = {"blink", 0.45f, 2.5f, 0, "Dims once or twice."},
    [W_ALIGN] = {"align", 2.2f, 2, 0, "Turns a vertex exactly toward you, holds, carries on."},
    [W_STEP] = {"step", 1.3f, 1, 0, "Two snap steps of 72 degrees, like a combination lock."},
    [W_UNFOLD] = {"unfold", 2.4f, 1, 0, "A fresh cut of patches opens down the sphere, then closes in reverse."},
    [W_SCAN] = {"scan", 1.6f, 1, 0, "One plane sweeps top to bottom."},
    [W_SPIKE] = {"spike", 1.2f, 1, 0, "One or two sharp spikes snap out of random nodes and back."},
    [W_CRAZY] = {"crazy", 1.9f, .35f, 0, "The full ERROR fit, rarely, out of nowhere."},
    [W_RIPPLE] = {"ripple", 1.8f, 1, 0, "A wave of lifted patches runs over the sphere from one side to the other."},
    [W_ORBIT] = {"orbit", 2.4f, 1, 0, "One patch lifts off, flies once round the core and docks again."},
    [W_PETAL] = {"petal", 2.2f, 1, 0, "The patches round one node open like a flower, ring by ring, and close."},
    [W_CONSTELLATION] = {"constellation", 2.4f, 1, 0, "A short chain of neighbouring nodes lights up one by one, then fades."},
    [W_TUMBLE] = {"tumble", 1.6f, 1, 0, "A slow half turn about a random axis that settles exactly."},
};

/* The motion map's defaults, worked out from the tables: each state may run the routines made for it, and play
 * the actions WHIMS weights for it (awake: IDLE and STANDBY; asleep: SLEEP). */
void motion_map_default(MotionMap *out) {
    for (int st = 0; st < ST_N; st++) {
        out[st].rt = out[st].act = 0;
        for (int r = 0; r < RT_N; r++) if (ROUTINES[r].home == st) out[st].rt |= 1u << r;
        for (int w = 1; w < W_N; w++) {
            float weight = st == ST_SLEEP ? WHIMS[w].sleep : st == ST_IDLE || st == ST_STANDBY ? WHIMS[w].idle : 0;
            if (weight > 0) out[st].act |= 1u << w;
        }
    }
}
MotionMap MOTION_MAP[ST_N];
void motion_map_init(void) {
    static int done;
    if (done) return;
    done = 1;
    motion_map_default(MOTION_MAP);
}

const WhimGap WHIM_GAP = {.idle_min = 3, .idle_max = 10, .sleep_min = 7, .sleep_max = 18};

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
    motion_map_init();
    snprintf(s->sid, sizeof s->sid, "%s", sid);
    s->created = s->last = s->state_t = t;
    s->burst_at = s->ring_at = s->vol_at = -1e9;
    s->vol = 0.8f;
    s->state = ST_IDLE;
    s->cur = LOOKS[ST_IDLE];
    s->defend_at = s->press_at = -1e9;
    motion_init(s);
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

/* a subagent's events: its own core, which the session's own state never sees (the session is busy on the Agent call
 * meanwhile, or has already stopped while a background one works on) */
#define SUB_STALE 900 /* s without a word from a running subagent before it counts as done */
static void sub_apply(Room *r, const Event *e) {
    char sid[72]; /* its session's id, a dash, its agent id, cut at 64 (as the hook makes it, for the msg file) */
    size_t n = 0;
    for (const char *c = e->sid; *c && n < 64; c++) sid[n++] = *c;
    if (n < 64) sid[n++] = '-';
    for (const char *c = e->aid; *c && n < 64; c++) sid[n++] = *c;
    sid[n] = 0;
    const char *ev = e->ev;
    Session *s = find(r, sid);
    if (!s) {
        if (!strcmp(ev, "SubagentStop") || !strcmp(ev, "Notification") || r->n >= MAX_SESS) return; /* (only what starts it, or its steps) */
        s = &r->s[r->n++];
        session_init(s, sid, e->t, NULL);
        s->sub = 1;
        snprintf(s->parent, sizeof s->parent, "%s", e->sid);
        set_state(s, ST_THINK, e->t);
    }
    s->last = e->t;
    snprintf(s->name, sizeof s->name, "%s", e->atype[0] ? e->atype : "agent");
    if (!strcmp(ev, "PreToolUse")) { snprintf(s->tool, sizeof s->tool, "%s", e->tool); set_state(s, ST_WORK, e->t); s->hold_work_until = e->t + HOLD_WORK; }
    else if (!strcmp(ev, "PostToolUse")) { set_state(s, ST_THINK, e->t); if (e->t + HOLD_WORK > s->hold_work_until) s->hold_work_until = e->t + HOLD_WORK; }
    /* done: SubagentStop, or its SubagentHandback (an agent that hands its report back that way ends there, and Claude
     * Code sends no SubagentStop for it) */
    if (!strcmp(ev, "SubagentStop") || (!strcmp(ev, "PostToolUse") && !strcmp(e->tool, "SubagentHandback"))) { set_state(s, ST_CHEER, e->t); s->burst_at = e->t; s->sub_done = e->t; if (e->msg) s->unread = 1; }
}

void room_apply(Room *r, const Event *e) {
    const char *ev = e->ev;
    if (e->aid[0] && strcmp(ev, "Notification")) { sub_apply(r, e); return; } /* (a permission prompt in a subagent asks you all the same: the session's) */
    Session *s = find(r, e->sid);
    if (!strcmp(ev, "SessionEnd")) {
        if (s) drop(r, s);
        for (int i = 0; i < r->n;) { if (r->s[i].sub && !strcmp(r->s[i].parent, e->sid)) drop(r, &r->s[i]); else i++; } /* and its subagents */
        return;
    }
    if (!strncmp(ev, "Subagent", 8)) return; /* (from an older hook: no agent id) */
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
    if (s->state == ST_CHEER && now - s->state_t > (s->mo.cheer_for > CHEER_FOR ? s->mo.cheer_for : CHEER_FOR)) return ST_IDLE; /* Bloom and Zip need longer */
    if (s->state == ST_IDLE && now - s->last > SLEEP_AFTER) return ST_SLEEP;
    return s->state;
}

static void ease(float *a, float b, float k) { *a += (b - *a) * k; }
static void ease_col(Col *a, Col b, float k) { ease(&a->r, b.r, k); ease(&a->g, b.g, k); ease(&a->b, b.b, k); }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static float bump(float u) { return u <= 0 || u >= 1 ? 0 : sinf(u * 3.14159265f); }      /* 0 → 1 → 0 */
static float hold(float u, float in) {                                                   /* up, stay, down */
    if (u <= 0 || u >= 1) return 0;
    float e = u < in ? u / in : u > 1 - in ? (1 - u) / in : 1;
    return e * e * (3 - 2 * e);
}

/* Whims: left alone (idle, or asleep, and not touched), a core now and then does something small on its
 * own. Most only shape the picture through the w_* fields; a twirl or a landing also nudges the spin and
 * the core's spring, so they flow on into the usual motion. */
static void whim_start(Session *s, int w, double now) {
    s->whim = w;
    s->whim_at = now;
    s->whim_a = frand(&s->rng) * 2 - 1;
    s->whim_b = frand(&s->rng) * 2 - 1;
    if (s->whim == W_TWIRL) s->spin_y += (s->whim_a < 0 ? -1 : 1) * (5.5f + 2.5f * fabsf(s->whim_b));
    if (s->whim == W_SHIVER) s->spin_x += 1.2f * s->whim_a;
}

void whim_play(Session *s, int w, double now) {
    if (w <= W_NONE || w >= W_N) return;
    whim_start(s, w, now);
    s->whim_forced = 1;
    s->whim_k = 1;
}

/* how likely action w is picked while asleep or not */
static float whim_weight(int w, int asleep) {
    float v = asleep && WHIMS[w].sleep > 0 ? WHIMS[w].sleep : WHIMS[w].idle;
    return v > 0 ? v : 1;
}

static void whims(Session *s, int shown, double now, float dt) {
    uint32_t acts = MOTION_MAP[shown].act & ~1u; /* the actions this state may play (MOTION_MAP) */
    int free = (acts || s->whim_forced) && !s->carried && !s->held && s->hover_to <= 0 && s->hover < 0.05f && s->mo.hole < 0.3f; /* (a black hole has its own) */
    if (s->whim_forced) free = 1;
    ease(&s->whim_k, free ? 1.0f : 0.0f, 1.0f - expf(-dt * (free ? 2.0f : 8.0f)));
    int asleep = shown == ST_SLEEP;
    if (!s->whim_next || (!free && s->whim == W_NONE)) /* (re)arm: the first one comes a while after the core settles */
        s->whim_next = now + WHIM_GAP.idle_min + frand(&s->rng) * (WHIM_GAP.idle_max - WHIM_GAP.idle_min);
    if (s->whim == W_NONE && free && acts && now >= s->whim_next) {
        float total = 0, pick;
        for (int w = 1; w < W_N; w++) if (acts & (1u << w)) total += whim_weight(w, asleep);
        pick = frand(&s->rng) * total;
        for (int w = 1; w < W_N; w++) {
            if (!(acts & (1u << w))) continue;
            pick -= whim_weight(w, asleep);
            if (pick < 0) { whim_start(s, w, now); break; }
        }
    }
    s->w_lx = s->w_ly = s->w_lean_x = s->w_lean_y = s->w_off_x = s->w_off_y = s->w_tall = 0;
    s->w_scale = s->w_bright = 1;
    if (s->whim == W_NONE) return;
    float d = WHIMS[s->whim].dur, el = (float)(now - s->whim_at), u = el / d, k = s->whim_k, a = s->whim_a, b = s->whim_b;
    if (u >= 1) { /* done: the next one after a pause */
        s->whim = W_NONE;
        s->whim_forced = 0;
        float lo = asleep ? WHIM_GAP.sleep_min : WHIM_GAP.idle_min, hi = asleep ? WHIM_GAP.sleep_max : WHIM_GAP.idle_max;
        s->whim_next = now + lo + frand(&s->rng) * (hi - lo);
        return;
    }
    switch (s->whim) {
    case W_LOOK: { /* two glances: (a, b·0.6) then the other way */
        float g1 = hold(u * 2, 0.3f), g2 = hold(u * 2 - 1, 0.3f);
        s->w_lx = 1.3f * (a * g1 - (a > 0 ? 1 : -1) * (0.5f + 0.5f * fabsf(b)) * g2);
        s->w_ly = 0.7f * (b * g1 + a * 0.5f * g2);
        break;
    }
    case W_PEEK: {
        float e = hold(u, 0.25f), side = a < 0 ? -1.0f : 1.0f;
        s->w_lean_x = 0.38f * side * e;
        s->w_lx = 1.1f * side * e;
        s->w_off_x = 0.05f * side * e;
        s->w_tall = 0.03f * e;
        break;
    }
    case W_TWIRL:
        s->w_ly = -0.4f * bump(u);
        s->w_bright = 1 + 0.2f * bump(u);
        break;
    case W_SHIVER: {
        float e = bump(u);
        s->w_off_x = 0.025f * e * sinf(el * 70);
        s->w_off_y = 0.012f * e * sinf(el * 53 + 1);
        s->w_bright = 1 - 0.15f * e;
        break;
    }
    case W_BLINK: { /* once, or twice quickly */
        float uu = b > 0.3f ? fmodf(u * 2, 1) : u;
        s->w_bright = 1 - 0.8f * bump(uu);
        s->w_scale = 1 - 0.03f * bump(uu);
        break;
    }
    }
    /* touched in the middle of a whim: it fades out instead of stopping dead */
    s->w_lx *= k; s->w_ly *= k; s->w_lean_x *= k; s->w_lean_y *= k; s->w_off_x *= k; s->w_off_y *= k; s->w_tall *= k;
    s->w_scale = 1 + (s->w_scale - 1) * k;
    s->w_bright = 1 + (s->w_bright - 1) * k;
}

/* Carried by the mouse (the panel sets carried and move_*): the shell leans into the motion, the core hangs
 * on a critically damped spring, so it lags a little when the mouse speeds up, stops or turns and settles
 * without bouncing; shaking builds up into a dizzy spin (and, in motion.c, a crazy fit). */
static void carried(Session *s, double now, float dt) {
    float k = 1.0f - expf(-dt * CARRY.ease);
    ease(&s->carry, (float)s->carried, k);
    float ix = (s->move_x - s->last_mx) / fmaxf(dt, 0.005f), iy = (s->move_y - s->last_my) / fmaxf(dt, 0.005f);
    s->last_mx = s->move_x;
    s->last_my = s->move_y;
    ix = clampf(ix, -3000, 3000);
    iy = clampf(iy, -3000, 3000);
    ease(&s->lean_x, clampf(s->move_x * CARRY.lean, -CARRY.lean_max, CARRY.lean_max), k);
    ease(&s->lean_y, clampf(s->move_y * CARRY.lean, -CARRY.lean_max, CARRY.lean_max), k);
    int steps = (int)ceilf(dt / 0.008f); /* small steps keep the stiff spring stable */
    float h = dt / (float)(steps > 0 ? steps : 1);
    for (int i = 0; i < steps; i++) {
        s->jig_vx += (-CARRY.spring * s->jig_x - CARRY.damp * s->jig_vx - CARRY.lag * ix) * h;
        s->jig_vy += (-CARRY.spring * s->jig_y - CARRY.damp * s->jig_vy - CARRY.lag * iy) * h;
        s->jig_x = clampf(s->jig_x + s->jig_vx * h, -CARRY.jig_max, CARRY.jig_max);
        s->jig_y = clampf(s->jig_y + s->jig_vy * h, -CARRY.jig_max, CARRY.jig_max);
    }
    s->shake = fminf(1, s->shake * expf(-dt * CARRY.shake_fade) + hypotf(ix, iy) * CARRY.shake_gain * dt);
    if (s->shake > 0.92f && now - s->burst_at > 2.0) s->burst_at = now; /* shaken hard: one flare */
    if (!s->held) s->ay += s->shake * s->shake * CARRY.shake_spin * dt;
}

void room_tick(Room *r, const Shape *sh, double now, float dt) {
    for (int i = 0; i < r->n;) {
        Session *s = &r->s[i];
        int gone = now - s->last > DROP_AFTER;
        if (s->sub && s->sub_done <= 0 && now - s->last > SUB_STALE) s->sub_done = now; /* never heard to finish (a lost stop): done after a long silence */
        if (s->sub && s->sub_done > 0) /* a finished subagent: once its reply has been seen and the pointer has left it */
            gone = gone || (SUB_UNTIL_SEEN ? s->sub_seen && s->hover_to <= 0 && s->hover < .05f : now - s->sub_done > SUB_KEEP);
        if (gone) drop(r, s);
        else i++;
    }
    float k = 1.0f - expf(-dt * 4.0f);
    for (int i = 0; i < r->n; i++) {
        Session *s = &r->s[i];
        const Look *L = &LOOKS[s->mo.closing ? s->mo.routine : session_shown(s, now)]; /* (the old state's look while its routine winds down) */
        Look *c = &s->cur;
        ease(&c->rot, L->rot, k); ease(&c->core, L->core, k); ease(&c->hz, L->hz, k); ease(&c->depth, L->depth, k);
        ease(&c->edge, L->edge, k); ease(&c->flow, L->flow, k); ease(&c->hr, L->hr, k); ease(&c->glitch, L->glitch, k);
        ease_col(&c->col, L->col, k); ease_col(&c->halo, L->halo, k);
        ease_col(&c->line, L->line, k); ease_col(&c->eh, L->eh, k);
        ease(&c->lc, L->lc, k); ease(&c->lh, L->lh, k);
        float kh = 1.0f - expf(-dt * HOVER.ease);
        ease(&s->hover, s->hover_to, kh);
        whims(s, session_shown(s, now), now, dt);
        carried(s, now, dt);
        motion_tick(s, sh, session_shown(s, now), now, dt);
        /* the gaze, after the routine: READY's gazes at the pointer as it winds up (w_lx, w_ly) */
        ease(&s->look_x, s->look_x_to * s->hover_to + s->w_lx, kh);
        ease(&s->look_y, s->look_y_to * s->hover_to + s->w_ly, kh);
        float spin = c->rot * s->mo.rate * (1 - HOVER.slow * s->hover), fade = expf(-dt * TOUCH.coast);
        if (s->sdir_tx != 0 || s->sdir_ty != 0) { /* scrolled: its own spin turns, eased, to the way it was sent */
            if (s->sdir_x == 0 && s->sdir_y == 0) { s->sdir_x = 0.37f; s->sdir_y = 1; }
            float kd = 1.0f - expf(-dt * 3.0f);
            s->sdir_x += (s->sdir_tx - s->sdir_x) * kd; s->sdir_y += (s->sdir_ty - s->sdir_y) * kd;
        }
        float dx = s->sdir_x != 0 || s->sdir_y != 0 ? s->sdir_x : 0.37f, dy = s->sdir_x != 0 || s->sdir_y != 0 ? s->sdir_y : 1;
        if (!s->held) { /* while dragged the panel turns it; after that a fling fades back into its own spin */
            s->ay += (spin * dy + s->spin_y) * dt;
            s->ax += (spin * dx + s->spin_x) * dt;
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
                               "Speaking", "Listening", "Transcribing", "Standby", "Error", "Ready"};

/* The mouse is doing something to it (pointed at, scrolled, held, carried, shaken): it answers at once. */
static int mouse_busy(const Session *s, double now) {
    return s->hover_to > 0 || s->hover > 0.01f || s->held || fabsf(s->spin_x) + fabsf(s->spin_y) > 0.02f
           || s->carried || s->carry > 0.01f || s->shake > 0.01f
           || fabsf(s->look_x) + fabsf(s->look_y) > 0.01f
           || fabsf(s->jig_x) + fabsf(s->jig_y) + 0.05f * (fabsf(s->jig_vx) + fabsf(s->jig_vy)) > 0.003f
           || now - s->ring_at < TOUCH.ring_for || now - s->vol_at < TOUCH.vol_for;
}

/* How often a session's picture is redrawn. A calm session moves slowly, so it needs far fewer frames.
 * A state change gets the full rate for a moment so the colours ease smoothly. */
double session_rate(const Session *s, double now, int fps) {
    static const double RATE[ST_N] = {[ST_IDLE] = 10, [ST_THINK] = 15, [ST_WORK] = 20, [ST_WAIT] = 20, [ST_CHEER] = 20, [ST_SLEEP] = 4,
                                      [ST_SPEAK] = 20, [ST_LISTEN] = 20, [ST_TRANSCRIBE] = 20, [ST_STANDBY] = 4, [ST_ERROR] = 20, [ST_READY] = 20};
    int busy = mouse_busy(s, now) || s->whim || motion_busy(s, now);
    double r = now - s->state_t < 1.5 || busy ? 20 : RATE[session_shown(s, now)];
    if (s->mo.hole > 0.01f && r < 10) r = 10; /* the black hole's disc keeps turning */
    if (s->unread && r < 12) r = 12; /* the flash of an unread reply */
    return r < fps ? r : fps;
}

/* The panels' Smoothness (Settings > Performance): pictures a second for each kind of moment, 0 meaning the
 * refresh rate of the display the core is on. Balanced is what the panels did before there was a choice
 * (session_rate × 1.5, and 60 in the hand). */
const Smooth SMOOTH[SM_CUSTOM] = {
    [SM_SAVER] = {.idle = 10, .think = 15, .busy = 20, .rest = 4, .hand = 30},
    [SM_BALANCED] = {.idle = 15, .think = 22.5f, .busy = 30, .rest = 6, .hand = 60},
    [SM_SMOOTH] = {.idle = 30, .think = 45, .busy = 60, .rest = 10, .hand = 60},
    [SM_MAX] = {.idle = 0, .think = 0, .busy = 0, .rest = 15, .hand = 0},
};
const char *SMOOTH_NAME[SM_N] = {"saver", "balanced", "smooth", "max", "custom"};

double panel_rate(const Session *s, double now, const Smooth *sm, double hz) {
    if (hz < 1) hz = 60;
    float r;
    if (s->hover_to > 0 || s->held || s->carried) r = sm->hand; /* in the hand */
    else if (mouse_busy(s, now) || s->whim || motion_busy(s, now) || now - s->state_t < 1.5) r = sm->busy;
    else switch (session_shown(s, now)) {
        case ST_IDLE: r = sm->idle; break;
        case ST_THINK: r = sm->think; break;
        case ST_SLEEP: case ST_STANDBY: r = sm->rest; break;
        default: r = sm->busy;
    }
    double out = r <= 0 ? hz : r;
    if (s->mo.hole > 0.5f && s->mo.rt == RT_HOLE && s->mo.rs.hole.moving && out < 60) out = 60; /* the black hole playing, or in the hand: smooth */
    if (s->mo.hole > 0.01f && out < 15) out = 15; /* the black hole's disc keeps turning (traced per pixel: not every refresh while it only turns) */
    if (s->unread && out < 18) out = 18;         /* the flash of an unread reply */
    return out > hz ? hz : out < 2 ? 2 : out;
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

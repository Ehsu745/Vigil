/* wasm.c: the shared C (geom, session, motion, render) as a WebAssembly module, for the Core Bestiary page
 * (web/bestiary.html). Built by `make wasm`; not part of the terminal or the panels.
 *
 * Each core on the page is a slot of one Room. The page sets what the panel would set (state, pointer,
 * carry, presses), calls v_tick once a frame and v_render for each core it shows, and copies the pixels
 * out of the module's memory. Times are the page's clock in seconds. */
#include "vigil.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXPORT(name) __attribute__((export_name(#name)))

static Shape g_sh;
static Room g_room;
static int g_shown[MAX_SESS];   /* the state each slot is held in (the page's bench, not the events) */
static Canvas g_cv;
static uint8_t *g_px;           /* RGBA, 8 bits, straight alpha */
static size_t g_cap;

EXPORT(v_init) int v_init(const char *shape) {
    motion_map_init();
    return shape_make(&g_sh, shape && shape[0] ? shape : "geo2");
}
/* a buffer the page may write a short string into (a shape name) */
EXPORT(v_scratch) char *v_scratch(void) { static char buf[64]; return buf; }

EXPORT(v_new) int v_new(double now) {
    if (g_room.n >= MAX_SESS) return -1;
    int i = g_room.n++;
    char sid[16];
    snprintf(sid, sizeof sid, "core-%d", i);
    session_init(&g_room.s[i], sid, now, &g_sh);
    snprintf(g_room.s[i].name, sizeof g_room.s[i].name, "%s", sid);
    g_shown[i] = ST_IDLE;
    return i;
}

static Session *slot(int i) { return i >= 0 && i < g_room.n ? &g_room.s[i] : NULL; }

/* hold slot i in state st (ST_*), as the panel's Settings preview does: an event state stays put, a panel
 * state is an overlay */
EXPORT(v_state) void v_state(int i, int st) { if (slot(i) && st >= 0 && st < ST_N) g_shown[i] = st; }
EXPORT(v_shown) int v_shown(int i, double now) { Session *s = slot(i); return s ? session_shown(s, now) : -1; }
EXPORT(v_play) void v_play(int i, int rt) { Session *s = slot(i); if (s && rt >= 0 && rt < RT_N) motion_play(s, rt); }
EXPORT(v_whim) void v_whim(int i, int w, double now) { Session *s = slot(i); if (s && w > W_NONE && w < W_N) whim_play(s, w, now); }
EXPORT(v_rt) int v_rt(int i) { Session *s = slot(i); return s ? s->mo.rt : -1; }
EXPORT(v_whim_now) int v_whim_now(int i) { Session *s = slot(i); return s ? s->whim : 0; }

/* the pointer: hover 0/1, where it is (offset from the centre in drawn shell radii, y down), lock-on 0/1 */
EXPORT(v_pointer) void v_pointer(int i, int hover, float lx, float ly, int lock) {
    Session *s = slot(i);
    if (!s) return;
    s->hover_to = hover ? 1.0f : 0.0f;
    if (hover) { s->look_x_to = lx; s->look_y_to = ly; }
    s->lock_to = lock ? 1.0f : 0.0f;
}
/* carried on the mouse, its speed in core widths a second (y down) */
EXPORT(v_carry) void v_carry(int i, int on, float mx, float my) {
    Session *s = slot(i);
    if (!s) return;
    s->carried = on;
    s->move_x = on ? mx : 0;
    s->move_y = on ? my : 0;
}
/* held under the hand (a drag spins it); sx, sy: the spin left when let go */
EXPORT(v_held) void v_held(int i, int on, float sx, float sy) {
    Session *s = slot(i);
    if (!s) return;
    s->held = on;
    s->spin_x = sx;
    s->spin_y = sy;
}
EXPORT(v_press) void v_press(int i, float lx, float ly, double now) { Session *s = slot(i); if (s) { s->press_at = now; s->press_x = lx; s->press_y = ly; } }
EXPORT(v_ring) void v_ring(int i, double now) { Session *s = slot(i); if (s) s->ring_at = now; }
EXPORT(v_defend) void v_defend(int i, float dx, float dy, double now) { Session *s = slot(i); if (s) { s->defend_at = now; s->defend_x = dx; s->defend_y = dy; } }

EXPORT(v_tick) void v_tick(double now, float dt) {
    for (int i = 0; i < g_room.n; i++) {
        Session *s = &g_room.s[i];
        int st = g_shown[i];
        s->state = st == ST_SLEEP || st >= ST_SPEAK ? ST_IDLE : st;
        s->overlay = st == ST_IDLE ? 0 : st;
        s->last = now; /* never falls asleep or drops out on its own */
    }
    room_tick(&g_room, &g_sh, now, dt);
}

/* draws slot i into a px x px square (the shell's sphere is SHELL_FRAC of it); returns RGBA pixels, straight
 * alpha (the light alone, see-through where dark), as the page's ImageData wants them */
EXPORT(v_render) uint8_t *v_render(int i, int px, float glow, float core, double now) {
    Session *s = slot(i);
    if (!s || px < 8 || px > 4096) return NULL;
    size_t n = (size_t)px * px;
    if (n * 4 > g_cap) { free(g_px); g_px = malloc(n * 4); g_cap = g_px ? n * 4 : 0; if (!g_px) return NULL; }
    canvas_resize(&g_cv, px, px);
    render_one(&g_cv, s, &g_sh, now, glow, core);
    canvas_rgba(&g_cv, g_px);
    for (size_t j = 0; j < n; j++) {
        uint8_t *q = g_px + j * 4;
        if (q[3] && q[3] < 255) for (int k = 0; k < 3; k++) { int v = q[k] * 255 / q[3]; q[k] = (uint8_t)(v > 255 ? 255 : v); }
    }
    return g_px;
}
/* turned by the hand (a drag), radians */
EXPORT(v_turn) void v_turn(int i, float dax, float day) { Session *s = slot(i); if (s) { s->ax += dax; s->ay += day; } }

/* the tables, so the page names things as the C does */
EXPORT(v_n) int v_n(int what) { return what == 0 ? RT_N : what == 1 ? W_N : ST_N; }
EXPORT(v_name) const char *v_name(int what, int k) {
    if (what == 0) return k >= 0 && k < RT_N ? ROUTINES[k].name : "";
    if (what == 1) return k >= 0 && k < W_N && WHIMS[k].name ? WHIMS[k].name : "";
    return k >= 0 && k < ST_N ? STATE_NAME[k] : "";
}
EXPORT(v_about) const char *v_about(int what, int k) {
    if (what == 0) return k >= 0 && k < RT_N && ROUTINES[k].about ? ROUTINES[k].about : "";
    if (what == 1) return k >= 0 && k < W_N && WHIMS[k].about ? WHIMS[k].about : "";
    return "";
}
EXPORT(v_home) int v_home(int k) { return k >= 0 && k < RT_N ? ROUTINES[k].home : -1; }
EXPORT(v_shell_frac) float v_shell_frac(void) { return SHELL_FRAC; }

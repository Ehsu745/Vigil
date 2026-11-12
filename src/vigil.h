#ifndef DODECA_H
#define DODECA_H
#include <stddef.h>
#include <stdint.h>

#define MAX_SESS 16
#define MAX_V 96
#define MAX_E 280
#define MAX_DEG 6
#define NPULSE 14
#define CIRCUM 1.7320508f   /* every shell is scaled so its vertices sit on a sphere of this radius */
#define INRADIUS 1.3763819f /* inradius of the dodecahedron on that sphere: the unit the core radius is given in */
#define SHELL_FRAC 0.36f    /* the shell's sphere radius as a fraction of the side of the square it is drawn in */
#define SHELL_HIT 0.375f    /* the drawn outline, perspective included: what the panel treats as "on the core" */

typedef struct { float r, g, b; } Col;

/* ---- events.c: follow the JSONL file the hook appends to ------------------------------------ */
typedef struct { double t; char sid[72], ev[24], cwd[40], tool[40], ntype[32]; int msg; } Event; /* msg: a new reply is in the msg file (DODECA_RICH) */
typedef struct { char path[1024]; long pos; char *buf; size_t buflen; } Tail;
void tail_init(Tail *t, const char *path);
int tail_history(Tail *t, double since, Event *out, int max); /* events newer than `since` already in the file */
int tail_poll(Tail *t, Event *out, int max);                  /* events appended since the last call */
int event_parse(const char *line, size_t n, Event *e);

/* ---- geom.c: the wireframe shells ------------------------------------------------------------ */
typedef struct {
    int nv, ne;
    float v[MAX_V][3];
    int e[MAX_E][2];
    int adj[MAX_V][MAX_DEG], nadj[MAX_V]; /* edges meeting at each vertex */
    float df;                             /* halo damping for dense meshes: sqrt(30 / edges) */
} Shape;
int shape_make(Shape *s, const char *name); /* "dodeca" "bucky" "geo2" "geo3"; 0 if the name is unknown */

/* ---- session.c: one polyhedron per Claude Code session ---------------------------------------- */
enum {
    ST_IDLE, ST_THINK, ST_WORK, ST_WAIT, ST_CHEER, ST_SLEEP,  /* from the events */
    ST_SPEAK, ST_LISTEN, ST_TRANSCRIBE, ST_STANDBY, ST_ERROR, /* overlays only the panel sets (Session.overlay) */
    ST_N
};
extern const char *STATE_NAME[ST_N];

typedef struct {
    float rot, core, hz, depth, edge, flow, hr, glitch;
    Col col, halo, line, eh; /* core body, glow around the core, thin edge line, edge halo */
    float lc, lh;            /* how much Session.level (speech or microphone) drives core brightness / glow size */
} Look;
extern const Look LOOKS[ST_N];

/* how a core reacts while the mouse points at it (only the panel sets hover; the terminal never does) */
typedef struct {
    float bright; /* core and shell brighten by this fraction */
    float slow;   /* rotation slows by this fraction */
    float tilt;   /* the shell turns up to this many radians toward the cursor */
    float gaze;   /* the core moves up to this fraction of the shell radius toward the cursor */
    float ease;   /* how fast it reacts (1/s) */
} Hover;
extern const Hover HOVER;

/* the other mouse reactions (panel only) */
typedef struct {
    float unread_hz, unread_amp; /* the soft, regular flash of a core whose reply is not read yet */
    float ring_for;              /* seconds the double-click shockwave ring takes */
    float vol_for;               /* seconds the shell shows the speech volume after a scroll */
    float coast;                 /* how fast a flung shell slows back to its own rotation (1/s) */
    float aura;                  /* single-core mode: how far the core is tinted with its session's colour (0..1) */
} Touch;
extern const Touch TOUCH;

/* carried by the mouse (panel only: ⌥-click or ⌥-drag). Speeds are in core widths per second. */
typedef struct {
    float lift;               /* picked up: core and shell brighten by this fraction */
    float ease;               /* how fast lift and lean follow (1/s) */
    float lean, lean_max;     /* the shell turns into the motion: radians per unit of speed, and at most */
    float stretch;            /* at full lean the shell stretches along the motion by this fraction */
    float spring, damp, lag;  /* the core hangs on a spring inside the shell; mouse acceleration pushes it by lag */
    float jig_max;            /* the core stays within this fraction of the shell radius */
    float shake_gain, shake_fade; /* shaking builds from acceleration and calms down at this rate (1/s) */
    float shake_spin;         /* extra spin at full shake (rad/s) */
    float shake_glitch;       /* how much full shake makes the core stutter (0..1) */
    float drop;               /* put down: the core drops by this speed and springs back */
} Carry;
extern const Carry CARRY;

/* Left alone, a core is not still: now and then it does something small on its own, like a living thing.
 * Each behaviour ("whim") is a row of WHIMS in session.c. */
enum { W_NONE, W_LOOK, W_PEEK, W_HOP, W_TWIRL, W_SHIVER, W_BLINK, W_YAWN, W_SIGH, W_N };
typedef struct {
    const char *name;
    float dur;           /* seconds */
    float idle, sleep;   /* how likely it is picked while idle / asleep (0 = never) */
} Whim;
extern const Whim WHIMS[W_N];
typedef struct { float idle_min, idle_max, sleep_min, sleep_max; } WhimGap; /* seconds between whims */
extern const WhimGap WHIM_GAP;
#define NHUE 8
extern const Col HUES[NHUE];     /* single-core mode: one colour per session, tinting its core */

typedef struct { int e, dir; float t, sp; } Pulse;

typedef struct {
    char sid[72], name[40], tool[40];
    int state;
    double created, last, state_t, hold_work_until, burst_at;
    Look cur;      /* what is drawn now: eases toward LOOKS[shown state] */
    float ax, ay;  /* rotation */
    double phase;  /* each session's own time offset, so they do not pulse in step */
    Pulse p[NPULSE];
    uint32_t rng;
    /* the mouse (panel only): *_to is set by the panel, the rest eases toward it in room_tick */
    float hover_to, look_x_to, look_y_to; /* 1 while pointed at; cursor offset from the centre, -1..1, y down */
    float hover, look_x, look_y;
    int unread; /* a reply arrived since the last prompt (only with DODECA_RICH; the panel reads the text) */
    /* set by the panel only */
    int overlay;           /* a panel state (ST_SPEAK, ...) shown instead of the event state; 0 = none */
    float level;           /* 0..1 speech or microphone level, see Look.lc / Look.lh */
    double ring_at, vol_at;/* when the double-click ring started / the volume last changed */
    float vol;             /* speech volume 0..1 */
    int muted, held;       /* never speaks on its own / being dragged by the mouse */
    float spin_x, spin_y;  /* extra rotation speed left from a drag, fades out */
    Col aura;              /* single-core mode: this session's colour, which tints its core */
    float aura_a;          /* how much, 0 = none */
    int carried;           /* riding on the mouse (see CARRY) */
    float move_x, move_y;  /* the mouse speed while carried, core widths per second, y down; 0 otherwise */
    /* the rest room_tick works out */
    float carry, lean_x, lean_y, shake, last_mx, last_my;
    float jig_x, jig_y, jig_vx, jig_vy; /* the core's offset on its spring, in shell radii, y down */
    /* whims (room_tick): what plays now, and what it does to the picture this frame */
    int whim;
    double whim_at, whim_next;
    float whim_a, whim_b, whim_k;  /* its random direction and size; how free it is to play (0 while touched) */
    float w_lx, w_ly;              /* gaze, added to the cursor look */
    float w_lean_x, w_lean_y;      /* shell turned, radians */
    float w_off_x, w_off_y;        /* the whole core moved, shell radii, y down */
    float w_tall;                  /* stretched up (+) or squashed (-) */
    float w_scale, w_bright;       /* size and brightness factors (1 = as usual) */
} Session;

typedef struct { Session s[MAX_SESS]; int n; } Room;

void room_apply(Room *r, const Event *e);
void room_move(Room *r, int from, int to); /* reorder: the session at from goes to position to (panel only) */
void room_tick(Room *r, const Shape *sh, double now, float dt);
int session_shown(const Session *s, double now);
void session_init(Session *s, const char *sid, double t, const Shape *sh);
extern const char *STATE_TEXT[ST_N];                          /* "Idle", "Thinking", ... for text labels */
double session_rate(const Session *s, double now, int fps); /* how many times a second its picture needs redrawing */

/* the built-in demo: made-up sessions that walk through every state */
typedef struct { int n, step[6]; double next[6]; } Demo;
void demo_start(Demo *d, int n, double now);
void demo_run(Demo *d, Room *room, double now);

/* ---- render.c: software renderer into a float RGB canvas -------------------------------------- */
typedef struct { int w, h; float *p; } Canvas;
void canvas_resize(Canvas *c, int w, int h);
void render_room(Canvas *c, const Room *r, const Shape *sh, double now, float glow, float core_frac);
void render_one(Canvas *c, const Session *s, const Shape *sh, double now, float glow, float core_frac);
void canvas_rgba(const Canvas *c, uint8_t *rgba);           /* premultiplied RGBA: the light alone, see-through where dark */
void canvas_bytes(const Canvas *c, uint8_t *rgb, Col bg); /* 8-bit RGB over the background colour, w*h*3 bytes */

#endif

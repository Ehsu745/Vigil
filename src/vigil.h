#ifndef DODECA_H
#define DODECA_H
#include <stddef.h>
#include <stdint.h>

#define MAX_SESS 16
#define MAX_V 96
#define MAX_E 280
#define MAX_DEG 6
#define MAX_T 180           /* triangles of the finest geodesic shell (geo3) */
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
    int vn[MAX_V][MAX_DEG];               /* the vertex at the other end of each of those edges */
    float df;                             /* halo damping for dense meshes: sqrt(30 / edges) */
    /* the triangles (geodesic shells only; 0 for dodeca and bucky, which then keep a rigid net) */
    int nt;
    int t[MAX_T][3];
    float tn[MAX_T][3];                   /* unit normal (the direction from the centre through the triangle) */
    int tnb[MAX_T][3];                    /* the triangle across each side, -1 if none */
    int et[MAX_E][2];                     /* the two triangles on each edge, -1 if none */
    int vt[MAX_V][MAX_DEG], nvt[MAX_V];   /* the triangles around each vertex */
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
enum { W_NONE, W_LOOK, W_PEEK, W_HOP, W_TWIRL, W_SHIVER, W_BLINK, W_YAWN, W_SIGH,
       W_ALIGN, W_STEP, W_UNFOLD, W_SCAN, W_SPIKE, W_CRAZY, /* geometric ones, played by motion.c */
       W_N };
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

/* ---- motion.c: how the shell moves (the "Core Bestiary" design) --------------------------------
 * At rest the shell is an exact geodesic sphere. It moves only in geometric ways: snap turns about
 * symmetry axes, irregular patches of triangles sliding out along their normal and locking back, layers
 * of triangles turning against the rest, spikes (one node pulled far out, or in toward the centre), a
 * scan plane and nodes lighting in sequence. Each state runs its own routine; see motion.c. */
#define MAX_TURN 6
#define MAX_SPIKE 32
#define MAX_EV 24
typedef struct { float axis[3], angle, done, dur; double at; int kind; } Turn; /* kind: 0 in-out, 1 speeds up and stops dead, 2 jerks out and back */
typedef struct { int v; float len, up, hd, dn; double at; } Spike;         /* len < 0: pulled in toward the centre */
typedef struct { int from, to, ntrail, trail[10]; double at, trail_at[10]; } Fish;
typedef struct { double at; int kind; } MEvent;                             /* something a routine asked to happen later */
typedef struct {
    float q[9];                          /* the snap turns, on top of the drift (applied before ax / ay) */
    Turn turn[MAX_TURN];
    float rate;                          /* how much of the state's own spin is used now, eased (0..1) */
    /* the cut: which triangles move together as one patch */
    int cut_for, cutid, ng;              /* cut_for: the triangle count it was made for (0 = none yet) */
    unsigned char grp[MAX_T];            /* each triangle's patch */
    float gw[MAX_T], gn[MAX_T][3];       /* each patch's reach (0 = stays put) and normal */
    float off[MAX_T];                    /* how far each triangle has slid out, shell radii */
    float ang[MAX_T], lax[3];            /* each triangle's layer turn, about the axis lax */
    int ang_on;                          /* a routine set layer turns this frame */
    float push[MAX_V];                   /* each node pushed out (+) or pulled in (-), shell radii */
    Spike sp[MAX_SPIKE];
    float glow[MAX_V];                   /* nodes lit by scans, waves and clicks */
    float scale;                         /* the whole shell (1 = as usual) */
    float inner, inner_ang, inner_rate;  /* the inner shell: how visible, its turn, its speed */
    float scan_h, scan_a;                /* a scan plane at view height scan_h (-1 top .. 1 bottom), 0 = none */
    double crazy_until, tense_until, last_crazy;
    double defend_seen, press_seen, press_at, wstart;
    int press_g, locked, flinched;
    float lock;
    unsigned char pdist[MAX_T];          /* patch steps from the pressed patch */
    MEvent ev[MAX_EV];
    /* the routine of the state shown now */
    int routine, r_flag, norder;
    double r_at, r_next;
    float r_end, seq_s[8], seq_d[8];
    signed char layer[MAX_T];            /* THINK: which of the two lifted layers a patch is in */
    unsigned char belt[MAX_T];           /* WORK: the triangles on the turbine belt */
    unsigned char order[MAX_T];          /* WORK: piston firing order; whims: unfold order */
    float rank[MAX_T];                   /* DONE: when each patch locks back (0..1) */
    unsigned char clicked[MAX_T];
    Fish fish[2];                        /* NEEDS YOU */
    double stir_next;                    /* IDLE hover */
    float stir[3];
} Motion;

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
    /* set by the panel only: lock-on (pointed at for 2 s), a rush at the core, a press on it */
    float lock_to;
    double defend_at, press_at;
    float defend_x, defend_y;      /* where the rush came from, view space, y down */
    float press_x, press_y;        /* where the press landed, like look_x / look_y */
    Motion mo;
} Session;

typedef struct { Session s[MAX_SESS]; int n; } Room;

void room_apply(Room *r, const Event *e);
void room_move(Room *r, int from, int to); /* reorder: the session at from goes to position to (panel only) */
void room_tick(Room *r, const Shape *sh, double now, float dt);
int session_shown(const Session *s, double now);
void session_init(Session *s, const char *sid, double t, const Shape *sh);
extern const char *STATE_TEXT[ST_N];                          /* "Idle", "Thinking", ... for text labels */
double session_rate(const Session *s, double now, int fps); /* how many times a second its picture needs redrawing */

void motion_init(Session *s);
void motion_tick(Session *s, const Shape *sh, int shown, double now, float dt);
int motion_busy(const Session *s, double now);            /* moving on its own: wants the full frame rate */
int motion_crazy(const Session *s, double now);           /* in the middle of a crazy fit (glitching) */
/* small 3x3 matrix helpers, row-major (shared with render.c) */
void m3_mul(float *r, const float *a, const float *b);    /* r = a * b (r may be a or b) */
void m3_rot(float *r, const float *axis, float angle);    /* about a unit axis */
void m3_app(float *out, const float *m, const float *v);
void m3_euler(float *r, float ax, float ay);              /* the drift: about y by ay, then about x by ax */

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

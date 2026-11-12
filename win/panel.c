/* vigil-panel.exe: Vigil's floating panel for Windows.
 *
 * Claude Code and the hook run inside WSL; this native Windows program reads the same events file through
 * the \\wsl$ share (the WSL launcher win/vigil-win passes its Windows path with --events) and draws the
 * cores in see-through, always-on-top windows, like the macOS panel. It uses the shared code unchanged
 * (events.c, geom.c, session.c, motion.c, render.c): every frame each core is rendered into its own canvas
 * and laid into a premultiplied BGRA bitmap shown with UpdateLayeredWindow.
 *
 * Single-core mode is the default here: one big core for the session in focus, the others small satellites
 * around it ("Show other sessions" off, or --no-others: only the big one), in one small window that Alt-drag
 * puts anywhere on the screen. All cores (--all, or Ctrl+Alt+C): every core is its own small window; they
 * start in a column on the right edge, and Alt-drag puts each one anywhere (kept per folder name).
 * Alt stands in for the macOS ⌥ everywhere: Alt-drag moves, Alt-click picks up and the next click puts down.
 *
 * Also: hover replies (the tooltip box with the last reply or permission notice, from the hook's msg files,
 * DODECA_RICH=1), labels, lock-on with the focus frame, the rush defence, press, drag to spin, double-click
 * ring and bring to front, away (STANDBY), right-click menu with Copy reply and Settings. Not yet: voice.
 *
 * All core rectangles are kept in screen pixels; each window knows where it is.
 *
 * Build (in WSL with mingw-w64, or cross-compiled): sh win/build.sh */
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <ole2.h>
#include <richedit.h>
#include <commdlg.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "../src/vigil.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x2
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static Room g_room, g_ghost;
static Shape g_shape;
static Tail g_tail;
static Event g_ev[1024];
static Demo g_demo;
static int g_demo_n, g_width;
static float g_glow = 1.0f, g_core = 0.35f, g_font = 14, g_scale = 1; /* g_scale: Settings > Panel size */
static float g_tip_size = 1;        /* Settings > Box size: how wide and tall the text box may grow */
static float g_gravity = 2;         /* how hard the main core holds the small ones on their orbit (Settings > Gravity) */
static char g_shape_name[16] = "geo2";
static double g_dpi = 1;
static char g_dir[1024];            /* the events file's folder (a Windows path): msg\ and the saved positions */
static char g_frame[16] = "segments"; /* --frame: drawn round a core while it is carried or locked on */
static int g_labels = 1;            /* all cores: name and state above each core (--labels hover: none) */
static double g_away_after = 120;   /* --away: seconds without keyboard or mouse input until "away" (0 = never) */
static int g_away;
static int g_tip_middle;            /* Settings > Text box: in the middle of the screen, else next to the core */
static int g_tip_bg;                /* Settings > Box colour: the text box's background, one of TIP_BGS, or 5: g_tip_custom */
static unsigned g_tip_custom = 0x2A2F3A; /* the colour picked in the palette window (0xRRGGBB) */
static int g_text_col;              /* Settings > Colours > Text: 0 auto (light on a dark box, dark on a light one), 1-4 TEXT_COLS, 5 g_text_custom */
static unsigned g_text_custom = 0xF2F2F5;
static int g_wheel_spin;            /* Settings > Scroll on a core: how the wheel spins it, one of WHEEL_NAMES (4: not at all) */
static int g_tip_mono = 1;          /* Settings > Text box font: the terminal's monospaced face, like Claude Code (else Segoe UI) */
static int g_open_settings;         /* --settings: open the Settings window at once */
static char g_set_shot[1024];       /* --settings-shot FILE: open it, let the preview run 2 s, save the window as a BMP and quit */
static int g_stats;                 /* --stats: frames, pictures and window updates a second, to panel-stats.txt */
static long g_stat_tick, g_stat_pic, g_stat_push;
static double g_stat_ms, g_stat_since;

/* single-core mode */
static int g_single = 1;            /* --all: every core its own window */
static int g_others = 1;            /* the other sessions float around small (--no-others: only the one in focus) */
static char g_mode_hotkey[32] = "ctrl+alt+c";
static char g_focus[72];            /* the session in the middle */
static int g_focus_pinned;          /* chosen by a click or the menu: auto-follow is off until "Follow automatically" */
static double g_focus_since;

/* a see-through window with its own bitmap: the cluster (single core), or one core (all cores) */
typedef struct {
    HWND w;
    int W, H, x, y, shown, transparent, dirty; /* dirty: draw it again even if no picture changed */
    HDC mem;
    HBITMAP bmp;
    uint32_t *px;                   /* W×H premultiplied BGRA, top-down */
} Pane;
static Pane g_cluster;              /* also owns the timer, the hotkey and the menus */
static Pane g_pane[MAX_SESS];       /* all cores: pane i shows session i */
static HWND g_tip;
static HWND g_set;                  /* the Settings window (kept in front of the cores while open) */
static int g_menu_open;             /* the right-click menu is up: the pointer is on it, not on a core under it */
static int g_col_w;                 /* the column's width */
static RECT g_work;                 /* the primary screen's work area */
static RECT g_core_rc[MAX_SESS];    /* where each core is, in screen pixels (empty = not drawn) */
static int g_core_n, g_top = -1;    /* g_top: drawn last, hit first */
static Canvas g_cv;
static uint8_t *g_rgba;
static size_t g_rgba_cap;

/* the pointer (screen pixels) */
static double g_last, g_hover_since;
static int g_hover = -1;
static char g_down_sid[72];         /* the core pressed on ("" = none) */
static int g_dragging, g_moving, g_carrying, g_swallow;
static POINT g_down_at, g_drag_at, g_move_mouse, g_move_origin, g_last_mouse;
static double g_drag_t, g_last_mouse_t;
static float g_vx, g_vy, g_mvx, g_mvy;
static HHOOK g_hook;
static double rush_t[128];
static POINT rush_p[128];
static int rush_n;

enum { ID_TIMER = 1, HK_MODE = 1, WM_UNHOOK = WM_APP + 1,
       ID_QUIT = 100, ID_PIN = 400, ID_MODE = 500, ID_FOLLOW, ID_RESET,
       ID_COPY = 1200, ID_FRONT, ID_REPLY, ID_SENDKEYS = 1400, ID_SENDENTER, ID_SETTINGS = 1500 };
static const char *SHAPES[4] = {"dodeca", "bucky", "geo2", "geo3"};
static const float GLOWS[3] = {0.7f, 1.0f, 1.4f};
static const float FONTS[5] = {12, 14, 16, 20, 24};
static const float CORES[4] = {0.25f, 0.35f, 0.45f, 0.55f};   /* the inner core's radius (--core) */
static const float SCALES[4] = {0.75f, 1.0f, 1.3f, 1.6f};     /* how big the cores are on the screen */
static const char *FRAMES[7] = {"segments", "lens", "polar", "diamond", "lock", "orbits", "none"};
static const wchar_t *FRAME_NAMES[7] = {L"Segments", L"Lens", L"Polar", L"Diamond", L"Lock", L"Orbits", L"None"};
static const float GRAVITIES[4] = {1, 2, 4, 8};             /* the orbit spring's strength, times the lightest */
static const double AWAYS[4] = {60, 120, 300, 0};
static const float TIPSIZES[4] = {0.75f, 1.0f, 1.35f, 1.75f};
static const unsigned TIP_BGS[5] = {0x18191E, 0x262626, 0x16233F, 0x2B1C3B, 0x15302A}; /* dark, so the text keeps its contrast */
static const wchar_t *TIP_BG_NAMES[6] = {L"Ink", L"Graphite", L"Navy", L"Plum", L"Pine", L"Custom"};
static const wchar_t *WHEEL_NAMES[5] = {L"Roll", L"Roll back", L"Turn", L"Turn back", L"Off"};
static unsigned tip_bgx(void) { return g_tip_bg == 5 ? g_tip_custom : TIP_BGS[g_tip_bg]; } /* 0xRRGGBB */
static COLORREF tip_bg(void) { unsigned v = tip_bgx(); return RGB(v >> 16 & 255, v >> 8 & 255, v & 255); }
static int box_light(void) { unsigned v = tip_bgx(); return 0.299f * (v >> 16 & 255) + 0.587f * (v >> 8 & 255) + 0.114f * (v & 255) > 140; }
static COLORREF tip_field(void) { /* the typing field: a step lighter on a dark box, a step darker on a light one */
    unsigned v = tip_bgx();
    int r = v >> 16 & 255, g = v >> 8 & 255, b = v & 255, d = box_light() ? -14 : 13;
    return RGB(max(0, min(255, r + d)), max(0, min(255, g + d)), max(0, min(255, b + d + (d > 0 ? 3 : 0))));
}
/* text on the box: as it is on a dark box; on a light one the same hue, dark, so it keeps its contrast */
static COLORREF ink(COLORREF c) { return box_light() ? RGB(GetRValue(c) * 3 / 10, GetGValue(c) * 3 / 10, GetBValue(c) * 3 / 10) : c; }
static const unsigned TEXT_COLS[5] = {0, 0xF2F2F5, 0xF5D9A0, 0x9FE8C2, 0x9CCBFF};
static const wchar_t *TEXT_NAMES[6] = {L"Auto", L"White", L"Cream", L"Mint", L"Sky", L"Custom"};
static unsigned inkx(unsigned v);
/* the reply's own text (and what is typed): the chosen colour, or on Auto the usual light text, dark on a light box */
static unsigned text_bodyx(void) { return g_text_col == 5 ? g_text_custom : g_text_col ? TEXT_COLS[g_text_col] : inkx(0xE6E8F0); }
static COLORREF text_body(void) { unsigned v = text_bodyx(); return RGB(v >> 16 & 255, v >> 8 & 255, v & 255); }
static unsigned inkx(unsigned v) { return box_light() ? ((v >> 16 & 255) * 3 / 10) << 16 | ((v >> 8 & 255) * 3 / 10) << 8 | (v & 255) * 3 / 10 : v; }
/* the wheel over a core: it spins one way or another (Settings > Scroll on a core) */
static void wheel_spin(Session *s, float notches) {
    float k = notches * 1.2f;
    switch (g_wheel_spin) {
    case 0: s->spin_x = fmaxf(-8, fminf(8, s->spin_x - k)); break; /* roll: the top goes away as the wheel goes down */
    case 1: s->spin_x = fmaxf(-8, fminf(8, s->spin_x + k)); break;
    case 2: s->spin_y = fmaxf(-8, fminf(8, s->spin_y - k)); break; /* turn: round the upright axis */
    case 3: s->spin_y = fmaxf(-8, fminf(8, s->spin_y + k)); break;
    }
}
#define DRAG_AFTER ((int)(4 * g_dpi)) /* pixels the mouse moves before a press becomes a drag */

/* --record SCENE FILE (to make the README's animations): the demo on a fixed clock, a scripted pointer in place of
 * the real one (the real mouse is never read or moved), and every frame of the panel's own windows written to FILE */
static char g_rec_scene[16], g_rec_file[1024];
static int g_on_set; static POINT g_on;  /* --on X,Y: the monitor (the one with that desktop point) for --record and the Settings window */
static double g_rec_now;            /* the made-up clock (0: the real one) */
static POINT g_rec_mouse;           /* the scripted pointer */
static int g_rec_still;             /* the demo sessions stop changing state */

static void cursor_pos(POINT *p) { if (g_rec_scene[0]) *p = g_rec_mouse; else GetCursorPos(p); }

static double wall(void) {
    if (g_rec_now > 0) return g_rec_now;
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    return (double)u.QuadPart / 1e7 - 11644473600.0; /* the Unix epoch, like the hook's time.time() in WSL */
}

static Room *shown_room(void) { return g_room.n ? &g_room : &g_ghost; }

static int index_of(const char *sid) {
    Room *r = shown_room();
    for (int i = 0; sid[0] && i < r->n; i++) if (!strcmp(r->s[i].sid, sid)) return i;
    return -1;
}
static int down_index(void) { return index_of(g_down_sid); }

/* ---- saved positions, per screen size --------------------------------------------------------------- */
/* ~/.dodeca-core/panel-position-win.json in WSL: {"1920x1080": [x, y], "1920x1080/<folder>": [x, y], ...}:
 * the cluster (single core), and each core put somewhere by hand (all cores), by its folder name since
 * session ids change every run. Its own file: the macOS panel keeps panel-position.json, y from the bottom. */
typedef struct { char key[96]; int x, y; } Pos;
static Pos g_pos[48];
static int g_pos_n;

static int pos_path(char *out, size_t n) {
    if (!g_dir[0]) return 0;
    snprintf(out, n, "%s\\panel-position-win.json", g_dir);
    return 1;
}

/* The desktop's layout, all monitors together ("3840x1200@0,0"), or that and "/<name>": a core's place (by its
 * folder; "@main" is the single-core mode's main core). Places are desktop coordinates, so they can be on any
 * monitor; another monitor layout keeps its own. 0 if the name cannot be a key. */
static int pos_key(char *k, size_t n, const char *name) {
    int len = snprintf(k, n, "%dx%d@%d,%d", GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN),
                       GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN));
    if (!name) return 1;
    if (!name[0] || strpbrk(name, "\"\\")) return 0;
    snprintf(k + len, n - (size_t)len, "/%s", name);
    return 1;
}

static void pos_read(void) {
    char path[1100], buf[8192];
    g_pos_n = 0;
    if (!pos_path(path, sizeof path)) return;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    size_t len = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[len] = 0;
    for (char *c = strchr(buf, '"'); c && g_pos_n < 48; c = strchr(c + 1, '"')) {
        Pos *p = &g_pos[g_pos_n];
        int used = 0;
        if (sscanf(c, "\"%95[^\"]\" : [ %d , %d ]%n", p->key, &p->x, &p->y, &used) == 3 && used) { g_pos_n++; c += used - 1; }
    }
}

static void pos_write(void) {
    char path[1100];
    if (g_rec_scene[0]) return; /* (a recording keeps nothing) */
    if (!pos_path(path, sizeof path)) return;
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fputc('{', f);
    for (int i = 0; i < g_pos_n; i++) fprintf(f, "%s\"%s\": [%d, %d]", i ? ", " : "", g_pos[i].key, g_pos[i].x, g_pos[i].y);
    fputs("}\n", f);
    fclose(f);
}

static Pos *pos_find(const char *name) {
    char k[96];
    if (!pos_key(k, sizeof k, name)) return NULL;
    for (int i = 0; i < g_pos_n; i++) if (!strcmp(g_pos[i].key, k)) return &g_pos[i];
    return NULL;
}

static void pos_put(const char *name, int x, int y) {
    char k[96];
    if (!pos_key(k, sizeof k, name)) return;
    Pos *p = pos_find(name);
    if (!p) {
        if (g_pos_n == 48) { memmove(g_pos, g_pos + 1, sizeof(Pos) * 47); g_pos_n--; } /* the oldest goes */
        p = &g_pos[g_pos_n++];
        snprintf(p->key, sizeof p->key, "%s", k);
    }
    p->x = x; p->y = y;
    pos_write();
}

/* forget the main core's place (cores = 0) or every core's place (cores = 1) in this monitor layout */
static void pos_forget(int cores) {
    char k[96];
    pos_key(k, sizeof k, NULL);
    size_t kl = strlen(k);
    int j = 0;
    for (int i = 0; i < g_pos_n; i++) {
        int main = !strcmp(g_pos[i].key + (strncmp(g_pos[i].key, k, kl) ? 0 : kl), "/@main");
        int mine = !strncmp(g_pos[i].key, k, kl) && g_pos[i].key[kl] == '/' && (cores ? !main : main);
        if (!mine) g_pos[j++] = g_pos[i];
    }
    g_pos_n = j;
    pos_write();
}

static int any_core_placed(void) {
    char k[96];
    pos_key(k, sizeof k, NULL);
    size_t kl = strlen(k);
    for (int i = 0; i < g_pos_n; i++) if (!strncmp(g_pos[i].key, k, kl) && g_pos[i].key[kl] == '/' && strcmp(g_pos[i].key + kl, "/@main")) return 1;
    return 0;
}

/* the work area (the screen less the taskbar) of the monitor at, or nearest to, a point on the desktop */
static RECT work_at(float x, float y) {
    POINT pt = {(LONG)x, (LONG)y};
    MONITORINFO mi;
    mi.cbSize = sizeof mi;
    if (GetMonitorInfo(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi)) return mi.rcWork;
    RECT r;
    SystemParametersInfo(SPI_GETWORKAREA, 0, &r, 0);
    return r;
}
/* is this point on the desktop in some monitor's work area? (an edge with no screen beyond it is a wall) */
static int on_work(float x, float y) {
    POINT pt = {(LONG)floorf(x), (LONG)floorf(y)};
    HMONITOR hm = MonitorFromPoint(pt, MONITOR_DEFAULTTONULL);
    MONITORINFO mi;
    mi.cbSize = sizeof mi;
    return hm && GetMonitorInfo(hm, &mi) && PtInRect(&mi.rcWork, pt);
}

static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

/* ---- the Settings menu: what it changes is kept in ~/.dodeca-core/panel-settings-win.json and wins over
 * the launch options; "Use launch options again" forgets it ------------------------------------------- */
static struct { float font, core, scale, glow, gravity, tip_size; int others, labels, tip_middle, tip_mono, tip_bg, wheel_spin, text_col; unsigned tip_custom, text_custom; double away; char shape[16], frame[16]; } g_launch;

static int settings_path(char *out, size_t n) {
    if (!g_dir[0]) return 0;
    snprintf(out, n, "%s\\panel-settings-win.json", g_dir);
    return 1;
}

static void remember_launch(void) {
    g_launch.font = g_font; g_launch.core = g_core; g_launch.scale = g_scale; g_launch.glow = g_glow; g_launch.gravity = g_gravity; g_launch.tip_size = g_tip_size; g_launch.others = g_others;
    g_launch.labels = g_labels; g_launch.away = g_away_after; g_launch.tip_middle = g_tip_middle; g_launch.tip_mono = g_tip_mono;
    g_launch.tip_bg = g_tip_bg; g_launch.wheel_spin = g_wheel_spin; g_launch.tip_custom = g_tip_custom; g_launch.text_col = g_text_col; g_launch.text_custom = g_text_custom;
    snprintf(g_launch.shape, sizeof g_launch.shape, "%s", g_shape_name);
    snprintf(g_launch.frame, sizeof g_launch.frame, "%s", g_frame);
}

static void restore_launch(void) {
    motion_map_default(MOTION_MAP); /* the motion map has no launch option: back to its defaults */
    g_font = g_launch.font; g_core = g_launch.core; g_scale = g_launch.scale; g_glow = g_launch.glow; g_gravity = g_launch.gravity; g_tip_size = g_launch.tip_size; g_others = g_launch.others;
    g_labels = g_launch.labels; g_away_after = g_launch.away; g_tip_middle = g_launch.tip_middle; g_tip_mono = g_launch.tip_mono;
    g_tip_bg = g_launch.tip_bg; g_wheel_spin = g_launch.wheel_spin; g_tip_custom = g_launch.tip_custom; g_text_col = g_launch.text_col; g_text_custom = g_launch.text_custom;
    snprintf(g_shape_name, sizeof g_shape_name, "%s", g_launch.shape);
    snprintf(g_frame, sizeof g_frame, "%s", g_launch.frame);
}

static int json_num(const char *j, const char *key, float *v) {
    char k[40];
    snprintf(k, sizeof k, "\"%s\"", key);
    const char *c = strstr(j, k);
    return c && sscanf(c + strlen(k), " : %f", v) == 1;
}

/* "key": [n, n, ...] with ST_N numbers; 0 if missing or short */
static int json_masks(const char *j, const char *key, uint32_t *out) {
    char k[40];
    snprintf(k, sizeof k, "\"%s\"", key);
    const char *c = strstr(j, k);
    if (!c || !(c = strchr(c + strlen(k), '['))) return 0;
    c++;
    for (int i = 0; i < ST_N; i++) {
        char *e;
        unsigned long v = strtoul(c, &e, 10);
        if (e == c) return 0;
        out[i] = (uint32_t)v;
        c = e;
        while (*c == ' ' || *c == ',') c++;
    }
    return 1;
}

static void settings_load(void) {
    char path[1100], buf[4096];
    if (!settings_path(path, sizeof path)) return;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    size_t len = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[len] = 0;
    float v;
    if (json_num(buf, "fontSize", &v) && v >= 8 && v <= 48) g_font = v;
    if (json_num(buf, "core", &v) && v > 0.05f && v < 0.9f) g_core = v;
    if (json_num(buf, "scale", &v) && v >= 0.5f && v <= 2.5f) g_scale = v;
    if (json_num(buf, "glow", &v) && v > 0) g_glow = v;
    if (json_num(buf, "gravity", &v) && v >= 0.5f && v <= 8) g_gravity = v;
    if (json_num(buf, "tipSize", &v) && v >= 0.5f && v <= 2) g_tip_size = v;
    if (json_num(buf, "others", &v)) g_others = v != 0;
    if (json_num(buf, "labels", &v)) g_labels = v != 0;
    if (json_num(buf, "tipMiddle", &v)) g_tip_middle = v != 0;
    if (json_num(buf, "tipMono", &v)) g_tip_mono = v != 0;
    if (json_num(buf, "tipColor", &v) && v >= 0 && v < 6) g_tip_bg = (int)v;
    if (json_num(buf, "tipCustom", &v) && v >= 0 && v <= 0xFFFFFF) g_tip_custom = (unsigned)v;
    if (json_num(buf, "textColor", &v) && v >= 0 && v < 6) g_text_col = (int)v;
    if (json_num(buf, "textCustom", &v) && v >= 0 && v <= 0xFFFFFF) g_text_custom = (unsigned)v;
    if (json_num(buf, "wheelSpin", &v) && v >= 0 && v < 5) g_wheel_spin = (int)v;
    if (json_num(buf, "away", &v) && v >= 0) g_away_after = v;
    char sh[16], *c = strstr(buf, "\"shape\"");
    if (c && sscanf(c, "\"shape\" : \"%15[a-z0-9]\"", sh) == 1) snprintf(g_shape_name, sizeof g_shape_name, "%s", sh);
    if ((c = strstr(buf, "\"frame\"")) && sscanf(c, "\"frame\" : \"%15[a-z]\"", sh) == 1) snprintf(g_frame, sizeof g_frame, "%s", sh);
    uint32_t rt[ST_N], act[ST_N]; /* Settings > Motions */
    MotionMap def[ST_N];
    motion_map_init();
    motion_map_default(def);
    if (json_masks(buf, "mapRoutines", rt) && json_masks(buf, "mapActions", act))
        for (int i = 0; i < ST_N; i++) {
            uint32_t r = rt[i] & ((1u << RT_N) - 1);
            MOTION_MAP[i].rt = r ? r : def[i].rt; /* a state always has a routine */
            MOTION_MAP[i].act = act[i] & ((1u << W_N) - 2);
        }
}

static void settings_save(void) {
    char path[1100];
    if (!settings_path(path, sizeof path)) return;
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "{\"fontSize\": %g, \"core\": %g, \"scale\": %g, \"glow\": %g, \"gravity\": %g, \"tipSize\": %g, \"others\": %d, \"labels\": %d, \"tipMiddle\": %d, \"tipMono\": %d, \"tipColor\": %d, \"tipCustom\": %u, \"textColor\": %d, \"textCustom\": %u, \"wheelSpin\": %d, \"away\": %g, "
               "\"shape\": \"%s\", \"frame\": \"%s\"}\n",
            g_font, g_core, g_scale, g_glow, g_gravity, g_tip_size, g_others, g_labels, g_tip_middle, g_tip_mono, g_tip_bg, g_tip_custom, g_text_col, g_text_custom, g_wheel_spin, g_away_after, g_shape_name, g_frame);
    fseek(f, -2, SEEK_CUR); /* back over "}\n": the motion map goes in the same object */
    fprintf(f, ", \"mapRoutines\": [");
    for (int i = 0; i < ST_N; i++) fprintf(f, "%s%lu", i ? ", " : "", (unsigned long)MOTION_MAP[i].rt);
    fprintf(f, "], \"mapActions\": [");
    for (int i = 0; i < ST_N; i++) fprintf(f, "%s%lu", i ? ", " : "", (unsigned long)MOTION_MAP[i].act);
    fprintf(f, "]}\n");
    fclose(f);
}

static int settings_saved(void) {
    char path[1100];
    return settings_path(path, sizeof path) && GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

/* ---- the windows ---------------------------------------------------------------------------------- */
static void pane_create(Pane *p) {
    p->w = CreateWindowEx(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_ACCEPTFILES,
                          L"VigilPanel", L"Vigil", WS_POPUP, 0, 0, 1, 1, NULL, NULL, GetModuleHandle(NULL), NULL);
    p->mem = CreateCompatibleDC(NULL);
    p->transparent = 1;
    extern IDropTarget g_dt_ref;
    RegisterDragDrop(p->w, &g_dt_ref); /* an image dragged over the core: it reads it */
}

static void pane_size(Pane *p, int w, int h) {
    if (w == p->W && h == p->H && p->bmp) return;
    if (p->bmp) DeleteObject(p->bmp);
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h; /* top-down */
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    p->bmp = CreateDIBSection(p->mem, &bi, DIB_RGB_COLORS, (void **)&p->px, NULL, 0);
    SelectObject(p->mem, p->bmp);
    p->W = w; p->H = h;
}

/* shows the bitmap at (x, y) on the screen */
static void pane_show(Pane *p, int x, int y) {
    POINT src = {0, 0}, dst = {x, y};
    SIZE size = {p->W, p->H};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(p->w, NULL, &dst, &size, p->mem, &src, 0, &blend, ULW_ALPHA);
    p->x = x; p->y = y; p->dirty = 0;
    g_stat_push++;
    if (!p->shown) {
        SetWindowPos(p->w, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW); p->shown = 1;
        if (g_set && IsWindowVisible(g_set)) SetWindowPos(g_set, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); /* (the Settings window stays in front) */
    }
}

static void pane_hide(Pane *p) { if (p->w && p->shown) { ShowWindow(p->w, SW_HIDE); p->shown = 0; p->dirty = 1; } }

/* only the place changes: no new picture for the compositor to take */
static void pane_move(Pane *p, int x, int y) {
    if (x == p->x && y == p->y) return;
    SetWindowPos(p->w, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    p->x = x; p->y = y;
}

/* Single core: the main core's centre on the desktop, and the cluster's span (sets the main core's size and the
 * orbit). Every core has its own window; the cluster is only where the main core is and where its orbit runs. */
static float g_mx, g_my;
static int g_cw;

static void clamp_main(void) { /* the main core stays wholly on the monitor it is on */
    RECT wa = work_at(g_mx, g_my);
    float r = g_cw * .32f;
    g_mx = clampf(g_mx, wa.left + r, wa.right - r);
    g_my = clampf(g_my, wa.top + r, wa.bottom - r);
}

/* where it was put last, else near the top of the primary screen's right edge */
static void main_place(void) {
    g_cw = (int)(fmax(200 * g_dpi, fmin(320 * g_dpi, g_col_w)) * g_scale);
    Pos *p = pos_find("@main");
    if (p) { g_mx = (float)p->x; g_my = (float)p->y; }
    else { g_mx = g_work.right - g_cw * .5f; g_my = g_work.top + g_cw * .6f; }
    clamp_main();
}

/* ---- drawing -------------------------------------------------------------------------------------- */
/* Each core's picture is kept and made again only as often as its state needs (session_rate, as in the macOS
 * panel: asleep 4 a second, idle 10, working 20, more while the mouse is on it, 60 while it is in the hand).
 * A window gets a new bitmap only when one of its pictures changed; moving it is just a new position. */
typedef struct { char sid[72]; uint32_t *img; int px; size_t cap; double due; } Pic; /* premultiplied BGRA */
static Pic g_pic[MAX_SESS];

static double core_rate(const Session *s, double now, float k) {
    if (g_down_sid[0] && !strcmp(s->sid, g_down_sid) && (g_dragging || g_moving)) return 60; /* in the hand */
    if (s->hover_to > 0 || s->carried) return 60; /* the mouse on it: it answers at once */
    double r = session_rate(s, now, 60) * k;
    return r > 60 ? 60 : r < 2 ? 2 : r;
}

static void pics_invalidate(void) { for (int i = 0; i < MAX_SESS; i++) g_pic[i].due = 0; }

/* a session's picture at px pixels, made again if it is due; 1 if it changed */
static int pic_make(Pic *p, const Session *s, int px, double now, float k) {
    if (px < 8) px = 8;
    if (p->img && p->px == px && now < p->due && !strcmp(p->sid, s->sid)) return 0;
    size_t need = (size_t)px * px;
    if (need > p->cap) { free(p->img); p->img = malloc(need * 4); p->cap = need; }
    canvas_resize(&g_cv, px, px);
    render_one(&g_cv, s, &g_shape, now, g_glow, g_core);
    if (need * 4 > g_rgba_cap) { free(g_rgba); g_rgba = malloc(need * 4); g_rgba_cap = need * 4; }
    canvas_rgba(&g_cv, g_rgba);
    const uint8_t *src = g_rgba;
    for (size_t j = 0; j < need; j++, src += 4) /* RGBA → BGRA, both premultiplied */
        p->img[j] = (uint32_t)src[2] | (uint32_t)src[1] << 8 | (uint32_t)src[0] << 16 | (uint32_t)src[3] << 24;
    snprintf(p->sid, sizeof p->sid, "%s", s->sid);
    p->px = px;
    p->due = now + 1.0 / core_rate(s, now, k) - 0.004;
    g_stat_pic++;
    return 1;
}

static int pic_update(int i, const Session *s, int px, double now, float k) { return pic_make(&g_pic[i], s, px, now, k); }

/* the same at a fractional position (bilinear), so a slowly drifting core moves smoothly, not in whole pixels */
static int g_blit_add; /* the Settings preview: add the light onto its opaque card instead of laying it over */
static void blit_pic_at(Pane *pn, float fx, float fy, const Pic *p) {
    int ix = (int)floorf(fx), iy = (int)floorf(fy);
    float ax = fx - ix, ay = fy - iy;
    if (ax < 0.02f && ay < 0.02f && !g_blit_add) { void blit_pic(Pane *, int, int, const Pic *); blit_pic(pn, ix, iy, p); return; }
    int w00 = (int)((1 - ax) * (1 - ay) * 256 + 0.5f), w10 = (int)(ax * (1 - ay) * 256 + 0.5f), w01 = (int)((1 - ax) * ay * 256 + 0.5f), w11 = 256 - w00 - w10 - w01;
    int n = p->px;
    for (int y = 0; y <= n; y++) {
        int Y = iy + y;
        if (Y < 0 || Y >= pn->H) continue;
        uint32_t *d = pn->px + (size_t)Y * pn->W;
        for (int x = 0; x <= n; x++) {
            int X = ix + x;
            if (X < 0 || X >= pn->W) continue;
            /* the destination pixel takes from the source pixels up-left of it */
            uint32_t q00 = x > 0 && y > 0 ? p->img[(size_t)(y - 1) * n + x - 1] : 0, q10 = x < n && y > 0 ? p->img[(size_t)(y - 1) * n + x] : 0;
            uint32_t q01 = x > 0 && y < n ? p->img[(size_t)y * n + x - 1] : 0, q11 = x < n && y < n ? p->img[(size_t)y * n + x] : 0;
            if (!(q00 | q10 | q01 | q11)) continue;
            uint32_t sp = 0;
            for (int sh = 0; sh < 32; sh += 8) {
                uint32_t v = ((q00 >> sh & 0xff) * w11 + (q10 >> sh & 0xff) * w01 + (q01 >> sh & 0xff) * w10 + (q11 >> sh & 0xff) * w00 + 128) >> 8;
                sp |= (v > 255 ? 255 : v) << sh;
            }
            uint32_t sa = sp >> 24, was = d[X];
            if (!sa) continue;
            if (g_blit_add) { /* light adds: the card under a faint glow keeps its colour */
                uint32_t out = 0xff000000u;
                for (int sh = 0; sh < 24; sh += 8) { uint32_t v = (sp >> sh & 0xff) + (was >> sh & 0xff); out |= (v > 255 ? 255 : v) << sh; }
                d[X] = out;
                continue;
            }
            if (!was || sa == 255) { d[X] = sp; continue; }
            uint32_t k = 255u - sa, out = 0;
            for (int sh = 0; sh < 32; sh += 8) {
                uint32_t v = (sp >> sh & 0xff) + ((was >> sh & 0xff) * k + 127) / 255;
                out |= (v > 255 ? 255 : v) << sh;
            }
            d[X] = out;
        }
    }
}

/* lays a picture over the pane at (x, y), premultiplied "over", clipped */
void blit_pic(Pane *pn, int x0, int y0, const Pic *p) {
    for (int y = 0; y < p->px; y++) {
        int Y = y0 + y;
        if (Y < 0 || Y >= pn->H) continue;
        const uint32_t *src = p->img + (size_t)y * p->px;
        uint32_t *d = pn->px + (size_t)Y * pn->W;
        for (int x = 0; x < p->px; x++) {
            int X = x0 + x;
            uint32_t sp = src[x], sa = sp >> 24;
            if (!sa || X < 0 || X >= pn->W) continue;
            uint32_t was = d[X];
            if (!was || sa == 255) { d[X] = sp; continue; }
            uint32_t k = 255u - sa, out = 0;
            for (int sh = 0; sh < 32; sh += 8) {
                uint32_t v = (sp >> sh & 0xff) + ((was >> sh & 0xff) * k + 127) / 255;
                out |= (v > 255 ? 255 : v) << sh;
            }
            d[X] = out;
        }
    }
}

/* ---- antialiased drawing into a pane (the focus frame, the labels' dot); pane pixels, y down ----------- */
static void px_over(Pane *pn, int x, int y, Col c, float a) {
    if (x < 0 || y < 0 || x >= pn->W || y >= pn->H || a <= 0.002f) return;
    if (a > 1) a = 1;
    uint32_t *d = &pn->px[(size_t)y * pn->W + x], was = *d;
    float k = 1 - a;
    int b = (int)(fminf(1, c.b) * a * 255 + (was & 0xff) * k + 0.5f), g = (int)(fminf(1, c.g) * a * 255 + (was >> 8 & 0xff) * k + 0.5f);
    int r = (int)(fminf(1, c.r) * a * 255 + (was >> 16 & 0xff) * k + 0.5f), al = (int)(a * 255 + (was >> 24) * k + 0.5f);
    *d = (uint32_t)(b > 255 ? 255 : b) | (uint32_t)(g > 255 ? 255 : g) << 8 | (uint32_t)(r > 255 ? 255 : r) << 16 | (uint32_t)(al > 255 ? 255 : al) << 24;
}

/* how much of a pixel at distance d from the middle of a line w wide is covered */
static float cover(float d, float w) { return w >= 1 ? fminf(1, fmaxf(0, w * 0.5f + 0.5f - d)) : w * fmaxf(0, 1 - d); }

static void box_of(Pane *pn, float x0, float y0, float x1, float y1, float pad, int *bx0, int *by0, int *bx1, int *by1) {
    *bx0 = (int)floorf(fminf(x0, x1) - pad); *by0 = (int)floorf(fminf(y0, y1) - pad);
    *bx1 = (int)ceilf(fmaxf(x0, x1) + pad); *by1 = (int)ceilf(fmaxf(y0, y1) + pad);
    if (*bx0 < 0) *bx0 = 0;
    if (*by0 < 0) *by0 = 0;
    if (*bx1 > pn->W) *bx1 = pn->W;
    if (*by1 > pn->H) *by1 = pn->H;
}

static void aa_seg(Pane *pn, float x0, float y0, float x1, float y1, float w, Col c, float a) {
    int bx0, by0, bx1, by1;
    box_of(pn, x0, y0, x1, y1, w * 0.5f + 1.5f, &bx0, &by0, &bx1, &by1);
    float dx = x1 - x0, dy = y1 - y0, l2 = dx * dx + dy * dy;
    for (int y = by0; y < by1; y++)
        for (int x = bx0; x < bx1; x++) {
            float px = x + 0.5f, py = y + 0.5f, t = l2 > 0 ? ((px - x0) * dx + (py - y0) * dy) / l2 : 0;
            t = fmaxf(0, fminf(1, t));
            float cv = cover(hypotf(x0 + t * dx - px, y0 + t * dy - py), w);
            if (cv > 0) px_over(pn, x, y, c, a * cv);
        }
}

static void aa_ring(Pane *pn, float ox, float oy, float rad, float w, Col c, float a) {
    int bx0, by0, bx1, by1;
    box_of(pn, ox - rad, oy - rad, ox + rad, oy + rad, w * 0.5f + 1.5f, &bx0, &by0, &bx1, &by1);
    for (int y = by0; y < by1; y++)
        for (int x = bx0; x < bx1; x++) {
            float d = fabsf(hypotf(x + 0.5f - ox, y + 0.5f - oy) - rad);
            if (d > w * 0.5f + 1) continue;
            px_over(pn, x, y, c, a * cover(d, w));
        }
}

/* the arc from angle g1 to g2 (counter-clockwise on the screen, like the macOS panel's) */
static void aa_arc(Pane *pn, float ox, float oy, float rad, double g1, double g2, float w, Col c, float a) {
    double span = g2 - g1, TAU = 2 * M_PI;
    if (span >= TAU - 1e-3) { aa_ring(pn, ox, oy, rad, w, c, a); return; }
    float mnx = 1e9f, mny = 1e9f, mxx = -1e9f, mxy = -1e9f;
    for (int j = 0; j <= 16; j++) {
        double g = g1 + span * j / 16;
        float x = ox + (float)cos(g) * rad, y = oy - (float)sin(g) * rad;
        mnx = fminf(mnx, x); mny = fminf(mny, y); mxx = fmaxf(mxx, x); mxy = fmaxf(mxy, y);
    }
    int bx0, by0, bx1, by1;
    box_of(pn, mnx, mny, mxx, mxy, w * 0.5f + 2, &bx0, &by0, &bx1, &by1);
    float ex0 = ox + (float)cos(g1) * rad, ey0 = oy - (float)sin(g1) * rad, ex1 = ox + (float)cos(g2) * rad, ey1 = oy - (float)sin(g2) * rad;
    for (int y = by0; y < by1; y++)
        for (int x = bx0; x < bx1; x++) {
            float px = x + 0.5f, py = y + 0.5f, vx = px - ox, vy = oy - py;
            double rel = fmod(atan2(vy, vx) - g1, TAU);
            if (rel < 0) rel += TAU;
            float d = rel <= span ? fabsf(hypotf(vx, vy) - rad) : fminf(hypotf(px - ex0, py - ey0), hypotf(px - ex1, py - ey1));
            float cv = cover(d, w);
            if (cv > 0) px_over(pn, x, y, c, a * cv);
        }
}

static void aa_disc(Pane *pn, float cx, float cy, float rad, Col c, float a) {
    int bx0, by0, bx1, by1;
    box_of(pn, cx - rad, cy - rad, cx + rad, cy + rad, 1.5f, &bx0, &by0, &bx1, &by1);
    for (int y = by0; y < by1; y++)
        for (int x = bx0; x < bx1; x++) {
            float cv = fminf(1, fmaxf(0, rad + 0.5f - hypotf(x + 0.5f - cx, y + 0.5f - cy)));
            if (cv > 0) px_over(pn, x, y, c, a * cv);
        }
}

static void aa_box(Pane *pn, float x0, float y0, float x1, float y1, Col c, float a) {
    int bx0, by0, bx1, by1;
    box_of(pn, x0, y0, x1, y1, 1, &bx0, &by0, &bx1, &by1);
    for (int y = by0; y < by1; y++)
        for (int x = bx0; x < bx1; x++) {
            float cx = fminf(x + 1.0f, x1) - fmaxf((float)x, x0), cy = fminf(y + 1.0f, y1) - fmaxf((float)y, y0);
            if (cx > 0 && cy > 0) px_over(pn, x, y, c, a * cx * cy);
        }
}

static void aa_tri(Pane *pn, const float *P, Col c, float a) { /* P: x0 y0 x1 y1 x2 y2; 4 samples a pixel */
    int bx0, by0, bx1, by1;
    box_of(pn, fminf(P[0], fminf(P[2], P[4])), fminf(P[1], fminf(P[3], P[5])), fmaxf(P[0], fmaxf(P[2], P[4])), fmaxf(P[1], fmaxf(P[3], P[5])), 1, &bx0, &by0, &bx1, &by1);
    for (int y = by0; y < by1; y++)
        for (int x = bx0; x < bx1; x++) {
            int in = 0;
            for (int sm = 0; sm < 4; sm++) {
                float px = x + 0.25f + 0.5f * (sm & 1), py = y + 0.25f + 0.5f * (sm >> 1);
                float e0 = (P[2] - P[0]) * (py - P[1]) - (P[3] - P[1]) * (px - P[0]);
                float e1 = (P[4] - P[2]) * (py - P[3]) - (P[5] - P[3]) * (px - P[2]);
                float e2 = (P[0] - P[4]) * (py - P[5]) - (P[1] - P[5]) * (px - P[4]);
                in += (e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0);
            }
            if (in) px_over(pn, x, y, c, a * in / 4);
        }
}

/* ---- the focus frame: drawn round a core while it is carried, or after the pointer has rested on it 2 s;
 * the macOS panel's six styles (focus_frame in mac/panel.m), angles counter-clockwise, y down ------------ */
static float g_frame_k;             /* the Settings preview: the real core's frame scale, shrunk with the preview (0 = work it out) */
static float frame_k(float side) { return g_frame_k > 0 ? g_frame_k : (float)(fmax(0.5, side / g_dpi / 300.0) * g_dpi); }

static void tick_mark(Pane *pn, float ox, float oy, double g, float r0, float r1, float w, Col c, float a) {
    aa_seg(pn, ox + (float)cos(g) * r0, oy - (float)sin(g) * r0, ox + (float)cos(g) * r1, oy - (float)sin(g) * r1, w, c, a);
}

static void focus_frame(Pane *pn, RECT r, const Session *s, double t) {
    float a = fmaxf(s->mo.lock, s->carry);
    if (a < 0.01f || !strcmp(g_frame, "none") || r.right - r.left < 12) return;
    if (a > 1) a = 1;
    float side = (float)(r.right - r.left), ox = (r.left + r.right) * 0.5f, oy = (r.top + r.bottom) * 0.5f, shell = side * SHELL_FRAC;
    float u = 1 - powf(1 - a, 3), rr = shell * (1.55f - 0.3f * u), k = frame_k(side);
    double TAU = 2 * M_PI;
    Col col = s->cur.line, gl = s->cur.eh;
    float ca = 0.75f * a, fa = 0.25f * a, ga = 0.2f * a; /* the line, faint marks, the glow under lit lines */
    if (!strcmp(g_frame, "segments")) { /* 12 segments with a lit run travelling round, a fine scale inside */
        double head = fmod(t * 5, 12);
        for (int i = 0; i < 12; i++) {
            double g1 = i / 12.0 * TAU + M_PI / 2 + 0.05, g2 = g1 + TAU / 12 - 0.1, d = fmod(head - i + 12, 12), on = d < 3 ? 1 - d / 3 : 0;
            if (on > 0) aa_arc(pn, ox, oy, rr, g1, g2, 6 * k, gl, ga);
            aa_arc(pn, ox, oy, rr, g1, g2, (float)(1.2 + 1.8 * on) * k, col, on > 0 ? ca : fa);
        }
        for (int i = 0; i < 72; i++) tick_mark(pn, ox, oy, i / 72.0 * TAU, rr - 6 * k, rr - (i % 6 ? 9 : 13) * k, 1 * k, col, fa);
    } else if (!strcmp(g_frame, "lens")) { /* two graduated arcs, ( ), with end caps and a long centre mark */
        double half = 0.78;
        for (int sd = 0; sd < 2; sd++) {
            double sdg = sd * M_PI;
            aa_arc(pn, ox, oy, rr, sdg - half, sdg + half, 5 * k, gl, ga);
            aa_arc(pn, ox, oy, rr, sdg - half, sdg + half, 1.4f * k, col, ca);
            for (int i = -6; i <= 6; i++) if (i) tick_mark(pn, ox, oy, sdg + half * i / 6, rr, rr - (abs(i) == 3 ? 9 : 5) * k, 1 * k, col, fa);
            tick_mark(pn, ox, oy, sdg, rr + 8 * k, rr + 26 * k, 1 * k, col, fa);
            tick_mark(pn, ox, oy, sdg, rr, rr - 16 * k, 1.6f * k, col, ca);
            tick_mark(pn, ox, oy, sdg - half, rr - 5 * k, rr + 5 * k, 1.6f * k, col, ca);
            tick_mark(pn, ox, oy, sdg + half, rr - 5 * k, rr + 5 * k, 1.6f * k, col, ca);
        }
    } else if (!strcmp(g_frame, "polar")) { /* spokes between two rings, turning slowly, a marker riding outside */
        float r1 = rr - 12 * k;
        aa_ring(pn, ox, oy, rr, 1 * k, col, fa);
        aa_ring(pn, ox, oy, r1, 1 * k, col, fa);
        for (int i = 0; i < 24; i++) {
            double g = t * 0.12 + i / 24.0 * TAU;
            if (i % 6) tick_mark(pn, ox, oy, g, r1, rr, 1 * k, col, fa);
            else { tick_mark(pn, ox, oy, g, r1, rr + 8 * k, 5 * k, gl, ga); tick_mark(pn, ox, oy, g, r1, rr + 8 * k, 1.5f * k, col, ca); }
        }
        double gm = -t * 0.6;
        float P[6] = {ox + (float)cos(gm) * (rr + 7 * k), oy - (float)sin(gm) * (rr + 7 * k),
                      ox + (float)cos(gm - 0.05) * (rr + 16 * k), oy - (float)sin(gm - 0.05) * (rr + 16 * k),
                      ox + (float)cos(gm + 0.05) * (rr + 16 * k), oy - (float)sin(gm + 0.05) * (rr + 16 * k)};
        aa_tri(pn, P, col, ca);
    } else if (!strcmp(g_frame, "diamond")) { /* a square on its point: notched sides, rings at the corners, a dotted inner square */
        float s2 = rr * 1.12f, V[4][2];
        for (int i = 0; i < 4; i++) { V[i][0] = ox + (float)cos(i * M_PI / 2 + M_PI / 2) * s2; V[i][1] = oy - (float)sin(i * M_PI / 2 + M_PI / 2) * s2; }
        for (int i = 0; i < 4; i++) {
            float *a0 = V[i], *b0 = V[(i + 1) % 4];
            for (int h = 0; h < 2; h++) {
                float f0 = h ? 0.56f : 0.1f, f1 = h ? 0.9f : 0.44f;
                float x0 = a0[0] + (b0[0] - a0[0]) * f0, y0 = a0[1] + (b0[1] - a0[1]) * f0, x1 = a0[0] + (b0[0] - a0[0]) * f1, y1 = a0[1] + (b0[1] - a0[1]) * f1;
                aa_seg(pn, x0, y0, x1, y1, 5 * k, gl, ga);
                aa_seg(pn, x0, y0, x1, y1, 1.4f * k, col, ca);
            }
            aa_disc(pn, (a0[0] + b0[0]) / 2, (a0[1] + b0[1]) / 2, 2.2f * k, col, ca);
            aa_disc(pn, a0[0], a0[1], 1.6f * k, col, ca);
            aa_ring(pn, a0[0], a0[1], 6 * k, 1 * k, col, fa);
        }
        for (int i = 0; i < 4; i++) { /* the dotted inner square: dashes 2 on, 5 off */
            float qx0 = ox + (V[i][0] - ox) * 0.86f, qy0 = oy + (V[i][1] - oy) * 0.86f;
            float qx1 = ox + (V[(i + 1) % 4][0] - ox) * 0.86f, qy1 = oy + (V[(i + 1) % 4][1] - oy) * 0.86f;
            float len = hypotf(qx1 - qx0, qy1 - qy0), on = 2 * k, off = 5 * k;
            for (float d = 0; d < len; d += on + off) {
                float e = fminf(len, d + on);
                aa_seg(pn, qx0 + (qx1 - qx0) * d / len, qy0 + (qy1 - qy0) * d / len, qx0 + (qx1 - qx0) * e / len, qy0 + (qy1 - qy0) * e / len, 1 * k, col, fa);
            }
        }
    } else if (!strcmp(g_frame, "lock")) { /* four corner pieces slide in from outside and lock square */
        float s2 = rr * (0.9f + 0.45f * (1 - u)), l = rr * 0.26f;
        int sg[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (int i = 0; i < 4; i++) {
            float sx = (float)sg[i][0], sy = (float)sg[i][1], cx = ox + sx * s2, cy = oy + sy * s2;
            aa_seg(pn, cx, cy - sy * l, cx, cy, 5 * k, gl, ga); aa_seg(pn, cx, cy, cx - sx * l, cy, 5 * k, gl, ga);
            aa_seg(pn, cx, cy - sy * l, cx, cy, 1.8f * k, col, ca); aa_seg(pn, cx, cy, cx - sx * l, cy, 1.8f * k, col, ca);
            aa_box(pn, cx - 3 * k + sx * 3 * k, cy - 3 * k + sy * 3 * k, cx + 3 * k + sx * 3 * k, cy + 3 * k + sy * 3 * k, col, ca);
            aa_seg(pn, cx - sx * l * 0.5f, cy, cx - sx * l * 0.5f, cy + sy * 6 * k, 1 * k, col, fa);
            aa_seg(pn, cx, cy - sy * l * 0.5f, cx + sx * 6 * k, cy - sy * l * 0.5f, 1 * k, col, fa);
        }
    } else if (!strcmp(g_frame, "orbits")) { /* three short arcs on three radii, each its own way and speed, a bead leading each */
        aa_ring(pn, ox, oy, rr * 1.07f, 1 * k, col, fa);
        static const double O[3][3] = {{1, 0.9, 1.1}, {1.07, -0.6, 0.7}, {1.14, 0.35, 1.6}};
        for (int i = 0; i < 3; i++) {
            double g = t * O[i][1] + i * 2.1, a0 = O[i][1] > 0 ? g - O[i][2] : g, a1 = O[i][1] > 0 ? g : g + O[i][2];
            float rad = rr * (float)O[i][0];
            aa_arc(pn, ox, oy, rad, a0, a1, 5 * k, gl, ga);
            aa_arc(pn, ox, oy, rad, a0, a1, 1.4f * k, col, ca);
            aa_disc(pn, ox + (float)cos(g) * rad, oy - (float)sin(g) * rad, 3 * k, col, ca);
        }
    }
}

/* the room a frame needs outside its core's square */
static int frame_margin(int side) { return strcmp(g_frame, "none") ? (int)ceilf(0.07f * side + 30 * frame_k((float)side)) : 0; }

/* ---- labels (all cores): a dot in the shell's glow colour and the folder name, then the state in grey ---- */
static HFONT g_label_font;
static HDC g_tdc;
static HBITMAP g_tbmp;
static uint32_t *g_tpx;
static int g_tw, g_th;

static int label_line_h(void) { return (int)(g_font * g_dpi * 1.25f) + (int)(3 * g_dpi); }

/* text drawn white on black with GDI into a scratch bitmap, then laid into the pane in colour c, with the
 * grey levels as coverage (a layered window needs real alpha, which GDI text does not write) */
static void text_into(Pane *pn, int x, int y, int w, int h, const char *u8, Col c) {
    if (w <= 0 || h <= 0) return;
    if (!g_label_font) {
        g_label_font = CreateFontW(-(int)(g_font * g_dpi + 0.5f), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                                   ANTIALIASED_QUALITY, FIXED_PITCH, L"Consolas");
    }
    if (!g_tdc) g_tdc = CreateCompatibleDC(NULL);
    if (w > g_tw || h > g_th) {
        if (g_tbmp) DeleteObject(g_tbmp);
        g_tw = w > g_tw ? w : g_tw; g_th = h > g_th ? h : g_th;
        BITMAPINFO bi;
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = g_tw; bi.bmiHeader.biHeight = -g_th;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        g_tbmp = CreateDIBSection(g_tdc, &bi, DIB_RGB_COLORS, (void **)&g_tpx, NULL, 0);
        SelectObject(g_tdc, g_tbmp);
    }
    for (int j = 0; j < h; j++) memset(g_tpx + (size_t)j * g_tw, 0, (size_t)w * 4);
    wchar_t wt[120];
    MultiByteToWideChar(CP_UTF8, 0, u8, -1, wt, 120);
    wt[119] = 0;
    SelectObject(g_tdc, g_label_font);
    SetTextColor(g_tdc, RGB(255, 255, 255));
    SetBkMode(g_tdc, TRANSPARENT);
    RECT r = {0, 0, w, h};
    DrawTextW(g_tdc, wt, -1, &r, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS | DT_VCENTER);
    GdiFlush();
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            uint32_t v = g_tpx[(size_t)j * g_tw + i] >> 8 & 0xff;
            if (v) px_over(pn, x + i, y + j, c, v / 255.0f);
        }
}

static void state_line(const Session *s, double now, char *out, size_t n) {
    int st = session_shown(s, now);
    if (!strcmp(s->sid, "ghost")) snprintf(out, n, "Waiting for Claude Code");
    else if (st == ST_WORK && s->tool[0]) snprintf(out, n, "%s \xC2\xB7 %s", STATE_TEXT[st], s->tool);
    else snprintf(out, n, "%s", STATE_TEXT[st]);
}

static void draw_label(Pane *pn, int x, int y, int w, const Session *s, double now) {
    int lh = label_line_h(), dot = (int)(g_font * g_dpi * 0.9f);
    Col name_c = {0.89f, 0.90f, 0.93f}, state_c = {0.52f, 0.54f, 0.59f};
    char st[80];
    aa_disc(pn, x + dot * 0.5f, y + lh * 0.5f, g_font * (float)g_dpi * 0.3f, s->cur.eh, 1); /* follows the shell's glow as it eases between states */
    text_into(pn, x + dot, y, w - dot, lh, s->name[0] ? s->name : "claude", name_c);
    state_line(s, now, st, sizeof st);
    text_into(pn, x + dot, y + lh, w - dot, lh, st, state_c);
}

/* All cores: each its own window, as large as a column of them would allow. A core put somewhere by hand
 * stays there; the rest fill the column on the right edge from the top, closing up the gaps. */
static int g_hold_x, g_hold_y;      /* where the core being moved is (screen, its square's top-left) */

static void draw_all(Room *room, int n, double now) {
    int gap = (int)(10 * g_dpi), top = (int)(30 * g_dpi), H = g_work.bottom - g_work.top, colw = (int)(g_col_w * g_scale);
    int texth = g_labels ? 2 * label_line_h() + (int)(4 * g_dpi) : 0;
    int side = (H - top) / (n > 0 ? n : 1) - gap - texth;
    if (side > colw - (int)(12 * g_dpi)) side = colw - (int)(12 * g_dpi);
    if (side < 24) side = 24;
    int m = frame_margin(side), pw = side + 2 * m, ph = texth + side + 2 * m; /* each window: label, core, room for the frame */
    int held = g_moving ? down_index() : -1, free_n = 0;
    Pos *at[MAX_SESS];
    for (int i = 0; i < n; i++) {
        at[i] = i == held || !g_room.n ? NULL : pos_find(room->s[i].name);
        if (!at[i] && i != held) free_n++;
    }
    int step = texth + side + gap, total = free_n * step, y0 = g_work.top + top + ((H - top) - total) / 2;
    if (y0 < g_work.top + top) y0 = g_work.top + top;
    int cx = g_work.right - colw + (colw - side) / 2;
    for (int i = 0, k = 0; i < n; i++) {
        int x, y; /* the core's square, on the screen */
        if (i == held) { x = g_hold_x; y = g_hold_y; }
        else if (at[i]) {
            RECT wa = work_at(at[i]->x + side * .5f, at[i]->y + side * .5f); /* on whichever monitor it was put */
            x = (int)fmax(wa.left, fmin(wa.right - side, at[i]->x));
            y = (int)fmax(wa.top + texth, fmin(wa.bottom - side, at[i]->y));
        } else { x = cx; y = y0 + k++ * step + texth; }
        SetRect(&g_core_rc[i], x, y, x + side, y + side);
        Pane *p = &g_pane[i];
        if (!p->w) pane_create(p);
        int changed = pic_update(i, &room->s[i], side, now, 1.5f);
        if (changed || p->dirty || !p->shown || p->W != pw || p->H != ph) {
            pane_size(p, pw, ph);
            memset(p->px, 0, (size_t)pw * ph * 4);
            RECT local = {m, m + texth, m + side, m + texth + side};
            if (g_labels) draw_label(p, m, m, side, &room->s[i], now);
            blit_pic(p, m, m + texth, &g_pic[i]);
            focus_frame(p, local, &room->s[i], now);
            pane_show(p, x - m, y - m - texth);
        } else pane_move(p, x - m, y - m - texth);
    }
    for (int i = n; i < MAX_SESS; i++) pane_hide(&g_pane[i]);
    g_top = held;
    if (held >= 0) SetWindowPos(g_pane[held].w, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

/* Each core of the cluster as a body on the desktop: the main core sits at (g_mx, g_my), each small core is held
 * to the orbit by a spring (its gravity, Settings > Gravity) and drawn along it to its place by a weaker one. While the cluster is
 * carried only gravity holds them, so they slide freely round the orbit to the side away from the motion and trail
 * behind; put down, they slide home. Pulled too far (carried very fast) gravity lets go: the small core flies
 * on with its speed, slowing slowly and bouncing off the screen's edges, until the main core comes close again. Alt-dragging a small core takes it
 * out of orbit by hand. */
typedef struct { char sid[72]; float px, py, vx, vy, w, rest; int free, init; } Orbit;

/* the springs: onto the orbit (gravity, strong) and along it to its place (weaker), where gravity lets go, where
 * the main core catches a loose one; for an orbit rx across with small cores `small` wide */
typedef struct { float K, C, KT, CT, BREAK, CATCH; } OrbitK;
static OrbitK orbit_k(float rx, float small) {
    OrbitK k = {150 * g_gravity, 19.6f * sqrtf(g_gravity), 36, 11, 3.2f * rx, 1.35f * rx + small};
    return k;
}

/* one step of a small core on its orbit: a bead on a ring (rx, ry) round (cx, cy), held onto the ring and drawn
 * along it toward its place at angle a (tx, ty) with strength kt (0: it slides freely); the others on the ring
 * push it off when they touch. Returns its distance from the centre (past k->BREAK it should let go). */
static float bead_step(Orbit *ob, float cx, float cy, float tx, float ty, double a, float rx, float ry, float small, float kt,
                       const OrbitK *k, Orbit **ring, int rn, float dt) {
    float d = 0;
    for (int st = 0; st < 4; st++) {
        float sub = dt / 4, dx = ob->px - cx, dy = ob->py - cy;
        d = hypotf(dx, dy);
        if (d < 1) { dx = tx - cx; dy = ty - cy; d = fmaxf(1, hypotf(dx, dy)); } /* (right on the centre: push it out towards its place) */
        float nx = dx / d, ny = dy / d, txn = ny, tyn = -nx; /* outwards, and along the ring the way its angle grows */
        double th = atan2(-dy, dx), da = remainder(a - th, 2 * M_PI);
        float rad = hypotf(rx * (float)cos(th), ry * (float)sin(th));
        float vr = ob->vx * nx + ob->vy * ny, vt = ob->vx * txn + ob->vy * tyn;
        float fr = k->K * (rad - d) - k->C * vr, ft = kt * (float)da * rad - k->CT * vt;
        float bx = 0, by = 0;
        for (int j = 0; j < rn; j++) {
            if (ring[j] == ob) continue;
            float ex = ob->px - ring[j]->px, ey = ob->py - ring[j]->py, e = hypotf(ex, ey), gap = small * 1.05f - e;
            if (gap > 0 && e > .01f) { bx += k->K * gap * ex / e; by += k->K * gap * ey / e; }
        }
        ob->vx += (fr * nx + ft * txn + bx) * sub; ob->vy += (fr * ny + ft * tyn + by) * sub;
        ob->px += ob->vx * sub; ob->py += ob->vy * sub;
    }
    return d;
}

/* a loose small core: it flies on, slowing a little (the caller bounces it off its walls) */
static const float BOUNCE = 0.7f; /* how much speed a bounce keeps */
static void fly_step(Orbit *ob, float dt) {
    float fr = expf(-dt * 0.9f);
    ob->vx *= fr; ob->vy *= fr; ob->px += ob->vx * dt; ob->py += ob->vy * dt;
    if (hypotf(ob->vx, ob->vy) < 4) { ob->vx = ob->vy = 0; ob->rest += dt; } else ob->rest = 0; /* (at rest) */
}
static Orbit g_orbit[MAX_SESS];
static int g_orbit_n;

static Orbit *orbit_of(const char *sid) {
    for (int i = 0; i < g_orbit_n; i++) if (!strcmp(g_orbit[i].sid, sid)) return &g_orbit[i];
    if (g_orbit_n == MAX_SESS) g_orbit_n = 0;
    Orbit *o = &g_orbit[g_orbit_n++];
    memset(o, 0, sizeof *o);
    snprintf(o->sid, sizeof o->sid, "%s", sid);
    return o;
}

static RECT g_cluster_rc;           /* the main core and its orbit, on the desktop (the text box goes beside it) */
static double g_last_draw;

static void draw_single(Room *room, int n, double now) {
    int fi = 0;
    for (int i = 0; i < n; i++) if (!strcmp(room->s[i].sid, g_focus)) fi = i;
    float dt = (float)fmin(0.1, now - g_last_draw);
    g_last_draw = now;
    static double orbit; /* the small cores' own clock: it stops while the pointer is on a core, so one stays put to be scrolled */
    if (g_hover < 0) orbit += dt;
    float big = g_cw * 0.6f, rx = g_cw / 2.0f - (float)(8 * g_dpi), ry = rx * 1.15f;
    float small = g_others && n > 1 ? fminf(g_cw * 0.2f, (float)(2 * M_PI * rx / (n - 1) * 0.75)) : 0;
    rx -= small / 2; ry -= small / 2;
    /* the pull back onto the orbit (strong) and along it to its place (weaker: a carried core leaves it sliding round
     * the orbit to the trailing side, and it slides home when the core stops), where it lets go, where the core catches it */
    OrbitK ok = orbit_k(rx, small);
    int dragged = g_moving == 2 ? down_index() : -1;
    static int last_fi = -1;
    /* while the main core is carried nothing draws the small ones to their places: only gravity holds them to the
     * orbit, so they slide round it as the motion takes them (and bump into each other); put down, they slide home */
    float kt = g_moving == 1 ? 0 : ok.KT;
    Orbit *ring_of[MAX_SESS];
    int rn = 0;
    for (int i = 0; i < n; i++) if (i != fi && i != dragged) { Orbit *o = orbit_of(room->s[i].sid); if (o->init && !o->free) ring_of[rn++] = o; }
    for (int i = 0, k = 0; i < n; i++) {
        Orbit *ob = orbit_of(room->s[i].sid);
        float tx, ty, tw, bob = 0;
        double a = 0;
        if (i == fi) { tx = g_mx; ty = g_my; tw = big; }
        else {
            a = M_PI / 2 + 2 * M_PI * k / (n - 1) + orbit * 0.10; /* its place: slowly around, starting at the top */
            bob = (float)(3 * g_dpi * sin(orbit * 0.8 + i * 1.7));
            tx = g_mx + rx * (float)cos(a); ty = g_my - ry * (float)sin(a) + bob; tw = small;
            k++;
        }
        if (!ob->init) { ob->px = tx; ob->py = ty; ob->vx = ob->vy = 0; ob->w = tw; ob->init = 1; }
        ob->w += (tw - ob->w) * (1 - expf(-dt * 7)); /* grows or shrinks when the focus changes */
        if (i == dragged) { /* in the hand (follow() moves it) */ }
        else if (i == fi) { /* the main core: where it is put, quickly (a core swapping in glides to the middle) */
            float f = g_moving == 1 ? 1 : 1 - expf(-dt * 12);
            ob->px += (tx - ob->px) * f; ob->py += (ty - ob->py) * f; ob->vx = ob->vy = 0; ob->free = 0;
        } else if (ob->free) { /* out of orbit: it keeps going, slowing a little, and bounces off the screen's edges
                                * (not where another monitor goes on), until the main core comes close */
            fly_step(ob, dt);
            RECT wa = work_at(ob->px, ob->py);
            float hs = ob->w / 2 + 2;
            if (ob->px < wa.left + hs && !on_work(wa.left - 1.0f, ob->py)) { ob->px = wa.left + hs; ob->vx = fabsf(ob->vx) * BOUNCE; }
            if (ob->px > wa.right - hs && !on_work((float)wa.right, ob->py)) { ob->px = wa.right - hs; ob->vx = -fabsf(ob->vx) * BOUNCE; }
            if (ob->py < wa.top + hs && !on_work(ob->px, wa.top - 1.0f)) { ob->py = wa.top + hs; ob->vy = fabsf(ob->vy) * BOUNCE; }
            if (ob->py > wa.bottom - hs && !on_work(ob->px, (float)wa.bottom)) { ob->py = wa.bottom - hs; ob->vy = -fabsf(ob->vy) * BOUNCE; }
            if (hypotf(ob->px - g_mx, ob->py - g_my) < ok.CATCH) ob->free = 0; /* caught: the spring takes it home */
        } else { /* on its orbit: a bead on a ring round the main core, held onto the ring and drawn along it to its place */
            float d = bead_step(ob, g_mx, g_my + bob, tx, ty, a, rx, ry, small, kt, &ok, ring_of, rn, dt);
            if (d > ok.BREAK) ob->free = 1; /* pulled too far: it lets go, and flies on with the speed it had */
        }
        /* its window: the core, room round it for the focus frame, drawn at its exact place (part pixels too) */
        int w4 = ((int)ob->w / 4) * 4;
        Pane *sp = &g_pane[i];
        if (!sp->w) pane_create(sp);
        if (w4 < 12 || (i != fi && !g_others)) { pane_hide(sp); SetRectEmpty(&g_core_rc[i]); continue; }
        int m = frame_margin(w4), pw = w4 + 2 * m + 1;
        float fx = ob->px - w4 / 2.0f - m, fy = ob->py - w4 / 2.0f - m;
        int ix = (int)floorf(fx), iy = (int)floorf(fy);
        int pc = pic_update(i, &room->s[i], w4, now, 1.5f);
        static float lfx[MAX_SESS], lfy[MAX_SESS];
        int shifted = fabsf((fx - ix) - lfx[i]) > .02f || fabsf((fy - iy) - lfy[i]) > .02f;
        SetRect(&g_core_rc[i], ix + m, iy + m, ix + m + w4, iy + m + w4);
        if (pc || shifted || sp->dirty || !sp->shown || sp->W != pw) {
            pane_size(sp, pw, pw);
            memset(sp->px, 0, (size_t)pw * pw * 4);
            blit_pic_at(sp, m + (fx - ix), m + (fy - iy), &g_pic[i]);
            RECT lr = {m, m, m + w4, m + w4};
            focus_frame(sp, lr, &room->s[i], now);
            pane_show(sp, ix, iy);
            lfx[i] = fx - ix; lfy[i] = fy - iy;
        } else pane_move(sp, ix, iy);
    }
    for (int i = n; i < MAX_SESS; i++) pane_hide(&g_pane[i]);
    if (fi != last_fi && fi < n && g_pane[fi].w) { /* the main core on top (under the Settings window) */
        SetWindowPos(g_pane[fi].w, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); last_fi = fi;
        if (g_set && IsWindowVisible(g_set)) SetWindowPos(g_set, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    g_top = fi;
    SetRect(&g_cluster_rc, (int)(g_mx - rx - small), (int)(g_my - ry - small), (int)(g_mx + rx + small), (int)(g_my + ry + small));
}

static void draw(double now) {
    Room *room = shown_room();
    int n = room->n > MAX_SESS ? MAX_SESS : room->n;
    if (g_single) {
        pane_hide(&g_cluster); /* (only the owner of the clock, the hotkey and the menus now) */
        draw_single(room, n, now);
    } else {
        pane_hide(&g_cluster);
        draw_all(room, n, now);
    }
    g_core_n = n;
}

/* ---- the tooltip: name and state, then the last reply (or the permission notice) --------------------- */
/* The hook keeps them in ~/.dodeca-core/msg/<sid>.json (only with DODECA_RICH=1); read again when the file
 * changes. --demo makes them up, like the macOS panel. */
static const char *DEMO_REPLY[] = {
    "Refactored the channel model so fading is applied per tap.\n\n- `channel.py`: one Rayleigh draw per tap, not per frame\n- `sim.py`: the seed covers the noise too, so runs repeat\n- `test_ber.py`: all 42 tests pass\n\nThe BER curve now matches the reference within 0.2 dB:\n\n```\nEb/N0   ref      ours\n 4 dB   1.2e-2   1.2e-2\n 8 dB   3.1e-4   3.3e-4\n12 dB   1.8e-6   1.9e-6\n```\n\n**How I checked**\n\n1. Ran the 42 tests three times with fixed seeds\n2. Swept Eb/N0 from 0 to 14 dB in 1 dB steps\n3. Compared each point with `reference.csv`\n\n**Files changed**\n\n- `channel.py` +48 -31\n- `sim.py` +12 -4\n- `test_ber.py` +20\n\nNext I can add **Doppler** if you want.",
    "The lock now debounces the reed switch for 30 ms.\nI also fixed the race where a second unlock arrived before the motor stopped.",
    "整理好今天的筆記了：三個待辦、兩個已完成。\n要不要我把待辦排進明天的計畫？",
    "Bumped the bootloader to v2.3 and rebuilt the image.\nFlash it with `make flash`; the OTA path is unchanged.",
    "Swept 2.40-2.48 GHz: best match at 2.437 GHz, S11 -18 dB.\nThe matching network values are in tune.csv.",
    "Scratch test done. Nothing to keep.",
};
static const char *DEMO_NOTICE = "Claude needs your permission to use Bash";

static void put_utf8(char **o, unsigned c) {
    char *p = *o;
    if (c < 0x80) *p++ = (char)c;
    else if (c < 0x800) { *p++ = (char)(0xC0 | c >> 6); *p++ = (char)(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { *p++ = (char)(0xE0 | c >> 12); *p++ = (char)(0x80 | (c >> 6 & 0x3F)); *p++ = (char)(0x80 | (c & 0x3F)); }
    else { *p++ = (char)(0xF0 | c >> 18); *p++ = (char)(0x80 | (c >> 12 & 0x3F)); *p++ = (char)(0x80 | (c >> 6 & 0x3F)); *p++ = (char)(0x80 | (c & 0x3F)); }
    *o = p;
}

/* the string value of "field" in a flat JSON object, as UTF-8 (malloc'd), or NULL */
static char *json_str(const char *j, const char *field) {
    char key[40];
    snprintf(key, sizeof key, "\"%s\"", field);
    const char *c = strstr(j, key);
    if (!c) return NULL;
    c += strlen(key);
    while (*c == ' ' || *c == ':') c++;
    if (*c != '"') return NULL;
    char *out = malloc(strlen(c) * 2 + 1), *o = out;
    for (c++; *c && *c != '"'; c++) {
        if (*c != '\\') { *o++ = *c; continue; }
        c++;
        switch (*c) {
        case 'n': *o++ = '\n'; break;
        case 't': *o++ = '\t'; break;
        case 'r': case 'b': case 'f': break;
        case 'u': {
            unsigned u = 0;
            if (sscanf(c + 1, "%4x", &u) != 1) goto done;
            c += 4;
            if (u >= 0xD800 && u < 0xDC00 && c[1] == '\\' && c[2] == 'u') { /* a surrogate pair */
                unsigned lo = 0;
                if (sscanf(c + 3, "%4x", &lo) == 1 && lo >= 0xDC00 && lo < 0xE000) { u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00); c += 6; }
            }
            put_utf8(&o, u);
            break;
        }
        case 0: goto done;
        default: *o++ = *c;
        }
    }
done:
    *o = 0;
    if (!out[0]) { free(out); return NULL; }
    return out;
}

static char g_msg_sid[72], *g_msg_reply, *g_msg_notice, *g_msg_tmux;
static FILETIME g_msg_mtime;
static double g_msg_checked;

static void msg_load(const Session *s, double now) {
    int same = !strcmp(g_msg_sid, s->sid);
    if (same && now - g_msg_checked < 0.5) return; /* over \\wsl.localhost: not every frame */
    g_msg_checked = now;
    if (g_demo_n) {
        if (same) return;
        free(g_msg_reply); free(g_msg_notice); free(g_msg_tmux); g_msg_reply = g_msg_notice = g_msg_tmux = NULL;
        snprintf(g_msg_sid, sizeof g_msg_sid, "%s", s->sid);
        int i;
        if (sscanf(s->sid, "demo-%d", &i) == 1 && i >= 0 && i < 6) { g_msg_reply = _strdup(DEMO_REPLY[i]); g_msg_notice = _strdup(DEMO_NOTICE); }
        return;
    }
    char path[1200];
    snprintf(path, sizeof path, "%s\\msg\\%s.json", g_dir, s->sid);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    int ok = g_dir[0] && strcmp(s->sid, "ghost") && GetFileAttributesExA(path, GetFileExInfoStandard, &fa);
    if (same && ok && !CompareFileTime(&fa.ftLastWriteTime, &g_msg_mtime)) return;
    free(g_msg_reply); free(g_msg_notice); free(g_msg_tmux); g_msg_reply = g_msg_notice = g_msg_tmux = NULL;
    snprintf(g_msg_sid, sizeof g_msg_sid, "%s", s->sid);
    memset(&g_msg_mtime, 0, sizeof g_msg_mtime);
    if (!ok) return;
    g_msg_mtime = fa.ftLastWriteTime;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char *buf = malloc(64 * 1024);
    size_t len = fread(buf, 1, 64 * 1024 - 1, f);
    fclose(f);
    buf[len] = 0;
    g_msg_reply = json_str(buf, "reply");
    g_msg_notice = json_str(buf, "notice");
    g_msg_tmux = json_str(buf, "tmux");
    free(buf);
}

/* UTF-8 → UTF-16, with a little Markdown taken out (what replies are made of): **, `, ``` lines, # heads,
 * "- " and "* " items as bullets */
static wchar_t *tip_text(const char *u8) {
    int n = MultiByteToWideChar(CP_UTF8, 0, u8, -1, NULL, 0);
    wchar_t *w = malloc(sizeof(wchar_t) * (size_t)(n + 1)), *out = malloc(sizeof(wchar_t) * (size_t)(n + 1)), *o = out;
    MultiByteToWideChar(CP_UTF8, 0, u8, -1, w, n);
    for (wchar_t *c = w; *c;) {
        wchar_t *eol = wcschr(c, L'\n'), *end = eol ? eol : c + wcslen(c);
        if (!wcsncmp(c, L"```", 3)) { c = eol ? eol + 1 : end; continue; }
        wchar_t *s = c;
        if (*s == L'#') { while (s < end && *s == L'#') s++; while (s < end && *s == L' ') s++; } /* a heading */
        while (s < end && *s == L' ') *o++ = *s++;
        if (s + 1 < end && (*s == L'-' || *s == L'*') && s[1] == L' ') { *o++ = 0x2022; s++; }
        for (; s < end; s++) {
            if (*s == L'`' || *s == L'\r') continue;
            if (*s == L'*' && s + 1 < end && s[1] == L'*') { s++; continue; }
            *o++ = *s;
        }
        if (eol) *o++ = L'\n';
        c = eol ? eol + 1 : end;
    }
    *o = 0;
    free(w);
    return out;
}

static wchar_t *g_tip_title, *g_tip_body;
static COLORREF g_tip_accent;
static HFONT g_tip_tf, g_tip_bf, g_tip_mf; /* title, body, monospaced */
static int g_tip_title_h, g_tip_body_h, g_tip_view_h, g_tip_line, g_tip_scroll;
static RECT g_tip_view;             /* where the reply is drawn in the box (window pixels), for clicks on links */
static char g_tip_sid[72];          /* whose text the box shows */

static int tip_pad(void) { return (int)(11 * g_dpi); }
static int tip_bar(void) { return (int)(3 * g_dpi); }
static int tip_gap(void) { return (int)(6 * g_dpi); }

/* the terminal's face: Cascadia Mono (it comes with Windows Terminal), else Consolas */
static const wchar_t *mono_face(void) {
    static wchar_t face[LF_FACESIZE];
    if (face[0]) return face;
    HFONT f = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, L"Cascadia Mono");
    HDC sdc = GetDC(NULL);
    HGDIOBJ old = SelectObject(sdc, f);
    GetTextFaceW(sdc, LF_FACESIZE, face);
    SelectObject(sdc, old);
    ReleaseDC(NULL, sdc);
    DeleteObject(f);
    if (wcscmp(face, L"Cascadia Mono")) wcscpy(face, L"Consolas");
    return face;
}

/* Chinese, Japanese and Korean text. The monospaced faces have no CJK glyphs, and GDI's own fallback (font
 * linking) puts in a serif face with 1-em characters, smaller and narrower than in the terminal. Windows Terminal
 * (where Claude Code runs) falls back to Microsoft JhengHei and gives each such character two cells; so does this:
 * CJK runs are drawn with that face at the same size and baseline, two cells wide when the text is monospaced. */
static int cjk_char(wchar_t c) { return (c >= 0x2E80 && c < 0xD800) || (c >= 0xF900 && c < 0xFFF0 && !(c >= 0xFF61 && c <= 0xFFDC)); }
static const wchar_t *cjk_face(void) {
    static wchar_t face[LF_FACESIZE];
    static int tried;
    if (tried) return face[0] ? face : NULL;
    tried = 1;
    static const wchar_t *TRY[] = {L"Microsoft JhengHei UI", L"Microsoft JhengHei", L"Microsoft YaHei UI", L"Yu Gothic UI", L"Malgun Gothic"};
    HDC sdc = GetDC(NULL);
    for (int i = 0; i < 5 && !face[0]; i++) {
        HFONT f = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, TRY[i]);
        HGDIOBJ old = SelectObject(sdc, f);
        wchar_t got[LF_FACESIZE];
        GetTextFaceW(sdc, LF_FACESIZE, got);
        SelectObject(sdc, old);
        DeleteObject(f);
        if (!wcscmp(got, TRY[i])) wcscpy(face, TRY[i]);
    }
    ReleaseDC(NULL, sdc);
    return face[0] ? face : NULL;
}
/* the CJK face matching font f (same height, weight, slant), made once per size */
static HFONT cjk_for(HFONT f) {
    static struct { LONG h, w; BYTE it; HFONT f; } cache[24];
    static int n;
    if (!cjk_face()) return f;
    LOGFONTW lf;
    if (!GetObjectW(f, sizeof lf, &lf)) return f;
    for (int i = 0; i < n; i++) if (cache[i].h == lf.lfHeight && cache[i].w == lf.lfWeight && cache[i].it == lf.lfItalic) return cache[i].f;
    HFONT c = CreateFontW(lf.lfHeight, 0, 0, 0, lf.lfWeight, lf.lfItalic, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, cjk_face());
    if (n < 24) { cache[n].h = lf.lfHeight; cache[n].w = lf.lfWeight; cache[n].it = lf.lfItalic; cache[n].f = c; n++; }
    return c;
}
static int font_mono(HFONT f) { LOGFONTW lf; return GetObjectW(f, sizeof lf, &lf) && (lf.lfPitchAndFamily & 3) == FIXED_PITCH; }
/* text in font f with its CJK runs in the CJK face: the width, or drawn at (x, y) when draw is set */
static int mix_text(HDC dc, HFONT f, const wchar_t *t, int n, int draw, int x, int y) {
    int w = 0, mono = font_mono(f), cell = 0;
    SIZE sz;
    if (mono) { SelectObject(dc, f); GetTextExtentPoint32W(dc, L"M", 1, &sz); cell = sz.cx; }
    for (int i = 0; i < n;) {
        int j = i, cjk = cjk_char(t[i]);
        while (j < n && cjk_char(t[j]) == cjk) j++;
        HFONT use = cjk ? cjk_for(f) : f;
        SelectObject(dc, use);
        if (cjk && mono) { /* two cells each, the glyph centred in them, as in the terminal */
            for (int k = i; k < j; k++) {
                GetTextExtentPoint32W(dc, t + k, 1, &sz);
                if (draw) TextOutW(dc, x + w + (2 * cell - sz.cx) / 2, y, t + k, 1);
                w += 2 * cell;
            }
        } else {
            GetTextExtentPoint32W(dc, t + i, j - i, &sz);
            if (draw) TextOutW(dc, x + w, y, t + i, j - i);
            w += sz.cx;
        }
        i = j;
    }
    SelectObject(dc, f);
    return w;
}

static void tip_fonts(void) {
    if (g_tip_tf) return;
    int h = -(int)(g_font * g_dpi + 0.5);
    const wchar_t *mono = mono_face(), *text = g_tip_mono ? mono : L"Segoe UI";
    DWORD pitch = g_tip_mono ? FIXED_PITCH : 0;
    g_tip_tf = CreateFontW(h, 0, 0, 0, g_tip_mono ? FW_BOLD : FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, pitch, text);
    g_tip_bf = CreateFontW(h, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, pitch, text);
    g_tip_mf = CreateFontW(g_tip_mono ? h : (int)(h * 0.92f), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, mono);
    void tip_more_fonts(int h, const wchar_t *text, DWORD pitch);
    tip_more_fonts(h, text, pitch);
}

static int input_open(void);
static void input_close(void);

/* ---- the reply as Markdown, laid out like Claude Code's terminal: a line of the reply is a line here, a
 * blank line a half-line gap. Lists ("- ", "1. ", "- [ ] ") with their wrapped lines hanging under the text;
 * # and ## headings larger, ### bold; **bold**, *italic*, ***both***, ~~struck~~, `code`, [links](url) (the
 * text, underlined); > quotes with a bar down the side; --- rules; | tables | as aligned columns (one line a
 * row when too wide); ``` blocks indented and dimmed. Long words and Chinese text wrap anywhere. Worked out
 * once per reply (tip_layout), drawn from the list of runs (tip_proc). */
enum { F_BODY, F_BOLD, F_MONO, F_ITAL, F_BOLDITAL, F_H1, F_H2, F_COUNT };
enum { DECO_UNDER = 1, DECO_STRIKE = 2 };
enum { SH_RULE = -1, SH_BAR = -2 };
typedef struct { int x, y, f, at, n, w, deco, url, url_n; COLORREF c; } Run; /* y: the baseline; a rule: x, y, w; a bar: x, y (top), n tall; url: where a link's address is in the text */
static Run *g_runs;
static int g_runs_n, g_runs_cap, g_lay_w;
static wchar_t *g_lay_text;
static int g_lay_len, g_lay_cap;
static HFONT g_tip_xf[F_COUNT];     /* italic, bold italic and the two heading sizes */
static int g_fasc[F_COUNT], g_fhgt[F_COUNT];

static HFONT lay_font(int f) { return f == F_BOLD ? g_tip_tf : f == F_MONO ? g_tip_mf : f == F_BODY ? g_tip_bf : g_tip_xf[f]; }

static const wchar_t *g_lay_url; /* while a link is being laid out: its address */
static int g_lay_url_n;

static void lay_add(Run r, const wchar_t *t) {
    if (g_runs_n == g_runs_cap) { g_runs_cap = g_runs_cap ? g_runs_cap * 2 : 256; g_runs = realloc(g_runs, sizeof(Run) * (size_t)g_runs_cap); }
    if (t && r.n > 0) {
        if (g_lay_len + r.n > g_lay_cap) { g_lay_cap = (g_lay_len + r.n) * 2 + 1024; g_lay_text = realloc(g_lay_text, sizeof(wchar_t) * (size_t)g_lay_cap); }
        memcpy(g_lay_text + g_lay_len, t, sizeof(wchar_t) * (size_t)r.n);
        r.at = g_lay_len;
        g_lay_len += r.n;
    }
    r.url = -1; r.url_n = 0;
    if (t && (r.deco & DECO_UNDER) && g_lay_url && g_lay_url_n > 0) { /* a link: its address goes along */
        if (g_lay_len + g_lay_url_n > g_lay_cap) { g_lay_cap = (g_lay_len + g_lay_url_n) * 2 + 1024; g_lay_text = realloc(g_lay_text, sizeof(wchar_t) * (size_t)g_lay_cap); }
        memcpy(g_lay_text + g_lay_len, g_lay_url, sizeof(wchar_t) * (size_t)g_lay_url_n);
        r.url = g_lay_len; r.url_n = g_lay_url_n;
        g_lay_len += g_lay_url_n;
    }
    g_runs[g_runs_n++] = r;
}
static void lay_put(int x, int base, int f, COLORREF c, const wchar_t *t, int n, int w, int deco) {
    if (n <= 0) return;
    lay_add((Run){.x = x, .y = base, .f = f, .n = n, .w = w, .deco = deco, .url = -1, .c = c}, t);
    if (x + w > g_lay_w) g_lay_w = x + w;
}
static void lay_shape(int kind, int x, int y, int len, COLORREF c) { lay_add((Run){.x = x, .y = y, .f = kind, .n = kind == SH_BAR ? len : 0, .w = kind == SH_RULE ? len : 0, .url = -1, .c = c}, NULL); }

static int text_w(HDC dc, int f, const wchar_t *t, int n) { return mix_text(dc, lay_font(f), t, n, 0, 0, 0); }

static int wide_char(wchar_t c) { return c >= 0x2E80 && !(c >= 0xFF61 && c <= 0xFFDC); } /* CJK and full-width: a line may break before any */

static const COLORREF MD_BODY = RGB(230, 232, 240), MD_BOLD = RGB(255, 255, 255), MD_CODE = RGB(173, 179, 255), MD_BLOCK = RGB(184, 189, 204),
                      MD_MARK = RGB(150, 156, 172), MD_LINK = RGB(125, 170, 255), MD_QUOTE = RGB(170, 176, 192), MD_RULE = RGB(70, 74, 86);

/* one line's words with their inline styles, wrapped at maxw from x (wrapped lines start at hang); y is the
 * top of the line, lh its height, asc its ascent. Returns the top of the last line used; *px where it ended. */
static int lay_inline(HDC dc, const wchar_t *c, const wchar_t *end, int *px, int hang, int y, int lh, int asc, int maxw,
                      int heading, COLORREF col, int code_block) {
    int x = *px, bold = 0, ital = 0, code = 0, strike = 0, link = 0, pending = 0;
    const wchar_t *link_end = NULL, *link_skip = NULL, *q = c;
    while (q < end) {
        if (!code_block) {
            if (link && q == link_end) { link = 0; g_lay_url = NULL; q = link_skip; continue; }
            if (*q == L'\\' && q + 1 < end && iswpunct(q[1])) { q++; goto word; } /* an escaped mark is just a character */
            if (!code && q + 1 < end && q[0] == L'*' && q[1] == L'*') { bold = !bold; q += 2; continue; }
            if (!code && q + 1 < end && q[0] == L'~' && q[1] == L'~') { strike = !strike; q += 2; continue; }
            if (*q == L'`') { code = !code; q++; continue; }
            if (!code && *q == L'*' && ((!ital && q + 1 < end && q[1] != L' ') || (ital && q > c && q[-1] != L' '))) { ital = !ital; q++; continue; }
            if (!code && *q == L'_' && ((!ital && (q == c || !iswalnum(q[-1])) && q + 1 < end && q[1] != L' ') || (ital && (q + 1 >= end || !iswalnum(q[1]))))) { ital = !ital; q++; continue; }
            if (!code && !link && *q == L'[') { /* [text](url): the text, underlined */
                const wchar_t *rb = q + 1;
                while (rb < end && *rb != L']') rb++;
                if (rb + 1 < end && rb[1] == L'(') {
                    const wchar_t *rp = rb + 2;
                    while (rp < end && *rp != L')') rp++;
                    if (rp < end) { link = 1; link_end = rb; link_skip = rp + 1; g_lay_url = rb + 2; g_lay_url_n = (int)(rp - (rb + 2)); q++; continue; }
                }
            }
        }
        if (*q == L' ' && !code_block) { /* spaces: only between words */
            const wchar_t *r = q;
            while (r < end && *r == L' ') r++;
            if (x > hang) pending += text_w(dc, code ? F_MONO : F_BODY, q, (int)(r - q));
            q = r;
            continue;
        }
    word:;
        int f = code || code_block ? F_MONO : heading == 1 ? F_H1 : heading == 2 ? F_H2 : (bold || heading) && ital ? F_BOLDITAL : bold || heading ? F_BOLD : ital ? F_ITAL : F_BODY;
        COLORREF cc = code_block ? MD_BLOCK : code ? MD_CODE : link ? MD_LINK : bold || heading ? MD_BOLD : col;
        const wchar_t *r = q; /* a word: up to a space or a mark, or one wide character */
        if (code_block) { while (r < end && !wide_char(*r)) r++; if (r == q) r++; } /* a code line keeps its spaces */
        else if (wide_char(*r) || *q == L'\\') r++;
        else {
            r++;
            while (r < end && *r != L' ' && !wide_char(*r) && !wcschr(L"`*_~[\\", *r) && !(link && r == link_end)) r++;
        }
        if (*q == L'\\' && !code_block) q++; /* (the backslash itself is not shown) */
        int n = (int)(r - q), w = n > 0 ? text_w(dc, f, q, n) : 0, deco = (link ? DECO_UNDER : 0) | (strike ? DECO_STRIKE : 0);
        if (x + pending + w > maxw && x > hang) { y += lh; x = hang; pending = 0; } /* wrap */
        while (w > maxw - x && n > 1) { /* still too long (a path, a URL): as much as fits, then the rest */
            int k = n - 1;
            while (k > 1 && text_w(dc, f, q, k) > maxw - x) k--;
            int kw = text_w(dc, f, q, k);
            lay_put(x + pending, y + asc, f, cc, q, k, kw, deco);
            y += lh; x = hang; pending = 0;
            q += k; n -= k; w = text_w(dc, f, q, n);
        }
        lay_put(x + pending, y + asc, f, cc, q, n, w, deco);
        x += pending + w; pending = 0;
        q = r;
    }
    g_lay_url = NULL;
    *px = x;
    return y;
}

/* A code block line with simple syntax colours, as a terminal shows it: comments, strings, numbers, keywords. */
static int lay_code(HDC dc, const wchar_t *c, const wchar_t *end, int x0, int y, int lh, int asc, int maxw) {
    static const wchar_t *KW[] = {L"if", L"else", L"for", L"while", L"return", L"function", L"const", L"let", L"var", L"def", L"class",
        L"import", L"from", L"export", L"static", L"int", L"void", L"char", L"float", L"double", L"struct", L"enum", L"typedef", L"true",
        L"false", L"null", L"None", L"True", L"False", L"new", L"async", L"await", L"fn", L"pub", L"use", L"match", L"switch", L"case",
        L"break", L"continue", L"in", L"of", L"and", L"or", L"not", L"elif", L"try", L"except", L"catch", L"finally", L"with", L"as",
        L"echo", L"then", L"fi", L"do", L"done", L"sudo", NULL};
    const COLORREF KWC = RGB(198, 140, 230), STR = RGB(152, 200, 121), NUM = RGB(214, 160, 110), COM = RGB(110, 120, 132);
    int x = x0, cw = text_w(dc, F_MONO, L" ", 1);
    const wchar_t *q = c;
    while (q < end) {
        const wchar_t *r = q + 1;
        COLORREF col = MD_BLOCK;
        if ((q[0] == L'/' && q + 1 < end && q[1] == L'/') || (q[0] == L'#' && (q == c || q[-1] == L' ')) || (q[0] == L'-' && q + 1 < end && q[1] == L'-' && q == c)) { r = end; col = COM; }
        else if (*q == L'"' || *q == L'\'' || *q == L'`') { const wchar_t qc = *q; while (r < end && *r != qc) { if (*r == L'\\') r++; r++; } if (r < end) r++; col = STR; }
        else if (iswdigit(*q)) { while (r < end && (iswalnum(*r) || *r == L'.')) r++; col = NUM; }
        else if (iswalpha(*q) || *q == L'_') {
            while (r < end && (iswalnum(*r) || *r == L'_')) r++;
            for (int k = 0; KW[k]; k++) if ((int)wcslen(KW[k]) == r - q && !wcsncmp(KW[k], q, (size_t)(r - q))) { col = KWC; break; }
        } else while (r < end && !iswalnum(*r) && !wcschr(L"\"'`#/_ ", *r)) r++;
        if (*q == L' ') { r = q; while (r < end && *r == L' ') r++; x += (int)(r - q) * cw; q = r; continue; }
        if (r > end) r = end;
        int n = (int)(r - q), w = text_w(dc, F_MONO, q, n);
        if (x + w > maxw && x > x0) { y += lh; x = x0 + 2 * cw; } /* a long line goes on, indented a little */
        lay_put(x, y + asc, F_MONO, col, q, n, w, 0);
        x += w;
        q = r;
    }
    return y;
}

/* a table cell without its marks, for measuring and drawing */
static int cell_plain(const wchar_t *a, const wchar_t *b, wchar_t *out, int cap) {
    while (a < b && *a == L' ') a++;
    while (b > a && b[-1] == L' ') b--;
    int n = 0;
    const wchar_t *a0 = a;
    for (; a < b && n < cap - 1; a++) if (*a != L'`' && !(*a == L'*' && ((a + 1 < b && a[1] == L'*') || (a > a0 && a[-1] == L'*')))) out[n++] = *a; /* drop ` and ** */
    out[n] = 0;
    return n;
}

void tip_more_fonts(int h, const wchar_t *text, DWORD pitch) {
    g_tip_xf[F_ITAL] = CreateFontW(h, 0, 0, 0, FW_NORMAL, TRUE, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, pitch, text);
    g_tip_xf[F_BOLDITAL] = CreateFontW(h, 0, 0, 0, FW_BOLD, TRUE, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, pitch, text);
    g_tip_xf[F_H1] = CreateFontW((int)(h * 1.32f), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, pitch, text);
    g_tip_xf[F_H2] = CreateFontW((int)(h * 1.14f), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, pitch, text);
}

static int tip_layout(HDC dc, const wchar_t *src, int maxw) {
    g_runs_n = 0; g_lay_len = 0; g_lay_w = 0;
    for (int f = 0; f < F_COUNT; f++) {
        TEXTMETRICW tm;
        SelectObject(dc, lay_font(f));
        GetTextMetricsW(dc, &tm);
        g_fasc[f] = tm.tmAscent; g_fhgt[f] = tm.tmHeight;
    }
    int lh = g_fhgt[F_BODY] + (int)(2 * g_dpi), asc = g_fasc[F_BODY], gap = lh / 2, y = 0, in_code = 0, blank = 1;
    int unit = text_w(dc, F_BODY, L"  ", 2) * 2; /* one level of nesting */
    /* Terminal mode looks like Claude Code: the reply opens with its mark, and every line sits two columns in */
    int lead = g_tip_mono ? text_w(dc, F_BODY, L"  ", 2) : 0, marked = 0;
    for (const wchar_t *ln = src; ln && *ln;) {
        const wchar_t *nl = wcschr(ln, L'\n'), *end = nl ? nl : ln + wcslen(ln);
        while (end > ln && (end[-1] == L'\r' || end[-1] == L' ' || end[-1] == L'\t')) end--;
        const wchar_t *c = ln;
        int sp = 0;
        while (c < end && (*c == L' ' || *c == L'\t')) { sp += *c == L'\t' ? 4 : 1; c++; }
        if (end - c >= 3 && !wcsncmp(c, L"```", 3)) { in_code = !in_code; ln = nl ? nl + 1 : NULL; continue; }
        if (c == end && !in_code) { if (!blank) y += gap; blank = 1; ln = nl ? nl + 1 : NULL; continue; }
        int x0 = lead + sp / 2 * unit;
        if (g_tip_mono && !marked) { lay_put(0, y + asc, F_BODY, MD_BOLD, L"\x25CF", 1, text_w(dc, F_BODY, L"\x25CF", 1), 0); marked = 1; } /* ●, then a space, as Claude Code writes it */
        if (in_code) {
            y = lay_code(dc, c, end, lead + unit + sp * text_w(dc, F_MONO, L" ", 1), y, lh, asc, maxw) + lh;
            blank = 0; ln = nl ? nl + 1 : NULL; continue;
        }
        /* --- *** ___ : a rule */
        { int k = 0, ok = 1; for (const wchar_t *d = c; d < end; d++) { if (*d == L'-' || *d == L'*' || *d == L'_') { if (*d != *c) ok = 0; k++; } else if (*d != L' ') ok = 0; }
          if (ok && k >= 3) { lay_shape(SH_RULE, x0, y + lh / 2, maxw - x0, MD_RULE); y += lh; blank = 0; ln = nl ? nl + 1 : NULL; continue; } }
        /* | a | b | : a table, all its rows together */
        if (*c == L'|') {
            const wchar_t *rows[64], *rend[64];
            int nr = 0;
            const wchar_t *t = c;
            for (const wchar_t *l = ln; l && *l && nr < 64;) {
                const wchar_t *n2 = wcschr(l, L'\n'), *e2 = n2 ? n2 : l + wcslen(l);
                while (e2 > l && (e2[-1] == L'\r' || e2[-1] == L' ')) e2--;
                const wchar_t *s2 = l;
                while (s2 < e2 && *s2 == L' ') s2++;
                if (s2 == e2 || *s2 != L'|') break;
                rows[nr] = s2; rend[nr++] = e2;
                l = n2 ? n2 + 1 : NULL;
                nl = n2; t = e2;
            }
            (void)t;
            /* cells, the header row, the separator row skipped */
            static wchar_t cell[64][12][160];
            int ncol = 0, cells[64], head = -1, keep[64], nk = 0;
            for (int r = 0; r < nr; r++) {
                int k = 0, sep = 1;
                const wchar_t *a = rows[r] + 1;
                while (a < rend[r] && k < 12) {
                    const wchar_t *b = a;
                    while (b < rend[r] && *b != L'|') b++;
                    int n = cell_plain(a, b, cell[r][k], 160);
                    for (int i = 0; i < n; i++) if (!wcschr(L"-: ", cell[r][k][i])) sep = 0;
                    k++;
                    a = b + 1;
                    if (b >= rend[r]) break;
                }
                if (k && !cell[r][k - 1][0] && rend[r][-1] == L'|') k--; /* the closing | */
                cells[r] = k;
                if (sep && r > 0) { if (head < 0) head = keep[nk - 1]; continue; }
                keep[nk++] = r;
                if (k > ncol) ncol = k;
            }
            int cw[12] = {0}, gapc = g_tip_mono ? text_w(dc, F_BODY, L" \x2502 ", 3) : unit, total = 0;
            for (int i = 0; i < nk; i++) { int r = keep[i]; for (int k = 0; k < cells[r]; k++) { int w = text_w(dc, r == head ? F_BOLD : F_BODY, cell[r][k], (int)wcslen(cell[r][k])); if (w > cw[k]) cw[k] = w; } }
            for (int k = 0; k < ncol; k++) total += cw[k] + (k ? gapc : 0);
            int sw = text_w(dc, F_BODY, L" ", 1), edge = g_tip_mono ? text_w(dc, F_BODY, L"\x2502 ", 2) : 0;
            if (g_tip_mono && total + 2 * edge <= maxw - x0) { /* a terminal table: box-drawing borders, padded to the columns */
                const COLORREF BORDER = RGB(110, 116, 130);
                int yy = y;
                #define BOXLINE(L, M, R) do { int bx = x0; wchar_t seg[256]; \
                    for (int k = 0; k < ncol; k++) { int nch = (cw[k] + sw - 1) / sw + 2, m2 = 0; seg[m2++] = k ? M : L; for (int z = 0; z < nch && m2 < 250; z++) seg[m2++] = 0x2500; \
                        lay_put(bx, yy + asc, F_BODY, BORDER, seg, m2, text_w(dc, F_BODY, seg, m2), 0); bx += text_w(dc, F_BODY, seg, m2); } \
                    wchar_t endc = R; lay_put(bx, yy + asc, F_BODY, BORDER, &endc, 1, text_w(dc, F_BODY, &endc, 1), 0); yy += lh; } while (0)
                BOXLINE(0x250C, 0x252C, 0x2510);
                for (int i = 0; i < nk; i++) {
                    int r = keep[i], bx = x0;
                    for (int k = 0; k < ncol; k++) {
                        int nch = (cw[k] + sw - 1) / sw + 2;
                        wchar_t bar = 0x2502;
                        lay_put(bx, yy + asc, F_BODY, BORDER, &bar, 1, text_w(dc, F_BODY, &bar, 1), 0);
                        int cx = bx + text_w(dc, F_BODY, &bar, 1) + sw, f = r == head ? F_BOLD : F_BODY;
                        if (k < cells[r]) { int n = (int)wcslen(cell[r][k]); lay_put(cx, yy + asc, f, r == head ? MD_BOLD : MD_BODY, cell[r][k], n, text_w(dc, f, cell[r][k], n), 0); }
                        bx += text_w(dc, F_BODY, &bar, 1) + nch * sw;
                    }
                    wchar_t bar = 0x2502;
                    lay_put(bx, yy + asc, F_BODY, BORDER, &bar, 1, text_w(dc, F_BODY, &bar, 1), 0);
                    yy += lh;
                    if (r == head) BOXLINE(0x251C, 0x253C, 0x2524);
                }
                BOXLINE(0x2514, 0x2534, 0x2518);
                #undef BOXLINE
                y = yy;
                blank = 0;
                ln = nl ? nl + 1 : NULL;
                continue;
            }
            for (int i = 0; i < nk; i++) {
                int r = keep[i], x = x0;
                if (total <= maxw - x0) { /* aligned columns */
                    for (int k = 0; k < cells[r]; k++) {
                        int f = r == head ? F_BOLD : F_BODY, n = (int)wcslen(cell[r][k]);
                        lay_put(x, y + asc, f, r == head ? MD_BOLD : MD_BODY, cell[r][k], n, text_w(dc, f, cell[r][k], n), 0);
                        x += cw[k] + gapc;
                    }
                    y += lh;
                    if (r == head) { lay_shape(SH_RULE, x0, y - (int)(3 * g_dpi), total, MD_RULE); y += (int)(2 * g_dpi); }
                } else { /* too wide: a row on its own lines, cells apart by a dot */
                    wchar_t row[2000] = L"";
                    for (int k = 0; k < cells[r]; k++) { if (k) wcscat(row, L"  \x00B7  "); wcsncat(row, cell[r][k], 1900 - wcslen(row)); }
                    int xx = x0;
                    y = lay_inline(dc, row, row + wcslen(row), &xx, x0 + unit, y, lh, asc, maxw, r == head ? 3 : 0, MD_BODY, 0) + lh;
                }
            }
            blank = 0;
            ln = nl ? nl + 1 : NULL;
            continue;
        }
        /* > a quote: indented, dimmed, a bar down the side */
        int quote = 0;
        while (c < end && *c == L'>') { quote++; c++; while (c < end && *c == L' ') c++; }
        if (quote) x0 += quote * unit;
        int heading = 0, hlh = lh, hasc = asc;
        COLORREF col = quote ? MD_QUOTE : MD_BODY;
        if (*c == L'#') {
            const wchar_t *d = c;
            while (d < end && *d == L'#') d++;
            if (d < end && *d == L' ') {
                heading = (int)(d - c);
                c = d; while (c < end && *c == L' ') c++;
                if (g_tip_mono) heading = 3; /* a terminal has one size: headings are bold */
                int f = heading == 1 ? F_H1 : heading == 2 ? F_H2 : F_BOLD;
                hlh = g_fhgt[f] + (int)(4 * g_dpi); hasc = g_fasc[f] + (int)(2 * g_dpi);
                if (y > 0 && !blank) y += gap / 2; /* a little air above a heading */
            }
        }
        int hang = x0, x = x0, top = y;
        if (!heading) { /* a list item: its marker, and the text (and its wrapped lines) after it */
            wchar_t mark[12] = L"";
            const wchar_t *d = c;
            while (d < end && iswdigit(*d) && d - c < 4) d++;
            if (end - c >= 2 && (*c == L'-' || *c == L'*' || *c == L'+' || *c == 0x2022) && c[1] == L' ') {
                mark[0] = g_tip_mono ? *c : 0x2022; mark[1] = 0; c += 2;
                if (!g_tip_mono && end - c >= 3 && c[0] == L'[' && c[2] == L']' && (c[1] == L' ' || c[1] == L'x' || c[1] == L'X')) { /* - [ ] / - [x] */
                    mark[0] = c[1] == L' ' ? 0x2610 : 0x2611; c += 3; while (c < end && *c == L' ') c++;
                }
            }
            else if (d > c && d + 1 < end && (*d == L'.' || *d == L')') && d[1] == L' ') { swprintf(mark, 12, L"%.*ls.", (int)(d - c), c); c = d + 2; }
            if (mark[0]) {
                int mn = (int)wcslen(mark), mw = text_w(dc, F_BODY, mark, mn);
                lay_put(x0, y + asc, F_BODY, MD_MARK, mark, mn, mw, 0);
                mw += text_w(dc, F_BODY, L" ", 1);
                hang = x = x0 + mw; /* the text right after the marker and one space, as Claude Code writes it */
            }
        }
        y = lay_inline(dc, c, end, &x, hang, y, heading ? hlh : lh, heading ? hasc : asc, maxw, heading, col, 0) + (heading ? hlh : lh);
        if (quote) lay_shape(SH_BAR, x0 - unit + (int)(4 * g_dpi), top + (int)(2 * g_dpi), y - top - (int)(4 * g_dpi), MD_RULE);
        if (heading) y += gap / 3;
        blank = 0;
        ln = nl ? nl + 1 : NULL;
    }
    return y > 0 ? y : lh;
}

static void tip_hide(void) {
    if (input_open()) return; /* a reply being typed keeps its box */
    if (g_tip && IsWindowVisible(g_tip)) ShowWindow(g_tip, SW_HIDE);
    g_tip_sid[0] = 0;
}

/* a new text size: the fonts are made again and the box measures itself again */
static void tip_refont(void) {
    if (g_tip_tf) { DeleteObject(g_tip_tf); DeleteObject(g_tip_bf); DeleteObject(g_tip_mf); g_tip_tf = g_tip_bf = g_tip_mf = NULL; }
    for (int i = 0; i < F_COUNT; i++) if (g_tip_xf[i]) { DeleteObject(g_tip_xf[i]); g_tip_xf[i] = NULL; }
    if (g_label_font) { DeleteObject(g_label_font); g_label_font = NULL; }
    free(g_tip_title); g_tip_title = NULL;
    input_close();
    void input_refont(void);
    input_refont();
    tip_hide();
}

/* the wheel over a core (or over the box while a reply is typed) scrolls a long reply, 3 lines a notch */
static void tip_scroll(float notches) {
    if (!g_tip || !IsWindowVisible(g_tip) || g_tip_body_h <= g_tip_view_h) return;
    int to = g_tip_scroll - (int)(notches * 3 * g_tip_line);
    if (to < 0) to = 0;
    if (to > g_tip_body_h - g_tip_view_h) to = g_tip_body_h - g_tip_view_h;
    if (to != g_tip_scroll) { g_tip_scroll = to; InvalidateRect(g_tip, NULL, FALSE); }
}

static LRESULT CALLBACK tip_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT || msg == WM_PRINTCLIENT) { /* drawn off-screen first, so scrolling does not flicker (WM_PRINTCLIENT: --record) */
        PAINTSTRUCT ps;
        HDC wdc = msg == WM_PAINT ? BeginPaint(w, &ps) : (HDC)wp;
        RECT r; GetClientRect(w, &r);
        HDC dc = CreateCompatibleDC(wdc);
        HBITMAP bm = CreateCompatibleBitmap(wdc, r.right, r.bottom), old = SelectObject(dc, bm);
        HBRUSH bg = CreateSolidBrush(tip_bg()), bar = CreateSolidBrush(g_tip_accent);
        FillRect(dc, &r, bg);
        RECT b = {0, 0, tip_bar(), r.bottom};
        FillRect(dc, &b, bar);
        DeleteObject(bg); DeleteObject(bar);
        SetBkMode(dc, TRANSPARENT);
        int pad = tip_pad();
        RECT t = {tip_bar() + pad, pad, r.right - pad, pad + g_tip_title_h};
        int ar = GetRValue(g_tip_accent), ag = GetGValue(g_tip_accent), ab = GetBValue(g_tip_accent);
        SetTextColor(dc, ink(RGB(ar + (255 - ar) * 3 / 10, ag + (255 - ag) * 3 / 10, ab + (255 - ab) * 3 / 10))); /* lifted toward white */
        SelectObject(dc, g_tip_tf);
        if (g_tip_title) DrawTextW(dc, g_tip_title, -1, &t, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        int top = t.bottom + tip_gap(), sb = g_tip_body_h > g_tip_view_h ? (int)(8 * g_dpi) : 0;
        RECT view = {t.left, top, r.right - pad - sb, top + g_tip_view_h};
        RECT body = {view.left, top - g_tip_scroll, view.right, top - g_tip_scroll + g_tip_body_h};
        g_tip_view = view;
        SetTextColor(dc, text_body());
        SelectObject(dc, g_tip_bf);
        IntersectClipRect(dc, view.left, view.top, view.right, view.bottom);
        SetTextAlign(dc, TA_BASELINE | TA_LEFT); /* every face on the same baseline */
        for (int i = 0; i < g_runs_n; i++) {
            const Run *rn = &g_runs[i];
            int by = body.top + rn->y, x = view.left + rn->x, th = (int)fmax(1, g_dpi);
            if (by + 3 * g_tip_line < view.top || by - 3 * g_tip_line > view.bottom) continue;
            if (rn->f == SH_RULE || rn->f == SH_BAR) { /* a rule, or a quote's bar */
                RECT sr = rn->f == SH_RULE ? (RECT){x, by, x + rn->w, by + th} : (RECT){x, by, x + (int)(3 * g_dpi), by + rn->n};
                HBRUSH br = CreateSolidBrush(ink(rn->c));
                FillRect(dc, &sr, br);
                DeleteObject(br);
                continue;
            }
            SetTextColor(dc, rn->c == MD_BODY || rn->c == MD_BOLD ? text_body() : ink(rn->c)); /* the text in the chosen colour; code, links and quotes keep theirs */
            mix_text(dc, lay_font(rn->f), g_lay_text + rn->at, rn->n, 1, x, by);
            if (rn->deco) { /* a link's underline, a strike through */
                HBRUSH br = CreateSolidBrush(ink(rn->c));
                if (rn->deco & DECO_UNDER) { RECT ur = {x, by + 2 * th, x + rn->w, by + 3 * th}; FillRect(dc, &ur, br); }
                if (rn->deco & DECO_STRIKE) { int sy = by - g_fasc[rn->f] * 3 / 10; RECT sr = {x, sy, x + rn->w, sy + th}; FillRect(dc, &sr, br); }
                DeleteObject(br);
            }
        }
        SetTextAlign(dc, TA_TOP | TA_LEFT);
        SelectClipRgn(dc, NULL);
        if (sb) { /* where in the reply the box is */
            int th = g_tip_view_h * g_tip_view_h / g_tip_body_h, ty = top + (g_tip_view_h - th) * g_tip_scroll / (g_tip_body_h - g_tip_view_h);
            RECT track = {r.right - pad - (int)(3 * g_dpi), top, r.right - pad, top + g_tip_view_h}, thumb = {track.left, ty, track.right, ty + th};
            HBRUSH tb = CreateSolidBrush(RGB(48, 50, 58)), hb = CreateSolidBrush(RGB(140, 145, 160));
            FillRect(dc, &track, tb); FillRect(dc, &thumb, hb);
            DeleteObject(tb); DeleteObject(hb);
        }
        BitBlt(wdc, 0, 0, r.right, r.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, old); DeleteObject(bm); DeleteDC(dc);
        if (msg == WM_PAINT) EndPaint(w, &ps);
        return 0;
    }
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_MOUSEWHEEL) { tip_scroll(GET_WHEEL_DELTA_WPARAM(wp) / 120.0f); return 0; }
    if (msg == WM_SETCURSOR || msg == WM_LBUTTONUP) { /* a link under the pointer: the hand, and a click opens it */
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(w, &pt);
        int found = -1;
        if (PtInRect(&g_tip_view, pt))
            for (int i = 0; i < g_runs_n && found < 0; i++) {
                const Run *rn = &g_runs[i];
                if (rn->f < 0 || rn->url < 0) continue;
                int bx = g_tip_view.left + rn->x, by = g_tip_view.top - g_tip_scroll + rn->y;
                if (pt.x >= bx && pt.x < bx + rn->w && pt.y >= by - g_fasc[rn->f] && pt.y < by + g_tip_line / 3) found = i;
            }
        if (msg == WM_SETCURSOR) { if (found >= 0) { SetCursor(LoadCursor(NULL, IDC_HAND)); return TRUE; } }
        else if (found >= 0) {
            wchar_t url[2048];
            int n = g_runs[found].url_n < 2047 ? g_runs[found].url_n : 2047;
            wcsncpy(url, g_lay_text + g_runs[found].url, (size_t)n); url[n] = 0;
            if (!_wcsnicmp(url, L"https://", 8) || !_wcsnicmp(url, L"http://", 7)) ShellExecuteW(NULL, L"open", url, NULL, NULL, SW_SHOWNORMAL); /* web links only */
            return 0;
        }
    }
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProc(w, msg, wp, lp);
}

static void input_place(void);
static int input_h(void);

/* The box: next to `beside` (screen pixels: the cluster, or the core), level with the core at height cy,
 * left of it when it fits, else right; or in the middle of the screen (Settings > Text box). It wraps,
 * and a reply longer than the box scrolls (the wheel over the core). */
static void tip_show(const Session *s, RECT beside, int cy, double now) {
    msg_load(s, now);
    int st = session_shown(s, now);
    char title[200];
    const char *name = s->name[0] ? s->name : "claude";
    char line[80];
    state_line(s, now, line, sizeof line);
    if (!strcmp(s->sid, "ghost")) snprintf(title, sizeof title, "%s", line);
    else snprintf(title, sizeof title, "%s \xC2\xB7 %s", name, line);
    if (s->unread) strncat(title, " \xC2\xB7 new reply", sizeof title - strlen(title) - 1);
    if (g_single && g_focus_pinned && !g_rec_scene[0] && !strcmp(g_focus, s->sid)) strncat(title, " \xC2\xB7 pinned", sizeof title - strlen(title) - 1);
    const char *body = (st == ST_WAIT && g_msg_notice) ? g_msg_notice : g_msg_reply ? g_msg_reply : strcmp(s->sid, "ghost") ? "(no reply yet)" : "Start a session in WSL; it shows up here.";
    wchar_t *wt = tip_text(title), *wb;
    { int n = MultiByteToWideChar(CP_UTF8, 0, body, -1, NULL, 0); wb = malloc(sizeof(wchar_t) * (size_t)n); MultiByteToWideChar(CP_UTF8, 0, body, -1, wb, n); } /* as written: laid out below */
    Col c = s->aura_a > 0.01f ? s->aura : s->cur.eh; /* single core: the session's own colour, else its state colour */
    COLORREF acc = RGB((int)(fminf(1, c.r) * 255), (int)(fminf(1, c.g) * 255), (int)(fminf(1, c.b) * 255));
    static RECT last_beside;
    static int last_middle = -1, last_input = -1;
    int other = strcmp(g_tip_sid, s->sid) != 0, body_new = !g_tip_body || wcscmp(wb, g_tip_body);
    int moved = memcmp(&beside, &last_beside, sizeof beside) != 0 || last_middle != g_tip_middle || last_input != input_open();
    last_input = input_open();
    int changed = !g_tip_title || wcscmp(wt, g_tip_title) || body_new || !IsWindowVisible(g_tip) || moved || other;
    if (other || body_new) g_tip_scroll = 0; /* another reply starts at its top */
    snprintf(g_tip_sid, sizeof g_tip_sid, "%s", s->sid);
    last_beside = beside;
    last_middle = g_tip_middle;
    free(g_tip_title); free(g_tip_body);
    g_tip_title = wt; g_tip_body = wb;
    if (acc != g_tip_accent) { g_tip_accent = acc; InvalidateRect(g_tip, NULL, FALSE); }
    if (!changed) return;
    tip_fonts();
    RECT wa = work_at((beside.left + beside.right) / 2.0f, (float)cy); /* the monitor the core is on */
    int ww = wa.right - wa.left, wh = wa.bottom - wa.top;
    int maxw = (int)(fmax(460, g_font * 36) * g_dpi * g_tip_size), pad = tip_pad(); /* Settings > Box size */
    if (g_tip_middle) maxw = (int)fmin(fmax(maxw, ww * 0.45 * g_tip_size), 1000 * g_dpi * g_tip_size);
    if (maxw > ww - 2 * pad) maxw = ww - 2 * pad;
    HDC dc = GetDC(g_tip);
    SelectObject(dc, g_tip_tf);
    RECT t = {0, 0, maxw, 0};
    DrawTextW(dc, wt, -1, &t, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    g_tip_title_h = t.bottom;
    SelectObject(dc, g_tip_bf);
    TEXTMETRICW tm; GetTextMetricsW(dc, &tm);
    g_tip_line = tm.tmHeight;
    RECT b = {0, 0, maxw, 0};
    g_tip_body_h = tip_layout(dc, wb, maxw);
    b.right = g_lay_w;
    ReleaseDC(g_tip, dc);
    int most = g_tip_middle ? (int)(wh * fmin(0.85, 0.6 * g_tip_size)) : (int)fmin(g_tip_line * 18 * g_tip_size, wh * 0.85); /* longer: it scrolls */
    /* a reply being typed sits under the box: both have to fit on the screen together */
    int below = input_open() ? input_h() + (int)(6 * g_dpi) : 0, chrome = pad + g_tip_title_h + tip_gap() + pad;
    if (most > wh - below - chrome - (int)(16 * g_dpi)) most = wh - below - chrome - (int)(16 * g_dpi);
    if (most < g_tip_line * 3) most = g_tip_line * 3;
    g_tip_view_h = g_tip_body_h < most ? g_tip_body_h : most;
    if (g_tip_scroll > g_tip_body_h - g_tip_view_h) g_tip_scroll = g_tip_body_h > g_tip_view_h ? g_tip_body_h - g_tip_view_h : 0;
    int sb = g_tip_body_h > g_tip_view_h ? (int)(8 * g_dpi) : 0;
    int w = (t.right > b.right ? t.right : b.right), h = pad + g_tip_title_h + tip_gap() + g_tip_view_h + pad;
    if (w > maxw) w = maxw;
    w += tip_bar() + 2 * pad + sb;
    int x, y, gap = (int)(8 * g_dpi);
    if (g_tip_middle) { x = wa.left + (ww - w) / 2; y = wa.top + (wh - h - below) / 2; } /* the box and the reply line, centred together */
    else {
        x = beside.left - w - gap; y = cy - h / 2;
        if (x < wa.left) x = beside.right + gap;                /* no room on the left: on the right */
    }
    if (x + w > wa.right) x = wa.right - w;
    if (x < wa.left) x = wa.left;
    if (y + h + below > wa.bottom) y = wa.bottom - h - below;
    if (y < wa.top) y = wa.top;
    SetWindowPos(g_tip, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    int rad = (int)(12 * g_dpi);
    SetWindowRgn(g_tip, CreateRoundRectRgn(0, 0, w + 1, h + 1, rad, rad), TRUE);
    InvalidateRect(g_tip, NULL, FALSE);
    if (input_open()) input_place();
}

/* ---- typing a reply: a double-click on a core opens a line under its box. Enter sends it to the session
 * (Shift+Enter: a new line), Esc or clicking elsewhere closes it and keeps what was typed for next time.
 * It goes into the session's tmux pane when it runs in one (the hook notes the pane), else the terminal
 * window comes to the front and the text is typed into it. ------------------------------------------- */
static HWND g_input, g_edit;
static WNDPROC g_edit_proc;
static HBRUSH g_input_bg;
static HFONT g_input_font;
static char g_input_sid[72];
static wchar_t *g_draft;            /* what was typed for g_draft_sid, kept when the line closes */
static char g_draft_sid[72];
static wchar_t *g_keys;             /* the fallback: text waiting to be typed into the terminal */
static HFONT g_input_tf, g_input_sf;  /* the reply box's title and hint line */
/* Images dropped on a core or on the reply box, as Claude Code shows them: each one puts an "[Image #N]" token
 * where the cursor is, which can be moved or deleted like any text (Backspace takes a whole token); on send each
 * token becomes the image's WSL path in its place, and an image whose token was deleted is not sent. */
#define MAX_ATTACH 8
static wchar_t g_attach[MAX_ATTACH][600]; /* as WSL paths */
static wchar_t g_attach_name[MAX_ATTACH][120];
static int g_attach_id[MAX_ATTACH];       /* the N of its token */
static int g_attach_n, g_attach_next;     /* g_attach_next: the last N given out for this reply */

static int input_open(void) { return g_input && IsWindowVisible(g_input); }

static void input_close(void) {
    if (!input_open()) return;
    int n = GetWindowTextLengthW(g_edit);
    free(g_draft);
    g_draft = malloc(sizeof(wchar_t) * (size_t)(n + 1));
    GetWindowTextW(g_edit, g_draft, n + 1);
    snprintf(g_draft_sid, sizeof g_draft_sid, "%s", g_input_sid);
    ShowWindow(g_input, SW_HIDE);
    LONG_PTR ex = GetWindowLongPtr(g_tip, GWL_EXSTYLE); /* the box lets clicks through again */
    SetWindowLongPtr(g_tip, GWL_EXSTYLE, ex | WS_EX_TRANSPARENT);
    g_input_sid[0] = 0;
}

/* the reply box: a title line, a rounded field of three lines, a hint line */
static int in_pad(void) { return (int)(12 * g_dpi); }
static int in_line(void) { return (int)(g_font * g_dpi * 1.45f); }
static int in_title_h(void) { return in_line(); }
static int in_field_h(void) { return in_line() * 3 + (int)(12 * g_dpi); }
static int in_hint_h(void) { return (int)(g_font * 0.86f * g_dpi * 1.45f); }

static int input_h(void) { int gap = (int)(6 * g_dpi); return in_pad() + in_title_h() + gap + in_field_h() + gap + in_hint_h() + in_pad(); }

/* under the text box, as wide as it */
static void input_place(void) {
    RECT tr;
    GetWindowRect(g_tip, &tr);
    int gap = (int)(6 * g_dpi), h = input_h(), y = tr.bottom + gap; /* the box made room for it */
    RECT wa = work_at((tr.left + tr.right) / 2.0f, (tr.top + tr.bottom) / 2.0f); /* the text box's monitor */
    if (y + h > wa.bottom) y = wa.bottom - h;
    SetWindowPos(g_input, HWND_TOPMOST, tr.left, y, tr.right - tr.left, h, SWP_NOACTIVATE);
    int rad = (int)(12 * g_dpi);
    SetWindowRgn(g_input, CreateRoundRectRgn(0, 0, tr.right - tr.left + 1, h + 1, rad, rad), TRUE);
}

/* "\\wsl.localhost\Ubuntu\home\..." → "Ubuntu"; 0 if the events file is not in WSL */
static int wsl_distro(char *out, size_t n) {
    const char *c = g_dir;
    if (!strncmp(c, "\\\\wsl.localhost\\", 16)) c += 16;
    else if (!strncmp(c, "\\\\wsl$\\", 7)) c += 7;
    else return 0;
    size_t i = 0;
    while (c[i] && c[i] != '\\' && i + 1 < n) { if (!isalnum((unsigned char)c[i]) && !strchr("._-", c[i])) return 0; out[i] = c[i]; i++; }
    out[i] = 0;
    return i > 0;
}

/* Into the tmux pane, as a bracketed paste (several lines stay one message) and then Enter on its own, which
 * Claude Code wants apart from the text. The text goes in on stdin, never on a command line. */
static int send_tmux(const char *pane, const wchar_t *text) {
    char distro[64];
    if (!pane || !wsl_distro(distro, sizeof distro) || pane[0] != '%' || !pane[1] || strspn(pane + 1, "0123456789") != strlen(pane + 1)) return 0;
    wchar_t cmd[400];
    swprintf(cmd, 400, L"wsl.exe -d %hs -e sh -c \"tmux load-buffer -b vigil - && tmux paste-buffer -p -d -b vigil -t $0 && sleep 0.2 && tmux send-keys -t $0 Enter\" %hs",
             distro, pane);
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return 0;
    SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si; si.dwFlags = STARTF_USESTDHANDLES; si.hStdInput = rd;
    si.hStdOutput = si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(rd);
    if (ok) {
        int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
        char *u8 = malloc((size_t)n);
        WideCharToMultiByte(CP_UTF8, 0, text, -1, u8, n, NULL, NULL);
        DWORD put;
        WriteFile(wr, u8, (DWORD)(n - 1), &put, NULL);
        free(u8);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
    CloseHandle(wr);
    return ok;
}

/* "[Image #N]" at c: the attachment it stands for (or -1), its N, and how many characters it takes */
static int token_at(const wchar_t *c, int *id, int *used) {
    int n = 0;
    if (wcsncmp(c, L"[Image #", 8)) return -1;
    if (swscanf(c + 8, L"%d]%n", id, &n) != 1 || n < 2 || c[8 + n - 1] != L']') return -1;
    *used = 8 + n;
    for (int k = 0; k < g_attach_n; k++) if (g_attach_id[k] == *id) return k;
    return -1;
}
/* is attachment k still in the text (its token not deleted)? */
static int attach_used(const wchar_t *text, int k) {
    wchar_t tok[32];
    swprintf(tok, 32, L"[Image #%d]", g_attach_id[k]);
    return wcsstr(text, tok) != NULL;
}
/* the text as the selection counts it (a line break is one \r) */
static wchar_t *edit_text(void) {
    GETTEXTLENGTHEX gl = {GTL_NUMCHARS | GTL_PRECISE, 1200};
    int n = (int)SendMessageW(g_edit, EM_GETTEXTLENGTHEX, (WPARAM)&gl, 0);
    wchar_t *t = malloc(sizeof(wchar_t) * (size_t)(n + 1));
    GETTEXTEX gt = {(DWORD)(sizeof(wchar_t) * (size_t)(n + 1)), GT_DEFAULT, 1200, NULL, NULL};
    LRESULT got = SendMessageW(g_edit, EM_GETTEXTEX, (WPARAM)&gt, (LPARAM)t);
    t[got >= 0 && got <= n ? got : 0] = 0;
    return t;
}
/* the reply box's look: the CLI's face and colour, CJK runs in the CJK face (set again after each change) */
static int g_styling;
static void edit_style(void) {
    if (!g_edit || g_styling) return;
    g_styling = 1;
    CHARFORMAT2W cf;
    memset(&cf, 0, sizeof cf);
    cf.cbSize = sizeof cf;
    cf.dwMask = CFM_COLOR | CFM_FACE | CFM_CHARSET;
    cf.crTextColor = text_body();
    cf.bCharSet = DEFAULT_CHARSET;
    wcsncpy(cf.szFaceName, mono_face(), LF_FACESIZE - 1);
    SendMessageW(g_edit, EM_SETCHARFORMAT, SCF_DEFAULT, (LPARAM)&cf);
    const wchar_t *cj = cjk_face();
    wchar_t *t = edit_text();
    int n = (int)wcslen(t), any = 0;
    for (int i = 0; i < n && !any; i++) any = cjk_char(t[i]);
    if (any && cj) {
        DWORD a = 0, b = 0;
        SendMessageW(g_edit, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
        SendMessageW(g_edit, WM_SETREDRAW, FALSE, 0);
        for (int i = 0; i < n;) {
            int j = i, cjk = cjk_char(t[i]);
            while (j < n && cjk_char(t[j]) == cjk) j++;
            wcsncpy(cf.szFaceName, cjk ? cj : mono_face(), LF_FACESIZE - 1);
            cf.bCharSet = cjk ? CHINESEBIG5_CHARSET : DEFAULT_CHARSET;
            SendMessageW(g_edit, EM_SETSEL, i, j);
            SendMessageW(g_edit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
            i = j;
        }
        SendMessageW(g_edit, EM_SETSEL, a, b);
        SendMessageW(g_edit, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(g_edit, NULL, FALSE);
    }
    free(t);
    g_styling = 0;
}
/* the tokens for attachments first..g_attach_n-1, typed in at the cursor */
static void insert_tokens(int first) {
    if (!g_edit || first >= g_attach_n) return;
    DWORD a = 0, b = 0;
    SendMessageW(g_edit, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
    wchar_t *t = edit_text(), ins[400] = L"";
    if (a > 0 && t[a - 1] != L' ' && t[a - 1] != L'\n') wcscat(ins, L" ");
    for (int k = first; k < g_attach_n; k++) { wchar_t tok[32]; swprintf(tok, 32, L"[Image #%d] ", g_attach_id[k]); wcscat(ins, tok); }
    free(t);
    SendMessageW(g_edit, EM_REPLACESEL, TRUE, (LPARAM)ins);
    InvalidateRect(g_input, NULL, FALSE);
}
/* Backspace just after a token, or Delete just before one: the whole token goes */
static int token_key(HWND w, int back) {
    DWORD a = 0, b = 0;
    SendMessageW(w, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
    if (a != b) return 0;
    wchar_t *t = edit_text();
    int len = (int)wcslen(t), done = 0;
    for (int st = (int)a - 20 < 0 ? 0 : (int)a - 20; st <= (int)a && st < len && !done; st++) {
        int id, used = 0;
        if (wcsncmp(t + st, L"[Image #", 8) || swscanf(t + st + 8, L"%d]%n", &id, &used) != 1) continue;
        int end = st + 8 + used;
        if ((back && end == (int)a) || (!back && st == (int)a)) {
            if (end < len && t[end] == L' ') end++; /* and the space after it */
            SendMessageW(w, EM_SETSEL, st, end);
            SendMessageW(w, EM_REPLACESEL, TRUE, (LPARAM)L"");
            done = 1;
        }
    }
    free(t);
    if (done) InvalidateRect(g_input, NULL, FALSE);
    return done;
}

static void bring_to_front(const Session *s);

static void send_reply(void) {
    int i = index_of(g_input_sid);
    wchar_t *text = edit_text();
    int n = (int)wcslen(text);
    for (int k = 0; k < n; k++) if (text[k] == L'\r') text[k] = L'\n'; /* the rich edit's line breaks */
    while (n > 0 && (text[n - 1] == L' ' || text[n - 1] == L'\n' || text[n - 1] == L'\r')) text[--n] = 0; /* no trailing blank line */
    SetWindowTextW(g_edit, L"");
    input_close();
    free(g_draft); g_draft = NULL;
    if (i < 0 || !n) { free(text); return; }
    if (g_attach_n) { /* each "[Image #N]" becomes its image's path, where it stands */
        size_t len = wcslen(text) + 1;
        for (int k = 0; k < g_attach_n; k++) len += wcslen(g_attach[k]) * 4;
        wchar_t *all = malloc(sizeof(wchar_t) * (len + 1)), *o = all;
        for (const wchar_t *c = text; *c;) {
            int id, used = 0, k = token_at(c, &id, &used);
            if (k >= 0) { wcscpy(o, g_attach[k]); o += wcslen(o); c += used; }
            else *o++ = *c++;
        }
        *o = 0;
        free(text);
        text = all;
        g_attach_n = g_attach_next = 0;
    }
    Session *s = &shown_room()->s[i];
    s->unread = 0;
    msg_load(s, wall() + 1);
    if (send_tmux(g_msg_tmux, text)) { free(text); return; }
    /* no tmux: its terminal to the front, then typed in (several lines become one, Enter would send early) */
    for (wchar_t *c = text; *c; c++) if (*c == L'\r' || *c == L'\n') *c = L' ';
    free(g_keys);
    g_keys = text;
    bring_to_front(s);
    SetTimer(g_cluster.w, ID_SENDKEYS, 250, NULL);
}

static void type_keys(void) { /* the fallback, once the terminal has come to the front */
    if (!g_keys) return;
    size_t n = wcslen(g_keys);
    INPUT *in = calloc(n * 2, sizeof(INPUT));
    for (size_t i = 0; i < n; i++) {
        in[2 * i].type = in[2 * i + 1].type = INPUT_KEYBOARD;
        in[2 * i].ki.wScan = in[2 * i + 1].ki.wScan = g_keys[i];
        in[2 * i].ki.dwFlags = KEYEVENTF_UNICODE;
        in[2 * i + 1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
    }
    SendInput((UINT)(n * 2), in, sizeof(INPUT));
    free(in);
    free(g_keys); g_keys = NULL;
    SetTimer(g_cluster.w, ID_SENDENTER, 150, NULL); /* Return on its own, a moment later */
}

static void type_enter(void) {
    INPUT in[2];
    memset(in, 0, sizeof in);
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[1].ki.wVk = VK_RETURN;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof in[0]);
}

static LRESULT CALLBACK edit_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN && wp == VK_RETURN && !(GetKeyState(VK_SHIFT) & 0x8000)) { send_reply(); return 0; }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) { input_close(); return 0; }
    if (msg == WM_PASTE) { SendMessageW(w, EM_PASTESPECIAL, CF_UNICODETEXT, 0); return 0; } /* text only, styled like the rest */
    if (msg == WM_KEYDOWN && (wp == VK_BACK || wp == VK_DELETE) && g_attach_n && token_key(w, wp == VK_BACK)) return 0; /* a whole image token at once */
    if (msg == WM_CHAR && (wp == L'\r' || wp == 27) && !(GetKeyState(VK_SHIFT) & 0x8000)) return 0;
    if (msg == WM_MOUSEWHEEL) { tip_scroll(GET_WHEEL_DELTA_WPARAM(wp) / 120.0f); return 0; } /* the wheel scrolls the reply */
    return CallWindowProcW(g_edit_proc, w, msg, wp, lp);
}

static void add_drop(HDROP d, int finish);

static LRESULT CALLBACK input_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { /* like the text box: dark card, the session's colour down the left edge, a rounded field */
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        RECT r; GetClientRect(w, &r);
        HBRUSH bg = CreateSolidBrush(tip_bg()), bar = CreateSolidBrush(g_tip_accent), field = CreateSolidBrush(tip_field());
        FillRect(dc, &r, bg);
        RECT b = {0, 0, tip_bar(), r.bottom};
        FillRect(dc, &b, bar);
        int pad = in_pad(), gap = (int)(6 * g_dpi), x0 = tip_bar() + pad, fy = pad + in_title_h() + gap;
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(58, 61, 72));
        HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, field);
        int rad = (int)(10 * g_dpi);
        RoundRect(dc, x0, fy, r.right - pad, fy + in_field_h(), rad, rad);
        SelectObject(dc, op); SelectObject(dc, ob);
        DeleteObject(pen); DeleteObject(bg); DeleteObject(bar); DeleteObject(field);
        SetBkMode(dc, TRANSPARENT);
        int ar = GetRValue(g_tip_accent), ag = GetGValue(g_tip_accent), ab = GetBValue(g_tip_accent);
        SetTextColor(dc, ink(RGB(ar + (255 - ar) * 3 / 10, ag + (255 - ag) * 3 / 10, ab + (255 - ab) * 3 / 10)));
        SelectObject(dc, g_input_tf);
        int i = index_of(g_input_sid);
        wchar_t title[120] = L"Reply";
        if (i >= 0) swprintf(title, 120, L"Reply to %hs", shown_room()->s[i].name[0] ? shown_room()->s[i].name : "claude");
        RECT tt = {x0, pad, r.right - pad, pad + in_title_h()};
        DrawTextW(dc, title, -1, &tt, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        wchar_t hint[1200];
        int used = 0;
        if (g_attach_n) {
            wchar_t *t = edit_text();
            hint[0] = 0;
            for (int k = 0; k < g_attach_n; k++) {
                if (!attach_used(t, k)) continue;
                wchar_t one[200];
                swprintf(one, 200, L"%ls[Image #%d] %ls", used++ ? L",  " : L"", g_attach_id[k], g_attach_name[k]);
                wcsncat(hint, one, 1000 - wcslen(hint));
            }
            free(t);
        }
        if (used) wcscat(hint, L"   \x00B7   Backspace deletes an image");
        else wcscpy(hint, L"Enter sends   \x00B7   Shift+Enter new line   \x00B7   Esc closes   \x00B7   drop an image on the core");
        SetTextColor(dc, ink(used ? RGB(190, 200, 225) : RGB(120, 126, 142)));
        SelectObject(dc, g_input_sf);
        RECT ht = {x0 + (int)(2 * g_dpi), fy + in_field_h() + gap, r.right - pad, r.bottom - pad};
        DrawTextW(dc, hint, -1, &ht, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        EndPaint(w, &ps);
        return 0;
    }
    case WM_SIZE: {
        int pad = in_pad(), inset = (int)(9 * g_dpi), gap = (int)(6 * g_dpi), x0 = tip_bar() + pad, fy = pad + in_title_h() + gap;
        MoveWindow(g_edit, x0 + inset, fy + (int)(6 * g_dpi), LOWORD(lp) - x0 - pad - 2 * inset, in_field_h() - (int)(12 * g_dpi), TRUE);
        InvalidateRect(w, NULL, FALSE);
        return 0;
    }
    case WM_DROPFILES: { int first = g_attach_n; add_drop((HDROP)wp, 1); insert_tokens(first); return 0; }
    case WM_COMMAND: if (HIWORD(wp) == EN_CHANGE) { edit_style(); InvalidateRect(w, NULL, FALSE); } return 0; /* CJK in its face; the hint lists the images still in the text */
    case WM_ACTIVATE: if (LOWORD(wp) == WA_INACTIVE) input_close(); return 0; /* clicked elsewhere */
    case WM_SETFOCUS: SetFocus(g_edit); return 0;
    }
    return DefWindowProc(w, msg, wp, lp);
}

void input_refont(void) {
    HFONT *f[3] = {&g_input_font, &g_input_tf, &g_input_sf};
    for (int i = 0; i < 3; i++) if (*f[i]) { DeleteObject(*f[i]); *f[i] = NULL; }
}

/* a Windows path as WSL sees it: C:\a b\c.png → /mnt/c/a\ b/c.png, \\wsl.localhost\Ubuntu\home\x → /home/x
 * (spaces escaped the way a terminal drop writes them); 0 for a path WSL cannot reach */
static int wsl_path(const wchar_t *win, wchar_t *out, size_t n) {
    wchar_t tmp[600];
    const wchar_t *rest;
    if (iswalpha(win[0]) && win[1] == L':' && (win[2] == L'\\' || win[2] == L'/')) {
        swprintf(tmp, 600, L"/mnt/%lc%ls", (wchar_t)towlower(win[0]), win + 2);
    } else if ((!_wcsnicmp(win, L"\\\\wsl.localhost\\", 16) && (rest = win + 16)) || (!_wcsnicmp(win, L"\\\\wsl$\\", 7) && (rest = win + 7))) {
        const wchar_t *sl = wcschr(rest, L'\\'); /* past the distro's name */
        if (!sl) return 0;
        swprintf(tmp, 600, L"%ls", sl);
    } else return 0;
    size_t o = 0;
    for (const wchar_t *c = tmp; *c && o + 2 < n; c++) {
        wchar_t ch = *c == L'\\' ? L'/' : *c;
        if (wcschr(L" '\"()&;$`!", ch)) out[o++] = L'\\';
        out[o++] = ch;
    }
    out[o] = 0;
    return 1;
}

static int is_image(const wchar_t *path) {
    const wchar_t *dot = wcsrchr(path, L'.');
    if (!dot) return 0;
    static const wchar_t *EXT[] = {L".png", L".jpg", L".jpeg", L".gif", L".webp"};
    for (int i = 0; i < 5; i++) if (!_wcsicmp(dot, EXT[i])) return 1;
    return 0;
}

/* image files dropped on a core or on the reply box: kept for the reply, each with its token number (other
 * files are passed over); finish: the drop came as WM_DROPFILES and is ours to free */
static void add_drop(HDROP d, int finish) {
    if (g_attach_n == MAX_ATTACH && g_edit) { /* full: forget the images whose tokens were deleted */
        wchar_t *t = edit_text();
        int o = 0;
        for (int k = 0; k < g_attach_n; k++) if (attach_used(t, k)) {
            if (o != k) { wcscpy(g_attach[o], g_attach[k]); wcscpy(g_attach_name[o], g_attach_name[k]); g_attach_id[o] = g_attach_id[k]; }
            o++;
        }
        g_attach_n = o;
        free(t);
    }
    UINT n = DragQueryFileW(d, 0xFFFFFFFF, NULL, 0);
    for (UINT i = 0; i < n && g_attach_n < MAX_ATTACH; i++) {
        wchar_t path[MAX_PATH * 2];
        if (!DragQueryFileW(d, i, path, MAX_PATH * 2) || !is_image(path)) continue;
        if (!wsl_path(path, g_attach[g_attach_n], 600)) continue;
        const wchar_t *base = wcsrchr(path, L'\\');
        wcsncpy(g_attach_name[g_attach_n], base ? base + 1 : path, 119);
        g_attach_name[g_attach_n][119] = 0;
        g_attach_id[g_attach_n] = ++g_attach_next;
        g_attach_n++;
    }
    if (finish) DragFinish(d);
    if (g_input) InvalidateRect(g_input, NULL, FALSE);
}
static int has_image(HDROP d) {
    UINT n = DragQueryFileW(d, 0xFFFFFFFF, NULL, 0);
    for (UINT i = 0; i < n; i++) { wchar_t path[MAX_PATH * 2]; if (DragQueryFileW(d, i, path, MAX_PATH * 2) && is_image(path)) return 1; }
    return 0;
}

static void input_open_for(const Session *s) {
    if (!strcmp(s->sid, "ghost")) return;
    if (!g_input) {
        WNDCLASS ic;
        memset(&ic, 0, sizeof ic);
        ic.lpfnWndProc = input_proc;
        ic.hInstance = GetModuleHandle(NULL);
        ic.hCursor = LoadCursor(NULL, IDC_IBEAM);
        ic.lpszClassName = L"VigilInput";
        RegisterClass(&ic);
        g_input_bg = CreateSolidBrush(RGB(36, 38, 46));
        g_input = CreateWindowEx(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_ACCEPTFILES, L"VigilInput", L"Vigil reply", WS_POPUP, 0, 0, 10, 10, NULL, NULL, ic.hInstance, NULL);
        LoadLibraryW(L"Msftedit.dll"); /* a rich edit: a font per run, so Chinese can have its own face, as in the terminal */
        g_edit = CreateWindowEx(0, MSFTEDIT_CLASS, L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN, 0, 0, 10, 10, g_input, NULL, ic.hInstance, NULL);
        SendMessageW(g_edit, EM_SETBKGNDCOLOR, 0, tip_field());
        SendMessageW(g_edit, EM_SETEVENTMASK, 0, ENM_CHANGE);
        g_edit_proc = (WNDPROC)SetWindowLongPtrW(g_edit, GWLP_WNDPROC, (LONG_PTR)edit_proc);
    }
    if (!g_input_font) {
        g_input_font = CreateFontW(-(int)(g_font * g_dpi + 0.5f), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, mono_face()); /* the CLI's face */
        g_input_tf = CreateFontW(-(int)(g_font * g_dpi + 0.5f), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        g_input_sf = CreateFontW(-(int)(g_font * 0.86f * g_dpi + 0.5f), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    }
    SendMessageW(g_edit, WM_SETFONT, (WPARAM)g_input_font, TRUE);
    edit_style();
    if (strcmp(g_input_sid, s->sid)) {
        if (strcmp(g_draft_sid, s->sid)) g_attach_n = g_attach_next = 0; /* images belong to the reply they were dropped for */
        snprintf(g_input_sid, sizeof g_input_sid, "%s", s->sid);
        SetWindowTextW(g_edit, g_draft && !strcmp(g_draft_sid, s->sid) ? g_draft : L"");
        edit_style();
    }
    InvalidateRect(g_input, NULL, FALSE);
    LONG_PTR ex = GetWindowLongPtr(g_tip, GWL_EXSTYLE); /* the box takes the wheel while a reply is typed */
    SetWindowLongPtr(g_tip, GWL_EXSTYLE, ex & ~WS_EX_TRANSPARENT);
    ShowWindow(g_input, SW_SHOW);
    input_place();
    void mask_alt(void);
    if (!g_rec_scene[0]) { /* (a recording never takes the keyboard) */
        mask_alt(); /* a key event from this process lets it take the foreground */
        SetForegroundWindow(g_input);
        SetFocus(g_edit);
    }
    SendMessageW(g_edit, EM_SETSEL, 0, -1);
    SendMessageW(g_edit, EM_SETSEL, (WPARAM)-1, -1);
}

/* ---- images dragged onto a core: it reads them (TRANSCRIBING) while they are over it and a moment after the
 * drop, then its reply box opens with an [Image #N] token for each. An OLE drop target on each core's window, so
 * the core can answer while the drag is still going on (WM_DROPFILES only says so at the end). */
static char g_drag_sid[72], g_drop_sid[72]; /* the core an image is over now; the one that just took a drop */
static double g_drop_until;
static int core_at(POINT p, float *lx, float *ly);
static Room *shown_room(void);
static void drop_on_core(int i, HDROP d) {
    Room *room = shown_room();
    if (i < 0 || i >= room->n || !strcmp(room->s[i].sid, "ghost") || !has_image(d)) return;
    Session *s = &room->s[i];
    if (strcmp(g_input_sid, s->sid) && strcmp(g_draft_sid, s->sid)) g_attach_n = g_attach_next = 0;
    snprintf(g_draft_sid, sizeof g_draft_sid, "%s", s->sid); /* so opening keeps them */
    int first = g_attach_n;
    add_drop(d, 0);
    snprintf(g_drop_sid, sizeof g_drop_sid, "%s", s->sid);
    g_drop_until = wall() + 1.4;
    input_open_for(s);
    insert_tokens(first);
}
static HDROP drag_files(IDataObject *d, STGMEDIUM *sm) {
    FORMATETC fe = {CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    return d && d->lpVtbl->GetData(d, &fe, sm) == S_OK ? (HDROP)sm->hGlobal : NULL;
}
static int g_drag_img; /* what is being dragged has an image in it */
static DWORD drag_at(POINTL pt) {
    POINT p = {pt.x, pt.y};
    Room *room = shown_room();
    int i = g_drag_img ? core_at(p, NULL, NULL) : -1;
    if (i >= 0 && i < room->n && strcmp(room->s[i].sid, "ghost")) { snprintf(g_drag_sid, sizeof g_drag_sid, "%s", room->s[i].sid); return DROPEFFECT_COPY; }
    g_drag_sid[0] = 0;
    return DROPEFFECT_NONE;
}
static HRESULT STDMETHODCALLTYPE dt_qi(IDropTarget *t, REFIID riid, void **pp) {
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDropTarget)) { *pp = t; return S_OK; }
    *pp = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE dt_ref(IDropTarget *t) { (void)t; return 1; } /* one static object, never freed */
static HRESULT STDMETHODCALLTYPE dt_enter(IDropTarget *t, IDataObject *d, DWORD keys, POINTL pt, DWORD *eff) {
    (void)t; (void)keys;
    STGMEDIUM sm;
    HDROP h = drag_files(d, &sm);
    g_drag_img = h && has_image(h);
    if (h) ReleaseStgMedium(&sm);
    *eff = drag_at(pt);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE dt_over(IDropTarget *t, DWORD keys, POINTL pt, DWORD *eff) { (void)t; (void)keys; *eff = drag_at(pt); return S_OK; }
static HRESULT STDMETHODCALLTYPE dt_leave(IDropTarget *t) { (void)t; g_drag_sid[0] = 0; return S_OK; }
static HRESULT STDMETHODCALLTYPE dt_drop(IDropTarget *t, IDataObject *d, DWORD keys, POINTL pt, DWORD *eff) {
    (void)t; (void)keys;
    POINT p = {pt.x, pt.y};
    STGMEDIUM sm;
    HDROP h = drag_files(d, &sm);
    g_drag_sid[0] = 0;
    *eff = DROPEFFECT_NONE;
    if (h) { drop_on_core(core_at(p, NULL, NULL), h); ReleaseStgMedium(&sm); *eff = DROPEFFECT_COPY; }
    return S_OK;
}
static IDropTargetVtbl g_dt_vtbl = {dt_qi, dt_ref, dt_ref, dt_enter, dt_over, dt_leave, dt_drop};
IDropTarget g_dt_ref = {&g_dt_vtbl};

/* ---- the pointer ---------------------------------------------------------------------------------- */
/* the core under p (screen pixels), or -1; look gets the offset from its centre in shell radii (y down).
 * The core drawn on top is asked first. */
static int core_at(POINT p, float *lx, float *ly) {
    if (g_menu_open) return -1;
    if (g_set && IsWindowVisible(g_set)) { RECT sr; GetWindowRect(g_set, &sr); if (PtInRect(&sr, p)) return -1; } /* (the Settings window is in front of the cores) */
    for (int k = -1; k < g_core_n; k++) {
        int i = k < 0 ? g_top : k;
        if (i < 0 || i >= g_core_n || (k >= 0 && i == g_top)) continue;
        RECT r = g_core_rc[i];
        if (r.right - r.left <= 0) continue;
        float rad = (r.right - r.left) * SHELL_HIT, dx = p.x - (r.left + r.right) * 0.5f, dy = p.y - (r.top + r.bottom) * 0.5f;
        if (dx * dx + dy * dy > rad * rad) continue;
        if (lx) { *lx = dx / rad; *ly = dy / rad; }
        return i;
    }
    return -1;
}

/* Defence only when the pointer rushes at a core: from over 2.2 shell radii away onto the core within 0.18 s. */
static void watch_for_rush(double now, POINT m) {
    if (rush_n == 128) { memmove(rush_t, rush_t + 1, 127 * sizeof *rush_t); memmove(rush_p, rush_p + 1, 127 * sizeof *rush_p); rush_n--; }
    rush_t[rush_n] = now; rush_p[rush_n++] = m;
    int drop = 0;
    while (drop < rush_n && now - rush_t[drop] > 0.18) drop++;
    if (drop) { memmove(rush_t, rush_t + drop, (size_t)(rush_n - drop) * sizeof *rush_t); memmove(rush_p, rush_p + drop, (size_t)(rush_n - drop) * sizeof *rush_p); rush_n -= drop; }
    if (g_down_sid[0] || g_carrying) return;
    Room *room = shown_room();
    for (int i = 0; i < g_core_n && i < room->n; i++) {
        Session *s = &room->s[i];
        RECT r = g_core_rc[i];
        float R = (r.right - r.left) * SHELL_FRAC, cx = (r.left + r.right) * 0.5f, cy = (r.top + r.bottom) * 0.5f;
        if (R < 4 || now - s->defend_at < 2 || session_shown(s, now) == ST_WORK) continue;
        if (hypotf(m.x - cx, m.y - cy) / R > 1.0f) continue;
        for (int j = 0; j < rush_n; j++) {
            if (hypotf(rush_p[j].x - cx, rush_p[j].y - cy) / R <= 2.2f) continue;
            float dx = (float)(m.x - rush_p[j].x), dy = (float)(m.y - rush_p[j].y), l = hypotf(dx, dy);
            s->defend_at = now; s->defend_x = dx / l; s->defend_y = dy / l; /* screen y is down, like the view */
            rush_n = 0;
            return;
        }
    }
}

/* clicks reach a window only while the pointer is on one of its cores (or it is busy with the mouse) */
static void set_transparent(Pane *p, int transparent) {
    if (!p->w || transparent == p->transparent) return;
    LONG_PTR ex = GetWindowLongPtr(p->w, GWL_EXSTYLE);
    SetWindowLongPtr(p->w, GWL_EXSTYLE, transparent ? ex | WS_EX_TRANSPARENT : ex & ~WS_EX_TRANSPARENT);
    p->transparent = transparent;
}

/* every frame: what the pointer is over */
static void track_mouse(double now) {
    POINT scr;
    cursor_pos(&scr);
    Room *room = shown_room();
    float lx = 0, ly = 0;
    int hit = core_at(scr, &lx, &ly), down = down_index();
    if (hit >= room->n) hit = -1;
    if (hit != g_hover) { g_hover = hit; g_hover_since = now; }
    int busy = g_moving || g_dragging || g_carrying;
    for (int i = 0; i < room->n; i++) {
        Session *s = &room->s[i];
        s->hover_to = i == hit || i == down;
        if (i == hit) { s->look_x_to = lx; s->look_y_to = ly; }
        s->lock_to = i == hit && !busy && now - g_hover_since > 2.0 ? 1.0f : 0.0f; /* lock-on after resting 2 s */
    }
    watch_for_rush(now, scr);
    int grabbed = busy || g_down_sid[0];
    for (int i = 0; i < MAX_SESS; i++) set_transparent(&g_pane[i], !(i == hit || (grabbed && i == down))); /* every core has its own window */
    if (input_open()) { /* typing a reply: the box stays on that session */
        int k = index_of(g_input_sid);
        if (k < 0) { input_close(); return; }
        RECT r = g_core_rc[k], beside = r;
        if (g_single && k == g_top) beside = g_cluster_rc;
        tip_show(&room->s[k], beside, (r.top + r.bottom) / 2, now);
        return;
    }
    if (busy) { g_hover_since = now; tip_hide(); return; }           /* no tooltip while dragging */
    if (hit < 0 || now - g_hover_since < 0.3) { tip_hide(); return; } /* a short pause before the tooltip */
    RECT r = g_core_rc[hit], beside = r;
    if (g_single && hit == g_top) beside = g_cluster_rc;
    tip_show(&room->s[hit], beside, (r.top + r.bottom) / 2, now);
}

/* Alt held down: with nothing else typed, letting go of Alt would open the menu bar of the window with the
 * keyboard focus. A key press nobody uses (vk 0xE8) in between stops that. */
static int alt_held(void) { return (GetAsyncKeyState(VK_MENU) & 0x8000) != 0; }
void mask_alt(void) {
    INPUT in[2];
    memset(in, 0, sizeof in);
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[1].ki.wVk = 0xE8;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof in[0]);
}

/* Alt-drag or Alt-click: until it is put down, every tick asks where the mouse is (with the button up there
 * are no mouse messages once it leaves the window), at a quicker pace */
static void start_follow(void) {
    cursor_pos(&g_last_mouse);
    g_last_mouse_t = wall();
    g_mvx = g_mvy = 0;
    int i = down_index();
    if (i >= 0) shown_room()->s[i].carried = 1;
}

/* The cluster (single core) or the held core (all cores) goes where the mouse is, and the cores feel the
 * motion: the carried one leans and sloshes (CARRY in session.c), and the small cores lag behind. */
static void follow(void) {
    POINT m;
    cursor_pos(&m);
    double now = wall(), dt = now - g_last_mouse_t;
    if (dt >= 1.0 / 240) { /* the mouse speed, smoothed a little */
        float a = 0.45f;
        g_mvx += ((float)((m.x - g_last_mouse.x) / dt) - g_mvx) * a;
        g_mvy += ((float)((m.y - g_last_mouse.y) / dt) - g_mvy) * a;
        g_last_mouse = m;
        g_last_mouse_t = now;
    }
    Room *room = shown_room();
    int hi = down_index();
    float w = hi >= 0 && hi < g_core_n && g_core_rc[hi].right - g_core_rc[hi].left > 1 ? (float)(g_core_rc[hi].right - g_core_rc[hi].left) : 100;
    for (int i = 0; i < room->n; i++) { /* single core: the whole cluster moves; all cores: only the held one */
        Session *s = &room->s[i];
        int moves = g_single || i == hi;
        s->move_x = moves ? g_mvx / w : 0;
        s->move_y = moves ? g_mvy / w : 0;
    }
    int tx = g_move_origin.x + m.x - g_move_mouse.x, ty = g_move_origin.y + m.y - g_move_mouse.y;
    if (!g_single) { g_hold_x = tx; g_hold_y = ty; return; }
    if (g_moving == 2 && hi >= 0) { /* a small core by hand: out of its orbit, where the mouse takes it */
        Orbit *ob = orbit_of(room->s[hi].sid);
        ob->px = (float)tx; ob->py = (float)ty; ob->vx = g_mvx; ob->vy = g_mvy; ob->free = 1;
        return;
    }
    g_mx = (float)tx; g_my = (float)ty; /* the main core; the small ones follow on their springs */
    clamp_main();
}

/* the end of an Alt-drag, or the click that puts a carried core down: it lands with a little bounce, and
 * its place is kept */
static void end_move(void) {
    Room *room = shown_room();
    for (int i = 0; i < room->n; i++) room->s[i].carried = 0, room->s[i].move_x = room->s[i].move_y = 0;
    int i = down_index();
    if (i >= 0) room->s[i].jig_vy += CARRY.drop;
    if (g_single && g_moving == 1) pos_put("@main", (int)g_mx, (int)g_my);
    else if (g_single && i >= 0) { Orbit *ob = orbit_of(room->s[i].sid); ob->vx = g_mvx * .5f; ob->vy = g_mvy * .5f; } /* let go: it drifts on a little */
    else if (!g_single && i >= 0 && g_room.n) pos_put(room->s[i].name, g_hold_x, g_hold_y);
    g_moving = 0;
    g_down_sid[0] = 0;
}

static void place_down(void) {
    if (!g_carrying) return;
    g_carrying = 0;
    follow();
    end_move();
}

/* while carried: the next click anywhere puts it down, and that click goes nowhere else */
static LRESULT CALLBACK mouse_hook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        if (g_carrying && (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN)) {
            place_down();
            g_swallow = wp == WM_LBUTTONDOWN ? WM_LBUTTONUP : WM_RBUTTONUP;
            return 1;
        }
        if (g_swallow && (UINT)wp == (UINT)g_swallow) {
            g_swallow = 0;
            PostMessage(g_cluster.w, WM_UNHOOK, 0, 0);
            return 1;
        }
    }
    return CallNextHookEx(g_hook, code, wp, lp);
}

/* an Alt-click that did not move picks it up: it follows the mouse with the button up */
static void carry(void) {
    g_carrying = 1;
    if (!g_hook) g_hook = SetWindowsHookEx(WH_MOUSE_LL, mouse_hook, GetModuleHandle(NULL), 0);
    if (!g_hook) place_down(); /* could not watch the clicks: put it straight down */
}

/* ---- away: no keyboard or mouse input for g_away_after seconds (asked of Windows, no permission needed).
 * Every core shows STANDBY then, except one that needs you; back again, cores with an unread reply flare up. */
static void update_overlays(double now) {
    LASTINPUTINFO li = {sizeof li, 0};
    double idle = GetLastInputInfo(&li) ? (GetTickCount() - li.dwTime) / 1000.0 : 0;
    int away = !g_rec_scene[0] && g_away_after > 0 && idle > g_away_after;
    Room *room = shown_room();
    if (g_away && !away)
        for (int i = 0; i < room->n; i++) if (room->s[i].unread) room->s[i].burst_at = now;
    g_away = away;
    for (int i = 0; i < room->n; i++) {
        Session *s = &room->s[i];
        s->overlay = 0;
        s->overlay = away && session_shown(s, now) != ST_WAIT ? ST_STANDBY : 0;
        if ((g_drag_sid[0] && !strcmp(s->sid, g_drag_sid)) || (now < g_drop_until && !strcmp(s->sid, g_drop_sid))) s->overlay = ST_TRANSCRIBE; /* an image on it */
    }
}

/* ---- bring to front: the window the session runs in. There is no iTerm2 session id on Windows, so this
 * looks for a terminal or VS Code window with the folder name in its title, else the Windows Terminal
 * window used last, else a console window; it cannot pick the tab. */
typedef struct { const char *name; HWND named, wt, con; } Find;

static int is_terminal(const char *cls, const char *title) {
    return !strcmp(cls, "CASCADIA_HOSTING_WINDOW_CLASS") || !strcmp(cls, "ConsoleWindowClass") ||
           (!strcmp(cls, "Chrome_WidgetWin_1") && strstr(title, "Visual Studio Code"));
}

static BOOL CALLBACK find_window(HWND w, LPARAM lp) { /* called top window first */
    Find *f = (Find *)lp;
    if (!IsWindowVisible(w) || GetWindow(w, GW_OWNER)) return TRUE;
    char cls[64], title[512];
    wchar_t wt[512];
    GetClassNameA(w, cls, sizeof cls);
    GetWindowTextW(w, wt, 512);
    WideCharToMultiByte(CP_UTF8, 0, wt, -1, title, sizeof title, NULL, NULL);
    if (!is_terminal(cls, title)) return TRUE;
    if (!f->named && f->name[0] && strstr(title, f->name)) f->named = w;
    if (!f->wt && !strcmp(cls, "CASCADIA_HOSTING_WINDOW_CLASS")) f->wt = w;
    if (!f->con && !strcmp(cls, "ConsoleWindowClass")) f->con = w;
    return TRUE;
}

static void bring_to_front(const Session *s) {
    if (!strcmp(s->sid, "ghost")) return;
    Find f = {s->name, NULL, NULL, NULL};
    EnumWindows(find_window, (LPARAM)&f);
    HWND w = f.named ? f.named : f.wt ? f.wt : f.con;
    if (!w) return;
    if (IsIconic(w)) ShowWindow(w, SW_RESTORE);
    mask_alt(); /* a key event from this process lets it hand the foreground over */
    SetForegroundWindow(w);
}

/* the last reply to the clipboard, as it came (Markdown and all) */
static void copy_reply(const Session *s) {
    msg_load(s, wall() + 1); /* (read again now, not in half a second) */
    if (!g_msg_reply || !OpenClipboard(g_cluster.w)) return;
    int n = MultiByteToWideChar(CP_UTF8, 0, g_msg_reply, -1, NULL, 0), lf = 0;
    for (const char *c = g_msg_reply; *c; c++) lf += *c == '\n';
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, sizeof(wchar_t) * (size_t)(n + lf));
    wchar_t *tmp = malloc(sizeof(wchar_t) * (size_t)n), *d = h ? GlobalLock(h) : NULL;
    if (d && tmp) {
        MultiByteToWideChar(CP_UTF8, 0, g_msg_reply, -1, tmp, n);
        for (wchar_t *c = tmp; ; c++) { if (*c == L'\n') *d++ = L'\r'; *d++ = *c; if (!*c) break; } /* Windows line ends */
        GlobalUnlock(h);
        EmptyClipboard();
        if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
    } else if (h) GlobalFree(h);
    free(tmp);
    CloseClipboard();
}

/* ---- focus (single core) -------------------------------------------------------------------------- */
typedef struct { char sid[72]; int hue; } HueOf;
static HueOf g_hue[MAX_SESS];
static int g_hue_n;

/* Which session is in the middle. Pinned by a click, it stays. Otherwise it moves only when there is news,
 * so it does not jump around while several sessions work: to a session that needs you, else to one with a
 * reply newer than the current focus; a session that ended is replaced by the one active last. Every core
 * gets its own colour, which tints it while others are shown around it. */
static void update_focus(double now) {
    Room *room = shown_room();
    HueOf keep[MAX_SESS];
    int kn = 0;
    for (int i = 0; i < room->n && i < MAX_SESS; i++) {
        Session *s = &room->s[i];
        int h = -1;
        for (int j = 0; j < g_hue_n; j++) if (!strcmp(g_hue[j].sid, s->sid)) h = g_hue[j].hue;
        if (h < 0) { /* the first free colour, starting from one picked by the folder name */
            unsigned start = 0;
            for (const char *c = s->name; *c; c++) start = start * 31 + (unsigned char)*c;
            h = (int)(start % NHUE);
            for (int k = 0; k < NHUE; k++) {
                int cand = (int)((start + k) % NHUE), used = 0;
                for (int j = 0; j < kn; j++) used |= keep[j].hue == cand;
                for (int j = 0; j < g_hue_n; j++) used |= g_hue[j].hue == cand && index_of(g_hue[j].sid) >= 0;
                if (!used) { h = cand; break; }
            }
        }
        snprintf(keep[kn].sid, sizeof keep[kn].sid, "%s", s->sid);
        keep[kn++].hue = h;
        s->aura = HUES[h];
        s->aura_a = g_single && g_others && room->n > 1 ? TOUCH.aura : 0; /* alone, it needs no name tag */
    }
    memcpy(g_hue, keep, sizeof(HueOf) * (size_t)kn);
    g_hue_n = kn;
    if (!g_single) return;
    int ci = index_of(g_focus);
    Session *cur = ci >= 0 ? &room->s[ci] : NULL;
    if (!cur) { g_focus_pinned = 0; g_focus[0] = 0; }
    if (g_focus_pinned) return;
    Session *pick = NULL, *fresh = NULL, *recent = NULL;
    for (int i = 0; i < room->n; i++) {
        Session *s = &room->s[i];
        if (s->state == ST_WAIT && (!pick || s->state_t > pick->state_t)) pick = s;
        if (!fresh || s->burst_at > fresh->burst_at) fresh = s;
        if (!recent || s->last > recent->last) recent = s;
    }
    if (!pick && fresh && fresh->burst_at > g_focus_since) pick = fresh;
    if (!pick) pick = cur ? cur : recent;
    if (pick && strcmp(pick->sid, g_focus)) { snprintf(g_focus, sizeof g_focus, "%s", pick->sid); g_focus_since = now; }
}

static void toggle_mode(void) {
    if (g_moving) return;
    g_single = !g_single;
    g_focus_pinned = 0;
    g_orbit_n = 0; /* every core back in orbit */
    pics_invalidate();
    tip_hide();
}

static double g_polled;

static void write_stats(double now) {
    double span = now - g_stat_since;
    if (span < 5) return;
    char path[1100];
    snprintf(path, sizeof path, "%s\\panel-stats.txt", g_dir[0] ? g_dir : ".");
    FILE *f = fopen(path, "a");
    if (f) {
        fprintf(f, "%.0f  ticks %.1f/s  pictures %.1f/s  window updates %.1f/s  tick %.2f ms avg\n", now, g_stat_tick / span,
                g_stat_pic / span, g_stat_push / span, g_stat_tick ? g_stat_ms / g_stat_tick : 0);
        fclose(f);
    }
    g_stat_tick = g_stat_pic = g_stat_push = 0;
    g_stat_ms = 0;
    g_stat_since = now;
}

static void settings_tick(double now);
static int set_open(void);

static void tick(void) {
    LARGE_INTEGER q0, q1, qf;
    QueryPerformanceCounter(&q0);
    double now = wall();
    float dt = (float)(now - g_last);
    if (dt > 0.25f) dt = 0.25f;
    g_last = now;
    if (g_demo_n && !g_rec_still) demo_run(&g_demo, &g_room, now);
    else if (now - g_polled >= 0.25) { /* the events file sits in WSL: each look is a trip into its VM, 4 a second is plenty */
        g_polled = now;
        int n = tail_poll(&g_tail, g_ev, 1024);
        for (int i = 0; i < n; i++) room_apply(&g_room, &g_ev[i]);
    }
    if (g_moving) follow();
    update_overlays(now);
    update_focus(now);
    track_mouse(now);
    room_tick(&g_room, &g_shape, now, dt);
    g_ghost.s[0].last = now;
    room_tick(&g_ghost, &g_shape, now, dt);
    draw(now);
    settings_tick(now);
    if (g_stats) {
        QueryPerformanceCounter(&q1); QueryPerformanceFrequency(&qf);
        g_stat_tick++;
        g_stat_ms += (q1.QuadPart - q0.QuadPart) * 1000.0 / qf.QuadPart;
        write_stats(now);
    }
}

/* how long until the next tick: 60 a second while the mouse is on a core or something is in the hand, else 30 (pictures keep their
 * own, lower rates; a tick that has nothing due costs next to nothing) */
static double tick_interval(void) { return g_moving || g_dragging || g_carrying || g_down_sid[0] || g_hover >= 0 || set_open() ? 1.0 / 60 : 1.0 / 30; }

/* ---- the Settings window: a dark card (design "A, calm cards"), a live preview across the top (the cluster with
 * its gravity: drag the core; the state it shows; a reply in the text box's font), three cards below (Look, Motion,
 * Replies) with icon tiles, sliders, a switch and chips. Drawn antialiased into its own bitmap and shown as a
 * layered window, so the corners are smooth too. Every choice applies at once and is kept, as the menu's were. */
enum { R_SHAPE, R_CORE, R_GLOW, R_FRAME, R_SCALE, R_GRAVITY, R_OTHERS, R_TIPAT, R_TIPFONT, R_TEXT, R_LABELS, R_AWAY, R_TIPSIZE, R_TIPBG, R_WHEEL, R_TEXTCOL };
enum { K_PICK, K_SLIDE, K_TOGGLE, K_CYCLE, K_STATE, K_RESET, K_CLOSE, K_TAB, K_MSTATE, K_MTOG, K_MPLAY };
typedef struct { RECT r; int kind, row, val, n; float t0, t1; } SetHit; /* a slider: n stops from t0 to t1 (pixels) */
static const int PV_STATES[6] = {ST_IDLE, ST_THINK, ST_WORK, ST_WAIT, ST_CHEER, ST_SLEEP};
static const wchar_t *PV_NAMES[6] = {L"Idle", L"Thinking", L"Working", L"Needs you", L"Done", L"Asleep"};
static const wchar_t *AWAY_TEXT[4] = {L"1 minute", L"2 minutes", L"5 minutes", L"never"};

/* the palette (0xRRGGBB) */
static const unsigned S_BG = 0x18181B, S_BAND = 0x0F0F12, S_CARD = 0x212126, S_TILE = 0x19191D, S_EDGE = 0x34343B,
                      S_EDGE_HOT = 0x55555E, S_TEXT = 0xECECEF, S_LABEL = 0xA1A1AA, S_MUTED = 0x8B8B94, S_CHIP = 0x27272D,
                      S_CHIP_HOT = 0x323239, S_CHIP_TXT = 0xB4B4BC, S_OR = 0xFF8A1F, S_ON_OR = 0x1F1206, S_KNOB = 0xFFF1E3, S_CLOSE_HOT = 0x5A2A22;
static Col hexc(unsigned v) { Col c = {(v >> 16 & 255) / 255.0f, (v >> 8 & 255) / 255.0f, (v & 255) / 255.0f}; return c; }
static COLORREF hexr(unsigned v) { return RGB(v >> 16 & 255, v >> 8 & 255, v & 255); }

static Pane g_sl, g_so;             /* the window's still layer (redrawn when something changes) and what is shown */
static int g_sl_dirty = 1;
static HFONT g_sf_title, g_sf_card, g_sf_text, g_sf_label, g_sf_small, g_sf_aa, g_sf_aam;
static SetHit g_hits[200];
static int g_hits_n, g_hot = -1, g_slide = -1, g_tracking, g_close_armed; /* armed: pressed on the close button */
typedef struct { RECT r; wchar_t t[160]; HFONT f; COLORREF c; UINT how; } TextOp;
static TextOp g_tx[160];
static int g_tx_n;
static RECT g_pv_rc;                /* the preview's cluster, in the window */
static Pane g_pv;                   /* its picture (a DIB, no window of its own) */
static Room g_pv_room;
static Pic g_pv_pic[4];
static Orbit g_pv_ob[4];
static int g_pv_st = ST_THINK, g_pv_drag; /* the state the preview's main core shows */
static int g_tab;                   /* 0 General, 1 Motions */
static double g_mot_msg_until;      /* "a state needs a routine" shows until then */
static float g_pv_zoom = 1;         /* the wheel over the preview: 1 to 3 times, to see the detail */
static float g_pv_mx, g_pv_my, g_pv_ox, g_pv_oy, g_pv_vx, g_pv_vy;
static double g_pv_orbit, g_pv_lock_until, g_pv_last;

static float S(float v) { return v * (float)g_dpi; }
static RECT SR(float x0, float y0, float x1, float y1) { RECT r = {(int)lroundf(S(x0)), (int)lroundf(S(y0)), (int)lroundf(S(x1)), (int)lroundf(S(y1))}; return r; }
static int SW_W(void) { return (int)lroundf(S(880)); }
static int SW_H(void) { return (int)lroundf(S(830)); }
static int set_open(void) { return g_set && IsWindowVisible(g_set); }

static int set_current(int row) {
    for (int i = 0; i < 7; i++) {
        switch (row) {
        case R_SHAPE: if (i < 4 && !strcmp(g_shape_name, SHAPES[i])) return i; break;
        case R_CORE: if (i < 4 && fabsf(g_core - CORES[i]) < 0.001f) return i; break;
        case R_GLOW: if (i < 3 && g_glow == GLOWS[i]) return i; break;
        case R_FRAME: if (!strcmp(g_frame, FRAMES[i])) return i; break;
        case R_SCALE: if (i < 4 && g_scale == SCALES[i]) return i; break;
        case R_GRAVITY: if (i < 4 && g_gravity == GRAVITIES[i]) return i; break;
        case R_OTHERS: return g_others;
        case R_TIPAT: return g_tip_middle;
        case R_TIPFONT: return !g_tip_mono;
        case R_TEXT: if (i < 5 && g_font == FONTS[i]) return i; break;
        case R_LABELS: return g_labels;
        case R_AWAY: if (i < 4 && g_away_after == AWAYS[i]) return i; break;
        case R_TIPSIZE: if (i < 4 && g_tip_size == TIPSIZES[i]) return i; break;
        case R_TIPBG: return g_tip_bg;
        case R_WHEEL: return g_wheel_spin;
        case R_TEXTCOL: return g_text_col;
        }
    }
    return -1;
}

static void settings_applied(int save) {
    if (save) settings_save();
    pics_invalidate();
    for (int i = 0; i < 4; i++) g_pv_pic[i].due = 0;
    for (int i = 0; i < MAX_SESS; i++) g_pane[i].dirty = 1;
    g_cluster.dirty = 1;
    g_sl_dirty = 1;
}

/* Lucide's "palette" icon (ISC License, lucide.dev), its outline sampled from the SVG path on a 24-unit grid,
 * drawn at (cx, cy) `size` pixels across with the icon's 2-unit round strokes */
static const float LUCIDE_PALETTE[] = {
    12.00f, 2.00f, 11.32f, 2.02f, 10.65f, 2.09f, 9.99f, 2.20f, 9.35f, 2.36f, 8.72f, 2.55f, 8.12f, 2.79f, 7.53f, 3.06f,
    6.96f, 3.37f, 6.42f, 3.71f, 5.90f, 4.09f, 5.40f, 4.50f, 4.94f, 4.94f, 4.50f, 5.40f, 4.09f, 5.90f, 3.71f, 6.42f,
    3.37f, 6.96f, 3.06f, 7.53f, 2.79f, 8.12f, 2.55f, 8.72f, 2.36f, 9.35f, 2.20f, 9.99f, 2.09f, 10.65f, 2.02f, 11.32f,
    2.00f, 12.00f, 2.02f, 12.68f, 2.09f, 13.35f, 2.20f, 14.01f, 2.36f, 14.65f, 2.55f, 15.28f, 2.79f, 15.88f, 3.06f, 16.47f,
    3.37f, 17.04f, 3.71f, 17.58f, 4.09f, 18.10f, 4.50f, 18.60f, 4.94f, 19.06f, 5.40f, 19.50f, 5.90f, 19.91f, 6.42f, 20.29f,
    6.96f, 20.63f, 7.53f, 20.94f, 8.12f, 21.21f, 8.72f, 21.45f, 9.35f, 21.64f, 9.99f, 21.80f, 10.65f, 21.91f, 11.32f, 21.98f,
    12.00f, 22.00f, 12.44f, 21.94f, 12.84f, 21.77f, 13.17f, 21.51f, 13.43f, 21.17f, 13.59f, 20.76f, 13.65f, 20.31f, 13.63f, 20.10f,
    13.59f, 19.89f, 13.53f, 19.69f, 13.44f, 19.51f, 13.33f, 19.34f, 13.21f, 19.19f, 13.08f, 19.04f, 12.97f, 18.87f, 12.88f, 18.69f,
    12.82f, 18.50f, 12.79f, 18.29f, 12.77f, 18.06f, 12.82f, 17.63f, 12.99f, 17.22f, 13.25f, 16.87f, 13.60f, 16.61f, 14.01f, 16.44f,
    14.44f, 16.39f, 16.44f, 16.39f, 16.82f, 16.38f, 17.19f, 16.34f, 17.55f, 16.28f, 17.91f, 16.19f, 18.25f, 16.09f, 18.59f, 15.96f,
    18.92f, 15.80f, 19.23f, 15.63f, 19.54f, 15.44f, 19.82f, 15.23f, 20.10f, 15.00f, 20.36f, 14.76f, 20.60f, 14.50f, 20.83f, 14.23f,
    21.04f, 13.94f, 21.23f, 13.64f, 21.40f, 13.32f, 21.55f, 12.99f, 21.68f, 12.66f, 21.79f, 12.31f, 21.88f, 11.95f, 21.94f, 11.59f,
    21.98f, 11.22f, 21.99f, 10.84f, 21.97f, 10.24f, 21.89f, 9.65f, 21.78f, 9.07f, 21.62f, 8.51f, 21.42f, 7.96f, 21.19f, 7.42f,
    20.91f, 6.90f, 20.60f, 6.40f, 20.26f, 5.92f, 19.88f, 5.46f, 19.47f, 5.02f, 19.03f, 4.61f, 18.57f, 4.22f, 18.07f, 3.86f,
    17.55f, 3.52f, 17.01f, 3.22f, 16.45f, 2.94f, 15.86f, 2.70f, 15.26f, 2.49f, 14.63f, 2.32f, 14.00f, 2.18f, 13.34f, 2.08f,
    12.68f, 2.02f, 12.00f, 2.00f,
};
static void palette_icon(Pane *pn, float cx, float cy, float size, unsigned col) {
    float k = size / 24, x0 = cx - size / 2, y0 = cy - size / 2;
    int n = (int)(sizeof LUCIDE_PALETTE / sizeof LUCIDE_PALETTE[0]) / 2;
    Col c = hexc(col);
    for (int i = 0; i + 1 < n; i++) {
        const float *a = LUCIDE_PALETTE + 2 * i;
        aa_seg(pn, x0 + a[0] * k, y0 + a[1] * k, x0 + a[2] * k, y0 + a[3] * k, 2 * k, c, 1);
    }
    static const float DOT[4][2] = {{13.5f, 6.5f}, {17.5f, 10.5f}, {8.5f, 7.5f}, {6.5f, 12.5f}};
    for (int i = 0; i < 4; i++) aa_disc(pn, x0 + DOT[i][0] * k, y0 + DOT[i][1] * k, 1.5f * k, c, 1);
}

/* the colour window opens in the middle of the Settings window, in front of it */
static UINT_PTR CALLBACK cc_hook(HWND d, UINT msg, WPARAM wp, LPARAM lp) {
    (void)wp; (void)lp;
    if (msg == WM_INITDIALOG && g_set) {
        RECT sr, dr;
        GetWindowRect(g_set, &sr); GetWindowRect(d, &dr);
        int w = dr.right - dr.left, h = dr.bottom - dr.top;
        SetWindowPos(d, HWND_TOPMOST, (sr.left + sr.right - w) / 2, (sr.top + sr.bottom - h) / 2, 0, 0, SWP_NOSIZE);
    }
    return 0; /* the dialog's own handling goes on */
}

/* the palette swatch: Windows' colour window, opened on the colour in use; the cores keep moving meanwhile (a plain
 * timer ticks while the window runs its own message loop) */
static int pick_colour(unsigned *target) {
    static COLORREF keep[16]; /* its own row of custom colours, for this run */
    CHOOSECOLORW cc;
    memset(&cc, 0, sizeof cc);
    cc.lStructSize = sizeof cc;
    cc.hwndOwner = g_set;
    cc.rgbResult = RGB(*target >> 16 & 255, *target >> 8 & 255, *target & 255);
    cc.lpCustColors = keep;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR | CC_ENABLEHOOK;
    cc.lpfnHook = cc_hook;
    SetTimer(g_cluster.w, ID_TIMER, 33, NULL);
    BOOL ok = ChooseColorW(&cc);
    KillTimer(g_cluster.w, ID_TIMER);
    if (!ok) return 0;
    *target = (unsigned)GetRValue(cc.rgbResult) << 16 | (unsigned)GetGValue(cc.rgbResult) << 8 | GetBValue(cc.rgbResult);
    return 1;
}
static void set_choose(int row, int val) {
    int palette = (row == R_TIPBG || row == R_TEXTCOL) && val == 5;
    if (val == set_current(row) && row != R_OTHERS && !palette) return;
    if (palette && !pick_colour(row == R_TIPBG ? &g_tip_custom : &g_text_custom)) return; /* cancelled: as it was */
    switch (row) {
    case R_SHAPE: snprintf(g_shape_name, sizeof g_shape_name, "%s", SHAPES[val]); shape_make(&g_shape, g_shape_name); break;
    case R_CORE: g_core = CORES[val]; break;
    case R_GLOW: g_glow = GLOWS[val]; break;
    case R_FRAME: snprintf(g_frame, sizeof g_frame, "%s", FRAMES[val]); g_pv_lock_until = wall() + 2.5; break;
    case R_SCALE: g_scale = SCALES[val]; main_place(); break;
    case R_GRAVITY: g_gravity = GRAVITIES[val]; break;
    case R_OTHERS: g_others = !g_others; break;
    case R_TIPAT: g_tip_middle = val; break;
    case R_TIPFONT: g_tip_mono = !val; tip_refont(); break;
    case R_TEXT: g_font = FONTS[val]; tip_refont(); break;
    case R_LABELS: g_labels = val; break;
    case R_AWAY: g_away_after = AWAYS[val]; break;
    case R_TIPSIZE: g_tip_size = TIPSIZES[val]; tip_hide(); break;
    case R_TIPBG: g_tip_bg = val; if (g_input) edit_style(); if (g_tip) InvalidateRect(g_tip, NULL, FALSE); if (g_input) { SendMessageW(g_edit, EM_SETBKGNDCOLOR, 0, tip_field()); edit_style(); InvalidateRect(g_input, NULL, FALSE); } break;
    case R_WHEEL: g_wheel_spin = val; break;
    case R_TEXTCOL: g_text_col = val; if (g_tip) InvalidateRect(g_tip, NULL, FALSE); if (g_input) { edit_style(); InvalidateRect(g_input, NULL, FALSE); } break;
    default: return;
    }
    settings_applied(1);
}

static void set_reset(void) {
    if (!settings_saved()) return;
    char path[1100];
    if (settings_path(path, sizeof path)) DeleteFileA(path);
    restore_launch();
    shape_make(&g_shape, g_shape_name);
    tip_refont();
    main_place();
    settings_applied(0);
}

/* ---- drawing: shapes antialiased straight into the bitmap, text with GDI after them (logical units, S() scales) */
static void aa_rrect(Pane *pn, float x0, float y0, float x1, float y1, float rad, Col c, float a, float line) {
    float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f, hx = (x1 - x0) * 0.5f, hy = (y1 - y0) * 0.5f;
    if (rad > hx) rad = hx;
    if (rad > hy) rad = hy;
    int bx0 = (int)floorf(x0) - 1, by0 = (int)floorf(y0) - 1, bx1 = (int)ceilf(x1) + 1, by1 = (int)ceilf(y1) + 1;
    if (bx0 < 0) bx0 = 0;
    if (by0 < 0) by0 = 0;
    if (bx1 > pn->W) bx1 = pn->W;
    if (by1 > pn->H) by1 = pn->H;
    for (int y = by0; y < by1; y++)
        for (int x = bx0; x < bx1; x++) {
            float qx = fabsf(x + 0.5f - cx) - (hx - rad), qy = fabsf(y + 0.5f - cy) - (hy - rad);
            float d = hypotf(fmaxf(qx, 0), fmaxf(qy, 0)) + fminf(fmaxf(qx, qy), 0) - rad;
            float cov = line > 0 ? 0.5f - (fabsf(d + line * 0.5f) - line * 0.5f) : 0.5f - d;
            if (cov > 0) px_over(pn, x, y, c, a * fminf(1, cov));
        }
}
static void box(Pane *pn, float x0, float y0, float x1, float y1, float rad, unsigned fill, unsigned edge, float ew) {
    aa_rrect(pn, S(x0), S(y0), S(x1), S(y1), S(rad), hexc(fill), 1, 0);
    if (ew > 0) aa_rrect(pn, S(x0), S(y0), S(x1), S(y1), S(rad), hexc(edge), 1, S(ew));
}
static void tx(float x0, float y0, float x1, float y1, const wchar_t *t, HFONT f, unsigned c, UINT how) {
    if (g_tx_n >= 160) return;
    TextOp *o = &g_tx[g_tx_n++];
    o->r = SR(x0, y0, x1, y1);
    wcsncpy(o->t, t, 159); o->t[159] = 0;
    o->f = f; o->c = hexr(c); o->how = how;
}
static void hit(float x0, float y0, float x1, float y1, int kind, int row, int val) {
    if (g_hits_n < 200) g_hits[g_hits_n++] = (SetHit){SR(x0, y0, x1, y1), kind, row, val, 0, 0, 0};
}
static int is_hot(int kind, int row, int val) { return g_hot >= 0 && g_hits[g_hot].kind == kind && g_hits[g_hot].row == row && g_hits[g_hot].val == val; }

static void set_fonts(void) {
    if (g_sf_title) return;
    const wchar_t *ui = L"Segoe UI";
    g_sf_title = CreateFontW(-(int)S(18), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, ui);
    g_sf_card = CreateFontW(-(int)S(14.5f), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, ui);
    g_sf_text = CreateFontW(-(int)S(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, ui);
    g_sf_label = CreateFontW(-(int)S(12), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, ui);
    g_sf_small = CreateFontW(-(int)S(11), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, ui);
    g_sf_aa = CreateFontW(-(int)S(19), 0, 0, 0, FW_MEDIUM, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, ui);
    g_sf_aam = CreateFontW(-(int)S(19), 0, 0, 0, FW_MEDIUM, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, mono_face());
}

/* a tile, button or chip that can be the chosen one */
static void pick_box(Pane *pn, float x0, float y0, float x1, float y1, float rad, int on, int hot, const wchar_t *t, HFONT f) {
    if (on) box(pn, x0, y0, x1, y1, rad, hot ? S_CHIP_HOT : S_TILE, S_OR, 1.5f); /* chosen: the frame alone */
    else box(pn, x0, y0, x1, y1, rad, hot ? S_CHIP_HOT : S_TILE, hot ? S_EDGE_HOT : S_EDGE, 1);
    if (t) tx(x0, y0, x1, y1, t, f, on ? S_TEXT : S_CHIP_TXT, DT_CENTER);
}

/* the four shells as small line icons, centred on (cx, cy) */
static void shape_icon(Pane *pn, int i, float cx, float cy, unsigned col) {
    Col c = hexc(col);
    float r = S(10), w = S(1.3f);
    cx = S(cx); cy = S(cy);
    if (i == 0 || i == 1) { /* a pentagon; the buckyball inside a circle */
        float rr = i ? r * 0.45f : r;
        for (int k = 0; k < 5; k++) {
            double a0 = -M_PI / 2 + k * 2 * M_PI / 5, a1 = a0 + 2 * M_PI / 5;
            aa_seg(pn, cx + rr * (float)cos(a0), cy + rr * (float)sin(a0), cx + rr * (float)cos(a1), cy + rr * (float)sin(a1), w, c, 1);
        }
        if (i) aa_ring(pn, cx, cy, r, w, c, 1);
    } else if (i == 2) { /* a hexagon with its spokes */
        for (int k = 0; k < 6; k++) {
            double a0 = -M_PI / 2 + k * M_PI / 3, a1 = a0 + M_PI / 3;
            aa_seg(pn, cx + r * (float)cos(a0), cy + r * (float)sin(a0), cx + r * (float)cos(a1), cy + r * (float)sin(a1), w, c, 1);
            if (k < 3) aa_seg(pn, cx + r * (float)cos(a0), cy + r * (float)sin(a0), cx - r * (float)cos(a0), cy - r * (float)sin(a0), w * 0.8f, c, 1);
        }
    } else { /* a circle, finely cut */
        aa_ring(pn, cx, cy, r, w, c, 1);
        for (int k = 0; k < 4; k++) {
            double a = k * M_PI / 4;
            aa_seg(pn, cx + r * (float)cos(a), cy + r * (float)sin(a), cx - r * (float)cos(a), cy - r * (float)sin(a), w * 0.7f, c, 1);
        }
    }
}

/* a slider over n stops between x0 and x1 at height y */
static void slider(Pane *pn, float x0, float y, float x1, int row, int n) {
    int cur = set_current(row);
    float kx = cur >= 0 ? x0 + (x1 - x0) * cur / (n - 1) : x0;
    box(pn, x0, y - 2, x1, y + 2, 2, S_EDGE, 0, 0);
    if (cur >= 0) box(pn, x0, y - 2, kx, y + 2, 2, S_OR, 0, 0);
    for (int i = 0; i < n; i++) { /* the stops */
        float sx = x0 + (x1 - x0) * i / (n - 1);
        if (sx > kx + 1) aa_disc(pn, S(sx), S(y), S(1.6f), hexc(S_EDGE_HOT), 1);
    }
    int hot = (g_hot >= 0 && g_hits[g_hot].kind == K_SLIDE && g_hits[g_hot].row == row) || (g_slide >= 0 && g_hits[g_slide].row == row);
    if (hot) aa_disc(pn, S(kx), S(y), S(12), hexc(S_OR), 0.18f);
    aa_disc(pn, S(kx), S(y), S(8), hexc(S_KNOB), 1);
    if (g_hits_n < 200) g_hits[g_hits_n++] = (SetHit){SR(x0 - 10, y - 13, x1 + 10, y + 13), K_SLIDE, row, cur, n, S(x0), S(x1)};
}

/* ---- the preview: four made-up sessions, the main one in the state picked beside it */
static void pv_init(double now) {
    if (g_pv_room.n) return;
    static const char *names[4] = {"vigil", "api", "docs", "web"};
    g_pv_room.n = 4;
    for (int i = 0; i < 4; i++) {
        session_init(&g_pv_room.s[i], names[i], now, &g_shape);
        snprintf(g_pv_room.s[i].name, sizeof g_pv_room.s[i].name, "%s", names[i]);
        g_pv_room.s[i].aura = HUES[i];
    }
    g_pv_last = now;
}

/* ---- the Motions page: which routines and actions each state may use (MOTION_MAP), and a preview of each */
static const int MOT_STATES[ST_N] = {ST_IDLE, ST_THINK, ST_WORK, ST_WAIT, ST_CHEER, ST_SLEEP, ST_SPEAK, ST_LISTEN, ST_TRANSCRIBE, ST_STANDBY, ST_ERROR};
static const wchar_t *MOT_STATE_NAMES[ST_N] = {L"Idle", L"Thinking", L"Working", L"Needs you", L"Done", L"Asleep",
                                              L"Speaking", L"Listening", L"Transcribing", L"Away", L"Error"};
static const wchar_t *state_wname(int st) { for (int i = 0; i < ST_N; i++) if (MOT_STATES[i] == st) return MOT_STATE_NAMES[i]; return L"?"; }
static void txa(float x0, float y0, float x1, float y1, const char *a, HFONT f, unsigned c, UINT how) {
    wchar_t w[160];
    MultiByteToWideChar(CP_UTF8, 0, a, -1, w, 160);
    w[159] = 0;
    tx(x0, y0, x1, y1, w, f, c, how);
}
static int bits(uint32_t v) { int n = 0; for (; v; v &= v - 1) n++; return n; }

/* a running routine that was just switched off makes way at once */
static void mot_refresh(int st, int rt) {
    for (int i = 0; i < g_room.n; i++) if (g_room.s[i].mo.rt == rt && g_room.s[i].mo.routine == st) g_room.s[i].mo.routine = -1;
    for (int i = 0; i < 4; i++) if (g_pv_room.s[i].mo.rt == rt && g_pv_room.s[i].mo.routine == st) g_pv_room.s[i].mo.routine = -1;
}
static void mot_play(int row, int id) {
    Session *m = &g_pv_room.s[0];
    if (row == 0) motion_play(m, id);
    else whim_play(m, id, wall());
    g_pv_lock_until = 0;
}
static void mot_toggle(int row, int id) {
    MotionMap *mm = &MOTION_MAP[g_pv_st];
    uint32_t *set = row == 0 ? &mm->rt : &mm->act, bit = 1u << id;
    if (row == 0 && (*set & bit) && bits(*set) == 1) { g_mot_msg_until = wall() + 3; g_sl_dirty = 1; return; } /* a state needs a routine */
    *set ^= bit;
    if (*set & bit) mot_play(row, id); /* switched on: shown at once */
    else if (row == 0) mot_refresh(g_pv_st, id);
    settings_applied(1);
}

/* a chip that can be switched on, with a play button at its right end */
static void mot_chip(Pane *pn, float x0, float y0, float x1, float y1, int on, int dis, int row, int id, const char *name) {
    int hot = is_hot(K_MTOG, row, id), hp = is_hot(K_MPLAY, row, id);
    box(pn, x0, y0, x1, y1, (y1 - y0) / 2, hot ? S_CHIP_HOT : S_TILE, on ? S_OR : (hot ? S_EDGE_HOT : S_EDGE), on ? 1.5f : 1);
    txa(x0 + 10, y0, x1 - 19, y1, name, g_sf_small, on ? S_TEXT : dis ? 0x55555E : S_CHIP_TXT, DT_LEFT | DT_END_ELLIPSIS);
    float cy = (y0 + y1) / 2, P[6] = {S(x1 - 16), S(cy - 4), S(x1 - 16), S(cy + 4), S(x1 - 9), S(cy)};
    aa_tri(pn, P, hexc(hp ? S_OR : S_MUTED), 1);
    if (!dis) hit(x0, y0, x1 - 22, y1, K_MTOG, row, id);
    hit(x1 - 23, y0, x1, y1, K_MPLAY, row, id);
}

static void motions_page(Pane *pn) {
    const float y0 = 304, l = 40, r = 840;
    int st = g_pv_st;
    box(pn, 24, y0, 856, 774, 16, S_CARD, 0, 0);
    tx(l, y0 + 12, 300, y0 + 34, L"Motions", g_sf_card, S_TEXT, DT_LEFT);
    tx(300, y0 + 12, r, y0 + 34, L"Each state picks one of its routines every time it starts; actions play now and then.", g_sf_small, S_MUTED, DT_RIGHT);
    /* the states: Claude Code's, then the panel's own */
    static const wchar_t *rowname[2] = {L"Claude Code", L"Panel"};
    for (int rw = 0; rw < 2; rw++) {
        float y = y0 + 42 + rw * 32;
        tx(l, y, l + 86, y + 26, rowname[rw], g_sf_small, S_MUTED, DT_LEFT);
        int n = rw ? 5 : 6, first = rw ? 6 : 0;
        float cw = (r - (l + 88) - 5 * 6) / 6;
        for (int i = 0; i < n; i++) {
            int sti = MOT_STATES[first + i], on = sti == st, hot = is_hot(K_MSTATE, 0, sti);
            float a = l + 88 + i * (cw + 6);
            box(pn, a, y, a + cw, y + 26, 13, hot ? S_CHIP_HOT : S_CHIP, S_OR, on ? 1.5f : 0);
            aa_disc(pn, S(a + 13), S(y + 13), S(3.5f), LOOKS[sti].halo, 1);
            tx(a + 22, y, a + cw - 6, y + 26, MOT_STATE_NAMES[first + i], g_sf_label, on ? S_TEXT : S_CHIP_TXT, DT_LEFT | DT_END_ELLIPSIS);
            hit(a, y, a + cw, y + 26, K_MSTATE, 0, sti);
        }
    }
    /* the routines: this state's own first */
    const MotionMap *mm = &MOTION_MAP[st];
    wchar_t lab[120];
    swprintf(lab, 120, L"Routines for %ls", state_wname(st));
    tx(l, y0 + 108, r, y0 + 128, lab, g_sf_label, S_LABEL, DT_LEFT);
    tx(l, y0 + 108, r, y0 + 128, L"click to use it here  \x25B6 to watch it", g_sf_small, S_MUTED, DT_RIGHT);
    int order[RT_N], n = 0;
    for (int pass = 0; pass < 2; pass++) for (int i = 1; i < RT_N; i++) if ((ROUTINES[i].home == st) == !pass) order[n++] = i;
    float cw = (r - l - 6 * 5) / 7;
    for (int i = 0; i < n; i++) {
        int id = order[i];
        float a = l + (i % 7) * (cw + 5), y = y0 + 132 + (i / 7) * 31;
        mot_chip(pn, a, y, a + cw, y + 26, (mm->rt >> id) & 1, id == RT_HOLE && st != ST_SLEEP, 0, id, ROUTINES[id].name);
    }
    /* the actions */
    float ya = y0 + 132 + ((n + 6) / 7) * 31 + 6;
    swprintf(lab, 120, L"Actions for %ls", state_wname(st));
    tx(l, ya, r, ya + 20, lab, g_sf_label, S_LABEL, DT_LEFT);
    float aw = (r - l - 7 * 5) / 8;
    for (int w = 1; w < W_N; w++) {
        float a = l + ((w - 1) % 8) * (aw + 5), y = ya + 24 + ((w - 1) / 8) * 31;
        mot_chip(pn, a, y, a + aw, y + 26, (mm->act >> w) & 1, 0, 1, w, WHIMS[w].name);
    }
    /* what the chip under the pointer does, or a summary */
    float yd = ya + 24 + ((W_N - 2) / 8 + 1) * 31 + 2;
    char line[400];
    if (g_hot >= 0 && (g_hits[g_hot].kind == K_MTOG || g_hits[g_hot].kind == K_MPLAY)) {
        int id = g_hits[g_hot].val;
        if (g_hits[g_hot].row == 0) {
            char home[60] = "";
            if (ROUTINES[id].home != st) { char hn[40]; WideCharToMultiByte(CP_UTF8, 0, state_wname(ROUTINES[id].home), -1, hn, 40, NULL, NULL); snprintf(home, sizeof home, " (made for %s)", hn); }
            snprintf(line, sizeof line, "%s%s: %s", ROUTINES[id].name, home, ROUTINES[id].about);
        } else snprintf(line, sizeof line, "%s: %s", WHIMS[id].name, WHIMS[id].about ? WHIMS[id].about : "");
        txa(l, yd, r, yd + 20, line, g_sf_small, S_LABEL, DT_LEFT | DT_END_ELLIPSIS);
    } else if (wall() < g_mot_msg_until) tx(l, yd, r, yd + 20, L"A state needs at least one routine: switch another on first.", g_sf_small, 0xFFB877, DT_LEFT);
    else {
        wchar_t sum[200];
        int nr = bits(mm->rt), na = bits(mm->act);
        swprintf(sum, 200, L"%ls uses %d routine%ls and %d action%ls. Changes apply the next time a state starts, and are kept.", state_wname(st), nr, nr == 1 ? L"" : L"s", na, na == 1 ? L"" : L"s");
        tx(l, yd, r, yd + 20, sum, g_sf_small, S_MUTED, DT_LEFT);
    }
}

/* the still layer: everything but the moving cluster */
static void set_build(void) {
    set_fonts();
    tip_fonts();
    Pane *pn = &g_sl;
    int W = SW_W(), H = SW_H();
    pane_size(pn, W, H);
    memset(pn->px, 0, (size_t)W * H * 4);
    g_hits_n = 0; g_tx_n = 0;
    box(pn, 0, 0, 880, 830, 18, S_BG, 0x2A2A30, 1);
    /* title and close */
    tx(28, 14, 400, 50, L"Settings", g_sf_title, S_TEXT, DT_LEFT);
    static const wchar_t *tabs[2] = {L"General", L"Motions"};
    for (int i = 0; i < 2; i++) { /* the two pages, as tabs: the open one ringed in orange like a chosen chip, the other plain text */
        float a = 132 + i * 96;
        int on = g_tab == i, hot = is_hot(K_TAB, 0, i);
        if (on) box(pn, a, 19, a + 88, 45, 13, S_CHIP, S_OR, 1.5f);
        else if (hot) box(pn, a, 19, a + 88, 45, 13, S_CHIP_HOT, 0, 0);
        tx(a, 19, a + 88, 45, tabs[i], g_sf_label, on ? 0xFFFFFF : hot ? S_TEXT : 0xC4C7CF, DT_CENTER);
        hit(a, 19, a + 88, 45, K_TAB, 0, i);
        if (!on && !(i + 1 < 2 && g_tab == i + 1)) box(pn, a + 93, 24, a + 94, 40, 0, S_EDGE, 0, 0); /* a thin divider after it */
    }
    int ch = is_hot(K_CLOSE, 0, 0);
    box(pn, 824, 16, 856, 48, 10, ch ? S_CLOSE_HOT : 0x26262B, 0, 0);
    aa_seg(pn, S(835), S(27), S(845), S(37), S(1.6f), hexc(ch ? 0xFFFFFF : S_LABEL), 1);
    aa_seg(pn, S(845), S(27), S(835), S(37), S(1.6f), hexc(ch ? 0xFFFFFF : S_LABEL), 1);
    hit(824, 16, 856, 48, K_CLOSE, 0, 0);
    /* the preview band: the cluster on the left; a reply in the text box's font, the hint and the states on the right */
    box(pn, 24, 58, 856, 288, 16, S_BAND, 0, 0);
    g_pv_rc = SR(40, 62, 564, 284);
    TEXTMETRICW tm;
    HDC mdc = pn->mem;
    SelectObject(mdc, g_tip_tf); GetTextMetricsW(mdc, &tm);
    float th = tm.tmHeight / (float)g_dpi;
    SelectObject(mdc, g_tip_bf); GetTextMetricsW(mdc, &tm);
    float bh = tm.tmHeight / (float)g_dpi, sh = fminf(112, 22 + th + 6 + bh * 2);
    box(pn, 580, 74, 840, 74 + sh, 10, tip_bgx(), 0, 0);
    box(pn, 580, 74, 583, 74 + sh, 1.5f, S_OR, 0, 0);
    tx(594, 84, 832, 84 + th, L"vigil · Done", g_tip_tf, inkx(0xFFB877), DT_LEFT | DT_END_ELLIPSIS);
    tx(594, 84 + th + 6, 832, 74 + sh - 8, L"The satellites slide round the orbit and trail behind.", g_tip_bf, text_bodyx(), DT_WORDBREAK | DT_END_ELLIPSIS);
    wchar_t hint[80];
    if (g_pv_zoom > 1.01f) swprintf(hint, 80, L"Scroll to zoom: %.1fx   \x00B7   double-click resets", g_pv_zoom);
    else wcscpy(hint, L"Drag the core   \x00B7   scroll on it to zoom in");
    tx(580, 190, 840, 210, hint, g_sf_label, S_MUTED, DT_LEFT);
    for (int i = 0; i < 6; i++) {
        float x0 = 580 + (i % 3) * (260 + 6) / 3.0f, y0 = 216 + (i / 3) * 34;
        float x1 = x0 + (260 - 12) / 3.0f;
        int on = PV_STATES[i] == g_pv_st, hot = is_hot(K_STATE, 0, i);
        box(pn, x0, y0, x1, y0 + 28, 14, hot ? S_CHIP_HOT : S_CHIP, S_OR, on ? 1.5f : 0);
        tx(x0, y0, x1, y0 + 28, PV_NAMES[i], g_sf_label, on ? S_TEXT : S_CHIP_TXT, DT_CENTER);
        hit(x0, y0, x1, y0 + 28, K_STATE, 0, i);
    }
    if (g_tab == 0) {
        /* the cards */
        const float y0 = 304, cw = (832 - 32) / 3.0f;
        float cx[3];
        for (int k = 0; k < 3; k++) { cx[k] = 24 + k * (cw + 16); box(pn, cx[k], y0, cx[k] + cw, 770, 16, S_CARD, 0, 0); }
        static const wchar_t *titles[3] = {L"Look", L"Motion", L"Replies"};
        for (int k = 0; k < 3; k++) tx(cx[k] + 16, y0 + 14, cx[k] + cw - 16, y0 + 36, titles[k], g_sf_card, S_TEXT, DT_LEFT);
        /* Look: shape, glow, size on screen, inner core */
        float l = cx[0] + 16, r = cx[0] + cw - 16, tw = (r - l - 18) / 4;
        tx(l, y0 + 46, r, y0 + 64, L"Shape", g_sf_label, S_LABEL, DT_LEFT);
        static const wchar_t *shapes[4] = {L"Dodecahedron", L"Buckyball", L"Geodesic", L"Fine geodesic"};
        for (int i = 0; i < 4; i++) {
            float a = l + i * (tw + 6);
            int sel = set_current(R_SHAPE) == i;
            pick_box(pn, a, y0 + 70, a + tw, y0 + 116, 12, sel, is_hot(K_PICK, R_SHAPE, i), NULL, NULL);
            shape_icon(pn, i, a + tw / 2, y0 + 93, sel ? S_OR : S_MUTED);
            hit(a, y0 + 70, a + tw, y0 + 116, K_PICK, R_SHAPE, i);
        }
        int hs = g_hot >= 0 && g_hits[g_hot].kind == K_PICK && g_hits[g_hot].row == R_SHAPE;
        tx(l + 60, y0 + 46, r, y0 + 64, hs ? shapes[g_hits[g_hot].val] : shapes[set_current(R_SHAPE) < 0 ? 2 : set_current(R_SHAPE)], g_sf_small, S_MUTED, DT_RIGHT);
        tx(l, y0 + 128, r, y0 + 146, L"Glow", g_sf_label, S_LABEL, DT_LEFT);
        aa_disc(pn, S(l + 4), S(y0 + 164), S(3.5f), hexc(S_OR), 0.45f);
        slider(pn, l + 20, y0 + 164, r - 22, R_GLOW, 3);
        aa_disc(pn, S(r - 5), S(y0 + 164), S(10), hexc(S_OR), 0.2f);
        aa_disc(pn, S(r - 5), S(y0 + 164), S(5.5f), hexc(S_OR), 1);
        tx(l, y0 + 186, r, y0 + 204, L"Size on screen", g_sf_label, S_LABEL, DT_LEFT);
        tx(l - 2, y0 + 212, l + 12, y0 + 232, L"S", g_sf_small, S_MUTED, DT_CENTER);
        slider(pn, l + 20, y0 + 222, r - 22, R_SCALE, 4);
        tx(r - 14, y0 + 210, r + 2, y0 + 234, L"L", g_sf_text, S_MUTED, DT_CENTER);
        tx(l, y0 + 244, r, y0 + 262, L"Inner core", g_sf_label, S_LABEL, DT_LEFT);
        aa_disc(pn, S(l + 4), S(y0 + 280), S(2.5f), hexc(S_KNOB), 0.8f);
        slider(pn, l + 20, y0 + 280, r - 22, R_CORE, 4);
        aa_disc(pn, S(r - 5), S(y0 + 280), S(5.5f), hexc(S_KNOB), 0.8f);
        /* Motion: gravity, other sessions, focus frame, what the wheel does */
        l = cx[1] + 16; r = cx[1] + cw - 16;
        tx(l, y0 + 46, r, y0 + 64, L"Gravity", g_sf_label, S_LABEL, DT_LEFT);
        slider(pn, l + 8, y0 + 82, r - 8, R_GRAVITY, 4);
        tx(l, y0 + 94, r, y0 + 112, L"Floaty", g_sf_small, S_MUTED, DT_LEFT);
        tx(l, y0 + 94, r, y0 + 112, L"Firm", g_sf_small, S_MUTED, DT_RIGHT);
        tx(l, y0 + 124, r - 56, y0 + 142, L"Other sessions", g_sf_text, S_TEXT, DT_LEFT);
        tx(l, y0 + 142, r - 56, y0 + 160, L"Small cores around the main one", g_sf_small, S_MUTED, DT_LEFT);
        int on = g_others, ht = is_hot(K_TOGGLE, R_OTHERS, 0);
        box(pn, r - 42, y0 + 130, r, y0 + 154, 12, on ? S_OR : (ht ? S_EDGE_HOT : S_EDGE), 0, 0);
        aa_disc(pn, S(on ? r - 12 : r - 30), S(y0 + 142), S(9), hexc(on ? S_ON_OR : S_LABEL), 1);
        hit(l, y0 + 122, r, y0 + 162, K_TOGGLE, R_OTHERS, 0);
        tx(l, y0 + 176, r, y0 + 194, L"Focus frame", g_sf_label, S_LABEL, DT_LEFT);
        float fw = (r - l - 12) / 3;
        for (int i = 0; i < 7; i++) {
            float a = l + (i % 3) * (fw + 6), b = y0 + 200 + (i / 3) * 34;
            pick_box(pn, a, b, a + fw, b + 28, 14, set_current(R_FRAME) == i, is_hot(K_PICK, R_FRAME, i), FRAME_NAMES[i], g_sf_label);
            hit(a, b, a + fw, b + 28, K_PICK, R_FRAME, i);
        }
        tx(l, y0 + 314, r, y0 + 332, L"Scroll on a core", g_sf_label, S_LABEL, DT_LEFT);
        for (int i = 0; i < 5; i++) {
            float a = l + (i % 3) * (fw + 6), b = y0 + 338 + (i / 3) * 34;
            pick_box(pn, a, b, a + fw, b + 28, 14, set_current(R_WHEEL) == i, is_hot(K_PICK, R_WHEEL, i), WHEEL_NAMES[i], g_sf_label);
            hit(a, b, a + fw, b + 28, K_PICK, R_WHEEL, i);
        }
        /* Replies: font, text size, box size, colours (box and text), where the box opens, labels */
        l = cx[2] + 16; r = cx[2] + cw - 16;
        float bw = (r - l - 6) / 2;
        tx(l, y0 + 46, r, y0 + 64, L"Font", g_sf_label, S_LABEL, DT_LEFT);
        for (int i = 0; i < 2; i++) {
            float a = l + i * (bw + 6);
            int sel = set_current(R_TIPFONT) == i;
            pick_box(pn, a, y0 + 70, a + bw, y0 + 118, 12, sel, is_hot(K_PICK, R_TIPFONT, i), NULL, NULL);
            tx(a, y0 + 72, a + bw, y0 + 100, L"Aa", i ? g_sf_aa : g_sf_aam, sel ? S_TEXT : S_CHIP_TXT, DT_CENTER);
            tx(a, y0 + 98, a + bw, y0 + 114, i ? L"Clean" : L"Terminal", g_sf_small, sel ? S_TEXT : S_MUTED, DT_CENTER);
            hit(a, y0 + 70, a + bw, y0 + 118, K_PICK, R_TIPFONT, i);
        }
        tx(l, y0 + 130, r, y0 + 148, L"Text size", g_sf_label, S_LABEL, DT_LEFT);
        tx(l - 2, y0 + 156, l + 12, y0 + 176, L"A", g_sf_small, S_MUTED, DT_CENTER);
        slider(pn, l + 20, y0 + 166, r - 22, R_TEXT, 5);
        tx(r - 16, y0 + 154, r + 2, y0 + 180, L"A", g_sf_card, S_MUTED, DT_CENTER);
        tx(l, y0 + 190, r, y0 + 208, L"Box size", g_sf_label, S_LABEL, DT_LEFT);
        box(pn, l, y0 + 220, l + 10, y0 + 228, 2, S_MUTED, 0, 0);
        slider(pn, l + 20, y0 + 224, r - 22, R_TIPSIZE, 4);
        box(pn, r - 12, y0 + 216, r + 2, y0 + 232, 3, S_MUTED, 0, 0);
        static const wchar_t *opens[2] = {L"Next to core", L"Middle"}, *labels[2] = {L"On hover", L"Always"};
        tx(l, y0 + 248, r, y0 + 266, L"Colours", g_sf_label, S_LABEL, DT_LEFT);
        int hb = g_hot >= 0 && g_hits[g_hot].kind == K_PICK && g_hits[g_hot].row == R_TIPBG, ht2 = g_hot >= 0 && g_hits[g_hot].kind == K_PICK && g_hits[g_hot].row == R_TEXTCOL;
        wchar_t names[60];
        swprintf(names, 60, L"%ls  \x00B7  %ls", TIP_BG_NAMES[hb ? g_hits[g_hot].val : g_tip_bg], TEXT_NAMES[ht2 ? g_hits[g_hot].val : g_text_col]);
        tx(l + 60, y0 + 248, r, y0 + 266, names, g_sf_small, S_MUTED, DT_RIGHT);
        float sl = l + 38, sw = (r - sl - 5 * 6) / 6;
        for (int row = 0; row < 2; row++) { /* Box: the default swatches, then the palette; Text: "Aa" in each colour on the box's colour, then the palette */
            float yy = y0 + 272 + row * 34;
            tx(l, yy, sl - 4, yy + 26, row ? L"Text" : L"Box", g_sf_small, S_MUTED, DT_LEFT);
            for (int i = 0; i < 6; i++) {
                float a = sl + i * (sw + 6);
                int rr = row ? R_TEXTCOL : R_TIPBG, sel = set_current(rr) == i, hot = is_hot(K_PICK, rr, i);
                unsigned fill = row ? tip_bgx() : i < 5 ? TIP_BGS[i] : g_tip_custom;
                box(pn, a, yy, a + sw, yy + 26, 7, fill, sel ? S_OR : hot ? S_EDGE_HOT : S_EDGE, sel ? 1.5f : 1);
                unsigned tc = row ? (i == 5 ? g_text_custom : i ? TEXT_COLS[i] : inkx(0xE6E8F0)) : 0;
                if (i == 5) { /* Lucide's palette icon: on Box light on a dark colour and dark on a light one, on Text in the picked colour */
                    unsigned v = fill;
                    int light = 0.299f * (v >> 16 & 255) + 0.587f * (v >> 8 & 255) + 0.114f * (v & 255) > 140;
                    palette_icon(pn, S(a + sw / 2), S(yy + 13), S(16), row ? tc : light ? 0x2A2A30 : (hot || sel ? 0xFFFFFF : 0xD4D4DA));
                } else if (row) tx(a, yy, a + sw, yy + 26, L"Aa", g_sf_small, tc, DT_CENTER);
                hit(a, yy, a + sw, yy + 26, K_PICK, rr, i);
            }
        }
        tx(l, y0 + 342, r, y0 + 360, L"Opens", g_sf_label, S_LABEL, DT_LEFT);
        for (int i = 0; i < 2; i++) {
            float a = l + i * (bw + 6);
            pick_box(pn, a, y0 + 364, a + bw, y0 + 392, 10, set_current(R_TIPAT) == i, is_hot(K_PICK, R_TIPAT, i), opens[i], g_sf_label);
            hit(a, y0 + 364, a + bw, y0 + 392, K_PICK, R_TIPAT, i);
        }
        tx(l, y0 + 404, r, y0 + 422, L"Names over cores (all-cores mode)", g_sf_label, S_LABEL, DT_LEFT);
        for (int i = 0; i < 2; i++) {
            float a = l + i * (bw + 6);
            pick_box(pn, a, y0 + 426, a + bw, y0 + 454, 10, set_current(R_LABELS) == i, is_hot(K_PICK, R_LABELS, i), labels[i], g_sf_label);
            hit(a, y0 + 426, a + bw, y0 + 454, K_PICK, R_LABELS, i);
        }
    } else motions_page(pn);
    /* the foot: standby time, reset */
    int aw = set_current(R_AWAY);
    tx(28, 784, 120, 816, L"Rest after", g_sf_label, S_MUTED, DT_LEFT);
    int ha = is_hot(K_CYCLE, R_AWAY, 0);
    box(pn, 92, 787, 184, 813, 13, ha ? S_CHIP_HOT : S_CHIP, 0, 0);
    tx(92, 787, 184, 813, aw >= 0 ? AWAY_TEXT[aw] : L"custom", g_sf_label, S_TEXT, DT_CENTER);
    hit(92, 787, 184, 813, K_CYCLE, R_AWAY, 0);
    tx(192, 784, 420, 816, aw == 3 ? L"(cores never rest)" : L"away: cores show STANDBY", g_sf_label, S_MUTED, DT_LEFT);
    int can = settings_saved(), hr = is_hot(K_RESET, 0, 0);
    tx(640, 784, 856, 816, L"Reset to defaults", g_sf_label, can ? (hr ? 0xFFB877 : S_OR) : 0x55555E, DT_RIGHT);
    if (can) hit(740, 788, 856, 812, K_RESET, 0, 0);
    /* the text, then everything inside the edge made opaque again (GDI leaves its alpha at 0) */
    GdiFlush();
    SetBkMode(mdc, TRANSPARENT);
    for (int i = 0; i < g_tx_n; i++) {
        TextOp *o = &g_tx[i];
        SelectObject(mdc, o->f);
        SetTextColor(mdc, o->c);
        UINT f = o->how & DT_WORDBREAK ? o->how | DT_NOPREFIX : o->how | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX;
        DrawTextW(mdc, o->t, -1, &o->r, f);
    }
    GdiFlush();
    int rad = (int)ceilf(S(18)) + 1;
    for (int y = 0; y < H; y++) {
        int corner = y < rad || y >= H - rad;
        uint32_t *p = pn->px + (size_t)y * W;
        for (int x = corner ? rad : 0; x < (corner ? W - rad : W); x++) p[x] |= 0xff000000u;
    }
    g_sl_dirty = 0;
}

/* each tick while it is open: the preview moves, the window is shown again */
static void settings_tick(double now) {
    if (!set_open()) return;
    float dt = (float)fmin(0.1, now - g_pv_last);
    g_pv_last = now;
    int shown[4] = {g_pv_st, ST_WORK, ST_IDLE, ST_THINK};
    for (int i = 0; i < 4; i++) {
        Session *s = &g_pv_room.s[i];
        s->state = shown[i] == ST_SLEEP ? ST_IDLE : shown[i];
        s->overlay = shown[i] == ST_IDLE ? 0 : shown[i];
        s->last = now;
        s->aura_a = g_others ? TOUCH.aura : 0;
    }
    if (g_sl_dirty) set_build();
    /* the cluster in the preview, scaled as Size scales it on the screen */
    int pw = g_pv_rc.right - g_pv_rc.left, ph = g_pv_rc.bottom - g_pv_rc.top;
    float cx = pw / 2.0f, cy = ph / 2.0f, cw = fminf((float)pw, (float)ph) * 0.97f * (0.55f + 0.45f * (g_scale - 0.75f) / 0.85f) * g_pv_zoom;
    /* the same proportions as draw_single, on a cluster cw wide instead of g_cw */
    float big = cw * 0.6f, rx = cw / 2.0f - (float)(8 * g_dpi) * cw / g_cw, ry = rx * 1.15f;
    float small = g_others ? fminf(cw * 0.2f, (float)(2 * M_PI * rx / 3 * 0.75)) : 0;
    rx -= small / 2; ry -= small / 2;
    OrbitK ok = orbit_k(rx, small);
    Session *m = &g_pv_room.s[0];
    if (g_pv_mx == 0 && g_pv_my == 0) { g_pv_mx = cx; g_pv_my = cy; }
    if (g_pv_drag) {
        POINT p; GetCursorPos(&p); ScreenToClient(g_set, &p);
        float nx = p.x - g_pv_rc.left - g_pv_ox, ny = p.y - g_pv_rc.top - g_pv_oy;
        if (dt > 0) { g_pv_vx += ((nx - g_pv_mx) / dt - g_pv_vx) * 0.45f; g_pv_vy += ((ny - g_pv_my) / dt - g_pv_vy) * 0.45f; }
        g_pv_mx = nx; g_pv_my = ny;
    } else { /* let go: it glides back to the middle */
        float f = 1 - expf(-dt * 5);
        g_pv_mx += (cx - g_pv_mx) * f; g_pv_my += (cy - g_pv_my) * f;
        g_pv_vx *= 0.8f; g_pv_vy *= 0.8f;
    }
    m->carried = g_pv_drag;
    m->move_x = g_pv_vx / big; m->move_y = g_pv_vy / big;
    int frame_hot = g_hot >= 0 && g_hits[g_hot].row == R_FRAME;
    m->lock_to = g_pv_drag || frame_hot || now < g_pv_lock_until ? 1.0f : 0.0f;
    m->hover_to = g_pv_drag;
    room_tick(&g_pv_room, &g_shape, now, dt);
    if (!g_pv_drag) g_pv_orbit += dt;
    Orbit *ring[3];
    int rn = 0;
    for (int i = 1; i < 4; i++) if (g_pv_ob[i].init && !g_pv_ob[i].free) ring[rn++] = &g_pv_ob[i];
    for (int i = 1; i < 4; i++) {
        Orbit *ob = &g_pv_ob[i];
        double a = M_PI / 2 + 2 * M_PI * (i - 1) / 3 + g_pv_orbit * 0.10;
        float bob = (float)(3 * g_dpi * sin(g_pv_orbit * 0.8 + i * 1.7));
        float tx = g_pv_mx + rx * (float)cos(a), ty = g_pv_my - ry * (float)sin(a) + bob;
        if (!ob->init) { ob->px = tx; ob->py = ty; ob->vx = ob->vy = 0; ob->w = small; ob->init = 1; ob->free = 0; }
        ob->w += (small - ob->w) * (1 - expf(-dt * 7));
        if (ob->free) { /* loose: it bounces round the preview until the core comes close or it has lain still a while */
            fly_step(ob, dt);
            float hs = ob->w / 2;
            if (ob->px < hs) { ob->px = hs; ob->vx = fabsf(ob->vx) * BOUNCE; }
            if (ob->px > pw - hs) { ob->px = pw - hs; ob->vx = -fabsf(ob->vx) * BOUNCE; }
            if (ob->py < hs) { ob->py = hs; ob->vy = fabsf(ob->vy) * BOUNCE; }
            if (ob->py > ph - hs) { ob->py = ph - hs; ob->vy = -fabsf(ob->vy) * BOUNCE; }
            if (hypotf(ob->px - g_pv_mx, ob->py - g_pv_my) < ok.CATCH || (!g_pv_drag && ob->rest > 1.5f)) ob->free = 0;
        } else if (bead_step(ob, g_pv_mx, g_pv_my + bob, tx, ty, a, rx, ry, small, g_pv_drag ? 0 : ok.KT, &ok, ring, rn, dt) > ok.BREAK)
            ob->free = 1, ob->rest = 0;
    }
    /* the cluster's picture: the band's colour, the small cores, the main one and its frame on top */
    pane_size(&g_pv, pw, ph);
    uint32_t bg = 0xff000000u | S_BAND;
    for (size_t j = 0; j < (size_t)pw * ph; j++) g_pv.px[j] = bg;
    g_blit_add = 1;
    for (int i = 1; i < 4 && g_others; i++) {
        int w4 = ((int)g_pv_ob[i].w / 4) * 4;
        if (w4 < 8) continue;
        pic_make(&g_pv_pic[i], &g_pv_room.s[i], w4, now, 1);
        blit_pic_at(&g_pv, g_pv_ob[i].px - w4 / 2.0f, g_pv_ob[i].py - w4 / 2.0f, &g_pv_pic[i]);
    }
    int w4 = ((int)big / 4) * 4;
    pic_make(&g_pv_pic[0], m, w4, now, 3); /* the one you watch: every frame */
    float fx = g_pv_mx - w4 / 2.0f, fy = g_pv_my - w4 / 2.0f;
    blit_pic_at(&g_pv, fx, fy, &g_pv_pic[0]);
    g_blit_add = 0;
    RECT lr = {(int)fx, (int)fy, (int)fx + w4, (int)fy + w4};
    float real = (float)(((int)(g_cw * 0.6f) / 4) * 4); /* the main core's side on the screen */
    g_frame_k = frame_k(real) * w4 / fmaxf(1, real); /* its frame, as much smaller as the preview is */
    focus_frame(&g_pv, lr, m, now);
    g_frame_k = 0;
    /* the window: the still layer with the cluster laid in */
    int W = g_sl.W, H = g_sl.H;
    pane_size(&g_so, W, H);
    memcpy(g_so.px, g_sl.px, (size_t)W * H * 4);
    for (int y = 0; y < ph; y++) {
        int Y = g_pv_rc.top + y;
        if (Y < 0 || Y >= H) continue;
        uint32_t *d = g_so.px + (size_t)Y * W + g_pv_rc.left, *s = g_pv.px + (size_t)y * pw;
        for (int x = 0; x < pw && g_pv_rc.left + x < W; x++) d[x] = s[x] | 0xff000000u;
    }
    RECT wr; GetWindowRect(g_set, &wr);
    POINT src = {0, 0}, dst = {wr.left, wr.top};
    SIZE size = {W, H};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(g_set, NULL, &dst, &size, g_so.mem, &src, 0, &blend, ULW_ALPHA);
}

static int set_hit_at(POINT p) { for (int i = g_hits_n - 1; i >= 0; i--) if (PtInRect(&g_hits[i].r, p)) return i; return -1; }

static void set_slide_to(int h, int x) {
    SetHit *sh = &g_hits[h];
    float u = (x - sh->t0) / fmaxf(1, sh->t1 - sh->t0);
    int v = (int)lroundf(fminf(1, fmaxf(0, u)) * (sh->n - 1));
    set_choose(sh->row, v);
}

static void set_close(void) { if (g_set) ShowWindow(g_set, SW_HIDE); g_pv_drag = 0; g_slide = -1; }

static LRESULT CALLBACK set_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    switch (msg) {
    case WM_NCHITTEST: { /* the top strip moves the window */
        POINT c = p; ScreenToClient(w, &c);
        if (c.y < (int)S(52) && c.x < (int)S(816) && set_hit_at(c) < 0) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_MOUSEMOVE: {
        if (!g_tracking) { TRACKMOUSEEVENT te = {sizeof te, TME_LEAVE, w, 0}; TrackMouseEvent(&te); g_tracking = 1; }
        if (g_slide >= 0) { set_slide_to(g_slide, p.x); return 0; }
        int h = set_hit_at(p);
        if (h != g_hot) { g_hot = h; g_sl_dirty = 1; }
        return 0;
    }
    case WM_MOUSELEAVE: g_tracking = 0; if (g_hot >= 0) { g_hot = -1; g_sl_dirty = 1; } return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT c; GetCursorPos(&c); ScreenToClient(w, &c);
            SetCursor(LoadCursor(NULL, set_hit_at(c) >= 0 || PtInRect(&g_pv_rc, c) ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONDBLCLK: /* on the preview: zoom back to 1; anywhere else a second press like the first */
        if (PtInRect(&g_pv_rc, p) && set_hit_at(p) < 0) { g_pv_zoom = 1; g_pv_mx = g_pv_my = 0; g_sl_dirty = 1; return 0; }
        /* fall through */
    case WM_LBUTTONDOWN: {
        int h = set_hit_at(p);
        if (h >= 0) {
            SetHit *sh = &g_hits[h];
            switch (sh->kind) {
            case K_PICK: set_choose(sh->row, sh->val); break;
            case K_TOGGLE: set_choose(sh->row, 0); break;
            case K_SLIDE: g_slide = h; SetCapture(w); set_slide_to(h, p.x); g_sl_dirty = 1; break;
            case K_CYCLE: { int c = set_current(sh->row); set_choose(sh->row, (c + 1) % 4); break; }
            case K_STATE: g_pv_st = PV_STATES[sh->val]; g_sl_dirty = 1; break;
            case K_TAB: g_tab = sh->val; g_sl_dirty = 1; break;
            case K_MSTATE: g_pv_st = sh->val; g_sl_dirty = 1; break;
            case K_MTOG: mot_toggle(sh->row, sh->val); break;
            case K_MPLAY: mot_play(sh->row, sh->val); break;
            case K_RESET: set_reset(); break;
            case K_CLOSE: g_close_armed = 1; SetCapture(w); break; /* it closes when the button is let go over it */
            }
            return 0;
        }
        if (PtInRect(&g_pv_rc, p)) { /* pick the preview core up (pressed off it: it jumps under the pointer) */
            g_pv_ox = p.x - g_pv_rc.left - g_pv_mx; g_pv_oy = p.y - g_pv_rc.top - g_pv_my;
            if (hypotf(g_pv_ox, g_pv_oy) > (g_pv_rc.bottom - g_pv_rc.top) * 0.25f) g_pv_ox = g_pv_oy = 0;
            g_pv_drag = 1;
            SetCapture(w);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        int h = set_hit_at(p), close = g_close_armed && h >= 0 && g_hits[h].kind == K_CLOSE;
        if (g_pv_drag || g_slide >= 0 || g_close_armed) { g_pv_drag = 0; g_slide = -1; g_close_armed = 0; g_sl_dirty = 1; ReleaseCapture(); }
        if (close) set_close();
        return 0;
    }
    case WM_CAPTURECHANGED: g_pv_drag = 0; g_close_armed = 0; if (g_slide >= 0) { g_slide = -1; g_sl_dirty = 1; } return 0;
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_NCRBUTTONDOWN: case WM_NCRBUTTONUP: case WM_CONTEXTMENU:
        tip_hide(); return 0; /* a right-click here is the window's own: no menu, nothing for the cores under it */
    case WM_MOUSEWHEEL: { /* over the preview: zoom in to see the detail */
        POINT c = p; ScreenToClient(w, &c);
        if (!PtInRect(&g_pv_rc, c)) return 0;
        g_pv_zoom = clampf(g_pv_zoom * powf(1.2f, GET_WHEEL_DELTA_WPARAM(wp) / 120.0f), 1, 3);
        if (g_pv_zoom < 1.05f) g_pv_zoom = 1;
        g_pv_mx = g_pv_my = 0; /* centred again */
        g_sl_dirty = 1;
        return 0;
    }
    case WM_KEYDOWN: if (wp == VK_ESCAPE) { set_close(); return 0; } break;
    case WM_CLOSE: set_close(); return 0;
    }
    return DefWindowProc(w, msg, wp, lp);
}

/* opens it in the middle of the monitor the pointer is on (or brings it back to the front) */
static void settings_open(void) {
    double now = wall();
    pv_init(now);
    if (!g_set) {
        WNDCLASS sc;
        memset(&sc, 0, sizeof sc);
        sc.lpfnWndProc = set_proc;
        sc.hInstance = GetModuleHandle(NULL);
        sc.hCursor = LoadCursor(NULL, IDC_ARROW);
        sc.lpszClassName = L"VigilSettings";
        sc.style = CS_DBLCLKS;
        RegisterClass(&sc);
        g_set = CreateWindowEx(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"VigilSettings", L"Vigil settings", WS_POPUP, 0, 0, 10, 10, NULL, NULL, sc.hInstance, NULL);
        g_sl.mem = CreateCompatibleDC(NULL);
        g_so.mem = CreateCompatibleDC(NULL);
        g_pv.mem = CreateCompatibleDC(NULL);
    }
    if (!IsWindowVisible(g_set)) {
        POINT c; GetCursorPos(&c); /* the monitor the pointer is on, or the one --on names */
        if (g_on_set) c = g_on;
        RECT wa = work_at((float)c.x, (float)c.y);
        int W = SW_W(), H = SW_H();
        SetWindowPos(g_set, HWND_TOPMOST, (wa.left + wa.right - W) / 2, (wa.top + wa.bottom - H) / 2, W, H, SWP_NOACTIVATE);
        for (int i = 1; i < 4; i++) g_pv_ob[i].init = 0;
        g_pv_mx = g_pv_my = 0;
        g_pv_last = now;
        g_sl_dirty = 1;
        g_hot = -1;
        ShowWindow(g_set, g_set_shot[0] || g_rec_scene[0] ? SW_SHOWNA : SW_SHOW); /* (a screenshot shows it without taking the foreground) */
        settings_tick(now);
    }
    if (g_set_shot[0] || g_rec_scene[0]) return; /* a screenshot never takes the keyboard (a game may be running) */
    mask_alt();
    SetForegroundWindow(g_set);
    SetFocus(g_set);
}

/* --settings-shot: the window as it is shown, to a 32-bit BMP (to check a change without looking at the screen) */
static void settings_shot(const char *path) {
    int W = g_so.W, H = g_so.H;
    FILE *f = fopen(path, "wb");
    if (!f || !g_so.px) { if (f) fclose(f); return; }
    BITMAPINFOHEADER bh;
    memset(&bh, 0, sizeof bh);
    bh.biSize = sizeof bh; bh.biWidth = W; bh.biHeight = -H; bh.biPlanes = 1; bh.biBitCount = 32; bh.biCompression = BI_RGB;
    BITMAPFILEHEADER fh = {0x4D42, (DWORD)(sizeof fh + sizeof bh + (size_t)W * H * 4), 0, 0, (DWORD)(sizeof fh + sizeof bh)};
    fwrite(&fh, sizeof fh, 1, f); fwrite(&bh, sizeof bh, 1, f); fwrite(g_so.px, (size_t)W * H * 4, 1, f);
    fclose(f);
}

/* ---- --record: two scripted scenes for the README, frames written raw ------------------------------------
 * "reply": the pointer rests on the main core (its text box, then lock-on), then on a small core.
 * "input": the wheel scrolls a long reply, a double-click opens the reply box, text is typed, an image is dragged onto
 * the core (it reads it) and dropped: an [Image #1] token. Nothing is sent.
 * "carry": Alt-click picks the cluster up, a slow move (the small cores trail behind), a dash toward the screen's corner
 * that turns away (they break off, fly on and bounce off its edges), then over to each one, which comes back to the
 * orbit, and put down. Recorded at Gravity: Floaty.
 * Each frame: int32 x, y (the pointer, in the frame), int32 shown (0 while carried: the pointer hides; 2 with an
 * image file under it), then W×H
 * BGR bytes of the cores and the text box over the terminal's background (#212121). Header: int32 W, H, fps. */
static RECT g_rec_rc;
static float rec_ease(float a) { a = clampf(a, 0, 1); return a * a * (3 - 2 * a); }
static void rec_glide(double t, double t0, double t1, float ax, float ay, float bx, float by) {
    if (t < t0 || t > t1 + 0.05) return;
    float e = rec_ease((float)((t - t0) / (t1 - t0)));
    g_rec_mouse.x = (LONG)lroundf(ax + (bx - ax) * e); g_rec_mouse.y = (LONG)lroundf(ay + (by - ay) * e);
}

static void rec_over(uint32_t *fr, int W, int H, const uint32_t *src, int sw, int sh, int ox, int oy, int opaque) {
    for (int y = 0; y < sh; y++) {
        int fy = oy + y;
        if (fy < 0 || fy >= H) continue;
        for (int x = 0; x < sw; x++) {
            int fx = ox + x;
            if (fx < 0 || fx >= W) continue;
            uint32_t sp = src[(size_t)y * sw + x], a = opaque ? 255 : sp >> 24, *d = &fr[(size_t)fy * W + fx], o = 0;
            if (!a && !opaque) continue;
            for (int k = 0; k < 24; k += 8) {
                uint32_t sc = sp >> k & 255, dc = *d >> k & 255, v = sc + dc * (255 - a) / 255;
                o |= (v > 255 ? 255 : v) << k;
            }
            *d = o | 0xFF000000u;
        }
    }
}

static void rec_frame(FILE *f) {
    int W = g_rec_rc.right - g_rec_rc.left, H = g_rec_rc.bottom - g_rec_rc.top;
    static uint32_t *fr;
    static uint8_t *bgr;
    if (!fr) { fr = malloc((size_t)W * H * 4); bgr = malloc((size_t)W * H * 3); }
    for (size_t i = 0; i < (size_t)W * H; i++) fr[i] = 0xFF212121u;
    for (int k = 0; k <= g_core_n; k++) { /* the small cores, then the main one on top */
        int i = k < g_core_n ? k : g_top;
        if (i < 0 || i >= MAX_SESS || (k < g_core_n && i == g_top)) continue;
        Pane *p = &g_pane[i];
        if (p->shown && p->px) rec_over(fr, W, H, p->px, p->W, p->H, p->x - g_rec_rc.left, p->y - g_rec_rc.top, 0);
    }
    if (g_tip && IsWindowVisible(g_tip)) { /* the text box paints itself into a bitmap of ours */
        RECT r;
        GetWindowRect(g_tip, &r);
        int tw = r.right - r.left, th = r.bottom - r.top;
        BITMAPINFO bi;
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = tw; bi.bmiHeader.biHeight = -th;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        uint32_t *tp = NULL;
        HDC mem = CreateCompatibleDC(NULL);
        HBITMAP bm = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, (void **)&tp, NULL, 0), old = SelectObject(mem, bm);
        SendMessage(g_tip, WM_PRINTCLIENT, (WPARAM)mem, PRF_CLIENT);
        GdiFlush();
        rec_over(fr, W, H, tp, tw, th, r.left - g_rec_rc.left, r.top - g_rec_rc.top, 1);
        SelectObject(mem, old); DeleteObject(bm); DeleteDC(mem);
    }
    if (g_input && IsWindowVisible(g_input)) { /* the reply box (a rich edit inside): as the compositor has it */
        RECT r;
        GetWindowRect(g_input, &r);
        int tw = r.right - r.left, th = r.bottom - r.top;
        BITMAPINFO bi;
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = tw; bi.bmiHeader.biHeight = -th;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        uint32_t *tp = NULL;
        HDC mem = CreateCompatibleDC(NULL);
        HBITMAP bm = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, (void **)&tp, NULL, 0), old = SelectObject(mem, bm);
        /* caught mid-repaint the rich edit comes out blank or half drawn: take it until two takes in a row agree */
        uint32_t *prev = malloc((size_t)tw * th * 4);
        RECT er; /* the text field, in the box's pixels */
        GetWindowRect(g_edit, &er);
        OffsetRect(&er, -r.left, -r.top);
        IntersectRect(&er, &er, &(RECT){0, 0, tw, th});
        int has_text = GetWindowTextLengthW(g_edit) > 0;
        for (int tries = 0; tries < 40; tries++) {
            RedrawWindow(g_input, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN); /* the text in it painted now, not later */
            PrintWindow(g_input, mem, 2 /* PW_RENDERFULLCONTENT */);
            GdiFlush();
            int flat = has_text;
            uint32_t c0 = tp[(size_t)er.top * tw + er.left] & 0xFFFFFF;
            for (int y = er.top; y < er.bottom && flat; y++) for (int x = er.left; x < er.right; x++) if ((tp[(size_t)y * tw + x] & 0xFFFFFF) != c0) { flat = 0; break; }
            if (tries && !flat && !memcmp(prev, tp, (size_t)tw * th * 4)) break;
            memcpy(prev, tp, (size_t)tw * th * 4);
            Sleep(3);
        }
        free(prev);
        HRGN rg = CreateRectRgn(0, 0, 0, 0);
        int has = GetWindowRgn(g_input, rg) != ERROR;
        for (int y = 0; y < th; y++) for (int x = 0; x < tw; x++) tp[(size_t)y * tw + x] = has && !PtInRegion(rg, x, y) ? 0 : tp[(size_t)y * tw + x] | 0xFF000000u; /* the round corners */
        DeleteObject(rg);
        rec_over(fr, W, H, tp, tw, th, r.left - g_rec_rc.left, r.top - g_rec_rc.top, 0);
        SelectObject(mem, old); DeleteObject(bm); DeleteDC(mem);
    }
    for (size_t i = 0; i < (size_t)W * H; i++) { bgr[i * 3] = fr[i] & 255; bgr[i * 3 + 1] = fr[i] >> 8 & 255; bgr[i * 3 + 2] = fr[i] >> 16 & 255; }
    int32_t head[3] = {g_rec_mouse.x - g_rec_rc.left, g_rec_mouse.y - g_rec_rc.top, g_carrying ? 0 : g_drag_img ? 2 : 1};
    fwrite(head, sizeof head, 1, f);
    fwrite(bgr, 1, (size_t)W * H * 3, f);
}

static void rec_run(void) {
    FILE *f = fopen(g_rec_file, "wb");
    if (!f) { fprintf(stderr, "vigil-panel: cannot write %s\n", g_rec_file); return; }
    int carry = !strcmp(g_rec_scene, "carry"), input = !strcmp(g_rec_scene, "input");
    g_single = 1; g_others = 1; g_focus_pinned = 0;
    snprintf(g_frame, sizeof g_frame, "diamond");
    if (carry) g_gravity = GRAVITIES[0]; /* Floaty: a flick breaks them loose */
    int W = (int)((carry ? 1300 : 1000) * g_dpi), H = (int)((carry ? 560 : input ? 620 : 440) * g_dpi), m = carry ? 0 : (int)(40 * g_dpi);
    RECT wk = g_on_set ? work_at((float)g_on.x, (float)g_on.y) : g_work; /* --on: that monitor */
    SetRect(&g_rec_rc, wk.right - m - W, wk.bottom - m - H, wk.right - m, wk.bottom - m);
    float L = (float)g_rec_rc.left, T = (float)g_rec_rc.top;
    g_mx = L + (carry ? .5f : .8f) * W; g_my = T + (input ? .36f : .5f) * H;
    float cx = g_mx, cy = g_my;
    g_rec_mouse.x = (LONG)(L + .2f * W); g_rec_mouse.y = (LONG)(T + .9f * H);
    if (carry) g_rec_rc.top -= (int)(110 * g_dpi); /* room above: the dash turns up near the top */
    int32_t head[3] = {g_rec_rc.right - g_rec_rc.left, g_rec_rc.bottom - g_rec_rc.top, 15};
    fwrite(head, sizeof head, 1, f);
    const double WARM = 17.5, END = carry ? 11.5 : input ? 13.0 : 8.6; /* the demo runs a while first, so all three sessions are there */
    double t0 = g_rec_now;
    int sat = -1, picked = 0, dropped = 0;
    float sx = 0, sy = 0;
    MSG msg;
    for (long fno = 0;; fno++) {
        g_rec_now = t0 + fno / 30.0;
        double t = fno / 30.0 - WARM;
        if (t >= 0 && !g_focus_pinned) { /* the same session in the middle all through (input: one with a reply) */
            g_focus_pinned = 1;
            if (input) { snprintf(g_focus, sizeof g_focus, "demo-0"); g_rec_still = 1; }
        }
        if (input && t >= 0) {
            Room *room = shown_room();
            int fi = index_of(g_focus);
            Session *s = fi >= 0 ? &room->s[fi] : NULL;
            float kx = cx - g_cw * .06f, ky = cy - g_cw * .04f, ox = L + .12f * W, oy = T + .93f * H;
            static const char *TYPE1 = "Looks right. Plot BER against the reference ", *TYPE2 = "and tell me if it still matches at 12 dB.";
            static int typed, typed2;
            rec_glide(t, 0.2, 0.9, L + .2f * W, T + .9f * H, kx, ky);
            for (int k = 0; k < 6 && s; k++) { /* the wheel: down four notches, up two (the core spins with it) */
                double at = 1.4 + k * .45;
                if (t >= at && t < at + 1 / 30.0) {
                    float notches = k < 4 ? -1.0f : 1.0f;
                    wheel_spin(s, notches);
                    if (!strcmp(g_tip_sid, s->sid)) tip_scroll(notches);
                }
            }
            if (s && t >= 4.6 && t < 4.6 + 1 / 30.0) { s->ring_at = wall(); s->unread = 0; input_open_for(s); } /* double-click */
            const char *line = t < 8 ? TYPE1 : TYPE2;
            int *at = t < 8 ? &typed : &typed2;
            if (g_edit && t >= (t < 8 ? 5.0 : 11.0) && fno % 2 == 0 && line[*at]) { /* typed, a key every 1/15 s */
                wchar_t ch[2] = {(wchar_t)line[(*at)++], 0};
                SendMessageW(g_edit, EM_REPLACESEL, TRUE, (LPARAM)ch);
            }
            rec_glide(t, 8.0, 8.7, kx, ky, ox, oy);   /* out to the file, and back with it */
            if (t >= 8.9 && t < 10.5) g_drag_img = 1;
            rec_glide(t, 8.9, 9.7, ox, oy, kx, ky);
            if (s && t >= 9.5 && t < 10.5) snprintf(g_drag_sid, sizeof g_drag_sid, "%s", s->sid); /* over the core: it reads it */
            if (s && t >= 10.5 && t < 10.5 + 1 / 30.0) { /* dropped: as drop_on_core, with a made-up file */
                g_drag_sid[0] = 0; g_drag_img = 0;
                snprintf(g_draft_sid, sizeof g_draft_sid, "%s", s->sid);
                int first = g_attach_n;
                wcscpy(g_attach[g_attach_n], L"/mnt/c/Users/me/Pictures/ber-curve.png");
                wcscpy(g_attach_name[g_attach_n], L"ber-curve.png");
                g_attach_id[g_attach_n++] = ++g_attach_next;
                snprintf(g_drop_sid, sizeof g_drop_sid, "%s", s->sid);
                g_drop_until = wall() + 1.4;
                insert_tokens(first);
            }
        } else if (!carry) {
            rec_glide(t, 1.0, 1.7, L + .2f * W, T + .9f * H, cx - g_cw * .08f, cy - g_cw * .06f);
            if (t >= 4.6 && sat < 0) { /* on to a small core: wherever it is now */
                for (int i = 0; i < g_core_n; i++) if (i != g_top && g_core_rc[i].right > g_core_rc[i].left) { sat = i; break; }
                sx = (float)g_rec_mouse.x; sy = (float)g_rec_mouse.y;
            }
            if (sat >= 0 && t <= 5.3) rec_glide(t, 4.6, 5.2, sx, sy, (g_core_rc[sat].left + g_core_rc[sat].right) * .5f, (g_core_rc[sat].top + g_core_rc[sat].bottom) * .5f);
            if (t >= 7.4) rec_glide(t, 7.4, 8.1, sx = (t < 7.45 ? (float)g_rec_mouse.x : sx), sy = (t < 7.45 ? (float)g_rec_mouse.y : sy), L + .3f * W, T + .95f * H);
        } else {
            rec_glide(t, 0.2, 0.9, L + .2f * W, T + .9f * H, cx, cy);
            if (t >= 1.0 && !picked && g_top >= 0) { /* Alt-click on the main core: it rides on the pointer */
                snprintf(g_down_sid, sizeof g_down_sid, "%s", shown_room()->s[g_top].sid);
                g_move_mouse = g_rec_mouse; g_move_origin.x = (LONG)g_mx; g_move_origin.y = (LONG)g_my;
                g_moving = 1; g_carrying = 1; picked = 1;
                start_follow();
            }
            /* a slow move (the small cores trail behind), then a dash toward the screen's corner that turns away
             * without stopping: they break off, fly on into the corner and bounce, and stay there */
            static const float PATH[][3] = {{.5f, .5f, 1.3f}, {.45f, .25f, 2.6f}, {.82f, .7f, 2.7f}, {.82f, .05f, 2.78f}, {.55f, .2f, 3.3f}};
            for (int k = 0; k < 4; k++)
                if (t >= PATH[k][2] && t < PATH[k + 1][2]) {
                    float e = (float)((t - PATH[k][2]) / (PATH[k + 1][2] - PATH[k][2]));
                    if (k == 0 || k == 3) e = rec_ease(e); /* the slow parts eased, the dash and the turn at full speed */
                    g_rec_mouse.x = (LONG)lroundf(L + W * (PATH[k][0] + (PATH[k + 1][0] - PATH[k][0]) * e));
                    g_rec_mouse.y = (LONG)lroundf(T + H * (PATH[k][1] + (PATH[k + 1][1] - PATH[k][1]) * e));
                }
            if (t >= 4.0 && t < 8.6) { /* over to each loose one, slowly: close enough, it comes back to the orbit */
                Orbit *best = NULL;
                float bd = 1e9f, px = (float)g_rec_mouse.x, py = (float)g_rec_mouse.y, gx = L + .5f * W, gy = T + .5f * H;
                for (int i = 0; i < g_orbit_n; i++) if (g_orbit[i].free) { float d = hypotf(g_orbit[i].px - px, g_orbit[i].py - py); if (d < bd) { bd = d; best = &g_orbit[i]; } }
                if (best) { gx = best->px; gy = best->py; }
                gx = clampf(gx, L + g_cw * .4f, L + W - g_cw * .4f); gy = clampf(gy, T + g_cw * .4f, T + H - g_cw * .4f);
                float dx = gx - px, dy = gy - py, d = hypotf(dx, dy), step = 300 * (float)g_dpi / 30;
                if (d > step) { dx *= step / d; dy *= step / d; }
                g_rec_mouse.x = (LONG)lroundf(px + dx); g_rec_mouse.y = (LONG)lroundf(py + dy);
            }
            if (t >= 9.0 && !dropped) { dropped = 1; place_down(); sx = (float)g_rec_mouse.x; sy = (float)g_rec_mouse.y; }
            if (dropped) rec_glide(t, 9.4, 10.2, sx, sy, L + .15f * W, T + .92f * H); /* the pointer goes away */
        }
        tick();
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessage(&msg); }
        if (t >= 0 && fno % 2 == 0) rec_frame(f);
        if (t > END) break;
    }
    fclose(f);
}

static void menu(HWND w, int core) {
    HMENU m = CreatePopupMenu(), pick = NULL;
    wchar_t buf[80];
    Room *room = shown_room();
    const Session *cs = core >= 0 && core < room->n ? &room->s[core] : NULL;
    char csid[72] = "";
    if (cs && strcmp(cs->sid, "ghost")) { /* this session */
        snprintf(csid, sizeof csid, "%s", cs->sid);
        msg_load(cs, wall() + 1);
        AppendMenu(m, MF_STRING | (g_msg_reply ? 0 : MF_GRAYED), ID_COPY, L"Copy reply");
        AppendMenu(m, MF_STRING, ID_REPLY, L"Reply...\tDouble-click");
        AppendMenu(m, MF_STRING, ID_FRONT, L"Bring to front");
        AppendMenu(m, MF_SEPARATOR, 0, NULL);
    }
    if (g_single && g_room.n > 1) { /* which session is in the middle */
        pick = CreatePopupMenu();
        for (int i = 0; i < g_room.n && i < MAX_SESS; i++) {
            MultiByteToWideChar(CP_UTF8, 0, g_room.s[i].name[0] ? g_room.s[i].name : "claude", -1, buf, 80);
            AppendMenu(pick, MF_STRING | (!strcmp(g_room.s[i].sid, g_focus) ? MF_CHECKED : 0), ID_PIN + i, buf);
        }
        AppendMenu(m, MF_POPUP, (UINT_PTR)pick, L"Session");
        if (g_focus_pinned) AppendMenu(m, MF_STRING, ID_FOLLOW, L"Follow automatically");
    }
    if (g_single ? pos_find("@main") != NULL : any_core_placed())
        AppendMenu(m, MF_STRING, ID_RESET, g_single ? L"Reset position" : L"Reset positions");
    wchar_t hk[40] = L"";
    if (strcmp(g_mode_hotkey, "none")) {
        swprintf(hk, 40, L"\t%hs", g_mode_hotkey);
        for (wchar_t *c = hk + 1; *c; c++) *c = (wchar_t)towupper(*c);
    }
    swprintf(buf, 80, L"%ls%ls", g_single ? L"All cores" : L"Single core", hk);
    AppendMenu(m, MF_STRING, ID_MODE, buf);
    AppendMenu(m, MF_SEPARATOR, 0, NULL);
    AppendMenu(m, MF_STRING, ID_SETTINGS, L"Settings...");
    AppendMenu(m, MF_SEPARATOR, 0, NULL);
    AppendMenu(m, MF_STRING, ID_QUIT, L"Quit Vigil");
    POINT p; GetCursorPos(&p);
    tip_hide();
    SetForegroundWindow(w); /* so the menu closes when you click elsewhere */
    SetTimer(w, ID_TIMER, 33, NULL); /* the menu runs its own loop: the cores keep moving on this timer meanwhile */
    g_menu_open = 1;
    int id = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, w, NULL);
    g_menu_open = 0;
    KillTimer(w, ID_TIMER);
    DestroyMenu(m);
    if (id == ID_QUIT) { PostQuitMessage(0); return; }
    if (id == ID_COPY || id == ID_FRONT || id == ID_REPLY) { /* the session may have moved in the list while the menu was open */
        int i = index_of(csid);
        if (i >= 0 && id == ID_COPY) copy_reply(&shown_room()->s[i]);
        else if (i >= 0 && id == ID_REPLY) input_open_for(&shown_room()->s[i]);
        else if (i >= 0) bring_to_front(&shown_room()->s[i]);
        return;
    }
    if (id == ID_SETTINGS) { settings_open(); return; }
    if (id >= ID_PIN && id < ID_PIN + MAX_SESS && id - ID_PIN < g_room.n) {
        snprintf(g_focus, sizeof g_focus, "%s", g_room.s[id - ID_PIN].sid);
        g_focus_pinned = 1;
        g_focus_since = wall();
    }
    else if (id == ID_FOLLOW) { g_focus_pinned = 0; g_focus_since = wall(); }
    else if (id == ID_RESET) { if (g_single) { pos_forget(0); main_place(); } else pos_forget(1); }
    else if (id == ID_MODE) toggle_mode();
}

static LRESULT CALLBACK proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    Room *room = shown_room();
    POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST && msg != WM_MOUSEWHEEL && msg != WM_MOUSEHWHEEL)
        ClientToScreen(w, &p); /* the pointer, in screen pixels (the wheel's already is) */
    switch (msg) {
    case WM_TIMER:
        if (wp == ID_SENDKEYS) { KillTimer(w, ID_SENDKEYS); type_keys(); }
        else if (wp == ID_SENDENTER) { KillTimer(w, ID_SENDENTER); type_enter(); }
        else tick();
        return 0;
    case WM_MOUSEWHEEL: { /* over a core: its reply scrolls, and the core spins with the wheel */
        int i = core_at(p, NULL, NULL);
        if (i < 0 || i >= room->n) return 0;
        float notches = GET_WHEEL_DELTA_WPARAM(wp) / 120.0f;
        Session *s = &room->s[i];
        wheel_spin(s, notches);
        if (!strcmp(g_tip_sid, s->sid)) tip_scroll(notches);
        return 0;
    }
    case WM_HOTKEY: if (wp == HK_MODE) toggle_mode(); return 0;
    case WM_UNHOOK: if (g_hook && !g_carrying) { UnhookWindowsHookEx(g_hook); g_hook = NULL; } return 0;
    case WM_LBUTTONDOWN: { /* a press: the patch under it goes in; it may become a drag */
        float lx, ly;
        int i = core_at(p, &lx, &ly);
        if (i < 0 || i >= room->n || g_carrying) return 0;
        Session *s = &room->s[i];
        snprintf(g_down_sid, sizeof g_down_sid, "%s", s->sid);
        g_dragging = 0; g_down_at = g_drag_at = p; g_drag_t = wall();
        g_move_mouse = p;
        SetCapture(w);
        if (alt_held()) { /* Alt-drag: the cluster (single core) or this core (all cores) goes anywhere */
            mask_alt();
            g_moving = 1;
            if (g_single && i == g_top) { g_move_origin.x = (LONG)g_mx; g_move_origin.y = (LONG)g_my; } /* the main core: the whole cluster */
            else if (g_single) { Orbit *ob = orbit_of(s->sid); g_move_origin.x = (LONG)ob->px; g_move_origin.y = (LONG)ob->py; g_moving = 2; } /* a small core, by hand */
            else { g_move_origin.x = g_core_rc[i].left; g_move_origin.y = g_core_rc[i].top; }
            g_hold_x = g_move_origin.x; g_hold_y = g_move_origin.y;
            start_follow();
            return 0;
        }
        s->press_at = wall(); s->press_x = lx; s->press_y = ly;
        return 0;
    }
    case WM_MOUSEMOVE: {
        int i = down_index();
        if (g_moving || i < 0) return 0; /* (a moved core follows the timer) */
        Session *s = &room->s[i]; /* drag: the shell turns under the hand, like the macOS panel */
        if (!g_dragging && abs(p.x - g_down_at.x) + abs(p.y - g_down_at.y) < DRAG_AFTER) return 0;
        g_dragging = 1; s->held = 1;
        double now = wall();
        float dx = (float)(p.x - g_drag_at.x) / (float)g_dpi, dy = (float)(p.y - g_drag_at.y) / (float)g_dpi, dt = (float)(now - g_drag_t);
        s->ay += dx * 0.012f; s->ax += dy * 0.012f;
        if (dt > 0.001f) { g_vx = dy * 0.012f / dt; g_vy = dx * 0.012f / dt; }
        g_drag_at = p; g_drag_t = now;
        return 0;
    }
    case WM_LBUTTONUP: {
        ReleaseCapture();
        if (g_moving) {
            POINT m; GetCursorPos(&m);
            if (!g_carrying && abs(m.x - g_move_mouse.x) + abs(m.y - g_move_mouse.y) < DRAG_AFTER) carry(); /* Alt-click */
            else if (!g_carrying) end_move();
            return 0;
        }
        int i = down_index();
        if (i >= 0 && g_dragging) { /* let go: it spins on and slows back to its own rotation */
            Session *s = &room->s[i];
            int thrown = wall() - g_drag_t < 0.1;
            s->held = 0;
            s->spin_x = thrown ? fmaxf(-8, fminf(8, g_vx)) : 0;
            s->spin_y = thrown ? fmaxf(-8, fminf(8, g_vy)) : 0;
        } else if (i >= 0) { /* a click: on a small core it comes to the middle (and stays); else the reply is read */
            Session *s = &room->s[i];
            if (g_single && strcmp(s->sid, g_focus) && g_room.n) {
                snprintf(g_focus, sizeof g_focus, "%s", s->sid);
                g_focus_pinned = 1;
                g_focus_since = wall();
            } else s->unread = 0;
            g_hover_since = 0; /* the tooltip shows without the usual pause */
        }
        g_down_sid[0] = 0; g_dragging = 0;
        return 0;
    }
    case WM_LBUTTONDBLCLK: { /* the ring, the reply counts as read, and a line opens to type an answer */
        int i = core_at(p, NULL, NULL);
        if (i >= 0 && i < room->n) { room->s[i].ring_at = wall(); room->s[i].unread = 0; input_open_for(&room->s[i]); }
        return 0;
    }
    case WM_RBUTTONUP: { int i = core_at(p, NULL, NULL); if (i >= 0) menu(w, i); return 0; }
    case WM_DROPFILES: { /* images dropped on a core (when the drop target below is not in use) */
        POINT dp;
        DragQueryPoint((HDROP)wp, &dp);
        ClientToScreen(w, &dp);
        drop_on_core(core_at(dp, NULL, NULL), (HDROP)wp);
        DragFinish((HDROP)wp);
        return 0;
    }
    case WM_SETCURSOR: /* moved by the mouse: the core is the pointer */
        if (g_moving) {
            POINT m; GetCursorPos(&m);
            if (g_carrying || abs(m.x - g_move_mouse.x) + abs(m.y - g_move_mouse.y) >= DRAG_AFTER) { SetCursor(NULL); return TRUE; }
        }
        break;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE; /* never take focus from the terminal */
    case WM_DESTROY: if (w == g_cluster.w) PostQuitMessage(0); return 0;
    }
    return DefWindowProc(w, msg, wp, lp);
}

/* "ctrl+alt+c" → modifiers and a virtual key; 0 if it does not parse ("opt" and "option" mean Alt here) */
static int parse_hotkey(const char *spec, UINT *mods, UINT *vk) {
    char buf[64];
    snprintf(buf, sizeof buf, "%s", spec);
    *mods = 0; *vk = 0;
    for (char *tok = strtok(buf, "+"); tok; tok = strtok(NULL, "+")) {
        for (char *c = tok; *c; c++) *c = (char)tolower((unsigned char)*c);
        if (!strcmp(tok, "ctrl") || !strcmp(tok, "control")) *mods |= MOD_CONTROL;
        else if (!strcmp(tok, "alt") || !strcmp(tok, "opt") || !strcmp(tok, "option")) *mods |= MOD_ALT;
        else if (!strcmp(tok, "shift")) *mods |= MOD_SHIFT;
        else if (!strcmp(tok, "win") || !strcmp(tok, "cmd")) *mods |= MOD_WIN;
        else if (!strcmp(tok, "space")) *vk = VK_SPACE;
        else if (tok[0] == 'f' && atoi(tok + 1) >= 1 && atoi(tok + 1) <= 12) *vk = VK_F1 + (UINT)atoi(tok + 1) - 1;
        else if (!tok[1] && isalnum((unsigned char)tok[0])) *vk = (UINT)toupper((unsigned char)tok[0]);
        else return 0;
    }
    return *vk && *mods;
}

static void usage(void) {
    MessageBoxA(NULL,
        "vigil-panel.exe: Vigil's floating panel for Windows\n\n"
        "  --events PATH        the events file (from WSL: \\\\wsl$\\<distro>\\home\\<you>\\.dodeca-core\\events.jsonl;\n"
        "                       the win/vigil-win launcher passes it for you)\n"
        "  --demo [N]           made-up sessions instead of the events file (1-6, default 3)\n"
        "  --all                every core its own window, each placed by hand (default: single core)\n"
        "  --no-others          single core: only the session in focus, no small cores around it\n"
        "  --mode-hotkey KEYS   switch single core / all cores (default ctrl+alt+c; none = off)\n"
        "  --shape NAME         dodeca | bucky | geo2 (default) | geo3\n"
        "  --width PIXELS       column width (default: 17% of the screen)\n"
        "  --font-size N        tooltip text size (default 14)\n"
        "  --glow X  --core X   glow strength, inner core radius (default 1, 0.35)\n"
        "  --frame NAME         focus frame: segments (default) | lens | polar | diamond | lock | orbits | none\n"
        "  --labels always|hover   all cores: name and state above each core (default always)\n"
        "  --away SECONDS       no input this long: cores show STANDBY (default 120, 0 = never)\n"
        "  --stats              every 5 s, frames, pictures and window updates a second to panel-stats.txt\n"
        "  --settings           open the Settings window at once\n"
        "  --settings-shot FILE open it, save it as a BMP after 2 s and quit (to check a change)\n"
        "  --settings-zoom X    start the preview zoomed in, 1 to 3 (with --settings-shot)\n"
        "  --on X,Y             --record and the Settings window go on the monitor with that desktop point\n"
        "  --record SCENE FILE  reply | input | carry: a scripted demo scene, frames written raw to FILE (the README's animations)\n"
        "  --settings-tab motions open it on the Motions page\n\n"
        "Right-click a core: Copy reply, Reply, Bring to front, and Settings... (a window with a live preview:\n"
        "shape, inner core, glow, focus frame, size, gravity, other sessions, text box, text size, labels,\n"
        "standby); settings are kept and win over these options until \"Reset to launch options\".\n\n"
        "Alt-drag a core: move the cluster (single core) or that core (all cores). Alt-click: pick it up,\n"
        "the next click puts it down.",
        "Vigil", MB_OK);
}

int main(int argc, char **argv) {
    const char *events = NULL;
    OleInitialize(NULL); /* the cores are drop targets (an image dragged over one) */
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--demo")) { g_demo_n = 3; if (v && v[0] >= '1' && v[0] <= '6' && !v[1]) { g_demo_n = v[0] - '0'; i++; } }
        else if (!strcmp(a, "--events") && v) { events = v; i++; }
        else if (!strcmp(a, "--all")) g_single = 0;
        else if (!strcmp(a, "--single")) g_single = 1;
        else if (!strcmp(a, "--others")) g_others = 1;
        else if (!strcmp(a, "--no-others")) g_others = 0;
        else if (!strcmp(a, "--mode-hotkey") && v) { snprintf(g_mode_hotkey, sizeof g_mode_hotkey, "%s", v); i++; }
        else if (!strcmp(a, "--shape") && v) { snprintf(g_shape_name, sizeof g_shape_name, "%s", v); i++; }
        else if (!strcmp(a, "--width") && v) { g_width = atoi(v); i++; }
        else if (!strcmp(a, "--font-size") && v) { g_font = (float)atof(v); if (g_font < 8) g_font = 8; i++; }
        else if (!strcmp(a, "--glow") && v) { g_glow = (float)atof(v); i++; }
        else if (!strcmp(a, "--core") && v) { g_core = (float)atof(v); i++; }
        else if (!strcmp(a, "--frame") && v) { snprintf(g_frame, sizeof g_frame, "%s", v); i++; }
        else if (!strcmp(a, "--labels") && v) { g_labels = strcmp(v, "hover") != 0; i++; }
        else if (!strcmp(a, "--away") && v) { g_away_after = atof(v); i++; }
        else if (!strcmp(a, "--stats")) g_stats = 1;
        else if (!strcmp(a, "--settings")) g_open_settings = 1;
        else if (!strcmp(a, "--settings-tab") && v) { g_tab = !strcmp(v, "motions"); g_open_settings = 1; i++; }
        else if (!strcmp(a, "--on") && v && sscanf(v, "%ld,%ld", &g_on.x, &g_on.y) == 2) { g_on_set = 1; i++; }
        else if (!strcmp(a, "--settings-zoom") && v) { g_pv_zoom = clampf((float)atof(v), 1, 3); i++; }
        else if (!strcmp(a, "--record") && v && i + 2 < argc) { snprintf(g_rec_scene, sizeof g_rec_scene, "%s", v); snprintf(g_rec_file, sizeof g_rec_file, "%s", argv[i + 2]); i += 2; }
        else if (!strcmp(a, "--settings-shot") && v) { snprintf(g_set_shot, sizeof g_set_shot, "%s", v); g_open_settings = 1; i++; }
        else { usage(); return a[0] == '-' && (!strcmp(a, "-h") || !strcmp(a, "--help")) ? 0 : 2; }
    }
    if (g_rec_scene[0]) { if (strcmp(g_rec_scene, "reply") && strcmp(g_rec_scene, "carry") && strcmp(g_rec_scene, "input")) { usage(); return 2; } g_demo_n = 3; g_rec_now = 1.7e9; }
    if (!shape_make(&g_shape, g_shape_name)) { usage(); return 2; }
    if (!events && !g_demo_n) { usage(); return 2; }
    if (events) { /* its folder holds msg\ and the saved positions */
        snprintf(g_dir, sizeof g_dir, "%s", events);
        char *cut = strrchr(g_dir, '\\'), *cut2 = strrchr(g_dir, '/');
        if (cut2 > cut) cut = cut2;
        if (cut) *cut = 0; else g_dir[0] = 0;
    }
    remember_launch();
    settings_load(); /* the Settings menu's choices win over the options */
    if (!shape_make(&g_shape, g_shape_name)) { snprintf(g_shape_name, sizeof g_shape_name, "%s", g_launch.shape); shape_make(&g_shape, g_shape_name); }
    pos_read();
    SetProcessDPIAware();
    HDC screen = GetDC(NULL);
    g_dpi = GetDeviceCaps(screen, LOGPIXELSX) / 96.0;
    ReleaseDC(NULL, screen);

    double now = wall();
    g_last = g_last_draw = now;
    session_init(&g_ghost.s[0], "ghost", now, &g_shape);
    snprintf(g_ghost.s[0].name, sizeof g_ghost.s[0].name, "no session");
    g_ghost.n = 1;
    if (g_demo_n) demo_start(&g_demo, g_demo_n, now);
    else {
        tail_init(&g_tail, events);
        int n = tail_history(&g_tail, now - 1800, g_ev, 1024); /* the sessions of the last half hour */
        for (int i = 0; i < n; i++) room_apply(&g_room, &g_ev[i]);
    }

    SystemParametersInfo(SPI_GETWORKAREA, 0, &g_work, 0);
    int sw = g_work.right - g_work.left;
    g_col_w = g_width > 0 ? g_width : (int)fmax(220 * g_dpi, fmin(360 * g_dpi, sw * 0.17));
    WNDCLASS wc;
    memset(&wc, 0, sizeof wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"VigilPanel";
    RegisterClass(&wc);
    WNDCLASS tc;
    memset(&tc, 0, sizeof tc);
    tc.lpfnWndProc = tip_proc;
    tc.hInstance = wc.hInstance;
    tc.hCursor = LoadCursor(NULL, IDC_ARROW);
    tc.lpszClassName = L"VigilTip";
    RegisterClass(&tc);
    pane_create(&g_cluster);
    g_tip = CreateWindowEx(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                           L"VigilTip", L"Vigil", WS_POPUP, 0, 0, 1, 1, NULL, NULL, wc.hInstance, NULL);
    SetLayeredWindowAttributes(g_tip, 0, 255, LWA_ALPHA); /* opaque: text behind it must not show through */
    main_place();
    UINT mods, vk;
    if (strcmp(g_mode_hotkey, "none")) {
        if (!parse_hotkey(g_mode_hotkey, &mods, &vk)) fprintf(stderr, "vigil-panel: cannot read --mode-hotkey %s\n", g_mode_hotkey);
        else if (!RegisterHotKey(g_cluster.w, HK_MODE, mods | MOD_NOREPEAT, vk)) fprintf(stderr, "vigil-panel: hotkey %s is taken by another app\n", g_mode_hotkey);
    }
    /* The clock: a high-resolution waitable timer (a SetTimer of 33 ms really fires every 47, Windows' timer
     * steps being 15.6 ms), waited on together with the window messages. */
    HANDLE clock = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!clock) clock = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS); /* before Windows 10 1803 */
    g_stat_since = wall();
    if (g_open_settings) settings_open();
    MSG m;
    if (g_rec_scene[0]) { rec_run(); goto done; }
    double shot_at = wall() + 2;
    for (;;) {
        tick();
        if (g_set_shot[0] && wall() > shot_at) { settings_shot(g_set_shot); break; }
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)(tick_interval() * 1e7);
        SetWaitableTimer(clock, &due, 0, NULL, NULL, FALSE);
        for (;;) { /* messages until the timer fires */
            DWORD r = MsgWaitForMultipleObjectsEx(1, &clock, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            while (PeekMessage(&m, NULL, 0, 0, PM_REMOVE)) {
                if (m.message == WM_QUIT) goto done;
                TranslateMessage(&m); DispatchMessage(&m);
            }
            if (r == WAIT_OBJECT_0) break;
        }
    }
done:
    if (g_hook) UnhookWindowsHookEx(g_hook);
    return 0;
}

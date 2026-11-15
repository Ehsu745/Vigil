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
 * Also: hover replies (the chat box with the last reply or permission notice, from the hook's msg files,
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
#define COBJMACROS /* (WIC's interfaces from C: a pasted image saved as PNG) */
#include <windows.h>
#include <windowsx.h>
#include <wincodec.h>
#include <shellapi.h>
#include <ole2.h>
#include <richedit.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <tlhelp32.h>
#include <winternl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "../src/vigil.h"
#include "gpu.h"

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
static float g_chat_size = 1;        /* Settings > Box size: how wide and tall the chat box may grow */
static float g_gravity = 2;         /* how hard the main core holds the small ones on their orbit (Settings > Gravity) */
static char g_shape_name[16] = "geo2";
static double g_dpi = 1;            /* the scale of the monitor the main core is on (1 = 96 dpi); followed as it moves */
static double g_sdpi = 1;           /* the Settings window's own monitor's */
static double g_hz = 60;            /* that monitor's refresh rate */
static long g_tick_n;               /* ticks so far: the small cores move on every n-th one */
static double g_tick_hz = 60;       /* how many ticks a second the clock runs at now (a whole fraction of g_hz) */
static double g_col_dip;            /* the core column's width in 96-dpi units (from the primary screen, or --width) */
static int g_smooth = SM_BALANCED;  /* Smoothness: how often pictures are made (right-click > Smoothness) */
static Smooth g_smooth_custom = {15, 22.5f, 30, 6, 60};
static int g_threads;                /* Settings > Performance > Drawing threads: 0 Auto, else this many in all (this one too) */
static const Smooth *smooth_now(void) { return g_smooth == SM_CUSTOM ? &g_smooth_custom : &SMOOTH[g_smooth]; }
static char g_dir[1024];            /* the events file's folder (a Windows path): msg\ and the saved positions */
static char g_frame[16] = "segments"; /* --frame: drawn round a core while it is carried or locked on */
static int g_labels = 1;            /* all cores: name and state above each core (--labels hover: none) */
static int g_shade_col;             /* Settings > Look > Darkness: the colour under the light, one of SHADE_COLS, or 5: g_shade_custom */
static unsigned g_shade_custom = 0x101830;
static int g_pv_light, g_pv_light_arg; /* the Settings preview on a light background (the sun / moon chip; --settings-light) */
static int g_backdrop;              /* a soft dark outline along the cores' lines and READY's pieces: 0 off, 1 on, 2 auto (when what is under them is light) */
static int g_bd_w = 1, g_bd_a = 1;   /* its width and strength: steps of BD_WIDTHS / BD_STRENGTHS (Settings > Look > Backdrop) */
static const float BD_WIDTHS[4] = {0.6f, 1, 1.5f, 2.2f}, BD_STRENGTHS[4] = {0.55f, 1, 1.3f, 1.6f};
static void bd_apply(void) { BACKDROP_W = BD_WIDTHS[g_bd_w & 3]; BACKDROP_A = BD_STRENGTHS[g_bd_a & 3]; }
static double g_away_after = 120;   /* --away: seconds without keyboard or mouse input until "away" (0 = never) */
static int g_away;
static int g_chat_middle;            /* Settings > Chat box: in the middle of the screen, else next to the core */
static int g_chat_bg;                /* Settings > Box colour: the chat box's background, one of CHAT_BGS, or 5: g_chat_custom */
static unsigned g_chat_custom = 0x2A2F3A; /* the colour picked in the palette window (0xRRGGBB) */
static int g_text_col;              /* Settings > Colours > Text: 0 auto (light on a dark box, dark on a light one), 1-4 TEXT_COLS, 5 g_text_custom */
static unsigned g_text_custom = 0xF2F2F5;
static int g_wheel_spin;            /* Settings > Scroll on a core: how the wheel spins it, one of WHEEL_NAMES (4: not at all) */
static int g_chat_mono = 1;          /* Settings > Chat box font: the terminal's monospaced face, like Claude Code (else Segoe UI) */
static int g_open_settings;         /* --settings: open the Settings window at once */
static float g_scroll_arg;           /* --settings-scroll PX: set once the page is laid out (it is clamped to the page until then) */
static char g_set_shot[1024];       /* --settings-shot FILE: open it, let the preview run 2 s, save the window as a BMP and quit */
static int g_shot_act, g_shot_n = 1; static float g_shot_step = .5f; /* --settings-hole A N STEP: once the preview is a grown black hole, play its action A, then save N shots STEP s apart (FILE_00.bmp ...) */
static int g_stats;                 /* --stats: frames, pictures and window updates a second, to panel-stats.txt */
static int g_gpu = 1;               /* the black hole drawn on the GPU (win/gpu.c) when it can be (Settings > Performance, holeGpu); --no-gpu: on the CPU */
static int g_nogpu_arg;
static double g_stat_gpu_ms; static long g_stat_gpu_n; /* (--stats: the GPU's share of the pictures) */
static long g_stat_tick, g_stat_pic, g_stat_push;
static double g_stat_ms, g_stat_since;
static double g_stat_gap, g_stat_gap2, g_stat_gap_max, g_stat_last; /* the time from one tick to the next: mean, spread, worst */
static long g_stat_late;            /* ticks that came more than half an interval late */
/* while READY's pieces are out (the layer shown): ticks, late ones, the worst gap and tick, the slowest events-file read,
 * and when (seconds into the span) each late tick came and how late, to see a stall that repeats */
static long g_sa_n, g_sa_late, g_sa_nl;
static double g_sa_gap_max, g_sa_work_max, g_sa_poll_max, g_sa_lt[24], g_sa_lg[24];
static long g_stat_paint_edit, g_stat_paint_card; /* repaints of the reply line's field and of its card */
static double g_stat_render_ms, g_stat_push_ms; /* of the tick time: making pictures, handing bitmaps to Windows */
static double qpc_ms(void) { LARGE_INTEGER q, f; QueryPerformanceCounter(&q); QueryPerformanceFrequency(&f); return q.QuadPart * 1000.0 / f.QuadPart; }

/* single-core mode */
static int g_single = 1;            /* --all: every core its own window */
static int g_others = 1;            /* the other sessions float around small (--no-others: only the one in focus) */
static char g_mode_hotkey[32] = "ctrl+alt+c";
static char g_hk_hide[32] = "ctrl+alt+v", g_hk_reply[32] = "ctrl+alt+r", g_hk_set[32] = "none"; /* Settings > Shortcuts: global hotkeys */
static int g_hk_fail[4];            /* a hotkey another app holds (RegisterHotKey failed) */
static int g_move_mod = VK_MENU;    /* the key held to move (drag) or carry (click) a core */
static int g_send_ctrl;             /* the reply line sends with Ctrl+Enter (and Enter makes a new line) */
static int g_hidden;                /* Show or hide Vigil: every core window hidden */
static float g_set_w = 1000, g_set_h = 720; /* the Settings window's size (96-dpi units), kept */
static int g_set_side = 1;          /* its sidebar open (else a rail of icons), kept */
static void hotkeys_apply(void);
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
static HWND g_chat;
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

enum { ID_TIMER = 1, HK_MODE = 1, HK_HIDE = 2, HK_REPLY = 3, HK_SET = 4, WM_UNHOOK = WM_APP + 1,
       ID_QUIT = 100, ID_PIN = 400, ID_MODE = 500, ID_FOLLOW, ID_RESET,
       ID_COPY = 1200, ID_FRONT, ID_REPLY, ID_SENDKEYS = 1400, ID_SENDENTER, ID_WTENTER, ID_SETTINGS = 1500, ID_SMOOTH = 1600 };
static const char *SHAPES[4] = {"dodeca", "bucky", "geo2", "geo3"};
static const float GLOWS[3] = {0.7f, 1.0f, 1.4f};
static const float FONTS[5] = {12, 14, 16, 20, 24};
static const float CORES[4] = {0.25f, 0.35f, 0.45f, 0.55f};   /* the inner core's radius (--core) */
static const float SCALES[4] = {0.75f, 1.0f, 1.3f, 1.6f};     /* how big the cores are on the screen */
static const char *FRAMES[7] = {"segments", "lens", "polar", "diamond", "lock", "orbits", "none"};
static const wchar_t *FRAME_NAMES[7] = {L"Segments", L"Lens", L"Polar", L"Diamond", L"Lock", L"Orbits", L"None"};
static const float GRAVITIES[4] = {1, 2, 4, 8};             /* the orbit spring's strength, times the lightest */
static const float TILTS[5] = {0, 1, 2.5f, 4, 6};            /* a black hole's own tilt and roll, how fast (HOLE_TILT) */
static const wchar_t *TILT_NAMES[5] = {L"Still", L"Slow", L"Normal", L"Fast", L"Faster"};
static const double AWAYS[8] = {60, 120, 300, 600, 900, 1800, 3600, 0}; /* (seconds; 0: never) */
static const float CHATSIZES[4] = {0.75f, 1.0f, 1.35f, 1.75f};
static const unsigned CHAT_BGS[5] = {0x18191E, 0x262626, 0x16233F, 0x2B1C3B, 0x15302A}; /* dark, so the text keeps its contrast */
static const wchar_t *CHAT_BG_NAMES[6] = {L"Ink", L"Graphite", L"Navy", L"Plum", L"Pine", L"Custom"};
static const unsigned SHADE_COLS[5] = {0x000000, 0x0A1128, 0x1C0A26, 0x071D14, 0x2A1206}; /* black, navy, plum, pine, ember */
static const wchar_t *SHADE_NAMES[6] = {L"Black", L"Navy", L"Plum", L"Pine", L"Ember", L"Custom"};
static unsigned shadex(void) { return g_shade_col == 5 ? g_shade_custom : SHADE_COLS[g_shade_col]; } /* 0xRRGGBB */
static const wchar_t *WHEEL_NAMES[5] = {L"Roll", L"Roll back", L"Turn", L"Turn back", L"Off"};
static unsigned chat_bgx(void) { return g_chat_bg == 5 ? g_chat_custom : CHAT_BGS[g_chat_bg]; } /* 0xRRGGBB */
static COLORREF chat_bg(void) { unsigned v = chat_bgx(); return RGB(v >> 16 & 255, v >> 8 & 255, v & 255); }
static int box_light(void) { unsigned v = chat_bgx(); return 0.299f * (v >> 16 & 255) + 0.587f * (v >> 8 & 255) + 0.114f * (v & 255) > 140; }
static COLORREF chat_field(void) { /* the typing field: a step lighter on a dark box, a step darker on a light one */
    unsigned v = chat_bgx();
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
/* the wheel over a core: it spins one way or another (Settings > Scroll on a core). A sideways scroll (a tilt wheel, or
 * a two-finger swipe on a touchpad, which sends both at once) spins it about the other axis, so a swipe spins it any
 * way you like; it keeps spinning that way afterwards (Session.sdir_*), until you send it another way */
static double wall(void);
static unsigned char g_sat_behind[MAX_SESS]; /* a small core behind the black hole now (its window goes under the hole's) */
static int is_hole(const Session *s) { return s->mo.rt == RT_HOLE && s->mo.hole > 0.5f && s->mo.rs.hole.grow > 0.5f; } /* (asleep, a grown black hole) */
static void wheel_spin2(Session *s, float notches, int sideways) {
    if (g_wheel_spin >= 4) return; /* Off */
    /* scrolled on and on, it spins up: each step pushes harder the longer the scrolling has gone on without a pause
     * (up to 3.5x after 1.5 s), and it may go faster; a pause, another core or the other way round starts over */
    static char sid[72]; static double t0, last; static int sgn[2];
    double now = wall();
    int sg = notches > 0 ? 1 : -1, ax = sideways != 0;
    if (strcmp(sid, s->sid) || now - last > .3 || (sgn[ax] && sgn[ax] != sg)) { snprintf(sid, sizeof sid, "%s", s->sid); t0 = now; sgn[0] = sgn[1] = 0; }
    sgn[ax] = sg; last = now;
    float boost = 1 + 2.5f * fminf(1, (float)(now - t0) / 1.5f), cap = 8 * boost;
    if (!sideways && is_hole(s)) { /* a black hole: the wheel pushes its gas round (back: the other way), spinning up the same way; a sideways swipe turns it over */
        HoleRS *H = &s->mo.rs.hole;
        float hc = 24 * boost; /* (its gas: up to 25 times its usual speed, 85 scrolled on) */
        H->flow_v = fmaxf(-hc, fminf(hc, H->flow_v + notches * 3.2f * boost * (g_wheel_spin & 1 ? -1 : 1)));
        return;
    }
    float k = notches * 1.2f * boost * (g_wheel_spin & 1 ? -1 : 1); /* (the "back" choices turn the other way) */
    int roll = (g_wheel_spin < 2) != (sideways != 0); /* roll: the top goes away as the wheel goes down; turn: round the upright axis */
    if (roll) s->spin_x = fmaxf(-cap, fminf(cap, s->spin_x - k));
    else s->spin_y = fmaxf(-cap, fminf(cap, s->spin_y - k)); /* (sideways: the front follows the fingers) */
    float l = hypotf(s->spin_x, s->spin_y);
    if (l > .05f) { s->sdir_tx = s->spin_x / l * 1.066f; s->sdir_ty = s->spin_y / l * 1.066f; } /* (as long as its usual way round) */
}
static void wheel_spin(Session *s, float notches) { wheel_spin2(s, notches, 0); }
#define DRAG_AFTER ((int)(4 * g_dpi)) /* pixels the mouse moves before a press becomes a drag */

/* --record SCENE FILE (to make the README's animations): the demo on a fixed clock, a scripted pointer in place of
 * the real one (the real mouse is never read or moved), and every frame of the panel's own windows written to FILE */
static char g_rec_scene[16], g_rec_file[1024];
static int g_on_set; static POINT g_on;  /* --on X,Y: the monitor (the one with that desktop point) for --record and the Settings window */
static double g_rec_now;            /* the made-up clock (0: the real one) */
static POINT g_rec_mouse;           /* the scripted pointer */
static int g_rec_still;             /* the demo sessions stop changing state */

static void cursor_pos(POINT *p) { if (g_rec_scene[0]) *p = g_rec_mouse; else GetCursorPos(p); }
/* the pointer's picture, the part that shows, from its hotspot in pixels: what READY throws at is the whole mouse,
 * not its tip. Measured from the cursor's own bitmaps (any pointer size or scheme), once per cursor; a recording's
 * scripted pointer, or one that cannot be read, counts as Windows' arrow at this scale. */
static void cursor_box(RECT *b) {
    static HCURSOR was; static RECT got; static int ok;
    CURSORINFO ci; memset(&ci, 0, sizeof ci); ci.cbSize = sizeof ci;
    if (g_rec_scene[0] || !GetCursorInfo(&ci) || !ci.hCursor) goto arrow;
    if (ci.hCursor == was) { if (ok) { *b = got; return; } goto arrow; }
    was = ci.hCursor; ok = 0;
    ICONINFO ii;
    if (!GetIconInfo(ci.hCursor, &ii)) goto arrow;
    BITMAP bm;
    HBITMAP src = ii.hbmColor ? ii.hbmColor : ii.hbmMask;
    if (src && GetObject(src, sizeof bm, &bm) && bm.bmWidth > 0 && bm.bmHeight > 0 && bm.bmWidth <= 256 && bm.bmHeight <= 512) {
        int W = bm.bmWidth, H = bm.bmHeight, h = ii.hbmColor ? H : H / 2; /* a mask-only cursor: AND mask over XOR mask */
        BITMAPINFO bi; memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        uint32_t *px = malloc(sizeof(uint32_t) * (size_t)W * H), *mk = malloc(sizeof(uint32_t) * (size_t)W * H);
        HDC dc = GetDC(NULL);
        int okc = px && GetDIBits(dc, src, 0, (UINT)H, px, &bi, DIB_RGB_COLORS);
        int okm = mk && ii.hbmColor && ii.hbmMask && GetDIBits(dc, ii.hbmMask, 0, (UINT)H, mk, &bi, DIB_RGB_COLORS);
        ReleaseDC(NULL, dc);
        if (okc) {
            int alpha = 0;
            if (ii.hbmColor) for (int i = 0; i < W * H && !alpha; i++) alpha = (px[i] >> 24) != 0;
            int x0 = W, y0 = h, x1 = -1, y1 = -1;
            for (int y = 0; y < h; y++) for (int x = 0; x < W; x++) {
                int on;
                if (!ii.hbmColor) on = (px[y * W + x] & 0xFFFFFF) == 0 || (px[(y + h) * W + x] & 0xFFFFFF) != 0; /* AND 0 (drawn) or XOR 1 (inverted) */
                else if (alpha) on = (px[y * W + x] >> 24) > 32;
                else on = okm ? (mk[y * W + x] & 0xFFFFFF) == 0 : (px[y * W + x] & 0xFFFFFF) != 0;
                if (on) { if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
            }
            if (x1 >= x0 && y1 >= y0) { SetRect(&got, x0 - (int)ii.xHotspot, y0 - (int)ii.yHotspot, x1 + 1 - (int)ii.xHotspot, y1 + 1 - (int)ii.yHotspot); ok = 1; }
        }
        free(px); free(mk);
    }
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    if (ok) { *b = got; return; }
arrow:
    SetRect(b, 0, 0, (int)(12 * g_dpi + .5), (int)(19 * g_dpi + .5));
}

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
static struct { float font, core, scale, glow, gravity, chat_size; int smooth, threads; Smooth custom; int others, labels, backdrop, shade_col, bd_w, bd_a, bd_core; unsigned shade_custom; int chat_middle, chat_mono, chat_bg, wheel_spin, text_col; unsigned chat_custom, text_custom; double away; char shape[16], frame[16], hk[4][32]; int move_mod, send_ctrl; } g_launch;

static int settings_path(char *out, size_t n) {
    if (!g_dir[0]) return 0;
    snprintf(out, n, "%s\\panel-settings-win.json", g_dir);
    return 1;
}

static void remember_launch(void) {
    g_launch.smooth = g_smooth; g_launch.custom = g_smooth_custom; g_launch.threads = g_threads;
    g_launch.font = g_font; g_launch.core = g_core; g_launch.scale = g_scale; g_launch.glow = g_glow; g_launch.gravity = g_gravity; g_launch.chat_size = g_chat_size; g_launch.others = g_others;
    g_launch.labels = g_labels; g_launch.backdrop = g_backdrop; g_launch.bd_w = g_bd_w; g_launch.bd_a = g_bd_a; g_launch.bd_core = BACKDROP_CORE; g_launch.shade_col = g_shade_col; g_launch.shade_custom = g_shade_custom; g_launch.away = g_away_after; g_launch.chat_middle = g_chat_middle; g_launch.chat_mono = g_chat_mono;
    g_launch.chat_bg = g_chat_bg; g_launch.wheel_spin = g_wheel_spin; g_launch.chat_custom = g_chat_custom; g_launch.text_col = g_text_col; g_launch.text_custom = g_text_custom;
    snprintf(g_launch.shape, sizeof g_launch.shape, "%s", g_shape_name);
    snprintf(g_launch.frame, sizeof g_launch.frame, "%s", g_frame);
    snprintf(g_launch.hk[0], 32, "%s", g_mode_hotkey); snprintf(g_launch.hk[1], 32, "%s", g_hk_hide); snprintf(g_launch.hk[2], 32, "%s", g_hk_reply); snprintf(g_launch.hk[3], 32, "%s", g_hk_set);
    g_launch.move_mod = g_move_mod; g_launch.send_ctrl = g_send_ctrl;
}

static void restore_launch(void) {
    motion_map_default(MOTION_MAP); /* the motion map has no launch option: back to its defaults */
    HOLE_ACTS = (1u << HA_N) - 2; READY_ATKS = (1u << RA_N) - 2;
    g_smooth = g_launch.smooth; g_smooth_custom = g_launch.custom; g_threads = g_launch.threads;
    g_font = g_launch.font; g_core = g_launch.core; g_scale = g_launch.scale; g_glow = g_launch.glow; g_gravity = g_launch.gravity; g_chat_size = g_launch.chat_size; g_others = g_launch.others;
    g_labels = g_launch.labels; g_backdrop = g_launch.backdrop; g_bd_w = g_launch.bd_w; g_bd_a = g_launch.bd_a; BACKDROP_CORE = g_launch.bd_core; bd_apply(); g_shade_col = g_launch.shade_col; g_shade_custom = g_launch.shade_custom; g_away_after = g_launch.away; g_chat_middle = g_launch.chat_middle; g_chat_mono = g_launch.chat_mono;
    g_chat_bg = g_launch.chat_bg; g_wheel_spin = g_launch.wheel_spin; g_chat_custom = g_launch.chat_custom; g_text_col = g_launch.text_col; g_text_custom = g_launch.text_custom;
    snprintf(g_shape_name, sizeof g_shape_name, "%s", g_launch.shape);
    snprintf(g_frame, sizeof g_frame, "%s", g_launch.frame);
    snprintf(g_mode_hotkey, 32, "%s", g_launch.hk[0]); snprintf(g_hk_hide, 32, "%s", g_launch.hk[1]); snprintf(g_hk_reply, 32, "%s", g_launch.hk[2]); snprintf(g_hk_set, 32, "%s", g_launch.hk[3]);
    g_move_mod = g_launch.move_mod; g_send_ctrl = g_launch.send_ctrl;
}

static int json_num(const char *j, const char *key, float *v) {
    char k[40];
    snprintf(k, sizeof k, "\"%s\"", key);
    const char *c = strstr(j, k);
    return c && sscanf(c + strlen(k), " : %f", v) == 1;
}

/* "key": [n, n, ...] with up to ST_N numbers; 0 if missing. A state past the end (the file is older than it) gets
 * UINT32_MAX: its defaults */
static int json_masks(const char *j, const char *key, uint32_t *out) {
    char k[40];
    snprintf(k, sizeof k, "\"%s\"", key);
    const char *c = strstr(j, k);
    if (!c || !(c = strchr(c + strlen(k), '['))) return 0;
    c++;
    for (int i = 0; i < ST_N; i++) out[i] = UINT32_MAX;
    for (int i = 0; i < ST_N; i++) {
        char *e;
        unsigned long v = strtoul(c, &e, 10);
        if (e == c) return i > 0;
        out[i] = (uint32_t)v;
        c = e;
        while (*c == ' ' || *c == ',') c++;
    }
    return 1;
}

static void settings_load(void) {
    char path[1100], buf[8192];
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
    if (json_num(buf, "holeTilt", &v)) for (int i = 0; i < 5; i++) if (fabsf(v - TILTS[i]) < 0.01f) HOLE_TILT = TILTS[i];
    if (json_num(buf, "tipSize", &v) && v >= 0.5f && v <= 2) g_chat_size = v;
    if (json_num(buf, "others", &v)) g_others = v != 0;
    if (json_num(buf, "labels", &v)) g_labels = v != 0;
    if (json_num(buf, "backdrop", &v) && v >= 0 && v <= 2) g_backdrop = (int)v;
    if (json_num(buf, "bdWidth", &v) && v >= 0 && v <= 3) g_bd_w = (int)v;
    if (json_num(buf, "bdStrength", &v) && v >= 0 && v <= 3) g_bd_a = (int)v;
    if (json_num(buf, "bdCentre", &v)) BACKDROP_CORE = v != 0;
    bd_apply();
    if (json_num(buf, "shadeColor", &v) && v >= 0 && v < 6) g_shade_col = (int)v;
    if (json_num(buf, "shadeCustom", &v) && v >= 0 && v <= 0xFFFFFF) g_shade_custom = (unsigned)v;
    if (json_num(buf, "previewLight", &v)) g_pv_light = v != 0;
    if (json_num(buf, "tipMiddle", &v)) g_chat_middle = v != 0;
    if (json_num(buf, "tipMono", &v)) g_chat_mono = v != 0;
    if (json_num(buf, "tipColor", &v) && v >= 0 && v < 6) g_chat_bg = (int)v;
    if (json_num(buf, "tipCustom", &v) && v >= 0 && v <= 0xFFFFFF) g_chat_custom = (unsigned)v;
    if (json_num(buf, "textColor", &v) && v >= 0 && v < 6) g_text_col = (int)v;
    if (json_num(buf, "textCustom", &v) && v >= 0 && v <= 0xFFFFFF) g_text_custom = (unsigned)v;
    if (json_num(buf, "wheelSpin", &v) && v >= 0 && v < 5) g_wheel_spin = (int)v;
    if (json_num(buf, "away", &v) && v >= 0) g_away_after = v;
    char sh[16], *c = strstr(buf, "\"shape\"");
    if (c && sscanf(c, "\"shape\" : \"%15[a-z0-9]\"", sh) == 1) snprintf(g_shape_name, sizeof g_shape_name, "%s", sh);
    if ((c = strstr(buf, "\"frame\"")) && sscanf(c, "\"frame\" : \"%15[a-z]\"", sh) == 1) snprintf(g_frame, sizeof g_frame, "%s", sh);
    if ((c = strstr(buf, "\"smooth\"")) && sscanf(c, "\"smooth\" : \"%15[a-z]\"", sh) == 1)
        for (int i = 0; i < SM_N; i++) if (!strcmp(sh, SMOOTH_NAME[i])) g_smooth = i;
    Smooth cu;
    if ((c = strstr(buf, "\"smoothCustom\"")) && sscanf(c, "\"smoothCustom\" : [ %f , %f , %f , %f , %f ]", &cu.idle, &cu.think, &cu.busy, &cu.rest, &cu.hand) == 5)
        g_smooth_custom = cu;
    if (json_num(buf, "threads", &v) && v >= 0 && v <= 8) g_threads = (int)v;
    if (json_num(buf, "holeGpu", &v)) g_gpu = v != 0;
    if (json_num(buf, "setW", &v) && v >= 620 && v <= 6000) g_set_w = v; /* the Settings window itself */
    if (json_num(buf, "setH", &v) && v >= 480 && v <= 6000) g_set_h = v;
    if (json_num(buf, "setSide", &v)) g_set_side = v != 0;
    if (json_num(buf, "moveMod", &v) && (v == VK_MENU || v == VK_CONTROL || v == VK_SHIFT)) g_move_mod = (int)v; /* Settings > Shortcuts */
    if (json_num(buf, "sendCtrl", &v)) g_send_ctrl = v != 0;
    static const char *HKK[4] = {"hkMode", "hkHide", "hkReply", "hkSettings"};
    char *hks[4] = {g_mode_hotkey, g_hk_hide, g_hk_reply, g_hk_set}, pat[48], hv[32];
    for (int i = 0; i < 4; i++) { snprintf(pat, sizeof pat, "\"%s\" : \"%%31[a-z0-9+]\"", HKK[i]); if ((c = strstr(buf, HKK[i]) ? strstr(buf, HKK[i]) - 1 : NULL) && sscanf(c, pat, hv) == 1) snprintf(hks[i], 32, "%s", hv); }
    uint32_t rt[ST_N], act[ST_N]; /* Settings > Motions */
    MotionMap def[ST_N];
    motion_map_init();
    motion_map_default(def);
    if (json_num(buf, "holeActs", &v)) { /* the black hole's own actions; any added since the file was written start on */
        float nv = 6;
        json_num(buf, "holeActsN", &nv);
        HOLE_ACTS = ((uint32_t)v | (((1u << HA_N) - 1) & ~((1u << (int)nv) - 1))) & HOLE_ACTS_DEF;
    }
    if (json_num(buf, "readyAttacks", &v)) READY_ATKS = (uint32_t)v & ((1u << RA_N) - 2); /* READY's attacks */
    if (json_masks(buf, "mapRoutines", rt) && json_masks(buf, "mapActions", act))
        for (int i = 0; i < ST_N; i++) {
            if (rt[i] == UINT32_MAX || act[i] == UINT32_MAX) { MOTION_MAP[i] = def[i]; continue; } /* a state the file is older than */
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
            g_font, g_core, g_scale, g_glow, g_gravity, g_chat_size, g_others, g_labels, g_chat_middle, g_chat_mono, g_chat_bg, g_chat_custom, g_text_col, g_text_custom, g_wheel_spin, g_away_after, g_shape_name, g_frame);
    fseek(f, -2, SEEK_CUR); /* back over "}\n": the motion map goes in the same object */
    fprintf(f, ", \"mapRoutines\": [");
    for (int i = 0; i < ST_N; i++) fprintf(f, "%s%lu", i ? ", " : "", (unsigned long)MOTION_MAP[i].rt);
    fprintf(f, "], \"holeTilt\": %g, \"holeActs\": %lu, \"holeActsN\": %d, \"readyAttacks\": %lu, \"mapActions\": [", HOLE_TILT, (unsigned long)HOLE_ACTS, HA_N, (unsigned long)READY_ATKS);
    for (int i = 0; i < ST_N; i++) fprintf(f, "%s%lu", i ? ", " : "", (unsigned long)MOTION_MAP[i].act);
    const Smooth *cu = &g_smooth_custom;
    fprintf(f, "], \"smooth\": \"%s\", \"smoothCustom\": [%g, %g, %g, %g, %g], \"threads\": %d, \"holeGpu\": %d, ", SMOOTH_NAME[g_smooth], cu->idle, cu->think, cu->busy, cu->rest, cu->hand, g_threads, g_gpu);
    fprintf(f, "\"setW\": %g, \"setH\": %g, \"setSide\": %d, \"hkMode\": \"%s\", \"hkHide\": \"%s\", \"hkReply\": \"%s\", \"hkSettings\": \"%s\", \"moveMod\": %d, \"sendCtrl\": %d, \"backdrop\": %d, \"bdWidth\": %d, \"bdStrength\": %d, \"bdCentre\": %d, \"shadeColor\": %d, \"shadeCustom\": %u, \"previewLight\": %d}\n",
            g_set_w, g_set_h, g_set_side, g_mode_hotkey, g_hk_hide, g_hk_reply, g_hk_set, g_move_mod, g_send_ctrl, g_backdrop, g_bd_w, g_bd_a, BACKDROP_CORE, g_shade_col, g_shade_custom, g_pv_light);
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

/* shows the bitmap at (x, y) on the screen; dirty: only that part of it changed (NULL: all of it) */
static void pane_show_dirty(Pane *p, int x, int y, const RECT *dirty) {
    POINT src = {0, 0}, dst = {x, y};
    SIZE size = {p->W, p->H};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    double u0 = g_stats ? qpc_ms() : 0;
    RECT one = {0, 0, 1, 1};
    if (dirty && p->shown) { /* only the dirty part is copied (an empty one: a pixel, so the window still moves) */
        UPDATELAYEREDWINDOWINFO ui = {sizeof ui, NULL, &dst, &size, p->mem, &src, 0, &blend, ULW_ALPHA, IsRectEmpty(dirty) ? &one : dirty};
        if (!UpdateLayeredWindowIndirect(p->w, &ui)) UpdateLayeredWindow(p->w, NULL, &dst, &size, p->mem, &src, 0, &blend, ULW_ALPHA);
    } else UpdateLayeredWindow(p->w, NULL, &dst, &size, p->mem, &src, 0, &blend, ULW_ALPHA);
    if (g_stats) g_stat_push_ms += qpc_ms() - u0;
    p->x = x; p->y = y; p->dirty = 0;
    g_stat_push++;
    if (!p->shown) {
        SetWindowPos(p->w, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW); p->shown = 1;
    }
}

static void pane_show(Pane *p, int x, int y) { pane_show_dirty(p, x, y, NULL); }

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

/* ---- monitors: each has its own scale (dpi) and refresh rate. The panel is per-monitor DPI aware, so
 * Windows never stretches its bitmaps: the cores are drawn at the scale of the monitor the main core is on. */
static double dpi_at(float x, float y) {
    static HRESULT(WINAPI * get)(HMONITOR, int, UINT *, UINT *);
    static int tried;
    if (!tried) { tried = 1; HMODULE sh = LoadLibraryW(L"shcore.dll"); if (sh) get = (void *)GetProcAddress(sh, "GetDpiForMonitor"); }
    UINT dx = 96, dy = 96;
    POINT p = {(LONG)x, (LONG)y};
    if (get && get(MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST), 0 /* MDT_EFFECTIVE_DPI */, &dx, &dy) == S_OK) return dx / 96.0;
    HDC screen = GetDC(NULL);
    double d = GetDeviceCaps(screen, LOGPIXELSX) / 96.0;
    ReleaseDC(NULL, screen);
    return d;
}
static double hz_at(float x, float y) {
    MONITORINFOEXW mi;
    mi.cbSize = sizeof mi;
    POINT p = {(LONG)x, (LONG)y};
    DEVMODEW dm;
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    if (GetMonitorInfoW(MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST), (MONITORINFO *)&mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)
        && dm.dmDisplayFrequency > 1)
        return dm.dmDisplayFrequency;
    return 60;
}

/* where it was put last, else near the top of the primary screen's right edge */
static void main_place(void) {
    g_dpi = dpi_at(g_work.right - 1.0f, g_work.top + 1.0f);
    g_col_w = (int)(g_col_dip * g_dpi);
    g_cw = (int)(fmax(200 * g_dpi, fmin(320 * g_dpi, g_col_w)) * g_scale);
    Pos *p = pos_find("@main");
    if (p) { g_mx = (float)p->x; g_my = (float)p->y; }
    else { g_mx = g_work.right - g_cw * .5f; g_my = g_work.top + g_cw * .6f; }
    clamp_main();
    void dpi_follow(int force);
    dpi_follow(1);
}

/* ---- drawing -------------------------------------------------------------------------------------- */
/* Each core's picture is kept and made again only as often as its state needs (session_rate, as in the macOS
 * panel: asleep 4 a second, idle 10, working 20, more while the mouse is on it, 60 while it is in the hand).
 * A window gets a new bitmap only when one of its pictures changed; moving it is just a new position. */
typedef struct { char sid[72]; uint32_t *img; int px; size_t cap; double due; float fx, fy; } Pic; /* premultiplied BGRA; drawn (fx, fy) px right and down */
static Pic g_pic[MAX_SESS];
static int g_orbiting;              /* small cores on screen that drift (single-core mode): something always moves */
static double g_need_hz;            /* the highest picture rate a core on screen wants now: the clock keeps up with it */

/* k: 1.5 for the cores on the desktop (the Smoothness as it is), lower or higher for the Settings preview */
static double core_rate(const Session *s, double now, float k) {
    if (s->mo.atk_busy || s->mo.lock_on > 0) return g_hz; /* READY attacking: every refresh of the display, as the Bestiary draws it */
    if (g_down_sid[0] && !strcmp(s->sid, g_down_sid) && (g_dragging || g_moving)) { /* in the hand */
        double h = smooth_now()->hand;
        return h <= 0 || h > g_hz ? g_hz : h;
    }
    double r = panel_rate(s, now, smooth_now(), g_hz) * k / 1.5;
    return r > g_hz ? g_hz : r < 2 ? 2 : r;
}

/* ---- drawing on several cores: a picture is cut into bands of rows, drawn and converted at the same time (READY's
 * hits light up hundreds of pixels across, and one core could not draw them in a 120 Hz frame). Every band gets the
 * same strokes in the same order, so the picture is the same to the bit as one drawn whole. */
#define WK_MAX 7
static int g_nwork = -1;             /* helper threads (0: draw alone; -1: not started yet) */
static int g_auto_work;              /* Auto's helpers: about one a physical core, leaving one for everything else */
static HANDLE g_wk_ev[WK_MAX], g_wk_done;
static volatile LONG g_wk_next, g_wk_active;
static int g_wk_n;
static void (*g_wk_fn)(int, void *);
static void *g_wk_arg;
static void wk_pull(void) {
    for (;;) {
        LONG i = InterlockedIncrement(&g_wk_next) - 1;
        if (i >= g_wk_n) return;
        g_wk_fn((int)i, g_wk_arg);
    }
}
static DWORD WINAPI wk_main(LPVOID a) {
    HANDLE ev = g_wk_ev[(intptr_t)a];
    for (;;) {
        WaitForSingleObject(ev, INFINITE);
        wk_pull();
        if (InterlockedDecrement(&g_wk_active) == 0) SetEvent(g_wk_done);
    }
    return 0;
}
static void wk_start(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int n = (int)si.dwNumberOfProcessors / 2 - 1, most = (int)si.dwNumberOfProcessors - 1;
    g_auto_work = n < 0 ? 0 : n > WK_MAX ? WK_MAX : n;
    g_nwork = most < 0 ? 0 : most > WK_MAX ? WK_MAX : most; /* (started all at once; idle ones only wait) */
    g_wk_done = CreateEvent(NULL, FALSE, FALSE, NULL);
    int pri = GetThreadPriority(GetCurrentThread());
    for (int i = 0; i < g_nwork; i++) {
        g_wk_ev[i] = CreateEvent(NULL, FALSE, FALSE, NULL);
        HANDLE h = CreateThread(NULL, 0, wk_main, (LPVOID)(intptr_t)i, 0, NULL);
        if (!h || !g_wk_ev[i] || !g_wk_done) { g_nwork = i; break; }
        SetThreadPriority(h, pri);
        CloseHandle(h);
    }
}
/* fn(0..n-1, arg), shared out between this thread and the helpers; back when all are done. Only the helpers woken for
 * this round run it, and each is finished before it returns, so a round never mixes with the next. */
static int wk_use(void) { /* the helpers in use now */
    if (g_nwork < 0) wk_start();
    int u = g_threads > 0 ? g_threads - 1 : g_auto_work;
    return u < g_nwork ? u : g_nwork;
}
static void par_for(int n, void (*fn)(int, void *), void *arg) {
    int use = wk_use();
    if (n <= 1 || use == 0) { for (int i = 0; i < n; i++) fn(i, arg); return; }
    int k = n - 1 < use ? n - 1 : use;
    g_wk_fn = fn; g_wk_arg = arg; g_wk_n = n;
    InterlockedExchange(&g_wk_next, 0);
    InterlockedExchange(&g_wk_active, k);
    for (int i = 0; i < k; i++) SetEvent(g_wk_ev[i]);
    wk_pull();
    WaitForSingleObject(g_wk_done, INFINITE);
}
static int bands_n(void) { int u = wk_use(); return u ? 2 * (u + 1) : 1; } /* (more bands than threads: the drawing is never spread evenly) */

#define BAND_MAX 16
typedef struct {
    Canvas *c; const Session *s; double now; int x0, y0, x1, y1, nb, cut[BAND_MAX + 1];
    Canvas band[BAND_MAX];
    uint8_t *rgba; uint32_t *px; int cx0, cx1;  /* converting: into rgba (w*h*4), then px (BGRA, w a row) if set, columns cx0..cx1 */
} Bands;
static void band_draw(int i, void *arg) {
    Bands *B = arg;
    Canvas *b = &B->band[i];
    *b = *B->c; b->ry0 = B->cut[i]; b->ry1 = B->cut[i + 1];
    render_band(b, B->s, &g_shape, B->now, g_glow, g_core, B->x0, B->y0, B->x1, B->y1);
}
static void band_convert(int i, void *arg) {
    Bands *B = arg;
    int y0 = B->cut[i], y1 = B->cut[i + 1], w = B->c->w;
    if (y1 <= y0) return;
    canvas_rgba_box(B->c, B->rgba, B->cx0, y0, B->cx1, y1);
    if (B->px)
        for (int y = y0; y < y1; y++) {
            const uint8_t *src = B->rgba + ((size_t)y * w + B->cx0) * 4;
            uint32_t *dst = B->px + (size_t)y * w + B->cx0;
            for (int x = B->cx0; x < B->cx1; x++, src += 4) *dst++ = (uint32_t)src[2] | (uint32_t)src[1] << 8 | (uint32_t)src[0] << 16 | (uint32_t)src[3] << 24; /* RGBA → BGRA, both premultiplied */
        }
}
/* cut rows 0..h into nb bands, spread evenly over r0..r1 (where the drawing is): the first from row 0, the last to h */
static void bands_cut(Bands *B, int nb, int h, int r0, int r1) {
    if (nb > BAND_MAX) nb = BAND_MAX;
    if (nb < 1) nb = 1;
    if (r0 < 0) r0 = 0;
    if (r1 > h) r1 = h;
    if (r1 <= r0) { r0 = 0; r1 = h; }
    B->nb = nb; B->cut[0] = 0; B->cut[nb] = h;
    for (int i = 1; i < nb; i++) B->cut[i] = r0 + (int)((long long)(r1 - r0) * i / nb);
}
/* the session drawn into c (render_prep done) laid out in x0..x1, y0..y1, in bands over the rows r0..r1 */
static void draw_banded(Bands *B, Canvas *c, const Session *s, double now, int x0, int y0, int x1, int y1, int r0, int r1) {
    B->c = c; B->s = s; B->now = now; B->x0 = x0; B->y0 = y0; B->x1 = x1; B->y1 = y1;
    bands_cut(B, bands_n(), c->h, r0, r1);
    par_for(B->nb, band_draw, B);
    for (int i = 0; i < B->nb; i++) canvas_box_merge(c, &B->band[i]);
}
/* the canvas's rows y0..y1, columns x0..x1, into rgba and (if px) px, in bands */
static void convert_banded(Bands *B, Canvas *c, uint8_t *rgba, uint32_t *px, int x0, int y0, int x1, int y1) {
    B->c = c; B->rgba = rgba; B->px = px; B->cx0 = x0; B->cx1 = x1;
    int nb = bands_n();
    if ((long long)(x1 - x0) * (y1 - y0) < 20000) nb = 1; /* (a small one: not worth waking anyone) */
    bands_cut(B, nb, y1, y0, y1);
    B->cut[0] = y0;
    par_for(B->nb, band_convert, B);
}

static void pics_invalidate(void) { for (int i = 0; i < MAX_SESS; i++) g_pic[i].due = 0; }

/* a session's picture at px pixels, made again if it is due; 1 if it changed */
static int pic_make_at(Pic *p, const Session *s, int px, double now, float k, float fx, float fy) {
    if (px < 8) px = 8;
    if (p->img && p->px == px && now < p->due && !strcmp(p->sid, s->sid) && p->fx == fx && p->fy == fy) return 0;
    size_t need = (size_t)px * px;
    if (need > p->cap) { free(p->img); p->img = malloc(need * 4); p->cap = need; }
    double r0 = g_stats ? qpc_ms() : 0;
    int ss = px <= 96 ? 2 : 1; /* a small core is drawn at twice the size and shrunk: cleaner edges where pixels are few */
    size_t big = need * ss * ss;
    BH_GPU_IMG.rgba = NULL;
    BH_GPU_ON = g_gpu && gpu_init(); /* (on the GPU: the CPU builds no maps of its own) */
    if (g_gpu && s->mo.rt == RT_HOLE && s->mo.hole > 0.01f) { /* a black hole: the GPU draws it, if it can (else the CPU, as before) */
        double g0 = g_stats ? qpc_ms() : 0;
        /* its picture at exactly the core's scale (growing with the hole as it forms), so it is laid in pixel for pixel */
        float E = bh_hole_E(s, px * ss);
        const unsigned char *img = gpu_hole(s, px * ss, E, now, s->pad <= 1); /* (a small core: a mini hole) */
        if (img) BH_GPU_IMG = (BhGpuImg){s, px * ss, E, img};
        else { static int said; if (!said) { said = 1; fprintf(stderr, "vigil-panel: the black hole stays on the CPU: %s\n", gpu_why()); } }
        if (g_stats) { g_stat_gpu_ms += qpc_ms() - g0; g_stat_gpu_n++; }
    }
    canvas_resize(&g_cv, px * ss, px * ss);
    g_cv.sx = fx * ss; g_cv.sy = fy * ss; g_cv.ss = ss;
    if (big * 4 > g_rgba_cap) { free(g_rgba); g_rgba = malloc(big * 4); g_rgba_cap = big * 4; }
    static Bands B;
    if (ss == 1 && px >= 128) { /* a big one: drawn and converted in bands, on several cores */
        render_prep(&g_cv, 1);
        draw_banded(&B, &g_cv, s, now, 0, 0, px, px, 0, px);
        g_cv.sx = g_cv.sy = 0; g_cv.ss = 0;
        convert_banded(&B, &g_cv, g_rgba, p->img, 0, 0, px, px);
    } else {
        render_one(&g_cv, s, &g_shape, now, g_glow, g_core);
        g_cv.sx = g_cv.sy = 0; g_cv.ss = 0;
        canvas_rgba(&g_cv, g_rgba);
    }
    if (ss == 1) {
        if (px < 128) {
            const uint8_t *src = g_rgba;
            for (size_t j = 0; j < need; j++, src += 4) /* RGBA → BGRA, both premultiplied */
                p->img[j] = (uint32_t)src[2] | (uint32_t)src[1] << 8 | (uint32_t)src[0] << 16 | (uint32_t)src[3] << 24;
        }
    } else
        for (int y = 0; y < px; y++) for (int x = 0; x < px; x++) { /* the average of each 2x2 (premultiplied, so a plain mean) */
            const uint8_t *a = g_rgba + ((size_t)(2 * y) * px * 2 + 2 * x) * 4, *b = a + (size_t)px * 2 * 4;
            unsigned c[4];
            for (int k2 = 0; k2 < 4; k2++) c[k2] = (a[k2] + a[4 + k2] + b[k2] + b[4 + k2] + 2) / 4;
            p->img[(size_t)y * px + x] = c[2] | c[1] << 8 | c[0] << 16 | c[3] << 24;
        }
    snprintf(p->sid, sizeof p->sid, "%s", s->sid);
    p->px = px;
    p->fx = fx; p->fy = fy;
    BH_GPU_IMG.rgba = NULL;
    if (g_stats) g_stat_render_ms += qpc_ms() - r0;
    double ticks = floor(g_tick_hz / core_rate(s, now, k) + 0.5); /* every n-th tick, not "about every 1/rate s" */
    if (ticks < 1) ticks = 1;
    p->due = now + (ticks - 0.5) / g_tick_hz;
    g_stat_pic++;
    return 1;
}

static int pic_make(Pic *p, const Session *s, int px, double now, float k) { return pic_make_at(p, s, px, now, k, 0, 0); }
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

static RECT g_cluster_rc;           /* the main core and its orbit, on the desktop (the chat box goes beside it) */
static double g_last_draw;

/* a black hole gets a picture HOLE_PAD times its core's square, the core the same size in the middle: room for its disc,
 * a torn star's debris and the flare's blob (Session.pad); its window grows with it, the core's own square (hits) does not */
#define HOLE_PAD 2.6f
static float hole_pad(const Session *s) { return s->mo.rt == RT_HOLE || s->mo.hole > 0.01f ? HOLE_PAD : 1; }
/* a black hole's Precession: frame dragging whips the cores round it, faster and closer as if being sucked in, then
 * lets them go back: how strong now (0..1) */
static float hole_drag(const Session *s, double now) {
    const HoleRS *H = &s->mo.rs.hole;
    if (s->mo.rt != RT_HOLE || H->act != HA_PRECESS) return 0;
    float u = (float)((now - H->act_at) / HOLE_ACT_DUR[HA_PRECESS]);
    if (u <= 0 || u >= 1) return 0;
    float w = sinf((float)M_PI * u), fo = fminf(1, fmaxf(0, (float)(H->act_end - now) / 0.9f));
    return w * w * fo * fo * (3 - 2 * fo); /* (cut short: it lets go smoothly) */
}
static void draw_single(Room *room, int n, double now) {
    int fi = 0;
    for (int i = 0; i < n; i++) if (!strcmp(room->s[i].sid, g_focus)) fi = i;
    float dt = (float)fmin(0.1, now - g_last_draw);
    g_last_draw = now;
    static double orbit; /* the small cores' own clock: it stops while the pointer is on a core, so one stays put to be scrolled */
    float fd = 0;
    for (int i = 0; i < n; i++) if (!strcmp(room->s[i].sid, g_focus)) fd = hole_drag(&room->s[i], now);
    if (g_hover < 0 || fd > 0) orbit += dt * (1 + 9 * fd);
    float big = g_cw * 0.6f, rx = g_cw / 2.0f - (float)(8 * g_dpi), ry = rx * 1.15f;
    float small = g_others && n > 1 ? fminf(g_cw * 0.2f, (float)(2 * M_PI * rx / (n - 1) * 0.75)) : 0;
    rx -= small / 2; ry -= small / 2;
    rx *= 1 - .5f * fd; ry *= 1 - .62f * fd; /* (dragged round the hole, closer in) */
    /* the pull back onto the orbit (strong) and along it to its place (weaker: a carried core leaves it sliding round
     * the orbit to the trailing side, and it slides home when the core stops), where it lets go, where the core catches it */
    OrbitK ok = orbit_k(rx, small);
    int dragged = g_moving == 2 ? down_index() : -1;
    static int last_fi = -1;
    /* the main core asleep as a black hole: the small cores orbit it in the plane of its disc, tilting with it, faster
     * the closer in, passing behind it (their windows under its window there, so its shadow and disc hide them); a flung
     * one is pulled round by it and caught into orbit; you away, they drift out wide and slow */
    static float hk_e; static double hclk;
    const Session *mh = fi < n ? &room->s[fi] : NULL;
    hk_e += ((mh && is_hole(mh) ? 1.0f : 0.0f) - hk_e) * (1 - expf(-dt * 1.2f));
    float hax[3] = {0, 1, 0}, he1[3] = {1, 0, 0}, he2[3] = {0, 0, -1}, wide = mh ? mh->quiet : 0;
    if (hk_e > 0.01f) {
        bh_hole_axis(mh, now, hax);
        float l = hypotf(hax[1], hax[0]);
        if (l > 1e-3f) { he1[0] = hax[1] / l; he1[1] = -hax[0] / l; he1[2] = 0; } /* (the line where its disc crosses the screen) */
        he2[0] = hax[1] * he1[2] - hax[2] * he1[1]; he2[1] = hax[2] * he1[0] - hax[0] * he1[2]; he2[2] = hax[0] * he1[1] - hax[1] * he1[0];
        hclk += dt * (1 - 0.7f * wide);
    }
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
            if (hk_e > 0.01f) { /* round the black hole, in its disc */
                float Rk = rx * (0.9f + 0.1f * (k % 3)) * (1 + 0.45f * wide), wk = 0.5f * powf(rx / Rk, 1.5f);
                double ph = hclk * wk + 2 * M_PI * k / (n - 1);
                float c = (float)cos(ph), sn = (float)sin(ph), P3[3];
                for (int q = 0; q < 3; q++) P3[q] = c * he1[q] + sn * he2[q];
                tx += (g_mx + Rk * P3[0] - tx) * hk_e; ty += (g_my - Rk * P3[1] - ty) * hk_e;
                g_sat_behind[i] = hk_e > 0.5f && P3[2] < 0;
            } else g_sat_behind[i] = 0;
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
            if (hk_e > 0.5f) { /* a black hole pulls it round: a slingshot, then caught into orbit */
                float dx = g_mx - ob->px, dy = g_my - ob->py, r = hypotf(dx, dy) + 1, G = 9e6f * (float)(g_dpi * g_dpi) * hk_e, f = G / (r * r * r);
                ob->vx += dx * f * dt; ob->vy += dy * f * dt;
                if (r < rx * 1.6f && hypotf(ob->vx, ob->vy) < 900 * g_dpi) ob->free = 0; /* (caught: it settles into its orbit) */
            }
            fly_step(ob, dt);
            RECT wa = work_at(ob->px, ob->py);
            float hs = ob->w / 2 + 2;
            if (ob->px < wa.left + hs && !on_work(wa.left - 1.0f, ob->py)) { ob->px = wa.left + hs; ob->vx = fabsf(ob->vx) * BOUNCE; }
            if (ob->px > wa.right - hs && !on_work((float)wa.right, ob->py)) { ob->px = wa.right - hs; ob->vx = -fabsf(ob->vx) * BOUNCE; }
            if (ob->py < wa.top + hs && !on_work(ob->px, wa.top - 1.0f)) { ob->py = wa.top + hs; ob->vy = fabsf(ob->vy) * BOUNCE; }
            if (ob->py > wa.bottom - hs && !on_work(ob->px, (float)wa.bottom)) { ob->py = wa.bottom - hs; ob->vy = -fabsf(ob->vy) * BOUNCE; }
            if (hypotf(ob->px - g_mx, ob->py - g_my) < ok.CATCH) ob->free = 0; /* caught: the spring takes it home */
        } else if (hk_e > 0.5f) { /* round a black hole: drawn to its place on its orbit (critically damped: no overshoot) */
            const float W = 5;
            ob->vx += (W * W * (tx - ob->px) - 2 * W * ob->vx) * dt; ob->vy += (W * W * (ty - ob->py) - 2 * W * ob->vy) * dt;
            ob->px += ob->vx * dt; ob->py += ob->vy * dt;
        } else { /* on its orbit: a bead on a ring round the main core, held onto the ring and drawn along it to its place */
            float d = bead_step(ob, g_mx, g_my + bob, tx, ty, a, rx, ry, small, kt, &ok, ring_of, rn, dt);
            if (fd > 0.02f) { float f = fminf(1, fd * 3); ob->px += (tx - ob->px) * f; ob->py += (ty - ob->py) * f; ob->vx *= 1 - f; ob->vy *= 1 - f; } /* (whipped round: on the spiral, never flung off) */
            else if (d > ok.BREAK) ob->free = 1; /* pulled too far: it lets go, and flies on with the speed it had */
        }
        /* its window: the core, room round it for the focus frame, drawn at its exact place (part pixels too) */
        int w4 = ((int)ob->w / 4) * 4;
        Pane *sp = &g_pane[i];
        if (!sp->w) pane_create(sp);
        if (w4 < 12 || (i != fi && !g_others)) { pane_hide(sp); SetRectEmpty(&g_core_rc[i]); continue; }
        Session *ss = &room->s[i];
        ss->pad = i == fi ? hole_pad(ss) : 0;
        int wp = ss->pad > 1 ? ((int)(w4 * ss->pad) / 4) * 4 : w4, pad_off = (wp - w4) / 2; /* the picture, and the core's square in it */
        int m = frame_margin(w4), pw = wp + 2 * m + 1;
        float fx = ob->px - wp / 2.0f - m, fy = ob->py - wp / 2.0f - m;
        int ix = (int)floorf(fx), iy = (int)floorf(fy);
        /* a small core in eighths of a pixel (it is drawn on its beat anyway, so a slow drift glides); the main core in
         * halves, so its last tiny easing toward its place does not ask for a new picture every tick */
        float qd = i == fi ? 2 : 8, qx = floorf((fx - ix) * qd + 0.5f) / qd, qy = floorf((fy - iy) * qd + 0.5f) / qd;
        if (qx >= 1) { ix++; qx = 0; }
        if (qy >= 1) { iy++; qy = 0; }
        if (i != fi && i != dragged) {
            /* a small core moves and turns on one steady beat: its place and its picture change together, every n-th
             * tick (Balanced: 30 a second), never in between, so neither the glide nor the turning stutters */
            double busy = smooth_now()->busy, hz = busy <= 0 || busy > g_tick_hz ? g_tick_hz : busy;
            long every = (long)floor(g_tick_hz / hz + 0.5);
            if (every < 1) every = 1;
            if (sp->shown && sp->W == pw && !sp->dirty && g_tick_n % every) continue;
            g_pic[i].due = 0;
        }
        int pc = pic_make_at(&g_pic[i], &room->s[i], wp, now, 1.5f, qx, qy);
        static float lfx[MAX_SESS], lfy[MAX_SESS];
        int shifted = qx != lfx[i] || qy != lfy[i];
        SetRect(&g_core_rc[i], ix + m + pad_off, iy + m + pad_off, ix + m + pad_off + w4, iy + m + pad_off + w4);
        if (pc || shifted || sp->dirty || !sp->shown || sp->W != pw) {
            pane_size(sp, pw, pw);
            memset(sp->px, 0, (size_t)pw * pw * 4);
            blit_pic(sp, m, m, &g_pic[i]);
            RECT lr = {m + pad_off, m + pad_off, m + pad_off + w4, m + pad_off + w4};
            focus_frame(sp, lr, &room->s[i], now);
            pane_show(sp, ix, iy);
            lfx[i] = qx; lfy[i] = qy;
        } else pane_move(sp, ix, iy);
    }
    for (int i = n; i < MAX_SESS; i++) pane_hide(&g_pane[i]);
    { /* behind the black hole: under its window (so its shadow and disc lie over it); in front: over it */
        static signed char was[MAX_SESS];
        for (int i = 0; i < n && i < MAX_SESS; i++) {
            if (i == fi || !g_pane[i].w || !g_pane[fi].w) continue;
            signed char want = g_sat_behind[i] ? 1 : 2;
            if (want == was[i]) continue;
            was[i] = want;
            if (want == 1) SetWindowPos(g_pane[i].w, g_pane[fi].w, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            else SetWindowPos(g_pane[i].w, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }
    if (fi != last_fi && fi < n && g_pane[fi].w) { /* the main core on top */
        SetWindowPos(g_pane[fi].w, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); last_fi = fi;
    }
    g_top = fi;
    SetRect(&g_cluster_rc, (int)(g_mx - rx - small), (int)(g_my - ry - small), (int)(g_mx + rx + small), (int)(g_my + ry + small));
}

/* the main core is on another monitor now (carried there, or the layout changed): sizes, fonts and pictures
 * follow that monitor's scale, and the clock its refresh rate */
void dpi_follow(int force) {
    float x = g_single ? g_mx : g_work.right - 1.0f, y = g_single ? g_my : g_work.top + 1.0f;
    double d = dpi_at(x, y);
    g_hz = hz_at(x, y);
    if (!force && fabs(d - g_dpi) < 0.01) return;
    double k = d / g_dpi;
    g_dpi = d;
    g_col_w = (int)(g_col_dip * g_dpi);
    g_cw = (int)(fmax(200 * g_dpi, fmin(320 * g_dpi, g_col_w)) * g_scale);
    for (int i = 0; i < g_orbit_n; i++) { /* the small cores keep their place round the main one, at the new scale */
        Orbit *o = &g_orbit[i];
        o->px = g_mx + (o->px - g_mx) * (float)k; o->py = g_my + (o->py - g_my) * (float)k;
        o->vx *= (float)k; o->vy *= (float)k; o->w *= (float)k;
    }
    if (g_label_font) { DeleteObject(g_label_font); g_label_font = NULL; }
    if (k != 1) { void chat_rescale(void); chat_rescale(); }
    pics_invalidate();
}

static void chat_hide(void);
static void draw(double now) {
    Room *room = shown_room();
    int n = room->n > MAX_SESS ? MAX_SESS : room->n;
    if (g_hidden) { /* Show or hide Vigil: hidden, till the hotkey again */
        for (int i = 0; i < MAX_SESS; i++) pane_hide(&g_pane[i]);
        pane_hide(&g_cluster);
        chat_hide();
        g_core_n = 0;
        return;
    }
    dpi_follow(0);
    g_need_hz = 0;
    RENDER_PART = 1; /* READY's loose pieces and sparks go on the layer (layer_draw) */
    for (int i = 0; i < n; i++) { double r = core_rate(&room->s[i], now, 1.5f); if (r > g_need_hz) g_need_hz = r; }
    g_orbiting = g_single && g_others && n > 1;
    if (g_single) {
        pane_hide(&g_cluster); /* (only the owner of the clock, the hotkey and the menus now) */
        draw_single(room, n, now);
    } else {
        pane_hide(&g_cluster);
        draw_all(room, n, now);
    }
    RENDER_PART = 0;
    g_core_n = n;
}

/* READY's layer: a see-through window that never takes the mouse or the focus, over the part of the core's monitor
 * where its pieces fly (from the core to the pointer), so a piece can reach the pointer anywhere. Shown only while
 * pieces are out. */
static Pane g_layer;
static Canvas g_lcv;
static void layer_draw(double now) {
    Room *room = shown_room();
    int who = -1;
    for (int i = 0; i < room->n && i < MAX_SESS && i < g_core_n; i++) if (room->s[i].mo.loose_on && !IsRectEmpty(&g_core_rc[i])) { who = i; break; }
    if (who < 0 || g_hidden) { pane_hide(&g_layer); return; }
    if (!g_layer.w) pane_create(&g_layer);
    const Session *s = &room->s[who];
    RECT cr = g_core_rc[who], b = cr;
    float u = (cr.right - cr.left) * SHELL_FRAC, cx = (cr.left + cr.right) * .5f, cy = (cr.top + cr.bottom) * .5f, m = 1.8f * u;
    const ReadyRS *R = &s->mo.rs.ready;
    if (s->mo.rt == RT_RESTLESS)
        for (int k = 0; k < R->n; k++) if (R->p[k].st != RP_HOME) {
            RECT p = {(int)(cx + R->p[k].pos[0] * u - m), (int)(cy + R->p[k].pos[1] * u - m), (int)(cx + R->p[k].pos[0] * u + m), (int)(cy + R->p[k].pos[1] * u + m)};
            UnionRect(&b, &b, &p);
        }
    if (s->mo.rt == RT_RESTLESS)
        for (int k = 0; k < R->nsh; k++) if (!R->sh[k].home) { /* the shell's shards, blown out */
            RECT p = {(int)(cx + R->sh[k].base[0] * u - m), (int)(cy + R->sh[k].base[1] * u - m), (int)(cx + R->sh[k].base[0] * u + m), (int)(cy + R->sh[k].base[1] * u + m)};
            UnionRect(&b, &b, &p);
        }
    float mf = 4.2f * u; /* a blow's flash reaches about 3.5 shell radii (the Bestiary's size), a crash's too */
    for (int k = 0; k < 4; k++) if (now - s->mo.imp[k].at < .6) { /* a blow's burst and flash at the pointer */
        RECT p = {(int)(cx + s->mo.imp[k].x * u - mf), (int)(cy + s->mo.imp[k].y * u - mf), (int)(cx + s->mo.imp[k].x * u + mf), (int)(cy + s->mo.imp[k].y * u + mf)};
        UnionRect(&b, &b, &p);
    }
    if (R->nsh) { RECT p = {(int)(cx - mf), (int)(cy - mf), (int)(cx + mf), (int)(cy + mf)}; UnionRect(&b, &b, &p); } /* a crash into the core: its flash */
    if (s->aim_on) { RECT p = {(int)(cx + s->aim_x * u - m), (int)(cy + s->aim_y * u - m), (int)(cx + s->aim_x * u + m), (int)(cy + s->aim_y * u + m)}; UnionRect(&b, &b, &p); }
    RECT want = b; /* where the drawing will mostly be (screen) */
    MONITORINFO mi;
    memset(&mi, 0, sizeof mi); mi.cbSize = sizeof mi;
    GetMonitorInfo(MonitorFromRect(&cr, MONITOR_DEFAULTTONEAREST), &mi);
    RECT mon = mi.rcMonitor;
    UnionRect(&mon, &mon, &cr); /* (the core itself may hang over the edge) */
    IntersectRect(&b, &b, &mon);
    /* while pieces are out the window only grows, in 128-pixel steps: a new size means a new bitmap and canvas, and
     * pieces scattering after a hit would otherwise change it every frame */
    static RECT keep;
    if (!g_layer.shown || IsRectEmpty(&keep)) keep = b;
    else UnionRect(&keep, &keep, &b);
    { RECT q = keep; int gw = ((q.right - q.left + 127) / 128) * 128, gh = ((q.bottom - q.top + 127) / 128) * 128;
      q.left -= (gw - (q.right - q.left)) / 2; q.right = q.left + gw; q.top -= (gh - (q.bottom - q.top)) / 2; q.bottom = q.top + gh;
      if (q.left < mon.left) OffsetRect(&q, mon.left - q.left, 0);
      if (q.right > mon.right) OffsetRect(&q, mon.right - q.right, 0);
      if (q.top < mon.top) OffsetRect(&q, 0, mon.top - q.top);
      if (q.bottom > mon.bottom) OffsetRect(&q, 0, mon.bottom - q.bottom);
      IntersectRect(&b, &q, &mon); keep = b; }
    int W = b.right - b.left, H = b.bottom - b.top;
    if (W < 8 || H < 8) { pane_hide(&g_layer); return; }
    /* Only what changed is redone: the pieces cover a sliver of the window, and clearing, converting and handing
     * Windows all of it every refresh took longer than a frame at 120 Hz (the pieces stuttered). The canvas keeps the
     * box it drew last; that box is cleared, the new one drawn, and the two together converted into the window's
     * bitmap and handed over as the window's dirty part. A new size starts all over (canvas_resize marks it all). */
    int sized = W != g_layer.W || H != g_layer.H || !g_layer.bmp;
    canvas_resize(&g_lcv, W, H);
    RECT was = {g_lcv.bx0, g_lcv.by0, g_lcv.bx1, g_lcv.by1};
    g_lcv.sx = g_pic[who].fx; g_lcv.sy = g_pic[who].fy; /* the same part-pixel place as its core's picture, so the pieces never step against it */
    static Bands B;
    RENDER_PART = 2;
    render_prep(&g_lcv, 0);
    { int r0 = want.top - b.top, r1 = want.bottom - b.top; /* the bands spread over where it drew last and where the pieces are now */
      if (!IsRectEmpty(&was)) { if (was.top < r0) r0 = was.top; if (was.bottom > r1) r1 = was.bottom; }
      draw_banded(&B, &g_lcv, s, now, cr.left - b.left, cr.top - b.top, cr.right - b.left, cr.bottom - b.top, r0, r1); }
    RENDER_PART = 0;
    g_lcv.sx = g_lcv.sy = 0;
    RECT now_rc = {g_lcv.bx0, g_lcv.by0, g_lcv.bx1, g_lcv.by1}, dirty;
    if (IsRectEmpty(&was)) dirty = now_rc;
    else if (IsRectEmpty(&now_rc)) dirty = was;
    else UnionRect(&dirty, &was, &now_rc);
    if (sized) SetRect(&dirty, 0, 0, W, H);
    size_t need = (size_t)W * H;
    if (need * 4 > g_rgba_cap) { free(g_rgba); g_rgba = malloc(need * 4); g_rgba_cap = need * 4; }
    pane_size(&g_layer, W, H);
    if (!IsRectEmpty(&dirty)) convert_banded(&B, &g_lcv, g_rgba, g_layer.px, dirty.left, dirty.top, dirty.right, dirty.bottom);
    pane_show_dirty(&g_layer, b.left, b.top, sized ? NULL : &dirty);
    if (g_layer.shown) SetWindowPos(g_layer.w, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); /* over the cores */
}

/* ---- the chat box: name and state, then the last reply (or the permission notice) --------------------- */
/* The hook keeps them in ~/.dodeca-core/msg/<sid>.json (only with DODECA_RICH=1); read again when the file
 * changes. --demo makes them up, like the macOS panel. */
static const char *DEMO_REPLY[] = {
    "Refactored the channel model so fading is applied per tap.\n\n- `channel.py`: one Rayleigh draw per tap, not per frame\n- `sim.py`: the seed covers the noise too, so runs repeat\n- `test_ber.py`: all 42 tests pass\n\nThe BER curve now matches the reference within 0.2 dB:\n\n```\nEb/N0   ref      ours\n 4 dB   1.2e-2   1.2e-2\n 8 dB   3.1e-4   3.3e-4\n12 dB   1.8e-6   1.9e-6\n```\n\n**How I checked**\n\n1. Ran the 42 tests three times with fixed seeds\n2. Swept Eb/N0 from 0 to 14 dB in 1 dB steps\n3. Compared each point with `reference.csv`\n\n**Files changed**\n\n- `channel.py` +48 -31\n- `sim.py` +12 -4\n- `test_ber.py` +20\n\nNext I can add **Doppler** if you want.",
    "The lock now debounces the reed switch for 30 ms, and a second unlock waits for the motor:\n\n| Case | Before | After |\n|---|---|---|\n| Bouncy switch | *2-3 unlocks* | **1 unlock** |\n| Unlock while moving | race | `queued` |\n\nAll 18 lock tests pass.",
    "整理好今天的筆記了：三個待辦、兩個已完成。\n要不要我把待辦排進明天的計畫？",
    "Bumped the bootloader to v2.3 and rebuilt the image.\nFlash it with `make flash`; the OTA path is unchanged.",
    "Swept 2.40-2.48 GHz: best match at 2.437 GHz, S11 -18 dB.\nThe matching network values are in tune.csv.",
    "Scratch test done. Nothing to keep.",
};
static const char *DEMO_NOTICE = "Claude needs your permission to use Bash";
static const char *DEMO_PROMPT[] = {"Fading looks wrong per frame. Make it per tap and check the BER curve against the reference.",
    "The lock double-unlocks sometimes", "幫我整理今天的筆記", "Bump the bootloader and rebuild", "Find the best match frequency", "Try the scratch test"};

static void put_utf8(char **o, unsigned c) {
    char *p = *o;
    if (c < 0x80) *p++ = (char)c;
    else if (c < 0x800) { *p++ = (char)(0xC0 | c >> 6); *p++ = (char)(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { *p++ = (char)(0xE0 | c >> 12); *p++ = (char)(0x80 | (c >> 6 & 0x3F)); *p++ = (char)(0x80 | (c & 0x3F)); }
    else { *p++ = (char)(0xF0 | c >> 18); *p++ = (char)(0x80 | (c >> 12 & 0x3F)); *p++ = (char)(0x80 | (c >> 6 & 0x3F)); *p++ = (char)(0x80 | (c & 0x3F)); }
    *o = p;
}

/* the string value of "field" in a flat JSON object, as UTF-8 (malloc'd), or NULL */
static char *json_unq(const char *c);
static char *json_str(const char *j, const char *field) {
    char key[40];
    snprintf(key, sizeof key, "\"%s\"", field);
    const char *c = strstr(j, key);
    if (!c) return NULL;
    c += strlen(key);
    while (*c == ' ' || *c == ':') c++;
    return json_unq(c);
}
/* a JSON string (c at its opening quote) as UTF-8, NULL when it is not one or empty */
static char *json_unq(const char *c) {
    if (!c || *c != '"') return NULL;
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

/* walking the msg file's nested parts (the turns): skip a value, find a key of an object */
static const char *js_ws(const char *c) { while (*c == ' ' || *c == '\n' || *c == '\r' || *c == '\t') c++; return c; }
static const char *js_skip(const char *c) {
    c = js_ws(c);
    if (*c == '"') { for (c++; *c && *c != '"'; c++) if (*c == '\\' && c[1]) c++; return *c ? c + 1 : c; }
    if (*c == '{' || *c == '[') {
        int depth = 0;
        for (; *c; c++) {
            if (*c == '"') { c = js_skip(c) - 1; continue; }
            if (*c == '{' || *c == '[') depth++;
            else if ((*c == '}' || *c == ']') && --depth == 0) return c + 1;
        }
        return c;
    }
    while (*c && *c != ',' && *c != '}' && *c != ']') c++;
    return c;
}
/* the value of key in the object at obj ('{'), or NULL; *kname, when given, walks every key instead (key NULL) */
static const char *js_key(const char *obj, const char *key) {
    if (!obj || *(obj = js_ws(obj)) != '{') return NULL;
    size_t n = strlen(key);
    for (const char *c = js_ws(obj + 1); *c == '"';) {
        int hit = !strncmp(c + 1, key, n) && c[1 + n] == '"';
        c = js_ws(js_skip(c));
        if (*c != ':') return NULL;
        c = js_ws(c + 1);
        if (hit) return c;
        c = js_ws(js_skip(c));
        if (*c != ',') return NULL;
        c = js_ws(c + 1);
    }
    return NULL;
}
/* the elements of an array: start with c = the '[', each call gives the next element or NULL */
static const char *js_next(const char **c) {
    const char *p = js_ws(*c);
    if (*p == '[') p = js_ws(p + 1);
    else if (*p == ',') p = js_ws(p + 1);
    if (!*p || *p == ']') return NULL;
    *c = js_skip(p);
    return p;
}

/* The last few turns as Claude Code's terminal shows them: the prompt, its steps (a tool and what it works on),
 * the reply; the hook keeps up to 5 in "turns" (the last one is going on), and the step running now in "run". */
enum { MT_MAX = 5, MT_STEPS = 8 };
typedef struct { char *p, *r, *tally, *tool[MT_STEPS], *what[MT_STEPS]; int n, ns; } MTurn;
static MTurn g_mt[MT_MAX];
static int g_mt_n;
static char *g_run_tool, *g_run_what;

static void mt_free(void) {
    for (int i = 0; i < g_mt_n; i++) {
        MTurn *t = &g_mt[i];
        free(t->p); free(t->r); free(t->tally);
        for (int k = 0; k < t->ns; k++) { free(t->tool[k]); free(t->what[k]); }
    }
    memset(g_mt, 0, sizeof g_mt);
    g_mt_n = 0;
    free(g_run_tool); free(g_run_what); g_run_tool = g_run_what = NULL;
}
static int js_int(const char *v) { return v ? atoi(v) : 0; }
static void mt_load(const char *buf) {
    const char *top = js_ws(buf), *arr = js_key(top, "turns"), *run = js_key(top, "run"), *e;
    if (run && *run == '[') { const char *c = run, *a = js_next(&c), *b = a ? js_next(&c) : NULL; g_run_tool = json_unq(a); g_run_what = json_unq(b); }
    if (!arr || *arr != '[') return;
    const char *all[64];
    int n = 0;
    for (const char *c = arr; (e = js_next(&c)) && n < 64;) all[n++] = e;
    for (int i = n > MT_MAX ? n - MT_MAX : 0; i < n; i++) {
        MTurn *t = &g_mt[g_mt_n++];
        t->p = json_unq(js_key(all[i], "p"));
        t->r = json_unq(js_key(all[i], "r"));
        t->n = js_int(js_key(all[i], "n"));
        const char *s = js_key(all[i], "s"), *st;
        if (s && *s == '[')
            for (const char *c = s; (st = js_next(&c)) && t->ns < MT_STEPS;) {
                const char *d = st, *a = js_next(&d), *b = a ? js_next(&d) : NULL;
                char *tool = json_unq(a);
                if (!tool) continue;
                t->tool[t->ns] = tool; t->what[t->ns] = json_unq(b); t->ns++;
            }
        /* the tally of a finished turn: "12 steps · Bash 5 · Edit 4 · Read 3", the most used first */
        const char *cnt = js_key(all[i], "c");
        char name[12][41]; int num[12], m = 0;
        if (cnt && *cnt == '{')
            for (const char *c = js_ws(cnt + 1); *c == '"' && m < 12;) {
                char *k = json_unq(c);
                c = js_ws(js_skip(c));
                if (*c != ':') { free(k); break; }
                c = js_ws(c + 1);
                if (k) { snprintf(name[m], 41, "%s", k); num[m++] = atoi(c); free(k); }
                c = js_ws(js_skip(c));
                if (*c != ',') break;
                c = js_ws(c + 1);
            }
        for (int a = 0; a < m; a++) for (int b = a + 1; b < m; b++) if (num[b] > num[a]) {
            int tn = num[a]; num[a] = num[b]; num[b] = tn;
            char tmp[41]; memcpy(tmp, name[a], 41); memcpy(name[a], name[b], 41); memcpy(name[b], tmp, 41);
        }
        if (t->n) {
            char line[300];
            int at = snprintf(line, sizeof line, "%d step%s", t->n, t->n == 1 ? "" : "s");
            for (int a = 0; a < m && a < 4 && at < (int)sizeof line - 60; a++) at += snprintf(line + at, sizeof line - at, " \xC2\xB7 %s %d", name[a], num[a]);
            t->tally = _strdup(line);
        }
    }
}

static wchar_t *g_pend;               /* a reply held while its session asks you something (see deliver) */
static char g_pend_sid[72];
static char g_msg_sid[72], *g_msg_reply, *g_msg_notice, *g_msg_tmux, *g_msg_inbox, *g_msg_wt; /* inbox: a vigil-claude launcher's pid; wt: its Windows Terminal tab */
static FILETIME g_msg_mtime;
static double g_msg_checked;

static void msg_load(const Session *s, double now) {
    int same = !strcmp(g_msg_sid, s->sid);
    if (same && now - g_msg_checked < 0.5) return; /* over \\wsl.localhost: not every frame */
    g_msg_checked = now;
    if (g_demo_n) {
        if (same) return;
        free(g_msg_reply); free(g_msg_notice); free(g_msg_tmux); free(g_msg_inbox); free(g_msg_wt); g_msg_reply = g_msg_notice = g_msg_tmux = g_msg_inbox = g_msg_wt = NULL;
        mt_free();
        snprintf(g_msg_sid, sizeof g_msg_sid, "%s", s->sid);
        int i;
        if (sscanf(s->sid, "demo-%d", &i) == 1 && i >= 0 && i < 6) {
            g_msg_reply = _strdup(DEMO_REPLY[i]); g_msg_notice = _strdup(DEMO_NOTICE);
            char demo[6000]; /* an earlier turn, then this one with its steps */
            snprintf(demo, sizeof demo, "{\"turns\": [{\"p\": \"Run the tests first\", \"r\": \"All 42 pass; two are slow (over 3 s).\", \"n\": 2, \"c\": {\"Bash\": 2}},"
                     " {\"p\": \"%s\", \"r\": \"%s\", \"n\": 5, \"s\": [[\"Grep\", \"rayleigh\"], [\"Read\", \"channel.py\"], [\"Edit\", \"channel.py\"], [\"Bash\", \"pytest -q\"]],"
                     " \"c\": {\"Read\": 2, \"Edit\": 1, \"Bash\": 1, \"Grep\": 1}}]}", DEMO_PROMPT[i], "-");
            mt_load(demo);
            if (g_mt_n == 2) { /* this turn's reply as the demo has it; the second session still at work */
                free(g_mt[1].r);
                g_mt[1].r = i == 1 ? NULL : _strdup(DEMO_REPLY[i]);
                if (i == 1) { g_run_tool = _strdup("Bash"); g_run_what = _strdup("pytest -q tests/test_lock.py"); }
            }
        }
        return;
    }
    char path[1200];
    snprintf(path, sizeof path, "%s\\msg\\%s.json", g_dir, s->sid);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    int ok = g_dir[0] && strcmp(s->sid, "ghost") && GetFileAttributesExA(path, GetFileExInfoStandard, &fa);
    if (same && ok && !CompareFileTime(&fa.ftLastWriteTime, &g_msg_mtime)) return;
    free(g_msg_reply); free(g_msg_notice); free(g_msg_tmux); free(g_msg_inbox); free(g_msg_wt); g_msg_reply = g_msg_notice = g_msg_tmux = g_msg_inbox = g_msg_wt = NULL;
    mt_free();
    snprintf(g_msg_sid, sizeof g_msg_sid, "%s", s->sid);
    memset(&g_msg_mtime, 0, sizeof g_msg_mtime);
    if (!ok) return;
    g_msg_mtime = fa.ftLastWriteTime;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char *buf = malloc(256 * 1024);
    size_t len = fread(buf, 1, 256 * 1024 - 1, f);
    fclose(f);
    buf[len] = 0;
    mt_load(buf);
    g_msg_reply = json_str(buf, "reply");
    g_msg_notice = json_str(buf, "notice");
    g_msg_tmux = json_str(buf, "tmux");
    g_msg_inbox = json_str(buf, "inbox");
    g_msg_wt = json_str(buf, "wt");
    free(buf);
}

/* UTF-8 → UTF-16, with a little Markdown taken out (what replies are made of): **, `, ``` lines, # heads,
 * "- " and "* " items as bullets */
static wchar_t *chat_text(const char *u8) {
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

static wchar_t *g_chat_title, *g_chat_body;
static COLORREF g_chat_accent;
static HFONT g_chat_tf, g_chat_bf, g_chat_mf; /* title, body, monospaced */
static int g_chat_title_h, g_chat_body_h, g_chat_view_h, g_chat_line, g_chat_scroll;
static int g_chat_auto = 1;          /* the box follows the conversation: the newest turn from its prompt, or its end while it runs */
static int g_pin = -1, g_pin_hot;    /* the prompt pinned at the top while its turn is scrolled through; the pointer on it shows it whole */
static RECT g_pin_rc;
static RECT g_chat_view;             /* where the reply is drawn in the box (window pixels), for clicks on links */
static char g_chat_sid[72];          /* whose text the box shows */

static int chat_pad(void) { return (int)(11 * g_dpi); }
static int chat_bar(void) { return (int)(3 * g_dpi); }
static int chat_gap(void) { return (int)(6 * g_dpi); }

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

static void chat_fonts(void) {
    if (g_chat_tf) return;
    int h = -(int)(g_font * g_dpi + 0.5);
    const wchar_t *mono = mono_face(), *text = g_chat_mono ? mono : L"Segoe UI";
    DWORD pitch = g_chat_mono ? FIXED_PITCH : 0;
    g_chat_tf = CreateFontW(h, 0, 0, 0, g_chat_mono ? FW_BOLD : FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, pitch, text);
    g_chat_bf = CreateFontW(h, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, pitch, text);
    g_chat_mf = CreateFontW(g_chat_mono ? h : (int)(h * 0.92f), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, mono);
    void chat_more_fonts(int h, const wchar_t *text, DWORD pitch);
    chat_more_fonts(h, text, pitch);
}

static int input_open(void);
/* Typing a reply: the pointer is hidden over the panel's windows until the mouse moves. It usually rests on the core
 * just double-clicked, whose window gets new pictures many times a second; each made Windows show the pointer again
 * while "hide pointer while typing" hid it on every key: it blinked. */
static int g_hide_pointer;
static POINT g_hide_at;
static void input_close(void);

/* ---- the reply as Markdown, laid out like Claude Code's terminal: a line of the reply is a line here, a
 * blank line a half-line gap. Lists ("- ", "1. ", "- [ ] ") with their wrapped lines hanging under the text;
 * # and ## headings larger, ### bold; **bold**, *italic*, ***both***, ~~struck~~, `code`, [links](url) (the
 * text, underlined); > quotes with a bar down the side; --- rules; | tables | as aligned columns (one line a
 * row when too wide); ``` blocks indented and dimmed. Long words and Chinese text wrap anywhere. Worked out
 * once per reply (chat_layout), drawn from the list of runs (chat_proc). */
enum { F_BODY, F_BOLD, F_MONO, F_ITAL, F_BOLDITAL, F_H1, F_H2, F_COUNT };
enum { DECO_UNDER = 1, DECO_STRIKE = 2 };
enum { SH_RULE = -1, SH_BAR = -2, SH_VLINE = -3, SH_FILL = -4 }; /* a rule across, a quote's bar, a thin line down (table borders), a prompt's band */
typedef struct { int x, y, f, at, n, w, deco, url, url_n; COLORREF c; } Run; /* y: the baseline; a rule: x, y, w; a bar: x, y (top), n tall; url: where a link's address is in the text */
static Run *g_runs;
static int g_runs_n, g_runs_cap, g_lay_w;
static wchar_t *g_lay_text;
static int g_lay_len, g_lay_cap;
static HFONT g_chat_xf[F_COUNT];     /* italic, bold italic and the two heading sizes */
static int g_fasc[F_COUNT], g_fhgt[F_COUNT];

static HFONT lay_font(int f) { return f == F_BOLD ? g_chat_tf : f == F_MONO ? g_chat_mf : f == F_BODY ? g_chat_bf : g_chat_xf[f]; }

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
/* where each prompt is in the laid-out conversation: its band (y0..y1), its runs, its first line (for the pinned strip) */
typedef struct { int y0, y1, r0, r1; wchar_t line[200]; } PromptAt;
static PromptAt g_pr[8];
static int g_pr_n;
static void lay_shape(int kind, int x, int y, int len, COLORREF c) { lay_add((Run){.x = x, .y = y, .f = kind, .n = kind == SH_RULE ? 0 : len, .w = kind == SH_RULE ? len : 0, .url = -1, .c = c}, NULL); }

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
    (void)a0;
    for (; a < b && n < cap - 1; a++) out[n++] = *a; /* the marks stay: cell_put draws them as bold, italic, code */
    out[n] = 0;
    return n;
}

/* a table cell at (x, base): **bold**, *italic* or _italic_, `code` as in the rest of the reply, the marks not drawn.
 * Returns its width; draw 0 only measures. The header row is bold throughout. */
static int cell_put(HDC dc, int x, int base, const wchar_t *t, int head, int draw) {
    int bold = 0, ital = 0, code = 0, x0 = x, n = (int)wcslen(t);
    const wchar_t *run = t;
    for (int i = 0; i <= n; i++) {
        int mark = 0;
        if (i < n) {
            if (t[i] == L'`') mark = 1;
            else if (!code && t[i] == L'*' && i + 1 < n && t[i + 1] == L'*') mark = 2;
            else if (!code && (t[i] == L'*' || (t[i] == L'_' && (i == 0 || !iswalnum(t[i - 1]) || ital)))) mark = 3;
        }
        if (i == n || mark) { /* the text so far, in the current style */
            int len = (int)(t + i - run);
            if (len > 0) {
                int f = code ? F_MONO : (bold || head) && ital ? F_BOLDITAL : bold || head ? F_BOLD : ital ? F_ITAL : F_BODY;
                COLORREF c = code ? MD_CODE : bold || head ? MD_BOLD : MD_BODY;
                int w = text_w(dc, f, run, len);
                if (draw) lay_put(x, base, f, c, run, len, w, 0);
                x += w;
            }
            if (mark == 1) code = !code;
            else if (mark == 2) { bold = !bold; i++; }
            else if (mark == 3) ital = !ital;
            run = t + i + 1;
        }
    }
    return x - x0;
}

void chat_more_fonts(int h, const wchar_t *text, DWORD pitch) {
    g_chat_xf[F_ITAL] = CreateFontW(h, 0, 0, 0, FW_NORMAL, TRUE, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, pitch, text);
    g_chat_xf[F_BOLDITAL] = CreateFontW(h, 0, 0, 0, FW_BOLD, TRUE, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, pitch, text);
    g_chat_xf[F_H1] = CreateFontW((int)(h * 1.32f), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, pitch, text);
    g_chat_xf[F_H2] = CreateFontW((int)(h * 1.14f), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, pitch, text);
}

static int chat_layout(HDC dc, const wchar_t *src, int maxw) {
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
    int lead = g_chat_mono ? text_w(dc, F_BODY, L"  ", 2) : 0, marked = 0;
    g_pr_n = 0;
    for (const wchar_t *ln = src; ln && *ln;) {
        const wchar_t *nl = wcschr(ln, L'\n'), *end = nl ? nl : ln + wcslen(ln);
        while (end > ln && (end[-1] == L'\r' || end[-1] == L' ' || end[-1] == L'\t')) end--;
        if (*ln >= 1 && *ln <= 6) { /* the conversation's own lines (chat_conv): prompts, steps, rules between turns, the notice */
            int kind = *ln, pad = lh / 4;
            in_code = 0;
            if (kind == 5) { y += gap; lay_shape(SH_RULE, 0, y, maxw, MD_RULE); y += gap + 1; blank = 1; }
            else if (kind == 1) { /* the prompt: every line of it in one band, "> " before the first, like the terminal */
                if (!blank) y += gap;
                int fi = g_runs_n, top = y, first = 1, gw = text_w(dc, F_BODY, L"> ", 2);
                lay_shape(SH_FILL, 0, top, 0, MD_RULE); /* its height once the lines are laid out */
                PromptAt *pa = g_pr_n < 8 ? &g_pr[g_pr_n++] : NULL;
                if (pa) pa->line[0] = 0;
                y += pad;
                for (; ln && *ln == 1;) {
                    nl = wcschr(ln, L'\n'); end = nl ? nl : ln + wcslen(ln);
                    while (end > ln && (end[-1] == L'\r' || end[-1] == L' ')) end--;
                    if (first) {
                        lay_put(lead, y + asc, F_BODY, MD_MARK, L">", 1, text_w(dc, F_BODY, L">", 1), 0);
                        if (pa) { int n = (int)(end - ln - 1); if (n > 199) n = 199; wcsncpy(pa->line, ln + 1, (size_t)n); pa->line[n] = 0; }
                    }
                    int x = lead + gw;
                    y = lay_inline(dc, ln + 1, end, &x, lead + gw, y, lh, asc, maxw - pad, 0, MD_BODY, 0) + lh;
                    first = 0;
                    ln = nl ? nl + 1 : NULL;
                }
                y += pad;
                g_runs[fi].n = y - top;
                if (pa) { pa->y0 = top; pa->y1 = y; pa->r0 = fi + 1; pa->r1 = g_runs_n; }
                y += gap / 2;
                blank = 1; marked = 0;
                continue;
            } else if (kind == 2 || kind == 3) { /* a step: "● Bash(make -j)", the one running now in the accent colour */
                const wchar_t *tab = ln + 1, *te;
                while (tab < end && *tab != L'\t') tab++;
                if (tab >= end) tab = NULL;
                te = tab ? tab : end;
                int x = lead;
                COLORREF dot = kind == 3 ? RGB(255, 170, 90) : RGB(110, 200, 140);
                int dw = text_w(dc, F_BODY, L"\x25CF ", 2);
                lay_put(0, y + asc, F_BODY, dot, L"\x25CF", 1, dw, 0);
                x = dw;
                int tw = text_w(dc, F_BOLD, ln + 1, (int)(te - ln - 1));
                lay_put(x, y + asc, F_BOLD, MD_BOLD, ln + 1, (int)(te - ln - 1), tw, 0);
                x += tw;
                if (tab && tab + 1 < end) {
                    wchar_t what[160];
                    int n = (int)(end - tab - 1);
                    if (n > 150) n = 150;
                    what[0] = L'(';
                    memcpy(what + 1, tab + 1, sizeof(wchar_t) * (size_t)n);
                    what[n + 1] = L')';
                    n += 2;
                    int ew = text_w(dc, F_BODY, L"\x2026)", 2), room = maxw - x - (kind == 3 ? text_w(dc, F_BODY, L" \x2026", 2) : 0);
                    int w = text_w(dc, F_BODY, what, n);
                    if (w > room) { /* one line: cut, with an ellipsis before the bracket */
                        while (n > 2 && text_w(dc, F_BODY, what, n - 2) + ew > room) n--;
                        n -= 1; what[n++] = 0x2026; what[n++] = L')';
                        w = text_w(dc, F_BODY, what, n);
                    }
                    lay_put(x, y + asc, F_BODY, MD_QUOTE, what, n, w, 0);
                    x += w;
                }
                if (kind == 3) lay_put(x, y + asc, F_BODY, dot, L" \x2026", 2, text_w(dc, F_BODY, L" \x2026", 2), 0);
                y += lh; blank = 0; marked = 0;
            } else if (kind == 4) { /* the steps of a finished turn in one line, or how many came before those shown */
                int x = lead, n = (int)(end - ln - 1);
                lay_put(x, y + asc, F_BODY, MD_MARK, L"\x2514 ", 2, text_w(dc, F_BODY, L"\x2514 ", 2), 0);
                x += text_w(dc, F_BODY, L"\x2514 ", 2);
                while (n > 0 && x + text_w(dc, F_BODY, ln + 1, n) > maxw) n--;
                lay_put(x, y + asc, F_BODY, MD_MARK, ln + 1, n, text_w(dc, F_BODY, ln + 1, n), 0);
                y += lh; blank = 0; marked = 0;
            } else if (kind == 6) { /* the permission notice, waiting for you */
                if (!blank) y += gap / 2;
                int x = lead;
                y = lay_inline(dc, ln + 1, end, &x, lead, y, lh, asc, maxw, 0, RGB(255, 184, 119), 0) + lh;
                blank = 0;
            }
            ln = nl ? nl + 1 : NULL;
            continue;
        }
        const wchar_t *c = ln;
        int sp = 0;
        while (c < end && (*c == L' ' || *c == L'\t')) { sp += *c == L'\t' ? 4 : 1; c++; }
        if (end - c >= 3 && !wcsncmp(c, L"```", 3)) { in_code = !in_code; ln = nl ? nl + 1 : NULL; continue; }
        if (c == end && !in_code) { if (!blank) y += gap; blank = 1; ln = nl ? nl + 1 : NULL; continue; }
        int x0 = lead + sp / 2 * unit;
        if (g_chat_mono && !marked) { lay_put(0, y + asc, F_BODY, MD_BOLD, L"\x25CF", 1, text_w(dc, F_BODY, L"\x25CF", 1), 0); marked = 1; } /* ●, then a space, as Claude Code writes it */
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
            int cw[12] = {0}, gapc = g_chat_mono ? text_w(dc, F_BODY, L" \x2502 ", 3) : unit, total = 0;
            for (int i = 0; i < nk; i++) { int r = keep[i]; for (int k = 0; k < cells[r]; k++) { int w = cell_put(dc, 0, 0, cell[r][k], r == head, 0); if (w > cw[k]) cw[k] = w; } }
            for (int k = 0; k < ncol; k++) total += cw[k] + (k ? gapc : 0);
            int sw = text_w(dc, F_BODY, L" ", 1), edge = g_chat_mono ? text_w(dc, F_BODY, L"\x2502 ", 2) : 0;
            if (g_chat_mono && total + 2 * edge <= maxw - x0) { /* a terminal table: box-drawing borders, padded to the columns */
                const COLORREF BORDER = RGB(110, 116, 130);
                /* the borders as thin lines (box-drawing characters left gaps where the rows met), the column
                 * lines where the characters' bars were, so the columns line up as before; each row padded */
                int barw = text_w(dc, F_BODY, L"\x2502", 1), th = (int)fmax(1, g_dpi), pad = lh / 4, rowh = lh + 2 * pad;
                int xb[13], top = y + pad;
                xb[0] = x0 + barw / 2;
                for (int k = 0; k < ncol; k++) xb[k + 1] = xb[k] + barw + ((cw[k] + sw - 1) / sw + 2) * sw;
                int tw = xb[ncol] - xb[0] + th;
                lay_shape(SH_RULE, xb[0], top, tw, BORDER);
                for (int i = 0; i < nk; i++) {
                    int r = keep[i], rt = top + i * rowh;
                    for (int k = 0; k < ncol && k < cells[r]; k++) cell_put(dc, xb[k] + barw / 2 + sw, rt + pad + asc, cell[r][k], r == head, 1);
                    lay_shape(SH_RULE, xb[0], rt + rowh, tw, BORDER); /* under every row; the last one is the bottom */
                }
                for (int k = 0; k <= ncol; k++) lay_shape(SH_VLINE, xb[k], top, nk * rowh + th, BORDER);
                if (xb[ncol] + th > g_lay_w) g_lay_w = xb[ncol] + th;
                y = top + nk * rowh + pad + th;
                blank = 0;
                ln = nl ? nl + 1 : NULL;
                continue;
            }
            for (int i = 0; i < nk; i++) {
                int r = keep[i], x = x0;
                if (total <= maxw - x0) { /* aligned columns */
                    for (int k = 0; k < cells[r]; k++) {
                        cell_put(dc, x, y + asc, cell[r][k], r == head, 1);
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
                if (g_chat_mono) heading = 3; /* a terminal has one size: headings are bold */
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
                mark[0] = g_chat_mono ? *c : 0x2022; mark[1] = 0; c += 2;
                if (!g_chat_mono && end - c >= 3 && c[0] == L'[' && c[2] == L']' && (c[1] == L' ' || c[1] == L'x' || c[1] == L'X')) { /* - [ ] / - [x] */
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

static void chat_hide(void) {
    if (input_open()) return; /* a reply being typed keeps its box */
    if (g_chat && IsWindowVisible(g_chat)) ShowWindow(g_chat, SW_HIDE);
    g_chat_sid[0] = 0;
    void chat_forget_side(void);
    chat_forget_side(); /* shown again: the side is chosen afresh */
}

/* a new text size: the fonts are made again and the box measures itself again */
static void chat_refont(void) {
    if (g_chat_tf) { DeleteObject(g_chat_tf); DeleteObject(g_chat_bf); DeleteObject(g_chat_mf); g_chat_tf = g_chat_bf = g_chat_mf = NULL; }
    for (int i = 0; i < F_COUNT; i++) if (g_chat_xf[i]) { DeleteObject(g_chat_xf[i]); g_chat_xf[i] = NULL; }
    if (g_label_font) { DeleteObject(g_label_font); g_label_font = NULL; }
    free(g_chat_title); g_chat_title = NULL;
    input_close();
    void input_refont(void);
    input_refont();
    chat_hide();
}

/* the wheel over a core (or over the box while a reply is typed) scrolls a long reply, 3 lines a notch */
static void chat_scroll(float notches) {
    if (!g_chat || !IsWindowVisible(g_chat) || g_chat_body_h <= g_chat_view_h) return;
    int to = g_chat_scroll - (int)(notches * 3 * g_chat_line);
    if (to < 0) to = 0;
    if (to > g_chat_body_h - g_chat_view_h) to = g_chat_body_h - g_chat_view_h;
    if (to != g_chat_scroll) { g_chat_scroll = to; g_chat_auto = 0; InvalidateRect(g_chat, NULL, FALSE); }
}

/* a prompt's band: the box's colour a little lighter (darker on a light box); the pinned strip a little more */
static COLORREF chat_band(int pinned) {
    COLORREF b = chat_bg();
    int k = pinned ? 16 : 9, to = box_light() ? 0 : 255;
    return RGB(GetRValue(b) + (to - GetRValue(b)) * k / 100, GetGValue(b) + (to - GetGValue(b)) * k / 100, GetBValue(b) + (to - GetBValue(b)) * k / 100);
}
/* the laid-out runs i0..i1 in view, the body's top at `top` */
static void chat_runs(HDC dc, int i0, int i1, RECT view, int top) {
    for (int i = i0; i < i1; i++) {
        const Run *rn = &g_runs[i];
        int by = top + rn->y, x = view.left + rn->x, th = (int)fmax(1, g_dpi);
        if (by + (rn->f == SH_VLINE || rn->f == SH_FILL ? rn->n : 0) + 3 * g_chat_line < view.top || by - 3 * g_chat_line > view.bottom) continue;
        if (rn->f == SH_FILL) { RECT fr = {view.left, by, view.right, by + rn->n}; HBRUSH br = CreateSolidBrush(chat_band(0)); FillRect(dc, &fr, br); DeleteObject(br); continue; }
        if (rn->f == SH_RULE || rn->f == SH_BAR || rn->f == SH_VLINE) { /* a rule, a quote's bar, a table's line down */
            RECT sr = rn->f == SH_RULE ? (RECT){x, by, x + rn->w, by + th}
                    : rn->f == SH_VLINE ? (RECT){x, by, x + th, by + rn->n} : (RECT){x, by, x + (int)(3 * g_dpi), by + rn->n};
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
}

static LRESULT CALLBACK chat_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT || msg == WM_PRINTCLIENT) { /* drawn off-screen first, so scrolling does not flicker (WM_PRINTCLIENT: --record) */
        PAINTSTRUCT ps;
        HDC wdc = msg == WM_PAINT ? BeginPaint(w, &ps) : (HDC)wp;
        RECT r; GetClientRect(w, &r);
        HDC dc = CreateCompatibleDC(wdc);
        HBITMAP bm = CreateCompatibleBitmap(wdc, r.right, r.bottom), old = SelectObject(dc, bm);
        HBRUSH bg = CreateSolidBrush(chat_bg()), bar = CreateSolidBrush(g_chat_accent);
        FillRect(dc, &r, bg);
        RECT b = {0, 0, chat_bar(), r.bottom};
        FillRect(dc, &b, bar);
        DeleteObject(bg); DeleteObject(bar);
        SetBkMode(dc, TRANSPARENT);
        int pad = chat_pad();
        RECT t = {chat_bar() + pad, pad, r.right - pad, pad + g_chat_title_h};
        int ar = GetRValue(g_chat_accent), ag = GetGValue(g_chat_accent), ab = GetBValue(g_chat_accent);
        SetTextColor(dc, ink(RGB(ar + (255 - ar) * 3 / 10, ag + (255 - ag) * 3 / 10, ab + (255 - ab) * 3 / 10))); /* lifted toward white */
        SelectObject(dc, g_chat_tf);
        if (g_chat_title) DrawTextW(dc, g_chat_title, -1, &t, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        int top = t.bottom + chat_gap(), sb = g_chat_body_h > g_chat_view_h ? (int)(8 * g_dpi) : 0;
        RECT view = {t.left, top, r.right - pad - sb, top + g_chat_view_h};
        RECT body = {view.left, top - g_chat_scroll, view.right, top - g_chat_scroll + g_chat_body_h};
        g_chat_view = view;
        SetTextColor(dc, text_body());
        SelectObject(dc, g_chat_bf);
        IntersectClipRect(dc, view.left, view.top, view.right, view.bottom);
        SetTextAlign(dc, TA_BASELINE | TA_LEFT); /* every face on the same baseline */
        chat_runs(dc, 0, g_runs_n, view, body.top);
        /* the prompt of the turn being read stays at the top as one line once it has scrolled away (as in the
         * terminal); the pointer on it shows it whole, a click goes back to it */
        g_pin = -1;
        SetRectEmpty(&g_pin_rc);
        for (int i = 0; i < g_pr_n; i++) if (g_pr[i].y0 <= g_chat_scroll) g_pin = i; /* the turn at the top */
        if (g_pin >= 0 && g_pr[g_pin].y0 == g_chat_scroll) g_pin = -1;             /* its prompt in full view there */
        if (g_pin >= 0) {
            const PromptAt *pa = &g_pr[g_pin];
            int full = g_pin_hot && pa->y1 - pa->y0 < g_chat_view_h, sh = full ? pa->y1 - pa->y0 : g_chat_line + (int)(8 * g_dpi);
            RECT strip = {view.left, view.top, view.right, view.top + sh};
            HBRUSH fb = CreateSolidBrush(chat_band(1));
            FillRect(dc, &strip, fb);
            DeleteObject(fb);
            if (full) chat_runs(dc, pa->r0, pa->r1, view, view.top - pa->y0);
            else {
                SetTextAlign(dc, TA_TOP | TA_LEFT);
                RECT tr = {view.left + (g_chat_mono ? text_w(dc, F_BODY, L"  ", 2) : 0), view.top, view.right - (int)(6 * g_dpi), view.top + sh};
                wchar_t line[220];
                swprintf(line, 220, L"> %ls", pa->line);
                SelectObject(dc, g_chat_bf);
                SetTextColor(dc, text_body());
                DrawTextW(dc, line, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
            }
            RECT edge = {view.left, strip.bottom, view.right, strip.bottom + (int)fmax(1, g_dpi)};
            HBRUSH eb = CreateSolidBrush(ink(MD_RULE));
            FillRect(dc, &edge, eb);
            DeleteObject(eb);
            g_pin_rc = strip;
        }
        SetTextAlign(dc, TA_TOP | TA_LEFT);
        SelectClipRgn(dc, NULL);
        if (sb) { /* where in the reply the box is */
            int th = g_chat_view_h * g_chat_view_h / g_chat_body_h, ty = top + (g_chat_view_h - th) * g_chat_scroll / (g_chat_body_h - g_chat_view_h);
            RECT track = {r.right - pad - (int)(3 * g_dpi), top, r.right - pad, top + g_chat_view_h}, thumb = {track.left, ty, track.right, ty + th};
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
    if (msg == WM_MOUSEWHEEL) { chat_scroll(GET_WHEEL_DELTA_WPARAM(wp) / 120.0f); return 0; }
    if (msg == WM_MOUSEMOVE || msg == WM_MOUSELEAVE) { /* the pinned prompt shows whole while the pointer is on it */
        POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
        int hot = msg == WM_MOUSEMOVE && g_pin >= 0 && PtInRect(&g_pin_rc, pt);
        if (msg == WM_MOUSEMOVE) { TRACKMOUSEEVENT tme = {sizeof tme, TME_LEAVE, w, 0}; TrackMouseEvent(&tme); }
        if (hot != g_pin_hot) { g_pin_hot = hot; InvalidateRect(w, NULL, FALSE); }
        return 0;
    }
    if (msg == WM_LBUTTONUP && g_pin >= 0) { /* a click on it: back to that prompt */
        POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
        if (PtInRect(&g_pin_rc, pt)) {
            int mx = g_chat_body_h > g_chat_view_h ? g_chat_body_h - g_chat_view_h : 0, y = g_pr[g_pin].y0;
            g_chat_scroll = y < mx ? y : mx; g_chat_auto = 0; g_pin_hot = 0;
            InvalidateRect(w, NULL, FALSE);
            return 0;
        }
    }
    if (msg == WM_SETCURSOR && g_pin >= 0) { POINT pt; GetCursorPos(&pt); ScreenToClient(w, &pt); if (PtInRect(&g_pin_rc, pt)) { SetCursor(LoadCursor(NULL, IDC_HAND)); return TRUE; } }
    if (msg == WM_SETCURSOR || msg == WM_LBUTTONUP) { /* a link under the pointer: the hand, and a click opens it */
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(w, &pt);
        int found = -1;
        if (PtInRect(&g_chat_view, pt))
            for (int i = 0; i < g_runs_n && found < 0; i++) {
                const Run *rn = &g_runs[i];
                if (rn->f < 0 || rn->url < 0) continue;
                int bx = g_chat_view.left + rn->x, by = g_chat_view.top - g_chat_scroll + rn->y;
                if (pt.x >= bx && pt.x < bx + rn->w && pt.y >= by - g_fasc[rn->f] && pt.y < by + g_chat_line / 3) found = i;
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
static int g_in_rows = 3;           /* the reply field's lines now (in_fit) */

/* The box: next to `beside` (screen pixels: the cluster, or the core), level with the core at height cy,
 * left of it when it fits, else right; or in the middle of the screen (Settings > Chat box). It wraps,
 * and a reply longer than the box scrolls (the wheel over the core). */
/* Where the box (and the reply line under it: bh is both) goes beside `beside` (the cluster, or a core): left, right,
 * below or above it, each kept on the monitor. A side counts only if it covers no core, small ones included (also
 * one flung away); in the middle third of the screen below and above come first, else the side away from the nearer
 * edge. The side used last is kept while it stays free, so drifting small cores do not make the box jump; when no
 * side is free, the one covering the least. */
static int g_chat_side = -1;          /* 0 left, 1 right, 2 below, 3 above */
void chat_forget_side(void) { g_chat_side = -1; }
static void chat_place(RECT beside, int cy, int bw, int bh, RECT wa, int *ox, int *oy) {
    int gap = (int)(10 * g_dpi), cx = (beside.left + beside.right) / 2;
    int at[4][2] = {{beside.left - gap - bw, cy - bh / 2}, {beside.right + gap, cy - bh / 2},
                    {cx - bw / 2, beside.bottom + gap}, {cx - bw / 2, beside.top - gap - bh}};
    long cover[4];
    for (int k = 0; k < 4; k++) {
        int x = at[k][0], y = at[k][1];
        if (x + bw > wa.right) x = wa.right - bw;
        if (x < wa.left) x = wa.left;
        if (y + bh > wa.bottom) y = wa.bottom - bh;
        if (y < wa.top) y = wa.top;
        at[k][0] = x; at[k][1] = y;
        RECT box = {x, y, x + bw, y + bh}, o;
        cover[k] = 0;
        for (int i = 0; i < g_core_n && i < MAX_SESS; i++) {
            RECT c = g_core_rc[i];
            InflateRect(&c, gap / 2, gap / 2);
            if (!IsRectEmpty(&g_core_rc[i]) && IntersectRect(&o, &box, &c)) cover[k] += (long)(o.right - o.left) * (o.bottom - o.top);
        }
        if (k < 2 && (k == 0 ? beside.left - gap - bw < wa.left : beside.right + gap + bw > wa.right))
            cover[k] += (long)bw * bh / 4; /* pushed back over the cluster by the screen's edge: a poor side */
    }
    int ww = wa.right - wa.left, mid = cx > wa.left + ww / 3 && cx < wa.right - ww / 3;
    int order[4];
    if (mid) { order[0] = 2; order[1] = 3; order[2] = cx > wa.left + ww / 2 ? 0 : 1; order[3] = 1 - order[2]; }
    else { order[0] = cx > wa.left + ww / 2 ? 0 : 1; order[1] = 2; order[2] = 3; order[3] = 1 - order[0]; }
    int pick = -1;
    if (g_chat_side >= 0 && cover[g_chat_side] == 0) pick = g_chat_side; /* still free: stays */
    for (int j = 0; j < 4 && pick < 0; j++) if (cover[order[j]] == 0) pick = order[j];
    if (pick < 0) { pick = order[0]; for (int j = 1; j < 4; j++) if (cover[order[j]] < cover[pick]) pick = order[j]; }
    g_chat_side = pick;
    *ox = at[pick][0]; *oy = at[pick][1];
}

/* The conversation for the box, as chat_layout reads it: each turn's prompt (lines marked 1), its steps (2, the one
 * running now 3; a finished turn has them in one line, 4), its reply as Markdown, a rule between turns (5), and
 * the permission notice at the end while it waits (6). NULL when the hook keeps no turns (an older hook). */
static void conv_put(char **o, int mark, const char *t, int lines) {
    for (const char *c = t; c && *c;) {
        if (lines && mark) *(*o)++ = (char)mark;
        for (; *c && *c != '\n'; c++) if ((unsigned char)*c > 6) *(*o)++ = *c;
        *(*o)++ = '\n';
        if (*c) c++;
    }
}
static char *chat_conv(int st) {
    if (!g_mt_n) return NULL;
    size_t cap = 4096 + (g_msg_notice ? strlen(g_msg_notice) * 2 : 0);
    for (int i = 0; i < g_mt_n; i++) {
        MTurn *t = &g_mt[i];
        cap += (t->p ? strlen(t->p) * 2 : 0) + (t->r ? strlen(t->r) + 4 : 0) + (t->tally ? strlen(t->tally) : 0) + 400 * MT_STEPS;
    }
    char *out = malloc(cap), *o = out;
    for (int i = 0; i < g_mt_n; i++) {
        MTurn *t = &g_mt[i];
        int live = i == g_mt_n - 1 && !t->r;
        if (i) conv_put(&o, 5, " ", 1);
        if (t->p) conv_put(&o, 1, t->p, 1);
        if (live) { /* going on: the last steps one by one, the running one last */
            char line[300];
            if (t->n > t->ns) { snprintf(line, sizeof line, "%d earlier step%s", t->n - t->ns, t->n - t->ns == 1 ? "" : "s"); conv_put(&o, 4, line, 1); }
            for (int k = 0; k < t->ns; k++) {
                snprintf(line, sizeof line, "%s\t%s", t->tool[k], t->what[k] ? t->what[k] : "");
                conv_put(&o, k == t->ns - 1 && g_run_tool ? 3 : 2, line, 1);
            }
            if (!t->ns && (st == ST_THINK || st == ST_WORK)) conv_put(&o, 3, "Thinking", 1);
        } else if (t->tally) conv_put(&o, 4, t->tally, 1);
        if (t->r) conv_put(&o, 0, t->r, 0);
    }
    if (st == ST_WAIT && g_msg_notice) conv_put(&o, 6, g_msg_notice, 1);
    *o = 0;
    return out;
}

static void chat_show(const Session *s, RECT beside, int cy, double now) {
    msg_load(s, now);
    int st = session_shown(s, now);
    char title[200];
    const char *name = s->name[0] ? s->name : "claude";
    char line[80];
    state_line(s, now, line, sizeof line);
    if (!strcmp(s->sid, "ghost")) snprintf(title, sizeof title, "%s", line);
    else snprintf(title, sizeof title, "%s \xC2\xB7 %s", name, line);
    if (s->unread) strncat(title, " \xC2\xB7 new reply", sizeof title - strlen(title) - 1);
    if (g_pend && !strcmp(g_pend_sid, s->sid)) strncat(title, " \xC2\xB7 your reply waits for your answer in the terminal", sizeof title - strlen(title) - 1);
    if (g_single && g_focus_pinned && !g_rec_scene[0] && !strcmp(g_focus, s->sid)) strncat(title, " \xC2\xB7 pinned", sizeof title - strlen(title) - 1);
    const char *body = (st == ST_WAIT && g_msg_notice) ? g_msg_notice : g_msg_reply ? g_msg_reply : strcmp(s->sid, "ghost") ? "(no reply yet)" : "Start a session in WSL; it shows up here.";
    char *conv = strcmp(s->sid, "ghost") ? chat_conv(st) : NULL;
    if (conv) body = conv;
    wchar_t *wt = chat_text(title), *wb;
    { int n = MultiByteToWideChar(CP_UTF8, 0, body, -1, NULL, 0); wb = malloc(sizeof(wchar_t) * (size_t)n); MultiByteToWideChar(CP_UTF8, 0, body, -1, wb, n); } /* as written: laid out below */
    free(conv);
    /* a new turn (or another session) is followed again, wherever the box was scrolled to */
    static unsigned last_turn;
    unsigned turn_key = (unsigned)g_mt_n * 2654435761u;
    if (g_mt_n && g_mt[g_mt_n - 1].p) for (const char *c = g_mt[g_mt_n - 1].p; *c; c++) turn_key = turn_key * 31 + (unsigned char)*c;
    Col c = s->aura_a > 0.01f ? s->aura : s->cur.eh; /* single core: the session's own colour, else its state colour */
    COLORREF acc = RGB((int)(fminf(1, c.r) * 255), (int)(fminf(1, c.g) * 255), (int)(fminf(1, c.b) * 255));
    static RECT last_beside;
    static int last_middle = -1, last_input = -1, last_rows;
    int other = strcmp(g_chat_sid, s->sid) != 0, body_new = !g_chat_body || wcscmp(wb, g_chat_body);
    int moved = memcmp(&beside, &last_beside, sizeof beside) != 0 || last_middle != g_chat_middle || last_input != input_open() || last_rows != g_in_rows;
    last_input = input_open(); last_rows = g_in_rows;
    int changed = !g_chat_title || wcscmp(wt, g_chat_title) || body_new || !IsWindowVisible(g_chat) || moved || other;
    if (other || turn_key != last_turn) { g_chat_scroll = 0; g_chat_auto = 1; } /* another reply starts at its top */
    last_turn = turn_key;
    snprintf(g_chat_sid, sizeof g_chat_sid, "%s", s->sid);
    last_beside = beside;
    last_middle = g_chat_middle;
    free(g_chat_title); free(g_chat_body);
    g_chat_title = wt; g_chat_body = wb;
    if (acc != g_chat_accent) { g_chat_accent = acc; InvalidateRect(g_chat, NULL, FALSE); }
    if (!changed) return;
    chat_fonts();
    RECT wa = work_at((beside.left + beside.right) / 2.0f, (float)cy); /* the monitor the core is on */
    { int m = (int)(12 * g_dpi); InflateRect(&wa, -m, -m); } /* never against the screen's edge */
    int ww = wa.right - wa.left, wh = wa.bottom - wa.top;
    int maxw = (int)(fmax(460, g_font * 36) * g_dpi * g_chat_size), pad = chat_pad(); /* Settings > Box size */
    if (g_chat_middle) maxw = (int)fmin(fmax(maxw, ww * 0.45 * g_chat_size), 1000 * g_dpi * g_chat_size);
    if (maxw > ww - 2 * pad) maxw = ww - 2 * pad;
    HDC dc = GetDC(g_chat);
    SelectObject(dc, g_chat_tf);
    RECT t = {0, 0, maxw, 0};
    DrawTextW(dc, wt, -1, &t, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    g_chat_title_h = t.bottom;
    SelectObject(dc, g_chat_bf);
    TEXTMETRICW tm; GetTextMetricsW(dc, &tm);
    g_chat_line = tm.tmHeight;
    RECT b = {0, 0, maxw, 0};
    g_chat_body_h = chat_layout(dc, wb, maxw);
    b.right = g_lay_w;
    ReleaseDC(g_chat, dc);
    int most = g_chat_middle ? (int)(wh * fmin(0.85, 0.6 * g_chat_size)) : (int)fmin(g_chat_line * 18 * g_chat_size, wh * 0.85); /* longer: it scrolls */
    /* a reply being typed sits under the box: both have to fit on the screen together */
    int below = input_open() ? input_h() + (int)(6 * g_dpi) : 0, chrome = pad + g_chat_title_h + chat_gap() + pad;
    if (most > wh - below - chrome - (int)(16 * g_dpi)) most = wh - below - chrome - (int)(16 * g_dpi);
    if (most < g_chat_line * 3) most = g_chat_line * 3;
    g_chat_view_h = g_chat_body_h < most ? g_chat_body_h : most;
    if (g_chat_scroll > g_chat_body_h - g_chat_view_h) g_chat_scroll = g_chat_body_h > g_chat_view_h ? g_chat_body_h - g_chat_view_h : 0;
    if (g_chat_auto && g_pr_n) { /* following: the newest prompt at the top, or the end while the turn runs */
        int mx = g_chat_body_h > g_chat_view_h ? g_chat_body_h - g_chat_view_h : 0, live = g_mt_n && !g_mt[g_mt_n - 1].r;
        g_chat_scroll = live || g_pr[g_pr_n - 1].y0 > mx ? mx : g_pr[g_pr_n - 1].y0;
    }
    int sb = g_chat_body_h > g_chat_view_h ? (int)(8 * g_dpi) : 0;
    int w = (t.right > b.right ? t.right : b.right), h = pad + g_chat_title_h + chat_gap() + g_chat_view_h + pad;
    if (input_open()) w = maxw; /* a reply being typed gets the whole width (the reply line is as wide as the box): a short answer made it a narrow slot */
    if (w > maxw) w = maxw;
    w += chat_bar() + 2 * pad + sb;
    int x, y;
    if (g_chat_middle) { x = wa.left + (ww - w) / 2; y = wa.top + (wh - h - below) / 2; } /* the box and the reply line, centred together */
    else chat_place(beside, cy, w, h + below, wa, &x, &y);
    if (x + w > wa.right) x = wa.right - w;
    if (x < wa.left) x = wa.left;
    if (y + h + below > wa.bottom) y = wa.bottom - h - below;
    if (y < wa.top) y = wa.top;
    static RECT was;
    RECT now_rc = {x, y, x + w, y + h};
    if (!EqualRect(&was, &now_rc) || !IsWindowVisible(g_chat)) { /* only when it moved or changed size: re-shaping repaints it, and the reply line under it */
        SetWindowPos(g_chat, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if (w != was.right - was.left || h != was.bottom - was.top) {
            int rad = (int)(12 * g_dpi);
            SetWindowRgn(g_chat, CreateRoundRectRgn(0, 0, w + 1, h + 1, rad, rad), TRUE);
        }
        was = now_rc;
        if (input_open()) input_place();
        for (int i = 0; i < g_core_n && i < MAX_SESS; i++) { /* a core it covers stays in front of it */
            RECT o;
            if (g_pane[i].w && g_pane[i].shown && IntersectRect(&o, &now_rc, &g_core_rc[i]))
                SetWindowPos(g_pane[i].w, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }
    InvalidateRect(g_chat, NULL, FALSE);
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
static int g_restyle;               /* pasted or a token put in: the whole text is styled again once */
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
    LONG_PTR ex = GetWindowLongPtr(g_chat, GWL_EXSTYLE); /* the box lets clicks through again */
    SetWindowLongPtr(g_chat, GWL_EXSTYLE, ex | WS_EX_TRANSPARENT);
    g_input_sid[0] = 0;
}

/* the reply box: a title line, a rounded field of three lines that grows with the text up to IN_ROWS_MAX, a hint line */
#define IN_ROWS_MAX 8
static int in_pad(void) { return (int)(12 * g_dpi); }
static int in_line(void) { return (int)(g_font * g_dpi * 1.45f); }
static int in_title_h(void) { return in_line(); }
static int in_field_h(void) { return in_line() * g_in_rows + (int)(12 * g_dpi); }
static int in_hint_h(void) { return (int)(g_font * 0.86f * g_dpi * 1.45f); }

static int input_h(void) { int gap = (int)(6 * g_dpi); return in_pad() + in_title_h() + gap + in_field_h() + gap + in_hint_h() + in_pad(); }

/* under the chat box, as wide as it */
static void input_place(void) {
    RECT tr;
    GetWindowRect(g_chat, &tr);
    int gap = (int)(6 * g_dpi), h = input_h(), y = tr.bottom + gap; /* the box made room for it */
    RECT wa = work_at((tr.left + tr.right) / 2.0f, (tr.top + tr.bottom) / 2.0f); /* the chat box's monitor */
    if (y + h > wa.bottom) y = wa.bottom - h;
    RECT ir;
    GetWindowRect(g_input, &ir);
    int w = tr.right - tr.left;
    if (ir.left == tr.left && ir.top == y && ir.right - ir.left == w && ir.bottom - ir.top == h) return; /* already there: no repaint */
    SetWindowPos(g_input, HWND_TOPMOST, tr.left, y, w, h, SWP_NOACTIVATE);
    if (ir.right - ir.left != w || ir.bottom - ir.top != h) {
        int rad = (int)(12 * g_dpi);
        SetWindowRgn(g_input, CreateRoundRectRgn(0, 0, w + 1, h + 1, rad, rad), TRUE);
    }
}

/* the field as tall as its text (wrapped lines count), 3 to IN_ROWS_MAX lines; longer, it scrolls. The chat box
 * makes room for it on its next tick (chat_show sees g_in_rows change). */
static void in_fit(void) {
    if (!g_edit) return;
    int n = (int)SendMessageW(g_edit, EM_GETLINECOUNT, 0, 0);
    int rows = n < 3 ? 3 : n > IN_ROWS_MAX ? IN_ROWS_MAX : n;
    if (rows == g_in_rows) return;
    g_in_rows = rows;
    if (!input_open()) return;
    input_place();
    if (n <= rows) SendMessageW(g_edit, WM_VSCROLL, SB_TOP, 0); /* all of it fits: none scrolled out of sight */
    SendMessageW(g_edit, EM_SCROLLCARET, 0, 0);
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

/* a session started with vigil-claude: the reply goes into its launcher's pipe (inbox/<pid> next to the events file),
 * which pastes it in and presses Enter, the terminal staying where it is. 0 if there is none (or it has gone). */
static int send_inbox(const char *pid, const wchar_t *text) {
    char distro[64], dir[1024];
    if (!pid || !pid[0] || strspn(pid, "0123456789") != strlen(pid) || strlen(pid) > 9 || !wsl_distro(distro, sizeof distro)) return 0;
    { /* the events folder as WSL sees it: \\wsl.localhost\<distro>\home\... -> /home/... */
        const char *c = g_dir + (!strncmp(g_dir, "\\\\wsl.localhost\\", 16) ? 16 : 7) + strlen(distro);
        size_t i = 0;
        for (; *c && i + 1 < sizeof dir; c++) { if (*c == '"' || *c == '$' || *c == '`' || *c == '\'' || (unsigned char)*c < 32) return 0; /* (passed as an argument; nothing a shell could read) */ dir[i++] = *c == '\\' ? '/' : *c; }
        dir[i] = 0;
        if (dir[0] != '/') return 0;
    }
    wchar_t cmd[1400];
    /* the pipe only if it is there and a launcher is reading it (it removes it as it ends); never wait long on it */
    swprintf(cmd, 1400, L"wsl.exe -d %hs -e sh -c \"f=\\\"$0/inbox/$1\\\"; [ -p \\\"$f\\\" ] && kill -0 $1 2>/dev/null && timeout 3 sh -c 'cat > \\\"$0\\\"' \\\"$f\\\"\" \"%hs\" %hs",
             distro, dir, pid);
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
    DWORD code = 1;
    if (ok) {
        int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL); /* with its NUL: the launcher's end of a message */
        char *u8 = malloc((size_t)n);
        WideCharToMultiByte(CP_UTF8, 0, text, -1, u8, n, NULL, NULL);
        DWORD put;
        WriteFile(wr, u8, (DWORD)n, &put, NULL);
        free(u8);
        CloseHandle(wr); wr = NULL;
        if (WaitForSingleObject(pi.hProcess, 6000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code); /* (it gave up: the old way) */
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
    if (wr) CloseHandle(wr);
    return ok && code == 0;
}

/* ---- a reply typed into its Windows Terminal tab, the window left where it is. Windows Terminal gives each tab an id,
 * WT_SESSION, in the environment of the program it runs there (wsl.exe), and passes it on into WSL, where the hook
 * notes it. The panel finds that program by the id in its environment, attaches to its console and adds the reply to
 * the console's input as keys, as if typed there: wsl.exe hands them on to Claude Code. Several lines go in as one
 * bracketed paste (so a newline does not send it early); Enter follows a moment later (ID_WTENTER). */
typedef NTSTATUS (NTAPI *NtQIP)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);
static int wt_env_is(DWORD pid, const wchar_t *want) { /* does this process's environment have WT_SESSION=want? */
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!h) return 0;
    static NtQIP q;
    if (!q) q = (NtQIP)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");
    PROCESS_BASIC_INFORMATION pbi;
    ULONG rl;
    int hit = 0;
    PVOID params = NULL, env = NULL;
    SIZE_T got = 0, esz = 0;
    /* (x64: the PEB's ProcessParameters at 0x20; there the Environment at 0x80 and EnvironmentSize at 0x3F0) */
    if (q && q(h, ProcessBasicInformation, &pbi, sizeof pbi, &rl) == 0 && pbi.PebBaseAddress
        && ReadProcessMemory(h, (char *)pbi.PebBaseAddress + 0x20, &params, sizeof params, &got) && params
        && ReadProcessMemory(h, (char *)params + 0x80, &env, sizeof env, &got) && env) {
        if (!ReadProcessMemory(h, (char *)params + 0x3F0, &esz, sizeof esz, &got) || !esz || esz > (1 << 20)) esz = 65536;
        wchar_t *buf = calloc(esz / 2 + 2, sizeof(wchar_t));
        SIZE_T rd = 0;
        if (buf && (ReadProcessMemory(h, env, buf, esz, &rd) || rd)) {
            size_t wl = wcslen(want);
            for (wchar_t *c = buf; c < buf + rd / 2 && *c && !hit; c += wcslen(c) + 1)
                hit = !wcsncmp(c, L"WT_SESSION=", 11) && !wcsncmp(c + 11, want, wl) && !c[11 + wl];
        }
        free(buf);
    }
    CloseHandle(h);
    return hit;
}
static DWORD wt_find(const char *id) { /* the program in that tab: a child of Windows Terminal with the tab's id */
    wchar_t want[48];
    if (!id || strlen(id) != 36 || strspn(id, "0123456789abcdefABCDEF-") != 36) return 0;
    MultiByteToWideChar(CP_UTF8, 0, id, -1, want, 48);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    DWORD wtp[16], found = 0;
    int nw = 0;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof pe;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && nw < 16; ok = Process32NextW(snap, &pe))
        if (!_wcsicmp(pe.szExeFile, L"WindowsTerminal.exe")) wtp[nw++] = pe.th32ProcessID;
    pe.dwSize = sizeof pe;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe)) {
        int child = 0;
        for (int k = 0; k < nw; k++) child |= pe.th32ParentProcessID == wtp[k];
        if (child && pe.th32ProcessID != GetCurrentProcessId() && _wcsicmp(pe.szExeFile, L"OpenConsole.exe") && wt_env_is(pe.th32ProcessID, want)) found = pe.th32ProcessID;
    }
    CloseHandle(snap);
    return found;
}
static int wt_keys(DWORD pid, const wchar_t *s) { /* s into that program's console input, as keys; 0 if it cannot */
    size_t n = wcslen(s);
    if (!pid || !n) return 0;
    HANDLE e0 = GetStdHandle(STD_ERROR_HANDLE), o0 = GetStdHandle(STD_OUTPUT_HANDLE), i0 = GetStdHandle(STD_INPUT_HANDLE);
    FreeConsole(); /* (a windowed program: none of its own) */
    int ok = 0;
    if (AttachConsole(pid)) {
        HANDLE in = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (in != INVALID_HANDLE_VALUE) {
            INPUT_RECORD *r = calloc(n * 2, sizeof *r);
            DWORD put = 0;
            for (size_t i = 0; r && i < n; i++) for (int d = 0; d < 2; d++) {
                KEY_EVENT_RECORD *k = &r[i * 2 + d].Event.KeyEvent;
                r[i * 2 + d].EventType = KEY_EVENT;
                k->bKeyDown = !d; k->wRepeatCount = 1; k->uChar.UnicodeChar = s[i];
                k->wVirtualKeyCode = s[i] == L'\r' ? VK_RETURN : 0;
            }
            ok = r && WriteConsoleInputW(in, r, (DWORD)(n * 2), &put) && put == n * 2;
            free(r);
            CloseHandle(in);
        }
        FreeConsole();
    }
    SetStdHandle(STD_ERROR_HANDLE, e0); SetStdHandle(STD_OUTPUT_HANDLE, o0); SetStdHandle(STD_INPUT_HANDLE, i0);
    return ok;
}
static DWORD g_wt_pid; /* where Enter goes, a moment after the text */
static int send_wt(const char *id, const wchar_t *text) {
    DWORD pid = wt_find(id);
    if (!pid) return 0;
    size_t n = wcslen(text);
    int lines = wcschr(text, L'\n') != NULL;
    wchar_t *k = malloc(sizeof(wchar_t) * (n + 16));
    if (!k) return 0;
    if (lines) { /* one paste, its lines kept (Claude Code asks for bracketed paste) */
        wcscpy(k, L"\x1b[200~");
        wchar_t *o = k + wcslen(k);
        for (const wchar_t *c = text; *c; c++) *o++ = *c == L'\n' ? L'\r' : *c;
        wcscpy(o, L"\x1b[201~");
    } else wcscpy(k, text);
    int ok = wt_keys(pid, k);
    free(k);
    if (ok) { g_wt_pid = pid; SetTimer(g_cluster.w, ID_WTENTER, 200, NULL); }
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
    /* all of it, every time: reopening the box sets its font again (WM_SETFONT), which puts every run back to the
     * default colour, and text put in (a pasted image's token) comes in plain; this runs only on opening, after a paste
     * or token, and when Settings change, never on a key, so it costs no blink while typing */
    SendMessageW(g_edit, EM_SETCHARFORMAT, SCF_DEFAULT, (LPARAM)&cf);
    SendMessageW(g_edit, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
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
    g_restyle = 1;
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

/* the command line's keys in the reply line: Ctrl+C with nothing selected clears it (with a selection it copies, as
 * usual), Ctrl+W and Ctrl+Backspace delete the word before the cursor (back to the space before it; an image token
 * whole), Ctrl+U everything before the cursor on its line. 1 if the key was used. */
static int edit_cli_key(HWND w, int key) {
    DWORD a = 0, b = 0;
    SendMessageW(w, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
    if (key == 'C') {
        if (a != b) return 0; /* a selection: copied as usual */
        SetWindowTextW(w, L"");
        g_attach_n = g_attach_next = 0;
        InvalidateRect(g_input, NULL, FALSE);
        return 1;
    }
    if (a != b) { SendMessageW(w, EM_REPLACESEL, TRUE, (LPARAM)L""); InvalidateRect(g_input, NULL, FALSE); return 1; } /* a selection: it goes */
    wchar_t *t = edit_text();
    int st = (int)a;
    if (key == 'U') while (st > 0 && t[st - 1] != L'\r' && t[st - 1] != L'\n') st--;
    else {
        while (st > 0 && (t[st - 1] == L' ' || t[st - 1] == L'\t')) st--;
        while (st > 0 && t[st - 1] != L' ' && t[st - 1] != L'\t' && t[st - 1] != L'\r' && t[st - 1] != L'\n') st--;
        if (st == (int)a && st > 0) st--; /* at the start of a line: the line break before it */
        for (int k = st - 20 < 0 ? 0 : st - 20; k < st; k++) { /* inside an image token ("[Image #3]" has a space): the whole token */
            int id, used = 0;
            if (!wcsncmp(t + k, L"[Image #", 8) && swscanf(t + k + 8, L"%d]%n", &id, &used) == 1 && k + 8 + used >= st) { st = k; break; }
        }
    }
    free(t);
    if (st < (int)a) {
        SendMessageW(w, EM_SETSEL, st, a);
        SendMessageW(w, EM_REPLACESEL, TRUE, (LPARAM)L"");
        InvalidateRect(g_input, NULL, FALSE);
    }
    return 1;
}

static void bring_to_front(const Session *s);

/* a reply sent while its session is asking you something (NEEDS YOU: a permission prompt) waits: typed in then, it
 * would land in the prompt, and its Enter could answer it. It goes in once the session moves on (update_overlays);
 * opening that session's reply line again takes it back as the draft. */
static void deliver(Session *s, wchar_t *text) { /* (text: taken over) */
    msg_load(s, wall() + 1);
    if (send_tmux(g_msg_tmux, text)) { free(text); return; }
    if (send_wt(g_msg_wt, text)) { free(text); return; }       /* its Windows Terminal tab, the window left where it is */
    if (send_inbox(g_msg_inbox, text)) { free(text); return; } /* started with vigil-claude */
    /* none of those: its terminal to the front, then typed in (several lines become one, Enter would send early) */
    for (wchar_t *c = text; *c; c++) if (*c == L'\r' || *c == L'\n') *c = L' ';
    free(g_keys);
    g_keys = text;
    bring_to_front(s);
    SetTimer(g_cluster.w, ID_SENDKEYS, 250, NULL);
}

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
    if (s->state == ST_WAIT) { free(g_pend); g_pend = text; snprintf(g_pend_sid, sizeof g_pend_sid, "%s", s->sid); return; } /* it is asking you something: later */
    deliver(s, text);
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

/* The face for what is typed next, set before it arrives: Chinese (through the IME) in its own face, anything else
 * in the terminal's. Re-styling the whole text after each key moved the selection run by run, which hid and showed
 * the caret and repainted the text: a blink per key. */
static void edit_typing_face(HWND w, int cjk) {
    const wchar_t *cj = cjk_face();
    if (cjk && !cj) return;
    CHARFORMAT2W cf;
    memset(&cf, 0, sizeof cf);
    cf.cbSize = sizeof cf;
    cf.dwMask = CFM_FACE | CFM_CHARSET | CFM_COLOR; /* the colour too: the control's own default is black */
    cf.crTextColor = text_body();
    wcsncpy(cf.szFaceName, cjk ? cj : mono_face(), LF_FACESIZE - 1);
    cf.bCharSet = cjk ? CHINESEBIG5_CHARSET : DEFAULT_CHARSET;
    SendMessageW(w, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
}
static int paste_image(void);
static LRESULT CALLBACK edit_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) g_stat_paint_edit++;
    if (msg == WM_CHAR || msg == WM_IME_COMPOSITION) {
        if (!g_hide_pointer) { GetCursorPos(&g_hide_at); g_hide_pointer = 1; }
        SetCursor(NULL);
    }
    if (msg == WM_SETCURSOR && g_hide_pointer) { SetCursor(NULL); return TRUE; }
    if (msg == WM_IME_STARTCOMPOSITION) edit_typing_face(w, 1);
    if (msg == WM_CHAR && wp >= 32) edit_typing_face(w, cjk_char((wchar_t)wp));
    int ctrl_down = (GetKeyState(VK_CONTROL) & 0x8000) != 0, shift_down = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (msg == WM_KEYDOWN && wp == VK_RETURN && (g_send_ctrl ? ctrl_down : !shift_down)) { send_reply(); return 0; } /* Enter sends (Shift+Enter: a new line), or Ctrl+Enter (Settings > Shortcuts) */
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) { input_close(); return 0; }
    int paste = msg == WM_PASTE || (msg == WM_KEYDOWN && ((wp == 'V' && ctrl_down) || (wp == VK_INSERT && shift_down)));
    if (paste) { if (!paste_image()) { g_restyle = 1; SendMessageW(w, EM_PASTESPECIAL, CF_UNICODETEXT, 0); } g_restyle = 0; edit_style(); return 0; } /* an image becomes a token; text pastes as text; either styled like the rest at once */
    if (msg == WM_CHAR && wp == 22) return 0; /* (the character Ctrl+V would type) */
    if (msg == WM_KEYDOWN && ctrl_down && (wp == 'C' || wp == 'W' || wp == 'U' || wp == VK_BACK) && edit_cli_key(w, (int)wp)) return 0; /* like a command line */
    if (msg == WM_CHAR && ctrl_down && (wp == 3 || wp == 23 || wp == 21 || wp == 127)) return 0; /* (the characters those keys would type) */
    if (msg == WM_KEYDOWN && (wp == VK_BACK || wp == VK_DELETE) && g_attach_n && token_key(w, wp == VK_BACK)) return 0; /* a whole image token at once */
    if (msg == WM_CHAR && (wp == 27 || (wp == L'\r' && !g_send_ctrl && !shift_down) || (wp == L'\n' && g_send_ctrl))) return 0;
    if (msg == WM_MOUSEWHEEL) { chat_scroll(GET_WHEEL_DELTA_WPARAM(wp) / 120.0f); return 0; } /* the wheel scrolls the reply */
    return CallWindowProcW(g_edit_proc, w, msg, wp, lp);
}

static void add_drop(HDROP d, int finish);

static LRESULT CALLBACK input_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) g_stat_paint_card++;
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { /* like the chat box: dark card, the session's colour down the left edge, a rounded field */
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        RECT r; GetClientRect(w, &r);
        HBRUSH bg = CreateSolidBrush(chat_bg()), bar = CreateSolidBrush(g_chat_accent), field = CreateSolidBrush(chat_field());
        FillRect(dc, &r, bg);
        RECT b = {0, 0, chat_bar(), r.bottom};
        FillRect(dc, &b, bar);
        int pad = in_pad(), gap = (int)(6 * g_dpi), x0 = chat_bar() + pad, fy = pad + in_title_h() + gap;
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(58, 61, 72));
        HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, field);
        int rad = (int)(10 * g_dpi);
        RoundRect(dc, x0, fy, r.right - pad, fy + in_field_h(), rad, rad);
        SelectObject(dc, op); SelectObject(dc, ob);
        DeleteObject(pen); DeleteObject(bg); DeleteObject(bar); DeleteObject(field);
        SetBkMode(dc, TRANSPARENT);
        int ar = GetRValue(g_chat_accent), ag = GetGValue(g_chat_accent), ab = GetBValue(g_chat_accent);
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
        int pad = in_pad(), inset = (int)(9 * g_dpi), gap = (int)(6 * g_dpi), x0 = chat_bar() + pad, fy = pad + in_title_h() + gap;
        MoveWindow(g_edit, x0 + inset, fy + (int)(6 * g_dpi), LOWORD(lp) - x0 - pad - 2 * inset, in_field_h() - (int)(12 * g_dpi), TRUE);
        InvalidateRect(w, NULL, FALSE);
        in_fit(); /* another width wraps it into another number of lines */
        return 0;
    }
    case WM_DROPFILES: { int first = g_attach_n; add_drop((HDROP)wp, 1); insert_tokens(first); return 0; }
    case WM_COMMAND: /* after a paste or a token, every run in its face again; the hint (images still in the text) only when there are images */
        if (HIWORD(wp) == EN_CHANGE) {
            if (g_restyle) { g_restyle = 0; edit_style(); }
            if (g_attach_n) InvalidateRect(w, NULL, FALSE);
            in_fit();
        }
        return 0;
    case WM_ACTIVATE: if (LOWORD(wp) == WA_INACTIVE) input_close(); return 0; /* clicked elsewhere */
    case WM_SETFOCUS: SetFocus(g_edit); return 0;
    }
    return DefWindowProc(w, msg, wp, lp);
}

void chat_rescale(void) {
    if (g_chat_tf) { DeleteObject(g_chat_tf); DeleteObject(g_chat_bf); DeleteObject(g_chat_mf); g_chat_tf = g_chat_bf = g_chat_mf = NULL; }
    for (int i = 0; i < F_COUNT; i++) if (g_chat_xf[i]) { DeleteObject(g_chat_xf[i]); g_chat_xf[i] = NULL; }
    free(g_chat_title); g_chat_title = NULL;
    chat_hide();
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

/* one image (a Windows path) kept for the reply, with the next token number */
static int add_image(const wchar_t *path, const wchar_t *name) {
    if (g_attach_n >= MAX_ATTACH || !wsl_path(path, g_attach[g_attach_n], 600)) return 0;
    wcsncpy(g_attach_name[g_attach_n], name, 119);
    g_attach_name[g_attach_n][119] = 0;
    g_attach_id[g_attach_n] = ++g_attach_next;
    g_attach_n++;
    return 1;
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
        const wchar_t *base = wcsrchr(path, L'\\');
        add_image(path, base ? base + 1 : path);
    }
    if (finish) DragFinish(d);
    if (g_input) InvalidateRect(g_input, NULL, FALSE);
}
static int has_image(HDROP d);
/* a bitmap written as a PNG with Windows' own encoder (WIC) */
static int save_png(HBITMAP hb, const wchar_t *path) {
    IWICImagingFactory *f = NULL;
    IWICBitmap *bm = NULL;
    IWICStream *st = NULL;
    IWICBitmapEncoder *enc = NULL;
    IWICBitmapFrameEncode *fr = NULL;
    int ok = 0;
    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&f))) return 0;
    if (SUCCEEDED(IWICImagingFactory_CreateBitmapFromHBITMAP(f, hb, NULL, WICBitmapIgnoreAlpha, &bm)) &&
        SUCCEEDED(IWICImagingFactory_CreateStream(f, &st)) && SUCCEEDED(IWICStream_InitializeFromFilename(st, path, GENERIC_WRITE)) &&
        SUCCEEDED(IWICImagingFactory_CreateEncoder(f, &GUID_ContainerFormatPng, NULL, &enc)) &&
        SUCCEEDED(IWICBitmapEncoder_Initialize(enc, (IStream *)st, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(IWICBitmapEncoder_CreateNewFrame(enc, &fr, NULL)) && SUCCEEDED(IWICBitmapFrameEncode_Initialize(fr, NULL)) &&
        SUCCEEDED(IWICBitmapFrameEncode_WriteSource(fr, (IWICBitmapSource *)bm, NULL)) &&
        SUCCEEDED(IWICBitmapFrameEncode_Commit(fr)) && SUCCEEDED(IWICBitmapEncoder_Commit(enc))) ok = 1;
    if (fr) IWICBitmapFrameEncode_Release(fr);
    if (enc) IWICBitmapEncoder_Release(enc);
    if (st) IWICStream_Release(st);
    if (bm) IWICBitmap_Release(bm);
    IWICImagingFactory_Release(f);
    return ok;
}
/* Ctrl+V in the reply line with an image on the clipboard: image files copied in Explorer are attached as they are;
 * a copied picture (a screenshot, an image copied in a browser) is saved as a PNG in the Windows temp folder
 * (vigil-paste, cleared of pastes older than a day) and attached like a dropped one. 0: nothing of the kind there. */
static int paste_image(void) {
    if (!OpenClipboard(g_input)) return 0;
    int first = g_attach_n, done = 0;
    HDROP drop = IsClipboardFormatAvailable(CF_HDROP) ? (HDROP)GetClipboardData(CF_HDROP) : NULL;
    if (drop && has_image(drop)) { add_drop(drop, 0); done = 1; }
    else {
        UINT png = RegisterClipboardFormatW(L"PNG");
        HANDLE hp = IsClipboardFormatAvailable(png) ? GetClipboardData(png) : NULL;
        HBITMAP hb = !hp && IsClipboardFormatAvailable(CF_BITMAP) ? (HBITMAP)GetClipboardData(CF_BITMAP) : NULL;
        if (hp || hb) {
            wchar_t dir[MAX_PATH], path[MAX_PATH + 64];
            GetTempPathW(MAX_PATH, dir);
            wcscat(dir, L"vigil-paste");
            CreateDirectoryW(dir, NULL);
            wchar_t pat[MAX_PATH + 8]; /* yesterday's pastes go */
            swprintf(pat, MAX_PATH + 8, L"%ls\\*.png", dir);
            WIN32_FIND_DATAW fd;
            HANDLE fh = FindFirstFileW(pat, &fd);
            FILETIME now; GetSystemTimeAsFileTime(&now);
            ULONGLONG tn = ((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime;
            if (fh != INVALID_HANDLE_VALUE) {
                do {
                    ULONGLONG tf = ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
                    if (tn - tf > 864000000000ULL) { swprintf(path, MAX_PATH + 64, L"%ls\\%ls", dir, fd.cFileName); DeleteFileW(path); }
                } while (FindNextFileW(fh, &fd));
                FindClose(fh);
            }
            SYSTEMTIME t; GetLocalTime(&t);
            swprintf(path, MAX_PATH + 64, L"%ls\\paste-%04d%02d%02d-%02d%02d%02d-%03d.png", dir, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
            int saved = 0;
            if (hp) { /* a PNG copy already there (browsers, Snipping Tool): written as it is */
                void *b = GlobalLock(hp);
                SIZE_T n = GlobalSize(hp);
                HANDLE out = b ? CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL) : INVALID_HANDLE_VALUE;
                DWORD wrote = 0;
                if (out != INVALID_HANDLE_VALUE) { saved = WriteFile(out, b, (DWORD)n, &wrote, NULL) && wrote == n; CloseHandle(out); }
                if (b) GlobalUnlock(hp);
            } else saved = save_png(hb, path);
            if (saved && add_image(path, L"Pasted image")) done = 1;
        }
    }
    CloseClipboard();
    if (done) { insert_tokens(first); if (g_input) InvalidateRect(g_input, NULL, FALSE); }
    return done;
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
        g_input = CreateWindowEx(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_ACCEPTFILES, L"VigilInput", L"Vigil reply", WS_POPUP | WS_CLIPCHILDREN, 0, 0, 10, 10, NULL, NULL, ic.hInstance, NULL);
        LoadLibraryW(L"Msftedit.dll"); /* a rich edit: a font per run, so Chinese can have its own face, as in the terminal */
        g_edit = CreateWindowEx(0, MSFTEDIT_CLASS, L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN, 0, 0, 10, 10, g_input, NULL, ic.hInstance, NULL);
        SendMessageW(g_edit, EM_SETBKGNDCOLOR, 0, chat_field());
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
    if (g_pend && !strcmp(g_pend_sid, s->sid)) { free(g_draft); g_draft = g_pend; g_pend = NULL; snprintf(g_draft_sid, sizeof g_draft_sid, "%s", s->sid); g_input_sid[0] = 0; } /* a held reply: back to edit */
    if (strcmp(g_input_sid, s->sid)) {
        if (strcmp(g_draft_sid, s->sid)) g_attach_n = g_attach_next = 0; /* images belong to the reply they were dropped for */
        snprintf(g_input_sid, sizeof g_input_sid, "%s", s->sid);
        SetWindowTextW(g_edit, g_draft && !strcmp(g_draft_sid, s->sid) ? g_draft : L"");
        edit_style();
    }
    InvalidateRect(g_input, NULL, FALSE);
    LONG_PTR ex = GetWindowLongPtr(g_chat, GWL_EXSTYLE); /* the box takes the wheel while a reply is typed */
    SetWindowLongPtr(g_chat, GWL_EXSTYLE, ex & ~WS_EX_TRANSPARENT);
    ShowWindow(g_input, SW_SHOW);
    input_place();
    in_fit();
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
/* What the chat box goes beside in single-core mode: the cores actually there (the main one and the small ones in
 * orbit), not the whole orbit's square, which reached far above and below them. Measured when the box opens for a
 * session and kept while it stays open (small cores drift: a box that followed them would move on every beat),
 * measured again when the main core has moved. */
static RECT cluster_cores(int who) {
    static RECT kept;
    static char kept_sid[72];
    static float kept_mx, kept_my;
    Room *room = shown_room();
    const char *sid = who >= 0 && who < room->n ? room->s[who].sid : "";
    int open = g_chat && IsWindowVisible(g_chat);
    if (open && !strcmp(sid, kept_sid) && fabsf(g_mx - kept_mx) + fabsf(g_my - kept_my) < 4 * g_dpi) return kept;
    RECT u;
    SetRectEmpty(&u);
    for (int i = 0; i < g_core_n && i < room->n && i < MAX_SESS; i++) {
        RECT o;
        if (IsRectEmpty(&g_core_rc[i]) || !IntersectRect(&o, &g_core_rc[i], &g_cluster_rc)) continue; /* (one flung away is not part of it) */
        UnionRect(&u, &u, &g_core_rc[i]);
    }
    if (IsRectEmpty(&u)) u = g_cluster_rc;
    kept = u;
    snprintf(kept_sid, sizeof kept_sid, "%s", sid);
    kept_mx = g_mx; kept_my = g_my;
    return kept;
}

static int set_open(void);
static void track_mouse(double now) {
    POINT scr;
    cursor_pos(&scr);
    if (g_hide_pointer && (!input_open() || abs(scr.x - g_hide_at.x) + abs(scr.y - g_hide_at.y) > 2)) { /* the mouse moved, or the reply closed: back */
        g_hide_pointer = 0;
        POINT p; GetCursorPos(&p); SetCursorPos(p.x, p.y); /* (makes Windows ask the window under it for its pointer again) */
    }
    Room *room = shown_room();
    float lx = 0, ly = 0;
    int hit = core_at(scr, &lx, &ly), down = down_index();
    if (hit >= room->n) hit = -1;
    if (hit != g_hover) { g_hover = hit; g_hover_since = now; }
    int busy = g_moving || g_dragging || g_carrying;
    for (int i = 0; i < room->n; i++) {
        Session *s = &room->s[i];
        s->hover_to = i == hit || i == down;
        if (s->sub && s->sub_done > 0 && i == hit && now - g_hover_since > .5) { s->sub_seen = 1; s->unread = 0; } /* a finished subagent's reply, read: it goes once the pointer leaves */
        if (i == hit) { s->look_x_to = lx; s->look_y_to = ly; }
        s->pressing = i == down && !g_dragging && !g_moving; /* (held down, not dragged: the black hole pulls gas off the pointer) */
        if (i == down) { g_hover_since = now; if (i == hit && s->pressing) { s->press_x = lx; s->press_y = ly; } } /* (a press, held or not, is not resting on it; held, it follows the pointer) */
        s->lock_to = i == hit && !busy && now - g_hover_since > 2.0 ? 1.0f : 0.0f; /* lock-on after resting 2 s, untouched */
    }
    watch_for_rush(now, scr);
    { /* READY: one core at a time throws at the pointer (the main one first), while the pointer is on its monitor, off every core */
        int atk = -1;
        for (int i = 0; i < room->n && i < MAX_SESS; i++) {
            room->s[i].aim_on = 0;
            if (room->s[i].overlay == ST_READY && !IsRectEmpty(&g_core_rc[i]) && (atk < 0 || i == g_top)) atk = i;
        }
        if (atk >= 0) {
            Session *s = &room->s[atk];
            RECT r = g_core_rc[atk];
            float u = (r.right - r.left) * SHELL_FRAC; /* one shell radius at the core's usual size, in pixels */
            RECT cb; cursor_box(&cb); /* aimed at the middle of the mouse, and a hit anywhere on it counts */
            float mx = (cb.left + cb.right) * .5f, my = (cb.top + cb.bottom) * .5f;
            s->aim_x = (scr.x + mx - (r.left + r.right) * .5f) / u; s->aim_y = (scr.y + my - (r.top + r.bottom) * .5f) / u;
            s->aim_hit = (.5f * hypotf((float)(cb.right - cb.left), (float)(cb.bottom - cb.top)) + 7 * (float)g_dpi) / u;
            s->aim_on = !busy && hit < 0 && !input_open() && !set_open() && MonitorFromPoint(scr, MONITOR_DEFAULTTONEAREST) == MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST);
        }
    }
    int grabbed = busy || g_down_sid[0];
    for (int i = 0; i < MAX_SESS; i++) set_transparent(&g_pane[i], !(i == hit || (grabbed && i == down))); /* every core has its own window */
    if (input_open()) { /* typing a reply: the box stays on that session */
        int k = index_of(g_input_sid);
        if (k < 0) { input_close(); return; }
        RECT r = g_core_rc[k], beside = r;
        if (g_single) { /* beside the whole cluster, level with its middle: a small core drifts, and a box that */
            beside = cluster_cores(k); /* followed it was moved, and the line typed in repainted, on every beat */
            r = beside;
        }
        chat_show(&room->s[k], beside, (r.top + r.bottom) / 2, now);
        return;
    }
    if (busy) { g_hover_since = now; chat_hide(); return; }           /* no chat box while dragging */
    if (hit < 0 || now - g_hover_since < 0.3) { chat_hide(); return; } /* a short pause before the chat box */
    room->s[hit].unread = 0; /* its box is open, so the reply is seen: READY ends (once its pieces are home) */
    RECT r = g_core_rc[hit], beside = r, o;
    if (g_single && (hit == g_top || IntersectRect(&o, &r, &g_cluster_rc))) { beside = cluster_cores(hit); r = beside; } /* (one flung away: beside itself) */
    chat_show(&room->s[hit], beside, (r.top + r.bottom) / 2, now);
}

/* Alt held down: with nothing else typed, letting go of Alt would open the menu bar of the window with the
 * keyboard focus. A key press nobody uses (vk 0xE8) in between stops that. */
static int alt_held(void) { return (GetAsyncKeyState(g_move_mod) & 0x8000) != 0; } /* the move key (Settings > Shortcuts; Alt by default) */
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
/* Backdrop > Auto: how light the screen is in r, 0..1, from 8×8 pixels picked across it. A plain BitBlt of the screen
 * leaves out layered windows (no CAPTUREBLT), so the cores and their pieces never count, only what is under them. A
 * read of the screen can take a frame or two, so it happens on its own thread (bd_thread), never in the tick. */
static float screen_light(RECT r) {
    if (r.right - r.left < 2 || r.bottom - r.top < 2) return 0;
    HDC scr = GetDC(NULL), mem = CreateCompatibleDC(scr);
    BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), 8, -8, 1, 32, BI_RGB, 0, 0, 0, 0, 0}, {{0, 0, 0, 0}}};
    void *bits = NULL;
    HBITMAP bm = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    float sum = 0;
    if (bm && bits) {
        HGDIOBJ old = SelectObject(mem, bm);
        SetStretchBltMode(mem, COLORONCOLOR); /* (picks pixels: no averaging over the whole area) */
        StretchBlt(mem, 0, 0, 8, 8, scr, r.left, r.top, r.right - r.left, r.bottom - r.top, SRCCOPY);
        GdiFlush();
        const uint8_t *p = bits;
        for (int i = 0; i < 64; i++, p += 4) sum += (0.0722f * p[0] + 0.7152f * p[1] + 0.2126f * p[2]) / 255.0f;
        SelectObject(mem, old); DeleteObject(bm);
    }
    DeleteDC(mem); ReleaseDC(NULL, scr);
    return sum / 64;
}
static float g_bd_light[MAX_SESS]; /* the last reading under each core (and its thrown pieces) */
/* what the reader is asked to read (each core's rectangle, and its pieces' layer, else empty) and its readings, under g_bd_cs */
static CRITICAL_SECTION g_bd_cs;
static RECT g_bd_req[MAX_SESS][2];
static int g_bd_nreq, g_bd_run;
static float g_bd_got[MAX_SESS];
static DWORD WINAPI bd_thread(LPVOID arg) {
    (void)arg;
    for (;;) {
        Sleep(500);
        RECT req[MAX_SESS][2]; int n;
        EnterCriticalSection(&g_bd_cs); n = g_bd_nreq; memcpy(req, g_bd_req, sizeof req); LeaveCriticalSection(&g_bd_cs);
        float got[MAX_SESS];
        for (int i = 0; i < n; i++) got[i] = fmaxf(IsRectEmpty(&req[i][0]) ? 0 : screen_light(req[i][0]), IsRectEmpty(&req[i][1]) ? 0 : screen_light(req[i][1]));
        EnterCriticalSection(&g_bd_cs); memcpy(g_bd_got, got, sizeof(float) * (size_t)n); LeaveCriticalSection(&g_bd_cs);
    }
    return 0;
}
/* each core's backdrop, eased toward Off / On / what Auto reads under it */
static void update_backdrop(double now, double dt) {
    Room *room = shown_room();
    unsigned sc = shadex();
    SHADE_COL = (Col){(sc >> 16 & 255) / 255.0f, (sc >> 8 & 255) / 255.0f, (sc & 255) / 255.0f};
    (void)now;
    if (g_backdrop == 2) { /* Auto: hand the reader the rectangles to read, take its last readings */
        if (!g_bd_run) { g_bd_run = 1; InitializeCriticalSection(&g_bd_cs); HANDLE h = CreateThread(NULL, 0, bd_thread, NULL, 0, NULL); if (h) CloseHandle(h); }
        EnterCriticalSection(&g_bd_cs);
        g_bd_nreq = room->n < MAX_SESS ? room->n : MAX_SESS;
        for (int i = 0; i < g_bd_nreq; i++) {
            g_bd_req[i][0] = g_core_rc[i]; SetRectEmpty(&g_bd_req[i][1]);
            if (room->s[i].mo.loose_on && g_layer.shown) SetRect(&g_bd_req[i][1], g_layer.x, g_layer.y, g_layer.x + g_layer.W, g_layer.y + g_layer.H);
            g_bd_light[i] = g_bd_got[i];
        }
        LeaveCriticalSection(&g_bd_cs);
    }
    for (int i = 0; i < room->n && i < MAX_SESS; i++) {
        Session *s = &room->s[i];
        float to = g_backdrop == 1 ? 1 : g_backdrop == 2 ? fminf(1, fmaxf(0, (g_bd_light[i] - 0.4f) / 0.25f)) : 0;
        s->backdrop += (to - s->backdrop) * (float)fmin(1, dt * 4);
        if (fabsf(to - s->backdrop) < 0.01f) s->backdrop = to;
    }
}

static void update_overlays(double now) {
    if (g_pend) { /* a reply held while its session asked you something: in it goes once it has moved on */
        int k = index_of(g_pend_sid);
        Session *ps = k >= 0 ? &shown_room()->s[k] : NULL;
        if (!ps) { free(g_draft); g_draft = g_pend; snprintf(g_draft_sid, sizeof g_draft_sid, "%s", g_pend_sid); g_pend = NULL; } /* (gone: kept as the draft) */
        else if (ps->state != ST_WAIT) { wchar_t *t = g_pend; g_pend = NULL; deliver(ps, t); }
    }
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
        int hole = s->mo.rt == RT_HOLE && s->mo.hole > 0.5f && session_shown(s, now) == ST_SLEEP; /* (asked with no overlay: what it is under it) */
        s->overlay = away && !hole && session_shown(s, now) != ST_WAIT ? ST_STANDBY : 0; /* (asleep as a black hole, it goes quiet instead) */
        s->quiet_to = away && hole ? 1.0f : 0.0f;
        if ((g_drag_sid[0] && !strcmp(s->sid, g_drag_sid)) || (now < g_drop_until && !strcmp(s->sid, g_drop_sid))) s->overlay = ST_TRANSCRIBE; /* an image on it */
        s->mo.hit_at = 0; /* READY: a piece hit the pointer; it keeps on until you point at the core */
        if (!s->overlay) { /* a reply nobody has seen: READY, until the pieces it threw are home */
            int base = session_shown(s, now);
            if ((s->unread && !s->sub && (base == ST_IDLE || base == ST_SLEEP)) || s->mo.atk_busy) s->overlay = ST_READY; /* (a subagent's reply just glows) */
        }
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
        if (s->sub) continue; /* (a subagent stays a satellite of its session) */
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
    chat_hide();
}

static double g_polled;

static void write_stats(double now) {
    double span = now - g_stat_since;
    if (span < 5) return;
    char path[1100];
    snprintf(path, sizeof path, "%s\\panel-stats.txt", g_dir[0] ? g_dir : ".");
    FILE *f = fopen(path, "a");
    if (f) {
        double n = g_stat_tick > 1 ? g_stat_tick - 1 : 1, mean = g_stat_gap / n, sd = sqrt(fmax(0, g_stat_gap2 / n - mean * mean));
        fprintf(f, "%.0f  ticks %.1f/s  pictures %.1f/s  window updates %.1f/s  tick %.2f ms avg  gap %.1f ms (sd %.1f, max %.1f)  late %ld  monitor %.0f Hz, scale %.2f  smooth %s  pictures %.0f ms/s  windows %.0f ms/s  reply paints %.1f/s field, %.1f/s card\n",
                now, g_stat_tick / span, g_stat_pic / span, g_stat_push / span, g_stat_tick ? g_stat_ms / g_stat_tick : 0,
                mean, sd, g_stat_gap_max, g_stat_late, g_hz, g_dpi, SMOOTH_NAME[g_smooth], g_stat_render_ms / span, g_stat_push_ms / span, g_stat_paint_edit / span, g_stat_paint_card / span);
        fprintf(f, "    black hole: %s, %.1f GPU pictures/s, %.2f ms each%s%s\n", g_gpu && gpu_init() ? "GPU" : "CPU", g_stat_gpu_n / span, g_stat_gpu_n ? g_stat_gpu_ms / g_stat_gpu_n : 0, g_gpu ? "" : "  (", g_gpu ? "" : "--no-gpu or none)");
        g_stat_gpu_ms = 0; g_stat_gpu_n = 0;
        if (g_sa_n) {
            fprintf(f, "    pieces out: ticks %ld  late %ld  worst gap %.1f ms  worst tick %.1f ms  slowest events read %.1f ms  late at:", g_sa_n, g_sa_late, g_sa_gap_max, g_sa_work_max, g_sa_poll_max);
            for (long i = 0; i < g_sa_nl; i++) fprintf(f, " %.2fs/%.0fms", g_sa_lt[i], g_sa_lg[i]);
            fputc('\n', f);
        }
        fclose(f);
    }
    g_sa_n = g_sa_late = g_sa_nl = 0; g_sa_gap_max = g_sa_work_max = g_sa_poll_max = 0;
    g_stat_tick = g_stat_pic = g_stat_push = g_stat_late = g_stat_paint_edit = g_stat_paint_card = 0;
    g_stat_ms = g_stat_gap = g_stat_gap2 = g_stat_gap_max = g_stat_render_ms = g_stat_push_ms = 0;
    g_stat_since = now;
}

static void settings_tick(double now);
static int set_open(void);

static double tick_interval(void);
static void tick(void) {
    g_tick_n++;
    LARGE_INTEGER q0, q1, qf;
    QueryPerformanceCounter(&q0);
    double now = wall();
    float dt = (float)(now - g_last);
    if (dt > 0.25f) dt = 0.25f;
    g_last = now;
    if (g_demo_n && !g_rec_still) demo_run(&g_demo, &g_room, now);
    else if (now - g_polled >= 0.25) { /* the events file sits in WSL: each look is a trip into its VM, 4 a second is plenty */
        g_polled = now;
        LARGE_INTEGER p0, p1; QueryPerformanceCounter(&p0);
        int n = tail_poll(&g_tail, g_ev, 1024);
        for (int i = 0; i < n; i++) room_apply(&g_room, &g_ev[i]);
        if (g_stats) { QueryPerformanceCounter(&p1); QueryPerformanceFrequency(&qf); double ms = (p1.QuadPart - p0.QuadPart) * 1000.0 / qf.QuadPart; if (ms > g_sa_poll_max) g_sa_poll_max = ms; }
    }
    if (g_moving) follow();
    update_overlays(now);
    update_backdrop(now, dt);
    update_focus(now);
    track_mouse(now);
    room_tick(&g_room, &g_shape, now, dt);
    g_ghost.s[0].last = now;
    room_tick(&g_ghost, &g_shape, now, dt);
    draw(now);
    layer_draw(now);
    settings_tick(now);
    if (g_stats) {
        QueryPerformanceCounter(&q1); QueryPerformanceFrequency(&qf);
        double t0 = q0.QuadPart * 1000.0 / qf.QuadPart, want = tick_interval() * 1000;
        if (g_stat_last > 0 && g_stat_tick > 0) {
            double gap = t0 - g_stat_last;
            g_stat_gap += gap; g_stat_gap2 += gap * gap;
            if (gap > g_stat_gap_max) g_stat_gap_max = gap;
            if (gap > want * 1.5) g_stat_late++;
            if (g_layer.shown) {
                double work = (q1.QuadPart - q0.QuadPart) * 1000.0 / qf.QuadPart;
                g_sa_n++;
                if (gap > g_sa_gap_max) g_sa_gap_max = gap;
                if (work > g_sa_work_max) g_sa_work_max = work;
                if (gap > 1000.0 / g_hz * 1.5) { g_sa_late++; if (g_sa_nl < 24) { g_sa_lt[g_sa_nl] = now - g_stat_since; g_sa_lg[g_sa_nl++] = gap; } }
            }
        }
        g_stat_last = t0;
        g_stat_tick++;
        g_stat_ms += (q1.QuadPart - q0.QuadPart) * 1000.0 / qf.QuadPart;
        write_stats(now);
    }
}

/* how long until the next tick: 60 a second while the mouse is on a core or something is in the hand, else 30 (pictures keep their
 * own, lower rates; a tick that has nothing due costs next to nothing) */
static double tick_interval(void) {
    double hand = smooth_now()->hand, mv = hand <= 0 || hand > g_hz ? g_hz : hand < 30 ? 30 : hand; /* moving windows: smooth, aligned to the display */
    if (g_layer.shown) return 1.0 / g_hz; /* READY's pieces are out: every refresh */
    if (g_moving || g_dragging || g_carrying || g_down_sid[0] || g_hover >= 0 || set_open() || g_orbiting) return 1.0 / mv;
    double need = g_need_hz < 10 ? 10 : g_need_hz;
    return 1.0 / (need > g_hz ? g_hz : need);
}

/* ---- the Settings window (web/settings-mock.html is its design): a sidebar of pages that folds to icons, a big
 * live preview across the top (the cluster with its gravity: drag the core; the wheel zooms; the state it shows), and
 * below it the page: section cards of rows, each a name (and hint) on the left and its control on the right. The
 * window resizes from its edges and corner; its size and the sidebar's fold are kept. Drawn antialiased into its own
 * bitmap and shown as a layered window, so the corners are smooth too. Every choice applies at once and is kept. */
#include "lucide.h"
enum { R_SHAPE, R_CORE, R_GLOW, R_FRAME, R_SCALE, R_GRAVITY, R_OTHERS, R_CHATAT, R_CHATFONT, R_TEXT, R_LABELS, R_AWAY, R_CHATSIZE, R_CHATBG, R_WHEEL, R_TEXTCOL,
       R_MODE, R_SMOOTH, R_MOVEMOD, R_SEND, R_BACKDROP, R_SHADECOL, R_RIDLE, R_RTHINK, R_RBUSY, R_RREST, R_RHAND, R_THREADS, R_BDW, R_BDA, R_BDCORE, R_HOLETILT, R_HOLEGPU, R_HOLELENS };
/* Settings > Performance: pictures a second for each kind of moment (0: the monitor's refresh rate), the steps a
 * slider stops at; and how many threads draw */
#define NRATES 12
static const float RATES[NRATES] = {2, 4, 6, 10, 15, 20, 30, 45, 60, 90, 120, 0}; /* (90, 120: below Max on a 144 or 240 Hz monitor) */
static const wchar_t *RATE_NAMES[NRATES] = {L"2", L"4", L"6", L"10", L"15", L"20", L"30", L"45", L"60", L"90", L"120", L"Max"};
static const int THREADS[5] = {0, 1, 2, 4, 8};
static const wchar_t *THREAD_NAMES[5] = {L"Auto", L"1", L"2", L"4", L"8"};
static const wchar_t *HOLEGPU_NAMES[2] = {L"GPU", L"CPU"};
static float *rate_of(Smooth *m, int row) { return row == R_RIDLE ? &m->idle : row == R_RTHINK ? &m->think : row == R_RBUSY ? &m->busy : row == R_RREST ? &m->rest : &m->hand; }
static int rate_stop(float v) { /* the slider's step nearest a rate */
    if (v <= 0) return NRATES - 1;
    int best = 0;
    for (int i = 1; i < NRATES - 1; i++) if (fabsf(RATES[i] - v) < fabsf(RATES[best] - v)) best = i;
    return best;
}
enum { K_PICK, K_SLIDE, K_TOGGLE, K_STATE, K_RESET, K_CLOSE, K_PAGE, K_MSTATE, K_MTOG, K_SIDE, K_ZOOM, K_MTRY, K_MRESET, K_KREC, K_KCLR, K_SIZE, K_PVBG, K_HPLAY, K_HTOG };
enum { PG_LOOK, PG_CLUSTER, PG_REPLIES, PG_MOTIONS, PG_KEYS, PG_PERF, PG_N };
static const wchar_t *PG_NAME[PG_N] = {L"Look", L"Cluster", L"Replies", L"Motions", L"Shortcuts", L"Performance"};
static const wchar_t *PG_HINT[PG_N] = {L"how a core looks", L"the cores and their orbit", L"the box with the last reply", L"what each state may play", L"click a key to change it", L"smoother costs more CPU"};
static const char *PG_ICON[PG_N] = {"palette", "orbit", "message", "activity", "keyboard", "gauge"};
typedef struct { RECT r; int kind, row, val, n; float t0, t1; } SetHit; /* a slider: n stops from t0 to t1 (pixels) */
static const int PV_STATES[6] = {ST_IDLE, ST_THINK, ST_WORK, ST_WAIT, ST_CHEER, ST_SLEEP};
static const wchar_t *PV_NAMES[6] = {L"Idle", L"Thinking", L"Working", L"Needs you", L"Done", L"Asleep"};

/* the palette (0xRRGGBB) */
static const unsigned PV_LIGHT = 0xF2F2F4; /* the preview's light background */
static const unsigned S_BG = 0x18181B, S_BAND = 0x0F0F12, S_CARD = 0x212126, S_TILE = 0x19191D, S_EDGE = 0x34343B,
                      S_EDGE_HOT = 0x55555E, S_TEXT = 0xECECEF, S_LABEL = 0xA1A1AA, S_MUTED = 0x8B8B94, S_CHIP = 0x27272D,
                      S_CHIP_HOT = 0x323239, S_CHIP_TXT = 0xB4B4BC, S_OR = 0xFF8A1F, S_ON_OR = 0x1F1206, S_KNOB = 0xFFF1E3, S_CLOSE_HOT = 0x5A2A22,
                      S_SIDE = 0x131316, S_RULE = 0x2C2C33;
static Col hexc(unsigned v) { Col c = {(v >> 16 & 255) / 255.0f, (v >> 8 & 255) / 255.0f, (v & 255) / 255.0f}; return c; }
static COLORREF hexr(unsigned v) { return RGB(v >> 16 & 255, v >> 8 & 255, v & 255); }

static Pane g_sl, g_so;             /* the window's still layer (redrawn when something changes) and what is shown */
static int g_sl_dirty = 1;
static HFONT g_sf_title, g_sf_card, g_sf_text, g_sf_label, g_sf_small, g_sf_aa, g_sf_aam, g_sf_cap;
static SetHit g_hits[400];
static int g_hits_n, g_hot = -1, g_slide = -1, g_slide_row = -1, g_tracking, g_close_armed; /* armed: pressed on the close button */
/* g_slide: the slider being dragged, as its place in g_hits; a choice can rebuild the page and move it (a preset turned
 * Custom shows one more chip above), so it is found again by its row (g_slide_row) before each use */
static int slide_hit(void) { for (int i = 0; i < g_hits_n; i++) if (g_hits[i].kind == K_SLIDE && g_hits[i].row == g_slide_row) return i; return -1; }
typedef struct { RECT r; wchar_t t[160]; HFONT f; COLORREF c; UINT how; int clip; } TextOp; /* clip: cut at the page's edge */
static TextOp g_tx[400];
static int g_tx_n;
static RECT g_pv_rc;                /* the preview's cluster, in the window */
static RECT g_ovr[16];              /* controls drawn over the preview: laid back over its picture each frame */
static Pane g_om;                    /* their outlines' coverage (alpha only): their soft edges blend into the picture, light or dark */
static int g_ovr_n;
static RECT g_clip;                 /* while the page is drawn: text and hits outside it are left out (it scrolls) */
static int g_clipping;
static Pane g_pv;                   /* its picture (a DIB, no window of its own) */
static Room g_pv_room;
static Pic g_pv_pic[4];
static Orbit g_pv_ob[4];
static int g_pv_st = ST_THINK, g_pv_drag; /* the state the preview's main core shows */
static int g_tab;                   /* the page open (PG_*) */
static double g_mot_msg_until;      /* "a state needs a routine" shows until then */
static float g_pv_zoom = 1;         /* the wheel over the preview: 1 to 3 times, to see the detail */
static float g_pv_mx, g_pv_my, g_pv_ox, g_pv_oy, g_pv_vx, g_pv_vy, g_pv_big = 100;
static POINT g_pv_down, g_pv_turn_at; /* where the preview core was pressed; where a drag turning it last was */
static int g_pv_turn; static double g_pv_turn_t; static float g_pv_tvx, g_pv_tvy; /* pressed on it (no move key): held, or turning it */
static double g_pv_orbit, g_pv_lock_until, g_pv_last;
static float g_set_scroll, g_set_scroll_to, g_set_content_h, g_set_view_h; /* the page scrolls when it is taller than its room; it glides to _to */
static int g_sizing;                /* dragging an edge or the corner: 1 right, 2 bottom, 3 both */
static POINT g_size_from;
static float g_size_w0, g_size_h0;
static int g_krec = -1;             /* the shortcut being recorded (its row on the Shortcuts page), -1 none */
static double g_kmsg_until;         /* "add Ctrl, Alt or Shift" shows until then */
static const wchar_t *g_kmsg;
static int g_md_row = -1, g_md_id, g_md_moved; /* a motion tile pressed: a click plays it, a drag moves it in or out of use */
static POINT g_md_from, g_md_at;
static RECT g_mzone[3][2];          /* [routines, actions, black hole][in use, more]: where a dragged tile may be dropped */
static const char *HOLE_ICON[HA_N] = {"", "unfold", "sparkles", "sunDim", "scanEye", "rotateCcw", "orbit"};
static const char *ATK_ICON[] = {"", "rocket", "hammer", "orbit", "triangle"};
_Static_assert(sizeof ATK_ICON / sizeof *ATK_ICON == RA_N, "one icon per READY attack (a missing one crashes the Motions page)");
static int own_rdy(void) { return g_pv_st == ST_READY; } /* row 2 of the Motions page: READY's attacks, else the black hole's actions */

static float S(float v) { return v * (float)g_sdpi; }
static RECT SR(float x0, float y0, float x1, float y1) { RECT r = {(int)lroundf(S(x0)), (int)lroundf(S(y0)), (int)lroundf(S(x1)), (int)lroundf(S(y1))}; return r; }
static float SL_W(void) { return g_set_w; }  /* the window's size in 96-dpi units */
static float SL_H(void) { return g_set_h; }
static int SW_W(void) { return (int)lroundf(S(SL_W())); }
static int SW_H(void) { return (int)lroundf(S(SL_H())); }
static float side_w(void) { return g_set_side ? 188 : 58; }
#define SET_TOP 52.0f /* the top bar's height; the preview starts under it */
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
        case R_HOLETILT: if (i < 5 && HOLE_TILT == TILTS[i]) return i; break;
        case R_OTHERS: return g_others;
        case R_HOLELENS: return HOLE_ACTS >> HA_LENS & 1;
        case R_CHATAT: return g_chat_middle;
        case R_CHATFONT: return !g_chat_mono;
        case R_TEXT: if (i < 5 && g_font == FONTS[i]) return i; break;
        case R_LABELS: return g_labels;
        case R_AWAY: if (i < 8 && g_away_after == AWAYS[i]) return i; break;
        case R_HOLEGPU: return g_gpu ? 0 : 1;
        case R_CHATSIZE: if (i < 4 && g_chat_size == CHATSIZES[i]) return i; break;
        case R_CHATBG: return g_chat_bg;
        case R_WHEEL: return g_wheel_spin;
        case R_TEXTCOL: return g_text_col;
        case R_MODE: return !g_single;
        case R_SMOOTH: return g_smooth;
        case R_RIDLE: case R_RTHINK: case R_RBUSY: case R_RREST: case R_RHAND: { Smooth m = *smooth_now(); return rate_stop(*rate_of(&m, row)); }
        case R_THREADS: for (int k = 0; k < 5; k++) if (THREADS[k] == g_threads) return k; return 0;
        case R_MOVEMOD: return g_move_mod == VK_CONTROL ? 1 : g_move_mod == VK_SHIFT ? 2 : 0;
        case R_SEND: return g_send_ctrl;
        case R_BACKDROP: return g_backdrop;
        case R_BDW: return g_bd_w;
        case R_BDA: return g_bd_a;
        case R_BDCORE: return BACKDROP_CORE;
        case R_SHADECOL: return g_shade_col;
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
    int palette = (row == R_CHATBG || row == R_TEXTCOL || row == R_SHADECOL) && val == 5;
    if (val == set_current(row) && row != R_OTHERS && row != R_BDCORE && row != R_HOLELENS && !palette) return; /* (a switch: flipped whatever it is) */
    if (palette && !pick_colour(row == R_CHATBG ? &g_chat_custom : row == R_SHADECOL ? &g_shade_custom : &g_text_custom)) return; /* cancelled: as it was */
    switch (row) {
    case R_SHAPE: snprintf(g_shape_name, sizeof g_shape_name, "%s", SHAPES[val]); shape_make(&g_shape, g_shape_name); break;
    case R_CORE: g_core = CORES[val]; break;
    case R_GLOW: g_glow = GLOWS[val]; break;
    case R_FRAME: snprintf(g_frame, sizeof g_frame, "%s", FRAMES[val]); g_pv_lock_until = wall() + 2.5; break;
    case R_SCALE: g_scale = SCALES[val]; main_place(); break;
    case R_GRAVITY: g_gravity = GRAVITIES[val]; break;
    case R_HOLETILT: HOLE_TILT = TILTS[val]; break;
    case R_OTHERS: g_others = !g_others; break;
    case R_CHATAT: g_chat_middle = val; break;
    case R_CHATFONT: g_chat_mono = !val; chat_refont(); break;
    case R_TEXT: g_font = FONTS[val]; chat_refont(); break;
    case R_LABELS: g_labels = val; break;
    case R_AWAY: if (val >= 0 && val < 8) g_away_after = AWAYS[val]; break;
    case R_HOLEGPU: g_gpu = val == 0; break;
    case R_CHATSIZE: g_chat_size = CHATSIZES[val]; chat_hide(); break;
    case R_CHATBG: g_chat_bg = val; if (g_chat) InvalidateRect(g_chat, NULL, FALSE); if (g_input) { SendMessageW(g_edit, EM_SETBKGNDCOLOR, 0, chat_field()); edit_style(); InvalidateRect(g_input, NULL, FALSE); } break;
    case R_WHEEL: g_wheel_spin = val; break;
    case R_TEXTCOL: g_text_col = val; if (g_chat) InvalidateRect(g_chat, NULL, FALSE); if (g_input) { edit_style(); InvalidateRect(g_input, NULL, FALSE); } break;
    case R_MODE: toggle_mode(); break;
    case R_SMOOTH: g_smooth = val; break;
    case R_RIDLE: case R_RTHINK: case R_RBUSY: case R_RREST: case R_RHAND: /* a rate of its own: the preset becomes Custom, starting from it */
        if (val < 0 || val >= NRATES) return;
        if (g_smooth != SM_CUSTOM) { g_smooth_custom = *smooth_now(); g_smooth = SM_CUSTOM; }
        *rate_of(&g_smooth_custom, row) = RATES[val];
        break;
    case R_THREADS: if (val >= 0 && val < 5) g_threads = THREADS[val]; break;
    case R_MOVEMOD: g_move_mod = val == 1 ? VK_CONTROL : val == 2 ? VK_SHIFT : VK_MENU; break;
    case R_SEND: g_send_ctrl = val; break;
    case R_BACKDROP: g_backdrop = val; break;
    case R_BDW: if (val >= 0 && val < 4) { g_bd_w = val; bd_apply(); } break;
    case R_BDA: if (val >= 0 && val < 4) { g_bd_a = val; bd_apply(); } break;
    case R_BDCORE: BACKDROP_CORE = !BACKDROP_CORE; break;
    case R_HOLELENS: HOLE_ACTS ^= 1u << HA_LENS; break;
    case R_SHADECOL: g_shade_col = val; break;
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
    chat_refont();
    main_place();
    hotkeys_apply();
    settings_applied(0);
}

/* one page back to the launch options (the others stay) */
static void page_reset(int pg) {
    switch (pg) {
    case PG_LOOK:
        snprintf(g_shape_name, sizeof g_shape_name, "%s", g_launch.shape); shape_make(&g_shape, g_shape_name);
        snprintf(g_frame, sizeof g_frame, "%s", g_launch.frame);
        g_glow = g_launch.glow; g_core = g_launch.core; g_scale = g_launch.scale; g_backdrop = g_launch.backdrop; g_shade_col = g_launch.shade_col; g_shade_custom = g_launch.shade_custom;
        g_bd_w = g_launch.bd_w; g_bd_a = g_launch.bd_a; BACKDROP_CORE = g_launch.bd_core; bd_apply(); main_place(); break;
    case PG_CLUSTER: g_others = g_launch.others; g_labels = g_launch.labels; g_gravity = g_launch.gravity; g_away_after = g_launch.away; g_wheel_spin = g_launch.wheel_spin; break;
    case PG_REPLIES:
        g_chat_mono = g_launch.chat_mono; g_font = g_launch.font; g_chat_size = g_launch.chat_size; g_chat_bg = g_launch.chat_bg;
        g_text_col = g_launch.text_col; g_chat_middle = g_launch.chat_middle; chat_refont(); chat_hide(); break;
    case PG_MOTIONS: motion_map_default(MOTION_MAP); HOLE_ACTS = (1u << HA_N) - 2; READY_ATKS = (1u << RA_N) - 2; break;
    case PG_KEYS:
        snprintf(g_mode_hotkey, sizeof g_mode_hotkey, "%s", g_launch.hk[0]); snprintf(g_hk_hide, sizeof g_hk_hide, "%s", g_launch.hk[1]);
        snprintf(g_hk_reply, sizeof g_hk_reply, "%s", g_launch.hk[2]); snprintf(g_hk_set, sizeof g_hk_set, "%s", g_launch.hk[3]);
        g_move_mod = g_launch.move_mod; g_send_ctrl = g_launch.send_ctrl; hotkeys_apply(); break;
    case PG_PERF: g_smooth = g_launch.smooth; g_smooth_custom = g_launch.custom; g_threads = g_launch.threads; g_gpu = 1; break;
    }
    settings_applied(1);
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
    /* the solid colour, premultiplied, for pixels wholly inside a fill drawn at full strength */
    uint32_t solid = (uint32_t)lroundf(fminf(1, c.b) * 255) | (uint32_t)lroundf(fminf(1, c.g) * 255) << 8 | (uint32_t)lroundf(fminf(1, c.r) * 255) << 16 | 0xff000000u;
    float E = line > 0 ? line + 2 : 2; /* how near an edge a pixel needs its distance worked out */
    for (int y = by0; y < by1; y++) {
        float qy = fabsf(y + 0.5f - cy) - (hy - rad);
        int sx0 = bx1, sx1 = bx1; /* the inside span of this row: filled at once (a fill) or skipped (an outline) */
        if (-qy > E) { sx0 = (int)ceilf(x0 + E); sx1 = (int)floorf(x1 - E); if (sx0 < bx0) sx0 = bx0; if (sx1 > bx1) sx1 = bx1; if (sx1 < sx0) sx1 = sx0; }
        for (int x = bx0; x < bx1; x++) {
            if (x == sx0 && sx1 > sx0) {
                if (line <= 0) {
                    if (a >= 1) { uint32_t *d = pn->px + (size_t)y * pn->W; for (int k = sx0; k < sx1; k++) d[k] = solid; }
                    else for (int k = sx0; k < sx1; k++) px_over(pn, k, y, c, a);
                }
                x = sx1 - 1;
                continue;
            }
            float qx = fabsf(x + 0.5f - cx) - (hx - rad);
            float d = hypotf(fmaxf(qx, 0), fmaxf(qy, 0)) + fminf(fmaxf(qx, qy), 0) - rad;
            float cov = line > 0 ? 0.5f - (fabsf(d + line * 0.5f) - line * 0.5f) : 0.5f - d;
            if (cov > 0) px_over(pn, x, y, c, a * fminf(1, cov));
        }
    }
}
static void box(Pane *pn, float x0, float y0, float x1, float y1, float rad, unsigned fill, unsigned edge, float ew) {
    aa_rrect(pn, S(x0), S(y0), S(x1), S(y1), S(rad), hexc(fill), 1, 0);
    if (ew > 0) aa_rrect(pn, S(x0), S(y0), S(x1), S(y1), S(rad), hexc(edge), 1, S(ew));
}
static void frame(Pane *pn, float x0, float y0, float x1, float y1, float rad, unsigned edge, float ew) { aa_rrect(pn, S(x0), S(y0), S(x1), S(y1), S(rad), hexc(edge), 1, S(ew)); }
/* a control over the preview: its rect laid back over the picture, its outline into the coverage mask */
static void ovr_add(float x0, float y0, float x1, float y1, float rad) {
    if (g_ovr_n < 16) g_ovr[g_ovr_n++] = SR(x0, y0, x1, y1);
    aa_rrect(&g_om, S(x0), S(y0), S(x1), S(y1), S(rad), (Col){1, 1, 1}, 1, 0);
}
static void tx(float x0, float y0, float x1, float y1, const wchar_t *t, HFONT f, unsigned c, UINT how) {
    if (g_tx_n >= 400) return;
    RECT r = SR(x0, y0, x1, y1);
    if (g_clipping) { /* on the page: only what shows between its top and bottom */
        if (r.bottom <= g_clip.top || r.top >= g_clip.bottom) return;
    }
    TextOp *o = &g_tx[g_tx_n++];
    o->r = r;
    o->clip = g_clipping && (r.top < g_clip.top || r.bottom > g_clip.bottom); /* half out: drawn, cut at the edge */
    wcsncpy(o->t, t, 159); o->t[159] = 0;
    o->f = f; o->c = hexr(c); o->how = how;
}
static void hit_px(RECT r, int kind, int row, int val, int n, float t0, float t1) {
    if (g_clipping) { RECT c; if (!IntersectRect(&c, &r, &g_clip)) return; r = c; }
    if (g_hits_n < 400) g_hits[g_hits_n++] = (SetHit){r, kind, row, val, n, t0, t1};
}
static void hit(float x0, float y0, float x1, float y1, int kind, int row, int val) { hit_px(SR(x0, y0, x1, y1), kind, row, val, 0, 0, 0); }
static int is_hot(int kind, int row, int val) { return g_hot >= 0 && g_hits[g_hot].kind == kind && g_hits[g_hot].row == row && g_hits[g_hot].val == val; }
/* a text's width in logical units */
static float tw_of(const wchar_t *t, HFONT f) {
    SIZE z; HDC dc = g_sl.mem;
    SelectObject(dc, f);
    GetTextExtentPoint32W(dc, t, (int)wcslen(t), &z);
    return z.cx / (float)g_sdpi;
}

/* a Lucide icon (lucide.h) by name, `size` units across, centred on (cx, cy), in its 2-unit round strokes */
static void lu_icon(Pane *pn, const char *name, float cx, float cy, float size, unsigned col) {
    for (int i = 0; i < LU_N; i++) {
        if (strcmp(LU_ICON[i].name, name)) continue;
        float k = S(size) / 24, x0 = S(cx) - S(size) / 2, y0 = S(cy) - S(size) / 2, w = fmaxf(1.1f, 2 * k);
        Col c = hexc(col);
        int p = 0;
        for (int r = 0; r < LU_ICON[i].run; r++) p += LU_RUNS[r];
        for (int r = LU_ICON[i].run; r < LU_ICON[i].run + LU_ICON[i].n; r++) {
            const float *a = LU_PTS + 2 * p;
            for (int j = 0; j + 1 < LU_RUNS[r]; j++)
                aa_seg(pn, x0 + a[2 * j] * k, y0 + a[2 * j + 1] * k, x0 + a[2 * j + 2] * k, y0 + a[2 * j + 3] * k, w, c, 1);
            p += LU_RUNS[r];
        }
        return;
    }
}

static void set_fonts(void);
/* the Settings window moved to a monitor with another scale: its fonts are made again at that size */
static void set_refont(void) {
    HFONT *f[8] = {&g_sf_title, &g_sf_card, &g_sf_text, &g_sf_label, &g_sf_small, &g_sf_aa, &g_sf_aam, &g_sf_cap};
    for (int i = 0; i < 8; i++) if (*f[i]) { DeleteObject(*f[i]); *f[i] = NULL; }
    g_sl_dirty = 1;
}
static void set_fonts(void) {
    if (g_sf_title) return;
    const wchar_t *ui = L"Segoe UI";
    g_sf_title = CreateFontW(-(int)S(16), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, ui);
    g_sf_card = CreateFontW(-(int)S(15), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, ui);
    g_sf_text = CreateFontW(-(int)S(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, ui);
    g_sf_label = CreateFontW(-(int)S(12), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, ui);
    g_sf_small = CreateFontW(-(int)S(11.5f), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, ui);
    g_sf_cap = CreateFontW(-(int)S(11), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, ui);
    g_sf_aa = CreateFontW(-(int)S(16), 0, 0, 0, FW_MEDIUM, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, ui);
    g_sf_aam = CreateFontW(-(int)S(16), 0, 0, 0, FW_MEDIUM, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, mono_face());
}

/* a tile, button or chip that can be the chosen one: chosen, the orange frame alone, the fill as it was */
static void pick_box(Pane *pn, float x0, float y0, float x1, float y1, float rad, int on, int hot, const wchar_t *t, HFONT f) {
    if (on) box(pn, x0, y0, x1, y1, rad, hot ? S_CHIP_HOT : S_TILE, S_OR, 1.5f);
    else box(pn, x0, y0, x1, y1, rad, hot ? S_CHIP_HOT : S_TILE, hot ? S_EDGE_HOT : S_EDGE, 1);
    if (t) tx(x0, y0, x1, y1, t, f, on ? S_TEXT : S_CHIP_TXT, DT_CENTER);
}
/* a pill: grey, the chosen one framed in orange */
static void pill(Pane *pn, float x0, float y0, float x1, float y1, int on, int hot, const wchar_t *t) {
    box(pn, x0, y0, x1, y1, (y1 - y0) / 2, hot ? S_CHIP_HOT : S_CHIP, S_OR, on ? 1.5f : 0);
    if (t) tx(x0, y0, x1, y1, t, g_sf_label, on ? S_TEXT : S_CHIP_TXT, DT_CENTER);
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


/* ---- the preview: four made-up sessions, the main one in the state picked */
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

/* ---- the motion map: which routines and actions each state may use (MOTION_MAP) */
static const int MOT_STATES[ST_N] = {ST_IDLE, ST_THINK, ST_WORK, ST_WAIT, ST_CHEER, ST_SLEEP, ST_READY, ST_SPEAK, ST_LISTEN, ST_TRANSCRIBE, ST_STANDBY, ST_ERROR};
static const wchar_t *MOT_STATE_NAMES[ST_N] = {L"Idle", L"Thinking", L"Working", L"Needs you", L"Done", L"Asleep",
                                              L"Ready", L"Speaking", L"Listening", L"Transcribing", L"Away", L"Error"};
static const char *MOT_RT_ICON[RT_N] = {"rotateCw", "eye", "puzzle", "blend", "rotateCw", "swords", "cog", "orbit", "dna", "keyboard", "hammer", "grid", "arrows", "minimize",
                                    "fish", "tower", "bell", "unlock", "flower", "chevrons", "moon", "hole", "radio", "mic", "binary", "lock", "zap", "crosshair"};
static const char *MOT_W_ICON[W_N] = {"", "eye", "scanEye", "refresh", "vibrate", "sunDim", "crosshair", "step", "unfold", "scan", "triangle", "alert", "waves", "orbit", "flower", "sparkles", "rotateCcw"};
_Static_assert(RT_N == 28 && W_N == 17, "MOT_RT_ICON and MOT_W_ICON have one icon per routine and action");
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
static const char *mot_name(int row, int id) { return row == 0 ? ROUTINES[id].name : row == 1 ? WHIMS[id].name : own_rdy() ? READY_ATK_NAME[id] : HOLE_ACT_NAME[id]; }
static const char *mot_icon(int row, int id) { return row == 0 ? MOT_RT_ICON[id] : row == 1 ? MOT_W_ICON[id] : own_rdy() ? ATK_ICON[id] : HOLE_ICON[id]; }
static const char *mot_about(int row, int id) { return row == 0 ? ROUTINES[id].about : row == 1 ? (WHIMS[id].about ? WHIMS[id].about : "") : own_rdy() ? READY_ATK_ABOUT[id] : HOLE_ACT_ABOUT[id]; }
static uint32_t mot_set(int row) { return row == 0 ? MOTION_MAP[g_pv_st].rt : row == 1 ? MOTION_MAP[g_pv_st].act : own_rdy() ? READY_ATKS : HOLE_ACTS; }
static void mot_play(int row, int id) {
    Session *m = &g_pv_room.s[0];
    if (row == 0) motion_play(m, id);
    else if (row == 2 && own_rdy()) ready_play(m, id);
    else if (row == 2) hole_play(m, id); /* (it becomes a black hole first if it is not one) */
    else whim_play(m, id, wall());
    g_pv_lock_until = 0;
}
static void mot_toggle(int row, int id) {
    MotionMap *mm = &MOTION_MAP[g_pv_st];
    uint32_t *set = row == 0 ? &mm->rt : row == 1 ? &mm->act : own_rdy() ? &READY_ATKS : &HOLE_ACTS, bit = 1u << id;
    if (row == 0 && (*set & bit) && bits(*set) == 1) { g_mot_msg_until = wall() + 3; g_sl_dirty = 1; return; } /* a state needs a routine */
    *set ^= bit;
    if (*set & bit) mot_play(row, id); /* switched on: shown at once */
    else if (row == 0) mot_refresh(g_pv_st, id);
    settings_applied(1);
}

/* ---- the shortcuts: four global hotkeys (RegisterHotKey), kept as "ctrl+alt+c" like --mode-hotkey */
static char *hk_spec(int i) { return i == 0 ? g_mode_hotkey : i == 1 ? g_hk_hide : i == 2 ? g_hk_reply : g_hk_set; }
static const wchar_t *HK_LABEL[4] = {L"One core / all cores", L"Show or hide Vigil", L"Reply to the main core", L"Open Settings"};
static const char *HK_ICON[4] = {"layers", "eyeOff", "reply", "sliders"};
/* a spec as key caps: "ctrl+alt+c" → Ctrl, Alt, C */
static int hk_caps(const char *spec, wchar_t caps[5][12]) {
    int n = 0;
    if (!spec[0] || !strcmp(spec, "none")) return 0;
    char buf[64];
    snprintf(buf, sizeof buf, "%s", spec);
    for (char *t = strtok(buf, "+"); t && n < 5; t = strtok(NULL, "+")) {
        const char *w = !strcmp(t, "ctrl") ? "Ctrl" : !strcmp(t, "alt") ? "Alt" : !strcmp(t, "shift") ? "Shift" : !strcmp(t, "win") ? "Win" : !strcmp(t, "space") ? "Space" : NULL;
        if (w) swprintf(caps[n], 12, L"%hs", w);
        else { swprintf(caps[n], 12, L"%hs", t); for (wchar_t *c = caps[n]; *c; c++) *c = towupper(*c); }
        n++;
    }
    return n;
}

/* ---- a page: section cards of rows; two columns when there is room */
enum { RK_SLIDER, RK_SEG, RK_TOGGLE, RK_SHAPES, RK_FONTS, RK_SWATCH, RK_HOTKEY, RK_CAPS };
typedef struct { int kind, row, n; const wchar_t *label, *hint; const wchar_t *const *names; const wchar_t *lo, *hi; const char *icon; } SRow;
typedef struct { const wchar_t *title; int n; SRow r[6]; } SSec;
static const wchar_t *GLOW_NAMES[3] = {L"Soft", L"Normal", L"Bright"}, *CORE_NAMES[4] = {L"XS", L"S", L"M", L"L"},
                     *TEXT_SIZES[5] = {L"XS", L"S", L"M", L"L", L"XL"}, *BOX_SIZES[4] = {L"S", L"M", L"L", L"XL"},
                     *MODE_NAMES[2] = {L"Single core", L"All cores"}, *LABEL_NAMES[2] = {L"On hover", L"Always"},
                     *AWAY_NAMES[8] = {L"1 min", L"2 min", L"5 min", L"10 min", L"15 min", L"30 min", L"1 hour", L"Never"}, *OPEN_NAMES[2] = {L"Next to core", L"Middle"},
                     *SM_NAMES[SM_N] = {L"Saver", L"Balanced", L"Smooth", L"Max", L"Custom"},
                     *MOD_NAMES[3] = {L"Alt", L"Ctrl", L"Shift"}, *BACKDROP_NAMES[3] = {L"Off", L"On", L"Auto"}, *BDW_NAMES[4] = {L"Thin", L"Medium", L"Thick", L"Heavy"}, *BDA_NAMES[4] = {L"Light", L"Medium", L"Strong", L"Max"}, *SEND_NAMES[2] = {L"Enter", L"Ctrl+Enter"};
static const wchar_t *SM_NOTE[SM_N] = {L"for a laptop on battery: cores still answer the mouse at once", L"the rates the panel uses by default",
                                      L"at least 30 pictures a second; busy states 60", L"every picture at the monitor's refresh rate: the most CPU", L"your own rates: set them below"};
static int page_sections(int pg, SSec *s) {
    int n = 0;
#define ROW(sec, ...) (s[sec].r[s[sec].n++] = (SRow){__VA_ARGS__})
    memset(s, 0, sizeof(SSec) * 4);
    switch (pg) {
    case PG_LOOK:
        s[0].title = L"SHELL"; s[1].title = L"ON SCREEN"; s[2].title = L"BACKDROP"; n = 3;
        ROW(0, RK_SHAPES, R_SHAPE, 4, L"Shape", NULL, NULL, NULL, NULL, NULL);
        ROW(0, RK_SLIDER, R_GLOW, 3, L"Glow", NULL, GLOW_NAMES, NULL, NULL, NULL);
        ROW(0, RK_SLIDER, R_CORE, 4, L"Inner core", NULL, CORE_NAMES, NULL, NULL, NULL);
        ROW(1, RK_SLIDER, R_SCALE, 4, L"Size", NULL, NULL, L"S", L"L", NULL);
        ROW(1, RK_SEG, R_FRAME, 7, L"Focus frame", L"round a carried core, or one you rest on", FRAME_NAMES, NULL, NULL, NULL);
        ROW(2, RK_SEG, R_BACKDROP, 3, L"Backdrop", L"a soft dark outline along the lines, so they read over a light app; Auto: light apps only", BACKDROP_NAMES, NULL, NULL, NULL);
        ROW(2, RK_SWATCH, R_SHADECOL, 6, L"Darkness colour", L"of the outline", SHADE_NAMES, NULL, NULL, NULL);
        ROW(2, RK_SLIDER, R_BDW, 4, L"Outline width", NULL, BDW_NAMES, NULL, NULL, NULL);
        ROW(2, RK_SLIDER, R_BDA, 4, L"Outline strength", NULL, BDA_NAMES, NULL, NULL, NULL);
        ROW(2, RK_TOGGLE, R_BDCORE, 1, L"Behind the centre", L"a small soft disc behind the bright centre too", NULL, NULL, NULL, NULL);
        break;
    case PG_CLUSTER:
        s[0].title = L"LAYOUT"; s[1].title = L"MOTION"; s[2].title = L"AWAY"; n = 3;
        ROW(0, RK_SEG, R_MODE, 2, L"Layout", NULL, MODE_NAMES, NULL, NULL, NULL);
        ROW(0, RK_TOGGLE, R_OTHERS, 1, L"Other sessions", L"small cores round the main one", NULL, NULL, NULL, NULL);
        ROW(0, RK_SEG, R_LABELS, 2, L"Names over cores", L"all-cores mode", LABEL_NAMES, NULL, NULL, NULL);
        ROW(1, RK_SLIDER, R_GRAVITY, 4, L"Gravity", L"drag the core above to feel it", NULL, L"Floaty", L"Firm", NULL);
        ROW(1, RK_SEG, R_WHEEL, 5, L"Scroll on a core", NULL, WHEEL_NAMES, NULL, NULL, NULL);
        ROW(2, RK_SLIDER, R_AWAY, 8, L"Rest after", L"no mouse or keys this long: the cores show STANDBY", AWAY_NAMES, NULL, NULL, NULL);
        break;
    case PG_REPLIES:
        s[0].title = L"TEXT"; s[1].title = L"BOX"; n = 2;
        ROW(0, RK_FONTS, R_CHATFONT, 2, L"Font", NULL, NULL, NULL, NULL, NULL);
        ROW(0, RK_SLIDER, R_TEXT, 5, L"Text size", NULL, TEXT_SIZES, NULL, NULL, NULL);
        ROW(1, RK_SLIDER, R_CHATSIZE, 4, L"Box size", L"how far it grows before it scrolls", BOX_SIZES, NULL, NULL, NULL);
        ROW(1, RK_SWATCH, R_CHATBG, 6, L"Box colour", NULL, CHAT_BG_NAMES, NULL, NULL, NULL);
        ROW(1, RK_SWATCH, R_TEXTCOL, 6, L"Text colour", NULL, TEXT_NAMES, NULL, NULL, NULL);
        ROW(1, RK_SEG, R_CHATAT, 2, L"Opens", NULL, OPEN_NAMES, NULL, NULL, NULL);
        break;
    case PG_KEYS:
        s[0].title = L"ANYWHERE"; s[1].title = L"ON A CORE"; s[2].title = L"IN THE REPLY LINE"; n = 3;
        for (int i = 0; i < 4; i++) ROW(0, RK_HOTKEY, i, 0, HK_LABEL[i], NULL, NULL, NULL, NULL, HK_ICON[i]);
        ROW(1, RK_SEG, R_MOVEMOD, 3, L"Move or carry a core", L"hold it, then drag (move) or click (carry)", MOD_NAMES, NULL, NULL, "move");
        ROW(1, RK_CAPS, 0, 0, L"Type a reply", NULL, NULL, L"Double-click", NULL, "pointer");
        ROW(1, RK_CAPS, 0, 0, L"Spin the shell", L"flick it and it coasts back", NULL, L"Drag", NULL, "rotateCw");
        ROW(2, RK_SEG, R_SEND, 2, L"Send", L"the other one makes a new line", SEND_NAMES, NULL, NULL, "send");
        ROW(2, RK_CAPS, 0, 0, L"Clear the line", L"with nothing selected; else it copies", NULL, L"Ctrl+C", NULL, "rotateCcw");
        ROW(2, RK_CAPS, 0, 0, L"Delete a word", L"back to the space before it; Ctrl+U: the whole line before", NULL, L"Ctrl+W", NULL, "newline");
        ROW(2, RK_CAPS, 0, 0, L"Close", NULL, NULL, L"Esc", NULL, "x");
        break;
    case PG_PERF:
        s[0].title = L"SMOOTHNESS"; s[1].title = L"PICTURES A SECOND"; s[2].title = L"DRAWING"; n = 3;
        ROW(0, RK_SEG, R_SMOOTH, g_smooth == SM_CUSTOM ? SM_N : SM_N - 1, L"Preset", SM_NOTE[g_smooth], SM_NAMES, NULL, NULL, NULL);
        ROW(1, RK_SLIDER, R_RIDLE, NRATES, L"Idle", NULL, RATE_NAMES, NULL, NULL, "eye");
        ROW(1, RK_SLIDER, R_RTHINK, NRATES, L"Thinking", NULL, RATE_NAMES, NULL, NULL, "sparkles");
        ROW(1, RK_SLIDER, R_RBUSY, NRATES, L"Busy", L"working, done, a motion", RATE_NAMES, NULL, NULL, "zap");
        ROW(1, RK_SLIDER, R_RREST, NRATES, L"Asleep or away", NULL, RATE_NAMES, NULL, NULL, "moon");
        ROW(1, RK_SLIDER, R_RHAND, NRATES, L"In the hand", L"pointed at or held", RATE_NAMES, NULL, NULL, "hand");
        ROW(2, RK_SEG, R_THREADS, 5, L"Drawing threads", L"how many share drawing a big core; Auto: about one a physical core", THREAD_NAMES, NULL, NULL, "layers");
        {
            static wchar_t gnote[200];
            if (gpu_init()) swprintf(gnote, 200, L"asleep, it is drawn on the graphics card: a sixth of the CPU");
            else swprintf(gnote, 200, L"no graphics card it can use here (%hs): the CPU draws it", gpu_why());
            ROW(2, RK_SEG, R_HOLEGPU, 2, L"Black hole drawn on", gnote, HOLEGPU_NAMES, NULL, NULL, "moon");
        }
        break;
    }
#undef ROW
    return n;
}

/* a row's chips: their width, and whether they fit beside the name */
static float seg_w(const SRow *r) {
    float w = 0;
    for (int i = 0; i < r->n; i++) w += tw_of(r->names[i], g_sf_label) + 24 + (i ? 4 : 0);
    return w;
}
/* how wide a row's control is (its name and hint keep clear of it) */
static float ctrl_w(const SRow *r) {
    switch (r->kind) {
    case RK_SLIDER: return fminf(230, 400) + (r->lo ? 48 : 0) + 44;
    case RK_SEG: return seg_w(r);
    case RK_TOGGLE: return 42;
    case RK_SHAPES: return 4 * 50 - 6;
    case RK_FONTS: return 2 * 110 - 6;
    case RK_SWATCH: return 6 * 32 - 6;
    case RK_HOTKEY: return 160;
    case RK_CAPS: return tw_of(r->lo, g_sf_label) + 14;
    }
    return 0;
}
static float row_h(const SRow *r, float colw) {
    float h = r->hint ? 56 : 48;
    if (r->kind == RK_SEG) { /* the chips go under the name when the name and hint would not fit beside them */
        float text = fmaxf(tw_of(r->label, g_sf_text), r->hint ? fminf(tw_of(r->hint, g_sf_small), 260) : 0);
        if (seg_w(r) + 16 + text > colw - 32) h += 36;
    }
    if (r->kind == RK_HOTKEY && g_hk_fail[r->row]) h = 56;
    return h;
}
static float sec_h(const SSec *s, float colw) {
    float h = 36 + 6;
    for (int i = 0; i < s->n; i++) h += row_h(&s->r[i], colw);
    return h;
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
    int hot = (g_hot >= 0 && g_hits[g_hot].kind == K_SLIDE && g_hits[g_hot].row == row) || (g_slide >= 0 && g_slide_row == row);
    if (hot) aa_disc(pn, S(kx), S(y), S(12), hexc(S_OR), 0.18f);
    aa_disc(pn, S(kx), S(y), S(8), hexc(S_KNOB), 1);
    hit_px(SR(x0 - 10, y - 13, x1 + 10, y + 13), K_SLIDE, row, cur, n, S(x0), S(x1));
}

/* one row: its name (and hint) on the left, its control on the right */
static void draw_row(Pane *pn, const SRow *r, float x0, float y, float x1, float h) {
    int warn = r->kind == RK_HOTKEY && g_hk_fail[r->row];
    float base = r->hint || warn ? 56 : 48, lx = x0 + 16, rx = x1 - 16, cy = y + (h > base + 1 ? base / 2 : h / 2);
    int wrapped = h > base + 1;
    if (r->icon) { lu_icon(pn, r->icon, lx + 8, cy, 16, S_LABEL); lx += 28; }
    const wchar_t *hint = warn ? L"held by another app: pick another" : r->hint;
    float tr = wrapped ? rx : rx - ctrl_w(r) - 16; /* the text stops short of the control */
    if (r->kind == RK_SLIDER) tr = rx - fminf(230, (x1 - x0) * 0.5f) - (r->hi ? 34 : 44) - (r->lo ? 48 : 0) - 16;
    if (hint) {
        tx(lx, cy - 18, tr, cy, r->label, g_sf_text, S_TEXT, DT_LEFT | DT_END_ELLIPSIS);
        tx(lx, cy, tr, cy + 16, hint, g_sf_small, warn ? 0xFFB066 : S_MUTED, DT_LEFT | DT_END_ELLIPSIS);
    } else tx(lx, cy - 10, tr, cy + 10, r->label, g_sf_text, S_TEXT, DT_LEFT | DT_END_ELLIPSIS);
    switch (r->kind) {
    case RK_SLIDER: {
        int cur = set_current(r->row);
        const wchar_t *lo = r->lo, *hi = r->hi ? r->hi : (r->names && cur >= 0 ? r->names[cur] : L"");
        float sw = fminf(230, (x1 - x0) * 0.5f), ex = rx - (r->hi ? 34 : 44), sx = ex - sw;
        if (lo) tx(sx - 40, cy - 9, sx - 8, cy + 9, lo, g_sf_small, S_MUTED, DT_RIGHT);
        slider(pn, sx, cy, ex, r->row, r->n);
        tx(ex + 10, cy - 9, rx + 4, cy + 9, hi, g_sf_small, S_MUTED, DT_LEFT);
        break;
    }
    case RK_SEG: {
        float w = seg_w(r), a = wrapped ? lx : rx - w, yy = wrapped ? y + h - 40 : cy - 14;
        int cur = set_current(r->row);
        for (int i = 0; i < r->n; i++) {
            float cw = tw_of(r->names[i], g_sf_label) + 24;
            pill(pn, a, yy, a + cw, yy + 28, cur == i, is_hot(K_PICK, r->row, i), r->names[i]);
            hit(a, yy, a + cw, yy + 28, K_PICK, r->row, i);
            a += cw + 4;
        }
        break;
    }
    case RK_TOGGLE: {
        int on = set_current(r->row) == 1, ht = is_hot(K_TOGGLE, r->row, 0);
        box(pn, rx - 42, cy - 12, rx, cy + 12, 12, on ? S_OR : (ht ? S_EDGE_HOT : S_EDGE), 0, 0);
        aa_disc(pn, S(on ? rx - 12 : rx - 30), S(cy), S(9), hexc(on ? S_ON_OR : S_LABEL), 1);
        hit(x0, y, x1, y + h, K_TOGGLE, r->row, 0);
        break;
    }
    case RK_SHAPES:
        for (int i = 0; i < 4; i++) {
            float a = rx - (4 - i) * 50 + 6;
            int sel = set_current(R_SHAPE) == i;
            pick_box(pn, a, cy - 20, a + 44, cy + 20, 9, sel, is_hot(K_PICK, R_SHAPE, i), NULL, NULL);
            shape_icon(pn, i, a + 22, cy, sel ? S_OR : S_MUTED);
            hit(a, cy - 20, a + 44, cy + 20, K_PICK, R_SHAPE, i);
        }
        break;
    case RK_FONTS:
        for (int i = 0; i < 2; i++) {
            float a = rx - (2 - i) * 110 + 6;
            int sel = set_current(R_CHATFONT) == i;
            pick_box(pn, a, cy - 20, a + 104, cy + 20, 9, sel, is_hot(K_PICK, R_CHATFONT, i), NULL, NULL);
            tx(a + 10, cy - 20, a + 40, cy + 20, L"Aa", i ? g_sf_aa : g_sf_aam, sel ? S_TEXT : S_CHIP_TXT, DT_CENTER);
            tx(a + 42, cy - 20, a + 100, cy + 20, i ? L"Clean" : L"Terminal", g_sf_label, sel ? S_TEXT : S_MUTED, DT_LEFT);
            hit(a, cy - 20, a + 104, cy + 20, K_PICK, R_CHATFONT, i);
        }
        break;
    case RK_SWATCH:
        for (int i = 0; i < 6; i++) {
            float a = rx - (6 - i) * 32 + 6;
            int sel = set_current(r->row) == i, hot = is_hot(K_PICK, r->row, i), text = r->row == R_TEXTCOL;
            unsigned fill = text ? chat_bgx() : r->row == R_SHADECOL ? (i < 5 ? SHADE_COLS[i] : g_shade_custom) : i < 5 ? CHAT_BGS[i] : g_chat_custom;
            box(pn, a, cy - 13, a + 26, cy + 13, 7, fill, sel ? S_OR : hot ? S_EDGE_HOT : S_EDGE, sel ? 1.5f : 1);
            unsigned tc = text ? (i == 5 ? g_text_custom : i ? TEXT_COLS[i] : inkx(0xE6E8F0)) : 0;
            if (i == 5) {
                int light = 0.299f * (fill >> 16 & 255) + 0.587f * (fill >> 8 & 255) + 0.114f * (fill & 255) > 140;
                palette_icon(pn, S(a + 13), S(cy), S(15), text ? tc : light ? 0x2A2A30 : (hot || sel ? 0xFFFFFF : 0xD4D4DA));
            } else if (text) tx(a, cy - 13, a + 26, cy + 13, L"Aa", g_sf_small, tc, DT_CENTER);
            hit(a, cy - 13, a + 26, cy + 13, K_PICK, r->row, i);
        }
        break;
    case RK_HOTKEY: {
        int i = r->row, rec = g_krec == i;
        if (rec) { /* waiting for the keys */
            box(pn, rx - 214, cy - 15, rx, cy + 15, 8, S_CHIP, S_OR, 1.5f);
            tx(rx - 214, cy - 15, rx, cy + 15, wall() < g_kmsg_until && g_kmsg ? g_kmsg : L"Press keys \x00B7 Esc cancels \x00B7 Del clears", g_sf_label, S_TEXT, DT_CENTER);
            hit(rx - 214, cy - 15, rx, cy + 15, K_KREC, 0, i);
            break;
        }
        wchar_t caps[5][12];
        int n = hk_caps(hk_spec(i), caps), hot = is_hot(K_KREC, 0, i);
        float a = rx;
        if (!n) { tx(rx - 90, cy - 10, rx - 8, cy + 10, L"Set\x2026", g_sf_label, hot ? S_TEXT : S_MUTED, DT_RIGHT); a = rx - 96; }
        for (int k = n - 1; k >= 0; k--) {
            float w = fmaxf(24, tw_of(caps[k], g_sf_label) + 14);
            a -= w;
            box(pn, a, cy - 12, a + w, cy + 12, 6, S_CHIP, 0x44444C, 1);
            tx(a, cy - 12, a + w, cy + 12, caps[k], g_sf_label, S_TEXT, DT_CENTER);
            a -= 4;
        }
        if (hot || g_hk_fail[i]) frame(pn, a - 4, cy - 16, rx + 4, cy + 16, 9, g_hk_fail[i] ? S_OR : S_EDGE_HOT, 1.2f);
        hit(a - 4, cy - 16, rx + 4, cy + 16, K_KREC, 0, i);
        break;
    }
    case RK_CAPS: {
        float w = tw_of(r->lo, g_sf_label) + 14;
        box(pn, rx - w, cy - 12, rx, cy + 12, 6, S_CHIP, 0x44444C, 1);
        tx(rx - w, cy - 12, rx, cy + 12, r->lo, g_sf_label, S_TEXT, DT_CENTER);
        break;
    }
    }
}

/* the page's sections, laid out from (x0, y0) to x1; returns the height used */
static float rows_page(Pane *pn, int pg, float x0, float y0, float x1) {
    SSec s[4];
    int n = page_sections(pg, s);
    float cw = x1 - x0;
    int cols = cw >= 820 ? 2 : 1;
    float colw = (cw - 12 * (cols - 1)) / cols, colh[2] = {0, 0};
    for (int k = 0; k < n; k++) {
        int c = cols == 2 && colh[1] < colh[0] ? 1 : 0;
        float x = x0 + c * (colw + 12), y = y0 + colh[c], h = sec_h(&s[k], colw);
        box(pn, x, y, x + colw, y + h, 12, S_CARD, 0, 0);
        tx(x + 16, y + 12, x + colw - 16, y + 30, s[k].title, g_sf_cap, S_MUTED, DT_LEFT);
        float ry = y + 36;
        for (int i = 0; i < s[k].n; i++) {
            float rh = row_h(&s[k].r[i], colw);
            if (i) box(pn, x + 16, ry, x + colw - 16, ry + 1, 0, S_RULE, 0, 0);
            draw_row(pn, &s[k].r[i], x, ry, x + colw, rh);
            ry += rh;
        }
        colh[c] += h + 12;
    }
    return fmaxf(colh[0], colh[1]);
}

/* ---- the Motions page: every state at a glance on the left, the chosen one's motions on the right */
static void mot_tile(Pane *pn, float x0, float y0, float x1, float y1, int row, int id, int on, int dis) {
    if (g_md_row == row && g_md_id == id && g_md_moved) { frame(pn, x0, y0, x1, y1, 10, S_EDGE, 1); return; } /* its place, while it is carried */
    int hot = is_hot(K_MTOG, row, id);
    box(pn, x0, y0, x1, y1, 10, hot && !dis ? S_CHIP_HOT : S_TILE, on ? S_OR : dis ? S_EDGE : hot ? S_EDGE_HOT : S_RULE, on ? 1.5f : 1);
    unsigned ink = on ? S_OR : dis ? 0x55555E : S_MUTED;
    lu_icon(pn, mot_icon(row, id), (x0 + x1) / 2, y0 + 20, 19, ink);
    txa(x0 + 4, y0 + 32, x1 - 4, y1 - 4, mot_name(row, id), g_sf_small, on ? S_TEXT : dis ? 0x55555E : S_CHIP_TXT, DT_CENTER | DT_END_ELLIPSIS);
    if (!dis) hit(x0, y0, x1, y1, K_MTOG, row, id);
}
/* the tiles of one kind: those in use, a MORE rule, the rest; returns the height used */
static float mot_tiles(Pane *pn, int row, float x0, float y0, float x1) {
    int st = g_pv_st;
    uint32_t set = mot_set(row);
    int first = 1, last = row == 0 ? RT_N : row == 1 ? W_N : own_rdy() ? RA_N : HA_N, cols = (int)fmaxf(1, floorf((x1 - x0 + 6) / (96 + 6)));
    float tw = (x1 - x0 - 6 * (cols - 1)) / cols, y = y0;
    wchar_t head[80];
    swprintf(head, 80, row == 0 ? L"ROUTINES \x00B7 %d" : row == 1 ? L"ACTIONS \x00B7 %d" : own_rdy() ? L"ATTACKS \x00B7 %d" : L"BLACK HOLE \x00B7 %d", bits(set & ~1u));
    float hx = x0;
    if (row == 2) { lu_icon(pn, MOT_RT_ICON[own_rdy() ? RT_RESTLESS : RT_HOLE], x0 + 7, y + 9, 13, S_OR); hx += 20; } /* the black hole's own: marked as its */
    tx(hx, y, hx + 120, y + 18, head, g_sf_cap, row == 2 ? S_TEXT : S_MUTED, DT_LEFT);
    tx(hx + (row == 2 ? 120 : 110), y, x1, y + 18, row == 0 ? L"one picked at random each time it starts" : row == 1 ? L"played now and then while left alone" : own_rdy() ? L"thrown at the pointer every second or two; point at the core to read the reply" : L"its own actions, now and then, once it is a black hole", g_sf_small, S_MUTED, DT_LEFT | DT_END_ELLIPSIS);
    y += 26;
    for (int pass = 0; pass < 2; pass++) {
        float zy = y;
        int k = 0;
        for (int id = first; id < last; id++) {
            int on = (set >> id) & 1;
            if (on != !pass) continue;
            if (row == 0 && id == RT_DORMANT) continue; /* (retired: asleep, it is the black hole) */
            if (row == 2 && !own_rdy() && (HA_RETIRED >> id & 1)) continue; /* (Jets: retired) */
            float a = x0 + (k % cols) * (tw + 6), b = y + (k / cols) * 64;
            mot_tile(pn, a, b, a + tw, b + 58, row, id, on, row == 0 && ((id == RT_HOLE && st != ST_SLEEP) || (id == RT_RESTLESS && st != ST_READY)));
            k++;
        }
        if (!pass && !k) { tx(x0, y + 4, x1, y + 52, L"None: drag one up here.", g_sf_small, S_MUTED, DT_CENTER); k = cols; }
        if (pass && !k) { tx(x0, y, x1, y + 30, L"All in use. Drag one down here to stop it.", g_sf_small, 0x6E6E78, DT_CENTER); y += 34; } /* still a place to drop on */
        y += ((k + cols - 1) / cols) * 64;
        g_mzone[row][pass] = SR(x0 - 4, zy - 4, x1 + 4, y); /* where a dragged tile lands in use (0) or out (1) */
        if (g_md_row == row && g_md_moved && PtInRect(&g_mzone[row][pass], g_md_at)) frame(pn, x0 - 4, zy - 4, x1 + 4, y - 2, 12, S_OR, 1.5f); /* it would land here */
        if (!pass) { /* the rule between those in use and the rest */
            tx(x0, y + 4, x0 + 50, y + 20, L"MORE", g_sf_cap, 0x6E6E78, DT_LEFT);
            box(pn, x0 + 44, y + 12, x1, y + 13, 0, S_RULE, 0, 0);
            y += 26;
        }
    }
    return y - y0;
}
/* Asleep as a black hole: a page of its own. The hole itself (its sky, its tilt), its actions as a list (each played
 * with its button or a click on its row, switched on or off with its switch), what your hands can do to it, and below
 * them the routines Asleep may pick from. Returns the height used; draws only when asked (measured first) */
static const char *HOLE_SHORT[HA_N] = {"", "", "a star torn apart as it swings past",
    "a loop of hot gas rises off the disc, snaps and flings a blob out", "", "its disc wobbles round like a top", "a hot clump races round its inner edge"};
static float sleep_body(Pane *pn, float a, float y0, float b, int draw) {
    float y = y0;
    { /* the black hole itself */
        float h = 30 + 56 + 1 + 56 + 6;
        if (draw) {
            box(pn, a, y, b, y + h, 12, S_TILE, S_RULE, 1);
            lu_icon(pn, "hole", a + 19, y + 17, 13, S_OR);
            tx(a + 32, y + 8, b - 12, y + 26, L"THE BLACK HOLE", g_sf_cap, S_TEXT, DT_LEFT);
            SRow lens = {RK_TOGGLE, R_HOLELENS, 1, L"Lensing", L"the stars behind it, bent round its shadow", NULL, NULL, NULL, "scanEye"};
            SRow tilt = {RK_SLIDER, R_HOLETILT, 5, L"Tilt", L"how fast it tilts and rolls on its own", TILT_NAMES, NULL, NULL, "rotateCcw"};
            draw_row(pn, &lens, a, y + 30, b, 56);
            box(pn, a + 16, y + 86, b - 16, y + 87, 0, S_RULE, 0, 0);
            draw_row(pn, &tilt, a, y + 87, b, 56);
        }
        y += h + 12;
    }
    { /* its actions */
        int n = 0;
        for (int id = 1; id < HA_N; id++) if (id != HA_LENS && !(HA_RETIRED >> id & 1)) n++;
        float h = 30 + n * 52 + 6;
        if (draw) {
            box(pn, a, y, b, y + h, 12, S_TILE, S_RULE, 1);
            lu_icon(pn, "sparkles", a + 19, y + 17, 13, S_OR);
            wchar_t head[64];
            swprintf(head, 64, L"ITS ACTIONS \x00B7 %d", bits(HOLE_ACTS & ~1u & ~(1u << HA_LENS) & ~HA_RETIRED));
            tx(a + 32, y + 8, a + 200, y + 26, head, g_sf_cap, S_TEXT, DT_LEFT);
            tx(a + 150, y + 8, b - 16, y + 26, L"one now and then, while it sleeps", g_sf_small, S_MUTED, DT_RIGHT | DT_END_ELLIPSIS);
            int k = 0;
            for (int id = 1; id < HA_N; id++) {
                if (id == HA_LENS || (HA_RETIRED >> id & 1)) continue;
                float ry = y + 30 + 52 * k++;
                int on = HOLE_ACTS >> id & 1, hr = is_hot(K_HPLAY, 2, id), hs = is_hot(K_HTOG, 2, id);
                if (k > 1) box(pn, a + 16, ry, b - 16, ry + 1, 0, S_RULE, 0, 0);
                if (hr) box(pn, a + 6, ry + 3, b - 6, ry + 49, 9, S_CHIP_HOT, 0, 0);
                lu_icon(pn, HOLE_ICON[id], a + 30, ry + 26, 18, on ? S_OR : S_MUTED);
                txa(a + 52, ry + 8, b - 120, ry + 27, HOLE_ACT_NAME[id], g_sf_text, on ? S_TEXT : S_CHIP_TXT, DT_LEFT | DT_END_ELLIPSIS);
                txa(a + 52, ry + 27, b - 120, ry + 45, HOLE_SHORT[id], g_sf_small, S_MUTED, DT_LEFT | DT_END_ELLIPSIS);
                /* play: a round button with a triangle; the row plays it too */
                float px = b - 88, cy = ry + 26;
                aa_disc(pn, S(px), S(cy), S(14), hexc(hr ? S_OR : S_CHIP), 1);
                float P[6] = {S(px - 4), S(cy - 6), S(px - 4), S(cy + 6), S(px + 6), S(cy)};
                aa_tri(pn, P, hexc(hr ? S_ON_OR : S_TEXT), 1);
                hit(a, ry, b - 70, ry + 52, K_HPLAY, 2, id);
                /* its switch */
                box(pn, b - 58, cy - 12, b - 16, cy + 12, 12, on ? S_OR : (hs ? S_EDGE_HOT : S_EDGE), 0, 0);
                aa_disc(pn, S(on ? b - 28 : b - 46), S(cy), S(9), hexc(on ? S_ON_OR : S_LABEL), 1);
                hit(b - 66, ry + 6, b - 10, ry + 46, K_HTOG, 2, id);
            }
        }
        y += h + 12;
    }
    { /* hands on: what the mouse does to it */
        static const char *IC[5] = {"pointer", "hand", "mouse", "move", "eye"};
        static const wchar_t *VERB[5] = {L"Click", L"Hold", L"Scroll", L"Drag or swipe", L"Rest on it"};
        static const wchar_t *WHAT[5] = {L"drops a star in from that side", L"pulls a stream of gas off the pointer", L"spins its gas; keep going and it speeds up", L"turns it over", L"for 2 s, untouched: it wakes"};
        int cols = b - a >= 560 ? 2 : 1, rows = (5 + cols - 1) / cols;
        float h = 30 + rows * 32 + 8, cw = (b - a - 16) / cols;
        if (draw) {
            box(pn, a, y, b, y + h, 12, S_TILE, S_RULE, 1);
            lu_icon(pn, "hand", a + 19, y + 17, 13, S_OR);
            tx(a + 32, y + 8, b - 12, y + 26, L"HANDS ON", g_sf_cap, S_TEXT, DT_LEFT);
            for (int i = 0; i < 5; i++) {
                float cx = a + 8 + (i % cols) * cw, cy = y + 30 + (i / cols) * 32 + 16;
                lu_icon(pn, IC[i], cx + 14, cy, 15, S_LABEL);
                float vw = tw_of(VERB[i], g_sf_label);
                tx(cx + 32, cy - 10, cx + 32 + vw + 2, cy + 10, VERB[i], g_sf_label, S_TEXT, DT_LEFT);
                tx(cx + 40 + vw, cy - 10, cx + cw - 8, cy + 10, WHAT[i], g_sf_small, S_MUTED, DT_LEFT | DT_END_ELLIPSIS);
            }
        }
        y += h + 16;
    }
    y += mot_tiles(pn, 0, a, draw ? y : -10000, b); /* the routines Asleep may pick from */
    if (draw) { g_mzone[2][0] = g_mzone[2][1] = SR(0, 0, 0, 0); } /* (no tiles of the black hole's to drag here) */
    return y - y0;
}
static float motions_page(Pane *pn, float x0, float y0, float x1) {
    float cw = x1 - x0, lw = cw >= 720 ? 250 : cw, ex0 = cw >= 720 ? x0 + lw + 12 : x0;
    /* the list: each state with the icons of its routines and how many actions it has */
    float lh = 8 + 2 * 26 + ST_N * 36 + 8, y = y0;
    box(pn, x0, y, x0 + lw, y + lh, 12, S_CARD, 0, 0);
    y += 8;
    for (int i = 0; i < ST_N; i++) {
        if (i == 0 || i == 6) { tx(x0 + 16, y + 6, x0 + lw - 16, y + 22, i ? L"PANEL" : L"CLAUDE CODE", g_sf_cap, S_MUTED, DT_LEFT); y += 26; }
        int st = MOT_STATES[i], on = st == g_pv_st, hot = is_hot(K_MSTATE, 0, st);
        float a = x0 + 8, b = y, c = x0 + lw - 8;
        box(pn, a, b, c, b + 34, 9, hot ? S_CHIP_HOT : S_CARD, S_OR, on ? 1.5f : 0);
        aa_disc(pn, S(a + 14), S(b + 17), S(4), LOOKS[st].halo, 1);
        tx(a + 26, b, c - 100, b + 34, MOT_STATE_NAMES[i], g_sf_text, on ? S_TEXT : S_CHIP_TXT, DT_LEFT | DT_END_ELLIPSIS);
        /* its routines' icons (up to four), then +n, then the actions' count */
        uint32_t rt = MOTION_MAP[st].rt & ~1u;
        int na = st == ST_SLEEP && (MOTION_MAP[st].rt >> RT_HOLE & 1) ? 0 : bits(MOTION_MAP[st].act & ~1u), shown = 0, more = 0; /* (a black hole plays none of them) */
        float ix = c - 10;
        wchar_t num[12];
        if (na) {
            swprintf(num, 12, L"%d", na);
            float w = tw_of(num, g_sf_small);
            tx(ix - w - 2, b, ix, b + 34, num, g_sf_small, S_MUTED, DT_RIGHT);
            lu_icon(pn, "sparkles", ix - w - 10, b + 17, 11, S_MUTED);
            ix -= w + 20;
        }
        int ids[4];
        for (int id = 1; id < RT_N; id++) if ((rt >> id) & 1) { if (shown < 4) ids[shown++] = id; else more++; }
        if (more) { swprintf(num, 12, L"+%d", more); tx(ix - 24, b, ix, b + 34, num, g_sf_small, S_MUTED, DT_RIGHT); ix -= 28; }
        for (int k = shown - 1; k >= 0; k--) lu_icon(pn, MOT_RT_ICON[ids[k]], ix - 8 - (shown - 1 - k) * 18, b + 17, 14, on ? S_OR : S_MUTED);
        hit(a, b, c, b + 34, K_MSTATE, 0, st);
        y += 36;
    }
    /* the editor */
    float ey = cw >= 720 ? y0 : y0 + lh + 12;
    int st = g_pv_st;
    float iy = ey + 16 + 30 + 10, ty = iy + 44 + 16;
    int hole = (st == ST_SLEEP && (MOTION_MAP[st].rt >> RT_HOLE & 1)) || (st == ST_READY && (MOTION_MAP[st].rt >> RT_RESTLESS & 1)); /* asleep as a black hole, or READY: their own first */
    int no_acts = hole && st == ST_SLEEP; /* (a black hole plays only its own: the shell's actions never run on it, so they are not offered) */
    int sleepy = hole && st == ST_SLEEP; /* (Asleep as a black hole: its own page) */
    float th = sleepy ? sleep_body(pn, ex0 + 16, ty, x1 - 16, 0) : mot_tiles(pn, 0, ex0 + 16, -10000, x1 - 16) + (no_acts ? 0 : 16 + mot_tiles(pn, 1, ex0 + 16, -10000, x1 - 16)) + (hole ? 16 + 24 + mot_tiles(pn, 2, ex0 + 28, -10000, x1 - 28) : 0); /* (measured off the page) */
    float eh = (ty - ey) + th + 16;
    box(pn, ex0, ey, x1, ey + eh, 12, S_CARD, 0, 0);
    aa_disc(pn, S(ex0 + 22), S(ey + 31), S(5), LOOKS[st].halo, 1);
    tx(ex0 + 34, ey + 16, x1 - 180, ey + 46, state_wname(st), g_sf_card, S_TEXT, DT_LEFT);
    if (sleepy) { /* (marked: it is a black hole) */
        float nw = tw_of(state_wname(st), g_sf_card), bx = ex0 + 34 + nw + 10;
        box(pn, bx, ey + 21, bx + 92, ey + 41, 10, S_CHIP, S_OR, 1);
        lu_icon(pn, "hole", bx + 13, ey + 31, 11, S_OR);
        tx(bx + 22, ey + 21, bx + 88, ey + 41, L"black hole", g_sf_small, S_OR, DT_LEFT);
    }
    int htr = is_hot(K_MTRY, 0, 0), hrs = is_hot(K_MRESET, 0, 0);
    box(pn, x1 - 152, ey + 18, x1 - 80, ey + 44, 13, htr ? S_CHIP_HOT : S_CHIP, 0, 0);
    float P[6] = {S(x1 - 138), S(ey + 26), S(x1 - 138), S(ey + 36), S(x1 - 130), S(ey + 31)};
    aa_tri(pn, P, hexc(S_TEXT), 1);
    tx(x1 - 126, ey + 18, x1 - 84, ey + 44, L"Try it", g_sf_label, S_TEXT, DT_LEFT);
    hit(x1 - 152, ey + 18, x1 - 80, ey + 44, K_MTRY, 0, 0);
    tx(x1 - 74, ey + 18, x1 - 16, ey + 44, L"Reset", g_sf_label, hrs ? S_TEXT : S_MUTED, DT_CENTER);
    hit(x1 - 74, ey + 18, x1 - 16, ey + 44, K_MRESET, 0, 0);
    /* the info line: what the tile under the pointer does */
    box(pn, ex0 + 16, iy, x1 - 16, iy + 44, 9, S_TILE, 0, 0);
    char line[400];
    if (g_hot >= 0 && (g_hits[g_hot].kind == K_MTOG || g_hits[g_hot].kind == K_HPLAY || g_hits[g_hot].kind == K_HTOG)) {
        int row = g_hits[g_hot].row, id = g_hits[g_hot].val;
        lu_icon(pn, mot_icon(row, id), ex0 + 36, iy + 22, 18, S_OR);
        snprintf(line, sizeof line, "%s: %s", mot_name(row, id), mot_about(row, id));
        txa(ex0 + 54, iy, x1 - 26, iy + 44, line, g_sf_small, S_LABEL, DT_LEFT | DT_END_ELLIPSIS);
    } else if (wall() < g_mot_msg_until) tx(ex0 + 28, iy, x1 - 26, iy + 44, L"A state needs at least one routine: switch another on first.", g_sf_small, 0xFFB877, DT_LEFT);
    else {
        wchar_t t[200];
        if (sleepy) swprintf(t, 200, L"Click an action to watch it above; its switch says whether it plays on its own.");
        else swprintf(t, 200, L"Click a motion to try it above; drag it up to use it in %ls, down to stop.", state_wname(st));
        tx(ex0 + 28, iy, x1 - 26, iy + 44, t, g_sf_small, S_MUTED, DT_LEFT | DT_END_ELLIPSIS);
    }
    if (sleepy) { sleep_body(pn, ex0 + 16, ty, x1 - 16, 1); return fmaxf(cw >= 720 ? lh : lh + 12 + eh, cw >= 720 ? eh : 0); }
    float hh = 0;
    if (hole) { /* the black hole's own actions, in a frame of their own above the state's */
        float hin = mot_tiles(pn, 2, ex0 + 28, -10000, x1 - 28);
        box(pn, ex0 + 16, ty, x1 - 16, ty + hin + 24, 12, S_TILE, S_RULE, 1);
        mot_tiles(pn, 2, ex0 + 28, ty + 12, x1 - 28);
        hh = hin + 24 + 16;
    }
    float h0 = mot_tiles(pn, 0, ex0 + 16, ty + hh, x1 - 16);
    if (!no_acts) mot_tiles(pn, 1, ex0 + 16, ty + hh + h0 + 16, x1 - 16);
    return fmaxf(cw >= 720 ? lh : lh + 12 + eh, cw >= 720 ? eh : 0);
}

/* the still layer: everything but the moving cluster */
static void set_build(void) {
    set_fonts();
    chat_fonts();
    Pane *pn = &g_sl;
    int W = SW_W(), H = SW_H();
    float WL = SL_W(), HL = SL_H(), SD = side_w(), cx0 = SD + 16, cx1 = WL - 16;
    pane_size(pn, W, H);
    memset(pn->px, 0, (size_t)W * H * 4);
    g_hits_n = 0; g_tx_n = 0; g_ovr_n = 0;
    pane_size(&g_om, pn->W, pn->H);
    memset(g_om.px, 0, (size_t)pn->W * pn->H * 4);
    HDC mdc = pn->mem;
    box(pn, 0, 0, WL, HL, 14, S_BG, 0x2A2A30, 1);
    /* the page first (it scrolls under the preview), then everything round it */
    float pvh = clampf(HL * 0.4f, 180, 420), bandT = SET_TOP, bandB = bandT + pvh, ct = bandB + 14, cb = HL - 14;
    g_set_view_h = cb - ct;
    g_set_scroll = clampf(g_set_scroll, 0, fmaxf(0, g_set_content_h - g_set_view_h));
    g_set_scroll_to = clampf(g_set_scroll_to, 0, fmaxf(0, g_set_content_h - g_set_view_h));
    g_clip = SR(cx0 - 4, ct, cx1 + 4, cb); g_clipping = 1;
    float top = ct - g_set_scroll;
    g_set_content_h = g_tab == PG_MOTIONS ? motions_page(pn, cx0, top, cx1) : rows_page(pn, g_tab, cx0, top, cx1);
    g_clipping = 0;
    box(pn, SD, 1, WL - 1, ct, 0, S_BG, 0, 0);      /* what scrolled up under the title */
    box(pn, SD, cb, WL - 1, HL - 1, 0, S_BG, 0, 0); /* and down past the bottom */
    frame(pn, 0, 0, WL, HL, 14, 0x2A2A30, 1);         /* the window's edge again */
    /* the preview band */
    box(pn, cx0, bandT, cx1, bandB, 14, S_BAND, 0, 0);
    g_pv_rc = SR(cx0 + 4, bandT + 4, cx1 - 4, bandB - 4);
    if (g_tab == PG_REPLIES) { /* a reply in the chat box's font, top right */
        TEXTMETRICW tm;
        SelectObject(mdc, g_chat_tf); GetTextMetricsW(mdc, &tm);
        float th = tm.tmHeight / (float)g_sdpi;
        SelectObject(mdc, g_chat_bf); GetTextMetricsW(mdc, &tm);
        float bh = tm.tmHeight / (float)g_sdpi, sh = fminf(112, 22 + th + 6 + bh * 2), bx1 = cx1 - 14, bx0 = fmaxf(cx0 + 160, bx1 - 300);
        float t0 = bandT + 12;
        box(pn, bx0, t0, bx1, t0 + sh, 8, chat_bgx(), 0, 0);
        box(pn, bx0, t0, bx0 + 3, t0 + sh, 1.5f, S_OR, 0, 0);
        tx(bx0 + 14, t0 + 10, bx1 - 8, t0 + 10 + th, L"vigil \x00B7 Done", g_chat_tf, inkx(0xFFB877), DT_LEFT | DT_END_ELLIPSIS);
        tx(bx0 + 14, t0 + 10 + th + 6, bx1 - 8, t0 + sh - 8, L"The satellites slide round the orbit and trail behind.", g_chat_bf, text_bodyx(), DT_WORDBREAK | DT_END_ELLIPSIS);
        ovr_add(bx0, t0, bx1, t0 + sh, 8);
    }
    /* the top bar: the page's name and hint, its reset, close (the bar drags the window) */
    int ch = is_hot(K_CLOSE, 0, 0);
    box(pn, cx1 - 32, 10, cx1, 42, 8, ch ? S_CLOSE_HOT : S_CHIP, 0, 0);
    aa_seg(pn, S(cx1 - 21), S(21), S(cx1 - 11), S(31), S(1.6f), hexc(ch ? 0xFFFFFF : S_LABEL), 1);
    aa_seg(pn, S(cx1 - 11), S(21), S(cx1 - 21), S(31), S(1.6f), hexc(ch ? 0xFFFFFF : S_LABEL), 1);
    hit(cx1 - 32, 10, cx1, 42, K_CLOSE, 0, 0);
    tx(cx0 + 2, 8, cx0 + 260, 44, PG_NAME[g_tab], g_sf_card, S_TEXT, DT_LEFT);
    tx(cx0 + 4 + tw_of(PG_NAME[g_tab], g_sf_card) + 10, 8, cx1 - 110, 44, PG_HINT[g_tab], g_sf_small, S_MUTED, DT_LEFT | DT_END_ELLIPSIS);
    int hr = is_hot(K_RESET, 1, 0);
    if (hr) box(pn, cx1 - 96, 13, cx1 - 42, 39, 13, S_CHIP, 0, 0);
    tx(cx1 - 96, 13, cx1 - 42, 39, L"Reset", g_sf_label, hr ? S_TEXT : S_MUTED, DT_CENTER);
    hit(cx1 - 96, 13, cx1 - 42, 39, K_RESET, 1, 0);
    /* bottom of the band: the state (not on Motions, whose list picks it) and the zoom */
    float by = bandB - 40;
    if (g_tab != PG_MOTIONS) {
        float a = cx0 + 10;
        for (int i = 0; i < 6; i++) {
            float w = tw_of(PV_NAMES[i], g_sf_label) + 34;
            if (a + w > cx1 - 174) break; /* (a narrow window shows fewer) */
            int on = PV_STATES[i] == g_pv_st, hot = is_hot(K_STATE, 0, i);
            box(pn, a, by, a + w, by + 28, 14, hot ? S_CHIP_HOT : S_CHIP, S_OR, on ? 1.5f : 0);
            aa_disc(pn, S(a + 13), S(by + 14), S(3.5f), LOOKS[PV_STATES[i]].halo, 1);
            tx(a + 22, by, a + w - 6, by + 28, PV_NAMES[i], g_sf_label, on ? S_TEXT : S_CHIP_TXT, DT_LEFT);
            hit(a, by, a + w, by + 28, K_STATE, 0, i);
            ovr_add(a, by, a + w, by + 28, 14);
            a += w + 4;
        }
    }
    float zx = cx1 - 124, lx = zx - 34; /* the background chip: the preview on dark (moon) or light (sun) */
    box(pn, lx, by, lx + 28, by + 28, 14, is_hot(K_PVBG, 0, 0) ? S_CHIP_HOT : S_CHIP, 0, 0);
    lu_icon(pn, g_pv_light ? "sunDim" : "moon", lx + 14, by + 14, 13, S_TEXT);
    hit(lx, by, lx + 28, by + 28, K_PVBG, 0, 0);
    ovr_add(lx, by, lx + 28, by + 28, 14);
    box(pn, zx, by, cx1 - 10, by + 28, 14, S_CHIP, 0, 0);
    for (int i = 0; i < 3; i++) {
        float a = zx + (i == 0 ? 2 : i == 1 ? 30 : 86), b = a + (i == 1 ? 56 : 26);
        if (is_hot(K_ZOOM, 0, i)) box(pn, a, by + 2, b, by + 26, 12, S_CHIP_HOT, 0, 0);
        hit(a, by, b, by + 28, K_ZOOM, 0, i);
    }
    lu_icon(pn, "minus", zx + 15, by + 14, 12, S_TEXT);
    lu_icon(pn, "plus", zx + 99, by + 14, 12, S_TEXT);
    wchar_t zl[12];
    swprintf(zl, 12, L"%d%%", (int)lroundf(g_pv_zoom * 100));
    tx(zx + 30, by, zx + 86, by + 28, zl, g_sf_label, S_TEXT, DT_CENTER);
    ovr_add(zx, by, cx1 - 10, by + 28, 14);
    /* the sidebar */
    box(pn, 1, 1, SD, HL - 1, 13, S_SIDE, 0, 0);
    box(pn, SD - 14, 1, SD, HL - 1, 0, S_SIDE, 0, 0);
    box(pn, SD, 1, SD + 1, HL - 1, 0, 0x26262C, 0, 0);
    int hs = is_hot(K_SIDE, 0, 0);
    if (hs) box(pn, 11, 14, 47, 50, 8, S_CHIP, 0, 0);
    lu_icon(pn, g_set_side ? "panelLeft" : "panelRight", 29, 32, 18, hs ? S_TEXT : S_LABEL);
    hit(11, 14, 47, 50, K_SIDE, 0, 0);
    if (g_set_side) tx(56, 14, SD - 10, 50, L"Settings", g_sf_title, S_TEXT, DT_LEFT);
    for (int i = 0; i < PG_N; i++) {
        float a = 10, b = 64 + i * 42, c = SD - 10;
        int on = g_tab == i, hot = is_hot(K_PAGE, 0, i);
        box(pn, a, b, c, b + 38, 9, hot && !on ? S_CHIP : S_SIDE, S_OR, on ? 1.5f : 0);
        lu_icon(pn, PG_ICON[i], a + 19, b + 19, 17, on ? S_TEXT : hot ? S_TEXT : S_CHIP_TXT);
        if (g_set_side) tx(a + 40, b, c - 6, b + 38, PG_NAME[i], g_sf_text, on || hot ? S_TEXT : S_CHIP_TXT, DT_LEFT);
        hit(a, b, c, b + 38, K_PAGE, 0, i);
    }
    int can = settings_saved(), hra = is_hot(K_RESET, 0, 0);
    lu_icon(pn, "rotateCcw", 29, HL - 34, 15, can ? (hra ? S_TEXT : S_MUTED) : 0x44444C);
    if (g_set_side) tx(48, HL - 48, SD - 10, HL - 20, L"Reset everything", g_sf_label, can ? (hra ? S_TEXT : S_MUTED) : 0x55555E, DT_LEFT);
    if (can) hit(10, HL - 50, SD - 10, HL - 18, K_RESET, 0, 0);
    if (g_md_row >= 0 && g_md_moved) { /* a motion tile being carried: under the pointer, over everything */
        float gx = g_md_at.x / (float)g_sdpi, gy = g_md_at.y / (float)g_sdpi, row = (float)g_md_row;
        box(pn, gx - 46, gy - 29, gx + 46, gy + 29, 10, S_CHIP_HOT, S_OR, 1.5f);
        lu_icon(pn, mot_icon(g_md_row, g_md_id), gx, gy - 9, 19, S_OR);
        txa(gx - 42, gy + 3, gx + 42, gy + 25, mot_name(g_md_row, g_md_id), g_sf_small, S_TEXT, DT_CENTER | DT_END_ELLIPSIS);
        (void)row;
    }
    /* the size grip, and the edges that resize */
    for (int k = 0; k < 3; k++) aa_seg(pn, S(WL - 4 - 4 * k), S(HL - 4), S(WL - 4), S(HL - 4 - 4 * k), S(1.1f), hexc(0x55555E), 1);
    hit(WL - 7, 14, WL, HL - 18, K_SIZE, 0, 1);
    hit(SD + 4, HL - 7, WL - 18, HL, K_SIZE, 0, 2);
    hit(WL - 18, HL - 18, WL, HL, K_SIZE, 0, 3);
    /* the text, then everything inside the edge made opaque again (GDI leaves its alpha at 0) */
    GdiFlush();
    SetBkMode(mdc, TRANSPARENT);
    for (int i = 0; i < g_tx_n; i++) {
        TextOp *o = &g_tx[i];
        SelectObject(mdc, o->f);
        SetTextColor(mdc, o->c);
        UINT f = o->how & DT_WORDBREAK ? o->how | DT_NOPREFIX : o->how | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX;
        int saved = o->clip ? SaveDC(mdc) : 0;
        if (o->clip) IntersectClipRect(mdc, g_clip.left, g_clip.top, g_clip.right, g_clip.bottom);
        DrawTextW(mdc, o->t, -1, &o->r, f);
        if (saved) RestoreDC(mdc, saved);
    }
    GdiFlush();
    int rad = (int)ceilf(S(14)) + 1;
    float rr = S(14);
    for (int y = 0; y < H; y++) {
        int corner = y < rad || y >= H - rad;
        uint32_t *p = pn->px + (size_t)y * W;
        for (int x = corner ? rad : 0; x < (corner ? W - rad : W); x++) p[x] |= 0xff000000u;
        if (!corner) continue;
        for (int x = 0; x < W; x++) { /* in the corner squares: outside the rounded edge, nothing */
            if (x >= rad && x < W - rad) continue;
            float qx = x + 0.5f < rr ? rr - (x + 0.5f) : x + 0.5f > W - rr ? x + 0.5f - (W - rr) : 0, qy = y + 0.5f < rr ? rr - (y + 0.5f) : y + 0.5f > H - rr ? y + 0.5f - (H - rr) : 0;
            if (hypotf(qx, qy) > rr + 0.5f) p[x] = 0;
        }
    }
    g_sl_dirty = 0;
}

/* each tick while it is open: the preview moves, the window is shown again */
static void settings_tick(double now) {
    if (!set_open()) return;
    static double last_frame, last_pics;
    if (g_sizing && now - last_frame < 1.0 / 24) return; /* being resized: fewer, cheaper frames (it catches up when let go) */
    last_frame = now;
    int pics_now = !g_sizing || now - last_pics > 1.0 / 8 || !g_pv_pic[0].img;
    if (pics_now) last_pics = now;
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
    /* resting on a motion tile: it plays after a moment, once */
    if (g_krec >= 0 || (g_kmsg_until > 0 && now > g_kmsg_until)) { if (now > g_kmsg_until) g_kmsg_until = 0; g_sl_dirty = 1; }
    if (fabsf(g_set_scroll_to - g_set_scroll) > 0.3f) { /* the page glides to where the wheel sent it (quick, no overshoot) */
        g_set_scroll += (g_set_scroll_to - g_set_scroll) * (1 - expf(-dt * 16));
        if (fabsf(g_set_scroll_to - g_set_scroll) <= 0.3f) g_set_scroll = g_set_scroll_to;
        g_sl_dirty = 1;
    }
    if (g_sl_dirty) set_build();
    /* the cluster in the preview, scaled as Size scales it on the screen */
    int pw = g_pv_rc.right - g_pv_rc.left, ph = g_pv_rc.bottom - g_pv_rc.top;
    float cx = pw / 2.0f, cy = ph / 2.0f - (float)S(14), cw = fminf((float)pw, (float)ph) * 0.97f * (0.55f + 0.45f * (g_scale - 0.75f) / 0.85f) * g_pv_zoom;
    /* the same proportions as draw_single, on a cluster cw wide instead of g_cw */
    float big = cw * 0.6f, rx = cw / 2.0f - (float)(8 * g_dpi) * cw / g_cw, ry = rx * 1.15f;
    float small = g_others ? fminf(cw * 0.2f, (float)(2 * M_PI * rx / 3 * 0.75)) : 0;
    rx -= small / 2; ry -= small / 2;
    OrbitK ok = orbit_k(rx, small);
    Session *m = &g_pv_room.s[0];
    if (g_pv_mx == 0 && g_pv_my == 0) { g_pv_mx = cx; g_pv_my = cy; }
    g_pv_big = big;
    if (g_pv_turn) { /* pressed on it: held still, or a drag that turns it under the hand */
        POINT q; GetCursorPos(&q); ScreenToClient(g_set, &q);
        if (m->pressing && abs(q.x - g_pv_down.x) + abs(q.y - g_pv_down.y) > (int)(4 * g_sdpi)) { m->pressing = 0; m->held = 1; }
        if (m->held) {
            float ddx = (float)(q.x - g_pv_turn_at.x) / (float)g_sdpi, ddy = (float)(q.y - g_pv_turn_at.y) / (float)g_sdpi, tdt = (float)(now - g_pv_turn_t);
            if (ddx != 0 || ddy != 0) {
                m->ay += ddx * 0.012f; m->ax += ddy * 0.012f;
                if (tdt > 0.001f) { g_pv_tvx = ddy * 0.012f / tdt; g_pv_tvy = ddx * 0.012f / tdt; }
                g_pv_turn_at = q; g_pv_turn_t = now;
            }
        }
    }
    if (g_pv_drag) {
        POINT p; GetCursorPos(&p); ScreenToClient(g_set, &p);
        float nx = p.x - g_pv_rc.left - g_pv_ox, ny = p.y - g_pv_rc.top - g_pv_oy;
        if (dt > 0) { g_pv_vx += ((nx - g_pv_mx) / dt - g_pv_vx) * 0.45f; g_pv_vy += ((ny - g_pv_my) / dt - g_pv_vy) * 0.45f; }
        g_pv_mx = nx; g_pv_my = ny;
    } else if (g_tab == PG_MOTIONS) { /* Motions: it stays where it was put (inside the preview), to watch a motion from
                                        * there or, in READY, to throw from there; a double-click puts it back in the middle */
        float mx = fminf(big * 0.45f, pw * 0.5f), my = fminf(big * 0.45f, ph * 0.5f);
        g_pv_mx = clampf(g_pv_mx, mx, pw - mx); g_pv_my = clampf(g_pv_my, my, ph - my);
        g_pv_vx *= 0.8f; g_pv_vy *= 0.8f;
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
    for (int i = 0; i < 4; i++) g_pv_room.s[i].backdrop = g_backdrop == 1 || (g_backdrop == 2 && g_pv_light); /* (Auto: as it would over that background) */
    m->aim_on = g_pv_st == ST_READY && !g_pv_drag; /* READY in the preview: it throws at the pointer over the preview, else at a point going round it */
    if (m->aim_on) {
        POINT p; GetCursorPos(&p); ScreenToClient(g_set, &p);
        float u = big * SHELL_FRAC, ax = (p.x - g_pv_rc.left - g_pv_mx) / u, ay = (p.y - g_pv_rc.top - g_pv_my) / u;
        float reach = fminf((g_pv_rc.right - g_pv_rc.left) * .42f, (g_pv_rc.bottom - g_pv_rc.top) * .4f) / u; /* the ghost: well out, inside the preview */
        float hit = 14 * (float)g_sdpi / u;
        if (!PtInRect(&g_pv_rc, p) || hypotf(ax, ay) < 1.05f) { ax = reach * cosf((float)now * .5f); ay = reach * .8f * sinf((float)now * .5f); }
        else { RECT cb; cursor_box(&cb); ax += (cb.left + cb.right) * .5f / u; ay += (cb.top + cb.bottom) * .5f / u; hit = (.5f * hypotf((float)(cb.right - cb.left), (float)(cb.bottom - cb.top)) + 7 * (float)g_sdpi) / u; } /* the whole mouse, as on the desktop */
        m->aim_x = ax; m->aim_y = ay; m->aim_hit = hit;
    }
    room_tick(&g_pv_room, &g_shape, now, dt);
    float pfd = hole_drag(m, now);
    if (!g_pv_drag) g_pv_orbit += dt * (1 + 9 * pfd);
    rx *= 1 - .5f * pfd; ry *= 1 - .62f * pfd;
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
        } else {
            float d = bead_step(ob, g_pv_mx, g_pv_my + bob, tx, ty, a, rx, ry, small, g_pv_drag ? 0 : ok.KT, &ok, ring, rn, dt);
            if (pfd > 0.02f) { float f = fminf(1, pfd * 3); ob->px += (tx - ob->px) * f; ob->py += (ty - ob->py) * f; ob->vx *= 1 - f; ob->vy *= 1 - f; }
            else if (d > ok.BREAK) ob->free = 1, ob->rest = 0;
        }
    }
    /* the cluster's picture: the band's colour, the small cores, the main one and its frame on top */
    pane_size(&g_pv, pw, ph);
    uint32_t bg = 0xff000000u | (g_pv_light ? PV_LIGHT : S_BAND);
    for (size_t j = 0; j < (size_t)pw * ph; j++) g_pv.px[j] = bg;
    g_blit_add = !g_pv_light; /* (on the light background the pictures are laid over it, as Windows does on the desktop) */
    for (int i = 1; i < 4 && g_others; i++) {
        int w4 = ((int)g_pv_ob[i].w / 4) * 4;
        if (w4 < 8) continue;
        if (pics_now) pic_make(&g_pv_pic[i], &g_pv_room.s[i], w4, now, 1);
        else if (g_pv_pic[i].img) w4 = g_pv_pic[i].px; else continue; /* (the last picture, at its size) */
        blit_pic_at(&g_pv, g_pv_ob[i].px - w4 / 2.0f, g_pv_ob[i].py - w4 / 2.0f, &g_pv_pic[i]);
    }
    int w4 = ((int)big / 4) * 4;
    m->pad = hole_pad(m);
    if (m->pad > 1) w4 = ((int)(w4 * m->pad) / 4) * 4; /* a black hole: room round it (the core the same size) */
    if (pics_now) { RENDER_PART = 1; pic_make(&g_pv_pic[0], m, w4, now, 3); RENDER_PART = 0; } /* the one you watch: every frame (READY's pieces: below) */
    else w4 = g_pv_pic[0].px;
    float fx = g_pv_mx - w4 / 2.0f, fy = g_pv_my - w4 / 2.0f;
    blit_pic_at(&g_pv, fx, fy, &g_pv_pic[0]);
    if (m->mo.loose_on) { /* READY: its pieces and sparks anywhere on the preview, as the layer does on the desktop */
        static Canvas cv;
        static uint8_t *rgba;
        static size_t cap;
        canvas_resize(&cv, pw, ph);
        RENDER_PART = 2;
        render_part(&cv, m, &g_shape, now, g_glow, g_core, (int)lroundf(fx), (int)lroundf(fy), (int)lroundf(fx) + w4, (int)lroundf(fy) + w4);
        RENDER_PART = 0;
        if ((size_t)pw * ph * 4 > cap) { free(rgba); cap = (size_t)pw * ph * 4; rgba = malloc(cap); }
        canvas_rgba(&cv, rgba);
        for (size_t j = 0; j < (size_t)pw * ph; j++) { /* light added onto the band, over the pieces' dark rim */
            const uint8_t *q = rgba + j * 4;
            if (!q[3]) continue;
            uint32_t d = g_pv.px[j];
            if (g_pv_light) { /* laid over, as on the desktop */
                unsigned k = 255 - q[3], b = (d & 255) * k / 255 + q[2], g = (d >> 8 & 255) * k / 255 + q[1], r = (d >> 16 & 255) * k / 255 + q[0];
                g_pv.px[j] = 0xff000000u | (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
                continue;
            }
            unsigned mx = q[0] > q[1] ? (q[0] > q[2] ? q[0] : q[2]) : (q[1] > q[2] ? q[1] : q[2]), keep = 255 - (q[3] - mx); /* (the shade: alpha above the light) */
            unsigned b = (d & 255) * keep / 255 + q[2], g = (d >> 8 & 255) * keep / 255 + q[1], r = (d >> 16 & 255) * keep / 255 + q[0];
            g_pv.px[j] = 0xff000000u | (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
        }
        { const char *dump = getenv("VIGIL_PV_DUMP"); /* TEST: the pieces alone over a light desktop */
          if (dump) { FILE *f = fopen(dump, "wb"); if (f) { fprintf(f, "P6 %d %d 255\n", pw, ph);
            for (size_t j = 0; j < (size_t)pw * ph; j++) { const uint8_t *q = rgba + j * 4; for (int k = 0; k < 3; k++) { unsigned v = 0xEE * (255 - q[3]) / 255 + q[k]; fputc(v > 255 ? 255 : v, f); } }
            fclose(f); } } }
    }
    g_blit_add = 0;
    RECT lr = {(int)fx, (int)fy, (int)fx + w4, (int)fy + w4};
    float real = (float)(((int)(g_cw * 0.6f) / 4) * 4); /* the main core's side on the screen */
    g_frame_k = frame_k(real) * w4 / fmaxf(1, real); /* its frame, as much smaller as the preview is */
    focus_frame(&g_pv, lr, m, now);
    g_frame_k = 0;
    /* the window: the still layer with the cluster laid in, its own controls back on top */
    int W = g_sl.W, H = g_sl.H;
    pane_size(&g_so, W, H);
    memcpy(g_so.px, g_sl.px, (size_t)W * H * 4);
    for (int y = 0; y < ph; y++) {
        int Y = g_pv_rc.top + y;
        if (Y < 0 || Y >= H) continue;
        uint32_t *d = g_so.px + (size_t)Y * W + g_pv_rc.left, *s = g_pv.px + (size_t)y * pw;
        for (int x = 0; x < pw && g_pv_rc.left + x < W; x++) d[x] = s[x] | 0xff000000u;
    }
    for (int k = 0; k < g_ovr_n; k++) { /* the close button, the state chips, the zoom, the reply sample */
        RECT o = g_ovr[k];
        for (int y = o.top; y < o.bottom; y++) {
            if (y < 0 || y >= H) continue;
            for (int x = o.left; x < o.right; x++) {
                if (x < 0 || x >= W) continue;
                size_t i = (size_t)y * W + x;
                unsigned a = g_om.W == W && g_om.H == H ? g_om.px[i] >> 24 : 255;
                uint32_t sp = g_sl.px[i];
                if (!a) continue; /* outside it: the picture */
                if (a == 255) { g_so.px[i] = sp; continue; }
                /* its soft edge was drawn over the band's dark: put the picture there instead (the edge is the control's
                 * colour times a plus the band times 1 - a; the band's share becomes the picture's) */
                uint32_t pv = g_so.px[i], out = 0xff000000u;
                for (int sh = 0; sh < 24; sh += 8) {
                    int v = (int)(sp >> sh & 255) + (int)(((int)(pv >> sh & 255) - (int)(S_BAND >> sh & 255)) * (255 - (int)a) / 255);
                    out |= (uint32_t)(v < 0 ? 0 : v > 255 ? 255 : v) << sh;
                }
                g_so.px[i] = out;
            }
        }
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

static void krec_stop(void) { g_krec = -1; hotkeys_apply(); g_sl_dirty = 1; }
static void set_close(void) { if (g_set) ShowWindow(g_set, SW_HIDE); g_pv_drag = 0; g_slide = -1; g_sizing = 0; if (g_krec >= 0) krec_stop(); }

/* a key pressed while a shortcut is being recorded: it becomes that shortcut ("ctrl+alt+c") */
static void krec_key(WPARAM vk) {
    if (vk == VK_ESCAPE) { krec_stop(); return; }
    if (vk == VK_BACK || vk == VK_DELETE) { snprintf(hk_spec(g_krec), 32, "none"); settings_save(); krec_stop(); return; }
    if (vk == VK_CONTROL || vk == VK_MENU || vk == VK_SHIFT || vk == VK_LWIN || vk == VK_RWIN || vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_LMENU || vk == VK_RMENU || vk == VK_LSHIFT || vk == VK_RSHIFT) return;
    char key[8];
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) snprintf(key, sizeof key, "%c", (char)tolower((int)vk));
    else if (vk >= VK_F1 && vk <= VK_F12) snprintf(key, sizeof key, "f%d", (int)(vk - VK_F1 + 1));
    else if (vk == VK_SPACE) snprintf(key, sizeof key, "space");
    else { g_kmsg = L"Use a letter, a number, an F-key or Space"; g_kmsg_until = wall() + 2; g_sl_dirty = 1; return; }
    int ctrl = GetKeyState(VK_CONTROL) & 0x8000, alt = GetKeyState(VK_MENU) & 0x8000, shift = GetKeyState(VK_SHIFT) & 0x8000, win = (GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000;
    if (!ctrl && !alt && !win && !(shift && vk >= VK_F1 && vk <= VK_F12)) { g_kmsg = L"Hold Ctrl, Alt or Win with it"; g_kmsg_until = wall() + 2; g_sl_dirty = 1; return; }
    snprintf(hk_spec(g_krec), 32, "%s%s%s%s%s", ctrl ? "ctrl+" : "", alt ? "alt+" : "", shift ? "shift+" : "", win ? "win+" : "", key);
    settings_save();
    krec_stop();
}

static LRESULT CALLBACK set_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    switch (msg) {
    case WM_DPICHANGED: { /* dragged onto a monitor with another scale: drawn again at that scale */
        const RECT *r = (const RECT *)lp;
        g_sdpi = LOWORD(wp) / 96.0;
        set_refont();
        SetWindowPos(w, NULL, r->left, r->top, SW_W(), SW_H(), SWP_NOZORDER | SWP_NOACTIVATE);
        settings_tick(wall());
        return 0;
    }
    case WM_NCHITTEST: { /* the sidebar's empty space and the page's title row move the window */
        POINT c = p; ScreenToClient(w, &c);
        if (set_hit_at(c) >= 0 || PtInRect(&g_pv_rc, c)) return HTCLIENT;
        float x = c.x / (float)g_sdpi, y = c.y / (float)g_sdpi;
        if (x < side_w() || y < SET_TOP) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_MOUSEMOVE: {
        if (!g_tracking) { TRACKMOUSEEVENT te = {sizeof te, TME_LEAVE, w, 0}; TrackMouseEvent(&te); g_tracking = 1; }
        if (g_sizing) { /* the edge or corner follows the mouse */
            POINT s; GetCursorPos(&s);
            RECT wa = work_at((float)s.x, (float)s.y);
            if (g_sizing & 1) g_set_w = clampf(g_size_w0 + (s.x - g_size_from.x) / (float)g_sdpi, 620, (wa.right - wa.left) / (float)g_sdpi);
            if (g_sizing & 2) g_set_h = clampf(g_size_h0 + (s.y - g_size_from.y) / (float)g_sdpi, 480, (wa.bottom - wa.top) / (float)g_sdpi);
            g_sl_dirty = 1; /* (drawn by the next tick, not on every mouse move) */
            return 0;
        }
        if (g_slide >= 0) { int h = slide_hit(); if (h >= 0) { g_slide = h; set_slide_to(h, p.x); } return 0; }
        if (g_md_row >= 0) { /* a motion tile carried */
            if (!g_md_moved && abs(p.x - g_md_from.x) + abs(p.y - g_md_from.y) > 6) g_md_moved = 1;
            g_md_at = p;
            if (g_md_moved) g_sl_dirty = 1;
            return 0;
        }
        int h = set_hit_at(p);
        if (h != g_hot) {
            g_hot = h; g_sl_dirty = 1;
        }
        return 0;
    }
    case WM_MOUSELEAVE: g_tracking = 0; if (g_hot >= 0) { g_hot = -1; g_sl_dirty = 1; } return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT c; GetCursorPos(&c); ScreenToClient(w, &c);
            int h = set_hit_at(c);
            LPCTSTR cur = h >= 0 && g_hits[h].kind == K_SIZE ? (g_hits[h].val == 1 ? IDC_SIZEWE : g_hits[h].val == 2 ? IDC_SIZENS : IDC_SIZENWSE)
                        : h >= 0 || PtInRect(&g_pv_rc, c) ? IDC_HAND : IDC_ARROW;
            SetCursor(LoadCursor(NULL, cur));
            return TRUE;
        }
        break;
    case WM_LBUTTONDBLCLK: /* on the preview: zoom back to 1 and the core back in the middle; anywhere else a second press like the first */
        if (PtInRect(&g_pv_rc, p) && set_hit_at(p) < 0) { g_pv_zoom = 1; g_pv_mx = g_pv_my = 0; g_sl_dirty = 1; return 0; }
        /* fall through */
    case WM_LBUTTONDOWN: {
        int h = set_hit_at(p);
        if (g_krec >= 0 && (h < 0 || g_hits[h].kind != K_KREC)) krec_stop(); /* a click elsewhere: recording stops */
        if (h >= 0) {
            SetHit *sh = &g_hits[h];
            switch (sh->kind) {
            case K_PICK: set_choose(sh->row, sh->val); break;
            case K_TOGGLE: set_choose(sh->row, 0); break;
            case K_SLIDE: g_slide = h; g_slide_row = g_hits[h].row; SetCapture(w); set_slide_to(h, p.x); g_sl_dirty = 1; break;
            case K_STATE: g_pv_st = PV_STATES[sh->val]; g_sl_dirty = 1; break;
            case K_PAGE: g_tab = sh->val; g_set_scroll = g_set_scroll_to = 0; g_sl_dirty = 1; break;
            case K_SIDE: g_set_side = !g_set_side; settings_save(); g_sl_dirty = 1; break;
            case K_PVBG: g_pv_light = !g_pv_light; g_sl_dirty = 1; settings_applied(1); break;
            case K_ZOOM: g_pv_zoom = sh->val == 1 ? 1 : clampf(g_pv_zoom + (sh->val ? 0.25f : -0.25f), 1, 3); g_pv_mx = g_pv_my = 0; g_sl_dirty = 1; break;
            case K_MSTATE: g_pv_st = sh->val; g_sl_dirty = 1; break;
            case K_MTOG: g_md_row = sh->row; g_md_id = sh->val; g_md_moved = 0; g_md_from = g_md_at = p; SetCapture(w); break; /* a click or a drag: told apart on the move */
            case K_MTRY: { uint32_t rt = MOTION_MAP[g_pv_st].rt & ~1u; int n = bits(rt), k = n ? rand() % n : 0; for (int id = 1; id < RT_N; id++) if ((rt >> id) & 1 && k-- == 0) { mot_play(0, id); break; } break; }
            case K_MRESET: { MotionMap def[ST_N]; motion_map_default(def); MOTION_MAP[g_pv_st] = def[g_pv_st]; if (g_pv_st == ST_SLEEP) { HOLE_ACTS = HOLE_ACTS_DEF; HOLE_TILT = 2.5f; } if (g_pv_st == ST_READY) READY_ATKS = (1u << RA_N) - 2; settings_applied(1); break; }
            case K_HPLAY: mot_play(2, sh->val); break;
            case K_HTOG: mot_toggle(2, sh->val); break;
            case K_KREC: if (g_krec == sh->val) krec_stop(); else { g_krec = sh->val; for (int i = 1; i <= 4; i++) UnregisterHotKey(g_cluster.w, i); g_sl_dirty = 1; } break; /* (no hotkey steals the keys meanwhile) */
            case K_RESET: if (sh->row == 1) page_reset(g_tab); else set_reset(); break;
            case K_SIZE: g_sizing = sh->val; GetCursorPos(&g_size_from); g_size_w0 = g_set_w; g_size_h0 = g_set_h; SetCapture(w); break;
            case K_CLOSE: g_close_armed = 1; SetCapture(w); break; /* it closes when the button is let go over it */
            }
            return 0;
        }
        if (PtInRect(&g_pv_rc, p)) { /* as on the desktop: the move key (Alt) picks the core up and carries it (pressed off it:
                                       * it jumps under the pointer); a plain press on it is a press, a drag turns it */
            Session *pm = &g_pv_room.s[0];
            float dx = p.x - g_pv_rc.left - g_pv_mx, dy = p.y - g_pv_rc.top - g_pv_my, u = g_pv_big * SHELL_FRAC;
            if (alt_held()) {
                g_pv_ox = dx; g_pv_oy = dy;
                if (hypotf(g_pv_ox, g_pv_oy) > (g_pv_rc.bottom - g_pv_rc.top) * 0.25f) g_pv_ox = g_pv_oy = 0;
                g_pv_drag = 1;
                SetCapture(w);
            } else if (hypotf(dx, dy) < u * (hole_pad(pm) > 1 ? 1.6f : 1.15f)) {
                g_pv_turn = 1; g_pv_down = g_pv_turn_at = p; g_pv_turn_t = wall(); g_pv_tvx = g_pv_tvy = 0;
                pm->press_at = wall(); pm->press_x = dx / u; pm->press_y = dy / u; pm->pressing = 1;
                SetCapture(w);
            }
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (g_md_row >= 0) { /* a motion tile let go: clicked, it plays; dragged across, it comes into use or goes out */
            int row = g_md_row, id = g_md_id, moved = g_md_moved;
            g_md_row = -1; g_md_moved = 0; g_sl_dirty = 1;
            ReleaseCapture();
            if (!moved) { mot_play(row, id); return 0; }
            uint32_t set = mot_set(row);
            int on = (set >> id) & 1;
            if ((PtInRect(&g_mzone[row][0], p) && !on) || (PtInRect(&g_mzone[row][1], p) && on)) mot_toggle(row, id);
            return 0;
        }
        int h = set_hit_at(p), close = g_close_armed && h >= 0 && g_hits[h].kind == K_CLOSE;
        if (g_sizing) { g_sizing = 0; settings_save(); g_sl_dirty = 1; settings_tick(wall()); } /* let go: drawn in full at once */
        if (g_pv_drag || g_slide >= 0 || g_close_armed) { g_pv_drag = 0; g_slide = -1; g_close_armed = 0; g_sl_dirty = 1; }
        if (g_pv_turn) { /* let go: thrown, it spins on, as on the desktop */
            Session *pm = &g_pv_room.s[0];
            int thrown = pm->held && wall() - g_pv_turn_t < 0.1;
            if (pm->held) { pm->spin_x = thrown ? fmaxf(-8, fminf(8, g_pv_tvx)) : 0; pm->spin_y = thrown ? fmaxf(-8, fminf(8, g_pv_tvy)) : 0; }
            g_pv_turn = 0;
        }
        g_pv_room.s[0].pressing = 0; g_pv_room.s[0].held = 0;
        ReleaseCapture();
        if (close) set_close();
        return 0;
    }
    case WM_CAPTURECHANGED: g_pv_drag = 0; g_pv_turn = 0; g_pv_room.s[0].pressing = g_pv_room.s[0].held = 0; g_close_armed = 0; if (g_md_row >= 0) { g_md_row = -1; g_md_moved = 0; g_sl_dirty = 1; } if (g_sizing) { g_sizing = 0; settings_save(); } if (g_slide >= 0) { g_slide = -1; g_sl_dirty = 1; } return 0;
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_NCRBUTTONDOWN: case WM_NCRBUTTONUP: case WM_CONTEXTMENU:
        chat_hide(); return 0; /* a right-click here is the window's own: no menu, nothing for the cores under it */
    case WM_MOUSEWHEEL: { /* over the preview: zoom; over the page: scroll it */
        POINT c = p; ScreenToClient(w, &c);
        float notch = GET_WHEEL_DELTA_WPARAM(wp) / 120.0f;
        Session *pm = &g_pv_room.s[0];
        float hx = c.x - g_pv_rc.left - g_pv_mx, hy = c.y - g_pv_rc.top - g_pv_my;
        if (PtInRect(&g_pv_rc, c) && hole_pad(pm) > 1 && hypotf(hx, hy) < g_pv_big * SHELL_FRAC * 1.6f) { wheel_spin(pm, notch); return 0; } /* a black hole: the wheel whirls its disc (zoom: the buttons) */
        if (PtInRect(&g_pv_rc, c)) {
            g_pv_zoom = clampf(g_pv_zoom * powf(1.2f, notch), 1, 3);
            if (g_pv_zoom < 1.05f) g_pv_zoom = 1;
            g_pv_mx = g_pv_my = 0; /* centred again */
        } else g_set_scroll_to = clampf(g_set_scroll_to - notch * 72, 0, fmaxf(0, g_set_content_h - g_set_view_h)); /* (settings_tick glides there) */
        g_sl_dirty = 1;
        return 0;
    }
    case WM_SYSKEYDOWN: case WM_KEYDOWN:
        if (g_krec >= 0) { krec_key(wp); return 0; }
        if (wp == VK_ESCAPE) { set_close(); return 0; }
        break;
    case WM_SYSCHAR: if (g_krec >= 0) return 0; break; /* (no beep, no menu, for Alt+key) */
    case WM_CLOSE: set_close(); return 0;
    }
    return DefWindowProc(w, msg, wp, lp);
}

/* opens it in the middle of the monitor the pointer is on (or brings it back to the front) */
/* Vigil's icon: the core itself, drawn by the shared renderer at that size on nothing (see-through round it), for the
 * Settings window's title bar and taskbar button; a small one is drawn at twice the size and shrunk, for clean lines */
static HICON vigil_icon(int px) {
    static Session ic; static Canvas cv; static uint8_t *rgba; static size_t cap;
    session_init(&ic, "vigil-icon", 0, &g_shape);
    ic.ax = 0.42f; ic.ay = 0.6f; /* (a turn that shows its depth, as assets/icon.png does) */
    ic.backdrop = 1; /* (the dark band along its lines: it reads on a light taskbar too) */
    int ss = px < 64 ? 2 : 1, n = px * ss, nb = (int)(n / 0.8f + 0.5f), o0 = (nb - n) / 2; /* drawn larger, its middle kept: the shell fills the icon */
    canvas_resize(&cv, nb, nb);
    render_one(&cv, &ic, &g_shape, 0.5, 1.25f, g_core);
    if ((size_t)nb * nb * 4 > cap) { free(rgba); cap = (size_t)nb * nb * 4; rgba = malloc(cap); }
    canvas_rgba(&cv, rgba);
    for (int y = 0; y < n; y++) memmove(rgba + (size_t)y * n * 4, rgba + ((size_t)(y + o0) * nb + o0) * 4, (size_t)n * 4); /* (rows only move up and left) */
    BITMAPV5HEADER bh; memset(&bh, 0, sizeof bh);
    bh.bV5Size = sizeof bh; bh.bV5Width = px; bh.bV5Height = -px; bh.bV5Planes = 1; bh.bV5BitCount = 32; bh.bV5Compression = BI_BITFIELDS;
    bh.bV5RedMask = 0x00FF0000; bh.bV5GreenMask = 0x0000FF00; bh.bV5BlueMask = 0x000000FF; bh.bV5AlphaMask = 0xFF000000;
    void *bits = NULL;
    HDC dc = GetDC(NULL);
    HBITMAP col = CreateDIBSection(dc, (BITMAPINFO *)&bh, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, dc);
    if (!col || !bits) return NULL;
    uint32_t *o = bits;
    for (int y = 0; y < px; y++) for (int x = 0; x < px; x++) {
        unsigned c[4] = {0, 0, 0, 0};
        for (int j = 0; j < ss; j++) for (int i = 0; i < ss; i++) { const uint8_t *q = rgba + ((size_t)(y * ss + j) * n + x * ss + i) * 4; for (int k = 0; k < 4; k++) c[k] += q[k]; }
        for (int k = 0; k < 4; k++) c[k] /= (unsigned)(ss * ss);
        unsigned a = c[3] ? c[3] : 0, r = a ? c[0] * 255 / a : 0, g = a ? c[1] * 255 / a : 0, b = a ? c[2] * 255 / a : 0; /* (an icon wants straight alpha) */
        o[y * px + x] = (a << 24) | ((r > 255 ? 255 : r) << 16) | ((g > 255 ? 255 : g) << 8) | (b > 255 ? 255 : b);
    }
    HBITMAP mask = CreateBitmap(px, px, 1, 1, NULL);
    ICONINFO ii = {TRUE, 0, 0, mask, col};
    HICON h = CreateIconIndirect(&ii);
    DeleteObject(mask); DeleteObject(col);
    return h;
}
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
        /* an ordinary window (in the taskbar and Alt+Tab): above the cores while in use, under another app once it is (WM_ACTIVATE) */
        g_set = CreateWindowEx(WS_EX_LAYERED | WS_EX_APPWINDOW, L"VigilSettings", L"Vigil settings", WS_POPUP, 0, 0, 10, 10, NULL, NULL, sc.hInstance, NULL);
        { /* its icons: Vigil's core, at the sizes Windows asks for on this monitor */
            UINT dpi = GetDpiForWindow(g_set);
            if (!dpi) dpi = 96;
            HICON big = vigil_icon(MulDiv(GetSystemMetrics(SM_CXICON), (int)dpi, 96) < 32 ? 32 : MulDiv(32, (int)dpi, 96)), small = vigil_icon(MulDiv(16, (int)dpi, 96));
            if (big) SendMessage(g_set, WM_SETICON, ICON_BIG, (LPARAM)big);
            if (small) SendMessage(g_set, WM_SETICON, ICON_SMALL, (LPARAM)small);
        }
        g_sl.mem = CreateCompatibleDC(NULL);
        g_so.mem = CreateCompatibleDC(NULL);
        g_pv.mem = CreateCompatibleDC(NULL);
    }
    if (!IsWindowVisible(g_set)) {
        POINT c; GetCursorPos(&c); /* the monitor the pointer is on, or the one --on names */
        if (g_on_set) c = g_on;
        RECT wa = work_at((float)c.x, (float)c.y);
        double d = dpi_at((float)c.x, (float)c.y);
        if (fabs(d - g_sdpi) > 0.01) { g_sdpi = d; set_refont(); }
        g_set_w = clampf(g_set_w, 620, (wa.right - wa.left) / (float)g_sdpi); /* the size kept, as much as this monitor has room for */
        g_set_h = clampf(g_set_h, 480, (wa.bottom - wa.top) / (float)g_sdpi);
        int W = SW_W(), H = SW_H();
        SetWindowPos(g_set, HWND_NOTOPMOST, (wa.left + wa.right - W) / 2, (wa.top + wa.bottom - H) / 2, W, H, SWP_NOACTIVATE); /* (an ordinary window: the cores, always on top, stay above it) */
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
 * "reply": the pointer rests on the main core (its chat box, then lock-on), then on a small core.
 * "input": the wheel scrolls a long reply, a double-click opens the reply box, text is typed, an image is dragged onto
 * the core (it reads it) and dropped: an [Image #1] token. Nothing is sent.
 * "carry": Alt-click picks the cluster up, a slow move (the small cores trail behind), a dash toward the screen's corner
 * that turns away (they break off, fly on and bounce off its edges), then over to each one, which comes back to the
 * orbit, and put down. Recorded at Gravity: Floaty.
 * Each frame: int32 x, y (the pointer, in the frame), int32 shown (0 while carried: the pointer hides; 2 with an
 * image file under it), then W×H
 * BGR bytes of the cores and the chat box over the terminal's background (#212121). Header: int32 W, H, fps. */
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
    if (g_layer.shown && g_layer.px) rec_over(fr, W, H, g_layer.px, g_layer.W, g_layer.H, g_layer.x - g_rec_rc.left, g_layer.y - g_rec_rc.top, 0); /* READY's pieces */
    if (g_chat && IsWindowVisible(g_chat)) { /* the chat box paints itself into a bitmap of ours */
        RECT r;
        GetWindowRect(g_chat, &r);
        int tw = r.right - r.left, th = r.bottom - r.top;
        BITMAPINFO bi;
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = tw; bi.bmiHeader.biHeight = -th;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        uint32_t *tp = NULL;
        HDC mem = CreateCompatibleDC(NULL);
        HBITMAP bm = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, (void **)&tp, NULL, 0), old = SelectObject(mem, bm);
        SendMessage(g_chat, WM_PRINTCLIENT, (WPARAM)mem, PRF_CLIENT);
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
    int carry = !strcmp(g_rec_scene, "carry"), input = !strcmp(g_rec_scene, "input"), ready = !strcmp(g_rec_scene, "ready");
    g_single = 1; g_others = 1; g_focus_pinned = 0;
    snprintf(g_frame, sizeof g_frame, "diamond");
    if (carry) g_gravity = GRAVITIES[0]; /* Floaty: a flick breaks them loose */
    int W = (int)((carry ? 1300 : 1000) * g_dpi), H = (int)((carry ? 560 : input ? 620 : 440) * g_dpi), m = carry ? 0 : (int)(40 * g_dpi);
    RECT wk = g_on_set ? work_at((float)g_on.x, (float)g_on.y) : g_work; /* --on: that monitor */
    SetRect(&g_rec_rc, wk.right - m - W, wk.bottom - m - H, wk.right - m, wk.bottom - m);
    float L = (float)g_rec_rc.left, T = (float)g_rec_rc.top;
    g_mx = L + (carry || ready ? .5f : .8f) * W; g_my = T + (input ? .36f : .5f) * H; /* (READY: in the middle, so nothing thrown is cut at the screen's edge) */
    float cx = g_mx, cy = g_my;
    g_rec_mouse.x = (LONG)(L + .2f * W); g_rec_mouse.y = (LONG)(T + .9f * H);
    if (carry) g_rec_rc.top -= (int)(110 * g_dpi); /* room above: the dash turns up near the top */
    int32_t head[3] = {g_rec_rc.right - g_rec_rc.left, g_rec_rc.bottom - g_rec_rc.top, 15};
    fwrite(head, sizeof head, 1, f);
    const double WARM = 17.5, END = carry ? 11.5 : input ? 13.0 : ready ? 12.0 : 8.6; /* the demo runs a while first, so all three sessions are there */
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
        if (ready && t >= 0) { /* READY: a reply nobody has seen; the pointer goes slowly round the core, a little way off */
            Room *room = shown_room();
            if (!g_rec_still) { g_rec_still = 1; snprintf(g_focus, sizeof g_focus, "demo-0"); }
            int fi = index_of(g_focus);
            if (fi >= 0) { Session *s = &room->s[fi]; s->state = ST_IDLE; s->state_t = s->last = g_rec_now - 30; s->unread = 1; } /* (the pointer stays off the core, so it stays unread for the film) */
            float a = (float)(t * .45) + 2.4f, r = g_cw * (.9f + .25f * sinf((float)t * .7f));
            g_rec_mouse.x = (LONG)lroundf(cx + cosf(a) * r * 1.3f); g_rec_mouse.y = (LONG)lroundf(cy + sinf(a) * r * .8f);
        } else if (input && t >= 0) {
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
                    if (!strcmp(g_chat_sid, s->sid)) chat_scroll(notches);
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

/* ---- the right-click menu, in the Settings window's colours: its card behind, its text, the hover chip, an orange
 * check, a thin rule between groups. Built as an ordinary menu, then every item turned owner-drawn (menu_own) and
 * drawn here (WM_MEASUREITEM / WM_DRAWITEM in proc); Windows' dark menus give it a dark frame (menu_dark). */
typedef struct { wchar_t text[96]; int sep, sub; } MenuDraw;
static MenuDraw g_md[48];
static int g_md_n;
static double g_menu_dpi = 1;
static HFONT g_menu_font;
static COLORREF rgbx(unsigned v) { return RGB(v >> 16 & 255, v >> 8 & 255, v & 255); }
static void menu_dark(void) { /* uxtheme's SetPreferredAppMode(ForceDark) and FlushMenuThemes (by ordinal, as Notepad++ and others do); none: a light frame */
    HMODULE ux = LoadLibraryW(L"uxtheme.dll");
    if (!ux) return;
    typedef int (WINAPI *SetMode)(int);
    typedef void (WINAPI *Flush)(void);
    SetMode set = (SetMode)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    Flush flush = (Flush)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (set) set(2);
    if (flush) flush();
}
static void menu_own(HMENU m) {
    static HBRUSH bg;
    if (!bg) bg = CreateSolidBrush(rgbx(S_CARD));
    MENUINFO mi;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi; mi.fMask = MIM_BACKGROUND; mi.hbrBack = bg;
    SetMenuInfo(m, &mi);
    int n = GetMenuItemCount(m);
    for (int i = 0; i < n && g_md_n < 48; i++) {
        MenuDraw *d = &g_md[g_md_n++];
        MENUITEMINFOW ii;
        memset(&ii, 0, sizeof ii);
        ii.cbSize = sizeof ii; ii.fMask = MIIM_FTYPE | MIIM_STRING | MIIM_SUBMENU; ii.dwTypeData = d->text; ii.cch = 95;
        d->text[0] = 0;
        GetMenuItemInfoW(m, (UINT)i, TRUE, &ii);
        d->sep = (ii.fType & MFT_SEPARATOR) != 0; d->sub = ii.hSubMenu != NULL;
        if (ii.hSubMenu) menu_own(ii.hSubMenu);
        MENUITEMINFOW so;
        memset(&so, 0, sizeof so);
        so.cbSize = sizeof so; so.fMask = MIIM_FTYPE | MIIM_DATA; so.fType = ii.fType | MFT_OWNERDRAW; so.dwItemData = (ULONG_PTR)d;
        SetMenuItemInfoW(m, (UINT)i, TRUE, &so);
    }
}
static int menu_measure(MEASUREITEMSTRUCT *mi) {
    if (mi->CtlType != ODT_MENU || !mi->itemData) return 0;
    const MenuDraw *d = (const MenuDraw *)mi->itemData;
    double k = g_menu_dpi;
    if (d->sep) { mi->itemWidth = (UINT)(60 * k); mi->itemHeight = (UINT)(9 * k); return 1; }
    HDC dc = GetDC(NULL);
    HGDIOBJ of = SelectObject(dc, g_menu_font);
    wchar_t label[96]; const wchar_t *acc = wcschr(d->text, L'\t');
    size_t ln = acc ? (size_t)(acc - d->text) : wcslen(d->text);
    wcsncpy(label, d->text, ln); label[ln] = 0;
    SIZE a = {0, 0}, b = {0, 0};
    GetTextExtentPoint32W(dc, label, (int)wcslen(label), &a);
    if (acc) GetTextExtentPoint32W(dc, acc + 1, (int)wcslen(acc + 1), &b);
    SelectObject(dc, of); ReleaseDC(NULL, dc);
    mi->itemWidth = (UINT)(a.cx + (acc ? b.cx + 28 * k : 0) + (30 + 26) * k);
    mi->itemHeight = (UINT)(30 * k);
    return 1;
}
static int menu_draw(DRAWITEMSTRUCT *di) {
    if (di->CtlType != ODT_MENU || !di->itemData) return 0;
    const MenuDraw *d = (const MenuDraw *)di->itemData;
    HDC dc = di->hDC;
    RECT r = di->rcItem;
    double k = g_menu_dpi;
    HBRUSH bg = CreateSolidBrush(rgbx(S_CARD));
    FillRect(dc, &r, bg); DeleteObject(bg);
    if (d->sep) {
        RECT ln = {r.left + (int)(10 * k), (r.top + r.bottom) / 2, r.right - (int)(10 * k), (r.top + r.bottom) / 2 + 1};
        HBRUSH rb = CreateSolidBrush(rgbx(S_RULE)); FillRect(dc, &ln, rb); DeleteObject(rb);
    } else {
        int off = (di->itemState & (ODS_GRAYED | ODS_DISABLED)) != 0, hot = (di->itemState & ODS_SELECTED) && !off;
        if (hot) { /* the Settings window's hover chip */
            HBRUSH hb = CreateSolidBrush(rgbx(S_CHIP_HOT));
            HGDIOBJ ob = SelectObject(dc, hb), op = SelectObject(dc, GetStockObject(NULL_PEN));
            int rr = (int)(8 * k);
            RoundRect(dc, r.left + (int)(4 * k), r.top + 1, r.right - (int)(4 * k) + 1, r.bottom, rr, rr);
            SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(hb);
        }
        int cy = (r.top + r.bottom) / 2;
        if (di->itemState & ODS_CHECKED) { /* an orange tick */
            HPEN pen = CreatePen(PS_SOLID, (int)fmax(2, 2 * k), rgbx(S_OR));
            HGDIOBJ op = SelectObject(dc, pen);
            int x = r.left + (int)(12 * k);
            MoveToEx(dc, x, cy, NULL); LineTo(dc, x + (int)(4 * k), cy + (int)(4 * k)); LineTo(dc, x + (int)(11 * k), cy - (int)(4 * k));
            SelectObject(dc, op); DeleteObject(pen);
        }
        HGDIOBJ of = SelectObject(dc, g_menu_font);
        SetBkMode(dc, TRANSPARENT);
        wchar_t label[96]; const wchar_t *acc = wcschr(d->text, L'\t');
        size_t ln = acc ? (size_t)(acc - d->text) : wcslen(d->text);
        wcsncpy(label, d->text, ln); label[ln] = 0;
        RECT tr = {r.left + (int)(30 * k), r.top, r.right - (int)(26 * k), r.bottom};
        SetTextColor(dc, rgbx(off ? 0x5A5A63 : hot ? 0xFFFFFF : S_TEXT));
        DrawTextW(dc, label, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
        if (acc) { SetTextColor(dc, rgbx(off ? 0x4A4A52 : S_MUTED)); DrawTextW(dc, acc + 1, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX); }
        SelectObject(dc, of);
        if (d->sub) { /* its own chevron, and the system's own arrow kept out (it would be drawn black) */
            HPEN pen = CreatePen(PS_SOLID, (int)fmax(1, 1.5 * k), rgbx(hot ? S_TEXT : S_LABEL));
            HGDIOBJ op = SelectObject(dc, pen);
            int x = r.right - (int)(16 * k);
            MoveToEx(dc, x, cy - (int)(4 * k), NULL); LineTo(dc, x + (int)(4 * k), cy); LineTo(dc, x - 1, cy + (int)(4 * k) + 1);
            SelectObject(dc, op); DeleteObject(pen);
        }
    }
    ExcludeClipRect(dc, r.left, r.top, r.right, r.bottom);
    return 1;
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
    HMENU sm = CreatePopupMenu(); /* until the Settings window has its Performance page */
    static const wchar_t *SM_TEXT[SM_N] = {L"Saver (least CPU)", L"Balanced", L"Smooth (up to 60)", L"Max (display rate)", L"Custom"};
    for (int i = 0; i < SM_N; i++)
        if (i != SM_CUSTOM || g_smooth == SM_CUSTOM) AppendMenu(sm, MF_STRING | (g_smooth == i ? MF_CHECKED : 0), ID_SMOOTH + i, SM_TEXT[i]);
    AppendMenu(m, MF_POPUP, (UINT_PTR)sm, L"Smoothness");
    AppendMenu(m, MF_STRING, ID_SETTINGS, L"Settings...");
    AppendMenu(m, MF_SEPARATOR, 0, NULL);
    AppendMenu(m, MF_STRING, ID_QUIT, L"Quit Vigil");
    POINT p; GetCursorPos(&p);
    { /* in the Settings window's colours, at the scale of the monitor it opens on */
        double k = dpi_at((float)p.x, (float)p.y);
        if (!g_menu_font || fabs(k - g_menu_dpi) > 0.01) {
            if (g_menu_font) DeleteObject(g_menu_font);
            g_menu_dpi = k;
            g_menu_font = CreateFontW(-(int)(13 * k + 0.5), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        }
        g_md_n = 0;
        menu_own(m);
    }
    chat_hide();
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
    if (id >= ID_SMOOTH && id < ID_SMOOTH + SM_N) { g_smooth = id - ID_SMOOTH; settings_save(); pics_invalidate(); return; }
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
        else if (wp == ID_WTENTER) { KillTimer(w, ID_WTENTER); wt_keys(g_wt_pid, L"\r"); }
        else tick();
        return 0;
    case WM_MOUSEWHEEL: { /* over a core: its reply scrolls, and the core spins with the wheel */
        int i = core_at(p, NULL, NULL);
        if (i < 0 || i >= room->n) return 0;
        float notches = GET_WHEEL_DELTA_WPARAM(wp) / 120.0f;
        Session *s = &room->s[i];
        wheel_spin(s, notches);
        g_hover_since = wall(); /* (scrolling on it is not resting on it) */
        if (!strcmp(g_chat_sid, s->sid)) chat_scroll(notches);
        return 0;
    }
    case WM_MOUSEHWHEEL: { /* sideways (a touchpad swipe): the core spins about its other axis */
        int i = core_at(p, NULL, NULL);
        if (i < 0 || i >= room->n) return 0;
        wheel_spin2(&room->s[i], GET_WHEEL_DELTA_WPARAM(wp) / 120.0f, 1);
        return 0;
    }
    case WM_HOTKEY:
        if (wp == HK_MODE) toggle_mode();
        else if (wp == HK_HIDE) g_hidden = !g_hidden;
        else if (wp == HK_REPLY) { /* the reply line under the main core's last answer */
            Room *rm = shown_room();
            int fi = index_of(g_focus);
            if (fi < 0 && rm->n) fi = 0;
            if (fi >= 0) { g_hidden = 0; input_open_for(&rm->s[fi]); }
        } else if (wp == HK_SET) settings_open();
        return 0;
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
            if (g_move_mod == VK_MENU) mask_alt();
            g_moving = 1;
            if (g_single && i == g_top) { g_move_origin.x = (LONG)g_mx; g_move_origin.y = (LONG)g_my; } /* the main core: the whole cluster */
            else if (g_single) { Orbit *ob = orbit_of(s->sid); g_move_origin.x = (LONG)ob->px; g_move_origin.y = (LONG)ob->py; g_moving = 2; } /* a small core, by hand */
            else { g_move_origin.x = g_core_rc[i].left; g_move_origin.y = g_core_rc[i].top; }
            g_hold_x = g_move_origin.x; g_hold_y = g_move_origin.y;
            start_follow();
            return 0;
        }
        s->press_at = wall(); s->press_x = lx; s->press_y = ly; s->pressing = 1;
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
            g_hover_since = 0; /* the chat box shows without the usual pause */
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
    case WM_MEASUREITEM: if (menu_measure((MEASUREITEMSTRUCT *)lp)) return TRUE; break;
    case WM_DRAWITEM: if (menu_draw((DRAWITEMSTRUCT *)lp)) return TRUE; break;
    case WM_DROPFILES: { /* images dropped on a core (when the drop target below is not in use) */
        POINT dp;
        DragQueryPoint((HDROP)wp, &dp);
        ClientToScreen(w, &dp);
        drop_on_core(core_at(dp, NULL, NULL), (HDROP)wp);
        DragFinish((HDROP)wp);
        return 0;
    }
    case WM_SETCURSOR: /* moved by the mouse: the core is the pointer */
        if (g_hide_pointer) { SetCursor(NULL); return TRUE; } /* typing a reply */
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
static int parse_hotkey(const char *spec, UINT *mods, UINT *vk);
/* the four global hotkeys (Settings > Shortcuts) registered again; one another app holds is marked */
static void hotkeys_apply(void) {
    for (int i = 0; i < 4; i++) {
        UnregisterHotKey(g_cluster.w, HK_MODE + i);
        g_hk_fail[i] = 0;
        const char *spec = hk_spec(i);
        UINT mods, vk;
        if (!spec[0] || !strcmp(spec, "none")) continue;
        if (!parse_hotkey(spec, &mods, &vk) || !RegisterHotKey(g_cluster.w, HK_MODE + i, mods | MOD_NOREPEAT, vk)) g_hk_fail[i] = 1;
    }
    g_sl_dirty = 1;
}
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

/* How long until the next tick, in 100 ns units. Ticks land just after the display's refresh (DWM's vblank), every
 * k refreshes for the rate wanted: on a 60 Hz screen 60 a second while anything moves, 30, 20 or 15 otherwise, so
 * a moving core steps the same distance each frame. Each due time follows the last, so the tick's own work no longer
 * stretches the interval; a tick that ran late joins the grid again at the next refresh. */
static LONGLONG next_wait(void) {
    static LONGLONG next;
    LARGE_INTEGER now, qf;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&qf);
    double period = qf.QuadPart / (g_hz > 1 ? g_hz : 60);
    LONGLONG vblank = now.QuadPart;
    DWM_TIMING_INFO ti;
    memset(&ti, 0, sizeof ti);
    ti.cbSize = sizeof ti;
    /* DWM's clock is the main display's; on a monitor with another refresh rate keep that monitor's period instead */
    if (DwmGetCompositionTimingInfo(NULL, &ti) == S_OK && ti.qpcRefreshPeriod > 0 && fabs(qf.QuadPart / (double)ti.qpcRefreshPeriod - g_hz) < 1.5) {
        period = (double)ti.qpcRefreshPeriod;
        vblank = (LONGLONG)ti.qpcVBlank;
    }
    int k = (int)(tick_interval() * qf.QuadPart / period + 0.5);
    if (k < 1) k = 1;
    LONGLONG step = (LONGLONG)(k * period), at = next + step;
    if (!next || at <= now.QuadPart || at > now.QuadPart + step + (LONGLONG)period) { /* first, late, or the rate changed: back onto the grid */
        double since = (double)(now.QuadPart - vblank);
        LONGLONG n = (LONGLONG)ceil(since / period + 0.05);
        at = vblank + (LONGLONG)(n * period) + qf.QuadPart / 1000; /* 1 ms after a refresh */
        if (at <= now.QuadPart) at += (LONGLONG)period;
    }
    next = at;
    g_tick_hz = qf.QuadPart / (double)step;
    return (at - now.QuadPart) * 10000000LL / qf.QuadPart;
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
        "  --width PIXELS       column width, in 96-dpi pixels (default: 17% of the primary screen)\n"
        "  --smooth NAME        how often cores are redrawn: saver, balanced (default), smooth or max\n"
        "  --font-size N        chat box text size (default 14)\n"
        "  --glow X  --core X   glow strength, inner core radius (default 1, 0.35)\n"
        "  --frame NAME         focus frame: segments (default) | lens | polar | diamond | lock | orbits | none\n"
        "  --labels always|hover   all cores: name and state above each core (default always)\n"
        "  --away SECONDS       no input this long: cores show STANDBY (default 120, 0 = never)\n"
        "  --stats              every 5 s, frames, pictures and window updates a second to panel-stats.txt\n"
        "  --settings           open the Settings window at once\n"
        "  --no-gpu             draw the black hole on the CPU, not the GPU\n"
        "  --settings-shot FILE open it, save it as a BMP after 2 s and quit (to check a change)\n"
        "  --settings-zoom X    start the preview zoomed in, 1 to 3 (with --settings-shot)\n"
        "  --settings-light     start the preview on its light background (with --settings-shot)\n"
        "  --on X,Y             --record and the Settings window go on the monitor with that desktop point\n"
        "  --record SCENE FILE  reply | input | carry: a scripted demo scene, frames written raw to FILE (the README's animations)\n"
        "  --settings-tab NAME  open it on that page: look, cluster, replies, motions, shortcuts, performance\n"
        "  --settings-fold      with its sidebar folded to icons\n"
        "  --settings-state S   its preview (and Motions) on that state, e.g. sleep\n"
        "  --settings-scroll PX its page scrolled down that far (with --settings-shot)\n"
        "  --settings-hole A N STEP  (with --settings-shot FILE) play the black hole's action A, save N shots STEP s apart\n\n"
        "Right-click a core: Copy reply, Reply, Bring to front, and Settings... (a window with a live preview:\n"
        "shape, inner core, glow, focus frame, size, gravity, other sessions, chat box, text size, labels,\n"
        "standby); settings are kept and win over these options until \"Reset to launch options\".\n\n"
        "Alt-drag a core: move the cluster (single core) or that core (all cores). Alt-click: pick it up,\n"
        "the next click puts it down.",
        "Vigil", MB_OK);
}

int main(int argc, char **argv) {
    const char *events = NULL;
    SUB_UNTIL_SEEN = 1; /* a finished subagent stays until its reply has been pointed at */
    menu_dark(); /* (the right-click menu's frame dark, like the Settings window) */
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
        else if (!strcmp(a, "--smooth") && v) {
            int k = -1;
            for (int j = 0; j < SM_CUSTOM; j++) if (!strcmp(v, SMOOTH_NAME[j])) k = j;
            if (k < 0) { usage(); return 2; }
            g_smooth = k; i++;
        }
        else if (!strcmp(a, "--font-size") && v) { g_font = (float)atof(v); if (g_font < 8) g_font = 8; i++; }
        else if (!strcmp(a, "--glow") && v) { g_glow = (float)atof(v); i++; }
        else if (!strcmp(a, "--core") && v) { g_core = (float)atof(v); i++; }
        else if (!strcmp(a, "--frame") && v) { snprintf(g_frame, sizeof g_frame, "%s", v); i++; }
        else if (!strcmp(a, "--labels") && v) { g_labels = strcmp(v, "hover") != 0; i++; }
        else if (!strcmp(a, "--away") && v) { g_away_after = atof(v); i++; }
        else if (!strcmp(a, "--stats")) g_stats = 1;
        else if (!strcmp(a, "--no-gpu")) g_nogpu_arg = 1;
        else if (!strcmp(a, "--settings")) g_open_settings = 1;
        else if (!strcmp(a, "--settings-tab") && v) { /* look, cluster, replies, motions, shortcuts, performance */
            static const char *TABS[PG_N] = {"look", "cluster", "replies", "motions", "shortcuts", "performance"};
            for (int k = 0; k < PG_N; k++) if (!strncmp(v, TABS[k], 4)) g_tab = k;
            g_open_settings = 1; i++;
        }
        else if (!strcmp(a, "--settings-fold")) g_set_side = 0;
        else if (!strcmp(a, "--settings-scroll") && v) { g_scroll_arg = (float)atof(v); i++; } /* (the page scrolled down that far: to see all of it in a screenshot) */
        else if (!strcmp(a, "--settings-state") && v) { /* the state its preview (and Motions) shows: idle, think, work, wait, cheer, sleep, ... */
            for (int k = 0; k < ST_N; k++) if (!_strnicmp(v, STATE_NAME[k], 4)) g_pv_st = k;
            i++;
        }
        else if (!strcmp(a, "--on") && v && sscanf(v, "%ld,%ld", &g_on.x, &g_on.y) == 2) { g_on_set = 1; i++; }
        else if (!strcmp(a, "--settings-light")) g_pv_light_arg = 1;
        else if (!strcmp(a, "--settings-zoom") && v) { g_pv_zoom = clampf((float)atof(v), 1, 3); i++; }
        else if (!strcmp(a, "--record") && v && i + 2 < argc) { snprintf(g_rec_scene, sizeof g_rec_scene, "%s", v); snprintf(g_rec_file, sizeof g_rec_file, "%s", argv[i + 2]); i += 2; }
        else if (!strcmp(a, "--settings-hole") && v && i + 3 < argc) { g_shot_act = atoi(v); g_shot_n = atoi(argv[i + 2]); g_shot_step = (float)atof(argv[i + 3]); g_pv_st = ST_SLEEP; i += 3; }
        else if (!strcmp(a, "--settings-shot") && v) { snprintf(g_set_shot, sizeof g_set_shot, "%s", v); g_open_settings = 1; i++; }
        else { usage(); return a[0] == '-' && (!strcmp(a, "-h") || !strcmp(a, "--help")) ? 0 : 2; }
    }
    if (g_rec_scene[0]) { if (strcmp(g_rec_scene, "reply") && strcmp(g_rec_scene, "carry") && strcmp(g_rec_scene, "input") && strcmp(g_rec_scene, "ready")) { usage(); return 2; } g_demo_n = 3; g_rec_now = 1.7e9; }
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
    if (g_nogpu_arg) g_gpu = 0; /* (--no-gpu wins: a check against the CPU) */
    if (g_pv_light_arg > 0) g_pv_light = 1;
    if (!shape_make(&g_shape, g_shape_name)) { snprintf(g_shape_name, sizeof g_shape_name, "%s", g_launch.shape); shape_make(&g_shape, g_shape_name); }
    pos_read();
    { /* per-monitor DPI aware (v2): every monitor at its own scale, nothing stretched by Windows */
        BOOL(WINAPI * aware)(HANDLE) = (void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
        if (!aware || !aware((HANDLE)-4 /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */)) SetProcessDPIAware();
    }

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
    g_dpi = g_sdpi = dpi_at(g_work.left + 1.0f, g_work.top + 1.0f);
    g_hz = hz_at(g_work.left + 1.0f, g_work.top + 1.0f);
    double sw = (g_work.right - g_work.left) / g_dpi; /* the primary screen's width in 96-dpi units */
    g_col_dip = g_width > 0 ? g_width : fmax(220, fmin(360, sw * 0.17));
    g_col_w = (int)(g_col_dip * g_dpi);
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
    tc.lpfnWndProc = chat_proc;
    tc.hInstance = wc.hInstance;
    tc.hCursor = LoadCursor(NULL, IDC_ARROW);
    tc.lpszClassName = L"VigilChat";
    RegisterClass(&tc);
    pane_create(&g_cluster);
    g_chat = CreateWindowEx(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                           L"VigilChat", L"Vigil", WS_POPUP, 0, 0, 1, 1, NULL, NULL, wc.hInstance, NULL);
    SetLayeredWindowAttributes(g_chat, 0, 255, LWA_ALPHA); /* opaque: text behind it must not show through */
    main_place();
    hotkeys_apply();
    for (int i = 0; i < 4; i++) if (g_hk_fail[i]) fprintf(stderr, "vigil-panel: hotkey %s cannot be used (another app has it, or it does not read)\n", hk_spec(i));
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
        if (g_scroll_arg > 0 && wall() > shot_at - 1.5) { g_set_scroll_to = g_scroll_arg; g_scroll_arg = 0; g_sl_dirty = 1; }
        if (g_set_shot[0] && g_shot_act) { /* (a sequence: the action played once the hole has grown) */
            static double play_at; static int k;
            Session *ps = &g_pv_room.s[0];
            if (!play_at && ps->mo.rt == RT_HOLE && ps->mo.rs.hole.grow > .97f && wall() > shot_at) { hole_play(ps, g_shot_act); play_at = wall() + .1; }
            if (play_at && wall() >= play_at + k * g_shot_step) {
                char f[1100]; snprintf(f, sizeof f, "%s_%02d.bmp", g_set_shot, k);
                settings_shot(f);
                if (++k >= g_shot_n) break;
            }
            if (wall() > shot_at + 60) break;
        }
        else if (g_set_shot[0] && wall() > shot_at) { settings_shot(g_set_shot); break; }
        LARGE_INTEGER due;
        due.QuadPart = -next_wait();
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

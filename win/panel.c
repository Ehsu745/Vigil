/* vigil-panel.exe: Vigil's floating panel for Windows.
 *
 * Claude Code and the hook run inside WSL; this native Windows program reads the same events file through
 * the \\wsl$ share (the WSL launcher win/vigil-win passes its Windows path with --events) and draws one
 * core per session in a see-through column on the right edge of the screen, like the macOS panel. It uses
 * the shared code unchanged (events.c, geom.c, session.c, motion.c, render.c): every frame each core is
 * rendered into its own canvas and laid into a premultiplied BGRA bitmap shown with UpdateLayeredWindow.
 *
 * First version: the cores, their motion, hover replies, lock-on, the rush defence, press, drag to spin,
 * double-click ring, and a right-click menu (shape, glow, quit). Not yet: tooltip text, single-core mode,
 * focus frame, voice, carry; the iTerm2 features have no Windows counterpart.
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
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/vigil.h"

static Room g_room, g_ghost;
static Shape g_shape;
static Tail g_tail;
static Event g_ev[1024];
static Demo g_demo;
static int g_demo_n, g_width;
static float g_glow = 1.0f, g_core = 0.35f;
static char g_shape_name[16] = "geo2";
static double g_dpi = 1;

static HWND g_wnd;
static int W, H;                  /* the window, in pixels */
static HDC g_mem;
static HBITMAP g_bmp;
static uint32_t *g_px;            /* W×H premultiplied BGRA, top-down */
static RECT g_core_rc[MAX_SESS];  /* where each core was drawn, in window pixels */
static int g_core_n;
static Canvas g_cv;
static uint8_t *g_rgba;
static size_t g_rgba_cap;

/* the pointer */
static double g_last, g_hover_since;
static int g_hover = -1, g_transparent = -1;
static int g_down = -1, g_dragging;
static POINT g_down_at, g_drag_at;
static double g_drag_t;
static float g_vx, g_vy;
static double rush_t[128];
static POINT rush_p[128];
static int rush_n;

enum { ID_TIMER = 1, ID_QUIT = 100, ID_SHAPE = 200, ID_GLOW = 300 };
static const char *SHAPES[4] = {"dodeca", "bucky", "geo2", "geo3"};
static const wchar_t *SHAPE_NAMES[4] = {L"Dodecahedron", L"Buckyball", L"Geodesic", L"Fine geodesic"};
static const float GLOWS[3] = {0.7f, 1.0f, 1.4f};
static const wchar_t *GLOW_NAMES[3] = {L"Soft", L"Normal", L"Bright"};

static double wall(void) {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    return (double)u.QuadPart / 1e7 - 11644473600.0; /* the Unix epoch, like the hook's time.time() in WSL */
}

static Room *shown_room(void) { return g_room.n ? &g_room : &g_ghost; }

/* ---- drawing -------------------------------------------------------------------------------------- */
static void make_bitmap(void) {
    if (g_bmp) DeleteObject(g_bmp);
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H; /* top-down */
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    g_bmp = CreateDIBSection(g_mem, &bi, DIB_RGB_COLORS, (void **)&g_px, NULL, 0);
    SelectObject(g_mem, g_bmp);
}

/* the cores in a column, as large as fits (the macOS panel's layout with --labels hover) */
static void lay_out(int n) {
    int gap = (int)(10 * g_dpi), top = (int)(30 * g_dpi);
    int side = (H - top) / (n > 0 ? n : 1) - gap;
    if (side > W - (int)(12 * g_dpi)) side = W - (int)(12 * g_dpi);
    if (side < 24) side = 24;
    int total = n * (side + gap), y0 = top + ((H - top) - total) / 2;
    if (y0 < top) y0 = top;
    for (int i = 0; i < n; i++) {
        int x = (W - side) / 2, y = y0 + i * (side + gap);
        SetRect(&g_core_rc[i], x, y, x + side, y + side);
    }
    g_core_n = n;
}

static void draw(double now) {
    Room *room = shown_room();
    int n = room->n > MAX_SESS ? MAX_SESS : room->n;
    lay_out(n);
    memset(g_px, 0, (size_t)W * H * 4);
    for (int i = 0; i < n; i++) {
        RECT r = g_core_rc[i];
        int px = r.right - r.left;
        canvas_resize(&g_cv, px, px);
        render_one(&g_cv, &room->s[i], &g_shape, now, g_glow, g_core);
        size_t need = (size_t)px * px * 4;
        if (need > g_rgba_cap) { free(g_rgba); g_rgba = malloc(need); g_rgba_cap = need; }
        canvas_rgba(&g_cv, g_rgba);
        for (int y = 0; y < px; y++) {
            int Y = r.top + y;
            if (Y < 0 || Y >= H) continue;
            const uint8_t *s = g_rgba + (size_t)y * px * 4;
            uint32_t *d = g_px + (size_t)Y * W + r.left;
            for (int x = 0; x < px; x++, s += 4) /* RGBA → BGRA, both premultiplied */
                if (s[3]) d[x] = (uint32_t)s[2] | (uint32_t)s[1] << 8 | (uint32_t)s[0] << 16 | (uint32_t)s[3] << 24;
        }
    }
    POINT src = {0, 0};
    SIZE size = {W, H};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(g_wnd, NULL, NULL, &size, g_mem, &src, 0, &blend, ULW_ALPHA);
}

/* ---- the pointer ---------------------------------------------------------------------------------- */
/* the core under p (window pixels), or -1; look gets the offset from its centre in shell radii (y down) */
static int core_at(POINT p, float *lx, float *ly) {
    for (int i = 0; i < g_core_n; i++) {
        RECT r = g_core_rc[i];
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
    if (g_down >= 0) return;
    Room *room = shown_room();
    RECT wr; GetWindowRect(g_wnd, &wr);
    for (int i = 0; i < g_core_n && i < room->n; i++) {
        Session *s = &room->s[i];
        RECT r = g_core_rc[i];
        float R = (r.right - r.left) * SHELL_FRAC, cx = wr.left + (r.left + r.right) * 0.5f, cy = wr.top + (r.top + r.bottom) * 0.5f;
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

/* every frame: what the pointer is over. Over a core the window takes clicks; elsewhere they go through. */
static void track_mouse(double now) {
    POINT scr, p;
    GetCursorPos(&scr);
    p = scr;
    ScreenToClient(g_wnd, &p);
    Room *room = shown_room();
    float lx = 0, ly = 0;
    int hit = core_at(p, &lx, &ly);
    if (hit >= room->n) hit = -1;
    if (hit != g_hover) { g_hover = hit; g_hover_since = now; }
    for (int i = 0; i < room->n; i++) {
        Session *s = &room->s[i];
        s->hover_to = i == hit || i == g_down;
        if (i == hit) { s->look_x_to = lx; s->look_y_to = ly; }
        s->lock_to = i == hit && !g_dragging && now - g_hover_since > 2.0 ? 1.0f : 0.0f; /* lock-on after resting 2 s */
    }
    watch_for_rush(now, scr);
    int transparent = hit < 0 && g_down < 0;
    if (transparent != g_transparent) {
        LONG_PTR ex = GetWindowLongPtr(g_wnd, GWL_EXSTYLE);
        SetWindowLongPtr(g_wnd, GWL_EXSTYLE, transparent ? ex | WS_EX_TRANSPARENT : ex & ~WS_EX_TRANSPARENT);
        g_transparent = transparent;
    }
}

static void tick(void) {
    double now = wall();
    float dt = (float)(now - g_last);
    if (dt > 0.25f) dt = 0.25f;
    g_last = now;
    if (g_demo_n) demo_run(&g_demo, &g_room, now);
    else {
        int n = tail_poll(&g_tail, g_ev, 1024);
        for (int i = 0; i < n; i++) room_apply(&g_room, &g_ev[i]);
    }
    track_mouse(now);
    room_tick(&g_room, &g_shape, now, dt);
    g_ghost.s[0].last = now;
    room_tick(&g_ghost, &g_shape, now, dt);
    draw(now);
}

static void menu(HWND w) {
    HMENU m = CreatePopupMenu(), shape = CreatePopupMenu(), glow = CreatePopupMenu();
    for (int i = 0; i < 4; i++) AppendMenu(shape, MF_STRING | (!strcmp(g_shape_name, SHAPES[i]) ? MF_CHECKED : 0), ID_SHAPE + i, SHAPE_NAMES[i]);
    for (int i = 0; i < 3; i++) AppendMenu(glow, MF_STRING | (g_glow == GLOWS[i] ? MF_CHECKED : 0), ID_GLOW + i, GLOW_NAMES[i]);
    AppendMenu(m, MF_POPUP, (UINT_PTR)shape, L"Shape");
    AppendMenu(m, MF_POPUP, (UINT_PTR)glow, L"Glow");
    AppendMenu(m, MF_SEPARATOR, 0, NULL);
    AppendMenu(m, MF_STRING, ID_QUIT, L"Quit Vigil");
    POINT p; GetCursorPos(&p);
    SetForegroundWindow(w); /* so the menu closes when you click elsewhere */
    int id = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, w, NULL);
    DestroyMenu(m);
    if (id == ID_QUIT) PostQuitMessage(0);
    else if (id >= ID_SHAPE && id < ID_SHAPE + 4) { snprintf(g_shape_name, sizeof g_shape_name, "%s", SHAPES[id - ID_SHAPE]); shape_make(&g_shape, g_shape_name); }
    else if (id >= ID_GLOW && id < ID_GLOW + 3) g_glow = GLOWS[id - ID_GLOW];
}

static LRESULT CALLBACK proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    Room *room = shown_room();
    POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    switch (msg) {
    case WM_TIMER: tick(); return 0;
    case WM_LBUTTONDOWN: { /* a press: the patch under it goes in; it may become a drag */
        float lx, ly;
        int i = core_at(p, &lx, &ly);
        if (i < 0 || i >= room->n) return 0;
        Session *s = &room->s[i];
        s->press_at = wall(); s->press_x = lx; s->press_y = ly;
        g_down = i; g_dragging = 0; g_down_at = g_drag_at = p; g_drag_t = wall();
        SetCapture(w);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_down >= 0 && g_down < room->n) { /* drag: the shell turns under the hand, like the macOS panel */
            Session *s = &room->s[g_down];
            if (!g_dragging && abs(p.x - g_down_at.x) + abs(p.y - g_down_at.y) < (int)(4 * g_dpi)) return 0;
            g_dragging = 1; s->held = 1;
            double now = wall();
            float dx = (float)(p.x - g_drag_at.x) / (float)g_dpi, dy = (float)(p.y - g_drag_at.y) / (float)g_dpi, dt = (float)(now - g_drag_t);
            s->ay += dx * 0.012f; s->ax += dy * 0.012f;
            if (dt > 0.001f) { g_vx = dy * 0.012f / dt; g_vy = dx * 0.012f / dt; }
            g_drag_at = p; g_drag_t = now;
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_down >= 0 && g_down < room->n && g_dragging) { /* let go: it spins on and slows back to its own rotation */
            Session *s = &room->s[g_down];
            int thrown = wall() - g_drag_t < 0.1;
            s->held = 0;
            s->spin_x = thrown ? fmaxf(-8, fminf(8, g_vx)) : 0;
            s->spin_y = thrown ? fmaxf(-8, fminf(8, g_vy)) : 0;
        } else if (g_down >= 0 && g_down < room->n) room->s[g_down].unread = 0;
        g_down = -1; g_dragging = 0;
        ReleaseCapture();
        return 0;
    case WM_LBUTTONDBLCLK: {
        int i = core_at(p, NULL, NULL);
        if (i >= 0 && i < room->n) room->s[i].ring_at = wall();
        return 0;
    }
    case WM_RBUTTONUP: if (core_at(p, NULL, NULL) >= 0) menu(w); return 0;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE; /* never take focus from the terminal */
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProc(w, msg, wp, lp);
}

static void usage(void) {
    MessageBoxA(NULL,
        "vigil-panel.exe: Vigil's floating panel for Windows\n\n"
        "  --events PATH   the events file (from WSL: \\\\wsl$\\<distro>\\home\\<you>\\.dodeca-core\\events.jsonl;\n"
        "                  the win/vigil-win launcher passes it for you)\n"
        "  --demo [N]      made-up sessions instead of the events file (1-6, default 3)\n"
        "  --shape NAME    dodeca | bucky | geo2 (default) | geo3\n"
        "  --width PIXELS  panel width (default: 17% of the screen)\n"
        "  --glow X  --core X",
        "Vigil", MB_OK);
}

int main(int argc, char **argv) {
    const char *events = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--demo")) { g_demo_n = 3; if (v && v[0] >= '1' && v[0] <= '6' && !v[1]) { g_demo_n = v[0] - '0'; i++; } }
        else if (!strcmp(a, "--events") && v) { events = v; i++; }
        else if (!strcmp(a, "--shape") && v) { snprintf(g_shape_name, sizeof g_shape_name, "%s", v); i++; }
        else if (!strcmp(a, "--width") && v) { g_width = atoi(v); i++; }
        else if (!strcmp(a, "--glow") && v) { g_glow = (float)atof(v); i++; }
        else if (!strcmp(a, "--core") && v) { g_core = (float)atof(v); i++; }
        else { usage(); return a[0] == '-' && (!strcmp(a, "-h") || !strcmp(a, "--help")) ? 0 : 2; }
    }
    if (!shape_make(&g_shape, g_shape_name)) { usage(); return 2; }
    if (!events && !g_demo_n) { usage(); return 2; }
    SetProcessDPIAware();
    HDC screen = GetDC(NULL);
    g_dpi = GetDeviceCaps(screen, LOGPIXELSX) / 96.0;
    ReleaseDC(NULL, screen);

    double now = wall();
    g_last = now;
    session_init(&g_ghost.s[0], "ghost", now, &g_shape);
    snprintf(g_ghost.s[0].name, sizeof g_ghost.s[0].name, "no session");
    g_ghost.n = 1;
    if (g_demo_n) demo_start(&g_demo, g_demo_n, now);
    else {
        tail_init(&g_tail, events);
        int n = tail_history(&g_tail, now - 1800, g_ev, 1024); /* the sessions of the last half hour */
        for (int i = 0; i < n; i++) room_apply(&g_room, &g_ev[i]);
    }

    /* the right edge of the primary screen's work area */
    RECT work;
    SystemParametersInfo(SPI_GETWORKAREA, 0, &work, 0);
    int sw = work.right - work.left;
    W = g_width > 0 ? g_width : (int)fmax(220 * g_dpi, fmin(360 * g_dpi, sw * 0.17));
    H = work.bottom - work.top;
    WNDCLASS wc;
    memset(&wc, 0, sizeof wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"VigilPanel";
    RegisterClass(&wc);
    g_wnd = CreateWindowEx(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                           L"VigilPanel", L"Vigil", WS_POPUP, work.right - W, work.top, W, H, NULL, NULL, wc.hInstance, NULL);
    g_mem = CreateCompatibleDC(NULL);
    make_bitmap();
    ShowWindow(g_wnd, SW_SHOWNOACTIVATE);
    SetTimer(g_wnd, ID_TIMER, 33, NULL);
    MSG m;
    while (GetMessage(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessage(&m); }
    return 0;
}

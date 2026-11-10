/* dodeca-core: one glowing polyhedron per running Claude Code session, drawn in the terminal.
 *
 * The picture is rendered in software and sent to the terminal one of three ways:
 *   kitty  - kitty graphics protocol (true pixels; kitty, Ghostty, WezTerm, recent iTerm2)
 *   iterm  - iTerm2 inline images (true pixels; any iTerm2)
 *   cells  - half-block characters, two pixels per cell (works anywhere with truecolor, but coarse)
 *
 * Two layouts:
 *   grid   - the sessions side by side in one picture, labels drawn into it (a window of its own)
 *   list   - a narrow column: for each session two lines of terminal text, then its polyhedron (a side pane)
 */
#include "dodeca.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>
#include <zlib.h>

enum { OUT_AUTO, OUT_CELLS, OUT_KITTY, OUT_ITERM };
enum { LAYOUT_GRID, LAYOUT_LIST };

static volatile sig_atomic_t g_quit, g_winch;
static struct termios g_old;
static int g_raw;
static char g_pidfile[1024];

static void on_quit(int s) { (void)s; g_quit = 1; }
static void on_winch(int s) { (void)s; g_winch = 1; }

static double wall(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + tv.tv_usec / 1e6;
}

/* ---- a growing output buffer: each frame goes out in one write ------------------------------- */
typedef struct { char *s; size_t n, cap; } Buf;
static void put(Buf *b, const void *s, size_t n) {
    if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2; b->s = realloc(b->s, b->cap); }
    memcpy(b->s + b->n, s, n);
    b->n += n;
}
static void puts_(Buf *b, const char *s) { put(b, s, strlen(s)); }
static size_t flush(Buf *b) {
    size_t off = 0, total = b->n;
    while (off < b->n) {
        ssize_t k = write(STDOUT_FILENO, b->s + off, b->n - off);
        if (k < 0) { if (errno == EINTR || errno == EAGAIN) continue; break; }
        off += (size_t)k;
    }
    b->n = 0;
    return total;
}

static size_t base64(const uint8_t *in, size_t n, char *out) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = T[v >> 18 & 63];
        out[o++] = T[v >> 12 & 63];
        out[o++] = i + 1 < n ? T[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? T[v & 63] : '=';
    }
    return o;
}

/* ---- terminal ---------------------------------------------------------------------------------- */
static void term_restore(void) {
    if (g_pidfile[0]) { unlink(g_pidfile); g_pidfile[0] = 0; }
    if (!g_raw) return;
    const char *off = "\x1b_Ga=d,d=A,q=2\x1b\\\x1b[0m\x1b[?25h\x1b[?1049l"; /* drop images, show cursor, leave alt screen */
    if (write(STDOUT_FILENO, off, strlen(off)) < 0) { /* nothing to do about it */ }
    usleep(120000); /* some terminals answer the delete request: let the reply arrive, then discard it with the rest of the input */
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_old);
    g_raw = 0;
}

static void term_raw(void) {
    struct termios t;
    tcgetattr(STDIN_FILENO, &g_old);
    t = g_old;
    t.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
    t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);
    g_raw = 1;
    atexit(term_restore);
    const char *on = "\x1b[?1049h\x1b[?25l\x1b[0m\x1b[2J\x1b[H";
    if (write(STDOUT_FILENO, on, strlen(on)) < 0) { }
}

static int stdin_ready(double wait) {
    fd_set fds;
    struct timeval tv = {(long)wait, (int)((wait - (long)wait) * 1e6)};
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    return select(STDIN_FILENO + 1, &fds, NULL, NULL, wait > 0 ? &tv : &(struct timeval){0, 0}) > 0;
}

/* Asks the terminal two things: its background colour (OSC 11) and whether it speaks the kitty graphics
 * protocol. The reply to the device-attributes request that follows marks the end of the answers. */
static int detect(Col *bg) {
    const char *q = "\x1b]11;?\x1b\\\x1b_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\x1b\\\x1b[c";
    char buf[512];
    size_t n = 0;
    if (write(STDOUT_FILENO, q, strlen(q)) < 0) return OUT_CELLS;
    double until = wall() + 0.5;
    while (wall() < until && n < sizeof buf - 1) {
        if (!stdin_ready(until - wall())) break;
        ssize_t k = read(STDIN_FILENO, buf + n, sizeof buf - 1 - n);
        if (k <= 0) break;
        n += (size_t)k;
        buf[n] = 0;
        const char *da = strstr(buf, "\x1b[?");
        if (da && strchr(da, 'c')) break;
    }
    buf[n] = 0;
    const char *c = strstr(buf, "]11;rgb:");
    if (c) { /* rgb:RRRR/GGGG/BBBB, two or four hex digits each */
        float v[3] = {0, 0, 0};
        c += 8;
        for (int i = 0; i < 3; i++) {
            char *end;
            unsigned long x = strtoul(c, &end, 16);
            int digits = (int)(end - c);
            if (digits < 1 || digits > 4) break;
            v[i] = (float)x / (float)((1ul << (4 * digits)) - 1);
            c = *end == '/' ? end + 1 : end;
        }
        *bg = (Col){v[0], v[1], v[2]};
    }
    if (getenv("TMUX")) return OUT_CELLS;
    if (strstr(buf, "_Gi=31;OK")) return OUT_KITTY;
    const char *tp = getenv("TERM_PROGRAM"), *lc = getenv("LC_TERMINAL");
    if ((tp && !strcmp(tp, "iTerm.app")) || (lc && !strcmp(lc, "iTerm2"))) return OUT_ITERM;
    return OUT_CELLS;
}

static void cursor_to(Buf *b, int row) {
    char t[32];
    put(b, t, (size_t)snprintf(t, sizeof t, "\x1b[%d;1H", row));
}

/* ---- output: kitty graphics protocol. `id` names the picture; sending it again replaces it ------- */
static void out_kitty(Buf *b, const uint8_t *rgb, int w, int h, int cols, int rows, int row0, int id) {
    static uint8_t *z;
    static char *b64;
    static size_t zcap;
    size_t raw = (size_t)w * (size_t)h * 3;
    uLongf zl = compressBound((uLong)raw);
    if (zl > zcap) { zcap = zl; z = realloc(z, zcap); b64 = realloc(b64, zcap * 4 / 3 + 8); }
    compress2(z, &zl, rgb, (uLong)raw, 1);
    size_t n = base64(z, zl, b64), off = 0;
    char head[160];
    cursor_to(b, row0);
    while (off < n) {
        size_t chunk = n - off > 4096 ? 4096 : n - off;
        int more = off + chunk < n;
        if (off == 0) snprintf(head, sizeof head, "\x1b_Ga=T,i=%d,p=1,q=2,f=24,o=z,s=%d,v=%d,c=%d,r=%d,C=1,m=%d;", id, w, h, cols, rows, more);
        else snprintf(head, sizeof head, "\x1b_Gm=%d;", more);
        puts_(b, head);
        put(b, b64 + off, chunk);
        puts_(b, "\x1b\\");
        off += chunk;
    }
}

/* ---- output: iTerm2 inline image (a PNG built with zlib) ---------------------------------------- */
static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static size_t png_chunk(uint8_t *out, const char *type, const uint8_t *data, size_t n) {
    be32(out, (uint32_t)n);
    memcpy(out + 4, type, 4);
    if (n) memcpy(out + 8, data, n);
    uLong crc = crc32(0, (const Bytef *)type, 4);
    if (n) crc = crc32(crc, data, (uInt)n);
    be32(out + 8 + n, (uint32_t)crc);
    return n + 12;
}

static void out_iterm(Buf *b, const uint8_t *rgb, int w, int h, int cols, int rows, int row0) {
    static uint8_t *scan, *z, *png;
    static char *b64;
    static size_t cap;
    size_t stride = (size_t)w * 3 + 1, raw = stride * (size_t)h;
    uLongf zl = compressBound((uLong)raw);
    if (zl + 64 > cap) {
        cap = zl + 64;
        scan = realloc(scan, raw + cap); z = realloc(z, cap); png = realloc(png, cap + 64); b64 = realloc(b64, (cap + 64) * 4 / 3 + 8);
    }
    for (int y = 0; y < h; y++) { /* each scanline starts with its filter type: 0, none */
        scan[stride * (size_t)y] = 0;
        memcpy(scan + stride * (size_t)y + 1, rgb + (size_t)y * (size_t)w * 3, (size_t)w * 3);
    }
    compress2(z, &zl, scan, (uLong)raw, 1);
    uint8_t ihdr[13] = {0};
    be32(ihdr, (uint32_t)w); be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = 2; /* 8 bits per channel, RGB */
    size_t size = 8;
    memcpy(png, "\x89PNG\r\n\x1a\n", 8);
    size += png_chunk(png + size, "IHDR", ihdr, 13);
    size += png_chunk(png + size, "IDAT", z, zl);
    size += png_chunk(png + size, "IEND", NULL, 0);
    size_t n = base64(png, size, b64);
    char head[160];
    cursor_to(b, row0);
    snprintf(head, sizeof head, "\x1b]1337;File=inline=1;size=%zu;width=%d;height=%d;preserveAspectRatio=0:", size, cols, rows);
    puts_(b, head);
    put(b, b64, n);
    puts_(b, "\a");
}

/* ---- output: half blocks. One cell is an upper-half block: fg is the top pixel, bg the bottom.
 * `slot` keeps the previous frame of each picture so only the cells that changed are sent. ------- */
static void out_cells(Buf *b, const uint8_t *rgb, int w, int h, int fresh, int row0, int slot) {
    static uint8_t *prevs[MAX_SESS + 1];
    static int pws[MAX_SESS + 1], phs[MAX_SESS + 1];
    int rows = h / 2;
    if (pws[slot] != w || phs[slot] != h) { free(prevs[slot]); prevs[slot] = malloc((size_t)w * (size_t)h * 3); pws[slot] = w; phs[slot] = h; fresh = 1; }
    uint8_t *prev = prevs[slot];
    int fg = -1, bg = -1;
    char t[64];
    for (int r = 0; r < rows; r++) {
        const uint8_t *top = rgb + (size_t)(2 * r) * (size_t)w * 3, *bot = top + (size_t)w * 3;
        const uint8_t *ptop = prev + (size_t)(2 * r) * (size_t)w * 3, *pbot = ptop + (size_t)w * 3;
        int x = 0;
        while (x < w) {
            if (!fresh && !memcmp(top + x * 3, ptop + x * 3, 3) && !memcmp(bot + x * 3, pbot + x * 3, 3)) { x++; continue; }
            put(b, t, (size_t)snprintf(t, sizeof t, "\x1b[%d;%dH", row0 + r, x + 1));
            while (x < w && (fresh || memcmp(top + x * 3, ptop + x * 3, 3) || memcmp(bot + x * 3, pbot + x * 3, 3))) { /* one run of changed cells */
                int f = top[x * 3] << 16 | top[x * 3 + 1] << 8 | top[x * 3 + 2], g = bot[x * 3] << 16 | bot[x * 3 + 1] << 8 | bot[x * 3 + 2];
                if (f != fg) { put(b, t, (size_t)snprintf(t, sizeof t, "\x1b[38;2;%d;%d;%dm", f >> 16, f >> 8 & 255, f & 255)); fg = f; }
                if (g != bg) { put(b, t, (size_t)snprintf(t, sizeof t, "\x1b[48;2;%d;%d;%dm", g >> 16, g >> 8 & 255, g & 255)); bg = g; }
                puts_(b, "\xe2\x96\x80");
                x++;
            }
        }
    }
    puts_(b, "\x1b[0m");
    memcpy(prev, rgb, (size_t)w * (size_t)h * 3);
}

/* ---- the list layout's text: a coloured dot and the folder name, then the state in grey ---------- */
static void list_text(Buf *b, const Session *s, double now, int row, int cols) {
    char t[160], state[96];
    int st = session_shown(s, now), room_for = cols - 4;
    if (room_for < 1) return;
    const Col *c = &s->cur.eh; /* the dot follows the shell's glow colour as it eases between states */
    if (st == ST_WORK && s->tool[0]) snprintf(state, sizeof state, "%s \xc2\xb7 %s", STATE_TEXT[st], s->tool);
    else snprintf(state, sizeof state, "%s", !strcmp(s->sid, "ghost") ? "Waiting for Claude Code" : STATE_TEXT[st]);
    cursor_to(b, row);
    put(b, t, (size_t)snprintf(t, sizeof t, "\x1b[0m\x1b[2K \x1b[38;2;%d;%d;%dm\xe2\x97\x8f\x1b[38;2;226;229;236m %.*s",
                               (int)(c->r * 255), (int)(c->g * 255), (int)(c->b * 255), room_for, s->name[0] ? s->name : "claude"));
    cursor_to(b, row + 1);
    put(b, t, (size_t)snprintf(t, sizeof t, "\x1b[0m\x1b[2K\x1b[38;2;132;137;150m   %.*s\x1b[0m", room_for + 1, state));
}

static void usage(void) {
    puts("dodeca-core: one glowing polyhedron per running Claude Code session\n"
         "\n"
         "  dodeca-core [options]\n"
         "\n"
         "  --pane            side-pane layout: sessions top to bottom, text first, then the polyhedron\n"
         "  --exit-when-empty quit once no session has been running for 15 seconds\n"
         "  --demo [N]        made-up sessions instead of the events file (1-6, default 3)\n"
         "  --shape NAME      dodeca | bucky | geo2 (default) | geo3\n"
         "  --kitty | --iterm | --cells   force how the picture is sent (default: ask the terminal)\n"
         "  --res N           longest side of each rendered picture in pixels (default 960, or 420 with --pane)\n"
         "  --fps N           most frames per second (default 20; calm sessions are redrawn less often)\n"
         "  --glow X          halo strength (default 1.0)\n"
         "  --core X          core radius as a fraction of the shell's inradius (default 0.35)\n"
         "  --events PATH     events file (default ~/.dodeca-core/events.jsonl, or $DODECA_EVENTS)\n"
         "  --snapshot FILE   render one demo frame to a PPM file and exit (with --at SECONDS, --size WxH)\n"
         "\n"
         "  q quits.");
}

int main(int argc, char **argv) {
    int out = OUT_AUTO, layout = LAYOUT_GRID, res = 0, fps = 20, demo_n = 0, snap_w = 900, snap_h = 500, exit_empty = 0;
    float glow = 1.0f, core_frac = 0.35f;
    double snap_at = 8.0;
    const char *shape_name = "geo2", *snapshot = NULL, *events = getenv("DODECA_EVENTS"), *home = getenv("HOME") ? getenv("HOME") : ".";
    char events_default[1024];
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--demo")) { demo_n = 3; if (v && v[0] >= '1' && v[0] <= '6' && !v[1]) { demo_n = v[0] - '0'; i++; } }
        else if (!strcmp(a, "--pane")) layout = LAYOUT_LIST;
        else if (!strcmp(a, "--exit-when-empty")) exit_empty = 1;
        else if (!strcmp(a, "--kitty")) out = OUT_KITTY;
        else if (!strcmp(a, "--iterm")) out = OUT_ITERM;
        else if (!strcmp(a, "--cells")) out = OUT_CELLS;
        else if (!strcmp(a, "--shape") && v) { shape_name = v; i++; }
        else if (!strcmp(a, "--res") && v) { res = atoi(v); i++; }
        else if (!strcmp(a, "--fps") && v) { fps = atoi(v); i++; }
        else if (!strcmp(a, "--glow") && v) { glow = (float)atof(v); i++; }
        else if (!strcmp(a, "--core") && v) { core_frac = (float)atof(v); i++; }
        else if (!strcmp(a, "--events") && v) { events = v; i++; }
        else if (!strcmp(a, "--snapshot") && v) { snapshot = v; i++; }
        else if (!strcmp(a, "--at") && v) { snap_at = atof(v); i++; }
        else if (!strcmp(a, "--size") && v) { if (sscanf(v, "%dx%d", &snap_w, &snap_h) != 2) { usage(); return 2; } i++; }
        else { usage(); return strcmp(a, "--help") && strcmp(a, "-h") ? 2 : 0; }
    }
    if (res == 0) res = layout == LAYOUT_LIST ? 420 : 960;
    if (res < 64) res = 64;
    if (fps < 1) fps = 1;
    if (fps > 60) fps = 60;
    if (!events) {
        snprintf(events_default, sizeof events_default, "%s/.dodeca-core/events.jsonl", home);
        events = events_default;
    }

    Shape shape;
    if (!shape_make(&shape, shape_name)) { fprintf(stderr, "unknown shape: %s\n", shape_name); return 2; }

    static Room room, ghost;
    static Event ev[1024];
    Canvas cv = {0, 0, NULL};
    Demo demo = {0};
    Col bg = {0, 0, 0};
    uint8_t *rgb = NULL;
    size_t rgb_cap = 0;

    /* --snapshot: run the demo on a made-up clock and write one frame */
    if (snapshot) {
        double t0 = 1000.0, dt = 1.0 / 24;
        demo_start(&demo, demo_n ? demo_n : 3, t0);
        canvas_resize(&cv, snap_w, snap_h);
        for (double now = t0; now <= t0 + snap_at; now += dt) { demo_run(&demo, &room, now); room_tick(&room, &shape, now, (float)dt); }
        if (layout == LAYOUT_LIST) render_one(&cv, &room.s[0], &shape, t0 + snap_at, glow, core_frac);
        else render_room(&cv, &room, &shape, t0 + snap_at, glow, core_frac);
        rgb = malloc((size_t)snap_w * (size_t)snap_h * 3);
        canvas_bytes(&cv, rgb, bg);
        FILE *f = fopen(snapshot, "wb");
        if (!f) { perror(snapshot); return 1; }
        fprintf(f, "P6\n%d %d\n255\n", snap_w, snap_h);
        fwrite(rgb, 1, (size_t)snap_w * (size_t)snap_h * 3, f);
        fclose(f);
        return 0;
    }

    if (!isatty(STDOUT_FILENO) || !isatty(STDIN_FILENO)) { fprintf(stderr, "dodeca-core needs a terminal\n"); return 1; }
    signal(SIGINT, on_quit);
    signal(SIGTERM, on_quit);
    signal(SIGHUP, on_quit);
    signal(SIGWINCH, on_winch);
    term_raw();
    int found = detect(&bg);
    if (out == OUT_AUTO) out = found;

    if (layout == LAYOUT_LIST && !demo_n) { /* lets the launcher see that a pane is already open */
        char dir[900];
        snprintf(dir, sizeof dir, "%.800s/.dodeca-core", home);
        mkdir(dir, 0700);
        snprintf(g_pidfile, sizeof g_pidfile, "%s/pane.pid", dir);
        FILE *f = fopen(g_pidfile, "w");
        if (f) { fprintf(f, "%d\n", (int)getpid()); fclose(f); } else g_pidfile[0] = 0;
    }

    Tail tail;
    double now = wall(), last = now, empty_since = now;
    if (demo_n) demo_start(&demo, demo_n, now);
    else {
        tail_init(&tail, events);
        int n = tail_history(&tail, now - 1800, ev, 1024); /* rebuild what the sessions were doing */
        for (int i = 0; i < n; i++) room_apply(&room, &ev[i]);
    }
    session_init(&ghost.s[0], "ghost", now, &shape); /* shown while no session is running */
    snprintf(ghost.s[0].name, sizeof ghost.s[0].name, layout == LAYOUT_LIST ? "No session" : "NO SESSION");
    ghost.s[0].state = ST_SLEEP;
    ghost.n = 1;

    Buf buf = {NULL, 0, 0};
    int fresh = 1, last_w = 0, last_h = 0, key_layout = -1;
    double due[MAX_SESS + 1] = {0}, started = wall(), t_render = 0, t_write = 0;
    long loops = 0, pictures = 0;
    size_t bytes = 0;
    while (!g_quit) {
        double frame_start = wall();
        now = frame_start;
        float dt = (float)(now - last);
        if (dt > 0.25f) dt = 0.25f;
        last = now;
        loops++;

        char key;
        while (stdin_ready(0) && read(STDIN_FILENO, &key, 1) == 1) if (key == 'q' || key == 'Q') g_quit = 1;

        if (demo_n) demo_run(&demo, &room, now);
        else {
            int n = tail_poll(&tail, ev, 1024);
            for (int i = 0; i < n; i++) room_apply(&room, &ev[i]);
        }
        room_tick(&room, &shape, now, dt);
        ghost.s[0].last = now;
        room_tick(&ghost, &shape, now, dt);
        if (room.n) empty_since = now;
        if (exit_empty && now - empty_since > 15) break;

        struct winsize ws = {0};
        ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
        int cols = ws.ws_col ? ws.ws_col : 80, rows = ws.ws_row ? ws.ws_row : 24;
        float cw = ws.ws_xpixel ? (float)ws.ws_xpixel / cols : 8.0f, ch = ws.ws_ypixel ? (float)ws.ws_ypixel / rows : 17.0f;
        int usable = out == OUT_ITERM && rows > 1 ? rows - 1 : rows; /* the cursor ends below an inline image: keep it on screen */
        const Room *shown = room.n ? &room : &ghost;

        /* the list layout: how many sessions fit, and how many rows each polyhedron gets (square at most) */
        int nvis = 1, srows = usable;
        if (layout == LAYOUT_LIST) {
            int square = (int)(cols * cw / ch + 0.5f);
            nvis = shown->n;
            while (nvis > 1 && usable / nvis < 6) nvis--;
            srows = usable / nvis - 3; /* two lines of text above and a blank line below */
            if (srows > square) srows = square;
            if (srows < 1) srows = 1;
        }
        int k = ((cols * 1000 + rows) * 20 + nvis) * 100 + srows;
        if (g_winch || k != key_layout) { /* the arrangement changed: start from an empty screen */
            g_winch = 0; key_layout = k; fresh = 1;
            puts_(&buf, "\x1b_Ga=d,d=A,q=2\x1b\\\x1b[0m\x1b[2J");
        }
        puts_(&buf, "\x1b[?2026h"); /* synchronized update: nothing shows until the frame is complete */
        for (int i = 0; i < nvis; i++) {
            int row0 = 1, pic_rows = usable, w, h;
            if (layout == LAYOUT_LIST) {
                int top = 1 + i * (srows + 3);
                list_text(&buf, &shown->s[i], now, top, cols);
                row0 = top + 2; pic_rows = srows;
            }
            double rate = session_rate(&shown->s[i], now, fps);
            if (layout == LAYOUT_GRID) for (int j = 1; j < shown->n; j++) { double r = session_rate(&shown->s[j], now, fps); if (r > rate) rate = r; }
            if (!fresh && now < due[i]) continue; /* this picture is not due yet */
            due[i] = now + 1.0 / rate - 0.5 / fps;
            double t0 = wall();
            if (out == OUT_CELLS) { w = cols; h = pic_rows * 2; }
            else {
                float pw = cols * cw, ph = pic_rows * ch, sc = (float)res / (pw > ph ? pw : ph);
                if (sc > 2) sc = 2; /* up to twice the reported size: a Retina screen has two device pixels per reported pixel */
                w = (int)(pw * sc); h = (int)(ph * sc);
                if (w < 16) w = 16;
                if (h < 16) h = 16;
            }
            canvas_resize(&cv, w, h);
            last_w = w; last_h = h;
            if (layout == LAYOUT_LIST) render_one(&cv, &shown->s[i], &shape, now, glow, core_frac);
            else render_room(&cv, shown, &shape, now, glow, core_frac);
            if ((size_t)w * (size_t)h * 3 > rgb_cap) { rgb_cap = (size_t)w * (size_t)h * 3; rgb = realloc(rgb, rgb_cap); }
            canvas_bytes(&cv, rgb, bg);
            t_render += wall() - t0;
            pictures++;
            if (out == OUT_KITTY) out_kitty(&buf, rgb, w, h, cols, pic_rows, row0, i + 1);
            else if (out == OUT_ITERM) out_iterm(&buf, rgb, w, h, cols, pic_rows, row0);
            else out_cells(&buf, rgb, w, h, fresh, row0, i);
        }
        puts_(&buf, "\x1b[?2026l");
        fresh = 0;
        double t1 = wall();
        bytes += flush(&buf);
        t_write += wall() - t1;

        double wait = frame_start + 1.0 / fps - wall();
        if (wait > 0) stdin_ready(wait); /* sleeps until the next frame, or until a key arrives */
    }
    term_restore();
    double secs = wall() - started;
    if (secs < 0.001) secs = 0.001;
    fprintf(stderr, "dodeca-core: mode %s, picture %dx%d\n", out == OUT_KITTY ? "kitty" : out == OUT_ITERM ? "iterm" : "cells", last_w, last_h);
    fprintf(stderr, "  %.1f loops/s, %.1f pictures/s, %.2f MB/s to the terminal\n", loops / secs, pictures / secs, bytes / secs / 1e6);
    fprintf(stderr, "  per picture: render %.1f ms; per loop: waiting on the terminal %.1f ms\n", pictures ? t_render / pictures * 1e3 : 0.0, loops ? t_write / loops * 1e3 : 0.0);
    return 0;
}

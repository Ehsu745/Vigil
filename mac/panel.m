// vigil-panel: the same polyhedra as vigil, in a floating panel on the right edge of the screen.
//
// The panel has no title bar, never takes the keyboard, lets clicks through (except on a core, see
// trackMouse:), and is see-through where the picture is dark, so it can sit over a full-screen iTerm2
// window: an empty terminal pane underneath keeps that part of the screen free. When started inside that pane (open_pane.sh does this) it sizes itself to
// the pane and quits when the pane is closed.
//
// Build: sh mac/build.sh. That makes Vigil.app and the launcher script ./vigil-panel, which starts the
// app through LaunchServices (so macOS asks for microphone and speech permission on behalf of the app, not
// the terminal) and stays in the pane: it passes the pane's terminal with --tty and its own pid with
// --launcher-pid, and the app quits when the launcher does (the pane was closed, or Ctrl-C).
#import <Cocoa/Cocoa.h>
#import <Carbon/Carbon.h> // RegisterEventHotKey: global hotkeys without the Accessibility permission
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>
#include "../src/vigil.h"
#import "speech.h"
#import "listen.h"
#import "tip.h"
#import "reply.h"
#import "settings.h"

static Room g_room, g_ghost;
static Shape g_shape;
static Tail g_tail;
static Event g_ev[1024];
static Demo g_demo;
static int g_demo_n, g_exit_empty, g_always, g_fit_pane, g_all_spaces;
static const char *g_reply_shot; // --reply-shot FILE: open a reply box, save it with its tooltip as a PNG, quit
static float g_glow = 1.0f, g_core = 0.35f;
// Settings window (Look, Motion, Replies): how big the cores are, the small cores, the text box's look
static float g_scale = 1, g_tip_size = 1;       // Size on screen; Box size
static int g_others = 1;                        // single-core mode: the other sessions as small cores around the main one
static int g_tip_mono, g_tip_middle;            // the text box in the terminal's face; in the middle of the screen
static int g_tip_bg, g_text_col;                // indexes into TIP_BGS / TEXT_COLS (5: the custom colour)
static unsigned g_tip_custom = 0x18191E, g_text_custom = 0xE6E8F0;
static int g_wheel_spin;                        // what ⌥-scroll on a core does: roll, roll back, turn, turn back, off
static const char *g_settings_shot;             // --settings-shot FILE: open the Settings window, save it as a PNG, quit
static int g_settings_open;                     // --settings: open it at start
static CGFloat g_width, g_top = 40, g_inset = 30, g_font_size = 12;
static NSString *g_font_name = @"Monaco";
static NSColor *g_bg;
static char g_pidfile[1024], g_msg_dir[1024];
static struct termios g_old_tty;
static int g_tty = -1;          // the pane's terminal while we keep it blank (stdout, or --tty), else -1
static int g_launcher_pid;      // --launcher-pid: quit when this process ends
static const char *g_tty_path;  // --tty
static double g_away_after = 120;               // --away: seconds without keyboard or mouse input until "away"
static const char *g_hotkey = "ctrl+opt+v";     // --hotkey: push-to-talk to the session that most needs you
static NSString *g_voice_id = @"system";         // --voice-id: "system" (System Settings' voice, Siri too), a language or an identifier
static float g_rate = 0.5f;                     // --rate: 0 (slowest) .. 1 (fastest), 0.5 is normal
static int g_voice;                             // --voice: spoken reports and push-to-talk (off: the panel stays silent)
static int g_auto_speak, g_summarize_claude;    // --auto-speak, --summarize claude (both imply --voice)
static NSString *g_claude;                      // --claude PATH (the launcher passes it), else found below
static NSString *g_listen_locale = @"zh-TW";     // --listen-locale: the language push-to-talk understands
static int g_confirm_send;                      // --confirm-send: a recognized reply waits for a second click
static NSString *g_mic;                         // --mic NAME: the input device to listen with
static const double PREVIEW_FOR = 1.5;          // seconds a recognized reply is shown before it is sent
static int g_debug;
static int g_labels_hover;                      // --labels hover: no name / state lines, only the cores (and the tooltip)
// single-core mode (--single, or the mode hotkey): one big core for one session, the others float around it
static int g_single;
static const char *g_mode_hotkey = "ctrl+opt+c";  // --mode-hotkey: switch between single core and all cores
static char g_shape_name[16] = "geo2";          // --shape
static char g_frame[16] = "segments";           // --frame: drawn round a core while it is carried or locked on
static NSString *g_focus;                       // the session in the middle
static BOOL g_focus_pinned;                     // chosen by a click: auto-follow is off until "Follow automatically"                             // --debug: log each voice step to stderr (never the words)
#define DBG(...) do { if (g_debug) { fprintf(stderr, "[vigil] " __VA_ARGS__); fputc('\n', stderr); fflush(stderr); } } while (0)

static const Room *shown_room(void) { return g_room.n ? &g_room : &g_ghost; }

static Session *session_by_sid(NSString *sid) {
    Room *r = g_room.n ? &g_room : &g_ghost;
    for (int i = 0; sid && i < r->n; i++) if ([sid isEqualToString:@(r->s[i].sid)]) return &r->s[i];
    return NULL;
}

static NSString *sid_of(const Session *s) { return [NSString stringWithUTF8String:s->sid] ?: @""; }

// Single-core mode: where the cluster was put with ⌥-drag, kept in ~/.dodeca-core/panel-position.json per
// screen size (another monitor setup has its own spot, so it never comes back off-screen).
static NSString *position_file(void) { return [@"~/.dodeca-core/panel-position.json" stringByExpandingTildeInPath]; }
static NSString *screen_key(NSScreen *sc) { return [NSString stringWithFormat:@"%.0fx%.0f", sc.frame.size.width, sc.frame.size.height]; }
static NSMutableDictionary *positions(void) {
    NSData *d = [NSData dataWithContentsOfFile:position_file()];
    id j = d ? [NSJSONSerialization JSONObjectWithData:d options:NSJSONReadingMutableContainers error:NULL] : nil;
    return [j isKindOfClass:[NSMutableDictionary class]] ? j : [NSMutableDictionary dictionary];
}
static void save_position(NSScreen *sc, NSPoint origin) {
    NSMutableDictionary *all = positions();
    if (sc) {
        if (origin.x == CGFLOAT_MAX) [all removeObjectForKey:screen_key(sc)];
        else all[screen_key(sc)] = @[@(origin.x), @(origin.y)];
    }
    [[NSFileManager defaultManager] createDirectoryAtPath:[position_file() stringByDeletingLastPathComponent]
                              withIntermediateDirectories:YES attributes:@{NSFilePosixPermissions: @0700} error:NULL];
    NSData *out = [NSJSONSerialization dataWithJSONObject:all options:0 error:NULL];
    [out writeToFile:position_file() atomically:YES];
    chmod(position_file().fileSystemRepresentation, 0600);
}

// Hides the mouse pointer while a core is carried in its place. A background app may only do that once it
// has asked the window server with the (private, but long-standing) SetsCursorInBackground property; looked
// up at run time, so without it the pointer simply stays visible.
static void pointer_visible(BOOL on) {
    static BOOL hidden, asked;
    if (on != hidden) return;
    if (!asked) {
        asked = YES;
        int (*conn)(void) = (int (*)(void))dlsym(RTLD_DEFAULT, "_CGSDefaultConnection");
        int (*prop)(int, int, CFStringRef, CFTypeRef) = (int (*)(int, int, CFStringRef, CFTypeRef))dlsym(RTLD_DEFAULT, "CGSSetConnectionProperty");
        if (conn && prop) prop(conn(), conn(), CFSTR("SetsCursorInBackground"), kCFBooleanTrue);
    }
    if (on) CGDisplayShowCursor(kCGDirectMainDisplay);
    else CGDisplayHideCursor(kCGDirectMainDisplay);
    hidden = !on;
}

// The right-click Settings menu: what it changes is kept in ~/.dodeca-core/panel-settings.json (600) and
// wins over the launch options; "Use launch options again" forgets it. g_launch is what the options said.
static struct {
    int voice, auto_speak, summarize, confirm_send, labels_hover; CGFloat font_size; float glow; char shape[16], frame[16]; double away;
    float core, scale, tip_size; int others, tip_mono, tip_middle, tip_bg, text_col, wheel_spin; unsigned tip_custom, text_custom;
} g_launch;
static NSString *settings_file(void) { return [@"~/.dodeca-core/panel-settings.json" stringByExpandingTildeInPath]; }
static void remember_launch(void) {
    g_launch.voice = g_voice; g_launch.auto_speak = g_auto_speak; g_launch.summarize = g_summarize_claude; g_launch.confirm_send = g_confirm_send;
    g_launch.labels_hover = g_labels_hover; g_launch.font_size = g_font_size; g_launch.glow = g_glow; g_launch.away = g_away_after;
    g_launch.core = g_core; g_launch.scale = g_scale; g_launch.tip_size = g_tip_size; g_launch.others = g_others; g_launch.tip_mono = g_tip_mono;
    g_launch.tip_middle = g_tip_middle; g_launch.tip_bg = g_tip_bg; g_launch.text_col = g_text_col; g_launch.wheel_spin = g_wheel_spin;
    g_launch.tip_custom = g_tip_custom; g_launch.text_custom = g_text_custom;
    snprintf(g_launch.shape, sizeof g_launch.shape, "%s", g_shape_name);
    snprintf(g_launch.frame, sizeof g_launch.frame, "%s", g_frame);
}
static void restore_launch(void) {
    g_voice = g_launch.voice; g_auto_speak = g_launch.auto_speak; g_summarize_claude = g_launch.summarize; g_confirm_send = g_launch.confirm_send;
    g_labels_hover = g_launch.labels_hover; g_font_size = g_launch.font_size; g_glow = g_launch.glow; g_away_after = g_launch.away;
    g_core = g_launch.core; g_scale = g_launch.scale; g_tip_size = g_launch.tip_size; g_others = g_launch.others; g_tip_mono = g_launch.tip_mono;
    g_tip_middle = g_launch.tip_middle; g_tip_bg = g_launch.tip_bg; g_text_col = g_launch.text_col; g_wheel_spin = g_launch.wheel_spin;
    g_tip_custom = g_launch.tip_custom; g_text_custom = g_launch.text_custom;
    snprintf(g_shape_name, sizeof g_shape_name, "%s", g_launch.shape);
    snprintf(g_frame, sizeof g_frame, "%s", g_launch.frame);
}
static void load_settings(void) {
    NSData *d = [NSData dataWithContentsOfFile:settings_file()];
    NSDictionary *j = d ? [NSJSONSerialization JSONObjectWithData:d options:0 error:NULL] : nil;
    if (![j isKindOfClass:[NSDictionary class]]) return;
    NSNumber *n;
    if ((n = j[@"voice"])) g_voice = n.boolValue;
    if ((n = j[@"autoSpeak"])) g_auto_speak = n.boolValue;
    if ((n = j[@"summarize"])) g_summarize_claude = n.boolValue;
    if ((n = j[@"confirmSend"])) g_confirm_send = n.boolValue;
    if ((n = j[@"labelsHover"])) g_labels_hover = n.boolValue;
    if ((n = j[@"fontSize"]) && n.doubleValue >= 6) g_font_size = n.doubleValue;
    if ((n = j[@"glow"]) && n.floatValue > 0) g_glow = n.floatValue;
    if ((n = j[@"away"])) g_away_after = n.doubleValue;
    if ((n = j[@"core"]) && n.floatValue > 0.05f && n.floatValue < 0.9f) g_core = n.floatValue;
    if ((n = j[@"scale"]) && n.floatValue >= 0.5f && n.floatValue <= 2.5f) g_scale = n.floatValue;
    if ((n = j[@"tipSize"]) && n.floatValue >= 0.5f && n.floatValue <= 2) g_tip_size = n.floatValue;
    if ((n = j[@"others"])) g_others = n.boolValue;
    if ((n = j[@"tipMono"])) g_tip_mono = n.boolValue;
    if ((n = j[@"tipMiddle"])) g_tip_middle = n.boolValue;
    if ((n = j[@"tipColor"]) && n.intValue >= 0 && n.intValue < 6) g_tip_bg = n.intValue;
    if ((n = j[@"textColor"]) && n.intValue >= 0 && n.intValue < 6) g_text_col = n.intValue;
    if ((n = j[@"tipCustom"]) && n.unsignedIntValue <= 0xFFFFFF) g_tip_custom = n.unsignedIntValue;
    if ((n = j[@"textCustom"]) && n.unsignedIntValue <= 0xFFFFFF) g_text_custom = n.unsignedIntValue;
    if ((n = j[@"wheelSpin"]) && n.intValue >= 0 && n.intValue < 5) g_wheel_spin = n.intValue;
    NSString *fr = j[@"frame"];
    if ([fr isKindOfClass:[NSString class]] && fr.length < sizeof g_frame) snprintf(g_frame, sizeof g_frame, "%s", fr.UTF8String);
    NSString *sh = j[@"shape"];
    if ([sh isKindOfClass:[NSString class]] && sh.length < sizeof g_shape_name) snprintf(g_shape_name, sizeof g_shape_name, "%s", sh.UTF8String);
}
static void save_settings(void) {
    NSDictionary *j = @{@"voice": @(g_voice != 0), @"autoSpeak": @(g_auto_speak != 0), @"summarize": @(g_summarize_claude != 0),
                        @"confirmSend": @(g_confirm_send != 0), @"labelsHover": @(g_labels_hover != 0), @"fontSize": @(g_font_size),
                        @"glow": @(g_glow), @"shape": @(g_shape_name), @"away": @(g_away_after), @"frame": @(g_frame),
                        @"core": @(g_core), @"scale": @(g_scale), @"tipSize": @(g_tip_size), @"others": @(g_others != 0),
                        @"tipMono": @(g_tip_mono != 0), @"tipMiddle": @(g_tip_middle != 0), @"tipColor": @(g_tip_bg), @"textColor": @(g_text_col),
                        @"tipCustom": @(g_tip_custom), @"textCustom": @(g_text_custom), @"wheelSpin": @(g_wheel_spin)};
    [[NSFileManager defaultManager] createDirectoryAtPath:[settings_file() stringByDeletingLastPathComponent]
                              withIntermediateDirectories:YES attributes:@{NSFilePosixPermissions: @0700} error:NULL];
    [[NSJSONSerialization dataWithJSONObject:j options:NSJSONWritingPrettyPrinted error:NULL] writeToFile:settings_file() atomically:YES];
    chmod(settings_file().fileSystemRepresentation, 0600);
}

// ---- the focus frame: drawn round a core while it is carried, or after the pointer has rested on it 2 s ----
static void stroke2(CGContextRef ctx, CGPathRef path, CGColorRef col, CGColorRef glow, CGFloat w, CGFloat gw) {
    if (glow) { CGContextSetStrokeColorWithColor(ctx, glow); CGContextSetLineWidth(ctx, gw); CGContextAddPath(ctx, path); CGContextStrokePath(ctx); }
    CGContextSetStrokeColorWithColor(ctx, col); CGContextSetLineWidth(ctx, w); CGContextAddPath(ctx, path); CGContextStrokePath(ctx);
}
static void tick_mark(CGMutablePathRef p, CGFloat ox, CGFloat oy, double g, CGFloat r0, CGFloat r1) {
    CGPathMoveToPoint(p, NULL, ox + cos(g) * r0, oy + sin(g) * r0); CGPathAddLineToPoint(p, NULL, ox + cos(g) * r1, oy + sin(g) * r1);
}
static void focus_frame(CGContextRef ctx, NSRect r, const Session *s, double t) {
    float a = fmaxf(s->mo.lock, s->carry);
    if (a < 0.01f || !strcmp(g_frame, "none")) return;
    if (a > 1) a = 1;
    CGFloat ox = NSMidX(r), oy = NSMidY(r), shell = r.size.width * SHELL_FRAC, u = 1 - pow(1 - a, 3);
    CGFloat rr = shell * (1.55 - 0.3 * u), k = fmax(0.5, r.size.width / 300.0), TAU = 2 * M_PI;
    Col c = s->cur.line, e = s->cur.eh;
    CGColorRef col = CGColorCreateSRGB(c.r, c.g, c.b, 0.75 * a), faint = CGColorCreateSRGB(c.r, c.g, c.b, 0.25 * a), glow = CGColorCreateSRGB(e.r, e.g, e.b, 0.2 * a);
    CGContextSaveGState(ctx);
    CGContextSetLineCap(ctx, kCGLineCapButt);
    CGMutablePathRef p;
    if (!strcmp(g_frame, "segments")) { // 12 segments with a lit run travelling round, a fine scale inside
        double head = fmod(t * 5, 12);
        for (int i = 0; i < 12; i++) {
            double g1 = i / 12.0 * TAU + M_PI / 2 + 0.05, g2 = g1 + TAU / 12 - 0.1, d = fmod(head - i + 12, 12), on = d < 3 ? 1 - d / 3 : 0;
            p = CGPathCreateMutable(); CGPathAddArc(p, NULL, ox, oy, rr, g1, g2, false);
            stroke2(ctx, p, on > 0 ? col : faint, on > 0 ? glow : NULL, (1.2 + 1.8 * on) * k, 6 * k); CGPathRelease(p);
        }
        p = CGPathCreateMutable();
        for (int i = 0; i < 72; i++) tick_mark(p, ox, oy, i / 72.0 * TAU, rr - 6 * k, rr - (i % 6 ? 9 : 13) * k);
        stroke2(ctx, p, faint, NULL, 1 * k, 0); CGPathRelease(p);
    } else if (!strcmp(g_frame, "lens")) { // two graduated arcs, ( ), with end caps and a long centre mark
        double half = 0.78;
        for (int sd = 0; sd < 2; sd++) {
            double side = sd * M_PI;
            p = CGPathCreateMutable(); CGPathAddArc(p, NULL, ox, oy, rr, side - half, side + half, false); stroke2(ctx, p, col, glow, 1.4 * k, 5 * k); CGPathRelease(p);
            p = CGPathCreateMutable();
            for (int i = -6; i <= 6; i++) if (i) tick_mark(p, ox, oy, side + half * i / 6, rr, rr - (abs(i) == 3 ? 9 : 5) * k);
            tick_mark(p, ox, oy, side, rr + 8 * k, rr + 26 * k);
            stroke2(ctx, p, faint, NULL, 1 * k, 0); CGPathRelease(p);
            p = CGPathCreateMutable();
            tick_mark(p, ox, oy, side, rr, rr - 16 * k); tick_mark(p, ox, oy, side - half, rr - 5 * k, rr + 5 * k); tick_mark(p, ox, oy, side + half, rr - 5 * k, rr + 5 * k);
            stroke2(ctx, p, col, NULL, 1.6 * k, 0); CGPathRelease(p);
        }
    } else if (!strcmp(g_frame, "polar")) { // spokes between two rings, turning slowly, a marker riding outside
        CGFloat r1 = rr - 12 * k;
        p = CGPathCreateMutable(); CGPathAddEllipseInRect(p, NULL, CGRectMake(ox - rr, oy - rr, 2 * rr, 2 * rr)); CGPathAddEllipseInRect(p, NULL, CGRectMake(ox - r1, oy - r1, 2 * r1, 2 * r1));
        stroke2(ctx, p, faint, NULL, 1 * k, 0); CGPathRelease(p);
        CGMutablePathRef minor = CGPathCreateMutable(), major = CGPathCreateMutable();
        for (int i = 0; i < 24; i++) { double g = t * 0.12 + i / 24.0 * TAU; tick_mark(i % 6 ? minor : major, ox, oy, g, r1, i % 6 ? rr : rr + 8 * k); }
        stroke2(ctx, minor, faint, NULL, 1 * k, 0); stroke2(ctx, major, col, glow, 1.5 * k, 5 * k); CGPathRelease(minor); CGPathRelease(major);
        double gm = -t * 0.6;
        CGContextSetFillColorWithColor(ctx, col); CGContextBeginPath(ctx);
        CGContextMoveToPoint(ctx, ox + cos(gm) * (rr + 7 * k), oy + sin(gm) * (rr + 7 * k));
        CGContextAddLineToPoint(ctx, ox + cos(gm - 0.05) * (rr + 16 * k), oy + sin(gm - 0.05) * (rr + 16 * k));
        CGContextAddLineToPoint(ctx, ox + cos(gm + 0.05) * (rr + 16 * k), oy + sin(gm + 0.05) * (rr + 16 * k));
        CGContextClosePath(ctx); CGContextFillPath(ctx);
    } else if (!strcmp(g_frame, "diamond")) { // a square on its point: notched sides, rings at the corners, a dotted inner square
        CGFloat s2 = rr * 1.12; CGPoint V[4];
        for (int i = 0; i < 4; i++) V[i] = CGPointMake(ox + cos(i * M_PI / 2 + M_PI / 2) * s2, oy + sin(i * M_PI / 2 + M_PI / 2) * s2);
        p = CGPathCreateMutable();
        for (int i = 0; i < 4; i++) {
            CGPoint a0 = V[i], b0 = V[(i + 1) % 4];
            for (int h = 0; h < 2; h++) { double f0 = h ? 0.56 : 0.1, f1 = h ? 0.9 : 0.44;
                CGPathMoveToPoint(p, NULL, a0.x + (b0.x - a0.x) * f0, a0.y + (b0.y - a0.y) * f0); CGPathAddLineToPoint(p, NULL, a0.x + (b0.x - a0.x) * f1, a0.y + (b0.y - a0.y) * f1); }
        }
        stroke2(ctx, p, col, glow, 1.4 * k, 5 * k); CGPathRelease(p);
        CGContextSetFillColorWithColor(ctx, col);
        for (int i = 0; i < 4; i++) {
            CGPoint a0 = V[i], b0 = V[(i + 1) % 4];
            CGContextFillEllipseInRect(ctx, CGRectMake((a0.x + b0.x) / 2 - 2.2 * k, (a0.y + b0.y) / 2 - 2.2 * k, 4.4 * k, 4.4 * k));
            CGContextFillEllipseInRect(ctx, CGRectMake(a0.x - 1.6 * k, a0.y - 1.6 * k, 3.2 * k, 3.2 * k));
            p = CGPathCreateMutable(); CGPathAddEllipseInRect(p, NULL, CGRectMake(a0.x - 6 * k, a0.y - 6 * k, 12 * k, 12 * k)); stroke2(ctx, p, faint, NULL, 1 * k, 0); CGPathRelease(p);
        }
        CGFloat dash[2] = {2 * k, 5 * k};
        CGContextSetLineDash(ctx, 0, dash, 2);
        p = CGPathCreateMutable();
        for (int i = 0; i < 4; i++) { CGPoint q = CGPointMake(ox + (V[i].x - ox) * 0.86, oy + (V[i].y - oy) * 0.86); if (i) CGPathAddLineToPoint(p, NULL, q.x, q.y); else CGPathMoveToPoint(p, NULL, q.x, q.y); }
        CGPathCloseSubpath(p); stroke2(ctx, p, faint, NULL, 1 * k, 0); CGPathRelease(p);
        CGContextSetLineDash(ctx, 0, NULL, 0);
    } else if (!strcmp(g_frame, "lock")) { // four corner pieces slide in from outside and lock square
        CGFloat s2 = rr * (0.9 + 0.45 * (1 - u)), l = rr * 0.26;
        int sg[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        CGContextSetFillColorWithColor(ctx, col);
        for (int i = 0; i < 4; i++) {
            CGFloat sx = sg[i][0], sy = sg[i][1], cx = ox + sx * s2, cy = oy + sy * s2;
            p = CGPathCreateMutable(); CGPathMoveToPoint(p, NULL, cx, cy - sy * l); CGPathAddLineToPoint(p, NULL, cx, cy); CGPathAddLineToPoint(p, NULL, cx - sx * l, cy);
            stroke2(ctx, p, col, glow, 1.8 * k, 5 * k); CGPathRelease(p);
            CGContextFillRect(ctx, CGRectMake(cx - 3 * k + sx * 3 * k, cy - 3 * k + sy * 3 * k, 6 * k, 6 * k));
            p = CGPathCreateMutable();
            CGPathMoveToPoint(p, NULL, cx - sx * l * 0.5, cy); CGPathAddLineToPoint(p, NULL, cx - sx * l * 0.5, cy + sy * 6 * k);
            CGPathMoveToPoint(p, NULL, cx, cy - sy * l * 0.5); CGPathAddLineToPoint(p, NULL, cx + sx * 6 * k, cy - sy * l * 0.5);
            stroke2(ctx, p, faint, NULL, 1 * k, 0); CGPathRelease(p);
        }
    } else if (!strcmp(g_frame, "orbits")) { // three short arcs on three radii, each its own way and speed, a bead leading each
        p = CGPathCreateMutable(); CGPathAddEllipseInRect(p, NULL, CGRectMake(ox - rr * 1.07, oy - rr * 1.07, 2 * rr * 1.07, 2 * rr * 1.07)); stroke2(ctx, p, faint, NULL, 1 * k, 0); CGPathRelease(p);
        double O[3][3] = {{1, 0.9, 1.1}, {1.07, -0.6, 0.7}, {1.14, 0.35, 1.6}};
        CGContextSetFillColorWithColor(ctx, col);
        for (int i = 0; i < 3; i++) {
            double g = t * O[i][1] + i * 2.1, rad = rr * O[i][0], a0 = O[i][1] > 0 ? g - O[i][2] : g, a1 = O[i][1] > 0 ? g : g + O[i][2];
            p = CGPathCreateMutable(); CGPathAddArc(p, NULL, ox, oy, rad, a0, a1, false); stroke2(ctx, p, col, glow, 1.4 * k, 5 * k); CGPathRelease(p);
            CGContextFillEllipseInRect(ctx, CGRectMake(ox + cos(g) * rad - 3 * k, oy + sin(g) * rad - 3 * k, 6 * k, 6 * k));
        }
    }
    CGContextRestoreGState(ctx);
    CGColorRelease(col); CGColorRelease(faint); CGColorRelease(glow);
}

static double wall(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + tv.tv_usec / 1e6;
}

static void cleanup(void) {
    if (g_pidfile[0]) { unlink(g_pidfile); g_pidfile[0] = 0; }
    if (g_tty >= 0) {
        const char *on = "\x1b[?25h";
        if (write(g_tty, on, strlen(on)) < 0) { }
        tcsetattr(g_tty, TCSAFLUSH, &g_old_tty);
        g_tty = -1;
    }
}

// the second line under a session's name
static NSString *state_line(const Session *s, int st) {
    if (!strcmp(s->sid, "ghost")) return @"Waiting for Claude Code";
    if (st == ST_WORK && s->tool[0]) return [NSString stringWithFormat:@"%s · %s", STATE_TEXT[st], s->tool];
    return [NSString stringWithUTF8String:STATE_TEXT[st]];
}

// What the hook keeps about a session besides its events (only with DODECA_RICH=1): the msg file
// ~/.dodeca-core/msg/<sid>.json with "reply", "notice", "iterm", "summary" and "t". Read again only when
// the file changes. --demo makes the same up.
static const char *DEMO_REPLY[] = {
    "Refactored the channel model so fading is applied per tap.\nAll 42 simulation tests pass; the BER curve now matches the reference within 0.2 dB.\nNext I can add Doppler if you want.",
    "The lock now debounces the reed switch for 30 ms.\nI also fixed the race where a second unlock arrived before the motor stopped.",
    "整理好今天的筆記了：三個待辦、兩個已完成。\n要不要我把待辦排進明天的計畫？",
    "Bumped the bootloader to v2.3 and rebuilt the image.\nFlash it with `make flash`; the OTA path is unchanged.",
    "Swept 2.40-2.48 GHz: best match at 2.437 GHz, S11 -18 dB.\nThe matching network values are in tune.csv.",
    "Scratch test done. Nothing to keep.",
};
static const char *DEMO_NOTICE = "Claude needs your permission to use Bash";

static NSDictionary *msg_of(const Session *s) {
    static NSMutableDictionary *cache;
    int i;
    if (g_demo_n) {
        if (sscanf(s->sid, "demo-%d", &i) != 1 || i < 0 || i >= 6) return nil;
        return @{@"reply": [NSString stringWithUTF8String:DEMO_REPLY[i]], @"notice": [NSString stringWithUTF8String:DEMO_NOTICE]};
    }
    if (!g_msg_dir[0] || !strcmp(s->sid, "ghost")) return nil;
    char path[1200];
    snprintf(path, sizeof path, "%s/%s.json", g_msg_dir, s->sid);
    struct stat st;
    NSString *key = [NSString stringWithUTF8String:s->sid];
    if (!key) return nil;
    if (!cache) cache = [NSMutableDictionary dictionary];
    if (stat(path, &st) != 0) { [cache removeObjectForKey:key]; return nil; }
    double mtime = st.st_mtimespec.tv_sec + st.st_mtimespec.tv_nsec / 1e9;
    NSArray *hit = cache[key];
    if (hit && [hit[0] doubleValue] == mtime) return hit[1];
    NSData *data = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:path]];
    id d = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:NULL] : nil;
    if (![d isKindOfClass:[NSDictionary class]]) return nil;
    cache[key] = @[@(mtime), d];
    return d;
}

static NSString *msg_text(const Session *s, NSString *field) {
    id v = msg_of(s)[field];
    return [v isKindOfClass:[NSString class]] && [v length] ? v : nil;
}

// Keeps a Claude summary in the msg file, so replaying does not ask again. Written only if the reply is
// still the one that was summarized (the hook clears "summary" whenever a new reply comes in).
static NSMutableDictionary *g_demo_summary; // --demo has no msg files: kept in memory
static void msg_save_summary(const Session *s, NSString *reply, NSString *summary) {
    NSString *sid = sid_of(s);
    if (g_demo_n) { if (!g_demo_summary) g_demo_summary = [NSMutableDictionary dictionary]; g_demo_summary[sid] = @[reply, summary]; return; }
    if (!g_msg_dir[0]) return;
    NSString *path = [NSString stringWithFormat:@"%s/%@.json", g_msg_dir, sid];
    NSData *data = [NSData dataWithContentsOfFile:path];
    NSMutableDictionary *d = data ? [[NSJSONSerialization JSONObjectWithData:data options:NSJSONReadingMutableContainers error:NULL] mutableCopy] : nil;
    if (![d isKindOfClass:[NSMutableDictionary class]] || ![d[@"reply"] isEqual:reply]) return;
    d[@"summary"] = summary;
    NSData *out = [NSJSONSerialization dataWithJSONObject:d options:0 error:NULL];
    NSString *tmp = [path stringByAppendingFormat:@".%d.tmp", (int)getpid()];
    int fd = open(tmp.fileSystemRepresentation, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return;
    BOOL ok = write(fd, out.bytes, out.length) == (ssize_t)out.length;
    close(fd);
    if (ok) rename(tmp.fileSystemRepresentation, path.fileSystemRepresentation); else unlink(tmp.fileSystemRepresentation);
}

static NSString *saved_summary(const Session *s, NSString *reply) {
    if (g_demo_n) { NSArray *a = g_demo_summary[sid_of(s)]; return [a[0] isEqual:reply] ? a[1] : nil; }
    return [msg_text(s, @"reply") isEqual:reply] ? msg_text(s, @"summary") : nil;
}

// ---- the view: for each session two lines of text, then its polyhedron ----------------------------------
// what the panel does when a core is clicked, held, ... (AppDelegate)
@protocol CoreActions
- (void)coreTouched:(NSString *)sid; // any mouse-down on a core, before it is known what it becomes
- (void)coreClicked:(NSString *)sid;
- (void)coreDoubleClicked:(NSString *)sid;
- (void)corePressBegan:(NSString *)sid; // held down longer than PRESS_AFTER: push-to-talk
- (void)corePressEnded:(NSString *)sid;
- (NSMenu *)menuForCore:(NSString *)sid;
- (void)coreDragOver:(NSString *)sid;                       // image files are being dragged over this core (nil: none)
- (BOOL)coreDropImages:(NSPasteboard *)pb on:(NSString *)sid;
@end

static const double PRESS_AFTER = 0.35; // seconds: a press held longer is push-to-talk, not a click
static const CGFloat DRAG_AFTER = 4;    // points the mouse moves before a press becomes a drag
static const float DRAG_TURN = 0.012f;  // radians per point dragged

@interface PanelView : NSView
@property(weak) id<CoreActions> actions;
@property(readonly) BOOL busy;   // in the middle of a press or drag: keep taking mouse events
@property(readonly) BOOL moving; // a core is being dragged, or the cluster moved (the tooltip stays away)
- (BOOL)anythingDue:(double)now;
- (int)coreAt:(NSPoint)p look:(NSPoint *)look center:(NSPoint *)center;
- (void)cancelPress;
- (NSRect)coreRect:(int)i;
- (void)relayout;
@end

@implementation PanelView {
    CGImageRef _img[MAX_SESS + 1];
    double _due[MAX_SESS + 1];
    Canvas _cv;
    int _lastCount;
    NSRect _core[MAX_SESS + 1]; // where each polyhedron was last drawn, for coreAt:
    int _coreN;
    // the gesture in progress
    NSString *_downSid, *_clickSid;
    NSPoint _downAt, _dragAt;
    double _dragTime;
    float _velX, _velY;
    BOOL _dragging, _pressing, _reordering;
    NSTimer *_pressTimer, *_clickTimer;
    NSMutableDictionary<NSString *, NSValue *> *_place; // single-core mode: where each core is drawn now (eases to its spot)
    CGFloat _grabY, _holdY; // ⌥-drag: where on the core it was grabbed, and where the mouse is now
    BOOL _moving;           // single-core mode ⌥-drag: the whole cluster follows the mouse
    NSPoint _moveMouse, _moveOrigin;
    BOOL _carrying, _swallowUp; // ⌥-click: the core rides on the mouse, button up, until the next click
    NSTimer *_followTimer;      // asks where the mouse is while a core rides on it
    id _placeMonitor;
    NSPoint _lastMouse;         // for the mouse speed the carried core reacts to
    double _lastMouseT;
    CGFloat _mvx, _mvy;         // points per second, smoothed
    double _lastDraw;
}

- (BOOL)isOpaque { return NO; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { (void)e; return YES; } // a click on a core never has to activate the app first
- (BOOL)busy { return _downSid != nil; }
- (BOOL)moving { return _dragging || _moving || _reordering; }
- (NSRect)coreRect:(int)i { return i >= 0 && i < _coreN ? _core[i] : NSZeroRect; }

- (NSString *)sidAt:(NSEvent *)e {
    int i = [self coreAt:[self convertPoint:e.locationInWindow fromView:nil] look:NULL center:NULL];
    const Room *r = shown_room();
    return i >= 0 && i < r->n ? sid_of(&r->s[i]) : nil;
}

// A press is told apart by what happens next: moved more than DRAG_AFTER = drag, held longer than
// PRESS_AFTER = push-to-talk, released before that = click. A single click is only acted on once the
// system's double-click time has passed without a second one, so a double click never starts with a single.
- (void)mouseDown:(NSEvent *)e {
    if (_carrying) { [self place]; _swallowUp = YES; return; } // the click that puts a carried core down
    NSString *sid = [self sidAt:e];
    if (!sid) return;
    if (e.clickCount >= 2 && _clickTimer.valid && [sid isEqualToString:_clickSid]) {
        [self.actions coreTouched:sid];
        [_clickTimer invalidate];
        _clickTimer = nil;
        [self.actions coreDoubleClicked:sid];
        return;
    }
    [_clickTimer invalidate]; // a click on another core: the earlier single click still counts
    if (_clickTimer) [self.actions coreClicked:_clickSid];
    _clickTimer = nil;
    [self.actions coreTouched:sid];
    _downSid = sid;
    _downAt = _dragAt = e.locationInWindow;
    _moveMouse = [NSEvent mouseLocation];
    if (e.modifierFlags & NSEventModifierFlagOption && g_single) { // ⌥-drag: the cluster goes anywhere on the screen
        _moving = YES;
        _moveOrigin = self.window.frame.origin;
        _dragging = _pressing = NO;
        [self startFollow];
        return;
    }
    if (e.modifierFlags & NSEventModifierFlagOption && !g_single) { // ⌥-drag: move this core up or down the column
        NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
        int i = [self coreAt:p look:NULL center:NULL];
        _grabY = i >= 0 ? p.y - NSMidY(_core[i]) : 0;
        _holdY = p.y;
        _reordering = YES;
        _dragging = _pressing = NO;
        [self startFollow];
        return;
    }
    _dragTime = e.timestamp;
    _dragging = _pressing = NO;
    __weak PanelView *me = self;
    _pressTimer = [NSTimer scheduledTimerWithTimeInterval:PRESS_AFTER repeats:NO block:^(NSTimer *t) {
        (void)t;
        PanelView *v = me;
        if (!v || v->_dragging || !v->_downSid) return;
        v->_pressing = YES;
        [v.actions corePressBegan:v->_downSid];
    }];
}

// ⌥-drag or ⌥-click: from here until the core is put down, a timer asks where the mouse is (with the
// button up there are no drag events), so the core also notices when the mouse stops
- (void)startFollow {
    _lastMouse = [NSEvent mouseLocation];
    _lastMouseT = wall();
    _mvx = _mvy = 0;
    Session *s = session_by_sid(_downSid);
    if (s) s->carried = 1;
    __weak PanelView *me = self;
    [_followTimer invalidate];
    _followTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 120 repeats:YES block:^(NSTimer *t) { (void)t; [me follow]; }];
    [[NSRunLoop currentRunLoop] addTimer:_followTimer forMode:NSEventTrackingRunLoopMode]; // also during the drag itself
}

// the index of the large core in single-core mode (as drawSingle picks it)
- (int)focusIndex {
    const Room *r = shown_room();
    for (int i = 0; i < r->n; i++) if (g_focus && [g_focus isEqualToString:sid_of(&r->s[i])]) return i;
    return 0;
}

// The cluster (single-core mode) or the held core goes where the mouse is, and the cores feel the motion:
// the carried one leans and sloshes (CARRY in session.c), and the small cores lag behind on their way.
- (void)follow {
    NSPoint m = [NSEvent mouseLocation];
    double now = wall(), dt = now - _lastMouseT;
    if (dt >= 1.0 / 240) { // the mouse speed, smoothed a little
        CGFloat a = 0.45;
        _mvx += ((m.x - _lastMouse.x) / dt - _mvx) * a;
        _mvy += ((m.y - _lastMouse.y) / dt - _mvy) * a;
        _lastMouse = m;
        _lastMouseT = now;
    }
    Room *room = (Room *)shown_room();
    Session *held = session_by_sid(_downSid);
    int hi = held ? (int)(held - room->s) : -1;
    CGFloat w = hi >= 0 && hi < _coreN && _core[hi].size.width > 1 ? _core[hi].size.width : 100;
    for (int i = 0; i < room->n; i++) { // in single-core mode the whole cluster moves; in the column only the held core
        Session *s = &room->s[i];
        BOOL moves = _moving || i == hi;
        s->move_x = moves && _moving ? (float)(_mvx / w) : 0;
        s->move_y = moves ? (float)(-_mvy / w) : 0;
    }
    if (_moving) {
        if (_carrying || hypot(m.x - _moveMouse.x, m.y - _moveMouse.y) >= DRAG_AFTER) pointer_visible(NO); // the core is the pointer
        NSPoint was = self.window.frame.origin, to = NSMakePoint(_moveOrigin.x + m.x - _moveMouse.x, _moveOrigin.y + m.y - _moveMouse.y);
        if (to.x == was.x && to.y == was.y) return;
        [self.window setFrameOrigin:to];
        // the small cores stay where they were on the screen for a moment and then catch up: they trail behind
        NSString *focus = room->n ? sid_of(&room->s[[self focusIndex]]) : nil;
        NSSize b = self.bounds.size;
        for (NSString *sid in _place.allKeys) {
            if ([sid isEqualToString:focus]) continue;
            NSRect r = _place[sid].rectValue;
            r.origin.x = fmax(0, fmin(b.width - r.size.width, r.origin.x - (to.x - was.x)));
            r.origin.y = fmax(0, fmin(b.height - r.size.height, r.origin.y - (to.y - was.y)));
            _place[sid] = [NSValue valueWithRect:r];
        }
        [self setNeedsDisplay:YES];
    } else if (_reordering) {
        NSPoint p = [self convertPoint:[self.window convertPointFromScreen:m] fromView:nil];
        _holdY = p.y;
        [self reorderTo:p];
        [self setNeedsDisplay:YES]; // the held core follows the mouse
    }
}

// the end of an ⌥-drag, or the click that puts a carried core down: it lands with a little bounce
- (void)endMove {
    [_followTimer invalidate];
    _followTimer = nil;
    pointer_visible(YES);
    Room *room = (Room *)shown_room();
    for (int i = 0; i < room->n; i++) room->s[i].carried = 0, room->s[i].move_x = room->s[i].move_y = 0;
    Session *s = session_by_sid(_downSid);
    if (s) s->jig_vy += CARRY.drop;
    _downSid = nil;
    if (_reordering) { _reordering = NO; [self setNeedsDisplay:YES]; }
    if (_moving) { _moving = NO; save_position(self.window.screen, self.window.frame.origin); }
}

// An ⌥-click that did not move picks the core up: it keeps following the mouse with the button up until the
// next click anywhere puts it down.
- (void)carry {
    _carrying = YES;
    __weak PanelView *me = self;
    // a click outside the panel (it can leave the column) also puts it down; mouse monitors need no permission
    _placeMonitor = [NSEvent addGlobalMonitorForEventsMatchingMask:NSEventMaskLeftMouseDown | NSEventMaskRightMouseDown
                                                           handler:^(NSEvent *ev) { (void)ev; [me place]; }];
}

- (void)place {
    if (!_carrying) return;
    _carrying = NO;
    if (_placeMonitor) { [NSEvent removeMonitor:_placeMonitor]; _placeMonitor = nil; }
    [self follow];
    [self endMove];
}

- (void)mouseDragged:(NSEvent *)e {
    if (_moving || _reordering) return; // the follow timer moves it
    if (!_downSid || _pressing) return;
    NSPoint p = e.locationInWindow;
    if (!_dragging && hypot(p.x - _downAt.x, p.y - _downAt.y) < DRAG_AFTER) return;
    Session *s = session_by_sid(_downSid);
    if (!s) return;
    if (!_dragging) { _dragging = YES; [_pressTimer invalidate]; }
    float dx = (float)(p.x - _dragAt.x), dy = (float)(p.y - _dragAt.y), dt = (float)(e.timestamp - _dragTime);
    s->held = 1;
    s->ay += dx * DRAG_TURN; // the shell follows the mouse like a ball under the hand
    s->ax -= dy * DRAG_TURN;
    if (dt > 0.001f) { _velX = -dy * DRAG_TURN / dt; _velY = dx * DRAG_TURN / dt; }
    _dragAt = p;
    _dragTime = e.timestamp;
    [self setNeedsDisplay:YES];
}

- (void)mouseUp:(NSEvent *)e {
    [_pressTimer invalidate];
    _pressTimer = nil;
    if (_swallowUp) { _swallowUp = NO; return; }
    if (_moving || _reordering) {
        NSPoint m = [NSEvent mouseLocation];
        if (!_carrying && hypot(m.x - _moveMouse.x, m.y - _moveMouse.y) < DRAG_AFTER) [self carry]; // ⌥-click
        else if (!_carrying) [self endMove];
        return;
    }
    NSString *sid = _downSid;
    _downSid = nil;
    if (!sid) return;
    if (_pressing) { _pressing = NO; [self.actions corePressEnded:sid]; return; }
    if (_dragging) { // let go: it keeps spinning the way it was thrown and slows back to its own rotation
        _dragging = NO;
        Session *s = session_by_sid(sid);
        if (s) {
            BOOL thrown = e.timestamp - _dragTime < 0.1;
            s->held = 0;
            s->spin_x = thrown ? fmaxf(-8, fminf(8, _velX)) : 0;
            s->spin_y = thrown ? fmaxf(-8, fminf(8, _velY)) : 0;
        }
        return;
    }
    _clickSid = sid;
    __weak PanelView *me = self;
    _clickTimer = [NSTimer scheduledTimerWithTimeInterval:[NSEvent doubleClickInterval] repeats:NO block:^(NSTimer *t) {
        (void)t;
        PanelView *v = me;
        if (!v) return;
        v->_clickTimer = nil;
        [v.actions coreClicked:v->_clickSid];
    }];
}

- (void)rightMouseDown:(NSEvent *)e {
    if (_carrying) { [self place]; return; }
    NSString *sid = [self sidAt:e];
    NSMenu *menu = sid ? [self.actions menuForCore:sid] : nil;
    if (menu) [NSMenu popUpContextMenu:menu withEvent:e forView:self];
}

// the wheel sets this core's speech volume, and the shell shows it for a moment; ⌥ + the wheel spins the core
// instead (Settings > Motion > ⌥ + scroll on a core), as the wheel alone does on Windows
- (void)scrollWheel:(NSEvent *)e {
    Session *s = session_by_sid([self sidAt:e]);
    if (!s) return;
    if (e.modifierFlags & NSEventModifierFlagOption) {
        float k = (float)e.scrollingDeltaY * (e.hasPreciseScrollingDeltas ? 0.12f : 1.2f);
        switch (g_wheel_spin) {
        case 0: s->spin_x = fmaxf(-8, fminf(8, s->spin_x - k)); break; // roll: the top goes away as the wheel goes down
        case 1: s->spin_x = fmaxf(-8, fminf(8, s->spin_x + k)); break;
        case 2: s->spin_y = fmaxf(-8, fminf(8, s->spin_y - k)); break; // turn: round the upright axis
        case 3: s->spin_y = fmaxf(-8, fminf(8, s->spin_y + k)); break;
        }
        return;
    }
    float step = (float)e.scrollingDeltaY * (e.hasPreciseScrollingDeltas ? 0.004f : 0.05f);
    s->vol = fmaxf(0, fminf(1, s->vol + step));
    s->vol_at = wall();
}

// While ⌥-dragging, the dragged core takes the place of the core nearest the mouse, so the column
// rearranges under the hand.
- (void)reorderTo:(NSPoint)p {
    Room *r = g_room.n && !g_single ? &g_room : NULL; // (single-core mode has no column)
    Session *s = session_by_sid(_downSid);
    if (!r || !s || s < r->s || s >= r->s + r->n) return;
    int from = (int)(s - r->s), to = from;
    CGFloat best = 1e9;
    for (int i = 0; i < _coreN && i < r->n; i++) {
        CGFloat d = fabs(p.y - NSMidY(_core[i]));
        if (d < best) { best = d; to = i; }
    }
    if (to == from) return;
    room_move(r, from, to);
    CGImageRef a = _img[from]; // the pictures move with their sessions
    if (from < to) memmove(&_img[from], &_img[from + 1], sizeof(CGImageRef) * (size_t)(to - from));
    else memmove(&_img[to + 1], &_img[to], sizeof(CGImageRef) * (size_t)(from - to));
    _img[to] = a;
    for (int i = 0; i <= MAX_SESS; i++) _due[i] = 0;
    [self setNeedsDisplay:YES];
}

// ---- image files dragged onto a core: it reads them (TRANSCRIBING), and its reply box opens with a token each
- (NSString *)imageCoreFor:(id<NSDraggingInfo>)info {
    if (![ReplyBox hasImage:info.draggingPasteboard]) return nil;
    NSString *sid = [self sidAtWindowPoint:info.draggingLocation];
    return sid && strcmp(sid.UTF8String, "ghost") ? sid : nil;
}
- (NSString *)sidAtWindowPoint:(NSPoint)w {
    int i = [self coreAt:[self convertPoint:w fromView:nil] look:NULL center:NULL];
    const Room *r = shown_room();
    return i >= 0 && i < r->n ? sid_of(&r->s[i]) : nil;
}
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)info { return [self draggingUpdated:info]; }
- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)info {
    NSString *sid = [self imageCoreFor:info];
    [self.actions coreDragOver:sid];
    return sid ? NSDragOperationCopy : NSDragOperationNone;
}
- (void)draggingExited:(id<NSDraggingInfo>)info { (void)info; [self.actions coreDragOver:nil]; }
- (BOOL)performDragOperation:(id<NSDraggingInfo>)info {
    NSString *sid = [self imageCoreFor:info];
    [self.actions coreDragOver:nil];
    return sid && [self.actions coreDropImages:info.draggingPasteboard on:sid];
}

// cancels a press in progress (Esc during push-to-talk)
- (void)cancelPress { [_pressTimer invalidate]; _pressTimer = nil; _pressing = NO; _downSid = nil; }

// The session whose core is under p (view coordinates), or -1. Only the round shell counts, not the
// square it is drawn in, so clicks next to a core still reach the terminal. look gets the offset from the
// core's centre in shell radii (y down, as in the picture), center the core's centre.
- (int)coreAt:(NSPoint)p look:(NSPoint *)look center:(NSPoint *)center {
    for (int i = 0; i < _coreN; i++) {
        NSRect r = _core[i];
        CGFloat rad = r.size.width * SHELL_HIT, dx = p.x - NSMidX(r), dy = p.y - NSMidY(r);
        if (dx * dx + dy * dy > rad * rad) continue;
        if (look) *look = NSMakePoint(dx / rad, -dy / rad);
        if (center) *center = NSMakePoint(NSMidX(r), NSMidY(r));
        return i;
    }
    return -1;
}

- (const Room *)shown { return shown_room(); }

- (BOOL)anythingDue:(double)now {
    const Room *r = [self shown];
    if (r->n != _lastCount) return YES;
    if (g_single && r->n > 1) return YES; // the small cores keep floating
    for (int i = 0; i < r->n && i <= MAX_SESS; i++) if (!_img[i] || now >= _due[i]) return YES;
    return NO;
}

// Single-core mode: the session in focus large near the top, the others small, floating around it on an
// ellipse. Every core eases toward its spot, so when the focus changes the two swap places smoothly.
- (void)drawSingle:(const Room *)room now:(double)now width:(CGFloat)W height:(CGFloat)H scale:(CGFloat)scale {
    CGContextRef ctx = [NSGraphicsContext currentContext].CGContext;
    int n = room->n, fi = 0;
    for (int i = 0; i < n; i++) if (g_focus && [g_focus isEqualToString:sid_of(&room->s[i])]) fi = i;
    if (room->n != _lastCount) { for (int i = 0; i <= MAX_SESS; i++) _due[i] = 0; _lastCount = room->n; }
    if (!_place) _place = [NSMutableDictionary dictionary];
    float dt = (float)fmin(0.25, now - _lastDraw);
    _lastDraw = now;
    CGFloat big = W * 0.6, rx = W / 2 - 8, ry = rx * 1.15;
    CGFloat small = n > 1 && g_others ? fmin(W * 0.2, 2 * M_PI * rx / (n - 1) * 0.75) : 0;
    rx -= small / 2;
    ry -= small / 2;
    CGFloat cx = W / 2, cy = H / 2; // the window is just the cluster
    float ease = 1 - expf(-dt * 7);
    NSRect at[MAX_SESS];
    for (int i = 0, k = 0; i < n; i++) {
        NSRect target;
        if (i == fi) target = NSMakeRect(cx - big / 2, cy - big / 2, big, big);
        else {
            double a = M_PI / 2 + 2 * M_PI * k / (n - 1) + now * 0.10; // slowly around, starting at the top
            CGFloat x = cx + rx * cos(a), y = cy + ry * sin(a) + 3 * sin(now * 0.8 + i * 1.7);
            target = NSMakeRect(x - small / 2, y - small / 2, small, small);
            k++;
        }
        NSString *sid = sid_of(&room->s[i]);
        NSValue *was = _place[sid];
        NSRect r = was ? was.rectValue : target;
        r.origin.x += (target.origin.x - r.origin.x) * ease;
        r.origin.y += (target.origin.y - r.origin.y) * ease;
        r.size.width += (target.size.width - r.size.width) * ease;
        r.size.height = r.size.width;
        _place[sid] = [NSValue valueWithRect:r];
        at[i] = r;
    }
    for (int pass = 0; pass < 2; pass++) // the small ones first, the one in focus on top
        for (int i = 0; i < n; i++) {
            if ((i == fi) != (pass == 1)) continue;
            if (i != fi && !g_others) { _core[i] = NSZeroRect; continue; } // Settings > Other sessions off
            const Session *s = &room->s[i];
            int px = ((int)(at[i].size.width * scale) / 8) * 8; // in steps, so a core growing is redrawn only now and then
            if (px < 32) px = 32;
            if (!_img[i] || now >= _due[i] || (int)CGImageGetWidth(_img[i]) != px) {
                if (_img[i]) CGImageRelease(_img[i]);
                _img[i] = [self newPictureOf:s pixels:px now:now];
                double rate = session_rate(s, now, 20) * (i == fi ? 1.5 : 0.75); // the small ones need fewer frames
                _due[i] = now + 1.0 / rate - 0.01;
            }
            _core[i] = at[i];
            CGContextDrawImage(ctx, NSRectToCGRect(at[i]), _img[i]);
            focus_frame(ctx, at[i], s, now);
        }
    _coreN = n;
}

// forget where things were: the next frame lays everything out again (mode switch, reorder)
- (void)relayout { _lastCount = -1; [_place removeAllObjects]; [self setNeedsDisplay:YES]; }

// renders one session into a see-through picture of px by px pixels
- (CGImageRef)newPictureOf:(const Session *)s pixels:(int)px now:(double)now {
    canvas_resize(&_cv, px, px);
    render_one(&_cv, s, &g_shape, now, g_glow, g_core);
    size_t n = (size_t)px * (size_t)px * 4;
    uint8_t *buf = malloc(n);
    canvas_rgba(&_cv, buf);
    CFDataRef data = CFDataCreateWithBytesNoCopy(NULL, buf, (CFIndex)n, kCFAllocatorMalloc); // frees buf when released
    CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGImageRef img = CGImageCreate((size_t)px, (size_t)px, 8, 32, (size_t)px * 4, space,
                                   (CGBitmapInfo)kCGImageAlphaPremultipliedLast, provider, NULL, true, kCGRenderingIntentDefault);
    CGColorSpaceRelease(space);
    CGDataProviderRelease(provider);
    CFRelease(data);
    return img;
}

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    double now = wall();
    const Room *room = [self shown];
    CGFloat W = self.bounds.size.width, H = self.bounds.size.height - g_inset; // the inset keeps the first line clear of the pane's title bar
    if (H < 60) H = 60;
    CGFloat scale = self.window ? self.window.backingScaleFactor : 2.0;
    if (g_single) { [self drawSingle:room now:now width:W height:self.bounds.size.height scale:scale]; return; }
    NSFont *font = [NSFont fontWithName:g_font_name size:g_font_size];
    if (!font) font = [NSFont monospacedSystemFontOfSize:g_font_size weight:NSFontWeightRegular];
    CGFloat lineH = ceil(font.ascender - font.descender) + 3, textH = g_labels_hover ? 0 : lineH * 2 + 4, gap = 10, left = 10;

    // how many sessions fit, and how large each polyhedron can be (square at most)
    int n = room->n;
    while (n > 1 && H / n < textH + gap + 70) n--;
    CGFloat side = H / n - textH - gap;
    if (side > W) side = W;
    if (side < 24) side = 24;
    if (room->n != _lastCount) { // the arrangement changed: every picture is redrawn
        for (int i = 0; i <= MAX_SESS; i++) _due[i] = 0;
        _lastCount = room->n;
    }

    NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
    para.lineBreakMode = NSLineBreakByTruncatingTail;
    CGContextRef ctx = [NSGraphicsContext currentContext].CGContext;
    int held = -1;

    for (int i = 0; i < n; i++) {
        const Session *s = &room->s[i];
        CGFloat top = H - i * (textH + side + gap); // this view's y runs upward, so the list starts at the top edge
        int st = session_shown(s, now);

        if (!g_labels_hover) { // a coloured dot and the folder name, then the state in grey
            Col c = s->cur.eh; // the dot follows the shell's glow colour as it eases between states
            NSColor *dotColor = [NSColor colorWithSRGBRed:c.r green:c.g blue:c.b alpha:1];
            NSString *name = [NSString stringWithUTF8String:s->name[0] ? s->name : "claude"];
            if (!name) name = @"claude";
            NSString *state = state_line(s, st);
            NSDictionary *dotAttr = @{NSFontAttributeName: font, NSForegroundColorAttributeName: dotColor};
            NSDictionary *nameAttr = @{NSFontAttributeName: font, NSParagraphStyleAttributeName: para,
                                       NSForegroundColorAttributeName: [NSColor colorWithSRGBRed:0.89 green:0.90 blue:0.93 alpha:1]};
            NSDictionary *stateAttr = @{NSFontAttributeName: font, NSParagraphStyleAttributeName: para,
                                        NSForegroundColorAttributeName: [NSColor colorWithSRGBRed:0.52 green:0.54 blue:0.59 alpha:1]};
            CGFloat indent = left + ceil([@"● " sizeWithAttributes:dotAttr].width);
            [@"●" drawInRect:NSMakeRect(left, top - lineH, indent - left, lineH) withAttributes:dotAttr];
            [name drawInRect:NSMakeRect(indent, top - lineH, W - indent - 6, lineH) withAttributes:nameAttr];
            [state drawInRect:NSMakeRect(indent, top - lineH * 2, W - indent - 6, lineH) withAttributes:stateAttr];
        }

        // the polyhedron: redrawn only as often as its state needs, otherwise the last picture is reused
        int px = (int)(side * scale);
        if (px < 32) px = 32;
        if (!_img[i] || now >= _due[i] || (int)CGImageGetWidth(_img[i]) != px) {
            if (_img[i]) CGImageRelease(_img[i]);
            _img[i] = [self newPictureOf:s pixels:px now:now];
            double rate = session_rate(s, now, 20) * 1.5; // drawing here is cheap, so run a little faster than in a terminal
            _due[i] = now + 1.0 / rate - 0.01;
        }
        _core[i] = NSMakeRect((W - side) / 2, top - textH - side, side, side);
        if (_reordering && [_downSid isEqualToString:sid_of(s)]) { held = i; continue; } // drawn last, at the mouse
        CGContextDrawImage(ctx, NSRectToCGRect(_core[i]), _img[i]);
        focus_frame(ctx, _core[i], s, now);
    }
    _coreN = n;
    if (held >= 0) { // ⌥-drag: the held core under the mouse, a little larger, above the others
        NSRect r = NSInsetRect(_core[held], -side * 0.06, -side * 0.06);
        r.origin.y = _holdY - _grabY - r.size.height / 2;
        CGContextSetAlpha(ctx, 0.92);
        CGContextDrawImage(ctx, NSRectToCGRect(r), _img[held]);
        CGContextSetAlpha(ctx, 1);
        focus_frame(ctx, r, &room->s[held], now);
    }
}
@end

// ---- the hover tooltip: name and state, then the first two lines of the last reply ----------------------
// ---- the application ------------------------------------------------------------------------------------
@interface AppDelegate : NSObject <NSApplicationDelegate, CoreActions, ReplyBoxDelegate, SettingsHost>
@property(strong) NSPanel *panel;
@property(strong) PanelView *view;
@property(strong) NSTimer *timer;
@property(strong) NSMutableArray *signals;
@property double last, emptySince;
@property(strong) NSPanel *tip;
@property(strong) TipView *tipView;
@property(copy) NSString *hoverSid, *pttSid; // pttSid: the session push-to-talk is for
@property double hoverSince, sizeCheck, pendingUntil;
@property BOOL away, pttByHotkey;
@property(strong) NSMutableDictionary<NSString *, NSNumber *> *overlay; // sid -> a panel state shown instead of its own
@property(strong) Speaker *speaker;
@property(strong) NSMutableDictionary<NSString *, NSArray *> *seen; // sid -> @[last reply time, last NEEDS YOU time] already handled
@property(strong) NSMutableSet<NSString *> *summarizing;             // sids waiting for claude -p
@property(strong) Listener *listener;
@property(copy) NSString *pendingSid, *pendingText; // a recognized reply waiting to be sent
@property BOOL pendingHeld;                         // the core was touched: wait for what the click means
@property(strong) NSMutableDictionary<NSString *, NSNumber *> *errorUntil; // sid -> show ERROR until
@property(strong) NSPanel *preview;                 // the recognized text, under the core
@property double focusSince;                        // when the focus last moved
@property(strong) NSMutableDictionary<NSString *, NSNumber *> *hue; // sid -> index into HUES
@property(strong) TipView *previewView;
@property unsigned short paneCols, paneRows;
@property(strong) ReplyBox *reply;                  // double-click a core: type a reply under its tooltip
@property(copy) NSString *dragSid;                  // the core image files are being dragged over
@property(strong) SettingsWindow *settings;         // right-click > Settings...
@end

@implementation AppDelegate

// The right edge of the screen, a sixth of its width unless --width says otherwise. With --fit-pane (what
// open_pane.sh uses) the panel takes the size of the terminal pane it was started in, so it covers exactly
// the pane that was opened to keep the space free.
- (void)place {
    NSScreen *screen = [NSScreen mainScreen] ?: [NSScreen screens].firstObject;
    NSRect f = screen.frame;
    CGFloat w = g_width > 0 ? g_width : fmax(220, fmin(360, f.size.width * 0.17)), h = f.size.height - g_top;
    struct winsize ws = {0};
    if (g_fit_pane && g_tty >= 0 && ioctl(g_tty, TIOCGWINSZ, &ws) == 0 && ws.ws_col && ws.ws_xpixel && ws.ws_ypixel) {
        CGFloat cw = (CGFloat)ws.ws_xpixel / ws.ws_col, k = 1;
        if (cw > 13) k = 1 / screen.backingScaleFactor; // the terminal reported device pixels, the screen is measured in points
        if (g_width <= 0) w = ws.ws_xpixel * k;
        h = ws.ws_ypixel * k;
    }
    if (!g_fit_pane && g_width <= 0) w = round(w * g_scale); // Settings > Size on screen (a pane keeps its own size)
    if (w > f.size.width * 0.6) w = f.size.width * 0.6;
    if (h > f.size.height) h = f.size.height;
    if (g_single) { // just the cluster, where it was put last (else the top of the right edge)
        CGFloat cw = round(fmax(200, fmin(320, w)) * g_scale), ch = ceil(cw * 1.2) + 8; // Settings > Size on screen
        NSArray *at = positions()[screen_key(screen)];
        NSRect vis = screen.visibleFrame;
        NSRect r = NSMakeRect(NSMaxX(f) - cw, NSMaxY(vis) - ch, cw, ch);
        if ([at isKindOfClass:[NSArray class]] && at.count == 2) r.origin = NSMakePoint([at[0] doubleValue], [at[1] doubleValue]);
        r.origin.x = fmax(NSMinX(vis), fmin(r.origin.x, NSMaxX(vis) - cw));
        r.origin.y = fmax(NSMinY(vis), fmin(r.origin.y, NSMaxY(vis) - ch));
        [self.panel setFrame:r display:YES];
        return;
    }
    [self.panel setFrame:NSMakeRect(NSMaxX(f) - w, NSMinY(f), w, h) display:YES];
}

// By default the panel belongs to the one Space it was opened in (the full-screen iTerm2 window it was
// started from) and is simply left there. With --all-spaces it follows you to every Space instead, and is
// then shown only while iTerm2 is the app in front, so it does not float over everything else.
- (void)updateVisibility {
    if (!g_all_spaces) {
        if (!self.panel.visible) [self.panel orderFrontRegardless];
        return;
    }
    NSString *front = [NSWorkspace sharedWorkspace].frontmostApplication.bundleIdentifier;
    if (g_always || [front isEqualToString:@"com.googlecode.iterm2"]) [self.panel orderFrontRegardless];
    else [self.panel orderOut:nil];
}

- (void)appChanged:(NSNotification *)note { (void)note; [self updateVisibility]; }
- (void)screenChanged:(NSNotification *)note { (void)note; [self place]; }

- (void)onSignal:(int)sig handler:(dispatch_block_t)block {
    signal(sig, SIG_IGN); // the dispatch source receives it instead
    dispatch_source_t src = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, (uintptr_t)sig, 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(src, block);
    dispatch_resume(src);
    [self.signals addObject:src];
}

- (void)applicationDidFinishLaunching:(NSNotification *)note {
    (void)note;
    NSPanel *p = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 290, 600)
                                            styleMask:(NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel)
                                              backing:NSBackingStoreBuffered
                                                defer:NO];
    p.level = NSFloatingWindowLevel;
    p.opaque = NO;
    p.backgroundColor = g_bg ?: [NSColor clearColor];
    p.hasShadow = NO;
    p.ignoresMouseEvents = YES;  // clicks go to whatever is underneath
    p.hidesOnDeactivate = NO;
    p.releasedWhenClosed = NO;
    p.collectionBehavior = NSWindowCollectionBehaviorFullScreenAuxiliary | NSWindowCollectionBehaviorIgnoresCycle; // may sit in a full-screen Space
    if (g_all_spaces) p.collectionBehavior |= NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorStationary;
    self.view = [[PanelView alloc] initWithFrame:NSMakeRect(0, 0, 290, 600)];
    self.view.actions = self;
    [self.view registerForDraggedTypes:[ReplyBox dragTypes]];
    self.reply = [[ReplyBox alloc] init];
    self.reply.delegate = self;
    self.reply.fontName = g_font_name;
    self.reply.fontSize = g_font_size;
    self.reply.behavior = p.collectionBehavior;
    self.reply.boxColor = hex_color(tip_bg_hex());
    self.reply.inkColor = tip_ink();
    self.overlay = [NSMutableDictionary dictionary];
    self.seen = [NSMutableDictionary dictionary];
    self.summarizing = [NSMutableSet set];
    self.errorUntil = [NSMutableDictionary dictionary];
    self.hue = [NSMutableDictionary dictionary];
    [self registerHotkeys];
    if (g_voice) [self setVoice:YES];
    p.contentView = self.view;
    self.panel = p;
    [self place];
    [self updateVisibility];

    self.signals = [NSMutableArray array];
    __weak AppDelegate *me = self;
    [self onSignal:SIGWINCH handler:^{ [me place]; }];
    [self onSignal:SIGHUP handler:^{ [NSApp terminate:nil]; }]; // the pane was closed
    if (g_launcher_pid > 0) { // started by the launcher script: it gets the pane's signals, we only see it end
        if (kill(g_launcher_pid, 0) != 0) { [NSApp terminate:nil]; return; }
        dispatch_source_t src = dispatch_source_create(DISPATCH_SOURCE_TYPE_PROC, (uintptr_t)g_launcher_pid, DISPATCH_PROC_EXIT, dispatch_get_main_queue());
        dispatch_source_set_event_handler(src, ^{ [NSApp terminate:nil]; });
        dispatch_resume(src);
        [self.signals addObject:src];
    }
    [self onSignal:SIGTERM handler:^{ [NSApp terminate:nil]; }];
    if (g_reply_shot) [self performSelector:@selector(replyShot) withObject:nil afterDelay:2.0];
    if (g_settings_open || g_settings_shot) [self openSettings];
    if (g_settings_shot) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(2 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        [self.settings saveShot:@(g_settings_shot)];
        [NSApp terminate:nil];
    });
    [self onSignal:SIGINT handler:^{ [NSApp terminate:nil]; }];
    [[[NSWorkspace sharedWorkspace] notificationCenter] addObserver:self selector:@selector(appChanged:)
                                                               name:NSWorkspaceDidActivateApplicationNotification object:nil];
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(screenChanged:)
                                                 name:NSApplicationDidChangeScreenParametersNotification object:nil];

    double now = wall();
    self.last = self.emptySince = now;
    self.timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 30 target:self selector:@selector(tick:) userInfo:nil repeats:YES];
    // also while a menu is open or a drag is tracked (other run loop modes), so the cores never freeze
    [[NSRunLoop currentRunLoop] addTimer:self.timer forMode:NSRunLoopCommonModes];
}

- (void)tick:(NSTimer *)timer {
    (void)timer;
    double now = wall();
    float dt = (float)(now - self.last);
    if (dt > 0.25f) dt = 0.25f;
    self.last = now;
    if (g_demo_n) demo_run(&g_demo, &g_room, now);
    else {
        int n = tail_poll(&g_tail, g_ev, 1024);
        for (int i = 0; i < n; i++) room_apply(&g_room, &g_ev[i]);
    }
    [self trackMouse:now];
    [self watchForNews:now];
    [self updateFocus:now];
    if (self.pendingSid && !self.pendingHeld && now > self.pendingUntil) {
        if (g_confirm_send) [self dropPending]; // not confirmed in time
        else [self sendPending];
    }
    [self updateOverlays:now];
    if (g_launcher_pid > 0 && g_tty >= 0 && now - self.sizeCheck > 0.5) { // no SIGWINCH outside the pane's process group
        struct winsize ws = {0};
        self.sizeCheck = now;
        if (ioctl(g_tty, TIOCGWINSZ, &ws) == 0 && (ws.ws_col != self.paneCols || ws.ws_row != self.paneRows)) {
            self.paneCols = ws.ws_col;
            self.paneRows = ws.ws_row;
            [self place];
        }
    }
    room_tick(&g_room, &g_shape, now, dt);
    g_ghost.s[0].last = now;
    room_tick(&g_ghost, &g_shape, now, dt);
    if (g_room.n) self.emptySince = now;
    if (g_exit_empty && now - self.emptySince > 15) { [NSApp terminate:nil]; return; }
    if (self.panel.visible && [self.view anythingDue:now]) [self.view setNeedsDisplay:YES];
}

// The core under the mouse, checked every frame. While the mouse is on a core the panel takes clicks;
// everywhere else it lets them through to the terminal underneath. (Asking for the position each frame
// needs no permission, and unlike event monitors it also sees the mouse while another app drags.)
- (void)trackMouse:(double)now {
    Room *room = g_room.n ? &g_room : &g_ghost;
    int hit = -1;
    NSPoint look = NSZeroPoint, center = NSZeroPoint;
    if (self.panel.visible) {
        NSPoint w = [self.panel convertPointFromScreen:[NSEvent mouseLocation]];
        hit = [self.view coreAt:[self.view convertPoint:w fromView:nil] look:&look center:&center];
        if (hit >= room->n) hit = -1;
    }
    for (int i = 0; i < room->n; i++) {
        Session *s = &room->s[i];
        s->hover_to = i == hit;
        if (i == hit) { s->look_x_to = (float)look.x; s->look_y_to = (float)look.y; }
        // lock-on: the pointer has rested on this core for 2 s (not while dragging or carrying)
        s->lock_to = i == hit && !self.view.moving && [sid_of(s) isEqualToString:self.hoverSid] && now - self.hoverSince > 2.0 ? 1 : 0;
    }
    [self watchForRush:now];
    BOOL ignore = hit < 0 && !self.view.busy; // a drag or press keeps going when the mouse leaves the core
    if (self.panel.ignoresMouseEvents != ignore) self.panel.ignoresMouseEvents = ignore;
    if (self.reply.open) { // while a reply is typed, the tooltip of its session stays up above it
        Session *s = session_by_sid(self.reply.sid);
        if (!s) { [self.reply close]; return; }
        NSRect r = [self.view coreRect:(int)(s - room->s)];
        [self showTipFor:s at:[self.view convertPoint:NSMakePoint(NSMidX(r), NSMidY(r)) toView:nil] now:now];
        return;
    }
    if (self.view.moving) { self.hoverSince = now; [self.tip orderOut:nil]; return; } // no tooltip while dragging

    NSString *sid = hit >= 0 ? [NSString stringWithUTF8String:room->s[hit].sid] : nil;
    if (![sid isEqualToString:self.hoverSid]) { self.hoverSid = sid; self.hoverSince = now; }
    if (hit < 0 || now - self.hoverSince < 0.3) { [self.tip orderOut:nil]; return; } // a short pause before the tooltip
    [self showTipFor:&room->s[hit] at:[self.view convertPoint:center toView:nil] now:now];
}

// Defence: only a rush at a core sets it off. The pointer has to start well away (over 2.2 shell radii
// from the centre) and land on the core within 0.18 s; passing by, or coming in at a normal pace, never does.
static double rush_t[128];
static NSPoint rush_p[128];
static int rush_n;
- (void)watchForRush:(double)now {
    NSPoint m = [NSEvent mouseLocation];
    if (rush_n == 128) { memmove(rush_t, rush_t + 1, 127 * sizeof(double)); memmove(rush_p, rush_p + 1, 127 * sizeof(NSPoint)); rush_n--; }
    rush_t[rush_n] = now; rush_p[rush_n++] = m;
    int drop = 0;
    while (drop < rush_n && now - rush_t[drop] > 0.18) drop++;
    if (drop) { memmove(rush_t, rush_t + drop, (size_t)(rush_n - drop) * sizeof(double)); memmove(rush_p, rush_p + drop, (size_t)(rush_n - drop) * sizeof(NSPoint)); rush_n -= drop; }
    if (!self.panel.visible || self.view.busy) return;
    Room *room = g_room.n ? &g_room : &g_ghost;
    for (int i = 0; i < room->n; i++) {
        Session *s = &room->s[i];
        NSRect r = [self.panel convertRectToScreen:[self.view convertRect:[self.view coreRect:i] toView:nil]];
        CGFloat R = r.size.width * SHELL_FRAC;
        if (R < 4 || now - s->defend_at < 2 || session_shown(s, now) == ST_WORK) continue;
        NSPoint c = NSMakePoint(NSMidX(r), NSMidY(r));
        if (hypot(m.x - c.x, m.y - c.y) / R > 1.0) continue;
        for (int j = 0; j < rush_n; j++) {
            if (hypot(rush_p[j].x - c.x, rush_p[j].y - c.y) / R <= 2.2) continue;
            CGFloat dx = m.x - rush_p[j].x, dy = m.y - rush_p[j].y, l = hypot(dx, dy);
            s->defend_at = now; s->defend_x = (float)(dx / l); s->defend_y = (float)(-dy / l); // screen y up, view y down
            rush_n = 0;
            return;
        }
    }
}

static unsigned tip_bg_hex(void) { return g_tip_bg >= 0 && g_tip_bg < 5 ? TIP_BGS[g_tip_bg] : g_tip_custom; }
static NSColor *tip_ink(void) { return g_text_col == 5 ? hex_color(g_text_custom) : g_text_col > 0 && g_text_col < 5 ? hex_color(TEXT_COLS[g_text_col]) : nil; }
// the text box's look (Settings > Replies): font, size, colours, how wide it may grow
- (void)styleTip:(TipView *)v {
    v.fontSize = g_font_size;
    v.monoBody = g_tip_mono;
    v.monoName = g_font_name;
    v.boxColor = hex_color(tip_bg_hex());
    v.inkColor = tip_ink();
    v.sizeK = g_tip_size;
    v.title = nil; // measures itself again
}
- (void)styleTips {
    if (self.tipView) { [self styleTip:self.tipView]; self.tipView.maxBodyLines = (int)lround(16 * g_tip_size); }
    if (self.previewView) [self styleTip:self.previewView];
    self.reply.fontSize = g_font_size;
    self.reply.boxColor = hex_color(tip_bg_hex());
    self.reply.inkColor = tip_ink();
    [self.tip orderOut:nil];
}

// a small floating box that never takes clicks (the hover tooltip, the recognized-text preview)
- (NSPanel *)newTipPanel:(TipView **)view {
    NSPanel *t = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 100, 40)
                                            styleMask:(NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel)
                                              backing:NSBackingStoreBuffered defer:NO];
    t.opaque = NO;
    t.backgroundColor = [NSColor clearColor];
    t.hasShadow = YES;
    t.ignoresMouseEvents = YES;
    t.level = NSFloatingWindowLevel;
    t.collectionBehavior = self.panel.collectionBehavior;
    *view = [[TipView alloc] initWithFrame:NSMakeRect(0, 0, 100, 40)];
    [self styleTip:*view];
    t.contentView = *view;
    return t;
}

- (void)showTipFor:(const Session *)s at:(NSPoint)center now:(double)now {
    if (self.preview.visible) { [self.tip orderOut:nil]; return; } // the preview has the floor
    if (!self.tip) {
        TipView *v;
        self.tip = [self newTipPanel:&v];
        v.maxBodyLines = (int)lround(16 * g_tip_size);
        self.tipView = v;
    }
    NSString *name = [NSString stringWithUTF8String:s->name[0] ? s->name : "claude"] ?: @"claude";
    NSString *title = [NSString stringWithFormat:@"%@ · %@", name, state_line(s, session_shown(s, now))];
    if (s->unread) title = [title stringByAppendingString:@" · new reply"];
    if (g_single && g_focus_pinned && [g_focus isEqualToString:sid_of(s)]) title = [title stringByAppendingString:@" · pinned"];
    int st = session_shown(s, now);
    NSString *body = (st == ST_WAIT ? msg_text(s, @"notice") : nil) ?: msg_text(s, @"reply") ?: @"(no reply yet)";
    Col c = s->aura_a > 0.01f ? s->aura : s->cur.eh; // single core: the session's own colour, else its state colour
    self.tipView.accent = [NSColor colorWithSRGBRed:c.r green:c.g blue:c.b alpha:1];
    if (![title isEqualToString:self.tipView.title] || ![body isEqualToString:self.tipView.body] || !self.tip.visible) {
        self.tipView.title = title;
        self.tipView.body = body;
        // to the left of the panel, level with the core, kept on the screen (or in the middle of it: Settings > Opens)
        NSRect vis = (self.panel.screen ?: [NSScreen mainScreen]).visibleFrame;
        self.tipView.maxWidth = g_tip_middle ? fmin(fmax(fmax(460, g_font_size * 36) * g_tip_size, vis.size.width * 0.45 * g_tip_size), 1000 * g_tip_size) : 0;
        NSSize sz = [self.tipView fit];
        NSPoint at = [self.panel convertPointToScreen:center];
        NSRect f = NSMakeRect(NSMinX(self.panel.frame) - sz.width - 8, at.y - sz.height / 2, sz.width, sz.height);
        if (g_tip_middle) f.origin = NSMakePoint(NSMidX(vis) - sz.width / 2, NSMidY(vis) - sz.height / 2);
        if (NSMinX(f) < NSMinX(vis)) f.origin.x = NSMinX(vis);
        if (NSMaxY(f) > NSMaxY(vis)) f.origin.y = NSMaxY(vis) - sz.height;
        if (NSMinY(f) < NSMinY(vis)) f.origin.y = NSMinY(vis);
        BOOL typing = self.reply.open && [self.reply.sid isEqualToString:sid_of(s)];
        if (typing && NSMinY(f) < NSMinY(vis) + self.reply.height + 6) f.origin.y = NSMinY(vis) + self.reply.height + 6; // room for the box
        [self.tip setFrame:f display:YES];
    }
    [self.tipView setNeedsDisplay:YES];
    if (!self.tip.visible) [self.tip orderFrontRegardless];
    if (self.reply.open && [self.reply.sid isEqualToString:sid_of(s)]) {
        self.reply.accent = self.tipView.accent;
        [self.reply placeUnder:self.tip.frame visible:(self.panel.screen ?: [NSScreen mainScreen]).visibleFrame];
    }
}

// ---- what clicks, presses and keys do ------------------------------------------------------------------

// single click: play the spoken report, or stop it; the reply counts as read. In single-core mode a click
// on one of the small cores brings it to the middle instead (and keeps it there).
- (void)coreClicked:(NSString *)sid {
    Session *s = session_by_sid(sid);
    if (!s) return;
    if (g_single && ![sid isEqualToString:g_focus]) {
        g_focus = sid;
        g_focus_pinned = YES;
        self.focusSince = wall();
        return;
    }
    if ([sid isEqualToString:self.pendingSid]) { // a recognized reply is waiting: the click sends or cancels it
        if (g_confirm_send) [self sendPending];
        else [self dropPending];
        return;
    }
    s->unread = 0;
    self.hoverSince = 0; // the tooltip shows without the usual pause
    if (!g_voice) return;
    if ([self.speaker has:sid] || [self.summarizing containsObject:sid]) {
        [self.speaker stop:sid];
        [self.summarizing removeObject:sid];
    } else [self report:s urgent:NO];
}

// ---- voice output -------------------------------------------------------------------------------------

// Says what a session has to report: the permission request while it needs you, else its last reply,
// summed up. Starts with where it comes from: "notes 完成了：…".
- (void)report:(Session *)s urgent:(BOOL)urgent {
    BOOL zh = self.speaker.chinese;
    NSString *sid = sid_of(s), *name = @(s->name[0] ? s->name : "claude") ?: @"claude";
    float vol = s->vol;
    NSString *notice = msg_text(s, @"notice"), *reply = msg_text(s, @"reply");
    if (session_shown(s, wall()) == ST_WAIT || (s->overlay && s->state == ST_WAIT)) {
        NSString *what = notice ? speech_clean(notice, zh) : (zh ? @"正在等你核准" : @"is waiting for your approval");
        [self.speaker say:[NSString stringWithFormat:zh ? @"%@ 需要你：%@" : @"%@ needs you: %@", name, what] sid:sid volume:vol urgent:YES];
        return;
    }
    if (!reply) {
        [self.speaker say:[NSString stringWithFormat:zh ? @"%@ 還沒有回覆" : @"%@ has no reply yet", name] sid:sid volume:vol urgent:urgent];
        return;
    }
    NSString *(^line)(NSString *) = ^NSString *(NSString *summary) {
        return [NSString stringWithFormat:zh ? @"%@ 完成了：%@" : @"%@ finished: %@", name, summary];
    };
    NSString *saved = g_summarize_claude ? saved_summary(s, reply) : nil;
    if (!g_summarize_claude || saved) {
        [self.speaker say:line(saved ?: speech_local_summary(reply, zh)) sid:sid volume:vol urgent:urgent];
        return;
    }
    // ask claude -p in the background; the local summary if it fails or takes longer than 15 s
    [self.summarizing addObject:sid];
    __weak AppDelegate *me = self;
    speech_claude_summary(g_claude, reply, zh, 15, ^(NSString *summary) {
        AppDelegate *d = me;
        if (!d || ![d.summarizing containsObject:sid]) return; // stopped meanwhile
        [d.summarizing removeObject:sid];
        Session *now = session_by_sid(sid);
        if (!now) return;
        if (summary) msg_save_summary(now, reply, summary);
        [d.speaker say:line(summary ?: speech_local_summary(reply, zh)) sid:sid volume:now->vol urgent:urgent];
    });
}

// New replies and new permission requests. With --auto-speak a reply is read out if you were at the
// keyboard or mouse in the last 60 seconds, and a permission request jumps the queue. What was already
// there when the panel started is never read out.
- (void)watchForNews:(double)now {
    Room *room = &g_room;
    double idle = CGEventSourceSecondsSinceLastEventType(kCGEventSourceStateCombinedSessionState, kCGAnyInputEventType);
    for (int i = 0; i < room->n; i++) {
        Session *s = &room->s[i];
        NSString *sid = sid_of(s);
        double replyAt = s->unread ? s->burst_at : 0, waitAt = s->state == ST_WAIT ? s->state_t : 0;
        NSArray *was = self.seen[sid];
        self.seen[sid] = @[@(fmax(replyAt, [was[0] doubleValue])), @(fmax(waitAt, [was[1] doubleValue]))];
        if (!was || !g_auto_speak || s->muted) continue;
        if (waitAt > [was[1] doubleValue]) [self report:s urgent:YES];
        else if (replyAt > [was[0] doubleValue] && idle < 60) { s->unread = 0; [self report:s urgent:NO]; }
    }
    (void)now;
}

// double click: a shockwave ring, and a reply box opens under the session's tooltip
- (void)coreDoubleClicked:(NSString *)sid {
    Session *s = session_by_sid(sid);
    if (!s) return;
    if ([sid isEqualToString:self.pendingSid]) { [self dropPending]; return; } // counts as a click: cancel
    s->ring_at = wall();
    s->unread = 0;
    [self openReplyFor:s];
}

- (void)openReplyFor:(Session *)s {
    if (!strcmp(s->sid, "ghost")) return;
    Col c = s->aura_a > 0.01f ? s->aura : s->cur.eh;
    NSString *name = @(s->name[0] ? s->name : "claude") ?: @"claude";
    [self.reply openFor:sid_of(s) name:name accent:[NSColor colorWithSRGBRed:c.r green:c.g blue:c.b alpha:1]];
    self.hoverSince = 0;
    [self trackMouse:wall()]; // the tooltip and the box in place at once
}

- (void)replyBox:(ReplyBox *)box send:(NSString *)text to:(NSString *)sid { (void)box; [self deliver:text to:sid]; }
- (void)replyBoxClosed:(ReplyBox *)box { (void)box; [self.tip orderOut:nil]; }

- (void)coreDragOver:(NSString *)sid {
    if ([sid isEqualToString:self.dragSid]) return;
    if (self.dragSid && ![self.dragSid isEqualToString:self.pttSid]) [self.overlay removeObjectForKey:self.dragSid];
    self.dragSid = sid;
    if (sid && !self.pttSid) self.overlay[sid] = @(ST_TRANSCRIBE);
}

- (BOOL)coreDropImages:(NSPasteboard *)pb on:(NSString *)sid {
    Session *s = session_by_sid(sid);
    if (!s) return NO;
    self.overlay[sid] = @(ST_TRANSCRIBE); // it reads them a moment longer
    __weak AppDelegate *me = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.4 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        AppDelegate *d = me;
        if (d && ![sid isEqualToString:d.pttSid] && ![sid isEqualToString:d.dragSid]) [d.overlay removeObjectForKey:sid];
    });
    [self openReplyFor:s];
    return [self.reply takeImagesFrom:pb forSid:sid];
}

// --reply-shot FILE: the reply box of the first session with some text and an image token, under its tooltip,
// saved as a PNG (the way to check a change to the box without clicking it)
- (void)replyShot {
    const Room *room = shown_room();
    if (!room->n) { [NSApp terminate:nil]; return; }
    Session *s = (Session *)&room->s[0];
    [self openReplyFor:s];
    [self.reply type:@"Looks good, now run the tests "];
    [self.reply addImages:@[[NSURL fileURLWithPath:@"/tmp/screenshot 1.png"]] forSid:sid_of(s)];
    [self trackMouse:wall()];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        DBG("reply box key: %d, first responder is the field: %d", self.reply.view.window.isKeyWindow,
            [self.reply.view.window.firstResponder isKindOfClass:[NSTextView class]]);
        NSRect a = self.tip.frame, b = self.reply.view.window.frame, u = NSUnionRect(a, b);
        u = NSInsetRect(u, -12, -12);
        NSImage *img = [[NSImage alloc] initWithSize:u.size];
        [img lockFocus];
        [[NSColor colorWithSRGBRed:0.05 green:0.05 blue:0.07 alpha:1] setFill];
        NSRectFill(NSMakeRect(0, 0, u.size.width, u.size.height));
        for (NSWindow *w in @[self.tip, self.reply.view.window]) {
            NSView *v = w.contentView;
            NSBitmapImageRep *rep = [v bitmapImageRepForCachingDisplayInRect:v.bounds];
            [v cacheDisplayInRect:v.bounds toBitmapImageRep:rep];
            [rep drawInRect:NSMakeRect(w.frame.origin.x - u.origin.x, w.frame.origin.y - u.origin.y, w.frame.size.width, w.frame.size.height)
                   fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1 respectFlipped:YES hints:nil];
        }
        [img unlockFocus];
        NSBitmapImageRep *out = [[NSBitmapImageRep alloc] initWithData:img.TIFFRepresentation];
        [[out representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@(g_reply_shot) atomically:YES];
        [NSApp terminate:nil];
    });
}

// "Bring to front": the iTerm2 pane of that session comes to the front
- (void)bringToFront:(NSString *)sid {
    Session *s = session_by_sid(sid);
    if (!s) return;
    s->ring_at = wall();
    NSString *iterm = msg_text(s, @"iterm");
    NSRange colon = [iterm rangeOfString:@":"];
    if (colon.location != NSNotFound) iterm = [iterm substringFromIndex:colon.location + 1]; // "w0t0p1:UUID" -> UUID
    if (iterm.length) [self focusItermSession:iterm];
}

- (void)focusItermSession:(NSString *)uuid {
    static NSString *script =
        @"on run argv\n"
         "  tell application \"iTerm2\"\n"
         "    repeat with w in windows\n"
         "      repeat with t in tabs of w\n"
         "        repeat with s in sessions of t\n"
         "          if (id of s as text) is (item 1 of argv) then\n"
         "            select w\n"
         "            tell t to select\n"
         "            tell s to select\n"
         "            activate\n"
         "            return\n"
         "          end if\n"
         "        end repeat\n"
         "      end repeat\n"
         "    end repeat\n"
         "  end tell\n"
         "end run\n";
    NSTask *task = [[NSTask alloc] init];
    task.executableURL = [NSURL fileURLWithPath:@"/usr/bin/osascript"];
    task.arguments = @[@"-e", script, uuid];
    task.standardOutput = task.standardError = [NSFileHandle fileHandleWithNullDevice];
    [task launchAndReturnError:NULL]; // asynchronous: the panel never waits for iTerm2
}

// ---- voice input: hold, speak, release; the text is shown for a moment, then typed into that session --

// long press (or the hotkey): start listening, or say why it cannot
- (void)corePressBegan:(NSString *)sid {
    if (!g_voice || !session_by_sid(sid)) return;
    if (self.pttSid) [self cancelPtt];
    [self dropPending];
    NSString *why = [self.listener unavailableReason];
    DBG("press began on %s: %s", sid.UTF8String, why ? why.UTF8String : "listening");
    if (why) { [self fail:sid because:why]; return; }
    [self.speaker stop:self.speaker.speakingSid]; // don't talk over you
    self.pttSid = sid;
    self.overlay[sid] = @(ST_LISTEN);
    [self setEscapeHotkey:YES];
    __weak AppDelegate *me = self;
    BOOL ok = [self.listener start:^(NSString *partial) {
        [me showPreview:partial forSid:sid hint:@"…"];
    }];
    DBG("recording %s", ok ? "started" : "FAILED to start");
    if (!ok) { [self cancelPtt]; [self fail:sid because:@"麥克風沒辦法開始錄音"]; return; }
    [self showPreview:@"" forSid:sid hint:@"說吧，放開就結束（Esc 取消）"];
}

- (void)corePressEnded:(NSString *)sid {
    DBG("press ended on %s (recording for %s)", sid.UTF8String, self.pttSid.UTF8String ?: "nothing");
    if (![sid isEqualToString:self.pttSid]) return;
    [self setEscapeHotkey:NO];
    self.overlay[sid] = @(ST_TRANSCRIBE);
    __weak AppDelegate *me = self;
    [self.listener finish:^(NSString *text) {
        AppDelegate *d = me;
        DBG("recognized %lu characters%s", (unsigned long)text.length, d && [d.pttSid isEqualToString:sid] ? "" : " (cancelled meanwhile)");
        if (!d || ![d.pttSid isEqualToString:sid]) return; // cancelled meanwhile
        [d.overlay removeObjectForKey:sid];
        d.pttSid = nil;
        if (!text) {
            [d.preview orderOut:nil];
            NSString *dev = d.listener.silentDevice;
            if (dev) [d fail:sid because:[NSString stringWithFormat:@"麥克風完全沒有聲音，用的是 %@。可以用 --mic 指定別的輸入裝置", dev]];
            return;
        }
        d.pendingSid = sid;
        d.pendingText = text;
        d.pendingHeld = NO;
        d.pendingUntil = wall() + (g_confirm_send ? 15 : PREVIEW_FOR);
        [d showPreview:text forSid:sid hint:g_confirm_send ? @"再點一下核心送出" : @"點核心取消"];
    }];
}

// Esc while recording: nothing is sent, the glow shrinks and fades back
- (void)cancelPtt {
    if (!self.pttSid) return;
    [self.listener cancel];
    [self.overlay removeObjectForKey:self.pttSid];
    self.pttSid = nil;
    [self setEscapeHotkey:NO];
    [self.view cancelPress];
    [self.preview orderOut:nil];
}

- (void)coreTouched:(NSString *)sid {
    Session *s = session_by_sid(sid);
    if (s) { s->press_at = wall(); s->press_x = s->look_x_to; s->press_y = s->look_y_to; } // the patch under the pointer goes in
    if ([sid isEqualToString:self.pendingSid]) self.pendingHeld = YES; // decided when the click is known
}

- (void)dropPending {
    self.pendingSid = self.pendingText = nil;
    [self.preview orderOut:nil];
}

// the recognized text under the core it is for, with a hint of what a click does
- (void)showPreview:(NSString *)text forSid:(NSString *)sid hint:(NSString *)hint {
    Session *s = session_by_sid(sid);
    const Room *room = shown_room();
    if (!s) return;
    if (!self.preview) {
        TipView *v;
        self.preview = [self newTipPanel:&v];
        v.maxBodyLines = 0; // all of it: this is what will be sent
        self.previewView = v;
    }
    NSString *name = @(s->name[0] ? s->name : "claude") ?: @"claude";
    self.previewView.title = [NSString stringWithFormat:@"→ %@ · %@", name, hint];
    self.previewView.body = text.length ? text : @" ";
    Col c = s->aura_a > 0.01f ? s->aura : s->cur.eh;
    self.previewView.accent = [NSColor colorWithSRGBRed:c.r green:c.g blue:c.b alpha:1];
    NSRect core = [self.view coreRect:(int)(s - room->s)];
    NSRect inScreen = [self.panel convertRectToScreen:[self.view convertRect:core toView:nil]];
    // beside the panel, level with the core, like the tooltip: wide enough for the whole text
    NSSize sz = [self.previewView fit];
    NSRect vis = (self.panel.screen ?: [NSScreen mainScreen]).visibleFrame;
    if (sz.height > vis.size.height - 20) sz.height = vis.size.height - 20;
    NSRect f = NSMakeRect(NSMinX(self.panel.frame) - sz.width - 8, NSMidY(inScreen) - sz.height / 2, sz.width, sz.height);
    if (NSMinX(f) < NSMinX(vis)) f.origin.x = NSMinX(vis);
    if (NSMaxY(f) > NSMaxY(vis)) f.origin.y = NSMaxY(vis) - sz.height;
    if (NSMinY(f) < NSMinY(vis)) f.origin.y = NSMinY(vis);
    [self.preview setFrame:f display:YES];
    [self.previewView setNeedsDisplay:YES];
    [self.preview orderFrontRegardless];
    [self.tip orderOut:nil];
}

// Sends the recognized reply, unless Claude is busy: then it asks first, since a reply typed while it works
// waits until the current step is done, and interrupting means pressing Esc in that session.
- (void)sendPending {
    NSString *sid = self.pendingSid, *text = self.pendingText;
    DBG("sending to %s", sid.UTF8String);
    [self dropPending];
    [self deliver:text to:sid];
}

// a reply (spoken or typed) for a session: sent at once, or after asking while Claude is working
- (void)deliver:(NSString *)text to:(NSString *)sid {
    Session *s = session_by_sid(sid);
    if (!s || !text) return;
    if (s->state == ST_WORK || s->state == ST_THINK) {
        [self.speaker say:@"還在工作中，要打斷嗎？" sid:sid volume:s->vol urgent:YES];
        NSMenu *m = [[NSMenu alloc] initWithTitle:@""];
        m.autoenablesItems = NO;
        NSArray *items = @[@[@"Send after this step (no interrupt)", @0], @[@"Interrupt and send", @1], @[@"Cancel", @2]];
        for (NSArray *it in items) {
            NSMenuItem *i = [m addItemWithTitle:it[0] action:@selector(menuSendChoice:) keyEquivalent:@""];
            i.target = self;
            i.tag = [it[1] integerValue];
            i.representedObject = @[sid, text];
        }
        const Room *room = shown_room();
        NSRect core = [self.view coreRect:(int)(s - room->s)];
        [m popUpMenuPositioningItem:nil atLocation:NSMakePoint(NSMidX(core), NSMidY(core)) inView:self.view];
        return;
    }
    [self send:text to:sid interrupt:NO];
}

- (void)menuSendChoice:(NSMenuItem *)i {
    NSArray *a = i.representedObject;
    if (i.tag == 2) return;
    [self send:a[1] to:a[0] interrupt:i.tag == 1];
}

// The ERROR look for a few seconds, and the reason out loud
- (void)fail:(NSString *)sid because:(NSString *)why {
    DBG("ERROR on %s: %s", sid.UTF8String, why.UTF8String);
    Session *s = session_by_sid(sid);
    self.errorUntil[sid] = @(wall() + 3);
    [self.speaker say:why sid:sid volume:s ? s->vol : 0.8f urgent:YES];
}

// Types text + Return into the session's iTerm2 pane. AppleScript first (it answers at once); if macOS
// does not let the panel control iTerm2, iTerm2's Python API through pane_api.py.
- (void)send:(NSString *)text to:(NSString *)sid interrupt:(BOOL)interrupt {
    Session *s = session_by_sid(sid);
    if (!s) return;
    NSString *name = @(s->name[0] ? s->name : "claude") ?: @"claude";
    NSCharacterSet *ctl = [NSCharacterSet controlCharacterSet];
    text = [[text componentsSeparatedByCharactersInSet:ctl] componentsJoinedByString:@" "]; // one line, no escapes
    if (g_demo_n) { // made-up sessions have no pane: pretend
        s->ring_at = wall();
        [self.speaker say:[NSString stringWithFormat:@"示範：已送給 %@：%@", name, text] sid:sid volume:s->vol urgent:NO];
        return;
    }
    NSString *iterm = msg_text(s, @"iterm");
    NSRange colon = [iterm rangeOfString:@":"];
    if (colon.location != NSNotFound) iterm = [iterm substringFromIndex:colon.location + 1];
    if (!iterm.length) { [self fail:sid because:@"不知道這個 session 在哪個 iTerm2 窗格，需要用 DODECA_RICH 啟動 hook"]; return; }
    static NSString *script =
        @"on run argv\n"
         "  tell application \"iTerm2\"\n"
         "    repeat with w in windows\n"
         "      repeat with t in tabs of w\n"
         "        repeat with s in sessions of t\n"
         "          if (id of s as text) is (item 1 of argv) then\n"
         "            if (item 3 of argv) is \"1\" then\n"
         "              tell s to write text (ASCII character 27) newline no\n"
         "              delay 0.4\n"
         "            end if\n"
         "            tell s to write text (item 2 of argv) newline no\n"
         "            delay 0.15\n"
         "            tell s to write text (ASCII character 13) newline no\n"
         "            return \"ok\"\n"
         "          end if\n"
         "        end repeat\n"
         "      end repeat\n"
         "    end repeat\n"
         "  end tell\n"
         "  return \"missing\"\n"
         "end run\n";
    NSTask *task = [[NSTask alloc] init];
    task.executableURL = [NSURL fileURLWithPath:@"/usr/bin/osascript"];
    task.arguments = @[@"-e", script, iterm, text, interrupt ? @"1" : @"0"];
    NSPipe *out = [NSPipe pipe];
    task.standardOutput = out;
    task.standardError = [NSFileHandle fileHandleWithNullDevice];
    __weak AppDelegate *me = self;
    task.terminationHandler = ^(NSTask *t) {
        NSString *r = [[[NSString alloc] initWithData:[out.fileHandleForReading readDataToEndOfFile] encoding:NSUTF8StringEncoding]
            stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
        int status = t.terminationStatus;
        DBG("osascript exit %d, answer \"%s\"", status, r.UTF8String ?: "");
        dispatch_async(dispatch_get_main_queue(), ^{
            if (status == 0) [me sent:r to:sid];
            else [me sendWithPythonApi:text iterm:iterm to:sid interrupt:interrupt]; // e.g. Automation not allowed
        });
    };
    if (![task launchAndReturnError:NULL]) [self sendWithPythonApi:text iterm:iterm to:sid interrupt:interrupt];
}

- (void)sendWithPythonApi:(NSString *)text iterm:(NSString *)iterm to:(NSString *)sid interrupt:(BOOL)interrupt {
    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *it2run = @"/Applications/iTerm.app/Contents/Resources/it2run";
    NSString *scripts = [@"~/Library/Application Support/iTerm2/Scripts" stringByExpandingTildeInPath];
    NSString *mine = [[[NSBundle mainBundle].bundlePath stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"pane_api.py"];
    NSString *copy = [scripts stringByAppendingPathComponent:@"vigil_pane.py"];
    if (![fm isExecutableFileAtPath:it2run] || ![fm fileExistsAtPath:scripts] || ![fm fileExistsAtPath:mine]) {
        [self fail:sid because:@"沒辦法控制 iTerm2：請允許面板控制 iTerm2，或打開 iTerm2 的 Python API"];
        return;
    }
    if (![fm contentsEqualAtPath:mine andPath:copy]) { [fm removeItemAtPath:copy error:NULL]; [fm copyItemAtPath:mine toPath:copy error:NULL]; }
    NSString *result = [NSTemporaryDirectory() stringByAppendingPathComponent:[NSString stringWithFormat:@"vigil-send-%d-%u", (int)getpid(), arc4random()]];
    NSTask *task = [[NSTask alloc] init];
    task.executableURL = [NSURL fileURLWithPath:it2run];
    task.arguments = @[@"vigil_pane.py", @"send", iterm, text, result, interrupt ? @"1" : @"0"];
    task.standardOutput = task.standardError = [NSFileHandle fileHandleWithNullDevice];
    if (![task launchAndReturnError:NULL]) { [self fail:sid because:@"沒辦法啟動 iTerm2 的 Python API"]; return; }
    [self waitForResult:result sid:sid tries:25]; // it2run returns at once: the script writes its answer to a file
}

- (void)waitForResult:(NSString *)path sid:(NSString *)sid tries:(int)tries {
    NSString *r = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:NULL];
    if (r) { [[NSFileManager defaultManager] removeItemAtPath:path error:NULL]; [self sent:r to:sid]; return; }
    if (tries <= 0) { [self fail:sid because:@"iTerm2 的 Python API 沒有回應，請確認已經打開"]; return; }
    __weak AppDelegate *me = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.2 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        [me waitForResult:path sid:sid tries:tries - 1];
    });
}

- (void)sent:(NSString *)result to:(NSString *)sid {
    Session *s = session_by_sid(sid);
    if ([result isEqualToString:@"ok"]) { if (s) s->ring_at = wall(); return; }
    [self fail:sid because:[result isEqualToString:@"missing"] ? @"找不到這個 session 的 iTerm2 窗格，可能已經關掉了" : @"送出失敗"];
}

- (NSMenu *)menuForCore:(NSString *)sid {
    Session *s = session_by_sid(sid);
    if (!s) return nil;
    NSMenu *m = [[NSMenu alloc] initWithTitle:@""];
    m.autoenablesItems = NO;
    if (!strcmp(s->sid, "ghost")) { [m addItem:[self settingsItem]]; return m; } // no session yet: only the settings
    NSString *reply = msg_text(s, @"reply");
    NSMenuItem *replay = [m addItemWithTitle:@"Replay" action:@selector(menuReplay:) keyEquivalent:@""];
    replay.enabled = g_voice && (reply != nil || msg_text(s, @"notice") != nil);
    NSMenuItem *copy = [m addItemWithTitle:@"Copy reply" action:@selector(menuCopy:) keyEquivalent:@""];
    copy.enabled = reply != nil;
    NSMenuItem *mute = [m addItemWithTitle:@"Mute this session" action:@selector(menuMute:) keyEquivalent:@""];
    mute.state = s->muted ? NSControlStateValueOn : NSControlStateValueOff;
    [m addItem:[NSMenuItem separatorItem]];
    [m addItemWithTitle:@"Bring to front" action:@selector(menuFront:) keyEquivalent:@""];
    [m addItemWithTitle:g_single ? @"All cores" : @"Single core" action:@selector(menuMode:) keyEquivalent:@""];
    if (g_single && g_focus_pinned) [m addItemWithTitle:@"Follow automatically" action:@selector(menuFollow:) keyEquivalent:@""];
    if (g_single && positions()[screen_key(self.panel.screen ?: [NSScreen mainScreen])]) [m addItemWithTitle:@"Reset position" action:@selector(menuResetPosition:) keyEquivalent:@""];
    for (NSMenuItem *i in m.itemArray) { i.target = self; i.representedObject = sid; }
    [m addItem:[NSMenuItem separatorItem]];
    [m addItem:[self settingsItem]];
    return m;
}

// ---- Settings...: the window (mac/settings.m); every choice applies at once and is remembered (see load_settings)
- (NSMenuItem *)settingsItem {
    NSMenuItem *i = [[NSMenuItem alloc] initWithTitle:@"Settings..." action:@selector(menuOpenSettings:) keyEquivalent:@""];
    i.target = self;
    return i;
}
- (void)menuOpenSettings:(NSMenuItem *)i { (void)i; [self openSettings]; }
- (void)openSettings {
    if (!self.settings) self.settings = [[SettingsWindow alloc] initWithHost:self];
    [self.settings showOn:self.panel.screen behavior:self.panel.collectionBehavior];
}

// SettingsHost: what the window reads and changes
- (id)setting:(NSString *)key {
    NSDictionary *d = @{@"shape": @(g_shape_name), @"glow": @(g_glow), @"scale": @(g_scale), @"core": @(g_core), @"frame": @(g_frame),
                        @"others": @(g_others != 0), @"wheelSpin": @(g_wheel_spin), @"tipMono": @(g_tip_mono != 0), @"fontSize": @(g_font_size),
                        @"tipSize": @(g_tip_size), @"tipColor": @(g_tip_bg), @"textColor": @(g_text_col), @"tipCustom": @(g_tip_custom),
                        @"textCustom": @(g_text_custom), @"tipMiddle": @(g_tip_middle != 0), @"labelsHover": @(g_labels_hover != 0),
                        @"away": @(g_away_after), @"voice": @(g_voice != 0), @"autoSpeak": @(g_auto_speak != 0),
                        @"summarize": @(g_summarize_claude != 0), @"confirmSend": @(g_confirm_send != 0), @"fontName": g_font_name};
    return d[key];
}
- (void)chooseSetting:(NSString *)key value:(id)value { [self applySetting:key value:value]; save_settings(); }
- (BOOL)settingsSaved { return [[NSFileManager defaultManager] fileExistsAtPath:settings_file()]; }
- (void)resetSettings { [self menuResetSettings:nil]; }
- (const Shape *)shape { return &g_shape; }
- (void)drawFocusFrame:(CGContextRef)ctx rect:(NSRect)r session:(const Session *)s now:(double)now { focus_frame(ctx, r, s, now); }
- (BOOL)canSummarize { return g_claude != nil; }

- (void)applySetting:(NSString *)key value:(id)v {
    if ([key isEqualToString:@"voice"]) [self setVoice:[v boolValue]];
    else if ([key isEqualToString:@"autoSpeak"]) { if ([v boolValue] && !g_voice) [self setVoice:YES]; g_auto_speak = [v boolValue]; }
    else if ([key isEqualToString:@"summarize"]) { if ([v boolValue] && !g_voice) [self setVoice:YES]; g_summarize_claude = [v boolValue] && g_claude; }
    else if ([key isEqualToString:@"confirmSend"]) g_confirm_send = [v boolValue];
    else if ([key isEqualToString:@"glow"]) g_glow = [v floatValue];
    else if ([key isEqualToString:@"away"]) g_away_after = [v doubleValue];
    else if ([key isEqualToString:@"frame"]) snprintf(g_frame, sizeof g_frame, "%s", [v UTF8String]);
    else if ([key isEqualToString:@"core"]) g_core = [v floatValue];
    else if ([key isEqualToString:@"others"]) g_others = [v boolValue];
    else if ([key isEqualToString:@"wheelSpin"]) g_wheel_spin = [v intValue];
    else if ([key isEqualToString:@"scale"]) { g_scale = [v floatValue]; [self place]; }
    else if ([key isEqualToString:@"tipMono"] || [key isEqualToString:@"tipSize"] || [key isEqualToString:@"tipMiddle"] || [key isEqualToString:@"tipColor"]
             || [key isEqualToString:@"textColor"] || [key isEqualToString:@"tipCustom"] || [key isEqualToString:@"textCustom"]) {
        if ([key isEqualToString:@"tipMono"]) g_tip_mono = [v boolValue];
        else if ([key isEqualToString:@"tipSize"]) g_tip_size = [v floatValue];
        else if ([key isEqualToString:@"tipMiddle"]) g_tip_middle = [v boolValue];
        else if ([key isEqualToString:@"tipColor"]) g_tip_bg = [v intValue];
        else if ([key isEqualToString:@"textColor"]) g_text_col = [v intValue];
        else if ([key isEqualToString:@"tipCustom"]) g_tip_custom = [v unsignedIntValue];
        else g_text_custom = [v unsignedIntValue];
        [self styleTips];
    }
    else if ([key isEqualToString:@"shape"]) {
        if (!shape_make(&g_shape, [v UTF8String])) return;
        snprintf(g_shape_name, sizeof g_shape_name, "%s", [v UTF8String]);
    } else if ([key isEqualToString:@"labels"] || [key isEqualToString:@"labelsHover"] || [key isEqualToString:@"fontSize"]) {
        if (![key isEqualToString:@"fontSize"]) g_labels_hover = [v boolValue];
        else {
            g_font_size = [v doubleValue];
            [self styleTips];
        }
        [self place];
    }
    [self.view relayout]; // every picture is drawn again with the new setting
    [self.settings changed];
}
- (void)menuResetSettings:(NSMenuItem *)i {
    (void)i;
    [[NSFileManager defaultManager] removeItemAtPath:settings_file() error:NULL];
    int voice = g_launch.voice;
    restore_launch();
    shape_make(&g_shape, g_shape_name);
    g_voice = !voice; [self setVoice:voice];
    g_auto_speak = g_launch.auto_speak; g_summarize_claude = g_launch.summarize && g_claude;
    [self styleTips];
    [self place];
    [self.view relayout];
    [self.settings changed];
}

- (void)menuReplay:(NSMenuItem *)i {
    Session *s = session_by_sid(i.representedObject);
    if (!s) return;
    [self.speaker stop:i.representedObject];
    s->unread = 0;
    [self report:s urgent:NO];
}
- (void)menuCopy:(NSMenuItem *)i {
    Session *s = session_by_sid(i.representedObject);
    NSString *reply = s ? msg_text(s, @"reply") : nil;
    if (!reply) return;
    [[NSPasteboard generalPasteboard] clearContents];
    [[NSPasteboard generalPasteboard] setString:reply forType:NSPasteboardTypeString];
}
- (void)menuMute:(NSMenuItem *)i { Session *s = session_by_sid(i.representedObject); if (s) s->muted = !s->muted; }
- (void)menuFront:(NSMenuItem *)i { [self bringToFront:i.representedObject]; }
- (void)menuMode:(NSMenuItem *)i { (void)i; [self toggleMode]; }
- (void)menuFollow:(NSMenuItem *)i { (void)i; g_focus_pinned = NO; self.focusSince = wall(); }
- (void)menuResetPosition:(NSMenuItem *)i {
    (void)i;
    save_position(self.panel.screen ?: [NSScreen mainScreen], NSMakePoint(CGFLOAT_MAX, 0)); // forget it
    [self place];
}

- (void)toggleMode {
    g_single = !g_single;
    g_focus_pinned = NO;
    [self place];
    [self.view relayout];
}

// Single-core mode: which session is in the middle. Pinned by a click, it stays. Otherwise it moves only
// when there is news, so it does not jump around while several sessions work: to a session that needs you,
// else to one with a reply newer than the current focus; a session that ended is replaced by the one
// active last. Every core gets its own aura colour.
- (void)updateFocus:(double)now {
    Room *room = g_room.n ? &g_room : &g_ghost;
    for (int i = 0; i < room->n; i++) { // colours: the first free one, starting from one picked by the folder name
        Session *s = &room->s[i];
        NSString *sid = sid_of(s);
        if (!self.hue[sid]) {
            NSMutableSet *used = [NSMutableSet set];
            for (int j = 0; j < room->n; j++) { NSNumber *h = self.hue[sid_of(&room->s[j])]; if (h) [used addObject:h]; }
            unsigned start = 0;
            for (const char *c = s->name; *c; c++) start = start * 31 + (unsigned char)*c;
            int pick = (int)(start % NHUE);
            for (int k = 0; k < NHUE; k++) if (![used containsObject:@((start + k) % NHUE)]) { pick = (int)((start + k) % NHUE); break; }
            self.hue[sid] = @(pick);
        }
        s->aura = HUES[self.hue[sid].intValue];
        s->aura_a = g_single && room->n > 1 && g_others ? TOUCH.aura : 0; // one core alone needs no name tag
    }
    if (!g_single) return;
    Session *cur = session_by_sid(g_focus);
    if (!cur) { g_focus_pinned = NO; g_focus = nil; }
    if (g_focus_pinned) return;
    Session *pick = NULL, *fresh = NULL, *recent = NULL;
    for (int i = 0; i < room->n; i++) {
        Session *s = &room->s[i];
        if (s->state == ST_WAIT && (!pick || s->state_t > pick->state_t)) pick = s;
        if (!fresh || s->burst_at > fresh->burst_at) fresh = s;
        if (!recent || s->last > recent->last) recent = s;
    }
    if (!pick && fresh && fresh->burst_at > self.focusSince) pick = fresh;
    if (!pick) pick = cur ?: recent;
    if (pick && ![sid_of(pick) isEqualToString:g_focus]) { g_focus = sid_of(pick); self.focusSince = now; }
}

// The panel's own states for each session, and the level that drives them. Away (no keyboard or mouse
// input for --away seconds, asked of the system, which needs no permission): every core shows STANDBY,
// except one that needs you. Back again: cores with an unread reply flare up once to get your attention.
- (void)updateOverlays:(double)now {
    double idle = CGEventSourceSecondsSinceLastEventType(kCGEventSourceStateCombinedSessionState, kCGAnyInputEventType);
    BOOL away = g_away_after > 0 && idle > g_away_after;
    Room *room = g_room.n ? &g_room : &g_ghost;
    if (self.away && !away)
        for (int i = 0; i < room->n; i++) if (room->s[i].unread) room->s[i].burst_at = now;
    self.away = away;
    for (int i = 0; i < room->n; i++) {
        Session *s = &room->s[i];
        NSNumber *ov = self.overlay[sid_of(s)];
        s->overlay = 0;
        int own = session_shown(s, now);
        s->overlay = ov ? ov.intValue : away && own != ST_WAIT ? ST_STANDBY : 0;
        if (!ov && [self.speaker.speakingSid isEqualToString:sid_of(s)]) s->overlay = ST_SPEAK;
        if ([self.errorUntil[sid_of(s)] doubleValue] > now) s->overlay = ST_ERROR;
        float lvl = 0;
        if (s->overlay == ST_SPEAK) lvl = self.speaker.level;
        else if (s->overlay == ST_LISTEN) lvl = self.listener.level;
        s->level += (fmaxf(0, fminf(1, lvl)) - s->level) * 0.35f;
    }
    for (NSString *sid in self.overlay.allKeys) if (!session_by_sid(sid)) [self.overlay removeObjectForKey:sid];
}

// The session the hotkey talks to: one that needs you, else the one that finished last, else the one
// active last.
- (Session *)mostWanted {
    Room *room = g_room.n ? &g_room : NULL;
    Session *best = NULL;
    for (int pass = 0; room && pass < 3 && !best; pass++) {
        double bestT = -1e18;
        for (int i = 0; i < room->n; i++) {
            Session *s = &room->s[i];
            double t = pass == 0 ? (s->state == ST_WAIT ? s->state_t : -1e19) : pass == 1 ? s->burst_at : s->last;
            if (t > -1e8 && t > bestT) { bestT = t; best = s; }
        }
    }
    return best;
}

// ---- global hotkeys (Carbon): push-to-talk, and Esc only while recording -----------------------------
enum { HK_PTT = 1, HK_ESC = 2, HK_MODE = 3 };
static EventHotKeyRef g_ptt_key, g_esc_key, g_mode_key;

static OSStatus hotkey_event(EventHandlerCallRef next, EventRef ev, void *user) {
    (void)next;
    EventHotKeyID id;
    GetEventParameter(ev, kEventParamDirectObject, typeEventHotKeyID, NULL, sizeof id, NULL, &id);
    AppDelegate *me = (__bridge AppDelegate *)user;
    BOOL down = GetEventKind(ev) == kEventHotKeyPressed;
    dispatch_async(dispatch_get_main_queue(), ^{ [me hotkey:(int)id.id down:down]; });
    return noErr;
}

- (void)hotkey:(int)which down:(BOOL)down {
    if (which == HK_ESC) { if (down) [self cancelPtt]; return; }
    if (which == HK_MODE) { if (down) [self toggleMode]; return; }
    if (down) {
        Session *s = g_single ? session_by_sid(g_focus) : NULL; // single core: talk to the one in the middle
        if (!s) s = [self mostWanted];
        if (!s) { NSBeep(); return; }
        self.pttByHotkey = YES;
        [self corePressBegan:sid_of(s)];
    } else if (self.pttByHotkey) {
        self.pttByHotkey = NO;
        if (self.pttSid) [self corePressEnded:self.pttSid];
    }
}

// "ctrl+opt+v" -> modifiers and a virtual key code; NO if it does not parse
static BOOL parse_hotkey(const char *spec, UInt32 *mods, UInt32 *key) {
    static const struct { const char *name; UInt32 code; } KEYS[] = {
        {"space", kVK_Space}, {"return", kVK_Return}, {"enter", kVK_Return}, {"tab", kVK_Tab},
        {"f1", kVK_F1}, {"f2", kVK_F2}, {"f3", kVK_F3}, {"f4", kVK_F4}, {"f5", kVK_F5}, {"f6", kVK_F6},
        {"f7", kVK_F7}, {"f8", kVK_F8}, {"f9", kVK_F9}, {"f10", kVK_F10}, {"f11", kVK_F11}, {"f12", kVK_F12},
        {"a", kVK_ANSI_A}, {"b", kVK_ANSI_B}, {"c", kVK_ANSI_C}, {"d", kVK_ANSI_D}, {"e", kVK_ANSI_E}, {"f", kVK_ANSI_F},
        {"g", kVK_ANSI_G}, {"h", kVK_ANSI_H}, {"i", kVK_ANSI_I}, {"j", kVK_ANSI_J}, {"k", kVK_ANSI_K}, {"l", kVK_ANSI_L},
        {"m", kVK_ANSI_M}, {"n", kVK_ANSI_N}, {"o", kVK_ANSI_O}, {"p", kVK_ANSI_P}, {"q", kVK_ANSI_Q}, {"r", kVK_ANSI_R},
        {"s", kVK_ANSI_S}, {"t", kVK_ANSI_T}, {"u", kVK_ANSI_U}, {"v", kVK_ANSI_V}, {"w", kVK_ANSI_W}, {"x", kVK_ANSI_X},
        {"y", kVK_ANSI_Y}, {"z", kVK_ANSI_Z}, {"0", kVK_ANSI_0}, {"1", kVK_ANSI_1}, {"2", kVK_ANSI_2}, {"3", kVK_ANSI_3},
        {"4", kVK_ANSI_4}, {"5", kVK_ANSI_5}, {"6", kVK_ANSI_6}, {"7", kVK_ANSI_7}, {"8", kVK_ANSI_8}, {"9", kVK_ANSI_9},
    };
    char buf[64];
    snprintf(buf, sizeof buf, "%s", spec);
    *mods = 0;
    *key = UINT32_MAX;
    for (char *tok = strtok(buf, "+"); tok; tok = strtok(NULL, "+")) {
        for (char *c = tok; *c; c++) *c = (char)tolower((unsigned char)*c);
        if (!strcmp(tok, "cmd") || !strcmp(tok, "command")) *mods |= cmdKey;
        else if (!strcmp(tok, "opt") || !strcmp(tok, "option") || !strcmp(tok, "alt")) *mods |= optionKey;
        else if (!strcmp(tok, "ctrl") || !strcmp(tok, "control")) *mods |= controlKey;
        else if (!strcmp(tok, "shift")) *mods |= shiftKey;
        else {
            for (size_t i = 0; i < sizeof KEYS / sizeof KEYS[0]; i++) if (!strcmp(tok, KEYS[i].name)) *key = KEYS[i].code;
            if (*key == UINT32_MAX) return NO;
        }
    }
    return *key != UINT32_MAX && *mods != 0;
}

- (void)registerHotkeys {
    EventTypeSpec types[] = {{kEventClassKeyboard, kEventHotKeyPressed}, {kEventClassKeyboard, kEventHotKeyReleased}};
    InstallApplicationEventHandler(NewEventHandlerUPP(hotkey_event), 2, types, (__bridge void *)self, NULL);
    UInt32 mods, key;
    if (strcmp(g_mode_hotkey, "none")) {
        if (!parse_hotkey(g_mode_hotkey, &mods, &key)) fprintf(stderr, "vigil-panel: cannot read --mode-hotkey %s\n", g_mode_hotkey);
        else if (RegisterEventHotKey(key, mods, (EventHotKeyID){'dodc', HK_MODE}, GetApplicationEventTarget(), 0, &g_mode_key) != noErr)
            fprintf(stderr, "vigil-panel: hotkey %s is taken by another app\n", g_mode_hotkey);
    }
}

// push-to-talk's hotkey is only ours while voice is on
- (void)setPttHotkey:(BOOL)on {
    UInt32 mods, key;
    if (!on) { if (g_ptt_key) { UnregisterEventHotKey(g_ptt_key); g_ptt_key = NULL; } return; }
    if (g_ptt_key || !strcmp(g_hotkey, "none")) return;
    if (!parse_hotkey(g_hotkey, &mods, &key)) { fprintf(stderr, "vigil-panel: cannot read --hotkey %s\n", g_hotkey); return; }
    if (RegisterEventHotKey(key, mods, (EventHotKeyID){'dodc', HK_PTT}, GetApplicationEventTarget(), 0, &g_ptt_key) != noErr)
        fprintf(stderr, "vigil-panel: hotkey %s is taken by another app\n", g_hotkey);
}

// voice on or off while running: the speaker and the listener are made the first time they are needed
- (void)setVoice:(BOOL)on {
    g_voice = on;
    if (on && !self.speaker) {
        self.speaker = [[Speaker alloc] initWithVoice:g_voice_id rate:g_rate];
        self.listener = [[Listener alloc] initWithLocale:g_listen_locale mic:g_mic];
        if (g_debug) self.listener.log = ^(NSString *line) { DBG("%s", line.UTF8String); };
    }
    if (!on) {
        g_auto_speak = g_summarize_claude = 0;
        [self cancelPtt];
        if (self.speaker.speakingSid) [self.speaker stop:self.speaker.speakingSid];
    }
    [self setPttHotkey:on];
}

// Esc is only ours while recording, so it never goes missing from the terminal otherwise
- (void)setEscapeHotkey:(BOOL)on {
    if (on && !g_esc_key) RegisterEventHotKey(kVK_Escape, 0, (EventHotKeyID){'dodc', HK_ESC}, GetApplicationEventTarget(), 0, &g_esc_key);
    if (!on && g_esc_key) { UnregisterEventHotKey(g_esc_key); g_esc_key = NULL; }
}

- (void)applicationWillTerminate:(NSNotification *)note { (void)note; pointer_visible(YES); cleanup(); }
@end

static void usage(void) {
    puts("vigil-panel: the vigil polyhedra in a floating panel on the right edge of the screen\n"
         "\n"
         "  --demo [N]         made-up sessions instead of the events file (1-6, default 3)\n"
         "  --exit-when-empty  quit once no session has been running for 15 seconds\n"
         "  --shape NAME       dodeca | bucky | geo2 (default) | geo3\n"
         "  --fit-pane         take the size of the terminal pane it was started in\n"
         "  --width POINTS     panel width (default: 17% of the screen)\n"
         "  --top POINTS       space left free at the top of the screen (default 40)\n"
         "  --inset POINTS     empty space above the first session, inside the panel (default 30)\n"
         "  --font NAME        text font (default Monaco)        --font-size N   (default 12)\n"
         "  --bg RRGGBB        solid background colour instead of see-through\n"
         "  --all-spaces       follow you to every Space instead of staying in the one it was opened in\n"
         "  --always           with --all-spaces: stay visible when iTerm2 is not the app in front\n"
         "  --glow X  --core X  --events PATH   as in vigil\n"
         "  --hotkey KEYS      push-to-talk to the session that needs you most (default ctrl+opt+v; none = off)\n"
         "  --away SECONDS     no input this long = away: cores go to standby (default 120; 0 = never)\n"
         "  --voice            spoken reports (click a core) and push-to-talk (hold a core, or the hotkey)\n"
         "  --voice-id ID      voice: system (default: the System Voice of Spoken Content, Siri works), a language or an identifier\n"
         "  --rate X           speaking rate 0..1 (default 0.5)\n"
         "  --auto-speak       read a new reply out on its own if you were active in the last 60 s\n"
         "  --summarize claude summarize replies with claude -p (15 s timeout) instead of the first sentences\n"
         "  --listen-locale L  the language push-to-talk understands (default zh-TW; on-device only)\n"
         "  --mic NAME         input device to listen with (part of its name; default: the system input, skipping\n"
         "                     virtual ones like BlackHole)\n"
         "  --single           single-core mode: one big core, the other sessions small around it (aura = who)\n"
         "  --mode-hotkey KEYS switch between single core and all cores (default ctrl+opt+c; none = off)\n"
         "  --labels hover     no name / state lines: only the cores, the tooltip shows who is who (default: always)\n"
         "  --frame NAME       focus frame round a carried or locked-on core: segments (default) | lens | polar |\n"
         "                     diamond | lock | orbits | none (also right-click > Settings > Focus frame)\n"
         "  --settings         open the Settings window at start (also right-click > Settings...)\n"
         "  --settings-shot FILE  open the Settings window, save it as a PNG after 2 s and quit\n"
         "  --reply-shot FILE  open a reply box, save it with its tooltip as a PNG and quit (for checking its look)\n"
         "  --debug            log each voice step to stderr (states and lengths, never what was said)\n"
         "  --confirm-send     a recognized reply waits for a second click instead of going out after 1.5 s\n"
         "  --tty PATH --launcher-pid N --claude PATH   (set by the ./vigil-panel launcher)");
}

int main(int argc, char **argv) {
    @autoreleasepool {
        const char *shape = "geo2", *events = getenv("DODECA_EVENTS"), *home = getenv("HOME") ? getenv("HOME") : ".";
        char events_default[1024];
        for (int i = 1; i < argc; i++) {
            const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
            if (!strcmp(a, "--demo")) { g_demo_n = 3; if (v && v[0] >= '1' && v[0] <= '6' && !v[1]) { g_demo_n = v[0] - '0'; i++; } }
            else if (!strcmp(a, "--exit-when-empty")) g_exit_empty = 1;
            else if (!strcmp(a, "--always")) g_always = 1;
            else if (!strcmp(a, "--fit-pane")) g_fit_pane = 1;
            else if (!strcmp(a, "--all-spaces")) g_all_spaces = 1;
            else if (!strcmp(a, "--shape") && v) { shape = v; i++; }
            else if (!strcmp(a, "--width") && v) { g_width = atof(v); i++; }
            else if (!strcmp(a, "--top") && v) { g_top = atof(v); i++; }
            else if (!strcmp(a, "--inset") && v) { g_inset = atof(v); i++; }
            else if (!strcmp(a, "--font") && v) { g_font_name = [NSString stringWithUTF8String:v] ?: @"Monaco"; i++; }
            else if (!strcmp(a, "--font-size") && v) { g_font_size = atof(v); i++; }
            else if (!strcmp(a, "--glow") && v) { g_glow = (float)atof(v); i++; }
            else if (!strcmp(a, "--core") && v) { g_core = (float)atof(v); i++; }
            else if (!strcmp(a, "--events") && v) { events = v; i++; }
            else if (!strcmp(a, "--tty") && v) { g_tty_path = v; i++; }
            else if (!strcmp(a, "--hotkey") && v) { g_hotkey = v; i++; }
            else if (!strcmp(a, "--voice-id") && v) { g_voice_id = @(v); i++; }
            else if (!strcmp(a, "--rate") && v) { g_rate = (float)atof(v); i++; }
            else if (!strcmp(a, "--voice")) g_voice = 1;
            else if (!strcmp(a, "--listen-locale") && v) { g_listen_locale = @(v); i++; }
            else if (!strcmp(a, "--confirm-send")) g_confirm_send = 1;
            else if (!strcmp(a, "--debug")) g_debug = 1;
            else if (!strcmp(a, "--reply-shot") && v) { g_reply_shot = v; i++; }
            else if (!strcmp(a, "--settings-shot") && v) { g_settings_shot = v; i++; }
            else if (!strcmp(a, "--settings")) g_settings_open = 1;
            else if (!strcmp(a, "--labels") && v) { g_labels_hover = !strcmp(v, "hover"); i++; }
            else if (!strcmp(a, "--single")) g_single = 1;
            else if (!strcmp(a, "--mode-hotkey") && v) { g_mode_hotkey = v; i++; }
            else if (!strcmp(a, "--mic") && v) { g_mic = @(v); i++; }
            else if (!strcmp(a, "--auto-speak")) g_auto_speak = g_voice = 1;
            else if (!strcmp(a, "--summarize") && v) { g_summarize_claude = !strcmp(v, "claude"); g_voice |= g_summarize_claude; i++; }
            else if (!strcmp(a, "--claude") && v) { g_claude = @(v); i++; }
            else if (!strcmp(a, "--away") && v) { g_away_after = atof(v); i++; }
            else if (!strcmp(a, "--frame") && v) { snprintf(g_frame, sizeof g_frame, "%s", v); i++; }
            else if (!strcmp(a, "--launcher-pid") && v) { g_launcher_pid = atoi(v); i++; }
            else if (!strcmp(a, "--bg") && v) {
                unsigned int rgb = 0;
                if (sscanf(v[0] == '#' ? v + 1 : v, "%6x", &rgb) == 1)
                    g_bg = [NSColor colorWithSRGBRed:(rgb >> 16 & 255) / 255.0 green:(rgb >> 8 & 255) / 255.0 blue:(rgb & 255) / 255.0 alpha:1];
                i++;
            }
            else { usage(); return strcmp(a, "--help") && strcmp(a, "-h") ? 2 : 0; }
        }
        snprintf(g_shape_name, sizeof g_shape_name, "%s", shape);
        remember_launch();
        load_settings();
        if (g_font_size < 6) g_font_size = 6;
        if (g_summarize_claude && !g_claude) { // started without the launcher: look where it is usually installed
            for (NSString *c in @[@"~/.local/bin/claude", @"/opt/homebrew/bin/claude", @"/usr/local/bin/claude"])
                if ([[NSFileManager defaultManager] isExecutableFileAtPath:c.stringByExpandingTildeInPath]) { g_claude = c.stringByExpandingTildeInPath; break; }
        }
        if (g_summarize_claude && !g_claude) { fprintf(stderr, "vigil-panel: claude not found, using the local summary\n"); g_summarize_claude = 0; }
        if (!shape_make(&g_shape, g_shape_name)) { fprintf(stderr, "unknown shape: %s\n", g_shape_name); return 2; }
        if (!events) {
            snprintf(events_default, sizeof events_default, "%s/.dodeca-core/events.jsonl", home);
            events = events_default;
        }

        double now = wall();
        if (g_demo_n) demo_start(&g_demo, g_demo_n, now);
        else {
            tail_init(&g_tail, events);
            snprintf(g_msg_dir, sizeof g_msg_dir, "%.1000s", events); // the msg folder sits next to the events file
            char *slash = strrchr(g_msg_dir, '/');
            if (slash) strcpy(slash + 1, "msg"); else snprintf(g_msg_dir, sizeof g_msg_dir, "msg");
            int n = tail_history(&g_tail, now - 1800, g_ev, 1024); // rebuild what the sessions were doing
            for (int i = 0; i < n; i++) room_apply(&g_room, &g_ev[i]);
            char dir[900]; // lets the launcher see that a panel is already open
            snprintf(dir, sizeof dir, "%.800s/.dodeca-core", home);
            mkdir(dir, 0700);
            snprintf(g_pidfile, sizeof g_pidfile, "%s/pane.pid", dir);
            FILE *f = fopen(g_pidfile, "w");
            if (f) { fprintf(f, "%d\n", (int)getpid()); fclose(f); } else g_pidfile[0] = 0;
        }
        session_init(&g_ghost.s[0], "ghost", now, &g_shape); // shown while no session is running
        snprintf(g_ghost.s[0].name, sizeof g_ghost.s[0].name, "No session");
        g_ghost.s[0].state = ST_SLEEP;
        g_ghost.n = 1;

        // in a terminal pane: keep the pane blank, it is only there to hold the space
        int fd = -1;
        if (g_tty_path) fd = open(g_tty_path, O_RDWR | O_NOCTTY);
        else if (isatty(STDOUT_FILENO) && isatty(STDIN_FILENO)) fd = STDOUT_FILENO;
        if (fd >= 0 && isatty(fd)) {
            struct termios t;
            tcgetattr(fd, &g_old_tty);
            t = g_old_tty;
            t.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
            tcsetattr(fd, TCSAFLUSH, &t);
            const char *blank = "\x1b[?25l\x1b[2J\x1b[H";
            if (write(fd, blank, strlen(blank)) < 0) { }
            g_tty = fd;
        }
        atexit(cleanup);

        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyAccessory]; // no Dock icon, never becomes the active app
        AppDelegate *delegate = [[AppDelegate alloc] init];
        app.delegate = delegate;
        [app run];
    }
    return 0;
}

// dodeca-panel: the same polyhedra as dodeca-core, in a floating panel on the right edge of the screen.
//
// The panel has no title bar, never takes the keyboard, lets clicks through (except on a core, see
// trackMouse:), and is see-through where the picture is dark, so it can sit over a full-screen iTerm2
// window: an empty terminal pane underneath keeps that part of the screen free. When started inside that pane (open_pane.sh does this) it sizes itself to
// the pane and quits when the pane is closed.
//
// Build: sh mac/build.sh. That makes DodecaPanel.app and the launcher script ./dodeca-panel, which starts the
// app through LaunchServices (so macOS asks for microphone and speech permission on behalf of the app, not
// the terminal) and stays in the pane: it passes the pane's terminal with --tty and its own pid with
// --launcher-pid, and the app quits when the launcher does (the pane was closed, or Ctrl-C).
#import <Cocoa/Cocoa.h>
#import <Carbon/Carbon.h> // RegisterEventHotKey: global hotkeys without the Accessibility permission
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>
#include "../src/dodeca.h"
#import "speech.h"
#import "listen.h"

static Room g_room, g_ghost;
static Shape g_shape;
static Tail g_tail;
static Event g_ev[1024];
static Demo g_demo;
static int g_demo_n, g_exit_empty, g_always, g_fit_pane, g_all_spaces;
static float g_glow = 1.0f, g_core = 0.35f;
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
static const double PREVIEW_FOR = 1.5;          // seconds a recognized reply is shown before it is sent

static const Room *shown_room(void) { return g_room.n ? &g_room : &g_ghost; }

static Session *session_by_sid(NSString *sid) {
    Room *r = g_room.n ? &g_room : &g_ghost;
    for (int i = 0; sid && i < r->n; i++) if ([sid isEqualToString:@(r->s[i].sid)]) return &r->s[i];
    return NULL;
}

static NSString *sid_of(const Session *s) { return [NSString stringWithUTF8String:s->sid] ?: @""; }

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
@end

static const double PRESS_AFTER = 0.35; // seconds: a press held longer is push-to-talk, not a click
static const CGFloat DRAG_AFTER = 4;    // points the mouse moves before a press becomes a drag
static const float DRAG_TURN = 0.012f;  // radians per point dragged

@interface PanelView : NSView
@property(weak) id<CoreActions> actions;
@property(readonly) BOOL busy; // in the middle of a press or drag: keep taking mouse events
- (BOOL)anythingDue:(double)now;
- (int)coreAt:(NSPoint)p look:(NSPoint *)look center:(NSPoint *)center;
- (void)cancelPress;
- (NSRect)coreRect:(int)i;
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
    BOOL _dragging, _pressing;
    NSTimer *_pressTimer, *_clickTimer;
}

- (BOOL)isOpaque { return NO; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { (void)e; return YES; } // a click on a core never has to activate the app first
- (BOOL)busy { return _downSid != nil; }
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

- (void)mouseDragged:(NSEvent *)e {
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
    NSString *sid = [self sidAt:e];
    NSMenu *menu = sid ? [self.actions menuForCore:sid] : nil;
    if (menu) [NSMenu popUpContextMenu:menu withEvent:e forView:self];
}

// the wheel sets this core's speech volume; the shell shows it for a moment
- (void)scrollWheel:(NSEvent *)e {
    Session *s = session_by_sid([self sidAt:e]);
    if (!s) return;
    float step = (float)e.scrollingDeltaY * (e.hasPreciseScrollingDeltas ? 0.004f : 0.05f);
    s->vol = fmaxf(0, fminf(1, s->vol + step));
    s->vol_at = wall();
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
    for (int i = 0; i < r->n && i <= MAX_SESS; i++) if (!_img[i] || now >= _due[i]) return YES;
    return NO;
}

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
    NSFont *font = [NSFont fontWithName:g_font_name size:g_font_size];
    if (!font) font = [NSFont monospacedSystemFontOfSize:g_font_size weight:NSFontWeightRegular];
    CGFloat lineH = ceil(font.ascender - font.descender) + 3, textH = lineH * 2 + 4, gap = 10, left = 10;

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

    for (int i = 0; i < n; i++) {
        const Session *s = &room->s[i];
        CGFloat top = H - i * (textH + side + gap); // this view's y runs upward, so the list starts at the top edge
        int st = session_shown(s, now);

        // a coloured dot and the folder name, then the state in grey
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
        CGContextDrawImage(ctx, NSRectToCGRect(_core[i]), _img[i]);
    }
    _coreN = n;
}
@end

// ---- the hover tooltip: name and state, then the first two lines of the last reply ----------------------
@interface TipView : NSView
@property(copy) NSString *title, *body;
@property(strong) NSColor *accent;
- (NSSize)fit;
@end

@implementation TipView
- (BOOL)isOpaque { return NO; }
- (NSFont *)font {
    return [NSFont fontWithName:g_font_name size:g_font_size] ?: [NSFont monospacedSystemFontOfSize:g_font_size weight:NSFontWeightRegular];
}
- (NSDictionary *)attr:(NSColor *)c {
    NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
    para.lineBreakMode = NSLineBreakByTruncatingTail;
    return @{NSFontAttributeName: [self font], NSForegroundColorAttributeName: c, NSParagraphStyleAttributeName: para};
}
- (NSArray *)lines {
    NSMutableArray *a = [NSMutableArray arrayWithObject:self.title ?: @""];
    for (NSString *l in [self.body componentsSeparatedByCharactersInSet:[NSCharacterSet newlineCharacterSet]]) {
        NSString *t = [l stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        if (t.length && a.count < 3) [a addObject:t];
    }
    return a;
}
static const CGFloat TIP_PAD = 9, TIP_MAX = 340;
- (NSSize)fit {
    CGFloat w = 0, lineH = ceil([self font].ascender - [self font].descender) + 3;
    NSArray *ls = [self lines];
    for (NSString *l in ls) w = fmax(w, ceil([l sizeWithAttributes:[self attr:NSColor.whiteColor]].width));
    return NSMakeSize(fmin(w, TIP_MAX) + TIP_PAD * 2, lineH * ls.count + TIP_PAD * 2 - 3);
}
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    NSRect b = NSInsetRect(self.bounds, 0.5, 0.5);
    NSBezierPath *box = [NSBezierPath bezierPathWithRoundedRect:b xRadius:7 yRadius:7];
    [[NSColor colorWithSRGBRed:0.07 green:0.08 blue:0.10 alpha:0.94] setFill];
    [box fill];
    [[self.accent colorWithAlphaComponent:0.7] setStroke];
    [box stroke];
    CGFloat lineH = ceil([self font].ascender - [self font].descender) + 3, y = self.bounds.size.height - TIP_PAD - lineH + 3;
    NSArray *ls = [self lines];
    for (NSUInteger i = 0; i < ls.count; i++, y -= lineH) {
        NSColor *c = i ? [NSColor colorWithSRGBRed:0.80 green:0.82 blue:0.86 alpha:1] : [NSColor colorWithSRGBRed:0.97 green:0.97 blue:0.99 alpha:1];
        [ls[i] drawInRect:NSMakeRect(TIP_PAD, y, self.bounds.size.width - TIP_PAD * 2, lineH) withAttributes:[self attr:c]];
    }
}
@end

// ---- the application ------------------------------------------------------------------------------------
@interface AppDelegate : NSObject <NSApplicationDelegate, CoreActions>
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
@property(strong) TipView *previewView;
@property unsigned short paneCols, paneRows;
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
    if (w > f.size.width * 0.6) w = f.size.width * 0.6;
    if (h > f.size.height) h = f.size.height;
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
    self.overlay = [NSMutableDictionary dictionary];
    self.seen = [NSMutableDictionary dictionary];
    self.summarizing = [NSMutableSet set];
    self.errorUntil = [NSMutableDictionary dictionary];
    if (g_voice) {
        self.speaker = [[Speaker alloc] initWithVoice:g_voice_id rate:g_rate];
        self.listener = [[Listener alloc] initWithLocale:g_listen_locale];
        [self registerHotkeys];
    }
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
    [self onSignal:SIGINT handler:^{ [NSApp terminate:nil]; }];
    [[[NSWorkspace sharedWorkspace] notificationCenter] addObserver:self selector:@selector(appChanged:)
                                                               name:NSWorkspaceDidActivateApplicationNotification object:nil];
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(screenChanged:)
                                                 name:NSApplicationDidChangeScreenParametersNotification object:nil];

    double now = wall();
    self.last = self.emptySince = now;
    self.timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 30 target:self selector:@selector(tick:) userInfo:nil repeats:YES];
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
    }
    BOOL ignore = hit < 0 && !self.view.busy; // a drag or press keeps going when the mouse leaves the core
    if (self.panel.ignoresMouseEvents != ignore) self.panel.ignoresMouseEvents = ignore;

    NSString *sid = hit >= 0 ? [NSString stringWithUTF8String:room->s[hit].sid] : nil;
    if (![sid isEqualToString:self.hoverSid]) { self.hoverSid = sid; self.hoverSince = now; }
    if (hit < 0 || now - self.hoverSince < 0.3) { [self.tip orderOut:nil]; return; } // a short pause before the tooltip
    [self showTipFor:&room->s[hit] at:[self.view convertPoint:center toView:nil] now:now];
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
    t.contentView = *view;
    return t;
}

- (void)showTipFor:(const Session *)s at:(NSPoint)center now:(double)now {
    if (self.preview.visible) { [self.tip orderOut:nil]; return; } // the preview has the floor
    if (!self.tip) {
        TipView *v;
        self.tip = [self newTipPanel:&v];
        self.tipView = v;
    }
    NSString *name = [NSString stringWithUTF8String:s->name[0] ? s->name : "claude"] ?: @"claude";
    NSString *title = [NSString stringWithFormat:@"%@ · %@", name, state_line(s, session_shown(s, now))];
    if (s->unread) title = [title stringByAppendingString:@" · new reply"];
    int st = session_shown(s, now);
    NSString *body = (st == ST_WAIT ? msg_text(s, @"notice") : nil) ?: msg_text(s, @"reply") ?: @"(no reply yet)";
    Col c = s->cur.eh;
    self.tipView.accent = [NSColor colorWithSRGBRed:c.r green:c.g blue:c.b alpha:1];
    if (![title isEqualToString:self.tipView.title] || ![body isEqualToString:self.tipView.body] || !self.tip.visible) {
        self.tipView.title = title;
        self.tipView.body = body;
        // to the left of the panel, level with the core, kept on the screen
        NSSize sz = [self.tipView fit];
        NSPoint at = [self.panel convertPointToScreen:center];
        NSRect f = NSMakeRect(NSMinX(self.panel.frame) - sz.width - 8, at.y - sz.height / 2, sz.width, sz.height);
        NSRect vis = (self.panel.screen ?: [NSScreen mainScreen]).visibleFrame;
        if (NSMinX(f) < NSMinX(vis)) f.origin.x = NSMinX(vis);
        if (NSMaxY(f) > NSMaxY(vis)) f.origin.y = NSMaxY(vis) - sz.height;
        if (NSMinY(f) < NSMinY(vis)) f.origin.y = NSMinY(vis);
        [self.tip setFrame:f display:YES];
    }
    [self.tipView setNeedsDisplay:YES];
    if (!self.tip.visible) [self.tip orderFrontRegardless];
}

// ---- what clicks, presses and keys do ------------------------------------------------------------------

// single click: play the spoken report, or stop it; the reply counts as read
- (void)coreClicked:(NSString *)sid {
    Session *s = session_by_sid(sid);
    if (!s) return;
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

// double click: a shockwave ring, and the iTerm2 pane of that session comes to the front
- (void)coreDoubleClicked:(NSString *)sid {
    Session *s = session_by_sid(sid);
    if (!s) return;
    if ([sid isEqualToString:self.pendingSid]) { [self dropPending]; return; } // counts as a click: cancel
    s->ring_at = wall();
    s->unread = 0;
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
    if (why) { [self fail:sid because:why]; return; }
    [self.speaker stop:self.speaker.speakingSid]; // don't talk over you
    self.pttSid = sid;
    self.overlay[sid] = @(ST_LISTEN);
    [self setEscapeHotkey:YES];
    __weak AppDelegate *me = self;
    BOOL ok = [self.listener start:^(NSString *partial) {
        [me showPreview:partial forSid:sid hint:@"…"];
    }];
    if (!ok) { [self cancelPtt]; [self fail:sid because:@"麥克風沒辦法開始錄音"]; return; }
    [self showPreview:@"" forSid:sid hint:@"說吧，放開就結束（Esc 取消）"];
}

- (void)corePressEnded:(NSString *)sid {
    if (![sid isEqualToString:self.pttSid]) return;
    [self setEscapeHotkey:NO];
    self.overlay[sid] = @(ST_TRANSCRIBE);
    __weak AppDelegate *me = self;
    [self.listener finish:^(NSString *text) {
        AppDelegate *d = me;
        if (!d || ![d.pttSid isEqualToString:sid]) return; // cancelled meanwhile
        [d.overlay removeObjectForKey:sid];
        d.pttSid = nil;
        if (!text) { [d.preview orderOut:nil]; return; }
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
        self.previewView = v;
    }
    NSString *name = @(s->name[0] ? s->name : "claude") ?: @"claude";
    self.previewView.title = [NSString stringWithFormat:@"→ %@ · %@", name, hint];
    self.previewView.body = text.length ? text : @" ";
    Col c = s->cur.eh;
    self.previewView.accent = [NSColor colorWithSRGBRed:c.r green:c.g blue:c.b alpha:1];
    NSRect core = [self.view coreRect:(int)(s - room->s)];
    NSRect inScreen = [self.panel convertRectToScreen:[self.view convertRect:core toView:nil]];
    NSSize sz = [self.previewView fit];
    NSRect pf = self.panel.frame;
    if (sz.width > pf.size.width - 8) sz.width = pf.size.width - 8;
    NSRect f = NSMakeRect(NSMidX(inScreen) - sz.width / 2, NSMinY(inScreen) + inScreen.size.height * 0.08 - sz.height, sz.width, sz.height);
    if (NSMaxX(f) > NSMaxX(pf) - 4) f.origin.x = NSMaxX(pf) - 4 - sz.width;
    if (NSMinX(f) < NSMinX(pf) + 4) f.origin.x = NSMinX(pf) + 4;
    [self.preview setFrame:f display:YES];
    [self.previewView setNeedsDisplay:YES];
    [self.preview orderFrontRegardless];
    [self.tip orderOut:nil];
}

// Sends the recognized reply, unless Claude is busy: then it asks first, since a reply typed while it works
// waits until the current step is done, and interrupting means pressing Esc in that session.
- (void)sendPending {
    NSString *sid = self.pendingSid, *text = self.pendingText;
    [self dropPending];
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
    NSString *copy = [scripts stringByAppendingPathComponent:@"dodeca_pane.py"];
    if (![fm isExecutableFileAtPath:it2run] || ![fm fileExistsAtPath:scripts] || ![fm fileExistsAtPath:mine]) {
        [self fail:sid because:@"沒辦法控制 iTerm2：請允許面板控制 iTerm2，或打開 iTerm2 的 Python API"];
        return;
    }
    if (![fm contentsEqualAtPath:mine andPath:copy]) { [fm removeItemAtPath:copy error:NULL]; [fm copyItemAtPath:mine toPath:copy error:NULL]; }
    NSString *result = [NSTemporaryDirectory() stringByAppendingPathComponent:[NSString stringWithFormat:@"dodeca-send-%d-%u", (int)getpid(), arc4random()]];
    NSTask *task = [[NSTask alloc] init];
    task.executableURL = [NSURL fileURLWithPath:it2run];
    task.arguments = @[@"dodeca_pane.py", @"send", iterm, text, result, interrupt ? @"1" : @"0"];
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
    if (!s || !strcmp(s->sid, "ghost")) return nil;
    NSMenu *m = [[NSMenu alloc] initWithTitle:@""];
    m.autoenablesItems = NO;
    NSString *reply = msg_text(s, @"reply");
    NSMenuItem *replay = [m addItemWithTitle:@"Replay" action:@selector(menuReplay:) keyEquivalent:@""];
    replay.enabled = g_voice && (reply != nil || msg_text(s, @"notice") != nil);
    NSMenuItem *copy = [m addItemWithTitle:@"Copy reply" action:@selector(menuCopy:) keyEquivalent:@""];
    copy.enabled = reply != nil;
    NSMenuItem *mute = [m addItemWithTitle:@"Mute this session" action:@selector(menuMute:) keyEquivalent:@""];
    mute.state = s->muted ? NSControlStateValueOn : NSControlStateValueOff;
    [m addItem:[NSMenuItem separatorItem]];
    [m addItemWithTitle:@"Bring to front" action:@selector(menuFront:) keyEquivalent:@""];
    for (NSMenuItem *i in m.itemArray) { i.target = self; i.representedObject = sid; }
    return m;
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
- (void)menuFront:(NSMenuItem *)i { [self coreDoubleClicked:i.representedObject]; }

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
enum { HK_PTT = 1, HK_ESC = 2 };
static EventHotKeyRef g_ptt_key, g_esc_key;

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
    if (down) {
        Session *s = [self mostWanted];
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
    if (!strcmp(g_hotkey, "none")) return;
    UInt32 mods, key;
    if (!parse_hotkey(g_hotkey, &mods, &key)) { fprintf(stderr, "dodeca-panel: cannot read --hotkey %s\n", g_hotkey); return; }
    if (RegisterEventHotKey(key, mods, (EventHotKeyID){'dodc', HK_PTT}, GetApplicationEventTarget(), 0, &g_ptt_key) != noErr)
        fprintf(stderr, "dodeca-panel: hotkey %s is taken by another app\n", g_hotkey);
}

// Esc is only ours while recording, so it never goes missing from the terminal otherwise
- (void)setEscapeHotkey:(BOOL)on {
    if (on && !g_esc_key) RegisterEventHotKey(kVK_Escape, 0, (EventHotKeyID){'dodc', HK_ESC}, GetApplicationEventTarget(), 0, &g_esc_key);
    if (!on && g_esc_key) { UnregisterEventHotKey(g_esc_key); g_esc_key = NULL; }
}

- (void)applicationWillTerminate:(NSNotification *)note { (void)note; cleanup(); }
@end

static void usage(void) {
    puts("dodeca-panel: the dodeca-core polyhedra in a floating panel on the right edge of the screen\n"
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
         "  --glow X  --core X  --events PATH   as in dodeca-core\n"
         "  --hotkey KEYS      push-to-talk to the session that needs you most (default ctrl+opt+v; none = off)\n"
         "  --away SECONDS     no input this long = away: cores go to standby (default 120; 0 = never)\n"
         "  --voice            spoken reports (click a core) and push-to-talk (hold a core, or the hotkey)\n"
         "  --voice-id ID      voice: system (default: the System Voice of Spoken Content, Siri works), a language or an identifier\n"
         "  --rate X           speaking rate 0..1 (default 0.5)\n"
         "  --auto-speak       read a new reply out on its own if you were active in the last 60 s\n"
         "  --summarize claude summarize replies with claude -p (15 s timeout) instead of the first sentences\n"
         "  --listen-locale L  the language push-to-talk understands (default zh-TW; on-device only)\n"
         "  --confirm-send     a recognized reply waits for a second click instead of going out after 1.5 s\n"
         "  --tty PATH --launcher-pid N --claude PATH   (set by the ./dodeca-panel launcher)");
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
            else if (!strcmp(a, "--auto-speak")) g_auto_speak = g_voice = 1;
            else if (!strcmp(a, "--summarize") && v) { g_summarize_claude = !strcmp(v, "claude"); g_voice |= g_summarize_claude; i++; }
            else if (!strcmp(a, "--claude") && v) { g_claude = @(v); i++; }
            else if (!strcmp(a, "--away") && v) { g_away_after = atof(v); i++; }
            else if (!strcmp(a, "--launcher-pid") && v) { g_launcher_pid = atoi(v); i++; }
            else if (!strcmp(a, "--bg") && v) {
                unsigned int rgb = 0;
                if (sscanf(v[0] == '#' ? v + 1 : v, "%6x", &rgb) == 1)
                    g_bg = [NSColor colorWithSRGBRed:(rgb >> 16 & 255) / 255.0 green:(rgb >> 8 & 255) / 255.0 blue:(rgb & 255) / 255.0 alpha:1];
                i++;
            }
            else { usage(); return strcmp(a, "--help") && strcmp(a, "-h") ? 2 : 0; }
        }
        if (g_font_size < 6) g_font_size = 6;
        if (g_summarize_claude && !g_claude) { // started without the launcher: look where it is usually installed
            for (NSString *c in @[@"~/.local/bin/claude", @"/opt/homebrew/bin/claude", @"/usr/local/bin/claude"])
                if ([[NSFileManager defaultManager] isExecutableFileAtPath:c.stringByExpandingTildeInPath]) { g_claude = c.stringByExpandingTildeInPath; break; }
        }
        if (g_summarize_claude && !g_claude) { fprintf(stderr, "dodeca-panel: claude not found, using the local summary\n"); g_summarize_claude = 0; }
        if (!shape_make(&g_shape, shape)) { fprintf(stderr, "unknown shape: %s\n", shape); return 2; }
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

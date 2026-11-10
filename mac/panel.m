// dodeca-panel: the same polyhedra as dodeca-core, in a floating panel on the right edge of the screen.
//
// The panel has no title bar, never takes the keyboard, lets clicks through, and is see-through where the
// picture is dark, so it can sit over a full-screen iTerm2 window: an empty terminal pane underneath keeps
// that part of the screen free. When started inside that pane (open_pane.sh does this) it sizes itself to
// the pane and quits when the pane is closed.
//
// Build: sh mac/build.sh
#import <Cocoa/Cocoa.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>
#include "../src/dodeca.h"

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
static char g_pidfile[1024];
static struct termios g_old_tty;
static int g_tty;

static double wall(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + tv.tv_usec / 1e6;
}

static void cleanup(void) {
    if (g_pidfile[0]) { unlink(g_pidfile); g_pidfile[0] = 0; }
    if (g_tty) {
        const char *on = "\x1b[?25h";
        if (write(STDOUT_FILENO, on, strlen(on)) < 0) { }
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_old_tty);
        g_tty = 0;
    }
}

// ---- the view: for each session two lines of text, then its polyhedron ----------------------------------
@interface PanelView : NSView
- (BOOL)anythingDue:(double)now;
@end

@implementation PanelView {
    CGImageRef _img[MAX_SESS + 1];
    double _due[MAX_SESS + 1];
    Canvas _cv;
    int _lastCount;
}

- (BOOL)isOpaque { return NO; }

- (const Room *)shown { return g_room.n ? &g_room : &g_ghost; }

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
        NSString *state;
        if (!strcmp(s->sid, "ghost")) state = @"Waiting for Claude Code";
        else if (st == ST_WORK && s->tool[0]) state = [NSString stringWithFormat:@"%s · %s", STATE_TEXT[st], s->tool];
        else state = [NSString stringWithUTF8String:STATE_TEXT[st]];
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
        CGContextDrawImage(ctx, CGRectMake((W - side) / 2, top - textH - side, side, side), _img[i]);
    }
}
@end

// ---- the application ------------------------------------------------------------------------------------
@interface AppDelegate : NSObject <NSApplicationDelegate>
@property(strong) NSPanel *panel;
@property(strong) PanelView *view;
@property(strong) NSTimer *timer;
@property(strong) NSMutableArray *signals;
@property double last, emptySince;
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
    if (g_fit_pane && g_tty && ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col && ws.ws_xpixel && ws.ws_ypixel) {
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
    p.contentView = self.view;
    self.panel = p;
    [self place];
    [self updateVisibility];

    self.signals = [NSMutableArray array];
    __weak AppDelegate *me = self;
    [self onSignal:SIGWINCH handler:^{ [me place]; }];
    [self onSignal:SIGHUP handler:^{ [NSApp terminate:nil]; }]; // the pane was closed
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
    room_tick(&g_room, &g_shape, now, dt);
    g_ghost.s[0].last = now;
    room_tick(&g_ghost, &g_shape, now, dt);
    if (g_room.n) self.emptySince = now;
    if (g_exit_empty && now - self.emptySince > 15) { [NSApp terminate:nil]; return; }
    if (self.panel.visible && [self.view anythingDue:now]) [self.view setNeedsDisplay:YES];
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
         "  --glow X  --core X  --events PATH   as in dodeca-core");
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
            else if (!strcmp(a, "--bg") && v) {
                unsigned int rgb = 0;
                if (sscanf(v[0] == '#' ? v + 1 : v, "%6x", &rgb) == 1)
                    g_bg = [NSColor colorWithSRGBRed:(rgb >> 16 & 255) / 255.0 green:(rgb >> 8 & 255) / 255.0 blue:(rgb & 255) / 255.0 alpha:1];
                i++;
            }
            else { usage(); return strcmp(a, "--help") && strcmp(a, "-h") ? 2 : 0; }
        }
        if (g_font_size < 6) g_font_size = 6;
        if (!shape_make(&g_shape, shape)) { fprintf(stderr, "unknown shape: %s\n", shape); return 2; }
        if (!events) {
            snprintf(events_default, sizeof events_default, "%s/.dodeca-core/events.jsonl", home);
            events = events_default;
        }

        double now = wall();
        if (g_demo_n) demo_start(&g_demo, g_demo_n, now);
        else {
            tail_init(&g_tail, events);
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
        if (isatty(STDOUT_FILENO) && isatty(STDIN_FILENO)) {
            struct termios t;
            tcgetattr(STDIN_FILENO, &g_old_tty);
            t = g_old_tty;
            t.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);
            const char *blank = "\x1b[?25l\x1b[2J\x1b[H";
            if (write(STDOUT_FILENO, blank, strlen(blank)) < 0) { }
            g_tty = 1;
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

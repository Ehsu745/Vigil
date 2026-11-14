// The Settings window, laid out like the Windows panel's (win/panel.c, set_build): a dark card with the title and
// a close button, a preview band (the cluster as it will look, the states to show it in, a reply in the text box's
// look), three cards (Look, Motion, Replies), a Voice strip (macOS only) and a foot (rest time, reset). Everything
// is drawn by one flipped view in Windows' logical units (880 wide); what can be clicked is kept as hit boxes.
// The preview is a view of its own with four made-up sessions, ticked while the window is open.
#import "settings.h"
#include "orbit.h"

const unsigned TIP_BGS[5] = {0x18191E, 0x262626, 0x16233F, 0x2B1C3B, 0x15302A};
const unsigned TEXT_COLS[5] = {0, 0xF2F2F5, 0xF5D9A0, 0x9FE8C2, 0x9CCBFF};
NSColor *hex_color(unsigned v) { return [NSColor colorWithSRGBRed:(v >> 16 & 255) / 255.0 green:(v >> 8 & 255) / 255.0 blue:(v & 255) / 255.0 alpha:1]; }
unsigned color_hex(NSColor *c) {
    NSColor *s = [c colorUsingColorSpace:[NSColorSpace sRGBColorSpace]];
    if (!s) return 0;
    return (unsigned)lround(s.redComponent * 255) << 16 | (unsigned)lround(s.greenComponent * 255) << 8 | (unsigned)lround(s.blueComponent * 255);
}
BOOL hex_light(unsigned v) { return 0.299 * (v >> 16 & 255) + 0.587 * (v >> 8 & 255) + 0.114 * (v & 255) > 140; }

// the palette, as on Windows
static const unsigned S_BG = 0x18181B, S_BAND = 0x0F0F12, S_CARD = 0x212126, S_TILE = 0x19191D, S_EDGE = 0x34343B,
                      S_EDGE_HOT = 0x55555E, S_TEXT = 0xECECEF, S_LABEL = 0xA1A1AA, S_MUTED = 0x8B8B94, S_CHIP = 0x27272D,
                      S_CHIP_HOT = 0x323239, S_CHIP_TXT = 0xB4B4BC, S_OR = 0xFF8A1F, S_ON_OR = 0x1F1206, S_KNOB = 0xFFF1E3, S_CLOSE_HOT = 0x5A2A22;
static const CGFloat WW = 880, WH = 900;

// what each row can be set to; a row's value is matched against these to find the chosen one
static NSDictionary<NSString *, NSArray *> *choices(void) {
    static NSDictionary *d;
    if (!d) d = @{
        @"shape": @[@"dodeca", @"bucky", @"geo2", @"geo3"],
        @"glow": @[@0.7, @1.0, @1.4],
        @"scale": @[@0.75, @1.0, @1.3, @1.6],
        @"core": @[@0.25, @0.35, @0.45, @0.55],
        @"gravity": @[@1, @2, @4, @8],
        @"frame": @[@"segments", @"lens", @"polar", @"diamond", @"lock", @"orbits", @"none"],
        @"wheelSpin": @[@0, @1, @2, @3, @4],
        @"tipMono": @[@YES, @NO],
        @"fontSize": @[@12, @14, @16, @20, @24],
        @"tipSize": @[@0.75, @1.0, @1.35, @1.75],
        @"tipColor": @[@0, @1, @2, @3, @4, @5],
        @"textColor": @[@0, @1, @2, @3, @4, @5],
        @"tipMiddle": @[@NO, @YES],
        @"labelsHover": @[@YES, @NO],
        @"away": @[@60, @120, @300, @0],
    };
    return d;
}
static NSArray *SHAPE_NAMES, *FRAME_NAMES, *WHEEL_NAMES, *TIP_BG_NAMES, *TEXT_NAMES, *AWAY_TEXT, *PV_NAMES;
static const int PV_STATES[6] = {ST_IDLE, ST_THINK, ST_WORK, ST_WAIT, ST_CHEER, ST_SLEEP};

enum { K_PICK, K_SLIDE, K_TOGGLE, K_CYCLE, K_STATE, K_RESET, K_CLOSE, K_TAB, K_MSTATE, K_MTOG, K_MPLAY };
// the Motions page: Claude Code's states, then the panel's own
static const int MOT_STATES[ST_N] = {ST_IDLE, ST_THINK, ST_WORK, ST_WAIT, ST_CHEER, ST_SLEEP, ST_SPEAK, ST_LISTEN, ST_TRANSCRIBE, ST_STANDBY, ST_ERROR};
static NSString *const MOT_STATE_NAMES[ST_N] = {@"Idle", @"Thinking", @"Working", @"Needs you", @"Done", @"Asleep",
                                              @"Speaking", @"Listening", @"Transcribing", @"Away", @"Error"};
static NSString *state_name(int st) { for (int i = 0; i < ST_N; i++) if (MOT_STATES[i] == st) return MOT_STATE_NAMES[i]; return @"?"; }
static int bits(uint32_t v) { int n = 0; for (; v; v &= v - 1) n++; return n; }
@interface Hit : NSObject
@property NSRect r;
@property int kind, val, n;
@property(copy) NSString *key;
@property CGFloat t0, t1; // a slider's track, left and right
@end
@implementation Hit
@end

// ---- the preview: the cluster with four made-up sessions ---------------------------------------------------
@interface PreviewView : NSView
@property(weak) id<SettingsHost> host;
@property int state;          // what the main core shows
- (Session *)main;            // the main core's session (the Motions page plays a routine or an action on it)
@property double lockUntil;   // the focus frame shows until then (a frame was just picked or is pointed at)
@property float zoom;
- (void)tick:(double)now;
@end

@implementation PreviewView {
    Room _room;
    Canvas _cv;
    double _last, _orbit;
    NSPoint _main, _grab, _vel; // the main core's centre (view points), where it was grabbed, its speed
    BOOL _dragging;
    NSRect _mainRect;
    Orbit _ob[4];             // the small cores (1..3), in this view's points
}
- (BOOL)isOpaque { return NO; }
- (BOOL)mouseDownCanMoveWindow { return NO; }
- (void)setup:(double)now {
    if (_room.n) return;
    static const char *names[4] = {"vigil", "api", "docs", "web"};
    _room.n = 4;
    for (int i = 0; i < 4; i++) {
        session_init(&_room.s[i], names[i], now, [self.host shape]);
        snprintf(_room.s[i].name, sizeof _room.s[i].name, "%s", names[i]);
        _room.s[i].aura = HUES[i];
    }
    _last = now;
    self.zoom = 1;
    self.state = ST_THINK;
}
- (float)num:(NSString *)k { return [[self.host setting:k] floatValue]; }
- (Session *)main { [self setup:[[NSDate date] timeIntervalSince1970]]; return &_room.s[0]; }
// a routine just switched off for a state makes way at once
- (void)dropRoutine:(int)rt state:(int)st {
    for (int i = 0; i < _room.n; i++) if (_room.s[i].mo.rt == rt && _room.s[i].mo.routine == st) _room.s[i].mo.routine = -1;
}

- (void)tick:(double)now {
    [self setup:now];
    float dt = (float)fmin(0.1, now - _last);
    _last = now;
    BOOL others = [[self.host setting:@"others"] boolValue];
    int shown[4] = {self.state, ST_WORK, ST_IDLE, ST_THINK};
    for (int i = 0; i < 4; i++) {
        Session *s = &_room.s[i];
        s->state = shown[i] == ST_SLEEP ? ST_IDLE : shown[i];
        s->overlay = shown[i] == ST_IDLE ? 0 : shown[i];
        s->last = now;
        s->aura_a = others ? TOUCH.aura : 0;
    }
    NSSize b = self.bounds.size;
    NSPoint c = NSMakePoint(b.width / 2, b.height / 2);
    if (_main.x == 0 && _main.y == 0) _main = c;
    if (_dragging) {
        NSPoint m = [self convertPoint:[self.window mouseLocationOutsideOfEventStream] fromView:nil];
        NSPoint to = NSMakePoint(m.x - _grab.x, m.y - _grab.y);
        if (dt > 0) { _vel.x += ((to.x - _main.x) / dt - _vel.x) * 0.45; _vel.y += ((to.y - _main.y) / dt - _vel.y) * 0.45; }
        _main = to;
    } else { // let go: it glides back to the middle
        float f = 1 - expf(-dt * 5);
        _main.x += (c.x - _main.x) * f; _main.y += (c.y - _main.y) * f;
        _vel.x *= 0.8; _vel.y *= 0.8;
    }
    Session *m = &_room.s[0];
    CGFloat scale = [self num:@"scale"];
    CGFloat cw = fmin(b.width, b.height) * 0.97 * (0.55 + 0.45 * (scale - 0.75) / 0.85) * self.zoom;
    CGFloat big = cw * 0.6;
    m->carried = _dragging;
    m->move_x = (float)(_vel.x / big); m->move_y = (float)(-_vel.y / big);
    m->lock_to = _dragging || now < self.lockUntil ? 1.0f : 0.0f;
    m->hover_to = _dragging;
    room_tick(&_room, [self.host shape], now, dt);
    if (!_dragging) _orbit += dt;
    // the small cores: gravity holds them on the orbit; dragged, the main core leaves them sliding round to the
    // trailing side, and a fast fling sends them bouncing round the preview until the core comes close
    CGFloat rx = cw / 2 - 8, ry = rx * 1.15, small = others ? fmin(cw * 0.2, 2 * M_PI * rx / 3 * 0.75) : 0;
    rx -= small / 2; ry -= small / 2;
    OrbitK ok = orbit_k([self num:@"gravity"], (float)rx, (float)small);
    Orbit *ring[3];
    int rn = 0;
    for (int i = 1; i < 4; i++) if (_ob[i].init && !_ob[i].free) ring[rn++] = &_ob[i];
    for (int i = 1; i < 4; i++) {
        Orbit *ob = &_ob[i];
        double a = M_PI / 2 + 2 * M_PI * (i - 1) / 3 + _orbit * 0.10;
        float bob = (float)(3 * sin(_orbit * 0.8 + i * 1.7));
        float tx = (float)(_main.x + rx * cos(a)), ty = (float)(_main.y + ry * sin(a)) + bob;
        if (!ob->init) { ob->px = tx; ob->py = ty; ob->vx = ob->vy = 0; ob->w = (float)small; ob->init = 1; ob->free = 0; }
        ob->w += ((float)small - ob->w) * (1 - expf(-dt * 7));
        if (ob->free) {
            fly_step(ob, dt);
            float hs = ob->w / 2;
            if (ob->px < hs) { ob->px = hs; ob->vx = fabsf(ob->vx) * ORBIT_BOUNCE; }
            if (ob->px > b.width - hs) { ob->px = (float)b.width - hs; ob->vx = -fabsf(ob->vx) * ORBIT_BOUNCE; }
            if (ob->py < hs) { ob->py = hs; ob->vy = fabsf(ob->vy) * ORBIT_BOUNCE; }
            if (ob->py > b.height - hs) { ob->py = (float)b.height - hs; ob->vy = -fabsf(ob->vy) * ORBIT_BOUNCE; }
            if (hypotf(ob->px - (float)_main.x, ob->py - (float)_main.y) < ok.CATCH || (!_dragging && ob->rest > 1.5f)) ob->free = 0;
        } else if (bead_step(ob, (float)_main.x, (float)_main.y + bob, tx, ty, a, (float)rx, (float)ry, (float)small, _dragging ? 0 : ok.KT, &ok, ring, rn, dt) > ok.BREAK) {
            ob->free = 1; ob->rest = 0;
        }
    }
    [self setNeedsDisplay:YES];
}

- (CGImageRef)newPictureOf:(const Session *)s pixels:(int)px now:(double)now CF_RETURNS_RETAINED {
    canvas_resize(&_cv, px, px);
    render_one(&_cv, s, [self.host shape], now, [self num:@"glow"], [self num:@"core"]);
    size_t n = (size_t)px * (size_t)px * 4;
    uint8_t *buf = malloc(n);
    canvas_rgba(&_cv, buf);
    CFDataRef data = CFDataCreateWithBytesNoCopy(NULL, buf, (CFIndex)n, kCFAllocatorMalloc);
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
    if (!_room.n) return;
    double now = _last;
    CGContextRef ctx = [NSGraphicsContext currentContext].CGContext;
    NSSize b = self.bounds.size;
    CGFloat px = self.window ? self.window.backingScaleFactor : 2;
    CGFloat scale = [self num:@"scale"];
    BOOL others = [[self.host setting:@"others"] boolValue];
    // the same proportions as drawSingle on the screen, on a cluster cw wide
    CGFloat cw = fmin(b.width, b.height) * 0.97 * (0.55 + 0.45 * (scale - 0.75) / 0.85) * self.zoom;
    CGFloat big = cw * 0.6;
    [NSGraphicsContext saveGraphicsState];
    NSRectClip(self.bounds);
    for (int i = 1; i < 4 && others; i++) {
        CGFloat w = _ob[i].w;
        NSRect r = NSMakeRect(_ob[i].px - w / 2, _ob[i].py - w / 2, w, w);
        int p = ((int)(w * px) / 4) * 4;
        if (p < 8) continue;
        CGImageRef img = [self newPictureOf:&_room.s[i] pixels:p now:now];
        CGContextDrawImage(ctx, NSRectToCGRect(r), img);
        CGImageRelease(img);
    }
    _mainRect = NSMakeRect(_main.x - big / 2, _main.y - big / 2, big, big);
    int p = ((int)(big * px) / 4) * 4;
    if (p >= 8) {
        CGImageRef img = [self newPictureOf:&_room.s[0] pixels:p now:now];
        CGContextDrawImage(ctx, NSRectToCGRect(_mainRect), img);
        CGImageRelease(img);
        [self.host drawFocusFrame:ctx rect:_mainRect session:&_room.s[0] now:now];
    }
    [NSGraphicsContext restoreGraphicsState];
}

- (BOOL)onMain:(NSPoint)p { return hypot(p.x - NSMidX(_mainRect), p.y - NSMidY(_mainRect)) < _mainRect.size.width * SHELL_HIT; }
- (void)mouseDown:(NSEvent *)e {
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    if (e.clickCount == 2) { self.zoom = 1; return; }
    if (![self onMain:p]) { [self.window performWindowDragWithEvent:e]; return; }
    _dragging = YES;
    _grab = NSMakePoint(p.x - _main.x, p.y - _main.y);
}
- (void)mouseUp:(NSEvent *)e { (void)e; _dragging = NO; }
- (void)scrollWheel:(NSEvent *)e { // zoom in up to 3 times, to see the detail
    float step = (float)e.scrollingDeltaY * (e.hasPreciseScrollingDeltas ? 0.01f : 0.1f);
    self.zoom = fmaxf(1, fminf(3, self.zoom + step));
    [self.superview setNeedsDisplay:YES]; // the hint shows the zoom
}
- (void)dealloc { free(_cv.p); }
@end

// ---- the window's own view ------------------------------------------------------------------------------------
@interface SettingsView : NSView
@property(weak) id<SettingsHost> host;
@property(weak) SettingsWindow *owner;
@property(strong) PreviewView *preview;
@property(strong) NSMutableArray<Hit *> *hits;
@property(strong) Hit *hot, *sliding;
@property BOOL closeArmed;
@property(copy) NSString *picking; // the colour row the colour panel is open for: tipCustom or textCustom
@property int tab;                 // 0 General, 1 Motions
@property double noteUntil;        // "a state needs at least one routine" shows until then
@end

@interface SettingsWindow ()
@property(strong) NSPanel *panel;
@property(strong) SettingsView *view;
@property(strong) NSTimer *timer;
@property(weak) id<SettingsHost> host;
@end

@interface SettingsPanel : NSPanel
@end
@implementation SettingsPanel
- (BOOL)canBecomeKeyWindow { return YES; }
@end

@implementation SettingsView

- (BOOL)isFlipped { return YES; }
- (BOOL)isOpaque { return NO; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { (void)e; return YES; }
- (BOOL)mouseDownCanMoveWindow { return NO; }

// ---- drawing helpers (Windows' logical units, y down) ----
static void fill_rr(NSRect r, CGFloat rad, unsigned c) { [hex_color(c) setFill]; [[NSBezierPath bezierPathWithRoundedRect:r xRadius:rad yRadius:rad] fill]; }
static void edge_rr(NSRect r, CGFloat rad, unsigned c, CGFloat w) {
    NSBezierPath *p = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(r, w / 2, w / 2) xRadius:fmax(0, rad - w / 2) yRadius:fmax(0, rad - w / 2)];
    p.lineWidth = w; [hex_color(c) setStroke]; [p stroke];
}
static void box(CGFloat x0, CGFloat y0, CGFloat x1, CGFloat y1, CGFloat rad, unsigned fill, unsigned edge, CGFloat ew) {
    NSRect r = NSMakeRect(x0, y0, x1 - x0, y1 - y0);
    fill_rr(r, rad, fill);
    if (ew > 0) edge_rr(r, rad, edge, ew);
}
static void disc(CGFloat cx, CGFloat cy, CGFloat rad, unsigned c, CGFloat a) {
    [[hex_color(c) colorWithAlphaComponent:a] setFill];
    [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(cx - rad, cy - rad, rad * 2, rad * 2)] fill];
}
static NSFont *ui(CGFloat size, NSFontWeight w) { return [NSFont systemFontOfSize:size weight:w]; }
// text in a box: align 0 left, 1 centre, 2 right; vertically centred unless wrap
static void tx(CGFloat x0, CGFloat y0, CGFloat x1, CGFloat y1, NSString *t, NSFont *f, unsigned c, int align, BOOL wrap) {
    NSMutableParagraphStyle *p = [[NSMutableParagraphStyle alloc] init];
    p.alignment = align == 1 ? NSTextAlignmentCenter : align == 2 ? NSTextAlignmentRight : NSTextAlignmentLeft;
    p.lineBreakMode = wrap ? NSLineBreakByWordWrapping : NSLineBreakByTruncatingTail;
    NSDictionary *a = @{NSFontAttributeName: f, NSForegroundColorAttributeName: hex_color(c), NSParagraphStyleAttributeName: p};
    CGFloat h = wrap ? y1 - y0 : ceil(f.ascender - f.descender + f.leading);
    CGFloat y = wrap ? y0 : y0 + (y1 - y0 - h) / 2;
    [t drawWithRect:NSMakeRect(x0, y, x1 - x0, h) options:NSStringDrawingUsesLineFragmentOrigin | NSStringDrawingTruncatesLastVisibleLine attributes:a context:nil];
}

- (void)hit:(CGFloat)x0 :(CGFloat)y0 :(CGFloat)x1 :(CGFloat)y1 kind:(int)kind key:(NSString *)key val:(int)val {
    Hit *h = [[Hit alloc] init];
    h.r = NSMakeRect(x0, y0, x1 - x0, y1 - y0); h.kind = kind; h.key = key; h.val = val;
    [self.hits addObject:h];
}
- (BOOL)isHot:(int)kind key:(NSString *)key val:(int)val {
    Hit *h = self.hot;
    return h && h.kind == kind && h.val == val && (key == h.key || [key isEqualToString:h.key]);
}
- (id)value:(NSString *)key { return [self.host setting:key]; }
// the index of the chosen value in a row, -1 if it is none of them
- (int)current:(NSString *)key {
    id v = [self value:key];
    NSArray *opts = choices()[key];
    for (NSUInteger i = 0; i < opts.count; i++) {
        id o = opts[i];
        if ([o isKindOfClass:[NSString class]] ? [o isEqual:v] : fabs([o doubleValue] - [v doubleValue]) < 0.001) return (int)i;
    }
    return -1;
}

// a chip or a tile: ringed in orange when chosen
- (void)pick:(CGFloat)x0 :(CGFloat)y0 :(CGFloat)x1 :(CGFloat)y1 rad:(CGFloat)rad key:(NSString *)key val:(int)val label:(NSString *)label {
    BOOL sel = [self current:key] == val, hot = [self isHot:K_PICK key:key val:val];
    box(x0, y0, x1, y1, rad, label ? (hot ? S_CHIP_HOT : S_CHIP) : (hot ? S_CHIP_HOT : S_TILE), sel ? S_OR : S_EDGE, sel ? 1.5 : label ? 0 : 1);
    if (label) tx(x0 + 4, y0, x1 - 4, y1, label, ui(12.5, NSFontWeightRegular), sel ? 0xFFFFFF : S_CHIP_TXT, 1, NO);
    [self hit:x0 :y0 :x1 :y1 kind:K_PICK key:key val:val];
}

// a slider of n stops: the track, filled in orange up to the knob
- (void)slider:(CGFloat)x0 y:(CGFloat)y x1:(CGFloat)x1 key:(NSString *)key n:(int)n {
    int cur = [self current:key];
    CGFloat t = cur < 0 ? 0.5 : (CGFloat)cur / (n - 1), kx = x0 + (x1 - x0) * t;
    box(x0, y - 2, x1, y + 2, 2, S_EDGE, 0, 0);
    box(x0, y - 2, kx, y + 2, 2, S_OR, 0, 0);
    for (int i = 1; i < n - 1; i++) { CGFloat sx = x0 + (x1 - x0) * i / (n - 1); if (sx > kx) disc(sx, y, 2, S_MUTED, 1); }
    BOOL hot = self.hot.kind == K_SLIDE && [self.hot.key isEqualToString:key];
    disc(kx, y, hot ? 12 : 11, S_OR, 0.25);
    disc(kx, y, 9, S_KNOB, 1);
    Hit *h = [[Hit alloc] init];
    h.r = NSMakeRect(x0 - 12, y - 14, x1 - x0 + 24, 28); h.kind = K_SLIDE; h.key = key; h.n = n; h.t0 = x0; h.t1 = x1;
    [self.hits addObject:h];
}

- (void)toggle:(CGFloat)l :(CGFloat)y :(CGFloat)r key:(NSString *)key title:(NSString *)title sub:(NSString *)sub enabled:(BOOL)enabled {
    BOOL on = [[self value:key] boolValue] && enabled, hot = [self isHot:K_TOGGLE key:key val:0];
    tx(l, y, r - 56, y + 18, title, ui(13, NSFontWeightRegular), enabled ? S_TEXT : S_MUTED, 0, NO);
    if (sub) tx(l, y + 18, r - 56, y + 36, sub, ui(11.5, NSFontWeightRegular), S_MUTED, 0, NO);
    CGFloat cy = sub ? y + 18 : y + 9;
    box(r - 42, cy - 12, r, cy + 12, 12, on ? S_OR : (hot && enabled ? S_EDGE_HOT : S_EDGE), 0, 0);
    disc(on ? r - 12 : r - 30, cy, 9, on ? S_ON_OR : S_LABEL, enabled ? 1 : 0.5);
    if (enabled) [self hit:l :y - 2 :r :y + (sub ? 38 : 22) kind:K_TOGGLE key:key val:0];
}

// the four shapes as small line icons
- (void)shapeIcon:(int)i cx:(CGFloat)cx cy:(CGFloat)cy color:(unsigned)col {
    NSBezierPath *p = [NSBezierPath bezierPath];
    p.lineWidth = 1.6; p.lineJoinStyle = NSLineJoinStyleRound;
    CGFloat R = 13;
    if (i == 0 || i == 1) { // a pentagon (in a ring for the buckyball)
        CGFloat r = i ? 7 : R;
        for (int k = 0; k < 5; k++) {
            double a = -M_PI / 2 + k * 2 * M_PI / 5;
            NSPoint q = NSMakePoint(cx + r * cos(a), cy + r * sin(a));
            if (k) [p lineToPoint:q]; else [p moveToPoint:q];
        }
        [p closePath];
        if (i) [p appendBezierPathWithOvalInRect:NSMakeRect(cx - R, cy - R, 2 * R, 2 * R)];
    } else if (i == 2) { // a hexagon with its triangles
        NSPoint v[6];
        for (int k = 0; k < 6; k++) { double a = -M_PI / 2 + k * M_PI / 3; v[k] = NSMakePoint(cx + R * cos(a), cy + R * sin(a)); }
        [p moveToPoint:v[0]];
        for (int k = 1; k < 6; k++) [p lineToPoint:v[k]];
        [p closePath];
        for (int k = 0; k < 6; k += 2) { [p moveToPoint:v[k]]; [p lineToPoint:v[(k + 2) % 6]]; }
        for (int k = 0; k < 3; k++) { [p moveToPoint:v[k]]; [p lineToPoint:v[k + 3]]; }
    } else { // a ring with spokes
        [p appendBezierPathWithOvalInRect:NSMakeRect(cx - R, cy - R, 2 * R, 2 * R)];
        for (int k = 0; k < 4; k++) {
            double a = k * M_PI / 4;
            [p moveToPoint:NSMakePoint(cx + R * cos(a), cy + R * sin(a))];
            [p lineToPoint:NSMakePoint(cx - R * cos(a), cy - R * sin(a))];
        }
        [p appendBezierPathWithOvalInRect:NSMakeRect(cx - 5, cy - 5, 10, 10)];
    }
    [hex_color(col) setStroke];
    [p stroke];
}

- (unsigned)tipBg {
    int i = [[self value:@"tipColor"] intValue];
    return i >= 0 && i < 5 ? TIP_BGS[i] : (unsigned)[[self value:@"tipCustom"] unsignedIntValue];
}
- (unsigned)textCol {
    int i = [[self value:@"textColor"] intValue];
    if (i == 5) return [[self value:@"textCustom"] unsignedIntValue];
    if (i > 0 && i < 5) return TEXT_COLS[i];
    return hex_light([self tipBg]) ? 0x45464B : 0xE6E8F0;
}

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    self.hits = [NSMutableArray array];
    box(0, 0, WW, WH, 18, S_BG, 0x2A2A30, 1);
    // title, tabs (one for now), close
    tx(28, 14, 400, 50, @"Settings", ui(18, NSFontWeightSemibold), S_TEXT, 0, NO);
    NSArray *tabs = @[@"General", @"Motions"];
    for (int i = 0; i < 2; i++) { // the open page ringed in orange like a chosen chip, the other plain text
        CGFloat a = 132 + i * 96;
        BOOL on = self.tab == i, hot = [self isHot:K_TAB key:nil val:i];
        if (on) box(a, 19, a + 88, 45, 13, S_CHIP, S_OR, 1.5);
        else if (hot) box(a, 19, a + 88, 45, 13, S_CHIP_HOT, 0, 0);
        tx(a, 19, a + 88, 45, tabs[i], ui(12.5, NSFontWeightRegular), on ? 0xFFFFFF : hot ? S_TEXT : 0xC4C7CF, 1, NO);
        [self hit:a :19 :a + 88 :45 kind:K_TAB key:nil val:i];
    }
    BOOL ch = [self isHot:K_CLOSE key:nil val:0];
    box(824, 16, 856, 48, 10, ch ? S_CLOSE_HOT : 0x26262B, 0, 0);
    NSBezierPath *x = [NSBezierPath bezierPath];
    [x moveToPoint:NSMakePoint(835, 27)]; [x lineToPoint:NSMakePoint(845, 37)];
    [x moveToPoint:NSMakePoint(845, 27)]; [x lineToPoint:NSMakePoint(835, 37)];
    x.lineWidth = 1.6; x.lineCapStyle = NSLineCapStyleRound;
    [hex_color(ch ? 0xFFFFFF : S_LABEL) setStroke]; [x stroke];
    [self hit:824 :16 :856 :48 kind:K_CLOSE key:nil val:0];

    // the preview band: the cluster on the left (its own view); a reply in the text box's look, the hint and the states
    box(24, 58, 856, 288, 16, S_BAND, 0, 0);
    BOOL mono = [[self value:@"tipMono"] boolValue];
    CGFloat fs = fmin(16, [[self value:@"fontSize"] doubleValue]);
    NSFont *tf = ui(fs, NSFontWeightSemibold);
    NSFont *bf = mono ? ([NSFont fontWithName:[self value:@"fontName"] size:fs] ?: [NSFont monospacedSystemFontOfSize:fs weight:NSFontWeightRegular]) : ui(fs, NSFontWeightRegular);
    CGFloat th = ceil(tf.ascender - tf.descender), bh = ceil(bf.ascender - bf.descender + bf.leading);
    CGFloat sh = fmin(112, 22 + th + 6 + bh * 2);
    unsigned bg = [self tipBg];
    box(580, 74, 840, 74 + sh, 10, bg, 0, 0);
    box(580, 74, 583, 74 + sh, 1.5, S_OR, 0, 0);
    tx(594, 84, 832, 84 + th, @"vigil · Done", tf, hex_light(bg) ? 0x6B3A0B : 0xFFB877, 0, NO);
    tx(594, 84 + th + 6, 832, 74 + sh - 6, @"The satellites slide round the orbit and trail behind.", bf, [self textCol], 0, YES);
    NSString *hint = self.preview.zoom > 1.01 ? [NSString stringWithFormat:@"Scroll to zoom: %.1fx   ·   double-click resets", self.preview.zoom]
                                              : @"Drag the core   ·   scroll on it to zoom in";
    tx(580, 190, 840, 210, hint, ui(12.5, NSFontWeightRegular), S_MUTED, 0, NO);
    for (int i = 0; i < 6; i++) {
        CGFloat x0 = 580 + (i % 3) * (260 + 6) / 3.0, y0 = 216 + (i / 3) * 34, x1 = x0 + (260 - 12) / 3.0;
        BOOL on = PV_STATES[i] == self.preview.state, hot = [self isHot:K_STATE key:nil val:i];
        box(x0, y0, x1, y0 + 28, 14, hot ? S_CHIP_HOT : S_CHIP, S_OR, on ? 1.5 : 0);
        tx(x0, y0, x1, y0 + 28, PV_NAMES[i], ui(12.5, NSFontWeightRegular), on ? S_TEXT : S_CHIP_TXT, 1, NO);
        [self hit:x0 :y0 :x1 :y0 + 28 kind:K_STATE key:nil val:i];
    }

    if (self.tab == 1) [self motionsPage]; else [self generalPage];
    [self foot];
}

- (void)generalPage {
    // the cards
    unsigned bg = [self tipBg];
    const CGFloat y0 = 304, cw = (832 - 32) / 3.0, y1 = 770;
    CGFloat cx[3];
    NSArray *titles = @[@"Look", @"Motion", @"Replies"];
    for (int k = 0; k < 3; k++) {
        cx[k] = 24 + k * (cw + 16);
        box(cx[k], y0, cx[k] + cw, y1, 16, S_CARD, 0, 0);
        tx(cx[k] + 16, y0 + 14, cx[k] + cw - 16, y0 + 36, titles[k], ui(15, NSFontWeightSemibold), S_TEXT, 0, NO);
    }
    NSFont *label = ui(12.5, NSFontWeightRegular), *small = ui(11.5, NSFontWeightRegular);

    // Look: shape, glow, size on screen, inner core
    CGFloat l = cx[0] + 16, r = cx[0] + cw - 16, tw = (r - l - 18) / 4;
    tx(l, y0 + 46, r, y0 + 64, @"Shape", label, S_LABEL, 0, NO);
    for (int i = 0; i < 4; i++) {
        CGFloat a = l + i * (tw + 6);
        BOOL sel = [self current:@"shape"] == i;
        [self pick:a :y0 + 70 :a + tw :y0 + 116 rad:12 key:@"shape" val:i label:nil];
        [self shapeIcon:i cx:a + tw / 2 cy:y0 + 93 color:sel ? S_OR : S_MUTED];
    }
    int hs = self.hot.kind == K_PICK && [self.hot.key isEqualToString:@"shape"] ? self.hot.val : [self current:@"shape"];
    tx(l + 60, y0 + 46, r, y0 + 64, SHAPE_NAMES[hs < 0 ? 2 : hs], small, S_MUTED, 2, NO);
    tx(l, y0 + 128, r, y0 + 146, @"Glow", label, S_LABEL, 0, NO);
    disc(l + 4, y0 + 164, 3.5, S_OR, 0.45);
    [self slider:l + 20 y:y0 + 164 x1:r - 22 key:@"glow" n:3];
    disc(r - 5, y0 + 164, 10, S_OR, 0.2); disc(r - 5, y0 + 164, 5.5, S_OR, 1);
    tx(l, y0 + 186, r, y0 + 204, @"Size on screen", label, S_LABEL, 0, NO);
    tx(l - 2, y0 + 212, l + 12, y0 + 232, @"S", small, S_MUTED, 1, NO);
    [self slider:l + 20 y:y0 + 222 x1:r - 22 key:@"scale" n:4];
    tx(r - 14, y0 + 210, r + 2, y0 + 234, @"L", ui(13, NSFontWeightRegular), S_MUTED, 1, NO);
    tx(l, y0 + 244, r, y0 + 262, @"Inner core", label, S_LABEL, 0, NO);
    disc(l + 4, y0 + 280, 2.5, S_KNOB, 0.8);
    [self slider:l + 20 y:y0 + 280 x1:r - 22 key:@"core" n:4];
    disc(r - 5, y0 + 280, 5.5, S_KNOB, 0.8);

    // Motion: gravity, other sessions, focus frame, what ⌥-scroll does (scrolling alone sets the volume on the Mac)
    l = cx[1] + 16; r = cx[1] + cw - 16;
    tx(l, y0 + 46, r, y0 + 64, @"Gravity", label, S_LABEL, 0, NO);
    [self slider:l + 8 y:y0 + 82 x1:r - 8 key:@"gravity" n:4];
    tx(l, y0 + 94, r, y0 + 112, @"Floaty", small, S_MUTED, 0, NO);
    tx(l, y0 + 94, r, y0 + 112, @"Firm", small, S_MUTED, 2, NO);
    [self toggle:l :y0 + 124 :r key:@"others" title:@"Other sessions" sub:@"Small cores around the main one" enabled:YES];
    tx(l, y0 + 172, r, y0 + 190, @"Focus frame", label, S_LABEL, 0, NO);
    CGFloat fw = (r - l - 12) / 3;
    for (int i = 0; i < 7; i++) {
        CGFloat a = l + (i % 3) * (fw + 6), b = y0 + 194 + (i / 3) * 34;
        [self pick:a :b :a + fw :b + 28 rad:14 key:@"frame" val:i label:FRAME_NAMES[i]];
    }
    tx(l, y0 + 304, r, y0 + 322, @"⌥ + scroll on a core", label, S_LABEL, 0, NO);
    for (int i = 0; i < 5; i++) {
        CGFloat a = l + (i % 3) * (fw + 6), b = y0 + 326 + (i / 3) * 34;
        [self pick:a :b :a + fw :b + 28 rad:14 key:@"wheelSpin" val:i label:WHEEL_NAMES[i]];
    }
    tx(l, y0 + 398, r, y0 + 456, @"Scroll alone sets that session's speech volume. Drag a core to spin it by hand; let go while moving and it spins on.",
       small, S_MUTED, 0, YES);

    // Replies: font, text size, box size, colours, where the box opens, labels
    l = cx[2] + 16; r = cx[2] + cw - 16;
    CGFloat bw = (r - l - 6) / 2;
    tx(l, y0 + 46, r, y0 + 64, @"Font", label, S_LABEL, 0, NO);
    for (int i = 0; i < 2; i++) {
        CGFloat a = l + i * (bw + 6);
        BOOL sel = [self current:@"tipMono"] == i;
        [self pick:a :y0 + 70 :a + bw :y0 + 118 rad:12 key:@"tipMono" val:i label:nil];
        NSFont *aa = i ? ui(22, NSFontWeightRegular) : ([NSFont fontWithName:[self value:@"fontName"] size:20] ?: [NSFont monospacedSystemFontOfSize:20 weight:NSFontWeightRegular]);
        tx(a, y0 + 72, a + bw, y0 + 100, @"Aa", aa, sel ? S_TEXT : S_CHIP_TXT, 1, NO);
        tx(a, y0 + 98, a + bw, y0 + 114, i ? @"Clean" : @"Terminal", small, sel ? S_TEXT : S_MUTED, 1, NO);
    }
    tx(l, y0 + 130, r, y0 + 148, @"Text size", label, S_LABEL, 0, NO);
    tx(l - 2, y0 + 156, l + 12, y0 + 176, @"A", small, S_MUTED, 1, NO);
    [self slider:l + 20 y:y0 + 166 x1:r - 22 key:@"fontSize" n:5];
    tx(r - 16, y0 + 154, r + 2, y0 + 180, @"A", ui(15, NSFontWeightSemibold), S_MUTED, 1, NO);
    tx(l, y0 + 190, r, y0 + 208, @"Box size", label, S_LABEL, 0, NO);
    box(l, y0 + 220, l + 10, y0 + 228, 2, S_MUTED, 0, 0);
    [self slider:l + 20 y:y0 + 224 x1:r - 22 key:@"tipSize" n:4];
    box(r - 12, y0 + 216, r + 2, y0 + 232, 3, S_MUTED, 0, 0);
    tx(l, y0 + 248, r, y0 + 266, @"Colours", label, S_LABEL, 0, NO);
    BOOL hb = self.hot.kind == K_PICK && [self.hot.key isEqualToString:@"tipColor"], ht = self.hot.kind == K_PICK && [self.hot.key isEqualToString:@"textColor"];
    int bi = hb ? self.hot.val : [[self value:@"tipColor"] intValue], ti = ht ? self.hot.val : [[self value:@"textColor"] intValue];
    tx(l + 60, y0 + 248, r, y0 + 266, [NSString stringWithFormat:@"%@  ·  %@", TIP_BG_NAMES[bi], TEXT_NAMES[ti]], small, S_MUTED, 2, NO);
    CGFloat sl = l + 38, sw = (r - sl - 5 * 6) / 6;
    for (int row = 0; row < 2; row++) { // Box: the swatches, then the palette; Text: "Aa" in each colour on the box's colour, then the palette
        CGFloat yy = y0 + 272 + row * 34;
        NSString *key = row ? @"textColor" : @"tipColor";
        tx(l, yy, sl - 4, yy + 26, row ? @"Text" : @"Box", small, S_MUTED, 0, NO);
        for (int i = 0; i < 6; i++) {
            CGFloat a = sl + i * (sw + 6);
            BOOL sel = [[self value:key] intValue] == i, hot = [self isHot:K_PICK key:key val:i];
            unsigned fill = row ? bg : i < 5 ? TIP_BGS[i] : [[self value:@"tipCustom"] unsignedIntValue];
            box(a, yy, a + sw, yy + 26, 7, fill, sel ? S_OR : hot ? S_EDGE_HOT : S_EDGE, sel ? 1.5 : 1);
            unsigned tc = row ? (i == 5 ? [[self value:@"textCustom"] unsignedIntValue] : i ? TEXT_COLS[i] : (hex_light(bg) ? 0x45464B : 0xE6E8F0)) : 0;
            if (i == 5) {
                NSImage *pal = [NSImage imageWithSystemSymbolName:@"paintpalette" accessibilityDescription:@"Custom colour"];
                NSImageSymbolConfiguration *cfg = [NSImageSymbolConfiguration configurationWithPointSize:13 weight:NSFontWeightRegular];
                pal = [pal imageWithSymbolConfiguration:cfg];
                unsigned ic = row ? tc : hex_light(fill) ? 0x2A2A30 : (hot || sel ? 0xFFFFFF : 0xD4D4DA);
                NSImage *tinted = [NSImage imageWithSize:pal.size flipped:NO drawingHandler:^BOOL(NSRect dst) {
                    [pal drawInRect:dst];
                    [hex_color(ic) set];
                    NSRectFillUsingOperation(dst, NSCompositingOperationSourceAtop);
                    return YES;
                }];
                NSSize s = tinted.size;
                [tinted drawInRect:NSMakeRect(a + (sw - s.width) / 2, yy + (26 - s.height) / 2, s.width, s.height) fromRect:NSZeroRect
                         operation:NSCompositingOperationSourceOver fraction:1 respectFlipped:YES hints:nil];
            } else if (row) tx(a, yy, a + sw, yy + 26, @"Aa", small, tc, 1, NO);
            [self hit:a :yy :a + sw :yy + 26 kind:K_PICK key:key val:i];
        }
    }
    tx(l, y0 + 342, r, y0 + 360, @"Opens", label, S_LABEL, 0, NO);
    NSArray *opens = @[@"Next to core", @"Middle"], *labels = @[@"On hover", @"Always"];
    for (int i = 0; i < 2; i++) { CGFloat a = l + i * (bw + 6); [self pick:a :y0 + 364 :a + bw :y0 + 392 rad:10 key:@"tipMiddle" val:i label:opens[i]]; }
    tx(l, y0 + 404, r, y0 + 422, @"Names over cores (all-cores mode)", label, S_LABEL, 0, NO);
    for (int i = 0; i < 2; i++) { CGFloat a = l + i * (bw + 6); [self pick:a :y0 + 426 :a + bw :y0 + 454 rad:10 key:@"labelsHover" val:i label:labels[i]]; }

    // Voice (macOS): four switches in a row
    box(24, 786, 856, 846, 16, S_CARD, 0, 0);
    tx(40, 786, 110, 846, @"Voice", ui(15, NSFontWeightSemibold), S_TEXT, 0, NO);
    CGFloat vw = (840 - 110 - 3 * 20) / 4.0;
    NSArray *vk = @[@"voice", @"autoSpeak", @"summarize", @"confirmSend"];
    NSArray *vt = @[@"Spoken reports", @"Read new replies", @"Claude summaries", @"Confirm send"];
    for (int i = 0; i < 4; i++) {
        CGFloat a = 110 + i * (vw + 20);
        BOOL en = i != 2 || [self.host canSummarize]; // (turning a reading option on turns the voice on too)
        [self toggle:a :807 :a + vw key:vk[i] title:vt[i] sub:nil enabled:en];
    }

}

- (void)foot {
    NSFont *label = ui(12.5, NSFontWeightRegular);
    // the foot: rest time, reset
    int aw = [self current:@"away"];
    tx(28, 856, 120, 888, @"Rest after", label, S_MUTED, 0, NO);
    BOOL ha = [self isHot:K_CYCLE key:@"away" val:0];
    box(92, 859, 184, 885, 13, ha ? S_CHIP_HOT : S_CHIP, 0, 0);
    tx(92, 859, 184, 885, aw >= 0 ? AWAY_TEXT[aw] : @"custom", label, S_TEXT, 1, NO);
    [self hit:92 :859 :184 :885 kind:K_CYCLE key:@"away" val:0];
    tx(192, 856, 420, 888, aw == 3 ? @"(cores never rest)" : @"away: cores show STANDBY", label, S_MUTED, 0, NO);
    BOOL can = [self.host settingsSaved], hr = [self isHot:K_RESET key:nil val:0];
    tx(640, 856, 856, 888, @"Reset to defaults", label, can ? (hr ? 0xFFB877 : S_OR) : 0x55555E, 2, NO);
    if (can) [self hit:740 :860 :856 :884 kind:K_RESET key:nil val:0];
}

// ---- the Motions page: which routines and actions each state may use (MOTION_MAP), with a preview of each ----
- (void)motionChip:(CGFloat)x0 :(CGFloat)y0 :(CGFloat)x1 :(CGFloat)y1 on:(BOOL)on off:(BOOL)dis row:(NSString *)row id:(int)i name:(const char *)name {
    BOOL hot = [self isHot:K_MTOG key:row val:i], hp = [self isHot:K_MPLAY key:row val:i];
    box(x0, y0, x1, y1, (y1 - y0) / 2, hot ? S_CHIP_HOT : S_TILE, on ? S_OR : (hot ? S_EDGE_HOT : S_EDGE), on ? 1.5 : 1);
    tx(x0 + 10, y0, x1 - 19, y1, @(name) ?: @"?", ui(11.5, NSFontWeightRegular), on ? S_TEXT : dis ? 0x55555E : S_CHIP_TXT, 0, NO);
    CGFloat cy = (y0 + y1) / 2;
    NSBezierPath *t = [NSBezierPath bezierPath]; // the play button
    [t moveToPoint:NSMakePoint(x1 - 16, cy - 4)]; [t lineToPoint:NSMakePoint(x1 - 16, cy + 4)]; [t lineToPoint:NSMakePoint(x1 - 9, cy)]; [t closePath];
    [hex_color(hp ? S_OR : S_MUTED) setFill]; [t fill];
    if (!dis) [self hit:x0 :y0 :x1 - 22 :y1 kind:K_MTOG key:row val:i];
    [self hit:x1 - 23 :y0 :x1 :y1 kind:K_MPLAY key:row val:i];
}

- (void)motionsPage {
    const CGFloat y0 = 304, l = 40, r = 840;
    int st = self.preview.state;
    NSFont *label = ui(12.5, NSFontWeightRegular), *small = ui(11.5, NSFontWeightRegular);
    box(24, y0, 856, 846, 16, S_CARD, 0, 0);
    tx(l, y0 + 12, 300, y0 + 34, @"Motions", ui(15, NSFontWeightSemibold), S_TEXT, 0, NO);
    tx(300, y0 + 12, r, y0 + 34, @"Each state picks one of its routines every time it starts; actions play now and then.", small, S_MUTED, 2, NO);
    // the states: Claude Code's, then the panel's own
    NSArray *rowname = @[@"Claude Code", @"Panel"];
    for (int rw = 0; rw < 2; rw++) {
        CGFloat y = y0 + 42 + rw * 32;
        tx(l, y, l + 86, y + 26, rowname[rw], small, S_MUTED, 0, NO);
        int n = rw ? 5 : 6, first = rw ? 6 : 0;
        CGFloat cw = (r - (l + 88) - 5 * 6) / 6;
        for (int i = 0; i < n; i++) {
            int sti = MOT_STATES[first + i];
            BOOL on = sti == st, hot = [self isHot:K_MSTATE key:nil val:sti];
            CGFloat a = l + 88 + i * (cw + 6);
            box(a, y, a + cw, y + 26, 13, hot ? S_CHIP_HOT : S_CHIP, S_OR, on ? 1.5 : 0);
            Col h = LOOKS[sti].halo;
            [[NSColor colorWithSRGBRed:h.r green:h.g blue:h.b alpha:1] setFill];
            [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(a + 9.5, y + 9.5, 7, 7)] fill];
            tx(a + 22, y, a + cw - 6, y + 26, MOT_STATE_NAMES[first + i], label, on ? S_TEXT : S_CHIP_TXT, 0, NO);
            [self hit:a :y :a + cw :y + 26 kind:K_MSTATE key:nil val:sti];
        }
    }
    // the routines: this state's own first
    const MotionMap *mm = &MOTION_MAP[st];
    tx(l, y0 + 108, r, y0 + 128, [NSString stringWithFormat:@"Routines for %@", state_name(st)], label, S_LABEL, 0, NO);
    tx(l, y0 + 108, r, y0 + 128, @"click to use it here   ▶ to watch it", small, S_MUTED, 2, NO);
    int order[RT_N], n = 0;
    for (int pass = 0; pass < 2; pass++) for (int i = 1; i < RT_N; i++) if ((ROUTINES[i].home == st) == !pass) order[n++] = i;
    CGFloat cw = (r - l - 6 * 5) / 7;
    for (int i = 0; i < n; i++) {
        int id = order[i];
        CGFloat a = l + (i % 7) * (cw + 5), y = y0 + 132 + (i / 7) * 31;
        [self motionChip:a :y :a + cw :y + 26 on:(mm->rt >> id) & 1 off:id == RT_HOLE && st != ST_SLEEP row:@"rt" id:id name:ROUTINES[id].name];
    }
    // the actions
    CGFloat ya = y0 + 132 + ((n + 6) / 7) * 31 + 6;
    tx(l, ya, r, ya + 20, [NSString stringWithFormat:@"Actions for %@", state_name(st)], label, S_LABEL, 0, NO);
    CGFloat aw = (r - l - 7 * 5) / 8;
    for (int w = 1; w < W_N; w++) {
        CGFloat a = l + ((w - 1) % 8) * (aw + 5), y = ya + 24 + ((w - 1) / 8) * 31;
        [self motionChip:a :y :a + aw :y + 26 on:(mm->act >> w) & 1 off:NO row:@"act" id:w name:WHIMS[w].name];
    }
    // what the chip under the pointer does, or a summary
    CGFloat yd = ya + 24 + ((W_N - 2) / 8 + 1) * 31 + 2;
    Hit *h = self.hot;
    if (h && (h.kind == K_MTOG || h.kind == K_MPLAY)) {
        NSString *line;
        if ([h.key isEqualToString:@"rt"]) {
            NSString *home = ROUTINES[h.val].home != st && ROUTINES[h.val].home >= 0 ? [NSString stringWithFormat:@" (made for %@)", state_name(ROUTINES[h.val].home)] : @"";
            line = [NSString stringWithFormat:@"%s%@: %s", ROUTINES[h.val].name, home, ROUTINES[h.val].about ?: ""];
        } else line = [NSString stringWithFormat:@"%s: %s", WHIMS[h.val].name, WHIMS[h.val].about ?: ""];
        tx(l, yd, r, yd + 40, line, small, S_LABEL, 0, YES);
    } else if ([[NSDate date] timeIntervalSince1970] < self.noteUntil) {
        tx(l, yd, r, yd + 20, @"A state needs at least one routine: switch another on first.", small, 0xFFB877, 0, NO);
    } else {
        int nr = bits(mm->rt), na = bits(mm->act);
        tx(l, yd, r, yd + 20, [NSString stringWithFormat:@"%@ uses %d routine%@ and %d action%@. Changes apply the next time a state starts, and are kept.",
                               state_name(st), nr, nr == 1 ? @"" : @"s", na, na == 1 ? @"" : @"s"], small, S_MUTED, 0, NO);
    }
}

- (void)motionPlay:(NSString *)row id:(int)i {
    Session *m = [self.preview main];
    if ([row isEqualToString:@"rt"]) motion_play(m, i);
    else whim_play(m, i, [[NSDate date] timeIntervalSince1970]);
    self.preview.lockUntil = 0;
}
- (void)motionToggle:(NSString *)row id:(int)i {
    int st = self.preview.state;
    BOOL rt = [row isEqualToString:@"rt"];
    uint32_t *set = rt ? &MOTION_MAP[st].rt : &MOTION_MAP[st].act, bit = 1u << i;
    if (rt && (*set & bit) && bits(*set) == 1) { // a state needs a routine
        self.noteUntil = [[NSDate date] timeIntervalSince1970] + 3;
        [self setNeedsDisplay:YES];
        __weak SettingsView *me = self;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(3.1 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ [me setNeedsDisplay:YES]; });
        return;
    }
    *set ^= bit;
    if (*set & bit) [self motionPlay:row id:i]; // switched on: shown at once
    else if (rt) [self.preview dropRoutine:i state:st];
    [self.host motionMapChangedForState:st routineOff:rt && !(*set & bit) ? i : -1];
    [self setNeedsDisplay:YES];
}

// ---- the mouse ----
- (Hit *)hitAt:(NSPoint)p { for (Hit *h in self.hits.reverseObjectEnumerator) if (NSPointInRect(p, h.r)) return h; return nil; }
- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    for (NSTrackingArea *t in self.trackingAreas) [self removeTrackingArea:t];
    [self addTrackingArea:[[NSTrackingArea alloc] initWithRect:self.bounds options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways | NSTrackingInVisibleRect owner:self userInfo:nil]];
}
- (void)mouseMoved:(NSEvent *)e {
    Hit *h = [self hitAt:[self convertPoint:e.locationInWindow fromView:nil]];
    if (h == self.hot) return;
    self.hot = h;
    if (h.kind == K_PICK && [h.key isEqualToString:@"frame"]) self.preview.lockUntil = 1e18; // the frame shows while a frame chip is pointed at
    else if (self.preview.lockUntil > 1e17) self.preview.lockUntil = 0;
    [self setNeedsDisplay:YES];
}
- (void)mouseExited:(NSEvent *)e { (void)e; self.hot = nil; if (self.preview.lockUntil > 1e17) self.preview.lockUntil = 0; [self setNeedsDisplay:YES]; }

- (void)choose:(NSString *)key value:(id)v { [self.host chooseSetting:key value:v]; [self setNeedsDisplay:YES]; }
- (void)slide:(Hit *)h at:(CGFloat)x {
    CGFloat t = (x - h.t0) / fmax(1, h.t1 - h.t0);
    int i = (int)lround(fmax(0, fmin(1, t)) * (h.n - 1));
    if (i != [self current:h.key]) [self choose:h.key value:choices()[h.key][i]];
}
- (void)mouseDown:(NSEvent *)e {
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    Hit *h = [self hitAt:p];
    if (!h) { [self.window performWindowDragWithEvent:e]; return; } // the background moves the window
    switch (h.kind) {
    case K_CLOSE: self.closeArmed = YES; return;
    case K_SLIDE: self.sliding = h; [self slide:h at:p.x]; return;
    case K_PICK: {
        NSString *key = h.key;
        if (([key isEqualToString:@"tipColor"] || [key isEqualToString:@"textColor"]) && h.val == 5) { [self pickColour:key]; return; }
        if ([key isEqualToString:@"frame"]) self.preview.lockUntil = [[NSDate date] timeIntervalSince1970] + 2.5;
        [self choose:key value:choices()[key][h.val]];
        return;
    }
    case K_TOGGLE: [self choose:h.key value:@(![[self value:h.key] boolValue])]; return;
    case K_CYCLE: { int i = [self current:h.key]; [self choose:h.key value:choices()[h.key][(i + 1) % 4]]; return; }
    case K_STATE: self.preview.state = PV_STATES[h.val]; [self setNeedsDisplay:YES]; return;
    case K_RESET: [self.host resetSettings]; [self setNeedsDisplay:YES]; return;
    case K_TAB: self.tab = h.val; self.hot = nil; [self setNeedsDisplay:YES]; return;
    case K_MSTATE: self.preview.state = h.val; [self setNeedsDisplay:YES]; return;
    case K_MTOG: [self motionToggle:h.key id:h.val]; return;
    case K_MPLAY: [self motionPlay:h.key id:h.val]; return;
    }
}
- (void)mouseDragged:(NSEvent *)e { if (self.sliding) [self slide:self.sliding at:[self convertPoint:e.locationInWindow fromView:nil].x]; }
- (void)mouseUp:(NSEvent *)e {
    self.sliding = nil;
    if (self.closeArmed && [self hitAt:[self convertPoint:e.locationInWindow fromView:nil]].kind == K_CLOSE) [self.owner close];
    self.closeArmed = NO;
}
- (void)keyDown:(NSEvent *)e { if (e.keyCode == 53) [self.owner close]; else [super keyDown:e]; } // Esc

// the palette swatch: the system colour panel, on the colour in use; every change applies at once
- (void)pickColour:(NSString *)row {
    self.picking = [row isEqualToString:@"tipColor"] ? @"tipCustom" : @"textCustom";
    NSColorPanel *cp = [NSColorPanel sharedColorPanel];
    cp.showsAlpha = NO;
    [cp setTarget:nil]; [cp setAction:NULL];
    cp.color = hex_color([[self value:self.picking] unsignedIntValue]);
    [cp setTarget:self];
    [cp setAction:@selector(colourChanged:)];
    cp.level = self.window.level + 1;
    [self choose:row value:@5];
    [NSApp activateIgnoringOtherApps:YES];
    [cp orderFront:nil];
}
- (void)colourChanged:(NSColorPanel *)cp {
    if (!self.picking) return;
    [self choose:self.picking value:@(color_hex(cp.color))];
}
@end

// ---- the window ------------------------------------------------------------------------------------------------
@implementation SettingsWindow

- (instancetype)initWithHost:(id<SettingsHost>)host {
    if ((self = [super init])) {
        _host = host;
        if (!SHAPE_NAMES) {
            SHAPE_NAMES = @[@"Dodecahedron", @"Buckyball", @"Geodesic", @"Fine geodesic"];
            FRAME_NAMES = @[@"Segments", @"Lens", @"Polar", @"Diamond", @"Lock", @"Orbits", @"None"];
            WHEEL_NAMES = @[@"Roll", @"Roll back", @"Turn", @"Turn back", @"Off"];
            TIP_BG_NAMES = @[@"Ink", @"Graphite", @"Navy", @"Plum", @"Pine", @"Custom"];
            TEXT_NAMES = @[@"Auto", @"White", @"Cream", @"Mint", @"Sky", @"Custom"];
            AWAY_TEXT = @[@"1 minute", @"2 minutes", @"5 minutes", @"never"];
            PV_NAMES = @[@"Idle", @"Thinking", @"Working", @"Needs you", @"Done", @"Asleep"];
        }
    }
    return self;
}

- (void)build {
    SettingsPanel *p = [[SettingsPanel alloc] initWithContentRect:NSMakeRect(0, 0, WW, WH)
                                                        styleMask:(NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel)
                                                          backing:NSBackingStoreBuffered defer:NO];
    p.opaque = NO;
    p.backgroundColor = [NSColor clearColor];
    p.hasShadow = YES;
    p.level = NSFloatingWindowLevel;
    p.hidesOnDeactivate = NO;
    p.releasedWhenClosed = NO;
    p.becomesKeyOnlyIfNeeded = NO;
    SettingsView *v = [[SettingsView alloc] initWithFrame:NSMakeRect(0, 0, WW, WH)];
    v.host = self.host;
    v.owner = self;
    PreviewView *pv = [[PreviewView alloc] initWithFrame:NSMakeRect(40, 62, 524, 222)]; // in the flipped view: the band's left part
    pv.host = self.host;
    [v addSubview:pv];
    v.preview = pv;
    [pv tick:[[NSDate date] timeIntervalSince1970]];
    p.contentView = v;
    self.view = v;
    self.panel = p;
}

- (BOOL)visible { return self.panel.visible; }

- (void)showOn:(NSScreen *)screen behavior:(NSWindowCollectionBehavior)behavior {
    if (!self.panel) [self build];
    self.panel.collectionBehavior = behavior;
    NSRect vis = (screen ?: [NSScreen mainScreen]).visibleFrame;
    if (!self.panel.visible) [self.panel setFrameOrigin:NSMakePoint(NSMidX(vis) - WW / 2, NSMidY(vis) - WH / 2)];
    [self.view setNeedsDisplay:YES];
    [self.panel makeKeyAndOrderFront:nil];
    [self.panel makeFirstResponder:self.view];
    if (!self.timer) {
        __weak SettingsWindow *me = self;
        self.timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 30 repeats:YES block:^(NSTimer *t) {
            (void)t;
            [me.view.preview tick:[[NSDate date] timeIntervalSince1970]];
        }];
        [[NSRunLoop currentRunLoop] addTimer:self.timer forMode:NSEventTrackingRunLoopMode]; // keeps moving while a slider is dragged
    }
}

- (void)close {
    [self.timer invalidate];
    self.timer = nil;
    [self.panel orderOut:nil];
    if ([NSColorPanel sharedColorPanelExists]) [[NSColorPanel sharedColorPanel] orderOut:nil];
    self.view.picking = nil;
}

- (void)changed { [self.view setNeedsDisplay:YES]; }
- (void)showPage:(int)page { if (!self.panel) [self build]; self.view.tab = page; [self.view setNeedsDisplay:YES]; }

- (BOOL)saveShot:(NSString *)path {
    if (!self.panel) return NO;
    NSView *v = self.view;
    NSBitmapImageRep *rep = [v bitmapImageRepForCachingDisplayInRect:v.bounds];
    [v cacheDisplayInRect:v.bounds toBitmapImageRep:rep];
    return [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:path atomically:YES];
}
@end

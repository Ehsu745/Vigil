// READY's layer and hearts on the Mac (see ready.h). Ported from layer_draw and hearts_draw in win/panel.c.
#import "ready.h"
#import <QuartzCore/QuartzCore.h>

static NSPanel *overlay_panel(void) {
    NSPanel *w = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 10, 10)
                                            styleMask:(NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel)
                                              backing:NSBackingStoreBuffered defer:NO];
    w.opaque = NO;
    w.backgroundColor = [NSColor clearColor];
    w.hasShadow = NO;
    w.ignoresMouseEvents = YES; // never in the way: clicks go through to whatever is underneath
    w.hidesOnDeactivate = NO;
    w.releasedWhenClosed = NO;
    return w;
}

static NSScreen *screen_at(NSPoint p) {
    for (NSScreen *sc in [NSScreen screens]) if (NSPointInRect(p, sc.frame)) return sc;
    return nil;
}

@implementation ReadyLayer {
    NSPanel *_w;
    CALayer *_pic;   // the drawn box of the canvas, laid where it belongs in the window
    Canvas _cv;
    uint8_t *_rgba;
    size_t _cap;
    NSRect _keep;    // while pieces are out the window only grows (a new size is a new canvas)
    BOOL _shown;
}
- (NSRect)frame { return _shown ? _w.frame : NSZeroRect; }
- (void)hide {
    if (_shown) [_w orderOut:nil];
    _shown = NO;
    _keep = NSZeroRect;
}

// a point on the stage (shell radii from the core's centre, y down), m round it, in screen points
static NSRect grow(NSRect b, NSPoint c, float u, float x, float y, float m) {
    return NSUnionRect(b, NSMakeRect(c.x + x * u - m, c.y - y * u - m, 2 * m, 2 * m));
}

- (void)draw:(const Session *)s core:(NSRect)core pixels:(int)px shape:(const Shape *)sh glow:(float)glow coreFrac:(float)cf now:(double)now {
    NSPoint c = NSMakePoint(NSMidX(core), NSMidY(core));
    NSScreen *sc = screen_at(c) ?: [NSScreen mainScreen];
    if (!s->mo.loose_on || !sc || px < 8) { [self hide]; return; }
    float u = (float)core.size.width * SHELL_FRAC, m = 1.8f * u, mf = 4.2f * u; // (a blow's flash reaches about 3.5 shell radii)
    const ReadyRS *R = &s->mo.rs.ready;
    NSRect b = core;
    if (s->mo.rt == RT_RESTLESS) {
        for (int k = 0; k < R->n; k++) if (R->p[k].st != RP_HOME) b = grow(b, c, u, R->p[k].pos[0], R->p[k].pos[1], m);
        for (int k = 0; k < R->nsh; k++) if (!R->sh[k].home) b = grow(b, c, u, R->sh[k].base[0], R->sh[k].base[1], m); // the shell's shards, blown out
    }
    for (int k = 0; k < 4; k++) if (now - s->mo.imp[k].at < .6) b = grow(b, c, u, s->mo.imp[k].x, s->mo.imp[k].y, mf); // a blow's burst at the pointer
    if (R->nsh) b = grow(b, c, u, 0, 0, mf); // a crash into the core: its flash
    if (s->aim_on) b = grow(b, c, u, s->aim_x, s->aim_y, m);
    NSRect mon = NSUnionRect(sc.frame, core); // (the core itself may hang over the edge)
    if (s->aim_on) { // the pointer's screen too: what is thrown follows it across
        NSScreen *ps = screen_at(NSMakePoint(c.x + s->aim_x * u, c.y - s->aim_y * u));
        if (ps) mon = NSUnionRect(mon, ps.frame);
    }
    b = NSIntersectionRect(b, mon);
    // it only grows while pieces are out, in steps of 128 points: pieces scattering after a hit would otherwise
    // change its size (a new canvas) every frame
    _keep = !_shown || NSIsEmptyRect(_keep) ? b : NSUnionRect(_keep, b);
    {
        NSRect q = _keep;
        CGFloat gw = ceil(q.size.width / 128) * 128, gh = ceil(q.size.height / 128) * 128;
        q = NSMakeRect(floor(q.origin.x - (gw - q.size.width) / 2), floor(q.origin.y - (gh - q.size.height) / 2), gw, gh);
        if (q.origin.x < NSMinX(mon)) q.origin.x = NSMinX(mon);
        if (NSMaxX(q) > NSMaxX(mon)) q.origin.x = NSMaxX(mon) - q.size.width;
        if (q.origin.y < NSMinY(mon)) q.origin.y = NSMinY(mon);
        if (NSMaxY(q) > NSMaxY(mon)) q.origin.y = NSMaxY(mon) - q.size.height;
        b = _keep = NSIntegralRect(NSIntersectionRect(q, mon));
    }
    CGFloat scale = sc.backingScaleFactor;
    int W = (int)lround(b.size.width * scale), H = (int)lround(b.size.height * scale);
    if (W < 8 || H < 8) { [self hide]; return; }
    canvas_resize(&_cv, W, H);
    // the core's square in the canvas (y down), on the same part-pixel place as its picture
    double x = (c.x - b.origin.x) * scale - px / 2.0, y = (NSMaxY(b) - c.y) * scale - px / 2.0;
    int x0 = (int)floor(x), y0 = (int)floor(y);
    _cv.sx = (float)(x - x0); _cv.sy = (float)(y - y0);
    RENDER_PART = 2;
    render_part(&_cv, s, sh, now, glow, cf, x0, y0, x0 + px, y0 + px); // (clears what it drew last time first)
    RENDER_PART = 0;
    _cv.sx = _cv.sy = 0;

    if (!_w) {
        _w = overlay_panel();
        NSView *v = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 10, 10)];
        v.wantsLayer = YES;
        _pic = [CALayer layer];
        _pic.magnificationFilter = kCAFilterNearest;
        [v.layer addSublayer:_pic];
        _w.contentView = v;
    }
    _w.level = self.level;
    _w.collectionBehavior = self.behavior;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    int bx0 = _cv.bx0, by0 = _cv.by0, bx1 = _cv.bx1, by1 = _cv.by1, bw = bx1 - bx0, bh = by1 - by0;
    if (bw <= 0 || bh <= 0) _pic.contents = nil;
    else { // only the drawn box is converted and handed over (the pieces cover a sliver of the window)
        size_t need = (size_t)W * H * 4;
        if (need > _cap) { free(_rgba); _rgba = malloc(need); _cap = need; }
        canvas_rgba_box(&_cv, _rgba, bx0, by0, bx1, by1);
        size_t row = (size_t)bw * 4;
        uint8_t *buf = malloc(row * bh);
        for (int r = 0; r < bh; r++) memcpy(buf + row * r, _rgba + ((size_t)(by0 + r) * W + bx0) * 4, row);
        CFDataRef data = CFDataCreateWithBytesNoCopy(NULL, buf, (CFIndex)(row * bh), kCFAllocatorMalloc);
        CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
        CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGImageRef img = CGImageCreate((size_t)bw, (size_t)bh, 8, 32, row, space, (CGBitmapInfo)kCGImageAlphaPremultipliedLast,
                                       provider, NULL, false, kCGRenderingIntentDefault);
        CGColorSpaceRelease(space);
        CGDataProviderRelease(provider);
        CFRelease(data);
        _pic.contents = (__bridge id)img;
        CGImageRelease(img);
        _pic.contentsScale = scale;
        _pic.frame = CGRectMake(bx0 / scale, (H - by1) / scale, bw / scale, bh / scale); // (the layer's y runs up)
    }
    [CATransaction commit];
    if (!NSEqualRects(_w.frame, b)) [_w setFrame:b display:NO];
    if (!_shown) { [_w orderFrontRegardless]; _shown = YES; }
}
- (void)dealloc { free(_rgba); free(_cv.p); free(_cv.dk); }
@end

// ---- the hearts ---------------------------------------------------------------------------------------------
@interface HeartsView : NSView
@property int hearts, on;
@property double gameAt, heartAt, gameEnd, now;
@end
@implementation HeartsView
- (BOOL)isOpaque { return NO; }
- (BOOL)isFlipped { return YES; }
static void heart(CGContextRef ctx, CGFloat cx, CGFloat cy, CGFloat sz, CGFloat r, CGFloat g, CGFloat b, CGFloat a) {
    if (a <= 0.002 || sz <= 0.5) return;
    CGContextSetRGBFillColor(ctx, r, g, b, a);
    CGFloat rad = sz * 0.29; // two discs and a triangle, as on Windows, but as one path so the seams never show
    CGMutablePathRef p = CGPathCreateMutable();
    CGPathAddEllipseInRect(p, NULL, CGRectMake(cx - sz * 0.25 - rad, cy - sz * 0.12 - rad, 2 * rad, 2 * rad));
    CGPathAddEllipseInRect(p, NULL, CGRectMake(cx + sz * 0.25 - rad, cy - sz * 0.12 - rad, 2 * rad, 2 * rad));
    CGPathMoveToPoint(p, NULL, cx - sz * 0.53, cy - sz * 0.02);
    CGPathAddLineToPoint(p, NULL, cx + sz * 0.53, cy - sz * 0.02);
    CGPathAddLineToPoint(p, NULL, cx, cy + sz * 0.55);
    CGPathCloseSubpath(p);
    CGContextAddPath(ctx, p);
    CGContextFillPath(ctx); // (nonzero winding: the union)
    CGPathRelease(p);
}
static CGFloat clamp01(CGFloat v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    CGContextRef ctx = [NSGraphicsContext currentContext].CGContext;
    CGFloat sz = 22, gap = 32, W = self.bounds.size.width, now = self.now;
    CGFloat out = self.on ? 1 : clamp01(1 - (now - self.gameEnd) / 1.2); // (the run over: they fade)
    for (int k = 0; k < GAME_HEARTS; k++) {
        CGFloat in = clamp01((now - self.gameAt - k * 0.06) / 0.25), pop = in * in * (3 - 2 * in) * (1 + 0.25 * sin(M_PI * in));
        CGFloat cx = W * 0.5, cy = 12 + gap * (k + 0.5);
        BOOL full = k < self.hearts, lost = k == self.hearts && self.heartAt > self.gameAt && now - self.heartAt < 0.6;
        if (full) {
            heart(ctx, cx, cy, sz * 1.25 * pop, 1, 0.30, 0.42, 0.22 * in * out);
            heart(ctx, cx, cy, sz * pop, 1, 0.30, 0.42, in * out);
            heart(ctx, cx - sz * 0.18, cy - sz * 0.2, sz * 0.22 * pop, 1, 1, 1, 0.55 * in * out);
        } else {
            heart(ctx, cx, cy, sz * pop, 0.17, 0.17, 0.21, in * out);
            if (lost) { CGFloat u = (now - self.heartAt) / 0.6; heart(ctx, cx, cy, sz * (1 + 0.8 * u), 1, 0.30, 0.42, (1 - u) * (1 - u) * out); } // (just lost: it pops)
        }
    }
}
@end

@implementation ReadyHearts {
    NSPanel *_w;
    HeartsView *_v;
}
- (void)hide { if (_w.visible) [_w orderOut:nil]; }
- (void)show:(const Session *)s screen:(NSScreen *)screen now:(double)now {
    if (!screen) { [self hide]; return; }
    if (!_w) {
        _w = overlay_panel();
        _v = [[HeartsView alloc] initWithFrame:NSMakeRect(0, 0, 48, 10)];
        _w.contentView = _v;
    }
    _w.level = self.level;
    _w.collectionBehavior = self.behavior;
    _v.hearts = s->hearts; _v.on = s->game_on; _v.gameAt = s->game_at; _v.heartAt = s->heart_at; _v.gameEnd = s->game_end; _v.now = now;
    NSRect wa = screen.visibleFrame;
    CGFloat w = 48, h = 32 * GAME_HEARTS + 24;
    NSRect f = NSMakeRect(NSMinX(wa) + 10, floor(NSMidY(wa) - h / 2), w, h);
    if (!NSEqualRects(_w.frame, f)) [_w setFrame:f display:NO];
    [_v setNeedsDisplay:YES];
    if (!_w.visible) [_w orderFrontRegardless];
}
@end

// The box wraps its text instead of cutting it: it only shows while you point at a core or talk to it, so it
// may stretch over the terminal to the left of the panel. The text is in the system font (Chinese comes out
// in PingFang), the title bold in the session's colour, with a bar of that colour down the left edge.
#import "tip.h"

static const CGFloat PAD = 11, BAR = 3, MAXW = 420, GAP = 6;

@implementation TipView
- (BOOL)isOpaque { return NO; }
- (CGFloat)size { return self.fontSize > 0 ? self.fontSize : 12; }
- (CGFloat)lineH { return ceil([self size] * 1.45); }
- (NSDictionary *)attr:(BOOL)title {
    NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
    para.lineBreakMode = NSLineBreakByWordWrapping;
    para.minimumLineHeight = para.maximumLineHeight = [self lineH];
    NSFont *f = title ? [NSFont systemFontOfSize:[self size] weight:NSFontWeightSemibold] : [NSFont systemFontOfSize:[self size]];
    NSColor *c = title ? [self titleColor] : [NSColor colorWithSRGBRed:0.90 green:0.91 blue:0.94 alpha:1];
    return @{NSFontAttributeName: f, NSForegroundColorAttributeName: c, NSParagraphStyleAttributeName: para};
}
// the session's colour, lifted toward white so it reads on the dark box
- (NSColor *)titleColor {
    NSColor *a = [self.accent colorUsingColorSpace:[NSColorSpace sRGBColorSpace]] ?: [NSColor whiteColor];
    return [a blendedColorWithFraction:0.3 ofColor:[NSColor whiteColor]] ?: a;
}
// the body without blank lines or surrounding spaces
- (NSString *)text {
    NSMutableArray *a = [NSMutableArray array];
    for (NSString *l in [self.body componentsSeparatedByCharactersInSet:[NSCharacterSet newlineCharacterSet]]) {
        NSString *t = [l stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        if (t.length) [a addObject:t];
    }
    return [a componentsJoinedByString:@"\n"];
}
- (CGFloat)heightOf:(NSString *)s title:(BOOL)title width:(CGFloat)w maxLines:(int)n {
    if (!s.length) return 0;
    CGFloat h = ceil([s boundingRectWithSize:NSMakeSize(w, 1e5) options:NSStringDrawingUsesLineFragmentOrigin attributes:[self attr:title]].size.height);
    return n > 0 ? fmin(h, n * [self lineH]) : h;
}
- (NSSize)fit {
    CGFloat w = ceil([self.title ?: @"" sizeWithAttributes:[self attr:YES]].width);
    for (NSString *l in [[self text] componentsSeparatedByString:@"\n"]) w = fmax(w, ceil([l sizeWithAttributes:[self attr:NO]].width));
    w = fmin(w + 1, MAXW);
    CGFloat body = [self heightOf:[self text] title:NO width:w maxLines:self.maxBodyLines];
    CGFloat h = [self heightOf:self.title title:YES width:w maxLines:0] + (body > 0 ? GAP + body : 0);
    return NSMakeSize(w + PAD * 2 + BAR, h + PAD * 2);
}
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    NSRect b = NSInsetRect(self.bounds, 0.5, 0.5);
    NSBezierPath *box = [NSBezierPath bezierPathWithRoundedRect:b xRadius:8 yRadius:8];
    [[NSColor colorWithSRGBRed:0.11 green:0.12 blue:0.15 alpha:0.98] setFill];
    [box fill];
    [NSGraphicsContext saveGraphicsState]; // the colour bar down the left edge, clipped to the rounded box
    [box addClip];
    [[self titleColor] setFill];
    NSRectFill(NSMakeRect(0, 0, BAR, self.bounds.size.height));
    [NSGraphicsContext restoreGraphicsState];
    [[self.accent colorWithAlphaComponent:0.45] setStroke];
    [box stroke];
    CGFloat x = PAD + BAR, w = self.bounds.size.width - PAD * 2 - BAR, top = self.bounds.size.height - PAD;
    CGFloat th = [self heightOf:self.title title:YES width:w maxLines:0];
    NSStringDrawingOptions opt = NSStringDrawingUsesLineFragmentOrigin | NSStringDrawingTruncatesLastVisibleLine;
    [self.title ?: @"" drawWithRect:NSMakeRect(x, top - th, w, th) options:opt attributes:[self attr:YES] context:nil];
    NSString *text = [self text];
    CGFloat bh = [self heightOf:text title:NO width:w maxLines:self.maxBodyLines];
    if (bh > 0) [text drawWithRect:NSMakeRect(x, top - th - GAP - bh, w, bh) options:opt attributes:[self attr:NO] context:nil];
}
@end

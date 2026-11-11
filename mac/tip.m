// The box wraps its text instead of cutting it: it only shows while you point at a core or talk to it, so it
// may stretch over the terminal to the left of the panel. The text is in the system font (Chinese comes out
// in PingFang), the title bold in the session's colour, with a bar of that colour down the left edge.
#import "tip.h"

static const CGFloat PAD = 11, BAR = 3, MAXW = 460, GAP = 6;

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
// ---- the body, laid out like Claude Code's terminal: paragraphs with a gap between them, "- " items as
// bullets with wrapped lines indented under the text, **bold**, `code` in a monospaced font, code blocks
// indented and dimmed. (A small Markdown subset: what replies are made of.)
- (NSColor *)textColor { return [NSColor colorWithSRGBRed:0.90 green:0.91 blue:0.94 alpha:1]; }
- (NSColor *)codeColor { return [NSColor colorWithSRGBRed:0.68 green:0.70 blue:1.00 alpha:1]; }
- (NSFont *)mono { return [NSFont monospacedSystemFontOfSize:[self size] * 0.95 weight:NSFontWeightRegular]; }

static NSRegularExpression *re(NSString *p) { return [NSRegularExpression regularExpressionWithPattern:p options:0 error:NULL]; }

// one line of text with **bold**, `code` and [links](...) turned into styles
- (NSAttributedString *)inline:(NSString *)line font:(NSFont *)font color:(NSColor *)color para:(NSParagraphStyle *)para {
    NSMutableAttributedString *out = [[NSMutableAttributedString alloc] init];
    NSDictionary *plain = @{NSFontAttributeName: font, NSForegroundColorAttributeName: color, NSParagraphStyleAttributeName: para};
    static NSRegularExpression *tok;
    if (!tok) tok = re(@"\\*\\*(.+?)\\*\\*|`([^`]+)`|\\[([^\\]]+)\\]\\([^)]*\\)");
    NSUInteger at = 0;
    for (NSTextCheckingResult *m in [tok matchesInString:line options:0 range:NSMakeRange(0, line.length)]) {
        if (m.range.location > at) [out appendAttributedString:[[NSAttributedString alloc] initWithString:[line substringWithRange:NSMakeRange(at, m.range.location - at)] attributes:plain]];
        NSMutableDictionary *st = [plain mutableCopy];
        NSRange g;
        if ([m rangeAtIndex:1].location != NSNotFound) { g = [m rangeAtIndex:1]; st[NSFontAttributeName] = [NSFont systemFontOfSize:font.pointSize weight:NSFontWeightBold]; st[NSForegroundColorAttributeName] = [NSColor whiteColor]; }
        else if ([m rangeAtIndex:2].location != NSNotFound) { g = [m rangeAtIndex:2]; st[NSFontAttributeName] = [self mono]; st[NSForegroundColorAttributeName] = [self codeColor]; }
        else { g = [m rangeAtIndex:3]; st[NSUnderlineStyleAttributeName] = @(NSUnderlineStyleSingle); }
        [out appendAttributedString:[[NSAttributedString alloc] initWithString:[line substringWithRange:g] attributes:st]];
        at = NSMaxRange(m.range);
    }
    if (at < line.length) [out appendAttributedString:[[NSAttributedString alloc] initWithString:[line substringFromIndex:at] attributes:plain]];
    return out;
}

- (NSMutableParagraphStyle *)paraIndent:(CGFloat)first rest:(CGFloat)rest gap:(BOOL)gap {
    NSMutableParagraphStyle *p = [[NSMutableParagraphStyle alloc] init];
    p.lineBreakMode = NSLineBreakByWordWrapping;
    p.minimumLineHeight = p.maximumLineHeight = [self lineH];
    p.firstLineHeadIndent = first;
    p.headIndent = rest;
    p.paragraphSpacingBefore = gap ? round([self lineH] * 0.55) : 0;
    return p;
}

- (NSAttributedString *)rendered {
    NSMutableAttributedString *out = [[NSMutableAttributedString alloc] init];
    NSFont *font = [NSFont systemFontOfSize:[self size]];
    static NSRegularExpression *item, *heading;
    if (!item) { item = re(@"^(\\s*)([-*+•]|\\d+[.)])\\s+(.*)$"); heading = re(@"^#{1,6}\\s+(.*)$"); }
    BOOL code = NO, gap = NO, first = YES;
    CGFloat step = ceil([@"• " sizeWithAttributes:@{NSFontAttributeName: font}].width);
    for (NSString *raw in [self.body ?: @"" componentsSeparatedByString:@"\n"]) {
        NSString *line = [raw stringByTrimmingCharactersInSet:[NSCharacterSet characterSetWithCharactersInString:@"\r"]];
        NSString *t = [line stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        if ([t hasPrefix:@"```"] || [t hasPrefix:@"~~~"]) { code = !code; gap = gap || code; continue; }
        if (!code && !t.length) { gap = !first; continue; }
        NSAttributedString *piece;
        if (code) {
            piece = [[NSAttributedString alloc] initWithString:line attributes:@{NSFontAttributeName: [self mono],
                NSForegroundColorAttributeName: [NSColor colorWithSRGBRed:0.62 green:0.66 blue:0.72 alpha:1],
                NSParagraphStyleAttributeName: [self paraIndent:step * 1.5 rest:step * 1.5 gap:gap && !first]}];
        } else {
            NSTextCheckingResult *m = [item firstMatchInString:line options:0 range:NSMakeRange(0, line.length)];
            NSTextCheckingResult *h = [heading firstMatchInString:t options:0 range:NSMakeRange(0, t.length)];
            if (m) { // "- text": the marker hangs, wrapped lines line up with the text after it
                CGFloat level = floor([m rangeAtIndex:1].length / 2.0) * step * 1.2; // two spaces of indent = one level
                NSString *marker = [line substringWithRange:[m rangeAtIndex:2]];
                if ([@"-*+" containsString:marker]) marker = @"•";
                marker = [marker stringByAppendingString:@" "];
                CGFloat mw = ceil([marker sizeWithAttributes:@{NSFontAttributeName: font}].width);
                NSMutableParagraphStyle *ps = [self paraIndent:level rest:level + mw gap:gap && !first];
                NSMutableAttributedString *a = [[NSMutableAttributedString alloc] initWithString:marker
                    attributes:@{NSFontAttributeName: font, NSForegroundColorAttributeName: [self titleColor], NSParagraphStyleAttributeName: ps}];
                [a appendAttributedString:[self inline:[line substringWithRange:[m rangeAtIndex:3]] font:font color:[self textColor] para:ps]];
                piece = a;
            } else if (h) {
                piece = [self inline:[t substringWithRange:[h rangeAtIndex:1]] font:[NSFont systemFontOfSize:[self size] weight:NSFontWeightBold]
                               color:[NSColor whiteColor] para:[self paraIndent:0 rest:0 gap:!first]];
            } else {
                piece = [self inline:t font:font color:[self textColor] para:[self paraIndent:0 rest:0 gap:gap && !first]];
            }
        }
        if (!first) [out appendAttributedString:[[NSAttributedString alloc] initWithString:@"\n"]];
        [out appendAttributedString:piece];
        first = NO;
        gap = NO;
    }
    return out;
}

- (CGFloat)titleHeight:(CGFloat)w {
    if (!self.title.length) return 0;
    return ceil([self.title boundingRectWithSize:NSMakeSize(w, 1e5) options:NSStringDrawingUsesLineFragmentOrigin attributes:[self attr:YES]].size.height);
}
- (CGFloat)bodyHeight:(NSAttributedString *)body width:(CGFloat)w {
    if (!body.length) return 0;
    CGFloat h = ceil([body boundingRectWithSize:NSMakeSize(w, 1e5) options:NSStringDrawingUsesLineFragmentOrigin].size.height);
    return self.maxBodyLines > 0 ? fmin(h, self.maxBodyLines * [self lineH]) : h;
}

- (NSSize)fit {
    NSAttributedString *body = [self rendered];
    CGFloat w = ceil([self.title ?: @"" sizeWithAttributes:[self attr:YES]].width);
    w = fmax(w, ceil([body boundingRectWithSize:NSMakeSize(MAXW, 1e5) options:NSStringDrawingUsesLineFragmentOrigin].size.width));
    w = fmin(w + 2, MAXW);
    CGFloat bh = [self bodyHeight:body width:w];
    CGFloat h = [self titleHeight:w] + (bh > 0 ? GAP + bh : 0);
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
    CGFloat th = [self titleHeight:w];
    NSStringDrawingOptions opt = NSStringDrawingUsesLineFragmentOrigin | NSStringDrawingTruncatesLastVisibleLine;
    [self.title ?: @"" drawWithRect:NSMakeRect(x, top - th, w, th) options:opt attributes:[self attr:YES] context:nil];
    NSAttributedString *body = [self rendered];
    CGFloat bh = [self bodyHeight:body width:w];
    if (bh > 0) [body drawWithRect:NSMakeRect(x, top - th - GAP - bh, w, bh) options:opt context:nil];
}
@end

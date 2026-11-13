// The reply box: a title line, a field of three lines in the terminal's face, a hint line, drawn like the
// tooltip (dark card, the session's colour down the left edge). Enter sends, Shift+Enter starts a new line,
// Esc or the × closes it and keeps what was typed for next time. It stays open when another app takes the
// keyboard, so an image can be picked up in Finder (or the screenshot thumbnail) and dragged onto it. Images
// dropped on the box (or on the core, see panel.m) or pasted each put an "[Image #N]" token at the cursor, as
// Claude Code shows them; the token moves and deletes like text (Backspace takes it whole), and on send it
// becomes the image's path. Promised files and image data are saved in a private temporary folder first.
// The window is a non-activating panel that may become key: it takes the keyboard without making Vigil the
// active app, so iTerm2 stays in front.
#import "reply.h"
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

static const CGFloat PAD = 12, BAR = 3, GAP = 6, MAX_ATTACH = 8;

@interface ReplyPanel : NSPanel
@end
@implementation ReplyPanel
- (BOOL)canBecomeKeyWindow { return YES; }
- (BOOL)canBecomeMainWindow { return NO; }
@end

@interface ReplyTextView : NSTextView
@property(weak) ReplyBox *box;
@end

@interface ReplyCard : NSView
@property(weak) ReplyBox *box;
@end

@interface ReplyBox () <NSTextViewDelegate, NSWindowDelegate>
@property(strong) ReplyPanel *panel;
@property(strong) ReplyCard *card;
@property(strong) NSScrollView *scroll;
@property(strong) ReplyTextView *text;
@property(strong) NSButton *closeButton;
@property(copy) NSString *name;
@property(readwrite, copy) NSString *sid;
@property(strong) NSMutableDictionary<NSString *, NSString *> *drafts;
@property(strong) NSMutableDictionary<NSString *, NSMutableArray<NSDictionary *> *> *attach; // sid -> @{id, path, name}
@property(strong) NSMutableDictionary<NSString *, NSNumber *> *nextId;                      // sid -> last N given out
- (NSString *)hint;
- (NSColor *)titleColor;
- (NSFont *)mono;
- (CGFloat)lineH;
- (NSRect)fieldRect;
@end

static NSRegularExpression *token_re(void) {
    static NSRegularExpression *re;
    if (!re) re = [NSRegularExpression regularExpressionWithPattern:@"\\[Image #(\\d+)\\]" options:0 error:NULL];
    return re;
}

// a path as a terminal drop writes it: spaces and shell characters escaped
static NSString *escaped_path(NSString *p) {
    NSMutableString *o = [NSMutableString string];
    NSCharacterSet *special = [NSCharacterSet characterSetWithCharactersInString:@" '\"()&;$`!\\"];
    for (NSUInteger i = 0; i < p.length; i++) {
        unichar c = [p characterAtIndex:i];
        if ([special characterIsMember:c]) [o appendString:@"\\"];
        [o appendFormat:@"%C", c];
    }
    return o;
}

@implementation ReplyTextView
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)info {
    return [ReplyBox hasImage:info.draggingPasteboard] ? NSDragOperationCopy : [super draggingEntered:info];
}
- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)info {
    return [ReplyBox hasImage:info.draggingPasteboard] ? NSDragOperationCopy : [super draggingUpdated:info];
}
- (BOOL)performDragOperation:(id<NSDraggingInfo>)info {
    if (self.box.sid && [ReplyBox hasImage:info.draggingPasteboard]) return [self.box takeImagesFrom:info.draggingPasteboard forSid:self.box.sid];
    return [super performDragOperation:info];
}
// ⌘V: image files or image data (with no text) come in as tokens, like Claude Code's own paste
- (void)paste:(id)sender {
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    BOOL files = [pb canReadObjectForClasses:@[[NSURL class]] options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
    BOOL text = [pb availableTypeFromArray:@[NSPasteboardTypeString]] != nil;
    if (self.box.sid && (files || !text) && [ReplyBox hasImage:pb] && [self.box takeImagesFrom:pb forSid:self.box.sid]) return;
    [super paste:sender];
}
@end

@implementation ReplyCard
- (BOOL)isOpaque { return NO; }
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    ReplyBox *b = self.box;
    NSRect r = NSInsetRect(self.bounds, 0.5, 0.5);
    NSBezierPath *box = [NSBezierPath bezierPathWithRoundedRect:r xRadius:10 yRadius:10];
    [[NSColor colorWithSRGBRed:0.11 green:0.12 blue:0.15 alpha:0.98] setFill];
    [box fill];
    [NSGraphicsContext saveGraphicsState];
    [box addClip];
    [[b titleColor] setFill];
    NSRectFill(NSMakeRect(0, 0, BAR, self.bounds.size.height));
    [NSGraphicsContext restoreGraphicsState];
    [[b.accent colorWithAlphaComponent:0.45] setStroke];
    [box stroke];

    CGFloat x0 = BAR + PAD, w = self.bounds.size.width - x0 - PAD, lh = [b lineH];
    NSRect field = [b fieldRect];
    NSBezierPath *f = [NSBezierPath bezierPathWithRoundedRect:field xRadius:8 yRadius:8];
    [[NSColor colorWithSRGBRed:0.16 green:0.17 blue:0.21 alpha:1] setFill];
    [f fill];
    [[NSColor colorWithSRGBRed:58 / 255.0 green:61 / 255.0 blue:72 / 255.0 alpha:1] setStroke];
    [f stroke];

    NSMutableParagraphStyle *clip = [[NSMutableParagraphStyle alloc] init];
    clip.lineBreakMode = NSLineBreakByTruncatingTail;
    NSString *title = [NSString stringWithFormat:@"Reply to %@", b.name.length ? b.name : @"claude"];
    NSDictionary *ta = @{NSFontAttributeName: [NSFont systemFontOfSize:b.fontSize weight:NSFontWeightSemibold],
                         NSForegroundColorAttributeName: [b titleColor], NSParagraphStyleAttributeName: clip};
    [title drawInRect:NSMakeRect(x0, self.bounds.size.height - PAD - lh, w - 26, lh) withAttributes:ta];

    NSString *hint = [b hint];
    BOOL images = [hint hasPrefix:@"[Image"];
    CGFloat hs = round(b.fontSize * 0.86);
    NSColor *hc = images ? [NSColor colorWithSRGBRed:190 / 255.0 green:200 / 255.0 blue:225 / 255.0 alpha:1]
                         : [NSColor colorWithSRGBRed:120 / 255.0 green:126 / 255.0 blue:142 / 255.0 alpha:1];
    NSDictionary *ha = @{NSFontAttributeName: [NSFont systemFontOfSize:hs], NSForegroundColorAttributeName: hc, NSParagraphStyleAttributeName: clip};
    [hint drawInRect:NSMakeRect(x0 + 2, PAD, w - 2, ceil(hs * 1.45)) withAttributes:ha];
}
@end

@implementation ReplyBox

- (instancetype)init {
    if ((self = [super init])) {
        _drafts = [NSMutableDictionary dictionary];
        _attach = [NSMutableDictionary dictionary];
        _nextId = [NSMutableDictionary dictionary];
        _fontSize = 12;
        _fontName = @"Monaco";
    }
    return self;
}

- (BOOL)open { return self.sid != nil && self.panel.visible; }
- (void)setAccent:(NSColor *)a { if ([a isEqual:_accent]) return; _accent = a; [self.card setNeedsDisplay:YES]; }
- (CGFloat)lineH { return ceil(self.fontSize * 1.45); }
- (CGFloat)hintH { return ceil(round(self.fontSize * 0.86) * 1.45); }
- (CGFloat)fieldH { return [self lineH] * 3 + 12; }
- (CGFloat)height { return PAD + [self lineH] + GAP + [self fieldH] + GAP + [self hintH] + PAD; }
- (NSRect)fieldRect {
    CGFloat x0 = BAR + PAD;
    return NSMakeRect(x0, PAD + [self hintH] + GAP, self.card.bounds.size.width - x0 - PAD, [self fieldH]);
}
- (NSFont *)mono { return [NSFont fontWithName:self.fontName size:self.fontSize] ?: [NSFont monospacedSystemFontOfSize:self.fontSize weight:NSFontWeightRegular]; }
- (NSColor *)titleColor {
    NSColor *a = [self.accent colorUsingColorSpace:[NSColorSpace sRGBColorSpace]] ?: [NSColor whiteColor];
    return [a blendedColorWithFraction:0.3 ofColor:[NSColor whiteColor]] ?: a;
}

- (void)build {
    ReplyPanel *p = [[ReplyPanel alloc] initWithContentRect:NSMakeRect(0, 0, 400, 120)
                                                  styleMask:(NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel)
                                                    backing:NSBackingStoreBuffered defer:NO];
    p.opaque = NO;
    p.backgroundColor = [NSColor clearColor];
    p.hasShadow = YES;
    p.level = NSFloatingWindowLevel;
    p.hidesOnDeactivate = NO;
    p.releasedWhenClosed = NO;
    p.becomesKeyOnlyIfNeeded = NO;
    p.delegate = self;
    self.card = [[ReplyCard alloc] initWithFrame:NSMakeRect(0, 0, 400, 120)];
    self.card.box = self;
    p.contentView = self.card;

    self.scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 100, 40)];
    self.scroll.drawsBackground = NO;
    self.scroll.hasVerticalScroller = YES;
    self.scroll.autohidesScrollers = YES;
    self.scroll.borderType = NSNoBorder;
    ReplyTextView *t = [[ReplyTextView alloc] initWithFrame:NSMakeRect(0, 0, 100, 40)];
    t.box = self;
    t.delegate = self;
    t.richText = NO;
    t.importsGraphics = NO;
    t.allowsUndo = YES;
    t.drawsBackground = NO;
    t.automaticQuoteSubstitutionEnabled = t.automaticDashSubstitutionEnabled = t.automaticTextReplacementEnabled = NO;
    t.continuousSpellCheckingEnabled = t.automaticSpellingCorrectionEnabled = NO;
    t.textColor = [NSColor colorWithSRGBRed:0.90 green:0.91 blue:0.94 alpha:1];
    t.insertionPointColor = [NSColor whiteColor];
    t.textContainerInset = NSMakeSize(0, 2);
    t.verticallyResizable = YES;
    t.horizontallyResizable = NO;
    t.autoresizingMask = NSViewWidthSizable;
    t.textContainer.widthTracksTextView = YES;
    [t registerForDraggedTypes:[ReplyBox dragTypes]];
    self.scroll.documentView = t;
    self.text = t;
    [self.card addSubview:self.scroll];
    NSButton *x = [NSButton buttonWithTitle:@"" target:self action:@selector(close)];
    x.bordered = NO;
    x.attributedTitle = [[NSAttributedString alloc] initWithString:@"✕" attributes:@{
        NSFontAttributeName: [NSFont systemFontOfSize:12 weight:NSFontWeightMedium],
        NSForegroundColorAttributeName: [NSColor colorWithSRGBRed:150 / 255.0 green:156 / 255.0 blue:172 / 255.0 alpha:1]}];
    x.toolTip = @"Close (Esc); what you typed is kept";
    self.closeButton = x;
    [self.card addSubview:x];
    self.panel = p;
}

- (void)layoutField {
    NSRect f = NSInsetRect([self fieldRect], 9, 6);
    self.scroll.frame = f;
    CGFloat lh = [self lineH];
    self.closeButton.frame = NSMakeRect(self.card.bounds.size.width - PAD - 20, self.card.bounds.size.height - PAD - lh, 22, lh);
    NSFont *font = [self mono];
    self.text.font = font;
    self.text.typingAttributes = @{NSFontAttributeName: font, NSForegroundColorAttributeName: self.text.textColor};
}

- (void)openFor:(NSString *)sid name:(NSString *)name accent:(NSColor *)accent {
    if (!sid) return;
    if (!self.panel) [self build];
    if (![sid isEqualToString:self.sid]) {
        if (self.sid) self.drafts[self.sid] = self.text.string;
        self.sid = sid;
        self.text.string = self.drafts[sid] ?: @"";
        [self.text.undoManager removeAllActions];
    }
    self.name = name;
    self.accent = accent;
    self.panel.collectionBehavior = self.behavior;
    [self layoutField];
    [self.card setNeedsDisplay:YES];
    [self.panel makeKeyAndOrderFront:nil];
    [self.panel makeFirstResponder:self.text];
    self.text.selectedRange = NSMakeRange(self.text.string.length, 0);
    [self.text scrollRangeToVisible:self.text.selectedRange];
}

- (void)placeUnder:(NSRect)tip visible:(NSRect)vis {
    if (!self.panel) return;
    CGFloat h = [self height], w = fmax(tip.size.width, 380);
    NSRect f = NSMakeRect(NSMinX(tip), NSMinY(tip) - GAP - h, w, h);
    if (NSMaxX(f) > NSMaxX(vis)) f.origin.x = NSMaxX(vis) - w;
    if (NSMinX(f) < NSMinX(vis)) f.origin.x = NSMinX(vis);
    if (NSMinY(f) < NSMinY(vis)) f.origin.y = NSMinY(vis);
    if (!NSEqualRects(f, self.panel.frame)) {
        [self.panel setFrame:f display:YES];
        [self layoutField];
    }
}

- (void)close {
    if (!self.sid) return;
    self.drafts[self.sid] = self.text.string;
    self.sid = nil;
    [self.panel orderOut:nil];
    [self.delegate replyBoxClosed:self];
}


// ---- keys ---------------------------------------------------------------------------------------------
- (BOOL)textView:(NSTextView *)tv doCommandBySelector:(SEL)cmd {
    if (cmd == @selector(insertNewline:)) {
        if ([NSApp currentEvent].modifierFlags & NSEventModifierFlagShift) { [tv insertNewlineIgnoringFieldEditor:nil]; return YES; }
        [self send];
        return YES;
    }
    if (cmd == @selector(cancelOperation:)) { [self close]; return YES; }
    if (cmd == @selector(deleteBackward:) || cmd == @selector(deleteForward:)) return [self deleteToken:cmd == @selector(deleteBackward:)];
    return NO;
}
- (void)textDidChange:(NSNotification *)n { (void)n; [self.card setNeedsDisplay:YES]; } // the hint lists the images still in the text

// Backspace just after a token, or Delete just before one: the whole token goes, and the space after it
- (BOOL)deleteToken:(BOOL)back {
    NSRange sel = self.text.selectedRange;
    if (sel.length || !self.attach[self.sid].count) return NO;
    NSString *s = self.text.string;
    for (NSTextCheckingResult *m in [token_re() matchesInString:s options:0 range:NSMakeRange(0, s.length)]) {
        NSRange r = m.range;
        if (!((back && NSMaxRange(r) == sel.location) || (!back && r.location == sel.location))) continue;
        if (NSMaxRange(r) < s.length && [s characterAtIndex:NSMaxRange(r)] == ' ') r.length++;
        if ([self.text shouldChangeTextInRange:r replacementString:@""]) {
            [self.text replaceCharactersInRange:r withString:@""];
            [self.text didChangeText];
        }
        return YES;
    }
    return NO;
}

// ---- sending --------------------------------------------------------------------------------------------
- (void)send {
    NSString *sid = self.sid;
    NSString *t = [self.text.string stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    NSArray *images = self.attach[sid];
    self.text.string = @"";
    [self.drafts removeObjectForKey:sid];
    [self close];
    if (!sid || !t.length) return;
    if (images.count) { // each "[Image #N]" becomes its image's path, where it stands
        NSMutableString *o = [NSMutableString string];
        NSUInteger at = 0;
        for (NSTextCheckingResult *m in [token_re() matchesInString:t options:0 range:NSMakeRange(0, t.length)]) {
            NSInteger n = [[t substringWithRange:[m rangeAtIndex:1]] integerValue];
            NSDictionary *img = nil;
            for (NSDictionary *d in images) if ([d[@"id"] integerValue] == n) img = d;
            if (!img) continue;
            [o appendString:[t substringWithRange:NSMakeRange(at, m.range.location - at)]];
            [o appendString:escaped_path(img[@"path"])];
            at = NSMaxRange(m.range);
        }
        [o appendString:[t substringFromIndex:at]];
        t = o;
    }
    [self.attach removeObjectForKey:sid];
    [self.nextId removeObjectForKey:sid];
    [self.delegate replyBox:self send:t to:sid];
}

// ---- images -----------------------------------------------------------------------------------------------
static BOOL is_image_type(NSString *uti) { UTType *t = [UTType typeWithIdentifier:uti]; return t && [t conformsToType:UTTypeImage]; }
static BOOL is_image_file(NSURL *u) { UTType *t = [UTType typeWithFilenameExtension:u.pathExtension]; return t && [t conformsToType:UTTypeImage]; }

// where promised files and pasted image data are kept: a folder of our own, mode 700
static NSURL *images_dir(void) {
    NSURL *d = [NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:@"vigil-images"] isDirectory:YES];
    [[NSFileManager defaultManager] createDirectoryAtURL:d withIntermediateDirectories:YES attributes:@{NSFilePosixPermissions: @0700} error:NULL];
    return d;
}

// a file the system may delete soon, such as the screenshot thumbnail's file in .../TemporaryItems/NSIRD_screencaptureui_*
static BOOL is_temporary(NSURL *u) {
    NSString *p = u.URLByResolvingSymlinksInPath.path;
    NSString *ours = images_dir().URLByResolvingSymlinksInPath.path;
    if ([p hasPrefix:[ours stringByAppendingString:@"/"]]) return NO;
    return [p hasPrefix:@"/private/var/folders/"] || [p hasPrefix:@"/var/folders/"] || [p containsString:@"/TemporaryItems/"];
}

// an image Claude Code reads as it is (png, jpg, gif, webp), else the same picture saved as a PNG next to the others;
// one in a temporary folder is copied next to the others first, so it is still there when Claude Code reads it
static NSURL *readable(NSURL *u) {
    if ([@[@"png", @"jpg", @"jpeg", @"gif", @"webp"] containsObject:u.pathExtension.lowercaseString]) {
        if (!is_temporary(u)) return u;
        NSURL *dir = images_dir(), *out = [dir URLByAppendingPathComponent:u.lastPathComponent];
        for (int n = 2; [[NSFileManager defaultManager] fileExistsAtPath:out.path]; n++)
            out = [dir URLByAppendingPathComponent:[NSString stringWithFormat:@"%@-%d.%@",
                   u.lastPathComponent.stringByDeletingPathExtension, n, u.pathExtension]];
        return [[NSFileManager defaultManager] copyItemAtURL:u toURL:out error:NULL] ? out : nil;
    }
    NSImage *img = [[NSImage alloc] initWithContentsOfURL:u];
    NSBitmapImageRep *rep = img ? [[NSBitmapImageRep alloc] initWithData:img.TIFFRepresentation] : nil;
    NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    NSURL *out = [[images_dir() URLByAppendingPathComponent:u.lastPathComponent.stringByDeletingPathExtension] URLByAppendingPathExtension:@"png"];
    return png && [png writeToURL:out atomically:YES] ? out : nil;
}

+ (NSArray<NSPasteboardType> *)dragTypes {
    return [@[NSPasteboardTypeFileURL, NSPasteboardTypePNG, NSPasteboardTypeTIFF] arrayByAddingObjectsFromArray:NSFilePromiseReceiver.readableDraggedTypes];
}

+ (NSArray<NSURL *> *)imageFilesIn:(NSPasteboard *)pb {
    NSMutableArray *out = [NSMutableArray array];
    for (NSURL *u in [pb readObjectsForClasses:@[[NSURL class]] options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}])
        if (is_image_file(u)) [out addObject:u];
    return out;
}
+ (NSArray<NSFilePromiseReceiver *> *)imagePromisesIn:(NSPasteboard *)pb {
    NSMutableArray *out = [NSMutableArray array];
    for (NSFilePromiseReceiver *r in [pb readObjectsForClasses:@[[NSFilePromiseReceiver class]] options:@{}]) {
        BOOL image = NO;
        for (NSString *uti in r.fileTypes) image = image || is_image_type(uti);
        if (image) [out addObject:r];
    }
    return out;
}
+ (BOOL)hasImage:(NSPasteboard *)pb {
    return [self imageFilesIn:pb].count || [self imagePromisesIn:pb].count || [pb availableTypeFromArray:@[NSPasteboardTypePNG, NSPasteboardTypeTIFF]];
}

- (BOOL)takeImagesFrom:(NSPasteboard *)pb forSid:(NSString *)sid {
    NSMutableArray *urls = [NSMutableArray array];
    for (NSURL *u in [ReplyBox imageFilesIn:pb]) { NSURL *r = readable(u); if (r) [urls addObject:r]; }
    if (urls.count) return [self addImages:urls forSid:sid];
    NSArray *promises = [ReplyBox imagePromisesIn:pb];
    if (promises.count) { // written by the app they come from, then added as they arrive
        __weak ReplyBox *me = self;
        for (NSFilePromiseReceiver *r in promises)
            [r receivePromisedFilesAtDestination:images_dir() options:@{} operationQueue:[NSOperationQueue mainQueue] reader:^(NSURL *u, NSError *err) {
                NSURL *ok = err ? nil : readable(u);
                if (ok) [me addImages:@[ok] forSid:sid];
            }];
        return YES;
    }
    NSData *png = [pb dataForType:NSPasteboardTypePNG];
    if (!png) {
        NSData *tiff = [pb dataForType:NSPasteboardTypeTIFF];
        png = tiff ? [[[NSBitmapImageRep alloc] initWithData:tiff] representationUsingType:NSBitmapImageFileTypePNG properties:@{}] : nil;
    }
    if (!png) return NO;
    NSDateFormatter *f = [[NSDateFormatter alloc] init];
    f.dateFormat = @"yyyyMMdd-HHmmss-SSS";
    NSURL *out = [images_dir() URLByAppendingPathComponent:[NSString stringWithFormat:@"pasted-%@.png", [f stringFromDate:[NSDate date]]]];
    return [png writeToURL:out atomically:YES] && [self addImages:@[out] forSid:sid];
}

- (BOOL)addImages:(NSArray<NSURL *> *)urls forSid:(NSString *)sid {
    if (!urls.count || !sid) return NO;
    BOOL here = [sid isEqualToString:self.sid];
    NSString *text = here ? self.text.string : (self.drafts[sid] ?: @"");
    NSMutableArray *list = self.attach[sid] ?: [NSMutableArray array];
    if (list.count >= MAX_ATTACH) { // full: forget the images whose tokens were deleted
        NSIndexSet *gone = [list indexesOfObjectsPassingTest:^BOOL(NSDictionary *d, NSUInteger i, BOOL *stop) {
            (void)i; (void)stop;
            return ![text containsString:[NSString stringWithFormat:@"[Image #%@]", d[@"id"]]];
        }];
        [list removeObjectsAtIndexes:gone];
    }
    NSMutableString *ins = [NSMutableString string];
    NSInteger next = [self.nextId[sid] integerValue];
    for (NSURL *u in urls) {
        if (list.count >= MAX_ATTACH) break;
        next++;
        [list addObject:@{@"id": @(next), @"path": u.path, @"name": u.lastPathComponent}];
        [ins appendFormat:@"[Image #%ld] ", (long)next];
    }
    if (!ins.length) return NO;
    self.attach[sid] = list;
    self.nextId[sid] = @(next);
    if (here) {
        NSRange sel = self.text.selectedRange;
        if (sel.location > 0) {
            unichar c = [text characterAtIndex:sel.location - 1];
            if (c != ' ' && c != '\n') [ins insertString:@" " atIndex:0];
        }
        [self.text insertText:ins replacementRange:sel];
        [self.card setNeedsDisplay:YES];
    } else {
        self.drafts[sid] = [text stringByAppendingString:(text.length && ![text hasSuffix:@" "] ? [@" " stringByAppendingString:ins] : ins)];
    }
    return YES;
}

- (void)type:(NSString *)text { [self.text insertText:text replacementRange:self.text.selectedRange]; [self.card setNeedsDisplay:YES]; }
- (NSView *)view { return self.card; }

- (NSString *)hint {
    NSMutableArray *names = [NSMutableArray array];
    NSString *t = self.text.string ?: @"";
    for (NSDictionary *d in self.attach[self.sid]) {
        NSString *tok = [NSString stringWithFormat:@"[Image #%@]", d[@"id"]];
        if ([t containsString:tok]) [names addObject:[NSString stringWithFormat:@"%@ %@", tok, d[@"name"]]];
    }
    if (names.count) return [[names componentsJoinedByString:@",  "] stringByAppendingString:@"   ·   Backspace deletes an image"];
    return @"Enter sends   ·   Shift+Enter new line   ·   Esc closes   ·   drop or paste an image";
}
@end

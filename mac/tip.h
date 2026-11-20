// The floating box beside the panel: the hover tooltip and the voice-reply preview. Used by mac/panel.m.
#import <Cocoa/Cocoa.h>

@interface TipView : NSView
@property(copy) NSString *title, *body;
@property(strong) NSColor *accent;   // the session's colour: title, edge bar and border
@property CGFloat fontSize;
@property int maxBodyLines;          // 0: as many as the text needs
@property BOOL monoBody;             // the body in the terminal's face (monoName) instead of the system font
@property(copy) NSString *monoName;
@property(strong) NSColor *boxColor, *inkColor; // nil: the usual dark box and light text
@property int cols;                  // Settings > Chat > Width: characters across at most (0: about 64)
// The wheel over the box scrolls a body taller than maxBodyLines (scrollUp: points up from its end, 0 = the end shows).
// Pushing on past the top or the end for pushNeed notches calls onPush(+1 or -1); showBar draws a scroll bar. Body lines starting with \x01 are
// your prompt (a band), \x02 dim, \x03 the running line in Claude Code's orange.
@property CGFloat scrollUp;
@property int pushNeed;
@property BOOL showBar;
@property(copy) void (^onPush)(int dir);
- (NSSize)fit;
@end

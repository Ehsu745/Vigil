// READY on the Mac: a reply nobody has read throws pieces of its core at the pointer (motion.c, Restless). Used by
// mac/panel.m, which decides who is READY and where the pointer is (Session.aim_*); these two only draw.
#import <Cocoa/Cocoa.h>
#include "../src/vigil.h"

// A see-through window over the part of the screen where the pieces fly (from the core to the pointer), that never
// takes the mouse; shown only while pieces are out. The core's own picture leaves them out (RENDER_PART 1), this
// draws only them (RENDER_PART 2), at the core's place, so a piece can reach the pointer anywhere on the screen.
@interface ReadyLayer : NSObject
@property NSWindowLevel level;                   // just over the panel's
@property NSWindowCollectionBehavior behavior;
// core: where the core is on the screen (points); px: the side of its picture in pixels
- (void)draw:(const Session *)s core:(NSRect)core pixels:(int)px shape:(const Shape *)sh glow:(float)glow coreFrac:(float)cf now:(double)now;
- (void)hide;
@property(readonly) NSRect frame;                // where the window is (screen points), empty while hidden
@end

// READY's dodge game: the hearts, a column at the left edge of the core's screen. They drop in one by one as a run
// starts; a lost one pops and goes dim; when the run ends they fade out (as on Windows).
@interface ReadyHearts : NSObject
@property NSWindowLevel level;
@property NSWindowCollectionBehavior behavior;
- (void)show:(const Session *)s screen:(NSScreen *)screen now:(double)now;
- (void)hide;
@end

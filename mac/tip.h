// The floating box beside the panel: the hover tooltip and the voice-reply preview. Used by mac/panel.m.
#import <Cocoa/Cocoa.h>

@interface TipView : NSView
@property(copy) NSString *title, *body;
@property(strong) NSColor *accent;   // the session's colour: title, edge bar and border
@property CGFloat fontSize;
@property int maxBodyLines;          // 0: as many as the text needs
- (NSSize)fit;
@end

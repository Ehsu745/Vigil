// The reply box under the tooltip: a double-click on a core opens it, Enter sends what was typed to that
// session. Used by mac/panel.m.
#import <Cocoa/Cocoa.h>

@class ReplyBox;
@protocol ReplyBoxDelegate
- (void)replyBox:(ReplyBox *)box send:(NSString *)text to:(NSString *)sid; // tokens already turned into paths
- (void)replyBoxClosed:(ReplyBox *)box;
@end

@interface ReplyBox : NSObject
@property(weak) id<ReplyBoxDelegate> delegate;
@property(copy) NSString *fontName; // the terminal's face, as in the panel (--font)
@property CGFloat fontSize;
@property NSWindowCollectionBehavior behavior;
@property(readonly) NSString *sid; // the session it is open for, nil when closed
@property(readonly) BOOL open;
@property(strong, nonatomic) NSColor *accent;
@property(strong) NSColor *boxColor, *inkColor; // the text box's colours (Settings > Replies > Colours); nil: the usual // the session's colour, kept the same as its tooltip's
- (CGFloat)height;
// opens for a session (or brings it back with what was typed for it last time) and takes the keyboard
- (void)openFor:(NSString *)sid name:(NSString *)name accent:(NSColor *)accent;
// right under the tooltip's frame, as wide as it, kept on the screen
- (void)placeUnder:(NSRect)tip visible:(NSRect)vis;
- (void)close; // keeps what was typed for next time
// image files dropped on a core or on the box: each gets an "[Image #N]" token at the cursor; NO if none was an image
- (BOOL)addImages:(NSArray<NSURL *> *)urls forSid:(NSString *)sid;
// what a drop or a paste may bring an image in: image files, file promises (the screenshot thumbnail, Photos),
// or image data (a screenshot copied to the clipboard)
+ (NSArray<NSPasteboardType> *)dragTypes;
+ (BOOL)hasImage:(NSPasteboard *)pb;
// the images in a drop or paste, as tokens; promised files arrive a moment later. NO if there was none
- (BOOL)takeImagesFrom:(NSPasteboard *)pb forSid:(NSString *)sid;
- (void)type:(NSString *)text; // at the cursor, as if typed (--reply-shot)
- (NSView *)view;              // the box as drawn (--reply-shot)
@end

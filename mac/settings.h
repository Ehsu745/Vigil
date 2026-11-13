// The Settings window of the macOS panel (right-click > Settings...), drawn like the Windows panel's: a live
// preview across the top and cards below. Used by mac/panel.m, which owns the values: the window only reads
// and changes them through SettingsHost, so every choice applies at once and is kept, as the menu's were.
#import <Cocoa/Cocoa.h>
#include "../src/vigil.h"

// the text box's colours (Settings > Replies > Colours), the same as the Windows panel's: 0xRRGGBB
extern const unsigned TIP_BGS[5];   // Ink, Graphite, Navy, Plum, Pine; index 5 is the custom colour
extern const unsigned TEXT_COLS[5]; // Auto (0: chosen from the box), White, Cream, Mint, Sky; 5 is custom
NSColor *hex_color(unsigned rgb);
unsigned color_hex(NSColor *c);
BOOL hex_light(unsigned rgb); // a light colour, that wants dark text on it

@protocol SettingsHost <NSObject>
- (id)setting:(NSString *)key;                       // the value in use (NSNumber or NSString)
- (void)chooseSetting:(NSString *)key value:(id)value; // applies it and keeps it
- (BOOL)settingsSaved;                               // something differs from the launch options
- (void)resetSettings;                               // back to the launch options
- (const Shape *)shape;
- (void)drawFocusFrame:(CGContextRef)ctx rect:(NSRect)r session:(const Session *)s now:(double)now;
- (BOOL)canSummarize;                                // claude was found, so "Summaries by Claude" can be on
// the Motions page changed MOTION_MAP for a state: keep it; rt (or -1) is a routine just switched off for it
- (void)motionMapChangedForState:(int)st routineOff:(int)rt;
@end

@interface SettingsWindow : NSObject
- (instancetype)initWithHost:(id<SettingsHost>)host;
- (void)showOn:(NSScreen *)screen behavior:(NSWindowCollectionBehavior)behavior;
- (void)close;
@property(readonly) BOOL visible;
- (void)changed;                      // a value changed elsewhere (the menu, a reset): draw again
- (void)showPage:(int)page;            // 0 General, 1 Motions
- (BOOL)saveShot:(NSString *)path;    // the window as a PNG (--settings-shot)
@end

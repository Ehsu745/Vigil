// Voice input for dodeca-panel: push-to-talk speech recognition, on this Mac only. Used by mac/panel.m.
#import <Foundation/Foundation.h>

@interface Listener : NSObject
- (instancetype)initWithLocale:(NSString *)locale;

// Why listening cannot work (no on-device recognition for the language, permission denied), or nil.
// The first call asks for the microphone and speech recognition permissions if macOS has not asked yet;
// until they are answered it returns a "waiting for permission" reason.
- (NSString *)unavailableReason;

// Starts recording. partial gets the text recognized so far (main thread). NO if it could not start.
- (BOOL)start:(void (^)(NSString *partial))partial;
// Stops recording and waits for the final text (at most a few seconds). done gets nil if nothing was
// understood; called on the main thread.
- (void)finish:(void (^)(NSString *text))done;
// Stops and throws away whatever was recorded.
- (void)cancel;
- (float)level; // 0..1, the microphone level while recording
@end

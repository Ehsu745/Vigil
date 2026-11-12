// Voice input for vigil-panel: push-to-talk speech recognition, on this Mac only. Used by mac/panel.m.
#import <Foundation/Foundation.h>

@interface Listener : NSObject
// mic: part of the input device's name to use (--mic), or nil: the system's default input, unless that is a
// virtual device (BlackHole, Loopback, ...), which carries no voice: then the built-in microphone.
- (instancetype)initWithLocale:(NSString *)locale mic:(NSString *)mic;

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
@property(copy) void (^log)(NSString *line); // what happened, for --debug (never the words)
@property(readonly, copy) NSString *silentDevice; // set when the last recording was pure silence: the device used
@end

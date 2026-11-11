// Voice output for dodeca-panel: turns a reply into something worth hearing and speaks it, one session at a
// time. Used only by mac/panel.m.
#import <Foundation/Foundation.h>

// Markdown -> plain speakable text: code blocks become a short "skipping a code block", links keep their
// text, URLs and Markdown symbols go. Paragraphs stay separated by a blank line.
NSString *speech_clean(NSString *markdown, BOOL chinese);

// The local summary: the first paragraph with words in it, at most 3 sentences or 150 characters.
NSString *speech_local_summary(NSString *markdown, BOOL chinese);

@interface Speaker : NSObject
@property(readonly) BOOL chinese;              // the voice speaks Chinese: the panel's own words are Chinese too
@property(readonly, copy) NSString *speakingSid;
@property(copy) void (^finished)(NSString *sid); // called on the main thread when an utterance ends or is stopped

- (instancetype)initWithVoice:(NSString *)voiceId rate:(float)rate;
// Queues text for a session. urgent puts it at the front of the queue (it still waits for the one speaking).
// A session has at most one item: a new one replaces what it had queued.
- (void)say:(NSString *)text sid:(NSString *)sid volume:(float)volume urgent:(BOOL)urgent;
- (void)stop:(NSString *)sid;                  // stops it if speaking, drops it from the queue
- (BOOL)has:(NSString *)sid;                   // speaking or queued
- (float)level;                                // 0..1, follows the words as they are spoken
@end

// Asks `claude -p` for a two or three sentence spoken summary, in the background. done gets nil on failure
// or after the timeout; it is called on the main thread.
void speech_claude_summary(NSString *claude, NSString *reply, BOOL chinese, double timeout, void (^done)(NSString *summary));

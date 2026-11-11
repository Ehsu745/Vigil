// Voice output: text cleanup, summaries and the one-at-a-time speaker. See speech.h.
#import "speech.h"
#import <AVFoundation/AVFoundation.h>
#include <sys/time.h>

static double now_s(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + tv.tv_usec / 1e6;
}

static NSString *replace(NSString *s, NSString *pattern, NSString *with, NSRegularExpressionOptions opt) {
    NSRegularExpression *re = [NSRegularExpression regularExpressionWithPattern:pattern options:opt error:NULL];
    return [re stringByReplacingMatchesInString:s options:0 range:NSMakeRange(0, s.length) withTemplate:with];
}

NSString *speech_clean(NSString *md, BOOL chinese) {
    if (!md.length) return @"";
    NSString *skip = chinese ? @"（略過一段程式碼）" : @"(skipping a code block)";
    NSString *s = [md stringByReplacingOccurrencesOfString:@"\r\n" withString:@"\n"];
    s = replace(s, @"^[ \\t]*(```|~~~)[^\\n]*\\n.*?^[ \\t]*\\1[ \\t]*$", [NSString stringWithFormat:@"\n%@\n", skip],
                NSRegularExpressionAnchorsMatchLines | NSRegularExpressionDotMatchesLineSeparators);
    s = replace(s, @"^[ \\t]*(```|~~~).*", skip, NSRegularExpressionAnchorsMatchLines | NSRegularExpressionDotMatchesLineSeparators); // an unclosed fence
    s = replace(s, @"!\\[([^\\]]*)\\]\\([^)]*\\)", @"$1", 0);                       // images
    s = replace(s, @"\\[([^\\]]+)\\]\\([^)]*\\)", @"$1", 0);                         // links keep their text
    s = replace(s, @"<?https?://[^\\s)>]+>?", @"", 0);                              // bare URLs
    s = replace(s, @"`([^`\\n]+)`", @"$1", 0);                                      // inline code keeps its text
    s = replace(s, @"^[ \\t]*#{1,6}[ \\t]+", @"", NSRegularExpressionAnchorsMatchLines);       // headings
    s = replace(s, @"^[ \\t]*>[ \\t]?", @"", NSRegularExpressionAnchorsMatchLines);            // quotes
    s = replace(s, @"^[ \\t]*([-*+]|\\d+[.)])[ \\t]+", @"", NSRegularExpressionAnchorsMatchLines); // list markers
    s = replace(s, @"^[ \\t]*\\|?[ \\t]*:?-{3,}.*$", @"", NSRegularExpressionAnchorsMatchLines);  // table rules
    s = replace(s, @"^[ \\t]*([-*_][ \\t]*){3,}$", @"", NSRegularExpressionAnchorsMatchLines);    // horizontal rules
    s = replace(s, @"[ \\t]*\\|[ \\t]*", @"，", 0);                                 // table cells
    s = replace(s, @"(\\*\\*|__|~~|\\*|(?<![A-Za-z0-9])_(?![A-Za-z0-9]))", @"", 0);  // emphasis
    s = replace(s, @"[ \\t]+", @" ", 0);
    s = replace(s, @"\\n{3,}", @"\n\n", 0);
    return [s stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
}

NSString *speech_local_summary(NSString *md, BOOL chinese) {
    NSString *skip = chinese ? @"（略過一段程式碼）" : @"(skipping a code block)";
    NSString *para = @"";
    NSString *body = replace(md ?: @"", @"^[ \\t]*#{1,6}[ \\t]+.*$", @"", NSRegularExpressionAnchorsMatchLines); // headings are not the news
    for (NSString *p in [speech_clean(body, chinese) componentsSeparatedByString:@"\n\n"]) {
        NSString *t = [[p stringByReplacingOccurrencesOfString:@"\n" withString:@" "]
            stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        if (t.length && ![t isEqualToString:skip]) { para = t; break; }
    }
    // up to 3 sentences (。！？ or . ! ? followed by a space), then at most 150 characters
    NSRegularExpression *re = [NSRegularExpression regularExpressionWithPattern:@"[^。！？!?.]*(?:[。！？]|[!?.](?=\\s|$))" options:0 error:NULL];
    NSArray *ms = [re matchesInString:para options:0 range:NSMakeRange(0, para.length)];
    if (ms.count > 3) para = [para substringToIndex:NSMaxRange([ms[2] range])];
    if (para.length > 150) {
        NSRange r = [para rangeOfComposedCharacterSequencesForRange:NSMakeRange(0, 150)];
        para = [[para substringWithRange:r] stringByAppendingString:@"…"];
    }
    return [para stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
}

// ---- the speaker -----------------------------------------------------------------------------------

// The best installed voice for a language: Premium, then Enhanced, then the regular one; never the
// novelty or Eloquence voices (robotic) unless nothing else is there. Downloading a better voice in System
// Settings is enough for it to be used.
static AVSpeechSynthesisVoice *best_voice(NSString *language) {
    AVSpeechSynthesisVoice *best = nil;
    int bestScore = -1;
    for (AVSpeechSynthesisVoice *v in [AVSpeechSynthesisVoice speechVoices]) {
        if (![v.language isEqualToString:language]) continue;
        NSString *id = v.identifier;
        int score = (int)v.quality * 10;
        if ([id containsString:@".premium."]) score += 5;
        else if ([id containsString:@".enhanced."]) score += 4;
        else if ([id containsString:@".compact."]) score += 3;
        else if ([id containsString:@".super-compact."]) score += 2;
        else if ([id containsString:@"eloquence"] || [id containsString:@"speech.synthesis"]) score += 0;
        else score += 1;
        if (score > bestScore) { bestScore = score; best = v; }
    }
    return best ?: [AVSpeechSynthesisVoice voiceWithLanguage:language];
}
@interface Speaker () <AVSpeechSynthesizerDelegate>
@end

@implementation Speaker {
    AVSpeechSynthesizer *_synth;
    AVSpeechSynthesisVoice *_voice;
    float _rate;
    NSMutableArray<NSDictionary *> *_queue; // {sid, text, volume}
    NSString *_speaking;
    double _wordAt;
    NSUInteger _wordLen;
}

- (instancetype)initWithVoice:(NSString *)voiceId rate:(float)rate {
    if (!(self = [super init])) return nil;
    _synth = [[AVSpeechSynthesizer alloc] init];
    _synth.delegate = self;
    _voice = [AVSpeechSynthesisVoice voiceWithIdentifier:voiceId] ?: best_voice(voiceId);
    if (!_voice) {
        fprintf(stderr, "dodeca-panel: no voice \"%s\"; using the system voice\n", voiceId.UTF8String);
        _voice = [AVSpeechSynthesisVoice voiceWithLanguage:nil];
    }
    _chinese = [_voice.language hasPrefix:@"zh"];
    _rate = rate;
    _queue = [NSMutableArray array];
    _wordAt = -1e9;
    return self;
}

- (NSString *)speakingSid { return _speaking; }

- (void)say:(NSString *)text sid:(NSString *)sid volume:(float)volume urgent:(BOOL)urgent {
    if (!text.length || !sid) return;
    [self dropQueued:sid];
    NSDictionary *item = @{@"sid": sid, @"text": text, @"volume": @(volume)};
    if (urgent) [_queue insertObject:item atIndex:0];
    else [_queue addObject:item];
    [self next];
}

- (void)dropQueued:(NSString *)sid {
    [_queue filterUsingPredicate:[NSPredicate predicateWithBlock:^BOOL(NSDictionary *d, NSDictionary *b) { (void)b; return ![d[@"sid"] isEqualToString:sid]; }]];
}

- (void)stop:(NSString *)sid {
    [self dropQueued:sid];
    if ([_speaking isEqualToString:sid]) [_synth stopSpeakingAtBoundary:AVSpeechBoundaryImmediate]; // didCancel follows
}

- (BOOL)has:(NSString *)sid {
    if ([_speaking isEqualToString:sid]) return YES;
    for (NSDictionary *d in _queue) if ([d[@"sid"] isEqualToString:sid]) return YES;
    return NO;
}

- (void)next {
    if (_speaking || !_queue.count) return;
    NSDictionary *d = _queue.firstObject;
    [_queue removeObjectAtIndex:0];
    _speaking = d[@"sid"];
    AVSpeechUtterance *u = [AVSpeechUtterance speechUtteranceWithString:d[@"text"]];
    u.voice = _voice;
    u.rate = _rate;
    u.volume = [d[@"volume"] floatValue];
    u.postUtteranceDelay = 0.25;
    [_synth speakUtterance:u];
}

// AVSpeechSynthesizer does not report loudness, so the level is an envelope: it jumps at every word,
// higher for longer words, and decays between them.
- (float)level {
    if (!_speaking) return 0;
    double since = now_s() - _wordAt;
    float peak = 0.55f + fminf(0.45f, _wordLen * 0.06f);
    float v = peak * (float)exp(-since * 3.5);
    return v < 0.12f ? 0.12f : v; // never fully dark while speaking
}

- (void)speechSynthesizer:(AVSpeechSynthesizer *)s willSpeakRangeOfSpeechString:(NSRange)r utterance:(AVSpeechUtterance *)u {
    (void)s; (void)u;
    _wordAt = now_s();
    _wordLen = r.length;
}

- (void)done {
    NSString *sid = _speaking;
    _speaking = nil;
    if (self.finished && sid) self.finished(sid);
    [self next];
}
- (void)speechSynthesizer:(AVSpeechSynthesizer *)s didFinishSpeechUtterance:(AVSpeechUtterance *)u { (void)s; (void)u; [self done]; }
- (void)speechSynthesizer:(AVSpeechSynthesizer *)s didCancelSpeechUtterance:(AVSpeechUtterance *)u { (void)s; (void)u; [self done]; }
@end

// ---- the Claude summary -------------------------------------------------------------------------------
void speech_claude_summary(NSString *claude, NSString *reply, BOOL chinese, double timeout, void (^done)(NSString *)) {
    NSString *prompt = chinese
        ? @"用繁體中文（台灣用語），把標準輸入裡這則 AI 助手的回覆濃縮成兩三句口語化的句子，給語音朗讀用。不要用 Markdown、不要列點、不要念出程式碼或網址。只輸出摘要本身。"
        : @"Condense the AI assistant reply on standard input into two or three conversational sentences to be read aloud. No Markdown, no lists, no code or URLs. Output only the summary.";
    NSTask *task = [[NSTask alloc] init];
    task.executableURL = [NSURL fileURLWithPath:claude];
    // no hooks (it would show up as a session of its own), no tools, nothing saved, outside any project
    task.arguments = @[@"-p", @"--model", @"haiku", @"--no-session-persistence", @"--tools", @"",
                       @"--settings", @"{\"disableAllHooks\":true}", prompt];
    task.currentDirectoryURL = [NSURL fileURLWithPath:NSTemporaryDirectory()];
    NSPipe *in = [NSPipe pipe], *out = [NSPipe pipe];
    task.standardInput = in;
    task.standardOutput = out;
    task.standardError = [NSFileHandle fileHandleWithNullDevice];
    __block BOOL answered = NO;
    void (^answer)(NSString *) = ^(NSString *text) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (answered) return;
            answered = YES;
            done(text);
        });
    };
    task.terminationHandler = ^(NSTask *t) {
        NSData *data = [out.fileHandleForReading readDataToEndOfFile];
        NSString *text = [[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding]
            stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
        answer(t.terminationStatus == 0 && text.length ? text : nil);
    };
    if (![task launchAndReturnError:NULL]) { answer(nil); return; }
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        [in.fileHandleForWriting writeData:[reply dataUsingEncoding:NSUTF8StringEncoding]];
        [in.fileHandleForWriting closeFile];
    });
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(timeout * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        if (task.running) [task terminate];
        answer(nil); // too slow: the caller falls back to the local summary
    });
}

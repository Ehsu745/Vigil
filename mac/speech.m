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
@interface Speaker () <AVSpeechSynthesizerDelegate, AVAudioPlayerDelegate>
@end

// Two ways to speak:
//  - a named voice (--voice-id zh-TW, or an identifier): AVSpeechSynthesizer, starts at once.
//  - "system" (the default): the voice chosen in System Settings > Accessibility > Spoken Content > System
//    Voice. Only /usr/bin/say reaches it, and it is the only way to use a Siri voice, which reads Chinese
//    with English words in it far better than any voice an app may pick. Each sentence is rendered to a
//    file by say and played here (so the volume and the real loudness are known); the next sentence
//    renders while one plays, so only the first one is waited for.
@implementation Speaker {
    AVSpeechSynthesizer *_synth;
    AVSpeechSynthesisVoice *_voice;
    BOOL _system;
    NSString *_tmp;                         // folder for the rendered sentences
    NSMutableArray<NSString *> *_chunks;    // sentences still to render, for the item being spoken
    NSMutableArray<NSString *> *_ready;     // rendered files waiting to play
    NSTask *_render;
    AVAudioPlayer *_player;
    float _volume;
    unsigned _gen;                          // bumped on stop: late callbacks of an old item are ignored
    float _rate;
    NSMutableArray<NSDictionary *> *_queue; // {sid, text, volume}
    NSString *_speaking;
    double _wordAt;
    NSUInteger _wordLen;
}

- (instancetype)initWithVoice:(NSString *)voiceId rate:(float)rate {
    if (!(self = [super init])) return nil;
    _rate = rate;
    _queue = [NSMutableArray array];
    _wordAt = -1e9;
    if ([voiceId isEqualToString:@"system"] && [[NSFileManager defaultManager] isExecutableFileAtPath:@"/usr/bin/say"]) {
        _system = YES;
        _chinese = [[NSLocale preferredLanguages].firstObject hasPrefix:@"zh"];
        _tmp = [NSTemporaryDirectory() stringByAppendingPathComponent:[NSString stringWithFormat:@"vigil-say-%d", (int)getpid()]];
        [[NSFileManager defaultManager] createDirectoryAtPath:_tmp withIntermediateDirectories:YES attributes:@{NSFilePosixPermissions: @0700} error:NULL];
        return self;
    }
    if ([voiceId isEqualToString:@"system"]) voiceId = @"zh-TW";
    _synth = [[AVSpeechSynthesizer alloc] init];
    _synth.delegate = self;
    _voice = [AVSpeechSynthesisVoice voiceWithIdentifier:voiceId] ?: best_voice(voiceId);
    if (!_voice) {
        fprintf(stderr, "vigil-panel: no voice \"%s\"; using the system voice\n", voiceId.UTF8String);
        _voice = [AVSpeechSynthesisVoice voiceWithLanguage:nil];
    }
    _chinese = [_voice.language hasPrefix:@"zh"];
    return self;
}

- (void)dealloc { if (_tmp) [[NSFileManager defaultManager] removeItemAtPath:_tmp error:NULL]; }

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
    if (![_speaking isEqualToString:sid]) return;
    if (_system) { [self systemStop]; [self done]; }
    else [_synth stopSpeakingAtBoundary:AVSpeechBoundaryImmediate]; // didCancel follows
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
    if (_system) { [self systemSpeak:d[@"text"] volume:[d[@"volume"] floatValue]]; return; }
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
    if (_system) { // the real loudness of what is playing
        if (!_player.playing) return 0.12f;
        [_player updateMeters];
        float db = [_player averagePowerForChannel:0]; // about -50 (quiet) .. -8 (loud)
        return fmaxf(0.12f, fminf(1, (db + 45) / 35));
    }
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
// ---- the system voice through say --------------------------------------------------------------------

// sentences of a sensible length: split after 。！？!? (and . before a space), short ones joined
static NSArray<NSString *> *sentences(NSString *text) {
    NSMutableArray *out = [NSMutableArray array];
    // "notes 完成了：…": the short lead-in plays on its own, so the first sound comes almost at once
    NSRange colon = [text rangeOfCharacterFromSet:[NSCharacterSet characterSetWithCharactersInString:@"：:"]];
    if (colon.location != NSNotFound && colon.location < 30 && NSMaxRange(colon) < text.length) {
        [out addObject:[text substringToIndex:NSMaxRange(colon)]];
        text = [text substringFromIndex:NSMaxRange(colon)];
    }
    NSRegularExpression *re = [NSRegularExpression regularExpressionWithPattern:@"[^。！？!?.]*(?:[。！？!?]+|\\.(?=\\s|$)|$)" options:0 error:NULL];
    NSMutableString *cur = [NSMutableString string];
    for (NSTextCheckingResult *m in [re matchesInString:text options:0 range:NSMakeRange(0, text.length)]) {
        NSString *part = [[text substringWithRange:m.range] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
        if (!part.length) continue;
        [cur appendString:cur.length ? [@" " stringByAppendingString:part] : part];
        if (cur.length >= 24) { [out addObject:[cur copy]]; [cur setString:@""]; }
    }
    if (cur.length) [out addObject:cur];
    return out;
}

- (void)systemSpeak:(NSString *)text volume:(float)volume {
    _volume = volume;
    _chunks = [sentences(text) mutableCopy];
    _ready = [NSMutableArray array];
    if (!_chunks.count) { [self done]; return; }
    [self renderNext];
}

- (void)renderNext {
    if (_render || !_chunks.count) return;
    NSString *text = _chunks.firstObject;
    [_chunks removeObjectAtIndex:0];
    NSString *file = [_tmp stringByAppendingPathComponent:[NSString stringWithFormat:@"%u-%u.aiff", _gen, arc4random()]];
    NSTask *t = [[NSTask alloc] init];
    t.executableURL = [NSURL fileURLWithPath:@"/usr/bin/say"];
    NSMutableArray *args = [NSMutableArray arrayWithObjects:@"-o", file, nil];
    if (fabsf(_rate - 0.5f) > 0.01f) [args addObjectsFromArray:@[@"-r", [NSString stringWithFormat:@"%d", (int)(180 * _rate / 0.5f)]]];
    [args addObjectsFromArray:@[@"--", text]];
    t.arguments = args;
    t.standardOutput = t.standardError = [NSFileHandle fileHandleWithNullDevice];
    unsigned gen = _gen;
    __weak Speaker *me = self;
    t.terminationHandler = ^(NSTask *task) {
        int ok = task.terminationStatus == 0;
        dispatch_async(dispatch_get_main_queue(), ^{
            Speaker *sp = me;
            if (!sp || gen != sp->_gen) { [[NSFileManager defaultManager] removeItemAtPath:file error:NULL]; return; }
            sp->_render = nil;
            if (ok) [sp->_ready addObject:file];
            [sp playNext];
            [sp renderNext];
        });
    };
    _render = t;
    if (![t launchAndReturnError:NULL]) { _render = nil; [self done]; }
}

- (void)playNext {
    if (_player.playing) return;
    if (!_ready.count) {
        if (!_render && !_chunks.count) [self done]; // everything has been played
        return;
    }
    NSString *file = _ready.firstObject;
    [_ready removeObjectAtIndex:0];
    _player = [[AVAudioPlayer alloc] initWithContentsOfURL:[NSURL fileURLWithPath:file] error:NULL];
    [[NSFileManager defaultManager] removeItemAtPath:file error:NULL]; // already loaded
    if (!_player) { [self playNext]; return; }
    _player.delegate = self;
    _player.volume = _volume;
    _player.meteringEnabled = YES;
    [_player play];
}

- (void)audioPlayerDidFinishPlaying:(AVAudioPlayer *)p successfully:(BOOL)ok {
    (void)ok;
    if (p != _player) return;
    _player = nil;
    [self playNext];
}

- (void)systemStop {
    _gen++;
    [_render terminate];
    _render = nil;
    [_player stop];
    _player = nil;
    for (NSString *f in _ready) [[NSFileManager defaultManager] removeItemAtPath:f error:NULL];
    [_ready removeAllObjects];
    [_chunks removeAllObjects];
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

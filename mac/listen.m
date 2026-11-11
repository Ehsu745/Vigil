// Voice input: SFSpeechRecognizer with on-device recognition only. Audio never leaves this Mac: if the
// language has no on-device model, listening is turned off instead of falling back to Apple's servers.
#import "listen.h"
#import <AVFoundation/AVFoundation.h>
#import <Speech/Speech.h>

@implementation Listener {
    SFSpeechRecognizer *_recognizer;
    AVAudioEngine *_engine;
    SFSpeechAudioBufferRecognitionRequest *_request;
    SFSpeechRecognitionTask *_task;
    NSString *_text;
    void (^_done)(NSString *);
    BOOL _asked;
    float _level; // written on the audio thread, read on the main thread: a float is fine for a meter
}

- (instancetype)initWithLocale:(NSString *)locale {
    if (!(self = [super init])) return nil;
    _recognizer = [[SFSpeechRecognizer alloc] initWithLocale:[NSLocale localeWithLocaleIdentifier:locale]];
    _engine = [[AVAudioEngine alloc] init];
    return self;
}

- (NSString *)unavailableReason {
    if (!_recognizer) return @"這個語言沒有語音辨識";
    if (!_recognizer.supportsOnDeviceRecognition) return @"這台 Mac 沒有這個語言的離線語音辨識，為了不把錄音上傳，按住說話已關閉";
    SFSpeechRecognizerAuthorizationStatus sr = [SFSpeechRecognizer authorizationStatus];
    AVAuthorizationStatus mic = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
    if (sr == SFSpeechRecognizerAuthorizationStatusNotDetermined || mic == AVAuthorizationStatusNotDetermined) {
        if (!_asked) {
            _asked = YES;
            [SFSpeechRecognizer requestAuthorization:^(SFSpeechRecognizerAuthorizationStatus s) {
                (void)s;
                [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL ok) { (void)ok; }];
            }];
        }
        return @"請先允許麥克風和語音辨識，然後再按一次";
    }
    if (sr != SFSpeechRecognizerAuthorizationStatusAuthorized) return @"語音辨識沒有被允許，請到系統設定的隱私權與安全性打開";
    if (mic != AVAuthorizationStatusAuthorized) return @"麥克風沒有被允許，請到系統設定的隱私權與安全性打開";
    if (!_recognizer.available) return @"語音辨識暫時不能用";
    return nil;
}

- (BOOL)start:(void (^)(NSString *))partial {
    [self cancel];
    if ([self unavailableReason]) return NO;
    _text = @"";
    _request = [[SFSpeechAudioBufferRecognitionRequest alloc] init];
    _request.requiresOnDeviceRecognition = YES; // never upload the recording
    _request.shouldReportPartialResults = YES;
    _request.addsPunctuation = YES;
    __weak Listener *me = self;
    _task = [_recognizer recognitionTaskWithRequest:_request resultHandler:^(SFSpeechRecognitionResult *r, NSError *err) {
        dispatch_async(dispatch_get_main_queue(), ^{
            Listener *l = me;
            if (!l) return;
            if (r) {
                l->_text = r.bestTranscription.formattedString ?: @"";
                if (!r.final && partial) partial(l->_text);
            }
            if ((r && r.final) || err) [l deliver];
        });
    }];
    AVAudioInputNode *input = _engine.inputNode;
    AVAudioFormat *format = [input outputFormatForBus:0];
    SFSpeechAudioBufferRecognitionRequest *req = _request;
    [input removeTapOnBus:0];
    [input installTapOnBus:0 bufferSize:1024 format:format block:^(AVAudioPCMBuffer *buf, AVAudioTime *when) {
        (void)when;
        [req appendAudioPCMBuffer:buf];
        Listener *l = me;
        if (!l || !buf.floatChannelData || !buf.frameLength) return;
        const float *x = buf.floatChannelData[0];
        float sum = 0;
        for (AVAudioFrameCount i = 0; i < buf.frameLength; i++) sum += x[i] * x[i];
        float db = 10 * log10f(sum / buf.frameLength + 1e-9f); // about -60 (quiet) .. -10 (loud)
        l->_level = fmaxf(0, fminf(1, (db + 55) / 40));
    }];
    NSError *err = nil;
    [_engine prepare];
    if (![_engine startAndReturnError:&err]) { [self cancel]; return NO; }
    return YES;
}

- (void)stopAudio {
    if (_engine.running) [_engine stop];
    [_engine.inputNode removeTapOnBus:0];
    _level = 0;
}

- (void)finish:(void (^)(NSString *))done {
    [self stopAudio];
    if (!_task) { done(nil); return; }
    _done = done;
    [_request endAudio];
    __weak Listener *me = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(5 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        [me deliver]; // no final result in time: take what was understood so far
    });
}

- (void)deliver {
    void (^done)(NSString *) = _done;
    if (!done) return; // still recording, or already delivered
    _done = nil;
    NSString *t = [_text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    [_task cancel];
    _task = nil;
    _request = nil;
    done(t.length ? t : nil);
}

- (void)cancel {
    [self stopAudio];
    _done = nil;
    [_task cancel];
    _task = nil;
    _request = nil;
}

- (float)level { return _engine.running ? _level : 0; }
@end

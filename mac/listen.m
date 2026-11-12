// Voice input: SFSpeechRecognizer with on-device recognition only. Audio never leaves this Mac: if the
// language has no on-device model, listening is turned off instead of falling back to Apple's servers.
#import "listen.h"
#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>
#import <Speech/Speech.h>

// ---- choosing the microphone -------------------------------------------------------------------------
static NSString *device_name(AudioDeviceID d) {
    CFStringRef name = NULL;
    UInt32 size = sizeof name;
    AudioObjectPropertyAddress a = {kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    if (AudioObjectGetPropertyData(d, &a, 0, NULL, &size, &name) != noErr || !name) return @"?";
    return (__bridge_transfer NSString *)name;
}

static BOOL has_input(AudioDeviceID d) {
    AudioObjectPropertyAddress a = {kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput, kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    return AudioObjectGetPropertyDataSize(d, &a, 0, NULL, &size) == noErr && size > 0;
}

static UInt32 transport(AudioDeviceID d) {
    UInt32 t = 0, size = sizeof t;
    AudioObjectPropertyAddress a = {kAudioDevicePropertyTransportType, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    AudioObjectGetPropertyData(d, &a, 0, NULL, &size, &t);
    return t;
}

// loopback and meeting-app devices that carry other apps' sound, not a voice
static BOOL is_virtual(AudioDeviceID d) {
    if (transport(d) == kAudioDeviceTransportTypeVirtual) return YES;
    NSString *n = device_name(d).lowercaseString;
    for (NSString *v in @[@"blackhole", @"loopback", @"soundflower", @"background music", @"zoomaudio", @"teams audio", @"virtual"])
        if ([n containsString:v]) return YES;
    return NO;
}

static AudioDeviceID pick_input(NSString *want) {
    AudioObjectPropertyAddress all = {kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    AudioObjectPropertyAddress def = {kAudioHardwarePropertyDefaultInputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    AudioDeviceID dflt = kAudioObjectUnknown, ids[64];
    UInt32 size = sizeof dflt;
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &def, 0, NULL, &size, &dflt);
    size = sizeof ids;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &all, 0, NULL, &size, ids) != noErr) return dflt;
    UInt32 n = size / sizeof ids[0];
    if (want.length)
        for (UInt32 i = 0; i < n; i++)
            if (has_input(ids[i]) && [device_name(ids[i]).lowercaseString containsString:want.lowercaseString]) return ids[i];
    if (dflt != kAudioObjectUnknown && !is_virtual(dflt)) return dflt;
    for (UInt32 i = 0; i < n; i++) if (has_input(ids[i]) && transport(ids[i]) == kAudioDeviceTransportTypeBuiltIn) return ids[i];
    for (UInt32 i = 0; i < n; i++) if (has_input(ids[i]) && !is_virtual(ids[i])) return ids[i];
    return dflt;
}

@implementation Listener {
    SFSpeechRecognizer *_recognizer;
    AVAudioEngine *_engine;
    SFSpeechAudioBufferRecognitionRequest *_request;
    SFSpeechRecognitionTask *_task;
    NSString *_text;
    void (^_done)(NSString *);
    BOOL _asked;
    float _level; // written on the audio thread, read on the main thread: a float is fine for a meter
    unsigned _buffers;
    float _peak;
    NSString *_mic, *_device;
}

- (void)note:(NSString *)fmt, ... {
    if (!self.log) return;
    va_list ap;
    va_start(ap, fmt);
    NSString *line = [[NSString alloc] initWithFormat:fmt arguments:ap];
    va_end(ap);
    self.log(line);
}

- (instancetype)initWithLocale:(NSString *)locale mic:(NSString *)mic {
    if (!(self = [super init])) return nil;
    _mic = [mic copy];
    _recognizer = [[SFSpeechRecognizer alloc] initWithLocale:[NSLocale localeWithLocaleIdentifier:locale]];
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
    _buffers = 0;
    _peak = 0;
    _request = [[SFSpeechAudioBufferRecognitionRequest alloc] init];
    _request.requiresOnDeviceRecognition = YES; // never upload the recording
    _request.shouldReportPartialResults = YES;
    _request.addsPunctuation = YES;
    __weak Listener *me = self;
    _task = [_recognizer recognitionTaskWithRequest:_request resultHandler:^(SFSpeechRecognitionResult *r, NSError *err) {
        dispatch_async(dispatch_get_main_queue(), ^{
            Listener *l = me;
            if (!l) return;
            if (err) [l note:@"recognizer error %@ %ld: %@", err.domain, (long)err.code, err.localizedDescription];
            if (r && r.final) [l note:@"final result, %lu characters", (unsigned long)r.bestTranscription.formattedString.length];
            if (r) {
                l->_text = r.bestTranscription.formattedString ?: @"";
                if (!r.final && partial) partial(l->_text);
            }
            if ((r && r.final) || err) [l deliver];
        });
    }];
    // A new engine for every recording: an engine keeps the format of the input it first saw, and a tap in
    // a format that no longer matches (the input device changed meanwhile) silently gets nothing.
    _engine = [[AVAudioEngine alloc] init];
    AVAudioInputNode *input = _engine.inputNode;
    AudioDeviceID dev = pick_input(_mic); // chosen again each time: devices come and go
    _device = device_name(dev);
    _silentDevice = nil;
    AudioUnitSetProperty(input.audioUnit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &dev, sizeof dev);
    AVAudioFormat *hw = [input inputFormatForBus:0];  // what the device delivers
    AVAudioFormat *format = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:hw.sampleRate channels:1];
    if (!hw.sampleRate || !format) { [self note:@"input %@ has no usable format", _device]; [self cancel]; return NO; }
    SFSpeechAudioBufferRecognitionRequest *req = _request;
    void (^tap)(AVAudioPCMBuffer *, AVAudioTime *) = ^(AVAudioPCMBuffer *buf, AVAudioTime *when) {
        (void)when;
        [req appendAudioPCMBuffer:buf];
        Listener *l = me;
        if (!l || !buf.floatChannelData || !buf.frameLength) return;
        const float *x = buf.floatChannelData[0];
        float sum = 0;
        for (AVAudioFrameCount i = 0; i < buf.frameLength; i++) sum += x[i] * x[i];
        float db = 10 * log10f(sum / buf.frameLength + 1e-9f); // about -60 (quiet) .. -10 (loud)
        l->_level = fmaxf(0, fminf(1, (db + 55) / 40));
        l->_buffers++;
        if (db > l->_peak || l->_buffers == 1) l->_peak = db;
    };
    @try {
        [input installTapOnBus:0 bufferSize:1024 format:format block:tap];
    } @catch (NSException *e) { // the node refuses the format: let it pick its own
        [self note:@"tap refused %.0f Hz mono (%@); using the node's format", hw.sampleRate, e.reason];
        format = nil;
        @try { [input installTapOnBus:0 bufferSize:1024 format:nil block:tap]; }
        @catch (NSException *e2) { [self note:@"tap failed: %@", e2.reason]; [self cancel]; return NO; }
    }
    NSError *err = nil;
    [_engine prepare];
    [self note:@"input %@: device %.0f Hz %u ch, tap %@", _device, hw.sampleRate, (unsigned)hw.channelCount, format ? @"mono" : @"node format"];
    if (![_engine startAndReturnError:&err]) { [self note:@"engine failed: %@", err.localizedDescription]; [self cancel]; return NO; }
    return YES;
}

- (void)stopAudio {
    if (_engine.running) [_engine stop];
    [_engine.inputNode removeTapOnBus:0];
    _level = 0;
}

- (void)finish:(void (^)(NSString *))done {
    [self note:@"stopped after %u audio buffers, loudest %.0f dB", _buffers, _peak];
    if (_buffers > 5 && _peak < -80) _silentDevice = _device; // not one sound: the wrong device, or muted
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

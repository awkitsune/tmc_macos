#import "audiosession.h"

#import <AVFAudio/AVFAudio.h>
#import <UIKit/UIKit.h>

#include "audio.h"

static BOOL sActive;  // the session is active
static BOOL sPlaying; // the output unit was started
static BOOL sInterrupted;

double AudioSessionSampleRate(void) {
    return AVAudioSession.sharedInstance.sampleRate;
}

static BOOL Activate(BOOL active) {
    NSError* error = nil;
    AVAudioSessionSetActiveOptions options = active ? 0 : AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation;
    if (![AVAudioSession.sharedInstance setActive:active withOptions:options error:&error]) {
        NSLog(@"audio session: %@ failed: %@", active ? @"activating" : @"deactivating", error);
        return NO;
    }
    sActive = active;
    return YES;
}

static void Interrupted(NSNotification* note) {
    NSUInteger type = [note.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue];
    if (type == AVAudioSessionInterruptionTypeBegan) {
        NSLog(@"audio session: interrupted");
        sInterrupted = YES;
        sActive = NO;
        AudioSetRunning(false);
        return;
    }
    NSLog(@"audio session: interruption ended");
    sInterrupted = NO;
    if (UIApplication.sharedApplication.applicationState != UIApplicationStateBackground && Activate(YES)) {
        AudioSetRunning(true);
    }
}

static void RouteChanged(NSNotification* note) {
    double rate = AudioSessionSampleRate();
    NSUInteger reason = [note.userInfo[AVAudioSessionRouteChangeReasonKey] unsignedIntegerValue];
    NSLog(@"audio session: route changed (reason %lu), %.0f Hz", (unsigned long)reason, rate);
    if (sPlaying && sActive && rate >= 8000 && rate != AudioDeviceRate()) {
        AudioRestart();
    }
}

// Media services were reset (rare): everything has to be set up again.
static void ServicesReset(NSNotification* note) {
    NSLog(@"audio session: media services reset");
    AudioStop();
    sPlaying = NO;
    AudioSessionStart();
}

BOOL AudioSessionStart(void) {
    AVAudioSession* session = AVAudioSession.sharedInstance;
    NSError* error = nil;
    static dispatch_once_t once;

    dispatch_once(&once, ^{
        NSNotificationCenter* centre = NSNotificationCenter.defaultCenter;
        NSOperationQueue* main = NSOperationQueue.mainQueue;
        [centre addObserverForName:AVAudioSessionInterruptionNotification object:session queue:main usingBlock:^(NSNotification* note) {
            Interrupted(note);
        }];
        [centre addObserverForName:AVAudioSessionRouteChangeNotification object:session queue:main usingBlock:^(NSNotification* note) {
            RouteChanged(note);
        }];
        [centre addObserverForName:AVAudioSessionMediaServicesWereResetNotification object:session queue:main
                        usingBlock:^(NSNotification* note) {
                            ServicesReset(note);
                        }];
    });
    if (![session setCategory:AVAudioSessionCategoryPlayback mode:AVAudioSessionModeDefault options:0 error:&error]) {
        NSLog(@"audio session: setting the category failed: %@", error);
    }
    // Short buffers keep the sound close to the picture (the ring adds ~50 ms).
    [session setPreferredIOBufferDuration:0.01 error:nil];
    if (!Activate(YES)) {
        return NO;
    }
    sPlaying = AudioStart();
    return sPlaying;
}

void AudioSessionSetForeground(BOOL foreground) {
    if (!sPlaying) {
        return;
    }
    if (!foreground) {
        AudioSetRunning(false);
        Activate(NO);
    } else if (!sInterrupted && Activate(YES)) {
        if (AudioSessionSampleRate() != AudioDeviceRate()) {
            AudioRestart();
        } else {
            AudioSetRunning(true);
        }
    }
}

BOOL AudioSessionIsActive(void) {
    return sActive && sPlaying;
}

NSString* AudioSessionDescription(void) {
    AVAudioSession* session = AVAudioSession.sharedInstance;
    NSMutableArray<NSString*>* outputs = [NSMutableArray array];
    for (AVAudioSessionPortDescription* port in session.currentRoute.outputs) {
        [outputs addObject:port.portName];
    }
    return [NSString stringWithFormat:@"category %@, %.0f Hz (unit %.0f Hz), %.1f ms buffer, output %@", session.category,
                                      session.sampleRate, AudioDeviceRate(), session.IOBufferDuration * 1000,
                                      [outputs componentsJoinedByString:@", "]];
}

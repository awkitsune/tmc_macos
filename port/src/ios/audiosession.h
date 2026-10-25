#import <Foundation/Foundation.h>

/*
 * The app's AVAudioSession: the playback category (the game is heard with the
 * ring/silent switch on silent, like a video), then the output unit
 * (apple/audio.c) on top of it. An interruption (a call, Siri) stops the unit
 * and resumes it afterwards; a route change (headphones, AirPlay, a Bluetooth
 * speaker) can change the rate, so the unit is made again at the new one.
 */

// Set up and activate the session, then start the sound. NO if there is no
// sound (the game runs silently).
BOOL AudioSessionStart(void);
// The app goes to the background (NO) or comes back (YES).
void AudioSessionSetForeground(BOOL foreground);
// The session is active and the output unit plays (self-test).
BOOL AudioSessionIsActive(void);
// The session's category, output rate and route (self-test).
NSString* AudioSessionDescription(void);

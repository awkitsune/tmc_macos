#import <Foundation/Foundation.h>

#import "renderer.h"

NS_ASSUME_NONNULL_BEGIN

/*
 * Where the settings are kept: their NSUserDefaults keys and the filter knobs
 * (the shader parameters of FilterParams). The Settings window (macOS) and the
 * settings sheet (iOS) show the same settings.
 */

extern NSString* const kFilterDefault;
extern NSString* const kIntegerScalingDefault;
extern NSString* const kGamepadLayoutDefault;
extern NSString* const kGpuPpuDefault;
// LCD response (pixel ghosting) on or off; its times are filter knobs.
extern NSString* const kLcdResponseDefault;
// How the game keeps its frame rate: 0 accurate (59.73 Hz timer), 1 display sync.
extern NSString* const kPacingDefault;
// Posted when the pacing setting changes.
extern NSString* const kPacingChangedNotification;
// The Debug menu (save state tools); on by default in development builds.
extern NSString* const kDebugMenuDefault;
// Posted when the Debug menu setting changes, to rebuild the menus.
extern NSString* const kDebugMenuChangedNotification;
// The Debug button in the game window's toolbar; off by default.
extern NSString* const kDebugToolbarDefault;
// Posted when the Debug button setting changes, to update the toolbar.
extern NSString* const kDebugToolbarChangedNotification;

/* Every NSUserDefaults key the Settings window sets, one control each. */
NSArray<NSString*>* SettingsKeys(void);

/* One filter knob: a float of FilterParams with its range, shown as a slider. */
typedef struct {
    NSString* _Nullable section; // starts a new section when set
    NSString* title;
    NSString* key;
    size_t offset;               // into FilterParams
    float min, max;
    float step;                  // values snap to this (0: 0.01)
    NSString* _Nullable format;  // for the value label (nil: two decimals)
} Knob;

#define KNOB_COUNT 15
extern const Knob kKnobs[KNOB_COUNT];

float* KnobValue(FilterParams* params, const Knob* knob);
NSString* KnobText(const Knob* knob, float value);
// The value snapped to the knob's step; two decimals is as fine as the eye can tell.
float KnobSnap(const Knob* knob, float value);

/* Filter knobs saved in NSUserDefaults; missing values fall back to the defaults. */
FilterParams LoadFilterParams(void);

NS_ASSUME_NONNULL_END

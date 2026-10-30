#import "settingsstore.h"

#include <math.h>
#include <stddef.h>

NSString* const kFilterDefault = @"Filter";
NSString* const kIntegerScalingDefault = @"IntegerScaling";
NSString* const kGamepadLayoutDefault = @"GamepadLayout";
NSString* const kGpuPpuDefault = @"GpuPpu";
NSString* const kGpuRenderingDefault = @"GpuRendering";
NSString* const kLcdResponseDefault = @"LCDResponse";
NSString* const kPacingDefault = @"FramePacing";
NSString* const kPacingChangedNotification = @"TMCPacingChanged";
NSString* const kDebugMenuDefault = @"ShowDebugMenu";
NSString* const kDebugMenuChangedNotification = @"TMCDebugMenuChanged";
NSString* const kDebugToolbarDefault = @"ShowDebugToolbar";
NSString* const kDebugToolbarChangedNotification = @"TMCDebugToolbarChanged";

const Knob kKnobs[KNOB_COUNT] = {
    { @"LCD Grid", @"Gap strength", @"LCD.GridStrength", offsetof(FilterParams, lcdGridStrength), 0, 1 },
    { nil, @"Gap width", @"LCD.GridWidth", offsetof(FilterParams, lcdGridWidth), 0, 0.5f },
    { nil, @"Subpixel strength", @"LCD.Subpixel", offsetof(FilterParams, lcdSubpixel), 0, 1 },
    { nil, @"Gap bloom", @"LCD.GapBloom", offsetof(FilterParams, lcdGapBloom), 0, 1 },
    { nil, @"Brightness", @"LCD.Brightness", offsetof(FilterParams, lcdBrightness), 0.5f, 2 },
    { @"CRT Scanlines", @"Scanline strength", @"CRT.Scanlines", offsetof(FilterParams, crtScanlines), 0, 1 },
    { nil, @"Bloom", @"CRT.Bloom", offsetof(FilterParams, crtBloom), 0, 1 },
    { nil, @"Beam width (dark)", @"CRT.BeamMin", offsetof(FilterParams, crtBeamMin), 0.05f, 0.6f },
    { nil, @"Beam width (bright)", @"CRT.BeamMax", offsetof(FilterParams, crtBeamMax), 0.05f, 0.6f },
    { nil, @"Aperture grille", @"CRT.Mask", offsetof(FilterParams, crtMask), 0, 1 },
    { nil, @"Brightness", @"CRT.Brightness", offsetof(FilterParams, crtBrightness), 0.5f, 2 },
    { @"LCD Response (all filters)", @"Response time", @"LCD.ResponseMs", offsetof(FilterParams, lcdResponseMs), 0, 40, 1,
      @"%.0f ms" },
    { nil, @"Darkening slowdown", @"LCD.DarkSlowdown", offsetof(FilterParams, lcdDarkSlowdown), 1, 2, 0, @"%.2f×" },
    { @"Colour (all filters)", @"GBA colour correction", @"Color.Correction", offsetof(FilterParams, colorCorrection), 0, 1 },
    { nil, @"Gamma", @"Color.Gamma", offsetof(FilterParams, gamma), 0.5f, 2 },
};

float* KnobValue(FilterParams* params, const Knob* knob) {
    return (float*)((char*)params + knob->offset);
}

NSString* KnobText(const Knob* knob, float value) {
    return [NSString stringWithFormat:knob->format ?: @"%.2f", value];
}

float KnobSnap(const Knob* knob, float value) {
    float step = knob->step > 0 ? knob->step : 0.01f;
    return roundf(value / step) * step;
}

NSArray<NSString*>* SettingsKeys(void) {
    NSMutableArray* keys = [NSMutableArray arrayWithObjects:kFilterDefault, kIntegerScalingDefault, kLcdResponseDefault,
                                                            kPacingDefault, kGamepadLayoutDefault, kDebugMenuDefault,
                                                            kDebugToolbarDefault, nil];
    for (size_t i = 0; i < KNOB_COUNT; i++) {
        [keys addObject:kKnobs[i].key];
    }
    return keys;
}

NSDictionary<NSString*, NSNumber*>* LoadGpuRenderingSettings(void) {
    NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
    NSDictionary* saved = [defaults dictionaryForKey:kGpuRenderingDefault];
    return saved ?: @{ @"enabled": @([defaults boolForKey:kGpuPpuDefault]) };
}

FilterParams LoadFilterParams(void) {
    NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
    FilterParams params = FilterParamsDefaults();
    for (size_t i = 0; i < KNOB_COUNT; i++) {
        NSNumber* stored = [defaults objectForKey:kKnobs[i].key];
        if ([stored isKindOfClass:NSNumber.class]) {
            *KnobValue(&params, &kKnobs[i]) = fminf(fmaxf(stored.floatValue, kKnobs[i].min), kKnobs[i].max);
        }
    }
    return params;
}

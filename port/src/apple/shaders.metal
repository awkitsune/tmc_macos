// Presentation shaders: draw the 240x160 GBA frame into the viewport rect that
// renderer.m picked (integer or aspect-correct), with a selectable filter.
// Compiled at launch from the app bundle's Resources.

#include <metal_stdlib>
using namespace metal;

// Keep in sync with FilterMode in renderer.h.
enum : uint {
    FilterNearest = 0,
    FilterSharpBilinear = 1,
    FilterBilinear = 2,
    FilterLCD = 3,
    FilterCRT = 4,
    FilterSoftSubpixel = 5,
};

// Keep in sync with Uniforms in renderer.m and FilterParams in renderer.h.
struct Uniforms {
    float2 sourceSize; // texels
    float2 outputSize; // viewport pixels
    uint filter;
    float lcdGridStrength;  // how dark the gaps between pixels are (0-1)
    float lcdGridWidth;     // gap width as a fraction of a pixel (0-0.5)
    float lcdSubpixel;      // RGB stripe strength (0-1)
    float lcdBrightness;
    float crtScanlines;     // scanline strength (0-1)
    float crtMask;          // aperture-grille strength (0-1)
    float crtBrightness;
    float colorCorrection;  // 0 = raw GBA colours, 1 = GBA LCD colour response
    float gamma;            // output = colour ^ gamma
    float crtBloom;         // 0 = every beam the same width, 1 = width follows brightness fully
    float crtBeamMin;       // beam width (Gaussian sigma, in scanlines) on black
    float crtBeamMax;       // beam width on white
    float lcdGapBloom;      // how much bright pixels shrink their gaps (0-1)
    float lcdResponseMs;    // used by renderer.m for lcd_response, not here
    float lcdDarkSlowdown;
    float softSubpixel;     // Soft Subpixel: stripe and row-gap strength (0-1)
    float softGlow;         // glow of bright pixels (1 = as measured)
    float softGlowRadius;   // glow Gaussian sigma, in texels
    float softColour;       // colour mixing between channels (0-1)
    float softBrightness;   // drive into the soft white clip
    // Debugging (Shader Buffers window): which intermediate to output instead
    // of the picture (Stage*), and where the viewport sits in the drawable when
    // rendering into a texture of the viewport's size (for position-based patterns).
    uint debugStage;
    float2 positionOffset;
};

// Keep in sync with DebugBuffer in renderer.h (the stage buffers).
enum : uint {
    StageNone = 0,
    StageFilterRaw = 1,  // the filter's colour before colour correction and gamma
    StageBeams = 2,      // CRT: the summed scanline beams
    StageMask = 3,       // LCD: grid x subpixel factor; CRT: aperture grille factor; Soft: stripes x row gaps
    StageBloom = 4,      // R spot brightness, G beam width (scanlines) or gap (texels), B scanline strength;
                         // Soft Subpixel: the glow (linear light, per channel)
    StagePreGamma = 5,   // after colour correction, before gamma
};

// Keep in sync with ResponseUniforms in renderer.m.
struct ResponseUniforms {
    float keepRise; // share of the old picture left after one step, getting brighter
    float keepFall; // the same, getting darker
};

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

vertex VertexOut present_vertex(uint vid [[vertex_id]]) {
    // Full-viewport triangle strip; uv (0,0) is the frame's top-left.
    float2 corner = float2(vid & 1, vid >> 1);
    VertexOut out;
    out.position = float4(corner.x * 2.0 - 1.0, 1.0 - corner.y * 2.0, 0.0, 1.0);
    out.uv = corner;
    return out;
}

// LCD response (ghosting): the GBA's screen takes ~20 ms to change a pixel, so
// each new frame only partly replaces what was shown. Run once per new game
// frame, as a render pass over the 240x160 targets: history holds what the
// simulated LCD showed, in linear light; shown is the new picture,
// gamma-encoded like the frame, for the present pass. Each channel follows its
// own target, darker targets with keepFall. (A compute kernel would do, but the
// first compute encoder costs ~35 MB of driver memory for the app's lifetime.)
struct ResponseOut {
    float4 nextHistory [[color(0)]];
    float4 shown [[color(1)]];
};

fragment ResponseOut lcd_response(VertexOut in [[stage_in]],
                                  texture2d<float, access::read> frame [[texture(0)]],
                                  texture2d<float, access::read> history [[texture(1)]],
                                  constant ResponseUniforms& u [[buffer(0)]]) {
    uint2 gid = uint2(in.position.xy);
    float3 target = pow(frame.read(gid).rgb, 2.2);
    float3 old = history.read(gid).rgb;
    float3 keep = select(float3(u.keepRise), float3(u.keepFall), target < old);
    // keep == 0 (a restart) copies the frame in without reading old at all:
    // render targets start out as garbage, and NaN * 0 is still NaN.
    float3 now = select(mix(target, old, keep), target, keep == 0.0);
    return { float4(now, 1.0), float4(pow(now, 1.0 / 2.2), 1.0) };
}

constexpr sampler nearestSampler(coord::normalized, address::clamp_to_edge, filter::nearest);
constexpr sampler linearSampler(coord::normalized, address::clamp_to_edge, filter::linear);

// Bilinear only across texel edges: crisp at any scale, without the uneven pixel
// widths of nearest at non-integer scales.
static float3 sharpBilinear(texture2d<float> frame, float2 uv, constant Uniforms& u) {
    float2 texel = uv * u.sourceSize;
    float2 scale = max(floor(u.outputSize / u.sourceSize), 1.0);
    float2 region = 0.5 - 0.5 / scale;
    float2 centre = fract(texel) - 0.5;
    float2 offset = (centre - clamp(centre, -region, region)) * scale + 0.5;
    return frame.sample(linearSampler, (floor(texel) + offset) / u.sourceSize).rgb;
}

// Fraction of [x - width/2, x + width/2] covered by the band [a, b] repeated
// every unit (one texel). This is the band box-filtered over one output pixel,
// so every repeating pattern averages to exactly its duty cycle at any scale:
// stripes of equal width stay neutral instead of favouring the middle one.
static float bandIntegral(float x, float a, float b) {
    return floor(x) * (b - a) + clamp(fract(x), a, b) - a;
}

static float bandCoverage(float x, float width, float a, float b) {
    return (bandIntegral(x + width * 0.5, a, b) - bandIntegral(x - width * 0.5, a, b)) / width;
}

// The colour response of the GBA's own screen (after Pokefan531's gba-color
// matrix): each row sums to 1, so greys stay grey and only saturation shifts.
static float3 gbaColour(float3 c, float amount) {
    if (amount <= 0.0) {
        return c;
    }
    float3 lin = pow(c, 2.2);
    float3 mixed = lin.r * float3(0.82, 0.125, 0.195) + lin.g * float3(0.24, 0.665, 0.075) + lin.b * float3(-0.06, 0.21, 0.73);
    float3 corrected = pow(saturate(mixed * 0.94), 1.0 / 2.2);
    return mix(c, corrected, amount);
}

// Brightness that drives beam and gap size: halfway between luma and the
// brightest channel, so saturated blues and reds still count as bright. One
// value for all three channels, so widths never differ per colour (no fringes).
static float spotBrightness(float3 c) {
    return 0.5 * (dot(c, float3(0.2126, 0.7152, 0.0722)) + max(c.r, max(c.g, c.b)));
}

// Colour of one scanline (texel row) at this x, sampled at the row centre so
// rows never blend vertically; the beams do that.
static float3 scanlineColour(texture2d<float> frame, float x, float row, constant Uniforms& u) {
    return sharpBilinear(frame, float2(x, (row + 0.5) / u.sourceSize.y), u);
}

// A scanline's beam at distance d (in scanlines) from its centre. Its width
// grows with the line's brightness by crtBloom, like a real electron beam.
static float beamWidth(float3 colour, constant Uniforms& u) {
    float fixedWidth = 0.5 * (u.crtBeamMin + u.crtBeamMax);
    return mix(fixedWidth, mix(u.crtBeamMin, u.crtBeamMax, spotBrightness(colour)), u.crtBloom);
}

static float3 beam(float3 colour, float d, constant Uniforms& u) {
    float width = beamWidth(colour, u);
    return colour * exp(-(d * d) / (2.0 * width * width));
}

static float3 lcd(texture2d<float> frame, float2 uv, constant Uniforms& u) {
    float2 texel = uv * u.sourceSize;
    float2 footprint = u.sourceSize / u.outputSize; // texels per output pixel
    float3 source = gbaColour(frame.sample(nearestSampler, uv).rgb, u.colorCorrection);

    // Gaps of lcdGridWidth centred on the pixel edges, dimmed by lcdGridStrength;
    // with lcdGapBloom, bright pixels shrink their gaps.
    float gap = u.lcdGridWidth * 0.5 * (1.0 - u.lcdGapBloom * spotBrightness(source));
    float insideX = bandCoverage(texel.x, footprint.x, gap, 1.0 - gap);
    float insideY = bandCoverage(texel.y, footprint.y, gap, 1.0 - gap);
    float grid = 1.0 - u.lcdGridStrength * (1.0 - insideX * insideY);

    // Subpixels: the lit part of the pixel (between the gaps) is split into equal
    // R, G, B thirds; each passes its channel fully and dims the other two by
    // lcdSubpixel. Keeping the stripes clear of the gap means the gap takes the
    // same share from every channel, so greys stay grey.
    float third = (1.0 - 2.0 * gap) / 3.0;
    float3 lit = float3(bandCoverage(texel.x, footprint.x, gap, gap + third),
                        bandCoverage(texel.x, footprint.x, gap + third, gap + 2.0 * third),
                        bandCoverage(texel.x, footprint.x, gap + 2.0 * third, 1.0 - gap));
    // The gap itself is left to the grid term, the same for every channel.
    float3 mask = 1.0 - u.lcdSubpixel * (insideX - lit);

    return source * mask * grid * u.lcdBrightness;
}

// Debugging: lcd()'s mask (grid x subpixel) and gap, worked out the same way.
// Kept apart so the picture's own arithmetic is exactly lcd()'s.
static void lcdDebug(texture2d<float> frame, float2 uv, constant Uniforms& u, thread float3& mask, thread float3& bloom) {
    float2 texel = uv * u.sourceSize;
    float2 footprint = u.sourceSize / u.outputSize;
    float3 source = gbaColour(frame.sample(nearestSampler, uv).rgb, u.colorCorrection);
    float gap = u.lcdGridWidth * 0.5 * (1.0 - u.lcdGapBloom * spotBrightness(source));
    float insideX = bandCoverage(texel.x, footprint.x, gap, 1.0 - gap);
    float insideY = bandCoverage(texel.y, footprint.y, gap, 1.0 - gap);
    float grid = 1.0 - u.lcdGridStrength * (1.0 - insideX * insideY);
    float third = (1.0 - 2.0 * gap) / 3.0;
    float3 lit = float3(bandCoverage(texel.x, footprint.x, gap, gap + third),
                        bandCoverage(texel.x, footprint.x, gap + third, gap + 2.0 * third),
                        bandCoverage(texel.x, footprint.x, gap + 2.0 * third, 1.0 - gap));
    mask = (1.0 - u.lcdSubpixel * (insideX - lit)) * grid;
    bloom = float3(spotBrightness(source), gap, 0.0);
}

// Soft Subpixel: after the look of Nintendo Switch Online's GBA LCD filter.
// The constants were fitted to a 4x capture of NSO (1280x720, file select
// screen) against the same frame from the headless build: 4x4 profiles of 19
// flat colours, edges and white; 4.7/255 median error. What NSO does there:
//   - colour: a steeper tone curve (gamma ~3.7 in) and some mixing between
//     channels (a green pixel lights the red and blue stripes a little);
//     blacks stay black (darkest blocks of the capture ~2/255).
//   - stripes: B, G, R from left to right in each pixel, each ~0.38 wide
//     with soft edges, a faint floor between them, and a thin dark line
//     between rows.
//   - drive: everything x2.2 into a soft clip just below white, so bright
//     pixels fill out into solid "pillows" while dark ones keep their stripes;
//   - glow: light above ~0.6 spreads over the pixel and a little past it.
// Light (linear) is worked out per channel from the 3x3 texels around, so
// stripes and glow blend into the neighbours, at any scale (the output
// pixel's own footprint widens each soft edge).
constant float kSoftGammaIn = 3.68;   // tone curve: source^this
constant float kSoftDrive = 2.19;     // gain into the clip
constant float kSoftGlowFrom = 0.609; // linear light above this glows
constant float kSoftGlowGain = 1.117;
constant float kSoftGlowFlat = 0.0064; // a faint glow of all light
constant float kSoftKnee = 0.673;     // soft clip starts at knee x ceiling
constant float kSoftCeiling = 0.963;  // white, linear (~0.98 encoded)
constant float3 kSoftStripeCentre = float3(0.757, 0.447, 0.155); // R, G, B; B G R left to right
constant float kSoftStripeHalf = 0.190;
constant float kSoftStripeEdge = 0.0745; // Gaussian sigma of the stripe edges, texels
constant float kSoftFloor = 0.1015;      // light between the stripes
constant float kSoftRowGap = 0.769;      // depth of the line between rows
constant float kSoftRowGapWidth = 0.0574;
// Rows: how much of each source channel goes into R, G, B (linear light).
constant float3x3 kSoftColour = float3x3(float3(0.9507, 0.0871, 0.0914),  // from R
                                         float3(0.1343, 0.9907, 0.1461),  // from G
                                         float3(0.0090, 0.0107, 0.8842)); // from B

// erf, Abramowitz & Stegun 7.1.26 (error < 1.5e-7); Metal has none.
static float softErf(float x) {
    float t = 1.0 / (1.0 + 0.3275911 * abs(x));
    float y = 1.0 - (((((1.061405429 * t - 1.453152027) * t) + 1.421413741) * t - 0.284496736) * t + 0.254829592) * t * exp(-x * x);
    return copysign(y, x);
}

static float3 softErf3(float3 x) {
    return float3(softErf(x.x), softErf(x.y), softErf(x.z));
}

// The band [a, b] blurred by a Gaussian of sigma s, at x.
static float softBand(float x, float a, float b, float s) {
    float q = 1.0 / (sqrt(2.0) * s);
    return 0.5 * (softErf((x - a) * q) - softErf((x - b) * q));
}

static float3 softBand3(float3 x, float3 a, float3 b, float s) {
    float q = 1.0 / (sqrt(2.0) * s);
    return 0.5 * (softErf3((x - a) * q) - softErf3((x - b) * q));
}

// One texel's light (linear, before the stripes).
static float3 softLight(texture2d<float> frame, int2 texel, constant Uniforms& u) {
    int2 size = int2(u.sourceSize);
    float3 c = frame.read(uint2(clamp(texel, int2(0), size - 1))).rgb;
    float3 lin = pow(gbaColour(c, u.colorCorrection), kSoftGammaIn);
    return mix(lin, kSoftColour * lin, u.softColour);
}

static float3 softClip(float3 x) {
    float knee = kSoftKnee * kSoftCeiling, room = kSoftCeiling - kSoftKnee * kSoftCeiling;
    return select(x, knee + room * (1.0 - exp(-(x - knee) / room)), x > knee);
}

static float3 softSubpixel(texture2d<float> frame, float2 uv, constant Uniforms& u, thread float3& mask, thread float3& glow) {
    float2 texel = uv * u.sourceSize;
    float2 footprint = u.sourceSize / u.outputSize;
    int2 base = int2(floor(texel));
    float2 f = texel - float2(base);
    // A box over the output pixel, folded into each Gaussian (variance w^2/12).
    float2 edge = sqrt(kSoftStripeEdge * kSoftStripeEdge + footprint * footprint / 12.0);
    float2 glowSigma = sqrt(u.softGlowRadius * u.softGlowRadius + footprint * footprint / 12.0);

    // Stripes along the row: each of the three texels lights its own stripes,
    // which reach over its edges into the neighbours.
    float3 light[3][3];
    for (int l = 0; l < 3; l++) {
        for (int k = 0; k < 3; k++) {
            light[l][k] = softLight(frame, base + int2(k - 1, l - 1), u);
        }
    }
    float3 stripes = 0.0, flat = 0.0, glowSum = 0.0;
    float floorShare = kSoftFloor, stripeShare = 1.0 - kSoftFloor;
    for (int k = -1; k <= 1; k++) {
        float x = f.x - float(k);
        float box = softBand(x, 0.0, 1.0, edge.x);
        float3 stripe = softBand3(float3(x), kSoftStripeCentre - kSoftStripeHalf, kSoftStripeCentre + kSoftStripeHalf, edge.x);
        stripes += light[1][k + 1] * (floorShare * box + stripeShare * stripe);
        flat += light[1][k + 1] * box;
    }
    // Glow: the light above kSoftGlowFrom, blurred over the 3x3 texels.
    for (int l = -1; l <= 1; l++) {
        float gy = softBand(f.y - float(l), 0.0, 1.0, glowSigma.y);
        for (int k = -1; k <= 1; k++) {
            float gx = softBand(f.x - float(k), 0.0, 1.0, glowSigma.x);
            float3 c = light[l + 1][k + 1];
            glowSum += (kSoftGlowGain * max(c - kSoftGlowFrom, 0.0) + kSoftGlowFlat * c) * (gx * gy);
        }
    }
    // The dark line between rows (its area kept as the footprint widens it).
    float rowSigma = sqrt(kSoftRowGapWidth * kSoftRowGapWidth + footprint.y * footprint.y / 12.0);
    float dy = min(f.y, 1.0 - f.y);
    float rows = 1.0 - kSoftRowGap * (kSoftRowGapWidth / rowSigma) * exp(-dy * dy / (2.0 * rowSigma * rowSigma));
    rows = mix(1.0, rows, u.softSubpixel);

    // Stripes normalised to their average, so subpixel strength keeps the brightness.
    float average = kSoftFloor + (1.0 - kSoftFloor) * 2.0 * kSoftStripeHalf;
    float3 lit = kSoftDrive * u.softBrightness * average * mix(flat, stripes / average, u.softSubpixel) * rows;
    glow = glowSum * u.softGlow;
    mask = select(float3(1.0), mix(1.0, stripes / (average * flat), u.softSubpixel) * rows, flat > 1e-6);
    return pow(max(softClip(lit + glow), 0.0), 1.0 / 2.2);
}

fragment float4 present_fragment(VertexOut in [[stage_in]],
                                 texture2d<float> frame [[texture(0)]],
                                 constant Uniforms& u [[buffer(0)]]) {
    float2 texel = in.uv * u.sourceSize;
    float2 cell = fract(texel);
    float2 pixelsPerTexel = u.outputSize / u.sourceSize;
    float2 position = in.position.xy + u.positionOffset;
    float3 colour;
    float3 debugMask = float3(1.0), debugBeams = float3(0.0), debugBloom = float3(0.0);

    switch (u.filter) {
    case FilterSharpBilinear:
        colour = sharpBilinear(frame, in.uv, u);
        break;
    case FilterBilinear:
        colour = frame.sample(linearSampler, in.uv).rgb;
        break;
    case FilterLCD:
        colour = lcd(frame, in.uv, u);
        if (u.debugStage == StageMask || u.debugStage == StageBloom) {
            lcdDebug(frame, in.uv, u, debugMask, debugBloom);
        }
        break;
    case FilterSoftSubpixel:
        colour = softSubpixel(frame, in.uv, u, debugMask, debugBloom);
        break;
    case FilterCRT: {
        // Scanlines: each texel row is a horizontal beam; this pixel adds up the
        // beams of its own row and the rows above and below, each with its own
        // width, so bright lines spread into their darker neighbours.
        float row = floor(texel.y);
        float d = cell.y - 0.5;
        float3 source = scanlineColour(frame, in.uv.x, row, u);
        float3 lines = beam(source, d, u) +
                       beam(scanlineColour(frame, in.uv.x, max(row - 1.0, 0.0), u), d + 1.0, u) +
                       beam(scanlineColour(frame, in.uv.x, min(row + 1.0, u.sourceSize.y - 1.0), u), d - 1.0, u);
        // Scanlines fade out where there are too few output rows per texel row.
        float strength = saturate((pixelsPerTexel.y - 1.5) / 2.0) * u.crtScanlines;
        colour = mix(source, lines * u.crtBrightness, strength);
        // Aperture grille: R, G, B columns one output pixel wide (neutral on average).
        float column = fmod(floor(position.x), 3.0);
        float3 grille = float3(column == 0.0, column == 1.0, column == 2.0);
        colour *= 1.0 - u.crtMask * (1.0 - grille);
        debugBeams = lines;
        debugMask = 1.0 - u.crtMask * (1.0 - grille);
        debugBloom = float3(spotBrightness(source), beamWidth(source, u), strength);
        break;
    }
    case FilterNearest:
    default:
        colour = frame.sample(nearestSampler, in.uv).rgb;
        break;
    }
    switch (u.debugStage) {
    case StageFilterRaw:
        return float4(colour, 1.0);
    case StageBeams:
        return float4(debugBeams, 1.0);
    case StageMask:
        return float4(debugMask, 1.0);
    case StageBloom:
        return float4(debugBloom, 1.0);
    default:
        break;
    }
    if (u.filter != FilterLCD && u.filter != FilterSoftSubpixel) {
        colour = gbaColour(saturate(colour), u.colorCorrection);
    }
    if (u.debugStage == StagePreGamma) {
        return float4(saturate(colour), 1.0);
    }
    return float4(pow(saturate(colour), u.gamma), 1.0);
}

// Debugging: an outline around a rectangle of the frame (a sprite picked in the
// Sprites window), drawn over the picture. rect is in source texels.
struct OverlayUniforms {
    float4 rect;       // x, y, width, height in texels
    float2 sourceSize;
    float2 outputSize; // viewport pixels
    float4 colour;
};

struct OverlayOut {
    float4 position [[position]];
    float2 local; // output pixels from the outline's top-left
};

vertex OverlayOut overlay_vertex(uint vid [[vertex_id]], constant OverlayUniforms& u [[buffer(0)]]) {
    float2 scale = u.outputSize / u.sourceSize;
    float pad = 2.0; // outline thickness in output pixels, drawn just outside the rect
    float2 corner = float2(vid & 1, vid >> 1);
    float2 size = u.rect.zw * scale + 2.0 * pad;
    float2 pixel = u.rect.xy * scale - pad + corner * size;
    OverlayOut out;
    out.position = float4(pixel.x / u.outputSize.x * 2.0 - 1.0, 1.0 - pixel.y / u.outputSize.y * 2.0, 0.0, 1.0);
    out.local = corner * size;
    return out;
}

fragment float4 overlay_fragment(OverlayOut in [[stage_in]], constant OverlayUniforms& u [[buffer(0)]]) {
    float2 scale = u.outputSize / u.sourceSize;
    float2 size = u.rect.zw * scale + 4.0;
    if (in.local.x > 2.0 && in.local.y > 2.0 && in.local.x < size.x - 2.0 && in.local.y < size.y - 2.0) {
        discard_fragment();
    }
    return u.colour;
}

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
    StageMask = 3,       // LCD: grid x subpixel factor; CRT: aperture grille factor
    StageBloom = 4,      // R spot brightness, G beam width (scanlines) or gap (texels), B scanline strength
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
    if (u.filter != FilterLCD) {
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

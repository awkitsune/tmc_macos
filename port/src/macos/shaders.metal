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
static float3 beam(float3 colour, float d, constant Uniforms& u) {
    float fixedWidth = 0.5 * (u.crtBeamMin + u.crtBeamMax);
    float width = mix(fixedWidth, mix(u.crtBeamMin, u.crtBeamMax, spotBrightness(colour)), u.crtBloom);
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

fragment float4 present_fragment(VertexOut in [[stage_in]],
                                 texture2d<float> frame [[texture(0)]],
                                 constant Uniforms& u [[buffer(0)]]) {
    float2 texel = in.uv * u.sourceSize;
    float2 cell = fract(texel);
    float2 pixelsPerTexel = u.outputSize / u.sourceSize;
    float3 colour;

    switch (u.filter) {
    case FilterSharpBilinear:
        colour = sharpBilinear(frame, in.uv, u);
        break;
    case FilterBilinear:
        colour = frame.sample(linearSampler, in.uv).rgb;
        break;
    case FilterLCD:
        colour = lcd(frame, in.uv, u);
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
        float column = fmod(floor(in.position.x), 3.0);
        float3 grille = float3(column == 0.0, column == 1.0, column == 2.0);
        colour *= 1.0 - u.crtMask * (1.0 - grille);
        break;
    }
    case FilterNearest:
    default:
        colour = frame.sample(nearestSampler, in.uv).rgb;
        break;
    }
    if (u.filter != FilterLCD) {
        colour = gbaColour(saturate(colour), u.colorCorrection);
    }
    return float4(pow(saturate(colour), u.gamma), 1.0);
}

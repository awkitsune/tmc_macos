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

struct Uniforms {
    float2 sourceSize; // texels
    float2 outputSize; // viewport pixels
    uint filter;
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

// Pixel coverage of [a, b] within the unit cell, smoothed by one output pixel.
static float band(float x, float a, float b, float soft) {
    return smoothstep(a - soft, a + soft, x) * (1.0 - smoothstep(b - soft, b + soft, x));
}

fragment float4 present_fragment(VertexOut in [[stage_in]],
                                 texture2d<float> frame [[texture(0)]],
                                 constant Uniforms& u [[buffer(0)]]) {
    float2 texel = in.uv * u.sourceSize;
    float2 cell = fract(texel);
    float2 pixelsPerTexel = u.outputSize / u.sourceSize;
    float2 soft = 0.5 / pixelsPerTexel;
    float3 colour;

    switch (u.filter) {
    case FilterSharpBilinear:
        colour = sharpBilinear(frame, in.uv, u);
        break;
    case FilterBilinear:
        colour = frame.sample(linearSampler, in.uv).rgb;
        break;
    case FilterLCD: {
        // GBA-style LCD: RGB subpixel stripes and a faint grid between pixels.
        float3 source = frame.sample(nearestSampler, in.uv).rgb;
        float3 stripes = float3(band(cell.x, 0.0, 1.0 / 3.0, soft.x),
                                band(cell.x, 1.0 / 3.0, 2.0 / 3.0, soft.x),
                                band(cell.x, 2.0 / 3.0, 1.0, soft.x));
        float3 mask = mix(float3(1.0), 0.45 + 0.75 * stripes, saturate(pixelsPerTexel.x / 4.0));
        float grid = band(cell.y, 0.08, 0.92, soft.y) * band(cell.x, 0.04, 0.96, soft.x);
        grid = mix(1.0, 0.75 + 0.25 * grid, saturate(pixelsPerTexel.y / 3.0));
        colour = source * mask * grid * 1.1;
        break;
    }
    case FilterCRT: {
        // Scanlines: each texel row is a beam whose width grows with brightness.
        float3 source = sharpBilinear(frame, in.uv, u);
        float3 above = sharpBilinear(frame, in.uv - float2(0.0, 1.0 / u.sourceSize.y), u);
        float3 below = sharpBilinear(frame, in.uv + float2(0.0, 1.0 / u.sourceSize.y), u);
        float d = cell.y - 0.5;
        float3 width = mix(float3(0.22), float3(0.36), source);
        float3 beam = exp(-(d * d) / (2.0 * width * width));
        float3 bleed = (d < 0.0 ? above : below) * exp(-((1.0 - abs(d)) * (1.0 - abs(d))) / 0.12) * 0.25;
        float strength = saturate((pixelsPerTexel.y - 1.5) / 2.0);
        colour = mix(source, source * beam * 1.35 + bleed, strength);
        // Slight aperture-grille tint every output pixel column.
        float column = fmod(in.position.x, 3.0);
        float3 grille = column < 1.0 ? float3(1.0, 0.9, 0.9) : column < 2.0 ? float3(0.9, 1.0, 0.9) : float3(0.9, 0.9, 1.0);
        colour *= mix(float3(1.0), grille, strength);
        break;
    }
    case FilterNearest:
    default:
        colour = frame.sample(nearestSampler, in.uv).rgb;
        break;
    }
    return float4(saturate(colour), 1.0);
}

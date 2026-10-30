#include <metal_stdlib>
using namespace metal;

// Must match PpuCaptureLine in port/ppucapture.h.
struct Line {
    uchar vram[0x18000], palette[0x400], oam[0x400], regs[0x58];
    int refX[2], refY[2], mosaicRefX[2], mosaicRefY[2];
    uint mosaicBgY, mosaicObjY, layerMask;
};

static uint word(device const uchar* p, uint at) { return p[at] | (uint(p[at + 1]) << 8); }
static uint reg(device const Line& l, uint at) { return word(l.regs, at); }
static uint pal(device const Line& l, uint i) { return word(l.palette, i * 2) & 0x7fff; }
static uint texel(device const Line& l, uint base, int x, int y, bool bpp8, uint limit) {
    uint at = base + y * (bpp8 ? 8 : 4) + (bpp8 ? x : x / 2);
    if (at >= limit) return 0;
    uint v = l.vram[at];
    return bpp8 ? v : ((x & 1) ? v >> 4 : v & 15);
}
static uint background(device const Line& l, int bg, int sx, int line, int mode) {
    uint cnt = reg(l, 8 + bg * 2), charBase = ((cnt >> 2) & 3) * 0x4000;
    uint screenBase = ((cnt >> 8) & 31) * 0x800;
    bool mosaic = cnt & 0x40;
    uint mos = reg(l, 0x4c);
    if (mosaic) sx -= sx % ((mos & 15) + 1);
    if ((mode == 1 && bg == 2) || mode == 2) {
        int i = bg - 2, r = bg == 2 ? 0x20 : 0x30, size = 128 << (cnt >> 14);
        int rx = mosaic ? l.mosaicRefX[i] : l.refX[i];
        int ry = mosaic ? l.mosaicRefY[i] : l.refY[i];
        int x = (rx + int(short(reg(l, r))) * sx) >> 8;
        int y = (ry + int(short(reg(l, r + 4))) * sx) >> 8;
        if (cnt & 0x2000) { x &= size - 1; y &= size - 1; }
        else if (x < 0 || y < 0 || x >= size || y >= size) return 0xffff;
        uint tile = l.vram[(screenBase + (y >> 3) * (size >> 3) + (x >> 3)) & 0xffff];
        uint index = texel(l, charBase + tile * 64, x & 7, y & 7, true, 0x10000);
        return index ? pal(l, index) : 0xffff;
    }
    int w = (cnt & 0x4000) ? 512 : 256, h = (cnt & 0x8000) ? 512 : 256;
    int x = (sx + (reg(l, 0x10 + bg * 4) & 511)) & (w - 1);
    int y = (line - (mosaic ? int(l.mosaicBgY) : 0) + (reg(l, 0x12 + bg * 4) & 511)) & (h - 1);
    uint block = (x >= 256 ? 1 : 0) + (y >= 256 ? (w == 512 ? 2 : 1) : 0);
    uint entry = word(l.vram, screenBase + block * 0x800 + (((y & 255) >> 3) * 32 + ((x & 255) >> 3)) * 2);
    int tx = x & 7, ty = y & 7;
    if (entry & 0x400) tx = 7 - tx;
    if (entry & 0x800) ty = 7 - ty;
    bool bpp8 = cnt & 0x80;
    uint index = texel(l, charBase + (entry & 0x3ff) * (bpp8 ? 64 : 32), tx, ty, bpp8, 0x10000);
    return index ? pal(l, bpp8 ? index : ((entry >> 12) * 16 + index)) : 0xffff;
}

struct Obj { uint color, priority, semi, window; };
static Obj sprite(device const Line& l, int x, int line, uint display) {
    constexpr uchar widths[12] = {8,16,32,64,16,32,32,64,8,8,16,32};
    constexpr uchar heights[12] = {8,16,32,64,8,8,16,32,16,32,32,64};
    Obj result = {0xffff, 4, 0, 0};
    for (int n = 127; n >= 0; n--) {
        uint at = n * 8, a0 = word(l.oam, at), a1 = word(l.oam, at + 2), a2 = word(l.oam, at + 4);
        bool affine = a0 & 0x100, doubleOrHidden = a0 & 0x200, bpp8 = a0 & 0x2000;
        int mode = (a0 >> 10) & 3, shape = a0 >> 14, size = a1 >> 14;
        if ((!affine && doubleOrHidden) || shape == 3 || mode == 3) continue;
        int w = widths[shape * 4 + size], h = heights[shape * 4 + size];
        int bw = w << (affine && doubleOrHidden), bh = h << (affine && doubleOrHidden);
        int top = a0 & 255; if (top + bh > 256) top -= 256;
        int left = a1 & 511; if (left >= 240) left -= 512;
        int bx = x - left, by = line - top;
        if (bx < 0 || bx >= bw || by < 0 || by >= bh) continue;
        if (a0 & 0x1000) {
            by = max(0, by - int(l.mosaicObjY));
            bx = max(0, bx - x % int(((reg(l, 0x4c) >> 8) & 15) + 1));
        }
        int tx, ty;
        if (affine) {
            uint p = ((a1 >> 9) & 31) * 32;
            int pa = short(word(l.oam, p + 6)), pb = short(word(l.oam, p + 14));
            int pc = short(word(l.oam, p + 22)), pd = short(word(l.oam, p + 30));
            int cx = bx - bw / 2, cy = by - bh / 2;
            tx = ((pa * cx + pb * cy) >> 8) + w / 2;
            ty = ((pc * cx + pd * cy) >> 8) + h / 2;
            if (tx < 0 || ty < 0 || tx >= w || ty >= h) continue;
        } else {
            tx = (a1 & 0x1000) ? w - 1 - bx : bx;
            ty = (a1 & 0x2000) ? h - 1 - by : by;
        }
        uint tile = a2 & 0x3ff;
        tile += (display & 0x40) ? ((ty >> 3) * (w >> 3) + (tx >> 3)) * (bpp8 ? 2 : 1)
                                 : (ty >> 3) * 32 + (tx >> 3) * (bpp8 ? 2 : 1);
        uint index = texel(l, 0x10000 + (tile & 0x3ff) * 32, tx & 7, ty & 7, bpp8, 0x18000);
        if (!index) continue;
        if (mode == 2) { result.window = 1; continue; }
        uint priority = (a2 >> 10) & 3;
        if (priority <= result.priority) {
            result.color = pal(l, 0x100 + (bpp8 ? index : (a2 >> 12) * 16 + index));
            result.priority = priority;
            result.semi = mode == 1;
        }
    }
    if (!(l.layerMask & 16)) result.color = 0xffff;
    return result;
}
static uint blend(uint a, uint b, uint eva, uint evb) {
    uint result = 0;
    for (int c = 0; c < 3; c++) result |= min(31u, (((a >> (c*5)) & 31) * eva + ((b >> (c*5)) & 31) * evb) >> 4) << (c*5);
    return result;
}
static uint brightness(uint a, uint y, bool up) {
    uint result = 0;
    for (int c = 0; c < 3; c++) {
        uint v = (a >> (c*5)) & 31;
        v = up ? v + (((31 - v) * y) >> 4) : v - ((v * y) >> 4);
        result |= v << (c*5);
    }
    return result;
}
static ushort gpu_color(device const Line* lines, uint2 pos) {
    device const Line& l = lines[pos.y];
    uint display = reg(l, 0), mode = display & 7;
    if (display & 0x80) return 0x7fff;
    uint colors[4], priority[4];
    for (int bg = 0; bg < 4; bg++) {
        bool enabled = (display & (0x100 << bg)) && (l.layerMask & (1 << bg)) &&
                       !(mode == 1 && bg == 3) && !(mode == 2 && bg < 2) && mode <= 2;
        colors[bg] = enabled ? background(l, bg, pos.x, pos.y, mode) : 0xffff;
        priority[bg] = reg(l, 8 + bg*2) & 3;
    }
    Obj obj = {0xffff, 4, 0, 0};
    if (display & 0x1000) obj = sprite(l, pos.x, pos.y, display);
    uint enable = 0x3f;
    if (display & 0xe000) {
        uint in = reg(l, 0x48), out = reg(l, 0x4a);
        enable = out & 0x3f;
        if ((display & 0x8000) && obj.window) enable = (out >> 8) & 0x3f;
        for (int w = 1; w >= 0; w--) if (display & (0x2000 << w)) {
            uint h = reg(l, 0x40 + w*2), v = reg(l, 0x44 + w*2);
            int x1 = h >> 8, x2 = h & 255, y1 = v >> 8, y2 = v & 255;
            bool inX = x1 <= x2 ? (int(pos.x) >= x1 && int(pos.x) < x2) : (int(pos.x) >= x1 || int(pos.x) < x2);
            bool inY = y1 <= y2 ? (int(pos.y) >= y1 && int(pos.y) < y2) : (int(pos.y) >= y1 || int(pos.y) < y2);
            if (inX && inY) enable = (in >> (w*8)) & 0x3f;
        }
    }
    uint top = 5, second = 5, topColor = pal(l, 0), secondColor = topColor;
    for (uint pr = 0; pr < 4 && second == 5; pr++) {
        if ((enable & 16) && obj.color != 0xffff && obj.priority == pr) {
            if (top == 5) { top = 4; topColor = obj.color; }
            else { second = 4; secondColor = obj.color; }
        }
        for (uint bg = 0; bg < 4 && second == 5; bg++) {
            if (!(enable & (1 << bg)) || colors[bg] == 0xffff || priority[bg] != pr) continue;
            if (top == 5) { top = bg; topColor = colors[bg]; }
            else { second = bg; secondColor = colors[bg]; }
        }
    }
    uint bld = reg(l, 0x50), alpha = reg(l, 0x52), effect = (bld >> 6) & 3;
    uint eva = min(alpha & 31, 16u), evb = min((alpha >> 8) & 31, 16u), evy = min(reg(l, 0x54) & 31, 16u);
    uint color = topColor;
    if (top == 4 && obj.semi && (bld & (0x100 << second))) color = blend(topColor, secondColor, eva, evb);
    else if ((enable & 0x20) && (bld & (1 << top))) {
        if (effect == 1 && (bld & (0x100 << second))) color = blend(topColor, secondColor, eva, evb);
        else if (effect == 2 || effect == 3) color = brightness(topColor, evy, effect == 2);
    }
    return ushort(color);
}

kernel void gpu_ppu(device const Line* lines [[buffer(0)]], device ushort* output [[buffer(1)]],
                    uint2 pos [[thread_position_in_grid]]) {
    if (pos.x < 240 && pos.y < 160) output[pos.y * 240 + pos.x] = gpu_color(lines, pos);
}

kernel void gpu_ppu_texture(device const Line* lines [[buffer(0)]],
                            texture2d<float, access::write> output [[texture(0)]],
                            uint2 pos [[thread_position_in_grid]]) {
    if (pos.x >= 240 || pos.y >= 160) return;
    uint c = gpu_color(lines, pos);
    uint3 rgb = uint3(c & 31, (c >> 5) & 31, (c >> 10) & 31);
    rgb = (rgb << 3) | (rgb >> 2);
    output.write(float4(float3(rgb) / 255.0, 1.0), pos);
}

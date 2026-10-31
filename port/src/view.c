/*
 * Extended view (port/notes/widescreen.md): the view size and the display
 * extension the PPU reads (port/include/port/view.h).
 */

#include "port/view.h"

#include <string.h>

// A setting, and outputs rebuilt from the game's state every frame: not part of
// save states (this file is not in TMC_PORT_STATE_SOURCES).
int gPortScreenW = 240;
int gPortScreenH = 160;
const uint16_t* gHwWideMap[4];
uint8_t gHwWideMapMask;
uint8_t gHwGbaLayerMask;
bool gHwGbaScreen;
int16_t gHwViewClip[4] = { 0, 0, PORT_VIEW_MAX_W, PORT_VIEW_MAX_H };

void PortViewFit(double width, double height, int* viewWidth, int* viewHeight) {
    double aspect = width > 0 && height > 0 ? width / height : 1.5;
    int w = 240, h = 160;

    if (aspect >= 1.5) {
        w = (int)(160 * aspect / 16 + 0.5) * 16;
    } else {
        h = (int)(240 / aspect / 8 + 0.5) * 8;
    }
    *viewWidth = w < 240 ? 240 : w > PORT_VIEW_MAX_W ? PORT_VIEW_MAX_W : w;
    *viewHeight = h < 160 ? 160 : h > PORT_VIEW_MAX_H ? PORT_VIEW_MAX_H : h;
}

void PortViewSet(int width, int height) {
    width = width < 240 ? 240 : width > PORT_VIEW_MAX_W ? PORT_VIEW_MAX_W : width;
    height = height < 160 ? 160 : height > PORT_VIEW_MAX_H ? PORT_VIEW_MAX_H : height;
    gPortScreenW = width & ~1;
    gPortScreenH = height;
}

// Two: the game builds one table while the other plays.
static struct {
    const void* gba;
    PortWideWindow edges;
} sWideWindows[2];
static int sWideNext;

PortWideWindow* PortViewWideWindow(const void* gbaTable) {
    int i = sWideWindows[0].gba == gbaTable ? 0 : sWideWindows[1].gba == gbaTable ? 1 : -1;

    if (i < 0) {
        i = sWideNext;
        sWideNext ^= 1;
        sWideWindows[i].gba = gbaTable;
    }
    memset(sWideWindows[i].edges, 0, sizeof(sWideWindows[i].edges));
    return &sWideWindows[i].edges;
}

const PortWideWindow* PortViewWideWindowFor(const void* gbaTable) {
    for (int i = 0; i < 2; i++) {
        if (gbaTable != NULL && sWideWindows[i].gba == gbaTable) {
            return &sWideWindows[i].edges;
        }
    }
    return NULL;
}

void PortViewWideTriangle(PortWideWindow* window, const int32_t x[3], const int32_t y[3]) {
    static const int sEdges[3][2] = { { 0, 1 }, { 0, 2 }, { 1, 2 } }; // sub_0801E49C's order

    for (int line = 0; line < gPortScreenH; line++) {
        int left = gPortScreenW, right = -1;

        // Where each edge crosses the line, stepped as sub_0801E64C does: from
        // the upper end with a 16.16 slope, both ends included.
        for (int e = 0; e < 3; e++) {
            int a = sEdges[e][0], b = sEdges[e][1];
            int32_t x1 = x[a], y1 = y[a], x2 = x[b], y2 = y[b], slope, cross;

            if (y1 > y2) {
                x1 = x[b], y1 = y[b], x2 = x[a], y2 = y[a];
            }
            if (y1 == y2 || line < y1 || line > y2) {
                continue;
            }
            slope = (x2 - x1) * 0x10000 / (y2 - y1);
            cross = (int32_t)((((int64_t)x1 << 16) + (int64_t)slope * (line - y1)) >> 16);
            cross = cross < 0 ? 0 : cross > gPortScreenW ? gPortScreenW : cross;
            left = cross < left ? cross : left;
            right = cross > right ? cross : right;
        }
        if (right >= 0) {
            (*window)[line][0] = left;
            (*window)[line][1] = right;
        }
    }
}

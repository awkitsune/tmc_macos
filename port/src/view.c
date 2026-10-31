/*
 * Extended view (port/notes/widescreen.md): the view size and the display
 * extension the PPU reads (port/include/port/view.h).
 */

#include "port/view.h"

// A setting, and outputs rebuilt from the game's state every frame: not part of
// save states (this file is not in TMC_PORT_STATE_SOURCES).
int gPortScreenW = 240;
int gPortScreenH = 160;
uint16_t gHwWideMap[4][HW_WIDE_MAP_COLS * HW_WIDE_MAP_ROWS];
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

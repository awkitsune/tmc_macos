#ifndef PORT_VIEW_H
#define PORT_VIEW_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Extended view (port/notes/widescreen.md): the game shows PORT_SCREEN_W x
 * PORT_SCREEN_H of the world instead of 240x160. 240x160 is the default and
 * changes nothing.
 *
 * Largest view: the room layers' wide map is 64x32 entries and the window into
 * it starts up to 15 px left of and 23 px above the screen; OBJ x wraps at 512
 * and a sprite is up to 64 wide.
 */
#define PORT_VIEW_MAX_W 432
#define PORT_VIEW_MAX_H 224

extern int gPortScreenW, gPortScreenH;

/* Set the view size (clamped to 240x160 .. the maximum, even widths). Before the
   first frame, or between frames. */
void PortViewSet(int width, int height);
static inline bool PortViewExtended(void) {
    return gPortScreenW != 240 || gPortScreenH != 160;
}

/*
 * The port's display extension, read by the PPU and the presenters each frame:
 *
 * - gHwWideMap[bg]: a 64x32-entry text map (row-major, 64 entries a row) that
 *   BG bg reads instead of its screen blocks, when bit bg of gHwWideMapMask is
 *   set. The GBA's BG VRAM has no room for 512-wide maps.
 * - gHwGbaLayerMask: BGs (bits 0-3) drawn as a 240x160 GBA layer centred in the
 *   view: their scroll is the GBA's and they are transparent outside the centre.
 * - gHwGbaScreen: the whole picture is a 240x160 GBA screen centred in the view
 *   (menus, title, file select), black around it.
 * - gHwViewClip: in the world view, the part of the view the room covers (left,
 *   top, right, bottom; the whole view for a room at least as large). Outside
 *   it only the backdrop shows, as past the edge of the GBA's screen.
 * - Windows: in the world view, a window edge at 240 (x) or 160 (y), the GBA's
 *   screen edge, means the view's edge.
 */
#define HW_WIDE_MAP_COLS 64
#define HW_WIDE_MAP_ROWS 32
extern uint16_t gHwWideMap[4][HW_WIDE_MAP_COLS * HW_WIDE_MAP_ROWS];
extern uint8_t gHwWideMapMask;
extern uint8_t gHwGbaLayerMask;
extern bool gHwGbaScreen;
extern int16_t gHwViewClip[4];

/* Left edge of a centred 240-wide GBA layer in the view. */
static inline int PortViewGbaLeft(void) {
    return (gPortScreenW - 240) / 2;
}
static inline int PortViewGbaTop(void) {
    return (gPortScreenH - 160) / 2;
}

#endif // PORT_VIEW_H

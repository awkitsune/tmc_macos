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
/* "Fit to display": the view for a picture area of width x height (any unit).
   As wide as the GBA's 3:2 or wider: 160 lines, the width to the nearest 16.
   Taller: 240 wide, the height to the nearest 8. Within 240x160 .. the maximum. */
void PortViewFit(double width, double height, int* viewWidth, int* viewHeight);
static inline bool PortViewExtended(void) {
    return gPortScreenW != 240 || gPortScreenH != 160;
}

/*
 * The port's display extension, read by the PPU and the presenters each frame:
 *
 * - gHwWideMap[bg]: a 64x32-entry text map (HW_WIDE_MAP_SIZE entries, where
 *   the game keeps it) that BG bg reads instead of its screen blocks, when bit
 *   bg of gHwWideMapMask is set: a 512x256 map as the
 *   GBA and the DS lay it out, two 32x32 screen blocks side by side
 *   (HwWideMapAt). The GBA's BG VRAM has no room for 512-wide maps.
 * - gHwGbaLayerMask: BGs (bits 0-3) drawn as a 240x160 GBA layer centred in the
 *   view: their scroll is the GBA's and they are transparent outside the centre
 *   (PortViewGbaLeft, PortViewGbaTop).
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
#define HW_WIDE_MAP_SIZE (HW_WIDE_MAP_COLS * HW_WIDE_MAP_ROWS)
extern const uint16_t* gHwWideMap[4];
/* The entry for tile column col (0-63) and row (0-31) of a wide map. */
static inline int HwWideMapAt(int col, int row) {
    return (col >> 5) << 10 | row << 5 | (col & 31);
}
extern uint8_t gHwWideMapMask;
extern uint8_t gHwGbaLayerMask;
extern bool gHwGbaScreen;
extern int16_t gHwViewClip[4];

/*
 * Wide window 0: the game's per-line window tables (the iris, the lantern's and
 * other circles, the white triangle) give WIN0 for the GBA's 160 lines in 8-bit
 * columns, clamped at 240. In the world view the game also writes the same
 * shape for every line of the view, in its columns: PortViewWideWindow returns
 * that table for one GBA table (left and right column per line; 0, 0 is none).
 * While the HBlank DMA plays a GBA table into WIN0H, the frame loop puts the
 * line's edges into I/O the GBA leaves unused (HW_WIDE_WIN0: left, right, then
 * 1 when on; in the line registers), and the renderers take WIN0's columns
 * from there instead.
 */
#define HW_WIDE_WIN0 0x56
typedef int16_t PortWideWindow[PORT_VIEW_MAX_H][2];
PortWideWindow* PortViewWideWindow(const void* gbaTable);
/* The table written for gbaTable, or NULL. */
const PortWideWindow* PortViewWideWindowFor(const void* gbaTable);
/* The white triangle's shape (corners in view pixels) into a wide window. */
void PortViewWideTriangle(PortWideWindow* window, const int32_t x[3], const int32_t y[3]);

/* Left edge of a centred 240-wide GBA layer in the view. */
static inline int PortViewGbaLeft(void) {
    return (gPortScreenW - 240) / 2;
}
static inline int PortViewGbaTop(void) {
    return (gPortScreenH - 160) / 2;
}

#endif // PORT_VIEW_H

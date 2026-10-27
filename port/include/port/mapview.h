#ifndef PORT_MAPVIEW_H
#define PORT_MAPVIEW_H

/*
 * The current room as a picture, read on the game thread without the game's
 * headers.
 */

#include <stdint.h>

// mapdump.c: the TMC_MAPDUMP picture of the current room (both layers, solid tiles darkened, grid,
// entity markers) into fb (width x height BGR555; 0 if the room is not drawable), and one layer's
// opaque pixels (top: 0 bottom, 1 top) over fb.
int MapRenderRoom(uint16_t* fb, int width, int height);
void MapRenderLayer(int top, uint16_t* fb, int width, int height);

#endif // PORT_MAPVIEW_H

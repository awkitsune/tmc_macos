#ifndef PORT_MAPVIEW_H
#define PORT_MAPVIEW_H

/*
 * The current room for the macOS app's Map window (debugwindows.m), read on the
 * game thread (port/src/mapview.c, port/src/mapdump.c) without the game's
 * headers. Two parts: a small state taken every frame while the window is open
 * (entities, camera, player), and the room's picture, drawn again only when the
 * room or its tiles change. Plus requests the window makes, carried out by the
 * game thread at its loop boundary: move the player within the room, warp to
 * another room (port/src/warp.c).
 */

#include <stdint.h>

#define MAPVIEW_TILES 64 // MAX_MAP_SIZE: a room is at most 64 x 64 tiles of 16 x 16 pixels
#define MAPVIEW_MAX_SIZE (MAPVIEW_TILES * 16)
#define MAPVIEW_MAX_ENTITIES 192
#define MAPVIEW_TRANSPARENT 0x8000 // a layer pixel no tile covers (colours are 15-bit)

typedef struct {
    uint8_t list, kind, id, type, action, subAction, layer, hasHitbox;
    int16_t x, y;                      // relative to the room
    int8_t hitboxX, hitboxY;           // hitbox centre, relative to x, y
    uint8_t hitboxWidth, hitboxHeight; // half the hitbox's width and height
} MapViewEntity;

typedef struct {
    uint64_t frame;
    int inGame; // gameplay (TASK_GAME): nothing below is meaningful without it
    int area, room;
    int originX, originY, width, height; // the room, in world pixels
    int cameraX, cameraY;                // the screen's top-left (240 x 160), relative to the room
    int playerX, playerY, playerLayer;
    int control;             // the player can be steered (moves and warps start only then)
    uint64_t imageSequence;  // of the newest room picture
    int entityCount;         // managers left out
    MapViewEntity entities[MAPVIEW_MAX_ENTITIES];
    char status[160];        // what became of the last move or warp request
} MapViewState;

typedef struct {
    uint64_t sequence; // 0: none drawn yet
    uint64_t frame;
    int area, room, width, height, tilesWide, tilesHigh;
    uint16_t backdrop; // BGR555
    // Bottom and top tile layers, width x height BGR555 pixels (row stride width),
    // MAPVIEW_TRANSPARENT where the layer is empty.
    uint16_t layer[2][MAPVIEW_MAX_SIZE * MAPVIEW_MAX_SIZE];
    // Per 16 x 16 tile (row stride MAPVIEW_TILES): the layer's metatile index and collision.
    uint16_t metatile[2][MAPVIEW_TILES * MAPVIEW_TILES];
    uint8_t collision[2][MAPVIEW_TILES * MAPVIEW_TILES];
} MapViewImage;

// mapdump.c: the TMC_MAPDUMP picture of the current room (both layers, solid tiles darkened, grid,
// entity markers) into fb (width x height BGR555; 0 if the room is not drawable), and one layer's
// opaque pixels (top: 0 bottom, 1 top) over fb.
int MapRenderRoom(uint16_t* fb, int width, int height);
void MapRenderLayer(int top, uint16_t* fb, int width, int height);

/*
 * Game thread. Fill state for this frame and, when the room or its tiles have
 * changed since image was drawn (and have stayed so for a few frames, outside a
 * fade) or redraw is set, draw image again with a new sequence. settled skips the
 * wait (paused: nothing will change). Returns whether image was drawn.
 */
int MapViewCapture(MapViewState* state, MapViewImage* image, uint64_t frame, int settled, int redraw);

// FNV-1a of the picture's layers, and the line of numbers the window's summary and the headless
// runner's TMC_MAPVIEW print.
uint64_t MapViewImageHash(const MapViewImage* image);
void MapViewSummary(const MapViewState* state, const MapViewImage* image, char* out, int size);

/*
 * Game thread, at the loop boundary. Move the player to room pixel x, y (the
 * centre of that tile), if the player has control and the tile is walkable
 * (collision 0 on the player's layer). Warp: start a warp to area:room the way
 * TMC_WARP does; MapViewUpdate carries it on every frame until it returns 0.
 * Each writes what happened into message. Returns whether it was done/started.
 */
int MapViewMovePlayer(int x, int y, char* message, int size);
int MapViewWarp(int area, int room, char* message, int size);
int MapViewUpdate(uint64_t frame, char* message, int size);

// Any thread (read-only tables, warp.c): the rooms an area has, and whether the game can load one.
int WarpRoomCount(int area);
int WarpRoomIsValid(int area, int room);

#endif // PORT_MAPVIEW_H

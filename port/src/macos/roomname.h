#ifndef PORT_MACOS_ROOMNAME_H
#define PORT_MACOS_ROOMNAME_H

#include <stdbool.h>

/*
 * Readable names for the game's rooms, generated from include/area.h and
 * include/roomid.h (port/scripts/room_names.py): "Hyrule Field – Lon Lon
 * Ranch", the area's name for a room without one of its own, or NULL.
 */
const char* RoomName(int area, int room);

/*
 * The room the player is in, read from the game's globals (racy but harmless:
 * two bytes the game thread writes on a room change). False outside gameplay
 * (title screen, file select).
 */
bool RoomCurrent(int* area, int* room);

#endif // PORT_MACOS_ROOMNAME_H

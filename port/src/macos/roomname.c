/* The current room for the window's subtitle; the names are in the generated room_names.c. */

#include "roomname.h"

#include "main.h"
#include "room.h"

bool RoomCurrent(int* area, int* room) {
    if (gMain.task != TASK_GAME) {
        return false;
    }
    *area = gRoomControls.area;
    *room = gRoomControls.room;
    return true;
}

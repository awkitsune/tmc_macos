/**
 * @file houseSign.c
 * @ingroup Objects
 *
 * @brief HouseSign object
 */
#include "object/houseSign.h"

#include "asm.h"
#ifdef PLATFORM_NATIVE
#include "manager/houseSignManager.h"
#endif

/*
This object is created by HouseSignManager.
It checks whether the 0x10 x 0x10 rect at field_0x80, field_0x82 is still on the screen.
If not, then it deletes itself and unsets the super->type2 bit in the managers field_0x20 bitfield.
*/
void HouseSign(HouseSignEntity* this) {
    if (super->action == 0) {
        super->action = 1;
    }
    if (CheckRectOnScreen(this->unk_80, this->unk_82, 0x10, 0x10) == 0) {
#ifdef PLATFORM_NATIVE
        // parent is the HouseSignManager. The GBA clears its bitfield (0x20) through Entity.zVelocity, also at 0x20;
        // natively the layouts differ, so use the field's own name.
        ((HouseSignManager*)super->parent)->bitfield &= ~(1 << super->type2);
#else
        super->parent->zVelocity &= ~(1 << super->type2);
#endif
        DeleteThisEntity();
    }
}

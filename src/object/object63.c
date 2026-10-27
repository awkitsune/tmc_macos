/**
 * @file object63.c
 * @ingroup Objects
 *
 * @brief Object63 object
 */
#include "object.h"
#include "physics.h"
#include "item.h"

void Object63_Init(Entity*);
void Object63_Action1(Entity*);
void Object63_Delete(Entity*);
void sub_08093E10(Entity*, Entity*);

void Object63(Entity* this) {
    static void (*const Object63_Actions[])(Entity*) = {
        Object63_Init,
        Object63_Action1,
        Object63_Delete,
    };

    Object63_Actions[this->action]((Entity*)this);
}

void Object63_Init(Entity* this) {
    Entity* objEnt;

    this->action = 1;
    this->z.HALF.HI -= 8;
    this->zVelocity = Q_16_16(2.625);
    this->timer = 32;
    switch (this->type) {
        case 0:
            if (CreateRandomItemDrop(this, 4) == 0) {
                this->action = 2;
            }
#ifdef PLATFORM_NATIVE
            // The drop is the child only when it is a ground item; with no drop (or a beetle)
            // the child is NULL and the GBA's write lands in BIOS, where it is lost.
            if (this->child != NULL) {
                this->child->zVelocity = this->zVelocity;
            }
#else
            this->child->zVelocity = this->zVelocity;
#endif
            break;
        case 1:
            objEnt = CreateObject(0, OBJECT_BLOCKING_STAIRS, 0);
            if (objEnt != NULL) {
                objEnt->timer = 5;
                sub_08093E10(this, objEnt);
            }
            break;
    }
}

void Object63_Action1(Entity* this) {
    if (--this->timer == 0) {
        this->action = 2;
    }

#ifdef PLATFORM_NATIVE
    // No child after a beetle drop (see Object63_Init): the GBA's increment goes to BIOS.
    if ((this->timer & 1) != 0 && this->child != NULL) {
#else
    if ((this->timer & 1) != 0) {
#endif
        this->child->y.HALF.HI++;
    }
}

void Object63_Delete(Entity* this) {
    DeleteEntity(this);
}

void sub_08093E10(Entity* this, Entity* ent) {
    CopyPosition(this, ent);
    ent->parent = this;
    this->child = ent;
    ent->zVelocity = this->zVelocity;
}

#ifndef PORT_STATE_MAP_H
#define PORT_STATE_MAP_H

/*
 * Pointer map of the save state segment, generated from the C types by
 * port/tools/datagen/statemap.py (build/native/state_map.c, with a report of its
 * decisions in state_map.txt). port/src/state.c uses it to know exactly which
 * bytes of each variable can hold a pointer.
 *
 * A pointer-capable word holds a pointer when its value is 2^32 or more (the
 * game's integers are 32-bit; the program lives above 4 GB), an integer
 * otherwise (PtrU32 fields, an Entity* field a position is kept in).
 */

#include <stdint.h>

// Pointer-capable 8-byte words at offset + i * stride, for i < count; a count
// of 0 repeats the pattern to the end of the variable's storage (arrays, whose
// GBA RAM region code may index past the declared length into).
typedef struct {
    uint32_t offset, stride, count;
} StateMapRun;

enum {
    STATE_POOL_NONE,    // the variable's own runs
    STATE_POOL_ENTITY,  // entity slots: each slot by its kind, id and type
    STATE_POOL_MANAGER, // manager slots, the same way
    STATE_POOL_HEAP,    // gzHeap: each block by the type zMalloc recorded
};

typedef struct {
    const char* name; // the symbol, with its leading underscore
    uint32_t firstRun, runCount;
    uint32_t pool; // STATE_POOL_*
    uint32_t size; // declared size (0 if incomplete); its fields may run past the next symbol
} StateMapVar;

// What an entity slot or heap block can hold.
typedef struct {
    uint32_t firstRun, runCount;
    uint32_t hash;   // of the type's name (heap blocks record it)
    uint32_t script; // entities: 1 if its script context word is pointer-capable always, 0 only while scripted
    const char* name;
} StateMapType;

// An entity kind and id whose layout depends on its type field too.
typedef struct {
    uint8_t kind, id, type;
    uint16_t typeIndex;
} StateMapByType;

// A zMalloc call and the type it allocates.
typedef struct {
    const char* file; // __FILE_NAME__
    uint32_t line;
    uint32_t hash; // StateMapType.hash
    uint32_t size; // what it allocates, rounded up to 4 as zMalloc does (0: unknown)
} StateMapHeapSite;

extern const StateMapRun gStateMapRuns[];
extern const StateMapVar gStateMapVars[]; // sorted by name, as strcmp sorts
extern const uint32_t gStateMapVarCount;
extern const StateMapType gStateMapTypes[]; // [0] GenericEntity, [1] an empty manager slot
extern const uint32_t gStateMapTypeCount;
extern const uint16_t gStateMapSubtype[10][256]; // [kind][id] -> gStateMapTypes
extern const StateMapByType gStateMapByType[];
extern const uint32_t gStateMapByTypeCount;
extern const StateMapHeapSite gStateMapHeapSites[];
extern const uint32_t gStateMapHeapSiteCount;

// Entity and manager slots: their size and where kind, id and type are.
extern const uint32_t gStateMapEntitySlot, gStateMapManagerSlot;
extern const uint32_t gStateMapKindOffset, gStateMapIdOffset, gStateMapTypeOffset, gStateMapFlagsOffset;
// GenericEntity.nativeScriptContext, where ENTITY_SCRIPT_CONTEXT keeps a pointer
// in any entity slot: its subtype may be shorter, or keep data there, in which
// case the word is a pointer only while the entity is scripted.
extern const uint32_t gStateMapScriptOffset;
#define STATE_MAP_ENT_SCRIPTED 0x2 // ENT_SCRIPTED (include/entity.h)

// FNV-1a 32 of a type's name, as StateMapType.hash.
static inline uint32_t StateMapHash(const char* name) {
    uint32_t hash = 0x811c9dc5u;
    while (*name != 0) {
        hash = (hash ^ (uint8_t)*name++) * 0x01000193u;
    }
    return hash;
}

// Heap blocks: the StateMapType.hash of the block starting at gzHeap + 4 * i
// (port/src/heap.c); 0 for none.
extern uint32_t gzHeapTypes[0x1000 / 4];

#endif // PORT_STATE_MAP_H

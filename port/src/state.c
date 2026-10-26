/*
 * Save states (port/include/port/state.h): the __TMCSTATE segment in a file.
 *
 * File (version 2): a fixed header, then a zlib-compressed body:
 *
 *   u32 segment size, the segment's bytes
 *   u32 count, then per symbol in the segment: u32 offset, u16 length, name
 *   u32 count, then per pointer target: u16 length, name of a symbol, or of a
 *       section ("@segment,section") for an address no symbol precedes in it
 *   u32 count, then per pointer: u32 offset, u32 target, u64 offset from it
 *
 * Which of the segment's words are pointers comes from the pointer map, made
 * from the C types at build time (port/include/port/state_map.h), so the list
 * is exact: a state loads the same at any address, and into another build as
 * long as that build has the variables and the pointer targets it names.
 *
 * Version 1 files list pointers guessed from their values (or, with a reference
 * run, measured): they load through the map too, which moves only the listed
 * words that can be pointers. Data that merely looked like a pointer stays.
 */

#include <errno.h>
#include <limits.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

#include "port/hw.h"
#include "port/state.h"
#include "port/state_map.h"

extern const struct mach_header_64 _mh_execute_header;

#define STATE_MAGIC "TMCSTATE"
#define STATE_VERSION 2
// Version 1: pointers listed by value (STATE_FLAG_EXACT_POINTERS: measured
// against a run at another address), each as u32 offset, u64 delta, u16 length,
// name; no target table.
#define STATE_VERSION_1 1
#define STATE_FLAG_EXACT_POINTERS 1

typedef struct {
    char magic[8];
    uint32_t version;
    uint32_t headerSize;
    uint32_t flags;
    uint8_t uuid[16];
    uint64_t imageBase; // where the program was loaded
    uint64_t frame;
    uint64_t checksum; // StateChecksum (version 1: of the bytes as they were)
    uint32_t bodySize; // uncompressed
    uint32_t packedSize;
    // Version 2.
    uint64_t imageSize; // the program's mapped span from imageBase
    uint32_t cpuType;
    uint32_t reserved;
} StateHeader;

#define STATE_HEADER_V1_SIZE offsetof(StateHeader, imageSize)

typedef struct {
    uint64_t addr;
    const char* name; // unique: repeated names get "#1", "#2"... in address order
    size_t order;     // index in address order
} Symbol;

typedef struct {
    uint64_t start, end;
    char name[40]; // "@segment,section"
} Section;

static struct {
    bool ready;
    uint8_t uuid[16];
    uint64_t lo, hi; // the program's mapped segments (not __LINKEDIT)
    uint8_t* segment;
    size_t segmentSize;
    Symbol* symbols; // by address
    Symbol* byName;
    size_t count;
    size_t segFirst, segEnd;    // symbols inside the segment
    const StateMapVar** maps;   // per symbol inside the segment, from segFirst; NULL: none
    size_t heapTypes;           // gzHeapTypes' offset in the segment, or SIZE_MAX
    Section sections[64];
    int sectionCount;
} sImage;

uint64_t StateHashBytes(const void* data, size_t size) {
    const uint8_t* p = data;
    uint64_t hash = 0xcbf29ce484222325ull;

    for (size_t i = 0; i < size; i++) {
        hash = (hash ^ p[i]) * 0x100000001b3ull;
    }
    return hash;
}

static int CompareAddr(const void* a, const void* b) {
    const Symbol *x = a, *y = b;
    return x->addr != y->addr ? (x->addr < y->addr ? -1 : 1) : strcmp(x->name, y->name);
}

static int CompareName(const void* a, const void* b) {
    const Symbol *x = a, *y = b;
    int c = strcmp(x->name, y->name);
    return c != 0 ? c : (x->addr > y->addr) - (x->addr < y->addr);
}

// The map of a symbol in the segment ("#n" duplicates share their name's).
static const StateMapVar* MapNamed(const char* name) {
    size_t len = strcspn(name, "#");
    uint32_t lo = 0, hi = gStateMapVarCount;

    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        int c = strncmp(gStateMapVars[mid].name, name, len);
        if (c == 0 && gStateMapVars[mid].name[len] != 0) {
            c = 1;
        }
        if (c == 0) {
            return &gStateMapVars[mid];
        }
        if (c < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return NULL;
}

// The program's symbol table, from __LINKEDIT as mapped in memory.
static void LoadImage(void) {
    const struct mach_header_64* header = &_mh_execute_header;
    const struct load_command* cmd = (const void*)(header + 1);
    const struct symtab_command* symtab = NULL;
    const struct segment_command_64* linkedit = NULL;
    intptr_t slide = 0;
    unsigned long size;

    if (sImage.ready) {
        return;
    }
    sImage.ready = true;
    sImage.lo = UINT64_MAX;
    sImage.heapTypes = SIZE_MAX;
    for (uint32_t i = 0; i < header->ncmds; i++, cmd = (const void*)((const uint8_t*)cmd + cmd->cmdsize)) {
        if (cmd->cmd == LC_UUID) {
            memcpy(sImage.uuid, ((const struct uuid_command*)cmd)->uuid, 16);
        } else if (cmd->cmd == LC_SYMTAB) {
            symtab = (const void*)cmd;
        } else if (cmd->cmd == LC_SEGMENT_64) {
            const struct segment_command_64* seg = (const void*)cmd;
            const struct section_64* sect = (const void*)(seg + 1);
            if (strcmp(seg->segname, "__TEXT") == 0) {
                slide = (intptr_t)header - (intptr_t)seg->vmaddr;
            }
            if (strcmp(seg->segname, "__LINKEDIT") == 0) {
                linkedit = seg;
                continue;
            }
            if (strcmp(seg->segname, "__PAGEZERO") == 0) {
                continue;
            }
            // Section addresses get the slide once __TEXT has been seen (it comes first).
            for (uint32_t j = 0; j < seg->nsects && sImage.sectionCount < 64; j++) {
                Section* s = &sImage.sections[sImage.sectionCount++];
                s->start = sect[j].addr + slide;
                s->end = s->start + sect[j].size;
                snprintf(s->name, sizeof(s->name), "@%.16s,%.16s", sect[j].segname, sect[j].sectname);
            }
            sImage.lo = seg->vmaddr + slide < sImage.lo ? seg->vmaddr + slide : sImage.lo;
            sImage.hi = seg->vmaddr + slide + seg->vmsize > sImage.hi ? seg->vmaddr + slide + seg->vmsize : sImage.hi;
        }
    }
    sImage.segment = getsegmentdata(header, "__TMCSTATE", &size);
    sImage.segmentSize = sImage.segment != NULL ? size : 0;
    if (symtab == NULL || linkedit == NULL) {
        return; // stripped: StateSave refuses (no way to tell the variables apart)
    }

    {
        const uint8_t* base = (const uint8_t*)(linkedit->vmaddr + slide - linkedit->fileoff);
        const struct nlist_64* list = (const void*)(base + symtab->symoff);
        const char* strings = (const char*)(base + symtab->stroff);
        size_t count = 0;

        sImage.symbols = malloc(sizeof(Symbol) * symtab->nsyms);
        for (uint32_t i = 0; i < symtab->nsyms; i++) {
            const char* name = strings + list[i].n_un.n_strx;
            if ((list[i].n_type & N_STAB) || (list[i].n_type & N_TYPE) != N_SECT || name[0] != '_') {
                continue;
            }
            sImage.symbols[count++] = (Symbol){ list[i].n_value + slide, name };
        }
        sImage.count = count;
    }

    // Unique names: the same static name in two files gets an index.
    sImage.byName = malloc(sizeof(Symbol) * (sImage.count + 1));
    memcpy(sImage.byName, sImage.symbols, sizeof(Symbol) * sImage.count);
    qsort(sImage.byName, sImage.count, sizeof(Symbol), CompareName);
    for (size_t i = 1, run = 0; i < sImage.count; i++) {
        if (strcmp(sImage.byName[i].name, sImage.byName[i - 1 - run].name) == 0) {
            char* unique;
            run++;
            asprintf(&unique, "%s#%zu", sImage.byName[i].name, run);
            sImage.byName[i].name = unique;
        } else {
            run = 0;
        }
    }
    memcpy(sImage.symbols, sImage.byName, sizeof(Symbol) * sImage.count);
    qsort(sImage.symbols, sImage.count, sizeof(Symbol), CompareAddr);
    for (size_t i = 0; i < sImage.count; i++) {
        sImage.symbols[i].order = i;
    }
    memcpy(sImage.byName, sImage.symbols, sizeof(Symbol) * sImage.count);
    qsort(sImage.byName, sImage.count, sizeof(Symbol), CompareName);

    sImage.segFirst = sImage.segEnd = sImage.count;
    for (size_t i = 0; i < sImage.count; i++) {
        uint64_t a = sImage.symbols[i].addr;
        if (a >= (uint64_t)sImage.segment && a < (uint64_t)sImage.segment + sImage.segmentSize) {
            if (sImage.segFirst == sImage.count) {
                sImage.segFirst = i;
            }
            sImage.segEnd = i + 1;
        }
    }
    sImage.maps = calloc(sImage.segEnd - sImage.segFirst + 1, sizeof(*sImage.maps));
    for (size_t i = sImage.segFirst; i < sImage.segEnd; i++) {
        sImage.maps[i - sImage.segFirst] = MapNamed(sImage.symbols[i].name);
        if (strcmp(sImage.symbols[i].name, "_gzHeapTypes") == 0) {
            sImage.heapTypes = sImage.symbols[i].addr - (uint64_t)sImage.segment;
        }
    }
}

static const Section* SectionAt(uint64_t addr) {
    for (int i = 0; i < sImage.sectionCount; i++) {
        if (addr >= sImage.sections[i].start && addr < sImage.sections[i].end) {
            return &sImage.sections[i];
        }
    }
    return NULL;
}

// Last symbol at or below addr, or NULL.
static const Symbol* SymbolBelow(uint64_t addr) {
    size_t lo = 0, hi = sImage.count;

    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (sImage.symbols[mid].addr <= addr) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo > 0 ? &sImage.symbols[lo - 1] : NULL;
}

static const Symbol* SymbolNamed(const char* name) {
    size_t lo = 0, hi = sImage.count;

    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(sImage.byName[mid].name, name);
        if (c == 0) {
            return &sImage.byName[mid];
        }
        if (c < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return NULL;
}

// Bytes from a symbol to the next one at a higher address, within its section.
static uint64_t SymbolExtent(const Symbol* s) {
    const Section* section = SectionAt(s->addr);
    size_t next = s->order + 1;
    uint64_t end = section != NULL ? section->end : s->addr;

    while (next < sImage.count && sImage.symbols[next].addr == s->addr) {
        next++;
    }
    if (next < sImage.count && sImage.symbols[next].addr < end) {
        end = sImage.symbols[next].addr;
    }
    return end - s->addr;
}

// Where a pointer target name ("_sym" or "@seg,sect") is in this program, and
// how far a pointer may reach past it (to its end inclusive). False if unknown.
static bool Resolve(const char* name, uint64_t* addr, uint64_t* extent) {
    if (name[0] == '@') {
        for (int i = 0; i < sImage.sectionCount; i++) {
            if (strcmp(sImage.sections[i].name, name) == 0) {
                *addr = sImage.sections[i].start;
                *extent = sImage.sections[i].end - sImage.sections[i].start;
                return true;
            }
        }
        return false;
    }
    const Symbol* s = SymbolNamed(name);
    if (s == NULL) {
        return false;
    }
    *addr = s->addr;
    *extent = SymbolExtent(s);
    return true;
}

// A pointer's target: index into sImage.symbols, or sImage.count + section index.
// False if the address is not in the program (the heap, a library, garbage).
static bool Locate(uint64_t value, size_t* target, uint64_t* delta) {
    const Section* section = SectionAt(value);
    const Symbol* symbol;

    if (section == NULL) {
        return false;
    }
    symbol = SymbolBelow(value);
    if (symbol != NULL && symbol->addr >= section->start) {
        *target = symbol->order;
        *delta = value - symbol->addr;
    } else {
        *target = sImage.count + (size_t)(section - sImage.sections);
        *delta = value - section->start;
    }
    return true;
}

static const char* TargetName(size_t target) {
    return target < sImage.count ? sImage.symbols[target].name : sImage.sections[target - sImage.count].name;
}

// ---- the pointer map ------------------------------------------------------------------

// Pointer-capable words, as a bit per byte offset where one starts.
static void Mark(uint8_t* bits, size_t at) {
    if (at + 8 <= sImage.segmentSize) {
        bits[at >> 3] |= (uint8_t)(1u << (at & 7));
    }
}

static bool Marked(const uint8_t* bits, size_t at) {
    return (bits[at >> 3] >> (at & 7)) & 1;
}

// Runs at base; repeating runs cover size bytes, and none goes past them
// unless the size is open (a variable's declared fields may run past the next
// symbol, a field alias datagen defines inside it).
static void MarkRuns(uint8_t* bits, size_t base, size_t size, bool open, uint32_t first, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        const StateMapRun* run = &gStateMapRuns[first + i];
        if (run->count == 0) {
            for (size_t at = run->offset; run->stride > 0 && at + 8 <= size; at += run->stride) {
                Mark(bits, base + at);
            }
            continue;
        }
        for (uint32_t k = 0; k < run->count; k++) {
            size_t at = run->offset + (size_t)k * run->stride;
            if (open || at + 8 <= size) {
                Mark(bits, base + at);
            }
        }
    }
}

static const StateMapType* SlotType(const uint8_t* slot, bool manager) {
    uint8_t kind = slot[gStateMapKindOffset], id = slot[gStateMapIdOffset], type = slot[gStateMapTypeOffset];

    if (kind >= 10 || (manager && kind != 9)) {
        return &gStateMapTypes[manager ? 1 : 0]; // empty
    }
    for (uint32_t i = 0; i < gStateMapByTypeCount; i++) {
        const StateMapByType* t = &gStateMapByType[i];
        if (t->kind == kind && t->id == id && t->type == type) {
            return &gStateMapTypes[t->typeIndex];
        }
    }
    return &gStateMapTypes[gStateMapSubtype[kind][id]];
}

static const StateMapType* TypeByHash(uint32_t hash) {
    for (uint32_t i = 0; i < gStateMapTypeCount && hash != 0; i++) {
        if (gStateMapTypes[i].hash == hash) {
            return &gStateMapTypes[i];
        }
    }
    return NULL;
}

static uint16_t ReadU16(const uint8_t* p) {
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}

static uint32_t ReadU32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static uint64_t ReadU64(const uint8_t* p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

// gzHeap's live blocks (zMalloc's table at its start: u16 count, then u16
// start and end per block, from the heap's start), each by its recorded type.
static bool MarkHeap(const uint8_t* seg, uint8_t* bits, size_t heap, size_t size, char* problem, size_t problemSize) {
    uint16_t count = ReadU16(seg + heap);
    bool ok = true;

    for (uint32_t i = 0; i < count && 2 + 4 * i + 4 <= size; i++) {
        uint16_t start = ReadU16(seg + heap + 2 + 4 * i), end = ReadU16(seg + heap + 4 + 4 * i);
        const StateMapType* type;
        if (start >= end || end > size) {
            continue;
        }
        type = sImage.heapTypes == SIZE_MAX ? NULL : TypeByHash(ReadU32(seg + sImage.heapTypes + (start / 4) * 4));
        if (type == NULL) {
            if (ok) {
                snprintf(problem, problemSize, "the heap block at gzHeap+0x%x (%u bytes) is of a type the pointer map "
                         "does not know", start, end - start);
            }
            ok = false;
            continue;
        }
        MarkRuns(bits, heap + start, end - start, false, type->firstRun, type->runCount);
    }
    return ok;
}

// Every pointer-capable word of a segment image (this program's layout). False,
// with problem set, if part of it is not covered by the map; the rest is marked.
static bool MarkSlots(const uint8_t* seg, uint8_t* bits, char* problem, size_t problemSize) {
    uint64_t segBase = (uint64_t)sImage.segment;
    size_t covered = 0;
    bool ok = true;

    memset(bits, 0, (sImage.segmentSize + 7) / 8);
    for (size_t i = sImage.segFirst; i < sImage.segEnd;) {
        size_t j = i;
        bool mapped = false;
        while (j < sImage.segEnd && sImage.symbols[j].addr == sImage.symbols[i].addr) {
            j++;
        }
        size_t start = sImage.symbols[i].addr - segBase;
        size_t end = j < sImage.segEnd ? sImage.symbols[j].addr - segBase : sImage.segmentSize;
        for (size_t k = i; k < j; k++) {
            const StateMapVar* var = sImage.maps[k - sImage.segFirst];
            if (var == NULL) {
                continue;
            }
            mapped = true;
            covered = start + var->size > covered ? start + var->size : covered;
            if (var->pool == STATE_POOL_ENTITY || var->pool == STATE_POOL_MANAGER) {
                bool manager = var->pool == STATE_POOL_MANAGER;
                size_t slot = manager ? gStateMapManagerSlot : gStateMapEntitySlot;
                for (size_t at = start; at + slot <= end; at += slot) {
                    const StateMapType* type = SlotType(seg + at, manager);
                    MarkRuns(bits, at, slot, false, type->firstRun, type->runCount);
                    if (!manager && (type->script || (seg[at + gStateMapFlagsOffset] & STATE_MAP_ENT_SCRIPTED))) {
                        Mark(bits, at + gStateMapScriptOffset);
                    }
                }
            } else if (var->pool == STATE_POOL_HEAP) {
                if (!MarkHeap(seg, bits, start, end - start, problem, problemSize) && ok) {
                    ok = false;
                }
            } else {
                size_t size = var->size > end - start ? var->size : end - start;
                MarkRuns(bits, start, size, true, var->firstRun, var->runCount);
            }
        }
        if (!mapped && end - start >= 8 && start >= covered && ok) {
            snprintf(problem, problemSize, "%s is not in the save state pointer map (port/tools/datagen/statemap.txt)",
                     sImage.symbols[i].name);
            ok = false;
        }
        i = j;
    }
    return ok;
}

bool StateCheckMap(char* message, size_t messageSize) {
    uint8_t* bits;
    bool ok;

    LoadImage();
    if (sImage.segment == NULL || sImage.count == 0) {
        snprintf(message, messageSize, "this program has no state segment or no symbols");
        return false;
    }
    bits = malloc((sImage.segmentSize + 7) / 8);
    ok = MarkSlots(sImage.segment, bits, message, messageSize);
    free(bits);
    return ok;
}

// ---- pointers -------------------------------------------------------------------------

typedef struct {
    uint32_t offset;
    size_t target; // see Locate
    uint64_t delta;
} Pointer;

// The segment's pointers (free *list); *canonical gets a copy of the segment
// with each pointer written as its target's name hash plus delta, the same at
// any address. *foreign counts pointer-capable words holding addresses outside
// the program, which are left as they are. False if the map does not cover it.
static bool FindPointers(Pointer** list, size_t* count, uint8_t** canonical, size_t* foreign, char* problem,
                         size_t problemSize) {
    uint8_t* bits = malloc((sImage.segmentSize + 7) / 8);
    size_t capacity = 1024;
    bool ok = MarkSlots(sImage.segment, bits, problem, problemSize);

    *list = malloc(sizeof(Pointer) * capacity);
    *count = 0;
    *foreign = 0;
    if (canonical != NULL) {
        *canonical = malloc(sImage.segmentSize);
        memcpy(*canonical, sImage.segment, sImage.segmentSize);
    }
    for (size_t byte = 0; byte < (sImage.segmentSize + 7) / 8; byte++) {
        if (bits[byte] == 0) {
            continue;
        }
        for (size_t at = byte * 8; at < byte * 8 + 8; at++) {
            uint64_t value;
            Pointer p;
            if (!Marked(bits, at)) {
                continue;
            }
            value = ReadU64(sImage.segment + at);
            if ((value >> 32) == 0) {
                continue; // an integer kept in a pointer-sized field
            }
            if (!Locate(value, &p.target, &p.delta)) {
                (*foreign)++;
                continue;
            }
            p.offset = (uint32_t)at;
            if (*count == capacity) {
                capacity *= 2;
                *list = realloc(*list, sizeof(Pointer) * capacity);
            }
            (*list)[(*count)++] = p;
            if (canonical != NULL) {
                const char* name = TargetName(p.target);
                uint64_t c = StateHashBytes(name, strlen(name)) + p.delta;
                memcpy(*canonical + at, &c, 8);
            }
        }
    }
    free(bits);
    return ok;
}

uint64_t StateChecksum(void) {
    Pointer* list;
    uint8_t* canonical;
    size_t count, foreign;
    char problem[256];
    uint64_t hash;

    LoadImage();
    if (sImage.segment == NULL) {
        return 0;
    }
    FindPointers(&list, &count, &canonical, &foreign, problem, sizeof(problem));
    hash = StateHashBytes(canonical, sImage.segmentSize);
    free(list);
    free(canonical);
    return hash;
}

// ---- writing ------------------------------------------------------------------

typedef struct {
    uint8_t* data;
    size_t size, capacity;
} Buffer;

static void Put(Buffer* b, const void* data, size_t size) {
    if (b->size + size > b->capacity) {
        b->capacity = (b->size + size) * 2;
        b->data = realloc(b->data, b->capacity);
    }
    memcpy(b->data + b->size, data, size);
    b->size += size;
}

static void PutU16(Buffer* b, uint16_t v) {
    Put(b, &v, 2);
}

static void PutU32(Buffer* b, uint32_t v) {
    Put(b, &v, 4);
}

static void PutU64(Buffer* b, uint64_t v) {
    Put(b, &v, 8);
}

static void PutName(Buffer* b, const char* name) {
    size_t len = strlen(name);
    PutU16(b, (uint16_t)len);
    Put(b, name, len);
}

static bool Fail(char* message, size_t size, const char* format, const char* detail) {
    snprintf(message, size, format, detail);
    return false;
}

bool StateSave(const char* path, uint64_t frame, char* message, size_t messageSize) {
    Buffer body = { 0 };
    StateHeader header = { STATE_MAGIC };
    Pointer* pointers;
    uint8_t* canonical;
    uint32_t* targetIds; // per target + 1, 0: not listed yet
    uint32_t targetCount = 0;
    size_t pointerCount, foreign;
    uint8_t* packed;
    uLongf packedSize;
    char temp[PATH_MAX], problem[256];
    FILE* file;
    bool ok;

    LoadImage();
    message[0] = 0;
    if (sImage.segment == NULL || sImage.count == 0) {
        return Fail(message, messageSize, "%s", "this program has no state segment or no symbols");
    }
    if (!FindPointers(&pointers, &pointerCount, &canonical, &foreign, problem, sizeof(problem))) {
        free(pointers);
        free(canonical);
        return Fail(message, messageSize, "cannot save the state: %s", problem);
    }
    if (foreign > 0) {
        fprintf(stderr, "save state: %zu pointer fields hold addresses outside the program; kept as they are\n",
                foreign);
    }

    PutU32(&body, (uint32_t)sImage.segmentSize);
    Put(&body, sImage.segment, sImage.segmentSize);
    PutU32(&body, (uint32_t)(sImage.segEnd - sImage.segFirst));
    for (size_t i = sImage.segFirst; i < sImage.segEnd; i++) {
        PutU32(&body, (uint32_t)(sImage.symbols[i].addr - (uint64_t)sImage.segment));
        PutName(&body, sImage.symbols[i].name);
    }
    targetIds = calloc(sImage.count + (size_t)sImage.sectionCount, sizeof(uint32_t));
    {
        size_t countAt = body.size;
        PutU32(&body, 0);
        for (size_t i = 0; i < pointerCount; i++) {
            if (targetIds[pointers[i].target] == 0) {
                targetIds[pointers[i].target] = ++targetCount;
                PutName(&body, TargetName(pointers[i].target));
            }
        }
        memcpy(body.data + countAt, &targetCount, 4);
    }
    PutU32(&body, (uint32_t)pointerCount);
    for (size_t i = 0; i < pointerCount; i++) {
        PutU32(&body, pointers[i].offset);
        PutU32(&body, targetIds[pointers[i].target] - 1);
        PutU64(&body, pointers[i].delta);
    }
    free(targetIds);
    free(pointers);

    memcpy(header.magic, STATE_MAGIC, 8);
    header.version = STATE_VERSION;
    header.headerSize = sizeof(header);
    memcpy(header.uuid, sImage.uuid, 16);
    header.imageBase = (uint64_t)&_mh_execute_header;
    header.imageSize = sImage.hi - header.imageBase;
    header.cpuType = (uint32_t)_mh_execute_header.cputype;
    header.frame = frame;
    header.checksum = StateHashBytes(canonical, sImage.segmentSize);
    free(canonical);
    header.bodySize = (uint32_t)body.size;
    packedSize = compressBound(body.size);
    packed = malloc(packedSize);
    compress2(packed, &packedSize, body.data, body.size, 6);
    header.packedSize = (uint32_t)packedSize;
    free(body.data);

    // Write beside the target and rename, so a crash never leaves half a state.
    snprintf(temp, sizeof(temp), "%s.tmp", path);
    file = fopen(temp, "wb");
    if (file == NULL) {
        free(packed);
        return Fail(message, messageSize, "cannot write %s", temp);
    }
    ok = fwrite(&header, sizeof(header), 1, file) == 1 && fwrite(packed, packedSize, 1, file) == 1;
    ok = fclose(file) == 0 && ok;
    free(packed);
    if (!ok || rename(temp, path) != 0) {
        unlink(temp);
        return Fail(message, messageSize, "cannot write %s", path);
    }
    return true;
}

// ---- files ------------------------------------------------------------------------

// Read a state file's header and uncompressed body (free *body).
static bool ReadStateFile(const char* path, StateHeader* header, uint8_t** body, size_t* bodySize, char* message,
                          size_t messageSize) {
    FILE* file = fopen(path, "rb");
    uint8_t* packed;
    uLongf size;
    bool ok;

    memset(header, 0, sizeof(*header));
    if (file == NULL) {
        snprintf(message, messageSize, "cannot open %s: %s", path, strerror(errno));
        return false;
    }
    if (fread(header, STATE_HEADER_V1_SIZE, 1, file) != 1 || memcmp(header->magic, STATE_MAGIC, 8) != 0) {
        fclose(file);
        return Fail(message, messageSize, "%s is not a save state", path);
    }
    if (!(header->version == STATE_VERSION_1 && header->headerSize == STATE_HEADER_V1_SIZE) &&
        !(header->version == STATE_VERSION && header->headerSize == sizeof(*header) &&
          fread((uint8_t*)header + STATE_HEADER_V1_SIZE, sizeof(*header) - STATE_HEADER_V1_SIZE, 1, file) == 1)) {
        fclose(file);
        return Fail(message, messageSize, "%s is from an unknown version of the save state format", path);
    }
    packed = malloc(header->packedSize);
    *body = malloc(header->bodySize);
    size = header->bodySize;
    ok = fread(packed, header->packedSize, 1, file) == 1 &&
         uncompress(*body, &size, packed, header->packedSize) == Z_OK && size == header->bodySize;
    fclose(file);
    free(packed);
    if (!ok) {
        free(*body);
        return Fail(message, messageSize, "%s is damaged", path);
    }
    *bodySize = size;
    return true;
}

// ---- reading ------------------------------------------------------------------

typedef struct {
    const uint8_t* p;
    const uint8_t* end;
    bool bad;
} Reader;

static const void* Take(Reader* r, size_t size) {
    const uint8_t* p = r->p;
    if (r->bad || (size_t)(r->end - r->p) < size) {
        r->bad = true;
        return NULL;
    }
    r->p += size;
    return p;
}

static uint32_t TakeU32(Reader* r) {
    const void* p = Take(r, 4);
    return p != NULL ? ReadU32(p) : 0;
}

static uint64_t TakeU64(Reader* r) {
    const void* p = Take(r, 8);
    return p != NULL ? ReadU64(p) : 0;
}

// Name into buf (truncated to fit).
static void TakeName(Reader* r, char* buf, size_t size) {
    const void* p = Take(r, 2);
    uint16_t len = p != NULL ? ReadU16(p) : 0;
    const char* s = Take(r, len);

    if (s == NULL) {
        buf[0] = 0;
        return;
    }
    snprintf(buf, size, "%.*s", (int)len, s);
}

typedef struct {
    uint32_t offset;
    char* name;
} SavedSymbol;

// The saved symbol whose bytes contain offset (symbols sorted by offset).
static const SavedSymbol* SavedSymbolAt(const SavedSymbol* list, uint32_t count, uint32_t offset) {
    uint32_t lo = 0, hi = count;

    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (list[mid].offset <= offset) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo > 0 ? &list[lo - 1] : NULL;
}

typedef struct {
    Reader r;
    const StateHeader* header;
    const uint8_t* saved;
    uint32_t savedSize;
    bool sameBuild;
    SavedSymbol* symbols;
    uint32_t symbolCount;
    uint64_t* newAddr; // per saved symbol: where its bytes went (0: nowhere)
    uint8_t* fresh;    // the new segment
    uint8_t* copied;   // a bit per byte of fresh that came from the state
    uint8_t* written;  // a bit per byte offset where a pointer was relocated to
    uint32_t missing, resized, dropped;
    char* message;
    size_t messageSize;
} Load;

static void SetBits(uint8_t* bits, size_t at, size_t size) {
    for (size_t i = at; i < at + size; i++) {
        bits[i >> 3] |= (uint8_t)(1u << (i & 7));
    }
}

// The state's variables into fresh: the whole segment for the same build, by
// name otherwise (variables this build does not have keep their values here).
static void CopyVariables(Load* l) {
    uint64_t segStart = (uint64_t)sImage.segment, segEnd = segStart + sImage.segmentSize;

    l->symbols = calloc(l->symbolCount + 1, sizeof(SavedSymbol));
    l->newAddr = calloc(l->symbolCount + 1, sizeof(uint64_t));
    for (uint32_t i = 0; i < l->symbolCount && !l->r.bad; i++) {
        char name[512];
        l->symbols[i].offset = TakeU32(&l->r);
        TakeName(&l->r, name, sizeof(name));
        l->symbols[i].name = strdup(name);
    }
    if (l->sameBuild) {
        memcpy(l->fresh, l->saved, l->savedSize);
        SetBits(l->copied, 0, l->savedSize);
        for (uint32_t i = 0; i < l->symbolCount; i++) {
            l->newAddr[i] = segStart + l->symbols[i].offset;
        }
        return;
    }
    for (uint32_t i = 0; i < l->symbolCount && !l->r.bad; i++) {
        // A variable runs to the next address; names at one address (aliases) share it.
        uint32_t next = i + 1;
        while (next < l->symbolCount && l->symbols[next].offset == l->symbols[i].offset) {
            next++;
        }
        uint32_t end = next < l->symbolCount ? l->symbols[next].offset : l->savedSize;
        const Symbol* mine = SymbolNamed(l->symbols[i].name);
        size_t mineSize, size;

        if (l->symbols[i].offset > end || end > l->savedSize) {
            l->r.bad = true;
            break;
        }
        if (mine == NULL || mine->addr < segStart || mine->addr >= segEnd) {
            l->missing++;
            continue;
        }
        {
            size_t after = mine->order + 1;
            while (after < sImage.segEnd && sImage.symbols[after].addr == mine->addr) {
                after++;
            }
            mineSize = (after < sImage.segEnd ? sImage.symbols[after].addr : segEnd) - mine->addr;
        }
        size = end - l->symbols[i].offset;
        if (size != mineSize) {
            l->resized++;
            size = size < mineSize ? size : mineSize;
        }
        memcpy(l->fresh + (mine->addr - segStart), l->saved + l->symbols[i].offset, size);
        SetBits(l->copied, mine->addr - segStart, size);
        l->newAddr[i] = mine->addr;
    }
}

// A state without gzHeapTypes (from before it existed) has heap blocks of no
// recorded type: give each the type of the only allocation of its size.
static void GuessHeapTypes(Load* l) {
    const Symbol* heap = SymbolNamed("_gzHeap");
    size_t at;

    if (heap == NULL || sImage.heapTypes == SIZE_MAX) {
        return;
    }
    for (uint32_t i = 0; i < l->symbolCount; i++) {
        if (strcmp(l->symbols[i].name, "_gzHeapTypes") == 0) {
            return; // recorded
        }
    }
    memset(l->fresh + sImage.heapTypes, 0, sizeof(gzHeapTypes));
    at = heap->addr - (uint64_t)sImage.segment;
    for (uint32_t i = 0, count = ReadU16(l->fresh + at); i < count && 4 * i + 6 <= 0x1000; i++) {
        uint16_t start = ReadU16(l->fresh + at + 2 + 4 * i), end = ReadU16(l->fresh + at + 4 + 4 * i);
        uint32_t hash = 0;
        for (uint32_t s = 0; s < gStateMapHeapSiteCount && start < end; s++) {
            const StateMapHeapSite* site = &gStateMapHeapSites[s];
            if (site->size == (uint32_t)(end - start)) {
                hash = hash == 0 || hash == site->hash ? site->hash : UINT32_MAX;
            }
        }
        if (start < end && hash != UINT32_MAX) {
            memcpy(l->fresh + sImage.heapTypes + (start / 4) * 4, &hash, 4);
        }
    }
}

// One saved pointer into fresh, if the map says that word can hold one here.
static bool Relocate(Load* l, const uint8_t* bits, uint32_t offset, const char* target, uint64_t delta,
                     uint64_t address, uint64_t extent) {
    const SavedSymbol* owner = SavedSymbolAt(l->symbols, l->symbolCount, offset);
    uint64_t at, value;

    if (owner == NULL || l->newAddr[owner - l->symbols] == 0) {
        return true; // in a variable this build does not have
    }
    at = l->newAddr[owner - l->symbols] - (uint64_t)sImage.segment + (offset - owner->offset);
    if (at + 8 > sImage.segmentSize || !Marked(bits, at)) {
        l->dropped++; // data that looked like a pointer (version 1), or a field that is no longer one
        return true;
    }
    if (address == 0 || delta > extent) {
        snprintf(l->message, l->messageSize,
                 "the state cannot be loaded into this build: %s+0x%llx holds a pointer to %s+0x%llx, which %s",
                 owner->name, (unsigned long long)(offset - owner->offset), target, (unsigned long long)delta,
                 address == 0 ? "this build does not have" : "is past its end here");
        return false;
    }
    value = address + delta;
    memcpy(l->fresh + at, &value, 8);
    SetBits(l->written, at, 1);
    return true;
}

// The saved pointers, version 2: targets by name, then (offset, target, delta).
static bool RelocateV2(Load* l, const uint8_t* bits) {
    uint32_t targetCount = TakeU32(&l->r);
    uint64_t* addresses;
    uint64_t* extents;
    char** names;
    uint32_t count;
    bool ok = true;

    if (targetCount > l->savedSize) {
        l->r.bad = true;
        return false;
    }
    addresses = calloc(targetCount + 1, sizeof(uint64_t));
    extents = calloc(targetCount + 1, sizeof(uint64_t));
    names = calloc(targetCount + 1, sizeof(char*));
    for (uint32_t i = 0; i < targetCount && !l->r.bad; i++) {
        char name[512];
        TakeName(&l->r, name, sizeof(name));
        names[i] = strdup(name);
        if (!Resolve(name, &addresses[i], &extents[i]) || (name[0] == '@' && !l->sameBuild)) {
            addresses[i] = 0; // a section's layout is only the same in the same build
        }
    }
    count = TakeU32(&l->r);
    for (uint32_t i = 0; i < count && ok && !l->r.bad; i++) {
        uint32_t offset = TakeU32(&l->r), target = TakeU32(&l->r);
        uint64_t delta = TakeU64(&l->r);
        if (target >= targetCount || offset + 8 > l->savedSize) {
            l->r.bad = true;
            break;
        }
        ok = Relocate(l, bits, offset, names[target], delta, addresses[target], extents[target]);
    }
    for (uint32_t i = 0; i < targetCount; i++) {
        free(names[i]);
    }
    free(names);
    free(addresses);
    free(extents);
    return ok;
}

// Version 1: (offset, delta, target name) per word that looked like a pointer.
static bool RelocateV1(Load* l, const uint8_t* bits) {
    uint32_t count = TakeU32(&l->r);
    bool ok = true;

    for (uint32_t i = 0; i < count && ok && !l->r.bad; i++) {
        uint32_t offset = TakeU32(&l->r);
        uint64_t delta = TakeU64(&l->r), address = 0, extent = 0;
        char name[512];
        TakeName(&l->r, name, sizeof(name));
        if (offset + 8 > l->savedSize) {
            l->r.bad = true;
            break;
        }
        if (!Resolve(name, &address, &extent) || (name[0] == '@' && !l->sameBuild)) {
            address = 0;
        }
        ok = Relocate(l, bits, offset, name, delta, address, extent);
    }
    return ok;
}

// No pointer of the saving program may be left: a pointer-capable word that
// came from the state, was not relocated, and still points into the program
// that wrote it would be wild here.
static bool CheckLeftovers(Load* l, const uint8_t* bits) {
    uint64_t base = l->header->imageBase;
    uint64_t span = l->header->imageSize != 0 ? l->header->imageSize : sImage.hi - sImage.lo;

    for (size_t at = 0; at + 8 <= sImage.segmentSize; at++) {
        uint64_t value;
        if (!Marked(bits, at) || !Marked(l->copied, at) || Marked(l->written, at)) {
            continue;
        }
        value = ReadU64(l->fresh + at);
        if (value >= base && value - base < span) {
            const Symbol* owner = SymbolBelow((uint64_t)sImage.segment + at);
            snprintf(l->message, l->messageSize,
                     "the state cannot be loaded into this build: %s+0x%llx holds an address of the program that saved "
                     "it, which its pointer list does not name",
                     owner != NULL ? owner->name : "?",
                     (unsigned long long)(owner != NULL ? (uint64_t)sImage.segment + at - owner->addr : at));
            return false;
        }
    }
    return true;
}

// Load at another address or into another build: variables by name, every
// pointer re-pointed at its target here, checked against this build's map.
static bool LoadRelocated(Load* l) {
    size_t bitsSize = (sImage.segmentSize + 7) / 8;
    uint8_t* bits = calloc(bitsSize, 1);
    char problem[256];
    bool ok;

    l->fresh = malloc(sImage.segmentSize);
    l->copied = calloc(bitsSize, 1);
    l->written = calloc(bitsSize, 1);
    memcpy(l->fresh, sImage.segment, sImage.segmentSize);
    l->symbolCount = TakeU32(&l->r);
    if (l->symbolCount > l->savedSize) {
        l->r.bad = true;
    } else {
        CopyVariables(l);
        GuessHeapTypes(l);
    }
    // The pointer-capable words of the new segment, by this build's map.
    if (!l->r.bad && !MarkSlots(l->fresh, bits, problem, sizeof(problem))) {
        snprintf(l->message, l->messageSize, "the state cannot be loaded into this build: %s", problem);
        ok = false;
    } else {
        ok = !l->r.bad && (l->header->version == STATE_VERSION_1 ? RelocateV1(l, bits) : RelocateV2(l, bits)) &&
             !l->r.bad && CheckLeftovers(l, bits);
    }
    if (l->r.bad) {
        Fail(l->message, l->messageSize, "%s", "the state file is damaged");
        ok = false;
    }
    if (ok) {
        memcpy(sImage.segment, l->fresh, sImage.segmentSize);
        if (l->header->version == STATE_VERSION_1) {
            snprintf(l->message, l->messageSize,
                     "loaded a version 1 state%s: %u of %u variables missing, %u resized, %u words that looked like "
                     "pointers kept as data",
                     l->sameBuild ? "" : " from another build", l->missing, l->symbolCount, l->resized, l->dropped);
        } else if (!l->sameBuild) {
            snprintf(l->message, l->messageSize,
                     "loaded by symbol from another build: %u of %u variables missing, %u resized%s", l->missing,
                     l->symbolCount, l->resized, l->dropped > 0 ? ", some pointer fields changed type" : "");
        }
    }
    for (uint32_t i = 0; i < l->symbolCount && l->symbols != NULL; i++) {
        free(l->symbols[i].name);
    }
    free(l->symbols);
    free(l->newAddr);
    free(l->fresh);
    free(l->copied);
    free(l->written);
    free(bits);
    return ok;
}

bool StateLoad(const char* path, uint64_t* frame, char* message, size_t messageSize) {
    StateHeader header;
    uint8_t* body;
    size_t bodySize;
    Load l = { 0 };
    bool ok;

    LoadImage();
    message[0] = 0;
    if (sImage.segment == NULL || sImage.count == 0) {
        return Fail(message, messageSize, "%s", "this program has no state segment or no symbols");
    }
    if (!ReadStateFile(path, &header, &body, &bodySize, message, messageSize)) {
        return false;
    }

    l.r = (Reader){ body, body + bodySize, false };
    l.header = &header;
    l.savedSize = TakeU32(&l.r);
    l.saved = Take(&l.r, l.savedSize);
    l.message = message;
    l.messageSize = messageSize;
    if (l.saved == NULL) {
        free(body);
        return Fail(message, messageSize, "%s is damaged", path);
    }
    l.sameBuild = memcmp(header.uuid, sImage.uuid, 16) == 0 && l.savedSize == sImage.segmentSize;
    if (l.sameBuild && header.imageBase == (uint64_t)&_mh_execute_header) {
        // Written at this very address: byte for byte.
        memcpy(sImage.segment, l.saved, l.savedSize);
        ok = true;
    } else {
        ok = LoadRelocated(&l);
    }
    free(body);
    if (ok) {
        *frame = header.frame;
        // States saved before RegisterRamReset set SOUNDBIAS (0x088) hold bias level 0,
        // which would clip every negative sample; give them the BIOS default.
        if (((gHwIo[0x88] | (gHwIo[0x89] << 8)) & 0x3FE) == 0) {
            gHwIo[0x88] = 0x00;
            gHwIo[0x89] = (gHwIo[0x89] & 0xC0) | 0x02;
        }
    }
    return ok;
}

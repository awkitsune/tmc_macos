/*
 * Save states (port/include/port/state.h): the __TMCSTATE segment in a file.
 *
 * File: a fixed header, then a zlib-compressed body:
 *
 *   u32 segment size, the segment's bytes
 *   u32 count, then per symbol in the segment: u32 offset, u16 length, name
 *   u32 count, then per pointer into the program: u32 offset, u64 delta, u16
 *       length, name of the symbol it points into (or "@segment,section")
 *
 * The symbol and pointer lists are only needed to load into a different build;
 * the program's own states are copied back as they are.
 */

#include <errno.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

#include "port/state.h"

#ifndef _POSIX_SPAWN_DISABLE_ASLR
#define _POSIX_SPAWN_DISABLE_ASLR 0x0100
#endif

extern char** environ;
extern const struct mach_header_64 _mh_execute_header;

#define STATE_MAGIC "TMCSTATE"
#define STATE_VERSION 1
// The pointer list is exact (taken by comparing with a run at another address,
// StateSave's reference) rather than guessed from values.
#define STATE_FLAG_EXACT_POINTERS 1

typedef struct {
    char magic[8];
    uint32_t version;
    uint32_t headerSize;
    uint32_t flags; // STATE_FLAG_*
    uint8_t uuid[16];
    uint64_t imageBase; // where the program was loaded
    uint64_t frame;
    uint64_t checksum; // of the segment
    uint32_t bodySize; // uncompressed
    uint32_t packedSize;
} StateHeader;

typedef struct {
    uint64_t addr;
    const char* name; // unique: repeated names get "#1", "#2"... in address order
    size_t order;     // index in address order
} Symbol;

typedef struct {
    uint64_t start, end;
    char name[40]; // "@segment,section"
    int target;    // TARGET_*: what game state can point at here
} Section;

// Where pointers held in game state go. A word in the segment is only taken for
// a pointer if it points into one of these: other values that happen to look
// like addresses (map data with a 1 in its upper half is common) must not be
// re-pointed when a state loads into another build.
enum { TARGET_NONE, TARGET_CODE, TARGET_DATA };

static struct {
    bool ready;
    uint8_t uuid[16];
    uint64_t lo, hi; // the program's mapped segments (not __LINKEDIT)
    uint8_t* segment;
    size_t segmentSize;
    Symbol* symbols; // by address
    Symbol* byName;
    size_t count;
    size_t segFirst, segEnd; // symbols inside the segment
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
                if (strcmp(s->name, "@__TEXT,__text") == 0) {
                    s->target = TARGET_CODE; // function pointers: only a function's start
                } else if (strcmp(s->name, "@__TEXT,__cstring") == 0 || strcmp(s->name, "@__TEXT,__const") == 0 ||
                           strcmp(sect[j].sectname, "__const") == 0 || strcmp(seg->segname, "__TMCSTATE") == 0) {
                    s->target = TARGET_DATA; // game data (any __const) and game state
                }
            }
            sImage.lo = seg->vmaddr + slide < sImage.lo ? seg->vmaddr + slide : sImage.lo;
            sImage.hi = seg->vmaddr + slide + seg->vmsize > sImage.hi ? seg->vmaddr + slide + seg->vmsize : sImage.hi;
        }
    }
    sImage.segment = getsegmentdata(header, "__TMCSTATE", &size);
    sImage.segmentSize = sImage.segment != NULL ? size : 0;
    if (symtab == NULL || linkedit == NULL) {
        return; // stripped: states still load into this same build
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
}

static const Section* SectionAt(uint64_t addr) {
    for (int i = 0; i < sImage.sectionCount; i++) {
        if (addr >= sImage.sections[i].start && addr < sImage.sections[i].end) {
            return &sImage.sections[i];
        }
    }
    return NULL;
}

// The section a pointer held in game state points into, or NULL if the value is
// not taken for a pointer. Table ends a little past a data section count too.
#define END_SLACK 64

static const Section* TargetSection(uint64_t addr) {
    const Section* section = SectionAt(addr);

    if (section != NULL && section->target != TARGET_NONE) {
        return section;
    }
    for (int i = 0; i < sImage.sectionCount; i++) {
        const Section* s = &sImage.sections[i];
        if (s->target == TARGET_DATA && addr >= s->end && addr - s->end < END_SLACK) {
            return s;
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

// Address of a pointer target name ("_sym" or "@seg,sect"); 0 if unknown here.
static uint64_t Resolve(const char* name) {
    if (name[0] == '@') {
        for (int i = 0; i < sImage.sectionCount; i++) {
            if (strcmp(sImage.sections[i].name, name) == 0) {
                return sImage.sections[i].start;
            }
        }
        return 0;
    }
    const Symbol* s = SymbolNamed(name);
    return s != NULL ? s->addr : 0;
}

uint64_t StateChecksum(void) {
    LoadImage();
    return StateHashBytes(sImage.segment, sImage.segmentSize);
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

// ---- files ------------------------------------------------------------------------

// Read a state file's header and uncompressed body (free *body).
static bool ReadStateFile(const char* path, StateHeader* header, uint8_t** body, size_t* bodySize, char* message,
                          size_t messageSize) {
    FILE* file = fopen(path, "rb");
    uint8_t* packed;
    uLongf size;
    bool ok;

    if (file == NULL) {
        snprintf(message, messageSize, "cannot open %s: %s", path, strerror(errno));
        return false;
    }
    if (fread(header, sizeof(*header), 1, file) != 1 || memcmp(header->magic, STATE_MAGIC, 8) != 0) {
        fclose(file);
        return Fail(message, messageSize, "%s is not a save state", path);
    }
    if (header->version != STATE_VERSION || header->headerSize != sizeof(*header)) {
        fclose(file);
        return Fail(message, messageSize, "%s is from an incompatible version of the save state format", path);
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

// The segment of a reference state for StateSave: the same build at the same
// frame, run at another address. NULL with message set if it cannot be used.
static uint8_t* ReadReference(const char* path, uint64_t frame, uint64_t* base, char* message, size_t messageSize) {
    StateHeader header;
    uint8_t* body;
    size_t size;
    uint32_t segmentSize;

    if (!ReadStateFile(path, &header, &body, &size, message, messageSize)) {
        return NULL;
    }
    memcpy(&segmentSize, body, size >= 4 ? 4 : 0);
    if (memcmp(header.uuid, sImage.uuid, 16) != 0 || size < 4 + sImage.segmentSize ||
        segmentSize != sImage.segmentSize || header.frame != frame) {
        Fail(message, messageSize, "reference %s is not this build at this frame", path);
    } else if (header.imageBase == (uint64_t)&_mh_execute_header) {
        Fail(message, messageSize, "reference %s ran at the same address (run it with TMC_ASLR=1)", path);
    } else {
        *base = header.imageBase;
        return body;
    }
    free(body);
    return NULL;
}

bool StateSave(const char* path, uint64_t frame, const char* reference, char* message, size_t messageSize) {
    Buffer body = { 0 };
    StateHeader header = { STATE_MAGIC };
    uint8_t* packed;
    uint8_t* refBody = NULL;
    uint64_t refBase = 0, base = (uint64_t)&_mh_execute_header;
    uLongf packedSize;
    uint32_t pointerCount = 0;
    size_t countAt;
    char temp[PATH_MAX];
    FILE* file;
    bool ok;

    LoadImage();
    message[0] = 0;
    if (sImage.segment == NULL) {
        return Fail(message, messageSize, "%s", "this program has no state segment");
    }
    if (reference != NULL && (refBody = ReadReference(reference, frame, &refBase, message, messageSize)) == NULL) {
        return false;
    }

    PutU32(&body, (uint32_t)sImage.segmentSize);
    Put(&body, sImage.segment, sImage.segmentSize);
    PutU32(&body, (uint32_t)(sImage.segEnd - sImage.segFirst));
    for (size_t i = sImage.segFirst; i < sImage.segEnd; i++) {
        PutU32(&body, (uint32_t)(sImage.symbols[i].addr - (uint64_t)sImage.segment));
        PutName(&body, sImage.symbols[i].name);
    }
    countAt = body.size;
    PutU32(&body, 0);
    for (size_t offset = 0; offset + 8 <= sImage.segmentSize; offset += 8) {
        uint64_t value;
        const Section* section;
        const Symbol* symbol;

        memcpy(&value, sImage.segment + offset, 8);
        if (value < sImage.lo || value >= sImage.hi) {
            continue;
        }
        symbol = SymbolBelow(value);
        if (refBody != NULL) {
            // Exact: a pointer is what moved with the program between the two runs.
            uint64_t ref;
            memcpy(&ref, refBody + 4 + offset, 8);
            if (ref == value || ref - refBase != value - base) {
                continue;
            }
            section = SectionAt(value);
        } else {
            // Guessed: only values pointing where game state points (see Section).
            if ((section = TargetSection(value)) == NULL ||
                (section->target == TARGET_CODE && (symbol == NULL || symbol->addr != value))) {
                continue;
            }
        }
        if (symbol != NULL && (section == NULL || symbol->addr >= section->start)) {
            PutU32(&body, (uint32_t)offset);
            PutU64(&body, value - symbol->addr);
            PutName(&body, symbol->name);
        } else if (section != NULL) {
            PutU32(&body, (uint32_t)offset);
            PutU64(&body, value - section->start);
            PutName(&body, section->name);
        } else {
            continue;
        }
        pointerCount++;
    }
    memcpy(body.data + countAt, &pointerCount, 4);

    memcpy(header.magic, STATE_MAGIC, 8);
    header.version = STATE_VERSION;
    header.headerSize = sizeof(header);
    header.flags = refBody != NULL ? STATE_FLAG_EXACT_POINTERS : 0;
    free(refBody);
    memcpy(header.uuid, sImage.uuid, 16);
    header.imageBase = (uint64_t)&_mh_execute_header;
    header.frame = frame;
    header.checksum = StateHashBytes(sImage.segment, sImage.segmentSize);
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
    uint32_t v = 0;
    const void* p = Take(r, 4);
    if (p != NULL) {
        memcpy(&v, p, 4);
    }
    return v;
}

static uint64_t TakeU64(Reader* r) {
    uint64_t v = 0;
    const void* p = Take(r, 8);
    if (p != NULL) {
        memcpy(&v, p, 8);
    }
    return v;
}

// Name into buf (truncated to fit).
static void TakeName(Reader* r, char* buf, size_t size) {
    uint16_t len = 0;
    const void* p = Take(r, 2);
    const char* s;

    if (p != NULL) {
        memcpy(&len, p, 2);
    }
    s = Take(r, len);
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

// Load into a different build: variables by name, pointers re-pointed by symbol.
static bool LoadBySymbol(Reader* r, const uint8_t* saved, uint32_t savedSize, bool exact, char* message,
                         size_t messageSize) {
    uint32_t count = TakeU32(r);
    SavedSymbol* list;
    uint8_t* fresh;
    uint64_t* newAddr; // where each saved symbol's bytes went (0: nowhere)
    uint32_t missing = 0, resized = 0, unresolved = 0, pointers;
    char name[512];
    bool ok;

    if (sImage.count == 0) {
        return Fail(message, messageSize, "%s", "the state is from another build, and this program has no symbols");
    }
    if (count > savedSize) {
        return Fail(message, messageSize, "%s", "the state file is damaged");
    }
    list = calloc(count + 1, sizeof(SavedSymbol));
    newAddr = calloc(count + 1, sizeof(uint64_t));
    for (uint32_t i = 0; i < count && !r->bad; i++) {
        list[i].offset = TakeU32(r);
        TakeName(r, name, sizeof(name));
        list[i].name = strdup(name);
    }

    // Variables this build does not have keep their current values.
    fresh = malloc(sImage.segmentSize);
    memcpy(fresh, sImage.segment, sImage.segmentSize);
    for (uint32_t i = 0; i < count && !r->bad; i++) {
        // A variable runs to the next address; names at one address (aliases) share it.
        uint32_t next = i + 1;
        while (next < count && list[next].offset == list[i].offset) {
            next++;
        }
        uint32_t end = next < count ? list[next].offset : savedSize;
        const Symbol* mine = SymbolNamed(list[i].name);
        uint64_t segStart = (uint64_t)sImage.segment, segEnd = segStart + sImage.segmentSize;
        size_t mineSize, size;

        if (list[i].offset > end || end > savedSize) {
            r->bad = true;
            break;
        }
        if (mine == NULL || mine->addr < segStart || mine->addr >= segEnd) {
            missing++;
            continue;
        }
        {
            size_t next = mine->order + 1;
            while (next < sImage.segEnd && sImage.symbols[next].addr == mine->addr) {
                next++;
            }
            mineSize = (next < sImage.segEnd ? sImage.symbols[next].addr : segEnd) - mine->addr;
        }
        size = end - list[i].offset;
        if (size != mineSize) {
            resized++;
            size = size < mineSize ? size : mineSize;
        }
        memcpy(fresh + (mine->addr - segStart), saved + list[i].offset, size);
        newAddr[i] = mine->addr;
    }

    pointers = TakeU32(r);
    for (uint32_t i = 0; i < pointers && !r->bad; i++) {
        uint32_t offset = TakeU32(r);
        uint64_t delta = TakeU64(r), target, value;
        const SavedSymbol* owner;

        TakeName(r, name, sizeof(name));
        owner = SavedSymbolAt(list, count, offset);
        if (owner == NULL || newAddr[owner - list] == 0) {
            continue; // in a variable this build does not have
        }
        target = Resolve(name);
        if (target == 0) {
            unresolved++;
            continue;
        }
        value = target + delta;
        memcpy(fresh + (newAddr[owner - list] - (uint64_t)sImage.segment) + (offset - owner->offset), &value, 8);
    }

    ok = !r->bad;
    if (ok) {
        memcpy(sImage.segment, fresh, sImage.segmentSize);
        snprintf(message, messageSize,
                 "loaded by symbol from another build%s: %u of %u variables missing, %u resized, %u of %u pointers "
                 "unresolved",
                 exact ? "" : " (pointers guessed, may misbehave; see port/README.md)", missing, count, resized,
                 unresolved, pointers);
    } else {
        Fail(message, messageSize, "%s", "the state file is damaged");
    }
    for (uint32_t i = 0; i < count; i++) {
        free(list[i].name);
    }
    free(list);
    free(newAddr);
    free(fresh);
    return ok;
}

bool StateLoad(const char* path, uint64_t* frame, char* message, size_t messageSize) {
    StateHeader header;
    uint8_t* body;
    size_t bodySize;
    Reader r;
    uint32_t savedSize;
    const uint8_t* saved;
    bool ok;

    LoadImage();
    message[0] = 0;
    if (sImage.segment == NULL) {
        return Fail(message, messageSize, "%s", "this program has no state segment");
    }
    if (!ReadStateFile(path, &header, &body, &bodySize, message, messageSize)) {
        return false;
    }

    r = (Reader){ body, body + bodySize, false };
    savedSize = TakeU32(&r);
    saved = Take(&r, savedSize);
    if (saved == NULL) {
        free(body);
        return Fail(message, messageSize, "%s is damaged", path);
    }
    if (memcmp(header.uuid, sImage.uuid, 16) == 0 && header.imageBase == (uint64_t)&_mh_execute_header &&
        savedSize == sImage.segmentSize) {
        memcpy(sImage.segment, saved, savedSize);
        ok = true;
    } else {
        ok = LoadBySymbol(&r, saved, savedSize, (header.flags & STATE_FLAG_EXACT_POINTERS) != 0, message,
                          messageSize);
    }
    free(body);
    if (ok) {
        *frame = header.frame;
    }
    return ok;
}

// ---- stable addresses -----------------------------------------------------------

void StateDisableAslr(char** argv) {
    posix_spawnattr_t attr;
    cpu_type_t cpuType = _mh_execute_header.cputype;
    char path[PATH_MAX];
    uint32_t size = sizeof(path);
    const char* keep = getenv("TMC_ASLR");

    if (getenv("TMC_ASLR_REEXEC") != NULL) {
        unsetenv("TMC_ASLR_REEXEC"); // this is the second run; never loop
        return;
    }
    if ((keep != NULL && strcmp(keep, "0") != 0) || _dyld_get_image_vmaddr_slide(0) == 0 ||
        _NSGetExecutablePath(path, &size) != 0) {
        return;
    }
    setenv("TMC_ASLR_REEXEC", "1", 1);
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETEXEC | _POSIX_SPAWN_DISABLE_ASLR);
    // Stay on this slice of a universal binary: an x86_64 run (Rosetta) would
    // otherwise come back as arm64.
    posix_spawnattr_setbinpref_np(&attr, 1, &cpuType, NULL);
    posix_spawn(NULL, path, NULL, &attr, argv, environ); // returns only on failure
    posix_spawnattr_destroy(&attr);
    unsetenv("TMC_ASLR_REEXEC");
}

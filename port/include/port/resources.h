#ifndef PORT_RESOURCES_H
#define PORT_RESOURCES_H

#include <stdint.h>

/*
 * The deferred resource loads queued by LoadResourceAsync and run at the next
 * VBlank by LoadResources. On GBA these are 12-byte records at gUnk_03000C24 + 12;
 * natively they hold real pointers.
 */
typedef struct {
    uint8_t type; // 0: copy, 1: LZ77 decompress, 2: fill
    uint16_t size;
    const void* src;
    void* dest;
} ResourceRequest;

#define RESOURCE_QUEUE_SIZE 0x28

extern ResourceRequest gResourceQueue[RESOURCE_QUEUE_SIZE];

#endif // PORT_RESOURCES_H

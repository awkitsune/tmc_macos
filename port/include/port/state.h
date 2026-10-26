#ifndef PORT_STATE_H
#define PORT_STATE_H

/*
 * Save states: a snapshot of everything the game can change, restored so that
 * the game carries on exactly as it would have from that frame.
 *
 * The game's writable globals (tmc_game, the RAM symbols datagen defines) and
 * the port's emulated hardware, EEPROM and sound engine are compiled into one
 * segment, __TMCSTATE (port/include/port/state_segment.h, force-included by
 * CMakeLists.txt). A state is that segment plus the frame counter, taken at the
 * main loop's frame boundary (PlatformAtLoopBoundary), where no game code has
 * anything live on the stack.
 *
 * Each pointer in the segment is saved as "symbol (or section) + offset". Which
 * words are pointers comes from the pointer map, generated from the C types at
 * build time (port/include/port/state_map.h), so the list is exact: a state
 * loads the same at any address, so with address randomization on, and into
 * another build (a rebuild, the app from a headless state, the other
 * architecture) by symbol: each variable is copied by name and each pointer is
 * re-pointed at the same target in this build. A target that build does not
 * have, or a pointer the map cannot place, makes the load fail with a message,
 * never a wild pointer.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Port globals that are configuration, not game state (sound sink, quality):
   kept out of the segment so loading a state leaves them alone. The DSi build
   has no save states, so nothing to keep out. */
#ifdef PLATFORM_DSI
#define STATE_EXCLUDE
#else
#define STATE_EXCLUDE __attribute__((section("__DATA,__data")))
#endif

/* File extension, without the dot. */
#define STATE_EXTENSION "tmcs"

/* Write the current state with its frame counter. On failure message says why. */
bool StateSave(const char* path, uint64_t frame, char* message, size_t messageSize);

/*
 * Replace the current state with the file's and set *frame to its frame
 * counter. Call only at the loop boundary. On failure nothing changes. On
 * success message is empty, or describes a load by symbol.
 */
bool StateLoad(const char* path, uint64_t* frame, char* message, size_t messageSize);

/* FNV-1a of the state segment, each pointer counted as its target's name plus
   offset: equal states, equal checksums, at any address (and across builds
   whose segment has the same layout). */
uint64_t StateChecksum(void);

/* FNV-1a 64 of any bytes (frame hashes). */
uint64_t StateHashBytes(const void* data, size_t size);

/* Check that the pointer map covers every variable of the state segment; the
   build runs this (TMC_STATE_MAP_CHECK=1). On failure message names one. */
bool StateCheckMap(char* message, size_t messageSize);

#endif // PORT_STATE_H

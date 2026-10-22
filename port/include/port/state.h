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
 * Files are tied to the build: a state loads byte for byte into the program
 * that wrote it (same LC_UUID, same load address; see StateDisableAslr). Into
 * anything else (a rebuild, the app from a headless state) it loads by symbol:
 * each variable is copied by name and every pointer the segment holds into
 * the program is re-pointed at the same symbol (+offset) in this build. The
 * message then says so, with anything that did not match (see StateSave on
 * which words are pointers).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Port globals that are configuration, not game state (sound sink, quality):
   kept out of the segment so loading a state leaves them alone. */
#define STATE_EXCLUDE __attribute__((section("__DATA,__data")))

/* File extension, without the dot. */
#define STATE_EXTENSION "tmcs"

/*
 * Write the current state with its frame counter. On failure message says why.
 *
 * Which of the segment's words are pointers is only known for sure by
 * comparison: reference (or NULL) is a state of this build at the same frame
 * from a run at another address (TMC_ASLR=1). With it the file lists its
 * pointers exactly and loads into other builds safely; without, they are
 * guessed from the values, which is fine within one build but can mistake
 * data for pointers in another (port/scripts/checkpoints.sh makes exact ones).
 */
bool StateSave(const char* path, uint64_t frame, const char* reference, char* message, size_t messageSize);

/*
 * Replace the current state with the file's and set *frame to its frame
 * counter. Call only at the loop boundary. On failure nothing changes. On
 * success message is empty, or describes a load by symbol.
 */
bool StateLoad(const char* path, uint64_t* frame, char* message, size_t messageSize);

/* FNV-1a of the state segment: equal states, equal checksums (within one build). */
uint64_t StateChecksum(void);

/* FNV-1a 64 of any bytes (frame hashes). */
uint64_t StateHashBytes(const void* data, size_t size);

/*
 * Run the program again (same process, via exec) with address randomization
 * off, so pointers in the state and its checksum are the same on every run.
 * Call first thing in main. TMC_ASLR=1 keeps randomization.
 */
void StateDisableAslr(char** argv);

#endif // PORT_STATE_H

#ifndef DSI_H
#define DSI_H

/*
 * The DSi layer's own interface. Plain C types only: the files that include
 * libnds and the ones that include the game's headers both use it, and the two
 * must never meet in one file (port/dsi/PLAN.md, "ABI notes").
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Boot log: printed on the console and kept in gDsiLog (read through GDB). */
void DsiLog(const char* format, ...) __attribute__((format(printf, 1, 2)));
_Noreturn void DsiFatal(const char* format, ...) __attribute__((format(printf, 1, 2)));
#define DSI_LOG_SIZE 4096
extern char gDsiLog[DSI_LOG_SIZE];

/* The GBA IWRAM (32 KB): the game's IWRAM globals live here (ram_layout.ld). */
#define DSI_IWRAM_SIZE 0x8000
extern uint8_t gDsiIwram[DSI_IWRAM_SIZE];

/* The ROM copy, from the linker (rom_symbols.ld). */
extern uint8_t __rom_copy[];
extern uint8_t __rom_copy_end[];
#define DSI_ROM_BASE 0x08000000u

/* Zero the GBA EWRAM block and IWRAM, as the GBA's crt0 leaves them. */
void DsiClearGbaRam(void);

/*
 * Read the player's ROM into the copy and check it. False (with the reason
 * logged) if there is no usable ROM.
 */
bool DsiRomLoad(void);

/*
 * Patch the pointer words in the copy (rom_relocs.s). False if a count is off
 * or a word did not hold the GBA address the generator expects.
 */
bool DsiRomRelocate(void);

/* Make the GBA slot (0x08000000-0x0AFFFFFF) fault, so stray ROM pointers show. */
void DsiProtectGbaSlot(void);

/* SHA-1 (sha1.c). */
typedef struct {
    uint32_t state[5];
    uint64_t length;
    uint8_t buffer[64];
} DsiSha1;
void DsiSha1Init(DsiSha1* ctx);
void DsiSha1Update(DsiSha1* ctx, const void* data, size_t length);
void DsiSha1Final(DsiSha1* ctx, uint8_t digest[20]);

/* CRC-32 as zlib's; pass 0 to start. */
uint32_t DsiCrc32(uint32_t crc, const void* data, size_t length);

/* Run the game from AgbMain; a BIOS SoftReset comes back here. */
_Noreturn void DsiRunGame(void);

/* The presenter: game shadows to the DS display, once per frame at VBlank. */
void DsiPresent(void);

#endif // DSI_H

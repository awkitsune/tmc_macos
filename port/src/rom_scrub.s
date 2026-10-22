// Room for the stretches of ROM bytes that port/tools/scrub_rom_data.py takes
// out of a linked TMC_BAKE_DATA=OFF program: a count, the capacity, then
// (section, offset, ROM offset, length) entries. port/src/romdata.c puts the
// bytes back. Empty in a baked build.

	.section __DATA,__tmcscrub
	.p2align 3
	.globl _gRomScrubTable
_gRomScrubTable:
	.long 0, 2048
	.space 2048 * 16

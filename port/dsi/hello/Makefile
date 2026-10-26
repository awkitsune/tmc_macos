# DSi-mode hello world: prints free main RAM and lists the SD card root.
# Build: make (needs BlocksDS, see port/dsi/README.md).
# make GBA_EWRAM=1 builds dsihello_gbaewram.nds instead: linked at 0x02040000,
# it also checks that 0x02000000-0x0203FFFF is free RAM for the game's EWRAM.

BLOCKSDS ?= /opt/wonderful/thirdparty/blocksds/core

NAME := dsihello
GAME_TITLE := TMC DSi hello
GAME_SUBTITLE := RAM and SD check
GAME_AUTHOR := tmc port

ifeq ($(GBA_EWRAM),1)
    NAME := dsihello_gbaewram
    GAME_SUBTITLE := ARM9 at 0x02040000
    export TMC_DSI_LD := $(abspath ../ld)
    SPECS := $(TMC_DSI_LD)/dsi_arm9_gba_ewram.specs
    DEFINES := -DTMC_GBA_EWRAM
endif

include $(BLOCKSDS)/sys/default_makefiles/rom_arm9/Makefile

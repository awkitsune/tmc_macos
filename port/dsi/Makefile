# The DSi port (port/dsi/PLAN.md). Needs BlocksDS (port/dsi/README.md) and the
# GBA build first (make -j4 in the repository root, which also extracts the
# assets), as the Mac build does.
#
#   make -C port/dsi -j4
#
# Builds build/dsi/tmc_dsi.nds and tmc_dsi.elf, with the link map and figures
# in build/dsi/ (tmc_dsi.map, gen/gen_report.txt).

export BLOCKSDS ?= /opt/wonderful/thirdparty/blocksds/core
WONDERFUL_TOOLCHAIN ?= /opt/wonderful
PREFIX := $(WONDERFUL_TOOLCHAIN)/toolchain/gcc-arm-none-eabi/bin/arm-none-eabi-
CC := $(PREFIX)gcc
NM := $(PREFIX)nm
SIZE := $(PREFIX)size
# The GBA relink uses the GBA build's own linker, as the root Makefile does.
GBA_LD ?= arm-none-eabi-ld
PYTHON ?= $(ROOT)/.venv/bin/python

ROOT := $(abspath ../..)
GBA_BUILD := $(ROOT)/build/USA
BUILD := $(ROOT)/build/dsi
GEN := $(BUILD)/gen

NAME := tmc_dsi
ELF := $(BUILD)/$(NAME).elf
MAP := $(BUILD)/$(NAME).map
ROM := $(BUILD)/$(NAME).nds
# The ARM7 program: BlocksDS's default one plus the sound channels' writes
# (arm7/main.c).
ARM7ELF := $(BUILD)/arm7.elf
GAME_FULL_TITLE := The Minish Cap;DSi port;tmc decomp

# Linked at 0x02040000, so the game's EWRAM keeps its GBA addresses (ld/).
export TMC_DSI_LD := $(abspath ld)
SPECS := $(TMC_DSI_LD)/dsi_arm9_gba_ewram.specs

# DSI_SOUND=hw (default): the m4a voices on the DS's sound channels
# (port/src/sound/dsvoices.c); DSI_SOUND=mix: the software mixer, streamed.
DSI_SOUND ?= hw
SOUND_HW := $(if $(filter hw,$(DSI_SOUND)),1,0)

ARCH := -mthumb -mcpu=arm946e-s+nofp
COMMON_FLAGS := $(ARCH) -O2 -g -ffunction-sections -fdata-sections -specs=$(SPECS) -MMD -MP

# Game files and the Mac port's C replacements for the GBA's assembly and BIOS:
# the GBA's data layout (PORT_HOSTED without PORT_LAYOUT_64). Enums are int-sized
# and u32/s32 are int-sized, as on agbcc (PLAN.md, "ABI notes"). agbcc assumed
# none of what GCC's optimiser may: no strict aliasing (the game's type punning),
# no loops cut short at an array's declared end (decomp structs index past it
# into the next field), no null checks dropped after a dereference.
GAME_DEFINES := -DPLATFORM_DSI=1 -DPORT_HOSTED=1 -DNON_MATCHING=1 -DUSA -DENGLISH -DREVISION=0 -DDSI_SOUND_HW=$(SOUND_HW)
GAME_FLAGS := $(COMMON_FLAGS) $(GAME_DEFINES) -std=gnu11 -fwrapv -fno-short-enums -fno-strict-aliasing \
	-fno-aggressive-loop-optimizations -fno-delete-null-pointer-checks \
	-U__UINT32_TYPE__ "-D__UINT32_TYPE__=unsigned int" -U__INT32_TYPE__ -D__INT32_TYPE__=int \
	-I$(ROOT)/include -I$(GBA_BUILD) -I$(ROOT)/port/include \
	-Wno-builtin-declaration-mismatch -Wno-multichar
# The DSi layer: libnds, never the game's headers. DSI_DEFINES is for test
# builds (-DDSI_TEST_STRAY_READ: main.c reads the GBA slot, which must fault).
# DSI_CHECKS=1 records the first DSI_RECORDS frames (1,024) for the comparison
# with the Mac (README.md): about half speed for those frames, with the buttons
# from tmc/input.txt instead of the DS's.
DSI_CHECKS ?= 0
DSI_RECORDS ?= 1024
# The version shown on the bottom screen: the Mac build's (port/CMakeLists.txt).
TMC_VERSION := $(shell sed -n 's/^project.tmc_native VERSION \([0-9.]*\).*/\1/p' $(ROOT)/port/CMakeLists.txt)
DSI_FLAGS := $(COMMON_FLAGS) -Wall -I$(BLOCKSDS)/libs/libnds/include -I$(ROOT)/port/include \
	-DTMC_VERSION=\"$(TMC_VERSION)\" \
	-DDSI_CHECKS=$(DSI_CHECKS) -DDSI_FRAME_RECORDS=$(DSI_RECORDS) -DDSI_SOUND_HW=$(SOUND_HW) $(DSI_DEFINES)

GAME_SRCS := $(filter-out src/eeprom.c,$(shell cd $(ROOT) && find src -name '*.c' | sort))
PORT_SRCS := port/src/bios.c port/src/interrupts.c port/src/eeprom.c port/src/data_glue.c \
	port/src/frame.c port/src/hw_crc.c port/src/trace.c port/src/view.c \
	$(sort $(wildcard $(ROOT)/port/src/asm/*.c)) \
	port/src/sound/m4a_engine.c port/src/sound/m4a_mixer.c port/src/sound/psg.c port/src/sound/dsvoices.c
PORT_SRCS := $(patsubst $(ROOT)/%,%,$(PORT_SRCS))
DSI_SRCS := $(patsubst $(ROOT)/%,%,$(sort $(wildcard $(ROOT)/port/dsi/src/*.c)))

GAME_OBJS := $(addprefix $(BUILD)/obj/,$(GAME_SRCS:.c=.o) $(PORT_SRCS:.c=.o))
DSI_OBJS := $(addprefix $(BUILD)/obj/,$(DSI_SRCS:.c=.o))
OBJS := $(GAME_OBJS) $(DSI_OBJS)

GEN_LD := $(GEN)/rom_symbols.ld $(GEN)/ram_layout.ld

LDFLAGS := $(ARCH) -specs=$(SPECS) -L$(BLOCKSDS)/libs/libnds/lib -Wl,-Map,$(MAP) \
	-Wl,--no-enum-size-warning -Wl,--no-wchar-size-warning

.PHONY: all clean figures
all: $(ROM)

$(BUILD)/obj/src/%.o: $(ROOT)/src/%.c
	@mkdir -p $(@D)
	@echo "  CC      src/$*.c"
	@$(CC) $(GAME_FLAGS) -c -o $@ $<

$(BUILD)/obj/port/src/%.o: $(ROOT)/port/src/%.c
	@mkdir -p $(@D)
	@echo "  CC      port/src/$*.c"
	@$(CC) $(GAME_FLAGS) -c -o $@ $<

$(BUILD)/obj/port/dsi/src/%.o: $(ROOT)/port/dsi/src/%.c
	@mkdir -p $(@D)
	@echo "  CC      port/dsi/src/$*.c"
	@$(CC) $(DSI_FLAGS) -c -o $@ $<

# The DSi layer (and the mixer) is rebuilt when DSI_CHECKS, DSI_RECORDS or
# DSI_SOUND changes.
CHECKS := $(DSI_CHECKS) $(DSI_RECORDS) $(DSI_SOUND)
CHECKS_STAMP := $(BUILD)/dsi_checks.txt
$(shell mkdir -p $(BUILD) && [ "`cat $(CHECKS_STAMP) 2>/dev/null`" = "$(CHECKS)" ] || echo $(CHECKS) > $(CHECKS_STAMP))
$(DSI_OBJS) $(BUILD)/obj/port/src/sound/m4a_mixer.o: $(CHECKS_STAMP)

ARM7_FLAGS := -mthumb -mcpu=arm7tdmi -O2 -g -ffunction-sections -fdata-sections -Wall -MMD -MP \
	-specs=$(BLOCKSDS)/sys/crts/ds_arm7.specs -D__NDS__ -D__BLOCKSDS__ -DARM7 -I$(BLOCKSDS)/libs/libnds/include

$(BUILD)/arm7/main.o: arm7/main.c src/sound_image.h
	@mkdir -p $(@D)
	@echo "  CC      arm7/main.c"
	@$(CC) $(ARM7_FLAGS) -c -o $@ $<

$(ARM7ELF): $(BUILD)/arm7/main.o
	@echo "  LD      $(notdir $@)"
	@$(CC) -mthumb -mcpu=arm7tdmi -specs=$(BLOCKSDS)/sys/crts/ds_arm7.specs -L$(BLOCKSDS)/libs/libnds/lib \
		-Wl,-Map,$(BUILD)/arm7.map -o $@ $< -Wl,--start-group -lnds7 -lc -Wl,--end-group

# The symbols the DSi objects define: those win over the ROM copy's.
$(GEN)/native_defs.txt: $(OBJS)
	@mkdir -p $(@D)
	@$(NM) -g --defined-only $(OBJS) | awk 'NF == 3 { print $$3 }' | sort -u > $@

# The GBA link again, keeping its relocations (only offsets and names are used).
$(GEN)/tmc_relocs.elf: $(ROOT)/tmc.elf $(GBA_BUILD)/linker.ld
	@mkdir -p $(@D)
	@echo "  LD      GBA relink with relocations"
	@cd $(GBA_BUILD) && $(GBA_LD) -n -q -T linker.ld -o $@ -L $(ROOT)/tools/agbcc/lib -lc

# The data objects' pointer fields that hold raw GBA addresses, from the Mac
# port's data converter (it knows the fields' C types). Its assembly output is
# only a by-product here and is removed.
$(GEN)/raw_pointers.txt: $(ROOT)/tmc.elf $(wildcard $(ROOT)/port/tools/datagen/*.py) $(ROOT)/port/tools/datagen/hints.txt
	@mkdir -p $(@D)
	@echo "  DATAGEN raw pointers in data objects"
	@rm -rf $(GEN)/datagen
	@cd $(ROOT)/port/tools && $(PYTHON) -m datagen --root $(ROOT) --gba-build $(GBA_BUILD) --out $(GEN)/datagen \
		--raw-pointers $@.tmp > $(GEN)/datagen.log
	@rm -rf $(GEN)/datagen
	@mv $@.tmp $@

# One run writes all of GEN_OUT (make 3.81 has no grouped targets), so what uses
# them depends on the stamp.
$(GEN)/gen.stamp: $(GEN)/tmc_relocs.elf $(GEN)/native_defs.txt $(GEN)/raw_pointers.txt $(GBA_BUILD)/tmc.map tools/gen_link.py
	@echo "  GEN     ROM symbols, RAM layout, relocations"
	@$(PYTHON) -I tools/gen_link.py --relocs-elf $(GEN)/tmc_relocs.elf --map $(GBA_BUILD)/tmc.map \
		--linker-ld $(GBA_BUILD)/linker.ld --native-defs $(GEN)/native_defs.txt \
		--raw-pointers $(GEN)/raw_pointers.txt --out $(GEN)
	@touch $@

$(GEN)/rom_relocs.o: $(GEN)/gen.stamp
	@$(CC) $(ARCH) -c -o $@ $(GEN)/rom_relocs.s

$(ELF): $(OBJS) $(GEN)/rom_relocs.o $(GEN)/gen.stamp
	@echo "  LD      $(notdir $@)"
	@$(CC) -o $@ $(OBJS) $(GEN)/rom_relocs.o $(GEN_LD) $(LDFLAGS) -Wl,--start-group -lnds9 -lc -Wl,--end-group

# The menu icon is the Mac app's (16 colours, colour 0 transparent).
ICON_PNG := $(ROOT)/port/src/macos/icon/icon_32.png
ICON := $(GEN)/icon.bmp

$(ICON): $(ICON_PNG) tools/icon_bmp.py
	@mkdir -p $(@D)
	@$(PYTHON) -I tools/icon_bmp.py $< $@

$(ROM): $(ELF) $(ARM7ELF) $(ICON)
	@echo "  NDSTOOL $(notdir $@)"
	@$(BLOCKSDS)/tools/ndstool/ndstool -c $@ -7 $(ARM7ELF) -9 $(ELF) \
		-b $(ICON) "$(GAME_FULL_TITLE)" > /dev/null

# Section sizes of the binary and the parts that make it up.
figures: $(ELF)
	@$(SIZE) -A $(ELF) | awk '$$2 > 0'
	@echo "game and port/src objects:"; $(SIZE) -t $(GAME_OBJS) | tail -1
	@echo "DSi layer objects:"; $(SIZE) -t $(DSI_OBJS) | tail -1

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)

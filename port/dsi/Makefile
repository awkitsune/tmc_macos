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
ARM7ELF := $(BLOCKSDS)/sys/arm7/main_core/arm7_maxmod.elf
GAME_FULL_TITLE := The Minish Cap;DSi port;tmc decomp

# Linked at 0x02040000, so the game's EWRAM keeps its GBA addresses (ld/).
export TMC_DSI_LD := $(abspath ld)
SPECS := $(TMC_DSI_LD)/dsi_arm9_gba_ewram.specs

ARCH := -mthumb -mcpu=arm946e-s+nofp
COMMON_FLAGS := $(ARCH) -O2 -g -ffunction-sections -fdata-sections -specs=$(SPECS) -MMD -MP

# Game files and the Mac port's C replacements for the GBA's assembly and BIOS:
# the GBA's data layout (PORT_HOSTED without PORT_LAYOUT_64). Enums are int-sized
# and u32/s32 are int-sized, as on agbcc (PLAN.md, "ABI notes"). agbcc assumed
# none of what GCC's optimiser may: no strict aliasing (the game's type punning),
# no loops cut short at an array's declared end (decomp structs index past it
# into the next field), no null checks dropped after a dereference.
GAME_DEFINES := -DPLATFORM_DSI=1 -DPORT_HOSTED=1 -DNON_MATCHING=1 -DUSA -DENGLISH -DREVISION=0
GAME_FLAGS := $(COMMON_FLAGS) $(GAME_DEFINES) -std=gnu11 -fwrapv -fno-short-enums -fno-strict-aliasing \
	-fno-aggressive-loop-optimizations -fno-delete-null-pointer-checks \
	-U__UINT32_TYPE__ "-D__UINT32_TYPE__=unsigned int" -U__INT32_TYPE__ -D__INT32_TYPE__=int \
	-I$(ROOT)/include -I$(GBA_BUILD) -I$(ROOT)/port/include \
	-Wno-builtin-declaration-mismatch -Wno-multichar
# The DSi layer: libnds, never the game's headers.
DSI_FLAGS := $(COMMON_FLAGS) -Wall -I$(BLOCKSDS)/libs/libnds/include -I$(ROOT)/port/include

GAME_SRCS := $(filter-out src/eeprom.c,$(shell cd $(ROOT) && find src -name '*.c' | sort))
PORT_SRCS := port/src/bios.c port/src/interrupts.c port/src/eeprom.c port/src/data_glue.c \
	$(sort $(wildcard $(ROOT)/port/src/asm/*.c)) \
	port/src/sound/m4a_engine.c port/src/sound/m4a_mixer.c port/src/sound/psg.c
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

# The symbols the DSi objects define: those win over the ROM copy's.
$(GEN)/native_defs.txt: $(OBJS)
	@mkdir -p $(@D)
	@$(NM) -g --defined-only $(OBJS) | awk 'NF == 3 { print $$3 }' | sort -u > $@

# The GBA link again, keeping its relocations (only offsets and names are used).
$(GEN)/tmc_relocs.elf: $(ROOT)/tmc.elf $(GBA_BUILD)/linker.ld
	@mkdir -p $(@D)
	@echo "  LD      GBA relink with relocations"
	@cd $(GBA_BUILD) && $(GBA_LD) -n -q -T linker.ld -o $@ -L $(ROOT)/tools/agbcc/lib -lc

# One run writes all of GEN_OUT (make 3.81 has no grouped targets), so what uses
# them depends on the stamp.
$(GEN)/gen.stamp: $(GEN)/tmc_relocs.elf $(GEN)/native_defs.txt $(GBA_BUILD)/tmc.map tools/gen_link.py
	@echo "  GEN     ROM symbols, RAM layout, relocations"
	@$(PYTHON) -I tools/gen_link.py --relocs-elf $(GEN)/tmc_relocs.elf --map $(GBA_BUILD)/tmc.map \
		--linker-ld $(GBA_BUILD)/linker.ld --native-defs $(GEN)/native_defs.txt --out $(GEN)
	@touch $@

$(GEN)/rom_relocs.o: $(GEN)/gen.stamp
	@$(CC) $(ARCH) -c -o $@ $(GEN)/rom_relocs.s

$(ELF): $(OBJS) $(GEN)/rom_relocs.o $(GEN)/gen.stamp
	@echo "  LD      $(notdir $@)"
	@$(CC) -o $@ $(OBJS) $(GEN)/rom_relocs.o $(GEN_LD) $(LDFLAGS) -Wl,--start-group -lnds9 -lc -Wl,--end-group

$(ROM): $(ELF)
	@echo "  NDSTOOL $(notdir $@)"
	@$(BLOCKSDS)/tools/ndstool/ndstool -c $@ -7 $(ARM7ELF) -9 $(ELF) \
		-b $(BLOCKSDS)/sys/icon.gif "$(GAME_FULL_TITLE)" > /dev/null

# Section sizes of the binary and the parts that make it up.
figures: $(ELF)
	@$(SIZE) -A $(ELF) | awk '$$2 > 0'
	@echo "game and port/src objects:"; $(SIZE) -t $(GAME_OBJS) | tail -1
	@echo "DSi layer objects:"; $(SIZE) -t $(DSI_OBJS) | tail -1

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)

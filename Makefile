#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

#---------------------------------------------------------------------------------
# TARGET   – name of the output binary (no extension)
# BUILD    – directory for intermediate object files
# SOURCES  – directories containing source files
# INCLUDES – directories containing header files
#---------------------------------------------------------------------------------
TARGET   := pokeStride
BUILD    := build
SOURCES  := source
INCLUDES := source

APP_TITLE       := PokeStride
APP_DESCRIPTION := PokeWalker emulator
APP_AUTHOR      := edgarburges

#---------------------------------------------------------------------------------
# Code-generation options
#---------------------------------------------------------------------------------
ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS  := -g -Wall -O2 -mword-relocations \
            -ffunction-sections \
            $(ARCH)
CFLAGS  += $(INCLUDE) -D__3DS__

CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11

ASFLAGS := -g $(ARCH)
LDFLAGS  = -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS    := -lcitro2d -lcitro3d -lctru -lm
LIBDIRS := $(PORTLIBS) $(CTRULIB)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT  := $(CURDIR)/$(TARGET)
export TOPDIR  := $(CURDIR)

export VPATH := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))

export DEPSDIR := $(CURDIR)/$(BUILD)

#
# Explicitly list the C files to compile.
#
#  utils.c is excluded – it is unity-included directly by walker.c via
#  `#include "utils.c"` and must not be compiled separately (doing so would
#  produce duplicate symbols).
#
CFILES   := walker.c queue.c i2c.c ir.c audio.c irtrace.c ir_test.c jsonlog.c ui.c pokeicon.c species_names.c 3ds_main.c
CPPFILES :=
SFILES   :=

ifeq ($(strip $(CPPFILES)),)
	export LD := $(CC)
else
	export LD := $(CXX)
endif

export OFILES_SOURCES := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES         := $(OFILES_SOURCES)

export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(CURDIR)/$(BUILD)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: $(BUILD) clean all

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(TARGET).elf

#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------

DEPENDS := $(OFILES:.o=.d)

$(OUTPUT).3dsx : $(OUTPUT).elf $(OUTPUT).smdh
	@3dsxtool $< $@ --smdh=$(OUTPUT).smdh
	@echo "built ... $(notdir $@)"

$(OUTPUT).smdh : $(TOPDIR)/icon.png $(TOPDIR)/Makefile
	@smdhtool --create "$(APP_TITLE)" "$(APP_DESCRIPTION)" "$(APP_AUTHOR)" $(TOPDIR)/icon.png $@
	@echo "built ... $(notdir $@)"

$(OUTPUT).elf : $(OFILES)

-include $(DEPENDS)

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------

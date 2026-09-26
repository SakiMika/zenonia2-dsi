.SUFFIXES:
ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITPRO=/opt/devkitpro; export DEVKITARM=$(DEVKITPRO)/devkitARM")
endif

include $(DEVKITARM)/ds_rules

GAME_TITLE      := Zenonia Lost Of Memories
GAME_SUBTITLE1  := GAMEVIL
GAME_SUBTITLE2  := ZLOM

TARGET      := Zenonia_Lost_Of_Memories
BUILD       := build
SOURCES     := source
INCLUDES    := source
DATA        :=
GRAPHICS    :=
AUDIO       :=
ICON        := icon.bmp
NITRO       := nitrofs

ARCH        := -march=armv5te -mtune=arm946e-s -mthumb-interwork
CFLAGS      := -g -Wall -Wextra -O2 -ffunction-sections -fdata-sections $(ARCH) $(INCLUDE) -DARM9
CXXFLAGS    := $(CFLAGS) -fno-rtti -fno-exceptions -fno-strict-aliasing -std=gnu++17
ASFLAGS     := -g $(ARCH)
LDFLAGS     := -specs=ds_arm9.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)
LIBS        := -lnds9

# NitroFS needs the filesystem/FAT compatibility libraries. Keep the same
# ordering used by the current devkitPro NDS template.
ifneq ($(strip $(NITRO)),)
LIBS        := -lfilesystem -lfat $(LIBS)
endif

LIBDIRS     := $(LIBNDS) $(PORTLIBS)

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT := $(CURDIR)/$(TARGET)
export VPATH := $(CURDIR) $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))

ifneq ($(strip $(ICON)),)
export GAME_ICON := $(CURDIR)/$(ICON)
endif
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

ifneq ($(strip $(NITRO)),)
export NITRO_FILES := $(CURDIR)/$(NITRO)
endif

export LD := $(CXX)
export OFILES_BIN := $(addsuffix .o,$(BINFILES))
export OFILES_SOURCES := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES := $(OFILES_BIN) $(OFILES_SOURCES)
export INCLUDE := $(foreach dir,$(INCLUDES),-iquote $(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: $(BUILD) clean all
all: $(BUILD)

$(BUILD):
	@mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).elf $(TARGET).nds $(TARGET).map

else
# v033 native sidecar-save profile.  The game still identifies itself by the
# Zenonia title/banner, but the NDS game code uses AAFA because current melonDS
# maps that retail profile to a regular 128 KiB EEPROM (SaveMemType 4).
# ARM9 is placed at 0x8000, matching the supplied EasyRPG DSi reference, so a
# retail-looking header never makes the homebrew ARM9 payload look like a DS
# secure-area block.  patch_nds_header.py then pads to the profile's exact
# 8 MiB ROM size and fixes device-capacity/header CRC.
$(OUTPUT).nds: $(OUTPUT).elf $(NITRO_FILES) $(GAME_ICON)
	@echo "creating Zenonia v033: EEPROM128 sidecar profile, ARM9@0x8000..."
	@ndstool -c $@ -9 $< $(_ARM7_ELF) -b $(GAME_ICON) "$(GAME_TITLE)" -h 0x8000 -g AAFA 01 "Zenonia" 33 $(_ADDFILES)
	@python "$(dir $(OUTPUT))tools/patch_nds_header.py" "$@"

$(OUTPUT).elf: $(OFILES)
$(OFILES_SOURCES):

# Banner icon: ndstool reads the indexed 32x32 BMP directly.

-include $(DEPSDIR)/*.d
endif

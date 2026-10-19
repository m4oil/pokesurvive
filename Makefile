# PokeSurvivor GBA
#   make          build build/pokesurvivor.gba   (needs devkitARM: https://devkitpro.org)
#   make perf     same, with a scanline readout in the HUD (>=160 means a dropped frame)
#   make host     build the PC test harness (gcc only) -> build/host_sim
#   make assets   regenerate source/assets_gen.* from the web game  (SRC=/path/to/pokesurvivor-main)
TARGET := pokesurvivor
BUILD  := build
SRCS   := source/game.c source/assets_gen.c

ifeq ($(strip $(DEVKITARM)),)
  DEVKITARM := /opt/devkitpro/devkitARM
endif
PREFIX  := $(DEVKITARM)/bin/arm-none-eabi-
ARCH    := -mthumb -mthumb-interwork -mcpu=arm7tdmi
CFLAGS  := -std=gnu11 -O2 -Wall -Wextra $(ARCH) -fomit-frame-pointer -ffunction-sections -fdata-sections -Isource
LDFLAGS := -specs=gba.specs $(ARCH) -Wl,--gc-sections -Wl,-Map,$(BUILD)/$(TARGET).map
GBAFIX  := $(firstword $(wildcard $(DEVKITPRO)/tools/bin/gbafix /opt/devkitpro/tools/bin/gbafix) gbafix)

all: $(BUILD)/$(TARGET).gba
perf: CFLAGS += -DDEBUG_PERF
perf: all

$(BUILD)/$(TARGET).gba: $(SRCS) source/game.h source/gba.h source/assets_gen.h
	@mkdir -p $(BUILD)
	$(PREFIX)gcc $(CFLAGS) $(SRCS) $(LDFLAGS) -o $(BUILD)/$(TARGET).elf
	$(PREFIX)objcopy -O binary $(BUILD)/$(TARGET).elf $@
	-$(GBAFIX) $@ -tPOKESURVIVOR -cPSVR -m01
	@ls -l $@

host: $(BUILD)/host_sim
$(BUILD)/host_sim: $(SRCS) host/host.c source/game.h source/gba.h source/assets_gen.h
	@mkdir -p $(BUILD)
	gcc -std=gnu11 -O2 -Wall -Wextra -Wno-misleading-indentation -fno-strict-aliasing -DHOST -Isource $(SRCS) host/host.c -o $@

assets:
	python3 tools/convert_assets.py --src $(SRC)

clean:
	rm -rf $(BUILD)
.PHONY: all perf host assets clean

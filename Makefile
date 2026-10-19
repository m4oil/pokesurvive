# PokeSurvivor GBA - flat layout (all files in one folder). `make` -> build/pokesurvivor.gba
TARGET := pokesurvivor
BUILD  := build
SRCS   := game.c assets_gen.c

ifeq ($(strip $(DEVKITARM)),)
  DEVKITARM := /opt/devkitpro/devkitARM
endif
PREFIX  := $(DEVKITARM)/bin/arm-none-eabi-
ARCH    := -mthumb -mthumb-interwork -mcpu=arm7tdmi
CFLAGS  := -std=gnu11 -O2 -Wall -Wextra $(ARCH) -fomit-frame-pointer -ffunction-sections -fdata-sections -I.
LDFLAGS := -specs=gba.specs $(ARCH) -Wl,--gc-sections -Wl,-Map,$(BUILD)/$(TARGET).map
GBAFIX  := $(firstword $(wildcard $(DEVKITPRO)/tools/bin/gbafix /opt/devkitpro/tools/bin/gbafix) gbafix)

all: $(BUILD)/$(TARGET).gba
perf: CFLAGS += -DDEBUG_PERF
perf: all

$(BUILD)/$(TARGET).gba: $(SRCS) game.h gba.h assets_gen.h
	@mkdir -p $(BUILD)
	$(PREFIX)gcc $(CFLAGS) $(SRCS) $(LDFLAGS) -o $(BUILD)/$(TARGET).elf
	$(PREFIX)objcopy -O binary $(BUILD)/$(TARGET).elf $@
	-$(GBAFIX) $@ -tPOKESURVIVOR -cPSVR -m01
	@ls -l $@

clean:
	rm -rf $(BUILD)
.PHONY: all perf clean

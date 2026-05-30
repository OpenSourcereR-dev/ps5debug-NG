# SPDX-License-Identifier: GPL-3.0-only

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

ELF       := build/ps5debug-NG.elf
BUILD_DIR := build

PAYLOAD_ELF  := ../debugger/build/debugger.elf
EMBEDDED_BIN := $(BUILD_DIR)/embedded_inner.elf.bin
EMBEDDED_OBJ := $(BUILD_DIR)/embedded_inner.o
EMBEDDED_S   := source/embedded_inner.S

CFLAGS  := -Wall -O2 -I../common/include -ffunction-sections -fdata-sections
LDFLAGS := -Wl,--gc-sections
LDLIBS  := -lSceLibcInternal -lkernel_sys -lSceNet -lSceNetCtl -lSceSysCore

CFILES := $(wildcard ../common/source/*.c) $(wildcard source/*.c)

all: $(ELF)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(EMBEDDED_BIN): $(PAYLOAD_ELF) | $(BUILD_DIR)
	cp $< $@

$(EMBEDDED_OBJ): $(EMBEDDED_S) $(EMBEDDED_BIN) | $(BUILD_DIR)
	$(CC) -c -o $@ $(EMBEDDED_S) -I$(BUILD_DIR)

$(ELF): $(CFILES) $(EMBEDDED_OBJ) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(CFILES) $(EMBEDDED_OBJ) $(LDFLAGS) $(LDLIBS)

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean

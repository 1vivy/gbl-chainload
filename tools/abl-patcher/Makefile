CC      ?= cc
CFLAGS  ?= -O1 -g -Wall -Wextra -Wno-unused-parameter -std=c11
PROJ    := $(realpath ../..)
DPL     := $(PROJ)/GblChainloadPkg/Library/DynamicPatchLib

CFLAGS += -D__HOST_BUILD__ -DGBL_MODE=1 \
          -I$(PROJ)/GblChainloadPkg/Include/Library \
          -I$(DPL)/Internal

SRCS := abl-patcher.c \
        $(DPL)/Internal/ScanLib.c \
        $(DPL)/Internal/PeSections.c \
        $(DPL)/Internal/Encode.c \
        $(DPL)/Internal/Arm64Decode.c \
        $(DPL)/Internal/PatchEngine.c \
        $(DPL)/PatchTable.c \
        $(DPL)/universal/universal.c \
        $(DPL)/oem/oneplus_canoe.c \
        $(DPL)/mode_1/mode_1.c

abl-patcher: $(SRCS)
	$(CC) $(CFLAGS) $^ -o $@

clean:
	rm -f abl-patcher abl-patcher-android *.o

# Quick self-check: build and exercise against the infiniti fixture.
test: abl-patcher
	./abl-patcher --in $(PROJ)/images/infiniti/LinuxLoader_infiniti.efi --check-anchors-only
	./abl-patcher --in $(PROJ)/images/infiniti/LinuxLoader_infiniti.efi --out /tmp/patched-infiniti.bin
	@echo "ok abl-patcher self-test"

# --- Android cross-compile target (NDK r27) ---
NDK     ?= $(ANDROID_NDK)
NDK_CC   = $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android31-clang
NDK_CFLAGS = -static -O2 -Wall -Wextra -Wno-unused-parameter -std=c11 \
             -D__HOST_BUILD__ -DGBL_MODE=1 \
             -I$(PROJ)/GblChainloadPkg/Include/Library \
             -I$(DPL)/Internal

android: abl-patcher-android

abl-patcher-android: $(SRCS)
	$(NDK_CC) $(NDK_CFLAGS) $^ -o $@

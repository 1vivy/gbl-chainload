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
	rm -f abl-patcher abl-patcher-android abl-patcher.exe \
	      abl-patcher-macos-x64 abl-patcher-macos-arm64 *.o

# --- Windows / macOS cross-compile (zig cc) ---
XCFLAGS = -O2 -Wall -Wextra -Wno-unused-parameter -std=c11 \
          -D__HOST_BUILD__ -DGBL_MODE=1 \
          -I$(PROJ)/GblChainloadPkg/Include/Library -I$(DPL)/Internal

windows:     abl-patcher.exe
macos-x64:   abl-patcher-macos-x64
macos-arm64: abl-patcher-macos-arm64

abl-patcher.exe: $(SRCS)
	zig cc -target x86_64-windows-gnu $(XCFLAGS) $^ -o $@

abl-patcher-macos-x64: $(SRCS)
	zig cc -target x86_64-macos $(XCFLAGS) $^ -o $@

abl-patcher-macos-arm64: $(SRCS)
	zig cc -target aarch64-macos $(XCFLAGS) $^ -o $@

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

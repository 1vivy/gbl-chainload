# tools/vbmeta-graft/Makefile
PROJ    := $(realpath ../..)
AVB     := $(PROJ)/GblChainloadPkg/Library/AvbParseLib
GPL     := $(PROJ)/GblChainloadPkg/Library/GblPayloadLib
INCS    := -I$(AVB)/Internal -I$(PROJ)/GblChainloadPkg/Include/Library \
           -I$(GPL)/Internal -DGBL_HOST_BUILD=1
CFLAGS  ?= -O1 -g -Wall -Wextra -std=c11
CFLAGS  += -D__HOST_BUILD__ $(INCS)

SRCS := vbmeta-graft.c $(AVB)/AvbParse.c $(GPL)/Sha256.c

all: vbmeta-graft

vbmeta-graft: $(SRCS)
	$(CC) $(CFLAGS) $^ -o $@

# --- Android cross-compile target (NDK r27) ---
NDK     ?= $(ANDROID_NDK)
NDK_CC   = $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android31-clang
NDK_CFLAGS = -static -O2 -Wall -Wextra -std=c11 -D__HOST_BUILD__ $(INCS)

android: vbmeta-graft-android

vbmeta-graft-android: $(SRCS)
	$(NDK_CC) $(NDK_CFLAGS) $^ -o $@

clean:
	rm -f vbmeta-graft vbmeta-graft-android vbmeta-graft.exe \
	      vbmeta-graft-macos-x64 vbmeta-graft-macos-arm64

# --- Windows / macOS cross-compile (zig cc) ---
XCFLAGS = -O2 -Wall -Wextra -std=c11 -D__HOST_BUILD__ $(INCS)

windows:     vbmeta-graft.exe
macos-x64:   vbmeta-graft-macos-x64
macos-arm64: vbmeta-graft-macos-arm64

vbmeta-graft.exe: $(SRCS)
	zig cc -target x86_64-windows-gnu $(XCFLAGS) $^ -o $@

vbmeta-graft-macos-x64: $(SRCS)
	zig cc -target x86_64-macos $(XCFLAGS) $^ -o $@

vbmeta-graft-macos-arm64: $(SRCS)
	zig cc -target aarch64-macos $(XCFLAGS) $^ -o $@

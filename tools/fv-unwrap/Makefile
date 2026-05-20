CC      ?= cc
CFLAGS  ?= -O1 -g -Wall -Wextra -std=c11
LDFLAGS ?= -llzma

fv-unwrap: fv-unwrap.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

clean:
	rm -f fv-unwrap fv-unwrap-android fv-unwrap.exe \
	      fv-unwrap-macos-x64 fv-unwrap-macos-arm64 fv-unwrap-linux *.o

# --- Android cross-compile target (NDK r27) ---
# Links the static liblzma cross-compiled by docker/Dockerfile into
# $(LIBLZMA_ANDROID) — same -llzma path as the host build, no vendored
# LZMA source. The Dockerfile exports LIBLZMA_ANDROID; the default below
# is the in-image install prefix for out-of-container invocations.
NDK     ?= $(ANDROID_NDK)
NDK_CC   = $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android31-clang
NDK_CFLAGS = -static -O2 -Wall -Wextra -std=c11
LIBLZMA_ANDROID ?= /opt/liblzma-android

android: fv-unwrap-android

fv-unwrap-android: fv-unwrap.c $(LIBLZMA_ANDROID)/lib/liblzma.a
	$(NDK_CC) $(NDK_CFLAGS) -I$(LIBLZMA_ANDROID)/include -o $@ \
	    fv-unwrap.c $(LIBLZMA_ANDROID)/lib/liblzma.a

# --- Windows / macOS cross-compile (zig cc) ---
# Links the static liblzma cross-compiled per target by docker/Dockerfile
# into $(LIBLZMA_WINDOWS|MACOS_X64|MACOS_ARM64) — the same -llzma role as
# the host (-llzma) and android ($(LIBLZMA_ANDROID)) builds.
XCFLAGS = -O2 -Wall -Wextra -std=c11

windows:     fv-unwrap.exe
macos-x64:   fv-unwrap-macos-x64
macos-arm64: fv-unwrap-macos-arm64
linux:       fv-unwrap-linux

fv-unwrap.exe: fv-unwrap.c $(LIBLZMA_WINDOWS)/lib/liblzma.a
	zig cc -target x86_64-windows-gnu $(XCFLAGS) \
	    -I$(LIBLZMA_WINDOWS)/include -o $@ \
	    fv-unwrap.c $(LIBLZMA_WINDOWS)/lib/liblzma.a

fv-unwrap-macos-x64: fv-unwrap.c $(LIBLZMA_MACOS_X64)/lib/liblzma.a
	zig cc -target x86_64-macos $(XCFLAGS) \
	    -I$(LIBLZMA_MACOS_X64)/include -o $@ \
	    fv-unwrap.c $(LIBLZMA_MACOS_X64)/lib/liblzma.a

fv-unwrap-macos-arm64: fv-unwrap.c $(LIBLZMA_MACOS_ARM64)/lib/liblzma.a
	zig cc -target aarch64-macos $(XCFLAGS) \
	    -I$(LIBLZMA_MACOS_ARM64)/include -o $@ \
	    fv-unwrap.c $(LIBLZMA_MACOS_ARM64)/lib/liblzma.a

fv-unwrap-linux: fv-unwrap.c $(LIBLZMA_LINUX)/lib/liblzma.a
	zig cc -target x86_64-linux-musl $(XCFLAGS) -static \
	    -I$(LIBLZMA_LINUX)/include -o $@ \
	    fv-unwrap.c $(LIBLZMA_LINUX)/lib/liblzma.a

# Project version (single source of truth: top-level VERSION file).
GBL_TOOL_VERSION := $(shell tr -d '[:space:]' < $(dir $(lastword $(MAKEFILE_LIST)))../../VERSION)
ifeq ($(GBL_TOOL_VERSION),)
$(error VERSION file is empty or missing)
endif
CFLAGS  += -DGBL_TOOL_VERSION='"$(GBL_TOOL_VERSION)"'
XCFLAGS += -DGBL_TOOL_VERSION='"$(GBL_TOOL_VERSION)"'
NDK_CFLAGS += -DGBL_TOOL_VERSION='"$(GBL_TOOL_VERSION)"'

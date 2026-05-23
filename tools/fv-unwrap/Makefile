# tools/fv-unwrap/Makefile
#
# PR2 Task 3: gbl_contains_utf16_efisp moved from tools/shared/efisp_scan.h
# into crates/pe-utils. Each cross target links the matching cargo
# target's libpe_utils.a.
CC      ?= cc
CFLAGS  ?= -O1 -g -Wall -Wextra -std=c11
LDFLAGS ?= -llzma
PROJ    := $(realpath ../..)

# Default target — declared up here so `make` builds fv-unwrap rather
# than tripping over the cargo staticlib rules listed below.
all: fv-unwrap

# crates/pe-utils staticlibs (one per cross target).
PE_UTILS_HOST        := $(PROJ)/target/release/libpe_utils.a
PE_UTILS_ANDROID     := $(PROJ)/target/aarch64-linux-android/release/libpe_utils.a
PE_UTILS_WINDOWS_X64 := $(PROJ)/target/x86_64-pc-windows-gnu/release/libpe_utils.a
PE_UTILS_MACOS_X64   := $(PROJ)/target/x86_64-apple-darwin/release/libpe_utils.a
PE_UTILS_MACOS_ARM64 := $(PROJ)/target/aarch64-apple-darwin/release/libpe_utils.a
PE_UTILS_LINUX_MUSL  := $(PROJ)/target/x86_64-unknown-linux-musl/release/libpe_utils.a

.PHONY: all $(PE_UTILS_HOST) $(PE_UTILS_ANDROID) $(PE_UTILS_WINDOWS_X64) \
        $(PE_UTILS_MACOS_X64) $(PE_UTILS_MACOS_ARM64) $(PE_UTILS_LINUX_MUSL)

$(PE_UTILS_HOST):
	cd $(PROJ) && cargo build --release -p pe-utils

$(PE_UTILS_ANDROID):
	cd $(PROJ) && cargo build --release --target aarch64-linux-android -p pe-utils

$(PE_UTILS_WINDOWS_X64):
	cd $(PROJ) && cargo build --release --target x86_64-pc-windows-gnu -p pe-utils

$(PE_UTILS_MACOS_X64):
	cd $(PROJ) && cargo build --release --target x86_64-apple-darwin -p pe-utils

$(PE_UTILS_MACOS_ARM64):
	cd $(PROJ) && cargo build --release --target aarch64-apple-darwin -p pe-utils

$(PE_UTILS_LINUX_MUSL):
	cd $(PROJ) && cargo build --release --target x86_64-unknown-linux-musl -p pe-utils

fv-unwrap: fv-unwrap.c $(PE_UTILS_HOST)
	$(CC) $(CFLAGS) fv-unwrap.c $(PE_UTILS_HOST) -o $@ $(LDFLAGS)

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

fv-unwrap-android: fv-unwrap.c $(LIBLZMA_ANDROID)/lib/liblzma.a $(PE_UTILS_ANDROID)
	$(NDK_CC) $(NDK_CFLAGS) -I$(LIBLZMA_ANDROID)/include -o $@ \
	    fv-unwrap.c $(LIBLZMA_ANDROID)/lib/liblzma.a $(PE_UTILS_ANDROID)

# --- Windows / macOS cross-compile (zig cc) ---
# Links the static liblzma cross-compiled per target by docker/Dockerfile
# into $(LIBLZMA_WINDOWS|MACOS_X64|MACOS_ARM64) — the same -llzma role as
# the host (-llzma) and android ($(LIBLZMA_ANDROID)) builds.
XCFLAGS = -O2 -Wall -Wextra -std=c11

windows:     fv-unwrap.exe
macos-x64:   fv-unwrap-macos-x64
macos-arm64: fv-unwrap-macos-arm64
linux:       fv-unwrap-linux

fv-unwrap.exe: fv-unwrap.c $(LIBLZMA_WINDOWS)/lib/liblzma.a $(PE_UTILS_WINDOWS_X64)
	zig cc -target x86_64-windows-gnu $(XCFLAGS) \
	    -I$(LIBLZMA_WINDOWS)/include -o $@ \
	    fv-unwrap.c $(LIBLZMA_WINDOWS)/lib/liblzma.a $(PE_UTILS_WINDOWS_X64)

fv-unwrap-macos-x64: fv-unwrap.c $(LIBLZMA_MACOS_X64)/lib/liblzma.a $(PE_UTILS_MACOS_X64)
	zig cc -target x86_64-macos $(XCFLAGS) \
	    -I$(LIBLZMA_MACOS_X64)/include -o $@ \
	    fv-unwrap.c $(LIBLZMA_MACOS_X64)/lib/liblzma.a $(PE_UTILS_MACOS_X64)

fv-unwrap-macos-arm64: fv-unwrap.c $(LIBLZMA_MACOS_ARM64)/lib/liblzma.a $(PE_UTILS_MACOS_ARM64)
	zig cc -target aarch64-macos $(XCFLAGS) \
	    -I$(LIBLZMA_MACOS_ARM64)/include -o $@ \
	    fv-unwrap.c $(LIBLZMA_MACOS_ARM64)/lib/liblzma.a $(PE_UTILS_MACOS_ARM64)

fv-unwrap-linux: fv-unwrap.c $(LIBLZMA_LINUX)/lib/liblzma.a $(PE_UTILS_LINUX_MUSL)
	zig cc -target x86_64-linux-musl $(XCFLAGS) -static \
	    -I$(LIBLZMA_LINUX)/include -o $@ \
	    fv-unwrap.c $(LIBLZMA_LINUX)/lib/liblzma.a $(PE_UTILS_LINUX_MUSL)

# Project version (single source of truth: top-level VERSION file).
GBL_TOOL_VERSION := $(shell tr -d '[:space:]' < $(dir $(lastword $(MAKEFILE_LIST)))../../VERSION)
ifeq ($(GBL_TOOL_VERSION),)
$(error VERSION file is empty or missing)
endif
CFLAGS  += -DGBL_TOOL_VERSION='"$(GBL_TOOL_VERSION)"'
XCFLAGS += -DGBL_TOOL_VERSION='"$(GBL_TOOL_VERSION)"'
NDK_CFLAGS += -DGBL_TOOL_VERSION='"$(GBL_TOOL_VERSION)"'

# tools/gblp1-inspect/Makefile
#
# PR2 Task 4: gbl_sha256 + gbl_crc32 (formerly Sha256.c + Crc32.c) now
# ship in libgblp1.a from crates/gblp1. Cargo builds the staticlib per
# target; each cross variant links the matching one.
CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -Werror -O2 -std=c99
PROJ    := $(realpath ../..)

CFLAGS  += -DGBL_HOST_BUILD=1

SRCS    := gblp1-inspect.c
HDRS    := ../shared/gblp1.h ../../crates/gblp1/include/gblp1_ffi.h

GBLP1_HOST        := $(PROJ)/target/release/libgblp1.a
GBLP1_ANDROID     := $(PROJ)/target/aarch64-linux-android/release/libgblp1.a
GBLP1_WINDOWS_X64 := $(PROJ)/target/x86_64-pc-windows-gnu/release/libgblp1.a
GBLP1_MACOS_X64   := $(PROJ)/target/x86_64-apple-darwin/release/libgblp1.a
GBLP1_MACOS_ARM64 := $(PROJ)/target/aarch64-apple-darwin/release/libgblp1.a
GBLP1_LINUX_MUSL  := $(PROJ)/target/x86_64-unknown-linux-musl/release/libgblp1.a

.PHONY: all android windows macos-x64 macos-arm64 linux clean \
        $(GBLP1_HOST) $(GBLP1_ANDROID) $(GBLP1_WINDOWS_X64) \
        $(GBLP1_MACOS_X64) $(GBLP1_MACOS_ARM64) $(GBLP1_LINUX_MUSL)

all: gblp1-inspect

$(GBLP1_HOST):
	cd $(PROJ) && cargo build --release -p gblp1
$(GBLP1_ANDROID):
	cd $(PROJ) && cargo build --release --target aarch64-linux-android -p gblp1
$(GBLP1_WINDOWS_X64):
	cd $(PROJ) && cargo build --release --target x86_64-pc-windows-gnu -p gblp1
$(GBLP1_MACOS_X64):
	cd $(PROJ) && cargo build --release --target x86_64-apple-darwin -p gblp1
$(GBLP1_MACOS_ARM64):
	cd $(PROJ) && cargo build --release --target aarch64-apple-darwin -p gblp1
$(GBLP1_LINUX_MUSL):
	cd $(PROJ) && cargo build --release --target x86_64-unknown-linux-musl -p gblp1

gblp1-inspect: $(SRCS) $(HDRS) $(GBLP1_HOST)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(GBLP1_HOST)

# --- Android cross-compile target (NDK r27) ---
NDK     ?= $(ANDROID_NDK)
NDK_CC   = $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android31-clang
NDK_CFLAGS = -static -O2 -Wall -Wextra -std=c99 -DGBL_HOST_BUILD=1

android: gblp1-inspect-android

gblp1-inspect-android: $(SRCS) $(HDRS) $(GBLP1_ANDROID)
	$(NDK_CC) $(NDK_CFLAGS) -o $@ $(SRCS) $(GBLP1_ANDROID)

clean:
	rm -f gblp1-inspect gblp1-inspect-android gblp1-inspect.exe \
	      gblp1-inspect-macos-x64 gblp1-inspect-macos-arm64 gblp1-inspect-linux

# --- Windows / macOS / Linux cross-compile (zig cc) ---
XCFLAGS = -O2 -Wall -Wextra -Werror -std=c99 -DGBL_HOST_BUILD=1

windows:     gblp1-inspect.exe
macos-x64:   gblp1-inspect-macos-x64
macos-arm64: gblp1-inspect-macos-arm64
linux:       gblp1-inspect-linux

gblp1-inspect.exe: $(SRCS) $(HDRS) $(GBLP1_WINDOWS_X64)
	zig cc -target x86_64-windows-gnu $(XCFLAGS) -o $@ $(SRCS) $(GBLP1_WINDOWS_X64)

gblp1-inspect-macos-x64: $(SRCS) $(HDRS) $(GBLP1_MACOS_X64)
	zig cc -target x86_64-macos $(XCFLAGS) -o $@ $(SRCS) $(GBLP1_MACOS_X64)

gblp1-inspect-macos-arm64: $(SRCS) $(HDRS) $(GBLP1_MACOS_ARM64)
	zig cc -target aarch64-macos $(XCFLAGS) -o $@ $(SRCS) $(GBLP1_MACOS_ARM64)

gblp1-inspect-linux: $(SRCS) $(HDRS) $(GBLP1_LINUX_MUSL)
	zig cc -target x86_64-linux-musl $(XCFLAGS) -static -o $@ $(SRCS) $(GBLP1_LINUX_MUSL)

# Project version (single source of truth: top-level VERSION file).
GBL_TOOL_VERSION := $(shell tr -d '[:space:]' < $(dir $(lastword $(MAKEFILE_LIST)))../../VERSION)
ifeq ($(GBL_TOOL_VERSION),)
$(error VERSION file is empty or missing)
endif
CFLAGS  += -DGBL_TOOL_VERSION='"$(GBL_TOOL_VERSION)"'
XCFLAGS += -DGBL_TOOL_VERSION='"$(GBL_TOOL_VERSION)"'
NDK_CFLAGS += -DGBL_TOOL_VERSION='"$(GBL_TOOL_VERSION)"'

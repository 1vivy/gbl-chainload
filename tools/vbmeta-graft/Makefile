# tools/vbmeta-graft/Makefile
#
# PR2 Task 4: gbl_sha256 single-shot + streaming context ship in
# libgblp1.a from crates/gblp1.
# PR2 Task 7: AVB structure parsing now ships in libavb_parse.a from
# crates/avb-parse. The deleted C library (AvbParse.c +
# Internal/AvbBigEndian.h) is gone; the public header
# `crates/avb-parse/include/avb_parse_ffi.h` re-declares the same C ABI
# (struct layouts + EFI_STATUS-returning AvbParse_* entry points +
# inline AvbReadU{32,64}Be helpers).
PROJ    := $(realpath ../..)
AVB_INC := $(PROJ)/crates/avb-parse/include
INCS    := -I$(AVB_INC) -I$(PROJ)/GblChainloadPkg/Include/Library \
           -DGBL_HOST_BUILD=1
CFLAGS  ?= -O1 -g -Wall -Wextra -std=c11
CFLAGS  += -D__HOST_BUILD__ $(INCS)

SRCS := vbmeta-graft.c

GBLP1_HOST        := $(PROJ)/target/release/libgblp1.a
GBLP1_ANDROID     := $(PROJ)/target/aarch64-linux-android/release/libgblp1.a
GBLP1_WINDOWS_X64 := $(PROJ)/target/x86_64-pc-windows-gnu/release/libgblp1.a
GBLP1_MACOS_X64   := $(PROJ)/target/x86_64-apple-darwin/release/libgblp1.a
GBLP1_MACOS_ARM64 := $(PROJ)/target/aarch64-apple-darwin/release/libgblp1.a
GBLP1_LINUX_MUSL  := $(PROJ)/target/x86_64-unknown-linux-musl/release/libgblp1.a

AVB_HOST          := $(PROJ)/target/release/libavb_parse.a
AVB_ANDROID       := $(PROJ)/target/aarch64-linux-android/release/libavb_parse.a
AVB_WINDOWS_X64   := $(PROJ)/target/x86_64-pc-windows-gnu/release/libavb_parse.a
AVB_MACOS_X64     := $(PROJ)/target/x86_64-apple-darwin/release/libavb_parse.a
AVB_MACOS_ARM64   := $(PROJ)/target/aarch64-apple-darwin/release/libavb_parse.a
AVB_LINUX_MUSL    := $(PROJ)/target/x86_64-unknown-linux-musl/release/libavb_parse.a

.PHONY: all android windows macos-x64 macos-arm64 linux clean \
        $(GBLP1_HOST) $(GBLP1_ANDROID) $(GBLP1_WINDOWS_X64) \
        $(GBLP1_MACOS_X64) $(GBLP1_MACOS_ARM64) $(GBLP1_LINUX_MUSL) \
        $(AVB_HOST) $(AVB_ANDROID) $(AVB_WINDOWS_X64) \
        $(AVB_MACOS_X64) $(AVB_MACOS_ARM64) $(AVB_LINUX_MUSL)

all: vbmeta-graft

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

$(AVB_HOST):
	cd $(PROJ) && cargo build --release -p avb-parse
$(AVB_ANDROID):
	cd $(PROJ) && cargo build --release --target aarch64-linux-android -p avb-parse
$(AVB_WINDOWS_X64):
	cd $(PROJ) && cargo build --release --target x86_64-pc-windows-gnu -p avb-parse
$(AVB_MACOS_X64):
	cd $(PROJ) && cargo build --release --target x86_64-apple-darwin -p avb-parse
$(AVB_MACOS_ARM64):
	cd $(PROJ) && cargo build --release --target aarch64-apple-darwin -p avb-parse
$(AVB_LINUX_MUSL):
	cd $(PROJ) && cargo build --release --target x86_64-unknown-linux-musl -p avb-parse

vbmeta-graft: $(SRCS) $(GBLP1_HOST) $(AVB_HOST)
	$(CC) $(CFLAGS) $(SRCS) $(AVB_HOST) $(GBLP1_HOST) -o $@

# --- Android cross-compile target (NDK r27) ---
NDK     ?= $(ANDROID_NDK)
NDK_CC   = $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android31-clang
NDK_CFLAGS = -static -O2 -Wall -Wextra -std=c11 -D__HOST_BUILD__ $(INCS)

android: vbmeta-graft-android

vbmeta-graft-android: $(SRCS) $(GBLP1_ANDROID) $(AVB_ANDROID)
	$(NDK_CC) $(NDK_CFLAGS) $(SRCS) $(AVB_ANDROID) $(GBLP1_ANDROID) -o $@

clean:
	rm -f vbmeta-graft vbmeta-graft-android vbmeta-graft.exe \
	      vbmeta-graft-macos-x64 vbmeta-graft-macos-arm64 vbmeta-graft-linux

# --- Windows / macOS cross-compile (zig cc) ---
XCFLAGS = -O2 -Wall -Wextra -std=c11 -D__HOST_BUILD__ $(INCS)

windows:     vbmeta-graft.exe
macos-x64:   vbmeta-graft-macos-x64
macos-arm64: vbmeta-graft-macos-arm64
linux:       vbmeta-graft-linux

vbmeta-graft.exe: $(SRCS) $(GBLP1_WINDOWS_X64) $(AVB_WINDOWS_X64)
	zig cc -target x86_64-windows-gnu $(XCFLAGS) $(SRCS) $(AVB_WINDOWS_X64) $(GBLP1_WINDOWS_X64) -o $@

vbmeta-graft-macos-x64: $(SRCS) $(GBLP1_MACOS_X64) $(AVB_MACOS_X64)
	zig cc -target x86_64-macos $(XCFLAGS) $(SRCS) $(AVB_MACOS_X64) $(GBLP1_MACOS_X64) -o $@

vbmeta-graft-macos-arm64: $(SRCS) $(GBLP1_MACOS_ARM64) $(AVB_MACOS_ARM64)
	zig cc -target aarch64-macos $(XCFLAGS) $(SRCS) $(AVB_MACOS_ARM64) $(GBLP1_MACOS_ARM64) -o $@

vbmeta-graft-linux: $(SRCS) $(GBLP1_LINUX_MUSL) $(AVB_LINUX_MUSL)
	zig cc -target x86_64-linux-musl $(XCFLAGS) -static $(SRCS) $(AVB_LINUX_MUSL) $(GBLP1_LINUX_MUSL) -o $@

# Project version (single source of truth: top-level VERSION file).
GBL_TOOL_VERSION := $(shell tr -d '[:space:]' < $(dir $(lastword $(MAKEFILE_LIST)))../../VERSION)
ifeq ($(GBL_TOOL_VERSION),)
$(error VERSION file is empty or missing)
endif
CFLAGS  += -DGBL_TOOL_VERSION='"$(GBL_TOOL_VERSION)"'
XCFLAGS += -DGBL_TOOL_VERSION='"$(GBL_TOOL_VERSION)"'
NDK_CFLAGS += -DGBL_TOOL_VERSION='"$(GBL_TOOL_VERSION)"'

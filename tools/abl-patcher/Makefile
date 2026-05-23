CC      ?= cc
CFLAGS  ?= -O1 -g -Wall -Wextra -Wno-unused-parameter -std=c11
PROJ    := $(realpath ../..)
PE_INC  := $(PROJ)/crates/patch-engine/include

# PR2 Task 6: DynamicPatchLib's C sources moved into crates/patch-engine.
# This Makefile no longer recursively compiles DPL .c files; it links
# the host libpatch_engine.a (built with --features host) instead.

CFLAGS += -DGBL_HOST_BUILD -I$(PE_INC)

# Default target — make abl-patcher the goal when no goal is given.
all: abl-patcher
.PHONY: all

# Cargo rebuilds the staticlib only when needed; on a no-op cargo build
# returns immediately. The .PHONY dep is explicit so make defers
# stale-ness to cargo (the authoritative source).
.PHONY: cargo-patch-engine cargo-patch-engine-win-x64 cargo-patch-engine-mac-x64 cargo-patch-engine-mac-arm64 cargo-patch-engine-linux-musl cargo-patch-engine-android

PE_LIB_HOST          := $(PROJ)/target/release/libpatch_engine.a
PE_LIB_WIN_X64       := $(PROJ)/target/x86_64-pc-windows-gnu/release/libpatch_engine.a
PE_LIB_MAC_X64       := $(PROJ)/target/x86_64-apple-darwin/release/libpatch_engine.a
PE_LIB_MAC_ARM64     := $(PROJ)/target/aarch64-apple-darwin/release/libpatch_engine.a
PE_LIB_LINUX_MUSL    := $(PROJ)/target/x86_64-unknown-linux-musl/release/libpatch_engine.a
PE_LIB_ANDROID       := $(PROJ)/target/aarch64-linux-android/release/libpatch_engine.a

cargo-patch-engine:
	@cd $(PROJ) && cargo build --release -p patch-engine --features host

cargo-patch-engine-win-x64:
	@cd $(PROJ) && cargo build --release --target x86_64-pc-windows-gnu -p patch-engine --features host

cargo-patch-engine-mac-x64:
	@cd $(PROJ) && cargo build --release --target x86_64-apple-darwin -p patch-engine --features host

cargo-patch-engine-mac-arm64:
	@cd $(PROJ) && cargo build --release --target aarch64-apple-darwin -p patch-engine --features host

cargo-patch-engine-linux-musl:
	@cd $(PROJ) && cargo build --release --target x86_64-unknown-linux-musl -p patch-engine --features host

cargo-patch-engine-android:
	@cd $(PROJ) && cargo build --release --target aarch64-linux-android -p patch-engine --features host

abl-patcher: abl-patcher.c cargo-patch-engine
	$(CC) $(CFLAGS) $< $(PE_LIB_HOST) -o $@

clean:
	rm -f abl-patcher abl-patcher-android abl-patcher.exe \
	      abl-patcher-macos-x64 abl-patcher-macos-arm64 abl-patcher-linux *.o

# --- Windows / macOS cross-compile (zig cc + cargo cross targets) ---
XCFLAGS = -O2 -Wall -Wextra -Wno-unused-parameter -std=c11 \
          -DGBL_HOST_BUILD -I$(PE_INC)

windows:     abl-patcher.exe
macos-x64:   abl-patcher-macos-x64
macos-arm64: abl-patcher-macos-arm64
linux:       abl-patcher-linux

abl-patcher.exe: abl-patcher.c cargo-patch-engine-win-x64
	zig cc -target x86_64-windows-gnu $(XCFLAGS) $< $(PE_LIB_WIN_X64) -o $@

abl-patcher-macos-x64: abl-patcher.c cargo-patch-engine-mac-x64
	zig cc -target x86_64-macos $(XCFLAGS) $< $(PE_LIB_MAC_X64) -o $@

abl-patcher-macos-arm64: abl-patcher.c cargo-patch-engine-mac-arm64
	zig cc -target aarch64-macos $(XCFLAGS) $< $(PE_LIB_MAC_ARM64) -o $@

abl-patcher-linux: abl-patcher.c cargo-patch-engine-linux-musl
	zig cc -target x86_64-linux-musl $(XCFLAGS) -static $< $(PE_LIB_LINUX_MUSL) -o $@

# Quick self-check: build and exercise against the infiniti fixture.
test: abl-patcher
	./abl-patcher --in $(PROJ)/images/infiniti/LinuxLoader_infiniti.efi --check-anchors-only
	./abl-patcher --in $(PROJ)/images/infiniti/LinuxLoader_infiniti.efi --out /tmp/patched-infiniti.bin
	@echo "ok abl-patcher self-test"

# --- Android cross-compile target (NDK r27) ---
NDK     ?= $(ANDROID_NDK)
NDK_CC   = $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android31-clang
NDK_CFLAGS = -static -O2 -Wall -Wextra -Wno-unused-parameter -std=c11 \
             -DGBL_HOST_BUILD -I$(PE_INC)

android: abl-patcher-android

abl-patcher-android: abl-patcher.c cargo-patch-engine-android
	$(NDK_CC) $(NDK_CFLAGS) $< $(PE_LIB_ANDROID) -o $@

# Project version (single source of truth: top-level VERSION file).
GBL_TOOL_VERSION := $(shell tr -d '[:space:]' < $(dir $(lastword $(MAKEFILE_LIST)))../../VERSION)
ifeq ($(GBL_TOOL_VERSION),)
$(error VERSION file is empty or missing)
endif
CFLAGS  += -DGBL_TOOL_VERSION='"$(GBL_TOOL_VERSION)"'
XCFLAGS += -DGBL_TOOL_VERSION='"$(GBL_TOOL_VERSION)"'
NDK_CFLAGS += -DGBL_TOOL_VERSION='"$(GBL_TOOL_VERSION)"'

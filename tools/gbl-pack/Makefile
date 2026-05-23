# tools/gbl-pack/Makefile
#
# PR2 Task 3: gbl_pe_sanity + gbl_contains_utf16_efisp moved into
# crates/pe-utils (Rust). Each cross target links the matching cargo
# target's libpe_utils.a.
#
# PR2 Task 4: gbl_sha256 + gbl_crc32 (and the GBLP1 parser itself) moved
# into crates/gblp1 (Rust). Each cross target now links both libpe_utils.a
# AND libgblp1.a from the matching `cargo build --release --target <T>`
# output.
CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -Werror -O2 -std=c99
PROJ    := $(realpath ../..)

CFLAGS  += -DGBL_HOST_BUILD=1

.PHONY: all android windows macos-x64 macos-arm64 linux clean

# Default target — declared up here so `make` builds gbl-pack rather than
# tripping over the cargo staticlib rules listed below.
all: gbl-pack

# crates/pe-utils + crates/gblp1 staticlibs. One per cross target.
# Cargo emits these on `cargo build --release --target <T> -p <crate>`.
PE_UTILS_HOST        := $(PROJ)/target/release/libpe_utils.a
PE_UTILS_ANDROID     := $(PROJ)/target/aarch64-linux-android/release/libpe_utils.a
PE_UTILS_WINDOWS_X64 := $(PROJ)/target/x86_64-pc-windows-gnu/release/libpe_utils.a
PE_UTILS_MACOS_X64   := $(PROJ)/target/x86_64-apple-darwin/release/libpe_utils.a
PE_UTILS_MACOS_ARM64 := $(PROJ)/target/aarch64-apple-darwin/release/libpe_utils.a
PE_UTILS_LINUX_MUSL  := $(PROJ)/target/x86_64-unknown-linux-musl/release/libpe_utils.a

GBLP1_HOST        := $(PROJ)/target/release/libgblp1.a
GBLP1_ANDROID     := $(PROJ)/target/aarch64-linux-android/release/libgblp1.a
GBLP1_WINDOWS_X64 := $(PROJ)/target/x86_64-pc-windows-gnu/release/libgblp1.a
GBLP1_MACOS_X64   := $(PROJ)/target/x86_64-apple-darwin/release/libgblp1.a
GBLP1_MACOS_ARM64 := $(PROJ)/target/aarch64-apple-darwin/release/libgblp1.a
GBLP1_LINUX_MUSL  := $(PROJ)/target/x86_64-unknown-linux-musl/release/libgblp1.a

# Treat the staticlib targets as always-stale so cargo decides when to
# rebuild — cargo is fast on no-op.
.PHONY: $(PE_UTILS_HOST) $(PE_UTILS_ANDROID) $(PE_UTILS_WINDOWS_X64) \
        $(PE_UTILS_MACOS_X64) $(PE_UTILS_MACOS_ARM64) $(PE_UTILS_LINUX_MUSL) \
        $(GBLP1_HOST) $(GBLP1_ANDROID) $(GBLP1_WINDOWS_X64) \
        $(GBLP1_MACOS_X64) $(GBLP1_MACOS_ARM64) $(GBLP1_LINUX_MUSL)

# Rule helpers — each rebuilds the rust crate(s) via cargo for its target.
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

gbl-pack: gbl-pack.c pack.c pack.h ../shared/gblp1.h ../shared/gbl_mode2_profile.h \
          $(PE_UTILS_HOST) $(GBLP1_HOST)
	$(CC) $(CFLAGS) -o $@ gbl-pack.c pack.c \
	      $(PE_UTILS_HOST) $(GBLP1_HOST)

# --- Android cross-compile target (NDK r27) ---
NDK     ?= $(ANDROID_NDK)
NDK_CC   = $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android31-clang
NDK_CFLAGS = -static -O2 -Wall -Wextra -std=c99 -DGBL_HOST_BUILD=1

android: gbl-pack-android

gbl-pack-android: gbl-pack.c pack.c pack.h ../shared/gblp1.h ../shared/gbl_mode2_profile.h \
                  $(PE_UTILS_ANDROID) $(GBLP1_ANDROID)
	$(NDK_CC) $(NDK_CFLAGS) -o $@ gbl-pack.c pack.c \
	      $(PE_UTILS_ANDROID) $(GBLP1_ANDROID)

clean:
	rm -f gbl-pack gbl-pack-android gbl-pack.exe \
	      gbl-pack-macos-x64 gbl-pack-macos-arm64 gbl-pack-linux Sha256.o

# --- Windows / macOS cross-compile (zig cc) ---
XCFLAGS = -O2 -Wall -Wextra -Werror -std=c99 -DGBL_HOST_BUILD=1
XSRCS   = gbl-pack.c pack.c

windows:     gbl-pack.exe
macos-x64:   gbl-pack-macos-x64
macos-arm64: gbl-pack-macos-arm64
linux:       gbl-pack-linux

gbl-pack.exe: $(XSRCS) $(PE_UTILS_WINDOWS_X64) $(GBLP1_WINDOWS_X64)
	zig cc -target x86_64-windows-gnu $(XCFLAGS) -o $@ $(XSRCS) \
	    $(PE_UTILS_WINDOWS_X64) $(GBLP1_WINDOWS_X64)

gbl-pack-macos-x64: $(XSRCS) $(PE_UTILS_MACOS_X64) $(GBLP1_MACOS_X64)
	zig cc -target x86_64-macos $(XCFLAGS) -o $@ $(XSRCS) \
	    $(PE_UTILS_MACOS_X64) $(GBLP1_MACOS_X64)

gbl-pack-macos-arm64: $(XSRCS) $(PE_UTILS_MACOS_ARM64) $(GBLP1_MACOS_ARM64)
	zig cc -target aarch64-macos $(XCFLAGS) -o $@ $(XSRCS) \
	    $(PE_UTILS_MACOS_ARM64) $(GBLP1_MACOS_ARM64)

gbl-pack-linux: $(XSRCS) $(PE_UTILS_LINUX_MUSL) $(GBLP1_LINUX_MUSL)
	zig cc -target x86_64-linux-musl $(XCFLAGS) -static -o $@ $(XSRCS) \
	    $(PE_UTILS_LINUX_MUSL) $(GBLP1_LINUX_MUSL)

# Project version (single source of truth: top-level VERSION file).
GBL_TOOL_VERSION := $(shell tr -d '[:space:]' < $(dir $(lastword $(MAKEFILE_LIST)))../../VERSION)
ifeq ($(GBL_TOOL_VERSION),)
$(error VERSION file is empty or missing)
endif
CFLAGS  += -DGBL_TOOL_VERSION='"$(GBL_TOOL_VERSION)"'
XCFLAGS += -DGBL_TOOL_VERSION='"$(GBL_TOOL_VERSION)"'
NDK_CFLAGS += -DGBL_TOOL_VERSION='"$(GBL_TOOL_VERSION)"'

# tools/mode2-profile/Makefile
#
# PR2 Task 4: gbl_sha256 ships in libgblp1.a from crates/gblp1.
# PR2 Task 5: TOML parsing + compile/parse logic ships in
#   libmode2_profile_core.a from crates/mode2-profile-core. The
#   vendored tomlc99 is gone; the Rust crate uses the `toml` crate from
#   crates.io. The C tool keeps the derive path (AvbParseLib + sha256)
#   for now; PR2 Task 8 folds both paths into the `gbl` multicall.
# PR2 Task 7: AvbParseLib ships in libavb_parse.a from crates/avb-parse;
#   AvbParse.c and Internal/AvbBigEndian.h are gone.
CC      ?= gcc
PROJ    := $(realpath ../..)
AVB_INC := $(PROJ)/crates/avb-parse/include
INCS    := -I$(AVB_INC) -I$(PROJ)/GblChainloadPkg/Include/Library \
           -D__HOST_BUILD__
CFLAGS  ?= -Wall -Wextra -Werror -O2 -std=c11 -DGBL_HOST_BUILD=1 $(INCS)
SRCS     = mode2-profile.c
HDRS     = ../shared/gbl_mode2_profile.h \
           ../../crates/gblp1/include/gblp1_ffi.h \
           ../../crates/mode2-profile-core/include/mode2_profile_ffi.h \
           ../../crates/avb-parse/include/avb_parse_ffi.h \
           $(PROJ)/GblChainloadPkg/Include/Library/AvbParseLib.h

GBLP1_HOST        := $(PROJ)/target/release/libgblp1.a
GBLP1_ANDROID     := $(PROJ)/target/aarch64-linux-android/release/libgblp1.a
GBLP1_WINDOWS_X64 := $(PROJ)/target/x86_64-pc-windows-gnu/release/libgblp1.a
GBLP1_MACOS_X64   := $(PROJ)/target/x86_64-apple-darwin/release/libgblp1.a
GBLP1_MACOS_ARM64 := $(PROJ)/target/aarch64-apple-darwin/release/libgblp1.a
GBLP1_LINUX_MUSL  := $(PROJ)/target/x86_64-unknown-linux-musl/release/libgblp1.a

MODE2_HOST        := $(PROJ)/target/release/libmode2_profile_core.a
MODE2_ANDROID     := $(PROJ)/target/aarch64-linux-android/release/libmode2_profile_core.a
MODE2_WINDOWS_X64 := $(PROJ)/target/x86_64-pc-windows-gnu/release/libmode2_profile_core.a
MODE2_MACOS_X64   := $(PROJ)/target/x86_64-apple-darwin/release/libmode2_profile_core.a
MODE2_MACOS_ARM64 := $(PROJ)/target/aarch64-apple-darwin/release/libmode2_profile_core.a
MODE2_LINUX_MUSL  := $(PROJ)/target/x86_64-unknown-linux-musl/release/libmode2_profile_core.a

AVB_HOST          := $(PROJ)/target/release/libavb_parse.a
AVB_ANDROID       := $(PROJ)/target/aarch64-linux-android/release/libavb_parse.a
AVB_WINDOWS_X64   := $(PROJ)/target/x86_64-pc-windows-gnu/release/libavb_parse.a
AVB_MACOS_X64     := $(PROJ)/target/x86_64-apple-darwin/release/libavb_parse.a
AVB_MACOS_ARM64   := $(PROJ)/target/aarch64-apple-darwin/release/libavb_parse.a
AVB_LINUX_MUSL    := $(PROJ)/target/x86_64-unknown-linux-musl/release/libavb_parse.a

.PHONY: all android windows macos-x64 macos-arm64 linux clean \
        $(GBLP1_HOST) $(GBLP1_ANDROID) $(GBLP1_WINDOWS_X64) \
        $(GBLP1_MACOS_X64) $(GBLP1_MACOS_ARM64) $(GBLP1_LINUX_MUSL) \
        $(MODE2_HOST) $(MODE2_ANDROID) $(MODE2_WINDOWS_X64) \
        $(MODE2_MACOS_X64) $(MODE2_MACOS_ARM64) $(MODE2_LINUX_MUSL) \
        $(AVB_HOST) $(AVB_ANDROID) $(AVB_WINDOWS_X64) \
        $(AVB_MACOS_X64) $(AVB_MACOS_ARM64) $(AVB_LINUX_MUSL)

all: mode2-profile

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

$(MODE2_HOST):
	cd $(PROJ) && cargo build --release -p mode2-profile-core
$(MODE2_ANDROID):
	cd $(PROJ) && cargo build --release --target aarch64-linux-android -p mode2-profile-core
$(MODE2_WINDOWS_X64):
	cd $(PROJ) && cargo build --release --target x86_64-pc-windows-gnu -p mode2-profile-core
$(MODE2_MACOS_X64):
	cd $(PROJ) && cargo build --release --target x86_64-apple-darwin -p mode2-profile-core
$(MODE2_MACOS_ARM64):
	cd $(PROJ) && cargo build --release --target aarch64-apple-darwin -p mode2-profile-core
$(MODE2_LINUX_MUSL):
	cd $(PROJ) && cargo build --release --target x86_64-unknown-linux-musl -p mode2-profile-core

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

# AvbReadU{32,64}Be are static inline in the new ffi header; only
# AvbReadU64Be is called by mode2-profile.c, so the static AvbReadU32Be
# is dead-stripped without a warning under `static inline`.
mode2-profile: $(SRCS) $(HDRS) $(GBLP1_HOST) $(MODE2_HOST) $(AVB_HOST)
	$(CC) $(CFLAGS) -o $@ $(SRCS) \
	    $(AVB_HOST) $(MODE2_HOST) $(GBLP1_HOST) -lm

# --- Android cross-compile (NDK r27) ---
NDK     ?= $(ANDROID_NDK)
NDK_CC   = $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android31-clang
NDK_CFLAGS = -static -O2 -Wall -Wextra -Werror -std=c11 -DGBL_HOST_BUILD=1 $(INCS)

android: mode2-profile-android

mode2-profile-android: $(SRCS) $(HDRS) $(GBLP1_ANDROID) $(MODE2_ANDROID) $(AVB_ANDROID)
	$(NDK_CC) $(NDK_CFLAGS) -o $@ $(SRCS) \
	    $(AVB_ANDROID) $(MODE2_ANDROID) $(GBLP1_ANDROID) -lm

clean:
	rm -f mode2-profile mode2-profile-android \
	      mode2-profile.exe mode2-profile-macos-x64 mode2-profile-macos-arm64 \
	      mode2-profile-linux \
	      m2p-*.o avbparse*.o mode2-profile.o mode2-profile-android.o

# --- Windows / macOS cross-compile (zig cc) ---
XCFLAGS = -O2 -Wall -Wextra -Werror -std=c11 -DGBL_HOST_BUILD=1 $(INCS)

windows:     mode2-profile.exe
macos-x64:   mode2-profile-macos-x64
macos-arm64: mode2-profile-macos-arm64
linux:       mode2-profile-linux

mode2-profile.exe: $(SRCS) $(HDRS) $(GBLP1_WINDOWS_X64) $(MODE2_WINDOWS_X64) $(AVB_WINDOWS_X64)
	zig cc -target x86_64-windows-gnu $(XCFLAGS) -o $@ $(SRCS) \
	    $(AVB_WINDOWS_X64) $(MODE2_WINDOWS_X64) $(GBLP1_WINDOWS_X64)

mode2-profile-macos-x64: $(SRCS) $(HDRS) $(GBLP1_MACOS_X64) $(MODE2_MACOS_X64) $(AVB_MACOS_X64)
	zig cc -target x86_64-macos $(XCFLAGS) -o $@ $(SRCS) \
	    $(AVB_MACOS_X64) $(MODE2_MACOS_X64) $(GBLP1_MACOS_X64)

mode2-profile-macos-arm64: $(SRCS) $(HDRS) $(GBLP1_MACOS_ARM64) $(MODE2_MACOS_ARM64) $(AVB_MACOS_ARM64)
	zig cc -target aarch64-macos $(XCFLAGS) -o $@ $(SRCS) \
	    $(AVB_MACOS_ARM64) $(MODE2_MACOS_ARM64) $(GBLP1_MACOS_ARM64)

mode2-profile-linux: $(SRCS) $(HDRS) $(GBLP1_LINUX_MUSL) $(MODE2_LINUX_MUSL) $(AVB_LINUX_MUSL)
	zig cc -target x86_64-linux-musl $(XCFLAGS) -static -o $@ $(SRCS) \
	    $(AVB_LINUX_MUSL) $(MODE2_LINUX_MUSL) $(GBLP1_LINUX_MUSL)

# Project version (single source of truth: top-level VERSION file).
GBL_TOOL_VERSION := $(shell tr -d '[:space:]' < $(dir $(lastword $(MAKEFILE_LIST)))../../VERSION)
ifeq ($(GBL_TOOL_VERSION),)
$(error VERSION file is empty or missing)
endif
CFLAGS  += -DGBL_TOOL_VERSION='"$(GBL_TOOL_VERSION)"'
XCFLAGS += -DGBL_TOOL_VERSION='"$(GBL_TOOL_VERSION)"'
NDK_CFLAGS += -DGBL_TOOL_VERSION='"$(GBL_TOOL_VERSION)"'

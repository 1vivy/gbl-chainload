# tools/mode2-profile/Makefile
#
# PR2 Task 4: gbl_sha256 ships in libgblp1.a from crates/gblp1.
# PR2 Task 5: TOML parsing + compile/parse logic ships in
#   libmode2_profile_core.a from crates/mode2-profile-core. The vendored
#   tomlc99 is gone; the Rust crate uses the `toml` crate from crates.io.
#   The C tool keeps the derive path (AvbParseLib + sha256) for now;
#   PR2 Task 8 folds both paths into the `gbl` multicall.
CC      ?= gcc
PROJ    := $(realpath ../..)
AVB     := $(PROJ)/GblChainloadPkg/Library/AvbParseLib
INCS    := -I$(AVB)/Internal -I$(PROJ)/GblChainloadPkg/Include/Library \
           -D__HOST_BUILD__
CFLAGS  ?= -Wall -Wextra -Werror -O2 -std=c11 -DGBL_HOST_BUILD=1 $(INCS)
SRCS     = mode2-profile.c $(AVB)/AvbParse.c
HDRS     = ../shared/gbl_mode2_profile.h \
           ../../crates/gblp1/include/gblp1_ffi.h \
           ../../crates/mode2-profile-core/include/mode2_profile_ffi.h \
           $(PROJ)/GblChainloadPkg/Include/Library/AvbParseLib.h \
           $(AVB)/Internal/AvbBigEndian.h

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

.PHONY: all android windows macos-x64 macos-arm64 linux clean \
        $(GBLP1_HOST) $(GBLP1_ANDROID) $(GBLP1_WINDOWS_X64) \
        $(GBLP1_MACOS_X64) $(GBLP1_MACOS_ARM64) $(GBLP1_LINUX_MUSL) \
        $(MODE2_HOST) $(MODE2_ANDROID) $(MODE2_WINDOWS_X64) \
        $(MODE2_MACOS_X64) $(MODE2_MACOS_ARM64) $(MODE2_LINUX_MUSL)

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

# -Wno-unused-function on mode2-profile.o: AvbBigEndian.h defines
# AvbReadU32Be as a static function; we only call AvbReadU64Be here.
mode2-profile: $(SRCS) $(HDRS) $(GBLP1_HOST) $(MODE2_HOST)
	$(CC) $(CFLAGS) -Wno-unused-function -c -o mode2-profile.o mode2-profile.c
	$(CC) $(CFLAGS) -c -o avbparse.o $(AVB)/AvbParse.c
	$(CC) $(CFLAGS) -o $@ mode2-profile.o avbparse.o \
	    $(MODE2_HOST) $(GBLP1_HOST) -lm

# --- Android cross-compile (NDK r27) ---
NDK     ?= $(ANDROID_NDK)
NDK_CC   = $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android31-clang
NDK_CFLAGS = -static -O2 -Wall -Wextra -Werror -std=c11 -DGBL_HOST_BUILD=1 $(INCS)

android: mode2-profile-android

mode2-profile-android: $(SRCS) $(HDRS) $(GBLP1_ANDROID) $(MODE2_ANDROID)
	$(NDK_CC) $(NDK_CFLAGS) -Wno-unused-function -c -o mode2-profile-android.o mode2-profile.c
	$(NDK_CC) $(NDK_CFLAGS) -c -o avbparse-android.o $(AVB)/AvbParse.c
	$(NDK_CC) $(NDK_CFLAGS) -o $@ mode2-profile-android.o avbparse-android.o \
	    $(MODE2_ANDROID) $(GBLP1_ANDROID) -lm

clean:
	rm -f mode2-profile mode2-profile-android mode2-profile.o \
	      mode2-profile-android.o \
	      mode2-profile.exe mode2-profile-macos-x64 mode2-profile-macos-arm64 \
	      mode2-profile-linux \
	      m2p-*.o avbparse*.o

# --- Windows / macOS cross-compile (zig cc) ---
XCFLAGS = -O2 -Wall -Wextra -Werror -std=c11 -DGBL_HOST_BUILD=1 $(INCS)

windows:     mode2-profile.exe
macos-x64:   mode2-profile-macos-x64
macos-arm64: mode2-profile-macos-arm64
linux:       mode2-profile-linux

mode2-profile.exe: $(SRCS) $(HDRS) $(GBLP1_WINDOWS_X64) $(MODE2_WINDOWS_X64)
	zig cc -target x86_64-windows-gnu $(XCFLAGS) -Wno-unused-function -c -o m2p-win.o mode2-profile.c
	zig cc -target x86_64-windows-gnu $(XCFLAGS) -c -o avbparse-win.o $(AVB)/AvbParse.c
	zig cc -target x86_64-windows-gnu $(XCFLAGS) -o $@ \
	    m2p-win.o avbparse-win.o $(MODE2_WINDOWS_X64) $(GBLP1_WINDOWS_X64)

mode2-profile-macos-x64: $(SRCS) $(HDRS) $(GBLP1_MACOS_X64) $(MODE2_MACOS_X64)
	zig cc -target x86_64-macos $(XCFLAGS) -Wno-unused-function -c -o m2p-macx64.o mode2-profile.c
	zig cc -target x86_64-macos $(XCFLAGS) -c -o avbparse-macx64.o $(AVB)/AvbParse.c
	zig cc -target x86_64-macos $(XCFLAGS) -o $@ \
	    m2p-macx64.o avbparse-macx64.o $(MODE2_MACOS_X64) $(GBLP1_MACOS_X64)

mode2-profile-macos-arm64: $(SRCS) $(HDRS) $(GBLP1_MACOS_ARM64) $(MODE2_MACOS_ARM64)
	zig cc -target aarch64-macos $(XCFLAGS) -Wno-unused-function -c -o m2p-macarm64.o mode2-profile.c
	zig cc -target aarch64-macos $(XCFLAGS) -c -o avbparse-macarm64.o $(AVB)/AvbParse.c
	zig cc -target aarch64-macos $(XCFLAGS) -o $@ \
	    m2p-macarm64.o avbparse-macarm64.o $(MODE2_MACOS_ARM64) $(GBLP1_MACOS_ARM64)

mode2-profile-linux: $(SRCS) $(HDRS) $(GBLP1_LINUX_MUSL) $(MODE2_LINUX_MUSL)
	zig cc -target x86_64-linux-musl $(XCFLAGS) -static -Wno-unused-function -c -o m2p-linux.o mode2-profile.c
	zig cc -target x86_64-linux-musl $(XCFLAGS) -static -c -o avbparse-linux.o $(AVB)/AvbParse.c
	zig cc -target x86_64-linux-musl $(XCFLAGS) -static -o $@ \
	    m2p-linux.o avbparse-linux.o $(MODE2_LINUX_MUSL) $(GBLP1_LINUX_MUSL)

# Project version (single source of truth: top-level VERSION file).
GBL_TOOL_VERSION := $(shell tr -d '[:space:]' < $(dir $(lastword $(MAKEFILE_LIST)))../../VERSION)
ifeq ($(GBL_TOOL_VERSION),)
$(error VERSION file is empty or missing)
endif
CFLAGS  += -DGBL_TOOL_VERSION='"$(GBL_TOOL_VERSION)"'
XCFLAGS += -DGBL_TOOL_VERSION='"$(GBL_TOOL_VERSION)"'
NDK_CFLAGS += -DGBL_TOOL_VERSION='"$(GBL_TOOL_VERSION)"'

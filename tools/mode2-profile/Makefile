# tools/mode2-profile/Makefile
CC      ?= gcc
PROJ    := $(realpath ../..)
GPL     := $(PROJ)/GblChainloadPkg/Library/GblPayloadLib
AVB     := $(PROJ)/GblChainloadPkg/Library/AvbParseLib
INCS    := -I$(GPL) -I$(AVB)/Internal -I$(PROJ)/GblChainloadPkg/Include/Library \
           -D__HOST_BUILD__
CFLAGS  ?= -Wall -Wextra -Werror -O2 -std=c11 -DGBL_HOST_BUILD=1 $(INCS)
SRCS     = mode2-profile.c vendor/tomlc99/toml.c $(GPL)/Sha256.c $(AVB)/AvbParse.c
HDRS     = vendor/tomlc99/toml.h ../shared/gbl_mode2_profile.h \
           $(GPL)/Internal/Sha256.h \
           $(PROJ)/GblChainloadPkg/Include/Library/AvbParseLib.h \
           $(AVB)/Internal/AvbBigEndian.h

all: mode2-profile

# Build in two steps so we can suppress warnings only for the vendored TU.
# -Wno-unused-function on mode2-profile.o: AvbBigEndian.h defines AvbReadU32Be
# as a STATIC (static) function; we only call AvbReadU64Be in this TU.
mode2-profile: $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -Wno-unused-function -c -o mode2-profile.o mode2-profile.c
	$(CC) -w -O2 -std=c11 -DGBL_HOST_BUILD=1 -c -o toml.o vendor/tomlc99/toml.c
	$(CC) $(CFLAGS) -c -o sha256.o $(GPL)/Sha256.c
	$(CC) $(CFLAGS) -c -o avbparse.o $(AVB)/AvbParse.c
	$(CC) $(CFLAGS) -o $@ mode2-profile.o toml.o sha256.o avbparse.o

# --- Android cross-compile (NDK r27) ---
NDK     ?= $(ANDROID_NDK)
NDK_CC   = $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android31-clang
NDK_CFLAGS = -static -O2 -Wall -Wextra -Werror -std=c11 -DGBL_HOST_BUILD=1 $(INCS)

android: mode2-profile-android

mode2-profile-android: $(SRCS) $(HDRS)
	$(NDK_CC) $(NDK_CFLAGS) -Wno-unused-function -c -o mode2-profile-android.o mode2-profile.c
	$(NDK_CC) -w -static -O2 -std=c11 -DGBL_HOST_BUILD=1 -c -o toml-android.o vendor/tomlc99/toml.c
	$(NDK_CC) $(NDK_CFLAGS) -c -o sha256-android.o $(GPL)/Sha256.c
	$(NDK_CC) $(NDK_CFLAGS) -c -o avbparse-android.o $(AVB)/AvbParse.c
	$(NDK_CC) $(NDK_CFLAGS) -o $@ mode2-profile-android.o toml-android.o sha256-android.o avbparse-android.o

clean:
	rm -f mode2-profile mode2-profile-android mode2-profile.o toml.o sha256.o \
	      mode2-profile-android.o toml-android.o sha256-android.o \
	      mode2-profile.exe mode2-profile-macos-x64 mode2-profile-macos-arm64 \
	      mode2-profile-linux \
	      m2p-*.o toml-*.o sha256-*.o \
	      avbparse.o avbparse-android.o avbparse-win.o avbparse-macx64.o avbparse-macarm64.o avbparse-linux.o

# --- Windows / macOS cross-compile (zig cc) ---
XCFLAGS = -O2 -Wall -Wextra -Werror -std=c11 -DGBL_HOST_BUILD=1 $(INCS)

windows:     mode2-profile.exe
macos-x64:   mode2-profile-macos-x64
macos-arm64: mode2-profile-macos-arm64
linux:       mode2-profile-linux

mode2-profile.exe: $(SRCS) $(HDRS)
	zig cc -target x86_64-windows-gnu $(XCFLAGS) -Wno-unused-function -c -o m2p-win.o mode2-profile.c
	zig cc -target x86_64-windows-gnu -w -O2 -std=c11 -DGBL_HOST_BUILD=1 \
	    -c -o toml-win.o vendor/tomlc99/toml.c
	zig cc -target x86_64-windows-gnu $(XCFLAGS) -c -o sha256-win.o $(GPL)/Sha256.c
	zig cc -target x86_64-windows-gnu $(XCFLAGS) -c -o avbparse-win.o $(AVB)/AvbParse.c
	zig cc -target x86_64-windows-gnu $(XCFLAGS) -o $@ \
	    m2p-win.o toml-win.o sha256-win.o avbparse-win.o

mode2-profile-macos-x64: $(SRCS) $(HDRS)
	zig cc -target x86_64-macos $(XCFLAGS) -Wno-unused-function -c -o m2p-macx64.o mode2-profile.c
	zig cc -target x86_64-macos -w -O2 -std=c11 -DGBL_HOST_BUILD=1 \
	    -c -o toml-macx64.o vendor/tomlc99/toml.c
	zig cc -target x86_64-macos $(XCFLAGS) -c -o sha256-macx64.o $(GPL)/Sha256.c
	zig cc -target x86_64-macos $(XCFLAGS) -c -o avbparse-macx64.o $(AVB)/AvbParse.c
	zig cc -target x86_64-macos $(XCFLAGS) -o $@ \
	    m2p-macx64.o toml-macx64.o sha256-macx64.o avbparse-macx64.o

mode2-profile-macos-arm64: $(SRCS) $(HDRS)
	zig cc -target aarch64-macos $(XCFLAGS) -Wno-unused-function -c -o m2p-macarm64.o mode2-profile.c
	zig cc -target aarch64-macos -w -O2 -std=c11 -DGBL_HOST_BUILD=1 \
	    -c -o toml-macarm64.o vendor/tomlc99/toml.c
	zig cc -target aarch64-macos $(XCFLAGS) -c -o sha256-macarm64.o $(GPL)/Sha256.c
	zig cc -target aarch64-macos $(XCFLAGS) -c -o avbparse-macarm64.o $(AVB)/AvbParse.c
	zig cc -target aarch64-macos $(XCFLAGS) -o $@ \
	    m2p-macarm64.o toml-macarm64.o sha256-macarm64.o avbparse-macarm64.o

mode2-profile-linux: $(SRCS) $(HDRS)
	zig cc -target x86_64-linux-musl $(XCFLAGS) -static -Wno-unused-function -c -o m2p-linux.o mode2-profile.c
	zig cc -target x86_64-linux-musl -static -w -O2 -std=c11 -DGBL_HOST_BUILD=1 \
	    -c -o toml-linux.o vendor/tomlc99/toml.c
	zig cc -target x86_64-linux-musl $(XCFLAGS) -static -c -o sha256-linux.o $(GPL)/Sha256.c
	zig cc -target x86_64-linux-musl $(XCFLAGS) -static -c -o avbparse-linux.o $(AVB)/AvbParse.c
	zig cc -target x86_64-linux-musl $(XCFLAGS) -static -o $@ \
	    m2p-linux.o toml-linux.o sha256-linux.o avbparse-linux.o

# Project version (single source of truth: top-level VERSION file).
GBL_TOOL_VERSION := $(shell tr -d '[:space:]' < $(dir $(lastword $(MAKEFILE_LIST)))../../VERSION)
ifeq ($(GBL_TOOL_VERSION),)
$(error VERSION file is empty or missing)
endif
CFLAGS  += -DGBL_TOOL_VERSION='"$(GBL_TOOL_VERSION)"'
XCFLAGS += -DGBL_TOOL_VERSION='"$(GBL_TOOL_VERSION)"'
NDK_CFLAGS += -DGBL_TOOL_VERSION='"$(GBL_TOOL_VERSION)"'

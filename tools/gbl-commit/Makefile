# tools/gbl-commit/Makefile
CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -Werror -O2 -std=c99 -D_FILE_OFFSET_BITS=64
PROJ    := $(realpath ../..)
GPL     := $(PROJ)/GblChainloadPkg/Library/GblPayloadLib

CFLAGS  += -DGBL_HOST_BUILD=1 -I$(GPL)

SRCS = gbl-commit.c $(GPL)/Sha256.c

all: gbl-commit

gbl-commit: gbl-commit.c $(GPL)/Sha256.c $(GPL)/Internal/Sha256.h
	$(CC) $(CFLAGS) -o $@ gbl-commit.c $(GPL)/Sha256.c

# --- Android cross-compile target (NDK r27) ---
NDK     ?= $(ANDROID_NDK)
NDK_CC   = $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android31-clang
NDK_CFLAGS = -static -O2 -Wall -Wextra -std=c99 -D_FILE_OFFSET_BITS=64 \
             -DGBL_HOST_BUILD=1 -I$(GPL)

android: gbl-commit-android

gbl-commit-android: gbl-commit.c $(GPL)/Sha256.c $(GPL)/Internal/Sha256.h
	$(NDK_CC) $(NDK_CFLAGS) -o $@ gbl-commit.c $(GPL)/Sha256.c

clean:
	rm -f gbl-commit gbl-commit-android gbl-commit.exe \
	      gbl-commit-macos-x64 gbl-commit-macos-arm64

# --- Windows / macOS cross-compile (zig cc) ---
XCFLAGS = -O2 -Wall -Wextra -Werror -std=c99 -D_FILE_OFFSET_BITS=64 \
          -DGBL_HOST_BUILD=1 -I$(GPL)
XSRCS   = gbl-commit.c $(GPL)/Sha256.c

windows:     gbl-commit.exe
macos-x64:   gbl-commit-macos-x64
macos-arm64: gbl-commit-macos-arm64

gbl-commit.exe: $(XSRCS)
	zig cc -target x86_64-windows-gnu $(XCFLAGS) -o $@ $(XSRCS)

gbl-commit-macos-x64: $(XSRCS)
	zig cc -target x86_64-macos $(XCFLAGS) -o $@ $(XSRCS)

gbl-commit-macos-arm64: $(XSRCS)
	zig cc -target aarch64-macos $(XCFLAGS) -o $@ $(XSRCS)

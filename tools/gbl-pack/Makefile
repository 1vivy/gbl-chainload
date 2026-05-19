# tools/gbl-pack/Makefile
CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -Werror -O2 -std=c99
PROJ    := $(realpath ../..)
GPL     := $(PROJ)/GblChainloadPkg/Library/GblPayloadLib

CFLAGS  += -DGBL_HOST_BUILD=1 -I$(GPL)

PARSER_SRCS_NODEP = \
  $(GPL)/Crc32.c \
  $(GPL)/PeSanity.c

SRCS_ALL = gbl-pack.c pack.c $(PARSER_SRCS_NODEP) $(GPL)/Sha256.c

all: gbl-pack

gbl-pack: gbl-pack.c pack.c pack.h ../shared/gblp1.h ../shared/efisp_scan.h \
          ../shared/gbl_mode2_profile.h \
          $(PARSER_SRCS_NODEP) $(GPL)/Sha256.c $(GPL)/Internal/Sha256.h
	$(CC) $(CFLAGS) -o $@ gbl-pack.c pack.c $(PARSER_SRCS_NODEP) $(GPL)/Sha256.c

# --- Android cross-compile target (NDK r27) ---
NDK     ?= $(ANDROID_NDK)
NDK_CC   = $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android31-clang
NDK_CFLAGS = -static -O2 -Wall -Wextra -std=c99 -DGBL_HOST_BUILD=1 -I$(GPL)

android: gbl-pack-android

gbl-pack-android: gbl-pack.c pack.c pack.h ../shared/gblp1.h ../shared/efisp_scan.h \
                  $(PARSER_SRCS_NODEP) $(GPL)/Sha256.c $(GPL)/Internal/Sha256.h
	$(NDK_CC) $(NDK_CFLAGS) -o $@ gbl-pack.c pack.c $(PARSER_SRCS_NODEP) $(GPL)/Sha256.c

clean:
	rm -f gbl-pack gbl-pack-android gbl-pack.exe \
	      gbl-pack-macos-x64 gbl-pack-macos-arm64 Sha256.o

# --- Windows / macOS cross-compile (zig cc) ---
XCFLAGS = -O2 -Wall -Wextra -Werror -std=c99 -DGBL_HOST_BUILD=1 -I$(GPL)
XSRCS   = gbl-pack.c pack.c $(PARSER_SRCS_NODEP) $(GPL)/Sha256.c

windows:     gbl-pack.exe
macos-x64:   gbl-pack-macos-x64
macos-arm64: gbl-pack-macos-arm64

gbl-pack.exe: $(XSRCS)
	zig cc -target x86_64-windows-gnu $(XCFLAGS) -o $@ $(XSRCS)

gbl-pack-macos-x64: $(XSRCS)
	zig cc -target x86_64-macos $(XCFLAGS) -o $@ $(XSRCS)

gbl-pack-macos-arm64: $(XSRCS)
	zig cc -target aarch64-macos $(XCFLAGS) -o $@ $(XSRCS)

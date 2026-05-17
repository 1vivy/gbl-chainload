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
	rm -f gbl-pack gbl-pack-android Sha256.o

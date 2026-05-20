# tools/gblp1-inspect/Makefile
CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -Werror -O2 -std=c99
PROJ    := $(realpath ../..)
GPL     := $(PROJ)/GblChainloadPkg/Library/GblPayloadLib

CFLAGS  += -DGBL_HOST_BUILD=1 -I$(GPL)

SRCS    := gblp1-inspect.c $(GPL)/Crc32.c $(GPL)/Sha256.c
HDRS    := ../shared/gblp1.h $(GPL)/Internal/Sha256.h $(GPL)/Internal/Crc32.h

all: gblp1-inspect

gblp1-inspect: $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRCS)

# --- Android cross-compile target (NDK r27) ---
NDK     ?= $(ANDROID_NDK)
NDK_CC   = $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android31-clang
NDK_CFLAGS = -static -O2 -Wall -Wextra -std=c99 -DGBL_HOST_BUILD=1 -I$(GPL)

android: gblp1-inspect-android

gblp1-inspect-android: $(SRCS) $(HDRS)
	$(NDK_CC) $(NDK_CFLAGS) -o $@ $(SRCS)

clean:
	rm -f gblp1-inspect gblp1-inspect-android

# Project version (single source of truth: top-level VERSION file).
GBL_TOOL_VERSION := $(shell tr -d '[:space:]' < $(dir $(lastword $(MAKEFILE_LIST)))../../VERSION)
ifeq ($(GBL_TOOL_VERSION),)
$(error VERSION file is empty or missing)
endif
CFLAGS  += -DGBL_TOOL_VERSION='"$(GBL_TOOL_VERSION)"'
XCFLAGS += -DGBL_TOOL_VERSION='"$(GBL_TOOL_VERSION)"'
NDK_CFLAGS += -DGBL_TOOL_VERSION='"$(GBL_TOOL_VERSION)"'

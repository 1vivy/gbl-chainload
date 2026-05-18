# tools/vbmeta-graft/Makefile
PROJ    := $(realpath ../..)
AVB     := $(PROJ)/GblChainloadPkg/Library/AvbParseLib
INCS    := -I$(AVB)/Internal -I$(PROJ)/GblChainloadPkg/Include/Library
CFLAGS  ?= -O1 -g -Wall -Wextra -std=c11
CFLAGS  += -D__HOST_BUILD__ $(INCS)

SRCS := vbmeta-graft.c $(AVB)/AvbParse.c

all: vbmeta-graft

vbmeta-graft: $(SRCS)
	$(CC) $(CFLAGS) $^ -o $@

# --- Android cross-compile target (NDK r27) ---
NDK     ?= $(ANDROID_NDK)
NDK_CC   = $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android31-clang
NDK_CFLAGS = -static -O2 -Wall -Wextra -std=c11 -D__HOST_BUILD__ $(INCS)

android: vbmeta-graft-android

vbmeta-graft-android: $(SRCS)
	$(NDK_CC) $(NDK_CFLAGS) $^ -o $@

clean:
	rm -f vbmeta-graft vbmeta-graft-android

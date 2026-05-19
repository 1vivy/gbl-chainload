# tools/mode2-profile/Makefile
CC      ?= gcc
PROJ    := $(realpath ../..)
GPL     := $(PROJ)/GblChainloadPkg/Library/GblPayloadLib
CFLAGS  ?= -Wall -Wextra -Werror -O2 -std=c11 -DGBL_HOST_BUILD=1 -I$(GPL)
SRCS     = mode2-profile.c vendor/tomlc99/toml.c $(GPL)/Sha256.c
HDRS     = vendor/tomlc99/toml.h ../shared/gbl_mode2_profile.h \
           $(GPL)/Internal/Sha256.h

all: mode2-profile

# Build in two steps so we can suppress warnings only for the vendored TU.
mode2-profile: $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -c -o mode2-profile.o mode2-profile.c
	$(CC) -w -O2 -std=c11 -DGBL_HOST_BUILD=1 -c -o toml.o vendor/tomlc99/toml.c
	$(CC) $(CFLAGS) -c -o sha256.o $(GPL)/Sha256.c
	$(CC) $(CFLAGS) -o $@ mode2-profile.o toml.o sha256.o

# --- Android cross-compile (NDK r27) ---
NDK     ?= $(ANDROID_NDK)
NDK_CC   = $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android31-clang
NDK_CFLAGS = -static -O2 -Wall -Wextra -Werror -std=c11 -DGBL_HOST_BUILD=1 -I$(GPL)

android: mode2-profile-android

mode2-profile-android: $(SRCS) $(HDRS)
	$(NDK_CC) $(NDK_CFLAGS) -c -o mode2-profile-android.o mode2-profile.c
	$(NDK_CC) -w -static -O2 -std=c11 -DGBL_HOST_BUILD=1 -c -o toml-android.o vendor/tomlc99/toml.c
	$(NDK_CC) $(NDK_CFLAGS) -c -o sha256-android.o $(GPL)/Sha256.c
	$(NDK_CC) $(NDK_CFLAGS) -o $@ mode2-profile-android.o toml-android.o sha256-android.o

clean:
	rm -f mode2-profile mode2-profile-android mode2-profile.o toml.o sha256.o \
	      mode2-profile-android.o toml-android.o sha256-android.o

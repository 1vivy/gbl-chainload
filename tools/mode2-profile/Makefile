# tools/mode2-profile/Makefile
CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -O2 -std=c11 -DGBL_HOST_BUILD=1
SRCS     = mode2-profile.c vendor/tomlc99/toml.c
HDRS     = vendor/tomlc99/toml.h ../shared/gbl_mode2_profile.h

all: mode2-profile

# Build in two steps so we can suppress warnings only for the vendored TU.
mode2-profile: $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -c -o mode2-profile.o mode2-profile.c
	$(CC) -w -O2 -std=c11 -c -o toml.o vendor/tomlc99/toml.c
	$(CC) $(CFLAGS) -o $@ mode2-profile.o toml.o

# --- Android cross-compile (NDK r27) ---
NDK     ?= $(ANDROID_NDK)
NDK_CC   = $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android31-clang
NDK_CFLAGS = -static -O2 -Wall -Wextra -std=c11 -DGBL_HOST_BUILD=1

android: mode2-profile-android

mode2-profile-android: $(SRCS) $(HDRS)
	$(NDK_CC) $(NDK_CFLAGS) -c -o mode2-profile-android.o mode2-profile.c
	$(NDK_CC) -w -static -O2 -std=c11 -c -o toml-android.o vendor/tomlc99/toml.c
	$(NDK_CC) $(NDK_CFLAGS) -o $@ mode2-profile-android.o toml-android.o

clean:
	rm -f mode2-profile mode2-profile-android mode2-profile.o toml.o \
	      mode2-profile-android.o toml-android.o

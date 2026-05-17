CC      ?= cc
CFLAGS  ?= -O1 -g -Wall -Wextra -std=c11
LDFLAGS ?= -llzma

fv-unwrap: fv-unwrap.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

clean:
	rm -f fv-unwrap fv-unwrap-android *.o

# --- Android cross-compile target (NDK r27) ---
# Links the static liblzma cross-compiled by docker/Dockerfile into
# $(LIBLZMA_ANDROID) — same -llzma path as the host build, no vendored
# LZMA source. The Dockerfile exports LIBLZMA_ANDROID; the default below
# is the in-image install prefix for out-of-container invocations.
NDK     ?= $(ANDROID_NDK)
NDK_CC   = $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android31-clang
NDK_CFLAGS = -static -O2 -Wall -Wextra -std=c11
LIBLZMA_ANDROID ?= /opt/liblzma-android

android: fv-unwrap-android

fv-unwrap-android: fv-unwrap.c $(LIBLZMA_ANDROID)/lib/liblzma.a
	$(NDK_CC) $(NDK_CFLAGS) -I$(LIBLZMA_ANDROID)/include -o $@ \
	    fv-unwrap.c $(LIBLZMA_ANDROID)/lib/liblzma.a

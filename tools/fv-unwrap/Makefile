CC      ?= cc
CFLAGS  ?= -O1 -g -Wall -Wextra -std=c11
LDFLAGS ?= -llzma

fv-unwrap: fv-unwrap.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

clean:
	rm -f fv-unwrap fv-unwrap-android *.o

# --- Android cross-compile target (NDK r27) ---
# Uses vendored Igor Pavlov LZMA SDK (public domain) instead of liblzma,
# which is not available in the NDK sysroot.
NDK     ?= $(ANDROID_NDK)
NDK_CC   = $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android31-clang
NDK_CFLAGS = -static -O2 -Wall -Wextra -std=c11 -DFVUNWRAP_VENDORED_LZMA \
             -Wno-unused-parameter

LZMASDK = lzmasdk

android: fv-unwrap-android

fv-unwrap-android: fv-unwrap.c $(LZMASDK)/LzmaDec.c $(LZMASDK)/lzma_compat.h \
                   $(LZMASDK)/LzmaDec.h $(LZMASDK)/7zTypes.h
	$(NDK_CC) $(NDK_CFLAGS) -I$(LZMASDK) -o $@ fv-unwrap.c $(LZMASDK)/LzmaDec.c

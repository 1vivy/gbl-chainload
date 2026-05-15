# tests/host/helpers/Makefile
CC ?= gcc
CFLAGS ?= -Wall -Wextra -Werror -O2 -std=c99

BINS = test_efisp_scan
BINS += test_crc32
BINS += test_sha256
BINS += test_pe_sanity

all: $(BINS)

test_efisp_scan: test_efisp_scan.c ../../../tools/shared/efisp_scan.h
	$(CC) $(CFLAGS) -o $@ $<

test_crc32: test_crc32.c ../../../GblChainloadPkg/Library/GblPayloadLib/Crc32.c \
            ../../../GblChainloadPkg/Library/GblPayloadLib/Internal/Crc32.h
	$(CC) $(CFLAGS) -DGBL_HOST_BUILD=1 -o $@ test_crc32.c \
	      ../../../GblChainloadPkg/Library/GblPayloadLib/Crc32.c

test_sha256: test_sha256.c ../../../GblChainloadPkg/Library/GblPayloadLib/Sha256.c \
             ../../../GblChainloadPkg/Library/GblPayloadLib/Internal/Sha256.h
	$(CC) $(CFLAGS) -Wno-deprecated-declarations -DGBL_HOST_BUILD=1 -o $@ test_sha256.c \
	      ../../../GblChainloadPkg/Library/GblPayloadLib/Sha256.c -lcrypto

test_pe_sanity: test_pe_sanity.c ../../../GblChainloadPkg/Library/GblPayloadLib/PeSanity.c \
                ../../../GblChainloadPkg/Library/GblPayloadLib/Internal/PeSanity.h
	$(CC) $(CFLAGS) -DGBL_HOST_BUILD=1 -o $@ test_pe_sanity.c \
	      ../../../GblChainloadPkg/Library/GblPayloadLib/PeSanity.c

clean:
	rm -f $(BINS)

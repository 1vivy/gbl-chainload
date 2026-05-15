# tests/host/helpers/Makefile
CC ?= gcc
CFLAGS ?= -Wall -Wextra -Werror -O2 -std=c99

BINS = test_efisp_scan
BINS += test_crc32

all: $(BINS)

test_efisp_scan: test_efisp_scan.c ../../../tools/shared/efisp_scan.h
	$(CC) $(CFLAGS) -o $@ $<

test_crc32: test_crc32.c ../../../GblChainloadPkg/Library/GblPayloadLib/Crc32.c \
            ../../../GblChainloadPkg/Library/GblPayloadLib/Internal/Crc32.h
	$(CC) $(CFLAGS) -DGBL_HOST_BUILD=1 -o $@ test_crc32.c \
	      ../../../GblChainloadPkg/Library/GblPayloadLib/Crc32.c

clean:
	rm -f $(BINS)

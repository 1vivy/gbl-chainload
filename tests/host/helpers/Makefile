# tests/host/helpers/Makefile
#
# PR2 Task 3: test_pe_sanity + test_efisp_scan link the Rust pe-utils
# staticlib (target/release/libpe_utils.a) instead of the deleted C
# sources (PeSanity.c, efisp_scan.h).
CC ?= gcc
CFLAGS ?= -Wall -Wextra -Werror -O2 -std=c99
PROJ := $(realpath ../../..)
PE_UTILS_LIB := $(PROJ)/target/release/libpe_utils.a

BINS = test_efisp_scan
BINS += test_crc32
BINS += test_sha256
BINS += test_pe_sanity
BINS += test_manifest_parse

# Default target up here so we don't accidentally trigger the cargo
# staticlib rule when running bare `make`.
all: $(BINS)

.PHONY: all $(PE_UTILS_LIB)

$(PE_UTILS_LIB):
	cd $(PROJ) && cargo build --release -p pe-utils

test_efisp_scan: test_efisp_scan.c $(PE_UTILS_LIB)
	$(CC) $(CFLAGS) -o $@ test_efisp_scan.c $(PE_UTILS_LIB)

test_crc32: test_crc32.c ../../../GblChainloadPkg/Library/GblPayloadLib/Crc32.c \
            ../../../GblChainloadPkg/Library/GblPayloadLib/Internal/Crc32.h
	$(CC) $(CFLAGS) -DGBL_HOST_BUILD=1 -o $@ test_crc32.c \
	      ../../../GblChainloadPkg/Library/GblPayloadLib/Crc32.c

test_sha256: test_sha256.c ../../../GblChainloadPkg/Library/GblPayloadLib/Sha256.c \
             ../../../GblChainloadPkg/Library/GblPayloadLib/Internal/Sha256.h
	$(CC) $(CFLAGS) -DGBL_HOST_BUILD=1 -o $@ test_sha256.c \
	      ../../../GblChainloadPkg/Library/GblPayloadLib/Sha256.c

test_pe_sanity: test_pe_sanity.c $(PE_UTILS_LIB)
	$(CC) $(CFLAGS) -o $@ test_pe_sanity.c $(PE_UTILS_LIB)

test_manifest_parse: test_manifest_parse.c \
                     ../../../GblChainloadPkg/Library/GblPayloadLib/PayloadParse.c \
                     ../../../GblChainloadPkg/Library/GblPayloadLib/Crc32.c \
                     ../../../GblChainloadPkg/Library/GblPayloadLib/Sha256.c \
                     ../../../GblChainloadPkg/Library/GblPayloadLib/Internal/PayloadParse.h \
                     ../../../tools/shared/gblp1.h
	$(CC) $(CFLAGS) -DGBL_HOST_BUILD=1 -o $@ test_manifest_parse.c \
	      ../../../GblChainloadPkg/Library/GblPayloadLib/PayloadParse.c \
	      ../../../GblChainloadPkg/Library/GblPayloadLib/Crc32.c \
	      ../../../GblChainloadPkg/Library/GblPayloadLib/Sha256.c

BINS += parser_harness

PARSER_SRCS = \
  ../../../GblChainloadPkg/Library/GblPayloadLib/PayloadParse.c \
  ../../../GblChainloadPkg/Library/GblPayloadLib/Crc32.c \
  ../../../GblChainloadPkg/Library/GblPayloadLib/Sha256.c

parser_harness: parser_harness.c $(PARSER_SRCS)
	$(CC) $(CFLAGS) -DGBL_HOST_BUILD=1 \
	      -I../../../tools/shared \
	      -o $@ parser_harness.c $(PARSER_SRCS)

BINS += poison_byte

poison_byte: poison_byte.c
	$(CC) $(CFLAGS) -o $@ $<

BINS += inject_efisp

inject_efisp: inject_efisp.c
	$(CC) $(CFLAGS) -o $@ $<

BINS += locate_overlay_host

locate_overlay_host: locate_overlay_host.c
	$(CC) $(CFLAGS) -o $@ $<

MODE2_SRC = ../../../GblChainloadPkg/Library/GblPayloadLib/Mode2Profile.c
MODE2_REWRITE_SRC = ../../../GblChainloadPkg/Library/ProtocolHookLib/ProfileRewrite.c

BINS += mode2_harness

mode2_harness: mode2_harness.c $(MODE2_SRC) $(MODE2_REWRITE_SRC)
	$(CC) $(CFLAGS) -DGBL_HOST_BUILD=1 \
	  -I../../../GblChainloadPkg/Library/GblPayloadLib \
	  -I../../../GblChainloadPkg/Library/ProtocolHookLib \
	  -o $@ mode2_harness.c $(MODE2_SRC) $(MODE2_REWRITE_SRC)

clean:
	rm -f $(BINS)

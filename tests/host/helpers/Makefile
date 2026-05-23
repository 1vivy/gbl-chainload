# tests/host/helpers/Makefile
#
# PR2 Task 3: test_pe_sanity + test_efisp_scan link libpe_utils.a
# (crates/pe-utils, Rust).
# PR2 Task 4: test_crc32 + test_sha256 + test_manifest_parse +
# parser_harness link libgblp1.a (crates/gblp1, Rust), replacing the
# deleted Crc32.c / Sha256.c / PayloadParse.c.
CC ?= gcc
CFLAGS ?= -Wall -Wextra -Werror -O2 -std=c99
PROJ := $(realpath ../../..)
PE_UTILS_LIB := $(PROJ)/target/release/libpe_utils.a
GBLP1_LIB    := $(PROJ)/target/release/libgblp1.a

BINS = test_efisp_scan
BINS += test_crc32
BINS += test_sha256
BINS += test_pe_sanity
BINS += test_manifest_parse

# Default target up here so we don't accidentally trigger the cargo
# staticlib rule when running bare `make`.
all: $(BINS)

.PHONY: all $(PE_UTILS_LIB) $(GBLP1_LIB)

$(PE_UTILS_LIB):
	cd $(PROJ) && cargo build --release -p pe-utils

$(GBLP1_LIB):
	cd $(PROJ) && cargo build --release -p gblp1

test_efisp_scan: test_efisp_scan.c $(PE_UTILS_LIB)
	$(CC) $(CFLAGS) -o $@ test_efisp_scan.c $(PE_UTILS_LIB)

test_crc32: test_crc32.c $(GBLP1_LIB)
	$(CC) $(CFLAGS) -DGBL_HOST_BUILD=1 -o $@ test_crc32.c $(GBLP1_LIB)

test_sha256: test_sha256.c $(GBLP1_LIB)
	$(CC) $(CFLAGS) -DGBL_HOST_BUILD=1 -o $@ test_sha256.c $(GBLP1_LIB)

test_pe_sanity: test_pe_sanity.c $(PE_UTILS_LIB)
	$(CC) $(CFLAGS) -o $@ test_pe_sanity.c $(PE_UTILS_LIB)

test_manifest_parse: test_manifest_parse.c \
                     ../../../crates/gblp1/include/gblp1_ffi.h \
                     ../../../tools/shared/gblp1.h \
                     $(GBLP1_LIB)
	$(CC) $(CFLAGS) -DGBL_HOST_BUILD=1 -o $@ test_manifest_parse.c $(GBLP1_LIB)

BINS += parser_harness

parser_harness: parser_harness.c $(GBLP1_LIB)
	$(CC) $(CFLAGS) -DGBL_HOST_BUILD=1 \
	      -I../../../tools/shared \
	      -o $@ parser_harness.c $(GBLP1_LIB)

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

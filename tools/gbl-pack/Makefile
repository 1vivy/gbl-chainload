# tools/gbl-pack/Makefile
CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -Werror -O2 -std=c99
PROJ    := $(realpath ../..)
GPL     := $(PROJ)/GblChainloadPkg/Library/GblPayloadLib

CFLAGS  += -DGBL_HOST_BUILD=1 -I$(GPL)

# Sha256.c uses the OpenSSL 3.x-deprecated SHA256_CTX API; suppress that
# warning only for that translation unit so our own code stays under -Werror.
SHA256_CFLAGS = $(CFLAGS) -Wno-deprecated-declarations

PARSER_SRCS_NODEP = \
  $(GPL)/Crc32.c \
  $(GPL)/PeSanity.c

all: gbl-pack

Sha256.o: $(GPL)/Sha256.c $(GPL)/Internal/Sha256.h
	$(CC) $(SHA256_CFLAGS) -c -o $@ $<

gbl-pack: gbl-pack.c pack.c pack.h ../shared/gblp1.h ../shared/efisp_scan.h \
          $(PARSER_SRCS_NODEP) Sha256.o
	$(CC) $(CFLAGS) -o $@ gbl-pack.c pack.c $(PARSER_SRCS_NODEP) Sha256.o -lcrypto

clean:
	rm -f gbl-pack Sha256.o

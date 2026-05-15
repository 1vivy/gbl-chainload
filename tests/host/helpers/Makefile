# tests/host/helpers/Makefile
CC ?= gcc
CFLAGS ?= -Wall -Wextra -Werror -O2 -std=c99

BINS = test_efisp_scan

all: $(BINS)

test_efisp_scan: test_efisp_scan.c ../../../tools/shared/efisp_scan.h
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(BINS)

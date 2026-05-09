CC      ?= cc
CFLAGS  ?= -O1 -g -Wall -Wextra -Wno-unused-parameter -std=c11 -D__HOST_BUILD__
PROJ    := $(realpath ../..)

TESTS   := test_scanfor test_scan_bounded test_encode

all: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done

test_scanfor: test_scanfor.c \
              $(PROJ)/GblChainloadPkg/Library/DynamicPatchLib/Internal/ScanLib.c \
              $(PROJ)/GblChainloadPkg/Library/DynamicPatchLib/Internal/PeSections.c
	$(CC) $(CFLAGS) -I$(PROJ)/GblChainloadPkg/Library/DynamicPatchLib/Internal $^ -o $@

test_scan_bounded: test_scan_bounded.c \
                   $(PROJ)/GblChainloadPkg/Library/DynamicPatchLib/Internal/ScanLib.c \
                   $(PROJ)/GblChainloadPkg/Library/DynamicPatchLib/Internal/PeSections.c
	$(CC) $(CFLAGS) -I$(PROJ)/GblChainloadPkg/Library/DynamicPatchLib/Internal $^ -o $@

test_encode: test_encode.c \
             $(PROJ)/GblChainloadPkg/Library/DynamicPatchLib/Internal/Encode.c
	$(CC) $(CFLAGS) -D__HOST_BUILD__ \
	    -I$(PROJ)/GblChainloadPkg/Library/DynamicPatchLib/Internal $^ -o $@

clean:
	rm -f $(TESTS) *.o

CC      ?= cc
CFLAGS  ?= -O1 -g -Wall -Wextra -Wno-unused-parameter -std=c11 -D__HOST_BUILD__
PROJ    := $(realpath ../..)
DPL     := $(PROJ)/GblChainloadPkg/Library/DynamicPatchLib

INCLUDES := -I$(PROJ)/GblChainloadPkg/Include/Library \
            -I$(DPL)/Internal

TESTS := test_patch1 test_patch7 test_patch9

all: $(TESTS)
	@for t in $(TESTS); do echo "== $$t =="; ./$$t || exit 1; done

test_patch1: test_patch1.c \
             $(DPL)/Internal/ScanLib.c \
             $(DPL)/Internal/PeSections.c \
             $(DPL)/universal/universal.c
	$(CC) $(CFLAGS) $(INCLUDES) $^ -o $@

test_patch7: test_patch7.c \
             $(DPL)/Internal/ScanLib.c \
             $(DPL)/Internal/PeSections.c \
             $(DPL)/Internal/Encode.c \
             $(DPL)/oem/oneplus_canoe.c
	$(CC) $(CFLAGS) $(INCLUDES) $^ -o $@

test_patch9: test_patch9.c \
             $(DPL)/Internal/ScanLib.c \
             $(DPL)/Internal/PeSections.c \
             $(DPL)/Internal/Encode.c \
             $(DPL)/mode_1/mode_1.c
	$(CC) $(CFLAGS) $(INCLUDES) $^ -o $@

clean:
	rm -f $(TESTS) *.o

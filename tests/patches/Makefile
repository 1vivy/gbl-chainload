CC      ?= cc
CFLAGS  ?= -O1 -g -Wall -Wextra -Wno-unused-parameter -std=c11 -D__HOST_BUILD__
PROJ    := $(realpath ../..)
DPL     := $(PROJ)/GblChainloadPkg/Library/DynamicPatchLib

INCLUDES := -I$(PROJ)/GblChainloadPkg/Include/Library \
            -I$(DPL)/Internal

# Where the tests look for ABL fixtures. Untracked: device firmware blobs
# never land in git (see top-level .gitignore). Override at make time:
#   make TEST_FIXTURES_DIR=/path/to/blobs
TEST_FIXTURES_DIR ?= $(PROJ)/tests/images
CFLAGS  += -DTEST_FIXTURES_DIR='"$(TEST_FIXTURES_DIR)"'

TESTS := test_patch1 test_patch7 test_patch10 test_patch6

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

test_patch10: test_patch10.c \
              $(DPL)/Internal/ScanLib.c \
              $(DPL)/Internal/PeSections.c \
              $(DPL)/Internal/Encode.c \
              $(DPL)/Internal/Arm64Decode.c \
              $(DPL)/mode_1/mode_1.c
	$(CC) $(CFLAGS) $(INCLUDES) $^ -o $@

test_patch6: test_patch6.c \
             $(DPL)/Internal/ScanLib.c \
             $(DPL)/Internal/PeSections.c \
             $(DPL)/Internal/Encode.c \
             $(DPL)/Internal/Arm64Decode.c \
             $(DPL)/mode_1/mode_1.c
	$(CC) $(CFLAGS) $(INCLUDES) $^ -o $@

clean:
	rm -f $(TESTS) *.o
